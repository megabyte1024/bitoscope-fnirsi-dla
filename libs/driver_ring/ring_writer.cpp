/*
 * SPDX-License-Identifier: MIT
 */

#include "ring_writer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace bitoscope::plugin_ring {

namespace {

using bulk_ring::AcquireError;
using bulk_ring::AcquireErrorKind;
using bulk_ring::BulkRing;
using bulk_ring::ChunkHeader;
using bulk_ring::FailureKind;
using bulk_ring::ProducerState;

constexpr uint64_t kWaitSliceMs = 20;

std::string hexId(uint64_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out += kDigits[(v >> shift) & 0xFu];
  }
  return out;
}

std::string inQuotes(std::string_view name) { return "\"" + std::string(name) + "\""; }

std::string encodingName(uint32_t encoding) {
  if (encoding == kRingEncodingChannelMajor) {
    return "channel-major";
  }
  if (encoding == kRingEncodingSampleMajor) {
    return "sample-major";
  }
  return "encoding " + std::to_string(encoding);
}

RingWriteError writeError(RingWriteErrorKind kind, std::string message) {
  return RingWriteError{kind, std::move(message)};
}

std::expected<uint64_t, std::string> capturePayloadBytes(uint32_t encoding, uint32_t num_channels,
                                                         uint64_t num_samples) {
  if (auto ok = checkCaptureGeometry(encoding, num_channels); !ok) {
    return std::unexpected(ok.error());
  }
  return chunkPayloadBytes(encoding, num_channels, num_samples);
}

}

std::expected<uint64_t, std::string> samplesPerSlot(uint32_t encoding, uint32_t num_channels,
                                                    uint64_t slot_payload_bytes) {
  if (auto ok = checkCaptureGeometry(encoding, num_channels); !ok) {
    return std::unexpected(ok.error());
  }
  if (encoding == kRingEncodingChannelMajor) {
    const uint64_t rounds = slot_payload_bytes / num_channels;
    if (rounds > std::numeric_limits<uint64_t>::max() / 8) {
      return std::numeric_limits<uint64_t>::max() / 8 * 8;
    }
    return rounds * 8;
  }
  return slot_payload_bytes / ((uint64_t{num_channels} + 7) / 8);
}

class RingWriter {
 public:
  static std::expected<std::unique_ptr<RingWriter>, std::string> open(std::string_view name, uint64_t capture_id,
                                                                     bulk_ring::Timing timing) {
    auto ring = BulkRing::open(name, std::move(timing));
    if (!ring) {
      return std::unexpected("cannot write this capture into ring " + inQuotes(name) + ": " + ring.error());
    }
    if (ring->captureId() != capture_id) {
      return std::unexpected("ring " + inQuotes(name) + " was created for capture " + hexId(ring->captureId()) +
                             ", but this START is for capture " + hexId(capture_id));
    }
    return std::unique_ptr<RingWriter>(new RingWriter(std::move(*ring)));
  }

  uint64_t captureId() const { return ring_.captureId(); }
  uint64_t slotPayloadBytes() const { return ring_.slotPayloadBytes(); }
  uint32_t flushAfterMs() const { return ring_.flushAfterMs(); }

  void flushIfDue() {
    if (!stopped_) {
      (void)ring_.flushIfDue();
    }
  }
  RingFill fill() const { return RingFill{high_water_, ring_.slotCount()}; }

  void cancel() { cancelled_.store(true, std::memory_order_release); }

  std::expected<void, RingWriteError> write(uint64_t capture_id, const RingChunk& chunk,
                                            std::span<const uint8_t> payload) {
    if (auto ok = admit(capture_id); !ok) {
      return ok;
    }
    if (begun_bytes_.has_value()) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "chunk was written while one begun with beginChunk() was not committed",
                  "driver mixed written chunk into begun one");
    }
    const auto bytes = capturePayloadBytes(chunk.encoding, chunk.num_channels, chunk.num_samples);
    if (!bytes) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "chunk at sample " + std::to_string(chunk.first_sample_index) + " cannot be written: " +
                      bytes.error(),
                  "driver's chunk has no valid layout");
    }
    if (*bytes != payload.size()) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "chunk at sample " + std::to_string(chunk.first_sample_index) + " carries " +
                      std::to_string(payload.size()) + " payload bytes, but " +
                      std::to_string(chunk.num_samples) + " samples of " + std::to_string(chunk.num_channels) +
                      " channels " + encodingName(chunk.encoding) + " are " + std::to_string(*bytes),
                  "driver's chunk size does not match its samples");
    }
    if (chunk.num_samples == 0) {
      return {};
    }
    const uint64_t per_slot = samplesPerSlot(chunk.encoding, chunk.num_channels, ring_.slotPayloadBytes()).value();
    if (per_slot == 0) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "ring " + inQuotes(ring_.name()) + "'s slots of " + std::to_string(ring_.slotPayloadBytes()) +
                      " bytes cannot hold one " + encodingName(chunk.encoding) + " unit of " +
                      std::to_string(chunk.num_channels) + " channels",
                  "ring's slots are too small for this many channels");
    }

    uint64_t done = 0;
    uint64_t offset = 0;
    while (done < chunk.num_samples) {
      const uint64_t n = (std::min)(per_slot, chunk.num_samples - done);
      const uint64_t piece_bytes = capturePayloadBytes(chunk.encoding, chunk.num_channels, n).value();
      const uint64_t first = chunk.first_sample_index + done;
      auto slot = ring_.tryAcquire(piece_bytes);
      if (!slot) {
        return acquireFailed(slot.error(), first, chunk.num_samples - done);
      }
      noteFill();
      std::memcpy(slot->data(), payload.data() + offset, static_cast<size_t>(piece_bytes));
      if (auto committed = ring_.commit(ChunkHeader{first, n, chunk.num_channels, chunk.encoding,
                                                    chunk.sample_rate_milli_hz, captureId(), piece_bytes});
          !committed) {
        return fail(RingWriteErrorKind::BadChunk, first, chunk.num_samples - done, committed.error(),
                    "ring refused commit");
      }
      done += n;
      offset += piece_bytes;
      next_sample_ = chunk.first_sample_index + done;
    }
    return {};
  }

  std::expected<std::span<uint8_t>, RingWriteError> beginChunk(uint64_t capture_id, uint64_t max_bytes,
                                                               bool blocking) {
    if (auto ok = admit(capture_id); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    if (begun_bytes_.has_value()) {
      return failSpan(RingWriteErrorKind::BadChunk, "beginChunk() was called again before commitChunk()",
                      "driver began two chunks at once");
    }
    if (max_bytes == 0 || max_bytes > ring_.slotPayloadBytes()) {
      return failSpan(RingWriteErrorKind::BadChunk,
                      "beginChunk() asked for " + std::to_string(max_bytes) + " bytes; ring " +
                          inQuotes(ring_.name()) + "'s slots hold 1 to " +
                          std::to_string(ring_.slotPayloadBytes()),
                      "driver asked for chunk no slot can hold");
    }
    auto slot = ring_.tryAcquire(max_bytes);
    while (blocking && !slot && slot.error().kind == AcquireErrorKind::Full) {
      if (cancelled_.load(std::memory_order_acquire)) {
        stopped_ = true;
        return std::unexpected(writeError(RingWriteErrorKind::Stopped,
                                          "STOP arrived while waiting for free slot in ring " +
                                              inQuotes(ring_.name()) + "; nothing more is delivered"));
      }
      slot = ring_.acquire(max_bytes, ring_.nowMs() + kWaitSliceMs);
      if (!slot && slot.error().kind == AcquireErrorKind::Timeout) {
        slot = std::unexpected(AcquireError{AcquireErrorKind::Full, slot.error().message});
      }
    }
    if (!slot) {
      return acquireFailed(slot.error(), next_sample_, 0);
    }
    noteFill();
    begun_bytes_ = max_bytes;
    return slot->first(static_cast<size_t>(max_bytes));
  }

  std::expected<void, RingWriteError> commitChunk(const RingChunk& chunk, uint64_t payload_bytes) {
    if (stopped_) {
      begun_bytes_.reset();
      return std::unexpected(stoppedError());
    }
    if (!begun_bytes_.has_value()) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "commitChunk() without beginChunk()", "driver committed chunk it never began");
    }
    const auto bytes = capturePayloadBytes(chunk.encoding, chunk.num_channels, chunk.num_samples);
    if (!bytes) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "chunk at sample " + std::to_string(chunk.first_sample_index) + " cannot be committed: " +
                      bytes.error(),
                  "driver's chunk has no valid layout");
    }
    if (*bytes != payload_bytes || payload_bytes > *begun_bytes_) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples,
                  "commitChunk() of " + std::to_string(payload_bytes) + " bytes at sample " +
                      std::to_string(chunk.first_sample_index) + ": " + std::to_string(chunk.num_samples) +
                      " samples of " + std::to_string(chunk.num_channels) + " channels " +
                      encodingName(chunk.encoding) + " are " + std::to_string(*bytes) + " bytes, and " +
                      std::to_string(*begun_bytes_) + " were begun",
                  "driver's chunk size does not match its samples");
    }
    begun_bytes_.reset();
    if (chunk.num_samples == 0) {
      return {};
    }
    if (auto committed = ring_.commit(ChunkHeader{chunk.first_sample_index, chunk.num_samples, chunk.num_channels,
                                                  chunk.encoding, chunk.sample_rate_milli_hz, captureId(),
                                                  payload_bytes});
        !committed) {
      return fail(RingWriteErrorKind::BadChunk, chunk.first_sample_index, chunk.num_samples, committed.error(),
                  "ring refused commit");
    }
    next_sample_ = chunk.first_sample_index + chunk.num_samples;
    return {};
  }

  std::expected<void, RingWriteError> failCapture(uint64_t capture_id, uint64_t at_sample, uint64_t lost_samples,
                                                  std::string_view reason) {
    if (auto ok = admit(capture_id); !ok) {
      return ok;
    }
    stopped_ = true;
    begun_bytes_.reset();
    (void)ring_.markFailed(at_sample, lost_samples, reason);
    return {};
  }

  void finish() {
    stopped_ = true;
    begun_bytes_.reset();
    if (ring_.producerState() == ProducerState::Attached) {
      (void)ring_.end();
    }
  }

 private:
  explicit RingWriter(BulkRing ring) : ring_(std::move(ring)) {}

  void noteFill() {
    const uint64_t in_use = ring_.produced() - ring_.consumed() + 1;
    high_water_ = (std::max)(high_water_, in_use);
  }

  std::expected<void, RingWriteError> admit(uint64_t capture_id) const {
    if (capture_id != captureId()) {
      return std::unexpected(writeError(RingWriteErrorKind::WrongCapture,
                                        "chunk of capture " + hexId(capture_id) + " is not written into ring " +
                                            inQuotes(ring_.name()) + ", which is capture " +
                                            hexId(captureId()) + "'s"));
    }
    if (stopped_) {
      return std::unexpected(stoppedError());
    }
    return {};
  }

  RingWriteError stoppedError() const {
    return writeError(RingWriteErrorKind::Stopped, "delivery into ring " + inQuotes(ring_.name()) +
                                                       " has ended for this capture; nothing more is delivered");
  }

  std::unexpected<RingWriteError> fail(RingWriteErrorKind kind, uint64_t at, uint64_t lost, std::string message,
                                       std::string_view reason,
                                       FailureKind failure = FailureKind::Unspecified) {
    stopped_ = true;
    begun_bytes_.reset();
    (void)ring_.markFailed(at, lost, "ring " + ring_.name() + ": " + std::string(reason), failure);
    return std::unexpected(writeError(kind, std::move(message) + "; nothing more is delivered for this capture"));
  }

  static std::string undelivered(uint64_t at, uint64_t lost) {
    if (lost == 0) {
      return "no chunk was in hand; nothing from sample " + std::to_string(at) + " on is delivered";
    }
    return "samples " + std::to_string(at) + " to " + std::to_string(at + lost - 1) + " (" + std::to_string(lost) +
           ") were not delivered";
  }

  std::unexpected<RingWriteError> failSpan(RingWriteErrorKind kind, std::string message, std::string_view reason) {
    return fail(kind, next_sample_, 0, std::move(message), reason);
  }

  std::unexpected<RingWriteError> acquireFailed(const AcquireError& error, uint64_t at, uint64_t lost) {
    switch (error.kind) {
      case AcquireErrorKind::Full:
        return fail(RingWriteErrorKind::Overrun, at, lost,
                    "overrun: " + error.message + " - host fell behind; " + undelivered(at, lost),
                    "every slot is full, host fell behind", FailureKind::RingFull);
      case AcquireErrorKind::ConsumerStale:
        return fail(RingWriteErrorKind::ConsumerGone, at, lost, error.message + "; " + undelivered(at, lost),
                    "host stopped reading (its heartbeat went stale)", FailureKind::ConsumerStale);
      case AcquireErrorKind::ConsumerInvalid:
        return fail(RingWriteErrorKind::ConsumerGone, at, lost, error.message + "; " + undelivered(at, lost),
                    "host's read count is not one reader can have");
      case AcquireErrorKind::ConsumerClosed:
        stopped_ = true;
        return std::unexpected(writeError(RingWriteErrorKind::ConsumerGone,
                                          error.message + "; nothing more is delivered for this capture"));
      case AcquireErrorKind::TooLarge:
        return fail(RingWriteErrorKind::BadChunk, at, lost, error.message, "chunk larger than slot");
      case AcquireErrorKind::ProducerFinished:
      case AcquireErrorKind::Timeout:
        break;
    }
    stopped_ = true;
    return std::unexpected(writeError(RingWriteErrorKind::Stopped, error.message));
  }

  BulkRing ring_;
  bool stopped_ = false;
  uint64_t next_sample_ = 0;
  std::optional<uint64_t> begun_bytes_;
  std::atomic<bool> cancelled_{false};
  uint64_t high_water_ = 0;
};

CaptureRing::CaptureRing() = default;

CaptureRing::~CaptureRing() { stopFlushTimer(); }

void CaptureRing::startFlushTimer(uint32_t flush_after_ms) {
  if (flush_after_ms == 0 || flush_timer_.joinable()) {
    return;
  }
  {
    std::lock_guard lock(flush_timer_mutex_);
    flush_timer_stop_ = false;
  }
  try {
    flush_timer_ = std::thread([this, flush_after_ms] { runFlushTimer(flush_after_ms); });
  } catch (const std::system_error&) {
    // WHY: Without a flush thread a quiet driver's last chunks wait for its next chunk, STOP or a failure: late, never lost.
  }
}

void CaptureRing::stopFlushTimer() {
  if (!flush_timer_.joinable()) {
    return;
  }
  {
    std::lock_guard lock(flush_timer_mutex_);
    flush_timer_stop_ = true;
  }
  flush_timer_cv_.notify_all();
  flush_timer_.join();
}

void CaptureRing::runFlushTimer(uint32_t flush_after_ms) {
  const auto period = std::chrono::milliseconds(flush_after_ms);
  std::unique_lock lock(flush_timer_mutex_);
  while (!flush_timer_stop_) {
    flush_timer_cv_.wait_for(lock, period, [this] { return flush_timer_stop_; });
    if (flush_timer_stop_) {
      break;
    }
    lock.unlock();
    {
      // WHY: Use try_lock: a driver holding the ring is writing and commits slots itself, so it must never wait for this timer.
      std::unique_lock ring_lock(mutex_, std::try_to_lock);
      if (ring_lock.owns_lock() && writer_ != nullptr) {
        writer_->flushIfDue();
      }
    }
    lock.lock();
  }
}

std::expected<void, std::string> CaptureRing::beginCapture(std::string_view ring_name, uint64_t capture_id,
                                                           bulk_ring::Timing timing) {
  if (writer_ != nullptr) {
    writer_->cancel();
  }
  {
    std::lock_guard lock(mutex_);
    if (writer_ != nullptr) {
      writer_->finish();
      retired_.push_back(std::move(writer_));
    }
  }
  if (ring_name.empty()) {
    return std::unexpected(std::string(
        "this START names no bulk ring (LaAcquisitionCommand.bulk_ring_name is empty); capture's "
        "samples are delivered only into shared-memory ring host creates for its START, so "
        "nothing could be delivered"));
  }
  auto opened = RingWriter::open(ring_name, capture_id, std::move(timing));
  if (!opened) {
    return std::unexpected(opened.error());
  }
  const uint32_t flush_after_ms = (*opened)->flushAfterMs();
  {
    std::lock_guard lock(mutex_);
    writer_ = std::move(*opened);
  }
  startFlushTimer(flush_after_ms);
  return {};
}

void CaptureRing::captureStarted(bool started) {
  if (started) {
    retired_.clear();
    return;
  }
  stopFlushTimer();
  std::unique_ptr<RingWriter> refused;
  {
    std::lock_guard lock(mutex_);
    refused = std::move(writer_);
    begun_on_ = nullptr;
  }
}

void CaptureRing::cancelWaits() {
  if (writer_ != nullptr) {
    writer_->cancel();
  }
}

void CaptureRing::endCapture() {
  stopFlushTimer();
  std::unique_ptr<RingWriter> ended;
  {
    std::lock_guard lock(mutex_);
    if (writer_ != nullptr) {
      writer_->finish();
      ended = std::move(writer_);
    }
    begun_on_ = nullptr;
  }
  retired_.clear();
}

namespace {

RingWriteError noRingError() {
  return writeError(RingWriteErrorKind::NoRing,
                    "no ring is open: no START yet, capture was stopped, or its START was refused");
}

}

std::expected<void, RingWriteError> CaptureRing::write(uint64_t capture_id, const RingChunk& chunk,
                                                       std::span<const uint8_t> payload) {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::unexpected(noRingError());
  }
  return writer_->write(capture_id, chunk, payload);
}

std::expected<std::span<uint8_t>, RingWriteError> CaptureRing::beginChunk(uint64_t capture_id, uint64_t max_bytes,
                                                                          bool blocking) {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::unexpected(noRingError());
  }
  auto slot = writer_->beginChunk(capture_id, max_bytes, blocking);
  if (slot) {
    begun_on_ = writer_.get();
  }
  return slot;
}

std::expected<void, RingWriteError> CaptureRing::commitChunk(const RingChunk& chunk, uint64_t payload_bytes) {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::unexpected(noRingError());
  }
  if (begun_on_ != nullptr && begun_on_ != writer_.get()) {
    begun_on_ = nullptr;
    return std::unexpected(writeError(RingWriteErrorKind::WrongCapture,
                                      "this chunk was begun in earlier capture's ring, which later START ended"));
  }
  begun_on_ = nullptr;
  return writer_->commitChunk(chunk, payload_bytes);
}

std::expected<void, RingWriteError> CaptureRing::failCapture(uint64_t capture_id, uint64_t at_sample,
                                                             uint64_t lost_samples, std::string_view reason) {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::unexpected(noRingError());
  }
  auto failed = writer_->failCapture(capture_id, at_sample, lost_samples, reason);
  if (failed) {
    begun_on_ = nullptr;
  }
  return failed;
}

std::optional<uint64_t> CaptureRing::slotPayloadBytes() const {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::nullopt;
  }
  return writer_->slotPayloadBytes();
}

std::optional<RingFill> CaptureRing::highWater() const {
  std::lock_guard lock(mutex_);
  if (writer_ == nullptr) {
    return std::nullopt;
  }
  return writer_->fill();
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: Order: beginCapture() BEFORE the capture's thread starts; join that thread BEFORE endCapture() unmaps the ring.

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ring_encoding.h"
#include "bulk_ring.h"

namespace bitoscope::plugin_ring {

std::expected<uint64_t, std::string> samplesPerSlot(uint32_t encoding, uint32_t num_channels,
                                                    uint64_t slot_payload_bytes);

struct RingChunk {
  uint64_t first_sample_index = 0;
  uint64_t sample_rate_milli_hz = 0;
  uint32_t num_channels = 0;
  uint64_t num_samples = 0;
  uint32_t encoding = 0;
};

enum class RingWriteErrorKind {
  NoRing,
  WrongCapture,
  Overrun,
  BadChunk,
  ConsumerGone,
  Stopped,
};

struct RingWriteError {
  RingWriteErrorKind kind = RingWriteErrorKind::NoRing;
  std::string message;
};

struct RingFill {
  uint64_t high_water_slots = 0;
  uint32_t slot_count = 0;
};

class RingWriter;

class CaptureRing {
 public:
  CaptureRing();
  ~CaptureRing();
  CaptureRing(const CaptureRing&) = delete;
  CaptureRing& operator=(const CaptureRing&) = delete;

  std::expected<void, std::string> beginCapture(std::string_view ring_name, uint64_t capture_id,
                                                bulk_ring::Timing timing = {});

  void captureStarted(bool started);

  void cancelWaits();

  void endCapture();

  std::expected<void, RingWriteError> write(uint64_t capture_id, const RingChunk& chunk,
                                            std::span<const uint8_t> payload);

  std::expected<std::span<uint8_t>, RingWriteError> beginChunk(uint64_t capture_id, uint64_t max_bytes,
                                                               bool blocking);

  std::expected<void, RingWriteError> commitChunk(const RingChunk& chunk, uint64_t payload_bytes);

  std::expected<void, RingWriteError> failCapture(uint64_t capture_id, uint64_t at_sample, uint64_t lost_samples,
                                                  std::string_view reason);

  std::optional<uint64_t> slotPayloadBytes() const;

  std::optional<RingFill> highWater() const;

 private:
  mutable std::mutex mutex_;
  // WHY: writer_ is written only on the command thread, which therefore may read it without mutex_.
  std::unique_ptr<RingWriter> writer_;
  const RingWriter* begun_on_ = nullptr;
  std::vector<std::unique_ptr<RingWriter>> retired_;

  void startFlushTimer(uint32_t flush_after_ms);
  void stopFlushTimer();
  void runFlushTimer(uint32_t flush_after_ms);
  std::thread flush_timer_;
  std::mutex flush_timer_mutex_;
  std::condition_variable flush_timer_cv_;
  bool flush_timer_stop_ = false;
};

}

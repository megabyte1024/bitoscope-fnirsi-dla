/*
 * SPDX-License-Identifier: MIT
 */

#include "la_runtime.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <expected>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "device_properties.h"
#include "device_bytes.h"
#include "display_text.h"
#include "generic_facet.h"
#include "facet_properties.h"
#include "frame_output.h"
#include "sink_facts.h"
#include "usb_stream_properties.h"

using bitoscope::plugin_ring::CaptureRing;
using bitoscope::plugin_ring::RingChunk;
using bitoscope::plugin_ring::RingWriteError;
using bitoscope::plugin_ring::RingWriteErrorKind;

// WHY: Sink fields are written on the capture thread only, except the atomics the main loop and watchdog set.
struct bitoscope_sink {
  CaptureRing* ring = nullptr;
  uint64_t captureId = 0;
  uint64_t rateMilliHz = 0;
  uint64_t limit = 0;
  uint32_t numChannels = 0;
  uint32_t encoding = 0;
  bool buffered = false;
  uint32_t deviceChannels = 0;
  uint32_t channelMask = 0;
  std::optional<bitoscope::plugin_c_sdk::DeviceByteCompactor> bytes;
  std::vector<uint8_t> scratch;
  bool written = false;
  uint32_t silenceLimitMs = 0;
  std::optional<bitoscope::plugin_c_sdk::UsbStreamChoice> usbChoice;

  bool starting = false;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> abandoned{false};
  std::atomic<bool> stalled{false};
  std::atomic<bool> waitingForRoom{false};
  std::atomic<bool> streamWatchesSilence{false};
  std::atomic<uint64_t> progress{0};

  uint64_t next = 0;
  bool delivering = true;
  std::optional<std::string> failure;
  std::optional<uint64_t> begunSamples;
  uint8_t* begunRoom = nullptr;
};

namespace bitoscope::plugin_c_sdk {

namespace {

int over(bitoscope_sink& sink) {
  sink.delivering = false;
  return BITOSCOPE_CAPTURE_OVER;
}

// WHY: The ring turns non-ASCII into '?', so the message goes through asciiText().
int failDelivery(bitoscope_sink& sink, std::string message, bool markRing) {
  if (markRing) (void)sink.ring->failCapture(sink.captureId, sink.next, 0, display_text::asciiText(message));
  logLine(ipc::LogLevel::error, "capture failed: " + message);
  sink.failure = std::move(message);
  return over(sink);
}

bool refusedInStart(const bitoscope_sink& sink, const char* what) {
  if (!sink.starting) return false;
  logLine(ipc::LogLevel::error, std::string(what) + " in prepare function: samples are written by capture function");
  return true;
}

int failStalled(bitoscope_sink& sink) {
  return failDelivery(sink,
                      "device stalled: no sample in " + std::to_string(sink.silenceLimitMs) + " ms after " +
                          std::to_string(sink.next) + " samples",
                      true);
}

int gate(bitoscope_sink& sink) {
  if (refusedInStart(sink, "write")) return BITOSCOPE_CAPTURE_OVER;
  if (!sink.delivering || sink.abandoned.load()) return over(sink);
  // WHY: A buffered capture writes out what its device holds AFTER STOP; a streaming one stops writing.
  if (!sink.buffered && sink.stopRequested.load()) return over(sink);
  if (sink.stalled.load()) return failStalled(sink);
  return BITOSCOPE_OK;
}

std::expected<std::span<uint8_t>, RingWriteError> roomFor(bitoscope_sink& sink, uint64_t bytes, bool wait) {
  sink.waitingForRoom = wait;
  auto room = sink.ring->beginChunk(sink.captureId, bytes, wait);
  sink.waitingForRoom = false;
  return room;
}

void advance(bitoscope_sink& sink, uint64_t samples) {
  sink.next += samples;
  sink.progress.store(sink.next, std::memory_order_relaxed);
}

// WHY: STOP cancelling a wait is not a failure; the ring already holds its own failures.
int ringError(bitoscope_sink& sink, const RingWriteError& error) {
  if (error.kind == RingWriteErrorKind::Stopped && (sink.stopRequested.load() || sink.abandoned.load() || sink.failure)) {
    return over(sink);
  }
  return failDelivery(sink, error.message, false);
}

uint64_t bytesFor(const bitoscope_sink& sink, uint64_t samples) {
  return plugin_ring::chunkPayloadBytes(sink.encoding, sink.numChannels, samples).value_or(0);
}

uint64_t unitsFor(const bitoscope_sink& sink, uint64_t samples) {
  const uint64_t perUnit = sink.bytes->unitSamples();
  return samples / perUnit + (samples % perUnit != 0 ? 1 : 0);
}

uint64_t driverBytesFor(const bitoscope_sink& sink, uint64_t samples) {
  return unitsFor(sink, samples) * sink.bytes->deviceUnitBytes();
}

uint64_t samplesPerChunk(const bitoscope_sink& sink, Room room) {
  const auto slot = sink.ring->slotPayloadBytes();
  if (!slot || !sink.bytes) return 0;
  const uint64_t unitBytes =
      room == Room::deviceBytes ? sink.bytes->deviceUnitBytes() : sink.bytes->capturedUnitBytes();
  return *slot / unitBytes * sink.bytes->unitSamples();
}

std::expected<void, std::string> describeBytes(bitoscope_sink& sink, const bitoscope_device_bytes& bytes) {
  auto compactor = DeviceByteCompactor::create(bytes, sink.deviceChannels, sink.channelMask);
  if (!compactor) return std::unexpected(std::move(compactor.error()));
  sink.bytes = std::move(*compactor);
  sink.encoding = sink.bytes->channelMajor() ? plugin_ring::kRingEncodingChannelMajor
                                             : plugin_ring::kRingEncodingSampleMajor;
  return {};
}

void compactInto(const bitoscope_sink& sink, const uint8_t* device, uint64_t samples, uint8_t* captured) {
  sink.bytes->compact(device, unitsFor(sink, samples), captured);
}

int delivered(bitoscope_sink& sink, uint64_t samples) {
  advance(sink, samples);
  if (sink.limit != 0 && sink.next >= sink.limit) return over(sink);
  return BITOSCOPE_OK;
}

uint64_t withinLimit(const bitoscope_sink& sink, uint64_t samples) {
  if (sink.limit == 0) return samples;
  return (std::min)(samples, sink.limit - sink.next);
}

int write(bitoscope_sink* sink, const void* samples, uint64_t count, bool wait) {
  if (sink == nullptr) return BITOSCOPE_CAPTURE_OVER;
  if (const int status = gate(*sink); status != BITOSCOPE_OK) return status;
  if (samples == nullptr && count > 0) return failDelivery(*sink, "write of samples from null pointer", true);
  if (sink->begunSamples) {
    return failDelivery(*sink, "write while chunk from bitoscope_begin_samples() is not committed", true);
  }
  sink->written = true;
  const uint64_t take = withinLimit(*sink, count);
  const auto* bytes = static_cast<const uint8_t*>(samples);
  const uint64_t total = bytesFor(*sink, take);
  const bool copies = sink->bytes->copies();
  if (!wait) {
    if (!copies) {
      try {
        sink->scratch.resize(static_cast<size_t>(total));
      } catch (const std::bad_alloc&) {
        return failDelivery(*sink, "out of memory for " + std::to_string(total) + " bytes of " +
                                       std::to_string(take) + " captured samples", true);
      }
      compactInto(*sink, bytes, take, sink->scratch.data());
      bytes = sink->scratch.data();
    }
    auto written = sink->ring->write(
        sink->captureId, RingChunk{sink->next, sink->rateMilliHz, sink->numChannels, take, sink->encoding},
        std::span<const uint8_t>(bytes, static_cast<size_t>(total)));
    if (!written) return ringError(*sink, written.error());
    return delivered(*sink, take);
  }
  // WHY: Each piece holds whole units of the device's bytes so it starts at a unit boundary of the caller's data.
  const uint64_t perChunk = samplesPerChunk(*sink, Room::capturedBytes);
  if (perChunk == 0) return failDelivery(*sink, "ring's slots cannot hold one sample of this capture", true);
  uint64_t done = 0;
  uint64_t offset = 0;
  while (done < take) {
    if (const int status = gate(*sink); status != BITOSCOPE_OK) return status;
    const uint64_t n = (std::min)(perChunk, take - done);
    const uint64_t pieceBytes = bytesFor(*sink, n);
    auto room = roomFor(*sink, pieceBytes, true);
    if (!room) return ringError(*sink, room.error());
    if (copies) {
      std::memcpy(room->data(), bytes + offset, static_cast<size_t>(pieceBytes));
    } else {
      compactInto(*sink, bytes + offset, n, room->data());
    }
    auto committed = sink->ring->commitChunk(
        RingChunk{sink->next, sink->rateMilliHz, sink->numChannels, n, sink->encoding}, pieceBytes);
    if (!committed) return ringError(*sink, committed.error());
    advance(*sink, n);
    done += n;
    offset += driverBytesFor(*sink, n);
  }
  return delivered(*sink, 0);
}

}

void* beginSamples(bitoscope_sink* sink, uint64_t max_samples, bool wait, Room room) {
  if (sink == nullptr || gate(*sink) != BITOSCOPE_OK) return nullptr;
  sink->written = true;
  if (sink->begunSamples) {
    (void)failDelivery(*sink, "bitoscope_begin_samples() again before bitoscope_commit_samples()", true);
    return nullptr;
  }
  const uint64_t most = samplesPerChunk(*sink, room);
  if (max_samples == 0 || max_samples > most) {
    (void)failDelivery(*sink,
                       "bitoscope_begin_samples() asked for room for " + std::to_string(max_samples) +
                           " samples; one chunk holds 1 to " + std::to_string(most) +
                           " (bitoscope_max_chunk_samples())", true);
    return nullptr;
  }
  const uint64_t bytes = room == Room::deviceBytes ? driverBytesFor(*sink, max_samples) : bytesFor(*sink, max_samples);
  auto begun = roomFor(*sink, bytes, wait);
  if (!begun) {
    (void)ringError(*sink, begun.error());
    return nullptr;
  }
  sink->begunSamples = max_samples;
  sink->begunRoom = begun->data();
  return begun->data();
}

int commitSamples(bitoscope_sink* sink, uint64_t num_samples, Room room) {
  if (sink == nullptr) return BITOSCOPE_CAPTURE_OVER;
  if (refusedInStart(*sink, "bitoscope_commit_samples()")) return BITOSCOPE_CAPTURE_OVER;
  if (!sink->begunSamples) {
    if (!sink->delivering) return BITOSCOPE_CAPTURE_OVER;
    return failDelivery(*sink, "bitoscope_commit_samples() without bitoscope_begin_samples()", true);
  }
  const uint64_t begun = *sink->begunSamples;
  sink->begunSamples.reset();
  if (num_samples > begun) {
    return failDelivery(*sink,
                        "bitoscope_commit_samples() of " + std::to_string(num_samples) + " samples, but room for " +
                            std::to_string(begun) + " was begun", true);
  }
  // WHY: A chunk begun before STOP is still delivered; only the samples up to the limit count.
  const uint64_t take = withinLimit(*sink, num_samples);
  if (room == Room::deviceBytes && !sink->bytes->copies()) compactInto(*sink, sink->begunRoom, take, sink->begunRoom);
  auto committed = sink->ring->commitChunk(
      RingChunk{sink->next, sink->rateMilliHz, sink->numChannels, take, sink->encoding}, bytesFor(*sink, take));
  if (!committed) return ringError(*sink, committed.error());
  return delivered(*sink, take);
}

namespace {

std::string rateText(uint64_t milliHz) {
  const ipc::props::Property rate{"rate", "rate", "", false, ipc::props::Unit::hertz,
                                  ipc::props::DecimalSpec{3, ipc::props::Unbounded{}, ipc::props::Unbounded{}, 0}};
  auto text = device_properties::formatValue(rate, ipc::props::PropertyValue{"rate", static_cast<int64_t>(milliHz)});
  return text ? *text : std::to_string(milliHz) + " mHz";
}

std::string unitText(const bitoscope_sink& sink) {
  const std::string samples = sink.bytes->channelMajor() ? "8 samples of " : "1 sample of ";
  return samples + std::to_string(sink.numChannels) + " channels, " + bytesText(sink.bytes->capturedUnitBytes());
}


}

SinkStreamFacts sinkStreamFacts(const bitoscope_sink& sink) {
  SinkStreamFacts facts;
  facts.rateMilliHz = sink.rateMilliHz;
  facts.buffered = sink.buffered;
  facts.starting = sink.starting;
  facts.silenceLimitMs = sink.silenceLimitMs;
  facts.usbChoice = sink.usbChoice;
  facts.chunkSamples = samplesPerChunk(sink, Room::capturedBytes);
  if (sink.bytes) {
    facts.unitSamples = sink.bytes->unitSamples();
    facts.unitBytes = sink.bytes->deviceUnitBytes();
    if (!sink.bytes->copies()) facts.compactor = &*sink.bytes;
  }
  return facts;
}

SilenceWatchedByStream::SilenceWatchedByStream(bitoscope_sink& sink) : sink_(sink) { sink_.streamWatchesSilence = true; }

SilenceWatchedByStream::~SilenceWatchedByStream() { sink_.streamWatchesSilence = false; }

namespace {

constexpr std::chrono::milliseconds kStopLimit{BITOSCOPE_STOP_LIMIT_MS};
constexpr const char* kStuckRefusal = "START refused: capture function of earlier capture has not returned since its STOP";

std::string stopLimitText() {
  return "capture function did not return within " + std::to_string(BITOSCOPE_STOP_LIMIT_MS) + " ms of STOP";
}

bool watchesSilence(const bitoscope_sink& sink) {
  return !sink.buffered && sink.silenceLimitMs != 0 && sink.silenceLimitMs != BITOSCOPE_NO_SILENCE_LIMIT;
}

bool silenceCounts(const bitoscope_sink& sink, uint64_t written) {
  if (written == 0) return false;
  if (sink.limit != 0 && written >= sink.limit) return false;
  return !sink.waitingForRoom.load() && !sink.streamWatchesSilence.load() && !sink.stopRequested.load() &&
         !sink.abandoned.load();
}

}

struct LaRuntime::Run {
  bitoscope_sink sink;
  bitoscope_capture capture{};
  std::vector<ipc::props::PropertyValue> properties;
  std::vector<bitoscope_property_value> cProperties;
  std::thread thread;
  std::thread watchdog;
  std::mutex mutex;
  std::condition_variable changed;
  bool returned = false;

  void markReturned() {
    {
      std::lock_guard lock(mutex);
      returned = true;
    }
    changed.notify_all();
  }

  bool hasReturned() {
    std::lock_guard lock(mutex);
    return returned;
  }

  bool waitReturned(std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, limit, [this] { return returned; });
  }

  void join() {
    if (thread.joinable()) thread.join();
    if (watchdog.joinable()) watchdog.join();
  }

  void watchSilence() {
    const std::chrono::milliseconds limit{sink.silenceLimitMs};
    const auto period = std::clamp<std::chrono::milliseconds>(limit / 8, std::chrono::milliseconds(1),
                                                              std::chrono::milliseconds(100));
    std::unique_lock lock(mutex);
    uint64_t seen = sink.progress.load(std::memory_order_relaxed);
    auto quietSince = std::chrono::steady_clock::now();
    while (!changed.wait_for(lock, period, [this] { return returned; })) {
      const uint64_t written = sink.progress.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      if (written != seen || !silenceCounts(sink, written)) {
        seen = written;
        quietSince = now;
        continue;
      }
      if (now - quietSince >= limit) {
        // WHY: Only set the flag: the capture thread fails the capture itself, so no sink field is written here.
        sink.stalled = true;
        return;
      }
    }
  }
};

LaRuntime::LaRuntime(const LogicAnalyzerRegistration& registration,
                     const std::vector<ipc::props::PropertyValue>& properties)
    : registration_(registration), properties_(properties) {}

LaRuntime::~LaRuntime() { abandon(); }

std::expected<void, ApplyRefusal> LaRuntime::apply(std::span<const ipc::props::PropertyValue> values, bool capturing) {
  if (!capturing) return {};
  return applyThrough(registration_.applyDuringCapture, registration_.session, values);
}

void LaRuntime::abandon() {
  if (!run_ && !stuck_) return;
  for (Run* run : {run_.get(), stuck_.get()}) {
    if (run != nullptr) run->sink.abandoned = true;
  }
  ring_.cancelWaits();
  for (std::unique_ptr<Run>* run : {&run_, &stuck_}) {
    if (*run) (*run)->join();
    run->reset();
  }
}

std::optional<std::string> LaRuntime::stopCapture(bool endRing) {
  if (!run_) return std::nullopt;
  run_->sink.stopRequested = true;
  // WHY: A streaming write waiting for room gives up; a buffered capture is still writing out and must finish.
  if (!run_->sink.buffered) ring_.cancelWaits();
  if (!run_->waitReturned(kStopLimit)) {
    // WHY: Keep the ring open: the capture thread may still hold a begun chunk in it.
    run_->sink.abandoned = true;
    ring_.cancelWaits();
    const std::string text = stopLimitText();
    logLine(ipc::LogLevel::error, text + "; every START is refused until it returns");
    stuck_ = std::move(run_);
    return text;
  }
  run_->join();
  if (endRing) ring_.endCapture();
  run_.reset();
  return std::nullopt;
}

bool LaRuntime::stuckCaptureReturned() {
  if (!stuck_) return true;
  if (!stuck_->hasReturned()) return false;
  stuck_->join();
  ring_.endCapture();
  stuck_.reset();
  return true;
}

namespace {

void runCapture(const LogicAnalyzerRegistration& la, bitoscope_sink& sink, const bitoscope_capture& capture) {
  try {
    la.capture(la.session, &capture, &sink);
  } catch (const std::exception& e) {
    (void)failDelivery(sink, std::string("capture function threw: ") + e.what(), !sink.failure);
  } catch (...) {
    (void)failDelivery(sink, "capture function threw", !sink.failure);
  }
  const bool stopped = sink.stopRequested.load() || sink.abandoned.load();
  if (sink.stalled.load() && !sink.failure && !stopped) (void)failStalled(sink);
  if (stopped) return;

  ipc::la::CaptureEnded ended{sink.captureId, ipc::la::EndReason::failure, {}};
  if (sink.failure) {
    ended.message = *sink.failure;
  } else if (sink.limit != 0 && sink.next >= sink.limit) {
    ended.reason = ipc::la::EndReason::sampleLimitReached;
  } else {
    std::string why = sink.limit != 0 ? "before sample limit of " + std::to_string(sink.limit)
                                      : "before STOP (capture with no sample limit runs until STOP)";
    (void)failDelivery(sink, "capture function returned after " + std::to_string(sink.next) + " samples, " + why, true);
    ended.message = *sink.failure;
  }
  auto payload = ipc::la::encodeEvent(ended);
  if (!payload) {
    logLine(ipc::LogLevel::error, "capture's end could not be encoded: " + payload.error());
    return;
  }
  if (auto sent = sendFacetEvent(kLaFacetName, std::move(*payload)); !sent) {
    logLine(ipc::LogLevel::error, "capture's end could not be sent: " + sent.error());
  }
}

}

FacetAnswer LaRuntime::handle(std::span<const uint8_t> payload) {
  auto command = ipc::la::decodeCommand(payload);
  if (!command) {
    const ipc::DecodeError& error = command.error();
    return ipc::Refusal{"la.acquisition cannot act on this command: " + error.message,
                        error.kind == ipc::DecodeErrorKind::unknownMember ? error.member : uint16_t{0}};
  }
  if (const auto* s = std::get_if<ipc::la::StartCapture>(&*command)) return start(*s);
  const bool wasRunning = run_ != nullptr;
  if (auto stuck = stopCapture(true)) return ipc::FacetReply{false, std::move(*stuck), {}};
  if (!stuckCaptureReturned()) return ipc::FacetReply{false, stopLimitText(), {}};
  return ipc::FacetReply{true, wasRunning ? "capture stopped" : "no capture was running", {}};
}

FacetAnswer LaRuntime::start(const ipc::la::StartCapture& s) {
  if (!stuckCaptureReturned()) return ipc::FacetReply{false, kStuckRefusal, {}};
  const uint32_t channels = registration_.numChannels;
  if (channels < 32 && (s.channelMask >> channels) != 0) {
    const int highest = 31 - std::countl_zero(s.channelMask);
    return ipc::FacetReply{false,
                           "START refused: channel mask " + maskText(s.channelMask) + " selects channel " +
                               std::to_string(highest) + ", but this logic analyzer has channels 0 to " +
                               std::to_string(channels - 1),
                           {}};
  }
  const auto& rates = registration_.sampleRatesMilliHz;
  if (std::find(rates.begin(), rates.end(), s.sampleRateMilliHz) == rates.end()) {
    return ipc::FacetReply{false,
                           "START refused: sample rate of " + std::to_string(s.sampleRateMilliHz) +
                               " mHz is not one this logic analyzer offers",
                           {}};
  }
  const auto selected = static_cast<uint32_t>(std::popcount(s.channelMask));
  const auto& ceilings = registration_.rateCeilingsMilliHz;
  if (selected >= 1 && selected <= ceilings.size() && s.sampleRateMilliHz > ceilings[selected - 1]) {
    const std::string& reason = registration_.rateCeilingReason;
    return ipc::FacetReply{false,
                           "START refused: " + std::to_string(selected) + " channels: at most " +
                               rateText(ceilings[selected - 1]) + ", and " + rateText(s.sampleRateMilliHz) +
                               " was asked for" + (reason.empty() ? "" : ": " + reason),
                           {}};
  }
  if (auto stuck = stopCapture(true)) return ipc::FacetReply{false, "START refused: " + *stuck, {}};
  if (auto opened = ring_.beginCapture(s.bulkRingName, s.captureId); !opened) {
    return ipc::FacetReply{false, "START refused: " + opened.error(), {}};
  }
  auto run = std::make_unique<Run>();
  run->sink.ring = &ring_;
  run->sink.captureId = s.captureId;
  run->sink.rateMilliHz = s.sampleRateMilliHz;
  run->sink.limit = s.sampleLimit;
  run->sink.numChannels = static_cast<uint32_t>(std::popcount(s.channelMask));
  run->sink.deviceChannels = channels;
  run->sink.channelMask = s.channelMask;
  run->sink.silenceLimitMs = registration_.silenceLimitMs;
  if (auto chosen = describeBytes(run->sink, registration_.deviceBytes); !chosen) {
    ring_.captureStarted(false);
    return ipc::FacetReply{false, "START refused: " + chosen.error(), {}};
  }
  run->sink.buffered = s.mode == ipc::la::AcquisitionMode::buffered;
  run->capture.sample_rate_milli_hz = s.sampleRateMilliHz;
  run->capture.channel_mask = s.channelMask;
  run->capture.num_captured_channels = run->sink.numChannels;
  run->capture.sample_limit = s.sampleLimit;
  run->capture.buffered = run->sink.buffered ? 1 : 0;
  // WHY: Copy the values: a setProperties during the capture must not change what the capture started with.
  run->properties = properties_;
  run->cProperties = cValues(run->properties);
  run->capture.properties = run->cProperties.empty() ? nullptr : run->cProperties.data();
  run->capture.num_properties = run->cProperties.size();
  if (registration_.usbStreamProperties) run->sink.usbChoice = usbStreamChoice(run->properties);
  if (registration_.prepare != nullptr) {
    bitoscope_reply reply;
    int status = BITOSCOPE_ERROR;
    run->sink.starting = true;
    try {
      status = registration_.prepare(registration_.session, &run->capture, &run->sink, &reply);
    } catch (const std::exception& e) {
      reply.message = std::string("prepare function threw: ") + e.what();
    } catch (...) {
      reply.message = "prepare function threw";
    }
    run->sink.starting = false;
    if (status != BITOSCOPE_OK) {
      ring_.captureStarted(false);
      return ipc::FacetReply{
          false, "START refused: " + (reply.message.empty() ? std::string("driver refused it") : reply.message), {}};
    }
  }
  if (samplesPerChunk(run->sink, Room::capturedBytes) == 0) {
    std::string refusal = "START refused: ring slot holds " + bytesText(ring_.slotPayloadBytes().value_or(0)) +
                          ", less than one unit of this capture (" + unitText(run->sink) +
                          "); nothing could be delivered";
    ring_.captureStarted(false);
    return ipc::FacetReply{false, std::move(refusal), {}};
  }
  try {
    Run* r = run.get();
    if (watchesSilence(r->sink)) r->watchdog = std::thread([r] { r->watchSilence(); });
    run->thread = std::thread([this, r] {
      runCapture(registration_, r->sink, r->capture);
      r->markReturned();
    });
  } catch (const std::system_error& e) {
    run->markReturned();
    run->join();
    ring_.captureStarted(false);
    return ipc::FacetReply{false, std::string("START refused: no capture thread could be started: ") + e.what(), {}};
  }
  ring_.captureStarted(true);
  run_ = std::move(run);
  return ipc::FacetReply{true, "capture started", {}};
}

}

namespace sdk = bitoscope::plugin_c_sdk;

namespace {

int overAfterThrow(bitoscope_sink* sink, const char* what) {
  if (sink == nullptr || sink->failure) return BITOSCOPE_CAPTURE_OVER;
  try {
    (void)sdk::failDelivery(*sink, std::string(what) + " failed", true);
  } catch (...) {
    sink->delivering = false;
  }
  return BITOSCOPE_CAPTURE_OVER;
}

}

extern "C" {

int bitoscope_write_samples(bitoscope_sink* sink, const void* samples, uint64_t num_samples) {
  try {
    return sdk::write(sink, samples, num_samples, true);
  } catch (...) {
    return overAfterThrow(sink, "bitoscope_write_samples()");
  }
}

int bitoscope_write_samples_now(bitoscope_sink* sink, const void* samples, uint64_t num_samples) {
  try {
    return sdk::write(sink, samples, num_samples, false);
  } catch (...) {
    return overAfterThrow(sink, "bitoscope_write_samples_now()");
  }
}

void* bitoscope_begin_samples(bitoscope_sink* sink, uint64_t max_samples, int wait) {
  try {
    return sdk::beginSamples(sink, max_samples, wait != 0, sdk::Room::deviceBytes);
  } catch (...) {
    (void)overAfterThrow(sink, "bitoscope_begin_samples()");
    return nullptr;
  }
}

int bitoscope_commit_samples(bitoscope_sink* sink, uint64_t num_samples) {
  try {
    return sdk::commitSamples(sink, num_samples, sdk::Room::deviceBytes);
  } catch (...) {
    return overAfterThrow(sink, "bitoscope_commit_samples()");
  }
}

uint64_t bitoscope_slot_bytes(const bitoscope_sink* sink) {
  if (sink == nullptr || sink->ring == nullptr) return 0;
  return sink->ring->slotPayloadBytes().value_or(0);
}

uint64_t bitoscope_max_chunk_samples(const bitoscope_sink* sink) {
  if (sink == nullptr) return 0;
  return sdk::samplesPerChunk(*sink, sdk::Room::deviceBytes);
}

uint64_t bitoscope_sample_bytes(const bitoscope_sink* sink, uint64_t num_samples) {
  if (sink == nullptr) return 0;
  return sdk::driverBytesFor(*sink, num_samples);
}

int bitoscope_stopping(const bitoscope_sink* sink) {
  if (sink == nullptr) return 1;
  return sink->stopRequested.load() || sink->abandoned.load() || sink->stalled.load() ? 1 : 0;
}

int bitoscope_capture_failed(const bitoscope_sink* sink) {
  return sink != nullptr && (sink->failure || sink->stalled.load()) ? 1 : 0;
}

int bitoscope_set_device_bytes(bitoscope_sink* sink, const bitoscope_device_bytes* bytes) {
  try {
    if (sink == nullptr || bytes == nullptr) return BITOSCOPE_ERROR;
    if (!sink->starting && (sink->written || sink->failure || !sink->delivering)) {
      sdk::logLine(bitoscope::ipc::LogLevel::error,
                   "bitoscope_set_device_bytes() after first write: capture's bytes are described before it writes");
      return BITOSCOPE_ERROR;
    }
    if (auto described = sdk::describeBytes(*sink, *bytes); !described) {
      sdk::logLine(bitoscope::ipc::LogLevel::error, "bitoscope_set_device_bytes(): " + described.error());
      return BITOSCOPE_ERROR;
    }
    if (!sink->starting && sdk::samplesPerChunk(*sink, sdk::Room::capturedBytes) == 0) {
      (void)sdk::failDelivery(*sink,
                              "ring slot holds " + sdk::bytesText(sink->ring->slotPayloadBytes().value_or(0)) +
                                  ", less than one unit of this capture (" + sdk::unitText(*sink) +
                                  "); nothing could be delivered",
                              true);
      return BITOSCOPE_ERROR;
    }
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_ring_fill(const bitoscope_sink* sink, uint64_t* high_water_slots, uint64_t* slot_count) {
  try {
    if (sink == nullptr || high_water_slots == nullptr || slot_count == nullptr) return BITOSCOPE_ERROR;
    const auto fill = sink->ring->highWater();
    if (!fill) return BITOSCOPE_ERROR;
    *high_water_slots = fill->high_water_slots;
    *slot_count = fill->slot_count;
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

void bitoscope_fail_capture(bitoscope_sink* sink, const char* reason) {
  try {
    if (sink == nullptr || sink->failure) return;  // WHY: the first failure stands
    if (sdk::refusedInStart(*sink, "bitoscope_fail_capture()")) return;
    sink->begunSamples.reset();
    (void)sdk::failDelivery(*sink, reason != nullptr && reason[0] != '\0' ? reason : "driver reported failure", true);
  } catch (...) {
  }
}

}

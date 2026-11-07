/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_stream.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libusb_stream_transport.h"
#include "sink_facts.h"

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
constexpr uint32_t kMaxUnitBytes = 4096;
constexpr uint32_t kMaxUnitSamples = 4096;
constexpr size_t kRecentStatuses = 8;
constexpr std::string_view kCompletedName = "LIBUSB_TRANSFER_COMPLETED";

uint64_t saturatingMul(uint64_t a, uint64_t b) { return b != 0 && a > kMax / b ? kMax : a * b; }

uint64_t ceilDiv(uint64_t a, uint64_t b) { return a / b + (a % b != 0 ? 1 : 0); }

std::string hexByte(uint8_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return std::string("0x") + kDigits[value >> 4] + kDigits[value & 0xF];
}

std::string sizeText(uint64_t bytes) {
  if (bytes >= 1024 * 1024) return std::to_string(bytes >> 20) + " MiB";
  if (bytes >= 1024) return std::to_string(bytes >> 10) + " KiB";
  return std::to_string(bytes) + " bytes";
}

void logText(int level, const std::string& line) { bitoscope_log(level, "%s", line.c_str()); }

struct Settings {
  uint8_t endpoint = 0;
  uint32_t transfers = 0;
  uint32_t transferBytes = 0;
  uint64_t unitBytes = 0;
  uint64_t unitSamples = 0;
  uint64_t outUnitBytes = 0;
  uint64_t unitsPerChunk = 0;
  uint64_t prerollBytes = 0;
  bool drain = false;
  std::optional<uint64_t> silenceLimitMs;
  uint64_t lagLimitBytes = 0;
  uint64_t bytesPerSecond = 0;
  bitoscope_usb_started_fn started = nullptr;
  bitoscope_usb_convert_fn convert = nullptr;
  const DeviceByteCompactor* compactor = nullptr;
  bitoscope_usb_stop_fn stop = nullptr;
  void* user = nullptr;
  std::string deviceName;
};

std::expected<Settings, std::string> settingsFor(bitoscope_sink* sink, const SinkStreamFacts& facts,
                                                 const bitoscope_usb_stream& stream) {
  if ((stream.endpoint & 0x80) == 0) {
    return std::unexpected("endpoint " + hexByte(stream.endpoint) +
                           " is OUT endpoint: samples arrive on IN endpoint (0x80 set)");
  }
  if (stream.transfers != 0 && (stream.transfers < kMinUsbTransfers || stream.transfers > kMaxUsbTransfers)) {
    return std::unexpected("transfers " + std::to_string(stream.transfers) + ": " + std::to_string(kMinUsbTransfers) +
                           " to " + std::to_string(kMaxUsbTransfers) + " can be in flight");
  }
  if (stream.transfer_bytes % kUsbTransferGranuleBytes != 0) {
    return std::unexpected("transfer_bytes " + std::to_string(stream.transfer_bytes) + " is not multiple of " +
                           std::to_string(kUsbTransferGranuleBytes));
  }
  if (stream.transfer_bytes > kMaxUsbTransferBytes) {
    return std::unexpected("transfer_bytes " + std::to_string(stream.transfer_bytes) + " is above " +
                           std::to_string(kMaxUsbTransferBytes));
  }
  const bool converts = stream.convert != nullptr;
  if (converts && (stream.unit_bytes == 0 || stream.unit_samples == 0)) {
    return std::unexpected(std::string("convert needs unit_bytes and unit_samples"));
  }
  if (!converts && (stream.unit_bytes != 0 || stream.unit_samples != 0)) {
    return std::unexpected(std::string("unit_bytes and unit_samples need convert"));
  }
  if (stream.unit_bytes > kMaxUnitBytes || stream.unit_samples > kMaxUnitSamples) {
    return std::unexpected("unit of " + std::to_string(stream.unit_bytes) + " bytes and " +
                           std::to_string(stream.unit_samples) + " samples is above " + std::to_string(kMaxUnitBytes) +
                           " of either");
  }
  if (converts && stream.unit_samples % facts.unitSamples != 0) {
    return std::unexpected("unit_samples " + std::to_string(stream.unit_samples) + " is not multiple of " +
                           std::to_string(facts.unitSamples) + " (BITOSCOPE_CHANNEL_MAJOR)");
  }
  Settings s;
  s.endpoint = stream.endpoint;
  s.unitBytes = converts ? stream.unit_bytes : facts.unitBytes;
  s.unitSamples = converts ? stream.unit_samples : facts.unitSamples;
  s.compactor = converts ? nullptr : facts.compactor;
  s.outUnitBytes = converts      ? bitoscope_sample_bytes(sink, s.unitSamples)
                   : s.compactor ? s.compactor->capturedUnitBytes()
                                 : s.unitBytes;
  const uint64_t slotSamples = converts ? bitoscope_max_chunk_samples(sink) : facts.chunkSamples;
  s.unitsPerChunk = slotSamples / s.unitSamples;
  if (s.unitsPerChunk == 0) {
    return std::unexpected("ring slot holds " + std::to_string(slotSamples) + " samples, fewer than one unit of " +
                           std::to_string(s.unitSamples));
  }
  const uint64_t prerollUnits = ceilDiv(stream.preroll_samples, s.unitSamples);
  if (prerollUnits > kMax / s.unitBytes) {
    return std::unexpected("preroll_samples " + std::to_string(stream.preroll_samples) + " is too many");
  }
  s.prerollBytes = prerollUnits * s.unitBytes;
  s.bytesPerSecond = streamBytesPerSecond(facts.rateMilliHz, s.unitBytes, s.unitSamples);
  uint32_t transfers = stream.transfers;
  uint64_t transferBytes = stream.transfer_bytes;
  if (facts.usbChoice) {
    if (transfers != 0 || transferBytes != 0) {
      return std::unexpected(
          std::string("transfers and transfer_bytes come from USB properties (usb_stream_properties): leave them 0"));
    }
    transfers = facts.usbChoice->transfers;
    transferBytes = uint64_t{facts.usbChoice->transferKib} * kUsbTransferGranuleBytes;
  }
  s.transfers = transfers != 0 ? transfers : kDefaultUsbTransfers;
  s.transferBytes = transferBytes != 0 ? static_cast<uint32_t>(transferBytes)
                                       : defaultTransferBytes(facts.rateMilliHz, s.unitBytes, s.unitSamples);
  if (facts.buffered || facts.silenceLimitMs == BITOSCOPE_NO_SILENCE_LIMIT) {
    s.silenceLimitMs.reset();
  } else if (facts.silenceLimitMs != 0) {
    s.silenceLimitMs = facts.silenceLimitMs;
  } else {
    s.silenceLimitMs = defaultSilenceLimitMs(s.transferBytes, facts.rateMilliHz, s.unitBytes, s.unitSamples);
  }
  s.lagLimitBytes = facts.buffered ? 0 : stream.lag_limit_bytes;
  s.drain = stream.drain != 0;
  s.started = stream.started;
  s.convert = stream.convert;
  s.stop = stream.stop;
  s.user = stream.user;
  s.deviceName = stream.device_name != nullptr && stream.device_name[0] != '\0' ? stream.device_name : "USB device";
  if (stream.device == nullptr) return std::unexpected(std::string("bitoscope_stream_usb(): no USB device"));
  return s;
}

class DeliveryLag {
 public:
  DeliveryLag(uint64_t bytesPerSecond, uint64_t limitBytes)
      : bytesPerSecond_(bytesPerSecond), limitBytes_(limitBytes) {}

  std::optional<uint64_t> onDelivered(uint64_t nowMs, uint64_t deliveredBytes) {
    if (bytesPerSecond_ == 0 || limitBytes_ == 0) return std::nullopt;
    const bool first = !started_;
    if (first) {
      started_ = true;
      startMs_ = nowMs;
    }
    const auto produced = static_cast<int64_t>(saturatingMul(nowMs - startMs_, bytesPerSecond_) / 1000);
    const int64_t behind = produced - static_cast<int64_t>(deliveredBytes);
    if (first || behind < leastBehind_) leastBehind_ = behind;
    lagBytes_ = static_cast<uint64_t>(behind - leastBehind_);
    if (lagBytes_ >= limitBytes_) return lagBytes_;
    return std::nullopt;
  }

  uint64_t lagMs() const { return bytesPerSecond_ == 0 ? 0 : saturatingMul(lagBytes_, 1000) / bytesPerSecond_; }

 private:
  uint64_t bytesPerSecond_;
  uint64_t limitBytes_;
  bool started_ = false;
  uint64_t startMs_ = 0;
  int64_t leastBehind_ = 0;
  uint64_t lagBytes_ = 0;
};

class RecentStatuses {
 public:
  void remember(std::string_view name) {
    names_[count_ % names_.size()].assign(name);
    ++count_;
  }

  std::string text() const {
    if (count_ == 0) return "none";
    const size_t held = (std::min)(count_, names_.size());
    std::string out;
    std::string_view runName;
    size_t run = 0;
    const auto flush = [&] {
      if (run == 0) return;
      if (!out.empty()) out += ", ";
      out += std::string(runName) + " x" + std::to_string(run);
    };
    for (size_t k = count_ - held; k < count_; ++k) {
      const std::string_view name = names_[k % names_.size()];
      if (run > 0 && name == runName) {
        ++run;
        continue;
      }
      flush();
      runName = name;
      run = 1;
    }
    flush();
    return out;
  }

 private:
  std::array<std::string, kRecentStatuses> names_;
  size_t count_ = 0;
};

class UsbStreamRun {
 public:
  UsbStreamRun(bitoscope_sink* sink, const Settings& settings, UsbStreamTransport& transport)
      : sink_(sink),
        s_(settings),
        t_(transport),
        prerollLeft_(settings.prerollBytes),
        lag_(settings.bytesPerSecond, settings.lagLimitBytes) {}

  int run() {
    if (s_.drain) drain();
    if (end_ == End::none) startTransfers();
    if (end_ == End::none && s_.started != nullptr && s_.started(s_.user) != BITOSCOPE_OK) {
      fail(s_.deviceName + " start failed");
    }
    lastActivityMs_ = t_.nowMs();
    while (end_ == End::none) {
      if (bitoscope_stopping(sink_) != 0) {
        end_ = End::done;
        break;
      }
      done_.clear();
      t_.waitForCompletions(kStreamPollMs, done_);
      const uint64_t now = t_.nowMs();
      for (const TransferDone& d : done_) markDone(d, now);
      processInOrder(now);
      if (end_ == End::none) checkStall(now);
    }
    cancelAndWait();
    switch (end_) {
      case End::done:
        return BITOSCOPE_OK;
      case End::gone:
        return BITOSCOPE_DEVICE_GONE;
      case End::none:
      case End::failed:
        break;
    }
    return BITOSCOPE_ERROR;
  }

 private:
  enum class End { none, done, failed, gone };

  struct Slot {
    bool inFlight = false;
    bool done = false;
    TransferStatus status = TransferStatus::completed;
    uint64_t bytes = 0;
  };

  std::string at() const { return std::to_string(delivered_); }

  std::string afterDelivered() const {
    return ", after " + at() + " samples were delivered - capture fails at sample " + at();
  }

  void fail(const std::string& reason) {
    bitoscope_fail_capture(sink_, reason.c_str());
    end_ = End::failed;
  }

  void gone(std::string_view usbName) {
    const std::string reason = s_.deviceName + " was disconnected (USB: " + std::string(usbName) + ")";
    logText(BITOSCOPE_LOG_ERROR, reason + afterDelivered());
    bitoscope_device_gone(sink_, reason.c_str());
    end_ = End::gone;
  }

  void drain() {
    std::vector<uint8_t> scratch(kDrainReadBytes);
    uint64_t thrownAway = 0;
    const uint64_t startMs = t_.nowMs();
    for (;;) {
      const auto read = t_.readNow(scratch, kDrainReadTimeoutMs);
      if (!read) {
        if (read.error().noDevice) {
          gone(read.error().name);
          return;
        }
        logText(BITOSCOPE_LOG_WARNING, "bulk IN drain ended with " + read.error().name + " after " +
                                           std::to_string(thrownAway) + " bytes thrown away");
        return;
      }
      if (*read == 0) return;
      thrownAway += *read;
      const uint64_t elapsedMs = t_.nowMs() - startMs;
      if (elapsedMs >= kDrainLimitMs) {
        logText(BITOSCOPE_LOG_ERROR, "stale data from earlier run could not be cleared: " + s_.deviceName +
                                         " still sending after " + std::to_string(elapsedMs) + " ms, " +
                                         std::to_string(thrownAway) + " bytes thrown away; capture fails at sample 0");
        fail("stale data of earlier run not cleared: " + s_.deviceName + " still sending, " +
             std::to_string(thrownAway) + " bytes in " + std::to_string(elapsedMs) + " ms");
        return;
      }
    }
  }

  void startTransfers() {
    if (auto allocated = t_.allocate(s_.transfers, s_.transferBytes); !allocated) {
      fail("USB transfers could not be set up: " + allocated.error());
      return;
    }
    slots_.assign(s_.transfers, Slot{});
    for (uint32_t i = 0; i < s_.transfers; ++i) {
      if (!submit(i)) return;
    }
    logText(BITOSCOPE_LOG_INFO, "USB stream: " + std::to_string(s_.transfers) + " transfers of " +
                                    std::to_string(s_.transferBytes / 1024) + " KiB in flight on endpoint " +
                                    hexByte(s_.endpoint));
  }

  bool submit(uint32_t transfer) {
    if (auto submitted = t_.submit(transfer); !submitted) {
      if (submitted.error().noDevice) {
        gone(submitted.error().name);
      } else {
        fail("USB transfer could not be queued: " + submitted.error().name);
      }
      return false;
    }
    slots_[transfer].inFlight = true;
    ++inFlight_;
    order_.push_back(transfer);
    return true;
  }

  void markDone(const TransferDone& d, uint64_t now) {
    Slot& slot = slots_[d.transfer];
    slot.inFlight = false;
    --inFlight_;
    slot.done = true;
    slot.status = d.status;
    slot.bytes = d.bytes;
    lastActivityMs_ = now;
  }

  // WHY: Deliver in submission order: the device filled the transfers in that order, whatever order they complete in.
  void processInOrder(uint64_t now) {
    while (end_ == End::none && !order_.empty() && slots_[order_.front()].done) {
      const uint32_t transfer = order_.front();
      order_.pop_front();
      slots_[transfer].done = false;
      handle(transfer, slots_[transfer].status, slots_[transfer].bytes, now);
    }
  }

  void handle(uint32_t transfer, TransferStatus status, uint64_t bytes, uint64_t now) {
    if (status == TransferStatus::noDevice) {
      recent_.remember(transferStatusName(status));
      gone(transferStatusName(status));
      return;
    }
    if (status != TransferStatus::completed) {
      recent_.remember(transferStatusName(status));
      const std::string name(transferStatusName(status));
      logText(BITOSCOPE_LOG_ERROR,
              "USB transfer ended with " + name + " carrying " + std::to_string(bytes) + " bytes" + afterDelivered());
      fail("USB transfer ended with " + name + "; stream lost from sample " + at());
      return;
    }
    streamBytes_ += bytes;
    // WHY: The transfer that shows the lag is not delivered: data may already be missing before it.
    if (const auto lag = lag_.onDelivered(now, streamBytes_)) {
      recent_.remember(kCompletedName);
      const std::string reason = "delivery fell " + sizeText(*lag) + " (" + std::to_string(lag_.lagMs()) +
                                 " ms) behind sampling; device may drop data";
      logText(BITOSCOPE_LOG_ERROR, reason + afterDelivered());
      fail(reason);
      return;
    }
    deliver(t_.received(transfer).first(static_cast<size_t>(bytes)));
    if (end_ != End::none) {
      recent_.remember(kCompletedName);
      return;
    }
    resubmit(transfer, bytes);
  }

  void resubmit(uint32_t transfer, uint64_t bytes) {
    auto submitted = t_.submit(transfer);
    if (submitted) {
      recent_.remember(kCompletedName);
      slots_[transfer].inFlight = true;
      ++inFlight_;
      order_.push_back(transfer);
      return;
    }
    recent_.remember(submitted.error().name);
    if (submitted.error().noDevice) {
      gone(submitted.error().name);
      return;
    }
    ++refused_;
    logText(BITOSCOPE_LOG_ERROR, "USB transfer resubmission refused with " + submitted.error().name + " (its " +
                                     std::to_string(bytes) + " bytes were delivered), after " + at() +
                                     " samples were delivered; " + std::to_string(inFlight_) +
                                     " transfer(s) still in flight (" + std::to_string(refused_) +
                                     " refused in this stream)");
  }

  Room roomOf() const { return s_.compactor != nullptr ? Room::capturedBytes : Room::deviceBytes; }

  bool transformUnits(const uint8_t* device, uint64_t units, uint8_t* samples) {
    if (units == 0) return true;
    if (s_.convert == nullptr) {
      s_.compactor->compact(device, units, samples);
      return true;
    }
    if (s_.convert(s_.user, device, units, samples) == BITOSCOPE_OK) return true;
    fail(s_.deviceName + " bytes could not be converted");
    return false;
  }

  // WHY: Cut only at whole units: a cut inside one would shift every channel after it.
  void deliver(std::span<const uint8_t> data) {
    if (prerollLeft_ != 0) {
      const uint64_t drop = (std::min)(static_cast<uint64_t>(data.size()), prerollLeft_);
      prerollLeft_ -= drop;
      data = data.subspan(static_cast<size_t>(drop));
    }
    while (!data.empty() && end_ == End::none) {
      const uint64_t carried = carry_.size();
      const uint64_t have = carried + data.size();
      if (have < s_.unitBytes) {
        carry_.insert(carry_.end(), data.begin(), data.end());
        return;
      }
      const uint64_t units = (std::min)(have / s_.unitBytes, s_.unitsPerChunk);
      const uint64_t samples = units * s_.unitSamples;
      auto* room = static_cast<uint8_t*>(beginSamples(sink_, samples, false, roomOf()));
      if (room == nullptr) {
        end_ = bitoscope_capture_failed(sink_) != 0 ? End::failed : End::done;
        return;
      }
      const auto taken = static_cast<size_t>(units * s_.unitBytes - carried);
      if (s_.convert == nullptr && s_.compactor == nullptr) {
        if (carried != 0) std::memcpy(room, carry_.data(), static_cast<size_t>(carried));
        std::memcpy(room + carried, data.data(), taken);
      } else {
        uint64_t first = 0;
        size_t from = 0;
        if (carried != 0) {
          from = static_cast<size_t>(s_.unitBytes - carried);
          unit_.assign(carry_.begin(), carry_.end());
          unit_.insert(unit_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(from));
          if (!transformUnits(unit_.data(), 1, room)) return;
          first = 1;
        }
        if (!transformUnits(data.data() + from, units - first, room + first * s_.outUnitBytes)) return;
      }
      carry_.clear();
      data = data.subspan(taken);
      if (commitSamples(sink_, samples, roomOf()) != BITOSCOPE_OK) {
        end_ = bitoscope_capture_failed(sink_) != 0 ? End::failed : End::done;
        return;
      }
      delivered_ += samples;
    }
  }

  void checkStall(uint64_t now) {
    if (inFlight_ == 0) {
      const std::string recent = recent_.text();
      logText(BITOSCOPE_LOG_ERROR, "USB stream dead: no transfer in flight, every resubmission refused" +
                                       afterDelivered() + "; nothing more can arrive. Last statuses: " + recent);
      fail("no USB transfer in flight; last: " + recent);
      return;
    }
    const uint64_t silentMs = now > lastActivityMs_ ? now - lastActivityMs_ : 0;
    if (!s_.silenceLimitMs || silentMs < *s_.silenceLimitMs) return;
    const std::string recent = recent_.text();
    const std::string flying = std::to_string(inFlight_);
    logText(BITOSCOPE_LOG_ERROR, "USB stream stalled: no transfer completed in " + std::to_string(silentMs) +
                                     " ms with " + flying + " in flight" + afterDelivered() +
                                     ". Last statuses: " + recent);
    fail("no USB transfer completed in " + std::to_string(silentMs) + " ms, " + flying + " in flight; last: " + recent);
  }

  // WHY: Return only when every transfer came back: libusb writes into its buffer until its completion arrives.
  void cancelAndWait() {
    for (uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].inFlight) t_.cancel(i);
    }
    while (inFlight_ > 0) {
      done_.clear();
      t_.waitForCompletions(kStreamPollMs, done_);
      for (const TransferDone& d : done_) {
        slots_[d.transfer].inFlight = false;
        --inFlight_;
      }
    }
  }

  bitoscope_sink* sink_;
  const Settings& s_;
  UsbStreamTransport& t_;
  std::vector<Slot> slots_;
  std::deque<uint32_t> order_;
  std::vector<TransferDone> done_;
  std::vector<uint8_t> carry_;
  std::vector<uint8_t> unit_;
  uint32_t inFlight_ = 0;
  End end_ = End::none;
  uint64_t delivered_ = 0;
  uint64_t streamBytes_ = 0;
  uint64_t prerollLeft_ = 0;
  uint64_t lastActivityMs_ = 0;
  uint64_t refused_ = 0;
  DeliveryLag lag_;
  RecentStatuses recent_;
};

// WHY: Copy only `size` bytes of a caller's struct: a driver built against an older header passes a shorter one.
template <typename T>
T copiedFromPossiblyShorterStruct(const T* from, size_t size) {
  T out{};
  std::memcpy(&out, from, (std::min)(size, sizeof(T)));
  return out;
}

bool refusedInStart(const bitoscope_sink& sink) {
  if (!sinkStreamFacts(sink).starting) return false;
  logText(BITOSCOPE_LOG_ERROR, "bitoscope_stream_usb() in prepare function: samples are streamed by capture function");
  return true;
}

void stopDevice(const bitoscope_usb_stream& stream) {
  if (stream.stop == nullptr || stream.stop(stream.user) == BITOSCOPE_OK) return;
  const bool named = stream.device_name != nullptr && stream.device_name[0] != '\0';
  logText(BITOSCOPE_LOG_WARNING, std::string(named ? stream.device_name : "USB device") + " stop failed");
}

int failedBeforeStream(bitoscope_sink* sink, const bitoscope_usb_stream& stream, const std::string& reason) {
  bitoscope_fail_capture(sink, reason.c_str());
  stopDevice(stream);
  return BITOSCOPE_ERROR;
}

}

CppStream::CppStream(plugin::Sink& sink, const plugin::UsbStream& stream)
    : sink_(sink),
      stream_(stream),
      outUnitBytes_(stream.unitSamples != 0 ? sink.sampleBytes(stream.unitSamples) : 0) {
  c_.device = stream.device != nullptr ? stream.device->get() : nullptr;
  c_.endpoint = stream.endpoint;
  c_.user = this;
  c_.started = stream.started ? &CppStream::started : nullptr;
  c_.stop = stream.stop ? &CppStream::stop : nullptr;
  c_.drain = stream.drain ? 1 : 0;
  c_.preroll_samples = stream.prerollSamples;
  c_.transfer_bytes = stream.transferBytes;
  c_.transfers = stream.transfers;
  c_.lag_limit_bytes = stream.lagLimitBytes;
  c_.convert = stream.convert ? &CppStream::convert : nullptr;
  c_.unit_bytes = stream.unitBytes;
  c_.unit_samples = stream.unitSamples;
  c_.device_name = stream.deviceName.empty() ? nullptr : stream.deviceName.c_str();
}

int CppStream::failed(std::string_view reason) {
  sink_.fail(reason);
  return BITOSCOPE_ERROR;
}

std::string CppStream::deviceName() const { return stream_.deviceName.empty() ? "USB device" : stream_.deviceName; }

int CppStream::stop(void* user) {
  auto* self = static_cast<CppStream*>(user);
  std::string why;
  try {
    auto stopped = self->stream_.stop();
    if (stopped) return BITOSCOPE_OK;
    why = stopped.error();
  } catch (const std::exception& e) {
    why = std::string("stop threw: ") + e.what();
  } catch (...) {
    why = "stop threw";
  }
  logText(BITOSCOPE_LOG_WARNING, self->deviceName() + " stop failed: " + why);
  return BITOSCOPE_OK;
}

template <typename Call>
int CppStream::guarded(Call&& call, std::string_view what) {
  try {
    auto result = call();
    return result ? BITOSCOPE_OK : failed(result.error());
  } catch (const std::exception& e) {
    return failed(std::string(what) + " threw: " + e.what());
  } catch (...) {
    return failed(std::string(what) + " threw");
  }
}

int CppStream::started(void* user) {
  auto* self = static_cast<CppStream*>(user);
  return self->guarded([self] { return self->stream_.started(); }, "started");
}

int CppStream::convert(void* user, const uint8_t* device, uint64_t units, uint8_t* samples) {
  auto* self = static_cast<CppStream*>(user);
  const std::span<const uint8_t> in(device, static_cast<size_t>(units * self->stream_.unitBytes));
  const std::span<uint8_t> out(samples, static_cast<size_t>(units * self->outUnitBytes_));
  return self->guarded([self, in, out] { return self->stream_.convert(in, out); }, "convert");
}

std::string_view transferStatusName(TransferStatus status) {
  switch (status) {
    case TransferStatus::completed:
      return kCompletedName;
    case TransferStatus::error:
      return "LIBUSB_TRANSFER_ERROR";
    case TransferStatus::timedOut:
      return "LIBUSB_TRANSFER_TIMED_OUT";
    case TransferStatus::cancelled:
      return "LIBUSB_TRANSFER_CANCELLED";
    case TransferStatus::stall:
      return "LIBUSB_TRANSFER_STALL";
    case TransferStatus::noDevice:
      return "LIBUSB_TRANSFER_NO_DEVICE";
    case TransferStatus::overflow:
      return "LIBUSB_TRANSFER_OVERFLOW";
    case TransferStatus::unknown:
      break;
  }
  return "LIBUSB_TRANSFER_(unknown status)";
}

uint64_t streamBytesPerSecond(uint64_t rateMilliHz, uint64_t unitBytes, uint64_t unitSamples) {
  if (unitSamples == 0) return 0;
  return saturatingMul(rateMilliHz, unitBytes) / saturatingMul(unitSamples, 1000);
}

uint32_t defaultTransferBytes(uint64_t rateMilliHz, uint64_t unitBytes, uint64_t unitSamples) {
  if (rateMilliHz == 0 || unitSamples == 0) return kMaxDefaultTransferBytes;
  const uint64_t bytes = ceilDiv(saturatingMul(saturatingMul(rateMilliHz, unitBytes), kDefaultTransferFillMs),
                                 saturatingMul(unitSamples, 1'000'000));
  const uint64_t rounded = saturatingMul(ceilDiv(bytes, kUsbTransferGranuleBytes), kUsbTransferGranuleBytes);
  return static_cast<uint32_t>(std::clamp<uint64_t>(rounded, kUsbTransferGranuleBytes, kMaxDefaultTransferBytes));
}

uint64_t defaultSilenceLimitMs(uint64_t transferBytes, uint64_t rateMilliHz, uint64_t unitBytes, uint64_t unitSamples) {
  const uint64_t perSecond = saturatingMul(rateMilliHz, unitBytes);
  if (perSecond == 0) return kSilenceFloorMs;
  const uint64_t periodMs = ceilDiv(saturatingMul(saturatingMul(transferBytes, 1'000'000), unitSamples), perSecond);
  return (std::max)(kSilenceFloorMs, saturatingMul(periodMs, kSilenceTransferPeriods));
}

int runUsbStream(bitoscope_sink* sink, const bitoscope_usb_stream& stream, UsbStreamTransport& transport) {
  if (sink == nullptr) return BITOSCOPE_ERROR;
  try {
    if (refusedInStart(*sink)) return BITOSCOPE_ERROR;
    const auto settings = settingsFor(sink, sinkStreamFacts(*sink), stream);
    if (!settings) return failedBeforeStream(sink, stream, settings.error());
    UsbStreamRun run(sink, *settings, transport);
    const SilenceWatchedByStream watched(*sink);
    const int answer = run.run();
    if (answer != BITOSCOPE_DEVICE_GONE) stopDevice(stream);
    return answer;
  } catch (const std::exception& e) {
    return failedBeforeStream(sink, stream, std::string("USB stream failed: ") + e.what());
  } catch (...) {
    return failedBeforeStream(sink, stream, "USB stream failed");
  }
}

void streamUsbThrough(plugin::Sink& sink, const plugin::UsbStream& stream, UsbStreamTransport& transport) {
  CppStream bridge(sink, stream);
  (void)runUsbStream(sink.get(), bridge.c(), transport);
}

}

namespace bitoscope::plugin {

void streamUsb(Sink& sink, const UsbStream& stream) {
  plugin_c_sdk::CppStream bridge(sink, stream);
  bitoscope_stream_usb(sink.get(), &bridge.c());
}

}

extern "C" void bitoscope_stream_usb_sized(bitoscope_sink* sink, const bitoscope_usb_stream* stream,
                                           size_t stream_size) {
  namespace sdk = bitoscope::plugin_c_sdk;
  try {
    if (sink == nullptr || sdk::refusedInStart(*sink)) return;
    if (stream == nullptr) {
      bitoscope_fail_capture(sink, "bitoscope_stream_usb(): no stream description");
      return;
    }
    const bitoscope_usb_stream copied = sdk::copiedFromPossiblyShorterStruct(stream, stream_size);
    if (const auto settings = sdk::settingsFor(sink, sdk::sinkStreamFacts(*sink), copied); !settings) {
      (void)sdk::failedBeforeStream(sink, copied, settings.error());
      return;
    }
    const auto transport = sdk::makeLibusbStreamTransport(bitoscope_usb_libusb_context(copied.device),
                                                          bitoscope_usb_libusb_handle(copied.device), copied.endpoint);
    (void)sdk::runUsbStream(sink, copied, *transport);
  } catch (...) {
    bitoscope_fail_capture(sink, "USB stream failed");
  }
}

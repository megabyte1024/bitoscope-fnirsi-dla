/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/wch_device_system.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "wch/wch_request.h"
#include "wch/wch_route.h"
#include "wch/wch_stream_transport.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr uint32_t kErrorPending = 997;
constexpr uint32_t kErrorCancelled = 995;
constexpr uint8_t kDirectionIn = 0x80;
constexpr int kLowSpeed = 0;
constexpr int kFullSpeed = 1;
constexpr int kHighSpeed = 2;
constexpr int kSuperSpeed = 3;
constexpr const char* kStrandedText = "WCH driver kept cancelled request: unplug and replug device";

std::string errorNameOf(uint32_t error) {
  switch (wchTransferStatusOf(error)) {
    case TransferStatus::stall:
      return std::format("{} (Windows error {})", kWchHaltedEndpointText, error);
    case TransferStatus::timedOut:
      return std::format("timed out (Windows error {})", error);
    case TransferStatus::cancelled:
      return std::format("request cancelled (Windows error {})", error);
    case TransferStatus::noDevice:
      return std::format("device gone (Windows error {})", error);
    default:
      break;
  }
  switch (wchCallKindOf(error)) {
    case UsbCallKind::busy:
      return std::format("device open in another program (Windows error {})", error);
    case UsbCallKind::access:
      return std::format("access denied (Windows error {})", error);
    default:
      return std::format("Windows error {}", error);
  }
}

UsbCall callOf(uint32_t error) { return UsbCall{wchCallKindOf(error), errorNameOf(error)}; }

struct Exchange {
  UsbCall call;
  uint32_t bytes = 0;
};

class WchDeviceSystem final : public UsbDeviceSystem, public WchRoute, private WchStreamHost {
 public:
  WchDeviceSystem(WchLister lister, std::unique_ptr<WchPort> port, WchOtherUser otherUser)
      : lister_(std::move(lister)), port_(std::move(port)), otherUser_(std::move(otherUser)) {}
  WchDeviceSystem(const WchDeviceSystem&) = delete;
  WchDeviceSystem& operator=(const WchDeviceSystem&) = delete;
  ~WchDeviceSystem() override { close(); }

  std::expected<std::vector<UsbDeviceRecord>, UsbCall> list() override {
    auto listed = lister_();
    if (!listed) return std::unexpected(listed.error());
    listed_ = std::move(*listed);
    std::vector<UsbDeviceRecord> records;
    records.reserve(listed_.size());
    for (const WchListedDevice& device : listed_) records.push_back(device.record);
    return records;
  }

  UsbCall open(size_t listed) override {
    if (listed >= listed_.size()) return UsbCall{UsbCallKind::notFound, "device not in WCH driver's list"};
    if (otherUser_) {
      if (std::optional<std::string> why = otherUser_()) return UsbCall{UsbCallKind::busy, std::move(*why)};
    }
    if (const uint32_t error = port_->open(listed_[listed].devicePath); error != 0) return callOf(error);
    open_ = true;
    driverTimeoutMs_.reset();
    if (const UsbCall exclusive = setting(wchSetExclusiveRequest(true)); exclusive.kind != UsbCallKind::ok) {
      close();
      return exclusive;
    }
    speed_ = listed_[listed].speed;
    return {};
  }

  UsbCall claim([[maybe_unused]] uint8_t interfaceNumber) override { return {}; }

  void release([[maybe_unused]] uint8_t interfaceNumber) override {}

  void close() override {
    if (!open_) return;
    port_->close();
    open_ = false;
  }

  UsbLinkSpeed speed() override { return speed_; }

  bool openedOnWch() const override { return true; }

  UsbCall bulk(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) override {
    const std::lock_guard lock(commandMutex_);
    return bulkLocked(endpoint, data, timeoutMs);
  }

  UsbCall control([[maybe_unused]] const UsbSetup& setup, [[maybe_unused]] std::span<uint8_t> data,
                  [[maybe_unused]] uint32_t timeoutMs) override {
    return UsbCall{UsbCallKind::notSupported, "WCH driver path has no control transfers"};
  }

  UsbCall clearHalt([[maybe_unused]] uint8_t endpoint) override {
    return UsbCall{UsbCallKind::notSupported, "WCH driver cannot clear endpoint halt"};
  }

  std::unique_ptr<UsbStreamTransport> streamTransport(uint8_t endpoint) override {
    return makeWchStreamTransport(*this, endpoint);
  }

 private:
  UsbCall beginStream(uint32_t driverTimeoutMs) override {
    const std::lock_guard lock(commandMutex_);
    if (stranded_) return UsbCall{UsbCallKind::other, kStrandedText};
    if (!open_) return UsbCall{UsbCallKind::other, "WCH device not open"};
    if (const UsbCall set = setting(wchSetTimeoutsRequest(driverTimeoutMs)); set.kind != UsbCallKind::ok) return set;
    driverTimeoutMs_ = driverTimeoutMs;
    streaming_ = true;
    return {};
  }

  void endStream() override {
    const std::lock_guard lock(commandMutex_);
    streaming_ = false;
  }

  UsbCall readNow(uint8_t endpoint, std::span<uint8_t> into, uint32_t timeoutMs) override {
    return bulk(endpoint, into, timeoutMs);
  }

  std::unique_ptr<WchStreamPort> streamPort(uint32_t slots) override {
    const std::lock_guard lock(commandMutex_);
    if (!open_ || stranded_) return nullptr;
    return port_->streamPort(slots);
  }

  UsbCall bulkLocked(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) {
    if (stranded_) return UsbCall{UsbCallKind::other, kStrandedText};
    if (!open_) return UsbCall{UsbCallKind::other, "WCH device not open"};
    if (data.size() > std::numeric_limits<uint32_t>::max()) return UsbCall{UsbCallKind::other, "transfer too long"};
    const auto size = static_cast<uint32_t>(data.size());
    if (!streaming_ && driverTimeoutMs_ != timeoutMs) {
      if (const UsbCall set = setting(wchSetTimeoutsRequest(timeoutMs)); set.kind != UsbCallKind::ok) return set;
      driverTimeoutMs_ = timeoutMs;
    }
    if ((endpoint & kDirectionIn) != 0) return read(endpoint, data, size, timeoutMs);
    return write(endpoint, data, size, timeoutMs);
  }

  UsbCall read(uint8_t endpoint, std::span<uint8_t> data, uint32_t size, uint32_t timeoutMs) {
    const auto header = wchReadHeader(endpoint, size);
    if (!header) return UsbCall{UsbCallKind::other, header.error()};
    reply_.assign(kWchHeaderBytes + data.size(), 0);
    const Exchange done = exchange(*header, limitFor(timeoutMs));
    if (done.call.kind != UsbCallKind::ok) return done.call;
    const auto moved = parseWchReadReply(std::span<const uint8_t>(reply_).first(std::min<size_t>(done.bytes, reply_.size())), size);
    if (!moved) return UsbCall{UsbCallKind::other, moved.error()};
    if (moved->empty()) return UsbCall{UsbCallKind::timeout, "timed out"};
    std::copy(moved->begin(), moved->end(), data.begin());
    UsbCall call;
    call.transferred = moved->size();
    return call;
  }

  UsbCall write(uint8_t endpoint, std::span<const uint8_t> data, uint32_t size, uint32_t timeoutMs) {
    const auto request = wchWriteRequest(endpoint, data);
    if (!request) return UsbCall{UsbCallKind::other, request.error()};
    reply_.assign(kWchHeaderBytes, 0);
    const Exchange done = exchange(*request, limitFor(timeoutMs));
    if (done.call.kind != UsbCallKind::ok) return done.call;
    const auto written = parseWchWriteReply(std::span<const uint8_t>(reply_).first(std::min<size_t>(done.bytes, reply_.size())), size);
    if (!written) return UsbCall{UsbCallKind::other, written.error()};
    UsbCall call;
    if (*written < size) call = UsbCall{UsbCallKind::timeout, "timed out"};
    call.transferred = *written;
    return call;
  }

  static uint32_t limitFor(uint32_t timeoutMs) {
    return timeoutMs > std::numeric_limits<uint32_t>::max() - kWchDriverMarginMs ? std::numeric_limits<uint32_t>::max()
                                                                                  : timeoutMs + kWchDriverMarginMs;
  }

  UsbCall setting(const std::array<uint8_t, kWchCommandBytes>& request) {
    reply_.assign(kWchCommandBytes, 0);
    return exchange(request, kWchCancelLimitMs).call;
  }

  Exchange exchange(std::span<const uint8_t> request, uint32_t limitMs) {
    request_.assign(request.begin(), request.end());
    if (reply_.size() < request_.size()) reply_.resize(request_.size(), 0);
    const uint32_t submitted = port_->submit(request_, reply_);
    if (submitted != 0 && submitted != kErrorPending) return {callOf(submitted)};
    WchCompletion completion = port_->wait(limitMs);
    if (!completion.done) {
      port_->cancel();
      completion = port_->wait(kWchCancelLimitMs);
      if (!completion.done) return strand();
      if (completion.error == kErrorCancelled) return {UsbCall{UsbCallKind::timeout, "timed out"}};
    }
    if (completion.error != 0) return {callOf(completion.error)};
    return {UsbCall{}, completion.bytes};
  }

  Exchange strand() {
    stranded_ = true;
    keepWchBufferForever(std::move(request_));
    keepWchBufferForever(std::move(reply_));
    request_ = {};
    reply_ = {};
    return {UsbCall{UsbCallKind::other, kStrandedText}};
  }

  WchLister lister_;
  std::unique_ptr<WchPort> port_;
  WchOtherUser otherUser_;
  std::vector<WchListedDevice> listed_;
  std::vector<uint8_t> request_;
  std::vector<uint8_t> reply_;
  UsbLinkSpeed speed_ = UsbLinkSpeed::unknown;
  std::optional<uint32_t> driverTimeoutMs_;
  std::mutex commandMutex_;
  bool streaming_ = false;
  bool open_ = false;
  bool stranded_ = false;
};

}

std::string wchErrorName(uint32_t windowsError) { return errorNameOf(windowsError); }

void keepWchBufferForever(std::vector<uint8_t> buffer) {
  static std::mutex guard;
  static std::vector<std::vector<uint8_t>> kept;
  std::lock_guard lock(guard);
  kept.push_back(std::move(buffer));
}

std::unique_ptr<UsbDeviceSystem> makeWchDeviceSystem(WchLister lister, std::unique_ptr<WchPort> port,
                                                     WchOtherUser otherUser) {
  return std::make_unique<WchDeviceSystem>(std::move(lister), std::move(port), std::move(otherUser));
}

UsbLinkSpeed usbLinkSpeedOfHub(bool superSpeedPlus, bool superSpeed, std::optional<int> legacySpeed) {
  if (superSpeedPlus) return UsbLinkSpeed::superPlus;
  if (superSpeed) return UsbLinkSpeed::super;
  if (!legacySpeed) return UsbLinkSpeed::unknown;
  switch (*legacySpeed) {
    case kLowSpeed:
      return UsbLinkSpeed::low;
    case kFullSpeed:
      return UsbLinkSpeed::full;
    case kHighSpeed:
      return UsbLinkSpeed::high;
    case kSuperSpeed:
      return UsbLinkSpeed::super;
    default:
      return UsbLinkSpeed::unknown;
  }
}

}

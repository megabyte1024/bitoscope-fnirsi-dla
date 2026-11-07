/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/wch_stream_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wch/wch_request.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr uint32_t kErrorPending = 997;
constexpr uint32_t kEndPollMs = 20;

class WchStreamTransport final : public UsbStreamTransport {
 public:
  WchStreamTransport(WchStreamHost& host, uint8_t endpoint, uint32_t cancelLimitMs)
      : host_(host), endpoint_(endpoint), cancelLimitMs_(cancelLimitMs) {}
  WchStreamTransport(const WchStreamTransport&) = delete;
  WchStreamTransport& operator=(const WchStreamTransport&) = delete;
  ~WchStreamTransport() override { end(); }

  uint64_t nowMs() override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }

  std::expected<uint64_t, UsbError> readNow(std::span<uint8_t> into, uint32_t timeoutMs) override {
    const UsbCall call = host_.readNow(endpoint_, into, timeoutMs);
    if (call.kind == UsbCallKind::ok || call.kind == UsbCallKind::timeout) return call.transferred;
    return std::unexpected(UsbError{call.kind == UsbCallKind::noDevice, call.name});
  }

  std::expected<void, std::string> allocate(uint32_t transfers, uint32_t transferBytes) override {
    if (port_ != nullptr) return std::unexpected(std::string("WCH stream already has its transfers"));
    if (transfers == 0) return std::unexpected(std::string("WCH stream needs at least one transfer"));
    const auto header = wchReadHeader(endpoint_, transferBytes);
    if (!header) return std::unexpected(header.error());
    try {
      slots_.resize(transfers);
      for (Slot& slot : slots_) slot.buffer.assign(kWchHeaderBytes + transferBytes, 0);
    } catch (const std::bad_alloc&) {
      slots_.clear();
      return std::unexpected("no memory for " + std::to_string(transfers) + " transfers of " +
                             std::to_string(transferBytes) + " bytes");
    }
    if (const UsbCall begun = host_.beginStream(kWchStreamDriverTimeoutMs); begun.kind != UsbCallKind::ok) {
      slots_.clear();
      return std::unexpected("WCH driver did not start stream: " + begun.name);
    }
    streaming_ = true;
    port_ = host_.streamPort(transfers);
    if (port_ == nullptr) {
      slots_.clear();
      return std::unexpected(std::string("WCH driver path gave no stream port"));
    }
    header_ = *header;
    transferBytes_ = transferBytes;
    return {};
  }

  std::span<const uint8_t> received(uint32_t transfer) const override {
    const Slot& slot = slots_.at(transfer);
    if (slot.abandoned) return {};
    return std::span<const uint8_t>(slot.buffer).subspan(kWchHeaderBytes, transferBytes_);
  }

  std::expected<void, UsbError> submit(uint32_t transfer) override {
    Slot& slot = slots_.at(transfer);
    if (slot.abandoned) return std::unexpected(UsbError{false, "transfer abandoned: WCH driver never finished its cancel"});
    const uint32_t submitted = port_->submit(transfer, header_, slot.buffer);
    if (submitted != 0 && submitted != kErrorPending) {
      return std::unexpected(UsbError{wchTransferStatusOf(submitted) == TransferStatus::noDevice, wchErrorName(submitted)});
    }
    slot.inFlight = true;
    slot.cancelledAtMs.reset();
    return {};
  }

  void cancel(uint32_t transfer) override {
    Slot& slot = slots_.at(transfer);
    if (!slot.inFlight || slot.cancelledAtMs) return;
    port_->cancel(transfer);
    slot.cancelledAtMs = nowMs();
  }

  void waitForCompletions(uint32_t timeoutMs, std::vector<TransferDone>& done) override {
    if (port_ == nullptr) return;
    const size_t before = done.size();
    abandonOverdue(done);
    if (done.size() > before) return;
    slotDone_.clear();
    port_->waitAny(waitFor(timeoutMs), slotDone_);
    for (const WchSlotDone& d : slotDone_) record(d, done);
    abandonOverdue(done);
  }

 private:
  struct Slot {
    std::vector<uint8_t> buffer;
    bool inFlight = false;
    bool abandoned = false;
    std::optional<uint64_t> cancelledAtMs;
  };

  uint32_t waitFor(uint32_t timeoutMs) {
    uint32_t wait = timeoutMs;
    const uint64_t now = nowMs();
    for (const Slot& slot : slots_) {
      if (!slot.inFlight || !slot.cancelledAtMs) continue;
      const uint64_t deadline = *slot.cancelledAtMs + cancelLimitMs_;
      const uint64_t left = deadline > now ? deadline - now : 0;
      wait = static_cast<uint32_t>(std::min<uint64_t>(wait, left));
    }
    return wait;
  }

  void record(const WchSlotDone& d, std::vector<TransferDone>& done) {
    if (d.slot >= slots_.size()) return;
    Slot& slot = slots_[d.slot];
    if (!slot.inFlight) return;
    slot.inFlight = false;
    slot.cancelledAtMs.reset();
    if (d.error != 0) {
      done.push_back(TransferDone{d.slot, wchTransferStatusOf(d.error), 0});
      return;
    }
    const auto reply = std::span<const uint8_t>(slot.buffer).first(std::min<size_t>(d.bytes, slot.buffer.size()));
    const auto data = parseWchReadReply(reply, transferBytes_);
    if (!data) {
      done.push_back(TransferDone{d.slot, TransferStatus::error, 0});
      return;
    }
    done.push_back(TransferDone{d.slot, TransferStatus::completed, data->size()});
  }

  void abandonOverdue(std::vector<TransferDone>& done) {
    const uint64_t now = nowMs();
    for (uint32_t i = 0; i < slots_.size(); ++i) {
      Slot& slot = slots_[i];
      if (!slot.inFlight || !slot.cancelledAtMs || now - *slot.cancelledAtMs < cancelLimitMs_) continue;
      abandon(i);
      done.push_back(TransferDone{i, TransferStatus::cancelled, 0});
    }
  }

  void abandon(uint32_t transfer) {
    Slot& slot = slots_[transfer];
    port_->abandon(transfer);
    keepWchBufferForever(std::move(slot.buffer));
    slot.buffer = {};
    slot.inFlight = false;
    slot.abandoned = true;
    slot.cancelledAtMs.reset();
  }

  bool anyInFlight() const {
    return std::ranges::any_of(slots_, [](const Slot& slot) { return slot.inFlight; });
  }

  void end() {
    if (port_ != nullptr) {
      for (uint32_t i = 0; i < slots_.size(); ++i) cancel(i);
      const uint64_t deadline = nowMs() + cancelLimitMs_ + 2 * kEndPollMs;
      std::vector<TransferDone> done;
      while (anyInFlight() && nowMs() < deadline) {
        done.clear();
        waitForCompletions(kEndPollMs, done);
      }
      for (uint32_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inFlight) abandon(i);
      }
      port_.reset();
    }
    if (streaming_) host_.endStream();
    streaming_ = false;
  }

  WchStreamHost& host_;
  uint8_t endpoint_;
  uint32_t cancelLimitMs_;
  std::unique_ptr<WchStreamPort> port_;
  std::vector<Slot> slots_;
  std::vector<WchSlotDone> slotDone_;
  std::array<uint8_t, kWchHeaderBytes> header_{};
  uint32_t transferBytes_ = 0;
  bool streaming_ = false;
};

}

std::unique_ptr<UsbStreamTransport> makeWchStreamTransport(WchStreamHost& host, uint8_t endpoint,
                                                           uint32_t cancelLimitMs) {
  return std::make_unique<WchStreamTransport>(host, endpoint, cancelLimitMs);
}

}

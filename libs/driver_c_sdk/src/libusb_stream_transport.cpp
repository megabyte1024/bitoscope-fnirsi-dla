/*
 * SPDX-License-Identifier: MIT
 */

#include "libusb_stream_transport.h"

#include <libusb.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr unsigned kNoTimeout = 0;
constexpr long kTeardownPumpUs = 100'000;

timeval timevalOf(uint32_t ms) {
  timeval tv{};
  tv.tv_sec = static_cast<decltype(tv.tv_sec)>(ms / 1000);
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000);
  return tv;
}

class LibusbStreamTransport final : public UsbStreamTransport {
 public:
  LibusbStreamTransport(libusb_context* context, libusb_device_handle* handle, uint8_t endpoint)
      : context_(context), handle_(handle), endpoint_(endpoint) {}

  LibusbStreamTransport(const LibusbStreamTransport&) = delete;
  LibusbStreamTransport& operator=(const LibusbStreamTransport&) = delete;

  ~LibusbStreamTransport() override {
    for (auto& t : transfers_) {
      if (inFlight(*t)) libusb_cancel_transfer(t->transfer);
    }
    // WHY: Free nothing while a transfer is in flight: libusb writes into its buffer and calls back until then.
    while (anyInFlight()) {
      timeval tv{0, kTeardownPumpUs};
      libusb_handle_events_timeout_completed(context_, &tv, nullptr);
    }
    for (auto& t : transfers_) release(*t);
  }

  uint64_t nowMs() override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }

  std::expected<uint64_t, UsbError> readNow(std::span<uint8_t> into, uint32_t timeoutMs) override {
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, endpoint_, into.data(), static_cast<int>(into.size()), &transferred,
                                        timeoutMs);
    return bulkReadAnswer(rc, transferred);
  }

  // WHY: RAW_IO is not set: need unverified; libusb_endpoint_set_raw_io() is in 1.0.30, not in 1.0.27.
  std::expected<void, std::string> allocate(uint32_t transfers, uint32_t transferBytes) override {
    done_.reserve(transfers);
    transfers_.reserve(transfers);
    for (uint32_t i = 0; i < transfers; ++i) {
      auto t = std::make_unique<Transfer>();
      t->owner = this;
      t->index = i;
      t->size = transferBytes;
      // WHY: libusb_dev_mem_alloc returning NULL is a normal fallback (no zero-copy memory there), not an error.
      t->buffer = libusb_dev_mem_alloc(handle_, transferBytes);
      t->deviceMemory = t->buffer != nullptr;
      if (t->buffer == nullptr) t->buffer = static_cast<unsigned char*>(std::malloc(transferBytes));
      if (t->buffer == nullptr) {
        return std::unexpected("no memory for " + std::to_string(transfers) + " transfers of " +
                               std::to_string(transferBytes) + " bytes");
      }
      t->transfer = libusb_alloc_transfer(0);
      if (t->transfer == nullptr) {
        release(*t);
        return std::unexpected(std::string("libusb_alloc_transfer failed"));
      }
      libusb_fill_bulk_transfer(t->transfer, handle_, endpoint_, t->buffer, static_cast<int>(transferBytes),
                                &LibusbStreamTransport::completed, t.get(), kNoTimeout);
      transfers_.push_back(std::move(t));
    }
    return {};
  }

  std::span<const uint8_t> received(uint32_t transfer) const override {
    const Transfer& t = *transfers_.at(transfer);
    return {t.buffer, t.size};
  }

  std::expected<void, UsbError> submit(uint32_t transfer) override {
    Transfer& t = *transfers_.at(transfer);
    // WHY: Mark it in flight BEFORE submitting: its completion can come at once, on another thread handling events.
    setInFlight(t, true);
    if (const int rc = libusb_submit_transfer(t.transfer); rc != LIBUSB_SUCCESS) {
      setInFlight(t, false);
      return std::unexpected(usbErrorOf(rc));
    }
    return {};
  }

  void cancel(uint32_t transfer) override { libusb_cancel_transfer(transfers_.at(transfer)->transfer); }

  void waitForCompletions(uint32_t timeoutMs, std::vector<TransferDone>& done) override {
    if (takeDone(done)) return;
    timeval tv = timevalOf(timeoutMs);
    libusb_handle_events_timeout_completed(context_, &tv, nullptr);
    (void)takeDone(done);
  }

 private:
  struct Transfer {
    LibusbStreamTransport* owner = nullptr;
    uint32_t index = 0;
    libusb_transfer* transfer = nullptr;
    unsigned char* buffer = nullptr;
    size_t size = 0;
    bool deviceMemory = false;
    bool inFlight = false;
  };

  // WHY: Only queue here: libusb calls back on whichever thread handles events; the stream is the capture thread's.
  static void LIBUSB_CALL completed(libusb_transfer* transfer) {
    auto* t = static_cast<Transfer*>(transfer->user_data);
    LibusbStreamTransport& owner = *t->owner;
    const std::lock_guard lock(owner.mutex_);
    t->inFlight = false;
    owner.done_.push_back(TransferDone{t->index, transferStatusOf(transfer->status),
                                       transfer->actual_length > 0 ? static_cast<uint64_t>(transfer->actual_length)
                                                                   : 0});
  }

  bool takeDone(std::vector<TransferDone>& done) {
    const std::lock_guard lock(mutex_);
    if (done_.empty()) return false;
    done.insert(done.end(), done_.begin(), done_.end());
    done_.clear();
    return true;
  }

  bool inFlight(const Transfer& t) {
    const std::lock_guard lock(mutex_);
    return t.inFlight;
  }

  void setInFlight(Transfer& t, bool value) {
    const std::lock_guard lock(mutex_);
    t.inFlight = value;
  }

  bool anyInFlight() {
    const std::lock_guard lock(mutex_);
    for (const auto& t : transfers_) {
      if (t->inFlight) return true;
    }
    return false;
  }

  void release(Transfer& t) {
    if (t.transfer != nullptr) {
      libusb_free_transfer(t.transfer);
      t.transfer = nullptr;
    }
    if (t.buffer != nullptr) {
      if (t.deviceMemory) {
        libusb_dev_mem_free(handle_, t.buffer, t.size);
      } else {
        std::free(t.buffer);
      }
      t.buffer = nullptr;
    }
  }

  libusb_context* context_;
  libusb_device_handle* handle_;
  uint8_t endpoint_;
  std::vector<std::unique_ptr<Transfer>> transfers_;
  std::mutex mutex_;
  std::vector<TransferDone> done_;
};

}

TransferStatus transferStatusOf(int libusbTransferStatus) {
  switch (libusbTransferStatus) {
    case LIBUSB_TRANSFER_COMPLETED:
      return TransferStatus::completed;
    case LIBUSB_TRANSFER_ERROR:
      return TransferStatus::error;
    case LIBUSB_TRANSFER_TIMED_OUT:
      return TransferStatus::timedOut;
    case LIBUSB_TRANSFER_CANCELLED:
      return TransferStatus::cancelled;
    case LIBUSB_TRANSFER_STALL:
      return TransferStatus::stall;
    case LIBUSB_TRANSFER_NO_DEVICE:
      return TransferStatus::noDevice;
    case LIBUSB_TRANSFER_OVERFLOW:
      return TransferStatus::overflow;
    default:
      return TransferStatus::unknown;
  }
}

UsbError usbErrorOf(int libusbError) {
  UsbError error{libusbError == LIBUSB_ERROR_NO_DEVICE, libusb_error_name(libusbError)};
#if defined(__linux__)
  if (libusbError == LIBUSB_ERROR_NO_MEM) {
    error.name += " (Linux limits usbfs memory: see /sys/module/usbcore/parameters/usbfs_memory_mb)";
  }
#endif
  return error;
}

std::expected<uint64_t, UsbError> bulkReadAnswer(int libusbResult, int transferred) {
  if (libusbResult != LIBUSB_SUCCESS && libusbResult != LIBUSB_ERROR_TIMEOUT) {
    return std::unexpected(usbErrorOf(libusbResult));
  }
  return transferred > 0 ? static_cast<uint64_t>(transferred) : 0;
}

std::unique_ptr<UsbStreamTransport> makeLibusbStreamTransport(void* context, void* handle, uint8_t endpoint) {
  return std::make_unique<LibusbStreamTransport>(static_cast<libusb_context*>(context),
                                                 static_cast<libusb_device_handle*>(handle), endpoint);
}

}

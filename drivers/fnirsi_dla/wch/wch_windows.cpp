/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/wch_device_system.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bitoscope_plugin_common.h"
#include "libusb_bus.h"
#include "wch/manufacturer_app.h"
#include "wch/windows_usb_drivers.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr DWORD kWchIoctl = 0x223CDC;
constexpr DWORD kShareNone = 0;
constexpr uint32_t kNoRequestPending = ERROR_INVALID_FUNCTION;

struct PendingIo {
  OVERLAPPED overlapped{};
};

void keepForever(PendingIo* io) {
  static std::mutex guard;
  static std::vector<PendingIo*> kept;
  std::lock_guard lock(guard);
  kept.push_back(io);
}

class WindowsWchStreamPort final : public WchStreamPort {
 public:
  WindowsWchStreamPort(HANDLE handle, uint32_t slots) : handle_(handle), slots_(slots) {
    for (Slot& slot : slots_) {
      slot.io = new PendingIo;
      slot.io->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
  }
  WindowsWchStreamPort(const WindowsWchStreamPort&) = delete;
  WindowsWchStreamPort& operator=(const WindowsWchStreamPort&) = delete;
  ~WindowsWchStreamPort() override {
    for (Slot& slot : slots_) {
      if (slot.io == nullptr) continue;
      if (slot.pending) {
        keepForever(slot.io);
        continue;
      }
      if (slot.io->overlapped.hEvent != nullptr) CloseHandle(slot.io->overlapped.hEvent);
      delete slot.io;
    }
  }

  bool ready() const {
    return std::ranges::all_of(slots_, [](const Slot& s) { return s.io->overlapped.hEvent != nullptr; });
  }

  uint32_t submit(uint32_t slot, std::span<const uint8_t> request, std::span<uint8_t> reply) override {
    Slot& s = slots_.at(slot);
    if (s.io == nullptr) return ERROR_INVALID_HANDLE;
    const HANDLE event = s.io->overlapped.hEvent;
    s.io->overlapped = OVERLAPPED{};
    s.io->overlapped.hEvent = event;
    ResetEvent(event);
    s.pending = true;
    s.sequence = ++sequence_;
    if (DeviceIoControl(handle_, kWchIoctl, const_cast<uint8_t*>(request.data()), static_cast<DWORD>(request.size()),
                        reply.data(), static_cast<DWORD>(reply.size()), nullptr, &s.io->overlapped)) {
      return 0;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) s.pending = false;
    return error;
  }

  void waitAny(uint32_t timeoutMs, std::vector<WchSlotDone>& done) override {
    if (collect(done)) return;
    std::vector<uint32_t> pending;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].pending) pending.push_back(i);
    }
    if (pending.empty()) return;
    std::ranges::sort(pending, [&](uint32_t a, uint32_t b) { return slots_[a].sequence < slots_[b].sequence; });
    if (pending.size() > MAXIMUM_WAIT_OBJECTS) pending.resize(MAXIMUM_WAIT_OBJECTS);
    std::vector<HANDLE> events;
    events.reserve(pending.size());
    for (const uint32_t i : pending) events.push_back(slots_[i].io->overlapped.hEvent);
    (void)WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, timeoutMs);
    (void)collect(done);
  }

  void cancel(uint32_t slot) override {
    Slot& s = slots_.at(slot);
    if (s.pending) CancelIoEx(handle_, &s.io->overlapped);
  }

  void abandon(uint32_t slot) override {
    Slot& s = slots_.at(slot);
    if (s.io == nullptr) return;
    if (s.pending) {
      keepForever(s.io);
    } else {
      if (s.io->overlapped.hEvent != nullptr) CloseHandle(s.io->overlapped.hEvent);
      delete s.io;
    }
    s.io = nullptr;
    s.pending = false;
  }

 private:
  struct Slot {
    PendingIo* io = nullptr;
    bool pending = false;
    uint64_t sequence = 0;
  };

  bool collect(std::vector<WchSlotDone>& done) {
    bool any = false;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
      Slot& s = slots_[i];
      if (!s.pending || !HasOverlappedIoCompleted(&s.io->overlapped)) continue;
      DWORD moved = 0;
      const BOOL ok = GetOverlappedResult(handle_, &s.io->overlapped, &moved, FALSE);
      const DWORD error = ok ? 0 : GetLastError();
      if (!ok && error == ERROR_IO_INCOMPLETE) continue;
      s.pending = false;
      done.push_back(WchSlotDone{i, error, moved});
      any = true;
    }
    return any;
  }

  HANDLE handle_;
  std::vector<Slot> slots_;
  uint64_t sequence_ = 0;
};

class WindowsWchPort final : public WchPort {
 public:
  WindowsWchPort() = default;
  WindowsWchPort(const WindowsWchPort&) = delete;
  WindowsWchPort& operator=(const WindowsWchPort&) = delete;
  ~WindowsWchPort() override { close(); }

  uint32_t open(const std::string& devicePath) override {
    close();
    const HANDLE handle = CreateFileA(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE, kShareNone, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
      const DWORD error = GetLastError();
      CloseHandle(handle);
      return error;
    }
    handle_ = handle;
    io_ = new PendingIo;
    io_->overlapped.hEvent = event;
    return 0;
  }

  void close() override {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    if (pending_) {
      keepForever(io_);
    } else {
      CloseHandle(io_->overlapped.hEvent);
      delete io_;
    }
    io_ = nullptr;
    pending_ = false;
  }

  uint32_t submit(std::span<const uint8_t> request, std::span<uint8_t> reply) override {
    if (handle_ == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    const HANDLE event = io_->overlapped.hEvent;
    io_->overlapped = OVERLAPPED{};
    io_->overlapped.hEvent = event;
    ResetEvent(event);
    pending_ = true;
    if (DeviceIoControl(handle_, kWchIoctl, const_cast<uint8_t*>(request.data()), static_cast<DWORD>(request.size()),
                        reply.data(), static_cast<DWORD>(reply.size()), nullptr, &io_->overlapped)) {
      return 0;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) pending_ = false;
    return error;
  }

  WchCompletion wait(uint32_t timeoutMs) override {
    if (!pending_) return WchCompletion{true, kNoRequestPending, 0};
    if (WaitForSingleObject(io_->overlapped.hEvent, timeoutMs) != WAIT_OBJECT_0) return {};
    DWORD moved = 0;
    const BOOL ok = GetOverlappedResult(handle_, &io_->overlapped, &moved, FALSE);
    const DWORD error = ok ? 0 : GetLastError();
    pending_ = false;
    return WchCompletion{true, error, moved};
  }

  void cancel() override {
    if (pending_) CancelIoEx(handle_, &io_->overlapped);
  }

  std::unique_ptr<WchStreamPort> streamPort(uint32_t slots) override {
    if (handle_ == INVALID_HANDLE_VALUE || slots == 0) return nullptr;
    auto port = std::make_unique<WindowsWchStreamPort>(handle_, slots);
    if (!port->ready()) return nullptr;
    return port;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  PendingIo* io_ = nullptr;
  bool pending_ = false;
};

std::expected<std::vector<WchListedDevice>, UsbCall> listWchDevices() {
  auto bus = makeLibusbBus();
  if (!bus) return std::unexpected(UsbCall{UsbCallKind::other, bus.error()});
  const auto records = (*bus)->enumerate();
  if (!records) return std::unexpected(UsbCall{UsbCallKind::other, records.error()});
  const auto windows = listWindowsUsbDevices();
  if (!windows) return std::unexpected(UsbCall{UsbCallKind::other, windows.error()});
  std::vector<WchListedDevice> listed;
  for (const UsbDeviceRecord& record : *records) {
    const UsbPathMatch match = matchWindowsUsbDevice(record, *windows);
    if (match.kind != UsbPathMatchKind::found) continue;
    if (usbPathKindOfService(match.device->service) != UsbPathKind::wch) continue;
    if (match.device->wchDevicePath.empty()) continue;
    listed.push_back(WchListedDevice{record, match.device->wchDevicePath, windowsUsbLinkSpeed(*match.device)});
  }
  return listed;
}

std::string asciiProgramName(const wchar_t* name) {
  std::string out;
  for (; *name != L'\0'; ++name) out.push_back(*name < 0x80 ? static_cast<char>(*name) : '?');
  return out;
}

std::vector<std::string> runningProgramNames() {
  std::vector<std::string> names;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return names;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    names.push_back(asciiProgramName(entry.szExeFile));
  }
  CloseHandle(snapshot);
  return names;
}

std::optional<std::string> manufacturerAppInUse() {
  static std::atomic<bool> reported{false};
  std::optional<std::string> why = manufacturerAppReason(runningProgramNames());
  if (!why) {
    reported = false;
  } else if (!reported.exchange(true)) {
    bitoscope_log(BITOSCOPE_LOG_WARNING, "%s", why->c_str());
  }
  return why;
}

}

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeWindowsWchDeviceSystem() {
  return makeWchDeviceSystem(listWchDevices, std::make_unique<WindowsWchPort>(), manufacturerAppInUse);
}

std::optional<ExclusiveOpenResult> tryWindowsWchExclusiveOpen(const FoundUsbDevice& device) {
  auto system = makeWindowsWchDeviceSystem();
  if (!system) return std::nullopt;
  const auto listed = (*system)->list();
  if (!listed) return std::nullopt;
  const UsbPlace place{device.vendorId, device.productId, device.interfaceNumber, device.bus, device.address,
                       device.ports};
  const std::optional<size_t> at = listedAt(*listed, place);
  if (!at) return std::nullopt;
  const UsbCall opened = (*system)->open(*at);
  if (opened.kind == UsbCallKind::ok) {
    (*system)->close();
    return ExclusiveOpenResult{ExclusiveOpenOutcome::opened, {}};
  }
  if (opened.kind == UsbCallKind::busy || opened.kind == UsbCallKind::access) {
    return ExclusiveOpenResult{ExclusiveOpenOutcome::held, opened.name};
  }
  return ExclusiveOpenResult{ExclusiveOpenOutcome::failed, opened.name};
}

}

#else

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeWindowsWchDeviceSystem() {
  return std::unexpected(UsbCall{UsbCallKind::notSupported, "WCH driver exists only on Windows"});
}

std::optional<ExclusiveOpenResult> tryWindowsWchExclusiveOpen([[maybe_unused]] const FoundUsbDevice& device) {
  return std::nullopt;
}

}

#endif

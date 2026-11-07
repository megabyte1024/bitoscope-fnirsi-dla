/*
 * SPDX-License-Identifier: MIT
 */

#include "device_watch.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// WHY: windows.h first: cfgmgr32.h and the GUID headers need its types.
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>

#include <mutex>
#include <utility>

namespace bitoscope::plugin_c_sdk {

namespace {

class WindowsUsbChangeSource final : public DeviceChangeSource {
 public:
  ~WindowsUsbChangeSource() override { stop(); }

  std::expected<void, std::string> start(std::vector<UsbId> ids, std::function<void()> changed,
                                         [[maybe_unused]] std::function<void(std::string_view why)> failed) override {
    {
      std::lock_guard lock(mutex_);
      ids_ = std::move(ids);
      changed_ = std::move(changed);
    }
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof filter;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
    const CONFIGRET result = CM_Register_Notification(&filter, this, &WindowsUsbChangeSource::callback, &handle_);
    if (result != CR_SUCCESS) {
      handle_ = nullptr;
      return std::unexpected("Windows refused USB device notifications (CM_Register_Notification returned " +
                             std::to_string(result) + ")");
    }
    return {};
  }

  void stop() override {
    // WHY: CM_Unregister_Notification waits for a running callback, so it must never be called from one (deadlock).
    if (handle_ != nullptr) {
      CM_Unregister_Notification(handle_);
      handle_ = nullptr;
    }
    std::lock_guard lock(mutex_);
    changed_ = nullptr;
  }

 private:
  static DWORD CALLBACK callback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                 PCM_NOTIFY_EVENT_DATA data, DWORD) {
    if (action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL && action != CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
      return ERROR_SUCCESS;
    }
    if (data == nullptr || data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
    auto* self = static_cast<WindowsUsbChangeSource*>(context);
    const std::wstring_view link = data->u.DeviceInterface.SymbolicLink;
    std::function<void()> changed;
    bool matches = false;
    {
      std::lock_guard lock(self->mutex_);
      matches = symbolicLinkMatches(link, self->ids_);
      if (matches) changed = self->changed_;
    }
    std::string narrow;
    for (const wchar_t c : link) narrow += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
    logLine(ipc::LogLevel::debug, std::string("USB device ") +
                                      (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ? "arrived: " : "removed: ") +
                                      narrow + (matches ? " (watched)" : " (not watched)"));
    if (changed) changed();
    return ERROR_SUCCESS;
  }

  std::mutex mutex_;
  std::vector<UsbId> ids_;
  std::function<void()> changed_;
  HCMNOTIFICATION handle_ = nullptr;
};

}

std::unique_ptr<DeviceChangeSource> makeSystemDeviceChangeSource() {
  return std::make_unique<WindowsUsbChangeSource>();
}

}

#elif defined(__linux__)

// WHY: Read the kernel's own uevent broadcast (multicast group 1), not udev's monitor socket: needs no libudev.

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <linux/netlink.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "linux_uevent_reader.h"

namespace bitoscope::plugin_c_sdk {

namespace {

class LinuxUsbChangeSource final : public DeviceChangeSource {
 public:
  ~LinuxUsbChangeSource() override { stop(); }

  std::expected<void, std::string> start(std::vector<UsbId> ids, std::function<void()> changed,
                                         std::function<void(std::string_view why)> failed) override {
    ids_ = std::move(ids);
    changed_ = std::move(changed);
    failed_ = std::move(failed);
    socket_fd_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (socket_fd_ < 0) {
      return std::unexpected(std::string("could not open NETLINK_KOBJECT_UEVENT socket: ") + std::strerror(errno));
    }
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;
    addr.nl_pid = 0;
    if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      const std::string message = std::string("could not bind uevent socket: ") + std::strerror(errno);
      ::close(socket_fd_);
      socket_fd_ = -1;
      return std::unexpected(message);
    }
    if (::pipe2(stop_pipe_, O_CLOEXEC) != 0) {
      const std::string message = std::string("could not create stop pipe: ") + std::strerror(errno);
      ::close(socket_fd_);
      socket_fd_ = -1;
      return std::unexpected(message);
    }
    thread_ = std::thread(&LinuxUsbChangeSource::run, this);
    return {};
  }

  void stop() override {
    if (stop_pipe_[1] >= 0) {
      const char byte = 0;
      [[maybe_unused]] const ssize_t written = ::write(stop_pipe_[1], &byte, 1);
    }
    if (thread_.joinable()) thread_.join();
    if (socket_fd_ >= 0) {
      ::close(socket_fd_);
      socket_fd_ = -1;
    }
    for (int& fd : stop_pipe_) {
      if (fd >= 0) ::close(fd);
      fd = -1;
    }
    std::lock_guard lock(mutex_);
    changed_ = nullptr;
    failed_ = nullptr;
  }

 private:
  // WHY: `changed` and `failed` are taken under the lock each time, as stop() clears them.
  void run() {
    readUevents(
        socket_fd_, stop_pipe_[0], ids_,
        [this] {
          std::function<void()> changed;
          {
            std::lock_guard lock(mutex_);
            changed = changed_;
          }
          if (changed) changed();
        },
        [this](std::string_view why) {
          std::function<void(std::string_view why)> failed;
          {
            std::lock_guard lock(mutex_);
            failed = failed_;
          }
          if (failed) failed(why);
        });
  }

  std::vector<UsbId> ids_;  // WHY: set once in start(), read only by run(): safe without a lock
  std::mutex mutex_;
  std::function<void()> changed_;
  std::function<void(std::string_view why)> failed_;
  int socket_fd_ = -1;
  int stop_pipe_[2] = {-1, -1};
  std::thread thread_;
};

}

std::unique_ptr<DeviceChangeSource> makeSystemDeviceChangeSource() { return std::make_unique<LinuxUsbChangeSource>(); }

}

#else

namespace bitoscope::plugin_c_sdk {

std::unique_ptr<DeviceChangeSource> makeSystemDeviceChangeSource() { return nullptr; }

}

#endif

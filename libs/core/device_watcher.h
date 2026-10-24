/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: Callbacks run on the reader thread and must not call stop(): stop() joins that thread.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ipc_messages.h"
#include "plugin_link.h"
#include "plugin_process.h"

namespace bitoscope::host {

struct WatchEnd {
  std::string message;
  std::string cause;
};

struct DriverConnection {
  std::string networkAddress;
  std::optional<uint16_t> networkPort;
  std::string serialPort;
};

std::vector<std::string> driverArguments(std::vector<std::string> modeArguments, const DriverConnection& connection);
std::vector<std::string> watchArguments(const DriverConnection& connection);

struct DeviceWatcherEvents {
  std::function<void(const ipc::DeviceInfo& device)> onDeviceAppeared;
  std::function<void(const ipc::DeviceInfo& device)> onDeviceChanged;
  std::function<void(std::string_view deviceId, std::string_view reason)> onDeviceLost;
  std::function<void(const ipc::LogEvent& log)> onLog;
  std::function<void(const WatchEnd& end)> onEnded;
};

class DeviceWatcher {
 public:
  static std::expected<std::unique_ptr<DeviceWatcher>, std::string> start(std::unique_ptr<PluginLink> link,
                                                                          DeviceWatcherEvents events,
                                                                          std::chrono::milliseconds timeout);
  static std::expected<std::unique_ptr<DeviceWatcher>, std::string> start(const std::filesystem::path& executable,
                                                                          std::string name,
                                                                          StderrLineFn onStderrLine,
                                                                          DeviceWatcherEvents events,
                                                                          std::chrono::milliseconds timeout,
                                                                          const DriverConnection& connection = {});

  ~DeviceWatcher();
  DeviceWatcher(const DeviceWatcher&) = delete;
  DeviceWatcher& operator=(const DeviceWatcher&) = delete;

  void stop();

  std::expected<void, std::string> listAgain();

  std::vector<ipc::DeviceInfo> devices() const;

 private:
  DeviceWatcher(std::unique_ptr<PluginLink> link, DeviceWatcherEvents events, std::vector<ipc::DeviceInfo> devices);
  void readLoop();

  std::unique_ptr<PluginLink> link_;
  DeviceWatcherEvents events_;
  mutable std::mutex mutex_;
  std::vector<ipc::DeviceInfo> devices_;
  bool stopping_ = false;
  std::mutex writeMutex_;
  std::thread reader_;
};

}

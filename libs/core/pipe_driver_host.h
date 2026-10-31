/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "device_watcher.h"
#include "facet_client.h"
#include "ipc_messages.h"
#include "opened_device.h"
#include "plugin_connection.h"
#include "plugin_process.h"

namespace bitoscope::host {

std::expected<ipc::DeviceList, std::string> listDevices(const std::filesystem::path& executable,
                                                        const StderrLineFn& onStderrLine = {},
                                                        std::chrono::milliseconds timeout = kDefaultRequestTimeout,
                                                        std::string name = {},
                                                        const DriverConnection& connection = {});

struct PipeDriverHostOptions {
  std::filesystem::path driverDirectory;
  StderrLineFn onStderrLine;
  std::function<void(std::string_view driverDeviceId, const ipc::LogEvent&)> onLog;
  std::function<void(std::string_view driverDeviceId, const ipc::FacetEvent&)> onFacetEvent;
  std::function<void(std::string_view driverDeviceId, std::string_view reason)> onClosed;
  std::function<void(std::string_view driverDeviceId, std::string_view notice)> onNotice;
  std::function<void(std::string_view driverDeviceId, std::string_view reason)> onDeviceLost;
  std::function<void(std::string_view driverDeviceId, const ipc::OpenProgress&)> onOpenProgress;
  std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout;
  std::chrono::milliseconds startTimeout{0};
};

class PipeDriverHost final : public FacetTransport {
 public:
  explicit PipeDriverHost(PipeDriverHostOptions options);
  ~PipeDriverHost() override;
  PipeDriverHost(const PipeDriverHost&) = delete;
  PipeDriverHost& operator=(const PipeDriverHost&) = delete;

  std::vector<std::string> drivers() const;

  std::expected<ipc::DeviceList, std::string> listDevices(std::string_view driver,
                                                          const DriverConnection& connection = {}) const;

  std::expected<std::unique_ptr<DeviceWatcher>, std::string> watch(std::string_view driver, DeviceWatcherEvents events,
                                                                   const DriverConnection& connection = {}) const;

  std::expected<std::string, OpenRefusal> open(std::string_view driver, std::string_view deviceId,
                                                HandshakeOffer offer = {},
                                               const DriverConnection& connection = {});

  bool close(std::string_view driverDeviceId);

  std::shared_ptr<PluginConnection> connection(std::string_view driverDeviceId) const;

  std::expected<std::string, std::string> sendCommand(std::string_view driverDeviceId, std::string_view facet,
                                                      std::string_view payload) override;
  std::expected<void, PropertiesRefusal> setProperties(std::string_view driverDeviceId, std::string_view facet,
                                                       std::span<const ipc::props::PropertyValue> values) override;

 private:
  std::expected<std::filesystem::path, std::string> executableOf(std::string_view driver) const;
  std::chrono::milliseconds startTimeout() const;

  PipeDriverHostOptions options_;
  std::mutex openMutex_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<PluginConnection>, std::less<>> connections_;
};

}

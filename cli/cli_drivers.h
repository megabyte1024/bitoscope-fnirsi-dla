/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: Several devices found and none named: refuse and list them, never pick one, because choosing starts hardware.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipc_la_acquisition.h"
#include "ipc_messages.h"
#include "pipe_driver_host.h"
#include "driver_device.h"

namespace bitoscope::cli {

std::filesystem::path defaultDriverDirectory(const std::filesystem::path& executable);

std::filesystem::path currentExecutable(std::string_view argv0);

std::filesystem::path driverDirectory(std::string_view driver_dir, const std::filesystem::path& executable);

std::expected<std::string, std::string> selectDriver(const std::vector<std::string>& drivers,
                                                     std::string_view requested,
                                                     const std::filesystem::path& directory);

std::expected<ipc::DeviceInfo, std::string> selectDevice(const ipc::DeviceList& list, std::string_view driver,
                                                         std::string_view requested);

std::string describeFacet(const ipc::FacetInfo& facet);

std::string describeDevices(std::string_view driver, const ipc::DeviceList& list);

std::string describeListedDevice(std::string_view driver, const ipc::DeviceInfo& device);

std::string describeOpenProgress(std::string_view driverDeviceId, const ipc::OpenProgress& progress);

std::string describeDeviceProperties(std::string_view driver, const ipc::DeviceInfo& device);

std::string formatLogLine(std::string_view driver_device_id, const ipc::LogEvent& event);
bool isErrorLog(const ipc::LogEvent& event);

class CaptureEvents {
 public:
  void onFacetEvent(const ipc::FacetEvent& event);
  std::optional<ipc::la::CaptureEnded> ended(uint64_t capture_id) const;
  std::optional<ipc::la::CaptureEnded> latest() const;

 private:
  mutable std::mutex mutex_;
  std::map<uint64_t, ipc::la::CaptureEnded> ended_;
  std::optional<ipc::la::CaptureEnded> latest_;
};

std::string describeCaptureEnd(const ipc::la::CaptureEnded& ended);

std::expected<host::pipe::DriverDevice, std::string> openDevice(host::PipeDriverHost& host, std::string_view driver_name,
                                                                std::string_view device,
                                                                const std::filesystem::path& directory);

}

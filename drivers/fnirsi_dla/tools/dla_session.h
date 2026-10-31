/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ipc_properties.h"
#include "la_pipe_client.h"
#include "pipe_driver_host.h"
#include "driver_device.h"
#include "siggen_pipe_client.h"

namespace fnirsi::tools {

inline constexpr std::string_view kDlaDriver = "fnirsi_dla";

struct DlaSessionOptions {
  std::filesystem::path driver_dir;
  std::string device_id;
  std::chrono::milliseconds start_timeout{30'000};
  std::chrono::milliseconds request_timeout{15'000};
};

class DlaSession {
 public:
  static std::expected<std::unique_ptr<DlaSession>, std::string> open(const DlaSessionOptions& options);

  ~DlaSession();
  DlaSession(const DlaSession&) = delete;
  DlaSession& operator=(const DlaSession&) = delete;

  const std::string& driverDeviceId() const { return device_.driver_device_id; }
  const std::string& deviceName() const { return device_.device.name; }

  bitoscope::host::pipe::LaAcquisitionClient& la() { return *la_; }
  bitoscope::host::pipe::SiggenClient& siggen() { return *siggen_; }

  std::expected<void, std::string> setProperties(std::string_view facet,
                                                 std::span<const bitoscope::ipc::props::PropertyValue> values);

  std::optional<std::string> closedReason() const;

 private:
  DlaSession() = default;

  std::shared_ptr<std::atomic<bool>> closing_ = std::make_shared<std::atomic<bool>>(false);
  std::unique_ptr<bitoscope::host::PipeDriverHost> host_;
  bitoscope::host::pipe::DriverDevice device_;
  std::optional<bitoscope::host::pipe::LaAcquisitionClient> la_;
  std::optional<bitoscope::host::pipe::SiggenClient> siggen_;
};

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bitoscope_driver.h"
#include "device_source.h"
#include "ipc_messages.h"

namespace bitoscope::plugin_c_sdk {

using SteadyTime = std::chrono::steady_clock::time_point;

inline constexpr std::chrono::seconds kFirstOtherRetry{2};
inline constexpr std::chrono::seconds kLongestOtherRetry{60};
inline constexpr std::chrono::seconds kFirmwareGracePeriod{5};

class IdentityCache {
 public:
  IdentityCache(const bitoscope_driver& driver, const bitoscope_connection& connection, DeviceSource& source);

  std::vector<ipc::DeviceInfo> list(std::span<const SourceDevice> found, SteadyTime now);

 private:
  enum class Failure { none, busy, noPermission, firmwareLoading, other };

  struct Known {
    std::string appearanceKey;
    std::optional<ipc::DeviceInfo> identified;
    Failure failure = Failure::none;
    std::string failureDetail;
    std::chrono::seconds otherRetryDelay{0};
    SteadyTime otherRetryAt{};
    bool checkFoundUnavailable = false;
    bool notSupported = false;
    std::optional<SteadyTime> absentSince;
    ipc::DeviceInfo listed;
  };

  bool identifyDue(const Known& known, SteadyTime now) const;
  void identify(Known& known, const SourceDevice& found, SteadyTime now);
  ipc::DeviceInfo genericEntry(const SourceDevice& found) const;
  ipc::DeviceInfo unavailableEntry(const Known& known, const SourceDevice& found, ipc::UnavailableReason reason,
                                   const std::string& detail) const;

  const bitoscope_driver& driver_;
  const bitoscope_connection& connection_;
  DeviceSource& source_;
  std::map<std::string, Known> known_;
};

}

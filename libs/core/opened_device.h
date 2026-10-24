/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

#include "ipc_messages.h"

namespace bitoscope::host {

struct OpenedDevice {
  ipc::DeviceInfo device;
  std::string pluginVersion{};
  bool operator==(const OpenedDevice&) const = default;
};

struct OpenRefusal {
  std::string message;
  ipc::UnavailableReason unavailable = ipc::UnavailableReason::none;
};

}

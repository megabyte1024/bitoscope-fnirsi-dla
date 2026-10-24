/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

#include "ipc_messages.h"

namespace bitoscope::host::pipe {

struct PluginDevice {
  std::string plugin_id;
  ipc::DeviceInfo device;
};

}

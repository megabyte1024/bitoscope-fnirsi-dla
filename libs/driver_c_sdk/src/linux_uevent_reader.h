/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

void readUevents(int socketFd, int stopFd, const std::vector<UsbId>& ids, const std::function<void()>& changed,
                 const std::function<void(std::string_view why)>& failed);

}

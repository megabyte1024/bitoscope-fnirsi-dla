/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <span>
#include <string>
#include <vector>

#include "ipc_properties.h"
#include "sink_facts.h"

namespace bitoscope::plugin_c_sdk {

std::expected<void, std::string> addUsbStreamProperties(std::vector<ipc::props::PropertyNode>& tree);

UsbStreamChoice usbStreamChoice(std::span<const ipc::props::PropertyValue> values);

}

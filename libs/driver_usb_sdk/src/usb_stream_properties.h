/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "ipc_properties.h"

namespace bitoscope::plugin_c_sdk {

struct UsbStreamChoice {
  uint32_t transfers = 0;
  uint32_t transferKib = 0;
};

std::optional<UsbStreamChoice> usbStreamChoice(std::span<const ipc::props::PropertyValue> values);

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>

#include "bitoscope_driver_usb_cpp.h"
#include "usb_inventory.h"

namespace fnirsi::wch {

class WchRoute {
 public:
  virtual ~WchRoute() = default;
  virtual bool openedOnWch() const = 0;
};

bool openedOnWch(const bitoscope::plugin::UsbDevice& device);

std::optional<bitoscope::plugin_c_sdk::ExclusiveOpenResult> namedWchAnswer(
    std::optional<bitoscope::plugin_c_sdk::ExclusiveOpenResult> answer);

std::optional<bitoscope::plugin_c_sdk::ExclusiveOpenResult> wchSecondOpen(
    const bitoscope::plugin_c_sdk::FoundUsbDevice& device);

void useWchDriverWhereBound();

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <string>
#include <vector>

#include "bitoscope_driver_usb_cpp.h"
#include "usb_device.h"

namespace fnirsi {

std::expected<std::vector<bitoscope::plugin::FoundDevice>, std::string> listDlaOnUsb(
    const bitoscope::plugin_c_sdk::UsbDeviceSystemFactory& systems);

}

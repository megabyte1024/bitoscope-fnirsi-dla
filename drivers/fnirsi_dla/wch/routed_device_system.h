/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <memory>

#include "usb_device.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

std::unique_ptr<UsbDeviceSystem> makeRoutedDeviceSystem(std::unique_ptr<UsbDeviceSystem> primary,
                                                        UsbDeviceSystemFactory wch);

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeDlaDeviceSystem();

}

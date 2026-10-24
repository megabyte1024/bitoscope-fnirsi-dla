/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <memory>
#include <string>

#include "usb_device.h"
#include "usb_inventory.h"

namespace bitoscope::plugin_c_sdk {

UsbCallKind usbCallKindOf(int libusbResult);

UsbCall usbCallOf(int libusbResult);

UsbLinkSpeed usbLinkSpeedOf(int libusbSpeed);

ExclusiveOpenResult exclusiveOpenResultOf(int libusbOpenResult);

std::expected<std::unique_ptr<UsbBus>, std::string> makeLibusbBus();

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeLibusbDeviceSystem();

}

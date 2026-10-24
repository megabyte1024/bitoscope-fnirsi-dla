/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bitoscope_driver.h"
#include "device_source.h"
#include "device_watch.h"
#include "libusb_bus.h"
#include "plugin_model.h"
#include "usb_checks.h"
#include "usb_inventory.h"

namespace bitoscope::plugin_c_sdk {

using UsbInventory =
    std::function<std::expected<std::vector<FoundUsbDevice>, std::string>(std::span<const DeclaredUsbId> declared)>;
using UsbHeldCheck = std::function<UsbCheck(const FoundUsbDevice& device)>;
using UsbBusFactory = std::function<std::expected<std::unique_ptr<UsbBus>, std::string>()>;

struct UsbSourceSeams {
  UsbInventory devices;
  UsbHeldCheck check;
  UsbBusFactory bus = makeLibusbBus;
  ChangeSourceFactory changes;
};

std::unique_ptr<DeviceSource> makeUsbSource(std::span<const bitoscope_usb_id> declared, UsbSourceSeams seams);

SourceDevice usbSourceDevice(const FoundUsbDevice& device, std::span<const bitoscope_usb_id> declared);

std::unique_ptr<ChangeNotices> usbChangeNotices(std::unique_ptr<DeviceChangeSource> system, std::vector<UsbId> ids);

}

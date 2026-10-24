/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>

#include "bitoscope_driver.h"
#include "usb_device.h"

struct bitoscope_usb_device {
  std::unique_ptr<bitoscope::plugin_c_sdk::OpenedUsbDevice> opened;
};

namespace bitoscope::plugin_c_sdk {

class ScopedUsbDeviceSystems {
 public:
  explicit ScopedUsbDeviceSystems(UsbDeviceSystemFactory factory);
  ScopedUsbDeviceSystems(const ScopedUsbDeviceSystems&) = delete;
  ScopedUsbDeviceSystems& operator=(const ScopedUsbDeviceSystems&) = delete;
  ~ScopedUsbDeviceSystems();

 private:
  UsbDeviceSystemFactory previous_;
};

bitoscope_usb_device* adoptedUsbDevice(std::unique_ptr<OpenedUsbDevice> opened);

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <string>
#include <vector>

#include "bitoscope_driver.h"
#include "device_source.h"
#include "frame_output.h"
#include "ipc_messages.h"
#include "plugin_model.h"

struct bitoscope_found_devices {
  std::vector<bitoscope::plugin_c_sdk::SourceDevice> devices;
};

struct bitoscope_progress {
  bitoscope::plugin_c_sdk::FrameOutput* output = nullptr;
};

namespace bitoscope::plugin_c_sdk {

class GivenDevice {
 public:
  GivenDevice(const SourceDevice& device, const bitoscope_connection& connection);
  GivenDevice(const GivenDevice&) = delete;
  GivenDevice& operator=(const GivenDevice&) = delete;

  const bitoscope_found_device& get() const { return device_; }

 private:
  bitoscope_usb_location usb_{};
  bitoscope_serial_location serial_{};
  bitoscope_network_location network_{};
  bitoscope_found_device device_{};
};

std::expected<SourceDevice, std::string> sourceDeviceOf(const bitoscope_found_device& device);

enum class DriverCall { identify, open };

std::string functionName(DriverCall call);

struct Attempt {
  int status = BITOSCOPE_ERROR;
  std::string text;
};

std::string genericNameOf(const bitoscope_driver& driver, const SourceDevice& device);

class DeviceDescription {
 public:
  explicit DeviceDescription(const std::string& id);
  DeviceDescription(const DeviceDescription&) = delete;
  DeviceDescription& operator=(const DeviceDescription&) = delete;

  bitoscope_device& device() { return *devices_.devices.front(); }
  ipc::DeviceInfo info(const std::string& genericName) const;

 private:
  bitoscope_devices devices_;
};

Attempt callIdentify(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device);

Attempt callOpen(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device,
                 FrameOutput& output, void*& session);

ipc::UnavailableReason reasonOf(int status);

ipc::UnavailableReason reasonOfCheck(Availability availability);

}

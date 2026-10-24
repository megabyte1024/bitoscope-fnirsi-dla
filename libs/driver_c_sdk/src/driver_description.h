/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitoscope_driver.h"
#include "device_source.h"
#include "device_watch.h"
#include "driver_calls.h"
#include "frame_output.h"
#include "ipc_byte_stream.h"
#include "ipc_protocol_version.h"
#include "usb_source.h"

namespace bitoscope::plugin_c_sdk {

inline constexpr std::chrono::milliseconds kBusyRetryDelay{200};
inline constexpr std::chrono::milliseconds kReenumerationPoll{100};
inline constexpr std::chrono::milliseconds kReenumerationLimit{4000};

using RetrySleep = std::function<void(std::chrono::milliseconds delay)>;
using DeviceSourceFactory = std::function<std::unique_ptr<DeviceSource>(const bitoscope_driver& driver,
                                                                        const bitoscope_connection& connection)>;

struct DriverRunOptions {
  uint32_t minProtocolMajor = ipc::kProtocolMajor;
  uint32_t maxProtocolMajor = ipc::kProtocolMajor;
  UsbInventory usbDevices;
  UsbHeldCheck usbCheck;
  UsbBusFactory usbBus = makeLibusbBus;
  DeviceSourceFactory deviceSource;
  std::chrono::milliseconds busyRetryDelay = kBusyRetryDelay;
  RetrySleep sleep;
  WatchClock* clock = nullptr;
  ChangeSourceFactory changeSource;
  WatchTiming watchTiming;
};

std::expected<void, std::string> checkDriverDescription(const bitoscope_driver& driver);

std::unique_ptr<DeviceSource> deviceSourceFor(const bitoscope_driver& driver, const bitoscope_connection& connection,
                                              const DriverRunOptions& options);

std::expected<void, std::string> reportProgress(bitoscope_progress& progress, int code, std::string_view detail);

int runDriver(std::span<const std::string> args, ipc::ByteSource& in, ipc::ByteSink& out,
              const bitoscope_driver& driver, const DriverRunOptions& options = {});

}

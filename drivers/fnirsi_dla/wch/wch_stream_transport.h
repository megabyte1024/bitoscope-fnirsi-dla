/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "usb_device.h"
#include "usb_stream_transport.h"
#include "wch/wch_device_system.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

inline constexpr uint32_t kWchStreamDriverTimeoutMs = 300000;

class WchStreamHost {
 public:
  virtual ~WchStreamHost() = default;
  virtual UsbCall beginStream(uint32_t driverTimeoutMs) = 0;
  virtual void endStream() = 0;
  virtual UsbCall readNow(uint8_t endpoint, std::span<uint8_t> into, uint32_t timeoutMs) = 0;
  virtual std::unique_ptr<WchStreamPort> streamPort(uint32_t slots) = 0;
};

std::unique_ptr<UsbStreamTransport> makeWchStreamTransport(WchStreamHost& host, uint8_t endpoint,
                                                           uint32_t cancelLimitMs = kWchCancelLimitMs);

}

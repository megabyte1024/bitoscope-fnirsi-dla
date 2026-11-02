/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>

#include "bitoscope_driver_cpp.h"
#include "update_transport.h"

namespace fnirsi {

struct ReopenTiming {
  std::chrono::milliseconds give_up{15'000};
  std::chrono::milliseconds retry{250};
};

using ReopenSleep = std::function<void(std::chrono::milliseconds)>;

class UsbUpdateTransport final : public UpdateTransport {
 public:
  UsbUpdateTransport(bitoscope::plugin::UsbDevice device, bitoscope::plugin::FoundDevice unit, ReopenTiming timing = {},
                     ReopenSleep sleep = nullptr);

  TransferResult writeCommand(std::span<const uint8_t> packet, uint32_t timeout_ms) override;
  TransferResult readReply(std::span<uint8_t> reply, uint32_t timeout_ms) override;
  void clearHalts() override;
  void close() override;
  std::expected<void, std::string> reopen() override;

 private:
  bitoscope::plugin::UsbDevice device_;
  bitoscope::plugin::FoundDevice unit_;
  ReopenTiming timing_;
  ReopenSleep sleep_;
};

}

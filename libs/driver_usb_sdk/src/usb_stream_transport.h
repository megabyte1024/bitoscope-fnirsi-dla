/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace bitoscope::plugin_c_sdk {

enum class TransferStatus { completed, error, timedOut, cancelled, stall, noDevice, overflow, unknown };

struct UsbError {
  bool noDevice = false;
  std::string name;
};

struct TransferDone {
  uint32_t transfer = 0;
  TransferStatus status = TransferStatus::completed;
  uint64_t bytes = 0;
};

class UsbStreamTransport {
 public:
  virtual ~UsbStreamTransport() = default;

  virtual uint64_t nowMs() = 0;
  virtual std::expected<uint64_t, UsbError> readNow(std::span<uint8_t> into, uint32_t timeoutMs) = 0;
  virtual std::expected<void, std::string> allocate(uint32_t transfers, uint32_t transferBytes) = 0;
  virtual std::span<const uint8_t> received(uint32_t transfer) const = 0;
  virtual std::expected<void, UsbError> submit(uint32_t transfer) = 0;
  virtual void cancel(uint32_t transfer) = 0;
  virtual void waitForCompletions(uint32_t timeoutMs, std::vector<TransferDone>& done) = 0;
};

}

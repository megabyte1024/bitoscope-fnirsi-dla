/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace fnirsi {

enum class TransferStatus { kOk, kTimeout, kStall, kGone, kError };

struct TransferResult {
  TransferStatus status = TransferStatus::kOk;
  size_t transferred = 0;
  std::string detail;
};

class UpdateTransport {
 public:
  virtual ~UpdateTransport() = default;
  virtual TransferResult writeCommand(std::span<const uint8_t> packet, uint32_t timeout_ms) = 0;
  virtual TransferResult readReply(std::span<uint8_t> reply, uint32_t timeout_ms) = 0;
  virtual void clearHalts() = 0;
  virtual void close() = 0;
  virtual std::expected<void, std::string> reopen() = 0;
};

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "fnirsi_protocol.h"

namespace fnirsi {

class FramedCommandSender {
 public:
  virtual ~FramedCommandSender() = default;

  [[nodiscard]] virtual std::expected<void, std::string> sendFramed(
      FramedCommand cmd, std::span<const uint8_t> payload) = 0;
};

}

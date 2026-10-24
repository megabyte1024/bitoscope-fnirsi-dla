/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace bitoscope::ipc {

class ByteSource {
 public:
  virtual ~ByteSource() = default;

  virtual std::expected<std::size_t, std::string> read(std::span<uint8_t> into) = 0;
};

class ByteSink {
 public:
  virtual ~ByteSink() = default;

  virtual std::expected<void, std::string> write(std::span<const uint8_t> bytes) = 0;
};

}

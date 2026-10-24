/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ipc_byte_stream.h"

namespace bitoscope::ipc {

class MemoryByteSource final : public ByteSource {
 public:
  explicit MemoryByteSource(std::vector<std::vector<uint8_t>> reads,
                            std::optional<std::string> failWith = std::nullopt);

  std::expected<std::size_t, std::string> read(std::span<uint8_t> into) override;

  std::size_t readCalls() const { return readCalls_; }

 private:
  std::vector<std::vector<uint8_t>> reads_;
  std::optional<std::string> failWith_;
  std::size_t piece_ = 0;
  std::size_t offset_ = 0;
  std::size_t readCalls_ = 0;
};

class MemoryByteSink final : public ByteSink {
 public:
  explicit MemoryByteSink(std::optional<std::string> failWith = std::nullopt);

  std::expected<void, std::string> write(std::span<const uint8_t> bytes) override;

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::size_t writeCalls() const { return writeCalls_; }

 private:
  std::optional<std::string> failWith_;
  std::vector<uint8_t> bytes_;
  std::size_t writeCalls_ = 0;
};

}

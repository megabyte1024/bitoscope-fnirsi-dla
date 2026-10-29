/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ipc_byte_stream.h"

namespace bitoscope::ipc {

inline constexpr std::size_t kFrameHeaderBytes = 4;

inline constexpr uint32_t kDefaultMaxFrameBytes = 1u << 20;

enum class FrameErrorKind {
  tooLarge,
  truncated,
  empty,
  streamFailed
};

struct FrameError {
  FrameErrorKind kind;
  std::string message;
};

std::array<uint8_t, kFrameHeaderBytes> frameHeader(uint32_t bodyBytes);

class Frame {
 public:
  explicit Frame(std::size_t bodyBytes);

  std::span<uint8_t> body() { return {storage_.get() + kFrameHeaderBytes, bodyBytes_}; }
  std::span<const uint8_t> body() const { return {storage_.get() + kFrameHeaderBytes, bodyBytes_}; }
  std::span<const uint8_t> bytes() const { return {storage_.get(), kFrameHeaderBytes + bodyBytes_}; }

 private:
  std::unique_ptr<uint8_t[]> storage_;
  std::size_t bodyBytes_;
};

class FrameReader {
 public:
  explicit FrameReader(ByteSource& source, uint32_t maxFrameBytes = kDefaultMaxFrameBytes);

  std::expected<std::optional<std::vector<uint8_t>>, FrameError> next();

 private:
  std::expected<bool, FrameError> fill();

  ByteSource& source_;
  uint32_t maxFrameBytes_;
  std::vector<uint8_t> buffered_;
  std::size_t consumed_ = 0;
  std::optional<FrameError> failed_;
  bool ended_ = false;
};

class FrameWriter {
 public:
  explicit FrameWriter(ByteSink& sink, uint32_t maxFrameBytes = kDefaultMaxFrameBytes);

  std::expected<void, FrameError> write(std::span<const uint8_t> message);
  std::expected<void, FrameError> write(const Frame& frame);

 private:
  std::expected<void, FrameError> checkSize(std::size_t messageBytes) const;

  ByteSink& sink_;
  uint32_t maxFrameBytes_;
};

}

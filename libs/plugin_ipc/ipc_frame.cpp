/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_frame.h"

#include <algorithm>
#include <string>
#include <utility>

namespace bitoscope::ipc {

namespace {

constexpr std::size_t kReadChunkBytes = 64 * 1024;

uint32_t lengthAt(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}

std::array<uint8_t, kFrameHeaderBytes> frameHeader(uint32_t bodyBytes) {
  return {static_cast<uint8_t>(bodyBytes), static_cast<uint8_t>(bodyBytes >> 8), static_cast<uint8_t>(bodyBytes >> 16),
          static_cast<uint8_t>(bodyBytes >> 24)};
}

FrameReader::FrameReader(ByteSource& source, uint32_t maxFrameBytes)
    : source_(source), maxFrameBytes_(maxFrameBytes) {}

std::expected<bool, FrameError> FrameReader::fill() {
  if (consumed_ > 0) {
    buffered_.erase(buffered_.begin(), buffered_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
  }
  const std::size_t had = buffered_.size();
  buffered_.resize(had + kReadChunkBytes);
  auto got = source_.read(std::span<uint8_t>(buffered_.data() + had, kReadChunkBytes));
  if (!got) {
    buffered_.resize(had);
    return std::unexpected(FrameError{FrameErrorKind::streamFailed, "reading stream failed: " + got.error()});
  }
  buffered_.resize(had + (std::min)(*got, kReadChunkBytes));
  return *got > 0;
}

std::expected<std::optional<std::vector<uint8_t>>, FrameError> FrameReader::next() {
  if (failed_) return std::unexpected(*failed_);
  if (ended_) return std::optional<std::vector<uint8_t>>{};

  auto fail = [this](FrameError error) -> std::expected<std::optional<std::vector<uint8_t>>, FrameError> {
    failed_ = error;
    return std::unexpected(std::move(error));
  };
  auto available = [this] { return buffered_.size() - consumed_; };

  while (available() < kFrameHeaderBytes) {
    auto more = fill();
    if (!more) return fail(more.error());
    if (!*more) {
      if (available() == 0) {
        ended_ = true;
        return std::optional<std::vector<uint8_t>>{};
      }
      return fail({FrameErrorKind::truncated, "stream ended inside frame's length (" +
                                                  std::to_string(available()) + " of " +
                                                  std::to_string(kFrameHeaderBytes) + " bytes)"});
    }
  }
  const uint32_t length = lengthAt(buffered_.data() + consumed_);
  if (length > maxFrameBytes_) {
    return fail({FrameErrorKind::tooLarge, "frame of " + std::to_string(length) + " bytes is over limit of " +
                                               std::to_string(maxFrameBytes_) + " bytes"});
  }

  while (available() < kFrameHeaderBytes + length) {
    auto more = fill();
    if (!more) return fail(more.error());
    if (!*more) {
      return fail({FrameErrorKind::truncated, "stream ended inside frame (" +
                                                  std::to_string(available() - kFrameHeaderBytes) + " of " +
                                                  std::to_string(length) + " bytes)"});
    }
  }
  const auto begin = buffered_.begin() + static_cast<std::ptrdiff_t>(consumed_ + kFrameHeaderBytes);
  std::vector<uint8_t> body(begin, begin + static_cast<std::ptrdiff_t>(length));
  consumed_ += kFrameHeaderBytes + length;
  return std::optional<std::vector<uint8_t>>{std::move(body)};
}

FrameWriter::FrameWriter(ByteSink& sink, uint32_t maxFrameBytes) : sink_(sink), maxFrameBytes_(maxFrameBytes) {}

std::expected<void, FrameError> FrameWriter::write(std::span<const uint8_t> message) {
  if (message.empty()) return std::unexpected(FrameError{FrameErrorKind::empty, "empty message is not frame"});
  if (message.size() > maxFrameBytes_) {
    return std::unexpected(FrameError{FrameErrorKind::tooLarge, "message of " + std::to_string(message.size()) +
                                                                    " bytes is over frame limit of " +
                                                                    std::to_string(maxFrameBytes_) + " bytes"});
  }
  const auto header = frameHeader(static_cast<uint32_t>(message.size()));
  std::vector<uint8_t> frame;
  frame.reserve(kFrameHeaderBytes + message.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), message.begin(), message.end());
  if (auto wrote = sink_.write(frame); !wrote) {
    return std::unexpected(FrameError{FrameErrorKind::streamFailed, "writing stream failed: " + wrote.error()});
  }
  return {};
}

}

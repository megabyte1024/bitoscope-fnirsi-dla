/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_memory_stream.h"

#include <algorithm>
#include <utility>

namespace bitoscope::ipc {

MemoryByteSource::MemoryByteSource(std::vector<std::vector<uint8_t>> reads, std::optional<std::string> failWith)
    : reads_(std::move(reads)), failWith_(std::move(failWith)) {}

std::expected<std::size_t, std::string> MemoryByteSource::read(std::span<uint8_t> into) {
  ++readCalls_;
  // WHY: An empty scripted piece must be skipped: it would otherwise read as end of stream.
  while (piece_ < reads_.size() && offset_ == reads_[piece_].size()) {
    ++piece_;
    offset_ = 0;
  }
  if (piece_ == reads_.size()) {
    if (failWith_) return std::unexpected(*failWith_);
    return std::size_t{0};
  }
  const std::vector<uint8_t>& piece = reads_[piece_];
  const std::size_t n = (std::min)(into.size(), piece.size() - offset_);
  std::copy_n(piece.begin() + static_cast<std::ptrdiff_t>(offset_), n, into.begin());
  offset_ += n;
  return n;
}

MemoryByteSink::MemoryByteSink(std::optional<std::string> failWith) : failWith_(std::move(failWith)) {}

std::expected<void, std::string> MemoryByteSink::write(std::span<const uint8_t> bytes) {
  ++writeCalls_;
  if (failWith_) return std::unexpected(*failWith_);
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return {};
}

}

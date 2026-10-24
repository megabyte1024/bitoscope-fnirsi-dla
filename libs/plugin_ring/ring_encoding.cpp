/*
 * SPDX-License-Identifier: MIT
 */

#include "ring_encoding.h"

#include <limits>

namespace bitoscope::plugin_ring {

namespace {

std::expected<uint64_t, std::string> decodePayloadBytes(uint32_t encoding, uint32_t num_channels,
                                                        uint64_t num_samples) {
  if (num_channels == 0) {
    return std::unexpected(std::string("chunk needs at least one channel"));
  }
  if (encoding == kRingEncodingSkip) {
    return (uint64_t{num_channels} + 7) / 8;
  }
  const uint64_t words = num_samples / 64 + (num_samples % 64 != 0 ? 1 : 0);
  if (words > std::numeric_limits<uint64_t>::max() / 8 / num_channels) {
    return std::unexpected(std::to_string(num_samples) + " samples of " + std::to_string(num_channels) +
                           " channels do not fit 64-bit byte count");
  }
  return words * 8 * num_channels;
}

std::expected<uint64_t, std::string> capturePayloadBytes(uint32_t encoding, uint32_t num_channels,
                                                         uint64_t num_samples) {
  if (auto ok = checkCaptureGeometry(encoding, num_channels); !ok) {
    return std::unexpected(ok.error());
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  const uint64_t count = encoding == kRingEncodingChannelMajor ? num_samples / 8 + (num_samples % 8 != 0 ? 1 : 0)
                                                               : num_samples;
  const uint64_t width = encoding == kRingEncodingChannelMajor ? num_channels : (uint64_t{num_channels} + 7) / 8;
  if (count != 0 && width > kMax / count) {
    return std::unexpected(std::to_string(num_samples) + " samples of " + std::to_string(num_channels) +
                           " channels do not fit 64-bit byte count");
  }
  return count * width;
}

}

std::expected<void, std::string> checkCaptureGeometry(uint32_t encoding, uint32_t num_channels) {
  if (encoding != kRingEncodingChannelMajor && encoding != kRingEncodingSampleMajor) {
    return std::unexpected("encoding " + std::to_string(encoding) +
                           " is neither channel-major (1) nor sample-major (2)");
  }
  if (num_channels == 0) {
    return std::unexpected(std::string("chunk needs at least one channel"));
  }
  return {};
}

std::expected<uint64_t, std::string> chunkPayloadBytes(uint32_t encoding, uint32_t num_channels,
                                                       uint64_t num_samples) {
  if (encoding == kRingEncodingChannelPlanes || encoding == kRingEncodingSkip) {
    return decodePayloadBytes(encoding, num_channels, num_samples);
  }
  return capturePayloadBytes(encoding, num_channels, num_samples);
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace bitoscope::plugin_ring {

// WHY: Values duplicate SampleEncoding's on purpose: this library does not include the host's header.
inline constexpr uint32_t kRingEncodingChannelMajor = 1;
inline constexpr uint32_t kRingEncodingSampleMajor = 2;
inline constexpr uint32_t kRingEncodingChannelPlanes = 3;
inline constexpr uint32_t kRingEncodingSkip = 4;

std::expected<void, std::string> checkCaptureGeometry(uint32_t encoding, uint32_t num_channels);

std::expected<uint64_t, std::string> chunkPayloadBytes(uint32_t encoding, uint32_t num_channels,
                                                       uint64_t num_samples);

}

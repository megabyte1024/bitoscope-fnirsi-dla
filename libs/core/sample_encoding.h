/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace bitoscope::host {

// WHY: Values are the wire's: a ring's chunk record carries them as they are. Never renumber.
enum class SampleEncoding : uint32_t {
  Unspecified = 0,
  ChannelMajorPacked = 1,
  SampleMajorPacked = 2,
  ChannelPlanes = 3,
  SkipRecord = 4,
};

inline size_t expectedPayloadBytes(SampleEncoding encoding, uint32_t num_channels, uint64_t num_samples) {
  if (encoding == SampleEncoding::ChannelMajorPacked) {
    const uint64_t bytes_per_channel = (num_samples + 7) / 8;
    return static_cast<size_t>(bytes_per_channel * num_channels);
  }
  if (encoding != SampleEncoding::SampleMajorPacked) {
    if (encoding == SampleEncoding::ChannelPlanes) {
      const uint64_t plane_bytes = (num_samples + 63) / 64 * 8;
      return static_cast<size_t>(plane_bytes * num_channels);
    }
    if (encoding == SampleEncoding::SkipRecord) {
      return static_cast<size_t>((static_cast<uint64_t>(num_channels) + 7) / 8);
    }
  }
  const uint64_t bytes_per_sample = (static_cast<uint64_t>(num_channels) + 7) / 8;
  return static_cast<size_t>(num_samples * bytes_per_sample);
}

}

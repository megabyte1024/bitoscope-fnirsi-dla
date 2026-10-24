/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "bulk_ring.h"
#include "pwm_check.h"

namespace fnirsi::tools::synth {

struct ChannelSignal {
  bool initial_level = false;
  std::vector<uint64_t> edges;
  uint64_t length = 0;

  bool operator==(const ChannelSignal&) const = default;
};

ChannelSignal pwmSignal(const PwmShape& shape, uint64_t rise_units, uint64_t length);

ChannelSignal flatSignal(bool level, uint64_t length);

ChannelSignal noiseSignal(uint64_t seed, uint64_t length);

ChannelSignal removeSamples(const ChannelSignal& signal, uint64_t at, uint64_t count);

ChannelSignal duplicateSamples(const ChannelSignal& signal, uint64_t at, uint64_t count);

bool levelAt(const ChannelSignal& signal, uint64_t sample);

std::vector<uint8_t> pack(std::span<const ChannelSignal> channels);

struct Chunk {
  bitoscope::bulk_ring::ChunkHeader header;
  std::vector<uint8_t> payload;
};

std::vector<Chunk> splitIntoChunks(std::span<const uint8_t> stream, uint32_t num_channels,
                                   std::span<const uint64_t> rounds_per_chunk, uint64_t sample_rate_milli_hz,
                                   uint64_t capture_id);

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "pwm_synth.h"

#include <algorithm>

namespace fnirsi::tools::synth {

namespace {

uint64_t ceilDiv(uint64_t a, uint64_t b) { return a / b + (a % b != 0 ? 1 : 0); }

uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}

ChannelSignal pwmSignal(const PwmShape& shape, uint64_t rise_units, uint64_t length) {
  ChannelSignal out;
  out.length = length;
  const uint64_t p = shape.period_num;
  const uint64_t h = shape.high_num;
  const bool fall_before_zero = rise_units + h <= p;
  out.initial_level = rise_units == 0 || !fall_before_zero;
  const auto add = [&](uint64_t crossing_units) {
    const uint64_t sample = ceilDiv(crossing_units, shape.den);
    if (crossing_units > 0 && sample < length) out.edges.push_back(sample);
    return sample < length;
  };
  if (!fall_before_zero && !add(rise_units + h - p)) return out;
  for (uint64_t m = 0;; ++m) {
    if (!add(rise_units + m * p)) break;
    if (!add(rise_units + h + m * p)) break;
  }
  return out;
}

ChannelSignal flatSignal(bool level, uint64_t length) { return ChannelSignal{level, {}, length}; }

ChannelSignal noiseSignal(uint64_t seed, uint64_t length) {
  ChannelSignal out;
  out.length = length;
  uint64_t state = seed;
  uint64_t bits = 0;
  bool last = false;
  for (uint64_t s = 0; s < length; ++s) {
    if (s % 64 == 0) bits = splitmix64(state);
    const bool level = ((bits >> (s % 64)) & 1) != 0;
    if (s == 0) {
      out.initial_level = level;
    } else if (level != last) {
      out.edges.push_back(s);
    }
    last = level;
  }
  return out;
}

bool levelAt(const ChannelSignal& signal, uint64_t sample) {
  const auto toggles = std::upper_bound(signal.edges.begin(), signal.edges.end(), sample) - signal.edges.begin();
  return signal.initial_level != (toggles % 2 != 0);
}

ChannelSignal removeSamples(const ChannelSignal& signal, uint64_t at, uint64_t count) {
  ChannelSignal out;
  out.length = signal.length - count;
  if (at == 0) {
    out.initial_level = out.length > 0 && levelAt(signal, count);
  } else {
    out.initial_level = signal.initial_level;
  }
  for (const uint64_t e : signal.edges) {
    if (e < at) out.edges.push_back(e);
  }
  if (at > 0 && at < out.length && levelAt(signal, at - 1) != levelAt(signal, at + count)) out.edges.push_back(at);
  for (const uint64_t e : signal.edges) {
    if (e > at + count) out.edges.push_back(e - count);
  }
  return out;
}

ChannelSignal duplicateSamples(const ChannelSignal& signal, uint64_t at, uint64_t count) {
  ChannelSignal out;
  out.length = signal.length + count;
  out.initial_level = signal.initial_level;
  for (const uint64_t e : signal.edges) {
    if (e < at + count) out.edges.push_back(e);
  }
  if (count > 0 && levelAt(signal, at + count - 1) != levelAt(signal, at)) out.edges.push_back(at + count);
  for (const uint64_t e : signal.edges) {
    if (e > at) out.edges.push_back(e + count);
  }
  return out;
}

std::vector<uint8_t> pack(std::span<const ChannelSignal> channels) {
  if (channels.empty()) return {};
  uint64_t length = channels[0].length;
  for (const auto& c : channels) length = std::min(length, c.length);
  const uint64_t bytes = length / 8;
  const size_t n = channels.size();
  std::vector<uint8_t> out(static_cast<size_t>(bytes * n));
  for (size_t c = 0; c < n; ++c) {
    const auto& edges = channels[c].edges;
    bool level = channels[c].initial_level;
    size_t next = 0;
    for (uint64_t j = 0; j < bytes; ++j) {
      const uint64_t first = j * 8;
      uint8_t byte = 0;
      if (next == edges.size() || edges[next] >= first + 8) {
        byte = level ? 0xFF : 0x00;
      } else {
        for (uint64_t bit = 0; bit < 8; ++bit) {
          if (next < edges.size() && edges[next] == first + bit) {
            level = !level;
            ++next;
          }
          if (level) byte = static_cast<uint8_t>(byte | (0x01u << bit));
        }
      }
      out[static_cast<size_t>(j * n + c)] = byte;
    }
  }
  return out;
}

std::vector<Chunk> splitIntoChunks(std::span<const uint8_t> stream, uint32_t num_channels,
                                   std::span<const uint64_t> rounds_per_chunk, uint64_t sample_rate_milli_hz,
                                   uint64_t capture_id) {
  std::vector<Chunk> out;
  if (num_channels == 0 || rounds_per_chunk.empty()) return out;
  const uint64_t rounds = stream.size() / num_channels;
  uint64_t round = 0;
  for (size_t i = 0; round < rounds; ++i) {
    const uint64_t take = std::min<uint64_t>(std::max<uint64_t>(rounds_per_chunk[i % rounds_per_chunk.size()], 1),
                                             rounds - round);
    Chunk chunk;
    const auto begin = stream.begin() + static_cast<std::ptrdiff_t>(round * num_channels);
    chunk.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(take * num_channels));
    chunk.header.first_sample_index = round * 8;
    chunk.header.num_samples = take * 8;
    chunk.header.num_channels = num_channels;
    chunk.header.encoding = kChannelMajorPackedEncoding;
    chunk.header.sample_rate_milli_hz = sample_rate_milli_hz;
    chunk.header.capture_id = capture_id;
    chunk.header.payload_bytes = chunk.payload.size();
    out.push_back(std::move(chunk));
    round += take;
  }
  return out;
}

}

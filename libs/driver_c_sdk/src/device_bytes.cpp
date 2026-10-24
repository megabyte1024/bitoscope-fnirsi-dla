/*
 * SPDX-License-Identifier: MIT
 */

#include "device_bytes.h"

#include <bit>
#include <cstring>

namespace bitoscope::plugin_c_sdk {

std::string maskText(uint32_t mask) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  do {
    out.insert(out.begin(), kDigits[mask & 0xF]);
    mask >>= 4;
  } while (mask != 0);
  return "0x" + out;
}

std::string bytesText(uint64_t bytes) { return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes"); }

namespace {

using Tables = std::array<std::array<uint32_t, 256>, 4>;

constexpr uint32_t kMaskBits = 32;

uint32_t everyChannel(uint32_t channels) {
  return channels >= kMaskBits ? 0xFFFFFFFFu : (uint32_t{1} << channels) - 1;
}

uint32_t carriedMaskOf(const bitoscope_device_bytes& bytes, uint32_t device_channels) {
  return bytes.channel_mask == 0 ? everyChannel(device_channels) : bytes.channel_mask;
}

uint32_t minimalStride(bool channel_major, uint32_t carried) {
  const auto count = static_cast<uint32_t>(std::popcount(carried));
  return channel_major ? count : (count + 7) / 8;
}

template <uint32_t kOut>
void storeLow(uint8_t* to, uint32_t value) {
  if constexpr (kOut == 1) {
    to[0] = static_cast<uint8_t>(value);
  } else if constexpr (kOut == 2) {
    const auto low = static_cast<uint16_t>(value);
    std::memcpy(to, &low, 2);
  } else if constexpr (kOut == 3) {
    const auto low = static_cast<uint16_t>(value);
    std::memcpy(to, &low, 2);
    to[2] = static_cast<uint8_t>(value >> 16);
  } else {
    std::memcpy(to, &value, 4);
  }
}

template <uint32_t kOut>
void keepLowest(const uint8_t* in, uint64_t samples, uint32_t stride, uint8_t top, uint8_t* out) {
  // WHY: Store 4 bytes for 3 only with a stride of 4 or more: in place they would overwrite the next sample.
  if constexpr (kOut == 3) {
    if (stride >= 4 && samples > 0) {
      const uint32_t keep = 0x0000FFFFu | (uint32_t{top} << 16);
      for (uint64_t s = 0; s + 1 < samples; ++s) {
        uint32_t value;
        std::memcpy(&value, in + s * stride, 4);
        value &= keep;
        std::memcpy(out + s * 3, &value, 4);
      }
      in += (samples - 1) * stride;
      out += (samples - 1) * 3;
      samples = 1;
    }
  }
  for (uint64_t s = 0; s < samples; ++s) {
    // WHY: Copy through a local: in place the first sample's source and destination are the same bytes.
    uint8_t sample[4];
    std::memcpy(sample, in + s * stride, kOut);
    sample[kOut - 1] &= top;
    std::memcpy(out + s * kOut, sample, kOut);
  }
}

template <uint32_t kReached, uint32_t kOut>
void keepThroughTables(const Tables& bits, const uint8_t* in, uint64_t samples, uint32_t stride, uint8_t* out) {
  const auto value = [&bits](const uint8_t* sample) {
    uint32_t v = bits[0][sample[0]];
    if constexpr (kReached > 1) v |= bits[1][sample[1]];
    if constexpr (kReached > 2) v |= bits[2][sample[2]];
    if constexpr (kReached > 3) v |= bits[3][sample[3]];
    return v;
  };
  // WHY: Store 4 bytes for 3 only with a stride of 4 or more: in place they would overwrite the next sample.
  if constexpr (kOut == 3) {
    if (stride >= 4 && samples > 0) {
      for (uint64_t s = 0; s + 1 < samples; ++s) {
        const uint32_t v = value(in + s * stride);
        std::memcpy(out + s * 3, &v, 4);
      }
      in += (samples - 1) * stride;
      out += (samples - 1) * 3;
      samples = 1;
    }
  }
  for (uint64_t s = 0; s < samples; ++s) storeLow<kOut>(out + s * kOut, value(in + s * stride));
}

template <uint32_t kReached>
void keepThroughTablesOf(const Tables& bits, const uint8_t* in, uint64_t samples, uint32_t stride, uint32_t out_bytes,
                         uint8_t* out) {
  switch (out_bytes) {
    case 1:
      keepThroughTables<kReached, 1>(bits, in, samples, stride, out);
      return;
    case 2:
      keepThroughTables<kReached, 2>(bits, in, samples, stride, out);
      return;
    case 3:
      keepThroughTables<kReached, 3>(bits, in, samples, stride, out);
      return;
    default:
      keepThroughTables<kReached, 4>(bits, in, samples, stride, out);
      return;
  }
}

template <uint32_t kStride>
void gatherRounds(const uint8_t* in, uint64_t rounds, const std::array<uint8_t, 32>& kept_in, uint32_t n,
                  uint8_t* out) {
  uint8_t kept[kStride];
  for (uint32_t k = 0; k < n; ++k) kept[k] = kept_in[k];
  for (uint64_t r = 0; r < rounds; ++r, in += kStride, out += n) {
    for (uint32_t k = 0; k < n; ++k) out[k] = in[kept[k]];
  }
}

void gatherRoundsOfAnyStride(const uint8_t* in, uint64_t rounds, uint32_t stride, const std::array<uint8_t, 32>& kept_in,
                             uint32_t n, uint8_t* out) {
  uint8_t kept[32];
  for (uint32_t k = 0; k < n; ++k) kept[k] = kept_in[k];
  for (uint64_t r = 0; r < rounds; ++r, in += stride, out += n) {
    for (uint32_t k = 0; k < n; ++k) out[k] = in[kept[k]];
  }
}

}

std::expected<void, std::string> checkDeviceBytes(const bitoscope_device_bytes& bytes, uint32_t device_channels) {
  if (device_channels == 0 || device_channels > kMaskBits) {
    return std::unexpected("device has " + std::to_string(device_channels) +
                           " channels; logic analyzer has 1 to 32 channels");
  }
  if (bytes.layout != BITOSCOPE_SAMPLE_MAJOR && bytes.layout != BITOSCOPE_CHANNEL_MAJOR) {
    return std::unexpected("layout " + std::to_string(bytes.layout) +
                           " is neither BITOSCOPE_SAMPLE_MAJOR nor BITOSCOPE_CHANNEL_MAJOR");
  }
  const uint32_t every = everyChannel(device_channels);
  if ((bytes.channel_mask & ~every) != 0) {
    return std::unexpected("bytes carry channel " + std::to_string(31 - std::countl_zero(bytes.channel_mask)) +
                           ", but logic analyzer has channels 0 to " + std::to_string(device_channels - 1));
  }
  const bool channel_major = bytes.layout == BITOSCOPE_CHANNEL_MAJOR;
  const uint32_t carried = carriedMaskOf(bytes, device_channels);
  const uint32_t minimal = minimalStride(channel_major, carried);
  if (bytes.stride_bytes != 0 && bytes.stride_bytes < minimal) {
    return std::unexpected("stride of " + bytesText(bytes.stride_bytes) + " is less than " + bytesText(minimal) +
                           " that " + std::to_string(std::popcount(carried)) + " channels take");
  }
  return {};
}

std::expected<DeviceByteCompactor, std::string> DeviceByteCompactor::create(const bitoscope_device_bytes& bytes,
                                                                            uint32_t device_channels,
                                                                            uint32_t capture_mask) {
  if (auto checked = checkDeviceBytes(bytes, device_channels); !checked) return std::unexpected(checked.error());
  if (capture_mask == 0) return std::unexpected(std::string("capture selects no channel"));
  const uint32_t carried = carriedMaskOf(bytes, device_channels);
  if (const uint32_t missing = capture_mask & ~carried; missing != 0) {
    return std::unexpected("capture selects channel " + std::to_string(std::countr_zero(missing)) +
                           ", which bytes do not carry (they carry " + maskText(carried) + ")");
  }
  DeviceByteCompactor c;
  c.channel_major_ = bytes.layout == BITOSCOPE_CHANNEL_MAJOR;
  const uint32_t minimal = minimalStride(c.channel_major_, carried);
  c.stride_ = bytes.stride_bytes == 0 ? minimal : bytes.stride_bytes;
  c.carried_channels_ = static_cast<uint32_t>(std::popcount(carried));
  c.kept_count_ = static_cast<uint32_t>(std::popcount(capture_mask));
  c.out_bytes_ = c.channel_major_ ? c.kept_count_ : (c.kept_count_ + 7) / 8;
  uint32_t k = 0;
  for (uint32_t channel = 0; channel < kMaskBits; ++channel) {
    if (((capture_mask >> channel) & 1u) == 0) continue;
    c.kept_[k++] = static_cast<uint8_t>(std::popcount(carried & ((uint32_t{1} << channel) - 1)));
  }
  bool lowest = true;
  for (uint32_t j = 0; j < c.kept_count_; ++j) lowest = lowest && c.kept_[j] == j;
  if (carried == capture_mask && c.stride_ == minimal) {
    c.kind_ = Kind::copy;
    return c;
  }
  if (c.channel_major_) {
    c.kind_ = Kind::gather;
    return c;
  }
  if (lowest) {
    c.kind_ = Kind::lowest;
    c.top_mask_ = static_cast<uint8_t>(c.kept_count_ % 8 == 0 ? 0xFF : (1u << (c.kept_count_ % 8)) - 1);
    return c;
  }
  c.kind_ = Kind::table;
  c.bytes_reached_ = c.kept_[c.kept_count_ - 1] / 8u + 1;
  for (uint32_t b = 0; b < c.bytes_reached_; ++b) {
    for (uint32_t v = 0; v < 256; ++v) {
      uint32_t packed = 0;
      for (uint32_t j = 0; j < c.kept_count_; ++j) {
        const uint32_t position = c.kept_[j];
        if (position / 8 == b && ((v >> (position % 8)) & 1u) != 0) packed |= uint32_t{1} << j;
      }
      c.bits_[b][v] = packed;
    }
  }
  return c;
}

void DeviceByteCompactor::compact(const uint8_t* device, uint64_t units, uint8_t* captured) const {
  if (units == 0) return;
  switch (kind_) {
    case Kind::copy:
      if (captured != device) std::memmove(captured, device, static_cast<size_t>(units * stride_));
      return;
    case Kind::lowest:
      switch (out_bytes_) {
        case 1:
          keepLowest<1>(device, units, stride_, top_mask_, captured);
          return;
        case 2:
          keepLowest<2>(device, units, stride_, top_mask_, captured);
          return;
        case 3:
          keepLowest<3>(device, units, stride_, top_mask_, captured);
          return;
        default:
          keepLowest<4>(device, units, stride_, top_mask_, captured);
          return;
      }
    case Kind::table:
      switch (bytes_reached_) {
        case 1:
          keepThroughTablesOf<1>(bits_, device, units, stride_, out_bytes_, captured);
          return;
        case 2:
          keepThroughTablesOf<2>(bits_, device, units, stride_, out_bytes_, captured);
          return;
        case 3:
          keepThroughTablesOf<3>(bits_, device, units, stride_, out_bytes_, captured);
          return;
        default:
          keepThroughTablesOf<4>(bits_, device, units, stride_, out_bytes_, captured);
          return;
      }
    case Kind::gather:
      switch (stride_) {
        case 4:
          gatherRounds<4>(device, units, kept_, kept_count_, captured);
          return;
        case 8:
          gatherRounds<8>(device, units, kept_, kept_count_, captured);
          return;
        case 16:
          gatherRounds<16>(device, units, kept_, kept_count_, captured);
          return;
        case 32:
          gatherRounds<32>(device, units, kept_, kept_count_, captured);
          return;
        default:
          gatherRoundsOfAnyStride(device, units, stride_, kept_, kept_count_, captured);
          return;
      }
  }
}

}

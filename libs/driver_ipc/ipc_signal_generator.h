/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ipc_decode_error.h"
#include "ipc_messages.h"

namespace bitoscope::ipc::siggen {

inline constexpr std::string_view kFacetName = "siggen";

struct SignalGeneratorInfo {
  uint32_t numChannels = 0;
  std::vector<std::string> supportedWaveforms;
  uint64_t minFrequencyMilliHz = 0;
  uint64_t maxFrequencyMilliHz = 0;
  bool supportsSynchronizedStart = false;
  bool operator==(const SignalGeneratorInfo&) const = default;
};

struct Waveform {
  std::string name;
  Bytes params;
  bool operator==(const Waveform&) const = default;
};

struct StartOutput {
  Waveform waveform;
  bool operator==(const StartOutput&) const = default;
};

struct StopOutput {
  bool operator==(const StopOutput&) const = default;
};

struct ConfigureOutput {
  Waveform waveform;
  bool operator==(const ConfigureOutput&) const = default;
};

struct SignalGeneratorCommand {
  uint32_t channelMask = 0;
  std::variant<StartOutput, StopOutput, ConfigureOutput> action;
  bool operator==(const SignalGeneratorCommand&) const = default;
};

struct SquareWaveParams {
  uint64_t frequencyMilliHz = 0;
  uint32_t dutyPercent = 0;
  bool operator==(const SquareWaveParams&) const = default;
};

struct SineWaveParams {
  uint64_t frequencyMilliHz = 0;
  uint64_t amplitudeMicrovolts = 0;
  int64_t offsetMicrovolts = 0;
  uint32_t phaseMilliDegrees = 0;
  bool operator==(const SineWaveParams&) const = default;
};

std::expected<Bytes, std::string> encodeInfo(const SignalGeneratorInfo& info);
std::expected<Bytes, std::string> encodeCommand(const SignalGeneratorCommand& command);
std::expected<Bytes, std::string> encodeSquare(const SquareWaveParams& params);
std::expected<Bytes, std::string> encodeSine(const SineWaveParams& params);

std::expected<SignalGeneratorInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes);
std::expected<SignalGeneratorCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes);
std::expected<SquareWaveParams, DecodeError> decodeSquare(std::span<const uint8_t> bytes);
std::expected<SineWaveParams, DecodeError> decodeSine(std::span<const uint8_t> bytes);

}

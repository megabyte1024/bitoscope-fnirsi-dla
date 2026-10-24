/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "facet_client.h"
#include "ipc_signal_generator.h"

namespace bitoscope::host::pipe {

inline constexpr std::string_view kSiggenFacetName = ipc::siggen::kFacetName;

enum class StartMode {
  Independent,
  Synchronized,
};

struct SiggenCapabilities {
  uint32_t num_ports = 0;
  std::vector<std::string> supported_waveforms;
  uint64_t min_frequency_milli_hz = 0;
  uint64_t max_frequency_milli_hz = 0;
  bool supports_synchronized_start = false;

  bool supportsWaveform(std::string_view waveform) const;
};

struct SquareWave {
  uint64_t frequency_milli_hz = 0;
  uint32_t duty_percent = 50;
};

class SiggenClient {
 public:
  SiggenClient(FacetClient client, SiggenCapabilities capabilities);

  static std::expected<SiggenCapabilities, std::string> parseCapabilities(std::span<const uint8_t> info);

  const SiggenCapabilities& capabilities() const { return capabilities_; }

  std::expected<void, std::string> configureSquare(uint32_t port_mask, const SquareWave& wave);
  std::expected<void, std::string> start(uint32_t port_mask, StartMode mode);
  std::expected<void, std::string> stop(uint32_t port_mask);

 private:
  std::expected<void, std::string> checkPorts(uint32_t port_mask) const;
  std::expected<void, std::string> send(const ipc::siggen::SignalGeneratorCommand& command);

  FacetClient client_;
  SiggenCapabilities capabilities_;
};

}

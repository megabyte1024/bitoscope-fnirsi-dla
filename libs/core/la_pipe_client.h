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
#include "ipc_la_acquisition.h"

namespace bitoscope::host::pipe {

inline constexpr std::string_view kLaAcquisitionFacetName = ipc::la::kFacetName;

struct LaCapabilities {
  uint32_t num_channels = 0;
  std::vector<uint64_t> supported_sample_rates_milli_hz;
  bool bulk_ring = false;
  std::vector<uint64_t> rate_ceilings_milli_hz;
  uint64_t default_sample_rate_milli_hz = 0;
  std::vector<ipc::la::ChannelPreset> channel_presets;

  bool supportsSampleRate(uint64_t milli_hz) const;
};

struct CaptureRequest {
  uint64_t sample_rate_milli_hz = 0;
  uint32_t channel_mask = 0;
  uint64_t sample_limit = 0;
  ipc::la::AcquisitionMode mode = ipc::la::AcquisitionMode::streaming;
  uint64_t capture_id = 0;
  std::string bulk_ring_name{};
};

class LaAcquisitionClient {
 public:
  LaAcquisitionClient(FacetClient client, LaCapabilities capabilities);

  static std::expected<LaCapabilities, std::string> parseCapabilities(std::span<const uint8_t> info);

  const LaCapabilities& capabilities() const { return capabilities_; }

  std::expected<void, std::string> check(const CaptureRequest& request) const;
  std::expected<void, std::string> start(const CaptureRequest& request);
  std::expected<void, std::string> stop();

 private:
  std::expected<void, std::string> send(const ipc::la::LaAcquisitionCommand& command);

  FacetClient client_;
  LaCapabilities capabilities_;
};

}

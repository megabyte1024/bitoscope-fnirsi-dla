/*
 * SPDX-License-Identifier: MIT
 */

#include "la_pipe_client.h"

#include <algorithm>
#include <utility>

#include "channel_mask.h"

namespace bitoscope::host::pipe {

namespace {

std::string formatRate(uint64_t milli_hz) {
  if (milli_hz % 1000 == 0) return std::to_string(milli_hz / 1000) + " Hz";
  return std::to_string(milli_hz) + " mHz";
}

}

bool LaCapabilities::supportsSampleRate(uint64_t milli_hz) const {
  return std::find(supported_sample_rates_milli_hz.begin(), supported_sample_rates_milli_hz.end(), milli_hz) !=
         supported_sample_rates_milli_hz.end();
}

LaAcquisitionClient::LaAcquisitionClient(FacetClient client, LaCapabilities capabilities)
    : client_(std::move(client)), capabilities_(std::move(capabilities)) {}

std::expected<LaCapabilities, std::string> LaAcquisitionClient::parseCapabilities(std::span<const uint8_t> info) {
  auto decoded = ipc::la::decodeInfo(info);
  if (!decoded) return std::unexpected("malformed la.acquisition descriptor: " + decoded.error().message);
  LaCapabilities caps;
  caps.num_channels = decoded->numChannels;
  caps.supported_sample_rates_milli_hz = std::move(decoded->supportedSampleRatesMilliHz);
  caps.bulk_ring = decoded->bulkRing;
  caps.rate_ceilings_milli_hz = std::move(decoded->rateCeilingsMilliHz);
  caps.default_sample_rate_milli_hz = decoded->defaultSampleRateMilliHz;
  caps.channel_presets = std::move(decoded->channelPresets);
  return caps;
}

std::expected<void, std::string> LaAcquisitionClient::check(const CaptureRequest& request) const {
  if (request.sample_rate_milli_hz == 0) return std::unexpected(std::string("sample rate must be non-zero"));
  if (request.channel_mask == 0) return std::unexpected(std::string("channel_mask selects no channels"));
  if ((request.channel_mask & ~lowBitsMask(capabilities_.num_channels)) != 0) {
    return std::unexpected("channel_mask 0x" + maskToHex(request.channel_mask) +
                           " selects channel this device does not have - it has " +
                           std::to_string(capabilities_.num_channels));
  }
  const auto& rates = capabilities_.supported_sample_rates_milli_hz;
  if (!rates.empty() && !capabilities_.supportsSampleRate(request.sample_rate_milli_hz)) {
    const auto [lowest, highest] = std::minmax_element(rates.begin(), rates.end());
    return std::unexpected("this device does not support sample rate of " +
                           formatRate(request.sample_rate_milli_hz) + "; it advertises " +
                           std::to_string(rates.size()) + " rates from " + formatRate(*lowest) + " to " +
                           formatRate(*highest));
  }
  return {};
}

std::expected<void, std::string> LaAcquisitionClient::start(const CaptureRequest& request) {
  if (auto checked = check(request); !checked) return checked;
  return send(ipc::la::StartCapture{request.sample_rate_milli_hz, request.channel_mask, request.sample_limit,
                                    request.mode, request.capture_id, request.bulk_ring_name});
}

std::expected<void, std::string> LaAcquisitionClient::stop() { return send(ipc::la::StopCapture{}); }

std::expected<void, std::string> LaAcquisitionClient::send(const ipc::la::LaAcquisitionCommand& command) {
  auto payload = ipc::la::encodeCommand(command);
  if (!payload) return std::unexpected("could not encode la.acquisition command: " + payload.error());
  auto answer = client_.call(std::string_view(reinterpret_cast<const char*>(payload->data()), payload->size()));
  if (!answer) return std::unexpected(answer.error());
  return {};
}

}

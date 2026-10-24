/*
 * SPDX-License-Identifier: MIT
 */

#include "siggen_pipe_client.h"

#include <algorithm>
#include <utility>

#include "channel_mask.h"

namespace bitoscope::host::pipe {

bool SiggenCapabilities::supportsWaveform(std::string_view waveform) const {
  return std::find(supported_waveforms.begin(), supported_waveforms.end(), waveform) != supported_waveforms.end();
}

SiggenClient::SiggenClient(FacetClient client, SiggenCapabilities capabilities)
    : client_(std::move(client)), capabilities_(std::move(capabilities)) {}

std::expected<SiggenCapabilities, std::string> SiggenClient::parseCapabilities(std::span<const uint8_t> info) {
  auto decoded = ipc::siggen::decodeInfo(info);
  if (!decoded) return std::unexpected("malformed siggen descriptor: " + decoded.error().message);
  SiggenCapabilities caps;
  caps.num_ports = decoded->numChannels;
  caps.supported_waveforms = std::move(decoded->supportedWaveforms);
  caps.min_frequency_milli_hz = decoded->minFrequencyMilliHz;
  caps.max_frequency_milli_hz = decoded->maxFrequencyMilliHz;
  caps.supports_synchronized_start = decoded->supportsSynchronizedStart;
  return caps;
}

std::expected<void, std::string> SiggenClient::configureSquare(uint32_t port_mask, const SquareWave& wave) {
  if (auto ports = checkPorts(port_mask); !ports) return ports;
  if (!capabilities_.supportsWaveform("square")) {
    std::string message = "this device does not support 'square' waveform; it advertises:";
    if (capabilities_.supported_waveforms.empty()) message += " none";
    for (const std::string& supported : capabilities_.supported_waveforms) message += " " + supported;
    return std::unexpected(message);
  }
  auto params = ipc::siggen::encodeSquare(ipc::siggen::SquareWaveParams{wave.frequency_milli_hz, wave.duty_percent});
  if (!params) return std::unexpected("could not encode square wave: " + params.error());
  return send(ipc::siggen::SignalGeneratorCommand{
      port_mask, ipc::siggen::ConfigureOutput{ipc::siggen::Waveform{"square", std::move(*params)}}});
}

std::expected<void, std::string> SiggenClient::start(uint32_t port_mask, StartMode mode) {
  if (auto ports = checkPorts(port_mask); !ports) return ports;
  if (mode == StartMode::Synchronized && !capabilities_.supports_synchronized_start) {
    // WHY: Refused rather than quietly downgraded: a caller that asked for simultaneity would get sequential starts.
    return std::unexpected(std::string(
        "this device does not support synchronized start - it would start requested ports one by one, "
        "so they would not be phase-locked; use StartMode::Independent to accept that explicitly"));
  }
  // WHY: One command for every port: splitting it here would guarantee the ports do not start together.
  return send(ipc::siggen::SignalGeneratorCommand{port_mask, ipc::siggen::StartOutput{}});
}

std::expected<void, std::string> SiggenClient::stop(uint32_t port_mask) {
  if (auto ports = checkPorts(port_mask); !ports) return ports;
  return send(ipc::siggen::SignalGeneratorCommand{port_mask, ipc::siggen::StopOutput{}});
}

std::expected<void, std::string> SiggenClient::checkPorts(uint32_t port_mask) const {
  if (port_mask == 0) return std::unexpected(std::string("port_mask selects no ports"));
  if ((port_mask & ~lowBitsMask(capabilities_.num_ports)) != 0) {
    return std::unexpected("port_mask 0x" + maskToHex(port_mask) +
                           " selects port this device does not have - it has " +
                           std::to_string(capabilities_.num_ports));
  }
  return {};
}

std::expected<void, std::string> SiggenClient::send(const ipc::siggen::SignalGeneratorCommand& command) {
  auto payload = ipc::siggen::encodeCommand(command);
  if (!payload) return std::unexpected("could not encode siggen command: " + payload.error());
  auto answer = client_.call(std::string_view(reinterpret_cast<const char*>(payload->data()), payload->size()));
  if (!answer) return std::unexpected(answer.error());
  return {};
}

}

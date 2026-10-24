/*
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bitoscope_driver.h"
#include "device_bytes.h"
#include "driver_calls.h"
#include "driver_description.h"
#include "driver_model.h"
#include "driver_plugin.h"
#include "facet_properties.h"
#include "frame_output.h"
#include "ipc_la_acquisition.h"
#include "ipc_signal_generator.h"
#include "la_runtime.h"
#include "plugin_model.h"
#include "private_stdio.h"
#include "signal_generator_facet.h"

namespace sdk = bitoscope::plugin_c_sdk;
namespace ipc = bitoscope::ipc;

namespace {

constexpr uint32_t kChannelMaskBits = 32;
constexpr int kHostChannelColorCount = 32;
constexpr const char* kFacetsAreIdentifys = "facets and their properties are added in identify only";

int refused(const std::string& message) {
  sdk::logLine(ipc::LogLevel::error, message);
  return BITOSCOPE_ERROR;
}

bool empty(const char* text) { return text == nullptr || text[0] == '\0'; }

// WHY: Copy only `size` bytes of a caller's struct: a driver built against an older header passes a shorter one.
template <typename T>
T copiedFromPossiblyShorterStruct(const T* from, size_t size) {
  T out{};
  std::memcpy(&out, from, (std::min)(size, sizeof(T)));
  return out;
}

std::expected<void, std::string> addFound(bitoscope_found_devices& found, const bitoscope_found_device& device) {
  if (empty(device.id)) return std::unexpected(std::string("device needs id (host picks device by it)"));
  const std::string id = device.id;
  if (std::any_of(found.devices.begin(), found.devices.end(), [&](const sdk::SourceDevice& d) { return d.id == id; })) {
    return std::unexpected("id \"" + id + "\" is found already");
  }
  auto located = sdk::sourceDeviceOf(device);
  if (!located) return std::unexpected(std::move(located.error()));
  found.devices.push_back(std::move(*located));
  return {};
}

bool hasFacet(const bitoscope_device& device, const std::string& name, uint32_t major, uint32_t protocolMajor) {
  return std::any_of(device.facets.begin(), device.facets.end(), [&](const sdk::FacetRegistration& r) {
    return r.name == name && r.major == major && r.protocolMajor == protocolMajor;
  });
}

std::expected<std::vector<uint64_t>, std::string> rateCeilings(const bitoscope_logic_analyzer& la,
                                                               const std::vector<uint64_t>& rates) {
  if (la.num_rate_ceilings == 0) return std::vector<uint64_t>{};
  if (la.rate_ceilings_milli_hz == nullptr) return std::unexpected(std::string("rate ceilings missing"));
  if (la.num_rate_ceilings != la.num_channels) {
    return std::unexpected(std::to_string(la.num_rate_ceilings) + " rate ceilings for " +
                           std::to_string(la.num_channels) + " channels; give exactly one per number of channels (1 to " +
                           std::to_string(la.num_channels) + "), or none");
  }
  const uint64_t lowest = *std::min_element(rates.begin(), rates.end());
  std::vector<uint64_t> out(la.rate_ceilings_milli_hz, la.rate_ceilings_milli_hz + la.num_rate_ceilings);
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] < lowest) {
      return std::unexpected("rate ceiling for " + std::to_string(i + 1) + " channels, " + std::to_string(out[i]) +
                             " mHz, is below lowest rate, " + std::to_string(lowest) + " mHz: " +
                             std::to_string(i + 1) + " channels would have no rate");
    }
  }
  return out;
}

bool isColorId(const std::string& id) {
  static constexpr std::string_view kPrefix = "color-";
  if (id.size() <= kPrefix.size() || id.compare(0, kPrefix.size(), kPrefix) != 0) return false;
  const std::string digits = id.substr(kPrefix.size());
  if (digits.size() > 2 || digits[0] == '0' || digits.find_first_not_of("0123456789") != std::string::npos) return false;
  const int n = std::stoi(digits);
  return n >= 1 && n <= kHostChannelColorCount;
}

std::expected<std::vector<ipc::la::ChannelPreset>, std::string> channelPresets(const bitoscope_logic_analyzer& la) {
  std::vector<ipc::la::ChannelPreset> out;
  if (la.num_channel_presets == 0) return out;
  if (la.channel_presets == nullptr) return std::unexpected(std::string("channel presets missing"));
  for (size_t i = 0; i < la.num_channel_presets; ++i) {
    const bitoscope_channel_preset& p = la.channel_presets[i];
    if (p.channel >= la.num_channels) {
      return std::unexpected("channel preset for channel " + std::to_string(p.channel) +
                             ", but this logic analyzer has channels 0 to " + std::to_string(la.num_channels - 1));
    }
    if (std::any_of(out.begin(), out.end(), [&](const ipc::la::ChannelPreset& o) { return o.channel == p.channel; })) {
      return std::unexpected("channel " + std::to_string(p.channel) + " has two channel presets");
    }
    ipc::la::ChannelPreset preset{p.channel, p.name != nullptr ? p.name : "", p.color_id != nullptr ? p.color_id : ""};
    if (!preset.colorId.empty() && !isColorId(preset.colorId)) {
      return std::unexpected("channel " + std::to_string(p.channel) + "'s preset color \"" + preset.colorId +
                             "\" is not one of \"color-1\" to \"color-32\"");
    }
    out.push_back(std::move(preset));
  }
  return out;
}

}

extern "C" {

int bitoscope_run_driver_sized(int argc, char** argv, const bitoscope_driver* driver, size_t driver_size) {
  try {
    if (driver == nullptr) {
      std::fprintf(stderr, "bitoscope driver: bitoscope_run_driver() was given no driver description\n");
      return sdk::kExitFailed;
    }
    const bitoscope_driver described = copiedFromPossiblyShorterStruct(driver, driver_size);
    std::vector<std::string> args;
    for (int i = 1; argv != nullptr && i < argc; ++i) args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    // WHY: Take stdin and stdout for the protocol BEFORE any driver code runs, so a stray printf cannot corrupt it.
    auto stdio = sdk::takeStdio(2);
    if (!stdio) {
      std::fprintf(stderr, "bitoscope driver: cannot take stdin and stdout for host: %s\n", stdio.error().c_str());
      return sdk::kExitFailed;
    }
    return sdk::runDriver(args, *stdio->in, *stdio->out, described);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bitoscope driver: %s\n", e.what());
    return sdk::kExitFailed;
  } catch (...) {
    return sdk::kExitFailed;
  }
}

int bitoscope_add_found_device(bitoscope_found_devices* found, const char* id) {
  try {
    if (found == nullptr) return BITOSCOPE_ERROR;
    bitoscope_found_device device{};
    device.id = id;
    auto added = addFound(*found, device);
    if (!added) return refused("bitoscope_add_found_device: " + added.error());
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_add_located_device_sized(bitoscope_found_devices* found, const bitoscope_found_device* device,
                                       size_t device_size) {
  try {
    if (found == nullptr || device == nullptr) return BITOSCOPE_ERROR;
    auto added = addFound(*found, copiedFromPossiblyShorterStruct(device, device_size));
    if (!added) return refused("bitoscope_add_located_device: " + added.error());
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_report_progress(bitoscope_progress* progress, int code, const char* detail) {
  try {
    if (progress == nullptr) return BITOSCOPE_ERROR;
    auto reported = sdk::reportProgress(*progress, code, detail != nullptr ? detail : "");
    if (!reported) return refused("bitoscope_report_progress: " + reported.error());
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_set_name(bitoscope_device* device, const char* name) {
  try {
    if (device == nullptr) return BITOSCOPE_ERROR;
    device->name = name != nullptr ? name : "";
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_set_serial_number(bitoscope_device* device, const char* serial_number) {
  try {
    if (device == nullptr) return BITOSCOPE_ERROR;
    device->serialNumber = serial_number != nullptr ? serial_number : "";
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_add_logic_analyzer_sized(bitoscope_device* device, const bitoscope_logic_analyzer* analyzer,
                                       size_t analyzer_size) {
  try {
    if (device == nullptr || analyzer == nullptr) return BITOSCOPE_ERROR;
    const bitoscope_logic_analyzer la = copiedFromPossiblyShorterStruct(analyzer, analyzer_size);
    const std::string where = "bitoscope_add_logic_analyzer (device \"" + device->id + "\"): ";
    if (device->facetsFixed) return refused(where + kFacetsAreIdentifys);
    if (la.capture == nullptr) return refused(where + "no capture function");
    if (la.num_channels == 0 || la.num_channels > kChannelMaskBits) {
      return refused(where + std::to_string(la.num_channels) + " channels; logic analyzer has 1 to 32");
    }
    if (la.num_sample_rates == 0 || la.sample_rates_milli_hz == nullptr) return refused(where + "no sample rate");
    if (auto checked = sdk::checkDeviceBytes(la.device_bytes, la.num_channels); !checked) {
      return refused(where + checked.error());
    }
    if (hasFacet(*device, sdk::kLaFacetName, sdk::kLaFacetMajor, 0)) {
      return refused(where + "device has logic analyzer already");
    }
    sdk::LogicAnalyzerRegistration registration;
    registration.numChannels = la.num_channels;
    registration.sampleRatesMilliHz.assign(la.sample_rates_milli_hz, la.sample_rates_milli_hz + la.num_sample_rates);
    registration.capture = la.capture;
    registration.deviceBytes = la.device_bytes;
    registration.prepare = la.prepare;
    if (la.rate_ceiling_reason != nullptr) registration.rateCeilingReason = la.rate_ceiling_reason;
    registration.applyProperties = la.apply_properties;
    auto ceilings = rateCeilings(la, registration.sampleRatesMilliHz);
    if (!ceilings) return refused(where + ceilings.error());
    registration.rateCeilingsMilliHz = std::move(*ceilings);
    auto presets = channelPresets(la);
    if (!presets) return refused(where + presets.error());
    ipc::la::LaAcquisitionInfo described;
    described.numChannels = la.num_channels;
    described.supportedSampleRatesMilliHz = registration.sampleRatesMilliHz;
    described.bulkRing = true;
    described.rateCeilingsMilliHz = registration.rateCeilingsMilliHz;
    const auto& rates = registration.sampleRatesMilliHz;
    described.defaultSampleRateMilliHz =
        std::find(rates.begin(), rates.end(), la.default_sample_rate_milli_hz) != rates.end()
            ? la.default_sample_rate_milli_hz
            : rates.front();
    described.channelPresets = std::move(*presets);
    auto info = ipc::la::encodeInfo(described);
    if (!info) return refused(where + "its description could not be encoded: " + info.error());
    device->facets.push_back(sdk::FacetRegistration{
        sdk::kLaFacetName, sdk::kLaFacetMajor, sdk::kLaFacetMinor, 0, {}, std::move(*info),
        [registration = std::move(registration), device](const std::vector<ipc::props::PropertyValue>& values) {
          sdk::LogicAnalyzerRegistration session = registration;
          session.session = device->session;
          return std::unique_ptr<sdk::FacetHandler>(std::make_unique<sdk::LaRuntime>(session, values));
        }});
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_add_signal_generator_sized(bitoscope_device* device, const bitoscope_signal_generator* generator,
                                         size_t generator_size) {
  try {
    if (device == nullptr || generator == nullptr) return BITOSCOPE_ERROR;
    const bitoscope_signal_generator g = copiedFromPossiblyShorterStruct(generator, generator_size);
    const std::string where = "bitoscope_add_signal_generator (device \"" + device->id + "\"): ";
    if (device->facetsFixed) return refused(where + kFacetsAreIdentifys);
    if (g.start == nullptr) return refused(where + "no start function");
    if (g.stop == nullptr) return refused(where + "no stop function");
    if (g.configure == nullptr) return refused(where + "no configure function");
    if (g.num_channels == 0 || g.num_channels > kChannelMaskBits) {
      return refused(where + std::to_string(g.num_channels) + " outputs; signal generator has 1 to 32");
    }
    if (g.num_waveforms > 0 && g.waveforms == nullptr) return refused(where + "waveform names missing");
    ipc::siggen::SignalGeneratorInfo described{g.num_channels, {}, g.min_frequency_milli_hz, g.max_frequency_milli_hz,
                                               g.supports_synchronized_start != 0};
    for (size_t i = 0; i < g.num_waveforms; ++i) {
      if (empty(g.waveforms[i])) return refused(where + "waveform needs name");
      described.supportedWaveforms.emplace_back(g.waveforms[i]);
    }
    const std::string name(ipc::siggen::kFacetName);
    if (hasFacet(*device, name, 1, 0)) return refused(where + "device has signal generator already");
    auto info = ipc::siggen::encodeInfo(described);
    if (!info) return refused(where + "its description could not be encoded: " + info.error());
    const sdk::SignalGeneratorRegistration registration{g.start, g.stop, g.configure, nullptr, g.apply_properties};
    device->facets.push_back(sdk::FacetRegistration{
        name, 1, 0, 0, {}, std::move(*info), [registration, device](const std::vector<ipc::props::PropertyValue>&) {
          sdk::SignalGeneratorRegistration session = registration;
          session.session = device->session;
          return sdk::makeSignalGeneratorFacet(session);
        }});
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_read_square_wave(const bitoscope_waveform* waveform, bitoscope_square_wave* square,
                               bitoscope_reply* reply) {
  try {
    if (waveform == nullptr || square == nullptr) return BITOSCOPE_ERROR;
    const auto* bytes = static_cast<const uint8_t*>(waveform->params);
    auto decoded = ipc::siggen::decodeSquare(std::span<const uint8_t>(bytes, bytes != nullptr ? waveform->params_size : 0));
    if (!decoded) {
      if (reply != nullptr) reply->message = decoded.error().message;
      return BITOSCOPE_ERROR;
    }
    square->frequency_milli_hz = decoded->frequencyMilliHz;
    square->duty_percent = decoded->dutyPercent;
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_add_facet_sized(bitoscope_device* device, const bitoscope_facet* facet, size_t facet_size) {
  try {
    if (device == nullptr || facet == nullptr) return BITOSCOPE_ERROR;
    const bitoscope_facet f = copiedFromPossiblyShorterStruct(facet, facet_size);
    const std::string where = "bitoscope_add_facet (device \"" + device->id + "\"): ";
    if (device->facetsFixed) return refused(where + kFacetsAreIdentifys);
    if (empty(f.name)) return refused(where + "facet needs name");
    if (f.command == nullptr) return refused(where + "facet \"" + f.name + "\" has no command function");
    const uint32_t major = f.version_major == 0 ? 1 : f.version_major;
    if (hasFacet(*device, f.name, major, f.protocol_major)) {
      return refused(where + "facet \"" + f.name + "\" major " + std::to_string(major) +
                     (f.protocol_major != 0 ? " for protocol major " + std::to_string(f.protocol_major) : std::string()) +
                     " is registered already");
    }
    if (f.num_capabilities > 0 && f.capabilities == nullptr) return refused(where + "capabilities missing");
    if (f.info_size > 0 && f.info == nullptr) return refused(where + "info missing");
    sdk::FacetRegistration registration;
    registration.name = f.name;
    registration.major = major;
    registration.minor = f.version_minor;
    registration.protocolMajor = f.protocol_major;
    for (size_t i = 0; i < f.num_capabilities; ++i) {
      if (empty(f.capabilities[i])) return refused(where + "capability needs name");
      registration.capabilities.emplace_back(f.capabilities[i]);
    }
    const auto* info = static_cast<const uint8_t*>(f.info);
    registration.info.assign(info, info + f.info_size);
    registration.makeHandler = [name = registration.name,
                                generic = sdk::GenericRegistration{f.command, nullptr, f.apply_properties},
                                device](const std::vector<ipc::props::PropertyValue>&) {
      sdk::GenericRegistration session = generic;
      session.session = device->session;
      return sdk::makeGenericFacet(name, session);
    };
    device->facets.push_back(std::move(registration));
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_reply_payload(bitoscope_reply* reply, const void* payload, size_t payload_size) {
  try {
    if (reply == nullptr || (payload == nullptr && payload_size > 0)) return BITOSCOPE_ERROR;
    const auto* bytes = static_cast<const uint8_t*>(payload);
    reply->payload.assign(bytes, bytes + payload_size);
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_reply_refused_property(bitoscope_reply* reply, const char* id) {
  try {
    if (reply == nullptr) return BITOSCOPE_ERROR;
    reply->refusedProperty = id != nullptr ? id : "";
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

int bitoscope_add_property_sized(bitoscope_device* device, const char* facet_name, const bitoscope_property* property,
                                 size_t property_size) {
  try {
    if (device == nullptr || property == nullptr) return BITOSCOPE_ERROR;
    const bitoscope_property p = copiedFromPossiblyShorterStruct(property, property_size);
    const std::string facet = facet_name != nullptr ? facet_name : "";
    const std::string where = "bitoscope_add_property (device \"" + device->id + "\", facet \"" + facet +
                              "\", property \"" + (p.id != nullptr ? p.id : "") + "\"): ";
    if (facet.empty()) return refused(where + "no facet name");
    if (device->facetsFixed) return refused(where + kFacetsAreIdentifys);
    const bool added = std::any_of(device->facets.begin(), device->facets.end(),
                                   [&](const sdk::FacetRegistration& r) { return r.name == facet; });
    if (!added) return refused(where + "device has no facet \"" + facet + "\"; add facet first");
    auto placed = sdk::propertyFrom(p);
    if (!placed) return refused(where + placed.error());
    auto tree = std::find_if(device->properties.begin(), device->properties.end(),
                             [&](const sdk::FacetPropertyTree& t) { return t.facet == facet; });
    std::vector<ipc::props::PropertyNode> nodes = tree != device->properties.end() ? tree->tree
                                                                                    : std::vector<ipc::props::PropertyNode>{};
    if (auto ok = sdk::addProperty(nodes, std::move(*placed)); !ok) return refused(where + ok.error());
    if (tree != device->properties.end()) {
      tree->tree = std::move(nodes);
    } else {
      device->properties.push_back(sdk::FacetPropertyTree{facet, std::move(nodes)});
    }
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

const bitoscope_property_value* bitoscope_find_property_value(const bitoscope_property_value* values, size_t num_values,
                                                              const char* id) {
  if (values == nullptr || id == nullptr) return nullptr;
  for (size_t i = 0; i < num_values; ++i) {
    if (values[i].id != nullptr && std::strcmp(values[i].id, id) == 0) return &values[i];
  }
  return nullptr;
}

void bitoscope_device_gone(bitoscope_sink* sink, const char* reason) {
  try {
    const std::string why = reason != nullptr && reason[0] != '\0' ? reason : "device is gone";
    // WHY: Tell the host the device is gone BEFORE the ring fails, so a host reading the ring does not retry a start.
    if (auto sent = sdk::sendDeviceLost(why); !sent) {
      sdk::logLine(ipc::LogLevel::warning, "host could not be told that device is gone: " + sent.error());
    }
    if (sink != nullptr) bitoscope_fail_capture(sink, why.c_str());
  } catch (...) {
  }
}

int bitoscope_send_event(const char* facet, const void* payload, size_t payload_size) {
  try {
    if (empty(facet) || (payload == nullptr && payload_size > 0)) return BITOSCOPE_ERROR;
    const auto* bytes = static_cast<const uint8_t*>(payload);
    auto sent = sdk::sendFacetEvent(facet, ipc::Bytes(bytes, bytes + payload_size));
    if (!sent) return refused(std::string("bitoscope_send_event (") + facet + "): " + sent.error());
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

}

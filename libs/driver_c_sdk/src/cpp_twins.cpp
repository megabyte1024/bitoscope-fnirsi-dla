/*
 * SPDX-License-Identifier: MIT
 */

#include "cpp_twins.h"

#include <exception>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace bitoscope::plugin_c_sdk {

namespace {

using plugin::PropertyNode;
using plugin::PropertyRefusal;
using plugin::PropertyValue;

std::expected<void, std::string> collect(const std::vector<PropertyNode>& nodes, const std::string& path,
                                         std::vector<std::pair<const plugin::Property*, std::string>>& out) {
  for (const PropertyNode& node : nodes) {
    if (const auto* property = std::get_if<plugin::Property>(&node.node)) {
      out.emplace_back(property, path);
      continue;
    }
    const auto& group = std::get<plugin::PropertyGroup>(node.node);
    if (group.name.empty()) return std::unexpected("group has empty name" + (path.empty() ? "" : " (in \"" + path + "\")"));
    if (group.name.find('/') != std::string::npos) {
      return std::unexpected("group \"" + group.name + "\": group name cannot hold '/'");
    }
    if (auto inner = collect(group.nodes, path.empty() ? group.name : path + "/" + group.name, out); !inner) return inner;
  }
  return {};
}

bitoscope_property cProperty(const plugin::Property& p, const std::string& group, std::vector<const char*>& names) {
  bitoscope_property c{};
  c.id = p.id.c_str();
  c.name = p.name.c_str();
  c.help = p.help.c_str();
  c.group = group.c_str();
  c.unit = static_cast<int>(p.unit);
  c.changeable_during_capture = p.changeableDuringCapture ? 1 : 0;
  if (const auto* integer = std::get_if<plugin::IntegerType>(&p.type)) {
    c.type = BITOSCOPE_PROPERTY_INTEGER;
    c.has_minimum = integer->minimum.has_value() ? 1 : 0;
    c.minimum = integer->minimum.value_or(0);
    c.has_maximum = integer->maximum.has_value() ? 1 : 0;
    c.maximum = integer->maximum.value_or(0);
    c.default_value = integer->defaultValue;
  } else if (const auto* decimal = std::get_if<plugin::DecimalType>(&p.type)) {
    c.type = BITOSCOPE_PROPERTY_DECIMAL;
    c.digits = decimal->digits;
    c.has_minimum = decimal->minimum.has_value() ? 1 : 0;
    c.minimum = decimal->minimum.value_or(0);
    c.has_maximum = decimal->maximum.has_value() ? 1 : 0;
    c.maximum = decimal->maximum.value_or(0);
    c.default_value = decimal->defaultValue;
  } else if (const auto* boolean = std::get_if<plugin::BooleanType>(&p.type)) {
    c.type = BITOSCOPE_PROPERTY_BOOLEAN;
    c.default_value = boolean->defaultValue ? 1 : 0;
    c.boolean_style = static_cast<int>(boolean->style);
  } else {
    const auto& enumeration = std::get<plugin::EnumerationType>(p.type);
    c.type = BITOSCOPE_PROPERTY_ENUMERATION;
    c.digits = enumeration.digits;
    c.default_index = enumeration.defaultIndex;
    for (const std::string& name : enumeration.names) names.push_back(name.c_str());
    c.item_names = names.empty() ? nullptr : names.data();
    c.item_numbers = enumeration.numbers.empty() ? nullptr : enumeration.numbers.data();
    c.num_items = !enumeration.names.empty() ? enumeration.names.size() : enumeration.numbers.size();
  }
  return c;
}

plugin::LogicAnalyzer& analyzerOf(void* session) { return *static_cast<plugin::Session*>(session)->logicAnalyzer(); }

plugin::SignalGenerator& generatorOf(void* session) {
  return *static_cast<plugin::Session*>(session)->signalGenerator();
}

template <typename Apply>
int refusedOrApplied(bitoscope_reply* reply, std::string_view function, Apply&& apply) {
  PropertyRefusal refusal;
  try {
    auto applied = apply();
    if (applied) return BITOSCOPE_OK;
    refusal = std::move(applied.error());
  } catch (const std::exception& e) {
    refusal.reason = std::string(function) + " threw: " + e.what();
  } catch (...) {
    refusal.reason = std::string(function) + " threw";
  }
  (void)bitoscope_reply_message(reply, refusal.reason.c_str());
  if (!refusal.propertyId.empty()) (void)bitoscope_reply_refused_property(reply, refusal.propertyId.c_str());
  return BITOSCOPE_ERROR;
}

int applyDuringCaptureTrampoline(void* session, const bitoscope_property_value* values, size_t count,
                                 bitoscope_reply* reply) {
  return refusedOrApplied(reply, "applyDuringCapture", [&] { return analyzerOf(session).applyDuringCapture(cppValues(values, count)); });
}

int generatorApplyTrampoline(void* session, const bitoscope_property_value* values, size_t count, int capturing,
                             bitoscope_reply* reply) {
  return refusedOrApplied(reply, "applyProperties", [&] { return generatorOf(session).applyProperties(cppValues(values, count), capturing != 0); });
}

void captureTrampoline(void* session, const bitoscope_capture* capture, bitoscope_sink* sink) {
  plugin::Sink s(sink);
  try {
    auto captured = analyzerOf(session).capture(cppCapture(*capture), s);
    if (!captured) s.fail(captured.error());
  } catch (const std::exception& e) {
    s.fail(std::string("capture threw: ") + e.what());
  } catch (...) {
    s.fail("capture threw");
  }
}

int prepareTrampoline(void* session, const bitoscope_capture* capture, bitoscope_sink* sink, bitoscope_reply* reply) {
  std::string reason;
  try {
    plugin::Sink s(sink);
    auto prepared = analyzerOf(session).prepare(cppCapture(*capture), s);
    if (prepared) return BITOSCOPE_OK;
    reason = std::move(prepared.error());
  } catch (const std::exception& e) {
    reason = std::string("prepare threw: ") + e.what();
  } catch (...) {
    reason = "prepare threw";
  }
  (void)bitoscope_reply_message(reply, reason.c_str());
  return BITOSCOPE_ERROR;
}

plugin::Waveform toWaveform(const bitoscope_waveform& w) {
  const auto* bytes = static_cast<const uint8_t*>(w.params);
  return plugin::Waveform{w.name != nullptr ? w.name : "",
                          bytes != nullptr ? std::vector<uint8_t>(bytes, bytes + w.params_size) : std::vector<uint8_t>{}};
}

template <typename Command>
int generatorTrampoline(bitoscope_reply* reply, Command&& command) {
  std::string message;
  int status = BITOSCOPE_ERROR;
  try {
    std::expected<std::string, std::string> answer = command();
    if (answer) {
      message = std::move(*answer);
      status = BITOSCOPE_OK;
    } else {
      message = std::move(answer.error());
    }
  } catch (const std::exception& e) {
    message = std::string("signal generator threw: ") + e.what();
  } catch (...) {
    message = "signal generator threw";
  }
  (void)bitoscope_reply_message(reply, message.c_str());
  return status;
}

int generatorStart(void* session, uint32_t mask, const bitoscope_waveform* waveform, bitoscope_reply* reply) {
  return generatorTrampoline(reply, [&] { return generatorOf(session).start(mask, toWaveform(*waveform)); });
}

int generatorStop(void* session, uint32_t mask, bitoscope_reply* reply) {
  return generatorTrampoline(reply, [&] { return generatorOf(session).stop(mask); });
}

int generatorConfigure(void* session, uint32_t mask, const bitoscope_waveform* waveform, bitoscope_reply* reply) {
  return generatorTrampoline(reply, [&] { return generatorOf(session).configure(mask, toWaveform(*waveform)); });
}

}

CProperties::CProperties(const std::vector<PropertyNode>& tree) {
  std::vector<std::pair<const plugin::Property*, std::string>> collected;
  if (auto ok = collect(tree, "", collected); !ok) {
    error_ = ok.error();
    return;
  }
  // WHY: c_ points into entries_' strings and arrays: size entries_ once here and never grow it afterwards.
  entries_.resize(collected.size());
  c_.reserve(collected.size());
  for (size_t i = 0; i < collected.size(); ++i) {
    Entry& e = entries_[i];
    e.source = collected[i].first;
    e.group = std::move(collected[i].second);
    c_.push_back(cProperty(*e.source, e.group, e.names));
  }
}

CLogicAnalyzer::CLogicAnalyzer(const plugin::LogicAnalyzerInfo& info) : properties_(info.properties) {
  for (const plugin::ChannelPreset& p : info.channelPresets) {
    presets_.push_back({p.channel, p.name.c_str(), p.colorId.c_str()});
  }
  c_.num_channels = info.numChannels;
  c_.sample_rates_milli_hz = info.sampleRatesMilliHz.empty() ? nullptr : info.sampleRatesMilliHz.data();
  c_.num_sample_rates = info.sampleRatesMilliHz.size();
  c_.capture = &captureTrampoline;
  c_.device_bytes = bitoscope_device_bytes{static_cast<int>(info.deviceBytes.layout), info.deviceBytes.channelMask,
                                           info.deviceBytes.strideBytes};
  c_.prepare = &prepareTrampoline;
  c_.rate_ceilings_milli_hz = info.rateCeilingsMilliHz.empty() ? nullptr : info.rateCeilingsMilliHz.data();
  c_.num_rate_ceilings = info.rateCeilingsMilliHz.size();
  c_.default_sample_rate_milli_hz = info.defaultSampleRateMilliHz;
  c_.channel_presets = presets_.empty() ? nullptr : presets_.data();
  c_.num_channel_presets = presets_.size();
  c_.apply_during_capture = &applyDuringCaptureTrampoline;
  c_.rate_ceiling_reason = info.rateCeilingReason.empty() ? nullptr : info.rateCeilingReason.c_str();
  c_.properties = properties_.data();
  c_.num_properties = properties_.size();
  c_.silence_limit_ms = info.silenceLimitMs;
  c_.usb_stream_properties = info.usbStreamProperties ? 1 : 0;
}

CSignalGenerator::CSignalGenerator(const plugin::SignalGeneratorInfo& info) : properties_(info.properties) {
  for (const std::string& name : info.supportedWaveforms) waveforms_.push_back(name.c_str());
  c_.num_channels = info.numChannels;
  c_.start = &generatorStart;
  c_.stop = &generatorStop;
  c_.configure = &generatorConfigure;
  c_.waveforms = waveforms_.empty() ? nullptr : waveforms_.data();
  c_.num_waveforms = waveforms_.size();
  c_.min_frequency_milli_hz = info.minFrequencyMilliHz;
  c_.max_frequency_milli_hz = info.maxFrequencyMilliHz;
  c_.supports_synchronized_start = info.supportsSynchronizedStart ? 1 : 0;
  c_.apply_properties = &generatorApplyTrampoline;
  c_.properties = properties_.data();
  c_.num_properties = properties_.size();
}

std::vector<PropertyValue> cppValues(const bitoscope_property_value* values, size_t count) {
  std::vector<PropertyValue> out;
  for (size_t i = 0; values != nullptr && i < count; ++i) {
    const bitoscope_property_value& v = values[i];
    out.push_back(PropertyValue{v.id != nullptr ? v.id : "", v.integer, v.boolean != 0, v.enum_index});
  }
  return out;
}

plugin::Capture cppCapture(const bitoscope_capture& c) {
  plugin::Capture out{c.sample_rate_milli_hz, c.channel_mask, c.num_captured_channels, c.sample_limit, c.buffered != 0,
                      {}};
  for (PropertyValue& value : cppValues(c.properties, c.num_properties)) {
    std::string id = value.id;
    out.properties.emplace(std::move(id), std::move(value));
  }
  return out;
}

}

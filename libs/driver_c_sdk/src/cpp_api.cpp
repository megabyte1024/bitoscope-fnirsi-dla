/*
 * SPDX-License-Identifier: MIT
 */

#include "bitoscope_driver_cpp.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <variant>

#include "facet_properties.h"
#include "ipc_signal_generator.h"
#include "plugin_model.h"

namespace bitoscope::plugin {

namespace {

PropertyValue toValue(const bitoscope_property_value& v) {
  return PropertyValue{v.id != nullptr ? v.id : "", v.integer, v.boolean != 0, v.enum_index};
}

std::vector<PropertyValue> toValues(const bitoscope_property_value* values, size_t count) {
  std::vector<PropertyValue> out;
  for (size_t i = 0; values != nullptr && i < count; ++i) out.push_back(toValue(values[i]));
  return out;
}

Capture toCapture(const bitoscope_capture& c) {
  Capture out{c.sample_rate_milli_hz, c.channel_mask, c.num_captured_channels, c.sample_limit, c.buffered != 0, {}};
  for (PropertyValue& value : toValues(c.properties, c.num_properties)) {
    std::string id = value.id;
    out.properties.emplace(std::move(id), std::move(value));
  }
  return out;
}

// WHY: Entry holds pointers into its own members: never move one after cEntries() has set them.
struct Entry {
  const Property* source = nullptr;
  std::string group;
  std::vector<const char*> names;
  bitoscope_property c{};
};

std::expected<void, std::string> collect(const std::vector<PropertyNode>& nodes, const std::string& path,
                                         std::vector<std::pair<const Property*, std::string>>& out) {
  for (const PropertyNode& node : nodes) {
    if (const auto* property = std::get_if<Property>(&node.node)) {
      out.emplace_back(property, path);
      continue;
    }
    const auto& group = std::get<PropertyGroup>(node.node);
    if (group.name.empty()) return std::unexpected("group has empty name" + (path.empty() ? "" : " (in \"" + path + "\")"));
    if (group.name.find('/') != std::string::npos) {
      return std::unexpected("group \"" + group.name + "\": group name cannot hold '/'");
    }
    if (auto inner = collect(group.nodes, path.empty() ? group.name : path + "/" + group.name, out); !inner) return inner;
  }
  return {};
}

void fill(Entry& e) {
  const Property& p = *e.source;
  e.c.id = p.id.c_str();
  e.c.name = p.name.c_str();
  e.c.help = p.help.c_str();
  e.c.group = e.group.c_str();
  e.c.unit = static_cast<int>(p.unit);
  e.c.changeable_during_capture = p.changeableDuringCapture ? 1 : 0;
  if (const auto* integer = std::get_if<IntegerType>(&p.type)) {
    e.c.type = BITOSCOPE_PROPERTY_INTEGER;
    e.c.has_minimum = integer->minimum.has_value() ? 1 : 0;
    e.c.minimum = integer->minimum.value_or(0);
    e.c.has_maximum = integer->maximum.has_value() ? 1 : 0;
    e.c.maximum = integer->maximum.value_or(0);
    e.c.default_value = integer->defaultValue;
  } else if (const auto* decimal = std::get_if<DecimalType>(&p.type)) {
    e.c.type = BITOSCOPE_PROPERTY_DECIMAL;
    e.c.digits = decimal->digits;
    e.c.has_minimum = decimal->minimum.has_value() ? 1 : 0;
    e.c.minimum = decimal->minimum.value_or(0);
    e.c.has_maximum = decimal->maximum.has_value() ? 1 : 0;
    e.c.maximum = decimal->maximum.value_or(0);
    e.c.default_value = decimal->defaultValue;
  } else if (const auto* boolean = std::get_if<BooleanType>(&p.type)) {
    e.c.type = BITOSCOPE_PROPERTY_BOOLEAN;
    e.c.default_value = boolean->defaultValue ? 1 : 0;
    e.c.boolean_style = static_cast<int>(boolean->style);
  } else {
    const auto& enumeration = std::get<EnumerationType>(p.type);
    e.c.type = BITOSCOPE_PROPERTY_ENUMERATION;
    e.c.digits = enumeration.digits;
    e.c.default_index = enumeration.defaultIndex;
    for (const std::string& name : enumeration.names) e.names.push_back(name.c_str());
    e.c.item_names = e.names.empty() ? nullptr : e.names.data();
    e.c.item_numbers = enumeration.numbers.empty() ? nullptr : enumeration.numbers.data();
    e.c.num_items = !enumeration.names.empty() ? enumeration.names.size() : enumeration.numbers.size();
  }
}

// WHY: Check the tree BEFORE registering the facet: the SDK must never hold a facet whose C++ object was dropped.
std::expected<std::vector<Entry>, std::string> cEntries(const std::vector<PropertyNode>& tree) {
  std::vector<std::pair<const Property*, std::string>> collected;
  if (auto ok = collect(tree, "", collected); !ok) return std::unexpected(ok.error());
  std::vector<Entry> out(collected.size());
  for (size_t i = 0; i < collected.size(); ++i) {
    out[i].source = collected[i].first;
    out[i].group = std::move(collected[i].second);
    fill(out[i]);
  }
  std::vector<bitoscope::ipc::props::PropertyNode> scratch;
  for (const Entry& e : out) {
    const std::string where = "property \"" + e.source->id + "\": ";
    auto placed = bitoscope::plugin_c_sdk::propertyFrom(e.c);
    if (!placed) return std::unexpected(where + placed.error());
    if (auto added = bitoscope::plugin_c_sdk::addProperty(scratch, std::move(*placed)); !added) {
      return std::unexpected(where + added.error());
    }
  }
  return out;
}

bool addEntries(bitoscope_device* device, const char* facet, const std::vector<Entry>& entries) {
  for (const Entry& e : entries) {
    if (bitoscope_add_property(device, facet, &e.c) != BITOSCOPE_OK) return false;
  }
  return true;
}

LogicAnalyzer& analyzerOf(void* session) { return *static_cast<Session*>(session)->logicAnalyzer(); }

SignalGenerator& generatorOf(void* session) { return *static_cast<Session*>(session)->signalGenerator(); }

template <auto facetOf>
int applyTrampoline(void* session, const bitoscope_property_value* values, size_t count, int capturing,
                    bitoscope_reply* reply) {
  PropertyRefusal refusal;
  try {
    const std::vector<PropertyValue> all = toValues(values, count);
    auto applied = facetOf(session).applyProperties(all, capturing != 0);
    if (applied) return BITOSCOPE_OK;
    refusal = std::move(applied.error());
  } catch (const std::exception& e) {
    refusal.reason = std::string("applyProperties threw: ") + e.what();
  } catch (...) {
    refusal.reason = "applyProperties threw";
  }
  (void)bitoscope_reply_message(reply, refusal.reason.c_str());
  if (!refusal.propertyId.empty()) (void)bitoscope_reply_refused_property(reply, refusal.propertyId.c_str());
  return BITOSCOPE_ERROR;
}

bool sizeMatches(Sink& sink, std::span<const uint8_t> samples, uint64_t numSamples) {
  const uint64_t want = sink.sampleBytes(numSamples);
  if (samples.size() == want) return true;
  sink.fail("write of " + std::to_string(numSamples) + " samples with " + std::to_string(samples.size()) +
                   " bytes; they take " + std::to_string(want) + " bytes");
  return false;
}

void captureTrampoline(void* session, const bitoscope_capture* capture, bitoscope_sink* sink) {
  Sink s(sink);
  try {
    auto captured = analyzerOf(session).capture(toCapture(*capture), s);
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
    Sink s(sink);
    auto prepared = analyzerOf(session).prepare(toCapture(*capture), s);
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

namespace siggen = bitoscope::ipc::siggen;

Waveform toWaveform(const bitoscope_waveform& w) {
  const auto* bytes = static_cast<const uint8_t*>(w.params);
  return Waveform{w.name != nullptr ? w.name : "",
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

std::expected<SquareWave, std::string> decodeSquareWave(std::span<const uint8_t> params) {
  auto square = siggen::decodeSquare(params);
  if (!square) return std::unexpected(square.error().message);
  return SquareWave{square->frequencyMilliHz, square->dutyPercent};
}

bool Sink::write(std::span<const uint8_t> samples, uint64_t numSamples) {
  if (!sizeMatches(*this, samples, numSamples)) return false;
  return bitoscope_write_samples(sink_, samples.data(), numSamples) == BITOSCOPE_OK;
}

bool Sink::writeNow(std::span<const uint8_t> samples, uint64_t numSamples) {
  if (!sizeMatches(*this, samples, numSamples)) return false;
  return bitoscope_write_samples_now(sink_, samples.data(), numSamples) == BITOSCOPE_OK;
}

std::span<uint8_t> Sink::begin(uint64_t maxSamples, bool wait) {
  void* room = bitoscope_begin_samples(sink_, maxSamples, wait ? 1 : 0);
  if (room == nullptr) return {};
  return std::span<uint8_t>(static_cast<uint8_t*>(room), static_cast<size_t>(sampleBytes(maxSamples)));
}

bool Sink::commit(uint64_t numSamples) { return bitoscope_commit_samples(sink_, numSamples) == BITOSCOPE_OK; }

uint64_t Sink::maxChunkSamples() const { return bitoscope_max_chunk_samples(sink_); }

uint64_t Sink::sampleBytes(uint64_t numSamples) const { return bitoscope_sample_bytes(sink_, numSamples); }

uint64_t Sink::slotBytes() const { return bitoscope_slot_bytes(sink_); }

bool Sink::stopping() const { return bitoscope_stopping(sink_) != 0; }

bool Sink::failed() const { return bitoscope_capture_failed(sink_) != 0; }

void Sink::fail(std::string_view reason) { bitoscope_fail_capture(sink_, std::string(reason).c_str()); }

void Sink::deviceGone(std::string_view reason) { bitoscope_device_gone(sink_, std::string(reason).c_str()); }

bool Sink::setDeviceBytes(const DeviceBytes& bytes) {
  const bitoscope_device_bytes c{static_cast<int>(bytes.layout), bytes.channelMask, bytes.strideBytes};
  return bitoscope_set_device_bytes(sink_, &c) == BITOSCOPE_OK;
}

std::optional<RingFill> Sink::ringFill() const {
  RingFill fill;
  if (bitoscope_ring_fill(sink_, &fill.highWaterSlots, &fill.slotCount) != BITOSCOPE_OK) return std::nullopt;
  return fill;
}

std::expected<void, std::string> LogicAnalyzer::prepare(const Capture&, Sink&) { return {}; }

std::expected<void, PropertyRefusal> LogicAnalyzer::applyProperties(std::span<const PropertyValue>, bool) { return {}; }
std::expected<void, PropertyRefusal> SignalGenerator::applyProperties(std::span<const PropertyValue>, bool) { return {}; }

LogicAnalyzer* Session::logicAnalyzer() { return nullptr; }
SignalGenerator* Session::signalGenerator() { return nullptr; }

bool Device::setName(std::string_view name) {
  return bitoscope_set_name(device_, std::string(name).c_str()) == BITOSCOPE_OK;
}

bool Device::setSerialNumber(std::string_view serialNumber) {
  return bitoscope_set_serial_number(device_, std::string(serialNumber).c_str()) == BITOSCOPE_OK;
}

bool Device::add(const LogicAnalyzerInfo& analyzer) {
  auto entries = cEntries(analyzer.properties);
  if (!entries) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "logic analyzer was refused: its properties: %s", entries.error().c_str());
    return false;
  }
  std::vector<bitoscope_channel_preset> presets;
  for (const ChannelPreset& p : analyzer.channelPresets) presets.push_back({p.channel, p.name.c_str(), p.colorId.c_str()});
  bitoscope_logic_analyzer la{};
  la.num_channels = analyzer.numChannels;
  la.sample_rates_milli_hz = analyzer.sampleRatesMilliHz.data();
  la.num_sample_rates = analyzer.sampleRatesMilliHz.size();
  la.capture = &captureTrampoline;
  la.device_bytes = bitoscope_device_bytes{static_cast<int>(analyzer.deviceBytes.layout),
                                           analyzer.deviceBytes.channelMask, analyzer.deviceBytes.strideBytes};
  la.prepare = &prepareTrampoline;
  la.rate_ceilings_milli_hz = analyzer.rateCeilingsMilliHz.empty() ? nullptr : analyzer.rateCeilingsMilliHz.data();
  la.num_rate_ceilings = analyzer.rateCeilingsMilliHz.size();
  la.default_sample_rate_milli_hz = analyzer.defaultSampleRateMilliHz;
  la.channel_presets = presets.empty() ? nullptr : presets.data();
  la.num_channel_presets = presets.size();
  la.apply_properties = &applyTrampoline<analyzerOf>;
  la.rate_ceiling_reason = analyzer.rateCeilingReason.empty() ? nullptr : analyzer.rateCeilingReason.c_str();
  if (bitoscope_add_logic_analyzer(device_, &la) != BITOSCOPE_OK) return false;
  return addEntries(device_, "la.acquisition", *entries);
}

bool Device::add(const SignalGeneratorInfo& generator) {
  auto entries = cEntries(generator.properties);
  if (!entries) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "signal generator was refused: its properties: %s", entries.error().c_str());
    return false;
  }
  std::vector<const char*> waveforms;
  for (const std::string& name : generator.supportedWaveforms) waveforms.push_back(name.c_str());
  bitoscope_signal_generator described{};
  described.num_channels = generator.numChannels;
  described.start = &generatorStart;
  described.stop = &generatorStop;
  described.configure = &generatorConfigure;
  described.waveforms = waveforms.empty() ? nullptr : waveforms.data();
  described.num_waveforms = waveforms.size();
  described.min_frequency_milli_hz = generator.minFrequencyMilliHz;
  described.max_frequency_milli_hz = generator.maxFrequencyMilliHz;
  described.supports_synchronized_start = generator.supportsSynchronizedStart ? 1 : 0;
  described.apply_properties = &applyTrampoline<generatorOf>;
  if (bitoscope_add_signal_generator(device_, &described) != BITOSCOPE_OK) return false;
  return addEntries(device_, BITOSCOPE_SIGNAL_GENERATOR_FACET, *entries);
}

Transport FoundDevice::transport() const {
  if (usb() != nullptr) return Transport::usb;
  if (serial() != nullptr) return Transport::serial;
  if (network() != nullptr) return Transport::network;
  return Transport::none;
}

bool Progress::report(ProgressCode code, std::string_view detail) {
  return bitoscope_report_progress(progress_, static_cast<int>(code), std::string(detail).c_str()) == BITOSCOPE_OK;
}

namespace {

Connection connectionOf(const bitoscope_connection* c) {
  Connection out;
  if (c == nullptr) return out;
  if (c->network_address != nullptr) out.networkAddress = c->network_address;
  if (c->network_port != 0) out.networkPort = c->network_port;
  if (c->serial_port != nullptr) out.serialPort = c->serial_port;
  return out;
}

FoundDevice foundOf(const bitoscope_found_device& d) {
  FoundDevice out;
  out.id = d.id != nullptr ? d.id : "";
  out.connection = connectionOf(d.connection);
  if (d.appearance_key != nullptr) out.appearanceKey = d.appearance_key;
  if (d.transport == BITOSCOPE_TRANSPORT_USB && d.usb != nullptr) {
    UsbLocation usb{d.usb->vendor_id, d.usb->product_id, d.usb->interface_number, d.usb->bus, d.usb->address, {}};
    if (d.usb->ports != nullptr) usb.ports.assign(d.usb->ports, d.usb->ports + d.usb->num_ports);
    out.location = std::move(usb);
  } else if (d.transport == BITOSCOPE_TRANSPORT_SERIAL && d.serial != nullptr) {
    out.location = SerialLocation{d.serial->port != nullptr ? d.serial->port : ""};
  } else if (d.transport == BITOSCOPE_TRANSPORT_NETWORK && d.network != nullptr) {
    out.location = NetworkLocation{d.network->address != nullptr ? d.network->address : "", d.network->port};
  }
  return out;
}

int addEnumerated(bitoscope_found_devices* found, const EnumeratedDevice& device) {
  bitoscope_found_device given{};
  bitoscope_usb_location usb{};
  bitoscope_serial_location serial{};
  bitoscope_network_location network{};
  given.id = device.id.c_str();
  given.appearance_key = device.appearanceKey.empty() ? nullptr : device.appearanceKey.c_str();
  if (const auto* usbPlace = std::get_if<UsbLocation>(&device.location)) {
    usb = bitoscope_usb_location{usbPlace->vendorId, usbPlace->productId, usbPlace->interfaceNumber, usbPlace->bus,
                                 usbPlace->address,  usbPlace->ports.empty() ? nullptr : usbPlace->ports.data(),
                                 usbPlace->ports.size()};
    given.transport = BITOSCOPE_TRANSPORT_USB;
    given.usb = &usb;
  } else if (const auto* serialPlace = std::get_if<SerialLocation>(&device.location)) {
    serial.port = serialPlace->port.c_str();
    given.transport = BITOSCOPE_TRANSPORT_SERIAL;
    given.serial = &serial;
  } else if (const auto* networkPlace = std::get_if<NetworkLocation>(&device.location)) {
    network = bitoscope_network_location{networkPlace->address.c_str(), networkPlace->port};
    given.transport = BITOSCOPE_TRANSPORT_NETWORK;
    given.network = &network;
  }
  return bitoscope_add_located_device(found, &given);
}

int statusOf(DeviceProblem problem) {
  switch (problem) {
    case DeviceProblem::busy:
      return BITOSCOPE_BUSY;
    case DeviceProblem::noPermission:
      return BITOSCOPE_NO_PERMISSION;
    case DeviceProblem::firmwareLoading:
      return BITOSCOPE_FIRMWARE_LOADING;
    case DeviceProblem::notSupported:
      return BITOSCOPE_NOT_SUPPORTED;
    case DeviceProblem::other:
      break;
  }
  return BITOSCOPE_ERROR;
}

constexpr const char* kNoOpen =
    "driver description has no open function: it returns session that runs device's facets";

bool hasFacet(const bitoscope_device& device, std::string_view name) {
  return std::any_of(device.facets.begin(), device.facets.end(),
                     [&](const bitoscope::plugin_c_sdk::FacetRegistration& r) { return r.name == name; });
}

std::string missingFacetOf(const bitoscope_device& device, Session& session) {
  if (hasFacet(device, "la.acquisition") && session.logicAnalyzer() == nullptr) return "logic analyzer";
  if (hasFacet(device, BITOSCOPE_SIGNAL_GENERATOR_FACET) && session.signalGenerator() == nullptr) {
    return "signal generator";
  }
  return {};
}

template <typename Call>
int deviceCall(bitoscope_reply* reply, std::string_view function, Call&& call) {
  DeviceFailure failure;
  try {
    std::expected<void, DeviceFailure> done = call();
    if (done) return BITOSCOPE_OK;
    failure = std::move(done.error());
  } catch (const std::exception& e) {
    failure.detail = std::string(function) + " threw: " + e.what();
  } catch (...) {
    failure.detail = std::string(function) + " threw";
  }
  if (!failure.detail.empty()) (void)bitoscope_reply_message(reply, failure.detail.c_str());
  return statusOf(failure.problem);
}

}

Driver::Driver(DriverDescription description) : description_(std::move(description)) {
  for (const UsbId& id : description_.usbIds) {
    usbIds_.push_back(bitoscope_usb_id{id.vendorId, id.productId, id.interfaceNumber,
                                       id.genericName.empty() ? nullptr : id.genericName.c_str()});
  }
  driver_.display_name = description_.displayName.c_str();
  driver_.version = description_.version.c_str();
  driver_.transport = static_cast<bitoscope_transport>(description_.transport);
  driver_.usb_ids = usbIds_.empty() ? nullptr : usbIds_.data();
  driver_.num_usb_ids = usbIds_.size();
  driver_.enumerate = description_.enumerate ? &Driver::enumerate : nullptr;
  driver_.identify = description_.identify ? &Driver::identify : nullptr;
  driver_.user = this;
  driver_.open = &Driver::open;
  driver_.close = &Driver::close;
  driver_.held_check = description_.heldCheck ? &Driver::heldCheck : nullptr;
}

int Driver::enumerate(void* user, const bitoscope_connection* connection, bitoscope_found_devices* found) {
  auto* self = static_cast<Driver*>(user);
  try {
    auto ids = self->description_.enumerate(connectionOf(connection));
    if (!ids) {
      bitoscope_log(BITOSCOPE_LOG_ERROR, "devices could not be looked for: %s", ids.error().c_str());
      return BITOSCOPE_ERROR;
    }
    for (const EnumeratedDevice& device : *ids) {
      if (addEnumerated(found, device) != BITOSCOPE_OK) return BITOSCOPE_ERROR;
    }
    return BITOSCOPE_OK;
  } catch (const std::exception& e) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "enumerate threw: %s", e.what());
  } catch (...) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "enumerate threw");
  }
  return BITOSCOPE_ERROR;
}

int Driver::identify(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                     bitoscope_reply* reply) {
  auto* self = static_cast<Driver*>(user);
  Device described(device);
  return deviceCall(reply, "identify", [&] { return self->description_.identify(foundOf(*found), described); });
}

int Driver::open(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                 bitoscope_progress* progress, void** session, bitoscope_reply* reply) {
  auto* self = static_cast<Driver*>(user);
  Device described(device);
  Progress reports(progress);
  return deviceCall(reply, "open", [&]() -> std::expected<void, DeviceFailure> {
    if (!self->description_.open) return std::unexpected(DeviceFailure{DeviceProblem::other, kNoOpen});
    auto opened = self->description_.open(foundOf(*found), described, reports);
    if (!opened) return std::unexpected(std::move(opened.error()));
    if (*opened == nullptr) return std::unexpected(DeviceFailure{DeviceProblem::other, "open gave no session"});
    if (auto missing = missingFacetOf(*device, **opened); !missing.empty()) {
      return std::unexpected(DeviceFailure{DeviceProblem::other, "open gave session without " + missing +
                                                                     ", though identify described one"});
    }
    *session = opened->release();
    return {};
  });
}

void Driver::close([[maybe_unused]] void* user, void* session) { delete static_cast<Session*>(session); }

int Driver::heldCheck(void* user, const bitoscope_found_device* device) {
  try {
    return static_cast<Driver*>(user)->description_.heldCheck(foundOf(*device)) ? BITOSCOPE_BUSY : BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_OK;
  }
}

int Driver::run(int argc, char** argv) {
  if (!description_.open) {
    std::fprintf(stderr, "bitoscope driver: %s\n", kNoOpen);
    return 1;
  }
  return bitoscope_run_driver(argc, argv, &driver_);
}

int runDriver(int argc, char** argv, DriverDescription description) {
  Driver driver(std::move(description));
  return driver.run(argc, argv);
}

}

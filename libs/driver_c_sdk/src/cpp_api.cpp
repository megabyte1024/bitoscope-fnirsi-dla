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

#include "cpp_twins.h"
#include "device_location.h"
#include "driver_model.h"
#include "facet_properties.h"
#include "ipc_signal_generator.h"
#include "plugin_model.h"

namespace bitoscope::plugin {

namespace {

bool sizeMatches(Sink& sink, std::span<const uint8_t> samples, uint64_t numSamples) {
  const uint64_t want = sink.sampleBytes(numSamples);
  if (samples.size() == want) return true;
  sink.fail("write of " + std::to_string(numSamples) + " samples with " + std::to_string(samples.size()) +
                   " bytes; they take " + std::to_string(want) + " bytes");
  return false;
}

namespace siggen = bitoscope::ipc::siggen;

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

std::expected<void, PropertyRefusal> LogicAnalyzer::applyDuringCapture(std::span<const PropertyValue>) { return {}; }
std::expected<void, PropertyRefusal> SignalGenerator::applyProperties(std::span<const PropertyValue>, bool) { return {}; }

LogicAnalyzer* Session::logicAnalyzer() { return nullptr; }
SignalGenerator* Session::signalGenerator() { return nullptr; }

bool Device::setName(std::string_view name) {
  return bitoscope_set_name(device_, std::string(name).c_str()) == BITOSCOPE_OK;
}

bool Device::setSerialNumber(std::string_view serialNumber) {
  return bitoscope_set_serial_number(device_, std::string(serialNumber).c_str()) == BITOSCOPE_OK;
}

namespace {

std::expected<void, std::string> refusalOf(int status, const bitoscope_reply& reply) {
  if (status == BITOSCOPE_OK) return {};
  return std::unexpected(reply.message);
}

}

std::expected<void, std::string> Device::add(const LogicAnalyzerInfo& analyzer) {
  const plugin_c_sdk::CLogicAnalyzer twin(analyzer);
  if (!twin.error().empty()) {
    std::string why = "logic analyzer was refused: its properties: " + twin.error();
    bitoscope_log(BITOSCOPE_LOG_ERROR, "%s", why.c_str());
    return std::unexpected(std::move(why));
  }
  bitoscope_reply reply;
  return refusalOf(bitoscope_add_logic_analyzer(device_, &twin.get(), &reply), reply);
}

std::expected<void, std::string> Device::add(const SignalGeneratorInfo& generator) {
  const plugin_c_sdk::CSignalGenerator twin(generator);
  if (!twin.error().empty()) {
    std::string why = "signal generator was refused: its properties: " + twin.error();
    bitoscope_log(BITOSCOPE_LOG_ERROR, "%s", why.c_str());
    return std::unexpected(std::move(why));
  }
  bitoscope_reply reply;
  return refusalOf(bitoscope_add_signal_generator(device_, &twin.get(), &reply), reply);
}

namespace {

int64_t valueOf(const Capture& capture, std::string_view id, auto field) {
  const auto it = capture.properties.find(id);
  if (it == capture.properties.end()) {
    plugin_c_sdk::logMissingProperty(id);
    return 0;
  }
  return static_cast<int64_t>(it->second.*field);
}

}

int64_t Capture::integer(std::string_view id) const { return valueOf(*this, id, &PropertyValue::integer); }

bool Capture::boolean(std::string_view id) const { return valueOf(*this, id, &PropertyValue::boolean) != 0; }

uint32_t Capture::enumIndex(std::string_view id) const {
  return static_cast<uint32_t>(valueOf(*this, id, &PropertyValue::enumIndex));
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

int addEnumerated(bitoscope_found_devices* found, const EnumeratedDevice& device) {
  const plugin_c_sdk::CFoundDevice given(device.id, device.location, device.appearanceKey, nullptr);
  return bitoscope_add_located_device(found, &given.get());
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
  if (hasFacet(device, plugin_c_sdk::kLaFacetName) && session.logicAnalyzer() == nullptr) return "logic analyzer";
  if (hasFacet(device, siggen::kFacetName) && session.signalGenerator() == nullptr) {
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
    auto ids = self->description_.enumerate(plugin_c_sdk::cppConnection(connection));
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
  return deviceCall(reply, "identify", [&] { return self->description_.identify(plugin_c_sdk::cppFoundDevice(*found), described); });
}

int Driver::open(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                 bitoscope_progress* progress, void** session, bitoscope_reply* reply) {
  auto* self = static_cast<Driver*>(user);
  Device described(device);
  Progress reports(progress);
  return deviceCall(reply, "open", [&]() -> std::expected<void, DeviceFailure> {
    if (!self->description_.open) return std::unexpected(DeviceFailure{DeviceProblem::other, kNoOpen});
    auto opened = self->description_.open(plugin_c_sdk::cppFoundDevice(*found), described, reports);
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
    return static_cast<Driver*>(user)->description_.heldCheck(plugin_c_sdk::cppFoundDevice(*device)) ? BITOSCOPE_BUSY : BITOSCOPE_OK;
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

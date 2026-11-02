/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_driver.h"

#include <format>
#include <thread>
#include <utility>

#include "dla_model.h"
#include "fnirsi_protocol.h"

namespace fnirsi {

namespace plugin = bitoscope::plugin;

namespace {

plugin::DeviceFailure notServed(std::string_view device_name) {
  return {plugin::DeviceProblem::notSupported, dlaDisplayName(device_name) + " is not supported yet; driver serves " +
                                                   std::string(kDla16Plus.display_name) + " only"};
}

void sleepFor(std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); }

}

DlaSession::DlaSession(std::shared_ptr<SampleStreamDevice> device, std::string name, FirmwareUpdateSetup update)
    : device_(std::move(device)),
      name_(std::move(name)),
      gate_(std::make_shared<UpdateGate>()),
      gated_(std::make_shared<GatedDevice>(device_, gate_)),
      analyzer_(gated_),
      generator_(gated_),
      update_(std::move(update), gate_) {}

DlaSession::~DlaSession() { bitoscope_log(BITOSCOPE_LOG_INFO, "closing %s", name_.c_str()); }

plugin::LogicAnalyzer* DlaSession::logicAnalyzer() { return &analyzer_; }

plugin::SignalGenerator* DlaSession::signalGenerator() { return &generator_; }

std::expected<plugin::CommandAnswer, std::string> DlaSession::command(std::string_view facet,
                                                                      std::span<const uint8_t> payload) {
  if (facet != bitoscope::ipc::firmware::kFacetName) return Session::command(facet, payload);
  return update_.command(payload);
}

DlaBootloaderSession::DlaBootloaderSession(std::shared_ptr<SampleStreamDevice> device, std::string name,
                                           FirmwareUpdateSetup update)
    : device_(std::move(device)),
      name_(std::move(name)),
      gate_(std::make_shared<UpdateGate>()),
      update_(std::move(update), gate_) {}

std::expected<plugin::CommandAnswer, std::string> DlaBootloaderSession::command(std::string_view facet,
                                                                                std::span<const uint8_t> payload) {
  if (facet != bitoscope::ipc::firmware::kFacetName) return Session::command(facet, payload);
  return update_.command(payload);
}

DlaBootloaderSession::~DlaBootloaderSession() { bitoscope_log(BITOSCOPE_LOG_INFO, "closing %s", name_.c_str()); }

DlaDriver::DlaDriver(DlaUsb& usb, std::chrono::milliseconds open_retry_delay, OpenRetrySleep sleep,
                     FirmwareUpdateTuning update_tuning)
    : usb_(usb),
      open_retry_delay_(open_retry_delay),
      sleep_(sleep ? std::move(sleep) : OpenRetrySleep(&sleepFor)),
      update_tuning_(std::move(update_tuning)) {}

plugin::DriverDescription DlaDriver::description() {
  plugin::DriverDescription description;
  description.displayName = std::string(kDlaFamilyName);
  description.version = std::string(kDlaDriverVersion);
  description.transport = plugin::Transport::usb;
  description.usbIds = {plugin::UsbId{kVendorId, kProductId, kInterfaceNumber, std::string(kDlaFamilyName)}};
  description.identify = [this](const plugin::FoundDevice& unit, plugin::Device& device) {
    return identify(unit, device);
  };
  description.open = [this](const plugin::FoundDevice& unit, plugin::Device&, plugin::Progress&) {
    return open(unit);
  };
  return description;
}

std::expected<void, plugin::DeviceFailure> DlaDriver::identify(const plugin::FoundDevice& unit,
                                                               plugin::Device& device) {
  auto identity = usb_.readIdentity(unit);
  if (!identity) return std::unexpected(std::move(identity.error()));
  if (identity->in_bootloader) {
    (void)device.setName(kDlaBootloaderDisplayName);
    if (auto added = device.add(firmwareUpdateFacetInfo(true, {})); !added) {
      return std::unexpected(plugin::DeviceFailure{plugin::DeviceProblem::other, std::move(added.error())});
    }
    return {};
  }
  const std::string& device_name = identity->device_name;
  if (!isServedModel(device_name)) return std::unexpected(notServed(device_name));
  const auto refused = [](std::string why) {
    return std::unexpected(plugin::DeviceFailure{plugin::DeviceProblem::other, std::move(why)});
  };
  (void)device.setName(dlaDisplayName(device_name));
  if (auto added = device.add(dlaAnalyzerInfo()); !added) return refused(std::move(added.error()));
  if (auto added = device.add(dlaGeneratorInfo()); !added) return refused(std::move(added.error()));
  if (auto added = device.add(firmwareUpdateFacetInfo(false, device_name)); !added) {
    return refused(std::move(added.error()));
  }
  return {};
}

std::expected<std::unique_ptr<plugin::Session>, plugin::DeviceFailure> DlaDriver::open(const plugin::FoundDevice& unit) {
  auto opened = usb_.open(unit);
  // WHY: Retry other failures only: SDK retries busy itself, and missing permission never comes back by waiting.
  if (!opened && opened.error().problem == plugin::DeviceProblem::other) {
    bitoscope_log(BITOSCOPE_LOG_WARNING, "failed to open DLA %s: %s; trying once more in %lld ms", unit.id.c_str(),
                  opened.error().detail.c_str(), static_cast<long long>(open_retry_delay_.count()));
    sleep_(open_retry_delay_);
    opened = usb_.open(unit);
  }
  if (!opened) return std::unexpected(std::move(opened.error()));
  if (opened->in_bootloader) {
    const std::string name = std::format("{} {}", kDlaBootloaderDisplayName, unit.id);
    bitoscope_log(BITOSCOPE_LOG_INFO, "connected to %s (%s)", name.c_str(), opened->description.c_str());
    FirmwareUpdateSetup update{true, {}, std::move(opened->update_transport), update_tuning_};
    return std::make_unique<DlaBootloaderSession>(std::move(opened->device), name, std::move(update));
  }
  if (!isServedModel(opened->device_name)) return std::unexpected(notServed(opened->device_name));
  const std::string name = dlaDisplayName(opened->device_name) + " " + unit.id;
  bitoscope_log(BITOSCOPE_LOG_INFO, "connected to %s (%s)", name.c_str(), opened->description.c_str());
  FirmwareUpdateSetup update{false, opened->device_name, std::move(opened->update_transport), update_tuning_};
  return std::make_unique<DlaSession>(std::move(opened->device), name, std::move(update));
}

}

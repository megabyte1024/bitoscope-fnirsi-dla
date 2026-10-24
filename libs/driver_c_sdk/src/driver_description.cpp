/*
 * SPDX-License-Identifier: MIT
 */

#include "driver_description.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "driver_source.h"
#include "identity_cache.h"
#include "plugin_model.h"
#include "plugin_version.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

namespace {

bool empty(const char* text) { return text == nullptr || text[0] == '\0'; }

std::optional<InterfaceKind> sessionKind(bitoscope_transport transport) {
  switch (transport) {
    case BITOSCOPE_TRANSPORT_NONE:
      return InterfaceKind::none;
    case BITOSCOPE_TRANSPORT_USB:
      return InterfaceKind::usb;
    case BITOSCOPE_TRANSPORT_SERIAL:
      return InterfaceKind::serial;
    case BITOSCOPE_TRANSPORT_NETWORK:
      return InterfaceKind::network;
  }
  return std::nullopt;
}

std::string failureMessage(DriverCall call, const std::string& id, const Attempt& failed,
                           std::chrono::milliseconds retryDelay) {
  const std::string device = "device \"" + id + "\"";
  std::string what;
  switch (failed.status) {
    case BITOSCOPE_BUSY:
      what = device + " is in use by another program (asked twice, " + std::to_string(retryDelay.count()) +
             " ms apart)";
      break;
    case BITOSCOPE_NO_PERMISSION:
      what = "no permission to use " + device;
      break;
    case BITOSCOPE_FIRMWARE_LOADING:
      what = device + " is loading its firmware";
      break;
    case BITOSCOPE_NOT_SUPPORTED:
      what = device + " is not supported by this driver";
      break;
    default:
      what = device + " could not be " + (call == DriverCall::open ? "opened" : "identified");
      if (failed.text.empty()) {
        what += " (driver's " + functionName(call) + " function returned " + std::to_string(failed.status) + ")";
      }
      break;
  }
  return failed.text.empty() ? what : what + ": " + failed.text;
}

std::string checkRefusal(const std::string& id, const AvailabilityCheck& checked,
                         std::chrono::milliseconds retryDelay) {
  const std::string device = "device \"" + id + "\"";
  std::string what;
  switch (checked.availability) {
    case Availability::busy:
      what = device + " is in use by another program (asked twice, " + std::to_string(retryDelay.count()) +
             " ms apart)";
      break;
    case Availability::noPermission:
      what = "no permission to use " + device;
      break;
    case Availability::free:
    case Availability::other:
      what = device + " cannot be used";
      break;
  }
  return checked.detail.empty() ? what : what + ": " + checked.detail;
}

void sleepFor(const DriverRunOptions& options, std::chrono::milliseconds delay) {
  if (options.sleep) {
    options.sleep(delay);
  } else {
    std::this_thread::sleep_for(delay);
  }
}

AvailabilityCheck checkBeforeDriver(DeviceSource& source, const SourceDevice& device,
                                    const DriverRunOptions& options) {
  AvailabilityCheck checked = source.check(device);
  if (checked.availability != Availability::busy) return checked;
  sleepFor(options, options.busyRetryDelay);
  return source.check(device);
}

class CloseAtExit {
 public:
  CloseAtExit(const bitoscope_driver& driver, void* session, bool held)
      : driver_(driver), session_(session), held_(held && driver.close != nullptr) {}
  ~CloseAtExit() {
    if (!held_) return;
    try {
      driver_.close(driver_.user, session_);
    } catch (...) {
      logLine(ipc::LogLevel::error, "driver's close function threw");
    }
  }
  CloseAtExit(const CloseAtExit&) = delete;
  CloseAtExit& operator=(const CloseAtExit&) = delete;

 private:
  const bitoscope_driver& driver_;
  void* session_;
  bool held_;
};

struct Identified {
  Attempt result;
  std::unique_ptr<DeviceDescription> description;
};

Identified identifyAsked(const bitoscope_driver& driver, const bitoscope_found_device& found,
                         const DriverRunOptions& options) {
  Identified identified;
  for (int asked = 1;; ++asked) {
    identified.description = std::make_unique<DeviceDescription>(found.id);
    identified.result = callIdentify(driver, found, identified.description->device());
    if (identified.result.status != BITOSCOPE_BUSY || asked == 2) return identified;
    sleepFor(options, options.busyRetryDelay);
  }
}

Attempt openAsked(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device,
                  FrameOutput& output, void*& session, const DriverRunOptions& options) {
  for (int asked = 1;; ++asked) {
    session = driver.user;
    Attempt result = callOpen(driver, found, device, output, session);
    if (result.status != BITOSCOPE_BUSY || asked == 2) return result;
    sleepFor(options, options.busyRetryDelay);
  }
}

std::optional<SourceDevice> presentAs(DeviceSource& source, const std::string& id) {
  auto found = source.find();
  if (!found) return std::nullopt;
  const auto device = std::find_if(found->begin(), found->end(), [&](const SourceDevice& d) { return d.id == id; });
  if (device == found->end()) return std::nullopt;
  return *device;
}

std::optional<SourceDevice> enumeratedAgain(DeviceSource& source, const SourceDevice& before,
                                            const DriverRunOptions& options) {
  bool wasAbsent = false;
  for (auto waited = std::chrono::milliseconds{0}; waited < kReenumerationLimit; waited += kReenumerationPoll) {
    sleepFor(options, kReenumerationPoll);
    const std::optional<SourceDevice> now = presentAs(source, before.id);
    if (!now) {
      wasAbsent = true;
      continue;
    }
    if (!wasAbsent && (before.appearanceKey.empty() || now->appearanceKey == before.appearanceKey)) continue;
    sleepFor(options, options.watchTiming.settleAfterLastNotice);
    if (std::optional<SourceDevice> settled = presentAs(source, before.id)) return settled;
    wasAbsent = true;
  }
  return std::nullopt;
}

bitoscope_connection connectionOf(const RunMode& mode, bitoscope_transport transport) {
  bitoscope_connection connection{};
  if (transport == BITOSCOPE_TRANSPORT_NETWORK) {
    connection.network_address = mode.networkAddress ? mode.networkAddress->c_str() : nullptr;
    connection.network_port = mode.networkPort.value_or(0);
  }
  if (transport == BITOSCOPE_TRANSPORT_SERIAL) {
    connection.serial_port = mode.serialPort ? mode.serialPort->c_str() : nullptr;
  }
  return connection;
}

std::string idsOf(const std::vector<SourceDevice>& found) {
  std::string ids;
  for (const SourceDevice& d : found) ids += (ids.empty() ? "" : ", ") + d.id;
  return ids.empty() ? std::string("none") : ids;
}

int serveRefusal(ipc::ByteSource& in, FrameOutput& output, ServedDevice served, const SessionOptions& session) {
  logLine(ipc::LogLevel::error, served.refusal);
  return serveDevice(in, output, served, session);
}

int runDevice(ipc::ByteSource& in, FrameOutput& output, const bitoscope_driver& driver, const RunMode& mode,
              const DriverRunOptions& options) {
  SessionOptions session;
  session.minProtocolMajor = options.minProtocolMajor;
  session.maxProtocolMajor = options.maxProtocolMajor;
  session.interfaceKind = *sessionKind(driver.transport);
  session.pluginVersion = driver.version;

  const bitoscope_connection connection = connectionOf(mode, driver.transport);
  ServedDevice served;
  const std::unique_ptr<DeviceSource> source = deviceSourceFor(driver, connection, options);
  const auto found = source->find();
  if (!found) {
    served.refusal = found.error();
    return serveRefusal(in, output, std::move(served), session);
  }
  const auto selected =
      std::find_if(found->begin(), found->end(), [&](const SourceDevice& d) { return d.id == mode.deviceId; });
  if (selected == found->end()) {
    served.refusal = "no device with id \"" + mode.deviceId + "\": this driver found " + idsOf(*found);
    return serveRefusal(in, output, std::move(served), session);
  }

  if (const AvailabilityCheck checked = checkBeforeDriver(*source, *selected, options);
      checked.availability != Availability::free) {
    served.refusal = checkRefusal(selected->id, checked, options.busyRetryDelay);
    served.unavailable = reasonOfCheck(checked.availability);
    return serveRefusal(in, output, std::move(served), session);
  }

  SourceDevice device = *selected;
  auto given = std::make_unique<GivenDevice>(device, connection);
  Identified identified = identifyAsked(driver, given->get(), options);
  if (identified.result.status == BITOSCOPE_FIRMWARE_LOADING) {
    bitoscope_progress progress{&output};
    const std::string about = "device \"" + device.id + "\"";
    (void)reportProgress(progress, BITOSCOPE_PROGRESS_FIRMWARE_LOADING,
                         identified.result.text.empty() ? about + " loads its firmware" : identified.result.text);
    (void)reportProgress(progress, BITOSCOPE_PROGRESS_WAITING_FOR_DEVICE, about + " enumerates again");
    std::optional<SourceDevice> back = enumeratedAgain(*source, device, options);
    if (!back) {
      served.refusal = failureMessage(DriverCall::identify, device.id, identified.result, options.busyRetryDelay) +
                       "; it did not enumerate again within " + std::to_string(kReenumerationLimit.count()) + " ms";
      served.unavailable = ipc::UnavailableReason::firmwareLoading;
      return serveRefusal(in, output, std::move(served), session);
    }
    device = std::move(*back);
    if (const AvailabilityCheck checked = checkBeforeDriver(*source, device, options);
        checked.availability != Availability::free) {
      served.refusal = checkRefusal(device.id, checked, options.busyRetryDelay);
      served.unavailable = reasonOfCheck(checked.availability);
      return serveRefusal(in, output, std::move(served), session);
    }
    given = std::make_unique<GivenDevice>(device, connection);
    identified = identifyAsked(driver, given->get(), options);
  }
  if (identified.result.status != BITOSCOPE_OK) {
    served.refusal = failureMessage(DriverCall::identify, device.id, identified.result, options.busyRetryDelay);
    served.unavailable = reasonOf(identified.result.status);
    return serveRefusal(in, output, std::move(served), session);
  }

  bitoscope_device& described = identified.description->device();
  described.facetsFixed = true;
  void* sessionContext = driver.user;
  const bool opens = driver.open != nullptr;
  const Attempt opened =
      opens ? openAsked(driver, given->get(), described, output, sessionContext, options) : Attempt{BITOSCOPE_OK, {}};
  const CloseAtExit closer(driver, sessionContext, opens && opened.status == BITOSCOPE_OK);
  if (opened.status != BITOSCOPE_OK) {
    served.refusal = failureMessage(DriverCall::open, device.id, opened, options.busyRetryDelay);
    served.unavailable = reasonOf(opened.status);
    return serveRefusal(in, output, std::move(served), session);
  }
  if (described.name.empty()) described.name = genericNameOf(driver, device);
  described.session = sessionContext;
  served.device = &described;
  served.identified = true;
  return serveDevice(in, output, served, session);
}

int runListing(ipc::ByteSource& in, FrameOutput& output, const bitoscope_driver& driver, const RunMode& mode,
               const DriverRunOptions& options) {
  const bitoscope_connection connection = connectionOf(mode, driver.transport);
  WatchClock& clock = options.clock != nullptr ? *options.clock : steadyWatchClock();
  const std::unique_ptr<DeviceSource> source = deviceSourceFor(driver, connection, options);
  IdentityCache cache(driver, connection, *source);
  const Relist listNow = [&]() -> std::expected<std::vector<ipc::DeviceInfo>, std::string> {
    auto found = source->find();
    if (!found) return std::unexpected(std::move(found.error()));
    return cache.list(*found, clock.now());
  };
  auto initial = listNow();
  if (!initial) {
    logLine(ipc::LogLevel::error, initial.error());
    return kExitFailed;
  }
  std::unique_ptr<ChangeNotices> changes = source->changeNotices();
  if (mode.kind == RunMode::Kind::list) {
    ipc::DeviceList listed{std::move(*initial), changes != nullptr, driver.display_name, driver.version};
    if (auto sent = output.send(ipc::Envelope{0, std::move(listed)}); !sent) {
      logLine(ipc::LogLevel::error, "device list could not be written: " + sent.error().message);
      return kExitFailed;
    }
    return kExitOk;
  }
  if (!changes) {
    logLine(ipc::LogLevel::error,
            "this driver cannot watch for devices on this system: driver SDK has no device watcher here yet "
            "(only on Windows and Linux)");
    return kExitFailed;
  }
  return runWatch(in, output, std::move(*initial), listNow, *changes, options.watchTiming,
                  WatchReport{driver.display_name, driver.version}, clock);
}

}

std::unique_ptr<DeviceSource> deviceSourceFor(const bitoscope_driver& driver, const bitoscope_connection& connection,
                                              const DriverRunOptions& options) {
  std::unique_ptr<DeviceSource> source;
  if (options.deviceSource) {
    source = options.deviceSource(driver, connection);
  } else if (driver.num_usb_ids > 0) {
    source = makeUsbSource(std::span<const bitoscope_usb_id>(driver.usb_ids, driver.num_usb_ids),
                           UsbSourceSeams{options.usbDevices, options.usbCheck, options.usbBus, options.changeSource});
  } else {
    source = makeEnumerateSource(driver, connection);
  }
  return withHeldCheck(std::move(source), driver, connection);
}

std::expected<void, std::string> checkDriverDescription(const bitoscope_driver& driver) {
  if (empty(driver.display_name)) return std::unexpected(std::string("driver description has no display_name"));
  if (empty(driver.version)) return std::unexpected(std::string("driver description has no version"));
  if (auto parsed = ipc::parsePluginVersion(driver.version); !parsed) {
    return std::unexpected("driver description's " + parsed.error());
  }
  if (!sessionKind(driver.transport)) {
    return std::unexpected("driver description's transport " + std::to_string(static_cast<int>(driver.transport)) +
                           " is none of BITOSCOPE_TRANSPORT_NONE, _USB, _SERIAL, _NETWORK");
  }
  const bool byUsb = driver.num_usb_ids > 0;
  if (byUsb && driver.transport != BITOSCOPE_TRANSPORT_USB) {
    return std::unexpected(std::string("driver description gives usb_ids, so its transport is "
                                       "BITOSCOPE_TRANSPORT_USB"));
  }
  if (byUsb && driver.usb_ids == nullptr) {
    return std::unexpected("driver description counts " + std::to_string(driver.num_usb_ids) +
                           " usb_ids but gives none");
  }
  if (!byUsb && driver.enumerate == nullptr) {
    return std::unexpected(std::string("driver description has no way to find devices: give usb_ids or enumerate"));
  }
  if (byUsb && driver.enumerate != nullptr) {
    return std::unexpected(std::string("driver description gives both usb_ids and enumerate: give usb_ids or "
                                       "enumerate, not both"));
  }
  if (driver.identify == nullptr) return std::unexpected(std::string("driver description has no identify function"));
  return {};
}

std::expected<void, std::string> reportProgress(bitoscope_progress& progress, int code, std::string_view detail) {
  ipc::OpenProgressCode sent = ipc::OpenProgressCode::other;
  switch (code) {
    case BITOSCOPE_PROGRESS_OTHER:
      sent = ipc::OpenProgressCode::other;
      break;
    case BITOSCOPE_PROGRESS_FIRMWARE_LOADING:
      sent = ipc::OpenProgressCode::firmwareLoading;
      break;
    case BITOSCOPE_PROGRESS_WAITING_FOR_DEVICE:
      sent = ipc::OpenProgressCode::waitingForDevice;
      break;
    default:
      return std::unexpected("progress code " + std::to_string(code) +
                             " is none of BITOSCOPE_PROGRESS_OTHER, _FIRMWARE_LOADING, _WAITING_FOR_DEVICE");
  }
  auto written = progress.output->send(ipc::Envelope{0, ipc::OpenProgress{sent, std::string(detail)}});
  if (!written) return std::unexpected("progress could not be sent: " + written.error().message);
  return {};
}

int runDriver(std::span<const std::string> args, ipc::ByteSource& in, ipc::ByteSink& out,
              const bitoscope_driver& driver, const DriverRunOptions& options) {
  if (auto checked = checkDriverDescription(driver); !checked) {
    logLine(ipc::LogLevel::error, "bitoscope_run_driver(): " + checked.error());
    return kExitFailed;
  }
  const auto mode = parseRunMode(args);
  if (!mode) {
    printUsage();
    return kExitUsage;
  }
  if (mode->kind == RunMode::Kind::interface) return answerInterface(out, *sessionKind(driver.transport));
  FrameOutput output(out);
  ActiveOutput active(output);
  if (!active.held()) {
    logLine(ipc::LogLevel::error,
            "another plugin SDK session is already active in this process (one SDK loop per process)");
    return kExitFailed;
  }
  if (mode->kind == RunMode::Kind::device) return runDevice(in, output, driver, *mode, options);
  return runListing(in, output, driver, *mode, options);
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "driver_calls.h"

#include <exception>
#include <memory>
#include <utility>

#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

namespace {

template <typename Function>
Attempt attempt(DriverCall call, Function&& function) {
  bitoscope_reply reply;
  Attempt result;
  try {
    result.status = function(reply);
  } catch (const std::exception& e) {
    result.status = BITOSCOPE_ERROR;
    reply.message = functionName(call) + " function threw: " + e.what();
  } catch (...) {
    result.status = BITOSCOPE_ERROR;
    reply.message = functionName(call) + " function threw";
  }
  result.text = std::move(reply.message);
  return result;
}

}

std::string functionName(DriverCall call) { return call == DriverCall::open ? "open" : "identify"; }

std::string genericNameOf(const bitoscope_driver& driver, const SourceDevice& device) {
  return device.genericName.empty() ? std::string(driver.display_name) : device.genericName;
}

DeviceDescription::DeviceDescription(const std::string& id) {
  devices_.devices.push_back(std::make_unique<bitoscope_device>());
  devices_.devices.front()->id = id;
}

ipc::DeviceInfo DeviceDescription::info(const std::string& genericName) const {
  ipc::DeviceInfo described = describeDevices(devices_).front();
  if (described.name.empty()) described.name = genericName;
  return described;
}

Attempt callIdentify(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device) {
  return attempt(DriverCall::identify,
                 [&](bitoscope_reply& reply) { return driver.identify(driver.user, &found, &device, &reply); });
}

Attempt callOpen(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device,
                 FrameOutput& output, void*& session) {
  bitoscope_progress progress{&output};
  return attempt(DriverCall::open, [&](bitoscope_reply& reply) {
    return driver.open(driver.user, &found, &device, &progress, &session, &reply);
  });
}

ipc::UnavailableReason reasonOf(int status) {
  switch (status) {
    case BITOSCOPE_BUSY:
      return ipc::UnavailableReason::busy;
    case BITOSCOPE_NO_PERMISSION:
      return ipc::UnavailableReason::noPermission;
    case BITOSCOPE_FIRMWARE_LOADING:
      return ipc::UnavailableReason::firmwareLoading;
    default:
      return ipc::UnavailableReason::other;
  }
}

ipc::UnavailableReason reasonOfCheck(Availability availability) {
  switch (availability) {
    case Availability::free:
      return ipc::UnavailableReason::none;
    case Availability::busy:
      return ipc::UnavailableReason::busy;
    case Availability::noPermission:
      return ipc::UnavailableReason::noPermission;
    case Availability::other:
      break;
  }
  return ipc::UnavailableReason::other;
}

}

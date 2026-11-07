/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/wch_route.h"

#include <utility>

#include "usb_checks.h"
#include "usb_device_api.h"
#include "wch/routed_device_system.h"
#include "wch/wch_device_system.h"

namespace fnirsi::wch {

namespace sdk = bitoscope::plugin_c_sdk;

bool openedOnWch(const bitoscope::plugin::UsbDevice& device) {
  bitoscope_usb_device* handle = device.get();
  if (handle == nullptr || !handle->opened) return false;
  const auto* route = dynamic_cast<const WchRoute*>(&handle->opened->system());
  return route != nullptr && route->openedOnWch();
}

std::optional<sdk::ExclusiveOpenResult> namedWchAnswer(std::optional<sdk::ExclusiveOpenResult> answer) {
  if (answer && answer->outcome == sdk::ExclusiveOpenOutcome::failed) answer->detail = "WCH driver: " + answer->detail;
  return answer;
}

std::optional<sdk::ExclusiveOpenResult> wchSecondOpen(const sdk::FoundUsbDevice& device) {
  return namedWchAnswer(tryWindowsWchExclusiveOpen(device));
}

void useWchDriverWhereBound() {
#if defined(_WIN32)
  (void)sdk::setUsbDeviceSystems(makeDlaDeviceSystem);
  (void)sdk::setUsbSecondOpen(wchSecondOpen);
#endif
}

}

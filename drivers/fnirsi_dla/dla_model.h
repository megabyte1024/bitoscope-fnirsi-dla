/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace fnirsi {

struct DlaModel {
  std::string_view device_name;
  std::string_view display_name;
  bool served = false;
};

inline constexpr std::string_view kDlaFamilyName = "FNIRSI DLA";
inline constexpr std::string_view kDlaBootloaderDisplayName = "FNIRSI DLA (bootloader)";

enum class DlaUsbDriver { unnamed, winusb, wch };

inline constexpr std::string_view kWchReplugAdvice = "unplug and replug analyzer: WCH driver cannot recover it otherwise";
inline constexpr std::string_view kReplugAdvice = "unplug and replug analyzer";

struct DlaIdentity {
  bool in_bootloader = false;
  std::string device_name;
  DlaUsbDriver usb_driver = DlaUsbDriver::unnamed;
};
inline constexpr DlaModel kDla16Plus{"DL16", "FNIRSI DLA-16 Plus", true};
inline constexpr DlaModel kDla32Plus{"DL32", "FNIRSI DLA-32 Plus", false};
inline constexpr std::array kDlaModels{kDla16Plus, kDla32Plus};

constexpr const DlaModel* dlaModelNamed(std::string_view device_name) {
  for (const DlaModel& model : kDlaModels) {
    if (model.device_name == device_name) return &model;
  }
  return nullptr;
}

constexpr bool isServedModel(std::string_view device_name) {
  const DlaModel* model = dlaModelNamed(device_name);
  return model != nullptr && model->served;
}

constexpr std::string dlaDisplayName(std::string_view device_name) {
  if (const DlaModel* model = dlaModelNamed(device_name)) return std::string(model->display_name);
  std::string printable;
  for (const char c : device_name) printable.push_back(c >= ' ' && c <= '~' ? c : '?');
  return std::string(kDlaFamilyName) + " (model \"" + printable + "\")";
}

constexpr std::string dlaListedName(std::string_view device_name, DlaUsbDriver usb_driver) {
  switch (usb_driver) {
    case DlaUsbDriver::winusb:
      return dlaDisplayName(device_name) + " (WinUSB)";
    case DlaUsbDriver::wch:
      return dlaDisplayName(device_name) + " (WCH driver)";
    case DlaUsbDriver::unnamed:
      break;
  }
  return dlaDisplayName(device_name);
}

}

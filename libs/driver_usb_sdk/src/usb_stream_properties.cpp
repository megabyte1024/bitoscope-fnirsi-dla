/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_stream_properties.h"

#include <array>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bitoscope_driver_usb.h"
#include "bitoscope_driver_usb_cpp.h"
#include "usb_stream.h"

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr std::string_view kDiagnosticsGroup = "Diagnostics";

bitoscope_property usbIntegerProperty(const char* id, const char* name, const char* help, int64_t minimum,
                                      int64_t maximum, int64_t defaultValue) {
  bitoscope_property property{};
  property.id = id;
  property.name = name;
  property.type = BITOSCOPE_PROPERTY_INTEGER;
  property.help = help;
  property.group = kDiagnosticsGroup.data();
  property.has_minimum = 1;
  property.minimum = minimum;
  property.has_maximum = 1;
  property.maximum = maximum;
  property.default_value = defaultValue;
  return property;
}

const std::array<bitoscope_property, 2>& usbStreamPropertyList() {
  static const std::array<bitoscope_property, 2> list{
      usbIntegerProperty(BITOSCOPE_USB_TRANSFERS_PROPERTY, "USB transfers",
                         "USB transfers kept in flight while streaming; diagnostic setting.", kMinUsbTransfers,
                         kMaxUsbTransfers, kDefaultUsbTransfers),
      usbIntegerProperty(BITOSCOPE_USB_TRANSFER_KIB_PROPERTY, "USB transfer size (KiB, 0: automatic)",
                         "Size of each USB transfer, in KiB; 0: automatic, about 0.1 s of data, at most 1 MiB; "
                         "diagnostic setting.",
                         0, kMaxUsbTransferBytes / kUsbTransferGranuleBytes, 0),
  };
  return list;
}

const int64_t* integerNamed(std::span<const ipc::props::PropertyValue> values, std::string_view id) {
  for (const ipc::props::PropertyValue& value : values) {
    const auto* integer = std::get_if<int64_t>(&value.value);
    if (value.id == id && integer != nullptr) return integer;
  }
  return nullptr;
}

}

std::optional<UsbStreamChoice> usbStreamChoice(std::span<const ipc::props::PropertyValue> values) {
  const int64_t* transfers = integerNamed(values, BITOSCOPE_USB_TRANSFERS_PROPERTY);
  const int64_t* transferKib = integerNamed(values, BITOSCOPE_USB_TRANSFER_KIB_PROPERTY);
  if (transfers == nullptr && transferKib == nullptr) return std::nullopt;
  return UsbStreamChoice{transfers != nullptr ? static_cast<uint32_t>(*transfers) : 0,
                         transferKib != nullptr ? static_cast<uint32_t>(*transferKib) : 0};
}

}

namespace bitoscope::plugin {

void addUsbStreamProperties(std::vector<PropertyNode>& properties) {
  std::vector<PropertyNode> usb;
  for (const bitoscope_property& c : plugin_c_sdk::usbStreamPropertyList()) {
    usb.push_back(PropertyNode{Property{c.id, c.name, IntegerType{c.minimum, c.maximum, c.default_value}, Unit::none,
                                        false, c.help}});
  }
  for (PropertyNode& node : properties) {
    auto* group = std::get_if<PropertyGroup>(&node.node);
    if (group == nullptr || group->name != plugin_c_sdk::kDiagnosticsGroup) continue;
    group->nodes.insert(group->nodes.end(), usb.begin(), usb.end());
    return;
  }
  properties.push_back(PropertyNode{PropertyGroup{std::string(plugin_c_sdk::kDiagnosticsGroup), std::move(usb)}});
}

}

extern "C" const bitoscope_property* bitoscope_usb_stream_properties(size_t* count) {
  const auto& list = bitoscope::plugin_c_sdk::usbStreamPropertyList();
  if (count != nullptr) *count = list.size();
  return list.data();
}

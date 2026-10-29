/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_stream_properties.h"

#include <cstdint>
#include <string_view>
#include <variant>

#include "bitoscope_driver.h"
#include "facet_properties.h"
#include "usb_stream.h"

namespace bitoscope::plugin_c_sdk {

namespace {

ipc::props::Property usbIntegerProperty(const char* id, const char* name, const char* help, int64_t minimum,
                                        int64_t maximum, int64_t defaultValue) {
  return ipc::props::Property{id, name, help, false, ipc::props::Unit::none,
                              ipc::props::IntegerSpec{minimum, maximum, defaultValue}};
}

int64_t integerNamed(std::span<const ipc::props::PropertyValue> values, std::string_view id) {
  for (const ipc::props::PropertyValue& value : values) {
    const auto* integer = std::get_if<int64_t>(&value.value);
    if (value.id == id && integer != nullptr) return *integer;
  }
  return 0;
}

}

std::expected<void, std::string> addUsbStreamProperties(std::vector<ipc::props::PropertyNode>& tree) {
  const std::vector<ipc::props::Property> properties{
      usbIntegerProperty(BITOSCOPE_USB_TRANSFERS_PROPERTY, "USB transfers",
                         "USB transfers kept in flight while streaming; diagnostic setting.", kMinUsbTransfers,
                         kMaxUsbTransfers, kDefaultUsbTransfers),
      usbIntegerProperty(BITOSCOPE_USB_TRANSFER_KIB_PROPERTY, "USB transfer size (KiB, 0: automatic)",
                         "Size of each USB transfer, in KiB; 0: automatic, about 0.1 s of data, at most 1 MiB; "
                         "diagnostic setting.",
                         0, kMaxUsbTransferBytes / kUsbTransferGranuleBytes, 0),
  };
  for (const ipc::props::Property& property : properties) {
    if (auto added = addProperty(tree, PlacedProperty{{"Diagnostics"}, property}); !added) {
      return std::unexpected("USB stream property \"" + property.id + "\": " + added.error());
    }
  }
  return {};
}

UsbStreamChoice usbStreamChoice(std::span<const ipc::props::PropertyValue> values) {
  return UsbStreamChoice{static_cast<uint32_t>(integerNamed(values, BITOSCOPE_USB_TRANSFERS_PROPERTY)),
                         static_cast<uint32_t>(integerNamed(values, BITOSCOPE_USB_TRANSFER_KIB_PROPERTY))};
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitoscope_driver.h"
#include "ipc_properties.h"

namespace bitoscope::plugin_c_sdk {

struct PlacedProperty {
  std::vector<std::string> groups;
  ipc::props::Property property;
};
std::expected<PlacedProperty, std::string> propertyFrom(const bitoscope_property& property);

std::expected<void, std::string> addProperty(std::vector<ipc::props::PropertyNode>& tree, PlacedProperty placed);

std::vector<bitoscope_property_value> cValues(std::span<const ipc::props::PropertyValue> values);

}

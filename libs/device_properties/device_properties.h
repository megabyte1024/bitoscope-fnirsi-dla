/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipc_properties.h"

namespace bitoscope::device_properties {

using Property = bitoscope::ipc::props::Property;
using PropertyNode = bitoscope::ipc::props::PropertyNode;
using PropertyGroup = bitoscope::ipc::props::PropertyGroup;
using PropertyValue = bitoscope::ipc::props::PropertyValue;

std::expected<PropertyValue, std::string> effectiveDefault(const Property& property);

std::expected<void, std::string> checkValue(const Property& property, const PropertyValue& value,
                                            bool capturing);

std::expected<void, std::string> checkTree(const std::vector<PropertyNode>& nodes);

std::expected<PropertyValue, std::string> parseValue(const Property& property, std::string_view text);

std::expected<std::string, std::string> formatValue(const Property& property, const PropertyValue& value);

const Property* findProperty(const std::vector<PropertyNode>& nodes, std::string_view id);

struct PropertyListing {
  std::string path;
  const Property* property = nullptr;
};
std::vector<PropertyListing> listProperties(const std::vector<PropertyNode>& nodes);

std::expected<std::vector<PropertyValue>, std::string> readSettings(
    const std::vector<PropertyNode>& nodes, const std::map<std::string, std::string>& settings);

std::string_view unitSymbol(bitoscope::ipc::props::Unit unit);

}

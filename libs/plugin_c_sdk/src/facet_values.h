/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipc_properties.h"

namespace bitoscope::plugin_c_sdk {

std::vector<ipc::props::PropertyValue> defaultValues(const std::vector<ipc::props::PropertyNode>& tree);

struct PropertyRefusal {
  std::string message;
  std::string propertyId;
};

std::expected<void, PropertyRefusal> checkValues(std::string_view facet, const std::vector<ipc::props::PropertyNode>& tree,
                                                 std::span<const ipc::props::PropertyValue> values, bool capturing);

void takeValues(std::vector<ipc::props::PropertyValue>& current, std::span<const ipc::props::PropertyValue> values);

}

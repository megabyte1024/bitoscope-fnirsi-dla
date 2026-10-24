/*
 * SPDX-License-Identifier: MIT
 */

#include "facet_values.h"

#include <algorithm>
#include <set>
#include <utility>

#include "device_properties.h"

namespace bitoscope::plugin_c_sdk {

namespace props = ipc::props;
namespace model = bitoscope::device_properties;

std::vector<props::PropertyValue> defaultValues(const std::vector<props::PropertyNode>& tree) {
  std::vector<props::PropertyValue> out;
  for (const model::PropertyListing& listing : model::listProperties(tree)) {
    if (auto value = model::effectiveDefault(*listing.property)) out.push_back(std::move(*value));
  }
  return out;
}

std::expected<void, PropertyRefusal> checkValues(std::string_view facet, const std::vector<props::PropertyNode>& tree,
                                                 std::span<const props::PropertyValue> values, bool capturing) {
  std::set<std::string_view> given;
  for (const props::PropertyValue& value : values) {
    const props::Property* property = model::findProperty(tree, value.id);
    if (property == nullptr) {
      return std::unexpected(PropertyRefusal{
          "setProperties refused: facet \"" + std::string(facet) + "\" has no property \"" + value.id + "\"", value.id});
    }
    if (!given.insert(value.id).second) {
      return std::unexpected(PropertyRefusal{
          "setProperties refused: \"" + property->name + "\" is given twice in one request", value.id});
    }
    if (auto allowed = model::checkValue(*property, value, capturing); !allowed) {
      return std::unexpected(PropertyRefusal{"setProperties refused: " + allowed.error(), value.id});
    }
  }
  return {};
}

void takeValues(std::vector<props::PropertyValue>& current, std::span<const props::PropertyValue> values) {
  for (const props::PropertyValue& value : values) {
    auto found = std::find_if(current.begin(), current.end(),
                              [&](const props::PropertyValue& c) { return c.id == value.id; });
    if (found != current.end()) found->value = value.value;
  }
}

}

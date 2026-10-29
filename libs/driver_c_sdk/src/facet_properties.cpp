/*
 * SPDX-License-Identifier: MIT
 */

#include "facet_properties.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "device_properties.h"
#include "frame_output.h"

namespace bitoscope::plugin_c_sdk {

namespace props = ipc::props;
namespace model = bitoscope::device_properties;

namespace {

props::Bound bound(int has, int64_t value) {
  if (has != 0) return props::Bound{value};
  return props::Bound{props::Unbounded{}};
}

std::string text(const char* s) { return s != nullptr ? std::string(s) : std::string(); }

std::expected<std::vector<std::string>, std::string> groupsOf(const std::string& path) {
  std::vector<std::string> out;
  if (path.empty()) return out;
  size_t from = 0;
  for (;;) {
    const size_t slash = path.find('/', from);
    std::string name = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
    if (name.empty()) {
      return std::unexpected("group \"" + path + "\" has empty group name in it (groups nest with '/')");
    }
    out.push_back(std::move(name));
    if (slash == std::string::npos) return out;
    from = slash + 1;
  }
}

std::expected<void, std::string> normalizeDefault(props::Property& property) {
  auto value = model::effectiveDefault(property);
  if (!value) return std::unexpected(value.error());
  if (auto* integer = std::get_if<props::IntegerSpec>(&property.type)) {
    integer->defaultValue = std::get<int64_t>(value->value);
  } else if (auto* decimal = std::get_if<props::DecimalSpec>(&property.type)) {
    decimal->defaultValue = std::get<int64_t>(value->value);
  } else if (auto* enumeration = std::get_if<props::EnumerationSpec>(&property.type)) {
    enumeration->defaultIndex = std::get<props::EnumIndex>(value->value).index;
  }
  return {};
}

}

std::expected<PlacedProperty, std::string> propertyFrom(const bitoscope_property& p) {
  PlacedProperty out;
  auto groups = groupsOf(text(p.group));
  if (!groups) return std::unexpected(groups.error());
  out.groups = std::move(*groups);
  if (p.unit < BITOSCOPE_UNIT_NONE || p.unit > BITOSCOPE_UNIT_SAMPLES) {
    return std::unexpected("unit " + std::to_string(p.unit) + " is not one of BITOSCOPE_UNIT_ values");
  }
  props::Property& property = out.property;
  property.id = text(p.id);
  property.name = text(p.name);
  property.help = text(p.help);
  property.changeableDuringCapture = p.changeable_during_capture != 0;
  property.unit = static_cast<props::Unit>(p.unit);
  switch (p.type) {
    case BITOSCOPE_PROPERTY_INTEGER:
      property.type = props::IntegerSpec{bound(p.has_minimum, p.minimum), bound(p.has_maximum, p.maximum),
                                         p.default_value};
      break;
    case BITOSCOPE_PROPERTY_DECIMAL:
      property.type = props::DecimalSpec{p.digits, bound(p.has_minimum, p.minimum), bound(p.has_maximum, p.maximum),
                                         p.default_value};
      break;
    case BITOSCOPE_PROPERTY_BOOLEAN:
      if (p.boolean_style != BITOSCOPE_BOOLEAN_ON_OFF && p.boolean_style != BITOSCOPE_BOOLEAN_ENABLE_DISABLE) {
        return std::unexpected("boolean style " + std::to_string(p.boolean_style) +
                               " is neither BITOSCOPE_BOOLEAN_ON_OFF nor BITOSCOPE_BOOLEAN_ENABLE_DISABLE");
      }
      property.type = props::BooleanSpec{p.default_value != 0, static_cast<props::BooleanStyle>(p.boolean_style)};
      break;
    case BITOSCOPE_PROPERTY_ENUMERATION: {
      if (p.item_names != nullptr && p.item_numbers != nullptr) {
        return std::unexpected(std::string("enumeration's items are names or numbers, not both (item_names and "
                                           "item_numbers are both set)"));
      }
      if (p.num_items > 0 && p.item_names == nullptr && p.item_numbers == nullptr) {
        return std::unexpected("num_items is " + std::to_string(p.num_items) +
                               ", but neither item_names nor item_numbers is set");
      }
      props::EnumerationSpec spec;
      spec.digits = p.digits;
      spec.defaultIndex = p.default_index;
      for (size_t i = 0; i < p.num_items; ++i) {
        if (p.item_names != nullptr) {
          spec.items.emplace_back(text(p.item_names[i]));
        } else {
          spec.items.emplace_back(p.item_numbers[i]);
        }
      }
      property.type = std::move(spec);
      break;
    }
    default:
      return std::unexpected("type " + std::to_string(p.type) + " is not one of BITOSCOPE_PROPERTY_ types");
  }
  return out;
}

std::expected<void, std::string> addProperty(std::vector<props::PropertyNode>& tree, PlacedProperty placed) {
  std::vector<props::PropertyNode> candidate = tree;
  std::vector<props::PropertyNode>* level = &candidate;
  for (const std::string& name : placed.groups) {
    auto group = std::find_if(level->begin(), level->end(), [&](const props::PropertyNode& node) {
      const auto* g = std::get_if<props::PropertyGroup>(&node.node);
      return g != nullptr && g->name == name;
    });
    if (group == level->end()) {
      level->push_back(props::PropertyNode{props::PropertyGroup{name, {}}});
      group = std::prev(level->end());
    }
    level = &std::get<props::PropertyGroup>(group->node).nodes;
  }
  if (auto normalized = normalizeDefault(placed.property); !normalized) return normalized;
  level->push_back(props::PropertyNode{std::move(placed.property)});
  if (auto checked = model::checkTree(candidate); !checked) return checked;
  tree = std::move(candidate);
  return {};
}

std::expected<std::vector<props::PropertyNode>, std::string> propertyTreeOf(const bitoscope_property* properties,
                                                                           size_t count) {
  std::vector<props::PropertyNode> tree;
  if (count == 0) return tree;
  if (properties == nullptr) return std::unexpected(std::to_string(count) + " properties, but properties is NULL");
  for (size_t i = 0; i < count; ++i) {
    const std::string where = "property \"" + text(properties[i].id) + "\": ";
    auto placed = propertyFrom(properties[i]);
    if (!placed) return std::unexpected(where + placed.error());
    if (auto added = addProperty(tree, std::move(*placed)); !added) return std::unexpected(where + added.error());
  }
  return tree;
}

void logMissingProperty(std::string_view id) {
  logLine(ipc::LogLevel::error, "logic analyzer has no property \"" + std::string(id) + "\"");
}

std::vector<bitoscope_property_value> cValues(std::span<const props::PropertyValue> values) {
  std::vector<bitoscope_property_value> out;
  out.reserve(values.size());
  for (const props::PropertyValue& value : values) {
    bitoscope_property_value c{};
    c.id = value.id.c_str();
    if (const auto* integer = std::get_if<int64_t>(&value.value)) c.integer = *integer;
    if (const auto* boolean = std::get_if<bool>(&value.value)) c.boolean = *boolean ? 1 : 0;
    if (const auto* index = std::get_if<props::EnumIndex>(&value.value)) c.enum_index = index->index;
    out.push_back(c);
  }
  return out;
}

}

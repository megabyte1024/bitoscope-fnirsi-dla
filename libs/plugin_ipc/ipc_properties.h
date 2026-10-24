/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace bitoscope::ipc::props {

struct UnknownMember {
  uint16_t member = 0;
  bool operator==(const UnknownMember&) const = default;
};

enum class Unit : uint16_t { none = 0, volt = 1, second = 2, hertz = 3, percent = 4, samples = 5 };
enum class BooleanStyle : uint16_t { onOff = 0, enableDisable = 1 };

struct Unbounded {
  bool operator==(const Unbounded&) const = default;
};
using Bound = std::variant<Unbounded, int64_t, UnknownMember>;

struct IntegerSpec {
  Bound min;
  Bound max;
  int64_t defaultValue = 0;
  bool operator==(const IntegerSpec&) const = default;
};

struct DecimalSpec {
  uint32_t digits = 0;
  Bound min;
  Bound max;
  int64_t defaultValue = 0;
  bool operator==(const DecimalSpec&) const = default;
};

struct BooleanSpec {
  bool defaultValue = false;
  BooleanStyle style = BooleanStyle::onOff;
  bool operator==(const BooleanSpec&) const = default;
};

using EnumItem = std::variant<std::string, int64_t, UnknownMember>;

struct EnumerationSpec {
  std::vector<EnumItem> items;
  uint32_t digits = 0;
  uint32_t defaultIndex = 0;
  bool operator==(const EnumerationSpec&) const = default;
};

using PropertyType = std::variant<IntegerSpec, DecimalSpec, BooleanSpec, EnumerationSpec, UnknownMember>;

struct Property {
  std::string id;
  std::string name;
  std::string help;
  bool changeableDuringCapture = false;
  Unit unit = Unit::none;
  PropertyType type;
  bool operator==(const Property&) const = default;
};

struct PropertyNode;

struct PropertyGroup {
  std::string name;
  std::vector<PropertyNode> nodes;
  bool operator==(const PropertyGroup& other) const;
};

struct PropertyNode {
  std::variant<PropertyGroup, Property, UnknownMember> node;
  bool operator==(const PropertyNode&) const = default;
};

struct EnumIndex {
  uint32_t index = 0;
  bool operator==(const EnumIndex&) const = default;
};

struct PropertyValue {
  std::string id;
  std::variant<int64_t, bool, EnumIndex, UnknownMember> value;
  bool operator==(const PropertyValue&) const = default;
};

struct SetProperties {
  std::string facet;
  std::vector<PropertyValue> values;
  bool operator==(const SetProperties&) const = default;
};

}

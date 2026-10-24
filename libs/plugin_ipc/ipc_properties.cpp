/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_properties.h"

#include <string>
#include <utility>

#include "ipc_capnp_values.h"
#include "ipc_properties_wire.h"

namespace bitoscope::ipc::props {

bool PropertyGroup::operator==(const PropertyGroup& other) const {
  return name == other.name && nodes == other.nodes;
}

namespace detail {

namespace {

namespace w = bitoscope::ipc::wire::props;
using ipc::detail::asText;

DecodeError missing(std::string message) { return DecodeError{DecodeErrorKind::missingField, std::move(message), {}, 0}; }

void build(w::Bound::Builder out, const Bound& in) {
  if (const auto* value = std::get_if<int64_t>(&in)) {
    out.setValue(*value);
  } else {
    // WHY: An UnknownMember decoded from a newer peer cannot be written back: it stays none.
    out.setNone();
  }
}

void build(w::EnumItem::Builder out, const EnumItem& in) {
  if (const auto* name = std::get_if<std::string>(&in)) {
    out.setName(asText(*name));
  } else if (const auto* number = std::get_if<int64_t>(&in)) {
    out.setNumber(*number);
  }
}

void build(w::Property::Builder out, const Property& in) {
  if (!in.id.empty()) out.setId(asText(in.id));
  if (!in.name.empty()) out.setName(asText(in.name));
  if (!in.help.empty()) out.setHelp(asText(in.help));
  out.setChangeableDuringCapture(in.changeableDuringCapture);
  out.setUnit(static_cast<w::Unit>(static_cast<uint16_t>(in.unit)));
  auto type = out.getType();
  if (const auto* integer = std::get_if<IntegerSpec>(&in.type)) {
    auto spec = type.initInteger();
    build(spec.initMin(), integer->min);
    build(spec.initMax(), integer->max);
    spec.setDefaultValue(integer->defaultValue);
  } else if (const auto* decimal = std::get_if<DecimalSpec>(&in.type)) {
    auto spec = type.initDecimal();
    spec.setDigits(decimal->digits);
    build(spec.initMin(), decimal->min);
    build(spec.initMax(), decimal->max);
    spec.setDefaultValue(decimal->defaultValue);
  } else if (const auto* boolean = std::get_if<BooleanSpec>(&in.type)) {
    auto spec = type.initBoolean();
    spec.setDefaultValue(boolean->defaultValue);
    spec.setStyle(static_cast<w::BooleanStyle>(static_cast<uint16_t>(boolean->style)));
  } else if (const auto* enumeration = std::get_if<EnumerationSpec>(&in.type)) {
    auto spec = type.initEnumeration();
    if (!enumeration->items.empty()) {
      auto items = spec.initItems(static_cast<unsigned>(enumeration->items.size()));
      for (unsigned i = 0; i < items.size(); ++i) build(items[i], enumeration->items[i]);
    }
    spec.setDigits(enumeration->digits);
    spec.setDefaultIndex(enumeration->defaultIndex);
  }
}

void build(w::PropertyNode::Builder out, const PropertyNode& in) {
  if (const auto* group = std::get_if<PropertyGroup>(&in.node)) {
    auto g = out.initGroup();
    if (!group->name.empty()) g.setName(asText(group->name));
    if (!group->nodes.empty()) detail::build(g.initNodes(static_cast<unsigned>(group->nodes.size())), group->nodes);
  } else if (const auto* property = std::get_if<Property>(&in.node)) {
    build(out.initProperty(), *property);
  }
}

std::string named(const std::string& id) { return id.empty() ? std::string("property") : id; }

Bound read(w::Bound::Reader in) {
  switch (in.which()) {
    case w::Bound::NONE:
      return Unbounded{};
    case w::Bound::VALUE:
      return in.getValue();
  }
  return UnknownMember{static_cast<uint16_t>(in.which())};
}

std::optional<DecodeError> read(w::EnumerationSpec::Reader in, const std::string& id, EnumerationSpec& out) {
  unsigned index = 0;
  for (w::EnumItem::Reader item : in.getItems()) {
    switch (item.which()) {
      case w::EnumItem::NONE:
        return missing(named(id) + "'s enumeration item " + std::to_string(index) + " holds nothing");
      case w::EnumItem::NAME:
        out.items.emplace_back(ipc::detail::toString(item.getName()));
        break;
      case w::EnumItem::NUMBER:
        out.items.emplace_back(item.getNumber());
        break;
      default:
        out.items.emplace_back(UnknownMember{static_cast<uint16_t>(item.which())});
        break;
    }
    ++index;
  }
  out.digits = in.getDigits();
  out.defaultIndex = in.getDefaultIndex();
  return std::nullopt;
}

std::optional<DecodeError> read(w::Property::Reader in, Property& out) {
  out.id = ipc::detail::toString(in.getId());
  out.name = ipc::detail::toString(in.getName());
  out.help = ipc::detail::toString(in.getHelp());
  out.changeableDuringCapture = in.getChangeableDuringCapture();
  out.unit = static_cast<Unit>(static_cast<uint16_t>(in.getUnit()));
  auto type = in.getType();
  switch (type.which()) {
    case w::Property::Type::NONE:
      return missing(named(out.id) + " has no type (its union is `none`)");
    case w::Property::Type::INTEGER: {
      auto spec = type.getInteger();
      out.type = IntegerSpec{read(spec.getMin()), read(spec.getMax()), spec.getDefaultValue()};
      return std::nullopt;
    }
    case w::Property::Type::DECIMAL: {
      auto spec = type.getDecimal();
      out.type = DecimalSpec{spec.getDigits(), read(spec.getMin()), read(spec.getMax()), spec.getDefaultValue()};
      return std::nullopt;
    }
    case w::Property::Type::BOOLEAN: {
      auto spec = type.getBoolean();
      out.type = BooleanSpec{spec.getDefaultValue(), static_cast<BooleanStyle>(static_cast<uint16_t>(spec.getStyle()))};
      return std::nullopt;
    }
    case w::Property::Type::ENUMERATION: {
      EnumerationSpec spec;
      if (auto error = read(type.getEnumeration(), out.id, spec)) return error;
      out.type = std::move(spec);
      return std::nullopt;
    }
  }
  out.type = UnknownMember{static_cast<uint16_t>(type.which())};
  return std::nullopt;
}

}

void build(capnp::List<w::PropertyNode>::Builder out, const std::vector<PropertyNode>& in) {
  for (unsigned i = 0; i < out.size(); ++i) build(out[i], in[i]);
}

void build(w::SetProperties::Builder out, const SetProperties& in) {
  if (!in.facet.empty()) out.setFacet(asText(in.facet));
  if (in.values.empty()) return;
  auto values = out.initValues(static_cast<unsigned>(in.values.size()));
  for (unsigned i = 0; i < values.size(); ++i) {
    const PropertyValue& value = in.values[i];
    if (!value.id.empty()) values[i].setId(asText(value.id));
    if (const auto* integer = std::get_if<int64_t>(&value.value)) {
      values[i].setInteger(*integer);
    } else if (const auto* boolean = std::get_if<bool>(&value.value)) {
      values[i].setBoolean(*boolean);
    } else if (const auto* index = std::get_if<EnumIndex>(&value.value)) {
      values[i].setEnumIndex(index->index);
    }
  }
}

std::optional<DecodeError> read(capnp::List<w::PropertyNode>::Reader in, std::vector<PropertyNode>& out) {
  out.reserve(in.size());
  for (w::PropertyNode::Reader node : in) {
    switch (node.which()) {
      case w::PropertyNode::NONE:
        return missing("property node holds nothing (its union is `none`)");
      case w::PropertyNode::GROUP: {
        auto group = node.getGroup();
        PropertyGroup value{ipc::detail::toString(group.getName()), {}};
        if (auto error = read(group.getNodes(), value.nodes)) return error;
        out.push_back(PropertyNode{std::move(value)});
        break;
      }
      case w::PropertyNode::PROPERTY: {
        Property value;
        if (auto error = read(node.getProperty(), value)) return error;
        out.push_back(PropertyNode{std::move(value)});
        break;
      }
      default:
        out.push_back(PropertyNode{UnknownMember{static_cast<uint16_t>(node.which())}});
        break;
    }
  }
  return std::nullopt;
}

std::optional<DecodeError> read(w::SetProperties::Reader in, SetProperties& out) {
  out.facet = ipc::detail::toString(in.getFacet());
  if (out.facet.empty()) return missing("setProperties request names no facet");
  for (w::PropertyValue::Reader value : in.getValues()) {
    PropertyValue v;
    v.id = ipc::detail::toString(value.getId());
    switch (value.which()) {
      case w::PropertyValue::NONE:
        return missing("value for " + named(v.id) + " holds nothing (its union is `none`)");
      case w::PropertyValue::INTEGER:
        v.value = value.getInteger();
        break;
      case w::PropertyValue::BOOLEAN:
        v.value = value.getBoolean();
        break;
      case w::PropertyValue::ENUM_INDEX:
        v.value = EnumIndex{value.getEnumIndex()};
        break;
      default:
        v.value = UnknownMember{static_cast<uint16_t>(value.which())};
        break;
    }
    out.values.push_back(std::move(v));
  }
  return std::nullopt;
}

}

}

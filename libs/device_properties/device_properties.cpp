/*
 * SPDX-License-Identifier: MIT
 */

#include "device_properties.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace bitoscope::device_properties {

namespace props = bitoscope::ipc::props;

std::string_view unitSymbol(props::Unit unit) {
  switch (unit) {
    case props::Unit::none: return "";
    case props::Unit::volt: return "V";
    case props::Unit::second: return "s";
    case props::Unit::hertz: return "Hz";
    case props::Unit::percent: return "%";
    case props::Unit::samples: return "S";
  }
  return "";
}

namespace {

std::optional<int64_t> boundValue(const props::Bound& bound) {
  if (const auto* v = std::get_if<int64_t>(&bound)) return *v;
  return std::nullopt;
}

constexpr std::array<std::pair<std::string_view, int>, 7> kPrefixLetters = {{
    {"p", -12}, {"n", -9}, {"u", -6}, {"m", -3}, {"k", 3}, {"M", 6}, {"G", 9}}};

std::optional<int> prefixExponent(char c) {
  for (const auto& [letter, exp] : kPrefixLetters) {
    if (letter.front() == c) return exp;
  }
  return std::nullopt;
}

std::string formatExactDecimal(int64_t raw, int scale) {
  const bool negative = raw < 0;
  // WHY: Negate in uint64_t: signed negation of INT64_MIN is undefined behaviour.
  const uint64_t magnitude = negative ? (0 - static_cast<uint64_t>(raw)) : static_cast<uint64_t>(raw);
  std::string digits = std::to_string(magnitude);

  std::string result;
  if (scale <= 0) {
    digits.append(static_cast<size_t>(-scale), '0');
    result = digits;
  } else {
    const size_t decimals = static_cast<size_t>(scale);
    if (digits.size() <= decimals) {
      digits.insert(0, decimals - digits.size() + 1, '0');
    }
    const size_t point = digits.size() - decimals;
    std::string fraction = digits.substr(point);
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    result = digits.substr(0, point);
    if (!fraction.empty()) result += "." + fraction;
  }
  return negative ? "-" + result : result;
}

std::expected<int64_t, std::string> scaleExact(bool negative, std::string digits, long long exp) {
  if (exp >= 0) {
    digits.append(static_cast<size_t>(exp), '0');
  } else {
    const size_t lose = static_cast<size_t>(-exp);
    std::string dropped;
    std::string kept;
    if (lose >= digits.size()) {
      dropped = digits;
      kept = "0";
    } else {
      dropped = digits.substr(digits.size() - lose);
      kept = digits.substr(0, digits.size() - lose);
    }
    for (const char c : dropped) {
      if (c != '0') return std::unexpected(std::string("inexact"));
    }
    digits = std::move(kept);
  }

  const size_t firstNonZero = digits.find_first_not_of('0');
  digits = (firstNonZero == std::string::npos) ? std::string("0") : digits.substr(firstNonZero);

  constexpr std::string_view kMaxPositive = "9223372036854775807";
  constexpr std::string_view kMaxNegative = "9223372036854775808";

  if (negative && digits == kMaxNegative) {
    return std::numeric_limits<int64_t>::min();
  }
  const std::string_view limit = negative ? kMaxNegative : kMaxPositive;
  if (digits.size() > limit.size() || (digits.size() == limit.size() && digits > limit)) {
    return std::unexpected(std::string("overflow"));
  }

  int64_t magnitude = 0;
  const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
  if (result.ec != std::errc{}) {
    return std::unexpected(std::string("overflow"));
  }
  return negative ? -magnitude : magnitude;
}

struct ParsedNumber {
  bool negative = false;
  std::string digits;
  long long fracExp = 0;
};

std::optional<ParsedNumber> scanNumber(std::string_view text, std::string_view& restOut) {
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && text[i] == '-') {
    negative = true;
    ++i;
  }
  const size_t intStart = i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  const std::string_view intPart = text.substr(intStart, i - intStart);

  std::string_view fracPart;
  if (i < text.size() && text[i] == '.') {
    ++i;
    const size_t fracStart = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    fracPart = text.substr(fracStart, i - fracStart);
  }
  if (intPart.empty() && fracPart.empty()) return std::nullopt;

  restOut = text.substr(i);
  ParsedNumber number;
  number.negative = negative;
  number.digits = std::string(intPart) + std::string(fracPart);
  number.fracExp = -static_cast<long long>(fracPart.size());
  return number;
}

std::expected<int, std::string> parseUnitSuffix(std::string_view rest, props::Unit unit,
                                                std::string_view propertyName) {
  const std::string_view symbol = unitSymbol(unit);
  const auto matches = [&](std::string_view remainder) { return remainder.empty() || remainder == symbol; };
  if (matches(rest)) return 0;
  if (!rest.empty()) {
    if (const auto exp = prefixExponent(rest.front())) {
      if (matches(rest.substr(1))) return *exp;
    }
  }
  if (symbol.empty()) {
    return std::unexpected(std::format("\"{}\": '{}' takes no unit or prefix", propertyName, rest));
  }
  return std::unexpected(
      std::format("\"{}\": '{}' is not recognized prefix or unit (expected {}, "
                  "optionally with p/n/u/m/k/M/G)",
                  propertyName, rest, symbol));
}

std::expected<int64_t, std::string> parseNumericText(std::string_view text, props::Unit unit,
                                                      uint32_t digits, std::string_view propertyName) {
  std::string_view rest;
  const auto number = scanNumber(text, rest);
  if (!number.has_value()) {
    return std::unexpected(std::format("\"{}\": '{}' is not number", propertyName, text));
  }
  while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
  const auto prefixExp = parseUnitSuffix(rest, unit, propertyName);
  if (!prefixExp.has_value()) {
    return std::unexpected(prefixExp.error());
  }
  const long long totalExp = number->fracExp + *prefixExp + static_cast<long long>(digits);
  const auto scaled = scaleExact(number->negative, number->digits, totalExp);
  if (!scaled.has_value()) {
    if (scaled.error() == "inexact") {
      return std::unexpected(std::format(
          "\"{}\": '{}' is finer than its {} digit(s); it is never rounded", propertyName, text, digits));
    }
    return std::unexpected(std::format("\"{}\": '{}' is too large to represent", propertyName, text));
  }
  return *scaled;
}

std::string formatNumeric(int64_t raw, uint32_t propertyDigits, props::Unit unit) {
  const std::string_view symbol = unitSymbol(unit);
  if (unit == props::Unit::none) {
    return formatExactDecimal(raw, static_cast<int>(propertyDigits));
  }
  if (raw == 0) {
    return formatExactDecimal(0, static_cast<int>(propertyDigits)) + " " + std::string(symbol);
  }

  const uint64_t magnitude = raw < 0 ? (0 - static_cast<uint64_t>(raw)) : static_cast<uint64_t>(raw);
  const int numDigits = static_cast<int>(std::to_string(magnitude).size());

  static constexpr std::array<std::pair<std::string_view, int>, 8> kCandidates = {
      {{"G", 9}, {"M", 6}, {"k", 3}, {"", 0}, {"m", -3}, {"u", -6}, {"n", -9}, {"p", -12}}};
  for (const auto& [prefixSymbol, exp] : kCandidates) {
    if (numDigits > static_cast<int>(propertyDigits) + exp) {
      const int scale = static_cast<int>(propertyDigits) + exp;
      return formatExactDecimal(raw, scale) + " " + std::string(prefixSymbol) + std::string(symbol);
    }
  }
  // WHY: Unreachable after checkTree() but kept: a caller may pass a tree checkTree() never validated.
  const int scale = static_cast<int>(propertyDigits) - 12;
  return formatExactDecimal(raw, scale) + " p" + std::string(symbol);
}

std::optional<bool> parseBoolean(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr std::array<std::string_view, 5> kTrue = {"on", "true", "1", "enable", "yes"};
  static constexpr std::array<std::string_view, 5> kFalse = {"off", "false", "0", "disable", "no"};
  for (const auto s : kTrue) {
    if (lower == s) return true;
  }
  for (const auto s : kFalse) {
    if (lower == s) return false;
  }
  return std::nullopt;
}

std::string withPath(const std::string& path, const std::string& name) {
  return path.empty() ? std::format("\"{}\"", name) : std::format("{} > \"{}\"", path, name);
}

std::expected<void, std::string> checkTypeSpec(const std::string& path, const std::string& name,
                                                const props::PropertyType& type) {
  return std::visit(
      [&](const auto& spec) -> std::expected<void, std::string> {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec>) {
          if (std::holds_alternative<props::UnknownMember>(spec.min) ||
              std::holds_alternative<props::UnknownMember>(spec.max)) {
            return std::unexpected(
                std::format("{}: has limit of kind this build does not recognize", withPath(path, name)));
          }
          const auto mn = boundValue(spec.min);
          const auto mx = boundValue(spec.max);
          if (mn && mx && *mn > *mx) {
            return std::unexpected(std::format("{}: its minimum is above its maximum", withPath(path, name)));
          }
          return {};
        } else if constexpr (std::is_same_v<T, props::DecimalSpec>) {
          if (spec.digits > 18) {
            return std::unexpected(std::format("{}: {} digits exceeds 18, most that int64 holds exactly",
                                               withPath(path, name), spec.digits));
          }
          if (std::holds_alternative<props::UnknownMember>(spec.min) ||
              std::holds_alternative<props::UnknownMember>(spec.max)) {
            return std::unexpected(
                std::format("{}: has limit of kind this build does not recognize", withPath(path, name)));
          }
          const auto mn = boundValue(spec.min);
          const auto mx = boundValue(spec.max);
          if (mn && mx && *mn > *mx) {
            return std::unexpected(std::format("{}: its minimum is above its maximum", withPath(path, name)));
          }
          return {};
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          return {};
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          if (spec.digits > 18) {
            return std::unexpected(std::format("{}: {} digits exceeds 18, most that int64 holds exactly",
                                               withPath(path, name), spec.digits));
          }
          if (spec.items.empty()) {
            return std::unexpected(std::format("{}: has no items", withPath(path, name)));
          }
          for (size_t i = 0; i < spec.items.size(); ++i) {
            const auto& item = spec.items[i];
            if (std::holds_alternative<props::UnknownMember>(item)) {
              return std::unexpected(std::format("{}: item {} is kind this build does not recognize",
                                                 withPath(path, name), i));
            }
            if (const auto* s = std::get_if<std::string>(&item); s != nullptr && s->empty()) {
              return std::unexpected(std::format("{}: item {} has empty name", withPath(path, name), i));
            }
          }
          return {};
        } else {
          return std::unexpected(
              std::format("{}: has property type this build does not recognize", withPath(path, name)));
        }
      },
      type);
}

std::expected<void, std::string> checkNodes(const std::vector<PropertyNode>& nodes, const std::string& path,
                                            std::set<std::string>& seenIds) {
  for (const PropertyNode& n : nodes) {
    if (std::holds_alternative<props::UnknownMember>(n.node)) {
      return std::unexpected(std::format(
          "{}: node of kind this build does not recognize", path.empty() ? std::string("(top level)") : path));
    }
    if (const auto* group = std::get_if<PropertyGroup>(&n.node)) {
      if (group->name.empty()) {
        return std::unexpected(
            std::format("{}: group has empty name", path.empty() ? std::string("(top level)") : path));
      }
      const std::string childPath = path.empty() ? group->name : path + " > " + group->name;
      if (auto r = checkNodes(group->nodes, childPath, seenIds); !r.has_value()) return r;
      continue;
    }
    const auto& property = std::get<Property>(n.node);
    if (property.id.empty()) {
      return std::unexpected(std::format("{}: has empty id", withPath(path, property.name)));
    }
    if (property.name.empty()) {
      return std::unexpected(
          std::format("{}: property \"{}\" has empty name", path.empty() ? std::string("(top level)") : path,
                     property.id));
    }
    if (!seenIds.insert(property.id).second) {
      return std::unexpected(
          std::format("property id \"{}\" is used more than once, at {}", property.id, withPath(path, property.name)));
    }
    if (auto r = checkTypeSpec(path, property.name, property.type); !r.has_value()) return r;
  }
  return {};
}

}

std::expected<PropertyValue, std::string> effectiveDefault(const Property& property) {
  return std::visit(
      [&](const auto& spec) -> std::expected<PropertyValue, std::string> {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec> || std::is_same_v<T, props::DecimalSpec>) {
          int64_t value = spec.defaultValue;
          if (const auto mn = boundValue(spec.min); mn.has_value() && value < *mn) value = *mn;
          if (const auto mx = boundValue(spec.max); mx.has_value() && value > *mx) value = *mx;
          return PropertyValue{property.id, value};
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          return PropertyValue{property.id, spec.defaultValue};
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          uint32_t index = spec.defaultIndex;
          if (index >= spec.items.size()) index = 0;
          return PropertyValue{property.id, props::EnumIndex{index}};
        } else {
          return std::unexpected(
              std::format("\"{}\": has property type this build does not recognize", property.name));
        }
      },
      property.type);
}

std::expected<void, std::string> checkValue(const Property& property, const PropertyValue& value,
                                            bool capturing) {
  auto rangeCheck = [&](int64_t v, uint32_t digits, const props::Bound& min,
                       const props::Bound& max) -> std::expected<void, std::string> {
    if (const auto mn = boundValue(min); mn.has_value() && v < *mn) {
      return std::unexpected(std::format("\"{}\": {} is below minimum, {}", property.name,
                                         formatNumeric(v, digits, property.unit),
                                         formatNumeric(*mn, digits, property.unit)));
    }
    if (const auto mx = boundValue(max); mx.has_value() && v > *mx) {
      return std::unexpected(std::format("\"{}\": {} is above maximum, {}", property.name,
                                         formatNumeric(v, digits, property.unit),
                                         formatNumeric(*mx, digits, property.unit)));
    }
    return {};
  };

  auto typeResult = std::visit(
      [&](const auto& spec) -> std::expected<void, std::string> {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec>) {
          const auto* v = std::get_if<int64_t>(&value.value);
          if (v == nullptr) {
            return std::unexpected(std::format("\"{}\": expects integer value", property.name));
          }
          return rangeCheck(*v, 0, spec.min, spec.max);
        } else if constexpr (std::is_same_v<T, props::DecimalSpec>) {
          const auto* v = std::get_if<int64_t>(&value.value);
          if (v == nullptr) {
            return std::unexpected(std::format("\"{}\": expects decimal value", property.name));
          }
          return rangeCheck(*v, spec.digits, spec.min, spec.max);
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          if (!std::holds_alternative<bool>(value.value)) {
            return std::unexpected(std::format("\"{}\": expects boolean value", property.name));
          }
          return {};
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          const auto* idx = std::get_if<props::EnumIndex>(&value.value);
          if (idx == nullptr) {
            return std::unexpected(std::format("\"{}\": expects one of its enumeration items", property.name));
          }
          if (idx->index >= spec.items.size()) {
            return std::unexpected(std::format("\"{}\": item {} does not exist ({} item(s))", property.name,
                                               idx->index, spec.items.size()));
          }
          if (std::holds_alternative<props::UnknownMember>(spec.items[idx->index])) {
            return std::unexpected(std::format("\"{}\": item {} is kind this build does not recognize",
                                               property.name, idx->index));
          }
          return {};
        } else {
          return std::unexpected(
              std::format("\"{}\": has property type this build does not recognize", property.name));
        }
      },
      property.type);
  if (!typeResult.has_value()) return typeResult;

  if (capturing && !property.changeableDuringCapture) {
    return std::unexpected(std::format("\"{}\" cannot change during capture", property.name));
  }
  return {};
}

std::expected<void, std::string> checkTree(const std::vector<PropertyNode>& nodes) {
  std::set<std::string> seenIds;
  return checkNodes(nodes, "", seenIds);
}

std::expected<PropertyValue, std::string> parseValue(const Property& property, std::string_view text) {
  return std::visit(
      [&](const auto& spec) -> std::expected<PropertyValue, std::string> {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec>) {
          const auto v = parseNumericText(text, property.unit, 0, property.name);
          if (!v.has_value()) return std::unexpected(v.error());
          return PropertyValue{property.id, *v};
        } else if constexpr (std::is_same_v<T, props::DecimalSpec>) {
          const auto v = parseNumericText(text, property.unit, spec.digits, property.name);
          if (!v.has_value()) return std::unexpected(v.error());
          return PropertyValue{property.id, *v};
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          const auto v = parseBoolean(text);
          if (!v.has_value()) {
            return std::unexpected(std::format(
                "\"{}\": '{}' is not on/off, true/false, 1/0, enable/disable or yes/no", property.name, text));
          }
          return PropertyValue{property.id, *v};
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          for (size_t i = 0; i < spec.items.size(); ++i) {
            if (const auto* s = std::get_if<std::string>(&spec.items[i]); s != nullptr && *s == text) {
              return PropertyValue{property.id, props::EnumIndex{static_cast<uint32_t>(i)}};
            }
          }
          const auto number = parseNumericText(text, property.unit, spec.digits, property.name);
          if (number.has_value()) {
            for (size_t i = 0; i < spec.items.size(); ++i) {
              if (const auto* n = std::get_if<int64_t>(&spec.items[i]); n != nullptr && *n == *number) {
                return PropertyValue{property.id, props::EnumIndex{static_cast<uint32_t>(i)}};
              }
            }
            return std::unexpected(
                std::format("\"{}\": '{}' does not match any of its items", property.name, text));
          }
          return std::unexpected(
              std::format("\"{}\": '{}' is not one of its items by name or by number", property.name, text));
        } else {
          return std::unexpected(
              std::format("\"{}\": has property type this build does not recognize", property.name));
        }
      },
      property.type);
}

std::expected<std::string, std::string> formatValue(const Property& property, const PropertyValue& value) {
  return std::visit(
      [&](const auto& spec) -> std::expected<std::string, std::string> {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, props::IntegerSpec>) {
          const auto* v = std::get_if<int64_t>(&value.value);
          if (v == nullptr) return std::unexpected(std::format("\"{}\": not integer value", property.name));
          return formatNumeric(*v, 0, property.unit);
        } else if constexpr (std::is_same_v<T, props::DecimalSpec>) {
          const auto* v = std::get_if<int64_t>(&value.value);
          if (v == nullptr) return std::unexpected(std::format("\"{}\": not decimal value", property.name));
          return formatNumeric(*v, spec.digits, property.unit);
        } else if constexpr (std::is_same_v<T, props::BooleanSpec>) {
          const auto* v = std::get_if<bool>(&value.value);
          if (v == nullptr) return std::unexpected(std::format("\"{}\": not boolean value", property.name));
          if (spec.style == props::BooleanStyle::enableDisable) {
            return std::string(*v ? "enabled" : "disabled");
          }
          return std::string(*v ? "on" : "off");
        } else if constexpr (std::is_same_v<T, props::EnumerationSpec>) {
          const auto* idx = std::get_if<props::EnumIndex>(&value.value);
          if (idx == nullptr || idx->index >= spec.items.size()) {
            return std::unexpected(std::format("\"{}\": not one of its enumeration items", property.name));
          }
          const auto& item = spec.items[idx->index];
          if (const auto* s = std::get_if<std::string>(&item)) return *s;
          if (const auto* n = std::get_if<int64_t>(&item)) return formatNumeric(*n, spec.digits, property.unit);
          return std::unexpected(std::format("\"{}\": item {} is kind this build does not recognize",
                                             property.name, idx->index));
        } else {
          return std::unexpected(
              std::format("\"{}\": has property type this build does not recognize", property.name));
        }
      },
      property.type);
}

const Property* findProperty(const std::vector<PropertyNode>& nodes, std::string_view id) {
  for (const PropertyNode& n : nodes) {
    if (const auto* p = std::get_if<Property>(&n.node)) {
      if (p->id == id) return p;
    } else if (const auto* g = std::get_if<PropertyGroup>(&n.node)) {
      if (const Property* found = findProperty(g->nodes, id)) return found;
    }
  }
  return nullptr;
}

namespace {
void collectListing(const std::vector<PropertyNode>& nodes, const std::string& path,
                    std::vector<PropertyListing>& out) {
  for (const PropertyNode& n : nodes) {
    if (const auto* p = std::get_if<Property>(&n.node)) {
      out.push_back(PropertyListing{path, p});
    } else if (const auto* g = std::get_if<PropertyGroup>(&n.node)) {
      const std::string childPath = path.empty() ? g->name : path + " > " + g->name;
      collectListing(g->nodes, childPath, out);
    }
  }
}
}

std::vector<PropertyListing> listProperties(const std::vector<PropertyNode>& nodes) {
  std::vector<PropertyListing> out;
  collectListing(nodes, "", out);
  return out;
}

std::expected<std::vector<PropertyValue>, std::string> readSettings(
    const std::vector<PropertyNode>& nodes, const std::map<std::string, std::string>& settings) {
  std::vector<PropertyValue> values;
  values.reserve(settings.size());
  for (const auto& [id, text] : settings) {
    const std::string typed = id + "=" + text + ": ";
    const Property* property = findProperty(nodes, id);
    if (property == nullptr) {
      std::string ids;
      for (const PropertyListing& listed : listProperties(nodes)) {
        ids += (ids.empty() ? "" : ", ") + listed.property->id;
      }
      return std::unexpected(typed + "no property has id '" + id + "'; " +
                             (ids.empty() ? std::string("this device lists no properties") : "ids are: " + ids));
    }
    auto value = parseValue(*property, text);
    if (!value.has_value()) return std::unexpected(typed + value.error());
    constexpr bool capturing = false;
    if (auto allowed = checkValue(*property, *value, capturing); !allowed.has_value()) {
      return std::unexpected(typed + allowed.error());
    }
    values.push_back(std::move(*value));
  }
  return values;
}

}

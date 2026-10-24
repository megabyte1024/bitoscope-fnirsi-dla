/*
 * SPDX-License-Identifier: MIT
 */

#include "display_text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

namespace bitoscope::display_text {

namespace {

static_assert(std::string_view("\u00B2").size() == 2 &&
                  static_cast<unsigned char>(std::string_view("\u00B2")[0]) == 0xC2 &&
                  static_cast<unsigned char>(std::string_view("\u00B2")[1]) == 0xB2,
              "narrow literals must be UTF-8");

struct Builtin {
  std::string_view upper;
  std::string_view shown;
  bool alsoWord;
};

constexpr auto kBuiltin = std::to_array<Builtin>({
    {"SPI", "SPI", false},
    {"QSPI", "QSPI", false},
    {"UART", "UART", false},
    {"TX", "TX", false},
    {"RX", "RX", false},
    {"MOSI", "MOSI", false},
    {"MISO", "MISO", false},
    {"SCK", "SCK", false},
    {"SCLK", "SCLK", false},
    {"CS", "CS", false},
    {"SDA", "SDA", false},
    {"SCL", "SCL", false},
    {"USB", "USB", false},
    {"CAN", "CAN", true},
    {"NOR", "NOR", true},
    {"ACK", "ACK", false},
    {"NACK", "NACK", false},
    {"I2C", "I\u00B2C", false},
    {"I2S", "I\u00B2S", false},
    {"MSB", "MSB", false},
    {"LSB", "LSB", false},
    {"COBS", "COBS", false},
    {"CRC", "CRC", false},
    {"JEDEC", "JEDEC", false},
    {"CLK", "CLK", false},
    {"CPOL", "CPOL", false},
    {"CPHA", "CPHA", false},
});

std::vector<Abbreviation> builtinCopy() {
  std::vector<Abbreviation> out;
  out.reserve(kBuiltin.size());
  for (const Builtin& b : kBuiltin) out.push_back({std::string(b.upper), std::string(b.shown), b.alsoWord});
  return out;
}

std::mutex& tableMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<Abbreviation>& table() {
  static std::vector<Abbreviation> live = builtinCopy();
  return live;
}

// WHY: Non-ASCII bytes are word bytes, so UTF-8 text is never split inside a character.
constexpr bool isWordByte(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u >= 0x80;
}

constexpr char upperAscii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool equalsIgnoreCase(std::string_view word, std::string_view upper) {
  if (word.size() != upper.size()) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (upperAscii(word[i]) != upper[i]) return false;
  }
  return true;
}

bool isAllUpper(std::string_view word) {
  for (const char c : word) {
    if (c >= 'a' && c <= 'z') return false;
  }
  return true;
}

std::string_view shownOf(const Abbreviation& entry) { return entry.shown.empty() ? entry.upper : entry.shown; }

// WHY: Called with the table locked, so `each` must not convert anything itself (deadlock).
template <typename Each>
std::string overWords(std::string_view text, Each each) {
  std::string out;
  out.reserve(text.size() + 4);
  std::size_t i = 0;
  while (i < text.size()) {
    if (!isWordByte(text[i])) {
      out += text[i++];
      continue;
    }
    std::size_t end = i;
    while (end < text.size() && isWordByte(text[end])) ++end;
    const std::string_view word = text.substr(i, end - i);
    out += each(word);
    i = end;
  }
  return out;
}

}

std::optional<std::string> problemWith(const Abbreviation& entry) {
  const std::string named = "'" + entry.upper + "'";
  if (entry.upper.empty()) return std::string("abbreviation needs 'upper'");
  for (const char c : entry.upper) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
    if (static_cast<unsigned char>(c) >= 0x80) {
      return named + " is not ASCII; write ASCII 'upper' and put its own text in 'shown'";
    }
    return named + " must be upper case ASCII letters and digits";
  }
  if (entry.upper == "THE" || entry.upper == "A" || entry.upper == "AN") {
    return named + " is article, and user-visible text has none";
  }
  return std::nullopt;
}

std::span<const Abbreviation> builtinAbbreviations() {
  static const std::vector<Abbreviation> builtin = builtinCopy();
  return builtin;
}

std::vector<Abbreviation> abbreviations() {
  const std::lock_guard lock(tableMutex());
  return table();
}

void setAbbreviations(std::vector<Abbreviation> replacement) {
  const std::lock_guard lock(tableMutex());
  table() = replacement.empty() ? builtinCopy() : std::move(replacement);
}

void addAbbreviations(std::span<const Abbreviation> more) {
  const std::lock_guard lock(tableMutex());
  std::vector<Abbreviation>& live = table();
  for (const Abbreviation& entry : more) {
    const bool there = std::any_of(live.begin(), live.end(), [&entry](const Abbreviation& existing) {
      return equalsIgnoreCase(entry.upper, existing.upper);
    });
    if (!there) live.push_back(entry);
  }
}

void overrideAbbreviations(std::span<const Abbreviation> more) {
  const std::lock_guard lock(tableMutex());
  std::vector<Abbreviation>& live = table();
  for (const Abbreviation& entry : more) {
    const auto there = std::find_if(live.begin(), live.end(), [&entry](const Abbreviation& existing) {
      return equalsIgnoreCase(entry.upper, existing.upper);
    });
    if (there == live.end()) {
      live.push_back(entry);
    } else {
      *there = entry;
    }
  }
}

std::string displayText(std::string_view ascii) {
  const std::lock_guard lock(tableMutex());
  const std::vector<Abbreviation>& live = table();
  return overWords(ascii, [&live](std::string_view word) -> std::string_view {
    for (const Abbreviation& entry : live) {
      if (!equalsIgnoreCase(word, entry.upper)) continue;
      if (entry.alsoWord && !isAllUpper(word)) return word;
      return shownOf(entry);
    }
    return word;
  });
}

std::string displayName(std::string_view name) {
  const std::lock_guard lock(tableMutex());
  const std::vector<Abbreviation>& live = table();
  return overWords(name, [&live](std::string_view word) -> std::string_view {
    for (const Abbreviation& entry : live) {
      if (equalsIgnoreCase(word, entry.upper)) return shownOf(entry);
    }
    return word;
  });
}

std::string shownName(std::string_view name, std::string_view display) {
  return displayName(display.empty() ? name : display);
}

std::string asciiText(std::string_view display) {
  const std::lock_guard lock(tableMutex());
  const std::vector<Abbreviation>& live = table();
  return overWords(display, [&live](std::string_view word) -> std::string_view {
    for (const Abbreviation& entry : live) {
      if (shownOf(entry) != entry.upper && word == shownOf(entry)) return entry.upper;
    }
    return word;
  });
}

}

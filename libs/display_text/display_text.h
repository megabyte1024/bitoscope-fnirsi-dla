/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitoscope::display_text {

struct Abbreviation {
  std::string upper;
  std::string shown;
  bool alsoWord = false;
};

std::optional<std::string> problemWith(const Abbreviation& entry);

std::span<const Abbreviation> builtinAbbreviations();

std::vector<Abbreviation> abbreviations();

void setAbbreviations(std::vector<Abbreviation> table);

void addAbbreviations(std::span<const Abbreviation> more);

void overrideAbbreviations(std::span<const Abbreviation> more);

std::string displayText(std::string_view ascii);

std::string displayName(std::string_view name);

std::string shownName(std::string_view name, std::string_view display);

std::string asciiText(std::string_view display);

}

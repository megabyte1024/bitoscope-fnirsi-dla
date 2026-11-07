/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/manufacturer_app.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace fnirsi::wch {

namespace {

constexpr std::string_view kManufacturerAppProgram = "dla-logic.exe";

bool sameProgram(std::string_view name, std::string_view lowerCaseProgram) {
  return name.size() == lowerCaseProgram.size() &&
         std::equal(name.begin(), name.end(), lowerCaseProgram.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
         });
}

}

std::optional<std::string> manufacturerAppReason(std::span<const std::string> runningPrograms) {
  const bool running = std::any_of(runningPrograms.begin(), runningPrograms.end(), [](const std::string& name) {
    return sameProgram(name, kManufacturerAppProgram);
  });
  if (!running) return std::nullopt;
  return std::string("DLA-Logic is running: close it to use analyzer");
}

}

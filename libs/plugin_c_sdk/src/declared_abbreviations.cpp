/*
 * SPDX-License-Identifier: MIT
 */

#include "declared_abbreviations.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>

#include "bitoscope_plugin_common.h"
#include "display_text.h"
#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

namespace {

std::mutex& listMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<ipc::Abbreviation>& list() {
  static std::vector<ipc::Abbreviation> declared;
  return declared;
}

}

std::vector<ipc::Abbreviation> declaredAbbreviations() {
  const std::lock_guard lock(listMutex());
  return list();
}

void forgetDeclaredAbbreviations() {
  const std::lock_guard lock(listMutex());
  list().clear();
}

int addDeclaredAbbreviation(const char* upper, const char* shown, int alsoWord) {
  namespace dt = bitoscope::display_text;
  const dt::Abbreviation entry{upper != nullptr ? std::string(upper) : std::string(),
                               shown != nullptr ? std::string(shown) : std::string(), alsoWord != 0};
  if (const auto problem = dt::problemWith(entry)) {
    logLine(ipc::LogLevel::error, "bitoscope_add_abbreviation: " + *problem);
    return BITOSCOPE_ERROR;
  }
  {
    const std::lock_guard lock(listMutex());
    std::vector<ipc::Abbreviation>& declared = list();
    const bool there = std::any_of(declared.begin(), declared.end(), [&entry](const ipc::Abbreviation& other) {
      return other.upper == entry.upper;
    });
    if (!there) declared.push_back(ipc::Abbreviation{entry.upper, entry.shown, entry.alsoWord});
  }
  const std::array<dt::Abbreviation, 1> one{entry};
  dt::addAbbreviations(one);
  return BITOSCOPE_OK;
}

}

extern "C" int bitoscope_add_abbreviation(const char* upper, const char* shown, int also_word) {
  try {
    return bitoscope::plugin_c_sdk::addDeclaredAbbreviation(upper, shown, also_word);
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

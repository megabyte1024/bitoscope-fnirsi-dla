/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <vector>

#include "ipc_messages.h"

namespace bitoscope::plugin_c_sdk {

std::vector<ipc::Abbreviation> declaredAbbreviations();

void forgetDeclaredAbbreviations();

int addDeclaredAbbreviation(const char* upper, const char* shown, int alsoWord);

}

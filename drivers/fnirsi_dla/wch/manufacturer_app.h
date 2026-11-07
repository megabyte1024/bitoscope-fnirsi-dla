/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>
#include <span>
#include <string>

namespace fnirsi::wch {

std::optional<std::string> manufacturerAppReason(std::span<const std::string> runningPrograms);

}

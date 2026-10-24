/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace bitoscope::ipc {

struct PluginVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string suffix;

    bool operator==(const PluginVersion&) const = default;
};

constexpr std::size_t maxPluginVersionLength = 32;

std::expected<PluginVersion, std::string> parsePluginVersion(std::string_view text);

}

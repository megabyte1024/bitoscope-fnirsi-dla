/*
 * SPDX-License-Identifier: MIT
 */

#include "plugin_version.h"

#include <cstddef>
#include <limits>

namespace bitoscope::ipc {

namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string refusal(std::string_view text, std::string_view what)
{
    std::string message = "version '";
    message += text;
    message += "' ";
    message += what;
    return message;
}

std::expected<uint32_t, std::string> parseNumber(std::string_view text, std::size_t& pos,
                                                 std::string_view name)
{
    const std::size_t begin = pos;
    uint64_t value = 0;
    while (pos < text.size() && isDigit(text[pos])) {
        value = value * 10 + static_cast<uint64_t>(text[pos] - '0');
        if (value > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(refusal(text, std::string(name) + " number is above 4294967295"));
        }
        ++pos;
    }
    if (pos == begin) {
        return std::unexpected(refusal(text, "needs three numbers"));
    }
    if (text[begin] == '0' && pos - begin > 1) {
        return std::unexpected(refusal(text, std::string(name) + " number has leading zeros"));
    }
    return static_cast<uint32_t>(value);
}

}

std::expected<PluginVersion, std::string> parsePluginVersion(std::string_view text)
{
    if (text.empty()) {
        return std::unexpected(std::string("version is empty"));
    }
    if (text.size() > maxPluginVersionLength) {
        return std::unexpected(std::string("version is longer than 32 characters"));
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) > 0x7F) {
            return std::unexpected(refusal(text, "has character outside ASCII"));
        }
    }

    PluginVersion version;
    std::size_t pos = 0;
    uint32_t* const parts[] = {&version.major, &version.minor, &version.patch};
    const std::string_view names[] = {"major", "minor", "patch"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (pos >= text.size() || text[pos] != '.') {
                return std::unexpected(refusal(text, "needs three numbers"));
            }
            ++pos;
        }
        const auto number = parseNumber(text, pos, names[i]);
        if (!number) {
            return std::unexpected(number.error());
        }
        *parts[i] = *number;
    }

    if (pos == text.size()) {
        return version;
    }
    if (text[pos] != '-') {
        return std::unexpected(refusal(text, "has character that does not belong after patch number"));
    }
    ++pos;
    if (pos == text.size()) {
        return std::unexpected(refusal(text, "has empty suffix after '-'"));
    }
    if (!isLetter(text[pos]) && !isDigit(text[pos])) {
        return std::unexpected(refusal(text, "has suffix that starts with neither letter nor digit"));
    }
    for (std::size_t i = pos; i < text.size(); ++i) {
        const char c = text[i];
        if (!isLetter(c) && !isDigit(c) && c != '.') {
            return std::unexpected(refusal(text, "has suffix with character other than letters, digits and dots"));
        }
    }
    version.suffix = std::string(text.substr(pos));
    return version;
}

}

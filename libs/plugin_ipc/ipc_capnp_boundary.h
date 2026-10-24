/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ipc_decode_error.h"

namespace capnp {
class MessageBuilder;
class MessageReader;
}

namespace bitoscope::ipc::detail {

// WHY: Nesting limit 64 leaves room for about fifteen group levels (3 pointers each)
// while still bounding decoder recursion.
inline constexpr int kNestingLimit = 64;

inline constexpr uint64_t kTraversalWordsPerWord = 8;
inline constexpr uint64_t kTraversalFloorWords = 1024;

using MessageReadFn = std::function<std::optional<DecodeError>(capnp::MessageReader& reader)>;
std::optional<DecodeError> readMessage(std::span<const uint8_t> bytes, const MessageReadFn& read);

using MessageBuildFn = std::function<void(capnp::MessageBuilder& builder)>;
std::expected<std::vector<uint8_t>, std::string> buildMessage(const MessageBuildFn& build);

}

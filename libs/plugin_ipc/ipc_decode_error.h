/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bitoscope::ipc {

enum class DecodeErrorKind {
  empty,
  notWholeWords,
  malformed,
  trailingBytes,
  noMessage,
  unknownMember,
  missingField,
  unknownEnum
};

std::string_view toString(DecodeErrorKind kind);

struct DecodeError {
  DecodeErrorKind kind = DecodeErrorKind::malformed;
  std::string message;
  std::optional<uint64_t> requestId;
  uint16_t member = 0;
};

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <capnp/blob.h>

namespace bitoscope::ipc::detail {

inline std::string toString(capnp::Text::Reader text) { return std::string(text.cStr(), text.size()); }

inline std::vector<uint8_t> toBytes(capnp::Data::Reader data) { return std::vector<uint8_t>(data.begin(), data.end()); }

inline capnp::Text::Reader asText(const std::string& text) { return capnp::Text::Reader(text.c_str(), text.size()); }

inline capnp::Data::Reader asData(const std::vector<uint8_t>& bytes) {
  return capnp::Data::Reader(bytes.data(), bytes.size());
}

}

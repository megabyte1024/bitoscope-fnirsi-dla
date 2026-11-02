/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace fnirsi {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest sha256(std::span<const uint8_t> data);

std::string sha256Hex(const Sha256Digest& digest);

}

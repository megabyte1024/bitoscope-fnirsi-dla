/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace fnirsi {

// WHY: 1 ms lies between clean 1.05 ms and corrupting 0.26 ms transfers; revisit once host wake-up delay is fixed.
inline constexpr uint64_t kMinTransferFillMs = 1;

uint64_t minTransferBytes(uint64_t rate_hz, uint32_t num_channels);

std::expected<void, std::string> checkTransferFillTime(uint32_t transfer_kib, uint64_t rate_hz, uint32_t num_channels);

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "usb_device.h"
#include "usb_stream_transport.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

inline constexpr size_t kWchHeaderBytes = 8;
inline constexpr uint32_t kWchMaxTransferBytes = 0x400000;
inline constexpr size_t kWchCommandBytes = 72;

std::expected<std::array<uint8_t, kWchHeaderBytes>, std::string> wchReadHeader(uint8_t endpointAddress,
                                                                                uint32_t length);

std::expected<std::vector<uint8_t>, std::string> wchWriteRequest(uint8_t endpointAddress,
                                                                 std::span<const uint8_t> data);

std::expected<std::span<const uint8_t>, std::string> parseWchReadReply(std::span<const uint8_t> reply,
                                                                       uint32_t requested);

std::expected<uint32_t, std::string> parseWchWriteReply(std::span<const uint8_t> reply, uint32_t sent);

std::array<uint8_t, kWchCommandBytes> wchSetTimeoutsRequest(uint32_t timeoutMs);

std::array<uint8_t, kWchCommandBytes> wchSetExclusiveRequest(bool exclusive);

TransferStatus wchTransferStatusOf(uint32_t windowsError);

UsbCallKind wchCallKindOf(uint32_t windowsError);

}

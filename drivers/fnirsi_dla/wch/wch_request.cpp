/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/wch_request.h"

#include <algorithm>
#include <format>
#include <optional>

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr uint8_t kDirectionIn = 0x80;
constexpr uint8_t kEndpointNumberMask = 0x0F;
constexpr uint8_t kFirstEndpoint = 1;
constexpr uint8_t kLastEndpoint = 8;
constexpr uint32_t kReadFlag = 0x10000;
constexpr uint32_t kWriteFlag = 0x20000;
constexpr size_t kLengthOffset = 4;
constexpr uint32_t kSetExclusiveFunction = 0x0B;
constexpr uint32_t kSetTimeoutsFunction = 0x0F;
constexpr size_t kTimeoutCount = 4;

constexpr uint32_t kErrorAccessDenied = 5;
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorSharingViolation = 32;
constexpr uint32_t kErrorGeneralFailure = 31;
constexpr uint32_t kErrorSemaphoreTimeout = 121;
constexpr uint32_t kErrorOperationAborted = 995;
constexpr uint32_t kErrorDeviceNotConnected = 1167;
constexpr uint32_t kErrorTimeout = 1460;

void put32(uint8_t* at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t get32(const uint8_t* at) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(at[i]) << (8 * i);
  return value;
}

std::expected<uint32_t, std::string> functionFor(uint8_t endpointAddress, bool read, uint32_t length) {
  const uint8_t number = endpointAddress & kEndpointNumberMask;
  if (number < kFirstEndpoint || number > kLastEndpoint) {
    return std::unexpected(std::format("endpoint {:#04x} is not 1 to {}", endpointAddress, kLastEndpoint));
  }
  const bool in = (endpointAddress & kDirectionIn) != 0;
  if (read && !in) {
    return std::unexpected(std::format("endpoint {:#04x} is OUT: a read needs an IN endpoint", endpointAddress));
  }
  if (!read && in) {
    return std::unexpected(std::format("endpoint {:#04x} is IN: a write needs an OUT endpoint", endpointAddress));
  }
  if (length == 0) return std::unexpected(std::string("length 0 is not a transfer"));
  if (length > kWchMaxTransferBytes) {
    return std::unexpected(std::format("length {} is more than {}, the most one request takes", length,
                                       kWchMaxTransferBytes));
  }
  return static_cast<uint32_t>(number - kFirstEndpoint) | (read ? kReadFlag : kWriteFlag);
}

}

std::expected<std::array<uint8_t, kWchHeaderBytes>, std::string> wchReadHeader(uint8_t endpointAddress,
                                                                                uint32_t length) {
  const auto function = functionFor(endpointAddress, true, length);
  if (!function) return std::unexpected(function.error());
  std::array<uint8_t, kWchHeaderBytes> header{};
  put32(header.data(), *function);
  put32(header.data() + kLengthOffset, length);
  return header;
}

std::expected<std::vector<uint8_t>, std::string> wchWriteRequest(uint8_t endpointAddress,
                                                                 std::span<const uint8_t> data) {
  const uint32_t length = data.size() > kWchMaxTransferBytes ? kWchMaxTransferBytes + 1
                                                             : static_cast<uint32_t>(data.size());
  const auto function = functionFor(endpointAddress, false, length);
  if (!function) {
    if (data.size() > kWchMaxTransferBytes) {
      return std::unexpected(std::format("length {} is more than {}, the most one request takes", data.size(),
                                         kWchMaxTransferBytes));
    }
    return std::unexpected(function.error());
  }
  std::vector<uint8_t> request(kWchHeaderBytes + data.size());
  put32(request.data(), *function);
  put32(request.data() + kLengthOffset, length);
  std::copy(data.begin(), data.end(), request.begin() + kWchHeaderBytes);
  return request;
}

std::expected<std::span<const uint8_t>, std::string> parseWchReadReply(std::span<const uint8_t> reply,
                                                                       uint32_t requested) {
  if (reply.size() < kWchHeaderBytes) {
    return std::unexpected(std::format("reply of {} bytes is shorter than its {}-byte header", reply.size(),
                                       kWchHeaderBytes));
  }
  const uint32_t moved = get32(reply.data() + kLengthOffset);
  if (moved > requested) {
    return std::unexpected(std::format("driver reported {} bytes for a read of {}", moved, requested));
  }
  if (moved > reply.size() - kWchHeaderBytes) {
    return std::unexpected(std::format("driver reported {} bytes but its reply holds {}", moved,
                                       reply.size() - kWchHeaderBytes));
  }
  return reply.subspan(kWchHeaderBytes, moved);
}

std::expected<uint32_t, std::string> parseWchWriteReply(std::span<const uint8_t> reply, uint32_t sent) {
  if (reply.size() < kWchHeaderBytes) {
    return std::unexpected(std::format("reply of {} bytes is shorter than its {}-byte header", reply.size(),
                                       kWchHeaderBytes));
  }
  const uint32_t written = get32(reply.data() + kLengthOffset);
  if (written > sent) {
    return std::unexpected(std::format("driver reported {} bytes written of {} sent", written, sent));
  }
  return written;
}

std::array<uint8_t, kWchCommandBytes> wchSetTimeoutsRequest(uint32_t timeoutMs) {
  std::array<uint8_t, kWchCommandBytes> request{};
  put32(request.data(), kSetTimeoutsFunction);
  put32(request.data() + kLengthOffset, kTimeoutCount * sizeof(uint32_t));
  for (size_t i = 0; i < kTimeoutCount; ++i) put32(request.data() + kWchHeaderBytes + i * sizeof(uint32_t), timeoutMs);
  return request;
}

std::array<uint8_t, kWchCommandBytes> wchSetExclusiveRequest(bool exclusive) {
  std::array<uint8_t, kWchCommandBytes> request{};
  put32(request.data(), kSetExclusiveFunction);
  put32(request.data() + kLengthOffset, 1);
  request[kWchHeaderBytes] = exclusive ? 1 : 0;
  return request;
}

TransferStatus wchTransferStatusOf(uint32_t windowsError) {
  switch (windowsError) {
    case 0:
      return TransferStatus::completed;
    case kErrorGeneralFailure:
      return TransferStatus::stall;
    case kErrorSemaphoreTimeout:
    case kErrorTimeout:
      return TransferStatus::timedOut;
    case kErrorOperationAborted:
      return TransferStatus::cancelled;
    case kErrorFileNotFound:
    case kErrorDeviceNotConnected:
      return TransferStatus::noDevice;
    default:
      return TransferStatus::error;
  }
}

UsbCallKind wchCallKindOf(uint32_t windowsError) {
  switch (windowsError) {
    case 0:
      return UsbCallKind::ok;
    case kErrorSemaphoreTimeout:
    case kErrorTimeout:
      return UsbCallKind::timeout;
    case kErrorFileNotFound:
    case kErrorDeviceNotConnected:
      return UsbCallKind::noDevice;
    case kErrorSharingViolation:
      return UsbCallKind::busy;
    case kErrorAccessDenied:
      return UsbCallKind::access;
    default:
      return UsbCallKind::other;
  }
}

}

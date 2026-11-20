/*
 * SPDX-License-Identifier: MIT
 */

#include "halt_crossing.h"

#include <format>

#include "number_text.h"

namespace fnirsi {

namespace {

constexpr size_t kStreamHaltReplyBytes = 16;
constexpr size_t kStreamStateReplyBytes = 24;

std::string faultName(uint8_t fault) {
  switch (fault) {
    case 1:
      return "FPGA block arrived with receive error";
    case 2:
      return "FPGA receive FIFO overflowed";
    case 3:
      return "FPGA block lost";
    case 4:
      return "host did not read fast enough, buffer full";
    case 5:
      return "USB took more packets than offered";
    case 6:
      return "no USB 3 link";
    default:
      return std::format("fault {}", fault);
  }
}

uint64_t littleEndian(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = bytes.size(); i-- > 0;) value = value << 8 | bytes[i];
  return value;
}

}

std::expected<StreamHalt, std::string> parseStreamHalt(std::span<const uint8_t> reply) {
  if (reply.size() < kStreamHaltReplyBytes) {
    return std::unexpected(std::format("0a a3 reply has {} bytes, needs {}", reply.size(), kStreamHaltReplyBytes));
  }
  if (reply[0] != 0x0A || reply[1] != kStreamHaltCommand) {
    return std::unexpected(std::format("reply to {:02x} {:02x}, not to 0a a3", reply[0], reply[1]));
  }
  if (reply[2] != 1) return std::unexpected(std::format("firmware answered 0a a3 with status {}", reply[2]));
  StreamHalt state{static_cast<uint32_t>(littleEndian(reply.subspan(4, 4))), littleEndian(reply.subspan(8, 8))};
  if (reply.size() >= kStreamStateReplyBytes) {
    state.fault = reply[3];
    state.faultAt = littleEndian(reply.subspan(16, 8));
  }
  return state;
}

HaltCrossing crossHalt(const StreamHalt& now, uint32_t haltsBefore, uint64_t receivedBytes) {
  if (now.halts <= haltsBefore) return {HaltAnswer::notYetSeen, 0};
  if (now.position < receivedBytes) return {HaltAnswer::positionBehindHost, 0};
  return {HaltAnswer::valid, now.position - receivedBytes};
}

std::expected<uint64_t, std::string> askValidBytes(
    const std::function<std::expected<std::vector<uint8_t>, std::string>()>& query, const std::function<void()>& wait,
    uint32_t& haltsSeen, uint64_t receivedBytes) {
  for (int ask = 1; ask <= kStreamHaltAsks; ++ask) {
    const auto reply = query();
    if (!reply) return std::unexpected(reply.error());
    const auto halt = parseStreamHalt(*reply);
    if (!halt) return std::unexpected(halt.error());
    if (halt->halts > haltsSeen + 1) {
      return std::unexpected(std::format("firmware counted {} new endpoint halts, one expected", halt->halts - haltsSeen));
    }
    const HaltCrossing crossing = crossHalt(*halt, haltsSeen, receivedBytes);
    if (crossing.answer == HaltAnswer::valid) {
      haltsSeen = halt->halts;
      return crossing.validBytes;
    }
    if (crossing.answer == HaltAnswer::positionBehindHost) {
      return std::unexpected(std::format("firmware position {} is before host's {}", halt->position, receivedBytes));
    }
    if (ask < kStreamHaltAsks) wait();
  }
  return std::unexpected(std::format("firmware saw no endpoint halt after {} asks", kStreamHaltAsks));
}

std::string faultText(const StreamHalt& state) {
  if (state.fault == 0) return {};
  return std::format("DLA data path stopped at device byte {}: {}", withThousands(state.faultAt),
                     faultName(state.fault));
}

std::expected<std::vector<uint8_t>, std::string> askStreamStateOverControl(const ControlIn& control) {
  std::vector<uint8_t> reply(kStreamStateBytes, 0);
  const auto received = control(kVendorDeviceIn, kStreamHaltCommand, reply);
  if (!received) return std::unexpected("stream state over control endpoint failed: " + received.error());
  reply.resize(*received);
  return reply;
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "firmware_flasher.h"
#include "fnirsi_protocol.h"
#include "update_transport.h"

namespace fnirsi {

inline constexpr uint8_t kOwnBootloaderMark = 1;
inline constexpr uint8_t kFnirsiBootloaderMark = 0;
inline constexpr std::array<uint8_t, 3> kExpectedFlashId{0x85, 0x20, 0x18};

struct OwnBootPart {
  std::string wire_name;
  std::span<const uint8_t> bytes;
};

struct OwnBootHooks {
  std::function<bool()> cancel_requested;
  std::function<void(bool)> set_unsafe_to_stop;
  std::function<void()> images_done;
  std::function<void(FlashPhase, uint64_t, int)> progress;
  std::function<void(std::string_view)> log;
};

class OwnBootUpdate {
 public:
  OwnBootUpdate(UpdateTransport& transport, std::optional<OwnBootPart> mcu, std::optional<OwnBootPart> fpga,
                OwnBootHooks hooks, FlashTiming timing, FlashLimits limits, FlashSleep sleep, FlashClock clock);
  OwnBootUpdate(const OwnBootUpdate&) = delete;
  OwnBootUpdate& operator=(const OwnBootUpdate&) = delete;

  FlashResult run(bool entered_from_program);

 private:
  struct Fault {
    bool usb_lost = false;
    std::string text;
  };
  template <typename T>
  using Io = std::expected<T, Fault>;

  struct Stopped {
    std::string why;
    bool reached = true;
    bool cancelled = false;
  };
  template <typename T>
  using Stage = std::expected<T, Stopped>;

  struct FlashAnswer {
    bool ok = false;
    uint32_t crc = 0;
    std::array<uint8_t, 3> id{};
    uint32_t count = 0;
    uint8_t state = 0;
  };
  enum class FpgaWrite { kWritten, kAlreadyThere };

  bool installerRan() const;
  static Stopped installerDidNotReturn(std::string_view reason);
  Stage<void> update(bool entered_from_program);
  Stage<void> sendMcuPart();
  Io<void> sendMcuOnce();
  Stage<FpgaWrite> writeFpgaPart();
  Io<std::optional<std::string>> sendFpgaOnce(bool& start_refused);
  Io<std::optional<std::string>> sendFpgaChunk(size_t number, size_t total, std::span<const uint8_t> data,
                                               uint32_t accepted, bool resent);
  Io<std::optional<FlashAnswer>> askCount();
  Io<std::vector<std::string>> verifyFpga();
  Stage<std::optional<std::string>> verifyAfterWrite();
  std::string fpgaFailure(uint32_t before_crc, std::string_view why);
  Stage<void> reconnect(std::string_view reason);
  Io<McuVersion> enterBootloader();
  std::optional<std::string> foreignBootloader(const McuVersion& version) const;
  FlashResult finishStopped(const Stopped& stopped);

  Io<FlashAnswer> flashQuery(uint32_t address, uint32_t length, uint32_t crc_in);
  Io<uint32_t> flashCrc(uint32_t start, uint32_t length);
  Io<std::vector<std::pair<uint32_t, uint32_t>>> differingRanges(uint32_t start, std::span<const uint8_t> data);
  Io<McuVersion> version();
  Io<void> storeSelection(bool bootloader);
  Io<void> restart(bool bootloader);
  Io<std::vector<uint8_t>> expectReply(std::span<const uint8_t> packet, std::string_view what,
                                       std::span<const uint8_t> prefix, std::chrono::milliseconds timeout);
  Io<std::optional<std::vector<uint8_t>>> exchange(std::span<const uint8_t> packet, std::string_view what,
                                                   std::chrono::milliseconds timeout);
  Io<void> write(std::span<const uint8_t> packet, std::string_view what, bool gone_after_whole_packet_is_sent = false);
  Io<std::optional<std::vector<uint8_t>>> read(std::chrono::milliseconds timeout);
  Io<void> drain();

  bool cancelRequested() const;
  void progress(FlashPhase phase, uint64_t bytes_sent, int attempt);
  void log(std::string_view line);

  UpdateTransport& transport_;
  std::optional<OwnBootPart> mcu_;
  std::optional<OwnBootPart> fpga_;
  OwnBootHooks hooks_;
  FlashTiming timing_;
  FlashLimits limits_;
  FlashSleep sleep_;
  FlashClock clock_;

  bool had_firmware_ = false;
  bool mcu_incomplete_ = false;
  bool bootloader_installed_ = false;
  bool fpga_changed_ = false;
  int last_attempt_ = 0;
  std::vector<std::string> done_notes_;
};

}

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fnirsi_protocol.h"
#include "update_transport.h"

namespace fnirsi {

struct FlashImages {
  std::string mcu_wire_name;
  std::span<const uint8_t> mcu;
  std::string fpga_wire_name;
  std::span<const uint8_t> fpga;
};

struct FlashTiming {
  std::chrono::milliseconds write_timeout{10'000};
  std::chrono::milliseconds reply_timeout{15'000};
  std::chrono::milliseconds fpga_start_reply_timeout{20'000};
  std::chrono::milliseconds drain_read_timeout{100};
  std::chrono::milliseconds pace{30};
  std::chrono::milliseconds phase_pause{500};
  std::chrono::milliseconds reset_wait{1'000};
};

struct FlashLimits {
  int attempts = 8;
  int writes_per_packet = 6;
  int drain_stalls = 3;
  int drain_replies = 64;
};

enum class FlashPhase { kPreparing, kMcu, kFpga, kFinishing };

struct FlashProgress {
  FlashPhase phase = FlashPhase::kPreparing;
  uint64_t bytes_sent = 0;
  uint64_t bytes_total = 0;
  int attempt = 0;
  int resends = 0;
};

enum class FlashOutcome { kDone, kCancelled, kFailed, kNeedsReplug };

struct FlashResult {
  FlashOutcome outcome = FlashOutcome::kFailed;
  bool can_capture = true;
  int attempts = 0;
  std::string message;
};

enum class CancelAnswer { kAccepted, kNeedsConfirmation, kTooLate };

struct FlashEvents {
  std::function<void(const FlashProgress&)> on_progress;
  std::function<void(std::string_view)> on_log;
};

using FlashSleep = std::function<void(std::chrono::milliseconds)>;

class FirmwareFlasher {
 public:
  FirmwareFlasher(UpdateTransport& transport, FlashImages images, FlashEvents events = {}, FlashTiming timing = {},
                  FlashLimits limits = {}, FlashSleep sleep = nullptr);
  FirmwareFlasher(const FirmwareFlasher&) = delete;
  FirmwareFlasher& operator=(const FirmwareFlasher&) = delete;

  FlashResult run();
  FlashResult startProgram();
  CancelAnswer requestCancel(bool confirm_cannot_capture);

 private:
  enum class Stop { kRestart, kFail, kReplug, kCancel };
  struct Interrupt {
    Stop stop = Stop::kFail;
    std::string why;
    bool device_untouched = false;
  };
  template <typename T>
  using Step = std::expected<T, Interrupt>;
  enum class Image { kMcu, kFpga };

  Step<void> flash();
  Step<void> attemptOnce(std::optional<McuVersion>& mode);
  Step<void> sendImage(Image image);
  Step<void> finishInProgram();
  Step<McuVersion> queryVersion();
  Step<void> resetInto(bool bootloader);
  Step<void> drain();
  Step<std::vector<uint8_t>> exchange(std::span<const uint8_t> packet, std::string_view what,
                                      std::optional<uint8_t> expected_command, std::chrono::milliseconds timeout);
  Step<void> send(std::span<const uint8_t> packet, std::string_view what, bool gone_after_whole_packet_is_sent);
  Step<std::vector<uint8_t>> readReply(std::string_view what, std::chrono::milliseconds timeout);
  Step<void> checkCancel();
  bool leaveInProgram();
  FlashResult finish(FlashOutcome outcome, std::string message);
  std::string captureNote() const;
  void report();
  void log(std::string_view line);

  UpdateTransport& transport_;
  FlashImages images_;
  FlashEvents events_;
  FlashTiming timing_;
  FlashLimits limits_;
  FlashSleep sleep_;
  FlashProgress progress_;

  std::mutex mutex_;
  bool cancel_requested_ = false;
  bool fpga_in_progress_ = false;
  bool fpga_damaged_ = false;
  bool images_done_ = false;
  bool finished_ = false;
};

}

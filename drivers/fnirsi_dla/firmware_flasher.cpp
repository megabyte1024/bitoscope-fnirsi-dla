/*
 * SPDX-License-Identifier: MIT
 */

#include "firmware_flasher.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <thread>
#include <utility>

#include "number_text.h"

namespace fnirsi {

namespace {

constexpr size_t kPacketSize = 512;
constexpr size_t kBootSelectSize = 10;
constexpr size_t kChunkSize = 256;
constexpr size_t kImageStartHeader = 6;
constexpr size_t kChunkHeader = 4;
constexpr size_t kReplyHeader = 3;
constexpr size_t kShownReplyBytes = 16;
constexpr uint8_t kPrefix = 0x0A;
constexpr uint8_t kAccepted = 0x01;

constexpr uint8_t kFlush = 0x0B;
constexpr uint8_t kVersion = 0x81;
constexpr uint8_t kRelease = 0x87;
constexpr uint8_t kBootSelect = 0x89;
constexpr uint8_t kReset = 0x90;
constexpr uint8_t kMcuStart = 0x82;
constexpr uint8_t kMcuChunk = 0x83;
constexpr uint8_t kFpgaStart = 0x85;
constexpr uint8_t kFpgaChunk = 0x86;

void sleepFor(std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); }

std::vector<uint8_t> padded(std::initializer_list<uint8_t> head) {
  std::vector<uint8_t> packet(kPacketSize, 0x00);
  std::ranges::copy(head, packet.begin());
  return packet;
}

std::vector<uint8_t> bootSelectPacket(bool bootloader) {
  std::vector<uint8_t> packet(kBootSelectSize, 0x00);
  packet[0] = kPrefix;
  packet[1] = kBootSelect;
  packet[2] = bootloader ? 0x01 : 0x00;
  return packet;
}

std::vector<uint8_t> imageStartPacket(uint8_t command, uint32_t size, std::string_view name) {
  std::vector<uint8_t> packet = padded({kPrefix, command});
  for (size_t i = 0; i < 4; ++i) packet[2 + i] = static_cast<uint8_t>(size >> (8 * i));
  std::ranges::copy(name, packet.begin() + kImageStartHeader);
  return packet;
}

std::vector<uint8_t> chunkPacket(uint8_t command, std::span<const uint8_t> chunk) {
  std::vector<uint8_t> packet = padded({kPrefix, command});
  packet[2] = static_cast<uint8_t>(chunk.size() & 0xFF);
  packet[3] = static_cast<uint8_t>(chunk.size() >> 8);
  std::ranges::copy(chunk, packet.begin() + kChunkHeader);
  return packet;
}

std::string hexOf(std::span<const uint8_t> bytes) {
  std::string hex;
  for (const uint8_t b : bytes.first((std::min)(bytes.size(), kShownReplyBytes))) {
    hex += std::format("{}{:02x}", hex.empty() ? "" : " ", b);
  }
  return hex.empty() ? std::string("nothing") : hex;
}

std::string durationText(std::chrono::milliseconds duration) {
  const auto count = static_cast<uint64_t>(duration.count());
  if (count % 1000 == 0) return std::format("{} s", withThousands(count / 1000));
  return std::format("{} ms", withThousands(count));
}

uint32_t milliseconds(std::chrono::milliseconds duration) { return static_cast<uint32_t>(duration.count()); }

}

FirmwareFlasher::FirmwareFlasher(UpdateTransport& transport, FlashImages images, FlashEvents events,
                                 FlashTiming timing, FlashLimits limits, FlashSleep sleep)
    : transport_(transport),
      images_(std::move(images)),
      events_(std::move(events)),
      timing_(timing),
      limits_(limits),
      sleep_(sleep ? std::move(sleep) : FlashSleep(&sleepFor)) {}

FlashResult FirmwareFlasher::run() {
  if (images_.mcu.empty() || images_.fpga.empty()) {
    return finish(FlashOutcome::kFailed, "firmware image is empty: nothing was sent to analyzer");
  }
  const size_t longestName = (std::max)(images_.mcu_wire_name.size(), images_.fpga_wire_name.size());
  if (kImageStartHeader + longestName >= kPacketSize) {
    return finish(FlashOutcome::kFailed, "firmware image name is too long: nothing was sent to analyzer");
  }
  progress_.bytes_total = images_.mcu.size() + images_.fpga.size();

  const Step<void> flashed = flash();
  if (flashed) {
    return finish(FlashOutcome::kDone,
                  std::format("firmware updated in attempt {} of {}", progress_.attempt, limits_.attempts));
  }
  const Interrupt& stop = flashed.error();
  switch (stop.stop) {
    case Stop::kReplug:
      if (images_done_) {
        return finish(FlashOutcome::kNeedsReplug,
                      std::format("{}: unplug and plug in analyzer to start its new firmware", stop.why));
      }
      return finish(FlashOutcome::kNeedsReplug,
                    std::format("{}: unplug and plug in analyzer, then start update again{}", stop.why, captureNote()));
    case Stop::kCancel:
      if (leaveInProgram()) {
        return finish(FlashOutcome::kCancelled, std::format("update cancelled; analyzer runs its program{}", captureNote()));
      }
      return finish(FlashOutcome::kNeedsReplug,
                    std::format("update cancelled, but analyzer did not return to its program: unplug and plug in "
                                "analyzer{}",
                                captureNote()));
    case Stop::kRestart:
    case Stop::kFail:
      break;
  }
  if (leaveInProgram()) {
    return finish(FlashOutcome::kFailed,
                  std::format("update failed: {}; analyzer runs its program{}", stop.why, captureNote()));
  }
  return finish(FlashOutcome::kNeedsReplug,
                std::format("update failed: {}; analyzer did not return to its program: unplug and plug in analyzer, "
                            "then start update again{}",
                            stop.why, captureNote()));
}

FlashResult FirmwareFlasher::startProgram() {
  if (leaveInProgram()) return finish(FlashOutcome::kDone, "analyzer runs its program");
  return finish(FlashOutcome::kNeedsReplug, "analyzer did not start its program: unplug and plug in analyzer");
}

CancelAnswer FirmwareFlasher::requestCancel(bool confirm_cannot_capture) {
  std::lock_guard lock(mutex_);
  if (finished_ || images_done_) return CancelAnswer::kTooLate;
  if ((fpga_in_progress_ || fpga_damaged_) && !confirm_cannot_capture) return CancelAnswer::kNeedsConfirmation;
  cancel_requested_ = true;
  return CancelAnswer::kAccepted;
}

FirmwareFlasher::Step<void> FirmwareFlasher::flash() {
  std::optional<McuVersion> mode;
  std::string lastWhy;
  for (int attempt = 1; attempt <= limits_.attempts; ++attempt) {
    progress_.attempt = attempt;
    progress_.bytes_sent = 0;
    progress_.phase = FlashPhase::kPreparing;
    report();
    const Step<void> done = attemptOnce(mode);
    if (done) return {};
    if (done.error().stop != Stop::kRestart) return done;
    lastWhy = done.error().why;
    mode.reset();
    log(std::format("attempt {} of {} stopped: {}", attempt, limits_.attempts, lastWhy));
  }
  return std::unexpected(
      Interrupt{Stop::kFail, std::format("none of {} attempts completed; last one stopped: {}", limits_.attempts, lastWhy)});
}

FirmwareFlasher::Step<void> FirmwareFlasher::attemptOnce(std::optional<McuVersion>& mode) {
  if (auto cancelled = checkCancel(); !cancelled) return cancelled;
  if (auto drained = drain(); !drained) return drained;
  if (!mode) {
    auto version = queryVersion();
    if (!version) return std::unexpected(version.error());
    mode = *version;
  }
  if (!mode->application_running) {
    if (auto reset = resetInto(false); !reset) return reset;
    auto version = queryVersion();
    if (!version) return std::unexpected(version.error());
    mode = *version;
    if (!mode->application_running) {
      return std::unexpected(Interrupt{Stop::kFail, "analyzer did not start its program from bootloader"});
    }
  }
  if (auto cancelled = checkCancel(); !cancelled) return cancelled;
  if (auto released = send(padded({kPrefix, kRelease, 0x01}), "release command", false); !released) return released;
  if (auto drained = drain(); !drained) return drained;
  if (auto reset = resetInto(true); !reset) return reset;
  auto version = queryVersion();
  if (!version) return std::unexpected(version.error());
  mode = *version;
  if (mode->application_running || mode->next_start != NextStart::kBootloader) {
    return std::unexpected(Interrupt{Stop::kFail, "analyzer did not enter its bootloader after reset"});
  }
  sleep_(timing_.phase_pause);
  if (auto sent = sendImage(Image::kMcu); !sent) return sent;
  sleep_(timing_.phase_pause);
  if (auto sent = sendImage(Image::kFpga); !sent) return sent;
  {
    std::lock_guard lock(mutex_);
    images_done_ = true;
    fpga_in_progress_ = false;
    fpga_damaged_ = false;
  }
  progress_.phase = FlashPhase::kFinishing;
  report();
  sleep_(timing_.phase_pause);
  return finishInProgram();
}

FirmwareFlasher::Step<void> FirmwareFlasher::sendImage(Image image) {
  const bool fpga = image == Image::kFpga;
  const std::span<const uint8_t> bytes = fpga ? images_.fpga : images_.mcu;
  const std::string_view name = fpga ? images_.fpga_wire_name : images_.mcu_wire_name;
  const std::string_view label = fpga ? "FPGA" : "MCU";
  const uint8_t startCommand = fpga ? kFpgaStart : kMcuStart;
  const uint8_t chunkCommand = fpga ? kFpgaChunk : kMcuChunk;
  progress_.phase = fpga ? FlashPhase::kFpga : FlashPhase::kMcu;
  report();

  if (fpga) {
    std::lock_guard lock(mutex_);
    if (cancel_requested_) return std::unexpected(Interrupt{Stop::kCancel, {}});
    fpga_in_progress_ = true;
  } else if (auto cancelled = checkCancel(); !cancelled) {
    return cancelled;
  }
  const auto startTimeout = fpga ? timing_.fpga_start_reply_timeout : timing_.reply_timeout;
  auto started = exchange(imageStartPacket(startCommand, static_cast<uint32_t>(bytes.size()), name),
                          std::format("{} image start", label), startCommand, startTimeout);
  if (fpga) {
    std::lock_guard lock(mutex_);
    fpga_in_progress_ = false;
    if (started || !started.error().device_untouched) fpga_damaged_ = true;
  }
  if (!started) return std::unexpected(started.error());

  const size_t chunks = (bytes.size() + kChunkSize - 1) / kChunkSize;
  for (size_t index = 0; index < chunks; ++index) {
    if (auto cancelled = checkCancel(); !cancelled) return cancelled;
    const size_t offset = index * kChunkSize;
    const auto chunk = bytes.subspan(offset, (std::min)(kChunkSize, bytes.size() - offset));
    auto answered = exchange(chunkPacket(chunkCommand, chunk), std::format("{} chunk {} of {}", label, withThousands(index + 1), withThousands(chunks)),
                             chunkCommand, timing_.reply_timeout);
    if (!answered) return std::unexpected(answered.error());
    progress_.bytes_sent += chunk.size();
    report();
  }
  return {};
}

FirmwareFlasher::Step<void> FirmwareFlasher::finishInProgram() {
  const auto replug = [](const Interrupt& cause) {
    return std::unexpected(
        Interrupt{Stop::kReplug, std::format("both images are in flash, but analyzer did not restart: {}", cause.why)});
  };
  if (auto reset = resetInto(false); !reset) return replug(reset.error());
  auto version = queryVersion();
  if (!version) return replug(version.error());
  if (!version->application_running) {
    return std::unexpected(Interrupt{Stop::kReplug, "both images are in flash, but analyzer stayed in its bootloader"});
  }
  return {};
}

FirmwareFlasher::Step<McuVersion> FirmwareFlasher::queryVersion() {
  if (auto flushed = exchange(padded({kPrefix, kFlush}), "flush", std::nullopt, timing_.reply_timeout); !flushed) {
    return std::unexpected(flushed.error());
  }
  auto reply = exchange(padded({kPrefix, kVersion}), "version query", kVersion, timing_.reply_timeout);
  if (!reply) return std::unexpected(reply.error());
  const auto version = parseMcuVersionReply(*reply);
  if (!version) {
    return std::unexpected(
        Interrupt{Stop::kRestart, std::format("version query: reply {} does not decode", hexOf(*reply))});
  }
  return *version;
}

FirmwareFlasher::Step<void> FirmwareFlasher::resetInto(bool bootloader) {
  const std::string what = bootloader ? "boot selection of bootloader" : "boot selection of program";
  if (auto selected = exchange(bootSelectPacket(bootloader), what, kBootSelect, timing_.reply_timeout); !selected) {
    return std::unexpected(selected.error());
  }
  if (auto reset = send(padded({kPrefix, kReset}), "reset", true); !reset) return reset;
  transport_.close();
  sleep_(timing_.reset_wait);
  if (auto reopened = transport_.reopen(); !reopened) {
    return std::unexpected(
        Interrupt{Stop::kReplug, std::format("analyzer did not come back after reset: {}", reopened.error())});
  }
  return drain();
}

FirmwareFlasher::Step<void> FirmwareFlasher::drain() {
  int stalls = 0;
  int replies = 0;
  for (;;) {
    std::vector<uint8_t> reply(kPacketSize);
    const TransferResult r = transport_.readReply(reply, milliseconds(timing_.drain_read_timeout));
    switch (r.status) {
      case TransferStatus::kOk:
        if (++replies > limits_.drain_replies) {
          return std::unexpected(Interrupt{Stop::kReplug, "analyzer keeps sending replies nobody asked for"});
        }
        stalls = 0;
        log(std::format("late reply drained: {}", hexOf(std::span<const uint8_t>(reply).first(r.transferred))));
        continue;
      case TransferStatus::kTimeout:
        return {};
      case TransferStatus::kGone:
        return std::unexpected(Interrupt{Stop::kReplug, "analyzer disconnected"});
      case TransferStatus::kStall:
      case TransferStatus::kError:
        transport_.clearHalts();
        if (++stalls >= limits_.drain_stalls) {
          return std::unexpected(
              Interrupt{Stop::kReplug, std::format("reply endpoint stays stalled after {} clears ({})", stalls, r.detail)});
        }
        continue;
    }
  }
}

FirmwareFlasher::Step<std::vector<uint8_t>> FirmwareFlasher::exchange(std::span<const uint8_t> packet,
                                                                      std::string_view what,
                                                                      std::optional<uint8_t> expected_command,
                                                                      std::chrono::milliseconds timeout) {
  if (auto sent = send(packet, what, false); !sent) return std::unexpected(sent.error());
  sleep_(timing_.pace);
  auto reply = readReply(what, timeout);
  if (!reply) return std::unexpected(reply.error());
  sleep_(timing_.pace);
  if (expected_command) {
    const bool accepted = reply->size() >= kReplyHeader && (*reply)[0] == kPrefix &&
                          (*reply)[1] == *expected_command && (*reply)[2] == kAccepted;
    if (!accepted) {
      return std::unexpected(Interrupt{Stop::kRestart, std::format("{}: unexpected reply {}", what, hexOf(*reply))});
    }
  }
  return reply;
}

FirmwareFlasher::Step<void> FirmwareFlasher::send(std::span<const uint8_t> packet, std::string_view what,
                                                  bool gone_after_whole_packet_is_sent) {
  for (int tryNumber = 1; tryNumber <= limits_.writes_per_packet; ++tryNumber) {
    const TransferResult r = transport_.writeCommand(packet, milliseconds(timing_.write_timeout));
    if (r.status == TransferStatus::kOk && r.transferred == packet.size()) return {};
    if (r.status == TransferStatus::kGone && gone_after_whole_packet_is_sent && r.transferred == packet.size()) {
      return {};
    }
    if (r.status == TransferStatus::kGone) {
      return std::unexpected(Interrupt{Stop::kReplug, std::format("analyzer disconnected during {}", what)});
    }
    if (r.transferred != 0) {
      return std::unexpected(Interrupt{
          Stop::kRestart, std::format("{}: write ended with {} of {} bytes taken", what, withThousands(r.transferred),
                                withThousands(packet.size()))});
    }
    ++progress_.resends;
    log(std::format("{}: write not taken ({}), sending it again", what, r.detail));
    if (tryNumber >= 2) transport_.clearHalts();
  }
  return std::unexpected(Interrupt{
      Stop::kReplug, std::format("{}: analyzer took none of {} writes", what, limits_.writes_per_packet), true});
}

FirmwareFlasher::Step<std::vector<uint8_t>> FirmwareFlasher::readReply(std::string_view what,
                                                                       std::chrono::milliseconds timeout) {
  std::vector<uint8_t> reply(kPacketSize);
  const TransferResult r = transport_.readReply(reply, milliseconds(timeout));
  switch (r.status) {
    case TransferStatus::kOk:
      reply.resize(r.transferred);
      return reply;
    case TransferStatus::kTimeout:
      return std::unexpected(
          Interrupt{Stop::kRestart, std::format("{}: no reply in {}", what, durationText(timeout)), true});
    case TransferStatus::kStall:
      transport_.clearHalts();
      return std::unexpected(Interrupt{Stop::kRestart, std::format("{}: reply endpoint stalled ({})", what, r.detail)});
    case TransferStatus::kGone:
      return std::unexpected(
          Interrupt{Stop::kReplug, std::format("analyzer disconnected while {} waited for reply", what)});
    case TransferStatus::kError:
      break;
  }
  return std::unexpected(Interrupt{Stop::kRestart, std::format("{}: reply failed ({})", what, r.detail)});
}

FirmwareFlasher::Step<void> FirmwareFlasher::checkCancel() {
  std::lock_guard lock(mutex_);
  if (cancel_requested_) return std::unexpected(Interrupt{Stop::kCancel, {}});
  return {};
}

bool FirmwareFlasher::leaveInProgram() {
  if (!drain()) return false;
  auto version = queryVersion();
  if (!version) return false;
  if (version->application_running) return true;
  if (!resetInto(false)) return false;
  auto after = queryVersion();
  return after && after->application_running;
}

FlashResult FirmwareFlasher::finish(FlashOutcome outcome, std::string message) {
  std::lock_guard lock(mutex_);
  finished_ = true;
  return FlashResult{outcome, !fpga_damaged_, progress_.attempt, std::move(message)};
}

std::string FirmwareFlasher::captureNote() const {
  return fpga_damaged_ ? "; analyzer cannot capture until update completes" : "";
}

void FirmwareFlasher::report() {
  if (events_.on_progress) events_.on_progress(progress_);
}

void FirmwareFlasher::log(std::string_view line) {
  if (events_.on_log) events_.on_log(line);
}

}

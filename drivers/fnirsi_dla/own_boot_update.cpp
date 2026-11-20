/*
 * SPDX-License-Identifier: MIT
 */

#include "own_boot_update.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <utility>

#include "firmware_parts.h"
#include "number_text.h"

namespace fnirsi {

namespace {

constexpr size_t kPacketSize = 512;
constexpr size_t kBootSelectSize = 10;
constexpr size_t kChunkSize = 256;
constexpr size_t kImageStartHeader = 6;
constexpr size_t kChunkHeader = 4;
constexpr size_t kShownReplyBytes = 16;
constexpr size_t kFlashAnswerBytes = 16;
constexpr uint32_t kSectorBytes = 0x1000;
constexpr uint32_t kCrcPieceBytes = 0x10000;
constexpr size_t kMinImageBytes = 8;
constexpr uint8_t kPrefix = 0x0A;
constexpr uint8_t kAccepted = 0x01;
constexpr uint8_t kRefused = 0x00;
constexpr uint8_t kFlush = 0x0B;
constexpr uint8_t kVersion = 0x81;
constexpr uint8_t kRelease = 0x87;
constexpr uint8_t kBootSelect = 0x89;
constexpr uint8_t kReset = 0x90;
constexpr uint8_t kMcuStart = 0x82;
constexpr uint8_t kMcuChunk = 0x83;
constexpr uint8_t kFpgaStart = 0x85;
constexpr uint8_t kFpgaChunk = 0x86;
constexpr uint8_t kFlashQuery = 0xB2;
constexpr uint8_t kStateNone = 0;
constexpr uint8_t kStateFpga = 2;
constexpr std::array<uint8_t, 3> kNoFlashLow{0x00, 0x00, 0x00};
constexpr std::array<uint8_t, 3> kNoFlashHigh{0xFF, 0xFF, 0xFF};
constexpr int kFpgaProgressChunks = 16;
constexpr std::string_view kBootloaderInstalledNote = "Bootloader installed. Install firmware now.";

std::vector<uint8_t> padded(std::initializer_list<uint8_t> head) {
  std::vector<uint8_t> packet(kPacketSize, 0x00);
  std::ranges::copy(head, packet.begin());
  return packet;
}

void putU32(std::vector<uint8_t>& packet, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) packet[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t u32At(std::span<const uint8_t> bytes, size_t at) {
  return uint32_t{bytes[at]} | (uint32_t{bytes[at + 1]} << 8) | (uint32_t{bytes[at + 2]} << 16) |
         (uint32_t{bytes[at + 3]} << 24);
}

std::vector<uint8_t> imageStartPacket(uint8_t command, size_t size, std::string_view name) {
  std::vector<uint8_t> packet = padded({kPrefix, command});
  putU32(packet, 2, static_cast<uint32_t>(size));
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

std::string idText(std::span<const uint8_t> id) { return std::format("{:02x} {:02x} {:02x}", id[0], id[1], id[2]); }

std::string countText(std::chrono::milliseconds duration) {
  return std::format("{} s", withThousands(static_cast<uint64_t>(duration.count()) / 1000));
}

uint32_t milliseconds(std::chrono::milliseconds duration) { return static_cast<uint32_t>(duration.count()); }

uint32_t lastSectorEnd(size_t size) {
  return static_cast<uint32_t>((size + kSectorBytes - 1) / kSectorBytes * kSectorBytes);
}

uint32_t erasedCrc(uint32_t length) {
  const std::vector<uint8_t> erased(length, 0xFF);
  return crc32Of(erased);
}

}

OwnBootUpdate::OwnBootUpdate(UpdateTransport& transport, std::optional<OwnBootPart> mcu,
                             std::optional<OwnBootPart> fpga, OwnBootHooks hooks, FlashTiming timing,
                             FlashLimits limits, FlashSleep sleep, FlashClock clock)
    : transport_(transport),
      mcu_(std::move(mcu)),
      fpga_(std::move(fpga)),
      hooks_(std::move(hooks)),
      timing_(timing),
      limits_(limits),
      sleep_(std::move(sleep)),
      clock_(std::move(clock)) {}

bool OwnBootUpdate::installerRan() const { return mcu_ && holdsBootloaderInstallerMark(mcu_->bytes); }

OwnBootUpdate::Stopped OwnBootUpdate::installerDidNotReturn(std::string_view reason) {
  return Stopped{std::format("bootloader installer did not bring analyzer back on USB ({}); if status light flashes red "
                             "and blue, installer failed; if nothing answers after unplug and plug in, bootloader "
                             "must be installed with case open (README, section 'Optional: bootloader')",
                             reason),
                 false};
}

FlashResult OwnBootUpdate::run(bool entered_from_program) {
  had_firmware_ = entered_from_program;
  const Stage<void> updated = update(entered_from_program);
  if (!updated) return finishStopped(updated.error());
  std::string message;
  for (const std::string& note : done_notes_) message += (message.empty() ? "" : " ") + note;
  return FlashResult{FlashOutcome::kDone, !bootloader_installed_, last_attempt_, std::move(message)};
}

OwnBootUpdate::Stage<void> OwnBootUpdate::update(bool entered_from_program) {
  if (mcu_ && (mcu_->bytes.size() < kMinImageBytes || mcu_->bytes.size() > kMaxMcuImageBytes)) {
    return std::unexpected(Stopped{std::format("MCU image of {} bytes does not fit analyzer ({} to {} bytes); nothing "
                                               "written",
                                               withThousands(mcu_->bytes.size()), kMinImageBytes,
                                               withThousands(kMaxMcuImageBytes))});
  }
  if (fpga_ && (fpga_->bytes.size() < kMinImageBytes || fpga_->bytes.size() > kMaxFpgaImageBytes)) {
    return std::unexpected(Stopped{std::format("FPGA image of {} bytes does not fit analyzer ({} to {} bytes); "
                                               "nothing written",
                                               withThousands(fpga_->bytes.size()), kMinImageBytes,
                                               withThousands(kMaxFpgaImageBytes))});
  }
  bool mcuWritten = false;
  if (!entered_from_program && fpga_) {
    log("FPGA image is written after firmware has run since power-up: starting firmware once");
    if (auto restarted = restart(false); !restarted) return std::unexpected(Stopped{restarted.error().text});
    auto now = version();
    if (!now) return std::unexpected(Stopped{now.error().text});
    if (now->application_running) {
      had_firmware_ = true;
      auto entered = enterBootloader();
      if (!entered) return std::unexpected(Stopped{entered.error().text});
      if (auto foreign = foreignBootloader(*entered)) return std::unexpected(Stopped{*foreign});
    } else if (mcu_) {
      log("no firmware installed: writing MCU image first");
      if (auto stored = storeSelection(true); !stored) return std::unexpected(Stopped{stored.error().text});
      if (auto sent = sendMcuPart(); !sent) return sent;
      mcuWritten = true;
      had_firmware_ = true;
      if (auto restarted = restart(false); !restarted) return std::unexpected(Stopped{restarted.error().text});
      auto started = version();
      if (!started) return std::unexpected(Stopped{started.error().text});
      if (!started->application_running) {
        if (installerRan() && started->bootloader_mark == kOwnBootloaderMark) {
          had_firmware_ = false;
          return std::unexpected(Stopped{"bootloader installed, but FPGA image is not written yet: install firmware, "
                                         "then start update again"});
        }
        return std::unexpected(Stopped{"firmware did not start"});
      }
      auto entered = enterBootloader();
      if (!entered) return std::unexpected(Stopped{entered.error().text});
      if (auto foreign = foreignBootloader(*entered)) return std::unexpected(Stopped{*foreign});
    }
  }
  if (cancelRequested()) return std::unexpected(Stopped{{}, true, true});
  if (auto stored = storeSelection(true); !stored) return std::unexpected(Stopped{stored.error().text});
  sleep_(timing_.phase_pause);
  if (mcu_ && !mcuWritten) {
    if (auto sent = sendMcuPart(); !sent) return sent;
    sleep_(timing_.phase_pause);
  }
  if (fpga_) {
    auto written = writeFpgaPart();
    if (!written) return std::unexpected(written.error());
  }
  if (hooks_.images_done) hooks_.images_done();
  progress(FlashPhase::kFinishing, (mcu_ ? mcu_->bytes.size() : 0) + (fpga_ ? fpga_->bytes.size() : 0),
           last_attempt_);
  if (auto restarted = restart(false); !restarted) {
    if (installerRan()) return std::unexpected(installerDidNotReturn(restarted.error().text));
    return std::unexpected(Stopped{std::format("images are in flash, but analyzer did not restart: {}",
                                               restarted.error().text)});
  }
  auto after = version();
  if (!after) {
    if (installerRan()) return std::unexpected(installerDidNotReturn(after.error().text));
    return std::unexpected(
        Stopped{std::format("images are in flash, but analyzer did not answer after restart: {}", after.error().text)});
  }
  if (!after->application_running) {
    if (installerRan()) {
      if (auto foreign = foreignBootloader(*after)) {
        return std::unexpected(Stopped{std::format("bootloader installer finished, but {}", *foreign)});
      }
      done_notes_.push_back(std::string(kBootloaderInstalledNote));
      bootloader_installed_ = true;
      return {};
    }
    if (had_firmware_ || mcu_) return std::unexpected(Stopped{"firmware did not start"});
    done_notes_.push_back("No firmware installed: analyzer waits in its bootloader until MCU firmware is installed.");
  }
  return {};
}

OwnBootUpdate::Stage<void> OwnBootUpdate::sendMcuPart() {
  const int attempts = limits_.own_image_attempts;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    last_attempt_ = attempt;
    progress(FlashPhase::kMcu, 0, attempt);
    const Io<void> sent = sendMcuOnce();
    if (sent) {
      mcu_incomplete_ = false;
      if (hooks_.set_unsafe_to_stop) hooks_.set_unsafe_to_stop(false);
      done_notes_.push_back(std::format("MCU image written ({} bytes).", withThousands(mcu_->bytes.size())));
      return {};
    }
    const std::string reason = sent.error().text;
    if (reason.empty()) return std::unexpected(Stopped{{}, true, true});
    if (attempt == attempts) {
      return std::unexpected(Stopped{std::format("MCU image incomplete: {}, {} attempts made", reason, attempts)});
    }
    if (sent.error().usb_lost) {
      if (auto back = reconnect(reason); !back) {
        return std::unexpected(Stopped{std::format("MCU image incomplete: {}; {}; {} of {} attempts made", reason,
                                                   back.error().why, attempt, attempts),
                                       back.error().reached});
      }
    } else {
      log(std::format("MCU image: {}", reason));
      if (auto drained = drain(); !drained) {
        if (auto back = reconnect(drained.error().text); !back) {
          return std::unexpected(Stopped{std::format("MCU image incomplete: {}; {}; {} of {} attempts made", reason,
                                                     back.error().why, attempt, attempts),
                                         back.error().reached});
        }
      }
    }
    log(std::format("MCU image: starting again ({} of {})", attempt + 1, attempts));
  }
  return std::unexpected(Stopped{"MCU image incomplete"});
}

OwnBootUpdate::Io<void> OwnBootUpdate::sendMcuOnce() {
  const std::span<const uint8_t> bytes = mcu_->bytes;
  const size_t chunks = (bytes.size() + kChunkSize - 1) / kChunkSize;
  std::string place = "image start";
  const auto lostAt = [&place](const Fault& fault) {
    return std::unexpected(Fault{fault.usb_lost, fault.usb_lost ? std::format("USB lost at {}: {}", place, fault.text)
                                                                : fault.text});
  };
  if (hooks_.set_unsafe_to_stop) hooks_.set_unsafe_to_stop(true);
  mcu_incomplete_ = true;
  const std::array<uint8_t, 3> startOk{kPrefix, kMcuStart, kAccepted};
  if (auto started = expectReply(imageStartPacket(kMcuStart, bytes.size(), mcu_->wire_name), "MCU image start",
                                 startOk, timing_.fpga_start_reply_timeout);
      !started) {
    return lostAt(started.error());
  }
  const std::array<uint8_t, 3> chunkOk{kPrefix, kMcuChunk, kAccepted};
  for (size_t index = 0; index < chunks; ++index) {
    if (cancelRequested()) return std::unexpected(Fault{false, {}});
    const size_t offset = index * kChunkSize;
    const auto chunk = bytes.subspan(offset, (std::min)(kChunkSize, bytes.size() - offset));
    place = std::format("chunk {} of {}", withThousands(index + 1), withThousands(chunks));
    if (auto answered = expectReply(chunkPacket(kMcuChunk, chunk), std::format("MCU {}", place), chunkOk,
                                    timing_.reply_timeout);
        !answered) {
      return lostAt(answered.error());
    }
    progress(FlashPhase::kMcu, offset + chunk.size(), last_attempt_);
  }
  return {};
}

OwnBootUpdate::Stage<OwnBootUpdate::FpgaWrite> OwnBootUpdate::writeFpgaPart() {
  const std::span<const uint8_t> image = fpga_->bytes;
  const uint32_t size = static_cast<uint32_t>(image.size());
  const uint32_t fileCrc = crc32Of(image);
  const uint64_t mcuBytes = mcu_ ? mcu_->bytes.size() : 0;
  progress(FlashPhase::kFpga, mcuBytes, 1);
  const auto beforeWrite = [](const Fault& fault) {
    return std::unexpected(Stopped{std::format("FPGA image: {}; nothing written",
                                               fault.usb_lost ? "USB lost before writing: " + fault.text : fault.text)});
  };
  auto identity = flashQuery(0, 0, 0);
  if (!identity) return beforeWrite(identity.error());
  if (identity->id == kNoFlashLow || identity->id == kNoFlashHigh) {
    return std::unexpected(Stopped{"FPGA image: no flash answer; nothing written"});
  }
  if (identity->id != kExpectedFlashId) {
    return std::unexpected(Stopped{std::format("FPGA image: flash id {} is not {} (PUYA 16 MiB); nothing written",
                                               idText(identity->id), idText(kExpectedFlashId))});
  }
  auto before = flashCrc(0, size);
  if (!before) return beforeWrite(before.error());
  const uint32_t tail = lastSectorEnd(size) - size;
  auto tailNow = flashCrc(size, tail);
  if (!tailNow) return beforeWrite(tailNow.error());
  if (*before == fileCrc && *tailNow == erasedCrc(tail)) {
    done_notes_.push_back(std::format("FPGA image already in analyzer (CRC {:08x}); nothing written.", fileCrc));
    progress(FlashPhase::kFpga, mcuBytes + size, 1);
    return FpgaWrite::kAlreadyThere;
  }
  log(std::format("writing FPGA image, {} bytes", withThousands(size)));
  const int attempts = limits_.own_image_attempts;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    last_attempt_ = attempt;
    progress(FlashPhase::kFpga, mcuBytes, attempt);
    bool startRefused = false;
    bool reported = false;
    std::optional<std::string> reason;
    auto sent = sendFpgaOnce(startRefused);
    if (!sent) {
      if (sent.error().text.empty()) return std::unexpected(Stopped{{}, true, true});
      if (!sent.error().usb_lost) return std::unexpected(Stopped{fpgaFailure(*before, sent.error().text)});
      reason = sent.error().text;
      if (auto back = reconnect(*reason); !back) {
        return std::unexpected(Stopped{fpgaFailure(*before, std::format("{}; {}; {} of {} attempts made", *reason,
                                                                         back.error().why, attempt, attempts)),
                                       back.error().reached});
      }
      reported = true;
    } else if (startRefused) {
      if (attempt == 1) return std::unexpected(Stopped{"FPGA image: analyzer refused start; nothing written"});
      reason = "analyzer refused start";
    } else {
      reason = *sent;
    }
    if (!reason) {
      auto verified = verifyAfterWrite();
      if (!verified) {
        return std::unexpected(Stopped{fpgaFailure(*before, verified.error().why), verified.error().reached});
      }
      reason = *verified;
      if (!reason) {
        fpga_changed_ = false;
        if (hooks_.set_unsafe_to_stop) hooks_.set_unsafe_to_stop(false);
        done_notes_.push_back(std::format("FPGA image written and verified ({} bytes, CRC {:08x}). Unplug analyzer "
                                          "and plug it in again to load it.",
                                          withThousands(size), fileCrc));
        progress(FlashPhase::kFpga, mcuBytes + size, attempt);
        return FpgaWrite::kWritten;
      }
    }
    if (attempt == attempts) {
      return std::unexpected(Stopped{fpgaFailure(*before, std::format("{}, {} attempts made", *reason, attempts))});
    }
    if (reported) {
      log(std::format("FPGA image: starting again ({} of {})", attempt + 1, attempts));
    } else {
      log(std::format("FPGA image: {}; starting again ({} of {})", *reason, attempt + 1, attempts));
    }
  }
  return std::unexpected(Stopped{fpgaFailure(*before, "FPGA image incomplete")});
}

OwnBootUpdate::Io<std::optional<std::string>> OwnBootUpdate::sendFpgaOnce(bool& start_refused) {
  const std::span<const uint8_t> image = fpga_->bytes;
  const size_t chunks = (image.size() + kChunkSize - 1) / kChunkSize;
  const uint64_t mcuBytes = mcu_ ? mcu_->bytes.size() : 0;
  std::string place = "image start";
  const auto lostAt = [&place](const Fault& fault) {
    return std::unexpected(Fault{fault.usb_lost, fault.usb_lost ? std::format("USB lost at {}: {}", place, fault.text)
                                                                : fault.text});
  };
  if (cancelRequested()) return std::unexpected(Fault{false, {}});
  auto started = exchange(imageStartPacket(kFpgaStart, image.size(), fpga_->wire_name), "FPGA image start",
                          timing_.fpga_start_reply_timeout);
  if (!started) return lostAt(started.error());
  const auto verdict = [](const std::optional<std::vector<uint8_t>>& reply, uint8_t command) -> std::optional<bool> {
    if (!reply || reply->size() < 3 || (*reply)[0] != kPrefix || (*reply)[1] != command) return std::nullopt;
    if ((*reply)[2] != kAccepted && (*reply)[2] != kRefused) return std::nullopt;
    return (*reply)[2] == kAccepted;
  };
  const std::optional<bool> startVerdict = verdict(*started, kFpgaStart);
  if (startVerdict == false) {
    start_refused = true;
    return std::optional<std::string>{};
  }
  if (!startVerdict) {
    auto answer = askCount();
    if (!answer) return lostAt(answer.error());
    if (!*answer || (*answer)->state != kStateFpga || (*answer)->count != 0) {
      return std::optional<std::string>{"no clear answer to start"};
    }
  }
  uint32_t accepted = 0;
  for (size_t number = 1; number <= chunks; ++number) {
    if (cancelRequested()) return std::unexpected(Fault{false, {}});
    if (number == 1) {
      fpga_changed_ = true;
      if (hooks_.set_unsafe_to_stop) hooks_.set_unsafe_to_stop(true);
    }
    const size_t offset = (number - 1) * kChunkSize;
    const auto data = image.subspan(offset, (std::min)(kChunkSize, image.size() - offset));
    place = std::format("chunk {} of {}", withThousands(number), withThousands(chunks));
    auto reason = sendFpgaChunk(number, chunks, data, accepted, false);
    if (!reason) return lostAt(reason.error());
    if (*reason) return *reason;
    accepted += static_cast<uint32_t>(data.size());
    if (number % kFpgaProgressChunks == 0 || number == chunks) {
      progress(FlashPhase::kFpga, mcuBytes + accepted, last_attempt_);
    }
  }
  return std::optional<std::string>{};
}

OwnBootUpdate::Io<std::optional<std::string>> OwnBootUpdate::sendFpgaChunk(size_t number, size_t total,
                                                                          std::span<const uint8_t> data,
                                                                          uint32_t accepted, bool resent) {
  auto reply = exchange(chunkPacket(kFpgaChunk, data), std::format("FPGA chunk {}", withThousands(number)),
                        timing_.reply_timeout);
  if (!reply) return std::unexpected(reply.error());
  const std::optional<std::vector<uint8_t>>& bytes = *reply;
  const bool wellFormed = bytes && bytes->size() >= 3 && (*bytes)[0] == kPrefix && (*bytes)[1] == kFpgaChunk &&
                          ((*bytes)[2] == kAccepted || (*bytes)[2] == kRefused);
  if (wellFormed && (*bytes)[2] == kAccepted) return std::optional<std::string>{};
  if (wellFormed) {
    return std::optional<std::string>{
        std::format("chunk {} of {} refused", withThousands(number), withThousands(total))};
  }
  log(std::format("FPGA image: no answer to chunk {} of {}; asking analyzer how many bytes it holds",
                  withThousands(number), withThousands(total)));
  auto answer = askCount();
  if (!answer) return std::unexpected(answer.error());
  if (!*answer) {
    return std::optional<std::string>{
        std::format("no answer about chunk {} of {}", withThousands(number), withThousands(total))};
  }
  const uint32_t size = static_cast<uint32_t>(fpga_->bytes.size());
  const uint32_t after = accepted + static_cast<uint32_t>(data.size());
  const FlashAnswer& held = **answer;
  if (held.count == after && (held.state == kStateFpga || (after == size && held.state == kStateNone))) {
    return std::optional<std::string>{};
  }
  if (held.count == accepted && held.state == kStateFpga && !resent) {
    log(std::format("FPGA image: chunk {} did not arrive; sending it again", withThousands(number)));
    return sendFpgaChunk(number, total, data, accepted, true);
  }
  return std::optional<std::string>{std::format("analyzer holds {} bytes after chunk {} of {}",
                                                withThousands(held.count), withThousands(number),
                                                withThousands(total))};
}

OwnBootUpdate::Io<std::optional<OwnBootUpdate::FlashAnswer>> OwnBootUpdate::askCount() {
  if (auto drained = drain(); !drained) return std::unexpected(drained.error());
  auto answer = flashQuery(0, 0, 0);
  if (answer) return std::optional<FlashAnswer>{*answer};
  if (answer.error().usb_lost) return std::unexpected(answer.error());
  return std::optional<FlashAnswer>{};
}

OwnBootUpdate::Io<std::vector<std::string>> OwnBootUpdate::verifyFpga() {
  const std::span<const uint8_t> image = fpga_->bytes;
  const uint32_t size = static_cast<uint32_t>(image.size());
  std::vector<std::string> problems;
  auto answer = flashQuery(0, 0, 0);
  if (!answer) return std::unexpected(answer.error());
  if (answer->count != size || answer->state != kStateNone) {
    problems.push_back(std::format("analyzer reports {} of {} bytes accepted and image {}", withThousands(answer->count),
                                   withThousands(size), answer->state == kStateNone ? "closed" : "still open"));
  }
  auto inImage = differingRanges(0, image);
  if (!inImage) return std::unexpected(inImage.error());
  for (const auto& [first, last] : *inImage) {
    problems.push_back(std::format("bytes {}-{} read back differ from file", withThousands(first), withThousands(last)));
  }
  const std::vector<uint8_t> erased(lastSectorEnd(size) - size, 0xFF);
  auto afterImage = differingRanges(size, erased);
  if (!afterImage) return std::unexpected(afterImage.error());
  for (const auto& [first, last] : *afterImage) {
    problems.push_back(std::format("bytes {}-{} after image are not erased", withThousands(first), withThousands(last)));
  }
  return problems;
}

OwnBootUpdate::Stage<std::optional<std::string>> OwnBootUpdate::verifyAfterWrite() {
  bool lostHere = false;
  const int attempts = limits_.own_image_attempts;
  for (int tries = 1; tries <= attempts; ++tries) {
    auto problems = verifyFpga();
    if (problems) {
      if (problems->empty()) return std::optional<std::string>{};
      std::string joined;
      for (const std::string& p : *problems) joined += (joined.empty() ? "" : "; ") + p;
      if (!lostHere) return std::unexpected(Stopped{joined});
      return std::optional<std::string>{joined};
    }
    const std::string reason = problems.error().usb_lost
                                   ? std::format("USB lost at verification: {}", problems.error().text)
                                   : problems.error().text;
    if (!problems.error().usb_lost) return std::unexpected(Stopped{reason});
    if (auto back = reconnect(reason); !back) {
      return std::unexpected(Stopped{std::format("{}; {}", reason, back.error().why), back.error().reached});
    }
    lostHere = true;
    if (tries == attempts) return std::unexpected(Stopped{std::format("{}, {} attempts made", reason, attempts)});
    log("FPGA image: verifying again");
  }
  return std::optional<std::string>{};
}

std::string OwnBootUpdate::fpgaFailure(uint32_t before_crc, std::string_view why) {
  const std::span<const uint8_t> image = fpga_->bytes;
  const uint32_t fileCrc = crc32Of(image);
  constexpr std::string_view kConsequence =
      "Analyzer cannot capture after it is unplugged until FPGA image is written completely. Run update again.";
  if (auto drained = drain(); !drained) {
    return std::format("FPGA image: {}. Analyzer does not answer, so FPGA image may be incomplete. {}", why,
                       kConsequence);
  }
  auto now = flashCrc(0, static_cast<uint32_t>(image.size()));
  if (!now) {
    return std::format("FPGA image: {}. Analyzer does not answer, so FPGA image may be incomplete. {}", why,
                       kConsequence);
  }
  if (*now == before_crc) {
    fpga_changed_ = false;
    return std::format("FPGA image: {}. FPGA image unchanged (CRC {:08x}). Run update again.", why, *now);
  }
  return std::format("FPGA image incomplete: {} (read back CRC {:08x}, file {:08x}). {}", why, *now, fileCrc,
                     kConsequence);
}

OwnBootUpdate::Stage<void> OwnBootUpdate::reconnect(std::string_view reason) {
  log(std::format("{}; waiting up to {} for analyzer on USB", reason, countText(timing_.usb_return_wait)));
  transport_.close();
  const std::chrono::milliseconds deadline = clock_() + timing_.usb_return_wait;
  std::string last = "not found";
  std::optional<McuVersion> back;
  for (;;) {
    if (auto reopened = transport_.reopen(); reopened) {
      if (auto drained = drain(); drained) {
        auto now = version();
        if (now) {
          back = *now;
          break;
        }
        last = now.error().text;
      } else {
        last = drained.error().text;
      }
      transport_.close();
    } else {
      last = reopened.error();
    }
    if (clock_() >= deadline) {
      return std::unexpected(Stopped{std::format("analyzer did not come back on USB within {} ({})",
                                                 countText(timing_.usb_return_wait), last),
                                     false});
    }
    sleep_(timing_.usb_return_poll);
  }
  if (back->application_running) {
    auto entered = enterBootloader();
    if (!entered) return std::unexpected(Stopped{entered.error().text});
    back = *entered;
  }
  if (auto foreign = foreignBootloader(*back)) return std::unexpected(Stopped{*foreign});
  if (auto stored = storeSelection(true); !stored) return std::unexpected(Stopped{stored.error().text});
  sleep_(timing_.phase_pause);
  log("analyzer is back in its own bootloader");
  return {};
}

OwnBootUpdate::Io<McuVersion> OwnBootUpdate::enterBootloader() {
  log("restarting analyzer into its bootloader");
  if (auto released = write(padded({kPrefix, kRelease, 0x01}), "release command"); !released) {
    return std::unexpected(released.error());
  }
  if (auto drained = drain(); !drained) return std::unexpected(drained.error());
  if (auto restarted = restart(true); !restarted) return std::unexpected(restarted.error());
  auto now = version();
  if (!now) return std::unexpected(now.error());
  if (now->application_running || now->next_start != NextStart::kBootloader) {
    return std::unexpected(Fault{false, "analyzer did not stay in its bootloader"});
  }
  return now;
}

std::optional<std::string> OwnBootUpdate::foreignBootloader(const McuVersion& version) const {
  if (version.bootloader_mark == kOwnBootloaderMark) return std::nullopt;
  if (version.bootloader_mark == kFnirsiBootloaderMark) {
    return "analyzer now runs FNIRSI bootloader or own bootloader without FPGA support (byte 9 of version reply is "
           "0); nothing more written";
  }
  return std::format("analyzer now runs unknown bootloader (byte 9 of version reply is {}); nothing more written",
                     version.bootloader_mark);
}

FlashResult OwnBootUpdate::finishStopped(const Stopped& stopped) {
  const bool canCapture = !fpga_changed_;
  const std::string captureNote =
      canCapture ? "" : "; analyzer cannot capture after it is unplugged until update completes";
  std::string done;
  for (const std::string& note : done_notes_) done += note + " ";
  const std::string what =
      done + (stopped.cancelled ? std::string("update cancelled") : std::format("update failed: {}", stopped.why));
  if (!stopped.reached) {
    return FlashResult{FlashOutcome::kNeedsReplug, canCapture, last_attempt_,
                       std::format("{}: unplug and plug in analyzer, then start update again", what)};
  }
  if (mcu_incomplete_) {
    return FlashResult{stopped.cancelled ? FlashOutcome::kCancelled : FlashOutcome::kFailed, false, last_attempt_,
                       std::format("{}; analyzer waits in its bootloader until update runs again to its end", what)};
  }
  const bool drained = drain().has_value();
  const auto now = drained ? version() : Io<McuVersion>(std::unexpected(Fault{}));
  bool running = now && now->application_running;
  if (now && !running && restart(false)) {
    const auto after = version();
    running = after && after->application_running;
  }
  const FlashOutcome outcome = stopped.cancelled ? FlashOutcome::kCancelled : FlashOutcome::kFailed;
  if (running) {
    return FlashResult{outcome, canCapture, last_attempt_,
                       std::format("{}; analyzer runs its program{}", what, captureNote)};
  }
  if (now && !had_firmware_) {
    return FlashResult{outcome, canCapture, last_attempt_,
                       std::format("{}; analyzer waits in its bootloader (no firmware installed){}", what, captureNote)};
  }
  return FlashResult{FlashOutcome::kNeedsReplug, canCapture, last_attempt_,
                     std::format("{}; analyzer did not return to its program: unplug and plug in analyzer, then start "
                                 "update again{}",
                                 what, captureNote)};
}

OwnBootUpdate::Io<OwnBootUpdate::FlashAnswer> OwnBootUpdate::flashQuery(uint32_t address, uint32_t length,
                                                                         uint32_t crc_in) {
  std::vector<uint8_t> packet = padded({kPrefix, kFlashQuery});
  putU32(packet, 2, address);
  putU32(packet, 6, length);
  putU32(packet, 10, crc_in);
  const std::array<uint8_t, 2> prefix{kPrefix, kFlashQuery};
  auto reply = expectReply(packet, "flash check", prefix, timing_.reply_timeout);
  if (!reply) return std::unexpected(reply.error());
  if (reply->size() < kFlashAnswerBytes) {
    return std::unexpected(Fault{false, std::format("flash check: answer of {} bytes is too short", reply->size())});
  }
  FlashAnswer answer;
  answer.ok = (*reply)[2] == kAccepted;
  answer.crc = u32At(*reply, 4);
  std::copy_n(reply->begin() + 8, 3, answer.id.begin());
  answer.count = uint32_t{(*reply)[12]} | (uint32_t{(*reply)[13]} << 8) | (uint32_t{(*reply)[14]} << 16);
  answer.state = (*reply)[15];
  if (!answer.ok) {
    return std::unexpected(Fault{false, std::format("flash check at {}, {} bytes: analyzer refused",
                                                    withThousands(address), withThousands(length))});
  }
  return answer;
}

OwnBootUpdate::Io<uint32_t> OwnBootUpdate::flashCrc(uint32_t start, uint32_t length) {
  uint32_t crc = 0;
  for (uint32_t offset = 0; offset < length; offset += kCrcPieceBytes) {
    auto answer = flashQuery(start + offset, (std::min)(kCrcPieceBytes, length - offset), crc);
    if (!answer) return std::unexpected(answer.error());
    crc = answer->crc;
  }
  return crc;
}

OwnBootUpdate::Io<std::vector<std::pair<uint32_t, uint32_t>>> OwnBootUpdate::differingRanges(
    uint32_t start, std::span<const uint8_t> data) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  uint32_t expected = 0;
  for (size_t offset = 0; offset < data.size(); offset += kCrcPieceBytes) {
    const auto piece = data.subspan(offset, (std::min)(size_t{kCrcPieceBytes}, data.size() - offset));
    const uint32_t following = crc32Of(piece, expected);
    auto answer = flashQuery(start + static_cast<uint32_t>(offset), static_cast<uint32_t>(piece.size()), expected);
    if (!answer) return std::unexpected(answer.error());
    if (answer->crc != following) {
      ranges.emplace_back(start + static_cast<uint32_t>(offset),
                          start + static_cast<uint32_t>(offset + piece.size() - 1));
    }
    expected = following;
  }
  return ranges;
}

OwnBootUpdate::Io<McuVersion> OwnBootUpdate::version() {
  if (auto flushed = exchange(padded({kPrefix, kFlush}), "flush", timing_.reply_timeout); !flushed) {
    return std::unexpected(flushed.error());
  }
  const std::array<uint8_t, 3> prefix{kPrefix, kVersion, kAccepted};
  auto reply = expectReply(padded({kPrefix, kVersion}), "version query", prefix, timing_.reply_timeout);
  if (!reply) return std::unexpected(reply.error());
  const auto parsed = parseMcuVersionReply(*reply);
  if (!parsed) return std::unexpected(Fault{false, std::format("version query: reply {} does not decode", hexOf(*reply))});
  return *parsed;
}

OwnBootUpdate::Io<void> OwnBootUpdate::storeSelection(bool bootloader) {
  std::vector<uint8_t> packet(kBootSelectSize, 0x00);
  packet[0] = kPrefix;
  packet[1] = kBootSelect;
  packet[2] = bootloader ? 0x01 : 0x00;
  const std::array<uint8_t, 3> prefix{kPrefix, kBootSelect, kAccepted};
  auto reply = expectReply(packet, "start selection", prefix, timing_.reply_timeout);
  if (!reply) return std::unexpected(reply.error());
  return {};
}

OwnBootUpdate::Io<void> OwnBootUpdate::restart(bool bootloader) {
  if (auto stored = storeSelection(bootloader); !stored) return stored;
  if (auto reset = write(padded({kPrefix, kReset}), "restart", true); !reset) return reset;
  transport_.close();
  sleep_(timing_.reset_wait);
  if (auto reopened = transport_.reopen(); !reopened) {
    return std::unexpected(Fault{true, std::format("analyzer did not come back after restart: {}", reopened.error())});
  }
  return drain();
}

OwnBootUpdate::Io<std::vector<uint8_t>> OwnBootUpdate::expectReply(std::span<const uint8_t> packet,
                                                                   std::string_view what,
                                                                   std::span<const uint8_t> prefix,
                                                                   std::chrono::milliseconds timeout) {
  auto reply = exchange(packet, what, timeout);
  if (!reply) return std::unexpected(reply.error());
  if (!*reply) return std::unexpected(Fault{false, std::format("{}: analyzer did not answer", what)});
  const std::vector<uint8_t>& bytes = **reply;
  if (bytes.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), bytes.begin())) {
    return std::unexpected(Fault{false, std::format("{}: unexpected answer {}", what, hexOf(bytes))});
  }
  return bytes;
}

OwnBootUpdate::Io<std::optional<std::vector<uint8_t>>> OwnBootUpdate::exchange(std::span<const uint8_t> packet,
                                                                               std::string_view what,
                                                                               std::chrono::milliseconds timeout) {
  if (auto sent = write(packet, what); !sent) return std::unexpected(sent.error());
  sleep_(timing_.pace);
  auto reply = read(timeout);
  if (!reply) return std::unexpected(reply.error());
  sleep_(timing_.pace);
  return reply;
}

OwnBootUpdate::Io<void> OwnBootUpdate::write(std::span<const uint8_t> packet, std::string_view what,
                                             bool gone_after_whole_packet_is_sent) {
  for (int tryNumber = 1; tryNumber <= limits_.writes_per_packet; ++tryNumber) {
    const TransferResult r = transport_.writeCommand(packet, milliseconds(timing_.write_timeout));
    if (r.status == TransferStatus::kOk && r.transferred == packet.size()) return {};
    if (r.status == TransferStatus::kGone && gone_after_whole_packet_is_sent && r.transferred == packet.size()) {
      return {};
    }
    if (r.status != TransferStatus::kTimeout) return std::unexpected(Fault{true, r.detail});
    if (r.transferred != 0) {
      return std::unexpected(Fault{true, std::format("{}: write ended with {} of {} bytes taken", what,
                                                     withThousands(r.transferred), withThousands(packet.size()))});
    }
    log(std::format("{}: write not taken ({}), sending it again", what, r.detail));
    if (tryNumber >= 2) transport_.clearHalts();
  }
  return std::unexpected(
      Fault{false, std::format("{}: analyzer took none of {} writes", what, limits_.writes_per_packet)});
}

OwnBootUpdate::Io<std::optional<std::vector<uint8_t>>> OwnBootUpdate::read(std::chrono::milliseconds timeout) {
  std::vector<uint8_t> reply(kPacketSize);
  const TransferResult r = transport_.readReply(reply, milliseconds(timeout));
  switch (r.status) {
    case TransferStatus::kOk:
      reply.resize(r.transferred);
      return std::optional<std::vector<uint8_t>>{std::move(reply)};
    case TransferStatus::kTimeout:
      return std::optional<std::vector<uint8_t>>{};
    case TransferStatus::kStall:
    case TransferStatus::kGone:
    case TransferStatus::kError:
      break;
  }
  return std::unexpected(Fault{true, r.detail});
}

OwnBootUpdate::Io<void> OwnBootUpdate::drain() {
  for (int replies = 0;; ++replies) {
    auto reply = read(timing_.drain_read_timeout);
    if (!reply) return std::unexpected(reply.error());
    if (!*reply) return {};
    if (replies >= limits_.drain_replies) {
      return std::unexpected(Fault{false, "analyzer keeps sending replies nobody asked for"});
    }
    log(std::format("late reply drained: {}", hexOf(**reply)));
  }
}

bool OwnBootUpdate::cancelRequested() const { return hooks_.cancel_requested && hooks_.cancel_requested(); }

void OwnBootUpdate::progress(FlashPhase phase, uint64_t bytes_sent, int attempt) {
  if (hooks_.progress) hooks_.progress(phase, bytes_sent, attempt);
}

void OwnBootUpdate::log(std::string_view line) {
  if (hooks_.log) hooks_.log(line);
}

}

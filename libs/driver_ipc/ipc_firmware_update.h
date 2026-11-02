/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ipc_decode_error.h"
#include "ipc_messages.h"

namespace bitoscope::ipc::firmware {

inline constexpr std::string_view kFacetName = "firmware_update";

enum class Running : uint16_t { program = 0, bootloader = 1 };

struct FirmwareUpdateInfo {
  Running running = Running::program;
  std::string model;
  std::vector<std::string> modelChoices;
  std::string fileDescription;
  std::vector<std::string> imageNames;
  uint32_t estimatedSeconds = 0;
  bool operator==(const FirmwareUpdateInfo&) const = default;
};

struct BeginUpload {
  uint64_t totalBytes = 0;
  std::string fileName;
  bool operator==(const BeginUpload&) const = default;
};

struct UploadPiece {
  uint32_t uploadId = 0;
  uint64_t offset = 0;
  Bytes bytes;
  bool operator==(const UploadPiece&) const = default;
};

struct Check {
  uint32_t uploadId = 0;
  std::string model;
  bool confirmUnknownFile = false;
  bool confirmOtherModel = false;
  bool operator==(const Check&) const = default;
};

struct Start {
  uint32_t uploadId = 0;
  std::string model;
  bool confirmUnknownFile = false;
  bool confirmOtherModel = false;
  bool operator==(const Start&) const = default;
};

struct Cancel {
  bool confirmCannotCapture = false;
  bool operator==(const Cancel&) const = default;
};

struct StartProgram {
  bool operator==(const StartProgram&) const = default;
};

struct Status {
  bool operator==(const Status&) const = default;
};

using FirmwareUpdateCommand = std::variant<BeginUpload, UploadPiece, Check, Start, Cancel, StartProgram, Status>;

struct UploadReply {
  uint32_t uploadId = 0;
  bool operator==(const UploadReply&) const = default;
};

enum class Verdict : uint16_t { fits = 0, unreadable = 1, modelUnknown = 2, unknownFile = 3, otherModel = 4 };

struct CheckReply {
  Verdict verdict = Verdict::fits;
  std::string fileModel;
  Bytes sha256;
  std::string message;
  bool operator==(const CheckReply&) const = default;
};

enum class CancelAnswer : uint16_t { accepted = 0, needsConfirmation = 1, tooLate = 2 };

struct CancelReply {
  CancelAnswer answer = CancelAnswer::accepted;
  bool operator==(const CancelReply&) const = default;
};

enum class Phase : uint16_t { preparing = 0, sending = 1, finishing = 2 };

struct Progress {
  Phase phase = Phase::preparing;
  std::string imageName;
  uint64_t bytesSent = 0;
  uint64_t bytesTotal = 0;
  uint32_t attempt = 0;
  uint32_t attempts = 0;
  uint32_t resends = 0;
  bool operator==(const Progress&) const = default;
};

enum class Outcome : uint16_t { done = 0, cancelled = 1, failed = 2, needsReplug = 3 };

struct Finished {
  Outcome outcome = Outcome::done;
  std::string message;
  bool canCapture = true;
  uint32_t attempts = 0;
  bool operator==(const Finished&) const = default;
};

using FirmwareUpdateEvent = std::variant<Progress, Finished>;

struct StatusReply {
  bool running = false;
  std::optional<FirmwareUpdateEvent> last;
  bool operator==(const StatusReply&) const = default;
};

std::expected<Bytes, std::string> encodeInfo(const FirmwareUpdateInfo& info);
std::expected<Bytes, std::string> encodeCommand(const FirmwareUpdateCommand& command);
std::expected<Bytes, std::string> encodeUploadReply(const UploadReply& reply);
std::expected<Bytes, std::string> encodeCheckReply(const CheckReply& reply);
std::expected<Bytes, std::string> encodeCancelReply(const CancelReply& reply);
std::expected<Bytes, std::string> encodeEvent(const FirmwareUpdateEvent& event);
std::expected<Bytes, std::string> encodeStatusReply(const StatusReply& reply);

std::expected<FirmwareUpdateInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes);
std::expected<FirmwareUpdateCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes);
std::expected<UploadReply, DecodeError> decodeUploadReply(std::span<const uint8_t> bytes);
std::expected<CheckReply, DecodeError> decodeCheckReply(std::span<const uint8_t> bytes);
std::expected<CancelReply, DecodeError> decodeCancelReply(std::span<const uint8_t> bytes);
std::expected<FirmwareUpdateEvent, DecodeError> decodeEvent(std::span<const uint8_t> bytes);
std::expected<StatusReply, DecodeError> decodeStatusReply(std::span<const uint8_t> bytes);

}

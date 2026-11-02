/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_firmware_update.h"

#include <optional>
#include <string>
#include <utility>

#include <capnp/message.h>

#include "firmware_update.capnp.h"
#include "ipc_capnp_boundary.h"
#include "ipc_capnp_values.h"

namespace bitoscope::ipc::firmware {

namespace {

namespace w = bitoscope::ipc::wire::firmware;
using detail::asData;
using detail::asText;

DecodeError missing(std::string message) { return DecodeError{DecodeErrorKind::missingField, std::move(message), {}, 0}; }

DecodeError empty(std::string_view what) {
  return DecodeError{DecodeErrorKind::noMessage, std::string(what) + " holds nothing (its union is `none`)", {}, 0};
}

DecodeError unknownMember(std::string_view what, uint16_t member) {
  return DecodeError{DecodeErrorKind::unknownMember,
                     "unknown " + std::string(what) + " member " + std::to_string(member) +
                         " (from newer peer that this build does not know)",
                     {},
                     member};
}

template <typename Enum, typename Wire>
std::optional<DecodeError> readEnum(Wire wire, Enum last, std::string_view what, Enum& out) {
  const auto value = static_cast<uint16_t>(wire);
  if (value > static_cast<uint16_t>(last)) {
    return DecodeError{DecodeErrorKind::unknownEnum, "unknown " + std::string(what) + " " + std::to_string(value), {}, 0};
  }
  out = static_cast<Enum>(value);
  return std::nullopt;
}

template <typename List>
void setTexts(List list, const std::vector<std::string>& texts) {
  for (unsigned i = 0; i < list.size(); ++i) list.set(i, asText(texts[i]));
}

void build(w::FirmwareUpdateProgress::Builder out, const Progress& in) {
  out.setPhase(static_cast<w::FirmwareUpdateProgress::Phase>(in.phase));
  if (!in.imageName.empty()) out.setImageName(asText(in.imageName));
  out.setBytesSent(in.bytesSent);
  out.setBytesTotal(in.bytesTotal);
  out.setAttempt(in.attempt);
  out.setAttempts(in.attempts);
  out.setResends(in.resends);
}

void build(w::FirmwareUpdateFinished::Builder out, const Finished& in) {
  out.setOutcome(static_cast<w::FirmwareUpdateFinished::Outcome>(in.outcome));
  if (!in.message.empty()) out.setMessage(asText(in.message));
  out.setCanCapture(in.canCapture);
  out.setAttempts(in.attempts);
}

void build(w::FirmwareUpdateEvent::Builder out, const FirmwareUpdateEvent& in) {
  if (const auto* progress = std::get_if<Progress>(&in)) {
    build(out.initProgress(), *progress);
  } else {
    build(out.initFinished(), std::get<Finished>(in));
  }
}

std::optional<DecodeError> read(w::FirmwareUpdateEvent::Reader in, FirmwareUpdateEvent& out) {
  switch (in.which()) {
    case w::FirmwareUpdateEvent::NONE:
      return empty("firmware update event");
    case w::FirmwareUpdateEvent::PROGRESS: {
      const auto wire = in.getProgress();
      Progress progress;
      if (auto bad = readEnum(wire.getPhase(), Phase::finishing, "firmware update phase", progress.phase)) return bad;
      progress.imageName = detail::toString(wire.getImageName());
      progress.bytesSent = wire.getBytesSent();
      progress.bytesTotal = wire.getBytesTotal();
      progress.attempt = wire.getAttempt();
      progress.attempts = wire.getAttempts();
      progress.resends = wire.getResends();
      out = std::move(progress);
      return std::nullopt;
    }
    case w::FirmwareUpdateEvent::FINISHED: {
      const auto wire = in.getFinished();
      Finished finished;
      if (auto bad = readEnum(wire.getOutcome(), Outcome::needsReplug, "firmware update outcome", finished.outcome)) {
        return bad;
      }
      finished.message = detail::toString(wire.getMessage());
      finished.canCapture = wire.getCanCapture();
      finished.attempts = wire.getAttempts();
      out = std::move(finished);
      return std::nullopt;
    }
  }
  return unknownMember("firmware update event", static_cast<uint16_t>(in.which()));
}

}

std::expected<Bytes, std::string> encodeInfo(const FirmwareUpdateInfo& info) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::FirmwareUpdateInfo>();
    root.setRunning(static_cast<w::FirmwareUpdateInfo::Running>(info.running));
    if (!info.model.empty()) root.setModel(asText(info.model));
    if (!info.modelChoices.empty()) {
      setTexts(root.initModelChoices(static_cast<unsigned>(info.modelChoices.size())), info.modelChoices);
    }
    if (!info.fileDescription.empty()) root.setFileDescription(asText(info.fileDescription));
    if (!info.imageNames.empty()) {
      setTexts(root.initImageNames(static_cast<unsigned>(info.imageNames.size())), info.imageNames);
    }
    root.setEstimatedSeconds(info.estimatedSeconds);
  });
}

std::expected<Bytes, std::string> encodeCommand(const FirmwareUpdateCommand& command) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::FirmwareUpdateCommand>();
    if (const auto* begin = std::get_if<BeginUpload>(&command)) {
      auto out = root.initBeginUpload();
      out.setTotalBytes(begin->totalBytes);
      if (!begin->fileName.empty()) out.setFileName(asText(begin->fileName));
    } else if (const auto* piece = std::get_if<UploadPiece>(&command)) {
      auto out = root.initUploadPiece();
      out.setUploadId(piece->uploadId);
      out.setOffset(piece->offset);
      if (!piece->bytes.empty()) out.setBytes(asData(piece->bytes));
    } else if (const auto* check = std::get_if<Check>(&command)) {
      auto out = root.initCheck();
      out.setUploadId(check->uploadId);
      if (!check->model.empty()) out.setModel(asText(check->model));
      out.setConfirmUnknownFile(check->confirmUnknownFile);
      out.setConfirmOtherModel(check->confirmOtherModel);
    } else if (const auto* start = std::get_if<Start>(&command)) {
      auto out = root.initStart();
      out.setUploadId(start->uploadId);
      if (!start->model.empty()) out.setModel(asText(start->model));
      out.setConfirmUnknownFile(start->confirmUnknownFile);
      out.setConfirmOtherModel(start->confirmOtherModel);
    } else if (const auto* cancel = std::get_if<Cancel>(&command)) {
      root.initCancel().setConfirmCannotCapture(cancel->confirmCannotCapture);
    } else if (std::holds_alternative<StartProgram>(command)) {
      root.setStartProgram();
    } else {
      root.setStatus();
    }
  });
}

std::expected<Bytes, std::string> encodeUploadReply(const UploadReply& reply) {
  return detail::buildMessage(
      [&](capnp::MessageBuilder& message) { message.initRoot<w::UploadReply>().setUploadId(reply.uploadId); });
}

std::expected<Bytes, std::string> encodeCheckReply(const CheckReply& reply) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::CheckReply>();
    root.setVerdict(static_cast<w::CheckReply::Verdict>(reply.verdict));
    if (!reply.fileModel.empty()) root.setFileModel(asText(reply.fileModel));
    if (!reply.sha256.empty()) root.setSha256(asData(reply.sha256));
    if (!reply.message.empty()) root.setMessage(asText(reply.message));
  });
}

std::expected<Bytes, std::string> encodeCancelReply(const CancelReply& reply) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    message.initRoot<w::CancelReply>().setAnswer(static_cast<w::CancelReply::Answer>(reply.answer));
  });
}

std::expected<Bytes, std::string> encodeEvent(const FirmwareUpdateEvent& event) {
  return detail::buildMessage(
      [&](capnp::MessageBuilder& message) { build(message.initRoot<w::FirmwareUpdateEvent>(), event); });
}

std::expected<Bytes, std::string> encodeStatusReply(const StatusReply& reply) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::StatusReply>();
    root.setRunning(reply.running);
    if (reply.last) build(root.initLast(), *reply.last);
  });
}

std::expected<FirmwareUpdateInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes) {
  FirmwareUpdateInfo out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::FirmwareUpdateInfo>();
    if (auto bad = readEnum(root.getRunning(), Running::bootloader, "running program", out.running)) return bad;
    out.model = detail::toString(root.getModel());
    for (capnp::Text::Reader model : root.getModelChoices()) out.modelChoices.push_back(detail::toString(model));
    out.fileDescription = detail::toString(root.getFileDescription());
    for (capnp::Text::Reader name : root.getImageNames()) out.imageNames.push_back(detail::toString(name));
    out.estimatedSeconds = root.getEstimatedSeconds();
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<FirmwareUpdateCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes) {
  FirmwareUpdateCommand out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::FirmwareUpdateCommand>();
    switch (root.which()) {
      case w::FirmwareUpdateCommand::NONE:
        return empty("firmware update command");
      case w::FirmwareUpdateCommand::BEGIN_UPLOAD: {
        const auto in = root.getBeginUpload();
        if (in.getTotalBytes() == 0) return missing("upload names no size");
        out = BeginUpload{in.getTotalBytes(), detail::toString(in.getFileName())};
        return std::nullopt;
      }
      case w::FirmwareUpdateCommand::UPLOAD_PIECE: {
        const auto in = root.getUploadPiece();
        out = UploadPiece{in.getUploadId(), in.getOffset(), detail::toBytes(in.getBytes())};
        return std::nullopt;
      }
      case w::FirmwareUpdateCommand::CHECK: {
        const auto in = root.getCheck();
        out = Check{in.getUploadId(), detail::toString(in.getModel()), in.getConfirmUnknownFile(),
                    in.getConfirmOtherModel()};
        return std::nullopt;
      }
      case w::FirmwareUpdateCommand::START: {
        const auto in = root.getStart();
        out = Start{in.getUploadId(), detail::toString(in.getModel()), in.getConfirmUnknownFile(),
                    in.getConfirmOtherModel()};
        return std::nullopt;
      }
      case w::FirmwareUpdateCommand::CANCEL:
        out = Cancel{root.getCancel().getConfirmCannotCapture()};
        return std::nullopt;
      case w::FirmwareUpdateCommand::START_PROGRAM:
        out = StartProgram{};
        return std::nullopt;
      case w::FirmwareUpdateCommand::STATUS:
        out = Status{};
        return std::nullopt;
    }
    return unknownMember("firmware update command", static_cast<uint16_t>(root.which()));
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<UploadReply, DecodeError> decodeUploadReply(std::span<const uint8_t> bytes) {
  UploadReply out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    out.uploadId = reader.getRoot<w::UploadReply>().getUploadId();
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<CheckReply, DecodeError> decodeCheckReply(std::span<const uint8_t> bytes) {
  CheckReply out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::CheckReply>();
    if (auto bad = readEnum(root.getVerdict(), Verdict::otherModel, "firmware check verdict", out.verdict)) return bad;
    out.fileModel = detail::toString(root.getFileModel());
    out.sha256 = detail::toBytes(root.getSha256());
    out.message = detail::toString(root.getMessage());
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<CancelReply, DecodeError> decodeCancelReply(std::span<const uint8_t> bytes) {
  CancelReply out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    return readEnum(reader.getRoot<w::CancelReply>().getAnswer(), CancelAnswer::tooLate, "cancel answer", out.answer);
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<FirmwareUpdateEvent, DecodeError> decodeEvent(std::span<const uint8_t> bytes) {
  FirmwareUpdateEvent out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    return read(reader.getRoot<w::FirmwareUpdateEvent>(), out);
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<StatusReply, DecodeError> decodeStatusReply(std::span<const uint8_t> bytes) {
  StatusReply out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::StatusReply>();
    out.running = root.getRunning();
    if (root.hasLast() && root.getLast().which() != w::FirmwareUpdateEvent::NONE) {
      FirmwareUpdateEvent last;
      if (auto bad = read(root.getLast(), last)) return bad;
      out.last = std::move(last);
    }
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

}

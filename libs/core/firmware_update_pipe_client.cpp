/*
 * SPDX-License-Identifier: MIT
 */

#include "firmware_update_pipe_client.h"

#include <algorithm>
#include <format>

namespace bitoscope::host::pipe {

namespace {

namespace fw = bitoscope::ipc::firmware;

std::span<const uint8_t> bytesOf(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

template <typename Decode>
auto decoded(Decode decode, const std::string& payload, std::string_view what)
    -> std::expected<typename decltype(decode(bytesOf(payload)))::value_type, std::string> {
  auto reply = decode(bytesOf(payload));
  if (!reply) return std::unexpected(std::format("reply to {} cannot be read: {}", what, reply.error().message));
  return std::move(*reply);
}

}

FirmwareUpdateClient::FirmwareUpdateClient(FacetClient client, ipc::firmware::FirmwareUpdateInfo info)
    : client_(std::move(client)), info_(std::move(info)) {}

std::expected<ipc::firmware::FirmwareUpdateInfo, std::string> FirmwareUpdateClient::parseInfo(
    std::span<const uint8_t> info) {
  auto parsed = fw::decodeInfo(info);
  if (!parsed) return std::unexpected("firmware update facet's info cannot be read: " + parsed.error().message);
  return std::move(*parsed);
}

std::expected<std::string, std::string> FirmwareUpdateClient::send(const fw::FirmwareUpdateCommand& command) {
  const auto bytes = fw::encodeCommand(command);
  if (!bytes) return std::unexpected("firmware update command cannot be encoded: " + bytes.error());
  return client_.call(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

std::expected<uint32_t, std::string> FirmwareUpdateClient::upload(std::span<const uint8_t> file,
                                                                   std::string_view file_name, size_t piece_bytes) {
  if (file.empty()) return std::unexpected("firmware file is empty");
  if (piece_bytes == 0) piece_bytes = kFirmwareUploadPieceBytes;
  auto begun = send(fw::BeginUpload{file.size(), std::string(file_name)});
  if (!begun) return std::unexpected("upload could not start: " + begun.error());
  auto id = decoded(fw::decodeUploadReply, *begun, "upload start");
  if (!id) return std::unexpected(id.error());
  const size_t pieces = (file.size() + piece_bytes - 1) / piece_bytes;
  for (size_t index = 0; index < pieces; ++index) {
    const size_t offset = index * piece_bytes;
    const auto part = file.subspan(offset, (std::min)(piece_bytes, file.size() - offset));
    auto sent = send(fw::UploadPiece{id->uploadId, offset, ipc::Bytes(part.begin(), part.end())});
    if (!sent) return std::unexpected(std::format("upload piece {} of {} was refused: {}", index + 1, pieces, sent.error()));
  }
  return id->uploadId;
}

std::expected<ipc::firmware::CheckReply, std::string> FirmwareUpdateClient::check(uint32_t upload_id,
                                                                                  std::string_view model,
                                                                                  FirmwareConfirmations confirmed) {
  auto answer = send(fw::Check{upload_id, std::string(model), confirmed.unknown_file, confirmed.other_model});
  if (!answer) return std::unexpected("firmware check failed: " + answer.error());
  return decoded(fw::decodeCheckReply, *answer, "check");
}

std::expected<void, std::string> FirmwareUpdateClient::start(uint32_t upload_id, std::string_view model,
                                                              FirmwareConfirmations confirmed) {
  auto answer = send(fw::Start{upload_id, std::string(model), confirmed.unknown_file, confirmed.other_model});
  if (!answer) return std::unexpected(answer.error());
  return {};
}

std::expected<ipc::firmware::CancelAnswer, std::string> FirmwareUpdateClient::cancel(bool confirm_cannot_capture) {
  auto answer = send(fw::Cancel{confirm_cannot_capture});
  if (!answer) return std::unexpected("cancel failed: " + answer.error());
  auto reply = decoded(fw::decodeCancelReply, *answer, "cancel");
  if (!reply) return std::unexpected(reply.error());
  return reply->answer;
}

std::expected<void, std::string> FirmwareUpdateClient::startProgram() {
  auto answer = send(fw::StartProgram{});
  if (!answer) return std::unexpected(answer.error());
  return {};
}

std::expected<ipc::firmware::StatusReply, std::string> FirmwareUpdateClient::status() {
  auto answer = send(fw::Status{});
  if (!answer) return std::unexpected("status failed: " + answer.error());
  return decoded(fw::decodeStatusReply, *answer, "status");
}

}

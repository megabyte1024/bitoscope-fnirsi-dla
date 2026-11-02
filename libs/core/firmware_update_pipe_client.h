/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "facet_client.h"
#include "ipc_firmware_update.h"

namespace bitoscope::host::pipe {

inline constexpr std::string_view kFirmwareUpdateFacetName = ipc::firmware::kFacetName;
inline constexpr size_t kFirmwareUploadPieceBytes = 256 * 1024;

struct FirmwareConfirmations {
  bool unknown_file = false;
  bool other_model = false;
};

class FirmwareUpdateClient {
 public:
  FirmwareUpdateClient(FacetClient client, ipc::firmware::FirmwareUpdateInfo info);

  static std::expected<ipc::firmware::FirmwareUpdateInfo, std::string> parseInfo(std::span<const uint8_t> info);

  const ipc::firmware::FirmwareUpdateInfo& info() const { return info_; }

  std::expected<uint32_t, std::string> upload(std::span<const uint8_t> file, std::string_view file_name,
                                              size_t piece_bytes = kFirmwareUploadPieceBytes);
  std::expected<ipc::firmware::CheckReply, std::string> check(uint32_t upload_id, std::string_view model,
                                                              FirmwareConfirmations confirmed);
  std::expected<void, std::string> start(uint32_t upload_id, std::string_view model, FirmwareConfirmations confirmed);
  std::expected<ipc::firmware::CancelAnswer, std::string> cancel(bool confirm_cannot_capture);
  std::expected<void, std::string> startProgram();
  std::expected<ipc::firmware::StatusReply, std::string> status();

 private:
  std::expected<std::string, std::string> send(const ipc::firmware::FirmwareUpdateCommand& command);

  FacetClient client_;
  ipc::firmware::FirmwareUpdateInfo info_;
};

}

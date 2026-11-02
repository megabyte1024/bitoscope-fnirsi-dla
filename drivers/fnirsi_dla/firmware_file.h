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

#include "sha256.h"

namespace fnirsi {

enum class FirmwareModel { k16Channels, k32Channels };

struct FirmwareImage {
  std::string container_name;
  std::string wire_name;
  uint32_t offset = 0;
  uint32_t size = 0;
};

struct FirmwareFile {
  FirmwareImage mcu;
  FirmwareImage fpga;
  Sha256Digest sha256{};
  std::optional<FirmwareModel> known_model;
};

std::expected<FirmwareFile, std::string> parseFirmwareFile(std::span<const uint8_t> file);

std::span<const uint8_t> imageBytes(std::span<const uint8_t> file, const FirmwareImage& image);

std::expected<std::string, std::string> wireNameOf(std::string_view prefix, std::string_view container_name);

std::optional<FirmwareModel> knownFirmwareModel(const Sha256Digest& digest);

std::optional<FirmwareModel> firmwareModelHintOf(std::string_view file_name);

std::optional<FirmwareModel> firmwareModelOfDevice(std::string_view device_name);

struct FlashConfirmations {
  bool unknown_file = false;
  bool other_model = false;
};

enum class FitRefusalKind { kAnalyzerModelUnknown, kUnknownFile, kOtherModel };

struct FitRefusal {
  FitRefusalKind kind = FitRefusalKind::kUnknownFile;
  std::string message;
};

std::expected<void, FitRefusal> checkFirmwareFitsAnalyzer(const FirmwareFile& file, std::string_view file_name,
                                                         std::optional<FirmwareModel> analyzer,
                                                         FlashConfirmations confirmed);

}

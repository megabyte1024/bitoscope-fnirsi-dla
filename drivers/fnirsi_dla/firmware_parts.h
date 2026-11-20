/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "firmware_file.h"

namespace fnirsi {

inline constexpr uint32_t kFpgaCrc16Channels = 0x3A340F47;
inline constexpr uint32_t kFpgaCrc32Channels = 0x628C4E07;
inline constexpr size_t kMinMcuImageBytes = 1024;
inline constexpr size_t kMaxMcuImageBytes = 0x64000;
inline constexpr size_t kMinFpgaImageBytes = 8;
inline constexpr size_t kMaxFpgaImageBytes = 0x100000;
inline constexpr std::string_view kRawMcuWireName = "MCU_V1.1.bin";
inline constexpr std::string_view kRawFpgaWireName = "FPGA_V2.0.bin";

uint32_t crc32Of(std::span<const uint8_t> data, uint32_t crc_in = 0);

std::optional<FirmwareModel> knownFpgaModel(uint32_t crc);

bool holdsOwnFirmwareMark(std::span<const uint8_t> mcu_image);

inline constexpr std::string_view kBootloaderInstallerMark = "DLA16-BOOTLOADER-INSTALLER";

bool holdsBootloaderInstallerMark(std::span<const uint8_t> mcu_image);

enum class FileLayout { kContainer, kMcuImage, kFpgaImage };

struct FirmwareParts {
  FileLayout layout = FileLayout::kContainer;
  std::optional<FirmwareImage> mcu;
  std::optional<FirmwareImage> fpga;
  uint32_t fpga_crc = 0;
  Sha256Digest sha256{};
};

std::expected<FirmwareParts, std::string> splitFirmwareFile(std::span<const uint8_t> file);

enum class PartsRefusalKind { kAnalyzerModelUnknown, kUnusable, kUnknownFile, kOtherModel };

struct PartsRefusal {
  PartsRefusalKind kind = PartsRefusalKind::kUnusable;
  std::string message;
};

struct PartsVerdict {
  std::optional<FirmwareModel> file_model;
  std::expected<void, PartsRefusal> fit;
};

PartsVerdict judgeFirmwareParts(const FirmwareParts& parts, std::span<const uint8_t> file, std::string_view file_name,
                                std::optional<FirmwareModel> analyzer, FlashConfirmations confirmed);

}

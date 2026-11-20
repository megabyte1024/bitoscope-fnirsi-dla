/*
 * SPDX-License-Identifier: MIT
 */

#include "firmware_parts.h"

#include <algorithm>
#include <array>
#include <format>

#include "number_text.h"

namespace fnirsi {

namespace {

constexpr std::string_view kOwnFirmwareMark = "megabyte";
constexpr uint32_t kCrcPolynomial = 0xEDB88320u;

constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (kCrcPolynomial & (0u - (value & 1u)));
    table[i] = value;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = makeCrcTable();

struct KnownFpga {
  uint32_t crc;
  FirmwareModel model;
};

constexpr std::array kKnownFpga{
    KnownFpga{kFpgaCrc16Channels, FirmwareModel::k16Channels},
    KnownFpga{kFpgaCrc32Channels, FirmwareModel::k32Channels},
};

std::string_view channelsOf(FirmwareModel model) {
  return model == FirmwareModel::k16Channels ? "16-channel" : "32-channel";
}

FirmwareImage wholeFileAs(std::string_view wire_name, size_t size) {
  return FirmwareImage{{}, std::string(wire_name), 0, static_cast<uint32_t>(size)};
}

PartsVerdict refusal(std::optional<FirmwareModel> file_model, PartsRefusalKind kind, std::string message) {
  return PartsVerdict{file_model, std::unexpected(PartsRefusal{kind, std::move(message)})};
}

bool holdsText(std::span<const uint8_t> image, std::string_view text) {
  return !std::ranges::search(image, text, {}, {}, [](char c) { return static_cast<uint8_t>(c); }).empty();
}

std::optional<FirmwareModel> identifiedModel(const FirmwareParts& parts) {
  if (parts.fpga) {
    if (const auto model = knownFpgaModel(parts.fpga_crc)) return model;
  }
  if (parts.layout == FileLayout::kContainer) return knownFirmwareModel(parts.sha256);
  return std::nullopt;
}

}

uint32_t crc32Of(std::span<const uint8_t> data, uint32_t crc_in) {
  uint32_t value = ~crc_in;
  for (const uint8_t byte : data) value = kCrcTable[(value ^ byte) & 0xFFu] ^ (value >> 8);
  return ~value;
}

std::optional<FirmwareModel> knownFpgaModel(uint32_t crc) {
  for (const KnownFpga& known : kKnownFpga) {
    if (known.crc == crc) return known.model;
  }
  return std::nullopt;
}

bool holdsOwnFirmwareMark(std::span<const uint8_t> mcu_image) { return holdsText(mcu_image, kOwnFirmwareMark); }

bool holdsBootloaderInstallerMark(std::span<const uint8_t> mcu_image) {
  return holdsText(mcu_image, kBootloaderInstallerMark);
}

std::expected<FirmwareParts, std::string> splitFirmwareFile(std::span<const uint8_t> file) {
  FirmwareParts parts;
  parts.sha256 = sha256(file);
  auto container = parseFirmwareFile(file);
  if (container) {
    parts.layout = FileLayout::kContainer;
    parts.mcu = container->mcu;
    parts.fpga = container->fpga;
    parts.fpga_crc = crc32Of(imageBytes(file, container->fpga));
    return parts;
  }
  parts.fpga_crc = crc32Of(file);
  if (knownFpgaModel(parts.fpga_crc)) {
    parts.layout = FileLayout::kFpgaImage;
    parts.fpga = wholeFileAs(kRawFpgaWireName, file.size());
    return parts;
  }
  if (file.size() >= kMinMcuImageBytes && file.size() <= kMaxMcuImageBytes) {
    parts.layout = FileLayout::kMcuImage;
    parts.mcu = wholeFileAs(kRawMcuWireName, file.size());
    return parts;
  }
  if (file.size() > kMaxMcuImageBytes && file.size() <= kMaxFpgaImageBytes) {
    parts.layout = FileLayout::kFpgaImage;
    parts.fpga = wholeFileAs(kRawFpgaWireName, file.size());
    return parts;
  }
  return std::unexpected(std::format(
      "{}; as single image, file of {} bytes is neither MCU image ({} to {} bytes) nor FPGA image (at most {} bytes)",
      container.error(), withThousands(file.size()), withThousands(kMinMcuImageBytes),
      withThousands(kMaxMcuImageBytes), withThousands(kMaxFpgaImageBytes)));
}

PartsVerdict judgeFirmwareParts(const FirmwareParts& parts, std::span<const uint8_t> file, std::string_view file_name,
                                std::optional<FirmwareModel> analyzer, FlashConfirmations confirmed) {
  const std::optional<FirmwareModel> identified = identifiedModel(parts);
  const std::optional<FirmwareModel> hinted = identified ? identified : firmwareModelHintOf(file_name);
  if (!analyzer) {
    return refusal(hinted, PartsRefusalKind::kAnalyzerModelUnknown,
                   "Analyzer is in update mode and cannot report its model. Choose its number of channels: 16 or 32.");
  }
  if (identified && *identified != *analyzer) {
    const std::string what = parts.fpga && knownFpgaModel(parts.fpga_crc)
                                 ? std::format("This file holds FPGA image for {} analyzers (CRC-32 {:08x})",
                                               channelsOf(*identified), parts.fpga_crc)
                                 : std::format("This firmware file is for {} analyzers", channelsOf(*identified));
    return refusal(identified, PartsRefusalKind::kUnusable,
                   std::format("{}; your analyzer is {} model. bitoscope does not install it: it can damage your "
                               "analyzer.",
                               what, channelsOf(*analyzer)));
  }
  const bool fpgaKnown = !parts.fpga || knownFpgaModel(parts.fpga_crc).has_value();
  const bool mcuKnown = !parts.mcu || (parts.layout == FileLayout::kContainer && knownFirmwareModel(parts.sha256)) ||
                        holdsOwnFirmwareMark(imageBytes(file, *parts.mcu));
  if (fpgaKnown && mcuKnown) return PartsVerdict{identified, {}};

  const std::string hint =
      hinted ? std::format(" (its name suggests {} analyzers)", channelsOf(*hinted)) : std::string();
  if (!confirmed.unknown_file) {
    const std::string what =
        !fpgaKnown ? std::format("FPGA image in this file is not known to bitoscope (CRC-32 {:08x}){}",
                                 parts.fpga_crc, hint)
                   : std::format("MCU image in this file is neither own firmware nor part of FNIRSI firmware file "
                                 "known to bitoscope{}",
                                 hint);
    return refusal(hinted, PartsRefusalKind::kUnknownFile,
                   std::format("{}. Install it only if you are sure it is made for your {} analyzer. File "
                               "fingerprint (SHA-256): {}.",
                               what, channelsOf(*analyzer), sha256Hex(parts.sha256)));
  }
  if (hinted && *hinted != *analyzer && !confirmed.other_model) {
    return refusal(hinted, PartsRefusalKind::kOtherModel,
                   std::format("This firmware file is for {} analyzers; your analyzer is {} model. Installing it can "
                               "damage your analyzer.",
                               channelsOf(*hinted), channelsOf(*analyzer)));
  }
  return PartsVerdict{hinted, {}};
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "firmware_file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <vector>

#include "number_text.h"

namespace fnirsi {

namespace {

constexpr size_t kHeaderSize = 0x58;
constexpr size_t kImageEntrySize = 0x2C;
constexpr size_t kNameSize = 0x20;
constexpr size_t kOffsetField = 0x20;
constexpr size_t kSizeField = 0x24;
constexpr size_t kLastByteField = 0x28;
constexpr std::string_view kBinSuffix = ".bin";

constexpr uint8_t hexDigit(char c) {
  return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}

constexpr Sha256Digest digestFromHex(std::string_view hex) {
  Sha256Digest digest{};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>((hexDigit(hex[2 * i]) << 4) | hexDigit(hex[2 * i + 1]));
  }
  return digest;
}

struct KnownFirmware {
  Sha256Digest sha256;
  FirmwareModel model;
};

constexpr std::array kKnownFirmware{
    KnownFirmware{digestFromHex("ffabe34478ef69a8b7108b7f1862a7c203f4b71744d4812d8397d812ce30aa4a"),
                  FirmwareModel::k16Channels},
    KnownFirmware{digestFromHex("c5a630ef268f9fdd3abba0fdd39b320954a69a78d24e2cc5d1a5dbfbcb2d808e"),
                  FirmwareModel::k32Channels},
};

uint32_t readU32(std::span<const uint8_t> file, size_t at) {
  return uint32_t{file[at]} | (uint32_t{file[at + 1]} << 8) | (uint32_t{file[at + 2]} << 16) |
         (uint32_t{file[at + 3]} << 24);
}

std::string_view channelsOf(FirmwareModel model) {
  return model == FirmwareModel::k16Channels ? "16-channel" : "32-channel";
}

bool readable(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) { return c >= ' ' && c <= '~'; });
}

std::expected<FirmwareImage, std::string> readImage(std::span<const uint8_t> file, int number,
                                                    std::string_view requiredPrefix, std::string_view wirePrefix) {
  const size_t base = static_cast<size_t>(number - 1) * kImageEntrySize;
  const auto nameField = file.subspan(base, kNameSize);
  const auto nameEnd = std::ranges::find(nameField, uint8_t{0});
  const std::string name(nameField.begin(), nameEnd);
  if (!readable(name)) return std::unexpected(std::format("firmware image {} name is not readable text", number));
  if (!name.starts_with(requiredPrefix)) {
    return std::unexpected(std::format("firmware image {} is named '{}', not {}...", number, name, requiredPrefix));
  }
  const uint32_t offset = readU32(file, base + kOffsetField);
  const uint32_t size = readU32(file, base + kSizeField);
  const uint32_t lastByte = readU32(file, base + kLastByteField);
  if (size == 0) return std::unexpected(std::format("firmware image {} ({}) is empty", number, name));
  const uint64_t end = uint64_t{offset} + size;
  if (uint64_t{lastByte} != end - 1) {
    return std::unexpected(std::format("firmware header of image {} ({}): last byte 0x{:X} is not offset + size - 1 (0x{:X})",
                                       number, name, lastByte, end - 1));
  }
  if (end > file.size()) {
    return std::unexpected(std::format("firmware image {} ({}) runs past end of file: it ends at byte {}, file has {} bytes",
                                       number, name, withThousands(end), withThousands(file.size())));
  }
  auto wireName = wireNameOf(wirePrefix, name);
  if (!wireName) return std::unexpected(std::format("firmware image {}: {}", number, wireName.error()));
  return FirmwareImage{name, std::move(*wireName), offset, size};
}

}

std::expected<FirmwareFile, std::string> parseFirmwareFile(std::span<const uint8_t> file) {
  if (file.size() < kHeaderSize) {
    return std::unexpected(
        std::format("firmware file has {} bytes; its header alone has {}", withThousands(file.size()), kHeaderSize));
  }
  auto mcu = readImage(file, 1, "APP_", "MCU");
  if (!mcu) return std::unexpected(std::move(mcu.error()));
  auto fpga = readImage(file, 2, "FPGA_", "FPGA");
  if (!fpga) return std::unexpected(std::move(fpga.error()));
  FirmwareFile parsed{std::move(*mcu), std::move(*fpga), sha256(file), std::nullopt};
  parsed.known_model = knownFirmwareModel(parsed.sha256);
  return parsed;
}

std::span<const uint8_t> imageBytes(std::span<const uint8_t> file, const FirmwareImage& image) {
  if (uint64_t{image.offset} + image.size > file.size()) return {};
  return file.subspan(image.offset, image.size);
}

std::expected<std::string, std::string> wireNameOf(std::string_view prefix, std::string_view container_name) {
  std::string_view stem = container_name;
  if (stem.ends_with(kBinSuffix)) stem.remove_suffix(kBinSuffix.size());
  std::vector<std::string_view> versions;
  size_t start = 0;
  while (start <= stem.size()) {
    const size_t end = std::min(stem.find('_', start), stem.size());
    const std::string_view part = stem.substr(start, end - start);
    if (part.starts_with('V')) versions.push_back(part);
    start = end + 1;
  }
  if (versions.size() != 1) {
    return std::unexpected(std::format("name '{}' has no single version part starting with V", container_name));
  }
  return std::format("{}_{}{}", prefix, versions.front(), kBinSuffix);
}

std::optional<FirmwareModel> knownFirmwareModel(const Sha256Digest& digest) {
  for (const KnownFirmware& known : kKnownFirmware) {
    if (known.sha256 == digest) return known.model;
  }
  return std::nullopt;
}

std::optional<FirmwareModel> firmwareModelHintOf(std::string_view file_name) {
  const size_t slash = file_name.find_last_of("/\\");
  std::string_view stem = slash == std::string_view::npos ? file_name : file_name.substr(slash + 1);
  if (stem.size() >= kBinSuffix.size()) {
    const std::string_view suffix = stem.substr(stem.size() - kBinSuffix.size());
    const bool isBin = std::ranges::equal(suffix, kBinSuffix, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) == b;
    });
    if (isBin) stem.remove_suffix(kBinSuffix.size());
  }
  if (stem.ends_with("16")) return FirmwareModel::k16Channels;
  if (stem.ends_with("32")) return FirmwareModel::k32Channels;
  return std::nullopt;
}

std::optional<FirmwareModel> firmwareModelOfDevice(std::string_view device_name) {
  if (device_name == "DL16") return FirmwareModel::k16Channels;
  if (device_name == "DL32") return FirmwareModel::k32Channels;
  return std::nullopt;
}

std::expected<void, FitRefusal> checkFirmwareFitsAnalyzer(const FirmwareFile& file, std::string_view file_name,
                                                         std::optional<FirmwareModel> analyzer,
                                                         FlashConfirmations confirmed) {
  if (!analyzer) {
    return std::unexpected(FitRefusal{FitRefusalKind::kAnalyzerModelUnknown,
                                      "Analyzer is in update mode and cannot report its model. Choose its "
                                      "number of channels: 16 or 32."});
  }
  const std::optional<FirmwareModel> fileModel = file.known_model ? file.known_model : firmwareModelHintOf(file_name);
  if (!file.known_model && !confirmed.unknown_file) {
    const std::string hint =
        fileModel ? std::format(" (its name suggests {} analyzers)", channelsOf(*fileModel)) : std::string();
    return std::unexpected(FitRefusal{
        FitRefusalKind::kUnknownFile,
        std::format("This file is not one of FNIRSI firmware files known to bitoscope{}. Install it only if you are "
                    "sure it is made for your {} analyzer. File fingerprint (SHA-256): {}.",
                    hint, channelsOf(*analyzer), sha256Hex(file.sha256))});
  }
  if (fileModel && *fileModel != *analyzer && !confirmed.other_model) {
    return std::unexpected(FitRefusal{
        FitRefusalKind::kOtherModel,
        std::format("This firmware file is for {} analyzers; your analyzer is {} model. Installing it can damage "
                    "your analyzer.",
                    channelsOf(*fileModel), channelsOf(*analyzer))});
  }
  return {};
}

}

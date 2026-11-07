/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_analyzer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <string_view>
#include <utility>

#include "channel_padding.h"
#include "data_rate_limit.h"
#include "transfer_fill_time.h"

namespace fnirsi {

namespace {

constexpr std::string_view kThresholdId = "threshold";
constexpr std::string_view kPrerollId = "preroll";
constexpr int64_t kThresholdLimitCentivolts = 500;
constexpr int64_t kDefaultThresholdCentivolts = 160;
constexpr int32_t kMillivoltsPerCentivolt = 10;
constexpr const char* kDeviceName = "DLA";
constexpr uint64_t kWchCleanBitsPerSecond = 1'600'000'000;

}

uint64_t prerollSamples(uint32_t preroll_ms, uint64_t hz) { return (uint64_t{preroll_ms} * hz + 999) / 1000; }

DlaAnalyzer::DlaAnalyzer(std::shared_ptr<SampleStreamDevice> device, DlaUsbDriver usb_driver)
    : device_(std::move(device)), usb_driver_(usb_driver) {}

namespace {

std::vector<bitoscope::plugin::PropertyNode> dlaAnalyzerProperties() {
  namespace plugin = bitoscope::plugin;
  const auto property = [](std::string_view id, std::string name, plugin::PropertyType type, plugin::Unit unit,
                           std::string help) {
    constexpr bool kFixedDuringCapture = false;
    return plugin::PropertyNode{
        plugin::Property{std::string(id), std::move(name), std::move(type), unit, kFixedDuringCapture,
                         std::move(help)}};
  };
  return {
      plugin::PropertyNode{plugin::PropertyGroup{
          "Input",
          {property(kThresholdId, "Threshold",
                    plugin::DecimalType{2, -kThresholdLimitCentivolts, kThresholdLimitCentivolts,
                                        kDefaultThresholdCentivolts},
                    plugin::Unit::volt, "Input voltage above which sample reads as high.")}}},
      plugin::PropertyNode{plugin::PropertyGroup{
          "Diagnostics",
          {property(kPrerollId, "Pre-roll",
                    plugin::DecimalType{3, 0, int64_t{kMaxPrerollMs}, int64_t{kDefaultPrerollMs}}, plugin::Unit::second,
                    "Time dropped at start of streaming capture, so changed threshold settles.")}}},
  };
}

}

bitoscope::plugin::LogicAnalyzerInfo dlaAnalyzerInfo() {
  bitoscope::plugin::LogicAnalyzerInfo info;
  info.numChannels = kDlaChannels;
  for (const auto& entry : kRateTable) info.sampleRatesMilliHz.push_back(entry.hz * 1000);
  info.properties = dlaAnalyzerProperties();
  bitoscope::plugin::addUsbStreamProperties(info.properties);
  for (uint32_t n = 1; n <= kDlaChannels; ++n) info.rateCeilingsMilliHz.push_back(rateCeilingHz(n).value_or(0) * 1000);
  info.rateCeilingReason = kRateCeilingReason;
  // WHY: Wires 1 to 4 of a socket are red, yellow, blue, green; a palette entry's identifier is one past its
  // number.
  static constexpr std::array<const char*, 4> kWireColors{"color-3", "color-5", "color-7", "color-6"};
  for (uint32_t c = 0; c < kDlaChannels; ++c) {
    info.channelPresets.push_back({c, std::format("{}.{}", c / 4 + 1, c % 4 + 1), kWireColors[c % 4]});
  }
  return info;
}

std::expected<void, std::string> DlaAnalyzer::prepare(const bitoscope::plugin::Capture& capture,
                                                      bitoscope::plugin::Sink& sink) {
  if (replug_needed_) return std::unexpected(std::format("last capture failed; {}", kWchReplugAdvice));
  const uint64_t hz = capture.sampleRateMilliHz / 1000;

  const int32_t threshold_millivolts =
      static_cast<int32_t>(capture.integer(kThresholdId)) * kMillivoltsPerCentivolt;
  const uint32_t preroll_ms = static_cast<uint32_t>(capture.integer(kPrerollId));
  const auto transfer_kib = static_cast<uint32_t>(capture.integer(BITOSCOPE_USB_TRANSFER_KIB_PROPERTY));

  // WHY: Buffer mode needs an exact sample count as depth; streaming mode is depth 0 whatever the limit.
  const uint32_t depth = capture.buffered ? static_cast<uint32_t>(std::min<uint64_t>(capture.sampleLimit, 0xFFFFFFFFu)) : 0;
  if (capture.buffered && depth == 0) return std::unexpected("buffered mode requires nonzero sample_limit");

  const auto recorded = recordedChannelMask(capture.channelMask);
  if (!recorded) return std::unexpected(recorded.error());
  const auto user_channels = static_cast<uint32_t>(std::popcount(capture.channelMask));
  const auto device_channels = static_cast<uint32_t>(std::popcount(*recorded));
  if (auto fills = checkTransferFillTime(transfer_kib, hz, device_channels); !fills) {
    return std::unexpected(fills.error());
  }

  auto param_setting = buildParameterSetting(hz, threshold_millivolts, depth);
  if (!param_setting.has_value()) return std::unexpected(param_setting.error());
  parameter_setting_ = *param_setting;
  simple_trigger_ = buildSimpleTrigger(*recorded);
  if (!sink.setDeviceBytes({bitoscope::plugin::Layout::channelMajor, *recorded, 0})) {
    return std::unexpected("SDK refused DLA's device bytes");
  }

  if (const uint64_t bits_per_second = hz * device_channels;
      usb_driver_ == DlaUsbDriver::wch && bits_per_second > kWchCleanBitsPerSecond) {
    const uint64_t tenths = bits_per_second / 100'000'000;
    const std::string warning = std::format(
        "{}.{} Gbit/s on WCH driver is above 1.6 Gbit/s it kept clean in tests: capture fails if delivery falls "
        "behind (nothing broken is saved); WinUSB carries 2 Gbit/s",
        tenths / 10, tenths % 10);
    bitoscope_log(BITOSCOPE_LOG_WARNING, "%s", warning.c_str());
  }

  stream_ = bitoscope::plugin::UsbStream{};
  stream_.started = [this] { return startDevice(); };
  stream_.stop = [this] { return stopDevice(); };
  stream_.drain = true;
  stream_.prerollSamples = capture.buffered ? 0 : prerollSamples(preroll_ms, hz);
  stream_.lagLimitBytes = kDeliveryLagLimitBytes;
  stream_.deviceName = kDeviceName;
  started_line_ = "capture started: " + std::to_string(hz) + " Hz, " + std::to_string(user_channels) + " channel(s), " +
                  (device_channels != user_channels ? "recorded as " + std::to_string(device_channels) + ", " : std::string()) +
                  (capture.buffered ? "buffered, depth=" + std::to_string(depth)
                                    : "streaming, pre-roll " + std::to_string(preroll_ms) + " ms");
  return {};
}

std::expected<void, std::string> DlaAnalyzer::startDevice() {
  commands_answered_ = false;
  stop_answered_ = false;
  if (auto r = device_->sendFramed(FramedCommand::kParameterSetting, parameter_setting_); !r.has_value()) {
    return std::unexpected("ParameterSetting failed: " + r.error());
  }
  if (auto r = device_->sendFramed(FramedCommand::kSimpleTrigger, simple_trigger_); !r.has_value()) {
    return std::unexpected("SimpleTrigger failed: " + r.error());
  }
  commands_answered_ = true;
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s", started_line_.c_str());
  return {};
}

std::expected<void, std::string> DlaAnalyzer::stopDevice() {
  if (auto r = device_->sendFramed(FramedCommand::kStop, buildStopPayload()); !r.has_value()) {
    return std::unexpected("Stop command failed: " + r.error());
  }
  stop_answered_ = true;
  return {};
}

std::expected<void, std::string> DlaAnalyzer::capture(const bitoscope::plugin::Capture&,
                                                      bitoscope::plugin::Sink& sink) {
  device_->streamSamples(sink, stream_);
  if (usb_driver_ == DlaUsbDriver::wch && sink.failed()) {
    replug_needed_ = true;
    bitoscope_log(BITOSCOPE_LOG_ERROR, "capture failed; %.*s", static_cast<int>(kWchReplugAdvice.size()),
                  kWchReplugAdvice.data());
  }
  const auto fill = sink.ringFill();
  if (sink.failed() && commands_answered_ && stop_answered_ && fill && fill->highWaterSlots == 0) {
    bitoscope_log(BITOSCOPE_LOG_ERROR,
                  "analyzer answers but sends no samples: start again; if it still sends none, unplug and replug it; "
                  "if that does not help, its FPGA image may be damaged: update firmware");
  }
  bitoscope_log(BITOSCOPE_LOG_INFO, "capture stopped");
  return {};
}

}

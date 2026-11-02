/*
 * SPDX-License-Identifier: MIT
 */

#include "fnirsi_protocol.h"

#include <format>

namespace fnirsi {

std::string connectDescription(const McuVersion& version) {
  return std::format("MCU fw {}.{}", version.firmware_major, version.firmware_minor);
}

std::optional<McuVersion> parseMcuVersionReply(std::span<const uint8_t> reply) {
  constexpr uint8_t kNextStartApplication = 0x61;
  constexpr uint8_t kNextStartBootloader = 0x62;
  if (reply.size() < 9 || reply[0] != 0x0A || reply[1] != 0x81 || reply[2] != 0x01) {
    return std::nullopt;
  }
  if (reply[3] != kNextStartApplication && reply[3] != kNextStartBootloader) {
    return std::nullopt;
  }
  McuVersion v;
  v.next_start = reply[3] == kNextStartBootloader ? NextStart::kBootloader : NextStart::kApplication;
  v.firmware_major = reply[4];
  v.firmware_minor = reply[5];
  const bool bootloaderReportsNoVersion = v.firmware_major == 0 && v.firmware_minor == 0;
  v.application_running = !bootloaderReportsNoVersion;
  v.hardware_version = reply[6];
  v.product_tier = reply[8];
  return v;
}

std::optional<DeviceData> parseDeviceDataReply(std::span<const uint8_t> reply) {
  if (reply.size() < 17 || reply[0] != 0x0A || reply[1] != 0x02 || reply[2] != 0x0D) {
    return std::nullopt;
  }
  DeviceData d;
  d.usb_version = reply[7];
  d.fpga_firmware_version = reply[9] * 100 + reply[10];
  d.device_name.assign(reinterpret_cast<const char*>(&reply[13]), 4);
  const auto nul = d.device_name.find('\0');
  if (nul != std::string::npos) {
    d.device_name.resize(nul);
  }
  return d;
}

static_assert(gCRC32(std::array<uint8_t, 9>{'1', '2', '3', '4', '5', '6', '7', '8', '9'}) ==
              0xD202D277u);

static_assert(rateIndexForHz(1'000'000).value() == 0x00);
static_assert(rateIndexForHz(1'000'000'000).value() == 0x0E);
static_assert(rateIndexForHz(125'000'000).value() == 0x0A);
static_assert(!rateIndexForHz(600'000'000).has_value());
static_assert(!rateIndexForHz(3).has_value());

static_assert(buildSimpleTrigger(0x0F)[1] == 0x0F);
static_assert(buildSimpleTrigger(0x0F)[2] == 0x00);
static_assert(buildSimpleTrigger(0x0100)[1] == 0x00);
static_assert(buildSimpleTrigger(0x0100)[2] == 0x01);
static_assert(buildSimpleTrigger(0xFFFF)[8] == 0x00);

inline constexpr auto kExampleParamSetting =
    buildParameterSetting(1'000'000, 1600, 1'000'000);
static_assert(kExampleParamSetting.has_value());
static_assert((*kExampleParamSetting)[5] == 0x40 && (*kExampleParamSetting)[6] == 0x42 &&
              (*kExampleParamSetting)[7] == 0x0F && (*kExampleParamSetting)[8] == 0x00);
static_assert((*kExampleParamSetting)[9] == 0x00);
static_assert((*kExampleParamSetting)[10] == 0xA0 && (*kExampleParamSetting)[11] == 0x00);
static_assert((*kExampleParamSetting)[20] == 0x40 && (*kExampleParamSetting)[21] == 0x42 &&
              (*kExampleParamSetting)[22] == 0x0F && (*kExampleParamSetting)[23] == 0x00);

static_assert(buildParameterSetting(1'000'000, -1000, 0).value()[10] == 0x9C);
static_assert(buildParameterSetting(1'000'000, -1000, 0).value()[11] == 0xFF);

static_assert(buildParameterSetting(1'000'000, 1600, 0).value()[13] == 0x00);
static_assert(buildParameterSetting(1'000'000, 1600, 0).value()[20] == 0x00);

static_assert(!buildParameterSetting(600'000'000, 1600, 0).has_value());

static_assert(!buildParameterSetting(1'000'000, 1601, 0).has_value());
static_assert(!buildParameterSetting(1'000'000, 6000, 0).has_value());
static_assert(!centivoltsFromMillivolts(1601).has_value());
static_assert(centivoltsFromMillivolts(1600).value() == 160);

static_assert(buildFramedCommand(FramedCommand::kStop, buildStopPayload()).size() %
                  kFramedCommandAlignment ==
              0);
static_assert(buildFramedCommand(FramedCommand::kParameterSetting,
                                  *buildParameterSetting(1'000'000, 1600, 0))
                      .size() %
                  kFramedCommandAlignment ==
              0);

static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[0] == 0x10);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[1] == 0xE8);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[2] == 0x03);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 150)[5] == 100);
static_assert(buildPwmStop(PwmPort::kP3)[0] == 0x13);
static_assert(buildPwmStopAll()[0] == 0x14);

static_assert(buildDirectCommand(DirectCommand::kFlush).size() == kDirectCommandSize);
static_assert(buildDirectCommand(DirectCommand::kFlush)[0] == 0x0A);
static_assert(buildDirectCommand(DirectCommand::kFlush)[1] == 0x0B);
static_assert(buildDirectCommand(DirectCommand::kSetResetState,
                                  std::array<uint8_t, 1>{0x01})[2] == 0x01);

}

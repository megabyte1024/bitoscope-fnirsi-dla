/*
 * SPDX-License-Identifier: MIT
 */

#include "fnirsi_protocol.h"

namespace fnirsi {

std::optional<McuVersion> parseMcuVersionReply(std::span<const uint8_t> reply) {
  if (reply.size() < 9 || reply[0] != 0x0A || reply[1] != 0x81 || reply[2] != 0x01) {
    return std::nullopt;
  }
  McuVersion v;
  v.application_mode = (reply[3] == 0x61);
  v.firmware_major = reply[4];
  v.firmware_minor = reply[5];
  v.hardware_version = reply[6];
  v.product_tier = reply[8];
  return v;
}

std::optional<DeviceData> parseDeviceDataReply(std::span<const uint8_t> reply) {
  if (reply.size() < 17 || reply[0] != 0x0A || reply[1] != 0x02 || reply[2] != 0x0D) {
    return std::nullopt;
  }
  DeviceData d;
  d.fpga_healthy = (reply[6] == 1);
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

static_assert(buildSimpleTrigger(0x0F)[2] == 0x0F);
static_assert(buildSimpleTrigger(0x0F)[3] == 0x00);
static_assert(buildSimpleTrigger(0x0100)[2] == 0x00);
static_assert(buildSimpleTrigger(0x0100)[3] == 0x01);
static_assert(buildSimpleTrigger(0xFFFF)[9] == 0x00);

inline constexpr auto kExampleParamSetting =
    buildParameterSetting(1'000'000, 1600, 1'000'000);
static_assert(kExampleParamSetting.has_value());
static_assert((*kExampleParamSetting)[6] == 0x40 && (*kExampleParamSetting)[7] == 0x42 &&
              (*kExampleParamSetting)[8] == 0x0F && (*kExampleParamSetting)[9] == 0x00);
static_assert((*kExampleParamSetting)[10] == 0x00);
static_assert((*kExampleParamSetting)[11] == 0xA0 && (*kExampleParamSetting)[12] == 0x00);
static_assert((*kExampleParamSetting)[21] == 0x40 && (*kExampleParamSetting)[22] == 0x42 &&
              (*kExampleParamSetting)[23] == 0x0F && (*kExampleParamSetting)[24] == 0x00);

static_assert(buildParameterSetting(1'000'000, -1000, 0).value()[11] == 0x9C);
static_assert(buildParameterSetting(1'000'000, -1000, 0).value()[12] == 0xFF);

static_assert(buildParameterSetting(1'000'000, 1600, 0).value()[14] == 0x00);
static_assert(buildParameterSetting(1'000'000, 1600, 0).value()[21] == 0x00);

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

static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[1] == 0x10);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[2] == 0xE8);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 50)[3] == 0x03);
static_assert(buildPwmStart(PwmPort::kP0, 1'000, 150)[6] == 100);
static_assert(buildPwmStop(PwmPort::kP3)[1] == 0x13);
static_assert(buildPwmStopAll()[0] == 0x14);

static_assert(buildDirectCommand(DirectCommand::kFlush).size() == kDirectCommandSize);
static_assert(buildDirectCommand(DirectCommand::kFlush)[0] == 0x0A);
static_assert(buildDirectCommand(DirectCommand::kFlush)[1] == 0x0B);
static_assert(buildDirectCommand(DirectCommand::kSetResetState,
                                  std::array<uint8_t, 1>{0x01})[2] == 0x01);

}

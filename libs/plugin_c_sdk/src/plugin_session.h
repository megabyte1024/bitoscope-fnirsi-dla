/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "frame_output.h"
#include "ipc_byte_stream.h"
#include "ipc_messages.h"
#include "ipc_protocol_version.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;

enum class InterfaceKind { usb, network, serial, none };

using ListDevices = std::function<int(bitoscope_devices& devices)>;

struct SessionOptions {
  uint32_t minProtocolMajor = ipc::kProtocolMajor;
  uint32_t maxProtocolMajor = ipc::kProtocolMajor;
  InterfaceKind interfaceKind = InterfaceKind::none;
  std::string pluginVersion;
  std::string runFunction = "bitoscope_run_decoder_plugin";
};

int runSession(std::span<const std::string> args, ipc::ByteSource& in, ipc::ByteSink& out, const ListDevices& list,
               const SessionOptions& options = {});

struct RunMode {
  enum class Kind { list, device, watch, interface };
  Kind kind = Kind::list;
  std::string deviceId;
  std::optional<std::string> networkAddress{};
  std::optional<uint16_t> networkPort{};
  std::optional<std::string> serialPort{};
};

std::optional<RunMode> parseRunMode(std::span<const std::string> args);
void printUsage();
int answerInterface(ipc::ByteSink& out, InterfaceKind kind);

struct ServedDevice {
  const bitoscope_device* device = nullptr;
  std::string refusal;
  ipc::UnavailableReason unavailable = ipc::UnavailableReason::none;
  bool identified = false;
};

int serveDevice(ipc::ByteSource& in, FrameOutput& out, const ServedDevice& served, const SessionOptions& options);

std::vector<ipc::DeviceInfo> describeDevices(const bitoscope_devices& devices);

std::string listFailed(int result, const bitoscope_devices& devices);

}

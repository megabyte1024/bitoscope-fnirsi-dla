/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "ipc_byte_stream.h"

namespace bitoscope::host {

inline constexpr std::chrono::milliseconds kShutdownGrace{2000};

class PluginLink {
 public:
  virtual ~PluginLink() = default;

  virtual const std::string& name() const = 0;

  virtual ipc::ByteSource& fromPlugin() = 0;
  virtual ipc::ByteSink& toPlugin() = 0;

  virtual void shutdown(std::chrono::milliseconds grace) = 0;

  virtual std::optional<int> waitForExit(std::chrono::milliseconds wait) = 0;
};

}

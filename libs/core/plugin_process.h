/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: CREATE_NO_WINDOW: a Ctrl+C in the host's console must reach the host, not kill every plugin mid-capture.
// HANDLE_LIST names the child's handles only, so a concurrent spawn cannot hold another plugin's stdout open.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "plugin_link.h"
#include "work_placement.h"

namespace bitoscope::host {

using StderrLineFn = std::function<void(std::string_view line)>;

class StderrLines {
 public:
  static constexpr std::size_t kMaxLineBytes = 16 * 1024;

  StderrLines(std::string prefix, StderrLineFn sink);
  void feed(std::span<const uint8_t> bytes);
  void finish();

 private:
  void emit();

  std::string prefix_;
  StderrLineFn sink_;
  std::string line_;
};

struct PluginProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::string name;
  StderrLineFn onStderrLine;
  cpu::WorkPlacement placement = {};
};

class PluginProcess final : public PluginLink {
 public:
  static std::expected<std::unique_ptr<PluginProcess>, std::string> start(PluginProcessOptions options);

  ~PluginProcess() override;
  PluginProcess(const PluginProcess&) = delete;
  PluginProcess& operator=(const PluginProcess&) = delete;

  const std::string& name() const override;
  ipc::ByteSource& fromPlugin() override;
  ipc::ByteSink& toPlugin() override;
  void shutdown(std::chrono::milliseconds grace) override;
  std::optional<int> waitForExit(std::chrono::milliseconds wait) override;

  void closeInput();
  void kill();
  uint64_t processId() const;

  struct Native;

 private:
  explicit PluginProcess(std::unique_ptr<Native> native);
  std::unique_ptr<Native> native_;
};

std::vector<std::filesystem::path> listPluginExecutables(const std::filesystem::path& directory);

}

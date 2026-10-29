/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: START opens the ring BEFORE starting the capture thread; STOP joins the thread BEFORE ending the ring.

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "ring_writer.h"
#include "driver_model.h"
#include "ipc_la_acquisition.h"
#include "ipc_messages.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

class LaRuntime final : public FacetHandler {
 public:
  LaRuntime(const LogicAnalyzerRegistration& registration, const std::vector<ipc::props::PropertyValue>& properties);
  ~LaRuntime() override;
  LaRuntime(const LaRuntime&) = delete;
  LaRuntime& operator=(const LaRuntime&) = delete;

  FacetAnswer handle(std::span<const uint8_t> payload) override;

  bool capturing() const override { return run_ != nullptr; }

  std::expected<void, ApplyRefusal> apply(std::span<const ipc::props::PropertyValue> values, bool capturing) override;

  void abandon() override;

 private:
  struct Run;

  FacetAnswer start(const ipc::la::StartCapture& start);
  std::optional<std::string> stopCapture(bool endRing);
  bool stuckCaptureReturned();

  LogicAnalyzerRegistration registration_;
  const std::vector<ipc::props::PropertyValue>& properties_;
  plugin_ring::CaptureRing ring_;
  std::unique_ptr<Run> run_;
  std::unique_ptr<Run> stuck_;
};

}

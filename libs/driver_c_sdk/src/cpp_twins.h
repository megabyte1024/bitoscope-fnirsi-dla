/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "bitoscope_driver.h"
#include "bitoscope_driver_cpp.h"

namespace bitoscope::plugin_c_sdk {

class CProperties {
 public:
  explicit CProperties(const std::vector<plugin::PropertyNode>& tree);
  CProperties(const CProperties&) = delete;
  CProperties& operator=(const CProperties&) = delete;

  const std::string& error() const { return error_; }
  const bitoscope_property* data() const { return c_.empty() ? nullptr : c_.data(); }
  size_t size() const { return c_.size(); }

 private:
  struct Entry {
    const plugin::Property* source = nullptr;
    std::string group;
    std::vector<const char*> names;
  };

  std::vector<Entry> entries_;
  std::vector<bitoscope_property> c_;
  std::string error_;
};

class CLogicAnalyzer {
 public:
  explicit CLogicAnalyzer(const plugin::LogicAnalyzerInfo& info);
  CLogicAnalyzer(const CLogicAnalyzer&) = delete;
  CLogicAnalyzer& operator=(const CLogicAnalyzer&) = delete;

  const std::string& error() const { return properties_.error(); }
  const bitoscope_logic_analyzer& get() const { return c_; }

 private:
  CProperties properties_;
  std::vector<bitoscope_channel_preset> presets_;
  bitoscope_logic_analyzer c_{};
};

class CSignalGenerator {
 public:
  explicit CSignalGenerator(const plugin::SignalGeneratorInfo& info);
  CSignalGenerator(const CSignalGenerator&) = delete;
  CSignalGenerator& operator=(const CSignalGenerator&) = delete;

  const std::string& error() const { return properties_.error(); }
  const bitoscope_signal_generator& get() const { return c_; }

 private:
  CProperties properties_;
  std::vector<const char*> waveforms_;
  bitoscope_signal_generator c_{};
};

plugin::Capture cppCapture(const bitoscope_capture& capture);

std::vector<plugin::PropertyValue> cppValues(const bitoscope_property_value* values, size_t count);

}

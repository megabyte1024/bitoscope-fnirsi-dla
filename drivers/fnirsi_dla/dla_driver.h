/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "bitoscope_driver_cpp.h"
#include "dla_analyzer.h"
#include "dla_signal_generator.h"
#include "sample_stream_device.h"

namespace fnirsi {

inline constexpr std::string_view kDlaDriverVersion = "0.2.0";

struct OpenedDla {
  std::shared_ptr<SampleStreamDevice> device;
  std::string device_name;
  std::string description;
};

class DlaUsb {
 public:
  virtual ~DlaUsb() = default;
  virtual std::expected<std::string, bitoscope::plugin::DeviceFailure> readDeviceName(
      const bitoscope::plugin::FoundDevice& unit) = 0;
  virtual std::expected<OpenedDla, bitoscope::plugin::DeviceFailure> open(const bitoscope::plugin::FoundDevice& unit) = 0;
};

std::unique_ptr<DlaUsb> makeDlaUsb();

inline constexpr std::chrono::milliseconds kOpenRetryDelay{3000};

using OpenRetrySleep = std::function<void(std::chrono::milliseconds delay)>;

class DlaSession final : public bitoscope::plugin::Session {
 public:
  DlaSession(std::shared_ptr<SampleStreamDevice> device, std::string name);
  ~DlaSession() override;
  DlaSession(const DlaSession&) = delete;
  DlaSession& operator=(const DlaSession&) = delete;

  bitoscope::plugin::LogicAnalyzer* logicAnalyzer() override;
  bitoscope::plugin::SignalGenerator* signalGenerator() override;

 private:
  std::shared_ptr<SampleStreamDevice> device_;
  std::string name_;
  DlaAnalyzer analyzer_;
  DlaSignalGenerator generator_;
};

class DlaDriver {
 public:
  explicit DlaDriver(DlaUsb& usb, std::chrono::milliseconds open_retry_delay = kOpenRetryDelay,
                     OpenRetrySleep sleep = nullptr);

  bitoscope::plugin::DriverDescription description();

  std::expected<void, bitoscope::plugin::DeviceFailure> identify(const bitoscope::plugin::FoundDevice& unit,
                                                                 bitoscope::plugin::Device& device);
  std::expected<std::unique_ptr<bitoscope::plugin::Session>, bitoscope::plugin::DeviceFailure> open(
      const bitoscope::plugin::FoundDevice& unit);

 private:
  DlaUsb& usb_;
  std::chrono::milliseconds open_retry_delay_;
  OpenRetrySleep sleep_;
};

}

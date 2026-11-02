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
#include "dla_firmware_update.h"
#include "dla_model.h"
#include "dla_signal_generator.h"
#include "sample_stream_device.h"

namespace fnirsi {

inline constexpr std::string_view kDlaDriverVersion = "0.3.0";

struct OpenedDla {
  std::shared_ptr<SampleStreamDevice> device;
  std::string device_name;
  std::string description;
  bool in_bootloader = false;
  UpdateTransportFactory update_transport;
};

class DlaUsb {
 public:
  virtual ~DlaUsb() = default;
  virtual std::expected<DlaIdentity, bitoscope::plugin::DeviceFailure> readIdentity(
      const bitoscope::plugin::FoundDevice& unit) = 0;
  virtual std::expected<OpenedDla, bitoscope::plugin::DeviceFailure> open(const bitoscope::plugin::FoundDevice& unit) = 0;
};

std::unique_ptr<DlaUsb> makeDlaUsb();

inline constexpr std::chrono::milliseconds kOpenRetryDelay{3000};

using OpenRetrySleep = std::function<void(std::chrono::milliseconds delay)>;

class DlaSession final : public bitoscope::plugin::Session {
 public:
  DlaSession(std::shared_ptr<SampleStreamDevice> device, std::string name, FirmwareUpdateSetup update);
  ~DlaSession() override;
  DlaSession(const DlaSession&) = delete;
  DlaSession& operator=(const DlaSession&) = delete;

  bitoscope::plugin::LogicAnalyzer* logicAnalyzer() override;
  bitoscope::plugin::SignalGenerator* signalGenerator() override;
  std::expected<bitoscope::plugin::CommandAnswer, std::string> command(std::string_view facet,
                                                                       std::span<const uint8_t> payload) override;

 private:
  std::shared_ptr<SampleStreamDevice> device_;
  std::string name_;
  std::shared_ptr<UpdateGate> gate_;
  std::shared_ptr<GatedDevice> gated_;
  DlaAnalyzer analyzer_;
  DlaSignalGenerator generator_;
  DlaFirmwareUpdate update_;
};

class DlaBootloaderSession final : public bitoscope::plugin::Session {
 public:
  DlaBootloaderSession(std::shared_ptr<SampleStreamDevice> device, std::string name, FirmwareUpdateSetup update);
  ~DlaBootloaderSession() override;
  DlaBootloaderSession(const DlaBootloaderSession&) = delete;
  DlaBootloaderSession& operator=(const DlaBootloaderSession&) = delete;

  std::expected<bitoscope::plugin::CommandAnswer, std::string> command(std::string_view facet,
                                                                       std::span<const uint8_t> payload) override;

 private:
  std::shared_ptr<SampleStreamDevice> device_;
  std::string name_;
  std::shared_ptr<UpdateGate> gate_;
  DlaFirmwareUpdate update_;
};

class DlaDriver {
 public:
  explicit DlaDriver(DlaUsb& usb, std::chrono::milliseconds open_retry_delay = kOpenRetryDelay,
                     OpenRetrySleep sleep = nullptr, FirmwareUpdateTuning update_tuning = {});

  bitoscope::plugin::DriverDescription description();

  std::expected<void, bitoscope::plugin::DeviceFailure> identify(const bitoscope::plugin::FoundDevice& unit,
                                                                 bitoscope::plugin::Device& device);
  std::expected<std::unique_ptr<bitoscope::plugin::Session>, bitoscope::plugin::DeviceFailure> open(
      const bitoscope::plugin::FoundDevice& unit);

 private:
  DlaUsb& usb_;
  std::chrono::milliseconds open_retry_delay_;
  OpenRetrySleep sleep_;
  FirmwareUpdateTuning update_tuning_;
};

}

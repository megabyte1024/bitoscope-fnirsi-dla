/*
 * SPDX-License-Identifier: MIT
 */

#include "driver_source.h"

#include <string>
#include <utility>
#include <vector>

#include "driver_calls.h"

namespace bitoscope::plugin_c_sdk {

namespace {

class EnumerateSource final : public DeviceSource {
 public:
  EnumerateSource(const bitoscope_driver& driver, const bitoscope_connection& connection)
      : driver_(driver), connection_(connection) {}

  std::expected<std::vector<SourceDevice>, std::string> find() override {
    bitoscope_found_devices found;
    int result = BITOSCOPE_ERROR;
    try {
      result = driver_.enumerate(driver_.user, &connection_, &found);
    } catch (...) {
      result = BITOSCOPE_ERROR;
    }
    if (result != BITOSCOPE_OK) {
      return std::unexpected("driver could not look for its devices (its enumerate function returned " +
                             std::to_string(result) + ")");
    }
    return std::move(found.devices);
  }

  AvailabilityCheck check([[maybe_unused]] const SourceDevice& device) override { return {Availability::free, {}}; }

  std::unique_ptr<ChangeNotices> changeNotices() override { return makeSilentChangeNotices(); }

 private:
  const bitoscope_driver& driver_;
  const bitoscope_connection& connection_;
};

class HeldCheckSource final : public DeviceSource {
 public:
  HeldCheckSource(std::unique_ptr<DeviceSource> source, const bitoscope_driver& driver,
                  const bitoscope_connection& connection)
      : source_(std::move(source)), driver_(driver), connection_(connection) {}

  std::expected<std::vector<SourceDevice>, std::string> find() override { return source_->find(); }

  AvailabilityCheck check(const SourceDevice& device) override {
    const GivenDevice given(device, connection_);
    int held = BITOSCOPE_OK;
    try {
      held = driver_.held_check(driver_.user, &given.get());
    } catch (...) {
      held = BITOSCOPE_OK;
    }
    return {held == BITOSCOPE_BUSY ? Availability::busy : Availability::free, {}};
  }

  std::unique_ptr<ChangeNotices> changeNotices() override { return source_->changeNotices(); }

 private:
  std::unique_ptr<DeviceSource> source_;
  const bitoscope_driver& driver_;
  const bitoscope_connection& connection_;
};

}

std::unique_ptr<DeviceSource> makeEnumerateSource(const bitoscope_driver& driver,
                                                  const bitoscope_connection& connection) {
  return std::make_unique<EnumerateSource>(driver, connection);
}

std::unique_ptr<DeviceSource> withHeldCheck(std::unique_ptr<DeviceSource> source, const bitoscope_driver& driver,
                                            const bitoscope_connection& connection) {
  if (driver.held_check == nullptr) return source;
  return std::make_unique<HeldCheckSource>(std::move(source), driver, connection);
}

}

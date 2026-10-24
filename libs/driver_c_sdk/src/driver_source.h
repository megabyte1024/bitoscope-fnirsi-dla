/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>

#include "bitoscope_driver.h"
#include "device_source.h"

namespace bitoscope::plugin_c_sdk {

std::unique_ptr<DeviceSource> makeEnumerateSource(const bitoscope_driver& driver,
                                                  const bitoscope_connection& connection);

std::unique_ptr<DeviceSource> withHeldCheck(std::unique_ptr<DeviceSource> source, const bitoscope_driver& driver,
                                            const bitoscope_connection& connection);

}

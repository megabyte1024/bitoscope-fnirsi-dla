/*
 * SPDX-License-Identifier: MIT
 */

#include "device_finder.h"

#include <new>
#include <utility>

struct bitoscope_finder {
  std::unique_ptr<bitoscope::plugin_c_sdk::DeviceFinder> finder;
};

namespace bitoscope::plugin_c_sdk {

bitoscope_finder* adoptedFinder(std::unique_ptr<DeviceFinder> finder) {
  return new (std::nothrow) bitoscope_finder{std::move(finder)};
}

const DeviceFinder& finderOf(const bitoscope_finder& finder) { return *finder.finder; }

}

extern "C" void bitoscope_free_finder(bitoscope_finder* finder) { delete finder; }

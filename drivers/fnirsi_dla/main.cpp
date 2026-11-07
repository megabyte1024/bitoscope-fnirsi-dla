/*
 * SPDX-License-Identifier: MIT
 */

#include <memory>

#include "bitoscope_driver_usb_cpp.h"
#include "dla_driver.h"
#include "wch/wch_route.h"

int main(int argc, char** argv) {
  fnirsi::wch::useWchDriverWhereBound();
  const std::unique_ptr<fnirsi::DlaUsb> usb = fnirsi::makeDlaUsb();
  fnirsi::DlaDriver dla(*usb);
  bitoscope::plugin::Driver driver(dla.description());
  return driver.run(argc, argv);
}

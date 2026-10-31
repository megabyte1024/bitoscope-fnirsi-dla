/*
 * SPDX-License-Identifier: MIT
 */

#include <memory>

#include "bitoscope_driver_cpp.h"
#include "dla_driver.h"

int main(int argc, char** argv) {
  const std::unique_ptr<fnirsi::DlaUsb> usb = fnirsi::makeDlaUsb();
  fnirsi::DlaDriver dla(*usb);
  bitoscope::plugin::Driver driver(dla.description());
  return driver.run(argc, argv);
}

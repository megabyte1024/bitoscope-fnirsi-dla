/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "bitoscope_driver_cpp.h"
#include "frame_sender.h"

namespace fnirsi {

class SampleStreamDevice : public FramedCommandSender {
 public:
  virtual void streamSamples(bitoscope::plugin::Sink& sink, const bitoscope::plugin::UsbStream& stream) = 0;
};

}

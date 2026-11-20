/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

#include "bitoscope_driver_usb_cpp.h"
#include "frame_sender.h"

namespace fnirsi {

enum class EndpointResetOutcome { notAttempted, done, failed };

struct EndpointReset {
  EndpointResetOutcome outcome = EndpointResetOutcome::notAttempted;
  std::string failure;
};

class SampleStreamDevice : public FramedCommandSender {
 public:
  virtual void streamSamples(bitoscope::plugin::Sink& sink, const bitoscope::plugin::UsbStream& stream) = 0;
  virtual EndpointReset endpointResetAfterStream() const { return {}; }
};

}

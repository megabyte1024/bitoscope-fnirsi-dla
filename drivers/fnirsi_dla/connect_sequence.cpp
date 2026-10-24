/*
 * SPDX-License-Identifier: MIT
 */

#include "connect_sequence.h"

#include <array>

namespace fnirsi {

std::vector<HousekeepingStep> connectHousekeeping() {
  // WHY: Opening the device must not stop PWM: the signal generator stays on until explicitly stopped (owner
  // decision).
  const std::array<uint8_t, 1> release_reset{0x01};
  constexpr bool readsReply = false;
  return {
      HousekeepingStep{DirectCommand::kSetResetState,
                        std::vector<uint8_t>(release_reset.begin(), release_reset.end()),
                        readsReply},
  };
}

std::vector<Farewell> closeFarewell() {
  // WHY: Closing the device must not stop PWM either: the signal generator stays on until explicitly stopped.
  return {
      Farewell{FramedCommand::kExit, {}},
  };
}

}

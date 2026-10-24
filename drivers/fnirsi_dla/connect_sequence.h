/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "command_link.h"
#include "fnirsi_protocol.h"

namespace fnirsi {

struct HousekeepingStep {
  std::variant<FramedCommand, DirectCommand> cmd;
  std::vector<uint8_t> payload;
  bool read_reply = false;
};

std::vector<HousekeepingStep> connectHousekeeping();

std::vector<Farewell> closeFarewell();

}

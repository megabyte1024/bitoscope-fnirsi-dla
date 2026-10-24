/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <memory>
#include <string>

#include "ipc_byte_stream.h"

namespace bitoscope::plugin_c_sdk {

struct PrivateStdio {
  std::unique_ptr<ipc::ByteSource> in;
  std::unique_ptr<ipc::ByteSink> out;
};

std::expected<PrivateStdio, std::string> takeStdio(int strayOutputFd = 2);

}

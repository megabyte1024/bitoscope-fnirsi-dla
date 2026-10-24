/*
 * SPDX-License-Identifier: MIT
 */

#include <cstdarg>
#include <cstdio>
#include <string>

#include "bitoscope_plugin_common.h"
#include "frame_output.h"
#include "plugin_model.h"

namespace sdk = bitoscope::plugin_c_sdk;
namespace ipc = bitoscope::ipc;

namespace {

ipc::LogLevel levelOf(int level) {
  switch (level) {
    case BITOSCOPE_LOG_DEBUG:
      return ipc::LogLevel::debug;
    case BITOSCOPE_LOG_WARNING:
      return ipc::LogLevel::warning;
    case BITOSCOPE_LOG_ERROR:
      return ipc::LogLevel::error;
    default:
      return ipc::LogLevel::info;
  }
}

}

extern "C" {

void bitoscope_log(int level, const char* format, ...) {
  try {
    if (format == nullptr) return;
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int length = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    std::string message;
    if (length > 0) {
      message.resize(static_cast<size_t>(length) + 1);
      std::vsnprintf(message.data(), message.size(), format, again);
      message.resize(static_cast<size_t>(length));
    }
    va_end(again);
    sdk::logLine(levelOf(level), message);
  } catch (...) {
  }
}

int bitoscope_reply_message(bitoscope_reply* reply, const char* message) {
  try {
    if (reply == nullptr) return BITOSCOPE_ERROR;
    reply->message = message != nullptr ? message : "";
    return BITOSCOPE_OK;
  } catch (...) {
    return BITOSCOPE_ERROR;
  }
}

}

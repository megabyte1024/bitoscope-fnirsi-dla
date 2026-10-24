/*
 * SPDX-License-Identifier: MIT
 */

#include "device_source.h"

namespace bitoscope::plugin_c_sdk {

namespace {

class SilentChangeNotices final : public ChangeNotices {
 public:
  std::expected<void, std::string> start([[maybe_unused]] std::function<void()> changed,
                                         [[maybe_unused]] std::function<void(std::string_view why)> failed) override {
    return {};
  }
  void stop() override {}
};

}

std::unique_ptr<ChangeNotices> makeSilentChangeNotices() { return std::make_unique<SilentChangeNotices>(); }

}

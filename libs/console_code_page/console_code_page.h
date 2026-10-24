/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>

namespace bitoscope::console {

inline constexpr unsigned kUtf8CodePage = 65001;

class ConsoleCodePage {
 public:
  virtual ~ConsoleCodePage() = default;
  virtual unsigned output() = 0;
  virtual bool setOutput(unsigned code_page) = 0;
};

ConsoleCodePage& systemConsoleCodePage();

// WHY: One Utf8ConsoleOutput at a time: its state is process-wide, so a signal handler can reach it.
class Utf8ConsoleOutput {
 public:
  explicit Utf8ConsoleOutput(ConsoleCodePage& console);
  ~Utf8ConsoleOutput();
  Utf8ConsoleOutput(const Utf8ConsoleOutput&) = delete;
  Utf8ConsoleOutput& operator=(const Utf8ConsoleOutput&) = delete;

  bool switched() const { return switched_; }

  void restore();

  static void restoreActive();

 private:
  bool switched_ = false;
};

int runWithUtf8Console(ConsoleCodePage& console, const std::function<int()>& run);

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "console_code_page.h"

#include <atomic>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bitoscope::console {

namespace {

#ifdef _WIN32
class SystemConsole final : public ConsoleCodePage {
 public:
  unsigned output() override { return static_cast<unsigned>(GetConsoleOutputCP()); }
  bool setOutput(unsigned code_page) override { return SetConsoleOutputCP(static_cast<UINT>(code_page)) != 0; }
};
#else
class SystemConsole final : public ConsoleCodePage {
 public:
  unsigned output() override { return 0; }
  bool setOutput(unsigned) override { return false; }
};
#endif

std::atomic<ConsoleCodePage*> g_console{nullptr};
std::atomic<unsigned> g_previous{0};
std::atomic<bool> g_pending{false};

static_assert(std::atomic<ConsoleCodePage*>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free,
              "restoreActive() runs in a signal handler: lock-free atomics only");

void restorePending() {
  if (!g_pending.exchange(false, std::memory_order_acq_rel)) return;
  if (ConsoleCodePage* console = g_console.load(std::memory_order_acquire)) {
    (void)console->setOutput(g_previous.load(std::memory_order_acquire));
  }
}

}

ConsoleCodePage& systemConsoleCodePage() {
  static SystemConsole console;
  return console;
}

Utf8ConsoleOutput::Utf8ConsoleOutput(ConsoleCodePage& console) {
  const unsigned previous = console.output();
  if (previous == 0 || previous == kUtf8CodePage) return;
  if (!console.setOutput(kUtf8CodePage)) return;
  g_previous.store(previous, std::memory_order_release);
  g_console.store(&console, std::memory_order_release);
  g_pending.store(true, std::memory_order_release);
  switched_ = true;
}

Utf8ConsoleOutput::~Utf8ConsoleOutput() { restore(); }

void Utf8ConsoleOutput::restore() {
  if (switched_) restorePending();
}

void Utf8ConsoleOutput::restoreActive() { restorePending(); }

int runWithUtf8Console(ConsoleCodePage& console, const std::function<int()>& run) {
  Utf8ConsoleOutput utf8(console);
  try {
    return run();
  } catch (...) {
    // WHY: Restore the code page first: an uncaught exception ends in std::terminate(), which need not unwind.
    utf8.restore();
    throw;
  }
}

}

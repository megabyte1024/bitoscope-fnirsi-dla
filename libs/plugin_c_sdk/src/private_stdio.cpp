/*
 * SPDX-License-Identifier: MIT
 */

#include "private_stdio.h"

#include <cstdio>
#include <iostream>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace bitoscope::plugin_c_sdk {

namespace {

#if defined(_WIN32)

std::string lastError(const char* what) {
  return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

class HandleSource final : public ipc::ByteSource {
 public:
  explicit HandleSource(HANDLE handle) : handle_(handle) {}
  ~HandleSource() override { CloseHandle(handle_); }
  HandleSource(const HandleSource&) = delete;
  HandleSource& operator=(const HandleSource&) = delete;

  std::expected<std::size_t, std::string> read(std::span<uint8_t> into) override {
    DWORD got = 0;
    const DWORD want = into.size() > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(into.size());
    if (!ReadFile(handle_, into.data(), want, &got, nullptr)) {
      const DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return std::size_t{0};
      return std::unexpected("ReadFile on stdin failed (error " + std::to_string(error) + ")");
    }
    return static_cast<std::size_t>(got);
  }

 private:
  HANDLE handle_;
};

class HandleSink final : public ipc::ByteSink {
 public:
  explicit HandleSink(HANDLE handle) : handle_(handle) {}
  ~HandleSink() override { CloseHandle(handle_); }
  HandleSink(const HandleSink&) = delete;
  HandleSink& operator=(const HandleSink&) = delete;

  std::expected<void, std::string> write(std::span<const uint8_t> bytes) override {
    std::size_t done = 0;
    while (done < bytes.size()) {
      const std::size_t left = bytes.size() - done;
      const DWORD chunk = left > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(left);
      DWORD wrote = 0;
      if (!WriteFile(handle_, bytes.data() + done, chunk, &wrote, nullptr)) {
        return std::unexpected("WriteFile on stdout failed (error " + std::to_string(GetLastError()) + ")");
      }
      done += wrote;
    }
    return {};
  }

 private:
  HANDLE handle_;
};

// WHY: Made BEFORE any _dup2 touches `fd`.
std::expected<HANDLE, std::string> duplicateOf(int fd, const char* name) {
  const auto original = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (original == INVALID_HANDLE_VALUE || original == nullptr) return std::unexpected(std::string(name) + " is not open");
  HANDLE copy = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return std::unexpected(lastError("DuplicateHandle"));
  }
  return copy;
}

#else

class FdSource final : public ipc::ByteSource {
 public:
  explicit FdSource(int fd) : fd_(fd) {}
  ~FdSource() override { ::close(fd_); }
  FdSource(const FdSource&) = delete;
  FdSource& operator=(const FdSource&) = delete;

  std::expected<std::size_t, std::string> read(std::span<uint8_t> into) override {
    for (;;) {
      const ssize_t got = ::read(fd_, into.data(), into.size());
      if (got >= 0) return static_cast<std::size_t>(got);
      if (errno != EINTR) return std::unexpected(std::string("read on stdin failed: ") + std::strerror(errno));
    }
  }

 private:
  int fd_;
};

class FdSink final : public ipc::ByteSink {
 public:
  explicit FdSink(int fd) : fd_(fd) {}
  ~FdSink() override { ::close(fd_); }
  FdSink(const FdSink&) = delete;
  FdSink& operator=(const FdSink&) = delete;

  std::expected<void, std::string> write(std::span<const uint8_t> bytes) override {
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t wrote = ::write(fd_, bytes.data() + done, bytes.size() - done);
      if (wrote < 0) {
        if (errno == EINTR) continue;
        return std::unexpected(std::string("write on stdout failed: ") + std::strerror(errno));
      }
      done += static_cast<std::size_t>(wrote);
    }
    return {};
  }

 private:
  int fd_;
};

#endif

}

std::expected<PrivateStdio, std::string> takeStdio(int strayOutputFd) {
  std::cout.flush();
  std::fflush(stdout);
#if defined(_WIN32)
  auto in = duplicateOf(0, "stdin");
  if (!in) return std::unexpected(in.error());
  auto out = duplicateOf(1, "stdout");
  if (!out) {
    CloseHandle(*in);
    return std::unexpected(out.error());
  }
  PrivateStdio result{std::make_unique<HandleSource>(*in), std::make_unique<HandleSink>(*out)};
  // WHY: Only now may fd 1 be replaced: _dup2 closes the handle it replaces.
  if (_dup2(strayOutputFd, 1) != 0) return std::unexpected(std::string("pointing stdout at stderr failed"));
  SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
  int nul = -1;
  if (_sopen_s(&nul, "NUL", _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) == 0 && nul >= 0) {
    _dup2(nul, 0);
    _close(nul);
    SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(0)));
  }
  return result;
#else
  const int in = ::fcntl(0, F_DUPFD_CLOEXEC, 3);
  if (in < 0) return std::unexpected(std::string("stdin cannot be duplicated: ") + std::strerror(errno));
  const int out = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
  if (out < 0) {
    ::close(in);
    return std::unexpected(std::string("stdout cannot be duplicated: ") + std::strerror(errno));
  }
  PrivateStdio result{std::make_unique<FdSource>(in), std::make_unique<FdSink>(out)};
  if (::dup2(strayOutputFd, 1) < 0) return std::unexpected(std::string("pointing stdout at stderr failed"));
  const int nul = ::open("/dev/null", O_RDONLY);
  if (nul >= 0) {
    ::dup2(nul, 0);
    ::close(nul);
  }
  // WHY: A host that died must not kill the plugin with SIGPIPE: the write fails and the loop ends.
  std::signal(SIGPIPE, SIG_IGN);
  return result;
#endif
}

}

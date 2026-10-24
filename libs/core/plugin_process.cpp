/*
 * SPDX-License-Identifier: MIT
 */

#include "plugin_process.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace bitoscope::host {

StderrLines::StderrLines(std::string prefix, StderrLineFn sink) : prefix_(std::move(prefix)), sink_(std::move(sink)) {}

void StderrLines::feed(std::span<const uint8_t> bytes) {
  for (uint8_t byte : bytes) {
    if (byte == '\n') {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      emit();
      continue;
    }
    line_.push_back(static_cast<char>(byte));
    if (line_.size() >= kMaxLineBytes) emit();
  }
}

void StderrLines::finish() {
  if (line_.empty()) return;
  if (line_.back() == '\r') line_.pop_back();
  emit();
}

void StderrLines::emit() {
  if (sink_) sink_(prefix_ + ": " + line_);
  line_.clear();
}

namespace {

std::string pathText(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

void placeStderrThread(const cpu::WorkPlacement& placement, const std::string& prefix, const StderrLineFn& sink) {
  if (placement.asksNothing()) return;
  if (auto placed = cpu::applyToThisThread(placement); !placed && sink) sink(prefix + ": stderr thread: " + placed.error());
}

struct DoneFlag {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;

  void set() {
    {
      std::lock_guard lock(mutex);
      done = true;
    }
    cv.notify_all();
  }
  bool waitFor(std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, limit, [this] { return done; });
  }
};

}

#if defined(_WIN32)

namespace {

// WHY: Plugin output pipe is 1 MiB: the small default made every 32 KiB event write wait for the host's reader.
constexpr DWORD kPluginOutputPipeBytes = 1024 * 1024;

std::string windowsError(DWORD error) {
  char* text = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
      MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<char*>(&text), 0, nullptr);
  std::string out = "error " + std::to_string(error);
  if (length != 0 && text != nullptr) {
    std::string message(text, length);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
                                message.back() == '.')) {
      message.pop_back();
    }
    out += " (" + message + ")";
  }
  if (text != nullptr) LocalFree(text);
  return out;
}

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::wstring quoted(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(*it);
  }
  out.push_back(L'"');
  return out;
}

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() { reset(); }
  Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  HANDLE get() const { return handle_; }
  HANDLE* out() { return &handle_; }
  void reset() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = nullptr;
  }

 private:
  HANDLE handle_ = nullptr;
};

class PipeSource final : public ipc::ByteSource {
 public:
  explicit PipeSource(Handle handle) : handle_(std::move(handle)) {}

  std::expected<std::size_t, std::string> read(std::span<uint8_t> into) override {
    DWORD got = 0;
    const DWORD want = into.size() > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(into.size());
    reading_.store(true);
    const BOOL ok = ReadFile(handle_.get(), into.data(), want, &got, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    reading_.store(false);
    if (!ok) {
      if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_OPERATION_ABORTED) {
        ended_.store(true);
        return std::size_t{0};
      }
      return std::unexpected("reading plugin's stdout failed: " + windowsError(error));
    }
    if (got == 0) ended_.store(true);
    return static_cast<std::size_t>(got);
  }

  void cancelIfStuck(std::chrono::milliseconds patience) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!ended_.load() && reading_.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!ended_.load() && reading_.load()) CancelIoEx(handle_.get(), nullptr);
  }

 private:
  Handle handle_;
  std::atomic<bool> reading_{false};
  std::atomic<bool> ended_{false};
};

class PipeSink final : public ipc::ByteSink {
 public:
  explicit PipeSink(Handle handle) : handle_(std::move(handle)) {}
  ~PipeSink() override {
    closed_.store(true);
    std::lock_guard lock(mutex_);
    handle_.reset();
  }

  std::expected<void, std::string> write(std::span<const uint8_t> bytes) override {
    std::lock_guard lock(mutex_);
    if (closed_.load()) return std::unexpected(std::string("plugin's stdin is closed"));
    std::size_t done = 0;
    while (done < bytes.size()) {
      const std::size_t left = bytes.size() - done;
      const DWORD chunk = left > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(left);
      DWORD wrote = 0;
      if (!WriteFile(handle_.get(), bytes.data() + done, chunk, &wrote, nullptr)) {
        return std::unexpected("writing plugin's stdin failed: " + windowsError(GetLastError()));
      }
      done += wrote;
    }
    return {};
  }

  void close() {
    if (!closed_.exchange(true)) CancelIoEx(handle_.get(), nullptr);
    std::unique_lock lock(mutex_, std::defer_lock);
    if (lock.try_lock_for(std::chrono::milliseconds(200))) handle_.reset();
  }

 private:
  std::timed_mutex mutex_;
  std::atomic<bool> closed_{false};
  Handle handle_;
};

}

struct PluginProcess::Native {
  std::string name;
  Handle process;
  DWORD pid = 0;
  std::unique_ptr<PipeSource> out;
  std::unique_ptr<PipeSink> in;
  Handle errRead;
  std::thread errThread;
  DoneFlag errDone;
  std::mutex exitMutex;
  std::optional<int> exitCode;
};

std::expected<std::unique_ptr<PluginProcess>, std::string> PluginProcess::start(PluginProcessOptions options) {
  auto native = std::make_unique<Native>();
  native->name = options.name.empty() ? pathText(options.executable.stem()) : options.name;
  const std::string what = "cannot start plugin " + pathText(options.executable);
  const auto affinity = cpu::hardwareThreadMask(options.placement.hardware_threads);
  if (!affinity) return std::unexpected(what + ": " + affinity.error());

  // WHY: Pipes are created non-inheritable; only the child's ends become inheritable, for the one CreateProcess below.
  Handle inRead, inWrite, outRead, outWrite, errRead, errWrite;
  if (!CreatePipe(inRead.out(), inWrite.out(), nullptr, 0) ||
      !CreatePipe(outRead.out(), outWrite.out(), nullptr, kPluginOutputPipeBytes) ||
      !CreatePipe(errRead.out(), errWrite.out(), nullptr, 0)) {
    return std::unexpected(what + ": CreatePipe failed: " + windowsError(GetLastError()));
  }
  for (HANDLE child : {inRead.get(), outWrite.get(), errWrite.get()}) {
    if (!SetHandleInformation(child, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
      return std::unexpected(what + ": SetHandleInformation failed: " + windowsError(GetLastError()));
    }
  }

  HANDLE inherited[3] = {inRead.get(), outWrite.get(), errWrite.get()};
  SIZE_T attributeBytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
  std::vector<uint8_t> attributeStorage(attributeBytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
    return std::unexpected(what + ": InitializeProcThreadAttributeList failed: " + windowsError(GetLastError()));
  }
  struct AttributeListGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~AttributeListGuard() { DeleteProcThreadAttributeList(list); }
  } attributeGuard{attributes};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited,
                                 nullptr, nullptr)) {
    return std::unexpected(what + ": UpdateProcThreadAttribute failed: " + windowsError(GetLastError()));
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof startup;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = inRead.get();
  startup.StartupInfo.hStdOutput = outWrite.get();
  startup.StartupInfo.hStdError = errWrite.get();
  startup.lpAttributeList = attributes;

  const std::wstring application = options.executable.wstring();
  std::wstring commandLine = quoted(application);
  for (const std::string& arg : options.arguments) commandLine += L" " + quoted(widen(arg));

  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  if (options.placement.below_normal_priority) flags |= BELOW_NORMAL_PRIORITY_CLASS;
  if (*affinity != 0) flags |= CREATE_SUSPENDED;
  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                      nullptr, &startup.StartupInfo, &info);
  const DWORD createError = created ? 0 : GetLastError();
  // WHY: The host keeps none of the child's ends: while it held one, the child's exit would not close its stdout.
  inRead.reset();
  outWrite.reset();
  errWrite.reset();
  if (!created) return std::unexpected(what + ": " + windowsError(createError));
  Handle process(info.hProcess);
  Handle mainThread(info.hThread);
  if (*affinity != 0) {
    std::optional<std::string> failure;
    if (!SetProcessAffinityMask(process.get(), static_cast<DWORD_PTR>(*affinity))) {
      failure = "process could not be placed on hardware threads " +
                cpu::hardwareThreadList(options.placement.hardware_threads) + ": " + windowsError(GetLastError());
    } else if (ResumeThread(mainThread.get()) == static_cast<DWORD>(-1)) {
      failure = "process could not be let run: " + windowsError(GetLastError());
    }
    if (failure) {
      TerminateProcess(process.get(), 1);
      WaitForSingleObject(process.get(), 5000);
      return std::unexpected(what + ": " + *failure);
    }
  }

  native->process = std::move(process);
  native->pid = info.dwProcessId;
  native->out = std::make_unique<PipeSource>(std::move(outRead));
  native->in = std::make_unique<PipeSink>(std::move(inWrite));
  native->errRead = std::move(errRead);
  Native* raw = native.get();
  native->errThread = std::thread([raw, prefix = native->name, sink = std::move(options.onStderrLine),
                                   placement = std::move(options.placement)] {
    placeStderrThread(placement, prefix, sink);
    StderrLines lines(prefix, sink);
    uint8_t buffer[4096];
    for (;;) {
      DWORD got = 0;
      if (!ReadFile(raw->errRead.get(), buffer, sizeof buffer, &got, nullptr) || got == 0) break;
      lines.feed(std::span<const uint8_t>(buffer, got));
    }
    lines.finish();
    raw->errDone.set();
  });
  return std::unique_ptr<PluginProcess>(new PluginProcess(std::move(native)));
}

PluginProcess::~PluginProcess() {
  shutdown(kShutdownGrace);
  if (native_->errThread.joinable()) {
    for (int attempt = 0; !native_->errDone.waitFor(std::chrono::milliseconds(attempt == 0 ? 1000 : 50)); ++attempt) {
      CancelSynchronousIo(native_->errThread.native_handle());
      if (attempt > 100) break;
    }
    if (native_->errDone.waitFor(std::chrono::milliseconds(0))) {
      native_->errThread.join();
    } else {
      native_->errThread.detach();
      (void)native_.release();  // WHY: Leaked on purpose: the reader thread still uses the native state.
      return;
    }
  }
}

std::optional<int> PluginProcess::waitForExit(std::chrono::milliseconds wait) {
  // WHY: No lock while waiting: kill() on another thread must not wait behind a long wait here.
  const DWORD ms = static_cast<DWORD>((std::max<int64_t>)(0, (std::min<int64_t>)(wait.count(), 0x7FFFFFFE)));
  if (WaitForSingleObject(native_->process.get(), ms) != WAIT_OBJECT_0) return std::nullopt;
  std::lock_guard lock(native_->exitMutex);
  if (!native_->exitCode) {
    DWORD code = 0;
    if (!GetExitCodeProcess(native_->process.get(), &code)) return std::nullopt;
    native_->exitCode = static_cast<int>(code);
  }
  return native_->exitCode;
}

void PluginProcess::closeInput() { native_->in->close(); }

void PluginProcess::kill() {
  if (!waitForExit(std::chrono::milliseconds(0))) {
    TerminateProcess(native_->process.get(), 1);
    (void)waitForExit(std::chrono::milliseconds(5000));
  }
}

void PluginProcess::shutdown(std::chrono::milliseconds grace) {
  closeInput();
  if (!waitForExit(grace)) kill();
  if (waitForExit(std::chrono::milliseconds(0))) native_->out->cancelIfStuck(std::chrono::milliseconds(500));
}

uint64_t PluginProcess::processId() const { return native_->pid; }

#else

namespace {

std::string posixError(const char* what, int error) { return std::string(what) + ": " + std::strerror(error); }

bool makePipe(int fds[2]) {
#if defined(__linux__)
  return ::pipe2(fds, O_CLOEXEC) == 0;
#else
  // WHY: Without pipe2, a spawn on another thread between pipe() and fcntl() could inherit these ends.
  if (::pipe(fds) != 0) return false;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}

class FdSource final : public ipc::ByteSource {
 public:
  explicit FdSource(int fd) : fd_(fd) {}
  ~FdSource() override { ::close(fd_); }
  std::expected<std::size_t, std::string> read(std::span<uint8_t> into) override {
    for (;;) {
      const ssize_t got = ::read(fd_, into.data(), into.size());
      if (got >= 0) return static_cast<std::size_t>(got);
      if (errno != EINTR) return std::unexpected(posixError("reading plugin's stdout failed", errno));
    }
  }

 private:
  int fd_;
};

class FdSink final : public ipc::ByteSink {
 public:
  explicit FdSink(int fd) : fd_(fd) {}
  ~FdSink() override { close(); }
  std::expected<void, std::string> write(std::span<const uint8_t> bytes) override {
    std::lock_guard lock(mutex_);
    if (fd_ < 0) return std::unexpected(std::string("plugin's stdin is closed"));
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t wrote = ::write(fd_, bytes.data() + done, bytes.size() - done);
      if (wrote < 0) {
        if (errno == EINTR) continue;
        return std::unexpected(posixError("writing plugin's stdin failed", errno));
      }
      done += static_cast<std::size_t>(wrote);
    }
    return {};
  }
  void close() {
    std::lock_guard lock(mutex_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  std::mutex mutex_;
  int fd_;
};

}

struct PluginProcess::Native {
  std::string name;
  pid_t pid = -1;
  std::unique_ptr<FdSource> out;
  std::unique_ptr<FdSink> in;
  int errFd = -1;
  std::atomic<bool> stopErr{false};
  std::thread errThread;
  DoneFlag errDone;
  std::mutex exitMutex;
  std::optional<int> exitCode;
};

std::expected<std::unique_ptr<PluginProcess>, std::string> PluginProcess::start(PluginProcessOptions options) {
  // WHY: A write to a plugin that died must fail, not kill the host.
  std::signal(SIGPIPE, SIG_IGN);
  auto native = std::make_unique<Native>();
  native->name = options.name.empty() ? pathText(options.executable.stem()) : options.name;
  const std::string what = "cannot start plugin " + pathText(options.executable);

  int in[2], out[2], err[2];
  if (!makePipe(in)) return std::unexpected(posixError((what + ": pipe").c_str(), errno));
  if (!makePipe(out)) {
    ::close(in[0]), ::close(in[1]);
    return std::unexpected(posixError((what + ": pipe").c_str(), errno));
  }
  if (!makePipe(err)) {
    ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
    return std::unexpected(posixError((what + ": pipe").c_str(), errno));
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);

  const std::string program = options.executable.string();
  std::vector<std::string> args{program};
  args.insert(args.end(), options.arguments.begin(), options.arguments.end());
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  int spawned = 0;
  std::optional<std::string> notPlaced;
  const auto spawn = [&] { spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ); };
  if (options.placement.asksNothing()) {
    spawn();
  } else {
    std::thread spawner([&] {
      if (auto placed = cpu::applyToThisThread(options.placement); !placed) {
        notPlaced = placed.error();
      } else {
        spawn();
      }
    });
    spawner.join();
  }
  posix_spawn_file_actions_destroy(&actions);
  ::close(in[0]);
  ::close(out[1]);
  ::close(err[1]);
  if (notPlaced || spawned != 0) {
    ::close(in[1]), ::close(out[0]), ::close(err[0]);
    return std::unexpected(notPlaced ? what + ": " + *notPlaced : posixError(what.c_str(), spawned));
  }
  native->pid = pid;
  native->out = std::make_unique<FdSource>(out[0]);
  native->in = std::make_unique<FdSink>(in[1]);
  native->errFd = err[0];
  Native* raw = native.get();
  native->errThread = std::thread([raw, prefix = native->name, sink = std::move(options.onStderrLine),
                                   placement = std::move(options.placement)] {
    placeStderrThread(placement, prefix, sink);
    StderrLines lines(prefix, sink);
    uint8_t buffer[4096];
    while (!raw->stopErr.load()) {
      pollfd p{raw->errFd, POLLIN, 0};
      const int ready = ::poll(&p, 1, 100);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) break;
      if (ready == 0) continue;
      const ssize_t got = ::read(raw->errFd, buffer, sizeof buffer);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      lines.feed(std::span<const uint8_t>(buffer, static_cast<size_t>(got)));
    }
    lines.finish();
    raw->errDone.set();
  });
  return std::unique_ptr<PluginProcess>(new PluginProcess(std::move(native)));
}

PluginProcess::~PluginProcess() {
  shutdown(kShutdownGrace);
  if (native_->errThread.joinable()) {
    if (!native_->errDone.waitFor(std::chrono::milliseconds(1000))) native_->stopErr.store(true);
    native_->errThread.join();
  }
  if (native_->errFd >= 0) ::close(native_->errFd);
}

std::optional<int> PluginProcess::waitForExit(std::chrono::milliseconds wait) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  for (;;) {
    {
      // WHY: Lock held for one non-blocking waitpid() at a time: the child must be reaped exactly once.
      std::lock_guard lock(native_->exitMutex);
      if (native_->exitCode) return native_->exitCode;
      int status = 0;
      const pid_t done = ::waitpid(native_->pid, &status, WNOHANG);
      if (done == native_->pid) {
        native_->exitCode =
            WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return native_->exitCode;
      }
      if (done < 0 && errno != EINTR) return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void PluginProcess::closeInput() { native_->in->close(); }

void PluginProcess::kill() {
  if (!waitForExit(std::chrono::milliseconds(0))) {
    ::kill(native_->pid, SIGKILL);
    (void)waitForExit(std::chrono::milliseconds(5000));
  }
}

void PluginProcess::shutdown(std::chrono::milliseconds grace) {
  closeInput();
  if (!waitForExit(grace)) kill();
}

uint64_t PluginProcess::processId() const { return static_cast<uint64_t>(native_->pid); }

#endif

PluginProcess::PluginProcess(std::unique_ptr<Native> native) : native_(std::move(native)) {}

const std::string& PluginProcess::name() const { return native_->name; }
ipc::ByteSource& PluginProcess::fromPlugin() { return *native_->out; }
ipc::ByteSink& PluginProcess::toPlugin() { return *native_->in; }

std::vector<std::filesystem::path> listPluginExecutables(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> found;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    std::error_code statusError;
    if (!it->is_regular_file(statusError) || statusError) continue;
#if defined(_WIN32)
    std::string extension = pathText(it->path().extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    if (extension != ".exe") continue;
#else
    const auto mode = it->status(statusError).permissions();
    using std::filesystem::perms;
    if (statusError || (mode & (perms::owner_exec | perms::group_exec | perms::others_exec)) == perms::none) continue;
#endif
    found.push_back(it->path());
  }
  std::sort(found.begin(), found.end());
  return found;
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "bulk_ring.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>
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
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace bitoscope::bulk_ring {

namespace {

template <typename T>
T loadAcquire(T& word) {
  return std::atomic_ref<T>(word).load(std::memory_order_acquire);
}

template <typename T>
T loadRelaxed(T& word) {
  return std::atomic_ref<T>(word).load(std::memory_order_relaxed);
}

template <typename T>
void storeRelease(T& word, T value) {
  std::atomic_ref<T>(word).store(value, std::memory_order_release);
}

// WHY: Wake-up handshake (store then load on each side) needs one total order: sequentially consistent.
template <typename T>
T loadSeqCst(T& word) {
  return std::atomic_ref<T>(word).load(std::memory_order_seq_cst);
}

template <typename T>
void storeSeqCst(T& word, T value) {
  std::atomic_ref<T>(word).store(value, std::memory_order_seq_cst);
}

constexpr auto kSemaphorePollInterval = std::chrono::microseconds(50);
constexpr auto kFailedWaitPause = std::chrono::milliseconds(1);

// WHY: Waiting never sleeps or yields: a sleep costs a whole OS timer tick, a yield on a busy machine a whole
// quantum (both measured). Spin a few microseconds, then block on the wake-up object.

void spinHint() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#endif
}

class Backoff {
 public:
  bool spin() {
    if (round_ < kSpins) {
      spinHint();
      ++round_;
      return true;
    }
    return false;
  }

 private:
  static constexpr unsigned kSpins = 64;

  unsigned round_ = 0;
};

std::string inQuotes(std::string_view name) { return "\"" + std::string(name) + "\""; }

std::string hex64(uint64_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out += kDigits[(v >> shift) & 0xFu];
  }
  return out;
}

std::expected<void, std::string> checkName(std::string_view name) {
  if (name.empty()) {
    return std::unexpected("ring name must not be empty");
  }
  if (name.size() > kMaxNameLength) {
    return std::unexpected("ring name " + inQuotes(name) + " is " + std::to_string(name.size()) +
                           " characters long; at most " + std::to_string(kMaxNameLength) + " are allowed");
  }
  for (const char ch : name) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.';
    if (!ok) {
      return std::unexpected("ring name " + inQuotes(name) +
                             " has character other than A-Z, a-z, 0-9, '-', '_' and '.'");
    }
  }
  return {};
}

std::expected<uint64_t, std::string> mappingBytesFor(uint64_t slot_count, uint64_t slot_payload_bytes) {
  if (slot_count == 0) {
    return std::unexpected(std::string("ring needs at least 1 slot; slot count is 0"));
  }
  if (slot_payload_bytes == 0) {
    return std::unexpected(std::string("ring's slots need payload size above 0 bytes"));
  }
  constexpr uint64_t kLimit =
      (std::min)(uint64_t{std::numeric_limits<size_t>::max()}, std::numeric_limits<uint64_t>::max());
  const std::string overflow = std::to_string(slot_count) + " slots of " + std::to_string(slot_payload_bytes) +
                               " payload bytes do not fit in address space";
  if (slot_payload_bytes > kLimit - kPayloadAlignment - layout::kSlotFixedBytes) {
    return std::unexpected(overflow);
  }
  const uint64_t stride = layout::slotStride(slot_payload_bytes);
  if (stride > (kLimit - sizeof(layout::ControlBlock)) / slot_count) {
    return std::unexpected(overflow);
  }
  return sizeof(layout::ControlBlock) + slot_count * stride;
}

uint64_t steadyNowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Mapping {
  uint8_t* base = nullptr;
  size_t bytes = 0;
  void* handle = nullptr;
};

#if defined(_WIN32)

std::string systemMessage(DWORD code) {
  std::string text = std::system_category().message(static_cast<int>(code));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  return text + " (error " + std::to_string(code) + ")";
}

std::wstring objectName(std::string_view name) { return L"Local\\" + std::wstring(name.begin(), name.end()); }

std::expected<Mapping, std::string> createMapping(std::string_view name, uint64_t bytes) {
  const std::wstring object = objectName(name);
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xFFFFFFFFu),
                                      object.c_str());
  if (section == nullptr) {
    const DWORD error = GetLastError();
    return std::unexpected("cannot create ring " + inQuotes(name) + " of " + std::to_string(bytes) +
                           " bytes: " + systemMessage(error));
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return std::unexpected("cannot create ring " + inQuotes(name) + ": mapping of that name already exists");
  }
  void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
  if (view == nullptr) {
    const DWORD error = GetLastError();
    CloseHandle(section);
    return std::unexpected("cannot map ring " + inQuotes(name) + " of " + std::to_string(bytes) +
                           " bytes: " + systemMessage(error));
  }
  return Mapping{static_cast<uint8_t*>(view), static_cast<size_t>(bytes), section};
}

std::expected<Mapping, std::string> openMapping(std::string_view name) {
  const std::wstring object = objectName(name);
  HANDLE section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, object.c_str());
  if (section == nullptr) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return std::unexpected("there is no ring named " + inQuotes(name));
    }
    return std::unexpected("cannot open ring " + inQuotes(name) + ": " + systemMessage(error));
  }
  // WHY: Size comes from the view: Windows has no documented call for a section's size, VirtualQuery gives it.
  void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (view == nullptr) {
    const DWORD error = GetLastError();
    CloseHandle(section);
    return std::unexpected("cannot map ring " + inQuotes(name) + ": " + systemMessage(error));
  }
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(view, &info, sizeof(info)) == 0) {
    const DWORD error = GetLastError();
    UnmapViewOfFile(view);
    CloseHandle(section);
    return std::unexpected("cannot size ring " + inQuotes(name) + ": " + systemMessage(error));
  }
  return Mapping{static_cast<uint8_t*>(view), static_cast<size_t>(info.RegionSize), section};
}

void closeMapping(uint8_t* base, [[maybe_unused]] size_t bytes, void* handle, [[maybe_unused]] std::string_view name,
                  [[maybe_unused]] bool owner) noexcept {
  UnmapViewOfFile(base);
  if (handle != nullptr) {
    CloseHandle(handle);
  }
}

std::wstring wakeObject(std::string_view wake_name) {
  return L"Local\\" + std::wstring(wake_name.begin(), wake_name.end());
}

std::expected<void*, std::string> createWake(std::string_view ring_name, std::string_view wake_name) {
  const std::wstring object = wakeObject(wake_name);
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, object.c_str());
  if (event == nullptr) {
    const DWORD error = GetLastError();
    return std::unexpected("cannot create wake-up event " + inQuotes(wake_name) + " of ring " +
                           inQuotes(ring_name) + ": " + systemMessage(error));
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(event);
    return std::unexpected("cannot create ring " + inQuotes(ring_name) + ": its wake-up event " +
                           inQuotes(wake_name) + " already exists");
  }
  return static_cast<void*>(event);
}

std::expected<void*, std::string> openWake(std::string_view ring_name, std::string_view wake_name) {
  const std::wstring object = wakeObject(wake_name);
  HANDLE event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, object.c_str());
  if (event == nullptr) {
    const DWORD error = GetLastError();
    return std::unexpected("ring " + inQuotes(ring_name) + ": cannot open its wake-up event " +
                           inQuotes(wake_name) + ": " + systemMessage(error));
  }
  return static_cast<void*>(event);
}

void closeWake(void* wake, [[maybe_unused]] std::string_view wake_name, [[maybe_unused]] bool owner) noexcept {
  CloseHandle(wake);
}

void signalWake(void* wake) noexcept { SetEvent(wake); }

enum class WakeWait { Woken, TimedOut, Failed };

WakeWait waitWake(void* wake, uint64_t timeout_ms) noexcept {
  switch (WaitForSingleObject(wake, static_cast<DWORD>(timeout_ms))) {
    case WAIT_OBJECT_0:
      return WakeWait::Woken;
    case WAIT_TIMEOUT:
      return WakeWait::TimedOut;
    default:
      return WakeWait::Failed;
  }
}

#else

std::string errnoMessage(int error) {
  return std::generic_category().message(error) + " (errno " + std::to_string(error) + ")";
}

std::string objectName(std::string_view name) { return "/" + std::string(name); }

std::expected<Mapping, std::string> createMapping(std::string_view name, uint64_t bytes) {
  const std::string object = objectName(name);
  const int fd = ::shm_open(object.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    const int error = errno;
    if (error == EEXIST) {
      return std::unexpected("cannot create ring " + inQuotes(name) + ": mapping of that name already exists");
    }
    return std::unexpected("cannot create ring " + inQuotes(name) + ": shm_open: " + errnoMessage(error));
  }
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    const int error = errno;
    ::close(fd);
    ::shm_unlink(object.c_str());
    return std::unexpected("cannot size ring " + inQuotes(name) + " to " + std::to_string(bytes) +
                           " bytes: " + errnoMessage(error));
  }
  void* view = ::mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  const int error = errno;
  ::close(fd);
  if (view == MAP_FAILED) {
    ::shm_unlink(object.c_str());
    return std::unexpected("cannot map ring " + inQuotes(name) + ": " + errnoMessage(error));
  }
  return Mapping{static_cast<uint8_t*>(view), static_cast<size_t>(bytes), nullptr};
}

std::expected<Mapping, std::string> openMapping(std::string_view name) {
  const std::string object = objectName(name);
  const int fd = ::shm_open(object.c_str(), O_RDWR, 0);
  if (fd < 0) {
    const int error = errno;
    if (error == ENOENT) {
      return std::unexpected("there is no ring named " + inQuotes(name));
    }
    return std::unexpected("cannot open ring " + inQuotes(name) + ": shm_open: " + errnoMessage(error));
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int error = errno;
    ::close(fd);
    return std::unexpected("cannot size ring " + inQuotes(name) + ": " + errnoMessage(error));
  }
  if (st.st_size < static_cast<off_t>(sizeof(layout::ControlBlock))) {
    ::close(fd);
    return std::unexpected("ring " + inQuotes(name) + ": mapping holds " + std::to_string(st.st_size) +
                           " bytes, less than " + std::to_string(sizeof(layout::ControlBlock)) +
                           "-byte control block");
  }
  const size_t bytes = static_cast<size_t>(st.st_size);
  void* view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  const int error = errno;
  ::close(fd);
  if (view == MAP_FAILED) {
    return std::unexpected("cannot map ring " + inQuotes(name) + ": " + errnoMessage(error));
  }
  return Mapping{static_cast<uint8_t*>(view), bytes, nullptr};
}

void closeMapping(uint8_t* base, size_t bytes, [[maybe_unused]] void* handle, std::string_view name,
                  bool owner) noexcept {
  ::munmap(base, bytes);
  if (owner) {
    ::shm_unlink(objectName(name).c_str());
  }
}

struct SemPath {
  char text[kMaxNameLength + 2] = {};
};

// WHY: Built without allocating: closeWake() is noexcept and uses it.
SemPath semPath(std::string_view wake_name) {
  SemPath path;
  path.text[0] = '/';
  const size_t n = (std::min)(wake_name.size(), kMaxNameLength);
  std::memcpy(path.text + 1, wake_name.data(), n);
  return path;
}

std::expected<void*, std::string> createWake(std::string_view ring_name, std::string_view wake_name) {
  const SemPath path = semPath(wake_name);
  sem_t* sem = ::sem_open(path.text, O_CREAT | O_EXCL, 0600, 0u);
  if (sem == SEM_FAILED) {
    const int error = errno;
    if (error == EEXIST) {
      return std::unexpected("cannot create ring " + inQuotes(ring_name) + ": its wake-up semaphore " +
                             inQuotes(wake_name) + " already exists");
    }
    return std::unexpected("cannot create wake-up semaphore " + inQuotes(wake_name) + " of ring " +
                           inQuotes(ring_name) + ": sem_open: " + errnoMessage(error));
  }
  return static_cast<void*>(sem);
}

std::expected<void*, std::string> openWake(std::string_view ring_name, std::string_view wake_name) {
  const SemPath path = semPath(wake_name);
  sem_t* sem = ::sem_open(path.text, 0);
  if (sem == SEM_FAILED) {
    const int error = errno;
    return std::unexpected("ring " + inQuotes(ring_name) + ": cannot open its wake-up semaphore " +
                           inQuotes(wake_name) + ": sem_open: " + errnoMessage(error));
  }
  return static_cast<void*>(sem);
}

void closeWake(void* wake, std::string_view wake_name, bool owner) noexcept {
  ::sem_close(static_cast<sem_t*>(wake));
  if (owner) {
    ::sem_unlink(semPath(wake_name).text);
  }
}

void signalWake(void* wake) noexcept { ::sem_post(static_cast<sem_t*>(wake)); }

enum class WakeWait { Woken, TimedOut, Failed };

WakeWait waitWake(void* wake, uint64_t timeout_ms) noexcept {
  sem_t* sem = static_cast<sem_t*>(wake);
#if defined(__APPLE__)
  // WHY: macOS has no sem_timedwait: poll sem_trywait every 50 us until the timeout.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (::sem_trywait(sem) == 0) {
      return WakeWait::Woken;
    }
    if (errno != EAGAIN && errno != EINTR) {
      return WakeWait::Failed;
    }
    if (std::chrono::steady_clock::now() >= until) {
      return WakeWait::TimedOut;
    }
    std::this_thread::sleep_for(kSemaphorePollInterval);
  }
#else
  // WHY: sem_timedwait's deadline is on CLOCK_REALTIME: a wall-clock step lengthens or shortens one wait.
  timespec until{};
  if (::clock_gettime(CLOCK_REALTIME, &until) != 0) {
    return WakeWait::Failed;
  }
  until.tv_sec += static_cast<time_t>(timeout_ms / 1000);
  until.tv_nsec += static_cast<long>((timeout_ms % 1000) * 1'000'000);
  if (until.tv_nsec >= 1'000'000'000) {
    until.tv_sec += 1;
    until.tv_nsec -= 1'000'000'000;
  }
  for (;;) {
    if (::sem_timedwait(sem, &until) == 0) {
      return WakeWait::Woken;
    }
    if (errno != EINTR) {
      return errno == ETIMEDOUT ? WakeWait::TimedOut : WakeWait::Failed;
    }
  }
#endif
}

#endif

uint64_t mix64(uint64_t x) {
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

}

std::string makeRingName() {
  static std::atomic<uint64_t> counter{0};
  uint64_t value = 0;
  try {
    std::random_device device;
    value = (uint64_t{device()} << 32) ^ uint64_t{device()};
  } catch (...) {
  }
  // WHY: Mix in the clock and a per-process counter: some standard libraries ship a deterministic random_device.
  const auto ticks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  value ^= mix64(ticks + 0x9E3779B97F4A7C15ull * (counter.fetch_add(1, std::memory_order_relaxed) + 1));
  return "bts-" + hex64(value).substr(2);
}

namespace {

std::string wakeDigits(std::string_view ring_name) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const char ch : ring_name) {
    hash = (hash ^ static_cast<unsigned char>(ch)) * 0x100000001B3ull;
  }
  return hex64(mix64(hash)).substr(2);
}

}

std::string wakeObjectName(std::string_view ring_name) { return "btw-" + wakeDigits(ring_name); }

std::string producerWakeObjectName(std::string_view ring_name) { return "btp-" + wakeDigits(ring_name); }

std::expected<BulkRing, std::string> BulkRing::create(std::string_view name, uint32_t slot_count,
                                                      uint64_t slot_payload_bytes, uint64_t capture_id,
                                                      Timing timing, uint32_t flush_after_ms,
                                                      CreatorRole creator_role) {
  if (auto ok = checkName(name); !ok) {
    return std::unexpected(ok.error());
  }
  const auto bytes = mappingBytesFor(slot_count, slot_payload_bytes);
  if (!bytes) {
    return std::unexpected("cannot create ring " + inQuotes(name) + ": " + bytes.error());
  }
  auto mapping = createMapping(name, *bytes);
  if (!mapping) {
    return std::unexpected(mapping.error());
  }
  BulkRing ring(std::string(name), mapping->base, mapping->bytes, mapping->handle, true, std::move(timing));
  ring.slot_count_ = slot_count;
  ring.slot_payload_bytes_ = slot_payload_bytes;
  ring.capture_id_ = capture_id;
  ring.flush_after_ms_ = flush_after_ms;
  ring.creator_role_ = creator_role;
  // WHY: Create the wake-up object before writing the magic: a plugin that finds a complete mapping must find it too.
  ring.wake_name_ = wakeObjectName(name);
  auto wake = createWake(name, ring.wake_name_);
  if (!wake) {
    return std::unexpected(wake.error());
  }
  ring.wake_ = *wake;
  ring.producer_wake_name_ = producerWakeObjectName(name);
  auto producer_wake = createWake(name, ring.producer_wake_name_);
  if (!producer_wake) {
    return std::unexpected(producer_wake.error());
  }
  ring.producer_wake_ = *producer_wake;

  layout::ControlBlock& c = ring.control();
  std::memset(&c, 0, sizeof(c));
  c.info.layout_version = kLayoutVersion;
  c.info.slot_count = slot_count;
  c.info.slot_payload_bytes = slot_payload_bytes;
  c.info.capture_id = capture_id;
  c.info.flush_after_ms = flush_after_ms;
  storeRelease(c.producer.producer_state, static_cast<uint32_t>(ProducerState::Attached));
  storeRelease(c.consumer.consumer_state, static_cast<uint32_t>(ConsumerState::Reading));
  storeRelease(c.consumer.consumer_heartbeat_ms, ring.nowMs());
  // WHY: Write the magic last: a mapping whose magic is right is completely written.
  storeRelease(c.info.magic, kMagic);
  return ring;
}

std::expected<BulkRing, std::string> BulkRing::open(std::string_view name, Timing timing) {
  if (auto ok = checkName(name); !ok) {
    return std::unexpected(ok.error());
  }
  auto mapping = openMapping(name);
  if (!mapping) {
    return std::unexpected(mapping.error());
  }
  BulkRing ring(std::string(name), mapping->base, mapping->bytes, mapping->handle, false, std::move(timing));
  const std::string where = "ring " + inQuotes(name) + ": ";
  if (ring.mapping_bytes_ < sizeof(layout::ControlBlock)) {
    return std::unexpected(where + "mapping holds " + std::to_string(ring.mapping_bytes_) +
                           " bytes, less than " + std::to_string(sizeof(layout::ControlBlock)) +
                           "-byte control block");
  }
  layout::ControlBlock& c = ring.control();
  const uint64_t magic = loadAcquire(c.info.magic);
  if (magic != kMagic) {
    return std::unexpected(where + "wrong magic " + hex64(magic) + " (bulk ring has " + hex64(kMagic) +
                           "): not bitoscope bulk ring, or not completely created");
  }
  const uint32_t version = c.info.layout_version;
  if (version != kLayoutVersion) {
    return std::unexpected(where + "layout version " + std::to_string(version) +
                           ", but this build reads layout version " + std::to_string(kLayoutVersion));
  }
  const uint32_t slot_count = c.info.slot_count;
  const uint64_t slot_payload_bytes = c.info.slot_payload_bytes;
  const auto needed = mappingBytesFor(slot_count, slot_payload_bytes);
  if (!needed) {
    return std::unexpected(where + "its header is not ring: " + needed.error());
  }
  if (*needed > ring.mapping_bytes_) {
    return std::unexpected(where + "mapping holds " + std::to_string(ring.mapping_bytes_) +
                           " bytes, but its header describes " + std::to_string(slot_count) + " slots of " +
                           std::to_string(slot_payload_bytes) + " payload bytes, which need " +
                           std::to_string(*needed) + " bytes");
  }
  ring.slot_count_ = slot_count;
  ring.slot_payload_bytes_ = slot_payload_bytes;
  ring.capture_id_ = c.info.capture_id;
  ring.flush_after_ms_ = c.info.flush_after_ms;
  ring.wake_name_ = wakeObjectName(name);
  auto wake = openWake(name, ring.wake_name_);
  if (!wake) {
    return std::unexpected(wake.error());
  }
  ring.wake_ = *wake;
  ring.producer_wake_name_ = producerWakeObjectName(name);
  auto producer_wake = openWake(name, ring.producer_wake_name_);
  if (!producer_wake) {
    return std::unexpected(producer_wake.error());
  }
  ring.producer_wake_ = *producer_wake;
  return ring;
}

BulkRing::BulkRing(std::string name, uint8_t* base, size_t mapping_bytes, void* native_handle, bool owner,
                   Timing timing)
    : name_(std::move(name)),
      base_(base),
      mapping_bytes_(mapping_bytes),
      native_handle_(native_handle),
      owner_(owner),
      timing_(std::move(timing)) {}

BulkRing::BulkRing(BulkRing&& other) noexcept
    : name_(std::move(other.name_)),
      base_(std::exchange(other.base_, nullptr)),
      mapping_bytes_(std::exchange(other.mapping_bytes_, 0)),
      native_handle_(std::exchange(other.native_handle_, nullptr)),
      wake_(std::exchange(other.wake_, nullptr)),
      wake_name_(std::move(other.wake_name_)),
      producer_wake_(std::exchange(other.producer_wake_, nullptr)),
      producer_wake_name_(std::move(other.producer_wake_name_)),
      owner_(std::exchange(other.owner_, false)),
      creator_role_(other.creator_role_),
      timing_(std::move(other.timing_)),
      slot_count_(other.slot_count_),
      slot_payload_bytes_(other.slot_payload_bytes_),
      capture_id_(other.capture_id_),
      flush_after_ms_(other.flush_after_ms_),
      filling_(std::exchange(other.filling_, Filling{})),
      reading_(std::exchange(other.reading_, Reading{})),
      consumer_waits_(std::exchange(other.consumer_waits_, WaitCounters{})),
      producer_waits_(std::exchange(other.producer_waits_, WaitCounters{})) {}

BulkRing& BulkRing::operator=(BulkRing&& other) noexcept {
  if (this != &other) {
    releaseMapping();
    name_ = std::move(other.name_);
    base_ = std::exchange(other.base_, nullptr);
    mapping_bytes_ = std::exchange(other.mapping_bytes_, 0);
    native_handle_ = std::exchange(other.native_handle_, nullptr);
    wake_ = std::exchange(other.wake_, nullptr);
    wake_name_ = std::move(other.wake_name_);
    producer_wake_ = std::exchange(other.producer_wake_, nullptr);
    producer_wake_name_ = std::move(other.producer_wake_name_);
    owner_ = std::exchange(other.owner_, false);
    creator_role_ = other.creator_role_;
    timing_ = std::move(other.timing_);
    slot_count_ = other.slot_count_;
    slot_payload_bytes_ = other.slot_payload_bytes_;
    capture_id_ = other.capture_id_;
    flush_after_ms_ = other.flush_after_ms_;
    filling_ = std::exchange(other.filling_, Filling{});
    reading_ = std::exchange(other.reading_, Reading{});
    consumer_waits_ = std::exchange(other.consumer_waits_, WaitCounters{});
    producer_waits_ = std::exchange(other.producer_waits_, WaitCounters{});
  }
  return *this;
}

BulkRing::~BulkRing() { releaseMapping(); }

void BulkRing::releaseMapping() noexcept {
  if (base_ == nullptr) {
    return;
  }
  if (owner_ && creator_role_ == CreatorRole::Consumer) {
    close();
  } else if (owner_) {
    abandonProduction();
  }
  closeMapping(base_, mapping_bytes_, native_handle_, name_, owner_);
  if (wake_ != nullptr) {
    closeWake(wake_, wake_name_, owner_);
  }
  if (producer_wake_ != nullptr) {
    closeWake(producer_wake_, producer_wake_name_, owner_);
  }
  base_ = nullptr;
  native_handle_ = nullptr;
  wake_ = nullptr;
  producer_wake_ = nullptr;
}

void BulkRing::abandonProduction() noexcept {
  if (producerState() != ProducerState::Attached) {
    return;
  }
  filling_.held = false;
  commitSlot();
  try {
    (void)markFailed(endOfLastCommit(), 0, "producer went away without ending stream");
  } catch (...) {
  }
}

uint64_t BulkRing::endOfLastCommit() const {
  const uint64_t produced = loadRelaxed(control().producer.produced);
  if (produced == 0) {
    return 0;
  }
  const uint64_t slot = produced - 1;
  const uint32_t count = (std::min)(slotHeader(slot).chunk_count, layout::kMaxChunksPerSlot);
  const uint64_t area = layout::chunkAreaBytes(slot_payload_bytes_);
  uint64_t offset = 0;
  uint64_t end = 0;
  for (uint32_t i = 0; i < count && offset + sizeof(layout::ChunkRecord) <= area; ++i) {
    const auto& record = *reinterpret_cast<const layout::ChunkRecord*>(chunkArea(slot) + offset);
    end = record.first_sample_index + record.num_samples;
    offset += layout::chunkBytes((std::min)(record.payload_bytes, slot_payload_bytes_));
  }
  return end;
}

layout::ControlBlock& BulkRing::control() const { return *reinterpret_cast<layout::ControlBlock*>(base_); }

layout::SlotHeader& BulkRing::slotHeader(uint64_t index) const {
  return *reinterpret_cast<layout::SlotHeader*>(base_ + layout::slotOffset(slot_payload_bytes_, index % slot_count_));
}

uint8_t* BulkRing::chunkArea(uint64_t index) const {
  return base_ + layout::chunkAreaOffset(slot_payload_bytes_, index % slot_count_);
}

uint64_t BulkRing::nowMs() const { return timing_.now_ms ? timing_.now_ms() : steadyNowMs(); }

uint64_t BulkRing::produced() const { return loadAcquire(control().producer.produced); }

uint64_t BulkRing::consumed() const { return loadAcquire(control().consumer.consumed); }

ProducerState BulkRing::producerState() const {
  return static_cast<ProducerState>(loadAcquire(control().producer.producer_state));
}

ConsumerState BulkRing::consumerState() const {
  return static_cast<ConsumerState>(loadAcquire(control().consumer.consumer_state));
}

uint64_t BulkRing::consumerHeartbeatMs() const { return loadAcquire(control().consumer.consumer_heartbeat_ms); }

FailedRecord BulkRing::failedRecord() const {
  layout::ProducerLine& p = control().producer;
  // WHY: Write the failure record before release-storing Failed, never after: a reader trusts it once it sees Failed.
  if (loadAcquire(p.producer_state) != static_cast<uint32_t>(ProducerState::Failed)) {
    return {};
  }
  const char* reason = p.reason;
  const char* reason_end = std::find(reason, reason + kReasonBytes, '\0');
  return FailedRecord{p.failed_at_sample, p.lost_samples, std::string(reason, reason_end),
                      static_cast<FailureKind>(p.failure_kind)};
}

AcquireError BulkRing::finishedError() const {
  const bool failed =
      loadRelaxed(control().producer.producer_state) == static_cast<uint32_t>(ProducerState::Failed);
  return AcquireError{AcquireErrorKind::ProducerFinished,
                      failed ? "producer has already recorded failure on ring " + inQuotes(name_)
                             : "producer has already ended stream on ring " + inQuotes(name_)};
}

bool BulkRing::fits(uint64_t max_bytes) const {
  return filling_.chunks < layout::kMaxChunksPerSlot &&
         filling_.payload_bytes + layout::padded(max_bytes) <= layout::padded(slot_payload_bytes_);
}

std::expected<std::span<uint8_t>, AcquireError> BulkRing::tryAcquire(uint64_t max_bytes) {
  if (max_bytes > slot_payload_bytes_) {
    return std::unexpected(AcquireError{
        AcquireErrorKind::TooLarge, "chunk of " + std::to_string(max_bytes) + " bytes does not fit ring " +
                                        inQuotes(name_) + "'s slots of " + std::to_string(slot_payload_bytes_) +
                                        " bytes; chunk never spans two slots"});
  }
  layout::ControlBlock& c = control();
  if (loadRelaxed(c.producer.producer_state) != static_cast<uint32_t>(ProducerState::Attached)) {
    return std::unexpected(finishedError());
  }
  if (loadAcquire(c.consumer.consumer_state) == static_cast<uint32_t>(ConsumerState::Closed)) {
    return std::unexpected(
        AcquireError{AcquireErrorKind::ConsumerClosed, "consumer has closed ring " + inQuotes(name_)});
  }
  filling_.held = false;
  if (filling_.open && !fits(max_bytes)) {
    commitSlot();
  }
  const uint64_t produced = loadRelaxed(c.producer.produced);
  if (!filling_.open) {
    const uint64_t consumed = loadAcquire(c.consumer.consumed);
    if (consumed > produced) {
      return std::unexpected(AcquireError{
          AcquireErrorKind::ConsumerInvalid,
          "consumer of ring " + inQuotes(name_) + " broke ring: its count " + std::to_string(consumed) +
              " is ahead of producer's " + std::to_string(produced)});
    }
    if (consumed < filling_.seen_consumed) {
      return std::unexpected(AcquireError{
          AcquireErrorKind::ConsumerInvalid,
          "consumer of ring " + inQuotes(name_) + " broke ring: its count moved back from " +
              std::to_string(filling_.seen_consumed) + " to " + std::to_string(consumed)});
    }
    filling_.seen_consumed = consumed;
    if (produced - consumed >= slot_count_) {
      return std::unexpected(AcquireError{
          AcquireErrorKind::Full, "ring " + inQuotes(name_) + " is full: all " + std::to_string(slot_count_) +
                                      " slots hold chunks consumer has not released"});
    }
    filling_.open = true;
    filling_.chunks = 0;
    filling_.payload_bytes = 0;
  }
  filling_.held = true;
  filling_.reserved_bytes = max_bytes;
  const uint64_t at = uint64_t{filling_.chunks} * sizeof(layout::ChunkRecord) + filling_.payload_bytes;
  return std::span<uint8_t>(chunkArea(produced) + at + sizeof(layout::ChunkRecord), static_cast<size_t>(max_bytes));
}

std::expected<std::span<uint8_t>, AcquireError> BulkRing::acquire(uint64_t max_bytes, uint64_t deadline_ms) {
  Backoff backoff;
  for (;;) {
    auto slot = tryAcquire(max_bytes);
    if (slot || slot.error().kind != AcquireErrorKind::Full) {
      return slot;
    }
    const uint64_t now = nowMs();
    const uint64_t beat = consumerHeartbeatMs();
    if (now > beat && now - beat > timing_.stale_after_ms) {
      return std::unexpected(AcquireError{
          AcquireErrorKind::ConsumerStale, "consumer of ring " + inQuotes(name_) + " last showed heartbeat " +
                                               std::to_string(now - beat) + " ms ago (limit is " +
                                               std::to_string(timing_.stale_after_ms) + " ms)"});
    }
    if (now >= deadline_ms) {
      return std::unexpected(AcquireError{AcquireErrorKind::Timeout,
                                          "ring " + inQuotes(name_) + " stayed full until deadline"});
    }
    if (!backoff.spin()) {
      blockUntilRoom((std::min)(deadline_ms - now, kMaxWakeWaitMs));
    }
  }
}

void BulkRing::blockUntilRoom(uint64_t timeout_ms) {
  layout::ControlBlock& c = control();
  if (producer_wake_ == nullptr) {
    std::this_thread::sleep_for(kFailedWaitPause);
    return;
  }
  storeSeqCst(c.producer.producer_waiting, uint32_t{1});
  const auto full = [&] {
    return loadRelaxed(c.producer.produced) - loadSeqCst(c.consumer.consumed) >= slot_count_ &&
           loadSeqCst(c.consumer.consumer_state) == static_cast<uint32_t>(ConsumerState::Reading);
  };
  if (full()) {
    ++producer_waits_.blocks;
    const WakeWait woke = waitWake(producer_wake_, timeout_ms);
    if (woke == WakeWait::TimedOut) {
      ++producer_waits_.timeouts;
      producer_waits_.lost += full() ? 0 : 1;
    } else if (woke == WakeWait::Failed) {
      std::this_thread::sleep_for(kFailedWaitPause);
    }
  }
  storeRelease(c.producer.producer_waiting, uint32_t{0});
}

std::expected<void, std::string> BulkRing::commit(const ChunkHeader& header) {
  if (!filling_.held) {
    return std::unexpected("commit on ring " + inQuotes(name_) + " without room from tryAcquire() or acquire()");
  }
  if (header.payload_bytes > filling_.reserved_bytes) {
    return std::unexpected("chunk of " + std::to_string(header.payload_bytes) + " bytes is committed on ring " +
                           inQuotes(name_) + ", but only " + std::to_string(filling_.reserved_bytes) +
                           " bytes were reserved for it");
  }
  layout::ControlBlock& c = control();
  const uint64_t produced = loadRelaxed(c.producer.produced);
  const uint64_t at = uint64_t{filling_.chunks} * sizeof(layout::ChunkRecord) + filling_.payload_bytes;
  layout::ChunkRecord& record = *reinterpret_cast<layout::ChunkRecord*>(chunkArea(produced) + at);
  std::memset(&record, 0, sizeof(record));
  record.first_sample_index = header.first_sample_index;
  record.num_samples = header.num_samples;
  record.num_channels = header.num_channels;
  record.encoding = header.encoding;
  record.sample_rate_milli_hz = header.sample_rate_milli_hz;
  record.capture_id = header.capture_id;
  record.payload_bytes = header.payload_bytes;
  filling_.chunks += 1;
  filling_.payload_bytes += layout::padded(header.payload_bytes);
  layout::SlotHeader& slot = slotHeader(produced);
  slot.chunk_count = filling_.chunks;
  slot.used_bytes = at + layout::chunkBytes(header.payload_bytes);
  filling_.held = false;
  const uint64_t now = nowMs();
  if (filling_.chunks == 1) {
    filling_.first_ms = now;
  }
  const bool idle_before = !filling_.committed_any ||
                           (now >= filling_.last_commit_ms && now - filling_.last_commit_ms >= flush_after_ms_);
  filling_.committed_any = true;
  filling_.last_commit_ms = now;
  const bool slot_old = now >= filling_.first_ms && now - filling_.first_ms >= flush_after_ms_;
  if (flush_after_ms_ == 0 || !fits(filling_.reserved_bytes) || slot_old || idle_before) {
    commitSlot();
  }
  return {};
}

void BulkRing::commitSlot() {
  if (!filling_.open || filling_.chunks == 0) {
    return;
  }
  layout::ControlBlock& c = control();
  const uint64_t produced = loadRelaxed(c.producer.produced);
  storeSeqCst(c.producer.produced, produced + 1);
  filling_.open = false;
  filling_.chunks = 0;
  filling_.payload_bytes = 0;
  wakeConsumer();
}

void BulkRing::flush() {
  if (!filling_.held) {
    commitSlot();
  }
}

bool BulkRing::flushIfDue() {
  if (!filling_.open || filling_.chunks == 0 || filling_.held) {
    return false;
  }
  const uint64_t now = nowMs();
  if (now < filling_.first_ms || now - filling_.first_ms < flush_after_ms_) {
    return false;
  }
  commitSlot();
  return true;
}

std::expected<void, std::string> BulkRing::end() {
  layout::ControlBlock& c = control();
  if (loadRelaxed(c.producer.producer_state) != static_cast<uint32_t>(ProducerState::Attached)) {
    return std::unexpected(finishedError().message);
  }
  filling_.held = false;
  commitSlot();
  storeSeqCst(c.producer.producer_state, static_cast<uint32_t>(ProducerState::Ended));
  wakeConsumer();
  return {};
}

std::expected<void, std::string> BulkRing::markFailed(uint64_t at_sample, uint64_t lost_samples,
                                                      std::string_view reason, FailureKind kind) {
  layout::ControlBlock& c = control();
  if (loadRelaxed(c.producer.producer_state) != static_cast<uint32_t>(ProducerState::Attached)) {
    return std::unexpected(finishedError().message);
  }
  filling_.held = false;
  commitSlot();
  layout::ProducerLine& p = c.producer;
  p.failed_at_sample = at_sample;
  p.lost_samples = lost_samples;
  p.failure_kind = static_cast<uint32_t>(kind);
  const size_t length = (std::min)(reason.size(), kReasonBytes - 1);
  for (size_t i = 0; i < length; ++i) {
    const auto ch = static_cast<unsigned char>(reason[i]);
    p.reason[i] = ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?';
  }
  std::memset(p.reason + length, 0, kReasonBytes - length);
  storeSeqCst(p.producer_state, static_cast<uint32_t>(ProducerState::Failed));
  wakeConsumer();
  return {};
}

void BulkRing::wakeConsumer() {
  if (wake_ != nullptr && loadSeqCst(control().consumer.consumer_waiting) != 0) {
    signalWake(wake_);
  }
}

Poll BulkRing::peek() {
  layout::ControlBlock& c = control();
  // WHY: Load producer state BEFORE produced: Ended or Failed is stored after the last commit, so produced then
  // counts every commit.
  const uint32_t state = loadAcquire(c.producer.producer_state);
  const uint64_t produced = loadAcquire(c.producer.produced);
  const uint64_t consumed = loadRelaxed(c.consumer.consumed);

  Poll poll;
  const auto invalid = [&poll, this](std::string what) {
    poll.status = PollStatus::Invalid;
    poll.error = "ring " + inQuotes(name_) + ": " + std::move(what);
    return poll;
  };
  if (produced < consumed) {
    return invalid("producer's count " + std::to_string(produced) + " is behind consumer's " +
                   std::to_string(consumed));
  }
  if (produced - consumed > slot_count_) {
    return invalid("producer's count " + std::to_string(produced) + " is more than ring's " +
                   std::to_string(slot_count_) + " slots ahead of consumer's " + std::to_string(consumed));
  }
  const uint64_t reading = consumed + reading_.ahead;
  if (produced != reading) {
    // WHY: Copy the slot header out once and validate the copy: a producer rewriting the slot must not move a payload
    // span outside it.
    const layout::SlotHeader& slot = slotHeader(reading);
    const uint32_t chunk_count = slot.chunk_count;
    const uint64_t used_bytes = slot.used_bytes;
    const std::string which = "slot " + std::to_string(reading);
    if (chunk_count == 0 || chunk_count > layout::kMaxChunksPerSlot) {
      return invalid(which + " was committed with " + std::to_string(chunk_count) + " chunks; slot holds 1 to " +
                     std::to_string(layout::kMaxChunksPerSlot));
    }
    if (used_bytes > layout::chunkAreaBytes(slot_payload_bytes_)) {
      return invalid(which + " says its chunks take " + std::to_string(used_bytes) + " bytes, but slot holds " +
                     std::to_string(layout::chunkAreaBytes(slot_payload_bytes_)));
    }
    if (reading_.chunks >= chunk_count) {
      return invalid(which + " says it holds " + std::to_string(chunk_count) + " chunks, but " +
                     std::to_string(reading_.chunks) + " of them have been read");
    }
    const uint64_t at = reading_.offset;
    if (at + sizeof(layout::ChunkRecord) > used_bytes) {
      return invalid("chunk " + std::to_string(reading_.chunks) + " of " + which + " starts past " +
                     std::to_string(used_bytes) + " bytes its chunks take");
    }
    const layout::ChunkRecord& record = *reinterpret_cast<const layout::ChunkRecord*>(chunkArea(reading) + at);
    const ChunkHeader header{record.first_sample_index, record.num_samples, record.num_channels,
                             record.encoding,           record.sample_rate_milli_hz, record.capture_id,
                             record.payload_bytes};
    if (header.payload_bytes > slot_payload_bytes_) {
      return invalid("chunk " + std::to_string(reading_.chunks) + " of " + which + " claims payload of " +
                     std::to_string(header.payload_bytes) + " bytes, but chunk carries at most " +
                     std::to_string(slot_payload_bytes_));
    }
    const uint64_t end = at + layout::chunkBytes(header.payload_bytes);
    const bool last = reading_.chunks + 1 == chunk_count;
    if (end > used_bytes || (last && end != used_bytes)) {
      return invalid(which + "'s chunk " + std::to_string(reading_.chunks) + " of " + std::to_string(chunk_count) +
                     " ends at byte " + std::to_string(end) + ", but slot says its chunks take " +
                     std::to_string(used_bytes));
    }
    poll.status = PollStatus::Chunk;
    poll.chunk.header = header;
    poll.chunk.payload = std::span<const uint8_t>(chunkArea(reading) + at + sizeof(layout::ChunkRecord),
                                                  static_cast<size_t>(header.payload_bytes));
    reading_.peeked = true;
    reading_.peeked_end = end;
    reading_.peeked_last = last;
    return poll;
  }
  switch (static_cast<ProducerState>(state)) {
    case ProducerState::Attached:
      poll.status = PollStatus::Empty;
      return poll;
    case ProducerState::Ended:
      poll.status = PollStatus::Ended;
      return poll;
    case ProducerState::Failed:
      poll.status = PollStatus::Failed;
      return poll;
  }
  return invalid("producer state " + std::to_string(state) + " is not one this build knows");
}

Poll BulkRing::wait(uint64_t deadline_ms) {
  Backoff backoff;
  for (;;) {
    Poll poll = peek();
    if (poll.status != PollStatus::Empty) {
      return poll;
    }
    heartbeat();
    const uint64_t now = nowMs();
    if (now >= deadline_ms) {
      return poll;
    }
    if (!backoff.spin()) {
      blockUntilWoken((std::min)(deadline_ms - now, kMaxWakeWaitMs));
    }
  }
}

void BulkRing::blockUntilWoken(uint64_t timeout_ms) {
  layout::ControlBlock& c = control();
  if (wake_ == nullptr) {
    std::this_thread::sleep_for(kFailedWaitPause);
    return;
  }
  storeSeqCst(c.consumer.consumer_waiting, uint32_t{1});
  const auto idle = [&] {
    return loadSeqCst(c.producer.produced) == loadRelaxed(c.consumer.consumed) + reading_.ahead &&
           loadSeqCst(c.producer.producer_state) == static_cast<uint32_t>(ProducerState::Attached);
  };
  if (idle()) {
    ++consumer_waits_.blocks;
    const WakeWait woke = waitWake(wake_, timeout_ms);
    if (woke == WakeWait::TimedOut) {
      ++consumer_waits_.timeouts;
      consumer_waits_.lost += idle() ? 0 : 1;
    } else if (woke == WakeWait::Failed) {
      std::this_thread::sleep_for(kFailedWaitPause);
    }
  }
  storeRelease(c.consumer.consumer_waiting, uint32_t{0});
}

std::expected<void, std::string> BulkRing::release() {
  if (!reading_.peeked) {
    return std::unexpected("release on ring " + inQuotes(name_) + " without chunk from peek()");
  }
  reading_.peeked = false;
  reading_.chunks += 1;
  reading_.offset = reading_.peeked_end;
  if (reading_.peeked_last) {
    layout::ControlBlock& c = control();
    const uint64_t consumed = loadRelaxed(c.consumer.consumed);
    reading_.chunks = 0;
    reading_.offset = 0;
    storeSeqCst(c.consumer.consumed, consumed + reading_.ahead + 1);
    reading_.ahead = 0;
    wakeProducer();
  } else if (reading_.ahead != 0) {
    layout::ControlBlock& c = control();
    storeSeqCst(c.consumer.consumed, loadRelaxed(c.consumer.consumed) + reading_.ahead);
    reading_.ahead = 0;
    wakeProducer();
  }
  return {};
}

void BulkRing::wakeProducer() {
  if (producer_wake_ != nullptr && loadSeqCst(control().producer.producer_waiting) != 0) {
    signalWake(producer_wake_);
  }
}

std::expected<void, std::string> BulkRing::advance() {
  if (!reading_.peeked) {
    return std::unexpected("advance on ring " + inQuotes(name_) + " without chunk from peek()");
  }
  reading_.peeked = false;
  reading_.chunks += 1;
  reading_.offset = reading_.peeked_end;
  if (reading_.peeked_last) {
    reading_.chunks = 0;
    reading_.offset = 0;
    reading_.ahead += 1;
  }
  return {};
}

std::expected<void, std::string> BulkRing::releaseSlotsBefore(uint64_t slot) {
  layout::ControlBlock& c = control();
  const uint64_t consumed = loadRelaxed(c.consumer.consumed);
  if (slot <= consumed) {
    return {};
  }
  if (slot - consumed > reading_.ahead) {
    return std::unexpected("ring " + inQuotes(name_) + ": cannot hand back slots before slot " +
                           std::to_string(slot) + ": slot " + std::to_string(slot) +
                           " and ones before it have not been read past (consumer reads slot " +
                           std::to_string(consumed + reading_.ahead) + ")");
  }
  reading_.ahead -= slot - consumed;
  storeSeqCst(c.consumer.consumed, slot);
  wakeProducer();
  return {};
}

uint64_t BulkRing::readingSlot() const { return loadRelaxed(control().consumer.consumed) + reading_.ahead; }

void BulkRing::close() {
  storeSeqCst(control().consumer.consumer_state, static_cast<uint32_t>(ConsumerState::Closed));
  wakeProducer();
}

void BulkRing::heartbeat() { storeRelease(control().consumer.consumer_heartbeat_ms, nowMs()); }

}

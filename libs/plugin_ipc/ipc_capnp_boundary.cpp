/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_capnp_boundary.h"

#include <cstring>
#include <exception>
#include <new>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/exception.h>
#include <kj/io.h>

namespace bitoscope::ipc {

std::string_view toString(DecodeErrorKind kind) {
  switch (kind) {
    case DecodeErrorKind::empty:
      return "empty";
    case DecodeErrorKind::notWholeWords:
      return "not whole words";
    case DecodeErrorKind::malformed:
      return "malformed";
    case DecodeErrorKind::trailingBytes:
      return "trailing bytes";
    case DecodeErrorKind::noMessage:
      return "no message";
    case DecodeErrorKind::unknownMember:
      return "unknown member";
    case DecodeErrorKind::missingField:
      return "missing field";
    case DecodeErrorKind::unknownEnum:
      return "unknown enum value";
  }
  return "unknown error kind";
}

}

namespace bitoscope::ipc::detail {

namespace {

// WHY: KJ stack traces stay off: a dbghelp stack walk costs 455 us per malformed frame on Windows
// against 14.8 us.
class NoStackTraceCallback final : public kj::ExceptionCallback {
 public:
  StackTraceMode stackTraceMode() override { return StackTraceMode::NONE; }
};

DecodeError malformed(std::string message) { return DecodeError{DecodeErrorKind::malformed, std::move(message), {}, 0}; }

// WHY: Segment size is measured, not derived: 64 KiB only halved page faults per job,
// 512 KiB cut them to about 350.
constexpr std::size_t kReusedSegmentWords = 65536;
static_assert(kReusedSegmentWords >= capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

struct ReusedSegment {
  // WHY: Reused first segment must be ZERO whenever no builder holds it:
  // Cap'n Proto requires that of a given first segment.
  kj::Array<capnp::word> words;
  bool busy = false;
};

thread_local ReusedSegment tReused;

}

std::optional<DecodeError> readMessage(std::span<const uint8_t> bytes, const MessageReadFn& read) {
  constexpr std::size_t kWord = sizeof(capnp::word);
  if (bytes.empty()) return DecodeError{DecodeErrorKind::empty, "message is empty", {}, 0};
  if (bytes.size() % kWord != 0) {
    return DecodeError{DecodeErrorKind::notWholeWords,
                       "message of " + std::to_string(bytes.size()) + " bytes is not whole number of 8-byte words",
                       {},
                       0};
  }

  NoStackTraceCallback noStackTraces;
  try {
    // WHY: Copy into a word-aligned buffer: the caller's bytes may sit at any address.
    const std::size_t words = bytes.size() / kWord;
    auto aligned = kj::heapArray<capnp::word>(words);
    std::memcpy(aligned.begin(), bytes.data(), bytes.size());

    capnp::ReaderOptions options;
    options.traversalLimitInWords = kTraversalWordsPerWord * words + kTraversalFloorWords;
    options.nestingLimit = kNestingLimit;
    capnp::FlatArrayMessageReader reader(aligned.asPtr(), options);
    if (reader.getEnd() != aligned.end()) {
      const auto used = static_cast<std::size_t>(reader.getEnd() - aligned.begin());
      return DecodeError{DecodeErrorKind::trailingBytes,
                         std::to_string((words - used) * kWord) + " bytes after end of message", {}, 0};
    }
    // WHY: Every access to the message must happen inside read, inside this try:
    // Cap'n Proto validates on access.
    return read(reader);
  } catch (const kj::Exception& e) {
    return malformed(std::string("malformed message: ") + e.getDescription().cStr());
  } catch (const std::bad_alloc&) {
    return malformed("out of memory while decoding message");
  } catch (const std::exception& e) {
    return malformed(std::string("message could not be decoded: ") + e.what());
  } catch (...) {
    return malformed("message could not be decoded (unknown exception)");
  }
}

std::expected<std::vector<uint8_t>, std::string> buildMessage(const MessageBuildFn& build) {
  NoStackTraceCallback noStackTraces;
  try {
    const auto flat = [](capnp::MessageBuilder& message) {
      std::vector<uint8_t> out(capnp::computeSerializedSizeInWords(message) * sizeof(capnp::word));
      kj::ArrayOutputStream stream(kj::arrayPtr(reinterpret_cast<kj::byte*>(out.data()), out.size()));
      capnp::writeMessage(stream, message);
      return out;
    };
    ReusedSegment& reused = tReused;
    if (reused.busy) {
      capnp::MallocMessageBuilder message;
      build(message);
      return flat(message);
    }
    if (reused.words == nullptr) {
      reused.words = kj::heapArray<capnp::word>(kReusedSegmentWords);
      std::memset(reused.words.begin(), 0, reused.words.size() * sizeof(capnp::word));
    }
    reused.busy = true;
    struct Release {
      bool& busy;
      ~Release() { busy = false; }
    } release{reused.busy};
    // WHY: The builder is destroyed before release: it zeroes the segment,
    // which must be zero before another builder takes it.
    capnp::MallocMessageBuilder message(reused.words.asPtr());
    build(message);
    return flat(message);
  } catch (const kj::Exception& e) {
    return std::unexpected(std::string("message could not be built: ") + e.getDescription().cStr());
  } catch (const std::bad_alloc&) {
    return std::unexpected(std::string("out of memory while building message"));
  } catch (const std::exception& e) {
    return std::unexpected(std::string("message could not be built: ") + e.what());
  } catch (...) {
    return std::unexpected(std::string("message could not be built (unknown exception)"));
  }
}

}

/*
 * SPDX-License-Identifier: MIT
 */

#include "linux_uevent_reader.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "device_watch.h"

namespace bitoscope::plugin_c_sdk {

namespace {

std::string systemError(std::string_view what, int errorNumber) {
  return std::string(what) + ": " + std::generic_category().message(errorNumber) + " (errno " +
         std::to_string(errorNumber) + ")";
}

}

void readUevents(int socketFd, int stopFd, const std::vector<UsbId>& ids, const std::function<void()>& changed,
                 const std::function<void(std::string_view why)>& failed) {
  std::vector<uint8_t> buffer(8192);
  for (;;) {
    pollfd fds[2] = {{socketFd, POLLIN, 0}, {stopFd, POLLIN, 0}};
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      const int error = errno;  // WHY: read errno at once, before anything else can change it
      if (error == EINTR) continue;
      failed(systemError("device event socket could not be waited on", error));
      return;
    }
    if (fds[1].revents & POLLIN) return;
    // WHY: Recv on ANY event: POLLERR/POLLNVAL come without POLLIN, and reading only on POLLIN spins forever.
    if (fds[0].revents == 0) continue;
    const ssize_t n = ::recv(socketFd, buffer.data(), buffer.size(), 0);
    const int error = n < 0 ? errno : 0;  // WHY: read errno at once, before anything else can change it
    const UeventRecvAction what = classifyUeventRecv(n, error);
    if (what == UeventRecvAction::stopReading) {
      failed(systemError("device event socket could not be read", error));
      return;
    }
    if (what == UeventRecvAction::readAgain) continue;
    if (what == UeventRecvAction::parseMessage &&
        !parseUsbUevent(std::string_view(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(n)),
                        ids)) {
      continue;
    }
    changed();
  }
}

}

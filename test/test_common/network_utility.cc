#include "test/test_common/network_utility.h"

#include "common/common/assert.h"
#include "common/network/address_impl.h"
#include "test/test_common/utility.h"

namespace Network {
namespace Test {

Address::InstanceConstSharedPtr findOrCheckFreePort(Address::InstanceConstSharedPtr addr_port,
                                                    Address::SocketType type) {
  if (addr_port == nullptr || addr_port->type() != Address::Type::Ip) {
    ADD_FAILURE() << "Not an internet address: " << (addr_port == nullptr ? "nullptr"
                                                                          : addr_port->asString());
    return nullptr;
  }
  const int fd = addr_port->socket(type);
  if (fd < 0) {
    const int err = errno;
    ADD_FAILURE() << "socket failed for '" << addr_port->asString()
                  << "' with error: " << strerror(err) << " (" << err << ")";
    return nullptr;
  }
  ScopedFdCloser closer(fd);
  // Not setting REUSEADDR, therefore if the address has been recently used we won't reuse it here.
  // However, because we're going to use the address while checking if it is available, we'll need
  // to set REUSEADDR on listener sockets created by tests using an address validated by this means.
  int rc = addr_port->bind(fd);
  const char* failing_fn = nullptr;
  if (rc != 0) {
    failing_fn = "bind";
  } else if (type == Address::SocketType::Stream) {
    // Try listening on the port also, if the type is TCP.
    rc = ::listen(fd, 1);
    if (rc != 0) {
      failing_fn = "listen";
    }
  }
  if (failing_fn != nullptr) {
    const int err = errno;
    if (err == EADDRINUSE) {
      // The port is already in use. Perfectly normal.
      return nullptr;
    } else if (err == EACCES) {
      // A privileged port, and we don't have privileges. Might want to log this.
      return nullptr;
    }
    // Unexpected failure.
    ADD_FAILURE() << failing_fn << " failed for '" << addr_port->asString()
                  << "' with error: " << strerror(err) << " (" << err << ")";
    return nullptr;
  }
  // If the port we bind is zero, then the OS will pick a free port for us (assuming there are
  // any), and we need to find out the port number that the OS picked so we can return it.
  if (addr_port->ip()->port() == 0) {
    return Address::addressFromFd(fd);
  }
  return addr_port;
}

Address::InstanceConstSharedPtr findOrCheckFreePort(const std::string& addr_port,
                                                    Address::SocketType type) {
  auto instance = Address::parseInternetAddressAndPort(addr_port);
  if (instance != nullptr) {
    instance = findOrCheckFreePort(instance, type);
  } else {
    ADD_FAILURE() << "Unable to parse as an address and port: " << addr_port;
  }
  return instance;
}

} // Test
} // Network

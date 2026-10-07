// Tests of the address policy of the outbound requests. Rails: surfguard (Surfguard.blocked_address?).
#include "app/network_guard.hpp"

#include <arpa/inet.h>
#include <doctest.h>

#include <string>

namespace campfire::app {

namespace {

bool blocked(const char* text) {
  unsigned char bytes[16] = {};
  if (inet_pton(AF_INET, text, bytes) == 1) return blocked_address(std::string_view(reinterpret_cast<char*>(bytes), 4));
  REQUIRE(inet_pton(AF_INET6, text, bytes) == 1);
  return blocked_address(std::string_view(reinterpret_cast<char*>(bytes), 16));
}

}  // namespace

TEST_CASE("network guard: the IPv4 ranges of surfguard") {
  for (const char* text : {"0.1.2.3", "10.0.0.1", "100.64.0.1", "127.0.0.1", "169.254.169.254", "172.16.0.1",
                           "192.0.0.1", "192.0.2.1", "192.88.99.1", "192.168.1.1", "198.18.0.1", "198.51.100.1",
                           "203.0.113.1", "224.0.0.1", "240.0.0.1", "255.255.255.255"}) {
    CHECK_MESSAGE(blocked(text), text);
  }
  for (const char* text : {"1.1.1.1", "8.8.8.8", "93.184.216.34", "172.32.0.1"})
    CHECK_FALSE_MESSAGE(blocked(text), text);
}

TEST_CASE("network guard: the IPv6 ranges of surfguard") {
  for (const char* text : {"::",
                           "::1",
                           "::7f00:1",
                           "::ffff:8.8.8.8",
                           "::ffff:127.0.0.1",
                           "100::1",
                           "2001::1",
                           "2001:2::1",
                           "2001:db8::1",
                           "2002:7f00:1::",
                           "2002:808:808::",
                           "fc00::1",
                           "fd00:ec2::254",
                           "fe80::1",
                           "fec0::1",
                           "ff02::1",
                           "64:ff9b::7f00:1",
                           "64:ff9b::a9fe:a9fe",
                           "64:ff9b:1::808:808",
                           "::ffff:0:7f00:1"}) {
    CHECK_MESSAGE(blocked(text), text);
  }
  for (const char* text : {"2606:4700:4700::1111", "2a00:1450:4001::200e", "64:ff9b::808:808", "::ffff:0:808:808"}) {
    CHECK_FALSE_MESSAGE(blocked(text), text);
  }
}

TEST_CASE("network guard: a length that is not an address is blocked") {
  CHECK(blocked_address("abc"));
  CHECK(blocked_address(""));
}

}  // namespace campfire::app

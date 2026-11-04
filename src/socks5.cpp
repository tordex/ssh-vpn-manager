#include "socks5.hpp"

#include <arpa/inet.h>

namespace socks5 {

namespace {
const char kGreetingReply[] = "\x05\x00";
const char kNoMethodReply[] = "\x05\xff";
const char kFailReply[] = "\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00";
}  // namespace

Result process(Stage& stage, const std::string& in) {
  Result r;

  if (stage == Stage::Greeting) {
    if (in.size() < 2) return r;
    const std::uint8_t ver = static_cast<std::uint8_t>(in[0]);
    const std::uint8_t nm = static_cast<std::uint8_t>(in[1]);
    if (ver != 5 || nm == 0) {
      r.kind = Result::Kind::Fail;
      r.reply.assign(kNoMethodReply, 2);
      r.consumed = in.size();  // drop everything; client is wrong
      return r;
    }
    if (in.size() < 2u + nm) return r;

    bool no_auth = false;
    for (std::size_t i = 0; i < nm; ++i) {
      if (static_cast<std::uint8_t>(in[2 + i]) == 0x00) {
        no_auth = true;
        break;
      }
    }
    r.consumed = 2 + nm;
    if (!no_auth) {
      r.kind = Result::Kind::Fail;
      r.reply.assign(kNoMethodReply, 2);
      stage = Stage::Stream;  // terminal
      return r;
    }
    r.kind = Result::Kind::Greeting;
    r.reply.assign(kGreetingReply, 2);
    stage = Stage::Request;
    return r;
  }

  if (stage == Stage::Request) {
    if (in.size() < 4) return r;
    const std::uint8_t ver = static_cast<std::uint8_t>(in[0]);
    const std::uint8_t cmd = static_cast<std::uint8_t>(in[1]);
    const std::uint8_t atyp = static_cast<std::uint8_t>(in[3]);

    std::size_t alen = 0;
    if (atyp == 0x01) {
      alen = 4;  // IPv4
    } else if (atyp == 0x04) {
      alen = 16;  // IPv6
    } else if (atyp == 0x03) {  // domain name
      if (in.size() < 5) return r;
      alen = static_cast<std::uint8_t>(in[4]);
    } else {
      r.kind = Result::Kind::Fail;
      r.reply.assign(kFailReply, 10);
      r.consumed = in.size();
      stage = Stage::Stream;
      return r;
    }

    const std::size_t total = (atyp == 0x03 ? 5 + alen : 4 + alen) + 2;
    if (in.size() < total) return r;

    if (ver != 5 || cmd != 0x01) {  // only CONNECT supported
      r.kind = Result::Kind::Fail;
      r.reply.assign(kFailReply, 10);
      r.consumed = total;
      stage = Stage::Stream;
      return r;
    }

    std::string host;
    if (atyp == 0x01) {
      char buf[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, in.data() + 4, buf, sizeof buf)) host = buf;
    } else if (atyp == 0x04) {
      char buf[INET6_ADDRSTRLEN];
      if (inet_ntop(AF_INET6, in.data() + 4, buf, sizeof buf)) host = buf;
    } else {
      host.assign(in.data() + 5, alen);
    }
    if (host.empty()) {
      r.kind = Result::Kind::Fail;
      r.reply.assign(kFailReply, 10);
      r.consumed = total;
      stage = Stage::Stream;
      return r;
    }

    r.kind = Result::Kind::Connect;
    r.host = std::move(host);
    r.port = static_cast<std::uint16_t>(
        (static_cast<std::uint8_t>(in[total - 2]) << 8) |
        static_cast<std::uint8_t>(in[total - 1]));
    r.consumed = total;
    stage = Stage::Stream;
    return r;
  }

  // Stage::Stream: no handshake processing.
  return r;
}

}  // namespace socks5

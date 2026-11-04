#include "../src/socks5.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool cond, const char* msg) {
  if (!cond) {
    std::printf("FAIL: %s\n", msg);
    ++failures;
  }
}

// Mimics the tunnel worker's buffer handling: append bytes, process, erase consumed.
struct Session {
  socks5::Stage stage = socks5::Stage::Greeting;
  std::string buf;

  socks5::Result feed(const std::string& bytes) {
    buf += bytes;
    socks5::Result r = socks5::process(stage, buf);
    if (r.consumed) {
      buf.erase(0, r.consumed);
    }
    return r;
  }
};

int main() {
  using namespace socks5;

  // 1) Greeting: select no-auth.
  {
    Session s;
    Result r = s.feed(std::string("\x05\x01\x00", 3));
    check(r.kind == Result::Kind::Greeting, "greeting selects no-auth");
    check(r.reply == std::string("\x05\x00", 2), "greeting reply bytes");
    check(s.stage == Stage::Request, "stage -> Request");
  }

  // 2) Greeting fed in two chunks.
  {
    Session s;
    check(s.feed(std::string("\x05", 1)).kind == Result::Kind::NeedMore, "partial greeting");
    Result r = s.feed(std::string("\x01\x00", 2));
    check(r.kind == Result::Kind::Greeting, "chunked greeting completes");
  }

  // 3) Greeting: no no-auth method offered -> fail.
  {
    Session s;
    Result r = s.feed(std::string("\x05\x02\x01\x02", 4));
    check(r.kind == Result::Kind::Fail, "no no-auth method fails");
  }

  // 4) Greeting: wrong version -> fail.
  {
    Session s;
    Result r = s.feed(std::string("\x04\x01\x00", 3));
    check(r.kind == Result::Kind::Fail, "wrong version fails");
  }

  // 5) CONNECT to IPv4 127.0.0.1:443.
  {
    Session s;
    s.feed(std::string("\x05\x01\x00", 3));
    Result r = s.feed(std::string("\x05\x01\x00\x01\x7f\x00\x00\x01\x01\xbb", 10));
    check(r.kind == Result::Kind::Connect, "ipv4 connect");
    check(r.host == "127.0.0.1", "ipv4 host");
    check(r.port == 443, "ipv4 port");
  }

  // 6) CONNECT to a domain name example.com:80.
  {
    Session s;
    s.feed(std::string("\x05\x01\x00", 3));
    std::string req;
    req.push_back('\x05');
    req.push_back('\x01');
    req.push_back('\x00');
    req.push_back('\x03');  // atyp = domain
    req.push_back('\x0b');  // len = 11
    req += "example.com";
    req.push_back('\x00');  // port hi
    req.push_back('\x50');  // port lo (80)
    Result r = s.feed(req);
    check(r.kind == Result::Kind::Connect, "domain connect");
    check(r.host == "example.com", "domain host");
    check(r.port == 80, "domain port");
  }

  // 7) CONNECT to IPv6 ::1:53.
  {
    Session s;
    s.feed(std::string("\x05\x01\x00", 3));
    std::string req;
    req.push_back('\x05');
    req.push_back('\x01');
    req.push_back('\x00');
    req.push_back('\x04');  // atyp = ipv6
    req.append(16, '\0');
    req.back() = '\x01';  // ::1
    req.push_back('\x00');
    req.push_back('\x35');  // port 53
    Result r = s.feed(req);
    check(r.kind == Result::Kind::Connect, "ipv6 connect");
    check(r.host == "::1", "ipv6 host");
    check(r.port == 53, "ipv6 port");
  }

  // 8) Non-CONNECT command (BIND=0x02) -> fail.
  {
    Session s;
    s.feed(std::string("\x05\x01\x00", 3));
    Result r = s.feed(std::string("\x05\x02\x00\x01\x00\x00\x00\x00\x00\x00", 10));
    check(r.kind == Result::Kind::Fail, "non-connect command fails");
  }

  // 9) Request split across chunks.
  {
    Session s;
    s.feed(std::string("\x05\x01\x00", 3));
    std::string req;
    req.push_back('\x05');
    req.push_back('\x01');
    req.push_back('\x00');
    req.push_back('\x01');  // atyp = ipv4
    req += std::string("\x08\x08\x08\x08", 4);
    req.push_back('\x01');
    req.push_back('\xbb');  // port 443
    check(s.feed(req.substr(0, 4)).kind == Result::Kind::NeedMore, "request header partial");
    Result r = s.feed(req.substr(4));
    check(r.kind == Result::Kind::Connect, "chunked request completes");
    check(r.host == "8.8.8.8", "chunked request host");
    check(r.port == 443, "chunked request port");
  }

  if (failures == 0) {
    std::printf("all socks5 tests passed\n");
    return 0;
  }
  std::printf("%d failures\n", failures);
  return 1;
}

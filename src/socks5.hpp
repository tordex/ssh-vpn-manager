#pragma once

#include <cstdint>
#include <string>

namespace socks5 {

enum class Stage { Greeting, Request, Stream };

// Result of feeding bytes into the handshake.
struct Result {
  enum class Kind {
    NeedMore,   // wait for more input
    Greeting,   // reply the selected auth method (in `reply`)
    Fail,       // reply a failure (in `reply`)
    Connect,    // proceed to open a direct connection to host:port
  } kind = Kind::NeedMore;

  std::string reply;    // bytes to write back to the client (Greeting/Fail)
  std::string host;     // target (Connect)
  std::uint16_t port = 0;
  std::size_t consumed = 0;  // bytes consumed from the front of `input`
};

// Minimal SOCKS5 CONNECT-only, no-auth handshake. `stage` advances
// Greeting -> Request -> Stream. The caller appends incoming bytes to a buffer,
// calls process(), and on a non-NeedMore result erases `consumed` bytes from the
// front of that buffer before the next call. On Fail/Connect the buffer has been
// fully consumed for that request.
Result process(Stage& stage, const std::string& input);

}  // namespace socks5

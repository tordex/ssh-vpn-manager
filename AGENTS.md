# AGENTS.md

Linux-only GTK4/Adwaita app (C++17, Meson) that manages SSH dynamic-forwarding
(SOCKS5-over-SSH) tunnels.

## Build & test

```sh
meson setup build          # add -Dtray=disabled to skip the tray dep
ninja -C build
meson test -C build        # one test: SOCKS5 handshake parser
./build/ssh-vpn-manager
```

Dependencies (pkg-config): `gtkmm-4.0`, `libadwaita-1`, `libssh`, `libsecret-1`,
`libcurl`, `nlohmann_json`. Optional: `ayatana-appindicator-glib` (tray).

## Architecture

- `src/main.cpp` — `adw_init()` then `App::run`. Adwaita is used via its C API
  (`adw_init`); the app/window are plain gtkmm `Gtk::Application`/`Gtk::ApplicationWindow`.
- `src/app.cpp` — owns `Model`, `SecretStore`, `TunnelController`, `TrayIcon`.
  `on_activate` (re-)shows the window (this is also the no-tray recovery path).
  `on_shutdown` stops all tunnels and saves config.
- `src/window.cpp` — two tabs (Tunnels/Servers) built with `Gtk::ListBox` of
  custom rows + modal `Gtk::Window` dialogs. Rows are updated in place on stats
  ticks; the list is rebuilt only on structural changes.
- `src/model.cpp` — servers + tunnels serialized to
  `~/.config/ssh-vpn-manager/config.json` (nlohmann/json). **Passwords are never
  written here.**
- `src/secret_store.cpp` — passwords in the Secret Service via a real libsecret
  `SecretSchema` (`org.sshvpnmanager.SshVpnManager.Password`, one `id` string
  attribute).
- `src/ssh_tunnel.cpp` — one `TunnelWorker` thread per running tunnel.
- `src/socks5.cpp` — pure SOCKS5 (CONNECT-only, no-auth) handshake parser.
- `src/tunnel_controller.cpp` — owns workers + `Glib::Dispatcher`s; emits
  `signal_changed(tunnel_id)`.

## Threading rules

- GTK runs on the main thread only. `TunnelController` methods are main-thread
  only (driven by the UI).
- Each tunnel runs in its own `TunnelWorker` thread. That thread owns a libssh
  session, a loopback SOCKS5 listener, all forward channels, and the reconnect
  loop. It never touches GTK.
- Stats are shared via a mutex inside `TunnelWorker`; the worker calls a
  `notify` callback → `Glib::Dispatcher::emit()` → UI refresh on the main loop.
- `stop()` sets an atomic flag + writes a byte to a pipe that `poll()` watches;
  `join()` happens on the main thread before the dispatcher is destroyed.

## libssh gotchas (the tricky part)

- The session is set **nonblocking** (`ssh_set_blocking(session, 0)`) and driven
  by a single `poll()` loop in `TunnelWorker::run_connected()`. `ssh_channel_open_forward`,
  `ssh_channel_read_nonblocking`, and `ssh_channel_write` all return `SSH_AGAIN`
  and must be retried on the next poll wake. Do not switch to blocking calls.
- Only the `direct-tcpip` channel is used (no shell); each SOCKS `CONNECT` opens
  one. Ping is done by a separate per-tunnel thread that makes an HTTP request
  through the tunnel's own SOCKS5 listener via libcurl (see
  `TunnelWorker::do_probe`), NOT by opening a `direct-tcpip` channel directly.
- `connect_and_auth()` is blocking (bounded by `SSH_OPTIONS_TIMEOUT`); it runs
  before the poll loop starts.
- `ssh_tunnel.cpp` needs `_GNU_SOURCE` (for `pipe2`/`accept4`/`SOCK_NONBLOCK`).
  It is defined before any include in that file.
- libssh >= 0.10: `SSH_OPTIONS_PORT` takes `unsigned int`, `SSH_OPTIONS_TIMEOUT`
  takes `long`.
- libssh has no built-in `ServerAlive*` option. Keepalive is done manually in
  `run_connected()` via `ssh_send_keepalive()` (from `libssh/server.h`; it sends
  `keepalive@openssh.com` with want_reply and is nonblocking on a nonblocking
  session). libssh's nonblocking global-request is a single-slot state machine,
  so `ssh_send_keepalive()` is called twice per interval to guarantee a send.

## Config & secrets format (do not break)

- Config keys match `Model::load` exactly: `servers[]` (id/name/host/port/login/
  ping_host/ping_port) and `tunnels[]` (id/local_port/server_id/ping_host/
  ping_port/ping_interval_sec/connect_timeout_sec/max_reconnect_attempts/
  server_alive_interval/server_alive_count_max/run_on_startup). Missing file
  → empty model is valid.
- Passwords use the real libsecret schema, keyed by server `id`. `secret-tool`
  with `schema <name>` stores under the *Generic* schema (name as an attribute),
  which is NOT what this app reads — use `SecretStore` to inspect/populate.

## Tray

- Uses `ayatana-appindicator-glib` (GLib-only reimplementation; menu is a
  `GMenu` + `GSimpleActionGroup`, left-click is the `activate-event` signal).
  NOT the deprecated GTK3 `ayatana-appindicator3-0.1`.
- The `tray.cpp` code path is not compiled in this repo's default build (the dep
  is optional), so it is the least-exercised code.

## Misc

- SOCKS listener binds loopback only (`127.0.0.1`). Host-key verification is off
  by design (equivalent to `StrictHostKeyChecking=no`).
- Tests live in `tests/`; the SOCKS5 parser is unit-tested via `meson test`.
  Feed it bytes incrementally and honour `Result::consumed`.

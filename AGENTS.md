# AGENTS.md

Linux-only GTK4/Adwaita app (C++17, Meson) that manages SSH dynamic-forwarding
(SOCKS5-over-SSH) tunnels.

## Build & test

```sh
meson setup build          # add -Dtray=disabled to skip the tray dep
ninja -C build
./build/ssh-vpn-manager
```

Dependencies (pkg-config): `gtkmm-4.0`, `libadwaita-1`, `libsecret-1`,
`libcurl`, `nlohmann_json`. Optional: `ayatana-appindicator-glib` (tray).
Requires the OpenSSH `ssh` client binary at runtime (tunnels run `ssh -N -D`).

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
- `src/ssh_tunnel.cpp` — one `TunnelWorker` thread per running tunnel; supervises
  an `ssh -N -D` child process and restarts it on exit.
- `src/ssh_runner.cpp` — shared helpers to spawn `ssh` with a password supplied
  via an `SSH_ASKPASS` helper, capture stderr, and terminate the child's process
  group. Used by both the tunnel worker and the server checker.
- `src/tunnel_controller.cpp` — owns workers + `Glib::Dispatcher`s; emits
  `signal_changed(tunnel_id)`.

## Threading rules

- GTK runs on the main thread only. `TunnelController` methods are main-thread
  only (driven by the UI).
- Each tunnel runs in its own `TunnelWorker` thread. That thread spawns and
  supervises an `ssh -N -D` child process and runs the reconnect loop. It never
  touches GTK.
- Stats are shared via a mutex inside `TunnelWorker`; the worker calls a
  `notify` callback → `Glib::Dispatcher::emit()` → UI refresh on the main loop.
- `stop()` sets an atomic flag + writes a byte to a pipe that `poll()` watches;
  `join()` happens on the main thread before the dispatcher is destroyed.

## ssh -D / process supervision (the tricky part)

- Tunnels run `ssh -N -D 127.0.0.1:<port> ... <login>@<host>` as a child process
  (via `posix_spawn` with `POSIX_SPAWN_SETSID`), so the whole process group can
  be signalled as a unit. `ssh` owns the SOCKS5 listener and the forwarding; the
  app no longer implements either.
- The password is supplied through an `SSH_ASKPASS` helper script that echoes
  `$SSH_VPN_MANAGER_PASSWORD`, with `SSH_ASKPASS_REQUIRE=force` and stdin wired
  to `/dev/null`. The password is passed per-spawn via the environment (never in
  argv), so it stays out of `ps`. This needs OpenSSH >= 8.4 (for
  `SSH_ASKPASS_REQUIRE`).
- "Connected" is detected by probing `127.0.0.1:<port>` with a nonblocking
  `connect()` — `ssh -D` binds the listener only after authentication succeeds.
  Child exit before that is treated as a connect failure (counted against
  `max_reconnect_attempts`); exit after connecting is a plain reconnect.
- Keepalive maps to `ssh`'s native `ServerAliveInterval`/`ServerAliveCountMax`
  (omitted when `server_alive_interval` is 0); `ExitOnForwardFailure=yes` makes a
  bind failure (port in use) exit non-zero; `StrictHostKeyChecking=no` +
  `UserKnownHostsFile=/dev/null` preserve the old no-verification behaviour.
- Error messages are parsed from ssh's captured stderr
  (`ssh_runner::error_from_stderr`, prefers specific patterns like "Address
  already in use" over a trailing generic line).
- Stats are sampled from `/proc`: active connections from `/proc/net/tcp`
  (ESTABLISHED entries on the tunnel port), Tx/Rx from `/proc/<pid>/io`
  (`wchar`/`rchar`) — approximate, includes SSH framing overhead.
- `ssh_runner.cpp` needs `_GNU_SOURCE` (for `pipe2`); it is defined before any
  include in that file. `ssh_tunnel.cpp` also defines it (for `pipe2`).

## Config & secrets format (do not break)

- Config keys match `Model::load` exactly: `servers[]` (id/name/host/port/login/
  ping_host/ping_port), `tunnels[]` (id/local_port/server_id/ping_host/
  ping_port/ping_interval_sec/connect_timeout_sec/max_reconnect_attempts/
  server_alive_interval/server_alive_count_max/run_on_startup), and a `settings`
  object (`route_on_command`/`route_off_command` — shell hooks run around
  sing-box routing). Missing file → empty model is valid.
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

- `ssh -D` binds loopback only (`127.0.0.1`). Host-key verification is off
  by design (equivalent to `StrictHostKeyChecking=no`).
- The per-server "Check" button runs a one-shot `ssh ... true` (via
  `ssh_runner::spawn`) and reports `ping_ms` as the login round-trip; the
  per-server `ping_host`/`ping_port` are no longer used for that check.

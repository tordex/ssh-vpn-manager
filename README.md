# SSH VPN Manager

A GTK4/Adwaita desktop application for managing SSH **dynamic port forwarding**
(SOCKS5-over-SSH) tunnels on Linux. Written in C++17.

## Features

- Maintains a list of SSH servers (name, host/IP, port, login, password).
- Passwords are encrypted at rest in the freedesktop Secret Service
  (GNOME Keyring / KWallet), never written to disk in plain text.
- Add / remove / edit servers.
- Maintains a list of local SOCKS5 tunnels, each bound to a local port and linked
  to an SSH server. Add / remove / edit tunnels.
- Per-tunnel **Run** / **Stop** actions. Running a tunnel is equivalent to
  `ssh -D <port> <login>@<server>`.
- Configurable connection timeout.
- Automatic reconnect with exponential backoff when the SSH connection drops.
- Live status (stopped / connecting / connected / reconnecting / error).
- Ping latency (RTT) to a configured host, measured through the tunnel.
- Transferred byte counters (Tx / Rx, approximate — see Notes).
- "Run on startup" flag per tunnel (starts the tunnel when the app launches).
- Closing the window hides it to a system tray icon; the tray icon restores it.
- Hidden mode: start with no window (tray only) via `--hidden`, so tunnels marked
  "run on startup" come up silently at login.

## Dependencies

| Library | Purpose | pkg-config name |
|---|---|---|
| GTK4 (gtkmm) | UI | `gtkmm-4.0` |
| libadwaita | Adwaita look & behaviour | `libadwaita-1` |
| OpenSSH client | SSH transport (`ssh` binary) | — |
| libsecret | encrypted password storage | `libsecret-1` |
| nlohmann/json | config serialization | `nlohmann_json` |
| ayatana-appindicator-glib (optional) | system tray | `ayatana-appindicator-glib` |

## Build

```sh
meson setup build
ninja -C build
```

The `ssh` (OpenSSH) client binary must be installed; tunnels are implemented by
running `ssh -N -D <port> <login>@<server>` as a supervised child process.

Run:

```sh
./build/ssh-vpn-manager
```

Install (optional):

```sh
meson install -C build
```

### Hidden mode & autostart

Start the app hidden (tray icon only, no main window):

```sh
./build/ssh-vpn-manager --hidden
```

Tunnels marked "run on startup" are still started in hidden mode. To show the
window afterwards, click the tray icon (or launch the app again).

To start hidden automatically at login, copy the provided desktop entry into
your XDG autostart directory:

```sh
cp data/org.sshvpnmanager.SshVpnManager.autostart.desktop ~/.config/autostart/
```

(Log out and back in, or use your desktop environment's autostart settings.)

### System tray

The tray icon uses the GLib-only
[`libayatana-appindicator-glib`](https://github.com/AyatanaIndicators/libayatana-appindicator-glib)
library (the newer, GTK-free reimplementation — not the deprecated GTK3 one). It
is detected automatically at configure time. If it is not installed, the app
still builds and runs: closing the window hides it, and launching the app again
re-shows it. You can force the behaviour with `meson setup build -Dtray=enabled`
or `-Dtray=disabled`.

## Configuration & data

- Servers and tunnels are stored in `~/.config/ssh-vpn-manager/config.json`
  (respects `$XDG_CONFIG_HOME`).
- Passwords are stored in the Secret Service keyring, keyed by the server id.
- The SOCKS5 listener binds to loopback (`127.0.0.1`) only.

## Notes

- Host-key verification is not performed (the app connects like
  `ssh -o StrictHostKeyChecking=no`). Treat the SSH servers as trusted.
- Decrypted passwords are held in memory only while the corresponding tunnel is
  running (needed for reconnects).
- The Tx / Rx counters are read from `/proc/<pid>/io` for the `ssh` process and
  therefore include SSH framing and keepalive overhead; they are an approximation
  of tunneled traffic, not an exact count.

## License

Licensed under the [MIT License](LICENSE).

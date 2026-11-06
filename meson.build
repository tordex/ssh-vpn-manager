project('ssh-vpn-manager', 'cpp',
  version: '0.1.0',
  meson_version: '>= 0.61',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtkmm     = dependency('gtkmm-4.0')
adwaita   = dependency('libadwaita-1')
libsecret = dependency('libsecret-1')
libcurl   = dependency('libcurl')
nlohmann  = dependency('nlohmann_json')
threads   = dependency('threads')

deps = [gtkmm, adwaita, libsecret, libcurl, nlohmann, threads]

# System tray via StatusNotifierItem. Optional: the app still works without it
# (closing hides the window, launching the app again re-shows it).
appindicator = dependency('ayatana-appindicator-glib', required: get_option('tray'))
if appindicator.found()
  deps += appindicator
  add_project_arguments('-DHAVE_APPINDICATOR=1', language: 'cpp')
endif

sources = files(
  'src/main.cpp',
  'src/app.cpp',
  'src/window.cpp',
  'src/model.cpp',
  'src/secret_store.cpp',
  'src/server_checker.cpp',
  'src/ssh_runner.cpp',
  'src/ssh_tunnel.cpp',
  'src/tunnel_controller.cpp',
  'src/log.cpp',
  'src/tray.cpp',
)

# Reformat the sources with clang-format before building:
#   ninja -C build format
# `format` runs clang-format -i on every source/header file in `sources`.
clang_format = find_program('clang-format', required: false)
if clang_format.found()
  run_target('format',
    command: [clang_format, '-i', sources])
endif

executable('ssh-vpn-manager', sources,
  dependencies: deps,
  install: true)


install_data('data/org.sshvpnmanager.SshVpnManager.desktop',
  install_dir: get_option('datadir') / 'applications')

install_data('data/org.sshvpnmanager.SshVpnManager.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')

install_data('data/icons/org.sshvpnmanager.SshVpnManager.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

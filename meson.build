project('ssh-vpn-manager', 'cpp',
  version: '0.1.0',
  meson_version: '>= 0.61',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtkmm     = dependency('gtkmm-4.0')
adwaita   = dependency('libadwaita-1')
libssh    = dependency('libssh')
libsecret = dependency('libsecret-1')
nlohmann  = dependency('nlohmann_json')
threads   = dependency('threads')

deps = [gtkmm, adwaita, libssh, libsecret, nlohmann, threads]

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
  'src/ssh_tunnel.cpp',
  'src/socks5.cpp',
  'src/tunnel_controller.cpp',
  'src/log.cpp',
  'src/tray.cpp',
)

executable('ssh-vpn-manager', sources,
  dependencies: deps,
  install: true)

socks5_test = executable('socks5_test',
  ['src/socks5.cpp', 'tests/socks5_test.cpp'],
  dependencies: threads)
test('socks5', socks5_test)


install_data('data/org.sshvpnmanager.SshVpnManager.desktop',
  install_dir: get_option('datadir') / 'applications')

install_data('data/org.sshvpnmanager.SshVpnManager.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')

install_data('data/icons/org.sshvpnmanager.SshVpnManager.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

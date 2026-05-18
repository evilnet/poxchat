# Vendored zstd (single-file amalgamation, v1.5.7)
zstd_lib = static_library('zstd',
  sources: 'zstd/zstd.c',
  c_args: ['-DZSTD_DISABLE_ASM', '-Wno-inline'],
  pic: true,
)
zstd_dep = declare_dependency(
  link_with: zstd_lib,
  include_directories: include_directories('zstd'),
)

common_sources = [
  'cfgfiles.c',
  'chanopt.c',
  'chathistory.c',
  'ctcp.c',
  'dcc.c',
  'poxchat.c',
  'history.c',
  'ignore.c',
  'inbound.c',
  'modes.c',
  'network.c',
  'network-icon.c',
  'notify.c',
  'outbound.c',
  'plugin.c',
  'plugin-identd.c',
  'plugin-timer.c',
  'proto-irc.c',
  'scram.c',
  'scrollback.c',
  'server.c',
  'sqlite-zstd-vfs.c',
  'servlist.c',
	'text.c',
  'tree.c',
  'url.c',
  'userlist.c',
  'util.c'
]

common_sysinfo_deps = []

common_deps = [
  libgio_dep,
  libcanberra_dep,
  libsqlite3_dep,
  zstd_dep,
] + global_deps

common_includes = [
  config_h_include,
  include_directories('.')
]

common_cflags = [
  '-DHAVE_CONFIG_H',
]

if host_machine.system() == 'windows'
  common_deps += [
    cc.find_library('ws2_32'), # winsock
    cc.find_library('winmm'), # playsound
  ]
  common_sysinfo_deps += [
    cc.find_library('wbemuuid'), # sysinfo
  ]

  common_sources += 'sysinfo/win32/backend.c'
  common_includes += include_directories('sysinfo')
endif

marshal = gnome.genmarshal('marshal',
  sources: 'marshalers.list',
  prefix: '_poxchat_marshal',
  internal: true
)

make_te = find_program('make-te.py')

textevents = custom_target('textevents',
  input: 'textevents.in',
  output: ['textevents.h', 'textenums.h'],
  command: [make_te, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@']
)

# TODO:
#   LOOKUPD
#   SIGACTION
#   HAVE_GTK_MAC

if libssl_dep.found()
  common_sources += 'ssl.c'
  common_deps += libssl_dep
endif

if dbus_glib_dep.found()
  subdir('dbus')
  common_deps += poxchat_dbus_dep
  common_includes += include_directories('dbus')
endif

if get_option('plugin')
  common_deps += libgmodule_dep
  install_headers('poxchat-plugin.h')
endif

# libsoup3 and jansson for HTTP requests (network icon fetch, OAuth2 token exchange)
libsoup_dep = dependency('libsoup-3.0', required: false)
jansson_dep = dependency('jansson', version: '>= 2.10', required: false)
if libsoup_dep.found() and jansson_dep.found()
  common_deps += [libsoup_dep, jansson_dep]
  common_cflags += '-DUSE_LIBSOUP'
endif

# WebSocket/HTTP server support (for OAuth2 callbacks and Electron frontend)
if libwebsockets_dep.found()
  common_sources += ['ws-server.c', 'oauth.c', 'secure-storage.c']
  common_deps += libwebsockets_dep

  # libsecret for secure token storage on Linux
  libsecret_dep = dependency('libsecret-1', required: false)
  if libsecret_dep.found()
    common_deps += libsecret_dep
    common_cflags += '-DUSE_LIBSECRET'
  endif
endif

poxchat_common = static_library('poxchatcommon',
  sources: [textevents] + marshal + common_sources,
  include_directories: config_h_include,
  dependencies: common_deps + common_sysinfo_deps,
  c_args: common_cflags,
  pic: true
)

poxchat_common_dep = declare_dependency(
  sources: [textevents] + marshal,
  link_with: poxchat_common,
  include_directories: common_includes,
  compile_args: common_cflags,
  dependencies: common_deps,
)

poxchat_plugin_dep = declare_dependency(
  include_directories: common_includes,
  compile_args: common_cflags,
  dependencies: global_deps,
)

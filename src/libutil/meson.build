project(
  'nix-util',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    # TODO(Qyriad): increase the warning level
    'warning_level=1',
    'errorlogs=true', # Please print logs for tests that fail
  ],
  meson_version : '>= 1.8',
  license : 'LGPL-2.1-or-later',
)

cxx = meson.get_compiler('cpp')

subdir('nix-meson-build-support/deps-lists')

configdata_pub = configuration_data()
configdata_priv = configuration_data()

deps_private_maybe_subproject = []
deps_public_maybe_subproject = []
subdir('nix-meson-build-support/subprojects')

# Check for each of these functions, and create a define like `#define
# HAVE_POSIX_FALLOCATE 1`. The `#define` is unconditional, 0 for not
# found and 1 for found. One therefore uses it with `#if` not `#ifdef`.
check_funcs = [
  [
    'posix_fallocate',
    'Optionally used to preallocate files to be large enough before writing to them.',
  ],
]
foreach funcspec : check_funcs
  define_name = 'HAVE_' + funcspec[0].underscorify().to_upper()
  define_value = cxx.has_function(funcspec[0]).to_int()
  configdata_priv.set(define_name, define_value, description : funcspec[1])
endforeach

configdata_pub.set(
  'NIX_UBSAN_ENABLED',
  ('undefined' in get_option('b_sanitize')).to_int(),
  description : 'Whether nix has been built with UBSan enabled',
)

subdir('nix-meson-build-support/libatomic')

# header-only, so no dependency object and nothing in `deps_other`: this
# only checks the headers are present and new enough. The 'context'
# module was here for boost::coroutine2, whose only consumer was
# sinkToSource.
subdir('nix-meson-build-support/boost')

openssl = dependency(
  'libcrypto',
  'openssl',
  version : '>= 1.1.1',
)
deps_private += openssl


cxx = meson.get_compiler('cpp')

config_priv_h = configure_file(
  configuration : configdata_priv,
  output : 'util-config-private.hh',
)

subdir('nix-meson-build-support/common')

sources = [ config_priv_h ] + files(
  'archive.cc',
  'base-n.cc',
  'base-nix-32.cc',
  'canon-path.cc',
  'environment-variables.cc',
  'error.cc',
  'file-content-address.cc',
  'file-descriptor.cc',
  'file-system-at.cc',
  'file-system.cc',
  'fs-sink.cc',
  'hash.cc',
  'logging.cc',
  'memory-source-accessor.cc',
  'posix-source-accessor.cc',
  'progress.cc',
  'serialise.cc',
  'signals.cc',
  'source-accessor.cc',
  'source-path.cc',
  'strings.cc',
  'terminal.cc',
  'thread-pool.cc',
  'url.cc',
  'util.cc',
)

subdir('include/nix/util')

this_library = library(
  'nixutil',
  sources,
  soversion : nix_soversion,
  dependencies : deps_public + deps_private + deps_other,
  include_directories : include_dirs,
  prelink : true, # For C++ static initializers
  install : true,
  cpp_pch : do_pch ? [ 'pch/precompiled-headers.hh' ] : [],
)

install_headers(headers, subdir : 'nix/util', preserve_path : true)

libraries_private = []

subdir('nix-meson-build-support/export')

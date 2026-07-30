# Public headers directory

include_dirs = [
  include_directories('../..'),
]

config_pub_h = configure_file(
  configuration : configdata_pub,
  output : 'config.hh',
)

headers = [ config_pub_h ] + files(
  'content-address.hh',
  'export-import.hh',
  'gc-store.hh',
  'globals.hh',
  'local-settings.hh',
  'local-store.hh',
  'path-info.hh',
  'path.hh',
  'pathlocks.hh',
  'posix-fs-canonicalise.hh',
  'sqlite.hh',
  'store-api.hh',
  'store-dir-config.hh',
  'store-open.hh',
)

# Public headers directory

include_dirs = [
  include_directories('../..'),
]

config_pub_h = configure_file(
  configuration : configdata_pub,
  output : 'config.hh',
)

headers = [ config_pub_h ] + files(
  'common-protocol-impl.hh',
  'common-protocol.hh',
  'content-address.hh',
  'export-import.hh',
  'gc-store.hh',
  'global-paths.hh',
  'globals.hh',
  'length-prefixed-protocol-helper.hh',
  'local-fs-store.hh',
  'local-gc.hh',
  'local-settings.hh',
  'local-store.hh',
  'path-info.hh',
  'path.hh',
  'pathlocks.hh',
  'posix-fs-canonicalise.hh',
  'references.hh',
  'sqlite.hh',
  'store-api.hh',
  'store-cast.hh',
  'store-dir-config.hh',
  'store-open.hh',
  'store-reference.hh',
  'store-registration.hh',
)

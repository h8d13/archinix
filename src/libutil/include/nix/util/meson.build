# Public headers directory

include_dirs = [ include_directories('../..') ]

config_pub_h = configure_file(
  configuration : configdata_pub,
  output : 'config.hh',
)

headers = [ config_pub_h ] + files(
  'alignment.hh',
  'archive.hh',
  'array-from-string-literal.hh',
  'base-n.hh',
  'base-nix-32.hh',
  'canon-path.hh',
  'deleter.hh',
  'error.hh',
  'file-descriptor.hh',
  'file-path-impl.hh',
  'file-path.hh',
  'file-system-at.hh',
  'file-system.hh',
  'finally.hh',
  'fmt.hh',
  'fs-sink.hh',
  'fun.hh',
  'hash.hh',
  'logging.hh',
  'lru-cache.hh',
  'memory-source-accessor.hh',
  'progress.hh',
  'ref.hh',
  'scheduling.hh',
  'serialise.hh',
  'signals.hh',
  'source-accessor.hh',
  'source-path.hh',
  'split.hh',
  'strings-inline.hh',
  'strings.hh',
  'sync.hh',
  'terminal.hh',
  'thread-pool.hh',
  'types.hh',
  'util.hh',
  'variant-wrapper.hh',
)

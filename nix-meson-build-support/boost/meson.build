# Boost, headers only.
#
# Every boost facility this repo uses is header-only: lexical_cast
# (util.cc), format (fmt.hh, logging.hh), container::small_vector
# (strings.hh), and unordered::{unordered_flat_map,concurrent_flat_set}
# (local-store.hh). Nothing links against a libboost_*.
#
# `dependency('boost')` cannot express that. Meson's boost lookup scans
# for compiled libboost_* files first and returns "not found" if it sees
# none, before it ever reads a header (`run_check()` in
# mesonbuild/dependencies/boost.py). Distros do ship the headers alone
# (arch splits `boost` from `boost-libs`; `boost` has no dependency on
# it), so asking meson for boost drags in ~50 MiB of shared objects that
# exist only to satisfy the detector. Read the version header instead.
#
# Boost outside the default include path: pass `-isystem <dir>` in
# CXXFLAGS at setup time.

# 1.87 is the floor for unordered::concurrent_flat_set.
boost_minimum = 108700

boost_version_prefix = '#include <boost/version.hpp>'

if not cxx.has_header('boost/version.hpp')
  error('boost headers not found (need >= 1.87.0)')
endif

# BOOST_VERSION is the numeric form, 1.91.0 -> 109100; comparing it
# avoids the "1.87" vs "1.87.0" ambiguity of the string form.
boost_version = cxx.get_define(
  'BOOST_VERSION',
  prefix : boost_version_prefix,
).to_int()

boost_version_str = cxx.get_define(
  'BOOST_LIB_VERSION',
  prefix : boost_version_prefix,
).strip('"').replace('_', '.')

if boost_version < boost_minimum
  error(
    'boost @0@ is too old (need >= 1.87.0)'.format(boost_version_str),
  )
endif

message('boost (header-only): @0@'.format(boost_version_str))

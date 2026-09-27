// Stable third-party precompiled header — build-time optimization only.
//
// Force-included (via `-include cmake_pch.{h,hxx}`) at the top of every
// translation unit of the targets that opt in through
// target_precompile_headers() (luxrays, slg-core, slg-film, luxcore,
// luxcore_static). It intentionally contains ONLY stable system headers
// (C library, libc++) and Boost — the headers that dominate the ~2000+
// include fan-out of a typical TU and essentially never change during
// development. No LuxCore/project headers: they change under development
// and would invalidate the whole PCH on every edit.
//
// The __cplusplus guard lets C sources that live in a PCH-enabled target
// (e.g. deps/volk/volk.c inside luxrays) consume the generated C-mode
// PCH stub as a no-op.
#ifdef __cplusplus

// ------------------------------------------------------------------
// C library
// ------------------------------------------------------------------
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

// ------------------------------------------------------------------
// libc++ / STL
// ------------------------------------------------------------------
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// ------------------------------------------------------------------
// Boost (most-frequent dep paths measured across slg-core/slg-film/
// luxrays depfiles: lexical_cast ~2661 TUs, format, algorithm/string,
// serialization + archives used by every serializationutils consumer)
// ------------------------------------------------------------------
#include <boost/version.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <boost/circular_buffer.hpp>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <boost/archive/basic_archive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

#include <boost/serialization/access.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/assume_abstract.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/deque.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/level.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/optional.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/tracking.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/version.hpp>

// NOTE: embree4/rtcore*.h, Imath/half.h and OpenImageIO/image_span.h were
// tried here (each is pulled by ~1170 TUs via bvhbuild.h/imagemap.h) but
// the fatter PCH cost more to load per TU than it saved in parsing —
// measured +2% CPU regression vs this STL+Boost list. Keep this list to
// libc++ + Boost only; see doc/engineering/kernel-compile-performance.md.

#endif // __cplusplus

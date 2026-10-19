"""ASan round-trip of the actual FilmSamplesCounts class and methods.

Minimal sequential archive adapters exercise the unchanged serialized field order.
--baseline reads the original class from HEAD and requires its ASan failure.
"""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / "dev-tools/reviews/2026-10-16/fixes"
baseline = "--baseline" in sys.argv
header = subprocess.check_output(["git", "show", "HEAD:include/slg/film/film.h"], cwd=ROOT, text=True) if baseline else (ROOT / "include/slg/film/film.h").read_text()
start = header.index("class FilmSamplesCounts {")
declaration = header[start:header.index("\n};", start) + 3]
cpp = (ROOT / "src/slg/film/filmsamplescounts.cpp").read_text()
methods = cpp[cpp.index("using namespace std;"):]
program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <sys/types.h>
#include <vector>
#include <boost/serialization/access.hpp>
#include <boost/serialization/split_member.hpp>
#include "luxrays/utils/memory.h"
namespace luxrays { template<class T> T Max(T a, T b) { return std::max(a, b); } }
namespace slg {
'''+declaration+"\n}\n"+methods+r'''
struct Saving {
    using is_loading = std::false_type;
    unsigned count = 0;
    std::vector<double> values;
    Saving &operator&(unsigned &n) { count = n; return *this; }
    Saving &operator&(double &v) { values.push_back(v); return *this; }
};
struct Loading {
    using is_loading = std::true_type;
    const Saving &saved;
    size_t index = 0;
    Loading &operator&(unsigned &n) { n = saved.count; return *this; }
    Loading &operator&(double &v) { v = saved.values.at(index++); return *this; }
};
int main() {
    slg::FilmSamplesCounts counts;
    counts.Init(4);
    for (unsigned i = 0; i < 4; ++i) counts.AddSampleCount(i, 10. + i, 20. + i);
    Saving saved;
    boost::serialization::access::serialize(saved, counts, 1);
    slg::FilmSamplesCounts loaded;
    Loading archive{saved};
    boost::serialization::access::serialize(archive, loaded, 1);
    assert(loaded.GetSampleCount() == 86.);
    assert(loaded.GetSampleCount_RADIANCE_PER_PIXEL_NORMALIZED() == 46.);
    assert(loaded.GetSampleCount_RADIANCE_PER_SCREEN_NORMALIZED() == 86.);
    loaded.AddSampleCount(1u, 64., 0.);
    assert(loaded.GetSampleCount() == 150.);
    puts("PASS loaded thread allocation, atomic total and fresh pending batches");
}
'''
includes = []
args = shlex.split(json.loads((ROOT / "out/build/compile_commands.json").read_text())[0]["command"])
for i, arg in enumerate(args):
    if arg.startswith("-I"): includes.append(arg)
    elif arg == "-isystem": includes.extend((arg, args[i + 1]))
with tempfile.TemporaryDirectory(prefix="slc-film-counts-") as tmp:
    file, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
    file.write_text(program)
    built = subprocess.run(["clang++", "-std=c++20", "-O0", "-g", "-fsanitize=address", *includes, str(file), "-o", str(binary)], capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    p = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    tag = "baseline" if baseline else "fixed"
    (OUT / f"film-counts-{tag}.log").write_text(p.stdout + p.stderr)
    print("exit", p.returncode, p.stdout)
    if baseline: assert "heap-buffer-overflow" in p.stderr, p.stderr
    else: assert p.returncode == 0, p.stderr

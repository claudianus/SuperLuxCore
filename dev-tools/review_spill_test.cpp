#include "luxrays/utils/spillablearray.h"
#include "luxrays/core/color/spectrumgroup.h"
#include <cassert>
#include <cstdio>

int main(int argc, char **argv) {
    assert(argc == 2);
    luxrays::SpillableArray<int> a;
    a.push_back(42);
    a.reserve(1024);
    assert(a[0] == 42 && a.data() == a.begin());
    assert(a.Spill(argv[1]) == sizeof(int));
    assert(a.Spilled() && a[0] == 42);
    a.reserve(2048);
    assert(!a.Spilled() && a[0] == 42);
    a.push_back(43);
    assert(a.size() == 2 && a.back() == 43);
    assert(a.Spill(argv[1]) == 2 * sizeof(int));
    luxrays::SpillableArray<int> moved(std::move(a));
    assert(moved[0] == 42 && moved[1] == 43 && a.empty());
    moved.resize(3);
    assert(moved[0] == 42 && moved[1] == 43 && moved[2] == 0);

    struct Photon { luxrays::SpectrumGroup outgoingRadiance; };
    luxrays::SpillableArray<Photon> photons;
    Photon p;
    p.outgoingRadiance.Add(0, luxrays::Spectrum(42.f));
    photons.push_back(p);
    assert(photons.Spill(argv[1]) == 0);
    assert(!photons.Spilled() && photons[0].outgoingRadiance.Sum().c[0] == 42.f);
    photons.reserve(512);
    assert(photons[0].outgoingRadiance.Sum().c[0] == 42.f);
    puts("PASS spill: reserve, mapped reserve, mutation, move, owning payload");
}

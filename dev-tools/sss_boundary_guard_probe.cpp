#include "slg/volumes/sssboundaryguard.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace slg;
static float Legacy(float scatter, float boundary, float cosine, float epsilon, float mint) {
    const float guard = 8.f * epsilon;
    return (boundary - scatter) * cosine < guard ?
        luxrays::Max(mint, boundary - guard / luxrays::Max(cosine, 1e-3f)) : scatter;
}
static void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    const float before = Legacy(.1f, 100.f, 1e-8f, 1e-5f, 1e-5f);
    const float after = SSSGuardedScatter(.1f, 100.f, 1e-8f, 1e-5f, 1e-5f);
    Require(before > 99.f && after == .1f, "grazing regression was not reproduced/fixed");
    std::mt19937 rng(4217);
    auto uniform = [&]() { return float((rng() + .5) / 4294967296.); };
    const unsigned samples = 1048576;
    unsigned nearBoundary = 0, legacyAdvanced = 0;
    for (unsigned n = 0; n < samples; ++n) {
        const float mint = 1e-5f;
        const float scatter = mint + 100.f * uniform();
        const float boundary = scatter + 100.f * uniform();
        const float cosine = powf(uniform(), 6.f);
        const float epsilon = expf(-11.f + 8.f * uniform());
        const float actual = SSSGuardedScatter(scatter, boundary, cosine, epsilon, mint);
        Require(std::isfinite(actual) && actual >= mint && actual <= scatter,
            "guard moved scatter forward or crossed ray origin");
        if (actual != scatter) ++nearBoundary;
        if (Legacy(scatter, boundary, cosine, epsilon, mint) > scatter) ++legacyAdvanced;
        const float regularCosine = .001f + .999f * uniform();
        Require(SSSGuardedScatter(scatter, boundary, regularCosine, epsilon, mint) ==
            Legacy(scatter, boundary, regularCosine, epsilon, mint),
            "nongrazing arithmetic changed");
    }
    Require(nearBoundary > 0 && legacyAdvanced > 0, "probe missed boundary cases");
    std::cout << "{\"samples\":" << samples << ",\"legacy_advanced_cases\":" << legacyAdvanced
        << ",\"near_boundary_cases\":" << nearBoundary << ",\"nongrazing_exact\":true,"
        << "\"regression_before\":" << before << ",\"regression_after\":" << after
        << ",\"passed\":true}\n";
}

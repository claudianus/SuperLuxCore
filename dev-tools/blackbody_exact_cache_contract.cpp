// SPDX-License-Identifier: Apache-2.0
// Call against the loaded native module. Compare cached evaluation with the
// original independent SPD construction/integral, including temperature changes.
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include "slg/textures/blackbody.h"
#include "slg/textures/constfloat.h"
#include "luxrays/core/color/spds/blackbodyspd.h"

extern "C" int slc_blackbody_exact_cache_contract() {
    using namespace slg;
    using namespace luxrays;
    std::atomic<unsigned> count{0}, failures{0};
    const auto check = [&](unsigned worker) {
        HitPoint hp; hp.Init();
        for (const float t : {800.f, 1200.f, 2800.f, 7000.f, 12000.f,
                              2800.f, 2800.f, 2800.25f, 7000.f}) {
            for (bool normalize : {false, true}) {
                ConstFloatTexture input(t);
                BlackBodyTexture constant(t, normalize);
                BlackBodyTexture textured(input, t, normalize);
                for (BlackBodyTexture *texture : {&constant, &textured}) {
                    const BlackbodySPD spd(t);
                    const float y = texture->EvalSpectrumValue(hp).Y();
                    for (unsigned sample = 0; sample < 16; ++sample) {
                        PathWavelengths sw; sw.Sample((sample + .25f * worker) / 17.f);
                        for (const unsigned mask : {1u, 5u, 7u}) {
                            sw.aliveMask = mask;
                            const Spectrum expected = Spectral::WithLuminance(
                                Spectral::EvaluateSPD(spd, sw), spd, y);
                            const Spectrum actual = texture->EvalSpectralValue(hp, sw, true);
                            ++count;
                            if (std::memcmp(expected.c, actual.c, sizeof(expected.c))) ++failures;
                        }
                    }
                }
            }
        }
    };
    std::thread a(check, 0), b(check, 1), c(check, 2), d(check, 3);
    a.join(); b.join(); c.join(); d.join();
    std::cout << "BLACKBODY_EXACT_CACHE evaluations=" << count << " failures=" << failures << std::endl;
    return failures == 0 ? 1 : 0;
}

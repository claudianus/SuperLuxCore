// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include "slg/samplers/sobolsequence.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *out = std::fopen(argv[1], "wb");
    if (!out) return 3;
    slg::SobolSequence sequence;
    const auto begin = std::chrono::steady_clock::now();
    sequence.RequestSamples(10400);
    const auto prepared = std::chrono::steady_clock::now();
    rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    const long preparedRSS = usage.ru_maxrss;
    sequence.rngPass = 0x429173u;
    sequence.rng0 = .234567f;
    sequence.rng1 = .876543f;
    const unsigned passes[] = {0u, 1u, 2u, 17u, 255u, 65535u, 0xfffffffeu, 0xffffffffu};
    for (unsigned mode = 0; mode < 3; ++mode) {
        if (mode == 1) sequence.SetBlueNoiseSeed(349983u);
        if (mode == 2) sequence.SetOwenSeed(672113u, .37f);
        for (unsigned pass : passes)
            for (unsigned dim = 0; dim < 10400; ++dim) {
                const float value = sequence.GetSample(pass, dim);
                if (!(value >= 0.f && value < 1.f)) return 4;
                std::fwrite(&value, sizeof(value), 1, out);
            }
    }
    std::fclose(out);
    // Exercise repeated allocations and invalidation with a smaller request.
    sequence.RequestSamples(64);
    for (unsigned dim = 0; dim < 64; ++dim) sequence.GetSample(17u, dim);
    std::printf("{\"request_seconds\":%.9f,\"prepared_peak_rss_bytes\":%ld,\"values\":249600}\n",
        std::chrono::duration<double>(prepared-begin).count(), preparedRSS);
    return 0;
}

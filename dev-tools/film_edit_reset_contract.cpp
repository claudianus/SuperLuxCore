// SPDX-License-Identifier: Apache-2.0
// Run against the loaded module, including the previously failing build.
#include <algorithm>
#include <iostream>
#include "slg/film/film.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/film/sampleresult.h"
#include "slg/film/filters/filter.h"

extern "C" int slc_film_edit_reset_contract() {
    using namespace slg;
    using namespace luxrays;
    auto film = Film::Create(2, 2);
    film->AddChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED);
    film->Init();
    const FilterUPtr noFilter;
    FilmSampleSplatter splatter(noFilter);
    const Film::FilmChannels channels{Film::RADIANCE_PER_PIXEL_NORMALIZED};
    SampleResult sample(&channels, 1);
    sample.filmX = sample.filmY = .5f;
    sample.radiance[0] = Spectrum(2.f);
    splatter.AtomicSplatSample(*film, sample, 1.f);
    const auto collected = film->pixelLumaMoments;
    const bool accumulated = collected[0] > 0.f && collected[1] > 0.f;
    // Film merging clears radiance repeatedly; its sampler history must survive.
    film->Clear();
    const bool mergePreserved = collected == film->pixelLumaMoments;
    film->Reset();
    const bool editCleared = std::all_of(film->pixelLumaMoments.begin(),
        film->pixelLumaMoments.end(), [](float x) { return x == 0.f; });
    std::cout << "FILM_RESET accumulated=" << accumulated
        << " merge_preserved=" << mergePreserved
        << " edit_cleared=" << editCleared << std::endl;
    return accumulated && mergePreserved && editCleared;
}

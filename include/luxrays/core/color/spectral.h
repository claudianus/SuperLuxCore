/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   Licensed under the Apache License, Version 2.0 (the "License");       *
 *   you may not use this file except in compliance with the License.      *
 *   You may obtain a copy of the License at                               *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *                                                                         *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

#ifndef _LUXRAYS_SPECTRAL_H
#define _LUXRAYS_SPECTRAL_H

#include "luxrays/core/color/color.h"
#include "luxrays/core/color/spd.h"

#include <string>
#include <vector>

namespace luxrays {

// Hero-wavelength spectral transport: SPECTRAL_BINS wavelengths per path ride
// inside the existing 3-channel Spectrum. Channels become spectral samples at
// w[i] instead of RGB primaries; transport arithmetic is unchanged.
#define SPECTRAL_BINS 3
#define SPECTRAL_START 380.f
#define SPECTRAL_END 720.f
// Wavelength interval each bin is responsible for
#define SPECTRAL_BIN_WIDTH ((SPECTRAL_END - SPECTRAL_START) / SPECTRAL_BINS)

class PathWavelengths {
public:
	PathWavelengths() : hero(0), aliveMask(0x7) {
		w[0] = SPECTRAL_START;
		w[1] = SPECTRAL_START + SPECTRAL_BIN_WIDTH;
		w[2] = SPECTRAL_START + 2.f * SPECTRAL_BIN_WIDTH;
	}

	// Sample a stratified set of SPECTRAL_BINS wavelengths sharing one offset,
	// plus the hero bin index (used by dispersive events, slice S2).
	void Sample(const float u1) {
		const float x = u1 * SPECTRAL_BINS;
		hero = Min(Floor2UInt(x), (u_int)(SPECTRAL_BINS - 1)); // guard u1 == 1.0 edge
		const float xi = x - hero; // shared intra-bin offset in [0, 1)
		for (u_int i = 0; i < SPECTRAL_BINS; ++i)
			w[i] = SPECTRAL_START + (i + xi) * SPECTRAL_BIN_WIDTH;
		aliveMask = 0x7;
	}

	float w[SPECTRAL_BINS];
	u_int hero;
	u_int aliveMask;
};

// Thread-local spectral context. Render threads set the current path's
// wavelengths; evaluation sites (textures, lights, volumes) read them without
// any signature changes. Threads that never set it observe Current()==nullptr
// (i.e. plain RGB behaviour), so preprocessing/baking paths are unaffected.
namespace Spectral {

// (SetEnabled/IsEnabled are defined inline below, next to Current().)

// RGB->SPD upsampling model for leaf RGB triplets (texture constants,
// imagemap pixels, light colors). UPSAMPLING_SMITS is the classic
// white+secondary+primary basis decomposition (Smits 1999 style,
// spds/data/rgbE_32.h reflectance / rgbD65_32.h illuminant) and stays the
// default. UPSAMPLING_JH2019 is the Jakob-Hanika 2019 sigmoid model
// ("A Low-Dimensional Function Space for Efficient Spectral Upsampling",
// EGSR 2019; canonical rgb2spec implementation) applied to both
// reflectance and emission colors: the table is optimized for the
// LuxCore gamut under the flat CIE E illuminant, so an emitted SPD
// reproduces its RGB exactly. Selected by the opt-in
// path.spectral.upsampling = jh2019 property.
enum UpsamplingModel {
	UPSAMPLING_SMITS,
	UPSAMPLING_JH2019
};
void SetUpsamplingModel(const UpsamplingModel model);
UpsamplingModel GetUpsamplingModel();

// Read access to the embedded JH2019 coefficient table
// (spds/data/jh2019_32.h): res is the lookup grid resolution, scale[res]
// the non-linear dominant-channel node positions, coeffs[9*res^3] the
// sigmoid polynomial coefficients laid out [dominant][z][y][x][3]. The
// GPU backends upload the same data as a device buffer so CPU and GPU
// upsample identically.
u_int JH2019TableRes();
const float *JH2019TableScale();
const float *JH2019TableCoeffs();

// State lives in header-inline storage so the hot read path (Current(),
// called once per spectral-aware texture/light/volume evaluation) can
// inline the TLS flag test instead of paying an out-of-line call plus a
// _tlv_get_addr pair on every bounce.
namespace detail {
inline bool g_spectralEnabled = false;
inline thread_local PathWavelengths g_sw;
inline thread_local bool g_hasSW = false;
}

inline void SetEnabled(const bool enabled) { detail::g_spectralEnabled = enabled; }
inline bool IsEnabled() { return detail::g_spectralEnabled; }
inline void SetPathWavelengths(const PathWavelengths &sw) {
	detail::g_sw = sw;
	detail::g_hasSW = true;
}
inline void ClearPathWavelengths() { detail::g_hasSW = false; }
inline const PathWavelengths *Current() {
	return (detail::g_spectralEnabled && detail::g_hasSW) ? &detail::g_sw : nullptr;
}

// RAII guard suspending spectral evaluation for the current scope. Used by
// textures that perform RGB-space math on their children (HSV, normal map,
// vector math): child texture calls inside the scope observe
// Current()==nullptr and therefore evaluate plain RGB throughout the subtree.
class ScopePause {
public:
	ScopePause() : active(Current() != nullptr) {
		if (active) {
			saved = *Current();
			ClearPathWavelengths();
		}
	}
	~ScopePause() {
		if (active)
			SetPathWavelengths(saved);
	}
	ScopePause(const ScopePause &) = delete;
	ScopePause &operator=(const ScopePause &) = delete;
private:
	PathWavelengths saved;
	bool active;
};

// RAII guard activating a PathWavelengths for the current scope. A no-op
// when spectral transport is not enabled. Used at path start/end in the
// eye/light path entry points.
class ScopeWavelengths {
public:
	explicit ScopeWavelengths(const PathWavelengths &sw) : active(IsEnabled()) {
		if (active)
			SetPathWavelengths(sw);
	}
	~ScopeWavelengths() {
		if (active)
			ClearPathWavelengths();
	}
	bool Active() const { return active; }
	ScopeWavelengths(const ScopeWavelengths &) = delete;
	ScopeWavelengths &operator=(const ScopeWavelengths &) = delete;
private:
	bool active;
};

// Evaluate the RGB->SPD upsampling basis selected by GetUpsamplingModel()
// (Smits-style reflectance/illuminant bases by default, the JH2019 sigmoid
// table when UPSAMPLING_JH2019) at the given wavelengths. Returns `rgb`
// unchanged when spectral transport is not active on this thread.
Spectrum Reflectance(const Spectrum &rgb);
Spectrum Emission(const Spectrum &rgb);
Spectrum Reflectance(const Spectrum &rgb, const PathWavelengths &sw);
Spectrum Emission(const Spectrum &rgb, const PathWavelengths &sw);

// Evaluate an SPD at the path wavelengths (for textures with native spectra:
// blackbody, lamp spectra, irregular data).
Spectrum EvaluateSPD(const SPD &spd);
Spectrum EvaluateSPD(const SPD &spd, const PathWavelengths &sw);

// Expected film luminance of an SPD under ProjectToRGB (integrated over the
// sampled wavelength range - a constant of the SPD, not of the drawn bins).
float ExpectedLuminance(const SPD &spd);

// Rescale bins evaluated from `spd` so the SPD's expected film luminance
// equals yTarget (luminance parity with the RGB fallback of the same source
// data). The scale is a constant of the SPD: rescaling by the luminance of
// the drawn bins themselves biased saturated spectra.
Spectrum WithLuminance(const Spectrum &bins, const SPD &spd, const float yTarget);

// Dispersive event (S2): terminate the secondary wavelengths, keeping only
// the hero bin. Returns the Monte-Carlo weight for the wavelength selection
// (SPECTRAL_BINS when a collapse actually happened, 1.f otherwise): the
// surviving bin carries the whole path's spectral estimate for the rest of
// the path. No-op when spectral transport is not active on this thread.
float CollapseToHero();

// Project spectral bins to film RGB using CIE matching functions and the
// default color system. Each bin is weighted by its stratum width over the
// white response of the whole sampled range (a constant), so the estimate is
// unbiased: E[rgb] = integral of the spectrum against the white-normalized
// matching functions. A control variate on the mean bin value removes the
// wavelength noise of flat spectra (a flat spectrum reproduces its scalar
// exactly in every sample, as before).
Spectrum ProjectToRGB(const Spectrum &bins, const PathWavelengths &sw);

// -D definitions carrying the projection / upsampling normalization
// constants to the spectral GPU kernels (one source of truth: computed on
// the host from the same tables the kernels sample).
std::vector<std::string> KernelDefines();

// Per-wavelengths projection context: the CIE matching values and the
// sampled white point depend only on the drawn wavelengths, so a sample's
// ~15 spectral fields share one evaluation instead of re-sampling the
// CIE SPDs and re-normalizing per field (bit-identical results).
struct RGBProjector {
	// Per-bin RGB contribution: r_i = XYZToRGB · c_i (CIE at bin i) times
	// the stratum-width / range-white constant of the channel. Defined for
	// every drawn bin (dead bins enter the control variate).
	float cr[SPECTRAL_BINS], cg[SPECTRAL_BINS], cb[SPECTRAL_BINS];
	u_int aliveMask;
	bool valid;
};
void PrepareRGBProjection(const PathWavelengths &sw, RGBProjector &p);
Spectrum ProjectToRGB(const Spectrum &bins, const RGBProjector &p);

// Non-mutating hero-only collapse of a connect contribution (MNEE manifold
// connects): the half-vector constraint is satisfied only at the hero
// wavelength, so the connect carries the MC wavelength-selection weight
// (SPECTRAL_BINS) and the secondary bins are dropped. Passes through
// unchanged when the path was already collapsed.
inline Spectrum KeepHeroBins(const Spectrum &v, const PathWavelengths &sw) {
	const u_int heroMask = 1u << sw.hero;
	if (sw.aliveMask == heroMask)
		return v;
	Spectrum r(0.f);
	r.c[sw.hero] = v.c[sw.hero] * SPECTRAL_BINS;
	return r;
}

} // namespace Spectral

} // namespace luxrays

#endif // _LUXRAYS_SPECTRAL_H
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

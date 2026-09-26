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

#include <cmath>
#include <algorithm>

#include "luxrays/core/color/spectral.h"
#include "luxrays/core/color/spectrumwavelengths.h"
#include "luxrays/core/color/spds/data/rgbE_32.h"
#include "luxrays/core/color/spds/data/rgbD65_32.h"
#include "luxrays/core/color/spds/data/jh2019_32.h"

using namespace luxrays;

namespace {
	bool g_enabled = false;
	Spectral::UpsamplingModel g_upsampling = Spectral::UPSAMPLING_SMITS;
	thread_local PathWavelengths g_sw;
	thread_local bool g_hasSW = false;

	// Linear-interpolated sample of a raw SPD table, same math as SPD::Sample
	inline float SampleTable(const float *data, const u_int n,
			const float start, const float end, const float lambda) {
		if (n <= 1 || lambda < start || lambda > end)
			return 0.f;
		const float x = (lambda - start) * ((n - 1) / (end - start));
		const u_int b0 = Floor2UInt(std::max(x, 0.f));
		const u_int b1 = std::min(b0 + 1, n - 1);
		return std::lerp(data[b0], data[b1], x - b0);
	}

	struct BasisSet {
		const float *white, *cyan, *magenta, *yellow, *red, *green, *blue;
		u_int n; float start, end;
	};

	const BasisSet reflBasis = {
		refrgb2spect_white, refrgb2spect_cyan, refrgb2spect_magenta,
		refrgb2spect_yellow, refrgb2spect_red, refrgb2spect_green, refrgb2spect_blue,
		refrgb2spect_bins, refrgb2spect_start, refrgb2spect_end
	};
	const BasisSet illumBasis = {
		illumrgb2spect_white, illumrgb2spect_cyan, illumrgb2spect_magenta,
		illumrgb2spect_yellow, illumrgb2spect_red, illumrgb2spect_green,
		illumrgb2spect_blue,
		illumrgb2spect_bins, illumrgb2spect_start, illumrgb2spect_end
	};

	// Smits RGB->SPD decomposition evaluated directly at the path wavelengths:
	// rgb is decomposed into (white, secondary, primary) basis weights, then
	// each basis is sampled at every bin. The result is then renormalized so
	// its CIE luminance over the sampled bins matches rgb.Y() (metameric
	// round-trip: flat/achromatic inputs reproduce themselves exactly under
	// the film projector). Negative bins are clamped so sample validity
	// checks don't reject legitimately-upsampled values.
	Spectrum Upsample(const Spectrum &rgb, const PathWavelengths &sw,
			const BasisSet &basis, const float yTarget) {
		const float r = rgb.c[0], g = rgb.c[1], b = rgb.c[2];

		// Achromatic input: a flat SPD is the only metameric-exact choice —
		// the Smits white basis is NOT perfectly flat and would inject a
		// chromatic bias (observed: cyan cast on neutral walls). Keep the
		// round-trip exact: flat RGB -> flat bins -> flat projected RGB.
		if (r == g && g == b) {
			Spectrum out;
			for (u_int i = 0; i < SPECTRAL_BINS; ++i)
				out.c[i] = (sw.aliveMask & (1U << i)) ? std::max(r, 0.f) : 0.f;
			return out;
		}

		float wW; const float *sec; float wSec; const float *prim; float wPrim;
		if (r <= g && r <= b) {
			wW = r;
			sec = basis.cyan;
			if (g <= b) { wSec = g - r; prim = basis.blue;  wPrim = b - g; }
			else        { wSec = b - r; prim = basis.green; wPrim = g - b; }
		} else if (g <= r && g <= b) {
			wW = g;
			sec = basis.magenta;
			if (r <= b) { wSec = r - g; prim = basis.blue; wPrim = b - r; }
			else        { wSec = b - g; prim = basis.red;  wPrim = r - b; }
		} else {
			wW = b;
			sec = basis.yellow;
			if (r <= g) { wSec = r - b; prim = basis.green; wPrim = g - r; }
			else        { wSec = g - b; prim = basis.red;   wPrim = r - g; }
		}

		Spectrum out;
		float lum = 0.f, nY = 0.f;
		for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
			const float lambda = sw.w[i];
			const float cy = SpectrumWavelengths::spd_ciey.Sample(lambda);
			// The basis is evaluated and the luminance matched over the
			// FULL drawn wavelength set — the material's spectral shape is
			// a property of the material, not of the path state. After a
			// dispersive collapse only the write is masked.
			float v = wW * SampleTable(basis.white, basis.n, basis.start, basis.end, lambda)
				+ wSec * SampleTable(sec, basis.n, basis.start, basis.end, lambda)
				+ wPrim * SampleTable(prim, basis.n, basis.start, basis.end, lambda);
			v = std::max(v, 0.f);
			lum += v * cy;
			nY += cy;
			out.c[i] = (sw.aliveMask & (1U << i)) ? v : 0.f;
		}

		// Normalize so the projected luminance (sum bins*ciey / sum ciey)
		// matches the input luminance. Note the CIE SPDs are pre-scaled by
		// 683*range/samples, so the raw sum must be divided by nY to keep
		// the bins in the same magnitude range as the RGB input.
		if (lum > 0.f && yTarget > 0.f && nY > 0.f) {
			const float s = (yTarget * nY) / lum;
			for (u_int i = 0; i < SPECTRAL_BINS; ++i)
				out.c[i] *= s;
		}

		return out;
	}

	// ---------------------------------------------------------------------
	// Jakob-Hanika 2019 sigmoid upsampling
	// ("A Low-Dimensional Function Space for Efficient Spectral
	// Upsampling", W. Jakob & J. Hanika, EGSR 2019).
	//
	// Port of rgb2spec_fetch()/rgb2spec_eval_precise() from
	// https://github.com/mitsuba-renderer/rgb2spec (BSD-3-Clause,
	// (c) 2020 Wenzel Jakob, commit 721145dedf2491851bd46ab8fd165955cb38ddaf)
	// against the embedded jh2019_32.h table. Keep in sync with the
	// identical device code in include/slg/spectral_funcs.cl.

	// Largest index i in [0, res-2] with scale[i] <= x (scale is the
	// ascending smoothstep^2 dominant-channel grid).
	inline u_int JH2019FindInterval(const float x) {
		u_int left = 0, size = jh2019::res - 2;
		while (size > 0) {
			const u_int half = size >> 1;
			const u_int middle = left + half + 1;
			if (jh2019::scale[middle] <= x) {
				left = middle;
				size -= half + 1;
			} else
				size = half;
		}
		return std::min(left, jh2019::res - 2);
	}

	// Dominant-channel trilinear coefficient lookup for an RGB triplet
	// already clamped to [0,1]^3 (rgb2spec_fetch).
	inline void JH2019Fetch(const float rgb[3], float out[3]) {
		const float r = rgb[0], g = rgb[1], b = rgb[2];

		// Monochromatic input: closed-form coefficient reproducing the
		// flat spectrum v (black/white saturate the sigmoid to 0/1).
		if (r == g && g == b) {
			out[0] = out[1] = 0.f;
			out[2] = (r <= 0.f) ? -8192.f :
				((r >= 1.f) ? 8192.f : (r - .5f) / sqrtf(r * (1.f - r)));
			return;
		}

		u_int i = 0;
		if (g >= r) i = 1;
		if (b >= rgb[i]) i = 2;

		const float z = rgb[i];
		if (z <= 0.f) {
			// Unreachable after the [0,1] clamp (a non-monochromatic
			// triplet has a positive max); kept as a guard, same as
			// rgb2spec_fetch returning the first node.
			for (u_int j = 0; j < 3; ++j)
				out[j] = jh2019::coeffs[j];
			return;
		}

		const float invZ = (jh2019::res - 1) / z;
		const float x = rgb[(i + 1) % 3] * invZ;
		const float y = rgb[(i + 2) % 3] * invZ;

		const u_int res = jh2019::res;
		const u_int xi = std::min((u_int)x, res - 2);
		const u_int yi = std::min((u_int)y, res - 2);
		const u_int zi = JH2019FindInterval(z);
		const u_int offset = (((i * res + zi) * res + yi) * res + xi) * 3;
		const u_int dx = 3, dy = 3 * res, dz = 3 * res * res;

		const float x1 = x - xi, x0 = 1.f - x1;
		const float y1 = y - yi, y0 = 1.f - y1;
		const float z1 = (z - jh2019::scale[zi]) /
			(jh2019::scale[zi + 1] - jh2019::scale[zi]);
		const float z0 = 1.f - z1;

		const float *data = jh2019::coeffs;
		u_int o = offset;
		for (u_int j = 0; j < 3; ++j, ++o) {
			out[j] = ((data[o] * x0 + data[o + dx] * x1) * y0 +
					(data[o + dy] * x0 + data[o + dy + dx] * x1) * y1) * z0 +
				((data[o + dz] * x0 + data[o + dz + dx] * x1) * y0 +
					(data[o + dz + dy] * x0 + data[o + dz + dy + dx] * x1) * y1) * z1;
		}
	}

	// sigmoid((c0*l + c1)*l + c2), l in nm (rgb2spec_eval_precise).
	inline float JH2019Eval(const float coeff[3], const float lambda) {
		const float x = (coeff[0] * lambda + coeff[1]) * lambda + coeff[2];
		return .5f * x / sqrtf(1.f + x * x) + .5f;
	}

	// JH2019 upsample over the drawn wavelength set. The sigmoid keeps
	// every bin in [0,1] by construction (energy conservation for
	// reflectance); out-of-gamut inputs are scaled to unit max before
	// the [0,1] fetch so the returned bins exceed 1 exactly like the
	// input RGB does. No luminance renorm: the table is optimized to
	// reproduce the input RGB under CIE E, and rescaling could push a
	// reflectance bin above 1.
	Spectrum UpsampleJH2019(const Spectrum &rgb, const PathWavelengths &sw) {
		float cn[3] = { rgb.c[0], rgb.c[1], rgb.c[2] };
		const float vmax = std::max(cn[0], std::max(cn[1], cn[2]));
		// !(vmax > 0) also catches NaN (NaN comparisons are false)
		if (!(vmax > 0.f))
			return Spectrum(0.f);
		const float norm = (vmax > 1.f) ? 1.f / vmax : 1.f;
		for (u_int j = 0; j < 3; ++j) {
			// NaN/negative -> 0, >1 -> 1. std::min/max alone would
			// propagate a single-channel NaN into the table index
			// (undefined); comparisons with NaN are false, so this form
			// always produces a clean [0,1] coordinate.
			const float cj = cn[j] * norm;
			cn[j] = (cj >= 0.f) ? std::min(cj, 1.f) : 0.f;
		}

		float coeff[3];
		JH2019Fetch(cn, coeff);
		const float outScale = 1.f / norm;

		Spectrum out;
		for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
			const float v = JH2019Eval(coeff, sw.w[i]) * outScale;
			out.c[i] = (sw.aliveMask & (1U << i)) ? v : 0.f;
		}
		return out;
	}
}

void Spectral::SetEnabled(const bool enabled) { g_enabled = enabled; }
bool Spectral::IsEnabled() { return g_enabled; }

void Spectral::SetUpsamplingModel(const UpsamplingModel model) {
	g_upsampling = model;
}
Spectral::UpsamplingModel Spectral::GetUpsamplingModel() { return g_upsampling; }

u_int Spectral::JH2019TableRes() { return jh2019::res; }
const float *Spectral::JH2019TableScale() { return jh2019::scale; }
const float *Spectral::JH2019TableCoeffs() { return jh2019::coeffs; }

void Spectral::SetPathWavelengths(const PathWavelengths &sw) {
	g_sw = sw;
	g_hasSW = true;
}
void Spectral::ClearPathWavelengths() { g_hasSW = false; }
const PathWavelengths *Spectral::Current() {
	return (g_enabled && g_hasSW) ? &g_sw : nullptr;
}

float Spectral::CollapseToHero() {
	if (!(g_enabled && g_hasSW))
		return 1.f;
	const u_int heroMask = 1u << g_sw.hero;
	if (g_sw.aliveMask == heroMask)
		return 1.f; // already collapsed
	g_sw.aliveMask = heroMask;
	// Uniform pick of the hero bin out of SPECTRAL_BINS: the surviving
	// estimate carries the whole path's spectral weight.
	return (float)SPECTRAL_BINS;
}

Spectrum Spectral::Reflectance(const Spectrum &rgb) {
	const PathWavelengths *sw = Current();
	return sw ? Reflectance(rgb, *sw) : rgb;
}
Spectrum Spectral::Emission(const Spectrum &rgb) {
	const PathWavelengths *sw = Current();
	return sw ? Emission(rgb, *sw) : rgb;
}
Spectrum Spectral::Reflectance(const Spectrum &rgb, const PathWavelengths &sw) {
	return (g_upsampling == UPSAMPLING_JH2019) ?
		UpsampleJH2019(rgb, sw) : Upsample(rgb, sw, reflBasis, rgb.Y());
}
Spectrum Spectral::Emission(const Spectrum &rgb, const PathWavelengths &sw) {
	return (g_upsampling == UPSAMPLING_JH2019) ?
		UpsampleJH2019(rgb, sw) : Upsample(rgb, sw, illumBasis, rgb.Y());
}

Spectrum Spectral::EvaluateSPD(const SPD &spd) {
	const PathWavelengths *sw = Current();
	return sw ? EvaluateSPD(spd, *sw) : Spectrum();
}
Spectrum Spectral::EvaluateSPD(const SPD &spd, const PathWavelengths &sw) {
	Spectrum out(0.f);
	for (u_int i = 0; i < SPECTRAL_BINS; ++i)
		if (sw.aliveMask & (1U << i))
			out.c[i] = spd.Sample(sw.w[i]);
	return out;
}

Spectrum Spectral::WithLuminance(const Spectrum &bins, const PathWavelengths &sw,
		const float yTarget) {
	float lum = 0.f, nY = 0.f;
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		const float cy = SpectrumWavelengths::spd_ciey.Sample(sw.w[i]);
		nY += cy; // full drawn set (dead bins are zeros, not missing samples)
		if (sw.aliveMask & (1U << i))
			lum += bins.c[i] * cy;
	}
	if (lum <= 0.f || yTarget <= 0.f || nY <= 0.f)
		return bins;
	return bins * (yTarget * nY / lum);
}

Spectrum Spectral::ProjectToRGB(const Spectrum &bins, const PathWavelengths &sw) {
	// Accumulate XYZ under the CIE matching functions at the live bins;
	// the white-point normalizer always spans the full drawn wavelength
	// set so a post-collapse single bin keeps its chromaticity.
	float X = 0.f, Y = 0.f, Z = 0.f;
	float nX = 0.f, nY = 0.f, nZ = 0.f;
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		const float lambda = sw.w[i];
		const float cx = SpectrumWavelengths::spd_ciex.Sample(lambda);
		const float cy = SpectrumWavelengths::spd_ciey.Sample(lambda);
		const float cz = SpectrumWavelengths::spd_ciez.Sample(lambda);
		nX += cx;
		nY += cy;
		nZ += cz;
		if (!(sw.aliveMask & (1U << i)))
			continue;
		X += bins.c[i] * cx;
		Y += bins.c[i] * cy;
		Z += bins.c[i] * cz;
	}
	if (nY <= 0.f)
		return Spectrum(0.f);

	// Normalize by the sampled white point so a flat spectrum reproduces its
	// value exactly (achromatic-invariant projection)
	const XYZColor whiteXYZ(nX, nY, nZ);
	const RGBColor whiteRGB = ColorSystem::DefaultColorSystem.ToRGB(whiteXYZ);
	const RGBColor rgb = ColorSystem::DefaultColorSystem.ToRGB(XYZColor(X, Y, Z));

	return Spectrum(
			(whiteRGB.c[0] != 0.f) ? rgb.c[0] / whiteRGB.c[0] : 0.f,
			(whiteRGB.c[1] != 0.f) ? rgb.c[1] / whiteRGB.c[1] : 0.f,
			(whiteRGB.c[2] != 0.f) ? rgb.c[2] / whiteRGB.c[2] : 0.f);
}

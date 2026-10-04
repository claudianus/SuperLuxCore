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
#include <iomanip>
#include <sstream>

#include "luxrays/core/color/spectral.h"
#include "luxrays/core/color/spectrumwavelengths.h"
#include "luxrays/core/color/spds/data/rgbE_32.h"
#include "luxrays/core/color/spds/data/rgbD65_32.h"
#include "luxrays/core/color/spds/data/jh2019_32.h"

using namespace luxrays;

namespace {
	Spectral::UpsamplingModel g_upsampling = Spectral::UPSAMPLING_SMITS;

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
	// Rec.709 luminance weights (RGBColor::Y()).
	const float kLum[3] = { 0.212671f, 0.715160f, 0.072169f };

	// XYZToRGB(CIE matching functions) at lambda: the film RGB response.
	inline void RGBResponse(const float lambda, float out[3]) {
		const float x = SpectrumWavelengths::spd_ciex.Sample(lambda);
		const float y = SpectrumWavelengths::spd_ciey.Sample(lambda);
		const float z = SpectrumWavelengths::spd_ciez.Sample(lambda);
		const float (&m)[3][3] = ColorSystem::DefaultColorSystem.XYZToRGB;
		for (u_int c = 0; c < 3; ++c)
			out[c] = m[c][0] * x + m[c][1] * y + m[c][2] * z;
	}

	// Constants of the unbiased hero-wavelength estimator, integrated once
	// over the sampled range [SPECTRAL_START, SPECTRAL_END]:
	//  projK[c] = SPECTRAL_BIN_WIDTH / integral of the channel-c response:
	//            bin i is drawn uniformly over its stratum (pdf 1/width), so
	//            sum_i response_c(w_i) * L(w_i) * projK[c] has expectation
	//            integral(response_c * L) / integral(response_c) - a flat
	//            spectrum projects to exactly its value in expectation.
	//  yRefl[b] = expected film luminance of each Smits basis
	//            spectrum, so upsampled RGB keeps its luminance with a
	//            constant (per-RGB) scale instead of a per-sample one.
	struct Normalization {
		float projK[3];
		float yRefl[7];
	};

	inline const float *BasisTable(const BasisSet &b, const u_int i) {
		switch (i) {
			case 0: return b.white;
			case 1: return b.cyan;
			case 2: return b.magenta;
			case 3: return b.yellow;
			case 4: return b.red;
			case 5: return b.green;
			default: return b.blue;
		}
	}

	const Normalization &Norm() {
		static const Normalization norm = [] {
			Normalization n;
			const double step = 0.25;
			const u_int steps = (u_int)((SPECTRAL_END - SPECTRAL_START) / step);
			double white[3] = { 0.0, 0.0, 0.0 };
			double refl[7][3] = {};
			for (u_int k = 0; k < steps; ++k) {
				const float lambda = (float)(SPECTRAL_START + (k + 0.5) * step);
				float r[3];
				RGBResponse(lambda, r);
				for (u_int c = 0; c < 3; ++c) {
					white[c] += r[c] * step;
					for (u_int b = 0; b < 7; ++b) {
						refl[b][c] += r[c] * step * SampleTable(BasisTable(reflBasis, b),
								reflBasis.n, reflBasis.start, reflBasis.end, lambda);
					}
				}
			}
			for (u_int c = 0; c < 3; ++c)
				n.projK[c] = (white[c] != 0.0) ? (float)(SPECTRAL_BIN_WIDTH / white[c]) : 0.f;
			for (u_int b = 0; b < 7; ++b) {
				double yr = 0.0;
				for (u_int c = 0; c < 3; ++c)
					yr += kLum[c] * refl[b][c] / white[c];
				n.yRefl[b] = (float)yr;
			}
			return n;
		}();
		return norm;
	}

	// Expected film luminance of one basis table under the projector.
	inline float BasisY(const BasisSet &basis, const float *table) {
		const Normalization &n = Norm();
		for (u_int b = 0; b < 7; ++b)
			if (BasisTable(basis, b) == table)
				return n.yRefl[b];
		return 0.f;
	}

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
		for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
			const float lambda = sw.w[i];
			float v = wW * SampleTable(basis.white, basis.n, basis.start, basis.end, lambda)
				+ wSec * SampleTable(sec, basis.n, basis.start, basis.end, lambda)
				+ wPrim * SampleTable(prim, basis.n, basis.start, basis.end, lambda);
			v = std::max(v, 0.f);
			out.c[i] = (sw.aliveMask & (1U << i)) ? v : 0.f;
		}

		// Match the input luminance with a constant of this RGB: the
		// expected film luminance of the decomposition is linear in the
		// basis weights. (Normalizing by the luminance of the 3 drawn
		// wavelengths instead made the scale wavelength-dependent and
		// biased saturated colors - pure red rendered ~+50% too bright.)
		const float yExp = wW * BasisY(basis, basis.white) +
				wSec * BasisY(basis, sec) + wPrim * BasisY(basis, prim);
		if (yExp > 0.f && yTarget > 0.f) {
			const float s = yTarget / yExp;
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



void Spectral::SetUpsamplingModel(const UpsamplingModel model) {
	g_upsampling = model;
}
Spectral::UpsamplingModel Spectral::GetUpsamplingModel() { return g_upsampling; }

u_int Spectral::JH2019TableRes() { return jh2019::res; }
const float *Spectral::JH2019TableScale() { return jh2019::scale; }
const float *Spectral::JH2019TableCoeffs() { return jh2019::coeffs; }

float Spectral::CollapseToHero() {
	if (!(detail::g_spectralEnabled && detail::g_hasSW))
		return 1.f;
	const u_int heroMask = 1u << detail::g_sw.hero;
	if (detail::g_sw.aliveMask == heroMask)
		return 1.f; // already collapsed
	detail::g_sw.aliveMask = heroMask;
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
	// The film projection is white-normalized to the flat (CIE E) spectrum,
	// so emitted colors use the E-relative Smits basis too. The D65
	// illuminant basis (illumrgb2spect_*) projects its own white to a blue
	// cast (0.88, 1.13, 1.17) here and desaturated every colored light.
	return (g_upsampling == UPSAMPLING_JH2019) ?
		UpsampleJH2019(rgb, sw) : Upsample(rgb, sw, reflBasis, rgb.Y());
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

float Spectral::ExpectedLuminance(const SPD &spd) {
	// 5 nm midpoint rule: the same quadrature as the GPU kernel
	// (Spectral_PlanckExpectedY) so CPU/GPU agree.
	const Normalization &n = Norm();
	const float step = 5.f;
	const u_int steps = (u_int)((SPECTRAL_END - SPECTRAL_START) / step);
	float y = 0.f;
	for (u_int k = 0; k < steps; ++k) {
		const float lambda = SPECTRAL_START + (k + 0.5f) * step;
		float r[3];
		RGBResponse(lambda, r);
		const float g = (kLum[0] * r[0] * n.projK[0] + kLum[1] * r[1] * n.projK[1] +
				kLum[2] * r[2] * n.projK[2]) / SPECTRAL_BIN_WIDTH;
		y += g * spd.Sample(lambda) * step;
	}
	return y;
}

Spectrum Spectral::WithLuminance(const Spectrum &bins, const SPD &spd,
		const float yTarget) {
	const float y = ExpectedLuminance(spd);
	if (!(y > 0.f) || !(yTarget > 0.f))
		return bins;
	return bins * (yTarget / y);
}

void Spectral::PrepareRGBProjection(const PathWavelengths &sw, RGBProjector &p) {
	const Normalization &n = Norm();
	p.aliveMask = sw.aliveMask;
	p.valid = true;
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		float r[3];
		RGBResponse(sw.w[i], r);
		p.cr[i] = r[0] * n.projK[0];
		p.cg[i] = r[1] * n.projK[1];
		p.cb[i] = r[2] * n.projK[2];
	}
}

Spectrum Spectral::ProjectToRGB(const Spectrum &bins, const RGBProjector &p) {
	if (!p.valid || bins.Black())
		return Spectrum(0.f);

	// Unbiased projection with a control variate on the mean bin value:
	// rgb = mean + sum_i coeff_i * (bin_i - mean). The coefficients of a
	// flat spectrum sum to 1 in expectation, so the control term only
	// removes noise; for a flat spectrum it is exact in every sample.
	// Dead bins (after a hero collapse) hold 0 and still enter the sum.
	float v[SPECTRAL_BINS];
	float mean = 0.f;
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		v[i] = (p.aliveMask & (1U << i)) ? bins.c[i] : 0.f;
		mean += v[i];
	}
	mean *= 1.f / SPECTRAL_BINS;

	float r = mean, g = mean, b = mean;
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		const float d = v[i] - mean;
		r += d * p.cr[i];
		g += d * p.cg[i];
		b += d * p.cb[i];
	}

	return Spectrum(r, g, b);
}

Spectrum Spectral::ProjectToRGB(const Spectrum &bins, const PathWavelengths &sw) {
	RGBProjector p;
	PrepareRGBProjection(sw, p);
	return ProjectToRGB(bins, p);
}

std::vector<std::string> Spectral::KernelDefines() {
	const Normalization &n = Norm();
	std::vector<std::string> defs;
	auto add = [&defs](const std::string &name, const float v) {
		std::ostringstream o;
		o.imbue(std::locale::classic());
		o << "-D " << name << "=" << std::scientific << std::setprecision(9) << v << "f";
		defs.push_back(o.str());
	};
	add("SLG_SPECTRAL_PROJ_KR", n.projK[0]);
	add("SLG_SPECTRAL_PROJ_KG", n.projK[1]);
	add("SLG_SPECTRAL_PROJ_KB", n.projK[2]);
	static const char *names[7] = { "WHITE", "CYAN", "MAGENTA", "YELLOW", "RED", "GREEN", "BLUE" };
	for (u_int b = 0; b < 7; ++b) {
		add(std::string("SLG_SPECTRAL_YREFL_") + names[b], n.yRefl[b]);
	}
	return defs;
}

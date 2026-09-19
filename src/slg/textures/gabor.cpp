/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <cmath>

#include "luxrays/core/randomgen.h"

#include "slg/textures/gabor.h"
#include "slg/textures/whitenoise.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Gabor noise texture — see gabor.h for references
//------------------------------------------------------------------------------

// Hann-windowed Gaussian envelope x phasor (Eq. 6 of Lagae 2009 with the
// Hann window from Tavernier 2019). Support radius is 1.
void GaborNoiseTexture::Kernel(const float px, const float py,
		const float freq, const float orient, float &re, float &im) {
	const float distSqr = px * px + py * py;
	if (distSqr >= 1.f) {
		re = 0.f;
		im = 0.f;
		return;
	}

	// Gaussian envelope exp(-pi * r^2) windowed by a Hann window so the
	// truncation at r = 1 stays C1 continuous
	const float hann = .5f + .5f * cosf(M_PI * distSqr);
	const float envelope = expf(-M_PI * distSqr) * hann;

	const float dirX = cosf(orient), dirY = sinf(orient);
	const float angle = 2.f * M_PI * freq * (px * dirX + py * dirY);
	re = envelope * cosf(angle);
	im = envelope * sinf(angle);
}

void GaborNoiseTexture::EvalPhasor(const float x, const float y,
		const float freq, const float isotropy, const float orient,
		float &re, float &im) {
	const int cellX = Floor2Int(x), cellY = Floor2Int(y);
	const float localX = x - cellX, localY = y - cellY;

	float sumRe = 0.f, sumIm = 0.f;
	for (int j = -1; j <= 1; ++j) {
		for (int i = -1; i <= 1; ++i) {
			const int cx = cellX + i, cy = cellY + j;
			const float px = localX - i, py = localY - j;

			// One RNG stream per cell drives the whole impulse schedule,
			// so CPU and GPU agree bit-for-bit
			TauswortheRandomGenerator rnd(
					WhiteNoiseTexture::SeedFromVector(cx, cy, 0));

			for (u_int k = 0; k < IMPULSES_PER_CELL; ++k) {
				const float ix = px - rnd.floatValue();
				const float iy = py - rnd.floatValue();
				// Random sign weight (paper: w in {-1, +1})
				const float w = rnd.floatValue() < .5f ? -1.f : 1.f;
				// Isotropy blends the fixed base orientation with a
				// per-impulse random one
				const float o = isotropy >= 1.f ? orient :
						orient + (1.f - isotropy) * M_PI *
						(2.f * rnd.floatValue() - 1.f);

				float kRe, kIm;
				Kernel(ix, iy, freq, o, kRe, kIm);
				sumRe += w * kRe;
				sumIm += w * kIm;
			}
		}
	}
	re = sumRe;
	im = sumIm;
}

GaborNoiseTexture::GaborNoiseTexture(TextureRef v, const float s,
		const float f, const float i, const float o, const GaborOutput out) :
		vec(v), scale(s), frequency(f), isotropy(i), orientation(o),
		output(out), sigmaInv(SigmaInv(f)) { }

GaborNoiseTexture::GaborNoiseTexture(TextureRef v, const float s,
		const float f, const float i, const float o, const GaborOutput out,
		const float si) :
		vec(v), scale(s), frequency(f), isotropy(i), orientation(o),
		output(out), sigmaInv(si) { }

// Numeric 2nd moment of the kernel: integral of envelope^2 * cos^2(angle)
// over the unit disk, quadrature with 48x48 samples. sigma^2 of the noise
// is impulses-in-range * kernelVar; the impulse density is
// IMPULSES_PER_CELL per unit cell and the kernel support is the unit disk,
// so a point sees IMPULSES_PER_CELL * pi impulses on average.
float GaborNoiseTexture::SigmaInv(const float freq) {
	const int N = 48;
	double acc = 0.;
	for (int j = 0; j < N; ++j) {
		for (int i = 0; i < N; ++i) {
			const float x = (i + .5f) / N * 2.f - 1.f;
			const float y = (j + .5f) / N * 2.f - 1.f;
			float re, im;
			GaborNoiseTexture::Kernel(x, y, freq, 0.f, re, im);
			acc += re * re + im * im;
		}
	}
	const double cellArea = 4. / (N * N);
	const double kernelVar = acc * cellArea;
	const double sigma2 = GaborNoiseTexture::IMPULSES_PER_CELL * M_PI *
			kernelVar;
	return sigma2 > 0. ? static_cast<float>(1. / sqrt(sigma2)) : 1.f;
}

float GaborNoiseTexture::GetFloatValue(const HitPoint &hitPoint) const {
	const Spectrum p = vec.get().GetSpectrumValue(hitPoint);
	const float x = p.c[0] * scale, y = p.c[1] * scale;

	float re, im;
	EvalPhasor(x, y, frequency, isotropy, orientation, re, im);

	switch (output) {
		case GABOR_PHASE:
			return atan2f(im, re) * (1.f / (2.f * M_PI)) + .5f;
		case GABOR_INTENSITY:
			return Clamp(sqrtf(re * re + im * im) * sigmaInv * .5f, 0.f, 1.f);
		default:
			// Roughly N(0,1) -> [0,1] display range
			return Clamp(.5f + re * sigmaInv * .5f, 0.f, 1.f);
	}
}

Spectrum GaborNoiseTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	return Spectrum(GetFloatValue(hitPoint));
}

PropertiesUPtr GaborNoiseTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("gabornoise"));
	props->Set(Property("scene.textures." + name + ".vector")(vec.get().GetSDLValue()));
	props->Set(Property("scene.textures." + name + ".scale")(scale));
	props->Set(Property("scene.textures." + name + ".frequency")(frequency));
	props->Set(Property("scene.textures." + name + ".isotropy")(isotropy));
	props->Set(Property("scene.textures." + name + ".orientation")(orientation));
	const char *out = output == GABOR_PHASE ? "phase" :
			(output == GABOR_INTENSITY ? "intensity" : "value");
	props->Set(Property("scene.textures." + name + ".output")(out));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

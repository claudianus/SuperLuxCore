/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include "luxrays/core/color/spds/blackbodyspd.h"

#include "slg/textures/blackbody.h"
#include "slg/textures/blackbodylut.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Black body texture
//
// The temperature is a texture input so a per-point field (e.g. a density
// grid's temperature channel) can drive the emission color. A constant
// .temperature parses to an implicit ConstFloatTexture, so the two
// constructors below share the LUT eval path: the scalar ctor simply skips
// storing a texture reference and takes its temperature from the member.
//
// The RGB value comes from a normalized TemperatureToWhitePoint LUT (the
// full Planck->XYZ->RGB integral is too expensive to run per point); the
// spectral path still evaluates the exact Planckian SPD per wavelength.
//------------------------------------------------------------------------------

static Spectrum BlackBodyLutRGB(const float temperature, const bool normalize) {
	const float t = Clamp(temperature, SLG_BLACKBODY_LUT_TMIN, SLG_BLACKBODY_LUT_TMAX);
	const float x = (t - SLG_BLACKBODY_LUT_TMIN) *
			(SLG_BLACKBODY_LUT_N - 1) / (SLG_BLACKBODY_LUT_TMAX - SLG_BLACKBODY_LUT_TMIN);
	const int i = Min((int)x, SLG_BLACKBODY_LUT_N - 2);
	const float f = x - i;

	const float *a = &kBlackBodyRgbLut[i * 3];
	const float *b = &kBlackBodyRgbLut[(i + 1) * 3];
	const float scale = normalize ? 1.f : SLG_BLACKBODY_RAW_SCALE;
	return Spectrum(
			Lerp(f, a[0], b[0]) * scale,
			Lerp(f, a[1], b[1]) * scale,
			Lerp(f, a[2], b[2]) * scale);
}

BlackBodyTexture::BlackBodyTexture(const float temp, const bool norm) :
		temperatureTex(nullptr), temperature(temp), normalize(norm) {
	// Constant temperature: keep the exact integral (not the LUT) so existing
	// scenes render byte-identically to before this feature.
	rgb = TemperatureToWhitePoint(temperature, norm);
}

BlackBodyTexture::BlackBodyTexture(TextureConstRef tempTex, const float nominalTemp,
		const bool norm) :
		temperatureTex(&tempTex), temperature(nominalTemp), normalize(norm) {
	// rgb is the representative value at the nominal temperature (importance
	// hints, GetRGB()); the per-point eval uses temperatureTex.
	rgb = BlackBodyLutRGB(temperature, normalize);
}

Spectrum BlackBodyTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	if (!temperatureTex)
		return rgb;

	return BlackBodyLutRGB(temperatureTex->GetFloatValue(hitPoint), normalize);
}

// Evaluate the true Planckian SPD at the path wavelengths (instead of an RGB
// upsample), normalized to the same luminance as the RGB fallback.
Spectrum BlackBodyTexture::EvalSpectralValue(const HitPoint &hitPoint,
		const PathWavelengths &sw, const bool emission) const {
	const float temp = temperatureTex ?
			temperatureTex->GetFloatValue(hitPoint) : temperature;
	const Spectrum ref = temperatureTex ?
			BlackBodyLutRGB(temp, normalize) : rgb;
	// VDBs frequently evaluate the same temperature many times along a ray.
	// Keep one exact SPD/integral per worker, without quantizing temperature
	// or retaining an unbounded cache for spatially varying fields.
	struct CachedBlackbody {
		float temperature = 0.f;
		std::unique_ptr<BlackbodySPD> spd;
		float luminance = 0.f;
	};
	thread_local CachedBlackbody cached;
	if (!cached.spd || (cached.temperature != temp)) {
		auto spd = std::make_unique<BlackbodySPD>(temp);
		const float luminance = Spectral::ExpectedLuminance(*spd);
		cached.spd = std::move(spd);
		cached.temperature = temp;
		cached.luminance = luminance;
	}
	const Spectrum bins = Spectral::EvaluateSPD(*cached.spd, sw);
	const float yTarget = ref.Y();
	if ((cached.luminance > 0.f) && (yTarget > 0.f))
		return bins * (yTarget / cached.luminance);
	return bins;
}

PropertiesUPtr BlackBodyTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("blackbody"));
	if (temperatureTex)
		props->Set(Property("scene.textures." + name + ".temperature")(temperatureTex->GetSDLValue()));
	else
		props->Set(Property("scene.textures." + name + ".temperature")(temperature));
	props->Set(Property("scene.textures." + name + ".normalize")(normalize));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

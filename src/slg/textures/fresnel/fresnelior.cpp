/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include "slg/textures/fresnel/fresnelior.h"
#include "slg/bsdf/hitpoint.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Fresnel IOR texture
//------------------------------------------------------------------------------

float FresnelIorTexture::GetFloatValue(const HitPoint &hitPoint) const {
	// Incident cosine at the shading point: fixedDir points back along the
	// incoming ray, shadeN is already oriented toward the incident side by
	// HitPoint::Init so this dot stays positive for camera-facing hits.
	const float cosi = Clamp(Dot(-hitPoint.fixedDir, hitPoint.shadeN), -1.f, 1.f);
	return CauchyEvaluate(eta, cosi);
}

Spectrum FresnelIorTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const float f = GetFloatValue(hitPoint);
	return Spectrum(f, f, f);
}

float FresnelIorTexture::Y() const {
	// Constant-power sink for the generic texture filters: the value is
	// angle dependent so there is no single Y - report the normal-incidence
	// reflectance which is the dominant term.
	const float cosi = 1.f;
	return CauchyEvaluate(eta, cosi);
}

float FresnelIorTexture::Filter() const {
	// Angle-dependent - no meaningful spatial average; neutral weight.
	return 1.f;
}

Spectrum FresnelIorTexture::Evaluate(const HitPoint &hitPoint, const float cosi) const {
	// BSDF-driven eval with the caller's cosine (IOR convention: eta is the
	// *relative* index nt/ni inside CauchyEvaluate).
	const float f = CauchyEvaluate(eta, cosi);
	return Spectrum(f, f, f);
}

PropertiesUPtr FresnelIorTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("fresnelior"));
	props->Set(Property("scene.textures." + name + ".ior")(eta));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

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

#include "slg/textures/fresnel/fresnelfacing.h"
#include "slg/bsdf/hitpoint.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Facing texture: pow(1 - |cosi|, blend) over the hit incident angle
//------------------------------------------------------------------------------

float FacingTexture::GetFloatValue(const HitPoint &hitPoint) const {
	// fixedDir points back along the incoming ray; shadeN is oriented toward
	// the incident side by HitPoint::Init.
	const float cosi = Clamp(fabsf(Dot(-hitPoint.fixedDir, hitPoint.shadeN)), 0.f, 1.f);
	return powf(1.f - cosi, blend);
}

Spectrum FacingTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const float f = GetFloatValue(hitPoint);
	return Spectrum(f, f, f);
}

PropertiesUPtr FacingTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("facing"));
	props->Set(Property("scene.textures." + name + ".blend")(blend));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
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

#include "slg/textures/math/subtract.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Subtract texture
//------------------------------------------------------------------------------

float SubtractTexture::GetFloatValue(const HitPoint &hitPoint) const {
	return GetTexture1().GetFloatValue(hitPoint) - GetTexture2().GetFloatValue(hitPoint);
}

Spectrum SubtractTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	return GetTexture1().GetSpectrumValue(hitPoint) - GetTexture2().GetSpectrumValue(hitPoint);
}

Normal SubtractTexture::Bump(const HitPoint &hitPoint, const float sampleDistance) const {
	// 높이 기울기를 뺀다. 합성 원리는 AddTexture::Bump와 같다.
	const Vector u = Normalize(hitPoint.dpdu);
	const Vector v = Normalize(Cross(Vector(hitPoint.shadeN), hitPoint.dpdu));
	float du1, dv1, du2, dv2;
	BumpNormalToSlopes(hitPoint, GetTexture1().Bump(hitPoint, sampleDistance), u, v, du1, dv1);
	BumpNormalToSlopes(hitPoint, GetTexture2().Bump(hitPoint, sampleDistance), u, v, du2, dv2);

	return Normal(Normalize(Vector(hitPoint.shadeN) + (du1 - du2) * u + (dv1 - dv2) * v));
}

PropertiesUPtr SubtractTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();
	
	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("subtract"));
	props->Set(Property("scene.textures." + name + ".texture1")(GetTexture1().GetSDLValue()));
	props->Set(Property("scene.textures." + name + ".texture2")(GetTexture2().GetSDLValue()));
	
	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

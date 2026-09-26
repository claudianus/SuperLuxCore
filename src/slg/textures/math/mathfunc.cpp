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

#include <cmath>

#include "slg/textures/math/mathfunc.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Generic math-function texture (trig/exp/log)
//------------------------------------------------------------------------------

static float ApplyFunc(MathFuncOp op, const float a, const float b) {
	switch (op) {
		case MATHFUNC_SIN: return sinf(a);
		case MATHFUNC_COS: return cosf(a);
		case MATHFUNC_TAN: return tanf(a);
		// Clamp asin/acos inputs to their domain so out-of-range shading
		// values degrade to the boundary instead of producing NaN
		case MATHFUNC_ASIN: return asinf(Clamp(a, -1.f, 1.f));
		case MATHFUNC_ACOS: return acosf(Clamp(a, -1.f, 1.f));
		case MATHFUNC_ATAN: return atanf(a);
		case MATHFUNC_ATAN2: return atan2f(a, b);
		case MATHFUNC_EXP: return expf(a);
		case MATHFUNC_LN: return logf(Max(a, 1e-9f));
		case MATHFUNC_SINH: return sinhf(a);
		case MATHFUNC_COSH: return coshf(a);
		case MATHFUNC_TANH: return tanhf(a);
		case MATHFUNC_INVSQRT: return 1.f / sqrtf(Max(a, 1e-9f));
		// Floored modulo (Blender FLOORMOD); guard against mod 0
		case MATHFUNC_FLOORMOD: return (b == 0.f) ? 0.f :
				a - b * floorf(a / b);
		default:
			return 0.f;
	}
}

float MathFuncTexture::GetFloatValue(const HitPoint &hitPoint) const {
	return ApplyFunc(op, tex1.get().GetFloatValue(hitPoint),
			MathFuncIsBinary(op) ? tex2.get().GetFloatValue(hitPoint) : 0.f);
}

Spectrum MathFuncTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const Spectrum a = tex1.get().GetSpectrumValue(hitPoint);
	const Spectrum b = MathFuncIsBinary(op) ?
			tex2.get().GetSpectrumValue(hitPoint) : Spectrum(0.f);

	Spectrum result;
	for (u_int i = 0; i < COLOR_SAMPLES; ++i)
		result.c[i] = ApplyFunc(op, a.c[i], b.c[i]);
	return result;
}

const char *MathFuncTexture::OpToString(MathFuncOp o) {
	switch (o) {
		case MATHFUNC_SIN: return "sin";
		case MATHFUNC_COS: return "cos";
		case MATHFUNC_TAN: return "tan";
		case MATHFUNC_ASIN: return "asin";
		case MATHFUNC_ACOS: return "acos";
		case MATHFUNC_ATAN: return "atan";
		case MATHFUNC_ATAN2: return "atan2";
		case MATHFUNC_EXP: return "exp";
		case MATHFUNC_LN: return "ln";
		case MATHFUNC_SINH: return "sinh";
		case MATHFUNC_COSH: return "cosh";
		case MATHFUNC_TANH: return "tanh";
		case MATHFUNC_INVSQRT: return "invsqrt";
		case MATHFUNC_FLOORMOD: return "floormod";
		default: return "sin";
	}
}

PropertiesUPtr MathFuncTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("mathfunc"));
	props->Set(Property("scene.textures." + name + ".op")(OpToString(op)));
	props->Set(Property("scene.textures." + name + ".texture1")(tex1.get().GetSDLValue()));
	if (MathFuncIsBinary(op))
		props->Set(Property("scene.textures." + name + ".texture2")(tex2.get().GetSDLValue()));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

// SPDX-License-Identifier: Apache-2.0
#include "slg/textures/vectormath/normalvector.h"
#include "luxrays/core/color/spectral.h"

using namespace luxrays;
using namespace slg;

float NormalVectorTexture::GetFloatValue(const HitPoint &hitPoint) const {
	return EvalSpectrumValue(hitPoint).Y();
}

Spectrum NormalVectorTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const Normal normal = EvaluateNormal(hitPoint, .001f);
	return Spectrum(&normal.x);
}

Normal NormalVectorTexture::Bump(const HitPoint &hitPoint, const float sampleDistance) const {
	const Normal value = EvaluateNormal(hitPoint, sampleDistance);
	return Dot(hitPoint.fixedDir, Vector(hitPoint.geometryN)) < 0.f ? -value : value;
}

Normal NormalVectorTexture::EvaluateNormal(const HitPoint &hitPoint, const float sampleDistance) const {
	const Spectral::ScopePause pause;
	const float side = Dot(hitPoint.fixedDir, Vector(hitPoint.geometryN)) < 0.f ? -1.f : 1.f;
	Spectrum value;
	if (sourceBump) {
		const Normal normal = GetTexture().Bump(hitPoint, sampleDistance);
		const Normal facing = side * normal;
		value = Spectrum(&facing.x);
	} else
		value = GetTexture().GetSpectrumValue(hitPoint);
	const Vector vector(value.c);
	const float length2 = Dot(vector, vector);
	return std::isfinite(length2) && length2 > 0.f ?
			Normal(vector / sqrtf(length2)) : side * hitPoint.shadeN;
}

PropertiesUPtr NormalVectorTexture::ToProperties(const ImageMapCache &cache,
		const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();
	const std::string prefix = "scene.textures." + GetName();
	props->Set(Property(prefix + ".type")("normalvector"));
	props->Set(Property(prefix + ".texture")(GetTexture().GetSDLValue()));
	props->Set(Property(prefix + ".sourcebump")(sourceBump));
	return props;
}

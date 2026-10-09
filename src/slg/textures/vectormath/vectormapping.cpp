// SPDX-License-Identifier: Apache-2.0
#include "slg/textures/vectormath/vectormapping.h"
#include "luxrays/core/color/spectral.h"

using namespace luxrays;
using namespace slg;

float VectorMappingTexture::GetFloatValue(const HitPoint &hitPoint) const {
	return EvalSpectrumValue(hitPoint).Y();
}

Spectrum VectorMappingTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const Spectral::ScopePause pause;
	const Spectrum value = GetInput(0).GetSpectrumValue(hitPoint);
	const Spectrum location = GetInput(1).GetSpectrumValue(hitPoint);
	const Spectrum rotation = GetInput(2).GetSpectrumValue(hitPoint);
	const Spectrum scale = GetInput(3).GetSpectrumValue(hitPoint);
	const float cx = cosf(rotation.c[0]), cy = cosf(rotation.c[1]), cz = cosf(rotation.c[2]);
	const float sx = sinf(rotation.c[0]), sy = sinf(rotation.c[1]), sz = sinf(rotation.c[2]);
	const float matrix[3][3] = {
		{cy * cz, cz * sx * sy - cx * sz, sx * sz + cx * cz * sy},
		{cy * sz, cx * cz + sx * sy * sz, cx * sy * sz - cz * sx},
		{-sy, cy * sx, cx * cy}
	};
	float v[3];
	for (u_int i = 0; i < 3; ++i)
		v[i] = mode == 1 ? value.c[i] - location.c[i] :
				mode == 3 ? (scale.c[i] != 0.f ? value.c[i] / scale.c[i] : 0.f) :
				value.c[i] * scale.c[i];
	float result[3] = {0.f, 0.f, 0.f};
	for (u_int i = 0; i < 3; ++i) {
		for (u_int j = 0; j < 3; ++j)
			result[i] += (mode == 1 ? matrix[j][i] : matrix[i][j]) * v[j];
		if (mode == 0) result[i] += location.c[i];
		else if (mode == 1) result[i] = scale.c[i] != 0.f ? result[i] / scale.c[i] : 0.f;
	}
	if (mode == 3) {
		const float length = sqrtf(result[0] * result[0] + result[1] * result[1] + result[2] * result[2]);
		for (u_int i = 0; i < 3; ++i) result[i] = length > 0.f ? result[i] / length : 0.f;
	}
	return Spectrum(result[0], result[1], result[2]);
}

PropertiesUPtr VectorMappingTexture::ToProperties(const ImageMapCache &cache,
		const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();
	const std::string prefix = "scene.textures." + GetName();
	props->Set(Property(prefix + ".type")("vectormapping"));
	const char *keys[] = {"vector", "location", "rotation", "scale"};
	for (u_int i = 0; i < 4; ++i)
		props->Set(Property(prefix + "." + keys[i])(GetInput(i).GetSDLValue()));
	props->Set(Property(prefix + ".mode")(mode));
	return props;
}

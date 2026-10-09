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

#ifndef _SLG_WHITENOISETEX_H
#define	_SLG_WHITENOISETEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// White noise texture
//
// Deterministic per-seed spatial white noise (Cycles "White Noise" node). The
// input is a 3D seed (usually the shading position): each distinct seed maps
// to an uncorrelated pseudo-random value in [0, 1). The same seed always maps
// to the same value.
//------------------------------------------------------------------------------

class WhiteNoiseTexture : public Texture {
public:
	WhiteNoiseTexture(TextureRef t, const u_int o) : tex(t), seedOffset(o) { }
	virtual ~WhiteNoiseTexture() { }

	virtual TextureType GetType() const { return WHITENOISE_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const { return luxrays::Spectrum(.5f).Y(); }
	virtual float Filter() const { return .5f; }

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
		Texture::AddReferencedTextures(referencedTexs);

		GetTexture().AddReferencedTextures(referencedTexs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap * > &referencedImgMaps) const {
		GetTexture().AddReferencedImageMaps(referencedImgMaps);
	}

	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) override {
		updtex(tex, oldTex, newTex);
	}

	TextureConstRef GetTexture() const { return tex; }
	u_int GetSeedOffset() const { return seedOffset; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	// Reinterpret a float's bit pattern as an integer. Deterministic for all
	// float values (negatives, huge coords, inf) and bit-identical to the
	// OpenCL as_uint(); arithmetic float->uint casts are undefined for
	// negative inputs and diverge between CPU and GPU.
	static u_int FloatBits(const float v) {
		union { float f; u_int i; } b;
		b.f = v;
		return b.i;
	}

	// Spatial hash of a 3-component seed -> seed for the RNG. Bit-mixes the
	// component bit patterns then applies an integer avalanche so nearby
	// positions (correlated mantissa bits) still decorrelate. Shared by the
	// CPU and OpenCL implementations.
	static u_int SeedFromVector(const float x, const float y, const float z) {
		u_int h = FloatBits(x) * 0x85ebca6bu ^ FloatBits(y) * 0xc2b2ae35u ^
				FloatBits(z) * 0x27d4eb2fu;
		h ^= h >> 16u; h *= 0x85ebca6bu;
		h ^= h >> 13u; h *= 0xc2b2ae35u;
		h ^= h >> 16u;
		return h;
	}

private:
	std::reference_wrapper<Texture> tex;

	const u_int seedOffset;
};

}

#endif	/* _SLG_WHITENOISETEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

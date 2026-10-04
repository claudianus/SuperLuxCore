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

#ifndef _SLG_CYCLESNOISETEX_H
#define	_SLG_CYCLESNOISETEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// Cycles "Noise Texture" node, ported bit-for-bit from the Cycles kernel
// (kernel/svm/noise.h, fractal_noise.h, noisetex.h): Jenkins lookup3 hashed
// Perlin noise with the OSL range scales, the five fractal types, the
// FBM normalize option, distortion and the Color output seeds.
//
// The coordinates come from a vector texture (Cycles' Vector input; the
// exporter feeds Generated coordinates when it is unlinked). Every float
// input is a texture so linked sockets are honored.
//------------------------------------------------------------------------------

typedef enum {
	CYCLESNOISE_FBM = 0,
	CYCLESNOISE_MULTIFRACTAL,
	CYCLESNOISE_HYBRID_MULTIFRACTAL,
	CYCLESNOISE_RIDGED_MULTIFRACTAL,
	CYCLESNOISE_HETERO_TERRAIN
} CyclesNoiseType;

class CyclesNoiseTexture : public Texture {
public:
	CyclesNoiseTexture(TextureRef vec, TextureRef w, TextureRef scale,
			TextureRef detail, TextureRef roughness, TextureRef lacunarity,
			TextureRef offset, TextureRef gain, TextureRef distortion,
			const CyclesNoiseType noiseType, const u_int dimensions,
			const bool normalize, const bool colorOutput, const bool isColor);
	virtual ~CyclesNoiseTexture() { }

	virtual TextureType GetType() const { return CYCLESNOISE_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	// The Color output is authored RGB only when it feeds a color input;
	// as a vector (e.g. a distortion offset) it must pass through raw
	virtual luxrays::Spectrum EvalSpectralValue(const HitPoint &hitPoint,
			const luxrays::PathWavelengths &sw, const bool emission) const;
	virtual float Y() const { return .5f; }
	virtual float Filter() const { return .5f; }

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const;
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap * > &referencedImgMaps) const;
	virtual void UpdateTextureReferences(TextureRef oldTex, TextureRef newTex);

	TextureConstRef GetVec() const { return vec; }
	TextureConstRef GetW() const { return w; }
	TextureConstRef GetScale() const { return scale; }
	TextureConstRef GetDetail() const { return detail; }
	TextureConstRef GetRoughness() const { return roughness; }
	TextureConstRef GetLacunarity() const { return lacunarity; }
	TextureConstRef GetOffset() const { return offset; }
	TextureConstRef GetGain() const { return gain; }
	TextureConstRef GetDistortion() const { return distortion; }
	CyclesNoiseType GetNoiseType() const { return noiseType; }
	u_int GetDimensions() const { return dimensions; }
	bool GetNormalize() const { return normalize; }
	bool IsColorOutput() const { return colorOutput; }
	bool IsColor() const { return isColor; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	// Evaluates the node at the given (unscaled) inputs: value and, when
	// requested, the Color output.
	static void Evaluate(const float p[4], const float scale,
			const float detail, const float roughness, const float lacunarity,
			const float offset, const float gain, const float distortion,
			const CyclesNoiseType noiseType, const u_int dimensions,
			const bool normalize, const bool colorNeeded,
			float &value, float color[3]);

private:
	void EvalInputs(const HitPoint &hitPoint, float &value, float color[3],
			const bool colorNeeded) const;

	std::reference_wrapper<Texture> vec, w, scale, detail, roughness,
			lacunarity, offset, gain, distortion;
	const CyclesNoiseType noiseType;
	const u_int dimensions;
	const bool normalize, colorOutput, isColor;
};

}

#endif	/* _SLG_CYCLESNOISETEX_H */

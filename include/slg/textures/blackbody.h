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

#ifndef _SLG_BLACKBODYTEX_H
#define	_SLG_BLACKBODYTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// Black body texture
//------------------------------------------------------------------------------

class BlackBodyTexture : public Texture {
public:
	BlackBodyTexture(const float temp, const bool normalize = false);
	BlackBodyTexture(TextureConstRef tempTex, const float nominalTemp, const bool normalize);
	virtual ~BlackBodyTexture() { }

	virtual TextureType GetType() const { return BLACKBODY_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const {
		return EvalSpectrumValue(hitPoint).Y();
	}
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectralValue(const HitPoint &hitPoint,
			const luxrays::PathWavelengths &sw, const bool emission) const;
	virtual float Y() const { return rgb.Y(); }
	virtual float Filter() const { return rgb.Filter(); }

	virtual void AddReferencedTextures(std::unordered_set<const Texture *> &referencedTexs) const {
		Texture::AddReferencedTextures(referencedTexs);
		if (temperatureTex)
			temperatureTex->AddReferencedTextures(referencedTexs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap *> &referencedImgMaps) const {
		if (temperatureTex)
			temperatureTex->AddReferencedImageMaps(referencedImgMaps);
	}
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) override {
		if (temperatureTex == std::addressof(oldTex))
			temperatureTex = std::addressof(newTex);
	}

	// Representative RGB at the nominal temperature (importance-sampling hint
	// and the constant-temperature result).
	const luxrays::Spectrum &GetRGB() const { return rgb; }
	float GetTemperature() const { return temperature; }
	bool GetNormalize() const { return normalize; }
	// nullptr when the temperature is a compile-time constant.
	const Texture *GetTemperatureTex() const { return temperatureTex; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

private:
	// Per-point temperature source; nullptr on the constant-temperature path.
	const Texture *temperatureTex;
	const float temperature;
	const bool normalize;

	luxrays::Spectrum rgb;
};

}

#endif	/* _SLG_BLACKBODYTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_NORMALVECTORTEX_H
#define _SLG_NORMALVECTORTEX_H

#include "slg/textures/texture.h"

namespace slg {

// A data-vector shader normal, or a normal-map result exposed as data.
// It never interprets the vector's components as a height field.
class NormalVectorTexture : public Texture {
public:
	NormalVectorTexture(TextureConstRef texture, const bool sourceBump)
		: texture(texture), sourceBump(sourceBump) { }
	virtual TextureType GetType() const { return NORMAL_VECTOR_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectralValue(const HitPoint &hitPoint,
			const luxrays::PathWavelengths &sw, const bool emission) const {
		return EvalSpectrumValue(hitPoint);
	}
	virtual luxrays::Normal Bump(const HitPoint &hitPoint, const float sampleDistance) const;
	virtual float Y() const { return 1.f; }
	virtual float Filter() const { return 1.f; }
	virtual void AddReferencedTextures(std::unordered_set<const Texture *> &refs) const {
		Texture::AddReferencedTextures(refs);
		GetTexture().AddReferencedTextures(refs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap *> &refs) const {
		GetTexture().AddReferencedImageMaps(refs);
	}
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) override {
		updtex(texture, oldTex, newTex);
	}
	TextureConstRef GetTexture() const { return texture; }
	bool IsSourceBump() const { return sourceBump; }
	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &cache,
			const bool useRealFileName) const;

private:
	luxrays::Normal EvaluateNormal(const HitPoint &hitPoint, const float sampleDistance) const;
	std::reference_wrapper<const Texture> texture;
	const bool sourceBump;
};

}
#endif

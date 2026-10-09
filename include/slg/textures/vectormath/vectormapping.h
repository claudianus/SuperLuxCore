// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_VECTORMAPPINGTEX_H
#define _SLG_VECTORMAPPINGTEX_H

#include "slg/textures/texture.h"
#include <array>

namespace slg {

// Data-vector transform with texture-driven translation, Euler XYZ rotation
// and scale. Modes: Point, inverse Texture, Vector, normalized Normal.
class VectorMappingTexture : public Texture {
public:
	VectorMappingTexture(TextureRef vector, TextureRef location, TextureRef rotation,
			TextureRef scale, const u_int mode)
		: inputs{vector, location, rotation, scale}, mode(mode) { }
	virtual TextureType GetType() const { return VECTOR_MAPPING_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectralValue(const HitPoint &hitPoint,
			const luxrays::PathWavelengths &sw, const bool emission) const {
		return EvalSpectrumValue(hitPoint);
	}
	virtual float Y() const { return 1.f; }
	virtual float Filter() const { return 1.f; }
	virtual void AddReferencedTextures(std::unordered_set<const Texture *> &refs) const {
		Texture::AddReferencedTextures(refs);
		for (const auto &input : inputs) input.get().AddReferencedTextures(refs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap *> &refs) const {
		for (const auto &input : inputs) input.get().AddReferencedImageMaps(refs);
	}
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) override {
		for (auto &input : inputs) updtex(input, oldTex, newTex);
	}
	TextureConstRef GetInput(const u_int index) const { return inputs[index]; }
	u_int GetMode() const { return mode; }
	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &cache,
			const bool useRealFileName) const;

private:
	std::array<std::reference_wrapper<Texture>, 4> inputs;
	const u_int mode;
};

}
#endif

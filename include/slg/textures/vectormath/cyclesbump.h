// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_CYCLESBUMPTEX_H
#define _SLG_CYCLESBUMPTEX_H
#include "slg/textures/texture.h"
#include <array>
namespace slg {
class CyclesBumpTexture : public Texture {
public:
    CyclesBumpTexture(TextureRef height, TextureRef distance, TextureRef strength,
            TextureRef normal, const bool useNormal, const bool invert,
            const float filterWidth) : inputs{height, distance, strength, normal},
            useNormal(useNormal), invert(invert), filterWidth(luxrays::Max(filterWidth, 0.f)) { }
    virtual TextureType GetType() const { return CYCLES_BUMP_TEX; }
    virtual float GetFloatValue(const HitPoint &hp) const { return EvalSpectrumValue(hp).Y(); }
    virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hp) const;
    virtual luxrays::Spectrum EvalSpectralValue(const HitPoint &hp,
            const luxrays::PathWavelengths &sw, const bool emission) const { return EvalSpectrumValue(hp); }
    virtual luxrays::Normal Bump(const HitPoint &hp, const float sampleDistance) const;
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
    TextureConstRef GetInput(const u_int i) const { return inputs[i]; }
    bool UsesNormal() const { return useNormal; }
    bool IsInvert() const { return invert; }
    float GetFilterWidth() const { return filterWidth; }
    virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &cache, const bool real) const;
private:
    luxrays::Normal EvaluateNormal(const HitPoint &hp, const float sampleDistance) const;
    std::array<std::reference_wrapper<Texture>, 4> inputs;
    const bool useNormal, invert;
    const float filterWidth;
};
}
#endif

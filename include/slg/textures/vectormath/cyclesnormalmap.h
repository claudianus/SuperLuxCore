// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_CYCLESNORMALMAPTEX_H
#define _SLG_CYCLESNORMALMAPTEX_H
#include "slg/textures/texture.h"
#include <array>
namespace slg {
class CyclesNormalMapTexture : public Texture {
public:
    CyclesNormalMapTexture(TextureRef color, TextureRef strength, const u_int space,
            const bool invertGreen, const u_int normalIndex, const u_int tangentIndex,
            const u_int signIndex) : inputs{color, strength}, space(space),
            invertGreen(invertGreen), normalIndex(normalIndex), tangentIndex(tangentIndex), signIndex(signIndex) { }
    virtual TextureType GetType() const { return CYCLES_NORMAL_MAP_TEX; }
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
    u_int GetSpace() const { return space; }
    bool IsInvertGreen() const { return invertGreen; }
    u_int GetNormalIndex() const { return normalIndex; }
    u_int GetTangentIndex() const { return tangentIndex; }
    u_int GetSignIndex() const { return signIndex; }
    virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &cache, const bool real) const;
private:
    luxrays::Normal EvaluateNormal(const HitPoint &hp) const;
    std::array<std::reference_wrapper<Texture>, 2> inputs;
    const u_int space;
    const bool invertGreen;
    const u_int normalIndex, tangentIndex, signIndex;
};
}
#endif

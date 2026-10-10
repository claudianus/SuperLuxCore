// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_CYCLESBSSRDF_H
#define _SLG_CYCLESBSSRDF_H

#include "slg/materials/matte.h"
#include "slg/textures/constfloat.h"
#include "luxrays/core/randomgen.h"
#include "luxrays/core/intersectiondevice.h"

namespace slg {

class BSDF;
class Scene;
class PathVolumeInfo;

// Experimental nonlocal closure. RenderConfig rejects unsupported transport
// rather than allowing it to become a local diffuse/volume approximation.
class CyclesBSSRDFMaterial : public MatteMaterial {
public:
	CyclesBSSRDFMaterial(TextureConstPtr front, TextureConstPtr back,
			TextureConstPtr emitted, TextureConstPtr bump, TextureConstPtr color,
			TextureConstPtr radius, TextureConstPtr scale, TextureConstPtr ior,
			TextureConstPtr roughness, TextureConstPtr anisotropy);
	MaterialType GetType() const override { return CYCLES_BSSRDF; }
	void AddReferencedTextures(std::unordered_set<const Texture *> &textures) const override;
	void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) override;
	luxrays::PropertiesUPtr ToProperties(const ImageMapCache &cache, bool realFiles) const override;

	struct Parameters {
		luxrays::Spectrum color, radius, radiusRGB;
		float ior, roughness, anisotropy;
	};
	Parameters Freeze(const HitPoint &hit) const;
	std::string ExperimentalParameterFailure(bool spectral) const;
	const MatteMaterial &GetExitMaterial() const { return exitMaterial; }
	TextureConstPtr GetRadius() const { return radius; }
	TextureConstPtr GetScale() const { return scale; }
	TextureConstPtr GetIOR() const { return ior; }
	TextureConstPtr GetRoughness() const { return roughness; }
	TextureConstPtr GetAnisotropy() const { return anisotropy; }

private:
	TextureConstPtr radius, scale, ior, roughness, anisotropy;
	ConstFloatTexture white;
	MatteMaterial exitMaterial;
};

const CyclesBSSRDFMaterial *ResolveCyclesBSSRDF(MaterialConstRef material, const HitPoint &hit);
bool SelectCyclesBSSRDFClosure(BSDF &bsdf, luxrays::TauswortheRandomGenerator &rng,
		luxrays::Spectrum &weight);
bool SampleCyclesBSSRDF(SceneConstRef scene, luxrays::IntersectionDeviceRef device,
		const luxrays::Ray &entryRay,
		const luxrays::RayHit &entryHit, const PathVolumeInfo &volumes,
		BSDF &bsdf, luxrays::TauswortheRandomGenerator &rng, luxrays::Spectrum &weight);

}
#endif

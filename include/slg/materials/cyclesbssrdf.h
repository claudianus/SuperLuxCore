// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_CYCLESBSSRDF_H
#define _SLG_CYCLESBSSRDF_H

#include "slg/materials/matte.h"
#include "slg/textures/constfloat.h"
#include "luxrays/core/randomgen.h"
#include "luxrays/core/intersectiondevice.h"
#include <functional>
#include <limits>

namespace slg {

class BSDF;
class Scene;
class PathVolumeInfo;
class CyclesBSSRDFMaterial;

// Scene-owned inverse entry boundary used after a CPU adjoint random walk.
// The sampler returns the kernel/proposal ratio before BSDF's geometry-cosine
// adjoint correction. It does not model reciprocal rough glass.
class CyclesBSSRDFAdjointBoundary : public MatteMaterial {
public:
	CyclesBSSRDFAdjointBoundary(const CyclesBSSRDFMaterial &source, TextureConstPtr white);
	BSDFEvent GetEventTypes() const override;
	bool IsDelta() const override;
	luxrays::Spectrum Evaluate(const HitPoint &, const luxrays::Vector &localLight,
			const luxrays::Vector &localEye, BSDFEvent *, float *, float *) const override;
	luxrays::Spectrum Sample(const HitPoint &, const luxrays::Vector &localFixed,
			luxrays::Vector *localSampled, float, float, float, float *, BSDFEvent *) const override;
	void Pdf(const HitPoint &, const luxrays::Vector &localLight,
			const luxrays::Vector &localEye, float *, float *) const override;
private:
	const CyclesBSSRDFMaterial &source;
};

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
	std::string ExperimentalAdjointParameterFailure(bool spectral) const;
	const MatteMaterial &GetExitMaterial() const { return exitMaterial; }
	const CyclesBSSRDFAdjointBoundary &GetAdjointBoundary() const { return adjointBoundary; }
	TextureConstPtr GetRadius() const { return radius; }
	TextureConstPtr GetScale() const { return scale; }
	TextureConstPtr GetIOR() const { return ior; }
	TextureConstPtr GetRoughness() const { return roughness; }
	TextureConstPtr GetAnisotropy() const { return anisotropy; }

private:
	TextureConstPtr radius, scale, ior, roughness, anisotropy;
	ConstFloatTexture white;
	MatteMaterial exitMaterial;
	CyclesBSSRDFAdjointBoundary adjointBoundary;
};

const CyclesBSSRDFMaterial *ResolveCyclesBSSRDF(MaterialConstRef material, const HitPoint &hit);
bool SelectCyclesBSSRDFClosure(BSDF &bsdf, luxrays::TauswortheRandomGenerator &rng,
		luxrays::Spectrum &weight);
bool SampleCyclesBSSRDF(SceneConstRef scene, luxrays::IntersectionDeviceRef device,
		const luxrays::Ray &entryRay,
		const luxrays::RayHit &entryHit, const PathVolumeInfo &volumes,
		BSDF &bsdf, luxrays::TauswortheRandomGenerator &rng, luxrays::Spectrum &weight);

// A camera connection before the next interior direction is sampled. The
// initial white-diffuse escape and every HG collision are distinct vertices;
// a delta inverse boundary cannot connect the camera after the walk escapes.
struct CyclesBSSRDFAdjointVertex {
	luxrays::Point p;
	luxrays::Vector incoming;
	luxrays::Spectrum flux, sigmaT;
	float ior, anisotropy;
	bool diffuseEscape;
};
using CyclesBSSRDFAdjointConnect = std::function<void(
		const CyclesBSSRDFAdjointVertex &, luxrays::TauswortheRandomGenerator &)>;

// Trace only this physical object, including its material partitions. Other
// overlapping objects do not bound an object-local Cycles random walk.
bool TraceCyclesBSSRDFBoundary(SceneConstRef scene, luxrays::IntersectionDeviceRef device,
		const luxrays::Ray &ray, u_int entryMesh, luxrays::RayHit &hit,
		u_int skipEntryTriangle = std::numeric_limits<u_int>::max());
bool SampleCyclesBSSRDFAdjoint(SceneConstRef scene, luxrays::IntersectionDeviceRef device,
		const luxrays::Ray &entryRay, const luxrays::RayHit &entryHit,
		const PathVolumeInfo &volumes, BSDF &bsdf,
		luxrays::TauswortheRandomGenerator &rng, luxrays::Spectrum &weight,
		const CyclesBSSRDFAdjointConnect &connect = {});

}
#endif

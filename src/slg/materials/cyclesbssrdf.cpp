// SPDX-License-Identifier: Apache-2.0
#include "slg/materials/cyclesbssrdf.h"
#include "slg/materials/twosided.h"
#include "slg/materials/mix.h"
#include "slg/bsdf/bsdf.h"
#include "slg/scene/scene.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/utils/pathvolumeinfo.h"
#include "luxrays/core/geometry/frame.h"
#include "luxrays/core/color/spectral.h"
#include <cmath>

using namespace luxrays;
using namespace slg;

CyclesBSSRDFMaterial::CyclesBSSRDFMaterial(TextureConstPtr front, TextureConstPtr back,
		TextureConstPtr emitted, TextureConstPtr bump, TextureConstPtr color,
		TextureConstPtr radius, TextureConstPtr scale, TextureConstPtr ior,
		TextureConstPtr roughness, TextureConstPtr anisotropy) :
		MatteMaterial(front, back, emitted, bump, color), radius(radius), scale(scale),
		ior(ior), roughness(roughness), anisotropy(anisotropy), white(1.f),
		exitMaterial(nullptr, nullptr, nullptr, nullptr, TextureConstPtr(&white)) {
	exitMaterial.SetName("__cycles_bssrdf_diffuse_exit__");
}

CyclesBSSRDFMaterial::Parameters CyclesBSSRDFMaterial::Freeze(const HitPoint &hit) const {
	Parameters p;
	p.color = GetKd()->GetSpectrumValue(hit).Clamp(0.f, 1.f);
	p.radius = (radius->GetSpectrumValue(hit) * scale->GetFloatValue(hit)).Clamp();
	{
		// Cycles' strict local-channel cutoff applies to the authored
		// float32 Radius * Scale, before spectral upsampling.
		Spectral::ScopePause rgbScope;
		p.radiusRGB = (radius->GetSpectrumValue(hit) * scale->GetFloatValue(hit)).Clamp();
	}
	p.ior = Clamp(ior->GetFloatValue(hit), 1.01f, 3.8f);
	// Standalone Cycles SSS uses roughness directly as GGX alpha; the
	// Principled closure's separate squaring does not apply here.
	p.roughness = Clamp(roughness->GetFloatValue(hit), 0.f, 1.f);
	p.anisotropy = Clamp(anisotropy->GetFloatValue(hit), -.99f, .99f);
	return p;
}

std::string CyclesBSSRDFMaterial::ExperimentalParameterFailure(bool spectral) const {
	const auto constant = [](TextureConstPtr t) {
		return t->GetType() == CONST_FLOAT || t->GetType() == CONST_FLOAT3;
	};
	if (!constant(radius) || !constant(scale))
		return spectral ? "dynamic spectral Radius/Scale requires closure mapping support" : "";
	HitPoint hit;
	hit.Init();
	const Spectral::ScopePause rawRGB;
	const Spectrum radii = radius->GetSpectrumValue(hit) * scale->GetFloatValue(hit);
	bool anyLocal = false, allLocal = true;
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i) {
		if (!std::isfinite(radii.c[i]))
			return "Radius * Scale must be finite";
		anyLocal |= radii.c[i] < 1e-8f;
		allLocal &= radii.c[i] < 1e-8f;
	}
	if (spectral && anyLocal && !allLocal)
		return "partial-radius spectral closure mapping is not implemented yet";
	return "";
}

void CyclesBSSRDFMaterial::AddReferencedTextures(std::unordered_set<const Texture *> &textures) const {
	MatteMaterial::AddReferencedTextures(textures);
	for (const auto t : {radius, scale, ior, roughness, anisotropy})
		t->AddReferencedTextures(textures);
}

void CyclesBSSRDFMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	MatteMaterial::UpdateTextureReferences(oldTex, newTex);
	for (auto t : {&radius, &scale, &ior, &roughness, &anisotropy})
		if (*t == &oldTex)
			*t = &newTex;
}

PropertiesUPtr CyclesBSSRDFMaterial::ToProperties(const ImageMapCache &cache, bool realFiles) const {
	auto props = MatteMaterial::ToProperties(cache, realFiles);
	const std::string prefix = "scene.materials." + GetName() + ".";
	props->Set(Property(prefix + "type")("cyclesbssrdf"));
	props->Set(Property(prefix + "radius")(radius->GetSDLValue()));
	props->Set(Property(prefix + "scale")(scale->GetSDLValue()));
	props->Set(Property(prefix + "ior")(ior->GetSDLValue()));
	props->Set(Property(prefix + "roughness")(roughness->GetSDLValue()));
	props->Set(Property(prefix + "anisotropy")(anisotropy->GetSDLValue()));
	return props;
}

const CyclesBSSRDFMaterial *slg::ResolveCyclesBSSRDF(MaterialConstRef material, const HitPoint &hit) {
	MaterialConstPtr selected(&material);
	// Mixed branches are selected before transport. Resolve deterministic
	// wrappers here for callers inspecting a standalone closure.
	for (unsigned depth = 0; depth < 64; ++depth) {
		if (selected->GetType() == CYCLES_BSSRDF)
			return static_cast<const CyclesBSSRDFMaterial *>(selected.get());
		if (selected->GetType() != TWOSIDED)
			return nullptr;
		const auto &two = static_cast<const TwoSidedMaterial &>(*selected);
		selected = hit.intoObject ? two.GetFrontMaterial() : two.GetBackMaterial();
	}
	return nullptr;
}

bool slg::SelectCyclesBSSRDFClosure(BSDF &bsdf, TauswortheRandomGenerator &rng, Spectrum &weight) {
	for (unsigned depth = 0; depth < 64; ++depth) {
		const auto selected = bsdf.GetMaterial();
		if (!selected->HasCyclesBSSRDF() || selected->GetType() == CYCLES_BSSRDF)
			return true;
		MaterialConstPtr child;
		if (selected->GetType() == TWOSIDED) {
			const auto &two = static_cast<const TwoSidedMaterial &>(*selected);
			child = bsdf.hitPoint.intoObject ? two.GetFrontMaterial() : two.GetBackMaterial();
		} else if (selected->GetType() == MIX) {
			const auto &mix = static_cast<const MixMaterial &>(*selected);
			const float amount = mix.IsAdditive() ? .5f : Clamp(mix.GetMixFactor().GetFloatValue(bsdf.hitPoint), 0.f, 1.f);
			child = rng.floatValue() < 1.f - amount ? &mix.GetMaterialA() : &mix.GetMaterialB();
			// Mix's physical branch weight equals its sampling probability.
			// Add's two unit weights use p=1/2, requiring a factor of two.
			if (mix.IsAdditive()) weight *= 2.f;
		} else
			return false;
		bsdf.SetSubsurfaceScatteringMaterial(*child);
	}
	return false;
}

static Vector SampleVisibleGGX(const Vector &v, float alpha, float u0, float u1) {
	if (alpha == 0.f)
		return Vector(0.f, 0.f, 1.f);
	const Vector vh = Normalize(Vector(alpha * v.x, alpha * v.y, v.z));
	const float lensq = vh.x * vh.x + vh.y * vh.y;
	const Vector t1 = lensq > 0.f ? Vector(-vh.y, vh.x, 0.f) / sqrtf(lensq) : Vector(1.f, 0.f, 0.f);
	const Vector t2 = Cross(vh, t1);
	const float r = sqrtf(u0), phi = 2.f * M_PI * u1;
	const float x = r * cosf(phi), s = .5f * (1.f + vh.z);
	const float y = (1.f - s) * sqrtf(Max(0.f, 1.f - x * x)) + s * r * sinf(phi);
	const Vector nh = x * t1 + y * t2 + sqrtf(Max(0.f, 1.f - x * x - y * y)) * vh;
	return Normalize(Vector(alpha * nh.x, alpha * nh.y, Max(0.f, nh.z)));
}

static float VanDeHulstAlpha(float albedo, float g) {
	const float s = 4.20863f * albedo + 4.09712f -
			sqrtf(9.59217f + 41.6808f * albedo + 17.7126f * albedo * albedo);
	const float s2 = s * s;
	return Clamp((1.f - s2) / (1.f - g * s2), 0.f, .999999f);
}

static Vector SampleBSSRDFPhase(const Vector &direction, float g, float u0, float u1) {
	float c;
	if (fabsf(g) < 1e-6f)
		c = 1.f - 2.f * u0;
	else {
		const float a = (1.f - g * g) / (1.f - g + 2.f * g * u0);
		c = Clamp((1.f + g * g - a * a) / (2.f * g), -1.f, 1.f);
	}
	const float s = sqrtf(Max(0.f, 1.f - c * c)), phi = 2.f * M_PI * u1;
	Vector x, y;
	CoordinateSystem(direction, &x, &y);
	return Normalize(s * cosf(phi) * x + s * sinf(phi) * y + c * direction);
}

bool slg::SampleCyclesBSSRDF(SceneConstRef scene, IntersectionDeviceRef device,
		const Ray &entryRay, const RayHit &entryHit,
		const PathVolumeInfo &volumes, BSDF &bsdf, TauswortheRandomGenerator &rng,
		Spectrum &weight) {
	const auto *material = ResolveCyclesBSSRDF(*bsdf.GetMaterial(), bsdf.hitPoint);
	if (!material)
		return true;
	const auto p = material->Freeze(bsdf.hitPoint);
	bool allLocal = true, anyLocal = false;
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
		if (p.radiusRGB.c[i] < 1e-8f)
			anyLocal = true;
		else
			allLocal = false;
	if (allLocal)
		return true;
	if (anyLocal && Spectral::Current())
		throw std::runtime_error("Experimental cyclesbssrdf partial-radius spectral closure mapping is not implemented yet");
	Spectrum throughput;
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
		// Cycles' safe_divide(closure weight, albedo) is zero for an
		// exactly black channel, not one. In particular, a black closure
		// must not gain an unscattered escape through a thin object.
		throughput.c[i] = p.color.c[i] > 0.f ? 1.f : 0.f;
	if (anyLocal) {
		float localSum = 0.f, totalSum = 0.f;
		for (unsigned i = 0; i < COLOR_SAMPLES; ++i) {
			totalSum += p.color.c[i];
			if (p.radiusRGB.c[i] < 1e-8f)
				localSum += p.color.c[i];
		}
		const float localProbability = totalSum > 0.f ? localSum / totalSum : 1.f;
		if (rng.floatValue() < localProbability) {
			// The unchanged colored Matte closure supplies its color;
			// mask the local channels and compensate closure selection.
			for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
				weight.c[i] *= p.radiusRGB.c[i] < 1e-8f ? 1.f / localProbability : 0.f;
			return true;
		}
		for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
			throughput.c[i] = p.radiusRGB.c[i] < 1e-8f || p.color.c[i] == 0.f ?
					0.f : 1.f / (1.f - localProbability);
	}
	Spectrum sigmaT, alpha;
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i) {
		if (!std::isfinite(p.radius.c[i]))
			throw std::runtime_error("Experimental cyclesbssrdf radius must be finite");
		const bool local = p.radiusRGB.c[i] < 1e-8f;
		sigmaT.c[i] = local ? 1.f : 1.f / Max(p.radius.c[i], 1e-16f);
		alpha.c[i] = local ? 0.f : VanDeHulstAlpha(p.color.c[i], p.anisotropy);
		if (!local && alpha.c[i] < .2f) {
			// Preserve Cycles' compensated low-albedo closure contract,
			// including its zero-color endpoint. This is specific to the
			// imported BSSRDF, not the native bulk-volume model.
			throughput.c[i] *= alpha.c[i] / .2f;
			alpha.c[i] = .2f;
		}
	}
	const Vector wo = bsdf.GetFrame().ToLocal(bsdf.hitPoint.fixedDir);
	if (!(wo.z > 0.f)) {
		weight = Spectrum();
		return false;
	}
	const Vector h = SampleVisibleGGX(wo, p.roughness, rng.floatValue(), rng.floatValue());
	const float eta = 1.f / p.ior, cosHI = Dot(h, wo);
	const float cosHT = sqrtf(Max(0.f, 1.f - eta * eta * (1.f - cosHI * cosHI)));
	const Vector direction = Normalize(bsdf.GetFrame().ToWorld(-eta * wo + (eta * cosHI - cosHT) * h));
	if (!(Dot(direction, bsdf.hitPoint.geometryN) < 0.f)) {
		weight = Spectrum();
		return false;
	}
	// The closure's color is represented by the remapped scattering
	// coefficients. It is not multiplied a second time at entry or exit.
	Ray walk = entryRay;
	walk.o = bsdf.GetRayOrigin(direction);
	walk.d = direction;
	const std::string &entryGroup = scene.GetObjects().GetSceneObject(entryHit.meshIndex).GetSubsurfaceGroup();
	for (unsigned bounce = 0; bounce < 256; ++bounce) {
		float probabilities[COLOR_SAMPLES], sum = 0.f;
		for (unsigned i = 0; i < COLOR_SAMPLES; ++i) {
			probabilities[i] = Max(throughput.c[i] * alpha.c[i], 0.f);
			sum += probabilities[i];
		}
		if (!(sum > 0.f))
			break;
		for (float &q : probabilities)
			q /= sum;
		float selector = rng.floatValue();
		unsigned channel = COLOR_SAMPLES - 1;
		for (unsigned i = 0; i + 1 < COLOR_SAMPLES; ++i) {
			if (selector < probabilities[i]) { channel = i; break; }
			selector -= probabilities[i];
		}
		const float distance = -log1pf(-Min(rng.floatValue(), .99999994f)) / sigmaT.c[channel];
		walk.mint = MachineEpsilon::E(0.f);
		walk.maxt = distance;
		RayHit boundary;
		bool escaped = false;
		for (;;) {
			const bool hit = device.TraceRay(&walk, &boundary);
			if (!hit)
				break;
			if (boundary.meshIndex == entryHit.meshIndex ||
					(!entryGroup.empty() && scene.GetObjects().GetSceneObject(boundary.meshIndex).
							GetSubsurfaceGroup() == entryGroup)) {
				escaped = true;
				break;
			}
			// Object-local walk: overlapping objects must not become its
			// boundary, shading closure, or a different scattering medium.
			const float next = boundary.t + MachineEpsilon::E(boundary.t);
			if (!(next > walk.mint) || next >= distance)
				break;
			walk.mint = next;
		}
		const float t = escaped ? boundary.t : distance;
		Spectrum transmittance;
		float pdf = 0.f;
		for (unsigned i = 0; i < COLOR_SAMPLES; ++i) {
			transmittance.c[i] = expf(-sigmaT.c[i] * t);
			pdf += probabilities[i] * transmittance.c[i] * (escaped ? 1.f : sigmaT.c[i]);
		}
		if (!(pdf > 0.f) || !std::isfinite(pdf))
			break;
		for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
			throughput.c[i] *= transmittance.c[i] * (escaped ? 1.f : sigmaT.c[i] * alpha.c[i]) / pdf;
		if (escaped) {
			const Point exit = walk(boundary.t);
			bsdf.Init(scene, boundary.meshIndex, boundary.triangleIndex, exit,
					boundary.b1, boundary.b2, entryRay.time, rng.floatValue(), &volumes);
			bsdf.SetSubsurfaceExitMaterial(material->GetExitMaterial());
			weight *= throughput;
			return true;
		}
		walk.o = walk(distance);
		walk.d = SampleBSSRDFPhase(walk.d, p.anisotropy, rng.floatValue(), rng.floatValue());
		if (bounce >= 8) {
			const float survive = Clamp(throughput.Max(), .05f, .95f);
			if (rng.floatValue() >= survive)
				break;
			throughput /= survive;
		}
	}
	weight = Spectrum();
	return false;
}

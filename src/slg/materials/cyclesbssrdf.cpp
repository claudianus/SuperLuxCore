// SPDX-License-Identifier: Apache-2.0
#include "slg/materials/cyclesbssrdf.h"
#include "slg/materials/cyclesbssrdf_boundary.h"
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
	exitMaterial(nullptr, nullptr, nullptr, nullptr, TextureConstPtr(&white)),
	adjointBoundary(*this, TextureConstPtr(&white)) {
	exitMaterial.SetName("__cycles_bssrdf_diffuse_exit__");
}

CyclesBSSRDFAdjointBoundary::CyclesBSSRDFAdjointBoundary(const CyclesBSSRDFMaterial &source,
		TextureConstPtr white) : MatteMaterial(nullptr, nullptr, nullptr, nullptr, white), source(source) {
	SetName("__cycles_bssrdf_adjoint_entry__");
}

bool CyclesBSSRDFAdjointBoundary::IsDelta() const {
	const auto roughness = source.GetRoughness();
	if (roughness->GetType() != CONST_FLOAT && roughness->GetType() != CONST_FLOAT3)
		return false;
	HitPoint hit;
	hit.Init();
	return roughness->GetFloatValue(hit) <= 0.f;
}

BSDFEvent CyclesBSSRDFAdjointBoundary::GetEventTypes() const {
	return (IsDelta() ? SPECULAR : DIFFUSE) | TRANSMIT;
}

Spectrum CyclesBSSRDFAdjointBoundary::Evaluate(const HitPoint &hit,
		const Vector &light, const Vector &eye, BSDFEvent *event,
		float *directPdf, float *reversePdf) const {
	const float side = Dot(hit.fixedDir, hit.geometryN) < 0.f ? 1.f : -1.f;
	const auto p = cyclesbssrdfboundary::CyclesBSSRDF_BoundaryDensities(side * eye, side * light,
			Clamp(source.GetIOR()->GetFloatValue(hit), 1.01f, 3.8f),
			Clamp(source.GetRoughness()->GetFloatValue(hit), 0.f, 1.f));
	*event = DIFFUSE | TRANSMIT;
	if (directPdf) *directPdf = p.reversePdf;
	if (reversePdf) *reversePdf = p.forwardPdf;
	// This is f*cos(theta_light) for the original eye-side directional
	// kernel. BSDF::Evaluate supplies the geometry-cosine transpose.
	return Spectrum(p.valid && !p.delta ? p.forwardPdf : 0.f);
}

Spectrum CyclesBSSRDFAdjointBoundary::Sample(const HitPoint &hit,
		const Vector &fixed, Vector *sampled, const float u0, const float u1,
		const float, float *pdf, BSDFEvent *event) const {
	const float side = Dot(hit.fixedDir, hit.geometryN) < 0.f ? 1.f : -1.f;
	const auto s = cyclesbssrdfboundary::CyclesBSSRDF_SampleAdjointBoundary(side * fixed,
			Clamp(source.GetIOR()->GetFloatValue(hit), 1.01f, 3.8f),
			Clamp(source.GetRoughness()->GetFloatValue(hit), 0.f, 1.f), u0, u1);
	*event = (s.densities.delta ? SPECULAR : DIFFUSE) | TRANSMIT;
	*pdf = s.densities.delta ? s.densities.reverseMass : s.densities.reversePdf;
	if (!s.valid || !(*pdf > 0.f)) return Spectrum();
	*sampled = side * Normalize(s.direction);
	if (s.densities.delta)
		return Spectrum(s.densities.adjointWeight * fabsf(fixed.z / sampled->z));
	return Spectrum(s.densities.forwardPdf / s.densities.reversePdf);
}

void CyclesBSSRDFAdjointBoundary::Pdf(const HitPoint &hit, const Vector &light,
		const Vector &eye, float *directPdf, float *reversePdf) const {
	BSDFEvent event;
	Evaluate(hit, light, eye, &event, directPdf, reversePdf);
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

std::string CyclesBSSRDFMaterial::ExperimentalAdjointParameterFailure(bool spectral) const {
	const auto prior = ExperimentalParameterFailure(spectral);
	if (!prior.empty()) return prior;
	for (auto t : {GetKd(), radius, scale, ior, roughness, anisotropy})
		if (t->GetType() != CONST_FLOAT && t->GetType() != CONST_FLOAT3)
			return "adjoint textured coefficients require nonlocal kernel reconstruction";
	HitPoint hit;
	hit.Init();
	const auto p = Freeze(hit);
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i)
		if (!std::isfinite(p.color.c[i]) || !std::isfinite(p.radius.c[i]))
			return "adjoint color and radius must be finite";
	if (!std::isfinite(p.ior) || !std::isfinite(p.roughness) || !std::isfinite(p.anisotropy))
		return "adjoint IOR, roughness and anisotropy must be finite";
	bool nonlocal = false;
	for (unsigned i = 0; i < COLOR_SAMPLES; ++i) nonlocal |= p.radiusRGB.c[i] >= 1e-8f;
	if (nonlocal && !(p.roughness > 0.f))
		return "adjoint sharp camera boundaries require internal-vertex/manifold connections";
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
	bool selectedBranch = false;
	for (unsigned depth = 0; depth < 64; ++depth) {
		const auto selected = bsdf.GetMaterial();
		if ((!selected->HasCyclesBSSRDF() && !selected->HasNullLobes()) || selected->GetType() == CYCLES_BSSRDF) {
			// The old entry variate is restricted by transparent selection.
			// A newly chosen opaque component needs a fresh uniform variate
			// for its internal lobe sampler (including ordinary subtrees).
			if (selectedBranch) bsdf.hitPoint.passThroughEvent = rng.floatValue();
			return true;
		}
		MaterialConstPtr child;
		if (selected->GetType() == TWOSIDED) {
			const auto &two = static_cast<const TwoSidedMaterial &>(*selected);
			child = bsdf.hitPoint.intoObject ? two.GetFrontMaterial() : two.GetBackMaterial();
		} else if (selected->GetType() == MIX) {
			const auto &mix = static_cast<const MixMaterial &>(*selected);
			const float amount = mix.IsAdditive() ? .5f : Clamp(mix.GetMixFactor().GetFloatValue(bsdf.hitPoint), 0.f, 1.f);
			const float massA = (1.f - amount) * mix.GetMaterialA().GetNonNullSelectionProbability(bsdf.hitPoint);
			const float massB = amount * mix.GetMaterialB().GetNonNullSelectionProbability(bsdf.hitPoint);
			const float total = massA + massB;
			if (!(total > 0.f)) return false;
			child = rng.floatValue() < massA / total ? &mix.GetMaterialA() : &mix.GetMaterialB();
			// Conditioning both the component contribution and its PDF on
			// non-null selection cancels their shared mass. Add retains its
			// two unit weights, giving a factor of two at each Add level.
			if (mix.IsAdditive()) weight *= 2.f;
		} else
			return false;
		bsdf.SetSubsurfaceScatteringMaterial(*child);
		selectedBranch = true;
	}
	return false;
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

static bool SampleCyclesBSSRDFWalk(SceneConstRef scene, IntersectionDeviceRef device,
		const Ray &entryRay, const RayHit &entryHit,
		const PathVolumeInfo &volumes, BSDF &bsdf, TauswortheRandomGenerator &rng,
		Spectrum &weight, const bool adjoint) {
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
	const Vector fixed = bsdf.GetFrame().ToLocal(bsdf.hitPoint.fixedDir);
	const float side = fixed.z < 0.f ? -1.f : 1.f;
	const Vector wo = side * fixed;
	// Cycles face-forwards its shading frame at an inside/back-facing
	// entry too. Transparent branches can expose that side of a shell.
	if (!(wo.z > 0.f)) {
		weight = Spectrum();
		return false;
	}
	Vector direction;
	if (adjoint) {
		// Transpose the white diffuse escape, rather than refracting again
		// on the light side. Draw the interior direction under the geometry
		// cosine measure and retain the escape's shading/geometry correction.
		const float geometrySide = Dot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.geometryN) < 0.f ? -1.f : 1.f;
		const Normal landing = geometrySide * bsdf.hitPoint.geometryN;
		Vector x, y;
		CoordinateSystem(Vector(landing), &x, &y);
		const Vector local = CosineSampleHemisphere(rng.floatValue(), rng.floatValue());
		direction = Normalize(local.x * x + local.y * y - local.z * Vector(landing));
		const float geometryCos = AbsDot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.geometryN);
		if (!(geometryCos > 0.f)) { weight = Spectrum(); return false; }
		throughput *= AbsDot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.shadeN) / geometryCos;
	} else {
		const auto boundary = cyclesbssrdfboundary::CyclesBSSRDF_SampleEntryBoundary(
				wo, p.ior, p.roughness, rng.floatValue(), rng.floatValue());
		direction = Normalize(bsdf.GetFrame().ToWorld(side * boundary.direction));
	}
	if (!(side * Dot(direction, bsdf.hitPoint.geometryN) < 0.f)) {
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
			// Cycles shades the escape with a reversed ray, so an outside
			// walk returning into the shell must scatter on its inside face.
			const float exitSide = Dot(walk.d, bsdf.hitPoint.geometryN) < 0.f ? -1.f : 1.f;
			if (adjoint) {
				bsdf.hitPoint.fromLight = true;
				bsdf.hitPoint.fixedDir = -walk.d;
				bsdf.hitPoint.intoObject = exitSide < 0.f;
				bsdf.SetSubsurfaceExitMaterial(material->GetAdjointBoundary());
			} else {
				bsdf.hitPoint.fixedDir = exitSide * Vector(bsdf.hitPoint.geometryN);
				bsdf.hitPoint.intoObject = exitSide > 0.f;
				bsdf.SetSubsurfaceExitMaterial(material->GetExitMaterial());
			}
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

bool slg::SampleCyclesBSSRDF(SceneConstRef scene, IntersectionDeviceRef device,
		const Ray &entryRay, const RayHit &entryHit, const PathVolumeInfo &volumes,
		BSDF &bsdf, TauswortheRandomGenerator &rng, Spectrum &weight) {
	return SampleCyclesBSSRDFWalk(scene, device, entryRay, entryHit, volumes, bsdf, rng, weight, false);
}

bool slg::SampleCyclesBSSRDFAdjoint(SceneConstRef scene, IntersectionDeviceRef device,
		const Ray &entryRay, const RayHit &entryHit, const PathVolumeInfo &volumes,
		BSDF &bsdf, TauswortheRandomGenerator &rng, Spectrum &weight) {
	return SampleCyclesBSSRDFWalk(scene, device, entryRay, entryHit, volumes, bsdf, rng, weight, true);
}

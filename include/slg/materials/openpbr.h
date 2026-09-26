/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

#ifndef _SLG_OPENPBRMAT_H
#define	_SLG_OPENPBRMAT_H

#include "slg/materials/material.h"

namespace slg {

//------------------------------------------------------------------------------
// OpenPBR Surface material
//
// Implements the ASWF OpenPBR Surface v1.1 layer stack as a lobe mixture
// (albedo-scaling approximation, per the spec's "Reduction to a mixture of
// lobes"):
//
//   fuzz (Zeltner SGGX-LTC)
//   coat (GGX dielectric, statistical coverage)
//   base-substrate = mix(dielectric-base, metal, M)
//   dielectric-base = layer(specular, mix(opaque-base, transmission, T))
//   opaque-base    = mix(EON diffuse, subsurface, S)
//
// Transmission and subsurface refract into the material interior volume;
// the parser auto-creates a homogeneous volume from transmission_* /
// subsurface_* parameters when no explicit interior volume is set.
//
// References:
//   - OpenPBR Surface v1.1.1 spec (AcademySoftwareFoundation/OpenPBR)
//   - Reference implementation: open_pbr_surface.mtlx + OpenPBR-viewer
//   - EON diffuse: Portsmouth, Kutz, Hill 2024 (arXiv 2410.18026)
//   - F82 tint: Kutz 2021 / Gulbrandsen 2014
//   - Fuzz: Zeltner et al. 2022 SGGX-LTC fits
//------------------------------------------------------------------------------

class OpenPBRMaterial : public Material {
public:
	OpenPBRMaterial(
		TextureConstPtr frontTransp, TextureConstPtr backTransp,
		TextureConstPtr emitted, TextureConstPtr bump,
		TextureConstPtr baseColor, TextureConstPtr baseWeight,
		TextureConstPtr baseMetalness, TextureConstPtr baseDiffuseRoughness,
		TextureConstPtr specWeight, TextureConstPtr specColor,
		TextureConstPtr specRoughness, TextureConstPtr specAnisotropy,
		TextureConstPtr specRotation, TextureConstPtr specIor,
		TextureConstPtr transWeight, TextureConstPtr transColor,
		TextureConstPtr transDepth, TextureConstPtr transScatter,
		TextureConstPtr transScatterAniso, TextureConstPtr dispersion,
		TextureConstPtr sssWeight, TextureConstPtr sssColor,
		TextureConstPtr sssRadius, TextureConstPtr sssRadiusScale,
		TextureConstPtr sssAnisotropy,
		TextureConstPtr coatWeight, TextureConstPtr coatColor,
		TextureConstPtr coatRoughness, TextureConstPtr coatAnisotropy,
		TextureConstPtr coatRotation, TextureConstPtr coatIor,
		TextureConstPtr coatDarkening,
		TextureConstPtr fuzzWeight, TextureConstPtr fuzzColor,
		TextureConstPtr fuzzRoughness,
		TextureConstPtr filmWeight, TextureConstPtr filmThickness,
		TextureConstPtr filmIor
	);

	virtual MaterialType GetType() const { return OPENPBR; }
	virtual BSDFEvent GetEventTypes() const {
		return DIFFUSE | GLOSSY | REFLECT | TRANSMIT;
	};
	virtual bool IsDelta() const { return false; }

	virtual luxrays::Spectrum Albedo(const HitPoint &hitPoint) const;

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
		BSDFEvent *event,
		float *directPdfW = NULL, float *reversePdfW = NULL) const;
	virtual luxrays::Spectrum Sample(const HitPoint &hitPoint,
		const luxrays::Vector &localFixedDir, luxrays::Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const;
	virtual void Pdf(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const;

	virtual void AddReferencedTextures(std::unordered_set<const Texture *> &referencedTexs) const;
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex);

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache,
			const bool useRealFileName) const;

	TextureConstPtr GetBaseColor() const { return BaseColor; }
	TextureConstPtr GetBaseWeight() const { return BaseWeight; }
	TextureConstPtr GetBaseMetalness() const { return BaseMetalness; }
	TextureConstPtr GetBaseDiffuseRoughness() const { return BaseDiffuseRoughness; }
	TextureConstPtr GetSpecularWeight() const { return SpecularWeight; }
	TextureConstPtr GetSpecularColor() const { return SpecularColor; }
	TextureConstPtr GetSpecularRoughness() const { return SpecularRoughness; }
	TextureConstPtr GetSpecularAnisotropy() const { return SpecularAnisotropy; }
	TextureConstPtr GetSpecularRotation() const { return SpecularRotation; }
	TextureConstPtr GetSpecularIor() const { return SpecularIor; }
	TextureConstPtr GetTransmissionWeight() const { return TransmissionWeight; }
	TextureConstPtr GetTransmissionColor() const { return TransmissionColor; }
	TextureConstPtr GetTransmissionDepth() const { return TransmissionDepth; }
	TextureConstPtr GetTransmissionScatter() const { return TransmissionScatter; }
	TextureConstPtr GetTransmissionScatterAnisotropy() const { return TransmissionScatterAniso; }
	TextureConstPtr GetDispersion() const { return Dispersion; }
	TextureConstPtr GetSubsurfaceWeight() const { return SubsurfaceWeight; }
	TextureConstPtr GetSubsurfaceColor() const { return SubsurfaceColor; }
	TextureConstPtr GetSubsurfaceRadius() const { return SubsurfaceRadius; }
	TextureConstPtr GetSubsurfaceRadiusScale() const { return SubsurfaceRadiusScale; }
	TextureConstPtr GetSubsurfaceAnisotropy() const { return SubsurfaceAnisotropy; }
	TextureConstPtr GetCoatWeight() const { return CoatWeight; }
	TextureConstPtr GetCoatColor() const { return CoatColor; }
	TextureConstPtr GetCoatRoughness() const { return CoatRoughness; }
	TextureConstPtr GetCoatAnisotropy() const { return CoatAnisotropy; }
	TextureConstPtr GetCoatRotation() const { return CoatRotation; }
	TextureConstPtr GetCoatIor() const { return CoatIor; }
	TextureConstPtr GetCoatDarkening() const { return CoatDarkening; }
	TextureConstPtr GetFuzzWeight() const { return FuzzWeight; }
	TextureConstPtr GetFuzzColor() const { return FuzzColor; }
	TextureConstPtr GetFuzzRoughness() const { return FuzzRoughness; }
	TextureConstPtr GetFilmWeight() const { return FilmWeight; }
	TextureConstPtr GetFilmThickness() const { return FilmThickness; }
	TextureConstPtr GetFilmIor() const { return FilmIor; }

private:
	// Per-hitpoint evaluated parameter set, shared by Evaluate/Sample/Pdf
	struct Params {
		luxrays::Spectrum baseColor, specColor, transColor, transScatter;
		luxrays::Spectrum sssColor, sssRadiusScale, coatColor, fuzzColor;
		float baseWeight, metalness, diffuseRoughness;
		float specWeight, specRoughness, specAniso, specRotation, specIor;
		float transWeight, transDepth, transScatterAniso, dispersion;
		float sssWeight, sssRadius, sssAnisotropy;
		float coatWeight, coatRoughness, coatAniso, coatRotation, coatIor,
				coatDarkening;
		float fuzzWeight, fuzzRoughness;
		float filmWeight, filmThickness, filmIor;
		float extIor;
	};

	// Lobe ids matching the OpenPBR reference lobe list
	enum LobeId {
		LOBE_FUZZ, LOBE_COAT, LOBE_METAL, LOBE_SPEC,
		LOBE_BTDF, LOBE_DIFF, LOBE_SSS, LOBE_COUNT
	};

	void EvaluateParams(const HitPoint &hitPoint, Params &p) const;

	// specular_ior / exterior ratio blended toward coat IOR by coat weight,
	// with the spec's TIR-preserving ratio flip.
	float EtaS(const Params &p, const float cauchyB) const;

	// Lobe weights + probabilities at outgoing direction wo.
	void ComputeWeights(const HitPoint &hitPoint, const Params &p,
			const luxrays::Vector &wo,
			luxrays::Spectrum weights[LOBE_COUNT], float probs[LOBE_COUNT]) const;

	// Per-lobe evaluate: f * |cosI| (LuxCore convention), fills pdf.
	luxrays::Spectrum EvalGlossyRefl(const HitPoint &hitPoint, const Params &p,
			const bool coat,
			const luxrays::Vector &wo, const luxrays::Vector &wi,
			float &pdf) const;
	luxrays::Spectrum EvalMetal(const HitPoint &hitPoint, const Params &p,
			const luxrays::Vector &wo, const luxrays::Vector &wi,
			float &pdf) const;
	luxrays::Spectrum EvalBtdf(const HitPoint &hitPoint, const Params &p,
			const luxrays::Vector &wo, const luxrays::Vector &wi,
			float &pdf) const;
	// PDF of sampling `wi` given fixed `wo` for a single lobe.
	float LobePdf(const HitPoint &hitPoint, const Params &p, const LobeId lobe,
			const luxrays::Vector &wo, const luxrays::Vector &wi) const;

	luxrays::Spectrum EvalInternal(const HitPoint &hitPoint, const Params &p,
			const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
			BSDFEvent *event, float *directPdfW, float *reversePdfW) const;

	TextureConstPtr BaseColor, BaseWeight, BaseMetalness, BaseDiffuseRoughness;
	TextureConstPtr SpecularWeight, SpecularColor, SpecularRoughness,
			SpecularAnisotropy, SpecularRotation, SpecularIor;
	TextureConstPtr TransmissionWeight, TransmissionColor, TransmissionDepth,
			TransmissionScatter, TransmissionScatterAniso, Dispersion;
	TextureConstPtr SubsurfaceWeight, SubsurfaceColor, SubsurfaceRadius,
			SubsurfaceRadiusScale, SubsurfaceAnisotropy;
	TextureConstPtr CoatWeight, CoatColor, CoatRoughness, CoatAnisotropy,
			CoatRotation, CoatIor, CoatDarkening;
	TextureConstPtr FuzzWeight, FuzzColor, FuzzRoughness;
	TextureConstPtr FilmWeight, FilmThickness, FilmIor;
};

}

#endif	/* _SLG_OPENPBRMAT_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

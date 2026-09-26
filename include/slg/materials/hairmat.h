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

#ifndef _SLG_HAIRMAT_H
#define	_SLG_HAIRMAT_H

#include "slg/materials/material.h"
#include "slg/shapes/strands.h"

namespace slg {

// The strand tangent is carried in the vertex AOV layers defined in
// slg/shapes/strands.h (HAIR_TANGENT_*_DATA_INDEX, object space).

//------------------------------------------------------------------------------
// Hair material with two scattering models, selected by "model":
//   - "chiang" (default): Chiang et al. 2019 "A Practical and Controllable
//     Hair and Fur Model" — the model behind pbrt's HairBSDF and Blender's
//     Principled Hair v1.
//   - "huang": Huang et al. 2022 "A Microfacet-based Hair Scattering Model"
//     (EGSR 2022) — the model behind Blender's Principled Hair v2.
//
// Supports three absorption parameterizations, evaluated per-hit:
//   - "sigma_a" spectrum texture (direct absorption coefficient)
//   - "color" spectrum texture (inverted through SigmaAFromReflectance)
//   - "eumelanin"/"pheomelanin" float textures (melanin concentration model)
// Priority order: sigma_a > color > melanin (default brown when all absent).
//------------------------------------------------------------------------------

class HairMaterial : public Material {
public:
	enum class HairModel { CHIANG, HUANG };

	HairMaterial(TextureConstPtr frontTransp, TextureConstPtr backTransp,
			TextureConstPtr emitted, TextureConstPtr bump,
			TextureConstPtr sigmaA, TextureConstPtr color,
			TextureConstPtr eumelanin, TextureConstPtr pheomelanin,
			TextureConstPtr eta, TextureConstPtr betaM, TextureConstPtr betaN,
			TextureConstPtr alpha, TextureConstPtr roughness,
			TextureConstPtr aspectRatio, const float scaleR,
			const float scaleTT, const float scaleTRT,
			const HairModel model);

	virtual MaterialType GetType() const { return HAIR; }
	virtual BSDFEvent GetEventTypes() const { return GLOSSY | REFLECT | TRANSMIT; };

	virtual luxrays::Spectrum Albedo(const HitPoint &hitPoint) const;

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir, BSDFEvent *event,
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

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	TextureConstPtr GetSigmaA() const { return sigmaA; }
	TextureConstPtr GetColor() const { return color; }
	TextureConstPtr GetEumelanin() const { return eumelanin; }
	TextureConstPtr GetPheomelanin() const { return pheomelanin; }
	TextureConstPtr GetEta() const { return eta; }
	TextureConstPtr GetBetaM() const { return betaM; }
	TextureConstPtr GetBetaN() const { return betaN; }
	TextureConstPtr GetAlpha() const { return alpha; }
	TextureConstPtr GetRoughness() const { return roughness; }
	TextureConstPtr GetAspectRatio() const { return aspectRatio; }
	float GetScaleR() const { return scaleR; }
	float GetScaleTT() const { return scaleTT; }
	float GetScaleTRT() const { return scaleTRT; }
	HairModel GetHairModel() const { return model; }

private:
	// Per-hit hair geometry/parameter state
	struct HairContext {
		luxrays::Vector tangent;   // strand tangent in the local shading frame
		float gammaO;              // asin(h), azimuthal offset of the hit
		float eta;
		luxrays::Spectrum sigma_a;
		float v[4];                // longitudinal variances (p = 0..3)
		float s;                   // azimuthal logistic scale
		float sin2kAlpha[3];
		float cos2kAlpha[3];

		// Huang model state (only filled when model == HUANG)
		luxrays::Vector frameX, frameY, frameZ; // Huang local frame
		luxrays::Vector wi;        // fixed dir in the Huang frame
		float hDivR;               // hit offset over projected radius [-1,1]
		float radius;              // projected radius from the view dir
		float rough;               // GGX alpha (= artist roughness)
		float tilt;                // cuticle tilt, radians
		float aspect;              // minor/major axis ratio b
	};

	bool SetupContext(const HitPoint &hitPoint, const luxrays::Vector &localEyeDir,
			HairContext *ctx) const;
	luxrays::Spectrum EvalSigmaA(const HitPoint &hitPoint, const float betaN) const;

	luxrays::Spectrum f(const HitPoint &hitPoint, const HairContext &ctx,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir) const;
	void ComputeApPdf(const HairContext &ctx, const float cosThetaO, float *apPdf) const;

	// Huang: ctx->wi is the fixed (eye) dir in the hair frame; the methods
	// take/return the opposite direction in the same frame.
	luxrays::Spectrum HuangEval(const HairContext &ctx,
		const luxrays::Vector &woLight) const;
	luxrays::Spectrum HuangSample(const HairContext &ctx,
		luxrays::Vector *woLight, const float u[8]) const;

	TextureConstPtr sigmaA;
	TextureConstPtr color;
	TextureConstPtr eumelanin;
	TextureConstPtr pheomelanin;
	TextureConstPtr eta;
	TextureConstPtr betaM;
	TextureConstPtr betaN;
	TextureConstPtr alpha;
	TextureConstPtr roughness;
	TextureConstPtr aspectRatio;
	float scaleR, scaleTT, scaleTRT;
	HairModel model;
};

}

#endif	/* _SLG_HAIRMAT_H */

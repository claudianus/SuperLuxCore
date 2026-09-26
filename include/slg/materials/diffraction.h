/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   Licensed under the Apache License, Version 2.0 (the "License");       *
 *   you may not use this file except in compliance with the License.      *
 *   You may obtain a copy of the License at                               *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

#ifndef _SLG_DIFFRACTIONMAT_H
#define	_SLG_DIFFRACTIONMAT_H

#include "slg/materials/material.h"

namespace slg {

//------------------------------------------------------------------------------
// Diffraction grating material (1D reflective grating, Stam'99 style)
//
// Produces the rainbow iridescence of compact discs, holographic foils and
// grooved iridescent surfaces. Each diffraction order m is a discrete
// direction (a delta lobe): grooves run along the local tangent t and the
// outgoing direction is constrained along the grating direction s by
// sin(theta_m) = -sin(theta_i) + m * lambda / d.
//
// Groove orientation modes (DiffractionOrientation):
//   U / V   : grooves parallel to the U / V parameter direction
//   RADIALUV: grooves are circles around (centerU, centerV) in UV space
//   RADIAL  : grooves are circles around `center` in object space
//------------------------------------------------------------------------------

typedef enum {
	DIFFRACTION_U = 0,
	DIFFRACTION_V = 1,
	DIFFRACTION_RADIAL_UV = 2,
	DIFFRACTION_RADIAL = 3
} DiffractionOrientation;

class DiffractionMaterial : public Material {
public:
	using TexRef = TextureConstPtr;
	DiffractionMaterial(TexRef frontTransp, TexRef backTransp,
			TexRef emitted, TexRef bump,
			TexRef refl, TexRef spacing, TexRef rough, TexRef fill,
			const DiffractionOrientation orient,
			const luxrays::Point &center, const float centerU, const float centerV,
			const float blaze, const u_int maxOrder);

	virtual MaterialType GetType() const { return DIFFRACTION; }
	virtual BSDFEvent GetEventTypes() const { return SPECULAR | REFLECT; };

	virtual bool IsDelta() const { return true; }

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW = NULL, float *reversePdfW = NULL) const;
	virtual luxrays::Spectrum Sample(const HitPoint &hitPoint,
		const luxrays::Vector &localFixedDir, luxrays::Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const;
	virtual void Pdf(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
		if (directPdfW)
			*directPdfW = 0.f;
		if (reversePdfW)
			*reversePdfW = 0.f;
	}

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const;
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex);

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	TexRef GetKr() const { return Kr; }
	TexRef GetSpacing() const { return spacing; }
	TexRef GetRoughness() const { return roughness; }
	TexRef GetFillFactor() const { return fillFactor; }
	const DiffractionOrientation GetOrientation() const { return orientation; }
	const luxrays::Point &GetCenter() const { return center; }
	float GetCenterU() const { return centerU; }
	float GetCenterV() const { return centerV; }
	float GetBlaze() const { return blaze; }
	u_int GetMaxOrder() const { return maxOrder; }

private:
	TexRef Kr;
	TexRef spacing;
	TexRef roughness;
	TexRef fillFactor;
	const DiffractionOrientation orientation;
	const luxrays::Point center;
	const float centerU, centerV;
	// Blaze angle in radians (0 = symmetric lamellar grating)
	const float blaze;
	const u_int maxOrder;
};

}
#endif	/* _SLG_DIFFRACTIONMAT_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

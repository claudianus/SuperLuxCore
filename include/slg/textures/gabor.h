/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#ifndef _SLG_GABORTEX_H
#define	_SLG_GABORTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// Gabor noise texture (Cycles "Gabor Texture" node equivalent)
//
// Sparse Gabor convolution after Lagae et al. 2009, "Procedural noise using
// sparse Gabor convolution" (SIGGRAPH/TOG 28.3), normalized after Tavernier
// et al. 2019, "Making Gabor noise fast and normalized" (Eurographics), with
// phasor outputs after Tricard et al. 2019, "Procedural phasor noise".
//
// Per evaluation point the texture sums K Gabor kernels per cell over a
// 3x3 cell neighborhood. Each kernel is a Hann-windowed Gaussian envelope
// multiplied by a phasor (cos/sin of the orientation-projected position).
// The accumulated phasor is divided by the analytic standard deviation of
// the kernel sum so the "value" output is roughly [-1, 1]; it is then mapped
// to [0, 1]. "phase" returns the phasor angle in [0, 1) and "intensity" the
// normalized phasor magnitude.
//
// CPU and OpenCL implementations share the same cell hash and impulse
// schedule so both paths produce identical results.
//------------------------------------------------------------------------------

typedef enum {
	GABOR_VALUE = 0,
	GABOR_PHASE,
	GABOR_INTENSITY
} GaborOutput;

class GaborNoiseTexture : public Texture {
public:
	// vec: evaluation coordinates (usually uv or position)
	// scale: global coordinate multiplier; frequency: kernel bands rate
	// isotropy: 1 -> all kernels at `orientation`, 0 -> random per impulse
	GaborNoiseTexture(TextureRef v, const float s, const float f,
			const float i, const float o, const GaborOutput out);
	GaborNoiseTexture(TextureRef v, const float s, const float f,
			const float i, const float o, const GaborOutput out,
			const float sigmaInv);
	virtual ~GaborNoiseTexture() { }

	virtual TextureType GetType() const { return GABORNOISE_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const { return luxrays::Spectrum(.5f).Y(); }
	virtual float Filter() const { return .5f; }

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
		Texture::AddReferencedTextures(referencedTexs);
		vec.get().AddReferencedTextures(referencedTexs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap * > &referencedImgMaps) const {
		vec.get().AddReferencedImageMaps(referencedImgMaps);
	}
	virtual void UpdateTextureReferences(TextureRef oldTex, TextureRef newTex) {
		updtex(vec, oldTex, newTex);
	}

	TextureConstRef GetVec() const { return vec; }
	float GetScale() const { return scale; }
	float GetFrequency() const { return frequency; }
	float GetIsotropy() const { return isotropy; }
	float GetOrientation() const { return orientation; }
	GaborOutput GetOutput() const { return output; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	// Number of impulses summed per cell (Tavernier 2019 default schedule)
	static constexpr u_int IMPULSES_PER_CELL = 8;

	// 2D kernel: Hann-windowed Gaussian x phasor at relative position
	// (px, py). Returns cos/sin pair. Shared math for CPU and OpenCL.
	static void Kernel(const float px, const float py, const float freq,
			const float orient, float &re, float &im);

	// Full noise evaluation; re/im receive the un-normalized phasor sum.
	static void EvalPhasor(const float x, const float y, const float freq,
			const float isotropy, const float orient, float &re, float &im);

	// 1/sigma of the phasor sum for the given frequency (quadrature of the
	// kernel's second moment). Exposed for the OpenCL param compile path.
	static float SigmaInv(const float freq);

private:
	std::reference_wrapper<Texture> vec;
	const float scale, frequency, isotropy, orientation;
	const GaborOutput output;
	const float sigmaInv;
};

}

#endif	/* _SLG_GABORTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

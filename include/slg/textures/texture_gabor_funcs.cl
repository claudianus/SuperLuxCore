#line 2 "texture_gabor_funcs.cl"

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

//------------------------------------------------------------------------------
// Gabor noise texture
//
// Mirrors slg::GaborNoiseTexture: sparse Gabor convolution after Lagae 2009,
// normalized after Tavernier 2019, phasor outputs after Tricard 2019.
// Uses the white-noise cell hash and the same RNG stream schedule as the CPU
// implementation so both paths agree bit-for-bit.
//------------------------------------------------------------------------------

#define GABOR_IMPULSES_PER_CELL 8u

OPENCL_FORCE_INLINE void GaborNoiseTexture_Kernel(const float px,
		const float py, const float freq, const float orient,
		float *re, float *im) {
	const float distSqr = px * px + py * py;
	if (distSqr >= 1.f) {
		*re = 0.f;
		*im = 0.f;
		return;
	}

	const float hann = .5f + .5f * cos(M_PI_F * distSqr);
	const float envelope = exp(-M_PI_F * distSqr) * hann;

	const float angle = 2.f * M_PI_F * freq *
			(px * cos(orient) + py * sin(orient));
	*re = envelope * cos(angle);
	*im = envelope * sin(angle);
}

OPENCL_FORCE_INLINE void GaborNoiseTexture_EvalPhasor(const float x,
		const float y, const float freq, const float isotropy,
		const float orient, float *re, float *im) {
	const int cellX = Floor2Int(x), cellY = Floor2Int(y);
	const float localX = x - cellX, localY = y - cellY;

	float sumRe = 0.f, sumIm = 0.f;
	for (int j = -1; j <= 1; ++j) {
		for (int i = -1; i <= 1; ++i) {
			const float px = localX - i, py = localY - j;

			Seed seed;
			Rnd_Init(WhiteNoiseTexture_SeedFromVector(
					MAKE_FLOAT3(cellX + i, cellY + j, 0.f)), &seed);

			for (uint k = 0; k < GABOR_IMPULSES_PER_CELL; ++k) {
				const float ix = px - Rnd_FloatValue(&seed);
				const float iy = py - Rnd_FloatValue(&seed);
				const float w = Rnd_FloatValue(&seed) < .5f ? -1.f : 1.f;
				const float o = isotropy >= 1.f ? orient :
						orient + (1.f - isotropy) * M_PI_F *
						(2.f * Rnd_FloatValue(&seed) - 1.f);

				float kRe, kIm;
				GaborNoiseTexture_Kernel(ix, iy, freq, o, &kRe, &kIm);
				sumRe += w * kRe;
				sumIm += w * kIm;
			}
		}
	}
	*re = sumRe;
	*im = sumIm;
}

OPENCL_FORCE_INLINE float GaborNoiseTexture_ConstEvaluateFloat(
		const float3 v, const float scale, const float freq,
		const float isotropy, const float orient,
		const uint output, const float sigmaInv) {
	float re, im;
	GaborNoiseTexture_EvalPhasor(v.x * scale, v.y * scale, freq,
			isotropy, orient, &re, &im);

	if (output == 1u) { // GABOR_PHASE
		// atan2(0, 0) is undefined; sparse cells frequently sum to exactly
		// (0, 0) and some GPU fast-math atan2 implementations return NaN
		// or garbage instead of 0.
		const float phase = (re == 0.f && im == 0.f) ? .5f :
				atan2(im, re) * (1.f / (2.f * M_PI_F)) + .5f;
		return phase;
	}
	else if (output == 2u) // GABOR_INTENSITY
		return clamp(sqrt(re * re + im * im) * sigmaInv * .5f, 0.f, 1.f);
	else // GABOR_VALUE
		return clamp(.5f + re * sigmaInv * .5f, 0.f, 1.f);
}

OPENCL_FORCE_NOT_INLINE void GaborNoiseTexture_EvalOp(
		__global const Texture* restrict texture,
		const TextureEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint,
		const float sampleDistance
		TEXTURES_PARAM_DECL) {
	switch (evalType) {
		case EVAL_FLOAT: {
			float3 vec;
			EvalStack_PopFloat3(vec);

			const float eval = GaborNoiseTexture_ConstEvaluateFloat(vec,
					texture->gaborNoiseTex.scale,
					texture->gaborNoiseTex.frequency,
					texture->gaborNoiseTex.isotropy,
					texture->gaborNoiseTex.orientation,
					texture->gaborNoiseTex.output,
					texture->gaborNoiseTex.sigmaInv);
			EvalStack_PushFloat(eval);
			break;
		}
		case EVAL_SPECTRUM: {
			float3 vec;
			EvalStack_PopFloat3(vec);

			const float eval = GaborNoiseTexture_ConstEvaluateFloat(vec,
					texture->gaborNoiseTex.scale,
					texture->gaborNoiseTex.frequency,
					texture->gaborNoiseTex.isotropy,
					texture->gaborNoiseTex.orientation,
					texture->gaborNoiseTex.output,
					texture->gaborNoiseTex.sigmaInv);
			EvalStack_PushFloat3(MAKE_FLOAT3(eval, eval, eval));
			break;
		}
		case EVAL_BUMP_GENERIC_OFFSET_U:
			Texture_EvalOpGenericBumpOffsetU(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP_GENERIC_OFFSET_V:
			Texture_EvalOpGenericBumpOffsetV(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP:
			Texture_EvalOpGenericBump(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

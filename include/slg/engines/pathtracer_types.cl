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

typedef struct {
	unsigned int eyeSampleBootSize, eyeSampleStepSize, eyeSampleSize;

	PathDepthInfo maxPathDepth;

	// Russian roulette
	unsigned int rrDepth;
	float rrImportanceCap;

	// Clamping settings
	float sqrtVarianceClampMaxValue;

	int forceBlackBackground;

	// ReSTIR DI direct light resampling: RIS reservoir over the light
	// picking distribution (the proposal q). The target function is the
	// estimated direct contribution (radiance x geometry / (q * pdfW)).
	// See DirectLight_Illuminate() in pathoclbase_funcs.cl.
	struct {
		int enabled;
		unsigned int candidateCount;
		// Temporal reuse: merge each pixel's stored previous-pass
		// reservoir (lightIndex/wSum/M/target) as extra proposal draws.
		int temporalEnable;
		// Spatial reuse: GRIS merge of hash-grid neighbor reservoirs.
		// The grid cells live in restirReservoirs[reservoirCount ..].
		int spatialEnable;
		// Number of per-pixel temporal reservoirs at the head of the
		// restirReservoirs buffer; the spatial hash grid follows them.
		unsigned int reservoirCount;
	} restir;

	// MNEE (Manifold Next Event Estimation): direct light sampling through
	// a delta specular chain x0 -> ... -> y. The kernel port of
	// PathTracer::MNEEDirectSampling() (single vertex) and
	// PathTracer::MNEEMultiDirectSampling() (multi-specular chain, used
	// when maxSpecular > 1 and the single vertex solve fails).
	// Opt-in with path.mnee.enable; maxIterations mirrors
	// path.mnee.maxiterations, maxSpecular mirrors path.mnee.maxspecular.
	struct {
		int enabled;
		unsigned int maxIterations;
		unsigned int maxSpecular;
	} mnee;

	// Hybrid backward/forward path tracing settings
	struct {
		int enabled;
		float glossinessThreshold;
	} hybridBackForward;

	// Hero-wavelength spectral transport (P2-1 A2): when non-zero the kernel
	// is compiled with -D SLG_SPECTRAL, draws one extra boot dimension for
	// the path wavelengths and treats Spectrum channels as spectral bins.
	unsigned int spectralEnable;

	// PhotonGI cache settings
	struct {
		float glossinessUsageThreshold;
		float indirectLookUpRadius;
		float indirectLookUpNormalCosAngle;
		float indirectUsageThresholdScale;
		unsigned int causticPhotonTracedCount;
		float causticLookUpRadius;
		float causticLookUpNormalCosAngle;

		int indirectEnabled, causticEnabled;

		PhotonGIDebugType debugType;
	} pgic;

	// Albedo AOV settings
	struct {
		AlbedoSpecularSetting specularSetting;
		float specularGlossinessThreshold;
	} albedo;
} PathTracer;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

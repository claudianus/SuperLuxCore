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
		// Spatial reuse (E2b): GRIS merge of screen-space neighbour-
		// pixel reservoirs, gated by hit-point distance and landing
		// normal (same-surface gate).
		int spatialEnable;
		// Number of per-pixel reservoirs in the restirReservoirs buffer
		// (one slot per film pixel; doubles as temporal state and the
		// shareable state for the pixel-space spatial merge).
		unsigned int reservoirCount;
		// Visibility-weighted RIS target (E2a): each candidate's target
		// includes the binary visibility term V evaluated by tracing its
		// shadow ray. Candidates share the tail of the rays/hits buffers
		// (slots [visCandRayBase + gid*K ..]) so the regular per-iteration
		// EnqueueTraceRayBuffer pass traces them together with the normal
		// eye/shadow rays; a dedicated MK_RT_RESTIR state resolves the
		// reservoir on the next iteration. Costs (1+K)x the rays buffer
		// and one extra iteration of latency per path vertex.
		int visibilityEnable;
		// K: candidates traced per task (<= candidateCount, memory bound)
		unsigned int visCandCount;
		// First candidate slot index inside rays[]/rayHits[] (== taskCount)
		unsigned int visCandRayBase;
		// First candidate record index inside restirReservoirs[] (after
		// the per-pixel reservoirs)
		unsigned int visCandDataOffset;
	} restir;

	// ReSTIR GI (G1 GPU): per-pixel first-bounce reservoir resampling
	// (kernel port of RestirGI::ResampleFirstBounce; see
	// dev-tools/restir-gi-design.md). The state lives entirely inside
	// restirReservoirs[] (GI reservoirs + candidate/result records are
	// appended after the DI data) and the GI tail of rays[]/rayHits[]
	// (2K+1 slots per task: K bounce rays, then K NEE shadow rays, then
	// the temporal-merge visibility ray).
	struct {
		int enabled;
		unsigned int candidateCount;	// requested K
		int temporalEnable;
		int spatialEnable;
		// Buffer layout (filled at device init; RestirReservoir-slot
		// units inside restirReservoirs[], ray slots inside rays[]):
		unsigned int reservoirCount;	// GI per-pixel reservoir count
		unsigned int giReservoirOffset;	// first GI reservoir slot
		unsigned int giCandDataOffset;	// first candidate-record slot
		unsigned int giCandStride;		// per-task record block stride
										// (K RestirGICandidate + 1
										// RestirGIResult, slot units)
		unsigned int giCandRayBase;		// first GI tail slot in rays[]
		unsigned int giCandCount;		// effective K (memory clamped)
	} restirGI;

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
		// Manifold seed cache: converged single-vertex solutions are
		// stored in a hashed world-space grid on the occluder and reused
		// as Newton seeds by nearby attempts (warm start; skips the
		// mirror seed trace). path.mnee.seedcache, default on.
		int seedCacheEnable;
	} mnee;

	// Hybrid backward/forward path tracing settings
	struct {
		int enabled;
		float glossinessThreshold;
		// Adaptive caustic partition: the light pass owns path classes
		// the eye side can not sample efficiently (delta terminal or a
		// sharp glossy terminal facing a small light), classified by
		// connection difficulty instead of the fixed glossiness
		// threshold. terminalGlossiness bounds the glossy lobe still
		// worth NEE on the eye side; connectProb is the
		// solid-angle ratio epsilon (omegaLight < eps * omegaLobe).
		int adaptiveCaustic;
		float terminalGlossiness;
		float connectProb;
	} hybridBackForward;

	// GPU light tracing (camera-projection splatting): a second task
	// population in the tail range [eyeTaskCount, eyeTaskCount +
	// lightTaskCount) of every per-task buffer traces light paths and
	// splats their vertices into RADIANCE_PER_SCREEN_NORMALIZED.
	// Implies hybridBackForward on the eye side (caustic suppression).
	struct {
		int enabled;
		// Eye task count (tasks [0, eyeTaskCount)); light tasks live in
		// the tail. eyeTaskCount + lightTaskCount == totalTaskCount.
		unsigned int eyeTaskCount;
		unsigned int lightTaskCount;
		// First camera-visibility ray slot inside rays[]/rayHits[]
		// (one slot per light task, appended after the eye task range)
		unsigned int lightVisRayBase;
		unsigned int lightSampleBootSize, lightSampleStepSize, lightSampleSize;
		// Caustic focus cache (guided light emission, see
		// pathoclbase_datatypes.cl LIGHT_FOCUS_K): aim a fraction of
		// emissions at remembered productive targets. Mixture pdf keeps
		// the estimator unbiased: pdf = (1-ratio)*native + ratio*aim.
		int focusEnable;
		float focusRatio;		// guided-draw probability
		float focusRadiusFrac;	// aim-sphere radius / worldRadius
		// Distant-light caustic focusing: count of delta-specular caster
		// bounding spheres appended to lightFocus[] after the per-light
		// hotspot rings (float4: center.xyz + radius). Their projected
		// discs steer the emit ORIGIN (directions aren't steerable).
		unsigned int focusCasterCount;
	} lightTracing;

	// Hero-wavelength spectral transport (P2-1 A2): when non-zero the kernel
	// is compiled with -D SLG_SPECTRAL, draws one extra boot dimension for
	// the path wavelengths and treats Spectrum channels as spectral bins.
	unsigned int spectralEnable;

	// RIS product guiding (M4b, path.guiding.risk): K > 0 resamples K
	// candidates drawn from the (1-w)*BSDF + w*guide mixture against the
	// product target t = f*|cos|*Lhat. Mirrors PathTracer::guidingRisK.
	unsigned int guidingRisK;

	// Portal-guided bounce sampling (M5, path.portal.*): aperture rects
	// live in the portalRects buffer (4 float4 records each); share caps
	// the one-sample MIS weight. Mirrors PathTracer::portals/portalShare.
	unsigned int portalCount;
	float portalShare;
	float portalSideGate;
	unsigned int portalAdapt;

	// Vertex connection (M6, path.vertexconnection.enable): GPU port of
	// the BIDIRCPU eye x light vertex connect. Requires a light-task
	// population (the light vertex cache lives on light tasks); when set
	// the caustic-only splat gate and the hybrid diffuse-cut are replaced
	// by the SmallVCM MIS weights (misVm/misVc = 0 -> pure BPT).
	struct {
		int enabled;
		// Slots per light task inside lightVertices[]
		// (== maxPathDepth.depth, a bound on stored non-delta vertices)
		unsigned int slotsPerTask;
		// Total records in lightVertices[] (lightTaskCount*slotsPerTask;
		// 0 when the buffer is absent)
		unsigned int vertexCount;
	} vertexConnect;

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

#line 2 "pathoclstatebase_datatypes.cl"

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

//------------------------------------------------------------------------------
// Some OpenCL specific definition
//------------------------------------------------------------------------------

#if defined(SLG_OPENCL_KERNEL)

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#endif

//------------------------------------------------------------------------------
// GPUTask data types
//------------------------------------------------------------------------------

typedef enum {
	// Micro-kernel states
	MK_RT_NEXT_VERTEX = 0,
	MK_HIT_NOTHING = 1,
	MK_HIT_OBJECT = 2,
	MK_DL_ILLUMINATE = 3,
	MK_DL_SAMPLE_BSDF = 4,
	MK_RT_DL = 5,
	MK_GENERATE_NEXT_VERTEX_RAY = 6,
	MK_SPLAT_SAMPLE = 7,
	MK_NEXT_SAMPLE = 8,
	MK_GENERATE_CAMERA_RAY = 9,
	MK_DONE = 10,
	// MNEE (Manifold Next Event Estimation) sub-state machine: solves the
	// specular chain x0 -> x1 -> y after the direct light shadow ray was
	// blocked by a delta specular surface. One trace per render iteration:
	// the state kernel writes the next trace ray into rays[] (with
	// needsTrace = 1), the RT kernel traces it, the state kernel consumes
	// rayHits[] on the next dispatch (needsTrace is cleared there).
	MK_MNEE_NEXT_VERTEX = 11
} PathState;

typedef struct {
	union {
		struct {
			// Must be a power of 2
			unsigned int previewResolutionReduction, previewResolutionReductionStep;
			unsigned int resolutionReduction;
		} rtpathocl;
	} renderEngine;

	Scene scene;
	Sampler sampler;
	PathTracer pathTracer;
	Filter pixelFilter;
	Film film;
} GPUTaskConfiguration;

typedef struct {
	unsigned int lightIndex;
	float pickPdf;

	float directPdfW;

	// Radiance to add to the result if light source is visible
	// Note: it doesn't include the pathThroughput
	Spectrum lightRadiance;
	// This is used only if Film channel IRRADIANCE is enabled and
	// only for the first path vertex
	Spectrum lightIrradiance;

	unsigned int lightID;

	// RIS output weight (ReSTIR DI): carries the resampling factor
	// wSum / (M * target) of the reservoir pick. The direct light
	// sampling pdf stays the proposal q (for MIS consistency with the
	// direct-hit side); the RIS factor multiplies the final factor in
	// DirectLight_BSDFSampling(). Always 1.f when ReSTIR is disabled.
	float risScale;
} DirectLightIlluminateInfo;

// ReSTIR DI per-pixel reservoir state for temporal reuse (persisted
// across passes within a render session). The wSum/M/target triple is
// the REAL accumulated state of that pixel's last depth-0 reservoir:
// merging a stored reservoir with "wSum_new + wSum_prev" over
// "M_new + M_prev" draws is the same RIS as if all proposal draws had
// happened at once (same proposal q, same target family), so the
// combined estimator stays unbiased. "wSum_prev = M_prev * target"
// instead would correlate the merge weight with the stored sample and
// bias the estimator.
typedef struct {
	unsigned int lightIndex;	// NULL_INDEX when there is no reservoir yet
	float wSum;					// sum of the target weights of all draws
	unsigned int M;				// total number of proposal draws
	float target;				// target weight of the winning sample
} RestirReservoir;

// ReSTIR DI spatial reuse (Stage 4 port of restirdi.cpp): the same
// ReservoirEntry layout is reused for hash-grid cells appended after the
// per-pixel temporal reservoirs in the restirReservoirs buffer. The
// hashing and cell size match LightStrategyRestirDI::GridHash/CELL_SIZE.
#define RESTIR_SPATIAL_GRID_SIZE (1u << 14)
#define RESTIR_SPATIAL_CELL_SIZE .5f

// The state used to keep track of the rendered path
typedef struct {
	PathState state;

	Spectrum throughput;
	BSDF bsdf; // Variable size structure

	Seed seedPassThroughEvent;

	// Path guiding (P1-3 M2b-2): vertex-start accumulated radiance
	// (direct+emission channels) for incident-value training records
	float guideRadStart[3];
	
	int albedoToDo, photonGICacheEnabledOnLastHit,
			photonGICausticCacheUsed, photonGIShowIndirectPathMixUsed,
			// The shadow transparency lag used by Scene_Intersect()
			throughShadowTransparency;
} GPUTaskState;

typedef enum {
	ILLUMINATED, SHADOWED, NOT_VISIBLE
} DirectLightResult;

// MNEE chain sub-phase (see MK_MNEE_NEXT_VERTEX):
// - MNEE_PHASE_SEED_TRACE: rays[] holds the mirrored-light seed trace
//   (mirror materials only), not consumed yet.
// - MNEE_PHASE_STEP: no trace pending. Evaluate the constraint at the
//   current vertex, check convergence, compute the Newton step and write
//   the next proposal ray.
// - MNEE_PHASE_PROP_TRACE: rays[] holds a Newton proposal trace, not
//   consumed yet.
// - MNEE_PHASE_SEG2_TRACE: the solve ended; rays[] holds the x1 -> y
//   shadow trace, not consumed yet.
// - MNEE_PHASE_CONTRIBUTION: the seg2 trace is visible; assemble the
//   contribution (arithmetic only, runs in the same launch that consumed
//   the seg2 trace).
// - MNEE_PHASE_MS_DISCOVER: rays[] holds the straight-ray trace that
//   discovers chain vertex chainN (multi-specular chain;
//   MNEEMultiDirectSampling port). chainN vertices are stored so far.
// - MNEE_PHASE_MS_JACPERT: rays[] holds the re-projection of the FD
//   perturbation of vertex chainIdx along axis chainSub (0 = s, 1 = t).
// - MNEE_PHASE_MS_TRIAL: rays[] holds the re-projection of the line
//   search trial position of vertex chainIdx.
// - MNEE_PHASE_MS_COMMIT: rays[] holds the re-projection that rebuilds
//   the accepted trial position of vertex chainIdx.
// - MNEE_PHASE_MS_POST: rays[] holds the final re-projection of vertex
//   chainIdx for the post-solve mode check, the specular factor and the
//   volume update.
typedef enum {
	MNEE_PHASE_SEED_TRACE = 0,
	MNEE_PHASE_STEP = 1,
	MNEE_PHASE_PROP_TRACE = 2,
	MNEE_PHASE_SEG2_TRACE = 3,
	MNEE_PHASE_CONTRIBUTION = 4,
	MNEE_PHASE_MS_DISCOVER = 5,
	MNEE_PHASE_MS_JACPERT = 6,
	MNEE_PHASE_MS_TRIAL = 7,
	MNEE_PHASE_MS_COMMIT = 8,
	MNEE_PHASE_MS_POST = 9
} MneePhase;

// Multi-specular chain capacity (CPU MNEE_MS_MAX_VERTICES, the upper bound
// of path.mnee.maxspecular).
#define MNEE_MS_MAX_VERTICES 4

// 2-vector / 2x2 block with scalar members: OpenCL vector types do not exist
// in the host C++ compile of this file (same reason MneeVertex above uses
// component floats). Kernel code converts to float2/float4 at use sites.
typedef struct {
	float x, y;
} MneeVec2T;

typedef struct {
	float x, y, z, w;
} MneeMat2T;

// The specular chain vertex on the caustic caster surface (the kernel port
// of MneeVertex in pathtracer_mnee.cpp). Component floats instead of
// float3: this file is also compiled as C++ on the host for the buffer
// size computation, and OpenCL vector types do not exist there (they would
// also introduce backend-dependent padding).
typedef struct {
	float px, py, pz;
	float dpduX, dpduY, dpduZ;
	float dpdvX, dpdvY, dpdvZ;
	float nX, nY, nZ;
	float gnX, gnY, gnZ;
	float dnduX, dnduY, dnduZ;
	float dndvX, dndvY, dndvZ;
	// Orthonormal tangents (Zeltner make_orthonormal result)
	float sX, sY, sZ;
	float tX, tY, tZ;
	// Generalized half-vector IOR ratio: +1 same-side mirror reflection,
	// -1 opposite-side mirror reflection, nt/nc for glass transmission.
	float eta;
} MneeVertex;

// Persistent MNEE state, one per task (lives in GPUTaskDirectLight). It
// carries the whole Newton/line-search state across the micro-kernel
// launches of the MNEE sub-state machine.
typedef struct {
	MneePhase phase;
	// Skip the next processing launch: a new trace ray has just been
	// written into rays[] by the current iteration, so rayHits[] still
	// holds the result of the previous trace.
	int needsTrace;

	float lightPosX, lightPosY, lightPosZ;
	unsigned int shadowMeshIndex;
	// 1 = mirror occluder (eta = ±1 by the side test), 0 = glass occluder
	int mirrorMode;

	MneeVertex vtx;

	// Newton / line search state
	float beta;
	unsigned int iteration;
	float resNorm;

	// Multi-specular chain state (MNEEMultiDirectSampling port). chainN ==
	// 0 selects the single vertex solver above; chainN >= 2 an N-vertex
	// chain in chainVtx. Adds ~1.2KB per task (GPUTaskDirectLight is sized
	// from this struct on the host); acceptable on the Metal validation
	// machine, revisit with a pooled scratch buffer for small VRAM.
	int chainN;
	int chainMaxV;
	MneeVertex chainVtx[MNEE_MS_MAX_VERTICES];
	// Kernel material type (MIRROR/GLASS) per chain vertex, used by the
	// trial re-projection check and the post-solve mode check.
	unsigned int chainMatType[MNEE_MS_MAX_VERTICES];
	// Block tridiagonal FD Jacobian (prev/cur/next per vertex) and the
	// base residuals, filled by the MS_JACPERT phases and reused by the
	// geometric term (same vertex, so identical to a fresh evaluation).
	MneeMat2T chainJacPrev[MNEE_MS_MAX_VERTICES];
	MneeMat2T chainJacCur[MNEE_MS_MAX_VERTICES];
	MneeMat2T chainJacNxt[MNEE_MS_MAX_VERTICES];
	MneeVec2T chainRes[MNEE_MS_MAX_VERTICES];
	MneeVec2T chainTrialRes[MNEE_MS_MAX_VERTICES];
	MneeVec2T chainDx[MNEE_MS_MAX_VERTICES];
	// Phase cursors (vertex index / perturbation axis) and the trial
	// projection flag.
	int chainIdx;
	int chainSub;
	int chainProjected;
	// Accumulated specular product over the MS_POST phases.
	float chainSpecR, chainSpecG, chainSpecB;

	// Solve-end results consumed by the contribution launch
	float specFactorR, specFactorG, specFactorB;
	float geometricTerm;
	// BSDFEvent kept as int: this file is also compiled as C++ on the host
	// for the buffer size computation, where the kernel BSDFEvent enum is
	// not visible.
	int specEvent;
	// 1 when the solved constraint is the plain half-vector (vertex eta == 1,
	// the same-side reflection case): only then does the light weight carry
	// the r12^2 (directPdfW2) measure factor. Any eta != 1 (dielectric
	// transmission, opposite-side mirror law) makes the analytic geometric
	// term carry the full measure conversion instead.
	int plainHalfVector;
	float lightRadiance2R, lightRadiance2G, lightRadiance2B;
	float directPdfW2;
	// Pass-through event of the x1 -> y shadow trace (hashed, kept across
	// the launch boundary)
	float seg2PassThrough;
} MneeState;

typedef struct {
	// Used to store some intermediate result
	DirectLightIlluminateInfo illumInfo;

	Seed seedPassThroughEvent;

	DirectLightResult directLightResult;

	// The shadow transparency flag used by Scene_Intersect()
	int throughShadowTransparency;

	// MNEE specular chain solver state (MK_MNEE_NEXT_VERTEX) and the two
	// BSDF slots it needs: mneeBsdf is the trace target of the in-flight
	// MNEE proposal, mneeBsdfFinal is the BSDF of the current chain vertex
	// (the accepted proposal, the seed trace or the shadow-ray occluder).
	BSDF mneeBsdf, mneeBsdfFinal;
	MneeState mnee;
} GPUTaskDirectLight;

typedef struct {
	// The task seed
	Seed seed;

	// Space for temporary storage
	BSDF tmpBsdf; // Variable size structure

	// This is used by TriangleLight_Illuminate() to temporary store the
	// point on the light sources.
	// Also used by Scene_Intersect() for evaluating volume textures.
	HitPoint tmpHitPoint;
	
	// This is used by DirectLight_BSDFSampling()
	PathDepthInfo tmpPathDepthInfo;
} GPUTask;

typedef struct {
	unsigned int sampleCount;
} GPUTaskStats;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

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
	MK_MNEE_NEXT_VERTEX = 11,
	// ReSTIR DI visibility-weighted target (E2a): MK_DL_ILLUMINATE queued
	// K candidate shadow rays into rays[visCandRayBase + gid*K ..]; the
	// trace pass resolved them; this state folds the binary visibility
	// into each candidate's target, runs the reservoir merge, emits the
	// winner's real shadow ray into rays[gid] and continues to
	// MK_DL_SAMPLE_BSDF.
	MK_RT_RESTIR = 12,
	// ReSTIR GI (G1 GPU): two-stage tail resolution of the first-bounce
	// candidate rays queued by MK_GENERATE_NEXT_VERTEX_RAY.
	// - MK_RT_GI_BOUNCE consumes the bounce-ray hits: builds each hit
	//   candidate's x2 BSDF (tmpBsdf, sequentially), records x2's
	//   emission and queues the one-sample NEE shadow ray into the
	//   second half of the task's GI tail.
	// - MK_RT_GI_RESOLVE consumes the NEE hits, assembles the proxy
	//   targets, runs the RIS + temporal/spatial merges, stores the
	//   pre-spatial reservoir and hands the winner to
	//   MK_GENERATE_NEXT_VERTEX_RAY through the task's result record.
	MK_RT_GI_BOUNCE = 13,
	MK_RT_GI_RESOLVE = 14
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
	// E2b screen-space spatial reuse: the storing vertex's hit point and
	// landing normal. A neighbour reservoir is merged only when the two
	// shading points are geometrically similar (same-surface gate): that
	// keeps the pi_new/pi_old GRIS ratio near 1 - which is what makes
	// the merge reduce variance instead of adding it - and rejects
	// silhouette neighbours whose stored targets do not transfer.
	float hitP[3];
	float geomN[3];
	// Winning sample's light-surface draws (the two surface coords plus
	// the pass-through event). A neighbour merge replays the SAME light
	// point at the current shade point - the reconnection shift of GRIS
	// - so pi_new/pi_old reflects only the shading difference, not a
	// different point on the emitter (tighter ratio, less merge noise).
	// Zero-filled slots replay the degenerate (0,0,0) sample - same as
	// the old fixed-point merge.
	float lsU, lsV, lsP;
	float pad;
} RestirReservoir;

// ReSTIR DI visibility-weighted target (E2a): one record per candidate
// shadow ray queued into the rays[] tail. The candidate's own light
// sample is reused as the contribution sample (no re-sampling of the
// winner): the binary V test then covers the exact ray the estimator
// pays off, keeping V and the contribution consistent.
// Slots [K, K + RESTIR_PIXEL_MERGES_MAX) of each task's tail are merge
// candidates (E2d visibility-aware spatial shift): the ray replays a
// gated neighbour reservoir's stored light-surface sample, so the hit
// test folds real visibility into pi_new of the GRIS merge weight
// instead of the V-free approximation. For merge records nbrWSum /
// nbrTarget / nbrM carry the neighbour reservoir's stored state;
// lsU/lsV/lsP is the replayed light-surface sample (stored on fresh
// candidates too so the winner's sample needs no re-derivation).
typedef struct {
	unsigned int lightIndex;	// NULL_INDEX for culled draws
	float pickPdf;				// proposal pdf q of the light pick
	float directPdfW;			// light sample's pdf (solid angle)
	float target;				// unshadowed target Y(rad)/(q*pdfW)
	float radianceR, radianceG, radianceB; // Illuminate() radiance
	float lsU, lsV, lsP;		// light-surface sample the ray covers
	// Merge-candidate metadata (unused on fresh candidates)
	float nbrWSum;				// neighbour reservoir wSum
	float nbrTarget;			// neighbour stored target (pi_old)
	unsigned int nbrM;			// neighbour reservoir M
	float pad;
} RestirVisCandidate;

// ReSTIR DI spatial reuse (E2b): screen-space neighbour-pixel merge.
// Two pseudo-random offsets in a 5x5 window (centre excluded) are drawn
// per shade point; a candidate neighbour reservoir is merged only when
// it passes the same-surface geometric gate below. Pixel neighbours
// mostly shade the same surface, so their stored winners transfer with
// a pi_new/pi_old ratio near 1 - unlike the previous world-space hash
// grid, which merged unrelated surfaces and measured ~1.1-1.3x WORSE
// RMSE on the manylights scenes (e14). The CPU strategy keeps its
// world-space hash grid (it has no pixel context). Under the
// visibility-weighted target the CPU side restricts merging to the
// temporal/own-cell reservoir; the GPU side still runs this gated
// pixel merge as a documented approximation.
#define RESTIR_PIXEL_MERGES_MAX 2u
// Position gate: 2% of the world radius (squared value, multiplied by
// worldRadius^2 at the use site); normal gate: ~25 degrees.
#define RESTIR_PIXEL_MERGE_DIST2 ((0.02f * 0.02f))
#define RESTIR_PIXEL_MERGE_NORM 0.9063f
// Defensive bound on the GRIS reconnection ratio pi_new/pi_old: the
// same-surface gate keeps legit ratios near 1, but a steep emission
// gradient (spot/projection cone edge) can still produce a large
// outlier that inflates wSum - and the paid contribution scales
// linearly with wSum, so one outlier merge becomes a hot pixel
// (measured ~9-20x RMSE tail events on e14). Clamping the ratio at
// 64 bounds the per-merge inflation while leaving real transfers
// untouched; the bias is bounded and only affects the outlier tail.
#define RESTIR_MERGE_MAX_TARGET_RATIO 64.f

// ReSTIR GI (G1 GPU): per-pixel first-bounce reservoir, appended to
// restirReservoirs[] after the DI data (see taskConfig.pathTracer.
// restirGI.giReservoirOffset). Mirrors the CPU RestirGI::Reservoir
// semantics: the stored winner is a candidate bounce direction with
// its x2 endpoint and one-sample proxy radiance lHat; a merge
// reconnects x2 to the current x1 and carries the Jacobian of the
// solid-angle shift.
typedef struct {
	float x1X, x1Y, x1Z;		// shading vertex the reservoir was stored at
	float x1nX, x1nY, x1nZ;		// x1 geometry normal (same-surface gate)
	float x2X, x2Y, x2Z;		// candidate bounce hit point (isMiss == 0)
	float x2nX, x2nY, x2nZ;		// x2 geometry normal (Jacobian)
	float dirX, dirY, dirZ;		// winning direction from x1
	float lHatR, lHatG, lHatB;	// stored one-sample proxy radiance at x2
	float wSum;
	float target;				// winner's pi_hat at storage time
	unsigned int m;				// proposal draws (0 = no reservoir yet)
	unsigned int isMiss;		// winner hit the environment
	// Writer's sample-pass stamp, used as a seqlock: the store writes
	// 0xFFFFFFFF first, fills the payload, then publishes the stamp
	// last. Temporal merges read it before AND after snapshotting the
	// entry (and pair it with RestirGIResult.vSeq for the visibility
	// ray), so a torn or superseded record is always rejected. Unlike
	// the CPU side (a handful of threads rarely collide on a pixel),
	// thousands of GPU tasks hit the same pixel's reservoir inside a
	// single pass: without the stamp the temporal merge would fold a
	// just-written wSum back into itself several times per pass and
	// the GRIS weight compounds geometrically (measured ~1e7x hot
	// pixels on cornell). Temporal merges only accept entries stamped
	// with a strictly older pass; spatial merges intentionally keep
	// same-pass neighbours (that IS the spatial semantics).
	unsigned int pass;
} RestirGIReservoir;

// ReSTIR GI (G1 GPU): one record per candidate bounce ray queued into
// the GI tail of rays[]/rayHits[] (slots [giCandRayBase + gid*2K ..];
// the second half holds the NEE shadow rays resolved in
// MK_RT_GI_RESOLVE). The x2 hit point itself is recomputed from
// ray+hit at resolve time; only data not recoverable from the trace
// results is stored here.
typedef struct {
	float dirX, dirY, dirZ;		// candidate direction from x1
	float fcosR, fcosG, fcosB;	// f_r * |cos| at x1
	float pdfW;					// BSDF proposal pdf (0 = culled)
	unsigned int event;			// BSDFEvent of the proposal
	unsigned int miss;			// 0 = hit, 1 = env miss (NEE slot masked)
	float x2nX, x2nY, x2nZ;		// x2 geometry normal (hit only)
	float emisR, emisG, emisB;	// x2 emission toward x1, or env radiance
	float neeR, neeG, neeB;		// lightRad * eval2 / (pdfW2 * pickPdf);
								// the binary V is folded in at resolve
} RestirGICandidate;

// ReSTIR GI (G1 GPU): resolve -> MK_GENERATE_NEXT_VERTEX_RAY handoff.
// The resolve cannot write the continuation ray itself (the AddVertex/
// RR/throughput bookkeeping lives in MK_GENERATE), so it records the
// resampled (dir, fcos*W, risPdfW, event) triple here and re-enters
// MK_GENERATE, which consumes it in place of a fresh BSDF draw.
// pending: 0 = no GI decision, 1 = consume this record, 2 = the
// resolve found no usable winner -> take a normal BSDF sample (the
// same conditioned fallback as the CPU side's "return false").
typedef struct {
	float dirX, dirY, dirZ;
	float bsdfR, bsdfG, bsdfB;	// fcos * W continuation factor
	float pdfW;					// risPdfW: RIS marginal selection density
	unsigned int event;
	unsigned int pending;
	// Dense-dispatch trace barrier (same role as MneeState.needsTrace):
	// MK_RT_GI_BOUNCE sets it when it queues the NEE shadow rays, so the
	// MK_RT_GI_RESOLVE launch in the SAME advance pass skips the task
	// (the NEE hits only exist after the next trace).
	unsigned int needsTrace;
	// Seqlock tag for the temporal visibility ray: MK_RT_GI_BOUNCE
	// records the pass stamp of the stored entry it queues the ray
	// against; MK_RT_GI_RESOLVE accepts the merge only if the entry's
	// stamp still matches, so a reservoir rewritten between bounce and
	// resolve can never pair a stale occlusion test with a new sample.
	// 0xFFFFFFFF = no ray queued.
	unsigned int vSeq;
} RestirGIResult;

// MNEE seed cache (E4): fixed-size hash grid of converged single-vertex
// manifold solutions. 16384 entries * 32B = 512KB.
#define MNEE_SEED_CACHE_SIZE (1u << 14)
// Quantization cell of the occluder-hit key, in world units relative to
// the scene bounding sphere radius (divided by this value).
#define MNEE_SEED_CELL_FRAC 64.f

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

// Manifold seed cache entry (E4): a converged single-vertex MNEE solution
// stored in a fixed-size hashed grid, keyed by (light, occluder mesh,
// quantized shadow-ray occluder hit position). A nearby attempt can warm-
// start the Newton solve from the cached vertex instead of the line seed /
// mirrored-light seed trace. The seed only selects the Newton basin: the
// solve still verifies the half-vector constraint, so a stale or torn entry
// can cost iterations but never biases the result.
// 32 bytes.
typedef struct {
	// Solved vertex position and shading normal
	float vx, vy, vz;
	float nx, ny, nz;
	unsigned int lightIndex;
	unsigned int meshIndex;
	// 1 = mirror-mode solve, 0 = glass-mode solve
	unsigned int mirrorMode;
	unsigned int valid;
} MneeSeedEntry;

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
	// Shadow-ray occluder hit position: the seed-cache key component,
	// remembered from Mnee_Start for the store on solve success.
	float occlX, occlY, occlZ;

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

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
	MK_RT_GI_RESOLVE = 14,
	// GPU light tracing (doc/features/gpu_lighttracing.md): light-path
	// task states. MK_LIGHT_INIT samples the emitter and writes the
	// emission ray into rays[gid]; the fused consume kernel
	// MK_LIGHT_VERTEX resolves the pending camera-connect splat, consumes
	// the path hit, queues the camera-visibility ray into the
	// lightVisRayBase tail slot and continues the path - one iteration
	// per vertex, self-loop state.
	MK_LIGHT_INIT = 15,
	MK_LIGHT_VERTEX = 16,
	// Vertex connection (M6, GPU BDPT): sits between MK_RT_DL and
	// MK_GENERATE_NEXT_VERTEX_RAY. At every non-delta eye vertex the
	// kernel connects to the stored vertices of the paired light task's
	// subpath (the light vertex cache lightVertices[]) and traces each
	// connect shadow ray inline through Scene_Intersect - the rays[]
	// slot is free because the next bounce ray has not been generated
	// yet. Weighted by the SmallVCM MIS terms (misVm/misVc = 0 -> BPT).
	MK_VC_CONNECT = 17
} PathState;

// VC light vertex cache record: slot (t, k) of lightVertices[] holds
// the depth-(k+1) non-delta vertex of light task t's current subpath.
// Stale-but-consistent records (from an earlier, longer subpath) are
// still unbiased vertex samples - only field-level tearing is a hazard,
// which the seqlock excludes (odd seq = mid-write, changed seq = torn).
typedef struct {
	// Full vertex state (hitPoint + material index + frame): BSDF_Evaluate
	// on the stored copy reproduces the vertex's scattering response
	BSDF bsdf;
	// Light-subpath throughput up to this vertex (excludes the connect edge)
	float throughputR, throughputG, throughputB;
	// SmallVCM MIS bookkeeping of the light prefix. dVM is carried for
	// the vertex-merging strategy (M7); it stays inert when merging is
	// disabled (misVcWeightFactor = 0 -> pure BDPT like before)
	float dVCM, dVC, dVM;
	unsigned int lightID;
	// 1-based light-subpath depth (mirrors PathVertexVM::depth)
	unsigned int depth;
	// Seqlock: 0 = never written, odd = write in flight, even = stable
	unsigned int seq;
} VCLightVertex;

// Temporal connect reuse (M7d, ReSTIR-BDPT-style vertex replay):
// per-EYE-task persistent single-slot reservoir holding a copy of the
// light vertex that landed the most connect luminance at this task so
// far. The next eye vertex replays it as one extra deterministic
// connect candidate - same BSDF evaluations, MIS weights and shadow
// ray as a pool candidate - so the replayed term is an honest strategy
// sample and the estimator stays unbiased: the reservoir only chooses
// WHICH stale vertex gets a seat, it never weights the estimate.
// `staging` holds the last queued pool candidate's record (the vertex
// cache slot can be overwritten before the shadow ray resolves), so
// the resolve pass promotes staging -> vertex when the landed
// luminance beats the stored score. Owned by the eye task alone - no
// atomics, no cross-task writes.
typedef struct {
	VCLightVertex vertex;	// vertex.seq == 0 -> empty slot
	float score;			// best landed connect luminance
	VCLightVertex staging;
	float pad;
} VCReplay;

// Vertex merging (M7, Georgiev'12 VCM): the light-vertex cache is
// indexed by a spatial hash rebuilt every iteration - VC_MERGE_BUCKETS
// power-of-two cells (cell size = mergeRadius), each holding up to
// VC_MERGE_CAPACITY flat vertex indices. vcMergeHash layout:
// [0, BUCKETS) = atomic fill counters, [BUCKETS, BUCKETS*(1+CAP)) =
// per-bucket index lists.
#define VC_MERGE_BUCKETS 65536u
#define VC_MERGE_CAPACITY 16u
// VCMergeCellHash lives in pathoclbase_funcs.cl (this header is also
// compiled as host C++)

// Caustic focus cache: per-light ring of the last LIGHT_FOCUS_K world
// positions where a successfully-splatted light path crossed its first
// delta-specular surface. MK_LIGHT_INIT aims a fraction of light
// emissions at a sampled hotspot - the ring's contents are the empirical
// distribution of productive emission targets (frequency-weighted by
// construction), so no explicit weights are stored.
#define LIGHT_FOCUS_K 32

// GPU light tracing: per light-task path state (the eye-side
// EyePathInfo analogue). The pending camera-connect splat is deferred
// one iteration because the visibility ray resolves in the trace pass
// after it is queued.
// Component floats, not float3: this file is also compiled as C++ on
// the host for the buffer size computation.
typedef struct {
	PathDepthInfo depth;
	PathVolumeInfo volume;
	// Copies of `depth`/`volume` for the camera-visibility ray (the
	// connect ray must not mutate the path state)
	PathVolumeInfo connectVolInfo;
	PathDepthInfo connectDepth;
	int connectThroughShadow;

	int lastBSDFEvent;
	float lastBSDFPdfW;
	float lastGlossiness;
	float lastShadeNX, lastShadeNY, lastShadeNZ;
	int lastFromVolume;

	int isNearlyS, isNearlySD, isNearlySDS;

	// Adaptive caustic partition: all vertices so far are non-diffuse
	// (SPECULAR|GLOSSY), the widened-chain counterpart of isNearlyS.
	int isAdaptiveS;
	// Light-adjacent vertex (v1): the terminal of the eye-side chain.
	// Its lobe width vs the light's solid angle decides eye-side
	// connection difficulty. Lobe is 0 for delta terminals.
	float firstVertPX, firstVertPY, firstVertPZ;
	float firstVertGloss;
	int firstVertDelta;

	// The emitted light source (index into lights[]) and its group ID
	unsigned int lightIndex, lightGroupID;

	// Position of the first delta-specular vertex of the path (the
	// caustic-generating bounce) - credited to the emitting light's
	// focus cache when a camera connect splats
	float firstDeltaPX, firstDeltaPY, firstDeltaPZ;
	int hasDeltaVertex;

	// Sampled lens point for the camera connects of this path
	float lensPointX, lensPointY, lensPointZ;

	// The path terminated (miss/depth/RR); the pending splat still has
	// to be resolved before the task moves to the next light sample
	int pathDone;

	// Vertex connection (M6) light-prefix MIS bookkeeping, updated per
	// CPU BiDirCPURenderThread::TraceLightPath/Bounce. Snapshotted into
	// each VCLightVertex record. vcVertexCount is the number of valid
	// slots of this task's current subpath inside lightVertices[]
	// (the paired eye task connects to slots [0, vcVertexCount)).
	float dVCM, dVC;
	// Vertex-merging bookkeeping (M7): same recurrence as dVC with the
	// misVcWeightFactor cross-term (SmallVCM SubPathState::dVM)
	float dVM;
	unsigned int vcVertexCount;

	// A camera connect blocked by a delta occluder is being solved by
	// the light-side manifold walk (LMNEE, doc/features/gpu_lighttracing.md);
	// the path stalls in MK_LIGHT_VERTEX while the solve runs on the
	// visibility-ray slot
	int mneeActive;

	// Deferred camera-connect splat: the visibility ray in the
	// lightVisRayBase tail slot is resolved by the trace pass of the
	// following iteration(s)
	struct {
		float filmX, filmY;
		// throughput * bsdfEval * fluxToRadianceFactor (the visibility
		// ray's connectionThroughput is multiplied in at resolve time)
		float radianceR, radianceG, radianceB;
		unsigned int lightGroupID;
		// CPU addonlycaustics contract: only (nearly-)caustic light
		// connections reach the screen channel; the eye side owns the
		// rest (double counting otherwise)
		int isCaustic;
		// The queued visibility ray is a solved-manifold endpoint
		// segment (LMNEE), not a straight connect: if it is blocked the
		// occluder is the chain's exit interface and the light-side
		// multi-vertex solve takes over instead of a fresh LMnee_Start
		int fromMnee;
		// Receiver position x0 of a solved-manifold connect (LMNEE): a
		// camera-productive surface point credited to the emitting
		// light's focus ring (manifold-guided emission)
		float recvPX, recvPY, recvPZ;
		// Cryptomatte ids of the receiver surface (the splat's visible
		// surface, CPU ConnectToEye parity)
		float cryptoObjectID, cryptoMaterialID;
		int valid;
	} pendingSplat;
} LightPathInfo;

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

	// Vertex connection (M6): the emission-hit densities returned by
	// Illuminate() - CPU DirectLightSampling weightCamera terms
	// (emissionPdfW / cosThetaAtLight).
	float emissionPdfW, cosThetaAtLight;
	// Vertex connection (M6): the BDPT misWeight applied to
	// lightRadiance - stored so MK_RT_DL can lift it when the shadow ray
	// crossed a shadow-transparent occluder (CPU overrides misWeight to
	// 1 in that case).
	float vcMisWeight;
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

	// RIS product guiding (M4b): the bounce winner resampled in
	// MK_HIT_OBJECT (post-BSDF, pre-DL - the CPU draws candidates before
	// DirectLightSampling so the DL MIS sees pHat = t/zHatMis; the same
	// ordering is preserved across the state machine). risZhat > 0 marks
	// an active winner: MK_DL_SAMPLE_BSDF folds risZhatMis into the
	// bounce density and MK_GENERATE_NEXT_VERTEX_RAY applies dir/wt.
	float risZhat, risZhatMis;
	float risDirX, risDirY, risDirZ;
	float risWtR, risWtG, risWtB;
	float risPHat;
	// Winner's draw uniforms: a field-side winner re-runs a shadow BSDF
	// draw in the bounce kernel for single-lobe event bookkeeping
	float risUD0, risUD1;
	unsigned int risEvent;
	unsigned int risSideBsdf;

	// Portal bounce proposal (M5): decided in MK_HIT_OBJECT (post-RIS,
	// pre-DL) because the DL-side MIS folds wP*PortalPdfW into the bounce
	// density - the kernel order mirrors the CPU mirrored predicates.
	// portalW > 0 marks an active proposal (effective share at this
	// vertex); portalTake is the hashed selector outcome.
	float portalW;
	unsigned int portalTake;

	// Vertex connection (M6): one light-vertex connect per advance
	// iteration (the rays[gid] slot resolves in the next trace pass,
	// same contract as the DL shadow ray). vcCursor is the next light
	// vertex slot of the paired light task to evaluate; vcPending marks
	// a connect shadow ray in flight. The pending record carries
	// misWeight*geometryTerm*eyeBsdfEval*lightBsdfEval*lightThroughput -
	// folded with the shadow ray's connectionThroughput at resolve time.
	unsigned int vcCursor;
	unsigned int vcPending;
	unsigned int vcPendingLightID;
	unsigned int vcPendingEvent;
	float vcPendingR, vcPendingG, vcPendingB;
	// Sum of the candidate scores over the connect pool of the current
	// eye vertex (evaluated once when the cursor starts). Drives the
	// probabilistic-connection inclusion probability q_i (M7).
	float vcScoreSum;
	// Flat candidate index that queued the in-flight connect ray
	// (vcPendingCand == vcPoolSize marks the M7d replay candidate -
	// needed at resolve to tell pool candidates, which feed the
	// replay reservoir, from the replay itself)
	unsigned int vcPendingCand;

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
	// 1 = directional endpoint: lightPosX/Y/Z holds the constant unit
	// direction toward the light (wo is constant, the endpoint Jacobian is in
	// direction space, no finite emitter position). 0 = point-like emitter
	// position.
	int lightIsDir;
	// 1 = at least one manifold vertex is dispersive glass (cauchyB > 0): the
	// solve is hero-wavelength only and the connect contribution gets the
	// hero-bin collapse at contribution assembly (Spectral_KeepHeroBins).
	int dispersive;
	unsigned int shadowMeshIndex;
	// Incident side of the connect ray on the blocker (1 = ray hits the
	// -geometryN side, i.e. exiting a dielectric). Namespaces the seed
	// cache as meshIndex*2+shadowSide: a vertex solved for the opposite
	// side sits in the wrong Newton basin (CPU pathtracer_mnee.cpp parity).
	unsigned int shadowSide;
	// 1 = mirror occluder (eta = ±1 by the side test), 0 = glass occluder
	int mirrorMode;
	// Shadow-ray occluder hit position: the seed-cache key component,
	// remembered from Mnee_Start for the store on solve success.
	float occlX, occlY, occlZ;

	// 1 when the seed cache was already consulted for this attempt (mirror
	// cache-first hit, or the failure-rescue retry). The cache is a rescue
	// only: for glass the free cold line seed keeps the reference basin
	// selection and a cached vertex only re-seeds a FAILED solve - the
	// cache-first policy measured ~5% caustic energy loss on multi-root
	// casters by pinning nearby attempts to the first-cached basin.
	unsigned int seedCacheTried;

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
	// 1 while the discovery walk is inside a dielectric body (entered a
	// glass interface, not yet exited): a matte hit then is geometry
	// intruding into the glass and gets stepped past so the chain still
	// collects the exit interface (CPU MneeChainDiscover parity).
	int walkInGlass;
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

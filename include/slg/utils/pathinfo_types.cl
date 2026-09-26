#line 2 "pathinfo_types.cl"

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
	PathDepthInfo depth;
	PathVolumeInfo volume;

	int isPassThroughPath;

	// Last path vertex information
	BSDFEvent lastBSDFEvent;
	float lastBSDFPdfW;
	float lastGlossiness;
	Normal lastShadeN;
	bool lastFromVolume, isTransmittedPath;
	// The last vertex restricts direct-light sampling to infinite lights
	// (shadow catcher): its NEE proposal was the infinite distribution, so
	// DirectHit MIS must measure the hit against that same distribution
	bool lastOnlyInfiniteLights;
	// Light linking: the last vertex's receiver accept mask (~0 before
	// the first surface vertex / after a volume vertex = accepts all)
#if defined(SLG_OPENCL_KERNEL)
	ulong linkAcceptMask;
#else
	u_longlong linkAcceptMask;
#endif

	int isNearlyCaustic;
	// Specular, Specular+ Diffuse and Specular+ Diffuse Specular+ paths
	int isNearlyS, isNearlySD, isNearlySDS;

	// Adaptive caustic partition (hybridBackForward.adaptivecaustic):
	// like isNearlyCaustic but the chain accepts any non-diffuse event
	// (glossy of any glossiness), so boundary-glossy interior vertices
	// stay inside the light-tracing-owned class. The terminal
	// (light-adjacent) vertex is gated separately by connection
	// difficulty.
	int isAdaptiveCaustic;

	// Vertex connection (M6) eye-prefix MIS bookkeeping, per CPU
	// BiDirCPURenderThread. dVM (M7) carries the vertex-merging
	// bookkeeping; with merging disabled it stays at the same values
	// as dVC (misVcWeightFactor = 0 -> pure BDPT as before).
	// dVCM inits to MIS(1/cameraPdfW) at MK_GENERATE_CAMERA_RAY.
	float dVCM, dVC, dVM;
	// The MIS fold applied at the last hit (dVCM *= vcFoldVCM,
	// dVC *= vcFoldVC) - stored so a pass-through vertex (not a real
	// vertex on the CPU side) can undo it at bounce time.
	float vcFoldVCM, vcFoldVC;

	// LPE: live NFA state set per film.lpe.N.expression (u32 bitmask
	// each), seeded from the automaton's startAfterC in GenerateEyePath
	// and stepped per vertex in EyePathInfo_AddVertex (lpe_funcs.cl)
	unsigned int lpeStates[SLG_LPE_MAX_EXPRESSIONS];
} EyePathInfo;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

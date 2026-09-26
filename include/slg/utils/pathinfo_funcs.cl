#line 2 "pathinfo_funcs.cl"

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
// EyePathInfo
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void EyePathInfo_Init(__global EyePathInfo *pathInfo) {
	PathDepthInfo_Init(&pathInfo->depth);
	PathVolumeInfo_Init(&pathInfo->volume);
	
	pathInfo->isPassThroughPath = true;

	pathInfo->lastBSDFEvent = SPECULAR; // SPECULAR is required to avoid MIS
	pathInfo->lastBSDFPdfW = 1.f;
	pathInfo->lastGlossiness = 0.f;
	pathInfo->lastFromVolume = false;
	pathInfo->isTransmittedPath = true;
	pathInfo->lastOnlyInfiniteLights = false;

	pathInfo->isNearlyCaustic = false;
	pathInfo->isNearlyS = false;
	pathInfo->isNearlySD = false;
	pathInfo->isNearlySDS = false;
	pathInfo->isAdaptiveCaustic = false;

	// Vertex connection (M6) eye-prefix MIS bookkeeping - the real init
	// (dVCM = MIS(1/cameraPdfW)) happens in GenerateEyePath after the
	// camera ray exists
	pathInfo->dVCM = 0.f;
	pathInfo->dVC = 0.f;
	pathInfo->vcFoldVCM = 1.f;
	pathInfo->vcFoldVC = 1.f;
}

OPENCL_FORCE_INLINE bool EyePathInfo_UseRR(__global EyePathInfo *pathInfo, const uint rrDepth) {
	return !(pathInfo->lastBSDFEvent & SPECULAR) &&
			(PathDepthInfo_GetRRDepth(&pathInfo->depth) >= rrDepth);
}

OPENCL_FORCE_INLINE bool EyePathInfo_IsNearlySpecular(__global EyePathInfo *pathInfo,
		const BSDFEvent event, const float glossiness, const float glossinessThreshold) {
	return (event & SPECULAR) || ((event & GLOSSY) && (glossiness <= glossinessThreshold));
}

OPENCL_FORCE_INLINE bool EyePathInfo_CanBeNearlySpecular(__global EyePathInfo *pathInfo,
		__global const BSDF *bsdf, const float glossinessThreshold
		MATERIALS_PARAM_DECL) {
	const BSDFEvent eventTypes = BSDF_GetEventTypes(bsdf MATERIALS_PARAM);

	return (eventTypes & SPECULAR) || ((eventTypes & GLOSSY) && (BSDF_GetGlossiness(bsdf MATERIALS_PARAM) <= glossinessThreshold));
}

OPENCL_FORCE_INLINE void EyePathInfo_AddVertex(__global EyePathInfo *pathInfo,
		__global const BSDF *bsdf, const BSDFEvent event, const float pdfW,
		const float glossinessThreshold
		MATERIALS_PARAM_DECL) {
	//--------------------------------------------------------------------------
	// PathInfo::AddVertex() inlined here for performances
	//--------------------------------------------------------------------------

	// Increment path depth information
	PathDepthInfo_IncDepths(&pathInfo->depth, event);

	// Update volume information
	PathVolumeInfo_Update(&pathInfo->volume, event, bsdf MATERIALS_PARAM);

	const float glossiness = BSDF_GetGlossiness(bsdf MATERIALS_PARAM);
	const bool isNewVertexNearlySpecular = EyePathInfo_IsNearlySpecular(pathInfo,
			event, glossiness, glossinessThreshold);

	// Update isNearlySDS (must be done before isNearlySD)
	pathInfo->isNearlySDS = (pathInfo->isNearlySD || pathInfo->isNearlySDS) && isNewVertexNearlySpecular;

	// Update isNearlySD (must be done before isNearlyS)
	pathInfo->isNearlySD = pathInfo->isNearlyS && !isNewVertexNearlySpecular;

	// Update isNearlySpecular
	pathInfo->isNearlyS = ((pathInfo->depth.depth == 1) || pathInfo->isNearlyS) && isNewVertexNearlySpecular;

	// Update last path vertex information
	pathInfo->lastBSDFEvent = event;

	//--------------------------------------------------------------------------
	// EyePathInfo::AddVertex()
	//--------------------------------------------------------------------------

	// Update 
	//
	// Note: depth.depth has been already incremented by 1 with the depth.IncDepths(event);
	pathInfo->isNearlyCaustic = (pathInfo->depth.depth == 1) ? 
		// First vertex must a nearly diffuse
		(!isNewVertexNearlySpecular) :
		// All other vertices must be nearly specular
		(pathInfo->isNearlyCaustic && isNewVertexNearlySpecular);

	// Adaptive partition: the receiver (first vertex) only has to be
	// non-delta (a glossy receiver can still connect to the lens); every
	// later vertex must be non-diffuse so the chain concentrates light.
	pathInfo->isAdaptiveCaustic = (pathInfo->depth.depth == 1) ?
		!(event & SPECULAR) :
		(pathInfo->isAdaptiveCaustic && ((event & (SPECULAR | GLOSSY)) != 0));

	// Update last path vertex information
	pathInfo->lastBSDFPdfW = pdfW;
	const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
	VSTORE3F(bsdf->hitPoint.intoObject ? shadeN : -shadeN, &pathInfo->lastShadeN.x);
	pathInfo->lastFromVolume =  bsdf->isVolume;
	pathInfo->lastGlossiness = glossiness;
	// Inlined BSDF_IsShadowCatcherOnlyInfiniteLights() (bsdf_funcs.cl is
	// concatenated AFTER this file, so the helper is not declared yet)
	pathInfo->lastOnlyInfiniteLights =
			(bsdf->materialIndex != NULL_INDEX) &&
			mats[bsdf->materialIndex].isShadowCatcher &&
			mats[bsdf->materialIndex].isShadowCatcherOnlyInfiniteLights;
	
	pathInfo->isTransmittedPath = pathInfo->isTransmittedPath && (event & TRANSMIT) && (event & (SPECULAR | GLOSSY));
}

OPENCL_FORCE_INLINE bool EyePathInfo_IsCausticPath(__global EyePathInfo *pathInfo) {
	return pathInfo->isNearlyCaustic && (pathInfo->depth.depth > 1);
}

OPENCL_FORCE_INLINE bool EyePathInfo_IsCausticPathWithEvent(__global EyePathInfo *pathInfo,
		const BSDFEvent event, const float glossiness, const float glossinessThreshold) {
	// Note: the +1 is there for the event passed as method arguments
	return pathInfo->isNearlyCaustic && (pathInfo->depth.depth + 1 > 1) &&
			EyePathInfo_IsNearlySpecular(pathInfo, event, glossiness, glossinessThreshold);
}

//------------------------------------------------------------------------------
// Adaptive caustic partition
//
// The fixed glossinessThreshold only sees material constants: a glossy
// vertex just above the threshold can still be unreachable for the eye
// path when the light covers a negligible fraction of its BSDF lobe (a
// firefly source). The adaptive test keeps the S*D path shape but
// widens the chain to any non-diffuse vertex and adds a terminal
// difficulty gate: the light-adjacent vertex is "hard" when it is delta
// or when the light's solid angle covers a negligible fraction of its
// lobe (approximated as omegaLobe = PI * g^2, g == glossiness ==
// roughness). Both directions evaluate the same pure function of
// (vertex, light) so the eye/light partition stays disjoint and
// unbiased.
//------------------------------------------------------------------------------

// Eye-side connection difficulty of the light-adjacent vertex: the eye
// path completes the chain by BSDF-sampling this vertex's lobe onto the
// light (direct light sampling does not fire on delta terminals and is
// the covered technique otherwise).
OPENCL_FORCE_INLINE bool CausticPath_IsTerminalHard(
		const float terminalGlossiness, const float connectProb,
		const int vertexDelta, const float vertexGloss,
		const float lightSolidAngle) {
	if (vertexDelta)
		return true;
	if (vertexGloss > terminalGlossiness)
		return false;
	// A point-like light (omegaL == 0) is covered by direct light
	// sampling: it does not make the connection hard for the eye path
	return (lightSolidAngle > 0.f) &&
			(lightSolidAngle < connectProb * (M_PI_F * vertexGloss * vertexGloss));
}

// Adaptive counterpart of EyePathInfo_IsCausticPathWithEvent(): the
// path has the widened S*D shape (isAdaptiveCaustic) and the pending
// connection's terminal vertex (the one being evaluated) is hard for
// the eye path.
OPENCL_FORCE_INLINE bool EyePathInfo_IsAdaptiveCausticPath(__global EyePathInfo *pathInfo,
		const BSDFEvent event, const float glossiness,
		const float terminalGlossiness, const float connectProb,
		const float lightSolidAngle) {
	// Note: the +1 is there for the event passed as method arguments
	return pathInfo->isAdaptiveCaustic && (pathInfo->depth.depth + 1 > 1) &&
			((event & (SPECULAR | GLOSSY)) != 0) &&
			CausticPath_IsTerminalHard(terminalGlossiness, connectProb,
					(event & SPECULAR) != 0, glossiness, lightSolidAngle);
}

// Adaptive counterpart of EyePathInfo_IsCausticPath() for a direct
// emitter hit: the terminal vertex is the last added one (it scattered
// the path into the light).
OPENCL_FORCE_INLINE bool EyePathInfo_IsAdaptiveCausticHitPath(__global EyePathInfo *pathInfo,
		const float terminalGlossiness, const float connectProb,
		const float lightSolidAngle) {
	return pathInfo->isAdaptiveCaustic && (pathInfo->depth.depth > 1) &&
			((pathInfo->lastBSDFEvent & (SPECULAR | GLOSSY)) != 0) &&
			CausticPath_IsTerminalHard(terminalGlossiness, connectProb,
					(pathInfo->lastBSDFEvent & SPECULAR) != 0,
					pathInfo->lastGlossiness, lightSolidAngle);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

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

#ifndef _SLG_PATHINFO_H
#define	_SLG_PATHINFO_H

#include <ostream>

#include "slg/slg.h"
#include "slg/utils/pathdepthinfo.h"
#include "slg/utils/pathvolumeinfo.h"

namespace slg {

// OpenCL data types
namespace ocl {
#include "slg/utils/pathinfo_types.cl"
}

//------------------------------------------------------------------------------
// PathInfo
//------------------------------------------------------------------------------

class PathInfo {
public:
	PathInfo();
	~PathInfo() { }

	bool IsSpecularPath() const { return isNearlyS; }
	bool IsSpecularPath(const BSDFEvent event, const float glossiness, const float glossinessThreshold) const {
		return isNearlyS && IsNearlySpecular(event, glossiness, glossinessThreshold);
	}

	bool IsSDPath() const { return isNearlySD; }
	bool IsSDSPath() const { return isNearlySDS; }

	bool UseRR(const u_int rrDepth) const;

	PathDepthInfo depth;
	PathVolumeInfo volume;

	// Last path vertex information
	BSDFEvent lastBSDFEvent;

	static bool IsNearlySpecular(const BSDFEvent event, const float glossiness, const float glossinessThreshold);
	static bool CanBeNearlySpecular(const BSDF &bsdf, const float glossinessThreshold);

	// Adaptive caustic partition (see pathinfo_funcs.cl for the GPU
	// mirror): the light-adjacent vertex is "hard" for the eye path when
	// it is delta or when the light's solid angle covers a negligible
	// fraction of its lobe (omegaLobe = PI * g^2, g == glossiness).
	static bool IsAdaptiveTerminalHard(const float terminalGlossiness,
			const float connectProb, const bool vertexDelta,
			const float vertexGloss, const float lightSolidAngle) {
		if (vertexDelta)
			return true;
		if (vertexGloss > terminalGlossiness)
			return false;
		// A point-like light (omegaL == 0) is covered by direct light
		// sampling: it does not make the connection eye-hard
		return (lightSolidAngle > 0.f) &&
				(lightSolidAngle < connectProb * (M_PI * vertexGloss * vertexGloss));
	}

protected:
	// Specular, Specular+ Diffuse and Specular+ Diffuse Specular+ paths
	bool isNearlyS, isNearlySD, isNearlySDS;
};

//------------------------------------------------------------------------------
// EyePathInfo
//------------------------------------------------------------------------------

class EyePathInfo : public PathInfo {
public:
	EyePathInfo();
	~EyePathInfo() { }

	void AddVertex(const BSDF &bsdf, const BSDFEvent event, const float pdfW,
			const float glossinessThreshold);
	
	bool IsCausticPath() const { return isNearlyCaustic && (depth.depth > 1); }
	bool IsCausticPath(const BSDFEvent event, const float glossiness, const float glossinessThreshold) const;

	// Adaptive counterpart of IsCausticPath(event, ...): widened S*D
	// chain (isAdaptiveCaustic) + hard light-adjacent terminal. The
	// pending event/glossiness are the ones of the vertex being
	// evaluated; lightSolidAngle is Light_ConnectionSolidAngle().
	bool IsAdaptiveCausticPath(const BSDFEvent event, const float glossiness,
			const float terminalGlossiness, const float connectProb,
			const float lightSolidAngle) const {
		return isAdaptiveCaustic && (depth.depth + 1 > 1) &&
				((event & (SPECULAR | GLOSSY)) != 0) &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						(event & SPECULAR) != 0, glossiness, lightSolidAngle);
	}
	// Direct emitter hit variant: the terminal is the last added vertex
	bool IsAdaptiveCausticHitPath(const float terminalGlossiness,
			const float connectProb, const float lightSolidAngle) const {
		return isAdaptiveCaustic && (depth.depth > 1) &&
				((lastBSDFEvent & (SPECULAR | GLOSSY)) != 0) &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						(lastBSDFEvent & SPECULAR) != 0, lastGlossiness,
						lightSolidAngle);
	}

	bool isPassThroughPath;

	// Last path vertex information
	float lastBSDFPdfW;
	float lastGlossiness;
	luxrays::Normal lastShadeN;
	bool lastFromVolume, isTransmittedPath;
	// The last vertex restricts direct-light sampling to infinite lights
	// (shadow catcher): its NEE proposal was the infinite distribution, so
	// DirectHit MIS must measure the hit against that same distribution
	bool lastOnlyInfiniteLights;
	// Light linking: the last vertex's link accept mask (~0 before any
	// surface vertex / after a volume vertex = accepts every group)
	u_longlong linkAcceptMask;

	// Adaptive caustic partition (see isAdaptiveCaustic in
	// pathinfo_types.cl)
	bool isAdaptiveCaustic;

private:
	bool isNearlyCaustic;
};

inline std::ostream &operator<<(std::ostream &os, const EyePathInfo &epi) {
	os << "EyePathInfo[" <<
			epi.depth << ", " <<
			epi.volume << ", " <<
			epi.isPassThroughPath << ", " <<
			epi.lastBSDFEvent << ", " <<
			epi.lastBSDFPdfW << ", " <<
			epi.isPassThroughPath << ", " <<
			epi.lastGlossiness << ", " <<
			epi.lastShadeN << ", " <<
			epi.isTransmittedPath << ", " <<
			epi.lastFromVolume << ", " <<
			epi.IsCausticPath() << ", " <<
			epi.IsSpecularPath() << ", " <<
			epi.IsSDPath() << ", " <<
			epi.IsSDSPath() <<
			"]";

	return os;
}

//------------------------------------------------------------------------------
// LightPathInfo
//------------------------------------------------------------------------------

class LightPathInfo : public PathInfo {
public:
	LightPathInfo();
	~LightPathInfo() { }

	void AddVertex(const BSDF &bsdf, const BSDFEvent event, const float glossinessThreshold);

	bool IsCausticPath(const BSDFEvent event, const float glossiness, const float glossinessThreshold) const;

	// Adaptive counterpart: all-non-diffuse chain (isAdaptiveS),
	// non-delta receiver, hard light-adjacent terminal (v1).
	bool IsAdaptiveCausticPath(const BSDFEvent event,
			const float terminalGlossiness, const float connectProb,
			const float lightSolidAngle) const {
		return isAdaptiveS && (depth.depth + 1 > 1) &&
				!(event & SPECULAR) &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						firstVertexDelta, firstVertexGlossiness, lightSolidAngle);
	}

	luxrays::Point lensPoint;

	// Adaptive caustic partition: mirrors the GPU LightPathInfo fields.
	// firstVertex* describe the light-adjacent vertex (v1), the terminal
	// of the eye-side connection-difficulty test.
	bool isAdaptiveS;
	luxrays::Point firstVertexP;
	float firstVertexGlossiness;
	bool firstVertexDelta;
};

inline std::ostream &operator<<(std::ostream &os, const LightPathInfo &lpi) {
	os << "LightPathInfo[" <<
			lpi.depth << ", " <<
			lpi.volume << ", " <<
			lpi.lensPoint << ", " <<
			lpi.lastBSDFEvent << ", " <<
			lpi.IsSpecularPath() << ", " <<
			lpi.IsSDPath() << ", " <<
			lpi.IsSDSPath() <<
			"]";

	return os;
}

}

#endif	/* _SLG_PATHINFO_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

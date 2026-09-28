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
#include "slg/utils/lpe.h"
#include "slg/utils/pathdepthinfo.h"
#include "slg/utils/pathvolumeinfo.h"

namespace slg {

// OpenCL data types
namespace ocl {
using luxrays::ocl::Normal;
#include "slg/utils/pathinfo_types.cl"
}

//------------------------------------------------------------------------------
// PathInfo
//------------------------------------------------------------------------------

class BSDF;

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

	// LPE: seeds the per-expression NFA state sets with the automata's
	// camera-stepped start sets. lpeAutomata is borrowed (film-owned).
	void InitLPE(const LPEAutomaton *automata, const u_int count);
	// Terminal evaluation: bitmask of the expressions accepting on sym
	u_int LPEAcceptMask(const u_int sym) const {
		u_int accept = 0;
		for (u_int i = 0; i < lpeCount; ++i)
			accept |= LPEAccept(lpeAutomata[i], lpeStates[i], sym) << i;
		return accept;
	}
	// Next-event terminal: a light connection at the current vertex
	// carries the path C v1..vN L - vN's own event must be stepped
	// before the terminal (AddVertex runs after NEE in the loop)
	u_int LPEAcceptMask(const u_int vSym, const u_int termSym) const {
		u_int accept = 0;
		for (u_int i = 0; i < lpeCount; ++i)
			accept |= LPEAccept(lpeAutomata[i],
					LPEStep(lpeAutomata[i], lpeStates[i], vSym), termSym) << i;
		return accept;
	}

	// Caustic-class predicates share the media-transparent chain rule
	// (doc/features/caustics-sota.md Stage A): a medium scattering
	// vertex neither extends nor breaks the specular chain — the
	// classification is a pure function of the non-medium events on
	// both sides, keeping the eye/light partition disjoint.
	bool IsCausticPath() const { return isNearlyCaustic && (depth.depth > 1) &&
			(!lastFromVolume || causticHasSurface); }
	bool IsCausticPath(const BSDFEvent event, const float glossiness,
			const float glossinessThreshold, const bool terminalIsVolume = false) const;

	// Adaptive counterpart of IsCausticPath(event, ...): widened S*D
	// chain (isAdaptiveCaustic) + hard light-adjacent terminal. The
	// pending event/glossiness are the ones of the vertex being
	// evaluated; lightSolidAngle is Light_ConnectionSolidAngle().
	// A medium terminal counts as hard once the chain saw a surface:
	// its phase lobe can never aim at a small light.
	bool IsAdaptiveCausticPath(const BSDFEvent event, const float glossiness,
			const float terminalGlossiness, const float connectProb,
			const float lightSolidAngle, const bool terminalIsVolume = false) const {
		return isAdaptiveCaustic && (depth.depth + 1 > 1) &&
				(((event & (SPECULAR | GLOSSY)) != 0) ||
						(terminalIsVolume && causticHasSurface)) &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						((event & SPECULAR) != 0) || terminalIsVolume,
						glossiness, lightSolidAngle);
	}
	// Direct emitter hit variant: the terminal is the last added vertex
	bool IsAdaptiveCausticHitPath(const float terminalGlossiness,
			const float connectProb, const float lightSolidAngle) const {
		return isAdaptiveCaustic && (depth.depth > 1) &&
				(((lastBSDFEvent & (SPECULAR | GLOSSY)) != 0) ||
						(lastFromVolume && causticHasSurface)) &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						((lastBSDFEvent & SPECULAR) != 0) || lastFromVolume,
						lastGlossiness, lightSolidAngle);
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

	// Media-transparent chains: any non-medium vertex after the depth-1
	// receiver. Distinguishes "focused through a surface" from pure
	// ambient medium paths for the caustic-class predicates above.
	bool causticHasSurface;

	// LPE: live NFA state set per expression (u32 bitmask each), stepped
	// once per vertex event in AddVertex; see lpe_funcs.cl for the twin
	const LPEAutomaton *lpeAutomata;
	u_int lpeCount;
	u_int lpeStates[SLG_LPE_MAX_EXPRESSIONS];

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

	// Media-transparent chains: the receiver (connection event) must
	// stay non-nearly-specular; a medium vertex satisfies that. The
	// firstVertexSeen guard keeps pure-medium prefixes eye-owned.
	bool IsCausticPath(const BSDFEvent event, const float glossiness, const float glossinessThreshold) const;

	// Adaptive counterpart: all-non-diffuse chain (isAdaptiveS),
	// non-delta receiver, hard light-adjacent terminal (v1).
	bool IsAdaptiveCausticPath(const BSDFEvent event,
			const float terminalGlossiness, const float connectProb,
			const float lightSolidAngle) const {
		return isAdaptiveS && (depth.depth + 1 > 1) &&
				!(event & SPECULAR) && firstVertexSeen &&
				IsAdaptiveTerminalHard(terminalGlossiness, connectProb,
						firstVertexDelta, firstVertexGlossiness, lightSolidAngle);
	}

	luxrays::Point lensPoint;

	// Adaptive caustic partition: mirrors the GPU LightPathInfo fields.
	// firstVertex* describe the first NON-MEDIUM vertex — the real
	// light-adjacent terminal of the eye-side difficulty test.
	bool isAdaptiveS;
	bool firstVertexSeen;
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

//------------------------------------------------------------------------------
// SspTail: eye-side specular tail recorder
//
// Records the LEADING run of delta specular vertices of an eye path
// (camera -> s1 -> s2 -> ... -> sm -> terminator) plus the first
// non-eligible surface vertex that closes the run. The record is a pure
// geometry/material topology hint: a light-side camera connect blocked by
// a delta occluder can skip the LMNEE discovery walk and rebuild the chain
// directly from these anchors (reproject -> MneeChainVertexInit), then let
// the Newton solver verify the physical constraint. A stale or wrong
// record only wastes iterations - every solved chain is re-validated, so
// the estimator stays unbiased.
//
// GPU mirror lives in pathinfo_types.cl once the GPU recorder lands.
//------------------------------------------------------------------------------

#define SSP_TAIL_MAX_VERTICES 8

typedef struct SspTailVertex {
	luxrays::Point p;			// reproject anchor
	luxrays::Normal gn;			// reproject ray direction (-gn)
	u_int objectID;				// scene object index: blocker/stale match gate
	u_int pad;
} SspTailVertex;

typedef struct SspTail {
	u_int specN;				// recorded specular vertices (eye order: vtx[0] nearest camera)
	u_int flags;				// bit0 = run still open, bit1 = overflow (run longer than capacity)
	// Terminator: first non-eligible surface vertex that closed the run.
	// Phase-1 consumers only use the specular run; the terminator is kept
	// for receiver-endpoint solves (Model B).
	u_int termObjectID;
	luxrays::Point termP;
	luxrays::Normal termGn;
	SspTailVertex vtx[SSP_TAIL_MAX_VERTICES];

	void Reset() {
		specN = 0;
		flags = 1;				// open
		termObjectID = 0xffffffffu;
	}
	bool IsOpen() const { return (flags & 1u) != 0; }
	bool HasOverflow() const { return (flags & 2u) != 0; }
} SspTail;

}

#endif	/* _SLG_PATHINFO_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

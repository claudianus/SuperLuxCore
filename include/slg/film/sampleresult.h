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

#include "luxrays/core/color/spectrumgroup.h"

#include "slg/bsdf/bsdfevents.h"
#include "slg/film/film.h"

#ifndef _SLG_SAMPLERESULT_H
#define	_SLG_SAMPLERESULT_H

namespace slg {

// OpenCL data types
namespace ocl {
using luxrays::ocl::Point;
using luxrays::ocl::Normal;
using luxrays::ocl::UV;
using luxrays::ocl::Spectrum;
#include "slg/film/sampleresult_types.cl"
}

//------------------------------------------------------------------------------
// SampleResult
//------------------------------------------------------------------------------

class SampleResult {
public:
	SampleResult() : useFilmSplat(true), channels(nullptr), lpeRadiance(nullptr) { }
	SampleResult(const Film::FilmChannels *channels, const u_int radianceGroupCount) :
			lpeRadiance(nullptr) {
		Init(channels, radianceGroupCount);
	}
	// 3-arg form used at LPE-aware init sites - lpeRadiance nullptr is
	// preserved through Init.
	SampleResult(const Film::FilmChannels *channels, const u_int radianceGroupCount,
			const u_int lpeCount) : lpeRadiance(nullptr) {
		Init(channels, radianceGroupCount, lpeCount);
	}
	// Deep copy - lpeRadiance owns its Spectrum slots; a bit-copy would
	// double-free on destruction. Every other field is scalar/vector-free.
	SampleResult(const SampleResult &o) { CopyFrom(o); }
	SampleResult &operator=(const SampleResult &o) {
		if (this != &o) CopyFrom(o);
		return *this;
	}
	// Move: steal lpeRadiance, leave source with nullptr so a bit-move
	// can't double-free on destruction.
	SampleResult(SampleResult &&o) noexcept { MoveFrom(std::move(o)); }
	SampleResult &operator=(SampleResult &&o) noexcept {
		if (this != &o) MoveFrom(std::move(o));
		return *this;
	}
	// Copy helper: scalar fields + deep-copy the owned lpeRadiance buffer.
	void CopyFrom(const SampleResult &o);
	// Move helper: scalar fields + steal lpeRadiance.
	void MoveFrom(SampleResult &&o);

	void Init(const Film::FilmChannels *channels, const u_int radianceGroupCount,
			const u_int lpeCount = 0);
	// Re-run Init with the stored channel set - the idempotent reset used
	// between samples so every splat field returns to its default.
	void Reset() { Init(channels, radiance.Size(), lpeSlotsUsed); }

	// O(1) channel test via a mask computed in Init - the old
	// unordered_set::count walked a bucket chain per query (called ~20
	// times per splat inside AtomicAddSampleResultColor).
	bool HasChannel(const Film::FilmChannelType type) const { return (channelsMask >> (u_int)type) & 1ull; }

	luxrays::Spectrum GetSpectrum(const std::vector<RadianceChannelScale> &radianceChannelScales) const;
	float GetY(const std::vector<RadianceChannelScale> &radianceChannelScales) const;

	void AddEmission(const u_int lightID, const luxrays::Spectrum &pathThroughput,
		const luxrays::Spectrum &incomingRadiance);
	void AddDirectLight(const u_int lightID, const BSDFEvent bsdfEvent,
		const luxrays::Spectrum &pathThroughput, const luxrays::Spectrum &incomingRadiance,
		const float lightScale);

	void ClampRadiance(const u_int index, const float minRadiance, const float maxRadiance) {
		radiance[index] = radiance[index].ScaledClamp(minRadiance, maxRadiance);
	}

	void ClampRadiance(const float minRadiance, const float maxRadiance) {
		for (u_int i = 0; i < radiance.Size(); ++i)
			ClampRadiance(i, minRadiance, maxRadiance);
	}

	bool IsValid() const;

	static bool IsAllValid(const std::vector<SampleResult> &sampleResults);
	
	//--------------------------------------------------------------------------

	// pixelX and pixelY have to be initialized only if !useFilmSplat
	u_int pixelX, pixelY;
	float filmX, filmY;
	luxrays::SpectrumGroup radiance;

	float alpha, depth;
	luxrays::Point position;
	luxrays::Normal geometryNormal, shadingNormal;
	// Note: MATERIAL_ID_MASK is calculated starting from materialID field
	u_int materialID;
	// Note: OBJECT_ID_MASK is calculated starting from objectID field
	u_int objectID;
	// Cryptomatte float ids of the first camera-visible surface
	// (0.f = no contribution: miss, holdout or light-path sample)
	float cryptoObjectID, cryptoMaterialID;
	luxrays::Spectrum directDiffuseReflect, directDiffuseTransmit;
	luxrays::Spectrum directGlossyReflect, directGlossyTransmit;
	luxrays::Spectrum emission;
	luxrays::Spectrum indirectDiffuseReflect, indirectDiffuseTransmit;
	luxrays::Spectrum indirectGlossyReflect, indirectGlossyTransmit;
	luxrays::Spectrum indirectSpecularReflect, indirectSpecularTransmit;
	float directShadowMask, indirectShadowMask;
	luxrays::UV uv;
	float rayCount;
	luxrays::Spectrum irradiance;
	// Irradiance requires to store some additional information to be computed
	luxrays::Spectrum irradiancePathThroughput;
	luxrays::Spectrum albedo;
	// LPE: owned array, allocated in Init only when lpeCount>0 (nullptr
	// otherwise) - cuts 96B of dead weight off SampleResult.
	luxrays::Spectrum *lpeRadiance;

	// MOTION_VECTOR channel payload: screen-space velocity of the first
	// camera-visible surface point in pixels per scene time unit
	// (forward in time), plus flags: {vx, vy, valid, objectMotion}.
	float motionVector[4];

	BSDFEvent firstPathVertexEvent;
	bool isHoldout;
	// isCaustic is used only for RADIANCE_PER_SCREEN_NORMALIZED samples
	bool isCaustic;

	// Used to keep some state of the current sample
	bool firstPathVertex, lastPathVertex;

	// Cycles sample clamp (path.clamping.cycles.*): limit on the sum of
	// the RGB channels of each emission / direct-light contribution at
	// the current path vertex (0 = off). The path tracer sets them per
	// vertex: Cycles counts an emitter hit one bounce lower than the
	// vertex the hitting ray left, so both stay "direct" one vertex
	// longer for emission than for next-event estimation.
	float clampEmission, clampDirect;

	bool useFilmSplat;

private:
	const Film::FilmChannels *channels;
	u_longlong channelsMask = 0;
	// LPE slots in lpeRadiance that were live at Init - Init only zeroes
	// that prefix so LPE-off scenes pay nothing for the fixed array.
	u_int lpeSlotsUsed = 0;
};

}

#endif	/* _SLG_SAMPLERESULT_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

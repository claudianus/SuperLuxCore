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

#include "slg/film/film.h"
#include "slg/film/sampleresult.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// SampleResult
//------------------------------------------------------------------------------

void SampleResult::Init(const Film::FilmChannels *chnls, const u_int radianceGroupCount, const u_int lpeCount) {
	channels = chnls;
	channelsMask = 0;
	for (auto const c : *chnls)
		channelsMask |= (1ull << (u_int)c);

	if (HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED) && HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED))
		throw runtime_error("RADIANCE_PER_PIXEL_NORMALIZED and RADIANCE_PER_SCREEN_NORMALIZED, both used in SampleResult");
	else if (HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED) || HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED))
		radiance.Resize(radianceGroupCount);
	else
		radiance.Resize(0);

	// Every field the film splat reads must be deterministic - a reused
	// SampleResult (AddLightSampleResult resizes sampleResults) otherwise
	// carries stale pixels/normals/alpha into the frame buffer and the
	// denoiser.
	pixelX = pixelY = 0;
	filmX = filmY = 0.f;
	for (u_int i = 0; i < radiance.Size(); ++i)
		radiance[i] = Spectrum();

	alpha = 0.f;
	depth = numeric_limits<float>::infinity();
	position = Point(numeric_limits<float>::infinity(),
			numeric_limits<float>::infinity(),
			numeric_limits<float>::infinity());
	geometryNormal = Normal();
	shadingNormal = Normal();
	materialID = 0;
	objectID = 0;
	cryptoObjectID = 0.f;
	cryptoMaterialID = 0.f;
	// Field clears gated on channel presence - an unwritten field is
	// dead memory anyway; only zero the slots the splat actually reads.
	if (HasChannel(Film::DIRECT_DIFFUSE) || HasChannel(Film::DIRECT_DIFFUSE_REFLECT))
		directDiffuseReflect = Spectrum();
	if (HasChannel(Film::DIRECT_DIFFUSE) || HasChannel(Film::DIRECT_DIFFUSE_TRANSMIT))
		directDiffuseTransmit = Spectrum();
	if (HasChannel(Film::DIRECT_GLOSSY) || HasChannel(Film::DIRECT_GLOSSY_REFLECT))
		directGlossyReflect = Spectrum();
	if (HasChannel(Film::DIRECT_GLOSSY) || HasChannel(Film::DIRECT_GLOSSY_TRANSMIT))
		directGlossyTransmit = Spectrum();
	if (HasChannel(Film::EMISSION))
		emission = Spectrum();
	if (HasChannel(Film::INDIRECT_DIFFUSE) || HasChannel(Film::INDIRECT_DIFFUSE_REFLECT))
		indirectDiffuseReflect = Spectrum();
	if (HasChannel(Film::INDIRECT_DIFFUSE) || HasChannel(Film::INDIRECT_DIFFUSE_TRANSMIT))
		indirectDiffuseTransmit = Spectrum();
	if (HasChannel(Film::INDIRECT_GLOSSY) || HasChannel(Film::INDIRECT_GLOSSY_REFLECT))
		indirectGlossyReflect = Spectrum();
	if (HasChannel(Film::INDIRECT_GLOSSY) || HasChannel(Film::INDIRECT_GLOSSY_TRANSMIT))
		indirectGlossyTransmit = Spectrum();
	if (HasChannel(Film::INDIRECT_SPECULAR) || HasChannel(Film::INDIRECT_SPECULAR_REFLECT))
		indirectSpecularReflect = Spectrum();
	if (HasChannel(Film::INDIRECT_SPECULAR) || HasChannel(Film::INDIRECT_SPECULAR_TRANSMIT))
		indirectSpecularTransmit = Spectrum();
	if (HasChannel(Film::DIRECT_SHADOW_MASK))
		directShadowMask = 1.f;
	if (HasChannel(Film::INDIRECT_SHADOW_MASK))
		indirectShadowMask = 1.f;
	if (HasChannel(Film::UV))
		uv = UV(numeric_limits<float>::infinity(),
				numeric_limits<float>::infinity());
	if (HasChannel(Film::RAYCOUNT))
		rayCount = 0.f;
	if (HasChannel(Film::IRRADIANCE))
		irradiance = Spectrum();
	// irradiancePathThroughput is written unconditionally by the eye path
	// (it feeds the irradiance AOV even when the channel is not in the
	// output set) - keep the clear unconditional.
	irradiancePathThroughput = Spectrum();
	if (HasChannel(Film::ALBEDO))
		albedo = Spectrum();
	if (HasChannel(Film::MOTION_VECTOR)) {
		motionVector[0] = 0.f;
		motionVector[1] = 0.f;
		motionVector[2] = 0.f;
		motionVector[3] = 0.f;
	}

	lpeSlotsUsed = Min(lpeCount, (u_int)SLG_LPE_MAX_EXPRESSIONS);
	for (u_int i = 0; i < lpeSlotsUsed; ++i)
		lpeRadiance[i] = Spectrum();

	firstPathVertexEvent = NONE;
	firstPathVertex = true;
	// lastPathVertex can not be really initialized here without knowing
	// the max. path depth.
	lastPathVertex = false;

	isHoldout = false;
	isCaustic = false;
}

Spectrum SampleResult::GetSpectrum(const vector<RadianceChannelScale> &radianceChannelScales) const {
	Spectrum s = 0.f;
	for (u_int i = 0; i < radiance.Size(); ++i)
		s += radianceChannelScales[i].Scale(radiance[i]);
	
	return s;
}

float SampleResult::GetY(const vector<RadianceChannelScale> &radianceChannelScales) const {
	return GetSpectrum(radianceChannelScales).Y();
}

void SampleResult::AddEmission(const u_int lightID, const Spectrum &pathThroughput,
		const Spectrum &incomingRadiance) {
	const Spectrum r = pathThroughput * incomingRadiance;
	radiance[lightID] += r;

	if (firstPathVertex)
		emission += r;
	else {
		indirectShadowMask = 0.f;

		if ((firstPathVertexEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT))
			indirectDiffuseReflect += r;
		else if ((firstPathVertexEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT))
			indirectDiffuseTransmit += r;
		else if ((firstPathVertexEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT))
			indirectGlossyReflect += r;
		else if ((firstPathVertexEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT))
			indirectGlossyTransmit += r;
		else if ((firstPathVertexEvent & (SPECULAR | REFLECT)) == (SPECULAR | REFLECT))
			indirectSpecularReflect += r;
		else if ((firstPathVertexEvent & (SPECULAR | TRANSMIT)) == (SPECULAR | TRANSMIT))
			indirectSpecularTransmit += r;
	}
}

void SampleResult::AddDirectLight(const u_int lightID, const BSDFEvent bsdfEvent,
		const Spectrum &pathThroughput, const Spectrum &incomingRadiance, const float lightScale) {
	const Spectrum r = pathThroughput * incomingRadiance;
	radiance[lightID] += r;

	if (firstPathVertex) {
		// directShadowMask is supposed to be initialized to 1.0
		directShadowMask = Max(0.f, directShadowMask - lightScale);

		if ((bsdfEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT))
			directDiffuseReflect += r;
		else if ((bsdfEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT))
			directDiffuseTransmit += r;
		else if ((bsdfEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT))
			directGlossyReflect += r;
		else if ((bsdfEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT))
			directGlossyTransmit += r;
	} else {
		// indirectShadowMask is supposed to be initialized to 1.0
		indirectShadowMask = Max(0.f, indirectShadowMask - lightScale);

		if ((firstPathVertexEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT))
			indirectDiffuseReflect += r;
		else if ((firstPathVertexEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT))
			indirectDiffuseTransmit += r;
		else if ((firstPathVertexEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT))
			indirectGlossyReflect += r;
		else if ((firstPathVertexEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT))
			indirectGlossyTransmit += r;
		else if ((firstPathVertexEvent & (SPECULAR | REFLECT)) == (SPECULAR | REFLECT))
			indirectSpecularReflect += r;
		else if ((firstPathVertexEvent & (SPECULAR | TRANSMIT)) == (SPECULAR | TRANSMIT))
			indirectSpecularTransmit += r;

		irradiance += irradiancePathThroughput * incomingRadiance;
	}
}

bool SampleResult::IsValid() const {
	for (u_int i = 0; i < radiance.Size(); ++i)
		if (radiance[i].IsNaN() || radiance[i].IsInf() || radiance[i].IsNeg())
			return false;

	return true;
}

bool SampleResult::IsAllValid(const vector<SampleResult> &sampleResults) {
	for (u_int i = 0; i < sampleResults.size(); ++i)
		if (!sampleResults[i].IsValid())
			return false;
	
	return true;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

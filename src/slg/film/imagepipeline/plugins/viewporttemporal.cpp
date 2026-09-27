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

#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>

#include "slg/film/film.h"
#include "slg/film/imagepipeline/plugins/viewporttemporal.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::ViewportTemporalPlugin)

namespace {

// Row-major 4x4 stored as 16 floats (matches Matrix4x4::m layout)
inline void MulMatPoint(const float *m, const float x, const float y, const float z,
		float *ox, float *oy, float *oz, float *ow) {
	*ox = m[0] * x + m[1] * y + m[2]  * z + m[3];
	*oy = m[4] * x + m[5] * y + m[6]  * z + m[7];
	*oz = m[8] * x + m[9] * y + m[10] * z + m[11];
	*ow = m[12] * x + m[13] * y + m[14] * z + m[15];
}

bool ParseMat(const string *s, float *m) {
	if (!s)
		return false;
	std::istringstream iss(*s);
	for (u_int i = 0; i < 16; ++i)
		if (!(iss >> m[i]))
			return false;
	return true;
}

} // anonymous namespace

ViewportTemporalPlugin::ViewportTemporalPlugin() : histW(0), histH(0), hasHistory(false) {
}

ViewportTemporalPlugin::~ViewportTemporalPlugin() {
}

ImagePipelinePlugin *ViewportTemporalPlugin::Copy() const {
	// History is per-session state, not part of the pipeline definition
	return new ViewportTemporalPlugin();
}

void ViewportTemporalPlugin::Apply(Film &film, const u_int index) {
	GenericFrameBuffer<3, 0, float> *img = film.channel_IMAGEPIPELINEs[index].get();
	const u_int width = img->GetWidth();
	const u_int height = img->GetHeight();
	const u_int pixelCount = width * height;
	float *pixels = img->GetPixels();

	// Camera reprojection data published by RenderSession::EndSceneEdit()
	float curCtoW[16], curWtoR[16], curWtoC[16];
	const bool camData =
			ParseMat(film.GetMetadata("viewport.cam.ctow"), curCtoW) &&
			ParseMat(film.GetMetadata("viewport.cam.wtor"), curWtoR) &&
			ParseMat(film.GetMetadata("viewport.cam.wtoc"), curWtoC);
	const string *camOnlyStr = film.GetMetadata("viewport.edit.cameraonly");
	const bool cameraOnly = camOnlyStr && (*camOnlyStr == "1");
	const string *hitherStr = film.GetMetadata("viewport.cam.hither");
	const float hither = hitherStr ? (float)atof(hitherStr->c_str()) : 1e-3f;

	//------------------------------------------------------------------
	// Coverage: real eye-path weight or an LT splat (same rule as
	// VIEWPORT_INFILL); warped history only fills uncovered pixels

	vector<float> coverage(pixelCount, 0.f);
	const u_int groupCount = film.GetRadianceGroupCount();
	if (film.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED))
		for (u_int g = 0; g < groupCount; ++g) {
			const float *src = film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[g]->GetPixels();
			#pragma omp parallel for
			for (int j = 0; j < (int)pixelCount; ++j)
				coverage[j] += src[j * 4 + 3];
		}
	if (film.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED))
		for (u_int g = 0; g < groupCount; ++g) {
			const float *src = film.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[g]->GetPixels();
			#pragma omp parallel for
			for (int j = 0; j < (int)pixelCount; ++j) {
				const float *p = &src[j * 3];
				if (coverage[j] <= 0.f && (p[0] != 0.f || p[1] != 0.f || p[2] != 0.f))
					coverage[j] = 1.f;
			}
		}

	//------------------------------------------------------------------
	// Forward-warp the history frame into the current camera. Each
	// history pixel carries its world-space POSITION, which is
	// camera-independent, so it always reprojects exactly like
	// Camera::ProjectPointToFilm: world -> camera space -> raster.
	// The warp runs on every Apply (not just right after an edit):
	// until real samples land, uncovered pixels keep showing their
	// reprojected history instead of black. Non-camera edits keep
	// history from being trusted at all (stale material colors).

	const bool historyUsable = hasHistory && camData && cameraOnly &&
			(histW == width) && (histH == height);

	if (historyUsable) {
		// Pass 1: splat each history pixel into the new frame; the
		// nearest camera-space depth wins the slot (CAS min)
		auto warpZ = std::make_unique<std::atomic<float>[]>(pixelCount);
		for (u_int i = 0; i < pixelCount; ++i)
			warpZ[i].store(numeric_limits<float>::infinity());

		vector<int> warpSrc(pixelCount, -1); // hist pixel index winning each slot
		#pragma omp parallel for
		for (int i = 0; i < (int)pixelCount; ++i) {
			const float wx = histPos[i * 3], wy = histPos[i * 3 + 1],
					wz = histPos[i * 3 + 2];
			if (!isfinite(wx) || !isfinite(wy) || !isfinite(wz))
				continue;

			// Behind the near plane of the new camera? (same check as
			// Camera::ProjectPointToFilm for perspective cameras)
			float cx, cy, cz, cw;
			MulMatPoint(curWtoC, wx, wy, wz, &cx, &cy, &cz, &cw);
			if (cw != 0.f && cw != 1.f) {
				cx /= cw; cy /= cw; cz /= cw;
			}
			if (cz <= hither)
				continue;

			float rx, ry, rz, rw;
			MulMatPoint(curWtoR, wx, wy, wz, &rx, &ry, &rz, &rw);
			if (rw == 0.f)
				continue;
			const int nx = (int)floorf(rx / rw);
			const int ny = (int)floorf(height - 1.f - ry / rw);
			if ((nx < 0) || (nx >= (int)width) || (ny < 0) || (ny >= (int)height))
				continue;

			const u_int j = ny * width + nx;
			float old = warpZ[j].load();
			bool won = false;
			while (cz < old) {
				if (warpZ[j].compare_exchange_weak(old, cz)) {
					won = true;
					break;
				}
			}
			if (won)
				warpSrc[j] = i; // winning depth wrote us (races are benign)
		}

		// Pass 2: fill uncovered display pixels from the winning history
		// pixel; disoccluded holes (no splat) stay for the infill pass
		#pragma omp parallel for
		for (int j = 0; j < (int)pixelCount; ++j) {
			if (coverage[j] > 0.f)
				continue;
			const int s = warpSrc[j];
			if (s < 0)
				continue;
			for (u_int c = 0; c < 3; ++c)
				pixels[j * 3 + c] = histRGB[s * 3 + c];
		}
	}

	//------------------------------------------------------------------
	// Update history for next frame's reuse. Covered pixels take the
	// fresh display value + world position; uncovered pixels keep their
	// previous entry, so the buffer stays dense through film resets and
	// sparse early passes.

	if (film.HasChannel(Film::POSITION) && camData) {
		const float *posSrc = film.channel_POSITION->GetPixels();
		if ((histRGB.size() != pixelCount * 3) || (histW != width) || (histH != height)) {
			histRGB.assign(pixels, pixels + pixelCount * 3);
			histPos.assign(posSrc, posSrc + pixelCount * 3);
		} else {
			#pragma omp parallel for
			for (int i = 0; i < (int)pixelCount; ++i) {
				// LT splats count as coverage but carry no POSITION;
				// storing rgb without a matching pos would reproject
				// the color to a stale location, so such pixels keep
				// their previous consistent history entry
				if (coverage[i] <= 0.f || !isfinite(posSrc[i * 3]))
					continue;
				for (u_int c = 0; c < 3; ++c) {
					histRGB[i * 3 + c] = pixels[i * 3 + c];
					histPos[i * 3 + c] = posSrc[i * 3 + c];
				}
			}
		}
			histW = width;
		histH = height;
		hasHistory = true;
	} else
		hasHistory = false;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

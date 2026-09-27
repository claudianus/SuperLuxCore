/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include "slg/film/film.h"
#include "slg/film/imagepipeline/plugins/viewportinfill.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Viewport infill plugin
//
// Pull-push hole filling: build a coverage mask from the film's radiance
// weights, collapse it into a coarse pyramid averaging covered neighbours,
// then push fill colours back down so every hole inherits the colour of
// its nearest covered ancestor. Holes dissolve in gradually (fine details
// arrive last), which is exactly the perceptual "denoised preview" look
// wanted for interactive viewport rendering.
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::ViewportInfillPlugin)

ViewportInfillPlugin::ViewportInfillPlugin(const float ltb) : ltBlend(ltb) {
}

ViewportInfillPlugin::~ViewportInfillPlugin() {
}

ImagePipelinePlugin *ViewportInfillPlugin::Copy() const {
	return new ViewportInfillPlugin(ltBlend);
}

void ViewportInfillPlugin::Apply(Film &film, const u_int index) {
	GenericFrameBuffer<3, 0, float> *img = film.channel_IMAGEPIPELINEs[index].get();
	const u_int width = img->GetWidth();
	const u_int height = img->GetHeight();
	const u_int pixelCount = width * height;
	float *pixels = img->GetPixels();

	// Coverage = accumulated sample weight (eye paths) or a non-zero
	// screen-normalized splat (light paths / photonGI)
	const u_int groupCount = film.GetRadianceGroupCount();
	vector<float> coverage(pixelCount, 0.f);
	if (film.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
		for (u_int g = 0; g < groupCount; ++g) {
			const float *src = film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[g]->GetPixels();
			#pragma omp parallel for
			for (int j = 0; j < (int)pixelCount; ++j)
				coverage[j] += src[j * 4 + 3];
		}
	}
	// Light-tracing-only pixels: a splat landed but no eye path has
	// confirmed the pixel yet. They count as coverage (they carry real
	// light) but are also the isolated "speckle" sources a viewport user
	// sees, so they get blended toward the neighbourhood below.
	vector<char> ltOnly;
	u_int ltOnlyCount = 0;
	if (film.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED)) {
		ltOnly.assign(pixelCount, 0);
		for (u_int g = 0; g < groupCount; ++g) {
			const float *src = film.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[g]->GetPixels();
			#pragma omp parallel for reduction(+ : ltOnlyCount)
			for (int j = 0; j < (int)pixelCount; ++j) {
				const float *p = &src[j * 3];
				if (p[0] != 0.f || p[1] != 0.f || p[2] != 0.f) {
					if (coverage[j] <= 0.f) {
						coverage[j] = 1.f;
						ltOnly[j] = 1;
						++ltOnlyCount;
					}
				}
			}
		}
	}
	u_int holeCount = 0;
	#pragma omp parallel for reduction(+ : holeCount)
	for (int j = 0; j < (int)pixelCount; ++j)
		holeCount += (coverage[j] <= 0.f) ? 1u : 0u;

	// Converged frame: nothing to fill and no LT splats to soften. On a
	// 4K viewport the pyramid build below costs hundreds of ms per
	// pipeline run, so this early-out is significant once converged
	if ((holeCount == 0) && (ltOnlyCount == 0))
		return;

	// Pull: premultiplied pyramid levels {r, g, b, w}
	struct Level {
		u_int w, h;
		vector<float> v; // 4 floats per texel
	};
	vector<Level> pyr;
	pyr.push_back({ width, height, vector<float>(pixelCount * 4) });
	{
		float *lv = pyr[0].v.data();
		#pragma omp parallel for
		for (int j = 0; j < (int)pixelCount; ++j) {
			const float c = (coverage[j] > 0.f) ? 1.f : 0.f;
			lv[j * 4]     = pixels[j * 3] * c;
			lv[j * 4 + 1] = pixels[j * 3 + 1] * c;
			lv[j * 4 + 2] = pixels[j * 3 + 2] * c;
			lv[j * 4 + 3] = c;
		}
	}
	while (pyr.back().w > 4 || pyr.back().h > 4) {
		const Level &src = pyr.back();
		// Ceiling halving keeps the odd tail row/column reachable:
		// floor halving would orphan them from the pyramid forever
		const u_int dw = Max(1u, (src.w + 1) / 2), dh = Max(1u, (src.h + 1) / 2);
		Level dst{ dw, dh, vector<float>((size_t)dw * dh * 4) };
		#pragma omp parallel for
		for (int y = 0; y < (int)dh; ++y) {
			for (u_int x = 0; x < dw; ++x) {
				float *d = &dst.v[(y * dw + x) * 4];
				for (u_int dy = 0; dy < 2; ++dy) {
					const u_int sy = Min(y * 2 + dy, src.h - 1);
					for (u_int dx = 0; dx < 2; ++dx) {
						const u_int sx = Min(x * 2 + dx, src.w - 1);
						const float *s = &src.v[(sy * src.w + sx) * 4];
						d[0] += s[0]; d[1] += s[1]; d[2] += s[2]; d[3] += s[3];
					}
				}
			}
		}
		pyr.push_back(std::move(dst));
		if (pyr.size() > 16)
			break;
	}

	// Push: fill holes top-down. Ancestor lookup = nearest covered texel at
	// the coarsest level where coverage exists along this pixel's column.
	const u_int levelCount = (u_int)pyr.size();
	#pragma omp parallel for
	for (int j = 0; j < (int)pixelCount; ++j) {
		if (coverage[j] > 0.f)
			continue;
		u_int x = j % width, y = j / width;
		for (u_int l = 1; l < levelCount; ++l) {
			x = Min(x / 2, pyr[l].w - 1);
			y = Min(y / 2, pyr[l].h - 1);
			const float *s = &pyr[l].v[(y * pyr[l].w + x) * 4];
			if (s[3] > 0.f) {
				pixels[j * 3]     = s[0] / s[3];
				pixels[j * 3 + 1] = s[1] / s[3];
				pixels[j * 3 + 2] = s[2] / s[3];
				break;
			}
		}
	}

	// Speckle softening for light-tracing-only pixels: blend the splat
	// toward the surrounding fill colour so a lone splat reads as a
	// soft contribution instead of a 1 px high-energy dot. The pixel's
	// own contribution is subtracted from every pyramid texel - an
	// isolated speckle keeps walking levels until a real neighbourhood
	// exists, so it always gets attenuated.
	if ((ltBlend > 0.f) && !ltOnly.empty()) {
		const float own = 1.f - ltBlend;
		#pragma omp parallel for
		for (int j = 0; j < (int)pixelCount; ++j) {
			if (!ltOnly[j])
				continue;
			const float ownR = pixels[j * 3], ownG = pixels[j * 3 + 1], ownB = pixels[j * 3 + 2];
			u_int x = j % width, y = j / width;
			for (u_int l = 1; l < levelCount; ++l) {
				x = Min(x / 2, pyr[l].w - 1);
				y = Min(y / 2, pyr[l].h - 1);
				const float *s = &pyr[l].v[(y * pyr[l].w + x) * 4];
				const float nbhdW = s[3] - 1.f; // exclude self
				if (nbhdW > .5f) {
					const float inv = ltBlend / nbhdW;
					pixels[j * 3]     = own * ownR + (s[0] - ownR) * inv;
					pixels[j * 3 + 1] = own * ownG + (s[1] - ownG) * inv;
					pixels[j * 3 + 2] = own * ownB + (s[2] - ownB) * inv;
					break;
				}
			}
		}
	}
}

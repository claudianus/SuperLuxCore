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

#include <cmath>
#include <vector>

#include "slg/film/film.h"
#include "slg/film/imagepipeline/plugins/viewportsmooth.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// À-trous edge-aware filter (SVGF-style, Dammertz et al. 2010 /
// Schied et al. 2017 "SVGF"): wavelet iterations at step 1,2,4 with
// weights driven by color distance, depth discontinuity and normal
// deviation. Only applied to pixels below minSamps accumulated weight -
// converged pixels stay raw, preserving unbiased quality where samples
// have landed.
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::ViewportSmoothPlugin)

ViewportSmoothPlugin::ViewportSmoothPlugin(const float ms) : minSamps(ms) {
}

ViewportSmoothPlugin::~ViewportSmoothPlugin() {
}

ImagePipelinePlugin *ViewportSmoothPlugin::Copy() const {
	return new ViewportSmoothPlugin(minSamps);
}

void ViewportSmoothPlugin::Apply(Film &film, const u_int index) {
	if (!film.HasChannel(Film::DEPTH) || !film.HasChannel(Film::AVG_SHADING_NORMAL))
		return;

	GenericFrameBuffer<3, 0, float> *img = film.channel_IMAGEPIPELINEs[index].get();
	const u_int width = img->GetWidth();
	const u_int height = img->GetHeight();
	const u_int pixelCount = width * height;

	// Per-pixel accumulated weight (eye paths)
	vector<float> weight(pixelCount, 0.f);
	if (film.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
		const u_int groupCount = film.GetRadianceGroupCount();
		for (u_int g = 0; g < groupCount; ++g) {
			const float *src = film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[g]->GetPixels();
			#pragma omp parallel for
			for (int j = 0; j < (int)pixelCount; ++j)
				weight[j] += src[j * 4 + 3];
		}
	}

	const float *depth = film.channel_DEPTH->GetPixels();
	vector<float> normals(pixelCount * 3, 0.f);
	#pragma omp parallel for
	for (int i = 0; i < (int)pixelCount; ++i)
		film.channel_AVG_SHADING_NORMAL->GetWeightedPixel(i, &normals[i * 3]);

	// Which pixels are noisy enough to filter (real but low samples)?
	vector<char> noisy(pixelCount, 0);
	#pragma omp parallel for
	for (int j = 0; j < (int)pixelCount; ++j)
		noisy[j] = (weight[j] > 0.f) && (weight[j] < minSamps);

	vector<float> cur(img->GetPixels(), img->GetPixels() + pixelCount * 3);
	vector<float> nxt(pixelCount * 3, 0.f);

	// à-trous kernel offsets (3x3 binomial) at dilated steps 1, 2, 4
	static const float kW[3][3] = {
		{ 1.f / 16, 2.f / 16, 1.f / 16 },
		{ 2.f / 16, 4.f / 16, 2.f / 16 },
		{ 1.f / 16, 2.f / 16, 1.f / 16 }
	};
	const float sigmaColor = 1.25f, sigmaNormal = 128.f, sigmaDepth = .05f;

	for (u_int stepIdx = 0; stepIdx < 3; ++stepIdx) {
		const int step = 1 << stepIdx;
		#pragma omp parallel for
		for (int i = 0; i < (int)pixelCount; ++i) {
			float *dst = &nxt[i * 3];
			if (!noisy[i]) {
				for (u_int c = 0; c < 3; ++c)
					dst[c] = cur[i * 3 + c];
				continue;
			}

			const int px = i % width, py = i / width;
			const float zC = depth[i];
			const float *nC = &normals[i * 3];
			const float *cC = &cur[i * 3];

			float acc[3] = { 0.f, 0.f, 0.f }, wSum = 0.f;
			for (int dy = -1; dy <= 1; ++dy) {
				const int sy = py + dy * step;
				if ((sy < 0) || (sy >= (int)height))
					continue;
				for (int dx = -1; dx <= 1; ++dx) {
					const int sx = px + dx * step;
					if ((sx < 0) || (sx >= (int)width))
						continue;
					const int s = sy * width + sx;
					if (!noisy[s])
						continue; // keep converged pixels out of the filter

					// Depth edge stop (relative)
					const float dz = fabsf(depth[s] - zC);
					const float wZ = expf(-dz / Max(sigmaDepth * Max(zC, 1.f), 1e-6f));
					// Normal edge stop
					const float *nS = &normals[s * 3];
					const float nDot = Max(0.f,
							nS[0] * nC[0] + nS[1] * nC[1] + nS[2] * nC[2]);
					const float wN = powf(nDot, sigmaNormal);
					// Color (luminance) edge stop
					const float *cS = &cur[s * 3];
					const float lC = cC[0] + cC[1] + cC[2];
					const float lS = cS[0] + cS[1] + cS[2];
					const float wC = expf(-fabsf(lS - lC) / (sigmaColor * (lC + .1f)));

					const float w = kW[dy + 1][dx + 1] * wZ * wN * wC;
					for (u_int c = 0; c < 3; ++c)
						acc[c] += w * cS[c];
					wSum += w;
				}
			}
			if (wSum > 0.f)
				for (u_int c = 0; c < 3; ++c)
					dst[c] = acc[c] / wSum;
			else
				for (u_int c = 0; c < 3; ++c)
					dst[c] = cC[c];
		}
		cur.swap(nxt);
	}

	#pragma omp parallel for
	for (int i = 0; i < (int)pixelCount; ++i) {
		if (!noisy[i])
			continue;
		float *p = &img->GetPixels()[i * 3];
		for (u_int c = 0; c < 3; ++c)
			p[c] = cur[i * 3 + c];
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

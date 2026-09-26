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

#include "slg/imagemap/resizepolicies/resizepolicies.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// ImageMapResizeFixedPolicy::ApplyResizePolicy()
//------------------------------------------------------------------------------

ImageMapUPtr ImageMapResizeFixedPolicy::ApplyResizePolicy(const std::string &fileName,
		const ImageMapConfig &imgCfg, bool &toApply) const {
	if ((scale < 1.f)) {
		// Header-only probe: load the image already at the target size.
		// The ctor picks the smallest mip level covering the hint (tx
		// files) or streams decode+downscale through a tile-cached
		// ImageBuf (plain files) — the full-resolution pixels never
		// materialize in heap.
		const pair<u_int, u_int> size = ImageMap::GetSize(fileName);
		const u_int width = size.first;
		const u_int height = size.second;

		if (Max(width, height) > minSize) {
			u_int newWidth = Max<u_int>(width * scale, minSize);
			u_int newHeight = Max<u_int>(height * scale, minSize);

			if (newWidth >= newHeight)
				newHeight = Max<u_int>(newWidth * (width / (float)height), 1u);
			else
				newWidth = Max<u_int>(newHeight * (height / (float)width), 1u);

			SDL_LOG("Scaling ImageMap: " << fileName << " [from " << width << "x" << height <<
					" to " << newWidth << "x" << newHeight <<"]");

			auto im = std::make_unique<ImageMap>(fileName, imgCfg, newWidth, newHeight);

			toApply = false;
			return im;
		}
	}

	ImageMapUPtr im = std::make_unique<ImageMap>(fileName, imgCfg);

	const u_int width = im->GetWidth();
	const u_int height = im->GetHeight();

	if (scale > 1.f) {
		// Enlarge all images (may be for testing, not very useful otherwise)
		const u_int newWidth = width * scale;
		const u_int newHeight = height * scale;
		im->Resize(newWidth, newHeight);
		im->Preprocess();
	}
	// Nothing to do for a scale of 1.0 or for images already <= minSize

	toApply = false;

	return im;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

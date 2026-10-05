#line 2 "pathdepthinfo_types.cl"

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
	unsigned int depth, diffuseDepth, glossyDepth, specularDepth;
	// Number of transmission (TRANSMIT) events along the path and number
	// of transparent surfaces crossed so far (updated by Scene_Intersect()
	// while stepping through pass-through materials). Exposed to shading
	// through the "rayinfo" texture.
	unsigned int transmitDepth, transparentDepth;
	// Volume scattering vertices (counted apart from diffuseDepth). As a
	// maximum: 0 = legacy, volume scatters count against diffuseDepth;
	// > 0 = their own limit (Cycles "Volume" bounces).
	unsigned int volumeDepth;

	// Path-space regularization (PSR, path.regularization.*): the
	// engine seeds sigma/minDepth at path init; HitPoint_SetRayContext
	// gates them into HitPoint.regularization per vertex. 0 = off.
	float regularization;
	unsigned int regularizationMinDepth;

	// Cycles Filter Glossy (CPU PathDepthInfo twin): 1 / blur_glossy
	// (0 = off, eye paths only) and the smallest non-specular BSDF pdf
	float filterGlossy, minRayPdf;
} PathDepthInfo;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

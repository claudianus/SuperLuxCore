#line 2 "texture_densitygrid_funcs.cl"

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
// DensityGrid texture
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 DensityGridTexture_D(
		__global const ImageMap *imageMap,
		int x, int y, int z,
		int nx, int ny, int nz
		IMAGEMAPS_PARAM_DECL) {
	__global const void *pixels = ImageMap_GetPixelsAddress(
		imageMapBuff, imageMap->pageIndex, imageMap->pixelsIndex);
	const ImageMapStorageType storageType = imageMap->storageType;
	const uint channelCount = imageMap->channelCount;

	const uint index = ((clamp(z, 0, nz - 1) * ny) + clamp(y, 0, ny - 1)) * nx + clamp(x, 0, nx - 1);
	
	return ImageMap_GetTexel_SpectrumValue(storageType, pixels, channelCount, index);
}

OPENCL_FORCE_INLINE float4 DensityGridTexture_CubicWeights(const float t) {
	// Positive cubic B-spline weights, matching Cycles volume interpolation.
	return MAKE_FLOAT4(
			(((-1.f / 6.f) * t + .5f) * t - .5f) * t + (1.f / 6.f),
			((.5f * t - 1.f) * t) * t + (2.f / 3.f),
			((-.5f * t + .5f) * t + .5f) * t + (1.f / 6.f),
			(1.f / 6.f) * t * t * t);
}

OPENCL_FORCE_INLINE float DensityGridTexture_CubicWeight(const float4 weights, const int index) {
	// CUDA float4 is a struct and has no OpenCL-style subscript operator.
	return index == 0 ? weights.x : index == 1 ? weights.y : index == 2 ? weights.z : weights.w;
}

OPENCL_FORCE_NOT_INLINE float3 DensityGridTexture_ConstEvaluateSpectrum(__global const HitPoint *hitPoint,
		const int nx, const int ny, const int nz,
		const uint imageMapIndex, const uint interpolationType,
		__global const TextureMapping3D *mapping
		TEXTURES_PARAM_DECL) {
	__global const ImageMap *imageMap = &imageMapDescs[imageMapIndex];

	const float3 P = TextureMapping3D_Map(mapping, hitPoint, NULL TEXTURES_PARAM);

	float x, y, z;
	int vx, vy, vz;

	switch (imageMap->wrapType) {
		case WRAP_REPEAT:
			x = P.x * nx;
			vx = Floor2Int(x);
			x -= vx;
			vx = Mod(vx, nx);
			y = P.y * ny;
			vy = Floor2Int(y);
			y -= vy;
			vy = Mod(vy, ny);
			z = P.z * nz;
			vz = Floor2Int(z);
			z -= vz;
			vz = Mod(vz, nz);
			break;
		case WRAP_BLACK:
			if (P.x < 0.f || P.x >= 1.f ||
				P.y < 0.f || P.y >= 1.f ||
				P.z < 0.f || P.z >= 1.f)
				return BLACK;
			x = P.x * nx;
			vx = Floor2Int(x);
			x -= vx;
			y = P.y * ny;
			vy = Floor2Int(y);
			y -= vy;
			z = P.z * nz;
			vz = Floor2Int(z);
			z -= vz;
			break;
		case WRAP_WHITE:
			if (P.x < 0.f || P.x >= 1.f ||
				P.y < 0.f || P.y >= 1.f ||
				P.z < 0.f || P.z >= 1.f)
				return WHITE;
			x = P.x * nx;
			vx = Floor2Int(x);
			x -= vx;
			y = P.y * ny;
			vy = Floor2Int(y);
			y -= vy;
			z = P.z * nz;
			vz = Floor2Int(z);
			z -= vz;
			break;
		case WRAP_CLAMP:
			x = clamp(P.x, 0.f, 1.f) * nx;
			vx = min(Floor2Int(x), nx - 1);
			x -= vx;
			y = clamp(P.y, 0.f, 1.f) * ny;
			vy = min(Floor2Int(interpolationType == 1u ? y : P.y * ny), ny - 1);
			y -= vy;
			z = clamp(P.z, 0.f, 1.f) * nz;
			vz = min(Floor2Int(interpolationType == 1u ? z : P.z * nz), nz - 1);
			z -= vz;
			break;
		default:
			return BLACK;
	}

	if (interpolationType == 1u) {
		if (imageMap->wrapType == WRAP_CLAMP) {
			// Cubic weights require a bounded fraction. The legacy linear
			// y/z path uses the original coordinate for its integer part.
			x = clamp(P.x, 0.f, 1.f) * nx;
			y = clamp(P.y, 0.f, 1.f) * ny;
			z = clamp(P.z, 0.f, 1.f) * nz;
			vx = min(Floor2Int(x), nx - 1);
			vy = min(Floor2Int(y), ny - 1);
			vz = min(Floor2Int(z), nz - 1);
			x -= vx;
			y -= vy;
			z -= vz;
		}
		const float4 wx = DensityGridTexture_CubicWeights(x);
		const float4 wy = DensityGridTexture_CubicWeights(y);
		const float4 wz = DensityGridTexture_CubicWeights(z);
		float3 result = BLACK;
		for (int k = 0; k < 4; ++k) {
			float3 row = BLACK;
			for (int j = 0; j < 4; ++j) {
				float3 column = BLACK;
				for (int i = 0; i < 4; ++i) {
					const int sx = imageMap->wrapType == WRAP_REPEAT ? Mod(vx + i - 1, nx) : vx + i - 1;
					const int sy = imageMap->wrapType == WRAP_REPEAT ? Mod(vy + j - 1, ny) : vy + j - 1;
					const int sz = imageMap->wrapType == WRAP_REPEAT ? Mod(vz + k - 1, nz) : vz + k - 1;
						column += DensityGridTexture_CubicWeight(wx, i) * DensityGridTexture_D(imageMap,
							sx, sy, sz,
							nx, ny, nz IMAGEMAPS_PARAM);
				}
					row += DensityGridTexture_CubicWeight(wy, j) * column;
			}
				result += DensityGridTexture_CubicWeight(wz, k) * row;
		}
		return result;
	}

	// Trilinear interpolation of the grid element
	return
		mix(
			mix(
				mix(
					DensityGridTexture_D(imageMap, vx, vy, vz, nx, ny, nz IMAGEMAPS_PARAM),
					DensityGridTexture_D(imageMap, vx + 1, vy, vz, nx, ny, nz IMAGEMAPS_PARAM),
				x),
				mix(
					DensityGridTexture_D(imageMap, vx, vy + 1, vz, nx, ny, nz IMAGEMAPS_PARAM),
					DensityGridTexture_D(imageMap, vx + 1, vy + 1, vz, nx, ny, nz IMAGEMAPS_PARAM),
				x),
			y),
			mix(
				mix(
					DensityGridTexture_D(imageMap, vx, vy, vz + 1, nx, ny, nz IMAGEMAPS_PARAM),
					DensityGridTexture_D(imageMap, vx + 1, vy, vz + 1, nx, ny, nz IMAGEMAPS_PARAM),
				x),
				mix(
					DensityGridTexture_D(imageMap, vx, vy + 1, vz + 1, nx, ny, nz IMAGEMAPS_PARAM),
					DensityGridTexture_D(imageMap, vx + 1, vy + 1, vz + 1, nx, ny, nz IMAGEMAPS_PARAM),
				x),
			y),
		z);
}

OPENCL_FORCE_INLINE float DensityGridTexture_ConstEvaluateFloat(__global const HitPoint *hitPoint,
		const int nx, const int ny, const int nz,
		const uint imageMapIndex, const uint interpolationType,
		__global const TextureMapping3D *mapping
		TEXTURES_PARAM_DECL) {
	return Spectrum_Y(DensityGridTexture_ConstEvaluateSpectrum(hitPoint,
			nx, ny, nz,
			imageMapIndex, interpolationType, mapping
			TEXTURES_PARAM));
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

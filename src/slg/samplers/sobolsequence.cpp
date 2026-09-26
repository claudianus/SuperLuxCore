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

#include <math.h>

#include <limits>
#include <vector>

#include "slg/samplers/sobolsequence.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// SobolSequence
//------------------------------------------------------------------------------

SobolSequence::SobolSequence() : directions(NULL) {
	rngPass = 0;
	rng0 = 0.f;
	rng1 = 0.f;
	blueNoiseEnable = false;
	owenEnable = false;
	blueNoiseSeed = 0;
	pixelShift = -1.f;
}

SobolSequence::~SobolSequence() {
	delete[] directions;
}

void SobolSequence::RequestSamples(const u_int size) {
	directions = new u_int[size * SOBOL_BITS];
	GenerateDirectionVectors(directions, size);
}

u_int SobolSequence::SobolDimension(const u_int index, const u_int dimension) const {
	const u_int offset = dimension * SOBOL_BITS;
	u_int result = 0;
	u_int i = index;

	for (u_int j = 0; i; i >>= 1, j++) {
		if (i & 1)
			result ^= directions[offset + j];
	}

	return result;
}

u_int SobolSequence::BlueNoiseHash(u_int x) {
	// murmur3 32-bit finalizer (must match the GPU kernel version)
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

u_int SobolSequence::ReverseBits(u_int x) {
	x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
	x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
	x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
	x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
	return (x >> 16) | (x << 16);
}

u_int SobolSequence::ReversedBitOwen(u_int n, const u_int seed) {
	// Burley 2020, "Practical Hash-based Owen Scrambling" (JCGT 9(4)),
	// with Cessen's improved Laine-Karras hash (same construction as
	// Blender Cycles' sobol_burley sampler)
	n ^= n * 0x3d20adeau;
	n += seed;
	n *= (seed >> 16) | 1u;
	n ^= n * 0x05526c56u;
	n ^= n * 0x53a22864u;
	return n;
}

u_int SobolSequence::NestedUniformScramble(const u_int i, const u_int seed) {
	return ReverseBits(ReversedBitOwen(ReverseBits(i), seed));
}

float SobolSequence::GetSample(const u_int pass, const u_int index) {
	u_int iResult;
	float shift;

	if (owenEnable) {
		// Owen-scrambled Sobol: blueNoiseSeed carries the constant
		// per-pixel seed. The sequence index is shuffled by a
		// nested-uniform scramble (decorrelating sample order across
		// pixels), then each dimension is scrambled with a per-pixel,
		// per-dimension seed. No Cranley-Patterson rotation is needed.
		const u_int shuffleSeed = BlueNoiseHash(blueNoiseSeed ^ 0x70efbc49u);
		const u_int dimSeed = BlueNoiseHash(blueNoiseSeed ^ (index * 0x9e3779b9u + 0x85ebca6bu));
		const u_int i = NestedUniformScramble(pass, shuffleSeed);
		iResult = NestedUniformScramble(SobolDimension(i, index), dimSeed);
		// Blue-noise Cranley-Patterson offset: the scalar rank offset is
		// staggered per dimension by an irrational stride so dims stay
		// decorrelated while the spatial ordering is preserved
		shift = (pixelShift >= 0.f) ? pixelShift + index * 0.6180339887f : 0.f;
	} else if (blueNoiseEnable) {
		// Blue-noise dithered sampling (Heitz et al. 2019): per-pixel
		// constant, per-dimension hashed digital shift + Cranley-Patterson
		// offset. The Sobol index is used unscrambled so a pixel walks its
		// own stratified prefix of the sequence across passes while
		// neighboring pixels are decorrelated by the per-pixel seed.
		const u_int dimSeed = BlueNoiseHash(blueNoiseSeed ^ (index * 0x9e3779b9u + 0x85ebca6bu));
		iResult = SobolDimension(pass, index) ^ dimSeed;
		shift = BlueNoiseHash(dimSeed ^ 0xc2b2ae35u) * (1.f / 4294967296.f);
	} else {
		// I scramble pass too in order avoid correlations visible with LIGHTCPU and BIDIRCPU
		iResult = SobolDimension(pass + rngPass, index);

		// Cranley-Patterson rotation to reduce visible regular patterns
		shift = (index & 1) ? rng0 : rng1;
	}

	const float fResult = iResult * (1.f / 0xffffffffu);
	const float val = fResult + shift;

	return val - floorf(val);
}

void SobolSequence::GenerateScrambleTile(u_int *tile, const u_int size) {
	const u_int n = size * size;

	// Progressive farthest-point ordering on the toroidal tile: each step
	// places the next rank on the pixel farthest from all already-ranked
	// pixels. The rank field then has a blue-noise spectrum and every
	// prefix of the ranking covers the tile uniformly. Deterministic
	// (ties broken by lowest index), so host and device builds agree.
	std::vector<float> minDist2(n, std::numeric_limits<float>::max());
	std::vector<u_int> rank(n, 0xFFFFFFFFu);

	u_int cur = (size / 2) * size + size / 2;
	for (u_int r = 0; r < n; r++) {
		rank[cur] = r;

		const u_int cx = cur % size, cy = cur / size;
		for (u_int p = 0; p < n; p++) {
			const u_int px = p % size, py = p / size;
			u_int dx = (cx > px) ? (cx - px) : (px - cx);
			u_int dy = (cy > py) ? (cy - py) : (py - cy);
			if (dx > size - dx) dx = size - dx;
			if (dy > size - dy) dy = size - dy;
			const float d2 = (float)(dx * dx + dy * dy);
			if (d2 < minDist2[p])
				minDist2[p] = d2;
		}

		u_int best = 0;
		float bestD = -1.f;
		for (u_int p = 0; p < n; p++) {
			if (rank[p] != 0xFFFFFFFFu)
				continue;
			if (minDist2[p] > bestD) {
				bestD = minDist2[p];
				best = p;
			}
		}
		cur = best;
	}

	for (u_int p = 0; p < n; p++)
		tile[p] = rank[p];
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

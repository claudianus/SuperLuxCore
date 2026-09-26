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
#include <cstdio>

#include "luxrays/utils/mc.h"
#include "slg/engines/pathguiding.h"

using namespace std;
using namespace luxrays;
using namespace slg;

PathGuidingCache::PathGuidingCache(const Point &min, float size) :
		cubeMin(min), cubeSize(size), invCubeSize(1.f / size),
		cells(GRID_RES * GRID_RES * GRID_RES),
		cellsWrite(GRID_RES * GRID_RES * GRID_RES) {
}

void PathGuidingCache::MaybeSwap() const {
	if (writeRecords.load(std::memory_order_relaxed) <= SWAP_RECORDS)
		return;
	bool expected = false;
	if (!swapFlag.compare_exchange_strong(expected, true,
			std::memory_order_relaxed, std::memory_order_relaxed))
		return;
	// M2c: round length tunable (env LUX_PG_SWAP, default SWAP_RECORDS).
	// Longer rounds = richer but staler read side.
	static const unsigned long long kSwapRecords = []() {
		const char *e = getenv("LUX_PG_SWAP");
		return e ? Max(1000ULL, (unsigned long long)atoll(e)) : SWAP_RECORDS;
	}();
	if (writeRecords.load(std::memory_order_relaxed) > kSwapRecords) {
		cells.swap(cellsWrite);
		for (auto &c : cellsWrite)
			c.Clear();
		writeRecords.store(0uLL, std::memory_order_relaxed);
	}
	swapFlag.store(false, std::memory_order_relaxed);
}

u_int PathGuidingCache::CellIndex(const Point &p) const {
	const float fx = Clamp((p.x - cubeMin.x) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	const float fy = Clamp((p.y - cubeMin.y) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	const float fz = Clamp((p.z - cubeMin.z) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	return Min<u_int>(static_cast<u_int>(fx) +
			static_cast<u_int>(fy) * GRID_RES +
			static_cast<u_int>(fz) * GRID_RES * GRID_RES,
			GRID_RES * GRID_RES * GRID_RES - 1);
}

u_int PathGuidingCache::DirBin(const Vector &dir) {
	// Equal-area: phi uniform, cosTheta uniform in [-1, 1]
	const float phi = atan2f(dir.y, dir.x);  // [-pi, pi]
	const float c = Clamp(dir.z, -1.f, 1.f);
	u_int pi = Min(static_cast<u_int>((phi + M_PI) / (2.f * M_PI) * DIR_PHI), DIR_PHI - 1);
	u_int ti = Min(static_cast<u_int>((c * .5f + .5f) * DIR_THETA), DIR_THETA - 1);
	return ti * DIR_PHI + pi;
}

Vector PathGuidingCache::BinDir(u_int bin, float u0, float u1) {
	const u_int pi = bin % DIR_PHI;
	const u_int ti = bin / DIR_PHI;
	const float phi = ((pi + u0) / DIR_PHI) * 2.f * M_PI - M_PI;
	const float c = ((ti + u1) / DIR_THETA) * 2.f - 1.f;
	const float s = sqrtf(Max(0.f, 1.f - c * c));
	return Vector(s * cosf(phi), s * sinf(phi), c);
}

bool PathGuidingCache::CosineSample(const Vector &n, float u0, float u1,
		Vector *sampledDir, float *pdfW) {
	const float cosTheta = sqrtf(Max(0.f, 1.f - u0));
	if (!(cosTheta > 0.f))
		return false;
	const float sinTheta = sqrtf(Max(0.f, 1.f - cosTheta * cosTheta));
	const float phi = 2.f * M_PI * u1;
	// Orthonormal basis around n
	const Vector helper = (fabsf(n.z) < .999f) ? Vector(0.f, 0.f, 1.f) : Vector(0.f, 1.f, 0.f);
	const Vector u = Normalize(Cross(helper, n));
	const Vector v = Cross(n, u);
	*sampledDir = u * (sinTheta * cosf(phi)) + v * (sinTheta * sinf(phi)) + n * cosTheta;
	*pdfW = cosTheta * INV_PI;
	return true;
}

void PathGuidingCache::SnapshotCell(u_int cell, float *bins, float *total) const {
	const Cell &c = cells[cell];
	for (u_int i = 0; i < DIR_BINS; ++i)
		bins[i] = c.bins[i].load(std::memory_order_relaxed);
	*total = c.total.load(std::memory_order_relaxed);
}

void PathGuidingCache::Record(const Point &p, const Vector &wi, float flux) const {
	// Swap cadence counts record *attempts*, not kept records: in dim
	// scenes most arrivals carry ~0 local value and are dropped below, so
	// counting only keeps would stall rounds forever (read side never
	// warms). Attempts grow steadily regardless of scene brightness.
	writeRecords.fetch_add(1uLL, std::memory_order_relaxed);
	MaybeSwap();
	if (!(flux > 0.f) || !isfinite(flux))
		return;
	flux = Min(flux, RECORD_CLAMP);
	const u_int cell = CellIndex(p);
	const Cell &c = cellsWrite[cell];
	AtomicAdd(c.bins[DirBin(wi)], flux);
	AtomicAdd(c.total, flux);
	// Directional moment for the vMF fit (volume guiding)
	AtomicAdd(c.dirX, wi.x * flux);
	AtomicAdd(c.dirY, wi.y * flux);
	AtomicAdd(c.dirZ, wi.z * flux);
}

bool PathGuidingCache::RecordBin(u_int cell, u_int bin, float flux) const {
	if (!(flux > 0.f) || !isfinite(flux))
		return false;
	if (cell >= GRID_RES * GRID_RES * GRID_RES || bin >= DIR_BINS)
		return false;
	flux = Min(flux, RECORD_CLAMP);
	const Cell &c = cellsWrite[cell];
	AtomicAdd(c.bins[bin], flux);
	AtomicAdd(c.total, flux);
	const Vector d = BinDir(bin, .5f, .5f);
	AtomicAdd(c.dirX, d.x * flux);
	AtomicAdd(c.dirY, d.y * flux);
	AtomicAdd(c.dirZ, d.z * flux);
	return true;
}

void PathGuidingCache::ForceSwap() const {
	cells.swap(cellsWrite);
	for (auto &c : cellsWrite)
		c.Clear();
}

bool PathGuidingCache::CanGuide(const Point &p) const {
	// M2c: warmup threshold tunable (env LUX_PG_WARMUP, default 256).
	// Guiding from thin cells only dilutes against BSDF sampling.
	static const float kWarmup = []() {
		const char *e = getenv("LUX_PG_WARMUP");
		return e ? (float)Max(atof(e), 1.0) : (float)WARMUP_RECORDS;
	}();
	const Cell &c = cells[CellIndex(p)];
	return c.total.load(std::memory_order_relaxed) >= kWarmup;
}

void PathGuidingCache::SnapshotTable(std::vector<float> *out) const {
	out->resize(GRID_RES * GRID_RES * GRID_RES * (DIR_BINS + 1));
	float *dst = &(*out)[0];
	for (u_int cell = 0; cell < GRID_RES * GRID_RES * GRID_RES; ++cell) {
		const Cell &c = cells[cell];
		for (u_int i = 0; i < DIR_BINS; ++i)
			*dst++ = c.bins[i].load(std::memory_order_relaxed);
		*dst++ = c.total.load(std::memory_order_relaxed);
	}
}

void PathGuidingCache::SnapshotCoarseTable(std::vector<float> *out) const {
	out->assign(COARSE_CHUNKS * COARSE_CHUNK_CELLS * (COARSE_BINS + 1), 0.f);
	for (u_int cz = 0u; cz < COARSE_GRID; ++cz) {
		for (u_int cy = 0u; cy < COARSE_GRID; ++cy) {
			for (u_int cx = 0u; cx < COARSE_GRID; ++cx) {
				const u_int cc = cx + cy * COARSE_GRID + cz * COARSE_GRID * COARSE_GRID;
				float *dst = &(*out)[(cc >> 5) * COARSE_CHUNK_CELLS *
						(COARSE_BINS + 1) + (cc & 31u) * (COARSE_BINS + 1)];
				float ctotal = 0.f;
				for (u_int fz = 0u; fz < 2u; ++fz) {
					for (u_int fy = 0u; fy < 2u; ++fy) {
						for (u_int fx = 0u; fx < 2u; ++fx) {
							const u_int fc = (cx * 2u + fx) +
									(cy * 2u + fy) * GRID_RES +
									(cz * 2u + fz) * GRID_RES * GRID_RES;
							const Cell &c = cells[fc];
							for (u_int ti = 0u; ti < DIR_THETA; ++ti) {
								for (u_int pi = 0u; pi < DIR_PHI; ++pi)
									dst[(ti >> 1) * COARSE_PHI + (pi >> 1)] +=
											c.bins[ti * DIR_PHI + pi].load(std::memory_order_relaxed);
							}
							ctotal += c.total.load(std::memory_order_relaxed);
						}
					}
				}
				dst[COARSE_BINS] = ctotal;
			}
		}
	}
}

bool PathGuidingCache::Save(const std::string &path) const {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const u_int magic = 0x47554944u; // GUID
	const u_int version = 1u;
	const u_int grid = GRID_RES, phi = DIR_PHI, theta = DIR_THETA;
	fwrite(&magic, sizeof(magic), 1, f);
	fwrite(&version, sizeof(version), 1, f);
	fwrite(&grid, sizeof(grid), 1, f);
	fwrite(&phi, sizeof(phi), 1, f);
	fwrite(&theta, sizeof(theta), 1, f);
	fwrite(&cubeMin.x, sizeof(float), 1, f);
	fwrite(&cubeMin.y, sizeof(float), 1, f);
	fwrite(&cubeMin.z, sizeof(float), 1, f);
	fwrite(&cubeSize, sizeof(float), 1, f);
	std::vector<float> table;
	SnapshotTable(&table);
	const bool ok = fwrite(&table[0], sizeof(float), table.size(), f) == table.size();
	fclose(f);
	return ok;
}

PathGuidingCache *PathGuidingCache::Load(const std::string &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return nullptr;
	u_int magic, version, grid, phi, theta;
	float minx, miny, minz, size;
	bool ok = fread(&magic, sizeof(magic), 1, f) == 1 &&
			fread(&version, sizeof(version), 1, f) == 1 &&
			fread(&grid, sizeof(grid), 1, f) == 1 &&
			fread(&phi, sizeof(phi), 1, f) == 1 &&
			fread(&theta, sizeof(theta), 1, f) == 1 &&
			fread(&minx, sizeof(minx), 1, f) == 1 &&
			fread(&miny, sizeof(miny), 1, f) == 1 &&
			fread(&minz, sizeof(minz), 1, f) == 1 &&
			fread(&size, sizeof(size), 1, f) == 1;
	ok = ok && (magic == 0x47554944u) && (version == 1u) &&
			(grid == GRID_RES) && (phi == DIR_PHI) && (theta == DIR_THETA) &&
			(size > 0.f);
	PathGuidingCache *cache = nullptr;
	if (ok) {
		cache = new PathGuidingCache(Point(minx, miny, minz), size);
		const size_t n = (size_t)GRID_RES * GRID_RES * GRID_RES * (DIR_BINS + 1);
		std::vector<float> table(n);
		ok = fread(&table[0], sizeof(float), n, f) == n;
		if (ok) {
			const float *src = &table[0];
			for (u_int cell = 0; cell < GRID_RES * GRID_RES * GRID_RES; ++cell) {
				Cell &c = cache->cells[cell];
				for (u_int i = 0; i < DIR_BINS; ++i)
					c.bins[i].store(*src++, std::memory_order_relaxed);
				c.total.store(*src++, std::memory_order_relaxed);
			}
		} else {
			delete cache;
			cache = nullptr;
		}
	}
	fclose(f);
	return cache;
}


// M2c: cosine-product weights with additive smoothing. With ~15 records
// per bin the raw weights are noise-dominated and the proposal loses to
// smooth BSDF sampling; blending beta (a fraction of the mean bin mass)
// toward uniform keeps Sample()/Pdf() exact while flattening noise.
// Beta is tunable via LUX_PG_SMOOTH (default 0.1 = 10% uniform admixture).
// M2c-T (env LUX_PG_TRILINEAR=1): trilinear spatial sharing. The 16^3
// grid over-resolves space (~2000 records/cell) while the 128 direction
// bins starve (~15 each). Blending the 2x2x2 neighborhood multiplies the
// effective records per bin ~8x at the cost of spatial blur — the right
// trade for smooth indirect fields, with zero format change. Sample() and
// Pdf() MUST use the same blend (exactness); gating (CanGuide/MixWeight)
// keeps the center cell.
void PathGuidingCache::SnapshotBlend(const Point &p, float *bins, float *total) const {
	const float fx = Clamp((p.x - cubeMin.x) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	const float fy = Clamp((p.y - cubeMin.y) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	const float fz = Clamp((p.z - cubeMin.z) * invCubeSize, 0.f, .99999994f) * GRID_RES;
	const float cx = fx - .5f, cy = fy - .5f, cz = fz - .5f;
	int ix0 = (int)floorf(cx), iy0 = (int)floorf(cy), iz0 = (int)floorf(cz);
	const float tx = cx - ix0, ty = cy - iy0, tz = cz - iz0;
	for (u_int i = 0; i < DIR_BINS; ++i)
		bins[i] = 0.f;
	float t = 0.f;
	for (int dz = 0; dz < 2; ++dz) {
		const u_int iz = (u_int)Max(0, Min((int)GRID_RES - 1, iz0 + dz));
		const float wz = dz ? tz : 1.f - tz;
		for (int dy = 0; dy < 2; ++dy) {
			const u_int iy = (u_int)Max(0, Min((int)GRID_RES - 1, iy0 + dy));
			const float wy = dy ? ty : 1.f - ty;
			for (int dx = 0; dx < 2; ++dx) {
				const u_int ix = (u_int)Max(0, Min((int)GRID_RES - 1, ix0 + dx));
				const float w = (dx ? tx : 1.f - tx) * wy * wz;
				if (!(w > 0.f))
					continue;
				const Cell &c = cells[ix + iy * GRID_RES + iz * GRID_RES * GRID_RES];
				for (u_int i = 0; i < DIR_BINS; ++i)
					bins[i] += w * c.bins[i].load(std::memory_order_relaxed);
				t += w * c.total.load(std::memory_order_relaxed);
			}
		}
	}
	*total = t;
}

static void GuideWeights(const float *bins, float total,
		const luxrays::Vector &nn, float *weights, float *wSum,
		const bool isotropic = false) {
	// Relative beta (fraction of mean bin, constant dilution forever) vs
	// absolute beta (fixed pseudo-count: strong early, vanishes as data
	// grows). LUX_PG_SMOOTHABS > 0 selects absolute (M2c absolute-smoothing
	// experiment: rich cells finally use sharp proposals).
	static const float kBeta = []() {
		const char *e = getenv("LUX_PG_SMOOTH");
		return e ? Max(atof(e), 0.0) : 0.1;
	}();
	static const float kBetaAbs = []() {
		const char *e = getenv("LUX_PG_SMOOTHABS");
		return e ? Max(atof(e), 0.0) : 0.0;
	}();
	const float beta = (kBetaAbs > 0.f) ? (float)kBetaAbs :
		(float)(kBeta * total / PathGuidingCache::DIR_BINS);
	float sum = 0.f;
	for (u_int i = 0; i < PathGuidingCache::DIR_BINS; ++i) {
		const u_int pi = i % PathGuidingCache::DIR_PHI;
		const u_int ti = i / PathGuidingCache::DIR_PHI;
		const float phi = ((pi + .5f) / PathGuidingCache::DIR_PHI) * 2.f * M_PI - M_PI;
		const float c = ((ti + .5f) / PathGuidingCache::DIR_THETA) * 2.f - 1.f;
		const float s = sqrtf(Max(0.f, 1.f - c * c));
		const float cosB = s * cosf(phi) * nn.x + s * sinf(phi) * nn.y + c * nn.z;
		// Smooth only the valid hemisphere: away-facing bins stay at
		// weight 0 so they are never picked (picking them would draw
		// below-surface directions that only kill the path). Volumes
		// scatter into the full sphere: isotropic mode keeps every bin.
		weights[i] = isotropic ? (bins[i] + beta) :
				((cosB > 0.f) ? bins[i] * cosB + beta : 0.f);
		sum += weights[i];
	}
	*wSum = sum;
}

// vMF fit for volume isotropic guiding: r = |S1|/S0 is the resultant
// length (0 = uniform, 1 = delta). Below r_min the lobe is ill-defined
// and the flat bins/uniform fallback is kept. Banerjee's approximation
// maps r -> kappa; kappa is clamped for pdf/sampling stability.
bool PathGuidingCache::CellVmf(u_int cell, Vector &mu, float &kappa) const {
	static const float kWarmup = []() {
		const char *e = getenv("LUX_PG_WARMUP");
		return e ? (float)Max(atof(e), 1.0) : (float)WARMUP_RECORDS;
	}();
	static const bool kNoVmf = (getenv("LUX_PG_NOVMF") != nullptr);
	if (kNoVmf)
		return false;
	const Cell &c = cells[cell];
	const float s0 = c.total.load(std::memory_order_relaxed);
	if (s0 < kWarmup)
		return false;
	const Vector s1(c.dirX.load(std::memory_order_relaxed),
			c.dirY.load(std::memory_order_relaxed),
			c.dirZ.load(std::memory_order_relaxed));
	const float len = s1.Length();
	const float r = len / s0;
	if (!(r > .05f))
		return false;
	mu = s1 * (1.f / len);
	kappa = Min(r * (3.f - r * r) / (1.f - r * r), 64.f);
	return true;
}

float PathGuidingCache::VmfPdf(const float cosMuW, const float kappa) {
	return kappa * expf(kappa * (cosMuW - 1.f)) /
			(2.f * M_PI * (1.f - expf(-2.f * kappa)));
}

Vector PathGuidingCache::VmfSample(const Vector &mu, const float kappa,
		const float u0, const float u1) {
	// cos(theta) ~ k e^{k(z-1)} on [-1,1]: closed-form inversion
	const float z = 1.f + logf(Max(u1 + (1.f - u1) * expf(-2.f * kappa),
			1e-30f)) / kappa;
	const float s = sqrtf(Max(0.f, 1.f - z * z));
	const float phi = 2.f * M_PI * u0;
	const Vector helper = (fabsf(mu.z) < .999f) ? Vector(0.f, 0.f, 1.f) :
			Vector(0.f, 1.f, 0.f);
	const Vector u = Normalize(Cross(helper, mu));
	const Vector v = Cross(mu, u);
	return u * (s * cosf(phi)) + v * (s * sinf(phi)) + mu * z;
}

bool PathGuidingCache::Sample(const Point &p, const Normal &n,
		float uBin, float uDir0, float uDir1,
		Vector *sampledDir, float *pdfW, const bool isotropic) const {
	const u_int cell = CellIndex(p);
	if (isotropic) {
		// vMF lobe + uniform floor: exact sampling/pdf, sharp where the
		// incident field is directional, always covers the sphere.
		Vector mu;
		float kappa;
		if (CellVmf(cell, mu, kappa)) {
			*sampledDir = (uBin < .85f) ? VmfSample(mu, kappa, uDir0, uDir1) :
					UniformSampleSphere(uDir0, uDir1);
			*pdfW = .85f * VmfPdf(Dot(*sampledDir, mu), kappa) +
					.15f * .25f * INV_PI;
			return true;
		}
	}

	float bins[DIR_BINS];
	float total;
	static const bool kTrilinear = (getenv("LUX_PG_TRILINEAR") != nullptr);
	if (kTrilinear)
		SnapshotBlend(p, bins, &total);
	else
		SnapshotCell(cell, bins, &total);
	if (total <= 0.f)
		return false;

	// Cosine-product pick with additive smoothing (see GuideWeights):
	// bins facing away from n keep only the smoothing mass (isotropic
	// mode keeps all bins for full-sphere volume scattering).
	float weights[DIR_BINS];
	float wSum = 0.f;
	const Vector nn(n.x, n.y, n.z);
	GuideWeights(bins, total, nn, weights, &wSum, isotropic);

	float pick = uBin * wSum;
	u_int bin = 0;
	for (; bin < DIR_BINS - 1; ++bin) {
		pick -= weights[bin];
		if (pick <= 0.f)
			break;
	}
	if (!(weights[bin] > 0.f)) {
		// Degenerate pick (e.g. uBin = 0 over leading empty bins):
		// same valid-distribution fallback as above.
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
			*pdfW = .25f * INV_PI;
			return true;
		}
		return CosineSample(nn, uDir0, uDir1, sampledDir, pdfW);
	}

	*sampledDir = BinDir(bin, uDir0, uDir1);
	// pdf = P(pick bin) * uniform-in-bin density, from the same snapshot
	*pdfW = (weights[bin] / wSum) / BinSolidAngle();
	return true;
}

float PathGuidingCache::Pdf(const Point &p, const Normal &n,
		const Vector &dir, const bool isotropic) const {
	const u_int cell = CellIndex(p);
	if (isotropic) {
		Vector mu;
		float kappa;
		if (CellVmf(cell, mu, kappa))
			return .85f * VmfPdf(Dot(dir, mu), kappa) + .15f * .25f * INV_PI;
	}

	float bins[DIR_BINS];
	float total;
	static const bool kTrilinearPdf = (getenv("LUX_PG_TRILINEAR") != nullptr);
	if (kTrilinearPdf)
		SnapshotBlend(p, bins, &total);
	else
		SnapshotCell(cell, bins, &total);
	if (total <= 0.f)
		return 0.f;

	const Vector nn(n.x, n.y, n.z);
	float weights[DIR_BINS];
	float wSum = 0.f;
	GuideWeights(bins, total, nn, weights, &wSum, isotropic);

	// The bin of dir (same mapping as the sampler side)
	const float phi = atan2f(dir.y, dir.x);
	const float c = Clamp(dir.z, -1.f, 1.f);
	const u_int pi = Min(static_cast<u_int>((phi + M_PI) / (2.f * M_PI) * DIR_PHI), DIR_PHI - 1);
	const u_int ti = Min(static_cast<u_int>((c * .5f + .5f) * DIR_THETA), DIR_THETA - 1);
	return (weights[ti * DIR_PHI + pi] / wSum) / BinSolidAngle();
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

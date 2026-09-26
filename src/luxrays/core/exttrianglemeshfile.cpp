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

#include <iostream>
#include <fstream>
#include <cstring>
#include <bit>
#include <numeric>

#include <boost/format.hpp>

#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/utils/memspill.h"
#include "luxrays/utils/ply/rply.h"
#include "luxrays/utils/serializationutils.h"

using namespace std;
using namespace luxrays;

//------------------------------------------------------------------------------
// ExtMesh PLY reader
//------------------------------------------------------------------------------

// rply vertex callback
static int VertexCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	Point *p = *static_cast<Point **> (userData);
	if (!p) return 1;

	long vertIndex;
	ply_get_argument_element(argument, nullptr, &vertIndex);

	if (userIndex == 0)
		p[vertIndex].x =
			static_cast<float>(ply_get_argument_value(argument));
	else if (userIndex == 1)
		p[vertIndex].y =
			static_cast<float>(ply_get_argument_value(argument));
	else if (userIndex == 2)
		p[vertIndex].z =
			static_cast<float>(ply_get_argument_value(argument));

	return 1;
}

// rply normal callback
static int NormalCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;

	ply_get_argument_user_data(argument, &userData, &userIndex);

	Normal *n = *static_cast<Normal **> (userData);
	if (!n) return 1;

	long normIndex;
	ply_get_argument_element(argument, nullptr, &normIndex);

	if (userIndex == 0)
		n[normIndex].x =
			static_cast<float>(ply_get_argument_value(argument));
	else if (userIndex == 1)
		n[normIndex].y =
			static_cast<float>(ply_get_argument_value(argument));
	else if (userIndex == 2)
		n[normIndex].z =
			static_cast<float>(ply_get_argument_value(argument));

	return 1;
}

// rply uv callback
static int UVCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	auto uv = *static_cast<ExtMeshProp<UV>::Layer *> (userData);

	long uvIndex;
	ply_get_argument_element(argument, nullptr, &uvIndex);

	if (userIndex == 0)
		uv[uvIndex].u =
			static_cast<float>(ply_get_argument_value(argument));
	else if (userIndex == 1)
		uv[uvIndex].v =
			static_cast<float>(ply_get_argument_value(argument));

	return 1;
}

// rply color callback
static int ColorCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	auto c = *static_cast<ExtMeshProp<float>::Layer *> (userData);
	//float *c = *static_cast<float **> (userData);

	long colIndex;
	ply_get_argument_element(argument, nullptr, &colIndex);

	// Check the type of value used
	p_ply_property property = nullptr;
	ply_get_argument_property(argument, &property, nullptr, nullptr);
	e_ply_type dataType;
	ply_get_property_info(property, nullptr, &dataType, nullptr, nullptr);
	if (dataType == PLY_UCHAR) {
		if (userIndex == 0)
			c[colIndex * 3] =
				static_cast<float>(ply_get_argument_value(argument) / 255.0);
		else if (userIndex == 1)
			c[colIndex * 3 + 1] =
				static_cast<float>(ply_get_argument_value(argument) / 255.0);
		else if (userIndex == 2)
			c[colIndex * 3 + 2] =
				static_cast<float>(ply_get_argument_value(argument) / 255.0);
	} else {
		if (userIndex == 0)
			c[colIndex * 3] =
				static_cast<float>(ply_get_argument_value(argument));
		else if (userIndex == 1)
			c[colIndex * 3 + 1] =
				static_cast<float>(ply_get_argument_value(argument));
		else if (userIndex == 2)
			c[colIndex * 3 + 2] =
				static_cast<float>(ply_get_argument_value(argument));
	}

	return 1;
}

// rply vertex callback
static int AlphaCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	auto c = *static_cast<ExtMeshProp<float>::Layer *> (userData);

	long alphaIndex;
	ply_get_argument_element(argument, nullptr, &alphaIndex);

	// Check the type of value used
	p_ply_property property = nullptr;
	ply_get_argument_property(argument, &property, nullptr, nullptr);
	e_ply_type dataType;
	ply_get_property_info(property, nullptr, &dataType, nullptr, nullptr);
	if (dataType == PLY_UCHAR) {
		if (userIndex == 0)
			c[alphaIndex] =
				static_cast<float>(ply_get_argument_value(argument) / 255.0);
	} else {
		if (userIndex == 0)
			c[alphaIndex] =
				static_cast<float>(ply_get_argument_value(argument));		
	}

	return 1;
}

// rply vertex callback
static int VertexAOVCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	auto c = *static_cast<ExtMeshProp<float>::Layer *> (userData);

	long alphaIndex;
	ply_get_argument_element(argument, nullptr, &alphaIndex);

	// Check the type of value used
	p_ply_property property = nullptr;
	ply_get_argument_property(argument, &property, nullptr, nullptr);
	e_ply_type dataType;
	ply_get_property_info(property, nullptr, &dataType, nullptr, nullptr);
	if (dataType == PLY_UCHAR) {
		if (userIndex == 0)
			c[alphaIndex] =
				static_cast<float>(ply_get_argument_value(argument) / 255.0);
	} else {
		if (userIndex == 0)
			c[alphaIndex] =
				static_cast<float>(ply_get_argument_value(argument));		
	}

	return 1;
}

// rply face callback
static int FaceCB(p_ply_argument argument) {
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, nullptr);

	vector<Triangle> *tris = static_cast<vector<Triangle> *> (userData);

	long length, valueIndex;
	ply_get_argument_property(argument, nullptr, &length, &valueIndex);

	if (length == 3) {
		if (valueIndex < 0)
			tris->push_back(Triangle());
		else if (valueIndex < 3)
			tris->back().v[valueIndex] =
					static_cast<u_int> (ply_get_argument_value(argument));
	} else if (length == 4) {
		// I have to split the quad in 2x triangles
		if (valueIndex < 0) {
			tris->push_back(Triangle());
		} else if (valueIndex < 3)
			tris->back().v[valueIndex] =
					static_cast<u_int> (ply_get_argument_value(argument));
		else if (valueIndex == 3) {
			const u_int i0 = tris->back().v[0];
			const u_int i1 = tris->back().v[2];
			const u_int i2 = static_cast<u_int> (ply_get_argument_value(argument));

			tris->push_back(Triangle(i0, i1, i2));
		}
	}

	return 1;
}

// rply uv callback
static int TriAOVCB(p_ply_argument argument) {
	long userIndex = 0;
	void *userData = nullptr;
	ply_get_argument_user_data(argument, &userData, &userIndex);

	auto triAOV = *static_cast<ExtMeshProp<float>::Layer *> (userData);

	long triAOVIndex;
	ply_get_argument_element(argument, nullptr, &triAOVIndex);

	if (userIndex == 0)
		triAOV[triAOVIndex] =
			static_cast<float>(ply_get_argument_value(argument));

	return 1;
}

ExtTriangleMeshUPtr ExtTriangleMesh::LoadPly(const string &fileName) {
	p_ply plyfile = ply_open(fileName.c_str(), nullptr);
	if (!plyfile) {
		stringstream ss;
		ss << "Unable to read PLY mesh file '" << fileName << "'";
		throw runtime_error(ss.str());
	}

	if (!ply_read_header(plyfile)) {
		stringstream ss;
		ss << "Unable to read PLY header from '" << fileName << "'";
		throw runtime_error(ss.str());
	}

	VertexBuffer p;
	const long plyNbVerts = ply_set_read_cb(plyfile, "vertex", "x", VertexCB, &p, 0);
	ply_set_read_cb(plyfile, "vertex", "y", VertexCB, &p, 1);
	ply_set_read_cb(plyfile, "vertex", "z", VertexCB, &p, 2);
	if (plyNbVerts <= 0) {
		stringstream ss;
		ss << "No vertices found in '" << fileName << "'";
		throw runtime_error(ss.str());
	}

	std::vector<Triangle> vi;
	const long plyNbFaces = ply_set_read_cb(plyfile, "face", "vertex_indices", FaceCB, &vi, 0);
	if (plyNbFaces <= 0) {
		stringstream ss;
		ss << "No faces found in '" << fileName << "'";
		throw runtime_error(ss.str());
	}

	// Check if the file includes triaov information
	ExtMeshProp<float> TriAOVs;
	array<u_int, EXTMESH_MAX_DATA_COUNT> plyNbTriAOVs;
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		const string suffix = (i == 0) ? "" : ToString(i);

		plyNbTriAOVs[i] = ply_set_read_cb(plyfile, ("faceaov" + suffix).c_str(), "triaov", TriAOVCB, &TriAOVs[i], 0);
		if ((plyNbTriAOVs[i] > 0) && (plyNbTriAOVs[i] != plyNbFaces)) {
			stringstream ss;
			ss << "Wrong count of triangle AOV #" << i << " in '" << fileName << "'";
			throw runtime_error(ss.str());
		}
	}

	// Check if the file includes normal information
	NormalBuffer n;
	const long plyNbNormals = ply_set_read_cb(plyfile, "vertex", "nx", NormalCB, &n, 0);
	ply_set_read_cb(plyfile, "vertex", "ny", NormalCB, &n, 1);
	ply_set_read_cb(plyfile, "vertex", "nz", NormalCB, &n, 2);
	if ((plyNbNormals > 0) && (plyNbNormals != plyNbVerts)) {
		stringstream ss;
		ss << "Wrong count of normals in '" << fileName << "'";
		throw runtime_error(ss.str());
	}

	// This is our own extension to file PLY format in order to support multiple
	// UVs, Colors and Alphas for each vertex

	// Output buffers

	ExtMeshProp<UV> uvs; // Vertex uvs
	ExtMeshProp<Spectrum> cols; // Vertex colors
	ExtMeshProp<float> alphas; // Vertex alphas
	ExtMeshProp<float> vertexAOVs; // Vertex AOV

	array<u_int, EXTMESH_MAX_DATA_COUNT> plyNbUVs;
	array<u_int, EXTMESH_MAX_DATA_COUNT> plyNbColors;
	array<u_int, EXTMESH_MAX_DATA_COUNT> plyNbAlphas;
	array<u_int, EXTMESH_MAX_DATA_COUNT> plyNbVertexAOVs;

	// Get output buffer sizes
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		const string suffix = (i == 0) ? "" : ToString(i);

		// Check if the file includes uv information
		plyNbUVs[i] = ply_set_read_cb(plyfile, "vertex", ("s" + suffix).c_str(), UVCB, &uvs[i], 0);
		ply_set_read_cb(plyfile, "vertex", ("t" + suffix).c_str(), UVCB, &uvs[i], 1);
		if ((plyNbUVs[i] > 0) && (plyNbUVs[i] != plyNbVerts)) {
			stringstream ss;
			ss << "Wrong count of uvs #" << i << " in '" << fileName << "'";
			throw runtime_error(ss.str());
		}

		// Check if the file includes color information
		plyNbColors[i] = ply_set_read_cb(plyfile, "vertex", ("red" + suffix).c_str(), ColorCB, &cols[i], 0);
		ply_set_read_cb(plyfile, "vertex", ("green" + suffix).c_str(), ColorCB, &cols[i], 1);
		ply_set_read_cb(plyfile, "vertex", ("blue" + suffix).c_str(), ColorCB, &cols[i], 2);
		if ((plyNbColors[i] > 0) && (plyNbColors[i] != plyNbVerts)) {
			stringstream ss;
			ss << "Wrong count of colors #" << i << " in '" << fileName << "'";
			throw runtime_error(ss.str());
		}

		// Check if the file includes alpha information
		plyNbAlphas[i] = ply_set_read_cb(plyfile, "vertex", ("alpha" + suffix).c_str(), AlphaCB, &alphas[i], 0);
		if ((plyNbAlphas[i] > 0) && (plyNbAlphas[i] != plyNbVerts)) {
			stringstream ss;
			ss << "Wrong count of alphas #" << i << " in '" << fileName << "'";
			throw runtime_error(ss.str());
		}

		// Check if the file includes vertexAOV information
		plyNbVertexAOVs[i] = ply_set_read_cb(plyfile, "vertex", ("vertaov" + suffix).c_str(), VertexAOVCB, &vertexAOVs[i], 0);
		if ((plyNbVertexAOVs[i] > 0) && (plyNbVertexAOVs[i] != plyNbVerts)) {
			stringstream ss;
			ss << "Wrong count of vertex AOV #" << i << " in '" << fileName << "'";
			throw runtime_error(ss.str());
		}
	}

	// Allocate buffers
	p.Allocate(plyNbVerts);
	if (plyNbNormals == 0)
		n = NormalBuffer();
	else
		n.Allocate(plyNbNormals);

	// Helper
	auto allocProp = [&]<typename V, typename N>(V& values, const N& numbers, u_int i) {
		if (numbers[i] == 0) {
			values[i] = nullptr;
		} else {
			values.AllocateLayer(i, numbers[i]);
		}
	};
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		allocProp(uvs, plyNbUVs, i);
		allocProp(cols, plyNbColors, i);
		allocProp(alphas, plyNbAlphas, i);
		allocProp(vertexAOVs, plyNbVertexAOVs, i);
		allocProp(TriAOVs, plyNbTriAOVs, i);
	}

	// Read data
	if (!ply_read(plyfile)) {
		stringstream ss;
		ss << "Unable to parse PLY file '" << fileName << "'";

		throw runtime_error(ss.str());
	}

	ply_close(plyfile);

	// Copy triangle indices vector
	auto tris = TriangleBuffer(vi);
	//copy(vi.begin(), vi.end(), tris);

	auto mesh = std::make_unique<ExtTriangleMesh>(std::move(p), std::move(tris), std::move(n), uvs, cols, alphas);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (vertexAOVs[i])
			mesh->SetVertexAOV(i, vertexAOVs[i], plyNbVerts);
		mesh->SetTriAOV(i, TriAOVs[i], vi.size());
	}
	
	return mesh;
}

//------------------------------------------------------------------------------
// ExtTriangleMesh .lxm proxy (raw section dump, mmap-loadable)
//
// Layout: fixed 128-byte header, then 64-byte-aligned raw sections in a
// fixed order: vertices, triangles, normals?, then the present layers of
// uvs, cols, alphas, vertAOV, triAOV (each indexed by its mask bit).
// Loading maps the file copy-on-write and adopts each section in place —
// no parse, no heap copy, and clean pages stay evictable (out-of-core).
//------------------------------------------------------------------------------

namespace {

struct LxmHeader {
	char magic[4];       // "LXM1"
	u_int version;       // 1
	u_int flags;         // bit0: per-vertex normals present
	u_longlong vertCount;
	u_longlong triCount;
	u_int uvsMask, colsMask, alphasMask, vertAovMask, triAovMask;
	u_int reserved[19];
};
static_assert(sizeof(LxmHeader) == 128);
static_assert(std::is_trivially_copyable_v<LxmHeader>);

constexpr size_t LxmAlign = 64;

inline size_t LxmAlignUp(const size_t v) {
	return (v + LxmAlign - 1) & ~(LxmAlign - 1);
}

// Expected element sizes — the format is a raw dump of the in-memory
// layout, so it is only portable across builds with identical POD
// layouts (same compiler/architecture family). Guarded at load time.
inline bool LxmCheckSizes() {
	return sizeof(Point) == 12 && sizeof(Triangle) == 12 &&
			sizeof(Normal) == 12 && sizeof(UV) == 8 &&
			sizeof(Spectrum) == 12 && sizeof(float) == 4;
}

}

void ExtTriangleMesh::SaveProxy(const string &fileName) const {
	if (!LxmCheckSizes())
		throw runtime_error(".lxm proxy: unexpected POD sizes, cannot write");

	const u_int srcVertCount = GetTotalVertexCount();
	const u_int srcTriCount = GetTotalTriangleCount();
	const Point *srcVerts = static_cast<const Point *>(vertices.Data());
	const Triangle *srcTris = static_cast<const Triangle *>(tris.Data());

	// --- Spatial reorder for out-of-core page locality -----------------
	// Triangles are stored sorted by the Morton code of their centroid
	// and vertices renumbered by first use: rays then touch spatially
	// coherent, page-local runs of the mapped sections, so under memory
	// pressure the kernel can evict the untouched regions (virtualized
	// geometry at OS page granularity). Unreferenced vertices are
	// dropped, shrinking the file. Transparent to the reader: every
	// index layer is permuted consistently.
	std::vector<u_int> triPerm(srcTriCount), vertPerm;
	vertPerm.reserve(srcVertCount);
	std::vector<Triangle> newTris(srcTriCount);
	{
		BBox bb;
		for (u_int i = 0; i < srcVertCount; ++i)
			bb = Union(bb, srcVerts[i]);
		const Vector ext = bb.pMax - bb.pMin;
		const float ex = ext.x != 0.f ? ext.x : 1.f;
		const float ey = ext.y != 0.f ? ext.y : 1.f;
		const float ez = ext.z != 0.f ? ext.z : 1.f;

		// Split the bits of a 10-bit integer with 0s
		const auto split3 = [](u_int v) {
			v &= 0x3ffu;
			v = (v | (v << 16)) & 0x030000FFu;
			v = (v | (v << 8)) & 0x0300F00Fu;
			v = (v | (v << 4)) & 0x030C30C3u;
			v = (v | (v << 2)) & 0x09249249u;
			return v;
		};

		std::vector<u_int> keys(srcTriCount);
		for (u_int i = 0; i < srcTriCount; ++i) {
			const Triangle &t = srcTris[i];
			const Point c = (srcVerts[t.v[0]] + srcVerts[t.v[1]] +
					srcVerts[t.v[2]]) * (1.f / 3.f);
			const u_int kx = (u_int)(Clamp((c.x - bb.pMin.x) / ex, 0.f, 1.f) * 1023.f + .5f);
			const u_int ky = (u_int)(Clamp((c.y - bb.pMin.y) / ey, 0.f, 1.f) * 1023.f + .5f);
			const u_int kz = (u_int)(Clamp((c.z - bb.pMin.z) / ez, 0.f, 1.f) * 1023.f + .5f);
			keys[i] = split3(kx) | (split3(ky) << 1) | (split3(kz) << 2);
		}
		std::iota(triPerm.begin(), triPerm.end(), 0u);
		std::stable_sort(triPerm.begin(), triPerm.end(),
				[&](const u_int a, const u_int b) { return keys[a] < keys[b]; });

		// Vertex renumbering by first use in the sorted triangle order
		std::vector<u_int> remap(srcVertCount, ~0u);
		for (u_int i = 0; i < srcTriCount; ++i) {
			const Triangle &t = srcTris[triPerm[i]];
			for (u_int k = 0; k < 3; ++k) {
				if (remap[t.v[k]] == ~0u) {
					remap[t.v[k]] = vertPerm.size();
					vertPerm.push_back(t.v[k]);
				}
			}
			newTris[i] = Triangle(remap[t.v[0]], remap[t.v[1]], remap[t.v[2]]);
		}
	}

	LxmHeader hdr = {};
	hdr.magic[0] = 'L'; hdr.magic[1] = 'X';
	hdr.magic[2] = 'M'; hdr.magic[3] = '1';
	hdr.version = 1;
	hdr.flags = (HasNormals() ? 1u : 0u) | 2u /* spatially sorted */;
	hdr.vertCount = vertPerm.size();
	hdr.triCount = srcTriCount;
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (uvs.LayerHasValues(i)) hdr.uvsMask |= 1u << i;
		if (cols.LayerHasValues(i)) hdr.colsMask |= 1u << i;
		if (alphas.LayerHasValues(i)) hdr.alphasMask |= 1u << i;
		if (vertAOV.LayerHasValues(i)) hdr.vertAovMask |= 1u << i;
		if (triAOV.LayerHasValues(i)) hdr.triAovMask |= 1u << i;
	}

	ofstream file(fileName, ios::binary | ios::trunc);
	if (!file)
		throw runtime_error("Unable to write .lxm proxy: " + fileName);

	size_t pos = LxmAlignUp(sizeof(hdr));
	const auto pad = [&](ofstream &f, size_t &p) {
		static const char zeros[LxmAlign] = {};
		const size_t n = LxmAlignUp(p) - p;
		if (n) { f.write(zeros, n); p += n; }
	};
	const auto write = [&](const void *d, size_t bytes) {
		pad(file, pos);
		file.write(static_cast<const char *>(d), bytes);
		pos += bytes;
	};

	// Write `perm.size()` elements of src permuted by `perm`
	const auto writePermuted = [&](const auto *src,
			const std::vector<u_int> &perm) {
		using T = std::remove_const_t<std::remove_pointer_t<decltype(src)>>;
		std::vector<T> tmp(perm.size());
		for (size_t i = 0; i < perm.size(); ++i)
			tmp[i] = src[perm[i]];
		write(tmp.data(), tmp.size() * sizeof(T));
	};

	file.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

	writePermuted(srcVerts, vertPerm);
	write(newTris.data(), hdr.triCount * sizeof(Triangle));
	if (hdr.flags & 1u)
		writePermuted(static_cast<const Normal *>(normals.Data()), vertPerm);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (hdr.uvsMask & (1u << i))
			writePermuted(uvs.GetLayer(i).get(), vertPerm);
	}
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (hdr.colsMask & (1u << i))
			writePermuted(cols.GetLayer(i).get(), vertPerm);
	}
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (hdr.alphasMask & (1u << i))
			writePermuted(alphas.GetLayer(i).get(), vertPerm);
	}
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (hdr.vertAovMask & (1u << i))
			writePermuted(vertAOV.GetLayer(i).get(), vertPerm);
	}
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (hdr.triAovMask & (1u << i))
			writePermuted(triAOV.GetLayer(i).get(), triPerm);
	}

	file.flush();
	if (!file.good())
		throw runtime_error("Error while writing .lxm proxy: " + fileName);
}

ExtTriangleMeshUPtr ExtTriangleMesh::LoadProxy(const string &fileName) {
	if (!LxmCheckSizes())
		throw runtime_error(".lxm proxy: unexpected POD sizes, cannot load");

	size_t fileSize = 0;
	std::shared_ptr<void> mapped = MapFileCopyOnWrite(fileName, fileSize);
	if (!mapped)
		throw runtime_error("Unable to map .lxm proxy: " + fileName);

	if (fileSize < sizeof(LxmHeader))
		throw runtime_error("Truncated .lxm proxy: " + fileName);

	const LxmHeader &hdr = *static_cast<const LxmHeader *>(mapped.get());
	if (memcmp(hdr.magic, "LXM1", 4) || hdr.version != 1)
		throw runtime_error("Bad .lxm proxy header: " + fileName);

	const u_int allLayersMask = (EXTMESH_MAX_DATA_COUNT >= 32) ?
			0xffffffffu : ((1u << EXTMESH_MAX_DATA_COUNT) - 1u);
	if ((hdr.uvsMask | hdr.colsMask | hdr.alphasMask |
			hdr.vertAovMask | hdr.triAovMask) & ~allLayersMask)
		throw runtime_error("Bad .lxm proxy layer masks: " + fileName);

	// Whole-file size validation: every section must fit. Bounding each
	// count by fileSize first keeps count*elem from wrapping size_t on
	// crafted headers (every element is >= 4 bytes).
	size_t need = LxmAlignUp(sizeof(LxmHeader));
	const auto section = [&](const u_longlong count, const size_t elem) {
		if (count > fileSize)
			throw runtime_error("Bad .lxm proxy element count: " + fileName);
		need = LxmAlignUp(need);
		need += size_t(count) * elem;
	};
	section(hdr.vertCount, sizeof(Point));
	section(hdr.triCount, sizeof(Triangle));
	if (hdr.flags & 1u)
		section(hdr.vertCount, sizeof(Normal));
	const u_int uvc = std::popcount(hdr.uvsMask);
	const u_int colc = std::popcount(hdr.colsMask);
	const u_int alc = std::popcount(hdr.alphasMask);
	const u_int vac = std::popcount(hdr.vertAovMask);
	const u_int tac = std::popcount(hdr.triAovMask);
	for (u_int i = 0; i < uvc; ++i) section(hdr.vertCount, sizeof(UV));
	for (u_int i = 0; i < colc; ++i) section(hdr.vertCount, sizeof(Spectrum));
	for (u_int i = 0; i < alc; ++i) section(hdr.vertCount, sizeof(float));
	for (u_int i = 0; i < vac; ++i) section(hdr.vertCount, sizeof(float));
	for (u_int i = 0; i < tac; ++i) section(hdr.triCount, sizeof(float));
	if (need > fileSize)
		throw runtime_error("Truncated .lxm proxy: " + fileName);

	// Adopt each section in place; `mapped` (the keeper) is shared by
	// every adopted buffer, so the file stays mapped while any of the
	// mesh's buffers is alive.
	char *base = static_cast<char *>(mapped.get());
	size_t pos = LxmAlignUp(sizeof(LxmHeader));
	const auto take = [&](const size_t bytes) -> void * {
		pos = LxmAlignUp(pos);
		void *p = base + pos;
		pos += bytes;
		return p;
	};

	const size_t vBytes = size_t(hdr.vertCount) * sizeof(Point);
	const size_t tBytes = size_t(hdr.triCount) * sizeof(Triangle);

	VertexBuffer verts = VertexBuffer::Adopt(take(vBytes), vBytes, mapped);
	TriangleBuffer trisBuf = TriangleBuffer::Adopt(take(tBytes), tBytes, mapped);
	NormalBuffer norms;
	if (hdr.flags & 1u)
		norms = NormalBuffer::Adopt(take(size_t(hdr.vertCount) * sizeof(Normal)),
				size_t(hdr.vertCount) * sizeof(Normal), mapped);

	ExtMeshProp<UV> uvs;
	ExtMeshProp<Spectrum> cols;
	ExtMeshProp<float> alphas, vertAOVs, triAOVs;
	const auto propLayer = [&](auto &prop, const u_int mask,
			const u_int i, const u_longlong count) {
		if (!(mask & (1u << i)))
			return;
		using Prop = std::decay_t<decltype(prop)>;
		using E = std::remove_extent_t<typename Prop::Layer::element_type>;
		const size_t bytes = size_t(count) * sizeof(E);
		auto *p = static_cast<E *>(take(bytes));
		prop.SetLayer(i, typename Prop::Layer(mapped, p), size_t(count));
	};
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i)
		propLayer(uvs, hdr.uvsMask, i, hdr.vertCount);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i)
		propLayer(cols, hdr.colsMask, i, hdr.vertCount);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i)
		propLayer(alphas, hdr.alphasMask, i, hdr.vertCount);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i)
		propLayer(vertAOVs, hdr.vertAovMask, i, hdr.vertCount);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i)
		propLayer(triAOVs, hdr.triAovMask, i, hdr.triCount);

	auto mesh = std::make_unique<ExtTriangleMesh>(
			std::move(verts), std::move(trisBuf), std::move(norms),
			uvs, cols, alphas);
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		if (vertAOVs.LayerHasValues(i))
			mesh->SetVertexAOV(i, vertAOVs.GetLayer(i), size_t(hdr.vertCount));
		if (triAOVs.LayerHasValues(i))
			mesh->SetTriAOV(i, triAOVs.GetLayer(i), size_t(hdr.triCount));
	}

	return mesh;
}

//------------------------------------------------------------------------------
// ExtTriangleMesh Load
//------------------------------------------------------------------------------

ExtTriangleMeshUPtr ExtTriangleMesh::Load(const string &fileName) {
	const std::filesystem::path ext = std::filesystem::path(fileName).extension();
	if (ext == ".ply")
		return LoadPly(fileName);
	else if (ext == ".bpy")
		return LoadSerialized(fileName);
	else if (ext == ".lxm")
		return LoadProxy(fileName);
	else
		throw runtime_error("Unknown file extension while loading a mesh from: " + fileName);	
}

//------------------------------------------------------------------------------
// ExtTriangleMesh Save
//------------------------------------------------------------------------------

void ExtTriangleMesh::Save(const string &fileName) const {
	const std::filesystem::path ext = std::filesystem::path(fileName).extension();
	if (ext == ".ply")
		SavePly(fileName);
	else if (ext == ".bpy")
		SaveSerialized(fileName);
	else if (ext == ".lxm")
		SaveProxy(fileName);
	else
		throw runtime_error("Unknown file extension while saving a mesh to: " + fileName);
}

void ExtTriangleMesh::SavePly(const string &fileName) const {
	// The use of std::filesystem::path is required for UNICODE support: fileName
	// is supposed to be UTF-8 encoded.
	std::ofstream plyFile(std::filesystem::path(fileName),
			std::ofstream::out |
			std::ofstream::binary |
			std::ofstream::trunc);
	if(!plyFile.is_open())
		throw runtime_error("Unable to open: " + fileName);

	plyFile.imbue(cLocale);
	auto vertCount = vertices.Count();
	auto triCount = tris.Count();


	// Write the PLY header
	plyFile << "ply\n"
			"format " + string(ply_storage_mode_list[ply_arch_endian()]) + " 1.0\n"
			"comment Created by LuxRays v" LUXRAYS_VERSION "\n"
			"element vertex " << vertCount << "\n"
			"property float x\n"
			"property float y\n"
			"property float z\n";

	if (HasNormals())
		plyFile << "property float nx\n"
				"property float ny\n"
				"property float nz\n";

	// This is our own extension to file PLY format in order to support multiple
	// UVs, Colors, Alphas and Vertex AOVs for each vertex
	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		const string suffix = (i == 0) ? "" : ToString(i);

		if (HasUVs(i))
			plyFile << "property float s" << suffix << "\n"
					"property float t" << suffix << "\n";

		if (HasColors(i))
			plyFile << "property float red" << suffix << "\n"
					"property float green" << suffix << "\n"
					"property float blue" << suffix << "\n";

		if (HasAlphas(i))
			plyFile << "property float alpha" << suffix << "\n";	

		if (HasVertexAOV(i))
			plyFile << "property float vertaov" << suffix << "\n";	
	}

	plyFile << "element face " << triCount << "\n"
				"property list uchar uint vertex_indices\n";

	for (u_int i = 0; i < EXTMESH_MAX_DATA_COUNT; ++i) {
		const string suffix = (i == 0) ? "" : ToString(i);

		if (HasTriAOV(i))
			plyFile << "element faceaov" << suffix << " " << triCount << "\n"
					"property float triaov\n";
	}

	plyFile << "end_header\n";

	if (!plyFile.good())
		throw runtime_error("Unable to write PLY header to: " + fileName);

	// Write all vertex data
	for (size_t i = 0; i < vertCount; ++i) {
		plyFile.write((char *)&vertices[i], sizeof(Point));
		if (HasNormals())
			plyFile.write((char *)&normals[i], sizeof(Normal));

		for (size_t j = 0; j < EXTMESH_MAX_DATA_COUNT; ++j) {
			if (HasUVs(j))
				plyFile.write((char *)&uvs[j][i], sizeof(UV));
			if (HasColors(j))
				plyFile.write((char *)&cols[j][i], sizeof(Spectrum));
			if (HasAlphas(j))
				plyFile.write((char *)&alphas[j][i], sizeof(float));
			if (HasVertexAOV(j))
				plyFile.write((char *)&vertAOV[j][i], sizeof(float));
		}
	}

	if (!plyFile.good())
		throw runtime_error("Unable to write PLY vertex data to: " + fileName);

	// Write all face data
	const u_char len = 3;
	for (u_int i = 0; i < triCount; ++i) {
		plyFile.write((char *)&len, 1);
		plyFile.write((char *)&tris[i], sizeof(Triangle));
	}

	for (u_int j = 0; j < EXTMESH_MAX_DATA_COUNT; ++j) {
		if (HasTriAOV(j)) {
			for (u_int i = 0; i < triCount; ++i)
				plyFile.write((char *)&triAOV[j][i], sizeof(float));
		}
	}

	if (!plyFile.good())
		throw runtime_error("Unable to write PLY face data to: " + fileName);

	plyFile.close();
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

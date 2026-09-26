/***************************************************************************
 * Copyright 1998-2013 by authors (see AUTHORS.txt)                        *
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

#include <memory>

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>

#include "slg/textures/densitygrid.h"
#include "luxcore/luxcore.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// DensityGrid texture
//------------------------------------------------------------------------------

DensityGridTexture::DensityGridTexture(TextureMapping3DUPtr&& mp,
		const u_int nx, const u_int ny, const u_int nz,
        ImageMapConstRef map) : mapping(std::move(mp)),
		nx(nx), ny(ny), nz(nz), imageMap(map) {
}

ImageMapUPtr DensityGridTexture::ParseData(const luxrays::Property &dataProp,
		const bool isRGB,
		const u_int nx, const u_int ny, const u_int nz,
		const ImageMapStorage::StorageType storageType,
		const ImageMapStorage::WrapType wrapMode) {
	// Create an image map with the data

	// NOTE: wrapMode is only stored inside the ImageMap but is not then used to
	// sample the image. The image data are accessed directly and the wrapping is
	// implemented by the code accessing the data.

	const u_int channelCount = isRGB ? 3 : 1;
	auto imgMap(ImageMap::AllocImageMap(channelCount, nx, ny * nz,
			ImageMapConfig(1.f,
				(storageType == ImageMapStorage::AUTO) ? ImageMapStorage::HALF : storageType,
				wrapMode,
				ImageMapStorage::ChannelSelectionType::DEFAULT)));

	auto& imgStorage = imgMap->GetStorage();

	if (isRGB) {
		for (u_int z = 0, i = 0; z < nz; ++z)
			for (u_int y = 0; y < ny; ++y)
				for (u_int x = 0; x < nx; ++x, ++i) {
					const float r = dataProp.Get<double>(i * 3 + 0);
					const float g = dataProp.Get<double>(i * 3 + 1);
					const float b = dataProp.Get<double>(i * 3 + 2);

					imgStorage.SetSpectrum((z * ny + y) * nx + x, Spectrum(r, g, b));
				}
	} else {
		for (u_int z = 0, i = 0; z < nz; ++z)
			for (u_int y = 0; y < ny; ++y)
				for (u_int x = 0; x < nx; ++x, ++i)
					imgStorage.SetFloat((z * ny + y) * nx + x, dataProp.Get<double>(i));
	}

	return imgMap;
}

ImageMapUPtr DensityGridTexture::ParseOpenVDB(
	const string &fileName, const string &gridName,
	const u_int nx, const u_int ny, const u_int nz,
	const ImageMapStorage::StorageType storageType,
	const ImageMapStorage::WrapType wrapMode
) {
	SDL_LOG("OpenVDB file: " + fileName);

	openvdb::io::File file(fileName);	
	file.open();

	// List the grids included in the file
	SDL_LOG("OpenVDB grid names:");
	for (auto i = file.beginName(); i != file.endName(); ++i)
		SDL_LOG("  [" + *i + "]");

	// Check if the file has the specified grid
	if (!file.hasGrid(gridName))
		throw runtime_error("Unknown grid " + gridName + " in OpenVDB file " + fileName);
	
	// Read the grid from the file
	openvdb::GridBase::Ptr ovdbGrid = file.readGrid(gridName);

	// Compute the scale factor
	openvdb::CoordBBox gridBBox;
	ovdbGrid->baseTree().evalLeafBoundingBox(gridBBox);

	//const openvdb::CoordBBox gridBBox = ovdbGrid->evalActiveVoxelBoundingBox();
	SDL_LOG("OpenVDB grid bbox: "
			"[(" << gridBBox.min()[0] << ", " << gridBBox.min()[1] << ", " << gridBBox.min()[2] << "), "
			"(" << gridBBox.max()[0] << ", " << gridBBox.max()[1] << ", " << gridBBox.max()[2] << ")]");
	const openvdb::Coord gridBBoxSize = gridBBox.max() - gridBBox.min();
	SDL_LOG("OpenVDB grid size: (" << gridBBoxSize[0] << ", " << gridBBoxSize[1] << ", " << gridBBoxSize[2] << ")");
	
	const openvdb::Vec3f scale = openvdb::Vec3f(
		gridBBoxSize[0] / (float)nx,
		gridBBoxSize[1] / (float)ny,
		gridBBoxSize[2] / (float)nz);


	SDL_LOG("OpenVDB grid type: " + ovdbGrid->valueType());
	const u_int channelsCount =
			((ovdbGrid->valueType() == "vec3s") ||
			(ovdbGrid->valueType() == "vec3f") ||
			(ovdbGrid->valueType() == "vec3d")) ? 3 : 1;

	// NOTE: wrapMode is only stored inside the ImageMap but is not then used to
	// sample the image. The image data are accessed directly and the wrapping is
	// implemented by the code accessing the data.

	ImageMapUPtr imgMap(ImageMap::AllocImageMap(channelsCount, nx, ny * nz,
			ImageMapConfig(1.f,
				storageType,
				wrapMode,
				ImageMapStorage::ChannelSelectionType::DEFAULT)));

	auto& imgStorage = imgMap->GetStorage();	

	if (channelsCount == 3) {
		// Check if it is the right type of grid
		openvdb::VectorGrid::Ptr grid = openvdb::gridPtrCast<openvdb::VectorGrid>(ovdbGrid);
		if (!grid)
			throw runtime_error("Wrong OpenVDB file type in parsing file " + fileName + " for grid " + gridName);

		#pragma omp parallel for
		for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int z = 0; z < nz; ++z) {
			for (u_int y = 0; y < ny; ++y) {
				for (u_int x = 0; x < nx; ++x) {
					const openvdb::Vec3f xyz = scale * openvdb::Vec3f(x, y, z) + gridBBox.min();

					openvdb::VectorGrid::ValueType v;
					openvdb::tools::QuadraticSampler::sample(grid->tree(), xyz, v);
					openvdb::Vec3f v3f = v;

					imgStorage.SetSpectrum((z * ny + y) * nx + x, Spectrum(v3f.x(), v3f.y(), v3f.z()));
				}
			}
		}
	} else {
		// Check if it is the right type of grid
		openvdb::ScalarGrid::Ptr grid = openvdb::gridPtrCast<openvdb::ScalarGrid>(ovdbGrid);
		if (!grid)
			throw runtime_error("Wrong OpenVDB file type in parsing file " + fileName + " for grid " + gridName);

		#pragma omp parallel for
		for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int z = 0; z < nz; ++z) {
			for (u_int y = 0; y < ny; ++y) {
				for (u_int x = 0; x < nx; ++x) {
					const openvdb::Vec3f xyz = scale * openvdb::Vec3f(x, y, z) + gridBBox.min();

					openvdb::ScalarGrid::ValueType v;
					openvdb::tools::QuadraticSampler::sample(grid->tree(), xyz, v);

					imgStorage.SetFloat((z * ny + y) * nx + x, v);
				}
			}
		}
	}

	file.close();

	return std::move(imgMap);
}

Spectrum DensityGridTexture::D(int x, int y, int z) const {
	return imageMap.GetStorage().GetSpectrum(((Clamp(z, 0, nz - 1) * ny) + Clamp(y, 0, ny - 1)) * nx + Clamp(x, 0, nx - 1));
}

Spectrum DensityGridTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	const Point P(mapping->Map(hitPoint));

	float x, y, z;
	int vx, vy, vz;

	switch (imageMap.GetStorage().GetWrapType()) {
		case ImageMapStorage::REPEAT:
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
		case ImageMapStorage::BLACK:
			if (P.x < 0.f || P.x >= 1.f ||
				P.y < 0.f || P.y >= 1.f ||
				P.z < 0.f || P.z >= 1.f)
				return 0.f;
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
		case ImageMapStorage::WHITE:
			if (P.x < 0.f || P.x >= 1.f ||
				P.y < 0.f || P.y >= 1.f ||
				P.z < 0.f || P.z >= 1.f)
				return 1.f;
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
		case ImageMapStorage::CLAMP:
			x = Clamp(P.x, 0.f, 1.f) * nx;
			vx = Min(Floor2Int(x), nx - 1);
			x -= vx;
			y = Clamp(P.y, 0.f, 1.f) * ny;
			vy = Min(Floor2Int(P.y * ny), ny - 1);
			y -= vy;
			z = Clamp(P.z, 0.f, 1.f) * nz;
			vz = Min(Floor2Int(P.z * nz), nz - 1);
			z -= vz;
			break;
		default:
			return 0.f;
	}

	// Trilinear interpolation of the grid element
	return Lerp(z,
		Lerp(y, Lerp(x, D(vx, vy, vz), D(vx + 1, vy, vz)), Lerp(x, D(vx, vy + 1, vz), D(vx + 1, vy + 1, vz))),
		Lerp(y, Lerp(x, D(vx, vy, vz + 1), D(vx + 1, vy, vz + 1)), Lerp(x, D(vx, vy + 1, vz + 1), D(vx + 1, vy + 1, vz + 1))));
}

float DensityGridTexture::GetFloatValue(const HitPoint &hitPoint) const {
	return EvalSpectrumValue(hitPoint).Y();
}

bool DensityGridTexture::GetMaxInWorldBBox(const BBox &box, float *maxValue) const {
	// Only affine world->local mappings allow to bound a world-space box:
	// UVMapping3D depends on per-vertex UVs and LocalRandomMapping3D is
	// randomized per object/triangle.
	const TextureMapping3DType mappingType = mapping->GetType();
	if ((mappingType != GLOBALMAPPING3D) && (mappingType != LOCALMAPPING3D))
		return false;

	// Map the 8 world-space box corners to texture UVW space
	Point uvwMin(INFINITY, INFINITY, INFINITY);
	Point uvwMax(-INFINITY, -INFINITY, -INFINITY);
	for (u_int i = 0; i < 8; ++i) {
		const Point corner(
				(i & 1) ? box.pMax.x : box.pMin.x,
				(i & 2) ? box.pMax.y : box.pMin.y,
				(i & 4) ? box.pMax.z : box.pMin.z);
		const Point p = mapping->worldToLocal * corner;
		uvwMin = Point(Min(uvwMin.x, p.x), Min(uvwMin.y, p.y), Min(uvwMin.z, p.z));
		uvwMax = Point(Max(uvwMax.x, p.x), Max(uvwMax.y, p.y), Max(uvwMax.z, p.z));
	}

	const ImageMapStorage::WrapType wrap = imageMap.GetStorage().GetWrapType();

	// If the box maps outside the [0, 1)^3 domain
	const bool hasOutside =
			(uvwMin.x < 0.f) || (uvwMin.y < 0.f) || (uvwMin.z < 0.f) ||
			(uvwMax.x >= 1.f) || (uvwMax.y >= 1.f) || (uvwMax.z >= 1.f) ||
			!isfinite(uvwMin.x) || !isfinite(uvwMax.x) ||
			!isfinite(uvwMin.y) || !isfinite(uvwMax.y) ||
			!isfinite(uvwMin.z) || !isfinite(uvwMax.z);
	if ((wrap == ImageMapStorage::BLACK) &&
			((uvwMin.x >= 1.f) || (uvwMin.y >= 1.f) || (uvwMin.z >= 1.f) ||
			(uvwMax.x <= 0.f) || (uvwMax.y <= 0.f) || (uvwMax.z <= 0.f))) {
		*maxValue = 0.f;
		return true;
	}
	const float outsideValue =
			((wrap == ImageMapStorage::WHITE) && hasOutside) ? 1.f : 0.f;

	// The trilinear interpolation at UVW coordinate u reads the voxel
	// vertices [floor(u * n), floor(u * n) + 1], so the range of voxels
	// covering the box is [floor(min * n), floor(max * n) + 1].
	int i0, i1, j0, j1, k0, k1;
	if (wrap == ImageMapStorage::REPEAT) {
		// REPEAT tiles the whole grid
		i0 = j0 = k0 = 0;
		i1 = nx - 1; j1 = ny - 1; k1 = nz - 1;
	} else {
		// BLACK, WHITE and CLAMP only read voxels of the [0, 1]^3 part of
		// the mapped box (the outside contribution is in outsideValue or,
		// for CLAMP, edge voxels included by the clamped range)
		const float u0 = Clamp(uvwMin.x, 0.f, 1.f), u1 = Clamp(uvwMax.x, 0.f, 1.f);
		const float v0 = Clamp(uvwMin.y, 0.f, 1.f), v1 = Clamp(uvwMax.y, 0.f, 1.f);
		const float w0 = Clamp(uvwMin.z, 0.f, 1.f), w1 = Clamp(uvwMax.z, 0.f, 1.f);
		i0 = Floor2Int(u0 * nx); i1 = Floor2Int(u1 * nx) + 1;
		j0 = Floor2Int(v0 * ny); j1 = Floor2Int(v1 * ny) + 1;
		k0 = Floor2Int(w0 * nz); k1 = Floor2Int(w1 * nz) + 1;
	}
	i0 = Clamp(i0, 0, nx - 1); i1 = Clamp(i1, 0, nx - 1);
	j0 = Clamp(j0, 0, ny - 1); j1 = Clamp(j1, 0, ny - 1);
	k0 = Clamp(k0, 0, nz - 1); k1 = Clamp(k1, 0, nz - 1);

	auto& imgStorage = imageMap.GetStorage();
	float maxV = outsideValue;
	for (int z = k0; z <= k1; ++z)
		for (int y = j0; y <= j1; ++y)
			for (int x = i0; x <= i1; ++x)
				maxV = Max(maxV, imgStorage.GetSpectrum((z * ny + y) * nx + x).Max());

	*maxValue = maxV;
	return true;
}

bool DensityGridTexture::GetMinInWorldBBox(const BBox &box, float *minValue) const {
	const TextureMapping3DType mappingType = mapping->GetType();
	if ((mappingType != GLOBALMAPPING3D) && (mappingType != LOCALMAPPING3D))
		return false;

	Point uvwMin(INFINITY, INFINITY, INFINITY);
	Point uvwMax(-INFINITY, -INFINITY, -INFINITY);
	for (u_int i = 0; i < 8; ++i) {
		const Point corner(
				(i & 1) ? box.pMax.x : box.pMin.x,
				(i & 2) ? box.pMax.y : box.pMin.y,
				(i & 4) ? box.pMax.z : box.pMin.z);
		const Point p = mapping->worldToLocal * corner;
		uvwMin = Point(Min(uvwMin.x, p.x), Min(uvwMin.y, p.y), Min(uvwMin.z, p.z));
		uvwMax = Point(Max(uvwMax.x, p.x), Max(uvwMax.y, p.y), Max(uvwMax.z, p.z));
	}

	const ImageMapStorage::WrapType wrap = imageMap.GetStorage().GetWrapType();

	const bool fullyOutside =
			(uvwMin.x >= 1.f) || (uvwMin.y >= 1.f) || (uvwMin.z >= 1.f) ||
			(uvwMax.x <= 0.f) || (uvwMax.y <= 0.f) || (uvwMax.z <= 0.f);
	if (fullyOutside && !isfinite(uvwMin.x)) {
		// Degenerate box (e.g. the whole space): fall back to a scan of the
		// full grid plus the out-of-domain contribution
	} else if (fullyOutside) {
		*minValue = (wrap == ImageMapStorage::WHITE) ? 1.f :
				(wrap == ImageMapStorage::BLACK) ? 0.f : 0.f;
		// CLAMP maps the whole box to a single edge value: it is handled by
		// the clamped voxel range below instead
		if (wrap != ImageMapStorage::CLAMP && wrap != ImageMapStorage::REPEAT)
			return true;
	}
	const bool hasOutside =
			(uvwMin.x < 0.f) || (uvwMin.y < 0.f) || (uvwMin.z < 0.f) ||
			(uvwMax.x >= 1.f) || (uvwMax.y >= 1.f) || (uvwMax.z >= 1.f) ||
			!isfinite(uvwMin.x) || !isfinite(uvwMax.x) ||
			!isfinite(uvwMin.y) || !isfinite(uvwMax.y) ||
			!isfinite(uvwMin.z) || !isfinite(uvwMax.z);

	// See GetMaxInWorldBBox() for the voxel range computation
	int i0, i1, j0, j1, k0, k1;
	if (wrap == ImageMapStorage::REPEAT) {
		i0 = j0 = k0 = 0;
		i1 = nx - 1; j1 = ny - 1; k1 = nz - 1;
	} else {
		const float u0 = Clamp(uvwMin.x, 0.f, 1.f), u1 = Clamp(uvwMax.x, 0.f, 1.f);
		const float v0 = Clamp(uvwMin.y, 0.f, 1.f), v1 = Clamp(uvwMax.y, 0.f, 1.f);
		const float w0 = Clamp(uvwMin.z, 0.f, 1.f), w1 = Clamp(uvwMax.z, 0.f, 1.f);
		i0 = Floor2Int(u0 * nx); i1 = Floor2Int(u1 * nx) + 1;
		j0 = Floor2Int(v0 * ny); j1 = Floor2Int(v1 * ny) + 1;
		k0 = Floor2Int(w0 * nz); k1 = Floor2Int(w1 * nz) + 1;
	}
	i0 = Clamp(i0, 0, nx - 1); i1 = Clamp(i1, 0, nx - 1);
	j0 = Clamp(j0, 0, ny - 1); j1 = Clamp(j1, 0, ny - 1);
	k0 = Clamp(k0, 0, nz - 1); k1 = Clamp(k1, 0, nz - 1);

	// Out-of-domain contributions: BLACK wrap adds a 0 (lowers the minimum),
	// WHITE wrap adds a 1 (never lowers it), CLAMP/REPEAT are covered by the
	// scanned voxel range.
	auto& imgStorage = imageMap.GetStorage();
	float minV = ((wrap == ImageMapStorage::BLACK) && hasOutside) ? 0.f : INFINITY;
	for (int z = k0; z <= k1; ++z)
		for (int y = j0; y <= j1; ++y)
			for (int x = i0; x <= i1; ++x)
				minV = Min(minV, imgStorage.GetSpectrum((z * ny + y) * nx + x).Min());

	if (!isfinite(minV))
		minV = 0.f;
	*minValue = minV;
	return true;
}

PropertiesUPtr DensityGridTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("densitygrid"));
	props->Set(Property("scene.textures." + name + ".nx")(nx));
	props->Set(Property("scene.textures." + name + ".ny")(ny));
	props->Set(Property("scene.textures." + name + ".nz")(nz));
	props->Set(Property("scene.textures." + name + ".wrap")(ImageMapStorage::WrapType2String(imageMap.GetStorage().GetWrapType())));
	
	Property dataProp("scene.textures." + name + ".data");
	auto& imgStorage = imageMap.GetStorage();

	for (int z = 0; z < nz; ++z)
		for (int y = 0; y < ny; ++y)
			for (int x = 0; x < nx; ++x)
				dataProp.Add<float>(imgStorage.GetFloat((z * ny + y) * nx + x));

	props->Set(dataProp);
	
	props->Set(mapping->ToProperties("scene.textures." + name + ".mapping"));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

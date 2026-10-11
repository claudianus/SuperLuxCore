/***************************************************************************
 * Copyright 2026 SuperLuxCore contributors
 * Licensed under the Apache License, Version 2.0.
 ***************************************************************************/

#ifndef _SLG_OPENVDBGRID_H
#define _SLG_OPENVDBGRID_H

#include <mutex>
#include <openvdb/openvdb.h>

namespace slg {

// Cycles uses this additional tree for internally authored RGBA voxels.
// Register it here before reading external metadata or voxel data.
using OpenVDBVec4fGrid = openvdb::Grid<openvdb::tree::Tree4<openvdb::Vec4f, 5, 4, 3>::Type>;

inline void EnsureOpenVDBGridTypes() {
	static std::once_flag initialized;
	std::call_once(initialized, [] {
		openvdb::initialize();
		if (!OpenVDBVec4fGrid::isRegistered())
			OpenVDBVec4fGrid::registerGrid();
	});
}

}

#endif

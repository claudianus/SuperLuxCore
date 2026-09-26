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

#include <filesystem>

#include <boost/serialization/split_free.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include "luxrays/usings.h"
#include "slg/scene/extmeshcache.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// ExtMeshCache serialization code
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::ExtMeshCache)

template<class Archive> void ExtMeshCache::load(Archive &ar, const u_int version) {
	// Load the size
	u_int size;
	ar & size;

	for (u_int i = 0; i < size; ++i) {
		// Load the mesh (serialized as a raw pointer; the cache takes
		// ownership here)
		luxrays::ExtMesh *raw = nullptr;
		ar & raw;
		luxrays::ExtMeshUPtr m(raw);

		// File-backed proxy meshes are serialized as a name-only stub
		// (see save below): re-map the .lxm file instead of keeping an
		// empty mesh.
		// By-value: `m` is replaced below, which would free the storage
		// a reference points into
		const string mname = m->GetName();
		auto *tri = dynamic_cast<luxrays::ExtTriangleMesh *>(m.get());
		if (tri && tri->GetTotalVertexCount() == 0 && mname.size() > 4 &&
				mname.compare(mname.size() - 4, 4, ".lxm") == 0) {
			if (!std::filesystem::exists(mname))
				throw runtime_error("Proxy mesh file not found while "
						"loading serialized scene: " + mname);
			auto proxy = luxrays::ExtTriangleMesh::LoadProxy(mname);
			proxy->SetName(mname);
			Transform l2w;
			tri->GetLocal2World(0.f, l2w);
			proxy->SetLocal2World(l2w);
			m = std::move(proxy);
			SDL_LOG("Re-mapped proxy mesh: " << mname);
		}

		SDL_LOG("Loading serialized mesh: " << m->GetName());
		meshes.DefineObj(std::move(m));
	}

	ar & deleteMeshData;
}

template<class Archive> void ExtMeshCache::save(Archive &ar, const u_int version) const {
	// Save the size
	const u_int size = meshes.GetSize();
	ar & size;

	for (u_int i = 0; i < size; ++i) {
		// Non-owning pointer: the cache keeps owning the mesh
		ExtMesh *m = dynamic_cast<ExtMesh *>(meshes.objs[i].get());

		// .lxm proxy meshes serialize as a name-only stub: the reader
		// re-maps the file on load (see load above), so the archive
		// stays small and the child process keeps the mesh file-backed
		// instead of paging every byte into heap.
		std::unique_ptr<ExtTriangleMesh> stub;
		ExtTriangleMesh *etm = dynamic_cast<ExtTriangleMesh *>(m);
		if (etm && etm->buffersFromFileMapping &&
				std::filesystem::exists(etm->GetName())) {
			stub = std::make_unique<ExtTriangleMesh>();
			stub->SetName(etm->GetName());
			Transform l2w;
			etm->GetLocal2World(0.f, l2w);
			stub->SetLocal2World(l2w);
			m = stub.get();
			SDL_LOG("Saving proxy mesh as file reference: " <<
					etm->GetName());
		} else {
			SDL_LOG("Saving serialized mesh: " << m->GetName());
		}

		// Save the mesh
		ar & m;
	}

	ar & deleteMeshData;
}

namespace slg {
// Explicit instantiations for portable archives
template void ExtMeshCache::save(LuxOutputArchive &ar, const u_int version) const;
template void ExtMeshCache::load(LuxInputArchive &ar, const u_int version);
template void ExtMeshCache::save(LuxOutputArchiveText &ar, const u_int version) const;
template void ExtMeshCache::load(LuxInputArchiveText &ar, const u_int version);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

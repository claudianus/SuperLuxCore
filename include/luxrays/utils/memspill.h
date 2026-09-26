/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
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

#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace luxrays {

// Spills `bytes` of memory at `ptr` to `fileName`, maps the file back
// copy-on-write (PROT_READ|PROT_WRITE, MAP_PRIVATE) and returns an owning
// handle: the mapping stays valid while the returned pointer is alive and
// is munmapped on release. The file is unlinked right after the mapping,
// so nothing is left on disk once the mapping dies.
//
// File-backed clean pages can be evicted by the kernel under memory
// pressure (unlike anonymous pages, which would have to go through swap)
// and are paged back on demand — this is what turns large scene buffers
// into out-of-core data. Writes still work: dirty pages become anonymous
// copy-on-write pages.
//
// Returns nullptr on any failure (non-POSIX platform, I/O error);
// callers keep their in-memory copy.
std::shared_ptr<void> SpillToFile(const void *ptr, std::size_t bytes,
		const std::string &fileName);

}  // namespace luxrays

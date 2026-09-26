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

#include "luxrays/utils/memspill.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace luxrays {

std::shared_ptr<void> SpillToFile(const void *ptr, const std::size_t bytes,
		const std::string &fileName) {
#if defined(_WIN32)
	// Windows version (CreateFileMapping) not implemented yet: keep the
	// in-memory copy
	return nullptr;
#else
	if (!ptr || !bytes)
		return nullptr;

	const int fd = open(fileName.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return nullptr;
	if (ftruncate(fd, (off_t)bytes))
		goto fail;

	{
		const char *src = static_cast<const char *>(ptr);
		size_t off = 0;
		while (off < bytes) {
			const ssize_t written = write(fd, src + off, bytes - off);
			if (written <= 0)
				goto fail;
			off += (size_t)written;
		}
	}

	{
		void *mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
				MAP_PRIVATE, fd, 0);
		close(fd);
		// The file only exists to back the mapping
		unlink(fileName.c_str());
		if (mapped == MAP_FAILED)
			return nullptr;

		// Buffer access is scattered (BVH hits, shading gathers): avoid
		// read-ahead of pages that will likely never be touched
		madvise(mapped, bytes, MADV_RANDOM);

		return std::shared_ptr<void>(mapped,
				[bytes](void *p) { munmap(p, bytes); });
	}

fail:
	close(fd);
	unlink(fileName.c_str());
	return nullptr;
#endif
}

}  // namespace luxrays

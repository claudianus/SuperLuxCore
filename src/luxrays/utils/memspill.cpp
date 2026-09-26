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
#else
#include <windows.h>
#endif

namespace luxrays {

std::shared_ptr<void> SpillToFile(const void *ptr, const std::size_t bytes,
		const std::string &fileName) {
#if defined(_WIN32)
	if (!ptr || !bytes)
		return nullptr;

	// DELETE_ON_CLOSE + a live section object: the file vanishes when the
	// last view/handle dies, matching the POSIX unlink-after-mmap scheme.
	const HANDLE hFile = CreateFileA(fileName.c_str(),
			GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return nullptr;

	const char *src = static_cast<const char *>(ptr);
	size_t off = 0;
	while (off < bytes) {
		const DWORD chunk = (DWORD)std::min<size_t>(bytes - off, 1u << 30);
		DWORD written = 0;
		if (!WriteFile(hFile, src + off, chunk, &written, nullptr) || !written) {
			CloseHandle(hFile);
			return nullptr;
		}
		off += written;
	}

	// PAGE_WRITECOPY + FILE_MAP_COPY: private pages, never written back
	HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_WRITECOPY, 0, 0,
			nullptr);
	if (!hMap) {
		CloseHandle(hFile);
		return nullptr;
	}
	void *mapped = MapViewOfFile(hMap, FILE_MAP_COPY, 0, 0, bytes);
	CloseHandle(hFile);  // the section keeps the file object alive
	if (!mapped) {
		CloseHandle(hMap);
		return nullptr;
	}
	return std::shared_ptr<void>(mapped,
			[hMap](void *p) { UnmapViewOfFile(p); CloseHandle(hMap); });
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

std::shared_ptr<void> MapFileCopyOnWrite(const std::string &fileName,
		std::size_t &size) {
	size = 0;
#if defined(_WIN32)
	// PAGE_WRITECOPY needs a read/write handle per MSDN; the file itself
	// is never modified — writes land on private pages only.
	HANDLE hFile = CreateFileA(fileName.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	bool readOnlyView = false;
	if (hFile == INVALID_HANDLE_VALUE) {
		// Read-only assets (network shares, committed proxies): map
		// read-only instead. Callers only read mapped file content;
		// private writes would fault — same as POSIX O_RDONLY+MAP_PRIVATE
		// which needs PROT_WRITE though, so this is a graceful subset.
		hFile = CreateFileA(fileName.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		readOnlyView = true;
	}
	if (hFile == INVALID_HANDLE_VALUE)
		return nullptr;
	LARGE_INTEGER fs;
	if (!GetFileSizeEx(hFile, &fs) || fs.QuadPart <= 0) {
		CloseHandle(hFile);
		return nullptr;
	}
	size = (std::size_t)fs.QuadPart;

	HANDLE hMap = CreateFileMappingA(hFile, nullptr,
			readOnlyView ? PAGE_READONLY : PAGE_WRITECOPY, 0, 0, nullptr);
	CloseHandle(hFile);
	if (!hMap) {
		size = 0;
		return nullptr;
	}
	void *mapped = MapViewOfFile(hMap,
			readOnlyView ? FILE_MAP_READ : FILE_MAP_COPY, 0, 0, size);
	if (!mapped) {
		CloseHandle(hMap);
		size = 0;
		return nullptr;
	}
	return std::shared_ptr<void>(mapped,
			[hMap](void *p) { UnmapViewOfFile(p); CloseHandle(hMap); });
#else
	const int fd = open(fileName.c_str(), O_RDONLY);
	if (fd < 0)
		return nullptr;

	struct stat st;
	if (fstat(fd, &st) || st.st_size <= 0) {
		close(fd);
		return nullptr;
	}
	size = (std::size_t)st.st_size;

	void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE, fd, 0);
	close(fd);
	if (mapped == MAP_FAILED) {
		size = 0;
		return nullptr;
	}
	madvise(mapped, size, MADV_RANDOM);

	return std::shared_ptr<void>(mapped,
			[size](void *p) { munmap(p, size); });
#endif
}

}  // namespace luxrays

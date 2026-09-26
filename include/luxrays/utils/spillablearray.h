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
#include <type_traits>
#include <utility>
#include <vector>

#include "luxrays/utils/memspill.h"

namespace luxrays {

// A grow-only array (vector-like append interface) whose contents can be
// swapped for a copy-on-write file mapping via Spill(). After spilling,
// data()/size()/operator[] keep working transparently through the
// mapping, so consumers re-reading the array (e.g. a second render device
// uploading the same staging buffers, or a device thread restarted after
// a camera edit) page the data back on demand while the kernel is free to
// evict the clean file-backed pages under memory pressure.
//
// Any mutating call (resize/push_back/Append/Reset) pulls the contents
// back into heap storage first if the array is currently spilled.
template<typename T>
class SpillableArray {
public:
	SpillableArray() = default;

	// vector move keeps the underlying allocation, so `ptr` stays valid
	SpillableArray(SpillableArray &&o) noexcept :
			storage(std::move(o.storage)), keeper(std::move(o.keeper)),
			ptr(o.ptr), count(o.count) {
		o.ptr = nullptr; o.count = 0;
	}
	SpillableArray &operator=(SpillableArray &&o) noexcept {
		storage = std::move(o.storage);
		keeper = std::move(o.keeper);
		ptr = o.ptr; count = o.count;
		o.ptr = nullptr; o.count = 0;
		return *this;
	}
	SpillableArray(const SpillableArray &) = delete;
	SpillableArray &operator=(const SpillableArray &) = delete;

	size_t size() const { return count; }
	bool empty() const { return count == 0; }
	T *data() { return ptr; }
	const T *data() const { return ptr; }
	T &operator[](const size_t i) { return ptr[i]; }
	const T &operator[](const size_t i) const { return ptr[i]; }
	T &front() { return ptr[0]; }
	const T &front() const { return ptr[0]; }
	T &back() { return ptr[count - 1]; }
	const T &back() const { return ptr[count - 1]; }

	// Raw pointers double as iterators (storage is always contiguous)
	typedef T *iterator;
	typedef const T *const_iterator;
	iterator begin() { return ptr; }
	iterator end() { return ptr + count; }
	const_iterator begin() const { return ptr; }
	const_iterator end() const { return ptr + count; }

	// Discard contents and drop any file mapping
	void Reset() {
		keeper.reset();
		storage.clear();
		ptr = nullptr; count = 0;
	}
	void clear() { Reset(); }
	void resize(const size_t n) { EnsureHeap(); storage.resize(n); Sync(); }
	void reserve(const size_t n) { EnsureHeap(); storage.reserve(n); }
	void shrink_to_fit() { EnsureHeap(); storage.shrink_to_fit(); Sync(); }
	// vector interop used by image-map staging call sites
	SpillableArray(std::vector<T> &&v) : storage(std::move(v)) { Sync(); }
	SpillableArray &operator=(std::vector<T> v) {
		keeper.reset();
		storage = std::move(v);
		Sync();
		return *this;
	}
	void Assign(const std::vector<T> &v) { EnsureHeap(); storage = v; Sync(); }
	void push_back(const T &v) { EnsureHeap(); storage.push_back(v); Sync(); }
	void push_back(T &&v) { EnsureHeap(); storage.push_back(std::move(v)); Sync(); }
	template<class... Args> T &emplace_back(Args &&...args) {
		EnsureHeap();
		storage.emplace_back(std::forward<Args>(args)...);
		Sync();
		return back();
	}
	template<class It> void Append(It first, It last) {
		EnsureHeap(); storage.insert(storage.end(), first, last); Sync();
	}
	template<class It> iterator insert(iterator pos, It first, It last) {
		const size_t off = pos ? size_t(pos - ptr) : 0;
		EnsureHeap();
		const auto it = storage.insert(storage.begin() + off, first, last);
		Sync();
		return data() + (it - storage.begin());
	}

	// Writes contents to `fileName` and swaps the heap storage for a
	// copy-on-write file mapping. Returns the spilled byte count, 0 on
	// failure or when already spilled.
	size_t Spill(const std::string &fileName) {
		if (keeper || count == 0)
			return 0;
		std::shared_ptr<void> k = SpillToFile(ptr, count * sizeof(T), fileName);
		if (!k)
			return 0;
		T *mp = static_cast<T *>(k.get());
		keeper = std::move(k);
		std::vector<T>().swap(storage);
		ptr = mp;
		return count * sizeof(T);
	}
	bool Spilled() const { return (bool)keeper; }

private:
	// Pull the contents back into heap storage when a mutating call hits
	// a spilled array. Move-only element types are relocated with move
	// semantics (the mapping is released right after, so the moved-from
	// source objects are never observed again).
	void EnsureHeap() {
		if (!keeper)
			return;
		if constexpr (std::is_copy_constructible_v<T>) {
			storage.assign(ptr, ptr + count);
		} else {
			storage.reserve(count);
			for (size_t i = 0; i < count; ++i)
				storage.emplace_back(std::move(ptr[i]));
		}
		keeper.reset();
	}
	void Sync() {
		ptr = storage.empty() ? nullptr : storage.data();
		count = storage.size();
	}

	std::vector<T> storage;
	std::shared_ptr<void> keeper;
	T *ptr = nullptr;
	size_t count = 0;
};

}  // namespace luxrays

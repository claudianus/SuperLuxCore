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

#include "luxrays/core/geometry/point.h"
#include "luxrays/core/geometry/triangle.h"
#include "luxrays/utils/buffer.h"

#include <span>

using namespace luxrays;

//------------------------------------------------------------------------------
// Buffer
//------------------------------------------------------------------------------


// Constructors
template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD>::Buffer(size_t size) {
	Allocate(size);
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD>::Buffer(std::span<TYPE const> objs) {
	Allocate(objs.size());
	std::copy(objs.begin(), objs.end(), asType.begin());
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD>::Buffer(std::span<SUBTYPE const> subobjs) {
	Allocate(subobjs.size() * sizeof(SUBTYPE) / sizeof(TYPE) );
	std::copy(subobjs.begin(), subobjs.end(), asSubType.begin());
}

// Adopt externally owned memory (no copy, no trailing pad)
template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD> Buffer<TYPE, SUBTYPE, PAD>::Adopt(
		void *ptr, std::size_t byteSize, std::shared_ptr<void> keeper) {
	assert(byteSize % sizeof(TYPE) == 0);

	Buffer b;
	b.effectiveSize = byteSize;
	b.totalSize = byteSize;
	b.external = true;
	// Aliasing constructor: the buffer shares keeper's ownership while
	// get() points at the foreign memory.
	b.data = std::shared_ptr<std::byte[]>(std::move(keeper),
			reinterpret_cast<std::byte*>(ptr));
	b.asType = std::span<TYPE>(reinterpret_cast<TYPE*>(ptr),
			byteSize / sizeof(TYPE));
	b.asSubType = std::span<SUBTYPE>(reinterpret_cast<SUBTYPE*>(ptr),
			byteSize / sizeof(SUBTYPE));
	return b;
}

// Allocator
template< typename TYPE, typename SUBTYPE, std::array PAD >
void Buffer<TYPE, SUBTYPE, PAD>::Allocate(size_t count) {

	// Compute sizes
	effectiveSize = sizeof(TYPE) * count;
	totalSize = effectiveSize + padSize;

	// Allocate buffer
	data = std::make_shared<std::byte[]>(totalSize);
	external = false;

	// Add padding
	// Embree requires a padding field at the end
	// This is a trick so I can check if the buffer has been really allocated
	// with AllocVerticesBuffer() or not. It is useful for debugging LuxCore
	// applications.
	std::copy(pad.begin(), pad.end(), data.get() + effectiveSize);

	// Compute spans
	auto makeSpan = [&]<typename T>() {
		size_t size = effectiveSize / sizeof(T);
		auto * ptr = reinterpret_cast<T*>(data.get());
		return std::span<T>(ptr, size);
	};

	asType = makeSpan.template operator()<TYPE>();
	asSubType = makeSpan.template operator()<SUBTYPE>();

}


// Getters
template< typename TYPE, typename SUBTYPE, std::array PAD >
std::span<TYPE> Buffer<TYPE, SUBTYPE, PAD>::GetObjects() const {
	// Compute size (without padding)
	size_t size = effectiveSize / sizeof(TYPE);

	// Make span
	auto * ptr = reinterpret_cast<TYPE*>(data.get());
	return std::span<TYPE>(ptr, size);
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
std::span<SUBTYPE> Buffer<TYPE, SUBTYPE, PAD>::GetSubObjects() const {
	// Compute size (without padding)
	size_t size = effectiveSize / sizeof(SUBTYPE);

	// Make span
	auto * ptr = reinterpret_cast<SUBTYPE*>(data.get());
	return std::span<SUBTYPE>(ptr, size);
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
std::span<std::byte> Buffer<TYPE, SUBTYPE, PAD>::GetBytes(bool withPad) const {
	return std::span<std::byte>(data.get(), withPad ? totalSize : effectiveSize);
}


template< typename TYPE, typename SUBTYPE, std::array PAD >
std::span<const std::byte> Buffer<TYPE, SUBTYPE, PAD>::GetPad() const {
	// Adopted memory has no trailing pad in the allocation itself
	if (external)
		return std::span<const std::byte>(pad);
	return GetBytes(true).subspan(effectiveSize);
}


// Subset
template< typename TYPE, typename SUBTYPE, std::array PAD >
std::span<TYPE> Buffer<TYPE, SUBTYPE, PAD>::Subset(std::size_t offset, std::size_t count) {
	return asType.subspan(offset, count);
}

// Set: Copy 'from' into 'this'
// Realloc if needed
template< typename TYPE, typename SUBTYPE, std::array PAD >
void Buffer<TYPE, SUBTYPE, PAD>::Set(const Buffer<TYPE, SUBTYPE, PAD>& from) {
	// Check memory and adapt if necessary
	if (effectiveSize != from.effectiveSize) {
		Allocate(from.effectiveSize);
	}

	// Copy
	const auto from_bytes = std::span<std::byte>(from.data.get(), from.totalSize);
	auto to_bytes = std::span<std::byte>(data.get(), totalSize);

	std::copy(from_bytes.begin(), from_bytes.end(), to_bytes.begin());
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
void Buffer<TYPE, SUBTYPE, PAD>::Set(std::span<const TYPE> from) {
	// Check memory and adapt if necessary
	const size_t from_count = from.size();
	if (Count() != from_count) {
		Allocate(from_count);
	}

	// Copy
	std::copy(from.begin(), from.end(), asType.begin());
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
void Buffer<TYPE, SUBTYPE, PAD>::Set(std::span<const SUBTYPE> from) {
	// Check memory and adapt if necessary
	const size_t from_size = from.size() * sizeof(SUBTYPE);  // from_size in bytes
	assert(from_size % sizeof(TYPE) == 0);
	if (effectiveSize != from_size) {
		const size_t count = from_size / sizeof(TYPE);
		Allocate(count);
	}

	// Copy
	std::copy(from.begin(), from.end(), asSubType.begin());
}


// Indexation
template< typename TYPE, typename SUBTYPE, std::array PAD >
TYPE& Buffer<TYPE, SUBTYPE, PAD>::operator[](size_t index) {
        return asType[index];
}

template< typename TYPE, typename SUBTYPE, std::array PAD >
const TYPE& Buffer<TYPE, SUBTYPE, PAD>::operator[](size_t index) const {
        return asType[index];
}

// Implicit conversion operator
template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD>::operator std::span<TYPE>() const {
        return asType;
}

// Element count
template< typename TYPE, typename SUBTYPE, std::array PAD >
size_t Buffer<TYPE, SUBTYPE, PAD>::Count() const {
	return asType.size();
}

// Underlying structure
template< typename TYPE, typename SUBTYPE, std::array PAD >
void * Buffer<TYPE, SUBTYPE, PAD>::Data() const {
	return data.get();
}

// Emptiness
template< typename TYPE, typename SUBTYPE, std::array PAD >
Buffer<TYPE, SUBTYPE, PAD>::operator bool() const noexcept {
	return not asType.empty();
}


// Instanciations
template class luxrays::Buffer<luxrays::Point, float, VERTEXPAD>;
template class luxrays::Buffer<luxrays::Triangle, Triangle::subtype_t, NOPAD>;
template class luxrays::Buffer<luxrays::Normal, float, NOPAD>;

// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

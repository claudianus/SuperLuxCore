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


#include <array>
#include <span>
#include <bit>
#include <memory>
#include <string>

namespace luxrays {


inline constexpr auto NOPAD = std::array<std::byte,0>();

// A container for mesh components: points, normals etc.
// Can be end-padded, for the sake of embree or integrity check
//
// The container can be accessed via 3 levels:
// - Main objects, known as TYPE: points, triangles etc.
// - Underlying objects of main objects, known as SUBTYPE: float, size_t etc.
// - bytes 
//
// To spare compile time, the template is delibaretely intended not to
// be implicitely instantiable.
// Definition and instantiations are in cpp file
//
// As a convention:
// "size" is in bytes
// "count" is in TYPE elements
// "subcount" is in SUBTYPE elements
template< typename TYPE, typename SUBTYPE, std::array PAD=NOPAD >
class Buffer {

public:

	// Constructors
	inline Buffer() = default;
	explicit Buffer(std::size_t);
	explicit Buffer(std::span<const TYPE>);
	explicit Buffer(std::span<const SUBTYPE>);

	// Wrap externally owned memory without copying it. `keeper` is a
	// shared owner held by the buffer for its whole lifetime (e.g. a
	// reference to the Python array owning the memory): it is released
	// when the buffer is freed or reallocated. External memory carries
	// no trailing pad, so GetPad() returns the compile-time pad value.
	static Buffer Adopt(void *ptr, std::size_t byteSize,
			std::shared_ptr<void> keeper);

	// Spill the buffer contents to `fileName` and swap the storage for a
	// copy-on-write file mapping: the pages become file-backed so the
	// kernel can evict them under memory pressure and page them back on
	// demand (out-of-core geometry). Works on both owned and adopted
	// buffers — on adopted ones the external keeper (e.g. the source
	// Python array) is released. Returns false on failure, leaving the
	// buffer untouched.
	bool SpillToFile(const std::string &fileName);

	// Move is ok
	inline Buffer(Buffer&&) = default;
	inline Buffer& operator=(Buffer&&) = default;

	// No copy allowed
	Buffer(Buffer&) = delete;
	Buffer& operator=(Buffer&) = delete;

	// Allocate internal container for count objects of type TYPE
	void Allocate(std::size_t count);

	// Getters
	std::span<TYPE> GetObjects() const;
	std::span<SUBTYPE> GetSubObjects() const;
	std::span<std::byte> GetBytes(bool withPad=false) const;

	// Get pad value
	// Pad is directly read in buffer, so as it allows to check integrity
	std::span<const std::byte> GetPad() const;

	// Setters
	void Set(const Buffer<TYPE, SUBTYPE, PAD>& from);
	void Set(std::span<const TYPE> from);
	void Set(std::span<const SUBTYPE> from);

	// Subset
	std::span<TYPE> Subset(std::size_t offset, std::size_t count = std::dynamic_extent);

	// Indexation
	TYPE& operator[](size_t index);
	const TYPE& operator[](size_t index) const;

	// Implicit conversion operator
	operator std::span<TYPE>() const;

	// Element count (in TYPE elements)
	size_t Count() const;

	// Underlying structure (const)
	void * Data() const;

	// Emptiness
	explicit operator bool() const noexcept;

	// True when the storage is an adopted external mapping (e.g. an
	// .lxm section or a spill file) rather than owned heap memory.
	bool IsExternal() const noexcept { return external; }


private:
	// Underlying storage. shared_ptr so adopted external memory can
	// carry its keeper in the control block (aliasing constructor).
	std::shared_ptr<std::byte[]> data;
	static constexpr std::array pad{PAD};
	// True when `data` wraps memory owned elsewhere: no trailing pad
	// exists in the allocation.
	bool external = false;

	// Sizes (in bytes)
	size_t totalSize = 0;
	size_t effectiveSize = 0;
	static constexpr size_t padSize = std::size(PAD);

	// Spans
	std::span<TYPE> asType;
	std::span<SUBTYPE> asSubType;

};


// To compute padding for Buffer
template <typename T>
constexpr std::array<std::byte, sizeof(T)> to_bytes(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "to_bytes requires a trivially copyable type");
    return std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
}
inline constexpr auto VERTEXPAD = to_bytes(1234.1234f);

// Containers for points and triangles
class Point;
using VertexBuffer = Buffer<Point, float, VERTEXPAD>;
class Triangle;
using TriangleBuffer = Buffer<Triangle, unsigned int, NOPAD>;
class Normal;
using NormalBuffer = Buffer<Normal, float, NOPAD>;



}  // Namespace luxrays

// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

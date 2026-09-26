/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// MurmurHash3 (32-bit) + Cryptomatte float-id conversion.
// Public-domain reference algorithm (Austin Appleby), no external deps.

#ifndef _LUXRAYS_MURMURHASH_H
#define	_LUXRAYS_MURMURHASH_H

#include <cstdint>
#include <cstring>
#include <string>

namespace luxrays {

inline uint32_t MurmurHash3_32(const void *key, const size_t len,
		const uint32_t seed = 0) {
	const uint8_t *data = static_cast<const uint8_t *>(key);
	const int nblocks = (int)(len / 4);
	uint32_t h = seed;

	const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
	for (int i = 0; i < nblocks; ++i) {
		uint32_t k;
		memcpy(&k, data + i * 4, 4);	// endian-safe block load
		k *= c1; k = (k << 15) | (k >> 17); k *= c2;
		h ^= k; h = (h << 13) | (h >> 19); h = h * 5 + 0xe6546b64u;
	}
	uint32_t k = 0;
	const uint8_t *tail = data + nblocks * 4;
	switch (len & 3u) {
		case 3: k ^= (uint32_t)tail[2] << 16; // fall through
		case 2: k ^= (uint32_t)tail[1] << 8;  // fall through
		case 1: k ^= tail[0];
			k *= c1; k = (k << 15) | (k >> 17); k *= c2; h ^= k;
	}
	h ^= (uint32_t)len;
	h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13;
	h *= 0xc2b2ae35u; h ^= h >> 16;
	return h;
}

inline uint32_t MurmurHash3_32(const std::string &s, const uint32_t seed = 0) {
	return MurmurHash3_32(s.data(), s.size(), seed);
}

// Cryptomatte id: murmur3 -> float32 per the spec's hash_to_float.
// Bit-for-bit copy of all 32 bits (sign included); only the exponent
// is clamped away from 0 (subnormal) and 255 (NaN/Inf) by toggling
// its lowest bit. Ids may therefore be negative.
inline float CryptoHashToFloat(uint32_t hash) {
	const uint32_t exponent = (hash >> 23) & 0xffu;
	if ((exponent == 0u) || (exponent == 255u))
		hash ^= (1u << 23);	// 0 -> 1, 255 -> 254
	float f;
	memcpy(&f, &hash, 4);
	return f;
}

inline float CryptoNameToID(const std::string &name) {
	return CryptoHashToFloat(MurmurHash3_32(name));
}

// Manifest value format: id float bits as 8-hex.
inline std::string CryptoIDToHex(const float id) {
	uint32_t bits;
	memcpy(&bits, &id, 4);
	char buf[9];
	snprintf(buf, sizeof(buf), "%08x", bits);
	return buf;
}

// Cryptomatte manifest key: first 7 hex chars of the layer name's id
// float bits (the spec's "first 7 chars of the hashed type name",
// matching the reference layer_hash convention).
inline std::string CryptoManifestKey(const std::string &name) {
	return CryptoIDToHex(CryptoNameToID(name)).substr(0, 7);
}

} // namespace luxrays

#endif /* _LUXRAYS_MURMURHASH_H */

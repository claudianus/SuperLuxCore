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

// boost::serialization support for SpillableArray<T>. The wire format is
// byte-identical to std::vector<T> (collections_save_imp /
// collections_load_imp + the use_array_optimization fast path), so
// archives written by older LuxCore versions load unchanged and archives
// written here still load as vectors in older binaries.

#include <boost/serialization/collection_size_type.hpp>
#include <boost/serialization/item_version_type.hpp>
#include <boost/serialization/collections_save_imp.hpp>
#include <boost/serialization/detail/stack_constructor.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/array_wrapper.hpp>
#include <boost/serialization/array_optimization.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/library_version_type.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/mpl/bool.hpp>

#include "luxrays/utils/spillablearray.h"

namespace boost {
namespace serialization {

// Array-optimization path (bitwise-serializable element types)
template<class Archive, class T>
inline void save(Archive &ar, const luxrays::SpillableArray<T> &t,
		const unsigned int, mpl::true_) {
	const collection_size_type count(t.size());
	ar << BOOST_SERIALIZATION_NVP(count);
	if (count > 0)
		ar << serialization::make_array<const T, collection_size_type>(
				t.data(), count);
}

template<class Archive, class T>
inline void load(Archive &ar, luxrays::SpillableArray<T> &t,
		const unsigned int, mpl::true_) {
	collection_size_type count;
	ar >> BOOST_SERIALIZATION_NVP(count);
	item_version_type item_version(0);
	if (BOOST_SERIALIZATION_VECTOR_VERSIONED(ar.get_library_version()))
		ar >> BOOST_SERIALIZATION_NVP(item_version);
	t.resize(count);
	if (count > 0)
		ar >> serialization::make_array<T, collection_size_type>(
				t.data(), count);
}

// Collection path (class element types)
template<class Archive, class T>
inline void save(Archive &ar, const luxrays::SpillableArray<T> &t,
		const unsigned int, mpl::false_) {
	const collection_size_type count(t.size());
	ar << BOOST_SERIALIZATION_NVP(count);
	const item_version_type item_version(version<T>::value);
	ar << BOOST_SERIALIZATION_NVP(item_version);
	for (auto it = t.begin(); it != t.end(); ++it) {
		boost::serialization::save_construct_data_adl(
				ar, boost::addressof(*it), item_version);
		ar << boost::serialization::make_nvp("item", *it);
	}
}

template<class Archive, class T>
inline void load(Archive &ar, luxrays::SpillableArray<T> &t,
		const unsigned int, mpl::false_) {
	const library_version_type library_version(ar.get_library_version());
	item_version_type item_version(0);
	collection_size_type count;
	ar >> BOOST_SERIALIZATION_NVP(count);
	if (library_version_type(3) < library_version)
		ar >> BOOST_SERIALIZATION_NVP(item_version);
	t.clear();
	t.reserve(count);
	while (count-- > 0) {
		detail::stack_construct<Archive, T> u(ar, item_version);
		ar >> boost::serialization::make_nvp("item", u.reference());
		t.push_back(boost::move(u.reference()));
		ar.reset_object_address(&t.back(), u.address());
	}
}

template<class Archive, class T>
inline void save(Archive &ar, const luxrays::SpillableArray<T> &t,
		const unsigned int file_version) {
	typedef typename
		boost::serialization::use_array_optimization<Archive>::template apply<
			typename remove_const<T>::type
		>::type use_optimized;
	save(ar, t, file_version, use_optimized());
}

template<class Archive, class T>
inline void load(Archive &ar, luxrays::SpillableArray<T> &t,
		const unsigned int file_version) {
	typedef typename
		boost::serialization::use_array_optimization<Archive>::template apply<
			typename remove_const<T>::type
		>::type use_optimized;
	load(ar, t, file_version, use_optimized());
}

template<class Archive, class T>
inline void serialize(Archive &ar, luxrays::SpillableArray<T> &t,
		const unsigned int file_version) {
	boost::serialization::split_free(ar, t, file_version);
}

} // namespace serialization
} // namespace boost

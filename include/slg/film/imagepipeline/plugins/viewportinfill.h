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

#ifndef _SLG_VIEWPORTINFILL_PLUGIN_H
#define	_SLG_VIEWPORTINFILL_PLUGIN_H

#include <boost/serialization/version.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>

#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// ViewportInfillPlugin reconstructs display pixels that have no accumulated
// samples yet (weight == 0) from the neighbourhood of covered
// pixels via a pull-push pyramid, so a sparse pass produces a coherent
// image that dissolves in instead of leaving black/stale holes. Only the
// IMAGEPIPELINE display buffer is touched - the film's own accumulation
// channels stay honest and filled pixels are replaced the moment real
// samples land.
//
// ltBlend additionally softens light-tracing-only pixels: splats that
// landed without any eye-path coverage show up as isolated speckles in
// early passes, so they are blended toward the surrounding filled colour.
//------------------------------------------------------------------------------

class ViewportInfillPlugin : public ImagePipelinePlugin {
public:
	ViewportInfillPlugin(const float ltBlend = 0.5f);
	virtual ~ViewportInfillPlugin();

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	friend class boost::serialization::access;

private:
	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
		if (version >= 2)
			ar & ltBlend;
		else
			ltBlend = 0.5f;
	}

	float ltBlend;
};

}

BOOST_CLASS_VERSION(slg::ViewportInfillPlugin, 2)

BOOST_CLASS_EXPORT_KEY(slg::ViewportInfillPlugin)

#endif /* _SLG_VIEWPORTINFILL_PLUGIN_H */

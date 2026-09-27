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

#ifndef _SLG_VIEWPORTSMOOTH_PLUGIN_H
#define	_SLG_VIEWPORTSMOOTH_PLUGIN_H

#include <boost/serialization/version.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>

#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// ViewportSmoothPlugin: SVGF-lite edge-aware à-trous filtering for
// not-yet-converged pixels in interactive viewport rendering.
//
// Pixels whose accumulated weight is below `minSamps` are filtered with
// 3 à-trous iterations guided by the DEPTH and AVG_SHADING_NORMAL
// channels, so geometric edges survive while sparse-sample noise melts
// away. Converged pixels keep their raw samples; the film's own
// accumulation channels are never touched (display-side only, like
// VIEWPORT_INFILL). Runs on the linear HDR buffer before tonemapping.
//------------------------------------------------------------------------------

class ViewportSmoothPlugin : public ImagePipelinePlugin {
public:
	ViewportSmoothPlugin(const float minSamps = 8.f);
	virtual ~ViewportSmoothPlugin();

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	friend class boost::serialization::access;

private:
	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
		ar & minSamps;
	}

	float minSamps;
};

}

BOOST_CLASS_VERSION(slg::ViewportSmoothPlugin, 1)

BOOST_CLASS_EXPORT_KEY(slg::ViewportSmoothPlugin)

#endif /* _SLG_VIEWPORTSMOOTH_PLUGIN_H */

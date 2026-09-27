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

#ifndef _SLG_VIEWPORTTEMPORAL_PLUGIN_H
#define	_SLG_VIEWPORTTEMPORAL_PLUGIN_H

#include <vector>
#include <boost/serialization/version.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/vector.hpp>

#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// ViewportTemporalPlugin reuses the previous displayed frame across camera
// edits. The RenderSession publishes the current camera transforms into
// film metadata on every EndSceneEdit(); when the camera moved since the
// last snapshot, this plugin forward-warps the stored frame through the
// depth buffer (z-tested splat) and writes it into pixels that have no
// real samples yet. Real samples always win; disoccluded holes are left
// for the infill plugin. History lives in the plugin instance, so it
// survives the Film::Reset() that engines perform on scene edits.
//
// Ordering: must run AFTER VIEWPORT_INFILL (and optional smoothing) so
// the snapshot it stores is the dense, reconstructed display image.
//------------------------------------------------------------------------------

class ViewportTemporalPlugin : public ImagePipelinePlugin {
public:
	ViewportTemporalPlugin();
	virtual ~ViewportTemporalPlugin();

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	friend class boost::serialization::access;

private:
	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
	}

	// Previous displayed frame + per-pixel world positions (POSITION
	// channel snapshot). Positions are camera-independent, so no
	// history camera is stored: any stored point reprojects into
	// whatever camera the current frame uses
	std::vector<float> histRGB, histPos;
	u_int histW, histH;
	bool hasHistory;
};

}

BOOST_CLASS_VERSION(slg::ViewportTemporalPlugin, 1)

BOOST_CLASS_EXPORT_KEY(slg::ViewportTemporalPlugin)

#endif /* _SLG_VIEWPORTTEMPORAL_PLUGIN_H */

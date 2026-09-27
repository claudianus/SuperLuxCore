/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#ifndef _SLG_VIEWPORTINFILL_PLUGIN_H
#define	_SLG_VIEWPORTINFILL_PLUGIN_H

#include "luxrays/kernels/kernels.h"
#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// Viewport infill plugin
//
// Display-side reconstruction for progressive viewport rendering: pixels
// with zero accumulated sample weight are filled from the nearest covered
// pixels via a pull-push pyramid, so a sparse pass produces a coherent
// image that dissolves in instead of leaving black/stale holes. Only the
// IMAGEPIPELINE display buffer is touched - the film's own accumulation
// channels stay honest and filled pixels are replaced the moment real
// samples land.
//------------------------------------------------------------------------------

class ViewportInfillPlugin : public ImagePipelinePlugin {
public:
	ViewportInfillPlugin();
	virtual ~ViewportInfillPlugin();

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	friend class boost::serialization::access;

private:
	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
	}
};

}

BOOST_CLASS_VERSION(slg::ViewportInfillPlugin, 1)

BOOST_CLASS_EXPORT_KEY(slg::ViewportInfillPlugin)

#endif	/* _SLG_VIEWPORTINFILL_PLUGIN_H */

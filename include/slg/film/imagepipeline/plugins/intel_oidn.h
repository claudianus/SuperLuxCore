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

#ifndef _SLG_INTEL_OIDN_H
#define	_SLG_INTEL_OIDN_H

#if !defined(LUXCORE_DISABLE_OIDN)

#include <vector>
#include <string>

#include <boost/serialization/export.hpp>
#include <tbb/scalable_allocator.h>

//#include <OpenImageDenoise/oidn.hpp>

#include "luxrays/luxrays.h"
#include "luxrays/core/color/color.h"
#include "slg/film/film.h"
#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// Intel Open Image Denoise
//------------------------------------------------------------------------------

class IntelOIDN : public ImagePipelinePlugin {
public:
	IntelOIDN(const std::string filterType,
			const int oidnMemLimit, const float sharpness,
			bool enablePrefiltering,
			const std::string denoiseMode = "combined",
			const bool demodulate = true,
			const bool denoiseEmission = false,
			const float fireflySigma = 0.f);

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	// True when the per-component recipe is requested
	bool UsesComponents() const { return denoiseMode == "components"; }

	friend class boost::serialization::access;

private:
	// Used by serialization
	IntelOIDN();

	using float_buffer = std::vector<float, tbb::scalable_allocator<float>>;

	float_buffer PrepareBuffer (
		const std::string& imageName,
		const GenericFrameBuffer<4, 1, float>& channel,
		const u_int width,
		const u_int height,
		bool enablePrefiltering
	) const;
	void FilterImage (const std::string &imageName,
			const float *srcBuffer, float *dstBuffer,
			const float *albedoBuffer, const float *normalBuffer,
			const u_int width, const u_int height,
			const bool cleanAux) const;

	// Component-decomposed path: denoise each radiance component
	// separately and recombine (energy-exact residual passthrough)
	void ApplyComponents(Film &film, const u_int index);
	// Local outlier suppression before denoising (keeps fireflies from
	// smearing into the neighbourhood inside the network)
	void FireflyClamp(float *buf, const u_int width, const u_int height) const;

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
		ar & oidnMemLimit;
		ar & iTileCount;
		ar & jTileCount;
		ar & sharpness;
		ar & enablePrefiltering;
		if (version >= 5) {
			ar & denoiseMode;
			ar & demodulate;
			ar & denoiseEmission;
			ar & fireflySigma;
		}
	}

	std::string filterType;
	u_int iTileCount;
	u_int jTileCount;
	int oidnMemLimit; //needs to be signed int for OIDN call
	float sharpness;
	bool enablePrefiltering;
	// "combined" (classic single-buffer denoise) or "components"
	// (per-component denoise + exact recombination)
	std::string denoiseMode;
	// Albedo demodulation for the indirect diffuse component
	bool demodulate;
	// Denoise the emission component (default: passthrough, keeps
	// emitter silhouettes crisp)
	bool denoiseEmission;
	// Per-component firefly pre-clamp in units of local sigma (0 = off)
	float fireflySigma;
};

}


BOOST_CLASS_VERSION(slg::IntelOIDN, 5)

BOOST_CLASS_EXPORT_KEY(slg::IntelOIDN)

#endif
		
#endif /* _SLG_INTEL_OIDN_H */// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

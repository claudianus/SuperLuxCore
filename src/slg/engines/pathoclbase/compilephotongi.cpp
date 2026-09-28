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

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <iosfwd>
#include <limits>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "luxrays/core/bvh/bvhbuild.h"
#include "slg/materials/materialdefs.h"
#include "slg/engines/pathoclbase/compiledscene.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/kernels/kernels.h"
#include "slg/engines/pathtracer.h"

using namespace std;
using namespace luxrays;
using namespace slg;

void CompiledScene::CompilePhotonGI() {
	pgicRadiancePhotons.clear();
	pgicRadiancePhotons.shrink_to_fit();
	pgicRadiancePhotonsValues.clear();
	pgicRadiancePhotonsValues.shrink_to_fit();

	pgicRadiancePhotonsBVHArrayNode.clear();
	pgicRadiancePhotonsBVHArrayNode.shrink_to_fit();

	pgicCausticPhotons.clear();
	pgicCausticPhotons.shrink_to_fit();

	pgicCausticPhotonsBVHArrayNode.clear();
	pgicCausticPhotonsBVHArrayNode.shrink_to_fit();

	pgicCausticBeams.clear();
	pgicCausticBeams.shrink_to_fit();

	pgicCausticBeamsBVHArrayNode.clear();
	pgicCausticBeamsBVHArrayNode.shrink_to_fit();

	compiledPathTracer.pgic.indirectEnabled = false;
	compiledPathTracer.pgic.causticEnabled = false;
	compiledPathTracer.pgic.depositEnabled = false;
	compiledPathTracer.pgic.depositPhotonCapacity = 0;
	compiledPathTracer.pgic.depositBeamCapacity = 0;

	const PhotonGICache *photonGICache = pathTracer->GetPhotonGICache();
	if (!photonGICache)
		return;

	wasPhotonGICompiled = true;
	switch (photonGICache->GetDebugType()) {
		case PGIC_DEBUG_NONE:
			compiledPathTracer.pgic.debugType = slg::ocl::PGIC_DEBUG_NONE;
			break;
		case PGIC_DEBUG_SHOWINDIRECT:
			compiledPathTracer.pgic.debugType = slg::ocl::PGIC_DEBUG_SHOWINDIRECT;
			break;
		case PGIC_DEBUG_SHOWCAUSTIC:
			compiledPathTracer.pgic.debugType = slg::ocl::PGIC_DEBUG_SHOWCAUSTIC;
			break;
		case PGIC_DEBUG_SHOWINDIRECTPATHMIX:
			compiledPathTracer.pgic.debugType = slg::ocl::PGIC_DEBUG_SHOWINDIRECTPATHMIX;
			break;
		default:
			throw runtime_error("Unknown PhotonGI debug type in CompiledScene::CompilePhotonGI(): " + ToString(photonGICache->GetDebugType()));
	}

	compiledPathTracer.pgic.glossinessUsageThreshold = photonGICache->GetParams().glossinessUsageThreshold;
	compiledPathTracer.pgic.causticVolumeBeams = photonGICache->GetParams().caustic.volumeBeams;

	//--------------------------------------------------------------------------
	// Indirect cache
	//--------------------------------------------------------------------------

	if (photonGICache->IsIndirectEnabled()) {
		SLG_LOG("Compile PhotonGI indirect cache");
		compiledPathTracer.pgic.indirectEnabled = true;

		// Compile radiance photons

		const SpillableArray<RadiancePhoton> &radiancePhotons = photonGICache->GetRadiancePhotons();
		if (radiancePhotons.size() > 0) {
			pgicRadiancePhotons.resize(radiancePhotons.size());

			pgicLightGroupCounts = 1;
			for (auto const &p : radiancePhotons)
				pgicLightGroupCounts = Max(pgicLightGroupCounts, p.outgoingRadiance.Size());
			pgicRadiancePhotonsValues.resize(pgicRadiancePhotons.size() * pgicLightGroupCounts);
			for (auto &p : pgicRadiancePhotonsValues) {
				p.c[0] = 0.f;
				p.c[1] = 0.f;
				p.c[2] = 0.f;
			}

			for (u_int i = 0; i < radiancePhotons.size(); ++i) {
				const RadiancePhoton &radiancePhoton = radiancePhotons[i];
				slg::ocl::RadiancePhoton &oclRadiancePhoton = pgicRadiancePhotons[i];

				ASSIGN_VECTOR(oclRadiancePhoton.p, radiancePhoton.p);
				ASSIGN_NORMAL(oclRadiancePhoton.n, radiancePhoton.n);

				oclRadiancePhoton.outgoingRadianceIndex = i * pgicLightGroupCounts;
				const SpectrumGroup &outgoingRadiance = radiancePhoton.outgoingRadiance;			
				for (u_int j = 0; j < outgoingRadiance.Size(); ++j) {
					ASSIGN_SPECTRUM(pgicRadiancePhotonsValues[i * pgicLightGroupCounts + j], outgoingRadiance[j]);
				}

				oclRadiancePhoton.isVolume = radiancePhoton.isVolume;
			}

			// Compile radiance photons BVH

			const PGICRadiancePhotonBvh *radiancePhotonsBVH = photonGICache->GetRadiancePhotonsBVH();

			u_int nNodes;
			const luxrays::ocl::IndexBVHArrayNode *nodes = radiancePhotonsBVH->GetArrayNodes(&nNodes);
			pgicRadiancePhotonsBVHArrayNode.resize(nNodes);
			copy(&nodes[0], &nodes[0] + nNodes, pgicRadiancePhotonsBVHArrayNode.begin());

			compiledPathTracer.pgic.indirectLookUpRadius = radiancePhotonsBVH->GetEntryRadius();
			compiledPathTracer.pgic.indirectLookUpNormalCosAngle = radiancePhotonsBVH->GetEntryNormalCosAngle();
			compiledPathTracer.pgic.indirectUsageThresholdScale = photonGICache->GetParams().indirect.usageThresholdScale;
		}
	}

	//--------------------------------------------------------------------------
	// Caustic cache
	//--------------------------------------------------------------------------

	if (photonGICache->IsCausticEnabled()) {
		SLG_LOG("Compile PhotonGI caustic cache");
		compiledPathTracer.pgic.causticEnabled = true;

		// Compile caustic photons

		const SpillableArray<Photon> &causticPhotons = photonGICache->GetCausticPhotons();
		if (causticPhotons.size() > 0) {
			pgicCausticPhotons.resize(causticPhotons.size());
			for (u_int i = 0; i < causticPhotons.size(); ++i) {
				const Photon &photon = causticPhotons[i];
				slg::ocl::Photon &oclPhoton = pgicCausticPhotons[i];

				ASSIGN_VECTOR(oclPhoton.p, photon.p);
				ASSIGN_VECTOR(oclPhoton.d, photon.d);
				oclPhoton.lightID = photon.lightID;
				ASSIGN_SPECTRUM(oclPhoton.alpha, photon.alpha);
				ASSIGN_NORMAL(oclPhoton.landingSurfaceNormal, photon.landingSurfaceNormal);
				oclPhoton.isVolume = photon.isVolume;
			}

			// Compile caustic photons BVH

			const PGICPhotonBvh *causticPhotonsBVH = photonGICache->GetCausticPhotonsBVH();

			u_int nNodes;
			const luxrays::ocl::IndexBVHArrayNode *nodes = causticPhotonsBVH->GetArrayNodes(&nNodes);
			pgicCausticPhotonsBVHArrayNode.resize(nNodes);
			copy(&nodes[0], &nodes[0] + nNodes, pgicCausticPhotonsBVHArrayNode.begin());

			compiledPathTracer.pgic.causticPhotonTracedCount = photonGICache->GetCausticPhotonTracedCount();
			compiledPathTracer.pgic.causticLookUpRadius = causticPhotonsBVH->GetEntryRadius();
			compiledPathTracer.pgic.causticLookUpNormalCosAngle = causticPhotonsBVH->GetEntryNormalCosAngle();
		} else {
			// Beams can exist without a photon BVH (flights ending on delta
			// surfaces never make a point deposit): radius/count come from
			// the cache params so the beam kernel normalization stays sane.
			compiledPathTracer.pgic.causticPhotonTracedCount = photonGICache->GetCausticPhotonTracedCount();
			compiledPathTracer.pgic.causticLookUpRadius = photonGICache->GetParams().caustic.lookUpRadius;
		}

		// Compile caustic beams (in-medium specular flight segments)

		const std::vector<PhotonBeam> &causticBeams = photonGICache->GetCausticBeams();
		u_int nBeamNodes;
		const luxrays::ocl::IndexBVHArrayNode *beamNodes =
				photonGICache->GetCausticBeamsBVHArrayNodes(&nBeamNodes);
		if ((causticBeams.size() > 0) && beamNodes) {
			pgicCausticBeams.resize(causticBeams.size());
			for (u_int i = 0; i < causticBeams.size(); ++i) {
				const PhotonBeam &beam = causticBeams[i];
				slg::ocl::PhotonBeam &oclBeam = pgicCausticBeams[i];

				ASSIGN_VECTOR(oclBeam.p0, beam.p0);
				ASSIGN_VECTOR(oclBeam.d, beam.d);
				oclBeam.lightID = beam.lightID;
				ASSIGN_SPECTRUM(oclBeam.alpha, beam.alpha);
				oclBeam.length = beam.length;
				oclBeam.volumeIndex = scene.GetMaterials().GetMaterialIndex(beam.volume);
			}

			pgicCausticBeamsBVHArrayNode.resize(nBeamNodes);
			copy(&beamNodes[0], &beamNodes[0] + nBeamNodes, pgicCausticBeamsBVHArrayNode.begin());
		}
	}
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

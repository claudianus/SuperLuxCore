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
 *
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <cassert>
#include <boost/format.hpp>

#include "luxrays/utils/thread.h"

#include "slg/scene/scene.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/engines/renderengine.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/caches/photongi/tracephotonsthread.h"
#include "slg/utils/pathdepthinfo.h"
#include "slg/utils/pathinfo.h"
#include "slg/cameras/camera.h"
#include "slg/volumes/homogenous.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// TracePhotonsThread
//------------------------------------------------------------------------------

TracePhotonsThread::TracePhotonsThread(PhotonGICache &cache, const u_int index,
		const u_int seed, const u_int photonCount,
		const bool indirectCacheDone, const bool causticCacheDone,
		std::atomic<u_int> &gPhotonsCounter, std::atomic<u_int> &gIndirectPhotonsTraced,
		std::atomic<u_int> &gCausticPhotonsTraced, std::atomic<u_int> &gIndirectSize,
		std::atomic<u_int> &gCausticSize) :
	pgic(cache), threadIndex(index), seedBase(seed), photonTracedCount(photonCount),
	globalPhotonsCounter(gPhotonsCounter),
	globalIndirectPhotonsTraced(gIndirectPhotonsTraced),
	globalCausticPhotonsTraced(gCausticPhotonsTraced),
	globalIndirectSize(gIndirectSize),
	globalCausticSize(gCausticSize),
	renderThread(nullptr),
	indirectDone(indirectCacheDone), causticDone(causticCacheDone) {
}

TracePhotonsThread::~TracePhotonsThread() {
	Join();
}

void TracePhotonsThread::Start() {
	indirectPhotons.clear();
	causticPhotons.clear();
	causticBeams.clear();

	renderThread = std::make_unique<luxrays::JThread>(
		std::bind_front(&TracePhotonsThread::RenderFunc, this)
	);
	SetThreadName(renderThread, "LxTracePhotons");
}

void TracePhotonsThread::Join() {
	if (renderThread and renderThread->joinable()) {
		renderThread->join();
	}
}

void TracePhotonsThread::UniformMutate(RandomGenerator &rndGen, vector<float> &samples) const {
	for (auto &sample: samples)
		sample = rndGen.floatValue();
}

void TracePhotonsThread::Mutate(RandomGenerator &rndGen,
		const vector<float> &currentPathSamples,
		vector<float> &candidatePathSamples,
		const float mutationSize) const {
	assert (candidatePathSamples.size() == currentPathSamples.size());
	assert (mutationSize != 0.f);

	for (u_int i = 0; i < currentPathSamples.size(); ++i) {
		const float deltaU = powf(rndGen.floatValue(), 1.f / mutationSize + 1.f);

		float mutateValue = currentPathSamples[i];
		if (rndGen.floatValue() < .5f) {
			mutateValue += deltaU;
			mutateValue = (mutateValue < 1.f) ? mutateValue : (mutateValue - 1.f);
		} else {
			mutateValue -= deltaU;
			mutateValue = (mutateValue < 0.f) ? (mutateValue + 1.f) : mutateValue;
		}
		
		// mutateValue can still be 1.f due to numerical precision problems
		candidatePathSamples[i] = (mutateValue == 1.f) ? 0.f : mutateValue;

		assert ((candidatePathSamples[i] >= 0.f) && (candidatePathSamples[i] < 1.f));
	}
}

bool TracePhotonsThread::TracePhotonPath(RandomGenerator &rndGen,
		const vector<float> &samples,
		vector<RadiancePhotonEntry> &newIndirectPhotons,
		vector<Photon> &newCausticPhotons,
		vector<PhotonBeam> &newCausticBeams) {
	// Hard coded RR parameters
	const u_int rrDepth = 3;
	const float rrImportanceCap = .5f;

	newIndirectPhotons.clear();
	newCausticPhotons.clear();
	newCausticBeams.clear();
	std::vector<size_t> allNearEntryIndices;

	SceneConstRef scene = *pgic.scene;
	auto& camera = scene.GetCamera();

	bool usefulPath = false;

	Spectrum lightPathFlux;

	const float timeSample = samples[0];
	const float time = (pgic.params.photon.timeStart <= pgic.params.photon.timeEnd) ?
		Lerp(timeSample, pgic.params.photon.timeStart, pgic.params.photon.timeEnd) :
		camera.GenerateRayTime(timeSample);

	// Select one light source
	float lightPickPdf;
	auto light = scene.GetLightSources().GetEmitLightStrategy().
			SampleLights(scene, samples[1], &lightPickPdf);

	if (light) {
		// Initialize the light path
		float lightEmitPdfW;
		Ray nextEventRay;
		lightPathFlux = light->Emit(scene,
				time, samples[2], samples[3], samples[4], samples[5], samples[6],
				nextEventRay, lightEmitPdfW);

		if (!lightPathFlux.Black()) {
			lightPathFlux /= lightEmitPdfW * lightPickPdf;
			assert (lightPathFlux.IsValid());

			//------------------------------------------------------------------
			// Trace the light path
			//------------------------------------------------------------------

			// MPG-lite Phase B: the last strict-delta vertex of the walk is
			// tracked so a caustic deposit can inject it into the MNEE seed
			// cache - the light sub-path already solved (by construction) the
			// specular manifold an eye-side connection would have to Newton-
			// iterate for. Seeds only pick the solve basin, so an occasional
			// stale or colliding record can cost iterations, never bias.
			MneeSeedRecord specSeed;
			bool specSeedValid = false;

			LightPathInfo pathInfo;
			for (;;) {
				const u_int sampleOffset = sampleBootSize +	pathInfo.depth.depth * sampleStepSize;

				RayHit nextEventRayHit;
				BSDF bsdf;
				Spectrum connectionThroughput;
				// The volume this segment flies through (the march can mutate
				// pathInfo.volume at boundary crossings) and the packet flux
				// at segment start: caustic beams spread flux along the flight.
				// Same fallback as Scene::Intersect(): a null current volume
				// means the default world volume.
				const VolumeConstPtr flightVolume = pathInfo.volume.HasCurrentVolume() ?
						VolumeConstPtr(std::addressof(pathInfo.volume.GetCurrentVolume())) :
						(scene.HasDefaultWorldVolume() ?
								VolumeConstPtr(std::addressof(scene.GetDefaultWorldVolume())) : nullptr);
				const Spectrum beamFlux = lightPathFlux;
				const bool hit = scene.Intersect(nullptr, LIGHT_RAY | GENERIC_RAY, &pathInfo.volume, samples[sampleOffset],
						&nextEventRay, &nextEventRayHit, &bsdf,
						&connectionThroughput, nullptr, nullptr, false,
						&pathInfo.depth, pathInfo.lastBSDFEvent);

				if (hit) {
					// Something was hit

					// Record the in-medium flight segment itself as a caustic
					// beam for homogeneous volumes: a thin focused shaft is
					// dense along the line even where point deposits are
					// sparse. The deposit depends on the flight, not on the
					// endpoint vertex class (a shaft can end on a delta
					// surface and still be visible to eye paths). Point
					// photons keep depositing for GPU queries and
					// non-homogeneous media, where beams do not apply.
					if (pgic.params.caustic.volumeBeams && flightVolume &&
							(flightVolume->GetType() == HOMOGENEOUS_VOL) &&
							beamFlux.IsValid() &&
							pathInfo.IsSpecularPath() && pathInfo.firstVertexSeen &&
							!causticDone) {
						// Long flights are chunked so their segment AABBs stay
						// tight against the query box. Each chunk holds its
						// length share of the packet flux pre-attenuated to
						// its start, keeping the estimator identical to the
						// unsplit segment (with piecewise transmittance).
						const float segLen = Distance(nextEventRay.o, bsdf.hitPoint.p);
						if (segLen > DEFAULT_EPSILON_STATIC) {
							const Vector segDir = (bsdf.hitPoint.p - nextEventRay.o) / segLen;
							const float chunkLen = Max(8.f * pgic.params.caustic.lookUpRadius,
									segLen / 16.f);
							const u_int nChunks = (u_int)ceilf(segLen / chunkLen);
							for (u_int i = 0; i < nChunks; ++i) {
								const float s0 = i * chunkLen;
								const float s1 = Min((i + 1) * chunkLen, segLen);
								const Point a = nextEventRay.o + s0 * segDir;
								const Point b = nextEventRay.o + s1 * segDir;
								Spectrum f = beamFlux * ((s1 - s0) / segLen);
								if (s0 > 0.f)
									f *= flightVolume->TransmittanceEstimate(
											Ray(nextEventRay.o, segDir, 0.f, s0), .5f);
								newCausticBeams.push_back(PhotonBeam(a, b,
										light->GetID(), f, flightVolume));
							}
							usefulPath = true;
						}
					}

					lightPathFlux *= connectionThroughput;

					//----------------------------------------------------------
					// Deposit photons only on diffuse surfaces
					//----------------------------------------------------------

					if (pgic.IsPhotonGIEnabled(bsdf) &&
							// This is an anti-NaNs/Infs safety net to avoid poisoning
							// the caches
							lightPathFlux.IsValid()) {
						// Flip the normal if required
						const Normal landingSurfaceNormal = ((Dot(bsdf.hitPoint.geometryN, -nextEventRay.d) > 0.f) ?
							1.f : -1.f) * bsdf.hitPoint.geometryN;

						// Check if the point is visible (the kd-tree can be
						// absent in pure-medium scenes where beams carry the
						// whole caustic cache, or in frustum-culled
						// caustic-only mode where no visibility pass ran)
						allNearEntryIndices.clear();
						if (pgic.visibilityParticlesKdTree) {
							pgic.visibilityParticlesKdTree->GetAllNearEntries(
									allNearEntryIndices,
									bsdf.hitPoint.p, landingSurfaceNormal, bsdf.IsVolume(),
									pgic.params.visibility.lookUpRadius2,
									pgic.params.visibility.lookUpNormalCosAngle);
						}

						bool causticReceiver = allNearEntryIndices.size() > 0;
						if (!causticReceiver && pgic.UseFrustumCulling()) {
							// No visibility pass: keep photons that project
							// into the film frame (RTPM photon culling). A
							// 10% border absorbs lookup-radius spill at the
							// frame edges.
							float filmX, filmY;
							if (camera.ProjectPointToFilm(bsdf.hitPoint.p, time,
									&filmX, &filmY)) {
								const float padX = .1f * camera.filmWidth;
								const float padY = .1f * camera.filmHeight;
								causticReceiver =
										(filmX >= -padX) && (filmX < camera.filmWidth + padX) &&
										(filmY >= -padY) && (filmY < camera.filmHeight + padY);
							}
						}

						{
							// Media-transparent chains: isNearlyS survives
							// medium scatter vertices, so multi-scatter
							// deposits in volumes land here. firstVertexSeen
							// keeps pure-medium prefixes (ambient
							// in-scattering, no focusing surface) out.
							if ((pathInfo.depth.depth > 0) && pathInfo.IsSpecularPath() &&
									pathInfo.firstVertexSeen && causticReceiver && !causticDone) {
								// It is a caustic photon
								newCausticPhotons.push_back(Photon(bsdf.hitPoint.p, nextEventRay.d,
										light->GetID(), lightPathFlux, landingSurfaceNormal, bsdf.IsVolume()));

								// The last delta vertex of this walk sits on the
								// specular manifold the MNEE solver needs: queue it
								// as a warm-start seed for eye paths connecting the
								// same (light, occluder cell) pair. Keyed by the
								// occluder-side hit position like the eye-side
								// entries; fluxWeight carries the photon energy for
								// the retention policy.
								if (specSeedValid) {
									specSeed.fluxWeight = lightPathFlux.Y();
									mneeSeedRecords.push_back(specSeed);
								}

								usefulPath = true;
							}

							if (!indirectDone && (allNearEntryIndices.size() > 0)) {
								// It is an indirect photon

								// Add outgoingRadiance to each near visible entry 
								for (auto const &vpIndex : allNearEntryIndices)
									newIndirectPhotons.push_back(RadiancePhotonEntry(vpIndex,
											light->GetID(), lightPathFlux));

								usefulPath = true;
							}
						}
					}

					if (pathInfo.depth.depth + 1 >= pgic.params.photon.maxPathDepth)
						break;

					//----------------------------------------------------------
					// Build the next vertex path ray
					//----------------------------------------------------------

					float bsdfPdf;
					Vector sampledDir;
					BSDFEvent bsdfEvent;
					float cosSampleDir;
					const Spectrum bsdfSample = bsdf.Sample(&sampledDir,
							samples[sampleOffset + 2],
							samples[sampleOffset + 3],
							&bsdfPdf, &cosSampleDir, &bsdfEvent);
					if (bsdfSample.Black())
						break;

					pathInfo.AddVertex(bsdf, bsdfEvent, pgic.params.glossinessUsageThreshold);

					// Snapshot the vertex when it is a strict-delta event on an
					// unbroken specular chain: it becomes the seed the next
					// caustic deposit injects. The side bit mirrors the
					// eye-side convention (Dot(connectDir, geometryN) > 0):
					// the eye connection ray traverses the interface opposite
					// to the photon, so the photon-side sign is flipped.
					if ((bsdfEvent & SPECULAR) && pathInfo.IsSpecularPath()) {
						specSeed.p = bsdf.hitPoint.p;
						specSeed.n = bsdf.hitPoint.geometryN;
						specSeed.lightIndex = (u_int)(uintptr_t)light.get();
						specSeed.meshIndex = nextEventRayHit.meshIndex * 2u +
								(Dot(nextEventRay.d, bsdf.hitPoint.geometryN) < 0.f ? 1u : 0u);
						specSeed.mirrorMode =
								(bsdf.GetMaterial()->GetType() == MIRROR) ? 1u : 0u;
						specSeed.key = MneeSeedKey(specSeed.lightIndex,
								specSeed.meshIndex, specSeed.p,
								Max(scene.GetDataSet().GetBSphere().rad /
										MNEE_SEED_CELL_FRAC_CPU, 1e-4f));
						specSeedValid = true;
					}

					// If I have to fill only the caustic cache and last BSDF event
					// is not a (nearly) specular one, I can stop with this path
					if (indirectDone && !causticDone && !pathInfo.IsSpecularPath())
						break;

					// Russian Roulette
					if (pathInfo.UseRR(rrDepth)) {
						const float rrProb = RenderEngine::RussianRouletteProb(bsdfSample, rrImportanceCap);
						if (rrProb < samples[sampleOffset + 4])
							break;

						// Increase path contribution
						lightPathFlux /= rrProb;
					}
					
					lightPathFlux *= bsdfSample;
					assert (lightPathFlux.IsValid());

					nextEventRay.Update(bsdf.GetRayOrigin(sampledDir), sampledDir);
				} else {
					// Ray lost in space...
					break;
				}
			}
		}
	}

	return usefulPath;
}

void TracePhotonsThread::AddPhotons(const vector<RadiancePhotonEntry> &newIndirectPhotons,
		const vector<Photon> &newCausticPhotons,
		const vector<PhotonBeam> &newCausticBeams) {
	indirectPhotons.insert(indirectPhotons.end(), newIndirectPhotons.begin(),
			newIndirectPhotons.end());
	causticPhotons.insert(causticPhotons.end(), newCausticPhotons.begin(),
			newCausticPhotons.end());
	causticBeams.insert(causticBeams.end(), newCausticBeams.begin(),
			newCausticBeams.end());
}

void TracePhotonsThread::AddPhotons(const float currentPhotonsScale,
		const vector<RadiancePhotonEntry> &newIndirectPhotons,
		const vector<Photon> &newCausticPhotons,
		const vector<PhotonBeam> &newCausticBeams) {
	for (auto const &photon : newIndirectPhotons) {
		indirectPhotons.push_back(photon);
		indirectPhotons.back().alpha *= currentPhotonsScale;
	}
	
	for (auto const &photon : newCausticPhotons) {
		causticPhotons.push_back(photon);
		causticPhotons.back().alpha *= currentPhotonsScale;
	}

	for (auto const &beam : newCausticBeams) {
		causticBeams.push_back(beam);
		causticBeams.back().alpha *= currentPhotonsScale;
	}
}

// The metropolis sampler used here is based on:
//  "Robust Adaptive Photon Tracing using Photon Path Visibility"
//  by TOSHIYA HACHISUKA and HENRIK WANN JENSEN

void TracePhotonsThread::RenderFunc(std::stop_token stop_token) {
	const u_int workSize = 4096;

	//--------------------------------------------------------------------------
	// Initialization
	//--------------------------------------------------------------------------

	// This is really used only by Windows for 64+ threads support
	SetThreadGroupAffinity(threadIndex);
	
	RandomGenerator rndGen(seedBase + threadIndex);

	sampleBootSize = 7;
	sampleStepSize = 5;
	sampleSize = 
			sampleBootSize + // To generate the initial setup
			pgic.params.photon.maxPathDepth * sampleStepSize; // For each light vertex

	vector<float> currentPathSamples(sampleSize);
	vector<float> candidatePathSamples(sampleSize);
	vector<float> uniformPathSamples(sampleSize);

	vector<RadiancePhotonEntry> currentIndirectPhotons;
	vector<Photon> currentCausticPhotons;
	vector<PhotonBeam> currentCausticBeams;

	vector<RadiancePhotonEntry> candidateIndirectPhotons;
	vector<Photon> candidateCausticPhotons;
	vector<PhotonBeam> candidateCausticBeams;

	vector<RadiancePhotonEntry> uniformIndirectPhotons;
	vector<Photon> uniformCausticPhotons;
	vector<PhotonBeam> uniformCausticBeams;

	//--------------------------------------------------------------------------
	// Get a bucket of work to do
	//--------------------------------------------------------------------------

	const double startTime = WallClockTime();
	double lastPrintTime = startTime;
	bool foundUsefulFirstPrint = true;
	// The jthread's own token is never signaled (Join() only waits);
	// pgic.updateAbortRequested is the engine-stop path
	while(!stop_token.stop_requested() && !pgic.updateAbortRequested) {
		// Get some work to do
		u_int workCounter;
		do {
			workCounter = globalPhotonsCounter;
		} while (!globalPhotonsCounter.compare_exchange_weak(workCounter, workCounter + workSize));

		// Check if it is time to stop
		if (workCounter >= photonTracedCount)
			break;

		indirectDone = indirectDone || (!pgic.params.indirect.enabled) ||
				((pgic.params.indirect.maxSize > 0) && (globalIndirectSize >= pgic.params.indirect.maxSize));
		causticDone = causticDone || (!pgic.params.caustic.enabled) ||
				(globalCausticSize >= pgic.params.caustic.maxSize);

		// Check if it is time to stop
		if (indirectDone && causticDone)
			break;

		// Early-out for empty caches: when the first few million paths
		// stored nothing at all, the scene has no cacheable transport and
		// tracing to the full budget would only burn CPU every update.
		if ((workCounter >= 4194304) && (globalIndirectSize == 0) &&
				(globalCausticSize == 0))
			break;

		u_int workToDo = (workCounter + workSize > photonTracedCount) ?
			(photonTracedCount - workCounter) : workSize;

		if (!indirectDone)
			globalIndirectPhotonsTraced += workToDo;
		if (!causticDone)
			globalCausticPhotonsTraced += workToDo;

		// Print some progress information
		if (threadIndex == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				const float indirectProgress = !indirectDone ?
					(((globalIndirectSize > 0) && (pgic.params.indirect.maxSize > 0)) ?
						((100.0 * globalIndirectSize) / pgic.params.indirect.maxSize) : 0.f) :
					100.f;
				const float causticProgress = !causticDone ?
					((globalCausticSize > 0) ? ((100.0 * globalCausticSize) / pgic.params.caustic.maxSize) : 0.f) :
					100.f;

				SLG_LOG(boost::format("PhotonGI Cache photon traced: %d/%d [%.1f%%, %.1fM photons/sec, Map sizes (%.1f%%, %.1f%%)]") %
						workCounter % photonTracedCount %
						((100.0 * workCounter) / photonTracedCount) %
						(workCounter / (1000.0 * (now - startTime))) %
						indirectProgress %
						causticProgress);
				lastPrintTime = now;
			}
		}

		const u_int indirectPhotonsStart = indirectPhotons.size();
		const u_int causticPhotonsStart = causticPhotons.size();
		const u_int causticBeamsStart = causticBeams.size();

		//----------------------------------------------------------------------
		// Metropolis Sampler
		//----------------------------------------------------------------------

		if (pgic.params.samplerType == PGIC_SAMPLER_METROPOLIS) {
			// Look for a useful path to start with

			bool foundUseful = false;
			for (u_int i = 0; i < 16384; ++i) {
				UniformMutate(rndGen, currentPathSamples);

				foundUseful = TracePhotonPath(rndGen, currentPathSamples, currentIndirectPhotons,
						currentCausticPhotons, currentCausticBeams);
				if (foundUseful)
					break;

#ifdef WIN32
				// Work around Windows bad scheduling
                std::this_thread::yield();
#endif
			}

			if (!foundUseful) {
				// I was unable to find a useful path. Something wrong. this
				// may be an empty scene, a dark room, etc.
				if (foundUsefulFirstPrint) {
					SLG_LOG("PhotonGI metropolis sampler is unable to find a useful light path");
					foundUsefulFirstPrint = false;
				}
			} else {
				// Trace light paths

				u_int currentPhotonsScale = 1;
				float mutationSize = 1.f;
				u_int acceptedCount = 1;
				u_int mutatedCount = 1;
				u_int uniformCount = 1;
				u_int workToDoIndex = workToDo;
				while (workToDoIndex-- && !stop_token.stop_requested() &&
						!pgic.updateAbortRequested) {
					UniformMutate(rndGen, uniformPathSamples);

					if (TracePhotonPath(rndGen, uniformPathSamples, uniformIndirectPhotons,
							uniformCausticPhotons, uniformCausticBeams)) {
						// Add the old current photons (scaled by currentPhotonsScale)
						AddPhotons(currentPhotonsScale, currentIndirectPhotons, currentCausticPhotons,
								currentCausticBeams);

						// The candidate path becomes the current one
						copy(uniformPathSamples.begin(), uniformPathSamples.end(), currentPathSamples.begin());

						currentPhotonsScale = 1;
						currentIndirectPhotons = uniformIndirectPhotons;
						currentCausticPhotons = uniformCausticPhotons;
						currentCausticBeams = uniformCausticBeams;

						++uniformCount;
					} else {
						// Try a mutation of the current path
						Mutate(rndGen, currentPathSamples, candidatePathSamples, mutationSize);
						++mutatedCount;

						if (TracePhotonPath(rndGen, candidatePathSamples, candidateIndirectPhotons,
								candidateCausticPhotons, candidateCausticBeams)) {
							// Add the old current photons (scaled by currentPhotonsScale)
							AddPhotons(currentPhotonsScale, currentIndirectPhotons, currentCausticPhotons,
									currentCausticBeams);

							// The candidate path becomes the current one
							copy(candidatePathSamples.begin(), candidatePathSamples.end(), currentPathSamples.begin());

							currentPhotonsScale = 1;
							currentIndirectPhotons = candidateIndirectPhotons;
							currentCausticPhotons = candidateCausticPhotons;
							currentCausticBeams = candidateCausticBeams;

							++acceptedCount;
						} else
							++currentPhotonsScale;

						const float R = acceptedCount / (float)mutatedCount;
						// 0.234 => the optimal asymptotic acceptance ratio has been
						// derived 23.4% [Roberts et al. 1997]
						mutationSize += (R - .234f) / mutatedCount;
					}

#ifdef WIN32
					// Work around Windows bad scheduling
                    std::this_thread::yield();
#endif
				}

				// Add the last current photons (scaled by currentPhotonsScale)
				if (currentPhotonsScale > 1) {
					AddPhotons(currentPhotonsScale, currentIndirectPhotons, currentCausticPhotons,
							currentCausticBeams);
				}

				// Scale all photon values
				const float scaleFactor = uniformCount /  (float)workToDo;

				for (u_int i = indirectPhotonsStart; i < indirectPhotons.size(); ++i)
					indirectPhotons[i].alpha *= scaleFactor;
				for (u_int i = causticPhotonsStart; i < causticPhotons.size(); ++i)
					causticPhotons[i].alpha *= scaleFactor;
				for (u_int i = causticBeamsStart; i < causticBeams.size(); ++i)
					causticBeams[i].alpha *= scaleFactor;
			}
		} else

		//----------------------------------------------------------------------
		// Random Sampler
		//----------------------------------------------------------------------

		if (pgic.params.samplerType == PGIC_SAMPLER_RANDOM) {
			// Trace light paths

			u_int workToDoIndex = workToDo;
			while (workToDoIndex-- && !stop_token.stop_requested() &&
					!pgic.updateAbortRequested) {
				UniformMutate(rndGen, currentPathSamples);

				TracePhotonPath(rndGen, currentPathSamples, currentIndirectPhotons, currentCausticPhotons,
						currentCausticBeams);

				// Add the new photons
				AddPhotons(currentIndirectPhotons, currentCausticPhotons, currentCausticBeams);

#ifdef WIN32
				// Work around Windows bad scheduling
                std::this_thread::yield();
#endif
			}
		} else
			throw runtime_error("Unknown sampler type in TracePhotonsThread::RenderFunc(std::stop_token stop_token): " + ToString(pgic.params.samplerType));

		//----------------------------------------------------------------------
		
		// Update size counters
		globalIndirectSize += indirectPhotons.size() - indirectPhotonsStart;
		globalCausticSize += (causticPhotons.size() - causticPhotonsStart) +
				(causticBeams.size() - causticBeamsStart);
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

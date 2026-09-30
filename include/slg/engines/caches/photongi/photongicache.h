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

#ifndef _SLG_PHOTONGICACHE_H
#define	_SLG_PHOTONGICACHE_H

#include <functional>
#include <vector>
#include <barrier>

#include "luxrays/core/color/spectrumgroup.h"
#include "luxrays/utils/properties.h"
#include "luxrays/utils/utils.h"
#include "luxrays/utils/serializationutils.h"
#include "luxrays/utils/spillablearray.h"

#include "slg/slg.h"
#include "slg/usings.h"
#include "slg/engines/mneeseedcache.h"
#include "slg/samplers/sobol.h"
#include "slg/bsdf/bsdf.h"
#include "slg/scene/scene.h"
#include "slg/engines/caches/photongi/pgicbvh.h"
#include "slg/engines/caches/photongi/pgickdtree.h"
#include "slg/utils/pathdepthinfo.h"

namespace slg {

// OpenCL data types
namespace ocl {
#include "slg/engines/caches/photongi/pgic_types.cl"
}


//------------------------------------------------------------------------------
// Photon Mapping based GI cache
//------------------------------------------------------------------------------

struct GenericPhoton {
	GenericPhoton(const luxrays::Point &pt, const bool isVol) : p(pt), isVolume(isVol) {
	}

	luxrays::Point p;
	bool isVolume;

	friend class boost::serialization::access;
	
protected:
	// Used by serialization
	GenericPhoton() { }

	template<class Archive>
	void serialize(Archive &ar, const u_int version) {
		ar & p;
		ar & isVolume;
	}
};

struct PGICVisibilityParticle : GenericPhoton {
	PGICVisibilityParticle(
		const luxrays::Point &pt, const luxrays::Normal &nm,
		const luxrays::Spectrum& bsdfEvalTotal, const bool isVol
	) :
		GenericPhoton(pt, isVol), n(nm),
		bsdfEvaluateTotal(bsdfEvalTotal), hitsAccumulatedDistance(0.f),
		hitsCount(0)
	{}

	luxrays::SpectrumGroup ComputeRadiance(const float radius2, const float photonTraced) const {
		if (hitsCount > 0) {
			// The estimated area covered by the entry (if I have enough hits)
			const float area = (hitsCount < 16) ?  (radius2 * M_PI) :
				(luxrays::Sqr(2.f * hitsAccumulatedDistance / hitsCount) * M_PI);

			luxrays::SpectrumGroup result = alphaAccumulated;
			result *= (bsdfEvaluateTotal * INV_PI) / (photonTraced * area);

			return result;
		} else
			return luxrays::SpectrumGroup();
	}

	luxrays::Normal n;
	luxrays::Spectrum bsdfEvaluateTotal;
	luxrays::SpectrumGroup alphaAccumulated;

	// The following counters are used to estimate the surface covered by
	// this entry.
	float hitsAccumulatedDistance;
	u_int hitsCount;

	friend class boost::serialization::access;
	
protected:
	// Used by serialization
	PGICVisibilityParticle() { }

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(GenericPhoton);
		ar & n;
		ar & bsdfEvaluateTotal;
		ar & alphaAccumulated;
		ar & hitsAccumulatedDistance;
		ar & hitsCount;
	}
};

struct Photon : GenericPhoton {
	Photon(const luxrays::Point &pt, const luxrays::Vector &dir, const u_int id,
		const luxrays::Spectrum &a, const luxrays::Normal &n, const bool isVol) :
			GenericPhoton(pt, isVol), d(dir),
			lightID(id), alpha(a), landingSurfaceNormal(n) {
	}

	luxrays::Vector d;
	u_int lightID;
	luxrays::Spectrum alpha;
	luxrays::Normal landingSurfaceNormal;

	friend class boost::serialization::access;
	
protected:
	// Used by serialization
	Photon() { }

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(GenericPhoton);
		ar & d;
		ar & lightID;
		ar & alpha;
		ar & landingSurfaceNormal;
	}
};

// A caustic photon flight segment inside a homogeneous participating
// medium (Jarosz et al., "Progressive Photon Beams", 2011): the packet
// spreads its flux along the segment instead of a single point, which
// recovers thin focused shafts point photons almost never sample.
// Session-local (not serialized; point photons still cover persistent caches).
struct PhotonBeam {
	PhotonBeam(const luxrays::Point &a, const luxrays::Point &b, const u_int id,
			const luxrays::Spectrum &f, VolumeConstPtr vol) :
			p0(a), d(b - a), lightID(id), alpha(f), volume(vol) {
		length = d.Length();
		// A degenerate segment keeps a zero direction (NaN arithmetic
		// would otherwise poison every NaN-unsafe comparison in the
		// query path); the overlap test culls it on length == 0.
		if (length > 0.f)
			d /= length;
		else
			d = luxrays::Vector(0.f, 0.f, 0.f);
	}

	luxrays::Point p0;
	luxrays::Vector d;
	u_int lightID;
	luxrays::Spectrum alpha;
	float length;
	VolumeConstPtr volume;
};

struct RadiancePhoton : GenericPhoton {
	RadiancePhoton(const luxrays::Point &pt, const luxrays::Normal &nm,
		const luxrays::SpectrumGroup &rad, const bool isVol) :
				GenericPhoton(pt, isVol), n(nm), outgoingRadiance(rad) {
	}

	luxrays::Normal n;
	luxrays::SpectrumGroup outgoingRadiance;

	friend class boost::serialization::access;
	
protected:
	// Used by serialization
	RadiancePhoton() { }

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(GenericPhoton);
		ar & n;
		ar & outgoingRadiance;
	}
};

//------------------------------------------------------------------------------
// PhotonGICache
//------------------------------------------------------------------------------

typedef enum {
	PGIC_DEBUG_NONE, PGIC_DEBUG_SHOWINDIRECT, PGIC_DEBUG_SHOWCAUSTIC,
	PGIC_DEBUG_SHOWINDIRECTPATHMIX
} PhotonGIDebugType;

typedef enum {
	PGIC_SAMPLER_RANDOM, PGIC_SAMPLER_METROPOLIS
} PhotonGISamplerType;

typedef struct PhotonGICacheParams_t {
	PhotonGISamplerType samplerType;

	struct {
		u_int maxTracedCount, maxPathDepth;
		float timeStart, timeEnd;
	} photon;

	struct {
		float targetHitRate;
		u_int maxSampleCount;
		float lookUpRadius, lookUpRadius2, lookUpNormalAngle, lookUpNormalCosAngle;
	} visibility;

	float glossinessUsageThreshold;

	struct {
		bool enabled;
		u_int maxSize;
		float lookUpRadius, lookUpRadius2, lookUpNormalAngle,
				usageThresholdScale,
				filterRadiusScale, haltThreshold;
	} indirect;

	struct {
		bool enabled;
		u_int maxSize;
		float lookUpRadius, lookUpRadius2, lookUpNormalAngle,
				radiusReduction, minLookUpRadius;
		u_int updateSpp;
		// Deposit in-medium specular flight segments as photon beams and
		// answer volume-vertex caustic queries with the beam estimator.
		bool volumeBeams;
	} caustic;

	PhotonGIDebugType debugType;

	struct {
		std::string fileName;
		bool safeSave;
	} persistent;

	friend class boost::serialization::access;
	
protected:
	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & photon.maxTracedCount;
		ar & photon.maxPathDepth;

		ar & visibility.targetHitRate;
		ar & visibility.maxSampleCount;
		ar & visibility.lookUpRadius;
		ar & visibility.lookUpRadius2;
		ar & visibility.lookUpNormalAngle;
		ar & visibility.lookUpNormalCosAngle;

		ar & glossinessUsageThreshold;

		ar & indirect.enabled;
		ar & indirect.maxSize;
		ar & indirect.lookUpRadius;
		ar & indirect.lookUpRadius2;
		ar & indirect.lookUpNormalAngle;
		ar & indirect.usageThresholdScale;
		ar & indirect.filterRadiusScale;
		ar & indirect.haltThreshold;

		ar & caustic.enabled;
		ar & caustic.maxSize;
		ar & caustic.lookUpRadius;
		ar & caustic.lookUpRadius2;
		ar & caustic.lookUpNormalAngle;
		ar & caustic.radiusReduction;
		ar & caustic.minLookUpRadius;
		ar & caustic.updateSpp;
		if (version >= 7)
			ar & caustic.volumeBeams;

		ar & debugType;
		
		ar & persistent.fileName;
		ar & persistent.safeSave;
	}
} PhotonGICacheParams;

class PGICSceneVisibility;
class PGICBeamIndex;
class TracePhotonsThread;
class EyePathInfo;

class PhotonGICache {
public:
	PhotonGICache(SceneConstRef scn, const PhotonGICacheParams &params);
	virtual ~PhotonGICache();

	void SetScene(SceneRef scn) { scene = &scn; }
	PhotonGIDebugType GetDebugType() const { return params.debugType; }

	bool IsIndirectEnabled() const { return params.indirect.enabled; }
	bool IsCausticEnabled() const { return params.caustic.enabled; }
	// Caustic-only caches on projective cameras skip the visibility
	// pre-pass: deposits are frustum-culled instead of entry-gated.
	bool UseFrustumCulling() const;
	bool IsPhotonGIEnabled(const BSDF &bsdf) const;
	// Stricter gate for visibility-particle generation (upstream semantics)
	bool IsVisibilityEnabled(const BSDF &bsdf) const;
	float GetIndirectUsageThreshold(const BSDFEvent lastBSDFEvent,
			const float lastGlossiness, const float u0) const;
	bool IsDirectLightHitVisible(const EyePathInfo &pathInfo,
		const bool photonGICausticCacheUsed) const;

	const PhotonGICacheParams &GetParams() const { return params; }

	void Preprocess(const u_int threadCount);
	bool Update(const u_int threadIndex, const u_int filmSPP,
		const std::function<void()> &threadZeroCallback);
	bool Update(const u_int threadIndex, const u_int filmSPP) {
		const std::function<void()> noCallback;
		return Update(threadIndex, filmSPP, noCallback);
	}
	void FinishUpdate(const u_int threadIndex);

	const luxrays::SpectrumGroup *GetIndirectRadiance(const BSDF &bsdf) const;
	luxrays::SpectrumGroup ConnectWithCausticPaths(const BSDF &bsdf) const;

	const luxrays::SpillableArray<RadiancePhoton> &GetRadiancePhotons() const { return radiancePhotons; }
	const PGICRadiancePhotonBvh *GetRadiancePhotonsBVH() const { return radiancePhotonsBVH; }
	const u_int GetRadiancePhotonTracedCount() const { return indirectPhotonTracedCount; }

	const luxrays::SpillableArray<Photon> &GetCausticPhotons() const { return causticPhotons; }
	const PGICPhotonBvh *GetCausticPhotonsBVH() const { return causticPhotonsBVH; }
	const u_int GetCausticPhotonTracedCount() const { return causticPhotonTracedCount; }
	// Bumps once per swap (ApplyPendingUpdate): lets every render
	// thread detect a new generation after Update() returned.
	const u_int GetCausticPhotonPass() const { return causticPhotonPass; }
	const std::vector<PhotonBeam> &GetCausticBeams() const { return causticBeams; }
	// CPU semantics: maxSize caps the combined photon+beam population
	bool IsCausticFull() const {
		return causticPhotons.size() + causticBeams.size() >= params.caustic.maxSize;
	}
	const luxrays::ocl::IndexBVHArrayNode *GetCausticBeamsBVHArrayNodes(u_int *count = nullptr) const;

	static PhotonGISamplerType String2SamplerType(const std::string &type);
	static std::string SamplerType2String(const PhotonGISamplerType type);
	static PhotonGIDebugType String2DebugType(const std::string &type);
	static std::string DebugType2String(const PhotonGIDebugType type);

	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static luxrays::PropertiesUPtr GetDefaultProps();
	static PhotonGICache *FromProperties(SceneConstRef scn, const luxrays::Properties &cfg);

	// GPU photon generation (B1'): device-side light tasks deposit
	// caustic photon/beam records; the engine drains them into this
	// staging and the update worker absorbs it instead of running a
	// CPU re-trace (ingestOnly). The population accumulates across
	// generations (progressive photon mapping) under maxSize.
	void IngestTracedPhotons(const ocl::Photon *photons, const u_int nPhotons,
			const ocl::PhotonBeam *beams, const u_int nBeams, const u_int tracedCount);
	void SetIngestOnly(const bool v) { ingestOnly = v; }
	bool IsIngestOnly() const { return ingestOnly; }

	// MPG-lite Phase B: caustic photon deposits record their last
	// delta-specular vertex as an MNEE seed candidate (the photon already
	// walked the exact specular manifold an eye-side connection needs).
	// The PathTracer seed table may not exist yet when photons trace
	// (PATHCPU preprocesses before ParseOptions), so candidates collect
	// here and MergeMneeSeeds() drains them once the table is wired.
	void SetMneeSeedCache(MneeSeedEntry *cache) { mneeSeedCache = cache; }
	void MergeMneeSeeds();

	friend class PGICSceneVisibility;
	friend class TracePhotonsThread;
	friend class boost::serialization::access;

private:
	// Used by serialization
	PhotonGICache();

	float EvaluateBestRadius();
	void EvaluateBestRadiusImpl(const u_int threadIndex, const u_int workSize,
			float &accumulatedRadiusSize, u_int &radiusSizeCount) const;
	void TraceVisibilityParticles();
	void TracePhotons(const u_int seedBase, const u_int photonTracedCount,
		const bool indirectCacheDone, const bool causticCacheDone,
		std::atomic<u_int> &globalIndirectPhotonsTraced,
		std::atomic<u_int> &globalCausticPhotonsTraced,
		std::atomic<u_int> &globalIndirectSize,
		std::atomic<u_int> &globalCausticSize,
		luxrays::SpillableArray<Photon> &dstCausticPhotons,
		std::vector<PhotonBeam> &dstCausticBeams,
		u_int &dstCausticTracedCount);
	void TracePhotons(const bool indirectEnabled, const bool causticEnabled,
		luxrays::SpillableArray<Photon> *dstCausticPhotons = nullptr,
		std::vector<PhotonBeam> *dstCausticBeams = nullptr,
		u_int *dstCausticTracedCount = nullptr);
	void BuildCausticBeamsIndex();
	void BuildCausticBeamsIndex(const std::vector<PhotonBeam> &src,
			std::unique_ptr<PGICBeamIndex> &dst, const float radius);
	void UpdateWorker();
	void ApplyPendingUpdate() noexcept;
	void ConnectCausticBeams(const BSDF &bsdf, luxrays::SpectrumGroup &result) const;
	void FilterVisibilityParticlesRadiance(const std::vector<luxrays::SpectrumGroup> &radianceValues,
			std::vector<luxrays::SpectrumGroup> &filteredRadianceValues) const;
	void CreateRadiancePhotons();

	void LoadPersistentCache(const std::string &fileName);
	void SavePersistentCache(const std::string &fileName);

	template<class Archive> void serialize(Archive &ar, const u_int version);

	SceneConstPtr scene;

	PhotonGICacheParams params;

	u_int threadCount;

	// Runs the pending cache swap inside the barrier completion step: all
	// render threads are parked there, so queries can never observe a
	// half-updated cache.
	struct completion_t {
		PhotonGICache *cache = nullptr;
		void operator()() noexcept;
	};
	std::unique_ptr< std::barrier<completion_t> > threadsSyncBarrier;
	u_int lastUpdateSpp, updateSeedBase;
	// Written by FinishUpdate() between its two barrier phases and read
	// by peers outside any phase: atomic (and default-initialized, the
	// serialization path does not restore it)
	std::atomic<bool> finishUpdateFlag{false};

	// Visibility map
	luxrays::SpillableArray<PGICVisibilityParticle> visibilityParticles;
	PGICKdTree *visibilityParticlesKdTree;

	// Radiance photon map
	luxrays::SpillableArray<RadiancePhoton> radiancePhotons;
	PGICRadiancePhotonBvh *radiancePhotonsBVH;
	u_int indirectPhotonTracedCount;

	// Caustic photon maps
	luxrays::SpillableArray<Photon> causticPhotons;
	PGICPhotonBvh *causticPhotonsBVH;
	u_int causticPhotonTracedCount, causticPhotonPass;

	// Caustic photon beams (in-medium specular flight segments)
	std::vector<PhotonBeam> causticBeams;
	std::unique_ptr<PGICBeamIndex> causticBeamsIndex;

	// Shadow-copy state for stall-free cache updates: UpdateWorker()
	// traces photons and builds indices into these buffers while render
	// threads keep using the live cache; the barrier completion step
	// swaps them in atomically.
	// Guards updateThread: the pointer is created on thread 0 in
	// Update(), joined+reset inside the barrier completion step and
	// joined+reset by every render thread in FinishUpdate().
	std::mutex updateThreadMutex;
	std::unique_ptr<luxrays::JThread> updateThread;
	luxrays::SpillableArray<Photon> updateCausticPhotons;
	std::vector<PhotonBeam> updateCausticBeams;
	// Traced-path count of the pending generation: the worker writes
	// here so the live causticPhotonTracedCount (read by in-flight
	// queries) stays stable until the barrier swap publishes both.
	u_int updateCausticPhotonTracedCount = 0;
	PGICPhotonBvh *updateCausticPhotonsBVH = nullptr;
	std::unique_ptr<PGICBeamIndex> updateCausticBeamsIndex;
	float updateLookUpRadius;
	u_int updateFilmSPP;
	std::function<void()> updateCallback;
	std::atomic<bool> updateInFlight{false}, updatePendingSwap{false}, updateFailed{false};
	// Armed by the update jthread's stop callback on join/reset:
	// UpdateWorker() checks it between stages and TracePhotonsThread
	// work loops poll it, so engine stop never waits out a whole
	// photon trace
	std::atomic<bool> updateAbortRequested{false};
	// Deferred initial generation: launched by the first Update() call
	// (ingest mode is resolved by then, so GPU deposit sessions build
	// gen-1 from device records instead of a CPU trace)
	std::atomic<bool> initialUpdatePending{false};
	// Saturation backoff: logs the first skipped update pass only
	bool saturationBackoffLogged = false;

	// GPU photon deposit staging (B1'): IngestTracedPhotons() fills
	// these under the mutex; UpdateWorker() absorbs them into the
	// update* shadow buffers at build time.
	std::mutex ingestMutex;
	std::vector<Photon> ingestPhotons;
	std::vector<PhotonBeam> ingestBeams;
	u_int ingestTracedCount = 0;
	// true when photons arrive via device deposits and no CPU trace
	// runs at all (GPU engines): the worker becomes ingest + build
	bool ingestOnly = false;

	// MPG-lite Phase B seed staging: TracePhotonsThread pushes one
	// record per caustic deposit; TracePhotons() folds the per-thread
	// vectors in here, and MergeMneeSeeds() replays them into the
	// PathTracer seed table (non-owning pointer, wired by the engine).
	std::vector<MneeSeedRecord> mneeSeedRecords;
	MneeSeedEntry *mneeSeedCache = nullptr;
	bool mneeSeedLogged = false;
};

}

BOOST_CLASS_VERSION(slg::GenericPhoton, 1)
BOOST_CLASS_VERSION(slg::PGICVisibilityParticle, 2)
BOOST_CLASS_VERSION(slg::Photon, 2)
BOOST_CLASS_VERSION(slg::RadiancePhoton, 2)
BOOST_CLASS_VERSION(slg::PhotonGICacheParams, 7)
BOOST_CLASS_VERSION(slg::PhotonGICache, 3)

BOOST_CLASS_EXPORT_KEY(slg::GenericPhoton)
BOOST_CLASS_EXPORT_KEY(slg::PGICVisibilityParticle)
BOOST_CLASS_EXPORT_KEY(slg::Photon)
BOOST_CLASS_EXPORT_KEY(slg::RadiancePhoton)
BOOST_CLASS_EXPORT_KEY(slg::PhotonGICacheParams)
BOOST_CLASS_EXPORT_KEY(slg::PhotonGICache)

#endif	/* _SLG_PHOTONGICACHE_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

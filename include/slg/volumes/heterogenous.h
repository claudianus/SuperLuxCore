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

#ifndef _SLG_HETEROGENOUSVOL_H
#define	_SLG_HETEROGENOUSVOL_H

#include "luxrays/usings.h"
#include "luxrays/core/geometry/bbox.h"
#include "slg/volumes/volume.h"
#include <functional>
#include <vector>

namespace slg {

//------------------------------------------------------------------------------
// HeterogeneousVolume
//------------------------------------------------------------------------------

class HeterogeneousVolume : public Volume {
public:
	HeterogeneousVolume(
			TextureConstRef iorTex,
			TextureConstPtr emiTex,
			TextureConstRef a, TextureConstRef s,
			TextureConstRef g, const float stepSize, const u_int maxStepsCount,
			const bool multiScattering,
			const bool deltaTracking, const u_int majorantRes,
			const bool useHG = false);

	virtual float Scatter(const luxrays::Ray &ray, const float u, const bool scatteredStart,
		luxrays::Spectrum *connectionThroughput, luxrays::Spectrum *connectionEmission) const;
	virtual luxrays::Spectrum TransmittanceEstimate(const luxrays::Ray &ray,
		const float u) const;

	// Material interface

	virtual MaterialType GetType() const { return HETEROGENEOUS_VOL; }
	virtual BSDFEvent GetEventTypes() const { return DIFFUSE | REFLECT; };

	virtual luxrays::Spectrum Albedo(const HitPoint &hitPoint) const;

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW = NULL, float *reversePdfW = NULL) const;
	virtual luxrays::Spectrum Sample(const HitPoint &hitPoint,
		const luxrays::Vector &localFixedDir, luxrays::Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const;
	virtual void Pdf(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const;

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexsreferencedTexs) const;
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex);

	virtual luxrays::PropertiesUPtr ToProperties() const;

	TextureConstRef GetSigmaA() const { return sigmaA; }
	TextureConstRef GetSigmaS() const { return sigmaS; }
	TextureConstRef GetG() const { return schlickScatter.GetG(); }
	float GetStepSize() const { return stepSize; }
	u_int GetMaxStepsCount() const { return maxStepsCount; }
	bool IsMultiScattering() const { return multiScattering; }
	bool IsDeltaTracking() const { return deltaTracking; }
	u_int GetMajorantRes() const { return majorantRes; }
	bool IsHGPhase() const { return schlickScatter.IsHGPhase(); }

	// Builds the null-collision majorant grid over the given world-space
	// domain (typically the union of the bboxes of the objects using this
	// volume, or the scene bbox for world/exterior volumes). Called by
	// Scene::Preprocess() once the scene geometry is final.
	void BuildMajorantGrid(const luxrays::BBox &domain);
	bool HasMajorantGrid() const { return !majorantCells.empty(); }
	const luxrays::BBox &GetMajorantBBox() const { return majorantBBox; }
	const u_int *GetMajorantGridRes() const { return majorantGridRes; }
	const std::vector<float> &GetMajorantCells() const { return majorantCells; }
	const std::vector<float> &GetMinorantCells() const { return minorantCells; }
	float GetGlobalMajorant() const { return globalMajorant; }
	float GetGlobalMinorant() const { return globalMinorant; }

protected:
	virtual luxrays::Spectrum SigmaA(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum SigmaS(const HitPoint &hitPoint) const;

private:
	// Legacy fixed-step ray marching (kept as fallback and for "march" mode)
	float MarchScatter(const luxrays::Ray &ray, const float u, const bool scatteredStart,
		luxrays::Spectrum *connectionThroughput, luxrays::Spectrum *connectionEmission) const;
	luxrays::Spectrum MarchTransmittance(const luxrays::Ray &ray,
		const float u) const;

	// Null-collision tracking over the majorant grid (delta tracking for
	// free-flight sampling / ratio tracking for transmittance).
	// See doc/features/volume-tracking.md
	float DeltaTrackScatter(const luxrays::Ray &ray, const float u, const bool scatteredStart,
		luxrays::Spectrum *connectionThroughput, luxrays::Spectrum *connectionEmission) const;
	luxrays::Spectrum RatioTrackTransmittance(const luxrays::Ray &ray,
		const float u) const;

	// 3D DDA iterator over the majorant grid. WalkNext() returns successive
	// ray segments [t0, t1] sharing a constant majorant; regions outside the
	// grid domain are reported with globalMajorant. The state is stack-local
	// so Scatter() stays thread-safe.
	struct MajorantWalk {
		const luxrays::Ray *ray;
		int voxel[3];
		float nextCrossingT[3], deltaT[3];
		int step[3], voxelLimit[3];
		float tEnter, tExit;	// ray / grid domain intersection
		float t;				// current position
		float tMax;
		float t0, t1, maj, mn;	// current output segment (majorant/minorant)
		int phase;				// 0=pre-domain, 1=in-grid DDA, 2=post-domain, 3=done
		bool hasGrid, gridInit;
	};
	void WalkInit(MajorantWalk *w, const luxrays::Ray &ray, const float tEnd) const;
	bool WalkNext(MajorantWalk *w) const;
	float CellMajorant(const int x, const int y, const int z) const {
		return majorantCells[((u_int)z * majorantGridRes[1] + (u_int)y) * majorantGridRes[0] + (u_int)x];
	}
	float CellMinorant(const int x, const int y, const int z) const {
		return minorantCells[((u_int)z * majorantGridRes[1] + (u_int)y) * majorantGridRes[0] + (u_int)x];
	}

	std::reference_wrapper<const Texture> sigmaA, sigmaS;
	SchlickScatter schlickScatter;
	float stepSize;
	u_int maxStepsCount;
	const bool multiScattering;
	const bool deltaTracking;
	const u_int majorantRes;

	// Majorant grid (built by BuildMajorantGrid)
	luxrays::BBox majorantBBox;
	u_int majorantGridRes[3];			// cells per axis (cubic cells)
	float majorantCellSize;				// uniform cell edge length
	std::vector<float> majorantCells;	// resX*resY*resZ scalar sigma_t bounds
	std::vector<float> minorantCells;	// per-cell sigma_t lower bounds (residual tracking control)
	float globalMajorant;				// fallback outside majorantBBox
	float globalMinorant;
};

}

#endif	/* _SLG_HETEROGENOUSVOL_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _SLG_DENOISING_FRAME_H
#define _SLG_DENOISING_FRAME_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slg {

class Film;

// Immutable latest-complete-frame lease. A publisher retains only one frame;
// consumers may keep a lease while a newer frame renders, without holding the
// render/film lock or keeping a device queue busy.
struct DenoisingFrameSnapshot {
	uint64_t frameId = 0, sceneEpoch = 0;
	unsigned width = 0, height = 0;
	// Raw transport/AOVs retain the target raster. An optional exact integer
	// subgrid supplies a smaller neural input with its actual camera jitter.
	unsigned targetWidth = 0, targetHeight = 0, inputReduction = 1;
	std::vector<float> targetAlpha;
	std::vector<uint8_t> targetHoldout;
	std::vector<float> targetHoldoutRGB;
	float jitterX = 0.f, jitterY = 0.f;
	bool complete = false;
	bool motionCameraCompatible = false;
	bool losslessDelivery = false;
	double completedTime = 0.;
	std::vector<float> color, guides;
	std::vector<unsigned> sampleCount;
	float cameraToWorld[16] = {}, worldToCamera[16] = {}, cameraToRaster[16] = {};
	// Optional coherent CPU AOV/pipeline context. Only its presentation
	// scratch is mutated, under this lock; raw colour/guides stay immutable.
	std::shared_ptr<Film> presentationFilm;
	uint64_t presentationRevision = 0;
	bool presentationAccumulated = false;
	double accumulatedEyeSamples = 0.;
	size_t presentationChannelBytes = 0;
	std::string presentationError;
	mutable std::mutex presentationMutex;
};

}
#endif

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

#include <limits>
#include <algorithm>
#include <exception>

#include <boost/lexical_cast.hpp>

#include "slg/film/film.h"
#include "slg/film/imagepipeline/imagepipeline.h"
#include "slg/film/imagepipeline/radiancechannelscale.h"
#include "luxrays/utils/oclerror.h"
#include "luxrays/utils/thread.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Film image pipeline related methods
//------------------------------------------------------------------------------

bool Film::DeleteImagePipeline(ImagePipeline *pipeline) {
 if (!pipeline) return true;
 WaitAsyncExecuteImagePipeline();
 bool ok = true, pushed = false;
 if (hardwareDevice) {
  try { hardwareDevice->PushThreadCurrentDevice(); pushed = true; }
  catch (...) { ok = false; std::fprintf(stderr, "Film cleanup: context push failed\n"); }
  if (pushed) {
   try { hardwareDevice->FinishQueue(); }
   catch (...) { ok = false; std::fprintf(stderr, "Film cleanup: queue drain failed\n"); }
  }
 }
 // GPU plugins use cleanup-only buffer release; no exception may escape delete.
 // A failed context push is handled again by the owning CUDA buffer releaser.
 delete pipeline;
 if (pushed) {
  try { hardwareDevice->PopThreadCurrentDevice(); }
  catch (...) { ok = false; std::fprintf(stderr, "Film cleanup: context pop failed\n"); }
 }
 if (hardwareDevice && hardwareDevice->HasCleanupError()) ok = false;
 return ok;
}

void Film::SetImagePipelines(const u_int index, ImagePipeline *newImagePipeline) {
 bool ok = true;
 if (index < imagePipelines.size()) {
  ok = DeleteImagePipeline(imagePipelines[index]);
  imagePipelines[index] = newImagePipeline;
 } else if (index == imagePipelines.size()) {
  imagePipelines.push_back(newImagePipeline);
 } else {
  throw runtime_error("Wrong image pipeline index in Film::SetImagePipelines(): " + ToString(index));
 }
 ++imagePipelineRevision;
 // Publish ownership before reporting cleanup failure; retry/destruction must
 // never see a pointer whose destructor has already run.
 if (!ok) throw runtime_error("Image pipeline cleanup failed; replacement is owned, restart the render session");
}

void Film::SetImagePipelines(ImagePipeline *newImagePipeline) {
 std::vector<ImagePipeline *> replacement;
 if (newImagePipeline) replacement.push_back(newImagePipeline);
 bool ok = true;
 for (auto *&ip : imagePipelines) {
  ok = DeleteImagePipeline(ip) && ok;
  ip = nullptr;
 }
 imagePipelines.swap(replacement);
 ++imagePipelineRevision;
 if (!ok) throw runtime_error("Image pipeline cleanup failed; replacement is owned, restart the render session");
}

void Film::SetImagePipelines(std::vector<ImagePipeline *> &newImagePiepelines) {
 if (&newImagePiepelines == &imagePipelines) return;
 // Allocate the new pointer container before consuming any old ownership.
 auto replacement = newImagePiepelines;
 bool ok = true;
 for (auto *&ip : imagePipelines) {
  ok = DeleteImagePipeline(ip) && ok;
  ip = nullptr;
 }
 imagePipelines.swap(replacement);
 ++imagePipelineRevision;
 if (!ok) throw runtime_error("Image pipeline cleanup failed; replacement is owned, restart the render session");
}

void Film::MergeSampleBuffers(const u_int imagePipelineIndex) {
	const ImagePipeline *ip = (imagePipelineIndex < imagePipelines.size()) ? imagePipelines[imagePipelineIndex] : NULL;

	channel_IMAGEPIPELINEs[imagePipelineIndex]->Clear();
	Spectrum *p = (Spectrum *)channel_IMAGEPIPELINEs[imagePipelineIndex]->GetPixels();

	// Merge RADIANCE_PER_PIXEL_NORMALIZED and RADIANCE_PER_SCREEN_NORMALIZED buffers

	if (HasChannel(RADIANCE_PER_PIXEL_NORMALIZED)) {
		for (u_int i = 0; i < radianceGroupCount; ++i) {
			if (!ip || ip->radianceChannelScales[i].enabled) {
				#pragma omp parallel for
				for (
						// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
						unsigned
#endif
						int j = 0; j < pixelCount; ++j) {
					const float *sp = channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i]->GetPixel(j);

					if (sp[3] > 0.f) {
						Spectrum s(sp);
						s /= sp[3];
						s = ip->radianceChannelScales[i].Scale(s);

						p[j] += s;
					}
				}
			}
		}
	}

	if (HasChannel(RADIANCE_PER_SCREEN_NORMALIZED)) {
		const double RADIANCE_PER_SCREEN_NORMALIZED_SampleCount = samplesCounts.GetSampleCount_RADIANCE_PER_SCREEN_NORMALIZED();
		const float factor = (RADIANCE_PER_SCREEN_NORMALIZED_SampleCount > 0) ? (pixelCount / RADIANCE_PER_SCREEN_NORMALIZED_SampleCount) : 1.f;

		for (u_int i = 0; i < radianceGroupCount; ++i) {
			if (!ip || ip->radianceChannelScales[i].enabled) {
				#pragma omp parallel for
				for (
						// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
						unsigned
#endif
				int j = 0; j < pixelCount; ++j) {
					Spectrum s(channel_RADIANCE_PER_SCREEN_NORMALIZEDs[i]->GetPixel(j));

					if (!s.Black()) {
						s = factor * ip->radianceChannelScales[i].Scale(s);

						p[j] += s;
					}
				}
			}
		}
	}
}

void Film::AsyncExecuteImagePipeline(const u_int index) {
	if (isAsyncImagePipelineRunning)
		throw runtime_error("AsyncExecuteImagePipeline() called while another AsyncExecuteImagePipeline() is still running");

	// Retire the previous joinable worker and report its failure before reuse.
	HasDoneAsyncExecuteImagePipeline();
	isAsyncImagePipelineRunning = true;
	try {
		imagePipelineThread = std::make_unique<luxrays::JThread>(&Film::ExecuteImagePipelineThreadImpl, this, index);
	} catch (...) {
		isAsyncImagePipelineRunning = false;
		throw;
	}
	SetThreadName(imagePipelineThread, "LxImagePipeline");
}

bool Film::HasDoneAsyncExecuteImagePipeline() {
	if (isAsyncImagePipelineRunning)
		return false;
	WaitAsyncExecuteImagePipeline();
	// The release/acquire atomic flag and join publish the worker's exception.
	// Consume it once on the caller, never in a cleanup/destructor path.
	const auto error = asyncImagePipelineError;
	asyncImagePipelineError = nullptr;
	if (error)
		std::rethrow_exception(error);
	return true;
}

void Film::WaitAsyncExecuteImagePipeline() {
	// A completed worker is still joinable. Join it before replacing plugins
	// or starting a new worker so no native thread resources remain retained.
	if (imagePipelineThread && imagePipelineThread->joinable())
		imagePipelineThread->join();
}

void Film::ExecuteImagePipeline(const u_int index) {
	if (isAsyncImagePipelineRunning)
		throw runtime_error("ExecuteImagePipeline() called while an AsyncExecuteImagePipeline() is still running");

	ExecuteImagePipelineImpl(index);
}

void Film::ExecuteImagePipelineThreadImpl(const u_int index) {
	try {
		ExecuteImagePipelineImpl(index);
	} catch (...) {
		// A driver/plugin exception escaping a native thread calls terminate().
		// Keep it for the polling caller, which can report a failed denoise.
		asyncImagePipelineError = std::current_exception();
	}
	isAsyncImagePipelineRunning = false;
}

void Film::ExecuteImagePipelineImpl(const u_int index) {
	if ((!HasChannel(RADIANCE_PER_PIXEL_NORMALIZED) && !HasChannel(RADIANCE_PER_SCREEN_NORMALIZED)) ||
			!HasChannel(IMAGEPIPELINE)) {
		// Nothing to do
		return;
	}

	// Initialize OpenCL device
	if (hwEnable && !ctx) {
		CreateHWContext();
	
		if (hardwareDevice) {
			AllocateHWBuffers();
			CompileHWKernels();
		}
	}

	if (hwEnable && hardwareDevice)
		hardwareDevice->PushThreadCurrentDevice();

	try {
		// Merge all buffers
		//const double t1 = WallClockTime();
		if (hwEnable && hardwareDevice)
			MergeSampleBuffersHW(index);
		else
			MergeSampleBuffers(index);

		//const double t2 = WallClockTime();
		//SLG_LOG("MergeSampleBuffers time: " << int((t2 - t1) * 1000.0) << "ms");

		// Apply the image pipeline
		//const double p1 = WallClockTime();

		// Transfer all buffers to OpenCL device memory
		if (hwEnable && hardwareDevice && imagePipelines[index]->CanUseHW())
			WriteAllHWBuffers();

		imagePipelines[index]->Apply(*this, index);
	} catch (...) {
		// Do not leave this worker holding the film context after a failed
		// merge/plugin/readback. Preserve the original execution exception.
		if (hwEnable && hardwareDevice) {
			try { hardwareDevice->PopThreadCurrentDevice(); } catch (...) { }
		}
		throw;
	}

	if (hwEnable && hardwareDevice)
		hardwareDevice->PopThreadCurrentDevice();

	//const double p2 = WallClockTime();
	//SLG_LOG("Image pipeline " << index << " time: " << int((p2 - p1) * 1000.0) << "ms");
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

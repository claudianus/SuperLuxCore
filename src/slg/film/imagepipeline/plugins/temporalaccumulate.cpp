/***************************************************************************
 * Copyright 1998-2025 by authors (see AUTHORS.txt)                        *
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

#include <math.h>
#include <atomic>

#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <oneapi/tbb.h>

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>

#include "slg/film/imagepipeline/plugins/temporalaccumulate.h"
#include "slg/film/framebuffer.h"

using namespace std;
using namespace luxrays;
using namespace slg;
using namespace oneapi::tbb;

OIIO_NAMESPACE_USING

//------------------------------------------------------------------------------
//Temporal accumulation
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::TemporalAccumulate)

// Per-pixel history record persisted in the state EXR. Each accumulated
// target (beauty + every radiance component channel present in the film)
// stores its own RGBW block so channel sets may differ between frames;
// the geometric guides used for disocclusion validation are shared:
//   <TAG>R/G/B  accumulated linear radiance of the target
//   <TAG>W      EMA window (frames of history actually folded in)
//   Z, NX, NY, NZ, OID  first-hit depth, shading normal and object ID
#define TA_SHARED_CHANNELS 5

namespace {

// Radiance component targets: tag used in the state EXR, film channel id
// and member pointer. They are accumulated so a downstream denoiser
// reading the raw channels (e.g. OIDN in "components" mode) receives
// temporally filtered inputs instead of single-frame noise.
struct TAChannelTarget {
	const char *tag;
	const Film::FilmChannelType type;
	std::unique_ptr<GenericFrameBuffer<4, 1, float>> Film::*member;
};

const TAChannelTarget taChannelTargets[] = {
	{ "DD", Film::DIRECT_DIFFUSE, &Film::channel_DIRECT_DIFFUSE },
	{ "DDR", Film::DIRECT_DIFFUSE_REFLECT, &Film::channel_DIRECT_DIFFUSE_REFLECT },
	{ "DDT", Film::DIRECT_DIFFUSE_TRANSMIT, &Film::channel_DIRECT_DIFFUSE_TRANSMIT },
	{ "DG", Film::DIRECT_GLOSSY, &Film::channel_DIRECT_GLOSSY },
	{ "DGR", Film::DIRECT_GLOSSY_REFLECT, &Film::channel_DIRECT_GLOSSY_REFLECT },
	{ "DGT", Film::DIRECT_GLOSSY_TRANSMIT, &Film::channel_DIRECT_GLOSSY_TRANSMIT },
	{ "ID", Film::INDIRECT_DIFFUSE, &Film::channel_INDIRECT_DIFFUSE },
	{ "IDR", Film::INDIRECT_DIFFUSE_REFLECT, &Film::channel_INDIRECT_DIFFUSE_REFLECT },
	{ "IDT", Film::INDIRECT_DIFFUSE_TRANSMIT, &Film::channel_INDIRECT_DIFFUSE_TRANSMIT },
	{ "IG", Film::INDIRECT_GLOSSY, &Film::channel_INDIRECT_GLOSSY },
	{ "IGR", Film::INDIRECT_GLOSSY_REFLECT, &Film::channel_INDIRECT_GLOSSY_REFLECT },
	{ "IGT", Film::INDIRECT_GLOSSY_TRANSMIT, &Film::channel_INDIRECT_GLOSSY_TRANSMIT },
	{ "IS", Film::INDIRECT_SPECULAR, &Film::channel_INDIRECT_SPECULAR },
	{ "ISR", Film::INDIRECT_SPECULAR_REFLECT, &Film::channel_INDIRECT_SPECULAR_REFLECT },
	{ "IST", Film::INDIRECT_SPECULAR_TRANSMIT, &Film::channel_INDIRECT_SPECULAR_TRANSMIT },
	{ "EM", Film::EMISSION, &Film::channel_EMISSION },
};

// Disocclusion test between a history tap and the current pixel.
// Returns a rejection reason index for diagnostics (0 = accepted).
inline u_int HistoryTapValid(const float *tap, const u_int stride,
		const float curZ, const float *curN, const float curOID,
		const float depthRelEps, const float normalCosEps) {
	const float prevZ = tap[stride - 5];
	const bool curInf = (curZ == INFINITY), prevInf = (prevZ == INFINITY);
	if (curInf != prevInf)
		return 2;
	if (!curInf && fabsf(prevZ - curZ) > depthRelEps * Max(curZ, 1.f))
		return 3;

	if (tap[stride - 1] != curOID)
		return 4;

	// A zero normal means environment/background: only env can match env
	const float *pn = &tap[stride - 4];
	const float pnLen2 = pn[0] * pn[0] + pn[1] * pn[1] + pn[2] * pn[2];
	const float cnLen2 = curN[0] * curN[0] + curN[1] * curN[1] + curN[2] * curN[2];
	if ((pnLen2 < 1e-6f) || (cnLen2 < 1e-6f))
		return ((pnLen2 < 1e-6f) == (cnLen2 < 1e-6f)) ? 0u : 5u;
	return ((pn[0] * curN[0] + pn[1] * curN[1] + pn[2] * curN[2]) >= normalCosEps) ? 0u : 6u;
}

}

TemporalAccumulate::TemporalAccumulate(const u_int fi, const string &sd,
		const float hc, const float cs, const float dt, const float nt) {
	frameIndex = fi;
	stateDir = sd;
	historyCap = hc;
	clipSigma = cs;
	depthRelThreshold = dt;
	normalCosThreshold = nt;
}

TemporalAccumulate::TemporalAccumulate() {
	frameIndex = 0;
	stateDir = ".";
	historyCap = 32.f;
	clipSigma = 2.5f;
	depthRelThreshold = .05f;
	normalCosThreshold = .6f;
}

ImagePipelinePlugin *TemporalAccumulate::Copy() const {
	return new TemporalAccumulate(frameIndex, stateDir, historyCap, clipSigma,
			depthRelThreshold, normalCosThreshold);
}

void TemporalAccumulate::Apply(Film &film, const u_int index) {
	const double totalStartTime = WallClockTime();
	const u_int width = film.GetWidth();
	const u_int height = film.GetHeight();
	const u_int pixelCount = width * height;

	Spectrum *pixels = (Spectrum *)film.channel_IMAGEPIPELINEs[index]->GetPixels();

	if (!film.HasChannel(Film::MOTION_VECTOR)) {
		SLG_LOG("[TemporalAccumulate] WARNING: MOTION_VECTOR channel not found, pass through");
		return;
	}

	//------------------------------------------------------------------
	// Accumulation targets: the image pipeline beauty plus every
	// radiance component channel present in the film (a downstream
	// denoiser reading the raw channels then sees filtered inputs)

	struct Target {
		string tag;
		GenericFrameBuffer<4, 1, float> *ch; // nullptr = pipeline beauty
	};
	vector<Target> targets;
	targets.push_back({ "", nullptr });
	for (const auto &ct : taChannelTargets)
		if (film.HasChannel(ct.type))
			targets.push_back({ ct.tag, (film.*(ct.member)).get() });
	const u_int nTargets = targets.size();
	const u_int stride = 4 * nTargets + TA_SHARED_CHANNELS;

	//------------------------------------------------------------------
	// Extract current frame guides and normalized target values

	vector<float> depth(pixelCount, INFINITY);
	vector<float> normal(3 * pixelCount, 0.f);
	vector<float> objectID(pixelCount, 0.f);
	vector<float> sampleVar(3 * pixelCount, 0.f);
	// cur[t*3 + j] normalized radiance per pixel; wgt[t] raw buffer weight
	vector<float> cur(nTargets * 3 * pixelCount, 0.f);
	vector<float> wgt(nTargets * pixelCount, 0.f);

	const bool hasDepth = film.HasChannel(Film::DEPTH);
	const bool hasNormal = film.HasChannel(Film::AVG_SHADING_NORMAL);
	const bool hasOID = film.HasChannel(Film::OBJECT_ID);
	const bool hasVar = film.HasChannel(Film::VARIANCE);

	tbb::parallel_for(tbb::blocked_range<u_int>(0, pixelCount), [&](tbb::blocked_range<u_int> &r) {
		for (u_int i = r.begin(); i < r.end(); ++i) {
			if (hasDepth)
				depth[i] = *film.channel_DEPTH->GetPixel(i);
			if (hasNormal)
				film.channel_AVG_SHADING_NORMAL->GetWeightedPixel(i, &normal[i * 3]);
			if (hasOID)
				objectID[i] = (float)*film.channel_OBJECT_ID->GetPixel(i);

			for (u_int t = 0; t < nTargets; ++t) {
				float *dst = &cur[(t * pixelCount + i) * 3];
				if (targets[t].ch) {
					const float *raw = targets[t].ch->GetPixel(i);
					const float w = raw[3];
					wgt[t * pixelCount + i] = w;
					if (w > 0.f)
						for (u_int j = 0; j < 3; ++j)
							dst[j] = raw[j] / w;
				} else {
					for (u_int j = 0; j < 3; ++j)
						dst[j] = pixels[i].c[j];
				}
			}

			if (hasVar) {
				// VARIANCE accumulates E[x^2]: var = max(E[x^2] - mean^2, 0)
				float ex2[4];
				film.channel_VARIANCE->GetWeightedPixel(i, ex2);
				for (u_int j = 0; j < 3; ++j)
					sampleVar[i * 3 + j] = Max(ex2[j] - pixels[i].c[j] * pixels[i].c[j], 0.f);
			}
		}
	});

	//------------------------------------------------------------------
	// Load previous history state. Channel names carry the target tag:
	// "<TAG>R/G/B/W" (beauty has an empty tag) plus shared "Z/NX/NY/NZ/OID".

	const string stateFile = stateDir + "/slg_temporal_state_" +
			boost::lexical_cast<string>(index) + ".exr";
	vector<float> state;
	bool stateLoaded = false;
	if (frameIndex > 0) {
		ImageBuf buf(stateFile);
		if (buf.init_spec(stateFile, 0, 0) &&
				(buf.spec().width == (int)width) && (buf.spec().height == (int)height) &&
				buf.read(0, 0, true, TypeDesc::FLOAT)) {
			const ImageSpec &spec = buf.spec();
			// Resolve channel indices by name: shared guides + per-target RGBW
			vector<int> chIdx(stride, -1);
			const char *sharedNames[TA_SHARED_CHANNELS] = { "Z", "NX", "NY", "NZ", "OID" };
			auto resolve = [&](const string &name) -> int {
				for (int k = 0; k < spec.nchannels; ++k)
					if (spec.channelnames[k] == name)
						return k;
				return -1;
			};
			for (u_int c = 0; c < TA_SHARED_CHANNELS; ++c)
				chIdx[4 * nTargets + c] = resolve(sharedNames[c]);
			for (u_int t = 0; t < nTargets; ++t) {
				static const char *comp[4] = { "R", "G", "B", "W" };
				for (u_int c = 0; c < 4; ++c)
					chIdx[t * 4 + c] = resolve(targets[t].tag + comp[c]);
			}
			if (chIdx[stride - 1] >= 0) {
				state.resize(pixelCount * stride, 0.f);
				stateLoaded = true;
				for (ImageBuf::ConstIterator<float> it(buf); !it.done(); ++it) {
					const u_int i = it.y() * width + it.x();
					for (u_int c = 0; c < stride; ++c)
						if (chIdx[c] >= 0)
							state[i * stride + c] = buf.getchannel(it.x(), it.y(), 0, chIdx[c]);
				}
			}
		}
	}

	//------------------------------------------------------------------
	// Reproject, validate, clip and accumulate

	const u_int wr = 2; // 5x5 neighbourhood window for variance clipping
	vector<float> newState(pixelCount * stride, 0.f);
	std::atomic<u_int> validHistoryCount(0);
	// Rejection-reason counters: invalid MV, empty tap, inf mismatch,
	// depth mismatch, object-ID mismatch, normal mismatch
	std::atomic<u_int> reject[7];
	for (auto &c : reject) c.store(0);

	tbb::parallel_for(tbb::blocked_range<u_int>(0, pixelCount), [&](tbb::blocked_range<u_int> &r) {
		for (u_int i = r.begin(); i < r.end(); ++i) {
			const u_int x = i % width, y = i / width;
			float *ns = &newState[i * stride];
			const float curZ = depth[i];
			const float *curN = &normal[i * 3];
			const float curOID = objectID[i];

			// Shared reprojection: the visible surface was at p - v(p)
			// in the previous frame. Valid bilinear taps are reused for
			// every target.
			int validTap[4] = { -1, -1, -1, -1 };
			float bw[4] = { 0.f, 0.f, 0.f, 0.f };
			u_int nTaps = 0;
			if (stateLoaded) {
				const float *mv4 = film.channel_MOTION_VECTOR->GetPixel(i);
				if (mv4[2] > .5f) {
					const float hx = x - mv4[0];
					const float hy = y - mv4[1];
					const int x0 = (int)floorf(hx), y0 = (int)floorf(hy);
					const float fx = hx - x0, fy = hy - y0;
					const float w4[4] = {
						(1.f - fx) * (1.f - fy), fx * (1.f - fy),
						(1.f - fx) * fy, fx * fy };
					const int tx[4] = { x0, x0 + 1, x0, x0 + 1 };
					const int ty[4] = { y0, y0, y0 + 1, y0 + 1 };
					u_int lastReason = 2;
					for (u_int t = 0; t < 4; ++t) {
						if ((tx[t] < 0) || (tx[t] >= (int)width) ||
								(ty[t] < 0) || (ty[t] >= (int)height))
							continue;
						const float *tap = &state[(ty[t] * width + tx[t]) * stride];
						const u_int reason = HistoryTapValid(tap, stride, curZ, curN, curOID,
								depthRelThreshold, normalCosThreshold);
						if (reason != 0) {
							lastReason = reason;
							continue;
						}
						validTap[t] = ty[t] * width + tx[t];
						bw[t] = w4[t];
						++nTaps;
					}
					if (nTaps == 0)
						++reject[lastReason];
				} else
					++reject[0]; // invalid MV
			}

			// Accumulate every target with the shared taps
			for (u_int t = 0; t < nTargets; ++t) {
				const float *curT = &cur[(t * pixelCount + i) * 3];
				float histRGB[3] = { 0.f, 0.f, 0.f };
				float histW = 0.f, wSum = 0.f;
				for (u_int k = 0; k < 4; ++k) {
					if (validTap[k] < 0)
						continue;
					const float *tap = &state[validTap[k] * stride + t * 4];
					if (tap[3] <= 0.f)
						continue;
					for (u_int j = 0; j < 3; ++j)
						histRGB[j] += bw[k] * tap[j];
					wSum += bw[k];
					histW = Max(histW, tap[3]);
				}
				const bool histOK = (wSum > .25f);
				if (histOK)
					for (u_int j = 0; j < 3; ++j)
						histRGB[j] /= wSum;

				if (histOK && (clipSigma > 0.f)) {
					// TAA-style neighbourhood clip: bound the reprojected
					// history by the current frame's 5x5 mean/std, widened
					// by the beauty sample variance (a noisy current frame
					// must not over-clip the stable history)
					const u_int x0 = (x > wr) ? x - wr : 0;
					const u_int x1 = Min(x + wr, width - 1u);
					const u_int y0 = (y > wr) ? y - wr : 0;
					const u_int y1 = Min(y + wr, height - 1u);
					for (u_int j = 0; j < 3; ++j) {
						float mean = 0.f, m2 = 0.f;
						u_int n = 0;
						for (u_int yy = y0; yy <= y1; ++yy)
							for (u_int xx = x0; xx <= x1; ++xx) {
								const float v = cur[(t * pixelCount + yy * width + xx) * 3 + j];
								mean += v;
								m2 += v * v;
								++n;
							}
						mean /= n;
						const float spatialVar = Max(m2 / n - mean * mean, 0.f);
						const float sigma = sqrtf(spatialVar + sampleVar[i * 3 + j]);
						histRGB[j] = Clamp(histRGB[j],
								mean - clipSigma * sigma, mean + clipSigma * sigma);
					}
				}

				float outRGB[3], wNew;
				if (histOK) {
					wNew = Min(histW + 1.f, historyCap);
					const float alpha = 1.f / wNew;
					for (u_int j = 0; j < 3; ++j)
						outRGB[j] = std::lerp(histRGB[j], curT[j], alpha);
				} else {
					wNew = 1.f;
					for (u_int j = 0; j < 3; ++j)
						outRGB[j] = curT[j];
				}

				// Write the filtered value back
				if (targets[t].ch) {
					float *raw = targets[t].ch->GetPixel(i);
					const float w = wgt[t * pixelCount + i];
					for (u_int j = 0; j < 3; ++j)
						raw[j] = outRGB[j] * w;
				} else
					for (u_int j = 0; j < 3; ++j)
						pixels[i].c[j] = outRGB[j];
				if ((t == 0) && histOK)
					++validHistoryCount;

				ns[t * 4 + 0] = outRGB[0]; ns[t * 4 + 1] = outRGB[1];
				ns[t * 4 + 2] = outRGB[2]; ns[t * 4 + 3] = wNew;
			}

			// Persist the shared record the NEXT frame will validate against
			ns[stride - 5] = curZ;
			ns[stride - 4] = curN[0]; ns[stride - 3] = curN[1]; ns[stride - 2] = curN[2];
			ns[stride - 1] = curOID;
		}
	});

	//------------------------------------------------------------------
	// Save history state for the next frame

	ImageSpec spec(width, height, stride, TypeDesc::FLOAT);
	spec.channelnames.clear();
	for (u_int t = 0; t < nTargets; ++t) {
		static const char *comp[4] = { "R", "G", "B", "W" };
		for (u_int c = 0; c < 4; ++c)
			spec.channelnames.push_back(targets[t].tag + comp[c]);
	}
	for (const char *n : { "Z", "NX", "NY", "NZ", "OID" })
		spec.channelnames.push_back(n);
	ImageBuf outBuf(spec);
	for (ImageBuf::Iterator<float> it(outBuf); !it.done(); ++it) {
		float *px = (float *)outBuf.pixeladdr(it.x(), it.y(), 0);
		const float *ns = &newState[(it.y() * width + it.x()) * stride];
		for (u_int c = 0; c < stride; ++c)
			px[c] = ns[c];
	}
	if (!outBuf.write(stateFile))
		SLG_LOG("[TemporalAccumulate] WARNING: could not write state file " << stateFile);

	SLG_LOG("[TemporalAccumulate] frame " << frameIndex << ": accumulated " << nTargets <<
			" targets, history accepted on " <<
			(boost::format("%.1f") % (100.f * validHistoryCount / pixelCount)) <<
			"% of pixels, took " << (boost::format("%.3f") % (WallClockTime() - totalStartTime)) << "secs");
	if (stateLoaded && (validHistoryCount < pixelCount / 2)) {
		// Rejection breakdown for diagnostics: why history was dropped
		SLG_LOG("[TemporalAccumulate] rejection breakdown: " <<
				"mvInvalid=" << reject[0] << " emptyTap/oob=" << (reject[1] + reject[2]) <<
				" depth=" << reject[3] << " objectID=" << reject[4] <<
				" normal=" << (reject[5] + reject[6]));
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

// SPDX-License-Identifier: Apache-2.0
// Display-only context for reconstructed HDR. Canonical source channels are
// immutable. Ancillary weighted values, integer IDs and Cryptomatte pairs use
// center-nearest sampling together with their weights; no IDs are interpolated.
#ifndef SLC_PRESENTATION_RESAMPLE_H
#define SLC_PRESENTATION_RESAMPLE_H

#include "slg/film/film.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace slg {

inline void CopyScaledPresentationChannels(const Film &source, Film &target) {
 const unsigned sw = source.GetWidth(), sh = source.GetHeight();
 const unsigned tw = target.GetWidth(), th = target.GetHeight();
 const size_t sourcePixels = size_t(sw) * sh;
 std::vector<unsigned> xs(tw), ys(th);
 for (unsigned x = 0; x < tw; ++x)
  xs[x] = std::min(unsigned((uint64_t(2) * x + 1) * sw / (uint64_t(2) * tw)), sw - 1);
 for (unsigned y = 0; y < th; ++y)
  ys[y] = std::min(unsigned((uint64_t(2) * y + 1) * sh / (uint64_t(2) * th)), sh - 1);
 auto copy = [&](const auto &src, auto &dst) {
  if (!src) return;
  if (!dst) throw std::runtime_error("Missing reconstructed presentation channel");
  const size_t stride = src->GetSize() / sourcePixels / sizeof(*src->GetPixels());
  if (dst->GetSize() != size_t(tw) * th * stride * sizeof(*src->GetPixels()))
   throw std::runtime_error("Reconstructed presentation channel layout differs");
  for (unsigned y = 0; y < th; ++y) {
   const unsigned sy = ys[y];
   for (unsigned x = 0; x < tw; ++x) {
    const unsigned sx = xs[x];
    std::copy_n(src->GetPixel(sx + sy * sw), stride, dst->GetPixel(x + y * tw));
   }
  }
 };
	copy(source.channel_ALBEDO, target.channel_ALBEDO);
	copy(source.channel_ALPHA, target.channel_ALPHA);
	copy(source.channel_AVG_SHADING_NORMAL, target.channel_AVG_SHADING_NORMAL);
	for (size_t i = 0; i < source.channel_BY_MATERIAL_IDs.size(); ++i) copy(source.channel_BY_MATERIAL_IDs[i], target.channel_BY_MATERIAL_IDs[i]);
	for (size_t i = 0; i < source.channel_BY_OBJECT_IDs.size(); ++i) copy(source.channel_BY_OBJECT_IDs[i], target.channel_BY_OBJECT_IDs[i]);
	copy(source.channel_CONVERGENCE, target.channel_CONVERGENCE);
	copy(source.channel_CRYPTOMATTE_MATERIAL, target.channel_CRYPTOMATTE_MATERIAL);
	copy(source.channel_CRYPTOMATTE_OBJECT, target.channel_CRYPTOMATTE_OBJECT);
	copy(source.channel_DEPTH, target.channel_DEPTH);
	copy(source.channel_DIRECT_DIFFUSE, target.channel_DIRECT_DIFFUSE);
	copy(source.channel_DIRECT_DIFFUSE_REFLECT, target.channel_DIRECT_DIFFUSE_REFLECT);
	copy(source.channel_DIRECT_DIFFUSE_TRANSMIT, target.channel_DIRECT_DIFFUSE_TRANSMIT);
	copy(source.channel_DIRECT_GLOSSY, target.channel_DIRECT_GLOSSY);
	copy(source.channel_DIRECT_GLOSSY_REFLECT, target.channel_DIRECT_GLOSSY_REFLECT);
	copy(source.channel_DIRECT_GLOSSY_TRANSMIT, target.channel_DIRECT_GLOSSY_TRANSMIT);
	copy(source.channel_DIRECT_SHADOW_MASK, target.channel_DIRECT_SHADOW_MASK);
	copy(source.channel_EMISSION, target.channel_EMISSION);
	copy(source.channel_GEOMETRY_NORMAL, target.channel_GEOMETRY_NORMAL);
	for (size_t i = 0; i < source.channel_IMAGEPIPELINEs.size(); ++i) copy(source.channel_IMAGEPIPELINEs[i], target.channel_IMAGEPIPELINEs[i]);
	copy(source.channel_INDIRECT_DIFFUSE, target.channel_INDIRECT_DIFFUSE);
	copy(source.channel_INDIRECT_DIFFUSE_REFLECT, target.channel_INDIRECT_DIFFUSE_REFLECT);
	copy(source.channel_INDIRECT_DIFFUSE_TRANSMIT, target.channel_INDIRECT_DIFFUSE_TRANSMIT);
	copy(source.channel_INDIRECT_GLOSSY, target.channel_INDIRECT_GLOSSY);
	copy(source.channel_INDIRECT_GLOSSY_REFLECT, target.channel_INDIRECT_GLOSSY_REFLECT);
	copy(source.channel_INDIRECT_GLOSSY_TRANSMIT, target.channel_INDIRECT_GLOSSY_TRANSMIT);
	copy(source.channel_INDIRECT_SHADOW_MASK, target.channel_INDIRECT_SHADOW_MASK);
	copy(source.channel_INDIRECT_SPECULAR, target.channel_INDIRECT_SPECULAR);
	copy(source.channel_INDIRECT_SPECULAR_REFLECT, target.channel_INDIRECT_SPECULAR_REFLECT);
	copy(source.channel_INDIRECT_SPECULAR_TRANSMIT, target.channel_INDIRECT_SPECULAR_TRANSMIT);
	copy(source.channel_IRRADIANCE, target.channel_IRRADIANCE);
	for (size_t i = 0; i < source.channel_LPEs.size(); ++i) copy(source.channel_LPEs[i], target.channel_LPEs[i]);
	copy(source.channel_MATERIAL_ID, target.channel_MATERIAL_ID);
	copy(source.channel_MATERIAL_ID_COLOR, target.channel_MATERIAL_ID_COLOR);
	for (size_t i = 0; i < source.channel_MATERIAL_ID_MASKs.size(); ++i) copy(source.channel_MATERIAL_ID_MASKs[i], target.channel_MATERIAL_ID_MASKs[i]);
	copy(source.channel_MOTION_VECTOR, target.channel_MOTION_VECTOR);
	copy(source.channel_NOISE, target.channel_NOISE);
	copy(source.channel_OBJECT_ID, target.channel_OBJECT_ID);
	for (size_t i = 0; i < source.channel_OBJECT_ID_MASKs.size(); ++i) copy(source.channel_OBJECT_ID_MASKs[i], target.channel_OBJECT_ID_MASKs[i]);
	copy(source.channel_POSITION, target.channel_POSITION);
	for (size_t i = 0; i < source.channel_RADIANCE_PER_PIXEL_NORMALIZEDs.size(); ++i) copy(source.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i], target.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i]);
	for (size_t i = 0; i < source.channel_RADIANCE_PER_SCREEN_NORMALIZEDs.size(); ++i) copy(source.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[i], target.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[i]);
	copy(source.channel_RAYCOUNT, target.channel_RAYCOUNT);
	copy(source.channel_SAMPLECOUNT, target.channel_SAMPLECOUNT);
	copy(source.channel_SHADING_NORMAL, target.channel_SHADING_NORMAL);
	copy(source.channel_USER_IMPORTANCE, target.channel_USER_IMPORTANCE);
	copy(source.channel_UV, target.channel_UV);
	copy(source.channel_VARIANCE, target.channel_VARIANCE);
 for (unsigned y = 0; y < th; ++y)
  for (unsigned x = 0; x < tw; ++x) {
   const size_t src = size_t(xs[x] + ys[y] * sw) * 2, dst = size_t(x + y * tw) * 2;
   std::copy_n(source.pixelLumaMoments.data() + src, 2, target.pixelLumaMoments.data() + dst);
  }
 if (target.channel_MOTION_VECTOR) {
  auto *motion = target.channel_MOTION_VECTOR->GetPixels();
  for (size_t i = 0; i < size_t(tw) * th; ++i) {
   motion[i * 4] *= float(tw) / sw;
   motion[i * 4 + 1] *= float(th) / sh;
  }
 }
 const double ratio = double(tw) * th / sourcePixels;
 target.SetSampleCount(source.GetTotalSampleCount() * ratio,
   source.GetTotalEyeSampleCount() * ratio, source.GetTotalLightSampleCount() * ratio);
}

inline size_t ScaledPresentationBytes(const Film &source, const unsigned w, const unsigned h) {
 // Source coherent contexts are already bounded and have fixed per-pixel
 // channels. Include this processor's returned RGBA in the separate1GiB cap.
 const size_t bytesPerPixel = source.GetChannelStorageSize() /
   (size_t(source.GetWidth()) * source.GetHeight());
 return size_t(w) * h * (bytesPerPixel + 6 * sizeof(float)) +
   (size_t(w) + h) * sizeof(unsigned);
}

inline FilmUPtr CreateScaledPresentation(const Film &source, const unsigned w, const unsigned h) {
 const auto *region = source.GetSubRegion();
 const unsigned sw = source.GetWidth(), sh = source.GetHeight();
 const unsigned subregion[4] = {
  unsigned(uint64_t(region[0]) * w / sw),
  std::min(w - 1, unsigned((uint64_t(region[1] + 1) * w + sw - 1) / sw - 1)),
  unsigned(uint64_t(region[2]) * h / sh),
  std::min(h - 1, unsigned((uint64_t(region[3] + 1) * h + sh - 1) / sh - 1))
 };
 auto result = Film::Create(w, h, subregion);
 result->CopyDynamicSettings(source);
 result->hwEnable = false;
 result->GetDenoiser().SetEnabled(false);
 result->Init();
 return result;
}
}
#endif

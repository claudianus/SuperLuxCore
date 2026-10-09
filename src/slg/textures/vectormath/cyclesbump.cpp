// SPDX-License-Identifier: Apache-2.0
#include "slg/textures/vectormath/cyclesbump.h"
#include "luxrays/core/color/spectral.h"
using namespace luxrays;
using namespace slg;
static Vector BumpSafeNormalize(const Vector &v) {
    const float l2 = Dot(v, v);
    return std::isfinite(l2) && l2 > 0.f ? v / sqrtf(l2) : Vector();
}
Spectrum CyclesBumpTexture::EvalSpectrumValue(const HitPoint &hp) const {
    const Normal n = EvaluateNormal(hp, .001f);
    return Spectrum(&n.x);
}
Normal CyclesBumpTexture::Bump(const HitPoint &hp, const float sampleDistance) const {
    const Normal n = EvaluateNormal(hp, sampleDistance);
    return Dot(hp.fixedDir, Vector(hp.geometryN)) < 0.f ? -n : n;
}
Normal CyclesBumpTexture::EvaluateNormal(const HitPoint &hp, const float sampleDistance) const {
    const Spectral::ScopePause pause;
    const float side = Dot(hp.fixedDir, Vector(hp.geometryN)) < 0.f ? -1.f : 1.f;
    const Vector normal = useNormal ? Vector(GetInput(3).GetSpectrumValue(hp).c) : side * Vector(hp.shadeN);
    // Distance is local amplitude, never part of the differentiated height.
    const float distance = GetInput(1).GetFloatValue(hp) * (invert ? -1.f : 1.f);
    const float strength = Max(GetInput(2).GetFloatValue(hp), 0.f);
    const float precision = 4.76837158203125e-7f * Max(1.f,
            Max(fabsf(hp.p.x), Max(fabsf(hp.p.y), fabsf(hp.p.z))));
    const float step = Max(filterWidth * (hp.bumpFootprint > 0.f ? hp.bumpFootprint : sampleDistance), precision);
    const float uLength = hp.dpdu.Length(), vLength = hp.dpdv.Length();
    if (!(uLength > 0.f) || !(vLength > 0.f)) return Normal(normal);
    const float uu = step / uLength, vv = step / vLength;
    const float center = GetInput(0).GetFloatValue(hp);
    HitPoint shifted = hp;
    shifted.p = hp.p + uu * hp.dpdu;
    shifted.defaultUV.u = hp.defaultUV.u + uu;
    shifted.shadeN = Normalize(hp.shadeN + uu * hp.dndu);
    const float hu = GetInput(0).GetFloatValue(shifted);
    shifted = hp;
    shifted.p = hp.p + vv * hp.dpdv;
    shifted.defaultUV.v = hp.defaultUV.v + vv;
    shifted.shadeN = Normalize(hp.shadeN + vv * hp.dndv);
    const float hv = GetInput(0).GetFloatValue(shifted);
    const Vector rx = Cross(hp.dpdv, normal), ry = Cross(normal, hp.dpdu);
    const float det = Dot(hp.dpdu, rx);
    // Cycles flips both surface partials on backfaces before UV derivatives.
    const Vector gradient = side * (((hu - center) / uu) * rx + ((hv - center) / vv) * ry);
    const Vector perturbed = BumpSafeNormalize(fabsf(det) * normal - distance * Sgn(det) * gradient);
    if (Dot(perturbed, perturbed) == 0.f) return Normal(normal);
    return Normal(BumpSafeNormalize(strength * perturbed + (1.f - strength) * normal));
}
PropertiesUPtr CyclesBumpTexture::ToProperties(const ImageMapCache &cache, const bool real) const {
    auto p = std::make_unique<Properties>();
    const std::string prefix = "scene.textures." + GetName();
    p->Set(Property(prefix + ".type")("cyclesbump"));
    const char *names[] = {"height", "distance", "strength", "normal"};
    for (u_int i = 0; i < 4; ++i) p->Set(Property(prefix + "." + names[i])(GetInput(i).GetSDLValue()));
    p->Set(Property(prefix + ".usenormal")(useNormal));
    p->Set(Property(prefix + ".invert")(invert));
    p->Set(Property(prefix + ".filterwidth")(filterWidth));
    return p;
}

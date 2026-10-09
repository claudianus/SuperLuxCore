// SPDX-License-Identifier: Apache-2.0
#include "slg/textures/vectormath/cyclesnormalmap.h"
#include "luxrays/core/color/spectral.h"
using namespace luxrays;
using namespace slg;
static Vector SafeNormalVector(const Vector &v) {
    const float length2 = Dot(v, v);
    return std::isfinite(length2) && length2 > 0.f ? v / sqrtf(length2) : Vector();
}
Spectrum CyclesNormalMapTexture::EvalSpectrumValue(const HitPoint &hp) const {
    const Normal n = EvaluateNormal(hp);
    return Spectrum(&n.x);
}
Normal CyclesNormalMapTexture::Bump(const HitPoint &hp, const float sampleDistance) const {
    const Normal value = EvaluateNormal(hp);
    return (Dot(hp.fixedDir, Vector(hp.geometryN)) < 0.f) ? -value : value;
}
Normal CyclesNormalMapTexture::EvaluateNormal(const HitPoint &hp) const {
    const Spectral::ScopePause pause;
    const float side = Dot(hp.fixedDir, Vector(hp.geometryN)) < 0.f ? -1.f : 1.f;
    const Normal base = side * hp.shadeN;
    const Spectrum rgb = GetInput(0).GetSpectrumValue(hp);
    Vector color = 2.f * Vector(rgb.c) - Vector(1.f, 1.f, 1.f);
    if (invertGreen) color.y = -color.y;
    const float strength = GetInput(1).GetFloatValue(hp);
    Vector result;
    if (space == 0) {
        if (!hp.mesh || normalIndex == NULL_INDEX || tangentIndex == NULL_INDEX || signIndex == NULL_INDEX)
            return base;
        const Vector normal(hp.GetColor(normalIndex).c);
        const Vector tangent(hp.GetColor(tangentIndex).c);
        const float sign = hp.GetAlpha(signIndex);
        color.x *= strength;
        color.y *= strength;
        color.z = Lerp(Clamp(strength, 0.f, 1.f), 1.f, color.z);
        result = tangent * color.x + sign * Cross(normal, tangent) * color.y + normal * color.z;
        result = Vector(hp.localToWorld * Normal(SafeNormalVector(result)));
    } else {
        if (space == 3 || space == 4) { color.y = -color.y; color.z = -color.z; }
        result = (space == 1 || space == 3) ? Vector(hp.localToWorld * Normal(color)) : color;
    }
    result = SafeNormalVector(result);
    result *= side;
    if (space != 0 && strength != 1.f)
        result = SafeNormalVector(Vector(base) + (result - Vector(base)) * Max(strength, 0.f));
    const float length2 = Dot(result, result);
    return length2 > 0.f && std::isfinite(length2) ? Normal(result) : base;
}
PropertiesUPtr CyclesNormalMapTexture::ToProperties(const ImageMapCache &cache, const bool real) const {
    auto p = std::make_unique<Properties>();
    const std::string prefix = "scene.textures." + GetName();
    p->Set(Property(prefix + ".type")("cyclesnormalmap"));
    p->Set(Property(prefix + ".color")(GetInput(0).GetSDLValue()));
    p->Set(Property(prefix + ".strength")(GetInput(1).GetSDLValue()));
    p->Set(Property(prefix + ".space")(space));
    p->Set(Property(prefix + ".invertgreen")(invertGreen));
    p->Set(Property(prefix + ".normalindex")(normalIndex));
    p->Set(Property(prefix + ".tangentindex")(tangentIndex));
    p->Set(Property(prefix + ".signindex")(signIndex));
    return p;
}

#!/usr/bin/env python3
"""Generate the GGX-glass directional albedo LUT for the Huang hair model.

E(rough, mu, eta) = single-scattering directional albedo of a rough dielectric
(GGX) interface -- the "albedo correction" table used by Huang et al. 2022's
energy scale term, matching Cycles' `ggx_glass_E` / `ggx_glass_inv_E`
precomputed tables (intern/cycles/app/cycles_precompute.cpp).

Estimator (identities from Heitz'14 microfacet theory):
    wh ~ VNDF(wi, alpha)
    E_refl = F(cos_hi, eta) * G2(wi, wo_r) / G1(wi)
    E_tran = (1 - F)        * G2(wi, wo_t) / G1(wi)     (0 when TIR: F = 1)
    E      = E_refl + E_tran          (per-wh expectation, all terms share wh)

Outputs:
    include/slg/materials/hair_huang_lut.h   (C++ / shared scalar code)
    include/slg/materials/hair_huang_lut.cl  (OpenCL mirror)

Both files contain identical data generated from this single script.
"""

import numpy as np
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RES_R, RES_M, RES_Z = 16, 16, 16  # same resolution as Cycles ggx_glass_E
N_SAMPLES = 1 << 16               # stratified Halton samples per cell


def halton(i, base):
    f, r, b = 1.0, 0.0, float(base)
    while i > 0:
        f /= b
        r += f * (i % int(b))
        i //= int(b)
    return r


def vndf_sample(wi, alpha, u):
    """Heitz'18 isotropic GGX VNDF sampling (matches GgxSampleVNDF)."""
    # stretch incident direction to isotropic space
    v = np.stack([alpha * wi[..., 0], alpha * wi[..., 1], wi[..., 2]], -1)
    v /= np.linalg.norm(v, axis=-1, keepdims=True)
    # orthonormal basis around +z
    lensq = v[..., 0] ** 2 + v[..., 1] ** 2
    t1 = np.where(lensq[..., None] > 1e-12,
                  np.stack([-v[..., 1], v[..., 0], np.zeros_like(v[..., 2])], -1)
                  / np.sqrt(np.maximum(lensq, 1e-30))[..., None],
                  np.broadcast_to(np.array([1.0, 0.0, 0.0]), v.shape).copy())
    t2 = np.stack([v[..., 1] * t1[..., 2] - v[..., 2] * t1[..., 1],
                   v[..., 2] * t1[..., 0] - v[..., 0] * t1[..., 2],
                   v[..., 0] * t1[..., 1] - v[..., 1] * t1[..., 0]], -1)
    # sample hemisphere in isotropic space (Heitz'18 / pbrt-v4 warp)
    r = np.sqrt(u[..., 0])
    phi = 2.0 * np.pi * u[..., 1]
    t1v = r * np.cos(phi)
    t2v = r * np.sin(phi)
    s = 0.5 * (1.0 + v[..., 2])
    t2v = (1.0 - s) * np.sqrt(np.maximum(0.0, 1.0 - t1v * t1v)) + s * t2v
    h = t1 * t1v[..., None] + t2 * t2v[..., None] \
        + v * np.sqrt(np.maximum(0.0, 1.0 - t1v * t1v - t2v * t2v))[..., None]
    # unstretch
    wh = np.stack([alpha * h[..., 0], alpha * h[..., 1],
                   np.maximum(h[..., 2], 0.0)], -1)
    wh /= np.linalg.norm(wh, axis=-1, keepdims=True)
    return wh


def fresnel_dielectric(cos_i, eta):
    """Unpolarized dielectric Fresnel; returns 1 on TIR."""
    cos_i = np.clip(cos_i, 0.0, 1.0)
    sin2_t = (1.0 - cos_i * cos_i) / (eta * eta)
    cos_t = np.sqrt(np.maximum(0.0, 1.0 - sin2_t))
    tir = sin2_t >= 1.0
    cos_t_safe = np.where(tir, 1.0, cos_t)
    r_parl = (eta * cos_i - cos_t_safe) / (eta * cos_i + cos_t_safe)
    r_perp = (cos_i - eta * cos_t_safe) / (cos_i + eta * cos_t_safe)
    f = 0.5 * (r_parl * r_parl + r_perp * r_perp)
    return np.where(tir, 1.0, f)


def lambda_ggx(alpha, cos_w):
    """Smith Lambda for isotropic GGX, cos measured against +z."""
    c = np.abs(cos_w)
    return np.where(c < 1e-7, 1e30,
                    0.5 * (np.sqrt(1.0 + alpha * alpha * (1.0 - c * c)
                                   / (c * c)) - 1.0))


def cell_E(rough, mu, ior, u):
    alpha = rough * rough
    wi = np.tile(np.array([np.sqrt(max(0.0, 1.0 - mu * mu)), 0.0, mu]),
                 (u.shape[0], 1))
    wh = vndf_sample(wi, alpha, u)
    cos_hi = np.sum(wi * wh, -1)
    valid = cos_hi > 0.0
    cos_hi = np.clip(cos_hi, 0.0, 1.0)
    F = fresnel_dielectric(cos_hi, ior)

    g1 = 1.0 / (1.0 + lambda_ggx(alpha, wi[:, 2]))

    # reflected direction
    wo_r = 2.0 * cos_hi[:, None] * wh - wi
    g2_r = 1.0 / (1.0 + lambda_ggx(alpha, wi[:, 2])
                  + lambda_ggx(alpha, wo_r[:, 2]))
    e_refl = F * g2_r / g1

    # refracted direction (eta is relative index, >= 1 on entry side;
    # the caller stores the inv table by passing 1/ior)
    inv = 1.0 / ior
    sin2_t = (1.0 - cos_hi * cos_hi) * inv * inv
    tir = sin2_t >= 1.0
    cos_t = np.sqrt(np.maximum(0.0, 1.0 - sin2_t))
    wo_t = (inv * cos_hi - cos_t)[:, None] * wh - inv * wi
    # masking uses |cos| on the transmitted side (pbrt convention)
    g2_t = 1.0 / (1.0 + lambda_ggx(alpha, wi[:, 2])
                  + lambda_ggx(alpha, -wo_t[:, 2]))
    e_tran = np.where(tir, 0.0, (1.0 - F) * g2_t / g1)

    return np.where(valid, e_refl + e_tran, 0.0).mean()


def main():
    n = N_SAMPLES
    idx = np.arange(1, n + 1)
    u = np.stack([np.array([halton(i, 2) for i in idx]),
                  np.array([halton(i, 3) for i in idx])], -1)

    table = np.zeros((RES_R, RES_M, RES_Z), dtype=np.float32)
    table_inv = np.zeros_like(table)
    for ir in range(RES_R):
        rough = (ir + 0.5) / RES_R
        for im in range(RES_M):
            mu = (im + 0.5) / RES_M
            for iz in range(RES_Z):
                z = (iz + 0.5) / RES_Z
                ior = (1.0 + z * z) / (1.0 - z * z)
                table[ir, im, iz] = cell_E(rough, mu, ior, u)
                table_inv[ir, im, iz] = cell_E(rough, mu, 1.0 / ior, u)
        print(f"rough row {ir + 1}/{RES_R} done", flush=True)

    def fmt(t):
        return ",\n    ".join(
            ", ".join(f"{v:.6e}f" for v in row)
            for row in t.reshape(-1, RES_Z))

    for path, guard, fn, decl, inl, ptr in [
        (ROOT / "include/slg/materials/hair_huang_lut.h",
         "LUXCORE_HAIR_HUANG_LUT_H", "Huang_GlassE",
         "static const float", "static inline", "const float *"),
        (ROOT / "include/slg/materials/hair_huang_lut.cl",
         None, "HairHuang_GlassE", "__constant float",
         "OPENCL_FORCE_INLINE", "__constant float *"),
    ]:
        tab_e = "kHairHuangGlassE" if guard else "kHairHuangGlassE_cl"
        tab_i = "kHairHuangGlassEInv" if guard else "kHairHuangGlassEInv_cl"
        ns = "std::" if guard else ""
        body = f"""// Generated by dev-tools/e36_gen_huang_lut.py -- do not edit by hand.
// Directional albedo E(sqrt(alpha), cos_theta, ior) of a single-scattering
// GGX dielectric interface; the Huang'22 energy correction divides by this.
// Axes: rough = sqrt(alpha) in [0,1], mu = cos to the mesonormal, and
// z = sqrt(|eta - 1| / (eta + 1)); the *_inv table stores E for eta < 1
// (inside-out transmission). Trilinear lookup. Same data as Cycles'
// ggx_glass_E / ggx_glass_inv_E tables.
{f'#ifndef {guard}\n#define {guard}\n#include <cmath>\n' if guard else ''}
{decl} {tab_e}[{RES_R} * {RES_M} * {RES_Z}] = {{
    {fmt(table)}
}};

{decl} {tab_i}[{RES_R} * {RES_M} * {RES_Z}] = {{
    {fmt(table_inv)}
}};

{inl} float {fn}(const float mu, const float sqrtAlpha, const float eta) {{
    const bool inv = eta < 1.f;
    const float z = {ns}sqrt({ns}fabs((eta - 1.f) / (eta + 1.f)));
    {ptr}tab = inv ? {tab_i} : {tab_e};
    const float fr = {ns}fmin({ns}fmax(sqrtAlpha, 0.f), 1.f) * {RES_R}.f - 0.5f;
    const float fm = {ns}fmin({ns}fmax(mu, 0.f), 1.f) * {RES_M}.f - 0.5f;
    const float fz = {ns}fmin({ns}fmax(z, 0.f), 0.9999f) * {RES_Z}.f - 0.5f;
    const int r0 = (int){ns}fmin({ns}fmax({ns}floor(fr), 0.f), {RES_R - 2}.f);
    const int m0 = (int){ns}fmin({ns}fmax({ns}floor(fm), 0.f), {RES_M - 2}.f);
    const int z0 = (int){ns}fmin({ns}fmax({ns}floor(fz), 0.f), {RES_Z - 2}.f);
    const float tr = {ns}fmin({ns}fmax(fr - r0, 0.f), 1.f);
    const float tm = {ns}fmin({ns}fmax(fm - m0, 0.f), 1.f);
    const float tz = {ns}fmin({ns}fmax(fz - z0, 0.f), 1.f);
    float c[2][2][2];
    for (int dr = 0; dr < 2; ++dr)
        for (int dm = 0; dm < 2; ++dm)
            for (int dz = 0; dz < 2; ++dz)
                c[dr][dm][dz] = tab[((r0 + dr) * {RES_M} + (m0 + dm)) * {RES_Z} + (z0 + dz)];
    const float v00 = (1.f - tz) * c[0][0][0] + tz * c[0][0][1];
    const float v01 = (1.f - tz) * c[0][1][0] + tz * c[0][1][1];
    const float v10 = (1.f - tz) * c[1][0][0] + tz * c[1][0][1];
    const float v11 = (1.f - tz) * c[1][1][0] + tz * c[1][1][1];
    const float v0 = (1.f - tm) * v00 + tm * v01;
    const float v1 = (1.f - tm) * v10 + tm * v11;
    return (1.f - tr) * v0 + tr * v1;
}}
{f'#endif // {guard}' if guard else ''}
"""
        path.write_text(body)
        print(f"wrote {path} ({path.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()

// SPDX-License-Identifier: Apache-2.0
#ifndef SLG_CYCLES_BSSRDF_BOUNDARY_CL
#define SLG_CYCLES_BSSRDF_BOUNDARY_CL

// Shared by the CPU header and GPU kernel source. These are directional
// boundary measures, not the spatial BSSRDF PDF or a complete adjoint walk.
// Cycles omits physical GGX attenuation at entry. Its transpose consequently
// differs from an ordinary reciprocal rough-glass throughput.
#ifndef CYCLES_BSSRDF_BOUNDARY_PI
#define CYCLES_BSSRDF_BOUNDARY_PI M_PI_F
#endif

typedef struct {
	float forwardPdf, reversePdf, adjointWeight;
	float forwardMass, reverseMass;
	bool delta, valid;
} CyclesBSSRDFBoundaryDensities;

typedef struct {
	float3 direction;
	CyclesBSSRDFBoundaryDensities densities;
	bool valid;
} CyclesBSSRDFBoundarySample;

OPENCL_FORCE_INLINE float3 CyclesBSSRDF_GGX(const float3 v, const float alpha, const float u0, const float u1) {
	if (alpha == 0.f) return MAKE_FLOAT3(0.f, 0.f, 1.f);
	const float3 vh = normalize(MAKE_FLOAT3(alpha * v.x, alpha * v.y, v.z));
	const float l = vh.x * vh.x + vh.y * vh.y;
	const float3 t1 = l > 0.f ? MAKE_FLOAT3(-vh.y, vh.x, 0.f) / sqrt(l) : MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 t2 = cross(vh, t1);
	const float r = sqrt(u0), phi = 2.f * CYCLES_BSSRDF_BOUNDARY_PI * u1;
	const float x = r * cos(phi), mix = .5f * (1.f + vh.z);
	const float y = (1.f - mix) * sqrt(fmax(0.f, 1.f - x * x)) + mix * r * sin(phi);
	const float3 n = x * t1 + y * t2 + sqrt(fmax(0.f, 1.f - x * x - y * y)) * vh;
	return normalize(MAKE_FLOAT3(alpha * n.x, alpha * n.y, fmax(0.f, n.z)));
}

OPENCL_FORCE_INLINE float CyclesBSSRDF_BoundaryG1(const float3 v, const float alpha) {
	const float z = fabs(v.z);
	if (!(z > 0.f)) return 0.f;
	return 2.f * z / (z + sqrt(z * z + alpha * alpha * (v.x * v.x + v.y * v.y)));
}

OPENCL_FORCE_INLINE CyclesBSSRDFBoundaryDensities CyclesBSSRDF_BoundaryDensities(
		const float3 outside, const float3 inside, const float ior, const float alpha) {
	CyclesBSSRDFBoundaryDensities p;
	p.forwardPdf = p.reversePdf = p.adjointWeight = p.forwardMass = p.reverseMass = 0.f;
	p.delta = alpha == 0.f;
	p.valid = false;
	if (!(outside.z > 0.f && inside.z < 0.f && ior > 1.f && alpha >= 0.f) ||
			!isfinite(ior) || !isfinite(alpha)) return p;
	const float3 o = normalize(outside), d = normalize(inside);
	if (p.delta) {
		const float eta = 1.f / ior;
		const float3 expected = MAKE_FLOAT3(-eta * o.x, -eta * o.y,
				-sqrt(fmax(0.f, 1.f - eta * eta * (1.f - o.z * o.z))));
		const float3 error = expected - d;
		if (dot(error, error) > 1e-12f) return p;
		p.forwardMass = p.reverseMass = 1.f;
		p.adjointWeight = ior * ior;
		p.valid = true;
		return p;
	}
	float3 h = o + ior * d;
	const float length2 = dot(h, h);
	if (!(length2 > 0.f)) return p;
	h /= sqrt(length2);
	if (h.z < 0.f) h = -h;
	const float co = dot(o, h), ci = dot(d, h);
	if (!(co > 0.f && ci < 0.f)) return p;
	const float a2 = alpha * alpha;
	// Stable GGX form near the normal: do not subtract almost-equal floats.
	const float ndfDenom = h.x * h.x + h.y * h.y + a2 * h.z * h.z;
	const float D = a2 / (CYCLES_BSSRDF_BOUNDARY_PI * ndfDenom * ndfDenom);
	const float go = CyclesBSSRDF_BoundaryG1(o, alpha);
	const float gi = CyclesBSSRDF_BoundaryG1(d, alpha);
	const float denominator = co + ior * ci;
	const float inverseDenom2 = 1.f / (denominator * denominator);
	// Walter et al. 2007, eq. 17: half-normal -> refracted solid angle.
	p.forwardPdf = D * go * co / o.z * ior * ior * (-ci) * inverseDenom2;
	p.reversePdf = D * gi * (-ci) / (-d.z) * co * inverseDenom2;
	// Transpose of the Cycles directional-only entry kernel, not a second
	// Fresnel/Smith correction to its original eye-side throughput.
	p.adjointWeight = ior * ior * go / gi;
	p.valid = isfinite(p.forwardPdf) && isfinite(p.reversePdf) && isfinite(p.adjointWeight) &&
			p.forwardPdf > 0.f && p.reversePdf > 0.f;
	return p;
}

OPENCL_FORCE_INLINE CyclesBSSRDFBoundarySample CyclesBSSRDF_SampleEntryBoundary(
		const float3 outside, const float ior, const float alpha, const float u0, const float u1) {
	CyclesBSSRDFBoundarySample s;
	const float3 h = CyclesBSSRDF_GGX(outside, alpha, u0, u1);
	const float eta = 1.f / ior, cosHI = dot(h, outside);
	const float cosHT = sqrt(fmax(0.f, 1.f - eta * eta * (1.f - cosHI * cosHI)));
	// Keep the original unnormalised refraction expression. The transport
	// caller transforms/normalises it exactly once and tests geometry sides.
	s.direction = -eta * outside + (eta * cosHI - cosHT) * h;
	s.valid = outside.z > 0.f && ior > 1.f && alpha >= 0.f;
	s.densities = CyclesBSSRDF_BoundaryDensities(outside, s.direction, ior, alpha);
	return s;
}

OPENCL_FORCE_INLINE CyclesBSSRDFBoundarySample CyclesBSSRDF_SampleAdjointBoundary(
		const float3 inside, const float ior, const float alpha, const float u0, const float u1) {
	CyclesBSSRDFBoundarySample s;
	s.direction = MAKE_FLOAT3(0.f, 0.f, 0.f);
	s.valid = false;
	s.densities.forwardPdf = s.densities.reversePdf = s.densities.adjointWeight = 0.f;
	s.densities.forwardMass = s.densities.reverseMass = 0.f;
	s.densities.delta = alpha == 0.f;
	s.densities.valid = false;
	if (!(inside.z < 0.f && ior > 1.f && alpha >= 0.f)) return s;
	const float3 h = CyclesBSSRDF_GGX(-inside, alpha, u0, u1);
	const float ci = -dot(inside, h);
	const float transmission = 1.f - ior * ior * (1.f - ci * ci);
	// Total internal reflection has zero support in the transpose of the
	// entry-only Cycles kernel. Keep its rejected probability mass.
	if (!(transmission >= 0.f)) return s;
	s.direction = -ior * inside + (sqrt(transmission) - ior * ci) * h;
	s.densities = CyclesBSSRDF_BoundaryDensities(s.direction, inside, ior, alpha);
	s.valid = s.direction.z > 0.f && s.densities.valid;
	return s;
}

#endif

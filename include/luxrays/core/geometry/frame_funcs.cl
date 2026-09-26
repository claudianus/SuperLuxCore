#line 2 "frame_funcs.cl"

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

// A shading frame must never contain NaN/Inf: never let a non-finite
// or zero normal reach CoordinateSystem
OPENCL_FORCE_INLINE float3 Frame_SanitizeNormal(const float3 z) {
	const float zl2 = dot(z, z);
	return (isfinite(zl2) && (zl2 > 1e-20f)) ? z : MAKE_FLOAT3(0.f, 0.f, 1.f);
}

// A degenerate tangent (x parallel to z, e.g. dpdu collapsing to a zero
// vector) or non-finite input normalizes to NaN and poisons every BSDF
// evaluation done through the frame. Fall back to an arbitrary
// orthonormal basis around a sanitized normal instead.
OPENCL_FORCE_INLINE void Frame_BuildAxes(const float3 x, const float3 z,
		float3 *X, float3 *Y, float3 *Z) {
	const float3 y0 = cross(z, x);
	const float yl2 = dot(y0, y0);
	if (isfinite(yl2) && (yl2 > 1e-20f)) {
		*Z = z;
		*Y = normalize(y0);
		*X = cross(*Y, *Z);
	} else {
		*Z = Frame_SanitizeNormal(z);
		CoordinateSystem(*Z, X, Y);
	}
}

OPENCL_FORCE_INLINE void Frame_Set(__global Frame *frame, const float3 x, const float3 y, const float3 z) {
	float3 X, Y, Z;
	Frame_BuildAxes(x, z, &X, &Y, &Z);

	VSTORE3F(X, &frame->X.x);
	VSTORE3F(Y, &frame->Y.x);
	VSTORE3F(Z, &frame->Z.x);
}

OPENCL_FORCE_INLINE void Frame_Set_Private(Frame *frame, const float3 x, const float3 y, const float3 z) {
	float3 X, Y, Z;
	Frame_BuildAxes(x, z, &X, &Y, &Z);

	frame->X.x = X.x;
	frame->X.y = X.y;
	frame->X.z = X.z;
	frame->Y.x = Y.x;
	frame->Y.y = Y.y;
	frame->Y.z = Y.z;
	frame->Z.x = Z.x;
	frame->Z.y = Z.y;
	frame->Z.z = Z.z;
}

OPENCL_FORCE_INLINE void Frame_SetFromZ(__global Frame *frame, const float3 Z) {
	float3 X, Y;
	const float3 sZ = Frame_SanitizeNormal(Z);
	CoordinateSystem(sZ, &X, &Y);

	VSTORE3F(X, &frame->X.x);
	VSTORE3F(Y, &frame->Y.x);
	VSTORE3F(sZ, &frame->Z.x);
}

OPENCL_FORCE_INLINE void Frame_SetFromZ_Private(Frame *frame, const float3 Z)
{
	float3 X, Y;
	const float3 sZ = Frame_SanitizeNormal(Z);
	CoordinateSystem(sZ, &X, &Y);

	frame->X.x = X.x;
	frame->X.y = X.y;
	frame->X.z = X.z;
	frame->Y.x = Y.x;
	frame->Y.y = Y.y;
	frame->Y.z = Y.z;
	frame->Z.x = sZ.x;
	frame->Z.y = sZ.y;
	frame->Z.z = sZ.z;
}

OPENCL_FORCE_INLINE float3 ToWorld(const float3 X, const float3 Y, const float3 Z, const float3 v) {
	return X * v.x + Y * v.y + Z * v.z;
}

OPENCL_FORCE_INLINE float3 ToLocal(const float3 X, const float3 Y, const float3 Z, const float3 a) {
	return MAKE_FLOAT3(dot(a, X), dot(a, Y), dot(a, Z));
}

OPENCL_FORCE_INLINE float3 Frame_ToWorld(__global const Frame* restrict frame, const float3 v) {
	return ToWorld(VLOAD3F(&frame->X.x), VLOAD3F(&frame->Y.x), VLOAD3F(&frame->Z.x), v);
}

OPENCL_FORCE_INLINE float3 Frame_ToWorld_Private(const Frame *frame, const float3 v) {
	return ToWorld(
			MAKE_FLOAT3(frame->X.x, frame->X.y, frame->X.z),
			MAKE_FLOAT3(frame->Y.x, frame->Y.y, frame->Y.z),
			MAKE_FLOAT3(frame->Z.x, frame->Z.y, frame->Z.z), v);
}

OPENCL_FORCE_INLINE float3 Frame_ToLocal(__global const Frame* restrict frame, const float3 v) {
	return ToLocal(VLOAD3F(&frame->X.x), VLOAD3F(&frame->Y.x), VLOAD3F(&frame->Z.x), v);
}

OPENCL_FORCE_INLINE float3 Frame_ToLocal_Private(const Frame *frame, const float3 v) {
	return ToLocal(
			MAKE_FLOAT3(frame->X.x, frame->X.y, frame->X.z),
			MAKE_FLOAT3(frame->Y.x, frame->Y.y, frame->Y.z),
			MAKE_FLOAT3(frame->Z.x, frame->Z.y, frame->Z.z), v);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

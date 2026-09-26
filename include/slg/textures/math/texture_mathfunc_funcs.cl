#line 2 "texture_mathfunc_funcs.cl"

/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
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

//------------------------------------------------------------------------------
// Generic math-function texture (trig/exp/log)
//
// Op values mirror slg::MathFuncOp in include/slg/textures/math/mathfunc.h —
// keep them in sync.
//------------------------------------------------------------------------------

#define MATHFUNC_OP_SIN   0
#define MATHFUNC_OP_COS   1
#define MATHFUNC_OP_TAN   2
#define MATHFUNC_OP_ASIN  3
#define MATHFUNC_OP_ACOS  4
#define MATHFUNC_OP_ATAN  5
#define MATHFUNC_OP_ATAN2 6
#define MATHFUNC_OP_EXP   7
#define MATHFUNC_OP_LN    8
#define MATHFUNC_OP_SINH  9
#define MATHFUNC_OP_COSH  10
#define MATHFUNC_OP_TANH  11
#define MATHFUNC_OP_INVSQRT 12
#define MATHFUNC_OP_FLOORMOD 13

OPENCL_FORCE_INLINE bool MathFuncTexture_IsBinary(const uint op) {
	return op == MATHFUNC_OP_ATAN2 || op == MATHFUNC_OP_FLOORMOD;
}

OPENCL_FORCE_INLINE float MathFuncTexture_Apply(const uint op,
		const float a, const float b) {
	switch (op) {
		case MATHFUNC_OP_SIN: return sin(a);
		case MATHFUNC_OP_COS: return cos(a);
		case MATHFUNC_OP_TAN: return tan(a);
		// Clamp asin/acos inputs to their domain so out-of-range shading
		// values degrade to the boundary instead of producing NaN
		case MATHFUNC_OP_ASIN: return asin(clamp(a, -1.f, 1.f));
		case MATHFUNC_OP_ACOS: return acos(clamp(a, -1.f, 1.f));
		case MATHFUNC_OP_ATAN: return atan(a);
		case MATHFUNC_OP_ATAN2: return atan2(a, b);
		case MATHFUNC_OP_EXP: return exp(a);
		case MATHFUNC_OP_LN: return log(max(a, 1e-9f));
		case MATHFUNC_OP_SINH: return sinh(a);
		case MATHFUNC_OP_COSH: return cosh(a);
		case MATHFUNC_OP_TANH: return tanh(a);
		case MATHFUNC_OP_INVSQRT: return 1.f / sqrt(max(a, 1e-9f));
		// Floored modulo (Blender FLOORMOD); guard against mod 0
		case MATHFUNC_OP_FLOORMOD: return (b == 0.f) ? 0.f :
				a - b * floor(a / b);
		default: return 0.f;
	}
}

OPENCL_FORCE_NOT_INLINE void MathFuncTexture_EvalOp(
		__global const Texture* restrict texture,
		const TextureEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint,
		const float sampleDistance
		TEXTURES_PARAM_DECL) {
	const uint op = texture->mathFuncTex.op;

	switch (evalType) {
		case EVAL_FLOAT: {
			// Operand evals run tex1 then tex2, so tex2 sits on top
			float tex2 = 0.f;
			if (MathFuncTexture_IsBinary(op)) {
				EvalStack_PopFloat(tex2);
			}
			float tex1;
			EvalStack_PopFloat(tex1);

			const float eval = MathFuncTexture_Apply(op, tex1, tex2);
			EvalStack_PushFloat(eval);
			break;
		}
		case EVAL_SPECTRUM: {
			float3 tex2 = 0.f;
			if (MathFuncTexture_IsBinary(op)) {
				EvalStack_PopFloat3(tex2);
			}
			float3 tex1;
			EvalStack_PopFloat3(tex1);

			const float3 eval = (float3)(
					MathFuncTexture_Apply(op, tex1.x, tex2.x),
					MathFuncTexture_Apply(op, tex1.y, tex2.y),
					MathFuncTexture_Apply(op, tex1.z, tex2.z));
			EvalStack_PushFloat3(eval);
			break;
		}
		case EVAL_BUMP_GENERIC_OFFSET_U:
			Texture_EvalOpGenericBumpOffsetU(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP_GENERIC_OFFSET_V:
			Texture_EvalOpGenericBumpOffsetV(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP:
			Texture_EvalOpGenericBump(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

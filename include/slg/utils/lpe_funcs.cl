#line 2 "lpe_funcs.cl"

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

// LPE NFA evaluation - device twin of the inline helpers in lpe.h.
// A path carries one live-state bitmask per expression
// (EyePathInfo::lpeStates), stepped once per vertex event and once at
// the terminal symbol; acceptance routes the contribution into the
// matching lpeRadiance slot.

// Function-parameter plumbing for the automaton table. lpeCount is a
// KERNEL_ARGS_FILM kernel arg, lpeAutomata the uploaded table buffer.
#define LPE_PARAM_DECL \
		, __global const LPEAutomaton* restrict lpeAutomata, const uint lpeCount
#define LPE_PARAM , lpeAutomata, lpeCount

// Position of a single live bit (clz is not in the cl2msl function map)
OPENCL_FORCE_INLINE uint LPE_BitIndex(uint lsb) {
	uint i = 0;
	while (lsb >>= 1u)
		++i;
	return i;
}

// NFA state-set step: OR the delta rows of every live state (lpe.h LPEStep)
OPENCL_FORCE_INLINE uint LPE_Step(__global const LPEAutomaton* restrict aut,
		const uint states, const uint sym) {
	uint out = 0;
	for (uint m = states; m; m &= m - 1u) {
		const uint lsb = m & (~m + 1u);
		out |= aut->delta[LPE_BitIndex(lsb)][sym];
	}
	return out;
}

// Accept check after a terminal step (lpe.h LPEAccept)
OPENCL_FORCE_INLINE uint LPE_Accept(__global const LPEAutomaton* restrict aut,
		const uint states, const uint sym) {
	return (LPE_Step(aut, states, sym) & aut->acceptMask) ? 1u : 0u;
}

// BSDFEvent + volume flag -> concrete vertex symbol. Multi-class events
// pick the hardest class (SPECULAR > GLOSSY > DIFFUSE) - the severity
// order of the specular-path bookkeeping (lpe.h LPEVertexEvent).
OPENCL_FORCE_INLINE uint LPE_VertexEvent(const BSDFEvent event, const bool fromVolume) {
	if (fromVolume)
		return LPE_SYM_V;
	const bool t = (event & TRANSMIT) != 0;
	if (event & SPECULAR)
		return t ? LPE_SYM_ST : LPE_SYM_SR;
	if (event & GLOSSY)
		return t ? LPE_SYM_GT : LPE_SYM_GR;
	return t ? LPE_SYM_DT : LPE_SYM_DR;
}

// Terminal evaluation over every expression: bitmask of accepting
// automata (EyePathInfo::LPEAcceptMask)
OPENCL_FORCE_INLINE uint LPE_AcceptMask(__global const EyePathInfo* restrict pathInfo,
		__global const LPEAutomaton* restrict lpeAutomata,
		const uint lpeCount, const uint sym) {
	uint accept = 0;
	for (uint i = 0; i < lpeCount; ++i)
		accept |= LPE_Accept(&lpeAutomata[i], pathInfo->lpeStates[i], sym) << i;
	return accept;
}

// Route a terminal contribution into every accepted expression's
// channel (PathTracer::AccumulateLPE)
OPENCL_FORCE_INLINE void LPE_Accumulate(__global SampleResult *sampleResult,
		__global const EyePathInfo* restrict pathInfo,
		const uint sym, const float3 r
		LPE_PARAM_DECL) {
	if (!lpeAutomata || !lpeCount)
		return;
	const uint accept = LPE_AcceptMask(pathInfo, lpeAutomata, lpeCount, sym);
	for (uint i = 0; i < lpeCount; ++i)
		if (accept & (1u << i))
			VADD3F(sampleResult->lpeRadiance[i].c, r);
}

// Next-event terminal: a light connection at the current vertex carries
// the path C v1..vN L - vN's own event must step before the terminal
// (EyePathInfo::LPEAcceptMask(vSym, termSym); AddVertex runs after NEE)
OPENCL_FORCE_INLINE uint LPE_AcceptMaskVertex(__global const EyePathInfo* restrict pathInfo,
		__global const LPEAutomaton* restrict lpeAutomata,
		const uint lpeCount, const uint vSym, const uint termSym) {
	uint accept = 0;
	for (uint i = 0; i < lpeCount; ++i)
		accept |= LPE_Accept(&lpeAutomata[i],
				LPE_Step(&lpeAutomata[i], pathInfo->lpeStates[i], vSym), termSym) << i;
	return accept;
}

OPENCL_FORCE_INLINE void LPE_AccumulateVertex(__global SampleResult *sampleResult,
		__global const EyePathInfo* restrict pathInfo,
		const uint vSym, const uint termSym, const float3 r
		LPE_PARAM_DECL) {
	if (!lpeAutomata || !lpeCount)
		return;
	const uint accept = LPE_AcceptMaskVertex(pathInfo, lpeAutomata, lpeCount, vSym, termSym);
	for (uint i = 0; i < lpeCount; ++i)
		if (accept & (1u << i))
			VADD3F(sampleResult->lpeRadiance[i].c, r);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

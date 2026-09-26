/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// Light Path Expressions (LPE): each registered expression is compiled
// to a small NFA; a path carries the set of live NFA states as a u32
// bitmask per expression, stepped once per path vertex event and once
// at the terminal (emission / direct-light / env / miss) evaluation.
// Fixed-size tables, GPU friendly (see lpe_funcs.cl for the twin).

#ifndef _SLG_LPE_H
#define	_SLG_LPE_H

#include <string>
#include <vector>

#include <boost/serialization/array_wrapper.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>

#include "luxrays/luxrays.h"
#include "slg/bsdf/bsdfevents.h"

namespace slg {

// Concrete path symbols. Vertex events are (class x direction)
// products; terminals and the camera start are their own symbols.
// The numeric values are part of the GPU contract (lpe_funcs.cl).
enum LPESymbol {
	LPE_SYM_DR = 0,	// diffuse reflect vertex
	LPE_SYM_DT,		// diffuse transmit vertex
	LPE_SYM_GR,		// glossy reflect vertex
	LPE_SYM_GT,		// glossy transmit vertex
	LPE_SYM_SR,		// specular reflect vertex
	LPE_SYM_ST,		// specular transmit vertex
	LPE_SYM_V,		// volume scattering vertex
	LPE_SYM_L,		// terminal: emitter hit / next-event estimation
	LPE_SYM_E,		// terminal: environment/infinite light
	LPE_SYM_B,		// terminal: miss (background, no env light)
	LPE_SYM_C,		// camera start symbol
	LPE_NUM_SYMBOLS
};

#define SLG_LPE_MAX_STATES 32
#define SLG_LPE_MAX_EXPRESSIONS 8

// Compiled NFA: delta[s][sym] is the epsilon-closed next-state set for
// a single source state s; startAfterC is the init value for a fresh
// eye path (the start set already stepped by the camera symbol).
struct LPEAutomaton {
	u_int startMask;
	u_int startAfterC;
	u_int acceptMask;
	u_int delta[SLG_LPE_MAX_STATES][LPE_NUM_SYMBOLS];

	friend class boost::serialization::access;
	template<class Archive> void serialize(Archive &ar, const unsigned int) {
		ar & startMask;
		ar & startAfterC;
		ar & acceptMask;
		ar & boost::serialization::make_array(&delta[0][0],
				SLG_LPE_MAX_STATES * LPE_NUM_SYMBOLS);
	}
};

// One registered expression: compiled automaton + output channel name.
struct LPEExpression {
	std::string expression;
	std::string name;
	LPEAutomaton automaton;

	friend class boost::serialization::access;
	template<class Archive> void serialize(Archive &ar, const unsigned int) {
		ar & expression;
		ar & name;
		ar & automaton;
	}
};

// NFA state-set step: OR the delta rows of every live state.
inline u_int LPEStep(const LPEAutomaton &aut, const u_int states, const u_int sym) {
	u_int out = 0;
	for (u_int m = states; m; m &= m - 1) {
		const u_int s = (u_int)__builtin_ctz(m);
		out |= aut.delta[s][sym];
	}
	return out;
}

// Accept check after a terminal step.
inline u_int LPEAccept(const LPEAutomaton &aut, const u_int states, const u_int sym) {
	return (LPEStep(aut, states, sym) & aut.acceptMask) ? 1u : 0u;
}

// BSDFEvent + volume flag -> concrete vertex symbol. Multi-class
// events pick the hardest class (SPECULAR > GLOSSY > DIFFUSE), the
// same severity order used by the specular-path bookkeeping.
inline u_int LPEVertexEvent(const BSDFEvent event, const bool fromVolume) {
	if (fromVolume)
		return LPE_SYM_V;
	const bool t = (event & TRANSMIT) != 0;
	if (event & SPECULAR)
		return t ? LPE_SYM_ST : LPE_SYM_SR;
	if (event & GLOSSY)
		return t ? LPE_SYM_GT : LPE_SYM_GR;
	return t ? LPE_SYM_DT : LPE_SYM_DR;
}

// Compiles an LPE string into an automaton. Grammar:
//   expr := alt ;  alt := seq ('|' seq)* ;  seq := rep+
//   rep  := atom ('*'|'+'|'?')? ;  atom := '(' alt ')' | '.' | '<'preds'>' | char
// char preds: C L E B (self), D G S (class), R T (direction), V (volume).
// '<'preds'>' is a single event matching ALL listed predicates
// (<RD> = diffuse reflect); '.' matches any vertex event.
// Throws std::runtime_error with the token position on syntax errors
// and when the NFA exceeds SLG_LPE_MAX_STATES states.
LPEAutomaton LPECompileExpression(const std::string &expr);

}

#endif	/* _SLG_LPE_H */

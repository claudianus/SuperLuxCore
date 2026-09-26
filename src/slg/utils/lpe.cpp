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

#include <algorithm>
#include <stdexcept>

#include "slg/utils/lpe.h"

using namespace std;
using namespace luxrays;
using namespace slg;

namespace {

// Bitmask over the LPE_NUM_SYMBOLS concrete symbols.
const u_int LPE_VERTEX_MASK =
		(1u << LPE_SYM_DR) | (1u << LPE_SYM_DT) |
		(1u << LPE_SYM_GR) | (1u << LPE_SYM_GT) |
		(1u << LPE_SYM_SR) | (1u << LPE_SYM_ST) | (1u << LPE_SYM_V);

u_int PredMask(const char c) {
	switch (c) {
		case 'C': return 1u << LPE_SYM_C;
		case 'L': return 1u << LPE_SYM_L;
		case 'E': return 1u << LPE_SYM_E;
		case 'B': return 1u << LPE_SYM_B;
		case 'V': return 1u << LPE_SYM_V;
		case 'D': return (1u << LPE_SYM_DR) | (1u << LPE_SYM_DT);
		case 'G': return (1u << LPE_SYM_GR) | (1u << LPE_SYM_GT);
		case 'S': return (1u << LPE_SYM_SR) | (1u << LPE_SYM_ST);
		case 'R': return (1u << LPE_SYM_DR) | (1u << LPE_SYM_GR) | (1u << LPE_SYM_SR);
		case 'T': return (1u << LPE_SYM_DT) | (1u << LPE_SYM_GT) | (1u << LPE_SYM_ST);
		default: return 0;
	}
}

struct NFAState {
	// trans[sym] != 0 marks a transition on that concrete symbol to target
	u_int transMask;
	int transTarget;
	vector<int> eps;
	NFAState() : transMask(0), transTarget(-1) { }
};

struct Frag {
	int start;
	vector<int> outs;
};

struct Builder {
	vector<NFAState> st;
	const string &src;

	Builder(const string &s) : src(s) { }

	int NewState() {
		st.push_back(NFAState());
		return (int)st.size() - 1;
	}

	void LinkOuts(const vector<int> &outs, const int target) {
		for (const int o : outs)
			st[o].eps.push_back(target);
	}

	Frag Parse(size_t &pos) {
		Frag f = ParseAlt(pos);
		if (pos != src.size())
			throw runtime_error("LPE: unexpected token '" + string(1, src[pos]) +
					"' at position " + to_string(pos));
		return f;
	}

	Frag ParseAlt(size_t &pos) {
		Frag f = ParseSeq(pos);
		while (pos < src.size() && src[pos] == '|') {
			++pos;
			Frag g = ParseSeq(pos);
			const int s = NewState();
			st[s].eps.push_back(f.start);
			st[s].eps.push_back(g.start);
			f.outs.insert(f.outs.end(), g.outs.begin(), g.outs.end());
			f.start = s;
		}
		return f;
	}

	Frag ParseSeq(size_t &pos) {
		Frag f = ParseRep(pos);
		while (pos < src.size() && src[pos] != ')' && src[pos] != '|') {
			Frag g = ParseRep(pos);
			LinkOuts(f.outs, g.start);
			f.outs = g.outs;
		}
		return f;
	}

	Frag ParseRep(size_t &pos) {
		Frag f = ParseAtom(pos);
		if (pos < src.size() && (src[pos] == '*' || src[pos] == '+' || src[pos] == '?')) {
			const char q = src[pos++];
			const int s = NewState();
			if (q == '?') {
				st[s].eps.push_back(f.start);
				f.outs.push_back(s);
				f.start = s;
			} else {
				// Loop: after the fragment we may repeat or exit via s
				st[s].eps.push_back(f.start);
				LinkOuts(f.outs, s);
				if (q == '*') {
					f.outs = { s };
					f.start = s;
				} else // '+': enter through the fragment once, exit via s
					f.outs = { s };
			}
		}
		return f;
	}

	Frag ParseAtom(size_t &pos) {
		if (pos >= src.size())
			throw runtime_error("LPE: unexpected end of expression");
		const char c = src[pos];
		Frag f;
		if (c == '(') {
			++pos;
			f = ParseAlt(pos);
			if (pos >= src.size() || src[pos] != ')')
				throw runtime_error("LPE: missing ')' at position " + to_string(pos));
			++pos;
			return f;
		}
		u_int mask;
		if (c == '.') {
			++pos;
			mask = LPE_VERTEX_MASK;
		} else if (c == '<') {
			++pos;
			mask = ~0u;
			bool any = false;
			while (pos < src.size() && src[pos] != '>') {
				const u_int p = PredMask(src[pos]);
				if (!p)
					throw runtime_error("LPE: invalid predicate '" + string(1, src[pos]) +
							"' at position " + to_string(pos));
				mask &= p;
				any = true;
				++pos;
			}
			if (pos >= src.size())
				throw runtime_error("LPE: missing '>' at position " + to_string(pos));
			++pos;
			if (!any || !(mask & ((1u << LPE_NUM_SYMBOLS) - 1)))
				throw runtime_error("LPE: empty or contradictory predicate set at position " + to_string(pos));
		} else {
			mask = PredMask(c);
			if (!mask)
				throw runtime_error("LPE: invalid symbol '" + string(1, c) +
						"' at position " + to_string(pos));
			++pos;
		}
		// One NFA edge per matcher (transMask fans out over concrete symbols)
		const int s = NewState(), a = NewState();
		st[s].transMask = mask;
		st[s].transTarget = a;
		f.start = s;
		f.outs = { a };
		return f;
	}
};

void EpsClosure(const vector<NFAState> &st, const int s, u_int &set, vector<bool> &vis) {
	if (vis[s])
		return;
	vis[s] = true;
	set |= 1u << s;
	for (const int t : st[s].eps)
		EpsClosure(st, t, set, vis);
}

} // anonymous namespace

LPEAutomaton slg::LPECompileExpression(const string &expr) {
	if (expr.empty())
		throw runtime_error("LPE: empty expression");

	Builder b(expr);
	// Accept node: dedicated, index last
	size_t pos = 0;
	Frag f = b.Parse(pos);
	const int accept = b.NewState();
	b.LinkOuts(f.outs, accept);

	const int n = (int)b.st.size();
	if (n > SLG_LPE_MAX_STATES)
		throw runtime_error("LPE: expression needs " + to_string(n) +
				" NFA states (max " + to_string(SLG_LPE_MAX_STATES) + ")");

	auto closureOf = [&](const vector<int> &states) {
		u_int set = 0;
		vector<bool> vis(n, false);
		for (const int s : states)
			EpsClosure(b.st, s, set, vis);
		return set;
	};
	auto closure1 = [&](const int s) { return closureOf({ s }); };

	LPEAutomaton aut;
	aut.startMask = closure1(f.start);
	aut.acceptMask = 1u << accept;
	memset(aut.delta, 0, sizeof(aut.delta));
	for (int s = 0; s < n; ++s) {
		if (!b.st[s].transMask)
			continue;
		const u_int tgt = closure1(b.st[s].transTarget);
		for (u_int sym = 0; sym < LPE_NUM_SYMBOLS; ++sym)
			if (b.st[s].transMask & (1u << sym))
				aut.delta[s][sym] = tgt;
	}

	// Init value for a fresh eye path: start set stepped by the camera
	// symbol (paths not starting with C end up with an empty set).
	aut.startAfterC = LPEStep(aut, aut.startMask, LPE_SYM_C);

	return aut;
}

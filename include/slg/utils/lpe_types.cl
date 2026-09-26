#line 2 "lpe_types.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   This file is part of LuxCore.                                         *
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

// Light Path Expressions (LPE) - device twin of include/slg/utils/lpe.h.
// The host enum LPESymbol carries the same numeric values (the contract
// is the compiled automaton's delta table, uploaded as this POD).

#if defined(SLG_OPENCL_KERNEL)

// Symbol ids (values are part of the ABI with the host LPE compiler):
// 0..5 = vertex classes diffuse/glossy/specular x reflect/transmit,
// 6 = volume scattering, 7 = terminal emitter/NEE, 8 = terminal
// environment/infinite, 9 = terminal miss (background), 10 = camera start.
// NOTE: keep comments out of the #define values - a trailing // is part
// of the macro expansion and swallows the rest of the line at use sites.
#define LPE_SYM_DR 0
#define LPE_SYM_DT 1
#define LPE_SYM_GR 2
#define LPE_SYM_GT 3
#define LPE_SYM_SR 4
#define LPE_SYM_ST 5
#define LPE_SYM_V 6
#define LPE_SYM_L 7
#define LPE_SYM_E 8
#define LPE_SYM_B 9
#define LPE_SYM_C 10
#define LPE_NUM_SYMBOLS 11

#define SLG_LPE_MAX_STATES 32
#define SLG_LPE_MAX_EXPRESSIONS 8

// Compiled NFA, layout-identical to slg::LPEAutomaton (host fills the
// buffer straight from Film::lpeAutomata).
typedef struct {
	unsigned int startMask;
	unsigned int startAfterC;
	unsigned int acceptMask;
	unsigned int delta[SLG_LPE_MAX_STATES][LPE_NUM_SYMBOLS];
} LPEAutomaton;

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

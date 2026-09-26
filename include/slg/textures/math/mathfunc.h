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

#ifndef _SLG_MATHFUNCTEX_H
#define	_SLG_MATHFUNCTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// Generic math-function texture (trig/exp/log), driven by an `op` enum.
//
// The enum values are mirrored by the OpenCL side in
// include/slg/textures/math/texture_mathfunc_funcs.cl — keep them in sync.
//------------------------------------------------------------------------------

typedef enum {
	MATHFUNC_SIN = 0,
	MATHFUNC_COS,
	MATHFUNC_TAN,
	MATHFUNC_ASIN,
	MATHFUNC_ACOS,
	MATHFUNC_ATAN,
	MATHFUNC_ATAN2,	// binary: atan2(tex1, tex2)
	MATHFUNC_EXP,
	MATHFUNC_LN,
	MATHFUNC_SINH,
	MATHFUNC_COSH,
	MATHFUNC_TANH,
	MATHFUNC_INVSQRT,
	MATHFUNC_FLOORMOD	// binary: floored modulo tex1 mod tex2
} MathFuncOp;

inline bool MathFuncIsBinary(MathFuncOp o) {
	return o == MATHFUNC_ATAN2 || o == MATHFUNC_FLOORMOD;
}

class MathFuncTexture : public Texture {
public:
	// tex2 is only evaluated by binary ops; for unary ops pass tex1
	// again (it is stored but never sampled)
	MathFuncTexture(MathFuncOp o, TextureRef t1, TextureRef t2) :
			op(o), tex1(t1), tex2(t2) { }
	virtual ~MathFuncTexture() { }

	virtual TextureType GetType() const { return MATHFUNC_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const { return GetFloatValue(HitPoint()); } // Approximation
	virtual float Filter() const { return Y(); } // Approximation

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
		Texture::AddReferencedTextures(referencedTexs);

		tex1.get().AddReferencedTextures(referencedTexs);
		if (MathFuncIsBinary(op))
			tex2.get().AddReferencedTextures(referencedTexs);
	}
	virtual void AddReferencedImageMaps(std::unordered_set<const ImageMap * > &referencedImgMaps) const {
		tex1.get().AddReferencedImageMaps(referencedImgMaps);
		if (MathFuncIsBinary(op))
			tex2.get().AddReferencedImageMaps(referencedImgMaps);
	}

	virtual void UpdateTextureReferences(TextureRef oldTex, TextureRef newTex) {
		updtex(tex1, oldTex, newTex);
		if (MathFuncIsBinary(op))
			updtex(tex2, oldTex, newTex);
	}

	MathFuncOp GetOp() const { return op; }
	TextureConstRef GetTexture1() const { return tex1; }
	TextureConstRef GetTexture2() const { return tex2; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

	static const char *OpToString(MathFuncOp o);

private:
	MathFuncOp op;
	std::reference_wrapper<Texture> tex1, tex2;
};

}

#endif	/* _SLG_MATHFUNCTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

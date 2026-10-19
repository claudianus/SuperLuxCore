// SPDX-License-Identifier: Apache-2.0
#ifndef SLG_BIDIR_MEASURE_H
#define SLG_BIDIR_MEASURE_H

namespace slg {

// Infinite-light emission PDFs are defined on a projected disk. The
// virtual enclosing sphere supplies visibility endpoints, not a second
// projected-area cosine. All bidirectional strategies must agree.
inline float BiDirEmissionCosine(const bool environmental, const float cosine) {
	return environmental ? 1.f : cosine;
}

}
#endif

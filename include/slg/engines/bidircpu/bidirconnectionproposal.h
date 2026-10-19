// SPDX-License-Identifier: Apache-2.0
#ifndef SLG_BIDIR_CONNECTION_PROPOSAL_H
#define SLG_BIDIR_CONNECTION_PROPOSAL_H
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
namespace slg {
// Conditional selection only. The physical connection and its existing MIS
// weight are unchanged; a selected contribution is divided by K*probability.
class BiDirConnectionProposal {
public:
    struct Choice { size_t index; double probability; float remapped; };
    void Build(const std::vector<double> &weights, const bool importance = true) {
        cdf.resize(weights.size());
        if (cdf.empty()) return;
        double maximum = 0.;
        for (double value : weights)
            if (std::isfinite(value) && value > maximum) maximum = value;
        double sum = 0.;
        if (importance && maximum > 0.)
            for (double value : weights)
                if (std::isfinite(value) && value > 0.) sum += value / maximum;
        const double share = sum > 0. ? .9 : 0.;
        double prefix = 0.;
        for (size_t i = 0; i < cdf.size(); ++i) {
            const double weight = std::isfinite(weights[i]) && weights[i] > 0. ? weights[i] : 0.;
            prefix += (1. - share) / cdf.size() + (share > 0. ? share * (weight / maximum) / sum : 0.);
            cdf[i] = prefix;
        }
        cdf.back() = 1.;
    }
    Choice Sample(double u) const {
        if (cdf.empty()) return {0, 0., 0.f};
        u = std::clamp(u, 0., std::nextafter(1., 0.));
        const size_t index = std::upper_bound(cdf.begin(), cdf.end(), u) - cdf.begin();
        const double lower = index ? cdf[index-1] : 0.;
        const double probability = cdf[index] - lower;
        return {index, probability, std::min(.99999994f, float((u - lower) / probability))};
    }
private:
    std::vector<double> cdf;
};
}
#endif

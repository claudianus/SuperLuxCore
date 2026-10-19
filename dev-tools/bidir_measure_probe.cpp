// SPDX-License-Identifier: Apache-2.0
// One-volume-vertex environment path: compare all three strategy weights.
#include "slg/engines/bidircpu/bidirmeasure.h"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <algorithm>
static float square(float x) { return x*x; }
int main() try {
    uint32_t state=17;
    auto random=[&]() { state=1664525u*state+1013904223u;return (state>>8)*(1.f/16777216.f); };
    unsigned negativeFailures=0;double maxError=0,maxLegacyDeficit=0;
    for(unsigned n=0;n<1048576;++n) {
        const float cameraA=std::exp2(-8.f+16.f*random());
        const float phase=.0795774715f,direct=.0795774715f;
        const float radius=.25f+4.f*random(),emission=direct/(3.14159265f*radius*radius);
        const float cosine=.05f+.95f*random();
        if(slg::BiDirEmissionCosine(false,cosine)!=cosine || slg::BiDirEmissionCosine(true,cosine)!=1.f)
            throw std::runtime_error("wrong emission measure");
        // The recurrence's three alternatives, each evaluated on the same path.
        const float eye=1.f/(1.f+square(direct/phase)+square(emission/(phase*cameraA)));
        const float light=1.f/(1.f+square(cameraA*direct/emission)+square(cameraA*phase/emission));
        const float nee=1.f/(1.f+square(phase/direct)+square(emission/(direct*slg::BiDirEmissionCosine(true,cosine)*cameraA)));
        const float legacyNEE=1.f/(1.f+square(phase/direct)+square(emission/(direct*cosine*cameraA)));
        const double sum=double(eye)+light+nee,oldSum=double(eye)+light+legacyNEE;
        maxError=std::max(maxError,std::abs(sum-1.));
        maxLegacyDeficit=std::max(maxLegacyDeficit,1.-oldSum);
        if(std::abs(oldSum-1.)>1e-6)++negativeFailures;
        if(!std::isfinite(sum)||std::abs(sum-1.)>5e-7)
            throw std::runtime_error("bidirectional strategy weights do not partition unity");
    }
    if(negativeFailures<500000)throw std::runtime_error("negative control did not reproduce the defect");
    std::cout<<"{\"path_checks\":1048576,\"max_partition_error\":"<<maxError
        <<",\"legacy_partition_failures\":"<<negativeFailures
        <<",\"max_legacy_deficit\":"<<maxLegacyDeficit<<",\"passed\":true}\n";
} catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 2; }

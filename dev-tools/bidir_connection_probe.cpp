// SPDX-License-Identifier: Apache-2.0
#include "slg/engines/bidircpu/bidirconnectionproposal.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <cmath>
int main()try{
 unsigned long draws=0;double maxRelativeError=0.;unsigned cases=0,negativeFailures=0;
 for(unsigned n:{1u,2u,7u,32u,127u})for(unsigned pattern=0;pattern<4;++pattern){
  std::vector<double> w(n);double oracle=0.;
  for(unsigned i=0;i<n;++i){w[i]=pattern==0?1.:pattern==1?(i?1e-20:1e20):pattern==2?0.:std::pow(2.,double(int(i%19)-9));double cut=.05+.9*(i%13)/13.;oracle+=(1+i%7)*(cut+.2*cut*cut);}
  slg::BiDirConnectionProposal proposal;proposal.Build(w);double oldMean=0.,singleMean=0.;
  for(unsigned k:{1u,4u}){
   const unsigned m=1048576/k;double mean=0.;
   for(unsigned j=0;j<m;++j)for(unsigned s=0;s<k;++s){
    auto choice=proposal.Sample(((j+.5)/m+s)/k);++draws;
    if(choice.index>=n||choice.probability<.099999/n||choice.remapped<0||choice.remapped>=1)throw std::runtime_error("invalid selection support");
    unsigned i=choice.index;double cut=.05+.9*(i%13)/13.;double value=choice.remapped<cut?(1+i%7)*(1+.4*choice.remapped):0.;
    mean+=value/(k*choice.probability);if(k==1)oldMean+=value;
   }
   mean/=m;double err=std::abs(mean-oracle)/oracle;maxRelativeError=std::max(maxRelativeError,err);if(err>.002)throw std::runtime_error("conditional estimator mean mismatch");
   if(k==1)singleMean=mean;else if(std::abs(mean-singleMean)>1e-8*oracle)throw std::runtime_error("stratified budget changed expectation");++cases;
  }
  oldMean/=1048576;if(n>1&&std::abs(oldMean-oracle)>.01*oracle)++negativeFailures;
 }
 if(negativeFailures!=16)throw std::runtime_error("uncorrected negative control failed");
 std::cout<<"{\"cases\":"<<cases<<",\"draws\":"<<draws<<",\"max_relative_integration_error\":"<<maxRelativeError<<",\"uncorrected_failures\":"<<negativeFailures<<",\"passed\":true}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}

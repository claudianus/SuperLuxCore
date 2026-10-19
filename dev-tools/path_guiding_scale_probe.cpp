#include "slg/engines/pathguiding.h"
#include <iostream>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <random>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
using namespace slg;using namespace luxrays;
int main(int argc,char **) {
    if(argc==1)unsetenv("LUX_PG_BIC_LEGACY_MASS");
    else setenv("LUX_PG_BIC_LEGACY_MASS","1",1);
    unsetenv("LUX_PG_NOVA");
    PathGuidingCache::Settings settings;settings.maxLeaves=16;settings.swapRecords=10000000;
    std::unique_ptr<PathGuidingCache> fields[3];const float scales[]={1,.01f,.0001f};
    for(auto &field:fields)field=std::make_unique<PathGuidingCache>(Point(-1,-1,-1),2,settings);
    const unsigned nz=256,np=256;
    for(unsigned j=0;j<nz;++j)for(unsigned k=0;k<np;++k) {
        const float z=2.f*(j+.5f)/nz-1.f,phi=2*M_PI*(k+.5f)/np;
        const Vector d(sqrtf(1-z*z)*cosf(phi),sqrtf(1-z*z)*sinf(phi),z);
        const float h=expf(3*z);
        for(unsigned f=0;f<3;++f)fields[f]->Record(Point(0,0,0),d,h*scales[f]);
    }
    for(auto &field:fields)field->ForceSwap();
    double maxError=0;float peaks[3];
    for(unsigned f=0;f<3;++f)peaks[f]=fields[f]->ReadLeafAt(Point(0,0,0))->peak;
    for(unsigned j=0;j<1024;++j) {
        const float z=2.f*(j+.5f)/1024-1.f;const Vector d(sqrtf(1-z*z),0,z);
        const float reference=fields[0]->Pdf(Point(0,0,0),Normal(0,0,1),d,true);
        for(unsigned f=1;f<3;++f)maxError=std::max(maxError,double(fabs(fields[f]->Pdf(Point(0,0,0),Normal(0,0,1),d,true)-reference)/reference));
    }
    if(maxError>1e-3 || peaks[0]<.5f || peaks[1]<.5f || peaks[2]<.5f) {
        std::cerr<<"Direction fit changes with histogram units or collapses to uniform\n";return 2;
    }
    std::mt19937 rng(9155);auto uniform=[&](){return float((rng()+.5)/4294967296.);};
    double energy[2],errors[2];
    for(unsigned volume=0;volume<2;++volume) {
        double sum=0,sumSq=0;
        const unsigned count=262144;
        for(unsigned i=0;i<count;++i) {
            Vector d;float q;
            if(!fields[0]->Sample(Point(0,0,0),Normal(0,0,1),uniform(),uniform(),uniform(),&d,&q,volume) || !(q>0))return 3;
            const float physical=volume?.25f*INV_PI:Max(d.z,0.f)*INV_PI;
            const double weight=physical/q;sum+=weight;sumSq+=weight*weight;
        }
        energy[volume]=sum/count;errors[volume]=sqrt((sumSq/count-energy[volume]*energy[volume])/count);
        if(fabs(energy[volume]-1)>5*errors[volume])return 4;
    }
    std::cout<<"{\"records_each\":"<<nz*np<<",\"scales\":[1,0.01,0.0001],\"max_relative_pdf_error\":"<<maxError
        <<",\"peaks\":["<<peaks[0]<<','<<peaks[1]<<','<<peaks[2]<<"],\"surface_energy\":"<<energy[0]
        <<",\"volume_energy\":"<<energy[1]<<",\"surface_stderr\":"<<errors[0]<<",\"volume_stderr\":"<<errors[1]
        <<",\"passed\":true}\n";
}

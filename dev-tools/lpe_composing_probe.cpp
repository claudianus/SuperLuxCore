// Exercise the real normal/atomic film splats with no ordinary composing AOV.
#include "slg/slg.h"
#include "slg/film/film.h"
#include "slg/film/sampleresult.h"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace slg;using namespace luxrays;
int main(int argc,char **argv) try {
    const bool legacy=argc>1 && std::string(argv[1])=="legacy";
    slg::Init();unsigned checks=0;
    for(bool composing:{false,true})for(bool live:{false,true}){
        auto film=Film::Create(2,1);film->AddChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED);
        if(composing)film->AddChannel(Film::EMISSION);
        auto props=std::make_unique<Properties>();
        props->SetFromString("film.lpe.0.expression=C.*L\nfilm.lpe.0.name=all\n");
        if(live){film->Init();film->Parse(props);}else{film->Parse(props);film->Init();}
        if(film->GetLPECount()!=1)throw std::runtime_error("LPE configuration missing");
        const Film::FilmChannels channels={Film::RADIANCE_PER_PIXEL_NORMALIZED};
        SampleResult sample;sample.Init(&channels,1,1);
        sample.radiance[0]=Spectrum(2,4,8);sample.lpeRadiance[0]=Spectrum(3,6,9);
        for(unsigned i=0;i<256;++i)film->AddSample(0,0,sample);
        std::vector<std::jthread> threads;
        for(unsigned t=0;t<4;++t)threads.emplace_back([&](){for(unsigned i=0;i<256;++i)film->AtomicAddSample(1,0,sample);});
        threads.clear();float output[6];film->GetOutput(FilmOutputs::LPE,output,0,false);
        const float truth[3]={3,6,9};
        for(unsigned i=0;i<6;++i){
            const float expected=legacy && !composing?0.f:truth[i%3];
            if(output[i]!=expected)throw std::runtime_error("LPE-only normal/atomic splat incorrect");
            ++checks;
        }
    }
    std::cout<<"{\"normal_atomic_live_and_initial_checks\":"<<checks
        <<",\"legacy_negative_control\":"<<(legacy?"true":"false")<<",\"passed\":true}\n";
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 2;}

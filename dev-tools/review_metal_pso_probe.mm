// Cold function-name namespaces avoid reusing the previous run's PSO identity.
// Same prepared MSL and compiler options; only unreachable kernel creation differs.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <vector>
using namespace std;
static double now() { return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=5) return 2;
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    dev.shouldMaximizeConcurrentCompilation=YES;
    NSError *error=nil;
    id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if(!lib) {fprintf(stderr,"library: %s\n",error.localizedDescription.UTF8String);return 3;}
    MTLBinaryArchiveDescriptor *ad=[MTLBinaryArchiveDescriptor new];
    id<MTLBinaryArchive> archive=[dev newBinaryArchiveWithDescriptor:ad error:&error];
    if(!archive)return 4;
    vector<string> names={"Film_Clear","InitSeed","Init","AdvancePaths_MK_RT_NEXT_VERTEX","AdvancePaths_MK_HIT_NOTHING",
      "AdvancePaths_MK_HIT_OBJECT","AdvancePaths_MK_RT_DL","AdvancePaths_MK_RT_RESTIR","AdvancePaths_MK_RT_GI_BOUNCE",
      "AdvancePaths_MK_RT_GI_RESOLVE","AdvancePaths_MK_PT_BOUNCE","AdvancePaths_MK_PT_RESOLVE","AdvancePaths_MK_DL_ILLUMINATE","AdvancePaths_MK_DL_SAMPLE_BSDF",
      "AdvancePaths_MK_MNEE_NEXT_VERTEX","AdvancePaths_MK_GENERATE_NEXT_VERTEX_RAY","AdvancePaths_MK_SPLAT_SAMPLE",
      "AdvancePaths_MK_NEXT_SAMPLE","AdvancePaths_MK_GENERATE_CAMERA_RAY"};
    if(!atoi(argv[3])) names.erase(remove_if(names.begin(),names.end(),[](const string &n){
      return n=="AdvancePaths_MK_RT_RESTIR" || n.find("AdvancePaths_MK_RT_GI_")==0 || n.find("AdvancePaths_MK_PT_")==0;
    }),names.end());
    mutex archiveMutex,logMutex; vector<future<bool>> jobs; double start=now();
    for(auto name:names) jobs.push_back(async(launch::async,[&,name](){ @autoreleasepool {
      double t=now(); string symbol=string(argv[2])+name;
      id<MTLFunction> fn=[lib newFunctionWithName:@(symbol.c_str())];
      MTLComputePipelineDescriptor *pd=[MTLComputePipelineDescriptor new]; pd.computeFunction=fn; pd.label=fn.name;
      pd.binaryArchives=@[archive];
      id<MTLComputePipelineState> pso=[dev newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
      bool hit=pso!=nil; double lookup=now(); NSError *err=nil;
      if(!pso){pd.binaryArchives=nil; pso=[dev newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&err];}
      double built=now(); bool ok=pso!=nil;
      if(ok&&!hit){lock_guard<mutex> guard(archiveMutex);ok=[archive addComputePipelineFunctionsWithDescriptor:pd error:&err];}
      double done=now();
      {lock_guard<mutex> guard(logMutex);printf("{\"kernel\":\"%s\",\"archive_hit\":%s,\"lookup_s\":%.6f,\"pso_s\":%.6f,\"archive_add_wait_s\":%.6f,\"total_s\":%.6f,\"ok\":%s}\n",name.c_str(),hit?"true":"false",lookup-t,built-lookup,done-built,done-t,ok?"true":"false");fflush(stdout);}
      if(!ok)fprintf(stderr,"%s: %s\n",symbol.c_str(),err.localizedDescription.UTF8String);
      [pso release];[pd release];[fn release];return ok;
    }}));
    bool ok=true;for(auto &job:jobs)if(!job.get())ok=false;
    printf("{\"summary\":true,\"kernels\":%zu,\"seconds\":%.6f,\"ok\":%s}\n",names.size(),now()-start,ok?"true":"false");
    [archive release];[ad release];[lib release];[dev release];return ok?0:5;
}}

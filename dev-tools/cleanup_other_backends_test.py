"""Actual Metal/Vulkan cleanup methods with owned handles and queue errors.

Metal uses a small real Objective-C object (not a GPU buffer). Vulkan calls are
stubs. Neither models real in-flight GPU work, device loss or recovery.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method
COMMON=r'''
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace std;string fault;int wrappers=0,releases=0;
struct HardwareDeviceBuffer {virtual ~HardwareDeviceBuffer()=default;};
'''
VK=r'''
using VkDevice=int;using VkBuffer=int;using VkDeviceMemory=int;
void vkDestroyBuffer(int,int,void *){++releases;}
void vkFreeMemory(int,int,void *){++releases;}
struct VulkanDeviceBuffer:HardwareDeviceBuffer {
 int buff,mem;size_t size;
 explicit VulkanDeviceBuffer(bool empty):buff(empty?0:1),mem(empty?0:2),size(empty?0:16){++wrappers;}
 ~VulkanDeviceBuffer(){--wrappers;}
 bool IsNull(){return !buff;}
};
struct VulkanDevice {
 int device=7;size_t used=16;bool cleanupFailed=false;
 void MarkCleanupError()noexcept{cleanupFailed=true;}bool HasCleanupError()const noexcept{return cleanupFailed;}
 void FinishQueue(){if(fault=="queue")throw runtime_error("queue");}
 void FreeMemory(size_t v){used-=v;}
 void FreeBufferForDestruction(HardwareDeviceBuffer **)noexcept;
};
'''
METAL=r'''
#include <objc/runtime.h>
@protocol MTLBuffer
- (void)release;
@end
__attribute__((objc_root_class))
@interface OwnedBuffer <MTLBuffer>
+ (instancetype)new;
- (void)release;
@end
@implementation OwnedBuffer
+ (instancetype)new {return (OwnedBuffer *)class_createInstance(self,0);}
- (void)release {++releases;object_dispose(self);}
@end
struct MetalDeviceBuffer:HardwareDeviceBuffer {
 void *metalBuff;
 explicit MetalDeviceBuffer(bool empty):metalBuff(empty?nullptr:(__bridge void *)[OwnedBuffer new]){++wrappers;}
 ~MetalDeviceBuffer(){--wrappers;}
 bool IsNull(){return !metalBuff;}
 size_t GetSize(){return 16;}
};
struct MetalDevice {
 size_t used=16;bool cleanupFailed=false;
 void MarkCleanupError()noexcept{cleanupFailed=true;}bool HasCleanupError()const noexcept{return cleanupFailed;}
 void FinishQueue(){if(fault=="queue")throw runtime_error("queue");}
 void FreeMemory(size_t v){used-=v;}
 void FreeBufferForDestruction(HardwareDeviceBuffer **)noexcept;
};
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];DEVICE device;
 HardwareDeviceBuffer *buffer=fault=="null"?nullptr:new BUFFER(fault=="empty");
 device.FreeBufferForDestruction(&buffer);assert(buffer==nullptr&&wrappers==0);assert(device.HasCleanupError()==(fault=="queue"));
 device.FreeBufferForDestruction(&buffer);device.FreeBufferForDestruction(nullptr);
 assert(releases==((fault=="null"||fault=="empty")?0:RELEASES));
 if(fault=="success"||fault=="queue")assert(device.used==0);
 puts("PASS release attempts, consumed owner and idempotent cleanup");}
'''
a=argparse.ArgumentParser();a.add_argument('--root',type=Path,default=ROOT);a.add_argument('--out',type=Path,required=True);args=a.parse_args();rows=[]
with tempfile.TemporaryDirectory(prefix='slc-other-cleanup-') as tmp:
 for label,prefix,file,number in [('Metal',METAL,'metaldevice.mm',1),('Vulkan',VK,'vkdevice.cpp',2)]:
  src=Path(tmp)/(label+('.mm' if label=='Metal' else '.cpp'));exe=Path(tmp)/label;body=method((args.root/'src/luxrays/devices'/file).read_text(),'void '+label+'Device::FreeBufferForDestruction(')
  src.write_text(COMMON+prefix+body+MAIN.replace('DEVICE',label+'Device').replace('BUFFER',label+'DeviceBuffer').replace('RELEASES',str(number)))
  command=['clang++','-std=c++20','-fsanitize=address,undefined',str(src),'-o',str(exe)]+(['-lobjc'] if label=='Metal' else [])
  q=subprocess.run(command,capture_output=True,text=True,timeout=40);assert q.returncode==0,q.stderr
  for fault in ['success','null','empty','queue']:
   q=subprocess.run([str(exe),fault],capture_output=True,text=True,timeout=5);rows.append(dict(backend=label,fault=fault,returncode=q.returncode,stderr=q.stderr))
args.out.write_text(json.dumps(dict(scope='production cleanup methods; real mock ObjC lifetime and stub Vulkan; no in-flight GPU/device-loss proof',records=rows),indent=2)+'\n');print([(r['backend'],r['fault'],r['returncode']) for r in rows]);assert all(r['returncode']==0 for r in rows)

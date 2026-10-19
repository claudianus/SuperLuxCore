"""Deterministically interleave the real Metal read/drain method bodies.

Fake command buffers expose Objective-C lifetime errors with NSZombieEnabled.
Only the mutex type is instrumented to park the reader immediately after unlock.
No GPU fault or production debug hook is needed. All compiler outputs are temporary.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / "dev-tools/reviews/2026-10-16/fixes"
source = (ROOT / "src/luxrays/devices/metaldevice.mm").read_text()


def method(name, signature=None):
    start = source.index(signature or "void MetalDevice::" + name + "(")
    begin = source.index("{", start)
    # Method braces are balanced; comments/strings here contain no braces.
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end].replace("std::lock_guard<std::mutex>", "std::lock_guard<GateMutex>")


program = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
std::atomic<int> releasedPrograms{0};
std::atomic<bool> parked{false}, drained{false}, armed{true};
std::thread::id readerId;
struct GateMutex {
    std::mutex mutex;
    void lock() { mutex.lock(); }
    void unlock() {
        mutex.unlock();
        if (std::this_thread::get_id() == readerId && armed.exchange(false)) {
            parked = true;
            while (!drained) std::this_thread::yield();
        }
    }
};
@interface ReviewCB : NSObject
@property(readonly) MTLCommandBufferStatus status;
- (void)waitUntilCompleted;
@end
@implementation ReviewCB
- (MTLCommandBufferStatus)status { return MTLCommandBufferStatusCommitted; }
- (void)waitUntilCompleted { }
@end
@interface ReviewOwned : NSObject
@end
@implementation ReviewOwned
- (void)dealloc { ++releasedPrograms; [super dealloc]; }
@end
@interface ReviewBuffer : NSObject { int value; }
- (void *)contents;
@end
@implementation ReviewBuffer
- (void *)contents { value = 42; return &value; }
@end
using MTLCommandBufferHandle = void *;
struct ObjCRelease { void operator()(void *p) const { [(__bridge id)p release]; } };
using OwnedObjC = std::unique_ptr<void, ObjCRelease>;
struct HardwareDeviceBuffer {
    virtual ~HardwareDeviceBuffer() = default;
    virtual bool IsNull() const { return false; }
};
struct MetalDeviceBuffer : HardwareDeviceBuffer { void *metalBuff; size_t size = sizeof(int); };
struct InFlightDispatch { void *cb; std::vector<const MetalDeviceBuffer *> buffers; };
class MetalDevice {
public:
    GateMutex inFlightMutex;
    std::vector<InFlightDispatch> inFlightWork;
    void CommitPendingLocked() { }
    void EnqueueReadBuffer(const HardwareDeviceBuffer *, bool, size_t, void *);
    void FinishQueue();
};
class MetalDeviceProgram {
public:
    void *library, *binaryArchive;
    bool archiveDirty = false;
    std::string archivePath;
    ~MetalDeviceProgram();
};
'''
program += method("EnqueueReadBuffer") + "\n" + method("FinishQueue")
program += "\n" + method("", "MetalDeviceProgram::~MetalDeviceProgram()")
program += r'''
int main() {
    @autoreleasepool {
        {
            MetalDeviceProgram program;
            program.library = (__bridge void *)[ReviewOwned new];
            program.binaryArchive = (__bridge void *)[ReviewOwned new];
        }
        assert(releasedPrograms == 2);
        MetalDevice d;
        ReviewBuffer *buffer = [ReviewBuffer new];
        MetalDeviceBuffer b; b.metalBuff = (__bridge void *)buffer;
        d.inFlightWork.push_back({(__bridge void *)[ReviewCB new], {&b}});
        int output = 0;
        std::thread reader([&] {
            readerId = std::this_thread::get_id();
            d.EnqueueReadBuffer(&b, true, sizeof(output), &output);
        });
        while (!parked) std::this_thread::yield();
        d.FinishQueue();
        drained = true;
        reader.join();
        [buffer release];
        assert(output == 42);
        puts("PASS concurrent read/drain lifetime; program owning references released");
    }
}
'''
OUT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="slc-review-metal-") as tmp:
    file = Path(tmp) / "lifetime.mm"
    binary = Path(tmp) / "lifetime"
    file.write_text(program)
    p = subprocess.run(["clang++", "-std=c++20", "-fno-objc-arc", str(file), "-framework", "Foundation", "-framework", "Metal", "-o", str(binary)], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stderr
    p = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                       env={**os.environ, "NSZombieEnabled": "YES"})
    tag = "fixed" if "blockerOwner" in method("EnqueueReadBuffer") else "baseline"
    (OUT / f"metal-lifetime-{tag}.log").write_text(p.stdout + p.stderr)
    result = {"returncode": p.returncode, "zombie_message": "deallocated instance" in p.stderr,
              "passed": "PASS concurrent read/drain lifetime" in p.stdout,
              "method_bodies": "actual source with gate-instrumented mutex; fake Objective-C command buffer"}
    (OUT / f"metal-lifetime-{tag}.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    if tag == "fixed": assert result["passed"] and not result["zombie_message"]

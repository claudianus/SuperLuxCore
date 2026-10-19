"""Exercise production CUDA teardown with a deterministic context-stack driver.

No NVIDIA GPU is required. This checks driver-call ownership/order and failure
unwinding, not real CUDA/OptiX execution. Compiler products are always removed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std;
using CUcontext = int;
using CUmodule = int;
using CUresult = int;
using OptixResult = int;
constexpr int CUDA_SUCCESS = 0, OPTIX_SUCCESS = 0;
thread_local vector<int> contextStack;
vector<string> events;
string fault;
int check(const char *op) {
    events.push_back(op);
    if (fault == op) return 99;
    if (contextStack.empty() || contextStack.back() != 7) return 201;
    return 0;
}
int cuCtxPushCurrent(int ctx) {
    events.push_back("push");
    if (fault == "push") return 99;
    contextStack.push_back(ctx); return 0;
}
int cuCtxPopCurrent(void *) {
    int result = check("pop");
    // Error injection models an asynchronous error reported after a valid pop.
    contextStack.pop_back(); return result;
}
int cuCtxSynchronize() { return check("sync"); }
int optixDeviceContextDestroy(int) { return check("optix"); }
int cuModuleUnload(int module) { return check(module == 1 ? "module1" : "module2"); }
int cuCtxDestroy(int) { events.push_back("destroy"); return fault == "destroy" ? 99 : 0; }
#define CHECK_CUDA_ERROR(e) do { if ((e) != 0) throw runtime_error("wrong current context or injected driver failure"); } while(0)
#define CHECK_OPTIX_ERROR(e) CHECK_CUDA_ERROR(e)
class CUDADevice {
public:
    int cudaContext = 7, optixContext = 8;
    vector<int> loadedModules{1, 2};
    ~CUDADevice();
};
'''
MAIN = r'''
int main(int argc, char **argv) {
    string mode = argv[1];
    fault = argc > 2 ? argv[2] : "";
    auto run = [&] {
        contextStack = mode == "none" ? vector<int>{} : vector<int>{42};
        auto previous = contextStack;
        { CUDADevice device; if (mode == "no-optix") device.optixContext = 0; }
        if (contextStack != previous) return 10;
        if (events.empty() || events.back() != "destroy") return 11;
        if (fault != "push") {
            auto pos = [&](string event) {
                for (size_t i = 0; i < events.size(); ++i) if (events[i] == event) return i;
                return events.size();
            };
            if (!(pos("sync") < pos("module1") && pos("module1") < pos("module2") &&
                    pos("module2") < pos("pop") && pos("pop") < pos("destroy"))) return 12;
            if (mode != "no-optix" && !(pos("sync") < pos("optix") && pos("optix") < pos("module1"))) return 13;
        }
        for (const auto &event: events) printf("%s ", event.c_str());
        puts("PASS");
        return 0;
    };
    int result;
    if (mode == "thread") { thread worker([&] { result = run(); }); worker.join(); }
    else result = run();
    return result;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=ROOT/'src/luxrays/devices/cudadevice.cpp')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--expect-failure', action='store_true')
    args = parser.parse_args()
    body = method(args.source.read_text(), 'CUDADevice::~CUDADevice()')
    results = []
    with tempfile.TemporaryDirectory(prefix='slc-cuda-teardown-') as tmp:
        source, binary = Path(tmp)/'test.cpp', Path(tmp)/'test'
        source.write_text(PREAMBLE + body + MAIN)
        compile_result = subprocess.run(['clang++', '-std=c++20', '-pthread', str(source), '-o', str(binary)], capture_output=True, text=True, timeout=30)
        assert compile_result.returncode == 0, compile_result.stderr
        cases = [(mode, '') for mode in ('none', 'foreign', 'thread', 'no-optix')]
        cases += [('foreign', fault) for fault in ('sync', 'optix', 'module1', 'module2', 'pop', 'destroy', 'push')]
        for mode, fault in cases:
            p = subprocess.run([str(binary), mode, fault], capture_output=True, text=True, timeout=5)
            results.append(dict(mode=mode, injected_fault=fault, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(source=str(args.source), scope='extracted production destructor; simulated context-stack driver; no GPU certification', results=results), indent=2)+'\n')
    print(json.dumps(results, indent=2))
    if args.expect_failure:
        assert all(r['returncode'] != 0 for r in results[:4]), 'Baseline context defect was not reproduced'
    else:
        assert all(r['returncode'] == 0 for r in results), 'Teardown contract failed'


if __name__ == '__main__':
    main()

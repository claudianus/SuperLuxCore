"""Run real Film pipeline replacement methods against context-owning plugins.

The fake plugin aborts when its GPU resources are destroyed without their
context or while a worker still owns them. This is a host lifetime regression,
not a device rendering test. Compiler products live only in a temporary folder.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from cuda_teardown_test import method, ROOT

PREAMBLE = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std;
using u_int = unsigned int;
template<class T> string ToString(T x) { return to_string(x); }
thread_local int current = 42;
atomic<bool> workerOwns{false};
int deleted = 0;
struct Device {
    int saved = 0, syncs = 0;
    void PushThreadCurrentDevice() { saved = current; current = 7; }
    void PopThreadCurrentDevice() { current = saved; }
    void FinishQueue() { assert(current == 7); ++syncs; }
};
struct ImagePipeline {
    bool gpu;
    explicit ImagePipeline(bool gpu = true): gpu(gpu) {}
    ~ImagePipeline() {
        assert(!workerOwns);
        if (gpu) assert(current == 7);
        ++deleted;
    }
};
struct Film {
    vector<ImagePipeline *> imagePipelines;
    unique_ptr<thread> imagePipelineThread;
    atomic<bool> isAsyncImagePipelineRunning{false};
    Device *hardwareDevice = nullptr;
    unsigned long long imagePipelineRevision = 0;
    void DeleteImagePipeline(ImagePipeline *);
    void WaitAsyncExecuteImagePipeline();
    void SetImagePipelines(u_int, ImagePipeline *);
    void SetImagePipelines(ImagePipeline *);
    void SetImagePipelines(vector<ImagePipeline *> &);
};
'''
MAIN = r'''
int main(int argc, char **argv) {
    string mode = argv[1];
    Film film; Device device;
    bool gpu = mode != "cpu";
    if (gpu) film.hardwareDevice = &device;
    film.imagePipelines.push_back(new ImagePipeline(gpu));
    bool completedWorker = mode == "completed";
    if (mode == "worker" || completedWorker) {
        workerOwns = true; film.isAsyncImagePipelineRunning = true;
        film.imagePipelineThread = make_unique<thread>([&] {
            this_thread::sleep_for(chrono::milliseconds(10));
            workerOwns = false; film.isAsyncImagePipelineRunning = false;
        });
        if (completedWorker) while (workerOwns) this_thread::yield();
    }
    if (mode == "index") film.SetImagePipelines(0, new ImagePipeline(gpu));
    else if (mode == "vector") {
        vector<ImagePipeline *> next{new ImagePipeline(gpu), new ImagePipeline(gpu)};
        film.SetImagePipelines(next);
    } else if (mode == "all") film.SetImagePipelines(new ImagePipeline(gpu));
    else film.SetImagePipelines(static_cast<ImagePipeline *>(nullptr));
    assert(current == 42 && deleted == 1);
    if (gpu) assert(device.syncs >= 1);
    if (film.imagePipelineThread) assert(!film.imagePipelineThread->joinable());
    film.SetImagePipelines(static_cast<ImagePipeline *>(nullptr));
    assert(current == 42 && film.imagePipelines.empty());
    puts("PASS pipeline resource ownership, drain, context restore and worker join");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=ROOT/'src/slg/film/filmimagepipeline.cpp')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--expect-failure', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text()
    signatures = ['void Film::WaitAsyncExecuteImagePipeline()',
                  'void Film::SetImagePipelines(const u_int index',
                  'void Film::SetImagePipelines(ImagePipeline *',
                  'void Film::SetImagePipelines(std::vector<ImagePipeline *> &']
    if 'void Film::DeleteImagePipeline(' in source:
        signatures.insert(0, 'void Film::DeleteImagePipeline(')
    body = '\n'.join(method(source, sig) for sig in signatures)
    results = []
    with tempfile.TemporaryDirectory(prefix='slc-film-teardown-') as tmp:
        code, binary = Path(tmp)/'test.cpp', Path(tmp)/'test'
        code.write_text(PREAMBLE + body + MAIN)
        p = subprocess.run(['clang++', '-std=c++20', '-pthread', str(code), '-o', str(binary)], capture_output=True, text=True, timeout=30)
        assert p.returncode == 0, p.stderr
        for mode in ('index', 'vector', 'all', 'worker', 'completed', 'cpu'):
            p = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=5)
            results.append(dict(mode=mode, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(source=str(args.source), scope='production host methods; simulated device-owning plugin; no GPU certification', results=results), indent=2)+'\n')
    print(json.dumps(results, indent=2))
    if args.expect_failure:
        assert all(r['returncode'] != 0 for r in results[:5])
    else:
        assert all(r['returncode'] == 0 for r in results)


if __name__ == '__main__':
    main()

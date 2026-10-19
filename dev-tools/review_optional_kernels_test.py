"""Compile and execute the actual host kernel-selection block over all gate states."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/slg/engines/pathoclbase/pathoclbaseoclthreadkernels.cpp').read_text()
gates = source[source.index('\tconst auto &ptConfig ='):source.index('\tconst string newKernelSrcHash =')]
selection = source[source.index('\tvector<KernelCompileJob> jobs ='):source.index('\t// Wavefront per-state queue builder')]
fields = sorted(set(re.findall(r'&([A-Za-z_][A-Za-z0-9_]*)', selection)))
kernels = [n for n in fields if not n.endswith('WorkGroupSize')]
groups = [n for n in fields if n.endswith('WorkGroupSize')]
code = '''#include <memory>
#include <vector>
#include <set>
#include <string>
#include <cassert>
#include <iostream>
using namespace std; using u_int = unsigned int;
struct Config { struct { bool visibilityEnable; int visCandCount; } restir;
struct { bool enabled; int giCandCount; } restirGI;
struct { bool enabled; int ptCandCount; } restirPT; };
struct KernelCompileJob {unique_ptr<int>* kernel; size_t* workGroupSize; bool advancePaths; const char* name;};
int main() {
'''
code += '\n'.join('unique_ptr<int> '+n+';' for n in kernels)
code += '\n'+'\n'.join('size_t '+n+'=0;' for n in groups)
code += '''
set<unsigned> masks;
for (int bits=0; bits<64; ++bits) {
Config cfg{{bool(bits&1),bool(bits&2)}, {bool(bits&4),bool(bits&8)}, {bool(bits&16),bool(bits&32)}};
struct { Config pathTracer; } threadTaskConfig{cfg};
'''
code += '\n'.join(n+'=make_unique<int>(1);' for n in kernels)
code += gates + selection
code += '''
bool vis=cfg.restir.visibilityEnable && cfg.restir.visCandCount>0;
bool gi=cfg.restirGI.enabled && cfg.restirGI.giCandCount>0;
bool pt=cfg.restirPT.enabled && cfg.restirPT.ptCandCount>0;
set<string> names; for (auto &job:jobs) assert(names.insert(job.name).second);
assert(jobs.size()==14+vis+2*gi+2*pt);
assert(names.count("AdvancePaths_MK_RT_RESTIR")==vis);
assert(names.count("AdvancePaths_MK_RT_GI_BOUNCE")==gi);
assert(names.count("AdvancePaths_MK_RT_GI_RESOLVE")==gi);
assert(names.count("AdvancePaths_MK_PT_BOUNCE")==pt);
assert(names.count("AdvancePaths_MK_PT_RESOLVE")==pt);
assert(bool(advancePathsKernel_MK_RT_RESTIR)==vis);
assert(bool(advancePathsKernel_MK_RT_GI_BOUNCE)==gi);
assert(bool(advancePathsKernel_MK_PT_BOUNCE)==pt);
assert(optionalKernelMask==(unsigned(vis)|(unsigned(gi)<<1)|(unsigned(pt)<<2)));
masks.insert(optionalKernelMask);
}
assert(masks.size()==8);
cout << "64 gate combinations, 8 distinct selection identities, optional release: PASS\\n";
}
'''
assert 'HashString(kernelSource) + "-" + ToString(optionalKernelMask)' in source
with tempfile.TemporaryDirectory(prefix='slc-kernel-selection-') as temp:
    cpp = Path(temp)/'test.cpp'; exe = Path(temp)/'test'; cpp.write_text(code)
    subprocess.run(['clang++','-std=c++17','-O1',str(cpp),'-o',str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)

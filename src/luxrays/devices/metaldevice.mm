/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 *   You may not use this file except in compliance with the License.       *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 *   under the License is distributed on an "AS IS" BASIS,                 *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 *   See the License for the specific language governing permissions and    *
 *   limitations under the License.                                         *
 ***************************************************************************/

// ObjC imports FIRST: LuxCore's C++ headers (pulled in below) load
// dispatch etc. in C++ mode, which would poison __OBJC__ for the
// Foundation headers Metal depends on.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "luxrays/core/hardwaredevice.h"
#include "luxrays/usings.h"

#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)

#include "luxrays/devices/metaldevice.h"
#include "luxrays/core/context.h"
#include "luxrays/utils/strutils.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

// mkdir(2)/getpid(2) for the on-disk cl2msl translation cache
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

namespace luxrays {

//------------------------------------------------------------------------------
// MetalDeviceDescription
//------------------------------------------------------------------------------

MetalDeviceDescription::MetalDeviceDescription(MTLDeviceHandle dev, const size_t devIndex) :
		DeviceDescription("MetalInitializingDevice", DEVICE_TYPE_METAL_GPU),
		metalDevice(dev) {
	[(__bridge id<MTLDevice>)metalDevice retain];

	name = [(__bridge id<MTLDevice>)metalDevice name].UTF8String;
}

MetalDeviceDescription::~MetalDeviceDescription() {
	[(__bridge id<MTLDevice>)metalDevice release];
}

int MetalDeviceDescription::GetComputeUnits() const {
	// Apple GPUs expose no core-count API; a conservative constant keeps
	// the engine's task sizing heuristics from degenerating.
	return 8;
}

u_int MetalDeviceDescription::GetNativeVectorWidthFloat() const {
	return 4;
}

size_t MetalDeviceDescription::GetMaxMemory() const {
	// The unified-memory budget Metal recommends for GPU work
	return [(__bridge id<MTLDevice>)metalDevice recommendedMaxWorkingSetSize];
}

size_t MetalDeviceDescription::GetMaxMemoryAllocSize() const {
	// Shared-storage buffers can span (nearly) the whole working set
	return [(__bridge id<MTLDevice>)metalDevice recommendedMaxWorkingSetSize];
}

bool MetalDeviceDescription::HasOutOfCoreMemorySupport() const {
	// Apple silicon unified memory needs no discrete-VRAM paging
	return false;
}

void MetalDeviceDescription::AddDeviceDescs(std::vector<DeviceDescriptionUPtr> &descriptions) {
	@autoreleasepool {
		// MTLCopyAllDevices returns only EXTERNAL GPUs. On Apple silicon
		// the integrated GPU (the one we render on) is only reachable via
		// MTLCreateSystemDefaultDevice - so union both, deduplicating the
		// default device when it is also in the external list.
		NSMutableArray<id<MTLDevice>> *devices = [NSMutableArray array];
		id<MTLDevice> def = MTLCreateSystemDefaultDevice();
		if (def)
			[devices addObject:def];
		for (id<MTLDevice> d in MTLCopyAllDevices())
			if (d != def)
				[devices addObject:d];

		for (size_t i = 0; i < devices.count; ++i)
			descriptions.push_back(std::make_unique<MetalDeviceDescription>(
					(__bridge void *)devices[i], i));
	}
}

//------------------------------------------------------------------------------
// MetalDevice
//------------------------------------------------------------------------------

MetalDevice::MetalDevice(const Context & context,
		MetalDeviceDescriptionConstRef desc, const size_t devIndex) :
		Device(context, devIndex),
		HardwareDevice(),
		deviceDesc(desc),
		device(desc.GetMetalDevice()),
		queue(nullptr) {
	[(__bridge id<MTLDevice>)device retain];
	queue = (__bridge void *)[(__bridge id<MTLDevice>)device newCommandQueue];
	deviceName = desc.GetName() + " MetalIntersect";
}

MetalDevice::~MetalDevice() {
	FinishQueue();
	[(__bridge id<MTLCommandQueue>)queue release];
	[(__bridge id<MTLDevice>)device release];
}

void MetalDevice::PushThreadCurrentDevice() {
	// Metal has no per-thread context binding (command buffers are
	// created from the shared queue). no-op.
}

void MetalDevice::PopThreadCurrentDevice() {
}

//------------------------------------------------------------------------------
// Kernels handling
//------------------------------------------------------------------------------

// Run the engine-embedded .cl->MSL translator (src/slg/utils/cl2msl.py)
// over the program source - the same pipeline the OpenCL backend sends
// to its runtime compiler.
//
// The translation is a pure function of (program source, program
// parameters, translator) and it is expensive: cpp plus several regex
// passes over the multi-MB engine kernel source take ~7s on an M5 Pro -
// more than the Metal compilation itself. Each result is therefore
// cached on disk; a hit turns the whole step into two file reads.
static uint64_t Fnv1a64(const string &s, uint64_t h = 1469598103934665603ULL) {
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

static bool LoadCachedTranslation(const string &cacheDir, const string &key,
		string &mslSource, string &layoutJson) {
	const string mslPath = cacheDir + "/" + key + ".msl";
	const string jsonPath = cacheDir + "/" + key + ".json";
	ifstream mf(mslPath, ios::binary);
	ifstream jf(jsonPath, ios::binary);
	if (!mf || !jf)
		return false;
	stringstream ms, js;
	ms << mf.rdbuf();
	js << jf.rdbuf();
	if (ms.str().empty() || js.str().empty())
		return false;
	mslSource = ms.str();
	layoutJson = js.str();
	return true;
}

static void StoreCachedTranslation(const string &cacheDir, const string &key,
		const string &mslSource, const string &layoutJson) {
	// Write to a unique temp name then rename: concurrent sessions
	// (multiple Blender instances) must never read a half-written file.
	const string tmpSuffix = ".tmp" + to_string((long)getpid());
	{
		ofstream f(cacheDir + "/" + key + ".msl" + tmpSuffix, ios::binary);
		if (f) f << mslSource;
		else return;
	}
	{
		ofstream f(cacheDir + "/" + key + ".json" + tmpSuffix, ios::binary);
		if (f) f << layoutJson;
		else return;
	}
	rename((cacheDir + "/" + key + ".msl" + tmpSuffix).c_str(),
			(cacheDir + "/" + key + ".msl").c_str());
	rename((cacheDir + "/" + key + ".json" + tmpSuffix).c_str(),
			(cacheDir + "/" + key + ".json").c_str());
}

static bool RunCL2MSL(const Context &ctx,
		const vector<string> &programParameters,
		const string &programSource, const string &programName,
		string &mslSource, string &layoutJson,
		string &outCacheDir, string &outCacheKey) {
	const string srcFile = "/tmp/luxcore_metal_src.cl";
	const string mslFile = "/tmp/luxcore_metal_src.msl";
	const string jsonFile = "/tmp/luxcore_metal_layout.json";

	// The translator path resolution order:
	// 1. LUXCORE_CL2MSL_PATH env (relocated installs)
	// 2. the compile-time source root (LUXCORE_SOURCE_DIR, set by CMake)
	// 3. cwd fallback
	const char *envPath = getenv("LUXCORE_CL2MSL_PATH");
	string translatorPath;
	if (envPath)
		translatorPath = envPath;
	else {
#ifdef LUXCORE_SOURCE_DIR
		translatorPath = string(LUXCORE_SOURCE_DIR) + "/src/slg/utils/cl2msl.py";
#else
		translatorPath = "../src/slg/utils/cl2msl.py";
#endif
	}

	// ---- persistent translation cache ----
	string cacheDir;
	{
		const char *envCacheDir = getenv("LUXCORE_METAL_CACHE_DIR");
		if (envCacheDir)
			cacheDir = envCacheDir;
		else {
			const char *home = getenv("HOME");
			if (home)
#if defined(__APPLE__)
				cacheDir = string(home) + "/Library/Caches/LuxCoreRender/metal";
#else
				cacheDir = string(home) + "/.cache/luxcore/metal";
#endif
			else
				cacheDir = "/tmp/luxcore_metal_cache";
		}
	}

	// mkdir(2) does not create missing parents (i.e. LuxCoreRender may not
	// exist yet): try every path level, ignoring EEXIST.
	{
		string p;
		for (size_t i = 1; i <= cacheDir.size(); ++i) {
			if ((i == cacheDir.size()) || (cacheDir[i] == '/')) {
				p = cacheDir.substr(0, i);
				if (!p.empty())
					mkdir(p.c_str(), 0755);
			}
		}
	}

	// Key: program source + parameters + the translator's own content
	// (editing cl2msl.py must invalidate every cached translation).
	string keySrc = programSource;
	for (const string &p : programParameters)
		keySrc += "\x1f" + p;
	{
		ifstream tf(translatorPath, ios::binary);
		if (tf) {
			stringstream ss;
			ss << tf.rdbuf();
			keySrc += "\x1f" + ss.str();
		}
	}
	char keyBuf[32];
	snprintf(keyBuf, sizeof(keyBuf), "%016llx",
			(unsigned long long)Fnv1a64(keySrc));
	const string key = keyBuf;
	// The caller uses (cacheDir, key) to name the compiled-pipeline
	// archive kept next to the translation files
	outCacheDir = cacheDir;
	outCacheKey = key;

	if (LoadCachedTranslation(cacheDir, key, mslSource, layoutJson)) {
		LR_LOG(ctx, "[" << programName << "] cl2msl translation cache hit ("
				<< (mslSource.size() / 1024) << "Kbytes)");
		return true;
	}

	{
		ofstream f(srcFile, ios::binary);
		f << programSource;
	}

	stringstream cmd;
	cmd << "python3 \"" << translatorPath << "\""
		<< " \"" << mslFile << "\" \"" << jsonFile << "\"";
	for (const string &p : programParameters)
		cmd << " \"" << p << "\"";
	cmd << " < \"" << srcFile << "\"";

	const int rc = system(cmd.str().c_str());
	if (rc != 0) {
		LR_LOG(ctx, "[" << programName << "] cl2msl translator failed (rc=" << rc << ")");
		return false;
	}

	ifstream mf(mslFile, ios::binary);
	ifstream jf(jsonFile, ios::binary);
	if (!mf || !jf)
		return false;
	stringstream ms, js;
	ms << mf.rdbuf();
	js << jf.rdbuf();
	mslSource = ms.str();
	layoutJson = js.str();

	StoreCachedTranslation(cacheDir, key, mslSource, layoutJson);
	return true;
}

// MetalDeviceProgram: the compiled-pipeline archive is serialized when
// the program goes away (i.e. at session stop). Owning the ObjC objects
// here keeps the header free of Objective-C types.
MetalDeviceProgram::MetalDeviceProgram() :
		library(nullptr), binaryArchive(nullptr), archiveDirty(false) {
}

MetalDeviceProgram::~MetalDeviceProgram() {
	if (binaryArchive) {
		@autoreleasepool {
			id<MTLBinaryArchive> archive =
					(__bridge_transfer id<MTLBinaryArchive>)binaryArchive;
			if (archiveDirty && !archivePath.empty()) {
				NSString *path = [NSString stringWithUTF8String:archivePath.c_str()];
				NSError *err = nil;
				if ([archive serializeToURL:[NSURL fileURLWithPath:path]
						error:&err]) {
					fprintf(stderr, "[Metal] binary archive written: %s\n",
							archivePath.c_str());
				} else {
					// A failed serialize only means the next session
					// recompiles: never fatal.
					fprintf(stderr, "[Metal] binary archive serialize failed: %s\n",
							err ? err.localizedDescription.UTF8String : "?");
				}
			}
		}
	}
}

HardwareDeviceProgramUPtr MetalDevice::CompileProgram(
		const vector<string> &programParameters,
		const string &programSource,
		const string &programName
	) {
	string mslSource, layoutJson, cacheDir, cacheKey;
	if (!RunCL2MSL(deviceContext, programParameters, programSource, programName,
			mslSource, layoutJson, cacheDir, cacheKey)) {
		LR_LOG(deviceContext, "[" << programName << "] METAL program translation error");
		throw runtime_error(programName + " METAL program translation error");
	}

	LR_LOG(deviceContext, "[" << programName << "] Compiling Metal kernels ("
			<< (mslSource.size() / 1024) << "Kbytes)");

	id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
	NSError *err = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:
			[NSString stringWithUTF8String:mslSource.c_str()]
			options:nil error:&err];
	if (!lib) {
		LR_LOG(deviceContext, "[" << programName << "] Metal program compilation error: "
				<< endl << (err ? err.localizedDescription.UTF8String : "?"));
		throw runtime_error(programName + " Metal program compilation error");
	}

	auto metalDeviceProgram = std::make_unique<MetalDeviceProgram>();
	metalDeviceProgram->library = (__bridge_retained void *)lib;
	metalDeviceProgram->layoutJson = layoutJson;

	// ---- compiled-pipeline archive (cold PSO compilation is ~3-4s per
	// AdvancePaths variant; archive lookups are instant) ----
	{
		const string archivePath = cacheDir + "/" + cacheKey + ".archive";
		metalDeviceProgram->archivePath = archivePath;

		NSFileManager *fm = [NSFileManager defaultManager];
		NSString *nsPath = [NSString stringWithUTF8String:archivePath.c_str()];
		bool exists = [fm fileExistsAtPath:nsPath];

		MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
		if (exists)
			ad.url = [NSURL fileURLWithPath:nsPath];

		NSError *aerr = nil;
		id<MTLBinaryArchive> archive = [dev newBinaryArchiveWithDescriptor:ad
				error:&aerr];
		if (!archive && exists) {
			// Stale archive (i.e. after an OS/driver update): rebuild it.
			[fm removeItemAtPath:nsPath error:nil];
			ad.url = nil;
			archive = [dev newBinaryArchiveWithDescriptor:ad error:nil];
			exists = false;
		}

		if (archive) {
			metalDeviceProgram->binaryArchive = (__bridge_retained void *)archive;
			// Written back only when it did not exist yet: re-serializing
			// at every session end would just rewrite identical contents.
			metalDeviceProgram->archiveDirty = !exists;
			LR_LOG(deviceContext, "[" << programName << "] binary archive "
					<< (exists ? "loaded" : "created") << ": " << archivePath);
		} else {
			LR_LOG(deviceContext, "[" << programName << "] binary archive unavailable: "
					<< (aerr ? aerr.localizedDescription.UTF8String : "?"));
		}
	}

	return static_cast<HardwareDeviceProgramUPtr>(std::move(metalDeviceProgram));
}

// Dispatch _LayoutProbe_<kernel> to read sizeof/offsets of the
// KernelScalarsN struct from the GPU (the C++ host must fill that
// exact layout). Overwrites the marsh offsets with measured values.
static void RunLayoutProbe(MetalDeviceKernel::Marshalling &m,
		const string &kernelName, id<MTLLibrary> lib,
		id<MTLDevice> dev, id<MTLCommandQueue> probeQueue) {
	if (!m.hasScalarBundle)
		return;

	id<MTLFunction> fn = [lib newFunctionWithName:
			[NSString stringWithFormat:@"_LayoutProbe_%s", kernelName.c_str()]];
	if (!fn)
		return;   // probe missing: keep the computed offsets

	NSError *err = nil;
	id<MTLComputePipelineState> pso =
		[dev newComputePipelineStateWithFunction:fn error:&err];
	if (!pso)
		return;

	// out buffer: [sizeof, off_0, off_1, ...] one uint each + margin
	const size_t nOut = 2 + m.scalarNames.size() + 8;
	id<MTLBuffer> out = [dev newBufferWithLength:nOut * sizeof(uint32_t)
		options:MTLResourceStorageModeShared];
	memset(out.contents, 0, nOut * sizeof(uint32_t));

	id<MTLCommandBuffer> cb = [probeQueue commandBuffer];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:pso];
	[e setBuffer:out offset:0 atIndex:0];
	[e dispatchThreads:MTLSizeMake(1, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[e endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	const uint32_t *res = (const uint32_t *)out.contents;
	const uint32_t size = res[0];
	if (size > 0) {
		m.scalarBundleSize = size;
		for (size_t i = 0; i < m.scalarNames.size(); ++i)
			m.scalarOffsets[i] = res[1 + i];
	}
}

// Parse the cl2msl layout JSON for one kernel into the dispatch map.
// JSON shape per kernel: {"scalars": [[name,type,size],...],
// "direct": [[name,head],...], "table": [[name,head],...]}
// The scalar sizes are C++ mirror sizes (bytes); the Metal struct may
// add padding - the C++ bundle must match the METAL layout, so offsets
// are computed with the same alignment rules the MSL compiler uses
// (natural alignment, struct size rounded to largest member - all
// members are 4- or 8-byte here; gpuAddresses in the ptr bundle are
// 8-byte aligned).
static void ParseMarshalling(MetalDeviceKernel::Marshalling &m,
		const string &kernelName, const string &layoutJson) {
	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:layoutJson.data()
				length:layoutJson.size()];
		id obj = [NSJSONSerialization JSONObjectWithData:data
				options:0 error:nil];
		if (!obj)
			return;
		NSDictionary *all = (NSDictionary *)obj;
		NSDictionary *k = all[@(kernelName.c_str())];
		if (!k)
			return;

		// ---- scalar bundle ----
		NSArray *scalars = k[@"scalars"];
		size_t off = 0;
		for (NSArray *s in scalars) {
			const string name = [s[0] UTF8String];
			const size_t size = (size_t)[(NSNumber *)s[2] unsignedLongValue];
			// natural alignment: round up to size (1/4/8)
			const size_t align = (size < 4) ? size : ((size == 8) ? 8 : 4);
			off = (off + align - 1) & ~(align - 1);
			m.scalarNames.push_back(name);
			m.scalarOffsets.push_back(off);
			m.scalarSizes.push_back(size);
			off += size;
		}
		m.scalarBundleSize = (off + 7) & ~(size_t)7;
		m.hasScalarBundle = !m.scalarNames.empty();

		// ---- pointers: direct slots then table ----
		NSArray *direct = k[@"direct"];
		NSArray *table = k[@"table"];
		// Direct-bound pointers get slots 2.. in signature (= direct array) order
		int slot = 2;
		for (NSArray *d in direct) {
			const string name = [d[0] UTF8String];
			m.ptrNames.push_back(name);
			m.ptrSlots.push_back(slot++);
			m.ptrTableOffsets.push_back(0);
		}
		size_t toff = 0;
		for (NSArray *t in table) {
			const string name = [t[0] UTF8String];
			m.ptrNames.push_back(name);
			m.ptrSlots.push_back(-1);
			toff = (toff + 7) & ~(size_t)7;   // gpuAddress = 8-byte
			m.ptrTableOffsets.push_back(toff);
			toff += 8;
		}
		m.ptrBundleSize = (toff + 7) & ~(size_t)7;
		m.hasPtrBundle = !m.ptrNames.empty() && toff > 0;

		// ---- original-signature argument order ----
		// The engine sets args in the ORIGINAL OpenCL signature order,
		// which interleaves scalars and pointers freely (and direct with
		// table pointers). Walking argOrder keeps the two consumption
		// cursors aligned; the reordered direct+table concatenation
		// misassigns every pointer after the first category change.
		NSArray *order = k[@"order"];
		if (order) {
			// One entry per ORDER position in BOTH arrays (-1 = not that
			// role): the EnqueueKernel walk indexes them by order position.
			for (NSArray *e in order) {
				const NSString *role = (NSString *)e[0];
				const string name = [e[1] UTF8String];
				if ([role isEqualToString:@"p"]) {
					m.argIsPointer.push_back(true);
					int piFound = -1;
					for (size_t pi = 0; pi < m.ptrNames.size(); ++pi) {
						if (m.ptrNames[pi] == name) {
							piFound = (int)pi;
							break;
						}
					}
					m.argPtrIndex.push_back(piFound);
					m.argScalarIndex.push_back(-1);
				} else {
					m.argIsPointer.push_back(false);
					int siFound = -1;
					for (size_t si = 0; si < m.scalarNames.size(); ++si) {
						if (m.scalarNames[si] == name) {
							siFound = (int)si;
							break;
						}
					}
					m.argPtrIndex.push_back(-1);
					m.argScalarIndex.push_back(siFound);
				}
			}
		}
	}
}

HardwareDeviceKernelUPtr MetalDevice::GetKernel(
		HardwareDeviceProgramRef program,
		const string &kernelName
	) {
	auto [kernel, metalDeviceKernel] =
		CreateUniquePtr<HardwareDeviceKernel, MetalDeviceKernel>();

	auto& metalDeviceProgram = dynamic_cast<MetalDeviceProgramRef>(program);

	id<MTLLibrary> lib = (__bridge id<MTLLibrary>)metalDeviceProgram.library;
	id<MTLFunction> fn = [lib newFunctionWithName:
			[NSString stringWithUTF8String:kernelName.c_str()]];
	if (!fn)
		throw runtime_error("Metal kernel not found: " + kernelName);

	id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
	id<MTLBinaryArchive> archive =
		(__bridge id<MTLBinaryArchive>)metalDeviceProgram.binaryArchive;

	// Create the pipeline through the binary archive when available: a
	// pipeline stored there deserializes in milliseconds instead of the
	// 3-4s a cold compile of an AdvancePaths variant takes.
	NSError *err = nil;
	id<MTLComputePipelineState> pso;
	MTLComputePipelineDescriptor *pd = [[MTLComputePipelineDescriptor alloc] init];
	pd.computeFunction = fn;
	if (archive)
		pd.binaryArchives = @[ archive ];

	if (archive) {
		pso = [dev newComputePipelineStateWithDescriptor:pd
				options:MTLPipelineOptionNone reflection:nil error:&err];
	}

	if (!pso) {
		// No archive, or a lookup failure: compile from source now.
		err = nil;
		pso = [dev newComputePipelineStateWithFunction:fn error:&err];
	}
	if (!pso)
		throw runtime_error("Metal PSO creation failed for " + kernelName +
			": " + (err ? err.localizedDescription.UTF8String : "?"));

	// Populate the archive. NOTE: pd.binaryArchives is only a HINT - a
	// miss silently compiles from source, so every pipeline must be added
	// explicitly or the archive stays empty ("Nothing to serialize").
	// Entries already present report an error here: ignored.
	if (archive) {
		NSError *aerr = nil;
		// The result is deliberately ignored: entries already in the
		// archive report an error, and a fresh archive is written back
		// once per key (see archiveDirty in CompileProgram).
		[archive addComputePipelineFunctionsWithDescriptor:pd error:&aerr];
	}

	metalDeviceKernel.function = (__bridge_retained void *)fn;
	metalDeviceKernel.pipeline = (__bridge_retained void *)pso;

	ParseMarshalling(metalDeviceKernel.marsh, kernelName,
			metalDeviceProgram.layoutJson);

	// Measure the scalar bundle layout with Metal's own offsetof via
	// the generated probe kernel (no padding guesses)
	{
		id<MTLDevice> probeDev = (__bridge id<MTLDevice>)device;
		id<MTLCommandQueue> probeQueue = (__bridge id<MTLCommandQueue>)queue;
		RunLayoutProbe(metalDeviceKernel.marsh, kernelName, lib,
				probeDev, probeQueue);
	}

	return std::move(kernel);
}



u_int MetalDevice::GetKernelWorkGroupSize(HardwareDeviceKernelRPtr kernel) {
	return 32;
}

void MetalDevice::SetKernelArg(HardwareDeviceKernelRPtr kernel,
		const u_int index, const size_t size, const void *arg) {
	assert(kernel);
	assert(!kernel->IsNull());

	auto& metalDeviceKernel = dynamic_cast<MetalDeviceKernelRef>(*kernel);

	if (index >= metalDeviceKernel.args.size()) {
		metalDeviceKernel.args.resize(index + 1, nullptr);
		metalDeviceKernel.buffs.resize(index + 1, nullptr);
		metalDeviceKernel.roles.resize(index + 1, MetalDeviceKernel::ArgRole::UNSET);
	}

	// The engine passes literal nullptr for disabled buffer channels via
	// the scalar template (T = std::nullptr_t: Size() = sizeof(void*)).
	// OpenCL handled this as a NULL cl_mem; record it as a NULL POINTER
	// so the marshalling cursor stays aligned with the MSL signature.
	if (!arg && size == sizeof(void *)) {
		if (metalDeviceKernel.args[index]) {
			delete[] (char *)metalDeviceKernel.args[index];
			metalDeviceKernel.args[index] = nullptr;
		}
		metalDeviceKernel.buffs[index] = nullptr;
		metalDeviceKernel.roles[index] = MetalDeviceKernel::ArgRole::POINTER;
		return;
	}

	if (metalDeviceKernel.args[index]) {
		delete[] (char *)metalDeviceKernel.args[index];
		metalDeviceKernel.args[index] = nullptr;
	}
	metalDeviceKernel.buffs[index] = nullptr;
	metalDeviceKernel.roles[index] = MetalDeviceKernel::ArgRole::SCALAR;

	if (arg) {
		// Copy the argument bytes (CUDA-style deferred marshalling)
		void *argCpy = new char[size];
		memcpy(argCpy, arg, size);
		metalDeviceKernel.args[index] = argCpy;
	} else {
		// Nullptr with a nonzero size is the CUDA impl's "zero pointer"
		// case (e.g. an optional scalar): record a 4-byte zero so the
		// dispatch still consumes the slot as a scalar.
		if (size > 0) {
			void *argCpy = new char[4]();
			metalDeviceKernel.args[index] = argCpy;
		}
	}
}

void MetalDevice::SetKernelArgBuffer(HardwareDeviceKernelRPtr kernel,
		const u_int index, const HardwareDeviceBuffer *buff) {
	assert(kernel);
	assert(!kernel->IsNull());

	auto& metalDeviceKernel = dynamic_cast<MetalDeviceKernelRef>(*kernel);

	if (index >= metalDeviceKernel.args.size()) {
		metalDeviceKernel.args.resize(index + 1, nullptr);
		metalDeviceKernel.buffs.resize(index + 1, nullptr);
		metalDeviceKernel.roles.resize(index + 1, MetalDeviceKernel::ArgRole::UNSET);
	}

	if (metalDeviceKernel.args[index]) {
		delete[] (char *)metalDeviceKernel.args[index];
		metalDeviceKernel.args[index] = nullptr;
	}
	// A NULL buffer still marks the slot as a POINTER: the engine passes
	// nullptr for disabled channels (denoiser film buffers, unused image
	// maps) and OpenCL accepted NULL cl_mem there. Skipping the slot would
	// shift every later pointer argument - wild GPU addressing.
	metalDeviceKernel.buffs[index] = buff;
	metalDeviceKernel.roles[index] = MetalDeviceKernel::ArgRole::POINTER;
}

void MetalDevice::EnqueueKernel(HardwareDeviceKernelRPtr kernel,
		const HardwareDeviceRange &globalSize,
		const HardwareDeviceRange &workGroupSize) {
	assert(kernel);
	assert(!kernel->IsNull());

	// Every ObjC object created below (command buffers, encoders, the
	// diagnostic NSString) must leave scope in a drained pool: the engine's
	// render threads run this thousands of times and thread-exit otherwise
	// releases the whole TLS pool at once - after the device/queue may be
	// gone (observed as an objc_release crash at thread teardown).
	@autoreleasepool {
	auto& metalDeviceKernel = dynamic_cast<MetalDeviceKernelRef>(*kernel);
	auto &m = metalDeviceKernel.marsh;

	id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queue;
	// Explicit retain: commandBuffer's ownership convention (autoreleased
	// vs caller-owned) is ambiguous across SDKs - a wrong guess once left
	// command buffers alive only in the autorelease pool, and FinishQueue
	// then waited/released freed objects (objc zombie: waitUntilCompleted
	// sent to a deallocated instance).
	id<MTLCommandBuffer> cb = [[q commandBuffer] retain];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];

	[e setComputePipelineState:(__bridge id<MTLComputePipelineState>)metalDeviceKernel.pipeline];

	// Engine SetKernelArg order == MSL signature order:
	//   scalars[i] (by scalarNames) fill KernelScalarsN (buffer(0))
	//   direct pointers bind [[buffer(2+)]] in ptrNames order
	//   table pointers fill KernelPtrsN (buffer(1)) with gpuAddresses
	// This is the dispatch shape the metal-poc converge/scene tests
	// verified (scalars_ [[buffer(0)]], ptrs_ [[buffer(1)]] as DEVICE
	// memory - constant-space tables dereference to 0 on Apple GPUs).
	id<MTLDevice> dev = (__bridge id<MTLDevice>)device;

	std::vector<uint8_t> scalarBundle(m.scalarBundleSize, 0);
	std::vector<uint8_t> ptrBundle(m.ptrBundleSize, 0);

	// Map engine arg index -> (role, value)
	// Walk the ORIGINAL-signature argument order (argIsPointer /
	// argPtrIndex / argScalarIndex from the layout JSON): the engine
	// interleaves scalars and pointers freely, so consuming direct-then-
	// table lists sequentially would misassign every pointer after the
	// first category change. The per-index role recorded by SetKernelArg
	// (SCALAR/POINTER) validates the walk and handles the engine's
	// literal-nullptr buffer channels.
	const bool useArgOrder = !m.argIsPointer.empty();
	size_t scalarIdx = 0, ptrIdx = 0;
	size_t argOrderPos = 0;
	std::vector<bool> directBound;   // per m.ptrSlots index
	if (!m.ptrNames.empty())
		directBound.resize(m.ptrNames.size(), false);
	// Buffers this dispatch reads: EnqueueWriteBuffer() must not overwrite
	// any of them while the command buffer is still in flight.
	std::vector<const MetalDeviceBuffer *> usedBuffers;
	for (u_int i = 0; i < metalDeviceKernel.args.size(); ++i) {
		bool isPointer;
		if (useArgOrder && argOrderPos < m.argIsPointer.size()) {
			// trust the signature order (the roles recorded at set time
			// should agree; nullptr scalars of pointer slots were already
			// reclassified at SetKernelArg time)
			isPointer = m.argIsPointer[argOrderPos];
			++argOrderPos;
		} else {
			// no order info (older layout JSON): fall back to the
			// recorded per-index roles
			isPointer = (metalDeviceKernel.roles.size() > i) ?
				(metalDeviceKernel.roles[i] == MetalDeviceKernel::ArgRole::POINTER) :
				(metalDeviceKernel.buffs[i] != nullptr);
		}
		const HardwareDeviceBuffer *buff = metalDeviceKernel.buffs[i];
		const void *argBytes = metalDeviceKernel.args[i];

		if (isPointer) {
			// Resolve WHICH pointer slot this arg maps to: with the order
			// array it's a direct name lookup; otherwise sequential.
			size_t thisPtr;
			if (useArgOrder && argOrderPos <= m.argIsPointer.size()
					&& !m.argPtrIndex.empty()) {
				const size_t op = argOrderPos - 1;
				thisPtr = (op < m.argPtrIndex.size() && m.argPtrIndex[op] >= 0) ?
					(size_t)m.argPtrIndex[op] : ptrIdx;
			} else {
				thisPtr = ptrIdx;
			}
			if (thisPtr < m.ptrNames.size()) {
				const int slot = m.ptrSlots[thisPtr];
				if (buff) {
					const MetalDeviceBuffer *metalBuff =
						dynamic_cast<const MetalDeviceBuffer *>(buff);
					assert(metalBuff);
					usedBuffers.push_back(metalBuff);
					id<MTLBuffer> mtlBuff = (__bridge id<MTLBuffer>)metalBuff->metalBuff;
					if (slot >= 0) {
						[e setBuffer:mtlBuff offset:0 atIndex:(NSUInteger)slot];
						if (thisPtr < directBound.size())
							directBound[thisPtr] = true;
					} else {
						// Table entry: record the gpu address. Buffers
						// reached through gpuAddress (not setBuffer) are
						// NOT automatically resident for the dispatch -
						// without useResource the driver may leave their
						// pages unmapped and reads silently return zeros
						// (observed: curveCps/curveSegIndices read as 0 ->
						// zero curve tangent -> NaN BSDF frame). Read|Write
						// because table members include film/task-queue
						// outputs; the extra usage only widens hazard
						// tracking, residency is the required part.
						[e useResource:mtlBuff
								usage:MTLResourceUsageRead | MTLResourceUsageWrite];
						const size_t toff = m.ptrTableOffsets[thisPtr];
						if (toff + 8 <= ptrBundle.size()) {
							const uint64_t addr = mtlBuff.gpuAddress;
							memcpy(ptrBundle.data() + toff, &addr, 8);
						}
					}
				} else {
					// NULL buffer: direct slots stay unbound here (the
					// tail pass below binds nil); table entries record 0.
					if (slot < 0) {
						const size_t toff = m.ptrTableOffsets[thisPtr];
						if (toff + 8 <= ptrBundle.size()) {
							const uint64_t zero = 0;
							memcpy(ptrBundle.data() + toff, &zero, 8);
						}
					}
				}
			}
			++ptrIdx;
		} else if (argBytes) {
			// Which scalar slot: order-array lookup when available
			size_t thisScalar;
			if (useArgOrder && argOrderPos <= m.argIsPointer.size()
					&& !m.argScalarIndex.empty()) {
				const size_t op = argOrderPos - 1;
				thisScalar = (op < m.argScalarIndex.size() && m.argScalarIndex[op] >= 0) ?
					(size_t)m.argScalarIndex[op] : scalarIdx;
			} else {
				thisScalar = scalarIdx;
			}
			if (thisScalar < m.scalarNames.size()) {
				// scalar payload: the engine's SetKernelArg copied
				// exactly scalarSizes[i] bytes (bool=1, float=4...);
				// copying the inter-offset gap instead would overread
				// the argument buffer on sub-4-byte members.
				const size_t off = m.scalarOffsets[thisScalar];
				const size_t nextOff = (thisScalar + 1 < m.scalarOffsets.size())
					? m.scalarOffsets[thisScalar + 1] : m.scalarBundleSize;
				size_t size = (thisScalar < m.scalarSizes.size())
					? m.scalarSizes[thisScalar] : 4;
				if (nextOff > off && size > nextOff - off)
					size = nextOff - off;   // never cross into the next member
				if (size > 0 && off + size <= scalarBundle.size())
					memcpy(scalarBundle.data() + off, argBytes, size);
			}
			++scalarIdx;
		}
	}

	// Bind buffer(0) scalar bundle. The bundle ALWAYS exists when the
	// kernel has scalars - bind it even if some members were never set
	// (the unset ones stay zeroed, mirroring OpenCL's zeroed args).
	if (m.hasScalarBundle && m.scalarBundleSize > 0) {
		if (scalarBundle.size() < m.scalarBundleSize)
			scalarBundle.resize(m.scalarBundleSize, 0);
		[e setBytes:scalarBundle.data()
			length:scalarBundle.size() atIndex:0];
	}

	// Bind buffer(1) pointer table (device space; zeroed gpuAddresses
	// for buffers the engine never set - matching OpenCL NULL args).
	if (m.hasPtrBundle && m.ptrBundleSize > 0) {
		if (ptrBundle.size() < m.ptrBundleSize)
			ptrBundle.resize(m.ptrBundleSize, 0);
		[e setBytes:ptrBundle.data()
			length:ptrBundle.size() atIndex:1];
	}

	// Direct slots the engine never set must still be bound: an
	// unbound required buffer aborts the command buffer at commit.
	// A null Metal buffer is the OpenCL NULL cl_mem equivalent.
	for (size_t d = 0; d < m.ptrSlots.size(); ++d) {
		if (m.ptrSlots[d] >= 0 && !directBound[d])
			[e setBuffer:nil offset:0 atIndex:(NSUInteger)m.ptrSlots[d]];
	}

	const size_t groupSize = max<size_t>(workGroupSize.sizes[0], 1);
	const size_t global = globalSize.sizes[0];
	// dispatchThreads covers the whole grid in one call (the kernel
	// guards the tail beyond rayCount itself, like the OpenCL
	// ND-range does with a rounded-up global size).
	const MTLSize tgSize = MTLSizeMake(groupSize, 1, 1);
	const MTLSize gridSize = MTLSizeMake(global, 1, 1);
	[e dispatchThreads:gridSize threadsPerThreadgroup:tgSize];

	[e endEncoding];

	// Commit + track under one lock: FinishQueue (which can run on the
	// self-halt session thread) must never observe a committed-but-
	// untracked buffer (queue outlived) nor a tracked-but-uncommitted
	// one (waitUntilCompleted on an uncommitted buffer). Committing is
	// just an enqueue - cheap to hold the lock for.
	{
		std::lock_guard<std::mutex> lock(inFlightMutex);
		// NOTE: (__bridge_retained) is a NO-OP under MRC - it only retains
		// under ARC. Use an explicit -retain so the tracked command buffer
		// actually survives the autoreleasepool drain; FinishQueue()
		// balances it with -release after waitUntilCompleted.
		inFlightWork.push_back({(MTLCommandBufferHandle)[cb retain], usedBuffers});
		[cb commit];
	}

	}   // @autoreleasepool
}

void MetalDevice::CommitAndTrackInFlight(MTLCommandBufferHandle commandBuffer,
		const std::vector<const MetalDeviceBuffer *> &buffers) {
	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)commandBuffer;

	// Same protocol as the tail of EnqueueKernel(): commit + track under
	// one lock so FinishQueue() never observes a committed-but-untracked
	// buffer and EnqueueWriteBuffer() can still detect conflicts.
	// (__bridge_retained) would be a no-op under MRC - explicit -retain.
	std::lock_guard<std::mutex> lock(inFlightMutex);
	inFlightWork.push_back({(MTLCommandBufferHandle)[cb retain], buffers});
	[cb commit];
}

void MetalDevice::EnqueueReadBuffer(const HardwareDeviceBuffer *buff,
		const bool blocking, const size_t size, void *ptr) {
	assert(buff);
	assert(!buff->IsNull());

	const MetalDeviceBuffer *metalBuff =
		dynamic_cast<const MetalDeviceBuffer *>(buff);
	assert(metalBuff);

	// Shared storage: the host pointer IS the buffer contents, but the
	// kernels that produced it may still be executing. OpenCL's async
	// read defers the copy through the in-order queue; with a plain
	// host-side memcpy we must drain the queue FIRST or we copy zeros.
	FinishQueue();
	memcpy(ptr, [(__bridge id<MTLBuffer>)metalBuff->metalBuff contents], size);
}

void MetalDevice::EnqueueWriteBuffer(const HardwareDeviceBuffer *buff,
		const bool blocking, const size_t size, const void *ptr) {
	assert(buff);
	assert(!buff->IsNull());

	const MetalDeviceBuffer *metalBuff =
		dynamic_cast<const MetalDeviceBuffer *>(buff);
	assert(metalBuff);

	// Shared storage: this memcpy IS the transfer, executed on the host
	// right now - unlike OpenCL's async write, which the in-order queue
	// would run only after the already-enqueued work. If an in-flight
	// dispatch still reads this buffer, the memcpy would race with it and
	// the kernel would see a half-overwritten buffer. Only wait when the
	// buffer is actually referenced, so unrelated uploads keep overlapping
	// with the GPU.
	{
		bool conflicting = false;
		{
			std::lock_guard<std::mutex> lock(inFlightMutex);
			for (auto &w : inFlightWork) {
				for (const MetalDeviceBuffer *b : w.buffers) {
					if (b == metalBuff) {
						conflicting = true;
						break;
					}
				}
				if (conflicting)
					break;
			}
		}

		if (conflicting)
			FinishQueue();
	}

	memcpy([(__bridge id<MTLBuffer>)metalBuff->metalBuff contents], ptr, size);
	if (blocking)
		FinishQueue();
}

void MetalDevice::FlushQueue() {
}

void MetalDevice::FinishQueue() {
	// Shared-storage buffer reads need the kernel writes to have
	// landed before the CPU can observe them: wait on EVERY committed
	// buffer, not just the latest (worker threads dispatch kernels
	// from their own loop and the main thread may finish the queue).
	std::vector<InFlightDispatch> waiting;
	{
		std::lock_guard<std::mutex> lock(inFlightMutex);
		waiting.swap(inFlightWork);
	}
	for (auto &w : waiting) {
		[(__bridge id<MTLCommandBuffer>)w.cb waitUntilCompleted];
		[(__bridge id<MTLCommandBuffer>)w.cb release];
	}
}

//------------------------------------------------------------------------------
// Memory management
//------------------------------------------------------------------------------

void MetalDevice::AllocBuffer(HardwareDeviceBuffer **hdBuff, const BufferType type,
		void *src, const size_t size, const string &desc) {
	if (!*hdBuff)
		*hdBuff = new MetalDeviceBuffer();

	MetalDeviceBuffer *metalBuff = dynamic_cast<MetalDeviceBuffer *>(*hdBuff);
	assert(metalBuff);

	// Handle the case of an empty buffer
	if (!size) {
		if (metalBuff->metalBuff) {
			FinishQueue();   // pending work may still reference the buffer

			FreeMemory(metalBuff->size);
			[(__bridge id<MTLBuffer>)metalBuff->metalBuff release];
			metalBuff->metalBuff = nullptr;
			metalBuff->size = 0;
		}
		return;
	}

	if (metalBuff->metalBuff) {
		// Check the size of the already allocated buffer
		if (size == metalBuff->size) {
			// I can reuse the buffer; just update the content
			if (src)
				memcpy([(__bridge id<MTLBuffer>)metalBuff->metalBuff contents], src, size);
			return;
		} else {
			// Free the buffer
			FinishQueue();   // pending work may still reference the buffer

			FreeMemory(metalBuff->size);
			[(__bridge id<MTLBuffer>)metalBuff->metalBuff release];
			metalBuff->metalBuff = nullptr;
			metalBuff->size = 0;
		}
	}

	if (desc != "")
		LR_LOG(deviceContext, "[Device " << GetName() << "] " << desc <<
				" buffer size: " << ToMemString(size));

	id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
	id<MTLBuffer> buff = [dev newBufferWithLength:size
		options:MTLResourceStorageModeShared];
	if (!buff)
		throw runtime_error("MTLDevice buffer allocation failed for " + desc);
	metalBuff->metalBuff = (__bridge_retained void *)buff;
	metalBuff->size = size;

	if (src)
		memcpy([(__bridge id<MTLBuffer>)metalBuff->metalBuff contents], src, size);

	AllocMemory(size);
}

void MetalDevice::FreeBuffer(HardwareDeviceBuffer **buff) {
	if (*buff) {
		if (!(*buff)->IsNull()) {
			MetalDeviceBuffer *metalBuff =
				dynamic_cast<MetalDeviceBuffer *>(*buff);
			assert(metalBuff);

			// A committed command buffer can still reference this MTLBuffer
			// (the tile loop's last iteration breaks before the readback /
			// FinishQueue pair) - Metal's queue thread would then touch freed
			// memory. Drain everything first.
			FinishQueue();

			FreeMemory(metalBuff->GetSize());
			[(__bridge id<MTLBuffer>)metalBuff->metalBuff release];
			metalBuff->metalBuff = nullptr;
		}

		delete *buff;
		*buff = nullptr;
	}
}

MetalDeviceKernel::~MetalDeviceKernel() {
	if (function)
		[(__bridge id<MTLFunction>)function release];
	if (pipeline)
		[(__bridge id<MTLComputePipelineState>)pipeline release];
	for (auto &p : args)
		delete[] (char *)p;
}

}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

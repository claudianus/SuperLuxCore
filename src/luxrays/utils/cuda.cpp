/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <memory>
#if !defined(LUXRAYS_DISABLE_CUDA)

#include <iostream>
#include <fstream>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "luxrays/core/context.h"
#include "luxrays/luxrays.h"
#include "luxrays/utils/utils.h"
#include "luxrays/utils/config.h"
#include "luxrays/utils/cudacache.h"
#include "luxrays/utils/oclcache.h"

#include <optix_function_table_definition.h>

using namespace std;
using namespace luxrays;

// Physical RAM in bytes (0 when undetectable — treated as "not enough").
static uint64_t PhysMemBytes() {
#if defined(_WIN32)
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof(ms);
	return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
#elif defined(__linux__)
	return (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
#else
	return 0;
#endif
}

// Compile kernels to SASS (sm_<cap> cubin) instead of PTX. Default: on when
// the machine has >= 20 GB RAM — with --split-compile, NVRTC's embedded
// ptxas peaks around ~12 GB on the ~100k-line PathOCL megakernel, and a
// ptxas OOM inside NVRTC aborts the process before the PTX fallback can
// run (below the guard we stay on PTX + driver JIT: slower first start,
// never fatal). SASS skips the driver PTX->SASS JIT entirely (~20 min for
// the megakernel) and survives driver updates, unlike the NVIDIA compute
// cache. LUX_CUDA_SASS=0 forces PTX; any other non-empty value forces SASS.
static bool CudaSassRequested() {
	static const bool v = [] {
		const char *e = getenv("LUX_CUDA_SASS");
		if (e && e[0])
			return e[0] != '0';
		return PhysMemBytes() >= (20ull << 30);
	}();
	return v;
}

static bool CudaCubinCapable(const bool forcePTX) {
	return !forcePTX && CudaSassRequested() &&
			(nvrtcGetCUBIN != nullptr) && (nvrtcGetCUBINSize != nullptr);
}

static string GetCuda10Architecture() {
	CUdevice device;
	int major, minor;
	CHECK_CUDA_ERROR(cuCtxGetDevice(&device));
	CHECK_CUDA_ERROR(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device));
	CHECK_CUDA_ERROR(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device));

        // v2.10: as nvrtc has been updated, the below code should not still be necessary...
        // we keep it "just-in-case"
        //
	//if ((major >= 7) && (minor >= 5)) {
		//// NVIDIA driver doesn't include NVIDA RTC (Run Time Compiler) so we ship
		//// CUDA 10 NRTC with LuxCore however it supports only up to Turing architecture
		//// (Ampere 8.0 architecture is not supported). So I have to bound the required
		//// architecture in order to not get an error.

		//major = 7;
		//minor = 5;
	//}

	return to_string(major) + to_string(minor);
}

//------------------------------------------------------------------------------
// cudaKernelCache
//------------------------------------------------------------------------------

bool cudaKernelCache::ForcedCompilePTX(
	const vector<string> &kernelsParameters, const string &kernelSource,
	const string &programName, std::unique_ptr<char[]> * ptx, size_t *ptxSize, string *error,
	bool forcePTX
) {
	if (error)
		*error = "";

	// Prefer compiling straight to SASS (cubin): it skips the driver's PTX JIT
	// step entirely and does not depend on the driver's PTX ISA version
	// (NVRTC can be newer than the installed driver). Fall back to PTX
	// (compute_<cap>) when CUBIN output is unavailable or the arch is unknown
	// to the loaded NVRTC. forcePTX (e.g. OptiX modules) skips the SASS path.
	const string arch = GetCuda10Architecture();
	const bool cubinCapable = CudaCubinCapable(forcePTX);
	const int archModes = cubinCapable ? 2 : 1;

	for (int mode = 0; mode < archModes; ++mode) {
		const bool toCubin = cubinCapable && (mode == 0);
		const string targetArch = "--gpu-architecture=" +
				(toCubin ? "sm_" + arch : "compute_" + arch);

		nvrtcProgram prog;
		CHECK_NVRTC_ERROR(nvrtcCreateProgram(&prog, kernelSource.c_str(), programName.c_str(), 0, nullptr, nullptr));

		vector<const char *> cudaOpts;
		cudaOpts.push_back("--device-as-default-execution-space");
		//cudaOpts.push_back("--disable-warnings");

		// Set target architecture, based on current device's capability
		cudaOpts.push_back(targetArch.c_str());

		// To display warning numbers
		cudaOpts.push_back("-Xcudafe");
		cudaOpts.push_back("--display_error_number");

		// To suppress warning: warning #550-D: variable "xyz" was set but never used
		cudaOpts.push_back("-Xcudafe");
		cudaOpts.push_back("--diag_suppress=550");

		// To suppress warning: warning #1055-D: types cannot be declared in anonymous unions
		cudaOpts.push_back("-Xcudafe");
		cudaOpts.push_back("--diag_suppress=1055");

		// To suppress warning: warning #68-D: integer conversion resulted in a change of sign
		cudaOpts.push_back("-Xcudafe");
		cudaOpts.push_back("--diag_suppress=68");

		// Accelerate compilation
		//cudaOpts.push_back("--Ofast-compile=min"); # Only 12.9+
		// SASS mode needs split-compile: the embedded ptxas run with
		// --split-compile=0 OOM-aborts the process on the PathOCL
		// megakernel (~100k lines). 4 parallel units measured ~12GB peak
		// and produced a valid 64MB sm_120 cubin in ~8min.
		cudaOpts.push_back(toCubin ? "--split-compile=4" : "--split-compile=0");

		// Enable pre-compiled headers
		cudaOpts.push_back("--pch");

		// Enable debug info
		//cudaOpts.push_back("-G");
		// Enable only debug line info
		//cudaOpts.push_back("--generate-line-info");

		for	(auto const &p : kernelsParameters)
			cudaOpts.push_back(p.c_str());

		// For some debug
		//for (uint i = 0; i < cudaOpts.size(); ++i)
		//	cout << "Opt #" << i <<" : [" << cudaOpts[i] << "]\n";

		const nvrtcResult compilationResult = nvrtcCompileProgram(prog,
				cudaOpts.size(),
				(cudaOpts.size() > 0) ? &cudaOpts[0] : nullptr);

		size_t logSize;
		CHECK_NVRTC_ERROR(nvrtcGetProgramLogSize(prog, &logSize));
		auto log = std::make_unique<char[]>(logSize);
		CHECK_NVRTC_ERROR(nvrtcGetProgramLog(prog, log.get()));

		*error = string(log.get());

		if (compilationResult != NVRTC_SUCCESS) {
			CHECK_NVRTC_ERROR(nvrtcDestroyProgram(&prog));
			// Retry as PTX if the SASS target failed
			if (toCubin && archModes > 1)
				continue;
			return false;
		}

		if (toCubin) {
			// Obtain the SASS cubin (ELF) from the program.
			CHECK_NVRTC_ERROR(nvrtcGetCUBINSize(prog, ptxSize));
			*ptx = std::make_unique<char[]>(*ptxSize);
			CHECK_NVRTC_ERROR(nvrtcGetCUBIN(prog, ptx->get()));
		} else {
			// Obtain PTX from the program.
			CHECK_NVRTC_ERROR(nvrtcGetPTXSize(prog, ptxSize));
			*ptx = std::make_unique<char[]>(*ptxSize);
			CHECK_NVRTC_ERROR(nvrtcGetPTX(prog, ptx->get()));
		}

		CHECK_NVRTC_ERROR(nvrtcDestroyProgram(&prog));

		return true;
	}

	return false;
}

//------------------------------------------------------------------------------
// cudaKernelPersistentCache
//------------------------------------------------------------------------------

std::filesystem::path cudaKernelPersistentCache::GetCacheDir(const string &applicationName) {
	return luxrays::GetCacheDir() / "cuda_kernel_cache" / SanitizeFileName(applicationName);
}

cudaKernelPersistentCache::cudaKernelPersistentCache(const string &applicationName) {
	appName = applicationName;

	// Crate the cache directory
	std::filesystem::create_directories(GetCacheDir(appName));
}

cudaKernelPersistentCache::~cudaKernelPersistentCache() {
}

bool cudaKernelPersistentCache::CompilePTX(const vector<string> &kernelsParameters,
		const string &kernelSource, const string &programName,
		std::unique_ptr<char[]> *ptx, size_t *ptxSize, bool *cached, string *error,
		bool forcePTX) {
	if (error)
		*error = "";

	// Check if the kernel is available in the cache

	// CUBIN-capable NVRTC compiles to SASS (sm_<cap>); older ones produce PTX
	// (compute_<cap>). cuModuleLoadDataEx auto-detects either format, so the
	// suffix is only a cache-key detail. forcePTX callers (OptiX) always get
	// PTX since optixModuleCreateFromPTX cannot ingest cubins.
	const bool cubinCapable = CudaCubinCapable(forcePTX);
	const string kernelName =
			oclKernelPersistentCache::HashString(oclKernelPersistentCache::ToOptsString(kernelsParameters))
			+ "-" +
			oclKernelPersistentCache::HashString(kernelSource) +
                        (cubinCapable ?
				"_sm_" + GetCuda10Architecture() + ".cubin" :
				"_compute_" + GetCuda10Architecture() + ".ptx");
	const std::filesystem::path dirPath = GetCacheDir(appName);
	const std::filesystem::path filePath = dirPath / kernelName;
	const string fileName = filePath.generic_string();

	*cached = false;
	if (!std::filesystem::exists(filePath)) {
		// It isn't available, compile the source

		// Create the file only if the binaries include something
		if (ForcedCompilePTX(kernelsParameters, kernelSource, programName, ptx, ptxSize, error, forcePTX)) {
			// Add the kernel to the cache
			std::filesystem::create_directories(dirPath);

			// The use of std::filesystem::path is required for UNICODE support: fileName
			// is supposed to be UTF-8 encoded.
			std::ofstream file(std::filesystem::path(fileName),
					std::ofstream::out |
					std::ofstream::binary |
					std::ofstream::trunc);

			// Write the binary hash
			const u_int hashBin = oclKernelPersistentCache::HashBin(ptx->get(), *ptxSize);
			file.write((char *)&hashBin, sizeof(int));

			file.write(ptx->get(), *ptxSize);
			// Check for errors
			char buf[512];
			if (file.fail()) {
				sprintf(buf, "Unable to write kernel file cache %s", fileName.c_str());
				throw runtime_error(buf);
			}

			file.close();

			return true;
		} else
			return false;
	} else {
		const size_t fileSize = std::filesystem::file_size(filePath);

		if (fileSize > 4) {
			*ptxSize = fileSize - 4;

			*ptx = std::make_unique<char[]>(*ptxSize);

			// The use of std::filesystem::path is required for UNICODE support: fileName
			// is supposed to be UTF-8 encoded.
			std::ifstream file(std::filesystem::path(fileName),
					std::ifstream::in | std::ifstream::binary);

			// Read the binary hash
			u_int hashBin;
			file.read((char *)&hashBin, sizeof(int));

			file.read(ptx->get(), *ptxSize);
			// Check for errors
			char buf[512];
			if (file.fail()) {
				sprintf(buf, "Unable to read kernel file cache %s", fileName.c_str());
				throw runtime_error(buf);
			}

			file.close();

			// Check the binary hash
			if (hashBin != oclKernelPersistentCache::HashBin(ptx->get(), *ptxSize)) {
				// Something wrong in the file, remove the file and retry
				std::filesystem::remove(filePath);
				return CompilePTX(kernelsParameters, kernelSource, programName, ptx, ptxSize, cached, error, forcePTX);
			} else {
				*cached = true;

				return true;
			}
		} else {
			// Something wrong in the file, remove the file and retry
			std::filesystem::remove(filePath);
			return CompilePTX(kernelsParameters, kernelSource, programName, ptx, ptxSize, cached, error, forcePTX);
		}
	}
}

CUmodule cudaKernelPersistentCache::Compile(const vector<string> &kernelsParameters,
		const string &kernelSource, const string &programName,
		bool *cached, string *error) {
	std::unique_ptr<char[]> ptx;
	size_t ptxSize;
	if (CompilePTX(kernelsParameters, kernelSource, programName, &ptx, &ptxSize, cached, error)) {
		CUmodule module;
		CHECK_CUDA_ERROR(cuModuleLoadDataEx(&module, ptx.get(), 0, 0, 0));

		return module;
	} else
		return nullptr;
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

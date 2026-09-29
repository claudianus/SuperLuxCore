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

#ifndef _LUXRAYS_METALDEVICE_H
#define _LUXRAYS_METALDEVICE_H

// C++-SAFE header: ObjC types appear only as opaque void handles. The
// Metal/Foundation imports live exclusively in the .mm translation
// units (imported FIRST, before any LuxCore header can pull dispatch
// etc. in C++ mode and poison __OBJC__ for everything after).

#include <atomic>
#include <functional>
#include <unordered_set>

#include "luxrays/core/hardwaredevice.h"
#include "luxrays/core/intersectiondevice.h"
#include "luxrays/usings.h"

#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)

namespace luxrays {

// ObjC handle types (void* so any C++ TU can hold them)
typedef void *MTLDeviceHandle;
typedef void *MTLCommandQueueHandle;
typedef void *MTLBufferHandle;
typedef void *MTLLibraryHandle;
typedef void *MTLFunctionHandle;
typedef void *MTLComputePipelineStateHandle;
typedef void *MTLCommandBufferHandle;

//------------------------------------------------------------------------------
// MetalDeviceDescription
//------------------------------------------------------------------------------

class MetalDeviceDescription : public DeviceDescription {
public:
	// metalDevice ownership is transferred (the description retains it)
	MetalDeviceDescription(MTLDeviceHandle metalDevice, const size_t devIndex);
	virtual ~MetalDeviceDescription();

	virtual int GetComputeUnits() const;
	virtual u_int GetNativeVectorWidthFloat() const;
	virtual size_t GetMaxMemory() const;
	virtual size_t GetMaxMemoryAllocSize() const;
	virtual bool HasOutOfCoreMemorySupport() const;

	MTLDeviceHandle GetMetalDevice() const { return metalDevice; }

	friend class Context;

protected:
	static void AddDeviceDescs(std::vector<DeviceDescriptionUPtr> &descriptions);

	MTLDeviceHandle metalDevice;
};

//------------------------------------------------------------------------------
// MetalDeviceKernel
//------------------------------------------------------------------------------

class MetalDeviceBuffer;

class MetalDeviceKernel : public HardwareDeviceKernel {
public:
	MetalDeviceKernel() : function(nullptr), pipeline(nullptr) { }
	virtual ~MetalDeviceKernel();

	bool IsNull() const {
		return (pipeline == nullptr);
	}

	friend class MetalDevice;

protected:
	MTLFunctionHandle function;
	MTLComputePipelineStateHandle pipeline;

	// Per-index argument storage (CUDA-style deferred marshalling):
	// a slot holds either copied bytes (args) or a buffer pointer
	// (buffs) - exactly one is set at dispatch time.
	// roles disambiguates a NULL buffer arg (the engine passes nullptr
	// for disabled channels - e.g. filmDenoiser buffers, unused
	// imageMap slots) from an index the engine never set: a NULL
	// buffer still consumes its pointer slot, otherwise every later
	// pointer arg shifts and the GPU reads wild addresses.
	enum class ArgRole : uint8_t { UNSET, SCALAR, POINTER };
	std::vector<void *> args;
	std::vector<const HardwareDeviceBuffer *> buffs;
	std::vector<ArgRole> roles;

public:
	// Marshalling map (parsed from the cl2msl layout JSON at GetKernel
	// time). Engine SetKernelArg order == the ORIGINAL OpenCL signature
	// order (argOrder), NOT direct-then-table:
	//   scalars -> KernelScalarsN bundle (buffer(0))
	//   direct pointers -> [[buffer(2+)]] in ptrNames order
	//   table pointers -> KernelPtrsN bundle (buffer(1), device space)
	struct Marshalling {
		std::vector<std::string> scalarNames;   // signature order
		std::vector<size_t> scalarOffsets;
		std::vector<size_t> scalarSizes;       // MSL member size (bytes)
		size_t scalarBundleSize;
		std::vector<std::string> ptrNames;      // signature order
		std::vector<int> ptrSlots;              // >=2 direct, -1 = table
		std::vector<size_t> ptrTableOffsets;
		size_t ptrBundleSize;
		bool hasScalarBundle;
		bool hasPtrBundle;
		// Original-signature arg sequence: true = pointer, false = scalar.
		// The engine's SetKernelArg call sequence follows this order; the
		// two cursors (scalar slot / pointer slot) advance per entry.
		std::vector<bool> argIsPointer;
		// Per arg-order entry: index into ptrNames / scalarNames (-1 =
		// name not found - the arg is then skipped safely).
		std::vector<int> argPtrIndex;
		std::vector<int> argScalarIndex;
	};
	Marshalling marsh;

	// Dispatch marshal cache: kernel args are quasi-static between
	// dispatches (re-set only on init / the denoiser warm-up flip), so
	// EnqueueKernel skips the per-arg walk + memcpys when nothing
	// changed. SetKernelArg*/SetKernelArgBuffer flip argsDirty
	// (atomic: arg setters run under the engine mutex, the dispatch
	// path under inFlightMutex).
	std::atomic<bool> argsDirty{true};
	std::vector<uint8_t> marshalScalar, marshalPtr;
	// Per ptrSlots entry: resolved buffer for direct slots,
	// nullptr = unbound (bind nil).
	std::vector<const MetalDeviceBuffer *> marshalDirect;
	// Table-slot buffers needing useResource residency calls.
	std::vector<const MetalDeviceBuffer *> marshalTable;
	// Union of both for the pendingBuffers hazard scan.
	std::vector<const MetalDeviceBuffer *> marshalUsed;
};

//------------------------------------------------------------------------------
// MetalDeviceProgram
//------------------------------------------------------------------------------

class MetalDeviceProgram : public HardwareDeviceProgram {
public:
	MetalDeviceProgram();
	virtual ~MetalDeviceProgram();

	bool IsNull() const {
		return (library == nullptr);
	}

	friend class MetalDevice;

protected:
	MTLLibraryHandle library;

	// The translator's per-kernel argument marshalling map (raw JSON)
	std::string layoutJson;

	// Compiled-pipeline cache (MTLBinaryArchive). Compiling the engine
	// kernel set cold costs ~20-30s on the M5 Pro (the AdvancePaths
	// variants inline the material/texture VMs); the archive stores the
	// compiled pipelines so later sessions only do archive lookups.
	// Serialized to archivePath when the program is destroyed (if dirty).
	void *binaryArchive;     // id<MTLBinaryArchive> (retained) or null
	std::string archivePath;
	bool archiveDirty;
	// MTLBinaryArchive mutation is not thread-safe: serializes
	// addComputePipelineFunctionsWithDescriptor across the parallel
	// GetKernel() workers (the PSO compile itself stays outside)
	std::mutex archiveMutex;
};

//------------------------------------------------------------------------------
// MetalDeviceBuffer
//------------------------------------------------------------------------------

class MetalDeviceBuffer : public HardwareDeviceBuffer {
public:
	MetalDeviceBuffer() : metalBuff(nullptr), size(0) { }
	virtual ~MetalDeviceBuffer() {
	}

	bool IsNull() const {
		return (metalBuff == nullptr);
	}

	size_t GetSize() const {
		return size;
	}

	void *GetHostVisiblePointer() const override;

	MTLBufferHandle GetMetalBuffer() const { return metalBuff; }

	friend class MetalDevice;

protected:
	MTLBufferHandle metalBuff;
	size_t size;
};

//------------------------------------------------------------------------------
// MetalDevice
//------------------------------------------------------------------------------

class MetalDevice : virtual public HardwareDevice {
public:
	MetalDevice(const Context & context,
		MetalDeviceDescriptionConstRef desc, const size_t devIndex);
	virtual ~MetalDevice();

	virtual const DeviceDescription& GetDeviceDesc() const { return deviceDesc; }

	virtual void PushThreadCurrentDevice();
	virtual void PopThreadCurrentDevice();

	//--------------------------------------------------------------------------
	// Kernels handling for hardware (aka GPU) only applications
	//--------------------------------------------------------------------------

	virtual HardwareDeviceProgramUPtr CompileProgram(
		const std::vector<std::string> &programParameters,
		const std::string &programSource,
		const std::string &programName
	) override;

	virtual HardwareDeviceKernelUPtr GetKernel(
		HardwareDeviceProgramRef program,
		const std::string &kernelName
	) override;
	virtual u_int GetKernelWorkGroupSize(HardwareDeviceKernelRPtr kernel) override;
	// PSO creation is thread-safe; the binary archive is serialized on a
	// per-program mutex inside GetKernel()
	virtual bool HasThreadSafeKernelCreation() const override { return true; }
	virtual void SetKernelArg(HardwareDeviceKernelRPtr kernel,
			const u_int index, const size_t size, const void *arg) override;

	virtual void EnqueueKernel(HardwareDeviceKernelRPtr kernel,
			const HardwareDeviceRange &globalSize,
			const HardwareDeviceRange &workGroupSize) override;
	virtual void EnqueueReadBuffer(const HardwareDeviceBuffer *buff,
			const bool blocking, const size_t size, void *ptr) override;
	virtual void EnqueueWriteBuffer(const HardwareDeviceBuffer *buff,
			const bool blocking, const size_t size, const void *ptr) override;
	virtual void FlushQueue() override;
	virtual void FinishQueue() override;

	//--------------------------------------------------------------------------
	// Memory management for hardware (aka GPU) only applications
	//--------------------------------------------------------------------------

	virtual void AllocBuffer(HardwareDeviceBuffer **buff, const BufferType type,
			void *src, const size_t size, const std::string &desc = "");
	virtual void FreeBuffer(HardwareDeviceBuffer **buff);

	// Native handles + command-buffer tracking for device subsystems that
	// bypass the cl2msl kernel marshalling (e.g. the native HWRT kernel in
	// metalrtaccel.mm). CommitAndTrackInFlight() performs the same
	// commit-under-lock + in-flight bookkeeping as EnqueueKernel() so
	// FinishQueue() and EnqueueWriteBuffer() ordering guarantees hold for
	// externally encoded command buffers too.
	MTLDeviceHandle GetMTLDevice() const { return device; }
	MTLCommandQueueHandle GetMTLCommandQueue() const { return queue; }
	void CommitAndTrackInFlight(MTLCommandBufferHandle commandBuffer,
			const std::vector<const MetalDeviceBuffer *> &buffers);
	// Encode one compute pass into the shared pendingCB instead of a
	// dedicated command buffer: an RT trace no longer forces a commit
	// boundary through the batched compute work (CommitAndTrackInFlight
	// flushes pendingCB first, so every trace split the batch). The
	// callback receives a fresh MTLComputeCommandEncoder as void* (this
	// header stays ObjC-free; callers live in .mm files). usedBuffers
	// join the batch's conflict set exactly like EnqueueKernel.
	void EncodePendingCompute(const std::function<void(void *)> &encode,
			const std::vector<const MetalDeviceBuffer *> &usedBuffers);

	friend class Context;

protected:
	virtual void SetKernelArgBuffer(
		HardwareDeviceKernelRPtr kernel,
		const u_int index, const HardwareDeviceBuffer *buff
	) override;

	MetalDeviceDescriptionConstRef deviceDesc;

	MTLDeviceHandle device;
	MTLCommandQueueHandle queue;

	// A committed-but-unwaited command buffer plus the buffers its
	// dispatches may still read (thread-safe: the engine runs render
	// threads on worker threads).
	//
	// EnqueueWriteBuffer() is a plain host memcpy into the shared buffer
	// (there is no queued device-side copy), so it must not clobber a
	// buffer an in-flight dispatch still reads - OpenCL gets that
	// ordering from its in-order queue. buffers[] is what lets the write
	// path wait only on the conflicts instead of draining the queue for
	// every upload.
	struct InFlightDispatch {
		MTLCommandBufferHandle cb;
		std::vector<const MetalDeviceBuffer *> buffers;
	};

	std::mutex inFlightMutex;
	std::vector<InFlightDispatch> inFlightWork;

	// Deferred command-buffer batching: consecutive EnqueueKernel()
	// calls encode into pendingCB and are committed together at the
	// first synchronization point (FinishQueue/EnqueueReadBuffer/
	// EnqueueWriteBuffer-conflict/AllocBuffer/FreeBuffer) or when the
	// encoder cap is hit. The dense microkernel loop otherwise pays a
	// full command-buffer + commit + track cycle per dispatch (~15x
	// per iteration in dense PATHOCL).
	// pendingBuffers duplicates inFlightWork.buffers semantics for the
	// still-uncommitted encoders so EnqueueWriteBuffer's conflict scan
	// sees them too. All pending state is under inFlightMutex.
	MTLCommandBufferHandle pendingCB;
	// Set, not vector: the EnqueueWriteBuffer conflict scan runs on
	// every upload, and ~64 pending encoders x their buffer lists made
	// the linear scan O(thousands) per write.
	std::unordered_set<const MetalDeviceBuffer *> pendingBuffers;
	u_int pendingEncoderCount;

	// Commit pendingCB into inFlightWork. inFlightMutex must be held.
	void CommitPendingLocked();
	void FlushPending();   // lock + CommitPendingLocked, no wait
};

}

#endif

#endif	/* _LUXRAYS_METALDEVICE_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

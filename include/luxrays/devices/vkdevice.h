/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
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

#ifndef _LUXRAYS_VKDEVICE_H
#define	_LUXRAYS_VKDEVICE_H

// C++-SAFE header: Vulkan types appear only as opaque void handles. The
// vulkan.h include lives exclusively in vkdevice.cpp so consumers of this
// header (context.cpp, engines) need no Vulkan SDK.

#include <map>
#include <vector>
#include <string>

#include "luxrays/core/hardwaredevice.h"
#include "luxrays/core/intersectiondevice.h"
#include "luxrays/usings.h"

#if !defined(LUXRAYS_DISABLE_VULKAN)

namespace luxrays {

// Opaque Vulkan handles
typedef void *VkInstanceHandle;
typedef void *VkPhysicalDeviceHandle;
typedef void *VkDeviceHandle;
typedef void *VkQueueHandle;
typedef void *VkCommandPoolHandle;
typedef void *VkCommandBufferHandle;
typedef void *VkBufferHandle;
typedef void *VkDeviceMemoryHandle;
typedef void *VkShaderModuleHandle;
typedef void *VkPipelineHandle;
typedef void *VkPipelineLayoutHandle;
typedef void *VkDescriptorSetLayoutHandle;
typedef void *VkDescriptorPoolHandle;
typedef void *VkDescriptorSetHandle;
typedef void *VkFenceHandle;

//------------------------------------------------------------------------------
// VulkanDeviceDescription
//------------------------------------------------------------------------------

class VulkanDeviceDescription : public DeviceDescription {
public:
	VulkanDeviceDescription(VkPhysicalDeviceHandle physDev,
			VkInstanceHandle instance, const size_t devIndex);
	virtual ~VulkanDeviceDescription();

	virtual int GetComputeUnits() const;
	virtual u_int GetNativeVectorWidthFloat() const;
	virtual size_t GetMaxMemory() const;
	virtual size_t GetMaxMemoryAllocSize() const;
	virtual bool HasOutOfCoreMemorySupport() const { return false; }

	VkPhysicalDeviceHandle GetVulkanPhysicalDevice() const { return physDevice; }
	VkInstanceHandle GetVulkanInstance() const { return vkInstance; }

	// Cached properties (populated at enumeration)
	uint32_t maxComputeWorkGroupInvocations;
	uint32_t maxPushConstantsSize;
	uint32_t maxStorageBuffersPerStage;
	uint32_t maxBoundDescriptorSets;
	uint64_t maxStorageBufferRange;
	bool hasRayQuery;
	bool hasAccelStruct;
	bool hasUnifiedMemory;

	friend class Context;
protected:
	static void AddDeviceDescs(std::vector<DeviceDescriptionUPtr> &descriptions);

	VkPhysicalDeviceHandle physDevice;
	VkInstanceHandle vkInstance;
};

//------------------------------------------------------------------------------
// VulkanDeviceBuffer
//------------------------------------------------------------------------------

class VulkanDeviceBuffer : public HardwareDeviceBuffer {
public:
	VulkanDeviceBuffer() : buff(nullptr), mem(nullptr), size(0), deviceAddr(0) { }
	virtual ~VulkanDeviceBuffer() { }

	bool IsNull() const override { return buff == nullptr; }
	size_t GetSize() const override { return size; }

	VkBufferHandle buff;
	VkDeviceMemoryHandle mem;
	size_t size;
	uint64_t deviceAddr; // VK_KHR_buffer_device_address (0 when not needed)
};

//------------------------------------------------------------------------------
// VulkanDeviceProgram
//------------------------------------------------------------------------------

class VulkanDeviceProgram : public HardwareDeviceProgram {
public:
	VulkanDeviceProgram() : owner(nullptr) { }
	virtual ~VulkanDeviceProgram();

	// Owning VkDevice (needed to destroy the module)
	VkDeviceHandle owner;

	bool IsNull() const { return cacheKey.empty(); }

	// Split compilation: the monolithic module inlines the whole call
	// graph into every entry point serially (O(minutes) per kernel in a
	// single clspv process). Instead CompileProgram emits one LLVM
	// bitcode module, then prunes + compiles each __kernel to its own
	// SPIR-V in parallel (internalize+globaldce via opt, clspv -x ir).
	// cacheKey locates <key>-<kernel>.spv / .map in the vkcache dir.
	// kernelBasePaths overrides that with each kernel's resolved module
	// path base (per-kernel pruned-module hash — an edit to kernel A no
	// longer invalidates kernel B's .spv).
	std::string cacheDir;
	std::string cacheKey;
	std::vector<std::string> kernelNames;
	std::map<std::string, std::string> kernelBasePaths;

	// Per-kernel argument layout parsed from clspv's descriptor map:
	// for kernel arg index i -> role + descriptor binding or POD offset.
	struct ArgInfo {
		enum Kind { NONE, BUFFER, POD_UBO, POD_PUSHCONST, LOCAL } kind = NONE;
		uint32_t binding = 0;   // descriptor binding (BUFFER/POD_UBO)
		uint32_t podOffset = 0; // byte offset inside POD blob
		uint32_t podSize = 0;
	};
	struct KernelLayout {
		std::vector<ArgInfo> args;       // indexed by kernel arg ordinal
		uint32_t podUBOBinding = ~0u;    // binding of the clustered POD block
		uint32_t podUBOSize = 0;
		bool podIsUniform = false;       // block lives in Uniform class (UBO),
									   // not StorageBuffer (SSBO)
		uint32_t pushConstSize = 0;      // byte size of the push-constant block
		uint32_t localSizeCount = 0;
	};
	std::map<std::string, KernelLayout> layouts;
};

//------------------------------------------------------------------------------
// VulkanDeviceKernel
//------------------------------------------------------------------------------

class VulkanDeviceKernel : public HardwareDeviceKernel {
public:
	VulkanDeviceKernel() : pipeline(nullptr), pipelineLayout(nullptr),
		setLayout(nullptr), descPool(nullptr), podBuffer(nullptr),
		podBufferMem(nullptr), owner(nullptr) { }
	virtual ~VulkanDeviceKernel();

	VkDeviceHandle owner;

	bool IsNull() const { return pipeline == nullptr; }

	friend class VulkanDevice;

protected:
	VkShaderModuleHandle shaderModule = nullptr;
	VkPipelineHandle pipeline;
	VkPipelineLayoutHandle pipelineLayout;
	VkDescriptorSetLayoutHandle setLayout;
	VkDescriptorPoolHandle descPool;

	// Deferred arg marshalling (same convention as MetalDeviceKernel):
	// per-arg role + either a copied POD blob or a buffer pointer.
	enum class ArgRole : uint8_t { UNSET, SCALAR, POINTER };
	std::vector<std::vector<char>> args;   // POD payload per arg index
	std::vector<const HardwareDeviceBuffer *> buffs;
	std::vector<ArgRole> roles;

	// POD args are packed into a single UBO (clspv -pod-ubo style).
	VkBufferHandle podBuffer;
	VkDeviceMemoryHandle podBufferMem;
	size_t podBufferSize = 0;

	VulkanDeviceProgram::KernelLayout layout;

	// Workgroup size baked into the pipeline via workgroup_size_x spec const
	uint32_t localSizeX = 0;
	// Module-scope __constant SSBO to bind when this kernel's module has
	// one (per-kernel modules carry only their own constants).
	VkBufferHandle moduleConstBuff = nullptr;
	VkDeviceMemoryHandle moduleConstMem = nullptr;
	uint32_t moduleConstBinding = ~0u;

	// Descriptor sets in flight; recycled on FinishQueue.
	std::vector<VkDescriptorSetHandle> inflightSets;
};

//------------------------------------------------------------------------------
// VulkanDevice
//------------------------------------------------------------------------------

class VulkanDevice : virtual public HardwareDevice {
public:
	VulkanDevice(const Context &context,
			VulkanDeviceDescriptionConstRef desc, const size_t devIndex);
	virtual ~VulkanDevice();

	virtual void Start();
	virtual void Interrupt();
	virtual void Stop();

	virtual DeviceDescriptionConstRef GetDeviceDesc() const override { return deviceDesc; }

	// Kernels
	virtual HardwareDeviceProgramUPtr CompileProgram(
			const std::vector<std::string> &programParameters,
			const std::string &programSource,
			const std::string &programName);
	virtual HardwareDeviceKernelUPtr GetKernel(
			HardwareDeviceProgramRef program,
			const std::string &kernelName);
	virtual u_int GetKernelWorkGroupSize(HardwareDeviceKernelRPtr kernel);
	virtual void SetKernelArg(HardwareDeviceKernelRPtr kernel,
			const u_int index, const size_t size, const void *arg);
	virtual void EnqueueKernel(HardwareDeviceKernelRPtr kernel,
			const HardwareDeviceRange &globalSize,
			const HardwareDeviceRange &workGroupSize);
	virtual void EnqueueReadBuffer(const HardwareDeviceBuffer *buff,
			const bool blocking, const size_t size, void *ptr);
	virtual void EnqueueWriteBuffer(const HardwareDeviceBuffer *buff,
			const bool blocking, const size_t size, const void *ptr);
	virtual void FlushQueue();
	virtual void FinishQueue();

	// Memory
	virtual void AllocBuffer(HardwareDeviceBuffer **buff, const BufferType type,
			void *src, const size_t size, const std::string &desc = "");
	virtual void FreeBuffer(HardwareDeviceBuffer **buff);

	VkDeviceHandle GetVulkanDevice() const { return device; }
	VkQueueHandle GetVulkanQueue() const { return queue; }
	VkCommandPoolHandle GetVulkanCommandPool() const { return cmdPool; }

	// True when the device was created with ray query + AS support.
	bool HasRayTracingSupport() const { return hasRayTracing; }

protected:
	virtual void SetKernelArgBuffer(HardwareDeviceKernelRPtr kernel,
			const u_int index, const HardwareDeviceBuffer *buff);

	// Command batching: Enqueue* appends to the open command buffer;
	// FlushQueue submits. Read/writes on host-visible memory are direct
	// memcpy (unified memory / ReBAR); a staging path is a later opt.
	VkCommandBufferHandle BeginCmd();
	void EnsureCmdOpen();

	VulkanDeviceDescriptionConstRef deviceDesc;
	VkInstanceHandle instance;
	VkDeviceHandle device;
	VkQueueHandle queue;
	uint32_t queueFamily;
	VkCommandPoolHandle cmdPool;
	VkCommandBufferHandle openCmd;
	bool hasRayTracing;
};

}
#endif
#endif

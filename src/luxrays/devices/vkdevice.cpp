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

#if !defined(LUXRAYS_DISABLE_VULKAN)

#define VK_ENABLE_BETA_EXTENSIONS
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "luxrays/devices/vkdevice.h"
#include "luxrays/kernels/kernels.h"
#include "luxrays/utils/oclcache.h"
#include "luxrays/utils/properties.h"
#include "luxrays/utils/strutils.h"

// volk: meta-loader that dlopens libvulkan/libMoltenVK and fills the
// global function pointer table (same role clew/cuew play for OCL/CUDA).
#include "volk.h"

using namespace std;
using namespace luxrays;

static bool vulkanInitialized = false;
static bool vulkanAvailable = false;

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) \
	throw runtime_error("Vulkan error " + ToString(r_) + " at " __FILE__ ":" + ToString(__LINE__)); } while (0)

namespace luxrays {

// clspv binary used to translate the OpenCL kernel corpus to SPIR-V.
static string GetClspvPath() {
	const char *env = getenv("LUXRAYS_CLSPV");
	if (env && env[0])
		return env;
	return "clspv"; // PATH lookup
}

static string GetClspvReflectionPath() {
	const char *env = getenv("LUXRAYS_CLSPV_REFLECTION");
	if (env && env[0])
		return env;
	string c = GetClspvPath();
	const size_t slash = c.find_last_of('/');
	if (slash != string::npos)
		return c.substr(0, slash + 1) + "clspv-reflection";
	return "clspv-reflection";
}

static void InitVulkanLibrary() {
	if (vulkanInitialized)
		return;
	vulkanInitialized = true;

	// volkInitialize dlopens the loader (or libMoltenVK directly).
	if (volkInitialize() == VK_SUCCESS)
		vulkanAvailable = true;
}

//------------------------------------------------------------------------------
// VulkanDeviceDescription
//------------------------------------------------------------------------------

VulkanDeviceDescription::VulkanDeviceDescription(VkPhysicalDeviceHandle physDev,
		VkInstanceHandle instance, const size_t devIndex) :
		DeviceDescription("?", DEVICE_TYPE_VULKAN_GPU),
		physDevice(physDev), vkInstance(instance),
		maxComputeWorkGroupInvocations(128), maxPushConstantsSize(128),
		maxStorageBuffersPerStage(64), maxBoundDescriptorSets(4),
		maxStorageBufferRange(1u << 27),
		hasRayQuery(false), hasAccelStruct(false), hasUnifiedMemory(false) {

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties((VkPhysicalDevice)physDev, &props);
	name = props.deviceName;
	maxComputeWorkGroupInvocations = props.limits.maxComputeWorkGroupInvocations;
	maxPushConstantsSize = props.limits.maxPushConstantsSize;
	maxStorageBuffersPerStage = props.limits.maxPerStageDescriptorStorageBuffers;
	maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;
	maxStorageBufferRange = props.limits.maxStorageBufferRange;

	// Extension scan
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)physDev, nullptr, &extCount, nullptr);
	vector<VkExtensionProperties> exts(extCount);
	vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)physDev, nullptr, &extCount, exts.data());
	for (const auto &e : exts) {
		if (!strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME))
			hasRayQuery = true;
		if (!strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
			hasAccelStruct = true;
	}

	// Unified memory heuristic (Apple / integrated)
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties((VkPhysicalDevice)physDev, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
				(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			hasUnifiedMemory = true;
	}
}

VulkanDeviceDescription::~VulkanDeviceDescription() { }

int VulkanDeviceDescription::GetComputeUnits() const {
	// Approximation: subgroup count is not directly exposed; use
	// maxComputeWorkGroupCount[0]/1024 as a loose proxy is pointless.
	// Report shader-omega cores is impossible via core Vulkan; return
	// a sane constant — used only for informational logs.
	return 64;
}

u_int VulkanDeviceDescription::GetNativeVectorWidthFloat() const { return 4; }

size_t VulkanDeviceDescription::GetMaxMemory() const {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties((VkPhysicalDevice)physDevice, &mp);
	size_t total = 0;
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
		if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			total = std::max<size_t>(total, mp.memoryHeaps[i].size);
	return total ? total : std::numeric_limits<size_t>::max();
}

size_t VulkanDeviceDescription::GetMaxMemoryAllocSize() const {
	return std::min<size_t>(GetMaxMemory(), maxStorageBufferRange);
}

void VulkanDeviceDescription::AddDeviceDescs(vector<DeviceDescriptionUPtr> &descriptions) {
	InitVulkanLibrary();
	if (!vulkanAvailable)
		return;

	// RT on MoltenVK is opt-in: make sure the env var is set before the
	// instance is created (MoltenVK reads config at instance time).
#if defined(__APPLE__)
	if (getenv("LUXRAYS_VULKAN_RT") && !getenv("MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING"))
		setenv("MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING", "1", 1);
#endif

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "LuxCore";
	app.apiVersion = VK_API_VERSION_1_3;

	// Enable portability enumeration only when the implementation
	// advertises it (MoltenVK 1.4 removed it — unconditional enumeration).
	uint32_t nInstExt = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &nInstExt, nullptr);
	vector<VkExtensionProperties> instExts(nInstExt);
	vkEnumerateInstanceExtensionProperties(nullptr, &nInstExt, instExts.data());
	vector<const char *> enabledInstExts;
	VkInstanceCreateFlags instFlags = 0;
	for (const auto &e : instExts) {
		if (!strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
			enabledInstExts.push_back(e.extensionName);
			instFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
	}

	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.flags = instFlags;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)enabledInstExts.size();
	ici.ppEnabledExtensionNames = enabledInstExts.data();

	VkInstance inst = VK_NULL_HANDLE;
	if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS)
		return;
	volkLoadInstance(inst);

	uint32_t nPhys = 0;
	vkEnumeratePhysicalDevices(inst, &nPhys, nullptr);
	vector<VkPhysicalDevice> physDevs(nPhys);
	vkEnumeratePhysicalDevices(inst, &nPhys, physDevs.data());

	size_t idx = descriptions.size();
	for (auto pd : physDevs) {
		// GPUs only — skip CPU lavapipe-class devices unless nothing else
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pd, &p);
		if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
				p.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			continue;
		descriptions.push_back(make_unique<VulkanDeviceDescription>(
				(VkPhysicalDeviceHandle)pd, (VkInstanceHandle)inst, idx++));
	}
}

//------------------------------------------------------------------------------
// VulkanDevice
//------------------------------------------------------------------------------

VulkanDevice::VulkanDevice(const Context &context,
		VulkanDeviceDescriptionConstRef desc, const size_t devIndex) :
		Device(context, devIndex), HardwareDevice(),
		deviceDesc(desc), instance(desc.GetVulkanInstance()),
		device(nullptr), queue(nullptr), queueFamily(0),
		cmdPool(nullptr), openCmd(nullptr), hasRayTracing(false) {
	deviceName = desc.GetName() + " Vulkan";
}

VulkanDevice::~VulkanDevice() {
	if (device) {
		if (cmdPool)
			vkDestroyCommandPool((VkDevice)device, (VkCommandPool)cmdPool, nullptr);
		vkDestroyDevice((VkDevice)device, nullptr);
	}
}

VulkanDeviceProgram::~VulkanDeviceProgram() {
	// The device is owned by VulkanDevice; programs are destroyed while
	// the device is still alive (program lifetime is strictly shorter).
	// shaderModule is destroyed by the owning device at Stop() via the
	// device handle stored here — do it lazily through a registered list?
	// Simpler: keep a device pointer in the program at creation time.
	if (shaderModule && owner)
		vkDestroyShaderModule((VkDevice)owner, (VkShaderModule)shaderModule, nullptr);
}

VulkanDeviceKernel::~VulkanDeviceKernel() {
	if (!owner)
		return;
	VkDevice dev = (VkDevice)owner;
	if (pipeline) vkDestroyPipeline(dev, (VkPipeline)pipeline, nullptr);
	if (pipelineLayout) vkDestroyPipelineLayout(dev, (VkPipelineLayout)pipelineLayout, nullptr);
	if (setLayout) vkDestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout)setLayout, nullptr);
	if (descPool) vkDestroyDescriptorPool(dev, (VkDescriptorPool)descPool, nullptr);
	if (podBuffer) vkDestroyBuffer(dev, (VkBuffer)podBuffer, nullptr);
	if (podBufferMem) vkFreeMemory(dev, (VkDeviceMemory)podBufferMem, nullptr);
}

void VulkanDevice::Start() {
	VkPhysicalDevice phys = (VkPhysicalDevice)deviceDesc.GetVulkanPhysicalDevice();

	uint32_t nQ = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nQ, nullptr);
	vector<VkQueueFamilyProperties> qProps(nQ);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nQ, qProps.data());
	for (uint32_t i = 0; i < nQ; i++)
		if (qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queueFamily = i; break; }

	float prio = 1.f;
	VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = queueFamily;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;

	vector<const char *> exts;
	exts.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);

	// RT chain — only when the build/device supports it AND the user or
	// default enables it. Env LUXRAYS_VULKAN_RT=0 forces SW traversal.
	const bool wantRT = (!getenv("LUXRAYS_VULKAN_RT") ||
			strcmp(getenv("LUXRAYS_VULKAN_RT"), "0")) &&
			deviceDesc.hasAccelStruct && deviceDesc.hasRayQuery;
	if (wantRT) {
		exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
		exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
		exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
		exts.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
		exts.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
		exts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
		hasRayTracing = true;
	}
	// MoltenVK portability subset (beta-gated, required when advertised)
	uint32_t nExt = 0;
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &nExt, nullptr);
	{
		vector<VkExtensionProperties> avail(nExt);
		vkEnumerateDeviceExtensionProperties(phys, nullptr, &nExt, avail.data());
		for (const auto &e : avail)
			if (!strcmp(e.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
				exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
	}

	VkPhysicalDeviceBufferDeviceAddressFeatures bdaF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
	bdaF.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceAccelerationStructureFeaturesKHR asF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
	asF.accelerationStructure = VK_TRUE;
	VkPhysicalDeviceRayQueryFeaturesKHR rqF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
	rqF.rayQuery = VK_TRUE;

	void *featTail = &bdaF;
	if (wantRT) {
		asF.pNext = featTail; featTail = &asF;
		rqF.pNext = featTail; featTail = &rqF;
	}

	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = (uint32_t)exts.size();
	dci.ppEnabledExtensionNames = exts.data();
	dci.pNext = featTail;
	VK_CHECK(vkCreateDevice(phys, &dci, nullptr, (VkDevice *)&device));
	volkLoadDevice((VkDevice)device);

	vkGetDeviceQueue((VkDevice)device, queueFamily, 0, (VkQueue *)&queue);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = queueFamily;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK(vkCreateCommandPool((VkDevice)device, &pci, nullptr, (VkCommandPool *)&cmdPool));
}

void VulkanDevice::Interrupt() { }
void VulkanDevice::Stop() {
	FinishQueue();
}

//------------------------------------------------------------------------------
// Buffers
//------------------------------------------------------------------------------

static uint32_t FindMemType(VkPhysicalDevice phys, uint32_t bits,
		VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	uint32_t fallback = ~0u;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if (!(bits & (1u << i)))
			continue;
		const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if ((f & want) != want)
			continue;
		if ((f & prefer) == prefer)
			return i;
		if (fallback == ~0u)
			fallback = i;
	}
	return fallback;
}

void VulkanDevice::AllocBuffer(HardwareDeviceBuffer **buff, const BufferType type,
		void *src, const size_t size, const string &desc) {
	if (!*buff)
		*buff = new VulkanDeviceBuffer();
	VulkanDeviceBuffer *b = static_cast<VulkanDeviceBuffer *>(*buff);

	VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;

	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = std::max<VkDeviceSize>(size, 4); // zero-size buffers are invalid
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer((VkDevice)device, &bi, nullptr, (VkBuffer *)&b->buff));

	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements((VkDevice)device, (VkBuffer)b->buff, &mr);

	// MVP: host-visible coherent everywhere (unified memory / ReBAR).
	// DEVICE_LOCAL|HOST_VISIBLE first, plain HOST_VISIBLE as fallback —
	// covers Apple/integrated and ReBAR-enabled discrete GPUs.
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t mt = FindMemType((VkPhysicalDevice)deviceDesc.GetVulkanPhysicalDevice(),
			mr.memoryTypeBits, want, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mt == ~0u)
		throw runtime_error("Vulkan: no host-visible memory type for buffer " + desc);

	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mt;
	VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
	fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	ai.pNext = &fi;
	VK_CHECK(vkAllocateMemory((VkDevice)device, &ai, nullptr, (VkDeviceMemory *)&b->mem));
	VK_CHECK(vkBindBufferMemory((VkDevice)device, (VkBuffer)b->buff, (VkDeviceMemory)b->mem, 0));

	VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
	dai.buffer = (VkBuffer)b->buff;
	b->deviceAddr = vkGetBufferDeviceAddress((VkDevice)device, &dai);
	b->size = size;

	if (src)
		EnqueueWriteBuffer(*buff, true, size, src);

	AllocMemory(size);
	if (desc != "")
		LR_LOG(deviceContext, "[Device " << GetName() << "] " << desc <<
				" buffer size: " << ToMemString(size));
}

void VulkanDevice::FreeBuffer(HardwareDeviceBuffer **buff) {
	if (*buff && !(*buff)->IsNull()) {
		VulkanDeviceBuffer *b = static_cast<VulkanDeviceBuffer *>(*buff);
		FinishQueue(); // in-flight commands may reference the buffer
		vkDestroyBuffer((VkDevice)device, (VkBuffer)b->buff, nullptr);
		vkFreeMemory((VkDevice)device, (VkDeviceMemory)b->mem, nullptr);
		FreeMemory(b->size);
	}
	delete *buff;
	*buff = nullptr;
}

void VulkanDevice::EnqueueReadBuffer(const HardwareDeviceBuffer *buff,
		const bool blocking, const size_t size, void *ptr) {
	// Host-visible buffers: memcpy after queue sync (async staging path TBD)
	FinishQueue();
	const VulkanDeviceBuffer *b = static_cast<const VulkanDeviceBuffer *>(buff);
	void *m;
	VK_CHECK(vkMapMemory((VkDevice)device, (VkDeviceMemory)b->mem, 0, size, 0, &m));
	memcpy(ptr, m, size);
	vkUnmapMemory((VkDevice)device, (VkDeviceMemory)b->mem);
	(void)blocking;
}

void VulkanDevice::EnqueueWriteBuffer(const HardwareDeviceBuffer *buff,
		const bool blocking, const size_t size, const void *ptr) {
	FinishQueue(); // don't overwrite data still in use by in-flight work
	const VulkanDeviceBuffer *b = static_cast<const VulkanDeviceBuffer *>(buff);
	void *m;
	VK_CHECK(vkMapMemory((VkDevice)device, (VkDeviceMemory)b->mem, 0, size, 0, &m));
	memcpy(m, ptr, size);
	vkUnmapMemory((VkDevice)device, (VkDeviceMemory)b->mem);
	(void)blocking;
}

//------------------------------------------------------------------------------
// Command batching
//------------------------------------------------------------------------------

VkCommandBufferHandle VulkanDevice::BeginCmd() {
	EnsureCmdOpen();
	return openCmd;
}

void VulkanDevice::EnsureCmdOpen() {
	if (openCmd)
		return;
	VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = (VkCommandPool)cmdPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VK_CHECK(vkAllocateCommandBuffers((VkDevice)device, &ai, &cb));
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	openCmd = (VkCommandBufferHandle)cb;
}

void VulkanDevice::FlushQueue() {
	if (!openCmd)
		return;
	VkCommandBuffer cb = (VkCommandBuffer)openCmd;
	openCmd = nullptr;
	VK_CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VK_CHECK(vkQueueSubmit((VkQueue)queue, 1, &si, VK_NULL_HANDLE));
	// The command buffer is freed on the next FinishQueue via pool reset;
	// freeing while in flight is invalid, so just leak-track via pool reset.
}

void VulkanDevice::FinishQueue() {
	FlushQueue();
	VK_CHECK(vkQueueWaitIdle((VkQueue)queue));
	vkResetCommandPool((VkDevice)device, (VkCommandPool)cmdPool, 0);
	// Recycle in-flight descriptor sets is handled per-kernel on next use.
}

//------------------------------------------------------------------------------
// Program compilation (OpenCL C -> SPIR-V via clspv)
//------------------------------------------------------------------------------

HardwareDeviceProgramUPtr VulkanDevice::CompileProgram(
		const vector<string> &programParameters,
		const string &programSource,
		const string &programName) {

	// Same device-level contract as OpenCLDevice::CompileProgram:
	// LUXRAYS_OPENCL_DEVICE selects the OpenCL branch in the shared kernel
	// sources (atomics, ImageMap layout, ...); ocldevice_funcs provides
	// MAKE_FLOATn/VLOADn helpers. clspv translates the identical OpenCL C
	// the OCL backend compiles - zero kernel changes.
	vector<string> vkParams = programParameters;
	vkParams.push_back("-D LUXRAYS_OPENCL_DEVICE");
#if defined(__APPLE__)
	vkParams.push_back("-D LUXRAYS_OS_APPLE");
#elif defined(WIN32)
	vkParams.push_back("-D LUXRAYS_OS_WINDOWS");
#elif defined(__linux__)
	vkParams.push_back("-D LUXRAYS_OS_LINUX");
#endif
	vkParams.insert(vkParams.end(),
			additionalCompileOpts.begin(), additionalCompileOpts.end());

	const string vkSource =
		luxrays::ocl::KernelSource_ocldevice_funcs +
		programSource;

	// Kernel binary cache: same scheme as the persistent OCL cache.
	const string hash = oclKernelPersistentCache::HashString(
			oclKernelCache::ToOptsString(vkParams)) + "-" +
			oclKernelPersistentCache::HashString(vkSource) + "-vk1";

	const string cacheDir = string(getenv("HOME") ? getenv("HOME") : "/tmp") +
			"/.luxcore/vkcache";
	mkdir((string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.luxcore").c_str(), 0755);
	mkdir(cacheDir.c_str(), 0755);
	const string spvPath = cacheDir + "/" + hash + ".spv";
	const string mapPath = cacheDir + "/" + hash + ".map";

	struct stat st, mst;
	// Compile through clspv (subprocess — same in-spirit model as the
	// runtime NVRTC/MSL compiles on the other backends). Both the SPIR-V
	// and the reflection map must be cached; regenerate whichever is
	// missing so a stale/incomplete cache never silently runs without
	// argument layout.
	if (stat(spvPath.c_str(), &st) != 0 || stat(mapPath.c_str(), &mst) != 0) {
		const string srcPath = cacheDir + "/" + hash + ".cl";
		{
			ofstream srcFile(srcPath);
			srcFile << vkSource;
		}
		ostringstream cmd;
		cmd << "\"" << GetClspvPath() << "\""
			// Physical storage buffers: __global pointer args become u64
			// device addresses (legal SPIR-V function params), so clspv
			// keeps ordinary calls instead of inlining the whole call
			// graph into every kernel — required for corpus-scale sources.
			<< " --arch=spirv64 -physical-storage-buffers"
			// LuxCore structs use OpenCL/C packing (member directly after a
			// 12-byte float3 tail); legal only under VK_EXT_scalar_block_layout
			// (enabled in Start()). Our clspv build unlocks the flag.
			<< " -scalar-block-layout"
			// POD args -> one clustered storage buffer after the buffer args
			<< " -cluster-pod-kernel-args"
			// module-scope __constant tables (spectral LUTs, noise perm) ->
			// a single storage buffer, init data in the descriptor map
			<< " -module-constants-in-storage-buffer"
			// float8/float16 used by some kernel paths
			<< " -long-vector"
			// Physical pointer bitcasts are legal under buffer device
			// addressing: the producer reconciles pointee types with
			// OpBitcast instead of rewriting accesses (the rewriting path
			// cannot decompose multi-member structs like Transform and
			// materializes >64-bit ints that are not valid SPIR-V).
			<< " -replace-physical-pointer-bitcasts=false"
			// Pointer compares and inttoptr null materialization need 1.4+;
			// ray tracing entry points want 1.6.
			<< " --spv-version=1.6";
		for (const string &p : vkParams)
			cmd << " " << p;
		cmd << " -o \"" << spvPath << "\" \"" << srcPath << "\"";

		LR_LOG(deviceContext, "[" << programName << "] clspv: " << cmd.str());
		const int rc = system(cmd.str().c_str());
		if (rc != 0 || stat(spvPath.c_str(), &st) != 0) {
			LR_LOG(deviceContext, "[" << programName << "] clspv failed rc=" << rc);
			throw runtime_error(programName + " clspv SPIR-V compile failed");
		}

		// Reflection: kernel arg -> descriptor map (clspv-reflection tool)
		ostringstream rcmd;
		rcmd << "\"" << GetClspvReflectionPath() << "\" \"" << spvPath
			<< "\" -o \"" << mapPath << "\"";
		if (system(rcmd.str().c_str()) != 0) {
			LR_LOG(deviceContext, "[" << programName << "] clspv-reflection failed");
			throw runtime_error(programName + " clspv-reflection failed");
		}
	}

	vector<char> spv;
	{
		ifstream f(spvPath, ios::binary);
		spv.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
	}

	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = spv.size();
	smi.pCode = (const uint32_t *)spv.data();
	VkShaderModule mod;
	VK_CHECK(vkCreateShaderModule((VkDevice)device, &smi, nullptr, &mod));

	auto prog = make_unique<VulkanDeviceProgram>();
	prog->shaderModule = (VkShaderModuleHandle)mod;
	prog->owner = device;

	// Parse the clspv-reflection descriptor map. Format (CSV, key/value pairs):
	//   kernel_decl,<name>
	//   kernel,<name>,arg,<argName>,argOrdinal,<n>,descriptorSet,<s>,
	//       binding,<b>,offset,<o>,argKind,<kind>[,argSize,<z>]
	//   spec_constant,workgroup_size_x,spec_id,0 (y=1, z=2)
	//   constant,descriptorSet,<s>,binding,<b>,kind,constant[,data,<hex>]
	// argKind: buffer, pod, pod_ubo, local, sampler, ro_image, wo_image, ...
	ifstream mapFile(mapPath);
	string line;
	while (getline(mapFile, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		vector<string> f;
		string tok;
		istringstream ss(line);
		while (getline(ss, tok, ','))
			f.push_back(tok);
		if (f.size() < 2)
			continue;

		if (f[0] == "kernel" && f.size() >= 4) {
			// kernel,<name>,arg,<argName>,<key>,<val>,...
			const string &kname = f[1];
			auto &kl = prog->layouts[kname];
			uint32_t ord = ~0u, binding = 0, offset = 0, argSize = 0;
			string kind;
			for (size_t i = 4; i + 1 < f.size(); i += 2) {
				const string &k = f[i], &v = f[i + 1];
				if (k == "argOrdinal") ord = strtoul(v.c_str(), nullptr, 10);
				else if (k == "binding") binding = strtoul(v.c_str(), nullptr, 10);
				else if (k == "offset") offset = strtoul(v.c_str(), nullptr, 0);
				else if (k == "argSize") argSize = strtoul(v.c_str(), nullptr, 0);
				else if (k == "argKind") kind = v;
			}
			if (ord == ~0u)
				continue;
			if (kl.args.size() <= ord)
				kl.args.resize(ord + 1);
			VulkanDeviceProgram::ArgInfo ai;
			ai.binding = binding;
			if (kind == "buffer" || kind == "constant_ubo")
				ai.kind = VulkanDeviceProgram::ArgInfo::BUFFER;
			else if (kind == "pod" || kind == "pod_ubo" || kind == "pointer_ubo") {
				// -cluster-pod-kernel-args: every POD shares the trailing
				// binding; `offset` is the byte offset inside. Under
				// -physical-storage-buffers buffer args become u64
				// device addresses ("pointer_ubo") in the same block.
				ai.kind = VulkanDeviceProgram::ArgInfo::POD_UBO;
				ai.podOffset = offset;
				ai.podSize = argSize;
				if (kind != "pod")
					kl.podIsUniform = true;
			} else if (kind == "pod_pushconstant" || kind == "pointer_pushconstant") {
				// -cluster-pod-kernel-args lowers scalar args to push
				// constants; `offset`/`argSize` are the byte range inside
				// the push-constant block.
				ai.kind = VulkanDeviceProgram::ArgInfo::POD_PUSHCONST;
				ai.podOffset = offset;
				ai.podSize = argSize;
			} else if (kind == "local")
				ai.kind = VulkanDeviceProgram::ArgInfo::LOCAL;
			else
				ai.kind = VulkanDeviceProgram::ArgInfo::NONE; // sampler/image: NYI
			kl.args[ord] = ai;
			if (ai.kind == VulkanDeviceProgram::ArgInfo::POD_UBO) {
				kl.podUBOBinding = binding;
				kl.podUBOSize = std::max(kl.podUBOSize, offset + argSize);
			}
			if (ai.kind == VulkanDeviceProgram::ArgInfo::POD_PUSHCONST)
				kl.pushConstSize = std::max(kl.pushConstSize,
						offset + (argSize ? argSize : 4u));
		} else if (f[0] == "constant" && f.size() >= 6) {
			// module-scope __constant buffer: descriptorSet/binding + init hex
			uint32_t binding = 0;
			string data;
			for (size_t i = 1; i + 1 < f.size(); i += 2) {
				if (f[i] == "binding") binding = strtoul(f[i + 1].c_str(), nullptr, 10);
				else if (f[i] == "data" || f[i] == "hexbytes") data = f[i + 1];
			}
			prog->moduleConstantsBinding = binding;
			prog->moduleConstantsHex = data;
			prog->hasModuleConstants = true;
		}
	}

	// Upload the module-scope __constant blob once per program
	if (prog->hasModuleConstants && !prog->moduleConstantsHex.empty()) {
		const string &hex = prog->moduleConstantsHex;
		const size_t n = hex.size() / 2;
		vector<char> blob(n);
		for (size_t i = 0; i < n; i++)
			blob[i] = (char)strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16);
		HardwareDeviceBuffer *buf = nullptr;
		AllocBuffer(&buf, BUFFER_TYPE_READ_ONLY, blob.data(), n,
				programName + " module constants");
		VulkanDeviceBuffer *vb = static_cast<VulkanDeviceBuffer *>(buf);
		prog->moduleConstBuff = vb->buff;
		prog->moduleConstMem = vb->mem;
		prog->moduleConstSize = n;
		// Note: the VulkanDeviceBuffer object itself is owned by the engine
		// allocator path; keep handles for binding. Buffer lifetime = program
		// lifetime (both end at device Stop()).
	}

	return static_cast<HardwareDeviceProgramUPtr>(std::move(prog));
}

HardwareDeviceKernelUPtr VulkanDevice::GetKernel(
		HardwareDeviceProgramRef programRef,
		const string &kernelName) {
	const VulkanDeviceProgram &prog =
		dynamic_cast<const VulkanDeviceProgram &>(programRef);

	auto it = prog.layouts.find(kernelName);
	VulkanDeviceProgram::KernelLayout layout;
	if (it != prog.layouts.end())
		layout = it->second;

	// Descriptor set layout: one storage-buffer binding per BUFFER arg +
	// the clustered-POD SSBO + the module-constants SSBO when present.
	uint32_t maxBind = 0;
	for (const auto &a : layout.args)
		if (a.kind == VulkanDeviceProgram::ArgInfo::BUFFER)
			maxBind = std::max(maxBind, a.binding);
	if (layout.podUBOBinding != ~0u)
		maxBind = std::max(maxBind, layout.podUBOBinding);
	if (prog.hasModuleConstants)
		maxBind = std::max(maxBind, prog.moduleConstantsBinding);

	vector<VkDescriptorSetLayoutBinding> binds(maxBind + 1);
	vector<VkDescriptorSetLayoutBinding> used;
	for (const auto &a : layout.args) {
		if (a.kind != VulkanDeviceProgram::ArgInfo::BUFFER)
			continue;
		VkDescriptorSetLayoutBinding b{};
		b.binding = a.binding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		binds[a.binding] = b;
	}
	if (layout.podUBOBinding != ~0u) {
		VkDescriptorSetLayoutBinding b{};
		b.binding = layout.podUBOBinding;
		// The clustered block is Uniform-class when any arg came through
		// clspv's pod_ubo/pointer_ubo path (PSB pointer args always land
		// there); plain "pod" args use a storage buffer.
		b.descriptorType = layout.podIsUniform ?
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		binds[layout.podUBOBinding] = b;
	}
	if (prog.hasModuleConstants) {
		VkDescriptorSetLayoutBinding b{};
		b.binding = prog.moduleConstantsBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		binds[prog.moduleConstantsBinding] = b;
	}
	for (const auto &b : binds)
		if (b.stageFlags) used.push_back(b);

	VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = (uint32_t)used.size();
	dli.pBindings = used.data();

	auto kern = make_unique<VulkanDeviceKernel>();
	kern->owner = device;
	VK_CHECK(vkCreateDescriptorSetLayout((VkDevice)device, &dli, nullptr,
			(VkDescriptorSetLayout *)&kern->setLayout));

	VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, layout.pushConstSize};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	VkDescriptorSetLayout dl = (VkDescriptorSetLayout)kern->setLayout;
	pli.pSetLayouts = &dl;
	if (layout.pushConstSize) {
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &pcr;
	}
	VK_CHECK(vkCreatePipelineLayout((VkDevice)device, &pli, nullptr,
			(VkPipelineLayout *)&kern->pipelineLayout));

	// Bake the workgroup size: clspv exposes workgroup_size_{x,y,z} as
	// LocalSizeId spec constants (spec ids 0,1,2). Dispatch uses
	// localSizeX for the group count so coverage is always exact.
	kern->localSizeX = std::min<u_int>(deviceDesc.maxComputeWorkGroupInvocations, 256);
	uint32_t wgVals[3] = { kern->localSizeX, 1, 1 };
	VkSpecializationMapEntry specMap[3];
	for (int i = 0; i < 3; i++) {
		specMap[i].constantID = i;
		specMap[i].offset = i * sizeof(uint32_t);
		specMap[i].size = sizeof(uint32_t);
	}
	VkSpecializationInfo specInfo{};
	specInfo.mapEntryCount = 3;
	specInfo.pMapEntries = specMap;
	specInfo.dataSize = sizeof(wgVals);
	specInfo.pData = wgVals;

	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = (VkShaderModule)prog.shaderModule;
	cpi.stage.pName = kernelName.c_str();
	cpi.stage.pSpecializationInfo = &specInfo;
	cpi.layout = (VkPipelineLayout)kern->pipelineLayout;
	VK_CHECK(vkCreateComputePipelines((VkDevice)device, VK_NULL_HANDLE, 1, &cpi,
			nullptr, (VkPipeline *)&kern->pipeline));

	if (prog.hasModuleConstants) {
		kern->moduleConstBuff = prog.moduleConstBuff;
		kern->moduleConstBinding = prog.moduleConstantsBinding;
	}

	// Clustered POD backing buffer (storage buffer, raw-byte layout)
	if (layout.podUBOSize) {
		kern->podBufferSize = layout.podUBOSize;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = layout.podUBOSize;
		bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VK_CHECK(vkCreateBuffer((VkDevice)device, &bi, nullptr, (VkBuffer *)&kern->podBuffer));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements((VkDevice)device, (VkBuffer)kern->podBuffer, &mr);
		uint32_t mt = FindMemType((VkPhysicalDevice)deviceDesc.GetVulkanPhysicalDevice(),
				mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = mt;
		VK_CHECK(vkAllocateMemory((VkDevice)device, &ai, nullptr,
				(VkDeviceMemory *)&kern->podBufferMem));
		vkBindBufferMemory((VkDevice)device, (VkBuffer)kern->podBuffer,
				(VkDeviceMemory)kern->podBufferMem, 0);
	}

	// Per-kernel descriptor pool (recycled after FinishQueue)
	VkDescriptorPoolSize psz[2]{};
	psz[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	psz[0].descriptorCount = 1024;
	psz[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	psz[1].descriptorCount = 256;
	VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 256;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = psz;
	dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VK_CHECK(vkCreateDescriptorPool((VkDevice)device, &dpi, nullptr,
			(VkDescriptorPool *)&kern->descPool));

	kern->layout = layout;
	kern->args.resize(layout.args.size());
	kern->buffs.resize(layout.args.size(), nullptr);
	kern->roles.resize(layout.args.size(), VulkanDeviceKernel::ArgRole::UNSET);

	return static_cast<HardwareDeviceKernelUPtr>(std::move(kern));
}

u_int VulkanDevice::GetKernelWorkGroupSize(HardwareDeviceKernelRPtr kernel) {
	const VulkanDeviceKernel &k =
		dynamic_cast<const VulkanDeviceKernel &>(*kernel);
	return k.localSizeX;
}

//------------------------------------------------------------------------------
// Kernel args + dispatch
//------------------------------------------------------------------------------

void VulkanDevice::SetKernelArg(HardwareDeviceKernelRPtr kernel,
		const u_int index, const size_t size, const void *arg) {
	assert(kernel);
	auto &k = dynamic_cast<VulkanDeviceKernelRef>(*kernel);
	if (index >= k.roles.size()) {
		k.roles.resize(index + 1, VulkanDeviceKernel::ArgRole::UNSET);
		k.args.resize(index + 1);
		k.buffs.resize(index + 1, nullptr);
	}
	// The engine passes literal nullptr for disabled buffer channels via
	// the scalar template (size == sizeof(void*)): it is a NULL pointer,
	// not a scalar (same convention as MetalDevice/CUDADevice).
	if (!arg && size == sizeof(void *)) {
		k.roles[index] = VulkanDeviceKernel::ArgRole::POINTER;
		k.buffs[index] = nullptr;
		k.args[index].clear();
		return;
	}
	k.roles[index] = VulkanDeviceKernel::ArgRole::SCALAR;
	k.buffs[index] = nullptr;
	k.args[index].assign((const char *)arg, (const char *)arg + size);
}

void VulkanDevice::SetKernelArgBuffer(HardwareDeviceKernelRPtr kernel,
		const u_int index, const HardwareDeviceBuffer *buff) {
	assert(kernel);
	auto &k = dynamic_cast<VulkanDeviceKernelRef>(*kernel);
	if (index >= k.roles.size()) {
		k.roles.resize(index + 1, VulkanDeviceKernel::ArgRole::UNSET);
		k.args.resize(index + 1);
		k.buffs.resize(index + 1, nullptr);
	}
	k.roles[index] = VulkanDeviceKernel::ArgRole::POINTER;
	k.buffs[index] = buff;
	k.args[index].clear();
}

void VulkanDevice::EnqueueKernel(HardwareDeviceKernelRPtr kernel,
		const HardwareDeviceRange &globalSize,
		const HardwareDeviceRange &workGroupSize) {
	assert(kernel);
	auto &kk = dynamic_cast<VulkanDeviceKernelRef>(*kernel);
	VulkanDeviceKernel *k = &kk;
	const auto &layout = k->layout;

	// Fill the POD UBO. Under -physical-storage-buffers pointer args are
	// u64 PODs too: POINTER-role args write the buffer's device address
	// (0 for null) instead of scalar bytes.
	if (k->podBuffer) {
		char *m;
		VK_CHECK(vkMapMemory((VkDevice)device, (VkDeviceMemory)k->podBufferMem,
				0, k->podBufferSize, 0, (void **)&m));
		memset(m, 0, k->podBufferSize);
		for (size_t i = 0; i < layout.args.size(); i++) {
			const auto &ai = layout.args[i];
			if (ai.kind != VulkanDeviceProgram::ArgInfo::POD_UBO ||
					i >= k->roles.size())
				continue;
			if (k->roles[i] == VulkanDeviceKernel::ArgRole::POINTER) {
				const VulkanDeviceBuffer *vb =
						static_cast<const VulkanDeviceBuffer *>(k->buffs[i]);
				const uint64_t addr = vb ? vb->deviceAddr : 0;
				memcpy(m + ai.podOffset, &addr, 8);
			} else if (!k->args[i].empty())
				memcpy(m + ai.podOffset, k->args[i].data(),
						std::min<size_t>(k->args[i].size(), ai.podSize ? ai.podSize : k->args[i].size()));
		}
		vkUnmapMemory((VkDevice)device, (VkDeviceMemory)k->podBufferMem);
	}

	// Descriptor set for this dispatch
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = (VkDescriptorPool)k->descPool;
	VkDescriptorSetLayout dl = (VkDescriptorSetLayout)k->setLayout;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &dl;
	VkDescriptorSet dset;
	VkResult dres = vkAllocateDescriptorSets((VkDevice)device, &dai, &dset);
	if (dres != VK_SUCCESS) {
		// Pool exhausted: sync + reset + retry once
		FinishQueue();
		vkResetDescriptorPool((VkDevice)device, (VkDescriptorPool)k->descPool, 0);
		VK_CHECK(vkAllocateDescriptorSets((VkDevice)device, &dai, &dset));
	}
	k->inflightSets.push_back((VkDescriptorSetHandle)dset);

	vector<VkWriteDescriptorSet> writes;
	vector<VkDescriptorBufferInfo> infos;
	infos.reserve(layout.args.size() + 2); // +pod UBO; never reallocate
	                                     // (writes store pointers into infos)
	for (size_t i = 0; i < layout.args.size(); i++) {
		const auto &ai = layout.args[i];
		if (ai.kind != VulkanDeviceProgram::ArgInfo::BUFFER)
			continue;
		const VulkanDeviceBuffer *vb = nullptr;
		if (i < k->buffs.size())
			vb = static_cast<const VulkanDeviceBuffer *>(k->buffs[i]);
		infos.push_back({vb ? (VkBuffer)vb->buff : VK_NULL_HANDLE, 0,
				vb ? vb->size : 4});
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = dset;
		w.dstBinding = ai.binding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w.pBufferInfo = &infos.back();
		writes.push_back(w);
	}
	if (k->podBuffer) {
		infos.push_back({(VkBuffer)k->podBuffer, 0, k->podBufferSize});
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = dset;
		w.dstBinding = layout.podUBOBinding;
		w.descriptorCount = 1;
		w.descriptorType = layout.podIsUniform ?
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w.pBufferInfo = &infos.back();
		writes.push_back(w);
	}
	if (k->moduleConstBuff) {
		infos.push_back({(VkBuffer)k->moduleConstBuff, 0, VK_WHOLE_SIZE});
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = dset;
		w.dstBinding = k->moduleConstBinding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w.pBufferInfo = &infos.back();
		writes.push_back(w);
	}
	if (!writes.empty())
		vkUpdateDescriptorSets((VkDevice)device, (uint32_t)writes.size(),
				writes.data(), 0, nullptr);

	EnsureCmdOpen();
	VkCommandBuffer cb = (VkCommandBuffer)openCmd;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)k->pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			(VkPipelineLayout)k->pipelineLayout, 0, 1, &dset, 0, nullptr);

	// Push constants for scalar kernel args (clspv pod_pushconstant);
	// pointer args contribute their device address under
	// -physical-storage-buffers.
	if (layout.pushConstSize) {
		vector<char> pc(layout.pushConstSize, 0);
		for (size_t i = 0; i < layout.args.size(); i++) {
			const auto &ai = layout.args[i];
			if (ai.kind != VulkanDeviceProgram::ArgInfo::POD_PUSHCONST ||
					i >= k->roles.size())
				continue;
			if (k->roles[i] == VulkanDeviceKernel::ArgRole::POINTER) {
				const VulkanDeviceBuffer *vb =
						static_cast<const VulkanDeviceBuffer *>(k->buffs[i]);
				const uint64_t addr = vb ? vb->deviceAddr : 0;
				memcpy(pc.data() + ai.podOffset, &addr, 8);
			} else if (!k->args[i].empty())
				memcpy(pc.data() + ai.podOffset, k->args[i].data(),
						std::min<size_t>(k->args[i].size(),
								ai.podSize ? ai.podSize : k->args[i].size()));
		}
		vkCmdPushConstants(cb, (VkPipelineLayout)k->pipelineLayout,
				VK_SHADER_STAGE_COMPUTE_BIT, 0, layout.pushConstSize, pc.data());
	}

	// Group count from the pipeline's baked local size (callers pass the
	// value returned by GetKernelWorkGroupSize which is the same number)
	const u_int lx = k->localSizeX ? k->localSizeX : workGroupSize.sizes[0];
	const u_int gx = RoundUp<u_int>(globalSize.sizes[0], lx) / lx;
	const u_int gy = (globalSize.dimensions > 1) ?
		RoundUp<u_int>(globalSize.sizes[1], workGroupSize.sizes[1]) / workGroupSize.sizes[1] : 1;
	const u_int gz = (globalSize.dimensions > 2) ?
		RoundUp<u_int>(globalSize.sizes[2], workGroupSize.sizes[2]) / workGroupSize.sizes[2] : 1;
	vkCmdDispatch(cb, gx, gy, gz);
}

}

#endif

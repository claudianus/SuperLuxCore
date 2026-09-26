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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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
	// Bundled toolchain (dev-tools/vulkan-tools-install.sh): makes
	// kernel compilation work for hosts launched without a developer
	// PATH (GUI Blender, .app bundles, service processes).
	const char *home = getenv("HOME");
	if (home && home[0]) {
		const string bundled = string(home) + "/.luxcore/vktools/bin/clspv";
		if (access(bundled.c_str(), X_OK) == 0)
			return bundled;
	}
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

// opt binary used for per-kernel internalize+globaldce pruning before the
// SPIR-V producer pass. Lives next to the clspv build's bundled LLVM.
static string GetOptPath() {
	const char *env = getenv("LUXRAYS_OPT");
	if (env && env[0])
		return env;
	string c = GetClspvPath();
	const size_t slash = c.find_last_of('/');
	if (slash != string::npos) {
		// <...>/bin/clspv -> <...>/third_party/llvm/bin/opt
		const string derived = c.substr(0, slash) +
				"/../third_party/llvm/bin/opt";
		if (access(derived.c_str(), X_OK) == 0)
			return derived;
	}
	return "opt"; // PATH lookup
}

// Module handle of the MoltenVK dylib we loaded (RTLD_LOCAL — its symbols
// are invisible to dlsym(RTLD_DEFAULT), so the handle must be kept for
// private API lookups).
static void *moltenVKModule = nullptr;

static vector<string> GetMoltenVKCandidates() {
	vector<string> candidates;
	const char *env = getenv("LUXRAYS_MOLTENVK");
	if (env && env[0])
		candidates.push_back(env);
#if defined(__APPLE__)
	const char *home = getenv("HOME");
	if (home && home[0])
		candidates.push_back(string(home) +
				"/.luxcore/vktools/lib/libMoltenVK.dylib");
	// Next to the module containing this code (wheel/site-packages)
	Dl_info info;
	if (dladdr((const void *)&GetMoltenVKCandidates, &info) && info.dli_fname) {
		const string dir = string(info.dli_fname).substr(0,
				string(info.dli_fname).find_last_of('/'));
		candidates.push_back(dir + "/libMoltenVK.dylib");
		candidates.push_back(dir + "/vulkan/libMoltenVK.dylib");
	}
#endif
	return candidates;
}

// When volkInitialize's leaf-name dlopen fails (no Vulkan SDK on PATH,
// no DYLD_* env), dlopen MoltenVK by absolute path and hand its
// vkGetInstanceProcAddr to volkInitializeCustom — dyld does not match
// leaf-name dlopens against images loaded by another path.
static bool LoadMoltenVKCustom() {
	for (const auto &p : GetMoltenVKCandidates()) {
		void *mod = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!mod)
			continue;
		PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)
				dlsym(mod, "vkGetInstanceProcAddr");
		if (gipa) {
			volkInitializeCustom(gipa);
			if (vkCreateInstance) {
				moltenVKModule = mod;
				return true;
			}
		}
	}
	return false;
}

#if defined(__APPLE__)
// MoltenVK private configuration. MVKConfiguration is append-only across
// releases (see mvk_private_api.h), so a prefix mirror plus the size
// round-trip in vkGet/SetMoltenVKConfigurationMVK is ABI-safe. MoltenVK
// hides VK_KHR_acceleration_structure/ray_query from device enumeration
// unless enableExperimentalRayTracing is set (and restricts the advertised
// extension list unless advertiseExtensions == ALL); both default to
// disabled and cannot be reached via environment variables at runtime
// because MoltenVK snapshots NSProcessInfo at process launch.
struct LuxMVKConfig {
	uint32_t debugMode;
	uint32_t shaderConversionFlipVertexY;
	uint32_t synchronousQueueSubmits;
	uint32_t prefillMetalCommandBuffers;
	uint32_t maxActiveMetalCommandBuffersPerQueue;
	uint32_t supportLargeQueryPools;
	uint32_t presentWithCommandBuffer;
	uint32_t swapchainMinMagFilterUseNearest;
	uint64_t metalCompileTimeout;
	uint32_t performanceTracking;
	uint32_t performanceLoggingFrameCount;
	uint32_t displayWatermark;
	uint32_t specializedQueueFamilies;
	uint32_t switchSystemGPU;
	uint32_t fullImageViewSwizzle;
	uint32_t defaultGPUCaptureScopeQueueFamilyIndex;
	uint32_t defaultGPUCaptureScopeQueueIndex;
	uint32_t fastMathEnabled;
	uint32_t logLevel;
	uint32_t traceVulkanCalls;
	uint32_t forceLowPowerGPU;
	uint32_t semaphoreUseMTLFence;
	uint32_t semaphoreSupportStyle;
	uint32_t autoGPUCaptureScope;
	const char *autoGPUCaptureOutputFilepath;
	uint32_t texture1DAs2D;
	uint32_t preallocateDescriptors;
	uint32_t useCommandPooling;
	uint32_t useMTLHeap;
	uint32_t activityPerformanceLoggingStyle;
	uint32_t apiVersionToAdvertise;
	uint32_t advertiseExtensions;           // 1 == MVK_CONFIG_ADVERTISE_EXTENSIONS_ALL
	uint32_t resumeLostDevice;
	uint32_t useMetalArgumentBuffers;
	uint32_t shaderSourceCompressionAlgorithm;
	uint32_t shouldMaximizeConcurrentCompilation;
	float timestampPeriodLowPassAlpha;
	uint32_t useMetalPrivateAPI;
	const char *shaderDumpDir;
	uint32_t shaderLogEstimatedGLSL;
	uint32_t liveCheckAllResources;
	uint32_t enableExperimentalRayTracing;  // gates accelerationStructures feature
	// Newer MoltenVK releases append members + padding here; the size
	// round-trip covers the tail.
};

typedef VkResult (VKAPI_PTR *PFN_vkGetMoltenVKConfigurationMVK)(
		void *ignored, void *pConfiguration, size_t *pConfigurationSize);
typedef VkResult (VKAPI_PTR *PFN_vkSetMoltenVKConfigurationMVK)(
		void *ignored, const void *pConfiguration, size_t *pConfigurationSize);

static void EnableMoltenVKRayTracing() {
	// RTLD_LOCAL hides the private API from RTLD_DEFAULT; resolve through
	// the module we loaded, else scan loaded images for a MoltenVK dylib
	// (covers the Vulkan-ICD path where the loader opened it).
	void *mod = moltenVKModule;
	if (!mod) {
		const uint32_t n = _dyld_image_count();
		for (uint32_t i = 0; i < n && !mod; ++i) {
			const char *nm = _dyld_get_image_name(i);
			if (nm && strstr(nm, "MoltenVK"))
				mod = dlopen(nm, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		}
	}
	auto getCfg = mod ? (PFN_vkGetMoltenVKConfigurationMVK)
			dlsym(mod, "vkGetMoltenVKConfigurationMVK") : nullptr;
	auto setCfg = mod ? (PFN_vkSetMoltenVKConfigurationMVK)
			dlsym(mod, "vkSetMoltenVKConfigurationMVK") : nullptr;
	if (!getCfg || !setCfg)
		return; // native Vulkan driver or older MoltenVK

	// Learn MoltenVK's expected struct size, then round-trip the config.
	size_t sz = 0;
	const VkResult qr = getCfg(nullptr, nullptr, &sz);
	if ((qr != VK_SUCCESS && qr != VK_INCOMPLETE) || sz < sizeof(LuxMVKConfig))
		return; // layout older than our mirror: leave config untouched
	vector<char> buf(sz, 0);
	size_t got = sz;
	getCfg(nullptr, buf.data(), &got);
	LuxMVKConfig *cfg = (LuxMVKConfig *)buf.data();
	cfg->advertiseExtensions = 1; // MVK_CONFIG_ADVERTISE_EXTENSIONS_ALL
	cfg->enableExperimentalRayTracing = VK_TRUE;
	size_t put = got;
	setCfg(nullptr, buf.data(), &put);
}
#endif

static void InitVulkanLibrary() {
	if (vulkanInitialized)
		return;
	vulkanInitialized = true;

	// volkInitialize dlopens the loader (or libMoltenVK directly).
	if (volkInitialize() == VK_SUCCESS || LoadMoltenVKCustom()) {
#if defined(__APPLE__)
		EnableMoltenVKRayTracing();
#endif
		vulkanAvailable = true;
	}
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
		hasRayQuery(false), hasAccelStruct(false), hasUnifiedMemory(false),
		hasScalarBlockLayout(false) {

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

	// scalarBlockLayout is required by every kernel we compile
	// (clspv -scalar-block-layout): it is a Vulkan 1.2 core feature and
	// exists as VK_EXT_scalar_block_layout for older drivers. MoltenVK
	// tolerates its absence; native drivers do not.
	if (props.apiVersion >= VK_API_VERSION_1_2) {
		VkPhysicalDeviceVulkan12Features v12{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		f2.pNext = &v12;
		vkGetPhysicalDeviceFeatures2((VkPhysicalDevice)physDev, &f2);
		hasScalarBlockLayout = (v12.scalarBlockLayout == VK_TRUE);
	} else {
		for (const auto &e : exts)
			if (!strcmp(e.extensionName, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME))
				hasScalarBlockLayout = true;
	}

	// Unified memory heuristic (Apple / integrated)
	{
		ostringstream u;
		for (int i = 0; i < VK_UUID_SIZE; i++)
			u << std::hex << std::setw(2) << std::setfill('0')
					<< (u_int)props.pipelineCacheUUID[i];
		pipelineCacheUUID = u.str();
	}

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
		cmdPool(nullptr), openCmd(nullptr), pipeCache(nullptr),
		hasRayTracing(false) {
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
	// Program is a pure cache-key container under split compilation;
	// shader modules are owned by their kernels.
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
	if (moduleConstBuff) vkDestroyBuffer(dev, (VkBuffer)moduleConstBuff, nullptr);
	if (moduleConstMem) vkFreeMemory(dev, (VkDeviceMemory)moduleConstMem, nullptr);
	if (shaderModule) vkDestroyShaderModule(dev, (VkShaderModule)shaderModule, nullptr);
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

	// Every shader we compile is emitted with scalar block layout
	// (clspv -scalar-block-layout): LuxCore structs use OpenCL/C packing
	// that std430 cannot express. Refuse early with a clear message on
	// drivers lacking the feature instead of misreading descriptor data.
	if (!deviceDesc.hasScalarBlockLayout)
		throw runtime_error("Vulkan device '" + deviceDesc.GetName() +
				"' lacks scalarBlockLayout support (required by LuxCore kernels)");

	VkPhysicalDeviceBufferDeviceAddressFeatures bdaF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
	bdaF.bufferDeviceAddress = VK_TRUE;

	// scalarBlockLayout: core feature on Vulkan 1.2+; the EXT struct and
	// extension name cover pre-1.2 drivers (rare on LuxCore-class GPUs).
	VkPhysicalDeviceProperties devProps;
	vkGetPhysicalDeviceProperties(phys, &devProps);
	VkPhysicalDeviceVulkan12Features v12F{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceScalarBlockLayoutFeatures extSblF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
	const bool api12 = devProps.apiVersion >= VK_API_VERSION_1_2;
	if (api12)
		v12F.scalarBlockLayout = VK_TRUE;
	else {
		extSblF.scalarBlockLayout = VK_TRUE;
		exts.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
	}

	VkPhysicalDeviceAccelerationStructureFeaturesKHR asF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
	asF.accelerationStructure = VK_TRUE;
	VkPhysicalDeviceRayQueryFeaturesKHR rqF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
	rqF.rayQuery = VK_TRUE;

	void *featTail = &bdaF;
	if (api12) {
		v12F.pNext = featTail; featTail = &v12F;
	} else {
		extSblF.pNext = featTail; featTail = &extSblF;
	}
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

	// Persistent pipeline cache: MoltenVK runs SPIR-V -> MSL -> Metal
	// compilation inside vkCreateComputePipelines, and its MSL codegen is
	// occasionally nondeterministic (an undeclared-identifier MSL error was
	// observed on a rerun of an identical kernel). Seeding a VkPipelineCache
	// from disk makes each kernel's pipeline creation a one-time event;
	// cache hits skip shader compilation entirely on warm runs.
	{
		const string dir = string(getenv("HOME") ? getenv("HOME") : "/tmp") +
				"/.luxcore/vkcache";
		mkdir((string(getenv("HOME") ? getenv("HOME") : "/tmp") +
				"/.luxcore").c_str(), 0755);
		mkdir(dir.c_str(), 0755);
		pipeCachePath = dir + "/vkpipe-" + deviceDesc.pipelineCacheUUID + ".bin";

		vector<char> seed;
		{
			ifstream f(pipeCachePath, ios::binary);
			seed.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
		}
		VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		ci.initialDataSize = seed.size();
		ci.pInitialData = seed.empty() ? nullptr : seed.data();
		// Invalid/stale blobs are rejected by the driver per spec.
		VkResult rc = vkCreatePipelineCache((VkDevice)device, &ci, nullptr,
				(VkPipelineCache *)&pipeCache);
		if (rc != VK_SUCCESS) {
			pipeCache = nullptr;
			LR_LOG(deviceContext, "[Device " << GetName() <<
					"] vkCreatePipelineCache failed rc=" << rc << " (cold pipelines)");
		}
	}
}

void VulkanDevice::Interrupt() { }
void VulkanDevice::Stop() {
	FinishQueue();

	if (pipeCache) {
		// Persist the pipeline cache for the next run (see Start()).
		size_t n = 0;
		if (vkGetPipelineCacheData((VkDevice)device, (VkPipelineCache)pipeCache,
				&n, nullptr) == VK_SUCCESS && n) {
			vector<char> blob(n);
			if (vkGetPipelineCacheData((VkDevice)device, (VkPipelineCache)pipeCache,
					&n, blob.data()) == VK_SUCCESS) {
				const string tmp = pipeCachePath + ".tmp";
				ofstream f(tmp, ios::binary);
				f.write(blob.data(), n);
				f.close();
				rename(tmp.c_str(), pipeCachePath.c_str());
			}
		}
		vkDestroyPipelineCache((VkDevice)device, (VkPipelineCache)pipeCache, nullptr);
		pipeCache = nullptr;
	}
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

// Enumerate __kernel entry points in a textual IR module by scanning for
// spir_kernel definitions (same rule clspv uses: external linkage +
// calling convention).
static vector<string> ListKernelNames(const string &llPath) {
	vector<string> names;
	ifstream f(llPath);
	string line;
	while (getline(f, line)) {
		// define ... spir_kernel ... @Name(
		if (line.find("spir_kernel") == string::npos ||
				line.find("define") != 0)
			continue;
		const size_t at = line.find('@');
		const size_t par = line.find('(', at);
		if (at != string::npos && par != string::npos)
			names.push_back(line.substr(at + 1, par - at - 1));
	}
	return names;
}

// Disassemble bcPath once, drop the appending @llvm.global.annotations
// global (it keeps every spir_kernel alive as a GlobalDCE root, so each
// "per-kernel" module stays the full ~44MB program and clspv re-runs the
// expensive inlining over all kernels per invocation). The annotations are
// just clang's record of always_inline/etc. attributes which are already
// real IR attributes on the functions. Returns the path of the stripped
// .ll, which is also reused for kernel enumeration.
static string StripAnnotationsToLL(const string &bcPath) {
	const string opt = GetOptPath();
	const string slashDir = opt.substr(0, opt.find_last_of('/') + 1);
	const string llPath = bcPath + ".noann.ll";
	struct stat st;
	if (stat(llPath.c_str(), &st) == 0)
		return llPath;
	const string rawLL = bcPath + ".raw.ll";
	{
		ostringstream cmd;
		cmd << "\"" << slashDir << "llvm-dis\" \"" << bcPath
			<< "\" -o \"" << rawLL << "\"";
		if (system(cmd.str().c_str()) != 0)
			throw runtime_error("llvm-dis failed on " + bcPath);
	}
	ifstream in(rawLL);
	ofstream out(llPath + ".tmp");
	string line;
	while (getline(in, line)) {
		if (line.compare(0, 24, "@llvm.global.annotations") == 0)
			continue;
		out << line << "\n";
	}
	in.close();
	out.close();
	rename((llPath + ".tmp").c_str(), llPath.c_str());
	remove(rawLL.c_str());
	return llPath;
}

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
			oclKernelPersistentCache::HashString(vkSource) + "-vk2";

	const string cacheDir = string(getenv("HOME") ? getenv("HOME") : "/tmp") +
			"/.luxcore/vkcache";
	mkdir((string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.luxcore").c_str(), 0755);
	mkdir(cacheDir.c_str(), 0755);
	const string bcPath = cacheDir + "/" + hash + ".bc";
	const string srcPath = cacheDir + "/" + hash + ".cl";

	struct stat st;
	// Stage 1: frontend-only compile to LLVM bitcode (seconds; the clspv
	// middle-end is what is slow, and it runs per kernel below).
	if (stat(bcPath.c_str(), &st) != 0) {
		{
			ofstream srcFile(srcPath);
			srcFile << vkSource;
		}
		ostringstream cmd;
		// Pass the full producer flag set already at the bc stage: flags
		// like -physical-storage-buffers shape the frontend IR (PSB
		// pointers instead of __global addrspace(1)), and feeding
		// non-PSB bitcode to `clspv -x ir -physical-storage-buffers`
		// produces OpBitcast'd PSB pointers that SPIRV-Cross lowers to
		// reinterpret_cast forms Metal rejects.
		cmd << "\"" << GetClspvPath() << "\" --output-format=bc"
			<< " --arch=spirv64 -physical-storage-buffers"
			<< " -scalar-block-layout -cluster-pod-kernel-args"
			<< " -module-constants-in-storage-buffer -long-vector"
			<< " -replace-physical-pointer-bitcasts=false"
			// Bare decimal literals (0.5, 1e-5, 10000.) promote to fp64 in
			// OpenCL C; Metal has no fp64 at all, so keep literals fp32.
			<< " -cl-single-precision-constant"
			<< " --spv-version=1.6"
			<< " -o \"" << bcPath << "\" \"" << srcPath << "\"";
		for (const string &p : vkParams)
			cmd << " " << p;
		LR_LOG(deviceContext, "[" << programName << "] clspv frontend: " << cmd.str());
		const int rc = system(cmd.str().c_str());
		if (rc != 0 || stat(bcPath.c_str(), &st) != 0) {
			LR_LOG(deviceContext, "[" << programName << "] clspv frontend failed rc=" << rc);
			throw runtime_error(programName + " clspv bitcode compile failed");
		}
	}

	// Split compilation: enumerate kernels, prune each to its own module
	// (internalize+globaldce), then clspv -x ir -> .spv + clspv-reflection
	// -> .map. A monolithic compile serially inlines the full call graph
	// into every entry point (70+ min on PATHOCL); pruning one kernel per
	// module makes each compile seconds-to-minutes and parallelizes cleanly.
	const string prunedLL = StripAnnotationsToLL(bcPath);

	auto prog = make_unique<VulkanDeviceProgram>();
	prog->owner = device;
	prog->cacheDir = cacheDir;
	prog->cacheKey = hash;
	prog->kernelNames = ListKernelNames(prunedLL);
	if (prog->kernelNames.empty())
		throw runtime_error(programName + ": no __kernel entry points in bitcode");

	// Per-kernel cache keys hash the PRUNED module, not the whole program:
	// editing kernel A (or a helper it does not call) leaves the pruned
	// modules of the other kernels bit-identical, so a source edit only
	// recompiles the kernels whose call graph actually changed instead
	// of all of them.
	vector<string> todo;
	map<string, string> prunedBCs;
	for (const string &k : prog->kernelNames) {
		const string prunedBC = cacheDir + "/" + hash + "-" + k + ".pruned.bc";
		struct stat s3;
		if (stat(prunedBC.c_str(), &s3) != 0) {
			ostringstream cmd;
			// internalize keeps only this kernel external; on the
			// annotations-stripped input globaldce drops all other
			// kernels and their call graphs.
			cmd << "\"" << GetOptPath() << "\" \"" << prunedLL << "\""
				<< " -passes='internalize,globaldce'"
				<< " -internalize-public-api-list=" << k
				<< " -o \"" << prunedBC << "\"";
			if (system(cmd.str().c_str()) != 0 ||
					stat(prunedBC.c_str(), &s3) != 0)
				throw runtime_error(programName +
						": opt internalize/globaldce failed for " + k);
		}
		prunedBCs[k] = prunedBC;

		ifstream in(prunedBC, ios::binary);
		const string khash = oclKernelPersistentCache::HashString(
				string(istreambuf_iterator<char>(in),
						istreambuf_iterator<char>()));
		const string kbase = cacheDir + "/vkk2-" + k + "-" + khash;
		prog->kernelBasePaths[k] = kbase;
		if (stat((kbase + ".spv").c_str(), &st) != 0 ||
				stat((kbase + ".map").c_str(), &st) != 0)
			todo.push_back(k);
	}
	LR_LOG(deviceContext, "[" << programName << "] " << prog->kernelNames.size()
			<< " kernels, " << todo.size() << " to compile");

	if (!todo.empty()) {
		// Producer flags: identical to the monolithic path.
		const string flags =
			" --arch=spirv64 -physical-storage-buffers"
			// LuxCore structs use OpenCL/C packing (member directly after a
			// 12-byte float3 tail); legal only under VK_EXT_scalar_block_layout
			// (enabled in Start()). Our clspv build unlocks the flag.
			" -scalar-block-layout"
			// POD args -> one clustered storage buffer after the buffer args
			" -cluster-pod-kernel-args"
			// module-scope __constant tables (spectral LUTs, noise perm) ->
			// a per-kernel storage buffer, init data in the descriptor map
			" -module-constants-in-storage-buffer"
			// float8/float16 used by some kernel paths
			" -long-vector"
			// Physical pointer bitcasts are legal under buffer device
			// addressing: the producer reconciles pointee types with
			// OpBitcast instead of rewriting accesses (the rewriting path
			// cannot decompose multi-member structs like Transform and
			// materializes >64-bit ints that are not valid SPIR-V).
			" -replace-physical-pointer-bitcasts=false"
			// Bare double literals become fp32 (see bc stage note); the IR
			// producer ignores it but keep the flag set uniform.
			" -cl-single-precision-constant"
			// Pointer compares and inttoptr null materialization need 1.4+;
			// ray tracing entry points want 1.6.
			" --spv-version=1.6";

		std::atomic<u_int> next{0};
		std::atomic<u_int> failed{0};
		const u_int nWorkers = std::min<u_int>(4, todo.size());
		vector<std::thread> pool;
		for (u_int w = 0; w < nWorkers; w++) {
			pool.emplace_back([&, this]() {
				for (u_int i = next++; i < todo.size(); i = next++) {
					const string &k = todo[i];
					const string kbase = prog->kernelBasePaths[k];
					ostringstream cmd;
					// The pruned single-entry module was already produced
					// above; clspv only compiles this kernel's reachable
					// code, so each .spv has exactly one entry point.
					cmd << "\"" << GetClspvPath() << "\" -x ir" << flags
						<< " \"" << prunedBCs[k] << "\" -o \"" << kbase << ".spv\" && "
						// -d: the tool's built-in validator only knows up to
						// Vulkan 1.2; our SPIR-V 1.6 modules still parse fine.
						<< "\"" << GetClspvReflectionPath() << "\" -d \"" << kbase << ".spv\""
						<< " -o \"" << kbase << ".map\"";
					const int rc = system(cmd.str().c_str());
					struct stat s2;
					if (rc != 0 || stat((kbase + ".spv").c_str(), &s2) != 0 ||
							stat((kbase + ".map").c_str(), &s2) != 0) {
						LR_LOG(deviceContext, "[" << programName << "] kernel "
								<< k << " compile failed rc=" << rc);
						failed++;
					}
				}
			});
		}
		for (auto &t : pool)
			t.join();
		if (failed)
			throw runtime_error(programName + ": " + ToString(failed.load()) +
					" kernel(s) failed SPIR-V compile");
	}

	return static_cast<HardwareDeviceProgramUPtr>(std::move(prog));
}

// Parse one per-kernel clspv-reflection descriptor map. Format (CSV):
//   kernel,<name>,arg,<argName>,argOrdinal,<n>,descriptorSet,<s>,
//       binding,<b>,offset,<o>,argKind,<kind>[,argSize,<z>]
//   spec_constant,workgroup_size_x,spec_id,0 (y=1, z=2)
//   constant,descriptorSet,<s>,binding,<b>,kind,constant[,data,<hex>]
// argKind: buffer, pod, pod_ubo, local, sampler, ro_image, wo_image, ...
static void ParseKernelMap(const string &mapPath, const string &kernelName,
		VulkanDeviceProgram::KernelLayout &kl,
		uint32_t &constBinding, string &constHex) {
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

		if (f[0] == "kernel" && f.size() >= 4 && f[1] == kernelName) {
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
			for (size_t i = 1; i + 1 < f.size(); i += 2) {
				if (f[i] == "binding") constBinding = strtoul(f[i + 1].c_str(), nullptr, 10);
				else if (f[i] == "data" || f[i] == "hexbytes") constHex = f[i + 1];
			}
		}
	}
}

HardwareDeviceKernelUPtr VulkanDevice::GetKernel(
		HardwareDeviceProgramRef programRef,
		const string &kernelName) {
	const VulkanDeviceProgram &prog =
		dynamic_cast<const VulkanDeviceProgram &>(programRef);

	// Per-kernel SPIR-V module + descriptor map produced by CompileProgram.
	// kernelBasePaths carries the pruned-module-hash base when present.
	const auto kbp = prog.kernelBasePaths.find(kernelName);
	const string kbase = (kbp != prog.kernelBasePaths.end()) ? kbp->second
		: prog.cacheDir + "/" + prog.cacheKey + "-" + kernelName;
	vector<char> spv;
	{
		ifstream f(kbase + ".spv", ios::binary);
		spv.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
	}
	if (spv.empty())
		throw runtime_error("Vulkan kernel missing SPIR-V: " + kernelName);

	VulkanDeviceProgram::KernelLayout layout;
	uint32_t constBinding = ~0u;
	string constHex;
	ParseKernelMap(kbase + ".map", kernelName, layout, constBinding, constHex);
	const bool hasModuleConstants = !constHex.empty();

	// Descriptor set layout: one storage-buffer binding per BUFFER arg +
	// the clustered-POD SSBO + the module-constants SSBO when present.
	uint32_t maxBind = 0;
	for (const auto &a : layout.args)
		if (a.kind == VulkanDeviceProgram::ArgInfo::BUFFER)
			maxBind = std::max(maxBind, a.binding);
	if (layout.podUBOBinding != ~0u)
		maxBind = std::max(maxBind, layout.podUBOBinding);
	if (hasModuleConstants)
		maxBind = std::max(maxBind, constBinding);

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
	if (hasModuleConstants) {
		VkDescriptorSetLayoutBinding b{};
		b.binding = constBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		binds[constBinding] = b;
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

	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = spv.size();
	smi.pCode = (const uint32_t *)spv.data();
	VK_CHECK(vkCreateShaderModule((VkDevice)device, &smi, nullptr,
			(VkShaderModule *)&kern->shaderModule));

	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = (VkShaderModule)kern->shaderModule;
	cpi.stage.pName = kernelName.c_str();
	cpi.stage.pSpecializationInfo = &specInfo;
	cpi.layout = (VkPipelineLayout)kern->pipelineLayout;
	// MoltenVK's SPIR-V -> MSL codegen is occasionally nondeterministic
	// (observed: a rerun of an identical module emitted an undeclared
	// identifier). Retry a few times before giving up; the pipeline cache
	// above makes the successful result permanent across runs.
	VkResult prc = VK_ERROR_INITIALIZATION_FAILED;
	for (int attempt = 0; attempt < 4; attempt++) {
		prc = vkCreateComputePipelines((VkDevice)device,
				(VkPipelineCache)pipeCache, 1, &cpi, nullptr,
				(VkPipeline *)&kern->pipeline);
		if (prc == VK_SUCCESS)
			break;
	}
	VK_CHECK(prc);

	// Per-kernel module-scope __constant blob (each split module carries
	// only the constants its own call graph reached after globaldce).
	if (hasModuleConstants) {
		const size_t n = constHex.size() / 2;
		vector<char> blob(n);
		for (size_t i = 0; i < n; i++)
			blob[i] = (char)strtoul(constHex.substr(i * 2, 2).c_str(), nullptr, 16);
		// Direct VkBuffer creation (not AllocBuffer): the kernel owns the
		// handles and frees them in ~VulkanDeviceKernel, outside the
		// usedMemory accounting FreeBuffer balances.
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = n;
		bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VK_CHECK(vkCreateBuffer((VkDevice)device, &bi, nullptr,
				(VkBuffer *)&kern->moduleConstBuff));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements((VkDevice)device,
				(VkBuffer)kern->moduleConstBuff, &mr);
		const uint32_t mt = FindMemType(
				(VkPhysicalDevice)deviceDesc.GetVulkanPhysicalDevice(),
				mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = mt;
		VK_CHECK(vkAllocateMemory((VkDevice)device, &ai, nullptr,
				(VkDeviceMemory *)&kern->moduleConstMem));
		vkBindBufferMemory((VkDevice)device, (VkBuffer)kern->moduleConstBuff,
				(VkDeviceMemory)kern->moduleConstMem, 0);
		void *m;
		VK_CHECK(vkMapMemory((VkDevice)device,
				(VkDeviceMemory)kern->moduleConstMem, 0, n, 0, &m));
		memcpy(m, blob.data(), n);
		vkUnmapMemory((VkDevice)device, (VkDeviceMemory)kern->moduleConstMem);
		kern->moduleConstBinding = constBinding;
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

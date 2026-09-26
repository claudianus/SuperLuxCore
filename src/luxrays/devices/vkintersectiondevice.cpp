/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
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

#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "volk.h"

#include "luxrays/devices/vkintersectiondevice.h"
#include "luxrays/accelerators/bvhaccel.h"
#include "luxrays/core/accelerator.h"
#include "luxrays/core/context.h"
#include "luxrays/core/dataset.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/utils/oclcache.h"

#define VK_CHECK(r_) do { const VkResult r__ = (r_); if (r__ != VK_SUCCESS) \
	throw std::runtime_error("Vulkan error " + ToString(r__) + " at " __FILE__ ":" + ToString(__LINE__)); } while (0)

using namespace std;

namespace luxrays {

//------------------------------------------------------------------------------
// Hardware ray tracing: BLAS/TLAS + GL_EXT_ray_query compute kernel state.
// Defined before the class methods so unique_ptr<VulkanRTAccel> sees a
// complete type.
//------------------------------------------------------------------------------

struct VulkanIntersectionDevice::VulkanRTAccel {
	std::vector<HardwareDeviceBuffer *> ownedBuffs; // verts/tris/AS/instance bufs
	std::vector<VkAccelerationStructureKHR> blas;
	VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
	VkDescriptorPool descPool = VK_NULL_HANDLE;
	VkDescriptorSet descSet = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
};

//------------------------------------------------------------------------------
// Vulkan IntersectionDevice
//------------------------------------------------------------------------------

VulkanIntersectionDevice::VulkanIntersectionDevice(
	ContextConstRef context,
	VulkanDeviceDescriptionConstRef desc,
	const size_t devIndex
) :
	Device(context, devIndex), VulkanDevice(context, desc, devIndex),
	HardwareIntersectionDevice(), kernel(nullptr) {
}

VulkanIntersectionDevice::~VulkanIntersectionDevice() {
}

void VulkanIntersectionDevice::SetDataSet(DataSetSPtr newDataSet) {
	IntersectionDevice::SetDataSet(newDataSet);

	if (dataSet) {
		const AcceleratorType accelType = dataSet->GetAcceleratorType();
		if (accelType != ACCEL_AUTO) {
			accel = dataSet->GetAccelerator(accelType);
		} else {
			if (dataSet->RequiresInstanceSupport() || dataSet->RequiresMotionBlurSupport())
				accel = dataSet->GetAccelerator(ACCEL_MBVH);
			else
				accel = dataSet->GetAccelerator(ACCEL_BVH);
		}
	}
}

void VulkanIntersectionDevice::Update() {
	if (rtAccel) {
		// Dataset changed under a running device (scene edit): rebuild the
		// BLAS/TLAS set, same lifecycle as Stop+Start.
		FreeRTAccel();
		try {
			rtAccel.reset(BuildRTAccel());
		} catch (const std::exception &e) {
			LR_LOG(deviceContext, "[Device " << GetName() <<
					"] Vulkan HWRT rebuild failed, using SW traversal: " << e.what());
			kernel = accel->NewHardwareIntersectionKernel(*this);
		}
		return;
	}
	if (kernel)
		kernel->Update(dataSet);
}

void VulkanIntersectionDevice::FreeRTAccel() {
	if (!rtAccel)
		return;
	VkDevice dev = (VkDevice)device;
	vkDeviceWaitIdle(dev);
	if (rtAccel->pipeline) vkDestroyPipeline(dev, rtAccel->pipeline, nullptr);
	if (rtAccel->pipeLayout) vkDestroyPipelineLayout(dev, rtAccel->pipeLayout, nullptr);
	if (rtAccel->descPool) vkDestroyDescriptorPool(dev, rtAccel->descPool, nullptr);
	if (rtAccel->dsLayout) vkDestroyDescriptorSetLayout(dev, rtAccel->dsLayout, nullptr);
	if (rtAccel->tlas) vkDestroyAccelerationStructureKHR(dev, rtAccel->tlas, nullptr);
	for (auto &a : rtAccel->blas)
		vkDestroyAccelerationStructureKHR(dev, a, nullptr);
	for (auto &b : rtAccel->ownedBuffs)
		FreeBuffer(&b);
	rtAccel.reset();
}

void VulkanIntersectionDevice::Start() {
	VulkanDevice::Start();

	// Prefer hardware ray tracing (BLAS/TLAS + ray query) when the device
	// supports it; fall back to the SW traversal kernel on any failure.
	if (hasRayTracing) {
		try {
			rtAccel.reset(BuildRTAccel());
		} catch (const std::exception &e) {
			LR_LOG(deviceContext, "[Device " << GetName() <<
					"] Vulkan HWRT init failed, using SW traversal: " << e.what());
			rtAccel.reset();
		}
	}
	if (!rtAccel)
		kernel = accel->NewHardwareIntersectionKernel(*this);
	else
		LR_LOG(deviceContext, "[Device " << GetName() <<
				"] Vulkan HWRT: ray-query intersection active");
}

void VulkanIntersectionDevice::Stop() {
	FreeRTAccel();
	kernel.reset();

	VulkanDevice::Stop();
}

void VulkanIntersectionDevice::EnqueueTraceRayBuffer(HardwareDeviceBuffer *rayBuff,
		HardwareDeviceBuffer *rayHitBuff,
		const unsigned int rayCount) {
	if (rtAccel) {
		VkDevice dev = (VkDevice)device;
		VkDescriptorBufferInfo rbi{
				(VkBuffer)static_cast<VulkanDeviceBuffer *>(rayBuff)->buff,
				0, VK_WHOLE_SIZE};
		VkDescriptorBufferInfo hbi{
				(VkBuffer)static_cast<VulkanDeviceBuffer *>(rayHitBuff)->buff,
				0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[2]{};
		w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = rtAccel->descSet;
		w[0].dstBinding = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[0].descriptorCount = 1;
		w[0].pBufferInfo = &rbi;
		w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[1].dstSet = rtAccel->descSet;
		w[1].dstBinding = 2;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[1].descriptorCount = 1;
		w[1].pBufferInfo = &hbi;
		vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);

		EnsureCmdOpen();
		VkCommandBuffer cb = (VkCommandBuffer)openCmd;
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rtAccel->pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
				rtAccel->pipeLayout, 0, 1, &rtAccel->descSet, 0, nullptr);
		vkCmdPushConstants(cb, rtAccel->pipeLayout,
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rayCount), &rayCount);
		vkCmdDispatch(cb, (rayCount + 127) / 128, 1, 1);
	} else {
		kernel->EnqueueTraceRayBuffer(rayBuff, rayHitBuff, rayCount);
	}
	statsTotalDataParallelRayCount += rayCount;
}

// glslangValidator compiles the ray-query compute shader (OpenCL C/clspv
// cannot express rayQuery* SPIR-V ops). Resolution mirrors GetClspvPath():
// env, then the bundled vktools tree, then PATH.
static string GetGlslangPath() {
	const char *env = getenv("LUXRAYS_GLSLANG");
	if (env && env[0])
		return env;
	const char *home = getenv("HOME");
	if (home && home[0]) {
		const string bundled = string(home) + "/.luxcore/vktools/bin/glslangValidator";
		if (access(bundled.c_str(), X_OK) == 0)
			return bundled;
	}
	return "glslangValidator";
}

// Ray/RayHit mirror luxrays' 48B/20B packed layout (ray_types.cl); the
// shader addresses them as u32 words to stay layout-exact under std430.
// mask 0xFF accepts all BLAS instances; committed-hit fields map directly:
// instanceCustomIndex -> meshIndex, primitiveIndex -> triangleIndex,
// barycentrics (u,v) -> (b1,b2). Miss writes t=maxt + NULL_INDEX.
static const char *kRTShaderSrc = R"(#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 128) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(std430, set = 0, binding = 1) readonly buffer Rays { uint w[]; } rays;
layout(std430, set = 0, binding = 2) writeonly buffer Hits { uint w[]; } hits;
layout(push_constant) uniform PC { uint rayCount; } pc;

float rf(uint i) { return uintBitsToFloat(rays.w[i]); }

void main() {
	uint gid = gl_GlobalInvocationID.x;
	if (gid >= pc.rayCount)
		return;
	uint rb = gid * 12u;               // Ray = 12 u32 words
	if ((rays.w[rb + 9u] & 1u) != 0u)  // RAY_FLAGS_MASKED: leave hit untouched
		return;
	float maxt = rf(rb + 7u);

	rayQueryEXT q;
	rayQueryInitializeEXT(q, tlas, gl_RayFlagsNoneEXT, 0xFFu,
			vec3(rf(rb), rf(rb + 1u), rf(rb + 2u)), rf(rb + 6u),
			vec3(rf(rb + 3u), rf(rb + 4u), rf(rb + 5u)), maxt);
	while (rayQueryProceedEXT(q)) { }

	uint hb = gid * 5u;                // RayHit = 5 u32 words
	if (rayQueryGetIntersectionTypeEXT(q, true) ==
			gl_RayQueryCommittedIntersectionTriangleEXT) {
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(q, true);
		hits.w[hb]      = floatBitsToUint(rayQueryGetIntersectionTEXT(q, true));
		hits.w[hb + 1u] = floatBitsToUint(bary.x);
		hits.w[hb + 2u] = floatBitsToUint(bary.y);
		hits.w[hb + 3u] = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true));
		hits.w[hb + 4u] = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
	} else {
		hits.w[hb]      = floatBitsToUint(maxt);
		hits.w[hb + 1u] = floatBitsToUint(0.0);
		hits.w[hb + 2u] = floatBitsToUint(0.0);
		hits.w[hb + 3u] = 0xFFFFFFFFu;   // NULL_INDEX
		hits.w[hb + 4u] = 0xFFFFFFFFu;
	}
}
)";

VulkanIntersectionDevice::VulkanRTAccel *VulkanIntersectionDevice::BuildRTAccel() {
	const BVHAccel *bvh = dynamic_cast<const BVHAccel *>(accel.get());
	if (!bvh)
		throw runtime_error("RT path requires a BVH accelerator");
	const auto &meshes = bvh->GetMeshes();
	if (meshes.empty())
		throw runtime_error("empty scene");

	unique_ptr<VulkanRTAccel> rt(new VulkanRTAccel);
	VkDevice dev = (VkDevice)device;

	// ---- BLAS per mesh (vertices baked to world space, matching BVHKernel) ----
	vector<VkAccelerationStructureGeometryKHR> geoms(meshes.size());
	vector<VkAccelerationStructureBuildGeometryInfoKHR> blasInfos(meshes.size());
	vector<VkAccelerationStructureBuildRangeInfoKHR> blasRanges(meshes.size());
	vector<const VkAccelerationStructureBuildRangeInfoKHR *> blasRangePtrs(meshes.size());
	vector<HardwareDeviceBuffer *> scratchBuffs(meshes.size(), nullptr);
	rt->blas.resize(meshes.size());

	u_int i = 0;
	for (const Mesh *mesh : meshes) {
		const u_int nv = (u_int)mesh->GetTotalVertexCount();
		const u_int nt = (u_int)mesh->GetTotalTriangleCount();
		vector<Point> wv(nv);
		for (u_int v = 0; v < nv; ++v)
			wv[v] = mesh->GetVertex(Transform::TRANS_IDENTITY, v);
		const auto tris = mesh->GetTriangles();

		HardwareDeviceBuffer *vb = nullptr, *tb = nullptr;
		AllocBuffer(&vb, BUFFER_TYPE_READ_ONLY, wv.data(),
				nv * sizeof(Point), "rt-verts");
		AllocBuffer(&tb, BUFFER_TYPE_READ_ONLY, (void *)tris.data(),
				nt * sizeof(Triangle), "rt-tris");
		rt->ownedBuffs.push_back(vb);
		rt->ownedBuffs.push_back(tb);

		VkAccelerationStructureGeometryKHR &g = geoms[i];
		g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		auto &t = g.geometry.triangles;
		t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		t.vertexData.deviceAddress =
				static_cast<VulkanDeviceBuffer *>(vb)->deviceAddr;
		t.vertexStride = sizeof(Point);
		t.maxVertex = nv - 1; // maxVertex is the highest index, not the count
		t.indexType = VK_INDEX_TYPE_UINT32;
		t.indexData.deviceAddress =
				static_cast<VulkanDeviceBuffer *>(tb)->deviceAddr;

		auto &bi = blasInfos[i];
		bi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		bi.geometryCount = 1;
		bi.pGeometries = &g;

		VkAccelerationStructureBuildSizesInfoKHR sz{
				VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
		vkGetAccelerationStructureBuildSizesKHR(dev,
				VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &nt, &sz);

		HardwareDeviceBuffer *asb = nullptr;
		AllocBuffer(&asb, BUFFER_TYPE_READ_WRITE, nullptr,
				sz.accelerationStructureSize, "rt-blas");
		AllocBuffer(&scratchBuffs[i], BUFFER_TYPE_READ_WRITE, nullptr,
				sz.buildScratchSize, "rt-blas-scratch");
		rt->ownedBuffs.push_back(asb);

		VkAccelerationStructureCreateInfoKHR ci{
				VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
		ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		ci.size = sz.accelerationStructureSize;
		ci.buffer = (VkBuffer)static_cast<VulkanDeviceBuffer *>(asb)->buff;
		VK_CHECK(vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &rt->blas[i]));

		bi.dstAccelerationStructure = rt->blas[i];
		bi.scratchData.deviceAddress =
				static_cast<VulkanDeviceBuffer *>(scratchBuffs[i])->deviceAddr;
		blasRanges[i].primitiveCount = nt;
		blasRangePtrs[i] = &blasRanges[i];
		++i;
	}

	// ---- TLAS: one identity instance per mesh, custom index = mesh index ----
	vector<VkAccelerationStructureInstanceKHR> inst(meshes.size());
	for (u_int k = 0; k < (u_int)meshes.size(); ++k) {
		auto &in = inst[k];
		memset(&in, 0, sizeof(in));
		in.transform.matrix[0][0] = in.transform.matrix[1][1] =
				in.transform.matrix[2][2] = 1.f;
		in.instanceCustomIndex = k;
		in.mask = 0xFF;
		in.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		VkAccelerationStructureDeviceAddressInfoKHR ai{
				VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
		ai.accelerationStructure = rt->blas[k];
		in.accelerationStructureReference =
				vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
	}
	HardwareDeviceBuffer *instBuff = nullptr;
	AllocBuffer(&instBuff, BUFFER_TYPE_READ_ONLY, inst.data(),
			inst.size() * sizeof(VkAccelerationStructureInstanceKHR), "rt-instances");
	rt->ownedBuffs.push_back(instBuff);

	VkAccelerationStructureGeometryKHR tlasGeom{
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	tlasGeom.geometry.instances.sType =
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
	tlasGeom.geometry.instances.data.deviceAddress =
			static_cast<VulkanDeviceBuffer *>(instBuff)->deviceAddr;

	VkAccelerationStructureBuildGeometryInfoKHR tlasInfo{
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tlasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	tlasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	tlasInfo.geometryCount = 1;
	tlasInfo.pGeometries = &tlasGeom;

	const u_int instCount = (u_int)meshes.size();
	VkAccelerationStructureBuildSizesInfoKHR tsz{
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	vkGetAccelerationStructureBuildSizesKHR(dev,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasInfo,
			&instCount, &tsz);
	HardwareDeviceBuffer *tlasBuff = nullptr, *tlasScratch = nullptr;
	AllocBuffer(&tlasBuff, BUFFER_TYPE_READ_WRITE, nullptr,
			tsz.accelerationStructureSize, "rt-tlas");
	AllocBuffer(&tlasScratch, BUFFER_TYPE_READ_WRITE, nullptr,
			tsz.buildScratchSize, "rt-tlas-scratch");
	rt->ownedBuffs.push_back(tlasBuff);
	VkAccelerationStructureCreateInfoKHR tci{
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
	tci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tci.size = tsz.accelerationStructureSize;
	tci.buffer = (VkBuffer)static_cast<VulkanDeviceBuffer *>(tlasBuff)->buff;
	VK_CHECK(vkCreateAccelerationStructureKHR(dev, &tci, nullptr, &rt->tlas));
	tlasInfo.dstAccelerationStructure = rt->tlas;
	tlasInfo.scratchData.deviceAddress =
			static_cast<VulkanDeviceBuffer *>(tlasScratch)->deviceAddr;
	VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
	tlasRange.primitiveCount = instCount;
	const VkAccelerationStructureBuildRangeInfoKHR *tlasRangePtr = &tlasRange;

	// ---- record + submit all builds ----
	EnsureCmdOpen();
	VkCommandBuffer cb = (VkCommandBuffer)openCmd;
	vkCmdBuildAccelerationStructuresKHR(cb, (u_int)blasInfos.size(),
			blasInfos.data(), blasRangePtrs.data());
	VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			0, 1, &mb, 0, nullptr, 0, nullptr);
	vkCmdBuildAccelerationStructuresKHR(cb, 1, &tlasInfo, &tlasRangePtr);
	FinishQueue();
	for (auto &s : scratchBuffs)
		FreeBuffer(&s);
	FreeBuffer(&tlasScratch);

	// ---- ray-query compute pipeline ----
	VkDescriptorSetLayoutBinding bnd[3]{};
	bnd[0].binding = 0;
	bnd[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bnd[0].descriptorCount = 1;
	bnd[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	for (u_int k = 1; k < 3; ++k) {
		bnd[k].binding = k;
		bnd[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bnd[k].descriptorCount = 1;
		bnd[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dci{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dci.bindingCount = 3;
	dci.pBindings = bnd;
	VK_CHECK(vkCreateDescriptorSetLayout(dev, &dci, nullptr, &rt->dsLayout));

	VkDescriptorPoolSize psz[2]{};
	psz[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	psz[0].descriptorCount = 1;
	psz[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	psz[1].descriptorCount = 2;
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 1;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = psz;
	VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &rt->descPool));
	VkDescriptorSetAllocateInfo dai{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = rt->descPool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &rt->dsLayout;
	VK_CHECK(vkAllocateDescriptorSets(dev, &dai, &rt->descSet));

	VkWriteDescriptorSetAccelerationStructureKHR asw{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
	asw.accelerationStructureCount = 1;
	asw.pAccelerationStructures = &rt->tlas;
	VkWriteDescriptorSet asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw};
	asWrite.dstSet = rt->descSet;
	asWrite.dstBinding = 0;
	asWrite.descriptorCount = 1;
	asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	vkUpdateDescriptorSets(dev, 1, &asWrite, 0, nullptr);

	// Shader: glslangValidator -> vkcache, then vkCreateShaderModule
	const string home = getenv("HOME") ? getenv("HOME") : "/tmp";
	const string cacheDir = home + "/.luxcore/vkcache";
	mkdir((home + "/.luxcore").c_str(), 0755);
	mkdir(cacheDir.c_str(), 0755);
	const string hash = oclKernelPersistentCache::HashString(string(kRTShaderSrc));
	const string srcPath = cacheDir + "/vkrt-intersect-" + hash + ".comp";
	const string spvPath = cacheDir + "/vkrt-intersect-" + hash + ".spv";
	struct stat st{};
	if (stat(spvPath.c_str(), &st) != 0) {
		ofstream src(srcPath);
		src << kRTShaderSrc;
		src.close();
		ostringstream cmd;
		cmd << "\"" << GetGlslangPath() << "\" -V --target-env vulkan1.2 \"" <<
				srcPath << "\" -o \"" << spvPath << "\"";
		LR_LOG(deviceContext, "[VulkanRT] " << cmd.str());
		if (system(cmd.str().c_str()) != 0 || stat(spvPath.c_str(), &st) != 0)
			throw runtime_error("glslangValidator failed for ray-query shader");
	}
	ifstream spv(spvPath, ios::binary | ios::ate);
	const size_t spvSize = (size_t)spv.tellg();
	vector<char> spvCode(spvSize);
	spv.seekg(0);
	spv.read(spvCode.data(), spvSize);

	VkShaderModule module;
	VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	mci.codeSize = spvSize;
	mci.pCode = (const uint32_t *)spvCode.data();
	VK_CHECK(vkCreateShaderModule(dev, &mci, nullptr, &module));

	VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u_int)};
	VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	lci.setLayoutCount = 1;
	lci.pSetLayouts = &rt->dsLayout;
	lci.pushConstantRangeCount = 1;
	lci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &rt->pipeLayout));

	VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = module;
	cpci.stage.pName = "main";
	cpci.layout = rt->pipeLayout;
	VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci,
			nullptr, &rt->pipeline));
	vkDestroyShaderModule(dev, module, nullptr);

	LR_LOG(deviceContext, "[Device " << GetName() << "] Vulkan HWRT: " <<
			meshes.size() << " BLAS + TLAS built, ray-query pipeline ready");
	return rt.release();
}

}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

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

#include "luxrays/usings.h"
#include <cstdio>
#include <limits>
#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <mutex>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "luxrays/core/geometry/transform.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/devices/ocldevice.h"
#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)
#include "luxrays/devices/metaldevice.h"
#endif
#include "luxrays/kernels/kernels.h"

#include "luxcore/cfg.h"

#include "slg/slg.h"
#include "slg/kernels/kernels.h"
#include "slg/renderconfig.h"
#include "slg/engines/pathoclbase/pathoclbase.h"
#include "slg/samplers/sobol.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathOCLBaseOCLRenderThread kernels related methods
//------------------------------------------------------------------------------

std::tuple<HardwareDeviceKernelUPtr, size_t>
PathOCLBaseOCLRenderThread::CompileKernel(
		HardwareIntersectionDeviceRef device,
		HardwareDeviceProgramRef program,
		const std::string &name
) {
	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Compiling " << name << " Kernel");
	size_t workGroupSize;
	auto kernel = device.GetKernel(program, name.c_str());

	if (device.GetDeviceDesc().GetForceWorkGroupSize() > 0) {
		workGroupSize = device.GetDeviceDesc().GetForceWorkGroupSize();
	}
	else {
		workGroupSize = device.GetKernelWorkGroupSize(kernel);
		SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] "
				<< name << " workgroup size: " << workGroupSize);
	}

	return std::make_tuple(std::move(kernel), workGroupSize);
}

void PathOCLBaseOCLRenderThread::GetKernelParamters(
	std::vector<std::string> &params,
	HardwareIntersectionDeviceRef intersectionDevice,
	const string renderEngineType,
	const float epsilonMin, const float epsilonMax,
	const bool spectralEnable
) {
	params.push_back("-D LUXRAYS_OPENCL_KERNEL");
	params.push_back("-D SLG_OPENCL_KERNEL");
	params.push_back("-D RENDER_ENGINE_" + renderEngineType);
	params.push_back("-D PARAM_RAY_EPSILON_MIN=" + ToString(epsilonMin) + "f");
	params.push_back("-D PARAM_RAY_EPSILON_MAX=" + ToString(epsilonMax) + "f");
	if (spectralEnable)
		params.push_back("-D SLG_SPECTRAL");

	try {
		const auto& oclDeviceDesc =
			dynamic_cast<OpenCLDeviceDescriptionConstRef>(intersectionDevice.GetDeviceDesc());
		if (oclDeviceDesc.IsAMDPlatform())
			params.push_back("-D LUXCORE_AMD_OPENCL");
		else if (oclDeviceDesc.IsNVIDIAPlatform())
			params.push_back("-D LUXCORE_NVIDIA_OPENCL");
		else
			params.push_back("-D LUXCORE_GENERIC_OPENCL");
	}
	catch (std::bad_cast&) {
#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)
		try {
			dynamic_cast<MetalDeviceDescriptionConstRef>(
				intersectionDevice.GetDeviceDesc());
			params.push_back("-D LUXCORE_METAL");
		}
		catch (std::bad_cast&) {}
#endif
	}
}

string PathOCLBaseOCLRenderThread::GetKernelSources() {
	// Compile sources
	stringstream ssKernel;
	ssKernel <<
			// OpenCL LuxRays Types
			luxrays::ocl::KernelSource_luxrays_types <<
			luxrays::ocl::KernelSource_bvhbuild_types <<
			luxrays::ocl::KernelSource_randomgen_types <<
			luxrays::ocl::KernelSource_uv_types <<
			luxrays::ocl::KernelSource_point_types <<
			luxrays::ocl::KernelSource_vector_types <<
			luxrays::ocl::KernelSource_normal_types <<
			luxrays::ocl::KernelSource_triangle_types <<
			luxrays::ocl::KernelSource_ray_types <<
			luxrays::ocl::KernelSource_bbox_types <<
			luxrays::ocl::KernelSource_epsilon_types <<
			luxrays::ocl::KernelSource_color_types <<
			luxrays::ocl::KernelSource_frame_types <<
			luxrays::ocl::KernelSource_matrix4x4_types <<
			luxrays::ocl::KernelSource_quaternion_types <<
			luxrays::ocl::KernelSource_transform_types <<
			luxrays::ocl::KernelSource_motionsystem_types <<
			luxrays::ocl::KernelSource_trianglemesh_types <<
			luxrays::ocl::KernelSource_exttrianglemesh_types <<
			// OpenCL LuxRays Funcs
			luxrays::ocl::KernelSource_randomgen_funcs <<
			luxrays::ocl::KernelSource_atomic_funcs <<
			luxrays::ocl::KernelSource_epsilon_funcs <<
			luxrays::ocl::KernelSource_utils_funcs <<
			luxrays::ocl::KernelSource_mc_funcs <<
			luxrays::ocl::KernelSource_vector_funcs <<
			luxrays::ocl::KernelSource_ray_funcs <<
			luxrays::ocl::KernelSource_bbox_funcs <<
			luxrays::ocl::KernelSource_color_funcs <<
			luxrays::ocl::KernelSource_frame_funcs <<
			luxrays::ocl::KernelSource_matrix4x4_funcs <<
			luxrays::ocl::KernelSource_quaternion_funcs <<
			luxrays::ocl::KernelSource_transform_funcs <<
			luxrays::ocl::KernelSource_motionsystem_funcs <<
			luxrays::ocl::KernelSource_triangle_funcs <<
			luxrays::ocl::KernelSource_exttrianglemesh_funcs <<
			// OpenCL SLG Types
			slg::ocl::KernelSource_sceneobject_types <<
			slg::ocl::KernelSource_scene_types <<
			slg::ocl::KernelSource_hitpoint_types <<
			slg::ocl::KernelSource_imagemap_types <<
			slg::ocl::KernelSource_mapping_types <<
			slg::ocl::KernelSource_texture_types <<
			slg::ocl::KernelSource_bsdf_types <<
			slg::ocl::KernelSource_material_types <<
			slg::ocl::KernelSource_volume_types <<
			slg::ocl::KernelSource_sampleresult_types <<
			slg::ocl::KernelSource_film_types <<
			slg::ocl::KernelSource_filter_types <<
			slg::ocl::KernelSource_sampler_types <<
			slg::ocl::KernelSource_camera_types <<
			slg::ocl::KernelSource_light_types <<
			slg::ocl::KernelSource_dlsc_types <<
			slg::ocl::KernelSource_elvc_types <<
			slg::ocl::KernelSource_pgic_types <<
			// Spectral helpers/tables must precede every consumer
			// (hitpoint funcs, texture eval ops, materials, film splat)
			slg::ocl::KernelSource_spectral_funcs <<
			// OpenCL SLG Funcs
			slg::ocl::KernelSource_mortoncurve_funcs <<
			slg::ocl::KernelSource_evalstack_funcs <<
			slg::ocl::KernelSource_hitpoint_funcs << // Required by mapping funcs
			slg::ocl::KernelSource_mapping_funcs <<
			slg::ocl::KernelSource_imagemap_funcs <<
			slg::ocl::KernelSource_texture_bump_funcs <<
			slg::ocl::KernelSource_texture_noise_funcs <<
			slg::ocl::KernelSource_texture_blender_noise_funcs <<
			slg::ocl::KernelSource_texture_blender_noise_funcs2 <<
			slg::ocl::KernelSource_texture_blender_funcs <<
			slg::ocl::KernelSource_texture_abs_funcs <<
			slg::ocl::KernelSource_texture_bilerp_funcs <<
			slg::ocl::KernelSource_texture_blackbody_funcs <<
			slg::ocl::KernelSource_texture_bombing_funcs <<
			slg::ocl::KernelSource_texture_brick_funcs <<
			slg::ocl::KernelSource_texture_clamp_funcs <<
			slg::ocl::KernelSource_texture_colordepth_funcs <<
			slg::ocl::KernelSource_texture_densitygrid_funcs <<
			slg::ocl::KernelSource_texture_distort_funcs <<
			slg::ocl::KernelSource_texture_fresnelcolor_funcs <<
			slg::ocl::KernelSource_texture_fresnelconst_funcs <<
			slg::ocl::KernelSource_texture_hitpoint_funcs <<
			slg::ocl::KernelSource_texture_hsv_funcs <<
			slg::ocl::KernelSource_texture_irregulardata_funcs <<
			slg::ocl::KernelSource_texture_triplanar_funcs <<
			slg::ocl::KernelSource_texture_imagemap_funcs <<
			slg::ocl::KernelSource_texture_others_funcs <<
			slg::ocl::KernelSource_texture_random_funcs <<
			slg::ocl::KernelSource_texture_whitenoise_funcs <<
			slg::ocl::KernelSource_texture_funcs_evalops <<
			slg::ocl::KernelSource_texture_funcs;

	ssKernel <<
			slg::ocl::KernelSource_materialdefs_funcs_generic <<
			slg::ocl::KernelSource_materialdefs_funcs_default <<
			slg::ocl::KernelSource_materialdefs_funcs_thinfilmcoating <<
			slg::ocl::KernelSource_materialdefs_funcs_archglass <<
			slg::ocl::KernelSource_materialdefs_funcs_carpaint <<
			slg::ocl::KernelSource_materialdefs_funcs_clearvol <<
			slg::ocl::KernelSource_materialdefs_funcs_cloth <<
			slg::ocl::KernelSource_materialdefs_funcs_disney <<
			slg::ocl::KernelSource_materialdefs_funcs_glass <<
			slg::ocl::KernelSource_materialdefs_funcs_glossy2 <<
			slg::ocl::KernelSource_materialdefs_funcs_glossycoating <<
			slg::ocl::KernelSource_materialdefs_funcs_glossytranslucent <<
			slg::ocl::KernelSource_materialdefs_funcs_hair <<
			slg::ocl::KernelSource_materialdefs_funcs_heterogeneousvol <<
			slg::ocl::KernelSource_materialdefs_funcs_homogeneousvol <<
			slg::ocl::KernelSource_materialdefs_funcs_matte <<
			slg::ocl::KernelSource_materialdefs_funcs_matte_translucent <<
			slg::ocl::KernelSource_materialdefs_funcs_metal2 <<
			slg::ocl::KernelSource_materialdefs_funcs_mirror <<
			slg::ocl::KernelSource_materialdefs_funcs_mix <<
			slg::ocl::KernelSource_materialdefs_funcs_null <<
			slg::ocl::KernelSource_materialdefs_funcs_roughglass <<
			slg::ocl::KernelSource_materialdefs_funcs_roughmatte_translucent <<
			slg::ocl::KernelSource_materialdefs_funcs_twosided <<
			slg::ocl::KernelSource_materialdefs_funcs_velvet <<
			slg::ocl::KernelSource_material_funcs_evalops <<
			slg::ocl::KernelSource_material_funcs;

	ssKernel <<
			slg::ocl::KernelSource_pathdepthinfo_types <<
			slg::ocl::KernelSource_pathvolumeinfo_types <<
			slg::ocl::KernelSource_pathinfo_types <<
			slg::ocl::KernelSource_pathtracer_types <<
			// PathOCL types
			slg::ocl::KernelSource_pathoclbase_datatypes;

	ssKernel <<
			slg::ocl::KernelSource_bsdfutils_funcs << // Must be before volumeinfo_funcs
			slg::ocl::KernelSource_volume_funcs <<
			slg::ocl::KernelSource_pathdepthinfo_funcs <<
			slg::ocl::KernelSource_pathvolumeinfo_funcs <<
			slg::ocl::KernelSource_pathinfo_funcs <<
			slg::ocl::KernelSource_camera_funcs <<
			slg::ocl::KernelSource_dlsc_funcs <<
			slg::ocl::KernelSource_elvc_funcs <<
			slg::ocl::KernelSource_lightstrategy_funcs <<
			slg::ocl::KernelSource_light_funcs <<
			slg::ocl::KernelSource_filter_funcs <<
			slg::ocl::KernelSource_sampleresult_funcs <<
			slg::ocl::KernelSource_filmdenoiser_funcs <<
			slg::ocl::KernelSource_film_funcs <<
			slg::ocl::KernelSource_varianceclamping_funcs <<
			slg::ocl::KernelSource_sampler_random_funcs <<
			slg::ocl::KernelSource_sampler_sobol_funcs <<
			slg::ocl::KernelSource_sampler_metropolis_funcs <<
			slg::ocl::KernelSource_sampler_tilepath_funcs <<
			slg::ocl::KernelSource_sampler_pmj02_funcs <<
			slg::ocl::KernelSource_sampler_funcs <<
			slg::ocl::KernelSource_bsdf_funcs <<
			slg::ocl::KernelSource_scene_funcs <<
			slg::ocl::KernelSource_pgic_funcs <<
			// PathOCL Funcs
			slg::ocl::KernelSource_pathoclbase_funcs <<
			slg::ocl::KernelSource_pathoclbase_kernels_micro;

	return ssKernel.str();
}

void PathOCLBaseOCLRenderThread::InitKernels() {
	//--------------------------------------------------------------------------
	// Compile kernels
	//--------------------------------------------------------------------------

	const double tStart = WallClockTime();

	// A safety check
	switch (intersectionDevice.GetAccelerator()->GetType()) {
		case ACCEL_BVH:
			break;
		case ACCEL_MBVH:
			break;
		case ACCEL_EMBREE:
		throw runtime_error("EMBREE accelerator is not supported in PathOCLBaseRenderThread::InitKernels()");
		case ACCEL_OPTIX:
			break;
		default:
			throw runtime_error("Unknown accelerator in PathOCLBaseRenderThread::InitKernels()");
	}

	vector<string> kernelsParameters;
	GetKernelParamters(kernelsParameters, intersectionDevice,
			RenderEngine::RenderEngineType2String(renderEngine->GetType()),
			MachineEpsilon::GetMin(), MachineEpsilon::GetMax(),
			renderEngine->pathTracer.spectralEnable);

	const string kernelSource = GetKernelSources();

	if (renderEngine->writeKernelsToFile) {
		// Some debug code to write the OpenCL kernel source to a file
		const string kernelFileName = "kernel_source_device_" + ToString(threadIndex) + ".cl";
		ofstream kernelFile(kernelFileName.c_str());
		string kernelDefs = oclKernelPersistentCache::ToOptsString(kernelsParameters);
		boost::replace_all(kernelDefs, "-D", "\n#define");
		boost::replace_all(kernelDefs, "=", " ");
		kernelFile << kernelDefs << endl << endl << kernelSource << endl;
		kernelFile.close();
	}

	if ((renderEngine->additionalOpenCLKernelOptions.size() > 0) &&
			(intersectionDevice.GetDeviceDesc().GetType() & DEVICE_TYPE_OPENCL_ALL))
		kernelsParameters.insert(kernelsParameters.end(), renderEngine->additionalOpenCLKernelOptions.begin(), renderEngine->additionalOpenCLKernelOptions.end());
	if ((renderEngine->additionalCUDAKernelOptions.size() > 0) &&
			(intersectionDevice.GetDeviceDesc().GetType() & DEVICE_TYPE_CUDA_ALL))
		kernelsParameters.insert(kernelsParameters.end(), renderEngine->additionalCUDAKernelOptions.begin(), renderEngine->additionalCUDAKernelOptions.end());

	// Build the kernel source/parameters hash
	const string newKernelSrcHash = oclKernelPersistentCache::HashString(oclKernelPersistentCache::ToOptsString(kernelsParameters))
			+ "-" +
			oclKernelPersistentCache::HashString(kernelSource);
	if (newKernelSrcHash == kernelSrcHash) {
		// There is no need to re-compile the kernel
		return;
	} else
		kernelSrcHash = newKernelSrcHash;

	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Compiling kernels ");

	auto program = intersectionDevice.CompileProgram(kernelsParameters, kernelSource, "PathOCL kernel");

	std::tuple<HardwareDeviceKernelUPtr&, size_t&, const char *>
	kernels[] = {
		{filmClearKernel, filmClearWorkGroupSize, "Film_Clear"},
		{initSeedKernel, initWorkGroupSize, "InitSeed"},
		{initKernel, initWorkGroupSize, "Init"},
	};

	for (auto& [kernel, workGroupSize, name] : kernels) {
		std::tie(kernel, workGroupSize) = CompileKernel(intersectionDevice, *program, name);
	}


	// AdvancePaths kernel (Micro-Kernels)
	std::tuple<HardwareDeviceKernelUPtr&, const char *>
	microKernels[] = {
		{advancePathsKernel_MK_RT_NEXT_VERTEX, "AdvancePaths_MK_RT_NEXT_VERTEX"},
		{advancePathsKernel_MK_HIT_NOTHING, "AdvancePaths_MK_HIT_NOTHING"},
		{advancePathsKernel_MK_HIT_OBJECT, "AdvancePaths_MK_HIT_OBJECT"},
		{advancePathsKernel_MK_RT_DL, "AdvancePaths_MK_RT_DL"},
		{advancePathsKernel_MK_DL_ILLUMINATE, "AdvancePaths_MK_DL_ILLUMINATE"},
		{advancePathsKernel_MK_DL_SAMPLE_BSDF, "AdvancePaths_MK_DL_SAMPLE_BSDF"},
		{advancePathsKernel_MK_MNEE_NEXT_VERTEX, "AdvancePaths_MK_MNEE_NEXT_VERTEX"},
		{advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY, "AdvancePaths_MK_GENERATE_NEXT_VERTEX_RAY"},
		{advancePathsKernel_MK_SPLAT_SAMPLE, "AdvancePaths_MK_SPLAT_SAMPLE"},
		{advancePathsKernel_MK_NEXT_SAMPLE, "AdvancePaths_MK_NEXT_SAMPLE"},
		{advancePathsKernel_MK_GENERATE_CAMERA_RAY, "AdvancePaths_MK_GENERATE_CAMERA_RAY"},
		// Wavefront per-state queue builder (B2/E3): refills the queues
		// from taskState->state once per iteration; the histogram kernel
		// counts the per-(state, lambda) population for the host prefix.
		{advancePathsKernel_BuildQueues, "AdvancePaths_BuildQueues"},
		{advancePathsKernel_BucketHistogram, "AdvancePaths_BucketHistogram"},
	};

	advancePathsWorkGroupSize = std::numeric_limits<size_t>::max();

	for (auto& [microKernel, name] : microKernels) {
		// Compile kernel
		auto [kernel, workGroupSize] = CompileKernel(intersectionDevice, *program, name);

		// Assign to class members
		microKernel = std::move(kernel);
		advancePathsWorkGroupSize = std::min(advancePathsWorkGroupSize, workGroupSize);
	}

	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex
			<< "] AdvancePaths_MK_* workgroup size: "
			<< advancePathsWorkGroupSize
	);

	const double tEnd = WallClockTime();
	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Kernels compilation time: " << int((tEnd - tStart) * 1000.0) << "ms");

}

void PathOCLBaseOCLRenderThread::SetInitKernelArgs(const u_int filmIndex) {
	// initSeedKernel kernel
	u_int argIndex = 0;
	intersectionDevice.SetKernelArg(initSeedKernel, argIndex++, tasksBuff);
	intersectionDevice.SetKernelArg(initSeedKernel, argIndex++, renderEngine->seedBase + threadIndex * renderEngine->taskCount);

	// initKernel kernel
	argIndex = 0;
	intersectionDevice.SetKernelArg(initKernel, argIndex++, taskConfigBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, tasksBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, tasksDirectLightBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, tasksStateBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, taskStatsBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, samplerSharedDataBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, samplesBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, sampleDataBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, sampleResultsBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, eyePathInfosBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, restirReservoirsBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, pixelFilterBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, raysBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, cameraBuff);
	intersectionDevice.SetKernelArg(initKernel, argIndex++, cameraBokehDistributionBuff);

	// Film parameters
	argIndex = threadFilms[filmIndex]->SetFilmKernelArgs(intersectionDevice, initKernel, argIndex);

	initKernelArgsCount = argIndex;
}

void PathOCLBaseOCLRenderThread::SetAdvancePathsKernelArgs(
	HardwareDeviceKernelRPtr advancePathsKernel, const u_int filmIndex, const u_int queueState
) {
	CompiledScene *cscene = renderEngine->compiledScene;

	u_int argIndex = 0;
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, taskConfigBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, tasksBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, tasksDirectLightBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, tasksStateBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, taskStatsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pixelFilterBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, samplerSharedDataBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, samplesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, sampleDataBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, sampleResultsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, eyePathInfosBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, restirReservoirsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, directLightVolInfosBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, raysBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, hitsBuff);

	// Film parameters
	argIndex = threadFilms[filmIndex]->SetFilmKernelArgs(intersectionDevice, advancePathsKernel, argIndex);

	// Scene parameters
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->worldBSphere.center.x);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->worldBSphere.center.y);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->worldBSphere.center.z);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->worldBSphere.rad);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, materialsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, materialEvalOpsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, materialEvalStackBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->maxMaterialEvalStackSize);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, texturesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, textureEvalOpsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, textureEvalStackBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->maxTextureEvalStackSize);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, scnObjsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, meshDescsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, vertsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, normalsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, triNormalsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, uvsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, colsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, alphasBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, vertexAOVBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, triAOVBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, trianglesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, interpolatedTransformsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cameraBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cameraBokehDistributionBuff);
	// Lights
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, lightsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, envLightIndicesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, (u_int)cscene->envLightIndices.size());
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, lightIndexOffsetByMeshIndexBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, lightIndexByTriIndexBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, envLightDistributionsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, lightsDistributionBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, infiniteLightSourcesDistributionBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, dlscAllEntriesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, dlscDistributionsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, dlscBVHNodesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->dlscRadius2);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->dlscNormalCosAngle);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, elvcAllEntriesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, elvcDistributionsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, elvcTileDistributionOffsetsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, elvcBVHNodesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->elvcRadius2);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->elvcNormalCosAngle);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->elvcTilesXCount);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->elvcTilesYCount);

	// Images
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, imageMapDescsBuff);
	for (u_int i = 0; i < 8; ++i) {
		if (i < imageMapsBuff.size())
			intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, imageMapsBuff[i]);
		else
			intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, nullptr);
	}

	// PhotonGI cache
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pgicRadiancePhotonsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, cscene->pgicLightGroupCounts);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pgicRadiancePhotonsValuesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pgicRadiancePhotonsBVHNodesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pgicCausticPhotonsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, pgicCausticPhotonsBVHNodesBuff);

	// Path guiding (P1-3 M2b): 16 frozen coarse-table chunks (4224B each;
	// small uploads land reliably) + field bounds + enable. Chunks are
	// null (and guidingEnable 0) when unguided; kernels must not
	// dereference them then (gated on guidingEnable).
	for (u_int i = 0u; i < 16u; ++i)
		intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, guideChunkBuff[i]);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, renderEngine->guideHasTable ? 1u : 0u);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, renderEngine->guideCubeMin[0]);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, renderEngine->guideCubeMin[1]);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, renderEngine->guideCubeMin[2]);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, renderEngine->guideCubeSize);
	// Guiding stats buffer
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, guideDbgBuff);
	// Path guiding (P1-3 M2b-2): 16 training-record buffers (4KB each)
	for (u_int i = 0u; i < 16u; ++i)
		intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, guideRecBuff[i]);

	// Native curve primitives (Metal HWRT): null when no mesh carries curve
	// data; only dereferenced under RAYHIT_CURVE_FLAG hits.
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, curveCpsBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, curveSegIndicesBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, curveCpAttrsBuff);

	// Wavefront per-state task queues (B2/E3): queueState is this
	// kernel's own MK state (its input queue segment); wavefrontEnable
	// selects the queue-indirect WAVEFRONT_GID mapping in the kernels.
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, taskQueueBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, taskQueueCountBuff);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, (u_int)renderEngine->taskCount);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, queueState);
	intersectionDevice.SetKernelArg(advancePathsKernel, argIndex++, wavefrontQueues ? 1u : 0u);
}

// Mirror of the PathState enum in
// include/slg/engines/pathoclbase/kernels/pathoclbase_datatypes.cl —
// identifies each kernel's input queue segment.
namespace {
constexpr u_int MK_RT_NEXT_VERTEX = 0;
constexpr u_int MK_HIT_NOTHING = 1;
constexpr u_int MK_HIT_OBJECT = 2;
constexpr u_int MK_DL_ILLUMINATE = 3;
constexpr u_int MK_DL_SAMPLE_BSDF = 4;
constexpr u_int MK_RT_DL = 5;
constexpr u_int MK_GENERATE_NEXT_VERTEX_RAY = 6;
constexpr u_int MK_SPLAT_SAMPLE = 7;
constexpr u_int MK_NEXT_SAMPLE = 8;
constexpr u_int MK_GENERATE_CAMERA_RAY = 9;
constexpr u_int MK_DONE = 10;
constexpr u_int MK_MNEE_NEXT_VERTEX = 11;
}

void PathOCLBaseOCLRenderThread::SetAllAdvancePathsKernelArgs(const u_int filmIndex) {
	if (advancePathsKernel_MK_RT_NEXT_VERTEX)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_RT_NEXT_VERTEX, filmIndex, MK_RT_NEXT_VERTEX);
	if (advancePathsKernel_MK_HIT_NOTHING)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_HIT_NOTHING, filmIndex, MK_HIT_NOTHING);
	if (advancePathsKernel_MK_HIT_OBJECT)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_HIT_OBJECT, filmIndex, MK_HIT_OBJECT);
	if (advancePathsKernel_MK_RT_DL)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_RT_DL, filmIndex, MK_RT_DL);
	if (advancePathsKernel_MK_DL_ILLUMINATE)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_DL_ILLUMINATE, filmIndex, MK_DL_ILLUMINATE);
	if (advancePathsKernel_MK_DL_SAMPLE_BSDF)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_DL_SAMPLE_BSDF, filmIndex, MK_DL_SAMPLE_BSDF);
	if (advancePathsKernel_MK_MNEE_NEXT_VERTEX)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_MNEE_NEXT_VERTEX, filmIndex, MK_MNEE_NEXT_VERTEX);
	if (advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY, filmIndex, MK_GENERATE_NEXT_VERTEX_RAY);
	if (advancePathsKernel_MK_SPLAT_SAMPLE)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_SPLAT_SAMPLE, filmIndex, MK_SPLAT_SAMPLE);
	if (advancePathsKernel_MK_NEXT_SAMPLE)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_NEXT_SAMPLE, filmIndex, MK_NEXT_SAMPLE);
	if (advancePathsKernel_MK_GENERATE_CAMERA_RAY)
		SetAdvancePathsKernelArgs(advancePathsKernel_MK_GENERATE_CAMERA_RAY, filmIndex, MK_GENERATE_CAMERA_RAY);
}

void PathOCLBaseOCLRenderThread::SetKernelArgs() {
	// Set OpenCL kernel arguments

	// OpenCL kernel setArg() is the only non thread safe function in OpenCL 1.1 so
	// I need to use a mutex here

	std::unique_lock<std::mutex> lock(renderEngine->setKernelArgsMutex);

	//--------------------------------------------------------------------------
	// advancePathsKernels
	//--------------------------------------------------------------------------

	SetAllAdvancePathsKernelArgs(0);

	// Wavefront queue builder + histogram (B2/E3): fixed arg lists (no
	// film deps), bound once alongside the MK kernels.
	if (advancePathsKernel_BuildQueues) {
		u_int argIndex = 0;
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, tasksStateBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, sampleResultsBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, taskQueueBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, taskQueueBaseBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, taskLambdaBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BuildQueues, argIndex++, (u_int)renderEngine->taskCount);
	}
	if (advancePathsKernel_BucketHistogram) {
		u_int argIndex = 0;
		intersectionDevice.SetKernelArg(advancePathsKernel_BucketHistogram, argIndex++, tasksStateBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BucketHistogram, argIndex++, sampleResultsBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BucketHistogram, argIndex++, taskQueueCountBuff);
		intersectionDevice.SetKernelArg(advancePathsKernel_BucketHistogram, argIndex++, taskLambdaBuff);
	}

	//--------------------------------------------------------------------------
	// initKernel
	//--------------------------------------------------------------------------

	SetInitKernelArgs(0);
}

void PathOCLBaseOCLRenderThread::EnqueueAdvancePathsKernel() {
	const u_int taskCount = renderEngine->taskCount;

	// Micro kernels version
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_RT_NEXT_VERTEX,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_HIT_NOTHING,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_HIT_OBJECT,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_RT_DL,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_DL_ILLUMINATE,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_DL_SAMPLE_BSDF,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	// MNEE specular chain sub-state machine. Enqueued after
	// MK_DL_SAMPLE_BSDF (so a task leaving MK_RT_DL with a started solve is
	// only skipped by its needsTrace flag in this same iteration) and
	// before MK_GENERATE_NEXT_VERTEX_RAY (so a finished solve transitions
	// to the next path vertex in the same iteration).
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_MNEE_NEXT_VERTEX,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_SPLAT_SAMPLE,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_NEXT_SAMPLE,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
	intersectionDevice.EnqueueKernel(advancePathsKernel_MK_GENERATE_CAMERA_RAY,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));
}

void PathOCLBaseOCLRenderThread::EnqueueAdvancePathsWavefront() {
	const u_int taskCount = renderEngine->taskCount;

	// Zero the per-(state, lambda) histogram counters, then count every
	// live task's lambda bucket (AdvancePaths_BucketHistogram).
	static const u_int zeros[WAVEFRONT_NUM_STATES * WAVEFRONT_NUM_LAMBDA] = { 0u };
	intersectionDevice.EnqueueWriteBuffer(taskQueueCountBuff,
			CL_FALSE, sizeof(zeros), zeros);
	intersectionDevice.EnqueueKernel(advancePathsKernel_BucketHistogram,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));

	// Read back the histogram (blocking) to size each per-state launch
	// and to prefix the lambda segment bases. One host<->device sync
	// per wavefront iteration.
	intersectionDevice.EnqueueReadBuffer(taskQueueCountBuff,
			CL_TRUE, sizeof(u_int) * wavefrontQueueCounts.size(),
			wavefrontQueueCounts.data());

	// Exclusive prefix per state: base[s][0] = 0, base[s][l] =
	// base[s][l-1] + count[s][l-1]; totals feed the launch sizing.
	// Stack array (not static) — render threads call this concurrently.
	u_int queueBases[WAVEFRONT_NUM_STATES * WAVEFRONT_NUM_LAMBDA];
	for (u_int s = 0; s < WAVEFRONT_NUM_STATES; ++s) {
		u_int base = 0;
		for (u_int l = 0; l < WAVEFRONT_NUM_LAMBDA; ++l) {
			queueBases[s * WAVEFRONT_NUM_LAMBDA + l] = base;
			base += wavefrontQueueCounts[s * WAVEFRONT_NUM_LAMBDA + l];
		}
		wavefrontQueueTotals[s] = base;
	}
	// Blocking write: queueBases is stack storage, so the copy must
	// complete before returning (the buffer is consumed by the
	// immediately-following BuildQueues launch anyway).
	intersectionDevice.EnqueueWriteBuffer(taskQueueBaseBuff,
			CL_TRUE, sizeof(queueBases), queueBases);

	// Refill the queues: tasks land in lambda-contiguous segments of
	// their state's flat queue region (AdvancePaths_BuildQueues).
	intersectionDevice.EnqueueKernel(advancePathsKernel_BuildQueues,
			HardwareDeviceRange(taskCount), HardwareDeviceRange(advancePathsWorkGroupSize));

	// Debug (LUXRAYS_WAVEFRONT_DEBUG=1): validate that every queued task
	// index really is in the queue's state and appears exactly once,
	// plus the M2 lambda-coherence metrics (per-entry lambda check,
	// lambda transitions and run length in launch order).
	static const bool wavefrontDebug = getenv("LUXRAYS_WAVEFRONT_DEBUG") != nullptr;
	static u_int dbgIter = 0;
	if (wavefrontDebug && dbgIter++ < 8) {
		std::vector<u_int> q(WAVEFRONT_NUM_STATES * taskCount);
		intersectionDevice.EnqueueReadBuffer(taskQueueBuff,
				CL_TRUE, sizeof(u_int) * q.size(), q.data());
		std::vector<slg::ocl::pathoclbase::GPUTaskState> st(taskCount);
		intersectionDevice.EnqueueReadBuffer(tasksStateBuff,
				CL_TRUE, sizeof(st[0]) * taskCount, st.data());
		std::vector<u_int> lam(taskCount);
		intersectionDevice.EnqueueReadBuffer(taskLambdaBuff,
				CL_TRUE, sizeof(u_int) * taskCount, lam.data());
		u_int total = 0, badState = 0, badLambda = 0, dup = 0, oob = 0;
		u_int lamTrans = 0;
		std::vector<u_int> seen(taskCount, 0);
		for (u_int s = 0; s < WAVEFRONT_NUM_STATES; ++s) {
			u_int prevLambda = WAVEFRONT_NUM_LAMBDA;
			for (u_int i = 0; i < wavefrontQueueTotals[s]; ++i) {
				const u_int tid = q[s * taskCount + i];
				++total;
				if (tid >= taskCount) { ++oob; continue; }
				if (seen[tid]++) ++dup;
				if ((u_int)st[tid].state != s) ++badState;
				// The launch-order lambda must match the segment the
				// entry sits in: entries [0,count0) are lambda 0, etc.
				u_int seg = 0;
				u_int acc = wavefrontQueueCounts[s * WAVEFRONT_NUM_LAMBDA];
				while (seg < WAVEFRONT_NUM_LAMBDA - 1 && i >= acc) {
					++seg;
					acc += wavefrontQueueCounts[s * WAVEFRONT_NUM_LAMBDA + seg];
				}
				if (lam[tid] != seg) ++badLambda;
				if (lam[tid] != prevLambda) {
					++lamTrans;
					prevLambda = lam[tid];
				}
			}
		}
		u_int doneCount = 0;
		for (u_int t = 0; t < taskCount; ++t)
			if ((u_int)st[t].state == MK_DONE) ++doneCount;
		SLG_LOG("[WFDBG it=" << (dbgIter-1) << "] queued=" << total
				<< " done=" << doneCount << " oob=" << oob
				<< " dup=" << dup << " badState=" << badState
				<< " badLambda=" << badLambda
				<< " lamTrans=" << lamTrans
				<< " lamRunLen=" << (lamTrans ? (double)total / lamTrans : (double)total)
				<< " totals=[" << wavefrontQueueTotals[0] << ","
				<< wavefrontQueueTotals[1] << "," << wavefrontQueueTotals[2]
				<< "," << wavefrontQueueTotals[3] << "," << wavefrontQueueTotals[4]
				<< "," << wavefrontQueueTotals[5] << "," << wavefrontQueueTotals[6]
				<< "," << wavefrontQueueTotals[7] << "," << wavefrontQueueTotals[8]
				<< "," << wavefrontQueueTotals[9] << "," << wavefrontQueueTotals[10]
				<< "," << wavefrontQueueTotals[11] << "]");
	}

	// Launch each non-empty state kernel over its own queue, in the same
	// topological order as the dense path. A task advances (at most) one
	// state per iteration; tasks produced into an already-consumed state
	// wait for the next BuildQueues pass.
	static const std::pair<HardwareDeviceKernelUPtr PathOCLBaseOCLRenderThread::*, u_int>
	dispatchTable[] = {
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_RT_NEXT_VERTEX, MK_RT_NEXT_VERTEX},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_HIT_NOTHING, MK_HIT_NOTHING},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_HIT_OBJECT, MK_HIT_OBJECT},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_RT_DL, MK_RT_DL},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_DL_ILLUMINATE, MK_DL_ILLUMINATE},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_DL_SAMPLE_BSDF, MK_DL_SAMPLE_BSDF},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_MNEE_NEXT_VERTEX, MK_MNEE_NEXT_VERTEX},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY, MK_GENERATE_NEXT_VERTEX_RAY},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_SPLAT_SAMPLE, MK_SPLAT_SAMPLE},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_NEXT_SAMPLE, MK_NEXT_SAMPLE},
		{&PathOCLBaseOCLRenderThread::advancePathsKernel_MK_GENERATE_CAMERA_RAY, MK_GENERATE_CAMERA_RAY},
	};

	for (const auto &[kernelMember, state] : dispatchTable) {
		const u_int count = wavefrontQueueTotals[state];
		const HardwareDeviceKernelUPtr &kernel = this->*kernelMember;
		if (count && kernel) {
			// OpenCL requires the global size to be a multiple of the
			// workgroup size: round up; lanes past count exit on the
			// taskQueueCount bound check (WAVEFRONT_GUARD).
			const size_t launchSize = ((size_t)count + advancePathsWorkGroupSize - 1)
					/ advancePathsWorkGroupSize * advancePathsWorkGroupSize;
			intersectionDevice.EnqueueKernel(kernel,
					HardwareDeviceRange(launchSize), HardwareDeviceRange(advancePathsWorkGroupSize));
		}
	}
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

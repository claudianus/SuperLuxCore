################################################################################
# Copyright 1998-2020 by authors (see AUTHORS.txt)
#
#   This file is part of LuxCoreRender.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
################################################################################

################################################################################
################################################################################
################################################################################
###
### LuxRays Library
###
################################################################################
################################################################################
################################################################################

# Configure a header file to pass some of the CMake settings
# to the source code:

set(generated_header_dir "${GENERATED_INCLUDE_DIR}/luxrays")

file(MAKE_DIRECTORY ${generated_header_dir})

configure_file(
    "${PROJECT_SOURCE_DIR}/include/luxrays/cfg.h.in"
    "${generated_header_dir}/cfg.h")

################################################################################
#
# LuxRays Docs
#
################################################################################

include(doxylux)
generate_doc(luxrays)

################################################################################
#
# Kernel files
#
################################################################################

set(LUXRAYS_KERNELS
  ${PROJECT_SOURCE_DIR}/include/luxrays/luxrays_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/color/color_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/color/color_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/bvh/bvhbuild_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/accelerators/bvh.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/accelerators/mbvh.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/accelerators/optixaccel.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/accelerators/optixemptyaccel.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/exttrianglemesh_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/exttrianglemesh_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/randomgen_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/randomgen_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/trianglemesh_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/matrix4x4_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/matrix4x4_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/transform_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/transform_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/frame_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/frame_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/epsilon_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/epsilon_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/ray_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/ray_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/point_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/vector_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/vector_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/normal_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/quaternion_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/quaternion_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/bbox_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/bbox_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/motionsystem_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/motionsystem_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/triangle_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/triangle_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/geometry/uv_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/core/utils_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/devices/cudadevice_math.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/devices/cudadevice_oclemul_types.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/devices/cudadevice_oclemul_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/devices/ocldevice_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/utils/atomic_funcs.cl
  ${PROJECT_SOURCE_DIR}/include/luxrays/utils/mc_funcs.cl
)

# Make sure some IDE won't try to compile CL by some built-in rule:
SET_SOURCE_FILES_PROPERTIES(
    ${LUXRAYS_KERNELS} PROPERTIES HEADER_FILE_ONLY TRUE)

SOURCE_GROUP("Source Files\\LuxRays Kernels" FILES ${LUXRAYS_KERNELS})

################################################################################
#
# Library project files
#
################################################################################

set(LUXRAYS_SRCS
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/bvhaccel.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/bvhaccelhw.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/embreeaccel.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/mbvhaccel.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/mbvhaccelhw.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/optixaccel.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/accelerators/optixaccelhw.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/bvh/bvhbuild.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/bvh/bvhclassicbuild.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/bvh/bvhembreebuild.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/color.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spd.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/blackbodyspd.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/equalspd.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/frequencyspd.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/gaussianspd.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/irregular.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/regular.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/rgbillum.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spds/rgbrefl.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spectral.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/spectrumwavelengths.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/color/swcspectrum.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/accelerator.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/context.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/dataset.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/device.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/epsilon.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/exttrianglemesh.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/exttrianglemeshbevel.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/exttrianglemeshfile.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/hardwaredevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/hardwareintersectiondevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/init.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/intersectiondevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/namedobject.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/namedobjectvector.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/trianglemesh.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/basictypeserialization.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/bbox.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/matrix4x4.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/motionsystem.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/quaternion.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/core/geometry/transform.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/cudadevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/cudaintersectiondevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/nativeintersectiondevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/ocldevice.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/oclintersectiondevice.cpp

  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/buffer.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/config.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/cuda.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/mc.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/ocl.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/safesave.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/serializationutils.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/thread.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/ply/rply.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/properties.cpp
  ${PROJECT_SOURCE_DIR}/src/luxrays/utils/utils.cpp
  ${PROJECT_SOURCE_DIR}/deps/cuew/src/cuew.cpp
  ${PROJECT_SOURCE_DIR}/deps/clew/src/clew.cpp
)
SOURCE_GROUP("Source Files\\LuxRays Library" FILES ${LUXRAYS_SRCS})


set(generated_kernels_dir "${GENERATED_DIR}/luxrays-kernels")
file(MAKE_DIRECTORY ${generated_kernels_dir})

PreprocessOCLKernels(${generated_kernels_dir} luxrays ${LUXRAYS_KERNELS})

set(LUXRAYS_SRCS_KERNELS
  ${generated_kernels_dir}/luxrays_types_kernel.cpp
  ${generated_kernels_dir}/cudadevice_math_kernel.cpp
  ${generated_kernels_dir}/cudadevice_oclemul_types_kernel.cpp
  ${generated_kernels_dir}/cudadevice_oclemul_funcs_kernel.cpp
  ${generated_kernels_dir}/ocldevice_funcs_kernel.cpp
  ${generated_kernels_dir}/bvhbuild_types_kernel.cpp
  ${generated_kernels_dir}/bvh_kernel.cpp
  ${generated_kernels_dir}/mbvh_kernel.cpp
  ${generated_kernels_dir}/optixaccel_kernel.cpp
  ${generated_kernels_dir}/optixemptyaccel_kernel.cpp
  ${generated_kernels_dir}/randomgen_types_kernel.cpp
  ${generated_kernels_dir}/randomgen_funcs_kernel.cpp
  ${generated_kernels_dir}/trianglemesh_types_kernel.cpp
  ${generated_kernels_dir}/exttrianglemesh_types_kernel.cpp
  ${generated_kernels_dir}/exttrianglemesh_funcs_kernel.cpp
  ${generated_kernels_dir}/matrix4x4_types_kernel.cpp
  ${generated_kernels_dir}/matrix4x4_funcs_kernel.cpp
  ${generated_kernels_dir}/transform_types_kernel.cpp
  ${generated_kernels_dir}/transform_funcs_kernel.cpp
  ${generated_kernels_dir}/frame_types_kernel.cpp
  ${generated_kernels_dir}/frame_funcs_kernel.cpp
  ${generated_kernels_dir}/epsilon_types_kernel.cpp
  ${generated_kernels_dir}/epsilon_funcs_kernel.cpp
  ${generated_kernels_dir}/ray_types_kernel.cpp
  ${generated_kernels_dir}/ray_funcs_kernel.cpp
  ${generated_kernels_dir}/point_types_kernel.cpp
  ${generated_kernels_dir}/vector_types_kernel.cpp
  ${generated_kernels_dir}/vector_funcs_kernel.cpp
  ${generated_kernels_dir}/normal_types_kernel.cpp
  ${generated_kernels_dir}/quaternion_types_kernel.cpp
  ${generated_kernels_dir}/quaternion_funcs_kernel.cpp
  ${generated_kernels_dir}/bbox_types_kernel.cpp
  ${generated_kernels_dir}/bbox_funcs_kernel.cpp
  ${generated_kernels_dir}/motionsystem_types_kernel.cpp
  ${generated_kernels_dir}/motionsystem_funcs_kernel.cpp
  ${generated_kernels_dir}/triangle_types_kernel.cpp
  ${generated_kernels_dir}/triangle_funcs_kernel.cpp
  ${generated_kernels_dir}/color_types_kernel.cpp
  ${generated_kernels_dir}/color_funcs_kernel.cpp
  ${generated_kernels_dir}/uv_types_kernel.cpp
  ${generated_kernels_dir}/mc_funcs_kernel.cpp
  ${generated_kernels_dir}/utils_funcs_kernel.cpp
  ${generated_kernels_dir}/atomic_funcs_kernel.cpp
)

SOURCE_GROUP(
    "Source Files\\LuxRays Kernels\\Generated"
    FILES ${LUXRAYS_SRCS_KERNELS})

set(LUXRAYS_LIB_SRCS
  ${LUXRAYS_SRCS}
  ${LUXRAYS_KERNELS}
  ${LUXRAYS_SRCS_KERNELS}
)

add_library(luxrays STATIC ${LUXRAYS_LIB_SRCS})
add_definitions(-DCPP_API_EXPORTS ${VISIBILITY_FLAGS}) # for controlling visibility
target_link_libraries(luxrays PRIVATE
  embree::embree
  openimageio::openimageio
  boost::boost
  OpenMP::OpenMP_CXX
)
if(APPLE)
  target_link_libraries(luxrays PRIVATE
    "-framework Metal"
    "-framework Foundation"
  )
  # Metal device sources are ObjC++ (.mm). This CMake's ninja generator
# breaks on per-source LANGUAGE overrides in the main list AND ignores
# $<TARGET_OBJECTS> for STATIC archives, so: compile the .mm pair via a
# helper OBJECT library, then pass the resulting .o files DIRECTLY into
# the archive (object files in add_library sources pass through to the
# archive with real dependency tracking).
add_library(luxrays_metalobj STATIC
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metaldevice.mm
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metalintersectiondevice.mm
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metalrtaccel.mm)
# absolute path to the embedded .cl->MSL translator (cwd-relative fails
# when the render engine is started from another directory)
target_compile_definitions(luxrays_metalobj PRIVATE
  LUXCORE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# gnu++ std mode: dispatch's Swift-interop headers reject strict
# c++23 in ObjC mode on this SDK
target_compile_options(luxrays_metalobj PRIVATE "-std=gnu++23")
set_source_files_properties(
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metaldevice.mm
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metalintersectiondevice.mm
  ${PROJECT_SOURCE_DIR}/src/luxrays/devices/metalrtaccel.mm
  PROPERTIES LANGUAGE OBJCXX)
target_include_directories(luxrays_metalobj PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/deps/cuew/include
  ${PROJECT_SOURCE_DIR}/deps/clew/include
  ${PROJECT_SOURCE_DIR}/deps/optix-7.1.0/include
  ${GENERATED_INCLUDE_DIR})
# Dependency include dirs come from the conan-generated imported targets
# (INTERFACE_INCLUDE_DIRECTORIES propagates through target_link_libraries),
# which resolves version/arch/config-correct paths automatically.
target_link_libraries(luxrays_metalobj PRIVATE
  boost::boost
  onetbb::onetbb
  openimageio::openimageio
  Imath::Imath
  openexr::openexr
  embree::embree)
set_property(TARGET luxrays_metalobj PROPERTY POSITION_INDEPENDENT_CODE ON)
# Propagate the Metal archive: every luxrays consumer (pyluxcore,
# samples, tests) links BOTH archives - libluxrays.a references the
# Metal symbols, libluxrays_metalobj.a defines them.
target_link_libraries(luxrays PUBLIC luxrays_metalobj)
endif()

# BVH dumper: builds the real engine BVH for PLY meshes and writes the
# exact GPU buffers the OpenCL backend uploads (vertices / nodes with
# global leaf indices / per-triangle normals / meta). Used by the
# Metal PoC to render arbitrary geometry through the engine's own
# builder (see metal-poc/bvh_dump.cpp + bvhaccelhw.cpp BVHKernel).
add_executable(bvh_dump ${CMAKE_CURRENT_SOURCE_DIR}/../../../metal-poc/bvh_dump.cpp)
add_executable(enum_metal_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../metal-poc/enum_metal_test.cpp)
add_executable(metal_kernel_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../../../metal-poc/metal_kernel_smoke.cpp)
add_executable(metal_initseed_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../../../metal-poc/metal_initseed_smoke.cpp)
set_target_properties(bvh_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(bvh_dump PRIVATE
  luxrays
  embree::embree
  openimageio::openimageio
  boost::boost
  OpenMP::OpenMP_CXX
)
# Metal device enumeration smoke test (Context must list DEVICE_TYPE_METAL_GPU)
target_link_libraries(enum_metal_test PRIVATE
  luxrays
  embree::embree
  openimageio::openimageio
  boost::boost
  OpenMP::OpenMP_CXX
  "-framework Metal"
  "-framework Foundation")
target_link_libraries(metal_kernel_smoke PRIVATE
  luxrays
  embree::embree
  openimageio::openimageio
  boost::boost
  OpenMP::OpenMP_CXX
  "-framework Metal"
  "-framework Foundation")
target_link_libraries(metal_initseed_smoke PRIVATE
  luxrays
  embree::embree
  openimageio::openimageio
  boost::boost
  OpenMP::OpenMP_CXX
  "-framework Metal"
  "-framework Foundation")
set_target_properties(enum_metal_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

################################################################################
#
# Predefines
#
################################################################################

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "../lib")

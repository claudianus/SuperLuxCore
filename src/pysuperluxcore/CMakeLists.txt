################################################################################
# Copyright 1998-2025 by Authors (see AUTHORS.txt)
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
#
# PySuperLuxCore (Python bindings)
#
################################################################################

set(PYSUPERLUXCORE_SRCS)
include_directories(${PROJECT_SOURCE_DIR}/deps/bcd-1.1/include)

pybind11_add_module(pysuperluxcore MODULE
  ${PROJECT_SOURCE_DIR}/src/pysuperluxcore/pysuperluxcore.cpp
  ${PROJECT_SOURCE_DIR}/src/pysuperluxcore/pysuperluxcoreforblender.cpp
)

target_link_libraries(pysuperluxcore PRIVATE
    onetbb::onetbb
    oidn::oidn
    luxcore_static
    OpenVDB::openvdb
    pybind11::pybind11
    Python::Module
    tsl::robin_map
    openimageio::openimageio
    embree::embree
)

if(APPLE)
  target_link_libraries(pysuperluxcore PRIVATE OpenMP::OpenMP)
else()
  target_link_libraries(pysuperluxcore PRIVATE OpenMP::OpenMP_CXX)
endif(APPLE)

set_target_properties(pysuperluxcore PROPERTIES PREFIX "")
if(MSVC)
  set_target_properties(pysuperluxcore PROPERTIES SUFFIX ".pyd")
endif(MSVC)

# Install
INSTALL(
  TARGETS pysuperluxcore
  DESTINATION pysuperluxcore
  COMPONENT pysuperluxcore
  PERMISSIONS
    OWNER_WRITE OWNER_READ OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)

# The OpenCL -> Metal kernel translator is a runtime dependency of the
# Metal backend (src/luxrays/devices/metaldevice.mm runs it to build the
# MSL programs). It has to ship inside the wheel, next to the extension
# module, because the compile-time source root it used to fall back on
# only exists in a build tree.
install(FILES "${PROJECT_SOURCE_DIR}/src/slg/utils/cl2msl.py"
  DESTINATION pysuperluxcore
  COMPONENT pysuperluxcore
)

# Vulkan kernel compilation needs an external toolchain (clspv, opt,
# llvm-dis, glslangValidator, plus MoltenVK on macOS). The engine looks
# for it next to the installed module (vkdevice.cpp, dladdr) so a wheel
# that carries it works with no environment setup.
#
# Off by default: the set is ~168 MB and it only serves the opt-in
# Vulkan backend. Turn it on when the toolchain has been staged (see
# dev-tools/vulkan-tools-install.sh and VKTOOLS_DIR).
option(PYSUPERLUXCORE_BUNDLE_VKTOOLS
  "Bundle the Vulkan toolchain into the wheel (needs VKTOOLS_DIR)" OFF)
set(VKTOOLS_DIR "" CACHE PATH
  "Vulkan toolchain root to bundle (the directory holding bin/, lib/ and third_party/)")

if(PYSUPERLUXCORE_BUNDLE_VKTOOLS)
  if(NOT VKTOOLS_DIR)
    if(DEFINED ENV{LUX_VKTOOLS})
      set(VKTOOLS_DIR "$ENV{LUX_VKTOOLS}")
    else()
      set(VKTOOLS_DIR "$ENV{HOME}/.luxcore/vktools")
    endif()
  endif()
  if(NOT EXISTS "${VKTOOLS_DIR}/bin/clspv")
    message(FATAL_ERROR
      "PYSUPERLUXCORE_BUNDLE_VKTOOLS=ON but no clspv under "
      "'${VKTOOLS_DIR}/bin'. Run dev-tools/vulkan-tools-install.sh "
      "(LUX_VKTOOLS=${VKTOOLS_DIR}) or set VKTOOLS_DIR.")
  endif()
  message(STATUS "Bundling the Vulkan toolchain from ${VKTOOLS_DIR}")
  install(DIRECTORY "${VKTOOLS_DIR}/"
    DESTINATION pysuperluxcore/vktools
    COMPONENT pysuperluxcore
    USE_SOURCE_PERMISSIONS)
endif()

# Install pdb file (if msvc and debug)
if(MSVC)
  install(FILES "$<$<CONFIG:Debug>:$<TARGET_PDB_FILE:pysuperluxcore>>"
    DESTINATION pysuperluxcore
    COMPONENT pysuperluxcore
  )
endif()

# MacOS debug info
if(APPLE)
  find_program(DSYMUTIL_PROGRAM dsymutil)
  if (DSYMUTIL_PROGRAM)
    MESSAGE(STATUS "Found dsymutil: ${DSYMUTIL_PROGRAM}")
    foreach(lang C CXX)
      foreach(var LINK_EXECUTABLE CREATE_SHARED_LIBRARY CREATE_SHARED_MODULE)
        set(CMAKE_${lang}_${var} "${CMAKE_${lang}_${var}}" "${DSYMUTIL_PROGRAM} $<TARGET_FILE>")
      endforeach()
    endforeach()
    install(FILES "$<$<CONFIG:Debug,RelWithDebInfo>:$<TARGET_FILE:pysuperluxcore>.dsym>"
      DESTINATION pysuperluxcore
      COMPONENT pysuperluxcore
    )
  endif()
endif()

## OIDN
## oidnDenoise
#add_executable(oidn-denoise IMPORTED)
#set_property(TARGET oidn-denoise PROPERTY IMPORTED_LOCATION "${LUX_OIDN_DENOISE_PATH}")

#if(WIN32)
  ## Windows lacks rpath system, so we have to put oidnDenoise in the same
  ## folder as the dlls...
  #set(OIDN_DESTINATION "pysuperluxcore.libs")
#else()
  ## For Macos, reminder: don't put it in pysuperluxcore/.dylibs, it would prevent
  ## other deps' relocation
  #set(OIDN_DESTINATION "pysuperluxcore.oidn")
#endif(WIN32)

if(LINUX)
  #set_property(
    #TARGET oidn-denoise
    #PROPERTY INSTALL_RPATH "\\\${ORIGIN}/../pysuperluxcore.libs"
  #)
  install(
    PROGRAMS "${LUX_OIDN_DEVICE_CPU}"
    DESTINATION "pysuperluxcore.libs"
    COMPONENT pysuperluxcore
  )
endif(LINUX)

#if(APPLE)
  #set_target_properties(
    #oidn-denoise PROPERTIES
    #INSTALL_RPATH "\\\${ORIGIN}/../pysuperluxcore/.pylibs"
    #BUILD_RPATH ""
  #)
#endif(APPLE)

#INSTALL(
  #IMPORTED_RUNTIME_ARTIFACTS oidn-denoise
  #DESTINATION ${OIDN_DESTINATION}
  #PERMISSIONS
    #OWNER_WRITE OWNER_READ OWNER_EXECUTE
    #GROUP_READ GROUP_EXECUTE
    #WORLD_READ WORLD_EXECUTE
  #COMPONENT pysuperluxcore
#)


if(WIN32)
  # For Win32, we need to add device_cpu
  # device_cpu should first have a pyd extension to be repaired.
  # This will be reverted after repairing
  install(
    PROGRAMS "${LUX_OIDN_DENOISE_BINS}/LuxOpenImageDenoise_device_cpu.dll"
    DESTINATION "pysuperluxcore.libs"
    RENAME "LuxOpenImageDenoise_device_cpu.pyd"
    COMPONENT pysuperluxcore
  )

  ## Delvewheel doesn't handle executables, however we need to delocate oidnDenoise.
  ## Therefore we had to let Delvewheel believe oidnDenoise is a lib, and now
  ## we have to make it an executable again...
  #message(STATUS "oidnDenoise suffix back to pyd")
  #INSTALL(
    #CODE
    #"
    #MESSAGE(STATUS
        #\"Renaming '$<INSTALL_PREFIX>/pysuperluxcore.libs/oidnDenoise.exe' \
        #into '$<INSTALL_PREFIX>/pysuperluxcore.libs/oidnDenoise.pyd'\"
    #)
    #FILE(RENAME
        #$<INSTALL_PREFIX>/pysuperluxcore.libs/oidnDenoise.exe
        #$<INSTALL_PREFIX>/pysuperluxcore.libs/oidnDenoise.pyd
    #)
    #"
    #COMPONENT pysuperluxcore
  #)

endif(WIN32)


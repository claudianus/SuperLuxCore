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

#ifndef _LUXRAYS_VULKANINTERSECTIONDEVICE_H
#define	_LUXRAYS_VULKANINTERSECTIONDEVICE_H

#include "luxrays/devices/vkdevice.h"
#include "luxrays/core/hardwareintersectiondevice.h"

#if !defined(LUXRAYS_DISABLE_VULKAN)

namespace luxrays {

//------------------------------------------------------------------------------
// VulkanIntersectionDevice
//------------------------------------------------------------------------------

class VulkanIntersectionDevice : public VulkanDevice, public HardwareIntersectionDevice {
public:
	VulkanIntersectionDevice(
		ContextConstRef context,
		VulkanDeviceDescriptionConstRef desc,
		const size_t devIndex
	);
	virtual ~VulkanIntersectionDevice();

	virtual void SetDataSet(DataSetSPtr newDataSet);
	virtual void Start();
	virtual void Stop();

	//--------------------------------------------------------------------------
	// Data parallel interface: to trace a multiple rays (i.e. on the GPU)
	//--------------------------------------------------------------------------

	virtual void EnqueueTraceRayBuffer(HardwareDeviceBuffer *rayBuff,
			HardwareDeviceBuffer *rayHitBuff,
			const unsigned int rayCount);

	virtual bool HasHWSupport() const override {
		// Device capability before Start(), the actual path taken after.
		return rtAccel ? true : hasRayTracing;
	}

	friend class Context;

protected:
	virtual void Update();

	HardwareIntersectionKernelUPtr kernel;

	// VK_KHR_ray_query + VK_KHR_acceleration_structure state. Opaque to
	// keep Vulkan types out of the header; nullptr => SW traversal kernel.
	struct VulkanRTAccel;
	std::unique_ptr<VulkanRTAccel> rtAccel;
	VulkanRTAccel *BuildRTAccel();
	void FreeRTAccel();
};

}

#endif

#endif	/* _LUXRAYS_VULKANINTERSECTIONDEVICE_H */

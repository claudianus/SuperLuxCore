// End-to-end Vulkan intersection test: drives the real engine path
//   Context -> VulkanIntersectionDevice -> BVHAccel::NewHardwareIntersectionKernel
//   -> vkdevice CompileProgram (clspv -> SPIR-V -> reflection -> descriptors)
//   -> EnqueueTraceRayBuffer on GPU, results checked against the CPU BVH
//   accelerator (accel->Intersect) ray by ray.
//
// Mirrors the standalone dev-tools/vkrt/bench/bvh_test.cpp harness, but all
// GPU work goes through the LuxCore device layer.
#include "luxrays/core/context.h"

// same circular-dep stub the metal tests need (slg debug handler is
// referenced by ocldevice.cpp but lives in the slg library)
namespace slg { void (*SLG_DebugHandler)(const char *msg) = nullptr; }

#include "luxrays/core/device.h"
#include "luxrays/core/dataset.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/core/hardwareintersectiondevice.h"
#include "luxrays/core/accelerator.h"
#include "luxrays/core/geometry/ray.h"
#include <iostream>
#include <vector>
#include <cmath>
using namespace luxrays;

static void logCB(const char *msg) { std::cout << "[log] " << msg << std::endl; }

int main() {
	Context ctx(logCB);
	auto descs = ctx.GetAvailableDeviceDescriptions();

	DeviceDescriptions vkDescs;
	for (auto &d : descs)
		if (d.get().GetType() & DEVICE_TYPE_VULKAN_ALL)
			vkDescs.push_back(d);
	if (vkDescs.empty()) {
		std::cout << "VK_INTERSECT: FAIL (no Vulkan device)" << std::endl;
		return 1;
	}
	auto idevices = ctx.AddIntersectionDevices(vkDescs);

	// Two-triangle quad (same geometry as the standalone harness)
	VertexBuffer verts(4);
	verts[0] = Point(-1.f, -1.f, 0.f);
	verts[1] = Point( 1.f, -1.f, 0.f);
	verts[2] = Point( 1.f,  1.f, 0.f);
	verts[3] = Point(-1.f,  1.f, 0.f);
	TriangleBuffer tris(2);
	tris[0] = Triangle(0, 1, 2);
	tris[1] = Triangle(0, 2, 3);
	auto mesh = std::make_unique<TriangleMesh>(std::move(verts), std::move(tris));

	auto dataSet = std::make_shared<DataSet>(ctx);
	dataSet->Add(*mesh);
	dataSet->SetAcceleratorType(ACCEL_BVH);
	dataSet->Preprocess();
	ctx.SetDataSet(dataSet);
	ctx.Start();

	auto &dev = dynamic_cast<HardwareIntersectionDeviceRef>(idevices[0].get());

	// Ray set: hits tri0, hits tri1, miss, masked, plus edge cases
	std::vector<Ray> rays(8);
	auto mk = [&](int i, float ox, float oy, float dx, float dy, unsigned flags = RAY_FLAGS_NONE) {
		rays[i] = Ray(Point(ox, oy, 1.f), Vector(dx, dy, -1.f));
		rays[i].flags = flags;
	};
	// NOTE: keep hit points strictly inside one triangle — the shared
	// quad diagonal (x == y) is claimed by both leaves and either answer
	// is valid.
	mk(0,  0.5f, -0.5f, 0.f, 0.f);           // tri 0 (x > y)
	mk(1, -0.5f,  0.5f, 0.f, 0.f);           // tri 1 (x < y)
	mk(2,  5.0f,  5.0f, 0.f, 0.f);           // miss (outside quad)
	mk(3,  0.5f, -0.5f, 0.f, 0.f, RAY_FLAGS_MASKED); // masked -> untouched
	mk(4,  0.0f, -0.9f, 0.f, 0.f);           // near bottom edge, tri 0
	mk(5, -0.9f,  0.9f, 0.f, 0.f);           // near top-left, tri 1
	mk(6,  0.0f,  0.0f, 0.4f, 0.2f);         // angled ray, lands x>y -> tri 0
	mk(7,  0.2f, -0.4f, 0.f, 0.f);           // tri 0

	const u_int n = rays.size();
	std::vector<RayHit> gpuHits(n);
	for (auto &h : gpuHits) h.SetMiss();
	// The kernel writes nothing for masked rays: pre-fill a sentinel so a
	// spurious write would be caught.
	gpuHits[3].t = -777.f; gpuHits[3].b1 = -777.f; gpuHits[3].b2 = -777.f;
	gpuHits[3].meshIndex = 777; gpuHits[3].triangleIndex = 777;

	HardwareDeviceBuffer *rayBuff = nullptr, *hitBuff = nullptr;
	dev.AllocBufferRW(&rayBuff, rays.data(), sizeof(Ray) * n, "rays");
	dev.AllocBufferRW(&hitBuff, gpuHits.data(), sizeof(RayHit) * n, "rayHits");
	dev.EnqueueTraceRayBuffer(rayBuff, hitBuff, n);
	dev.FinishQueue();
	dev.EnqueueReadBuffer(hitBuff, true, sizeof(RayHit) * n, gpuHits.data());
	dev.FinishQueue();

	// CPU reference through the same accelerator the kernel traverses
	AcceleratorConstSPtr accel = dev.GetAccelerator();
	int fails = 0;
	for (u_int i = 0; i < n; ++i) {
		if (rays[i].flags & RAY_FLAGS_MASKED) {
			const bool ok = (gpuHits[i].meshIndex == 777);
			if (!ok) {
				std::cout << "  ray " << i << " masked but written: "
					<< gpuHits[i] << std::endl;
				++fails;
			}
			continue;
		}
		Ray cpuRay = rays[i];
		RayHit cpuHit;
		cpuHit.SetMiss();
		accel->Intersect(&cpuRay, &cpuHit);
		const bool ok =
			(gpuHits[i].Miss() == cpuHit.Miss()) &&
			(gpuHits[i].Miss() ||
			 (std::fabs(gpuHits[i].t - cpuHit.t) < 1e-5f &&
			  gpuHits[i].meshIndex == cpuHit.meshIndex &&
			  gpuHits[i].triangleIndex == cpuHit.triangleIndex));
		if (!ok) {
			std::cout << "  ray " << i << " MISMATCH gpu=" << gpuHits[i]
				<< " cpu=" << cpuHit << std::endl;
			++fails;
		}
	}
	std::cout << (fails == 0 ? "VK_INTERSECT: PASS (" : "VK_INTERSECT: FAIL (")
		<< n - fails << "/" << n << " rays)" << std::endl;

	dev.FreeBuffer(&rayBuff);
	dev.FreeBuffer(&hitBuff);
	ctx.Stop();
	return fails == 0 ? 0 : 1;
}

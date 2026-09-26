// End-to-end Vulkan intersection test: drives the real engine path
//   Context -> VulkanIntersectionDevice -> BVHAccel::NewHardwareIntersectionKernel
//   -> vkdevice CompileProgram (clspv -> SPIR-V -> reflection -> descriptors)
//   -> EnqueueTraceRayBuffer on GPU, results checked against the CPU BVH
//   accelerator (accel->Intersect) ray by ray.
//
// On RT-capable devices the same binary exercises the HWRT path instead:
//   VulkanIntersectionDevice::BuildRTAccel -> BLAS/TLAS build
//   -> GL_EXT_ray_query compute shader (glslang -> SPIR-V)
// LUXRAYS_VULKAN_RT=0 forces the SW traversal kernel, so both paths can be
// regression-tested on the same machine.
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

	auto dataSet = std::make_shared<DataSet>(ctx);
	// DataSet stores non-owning Mesh* — keep meshes alive for the test.
	std::vector<std::unique_ptr<TriangleMesh>> meshes;

	// Mesh 0: two-triangle quad in the z=0 plane (same as the old harness)
	{
		VertexBuffer verts(4);
		verts[0] = Point(-1.f, -1.f, 0.f);
		verts[1] = Point( 1.f, -1.f, 0.f);
		verts[2] = Point( 1.f,  1.f, 0.f);
		verts[3] = Point(-1.f,  1.f, 0.f);
		TriangleBuffer tris(2);
		tris[0] = Triangle(0, 1, 2);
		tris[1] = Triangle(0, 2, 3);
		meshes.push_back(std::make_unique<TriangleMesh>(std::move(verts), std::move(tris)));
		dataSet->Add(*meshes.back());
	}
	// Mesh 1: second quad behind the first (exercises instanceCustomIndex ->
	// meshIndex mapping on the HWRT path)
	{
		VertexBuffer verts(4);
		verts[0] = Point(-2.f, -2.f, -3.f);
		verts[1] = Point( 2.f, -2.f, -3.f);
		verts[2] = Point( 2.f,  2.f, -3.f);
		verts[3] = Point(-2.f,  2.f, -3.f);
		TriangleBuffer tris(2);
		tris[0] = Triangle(0, 1, 2);
		tris[1] = Triangle(0, 2, 3);
		meshes.push_back(std::make_unique<TriangleMesh>(std::move(verts), std::move(tris)));
		dataSet->Add(*meshes.back());
	}
	// Mesh 2: 10x10 tessellated plane at z=+2 (200 triangles, stresses
	// primitiveIndex -> triangleIndex mapping)
	{
		const int N = 10;
		VertexBuffer verts((N + 1) * (N + 1));
		for (int y = 0; y <= N; ++y)
			for (int x = 0; x <= N; ++x)
				verts[y * (N + 1) + x] = Point(
						-1.f + 2.f * x / N, -1.f + 2.f * y / N, 2.f);
		TriangleBuffer tris(2 * N * N);
		for (int y = 0; y < N; ++y)
			for (int x = 0; x < N; ++x) {
				const int q = 2 * (y * N + x);
				const u_int a = y * (N + 1) + x;
				tris[q]     = Triangle(a, a + 1, a + N + 2);
				tris[q + 1] = Triangle(a, a + N + 2, a + N + 1);
			}
		meshes.push_back(std::make_unique<TriangleMesh>(std::move(verts), std::move(tris)));
		dataSet->Add(*meshes.back());
	}

	dataSet->SetAcceleratorType(ACCEL_BVH);
	dataSet->Preprocess();
	ctx.SetDataSet(dataSet);
	ctx.Start();

	auto &dev = dynamic_cast<HardwareIntersectionDeviceRef>(idevices[0].get());

	// Rays: per-triangle hits on every mesh, misses, masked, mint/maxt
	// range culling, and angled rays landing on specific grid cells.
	std::vector<Ray> rays;
	auto mk = [&](float ox, float oy, float oz, float dx, float dy, float dz,
			unsigned flags = RAY_FLAGS_NONE, float mint = 0.f,
			float maxt = INFINITY) {
		rays.push_back(Ray(Point(ox, oy, oz), Vector(dx, dy, dz)));
		rays.back().flags = flags;
		rays.back().mint = mint;
		rays.back().maxt = maxt;
	};
	// NOTE: keep hit points strictly inside one triangle — the shared
	// quad diagonal (x == y) is claimed by both leaves and either answer
	// is valid.
	mk( 0.5f, -0.5f,  1.f, 0.f, 0.f, -1.f);          // mesh 0, tri 0 (x > y)
	mk(-0.5f,  0.5f,  1.f, 0.f, 0.f, -1.f);          // mesh 0, tri 1 (x < y)
	mk( 5.0f,  5.0f,  1.f, 0.f, 0.f, -1.f);          // miss (outside all)
	mk( 0.5f, -0.5f,  1.f, 0.f, 0.f, -1.f,
			RAY_FLAGS_MASKED);                       // masked -> untouched
	mk( 0.0f, -0.9f,  1.f, 0.f, 0.f, -1.f);          // mesh 0 near bottom edge
	mk(-0.9f,  0.9f,  1.f, 0.f, 0.f, -1.f);          // mesh 0 near top-left
	mk( 0.0f,  0.0f,  1.f, 0.4f, 0.2f, -1.f);        // angled, lands x>y
	mk( 0.2f, -0.4f,  1.f, 0.f, 0.f, -1.f);          // mesh 0, tri 0
	mk( 1.5f, -1.5f,  1.f, 0.f, 0.f, -1.f);          // through quad gap -> mesh 1
	mk(-1.5f,  1.5f,  1.f, 0.f, 0.f, -1.f);          // mesh 1
	mk( 0.53f, -0.5f,  3.f, 0.f, 0.f, -1.f);         // mesh 2 grid cell (off-diag)
	mk(-0.75f, 0.28f, 3.f, 0.f, 0.f, -1.f);          // mesh 2 other cell (off-diag)
	mk( 0.5f, -0.5f,  1.f, 0.f, 0.f, -1.f,
			RAY_FLAGS_NONE, 0.f, 0.5f);              // maxt short -> miss
	mk( 0.5f, -0.5f,  1.f, 0.f, 0.f, -1.f,
			RAY_FLAGS_NONE, 1.5f);                   // mint past mesh 0 -> mesh 1
	mk( 0.5f, -0.5f,  1.f, 0.f, 0.f, -1.f,
			RAY_FLAGS_MASKED);                       // second masked ray

	const u_int n = rays.size();
	std::vector<RayHit> gpuHits(n);
	for (auto &h : gpuHits) h.SetMiss();
	// The kernels write nothing for masked rays: pre-fill a sentinel so a
	// spurious write would be caught.
	for (u_int i = 0; i < n; ++i)
		if (rays[i].flags & RAY_FLAGS_MASKED) {
			gpuHits[i].t = -777.f;
			gpuHits[i].b1 = -777.f; gpuHits[i].b2 = -777.f;
			gpuHits[i].meshIndex = 777; gpuHits[i].triangleIndex = 777;
		}

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
			  std::fabs(gpuHits[i].b1 - cpuHit.b1) < 1e-5f &&
			  std::fabs(gpuHits[i].b2 - cpuHit.b2) < 1e-5f &&
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

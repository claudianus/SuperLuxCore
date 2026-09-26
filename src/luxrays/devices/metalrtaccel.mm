/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// ObjC imports FIRST (see metaldevice.mm for the reason)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)

#include "luxrays/devices/metalrtaccel.h"
#include "luxrays/devices/metalintersectiondevice.h"
#include "luxrays/devices/metaldevice.h"
#include "luxrays/accelerators/mbvhaccel.h"
#include "luxrays/core/context.h"
#include "luxrays/core/geometry/transform.h"
#include "luxrays/core/geometry/matrix4x4.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/usings.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <vector>

using namespace std;

namespace luxrays {

// Native Metal ray tracing kernel, byte-compatible with the software
// Accelerator_Intersect_RayBuffer:
//   buffer(0) -> Ray buffer    (luxrays::ocl::Ray layout, 48B)
//   buffer(1) -> RayHit buffer (luxrays::ocl::RayHit layout, 20B)
//   buffer(2) -> rayCount scalar
//   buffer(3) -> the instance acceleration structure
//
// Result semantics identical to the software MBVH kernel:
//   hit.meshIndex     = MBVH leaf reference index (== scene object index)
//   hit.triangleIndex = triangle index inside the leaf mesh
//   miss              = meshIndex == 0xffffffff
// Masked rays (RAY_FLAGS_MASKED) are skipped exactly like the OpenCL kernel.
static const char *HWRT_MSL_SOURCE = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;

struct LuxRay {
	float ox, oy, oz;
	float dx, dy, dz;
	float mint, maxt, time;
	uint flags;
	float pad0, pad1;
};

struct LuxRayHit {
	float t, b1, b2;
	uint meshIndex, triangleIndex;
};

kernel void Accelerator_Intersect_RayBuffer_HWRT(
		device const LuxRay *rays [[buffer(0)]],
		device LuxRayHit *rayHits [[buffer(1)]],
		constant uint &rayCount [[buffer(2)]],
		raytracing::instance_acceleration_structure sceneAS [[buffer(3)]],
		constant uint &useMotionTime [[buffer(4)]],
		uint gid [[thread_position_in_grid]]) {
	if (gid >= rayCount)
		return;

	const LuxRay r = rays[gid];
	// RAY_FLAGS_MASKED
	if (r.flags & 0x1u)
		return;

	raytracing::ray ray;
	ray.origin = float3(r.ox, r.oy, r.oz);
	ray.direction = float3(r.dx, r.dy, r.dz);
	ray.min_distance = r.mint;
	ray.max_distance = r.maxt;

	raytracing::intersector<raytracing::instancing, raytracing::triangle_data> itr;
	// r.time drives Metal's motion instance interpolation when the instance
	// acceleration structure was built with motion descriptors. On a static
	// (non-motion) instance AS the timed overload returns no intersection
	// for every ray, so useMotionTime must gate it.
	const auto hit = useMotionTime ?
		itr.intersect(ray, sceneAS, r.time) : itr.intersect(ray, sceneAS);

	if (hit.type == raytracing::intersection_type::none) {
		// Match the software kernel's miss record exactly:
		// t = ray->maxt, meshIndex = NULL_INDEX, triangleIndex = NULL_INDEX.
		rayHits[gid].t = r.maxt;
		rayHits[gid].meshIndex = 0xffffffffu;
		rayHits[gid].triangleIndex = 0xffffffffu;
		return;
	}

	rayHits[gid].t = hit.distance;
	rayHits[gid].b1 = hit.triangle_barycentric_coord.x;
	rayHits[gid].b2 = hit.triangle_barycentric_coord.y;
	// instance_id is the userID of the instance descriptor = MBVH leaf
	// reference index = the meshIndex the software kernel produces.
	rayHits[gid].meshIndex = hit.instance_id;
	rayHits[gid].triangleIndex = hit.primitive_id;
}
)MSL";

// Curve-capable variant of the same kernel (Metal 3.1 / macOS 14+):
// identical triangle handling, plus curve-primitive hits encoded for the
// shading side as
//   hit.triangleIndex = RAYHIT_CURVE_FLAG | mesh-local segment index
//   hit.b1            = curve parameter u along the segment
//   hit.b2            = 0
// (dev-tools/metal_curve_design.md). The flag constant is inlined because
// this source never passes through the cl2msl path.
static const char *HWRT_MSL_SOURCE_CURVES = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;

struct LuxRay {
	float ox, oy, oz;
	float dx, dy, dz;
	float mint, maxt, time;
	uint flags;
	float pad0, pad1;
};

struct LuxRayHit {
	float t, b1, b2;
	uint meshIndex, triangleIndex;
};

kernel void Accelerator_Intersect_RayBuffer_HWRT(
		device const LuxRay *rays [[buffer(0)]],
		device LuxRayHit *rayHits [[buffer(1)]],
		constant uint &rayCount [[buffer(2)]],
		raytracing::instance_acceleration_structure sceneAS [[buffer(3)]],
		constant uint &useMotionTime [[buffer(4)]],
		uint gid [[thread_position_in_grid]]) {
	if (gid >= rayCount)
		return;

	const LuxRay r = rays[gid];
	// RAY_FLAGS_MASKED
	if (r.flags & 0x1u)
		return;

	raytracing::ray ray;
	ray.origin = float3(r.ox, r.oy, r.oz);
	ray.direction = float3(r.dx, r.dy, r.dz);
	ray.min_distance = r.mint;
	ray.max_distance = r.maxt;

	raytracing::intersector<raytracing::instancing, raytracing::triangle_data,
			raytracing::curve_data> itr;
	// On a static (non-motion) instance AS the timed overload returns no
	// intersection for every ray, so useMotionTime must gate it.
	const auto hit = useMotionTime ?
		itr.intersect(ray, sceneAS, r.time) : itr.intersect(ray, sceneAS);

	if (hit.type == raytracing::intersection_type::none) {
		rayHits[gid].t = r.maxt;
		rayHits[gid].meshIndex = 0xffffffffu;
		rayHits[gid].triangleIndex = 0xffffffffu;
		return;
	}

	rayHits[gid].t = hit.distance;
	// instance_id is the userID of the instance descriptor = MBVH leaf
	// reference index = the meshIndex the software kernel produces.
	rayHits[gid].meshIndex = hit.instance_id;

	if (hit.type == raytracing::intersection_type::curve) {
		// Curve segment hit: segment-local u in b1, flagged segment index.
		rayHits[gid].b1 = hit.curve_parameter;
		rayHits[gid].b2 = 0.f;
		rayHits[gid].triangleIndex = 0x80000000u | hit.primitive_id;
	} else {
		rayHits[gid].b1 = hit.triangle_barycentric_coord.x;
		rayHits[gid].b2 = hit.triangle_barycentric_coord.y;
		rayHits[gid].triangleIndex = hit.primitive_id;
	}
}
)MSL";

class MetalRTKernel : public HardwareIntersectionKernel {
public:
	MetalRTKernel(HardwareIntersectionDevice &dev, const MBVHAccel &acc);
	virtual ~MetalRTKernel() {
		FreeAccelerationStructures();
		[primRetainBag release];
		[instRetainBag release];
		[pso release];
	}

	virtual void Update(DataSetConstSPtr newDataSet) override;
	virtual void EnqueueTraceRayBuffer(HardwareDeviceBuffer *rayBuff,
			HardwareDeviceBuffer *rayHitBuff, const unsigned int rayCount) override;

	// Static support check: can this device + accelerator go through the
	// native Metal HWRT path? (logs the reason when returning false)
	static bool IsSupported(HardwareIntersectionDevice &dev, const MBVHAccel &acc);

private:
	void FreeInstanceStructures();
	void FreeAccelerationStructures();
	void BuildPrimitiveStructures();
	void BuildInstanceStructure();
	void BuildAccelerationStructures();

	MetalIntersectionDevice &mdev;
	const MBVHAccel &mbvh;

	// Native curve primitives: enabled only when the OS/API supports curve
	// acceleration structures (macOS 14+) and LUXRAYS_METAL_CURVES != 0.
	// Meshes without curve data always use the triangle path regardless.
	bool useCurveData;

	// True when the instance acceleration structure was built with motion
	// instance descriptors (any leaf carries a motion system). The MSL
	// kernel must only pass the ray time to intersect() when this is set:
	// on a static (non-motion) instance AS the timed overload returns no
	// intersection for every ray.
	bool instanceASIsMotion;

	id<MTLDevice> mtlDev;
	id<MTLCommandQueue> queue;

	id<MTLComputePipelineState> pso;
	NSUInteger workGroupSize;

	// GPU resources (all +1 retained, released in FreeAccelerationStructures)
	id<MTLAccelerationStructure> instanceAS;
	std::vector<id<MTLAccelerationStructure>> primitiveAS;
	// Vertex/index/scratch buffers feeding the primitive acceleration
	// structures. Mesh geometry is invariant across DataSet updates (only
	// transforms/motion change), so these persist for the kernel lifetime.
	std::vector<id<MTLBuffer>> ownedPrimBuffers;
	// Instance-descriptor/motion/scratch buffers feeding the instance
	// acceleration structure. Rebuilt on every Update().
	std::vector<id<MTLBuffer>> ownedInstBuffers;

	// Retain bags for autoreleased ObjC objects handed to AS-build
	// descriptors/encoders (geometry descriptors, descriptor objects,
	// geometry/instance arrays). Committed command buffers may still
	// reference them at execution/completion time - releasing them when
	// the encoding autoreleasepool drains crashes Metal's completion
	// queue with an over-release. Split by lifetime: primitive descriptors
	// persist, instance descriptors are dropped on each Update().
	NSMutableArray *primRetainBag;
	NSMutableArray *instRetainBag;
};

//------------------------------------------------------------------------------
// Support check
//------------------------------------------------------------------------------

bool MetalRTKernel::IsSupported(HardwareIntersectionDevice &dev, const MBVHAccel &acc) {
	const char *env = getenv("LUXRAYS_METAL_HWRT");
	if (env && string(env) == "0") {
		LR_LOG(dev.GetContext(), "Metal HWRT disabled by LUXRAYS_METAL_HWRT=0");
		return false;
	}

	MetalIntersectionDevice *mdev = dynamic_cast<MetalIntersectionDevice *>(&dev);
	if (!mdev)
		return false;

	id<MTLDevice> mtlDev = (__bridge id<MTLDevice>)mdev->GetMTLDevice();
	if (![mtlDev supportsRaytracing]) {
		LR_LOG(dev.GetContext(), "Metal HWRT unavailable: device does not support ray tracing");
		return false;
	}

	if (!acc.initialized || acc.nRootNodes == 0)
		return false;

	// Scan the MBVH leaf references for anything HWRT can't express yet
	for (size_t k = 0; k < acc.uniqueLeafs.size(); ++k) {
		if (acc.uniqueLeafs[k]->GetMeshes().size() != 1) {
			LR_LOG(dev.GetContext(), "Metal HWRT unavailable: multi-mesh leaf present "
					"(software MBVH fallback)");
			return false;
		}
		if (acc.uniqueLeafs[k]->GetMeshes()[0]->GetTotalTriangleCount() == 0) {
			LR_LOG(dev.GetContext(), "Metal HWRT unavailable: empty mesh leaf "
					"(software MBVH fallback)");
			return false;
		}
	}

	return true;
}

//------------------------------------------------------------------------------
// Constructor: compile the native MSL kernel, then build the AS tree
//------------------------------------------------------------------------------

MetalRTKernel::MetalRTKernel(HardwareIntersectionDevice &dev, const MBVHAccel &acc) :
		HardwareIntersectionKernel(dev), mdev(dynamic_cast<MetalIntersectionDevice &>(dev)),
		mbvh(acc), pso(nil), workGroupSize(64), instanceAS(nil),
		instanceASIsMotion(false) {
	@autoreleasepool {
		mtlDev = (__bridge id<MTLDevice>)mdev.GetMTLDevice();
		queue = (__bridge id<MTLCommandQueue>)mdev.GetMTLCommandQueue();

		// Native curve primitives (dev-tools/metal_curve_design.md) require
		// MTLAccelerationStructureCurveGeometryDescriptor (macOS 14+) and the
		// curve_data intersector tag (Metal 3.1). LUXRAYS_METAL_CURVES=0
		// forces the triangle-tessellation path (previous behavior, also the
		// reference for CPU/GPU parity testing).
		const char *curveEnv = getenv("LUXRAYS_METAL_CURVES");
		useCurveData = !(curveEnv && string(curveEnv) == "0");
		if (useCurveData) {
			if (@available(macOS 14.0, *))
				; // supported below by the API availability check
			else
				useCurveData = false;
		}

		// Compile the native MSL kernel (never goes through cl2msl)
		MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
		NSError *err = nil;
		id<MTLLibrary> lib = nil;
		if (useCurveData) {
			opts.languageVersion = MTLLanguageVersion3_1;
			lib = [mtlDev newLibraryWithSource:
					[NSString stringWithUTF8String:HWRT_MSL_SOURCE_CURVES]
					options:opts error:&err];
			if (!lib) {
				// Older Metal toolchains reject the curve kernel: fall back
				// to the triangle-only kernel rather than losing HWRT.
				LR_LOG(dev.GetContext(), "Metal HWRT curve kernel unavailable ("
						<< (err ? err.localizedDescription.UTF8String : "?")
						<< "): falling back to triangle-only kernel");
				useCurveData = false;
				err = nil;
			}
		}
		if (!useCurveData) {
			opts.languageVersion = MTLLanguageVersion2_3;
			lib = [mtlDev newLibraryWithSource:
					[NSString stringWithUTF8String:HWRT_MSL_SOURCE]
					options:opts error:&err];
		}
		[opts release];
		if (!lib)
			throw runtime_error(string("Metal HWRT kernel compile error: ") +
					(err ? err.localizedDescription.UTF8String : "?"));

		id<MTLFunction> fn = [lib newFunctionWithName:@"Accelerator_Intersect_RayBuffer_HWRT"];
		[lib release];
		if (!fn)
			throw runtime_error("Metal HWRT kernel function not found");

		pso = [mtlDev newComputePipelineStateWithFunction:fn error:&err];
		[fn release];
		if (!pso)
			throw runtime_error(string("Metal HWRT pipeline error: ") +
					(err ? err.localizedDescription.UTF8String : "?"));

		workGroupSize = MIN((NSUInteger)256, pso.maxTotalThreadsPerThreadgroup);

		primRetainBag = [[NSMutableArray alloc] init];
		instRetainBag = [[NSMutableArray alloc] init];
		BuildAccelerationStructures();

		LR_LOG(dev.GetContext(), "Metal HWRT: native MTLAccelerationStructure path active ("
				<< primitiveAS.size() << " primitive AS, "
				<< mbvh.bvhLeafs.size() << " instances)");
	}
}

//------------------------------------------------------------------------------
// AS construction
//------------------------------------------------------------------------------

// Releases only the instance-level resources (instance AS + its descriptor,
// motion and scratch buffers). Called on every Update(): the primitive
// structures and their geometry buffers are left untouched.
void MetalRTKernel::FreeInstanceStructures() {
	if (instanceAS) { [instanceAS release]; instanceAS = nil; }
	for (auto b : ownedInstBuffers) [b release];
	ownedInstBuffers.clear();
	[instRetainBag release];
	instRetainBag = [[NSMutableArray alloc] init];
}

void MetalRTKernel::FreeAccelerationStructures() {
	FreeInstanceStructures();
	for (auto a : primitiveAS) [a release];
	primitiveAS.clear();
	for (auto b : ownedPrimBuffers) [b release];
	ownedPrimBuffers.clear();
	[primRetainBag release];
	primRetainBag = [[NSMutableArray alloc] init];
}

void MetalRTKernel::BuildAccelerationStructures() {
	FreeAccelerationStructures();
	BuildPrimitiveStructures();
	BuildInstanceStructure();
}

void MetalRTKernel::BuildPrimitiveStructures() {
	@autoreleasepool {
		//------------------------------------------------------------------
		// 1) One primitive (triangle) acceleration structure per unique leaf
		//------------------------------------------------------------------
		primitiveAS.reserve(mbvh.uniqueLeafs.size());

		// All primitive builds are encoded on one command buffer; each gets
		// its own scratch buffer so encoding order stays trivially correct.
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> enc =
				[cb accelerationStructureCommandEncoder];

		for (size_t k = 0; k < mbvh.uniqueLeafs.size(); ++k) {
			const Mesh *mesh = mbvh.uniqueLeafs[k]->GetMeshes()[0];

			// Native curve primitives (dev-tools/metal_curve_design.md):
			// strands meshes carry Catmull-Rom curve data next to their
			// triangle tessellation. Plain and instanced ExtTriangleMesh
			// leaves can use it (the instance transform is applied at the
			// instance AS level, same as for triangles); motion leaves keep
			// the triangle path (curve data has no deformation keyframes).
			// Mesh is a virtual base, so resolving needs dynamic_cast.
			const ExtTriangleMesh *curveMesh = nullptr;
			if (useCurveData) {
				if (const ExtInstanceTriangleMesh *imesh =
						dynamic_cast<const ExtInstanceTriangleMesh *>(mesh)) {
					curveMesh = &imesh->GetExtTriangleMesh();
				} else {
					curveMesh = dynamic_cast<const ExtTriangleMesh *>(mesh);
				}
				if (curveMesh && !curveMesh->HasCurveData())
					curveMesh = nullptr;
			}

			MTLAccelerationStructureGeometryDescriptor *geo = nil;

			if (curveMesh) {
				const auto &cps = curveMesh->GetCurveCps();
				const auto &segs = curveMesh->GetCurveSegIndices();

				// One float4 (xyz + radius) per control point: the radius
				// view aliases the same buffer at offset .w.
				id<MTLBuffer> cpBuf = [mtlDev newBufferWithBytes:cps.data()
						length:cps.size() * sizeof(CurveControlPoint)
						options:MTLResourceStorageModeShared];
				id<MTLBuffer> idxBuf = [mtlDev newBufferWithBytes:segs.data()
						length:segs.size() * sizeof(u_int)
						options:MTLResourceStorageModeShared];
				ownedPrimBuffers.push_back(cpBuf);
				ownedPrimBuffers.push_back(idxBuf);

				if (@available(macOS 14.0, *)) {
					MTLAccelerationStructureCurveGeometryDescriptor *cgeo =
							[MTLAccelerationStructureCurveGeometryDescriptor descriptor];
					cgeo.controlPointBuffer = cpBuf;
					cgeo.controlPointBufferOffset = 0;
					cgeo.controlPointCount = cps.size();
					cgeo.controlPointFormat = MTLAttributeFormatFloat3;
					cgeo.controlPointStride = sizeof(CurveControlPoint);
					cgeo.radiusBuffer = cpBuf;
					cgeo.radiusBufferOffset = offsetof(CurveControlPoint, radius);
					cgeo.radiusFormat = MTLAttributeFormatFloat;
					cgeo.radiusStride = sizeof(CurveControlPoint);
					cgeo.indexBuffer = idxBuf;
					cgeo.indexBufferOffset = 0;
					cgeo.indexType = MTLIndexTypeUInt32;
					cgeo.segmentCount = segs.size();
					cgeo.segmentControlPointCount = 4;
					cgeo.curveType = MTLCurveTypeRound;
					cgeo.curveBasis = MTLCurveBasisCatmullRom;
					cgeo.curveEndCaps = MTLCurveEndCapsNone;
					cgeo.opaque = YES;
					cgeo.allowDuplicateIntersectionFunctionInvocation = NO;
					geo = cgeo;
				}
			}

			if (!geo) {
				const std::span<Point> verts = mesh->GetVertices();
				const std::span<Triangle> tris = mesh->GetTriangles();

				id<MTLBuffer> vbuf = [mtlDev newBufferWithBytes:verts.data()
						length:verts.size() * sizeof(Point)
						options:MTLResourceStorageModeShared];
				id<MTLBuffer> ibuf = [mtlDev newBufferWithBytes:tris.data()
						length:tris.size() * sizeof(Triangle)
						options:MTLResourceStorageModeShared];
				ownedPrimBuffers.push_back(vbuf);
				ownedPrimBuffers.push_back(ibuf);

				MTLAccelerationStructureTriangleGeometryDescriptor *tgeo =
						[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
				tgeo.vertexBuffer = vbuf;
				tgeo.vertexBufferOffset = 0;
				tgeo.vertexStride = sizeof(Point);
				tgeo.vertexFormat = MTLAttributeFormatFloat3;
				tgeo.indexBuffer = ibuf;
				tgeo.indexBufferOffset = 0;
				tgeo.indexType = MTLIndexTypeUInt32;
				tgeo.triangleCount = tris.size();
				// All triangles are opaque: alpha/cutout is resolved by the
				// shading code (HITPOINTALPHA), never at intersection time.
				tgeo.opaque = YES;
				tgeo.allowDuplicateIntersectionFunctionInvocation = NO;
				geo = tgeo;
			}
			[primRetainBag addObject:geo];

			MTLPrimitiveAccelerationStructureDescriptor *primDesc =
					[MTLPrimitiveAccelerationStructureDescriptor descriptor];
			NSArray *geos = @[geo];
			primDesc.geometryDescriptors = geos;
			[primRetainBag addObject:primDesc];
			[primRetainBag addObject:geos];

			const MTLAccelerationStructureSizes sizes =
					[mtlDev accelerationStructureSizesWithDescriptor:primDesc];
			id<MTLAccelerationStructure> pas =
					[mtlDev newAccelerationStructureWithSize:sizes.accelerationStructureSize];
			id<MTLBuffer> scratch = [mtlDev newBufferWithLength:sizes.buildScratchBufferSize
					options:MTLResourceStorageModePrivate];
			ownedPrimBuffers.push_back(scratch);

			[enc buildAccelerationStructure:pas descriptor:primDesc
					scratchBuffer:scratch scratchBufferOffset:0];

			primitiveAS.push_back(pas);
		}
		[enc endEncoding];
		// Track so FinishQueue() covers AS builds as well
		mdev.CommitAndTrackInFlight((__bridge MTLCommandBufferHandle)cb, {});
		// Synchronous error check: a failed primitive build otherwise
		// surfaces only as silently-missing geometry.
		[cb waitUntilCompleted];
		if (cb.error)
			LR_LOG(device.GetContext(), "Metal HWRT primitive AS build error: "
					<< cb.error.localizedDescription.UTF8String);
	}
}

void MetalRTKernel::BuildInstanceStructure() {
	@autoreleasepool {
		//------------------------------------------------------------------
		// 2) Instance descriptors (one per MBVH leaf reference)
		//------------------------------------------------------------------
		// instancedAccelerationStructures takes the primitive structures as
		// an NSArray; wrap primitiveAS (unchanged across Update()).
		NSMutableArray<id<MTLAccelerationStructure>> *primArray =
				[NSMutableArray arrayWithCapacity:primitiveAS.size()];
		for (auto pas : primitiveAS)
			[primArray addObject:pas];
		[instRetainBag addObject:primArray];

		const size_t nLeafs = mbvh.bvhLeafs.size();

		// LuxRays row-major 4x4 -> Metal column-major 4x3
		// (columns[c] = first 3 rows of column c; column 3 = translation)
		auto toPacked = [](const Matrix4x4 &m) -> MTLPackedFloat4x3 {
			MTLPackedFloat4x3 t;
			for (int c = 0; c < 4; ++c)
				t.columns[c] = MTLPackedFloat3Make(m.m[0][c], m.m[1][c], m.m[2][c]);
			return t;
		};
		auto identityTransform = []() -> MTLPackedFloat4x3 {
			MTLPackedFloat4x3 t;
			for (int c = 0; c < 3; ++c)
				t.columns[c] = MTLPackedFloat3Make(c == 0 ? 1.f : 0.f,
						c == 1 ? 1.f : 0.f, c == 2 ? 1.f : 0.f);
			t.columns[3] = MTLPackedFloat3Make(0.f, 0.f, 0.f);
			return t;
		};
		// Static local->world transform of a leaf (instanced) or identity.
		auto leafStaticTransform = [&](const size_t i) -> MTLPackedFloat4x3 {
			const u_int tIndex = mbvh.bvhLeafs[i].bvhLeaf.transformIndex;
			return (tIndex != NULL_INDEX) ?
					toPacked(mbvh.uniqueLeafsTransform[tIndex]->m) : identityTransform();
		};

		// Does any leaf carry a motion system? If so the whole instance AS must
		// use MTLAccelerationStructureMotionInstanceDescriptor (the descriptor
		// type is uniform across the instance buffer).
		bool hasMotion = false;
		for (size_t i = 0; i < nLeafs; ++i)
			if (mbvh.bvhLeafs[i].bvhLeaf.motionIndex != NULL_INDEX) {
				hasMotion = true;
				break;
			}
		instanceASIsMotion = hasMotion;

		MTLInstanceAccelerationStructureDescriptor *instDesc =
				[MTLInstanceAccelerationStructureDescriptor descriptor];
		instDesc.instancedAccelerationStructures = primArray;
		instDesc.instanceCount = nLeafs;

		if (!hasMotion) {
			std::vector<MTLAccelerationStructureUserIDInstanceDescriptor> inst(nLeafs);
			for (size_t i = 0; i < nLeafs; ++i) {
				const BVHTreeNode &leaf = mbvh.bvhLeafs[i];

				inst[i].transformationMatrix = leafStaticTransform(i);
				inst[i].options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
				inst[i].mask = 0xFFFFFFFF;
				inst[i].intersectionFunctionTableOffset = 0;
				inst[i].accelerationStructureIndex = (uint32_t)leaf.bvhLeaf.leafIndex;
				// userID -> hit.instance_id -> rayHit.meshIndex
				inst[i].userID = (uint32_t)i;
			}

			id<MTLBuffer> instBuf = [mtlDev newBufferWithBytes:inst.data()
					length:inst.size() * sizeof(inst[0])
					options:MTLResourceStorageModeShared];
			ownedInstBuffers.push_back(instBuf);

			instDesc.instanceDescriptorType =
					MTLAccelerationStructureInstanceDescriptorTypeUserID;
			instDesc.instanceDescriptorBuffer = instBuf;
			instDesc.instanceDescriptorStride = sizeof(inst[0]);
		} else {
			std::vector<MTLAccelerationStructureMotionInstanceDescriptor> inst(nLeafs);
			// All instance keyframe transforms, laid out back to back; each
			// instance references a contiguous run via
			// motionTransformsStartIndex/Count.
			std::vector<MTLPackedFloat4x3> motionTransforms;

			for (size_t i = 0; i < nLeafs; ++i) {
				const BVHTreeNode &leaf = mbvh.bvhLeafs[i];

				inst[i].options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
				inst[i].mask = 0xFFFFFFFF;
				inst[i].intersectionFunctionTableOffset = 0;
				inst[i].accelerationStructureIndex = (uint32_t)leaf.bvhLeaf.leafIndex;
				inst[i].userID = (uint32_t)i;
				inst[i].motionStartBorderMode = MTLMotionBorderModeClamp;
				inst[i].motionEndBorderMode = MTLMotionBorderModeClamp;
				inst[i].motionTransformsStartIndex = (uint32_t)motionTransforms.size();

				const u_int motionIndex = leaf.bvhLeaf.motionIndex;
				if (motionIndex != NULL_INDEX) {
					const MotionSystem *msys = mbvh.uniqueLeafsMotionSystem[motionIndex];
					const float t0 = msys->StartTime();
					const float t1 = msys->EndTime();
					// Metal distributes keyframe transforms uniformly over
					// [motionStartTime, motionEndTime], so sample the
					// local->world transform (interpolatedInverseTransforms ==
					// SampleInverse) at uniform times. A single keyframe makes
					// the instance static.
					const uint32_t count = msys->IsStatic() ? 1u :
							(uint32_t)std::max<size_t>(2, msys->times.size());
					for (uint32_t k = 0; k < count; ++k) {
						const float t = (count == 1) ? t0 :
								t0 + (t1 - t0) * (float)k / (float)(count - 1);
						motionTransforms.push_back(toPacked(msys->SampleInverse(t)));
					}
					inst[i].motionTransformsCount = count;
					inst[i].motionStartTime = t0;
					inst[i].motionEndTime = t1;
				} else {
					// Static leaf inside a motion instance array: a single
					// keyframe makes it time-invariant.
					motionTransforms.push_back(leafStaticTransform(i));
					inst[i].motionTransformsCount = 1;
					inst[i].motionStartTime = 0.f;
					inst[i].motionEndTime = 0.f;
				}
			}

			id<MTLBuffer> instBuf = [mtlDev newBufferWithBytes:inst.data()
					length:inst.size() * sizeof(inst[0])
					options:MTLResourceStorageModeShared];
			ownedInstBuffers.push_back(instBuf);
			id<MTLBuffer> motionBuf = [mtlDev newBufferWithBytes:motionTransforms.data()
					length:motionTransforms.size() * sizeof(motionTransforms[0])
					options:MTLResourceStorageModeShared];
			ownedInstBuffers.push_back(motionBuf);

			instDesc.instanceDescriptorType =
					MTLAccelerationStructureInstanceDescriptorTypeMotion;
			instDesc.instanceDescriptorBuffer = instBuf;
			instDesc.instanceDescriptorStride = sizeof(inst[0]);
			instDesc.motionTransformBuffer = motionBuf;
			instDesc.motionTransformBufferOffset = 0;
			instDesc.motionTransformCount = motionTransforms.size();
		}
		instDesc.instanceDescriptorBufferOffset = 0;
		[instRetainBag addObject:instDesc];

		const MTLAccelerationStructureSizes sizes =
				[mtlDev accelerationStructureSizesWithDescriptor:instDesc];
		instanceAS = [mtlDev newAccelerationStructureWithSize:sizes.accelerationStructureSize];
		id<MTLBuffer> scratch = [mtlDev newBufferWithLength:sizes.buildScratchBufferSize
				options:MTLResourceStorageModePrivate];
		ownedInstBuffers.push_back(scratch);

		id<MTLCommandBuffer> cb2 = [queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> enc2 =
				[cb2 accelerationStructureCommandEncoder];
		[enc2 buildAccelerationStructure:instanceAS descriptor:instDesc
				scratchBuffer:scratch scratchBufferOffset:0];
		[enc2 endEncoding];
		mdev.CommitAndTrackInFlight((__bridge MTLCommandBufferHandle)cb2, {});
		[cb2 waitUntilCompleted];
		if (cb2.error)
			LR_LOG(device.GetContext(), "Metal HWRT instance AS build error: "
					<< cb2.error.localizedDescription.UTF8String);
	}
}

//------------------------------------------------------------------------------
// HardwareIntersectionKernel interface
//------------------------------------------------------------------------------

void MetalRTKernel::Update(DataSetConstSPtr newDataSet) {
	// MBVHAccel::Update() only refreshes leaf bounds and the root tree: the
	// mesh geometry behind uniqueLeafs is invariant, only the transforms and
	// motion keyframes (reached through persistent pointers) can change. The
	// primitive acceleration structures therefore stay valid - rebuild just
	// the instance descriptors/AS instead of re-baking every triangle AS.
	FreeInstanceStructures();
	BuildInstanceStructure();
}

void MetalRTKernel::EnqueueTraceRayBuffer(HardwareDeviceBuffer *rayBuff,
		HardwareDeviceBuffer *rayHitBuff, const unsigned int rayCount) {
	if (rayCount == 0)
		return;

	@autoreleasepool {
		const MetalDeviceBuffer *mRayBuff =
				dynamic_cast<const MetalDeviceBuffer *>(rayBuff);
		const MetalDeviceBuffer *mHitBuff =
				dynamic_cast<const MetalDeviceBuffer *>(rayHitBuff);
		assert(mRayBuff && mHitBuff);

		id<MTLBuffer> rays = (__bridge id<MTLBuffer>)mRayBuff->GetMetalBuffer();
		id<MTLBuffer> hits = (__bridge id<MTLBuffer>)mHitBuff->GetMetalBuffer();

		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
		[enc setComputePipelineState:pso];
		[enc setBuffer:rays offset:0 atIndex:0];
		[enc setBuffer:hits offset:0 atIndex:1];
		u_int rc = rayCount;
		[enc setBytes:&rc length:sizeof(rc) atIndex:2];
		[enc setAccelerationStructure:instanceAS atBufferIndex:3];
		u_int motionTime = instanceASIsMotion ? 1u : 0u;
		[enc setBytes:&motionTime length:sizeof(motionTime) atIndex:4];

		const MTLSize grid = MTLSizeMake(rayCount, 1, 1);
		const MTLSize tg = MTLSizeMake(workGroupSize, 1, 1);
		[enc dispatchThreads:grid threadsPerThreadgroup:tg];
		[enc endEncoding];

		mdev.CommitAndTrackInFlight((__bridge MTLCommandBufferHandle)cb,
				{mRayBuff, mHitBuff});
	}
}

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------

HardwareIntersectionKernelUPtr NewMetalRTKernelIfPossible(
		HardwareIntersectionDevice &device, const MBVHAccel &accel) {
	if (!MetalRTKernel::IsSupported(device, accel))
		return nullptr;

	try {
		return HardwareIntersectionKernelUPtr(new MetalRTKernel(device, accel));
	} catch (const exception &e) {
		LR_LOG(device.GetContext(), "Metal HWRT init failed (" << e.what()
				<< "), falling back to software MBVH");
		return nullptr;
	}
}

}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

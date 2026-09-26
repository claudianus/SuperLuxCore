// E9 Phase-1/3 unit test: ExtTriangleMesh vertex-motion series +
// MBVHAccel swept-bound intersection
#include <cstdio>
#include <cmath>
#include <deque>
#include <vector>
#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/core/geometry/transform.h"
#include "luxrays/core/context.h"
#include "luxrays/accelerators/mbvhaccel.h"
#include "luxrays/accelerators/bvhaccel.h"
#include "luxrays/accelerators/embreeaccel.h"

using namespace luxrays;

// luxrays' Context references slg::SLG_DebugHandler through the OpenCL
// device-description path; the test links only luxrays so provide the
// (unused) definition here.
namespace slg { void (*SLG_DebugHandler)(const char *msg) = NULL; }

static int fails = 0;
#define CHECK(cond, msg) do { if(!(cond)) { printf("FAIL: %s\n", msg); ++fails; } else printf("PASS: %s\n", msg); } while(0)

static ExtTriangleMeshUPtr makeQuad(float z0, float z1) {
	VertexBuffer vs(4);
	vs[0] = Point(0,0,0); vs[1] = Point(1,0,0);
	vs[2] = Point(1,1,0); vs[3] = Point(0,1,0);
	TriangleBuffer ts(2);
	ts[0] = Triangle(0,1,2); ts[1] = Triangle(0,2,3);
	NormalBuffer ns;  // no normals

	auto m = std::make_unique<ExtTriangleMesh>(std::move(vs), std::move(ts), std::move(ns));

	std::vector<VertexBuffer> steps;
	steps.emplace_back(4); steps.emplace_back(4);
	for (u_int v = 0; v < 4; ++v) { steps[0][v] = Point(v%2, v/2, z0); }
	for (u_int v = 0; v < 4; ++v) { steps[1][v] = Point(v%2, v/2, z1); }
	m->SetVertexMotion(std::vector<float>{0.f, 1.f}, std::move(steps));
	return m;
}

int main() {
	auto m = makeQuad(0.f, 2.f);

	// basics
	CHECK(m->HasVertexMotion(), "HasVertexMotion true");
	CHECK(m->GetVertexMotionStepCount() == 2, "2 steps");
	CHECK(m->GetVertexMotionTimes().size() == 2, "2 times");

	// clamping + lerp
	Point p0 = m->GetVertexAtTime(0, -1.f);
	CHECK(fabsf(p0.z - 0.f) < 1e-6, "clamp below -> step0");
	Point p1 = m->GetVertexAtTime(0, 2.f);
	CHECK(fabsf(p1.z - 2.f) < 1e-6, "clamp above -> step1");
	Point pm = m->GetVertexAtTime(0, 0.25f);
	CHECK(fabsf(pm.z - 0.5f) < 1e-6, "lerp 0.25 -> z=0.5");

	// no motion -> static vertex
	{
		VertexBuffer vs2(1); vs2[0] = Point(9,9,9);
		TriangleBuffer ts2; // empty
		// build a valid 1-tri mesh instead
		VertexBuffer v3(3);
		v3[0]=Point(0,0,0); v3[1]=Point(1,0,0); v3[2]=Point(0,1,0);
		TriangleBuffer t3(1); t3[0]=Triangle(0,1,2);
		NormalBuffer n3;
		ExtTriangleMesh plain(std::move(v3), std::move(t3), std::move(n3));
		CHECK(!plain.HasVertexMotion(), "plain mesh no motion");
		Point ps = plain.GetVertexAtTime(1, 0.5f);
		CHECK(fabsf(ps.x-1.f)<1e-6, "no-motion -> static vertex");
	}

	// validation errors
	{
		VertexBuffer v3(3); v3[0]=Point(0,0,0); v3[1]=Point(1,0,0); v3[2]=Point(0,1,0);
		TriangleBuffer t3(1); t3[0]=Triangle(0,1,2);
		NormalBuffer n3;
		ExtTriangleMesh bad(std::move(v3), std::move(t3), std::move(n3));
		bool threw = false;
		try {
			std::vector<VertexBuffer> s2; s2.emplace_back(3); s2.emplace_back(2); // wrong count
			bad.SetVertexMotion(std::vector<float>{0.f,1.f}, std::move(s2));
		} catch (...) { threw = true; }
		CHECK(threw, "vert-count mismatch throws");
		threw = false;
		try {
			std::vector<VertexBuffer> s2; s2.emplace_back(3); s2.emplace_back(3);
			bad.SetVertexMotion(std::vector<float>{1.f,0.f}, std::move(s2)); // non-monotonic
		} catch (...) { threw = true; }
		CHECK(threw, "non-monotonic throws");
		threw = false;
		try {
			std::vector<VertexBuffer> s2; s2.emplace_back(3);
			bad.SetVertexMotion(std::vector<float>{0.f}, std::move(s2)); // <2 steps
		} catch (...) { threw = true; }
		CHECK(threw, "single step throws");
	}

	// CopyExt preserves the series (no vertex override)
	{
		auto c = m->CopyExt(std::nullopt, std::nullopt, std::nullopt,
				std::nullopt, std::nullopt, std::nullopt, 0.f);
		CHECK(c->HasVertexMotion(), "CopyExt keeps motion");
		CHECK(fabsf(c->GetVertexAtTime(0, 0.5f).z - 1.f) < 1e-6, "copy lerp z=1");
	}
	// CopyExt with overridden vertices drops the series
	{
		VertexBuffer nv(4);
		for (u_int i = 0; i < 4; ++i) nv[i] = Point(i,i,i);
		auto c = m->CopyExt(std::move(nv), std::nullopt, std::nullopt,
				std::nullopt, std::nullopt, std::nullopt, 0.f);
		CHECK(!c->HasVertexMotion(), "CopyExt drops motion on vertex override");
	}

	// ApplyTransform transforms step buffers
	{
		auto c = m->Copy();
		Matrix4x4 mat = Matrix4x4::MAT_IDENTITY;
		mat.m[0][3] = 10.f; // translate +10 x
		c->ApplyTransform(Transform(mat));
		Point p = c->GetVertexAtTime(0, 1.f);
		CHECK(fabsf(p.x - 10.f) < 1e-6 && fabsf(p.z - 2.f) < 1e-6,
				"ApplyTransform moves step verts");
	}

	// Merge: identical times -> merged series; partial presence -> throw
	{
		auto a = makeQuad(0.f, 2.f);
		auto b = makeQuad(5.f, 7.f);
		std::vector<std::reference_wrapper<const ExtTriangleMesh>> in = {*a, *b};
		auto merged = ExtTriangleMesh::Merge(in, std::nullopt);
		CHECK(merged->HasVertexMotion(), "Merge keeps motion (identical times)");
		CHECK(merged->GetTotalVertexCount() == 8, "merged 8 verts");
		CHECK(fabsf(merged->GetVertexAtTime(5, 1.f).z - 7.f) < 1e-6,
				"merged step offset correct");

		// partial presence -> throw
		VertexBuffer v3(3); v3[0]=Point(0,0,0); v3[1]=Point(1,0,0); v3[2]=Point(0,1,0);
		TriangleBuffer t3(1); t3[0]=Triangle(0,1,2);
		NormalBuffer n3;
		auto plain = std::make_unique<ExtTriangleMesh>(std::move(v3), std::move(t3), std::move(n3));
		std::vector<std::reference_wrapper<const ExtTriangleMesh>> in2 = {*a, *plain};
		bool threw = false;
		try { ExtTriangleMesh::Merge(in2, std::nullopt); } catch (...) { threw = true; }
		CHECK(threw, "Merge throws on partial motion");

		// different times -> throw
		auto c = makeQuad(0.f, 2.f);
		// rebuild c with different times
		{
			std::vector<VertexBuffer> st;
			st.emplace_back(4); st.emplace_back(4);
			for (u_int v = 0; v < 4; ++v) { st[0][v]=Point(0,0,0); st[1][v]=Point(0,0,1); }
			c->SetVertexMotion(std::vector<float>{0.f, 0.7f}, std::move(st));
		}
		std::vector<std::reference_wrapper<const ExtTriangleMesh>> in3 = {*a, *c};
		threw = false;
		try { ExtTriangleMesh::Merge(in3, std::nullopt); } catch (...) { threw = true; }
		CHECK(threw, "Merge throws on different times");
	}

	// serialization drops motion (static fallback)
	{
		m->SaveSerialized("/tmp/e9_test/motion.bpy");
		auto loaded = ExtTriangleMesh::LoadSerialized("/tmp/e9_test/motion.bpy");
		CHECK(!loaded->HasVertexMotion(), "serialized mesh drops motion (static fallback)");
		CHECK(loaded->GetTotalVertexCount() == 4, "serialized verts intact");
	}

	//------------------------------------------------------------------
	// Phase 3: swept bounding boxes
	//------------------------------------------------------------------
	{
		// Quad at x in [0,1] moving to x in [2,3]: GetBBox must cover the
		// whole sweep so no time sample can escape the bounds.
		VertexBuffer vs(4);
		vs[0] = Point(0,0,0); vs[1] = Point(1,0,0);
		vs[2] = Point(1,1,0); vs[3] = Point(0,1,0);
		TriangleBuffer ts(2);
		ts[0] = Triangle(0,1,2); ts[1] = Triangle(0,2,3);
		NormalBuffer ns;
		ExtTriangleMesh sweep(std::move(vs), std::move(ts), std::move(ns));

		BBox staticBox = sweep.GetBBox();
		CHECK(staticBox.pMax.x < 1.01f, "static bbox does not cover sweep");

		std::vector<VertexBuffer> steps;
		steps.emplace_back(4); steps.emplace_back(4);
		for (u_int v = 0; v < 4; ++v) {
			steps[0][v] = Point(v%2, v/2, 0.f);
			steps[1][v] = Point(2.f + v%2, v/2, 0.f);
		}
		sweep.SetVertexMotion(std::vector<float>{0.f, 1.f}, std::move(steps));

		BBox sweptBox = sweep.GetBBox();
		CHECK(sweptBox.pMax.x > 2.99f && sweptBox.pMin.x < 0.01f,
				"swept bbox covers all motion steps");

		// FromMesh resolves the base ext mesh; plain meshes -> nullptr
		CHECK(ExtTriangleMesh::FromMesh(&sweep) == &sweep,
				"FromMesh resolves ext mesh");
		VertexBuffer pv(3);
		pv[0]=Point(0,0,0); pv[1]=Point(1,0,0); pv[2]=Point(0,1,0);
		TriangleBuffer pt(1); pt[0]=Triangle(0,1,2);
		TriangleMesh plain(std::move(pv), std::move(pt));
		CHECK(ExtTriangleMesh::FromMesh(&plain) == nullptr,
				"FromMesh returns nullptr for plain mesh");
	}

	//------------------------------------------------------------------
	// Phase 3: MBVHAccel intersection with vertex motion (CPU path)
	//------------------------------------------------------------------
	{
		Context ctx;
		std::deque<const Mesh *> meshes;

		// Leaf 0: static quad in the z=0 plane at x in [10,11]
		VertexBuffer svs(4);
		svs[0] = Point(10,0,0); svs[1] = Point(11,0,0);
		svs[2] = Point(11,1,0); svs[3] = Point(10,1,0);
		TriangleBuffer sts(2);
		sts[0] = Triangle(0,1,2); sts[1] = Triangle(0,2,3);
		auto staticQuad = std::make_unique<ExtTriangleMesh>(
				std::move(svs), std::move(sts), NormalBuffer());
		meshes.push_back(staticQuad.get());

		// Leaf 1: quad at x in [0,1] sweeping to x in [2,3] (z=0 plane)
		VertexBuffer mvs(4);
		mvs[0] = Point(0,0,0); mvs[1] = Point(1,0,0);
		mvs[2] = Point(1,1,0); mvs[3] = Point(0,1,0);
		TriangleBuffer mts(2);
		mts[0] = Triangle(0,1,2); mts[1] = Triangle(0,2,3);
		auto motionQuad = std::make_unique<ExtTriangleMesh>(
				std::move(mvs), std::move(mts), NormalBuffer());
		{
			std::vector<VertexBuffer> steps;
			steps.emplace_back(4); steps.emplace_back(4);
			for (u_int v = 0; v < 4; ++v) {
				steps[0][v] = Point(v%2, v/2, 0.f);
				steps[1][v] = Point(2.f + v%2, v/2, 0.f);
			}
			motionQuad->SetVertexMotion(std::vector<float>{0.f, 1.f}, std::move(steps));
		}
		meshes.push_back(motionQuad.get());

		MBVHAccel accel(ctx);
		accel.Init(meshes, 8, 4);

		RayHit hit;
		auto shoot = [&](float x, float y, float time) -> bool {
			hit.SetMiss();
			Ray ray(Point(x, y, 5.f), Vector(0,0,-1), 1e-4f, 100.f, time);
			return accel.Intersect(&ray, &hit);
		};

		// Ray at the motion quad's start position: hit at t=0, miss at t=1
		CHECK(shoot(0.5f, 0.5f, 0.f) && hit.meshIndex == 1,
				"MBVH motion: hit base pose at t=0");
		CHECK(!shoot(0.5f, 0.5f, 1.f),
				"MBVH motion: base pose empty at t=1");

		// Ray at the end position: only the swept bound lets traversal
		// reach the leaf; the interpolated triangle must hit only at t=1
		CHECK(!shoot(2.5f, 0.5f, 0.f),
				"MBVH motion: swept bound does not fabricate a hit at t=0");
		CHECK(shoot(2.5f, 0.5f, 1.f) && hit.meshIndex == 1 &&
				fabsf(hit.t - 5.f) < 1e-3,
				"MBVH motion: hit end pose at t=1");
		CHECK(shoot(1.5f, 0.5f, 0.5f) && hit.meshIndex == 1,
				"MBVH motion: hit interpolated pose at t=0.5 (x+1)");
		CHECK(!shoot(2.5f, 0.5f, 0.5f),
				"MBVH motion: mid-sweep position empty at t=0.5");

		// Mixed leaves: static quad attribution and occlusion ordering
		CHECK(shoot(10.5f, 0.5f, 0.5f) && hit.meshIndex == 0,
				"MBVH motion: static leaf unaffected");

		// Clamp outside the series range
		CHECK(!shoot(2.5f, 0.5f, -0.5f),
				"MBVH motion: clamp below range -> step0 pose (miss)");
		CHECK(shoot(0.5f, 0.5f, -0.5f) && hit.meshIndex == 1,
				"MBVH motion: clamp below range -> step0 pose (hit)");
		CHECK(shoot(2.5f, 0.5f, 1.7f) && hit.meshIndex == 1,
				"MBVH motion: clamp above range -> last step pose");

		// Occlusion: a static occluder in front of the swept volume wins
		// over the (correctly missed) interpolated triangle
		VertexBuffer ovs(4);
		ovs[0] = Point(2,0,2); ovs[1] = Point(3,0,2);
		ovs[2] = Point(3,1,2); ovs[3] = Point(2,1,2);
		TriangleBuffer ots(2);
		ots[0] = Triangle(0,1,2); ots[1] = Triangle(0,2,3);
		auto occluder = std::make_unique<ExtTriangleMesh>(
				std::move(ovs), std::move(ots), NormalBuffer());
		meshes.push_back(occluder.get());

		MBVHAccel accel2(ctx);
		accel2.Init(meshes, 12, 6);
		hit.SetMiss();
		Ray occl(Point(2.5f, 0.5f, 5.f), Vector(0,0,-1), 1e-4f, 100.f, 0.f);
		CHECK(accel2.Intersect(&occl, &hit) && hit.meshIndex == 2 &&
				fabsf(hit.t - 3.f) < 1e-3,
				"MBVH motion: occluder inside swept bound wins");
	}

	//------------------------------------------------------------------
	// Phase 3: non-uniform K=3 timing
	//------------------------------------------------------------------
	{
		Context ctx;
		std::deque<const Mesh *> meshes;

		VertexBuffer mvs(4);
		mvs[0] = Point(0,0,0); mvs[1] = Point(1,0,0);
		mvs[2] = Point(1,1,0); mvs[3] = Point(0,1,0);
		TriangleBuffer mts(2);
		mts[0] = Triangle(0,1,2); mts[1] = Triangle(0,2,3);
		auto q = std::make_unique<ExtTriangleMesh>(
				std::move(mvs), std::move(mts), NormalBuffer());
		{
			// Non-uniform times {0, 0.2, 1.0}: the quad reaches x+2 by
			// t=0.2 and stays there; uniform sampling would put step 1
			// at t=0.5 and misplace the pose.
			std::vector<VertexBuffer> steps;
			steps.emplace_back(4); steps.emplace_back(4); steps.emplace_back(4);
			for (u_int v = 0; v < 4; ++v) {
				steps[0][v] = Point(v%2, v/2, 0.f);
				steps[1][v] = Point(2.f + v%2, v/2, 0.f);
				steps[2][v] = Point(2.f + v%2, v/2, 0.f);
			}
			q->SetVertexMotion(std::vector<float>{0.f, 0.2f, 1.f}, std::move(steps));
		}
		meshes.push_back(q.get());

		MBVHAccel accel(ctx);
		accel.Init(meshes, 4, 2);

		RayHit hit;
		auto shoot = [&](float x, float y, float time) -> bool {
			hit.SetMiss();
			Ray ray(Point(x, y, 5.f), Vector(0,0,-1), 1e-4f, 100.f, time);
			return accel.Intersect(&ray, &hit);
		};

		CHECK(shoot(2.5f, 0.5f, 0.3f), "K=3 nonuniform: already at step1 pose at t=0.3");
		CHECK(shoot(2.5f, 0.5f, 0.2f), "K=3 nonuniform: exact step1 time");
		CHECK(shoot(1.5f, 0.5f, 0.1f), "K=3 nonuniform: mid first segment (x+1)");
		CHECK(!shoot(2.5f, 0.5f, 0.1f), "K=3 nonuniform: not yet at step1 at t=0.1");
		CHECK(shoot(2.5f, 0.5f, 1.f), "K=3 nonuniform: holds pose to end");
	}

	//------------------------------------------------------------------
	// Phase 4: EmbreeAccel vertex timesteps (CPU path)
	//------------------------------------------------------------------
	{
		Context ctx;
		std::deque<const Mesh *> meshes;

		// Leaf 0: static quad in the z=0 plane at x in [10,11]
		VertexBuffer svs(4);
		svs[0] = Point(10,0,0); svs[1] = Point(11,0,0);
		svs[2] = Point(11,1,0); svs[3] = Point(10,1,0);
		TriangleBuffer sts(2);
		sts[0] = Triangle(0,1,2); sts[1] = Triangle(0,2,3);
		auto staticQuad = std::make_unique<ExtTriangleMesh>(
				std::move(svs), std::move(sts), NormalBuffer());
		meshes.push_back(staticQuad.get());

		// Leaf 1: quad at x in [0,1] sweeping to x in [2,3] (z=0 plane)
		VertexBuffer mvs(4);
		mvs[0] = Point(0,0,0); mvs[1] = Point(1,0,0);
		mvs[2] = Point(1,1,0); mvs[3] = Point(0,1,0);
		TriangleBuffer mts(2);
		mts[0] = Triangle(0,1,2); mts[1] = Triangle(0,2,3);
		auto motionQuad = std::make_unique<ExtTriangleMesh>(
				std::move(mvs), std::move(mts), NormalBuffer());
		{
			std::vector<VertexBuffer> steps;
			steps.emplace_back(4); steps.emplace_back(4);
			for (u_int v = 0; v < 4; ++v) {
				steps[0][v] = Point(v%2, v/2, 0.f);
				steps[1][v] = Point(2.f + v%2, v/2, 0.f);
			}
			motionQuad->SetVertexMotion(std::vector<float>{0.f, 1.f}, std::move(steps));
		}
		meshes.push_back(motionQuad.get());

		EmbreeAccel accel(ctx);
		accel.Init(meshes, 8, 4);

		RayHit hit;
		auto shoot = [&](float x, float y, float time) -> bool {
			hit.SetMiss();
			Ray ray(Point(x, y, 5.f), Vector(0,0,-1), 1e-4f, 100.f, time);
			return accel.Intersect(&ray, &hit);
		};

		CHECK(shoot(0.5f, 0.5f, 0.f) && hit.meshIndex == 1,
				"Embree motion: hit base pose at t=0");
		CHECK(!shoot(0.5f, 0.5f, 1.f),
				"Embree motion: base pose empty at t=1");
		CHECK(!shoot(2.5f, 0.5f, 0.f),
				"Embree motion: no fabricated hit at t=0");
		CHECK(shoot(2.5f, 0.5f, 1.f) && hit.meshIndex == 1 &&
				fabsf(hit.t - 5.f) < 1e-3,
				"Embree motion: hit end pose at t=1");
		CHECK(shoot(1.5f, 0.5f, 0.5f) && hit.meshIndex == 1,
				"Embree motion: hit interpolated pose at t=0.5");
		CHECK(shoot(10.5f, 0.5f, 0.5f) && hit.meshIndex == 0,
				"Embree motion: static leaf unaffected");
		CHECK(shoot(0.5f, 0.5f, -0.5f) && hit.meshIndex == 1,
				"Embree motion: clamp below range");
		CHECK(shoot(2.5f, 0.5f, 1.7f) && hit.meshIndex == 1,
				"Embree motion: clamp above range");
	}

	printf("\n%d failures\n", fails);
	return fails ? 1 : 0;
}

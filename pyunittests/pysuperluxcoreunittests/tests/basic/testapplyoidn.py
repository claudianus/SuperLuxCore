# -*- coding: utf-8 -*-
################################################################################
# Copyright 1998-2025 by authors (see AUTHORS.txt)
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

import time
from array import array

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *


# Film.ApplyOIDN() denoises channel_IMAGEPIPELINEs[index] in place, and
# that channel only holds the result of the *last* ExecuteImagePipeline().
# The Blender viewport denoiser worker calls ApplyOIDN without running a
# readback first - and once the interactive denoiser owns the display,
# raw readbacks (which execute the pipeline) are suppressed entirely, so
# the channel froze at whatever the last raw update left. On RTPATHOCL
# the freeze typically landed while the film was still empty: every OIDN
# run then denoised an all-zero buffer and the viewport consumed black
# frames (the "denoiser does nothing" bug).
#
# ApplyOIDN now re-executes the image pipeline under filmMutex before
# denoising, so the input always reflects current samples.
#
# Regression coverage: ApplyOIDN on a running session with NO prior
# GetOutputFloat/ExecuteImagePipeline must still produce a real image,
# on both the CPU and the GPU engine (CPU/GPU parity rule).
class TestApplyOIDN(LuxCoreTest):
	def _scene(self):
		# A closed matte room with an interior emitter, camera inside:
		# every pixel converges to a lit surface, so a healthy denoise
		# output can not be confused with a legitimately black render.
		scene = pysuperluxcore.Scene()
		s = 5.0
		verts = [(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s),
			(-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]
		tris = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
			(0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
			(2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
		scene.DefineMesh("room", verts, tris, None, None, None, None)
		scene.DefineMesh("lamp",
			[(-1, -1, 2), (1, -1, 2), (1, 1, 2), (-1, 1, 2)],
			[(0, 1, 2), (0, 2, 3)], None, None, None, None)
		scene.Parse(pysuperluxcore.Properties().SetFromString("""
			scene.camera.lookat.orig = 0 0 -2
			scene.camera.lookat.target = 0.2 0.3 1
			scene.camera.fieldofview = 60
			scene.materials.matte.type = matte
			scene.materials.matte.kd = 0.7 0.7 0.7
			scene.materials.emit.type = matte
			scene.materials.emit.emission = 50 50 50
			scene.objects.room.shape = room
			scene.objects.room.material = matte
			scene.objects.lamp.shape = lamp
			scene.objects.lamp.material = emit
			"""))
		return scene

	def _run(self, cfgProps):
		session = pysuperluxcore.RenderSession(
			pysuperluxcore.RenderConfig(cfgProps, self._scene()))
		try:
			session.Start()

			# Let the film accumulate real samples first - denoising an
			# empty film legitimately produces a black image
			deadline = time.time() + 30.0
			while time.time() < deadline:
				session.UpdateStats()
				if session.GetStats().Get(
						"stats.renderengine.pass").GetInt() >= 4:
					break
				time.sleep(0.1)
			self.assertGreaterEqual(
				session.GetStats().Get("stats.renderengine.pass").GetInt(),
				4, "session produced no samples")

			film = session.GetFilm()
			w, h = film.GetWidth(), film.GetHeight()

			# The core regression: ApplyOIDN is called while NO raw
			# readback has ever executed the pipeline - the denoise must
			# still yield pixels, not the empty channel contents.
			# (A lone denormal/NaN straggler is not a real image: the
			# thresholds demand an actual rendered frame.)
			film.ApplyOIDN(0)

			data = array('f', bytes(w * h * 3 * 4))
			film.GetOutputFloat(
				pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
				data, 0, False)
			self.assertGreater(max(data), 0.5,
				"ApplyOIDN left channel_IMAGEPIPELINEs[0] empty - "
				"denoised a stale buffer instead of current samples")
			self.assertGreater(sum(data) / len(data), 1e-3,
				"ApplyOIDN produced a near-black image - "
				"denoised a stale buffer instead of current samples")
		finally:
			session.Stop()

	def test_oidn_refreshes_channel_rtpathcpu(self):
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHCPU
			sampler.type = RTPATHCPUSAMPLER
			film.width = 160
			film.height = 120
			film.imagepipelines.0.type = NOP
			film.imagepipelines.1.type = TONEMAP_LINEAR
			film.imagepipelines.1.scale = 1
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")
		self._run(cfg)

	def test_oidn_refreshes_channel_rtpathocl(self):
		if not LuxCoreHasOpenCL():
			self.skipTest("requires OpenCL")
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHOCL
			sampler.type = TILEPATHSAMPLER
			film.width = 160
			film.height = 120
			film.imagepipelines.0.type = NOP
			film.imagepipelines.1.type = TONEMAP_LINEAR
			film.imagepipelines.1.scale = 1
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")
		self._run(cfg)

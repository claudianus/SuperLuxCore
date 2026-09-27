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

import threading
import time
from array import array

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *


# RTPATHOCL runs the HW film image pipeline (Film::MergeSampleBuffersHW)
# on every GetOutputFloat: the merge kernels launch
# RoundUp(pixelCount, 256) work-items, so a film whose pixel count is not
# a multiple of 256 runs tail work-items with gid >= pixelCount. The
# kernel bounds check must reject them - a ">" check let exactly one
# work-item write 12 bytes past the imagepipeline buffer, a device-side
# OOB write on every film readback (on unified memory this can corrupt
# neighbouring allocations; observed as a Blender gizmo-list crash while
# orbiting the viewport after switching CPU -> GPU).
#
# The same session.Parse() resize path also replaces the Film object;
# the swap runs under filmMutex so readback threads cannot observe a
# half-destroyed film (previously a use-after-free window).
#
# Regression coverage:
#  - film 257x257 (pixelCount % 256 == 1, the worst tail case) survives
#    repeated GetOutputFloat readbacks through the HW merge path
#  - Parse() resizes racing an unlocked readback thread neither crash,
#    deadlock nor wedge the session (sample count keeps growing)
class TestRTPathOCLFilmResize(LuxCoreTest):
	def _scene(self):
		scene = pysuperluxcore.Scene()
		scene.DefineMesh("box",
			[(-5, -5, -1), (5, -5, -1), (5, 5, -1), (-5, 5, -1)],
			[(0, 1, 2), (0, 2, 3)], None, None, None, None)
		scene.DefineMesh("lamp",
			[(-1, -1, 2), (1, -1, 2), (1, 1, 2), (-1, 1, 2)],
			[(0, 1, 2), (0, 2, 3)], None, None, None, None)
		scene.Parse(pysuperluxcore.Properties().SetFromString("""
			scene.camera.lookat.orig = 0 0 -3
			scene.camera.lookat.target = 0.2 0.3 0
			scene.camera.fieldofview = 60
			scene.materials.matte.type = matte
			scene.materials.matte.kd = 0.7 0.7 0.7
			scene.materials.emit.type = matte
			scene.materials.emit.emission = 100 100 100
			scene.objects.box.shape = box
			scene.objects.box.material = matte
			scene.objects.lamp.shape = lamp
			scene.objects.lamp.material = emit
			"""))
		return scene

	def _sample_count(self, session):
		return session.GetFilm().GetStats().Get(
			"stats.film.total.samplecount").GetInt()

	def _readback_once(self, session):
		# One viewport-style readback: HW merge kernels + image pipeline
		film = session.GetFilm()
		w, h = film.GetWidth(), film.GetHeight()
		data = array('f', bytes(w * h * 3 * 4))
		film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
			data, 0, True)
		return data, w, h

	def test_hw_merge_at_non_workgroup_multiple_size(self):
		if not LuxCoreHasOpenCL():
			self.skipTest("requires OpenCL")

		scene = self._scene()
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHOCL
			sampler.type = TILEPATHSAMPLER
			film.width = 257
			film.height = 257
			film.imagepipelines.0.type = VIEWPORT_INFILL
			film.imagepipelines.1.type = VIEWPORT_TEMPORAL
			film.imagepipelines.2.type = NOP
			film.imagepipelines.3.type = TONEMAP_LINEAR
			film.imagepipelines.3.scale = 1
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")

		session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
		try:
			session.Start()

			# Repeated readbacks on the 257x257 film run the merge kernels
			# with one tail work-item at gid == pixelCount each time
			for _ in range(4):
				data, w, h = self._readback_once(session)
				self.assertEqual((w, h), (257, 257))
				self.assertEqual(len(data), 257 * 257 * 3)
			self.assertTrue(self._sample_count(session) > 0)
		finally:
			session.Stop()

	def test_resize_during_readback(self):
		if not LuxCoreHasOpenCL():
			self.skipTest("requires OpenCL")

		scene = self._scene()
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHOCL
			sampler.type = TILEPATHSAMPLER
			film.width = 257
			film.height = 257
			film.imagepipelines.0.type = VIEWPORT_INFILL
			film.imagepipelines.1.type = VIEWPORT_TEMPORAL
			film.imagepipelines.2.type = NOP
			film.imagepipelines.3.type = TONEMAP_LINEAR
			film.imagepipelines.3.scale = 1
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")

		session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
		stop = threading.Event()
		errors = []

		def reader():
			# Raw-API contract check: no Python-level lock, the session
			# must serialize Parse() against GetOutputFloat() itself
			try:
				while not stop.is_set():
					try:
						self._readback_once(session)
					except RuntimeError as ex:
						# A resize between the dimension query and the
						# output fetch legitimately shrinks the buffer
						# requirement mid-call; anything else is a bug
						if "Not enough space" not in str(ex):
							raise
			except BaseException as ex:
				errors.append(ex)
				stop.set()

		try:
			session.Start()
			thread = threading.Thread(target=reader, daemon=True)
			thread.start()

			# Resizes racing the readback thread: alternate non-multiple
			# and multiple-of-256 sizes plus a camera edit between them
			sizes = [(200, 300), (257, 257), (256, 256), (127, 129), (257, 257)]
			for i in range(10):
				w, h = sizes[i % len(sizes)]
				session.Parse(pysuperluxcore.Properties().SetFromString(
					"film.width = %d\nfilm.height = %d" % (w, h)))
				session.BeginSceneEdit()
				scene.Parse(pysuperluxcore.Properties().SetFromString(
					"scene.camera.lookat.target = %f %f 0" % (0.1 + i * 0.01, 0.1)))
				session.EndSceneEdit()

			stop.set()
			thread.join(timeout=30)
			self.assertFalse(thread.is_alive(), "readback thread wedged")
			if errors:
				raise errors[0]

			# The last resize's film must still be accumulating
			count = self._sample_count(session)
			deadline = time.time() + 15.0
			while self._sample_count(session) <= count and time.time() < deadline:
				time.sleep(0.05)
			self.assertTrue(self._sample_count(session) > count,
				"film stopped accumulating after resizes")

			data, w, h = self._readback_once(session)
			self.assertEqual((w, h), sizes[(10 - 1) % len(sizes)])
		finally:
			stop.set()
			session.Stop()

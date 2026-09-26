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

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *


# RTPATHOCL exposes RenderSession.SetRuntimeResolutionReduction(): a
# runtime override of rtpath.resolutionreduction meant for interactive
# viewports (sparse fast passes while the user drags, dense passes once
# settled). It applies at the next frame boundary without a film reset
# and must never stall the render.
#
# Regression coverage:
#  - override keeps the film accumulating (no pass stall)
#  - values below the configured reduction are clamped (task buffers were
#    sized for the configured density)
#  - a camera edit landing while the override is active still recovers
#    (observed wedge on Metal when overriding mid-edit burst)
class TestRTPathOCLRuntimeResolutionReduction(LuxCoreTest):
	def _scene(self):
		scene = pysuperluxcore.Scene()
		# Two quads: a ground plane and an emissive card above it
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

	def _wait_growth(self, session, last, timeout=15.0):
		"""Poll until the film's sample count exceeds ``last``."""
		deadline = time.time() + timeout
		while time.time() < deadline:
			count = self._sample_count(session)
			if count > last:
				return count
			time.sleep(0.05)
		self.fail("film stopped accumulating (count=%s)" % last)
		return last

	def test_runtime_resolution_reduction(self):
		if not LuxCoreHasOpenCL():
			self.skipTest("requires OpenCL")

		scene = self._scene()
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHOCL
			sampler.type = TILEPATHSAMPLER
			film.width = 256
			film.height = 256
			rtpath.resolutionreduction = 4
			rtpath.resolutionreduction.preview = 8
			rtpath.resolutionreduction.preview.step = 2
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")

		session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
		try:
			session.Start()

			# Baseline: the film accumulates
			count = self._wait_growth(session, 0)

			# Coarser override (interaction): still accumulating
			session.SetRuntimeResolutionReduction(16)
			count = self._wait_growth(session, count)

			# Denser than configured: clamped, must not wedge
			session.SetRuntimeResolutionReduction(2)
			count = self._wait_growth(session, count)

			# Camera edit while the override is active: the film resets
			# at the next boundary and must recover - this is the exact
			# path the viewport drives per draw during an orbit.
			session.SetRuntimeResolutionReduction(16)
			count = self._sample_count(session)
			session.BeginSceneEdit()
			scene.Parse(pysuperluxcore.Properties().SetFromString(
				"scene.camera.lookat.target = 0.1 0.1 0"))
			session.EndSceneEdit()
			# The film resets at the next boundary; the wedge this guards
			# against pins the count, so post-edit growth is the pass/fail.
			count = self._wait_growth(session, count)

			# Restore configured behaviour
			session.SetRuntimeResolutionReduction(0)
			self._wait_growth(session, count)
		finally:
			session.Stop()

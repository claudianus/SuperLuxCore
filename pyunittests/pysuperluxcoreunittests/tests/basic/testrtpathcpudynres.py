# -*- coding: utf-8 -*-
################################################################################
# Copyright 1998-2025 by authors (see AUTHORS.txt)
#
#   This file is part of LuxCoreRender.
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
################################################################################

import time

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *


# RTPATHCPU strips steady-state coverage in lines of zoomFactor rows:
# on wrap the sampler jumps to row (myStep * zoomFactor) % frameHeight.
# frameHeight used to be captured once per Reset() while
# SetRuntimeResolutionReduction() changes zoomFactor live without
# pausing the render threads - a stale frameHeight padded to a
# different factor (H not divisible by the built factor) could map
# myStep onto a row >= filmSubRegionHeight, indexing past the end of
# pixelRenderSequence (heap OOB read, observed as a viewport-orbit
# crash on macOS with multiple render threads faulting in NextPixel).
#
# Regression coverage:
#  - runtime reduction changes while rendering must keep the film
#    accumulating (pre-fix builds crashed in NextPixel once
#    myStep * zf % staleFrameHeight landed in [H, frameHeight))
#  - a scene edit landing while the override is active must recover -
#    this is the exact sequence the Blender viewport drives per draw
#    during an orbit (16 while dragging, back to base once settled)
#
# film.height = 181 is deliberately not divisible by the configured
# zoomphase 16 (RoundUp(181,16)=192), which is what made the stale
# modulus able to produce out-of-range rows pre-fix.
class TestRTPathCPURuntimeResolutionReduction(LuxCoreTest):
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
		scene = self._scene()
		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHCPU
			sampler.type = RTPATHCPUSAMPLER
			film.width = 320
			film.height = 181
			rtpathcpu.zoomphase.size = 16
			rtpathcpu.zoomphase.weight = 0.1
			batch.haltspp = 0
			batch.halttime = 0
			batch.haltthreshold = -1
			""")

		session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
		try:
			session.Start()

			# Baseline: the film accumulates
			count = self._wait_growth(session, 0)

			# Live factor change with no pause/edit: the stale modulus
			# mapped onto rows >= film height here (pre-fix crash)
			for reduction in (4, 16, 2, 8, 1):
				session.SetRuntimeResolutionReduction(reduction)
				count = self._wait_growth(session, count)

			# Scene edit while the override is active: Reset() rebuilds
			# the sequences with the live factor - the orbit-time path
			session.SetRuntimeResolutionReduction(16)
			session.BeginSceneEdit()
			scene.Parse(pysuperluxcore.Properties().SetFromString(
				"scene.camera.lookat.target = 0.1 0.1 0"))
			session.EndSceneEdit()
			count = self._wait_growth(session, count)

			# Restore the configured behaviour once interaction ends
			session.SetRuntimeResolutionReduction(4)
			self._wait_growth(session, count)
		finally:
			session.Stop()

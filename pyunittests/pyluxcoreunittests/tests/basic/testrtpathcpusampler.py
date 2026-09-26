# -*- coding: utf-8 -*-
################################################################################
# Copyright 1998-2018 by authors (see AUTHORS.txt)
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

import math
import time
from array import array as FloatArray

import pyluxcore

from pyluxcoreunittests.tests.utils import *


# RTPATHCPU renders a coarse "first frame" pass before the normal
# shuffled full-resolution pass: one sample per zoomFactor x zoomFactor
# block, splatted over the whole block with zoomphase.weight. The coarse
# blocks are visited in a shuffled (spatially scattered) order so the
# first frame shows a dithered full image instead of a row band filling
# bottom-to-top.
#
# This test polls the film while the first frame is in progress and checks
# that the earliest covered blocks are not confined to the bottom rows.
class TestRTPathCPUFirstFrame(LuxCoreTest):
	def test_first_frame_spatial_scatter(self):
		width, height, zf = 512, 512, 32
		cw, ch = width // zf, height // zf  # 16x16 = 256 coarse blocks

		scene = pyluxcore.Scene()

		# A closed mirror room: paths bounce until path.maxdepth, so each
		# sample takes long enough to observe the first frame mid-flight.
		s = 5.0
		verts = [(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s),
			(-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]
		tris = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
			(0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
			(2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
		scene.DefineMesh("room", verts, tris, None, None, None, None)

		# A small emissive "lamp" inside the room so the bouncing paths
		# terminate on a light and produce non-zero radiance (the mirror
		# walls make samples slow enough to observe the first frame)
		lampVerts = []
		lampTris = []
		lampSegs = 24
		for i in range(lampSegs):
			phi = math.acos(1.0 - 2.0 * (i + 0.5) / lampSegs)
			theta = math.pi * (1.0 + math.sqrt(5.0)) * i
			lampVerts.append((0.8 * math.sin(phi) * math.cos(theta) + 1.5,
				0.8 * math.sin(phi) * math.sin(theta) + 1.0,
				0.8 * math.cos(phi) + 1.0))
		for i in range(lampSegs - 2):
			lampTris.append((0, i + 1, i + 2))
		scene.DefineMesh("lamp", lampVerts, lampTris, None, None, None, None)

		scene.Parse(pyluxcore.Properties().SetFromString("""
			scene.camera.lookat.orig = 0 0 -2
			scene.camera.lookat.target = 0.2 0.3 1
			scene.camera.fieldofview = 60
			scene.materials.mirr.type = mirror
			scene.materials.mirr.kr = 0.95 0.95 0.95
			scene.materials.emit.type = matte
			scene.materials.emit.emission = 5000 5000 5000
			scene.objects.room.shape = room
			scene.objects.room.material = mirr
			scene.objects.lamp.shape = lamp
			scene.objects.lamp.material = emit
			"""))

		cfg = pyluxcore.Properties().SetFromString("""
			renderengine.type = RTPATHCPU
			sampler.type = RTPATHCPUSAMPLER
			film.width = %d
			film.height = %d
			rtpathcpu.zoomphase.size = %d
			path.maxdepth = 64
			native.threads.count = 1
			""" % (width, height, zf))

		session = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
		try:
			session.Start()

			film = session.GetFilm()
			buf = FloatArray('f', bytes(film.GetOutputSize(
				pyluxcore.FilmOutputType.RGB) * 4))

			# Poll the film until at least 4 coarse blocks are covered. A
			# splatted coarse sample fills its whole block, so checking the
			# block center pixel is enough to tell covered blocks apart.
			hits = set()
			deadline = time.time() + 30.0
			while len(hits) < 4 and time.time() < deadline:
				film.GetOutputFloat(pyluxcore.FilmOutputType.RGB, buf)
				for by in range(ch):
					for bx in range(cw):
						cx = bx * zf + zf // 2
						cy = by * zf + zf // 2
						if buf[(cy * width + cx) * 3] > 1e-9:
							hits.add((bx, by))
				time.sleep(0.002)
		finally:
			session.Stop()

		self.assertTrue(len(hits) >= 4,
			"no first-frame coverage observed within the time budget")
		self.assertTrue(len(hits) < cw * ch,
			"first frame already complete: cannot check block order")

		# A bottom-to-top scanline fill can only reach the top half of the
		# image after half of the coarse blocks are done; the scattered
		# order hits the top half almost immediately.
		topHalf = sum(1 for _, by in hits if by >= ch // 2)
		self.assertTrue(topHalf > 0,
			"first-frame coarse blocks confined to the bottom half (scanline order): %s" % sorted(hits))

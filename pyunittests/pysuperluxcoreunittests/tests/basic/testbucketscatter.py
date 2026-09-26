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

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *


# Bucket samplers (SOBOL / RANDOM / PMJ02) used to consume pixel buckets in
# Morton-tile row-major order: on CPU engines a handful of threads drain
# consecutive buckets, so the first pass visibly filled the film
# bottom-to-top (viewport + progressive final render). The shared data now
# permutes the bucket index (golden-ratio stride bijection) so buckets are
# served in a scattered order - every bucket still rendered exactly once
# per cycle.
#
# This test polls the film while the first pass is in flight and checks
# that coverage is not confined to the bottom rows.
class TestBucketScatter(LuxCoreTest):
	def _check_first_pass_scatter(self, samplerType):
		width, height = 512, 512

		scene = pysuperluxcore.Scene()

		# A closed mirror room: paths bounce until path.maxdepth, so each
		# sample takes long enough to observe the first pass mid-flight.
		s = 5.0
		verts = [(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s),
			(-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]
		tris = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
			(0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
			(2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
		scene.DefineMesh("room", verts, tris, None, None, None, None)

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

		scene.Parse(pysuperluxcore.Properties().SetFromString("""
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

		cfg = pysuperluxcore.Properties().SetFromString("""
			renderengine.type = PATHCPU
			sampler.type = %s
			film.width = %d
			film.height = %d
			path.maxdepth = 64
			native.threads.count = 1
			""" % (samplerType, width, height))

		session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
		try:
			session.Start()

			film = session.GetFilm()
			buf = FloatArray('f', bytes(film.GetOutputSize(
				pysuperluxcore.FilmOutputType.RGB) * 4))

			halfH = height // 2
			bottomHits = 0
			topHits = 0
			deadline = time.time() + 30.0
			# Wait until the bottom half has some coverage (a few buckets
			# done). A row sweep cannot reach the top half until ~half of
			# the frame is covered; the scattered order hits it almost
			# immediately (P(all of the first >=4 buckets in the bottom
			# half) < 1%).
			while time.time() < deadline:
				film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, buf)

				bottomHits = 0
				topHits = 0
				for y in range(height):
					base = y * width * 3
					if y < halfH:
						for x in range(width):
							if buf[base + x * 3] > 1e-9:
								bottomHits += 1
					else:
						for x in range(width):
							if buf[base + x * 3] > 1e-9:
								topHits += 1

				if bottomHits >= 64:
					break
				time.sleep(0.002)
		finally:
			session.Stop()

		self.assertTrue(bottomHits >= 64,
			"no first-pass coverage observed within the time budget")
		self.assertTrue(topHits > 0,
			"first-pass coverage confined to the bottom half "
			"(sequential tile-row order): bottom=%d top=%d" % (bottomHits, topHits))

	def test_sobol_scatter(self):
		self._check_first_pass_scatter("SOBOL")

	def test_random_scatter(self):
		self._check_first_pass_scatter("RANDOM")

	def test_pmj02_scatter(self):
		self._check_first_pass_scatter("PMJ02SAMPLER")

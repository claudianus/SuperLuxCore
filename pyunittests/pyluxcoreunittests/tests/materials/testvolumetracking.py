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

import math
import time
import unittest
import pyluxcore

from pyluxcoreunittests.tests.utils import *
from pyluxcoreunittests.tests.imagetest import *
from pyluxcoreunittests.tests.render import *


# Regression test for heterogeneous volume null-collision (delta) tracking:
# delta tracking and legacy ray marching are both unbiased estimators of the
# same path integral, so their images must converge to the same mean
# radiance. See doc/features/volume-tracking.md

def MakeDensityGrid(n):
	# Two thin vertical wisps: mostly empty space inside the unit grid
	data = []
	for z in range(n):
		for y in range(n):
			for x in range(n):
				px = (x + .5) / n - .5
				py = (y + .5) / n - .5
				pz = (z + .5) / n - .5
				cx = 0.12 * math.sin(pz * 9.0) - 0.15
				cy = 0.10 * math.sin(pz * 7.0 + 1.0)
				d1 = math.sqrt((px - cx) ** 2 + (py - cy) ** 2)
				cx2 = -0.10 * math.sin(pz * 8.0 + 2.0) + 0.2
				cy2 = 0.12 * math.cos(pz * 6.0)
				d2 = math.sqrt((px - cx2) ** 2 + (py - cy2) ** 2)
				v = max(0.0, 1.0 - d1 / 0.09) * 5.0 + max(0.0, 1.0 - d2 / 0.07) * 4.0
				data.append("%.4f" % v)
	return " ".join(data)


def BuildCube(objectName, materialName):
	# Unit cube centered at the origin
	prefix = "scene.objects." + objectName + "."
	props = pyluxcore.Properties()
	props.SetFromString(
		prefix + "material = " + materialName + "\n" +
		prefix + "vertices = "
			"-0.5 -0.5 -0.5  0.5 -0.5 -0.5  0.5 0.5 -0.5  -0.5 0.5 -0.5  "
			"-0.5 -0.5 0.5  0.5 -0.5 0.5  0.5 0.5 0.5  -0.5 0.5 0.5\n" +
		prefix + "faces = "
			"0 2 1  0 3 2  4 5 6  4 6 7  0 1 5  0 5 4  "
			"2 3 7  2 7 6  1 2 6  1 6 5  0 4 7  0 7 3\n"
	)
	return props


SCENE_TEMPLATE = """
scene.camera.lookat.orig = 0.0 -4.0 0.3
scene.camera.lookat.target = 0.0 0.0 0.0
scene.camera.fieldofview = 40

scene.textures.tex.type = densitygrid
scene.textures.tex.nx = 32
scene.textures.tex.ny = 32
scene.textures.tex.nz = 32
scene.textures.tex.wrap = black
scene.textures.tex.data = GRIDDATA
scene.textures.tex.mapping.type = globalmapping3d
scene.textures.tex.mapping.transformation = 1.0 0 0 0  0 1.0 0 0  0 0 1.0 0  0.5 0.5 0.5 1

scene.volumes.vol1.type = heterogeneous
scene.volumes.vol1.absorption = 0.15 0.15 0.15
scene.volumes.vol1.scattering = tex
scene.volumes.vol1.asymmetry = ASYMMETRY 0.3 0.3
scene.volumes.vol1.phase = PHASE
scene.volumes.vol1.tracking = TRACKING
scene.volumes.vol1.steps.size = 0.02
scene.volumes.vol1.steps.maxcount = 256
scene.volumes.vol1.multiscattering = 1

scene.materials.Matte.type = matte
scene.materials.Matte.kd = 0.7 0.7 0.7
scene.materials.mat1.type = null
scene.materials.mat1.volume.interior = vol1

scene.lights.sun.type = sun
scene.lights.sun.dir = -0.5 -1 0.2
scene.lights.sun.gain = 1.0 1.0 1.0
"""


def RenderVolumeScene(tracking, haltSPP, phase="schlick", asymmetry=0.3):
	cfgProps = pyluxcore.Properties()
	cfgProps.SetFromString("""
		film.width = 160
		film.height = 120
		sampler.type = SOBOL
		renderengine.type = PATHCPU
		batch.haltspp = HALTSPP
		path.pathdepth.total = 8
		""".replace("HALTSPP", str(haltSPP)))

	scnProps = pyluxcore.Properties()
	scnProps.SetFromString(SCENE_TEMPLATE
			.replace("GRIDDATA", MakeDensityGrid(32))
			.replace("TRACKING", tracking)
			.replace("PHASE", phase)
			.replace("ASYMMETRY", str(asymmetry)))
	scnProps.Set(BuildCube("volbox", "mat1"))
	# Floor plane
	floor = pyluxcore.Properties()
	floor.SetFromString("""
		scene.objects.floor.material = Matte
		scene.objects.floor.vertices = -4 -4 -0.6  -4 4 -0.6  4 4 -0.6  4 -4 -0.6
		scene.objects.floor.faces = 0 1 2  2 3 0
		""")
	scnProps.Set(floor)

	scene = pyluxcore.Scene()
	scene.Parse(scnProps)

	config = pyluxcore.RenderConfig(cfgProps, scene)
	session = pyluxcore.RenderSession(config)
	session.Start()
	while not session.HasDone():
		time.sleep(0.2)
		session.UpdateStats()
	session.Stop()

	return GetImagePipelineBuffer(session.GetFilm())


# Homogeneous fog + a point light embedded inside: the regime the
# equiangular/transmittance MIS distance sampler targets
# (Kulla & Fajardo, EGSR 2012). See doc/features/volume-tracking.md

EQ_SCENE_TEMPLATE = """
scene.camera.lookat.orig = 0.0 -5.0 1.0
scene.camera.lookat.target = 0.0 0.0 1.0
scene.camera.fieldofview = 40

scene.volumes.fog.type = homogeneous
scene.volumes.fog.absorption = 0.05 0.05 0.05
scene.volumes.fog.scattering = 1.0 1.0 1.0
scene.volumes.fog.asymmetry = 0.3 0.3 0.3
scene.volumes.fog.multiscattering = 1
scene.volumes.fog.distancesampling = DISTSAMPLING

scene.materials.FogMat.type = null
scene.materials.FogMat.volume.interior = fog

scene.lights.key.type = point
scene.lights.key.position = 0.0 0.5 1.4
scene.lights.key.gain = 40 40 42
"""


def RenderEqScene(distanceSampling, haltSPP, guiding=False):
	cfgProps = pyluxcore.Properties()
	cfgProps.SetFromString("""
		film.width = 160
		film.height = 120
		sampler.type = SOBOL
		renderengine.type = PATHCPU
		batch.haltspp = HALTSPP
		path.pathdepth.total = 6
		path.guiding.enable = GUIDING
		""".replace("HALTSPP", str(haltSPP)).replace("GUIDING", "1" if guiding else "0"))

	scnProps = pyluxcore.Properties()
	scnProps.SetFromString(EQ_SCENE_TEMPLATE.replace("DISTSAMPLING", distanceSampling))
	# Fog domain box
	dom = pyluxcore.Properties()
	dom.SetFromString("""
		scene.objects.domain.material = FogMat
		scene.objects.domain.vertices = -2 -2 0  2 -2 0  2 2 0  -2 2 0  -2 -2 2.6  2 -2 2.6  2 2 2.6  -2 2 2.6
		scene.objects.domain.faces = 0 2 1  0 3 2  4 5 6  4 6 7  0 1 5  0 5 4  2 3 7  2 7 6  1 2 6  1 6 5  0 4 7  0 7 3
		""")
	scnProps.Set(dom)

	scene = pyluxcore.Scene()
	scene.Parse(scnProps)

	config = pyluxcore.RenderConfig(cfgProps, scene)
	session = pyluxcore.RenderSession(config)
	session.Start()
	while not session.HasDone():
		time.sleep(0.2)
		session.UpdateStats()
	session.Stop()

	return GetImagePipelineBuffer(session.GetFilm())


class TestVolumeTracking(unittest.TestCase):
	def test_delta_vs_march_mean(self):
		# Both estimators are unbiased: the image means must agree within
		# a few Monte Carlo standard deviations
		imgDelta = RenderVolumeScene("delta", 48)
		imgMarch = RenderVolumeScene("march", 48)

		meanDelta = sum(imgDelta) / len(imgDelta)
		meanMarch = sum(imgMarch) / len(imgMarch)

		self.assertGreater(meanDelta, 0.0)
		self.assertGreater(meanMarch, 0.0)
		# 15% relative tolerance on the image mean is generous for two
		# unbiased estimators at 48 spp
		relErr = abs(meanDelta - meanMarch) / meanMarch
		self.assertLess(relErr, 0.15,
				"delta vs march mean radiance mismatch: %f vs %f" % (meanDelta, meanMarch))

	def test_hg_isotropic_matches_schlick(self):
		# At g = 0 both phase functions degenerate to isotropic scattering,
		# so HG and Schlick renders must converge to the same mean
		imgHG = RenderVolumeScene("delta", 48, phase="hg", asymmetry=0.0)
		imgSchlick = RenderVolumeScene("delta", 48, phase="schlick", asymmetry=0.0)

		meanHG = sum(imgHG) / len(imgHG)
		meanSchlick = sum(imgSchlick) / len(imgSchlick)

		self.assertGreater(meanHG, 0.0)
		relErr = abs(meanHG - meanSchlick) / meanSchlick
		self.assertLess(relErr, 0.15,
				"HG vs Schlick g=0 mean radiance mismatch: %f vs %f" % (meanHG, meanSchlick))

	def test_hg_anisotropic_converges(self):
		# Forward scattering g = 0.6: HG must stay unbiased (no NaN, sane
		# mean) and Schlick remains a close approximation of the same
		# scattering regime, so the means should roughly agree
		imgHG = RenderVolumeScene("delta", 48, phase="hg", asymmetry=0.6)
		imgSchlick = RenderVolumeScene("delta", 48, phase="schlick", asymmetry=0.6)

		meanHG = sum(imgHG) / len(imgHG)
		meanSchlick = sum(imgSchlick) / len(imgSchlick)

		self.assertGreater(meanHG, 0.0)
		for v in imgHG:
			self.assertFalse(math.isnan(v) or math.isinf(v))
		relErr = abs(meanHG - meanSchlick) / meanSchlick
		self.assertLess(relErr, 0.25,
				"HG vs Schlick g=0.6 mean radiance mismatch: %f vs %f" % (meanHG, meanSchlick))

	def test_equiangular_matches_transmittance(self):
		# The equiangular+transmittance MIS distance sampler and the plain
		# transmittance sampler are both unbiased estimators of the same
		# path integral: their image means must agree within MC error
		imgEq = RenderEqScene("equiangular", 64)
		imgTr = RenderEqScene("transmittance", 64)

		meanEq = sum(imgEq) / len(imgEq)
		meanTr = sum(imgTr) / len(imgTr)

		self.assertGreater(meanEq, 0.0)
		for v in imgEq:
			self.assertFalse(math.isnan(v) or math.isinf(v))
		relErr = abs(meanEq - meanTr) / meanTr
		self.assertLess(relErr, 0.10,
				"equiangular vs transmittance mean radiance mismatch: %f vs %f" % (meanEq, meanTr))

	def test_volume_guiding_unbiased(self):
		# Path guiding at volume vertices is a one-sample MIS against the
		# phase-function sampler (see doc/features/path-guiding.md): it may
		# only change variance, never the mean. Guided and unguided renders
		# of the same scene must agree within MC error. This exercises the
		# whole volume path: training records, round swaps, the vMF field
		# fit and the isotropic Sample/Pdf mixture.
		imgGuided = RenderEqScene("equiangular", 64, guiding=True)
		imgUnguided = RenderEqScene("equiangular", 64, guiding=False)

		meanGuided = sum(imgGuided) / len(imgGuided)
		meanUnguided = sum(imgUnguided) / len(imgUnguided)

		self.assertGreater(meanUnguided, 0.0)
		for v in imgGuided:
			self.assertFalse(math.isnan(v) or math.isinf(v))
		relErr = abs(meanGuided - meanUnguided) / meanUnguided
		self.assertLess(relErr, 0.10,
				"guided vs unguided mean radiance mismatch: %f vs %f" % (meanGuided, meanUnguided))

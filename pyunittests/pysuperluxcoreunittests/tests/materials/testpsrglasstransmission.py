# -*- coding: utf-8 -*-
################################################################################
# Copyright 1998-2026 by authors (see AUTHORS.txt)
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

# Regression: path.regularization.auto (default on) promoted specular
# glass to a GGX lobe at depth >= regularization.mindepth. For a ray
# *exiting* the dielectric (fixed dir below the shading plane, wo.z<0)
# GgxSampleVNDF degenerated to the horizon, returned pdfW ~ 4e-5 and a
# GLOSSY|TRANSMIT event; the next env direct-hit was MIS-weighted to
# ~3e-7 and the frame rendered ~200x too dark. The fix negates wo for
# the cap sample and un-negates wh (isotropic GGX is symmetric). This
# test renders a glass cube against a uniform infinite light and
# asserts the transmitted radiance reaches the film.

import time

import numpy as np

import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import LuxCoreTest


def RenderGlassAgainstEnv(extraCfg=""):
	# Camera stares at the unit cube through its front face; rays
	# transmit in (wo.z>0), bounce off the far face (wo.z<0, the
	# degenerate branch), and reach the uniform env light. With the
	# bug the exit vertex pdfW collapsed and the MIS weight zeroed the
	# env contribution.
	scnProps = pysuperluxcore.Properties()
	scnProps.SetFromString("""
		scene.camera.lookat.orig = 0.0 -3.0 0.0
		scene.camera.lookat.target = 0.0 0.0 0.0
		scene.camera.fieldofview = 30

		scene.materials.glass.type = glass
		scene.materials.glass.kr = 0.98 0.98 0.98
		scene.materials.glass.kt = 0.98 0.98 0.98
		scene.materials.glass.interiorior = 1.5
		scene.materials.glass.exteriorior = 1.0

		scene.lights.env.type = infinite
		scene.lights.env.gain = 1.0 1.0 1.0
		""")

	# Unit cube at the origin, front face toward the camera
	props = pysuperluxcore.Properties()
	props.SetFromString(
		"scene.objects.box.material = glass\n"
		"scene.objects.box.vertices = "
			"-0.5 -0.5 -0.5  0.5 -0.5 -0.5  0.5 0.5 -0.5  -0.5 0.5 -0.5  "
			"-0.5 -0.5 0.5  0.5 -0.5 0.5  0.5 0.5 0.5  -0.5 0.5 0.5\n"
		"scene.objects.box.faces = "
			"0 2 1  0 3 2  4 5 6  4 6 7  0 1 5  0 5 4  "
			"2 3 7  2 7 6  1 2 6  1 6 5  0 4 7  0 7 3\n")
	scnProps.Set(props)

	scene = pysuperluxcore.Scene()
	scene.Parse(scnProps)

	cfgProps = pysuperluxcore.Properties()
	# auto PSR is the default; no property pinning needed - the bug
	# fired with zero user config.
	cfgProps.SetFromString("""
		film.width = 96
		film.height = 72
		sampler.type = SOBOL
		renderengine.type = PATHCPU
		batch.haltspp = 24
		path.pathdepth.total = 8
		""" + extraCfg)
	config = pysuperluxcore.RenderConfig(cfgProps, scene)
	session = pysuperluxcore.RenderSession(config)
	session.Start()
	while not session.HasDone():
		time.sleep(0.2)
		session.UpdateStats()
	session.Stop()

	w, h = 96, 72
	rgb = np.zeros(w * h * 3, dtype=np.float32)
	session.GetFilm().GetOutputFloat(
			pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0)
	return rgb.reshape(h, w, 3)


class TestPSRGlassTransmission(LuxCoreTest):
	def test_auto_psr_env_through_glass_not_black(self):
		img = RenderGlassAgainstEnv()
		mean = float(img.mean())
		# Buggy: ~0.003 (MIS weight ~3e-7 zeroed the env hit).
		# Fixed: env gain 1 through glass ~0.5 after imagepipeline.
		self.assertGreater(mean, 0.05,
				f"glass+env+auto-PSR rendered dark: mean={mean:.5f} "
				"(GgxSampleVNDF below-horizon regression)")

	def test_auto_psr_matches_no_psr_within_blur(self):
		auto = RenderGlassAgainstEnv()
		nopsr = RenderGlassAgainstEnv("path.regularization.auto = 0\n")
		# PSR blurs by sigma=0.03 for the first ~64spp then decays; at
		# 24spp some blur remains, so allow a generous band - the test
		# only needs to catch the ~200x blackout, not pixel parity.
		rel = abs(float(auto.mean()) - float(nopsr.mean())) / max(float(nopsr.mean()), 1e-6)
		self.assertLess(rel, 0.35,
				f"auto mean={auto.mean():.4f} vs no-psr {nopsr.mean():.4f} "
				f"rel={rel:.3f}")

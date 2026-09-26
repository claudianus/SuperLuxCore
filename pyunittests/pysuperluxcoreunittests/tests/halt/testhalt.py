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

import time
import unittest
import pysuperluxcore

from pysuperluxcoreunittests.tests.utils import *
from pysuperluxcoreunittests.tests.imagetest import *

class TestHalt(unittest.TestCase):
	def RunHaltTest(self, testName, haltProps):
		# Load the configuration from file
		props = pysuperluxcore.Properties("resources/scenes/simple/simple.cfg")

		# Change the render engine to PATHCPU
		props.Set(pysuperluxcore.Property("renderengine.type", ["PATHCPU"]))
		props.Set(pysuperluxcore.Property("sampler.type", ["RANDOM"]))
		props.Set(GetDefaultEngineProperties("PATHCPU"))

		# Replace halt condition
		props.Delete("batch.haltspp")
		props.Delete("batch.halttime")
		props.Delete("batch.haltthreshold")
		# Run at full speed
		props.Delete("native.threads.count")
		props.Set(haltProps)

		config = pysuperluxcore.RenderConfig(props)
		session = pysuperluxcore.RenderSession(config)

		session.Start()
		while True:
			time.sleep(0.5)

			# Update statistics (and run the convergence test)
			session.UpdateStats()

			if session.HasDone():
				# Time to stop the rendering
				break
		session.Stop()

		image = GetImagePipelineImage(session.GetFilm())

		CheckResult(self, image, testName, False)

	def test_Halt_Threshold(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.haltthreshold", 0.075))
		props.Set(pysuperluxcore.Property("batch.haltthreshold.step", 16))
		
		self.RunHaltTest("Halt_Threshold", props)

	def test_Halt_Time(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.halttime", 20))
		
		self.RunHaltTest("Halt_Time", props)

	def test_Halt_SPP(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.haltspp", 128))
		
		self.RunHaltTest("Halt_SPP", props)

	def RunHaltEditTest(self, testName, haltProps):
		# Load the configuration from file
		props = pysuperluxcore.Properties("resources/scenes/simple/simple.cfg")

		# Change the render engine to PATHCPU
		props.Set(pysuperluxcore.Property("renderengine.type", ["PATHCPU"]))
		props.Set(pysuperluxcore.Property("sampler.type", ["RANDOM"]))
		props.Set(GetDefaultEngineProperties("PATHCPU"))

		# Replace halt condition
		props.Delete("batch.haltthreshold")
		# Run at full speed
		props.Delete("native.threads.count")

		config = pysuperluxcore.RenderConfig(props)
		session = pysuperluxcore.RenderSession(config)

		session.Start()
		time.sleep(10.0)
		session.Parse(haltProps)
		
		while True:
			time.sleep(0.5)

			# Update statistics (and run the convergence test)
			session.UpdateStats()

			if session.HasDone():
				# Time to stop the rendering
				break
		session.Stop()

		image = GetImagePipelineImage(session.GetFilm())

		CheckResult(self, image, testName, False)

	def test_HaltEdit_Threshold(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.haltthreshold", 0.075))
		props.Set(pysuperluxcore.Property("batch.haltthreshold.step", 16))
		
		self.RunHaltEditTest("HaltEdit_Threshold", props)

	def test_HaltEdit_Time(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.halttime", 20))
		
		self.RunHaltTest("HaltEdit_Time", props)

	def test_HaltEdit_SPP(self):
		props = pysuperluxcore.Properties()
		props.Set(pysuperluxcore.Property("batch.haltspp", 128))
		
		self.RunHaltTest("HaltEdit_SPP", props)

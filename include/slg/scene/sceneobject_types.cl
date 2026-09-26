#line 2 "sceneobject_types.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

typedef enum {
	COMBINED,
	LIGHTMAP
} BakeMapType;

typedef struct {
	unsigned int objectID;
	unsigned int materialIndex;

	unsigned int bakeMapIndex;
	BakeMapType bakeMapType;
	unsigned int  bakeMapUVIndex;

	int cameraInvisible;
	// Cryptomatte float id (host-computed murmur3 of the object name)
	float cryptoID;

	// Light linking: receiver accept mask (a light with mask L lights
	// this object iff L == 0 || (L & linkAcceptMask) != 0)
#if defined(SLG_OPENCL_KERNEL)
	ulong linkAcceptMask;
#else
	u_longlong linkAcceptMask;
#endif
} SceneObject;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

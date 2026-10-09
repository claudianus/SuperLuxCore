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

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/format.hpp>
#include <array>
#include <optional>
#include <typeinfo>
#include <unordered_map>

#include "luxrays/core/color/color.h"
#include "luxrays/usings.h"
#include "slg/materials/material.h"
#include "slg/materials/materialdefs.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/scene/scene.h"
#include "slg/core/sphericalfunction/sphericalfunction.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/textures/texturedefs.h"
#include "slg/textures/constfloat.h"
#include "slg/textures/constfloat3.h"
#include "slg/textures/fresnel/fresnelpreset.h"
#include "slg/textures/fresnel/fresneltexture.h"
#include "slg/textures/normalmap.h"

#include "slg/materials/archglass.h"
#include "slg/materials/carpaint.h"
#include "slg/materials/cloth.h"
#include "slg/materials/glass.h"
#include "slg/materials/glossy2.h"
#include "slg/materials/glossycoating.h"
#include "slg/materials/glossytranslucent.h"
#include "slg/materials/matte.h"
#include "slg/materials/mattetranslucent.h"
#include "slg/materials/metal2.h"
#include "slg/materials/mirror.h"
#include "slg/materials/mix.h"
#include "slg/materials/null.h"
#include "slg/materials/roughglass.h"
#include "slg/materials/roughmatte.h"
#include "slg/materials/roughmattetranslucent.h"
#include "slg/materials/velvet.h"
#include "slg/materials/disney.h"
#include "slg/materials/diffraction.h"
#include "slg/materials/hairmat.h"
#include "slg/materials/openpbr.h"
#include "slg/materials/twosided.h"
#include "slg/volumes/homogenous.h"

#include "slg/textures/math/scale.h"
#include "slg/textures/math/subtract.h"
#include "slg/textures/math/divide.h"

#include "slg/textures/texture.h"
#include "slg/usings.h"
#include "slg/utils/filenameresolver.h"

using namespace std;
using namespace luxrays;
using namespace slg;

namespace slg {
atomic<u_int> defaultMaterialIDIndex(0);
}

void Scene::ParseMaterials(const Properties &props) {
	vector<string> matKeys = props.GetAllUniqueSubNames("scene.materials");
	if (matKeys.size() == 0) {
		// There are not material definitions
		return;
	}

	// Cache isLightSource values before we go deleting materials (required for
	// updating mix material)
	std::unordered_map<const Material * , bool> cachedIsLightSource;

	for(const string &key: matKeys) {
		const string matName = Property::ExtractField(key, 2);
		if (matName == "")
			throw runtime_error("Syntax error in material definition: " + matName);

		if (matDefs->IsMaterialDefined(matName)) {
			auto& oldMat = matDefs->GetMaterial(matName);
			cachedIsLightSource[&oldMat] = oldMat.IsLightSource();
		}
	}

	// Now I can update the materials
	for(const string &key: matKeys) {
		// Extract the material name
		const string matName = Property::ExtractField(key, 2);
		if (matName == "")
			throw runtime_error("Syntax error in material definition: " + matName);

		if (matDefs->IsMaterialDefined(matName)) {
			SDL_LOG("Material re-definition: " << matName);
		} else {
			SDL_LOG("Material definition: " << matName);
		}

		// In order to have harlequin colors with MATERIAL_ID output
		const u_int index = defaultMaterialIDIndex++;
		const u_int matID = ((u_int)(RadicalInverse(index + 1, 2) * 255.f + .5f)) |
				(((u_int)(RadicalInverse(index + 1, 3) * 255.f + .5f)) << 8) |
				(((u_int)(RadicalInverse(index + 1, 5) * 255.f + .5f)) << 16);
		auto newMat = CreateMaterial(matID, matName, props);

		if (matDefs->IsMaterialDefined(matName)) {
			//// A replacement for an existing material
			//auto& oldMat = matDefs->GetMaterial(matName);

			// Add to material list
			auto [newMatRef, oldMatPtr] = matDefs->DefineMaterial(std::move(newMat));
			auto& oldMatRef = *oldMatPtr;

			// The mat should not be a volume: let's check it
			try {
				dynamic_cast<const Volume&>(oldMatRef);
				throw runtime_error(
					"You can not replace a material with the volume: " + matName
				);
			} catch(std::bad_cast&) {
			}

			// If old material was emitting light, delete all TriangleLight
			if (cachedIsLightSource[&oldMatRef])
				lightDefs->DeleteLightSourceByMaterial(oldMatRef);

			// Replace old material direct references with new one
			objDefs->UpdateMaterialReferences(oldMatRef, newMatRef);

			// If new material is emitting light, create all TriangleLight
			if (newMatRef.IsLightSource())
				objDefs->DefineIntersectableLights(*lightDefs, newMatRef);

			// Check if the old material was or the new material is a light source
			if (cachedIsLightSource[&oldMatRef] || newMatRef.IsLightSource())
				editActions.AddActions(LIGHTS_EDIT | LIGHT_TYPES_EDIT);

			moveToTrash(std::move(oldMatPtr));
		} else {

			// Only a new Material
			auto [newMatRef, oldMatPtr] = matDefs->DefineMaterial(std::move(newMat));
			assert(!oldMatPtr);
			// Check if the new material is a light source
			if (newMatRef.IsLightSource())
				editActions.AddActions(LIGHTS_EDIT | LIGHT_TYPES_EDIT);

		}
	}

	editActions.AddActions(MATERIALS_EDIT | MATERIAL_TYPES_EDIT);
}

// OpenPBR "subsurfacepreset" measured-media table. (sigma_a, sigma_s')
// measured at RGB primaries (Jensen et al. SIGGRAPH'01 Table III, as
// tabulated in PBRT-v3 medium.cpp) converted offline to the
// albedo-parametrized form the CB15 implicit interior volume consumes:
//   albedo   = apparent diffuse reflectance incl. internal-Fresnel
//              trapping at the medium's canonical IOR,
//   mfpScale = per-channel transport mfp (1/sigma_t') normalized to the
//              strongest channel,
//   radius   = that strongest-channel mfp in meters (scene units).
// Generated by dev-tools/gen_sss_presets.py - do not hand-edit.
struct SSSPreset {
	const char *name;
	float albedo[3], mfpScale[3], radius;
};
static constexpr SSSPreset sssPresets[] = {
	{"skin_dark", {0.468349f, 0.288709f, 0.191387f}, {1.f, 0.735238f, 0.518121f}, 0.00129534f},  // Jensen'01 Skin1, eta=1.4
	{"skin_light", {0.632232f, 0.466064f, 0.390088f}, {1.f, 0.664458f, 0.570026f}, 0.000906618f},  // Jensen'01 Skin2, eta=1.4
	{"chicken", {0.364783f, 0.218475f, 0.186212f}, {1.f, 0.574913f, 0.289474f}, 0.00606061f},  // Jensen'01 Chicken1, eta=1.4
	{"apple", {0.85486f, 0.849494f, 0.563993f}, {0.879198f, 0.842316f, 1.f}, 0.000496032f},  // Jensen'01 Apple, eta=1.35
	{"potato", {0.778643f, 0.639909f, 0.2793f}, {0.981829f, 0.944993f, 1.f}, 0.00149254f},  // Jensen'01 Potato, eta=1.35
	{"marble", {0.845639f, 0.80993f, 0.77501f}, {1.f, 0.835372f, 0.728975f}, 0.000456184f},  // Jensen'01 Marble, eta=1.5
	{"cream", {0.976865f, 0.905168f, 0.742083f}, {0.429026f, 0.578552f, 1.f}, 0.000315826f},  // Jensen'01 Cream, eta=1.35
	{"whole_milk", {0.91241f, 0.887153f, 0.774016f}, {1.f, 0.794141f, 0.674181f}, 0.000391988f},  // Jensen'01 Wholemilk, eta=1.35
	{"skim_milk", {0.825499f, 0.823673f, 0.703239f}, {1.f, 0.573742f, 0.366419f}, 0.00142572f},  // Jensen'01 Skimmilk, eta=1.35
	{"ketchup", {0.229273f, 0.0125948f, 0.00370183f}, {1.f, 0.231731f, 0.162838f}, 0.00414938f},  // Jensen'01 Ketchup, eta=1.35
};

static const SSSPreset *FindSSSPreset(const string &name) {
	for (const auto &p : sssPresets)
		if (name == p.name)
			return &p;
	return nullptr;
}

MaterialUPtr Scene::CreateMaterial(
	const u_int defaultMatID, const string &matName, const Properties &props
) {
	// A few helpful constants
	const string propName = "scene.materials." + matName;
	static constexpr auto nullTex = TextureConstPtr(nullptr);
	const auto zeroSpectrum = Spectrum(0.f);
	const auto oneSpectrum = Spectrum(1.f);

	// A few helpers to facilitate texture property parsing:

	// Parse required texture
	auto parseTex = [&](
		const std::string_view suffix,
		Spectrum defaultVal
	) -> TexturePtr {
		assert(suffix[0] != '.');
		auto& tex = GetTexture(
			props.Get(Property(propName + "." + std::string(suffix))(defaultVal))
		);

		return TexturePtr(std::addressof(tex));
	};

	// Parse optional texture
	auto parseTextureConstPtr = [&](  // Parse optional texture
		const std::string suffix,
		Spectrum defaultVal,
		TextureConstPtr fallbackTex=TextureConstPtr(nullptr)
	) -> TextureConstPtr {
		assert(suffix[0] != '.');
		return props.IsDefined(propName + "." + suffix) ?
		TexturePtr(
			&GetTexture(props.Get(Property(propName + "." + suffix)(defaultVal)))
		) :
		fallbackTex;
	};

	// Parse float
	auto parseFloat = [&](
		const std::string_view suffix,
		float defaultVal
	) -> float {
		assert(suffix[0] != '.');
		return props.Get(Property(propName + "." + std::string(suffix))(defaultVal))
			.Get<double>();
	};

	// Parse boolean
	auto parseBool = [&](
		const std::string_view suffix,
		bool defaultVal
	) -> bool {
		assert(suffix[0] != '.');
		return props.Get(Property(propName + "." + std::string(suffix))(defaultVal))
			.Get<bool>();
	};

	// Parse string
	auto parseString = [&](
		const std::string_view suffix,
		std::string defaultVal
	) -> std::string {
		assert(suffix[0] != '.');
		return props.Get(Property(propName + "." + std::string(suffix))(defaultVal))
			.Get<std::string>();
	};

	// Check whether texture is defined
	auto isDefined = [&](const std::string_view suffix) -> bool {
		assert(suffix[0] != '.');
		return props.IsDefined(propName + "." + std::string(suffix));
	};

	// Warn for deprecated property
	auto warnDeprecated = [&](std::string_view suffix) {
		assert(suffix[0] != '.');
		SLG_LOG("WARNING: deprecated property " + propName + "." + std::string(suffix));
	};

	// Implicit const-float3 texture for generated values
	auto constTex3 = [&](const Spectrum &v) -> TextureConstPtr {
		auto tex = std::make_unique<ConstFloat3Texture>(v);
		tex->SetName(NamedObject::GetUniqueName("Implicit-Const"));
		auto [ref, old] = texDefs->DefineTexture(std::move(tex));
		moveToTrash(std::move(old));
		return TextureConstPtr(&ref);
	};

	// Sellmeier 3-term dispersion: n^2(l) = 1 + sum_i B_i l^2/(l^2-C_i)
	// (l in um, C in um^2). Enabled by either:
	//   <mat>.sellmeier = "N-BK7"|"N-SF6"|"N-SF10"|"F2"|"fusedsilica"|
	//                     "diamond"|"water"      (named glass presets)
	//   <mat>.sellmeierb = B1 B2 B3  and  <mat>.sellmeierc = C1 C2 C3
	// When present it replaces the Cauchy-B model for that material.
	// Preset sources: Schott catalogue coefficients; Malitson 1965
	// (fused silica); Peter 1923 (diamond); Daimon-Masumura 2007 (water,
	// strongest 3 of 4 terms).
	static const std::unordered_map<string, std::array<float, 6>>
			sellmeierPresets = {
		{"N-BK7",      {1.03961212f, 0.231792344f, 1.01046945f,
						0.00600069867f, 0.0200179144f, 103.560653f}},
		{"N-SF6",      {1.72448482f, 0.390104889f, 1.04572858f,
						0.0134871947f, 0.0569318095f, 118.557185f}},
		{"N-SF10",     {1.61625977f, 0.259229334f, 1.07762317f,
						0.0127534559f, 0.0581983954f, 116.607680f}},
		{"F2",         {1.34533359f, 0.209073176f, 0.937357162f,
						0.00997743871f, 0.0470450767f, 111.886764f}},
		{"fusedsilica",{0.6961663f, 0.4079426f, 0.8974794f,
						0.004679148f, 0.013512063f, 97.9340025f}},
		{"diamond",    {0.3306f, 4.3356f, 0.f,
						0.030625f, 0.011236f, 1.f}},
		{"water",      {0.5684027565f, 0.1726177391f, 0.1130748688f,
						0.0051018297f, 0.0182115394f, 10.69792721f}},
	};
	auto parseSellmeier = [&](TextureConstPtr &sellB, TextureConstPtr &sellC) {
		sellB = nullptr;
		sellC = nullptr;
		if (isDefined("sellmeier")) {
			const string name = parseString("sellmeier", "");
			const auto it = sellmeierPresets.find(name);
			if (it == sellmeierPresets.end())
				throw std::runtime_error("Unknown Sellmeier glass preset: " + name);
			sellB = constTex3(Spectrum(it->second[0], it->second[1], it->second[2]));
			sellC = constTex3(Spectrum(it->second[3], it->second[4], it->second[5]));
		} else if (isDefined("sellmeierb") && isDefined("sellmeierc")) {
			sellB = parseTex("sellmeierb", {0.f, 0.f, 0.f});
			sellC = parseTex("sellmeierc", {0.f, 0.f, 0.f});
		}
	};

	// PARSING STARTS HERE
	const string matType = parseString("type", "matte");
	// For compatibility with the past
	auto transparencyTex = parseTextureConstPtr("transparency", Spectrum(0.f));

	auto frontTransparencyTex = parseTextureConstPtr(
		"transparency.front", Spectrum(0.f), transparencyTex
	);

	auto backTransparencyTex = parseTextureConstPtr(
		"transparency.back", {Spectrum(0.f)}, transparencyTex
	);

	// Start non-legacy parsing
	auto emissionTex = parseTextureConstPtr("emission", {Spectrum(0.f)});

	// Required to remove light source while editing the scene
	if (emissionTex && (
		((emissionTex->GetType() == CONST_FLOAT) && ((static_cast<const ConstFloatTexture*>(emissionTex.get()))->GetValue() == 0.f)) ||
		((emissionTex->GetType() == CONST_FLOAT3) && ((static_cast<const ConstFloat3Texture*>(emissionTex.get()))->GetColor().Black()))))
	{
		emissionTex = nullTex;
	}

	auto bumpTex = parseTextureConstPtr("bumptex", {1.f});
    if (!bumpTex) {
		auto normalTex = parseTextureConstPtr("normaltex", {1.f});

        if (normalTex) {
			const float scale = std::max(0.f, parseFloat("normaltex.scale", {1.0}));

            auto implBumpTex = std::make_unique<NormalMapTexture>(*normalTex, scale);
			implBumpTex->SetName(NamedObject::GetUniqueName("Implicit-NormalMapTexture"));

			auto [newTexRef, oldTexPtr] = texDefs->DefineTexture(std::move(implBumpTex));
            bumpTex = TextureConstPtr(&newTexRef);
			moveToTrash(std::move(oldTexPtr));
        }
    }

    const float bumpSampleDistance = parseFloat("bumpsamplingdistance", {.001f});

	MaterialUPtr mat;
	if (matType == "matte") {
		auto kd = parseTex("kd", {.75f, .75f, .75f});

		mat = std::make_unique<MatteMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex, kd
		);
	} else if (matType == "roughmatte") {
		auto kd = parseTex("kd", {.75f, .75f, .75f});
		auto sigma = parseTex("sigma", {.75f, .75f, .75f});

		mat = std::make_unique<RoughMatteMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex, kd, sigma
		);
	} else if (matType == "mirror") {
		auto kr = parseTex("kr", {1.f, 1.f, 1.f});

		mat = std::make_unique<MirrorMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex, kr
		);
	} else if (matType == "glass") {
		auto kr = parseTex("kr", {1.f, 1.f, 1.f});
		auto kt = parseTex("kt", {1.f, 1.f, 1.f});

		TextureConstPtr exteriorIor = nullptr;
		TextureConstPtr interiorIor = nullptr;
		// For compatibility with the past
		if (isDefined("ioroutside")) {
			warnDeprecated("ioroutside");
			exteriorIor = parseTex("ioroutside", 1.f);
		} else if (isDefined("exteriorior"))
			exteriorIor = parseTex("exteriorior", 1.f);
		// For compatibility with the past
		if (isDefined("iorinside")) {
			warnDeprecated("iorinside");
			interiorIor = parseTex("iorinside", 1.5f);
		} else if (isDefined("interiorior"))
			interiorIor = parseTex("interiorior", 1.5f);

		TextureConstPtr cauchyB = nullptr;
		if (isDefined("cauchyb"))
			cauchyB = parseTex("cauchyb", {0.f, 0.f, 0.f});
		// For compatibility with the past
		else if (isDefined("cauchyc")) {
			warnDeprecated("cauchyc");
			cauchyB = parseTex("cauchyc", {0.f, 0.f, 0.f});
		}

		TextureConstPtr filmThickness = nullptr;
		if (isDefined("filmthickness"))
			filmThickness = parseTex("filmthickness", {0.f});

		TextureConstPtr filmIor = nullptr;
		if (isDefined("filmior"))
			filmIor = parseTex("filmior", {1.5f});

		TextureConstPtr sellB, sellC;
		parseSellmeier(sellB, sellC);

		mat = std::make_unique<GlassMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex, kr, kt,
			exteriorIor, interiorIor, cauchyB, sellB, sellC, filmThickness, filmIor
		);
	} else if (matType == "archglass") {
		auto kr = parseTex("kr", {1.f, 1.f, 1.f});
		auto kt = parseTex("kt", {1.f, 1.f, 1.f});

		TextureConstPtr exteriorIor = nullptr;
		TextureConstPtr interiorIor = nullptr;
		// For compatibility with the past
		if (isDefined("ioroutside")) {
			warnDeprecated("ioroutside");
			exteriorIor = parseTex("ioroutside", {1.f});
		} else if (isDefined("exteriorior"))
			exteriorIor = parseTex("exteriorior", {1.f});
		// For compatibility with the past
		if (isDefined("iorinside")) {
			warnDeprecated("iorinside");
			interiorIor = parseTex("iorinside", {1.f});
		} else if (isDefined("interiorior"))
			interiorIor = parseTex("interiorior", {1.f});

		TextureConstPtr filmThickness = nullptr;
		if (isDefined("filmthickness"))
			filmThickness = parseTex("filmthickness", {0.f});

		TextureConstPtr filmIor = nullptr;
		if (isDefined("filmior"))
			filmIor = parseTex("filmior", {1.5f});

		mat = std::make_unique<ArchGlassMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kr, kt, exteriorIor, interiorIor, filmThickness, filmIor
		);
	} else if (matType == "mix") {
		auto& matA = matDefs->GetMaterial(parseString("material1", "mat1"));
		auto& matB = matDefs->GetMaterial(parseString("material2", "mat2"));
		auto mix = parseTex("amount", {.5f});

		auto mixMat = std::make_unique<MixMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			matA, matB, mix, parseBool("additive", false)
		);

		// Check if there is a loop in Mix material definition
		// (Note: this can not really happen at the moment because forward
		// declarations are not supported)
		if (mixMat->IsReferencing(*mixMat))
			throw runtime_error("There is a loop in Mix material definition: " + matName);

		mat = std::move(mixMat);
	} else if (matType == "null") {
		mat = std::make_unique<NullMaterial>(frontTransparencyTex, backTransparencyTex);
	} else if (matType == "mattetranslucent") {
		auto kr = parseTex("kr", {.5f, .5f, .5f});
		auto kt = parseTex("kt", {.5f, .5f, .5f});

		mat = std::make_unique<MatteTranslucentMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kr, kt
		);
	} else if (matType == "roughmattetranslucent") {
		auto kr = parseTex("kr", {.5f, .5f, .5f});
		auto kt = parseTex("kt", {.5f, .5f, .5f});
		auto sigma = parseTex("sigma", {0.f});

		mat = std::make_unique<RoughMatteTranslucentMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kr, kt, sigma
		);
	} else if (matType == "glossy2") {
		auto kd = parseTex("kd", {.5f, .5f, .5f});
		auto ks = parseTex("ks", {.5f, .5f, .5f});
		auto nu = parseTex("uroughness", {.1f});
		auto nv = parseTex("vroughness", {.1f});
		auto ka = parseTex("ka", {0.f, 0.f, 0.f});
		auto d = parseTex("d", {0.f});
		auto index = parseTex("index", {0.f, 0.f, 0.f});
		const auto multibounce = parseBool("multibounce", false);
		const auto doublesided = parseBool("doublesided", false);
		// Opt-in modern microfacet distribution (default: legacy Schlick)
		const auto useGgx = parseString("distribution", "schlick") == "ggx";

		mat = std::make_unique<Glossy2Material>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kd, ks, nu, nv, ka, d, index, multibounce, doublesided, useGgx
		);
	} else if (matType == "metal2") {
		auto nu = parseTex("uroughness", {.1f});
		auto nv = parseTex("vroughness", {.1f});
		const auto multibounce = parseBool("multibounce", false);
		const auto useGgx = parseString("distribution", "schlick") == "ggx";
		// Optional F82-style edge tint: Gulbrandsen-refits (n,k) so the
		// grazing tint stays consistent for single- and multi-bounce.
		auto edgeTint = isDefined("edgetint") ? parseTex("edgetint", {1.f, 1.f, 1.f}) : TextureConstPtr();

		TextureConstPtr n, k;
		if (isDefined("preset") || isDefined("name")) {
			FresnelTextureUPtr presetTex = AllocFresnelPresetTex(props, propName);
			const auto texname = NamedObject::GetUniqueName(matName + "-Implicit-FresnelPreset");
			presetTex->SetName(texname);
			auto [newTexRef, oldTexPtr] = texDefs->DefineTexture(std::move(presetTex));
			auto refpreset = FresnelTextureConstPtr(
				dynamic_cast<const FresnelTexture *>(std::addressof(newTexRef))
			);

			mat = std::make_unique<Metal2Material>(
				frontTransparencyTex,
				backTransparencyTex,
				emissionTex,
				bumpTex,
				refpreset,
				nu,
				nv,
				multibounce,
				useGgx,
				edgeTint
			);
			moveToTrash(std::move(oldTexPtr));
		} else if (isDefined("fresnel")) {
			auto tex = parseTex("fresnel", {5.f});
			if (!dynamic_cast<const FresnelTexture *>(tex.get()))
				throw runtime_error(
					"Metal2 fresnel property requires a fresnel texture: " + matName
				);

			auto fresnelTex = static_cast<const FresnelTexture *>(tex.get());
			mat = std::make_unique<Metal2Material>(
				frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
				FresnelTextureConstPtr(fresnelTex), nu, nv, multibounce, useGgx,
				edgeTint
			);
		} else {
			n = parseTex("n", {.5f, .5f, .5f});
			k = parseTex("k", {.5f, .5f, .5f});
			mat = std::make_unique<Metal2Material>(
				frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
				n, k, nu, nv, multibounce, useGgx, edgeTint
			);
		}
	} else if (matType == "roughglass") {
		auto kr = parseTex("kr", {1.f, 1.f, 1.f});
		auto kt = parseTex("kt", {1.f, 1.f, 1.f});

		TextureConstPtr exteriorIor = nullptr;
		TextureConstPtr interiorIor = nullptr;
		// For compatibility with the past
		if (isDefined("ioroutside")) {
			warnDeprecated("ioroutside");
			exteriorIor = parseTex("ioroutside", {1.f});
		} else if (isDefined("exteriorior"))
			exteriorIor = parseTex("exteriorior", {1.f});
		// For compatibility with the past
		if (isDefined("iorinside")) {
			warnDeprecated("iorinside");
			interiorIor = parseTex("iorinside", {1.5f});
		} else if (isDefined("interiorior"))
			interiorIor = parseTex("interiorior", {1.5f});

		auto nu = parseTex("uroughness", {.1f});
		auto nv = parseTex("vroughness", {.1f});

		TextureConstPtr cauchyB = nullptr;
		if (isDefined("cauchyb"))
			cauchyB = parseTex("cauchyb", {0.f, 0.f, 0.f});

		TextureConstPtr filmThickness = nullptr;
		if (isDefined("filmthickness"))
			filmThickness = parseTex("filmthickness", {0.f});

		TextureConstPtr filmIor = nullptr;
		if (isDefined("filmior"))
			filmIor = parseTex("filmior", {1.5f});

		const auto useGgx = parseString("distribution", "schlick") == "ggx";
		const bool multibounce = parseBool("multibounce", false);
		TextureConstPtr sellB, sellC;
		parseSellmeier(sellB, sellC);
		mat = std::make_unique<RoughGlassMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kr, kt, exteriorIor, interiorIor, nu, nv, cauchyB, sellB, sellC,
			filmThickness, filmIor, multibounce, useGgx
		);
	} else if (matType == "velvet") {
		auto kd = parseTex("kd", {.5f, .5f, .5f});
		auto p1 = parseTex("p1", {-2.0f});
		auto p2 = parseTex("p2", {20.0f});
		auto p3 = parseTex("p3", {2.0f});
		auto thickness = parseTex("thickness", {0.1f});
		const string model = parseString("model", "legacy");
		const bool useCharlie = (model == "charlie");
		if (!useCharlie && model != "legacy")
			SLG_LOG("WARNING: unknown velvet model '" << model << "', using legacy");
		// Charlie mode requires the texture (legacy mode ignores it)
		auto sheenRoughness = parseTex("sheenroughness", {0.5f});

		mat = std::make_unique<VelvetMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kd, p1, p2, p3, thickness, sheenRoughness, useCharlie
		);
	} else if (matType == "cloth") {
		slg::ocl::ClothPreset preset = slg::ocl::DENIM;

		if (isDefined("preset")) {
			const string type = parseString("preset", "denim");

			if (type == "denim")
				preset = slg::ocl::DENIM;
			else if (type == "silk_charmeuse")
				preset = slg::ocl::SILKCHARMEUSE;
			else if (type == "silk_shantung")
				preset = slg::ocl::SILKSHANTUNG;
			else if (type == "cotton_twill")
				preset = slg::ocl::COTTONTWILL;
			// "Gabardine" was misspelled in the past, ensure backwards-compatibility (fixed in v2.2)
			else if (type == "wool_gabardine" || type == "wool_garbardine")
				preset = slg::ocl::WOOLGABARDINE;
			else if (type == "polyester_lining_cloth")
				preset = slg::ocl::POLYESTER;
		}
		auto weft_kd = parseTex("weft_kd", {.5f, .5f, .5f});
		auto weft_ks = parseTex("weft_ks", {.5f, .5f, .5f});
		auto warp_kd = parseTex("warp_kd", {.5f, .5f, .5f});
		auto warp_ks = parseTex("warp_ks", {.5f, .5f, .5f});
		const float repeat_u = parseFloat("repeat_u", 100.0f);
		const float repeat_v = parseFloat("repeat_v", 100.0f);

		mat = std::make_unique<ClothMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			preset, weft_kd, weft_ks, warp_kd, warp_ks, repeat_u, repeat_v
		);
	} else if (matType == "carpaint") {
		auto ka = parseTex("ka", {0.f, 0.f, 0.f});
		auto d = parseTex("d", {0.f});
		const auto useGgx = parseString("distribution", "schlick") == "ggx";
		const bool multibounce = parseBool("multibounce", false);

		string preset = parseString("preset", "");
		if (preset != "") {
			const int numPaints = CarPaintMaterial::NbPresets();
			int i;
			for (i = 0; i < numPaints; ++i) {
				if (preset == CarPaintMaterial::data[i].name)
					break;
			}

			if (i == numPaints)
				preset = "";
			else {
				auto& cpData = CarPaintMaterial::data;
				auto& kd = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-kd"))
					(cpData[i].kd[0], cpData[i].kd[1], cpData[i].kd[2]));
				auto& ks1 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-ks1"))
					(cpData[i].ks1[0], cpData[i].ks1[1], cpData[i].ks1[2]));
				auto& ks2 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-ks2"))
					(cpData[i].ks2[0], cpData[i].ks2[1], cpData[i].ks2[2]));
				auto& ks3 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-ks3"))
					(cpData[i].ks3[0], cpData[i].ks3[1], cpData[i].ks3[2]));
				auto& r1 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-r1"))
					(cpData[i].r1));
				auto& r2 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-r2"))
					(cpData[i].r2));
				auto& r3 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-r3"))
					(cpData[i].r3));
				auto& m1 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-m1"))
					(cpData[i].m1));
				auto& m2 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-m2"))
					(cpData[i].m2));
				auto& m3 = GetTexture(Property(NamedObject::GetUniqueName(matName + "-Implicit-" + preset + "-m3"))
					(cpData[i].m3));
				mat = std::make_unique<CarPaintMaterial>(
					frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
					TextureConstPtr(&kd),
					TextureConstPtr(&ks1),
					TextureConstPtr(&ks2),
					TextureConstPtr(&ks3),
					TextureConstPtr(&m1),
					TextureConstPtr(&m2),
					TextureConstPtr(&m3),
					TextureConstPtr(&r1),
					TextureConstPtr(&r2),
					TextureConstPtr(&r3),
					ka,
					d,
					multibounce,
					useGgx
				);
			}
		}

		// preset can be reset above if the name is not found
		if (preset == "") {
			auto& cpData = CarPaintMaterial::data[0];
			auto kd = parseTex( "kd", {cpData.kd[0], cpData.kd[1], cpData.kd[2]});
			auto ks1 = parseTex("ks1", {cpData.ks1[0], cpData.ks1[1], cpData.ks1[2]});
			auto ks2 = parseTex("ks2", {cpData.ks2[0], cpData.ks2[1], cpData.ks2[2]});
			auto ks3 = parseTex("ks3", {cpData.ks3[0], cpData.ks3[1], cpData.ks3[2]});
			auto r1 = parseTex("r1", {cpData.r1});
			auto r2 = parseTex("r2", {cpData.r2});
			auto r3 = parseTex("r3", {cpData.r3});
			auto m1 = parseTex("m1", {cpData.m1});
			auto m2 = parseTex("m2", {cpData.m2});
			auto m3 = parseTex("m3", {cpData.m3});
			mat = std::make_unique<CarPaintMaterial>(
				frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
				kd, ks1, ks2, ks3, m1, m2, m3, r1, r2, r3, ka, d, multibounce, useGgx
			);
		}
	} else if (matType == "glossytranslucent") {
		auto kd = parseTex("kd", {.5f, .5f, .5f});
		auto kt = parseTex("kt", {.5f, .5f, .5f});
		auto ks = parseTex("ks", {.5f, .5f, .5f});
		auto ks_bf = parseTex("ks_bf", {.5f, .5f, .5f});
		auto nu = parseTex("uroughness", {.1f});
		auto nu_bf = parseTex("uroughness_bf", {.1f});
		auto nv = parseTex("vroughness", {.1f});
		auto nv_bf = parseTex("vroughness_bf", {.1f});
		auto ka = parseTex("ka", {0.f, 0.f, 0.f});
		auto ka_bf = parseTex("ka_bf", {0.f, 0.f, 0.f});
		auto d = parseTex("d", {0.f});
		auto d_bf = parseTex("d_bf", {0.f});
		auto index = parseTex("index", {0.f, 0.f, 0.f});
		auto index_bf = parseTex("index_bf", {0.f, 0.f, 0.f});
		const bool multibounce = parseBool("multibounce", false);
		const bool multibounce_bf = parseBool("multibounce_bf", false);
		const auto useGgx = parseString("distribution", "schlick") == "ggx";

		mat = std::make_unique<GlossyTranslucentMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kd, kt, ks, ks_bf, nu, nu_bf, nv, nv_bf,
			ka, ka_bf, d, d_bf, index, index_bf, multibounce, multibounce_bf,
			useGgx
		);
	} else if (matType == "glossycoating") {
		MaterialConstRef matBase = matDefs->GetMaterial(parseString("base", ""));
		auto ks = parseTex("ks", {.5f, .5f, .5f});
		auto nu = parseTex("uroughness", {.1f});
		auto nv = parseTex("vroughness", {.1f});
		auto ka = parseTex("ka", {0.f, 0.f, 0.f});
		auto d = parseTex("d", {0.f});
		auto index = parseTex("index", {0.f, 0.f, 0.f});
		const bool multibounce = parseBool("multibounce", false);
		const auto useGgx = parseString("distribution", "schlick") == "ggx";

		mat = std::make_unique<GlossyCoatingMaterial>(
			frontTransparencyTex,
			backTransparencyTex,
			emissionTex,
			bumpTex,
			MaterialConstPtr(&matBase),
			ks,
			nu,
			nv,
			ka,
			d,
			index,
			multibounce,
			useGgx
		);
	} else if (matType == "disney") {
		auto baseColor = parseTex("basecolor", {.5f, .5f, .5f});
		auto subsurface = parseTex("subsurface", {0.f});
		auto roughness = parseTex("roughness", {0.f});
		auto metallic = parseTex("metallic", {0.f});
		auto specular = parseTex("specular", {0.f});
		auto specularTint = parseTex("speculartint", {0.f});
		auto clearcoat = parseTex("clearcoat", {0.f});
		auto clearcoatGloss = parseTex("clearcoatgloss", {0.f});
		auto anisotropic = parseTex("anisotropic", {0.f});
		auto sheen = parseTex("sheen", {0.f});
		auto sheenTint = parseTex("sheentint", {0.f});
		// Estevez-Kulla "Charlie" microfacet sheen (opt-in): when the
		// sheenroughness texture is present the legacy Schlick sheen lobe
		// is replaced by the physical cloth sheen BRDF.
		TextureConstPtr sheenRoughness = nullptr;
		if (isDefined("sheenroughness"))
			sheenRoughness = parseTex("sheenroughness", {0.5f});
		// Integrated dielectric transmission lobe (Principled-style). When
		// transmission > 0 the material refracts instead of only reflecting.
		auto transmission = parseTex("transmission", {0.f});
		// Defaults to the base roughness when not specified
		TextureConstPtr transmissionRoughness = nullptr;
		if (isDefined("transmissionroughness"))
			transmissionRoughness = parseTex("transmissionroughness", {0.f});
		auto ior = parseTex("ior", {1.5f});
		TextureConstPtr cauchyB = nullptr;
		if (isDefined("cauchyb"))
			cauchyB = parseTex("cauchyb", {0.f});

		TextureConstPtr filmAmount = nullptr;
		if (isDefined("filmamount"))
			filmAmount = parseTex("filmamount", {1.f});

		TextureConstPtr filmThickness = nullptr;
		if (isDefined("filmthickness"))
			filmThickness = parseTex("filmthickness", {0.f});

		TextureConstPtr filmIor = nullptr;
		if (isDefined("filmior"))
			filmIor = parseTex("filmior", {1.5f});

		TextureConstPtr sellB, sellC;
		parseSellmeier(sellB, sellC);
		const bool multibounce = parseBool("multibounce", false);
		mat = std::make_unique<DisneyMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			baseColor, subsurface, roughness, metallic,
			specular, specularTint, clearcoat, clearcoatGloss, anisotropic,
			sheen, sheenTint, sheenRoughness, filmAmount, filmThickness, filmIor,
			transmission, transmissionRoughness, ior, cauchyB, sellB, sellC,
			multibounce
		);
	} else if (matType == "openpbr") {
		// ASWF OpenPBR Surface v1.1 (lobe-mixture approximation)
		auto baseColor = parseTex("basecolor", {.8f, .8f, .8f});
		auto baseWeight = parseTex("baseweight", {1.f});
		auto baseMetalness = parseTex("basemetalness", {0.f});
		auto baseDiffuseRoughness = parseTex("basediffuseroughness", {0.f});
		auto specWeight = parseTex("specularweight", {1.f});
		auto specColor = parseTex("specularcolor", {1.f, 1.f, 1.f});
		auto specRoughness = parseTex("specularroughness", {.3f});
		auto specAniso = parseTex("specularanisotropy", {0.f});
		auto specRotation = parseTex("specularrotation", {0.f});
		auto specIor = parseTex("specularior", {1.5f});
		auto transWeight = parseTex("transmissionweight", {0.f});
		auto transColor = parseTex("transmissioncolor", {1.f, 1.f, 1.f});
		auto transDepth = parseTex("transmissiondepth", {0.f});
		auto transScatter = parseTex("transmissionscatter", {0.f, 0.f, 0.f});
		auto transScatterAniso = parseTex("transmissionscatteranisotropy", {0.f});
		auto dispersion = parseTex("dispersion", {0.f});
		TextureConstPtr sellB, sellC;
		parseSellmeier(sellB, sellC);
		auto sssWeight = parseTex("subsurfaceweight", {0.f});
		// Measured-media presets supply the albedo-parametrized SSS
		// defaults; explicitly defined properties always win, so e.g.
		// preset + an albedo texture yields measured radii on an artist
		// albedo.
		const SSSPreset *sssPreset = nullptr;
		if (isDefined("subsurfacepreset")) {
			const string sssPresetName = parseString("subsurfacepreset", "");
			sssPreset = FindSSSPreset(sssPresetName);
			if (!sssPreset)
				throw runtime_error("Unknown " + propName +
						".subsurfacepreset: " + sssPresetName);
		}
		auto sssColor = parseTex("subsurfacecolor",
				sssPreset ? Spectrum(sssPreset->albedo) : Spectrum(1.f));
		auto sssRadius = parseTex("subsurfaceradius",
				{sssPreset ? sssPreset->radius : 1.f});
		auto sssRadiusScale = parseTex("subsurfaceradiusscale",
				sssPreset ? Spectrum(sssPreset->mfpScale) : Spectrum(1.f));
		auto sssAniso = parseTex("subsurfaceanisotropy", {0.f});
		auto coatWeight = parseTex("coatweight", {0.f});
		auto coatColor = parseTex("coatcolor", {1.f, 1.f, 1.f});
		auto coatRoughness = parseTex("coatroughness", {0.f});
		auto coatAniso = parseTex("coatanisotropy", {0.f});
		auto coatRotation = parseTex("coatrotation", {0.f});
		auto coatIor = parseTex("coatior", {1.5f});
		auto coatDarkening = parseTex("coatdarkening", {1.f});
		auto fuzzWeight = parseTex("fuzzweight", {0.f});
		auto fuzzColor = parseTex("fuzzcolor", {1.f, 1.f, 1.f});
		auto fuzzRoughness = parseTex("fuzzroughness", {.5f});
		auto filmWeight = parseTex("filmweight", {0.f});
		auto filmThickness = parseTex("filmthickness", {0.f});
		auto filmIor = parseTex("filmior", {1.5f});

		mat = std::make_unique<OpenPBRMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			baseColor, baseWeight, baseMetalness, baseDiffuseRoughness,
			specWeight, specColor, specRoughness, specAniso,
			specRotation, specIor,
			transWeight, transColor, transDepth, transScatter,
			transScatterAniso, dispersion, sellB, sellC,
			sssWeight, sssColor, sssRadius, sssRadiusScale, sssAniso,
			coatWeight, coatColor, coatRoughness, coatAniso,
			coatRotation, coatIor, coatDarkening,
			fuzzWeight, fuzzColor, fuzzRoughness,
			filmWeight, filmThickness, filmIor
		);
		auto *openpbr = static_cast<OpenPBRMaterial *>(mat.get());
		openpbr->SetCoatAffectsBaseIor(parseBool("coataffectsbaseior", true));
		openpbr->SetCyclesNormalSemantics(parseBool("cyclesnormalsemantics", false));
		openpbr->SetReflectionNormalCorrection(parseBool("reflectionnormalcorrection", false));
		if (isDefined("coatnormal")) openpbr->SetCoatNormal(parseTex("coatnormal", {0.f, 0.f, 1.f}));

		// Implicit interior volume: subsurface scattering (dense medium)
		// wins over the transmission medium when both are active - the
		// refracted ray then also scatters, matching the OpenPBR model
		// where the interior medium IS the subsurface medium.
		//   SSS:        albedo-parametrized volume - the volume inverts
		//               subsurface_color + mfp (= radius*radiusscale) to
		//               sigma_a/sigma_s internally (d'Eon, "A Hitchhiker's
		//               Guide to Multiple Scattering" Eq. 53.7), so
		//               subsurface_color reads as the diffuse surface albedo.
		//   Transmiss.: sigma_a = (1-transmission_color)/depth,
		//               sigma_s = transmission_scatter/depth
		// Skipped when the user sets an explicit volume.interior.
		const bool wantSSSVol = isDefined("subsurfaceweight") &&
				(isDefined("subsurfaceradius") || sssPreset);
		const bool wantTransVol = isDefined("transmissiondepth") &&
				parseFloat("transmissiondepth", 0.f) > 0.f;
		if (!props.IsDefined(propName + ".volume.interior") &&
				(wantSSSVol || wantTransVol)) {
			auto defineTex = [&](TextureUPtr tex) -> TexturePtr {
				tex->SetName(NamedObject::GetUniqueName("Implicit-OpenPBRVolTex"));
				auto [ref, old] = texDefs->DefineTexture(std::move(tex));
				moveToTrash(std::move(old));
				return TexturePtr(&ref);
			};
			auto constC = [&](const Spectrum &v) {
				return defineTex(std::make_unique<ConstFloat3Texture>(v));
			};
			auto mul = [&](TexturePtr a, TexturePtr b) {
				return defineTex(std::make_unique<ScaleTexture>(*a, *b));
			};
			auto sub1 = [&](TexturePtr a) {
				return defineTex(std::make_unique<SubtractTexture>(
						*constC(oneSpectrum), *a));
			};
			auto div = [&](TexturePtr a, TexturePtr b) {
				return defineTex(std::make_unique<DivideTexture>(*a, *b));
			};

			TexturePtr sigmaA, sigmaS, g;
			TextureConstPtr sssAlbedo = nullptr, sssMfp = nullptr;
			if (wantSSSVol) {
				sigmaA = constC(zeroSpectrum);
				sigmaS = constC(zeroSpectrum);
				sssAlbedo = sssColor;
				sssMfp = mul(sssRadius, sssRadiusScale);
				g = sssAniso;
			} else {
				sigmaA = div(sub1(transColor), transDepth);
				sigmaS = div(transScatter, transDepth);
				g = transScatterAniso;
			}

			// CB15 profile for the SSS path: OpenPBR's subsurface_radius
			// is defined as a diffusion radius (transport-corrected mfp)
			// and the inversion folds in the dielectric boundary
			// reflectance via the volume IOR - closer to the OpenPBR
			// spec than the van de Hulst remap.
			auto vol = std::make_unique<HomogeneousVolume>(
				*specIor, nullptr, *sigmaA, *sigmaS, *g,
				true /* multiScattering */, true /* HG phase */,
				true /* equiangular */, sssAlbedo, sssMfp,
				wantSSSVol ? 1 : 0);
			vol->SetName(NamedObject::GetUniqueName("Implicit-OpenPBRVolume"));
			auto [volRef, oldVol] = matDefs->DefineMaterial(std::move(vol));
			moveToTrash(std::move(oldVol));
			mat->SetInteriorVolume(dynamic_cast<const Volume &>(volRef));
		}
	} else if (matType == "hairmat") {
		// Absorption coefficient (mutually exclusive with color/eumelanin)
		TextureConstPtr sigmaA = nullptr;
		if (isDefined("sigma_a"))
			sigmaA = parseTex("sigma_a", {0.f});
		// Direct dye color (mutually exclusive with sigma_a/melanin)
		TextureConstPtr color = nullptr;
		if (isDefined("color"))
			color = parseTex("color", {0.5f, 0.5f, 0.5f});
		// Melanin concentration model (used when neither of the above is set)
		TextureConstPtr eumelanin = nullptr;
		if (isDefined("eumelanin"))
			eumelanin = parseTex("eumelanin", {1.3f});
		TextureConstPtr pheomelanin = nullptr;
		if (isDefined("pheomelanin"))
			pheomelanin = parseTex("pheomelanin", {0.f});
		auto eta = parseTex("eta", {1.55f});
		auto betaM = parseTex("beta_m", {0.3f});
		auto betaN = parseTex("beta_n", {0.3f});
		auto alpha = parseTex("alpha", {2.f});

		// Scattering model: "chiang" (default) or "huang" (microfacet)
		const string modelName = parseString("model", "chiang");
		HairMaterial::HairModel hairModel;
		if (modelName == "chiang")
			hairModel = HairMaterial::HairModel::CHIANG;
		else if (modelName == "huang")
			hairModel = HairMaterial::HairModel::HUANG;
		else
			throw runtime_error("Unknown hairmat model: " + modelName +
					" (expected 'chiang' or 'huang')");

		// Huang-only parameters (GGX microfacet roughness, elliptical
		// cross-section minor axis, per-lobe energy scales)
		auto roughness = parseTex("roughness", {0.3f});
		auto aspectRatio = parseTex("aspectratio", {1.f});
		const float scaleR = std::clamp(parseFloat("scale_r", 1.f), 0.f, 1.f);
		const float scaleTT = std::clamp(parseFloat("scale_tt", 1.f), 0.f, 1.f);
		const float scaleTRT = std::clamp(parseFloat("scale_trt", 1.f), 0.f, 1.f);

		mat = std::make_unique<HairMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			sigmaA, color, eumelanin, pheomelanin,
			eta, betaM, betaN, alpha,
			roughness, aspectRatio, scaleR, scaleTT, scaleTRT, hairModel
		);
	} else if (matType == "diffraction") {
		// 1D reflective diffraction grating (CDs, holographic foils)
		auto kr = parseTex("kr", {1.f, 1.f, 1.f});
		auto spacing = parseTex("spacing", {1600.f});
		auto roughness = parseTex("roughness", {0.f});
		auto fillFactor = parseTex("fillfactor", {.5f});

		const string orientName = parseString("orientation", "radial");
		DiffractionOrientation orientation;
		if (orientName == "u")
			orientation = DIFFRACTION_U;
		else if (orientName == "v")
			orientation = DIFFRACTION_V;
		else if (orientName == "radialuv")
			orientation = DIFFRACTION_RADIAL_UV;
		else if (orientName == "radial")
			orientation = DIFFRACTION_RADIAL;
		else
			throw runtime_error("Unknown diffraction orientation: " + orientName +
					" (expected u/v/radialuv/radial)");

		const Point center = props.Get(
				Property(propName + ".center")(Point())).Get<Point>();
		const float centerU = std::clamp(parseFloat("centeru", .5f), 0.f, 1.f);
		const float centerV = std::clamp(parseFloat("centerv", .5f), 0.f, 1.f);
		const float blaze = parseFloat("blaze", 0.f) * float(M_PI / 180.0);
		const u_int maxOrder = std::clamp(u_int(std::max(0.f, parseFloat("orders", 8.f))), 1u, 32u);

		mat = std::make_unique<DiffractionMaterial>(
			frontTransparencyTex, backTransparencyTex, emissionTex, bumpTex,
			kr, spacing, roughness, fillFactor, orientation,
			center, centerU, centerV, blaze, maxOrder);
	} else if (matType == "twosided") {
		MaterialConstRef frontMat = matDefs->GetMaterial(parseString("frontmaterial", "front"));
		MaterialConstRef backMat = matDefs->GetMaterial(parseString("backmaterial", "back"));

		auto twoSided = std::make_unique<TwoSidedMaterial>(
			frontTransparencyTex,
			backTransparencyTex,
			emissionTex,
			bumpTex,
			frontMat,
			backMat
		);

		// Check if there is a loop in Two-sided material definition
		// (Note: this can not really happen at the moment because forward
		// declarations are not supported)
		if (twoSided->IsReferencing(*twoSided))
			throw runtime_error("There is a loop in Two-sided material definition: " + matName);

		mat = std::move(twoSided);
	} else
		throw runtime_error("Unknown material type: " + matType);

	mat->SetName(matName);

	mat->SetID(props.Get(Property(propName + ".id")(defaultMatID)).Get<u_int>());
	mat->SetBumpSampleDistance(bumpSampleDistance);
	mat->SetBumpFilterWidth(Max(0.f, parseFloat("bumpfilterwidth", 0.f)));

	// Gain is not really a color so I avoid to use GetColor()
	mat->SetEmittedGain(props.Get(Property(propName + ".emission.gain")(Spectrum(1.f))).Get<Spectrum>());
	mat->SetEmittedPower(std::max(0.f, parseFloat("emission.power", 0.0)));
	mat->SetEmittedPowerNormalize(parseBool("emission.normalizebycolor", true));
	mat->SetEmittedGainNormalize(parseBool("emission.gain.normalizebycolor", false));
	mat->SetEmittedEfficency(
		std::max(
			0.0,
			props.Get(
				std::move(Property(propName + ".emission.efficiency")(0.0)),
				propName + ".emission.efficency"
			)->Get<double>()
		)
	);
	mat->SetEmittedTheta(std::clamp(parseFloat("emission.theta", 90.0), 0.f, 90.f));
	mat->SetEmissionTwoSided(parseBool("emission.twosided", false));
	mat->SetLightID(props.Get(Property(propName + ".emission.id")(0u)).Get<u_int>());
	mat->SetEmittedImportance(parseFloat("emission.importance", 1.0));
	mat->SetEmittedTemperature(parseFloat("emission.temperature", -1.f));
	mat->SetEmittedTemperatureNormalize(parseBool("emission.temperature.normalize", false));

	mat->SetPassThroughShadowTransparency(GetColor(props.Get(Property(propName + ".transparency.shadow")(Spectrum(0.f)))));
	if (props.IsDefined(propName + ".transparency.shadowoverride")){
		mat->SetPassThroughShadowTransparencyOverride(
			parseBool("transparency.shadowoverride", false)
		);
	} else {
		mat->SetPassThroughShadowTransparencyOverride(false);
	}

	const auto dlsType = parseString("emission.directlightsampling.type", "AUTO");
	if (dlsType == "ENABLED")
		mat->SetDirectLightSamplingType(DLS_ENABLED);
	else if (dlsType == "DISABLED")
		mat->SetDirectLightSamplingType(DLS_DISABLED);
	else if (dlsType == "AUTO")
		mat->SetDirectLightSamplingType(DLS_AUTO);
	else
		throw runtime_error("Unknown material emission direct sampling type: " + dlsType);

	mat->SetIndirectDiffuseVisibility(parseBool("visibility.indirect.diffuse.enable", true));
	mat->SetIndirectGlossyVisibility(parseBool("visibility.indirect.glossy.enable", true));
	mat->SetIndirectSpecularVisibility(parseBool("visibility.indirect.specular.enable", true));

	mat->SetShadowCatcher(parseBool("shadowcatcher.enable", false));
	mat->SetShadowCatcherOnlyInfiniteLights(parseBool("shadowcatcher.onlyinfinitelights", false));

	mat->SetPhotonGIEnabled(parseBool("photongi.enable", true));
	mat->SetHoldout(parseBool("holdout.enable", false));

	// Check if there is a image or IES map
	auto emissionMap = CreateEmissionMap(propName + ".emission", props);
	if (emissionMap) {
		// There is one
		mat->SetEmissionMap(*emissionMap);

		// An emission map is normalized to a unit integral over the
		// sphere while a plain Lambertian emitter integrates cos() to pi.
		// The spread profile is a drop-in for the Lambertian lobe (equal
		// power at equal gain), so restore the pi.
		if (!props.IsDefined(propName + ".emission.mapfile") &&
				!props.IsDefined(propName + ".emission.iesfile") &&
				!props.IsDefined(propName + ".emission.iesblob") &&
				(props.Get(Property(propName + ".emission.spread")(M_PI)).Get<float>() < M_PI - 1e-4f))
			mat->SetEmittedGain(mat->GetEmittedGain() * M_PI);

		// The spot profile is 1 on the axis: scale by its integral so the
		// gain is the on-axis intensity per unit emitter area
		if (props.IsDefined(propName + ".emission.spot.angle") &&
				!props.IsDefined(propName + ".emission.mapfile") &&
				!props.IsDefined(propName + ".emission.iesfile") &&
				!props.IsDefined(propName + ".emission.iesblob"))
			mat->SetEmittedGain(mat->GetEmittedGain() * mat->GetEmissionFunc()->Average());
	}

	// Interior volumes
	if (props.IsDefined(propName + ".volume.interior")) {
		const string volName = parseString("volume.interior", "vol1");
		MaterialConstRef m = matDefs->GetMaterial(volName);
		try {
			auto& v = dynamic_cast<const Volume&>(m);
			mat->SetInteriorVolume(v);
		} catch(std::bad_cast&) {
			throw runtime_error(
				volName
				+ " is not a volume and can not be used for material interior volume: "
				+ matName
			);
		}
	}

	// Exterior volumes
	if (props.IsDefined(propName + ".volume.exterior")) {
		const string volName = parseString("volume.exterior", "vol2");
		auto& m = matDefs->GetMaterial(volName);
		try {
			auto& v = dynamic_cast<const Volume&>(m);
			mat->SetExteriorVolume(v);
		} catch(std::bad_cast&) {
			throw runtime_error(volName + " is not a volume and can not be used for material exterior volume: " + matName);
		}
	}

	return mat;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

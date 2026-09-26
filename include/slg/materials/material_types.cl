#line 2 "material_types.cl"

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


//------------------------------------------------------------------------------
// Material evaluation op
//------------------------------------------------------------------------------

typedef enum {
	EVAL_ALBEDO,
	EVAL_GET_INTERIOR_VOLUME,
	EVAL_GET_EXTERIOR_VOLUME,
	EVAL_GET_EMITTED_RADIANCE,
	EVAL_GET_PASS_TROUGH_TRANSPARENCY,
	EVAL_EVALUATE,
	EVAL_SAMPLE,
	EVAL_CONDITIONAL_GOTO,
	EVAL_UNCONDITIONAL_GOTO,
	// For the very special case of Mix material
	EVAL_GET_VOLUME_MIX_SETUP1,
	EVAL_GET_VOLUME_MIX_SETUP2,
	EVAL_GET_EMITTED_RADIANCE_MIX_SETUP1,
	EVAL_GET_EMITTED_RADIANCE_MIX_SETUP2,
	EVAL_GET_PASS_TROUGH_TRANSPARENCY_MIX_SETUP1,
	EVAL_GET_PASS_TROUGH_TRANSPARENCY_MIX_SETUP2,
	EVAL_EVALUATE_MIX_SETUP1,
	EVAL_EVALUATE_MIX_SETUP2,
	EVAL_SAMPLE_MIX_SETUP1,
	EVAL_SAMPLE_MIX_SETUP2,
	// For the very special case of GlossyCoating material
	EVAL_EVALUATE_GLOSSYCOATING_SETUP,
	EVAL_SAMPLE_GLOSSYCOATING_SETUP,
	EVAL_SAMPLE_GLOSSYCOATING_CLOSE_SAMPLE_BASE,
	EVAL_SAMPLE_GLOSSYCOATING_CLOSE_EVALUATE_BASE,
	// For the very special case of TwoSided material
	EVAL_TWOSIDED_SETUP
} MaterialEvalOpType;

typedef struct {
	unsigned int matIndex;
	MaterialEvalOpType evalType;
	union {
		unsigned int opsCount;
	} opData;
} MaterialEvalOp;

//------------------------------------------------------------------------------
// Materials
//------------------------------------------------------------------------------

typedef enum {
	MATTE, MIRROR, GLASS, ARCHGLASS, MIX, NULLMAT, MATTETRANSLUCENT,
	GLOSSY2, METAL2, ROUGHGLASS, VELVET, CLOTH, CARPAINT, ROUGHMATTE,
	ROUGHMATTETRANSLUCENT, GLOSSYTRANSLUCENT, GLOSSYCOATING, DISNEY,
	TWOSIDED, HAIR, OPENPBR, DIFFRACTION,

	// Volumes
	HOMOGENEOUS_VOL, CLEAR_VOL, HETEROGENEOUS_VOL
} MaterialType;

typedef struct {
    unsigned int kdTexIndex;
} MatteParam;

typedef struct {
    unsigned int kdTexIndex;
    unsigned int sigmaTexIndex;
} RoughMatteParam;

typedef struct {
    unsigned int krTexIndex;
} MirrorParam;

typedef struct {
    unsigned int krTexIndex;
	unsigned int ktTexIndex;
	unsigned int exteriorIorTexIndex, interiorIorTexIndex;
	unsigned int cauchyBTex;
	unsigned int filmThicknessTexIndex;
	unsigned int filmIorTexIndex;
} GlassParam;

typedef struct {
    unsigned int krTexIndex;
	unsigned int expTexIndex;
} MetalParam;

typedef struct {
    unsigned int krTexIndex;
	unsigned int ktTexIndex;
	unsigned int exteriorIorTexIndex, interiorIorTexIndex;
	unsigned int filmThicknessTexIndex;
	unsigned int filmIorTexIndex;
} ArchGlassParam;

typedef struct {
	unsigned int matAIndex, matBIndex;
	unsigned int mixFactorTexIndex;
} MixParam;

typedef struct {
    unsigned int krTexIndex;
	unsigned int ktTexIndex;
} MatteTranslucentParam;

typedef struct {
    unsigned int krTexIndex;
	unsigned int ktTexIndex;
	unsigned int sigmaTexIndex;
} RoughMatteTranslucentParam;

typedef struct {
	unsigned int kdTexIndex;
	unsigned int ksTexIndex;
	unsigned int nuTexIndex;
	unsigned int nvTexIndex;
	unsigned int kaTexIndex;
	unsigned int depthTexIndex;
	unsigned int indexTexIndex;
	int multibounce;
	int doublesided;
	int useGgx;
} Glossy2Param;

typedef struct {
    unsigned int fresnelTexIndex;
    unsigned int nTexIndex;
	unsigned int kTexIndex;
	unsigned int nuTexIndex;
	unsigned int nvTexIndex;
	int multibounce;
	int useGgx;
} Metal2Param;

typedef struct {
    unsigned int krTexIndex;
	unsigned int ktTexIndex;
	unsigned int exteriorIorTexIndex, interiorIorTexIndex;
	unsigned int cauchyBTex;
	unsigned int nuTexIndex;
	unsigned int nvTexIndex;
	unsigned int filmThicknessTexIndex;
	unsigned int filmIorTexIndex;
	int useGgx;
} RoughGlassParam;

typedef struct {
    unsigned int kdTexIndex;
	unsigned int p1TexIndex;
	unsigned int p2TexIndex;
	unsigned int p3TexIndex;
	unsigned int thicknessTexIndex;
} VelvetParam;

typedef enum {
	DENIM, SILKSHANTUNG, SILKCHARMEUSE, COTTONTWILL, WOOLGABARDINE, POLYESTER
} ClothPreset;

typedef enum {
	WARP, WEFT
} YarnType;

// Data structure describing the properties of a single yarn
typedef struct {
	// Fiber twist angle
	float psi;
	// Maximum inclination angle
	float umax;
	// Spine curvature
	float kappa;
	// Width of segment rectangle
	float width;
	// Length of segment rectangle
	float length;
	/*! u coordinate of the yarn segment center,
	 * assumes that the tile covers 0 <= u, v <= 1.
	 * (0, 0) is lower left corner of the weave pattern
	 */
	float centerU;
	// v coordinate of the yarn segment center
	float centerV;

	// Weft/Warp flag
	YarnType yarn_type;
} Yarn;

typedef struct  {
	// Size of the weave pattern
	unsigned int tileWidth, tileHeight;
	
	// Uniform scattering parameter
	float alpha;
	// Forward scattering parameter
	float beta;
	// Filament smoothing
	float ss;
	// Highlight width
	float hWidth;
	// Combined area taken up by the warp & weft
	float warpArea, weftArea;

	// Noise-related parameters
	float fineness;

	float dWarpUmaxOverDWarp;
	float dWarpUmaxOverDWeft;
	float dWeftUmaxOverDWarp;
	float dWeftUmaxOverDWeft;
	float period;
} WeaveConfig;

typedef struct {
    ClothPreset Preset;
	unsigned int Weft_KdIndex;
	unsigned int Weft_KsIndex;
	unsigned int Warp_KdIndex;
	unsigned int Warp_KsIndex;
	float Repeat_U;
	float Repeat_V;
	float specularNormalization;
} ClothParam;

typedef struct {
	unsigned int KdTexIndex;
	unsigned int Ks1TexIndex;
	unsigned int Ks2TexIndex;
	unsigned int Ks3TexIndex;
	unsigned int M1TexIndex;
	unsigned int M2TexIndex;
	unsigned int M3TexIndex;
	unsigned int R1TexIndex;
	unsigned int R2TexIndex;
	unsigned int R3TexIndex;
	unsigned int KaTexIndex;
	unsigned int depthTexIndex;
	int useGgx;
} CarPaintParam;

typedef struct {
	unsigned int kdTexIndex;
	unsigned int ktTexIndex;
	unsigned int ksTexIndex;
	unsigned int ksbfTexIndex;
	unsigned int nuTexIndex;
	unsigned int nubfTexIndex;
	unsigned int nvTexIndex;
	unsigned int nvbfTexIndex;
	unsigned int kaTexIndex;
	unsigned int kabfTexIndex;
	unsigned int depthTexIndex;
	unsigned int depthbfTexIndex;
	unsigned int indexTexIndex;
	unsigned int indexbfTexIndex;
	int multibounce;
	int multibouncebf;
	int useGgx;
} GlossyTranslucentParam;

typedef struct {
	unsigned int matBaseIndex;
	unsigned int ksTexIndex;
	unsigned int nuTexIndex;
	unsigned int nvTexIndex;
	unsigned int kaTexIndex;
	unsigned int depthTexIndex;
	unsigned int indexTexIndex;
	int multibounce;
	int useGgx;
} GlossyCoatingParam;

typedef struct {
	unsigned int baseColorTexIndex;
	unsigned int subsurfaceTexIndex;
	unsigned int roughnessTexIndex;
	unsigned int metallicTexIndex;
	unsigned int specularTexIndex;
	unsigned int specularTintTexIndex;
	unsigned int clearcoatTexIndex;
	unsigned int clearcoatGlossTexIndex;
	unsigned int anisotropicTexIndex;
	unsigned int sheenTexIndex;
	unsigned int sheenTintTexIndex;
	unsigned int filmAmountTexIndex;
	unsigned int filmThicknessTexIndex;
	unsigned int filmIorTexIndex;
	unsigned int transmissionTexIndex;
	unsigned int transmissionRoughnessTexIndex;
	unsigned int iorTexIndex;
	unsigned int cauchyBTexIndex;
} DisneyParam;

typedef struct {
	unsigned int sigmaATexIndex;
	unsigned int colorTexIndex;
	unsigned int eumelaninTexIndex;
	unsigned int pheomelaninTexIndex;
	unsigned int etaTexIndex;
	unsigned int betaMTexIndex;
	unsigned int betaNTexIndex;
	unsigned int alphaTexIndex;
	// Huang'22 model parameters (model selects Chiang/Huang shading)
	unsigned int model; // 0 = chiang, 1 = huang
	unsigned int roughnessTexIndex;
	unsigned int aspectRatioTexIndex;
	float scaleR;
	float scaleTT;
	float scaleTRT;
} HairParam;

typedef struct {
	unsigned int frontMatIndex;
	unsigned int backMatIndex;
} TwoSidedParam;

// OpenPBR Surface (ASWF v1.1) lobe mixture. All texture indices.
typedef struct {
	unsigned int baseColorTexIndex;
	unsigned int baseWeightTexIndex;
	unsigned int baseMetalnessTexIndex;
	unsigned int baseDiffuseRoughnessTexIndex;
	unsigned int specWeightTexIndex;
	unsigned int specColorTexIndex;
	unsigned int specRoughnessTexIndex;
	unsigned int specAnisotropyTexIndex;
	unsigned int specRotationTexIndex;
	unsigned int specIorTexIndex;
	unsigned int transWeightTexIndex;
	unsigned int transColorTexIndex;
	unsigned int transDepthTexIndex;
	unsigned int transScatterTexIndex;
	unsigned int transScatterAnisoTexIndex;
	unsigned int dispersionTexIndex;
	unsigned int sssWeightTexIndex;
	unsigned int sssColorTexIndex;
	unsigned int sssRadiusTexIndex;
	unsigned int sssRadiusScaleTexIndex;
	unsigned int sssAnisotropyTexIndex;
	unsigned int coatWeightTexIndex;
	unsigned int coatColorTexIndex;
	unsigned int coatRoughnessTexIndex;
	unsigned int coatAnisotropyTexIndex;
	unsigned int coatRotationTexIndex;
	unsigned int coatIorTexIndex;
	unsigned int coatDarkeningTexIndex;
	unsigned int fuzzWeightTexIndex;
	unsigned int fuzzColorTexIndex;
	unsigned int fuzzRoughnessTexIndex;
	unsigned int filmWeightTexIndex;
	unsigned int filmThicknessTexIndex;
	unsigned int filmIorTexIndex;
} OpenPBRParam;

typedef struct {
	unsigned int krTexIndex;
	unsigned int spacingTexIndex; // groove period in nanometers
	unsigned int roughTexIndex;
	unsigned int fillTexIndex;
	float blaze;                  // blaze angle in radians (0 = symmetric)
	float centerX, centerY, centerZ; // object-space center (RADIAL mode)
	float centerU, centerV;       // UV-space center (RADIAL_UV mode)
	unsigned int maxOrder;        // hard cap on |m|
	unsigned int orientation;     // 0=U, 1=V, 2=RADIAL_UV, 3=RADIAL
} DiffractionParam;

typedef struct {
	unsigned int sigmaATexIndex;
} ClearVolumeParam;

typedef struct {
	unsigned int sigmaATexIndex;
	unsigned int sigmaSTexIndex;
	unsigned int gTexIndex;
	int multiScattering;
	// Volume phase function: 0 = Schlick approximation, 1 = Henyey-Greenstein
	int phaseFunc;
	// Distance sampling: when != 0 and eqLightCount > 0, the free-flight
	// distance is sampled by a one-sample MIS between the transmittance
	// and equiangular (Kulla & Fajardo, EGSR 2012) distributions
	int distanceSampling;
	// SSS albedo parametrization (random-walk subsurface): when
	// sssAlbedoTexIndex != NULL_INDEX, sigma_a/sigma_s are derived from the
	// surface diffuse albedo + mean free path (d'Eon inversion), ignoring
	// the raw coefficient textures.
	unsigned int sssAlbedoTexIndex;
	unsigned int sssMfpTexIndex;
} HomogenousVolumeParam;

typedef struct {
	unsigned int sigmaATexIndex;
	unsigned int sigmaSTexIndex;
	unsigned int gTexIndex;
	float stepSize;
	unsigned int maxStepsCount;
	int multiScattering;
	// Volume phase function: 0 = Schlick approximation, 1 = Henyey-Greenstein
	int phaseFunc;
	// Null-collision tracking: when deltaTracking != 0 and majorantOffset
	// != NULL_INDEX, free-flight sampling uses delta tracking over a
	// per-cell majorant grid (DDA traversal) instead of fixed-step
	// ray marching, and shadow rays use ratio tracking.
	int deltaTracking;
	// Majorant grid domain (world space) and cubic cell layout
	float majorantBBoxMinX, majorantBBoxMinY, majorantBBoxMinZ;
	float majorantBBoxMaxX, majorantBBoxMaxY, majorantBBoxMaxZ;
	float majorantCellSize;
	unsigned int majorantResX, majorantResY, majorantResZ;
	// Start index of the cells inside the volMajorants buffer
	// (NULL_INDEX when the grid is not available). Each cell is stored
	// as a float pair (minorant, majorant).
	unsigned int majorantOffset;
	// sigma_t bound outside the grid domain
	float globalMajorant;
	// sigma_t lower bound outside the grid domain (residual tracking control)
	float globalMinorant;
} HeterogenousVolumeParam;

typedef struct {
	unsigned int iorTexIndex;
	// This is a different kind of emission texture from the one in
	// Material class (i.e. is not sampled by direct light).
	unsigned int volumeEmissionTexIndex;
	unsigned int volumeLightID;
	int priority;

	union {
		ClearVolumeParam clear;
		HomogenousVolumeParam homogenous;
		HeterogenousVolumeParam heterogenous;
	};
} VolumeParam;

typedef struct {
	MaterialType type;
	unsigned int matID, lightID;
	float bumpSampleDistance;
	Spectrum emittedFactor;
	float emittedCosThetaMax;
	// Directional emission map (e.g. IES) for light tracing: the
	// SampleableSphericalFunction sampling distribution (offset into
	// envLightDistribution), its spherical average and the source map.
	// emissionFuncDistOffset == NULL_INDEX means no directional map.
	unsigned int emissionFuncDistOffset;
	float emissionFuncAverage;
	unsigned int emissionFuncImageMapIndex;
	int usePrimitiveArea;
	unsigned int frontTranspTexIndex, backTranspTexIndex;
	Spectrum passThroughShadowTransparency;
	// CPU Material::GetPassThroughShadowTransparencyOverride(): when set,
	// shadow-transparent paths are still traced by the light-path
	// estimator instead of being left to direct light sampling
	int passThroughShadowTransparencyOverride;
	unsigned int emitTexIndex, bumpTexIndex;
	// Type of indirect paths where a light source is visible with a direct hit. It is
	// an OR of DIFFUSE, GLOSSY and SPECULAR.
	BSDFEvent visibility;
	unsigned int interiorVolumeIndex, exteriorVolumeIndex;
	float glossiness, avgPassThroughTransparency;
	int isShadowCatcher, isShadowCatcherOnlyInfiniteLights, isPhotonGIEnabled,
			isHoldout;

	// The result of calling Material::GetEventTypes()
	BSDFEvent eventTypes;
	// The result of calling Material::IsDelta()
	int isDelta; 

	// Cryptomatte float id (host-computed murmur3 of the material name)
	float cryptoID;

	// Material eval. ops start index and length 
	unsigned int evalAlbedoOpStartIndex, evalAlbedoOpLength;
	unsigned int evalGetInteriorVolumeOpStartIndex, evalGetInteriorVolumeOpLength;
	unsigned int evalGetExteriorVolumeOpStartIndex, evalGetExteriorVolumeOpLength;
	unsigned int evalGetEmittedRadianceOpStartIndex, evalGetEmittedRadianceOpLength;
	unsigned int evalGetPassThroughTransparencyOpStartIndex, evalGetPassThroughTransparencyOpLength;
	unsigned int evalEvaluateOpStartIndex, evalEvaluateOpLength;
	unsigned int evalSampleOpStartIndex, evalSampleOpLength;

	union {
		MatteParam matte;
		RoughMatteParam roughmatte;
		MirrorParam mirror;
		GlassParam glass;
		MetalParam metal;
		ArchGlassParam archglass;
		MixParam mix;
		// NULLMAT has no parameters
		MatteTranslucentParam matteTranslucent;
		RoughMatteTranslucentParam roughmatteTranslucent;
		Glossy2Param glossy2;
		Metal2Param metal2;
		RoughGlassParam roughglass;
		VelvetParam velvet;
        ClothParam cloth;
		CarPaintParam carpaint;
		GlossyTranslucentParam glossytranslucent;
		GlossyCoatingParam glossycoating;
		DisneyParam disney;
		TwoSidedParam twosided;
		HairParam hair;
		OpenPBRParam openpbr;
		DiffractionParam diffraction;
		VolumeParam volume;
	};
} Material;

//------------------------------------------------------------------------------
// Some macro trick in order to have more readable code
//------------------------------------------------------------------------------

#if defined(SLG_OPENCL_KERNEL)

#define MATERIALS_PARAM_DECL \
	, __global const Material* restrict mats \
	, __global const MaterialEvalOp* restrict matEvalOps \
	, __global float *matEvalStacks \
	, const uint maxMaterialEvalStackSize \
	TEXTURES_PARAM_DECL
#define MATERIALS_PARAM \
	, mats \
	, matEvalOps \
	, matEvalStacks \
	, maxMaterialEvalStackSize \
	TEXTURES_PARAM

#define MATERIAL_EVALUATE_RETURN_BLACK { \
		const float3 result = BLACK; \
		EvalStack_PushFloat3(result); \
		const BSDFEvent event = NONE; \
		EvalStack_PushBSDFEvent(event); \
		const float directPdfW = 0.f; \
		EvalStack_PushFloat(directPdfW); \
		return; \
	}

#define MATERIAL_SAMPLE_RETURN_BLACK { \
		const float3 result = BLACK; \
		EvalStack_PushFloat3(result); \
		const float3 sampledDir = ZERO; \
		EvalStack_PushFloat3(sampledDir); \
		const BSDFEvent event = NONE; \
		EvalStack_PushBSDFEvent(event); \
		const float pdfW = 0.f; \
		EvalStack_PushFloat(pdfW); \
		return; \
	}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

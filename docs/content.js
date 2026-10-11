window.SLC_DATA = {
  "version": "2.11.27",
  "date": "2026-10-11",
  "core": "https://github.com/claudianus/SuperLuxCore",
  "addon": "https://github.com/claudianus/SuperBlendLuxCore",
  "categories": {
    "all": {
      "en": "All features",
      "ko": "전체 기능"
    },
    "hardware": {
      "en": "Hardware",
      "ko": "하드웨어"
    },
    "transport": {
      "en": "Light transport",
      "ko": "빛의 전달"
    },
    "caustics": {
      "en": "Caustics",
      "ko": "커스틱"
    },
    "materials": {
      "en": "Materials & hair",
      "ko": "재질 · 헤어"
    },
    "volumes": {
      "en": "Volumes & textures",
      "ko": "볼륨 · 텍스처"
    },
    "workflow": {
      "en": "Blender & production",
      "ko": "Blender · 제작"
    }
  },
  "features": [
    {
      "id": "metal",
      "category": "hardware",
      "title": {
        "en": "Native Metal & hardware ray tracing",
        "ko": "네이티브 Metal · 하드웨어 레이 트레이싱"
      },
      "description": {
        "en": "Apple GPU compute with triangle acceleration, instancing, refits and motion support. Native curves avoid hair tessellation on the supported Metal path.",
        "ko": "Apple GPU 연산과 삼각형 가속, 인스턴싱, 리핏, 모션을 지원합니다. 지원되는 Metal 경로에서는 네이티브 커브로 헤어를 처리합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/metal-backend.md",
      "badge": {
        "en": "Apple GPU",
        "ko": "Apple GPU"
      },
      "image": "assets/studio/architecture.jpg",
      "scene": "architecture",
      "technical": false,
      "position": [
        35,
        32
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "spectral",
      "category": "transport",
      "title": {
        "en": "Hero-wavelength spectral transport",
        "ko": "히어로 파장 스펙트럴 전달"
      },
      "description": {
        "en": "Trace sampled wavelengths instead of only RGB, with spectral reconstruction and wavelength-aware materials and lights.",
        "ko": "RGB만 추적하는 대신 파장을 샘플링합니다. 스펙트럴 색 재구성과 파장 기반 재질·조명을 함께 사용합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/spectral.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/prism.jpg",
      "scene": "prism",
      "technical": false,
      "position": [
        48,
        49
      ],
      "visualNote": {
        "en": "Native Metal RGB-dispersion artwork providing optical scene context. This image is not an isolated spectral-transport validation or a performance benchmark. See the feature documentation for controls and coverage.",
        "ko": "실제 Metal RGB 분산 렌더로 보는 광학 장면입니다. 이 이미지는 스펙트럴 전송의 단독 검증이나 속도 측정이 아닙니다. 제어와 지원 범위는 기능 문서에서 확인할 수 있습니다."
      }
    },
    {
      "id": "mnee",
      "category": "caustics",
      "title": {
        "en": "Manifold next-event estimation",
        "ko": "매니폴드 다음 사건 추정 · MNEE"
      },
      "description": {
        "en": "Connect light paths through specular chains for focused refractive and reflective caustics.",
        "ko": "정반사와 굴절의 연속 경로를 조명에 연결해 집중된 커스틱을 계산합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/mnee.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/prism.jpg",
      "scene": "prism",
      "technical": false,
      "position": [
        61,
        66
      ],
      "visualNote": {
        "en": "Native Metal RGB-dispersion artwork providing optical scene context. This image is not an isolated spectral-transport validation or a performance benchmark. See the feature documentation for controls and coverage.",
        "ko": "실제 Metal RGB 분산 렌더로 보는 광학 장면입니다. 이 이미지는 스펙트럴 전송의 단독 검증이나 속도 측정이 아닙니다. 제어와 지원 범위는 기능 문서에서 확인할 수 있습니다."
      }
    },
    {
      "id": "openpbr",
      "category": "materials",
      "title": {
        "en": "OpenPBR & principled transmission",
        "ko": "OpenPBR · Principled 통합 투과"
      },
      "description": {
        "en": "A modern layered material and a first-class transmission lobe in Disney, with roughness and thin-film controls.",
        "ko": "현대적인 레이어 재질과 Disney 내부의 통합 투과 로브를 제공합니다. 거칠기와 박막 효과를 함께 제어합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/disney.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/beauty.jpg",
      "scene": "beauty",
      "technical": false,
      "position": [
        44,
        48
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "volume",
      "category": "volumes",
      "title": {
        "en": "Advanced volume tracking",
        "ko": "고급 볼륨 추적"
      },
      "description": {
        "en": "Null-collision, residual/minorant tracking and equiangular MIS expand the tools for heterogeneous media and volumetric lighting.",
        "ko": "Null-collision, Residual·Minorant 추적과 Equiangular MIS로 비균질 매질과 볼륨 조명을 다룹니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/volume-tracking.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        57,
        65
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "blender",
      "category": "workflow",
      "title": {
        "en": "Blender viewport & F12",
        "ko": "Blender 뷰포트 · F12"
      },
      "description": {
        "en": "Work in Blender with node materials, interactive preview, final rendering and persistent scene export.",
        "ko": "Blender의 노드 재질, 인터랙티브 미리보기, 최종 렌더와 지속적인 씬 내보내기를 사용합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/blender-adapter.md",
      "badge": {
        "en": "Blender 5.2",
        "ko": "Blender 5.2"
      },
      "image": "assets/studio/heritage.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        40,
        47
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "multiscatter",
      "category": "materials",
      "title": {
        "en": "Multiple-scattering compensation",
        "ko": "다중 산란 에너지 보정"
      },
      "description": {
        "en": "Opt-in compensation recovers energy lost by single-scatter roughglass, carpaint and Disney lobes.",
        "ko": "단일 산란에서 손실되는 Rough Glass, Car Paint, Disney 로브의 에너지를 보정하는 선택 기능입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/multiscatter.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/audio.jpg",
      "scene": "audio",
      "technical": false,
      "position": [
        53,
        64
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "restirdi",
      "category": "transport",
      "title": {
        "en": "ReSTIR direct & indirect light",
        "ko": "ReSTIR 직접광 · 간접광"
      },
      "description": {
        "en": "Reservoir-based DI/GI reuse adds sampling choices for many lights and indirect illumination on CPU, OpenCL and Metal.",
        "ko": "CPU, OpenCL, Metal에서 Reservoir 기반 DI/GI 재사용으로 다수의 조명과 간접광을 샘플링합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/restir-di.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        36,
        46
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "gpult",
      "category": "caustics",
      "title": {
        "en": "GPU light tracing · LMNEE / MGE",
        "ko": "GPU 라이트 트레이싱 · LMNEE / MGE"
      },
      "description": {
        "en": "Trace light-origin paths on the GPU and connect constrained specular paths with dedicated light-side estimators.",
        "ko": "GPU에서 광원 출발 경로를 추적하고, 광원 쪽 전용 추정기로 제약된 정반사 경로를 연결합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/gpu_lighttracing.md",
      "badge": {
        "en": "OpenCL · Metal",
        "ko": "OpenCL · Metal"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        49,
        63
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "hair",
      "category": "materials",
      "title": {
        "en": "Chiang & Huang hair",
        "ko": "Chiang · Huang 헤어"
      },
      "description": {
        "en": "Dedicated hair scattering models, strand variation and a choice of hair appearance beyond surface-only shading.",
        "ko": "전용 헤어 산란 모델과 스트랜드별 변화를 사용해 표면 셰이딩 이상의 머리카락 표현을 제공합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/hair.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/textile.jpg",
      "scene": "textile",
      "technical": false,
      "position": [
        62,
        45
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "cryptomatte",
      "category": "workflow",
      "title": {
        "en": "Object & material Cryptomatte",
        "ko": "오브젝트 · 재질 Cryptomatte"
      },
      "description": {
        "en": "Generate ID-based mattes and manifests for selecting rendered objects and materials in compositing.",
        "ko": "렌더된 오브젝트와 재질을 합성에서 선택할 수 있도록 ID 마스크와 매니페스트를 생성합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/cryptomatte.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/interior-crypto-objects.jpg",
      "scene": "interior",
      "technical": true,
      "position": [
        45,
        62
      ],
      "visualNote": {
        "en": "An actual native renderer output, displayed as data. This view is not a final beauty or a timing comparison.",
        "ko": "실제 네이티브 렌더러 출력을 데이터로 표시한 보기입니다. 최종 작품이나 속도 비교가 아닙니다."
      }
    },
    {
      "id": "vdb",
      "category": "volumes",
      "title": {
        "en": "Named VDB fields & interpolation",
        "ko": "이름 기반 VDB 필드 · 보간"
      },
      "description": {
        "en": "Read density, color, vector and temperature fields; recent work preserves Linear/Cubic reconstruction and boundary behavior from Blender.",
        "ko": "밀도, 색, 벡터, 온도 필드를 읽습니다. 최근 업데이트에서는 Blender의 Linear/Cubic 재구성과 경계 동작을 보존합니다."
      },
      "href": "https://github.com/claudianus/SuperBlendLuxCore/blob/main/doc/engineering/2026-10-09-cycles-scene-goal.md",
      "badge": {
        "en": "2.11.27 · CPU/GPU",
        "ko": "2.11.27 · CPU/GPU"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        58,
        44
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "guiding",
      "category": "transport",
      "title": {
        "en": "Path guiding & portals",
        "ko": "경로 가이딩 · 포털"
      },
      "description": {
        "en": "SD-tree, RIS and directional fields add a learning-based sampling mode. Cited test scenes showed no measurable variance reduction; choose it for scene-specific evaluation.",
        "ko": "SD-tree·RIS·방향 필드로 학습 기반 샘플링 모드를 추가합니다. 문서의 검증 장면에서는 측정 가능한 분산 감소가 없었으며 장면별로 평가해 선택합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/path-guiding.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        41,
        61
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "samplers",
      "category": "transport",
      "title": {
        "en": "PMJ02 & blue-noise Sobol",
        "ko": "PMJ02 · 블루 노이즈 Sobol"
      },
      "description": {
        "en": "Progressive sample sequences and screen-space error shaping complement the inherited sampler family.",
        "ko": "점진적 샘플 시퀀스와 화면 공간의 오차 분포 제어로 기존 샘플러를 보완합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/samplers.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/heritage.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        54,
        43
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "adaptive",
      "category": "transport",
      "title": {
        "en": "Noise-aware render stopping",
        "ko": "노이즈 기준 렌더 종료"
      },
      "description": {
        "en": "Estimate image noise and stop against an error target alongside conventional sample or time limits.",
        "ko": "이미지 노이즈를 추정하고 오차 목표에 따라 종료합니다. 기존 샘플 수·시간 제한과 함께 사용할 수 있습니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/adaptive-error.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/heritage-raw.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        37,
        60
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "sellmeier",
      "category": "materials",
      "title": {
        "en": "Sellmeier glass dispersion",
        "ko": "Sellmeier 유리 분산"
      },
      "description": {
        "en": "Measured glass-family presets and a three-term dispersion model describe refractive index across wavelengths.",
        "ko": "측정된 유리 계열 프리셋과 3항 분산 모델로 파장에 따른 굴절률을 표현합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/dispersion.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/prism.jpg",
      "scene": "prism",
      "technical": false,
      "position": [
        50,
        42
      ],
      "visualNote": {
        "en": "Native Metal RGB-dispersion artwork providing optical scene context. This image is not an isolated spectral-transport validation or a performance benchmark. See the feature documentation for controls and coverage.",
        "ko": "실제 Metal RGB 분산 렌더로 보는 광학 장면입니다. 이 이미지는 스펙트럴 전송의 단독 검증이나 속도 측정이 아닙니다. 제어와 지원 범위는 기능 문서에서 확인할 수 있습니다."
      }
    },
    {
      "id": "diffraction",
      "category": "materials",
      "title": {
        "en": "Diffraction gratings",
        "ko": "회절 격자 재질"
      },
      "description": {
        "en": "Model wavelength-dependent diffraction orders, groove orientation, spacing and blaze for compact-disc-like optical effects.",
        "ko": "회절 차수, 홈 방향, 간격과 블레이즈를 제어해 CD와 같은 파장 기반 광학 효과를 표현합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/diffraction.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/music.jpg",
      "scene": "music",
      "technical": false,
      "position": [
        63,
        59
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "thinfilm",
      "category": "materials",
      "title": {
        "en": "Thin-film interference",
        "ko": "박막 간섭"
      },
      "description": {
        "en": "Use thickness and refractive-index controls to introduce interference color in supported layered and transmissive materials.",
        "ko": "지원되는 레이어·투과 재질에서 막 두께와 굴절률을 제어해 간섭색을 표현합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/disney.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/pearl.jpg",
      "scene": "pearl",
      "technical": false,
      "position": [
        46,
        41
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "ggx",
      "category": "materials",
      "title": {
        "en": "GGX & conductor random walks",
        "ko": "GGX · 금속 랜덤 워크"
      },
      "description": {
        "en": "Opt-in GGX distributions across multiple material types, plus Heitz-style multiple scattering for metal2.",
        "ko": "여러 재질에서 선택형 GGX 분포를 사용하며, Metal2에는 Heitz 방식의 다중 산란을 제공합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/ggx-multibounce.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/audio.jpg",
      "scene": "audio",
      "technical": false,
      "position": [
        59,
        58
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "sss",
      "category": "materials",
      "title": {
        "en": "Random-walk subsurface scattering",
        "ko": "랜덤 워크 표면 아래 산란 · SSS"
      },
      "description": {
        "en": "OpenPBR subsurface paths and hybrid profile controls expand translucent material rendering. Full Cycles BSSRDF matching remains under validation.",
        "ko": "OpenPBR 표면 아래 경로와 하이브리드 프로파일 제어로 반투명 재질을 표현합니다. Cycles BSSRDF 전체 대응은 검증 중입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/openpbr-sss-findings.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/beauty.jpg",
      "scene": "beauty",
      "technical": false,
      "position": [
        42,
        40
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "ssspresets",
      "category": "materials",
      "title": {
        "en": "Measured SSS presets",
        "ko": "측정 SSS 프리셋"
      },
      "description": {
        "en": "Start from measured subsurface color and distance presets, then tune the result for your material.",
        "ko": "측정된 산란색과 거리 프리셋으로 시작해 재질에 맞게 결과를 조절합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/sss-presets.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/beauty.jpg",
      "scene": "beauty",
      "technical": false,
      "position": [
        55,
        57
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "sheen",
      "category": "materials",
      "title": {
        "en": "Charlie sheen & measured metals",
        "ko": "Charlie 광택 · 측정 금속"
      },
      "description": {
        "en": "Expanded cloth sheen and metal edge tint combine with measured n/k optical presets for richer grazing-angle responses.",
        "ko": "확장된 천 광택과 금속 가장자리 색, 측정 n/k 광학 프리셋으로 비스듬한 각도의 반응을 표현합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/ggx-multibounce.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/textile.jpg",
      "scene": "textile",
      "technical": false,
      "position": [
        38,
        39
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "blackbody",
      "category": "volumes",
      "title": {
        "en": "Texture-driven blackbody emission",
        "ko": "텍스처 기반 흑체 방출"
      },
      "description": {
        "en": "Drive emission color with a temperature texture or VDB field, including wavelength-aware Planck emission in spectral mode.",
        "ko": "온도 텍스처나 VDB 필드로 방출색을 제어합니다. 스펙트럴 모드에서는 파장 기반 Planck 방출을 사용합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/textures.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        51,
        56
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "proctextures",
      "category": "volumes",
      "title": {
        "en": "Extended procedural textures",
        "ko": "확장된 절차적 텍스처"
      },
      "description": {
        "en": "White Noise, generic math functions and additional procedural mappings support a wider range of Blender node graphs.",
        "ko": "White Noise, 범용 수학 함수와 추가 절차적 매핑으로 더 다양한 Blender 노드 그래프를 지원합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/textures.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        64,
        38
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "cycles",
      "category": "workflow",
      "title": {
        "en": "Existing Cycles scene adapter",
        "ko": "기존 Cycles 씬 어댑터"
      },
      "description": {
        "en": "Translate supported Cycles nodes and settings directly for SuperLuxCore. The aim is sensible scene reuse with the renderer’s own quality, not bit-identical Cycles output.",
        "ko": "지원되는 Cycles 노드와 설정을 SuperLuxCore에 직접 연결합니다. 고유한 렌더 품질로 기존 씬을 합리적으로 재사용하는 것이 목적입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/blender-adapter.md",
      "badge": {
        "en": "Documented limits",
        "ko": "지원 범위 문서화"
      },
      "image": "assets/studio/heritage.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        47,
        55
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "displacement",
      "category": "workflow",
      "title": {
        "en": "True displacement & node semantics",
        "ko": "실제 디스플레이스먼트 · 노드 의미"
      },
      "description": {
        "en": "Recent Blender work covers scalar/vector displacement, ColorRamp and alpha behavior, bump details and volume-node semantics; support is tracked per feature.",
        "ko": "최근 Blender 작업에는 스칼라·벡터 디스플레이스먼트, ColorRamp·Alpha, 범프와 볼륨 노드 의미가 포함됩니다. 기능별 지원 범위를 추적합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/blender-adapter.md",
      "badge": {
        "en": "Scoped compatibility",
        "ko": "기능별 호환"
      },
      "image": "assets/studio/heritage-normal.jpg",
      "scene": "heritage",
      "technical": true,
      "position": [
        60,
        37
      ],
      "visualNote": {
        "en": "Actual native albedo or normal data from this scene. This is scene context, not an LPE, displacement or algorithm validation pass.",
        "ko": "이 장면에서 출력한 실제 알베도 또는 노멀 데이터입니다. 사용 장면을 보여주며 LPE·디스플레이스먼트·알고리즘 검증 패스는 아닙니다."
      }
    },
    {
      "id": "viewportcoverage",
      "category": "workflow",
      "title": {
        "en": "Immediate viewport coverage",
        "ko": "빠른 뷰포트 화면 채우기"
      },
      "description": {
        "en": "Lattice coverage and infill spread the first samples across the screen before spending more work on refinement.",
        "ko": "격자 커버리지와 빈 영역 채우기로 첫 샘플을 화면 전체에 배치한 뒤 세부 묘사를 진행합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/viewport-instant-coverage.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        43,
        54
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "viewportreuse",
      "category": "workflow",
      "title": {
        "en": "Adaptive preview & temporal reuse",
        "ko": "적응형 미리보기 · 시간 재사용"
      },
      "description": {
        "en": "Adaptive/foveal work allocation, temporal reuse and smoothing help interactive rendering settle during editing.",
        "ko": "적응형·중심 영역 작업 배분, 시간 재사용과 스무딩으로 편집 중 인터랙티브 렌더를 개선합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/viewport-adaptive-temporal.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        56,
        36
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "deltaexport",
      "category": "workflow",
      "title": {
        "en": "Persistent & incremental export",
        "ko": "지속 · 증분 씬 내보내기"
      },
      "description": {
        "en": "Keep the scene alive and update changed geometry or materials, reducing repeated full exports during interaction.",
        "ko": "씬을 유지하고 바뀐 지오메트리·재질을 갱신해 상호작용 중 전체 내보내기의 반복을 줄입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/blender-adapter.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/heritage.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        39,
        53
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "oidn",
      "category": "hardware",
      "title": {
        "en": "Metal OIDN & GPU film",
        "ko": "Metal OIDN · GPU 필름 처리"
      },
      "description": {
        "en": "Integrate the Metal-capable OIDN package and GPU film processing into the rendering/image pipeline.",
        "ko": "Metal을 지원하는 OIDN 패키지와 GPU 필름 처리를 렌더링·이미지 파이프라인에 연동합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/oidn-film.md",
      "badge": {
        "en": "Metal · OpenCL film",
        "ko": "Metal · OpenCL 필름"
      },
      "image": "assets/studio/heritage.jpg",
      "scene": "heritage",
      "technical": false,
      "position": [
        52,
        35
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "motion",
      "category": "hardware",
      "title": {
        "en": "Deformation motion blur",
        "ko": "변형 모션 블러"
      },
      "description": {
        "en": "Vertex motion is supported by Embree, software MBVH and Metal HWRT. Blender mesh export is covered; hair deformation export remains scoped.",
        "ko": "Embree, 소프트웨어 MBVH와 Metal HWRT에서 정점 모션을 지원합니다. Blender 메시 내보내기를 지원하며 헤어 변형은 제한됩니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/dev-tools/deformation-motion-blur-design.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/fan.jpg",
      "scene": "fan",
      "technical": false,
      "position": [
        35,
        52
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "lightbvh",
      "category": "transport",
      "title": {
        "en": "Light BVH for many lights",
        "ko": "다수 조명을 위한 Light BVH"
      },
      "description": {
        "en": "A hierarchical light-selection strategy provides another option for scenes with large light populations.",
        "ko": "조명이 많은 장면에서 계층적인 광원 선택 전략을 사용할 수 있습니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/light-bvh.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        48,
        34
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "lightlink",
      "category": "workflow",
      "title": {
        "en": "Light linking",
        "ko": "라이트 링크"
      },
      "description": {
        "en": "Use 64-bit illumination groups to control which lights illuminate which objects.",
        "ko": "64비트 조명 그룹으로 어떤 조명이 어떤 오브젝트를 비추는지 제어합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/light-linking.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/beauty.jpg",
      "scene": "beauty",
      "technical": false,
      "position": [
        61,
        51
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "lpe",
      "category": "workflow",
      "title": {
        "en": "Light path expression passes",
        "ko": "빛 경로 표현식 · LPE 패스"
      },
      "description": {
        "en": "Separate path contributions with expressions in PATHCPU/PATHOCL. Bidirectional and light-tracing LPE output is not supported.",
        "ko": "PATHCPU/PATHOCL에서 표현식으로 빛의 경로별 기여를 분리합니다. 양방향·라이트 트레이싱 LPE 출력은 지원하지 않습니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/lpe.md",
      "badge": {
        "en": "Path engines only",
        "ko": "패스 엔진 전용"
      },
      "image": "assets/studio/perfume-albedo.jpg",
      "scene": "perfume",
      "technical": true,
      "position": [
        44,
        33
      ],
      "visualNote": {
        "en": "Actual native albedo or normal data from this scene. This is scene context, not an LPE, displacement or algorithm validation pass.",
        "ko": "이 장면에서 출력한 실제 알베도 또는 노멀 데이터입니다. 사용 장면을 보여주며 LPE·디스플레이스먼트·알고리즘 검증 패스는 아닙니다."
      }
    },
    {
      "id": "temporal",
      "category": "workflow",
      "title": {
        "en": "Variance, motion & temporal film",
        "ko": "분산 · 모션 · 시간 누적 필름"
      },
      "description": {
        "en": "Additional channels and temporal accumulation supply more information for preview and downstream denoising workflows.",
        "ko": "추가 채널과 시간 누적으로 미리보기와 후속 디노이즈 작업에 필요한 정보를 제공합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/aov-audit-temporal-denoise.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/fan.jpg",
      "scene": "fan",
      "technical": false,
      "position": [
        57,
        50
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "outofcore",
      "category": "hardware",
      "title": {
        "en": "Large-scene host memory & proxies",
        "ko": "대규모 씬의 호스트 메모리 · 프록시"
      },
      "description": {
        "en": ".lxm proxies, CPU cluster residency and host geometry/texture/staging spill help manage RAM. GPU geometry still uploads in full; device streaming beyond VRAM is design work.",
        "ko": ".lxm 프록시, CPU 클러스터 상주 관리와 호스트 지오메트리·텍스처·업로드 버퍼의 저장소 활용으로 RAM을 관리합니다. GPU 지오메트리는 전체 업로드하며 VRAM 초과 장치 스트리밍은 설계 단계입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/out-of-core.md",
      "badge": {
        "en": "CPU residency · host spill",
        "ko": "CPU 상주 관리 · 호스트 저장소"
      },
      "image": "assets/studio/architecture.jpg",
      "scene": "architecture",
      "technical": false,
      "position": [
        40,
        32
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "adaptivecaustic",
      "category": "caustics",
      "title": {
        "en": "Adaptive caustic partition",
        "ko": "적응형 커스틱 분할"
      },
      "description": {
        "en": "Coordinate photon-cache and manifold paths by caustic class rather than treating all focused light alike.",
        "ko": "집중된 빛을 동일하게 다루지 않고 커스틱 유형에 따라 광자 캐시와 매니폴드 경로를 조율합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/adaptive-caustic.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        53,
        49
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "progressive",
      "category": "caustics",
      "title": {
        "en": "Progressive caustic caches",
        "ko": "점진적 커스틱 캐시"
      },
      "description": {
        "en": "Progressive cache replacement and automatic radius controls extend caustic reconstruction choices.",
        "ko": "점진적 캐시 교체와 자동 반경 제어로 커스틱 재구성 방법을 확장합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/progressive-caustics.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        36,
        66
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "mediacaustic",
      "category": "caustics",
      "title": {
        "en": "Caustics through media",
        "ko": "매질을 통과하는 커스틱"
      },
      "description": {
        "en": "Extended specular-chain handling includes transparent-media paths with documented validation scenes.",
        "ko": "투명 매질을 포함하는 정반사 연속 경로를 확장하고 장면별 검증 자료를 제공합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/caustics-sota.md",
      "badge": {
        "en": "CPU · GPU",
        "ko": "CPU · GPU"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        49,
        48
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "vertexconnect",
      "category": "caustics",
      "title": {
        "en": "GPU vertex connection & merging",
        "ko": "GPU 정점 연결 · 병합"
      },
      "description": {
        "en": "Light/eye vertex connection and merging add bidirectional-style options to the GPU transport stack.",
        "ko": "광원·시선 정점의 연결과 병합으로 GPU 전달에 양방향 방식의 선택지를 추가합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/gpu_vertexconnection.md",
      "badge": {
        "en": "OpenCL · Metal",
        "ko": "OpenCL · Metal"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        62,
        65
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "regularization",
      "category": "transport",
      "title": {
        "en": "Path-space regularization",
        "ko": "경로 공간 정규화"
      },
      "description": {
        "en": "Caustic-capable scenes can automatically start with a conservative decaying regularization schedule. Explicit settings control the smoothing; early frames differ while the radius decays.",
        "ko": "커스틱 장면에서는 보수적인 감소형 정규화로 시작할 수 있습니다. 명시적 설정으로 평활화를 제어하며 반경이 감소하는 동안 초기 프레임의 결과가 달라집니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/path-regularization.md",
      "badge": {
        "en": "Auto + explicit control",
        "ko": "자동 + 명시적 제어"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        45,
        47
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "clamping",
      "category": "transport",
      "title": {
        "en": "Adaptive robust clamping",
        "ko": "적응형 기여값 제한"
      },
      "description": {
        "en": "Optional variance-aware clamping controls unusually large sample contributions. Enable deliberately; clamping changes the estimator.",
        "ko": "선택형 분산 기준 제한으로 매우 큰 샘플 기여를 제어합니다. 추정기를 바꾸는 기능이므로 목적에 맞게 활성화하세요."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/adaptive-clamping.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/jewelry-raw.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        58,
        64
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "restirpt",
      "category": "transport",
      "title": {
        "en": "ReSTIR path tracing",
        "ko": "ReSTIR 패스 트레이싱"
      },
      "description": {
        "en": "An opt-in path-suffix reservoir mode on CPU and GPU, including dense and wavefront PATHOCL. Bounded-bias reuse and scene-dependent effectiveness require deliberate selection.",
        "ko": "CPU와 GPU의 경로 후반 Reservoir 선택 모드이며 Dense·Wavefront PATHOCL을 포함합니다. 제한된 편향과 장면별 효과를 고려해 사용합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/restir-pt.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        41,
        46
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "ssp",
      "category": "caustics",
      "title": {
        "en": "Eye-side specular tails",
        "ko": "시선 쪽 정반사 연속 경로 · SSP"
      },
      "description": {
        "en": "Recorded eye-side specular chains seed light-side manifold connections on CPU and GPU light-tracing paths.",
        "ko": "CPU와 GPU 라이트 트레이싱에서 기록된 시선 쪽 정반사 경로를 광원 쪽 매니폴드 연결의 시드로 사용합니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/ssp-tail.md",
      "badge": {
        "en": "CPU · GPU light tracing",
        "ko": "CPU · GPU 라이트 트레이싱"
      },
      "image": "assets/studio/jewelry.jpg",
      "scene": "jewelry",
      "technical": false,
      "position": [
        54,
        63
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "wavefront",
      "category": "hardware",
      "title": {
        "en": "Wavefront task queues",
        "ko": "웨이브프런트 작업 큐"
      },
      "description": {
        "en": "Optional state queues for GPU path kernels. Dense scheduling remains the automatic default until broader workload evidence supports a change.",
        "ko": "GPU 패스 커널에 상태별 작업 큐를 선택적으로 사용합니다. 더 넓은 장면 근거가 마련되기 전까지 자동 모드의 기본은 Dense입니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/dev-tools/wavefront-design.md",
      "badge": {
        "en": "Opt-in · OpenCL/Metal",
        "ko": "선택 기능 · OpenCL/Metal"
      },
      "image": "assets/studio/heritage-normal.jpg",
      "scene": "heritage",
      "technical": true,
      "position": [
        37,
        45
      ],
      "visualNote": {
        "en": "Actual native albedo or normal data from this scene. This is scene context, not an LPE, displacement or algorithm validation pass.",
        "ko": "이 장면에서 출력한 실제 알베도 또는 노멀 데이터입니다. 사용 장면을 보여주며 LPE·디스플레이스먼트·알고리즘 검증 패스는 아닙니다."
      }
    },
    {
      "id": "vulkan",
      "category": "hardware",
      "title": {
        "en": "Vulkan hardware-ray-tracing research",
        "ko": "Vulkan 하드웨어 레이 트레이싱 연구"
      },
      "description": {
        "en": "An experimental backend with MoltenVK validation. Native Vulkan driver coverage is pending; it is not the default production backend.",
        "ko": "MoltenVK 검증을 진행한 실험 백엔드입니다. 네이티브 Vulkan 드라이버 검증은 남아 있으며 기본 제작 백엔드는 아닙니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/features/vulkan-backend-assessment.md",
      "badge": {
        "en": "Experimental",
        "ko": "실험 기능"
      },
      "image": "assets/studio/architecture.jpg",
      "scene": "architecture",
      "technical": false,
      "position": [
        50,
        62
      ],
      "visualNote": {
        "en": "Native CPU artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 CPU 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "restirpg",
      "category": "transport",
      "title": {
        "en": "Reservoir-fed path guiding",
        "ko": "Reservoir 기반 경로 가이딩"
      },
      "description": {
        "en": "ReSTIR winners feed the guiding model on CPU and GPU. It adds a scene-dependent learning mode; measured neutral cases do not justify a universal speed claim or automatic promotion.",
        "ko": "CPU/GPU에서 ReSTIR 선택 결과를 가이딩 모델에 전달합니다. 장면별 학습 모드이며, 중립적인 측정 사례가 있어 일괄적인 속도 향상이나 자동 전환을 주장하지 않습니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/restir-pg-design.md",
      "badge": {
        "en": "Opt-in · CPU/GPU",
        "ko": "선택 기능 · CPU/GPU"
      },
      "image": "assets/studio/interior.jpg",
      "scene": "interior",
      "technical": false,
      "position": [
        63,
        44
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    },
    {
      "id": "photonbeams",
      "category": "caustics",
      "title": {
        "en": "Photon beams in participating media",
        "ko": "참여 매질의 광자 빔"
      },
      "description": {
        "en": "Extend PhotonGI with volume photon beams, GPU beam traversal/deposits and progressive cache updates for concentrated light inside media.",
        "ko": "PhotonGI에 볼륨 광자 빔, GPU 빔 조회·저장과 점진적 캐시 갱신을 추가해 매질 속 집중된 빛을 다룹니다."
      },
      "href": "https://github.com/claudianus/SuperLuxCore/blob/main/doc/engineering/pgic-beams.md",
      "badge": {
        "en": "CPU · GPU paths",
        "ko": "CPU · GPU 경로"
      },
      "image": "assets/studio/atmosphere.jpg",
      "scene": "atmosphere",
      "technical": false,
      "position": [
        46,
        61
      ],
      "visualNote": {
        "en": "Native Apple Metal artwork providing scene context. Feature controls and coverage are documented here; this image is not an isolated algorithm test or a performance benchmark.",
        "ko": "실제 Apple Metal 렌더로 보는 사용 장면입니다. 기능의 제어와 지원 범위는 문서에서 확인할 수 있으며, 이미지는 해당 알고리즘의 단독 검증이나 속도 측정이 아닙니다."
      }
    }
  ],
  "status": [
    [
      {
        "en": "Backends have different coverage",
        "ko": "백엔드마다 지원 범위가 다릅니다"
      },
      {
        "en": "CPU, OpenCL/CUDA and Metal share the engine, but not every new feature supports every engine. Metal is Apple-only; ReSTIR PT/SSP have CPU/GPU paths; LPE excludes bidirectional/light-tracing paths.",
        "ko": "CPU, OpenCL/CUDA, Metal은 엔진을 공유하지만 모든 새 기능이 모든 경로를 지원하지는 않습니다. Metal은 Apple 전용이며, ReSTIR PT는 CPU/GPU 선택 모드, SSP는 CPU/GPU 라이트 트레이싱을 지원합니다. LPE는 양방향·라이트 트레이싱 경로를 제외합니다."
      }
    ],
    [
      {
        "en": "Cycles reuse is practical, not complete",
        "ko": "Cycles 씬 재사용은 실용적인 호환을 목표로 합니다"
      },
      {
        "en": "Supported graphs render through the adapter with SuperLuxCore’s own light transport. OSL, baking, broad compositing/workflow equivalence and full SSS equivalence remain outside verified coverage.",
        "ko": "지원 그래프는 어댑터를 통해 SuperLuxCore의 빛 전달 방식으로 렌더합니다. OSL, 베이킹, 폭넓은 합성·작업 흐름 동등성과 SSS 전체 동등성은 검증 범위 밖입니다."
      }
    ],
    [
      {
        "en": "Publication and runtime acceptance are separate",
        "ko": "배포 완료와 실행 검수는 구분합니다"
      },
      {
        "en": "The signed native 2.11.27 wheel and focused VDB paths have installed-package evidence. The published Blender source bundle still includes four source/installed differences in normal/SSS/export/viewport areas without complete runtime acceptance.",
        "ko": "서명된 네이티브 2.11.27 wheel과 VDB 경로에는 실제 설치본 검증 근거가 있습니다. 공개 Blender 소스 번들은 Normal·SSS·내보내기·뷰포트 영역의 네 소스/설치본 차이에 대한 전체 실행 검수가 남아 있습니다."
      }
    ],
    [
      {
        "en": "Opt-in research stays visible",
        "ko": "선택 기능과 연구 기능을 명확히 표시합니다"
      },
      {
        "en": "Wavefront queues, ReSTIR PT and clamping are deliberate options; Vulkan is experimental. Regularization also has an automatic mode in eligible caustic scenes. Quality, convergence and performance depend on the scene and settings.",
        "ko": "웨이브프런트 큐·ReSTIR PT·기여값 제한은 목적에 맞게 선택하는 옵션이며 Vulkan은 실험 기능입니다. 정규화에는 적합한 커스틱 씬의 자동 모드도 있습니다. 품질·수렴·성능은 장면과 설정에 따라 달라집니다."
      }
    ]
  ],
  "faq": [
    [
      {
        "en": "Is SuperLuxCore a replacement for Cycles?",
        "ko": "SuperLuxCore는 Cycles의 대체를 목적으로 하나요?"
      },
      {
        "en": "It is an independent renderer built on LuxCoreRender. The Cycles adapter is a convenience for reusing supported scenes without manual node conversion. It aims for the same visual/logical meaning with SuperLuxCore quality, not bit-identical pixels.",
        "ko": "LuxCoreRender를 기반으로 하는 독립 렌더러입니다. Cycles 어댑터는 지원되는 씬을 수동 노드 변환 없이 재사용하기 위한 편의 기능입니다. SuperLuxCore 품질로 시각적·논리적 의미를 유지하는 것이 목적이며 픽셀 단위 동일성을 목표로 하지 않습니다."
      }
    ],
    [
      {
        "en": "Which download should I use?",
        "ko": "어떤 파일을 설치하면 되나요?"
      },
      {
        "en": "Use the Blender extension ZIP for your operating system and CPU architecture. Apple M-series Macs use macOS Apple silicon; Intel Macs use macOS Intel. Native wheels are for Python/API users rather than the normal Blender install.",
        "ko": "운영체제와 CPU 구조에 맞는 Blender 확장 ZIP을 사용하세요. Apple M 시리즈는 macOS Apple Silicon, Intel Mac은 macOS Intel입니다. 네이티브 wheel은 일반 Blender 설치보다 Python/API 사용자를 위한 파일입니다."
      }
    ],
    [
      {
        "en": "Are all the images new renders?",
        "ko": "모든 이미지가 이번 버전의 새 렌더인가요?"
      },
      {
        "en": "Yes. All thirteen scenes were rebuilt for this October 11 refresh, using 32 credited CC0 model, PBR texture and HDRI families. Both official fork-baseline and current SuperLuxCore binaries were rendered where labelled. Beauty uses OCIO ACES 2.0; technical passes show actual scene data. Asset credits, scene recipes and render fingerprints are available below.",
        "ko": "네. 10월 11일 개편을 위해 13개 씬을 모두 다시 만들고, 출처를 명시한 CC0 모델·PBR 텍스처·HDRI 32종을 활용했습니다. 표시된 비교에는 공식 포크 기준 버전과 현재 SuperLuxCore 바이너리를 각각 사용했습니다. 작품에는 OCIO ACES 2.0을, 기술 패스에는 실제 씬 데이터를 표시합니다. 에셋 크레딧·장면 레시피·렌더 지문도 공개합니다."
      }
    ],
    [
      {
        "en": "Does every new feature make rendering faster?",
        "ko": "새 기능은 항상 렌더를 더 빠르게 하나요?"
      },
      {
        "en": "No universal speedup is claimed. Many additions improve expressive range, optical behavior or workflow. Sampling, guiding, caustic and backend performance must be compared on matched scenes, settings, hardware and image quality.",
        "ko": "일괄적인 속도 향상을 주장하지 않습니다. 여러 추가 기능은 표현 범위, 광학 동작과 작업 흐름을 개선합니다. 샘플링, 가이딩, 커스틱과 백엔드 성능은 같은 장면·설정·하드웨어·이미지 품질 조건에서 비교해야 합니다."
      }
    ]
  ],
  "copy": {
    "title": {
      "en": "SuperLuxCore — Physically based rendering for Blender.",
      "ko": "SuperLuxCore — Blender를 위한 물리 기반 렌더러."
    },
    "description": {
      "en": "An open-source renderer for Blender with native Apple Metal, physically based materials, optical transport and production passes.",
      "ko": "네이티브 Apple Metal, 물리 기반 재질, 광학 전달과 제작 패스를 제공하는 Blender용 오픈 소스 렌더러입니다."
    },
    "skip": {
      "en": "Skip to content",
      "ko": "본문 바로가기"
    },
    "language": {
      "en": "Language",
      "ko": "언어"
    },
    "auto": {
      "en": "Auto",
      "ko": "자동"
    },
    "work": {
      "en": "Selected work",
      "ko": "작품"
    },
    "differences": {
      "en": "The difference",
      "ko": "눈으로 보는 차이"
    },
    "toolkit": {
      "en": "Visual toolkit",
      "ko": "비주얼 툴킷"
    },
    "download": {
      "en": "Get SuperLuxCore",
      "ko": "다운로드"
    },
    "heroTag": {
      "en": "PRODUCTION RENDERER LAB / BLENDER",
      "ko": "프로덕션 렌더러 랩 / BLENDER"
    },
    "heroFirst": {
      "en": "Physically based",
      "ko": "물리 기반"
    },
    "heroSecond": {
      "en": "rendering.",
      "ko": "렌더링."
    },
    "heroBody": {
      "en": "Trace light. Keep the material.\nNative CPU and Metal paths, inside Blender.",
      "ko": "빛을 추적하고, 재질을 보존합니다.\nBlender 안에서 CPU와 Metal 경로를 사용하세요."
    },
    "heroButton": {
      "en": "Explore the renderer",
      "ko": "렌더러 살펴보기"
    },
    "heroDownload": {
      "en": "Download extension",
      "ko": "확장 설치"
    },
    "renderCredit": {
      "en": "Rendered with SuperLuxCore · ACES 2.0",
      "ko": "SuperLuxCore 실제 렌더 · ACES 2.0"
    },
    "manifesto": {
      "en": "One renderer.\nMeasured in light.",
      "ko": "하나의 렌더러.\n빛으로 증명합니다."
    },
    "manifestoBody": {
      "en": "From diffraction and rough conductors to fibre, volumes and production passes. The image is the first proof; the data stays attached.",
      "ko": "회절과 거친 도체부터 섬유, 볼륨, 제작 패스까지. 이미지를 먼저 보여주고 실제 데이터는 함께 보존합니다."
    },
    "workEyebrow": {
      "en": "01 / RENDER DISCIPLINES",
      "ko": "01 / 렌더링 분야"
    },
    "workHeading": {
      "en": "Six scenes.\nSix light problems.",
      "ko": "여섯 장면.\n여섯 가지 빛의 문제."
    },
    "workIntro": {
      "en": "A focused set of native renders. Each discipline isolates a different material, transport or production question.",
      "ko": "각기 다른 재질, 빛의 전달, 제작 문제를 하나씩 보여주는 네이티브 렌더 모음입니다."
    },
    "capabilityEyebrow": {
      "en": "CAPABILITY MAP / CURRENT BUILD",
      "ko": "기능 맵 / 현재 빌드"
    },
    "capabilityHeading": {
      "en": "Built for the hard parts of light.",
      "ko": "빛의 어려운 문제를 위해 만들었습니다."
    },
    "capabilityIntro": {
      "en": "Read the renderer by the work it can do. Every label links to a documented control surface.",
      "ko": "렌더러가 실제로 해결하는 문제로 기능을 살펴보세요. 각 항목은 문서화된 제어로 연결됩니다."
    },
    "archiveIntro": {
      "en": "Open the complete scene set, passes and render notes.",
      "ko": "전체 씬과 패스, 렌더 노트를 엽니다."
    },
    "archiveOpen": {
      "en": "Open scene archive",
      "ko": "씬 아카이브 열기"
    },
    "archiveClose": {
      "en": "Close scene archive",
      "ko": "씬 아카이브 닫기"
    },
    "atlasEyebrow": {
      "en": "02 / MATERIAL ATLAS",
      "ko": "02 / 재질 아틀라스"
    },
    "atlasHeading": {
      "en": "Four surfaces.\nFour ways light changes.",
      "ko": "네 가지 표면.\n빛이 달라지는 네 가지 방식."
    },
    "atlasIntro": {
      "en": "Start with the visible mechanism: dispersion, film interference, transmission or fibre scattering. Open any plate for the documented control surface.",
      "ko": "보이는 현상부터 시작하세요. 분산, 박막 간섭, 투과, 섬유 산란을 한 장씩 보여줍니다. 각 패널에서 문서화된 제어를 확인할 수 있습니다."
    },
    "atlasNote": {
      "en": "Feature plates are visual evidence and scope references, not isolated performance benchmarks.",
      "ko": "기능 플레이트는 시각적 증거와 지원 범위를 보여주며, 독립 성능 벤치마크가 아닙니다."
    },
    "viewProject": {
      "en": "Open project",
      "ko": "작품 열기"
    },
    "allWork": {
      "en": "Scene archive",
      "ko": "씬 아카이브"
    },
    "product": {
      "en": "Product",
      "ko": "제품"
    },
    "space": {
      "en": "Spaces",
      "ko": "공간"
    },
    "material": {
      "en": "Materials",
      "ko": "소재"
    },
    "compareEyebrow": {
      "en": "02 / THE DIFFERENCE",
      "ko": "02 / 차이를 직접 보세요"
    },
    "compareHeading": {
      "en": "New possibilities.\nVisible differences.",
      "ko": "추가된 가능성.\n눈에 보이는 차이."
    },
    "compareIntro": {
      "en": "Get close to the material. Drag the divider to see the difference under the same light.",
      "ko": "재질에 가까이 다가가 보세요. 같은 빛 아래에서, 경계선을 움직이며 차이를 확인하세요."
    },
    "compareDrag": {
      "en": "Drag to compare",
      "ko": "드래그해서 비교"
    },
    "before": {
      "en": "LuxCoreRender 2.11.2",
      "ko": "LuxCoreRender 2.11.2"
    },
    "after": {
      "en": "SuperLuxCore 2.11.27",
      "ko": "SuperLuxCore 2.11.27"
    },
    "comparisonDetails": {
      "en": "How this comparison was made",
      "ko": "비교 구성 보기"
    },
    "toolkitEyebrow": {
      "en": "03 / THE VISUAL TOOLKIT",
      "ko": "03 / 모든 기능을 이미지로"
    },
    "toolkitHeading": {
      "en": "Renderer index / documented scope.",
      "ko": "렌더러 인덱스 · 지원 범위."
    },
    "toolkitIntro": {
      "en": "Read the native output first. Then inspect the material, pass, backend and documented scope.",
      "ko": "먼저 네이티브 출력을 확인하세요. 이어서 재질, 패스, 백엔드와 문서화된 범위를 살펴보세요."
    },
    "search": {
      "en": "Find a feature or a material…",
      "ko": "기능이나 소재 찾기…"
    },
    "searchLabel": {
      "en": "Search the visual toolkit",
      "ko": "비주얼 툴킷 검색"
    },
    "featureCount": {
      "en": "visual entries",
      "ko": "개의 시각 자료"
    },
    "noResults": {
      "en": "No matching features. Try another word.",
      "ko": "일치하는 기능이 없습니다. 다른 검색어를 입력하세요."
    },
    "docs": {
      "en": "Explore the controls ↗",
      "ko": "제어 방법 보기 ↗"
    },
    "detail": {
      "en": "The detail",
      "ko": "디테일 보기"
    },
    "technical": {
      "en": "ACTUAL RENDER PASS",
      "ko": "실제 렌더 패스"
    },
    "context": {
      "en": "VISUAL CONTEXT",
      "ko": "장면으로 보기"
    },
    "passNote": {
      "en": "Technical passes visualize scene data. Beauty images use ACES 2.0; normals, IDs and coverage are data views.",
      "ko": "기술 패스는 실제 씬 데이터를 시각화합니다. 작품은 ACES 2.0을 적용하며, 노멀·ID·커버리지는 데이터 보기입니다."
    },
    "downloadEyebrow": {
      "en": "04 / YOUR NEXT IMAGE",
      "ko": "04 / 다음 작품을 시작하세요"
    },
    "downloadFirst": {
      "en": "Your imagination.",
      "ko": "당신의 상상."
    },
    "downloadSecond": {
      "en": "Our light.",
      "ko": "그것을 비추는 빛."
    },
    "downloadBody": {
      "en": "Free. Open source. Inside Blender.\nChoose your platform and start creating.",
      "ko": "무료, 오픈 소스, Blender 안에서.\n플랫폼을 선택하고 작품을 시작하세요."
    },
    "downloadZip": {
      "en": "Download extension ZIP ↓",
      "ko": "확장 ZIP 다운로드 ↓"
    },
    "manual": {
      "en": "Installation guide",
      "ko": "설치 가이드"
    },
    "release": {
      "en": "Latest release",
      "ko": "최신 릴리스"
    },
    "installOne": {
      "en": "Download",
      "ko": "다운로드"
    },
    "installOneBody": {
      "en": "Choose the ZIP for your platform.",
      "ko": "플랫폼에 맞는 ZIP을 선택하세요."
    },
    "installTwo": {
      "en": "Install in Blender",
      "ko": "Blender에 설치"
    },
    "installTwoBody": {
      "en": "Preferences → Get Extensions → Install from Disk.",
      "ko": "환경설정 → 확장 가져오기 → 디스크에서 설치."
    },
    "installThree": {
      "en": "Make your image",
      "ko": "작품 시작"
    },
    "installThreeBody": {
      "en": "Select SuperLuxCore. Preview or press F12.",
      "ko": "SuperLuxCore 선택. 뷰포트 또는 F12로 렌더."
    },
    "support": {
      "en": "Coverage & practical details",
      "ko": "지원 범위 · 사용 안내"
    },
    "faq": {
      "en": "A few useful answers.",
      "ko": "시작하기 전에."
    },
    "close": {
      "en": "Close",
      "ko": "닫기"
    },
    "previous": {
      "en": "Previous project",
      "ko": "이전 작품"
    },
    "next": {
      "en": "Next project",
      "ko": "다음 작품"
    },
    "full": {
      "en": "Open original image ↗",
      "ko": "원본 이미지 열기 ↗"
    },
    "featureButton": {
      "en": "See this feature",
      "ko": "관련 기능 보기"
    },
    "sceneContext": {
      "en": "Scene context",
      "ko": "사용 장면"
    },
    "footerLine": {
      "en": "Built on LuxCoreRender. Made for your next image.",
      "ko": "LuxCoreRender를 기반으로. 당신의 다음 작품을 위해."
    },
    "credits": {
      "en": "Image credits & render notes",
      "ko": "이미지 크레딧 · 렌더 정보"
    },
    "scope": {
      "en": "Comparisons use the 2.11.2 fork baseline. New material models are shown against clearly labelled baseline alternatives; results depend on the scene and controls.",
      "ko": "비교 기준은 포크 출발점 2.11.2입니다. 추가된 재질 모델은 표시된 기존 대안과 비교하며, 결과는 씬과 설정에 따라 달라집니다."
    },
    "free": {
      "en": "OPEN SOURCE. FREE TO CREATE.",
      "ko": "오픈 소스. 창작은 자유롭게."
    },
    "artistNote": {
      "en": "Native SuperLuxCore artwork. CC0 models, PBR textures & HDRIs credited to their Poly Haven creators.",
      "ko": "SuperLuxCore 실제 렌더. CC0 모델·PBR 텍스처·HDRI: Poly Haven 제작자 크레딧 참조."
    }
  },
  "projects": [
    {
      "id": "beauty",
      "name": "MORNING",
      "area": {
        "en": "Still life / breakfast",
        "ko": "정물 · 아침 식탁"
      },
      "description": {
        "en": "Scanned pastry, floral porcelain, woven linen and fresh coffee.",
        "ko": "스캔한 페이스트리, 꽃무늬 도자기, 리넨과 커피."
      },
      "tags": [
        "Food photogrammetry",
        "OpenPBR",
        "Micro-surface detail"
      ],
      "features": [
        "sss",
        "ssspresets"
      ],
      "image": "assets/studio/beauty.jpg",
      "group": "product"
    },
    {
      "id": "heritage",
      "name": "ANALOG",
      "area": {
        "en": "Product / heritage camera",
        "ko": "제품 · 빈티지 카메라"
      },
      "description": {
        "en": "Detailed camera hardware, leather, paper and softbox reflections.",
        "ko": "카메라의 기계적 디테일, 가죽과 종이, 정돈된 소프트박스 반사."
      },
      "tags": [
        "Detailed CC0 model",
        "PBR metal & plastic",
        "Depth of field"
      ],
      "features": [
        "cryptomatte",
        "displacement"
      ],
      "image": "assets/studio/heritage.jpg",
      "group": "product"
    },
    {
      "id": "perfume",
      "name": "VINEA",
      "area": {
        "en": "Product / wine editorial",
        "ko": "제품 · 와인 에디토리얼"
      },
      "description": {
        "en": "Scanned wine packaging and printed labels, polished lacquer, fruit and controlled studio reflections.",
        "ko": "스캔한 와인 패키징과 인쇄 라벨, 매끈한 래커 마감, 과일과 정돈된 스튜디오 반사."
      },
      "tags": [
        "Scanned packaging",
        "Polished lacquer",
        "Fruit scans"
      ],
      "features": [
        "openpbr"
      ],
      "image": "assets/studio/perfume.jpg",
      "group": "product",
      "renderNote": {
        "en": "Metal artwork · intentionally opaque lacquered packaging; not a liquid-absorption demonstration.",
        "ko": "Metal 렌더 · 불투명 래커 패키징 연출이며 액체의 빛 흡수 시연이 아닙니다."
      }
    },
    {
      "id": "music",
      "name": "REWIND",
      "area": {
        "en": "Product / listening desk",
        "ko": "제품 · 리스닝 데스크"
      },
      "description": {
        "en": "Analogue electronics meet the colour of a real diffraction grating.",
        "ko": "빈티지 오디오와 실제 회절 격자가 만드는 색."
      },
      "tags": [
        "Diffraction CD",
        "120 mm proportions",
        "Detailed PBR electronics"
      ],
      "features": [
        "diffraction"
      ],
      "image": "assets/studio/music.jpg",
      "group": "product"
    },
    {
      "id": "jewelry",
      "name": "THE NAVIGATOR",
      "area": {
        "en": "Product / personal objects",
        "ko": "제품 · 개인 소품"
      },
      "description": {
        "en": "An engraved compass, spectacles and worn paper in a quiet close-up.",
        "ko": "음각 나침반과 안경, 손때 묻은 종이를 담은 근접 촬영."
      },
      "tags": [
        "Engraved metal",
        "Tangent normals",
        "Physical camera"
      ],
      "features": [
        "sellmeier",
        "mnee"
      ],
      "image": "assets/studio/jewelry.jpg",
      "group": "product"
    },
    {
      "id": "audio",
      "name": "PATINA",
      "area": {
        "en": "Material / brass still life",
        "ko": "소재 · 황동 정물"
      },
      "description": {
        "en": "Engraved vessels, weathered brass and textured timber.",
        "ko": "섬세한 음각, 오래된 황동과 실제 목재의 질감."
      },
      "tags": [
        "Rough conductor",
        "Multi-scatter compensation",
        "PBR maps"
      ],
      "features": [
        "multiscatter",
        "ggx"
      ],
      "image": "assets/studio/audio.jpg",
      "group": "product"
    },
    {
      "id": "pearl",
      "name": "PORCELAIN",
      "area": {
        "en": "Material / ceramic & film",
        "ko": "소재 · 도자기와 박막"
      },
      "description": {
        "en": "Blue-and-white porcelain and the delicate colour of soap film.",
        "ko": "청화 도자기와 비눗막의 섬세한 색."
      },
      "tags": [
        "Thin-film interference",
        "Glazed ceramic",
        "Native glass"
      ],
      "features": [
        "thinfilm",
        "openpbr"
      ],
      "image": "assets/studio/pearl.jpg",
      "group": "material"
    },
    {
      "id": "textile",
      "name": "SOFT HOURS",
      "area": {
        "en": "Material / wool & leather",
        "ko": "소재 · 울과 가죽"
      },
      "description": {
        "en": "A curled sheepskin groom on a detailed leather lounge chair.",
        "ko": "세밀한 가죽 라운지 체어 위에 놓인 곱슬 양모."
      },
      "tags": [
        "Huang fibres",
        "Native strand geometry",
        "Leather PBR"
      ],
      "features": [
        "hair",
        "sheen"
      ],
      "image": "assets/studio/textile.jpg",
      "group": "material"
    },
    {
      "id": "interior",
      "name": "AFTERNOON",
      "area": {
        "en": "Space / living room",
        "ko": "공간 · 거실"
      },
      "description": {
        "en": "Upholstery, foliage and timber in a furnished afternoon room.",
        "ko": "가구와 식물, 목재가 어우러진 오후의 거실."
      },
      "tags": [
        "Metric architecture",
        "PBR upholstery",
        "Cryptomatte"
      ],
      "features": [
        "proctextures",
        "guiding"
      ],
      "image": "assets/studio/interior.jpg",
      "group": "space"
    },
    {
      "id": "architecture",
      "name": "COURTYARD",
      "area": {
        "en": "Space / shaded terrace",
        "ko": "공간 · 그늘진 테라스"
      },
      "description": {
        "en": "A furnished terrace, slatted roof and a real dielectric water surface.",
        "ko": "가구와 차양이 있는 테라스, 실제 투과와 반사를 계산한 물."
      },
      "tags": [
        "Daylight",
        "Water refraction",
        "Photographic surfaces"
      ],
      "features": [
        "metal",
        "outofcore"
      ],
      "image": "assets/studio/architecture.jpg",
      "group": "space"
    },
    {
      "id": "atmosphere",
      "name": "SLOW LIGHT",
      "area": {
        "en": "Space / reading room",
        "ko": "공간 · 독서 공간"
      },
      "description": {
        "en": "Window light moves through a furnished room and a participating medium.",
        "ko": "가구가 있는 공간과 참여 매질을 통과하는 창의 빛."
      },
      "tags": [
        "Volume scattering",
        "Window structure",
        "Daylight"
      ],
      "features": [
        "volume",
        "photonbeams"
      ],
      "image": "assets/studio/atmosphere.jpg",
      "group": "space"
    },
    {
      "id": "fan",
      "name": "AIR / STILL",
      "area": {
        "en": "Space / motion study",
        "ko": "공간 · 모션 스터디"
      },
      "description": {
        "en": "A real wooden ceiling fan photographed in a furnished room.",
        "ko": "가구가 있는 공간에서 촬영한 실제 목재 천장 팬."
      },
      "tags": [
        "Native motion blur",
        "Wood grain",
        "Metric scene"
      ],
      "features": [
        "motion",
        "temporal"
      ],
      "image": "assets/studio/fan.jpg",
      "group": "product"
    },
    {
      "id": "prism",
      "name": "SPECTRUM",
      "area": {
        "en": "Optics / flint glass",
        "ko": "광학 · 플린트 유리"
      },
      "description": {
        "en": "A precision optical prism with wavelength-dependent flint glass.",
        "ko": "파장에 따라 굴절률이 달라지는 플린트 유리 프리즘."
      },
      "image": "assets/studio/prism.jpg",
      "group": "material",
      "tags": [
        "Sellmeier N-SF6",
        "RGB dispersion",
        "Optical edges"
      ],
      "features": [
        "sellmeier"
      ],
      "renderNote": {
        "en": "Metal RGB wavelength-sampled dispersion · N-SF6 Sellmeier glass.",
        "ko": "Metal RGB 파장 샘플링 분산 · N-SF6 Sellmeier 유리."
      }
    }
  ],
  "comparisons": [
    {
      "id": "diffraction",
      "tab": {
        "en": "CD / diffraction",
        "ko": "CD · 회절"
      },
      "title": {
        "en": "This rainbow is a shader.",
        "ko": "이 무지개는 셰이더입니다."
      },
      "body": {
        "en": "A compact disc under directional studio light. The new diffraction-grating material separates light into spectral orders. The upstream baseline uses a silver metal approximation — it has no equivalent diffraction shader.",
        "ko": "좁은 스튜디오 조명 아래의 CD. 새 회절 격자 재질이 빛을 파장별 회절 차수로 나눕니다. 기준 버전은 은 금속 근사를 사용하며, 동등한 회절 셰이더가 없습니다."
      },
      "before": "assets/studio/music-baseline-detail.jpg",
      "after": "assets/studio/music-detail.jpg",
      "left": {
        "en": "UPSTREAM / SILVER METAL APPROXIMATION",
        "ko": "업스트림 / 은 금속 근사"
      },
      "right": {
        "en": "SUPERLUXCORE / DIFFRACTION SHADER",
        "ko": "SUPERLUXCORE / 회절 셰이더"
      },
      "scope": {
        "en": "Official upstream binary with metal2 silver. Same geometry, lights, seed, samples, exposure and display transform. This is a new-shader comparison, not spectral on/off.",
        "ko": "공식 업스트림 바이너리의 metal2 은 재질. 형상·조명·시드·샘플·노출·색 변환을 맞췄습니다. 스펙트럴 켜기/끄기 비교가 아닌, 추가된 셰이더 비교입니다."
      },
      "fullBefore": "assets/studio/music-baseline.jpg",
      "fullAfter": "assets/studio/music.jpg"
    },
    {
      "id": "energy",
      "tab": {
        "en": "Metal / energy",
        "ko": "금속 · 에너지"
      },
      "title": {
        "en": "Roughness deserves radiance.",
        "ko": "거칠어도, 빛을 잃지 않게."
      },
      "body": {
        "en": "Engraved brass vessels under a broad window. At high roughness, the added multiple-scattering correction preserves reflected energy without changing the light or exposure.",
        "ko": "큰 창 아래의 새겨진 황동 기물. 거칠기가 높을 때 추가된 다중 산란 보정이 조명이나 노출을 바꾸지 않고 반사 에너지를 보존합니다."
      },
      "before": "assets/studio/audio-baseline-detail.jpg",
      "after": "assets/studio/audio-detail.jpg",
      "left": {
        "en": "UPSTREAM / SINGLE SCATTERING",
        "ko": "업스트림 / 단일 산란"
      },
      "right": {
        "en": "SUPERLUXCORE / MULTIPLE SCATTERING",
        "ko": "SUPERLUXCORE / 다중 산란 보정"
      },
      "scope": {
        "en": "Official LuxCoreRender 2.11.2 vs SuperLuxCore 2.11.27. The same textured Disney brass uses roughness 0.98; multibounce is enabled only in the fork. Camera, light, target samples and display match.",
        "ko": "공식 LuxCoreRender 2.11.2와 SuperLuxCore 2.11.27. 동일한 텍스처를 쓰는 Disney 황동의 거칠기는 0.98, 포크에서만 multibounce를 켰습니다. 카메라·조명·목표 샘플·색 변환은 같습니다."
      },
      "fullBefore": "assets/studio/audio-baseline.jpg",
      "fullAfter": "assets/studio/audio.jpg"
    },
    {
      "id": "hair",
      "tab": {
        "en": "Hair / fibre scattering",
        "ko": "헤어 · 섬유 산란"
      },
      "title": {
        "en": "The softness is in the scattering.",
        "ko": "부드러움은 빛의 산란에서."
      },
      "body": {
        "en": "The same curled sheepskin groom on a leather lounge chair. SuperLuxCore uses the Huang fibre model; the upstream reference uses a labelled Glossy2 surface approximation.",
        "ko": "가죽 라운지 체어 위의 동일한 곱슬 양모입니다. SuperLuxCore는 Huang 섬유 모델을, 업스트림 기준 렌더는 명시된 Glossy2 표면 근사를 사용합니다."
      },
      "before": "assets/studio/textile-baseline-detail.jpg",
      "after": "assets/studio/textile-detail.jpg",
      "left": {
        "en": "UPSTREAM / GLOSSY STRAND APPROXIMATION",
        "ko": "업스트림 / 일반 광택 스트랜드 근사"
      },
      "right": {
        "en": "SUPERLUXCORE / HUANG FIBRE SHADER",
        "ko": "SUPERLUXCORE / Huang 섬유 셰이더"
      },
      "scope": {
        "en": "Official upstream uses a glossy2 surface approximation on the identical strand geometry. The fork uses the added Huang hair BSDF. Shared lights, sampling, exposure and display; this is a shader comparison, not the best possible upstream artistic setup.",
        "ko": "공식 업스트림에서는 같은 스트랜드 형상에 glossy2 표면 근사를, 포크에서는 추가된 Huang 헤어 BSDF를 사용합니다. 조명·샘플·노출·색 변환을 공유하는 셰이더 비교입니다. 업스트림에서 가능한 모든 아트 디렉션의 한계를 뜻하지 않습니다."
      },
      "fullBefore": "assets/studio/textile-baseline.jpg",
      "fullAfter": "assets/studio/textile.jpg"
    }
  ],
  "hero": "prism"
};

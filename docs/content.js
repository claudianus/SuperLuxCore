window.SLC_DATA = {
  "version": "2.11.27",
  "date": "2026-10-12",
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
  "forkDelta": {
    "snapshot": {
      "baseline": "LuxCoreRender 2.11.2",
      "baselineSha": "e9ced7e7a",
      "engineCommits": 509,
      "forkCommits": 517,
      "upstreamCommits": 51,
      "areas": 8,
      "controls": 48,
      "scenes": 4,
      "historicalScenes": 13,
      "upstreamHead": "4209d6537",
      "date": "2026-10-12"
    },
    "areas": [
      {
        "id": "transport",
        "count": 148,
        "feature": "restirdi",
        "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
        "badge": {
          "en": "EXTENDED",
          "ko": "확장"
        },
        "title": {
          "en": "Hard light paths get more choices.",
          "ko": "어려운 빛의 경로에 선택지가 생깁니다."
        },
        "outcome": {
          "en": "Reservoirs, guiding and caustic solvers give artists a way to target the paths that matter.",
          "ko": "Reservoir·guiding·커스틱 솔버로 중요한 빛의 경로를 직접 겨냥할 수 있습니다."
        },
        "scope": {
          "en": "Opt-in and scene-dependent; no blanket speed claim.",
          "ko": "선택 기능이며 장면에 따라 다릅니다. 일괄적인 속도 향상은 주장하지 않습니다."
        },
        "chips": [
          "CPU",
          "GPU",
          "OPT-IN"
        ]
      },
      {
        "id": "volumes",
        "count": 86,
        "feature": "volume",
        "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
        "badge": {
          "en": "EXTENDED",
          "ko": "확장"
        },
        "title": {
          "en": "Volumes keep their depth.",
          "ko": "볼륨의 깊이를 잃지 않습니다."
        },
        "outcome": {
          "en": "Heterogeneous density, phase functions and photon beams make smoke, glass and atmosphere read as light, not a flat tint.",
          "ko": "이질 밀도·위상 함수·광자 빔으로 연기와 유리, 대기가 평면 색이 아니라 빛으로 읽힙니다."
        },
        "scope": {
          "en": "Volume paths vary by backend and material setup.",
          "ko": "볼륨 경로의 지원 범위는 백엔드와 재질 설정에 따라 다릅니다."
        },
        "chips": [
          "CPU",
          "METAL",
          "VOLUME"
        ]
      },
      {
        "id": "infrastructure",
        "count": 80,
        "feature": "outofcore",
        "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
        "badge": {
          "en": "POLISH",
          "ko": "기반 개선"
        },
        "title": {
          "en": "Large scenes use the memory they need.",
          "ko": "큰 씬은 필요한 메모리만 사용합니다."
        },
        "outcome": {
          "en": "Out-of-core geometry, image staging and bounded task memory keep production scenes workable as they grow.",
          "ko": "아웃 오브 코어 지오메트리·이미지 스테이징·제한된 태스크 메모리로 씬이 커져도 작업을 이어갑니다."
        },
        "scope": {
          "en": "Resource behavior is measured by scene and backend, not a render-quality shortcut.",
          "ko": "리소스 동작은 장면과 백엔드별로 측정하며 렌더 품질을 낮추는 지름길이 아닙니다."
        },
        "chips": [
          "CPU",
          "GPU",
          "RESOURCE"
        ]
      },
      {
        "id": "hardware",
        "count": 74,
        "feature": "metal",
        "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
        "badge": {
          "en": "NEW PATH",
          "ko": "새 경로"
        },
        "title": {
          "en": "Apple GPU is a native render path.",
          "ko": "Apple GPU가 네이티브 렌더 경로가 됩니다."
        },
        "outcome": {
          "en": "Metal compute, hardware ray tracing, instancing, motion and native curves meet in the same renderer.",
          "ko": "Metal 연산·하드웨어 레이 트레이싱·인스턴싱·모션·네이티브 커브를 하나의 렌더러에서 사용합니다."
        },
        "scope": {
          "en": "Supported Apple hardware; the motion scene is credited context, not a speed test.",
          "ko": "지원되는 Apple 하드웨어 범위입니다. 모션 장면은 출처가 있는 맥락이며 속도 측정이 아닙니다."
        },
        "chips": [
          "METAL",
          "HWRT",
          "CURVES"
        ]
      },
      {
        "id": "workflow",
        "count": 60,
        "feature": "cryptomatte",
        "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
        "badge": {
          "en": "WORKFLOW",
          "ko": "제작 흐름"
        },
        "title": {
          "en": "The beauty render keeps its evidence.",
          "ko": "작품 렌더가 근거를 함께 남깁니다."
        },
        "outcome": {
          "en": "Cryptomatte, LPE, variance, motion and depth channels turn one render into a usable finishing set.",
          "ko": "Cryptomatte·LPE·분산·모션·깊이 채널로 한 번의 렌더가 실제 마무리 세트가 됩니다."
        },
        "scope": {
          "en": "Pass availability follows the selected engine and scene.",
          "ko": "패스 제공 여부는 선택한 엔진과 장면에 따라 달라집니다."
        },
        "chips": [
          "AOV",
          "LPE",
          "BLENDER"
        ]
      },
      {
        "id": "materials",
        "count": 58,
        "feature": "multiscatter",
        "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
        "badge": {
          "en": "EXTENDED",
          "ko": "확장"
        },
        "title": {
          "en": "Surfaces stop losing the light they should keep.",
          "ko": "표면이 간직해야 할 빛을 덜 잃습니다."
        },
        "outcome": {
          "en": "OpenPBR, transmission, rough conductors, multiple scattering, sheen and hair models widen the material vocabulary.",
          "ko": "OpenPBR·투과·거친 도체·다중 산란·sheen·헤어 모델로 재질 표현 범위를 넓힙니다."
        },
        "scope": {
          "en": "Several controls are opt-in or backend-specific; the comparison recipe is disclosed.",
          "ko": "일부 제어는 선택 기능 또는 백엔드별 기능이며 비교 레시피를 공개합니다."
        },
        "chips": [
          "OPENPBR",
          "ENERGY",
          "FIBRE"
        ]
      },
      {
        "id": "release",
        "count": 58,
        "feature": "blender",
        "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
        "badge": {
          "en": "SHIP IT",
          "ko": "배포"
        },
        "title": {
          "en": "Install the renderer without archaeology.",
          "ko": "찾아 헤매지 않고 렌더러를 설치합니다."
        },
        "outcome": {
          "en": "Platform wheels, Blender extension bundles, version banners and provenance make the build you installed legible.",
          "ko": "플랫폼별 wheel·Blender 확장 번들·버전 배너·provenance로 설치한 빌드를 확인할 수 있습니다."
        },
        "scope": {
          "en": "Build and package evidence is separate from runtime feature acceptance.",
          "ko": "빌드·패키지 근거와 런타임 기능 검수는 구분합니다."
        },
        "chips": [
          "MAC",
          "WINDOWS",
          "LINUX"
        ]
      },
      {
        "id": "maintenance",
        "count": 97,
        "feature": "cycles",
        "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
        "badge": {
          "en": "POLISH",
          "ko": "다듬기"
        },
        "title": {
          "en": "The small fixes artists feel every day.",
          "ko": "매일 체감하는 작은 수정들."
        },
        "outcome": {
          "en": "Graph refreshes, camera transforms, safe bounds, deadlock fixes and clearer diagnostics remove friction between edits.",
          "ko": "그래프 갱신·카메라 변환·안전한 경계·교착 수정·진단 개선으로 편집과 렌더 사이의 마찰을 줄입니다."
        },
        "scope": {
          "en": "The area includes fixes, reverts and tooling; it is not 97 new features.",
          "ko": "수정·되돌림·도구 작업을 포함한 영역이며 새 기능 97개를 뜻하지 않습니다."
        },
        "chips": [
          "CYCLES",
          "STABILITY",
          "DIAGNOSTICS"
        ]
      }
    ],
    "spotlights": [
      {
        "feature": "diffraction",
        "image": "assets/studio/new-delta/holo_packaging-superlux-detail.png",
        "badge": {
          "en": "FORK ADDITION",
          "ko": "포크 추가"
        }
      },
      {
        "feature": "openpbr",
        "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-detail.png",
        "badge": {
          "en": "FORK ADDITION",
          "ko": "포크 추가"
        }
      },
      {
        "feature": "hair",
        "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-detail.png",
        "badge": {
          "en": "EXPERIMENTAL",
          "ko": "실험 기능"
        }
      },
      {
        "feature": "sellmeier",
        "image": "assets/studio/new-delta/smart_glass_atrium-superlux-detail.png",
        "badge": {
          "en": "EXTENDED",
          "ko": "확장"
        }
      }
    ],
    "micro": [
      {
        "kind": {
          "en": "SHADER DETAIL",
          "ko": "셰이더 디테일"
        },
        "title": {
          "en": "Smoothstep spot falloff",
          "ko": "Smoothstep 스폿 감쇠"
        },
        "body": {
          "en": "Cycles-style penumbra control survives the adapter.",
          "ko": "Cycles식 penumbra 제어를 어댑터에서 유지합니다."
        }
      },
      {
        "kind": {
          "en": "SHADER DETAIL",
          "ko": "셰이더 디테일"
        },
        "title": {
          "en": "Filter glossy and clamp controls",
          "ko": "Filter glossy·clamp 제어"
        },
        "body": {
          "en": "Glossy filtering and direct/indirect limits are explicit scene controls.",
          "ko": "Glossy 필터와 direct/indirect 제한을 씬 제어로 명시합니다."
        }
      },
      {
        "kind": {
          "en": "GRAPH FIDELITY",
          "ko": "그래프 충실도"
        },
        "title": {
          "en": "Object, generated and world position",
          "ko": "Object·Generated·World Position"
        },
        "body": {
          "en": "Procedural coordinates remain available to material graphs.",
          "ko": "프로시저럴 좌표를 머티리얼 그래프에서 계속 사용할 수 있습니다."
        }
      },
      {
        "kind": {
          "en": "GRAPH FIDELITY",
          "ko": "그래프 충실도"
        },
        "title": {
          "en": "Bump, normal and coat direction",
          "ko": "Bump·Normal·Coat 방향"
        },
        "body": {
          "en": "Normal spaces and displacement directions are mapped instead of silently dropped.",
          "ko": "노멀 공간과 디스플레이스 방향을 버리지 않고 매핑합니다."
        }
      },
      {
        "kind": {
          "en": "LIGHT",
          "ko": "조명"
        },
        "title": {
          "en": "Two-sided emission and mapped lights",
          "ko": "양면 방출·텍스처 조명"
        },
        "body": {
          "en": "Emission orientation, area spread and texture inputs are kept visible.",
          "ko": "방출 방향·에어리어 확산·텍스처 입력을 그대로 확인할 수 있습니다."
        }
      },
      {
        "kind": {
          "en": "VOLUME",
          "ko": "볼륨"
        },
        "title": {
          "en": "VDB index and reconstruction",
          "ko": "VDB 인덱스·재구성"
        },
        "body": {
          "en": "Linear and cubic density reconstruction are available for volume work.",
          "ko": "볼륨 작업에서 선형·큐빅 밀도 재구성을 사용할 수 있습니다."
        }
      },
      {
        "kind": {
          "en": "CAMERA",
          "ko": "카메라"
        },
        "title": {
          "en": "Transform cache and raster centers",
          "ko": "변환 캐시·래스터 중심"
        },
        "body": {
          "en": "Camera updates stay precise while the scene is edited.",
          "ko": "씬을 편집해도 카메라 갱신 정밀도를 유지합니다."
        }
      },
      {
        "kind": {
          "en": "BLENDER",
          "ko": "Blender"
        },
        "title": {
          "en": "Nested material refresh",
          "ko": "중첩 재질 갱신"
        },
        "body": {
          "en": "Material and mesh-light edits invalidate the right render state.",
          "ko": "재질·메시 라이트 수정이 필요한 렌더 상태만 갱신합니다."
        }
      },
      {
        "kind": {
          "en": "PASSES",
          "ko": "패스"
        },
        "title": {
          "en": "View depth and variance",
          "ko": "ViewDepth·Variance"
        },
        "body": {
          "en": "Depth and noise evidence remain available beside the beauty image.",
          "ko": "작품 옆에 깊이와 노이즈 근거를 함께 확인합니다."
        }
      },
      {
        "kind": {
          "en": "SAFETY",
          "ko": "안정성"
        },
        "title": {
          "en": "Bounds, NaN and lifecycle guards",
          "ko": "경계·NaN·수명 보호"
        },
        "body": {
          "en": "Failure paths are handled as renderer behavior, not hidden crashes.",
          "ko": "실패 경로를 숨은 충돌이 아니라 렌더러 동작으로 처리합니다."
        }
      },
      {
        "kind": {
          "en": "CLI",
          "ko": "CLI"
        },
        "title": {
          "en": "Version and help banners",
          "ko": "버전·도움말 배너"
        },
        "body": {
          "en": "The binary tells artists what they are running before the first frame.",
          "ko": "첫 프레임 전에 현재 실행 중인 빌드를 확인할 수 있습니다."
        }
      },
      {
        "kind": {
          "en": "RELEASE",
          "ko": "릴리스"
        },
        "title": {
          "en": "Wheel and bundle provenance",
          "ko": "Wheel·번들 provenance"
        },
        "body": {
          "en": "Installed artifacts carry enough identity to reproduce a report.",
          "ko": "설치된 아티팩트의 식별 정보를 보고서에 남길 수 있습니다."
        }
      }
    ]
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        35,
        32
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        48,
        49
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        61,
        66
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        44,
        48
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        57,
        65
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        40,
        47
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        53,
        64
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        36,
        46
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        49,
        63
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        62,
        45
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": true,
      "position": [
        45,
        62
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        58,
        44
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        41,
        61
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        54,
        43
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        37,
        60
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        50,
        42
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        63,
        59
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        46,
        41
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        59,
        58
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        42,
        40
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        55,
        57
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        38,
        39
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        51,
        56
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        64,
        38
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        47,
        55
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": true,
      "position": [
        60,
        37
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        43,
        54
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        56,
        36
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        39,
        53
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        52,
        35
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        35,
        52
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        48,
        34
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        61,
        51
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": true,
      "position": [
        44,
        33
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        57,
        50
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        40,
        32
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        53,
        49
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        36,
        66
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        49,
        48
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        62,
        65
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": false,
      "position": [
        45,
        47
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        58,
        64
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        41,
        46
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        54,
        63
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "scene": "holo_packaging",
      "technical": true,
      "position": [
        37,
        45
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "scene": "ev_candy_clearcoat",
      "technical": false,
      "position": [
        50,
        62
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "scene": "knit_sneaker_lab",
      "technical": false,
      "position": [
        63,
        44
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "scene": "smart_glass_atrium",
      "technical": false,
      "position": [
        46,
        61
      ],
      "visualNote": {
        "en": "New matched CPU scene study. The image is visual context; the comparison receipt records the exact upstream and fork recipes.",
        "ko": "새 CPU 매칭 장면 스터디입니다. 이미지는 시각적 맥락이며, 정확한 업스트림·포크 레시피는 비교 영수증에 기록되어 있습니다."
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
        "en": "The four new side-by-side pairs are matched CPU technical studies with Linear+Gamma display proof; the manifest records the limitation. They are separate from the renderer capability claims and do not reuse the legacy render wall.",
        "ko": "새 side-by-side 페어 4종은 Linear+Gamma 디스플레이 증거를 사용하는 CPU 기술 스터디이며, 매니페스트에 한계를 기록했습니다. 렌더러 기능 주장과 분리된 자료이며 기존 렌더 벽을 재사용하지 않습니다."
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
      "en": "SuperLuxCore matched CPU study · 64 spp",
      "ko": "SuperLuxCore CPU 매칭 스터디 · 64 spp"
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
      "en": "04 / RENDER DISCIPLINES",
      "ko": "04 / 렌더링 분야"
    },
    "workHeading": {
      "en": "Four scenes. Four material questions.",
      "ko": "네 장면. 네 가지 재질 질문."
    },
    "workIntro": {
      "en": "New authored studies cover optical packaging, automotive coat, footwear sheen and architectural glazing. Each is rendered in the same upstream/fork pair.",
      "ko": "광학 패키징·오토모티브 코트·풋웨어 sheen·건축 글레이징을 새 장면으로 만들었습니다. 모든 장면은 업스트림·포크 쌍으로 렌더했습니다."
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
    "delta": {
      "en": "Delta",
      "ko": "변화"
    },
    "deltaEyebrow": {
      "en": "FORK DELTA / 02",
      "ko": "포크 델타 / 02"
    },
    "deltaHeading": {
      "en": "What changed after LuxCoreRender 2.11.2.",
      "ko": "LuxCoreRender 2.11.2 이후, 달라진 것."
    },
    "deltaIntro": {
      "en": "509 engine commits distilled into visible renderer behavior. Open a plate to see the scene, pass, backend and scope.",
      "ko": "509개 엔진 커밋을 눈에 보이는 렌더러 동작으로 압축했습니다. 카드를 열어 장면·패스·백엔드·범위를 확인하세요."
    },
    "engineCommits": {
      "en": "ENGINE COMMITS",
      "ko": "엔진 커밋"
    },
    "areas": {
      "en": "CHANGE AREAS",
      "ko": "변경 분야"
    },
    "controls": {
      "en": "DOCUMENTED CONTROLS",
      "ko": "문서화된 제어"
    },
    "scenes": {
      "en": "NATIVE SCENES",
      "ko": "네이티브 씬"
    },
    "deltaBaseline": {
      "en": "FORK BASELINE / LUXCORERENDER 2.11.2",
      "ko": "포크 기준 / LUXCORERENDER 2.11.2"
    },
    "deltaCurrent": {
      "en": "SUPERLUXCORE 2.11.27",
      "ko": "SUPERLUXCORE 2.11.27"
    },
    "deltaUpstream": {
      "en": "UPSTREAM CHECKED 2026-10-12 / +51 COMMITS",
      "ko": "업스트림 확인 2026-10-12 / 기준 이후 +51 커밋"
    },
    "deltaCommits": {
      "en": "commits tagged in this area",
      "ko": "개 커밋이 이 분야에 포함"
    },
    "deltaSpotlightEyebrow": {
      "en": "VISIBLE FORK ADDITIONS",
      "ko": "눈에 보이는 포크 추가"
    },
    "deltaSpotlightHeading": {
      "en": "Four changes you can see immediately.",
      "ko": "바로 눈으로 확인하는 네 가지 변화."
    },
    "deltaSpotlightIntro": {
      "en": "A CD can produce ordered diffraction colour. A strand can scatter as fibre. A translucent surface can carry a measured preset. A caustic can stay legible. These are visual controls, not commit slogans.",
      "ko": "CD는 정렬된 회절 색을 만들고, 스트랜드는 섬유로 산란하며, 반투명 표면은 측정 프리셋을 사용하고, 커스틱은 형태를 유지합니다. 커밋 구호가 아니라 눈으로 확인하는 제어입니다."
    },
    "deltaMicroEyebrow": {
      "en": "SMALL BUT REAL",
      "ko": "작지만 실제로 체감되는 변화"
    },
    "deltaMicroHeading": {
      "en": "The details between the frames.",
      "ko": "프레임 사이를 매끄럽게 하는 디테일."
    },
    "deltaMicroIntro": {
      "en": "Fixes, graph semantics, diagnostics and release details are part of the renderer artists use. The complete 509-commit inventory stays available below.",
      "ko": "수정·그래프 의미·진단·릴리스 디테일까지 아티스트가 사용하는 렌더러의 일부입니다. 509개 전체 커밋 원장은 아래에서 확인할 수 있습니다."
    },
    "deltaProof": {
      "en": "Open the complete comparison record",
      "ko": "전체 비교 기록 열기"
    },
    "deltaProofBody": {
      "en": "The inventory snapshot through bb48fedc0 contains 509 engine commits from e9ced7e7a through adba86f43, plus eight website commits in the fork divergence. The current upstream check is 4209d6537 with 51 commits since the same baseline, mostly refactors and kernel/cache, SPIR-V and OIDN work. Later homepage polish is docs-only. The current homepage publishes four new technical comparison scenes; the retired thirteen-scene showcase remains historical evidence only. Area counts overlap because one commit can touch more than one area; fixes, reversions, experiments and tooling are included.",
      "ko": "bb48fedc0까지의 원장 스냅샷에는 e9ced7e7a부터 adba86f43까지 509개 엔진 커밋과 포크 분기 이후 홈페이지 커밋 8개가 포함됩니다. 같은 기준점에서 현재 업스트림 4209d6537까지는 51개 커밋이며, 주로 리팩터링과 커널·캐시·SPIR-V·OIDN 작업입니다. 이후 홈페이지 다듬기는 문서 커밋입니다. 현재 홈페이지에는 새 기술 비교 장면 4종을 공개하며, 폐기한 13종 스튜디오 세트는 역사적 근거로만 남깁니다. 하나의 커밋이 여러 분야에 걸칠 수 있어 분야 합계는 중복되며, 수정·되돌림·실험·도구 작업도 포함합니다."
    },
    "deltaAnalysis": {
      "en": "Read the scoped analysis",
      "ko": "범위가 적힌 분석 보기"
    },
    "deltaSummary": {
      "en": "Read the visual candidate summary",
      "ko": "시각 후보 요약 보기"
    },
    "deltaLedger": {
      "en": "Open the full commit ledger",
      "ko": "전체 커밋 원장 보기"
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
      "en": "03 / MATERIAL ATLAS",
      "ko": "03 / 재질 아틀라스"
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
      "en": "05 / THE DIFFERENCE",
      "ko": "05 / 차이를 직접 보세요"
    },
    "compareHeading": {
      "en": "New possibilities.\nVisible differences.",
      "ko": "추가된 가능성.\n눈에 보이는 차이."
    },
    "compareIntro": {
      "en": "Four new authored product and architecture studies, rendered side by side from the same geometry and light. Expand a pair to inspect the recipe and limitations.",
      "ko": "새로 만든 제품·건축 장면 4종을 동일한 형상과 조명으로 나란히 렌더했습니다. 각 쌍을 열어 레시피와 한계를 확인하세요."
    },
    "comparisonWallEyebrow": {
      "en": "SIDE BY SIDE / MATCHED SCENE",
      "ko": "SIDE BY SIDE / 동일 조건 비교"
    },
    "comparisonWallHeading": {
      "en": "The fork, frame by frame.",
      "ko": "같은 장면에서, 포크의 차이를."
    },
    "comparisonWallIntro": {
      "en": "Four new authored product and architecture studies, rendered side by side from the same geometry and light. Expand a pair to inspect the recipe and limitations.",
      "ko": "새로 만든 제품·건축 장면 4종을 동일한 형상과 조명으로 나란히 렌더했습니다. 각 쌍을 열어 레시피와 한계를 확인하세요."
    },
    "compareOpen": {
      "en": "Open matched comparison",
      "ko": "동일 조건 비교 열기"
    },
    "controlWallEyebrow": {
      "en": "FORK CONTROLS / SAME SCENE",
      "ko": "FORK CONTROLS / 같은 장면"
    },
    "controlWallHeading": {
      "en": "More material proof, clearly scoped.",
      "ko": "더 많은 재질 증거, 범위까지 명확하게."
    },
    "controlWallIntro": {
      "en": "These pairs isolate a SuperLuxCore control on the same authored scene. They are useful for material decisions and are intentionally labelled separately from the upstream binary comparisons above.",
      "ko": "이 페어는 같은 제작 장면에서 SuperLuxCore 제어 하나만 분리해 보여줍니다. 재질 판단에 유용하며, 위의 업스트림 바이너리 비교와 혼동하지 않도록 별도 범위로 표시했습니다."
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
      "en": "06 / THE VISUAL TOOLKIT",
      "ko": "06 / 모든 기능을 이미지로"
    },
    "toolkitHeading": {
      "en": "Renderer index / documented scope.",
      "ko": "렌더러 인덱스 · 지원 범위."
    },
    "toolkitIntro": {
      "en": "Start with one distinct native scene per discipline. Then inspect the material, pass, backend and documented scope.",
      "ko": "분야마다 다른 네이티브 장면을 먼저 확인하세요. 이어서 재질, 패스, 백엔드와 문서화된 범위를 살펴보세요."
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
      "en": "The four new comparison pairs are matched CPU technical studies exported with OIDN and a Linear+Gamma display proof. The receipt records the exact recipe; they are not presented as final ACES 2.0 beauty plates.",
      "ko": "새 비교 페어 4종은 OIDN과 Linear+Gamma 디스플레이 증거로 출력한 CPU 기술 스터디입니다. 정확한 레시피는 영수증에 기록했으며 최종 ACES 2.0 작품판으로 제시하지 않습니다."
    },
    "downloadEyebrow": {
      "en": "07 / YOUR NEXT IMAGE",
      "ko": "07 / 다음 작품을 시작하세요"
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
      "en": "Matched CPU technical evidence. Upstream-only features use the closest supported approximation and are labelled in each receipt; this wall is a material comparison, not a universal speed claim.",
      "ko": "CPU 매칭 기술 근거입니다. 업스트림에 없는 기능은 가장 가까운 지원 근사로 표시했으며, 이 벽은 보편적인 속도 주장이 아닌 재질 비교입니다."
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
      "id": "holo_packaging",
      "name": "AURORA / Serum packaging",
      "area": {
        "en": "Product / optical packaging",
        "ko": "제품 · 광학 패키징"
      },
      "description": {
        "en": "A sculpted serum bottle with a precision foil label, glass transmission and a metal cap under controlled studio light.",
        "ko": "정밀 포일 라벨과 유리 투과, 금속 캡을 통제된 스튜디오 조명으로 검증하는 세럼 패키지."
      },
      "tags": [
        "Glass transmission",
        "Diffraction foil",
        "Metal cap"
      ],
      "features": [
        "diffraction",
        "sellmeier"
      ],
      "image": "assets/studio/new-delta/holo_packaging-superlux-full.png",
      "group": "product",
      "renderNote": {
        "en": "New procedural study. Upstream is the closest supported approximation; SuperLuxCore applies the fork-only material override in the receipt.",
        "ko": "새 절차 메시 스터디입니다. 업스트림은 가장 가까운 지원 근사이며, SuperLuxCore는 영수증에 적힌 포크 전용 재질 오버라이드를 적용했습니다."
      }
    },
    {
      "id": "ev_candy_clearcoat",
      "name": "ION / Candy clearcoat",
      "area": {
        "en": "Automotive / clearcoat",
        "ko": "오토모티브 · 클리어코트"
      },
      "description": {
        "en": "A candy-painted EV study with a transparent cabin, conductor trim and a fork-only OpenPBR coat response.",
        "ko": "캔디 페인트 EV 스터디에 투명 캐빈, 도체 트림과 포크 전용 OpenPBR 코트 응답을 적용했습니다."
      },
      "tags": [
        "OpenPBR coat",
        "Clearcoat energy",
        "Conductor trim"
      ],
      "features": [
        "openpbr",
        "ggx",
        "multiscatter"
      ],
      "image": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png",
      "group": "product",
      "renderNote": {
        "en": "New procedural study. Upstream is the closest supported approximation; SuperLuxCore applies the fork-only material override in the receipt.",
        "ko": "새 절차 메시 스터디입니다. 업스트림은 가장 가까운 지원 근사이며, SuperLuxCore는 영수증에 적힌 포크 전용 재질 오버라이드를 적용했습니다."
      }
    },
    {
      "id": "knit_sneaker_lab",
      "name": "KINETIC / Knit sneaker",
      "area": {
        "en": "Product / footwear materials",
        "ko": "제품 · 풋웨어 소재"
      },
      "description": {
        "en": "A knit-forward footwear study separating the upper, sole, laces and badge so sheen and roughness are easy to read.",
        "ko": "어퍼·솔·끈·배지를 분리해 sheen과 거칠기 응답을 읽기 쉽게 만든 니트 풋웨어 스터디입니다."
      },
      "tags": [
        "OpenPBR sheen",
        "Knit upper",
        "Rough rubber"
      ],
      "features": [
        "hair",
        "sss"
      ],
      "image": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png",
      "group": "product",
      "renderNote": {
        "en": "New procedural study. Upstream is the closest supported approximation; SuperLuxCore applies the fork-only material override in the receipt.",
        "ko": "새 절차 메시 스터디입니다. 업스트림은 가장 가까운 지원 근사이며, SuperLuxCore는 영수증에 적힌 포크 전용 재질 오버라이드를 적용했습니다."
      }
    },
    {
      "id": "smart_glass_atrium",
      "name": "LUMEN / Smart-glass atrium",
      "area": {
        "en": "Architecture / glazing",
        "ko": "건축 · 스마트 글레이징"
      },
      "description": {
        "en": "An architectural glazing bay with layered panes, a metal frame, a planted interior and Sellmeier glass controls.",
        "ko": "다층 유리, 금속 프레임, 식재 인테리어와 Sellmeier 유리 제어를 함께 보여주는 글레이징 베이입니다."
      },
      "tags": [
        "Sellmeier glass",
        "Layered transmission",
        "Interior lighting"
      ],
      "features": [
        "volume",
        "thinfilm",
        "sellmeier"
      ],
      "image": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png",
      "group": "space",
      "renderNote": {
        "en": "New procedural study. Upstream is the closest supported approximation; SuperLuxCore applies the fork-only material override in the receipt.",
        "ko": "새 절차 메시 스터디입니다. 업스트림은 가장 가까운 지원 근사이며, SuperLuxCore는 영수증에 적힌 포크 전용 재질 오버라이드를 적용했습니다."
      }
    }
  ],
  "comparisons": [
    {
      "id": "holo_packaging",
      "project": "holo_packaging",
      "tab": {
        "en": "Packaging / diffraction",
        "ko": "패키징 · 회절"
      },
      "title": {
        "en": "Foil becomes a wavelength cue.",
        "ko": "포일이 파장 정보를 보여줍니다."
      },
      "body": {
        "en": "The same serum bottle is rendered once with upstream metal2 foil and once with SuperLuxCore diffraction. The label is the authored difference.",
        "ko": "같은 세럼 병을 업스트림 metal2 포일과 SuperLuxCore 회절 재질로 각각 렌더했습니다. 라벨 재질만 의도적으로 달랐습니다."
      },
      "before": "assets/studio/new-delta/holo_packaging-upstream-detail.png",
      "after": "assets/studio/new-delta/holo_packaging-superlux-detail.png",
      "left": {
        "en": "UPSTREAM / METAL2 APPROXIMATION",
        "ko": "업스트림 / METAL2 근사"
      },
      "right": {
        "en": "SUPERLUXCORE / DIFFRACTION",
        "ko": "SUPERLUXCORE / 회절"
      },
      "scope": {
        "en": "New procedural scene; LuxCoreRender 2.11.2 vs SuperLuxCore 2.11.27, PATHCPU, seed 42, matched camera/light/geometry and 64 target spp. The receipt labels unsupported fork materials as closest-supported upstream approximations.",
        "ko": "새 절차 장면입니다. LuxCoreRender 2.11.2와 SuperLuxCore 2.11.27을 PATHCPU, seed 42, 동일 카메라·조명·형상·목표 64 spp로 렌더했습니다. 포크 전용 재질은 영수증에 업스트림의 가장 가까운 지원 근사로 표시했습니다."
      },
      "fullBefore": "assets/studio/new-delta/holo_packaging-upstream-full.png",
      "fullAfter": "assets/studio/new-delta/holo_packaging-superlux-full.png"
    },
    {
      "id": "ev_candy_clearcoat",
      "project": "ev_candy_clearcoat",
      "tab": {
        "en": "Automotive / clearcoat",
        "ko": "오토모티브 · 클리어코트"
      },
      "title": {
        "en": "Clearcoat shapes the studio reflection.",
        "ko": "클리어코트가 스튜디오 반사를 만듭니다."
      },
      "body": {
        "en": "Identical EV geometry and lighting. Upstream uses glossy2; SuperLuxCore applies the OpenPBR coat override to make coat weight and roughness explicit.",
        "ko": "동일한 EV 형상과 조명입니다. 업스트림은 glossy2를, SuperLuxCore는 코트 무게와 거칠기를 명시하는 OpenPBR 코트 오버라이드를 사용합니다."
      },
      "before": "assets/studio/new-delta/ev_candy_clearcoat-upstream-detail.png",
      "after": "assets/studio/new-delta/ev_candy_clearcoat-superlux-detail.png",
      "left": {
        "en": "UPSTREAM / GLOSSY2 APPROXIMATION",
        "ko": "업스트림 / GLOSSY2 근사"
      },
      "right": {
        "en": "SUPERLUXCORE / OPENPBR COAT",
        "ko": "SUPERLUXCORE / OPENPBR COAT"
      },
      "scope": {
        "en": "New procedural scene; LuxCoreRender 2.11.2 vs SuperLuxCore 2.11.27, PATHCPU, seed 42, matched camera/light/geometry and 64 target spp. The receipt labels unsupported fork materials as closest-supported upstream approximations.",
        "ko": "새 절차 장면입니다. LuxCoreRender 2.11.2와 SuperLuxCore 2.11.27을 PATHCPU, seed 42, 동일 카메라·조명·형상·목표 64 spp로 렌더했습니다. 포크 전용 재질은 영수증에 업스트림의 가장 가까운 지원 근사로 표시했습니다."
      },
      "fullBefore": "assets/studio/new-delta/ev_candy_clearcoat-upstream-full.png",
      "fullAfter": "assets/studio/new-delta/ev_candy_clearcoat-superlux-full.png"
    },
    {
      "id": "knit_sneaker_lab",
      "project": "knit_sneaker_lab",
      "tab": {
        "en": "Footwear / sheen",
        "ko": "풋웨어 · sheen"
      },
      "title": {
        "en": "Sheen changes the knit read.",
        "ko": "Sheen이 니트의 인상을 바꿉니다."
      },
      "body": {
        "en": "The same footwear study uses a glossy2 upper upstream and the fork OpenPBR sheen controls in SuperLuxCore. Sole, lace and light stay fixed.",
        "ko": "같은 풋웨어 스터디에서 업스트림은 glossy2 어퍼를, SuperLuxCore는 OpenPBR sheen 제어를 사용합니다. 솔·끈·조명은 고정했습니다."
      },
      "before": "assets/studio/new-delta/knit_sneaker_lab-upstream-detail.png",
      "after": "assets/studio/new-delta/knit_sneaker_lab-superlux-detail.png",
      "left": {
        "en": "UPSTREAM / GLOSSY2 UPPER",
        "ko": "업스트림 / GLOSSY2 어퍼"
      },
      "right": {
        "en": "SUPERLUXCORE / OPENPBR SHEEN",
        "ko": "SUPERLUXCORE / OPENPBR SHEEN"
      },
      "scope": {
        "en": "New procedural scene; LuxCoreRender 2.11.2 vs SuperLuxCore 2.11.27, PATHCPU, seed 42, matched camera/light/geometry and 64 target spp. The receipt labels unsupported fork materials as closest-supported upstream approximations.",
        "ko": "새 절차 장면입니다. LuxCoreRender 2.11.2와 SuperLuxCore 2.11.27을 PATHCPU, seed 42, 동일 카메라·조명·형상·목표 64 spp로 렌더했습니다. 포크 전용 재질은 영수증에 업스트림의 가장 가까운 지원 근사로 표시했습니다."
      },
      "fullBefore": "assets/studio/new-delta/knit_sneaker_lab-upstream-full.png",
      "fullAfter": "assets/studio/new-delta/knit_sneaker_lab-superlux-full.png"
    },
    {
      "id": "smart_glass_atrium",
      "project": "smart_glass_atrium",
      "tab": {
        "en": "Architecture / glass",
        "ko": "건축 · 유리"
      },
      "title": {
        "en": "Refraction carries the room.",
        "ko": "굴절이 공간을 전달합니다."
      },
      "body": {
        "en": "The same glazing bay is compared with upstream Cauchy glass and the fork Sellmeier/thin-film override. The upstream side is the closest supported approximation, not feature parity.",
        "ko": "같은 글레이징 베이를 업스트림 Cauchy 유리와 포크 Sellmeier·박막 오버라이드로 비교합니다. 업스트림 쪽은 기능 동등성이 아닌 가장 가까운 지원 근사입니다."
      },
      "before": "assets/studio/new-delta/smart_glass_atrium-upstream-detail.png",
      "after": "assets/studio/new-delta/smart_glass_atrium-superlux-detail.png",
      "left": {
        "en": "UPSTREAM / CAUCHY GLASS",
        "ko": "업스트림 / CAUCHY 유리"
      },
      "right": {
        "en": "SUPERLUXCORE / SELLMEIER + FILM",
        "ko": "SUPERLUXCORE / SELLMEIER + FILM"
      },
      "scope": {
        "en": "New procedural scene; LuxCoreRender 2.11.2 vs SuperLuxCore 2.11.27, PATHCPU, seed 42, matched camera/light/geometry and 64 target spp. The receipt labels unsupported fork materials as closest-supported upstream approximations.",
        "ko": "새 절차 장면입니다. LuxCoreRender 2.11.2와 SuperLuxCore 2.11.27을 PATHCPU, seed 42, 동일 카메라·조명·형상·목표 64 spp로 렌더했습니다. 포크 전용 재질은 영수증에 업스트림의 가장 가까운 지원 근사로 표시했습니다."
      },
      "fullBefore": "assets/studio/new-delta/smart_glass_atrium-upstream-full.png",
      "fullAfter": "assets/studio/new-delta/smart_glass_atrium-superlux-full.png"
    }
  ],
  "controlComparisons": [],
  "hero": "holo_packaging"
};

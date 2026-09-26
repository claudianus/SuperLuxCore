# SuperLuxCore 렌더 가속 기능 — SOTA 프로덕션 렌더러 대비 감사 보고서

작성일: 2026-09-25 · 기준 트리: `feature/wavefront-queues` (HEAD `d4a5282f3bb85d8f1e95782f1ab8b14eb1e73e1f`)
비교 기준: Cycles 4.x/5.x, Arnold 7.x, V-Ray 7, RenderMan 27/XPU, Hyperion,
MoonRay, Redshift/Octane 및 2024–2026 공개 연구(SG/EG/HPGR) 수준.
**갱신(같은 날 후반, HEAD `cea50ae844aabf74fde19968c5678b70c622f2be`)**: §0/§2/§4/§5에 Cryptomatte·LPE·
light linking·P5 guiding·wavefront M3a·Vulkan M3 랜딩을 반영.

## 0. 요약 판정

**라이트 트랜스포트·샘플링 알고리즘 커버리지는 메인스트림 프로덕션
렌더러의 SOTA와 동등하며, 상당수 항목에서 초과한다.** 특히 GPU에서의
path guiding, ReSTIR DI/GI, vertex connection/merging, LMNEE/MGE,
잔차 추적 볼륨, 적응형 커스틱 파티션은 출시 중인 상용 렌더러 중
동등 기능을 제공하는 곳이 드물다(대부분 CPU 전용이거나 아예 없음).

같은 날의 후속 랜딩으로 프로덕션 인프라 갭의 상당 부분이 해소됐다:
**Cryptomatte(object/material+manifest)**, **LPE**(bounded NFA),
**light linking**(64비트 수신자 마스크), **적응형 robust clamping**,
**path.guiding.* 정식 프로퍼티화**, **wavefront M3a**(디바이스 prefix,
cornell 실측 dense 대비 ~2.5x).

**남은 뒤처짐**: deep EXR, shadow linking, OSL, USD/Hydra, full
wavefront 기본 활성화(추가 workload 검증 필요), Vulkan 네이티브
드라이버 검증·콜드 컴파일 시간(M3 본질은 완료: 21/21 커널 + 720p
렌더 PASS), 그리고 수만 씬 규모의 전투 검증.

---

## 1. 기능별 인벤토리 및 SOTA 대비 평가

### 1-A. 직접광 / many-light 샘플링

| 기능 | 구현 | CPU | GPU | SOTA 대비 |
|---|---|---|---|---|
| Light BVH (E&K'18) | `lightbvh.cpp`, `lightbvh_types.cl`, `lightbvh_funcs.cl` | ✅ | ✅ | **동등** — Cycles light tree, V-Ray adaptive lights와 동일 계열. e26에서 LOG_POWER 대비 동일 spp RMSE 1.5–1.8× 개선, CPU/GPU parity 0.01% |
| DLSC (direct light cache) | `dlscache.cpp` + impl | ✅ | ✅ | 업스트림 계승, 이후 감사 수정(첫 샘플 편향, samplesTaken 분모) |
| ELVC (env visibility cache) | `envlightvisibilitycache.h`, `elvc_funcs.cl` | ✅ | ✅ | env-map 가시성 캐시 — 상용에서 유사 기법 존재, 동등 |
| ReSTIR DI | `restirdi.cpp` + GPU 커널 | ✅ | ✅ | **초과** — RIS + visibility-weighted target + screen-space spatial + reconnection shift + GRIS clamp + temporal. 오프라인 프로덕션 중 ReSTIR DI 탑재 사실상 없음(RT 분야 기술). e30 실측 RMSE -37% |
| POWER/LOG_POWER/UNIFORM | `power.cpp`, `logpower.cpp`, `uniform.cpp` | ✅ | ✅ | 기본 분포, NaN/+Inf/음수 방어 추가됨 |

### 1-B. 간접광 / 경로 안내 (guiding)

| 기능 | 구현 | CPU | GPU | SOTA 대비 |
|---|---|---|---|---|
| Path guiding (SD-tree + vMF) | `pathguiding.cpp` (1191줄), `pathguiding.h` | ✅ | ✅ | **초과(GPU)/동등(CPU)** — OpenPGL 채택 렌더러(Cycles·V-Ray·Karma·Hyperion)는 CPU만. SuperLuxCore는 flattened SD-tree+vMF를 GPU 커널에서 평가. variance-aware target(Rath'20), flux-fraction split, pending radiance records, peak/count 기반 mixture weight까지 구현 |
| RIS product guiding (M4b) | `pathtracer.cpp` risZhat 경로 | ✅ | ⚠️ 부분 | **초과** — BSDF×L̂ product resampling은 최신 연구 수준, OpenPGL에도 없음 |
| Portal guiding (M5) | `pathtracer.cpp` portal 경로, BLC 라이트 포털 오브젝트 | ✅ | ✅ | 동등+ — GPU 포털 랜딩(`c92c2f3407e9eb55fa6e4c649a3c49b409fa867a`, MK_HIT_OBJECT 게이트+`portal_*` 커널, portal_slit 4/4 PASS). adaptive share는 field 기반으로 단순 portal보다 진보 |
| ReSTIR GI (G1/G2) | `restirgi.cpp` + `MK_RT_GI_*` 커널 | ✅ | ✅ | **초과** — first-bounce reservoir + spatial reuse. 오프라인 렌더러에 ReSTIR GI는 없음 |

### 1-C. 커스틱스 / SDS 경로

| 기능 | 구현 | CPU | GPU | SOTA 대비 |
|---|---|---|---|---|
| MNEE (Hanika'15) | `pathtracer_mnee.cpp` (1558줄), `MK_MNEE_NEXT_VERTEX` | ✅ | ✅ | **동등~초과** — Cycles도 MNEE 보유. 여기에 seed cache(cold-first+rescue), directional/distant 엔드포인트, hero-λ 분산 IOR까지 확장 |
| LMNEE (light-side) | GPU light-vertex 커널 | ❌ | ✅ | **초과** — Cycles는 eye-side만. light→camera manifold connect는 희귀 |
| MGE (manifold guided emission) | light tracing 발사 경로 | ❌ | ✅ | **초과/독자** — 커스틱 타깃 유도 방출 |
| Caustic focus cache | `lightFocusBuff` 링 버퍼 | ❌ | ✅ | **독자** — productive target 학습 |
| 적응형 커스틱 파티션 | `IsAdaptiveCausticPath`, eye/light 양측 classifier | ✅ | ✅ | **초과** — 고정 glossiness threshold를 solid-angle 기반 판정으로 대체. 상용의 caustic solver 방향과 같은 문제의식, unbiased 유지 |
| GPU light tracing | `MK_LIGHT_INIT`/`MK_LIGHT_VERTEX` 스테이트 머신 | ❌ | ✅ | **초과** — GPU에서 light subpath+splat은 드묾 |
| PhotonGI caustic cache | `photongicache.cpp` + tracephotonsthread | ✅ | ❌ | 동등(CPU) |
| Vertex merging (VCM, M7) | `VCBuildMergeHash`, merge radius, SmallVCM MIS | ❌ | ✅ | **초과** — GPU VCM merge. CPU 레퍼런스는 BIDIRVMCPU가 담당 |
| Vertex connection (M6) | `MK_VC_CONNECT`, VCReplay(M7d) | ❌ | ✅ | **초과** — GPU BDPT connect + temporal replay reservoir |

### 1-D. 볼륨

| 기능 | 구현 | CPU | GPU | SOTA 대비 |
|---|---|---|---|---|
| Delta tracking (null-collision) | `heterogenous.cpp`, `volume_funcs.cl` | ✅ | ✅ | 동등 |
| Ratio tracking | ✅ | ✅ | ✅ | 동등 |
| Residual ratio/delta tracking | ✅ | ✅ | ✅ | **초과** — 2024 연구급, majorant grid 조합 |
| Majorant grid | ✅ | ✅ | ✅ | 동등~초과 |
| Equiangular + transmittance MIS 거리 샘플링 | ✅ | ✅ | ✅ | **초과** — equiangular MIS 조합은 최신 |
| HG phase | ✅ | ✅ | ✅ | 동등 |
| 볼륨 bounce path guiding | ✅ | ✅ | ✅ | **초과** — vMF 필드가 볼륨 정점도 안내 |

### 1-E. 샘플러 / 픽셀 적응

| 기능 | 구현 | SOTA 대비 |
|---|---|---|
| Sobol (Owen scramble) | `sobol.cpp`, `sobolsequence.cpp` | 동등 — Arnold/PBRT-v4급 |
| PMJ02 | `pmj02.cpp`, `pmj02/` | 동등 — 최신 blue-noise 계열 |
| Blue-noise rank tiles | ✅ | 동등 |
| 2차 모멘트 적응 샘플링 | `sampler.*.adaptive.strength`, CONVERGENCE AOV | 동등 — Arnold adaptive와 같은 문제의식 |
| Metropolis sampler | `metropolis.cpp` | 존재(레거시 MLT 경로) |
| Radiance clamping (고정+variance sqrt) | `path.clamping.*` | 동등 — VarianceClamp는 일부 상용에만 존재 |

### 1-F. 레이 트래버설 / 커널 아키텍처

| 기능 | 구현 | SOTA 대비 |
|---|---|---|
| Embree CPU | `embreeaccel.cpp` + cluster-user-geometry | 동등 |
| BVH/MBVH 소프트웨어 | `bvhaccel*`, `mbvhaccel*` | 동등 |
| Metal HWRT (native AS) | `metalrtaccel.mm`, `metalintersectiondevice.mm` | **동등** — MTLAccelerationStructure 삼각형+네이티브 커브+모션블러 AS+인스턴스 리핏. Cycles MetalRT와 동일 계열 |
| 커브 HW 프리미티브 | `strands.cpp` + curve AS | 동등(MetalRT curve) |
| Micro-kernel 스테이트 머신 | 16개 `MK_*` 커널 | 기반 완료 |
| Wavefront 큐 (M1/M2/M3a) | `BuildQueues`, `QueuePrefix`, λ-segment | **↘M3a 랜딩**(`37cc04b4baa4b492334e8391a2fd2b0a4534e815`) — 디바이스 prefix, cornell ~2.5x. 잔여: M3b-e 옵션+기본 활성화 판단. Hyperion·XPU·Cycles GPU는 full wavefront |
| Vulkan 백엔드 | `vkdevice.cpp`, clspv 파이프라인, HWRT | 실험 단계 → **M3 본질 완료**(`ef7a4ca6054e3ab2403e5261865748c866570f44` BLAS/TLAS+ray_query, `bfc37876b0ce54827d49746a13cb94107860a3b8` 캐시, 21/21 커널+720p 렌더 PASS) — 네이티브 드라이버/콜드컴파일 잔여 |

### 1-G. 디노이즈 / 후처리

| 기능 | 구현 | SOTA 대비 |
|---|---|---|
| OIDN 2.x | `intel_oidn.cpp`, **Metal device 포함** | 동등+ — Metal OIDN은 자체 패치, ~17× vs CPU |
| OIDN component 모드 | ✅ | 동등 — DIRECT/INDIRECT/EMISSION 분해 입력 |
| OptiX denoiser | `optixdenoiser.cpp` | 동등 (NVIDIA 환경) |
| BCD | `bcddenoiser.cpp` | 동등 (레거시) |
| Temporal Accumulate (D1) | `temporalaccumulate.cpp` | **동등~초과** — MV+depth/normal/OID disocclusion+history clipping+전 radiance component 누적+EXR state. OIDN 3 temporal 선행 개념을 자체 구현 |
| VARIANCE / MOTION_VECTOR AOV | film channel | 동등 — 디노이저 입력급 AOV |

### 1-H. 스펙트럴

| 기능 | 구현 | SOTA 대비 |
|---|---|---|
| hero-λ 3빈 스펙트럴 | `Spectral::`, path.spectral.enable | **독보적(상용 대비)** — 대부분 RGB-only. Manuka/Ocean급 풀 스펙트럴엔 미달이나 상용엔 없음 |
| Cauchy 분산 glass | ✅ | 초과 |
| JH2019 rgb2spec 업샘플 | ✅ opt-in | 동등 — Jakob-Hanika'19 표준 기법 |
| 스펙트럴 볼륨/형광/4빈 | ❌ | 미구현 |

### 1-I. 메모리 / 스케일

| 기능 | 구현 | SOTA 대비 |
|---|---|---|
| scene.spill.* (지오메트리/이미지맵/스테이징/BVH 노드) | `memspill.h`, `SpillableArray` | **동등~초과** — demand-paged out-of-core, V-Ray/Arnold급 문제의식 |
| .lxm 프록시 (v1–v4) | `exttrianglemeshfile.cpp` | **동등** — V-Ray `.vrmesh` 대응물. v4 per-cluster vertex range = >VRAM DMA payload 준비 |
| 클러스터 레지던시 (Embree user-geom) | boundsFunc/intersectFunc | **초과(설계)** — ray-driven 페이지 폴트, 클러스터 단위 |
| 이미지맵 스트리밍 디코드 | lazy ImageBuf+resize | 동등 — 8K PNG 195MB→3MB |
| ClusterResidencyPool (>VRAM) | `geomstream.h` 스켈레톤 | 미구현 — unified memory라 미검증 |

### 1-J. 뷰포트 / 인터랙션 가속

| 기능 | 구현 |
|---|---|
| 비동기 세션 워커 | `session_worker.py` |
| 런타임 해상도 축소 (dyn-res 16→복원) | `SetRuntimeResolutionReduction` — apply→sample ~55→26ms |
| 편집 페이싱 / hold-last-frame / GIL 해제 | ✅ |
| RTPATHOCL 프리뷰 | ✅ |

---

## 2. 프로덕션 렌더러 대비 매트릭스

| 항목 | SuperLuxCore | Cycles | Arnold | V-Ray | RenderMan | Hyperion | Redshift/Octane |
|---|---|---|---|---|---|---|---|
| Light tree/BVH | ✅ CPU+GPU | ✅ | ✅(light sets) | ✅ | ✅ | ✅ | ✅ |
| Path guiding | ✅ **CPU+GPU** | CPU만 | ❌ | CPU | ⚠️일부 | ✅(OpenPGL v2) | ❌ |
| Product/RIS guiding | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| ReSTIR DI | ✅ CPU+GPU | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| ReSTIR GI | ✅ CPU+GPU | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| MNEE | ✅ CPU+GPU | ✅ | ⚠️caustic solver | ✅ | ⚠️ | ⚠️ | ⚠️ |
| LMNEE/MGE | ✅ GPU | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| VCM/vertex merging | ✅ GPU (+CPU ref) | ❌ | ❌ | ⚠️photon | ❌ | ❌ | ❌ |
| Light tracing GPU | ✅ | ❌ | ❌ | ⚠️ | ❌ | ⚠️ | ⚠️ |
| 적응 커스틱 파티션 | ✅ | ⚠️고정 | ⚠️ | ✅solver | ⚠️ | ⚠️ | ⚠️ |
| 잔차 비율 추적 볼륨 | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| 스펙트럴 | ✅3빈 hero-λ | ❌ | ❌ | ⚠️일부 | ⚠️일부 | ❌ | ⚠️Octane |
| HWRT | ✅Metal | ✅OptiX/HIPRT/MetalRT | ✅OptiX | ✅ | ✅ | ✅ | ✅ |
| Full wavefront | ⚠️M3a opt-in (~2.5x cornell) | ✅ | ✅GPU | ✅ | ✅XPU | ✅ | ✅ |
| OIDN | ✅+Metal | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Temporal denoise | ✅자체 D1 | ⚠️ | ⚠️ | ✅ | ⚠️ | ✅ | ✅ |
| Cryptomatte | ✅(object/material) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| LPE | ✅(PATH 계열) | ⚠️ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Deep EXR | ❌ | ⚠️ | ✅ | ⚠️ | ✅ | ✅ | ❌ |
| Light linking | ✅(64비트 수신자 마스크) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| OSL | ❌ | ✅ | ✅ | ❌ | ✅Rix | ❌ | ⚠️ |
| USD/Hydra | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| 분산 렌더 | ❌ | ⚠️ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Out-of-core | ✅spill+.lxm | ⚠️제한 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 메시 프록시 | ✅.lxm | ⚠️ | ✅ass | ✅vrmesh | ✅ | ✅ | ✅ |

(✅=출시 기능, ⚠️=부분/제한, ❌=부재 — 공개 문서 기준 근사치)

## 3. SOTA 대비 초과 항목 (차별점)

1. **GPU path guiding** — Cycles·V-Ray·Karma·Hyperion 모두 CPU 전용.
   flattened SD-tree + vMF를 디바이스 커널에서 평가하는 구조는 공개
   프로덕션 렌더러 중 유일급.
2. **ReSTIR DI/GI 오프라인** — 실시간 분야 기술을 오프라인 추정기로
   승격, GRIS-정합 merge + visibility target까지.
3. **GPU VCM 계열** — vertex connection/merging/replay를 GPU 태스크
   모델에 직결. BDPT/VCM은 통상 CPU 전용.
4. **LMNEE + MGE + caustic focus** — light-side manifold + 유도 방출은
   Cycles MNEE의 범위를 넘는다.
5. **적응형 커스틱 파티션** — 고정 임계값 대신 solid-angle 기반
   unbiased 판정.
6. **볼륨 잔차 추적** — residual ratio/delta + majorant grid +
   equiangular-MIS 조합은 2024급 연구 사양.
7. **hero-λ 스펙트럴(GPU 포함)** — 상용 RGB 렌더러 대비 독보적.
8. **Metal 네이티브 스택 전체** — HWRT + OIDN Metal + curve
   primitive + command-buffer 수명 수정까지, Apple 생태계 깊이.

## 4. SOTA 대비 부족 항목 (갭, 우선순위순)

| 우선 | 갭 | 영향 | 난이도 |
|---|---|---|---|
| 1 | ~~**Full wavefront M3**~~ → **M3a 랜딩**(`37cc04b4baa4b492334e8391a2fd2b0a4534e815`). 잔여: 추가 workload 재측정 → 기본 활성화 판단, M3b-e 옵션 | GPU 대형씬 스루풋 | 중 |
| 2 | ~~**Cryptomatte**~~ → ✅ 랜딩(`aabfdb3e5fbe98e75617309a30920ca27f0b8f7a`). 잔여: asset-level | 합성 파이프라인 필수 AOV | 저 |
| 3 | ~~**Light linking**~~ → ✅ 랜딩(`8d05a4ef1793ca9ccc98549dae78af2156bbe078`/`8d05a4ef1793ca9ccc98549dae78af2156bbe078`). 잔여: shadow linking | 아티스트 제어 | 중 |
| 4 | ~~**LPE**~~ → ✅ 랜딩(`7a1622a86dfefb044b17c7d4f3f332bee69b668f`). 잔여: BIDIR/LT 범위, 표현력 확장 | 합성 유연성 | 중~고 |
| 5 | ~~**Path guiding CPU 성숙도**~~ → ✅ P5 랜딩(`00804937b0f880ff2421a497591a27a6e2a531ba`): `path.guiding.*` 정식화+BIC-K+계층 폴백+.bcf 영속. 잔여: OpenPGL 트레이너 비교는 선택 사항 | 간접광 수렴 | 중 |
| 6 | **Deep EXR** | VFX 합성 | 중 |
| 7 | **스펙트럴 심화** — 4빈, n/k DB, λ범위 확장, 형광 | 스펙트럴 충실도 (P0-3) | 중 |
| 8 | **USD/Hydra delegate** | 파이프라인 채택 | 고 |
| 9 | **OSL** | 셰이딩 표현력 | 고(선택) |
| 10 | **분산 렌더/렌더팜** | 대형 프로덕션 | 고 |
| 11 | **ReSTIR PT/PG** (Lin'22, Wyman'25) — 연구급, 상용 선행 없음 | 연구 우위 유지 | 고 |
| 12 | **PhotonGI GPU화** | CPU-only 캐시의 병목 | 중 |
| 13 | **>VRAM 스트리밍** (ClusterResidencyPool 완성) | 디스크리트 VRAM 플랫폼 | 고 |
| 14 | **Vulkan M4 완결** — M3 랜딩(전 커널 디스패치+720p 렌더 PASS), 네이티브 드라이버 검증·콜드 컴파일 개선 잔여 | 멀티벤더 | 중 |

## 5. 알고리즘 완성도 관련 잔여 리스크

- **Wavefront M3a 랜딩됨, 다만 추가 workload 검증 잔여** — 디바이스
  prefix로 host sync 2→1, cornell 실측 ~2.5x 역전. totals readback은
  필수(stale sizing ~6x 회귀). 추가 씬(classroom/luxball/spectral/
  대형씬)에서 재측정 전까지 dense 기본 유지.
- **Path guiding 신뢰도 — P5로 대부분 해소** — `path.guiding.*` 정식
  프로퍼티가 CPU/GPU 공통 `SettingsFromProperties`를 통해 단일 경로화,
  LUX_PG_*는 디버그 폴백. 잔여: 장기 수렴·필드 동기 드리프트 회귀.
- **ReSTIR 장기 수렴** — bounded-bias clamp(64×) 의존 구간 존재.
  unbiased 보장 경계를 문서화해야 한다.
- **Vertex merging bias** — VCM merge는 본질적으로 consistent-biased
  (progressive shrinkage 미구현). BIDIRVMCPU 레퍼런스와의
  장기 수렴 게이트가 필요.
- ~~**포털 가이딩 GPU 부재**~~ — `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a`로 GPU 포털 랜딩
  (portal_slit 4/4 PASS, Metal+OpenCL). 해소됨.
- **Metropolis 경로** — `metropolis.cpp` 샘플러는 존재하나 MLT
  통합 엔진으로의 승격 상태는 미검증.
- **CUDA/OptiX 디노이저·가속** — macOS 개발 환경에서 미검증 경로.

## 6. 권장 방향

1. **엔진 알고리즘 트랙은 "초과" 구간을 유지·정착시키는 데 집중.**
   ReSTIR/가이딩/VCM의 장기 수렴·바이어스 게이트를 회귀로 고정하고,
   실험 env 플래그를 정식 프로퍼티로 승격.
2. **Wavefront M3a가 스루풋 레버로 랜딩됨** — 다음 단계는 워크로드별
   재측정 매트릭스로 기본 활성화/자동 선택 판단, M3b-e는 측정 후 옵션.
3. **프로덕션 인프라 갭은 상당수 해소** — Cryptomatte·light linking·LPE
   랜딩. 잔여 우선순위: deep EXR → shadow linking → crypto asset level →
   USD/Hydra. 이 영역이 "연구 프로토타입"과 "프로덕션 렌더러"의 실질 경계다.
4. **스펙트럴은 3빈→4빈+n/k DB가 다음 단계.** 이미 상용 대비 유일한
   차별점이므로 깊이를 더하는 가치가 크다.
5. **OpenPGL은 CPU 트레이너 교체 후보로만 검토.** GPU 계약(flattened
   tree+vMF)은 유지하면서 학습 품질만 비교.

---

*근거: 본 감사는 소스 인벤토리(src/slg, include/slg, dev-tools/e9–e38,
doc/features)와 공개 문서(Cycles 릴리스노트, OpenPGL ASWF 이관,
ReSTIR PG'25, MNEE/SMS 계보)에 기반한다. 상용 렌더러 열의 ⚠️/❌ 판정은
공개 문서 수준의 근사치로, 실사용 검증 시 세부 차이가 있을 수 있다.*

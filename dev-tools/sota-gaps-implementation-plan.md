# SOTA 갭 1~5 — 구현·개선·통합 계획서

작성일: 2026-09 · 기준 트리: `feature/wavefront-queues`
선행 문서: `dev-tools/sota-acceleration-audit.md` (갭 선정 근거)

대상 갭 (감사 보고서 우선순위 순):

1. **Wavefront M3** — compaction+정렬+기본 활성화 (GPU 대형씬 스루풋)
2. **Cryptomatte** — 모든 프로덕션 보유, 미구현
3. **Light linking** — 라이트↔오브젝트 조명 관계 필터
4. **LPE** (light path expression) — 확장 AOV 분해
5. **Path guiding 성숙도** — OpenPGL 검증 아이디어 선별 이식

공통 원칙: CPU/GPU 기능 parity, unbiased 보존, 어댑터 즉시 노출,
720p+ 시각 회귀 검증, feature-chunk 단위 리뷰 가능성.

---

## 0. 갭별 현재 상태 요약

| 갭 | 현재 상태 | 결정적 사실 |
|---|---|---|
| Wavefront | M1(큐 인덱싱)+M2(λ버켓) opt-in 구현 완료, `LUXRAYS_WAVEFRONT_QUEUES=1` | **전 워크로드에서 dense 대비 −7~−17% 패배** (cornell −17%, classroom −7%, luxball −8%, spectral −15%). `wavefront-design.md` M2 status, `roadmap.md:33` |
| Cryptomatte | `OBJECT_ID`/`MATERIAL_ID` 단일 ID 채널만 존재 | FNV-1a 24bit 안정 objectID 이미 확보 (`parseobjects.cpp:NameToObjectID`) — 매니페스트 기반 멀티 ID 없음 |
| Light linking | 없음. 라이트그룹은 **출력 그루핑**(radiance group)용으로만 존재 | GPU `BSDF::sceneObjectIndex`, CPU `BSDF::GetSceneObject()` 이미 존재 — 수신 오브젝트 식별 인프라 있음 |
| LPE | 없음. `EyePathInfo`에 `lastBSDFEvent` 단일 이벤트만 | 이벤트 원시재료(`BSDFEvent`, depth 카운터)는 있으나 시퀀스 상태 없음 |
| Path guiding | SD-tree+vMF 자체 구현, GPU 계약 확정, RIS product guiding | `LUX_PG_*` env 플래그 10여 개 실험 경로로 잔존, cold-leaf 하드 게이트, K=4 고정 EM |

---

## 1. Wavefront M3

### 1.1 문제 정의 (회귀 원인 분석)

현재 웨이브프론트 반복 구조 (`pathoclbaseoclthreadkernels.cpp:914-1067`):

```
EnqueueWriteBuffer(count=0)  → BucketHistogram kernel
EnqueueReadBuffer(counts, CL_TRUE)   ← per-iteration blocking host sync
host prefix (36 counters)
EnqueueWriteBuffer(bases, CL_TRUE)   ← 두 번째 동기화
BuildQueues kernel
17개 state kernel launch (count>0인 것만)
```

dense 경로는 같은 반복에 11개 커널을 `taskCount` 전체에 올리고 각 커널이
`tasksState[gid].state` 체크로 early-out한다. 측정된 −7~−17%의 비용 구성:

1. **호스트 동기화** — `EnqueueReadBuffer(CL_TRUE)`가 큐 전체를 드레인.
   반복당 수 ms급 파이프라인 스톨.
2. **추가 패스 2개** — histogram + build는 순수 오버헤드 (dense엔 없음).
3. **런치 수** — 17개 vs dense 11개 (빈 상태는 스킵되지만 런치 오버헤드 잔존).
4. **1 state-hop/iteration** — 태스크가 반복당 최대 1 상태만 진행 → 동일 샘플
   도달에 더 많은 반복 = 더 많은 런치/패스. coherence 이득이 이 비용을 못 덮음.

M3는 "재질 버켓 추가"가 아니라 **위 4개 비용을 제거해 dense를 이기는
스케줄러 재설계**로 정의한다. 재질 버켓은 그 위의 옵션 실험.

### 1.2 M3a: 호스트 동기화 제거 (최우선, 단독 가치 최대)

핵심 관찰: `WAVEFRONT_GUARD` (`pathoclbase_funcs.cl:6521`)가 이미
`gid >= count` early-out을 한다. **런치 크기는 상한일 뿐 정확할 필요가 없다.**

설계: 고정 크기 런치 + 디바이스 사이드 prefix.

- `AdvancePaths_BucketHistogram`: 유지 (taskCount 전체 스캔, atomic 카운터).
- 신규 `AdvancePaths_QueuePrefix`: 1개 워크그룹 커널. 36개 카운터의
  exclusive prefix를 디바이스에서 계산 → `taskQueueBase` 직접 기록.
  호스트 왕복 제거.
- `AdvancePaths_BuildQueues`: 유지 (`taskQueueBase` atomic cursor append).
- **런치 크기 = `taskCount` 고정** (또는 이전 반복 카운트의 stale 값 — 아래).
  각 커널 첫 명령이 `queueCount` 읽기 → 빈 상태는 워크그룹당 1회 브로드캐스트
  읽기 후 종료. 호스트는 카운트를 **읽지 않는다** → blocking sync 0.

잔여 비용: histogram+build 2패스 + 커널 수(17). 빈 상태 런치는 가드 종료로
수 µs → dense의 state-check 비용과 동급.

**Stale-count 런치 사이징** (선택 최적화): 카운트를 *비동기* readback해
**다음 반복의** 런치 크기로 사용. 큐는 매 반복 재구축되므로 stale 크기로
부족하게 띄운 태스크는 다음 반복 큐에 다시 들어감 → 정확성 무손실, 지연만.
마진 `min(taskCount, prevCount*1.2+1024)`로 언더런치 억제.

### 1.3 M3b: append-at-transition (단일 패스, 빌드 패스 제거) — 평가 후 결정

히스토그램+빌드 자체를 없애는 진짜 웨이브프론트: 각 MK 커널이 상태 전이
시점에 목적지 큐에 직접 atomic append.

- `per-(state,λ)` atomic cursor 배열 (18×3=54 u32) — `taskState->state = S`
  대신 `AppendToQueue(S, lambdaBin)` 매크로가 `slot = atomic_add(&cur[S][λ],1);
  queue[S][base[S][λ]+slot] = gid` 수행.
- λ 버킷 유지: (state,λ)별 커서 → λ-컨티규어스는 아니지만 λ-그룹화(atomic
  도착 순 내 λ 일관)는 유지됨. 워프 단위 coherence는 다소 약화 — 측정 필요.
- 초기 큐(반복 0)는 init 커널이 구성.
- 이점: 패스 2개 제거, 전이 지점 instrumentation 비용만 (~1 atomic/태스크).
- 리스크: 전이 지점 ~30곳 수정, 큐 슬롯 소비량 상한 관리(태스크는 반복당 여러
  큐에 착륙 가능 — 큐 크기를 `taskCount`로 유지하려면 반복 경계에 리셋 필요:
  모든 상태 커널이 끝난 시점에 소비되지 않은 항목 정리 → 1개 소형 리셋 커널
  또는 다음 반복 init에서 카운터 리셋).

### 1.4 M3c: 재질/이벤트 버켓 (측정 후 적용)

히스토그램 키를 `(state, λ)` → `(state, λ, matBucket)`로 확장.
`matBucket = bsdf.materialIndex & 7` — BSDF가 살아있는 상태
(MK_HIT_OBJECT, MK_DL_*, MK_GENERATE_NEXT_VERTEX_RAY)에서만 유효,
나머지는 bucket 0.

- 카운터 배열: `WAVEFRONT_NUM_STATES × λ × MAT_BUCKETS` (18×3×8=432 u32).
- BuildQueues/append 시 `bsdfs[gid].materialIndex` 읽기 — M3b의 append
  경로에선 전이 전 BSDF가 이미 hot register라 무료.
- 대안 키: materialIndex 대신 **BSDFEvent 클래스**(diffuse/glossy/specular
  /transmit 4버켓) — 텍스처 패치보다 명령 스트림 coherence가 목적이면 이쪽이
  더 저렴하고 효과적일 수 있음. 둘 다 env 플래그로 A/B.

### 1.5 M3d: 인덱스드 RT 디스패치

`EnqueueTraceRayBuffer`는 여전히 `raySlotCount` 전체를 trace —
광선을 낸 태스크만 처리하는게 웨이브프론트 취지.

- `MK_GENERATE_*`/MK_DL_* 가 rays[]에 기록할 때 `rayQueue[]`에 gid를
  atomic append (이미 `rayQueue` 버퍼 존재).
- 신규 `EnqueueTraceRayBufferIndexed(rayBuff, hitBuff, indexBuff, count)`
  — Metal: RT 커널이 index buffer 경유 indirection; OCL/네이티브 동일.
  count는 stale 또는 taskCount 상한 (hits는 마스크로 처리되므로 stale-safe).
- 효과: 스파스 상태에서 RT 워크 절감 — 대형씬에서 의미 큼.

### 1.6 M3e: 소형 상태 커널 융합

`MK_SPLAT_SAMPLE → MK_NEXT_SAMPLE → MK_GENERATE_CAMERA_RAY` 체인은
거의 모든 태스크가 매 반복 통과하는 고정 파이프라인. 웨이브프론트에서도
3개 큐를 거쳐 3 iteration 필요 → **단일 융합 커널**로 묶어 태스크가 한
런치에 체인 전체를 통과하게 함 (dense 의미론과 동일). 런치 수 17→15,
레이턴시 -2 hops.

### 1.7 통합 포인트

| 파일 | 변경 |
|---|---|
| `pathoclbaseoclthreadkernels.cpp` | `EnqueueAdvancePathsWavefront` 재작성 (동기 제거, 고정/stale 런치) |
| `pathoclbase_kernels_micro.cl` | 전이 지점 append 매크로 (M3b), rayQueue append (M3d), 융합 커널 (M3e) |
| `pathoclbase_datatypes.cl` | per-(state,λ[,mat]) 커서/베이스 배열 확장 |
| `intersectiondevice.h` + Metal/OCL 구현 | `EnqueueTraceRayBufferIndexed` 추가 |
| `pathoclbaseoclthreadinit.cpp` | 큐 버퍼 재구성, stale 카운트 staging 버퍼 |
| `pathocl/pathoclopenclthread.cpp:206` | 인덱스드 RT 호출 |

### 1.8 게이트 (실패 시 롤백 기준)

- 워크로드 매트릭스: cornell(저발산), classroom(다재질), luxball(멀티로브),
  spectral(λ), + **대형씬 신규 벤치**(수백만 tri + 수십 라이트 — 웨이브프론트가
  설계상 이겨야 할 워크로드).
- 승자 기준: dense 대비 wall-clock spp/s ≥ +3%인 워크로드가 1개 이상,
  나머지는 −2% 이내. 달성 시 `renderengine` 프로퍼티로 opt-in 유지,
  워크로드 휴리스틱(태스크 다이버전스 지표)으로 자동 선택 검토.
- 미달 시: 큐 인프라는 Cryptomatte/LPE 등 다른 용도(아래 §6)로 재활용 가능 —
  작업이 무의미해지지 않음. wavefront는 opt-in 유지 문서화.
- 회귀 테스트: `LUXRAYS_WAVEFRONT_DEBUG` 카운터(oob/dup/badState/badLambda)
  자동 assert, dense-vs-wavefront 128spp 동일 통계 검증 (기존 검증 절차 유지).

---

## 2. Cryptomatte

### 2.1 표준 요구사항

- 픽셀당 정렬된 **(id, coverage) 페어 목록** — 표준 6페어, 채널명
  `cryptomatte00`(id0,cov0,id1,cov1), `cryptomatte01`, `cryptomatte02` (각 RGBA).
- id = **murmur3-32(이름) → float32** (exponent/mantissa 리팩, 스펙의
  `hash_to_float`). coverage = 해당 id가 픽셀을 커버한 샘플 비율.
- EXR 메타데이터: `cryptomatte/<md5>/manifest` (JSON `{id_hex: "name"}`),
  `cryptomatte/<md5>/name`, `cryptomatte/<md5>/version` + hash 알고리즘 명시.
- 레이어: `CryptoObject`(오브젝트명), `CryptoMaterial`(머티리얼명),
  `CryptoAsset`(메쉬/에셋명 — v2).

### 2.2 ID·매니페스트 설계

- `include/luxrays/utils/murmurhash.h` 신규 (~50줄, 표준 구현).
  `CryptoID(name) = murmur3_32(name, seed=0) → float`.
- CPU `SceneObject`/`Material`에 `u_int cryptoID` — 씬 파싱 시 이름으로
  계산, `ocl::SceneObject`/`ocl::Material`에도 동일 필드 추가
  (`sceneobject_types.cl`, `material_types.cl` — tail 필드로 ABI 확장).
- 매니페스트: `Scene::GetCryptomatteManifest(kind)` → JSON 문자열 생성
  (이름↔해시 맵). Film은 씬을 모르므로 엔진이 필름에 주입:
  `Film::SetMetadata(key, json)` 범용 메타데이터 백 → `filmoutput.cpp`의
  `ImageSpec`에 `spec.attribute(key, val)` 전달.
- 기존 `objectID`(24bit FNV)는 **유지** — 기존 `OBJECT_ID` 계열 채널과의
  하위호환. cryptoID는 별도.

### 2.3 런타임 데이터 플로우

- `SampleResult`에 `cryptoObjID`, `cryptoMatID` (u32) 추가 —
  `materialID`/`objectID` 기록 지점 그대로 미러링:
  CPU `pathtracer.cpp:997` 부근, GPU `pathoclbase_kernels_micro.cl:304` 부근.
  miss 시 0 — 이미 그 패턴.
- 커버리지 시맨틱 v1: **샘플당 최대 1 id** (첫 비-패스스루 표면). stochastic
  transparency 구조상 자연스럽게 coverage=샘플 비율. v2: 패스스루 체인의
  복수 id 누적 (투명 레이어 커버리지).
- 필름 채널: `CRYPTOMATTE_OBJECT`, `CRYPTOMATTE_MATERIAL` —
  `GenericFrameBuffer<12,1,float>`가 아니라 **커스텀 머지** 필요:
  `CryptoFrameBuffer` = 픽셀당 6 슬롯 `{id:u32, cov:f32}` + weight 채널.
  `AddPixel(id, w)`: id 매치 → `cov += w`; 빈 슬롯 또는 최소 cov 슬롯보다
  크면 삽입/교체. CPU: `filmaddsample.cpp` 커스텀 경로.
  GPU: `filmCryptoObjId[px*6]`, `filmCryptoObjCov[px*6]` —
  `atomic_cmpxchg(&id, 0, newId)`로 슬롯 claim → `atomic_add(&cov, w)`.
  매치 실패 시 다음 슬롯 스캔 (최대 6). 잠금 없이 converge.
- 출력 정규화: coverage는 누적 weight — 출력 시 픽셀별 총 weight로 나눠
  [0,1] 커버리지로 (weight 채널 활용, `GenericFrameBuffer<13,1,float>`의
  13번째 또는 별도 카운트).

### 2.4 EXR 출력

- `filmoutput.cpp`에 `CRYPTOMATTE_OBJECT/MATERIAL` 출력 타입:
  단일 EXR에 `cryptomatte00`,`cryptomatte01`,`cryptomatte02` 명명 채널로
  12 float 배치 (채널명은 `spec.channelnames` 커스텀) + 메타데이터 attribute.
  정렬: 출력 시점에 coverage desc 정렬 후 emit.
- `FilmOutputs::FilmOutputType`에 2 enum 추가 + `String2FilmOutputType`
  매핑 + `filmparse.cpp` 채널 등록 + `film.h` 채널 enum/멤버 + GPU:
  `film_types.cl` hasChannel 플래그 2 + `FILM_PARAM_DECL` tail 4 버퍼 +
  `film_funcs.cl` 머지 + `InitFilm`/`SetFilmKernelArgs` 바인딩.
  **새 채널은 반드시 FILM_PARAM tail에 추가** (인자 순서 계약).

### 2.5 Blender 통합

- `aovs.py`: `view_layer.use_pass_cryptomatte_object/material/asset`
  (네이티브 Blender 프로퍼티 — 이미 존재) → `film.outputs.N.type =
  CRYPTOMATTE_OBJECT` 등 emit. 패스명은 Blender 컨벤션 `CryptoObject00`…
- 매니페스트 라이브 전달: EXR 경로는 메타데이터로 해결. 인-메모리
  RenderResult 경로는 `cryptomatte/<md5>/manifest` 메타데이터를
  render result에 스탬프하는 어댑터 API 조사 필요 — 불가 시 파일사이드
  폴백 + compositor Cryptomatte 노드는 파일 기반 동작 확인.
- UI: View Layer 패스 패널에 cryptomatte 토글 (네이티브 UI 그대로 사용).

### 2.6 테스트

- `dev-tools/eNN_cryptomatte_test.py`: 겹친 반투명 오브젝트 3개 +
  배경, 720p. 검증: (a) 매니페스트 JSON 완전성+id↔name 일치,
  (b) 픽셀 단위 커버리지 합 ≈ 1 (오브젝트 영역), (c) 반투명 겹침 픽셀에
  복수 id, (d) CPU/GPU 동일 해시/커버리지 (±몬테카를로), (e) EXR 채널명/
  메타데이터 스펙 준수 (oiio `iinfo -v` 파싱), (f) 6개 초과 id 시 최소
  커버리지 교체 정책.
- 시각: 컴포지터에서 id 마스크 추출 → 재색상 검증 이미지.

---

## 3. Light linking

### 3.1 시맨틱 정의

**수신자 기준 조명 링크**: 라이트 L이 오브젝트 O를 비출 수 있음 ⇔
L이 O의 수신 그룹 집합과 링크. Cycles 모델: 라이트가 receiver collection을
선언 → "그 컬렉션의 오브젝트만 비춘다". 우리는 **양방향 그룹 마스크** 모델:

- 링크 그룹 = named set (최대 64개 → u64 마스크).
- `scene.lights.Y.linkgroups = "a,b"` — 라이트가 속한 그룹 집합.
- `scene.objects.X.linkgroups = "a"` — 오브젝트가 받아들이는 그룹 집합.
- 조명 판정: `linked(O, L) = (L.groups ∩ O.groups ≠ ∅) || L.groups == ∅`
  — 링크 없는 라이트는 글로벌(모두를 비춤). 오브젝트에 링크만 있고
  라이트가 글로벌이면 역시 비춤 (Cycles 의미론: 글로벌 라이트는 무제한).
- `scene.objects.X.linkmode = include|exclude` — exclude는 수신 그룹을
  차단 집합으로 해석 (include의 부정).

에미시브 메쉬: 삼각형 라이트는 **발광 오브젝트의 linkgroups를 상속**
(`TriangleLight::sceneObject` 역참조로 자연 해결 — 라이트별 링크가 아닌
오브젝트 링크). 월드/무한 라이트: `scene.infinitelight.linkgroups` 등
동일 프로퍼티.

### 3.2 데이터 구조

- CPU: `SceneObject::linkMask` (u64), `LightSource::linkMask` (u64).
  그룹명→비트 매핑은 Scene가 보유 (`map<string,int> linkGroupTable`).
- GPU: `ocl::SceneObject`에 `unsigned long linkMask` (+8B, 24B→32B),
  `ocl::LightSource`에 동일 필드 — `compilesceneobjects.cpp`,
  `compilelights.cpp`에서 채움.
- 삼각형 라이트의 linkMask = 소유 오브젝트의 linkMask (컴파일 시 복사).

### 3.3 조명 판정 위치 (unbiased 정확성)

핵심: **필터가 실제 후보 분포와 pdf를 일치시켜야** unbiased 유지.

1. **Flat 분포** (UNIFORM/POWER/LOG_POWER/DLSC): 샘플 후 마스크 테스트 →
   비링크면 contribution=0, **pick pdf 유지** → unbiased (샘플 낭비뿐).
   CPU `PathTracer::DirectLightSampling` + GPU `DirectLight_Illuminate`
   (`pathoclbase_funcs.cl:1155`) — `SampleLightsBSDF` 직후 체크.
2. **LightBVH**: 노드에 `linkUnion` (u64, 서브트리 라이트 linkMask의 OR +
   비링크 라이트는 bit63 "global" 표시) 추가 — 56B 노드에 +8B.
   `LightBVH_NodeImportance`에서 `(nodeUnion & objAcceptBits) == 0`이면
   중요도 0 → 서브트리 프루닝 → **pdf 계산이 자연스럽게 필터된 분포 반영**
   (`SampleLightPdf`도 동일 규칙 적용 — 양쪽 일치).
   `objAcceptBits = objMask | bit63` (글로벌 라이트는 항상 수신).
3. **ReSTIR DI**: 후보 타겟 평가에 링크 테스트 — t=0이면 reservoir가
   자연 거절. pdf 계산 경로는 BVH/flat 규칙 공유.
4. **Direct hit** (BSDF 경로가 에미시브 히트): `DirectHitFiniteLight`/
   `DirectHitInfiniteLight` — 수신 오브젝트(직전 버텍스 bsdf의
   `sceneObjectIndex`) vs 히트 라이트 마스크. 비링크 → 기여 0.
   NEE가 샘플링할 수 있든 없든(정책 무관) 양쪽 0 → MIS 일관.
5. **Light tracing/BDPT/VC**: 커넥트 시점에 eye 버텍스 오브젝트 vs
   발광 라이트 마스크 테스트 (VC: light vertex의 originating light index
   → lights[].linkMask vs eye vertex 오브젝트 mask).
6. **볼륨**: `sceneObjectIndex == NULL`인 볼륨 버텍스는 전체 수신
   (마스크 올원) — v1; 볼륨 호스트 오브젝트 상속은 v2 옵션.
7. **월드 라이트**: `InfiniteLight`/`SkyLight`의 `linkgroups` 프로퍼티 —
   `lightDefs[i].linkMask`에 컴파일. 환경 링크는 "오브젝트가 환경광
   차단" 케이스 (제품 렌더에서 흔함).

### 3.4 Shadow linking (블로커 링크)

`light.shadowgroups`/`object.shadowmode` — 라이트 L에 대해 오브젝트 O가
섀도우를 드리울 수 있는가. 구현 지점이 다름: **섀도우 레이 추적 중
오클루더별 테스트** 필요 → `Scene::Intersect` 계열에 blocker mask 전달.
- shadowRay 생성 시 라이트의 shadowMask를 레이 컨텍스트에 탑재
  (GPUTask 필드 또는 shadow ray 스레드 로컬),
- intersection에서 후보 오클루더 `sceneObjectIndex → shadowMask` 체크,
  비링크면 패스스루처럼 계속 진행.
- 비용: 오클루더 히트마다 인디렉션 1회 — 프로덕션급 기능이지만 침습적.
  **Phase 2**로 분리 (illumination linking이 선행).

### 3.5 Blender 통합

- 네이티브 API 매핑: Blender 5.x의 `light.light_linking.receiver_collection`
  / `blocker_collection` → export 시 컬렉션 멤버십을 링크 그룹 비트로 해석
  (컬렉션명→그룹 인덱스 테이블을 exporter가 씬 단위로 생성).
  - `export/light.py`: `_convert_common_props`에 link group 해석 추가,
    `scene.lights.Y.linkgroups` emit.
  - `export/object.py` (오브젝트 emit 경로): `scene.objects.X.linkgroups`.
  - 월드: `export/world.py` — `scene.infinitelight.linkgroups`.
- 폴백/수동: `object.superluxcore.light_link_groups` (콤마 문자열) 프로퍼티
  + 오브젝트 패널 UI — 네이티브 light linking 미사용 씬 대비.
- 라이트그룹(AOV 그룹핑)과 **완전 분리** — 혼동 주의, UI 라벨링 구분
  ("Radiance Group" vs "Light Linking").

### 3.6 테스트

- `dev-tools/eNN_lightlink_test.py` 720p: 라이트 3개(A,B,C), 오브젝트 3개
  (A만 수신, B만 수신, 링크 없음). 검증: (a) 링크된 오브젝트만 해당 라이트에
  조명, (b) 글로벌 라이트는 전체 조명, (c) 에미시브 메쉬 상속,
  (d) CPU/GPU 동일 결과 (동일 spp RMSE ≈ 0 — **링크 판정은 결정적이므로
  몬테카를로가 아니라 정확히 동일해야**), (e) MIS 일관성: BSDF-히트 vs
  NEE 결과 동일 밝기 (같은 씬 `lightstrategy.type` UNIFORM vs LIGHT_BVH
  결과 비교 — 동일해야), (f) exclude 모드.
- 비편향성: 링크 없는 씬 vs 링크 테이블 비어있는 씬 — 동일 출력.

---

## 4. LPE (Light Path Expressions)

### 4.1 설계 원칙 — 문자열이 아닌 오토마타

경로별 이벤트 시퀀스를 들고 다니는 방식은 GPU에서 불가. 표준 해법:
등록된 각 LPE를 **NFA로 컴파일**, 경로가 NFA 상태 집합(비트마스크)을
운반. 버텍스 이벤트마다 전이 테이블로 갱신. 고정 메모리, GPU 친화적.

- 알파벳: `C`(카메라), `L`(라이트/에미션 히트), `E`(env/무한광), `B`(백그라운드
  miss), `D/G/S`(이벤트 클래스), `R/T`(반사/투과), `V`(볼륨 산란).
  BSDFEvent → {D,G,S}×{R,T} + isVolume → V.
- 문법 (canonical LPE 부분집합): 심볼, `<set>`, `.`, `*`, `+`, `|`, `()`.
  예: `C.*L`(전체), `CD.*L`(디퓨즈 간접), `CS+L`(커스틱 경로), `C<RD>`,
  `C.*<L.3>` (라이트그룹 태그 — 라이트그룹과 LPE의 시너지).
- NFA 컴파일 (Thompson) → ≤32 상태 보장, 초과 시 에러. 전이 테이블:
  `u32 delta[expr][state][symbol]` → 플랫 버퍼 업로드.

### 4.2 런타임 상태

- `EyePathInfo` (CPU `pathinfo.h`, GPU `pathinfo_types.cl`)에
  `u_int lpeStates[LPE_MAX]` 추가 — LPE_MAX=8 → +32B/task.
  카메라 레이 생성 시 NFA start-set으로 init (`EyePathInfo_Init` +
  `MK_GENERATE_CAMERA_RAY` + CPU 대응 지점).
- 전이 훅: `EyePathInfo_AddVertex` (GPU `pathinfo_funcs.cl:72`,
  CPU `PathInfo::AddVertex`)에서 심볼 방출 → `lpeStates[i] = step(...)`.
- 종단 평가: `SampleResult::AddEmission`/`AddDirectLight` (CPU) /
  `SampleResult_AddEmission`/`_AddDirectLight` (GPU `sampleresult_funcs.cl`)
  — 기여 생성 시점에 L 심볼로 최종 전이 후 accept 마스크 평가 → 매칭된
  표현식 채널에 radiance 추가. **env/무한광 히트는 E 심볼**, miss는 B.
- `SampleResult`에 `Spectrum lpeRadiance[LPE_MAX]` (+8×12B=96B).
  스플랫 시 `channel_LPEs[i]`에 누적 — RADIANCE_GROUP 패턴 재사용
  (`filmRadianceGroup` 포인터 배열과 동일 인프라).

### 4.3 채널·출력

- `film.lpe.N.expression = "C.*L"`, `film.lpe.N.name = "indirect"` —
  파싱 시 NFA 컴파일, 채널 i 등록. 실패 표현식 → 명확한 에러(토큰 위치).
- `channel_LPEs` = `vector<GenericFrameBuffer<4,1,float>>` —
  `film.outputs.N.type = LPE` + `.index`/`.name` → EXR 레이어명
  `LPE.<name>` (또는 사용자 지정).
- GPU: `FILM_PARAM_DECL`에 `__global float **filmLPE` 포인터 배열 추가
  (radiance group과 동일 패턴 — arg 비용은 포인터 1개).

### 4.4 알고리즘 경계 (v1 범위)

- **eye-path 기여만** 정확: 카메라 서브패스 이벤트 + 종단 L/E/B.
  NEE 엣지 = 종단 L로 취급 (표준 해석).
- 라이트 트레이싱/VC 스플랫: v1은 **eye-prefix + 종단 L** 근사로 분류
  (카메라 커넥트 버텍스의 누적 상태 + L). 라이트 서브패스 이벤트를 역순
  재생하는 정확 평가는 v2 — BDPT 의미론과의 정합성 검증 필요.
- MNEE/LMNEE: manifold 버텍스들은 eye 이벤트로 기록되므로 자연 커버.
- 패스스루(투명) 버텍스: 이벤트 없음 (표준 LPE 관례).

### 4.5 테스트

- 표준 표현식 세트로 720p 검증: `C.*L`==beauty, `CDL`==DIRECT_DIFFUSE+α,
  `C.*DL`==INDIRECT 합, `CS.*`==caustic 근사, `C.*<L.i>`==RADIANCE_GROUP_i
  — **기존 AOV와의 수치 동치성 검증**이 최강 테스트.
- 문법 에러 주입 (닫히지 않은 괄호, 잘못된 심볼) → 에러 메시지 검증.
- CPU/GPU parity 동일 spp.

---

## 5. Path guiding 성숙도

OpenPGL 도입이 아니라 **검증된 아이디어 3개의 선별 이식** + 프로덕션화
정리. GPU 계약(`guideNodes`/`guideLeaves` flattened 포맷, `LEAF_FLOATS=24`)
는 유지 — CPU 트레이너만 개선하면 GPU는 무상 업그레이드.

### 5.1 계층적 폴백 (cold leaf → warm ancestor) — 비용 최저/효과 최대

현재: `WARMUP_RECORDS=256` 하드 게이트 — 미달 리프는 가이드 자체 OFF.
변경: `SnapshotTree`/`BuildReadTree` 시점에 cold 리프에 **최근접 warm
조상의 피팅 데이터를 베이크**.

- 필요 인프라: 내부 노드에 서브트리 통계 집계 (리프 히스토그램 합산 —
  동일 16×8 빈 그리드라 합치기만 하면 됨). 쓰기 트리 빌드 시 bottom-up
  집계 → 조상별 EM 피팅 (분할 깊이별 비용은 리프 수에 선형).
- 리프 레코드에 "borrowed" 플래그 (count 필드 상위비트 또는 여유 슬롯) —
  `MixWeight`가 차용 피팅에 감쇠 적용 → 콜드 리프가 무리하게 가이드하는
  리스크 방지. GPU 계약 변경 0.
- 효과: 콜드스타트/드문 영역에서 가이드 커버리지 확장 — OpenPGL이 실제로
  쓰는 기법, 정직한 unbiased (피팅된 필드가 더 거칠 뿐 여전히 양의 pdf).

### 5.2 적응형 컴포넌트 수 (K=1..4)

고정 K=4 EM 리피팅 → **BIC/잔차 기반 선택**: K∈{1..4}로 EM을 돌리고
BIC(로그우도 − ½k·log n) 최대 모델 선택. 단일모드 리프의 과적합/
수렴 불안정 해소, 멀티모드는 K=4 유지. `nComp` 필드 이미 존재
(계약 변경 0). CPU `pathguiding.cpp` FitLeaf 내부 변경만.

### 5.3 PAVMM (parallax-aware vMF) — 리프 표현 v2

컴포넌트별 피벗 위치 저장 → 리프 내부 쿼리 위치에서 로브 재중심화.
큰 리프의 방향 스미어링 제거 — 품질 갭의 본체일 가능성 높음.
- 레코드 +3 float/component → `LEAF_FLOATS` 24→36 (+12/component ×3 =
  +36… 컴포넌트당 3B×4=12 float 추가 → 36B 구조) — **테이블 포맷 v4**
  버전업 + GPU 함수 업데이트 (`GuideTree_LeafAt` 소비자들).
- 순서: CPU 먼저 도입 + A/B (같은 씬 same-spp noise 비교) → 이득 확인 시
  GPU 포맷 버전업. 실패 시 CPU-only로 남기고 v3 유지 (호환 분기).

### 5.4 (선택) DQT / 볼륨 거리 가이딩 — 별도 평가

- Directional quadtree: 멀티모드 강건 대안 표현, flatten 용이 — GPU
  샘플러 신규 작성 필요, 비용 대비 우선순위 낮음. PAVMM 이후 재평가.
- 볼륨 거리 가이딩 (Herholz'19): 자유비행 거리까지 가이드 — 리프에
  별도 거리 히스토그램 필요, 신규 기능 영역. 볼륨 갭 감사 결과와 함께
  별도 트랙.

### 5.5 프로덕션화 정리 (성숙도의 절반)

- **`LUX_PG_*` env → 정식 프로퍼티**: `path.guiding.mindepth`,
  `path.guiding.diffuse`, `path.guiding.glossy`, `path.guiding.ris.*`,
  `path.guiding.portal.*` 등 매핑 + 기본값 확정. env는 디버그 폴백으로
  유지하되 프로퍼티가 우선. Blender 어댑터에 즉시 노출
  (`properties/world.py` or `scene.py` + `export/config.py` →
  `path.guiding.*` emit) — artist-facing 제어: guide strength,
  min depth, warm-up 표본수, 테이블 파일 경로, 리셋 버튼.
- **포털 가이딩 GPU parity**: CPU 전용 포털 경로 → GPU 포트
  (`portalRectsBuff` 버퍼는 이미 커널 인자로 존재 — 소비 경로 포팅).
- **테이블 영속성**: magic `GUID` v3 → v4 (PAVMM 도입 시) + 씬 호환성
  체크(월드 bbox·라이트 수·해시) — 불일치 시 cold-start 폴백 + 경고.
- **진단**: per-leaf 통계 덤프 정식화 (`LUX_PG_*DUMP` → `path.guiding.debug`
  프로퍼티), 수렴 지표 (가이드 수용률 = guided bounce 비율 채널/로그).
- **시간적 씬 편집 무효화**: `EndSceneEdit`에서 가이드 무효화/보존 정책
  명확화 (지오메트리 변경 시 관련 리프 리셋 — v1은 전체 리셋+경고도 허용,
  문서화).

### 5.6 테스트

- `dev-tools/eNN_pathguiding_test.py` 720p+: 어두운 실내+작은 창(하드
  케이스), 코넬, 볼륨 씬. 검증: (a) 가이드 on/off same-spp 이미지
  **동일 평균**(unbiased), (b) 분산/노이즈 지표 개선 (지역 variance
  비교), (c) 콜드스타트 안정성 (초기 spp에서 발산/아티팩트 없음),
  (d) CPU/GPU 동일 필드 (동일 테이블 파일 로드 시 동일 결과),
  (e) 테이블 저장→재로드→재개 round-trip, (f) 프로퍼티 vs env 우선순위.
- 벤치: guided bounce 비율, 리프 수/메모리, fit 시간.

---

## 6. 공통 통합 인프라 (5개 갭 공유)

### 6.1 GPU 필름 채널 추가 절차 (Cryptomatte/LPE 공용)

새 채널 추가 시 수정 지점 (이 순서대로 — 인자 순서 계약 엄수):

1. `include/slg/film/film.h` — `FilmChannelType` enum + `channel_*` 멤버.
2. `src/slg/film/filmparse.cpp` — `String2FilmChannelType` + 출력 등록.
3. `include/slg/film/filmoutputs.h` — `FilmOutputType` enum + 문자열 맵.
4. `include/slg/film/film_types.cl` — `hasChannel*` 플래그 + `FILM_PARAM_DECL`
   **tail** 버퍼 + `FILM_PARAM` tail 인자 (순서 엄수, 절대 중간 삽입 금지).
5. `include/slg/film/film_funcs.cl` — `Film_AddSampleResultData` 머지.
6. `include/slg/film/sampleresult_types.cl` + `sampleresult.h` — 필드 추가
   (tail, ABI 정렬 주의 — 둘 다 같은 레이아웃).
7. `src/slg/engines/pathoclbase/pathoclbaseoclthreadfilm.cpp` —
   `InitFilm` 할당 + `SetFilmKernelArgs` 바인딩 (argIndex 순서 = 매크로 순서).
8. `src/slg/film/filmaddsample.cpp` — CPU 누적 경로.
9. `src/slg/film/filmoutput.cpp` — EXR 출력 + 메타데이터.
10. `SuperBlendLuxCore/export/aovs.py` + `properties/aovs.py` +
    `ui/view_layer_aovs.py` — 어댑터 노출.

### 6.2 GPU 태스크 상태 확장 (LPE/light-link 공용)

- `EyePathInfo` (`pathinfo_types.cl` + CPU `pathinfo.h`): +32B (lpeStates).
- `ocl::SceneObject`: +8B linkMask (24B→32B).
- `ocl::LightSource`: +8B linkMask.
- 커널 인자는 KERNEL_ARGS tail 규칙 — Apple 인자 한계 주의
  (WAVEFRONT_GID 코멘트 참조: Apple OCL은 버퍼 인자 수 제한 있음 →
  새 인자는 포인터 배열로 묶거나 struct로 압축 검토).

### 6.3 씬 프로퍼티 네임스페이스

| 기능 | 프로퍼티 |
|---|---|
| Cryptomatte | `film.outputs.N.type = CRYPTOMATTE_OBJECT/MATERIAL/ASSET` |
| Light linking | `scene.objects.X.linkgroups`, `scene.lights.Y.linkgroups`, `scene.objects.X.linkmode`, `scene.lights.Y.shadowgroups` (ph2) |
| LPE | `film.lpe.N.expression`, `film.lpe.N.name`, `film.outputs.N.type = LPE` |
| Guiding | `path.guiding.*` (기존 + LUX_PG_* 정식화분) |

---

## 7. 페이즈·의존성·리스크

| Phase | 내용 | 의존성 | 게이트 |
|---|---|---|---|
| P0 | 공통 인프라: murmurhash 유틸, film 채널 추가 절차 문서화 검증(더미 채널 1개 end-to-end) | — | 채널 파이프라인 동작 확인 |
| P1 | Cryptomatte (object+material) | P0 | §2.6 테스트 + EXR 스펙 검증 |
| P2 | Light linking (illumination, flat+BVH) | — | §3.6 테스트, MIS 일관 |
| P3 | Wavefront M3a (sync 제거) | — | 동기 제거 후 회귀 재측정 → 계속/중단 결정 |
| P4 | LPE (eye-path 완전) | P0(채널) | §4.5 AOV 동치성 |
| P5 | Guiding 5.1+5.2+5.5 (fallback, adaptive K, 프로퍼티화) | — | §5.6 테스트 |
| P6 | Wavefront M3b-e (조건부) | P3 결과 | dense 대비 승리 워크로드 존재 |
| P7 | Guiding 5.3 PAVMM + GPU 포맷 v4 | P5 | CPU A/B 이득 확인 시 |
| P8 | Shadow linking, crypto asset layer, LPE light-path 정확 평가 | P2,P1,P4 | 각 테스트 |

**리스크·롤백**:
- Wavefront: 게이트 미달 시 코드는 opt-in 유지, 큐 인프라는 다른 기능
  (컴팩션 필요한 워크로드) 재활용 검토. 신규 동기 없는 스케줄러는
  `LUXRAYS_WAVEFRONT_QUEUES=1` 아래 격리 — dense 무영향.
- Cryptomatte: 채널 미등록 시 경로는 dead — 영향 0. SampleResult 필드는
  무조건 추가되나 ABI tail이라 안전.
- Light linking: 링크 없는 씬은 `linkMask==0`(글로벌) → 테스트 분기 외
  비용 ~0 (BVH 노드 +8B만).
- LPE: 미등록 표현식 → `lpeStates` 업데이트만 스킵, 기여 경로 무영향.
- Guiding: 계약 변경 없는 개선(5.1,5.2)만 우선 → PAVMM은 버전 게이트.

## 8. 검증 매트릭스 (공통)

- 자동 회귀: `dev-tools/` 신규 eNN 테스트 각 1개씩, 기존 e26 계열 패턴
  (unbiased same-spp 비교, CPU/GPU parity, finiteness).
- 시각: 720p+, AGX punch/ACES2.0 톤맵, 최종 쇼케이스는 wow-factor 씬.
- 성능: 벤치 씬 세트 (cornell/classroom/luxball/spectral + 대형씬),
  dense 기준선 대비 Δ, 메모리 델타.
- 문서: 각 기능 `doc/features/*.md` + 참조 논문/스펙 명시 (업스트림
  기여 가이드라인 준수 — 설명·근거·테스트 씬·빌드 가능성).

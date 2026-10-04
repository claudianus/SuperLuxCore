# 메가플랜 — 프로덕션 SOTA 렌더러 (무한 재귀개선 하니스)

작성일: 2026-09-30 · 상태 갱신: 2026-10-04 · 관리: ROADMAP.md §3/§4의 실행 재편본

> **10/04 진행 요약**: ① 건틀릿 v2 ~80%(태그 10/12 — `large-geo`·`production-hero`
> 미작성) · ② 프로파일/ledger ✅(r1–r3) · ③ 최적화 라운드 r2–r6 랜딩 후
> **10/03부터 중단**(Blender 5.2 패리티·2.11.8–2.11.11 릴리스로 전환), 5라운드
> 주기 전체 재측정 미실행 · ④ CI 절반(wheel 4플랫폼+GPU 스모크 ✅, parity CI·
> Vulkan 절차 ❌). 재개 순서는 ROADMAP §0-4.
한 줄 목표: **"켜면 작동하는" Corona급 zero-config UX + 사전캐시 없는 고속
프로그레시브 수렴 + 모든 고난도 경로(다중 커스틱스/간접광/볼륨/스펙트럴)의
품질·속도**를 CPU+GPU 동등성 아래 제공하는 프로덕션 렌더러.

---

## 1. 지시 해석 → 실행 원칙

| 지시 | 실행 원칙 |
|---|---|
| 사전캐시 없이 빠른 프로그레시브 | 캐시(PhotonGI 등)는 옵션 부스터로 격하. 기본 경로는 캐시 없이 수렴하는 추정기 조합(LT+MNEE+guiding+ReSTIR)으로 재편 |
| Corona식 UX | 씬 시그니처→엔진 프로퍼티 자동결정. 측정 입증분은 기본값 승격, 전부 호환 플래그로 가역 |
| 모든 고난도 경로 품질·속도 | 건틀릿이 최악 경로를 드러내고 ledger가 병목을 순위화, 라운드가 소진 |
| biased/unbiased 무관 | "물리적으로 makes sense"한 범위에서 수렴 속도 우선. 바이어스는 consistent·문서화·아티스트 제어 가능 |
| GPU+CPU 100% | 모든 신규 경로는 CPU+GPU 쌍 구현 + parity 게이트가 승격 조건 |
| 전 플랫폼/벤더 | 로컬 Metal+CPU, Win/Linux/NVIDIA는 CI 매트릭스. Vulkan 네이티브는 문서화된 수동/셀프호스티드 경로 |
| 무한 루프 | §4의 라운드 프로토콜을 커밋 단위로 반복. ledger가 다음 작업을 자동 생성 |

---

## 2. 기능→기본값 승격 매트릭스

"측정 이득 입증 + 패리티 + 가역 플래그" 3종 충족 시 기본값 승격.
상태: ✅이미기본 / ◐측정됨·승격검토 / ❌미측정 / — 유지(opt-in이 정답).

| 기능 | 프로퍼티/플래그 | 측정 근거 | 기본값 변경 리스크 | 상태 |
|---|---|---|---|---|
| 라이트 전략 자동 | `lightstrategy.type` 기본값 | e26 10/10(unbiased·RMSE -36%·cpu-gpu 패리티) + 건틀릿 평균동등 — 분기 규칙 불필요, BVH가 flat을 지배하고 emit 전용은 LogPower 폴백 | 중(전략 선택 바뀜) | ✅ `5be8ade72` 기본값 승격 |
| GGX 디폴트 | 머티리얼 `distribution=ggx` (6종 opt-in 완료) | 에너지 보존 정확도↑, 외관 델타 존재. 10/04 코드 확인: 6종 모두 기본 `schlick`(`parsematerials.cpp`) | 고(외관 변경) | ❌ 호환 플래그 설계 후 승격 — **다음 승격 1순위** |
| 멀티바운스 GGX | `multibounce` opt-in (metal2/roughglass/carpaint/disney) | e50 패리티, 등시간 이득 미정량 | 중 | ❌ 건틀릿 A/B 미실행 |
| Wavefront 큐 | `pathocl.wavefront=auto` (auto=off) | 9/25 cornell "~2.5x"는 동시 GPU 오염 측정 → **폐기**. 10/02 클린 재측정: classroom-hdr 720p off 9.52 vs on 3.89 Ms/s, cornell off 9.88 vs on 5.70 — wavefront가 느림(`a826d6079`, SESSION_LOG 10/02). auto 승격 기각(`12ef3746c`), 꺼진 상태에선 큐 커널 컴파일도 생략(22→19) | 중 | — **유지(auto=off)**. 재평가는 totals readback 제거 등 구조 개선 후 |
| Path guiding | `path.guiding.enable` (P5 정식화) | 10/02 A/B(48spp 고정): portal-interior ×2 BASE 0.312/PG 0.307(1차 +8.9%는 노이즈), classroom 동일 — 프로덕션 예산에서 이득 미입증 | 중 | — **opt-in 유지**(`5e10b6121`). 고spp/장시간 렌더 재측정 시 재개 |
| ReSTIR GI | `path.restir.gi.enable` + BLC 토글 | e19 10/10, e30 manylights RMSE -37%(고spp). 10/02 A/B manylights 48spp: BASE 0.20991/GI 0.2099 동일 | 중 | — **opt-in 유지**(`dab1dec91`). 고spp 조건 재측정 시 재개 |
| SSP tail | `path.ssp.enable` | 커스틱 경로 이득, 디퓨즈 씬 비용 | 중 | ❌ 건틀릿 A/B 미실행 |
| 적응 커스틱 파티션 | `path.hybridbackforward.adaptivecaustic` | e25 회귀 통과 | 저 | ◐ auto-causal 라우팅과 통합 |
| 라이트 패스 자동 | `path.lighttracing.auto`(기본 on) + 씬 시그니처 | 커스틱 가능 재질(SPECULAR\|GLOSSY)/산란볼륨+광원 → 자동 enable; 디퓨즈-only는 태스크 예산 보존 | 중 | ✅ `67b15c522` (HBF는 기존 프로모션 규칙으로 연동) |
| MNEE 자동 | `path.mnee.auto`(기본 on) + 동일 시그니처 | eye-side 커스틱 솔버 — connect당 자체 게이트라 미적용 씬에서 거의 무비용; GPU LMNEE는 LT 태스크에 편승 | 중 | ✅ `72369ed5f` (luxball/bigmonkey 6케이스 + stress 씬 24spp 커스틱 + GPU finite + 패리티 4/4) |
| GPU zero-tail 폴백 | `taskCount<=8192` 시 `lightTaskCount==0` | 억제만 걸리고 보상 패스 없는 잠복 블랙아웃 — PATHOCL은 네이티브로 라이트 패스 위임(hbf 강등), TILEPATHOCL은 네이티브/PGIC 보상 시 lt 유지, 무보상 시 lt/hbf/vc 해제 | 중 | ✅ (e17 T-1 회귀 추가, 패리티 4/4, demote 시 mean 0.0626 finite) |
| VC/VM | `path.vertexconnection.enable` | VCM 본질 biased. 10/02 A/B caustic-stress-many 48spp: 동일(기본 `mergeradius=0`이라 auto MNEE+LT가 이미 커버하는 연결만 추가) | 중 | — **opt-in 유지**(`a39db1d52`), 일관성 게이트는 여전히 선행 |
| PSR | `path.regularization.auto`(기본 on) + `.sigma`/`.halflife` | v1+감쇠 (`eadcf8319`) → 씬 시그니처 자동 시드 sigma=0.03/hl=64 (`72b42a4b7`); 커스틱 가능 씬만 시드, diffuse 제외·명시값 우선 검증; 델타→로브 치환은 후속 | 중 | ✅ auto 시드 랜딩 (focused-ring RMSE 측정 후 halflife 기본값 재평가) |
| ARC | 적응 클램핑 | e42 통과 | 저 | ❌ 기본값 A/B 미실행 |
| OIDN 잔차 피드백 | NOISE 채널 피드백 (e98, `1fbf83977`) | 디노이저 잔차→샘플링 | 저 | ❌ 기본값 A/B 미실행 |
| 볼륨 residual 트래킹 | 볼륨 설정 자동 | e34 패리티 | 저 | ✅ 볼륨 경로 기본 적용 영역 |

승격 실행 규칙: 한 항목=한 커밋. `Properties` 기본값 변경 + 구 동작 복원용
명시 플래그 + 건틀릿 등시간 A/B 수치 + `doc/features/` 기록 + BLC 노출.

## 3. 측정 게이트 규약 (SESSION_LOG gotchas 코드화)

모든 성능·품질 판단은 아래 규약을 따른다. 하니스에 내장한다.

1. **idle 가드**: wall 계측 전 `sysctl -n vm.loadavg`의 1분 부하 확인 —
   목표: 논리코어의 ~50% 미만. 고아 렌더/프로파일 프로세스는 정확 PID로 정리.
2. **min-of-3 interleaved A/B**: 대조군·실험군을 교대로 3회, min(또는
   median) 비교. 단발 wall 비교 금지.
3. **fixed-spp GPU A/B**: 상태머신/솔버 변경은 이미지 A/B(고정 spp) 먼저,
   wall은 그 다음. NaN/Inf 스케일 이동은 즉시 기각(crawl-bail revert 교훈).
4. **wavefront phase-경계 규칙**: 커널 상태머신 조기 종료는 phase 경계의
   클린 핸드오프로만.
5. **노이즈 플로어**: 동일 빌드 run-to-run 플로어를 먼저 측정(e52: relLum
   ±0.85%, hiRel ±6.5%)하고 그 이하 변화는 "변화 없음"으로 판정.
6. **패리티 게이트**: 모든 변경은 `dev-tools/parity-regression.sh` + 해당
   e-test 통과. 신규 기능은 CPU/GPU 쌍 + 어댑터 노출이 승격 조건.
7. **시각 게이트**: 720p+ 렌더를 이미지로 직접 확인(ACES 2.0, 상하반전
   주의), 결과는 고유 파일명으로 `renders/`에 보존.

## 4. 무한 재귀개선 루프 — 라운드 프로토콜

```
건틀릿 측정 → 프로파일/attribution → perf-ledger.md 갱신
   → 최상위 항목 1개 선택 → 구현 → 정확도 게이트(비트동일|fixed-spp)
   → 건틀릿 A/B(min-of-3 interleaved) → 패리티+e-test
   → 커밋+push → SESSION_LOG 기록 → 다음 라운드
```

- 한 라운드=한 커밋 단위. 예상 이득이 ledger에 기록된 것부터 소진.
- 라운드 산출물이 이득 0이면 "기각"도 SESSION_LOG에 기록(재시도 방지).
- 5라운드마다 건틀릿 전체 재측정으로 ledger 재순위화.

**초기 백로그(이미 계측됨, e53 gauntlet 640×360 CPU 프로파일)** — ✅ 1–3 전량
`e25c8ffc6`(9/30)로 소진:
1. ~~`Spectral::ProjectToRGB` ~1.5%~~ ✅
2. ~~`DataSet::GetAccelerator` per-ray ~0.3%~~ ✅
3. ~~`LightBVH::NodeImportance` 삼각항 ~0.1%~~ ✅
4. 이후 라운드 r2–r6(`537a48c6d` Sobol 필름 캐시, `242ec2765` 볼륨 상수 캐시,
   r4 디스패치/taskCount 스윕, r5 sampleResults, `0dd8d6ee7` 볼륨 fast-gate) 랜딩.
   현행 후보는 `doc/engineering/perf-ledger.md` "Backlog" 절 + 구조 항목(스레드별
   splat 버퍼, SampleResult 축소, Metropolis replay SIMD, GPU PGIC KD-tree).

**10/04 상태**: 라운드는 10/03 이후 중단. 재개 시 첫 작업은 유휴 머신에서
건틀릿 전체 재측정(5라운드 주기 미이행분) → ledger 재순위화.

## 5. 통합 건틀릿 v2 (측정 기준)

`dev-tools/g1_gauntlet_bench.py` 확장 + `scenes/gauntlet/` 코퍼스 확장.

| 태그 | 커버 | 비고 |
|---|---|---|
| prism-conservatory | 분산 커스틱+다중 유전체 | 기존 |
| vol-caustic-deep | 볼륨 커스틱 심층 | 기존 |
| focused-ring | 커스틱 포커스 링 | 기존 |
| multi-caustic-chain | SDS/SMDS 체인+분산 | ✅ |
| glossy-caustic-mix | 글로시↔스페큘러↔디퓨즈 혼합 | ✅ |
| dense-volume | 이질 볼륨+다중산란+볼륨빔 | ✅ `65e06d58f`/`4c72ac7ad` (10/03) |
| manylights-interior | 100+ 이미터 간접 | ✅ `65e06d58f` |
| sss-hair-fur | SSS+Huang 헤어 밀집 | ✅ `65e06d58f` |
| glints-thinfilm | 플레이크+박막+회절격자 | ✅ `65e06d58f`/`4c72ac7ad` (글린트 BSDF 자체는 미구현 — S5) |
| large-geo | 수백만 tri+인스턴싱+MB | ❌ **미작성** |
| portal-interior | 밀폐 간접광 최악 케이스 | ✅ |
| production-hero | 전 요소 혼합 ACES 홍보씬 | ❌ **미작성** |

하니스 메트릭: fixed-spp/등시간 RMSE·PSNR·수렴곡선·peak RSS·컴파일시간
분리·CPU/GPU 패리티 점수. `--refgen`으로 고spp 레퍼런스 생성(구현됨 — 출력은
EXR이 아니라 linear float32 `.npy`, `renders/gauntlet/ref/<tag>.npy`). 하니스에는
위 v2 태그 외 기존 회귀씬(mirror-maze/pool/manylights/classroom/bigmonkey-inst 등)도 등록됨.

## 6. 하드패스/품질 기능 백로그 (건틀릿 갭이 재정렬)

1. 매니폴드 패스 가이딩 — ◐ **MPG-lite 랜딩**(Phase A `940664923`, B `60dc8ec1f`;
   `doc/engineering/manifold-path-guiding.md`). 잔여: 풀 MPG(Fan'23)
2. PSR 완성 — ✅ halflife 감쇠(`eadcf8319`)+자동 시드. ❌ 델타→로브 치환
3. 스펙트럴 심화: 4빈·λ360–830·형광 bin-shift — ❌ (n/k 18종·Sellmeier는 10/01 랜딩)
4. S5 글린트(discrete counting+PNM)·S6 spec-AA — ❌, 공통 선행: ray-differential transport
5. N층 박막 transfer-matrix·형광
6. 볼륨 다중산란 근사
7. 수렴 게이트: VCM 일관성(BIDIRVM 레퍼런스)·ReSTIR bounded-bias 경계 문서화
8. VK-M4 네이티브 드라이버·>VRAM 스트리밍·deep EXR·shadow linking

## 7. CI 멀티플랫폼 매트릭스

- `wheel-builder.yml` 확장: ✅ ubuntu/windows-2022/macos-15(arm+intel) 빌드 +
  wheel GPU 스모크 게이트(`60f6115b7`/`47909b93f`/`b67251ac7`, GPU/OpenCL 없는 러너는
  skip) + 버전 락스텝 게이트. ❌ CPU 패리티 스모크(`parity-regression.sh`) CI 편입,
  스모크 리포트 아티팩트.
- Vulkan 콜드컴파일은 수십 분 — CI는 warm-캐시 게이트 또는 수동 트리거로.
- 네이티브 Vulkan(NV/AMD/Intel) 검증 절차를 `doc/engineering/`에 명세,
  셀프호스티드 러너 도입 시 연결.

## 8. 운영 규칙 (매 세션 강제)

- 커밋+push는 라운드 단위로 즉시 (main, GitHub origin). 커밋 전 diff 전량 리뷰.
- 문서: 기능→`doc/features/`, 노트→`doc/engineering/`, 이력→workspace
  `dev-tools/SESSION_LOG.md`. AGENTS.md 편집 금지.
- 엔진 기능은 같은 라운드에 BLC UI/익스포트 노출 +
  `SuperBlendLuxCore/dev-tools/sync_dev_install.sh`로 설치 Blender 반영.
- 공유 머신: 다른 세션의 렌더 점유 가능 — 계측 전 유휴 확인 필수.

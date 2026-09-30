# 메가플랜 — 프로덕션 SOTA 렌더러 (무한 재귀개선 하니스)

작성일: 2026-09-30 · 상태: 실행 중 · 관리: ROADMAP.md §3/§4의 실행 재편본
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
| GGX 디폴트 | 머티리얼 `distribution=ggx` (6종 opt-in 완료) | 에너지 보존 정확도↑, 외관 델타 존재 | 고(외관 변경) | ❌ 호환 플래그 설계 후 승격 |
| 멀티바운스 GGX | `multibounce` opt-in (metal2/roughglass/carpaint/disney) | e50 패리티, 등시간 이득 미정량 | 중 | ◐ 건틀릿 A/B 필요 |
| Wavefront 큐 | `pathocl.wavefront=auto` (현 auto=off) | cornell ~2.5x, classroom/luxball은 dense 우세 — 자동 분기 기준 미확립 | 중 | ❌ M2 매트릭스 후 판단 |
| Path guiding | `path.guiding.enable` (P5 정식화) | 간접 지배 씬 수렴 이득 입증, 디퓨즈 씬 오버헤드 | 중 | ◐ 자동 enable 조건 측정 |
| ReSTIR GI | `path.restir.gi.enable` + BLC 토글 | e19 10/10, manylights RMSE -37% | 중 | ◐ 자동 조건 설계 |
| SSP tail | `path.ssp.enable` | 커스틱 경로 이득, 디퓨즈 씬 비용 | 중 | ◐ |
| 적응 커스틱 파티션 | `path.hybridbackforward.adaptivecaustic` | e25 회귀 통과 | 저 | ◐ auto-causal 라우팅과 통합 |
| 라이트 패스 자동 | `path.lighttracing.auto`(기본 on) + 씬 시그니처 | 커스틱 가능 재질(SPECULAR\|GLOSSY)/산란볼륨+광원 → 자동 enable; 디퓨즈-only는 태스크 예산 보존 | 중 | ✅ `67b15c522` (HBF는 기존 프로모션 규칙으로 연동) |
| MNEE 자동 | `path.mnee.auto`(기본 on) + 동일 시그니처 | eye-side 커스틱 솔버 — connect당 자체 게이트라 미적용 씬에서 거의 무비용; GPU LMNEE는 LT 태스크에 편승 | 중 | ✅ `72369ed5f` (luxball/bigmonkey 6케이스 + stress 씬 24spp 커스틱 + GPU finite + 패리티 4/4) |
| GPU zero-tail 폴백 | `taskCount<=8192` 시 `lightTaskCount==0` | 억제만 걸리고 보상 패스 없는 잠복 블랙아웃 — PATHOCL은 네이티브로 라이트 패스 위임(hbf 강등), TILEPATHOCL은 네이티브/PGIC 보상 시 lt 유지, 무보상 시 lt/hbf/vc 해제 | 중 | ✅ (e17 T-1 회귀 추가, 패리티 4/4, demote 시 mean 0.0626 finite) |
| VC/VM | `path.vertexconnection.enable` | VCM 본질 biased — 문서화 후 제한적 기본 | 중 | — 일관성 게이트 먼저 |
| PSR | `path.regularization.sigma` | v1 랜딩, 감쇠 미구현 | 중 | ◐ halflife 완성 후 |
| ARC | 적응 클램핑 | e42 통과 | 저 | ◐ 기본값 재검토 |
| OIDN 잔차 피드백 | NOISE 채널 피드백 (e98) | 디노이저 잔차→샘플링 | 저 | ◐ |
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

**초기 백로그(이미 계측됨, e53 gauntlet 640×360 CPU 프로파일)**:
1. `Spectral::ProjectToRGB` ~1.5% — 블랙 채널 스킵+per-path weight 호이스트
2. `Scene::Intersect`의 `DataSet::GetAccelerator` per-ray ~0.3% — TLS 캐시
3. `LightBVH::NodeImportance` 삼각항 항등식 ~0.1% — 자체 검증 라운드
4. 이후 M2 건틀릿 v2 감사가 신규 항목 생성

## 5. 통합 건틀릿 v2 (측정 기준)

`dev-tools/g1_gauntlet_bench.py` 확장 + `scenes/gauntlet/` 코퍼스 확장.

| 태그 | 커버 | 비고 |
|---|---|---|
| prism-conservatory | 분산 커스틱+다중 유전체 | 기존 |
| vol-caustic-deep | 볼륨 커스틱 심층 | 기존 |
| focused-ring | 커스틱 포커스 링 | 기존 |
| multi-caustic-chain | SDS/SMDS 체인+분산 | 신규 |
| glossy-caustic-mix | 글로시↔스페큘러↔디퓨즈 혼합 | 신규 |
| dense-volume | 이질 볼륨+다중산란+볼륨빔 | 신규 |
| manylights-interior | 100+ 이미터 간접 | 신규/기존 재조합 |
| sss-hair-fur | SSS+Huang 헤어 밀집 | 신규 |
| glints-thinfilm | 플레이크+박막+회절격자 | 신규 |
| large-geo | 수백만 tri+인스턴싱+MB | 신규 |
| portal-interior | 밀폐 간접광 최악 케이스 | 신규 |
| production-hero | 전 요소 혼합 ACES 홍보씬 | 신규 |

하니스 메트릭: fixed-spp/등시간 RMSE·PSNR·수렴곡선·peak RSS·컴파일시간
분리·CPU/GPU 패리티 점수. `--refgen`으로 고spp 레퍼런스 EXR 생성.

## 6. 하드패스/품질 기능 백로그 (건틀릿 갭이 재정렬)

1. 매니폴드 패스 가이딩(MNEE seed 히스토리→중요도) — 다중 커스틱 체인
2. PSR 완성: halflife 감쇠+델타→로브 치환 (제로캐시 커스틱 핵심)
3. 스펙트럴 심화: 4빈·λ360–830·n/k DB 확장·형광 bin-shift
4. S5 글린트(discrete counting+PNM)·S6 spec-AA
5. N층 박막 transfer-matrix·형광
6. 볼륨 다중산란 근사
7. 수렴 게이트: VCM 일관성(BIDIRVM 레퍼런스)·ReSTIR bounded-bias 경계 문서화
8. VK-M4 네이티브 드라이버·>VRAM 스트리밍·deep EXR·shadow linking

## 7. CI 멀티플랫폼 매트릭스

- `wheel-builder.yml` 확장: windows-latest + ubuntu 빌드, CPU 패리티
  스모크(`parity-regression.sh`), 스모크 리포트 아티팩트.
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

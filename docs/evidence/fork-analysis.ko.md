# LuxCoreRender 포크 이후 SuperLuxCore가 더한 것

**2026년 10월 11일**, 엔진 소스 `adba86f43`, 네이티브 **2.11.27** 기준입니다.
비교의 출발점은 **LuxCoreRender 2.11.2 / `e9ced7e7a`**입니다.
[이후 509개 전체 커밋](commit-audit.json)의 날짜·제목·변경 파일·변경 분야를
분류하고, 홈페이지에 소개하는 개선의 현재 소스와 기능 문서를 확인했습니다.
모든 과거 diff를 줄 단위로 재검토했다는 의미는 아닙니다. 수정·되돌림·실험·도구
작업까지 포함하는 이력이므로, 커밋 509개가 새 기능 509개를 뜻하지 않습니다.

업스트림도 계속 개발되고 있습니다. 이날 확인한 업스트림 HEAD는
[`de8707e7232`](https://github.com/LuxCoreRender/LuxCore/commit/de8707e7232cd6c03692a92755e776e6df6ff9d1)입니다.
비교표는 포크 출발점을 기준으로 하며 이후 모든 업스트림 버전에 대한 주장이 아닙니다.

| 아티스트가 얻는 이점 | 추가·개선된 구현 | 소스 근거와 현재 범위 |
|---|---|---|
| Apple GPU 직접 활용 | Metal 연산, HWRT, 인스턴싱, 모션과 네이티브 커브 | [Metal](../../doc/features/metal-backend.md), [HWRT](https://github.com/claudianus/SuperLuxCore/commit/562a3bf5f). 지원 Apple 하드웨어 범위이며 일괄적인 속도 배수를 주장하지 않습니다. |
| 거친 금속의 빛을 더 충실하게 | Disney·Roughglass·Carpaint 다중 산란 보정과 금속 GGX 옵션 | [다중 산란](../../doc/features/multiscatter.md). 재질별 선택 기능이며 새 원본 바이너리 비교로 보여줍니다. |
| 하나의 Principled 재질 안에서 반사와 굴절 | Disney 유전체 투과 통합과 OpenPBR | [통합 투과](https://github.com/claudianus/SuperLuxCore/commit/a8785faa2). 업스트림도 별도 유리 재질로 유리를 만들 수 있습니다. |
| 파장에 따른 빛 표현 | CPU·BIDIRCPU·GPU 히어로 파장, 스펙트럴 업샘플링, Sellmeier와 회절 | [스펙트럴](../../doc/features/spectral.md). RGB·파장 비교는 포크 내부 기능 비교입니다. 업스트림에도 유리 분산이 있습니다. |
| 집중된 빛을 다루는 경로 확장 | MNEE·LMNEE·MGE, GPU 라이트 트레이싱, SSP, 적응형 분할, PhotonGI 점진적 갱신과 볼륨 광자 빔 | [지원 지도](../../doc/features/caustics-sota.md), [GPU 빔](https://github.com/claudianus/SuperLuxCore/commit/bf3c3531a), [GPU SSP](https://github.com/claudianus/SuperLuxCore/commit/2f86563f3). 업스트림의 양방향·PhotonGI 커스틱을 이어받아 확장합니다. |
| 부드러운 표면과 헤어의 표현 확장 | OpenPBR 랜덤 워크 SSS, 측정 SSS, Charlie, Chiang·Huang 헤어 | [헤어](../../doc/features/hair.md), [커브 변형 모션](https://github.com/claudianus/SuperLuxCore/commit/5e473bf8c). 코어 지원이 Blender 헤어 변형이나 Cycles SSS 전체 동등성을 뜻하지 않습니다. |
| 어려운 샘플링을 위한 선택지 | ReSTIR DI·GI·PT·PG, Light BVH, 가이딩, PMJ02, 블루 노이즈 Sobol과 노이즈 기준 종료 | [GPU PT](https://github.com/claudianus/SuperLuxCore/commit/312e5d6ff), [PG](../../doc/engineering/restir-pg-design.md). PT는 제한된 편향을 가진 선택 기능이며 PG는 중립적인 측정 사례도 있습니다. |
| 편집 중 결과의 일관성 | 씬 수정 후 필름 초기화, 중첩 재질 갱신, 지속·증분 내보내기와 뷰포트 제어 | [필름 초기화](https://github.com/claudianus/SuperLuxCore/commit/131c84733), [Blender](../../doc/features/blender-adapter.md). 지원 Cycles 노드를 고유한 빛 전달로 재사용합니다. |
| 마무리 단계의 제어 | Cryptomatte, 라이트 링크, LPE, 분산·모션 채널과 Metal OIDN | [패스](../../doc/features/aov-audit-temporal-denoise.md), [OIDN](https://github.com/claudianus/SuperLuxCore/commit/eaf91d5e4). LPE는 양방향·라이트 트레이싱 엔진을 제외합니다. |
| 배포·리소스 처리 개선 | 네 플랫폼 번들, 현대 CUDA 컴파일, 제한된 커널 컴파일 대기와 메모리 수정 | [CUDA SASS](https://github.com/claudianus/SuperLuxCore/commit/841af8310), [컴파일 제한](https://github.com/claudianus/SuperLuxCore/commit/485cba4d9). 로컬 이미지 검수는 macOS ARM64이고 나머지는 CI 빌드 근거입니다. |

현재 동작도 구분했습니다. ReSTIR PT와 SSP에는 CPU와 GPU 경로가 모두 있습니다.
정규화는 적합한 커스틱 씬에서 보수적인 감소형 스케줄로 자동 동작할 수 있습니다.
Wavefront 자동 전환은 철회되어 Dense가 자동 모드의 기본이며 Vulkan은 실험 기능입니다.

## 새 비교 렌더 구성

기존 홈페이지·진단 이미지는 전부 교체했습니다. [공개 씬 생성 코드](artist-scenes.py)로
새 형상·재질·스튜디오 조명을 만들고, 고유 이미지 11장을 실제 렌더했습니다.
1600×900 출력에 렌더러 OIDN과 실제 OCIO **ACES 2.0**을 적용합니다.
AI 작품 이미지·베이크된 조명·이미지별 밝기 정규화는 사용하지 않습니다.

금속 비교는 공식 업스트림 2.11.2와 포크 2.11.27에 동일한 형상·카메라·조명·거칠기를
사용하고 포크 쪽의 다중 산란 보정만 활성화합니다. 투과 비교는 Disney에 추가된
통합 로브가 뒤의 조명을 어떻게 굴절시키는지 보여줍니다. 회절 비교는 같은 포크의
RGB 근사와 히어로 파장 경로를 비교하며, 모든 스펙트럴 이미지가 더 좋아진다는 주장은 아닙니다.

각 쌍의 노출은 같고 양쪽에 OIDN을 사용합니다. OIDN 버전·장치는 다를 수 있습니다.
샘플 수·설정·라이브러리 지문을 기록합니다. M5에서 원본 업스트림의 선택형 OpenCL
장치 탐색만 메모리에서 껐으며 CPU 렌더러 바이너리는 수정하지 않았습니다.
작품용 기능 비교이며 최종 불편향 수렴 검증이나 성능 측정이 아닙니다.

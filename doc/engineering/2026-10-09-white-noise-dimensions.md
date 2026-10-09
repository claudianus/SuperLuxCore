# White Noise의 차원·W·스펙트럼 좌표 계약

기존 Blender White Noise는 1D의 W, 2D의 XY, 3D의 XYZ, 4D의 XYZ+W를 사용한다. 확장의 이전 `whitenoise` 매핑은 차원을 지우고 4D W를 누락했다. CPU에서는 스펙트럼 변환된 입력을 좌표로 해시할 위험도 있었다.

`cyclesnoise.noisetype=white` 모드를 추가한다. 기존 Noise·Wave의 번호와 계약은 유지한다. CPU·GPU가 같은 Jenkins 비트 해시를 사용하며 연결된 벡터는 raw 평가한다. Color는 색 소켓일 때만 반사·발광 스펙트럼으로 변환하고 벡터 입력일 때는 데이터를 유지한다. 차원별 Value와 Color를 별도로 평가한다. 일반 `whitenoise` 텍스처의 기존 인터페이스는 보존한다.

Blender 5.2.1, 720p, 16샘플의 연결 입력 1~4차원×Value/Color×W 두 값 16조건과 기존 3D 조건 하나를 CPU·Metal에서 렌더했다. 총 17조건의 유한 출력과 CPU 최대 평균 차이 1.11e-7, Metal 3.02e-7 미만을 확인했다. W 변화는 1D·4D에서만 결과를 바꾸며 2D·3D에서는 1e-5 허용치 안에서 영향을 주지 않는다. 모든 조건의 PNG 비교표를 직접 검토했다. 기본 스펙트럼 경로의 추가 검증 결과는 확장의 목표 문서에 기록한다.

검증 엔진은 2.11.13 기반의 현재 소스 빌드이며 정식 2.11.13 배포 바이너리에 새 모드가 있다는 뜻이 아니다. 확장 사용 경로는 새 엔진을 포함한 다음 설치 패키지와 함께 배포해야 한다. 검증 프로필은 `sync_dev_install.sh`로 네이티브 모듈·라이브러리·Python 패키지·캐시 wheel까지 동기화했다. 원본 증거는 상위 작업 폴더의 `test-scenes/validation-2026-10-09/cycles-scene-goal-phase3/`에 보존한다.

# Voronoi 차원·셀·출력 의미

Blender 5.2의 Voronoi는 1D W와 2D XY, 3D XYZ, 4D XYZ+W를 사용한다. 이전 `blender_voronoi` 변환은 1D를 읽지 못하고 특징·출력·연결 입력 일부를 잃었다. `cyclesnoise.noisetype=voronoi`는 기존 엔진 Noise·Wave·White 번호와 GPU 구조를 보존하면서 셀 평가를 추가한다.

CPU와 OpenCL/Metal이 `texture_cyclesvoronoi_funcs.cl`의 같은 스칼라 소스를 사용한다. 1D 셀은 부동소수점 Jenkins, 2~4D 셀은 Blender 5.2 정수 PCG 씨앗을 사용한다. 정수 넘침은 무부호 연산으로, 음수의 오른쪽 이동은 명시적인 부호 확장으로 구현한다. 기존 씬의 셀 패턴과 위치를 유지하기 위한 선택이며 렌더 추정기·스펙트럼·품질을 Cycles로 바꾸는 것은 아니다.

`feature`는 F1·F2·Smooth F1·Distance to Edge·N-Sphere Radius, `metric`은 Euclidean·Manhattan·Chebychev·Minkowski, `output`은 Distance·Color·Position·W·Radius다. Scale·Detail·Roughness·Lacunarity·Smoothness·Exponent·Randomness와 W를 모두 연결 가능한 텍스처로 전달한다. Smoothness·Exponent·Randomness는 기존 공통 구조의 offset·gain·distortion 슬롯을 사용하며 출력·특징·거리 번호는 기존 모드 필드에 보관한다. 속성 왕복은 읽기 쉬운 특징·거리·출력 이름을 유지한다.

Color는 색 입력에서만 스펙트럼으로 변환한다. Position과 W는 좌표 데이터이며 스펙트럼 입력을 좌표로 해시하지 않는다. 1D Position의 XYZ는 0이고 W는 선택한 셀 위치다. Fractal의 소수 Detail, 정규화, 위치 누적과 Scale 역변환을 반영한다. Radius는 Cycles의 셀 단위 계약을 따른다.

Blender 5.2.1 LTS, 1280×720, 16샘플에서 CPU·Metal 각각 41조건을 검사했다. 정규 격자의 네 차원·다섯 특징·위치 출력 24조건, 거리 방식 4조건, 프랙탈·정규화 8조건, 비동률 4D W 두 조건과 실제 UV 셀 패턴 3조건이다. 전부 유한 출력이고 변환 오류가 없으며 최대 평균 절대 차이는 CPU 0.000809 미만, Metal 0.000809 미만이다. 실제 셀 패턴의 Distance·Color·Position 비교 이미지를 직접 검토했다. 셀 형태·경계·색·위치가 유지된다.

거리 동률 셀의 Position/W 선택은 부동소수점 장치 연산에 따라 달라질 수 있다. 초기 W=.23 조건에서 4D F2 셀의 동률을 발견했으며 이를 수치 일치 합격으로 세지 않았다. 최종 W=.41 비동률 조건에서 두 장치의 값 .5를 확인했다. 테스트가 다루지 않은 극단 좌표·Minkowski 0 지수·최대 프랙탈 조합의 유효성을 전체 완료로 세지 않는다.

현재 소스 빌드는 정식 2.11.13 바이너리와 다르다. 확장의 새 매핑은 새 엔진 wheel과 함께 다음 버전으로 배포해야 한다. 원본 EXR·PNG·로그와 `pcg-summary.json`, 직접 검토한 `voronoi-pcg-comparison.png`는 상위 `test-scenes/validation-2026-10-09/cycles-scene-goal-phase4/`에 보존했다. 초기 엔진 자체 해시 결과는 별도 하위 폴더에 남겨 최종 PCG 증거와 구분한다.

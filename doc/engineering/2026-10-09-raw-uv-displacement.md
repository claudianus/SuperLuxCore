# Cycles 원본 UV와 스칼라 변위

Cycles의 Texture Coordinate/UV Map은 음수, 정수 타일과 UV=1 경계를 보존한다. 기존 엔진 `uv` 텍스처는 반복 무늬로 구현되어 정수 부분을 제거했다. 메시 꼭짓점에서 UV=1이 0이 되어, 높이 그래프를 실제 변위에 쓰면 모양과 깊이가 잘못되는 원인이었다.

`scene.textures.<이름>.wrap = false`는 매핑 후 원본 UV를 반환한다. 기존 엔진 씬의 반복 UV 무늬는 기본값 `true`로 유지한다. CPU와 OpenCL/Metal 평가, 컴파일된 텍스처 매개변수와 ToProperties가 같은 플래그를 사용한다. Blender 어댑터는 Cycles UV 출력에 `false`를 지정한다. 이미지 반복·클램프는 이미지 텍스처가 별도로 처리한다.

2.11.14 Release 모듈과 Blender 5.2.1에서 기본 스펙트럴을 유지한 1280×720 스칼라 변위 6조건을 CPU/Metal로 검사했다. BUMP는 표면 법선만 바꾸고, DISPLACEMENT는 메시를 움직이며, BOTH는 원래 법선을 기준으로 범프를 평가한다. 연결한 Midlevel/Scale과 UV 높이의 상수·선형 기울기를 비교한다. CPU 최대 법선 MAE는 0.000066, Metal은 0.000072이고 최대 깊이 MAE는 0.000860이다. 이 수치는 해당 표적 조건의 데이터 패스 비교이며 렌더 품질을 낮추는 일치 기준이 아니다.

UV 원본 보존은 두 노드의 정수 타일·음수·UV=1 경계 총 6조건을 별도 720p RGB 진단으로 검사한다. 결과와 이미지 검토는 작업공간 `test-scenes/validation-2026-10-09/cycles-scene-goal-phase7`에 보관한다.

스칼라 변위의 World 공간·연결 Normal·적응 세분화, BOTH의 변위 전 3D 좌표 복원, 벡터 변위 및 인스턴스별 원본 UV는 아직 별도 작업이다. 이 검증으로 전체 제작 씬 호환을 완료로 표시하지 않는다.

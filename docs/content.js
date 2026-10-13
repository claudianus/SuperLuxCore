/* Hand-written bilingual copy. Render receipts live in showcase.js (generated);
   the feature catalogue, FAQ and coverage notes live in data.js. */
window.SLC_COPY = {
  version: '2.11.27',
  date: '2026-10-12',
  core: 'https://github.com/claudianus/SuperLuxCore',
  addon: 'https://github.com/claudianus/SuperBlendLuxCore',
  // git: fork point e9ced7e7a (LuxCoreRender 2.11.2), compared against upstream main 4209d6537
  fork: {
    base: 'e9ced7e7a', upstreamHead: '4209d6537', engineHead: '08b9b0fd4', addonHead: '6414cb1f',
    engineCommits: 521, upstreamCommits: 51, engineFiles: 618, engineAdded: 137083, engineRemoved: 4799,
    upstreamAdded: 11052, addonCommits: 238, addonAdded: 45739, addonUpstreamCommits: 4, features: 48
  },
  t: {
    title: { en: 'SuperLuxCore — physically based rendering for Blender', ko: 'SuperLuxCore — Blender를 위한 물리 기반 렌더러' },
    description: {
      en: 'An open-source LuxCoreRender fork: spectral optics, zero-config caustics, multiple-scattering materials, diffraction and native Apple Metal — compared side by side with upstream.',
      ko: '오픈소스 LuxCoreRender 포크. 스펙트럴 광학, 설정 없는 커스틱, 다중 산란 재질, 회절, 네이티브 Apple Metal을 업스트림과 나란히 비교합니다.'
    },
    skip: { en: 'Skip to content', ko: '본문으로 건너뛰기' },
    navCompare: { en: 'Compare', ko: '비교' },
    navProof: { en: 'Measured', ko: '측정' },
    navSpeed: { en: 'Speed', ko: '속도' },
    navPasses: { en: 'Passes', ko: '패스' },
    navFeatures: { en: 'Features', ko: '기능' },
    download: { en: 'Download', ko: '다운로드' },

    heroKicker: { en: 'Open source · LuxCoreRender, extended', ko: '오픈소스 · LuxCoreRender 확장판' },
    heroTitle: { en: 'Light, the way\nglass actually bends it.', ko: '유리가 빛을 꺾는\n그대로.' },
    heroBody: {
      en: 'SuperLuxCore is a LuxCoreRender fork for Blender. It traces real wavelengths through measured glass, finds caustics without being asked, keeps rough metal bright and renders natively on your Mac\'s GPU.',
      ko: 'SuperLuxCore는 Blender용 LuxCoreRender 포크입니다. 실측 유리 데이터로 실제 파장을 추적하고, 설정 없이 커스틱을 찾아내며, 거친 금속의 밝기를 지키고, Mac GPU에서 네이티브로 렌더합니다.'
    },
    heroCta: { en: 'Get it for Blender', ko: 'Blender용 받기' },
    heroCompare: { en: 'See the difference', ko: '차이 보기' },
    heroCaption: { en: 'Rendered in SuperLuxCore', ko: 'SuperLuxCore 렌더' },

    statsTitle: { en: 'Not a theme. A different engine.', ko: '테마가 아닙니다. 다른 엔진입니다.' },
    featMore: { en: 'Under the hood', ko: '엔진 속' },
    devOcl: { en: 'OpenCL GPU · M5 Pro 20-core', ko: 'OpenCL GPU · M5 Pro 20코어' },
    statsBody: {
      en: 'Counted with git against the LuxCoreRender 2.11.2 fork point, and against what upstream added over the same period.',
      ko: 'LuxCoreRender 2.11.2 분기 지점을 기준으로 git으로 집계했고, 같은 기간 업스트림이 추가한 양과 나란히 놓았습니다.'
    },
    statCommits: { en: 'engine commits since the fork', ko: '분기 이후 엔진 커밋' },
    statCommitsVs: { en: 'upstream: {n}', ko: '업스트림: {n}' },
    statLines: { en: 'lines of renderer source added', ko: '추가된 렌더러 소스 줄 수' },
    statLinesVs: { en: 'upstream: +{n}', ko: '업스트림: +{n}' },
    statAddon: { en: 'Blender add-on commits', ko: 'Blender 애드온 커밋' },
    statAddonVs: { en: '+{n} lines of Python · upstream: {u} commits', ko: '파이썬 +{n}줄 · 업스트림: {u}커밋' },
    statFeatures: { en: 'documented features', ko: '문서화된 기능' },
    statFeaturesVs: { en: 'each linked to its source doc', ko: '모두 소스 문서로 연결' },

    compareKicker: { en: 'Same scene · same lights · same seconds', ko: '같은 장면 · 같은 조명 · 같은 시간' },
    compareTitle: { en: 'Drag the line.', ko: '선을 끌어 보세요.' },
    compareBody: {
      en: 'Same scene file, same lights, same camera. Left: upstream LuxCoreRender. Right: SuperLuxCore.',
      ko: '같은 장면 파일, 같은 조명, 같은 카메라. 왼쪽은 업스트림 LuxCoreRender, 오른쪽은 SuperLuxCore.'
    },
    hwLabel: { en: 'Test machine', ko: '테스트 장비' },
    devCpu: { en: 'CPU · M5 Pro 18-core', ko: 'CPU · M5 Pro 18코어' },
    devGpu: { en: 'Metal GPU · M5 Pro 20-core', ko: 'Metal GPU · M5 Pro 20코어' },
    devHybrid: { en: 'Metal GPU + CPU · M5 Pro 20 + 18-core', ko: 'Metal GPU + CPU · M5 Pro 20 + 18코어' },
    eqTime: { en: 'Same time: {s} s each', ko: '같은 시간: 각 {s}초' },
    eqSpp: { en: 'Same samples: {n} spp each', ko: '같은 샘플: 각 {n}spp' },
    whatChanged: { en: 'What differs', ko: '무엇이 다른가' },

    speedKicker: { en: 'Same scene · same time', ko: '같은 장면 · 같은 시간' },
    speedTitle: { en: 'How fast, on a Mac and on a PC.', ko: 'Mac과 PC에서 얼마나 빠른가.' },
    sysLabel: { en: 'Test system', ko: '테스트 시스템' },
    sysMac: { en: 'Mac · M5 Pro', ko: 'Mac · M5 Pro' },
    sysPc: { en: 'PC · RTX 5060 Ti (Linux)', ko: 'PC · RTX 5060 Ti (Linux)' },
    speedBody: {
      en: 'Each scene rendered twice for {s} seconds at {w}×{h}, with two different seeds. The bar is how much faster each setup gets to the same noise level as LuxCoreRender on the CPU. Longer is better.',
      ko: '장면마다 {w}×{h}, {s}초씩 시드를 바꿔 두 번 렌더했습니다. 막대는 CPU에서 돌린 LuxCoreRender와 같은 노이즈에 몇 배 빨리 도달하는지입니다. 길수록 좋습니다.'
    },
    benchBronze: { en: 'Bronze still life', ko: '청동 정물' },
    speedUnit: { en: '×N = noise of LuxCoreRender CPU ÷ noise of this setup, squared (noise falls with the square root of render time). Small print: samples per pixel. Samples alone are not a fair score: the engines leave different amounts of noise per sample (sampler, light sampling), and on caustic scenes SuperLuxCore also spends time on light-traced paths that a sample count does not show.', ko: '×N = (LuxCoreRender CPU 노이즈 ÷ 이 설정의 노이즈)². 노이즈는 렌더 시간의 제곱근에 비례해 줄기 때문입니다. 작은 글씨는 픽셀당 샘플 수입니다. 샘플 수만으로는 공정하지 않습니다. 엔진마다 샘플 하나가 남기는 노이즈가 다르고(샘플러, 광원 샘플링), 커스틱 장면에서는 SuperLuxCore가 샘플 수에 잡히지 않는 라이트 트레이싱 경로에도 시간을 쓰기 때문입니다.' },
    speedMetal: {
      en: 'same GPU: both backends compile only the texture, light and camera types a scene uses (OpenCL also its material types), and Metal gets to the same noise 6–37% faster than OpenCL on every scene. GPU only, CPU threads off.',
      ko: '같은 GPU: 두 백엔드 모두 장면이 쓰는 텍스처·조명·카메라 타입만 컴파일하고(OpenCL은 재질 타입도), 모든 장면에서 Metal이 OpenCL보다 같은 노이즈에 6–37% 빨리 도달합니다. GPU만 사용, CPU 스레드 끔.'
    },
    speedOpenCL: {
      en: 'on this MacBook the 18-core CPU is now ahead of the 20-core GPU on four scenes (7–31%) and level with it on roughness, so the CPU is the better default here; the GPU is still a good second device.',
      ko: '이 MacBook에서는 이제 18코어 CPU가 네 장면에서 20코어 GPU보다 7–31% 앞서고 거칠기 장면에서는 비슷합니다. 여기서는 CPU가 더 나은 기본 선택이고, GPU도 충분히 쓸 만한 두 번째 장치입니다.'
    },
    speedHonest: {
      en: 'Against upstream on the same device, SuperLuxCore is ahead on every scene but one: on thin film, upstream\'s OpenCL (1.57×) still beats our Metal backend (1.48×), because its lighter kernels draw about 15% more samples in the same time. Velvet on the GPU is a tie (1.46× vs 1.45×). Since 2026-10-13 SuperLuxCore no longer runs its light-tracing pass on scenes lit only by area lights and the sky, where it found nothing the camera paths miss, and its sampler uses blue-noise Sobol instead of Owen scrambling, which converged more slowly at equal time.',
      ko: '같은 장치끼리 비교하면 SuperLuxCore는 한 장면을 빼고 모두 앞섭니다. 박막 장면에서는 업스트림 OpenCL(1.57배)이 우리 Metal 백엔드(1.48배)보다 아직 빠릅니다. 업스트림 커널이 더 가벼워서 같은 시간에 샘플을 약 15% 더 뽑기 때문입니다. 벨벳은 GPU에서 비깁니다(1.46배 대 1.45배). 2026-10-13부터 SuperLuxCore는 면광원과 하늘로만 밝힌 장면에서 라이트 트레이싱을 돌리지 않습니다. 그런 장면에서는 카메라 경로가 놓치는 빛을 찾지 못했기 때문입니다. 샘플러도 같은 시간에 수렴이 더 느렸던 Owen 스크램블 대신 블루노이즈 Sobol을 씁니다.'
    },
    speedCuda: {
      en: "same RTX 5060 Ti: SuperLuxCore's CUDA backend gets to the same noise 13–19% faster than its OpenCL backend (upstream: 7–14%). On NVIDIA both run the generic kernels; the per-scene type specialization used on the Mac is off there. GPU only, CPU threads off.",
      ko: '같은 RTX 5060 Ti: SuperLuxCore의 CUDA 백엔드가 OpenCL 백엔드보다 같은 노이즈에 13–19% 빨리 도달합니다(업스트림은 7–14%). NVIDIA에서는 두 백엔드 모두 범용 커널을 씁니다. Mac에서 쓰는 장면별 타입 특수화는 여기서는 꺼져 있습니다. GPU만 사용, CPU 스레드 끔.'
    },
    speedPcGpu: {
      en: "on this box the GPU is 3.1–5.3× faster than the CPU for SuperLuxCore. The CPU is a 14-thread share of a cloud server, not a desktop CPU, and the bars are relative to LuxCoreRender on this CPU, so they cannot be compared with the Mac's bars.",
      ko: '이 시스템에서는 SuperLuxCore 기준 GPU가 CPU보다 3.1–5.3배 빠릅니다. CPU는 데스크톱 CPU가 아니라 클라우드 서버의 14스레드 몫이고, 막대는 이 CPU에서 돌린 LuxCoreRender 기준이라 Mac 막대와 직접 비교할 수 없습니다.'
    },
    speedPcHonest: {
      en: "On this GPU SuperLuxCore's CUDA backend is ahead of upstream's on three of five scenes: bronze (1.16×), roughness (1.38×) and subsurface scattering (1.81×). Upstream is still faster on thin film (1.44×) and velvet (1.31×): its lighter kernels draw 1.4–1.5× more samples in the same time on every scene, and on those two materials SuperLuxCore's samples are not cleaner enough to make up for it. On this CPU SuperLuxCore is ahead on four scenes (1.2–2.5×) and behind on thin film (0.79×).",
      ko: '이 GPU에서는 SuperLuxCore CUDA 백엔드가 다섯 장면 중 세 장면에서 업스트림 CUDA보다 빠릅니다. 청동 1.16배, 거칠기 1.38배, SSS 1.81배입니다. 박막(1.44배)과 벨벳(1.31배)은 아직 업스트림이 빠릅니다. 업스트림 커널이 더 가벼워 모든 장면에서 같은 시간에 샘플을 1.4–1.5배 더 뽑는데, 이 두 재질에서는 SuperLuxCore의 샘플이 그 차이를 메울 만큼 깨끗하지 않기 때문입니다. 이 CPU에서는 SuperLuxCore가 네 장면에서 앞서고(1.2–2.5배) 박막에서는 뒤집니다(0.79배).'
    },
    speedPcNote: {
      en: 'Test box: a rented vast.ai cloud instance running Ubuntu 24.04 (Linux), not Windows. Same five scenes, resolution, 30 seconds and two seeds as on the Mac; upstream is the LuxCoreRender 2.11.2 Linux wheel. Kernels were compiled and cached before timing (the first SuperLuxCore compile on NVIDIA takes about 30 minutes per backend).',
      ko: '테스트 시스템: Windows가 아니라 Ubuntu 24.04(Linux)를 쓰는 vast.ai 임대 클라우드 인스턴스입니다. Mac과 같은 다섯 장면·해상도·30초·두 시드로 쟀고, 업스트림은 LuxCoreRender 2.11.2 Linux 휠입니다. 커널은 측정 전에 컴파일해 캐시해 두었습니다(NVIDIA에서 SuperLuxCore 첫 컴파일은 백엔드마다 약 30분).'
    },
    speedEst: {
      en: '≈ Upstream OpenCL: Apple\'s driver crashed on every launch for the second seed, so its noise is estimated from one render against the upstream CPU pair.',
      ko: '≈ 업스트림 OpenCL: 두 번째 시드에서 Apple 드라이버가 매번 크래시해, 렌더 한 장을 업스트림 CPU 두 장과 비교해 노이즈를 추정했습니다.'
    },
    left: { en: 'LuxCoreRender 2.11.2', ko: 'LuxCoreRender 2.11.2' },
    right: { en: 'SuperLuxCore 2.11.27', ko: 'SuperLuxCore 2.11.27' },
    drag: { en: 'Drag or use ← →', ko: '드래그 또는 ← →' },
    receipt: { en: 'Render receipt', ko: '렌더 영수증' },
    differs: { en: 'Only these lines differ', ko: '다른 줄은 이것뿐' },
    samples: { en: 'samples / pixel', ko: '픽셀당 샘플' },
    seconds: { en: 'seconds', ko: '초' },
    denoised: { en: 'OIDN denoised', ko: 'OIDN 디노이즈' },
    raw: { en: 'raw, no denoiser', ko: '원본, 디노이저 없음' },

    proofKicker: { en: 'Measured, not claimed', ko: '주장이 아니라 측정' },
    proofTitle: { en: 'Numbers you can rerun.', ko: '다시 돌려 볼 수 있는 숫자.' },
    proofBody: {
      en: 'A white furnace is the oldest honesty test in rendering: a white object under uniform white light should vanish. Whatever it returns below 1.0 is light the renderer lost.',
      ko: '화이트 퍼니스는 렌더링에서 가장 오래된 정직성 테스트입니다. 균일한 흰 빛 속의 흰 물체는 사라져 보여야 합니다. 1.0보다 모자란 만큼이 렌더러가 잃어버린 빛입니다.'
    },
    furnaceTitle: { en: 'White metal, white furnace', ko: '흰 금속, 화이트 퍼니스' },
    furnaceNote: { en: 'Ideal = 1.00. Center of a sphere, 256 spp, depth 64. Metal GPU agrees with CPU within 1%.', ko: '이상값 = 1.00. 구 중앙, 256spp, 깊이 64. Metal GPU와 CPU 차이 1% 이내.' },
    roughness: { en: 'roughness', ko: '거칠기' },
    sellTitle: { en: 'Measured glass, color-neutral', ko: '실측 유리, 색 중립' },
    sellBody: {
      en: 'Building this page found a bug: Sellmeier coefficients were being color-upsampled in spectral mode, tinting dispersive glass green. Fixed on CPU and Metal; the furnace now reads neutral.',
      ko: '이 페이지를 만들다 버그를 찾았습니다. 스펙트럴 모드에서 Sellmeier 계수가 색처럼 업샘플링되어 분산 유리가 초록빛으로 물들었습니다. CPU와 Metal 모두 고쳤고 퍼니스 결과는 이제 중립입니다.'
    },
    before: { en: 'before', ko: '수정 전' },
    after: { en: 'after', ko: '수정 후' },

    passKicker: { en: 'Production passes', ko: '제작용 패스' },
    passTitle: { en: 'One render. Everything the compositor needs.', ko: '렌더 한 번으로 합성에 필요한 모든 것.' },
    passBody: {
      en: 'Albedo, shading normals, object and material IDs and depth come out of the same Metal render as the beauty, ready for denoising, relighting and masks. Cryptomatte and light path expressions are there when you need more.',
      ko: '알베도, 셰이딩 노멀, 오브젝트·재질 ID, 깊이가 뷰티와 같은 Metal 렌더에서 함께 나옵니다. 디노이즈, 리라이팅, 마스크에 바로 쓰세요. 더 필요하면 Cryptomatte와 라이트 패스 표현식도 있습니다.'
    },
    passBeauty: { en: 'Beauty', ko: '뷰티' },
    passAlbedo: { en: 'Albedo', ko: '알베도' },
    passNormal: { en: 'Normal', ko: '노멀' },
    passObject: { en: 'Object ID', ko: '오브젝트 ID' },
    passMaterial: { en: 'Material ID', ko: '재질 ID' },
    passDepth: { en: 'Depth', ko: '깊이' },

    areasKicker: { en: 'Where the work went', ko: '작업이 들어간 곳' },
    areasTitle: { en: 'Five hundred commits, by area.', ko: '500여 개 커밋, 영역별로.' },
    areasBody: {
      en: 'Every engine commit from the fork point through {sha} was sorted by the paths it touched. Areas overlap; fixes and tooling are counted.',
      ko: '분기 지점부터 {sha}까지의 모든 엔진 커밋을 변경 경로로 분류했습니다. 영역은 겹칠 수 있으며 수정과 도구 커밋도 포함합니다.'
    },
    commits: { en: 'commits', ko: '커밋' },

    featKicker: { en: 'The toolkit', ko: '툴킷' },
    featTitle: { en: 'Forty-eight things upstream doesn\'t do.', ko: '업스트림에 없는 48가지.' },
    featBody: { en: 'Search the catalogue. Each card links to the document with its controls, backends and limits.', ko: '카탈로그를 검색하세요. 각 카드는 설정, 지원 백엔드, 한계가 적힌 문서로 연결됩니다.' },
    search: { en: 'Search features', ko: '기능 검색' },
    noResults: { en: 'Nothing matches that search.', ko: '검색 결과가 없습니다.' },
    docs: { en: 'Read the doc', ko: '문서 보기' },

    dlKicker: { en: 'Free and open source', ko: '무료 · 오픈소스' },
    dlTitle: { en: 'Install it in Blender 5.2.', ko: 'Blender 5.2에 설치하세요.' },
    dlBody: { en: 'One ZIP per platform. Drag it into Blender, pick SuperLuxCore as the render engine, render.', ko: '플랫폼마다 ZIP 하나. Blender에 끌어다 놓고 렌더 엔진을 SuperLuxCore로 고른 뒤 렌더하세요.' },
    dlZip: { en: 'Extension ZIP', ko: '확장 ZIP' },
    manual: { en: 'User manual', ko: '사용 설명서' },
    release: { en: 'Release notes', ko: '릴리스 노트' },
    faq: { en: 'Questions', ko: '자주 묻는 질문' },
    coverage: { en: 'What is and isn\'t covered', ko: '지원 범위와 한계' },

    honestyTitle: { en: 'How these images were made', ko: '이 이미지들을 만든 방법' },
    honesty: {
      en: 'Scenes are built from CC0 Poly Haven assets plus procedural optics by a script in the repository, then rendered by the published Python modules of each engine (LuxCoreRender 2.11.2 wheel, SuperLuxCore 2.11.27) on one Apple M5 Pro, displayed through OpenColorIO ACES 2.0. Each image has a JSON receipt with its sample count, seconds, settings and hashes. The renderer source is Apache-2.0; the Blender extension is GPL-3.0.',
      ko: '장면은 저장소의 스크립트가 CC0 Poly Haven 에셋과 절차적 광학 오브젝트로 만들고, 각 엔진의 공개 파이썬 모듈(LuxCoreRender 2.11.2 wheel, SuperLuxCore 2.11.27)이 Apple M5 Pro 한 대에서 렌더했으며, OpenColorIO ACES 2.0으로 표시했습니다. 이미지마다 샘플 수, 시간, 설정, 해시가 담긴 JSON 영수증이 있습니다. 렌더러 소스는 Apache-2.0, Blender 확장은 GPL-3.0입니다.'
    },
    receipts: { en: 'All render receipts', ko: '전체 렌더 영수증' },
    credits: { en: 'Built on LuxCoreRender. Assets: Poly Haven (CC0).', ko: 'LuxCoreRender 기반. 에셋: Poly Haven (CC0).' }
  },
  // One short line per comparison (receipts come from showcase.js); 'fair' sits behind a disclosure
  stories: {
    spectrum: {
      tab: { en: 'Dispersion & caustics', ko: '분산 · 커스틱' },
      title: { en: 'Fire in the stone, light on the floor.', ko: '보석 속 불꽃, 바닥 위의 빛.' },
      cap: { en: 'Real wavelengths through measured glass. Caustics found automatically.', ko: '실측 유리를 지나는 실제 파장. 커스틱은 자동으로.' },
      fair: { en: 'Upstream has no Sellmeier glass, so it gets Cauchy dispersion fitted to the same glasses and path-traces the caustics. Nothing was enabled by hand on either side: SuperLuxCore switches its caustic tools on by itself when a scene has glass or metal.', ko: '업스트림에는 Sellmeier 유리가 없어 같은 유리에 맞춘 Cauchy 분산을 줬고, 커스틱은 패스 트레이싱으로 그립니다. 양쪽 모두 손으로 켠 것은 없습니다. SuperLuxCore는 장면에 유리나 금속이 있으면 커스틱 도구를 스스로 켭니다.' }
    },
    pool: {
      tab: { en: 'Pool caustics', ko: '수영장 커스틱' },
      title: { en: 'Sunlight dancing on the pool floor.', ko: '수영장 바닥에서 춤추는 햇빛.' },
      cap: { en: 'Seen through moving water, lit through moving water. No settings touched.', ko: '물결 너머로 보이고, 물결을 지나 비친 바닥. 설정은 그대로.' },
      fair: { en: 'Identical scene file and render settings. The camera sees the floor through the water, and the sun lights it through the water (an S-D-S path). Upstream\'s path tracer can\'t aim at the sun through the surface and its light tracer can\'t reach the camera through it, so upstream needs its PhotonGI caustic cache set up by hand; in our test (60 s, 2 M photons, 0.15 m radius) it reached 2 samples per pixel and still showed no pattern. SuperLuxCore turns on light tracing, manifold next-event estimation and path regularization automatically.', ko: '장면 파일과 렌더 설정이 완전히 같습니다. 카메라는 물 너머로 바닥을 보고, 해는 물을 통과해 바닥을 비춥니다(S-D-S 경로). 업스트림 패스 트레이서는 수면 너머의 해를 겨냥하지 못하고, 라이트 트레이서는 수면 너머의 카메라에 닿지 못합니다. 그래서 업스트림은 PhotonGI 커스틱 캐시를 손으로 설정해야 하는데, 우리 테스트(60초, 광자 200만, 반경 0.15m)에서는 픽셀당 2샘플에 그쳤고 무늬도 나오지 않았습니다. SuperLuxCore는 라이트 트레이싱, MNEE, 경로 정규화를 자동으로 켭니다.' }
    },
    thinfilm: {
      tab: { en: 'Thin film', ko: '박막 간섭' },
      title: { en: 'Anodized titanium and an oil slick.', ko: '양극산화 티타늄과 기름 막.' },
      cap: { en: 'Both engines have a thin film. Ours computes it wavelength by wavelength, and can put it on water, not only on metal.', ko: '두 엔진 모두 박막이 있습니다. 우리는 파장마다 계산하고, 금속뿐 아니라 물 위에도 올릴 수 있습니다.' },
      fair: { en: 'Same film thickness map (80–480 nm), same film IOR, same lights on both sides. Upstream: Disney filmamount / filmthickness / filmior, which multiplies one precomputed film colour onto the reflection. Its film only shows on a reflective lobe, so its oil has to sit on a dark metal and reads as tinted chrome. SuperLuxCore: OpenPBR thin film, Airy interference per wavelength over the real substrate: the metal\'s complex IOR for the titanium, water (n 1.33) under the oil, which reflects 4–8% and shows soft bands like a real slick. Rendered spectrally.', ko: '양쪽 모두 같은 막 두께 맵(80–480nm), 같은 막 굴절률, 같은 조명입니다. 업스트림은 Disney filmamount / filmthickness / filmior로, 미리 계산한 막 색 하나를 반사에 곱합니다. 반사 로브에서만 막이 보이기 때문에 기름도 어두운 금속 위에 올려야 하고, 그래서 색을 입힌 크롬처럼 보입니다. SuperLuxCore는 OpenPBR 박막으로, 실제 바탕 위에서 파장마다 Airy 간섭을 풉니다. 티타늄은 금속의 복소 굴절률, 기름은 물(n 1.33) 위라서 4–8%만 반사하고 실제 기름 막처럼 은은한 띠를 보입니다. 스펙트럴 렌더.' }
    },
    hologram: {
      tab: { en: 'Diffraction', ko: '회절' },
      title: { en: 'Rainbows from grooves, not a texture.', ko: '텍스처가 아니라 홈에서 생기는 무지개.' },
      cap: { en: 'CD grooves split light into rainbows that fan out from the centre.', ko: 'CD 홈이 빛을 갈라 중심에서 퍼져 나가는 무지개를 만듭니다.' },
      fair: { en: 'Upstream has no diffraction material, so its discs are the same polished silver. SuperLuxCore\'s discs have circular grooves 1.6 µm apart, the track pitch of a real CD; the colours are light diffracting off them, rendered spectrally.', ko: '업스트림에는 회절 재질이 없어 디스크가 같은 연마 은입니다. SuperLuxCore 디스크에는 실제 CD 트랙 간격과 같은 1.6µm 간격의 원형 홈이 있고, 색은 그 홈에서 회절한 빛을 스펙트럴로 렌더한 결과입니다.' }
    },
    fogbeam: {
      tab: { en: 'Light in fog', ko: '안개 속 빛' },
      title: { en: 'A glass ball focusing a spotlight in fog.', ko: '안개 속에서 스포트라이트를 모으는 유리구.' },
      cap: { en: 'The bright cone behind the ball is a caustic inside the fog.', ko: '유리구 뒤의 밝은 원뿔은 안개 속에 맺힌 커스틱입니다.' },
      fair: { en: "Identical scene file and render settings. Upstream's path tracer cannot connect a point in the fog to a light seen through glass, so the focused cone is missing; SuperLuxCore's automatic caustic tools reach it.", ko: '장면 파일과 렌더 설정이 완전히 같습니다. 업스트림 패스 트레이서는 안개 속 한 점을 유리 너머 조명과 잇지 못해 모인 원뿔이 빠지고, SuperLuxCore의 자동 커스틱 도구는 닿습니다.' }
    },
    roughness: {
      tab: { en: 'Rough metal', ko: '거친 금속' },
      title: { en: "Rough metal that doesn't go dark.", ko: '거칠어져도 어두워지지 않는 금속.' },
      cap: { en: 'Roughness 0.05 → 1.0. Rough gold stays gold instead of turning grey.', ko: '거칠기 0.05 → 1.0. 거친 금이 회색으로 죽지 않고 금색을 지킵니다.' },
      fair: { en: 'Same Disney metal on both sides; the fork adds one line, multibounce = 1 (multiple-scattering GGX). Single-scattering microfacets lose the light that bounces between facets more than once, up to 72% at roughness 1 (see the furnace test below).', ko: '양쪽 모두 같은 Disney 금속이고, 포크 쪽에 multibounce = 1(다중 산란 GGX) 한 줄만 더했습니다. 단일 산란 마이크로패싯은 면 사이에서 여러 번 튕기는 빛을 잃는데, 거칠기 1에서 최대 72%입니다(아래 퍼니스 테스트).' }
    },
    velvet: {
      tab: { en: 'Velvet sheen', ko: '벨벳 광택' },
      title: { en: 'Velvet that glows at the edges.', ko: '가장자리가 빛나는 벨벳.' },
      cap: { en: 'The soft rim light of real fabric.', ko: '실제 천의 부드러운 테두리 빛.' },
      fair: { en: 'Same Disney material with sheen = 1 on both sides; the fork adds sheenroughness = 0.35, which replaces Disney\'s sheen with Charlie microfacet sheen.', ko: '양쪽 모두 sheen = 1인 같은 Disney 재질이고, 포크 쪽에 sheenroughness = 0.35를 더해 Disney 쉰을 Charlie 마이크로패싯 쉰으로 바꿨습니다.' }
    },
    sss: {
      tab: { en: 'Subsurface', ko: '서브서피스' },
      title: { en: 'Light that travels inside.', ko: '안으로 들어가는 빛.' },
      cap: { en: 'Backlit legs, ears and manes glow, as real marble, skin and fruit do.', ko: '역광을 받은 다리, 귀, 갈기가 실제 대리석, 피부, 과일처럼 빛납니다.' },
      fair: { en: 'Same base colours, same lights. Upstream\'s only subsurface control on a surface is Disney subsurface, which reshapes the diffuse lobe: no light ever enters the object. SuperLuxCore uses OpenPBR subsurface with measured presets (marble, light skin, apple) and walks the light through the inside. That costs more per sample, so the fork side ran on the Metal GPU; 120 seconds each.', ko: '같은 기본 색, 같은 조명. 업스트림이 표면에 쓸 수 있는 서브서피스는 Disney subsurface뿐인데, 이것은 디퓨즈 모양만 바꿀 뿐 빛이 물체 안으로 들어가지 않습니다. SuperLuxCore는 실측 프리셋(대리석, 밝은 피부, 사과)을 쓴 OpenPBR 서브서피스로 빛을 물체 안에서 이동시킵니다. 샘플당 비용이 커서 포크 쪽은 Metal GPU로 렌더했고, 각 120초입니다.' }
    }
  },
  // White-furnace measurements (preview/fm-*.npz, 2026-10-12)
  furnace: [
    { r: 0.5, up: 0.838, slc: 0.935, gpu: 0.933 },
    { r: 1.0, up: 0.278, slc: 0.775, gpu: 0.768 }
  ],
  // Sellmeier furnace, N-SF6 orb, spectral CPU (before / after the 2026-10-12 fix)
  sellmeier: { before: [1.071, 1.05, 0.799], after: [1.0, 0.998, 1.001] }
};

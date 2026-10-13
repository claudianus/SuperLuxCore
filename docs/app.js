/* SuperLuxCore showcase: bilingual, no framework, no tracking. */
(() => {
  'use strict';
  const C = window.SLC_COPY, D = window.SLC_DATA, S = window.SLC_SHOW, $ = id => document.getElementById(id);
  const esc = v => String(v).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const KEY = 'superluxcore-language', valid = v => v === 'en' || v === 'ko';
  const saved = () => { try { return localStorage.getItem(KEY); } catch { return null; } };
  const detect = () => /^ko(?:-|$)/i.test((navigator.languages || [navigator.language])[0] || '') ? 'ko' : 'en';
  const urlLang = new URL(location.href).searchParams.get('lang');
  let pref = valid(urlLang) ? urlLang : valid(saved()) ? saved() : 'auto';
  let lang = pref === 'auto' ? detect() : pref;
  let pass = 0, category = 'all', query = '', sys = 'mac';

  const tr = v => (v == null ? '' : typeof v === 'string' ? v : v[lang]);
  const T = (k, vars) => { let s = tr(C.t[k]); for (const [a, b] of Object.entries(vars || {})) s = s.replace('{' + a + '}', b); return s; };
  const t = (k, vars) => esc(T(k, vars));
  const fmt = n => n.toLocaleString(lang === 'ko' ? 'ko-KR' : 'en-US');
  const short = n => n >= 1000 ? (lang === 'ko' ? (n / 10000).toFixed(1).replace(/\.0$/, '') + '만' : Math.round(n / 1000) + 'k') : String(n);
  const lines = s => esc(s).replace(/\n/g, '<br>');

  function head(kicker, title, body, id) {
    return `<div class="head"><div><p class="kicker">${t(kicker)}</p><h2${id ? ` id="${id}"` : ''}>${lines(T(title))}</h2></div><p>${body}</p></div>`;
  }

  function render() {
    document.documentElement.lang = lang;
    document.title = T('title');
    document.querySelector('meta[name="description"]').content = T('description');
    $('skip').textContent = T('skip');
    $('lang').value = pref;
    $('nav').innerHTML = [['compare', 'navCompare'], ['speed', 'navSpeed'], ['passes', 'navPasses'], ['features', 'navFeatures']]
      .map(([id, k]) => `<a href="#${id}">${t(k)}</a>`).join('') + `<a class="nav-cta" href="#download">${t('download')}</a>`;
    const F = C.fork, H = S.hero;
    $('main').innerHTML = `
      <section class="hero" id="top" aria-labelledby="hero-title">
        <img class="hero-img" src="${H.src}" width="${H.w}" height="${H.h}" alt="${esc(tr(H.alt))}" fetchpriority="high">
        <div class="hero-copy wrap">
          <p class="kicker">${t('heroKicker')}</p>
          <h1 id="hero-title">${lines(T('heroTitle'))}</h1>
          <p>${t('heroBody')}</p>
          <div class="actions"><a class="btn" href="#download">${t('heroCta')} ↓</a><a class="btn ghost" href="#compare">${t('heroCompare')} →</a></div>
        </div>
        <p class="hero-caption">${t('heroCaption')} ${esc(C.renderVersion)} · ${esc(H.backend)} ${t('heroSpectral')} · ${fmt(H.spp)} spp · ${t('minutes', { n: Math.round(H.seconds / 60) })} · ACES 2.0</p>
      </section>
      <div class="hero-rule" aria-hidden="true"></div>

      <section class="block wrap" aria-labelledby="stats-title">
        ${head('heroKicker', 'statsTitle', t('statsBody'), 'stats-title')}
        <div class="stats">
          ${stat(fmt(F.engineCommits), 'statCommits', T('statCommitsVs', { n: F.upstreamCommits }), F.engineCommits, F.upstreamCommits)}
          ${stat('+' + short(F.engineAdded), 'statLines', T('statLinesVs', { n: short(F.upstreamAdded) }), F.engineAdded, F.upstreamAdded)}
          ${stat(fmt(F.addonCommits), 'statAddon', T('statAddonVs', { n: short(F.addonAdded), u: F.addonUpstreamCommits }), F.addonCommits, F.addonUpstreamCommits)}
          ${stat(String(F.features), 'statFeatures', T('statFeaturesVs'))}
        </div>
      </section>

      <section class="block compare-block" id="compare" aria-labelledby="compare-title">
        <div class="wrap">
          ${head('compareKicker', 'compareTitle', t('compareBody'), 'compare-title')}
          ${hwChips()}
          <div class="jump" role="list">${S.comparisons.map(c => `<a role="listitem" href="#c-${c.id}">${esc(tr(C.stories[c.id].tab))}</a>`).join('')}</div>
          <div class="gallery">${S.comparisons.map(card).join('')}</div>
        </div>
      </section>

      <section class="block wrap" id="speed" aria-labelledby="speed-title">
        ${head('speedKicker', 'speedTitle', t('speedBody', { s: S.bench[0].seconds, w: S.bench[0].res[0], h: S.bench[0].res[1] }), 'speed-title')}
        <div class="chips sys" role="group" aria-label="${t('sysLabel')}">${['mac', 'pc'].map(k => `<button data-sys="${k}" aria-pressed="${k === sys}">${t(k === 'mac' ? 'sysMac' : 'sysPc')}</button>`).join('')}</div>
        <div id="speed-body">${speed()}</div>
      </section>

      <section class="block wrap" id="measured" aria-labelledby="measured-title">
        ${head('proofKicker', 'proofTitle', t('proofBody'), 'measured-title')}
        <div class="proof-grid two">${furnaceCard()}${sellmeierCard()}</div>
      </section>

      <section class="block wrap" id="passes" aria-labelledby="passes-title"><div class="pass-wrap">
        <div><p class="kicker">${t('passKicker')}</p><h2 id="passes-title">${t('passTitle')}</h2><p class="lead">${t('passBody')}</p>
          <div class="pass-list" role="group" aria-label="${t('passKicker')}">${S.passes.map((p, i) => `<button data-pass="${i}" aria-pressed="${i === pass}">${t(p.label)}<span>${esc(p.kind)}</span></button>`).join('')}</div></div>
        <div><div class="pass-view"><img id="pass-img" src="${S.passes[pass].src}" width="1600" height="1000" alt="${esc(T(S.passes[pass].label))}" loading="lazy"></div>
          <div class="thumbs">${S.passes.map((p, i) => `<img src="${p.thumb}" width="320" height="200" alt="" data-thumb="${i}" aria-current="${i === pass}" loading="lazy">`).join('')}</div></div>
      </div></section>

      <section class="block wrap" aria-labelledby="areas-title">
        ${head('areasKicker', 'areasTitle', t('areasBody', { sha: D.audit.through }), 'areas-title')}
        <div class="areas">${areas()}</div>
      </section>

      <section class="block wrap" id="features" aria-labelledby="features-title">
        ${head('featKicker', 'featTitle', t('featBody'), 'features-title')}
        <div class="feat-tools"><label class="search"><span aria-hidden="true">⌕</span><span class="sr">${t('search')}</span><input id="q" type="search" placeholder="${t('search')}" value="${esc(query)}" autocomplete="off"></label>
          <div class="chips" role="group" aria-label="${t('navFeatures')}">${Object.entries(D.categories).map(([id, l]) => `<button data-cat="${id}" aria-pressed="${id === category}">${esc(tr(l))}</button>`).join('')}</div></div>
        <p class="count" id="count" aria-live="polite"></p><div class="feats" id="feats"></div>
      </section>

      <section class="block dl" id="download" aria-labelledby="dl-title"><div class="wrap">
        ${head('dlKicker', 'dlTitle', t('dlBody'), 'dl-title')}
        <div class="dl-grid">${[['macos_arm64', 'macOS', 'Apple silicon · Metal'], ['macos_x64', 'macOS', 'Intel'], ['windows_x64', 'Windows', 'x64 · CUDA / OpenCL'], ['linux_x64', 'Linux', 'x64 · CUDA / OpenCL']]
          .map(([id, name, arch]) => `<a href="${C.addon}/releases/download/v${C.version}/SuperLuxCore-${C.version}-${id}.zip"><strong>${name}</strong><small>${arch}</small><em>${t('dlZip')} ↓</em></a>`).join('')}</div>
        <div class="dl-links"><span>${esc(C.version)} · Blender 5.2</span><a href="${C.addon}/releases/tag/v${C.version}">${t('release')} ↗</a><a href="${C.core}/tree/main/doc/features">${t('docs')} ↗</a></div>
        <div class="faq"><h3>${t('faq')}</h3>${D.faq.map(([q, a]) => `<details><summary>${esc(tr(q))}</summary><p>${esc(tr(a))}</p></details>`).join('')}
          <details><summary>${t('coverage')}</summary>${D.status.map(([a, b]) => `<p><b>${esc(tr(a))}.</b> ${esc(tr(b))}</p>`).join('')}</details></div>
      </div></section>`;
    $('footer').innerHTML = `<div class="wrap foot"><div><h3>${t('honestyTitle')}</h3><p>${t('honesty')}</p></div>
      <div class="links"><a href="evidence/showcase-2026-10-12.json">${t('receipts')} ↗</a><a href="${C.core}">SuperLuxCore · GitHub ↗</a><a href="${C.addon}">SuperBlendLuxCore · GitHub ↗</a><a href="https://polyhaven.com">Poly Haven ↗</a><span>${t('credits')}</span><span>© 2026 · Apache-2.0 · GPL-3.0</span></div></div>`;
    bindCompare(); renderFeatures(); bind();
  }

  function stat(big, label, small, a, b) {
    const r = a && b ? `<div class="ratio" aria-hidden="true"><i style="width:${a / (a + b) * 100}%"></i><i style="width:${b / (a + b) * 100}%"></i></div>` : '';
    return `<div class="stat"><strong>${esc(big)}</strong><span>${t(label)}</span><small>${esc(small)}</small>${r}</div>`;
  }

  function furnaceCard() {
    const row = (name, v, cls) => `<div class="bar-row ${cls}"><em>${esc(name)}</em><div class="track"><i style="width:${v * 100}%"></i><span class="ideal"></span></div><b>${v.toFixed(2)}</b></div>`;
    return `<div class="card"><h3>${t('furnaceTitle')}</h3><div class="bars">${C.furnace.map(f => `<div class="grp"><span>${t('roughness')} ${f.r.toFixed(1)}</span>${row('LuxCoreRender', f.up, '')}${row('SuperLuxCore', f.slc, 'slc')}${row('… Metal GPU', f.gpu, 'slc')}</div>`).join('')}</div><p class="note">${t('furnaceNote')}</p></div>`;
  }
  function sellmeierCard() {
    const sw = (rgb, label) => {
      const m = Math.max(...rgb), c = rgb.map(v => Math.round(Math.min(1, v / m * .82) ** (1 / 2.2) * 255));
      return `<div class="swatch"><i style="background:rgb(${c.join(',')})"></i><span>${esc(label)}<br>${rgb.map(v => v.toFixed(3)).join(' ')}</span></div>`;
    };
    return `<div class="card"><h3>${t('sellTitle')}</h3><p class="note" style="margin-top:12px;color:var(--ink2)">${t('sellBody')}</p><div class="swatches">${sw(C.sellmeier.before, T('before'))}${sw(C.sellmeier.after, T('after'))}</div><p class="note">${t('sellNote')}</p></div>`;
  }
  function areas() {
    const max = Math.max(...D.areas.map(a => a.count));
    return D.areas.slice().sort((a, b) => b.count - a.count).map(a => `<div class="area"><div class="n">${a.count}<small>${t('commits')}</small></div><div class="meter"><i style="width:${a.count / max * 100}%"></i></div><h3>${esc(tr(a.title))}</h3><p>${esc(tr(a.outcome))}</p><span class="id">${esc(tr(C.areaNames[a.id]) || a.id)}</span></div>`).join('');
  }

  const devName = d => T({ gpu: 'devGpu', hybrid: 'devHybrid', oclgpu: 'devOcl' }[d] || 'devCpu');
  function hwChips(H = S.hw) {
    return `<ul class="hw" aria-label="${t('hwLabel')}"><li><b>CPU</b>${esc(H.cpu)}</li><li><b>GPU</b>${esc(H.gpu)}</li><li><b>RAM</b>${esc(H.ram)}</li><li><b>OS</b>${esc(H.os)}</li></ul>`;
  }
  function card(c) {
    const s = C.stories[c.id], wide = c.w / c.h > 2;
    const diff = [...c.up.lines.map(l => `<span class="del">- ${esc(l)}</span>`), ...c.slc.lines.map(l => `<span class="add">+ ${esc(l)}</span>`)].join('\n');
    const same = c.equal === 'spp' ? T('eqSpp', { n: fmt(c.up.spp) }) : T('eqTime', { s: Math.round(c.up.seconds) });
    const sideTag = (r, name, cls) => `<span class="tag ${cls}">${esc(name)}<small>${esc(devName(r.device))} · ${fmt(r.spp)} spp</small></span>`;
    return `<article class="cc${wide ? ' wide' : ''}" id="c-${c.id}" aria-labelledby="h-${c.id}">
      <div class="cmp" data-cmp style="--split:50%;--ar:${c.w} / ${c.h}">
        <img src="${c.slc.src}" width="${c.w}" height="${c.h}" alt="${esc(T('right') + ' — ' + tr(s.title))}" loading="lazy" decoding="async">
        <img class="before" src="${c.up.src}" width="${c.w}" height="${c.h}" alt="${esc(T('left') + ' — ' + tr(s.title))}" loading="lazy" decoding="async">
        <div class="handle" aria-hidden="true"></div>
        ${sideTag(c.up, 'LuxCoreRender', 'l')}${sideTag(c.slc, 'SuperLuxCore', 'r')}
        <input type="range" min="0" max="100" step="1" value="50" aria-label="${esc(tr(s.tab))} — ${t('drag')}">
      </div>
      <div class="cc-cap">
        <h3 id="h-${c.id}">${esc(tr(s.title))}</h3>
        <p>${esc(tr(s.cap))}</p>
        <ul class="meta-chips"><li>${esc(same)}</li><li>${esc(c.up.device === c.slc.device ? devName(c.up.device) : devName(c.up.device) + ' vs ' + devName(c.slc.device))}</li><li>${t(c.slc.denoise ? 'denoised' : 'raw')}</li></ul>
        <details><summary>${t('whatChanged')}</summary><p class="fair">${esc(tr(s.fair))}</p><pre aria-label="${t('differs')}">${diff || '—'}</pre><a href="${c.receipt}">${t('receipt')} JSON ↗</a></details>
      </div>
    </article>`;
  }
  // speed-to-equal-noise legs per test system: [label, key, bar class]; GPU header = SuperLuxCore best backend vs upstream
  const SYS = {
    mac: { hw: () => S.hw, bench: () => S.bench, gpu: [['LuxCoreRender OpenCL', 'openclUp', ''], ['SuperLuxCore OpenCL', 'opencl', 'mid'], ['SuperLuxCore Metal', 'metal', 'slc']],
      x: ['metal', 'openclUp'], notes: [['Metal vs OpenCL', 'speedMetal'], ['GPU vs CPU', 'speedOpenCL'], [null, 'speedHonest']] },
    pc: { hw: () => S.hwPc, bench: () => S.benchPc, gpu: [['LuxCoreRender OpenCL', 'openclUp', ''], ['LuxCoreRender CUDA', 'cudaUp', ''], ['SuperLuxCore OpenCL', 'opencl', 'mid'], ['SuperLuxCore CUDA', 'cuda', 'slc']],
      x: ['cuda', 'cudaUp'], notes: [['CUDA vs OpenCL', 'speedCuda'], ['GPU vs CPU', 'speedPcGpu'], [null, 'speedPcHonest'], [null, 'speedPcNote']] }
  };
  function speed() {
    // bar = speed to the same noise (two seeds), relative to LuxCoreRender on the CPU; samples as a footnote
    const Y = SYS[sys], B = Y.bench();
    const rows = B.map(b => {
      const v = b.speed, max = Math.max(...Object.values(v));
      // SuperLuxCore vs LuxCoreRender on the same device; below 1 = upstream is faster
      const x = (a, c) => { const r = v[a] / v[c]; return `<b${r < 1 ? ' class="lt"' : ''}>${r.toFixed(2)}×</b>`; };
      const est = k => (b.est || []).includes(k) ? '≈' : '';
      const row = (name, k, cls) => `<div class="bar-row ${cls}"><em>${esc(name)}</em><div class="track"><i style="width:${v[k] / max * 100}%"></i></div><b>${est(k)}${v[k].toFixed(2)}×<small>${fmt(b.spp[k])} spp</small></b></div>`;
      return `<div class="sp"><img src="${b.thumb}" width="240" height="150" alt="" loading="lazy"><div><h3>${esc(b.id === 'bronze' ? T('benchBronze') : tr(C.stories[b.id].tab))}<span class="x"><small>${t('speedX')}</small>CPU ${x('cpuSlc', 'cpuUp')} · GPU ${x(...Y.x)}</span></h3>
        <div class="bars"><p class="grp-l">CPU</p>${row('LuxCoreRender', 'cpuUp', '')}${row('SuperLuxCore', 'cpuSlc', 'slc')}
        <p class="grp-l">GPU</p>${Y.gpu.map(g => row(...g)).join('')}</div></div></div>`;
    }).join('');
    const notes = Y.notes.map(([b, k]) => `<p>${b ? `<b>${b}</b> ` : ''}${t(k)}</p>`).join('');
    return `${hwChips(Y.hw())}<div class="speed">${rows}</div><p class="sp-unit">${t('speedUnit')}</p><div class="sp-notes">${notes}${B.some(b => (b.est || []).length) ? `<p><small>${t('speedEst')}</small></p>` : ''}</div>`;
  }
  function bindCompare() {
    document.querySelectorAll('[data-cmp]').forEach(view => {
      const range = view.querySelector('input');
      const set = v => { v = Math.max(0, Math.min(100, v)); view.style.setProperty('--split', v + '%'); range.value = v; };
      range.addEventListener('input', e => set(+e.target.value));
      const move = e => { const r = view.getBoundingClientRect(); set((e.clientX - r.left) / r.width * 100); };
      view.addEventListener('pointerdown', e => { if (e.pointerType === 'mouse' && e.button) return; view.setPointerCapture(e.pointerId); move(e); });
      view.addEventListener('pointermove', e => { if (view.hasPointerCapture(e.pointerId)) move(e); });
    });
  }

  function renderFeatures() {
    const q = query.trim().toLowerCase();
    const list = D.features.filter(f => (category === 'all' || f.category === category) &&
      (!q || [f.id, f.title.en, f.title.ko, f.description.en, f.description.ko, f.badge.en].join(' ').toLowerCase().includes(q)));
    $('count').textContent = `${list.length} / ${D.features.length}`;
    // features with a render tile first (left half upstream, right half SuperLuxCore), the rest as a compact list
    const shot = list.filter(f => S.tiles[f.id]), rest = list.filter(f => !S.tiles[f.id]);
    const tile = f => `<a class="feat shot" href="${f.href}" target="_blank" rel="noopener" title="${esc(tr(f.description))}">
        <div class="shot-img"><img src="${S.tiles[f.id]}" width="480" height="300" alt="${esc(tr(f.title))}" loading="lazy" decoding="async"><span class="l">LuxCoreRender</span><span class="r">SuperLuxCore</span></div>
        <div class="shot-cap"><h3>${esc(tr(f.title))}</h3><b>${esc(tr(f.badge))}</b></div></a>`;
    const mini = f => `<a class="feat mini cat-${f.category}" href="${f.href}" target="_blank" rel="noopener" title="${esc(tr(f.description))}"><i aria-hidden="true"></i><span><h3>${esc(tr(f.title))}</h3><small>${esc(tr(D.categories[f.category]))} · ${esc(tr(f.badge))}</small></span></a>`;
    $('feats').innerHTML = (shot.length ? `<div class="shots">${shot.map(tile).join('')}</div>` : '') +
      (rest.length ? `${shot.length ? `<p class="feat-sub">${t('featMore')}</p>` : ''}<div class="minis">${rest.map(mini).join('')}</div>` : '') ||
      `<p class="empty">${t('noResults')}</p>`;
    document.querySelectorAll('[data-cat]').forEach(b => b.setAttribute('aria-pressed', b.dataset.cat === category));
  }

  function setPass(i) {
    pass = i; const p = S.passes[i], img = $('pass-img');
    img.src = p.src; img.alt = T(p.label);
    document.querySelectorAll('[data-pass]').forEach(b => b.setAttribute('aria-pressed', +b.dataset.pass === i));
    document.querySelectorAll('[data-thumb]').forEach(b => b.setAttribute('aria-current', +b.dataset.thumb === i));
  }

  function bind() {
    document.querySelectorAll('[data-pass]').forEach(b => b.addEventListener('click', () => setPass(+b.dataset.pass)));
    document.querySelectorAll('[data-thumb]').forEach(b => b.addEventListener('click', () => setPass(+b.dataset.thumb)));
    document.querySelectorAll('[data-cat]').forEach(b => b.addEventListener('click', () => { category = b.dataset.cat; renderFeatures(); }));
    document.querySelectorAll('[data-sys]').forEach(b => b.addEventListener('click', () => {
      sys = b.dataset.sys; $('speed-body').innerHTML = speed();
      document.querySelectorAll('[data-sys]').forEach(e => e.setAttribute('aria-pressed', e.dataset.sys === sys));
    }));
    $('q').addEventListener('input', e => { query = e.target.value; renderFeatures(); });
  }

  $('lang').addEventListener('change', e => {
    pref = e.target.value;
    try { pref === 'auto' ? localStorage.removeItem(KEY) : localStorage.setItem(KEY, pref); } catch { /* storage unavailable */ }
    const u = new URL(location.href); pref === 'auto' ? u.searchParams.delete('lang') : u.searchParams.set('lang', pref); history.replaceState(null, '', u);
    lang = pref === 'auto' ? detect() : pref; render();
  });
  render();
  if (location.hash) { try { document.getElementById(decodeURIComponent(location.hash.slice(1)))?.scrollIntoView({ block: 'start' }); } catch { /* bad hash */ } }
})();

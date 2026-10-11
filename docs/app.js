/* No build step, trackers or third-party runtime. */
(() => {
  'use strict';
  const D = window.SLC_DATA;
  const $ = id => document.getElementById(id);
  const esc = value => String(value).replace(/[&<>"']/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
  const storageKey = 'superluxcore-language';
  const valid = value => value === 'en' || value === 'ko';
  function readPreference() { try { return localStorage.getItem(storageKey); } catch { return null; } }
  function resolveLanguage(urlLanguage, savedLanguage, languages) {
    if (valid(urlLanguage)) return urlLanguage;
    if (valid(savedLanguage)) return savedLanguage;
    return /^ko(?:-|$)/i.test((languages || [])[0] || '') ? 'ko' : 'en';
  }
  const urlLanguage = new URL(location.href).searchParams.get('lang');
  let preference = valid(urlLanguage) ? urlLanguage : (valid(readPreference()) ? readPreference() : 'auto');
  let lang = resolveLanguage(urlLanguage, preference, navigator.languages || [navigator.language]);
  let category = 'all', search = '', expanded = false, comparisonId = 'transmission', split = 50;
  const tr = value => typeof value === 'string' ? value : value[lang];
  const c = key => esc(tr(D.copy[key]));
  const text = value => esc(tr(value));
  const manualURL = () => `https://claudianus.github.io/SuperBlendLuxCore/${lang === 'ko' ? 'ko/' : ''}manual/`;
  const statusURL = `${D.addon}/blob/main/doc/engineering/2026-10-09-cycles-scene-goal.md`;
  const intro = (eyebrow, heading, body, link = '') => `<div class="section-intro"><div><p class="eyebrow">${c(eyebrow)}</p><h2>${c(heading)}</h2>${body ? `<p>${c(body)}</p>` : ''}</div>${link}</div>`;
  function render() {
    document.documentElement.lang = lang;
    document.title = tr(D.copy.title);
    document.querySelector('meta[name="description"]').content = tr(D.copy.description);
    document.querySelector('meta[property="og:title"]').content = tr(D.copy.title);
    document.querySelector('meta[property="og:description"]').content = tr(D.copy.description);
    $('skip-link').textContent = tr(D.copy.skip);
    $('language-label').textContent = tr(D.copy.language);
    $('language').options[0].text = tr(D.copy.auto);
    $('language').value = preference;
    $('navigation').setAttribute('aria-label', tr(D.copy.navigation));
    $('navigation').innerHTML = `<a href="#features">${c('navFeatures')}</a><a href="#differences">${c('navDifferences')}</a><a href="#comparisons">${c('navRenders')}</a><a href="#download">${c('navDownload')}</a>`;
    $('main').innerHTML = `
      <section class="hero"><div class="wrap">
        <div class="hero-grid"><div class="hero-copy"><div class="release-pill">${c('pill')}</div><h1>${c('heroFirst')}<br><span>${c('heroSecond')}</span></h1><p>${c('heroDescription')}</p><div class="hero-actions"><a class="button" href="#download">${c('download')} <span aria-hidden="true">↓</span></a><a class="button secondary" href="${D.core}">${c('source')} <span aria-hidden="true">↗</span></a></div><p class="hero-note">${c('heroNote')}</p></div>
        <figure class="hero-art"><img src="assets/renders/orbs.jpg?v=20261011-aces2-final" width="1600" height="900" fetchpriority="high" alt="${c('heroAlt')}"><figcaption>${c('heroCaption')}</figcaption></figure></div>
        <div class="hero-stats">${D.stats.map(([a,b]) => `<div><strong>${text(a)}</strong><span>${text(b)}</span></div>`).join('')}</div>
      </div></section>
      <section class="highlights"><div class="wrap highlight-grid">${D.highlights.map(([a,b], i) => `<article class="highlight"><span class="number">0${i+1} / SUPERLUXCORE</span><h3>${text(a)}</h3><p>${text(b)}</p></article>`).join('')}</div></section>
      <section class="section" id="comparisons"><div class="wrap">${intro('compareEyebrow','compareHeading','compareIntro',`<a class="text-link" href="evidence/provenance.json">${c('renderProvenance')}</a>`)}
        <div class="compare-tabs" role="tablist" aria-label="${c('navRenders')}">${D.comparisons.map(item => `<button class="tab" id="tab-${item.id}" role="tab" aria-selected="${item.id===comparisonId}" aria-controls="compare-panel" tabindex="${item.id===comparisonId ? 0 : -1}" data-comparison="${item.id}">${text(item.tab)}</button>`).join('')}</div>
        <div id="compare-panel" class="compare-card" role="tabpanel"></div><p class="comparison-note">${c('compareNote')}</p>
      </div></section>
      <section class="section" id="differences"><div class="wrap">${intro('differenceEyebrow','differenceHeading','differenceIntro')}
        <div class="matrix-scroll" tabindex="0" role="region" aria-label="${c('navDifferences')}"><table><caption>${c('navDifferences')}</caption><thead><tr><th scope="col">${c('tableFeature')}</th><th scope="col">${c('tableUpstream')}</th><th scope="col">${c('tableFork')}</th></tr></thead><tbody>${D.matrix.map(([area, upstream, fork, page]) => `<tr><th scope="row">${text(area)}</th><td>${text(upstream)}</td><td>${text(fork)}<a href="${D.core}/blob/main/doc/${page}">${c('readDocs')}</a></td></tr>`).join('')}</tbody></table></div>
        <p class="source-note">${c('baselineNote')} <a href="https://luxcorerender.org/">${c('upstreamSource')} ↗</a> · <a href="${D.core}/tree/e9ced7e7a">${c('forkBaseline')} ↗</a></p>
      </div></section>
      <section class="section audit" id="history"><div class="wrap">${intro('auditEyebrow','auditHeading','auditIntro')}
        <div class="audit-areas">${D.auditAreas.map(([count,name])=>`<div><strong>${count}</strong><span>${text(name)}</span></div>`).join('')}</div>
        <p class="small source-note">${lang==='ko'?'변경 분야는 중복 집계합니다. 숫자는 신규 기능 수가 아닙니다.':'Areas overlap. These are commit counts, not counts of new features.'}</p>
        <div class="audit-benefits">${D.auditBenefits.map(([title,body,sha])=>`<article><h3>${text(title)}</h3><p>${text(body)}</p><a href="${D.core}/commit/${sha}">${sha} ↗</a></article>`).join('')}</div>
        <div class="status-links"><a href="evidence/commit-audit.json">${c('auditInventory')}</a><a href="${D.core}/blob/main/docs/evidence/fork-analysis${lang==='ko'?'.ko':''}.md">${c('auditAnalysis')}</a></div>
      </div></section>
      <section class="section" id="features"><div class="wrap">${intro('featuresEyebrow','featuresHeading','featuresIntro',`<a class="text-link" href="${D.core}/tree/main/doc/features">${c('allDocs')}</a>`)}
        <div class="feature-tools"><label class="search"><span class="sr-only">${c('searchLabel')}</span><input id="feature-search" type="search" placeholder="${c('search')}" value="${esc(search)}" autocomplete="off"></label><span id="feature-count" class="feature-count" aria-live="polite"></span></div>
        <div class="filter-list" role="group" aria-label="${c('navFeatures')}">${Object.entries(D.categories).map(([id,name])=>`<button class="filter" data-category="${id}" aria-pressed="${id===category}">${text(name)}</button>`).join('')}</div>
        <div id="feature-grid" class="feature-grid"></div><button id="show-more" class="show-more" type="button"></button>
      </div></section>
      <section class="section gallery" id="gallery"><div class="wrap">${intro('galleryEyebrow','galleryHeading','galleryIntro')}
        <div class="gallery-grid">${D.gallery.map(([id,title,description,imagePath = `assets/renders/${id}.jpg`])=>`<figure class="gallery-card"><a href="${imagePath}" aria-label="${text(title)} · ${c('fullImages')}"><img src="${imagePath}?v=20261011-aces2-final" width="1600" height="900" loading="lazy" decoding="async" alt="${text(description)}"></a><figcaption><h3>${text(title)}</h3><p>${text(description)}</p><p class="small">${c('galleryDate')}</p></figcaption></figure>`).join('')}</div><p class="source-note">${c('galleryNote')}</p>
      </div></section>
      <section class="section" id="download"><div class="wrap"><div class="release-block">${intro('downloadEyebrow','downloadHeading','downloadIntro')}
        <div class="download-grid">${[
          ['macos_arm64','macOS',tr({en:'Apple silicon · ARM64',ko:'Apple Silicon · ARM64'})],
          ['macos_x64','macOS',tr({en:'Intel · x64',ko:'Intel · x64'})],
          ['windows_x64','Windows','x64'],['linux_x64','Linux','x64'],
        ].map(([id,name,arch])=>`<a class="download-card" href="${D.addon}/releases/download/latest/SuperLuxCore-Latest-${id}.zip"><strong>${name}</strong><span>${esc(arch)}</span><em>${c('downloadZip')}</em></a>`).join('')}</div>
        <div class="download-meta"><a href="${manualURL()}">${c('manual')} ↗</a><a href="${D.addon}/releases/tag/latest">${c('releaseNotes')} ↗</a><a href="${D.core}/releases/tag/wheels-latest">${c('nativeWheels')} ↗</a></div>
        <div class="install-steps">${['One','Two','Three'].map(n=>`<div class="install-step"><b>${c('install'+n)}</b><small>${c('install'+n+'Body')}</small></div>`).join('')}</div><p class="release-warning">${c('rollingNote')}</p>
      </div></div></section>
      <section class="section" id="updates"><div class="wrap">${intro('updatesEyebrow','updatesHeading','')}
        <div class="updates-grid">${D.updates.map(item=>`<article class="update"><time>${esc(item.date)}</time><h3>${text(item.title)}</h3><p>${text(item.description)}</p><a href="${item.href}">${c('readDocs')}</a></article>`).join('')}</div>
      </div></section>
      <section class="section" id="support"><div class="wrap support-grid"><div><p class="eyebrow">${c('supportEyebrow')}</p><h2>${c('supportHeading')}</h2><ul class="status-list">${D.status.map(([a,b])=>`<li><strong>${text(a)}</strong>${text(b)}</li>`).join('')}</ul><div class="status-links"><a href="${statusURL}">${c('statusDoc')} ↗</a><a href="${D.core}/tree/main/doc/features">${c('allDocs')} ↗</a></div></div>
        <div class="faq"><h2>${c('faqHeading')}</h2>${D.faq.map(([q,a])=>`<details><summary>${text(q)}</summary><p>${text(a)}</p></details>`).join('')}</div>
      </div></section>`;
    $('footer').innerHTML = `<div class="wrap"><div class="footer-row"><div><a class="brand" href="#"><img src="assets/mark.svg" width="27" height="27" alt=""><span>SuperLux<span class="brand-light">Core</span></span></a><p>${c('footerDescription')}</p></div><div class="footer-links"><a href="${D.core}">GitHub</a><a href="${manualURL()}">${c('manual')}</a><a href="${D.core}/issues">${lang==='ko'?'문제 제보':'Report an issue'}</a><a href="evidence/provenance.json">${c('renderProvenance')}</a></div></div><div class="footer-bottom"><span>${c('footerLicense')}</span><span>${c('footerDate')}</span></div></div>`;
    renderComparison();renderFeatures();bindMain();
  }
  function setSplit(value) {
    split = Math.max(0, Math.min(100, Number(value)));
    $('compare-view').style.setProperty('--split', `${split}%`);
    $('comparison-range').value = split;
    $('comparison-range').setAttribute('aria-valuetext',`${Math.round(split)}% ${tr(D.comparisons.find(i=>i.id===comparisonId).left)}`);
  }
  function renderComparison() {
    const item = D.comparisons.find(i=>i.id===comparisonId);
    $('compare-panel').setAttribute('aria-labelledby',`tab-${item.id}`);
    $('compare-panel').innerHTML = `<div class="compare-view" id="compare-view" style="--split:${split}%"><img src="${item.after}?v=20261011-aces2-final" width="1600" height="900" loading="lazy" alt="${text(item.title)} · ${text(item.right)}"><img class="compare-before" src="${item.before}?v=20261011-aces2-final" width="1600" height="900" loading="lazy" alt="${text(item.title)} · ${text(item.left)}"><div class="compare-divider" aria-hidden="true"></div><span class="compare-label left">${text(item.left)}</span><span class="compare-label right">${text(item.right)}</span></div>
      <div class="compare-bottom"><span class="chip">${text(item.kind)}</span><h3>${text(item.title)}</h3><p>${text(item.description)}</p><label class="range-label" for="comparison-range"><span>${c('slider')}</span><input id="comparison-range" type="range" min="0" max="100" value="${split}" aria-label="${c('slider')} · ${text(item.title)}"><span aria-hidden="true">↔</span></label><div class="compare-meta"><span>${text(item.meta)}</span><a href="${item.evidence}">${c('evidence')} ↗</a><a href="${item.before}">${text(item.left)} ↗</a><a href="${item.after}">${text(item.right)} ↗</a></div></div>`;
    $('comparison-range').addEventListener('input',e=>setSplit(e.target.value));
    const view = $('compare-view');
    const move = e => {const bounds=view.getBoundingClientRect();setSplit((e.clientX-bounds.left)/bounds.width*100);};
    view.addEventListener('pointerdown',e=>{if(e.button!==0)return;view.setPointerCapture(e.pointerId);move(e);});
    view.addEventListener('pointermove',e=>{if(view.hasPointerCapture(e.pointerId))move(e);});
    view.addEventListener('pointerup',e=>{if(view.hasPointerCapture(e.pointerId))view.releasePointerCapture(e.pointerId);});
    setSplit(split);
    document.querySelectorAll('[data-comparison]').forEach(button=>{const active=button.dataset.comparison===comparisonId;button.setAttribute('aria-selected',active);button.tabIndex=active?0:-1;});
  }
  function renderFeatures() {
    const query=search.trim().toLocaleLowerCase();
    const items=D.features.filter(item=>(category==='all'||item.category===category)&&(!query||[item.title.en,item.title.ko,item.description.en,item.description.ko,item.id].join(' ').toLocaleLowerCase().includes(query)));
    const limited=!expanded&&category==='all'&&!query;
    $('feature-count').textContent = `${items.length} ${tr(D.copy.featureCount)}`;
    $('feature-grid').innerHTML=items.map((item,i)=>`<article class="feature-card"${limited&&i>=12?' hidden':''}><header><span class="feature-category">${text(D.categories[item.category])}</span><span class="chip${/Experimental|실험/.test(text(item.badge))?' experimental':''}">${text(item.badge)}</span></header><h3>${text(item.title)}</h3><p>${text(item.description)}</p><a href="${item.href}">${c('readDocs')}</a></article>`).join('')||`<p class="empty">${c('noResults')}</p>`;
    $('show-more').hidden=category!=='all'||Boolean(query)||items.length<=12;
    $('show-more').textContent=tr(expanded?D.copy.showLess:D.copy.showAll)+(expanded?'':` (${items.length})`);
    $('show-more').setAttribute('aria-expanded',String(expanded));
    $('show-more').setAttribute('aria-controls','feature-grid');
    document.querySelectorAll('[data-category]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.category===category)));
  }
  function chooseComparison(id) {comparisonId=id;renderComparison();}
  function bindMain() {
    document.querySelectorAll('[data-comparison]').forEach(button=>{
      button.addEventListener('click',()=>chooseComparison(button.dataset.comparison));
      button.addEventListener('keydown',e=>{if(!['ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;e.preventDefault();const index=D.comparisons.findIndex(i=>i.id===comparisonId);const next=e.key==='Home'?0:e.key==='End'?D.comparisons.length-1:(index+(e.key==='ArrowRight'?1:-1)+D.comparisons.length)%D.comparisons.length;chooseComparison(D.comparisons[next].id);$(`tab-${comparisonId}`).focus();});
    });
    document.querySelectorAll('[data-category]').forEach(button=>button.addEventListener('click',()=>{category=button.dataset.category;renderFeatures();}));
    $('feature-search').addEventListener('input',e=>{search=e.target.value;renderFeatures();});
    $('show-more').addEventListener('click',()=>{expanded=!expanded;renderFeatures();if(!expanded)$('features').scrollIntoView();});
  }
  $('language').addEventListener('change',e=>{
    preference=e.target.value;
    try {if(preference==='auto')localStorage.removeItem(storageKey);else localStorage.setItem(storageKey,preference);} catch {}
    const url=new URL(location.href);if(preference==='auto')url.searchParams.delete('lang');else url.searchParams.set('lang',preference);
    history.replaceState(null,'',url);
    lang=resolveLanguage(null,preference,navigator.languages||[navigator.language]);render();
  });
  window.addEventListener('languagechange',()=>{if(preference==='auto'){lang=resolveLanguage(null,null,navigator.languages||[navigator.language]);render();}});
  render();
  // Sections are created after HTML parsing; honor shareable fragment links.
  if (location.hash) {
    try {
      const section = document.getElementById(decodeURIComponent(location.hash.slice(1)));
      if (section) section.scrollIntoView({behavior:'instant',block:'start'});
    } catch { /* A malformed fragment must not prevent the page from rendering. */ }
  }
  // A pure locale resolver is exposed for deterministic browser verification.
  window.SLC = Object.freeze({resolveLanguage,version:D.version,featureCount:D.features.length});
})();

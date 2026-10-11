/* Native renders, bilingual content, no framework, no tracking. */
(() => {
  'use strict';
  const D=window.SLC_DATA, $=id=>document.getElementById(id);
  const escape=v=>String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const valid=v=>v==='en'||v==='ko', storage='superluxcore-language';
  function saved(){try{return localStorage.getItem(storage)}catch{return null}}
  function resolveLanguage(url,pref,languages){return valid(url)?url:valid(pref)?pref:/^ko(?:-|$)/i.test((languages||[])[0]||'')?'ko':'en'}
  const initial=new URL(location.href).searchParams.get('lang');
  let preference=valid(initial)?initial:valid(saved())?saved():'auto',lang=resolveLanguage(initial,preference,navigator.languages||[navigator.language]);
  let category='all',query='',workGroup='featured',comparison='diffraction',split=50,lastFocus=null,currentProject=null,showAll=false;
  const featured=['prism','pearl','textile','architecture','atmosphere','music'];
  const backend={prism:'METAL',pearl:'METAL',textile:'CPU',architecture:'CPU',atmosphere:'METAL',music:'CPU',interior:'METAL',jewelry:'METAL',audio:'CPU',beauty:'CPU',heritage:'CPU',perfume:'METAL',fan:'CPU'};
  // Keep the first visual index deliberately scene-diverse. The full feature
  // catalogue remains available through the scope toggle, but the default
  // grid should not spend two cards on the same beauty render.
  const atlas=['metal','spectral','openpbr','diffraction','thinfilm','hair','volume','cryptomatte','lpe','gpult','motion','displacement'];
  const atlasPlates=[['sellmeier','DISPERSION'],['thinfilm','THIN FILM'],['openpbr','TRANSMISSION'],['hair','FIBRE SCATTERING']];
  const tr=v=>typeof v==='string'?v:v[lang], t=v=>escape(tr(v)), c=k=>t(D.copy[k]);
  const img=p=>p+'?v=production-lab-20261012-4', manual=()=>`https://claudianus.github.io/SuperBlendLuxCore/${lang==='ko'?'ko/':''}manual/`;
  const number=i=>String(i+1).padStart(2,'0');
  function picture(path,alt,classes='',priority=false){return `<img class="${classes}" src="${img(path)}" width="1920" height="1200" alt="${t(alt)}" ${priority?'fetchpriority="high"':'loading="lazy" decoding="async"'}>`}
  function heading(eyebrow,title,body){return `<div class="section-head"><p class="eyebrow">${c(eyebrow)}</p><div class="head-row"><h2>${c(title).replace(/\n/g,'<br>')}</h2><p>${c(body)}</p></div></div>`}
  function render(){
    document.documentElement.lang=lang;document.title=tr(D.copy.title);
    for(const selector of ['meta[name="description"]','meta[property="og:description"]'])document.querySelector(selector).content=tr(D.copy.description);
    document.querySelector('meta[property="og:title"]').content=tr(D.copy.title);
    $('skip-link').textContent=tr(D.copy.skip);$('language-label').textContent=tr(D.copy.language);$('language').options[0].text=tr(D.copy.auto);$('language').value=preference;
    $('navigation').innerHTML=[['gallery','work'],['comparisons','differences'],['features','toolkit']].map(([id,label])=>`<a href="#${id}">${c(label)}</a>`).join('')+`<a class="nav-cta" href="#download">${c('download')} <span>↗</span></a>`;
    const hero=D.projects.find(p=>p.id===D.hero);
    $('main').innerHTML=`
      <section class="hero" aria-label="${c('heroTag')}">
        ${picture(hero.image,hero.description,'hero-image',true)}<div class="hero-shade"></div>
        <div class="hero-top"><span class="eyebrow">${c('heroTag')}</span><span class="hero-release">V ${escape(D.version)} / 2026</span></div>
        <div class="hero-copy"><p class="hero-kicker">NATIVE LIGHT TRANSPORT / BLENDER</p><h1>${c('heroFirst')}<br><em>${c('heroSecond')}</em></h1><p>${c('heroBody').replace(/\n/g,'<br>')}</p><div class="hero-actions"><a class="primary-link" href="#download">${c('download')} <span>↗</span></a><a class="secondary-link" href="#comparisons">${c('differences')} <span>→</span></a></div></div>
        <div class="renderer-status" aria-label="${lang==='ko'?'렌더러 상태':'Renderer status'}"><div><b>2.11.27</b><span>CORE</span></div><div><b>CPU / METAL</b><span>BACKENDS</span></div><div><b>OPENPBR</b><span>MATERIALS</span></div><div><b>ACES 2.0</b><span>DISPLAY</span></div></div>
        <div class="hero-bottom"><button class="hero-project" data-project="${hero.id}"><span>${hero.name} / ${number(D.projects.indexOf(hero))}</span><small>${t(hero.area)} ↗</small></button><span>${c('renderCredit')}</span><a href="#download">${c('heroDownload')} ↓</a></div>
      </section>
      <section class="manifesto wrap"><p class="eyebrow">SUPERLUXCORE / BLENDER 5.2</p><div class="manifesto-grid"><h2>${c('manifesto').replace(/\n/g,'<br>')}</h2><div><p>${c('manifestoBody')}</p><div class="capability-line"><span>APPLE METAL</span><span>WAVE OPTICS</span><span>OPENPBR</span><span>CPU / GPU</span></div></div></div></section>
      <section class="capability-rail wrap" aria-label="${lang==='ko'?'렌더러 기능 영역':'Renderer capabilities'}"><div class="capability-rail-head"><p class="eyebrow">01 / CAPABILITY MAP</p><span>${lang==='ko'?'이미지가 보여주는 렌더링 영역':'What the image proves'}</span></div><div class="capability-rail-grid">${[['hardware','Hardware','하드웨어','heritage'],['transport','Light transport','빛의 전달','architecture'],['caustics','Caustics','커스틱스','pearl'],['materials','Materials & fibre','재질과 섬유','textile'],['volumes','Volumes','볼륨','atmosphere'],['passes','Production passes','제작 패스','interior']].map(([en,et,ko,id],i)=>`<button data-project="${id}"><span>${number(i)}</span><strong>${lang==='ko'?ko:et}</strong><small>${lang==='ko'?'실제 장면 보기':'Open native scene'} ↗</small></button>`).join('')}</div></section>
      <section class="material-atlas wrap" id="atlas">${heading('atlasEyebrow','atlasHeading','atlasIntro')}<div class="atlas-grid">${atlasPlates.map(([id,label],i)=>{const f=D.features.find(x=>x.id===id);return f?`<button class="atlas-plate ${i===0?'atlas-plate-wide':''}" data-feature="${id}" aria-label="${t(f.title)} / ${c('detail')}"><div class="atlas-plate-image">${picture(f.image,f.title)}<span>${number(i)} / ${label}</span><b>↗</b></div><div class="atlas-plate-caption"><h3>${t(f.title)}</h3><small>${t(f.badge)}</small></div></button>`:''}).join('')}</div><p class="atlas-note">${c('atlasNote')}</p></section>
      <section class="work-section wrap" id="gallery">${heading('workEyebrow','workHeading','workIntro')}
        <div class="work-filter" role="group" aria-label="${c('work')}">${[['featured',lang==='ko'?'대표 장면':'Featured'],['archive',lang==='ko'?'전체 아카이브':'Scene archive'],['product','product'],['space','space'],['material','material']].map(([id,label])=>`<button data-work="${id}" aria-pressed="${id===workGroup}">${escape(label)}</button>`).join('')}</div><div class="work-grid" id="work-grid"></div>
      </section>
      <section class="detail-strip" aria-label="${c('detail')}">${[['textile',lang==='ko'?'섬유 한 올까지':'EVERY STRAND'],['prism',lang==='ko'?'빛이 색이 되는 순간':'LIGHT IN COLOUR'],['pearl',lang==='ko'?'표면에 담긴 빛':'SURFACE & LIGHT']].map(([id,label])=>`<button data-project="${id}">${picture(`assets/studio/${id}.jpg`,D.projects.find(p=>p.id===id).description)}<span>${label}<b>↗</b></span></button>`).join('')}</section>
      <section class="comparison-section" id="comparisons"><div class="wrap">${heading('compareEyebrow','compareHeading','compareIntro')}
        <div class="compare-tabs" role="tablist" aria-label="${c('differences')}">${D.comparisons.map(p=>`<button role="tab" id="tab-${p.id}" data-comparison="${p.id}" aria-selected="${comparison===p.id}" aria-controls="compare-panel" tabindex="${comparison===p.id?0:-1}">${t(p.tab)}</button>`).join('')}</div><div id="compare-panel" role="tabpanel"></div>
        <p class="comparison-scope">${c('scope')}</p>
      </div></section>
      <section class="workflow-section wrap"><div class="workflow-copy"><p class="eyebrow">THE IMAGE / UNDER THE SURFACE</p><h2>${lang==='ko'?'작품 뒤에도,<br>완성도를.':'Behind the image.<br>More control.'}</h2><p>${lang==='ko'?'아름다운 결과에서 끝나지 않습니다. 표면 정보, 디노이즈와 커버리지까지 실제 출력으로 살펴보세요.':'Go beyond the beauty. Explore actual surface data, denoising and coverage from the same render.'}</p><button class="text-button" data-project="interior">${c('detail')} ↗</button><small>${c('passNote')}</small></div><button class="workflow-image" data-project="interior">${picture('assets/studio/interior.jpg',D.projects.find(p=>p.id==='interior').description)}<span class="pass-strip">${['interior-albedo','interior-normal','interior-crypto-objects'].map(p=>picture('assets/studio/'+p+'.jpg',D.copy.technical)).join('')}</span><b>${lang==='ko'?'작품 + 실제 패스 보기':'BEAUTY + ACTUAL PASSES'} ↗</b></button></section>
      <section class="toolkit-section wrap" id="features">${heading('toolkitEyebrow','toolkitHeading','toolkitIntro')}
        <div class="toolkit-tools"><label class="search"><span class="sr-only">${c('searchLabel')}</span><span aria-hidden="true">⌕</span><input id="feature-search" type="search" value="${escape(query)}" placeholder="${c('search')}" autocomplete="off"></label><div class="toolkit-count"><span id="feature-count" aria-live="polite"></span><button id="feature-scope" class="scope-toggle" type="button" aria-pressed="false"></button></div></div>
        <div class="filters" role="group" aria-label="${c('toolkit')}">${Object.entries(D.categories).map(([id,label])=>`<button data-category="${id}" aria-pressed="${category===id}">${t(label)}</button>`).join('')}</div><div class="visual-grid" id="feature-grid"></div>
      </section>
      <section class="download-section" id="download"><div class="wrap"><p class="eyebrow">${c('downloadEyebrow')}</p><div class="download-heading"><h2>${c('downloadFirst')}<br><em>${c('downloadSecond')}</em></h2><p>${c('downloadBody').replace(/\n/g,'<br>')}</p></div>
        <div class="download-grid">${[['macos_arm64','macOS','APPLE SILICON'],['macos_x64','macOS','INTEL'],['windows_x64','Windows','X64'],['linux_x64','Linux','X64']].map(([id,name,arch],i)=>`<a href="${D.addon}/releases/download/latest/SuperLuxCore-Latest-${id}.zip"><span class="download-index">${number(i)}</span><strong>${name}</strong><small>${arch}</small><span class="download-arrow">↗</span><em>${c('downloadZip')}</em></a>`).join('')}</div>
        <div class="install-steps">${['One','Two','Three'].map((n,i)=>`<div><span>${number(i)}</span><strong>${c('install'+n)}</strong><p>${c('install'+n+'Body')}</p></div>`).join('')}</div><div class="download-meta"><span>${escape(D.version)} · Blender 5.2 · ${lang==='ko'?'수시 갱신 프리릴리스':'Rolling prerelease'}</span><a href="${manual()}">${c('manual')} ↗</a><a href="${D.addon}/releases/tag/latest">${c('release')} ↗</a></div>
      </div></section>
      <section class="support-section wrap"><div class="support-head"><h2>${c('faq')}</h2><span>${c('free')}</span></div><div class="faq-grid">${D.faq.map(([q,a])=>`<details><summary>${t(q)}<span>+</span></summary><p>${t(a)}</p></details>`).join('')}</div><details class="coverage"><summary>${c('support')}<span>+</span></summary><div>${D.status.map(([a,b])=>`<p><strong>${t(a)}</strong>${t(b)}</p>`).join('')}<a href="${D.core}/tree/main/doc/features">${c('docs')}</a></div></details></section>
      <dialog id="visual-dialog" class="visual-dialog" aria-labelledby="dialog-title"></dialog>`;
    $('footer').innerHTML=`<div class="wrap"><div class="footer-top"><a class="footer-wordmark" href="#">SUPERLUX<span>CORE</span><i>↗</i></a><p>${c('footerLine')}</p></div><div class="footer-bottom"><span>© 2026 / APACHE-2.0 · GPL-3.0</span><span>${c('artistNote')}</span><div><a href="${D.core}">GitHub ↗</a><a href="evidence/provenance.json">${c('credits')} ↗</a></div></div></div>`;
    renderWork();renderComparison();renderFeatures();bind();
  }
  function bind(){
    document.querySelectorAll('[data-work]').forEach(b=>b.addEventListener('click',()=>{workGroup=b.dataset.work;renderWork();document.querySelectorAll('[data-work]').forEach(x=>x.setAttribute('aria-pressed',String(x.dataset.work===workGroup)))}));
    document.querySelectorAll('[data-project]').forEach(b=>b.addEventListener('click',()=>openProject(b.dataset.project,b)));
    document.querySelectorAll('[data-category]').forEach(b=>b.addEventListener('click',()=>{category=b.dataset.category;renderFeatures()}));
    $('feature-search').addEventListener('input',e=>{query=e.target.value;renderFeatures()});
    $('feature-scope').addEventListener('click',()=>{showAll=!showAll;renderFeatures()});
    document.querySelectorAll('[data-comparison]').forEach(b=>{
      b.addEventListener('click',()=>{comparison=b.dataset.comparison;split=50;renderComparison()});
      b.addEventListener('keydown',e=>{if(!['ArrowRight','ArrowLeft','Home','End'].includes(e.key))return;e.preventDefault();let i=D.comparisons.findIndex(p=>p.id===comparison);i=e.key==='Home'?0:e.key==='End'?D.comparisons.length-1:(i+(e.key==='ArrowRight'?1:-1)+D.comparisons.length)%D.comparisons.length;comparison=D.comparisons[i].id;renderComparison();$('tab-'+comparison).focus()});
    });
    $('visual-dialog').addEventListener('click',e=>{if(e.target===$('visual-dialog'))closeDialog()});
    $('visual-dialog').addEventListener('close',()=>{document.body.classList.remove('dialog-open');lastFocus?.focus()});
  }
  function renderWork(){
    const projects=D.projects.filter(p=>workGroup==='featured'?featured.includes(p.id):workGroup==='archive'?!featured.includes(p.id):p.group===workGroup);
    $('work-grid').innerHTML=projects.map((p,i)=>`<article class="project-card ${i===0?'project-wide':''}"><button data-project-card="${p.id}" aria-label="${p.name} / ${c('viewProject')}"><div class="project-image">${picture(p.image,p.description)}<span class="project-open">↗</span><span class="project-number">${number(D.projects.indexOf(p))} / ${D.projects.length}</span><span class="project-meta">NATIVE / ${backend[p.id]||'CPU'} / ACES 2.0</span></div><div class="project-caption"><h3>${p.name}</h3><span>${t(p.area)}</span></div></button><p>${t(p.description)}</p><div class="tags">${p.tags.map(tag=>`<span>${escape(tag)}</span>`).join('')}</div></article>`).join('');
    document.querySelectorAll('[data-project-card]').forEach(b=>b.addEventListener('click',()=>openProject(b.dataset.projectCard,b)));
  }
  function renderComparison(){
    const p=D.comparisons.find(i=>i.id===comparison);
    $('compare-panel').setAttribute('aria-labelledby','tab-'+p.id);
    $('compare-panel').innerHTML=`<div class="compare-view" id="compare-view" style="--split:${split}%">${picture(p.after,p.right)}${picture(p.before,p.left,'compare-before')}<div class="compare-divider"><span>↔</span></div><div class="compare-labels"><span>${t(p.left)}</span><span>${t(p.right)}</span></div><span class="drag-hint">${c('compareDrag')} ↔</span></div><div class="comparison-mode"><button id="comparison-mode">${lang==='ko'?'전체 장면 보기 ↗':'See the full scene ↗'}</button></div><div class="compare-controls"><span>${c('before')}</span><input type="range" id="comparison-range" min="0" max="100" value="${split}" aria-label="${c('compareDrag')}"><span>${c('after')}</span></div><div class="compare-story"><h3>${t(p.title)}</h3><div><p>${t(p.body)}</p><details><summary>${c('comparisonDetails')} +</summary><p>${t(p.scope)}</p><a href="evidence/provenance.json">${c('credits')} ↗</a></details></div></div>`;
    $('comparison-mode').addEventListener('click',e=>openProject({diffraction:'music',energy:'audio',hair:'textile'}[p.id],e.currentTarget));
    $('comparison-range').addEventListener('input',e=>setSplit(e.target.value));
    const view=$('compare-view'), move=e=>{const rect=view.getBoundingClientRect();setSplit((e.clientX-rect.left)/rect.width*100)};
    view.addEventListener('pointerdown',e=>{if(e.button!==0)return;view.setPointerCapture(e.pointerId);move(e)});
    view.addEventListener('pointermove',e=>{if(view.hasPointerCapture(e.pointerId))move(e)});
    view.addEventListener('pointerup',e=>{if(view.hasPointerCapture(e.pointerId))view.releasePointerCapture(e.pointerId)});
    document.querySelectorAll('[data-comparison]').forEach(b=>{const active=b.dataset.comparison===comparison;b.setAttribute('aria-selected',String(active));b.tabIndex=active?0:-1});
    setSplit(split);
  }
  function setSplit(v){split=Math.max(0,Math.min(100,Number(v)));$('compare-view').style.setProperty('--split',split+'%');$('comparison-range').value=split}
  function renderFeatures(){
    const q=query.trim().toLowerCase(), atlasMode=category==='all'&&!showAll&&!q;
    const features=D.features.filter(f=>(category==='all'||f.category===category)&&(!q||[f.id,f.title.en,f.title.ko,f.description.en,f.description.ko].join(' ').toLowerCase().includes(q))&&(!atlasMode||atlas.includes(f.id)));
    $('feature-count').textContent=(atlasMode?features.length+' / '+D.features.length:features.length)+' '+tr(D.copy.featureCount);
    $('feature-scope').setAttribute('aria-pressed',String(showAll));
    $('feature-scope').textContent=showAll?(lang==='ko'?'아틀라스 12개만 보기':'Show atlas only'):(lang==='ko'?'전체 기능 보기':'Show all documented features');
    $('feature-grid').innerHTML=features.map(f=>`<article class="visual-card"><button data-feature="${f.id}" aria-label="${t(f.title)} / ${c('detail')}"><div class="visual-card-image">${picture(f.image,f.title)}<span class="visual-card-type">${c(f.technical?'technical':'context')}</span><span class="visual-card-open">↗</span></div><div class="visual-card-caption"><span>${t(D.categories[f.category])}</span><h3>${t(f.title)}</h3><small>${t(f.badge)}</small></div></button></article>`).join('')||`<p class="empty">${c('noResults')}</p>`;
    document.querySelectorAll('[data-feature]').forEach(b=>b.addEventListener('click',()=>openFeature(b.dataset.feature,b)));
    document.querySelectorAll('[data-category]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.category===category)));
  }
  function showDialog(body,focus){
    const dialog=$('visual-dialog');lastFocus=focus||document.activeElement;dialog.innerHTML=`<button class="dialog-close" aria-label="${c('close')}">×</button>${body}`;
    if(!dialog.open)dialog.showModal();document.body.classList.add('dialog-open');dialog.querySelector('.dialog-close').addEventListener('click',closeDialog);dialog.querySelector('.dialog-close').focus();
  }
  function closeDialog(){$('visual-dialog').close()}
  function openProject(id,focus){
    currentProject=id;const p=D.projects.find(x=>x.id===id), index=D.projects.indexOf(p);
    const passes=[['beauty',p.image],['albedo',`assets/studio/${id}-albedo.jpg`],['normal',`assets/studio/${id}-normal.jpg`],['raw',`assets/studio/${id}-raw.jpg`]];
    if(id==='beauty')passes.push([lang==='ko'?'SSS 끄기':'SSS off',`assets/studio/beauty-sss-off.jpg`]);
    if(id==='pearl')passes.push([lang==='ko'?'박막 끄기':'Film off',`assets/studio/pearl-film-off.jpg`]);
    if(id==='interior')passes.push(['Cryptomatte objects',`assets/studio/${id}-crypto-objects.jpg`],['Cryptomatte materials',`assets/studio/${id}-crypto-materials.jpg`]);
    showDialog(`<div class="project-dialog-image"><img id="dialog-image" src="${img(p.image)}" width="1920" height="1200" alt="${t(p.description)}"></div><div class="dialog-main"><div class="dialog-heading"><p class="eyebrow">${number(index)} / ${D.projects.length} · ${t(p.area)}</p><h2 id="dialog-title">${p.name}</h2><p>${t(p.description)}</p></div><div class="dialog-meta"><span>${c('renderCredit')}</span><div class="tags">${p.tags.map(tag=>`<span>${escape(tag)}</span>`).join('')}</div><a href="${p.image}" target="_blank" rel="noopener">${c('full')}</a></div></div><div class="pass-tabs" role="group" aria-label="${c('detail')}">${passes.map(([name,path],i)=>`<button data-pass="${path}" aria-pressed="${i===0}">${escape(name==='beauty'?(lang==='ko'?'작품':'Beauty'):name==='raw'?(lang==='ko'?'디노이즈 전':'Before denoise'):name)}</button>`).join('')}</div><p class="pass-note">${c('passNote')}${p.renderNote?'<br>'+t(p.renderNote):''}</p><div class="dialog-feature-links">${p.features.map(fid=>{const f=D.features.find(x=>x.id===fid);return `<button data-dialog-feature="${fid}">${t(f.title)} ↗</button>`}).join('')}</div><div class="project-pagination"><button id="prev-project">← ${c('previous')}</button><span>${number(index)} / ${D.projects.length}</span><button id="next-project">${c('next')} →</button></div>`,focus);
    document.querySelectorAll('[data-pass]').forEach(b=>b.addEventListener('click',()=>{$('dialog-image').src=img(b.dataset.pass);$('dialog-image').alt=tr(p.description)+' / '+b.textContent;document.querySelectorAll('[data-pass]').forEach(x=>x.setAttribute('aria-pressed',String(x===b)))}));
    document.querySelectorAll('[data-dialog-feature]').forEach(b=>b.addEventListener('click',()=>openFeature(b.dataset.dialogFeature,focus)));
    $('prev-project').addEventListener('click',()=>openProject(D.projects[(index-1+D.projects.length)%D.projects.length].id,focus));$('next-project').addEventListener('click',()=>openProject(D.projects[(index+1)%D.projects.length].id,focus));
  }
  function openFeature(id,focus){
    const f=D.features.find(x=>x.id===id),p=D.projects.find(x=>x.id===f.scene);
    showDialog(`<div class="feature-dialog-image">${picture(f.image,f.title,'',true)}<span>${c(f.technical?'technical':'context')}</span></div><div class="feature-dialog-copy"><p class="eyebrow">${t(D.categories[f.category])} / ${t(f.badge)}</p><h2 id="dialog-title">${t(f.title)}</h2><p>${t(f.description)}</p><div class="feature-dialog-actions"><button id="context-project">${c('sceneContext')}: ${p.name} ↗</button><a href="${f.href}" target="_blank" rel="noopener">${c('docs')}</a></div><small>${t(f.visualNote)}</small></div>`,focus);
    $('context-project').addEventListener('click',()=>openProject(p.id,focus));
  }
  $('language').addEventListener('change',e=>{preference=e.target.value;try{if(preference==='auto')localStorage.removeItem(storage);else localStorage.setItem(storage,preference)}catch{}const u=new URL(location.href);if(preference==='auto')u.searchParams.delete('lang');else u.searchParams.set('lang',preference);history.replaceState(null,'',u);lang=resolveLanguage(null,preference,navigator.languages||[navigator.language]);render()});
  window.addEventListener('languagechange',()=>{if(preference==='auto'){lang=resolveLanguage(null,null,navigator.languages||[navigator.language]);render()}});
  render();
  if(location.hash){try{document.getElementById(decodeURIComponent(location.hash.slice(1)))?.scrollIntoView({behavior:'instant',block:'start'})}catch{}}
  window.SLC=Object.freeze({resolveLanguage,version:D.version,featureCount:D.features.length,projects:D.projects.length});
})();

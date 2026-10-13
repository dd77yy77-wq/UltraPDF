// Runs index.html's script against a hand-made DOM stub, a fake Android bridge and a fake pdf.js.
// usage: node tests/ui_test.js assets/index.html
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*?)<\/script>/)[1];

const timers = []; let now = 1000;
global.setTimeout = (f, ms) => { const t = { f, at: now + (ms || 0), id: timers.length + 1 }; timers.push(t); return t.id; };
global.clearTimeout = id => { const t = timers.find(x => x.id === id); if (t) t.f = null; };
global.requestAnimationFrame = f => global.setTimeout(f, 16);
Date.now = () => now;   // fake clock, advances only via runTimers
function runTimers(ms) { const end = now + ms; for (;;) { const c = timers.filter(t => t.f && t.at <= end).sort((a, b) => a.at - b.at)[0]; if (!c) break; now = Math.max(now, c.at); const f = c.f; c.f = null; f(); } now = end; }
const flush = async () => { for (let k = 0; k < 12; k++) { await new Promise(r => setImmediate(r)); runTimers(10); } };

const calls = { render: [], szs: [], pick: 0, share: 0, save: 0, upd: 0, shareApp: 0, shareApk: 0, git: 0, search: [], getDocument: [], theme: [], brightness: [], fullscreen: [], print: 0, copy: [], openUrl: [], pw: [] };
let focused = null, window_;

function mkEl(tag, id) {
  const el = {
    tag, id, style: {}, children: [], dataset: {}, _l: {}, className: '', parentNode: null, _txt: '',
    clientWidth: 400, clientHeight: 700, scrollTop: 0, scrollLeft: 0, offsetTop: 0, offsetHeight: 600, value: '', max: '',
    classList: {
      s: new Set(), add(c) { this.s.add(c); }, remove(c) { this.s.delete(c); },
      toggle(c, f) { const on = f === undefined ? !this.s.has(c) : f; on ? this.s.add(c) : this.s.delete(c); }, contains(c) { return this.s.has(c); }
    },
    appendChild(c) {
      if (c.isFrag) { c.children.forEach(k => this.appendChild(k)); c.children = []; return c; }
      c.parentNode = this; this.children.push(c);
      if (this.id === 'pages') c.offsetTop = (this.children.length - 1) * 620;
      if (this.tag === 'head' && c.tag === 'script') setTimeout(() => { window_.pdfjsLib = fakeLib; c.onload && c.onload(); }, 5);
      return c;
    },
    addEventListener(t, f) { (this._l[t] = this._l[t] || []).push(f); },
    fire(t, ev) { (this._l[t] || []).forEach(f => f(Object.assign({ preventDefault() {}, stopPropagation() {}, touches: [], target: el }, ev))); },
    querySelector() { return null; }, setAttribute() {},
    removeAttribute(a) { if (a === 'src') this._src = ''; },
    remove() { if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(x => x !== this); this.parentNode = null; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 400, height: 700 }; }, focus() { focused = this; }, blur() {},
    set innerHTML(v) { this.children = []; }, get innerHTML() { return ''; },
    set textContent(v) { this._txt = String(v); }, get textContent() { return this._txt; },
  };
  if (tag === 'img') {
    el._src = '';
    Object.defineProperty(el, 'src', {
      get() { return this._src; },
      set(v) { this._src = v; const m = /\/p\/(\d+)\.jpg\?w=(\d+)&n=(\d)&g=(\d+)/.exec(v); if (m) { calls.render.push([+m[1], +m[2], +m[3], +m[4]]); } setTimeout(() => { if (this._src === v && this.onload) this.onload(); }, 5); }
    });
  }
  return el;
}

// ---- fake pdf.js
const outline = [
  { title: 'Глава 1', dest: [{ ref: 0 }, { name: 'Fit' }], items: [{ title: 'Раздел 1.1', dest: 'named', items: [] }] },
  { title: 'Сайт', url: 'https://site.example/', items: [] }
];
const fakeDoc = {
  getDestination: async n => (n === 'named' ? [{ ref: 7 }, { name: 'Fit' }] : null),
  getPageIndex: async ref => ref.ref,
  getOutline: async () => outline,
  getPage: async n => ({
    getViewport: () => ({ width: 595, height: 842, convertToViewportRectangle: r => [r[0], 842 - r[1], r[2], 842 - r[3]] }),
    getAnnotations: async () => (n === 1 ? [
      { subtype: 'Link', rect: [10, 700, 110, 720], dest: [{ ref: 5 }, { name: 'Fit' }] },
      { subtype: 'Link', rect: [10, 600, 110, 620], url: 'https://x.y/' },
      { subtype: 'Text', rect: [0, 0, 1, 1] }] : []),
    getTextContent: async () => ({ items: [{ str: 'Hello', hasEOL: true }, { str: 'World', hasEOL: false }] })
  })
};
const fakeLib = { GlobalWorkerOptions: {}, getDocument: opts => { calls.getDocument.push(opts); return { promise: Promise.resolve(fakeDoc) }; } };

const byId = {};
const ids = ['scroller', 'pages', 'title', 'emsg', 'slider', 'lbl', 'jump', 'jin', 'open2', 'night', 'nlbl', 'jgo', 'jx',
  'burger', 'scrim', 'm-pro', 'm-open', 'm-search', 'm-share', 'm-save', 'm-about', 'about', 'aver', 'a-upd', 'a-share', 'a-apk', 'a-git', 'a-close',
  'm-hist', 'hist', 'hlist', 'h-clear', 'h-close', 'm-bm', 'bm', 'bmlist', 'bm-add', 'bm-close', 'm-toc', 'toc', 'toclist', 'toc-close',
  'm-txt', 'txt', 'txtt', 'ttext', 'txt-copy', 'txt-close', 'm-print', 'm-look', 'look', 'bright', 'bright-auto', 'look-close',
  'm-fs', 'fsx', 'pass', 'passmsg', 'pwd', 'pass-ok', 'pass-no', 'sbar', 'sclose', 'sq', 'scount', 'sprev', 'snext'];
ids.forEach(i => byId[i] = mkEl('div', i));
[['seg-mode', ['0', '2', '1']], ['seg-theme', ['dark', 'light']]].forEach(([id, vals]) => {
  byId[id] = mkEl('div', id);
  vals.forEach(v => { const b = mkEl('button'); b.dataset.v = v; byId[id].appendChild(b); });
});
Object.defineProperty(byId.pages, 'clientWidth', { get() { return 400 * parseFloat(this.style.width || '100') / 100; } });
byId.emsg._txt = 'default message';
const body = mkEl('body'); const head = mkEl('head'); const docL = {};
global.document = {
  getElementById: i => byId[i], createElement: t => mkEl(t), createDocumentFragment: () => Object.assign(mkEl('frag'), { isFrag: true }),
  body, head, addEventListener(t, f) { docL[t] = f; }
};
global.window = window_ = { devicePixelRatio: 3.5, addEventListener() {}, Android: null };
const store = {}; global.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };

// ---- fake Android bridge
const hist = [{ n: 'primary:Download/Один.pdf', t: 1760400000000 }, { n: 'Два.pdf', t: 1760300000000 }];
const bridge = { g: 0, err: 0, n: 0, nm: '', text: {}, unsup: false };
window.Android = {
  gen: () => bridge.g, err: () => bridge.err, count: () => bridge.n, name: () => bridge.nm,
  szs(from, n) { calls.szs.push([from, n]); let s = ''; for (let i = from; i < from + n && i < bridge.n; i++) s += '595,' + (i % 2 ? 800 : 842) + ';'; return s; },
  searchPage(i, q) { calls.search.push(i); if (bridge.unsup) return '!'; return bridge.text[i] || ''; },
  pick: () => { calls.pick++; }, share: () => { calls.share++; }, saveAs: () => { calls.save++; },
  history: () => JSON.stringify(hist), openHistory: i => { calls.hopen = i; }, clearHistory: () => { hist.length = 0; calls.hclear = (calls.hclear || 0) + 1; },
  version: () => '9.9', checkUpdate: () => { calls.upd++; }, shareApp: () => { calls.shareApp++; }, shareApk: () => { calls.shareApk++; }, github: () => { calls.git++; },
  theme: l => calls.theme.push(l), brightness: p => calls.brightness.push(p), fullscreen: f => calls.fullscreen.push(f),
  print: () => { calls.print++; }, copy: t => calls.copy.push(t), openUrl: u => calls.openUrl.push(u), openWithPassword: pw => { calls.pw.push(pw); }
};
const TB = 40;   // bars are shown: goTo leaves room under the top bar

let failed = 0; const ok = (c, m) => { console.log((c ? '  PASS ' : '  FAIL ') + m); if (!c) failed++; };
const lks = i => byId.pages.children[i].children.filter(c => c.className === 'lk');
const last = () => calls.render.slice(-1)[0];

(async () => {
console.log('== script loads and starts polling');
new Function('window', 'document', 'localStorage', 'setTimeout', 'clearTimeout', 'requestAnimationFrame', 'Date', js)(
  window, document, localStorage, setTimeout, clearTimeout, requestAnimationFrame, Date);
runTimers(500);
ok(!body.classList.contains('hasdoc'), 'no document yet, empty screen');
ok(!body.classList.contains('pro'), 'normal mode by default');
ok(calls.theme.slice(-1)[0] === false && calls.brightness.slice(-1)[0] === 0, 'dark theme and system brightness applied at start');
byId.open2.fire('click', {}); ok(calls.pick === 1, 'Выбрать PDF calls Android.pick()');

console.log('== hamburger menu');
byId.burger.fire('click', {}); ok(body.classList.contains('menu'), 'burger opens the menu');
byId['m-open'].fire('click', {}); ok(calls.pick === 2 && !body.classList.contains('menu'), 'menu "Открыть" picks a file and closes the menu');
byId.burger.fire('click', {}); byId.scrim.fire('click', {}); ok(!body.classList.contains('menu'), 'tap outside closes the menu');

console.log('== a document appears (generation 1, 40 pages)');
bridge.g = 1; bridge.n = 40; bridge.nm = 'primary:Download/Отчёт.pdf';
runTimers(1000); await flush();
ok(body.classList.contains('hasdoc'), 'viewer shown');
ok(byId.title.textContent === 'Отчёт.pdf', 'title is the clean file name: ' + byId.title.textContent);
ok(byId.pages.children.length === 40, '40 page placeholders');
ok(byId.slider.max === '40', 'slider max = 40');
ok(byId.lbl.textContent === '1 / 40', 'label "1 / 40"');
ok(calls.szs.length === 3 && calls.szs[1][0] === 1 && calls.szs[2][0] === 33, 'sizes read in batches of 32: ' + JSON.stringify(calls.szs));
ok(byId.pages.children[1].style.paddingBottom && Math.abs(parseFloat(byId.pages.children[1].style.paddingBottom) - 800 / 595 * 100) < 0.01, 'page 2 aspect from its own size');
ok(calls.render.length > 0 && calls.render.length <= 12, 'pages requested lazily: ' + calls.render.length + ' of 40');
ok(calls.render.every(([i, w, n, g]) => w >= 640 && w <= 2400 && w % 64 === 0 && n === 0 && g === 1), 'widths sane, day mode, generation 1');
ok(calls.render[0][0] === 0, 'nearest page (the first) requested first');
const p0 = byId.pages.children[0];
const img0 = p0.children.find(c => c.tag === 'img');
ok(img0 && /^https:\/\/pdf\.local\/p\/0\.jpg\?w=\d+&n=0&g=1$/.test(img0.src), 'image src is a native pdf.local URL: ' + (img0 && img0.src));
ok(p0.classList.contains('on'), 'placeholder hidden once the image loaded');
ok(calls.getDocument.length === 0, 'normal mode: pdf.js is not even loaded');

console.log('== scrolling updates label and requests new pages');
byId.scroller.scrollTop = 620 * 20 + 100; byId.scroller.fire('scroll', {}); runTimers(1500);
ok(byId.lbl.textContent === '21 / 40', 'label follows scroll: ' + byId.lbl.textContent);
ok(calls.render.some(([i]) => i === 20), 'page 21 requested after scrolling');
const stored = Object.keys(store).filter(k => k.startsWith('pos:'));
ok(stored.length === 1 && JSON.parse(store[stored[0]]).i === 20, 'position saved');

console.log('== slider and jump dialog');
byId.slider.value = '5'; byId.slider.fire('input', {}); runTimers(100); byId.slider.fire('change', {}); runTimers(300);
ok(byId.scroller.scrollTop === 620 * 4 - TB, 'slider jumps to page 5, top lands under the bar');
byId.lbl.fire('click', {}); ok(byId.jump.classList.contains('on'), 'jump dialog opens');
byId.jin.value = '33'; byId.jgo.fire('click', {});
ok(byId.scroller.scrollTop === 620 * 32 - TB && !byId.jump.classList.contains('on'), 'jump to page 33');
byId.lbl.fire('click', {}); byId.jin.value = '999'; byId.jgo.fire('click', {});
ok(byId.scroller.scrollTop === 620 * 32 - TB, 'out-of-range number ignored');

console.log('== pinch zoom');
const t = (x1, y1, x2, y2) => [{ clientX: x1, clientY: y1 }, { clientX: x2, clientY: y2 }];
byId.scroller.fire('touchstart', { touches: t(100, 300, 200, 300) });
byId.scroller.fire('touchmove', { touches: t(90, 300, 210, 300) });
byId.scroller.fire('touchmove', { touches: t(50, 300, 250, 300) });
runTimers(50);
ok(byId.pages.style.width === '200%', 'pinch x2 -> pages width 200% (one relayout per frame)');
byId.scroller.fire('touchmove', { touches: t(0, 300, 1000, 300) }); runTimers(50);
ok(byId.pages.style.width === '500%', 'zoom capped at 5x');
byId.scroller.fire('touchend', { touches: [] });
const before = calls.render.length; byId.scroller.scrollTop = 620 * 10; byId.scroller.fire('scroll', {}); runTimers(1500);
ok(calls.render.slice(before).some(([i, w]) => w === 2400), 'after zoom, visible pages re-requested at the capped 2400px');

console.log('== double tap / single tap');
now += 1000;
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(100);
byId.scroller.fire('click', { clientX: 202, clientY: 301 });
ok(byId.pages.style.width === '100%', 'double tap while zoomed in returns to 1x');
now += 1000;
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(100);
byId.scroller.fire('click', { clientX: 200, clientY: 300 });
ok(byId.pages.style.width === '250%', 'double tap at 1x zooms to 2.5x');
now += 1000;
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(100);
byId.scroller.fire('click', { clientX: 200, clientY: 300 });
ok(byId.pages.style.width === '100%', 'and back to 1x again');
now += 1000; runTimers(10);
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(500);
ok(body.classList.contains('nobars'), 'single tap hides both bars (one class drives top and bottom)');
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(500);
ok(!body.classList.contains('nobars'), 'single tap shows them again');
byId.burger.fire('click', {}); byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(500);
ok(!body.classList.contains('menu') && !body.classList.contains('nobars'), 'tap on the page while the menu is open only closes the menu');

console.log('== night mode (pages re-requested inverted by the app)');
byId.night.fire('click', {});
ok(body.classList.contains('night') && store.mode === '1' && byId.nlbl.textContent === 'Дневной режим', 'night on, remembered, menu label flips');
const b2 = calls.render.length; runTimers(1500);
const nr = calls.render.slice(b2);
ok(nr.length > 0 && nr.every(r => r[2] === 1), 'visible pages re-requested with n=1');
byId.night.fire('click', {}); runTimers(1500);
ok(!body.classList.contains('night') && last()[2] === 0, 'and back to day');

console.log('== search');
bridge.text[3] = '10,20,50,32|60,100,90,112;95,100,120,112';
bridge.text[30] = '5,700,40,712';
byId.scroller.scrollTop = 620 * 25; byId.scroller.fire('scroll', {}); runTimers(200);
byId['m-search'].fire('click', {});
ok(body.classList.contains('searching') && focused === byId.sq, 'search bar opens and takes focus');
byId.sq.value = 'abc'; byId.sq.fire('input', {}); runTimers(300);
ok(calls.search.length === 0, 'typing is debounced');
runTimers(1000);
ok(calls.search.length >= 40, 'all pages scanned (' + calls.search.length + ')');
ok(/^\d\/3$/.test(byId.scount.textContent), 'count shown: ' + byId.scount.textContent);
const hl3 = byId.pages.children[3].children.filter(c => String(c.className).startsWith('hl'));
ok(hl3.length === 3, 'page 4 has 3 highlight rects: ' + hl3.length);
ok(Math.abs(parseFloat(hl3[0].style.left) - 10 / 595 * 100) < 0.01, 'rect placed in % of the page');
const cur0 = byId.scount.textContent;
byId.snext.fire('click', {}); const c1 = byId.scount.textContent;
byId.snext.fire('click', {}); byId.snext.fire('click', {}); const c3 = byId.scount.textContent;
ok(cur0 !== c1 && c3 === cur0, 'next steps through matches and wraps around: ' + [cur0, c1, c3]);
byId.sprev.fire('click', {}); ok(byId.scount.textContent !== c3, 'previous goes back');
byId.sclose.fire('click', {});
ok(!body.classList.contains('searching') && !byId.pages.children.some(p => p.children.some(c => String(c.className).startsWith('hl'))), 'closing search clears highlights');
bridge.unsup = true;
byId['m-search'].fire('click', {}); byId.sq.value = 'zzz'; byId.sq.fire('input', {}); runTimers(1500);
ok(/Android 15/.test(byId.scount.textContent), 'old Android: clear message instead of a broken search');
byId.sclose.fire('click', {}); bridge.unsup = false;

console.log('== history');
byId['m-hist'].fire('click', {});
ok(byId.hist.classList.contains('on') && !body.classList.contains('menu'), 'history dialog opens, menu closes');
ok(byId.hlist.children.length === 2, 'two entries listed');
const e0 = byId.hlist.children[0];
ok(e0.children[0].textContent === 'Один.pdf' && /^\d\d\.\d\d\.\d{4} \d\d:\d\d$/.test(e0.children[1].textContent), 'entry shows the clean name and a date');
byId.hlist.children[1].fire('click', {});
ok(calls.hopen === 1 && !byId.hist.classList.contains('on'), 'tapping an entry asks the app to open it and closes the dialog');
byId['m-hist'].fire('click', {}); byId['h-clear'].fire('click', {});
ok(calls.hclear === 1 && byId.hlist.children.length === 1 && /пусто/.test(byId.hlist.children[0].textContent), 'clear empties the list');
byId['h-close'].fire('click', {}); ok(!byId.hist.classList.contains('on'), 'close');

console.log('== share / save / about');
byId['m-share'].fire('click', {}); ok(calls.share === 1, 'menu "Поделиться" -> Android.share()');
byId['m-save'].fire('click', {}); ok(calls.save === 1, 'menu "Сохранить как" -> Android.saveAs()');
byId['m-about'].fire('click', {});
ok(byId.about.classList.contains('on') && byId.aver.textContent === 'Версия 9.9', 'about dialog shows the version');
byId['a-share'].fire('click', {}); byId['a-git'].fire('click', {}); ok(calls.shareApp === 1 && calls.git === 1, 'share app and GitHub buttons call the app');
byId['a-apk'].fire('click', {}); ok(calls.shareApk === 1, 'send APK file button calls the app');
byId['a-upd'].fire('click', {}); ok(calls.upd === 1 && !byId.about.classList.contains('on'), 'update check calls the app and closes the dialog');

console.log('== normal mode ignores Pro-only look settings');
store.mode = '2'; store.theme = 'light'; store.bright = '70';
byId.night.fire('click', {}); byId.night.fire('click', {});   // re-evaluate
ok(!body.classList.contains('sepia') && !body.classList.contains('light'), 'sepia and light theme are not applied without Pro');
ok(calls.brightness.slice(-1)[0] === 0, 'brightness stays as in the system without Pro');
store.mode = '0'; byId.night.fire('click', {}); byId.night.fire('click', {}); runTimers(1500);

console.log('== Pro mode');
byId['m-pro'].fire('click', {});
ok(body.classList.contains('pro') && store.pro === '1', 'Pro on, remembered');
await flush();
ok(calls.getDocument.length === 1 && calls.getDocument[0].url === '/doc/1.pdf?g=1', 'pdf.js opened the streamed document once: ' + JSON.stringify(calls.getDocument.map(c => c.url)));
ok(calls.getDocument[0].disableAutoFetch === true && calls.getDocument[0].rangeChunkSize > 0, 'only the needed parts are fetched (ranges)');
ok(window_.pdfjsLib === fakeLib, 'pdf.js script was added once');
byId.scroller.scrollTop = 0; byId.scroller.fire('scroll', {}); runTimers(1500); await flush();
ok(lks(0).length === 2, 'links painted on page 1 (the non-link annotation is ignored): ' + lks(0).length);
ok(Math.abs(parseFloat(lks(0)[0].style.left) - 10 / 595 * 100) < 0.01 && Math.abs(parseFloat(lks(0)[0].style.top) - 122 / 842 * 100) < 0.01, 'link placed in % of the page');
lks(0)[0].fire('click', {}); await flush();
ok(byId.scroller.scrollTop === 620 * 5 - TB, 'internal link scrolls to its page: ' + byId.scroller.scrollTop);
lks(0)[1].fire('click', {});
ok(calls.openUrl.length === 1 && calls.openUrl[0] === 'https://x.y/', 'external link goes to the app');

console.log('== bookmarks');
byId.scroller.scrollTop = 620 * 8; byId.scroller.fire('scroll', {}); runTimers(300);
byId['m-bm'].fire('click', {});
ok(byId.bm.classList.contains('on') && /Добавить.*стр\. 9/.test(byId['bm-add'].textContent), 'dialog offers to bookmark page 9');
byId['bm-add'].fire('click', {});
ok(JSON.parse(store['bm:Отчёт.pdf:40']).join() === '8' && /Убрать/.test(byId['bm-add'].textContent), 'bookmark saved per document');
byId.bmlist.children[0].children[0].fire('click', {});
ok(byId.scroller.scrollTop === 620 * 8 - TB && !byId.bm.classList.contains('on'), 'tapping a bookmark jumps to its page');
byId['m-bm'].fire('click', {}); byId.bmlist.children[0].children[1].fire('click', {});
ok(JSON.parse(store['bm:Отчёт.pdf:40']).length === 0 && /Закладок пока нет/.test(byId.bmlist.children[0].textContent), 'delete button removes it');
byId['bm-close'].fire('click', {});

console.log('== table of contents');
byId['m-toc'].fire('click', {});
ok(byId.toc.classList.contains('on') && /Загрузка/.test(byId.toclist.children[0].textContent), 'dialog opens with a loading message');
await flush();
ok(byId.toclist.children.length === 3 && byId.toclist.children[1].style.paddingLeft === '26px', 'three entries, the nested one is indented');
byId.toclist.children[1].fire('click', {}); await flush();
ok(byId.scroller.scrollTop === 620 * 7 - TB && !byId.toc.classList.contains('on'), 'named destination resolved, jumped to page 8');
byId['m-toc'].fire('click', {}); await flush(); byId.toclist.children[2].fire('click', {});
ok(calls.openUrl.slice(-1)[0] === 'https://site.example/', 'url entry opens the link');
byId['toc-close'].fire('click', {});

console.log('== page text');
byId.scroller.scrollTop = 620 * 3; byId.scroller.fire('scroll', {}); runTimers(300);
byId['m-txt'].fire('click', {}); await flush();
ok(byId.txt.classList.contains('on') && byId.txtt.textContent === 'Текст страницы 4' && byId.ttext.value === 'Hello\nWorld', 'text of the current page: ' + JSON.stringify(byId.ttext.value));
byId['txt-copy'].fire('click', {});
ok(calls.copy.length === 1 && calls.copy[0] === 'Hello\nWorld', 'copy goes through the app clipboard');
byId['txt-close'].fire('click', {});

console.log('== print');
byId['m-print'].fire('click', {}); ok(calls.print === 1, 'menu "Печать" -> Android.print()');

console.log('== look: reading mode, theme, brightness');
byId['m-look'].fire('click', {}); ok(byId.look.classList.contains('on'), 'dialog opens');
const segM = byId['seg-mode'].children, segT = byId['seg-theme'].children;
const b3 = calls.render.length; segM[1].fire('click', {}); runTimers(1500);
ok(body.classList.contains('sepia') && store.mode === '2' && segM[1].classList.contains('on'), 'sepia selected and remembered');
ok(calls.render.slice(b3).length > 0 && calls.render.slice(b3).every(r => r[2] === 2), 'pages re-requested with n=2');
segT[1].fire('click', {});
ok(body.classList.contains('light') && calls.theme.slice(-1)[0] === true && store.theme === 'light', 'light theme applied, system bars told');
byId.bright.value = '70'; byId.bright.fire('input', {});
ok(calls.brightness.slice(-1)[0] === 70 && store.bright === '70', 'brightness 70% applied');
byId['bright-auto'].fire('click', {}); ok(calls.brightness.slice(-1)[0] === 0, 'brightness back to the system value');
byId['look-close'].fire('click', {}); ok(!byId.look.classList.contains('on'), 'dialog closes');

console.log('== full screen');
byId['m-fs'].fire('click', {});
ok(body.classList.contains('fs') && calls.fullscreen.slice(-1)[0] === true, 'full screen on, app told');
now += 1000; runTimers(10);
byId.scroller.fire('click', { clientX: 200, clientY: 300 }); runTimers(500);
ok(body.classList.contains('fs') && !body.classList.contains('nobars'), 'tapping the page does not change anything in full screen');
byId.fsx.fire('click', {});
ok(!body.classList.contains('fs') && calls.fullscreen.slice(-1)[0] === false, 'the cross leaves full screen');
byId['m-fs'].fire('click', {});
ok(window_.onBack() === true && !body.classList.contains('fs'), 'Back leaves full screen first');

console.log('== Back button closes the topmost thing');
ok(window_.onBack() === false, 'nothing open -> the app may leave');
byId.burger.fire('click', {}); ok(window_.onBack() === true && !body.classList.contains('menu'), 'closes the menu');
byId['m-about'].fire('click', {}); ok(window_.onBack() === true && !byId.about.classList.contains('on'), 'closes a dialog');
byId['m-search'].fire('click', {}); ok(window_.onBack() === true && !body.classList.contains('searching'), 'closes the search bar');

console.log('== password');
bridge.g = 2; bridge.err = 2; bridge.n = 0; bridge.nm = 'x/Секрет.pdf';
runTimers(1000); await flush();
ok(byId.pass.classList.contains('on') && /Секрет\.pdf/.test(byId.passmsg.textContent), 'password dialog for a protected document');
byId.pwd.value = 'bad'; byId['pass-ok'].fire('click', {});
ok(calls.pw.join() === 'bad' && !byId.pass.classList.contains('on'), 'password sent to the app');
bridge.g = 3; bridge.err = 3; runTimers(1000); await flush();
ok(byId.pass.classList.contains('on') && /Неверный/.test(byId.passmsg.textContent), 'wrong password: asked again');
byId.pwd.value = 'good'; byId['pwd'].fire('keydown', { key: 'Enter' });
ok(calls.pw.join() === 'bad,good', 'Enter submits too');
bridge.g = 4; bridge.err = 0; bridge.n = 5; runTimers(1000); await flush();
ok(body.classList.contains('hasdoc') && !byId.pass.classList.contains('on') && byId.title.textContent === 'Секрет.pdf', 'document opens after the right password');
byId.scroller.fire('scroll', {}); runTimers(1500); await flush();
ok(calls.getDocument.slice(-1)[0].password === 'good' && calls.getDocument.slice(-1)[0].url === '/doc/4.pdf?g=4', 'pdf.js gets the password and the new generation: ' + JSON.stringify(calls.getDocument.slice(-1)[0]));
bridge.g = 5; bridge.err = 2; bridge.n = 0; runTimers(1000); await flush();
byId['pass-no'].fire('click', {});
ok(!body.classList.contains('hasdoc') && /нужен пароль/.test(byId.emsg.textContent), 'cancel leaves the start screen with a hint');

console.log('== turning Pro off');
bridge.g = 6; bridge.err = 0; bridge.n = 12; bridge.nm = '1234/Новый.pdf'; runTimers(1000); await flush();
byId['m-fs'].fire('click', {});
const b4 = calls.render.length;
byId['m-pro'].fire('click', {}); runTimers(1500);
ok(!body.classList.contains('pro') && !body.classList.contains('fs') && calls.fullscreen.slice(-1)[0] === false, 'Pro off also leaves full screen');
ok(!body.classList.contains('sepia') && !body.classList.contains('light') && calls.theme.slice(-1)[0] === false && calls.brightness.slice(-1)[0] === 0, 'look returns to the normal defaults');
ok(calls.render.slice(b4).length > 0 && calls.render.slice(b4).every(r => r[2] === 0), 'pages re-requested as plain day pages');

console.log('== new document: position restored, old state cleared');
store['pos:Новый2.pdf:12'] = JSON.stringify({ i: 7, f: 0.5 });
bridge.g = 7; bridge.n = 12; bridge.nm = '1234/Новый2.pdf';
runTimers(2000); await flush();
ok(byId.pages.children.length === 12 && byId.lbl.textContent.endsWith('/ 12'), '12 pages after opening the next file');
ok(byId.scroller.scrollTop === 620 * 7 + 300, 'restored to page 8, half way: ' + byId.scroller.scrollTop);
ok(calls.render.slice(-3).every(r => r[3] === 7), 'new requests carry the new generation');

console.log('== errors');
bridge.g = 8; bridge.err = 1; bridge.n = 0; runTimers(800);
ok(!body.classList.contains('hasdoc') && byId.emsg.className.includes('err') && /защищ/.test(byId.emsg.textContent), 'error screen with hint');
bridge.g = 9; bridge.err = 0; bridge.n = 0; runTimers(800);
ok(/нет страниц/.test(byId.emsg.textContent), 'empty document message');
bridge.g = 10; bridge.n = 3; bridge.nm = '77'; runTimers(800);
ok(body.classList.contains('hasdoc') && byId.title.textContent === 'Документ', 'numeric provider id shown as "Документ"');

console.log('== evict keeps memory bounded');
bridge.g = 11; bridge.n = 60; bridge.nm = 'big.pdf'; runTimers(1500);
for (let p = 0; p < 60; p += 6) { byId.scroller.scrollTop = 620 * p; byId.scroller.fire('scroll', {}); runTimers(800); }
const live = byId.pages.children.filter(c => c.children.some(k => k.tag === 'img' && k.src)).length;
ok(live <= 12, 'decoded pages kept in memory: ' + live);

console.log('\nFAILED: ' + failed); process.exit(failed ? 1 : 0);
})().catch(e => { console.log('ERROR', e); process.exit(1); });

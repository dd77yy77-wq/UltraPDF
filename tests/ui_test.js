// Runs index.html's script against a hand-made DOM stub and a fake Android bridge.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*?)<\/script>/)[1];

const timers = []; let now = 1000;
global.setTimeout = (f, ms) => { const t = { f, at: now + (ms || 0), id: timers.length + 1 }; timers.push(t); return t.id; };
global.clearTimeout = id => { const t = timers.find(x => x.id === id); if (t) t.f = null; };
global.requestAnimationFrame = f => global.setTimeout(f, 16);
const realNow = Date.now; Date.now = () => now;   // fake clock, advances only via runTimers
function runTimers(ms) { const end = now + ms; for (;;) { const c = timers.filter(t => t.f && t.at <= end).sort((a, b) => a.at - b.at)[0]; if (!c) break; now = Math.max(now, c.at); const f = c.f; c.f = null; f(); } now = end; }

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
      return c;
    },
    addEventListener(t, f) { (this._l[t] = this._l[t] || []).push(f); },
    fire(t, ev) { (this._l[t] || []).forEach(f => f(Object.assign({ preventDefault() {}, touches: [], target: el }, ev))); },
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
let focused = null;
const calls = { render: [], szs: [], pick: 0, share: 0, save: 0, upd: 0, shareApp: 0, git: 0, search: [] };
const byId = {};
const ids = ['scroller', 'pages', 'title', 'emsg', 'slider', 'lbl', 'jump', 'jin', 'open2', 'night', 'nlbl', 'jgo', 'jx',
  'burger', 'scrim', 'm-open', 'm-search', 'm-share', 'm-save', 'm-about', 'about', 'aver', 'a-upd', 'a-share', 'a-git', 'a-close',
  'sbar', 'sclose', 'sq', 'scount', 'sprev', 'snext'];
ids.forEach(i => byId[i] = mkEl('div', i));
Object.defineProperty(byId.pages, 'clientWidth', { get() { return 400 * parseFloat(this.style.width || '100') / 100; } });
byId.emsg._txt = 'default message';
const body = mkEl('body'); const docL = {};
global.document = {
  getElementById: i => byId[i], createElement: t => mkEl(t), createDocumentFragment: () => Object.assign(mkEl('frag'), { isFrag: true }),
  body, addEventListener(t, f) { docL[t] = f; }
};
global.window = { devicePixelRatio: 3.5, addEventListener() {}, Android: null };
const store = {}; global.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };

// ---- fake Android bridge
const bridge = { g: 0, err: 0, n: 0, nm: '', text: {}, unsup: false };
window.Android = {
  gen: () => bridge.g, err: () => bridge.err, count: () => bridge.n, name: () => bridge.nm,
  szs(from, n) { calls.szs.push([from, n]); let s = ''; for (let i = from; i < from + n && i < bridge.n; i++) s += '595,' + (i % 2 ? 800 : 842) + ';'; return s; },
  searchPage(i, q) { calls.search.push(i); if (bridge.unsup) return '!'; return bridge.text[i] || ''; },
  pick: () => { calls.pick++; }, share: () => { calls.share++; }, saveAs: () => { calls.save++; },
  version: () => '9.9', checkUpdate: () => { calls.upd++; }, shareApp: () => { calls.shareApp++; }, github: () => { calls.git++; }
};
const TB = 40;   // bars are shown: goTo leaves room under the top bar

let failed = 0; const ok = (c, m) => { console.log((c ? '  PASS ' : '  FAIL ') + m); if (!c) failed++; };

console.log('== script loads and starts polling');
new Function('window', 'document', 'localStorage', 'setTimeout', 'clearTimeout', 'requestAnimationFrame', 'Date', js)(
  window, document, localStorage, setTimeout, clearTimeout, requestAnimationFrame, Date);
runTimers(500);
ok(!body.classList.contains('hasdoc'), 'no document yet, empty screen');
byId.open2.fire('click', {}); ok(calls.pick === 1, 'Выбрать PDF calls Android.pick()');

console.log('== hamburger menu');
byId.burger.fire('click', {}); ok(body.classList.contains('menu'), 'burger opens the menu');
byId['m-open'].fire('click', {}); ok(calls.pick === 2 && !body.classList.contains('menu'), 'menu "Открыть" picks a file and closes the menu');
byId.burger.fire('click', {}); byId.scrim.fire('click', {}); ok(!body.classList.contains('menu'), 'tap outside closes the menu');

console.log('== a document appears (generation 1, 40 pages)');
bridge.g = 1; bridge.n = 40; bridge.nm = 'primary:Download/Отчёт.pdf';
runTimers(1000);
ok(body.classList.contains('hasdoc'), 'viewer shown');
ok(byId.title.textContent === 'Отчёт.pdf', 'title is the clean file name: ' + byId.title.textContent);
ok(byId.pages.children.length === 40, '40 page placeholders');
ok(byId.slider.max === '40', 'slider max = 40');
ok(byId.lbl.textContent === '1 / 40', 'label "1 / 40"');
ok(calls.szs.length === 3 && calls.szs[1][0] === 1 && calls.szs[2][0] === 33, 'sizes read in batches of 32, not per page: ' + JSON.stringify(calls.szs));
ok(byId.pages.children[1].style.paddingBottom && Math.abs(parseFloat(byId.pages.children[1].style.paddingBottom) - 800 / 595 * 100) < 0.01, 'page 2 aspect from its own size');
ok(calls.render.length > 0, 'pages requested lazily: ' + JSON.stringify(calls.render.slice(0, 4)));
ok(calls.render.every(([i, w, n, g]) => w >= 640 && w <= 2400 && w % 64 === 0 && n === 0 && g === 1), 'widths are sane multiples of 64, day mode, generation 1: ' + [...new Set(calls.render.map(r => r[1]))]);
ok(calls.render[0][0] === 0, 'nearest page (the first) requested first');
ok(calls.render.length <= 12, 'only pages near the viewport requested (' + calls.render.length + ' of 40)');
const p0 = byId.pages.children[0];
const img0 = p0.children.find(c => c.tag === 'img');
ok(img0 && /^https:\/\/pdf\.local\/p\/0\.jpg\?w=\d+&n=0&g=1$/.test(img0.src), 'image src is a native pdf.local URL (no base64): ' + (img0 && img0.src));
ok(p0.classList.contains('on'), 'placeholder hidden once the image loaded');

console.log('== scrolling updates label and requests new pages');
byId.scroller.scrollTop = 620 * 20 + 100;
byId.scroller.fire('scroll', {});
runTimers(1500);
ok(byId.lbl.textContent === '21 / 40', 'label follows scroll: ' + byId.lbl.textContent);
ok(calls.render.some(([i]) => i === 20), 'page 21 requested after scrolling');
const stored = Object.keys(store).filter(k => k.startsWith('pos:'));
ok(stored.length === 1 && JSON.parse(store[stored[0]]).i === 20, 'position saved: ' + JSON.stringify(store));

console.log('== slider and jump dialog');
byId.slider.value = '5'; byId.slider.fire('input', {}); runTimers(100); byId.slider.fire('change', {}); runTimers(300);
ok(byId.scroller.scrollTop === 620 * 4 - TB, 'slider jumps to page 5, top lands under the bar: ' + byId.scroller.scrollTop);
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
ok(byId.pages.style.width === '200%', 'pinch x2 -> pages width 200% (one relayout per frame): ' + byId.pages.style.width);
byId.scroller.fire('touchmove', { touches: t(0, 300, 1000, 300) });
runTimers(50);
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
ok(body.classList.contains('night') && store.night === '1' && byId.nlbl.textContent === 'Дневной режим', 'night on, remembered, menu label flips');
const b2 = calls.render.length; runTimers(1500);
const nr = calls.render.slice(b2);
ok(nr.length > 0 && nr.every(r => r[2] === 1), 'visible pages re-requested with n=1: ' + JSON.stringify(nr.slice(0, 3)));

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
ok(byId.scount.textContent === '1/3' || /^\d\/3$/.test(byId.scount.textContent), 'count shown: ' + byId.scount.textContent);
const cm = byId.scount.textContent;
ok(Math.abs(byId.scroller.scrollTop - (620 * 30 + 700 / 842 * 600 - 700 * 0.3)) < 1 || cm === '1/3', 'jumped to the first match at/after the current page: scrollTop=' + byId.scroller.scrollTop + ' ' + cm);
const hl3 = byId.pages.children[3].children.filter(c => String(c.className).startsWith('hl'));
ok(hl3.length === 3, 'page 4 has 3 highlight rects (2 matches, one spans two lines): ' + hl3.length);
ok(Math.abs(parseFloat(hl3[0].style.left) - 10 / 595 * 100) < 0.01 && Math.abs(parseFloat(hl3[0].style.width) - 40 / 595 * 100) < 0.01, 'rect placed in % of the page');
const cur0 = byId.scount.textContent;
byId.snext.fire('click', {}); const c1 = byId.scount.textContent;
byId.snext.fire('click', {}); byId.snext.fire('click', {}); const c3 = byId.scount.textContent;
ok(cur0 !== c1 && c3 === cur0, 'next steps through matches and wraps around: ' + [cur0, c1, c3]);
byId.sprev.fire('click', {});
ok(byId.scount.textContent !== c3, 'previous goes back: ' + byId.scount.textContent);
ok(byId.pages.children.some(p => p.children.some(c => String(c.className) === 'hl cur')), 'current match is marked');
byId.sclose.fire('click', {});
ok(!body.classList.contains('searching') && !byId.pages.children.some(p => p.children.some(c => String(c.className).startsWith('hl'))), 'closing search clears highlights');
bridge.unsup = true;
byId['m-search'].fire('click', {}); byId.sq.value = 'zzz'; byId.sq.fire('input', {}); runTimers(1500);
ok(/Android 15/.test(byId.scount.textContent), 'old Android: clear message instead of a broken search: ' + byId.scount.textContent);
byId.sclose.fire('click', {}); bridge.unsup = false;

console.log('== share / save / about');
byId['m-share'].fire('click', {}); ok(calls.share === 1, 'menu "Поделиться" -> Android.share()');
byId['m-save'].fire('click', {}); ok(calls.save === 1, 'menu "Сохранить как" -> Android.saveAs()');
byId['m-about'].fire('click', {});
ok(byId.about.classList.contains('on') && byId.aver.textContent === 'Версия 9.9', 'about dialog shows the version');
byId['a-share'].fire('click', {}); byId['a-git'].fire('click', {}); ok(calls.shareApp === 1 && calls.git === 1, 'share app and GitHub buttons call the app');
byId['a-upd'].fire('click', {}); ok(calls.upd === 1 && !byId.about.classList.contains('on'), '"Проверить обновления" calls the app and closes the dialog');

console.log('== new document: position restored, old state cleared');
store['pos:Новый.pdf:12'] = JSON.stringify({ i: 7, f: 0.5 });
bridge.g = 2; bridge.n = 12; bridge.nm = '1234/Новый.pdf';
runTimers(2000);
ok(byId.pages.children.length === 12 && byId.lbl.textContent.endsWith('/ 12'), '12 pages after opening the second file');
ok(byId.scroller.scrollTop === 620 * 7 + 300, 'restored to page 8, half way: ' + byId.scroller.scrollTop);
ok(byId.lbl.textContent === '8 / 12', 'label 8 / 12');
ok(calls.render.slice(-3).every(r => r[3] === 2), 'new requests carry the new generation (stale ones are refused by the app)');

console.log('== errors');
bridge.g = 3; bridge.err = 1; bridge.n = 0; runTimers(800);
ok(!body.classList.contains('hasdoc') && byId.emsg.className.includes('err') && /защищ/.test(byId.emsg.textContent), 'error screen with hint');
bridge.g = 4; bridge.err = 0; bridge.n = 0; runTimers(800);
ok(/нет страниц/.test(byId.emsg.textContent), 'empty document message');
bridge.g = 5; bridge.n = 3; bridge.nm = '77'; runTimers(800);
ok(body.classList.contains('hasdoc') && byId.title.textContent === 'Документ', 'numeric provider id shown as "Документ"');

console.log('== evict keeps memory bounded');
bridge.g = 6; bridge.n = 60; bridge.nm = 'big.pdf'; runTimers(1500);
for (let p = 0; p < 60; p += 6) { byId.scroller.scrollTop = 620 * p; byId.scroller.fire('scroll', {}); runTimers(800); }
const live = byId.pages.children.filter(c => c.children.some(k => k.tag === 'img' && k.src)).length;
ok(live <= 12, 'decoded pages kept in memory: ' + live);

console.log('\nFAILED: ' + failed); process.exit(failed ? 1 : 0);

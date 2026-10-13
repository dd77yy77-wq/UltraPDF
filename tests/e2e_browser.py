#!/usr/bin/env python3
"""End-to-end check of the web half in real headless Chromium: the app side (assets, pages, document with Range)
is simulated by request routing, window.Android by an init script. Screenshots go to the directory given as argv[1]."""
import io, json, mimetypes, os, re, subprocess, sys, tempfile
from playwright.sync_api import sync_playwright
from PIL import Image, ImageOps

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ASSETS = os.path.join(ROOT, "assets")
PDF = os.path.join(ROOT, "tests", "fixture.pdf")
OUT = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
os.makedirs(OUT, exist_ok=True)
PDFBYTES = open(PDF, "rb").read()
requests_seen = {"range": [], "pages": []}

def page_jpeg(i, w, mode, rot=0):
    raw = subprocess.run(["pdftoppm", "-jpeg", "-jpegopt", "quality=85", "-f", str(i + 1), "-l", str(i + 1), "-scale-to-x", str(w), "-scale-to-y", "-1", PDF],
                         capture_output=True, check=True).stdout
    im = Image.open(io.BytesIO(raw)).convert("RGB")
    if rot: im = im.rotate(-rot, expand=True); im = im.resize((w, round(im.height * w / im.width)))
    if mode == 1: im = ImageOps.invert(im)
    elif mode == 2: im = ImageOps.colorize(ImageOps.grayscale(im), (0, 0, 0), (255, 237, 199))
    b = io.BytesIO(); im.save(b, "JPEG", quality=85); return b.getvalue()

def handler(route, request):
    from urllib.parse import urlparse, parse_qs
    u = urlparse(request.url); q = parse_qs(u.query); p = u.path
    if p.startswith("/app/"):
        f = os.path.join(ASSETS, p[5:])
        if not os.path.isfile(f): return route.fulfill(status=404, body="")
        ct = "text/html" if f.endswith(".html") else "application/javascript" if f.endswith(".js") else (mimetypes.guess_type(f)[0] or "application/octet-stream")
        return route.fulfill(status=200, body=open(f, "rb").read(), headers={"Content-Type": ct, "Cache-Control": "no-cache"})
    if p.startswith("/p/"):
        i = int(re.match(r"/p/(\d+)\.jpg", p).group(1)); w = int(q["w"][0]); n = int(q["n"][0]); rot = int(q.get("r", ["0"])[0])
        requests_seen["pages"].append((i, w, n))
        return route.fulfill(status=200, body=page_jpeg(i, w, n, rot), headers={"Content-Type": "image/jpeg", "Cache-Control": "no-store"})
    if p.startswith("/doc/"):
        size = len(PDFBYTES); rng = request.headers.get("range")
        requests_seen.setdefault("doc", []).append((request.method, rng))
        h = {"Content-Type": "application/pdf", "Accept-Ranges": "bytes", "Cache-Control": "no-store"}
        if rng and rng.startswith("bytes="):
            requests_seen["range"].append(rng)
            a, b = rng[6:].split("-")
            if a == "" and b != "": start, end = max(0, size - int(b)), size - 1
            else: start, end = int(a or 0), min(int(b) if b else size - 1, size - 1)
            h["Content-Range"] = "bytes %d-%d/%d" % (start, end, size); h["Content-Length"] = str(end - start + 1)
            return route.fulfill(status=206, body=PDFBYTES[start:end + 1], headers=h)
        h["Content-Length"] = str(size)
        return route.fulfill(status=200, body=PDFBYTES, headers=h)
    return route.fulfill(status=404, body="")

ANDROID = """
(function(){
  var calls = window.__calls = {};
  function rec(n,a){ (calls[n]=calls[n]||[]).push(a); }
  window.Android = {
    gen:function(){return 1}, err:function(){return 0}, count:function(){return 3}, name:function(){return 'primary:Download/Тестовый документ.pdf'},
    szs:function(f,n){var s='';for(var i=f;i<f+n&&i<3;i++)s+='595,842;';return s;},
    searchPage:function(i,q){ return (i===1 && q==='needle')? '120,86,152,100|230,86,262,100':''; },
    pick:function(){rec('pick',1)}, share:function(){rec('share',1)}, saveAs:function(){rec('saveAs',1)},
    history:function(){return JSON.stringify([{n:'primary:Download/Тестовый документ.pdf',t:Date.now()},{n:'Отчёт.pdf',t:Date.now()-86400000}])},
    openHistory:function(i){rec('openHistory',i)}, clearHistory:function(){rec('clearHistory',1)},
    version:function(){return '1.5'}, checkUpdate:function(){rec('checkUpdate',1)}, shareApp:function(){rec('shareApp',1)}, github:function(){rec('github',1)},
    theme:function(l){rec('theme',l)}, brightness:function(p){rec('brightness',p)}, fullscreen:function(f){rec('fullscreen',f)},
    print:function(){rec('print',1)}, copy:function(t){rec('copy',t)}, openUrl:function(u){rec('openUrl',u)}, openWithPassword:function(pw){rec('pw',pw)}
  };
})();
"""

fails = 0
def ok(c, m):
    global fails
    print(("  PASS " if c else "  FAIL ") + m)
    if not c: fails += 1

with sync_playwright() as pw:
    br = pw.chromium.launch()
    ctx = br.new_context(viewport={"width": 412, "height": 915}, device_scale_factor=2.5, is_mobile=True, has_touch=True, locale="ru-RU")
    ctx.route("https://pdf.local/**", handler)
    ctx.add_init_script(ANDROID)
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("console", lambda m: errors.append("console." + m.type + ": " + m.text) if m.type == "error" else None)
    page.goto("https://pdf.local/app/index.html")
    page.wait_for_function("document.body.classList.contains('hasdoc')", timeout=5000)
    page.wait_for_function("document.querySelectorAll('.pg.on').length >= 1", timeout=10000)
    page.wait_for_timeout(600)
    print("== document view")
    ok(page.evaluate("document.title") == "UltraPDF", "page title is UltraPDF")
    ok(page.evaluate("document.querySelector('#title').textContent") == "Тестовый документ.pdf", "file name in the top bar")
    ok(page.evaluate("document.querySelectorAll('.pg').length") == 3, "3 page placeholders")
    nat = page.evaluate("[...document.querySelectorAll('.pg img')].map(i => i.naturalWidth)")
    ok(len(nat) >= 1 and all(n > 500 for n in nat), "page images decoded from the native-style URL: %s" % nat)
    ok(not requests_seen["range"], "normal mode: the document is not fetched by pdf.js")
    page.screenshot(path=os.path.join(OUT, "1-reader.png"))
    bar_h = page.evaluate("document.querySelector('#top').getBoundingClientRect().height")
    ok(bar_h <= 41, "top bar is compact: %spx" % bar_h)

    print("== tap hides both bars smoothly")
    page.touchscreen.tap(206, 450); page.wait_for_timeout(430)
    mid = page.evaluate("getComputedStyle(document.querySelector('#top')).transform")
    page.wait_for_timeout(500)
    top_t = page.evaluate("document.querySelector('#top').getBoundingClientRect().bottom")
    bot_t = page.evaluate("document.querySelector('#bot').getBoundingClientRect().top")
    ok(top_t <= 1 and bot_t >= 914, "after a tap the top bar is above and the bottom bar is below the screen (top.bottom=%s bot.top=%s)" % (top_t, bot_t))
    ok(mid not in ("none", "matrix(1, 0, 0, 1, 0, 0)"), "bars animate (transform mid-transition: %s)" % mid)
    page.screenshot(path=os.path.join(OUT, "2-bars-hidden.png"))
    page.touchscreen.tap(206, 450); page.wait_for_timeout(600)
    ok(page.evaluate("document.querySelector('#top').getBoundingClientRect().top") >= 0, "second tap brings them back")

    print("== menu in the normal mode (as in 1.4)")
    page.click("#burger"); page.wait_for_timeout(300)
    vis = page.evaluate("[...document.querySelectorAll('#menu .mi')].filter(e => e.offsetParent !== null).map(e => e.textContent.trim())")
    ok(vis == ["Pro-режим", "Открыть", "История", "Ночной режим", "Поиск", "Поделиться", "Сохранить как…", "О приложении"], "normal menu: %s" % vis)
    page.screenshot(path=os.path.join(OUT, "3-menu-normal.png"))
    print("== Pro mode")
    page.click("#m-pro"); page.wait_for_timeout(300)
    vis = page.evaluate("[...document.querySelectorAll('#menu .mi')].filter(e => e.offsetParent !== null).map(e => e.textContent.trim())")
    ok(vis == ["Pro-режим", "Открыть", "История", "Ночной режим", "Поиск", "Поделиться", "Сохранить как…", "Закладки", "Оглавление", "Текст страницы", "Повернуть", "Печать", "Внешний вид", "Полный экран", "О приложении"], "Pro menu: %s" % vis)
    page.screenshot(path=os.path.join(OUT, "4-menu-pro.png"))
    page.click("#scrim", position={"x": 300, "y": 800}); page.wait_for_timeout(300)
    page.wait_for_function("window.__calls && true"); page.wait_for_timeout(1500)
    ok(len(requests_seen.get("doc", [])) >= 1, "pdf.js fetched the document through /doc/: %s" % requests_seen.get("doc", [])[:3])
    n_lk = page.evaluate("document.querySelectorAll('.pg')[0].querySelectorAll('a.lk').length")
    ok(n_lk == 2, "two links painted on page 1: %d" % n_lk)
    rects = page.evaluate("[...document.querySelectorAll('.pg')[0].querySelectorAll('a.lk')].map(a => { const r = a.getBoundingClientRect(), p = a.parentElement.getBoundingClientRect(); return [Math.round((r.left-p.left)/p.width*595), Math.round((r.top-p.top)/p.height*842), Math.round(r.width/p.width*595), Math.round(r.height/p.height*842)]; })")
    ok(any(abs(r[0] - 72) < 3 and abs(r[1] - 130) < 3 for r in rects), "link boxes sit where the PDF puts them: %s" % rects)
    page.screenshot(path=os.path.join(OUT, "5-pro-reader.png"))
    page.evaluate("document.querySelectorAll('.pg')[0].querySelectorAll('a.lk')[1].click()")
    ok(page.evaluate("window.__calls.openUrl") == ["https://example.com/"], "external link handed to the app")
    page.evaluate("[...document.querySelectorAll('.pg')[0].querySelectorAll('a.lk')].find(a => parseFloat(a.style.top) < 17).click()")
    page.wait_for_timeout(700)
    st = page.evaluate("document.querySelector('#scroller').scrollTop"); off = page.evaluate("document.querySelectorAll('.pg')[2].offsetTop")
    mx = page.evaluate("document.querySelector('#scroller').scrollHeight - document.querySelector('#scroller').clientHeight")
    ok(abs(st - min(off - 40, mx)) < 3, "internal link scrolled to page 3 (scrollTop %s, page 3 at %s, max %s)" % (st, off, mx))

    print("== table of contents, text, bookmarks")
    page.click("#burger"); page.click("#m-toc"); page.wait_for_function("document.querySelectorAll('#toclist .ti').length >= 3", timeout=8000)
    items = page.evaluate("[...document.querySelectorAll('#toclist .ti')].map(e => e.textContent)")
    ok(len(items) == 3 and 'Intro' in items[0], "outline listed: %s" % items)
    page.screenshot(path=os.path.join(OUT, "6-toc.png"))
    page.click("#toclist .ti:nth-child(2)"); page.wait_for_timeout(800)
    ok(page.evaluate("document.querySelector('#toc').classList.contains('on')") is False, "tapping an entry closes the dialog")
    cur = page.evaluate("document.querySelector('#lbl').textContent")
    ok(cur.startswith("2 /") or cur.startswith("1 /"), "and moves to the chapter page: %s" % cur)
    page.click("#burger"); page.click("#m-txt"); page.wait_for_function("document.querySelector('#ttext').value.length > 5 && !document.querySelector('#ttext').value.startsWith('Загр')", timeout=8000)
    tx = page.evaluate("document.querySelector('#ttext').value")
    ok("needle" in tx or "Second" in tx or "Hello" in tx or "Third" in tx, "page text shown: %r" % tx[:60])
    page.screenshot(path=os.path.join(OUT, "7-text.png"))
    page.click("#txt-copy"); ok(page.evaluate("(window.__calls.copy||[]).length") == 1, "copy goes to the app")
    page.click("#txt-close")
    page.click("#burger"); page.click("#m-bm"); page.click("#bm-add"); page.wait_for_timeout(200)
    page.screenshot(path=os.path.join(OUT, "8-bookmarks.png"))
    ok(page.evaluate("document.querySelectorAll('#bmlist .row').length") == 1, "bookmark added")
    page.click("#bm-close")

    print("== look dialog")
    page.click("#burger"); page.click("#m-look"); page.click("#seg-mode button:nth-child(2)"); page.wait_for_timeout(1200)
    ok("sepia" in page.evaluate("document.body.className"), "sepia mode on")
    modes = {m for (_, _, m) in requests_seen["pages"][-3:]}
    ok(modes == {2}, "pages re-requested as sepia: %s" % modes)
    page.screenshot(path=os.path.join(OUT, "9-look-sepia.png"))
    page.click("#seg-theme button:nth-child(2)"); page.wait_for_timeout(300)
    page.screenshot(path=os.path.join(OUT, "10-look-light.png"))
    page.click("#look-close"); page.wait_for_timeout(300)
    page.screenshot(path=os.path.join(OUT, "11-light-reader.png"))
    page.click("#burger"); page.click("#m-look"); page.click("#seg-theme button:nth-child(1)"); page.click("#seg-mode button:nth-child(3)"); page.click("#look-close"); page.wait_for_timeout(1200)
    ok("night" in page.evaluate("document.body.className") and "light" not in page.evaluate("document.body.className"), "night + dark theme")
    page.screenshot(path=os.path.join(OUT, "12-night.png"))

    print("== rotation")
    page.click("#burger"); page.click("#m-rot"); page.wait_for_timeout(1200)
    ok(page.evaluate("document.querySelector('.pg img').naturalWidth > document.querySelector('.pg img').naturalHeight"), "after a quarter turn the page image is landscape")
    ok(page.evaluate("(() => { const r = document.querySelector('.pg').getBoundingClientRect(); return r.width > r.height; })()"), "and so is its place on the screen")
    page.screenshot(path=os.path.join(OUT, "12b-rotated.png"))
    page.click("#burger"); page.click("#m-rot"); page.click("#burger"); page.click("#m-rot"); page.click("#burger"); page.click("#m-rot"); page.wait_for_timeout(1200)
    ok(page.evaluate("(() => { const r = document.querySelector('.pg').getBoundingClientRect(); return r.height > r.width; })()"), "four turns: upright again")

    print("== full screen")
    page.click("#burger"); page.click("#m-fs"); page.wait_for_timeout(500)
    box = page.evaluate("(() => { const r = document.querySelector('#fsx').getBoundingClientRect(); return [r.left, r.top, r.right, r.bottom, getComputedStyle(document.querySelector('#fsx')).display]; })()")
    ok(box[4] != "none" and box[3] < 80 and box[2] > 412 - 70 and box[2] <= 412, "cross is visible in the top right corner: %s" % box)
    ok(page.evaluate("document.querySelector('#top').getBoundingClientRect().bottom") <= 1, "bars are hidden in full screen")
    page.touchscreen.tap(206, 450); page.wait_for_timeout(500)
    ok(page.evaluate("document.querySelector('#top').getBoundingClientRect().bottom") <= 1, "tapping the page does not bring them back")
    page.screenshot(path=os.path.join(OUT, "13-fullscreen.png"))
    page.touchscreen.tap(box[0] + 20, box[1] + 20); page.wait_for_timeout(500)
    ok(page.evaluate("document.body.classList.contains('fs')") is False and page.evaluate("document.querySelector('#top').getBoundingClientRect().top") >= 0, "the cross leaves full screen and shows the bars")
    ok(page.evaluate("window.__calls.fullscreen") == [True, False], "app was told: %s" % page.evaluate("window.__calls.fullscreen"))

    print("== history and about")
    page.click("#burger"); page.click("#m-hist"); page.wait_for_timeout(200)
    page.screenshot(path=os.path.join(OUT, "14-history.png"))
    ok(page.evaluate("document.querySelectorAll('#hlist .hi').length") == 2, "history lists two documents")
    page.click("#h-close")
    page.click("#burger"); page.click("#m-about"); page.wait_for_timeout(200)
    page.screenshot(path=os.path.join(OUT, "15-about.png"))
    page.click("#a-close")
    ok(not errors, "no JS errors in the page: %s" % errors[:3])
    br.close()
print("\nFAILED: %d   (screenshots in %s)" % (fails, OUT))
sys.exit(1 if fails else 0)

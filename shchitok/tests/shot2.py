from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b=p.chromium.launch(); pg=b.new_page(viewport={'width':412,'height':860})
    errs=[]; pg.on('pageerror',lambda e:errs.append(str(e)))
    pg.add_init_script("window.AndroidApp={version:function(){return '1.2'},checkUpdate:function(){window.__c=(window.__c||0)+1}}")
    pg.goto('file:///home/claude/shchitok/assets/index.html'); pg.wait_for_timeout(300)
    pg.evaluate("document.getElementById('listWrap').scrollTo(0,99999)")
    print(pg.inner_text('#foot')); pg.click('#upd'); print('calls', pg.evaluate('window.__c')); print('label5', pg.inner_text('#list .row:nth-child(5) .name'))
    pg.screenshot(path='/tmp/s5.png'); print(errs); b.close()

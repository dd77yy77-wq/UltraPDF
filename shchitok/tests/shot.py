import sys
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page(viewport={'width':412,'height':860}, device_scale_factor=2)
    errs=[]
    pg.on('pageerror', lambda e: errs.append(str(e)))
    pg.on('console', lambda m: errs.append('console:'+m.text) if m.type=='error' else None)
    pg.goto('file:///home/claude/shchitok/assets/index.html')
    pg.wait_for_timeout(500)
    pg.screenshot(path='/tmp/s1.png')
    # tap list row 2
    pg.click('#list .row:nth-child(2)')
    pg.wait_for_timeout(500)
    pg.screenshot(path='/tmp/s2.png')
    # tap breaker 5 on photo
    box = pg.eval_on_selector('#ov', 'e=>{const r=e.getBoundingClientRect();return [r.x,r.y,r.width,r.height]}')
    x = box[0] + (968+1158)/2/2576*box[2]; y = box[1] + 700/1840*box[3]
    pg.mouse.click(x,y)
    pg.wait_for_timeout(500)
    print('selected row:', pg.eval_on_selector_all('#list .row.sel .num','e=>e.map(x=>x.textContent)'))
    pg.click('#zoom button[data-z="3"]')
    pg.wait_for_timeout(700)
    pg.screenshot(path='/tmp/s3.png')
    # edit dialog
    pg.click('#list .row:nth-child(5) .edit')
    pg.fill('#mInput','Свет в зале')
    pg.screenshot(path='/tmp/s4.png')
    pg.click('#mSave')
    print('label5:', pg.eval_on_selector('#list .row:nth-child(5) .name','e=>e.textContent'))
    pg.reload(); pg.wait_for_timeout(300)
    print('after reload label5:', pg.eval_on_selector('#list .row:nth-child(5) .name','e=>e.textContent'))
    print('errors:', errs)
    b.close()

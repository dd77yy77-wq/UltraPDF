from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A4, landscape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
import glob
f = glob.glob('/usr/share/fonts/**/DejaVuSans.ttf', recursive=True)[0]
fb = glob.glob('/usr/share/fonts/**/DejaVuSans-Bold.ttf', recursive=True)[0]
pdfmetrics.registerFont(TTFont('DV', f)); pdfmetrics.registerFont(TTFont('DVB', fb))
LOREM = ("Быстрая коричневая лиса прыгает через ленивую собаку. UltraPDF показывает документы без лишнего: "
         "поиск по тексту, закладки, оглавление и печать. The quick brown fox jumps over the lazy dog. ")
def make(path, pages=12, enc=None):
    c = canvas.Canvas(path, pagesize=A4, encrypt=enc)
    c.setTitle('Тестовый документ UltraPDF')
    for p in range(1, pages + 1):
        if p == 5: c.setPageSize(landscape(A4))
        elif p == 6: c.setPageSize(A4)
        W, H = c.\
            _pagesize
        c.bookmarkPage('p%d' % p)
        c.addOutlineEntry('Страница %d' % p if p % 4 else 'Глава %d' % (p // 4), 'p%d' % p, level=0 if p % 4 == 0 or p == 1 else 1)
        c.setFont('DVB', 26); c.drawString(50, H - 70, 'Глава %d. Заголовок страницы' % p)
        c.setFont('DV', 12)
        y = H - 110
        for i in range(30):
            c.drawString(50, y, LOREM[:95 + (i % 3) * 8]); y -= 18
            if y < 120: break
        c.setStrokeColorRGB(0.1, 0.4, 0.8); c.setFillColorRGB(0.8, 0.9, 1)
        c.rect(50, 60, 120, 40, fill=1)
        c.setFillColorRGB(0, 0, 0); c.setFont('DV', 11)
        c.drawString(60, 76, 'Ссылка на стр. %d' % (p % pages + 1))
        c.linkRect('', 'p%d' % (p % pages + 1), (50, 60, 170, 100), relative=0)
        c.drawString(200, 76, 'https://github.com/dd77yy77-wq/UltraPDF')
        c.linkURL('https://github.com/dd77yy77-wq/UltraPDF', (200, 70, 440, 90), relative=0)
        c.drawRightString(W - 50, 30, 'стр. %d' % p)
        c.showPage()
    c.save()
make('sample.pdf')
from reportlab.lib import pdfencrypt
make('locked.pdf', 3, pdfencrypt.StandardEncryption('secret', canPrint=1))
# big, many pages
make('long.pdf', 300)
print('ok')

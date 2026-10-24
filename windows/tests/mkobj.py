from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A4, A3, A5, letter, landscape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from PIL import Image, ImageDraw, ImageFont
import glob
f = glob.glob('/usr/share/fonts/**/LiberationSerif-Regular.ttf', recursive=True)[0]
fb = glob.glob('/usr/share/fonts/**/LiberationSerif-Bold.ttf', recursive=True)[0]
fs = glob.glob('/usr/share/fonts/**/LiberationSans-Regular.ttf', recursive=True)[0]
pdfmetrics.registerFont(TTFont('Serif', f)); pdfmetrics.registerFont(TTFont('SerifB', fb)); pdfmetrics.registerFont(TTFont('Sans', fs))
# a picture with transparency and a plain photo-like one
im = Image.new('RGBA', (400, 240), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.ellipse((10, 10, 390, 230), outline=(20, 60, 160, 255), width=8)
d.ellipse((30, 30, 370, 210), outline=(20, 60, 160, 255), width=3)
ft = ImageFont.truetype(fs, 46)
d.text((80, 90), "ООО «РОМАШКА»", fill=(20, 60, 160, 255), font=ft)
im.save('stamp.png')
ph = Image.new('RGB', (600, 400))
px = ph.load()
for y in range(400):
    for x in range(600):
        px[x, y] = (int(255 * x / 600), int(255 * y / 400), 160)
ph.save('photo.jpg', quality=88)

c = canvas.Canvas('obj.pdf', pagesize=A4)
c.setTitle('Объекты')
for p in (1, 2):
    W, H = A4
    c.setFont('SerifB', 24); c.drawString(60, H - 80, 'Договор поставки № %d' % (p * 17))
    c.setFont('Serif', 13)
    lines = ['Покупатель обязуется принять и оплатить товар в срок.', 'Поставщик гарантирует качество товара и сроки поставки.',
             'Стороны несут ответственность в соответствии с законом.', 'Все споры решаются в арбитражном суде по месту ответчика.']
    y = H - 120
    for l in lines:
        c.drawString(60, y, l); y -= 20
    c.setFont('Sans', 11)
    c.drawString(60, y - 10, 'Итого к оплате: 125 000 руб.')
    # table
    c.setStrokeColorRGB(0, 0, 0); c.setLineWidth(0.8)
    x0, y0, cw, rh = 60, 480, 120, 26
    for r in range(4):
        for col in range(3):
            c.rect(x0 + col * cw, y0 - r * rh, cw, rh)
            c.drawString(x0 + col * cw + 6, y0 - r * rh + 9, ['Товар', 'Кол-во', 'Цена'][col] if r == 0 else '%d' % (r * 10 + col))
    c.drawImage('photo.jpg', 60, 200, width=180, height=120)
    c.drawImage('stamp.png', 300, 190, width=200, height=120, mask='auto')
    c.setFillColorRGB(0.9, 0.95, 1); c.setStrokeColorRGB(0.1, 0.3, 0.7)
    c.rect(60, 100, 200, 50, fill=1)
    c.setFillColorRGB(0, 0, 0); c.setFont('Sans', 11); c.drawString(70, 120, 'Подпись: ____________')
    c.showPage()
c.save()

# pages of different sizes and a rotated one
c = canvas.Canvas('sizes.pdf', pagesize=A4)
specs = [('A4', A4), ('A3 альбомная', landscape(A3)), ('A5', A5), ('Letter', letter), ('Маленькая 300x200', (300, 200)), ('Большая 1000x700', (1000, 700))]
for name, sz in specs:
    c.setPageSize(sz)
    W, H = sz
    c.setFont('Sans', max(10, min(W, H) / 20)); c.drawString(W * 0.08, H * 0.88, 'Страница: ' + name)
    c.setStrokeColorRGB(0.8, 0.1, 0.1); c.setLineWidth(2); c.rect(W * 0.03, H * 0.03, W * 0.94, H * 0.94)
    c.setFillColorRGB(0.3, 0.5, 0.9); c.circle(W / 2, H / 2, min(W, H) / 5, fill=1)
    c.showPage()
c.save()

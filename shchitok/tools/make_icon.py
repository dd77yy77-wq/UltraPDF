from PIL import Image, ImageDraw, ImageFilter
def art(S=432):
    im = Image.new('RGB', (S, S), (16, 18, 22))
    d = ImageDraw.Draw(im)
    # subtle radial-ish glow
    glow = Image.new('RGB', (S, S), (16, 18, 22)); gd = ImageDraw.Draw(glow)
    gd.ellipse((S*0.2, S*0.2, S*0.8, S*0.8), fill=(52, 40, 14))
    glow = glow.filter(ImageFilter.GaussianBlur(S*0.12)); im = glow; d = ImageDraw.Draw(im)
    bw, bh, gap = 56, 128, 8
    n = 4; total = n*bw + (n-1)*gap
    x0 = (S-total)//2; y0 = (S-bh)//2
    for i in range(n):
        x = x0 + i*(bw+gap)
        sel = (i == 1)
        d.rounded_rectangle((x, y0, x+bw, y0+bh), 8, fill=(238, 241, 246), outline=(255,176,32) if sel else None, width=5 if sel else 0)
        # label strip
        d.rectangle((x+8, y0+14, x+bw-8, y0+24), fill=(60, 66, 78))
        d.rectangle((x+8, y0+30, x+bw-8, y0+36), fill=(150, 156, 168))
        # lever slot
        d.rounded_rectangle((x+10, y0+58, x+bw-10, y0+bh-12), 6, fill=(205, 210, 220))
        ly = y0+62 if sel else y0+bh-52
        d.rounded_rectangle((x+14, ly, x+bw-14, ly+34), 6, fill=(255,176,32) if sel else (14, 124, 107))
    return im
if __name__ == '__main__':
    art().save('/tmp/icon_bg.png')

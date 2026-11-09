# Builds watch/rubi_watch/icons.h: round 56x56 RGB565 menu icons (black outside the circle).
# Source: icons/*.png (same set as Rubi's screen menu). FOLLOW/UPDATE are kept from the old icons.h (re-masked round).
import re, sys
from PIL import Image, ImageDraw
S = 56; K = 8  # supersample
SRC = {'MUSIC':'nghe_nhac','STORY':'truyen_tranh','LEARN':'toan_lop1','ENGLISH':'song_ngu',
       'SET':'cai_dat','DRIVE':'lai_xe','DANCE':'nhay_mua_rb','SLEEP':'ru_ngu_rb','FOLLOW':'di_theo_rb'}
KEEP = ['UPDATE']
OUT = 'watch/rubi_watch/icons.h'
old = open(OUT).read()
def circle_mask(n):
    m = Image.new('L', (n*4, n*4), 0); ImageDraw.Draw(m).ellipse((0,0,n*4-1,n*4-1), fill=255)
    return m.resize((n, n), Image.LANCZOS)
def finish(img):  # img: RGB at S*K px square -> round on black at S px
    n = S*K
    f = Image.new('RGB', (n, n), (0,0,0)); f.paste(img, (0,0), circle_mask(n))
    return f.resize((S, S), Image.LANCZOS)
def from_png(name):
    n = S*K
    im = Image.open(f'icons/{name}.png').convert('RGB')
    w, h = im.size; k = n*0.84/max(w, h)
    im = im.resize((int(w*k), int(h*k)), Image.LANCZOS)
    bg = Image.new('RGB', (n, n), (250,250,252)); bg.paste(im, ((n-im.width)//2, (n-im.height)//2))
    return finish(bg)
def from_old(name):
    m = re.search(r'ICON_%s\[ICON_SIZE\*ICON_SIZE\] PROGMEM = \{(.*?)\};' % name, old, re.S)
    v = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{4}', m.group(1))]
    im = Image.new('RGB', (S, S))
    im.putdata([(((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31) for c in v])
    n = S*K; big = im.resize((n, n), Image.LANCZOS)
    return finish(big)
def arr(name, img):
    px = [((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r, g, b in img.getdata()]
    rows = [','.join('0x%04X' % p for p in px[i:i+16]) + ',' for i in range(0, len(px), 16)]
    return 'const uint16_t ICON_%s[ICON_SIZE*ICON_SIZE] PROGMEM = {\n%s\n};\n' % (name, '\n'.join(rows))
parts = ['// Tu dong tao boi tools/make_watch_icons.py tu icons/*.png (tron 56x56, RGB565, ngoai vong tron la nen den). Khong sua tay.\n#pragma once\n#include <stdint.h>\n#include <pgmspace.h>\n#define ICON_SIZE 56\n\n']
for n, f in SRC.items(): parts.append(arr(n, from_png(f)) + '\n')
for n in KEEP: parts.append(arr(n, from_old(n)) + '\n')
open(OUT, 'w').write(''.join(parts))
print('ok')

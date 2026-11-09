# Tao watch/rubi_watch/dials.h: cac mat so dong ho (khong co kim) 240x240 JPEG. Kim do dong ho tu ve.
import io, glob, os
from PIL import Image, ImageDraw
names = ['dial_hoathinh', 'dial_be', 'dial_vutru', 'dial_bien', 'dial_xanh']
S = 240
out = ['// Tu dong tao boi tools/make_watch_dials.py tu icons/dials/*.png. Khong sua tay.\n#pragma once\n#include <stdint.h>\n#include <pgmspace.h>\n\n']
lens = []
for i, n in enumerate(names):
    im = Image.open(f'icons/dials/{n}.png').convert('RGB').resize((S, S), Image.LANCZOS)
    m = Image.new('L', (S*4, S*4), 0); ImageDraw.Draw(m).ellipse((0, 0, S*4-1, S*4-1), fill=255); m = m.resize((S, S), Image.LANCZOS)
    bg = Image.new('RGB', (S, S), (0, 0, 0)); bg.paste(im, (0, 0), m)
    b = io.BytesIO(); bg.save(b, 'JPEG', quality=85, subsampling=2, optimize=False)
    d = b.getvalue(); lens.append(len(d))
    rows = [','.join(str(x) for x in d[j:j+24]) + ',' for j in range(0, len(d), 24)]
    out.append('const uint8_t DIAL_JPG_%d[] PROGMEM = {\n%s\n};\nconst uint32_t DIAL_JPG_%d_LEN = %d;\n\n' % (i, '\n'.join(rows), i, len(d)))
out.append('#define DIAL_COUNT %d\nconst uint8_t* const DIAL_JPG[DIAL_COUNT] = {%s};\nconst uint32_t DIAL_JPG_LEN[DIAL_COUNT] = {%s};\n' % (len(names), ','.join('DIAL_JPG_%d' % i for i in range(len(names))), ','.join('DIAL_JPG_%d_LEN' % i for i in range(len(names)))))
open('watch/rubi_watch/dials.h', 'w').write(''.join(out))
print('ok', lens)

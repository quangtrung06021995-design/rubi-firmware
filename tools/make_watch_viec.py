# Tao watch/rubi_watch/viec.h: 8 icon viec (thoi gian bieu) 112x112 JPEG, goc bo tron (nen den). Nguon: icons/viec/viec_*.png
import io
from PIL import Image, ImageDraw
names = ['viec_danh_rang', 'viec_an_com', 'viec_hoc_bai', 'viec_di_tam', 'viec_di_ngu', 'viec_don_do_choi', 'viec_uong_nuoc', 'viec_doc_sach']
S = 112; R = 20
out = ['// Tu dong tao boi tools/make_watch_viec.py tu icons/viec/*.png. Khong sua tay.\n#pragma once\n#include <stdint.h>\n#include <pgmspace.h>\n#define VIEC_SIZE %d\n\n' % S]
lens = []
for i, n in enumerate(names):
    im = Image.open(f'icons/viec/{n}.png').convert('RGB').resize((S, S), Image.LANCZOS)
    m = Image.new('L', (S*4, S*4), 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, S*4-1, S*4-1), R*4, fill=255); m = m.resize((S, S), Image.LANCZOS)
    bg = Image.new('RGB', (S, S), (0, 0, 0)); bg.paste(im, (0, 0), m)
    b = io.BytesIO(); bg.save(b, 'JPEG', quality=82, subsampling=2)
    d = b.getvalue(); lens.append(len(d))
    rows = [','.join(str(x) for x in d[j:j+24]) + ',' for j in range(0, len(d), 24)]
    out.append('const uint8_t VIEC_JPG_%d[] PROGMEM = {\n%s\n};\nconst uint32_t VIEC_JPG_%d_LEN = %d;\n\n' % (i, '\n'.join(rows), i, len(d)))
out.append('const uint8_t* const VIEC_JPG[8] = {%s};\nconst uint32_t VIEC_JPG_LEN[8] = {%s};\n' % (','.join('VIEC_JPG_%d' % i for i in range(8)), ','.join('VIEC_JPG_%d_LEN' % i for i in range(8))))
open('watch/rubi_watch/viec.h', 'w').write(''.join(out))
print('ok', lens, sum(lens))

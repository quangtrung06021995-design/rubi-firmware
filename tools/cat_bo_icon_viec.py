# Cat to anh 10 icon (3x3 + 1) thanh tung file PNG vuong 512x512 trong icons/viec/. Dung: python3 tools/cat_bo_icon_viec.py <anh_to.png>
import sys
from PIL import Image
import numpy as np
NAMES = ['viec_danh_rang', 'viec_an_com', 'viec_hoc_bai', 'viec_di_tam', 'viec_di_ngu', 'viec_don_do_choi', 'viec_uong_nuoc', 'viec_doc_sach', 'thoi_gian_bieu', 'sao_thuong']
im = Image.open(sys.argv[1]).convert('RGB')
nonw = (np.array(im).astype(int).sum(axis=2) < 740)
def runs(v, m=20):
    r = []; s = None
    for i, x in enumerate(v):
        if x and s is None: s = i
        if not x and s is not None:
            if i - s > m: r.append((s, i))
            s = None
    if s is not None: r.append((s, len(v)))
    return r
boxes = []
for (y0, y1) in runs(nonw.any(axis=1)):
    for (x0, x1) in runs(nonw[y0:y1].any(axis=0)): boxes.append((x0, y0, x1, y1))
assert len(boxes) == len(NAMES), boxes
INSET = 18
for n, (x0, y0, x1, y1) in zip(NAMES, boxes):
    t = im.crop((x0 + INSET, y0 + INSET, x1 - INSET, y1 - INSET))
    s = max(t.size)
    sq = Image.new('RGB', (s, s), (255, 255, 255)); sq.paste(t, ((s - t.width) // 2, (s - t.height) // 2))
    sq.resize((512, 512), Image.LANCZOS).save(f'icons/viec/{n}.png')
print('ok', len(boxes))

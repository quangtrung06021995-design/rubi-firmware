#!/usr/bin/env python3
"""Doi mot thu muc truyen (story.json + anh .jpg + giong doc .mp3) thanh thu muc chep vao the nho cua Rubi.

Cach dung (can Python 3, Pillow, ffmpeg):
    python tao_truyen.py <thu_muc_truyen> <thu_muc_dau_ra>
Vi du:
    python tao_truyen.py tho_va_rua ra
Ket qua:  ra/rubi/truyen/<id>/ten.txt, 00_bia.bin, 00_bia.wav, 01.bin, 01.wav ...
Chep thu muc "rubi" cua <thu_muc_dau_ra> vao goc the nho (gop voi thu muc rubi co san).
Anh duoc doi thanh RGB565 320x240 (153600 byte), am thanh thanh WAV 16 kHz mono 16 bit.
"""
import json, os, subprocess, sys
from PIL import Image


def to_rgb565(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    if (w, h) != (320, 240):   # fill the screen, cut the extra edge
        k = max(320 / w, 240 / h)
        im = im.resize((round(w * k), round(h * k)), Image.LANCZOS)
        l, t = (im.size[0] - 320) // 2, (im.size[1] - 240) // 2
        im = im.crop((l, t, l + 320, t + 240))
    data = bytearray()
    for r, g, b in im.get_flattened_data() if hasattr(im, 'get_flattened_data') else im.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        data += bytes((v & 0xFF, v >> 8))
    return bytes(data)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(src, 'story.json'), encoding='utf-8') as f:
        st = json.load(f)
    dst = os.path.join(out, 'rubi', 'truyen', st['id'])
    os.makedirs(dst, exist_ok=True)
    with open(os.path.join(dst, 'ten.txt'), 'w', encoding='utf-8') as f:
        f.write(st['title'] + '\n')
    for sc in st['scenes']:
        base = os.path.splitext(sc['image'])[0]
        with open(os.path.join(dst, base + '.bin'), 'wb') as f:
            f.write(to_rgb565(os.path.join(src, sc['image'])))
        mp3 = os.path.join(src, sc['audio'])
        if os.path.exists(mp3):
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', mp3, '-ar', '16000', '-ac', '1',
                            '-c:a', 'pcm_s16le', os.path.join(dst, base + '.wav')], check=True)
        print('xong', base)
    print('Chep thu muc', os.path.join(out, 'rubi'), 'vao the nho.')


if __name__ == '__main__':
    main()

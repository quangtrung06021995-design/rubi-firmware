#!/usr/bin/env python3
"""Tao bo giong doc "Toan lop 1" cho Rubi bang GIONG BAC (Ha Noi) cua Microsoft, mien phi, khong can khoa API.

Cai dat mot lan (Python 3.9+):
    pip install -U edge-tts av

Chay (de chung thu muc voi tao_giong_toan.py, vi file nay dung lai danh sach tu cua no):
    py tao_giong_toan_bac.py --thu                 # chi tao 6 file nghe thu (nen chay truoc)
    py tao_giong_toan_bac.py                       # tao TAT CA (~145 file), chay lai se bo qua file da co
    py tao_giong_toan_bac.py --giong vi-VN-NamMinhNeural     # giong nam Bac
    py tao_giong_toan_bac.py --toc-do -20%         # doc cham hon (mac dinh -10%)

Giong nu Bac: vi-VN-HoaiMyNeural (mac dinh). Giong nam Bac: vi-VN-NamMinhNeural.
Ket qua nam trong thu muc giong_rubi_bac/ (WAV 22,05 kHz, mono, 16 bit). Ten file giong het ban cu,
nen chi can chep de len thu muc rubi/hoc/toan/ tren the nho.
"""
import argparse, array, asyncio, io, os, sys, time

try:
    from tao_giong_toan import danh_sach, THU, cat_lang, ghi_wav, OUT_RATE
except ImportError:
    sys.exit("Hay dat file nay CUNG THU MUC voi tao_giong_toan.py")


async def tts(text, voice, rate):
    import edge_tts
    com = edge_tts.Communicate(text, voice, rate=rate)
    data = b""
    async for ch in com.stream():
        if ch["type"] == "audio":
            data += ch["data"]
    if not data:
        raise RuntimeError("khong nhan duoc am thanh")
    return data


def giai_ma(mp3):
    import av
    cont = av.open(io.BytesIO(mp3))
    rs = av.AudioResampler(format="s16", layout="mono", rate=OUT_RATE)
    pcm = array.array("h")
    for fr in cont.decode(audio=0):
        for o in rs.resample(fr):
            pcm.frombytes(o.to_ndarray().tobytes())
    for o in rs.resample(None):
        pcm.frombytes(o.to_ndarray().tobytes())
    cont.close()
    return pcm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thu", action="store_true", help="chi tao vai file nghe thu")
    ap.add_argument("--giong", default="vi-VN-HoaiMyNeural", help="vi-VN-HoaiMyNeural (nu) hoac vi-VN-NamMinhNeural (nam)")
    ap.add_argument("--toc-do", default="-10%", help="toc do doc, vi du -10%% (cham hon), +0%%")
    ap.add_argument("--ra", default="giong_rubi_bac", help="thu muc dau ra")
    args = ap.parse_args()
    try:
        import edge_tts, av  # noqa: F401
    except ImportError:
        sys.exit("Can cai: pip install -U edge-tts av")

    os.makedirs(args.ra, exist_ok=True)
    ds = danh_sach()
    if args.thu:
        ds = [d for d in ds if d[0] in THU]
    loi = 0
    for i, (ten, noidung) in enumerate(ds, 1):
        path = os.path.join(args.ra, ten + ".wav")
        if os.path.exists(path):
            continue
        pcm = None
        for lan in range(5):
            try:
                pcm = giai_ma(asyncio.run(tts(noidung, args.giong, args.toc_do)))
                break
            except Exception as e:
                print("  loi (%s), thu lai..." % str(e)[:150])
                time.sleep(4)
        if pcm is None or len(pcm) == 0:
            print("[%d/%d] %s: BO QUA (that bai)" % (i, len(ds), ten))
            loi += 1
            continue
        pcm = cat_lang(pcm, OUT_RATE)
        ghi_wav(path, pcm)
        print("[%d/%d] %s = %s  (%.1f giay)" % (i, len(ds), ten, noidung, len(pcm) / OUT_RATE))
        time.sleep(0.3)
    print("Xong. Loi: %d. Cac file nam trong thu muc %s" % (loi, args.ra))
    if loi:
        print("Chay lai lenh nay de tao not cac file con thieu.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Tao bo giong doc "Toan lop 1" cho Rubi bang GIONG BAC (Ha Noi) cua Microsoft, mien phi, KHONG can khoa API.
Dung dung giong va cach lam cua tao_giong.py (the hoc song ngu): edge-tts + ffmpeg, tu cai thu vien o lan dau.

Cach dung (Python 3.9+, can Internet):
    py tao_giong_toan_bac.py --thu      # chi tao 6 file nghe thu
    py tao_giong_toan_bac.py            # tao TAT CA (~145 file); chay lai se bo qua file da co
    py tao_giong_toan_bac.py --giong vi-VN-NamMinhNeural     # giong nam Bac
    py tao_giong_toan_bac.py --toc-do -25%                   # doc cham hon (mac dinh -15%)
    py tao_giong_toan_bac.py --lai tu_tru ten_mai tu_may     # xoa va tao lai vai file doc chua ung y

Ket qua nam trong thu muc giong_rubi_bac/ (WAV 22,05 kHz, mono, 16 bit, da cat khoang lang).
Ten file giong het ban cu: chep de len thu muc rubi/hoc/toan/ tren the nho.
"""
import argparse, array, asyncio, os, subprocess, sys, tempfile, time, wave

for mod, pkg in (("edge_tts", "edge-tts"), ("imageio_ffmpeg", "imageio-ffmpeg")):
    try:
        __import__(mod)
    except ImportError:
        print("Dang cai", pkg, "(chi lan dau, can Internet)...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", pkg])
import edge_tts, imageio_ffmpeg

OUT_RATE = 22050

UNITS = ["không", "một", "hai", "ba", "bốn", "năm", "sáu", "bảy", "tám", "chín"]


def so_viet(n):
    if n < 10:
        return UNITS[n]
    if n == 10:
        return "mười"
    if n == 100:
        return "một trăm"
    t, u = divmod(n, 10)
    if t == 1:
        return "mười " + ("lăm" if u == 5 else UNITS[u])
    s = UNITS[t] + " mươi"
    if u == 0:
        return s
    if u == 1:
        return s + " mốt"
    if u == 5:
        return s + " lăm"
    return s + " " + UNITS[u]


def danh_sach():
    """(ten_file, noi_dung) cho moi doan can thu."""
    ds = []
    for n in range(0, 101):
        ds.append(("so_%d" % n, so_viet(n)))
    for k, v in [("cong", "cộng"), ("tru", "trừ"), ("bang", "bằng"), ("may", "mấy"), ("bangmay", "bằng mấy")]:
        ds.append(("tu_" + k, v))
    for k, v in [("lan", "Lan"), ("nam", "Nam"), ("mai", "Mai"), ("hung", "Hùng"), ("na", "Na"), ("ti", "Tí"),
                 ("hoa", "Hoa"), ("minh", "Minh"), ("an", "An"), ("binh", "Bình")]:
        ds.append(("ten_" + k, v))
    for k, v in [("me", "Mẹ"), ("bo", "Bố"), ("ba", "Bà"), ("ong", "Ông"), ("chi", "Chị")]:
        ds.append(("nguoi_" + k, v))
    for k, v in [("quacam", "quả cam"), ("vienbi", "viên bi"), ("caikeo", "cái kẹo"), ("bonghoa", "bông hoa"),
                 ("quyentruyen", "quyển truyện"), ("caibanh", "cái bánh"), ("quatao", "quả táo"),
                 ("chiecbut", "chiếc bút"), ("contem", "con tem")]:
        ds.append(("vat_" + k, v))
    for k, v in [("co", "có"), ("chothem", "cho thêm"), ("chocban", "cho bạn"), ("hoi", "Hỏi"),
                 ("cotatcabaonhieu", "có tất cả bao nhiêu"), ("conlaibaonhieu", "còn lại bao nhiêu"),
                 ("haibancotatcabaonhieu", "hai bạn có tất cả bao nhiêu"), ("conhieuhon", "có nhiều hơn"),
                 ("coithon", "có ít hơn"), ("cobaonhieu", "có bao nhiêu"), ("trongroco", "Trong rổ có"),
                 ("an", "ăn"), ("trongroconlaibaonhieu", "Hỏi trong rổ còn lại bao nhiêu"),
                 ("nhieuhon", "nhiều hơn"), ("baonhieu", "bao nhiêu")]:
        ds.append(("cau_" + k, v))
    # Loi khen / nhac nho khi be tra loi (Rubi doc sau moi cau)
    for k, v in [("dung1", "Đúng rồi! Giỏi quá!"), ("dung2", "Chính xác! Con giỏi lắm!"), ("dung3", "Tuyệt vời! Con làm đúng rồi!"),
                 ("sai1", "Chưa đúng. Con thử lại nhé!"), ("sai2", "Chưa đúng rồi. Con suy nghĩ lại nhé!"),
                 ("dapan", "Đáp án là"), ("tiep", "Mình làm câu tiếp nhé!"), ("xong", "Xong rồi!"),
                 ("condung", "Con trả lời đúng ngay"), ("cau", "câu"),
                 ("khen9", "Giỏi quá, tuyệt vời!"), ("khen7", "Giỏi lắm!"),
                 ("khen5", "Khá lắm, luyện thêm nhé!"), ("khen0", "Cố lên, mình chơi lại nhé!")]:
        ds.append(("ph_" + k, v))
    return ds


THU = ["so_7", "so_21", "tu_cong", "tu_bangmay", "ten_lan", "cau_cotatcabaonhieu"]


def cat_lang(pcm, rate):
    """Cat khoang lang dau/cuoi, chua lai 60 ms."""
    if not pcm:
        return pcm
    peak = max(abs(x) for x in pcm) or 1
    thr = max(300, peak // 25)
    a, b = 0, len(pcm) - 1
    while a < len(pcm) and abs(pcm[a]) < thr:
        a += 1
    while b > a and abs(pcm[b]) < thr:
        b -= 1
    pad = int(rate * 0.06)
    a = max(0, a - pad)
    b = min(len(pcm), b + pad)
    return pcm[a:b]


def ghi_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_RATE)
        w.writeframes(pcm.tobytes())


async def tao_mot(text, voice, rate, ff):
    mp3 = os.path.join(tempfile.gettempdir(), "rubi_toan_tmp.mp3")
    await edge_tts.Communicate(text, voice, rate=rate).save(mp3)
    r = subprocess.run([ff, "-y", "-loglevel", "error", "-i", mp3, "-ac", "1", "-ar", str(OUT_RATE),
                        "-f", "s16le", "-"], capture_output=True)
    try:
        os.remove(mp3)
    except OSError:
        pass
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError("khong doi duoc am thanh")
    pcm = array.array("h")
    pcm.frombytes(r.stdout[: len(r.stdout) // 2 * 2])
    return pcm


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thu", action="store_true", help="chi tao vai file nghe thu")
    ap.add_argument("--giong", default="vi-VN-HoaiMyNeural", help="vi-VN-HoaiMyNeural (nu Bac) hoac vi-VN-NamMinhNeural (nam Bac)")
    ap.add_argument("--toc-do", default="-15%", help="toc do doc, vi du -15%% (cham hon), +0%%")
    ap.add_argument("--ra", default="giong_rubi_bac", help="thu muc dau ra")
    ap.add_argument("--lai", nargs="+", metavar="TEN", help="xoa va tao lai cac file nay (vi du tu_tru ten_mai)")
    args = ap.parse_args()

    ff = imageio_ffmpeg.get_ffmpeg_exe()
    os.makedirs(args.ra, exist_ok=True)
    ds = danh_sach()
    if args.thu:
        ds = [d for d in ds if d[0] in THU]
    if args.lai:
        ten_lai = [t[:-4] if t.endswith(".wav") else t for t in args.lai]
        ds = [d for d in danh_sach() if d[0] in ten_lai]
        for ten, _ in ds:
            p = os.path.join(args.ra, ten + ".wav")
            if os.path.exists(p):
                os.remove(p)
    loi = 0
    for i, (ten, noidung) in enumerate(ds, 1):
        path = os.path.join(args.ra, ten + ".wav")
        if os.path.exists(path):
            continue
        pcm = None
        for lan in range(5):
            try:
                pcm = await tao_mot(noidung, args.giong, args.toc_do, ff)
                break
            except Exception as e:
                print("  loi (%s), thu lai..." % str(e)[:150])
                await asyncio.sleep(4)
        if pcm is None or len(pcm) == 0:
            print("[%d/%d] %s: BO QUA (that bai)" % (i, len(ds), ten))
            loi += 1
            continue
        pcm = cat_lang(pcm, OUT_RATE)
        ghi_wav(path, pcm)
        print("[%d/%d] %s = %s  (%.1f giay)" % (i, len(ds), ten, noidung, len(pcm) / OUT_RATE))
        await asyncio.sleep(0.3)
    print("Xong. Loi: %d. Cac file nam trong thu muc %s" % (loi, args.ra))
    if loi:
        print("Chay lai lenh nay de tao not cac file con thieu.")


if __name__ == "__main__":
    asyncio.run(main())

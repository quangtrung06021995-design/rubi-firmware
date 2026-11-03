#!/usr/bin/env python3
"""Tao file giong TIENG ANH cho cac bai "Tieng Anh giao tiep" cua Rubi (edge-tts, mien phi, khong can khoa API).

Chi can MOT file nay: de no trong thu muc nao cung duoc (vi du D:\\hoc) roi chay bang VS Code hoac:
    py tao_giong_anh.py --thu       # chi tao 2 file nghe thu
    py tao_giong_anh.py             # tao tat ca cac bai
    py tao_giong_anh.py --giong en-US-AnaNeural --toc-do -20%
Lan dau chay, neu chua co thu muc bai_tieng_anh thi file tu tao bai mau "Lop 2 - Chao hoi" ngay canh file nay.
Muon them bai moi: them thu muc moi trong bai_tieng_anh roi chay lai.

Moi bai la 1 thu muc trong bai_tieng_anh/ gom:
    ten.txt   dong dau = ten bai hien tren man hinh
    bai.txt   moi dong:  ten_file|Cau tieng Anh|Nghia tieng Viet
Ket qua nam trong thu muc rubi_anh/<bai>/ (bai.txt, ten.txt, <ten_file>.wav = tieng Anh, <ten_file>_vi.wav = tieng Viet).
Chep CA thu muc rubi_anh vao the nho thanh  rubi/anh  (tren the se la rubi/anh/<bai>/...).
Chay lai se bo qua file da co.
"""
import argparse, array, asyncio, os, shutil, subprocess, sys, tempfile, wave

for mod, pkg in (("edge_tts", "edge-tts"), ("imageio_ffmpeg", "imageio-ffmpeg")):
    try:
        __import__(mod)
    except ImportError:
        print("Dang cai", pkg, "(chi lan dau, can Internet)...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", pkg])
import edge_tts, imageio_ffmpeg

OUT_RATE = 22050
HERE = os.path.dirname(os.path.abspath(__file__))

BAI_MAU = {
    "lop2_chao_hoi": ("Lớp 2 - Chào hỏi", """hello|Hello!|Xin chào!
hi|Hi!|Chào bạn!
goodmorning|Good morning!|Chào buổi sáng!
goodafternoon|Good afternoon!|Chào buổi chiều!
goodbye|Goodbye!|Tạm biệt!
howareyou|How are you?|Bạn khỏe không?
fine|I'm fine, thank you.|Mình khỏe, cảm ơn bạn.
yourname|What's your name?|Bạn tên là gì?
myname|My name is Nam.|Tên mình là Nam.
nicetomeet|Nice to meet you.|Rất vui được gặp bạn.
"""),
}


def tao_bai_mau(vao):
    for ten, (tieu_de, noi_dung) in BAI_MAU.items():
        d = os.path.join(vao, ten)
        os.makedirs(d, exist_ok=True)
        if not os.path.exists(os.path.join(d, "bai.txt")):
            open(os.path.join(d, "ten.txt"), "w", encoding="utf-8").write(tieu_de + "\n")
            open(os.path.join(d, "bai.txt"), "w", encoding="utf-8").write(noi_dung)


def cat_lang(pcm, rate):
    if not pcm:
        return pcm
    peak = max(abs(x) for x in pcm) or 1
    thr = max(300, peak // 25)
    a, b = 0, len(pcm) - 1
    while a < len(pcm) and abs(pcm[a]) < thr:
        a += 1
    while b > a and abs(pcm[b]) < thr:
        b -= 1
    pad = int(rate * 0.08)
    return pcm[max(0, a - pad):min(len(pcm), b + pad)]


def ghi_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_RATE)
        w.writeframes(pcm.tobytes())


async def tao_mot(text, voice, rate, ff):
    mp3 = os.path.join(tempfile.gettempdir(), "rubi_anh_tmp.mp3")
    tempo = None
    try:
        await edge_tts.Communicate(text, voice, rate=rate).save(mp3)
    except Exception as e:
        # Mot so may bao "No audio was received" khi chinh toc do: doc binh thuong roi lam cham bang ffmpeg
        try:
            pct = int(rate.replace("%", "").replace("+", ""))
        except ValueError:
            raise e
        if pct == 0:
            raise
        await edge_tts.Communicate(text, voice).save(mp3)
        tempo = max(0.5, min(2.0, 1 + pct / 100.0))
    cmd = [ff, "-y", "-loglevel", "error", "-i", mp3]
    if tempo is not None:
        cmd += ["-filter:a", "atempo=%.3f" % tempo]
    cmd += ["-ac", "1", "-ar", str(OUT_RATE), "-f", "s16le", "-"]
    r = subprocess.run(cmd, capture_output=True)
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
    ap.add_argument("--thu", action="store_true", help="chi tao 2 file nghe thu")
    ap.add_argument("--giong", default="en-US-AriaNeural", help="giong tieng Anh (mac dinh en-US-AriaNeural, giong nu nguoi lon ro rang)")
    ap.add_argument("--toc-do", default="-10%", help="toc do doc tieng Anh, vi du -20%% (cham hon)")
    ap.add_argument("--giong-viet", default="vi-VN-HoaiMyNeural", help="giong doc nghia tieng Viet (nu Bac)")
    ap.add_argument("--toc-do-viet", default="-15%", help="toc do doc tieng Viet")
    ap.add_argument("--vao", default=os.path.join(HERE, "bai_tieng_anh"), help="thu muc chua cac bai")
    ap.add_argument("--ra", default=os.path.join(HERE, "rubi_anh"), help="thu muc dau ra")
    args = ap.parse_args()

    ff = imageio_ffmpeg.get_ffmpeg_exe()
    if not os.path.isdir(args.vao):
        print("Chua co thu muc", args.vao, "- tao bai mau.")
        tao_bai_mau(args.vao)
    loi = 0
    for bai in sorted(os.listdir(args.vao)):
        src = os.path.join(args.vao, bai)
        txt = os.path.join(src, "bai.txt")
        if not os.path.isfile(txt):
            continue
        dst = os.path.join(args.ra, bai)
        os.makedirs(dst, exist_ok=True)
        for f in ("bai.txt", "ten.txt"):
            if os.path.isfile(os.path.join(src, f)):
                shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))
        dong = []
        for l in open(txt, encoding="utf-8"):
            p = l.strip().split("|")
            if len(p) >= 3:
                dong.append((p[0].strip(), p[1].strip(), p[2].strip()))
        if args.thu:
            dong = dong[:2]
        for i, (ten, cau, viet) in enumerate(dong, 1):
            # <ten>.wav = cau tieng Anh, <ten>_vi.wav = nghia tieng Viet (Rubi doc tieng Anh roi tieng Viet)
            for hau, noidung, giong, tocdo in ((".wav", cau, args.giong, args.toc_do),
                                               ("_vi.wav", viet, args.giong_viet, args.toc_do_viet)):
                path = os.path.join(dst, ten + hau)
                if os.path.exists(path) or not noidung:
                    continue
                pcm = None
                for lan in range(5):
                    try:
                        pcm = await tao_mot(noidung, giong, tocdo, ff)
                        break
                    except Exception as e:
                        print("  loi (%s), thu lai..." % str(e)[:150])
                        await asyncio.sleep(4)
                if pcm is None or len(pcm) == 0:
                    print("[%s %d/%d] %s%s: BO QUA (that bai)" % (bai, i, len(dong), ten, hau))
                    loi += 1
                    continue
                pcm = cat_lang(pcm, OUT_RATE)
                ghi_wav(path, pcm)
                print("[%s %d/%d] %s%s = %s  (%.1f giay)" % (bai, i, len(dong), ten, hau, noidung, len(pcm) / OUT_RATE))
                await asyncio.sleep(0.3)
    print("Xong. Loi: %d. Ket qua nam trong thu muc %s" % (loi, args.ra))
    print("Chep thu muc %s vao the nho thanh rubi/anh" % args.ra)
    if loi:
        print("Chay lai lenh nay de tao not cac file con thieu.")


if __name__ == "__main__":
    asyncio.run(main())

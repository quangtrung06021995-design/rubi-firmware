#!/usr/bin/env python3
"""Tao bo giong doc cho "Toan lop 1" cua Rubi bang Gemini (giong nguoi, tieng Viet).

Cach dung (Python 3.9+):
    pip install -U google-genai        (can ban 2.25 tro len)
    Windows (PowerShell):   $env:GEMINI_API_KEY = "khoa-cua-ban"
    Windows (cmd):          set GEMINI_API_KEY=khoa-cua-ban
    Mac/Linux:              export GEMINI_API_KEY=khoa-cua-ban

    py tao_giong_toan.py --thu          # chi tao 6 file nghe thu (tot nhat chay truoc)
    py tao_giong_toan.py                # tao TAT CA (khoang 145 file), chay lai se bo qua file da co
    py tao_giong_toan.py --giong Aoede  # doi giong (Kore, Aoede, Puck, Leda, Zephyr, Charon...)

Ket qua nam trong thu muc  giong_rubi/  (WAV 22,05 kHz, mono, 16 bit, da cat khoang lang).
Khoa API (API key): KHONG gui cho ai, KHONG dan vao chat.
"""
import argparse, array, base64, io, os, sys, time, wave

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


def doi_toc_do(pcm, src, dst):
    if src == dst:
        return pcm
    n = int(len(pcm) * dst / src)
    out = array.array("h", [0]) * n
    step = src / dst
    for i in range(n):
        pos = i * step
        j = int(pos)
        f = pos - j
        x0 = pcm[j]
        x1 = pcm[j + 1] if j + 1 < len(pcm) else x0
        out[i] = int(x0 + (x1 - x0) * f)
    return out


def ghi_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_RATE)
        w.writeframes(pcm.tobytes())


KIEU = "slow, warm and clear, like a kind teacher talking to a 7-year-old child"


def lay_pcm(client, types, model, giong, noidung):
    """Tra ve (pcm int16, toc do lay mau). Model 3.x dung Interactions API, model 2.5 dung generate_content."""
    if model.startswith("gemini-3"):
        it = client.interactions.create(
            model=model,
            input=[{"type": "user_input", "content": [{
                "type": "text", "text": noidung,
                "annotations": [{"type": "speech_metadata", "style": KIEU}]}]}],
            response_format={"type": "audio"},
            generation_config={"speech_config": [{"voice": giong}]},
        )
        data = base64.b64decode(it.output_audio.data)
    else:
        prompt = "Đọc bằng tiếng Việt, giọng ấm áp, rõ ràng, chậm rãi như cô giáo nói với bé 7 tuổi: " + noidung
        r = client.models.generate_content(
            model=model, contents=prompt,
            config=types.GenerateContentConfig(
                response_modalities=["AUDIO"],
                speech_config=types.SpeechConfig(voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=giong)))))
        cand = r.candidates[0] if r.candidates else None
        if cand is None or cand.content is None or not cand.content.parts:
            raise RuntimeError("model khong tra am thanh (finish_reason=%s)" % (getattr(cand, "finish_reason", None),))
        data = cand.content.parts[0].inline_data.data
    rate = 24000
    if data[:4] == b"RIFF":      # file WAV day du
        with wave.open(io.BytesIO(data), "rb") as w:
            rate = w.getframerate()
            frames = w.readframes(w.getnframes())
            if w.getnchannels() == 2:
                tmp = array.array("h")
                tmp.frombytes(frames[: len(frames) // 4 * 4])
                frames = tmp[::2].tobytes()
        data = frames
    pcm = array.array("h")
    pcm.frombytes(data[: len(data) // 2 * 2])
    return pcm, rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thu", action="store_true", help="chi tao vai file nghe thu")
    ap.add_argument("--giong", default="Kore", help="ten giong Gemini (mac dinh Kore)")
    ap.add_argument("--model", default="gemini-3.8-flash-tts")
    ap.add_argument("--ra", default="giong_rubi", help="thu muc dau ra")
    args = ap.parse_args()

    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        sys.exit("Chua dat GEMINI_API_KEY (xem huong dan dau file).")
    try:
        from google import genai
        from google.genai import types
    except ImportError:
        sys.exit("Can cai: pip install google-genai")

    client = genai.Client(api_key=key)
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
        rate = 24000
        for lan in range(6):
            try:
                pcm, rate = lay_pcm(client, types, args.model, args.giong, noidung)
                break
            except Exception as e:
                msg = str(e)
                wait = 25 if ("429" in msg or "RESOURCE_EXHAUSTED" in msg) else 5
                print("  loi (%s), thu lai sau %ds..." % (msg[:200], wait))
                time.sleep(wait)
        if pcm is None:
            print("[%d/%d] %s: BO QUA (that bai)" % (i, len(ds), ten))
            loi += 1
            continue
        pcm = doi_toc_do(cat_lang(pcm, rate), rate, OUT_RATE)
        ghi_wav(path, pcm)
        print("[%d/%d] %s = %s  (%.1f giay)" % (i, len(ds), ten, noidung, len(pcm) / OUT_RATE))
        time.sleep(1.0)

    print("Xong. Loi: %d. Cac file nam trong thu muc %s" % (loi, args.ra))
    if loi:
        print("Chay lai lenh nay de tao not cac file con thieu.")


if __name__ == "__main__":
    main()

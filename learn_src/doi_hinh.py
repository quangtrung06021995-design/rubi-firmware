# Doi hinh hoc song ngu cho Rubi (chay tren may tinh Windows/Mac/Linux).
# Cach dung: dat file nay vao thu muc co cac thu muc chu de (trai_cay, dong_vat, do_vat, hinh_khoi ...).
# Moi thu muc chu de co cac hinh .png (ten file = tu tieng Anh, vi du banana.png) va file tu.txt, moi dong:
#     ten_file|tieng Anh|tieng Viet        vi du:  banana|banana|qua chuoi
# Chay:  py doi_hinh.py      (lan dau can:  py -m pip install pillow)
# Ket qua nam trong thu muc "rubi_hoc"  ->  chep ca thu muc "rubi_hoc" vao the nho thanh  rubi/hoc
import os, sys
try:
    from PIL import Image
except ImportError:
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pillow"])
    from PIL import Image

SIZE = 140
here = os.path.dirname(os.path.abspath(__file__))
out_root = os.path.join(here, "rubi_hoc")
count = 0
for topic in sorted(os.listdir(here)):
    tdir = os.path.join(here, topic)
    words = os.path.join(tdir, "tu.txt")
    if not os.path.isdir(tdir) or topic == "rubi_hoc" or not os.path.exists(words):
        continue
    odir = os.path.join(out_root, topic)
    os.makedirs(odir, exist_ok=True)
    kept = []
    for line in open(words, encoding="utf-8-sig"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 3:
            print("  Dong sai (can 3 phan ngan cach bang |):", line)
            continue
        name, en, vi = parts
        src = os.path.join(tdir, name + ".png")
        if not os.path.exists(src):
            print("  Khong thay hinh:", src)
            continue
        im = Image.open(src).convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(bg, im).convert("RGB")
        im.thumbnail((SIZE, SIZE), Image.LANCZOS)
        canvas = Image.new("RGB", (SIZE, SIZE), (255, 255, 255))
        canvas.paste(im, ((SIZE - im.size[0]) // 2, (SIZE - im.size[1]) // 2))
        data = bytearray()
        for r, g, b in canvas.getdata():
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            data += bytes((v & 0xFF, v >> 8))
        with open(os.path.join(odir, name + ".bin"), "wb") as f:
            f.write(data)
        kept.append(f"{name}|{en}|{vi}")
        count += 1
    with open(os.path.join(odir, "tu.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(kept) + "\n")
    print(f"{topic}: {len(kept)} hinh")
print(f"Xong {count} hinh. Chep thu muc rubi_hoc vao the nho thanh rubi/hoc")
if sys.stdin.isatty() and os.name == "nt":
    input("Nhan Enter de dong...")

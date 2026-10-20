#!/usr/bin/env python3
"""Rubi mic test: receives recordings from the mic-test firmware and saves WAV files.
Usage:  pip install pyserial   then   python record_mic.py COM5   (Linux/Mac: /dev/ttyUSB0)
Optional: python record_mic.py COM5 --label 50cm   (adds a label to file names)
Speak a normal sentence when you see 'SPEAK NOW'. Stop with Ctrl+C."""
import argparse, base64, math, struct, sys, time, wave
import serial

ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("--baud", type=int, default=115200)
ap.add_argument("--label", default="test")
a = ap.parse_args()
ser = serial.Serial(a.port, a.baud, timeout=1)
try:
    ser.dtr = False; ser.rts = False
except Exception:
    pass
print(f"Opened {a.port} @ {a.baud}. If nothing appears, press RESET on the board.")
cur = None; data = bytearray(); n = 0
while True:
    line = ser.readline().decode("ascii", "ignore").strip()
    if not line.startswith("@"):
        continue
    if line.startswith("@HELLO"): print(line)
    elif line.startswith("@CUE"):
        print(f"  {line}  -> be ready")
    elif line.startswith("@REC"):
        print(">>> SPEAK NOW (6 s) <<<  " + line)
    elif line.startswith("@BEGIN"):
        kv = dict(x.split("=") for x in line.split()[1:])
        cur = {"gain": int(kv["gain"]), "rate": int(kv["rate"])}; data = bytearray()
        print("Receiving... (about 30-60 s, do not press Ctrl+C)")
    elif line.startswith("@D ") and cur is not None:
        try: data += base64.b64decode(line[3:])
        except Exception: print("bad line skipped")
        if len(data) % 19200 < 192:
            print(f"  received {len(data)//1024} KB of {cur['rate']*12//1024} KB", end="\r", flush=True)
    elif line.startswith("@END") and cur is not None:
        n += 1
        fn = f"mic_{a.label}_gain{cur['gain']}_{n:02d}.wav"
        with wave.open(fn, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(cur["rate"]); w.writeframes(bytes(data))
        s = struct.unpack("<%dh" % (len(data)//2), bytes(data))
        peak = max(abs(x) for x in s) if s else 0
        rms = math.sqrt(sum(x*x for x in s)/len(s)) if s else 0
        clip = sum(1 for x in s if abs(x) >= 32000)
        print(f"Saved {fn}  peak={peak}/32767  rms={rms:.0f}  clipped_samples={clip}")
        cur = None
    elif line.startswith("@ERR"): print(line)

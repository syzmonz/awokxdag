#!/usr/bin/env python3
"""Generate the Dual C5 Mini boot splash bitmap from the source art.

Downsamples assets/boot_screen_source.png to fit the Mini's 128x128 ST7735
(preserving the 3:4 aspect -> 96x128, centered horizontally), thresholds it to
1-bit, and emits an XBM-style PROGMEM C header (set bit = lit/white pixel, so the
Mini splash draws it white-on-black to match the Touch boot art).

Also writes an upscaled PNG preview so the 1-bit result can be eyeballed before
it is committed.

Usage:
  python3 scripts/gen_mini_boot.py [--threshold N] [--out HEADER] [--preview PNG]
"""
import argparse
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "assets" / "boot_screen_source.png"
FIT_W, FIT_H = 96, 128  # logo box; centered on the 128x128 panel by the firmware


def build(threshold: int):
    img = Image.open(SRC).convert("L").resize((FIT_W, FIT_H), Image.LANCZOS)
    # Lit (white) logo pixels become set bits.
    bits = img.point(lambda p: 255 if p >= threshold else 0, mode="1")
    return bits


def to_xbm_bytes(bits: Image.Image):
    px = bits.load()
    row_bytes = (FIT_W + 7) // 8
    out = bytearray()
    for y in range(FIT_H):
        for b in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < FIT_W and px[x, y]:  # '1' mode: nonzero == white == lit
                    byte |= 1 << bit  # XBM is LSB-first within each byte
            out.append(byte)
    return out


def write_header(data: bytearray, path: str):
    lines = ["#pragma once", "", "#include <Arduino.h>", "",
             f"constexpr int kMiniBootScreenWidth = {FIT_W};",
             f"constexpr int kMiniBootScreenHeight = {FIT_H};",
             "const uint8_t kMiniBootScreenBitmap[] PROGMEM = {"]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 12])
        lines.append("  " + chunk + ",")
    lines.append("};")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_preview(bits: Image.Image, path: str, scale: int = 3):
    # White logo on black, centered on a 128x128 panel, upscaled for visibility.
    panel = Image.new("L", (128, 128), 0)
    panel.paste(bits.convert("L"), ((128 - FIT_W) // 2, 0))
    panel.resize((128 * scale, 128 * scale), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=int, default=96)
    ap.add_argument("--out", default=str(ROOT / "AWOKxDAG" / "mini_boot_screen_data.h"))
    ap.add_argument("--preview", default="")
    args = ap.parse_args()
    bits = build(args.threshold)
    write_header(to_xbm_bytes(bits), args.out)
    if args.preview:
        write_preview(bits, args.preview)
    print(f"wrote {args.out} (threshold {args.threshold})")
    if args.preview:
        print(f"wrote {args.preview}")


if __name__ == "__main__":
    main()

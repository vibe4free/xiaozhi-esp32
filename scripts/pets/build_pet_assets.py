#!/usr/bin/env python3
"""
Build an assets.bin for the XiaoZhi 'assets' (spiffs) partition from a Codex pet.

Generic for any Codex pet: given a spritesheet.webp (192x208 cells, 8 columns,
9 standard animation rows), it extracts every used frame, optionally downscales,
converts to LVGL RGB565A8 (drawn directly from mmap'd flash on the device, no
decode buffer), and packs them into the project's custom asset container:

    [12B header: files(u32), checksum(u32), stored_len(u32)]
    [files x 44B table entries: name[32], size(u32), offset(u32), w(u16), h(u16)]
    [data region: each asset = b'ZZ' + payload]

checksum = sum of UNSIGNED bytes over (table+data) & 0xFFFF  (RISC-V char is unsigned)

A 'pet_manifest.json' asset describes the animations so the firmware loader stays
generic. A minimal 'index.json' keeps LvglStrategy::Apply happy (built-in emoji
remain as fallback).

Usage:
    build_pet_assets.py --spritesheet <pet>/spritesheet.webp --pet <name> \
        --lvgl-image-py <path> --output <assets.bin> [--scale 0.9] [--max-frames 4]
"""
import argparse, json, os, struct, subprocess, sys, tempfile
from PIL import Image

# The 9 standard Codex animation rows (row index -> name), with the used-frame
# count per row for evebot. Trailing cells in each row are transparent.
ROWS = [
    ("idle",          7, 180),
    ("running_right", 8, 140),
    ("running_left",  8, 140),
    ("waving",        4, 180),
    ("jumping",       5, 150),
    ("failed",        8, 150),
    ("waiting",       6, 180),
    ("running",       6, 180),
    ("review",        6, 180),
]
CW, CH = 192, 208
COLS = 8

NAME_LEN = 32
ENT = NAME_LEN + 4 + 4 + 2 + 2  # 44


def used_frame_count(alpha_cells):
    return sum(1 for a in alpha_cells if a)


def detect_used(sheet):
    """Return {row: used_count} by scanning alpha, to stay generic per pet."""
    import numpy as np
    arr = np.array(sheet)
    used = {}
    for r in range(len(ROWS)):
        cnt = 0
        for c in range(COLS):
            cell = arr[r*CH:(r+1)*CH, c*CW:(c+1)*CW, 3]
            if (cell > 16).mean() > 0.005:
                cnt += 1
        used[r] = cnt
    return used


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spritesheet", required=True)
    ap.add_argument("--pet", required=True)
    ap.add_argument("--lvgl-image-py", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--max-frames", type=int, default=0)  # 0 = keep all used frames
    ap.add_argument("--cf", default="I8",
                    help="LVGL color format: I8 (indexed, ~3x smaller, direct-draw) "
                         "or RGB565A8 (true-color+alpha)")
    args = ap.parse_args()

    sheet = Image.open(args.spritesheet).convert("RGBA")
    if sheet.size != (CW*COLS, CH*len(ROWS)):
        print(f"WARN: sheet {sheet.size} != expected {(CW*COLS, CH*len(ROWS))}")
    detected = detect_used(sheet)

    tw, th = int(round(CW*args.scale)), int(round(CH*args.scale))
    tmp = tempfile.mkdtemp(prefix="petframes_")
    png_dir = os.path.join(tmp, "png"); os.makedirs(png_dir)

    manifest = {"pet": args.pet, "cell": [tw, th], "animations": []}
    frame_files = []  # (asset_name, png_path)
    for r, (name, default_cnt, interval) in enumerate(ROWS):
        cnt = detected.get(r, default_cnt) or default_cnt
        # subsample evenly to at most max_frames
        idxs = list(range(cnt))
        if args.max_frames and cnt > args.max_frames:
            step = cnt / args.max_frames
            idxs = [int(i*step) for i in range(args.max_frames)]
        anim = {"name": name, "interval_ms": interval, "frames": len(idxs),
                "available": cnt}
        manifest["animations"].append(anim)
        for i, c in enumerate(idxs):
            cell = sheet.crop((c*CW, r*CH, (c+1)*CW, (r+1)*CH))
            if args.scale != 1.0:
                cell = cell.resize((tw, th), Image.LANCZOS)
            an = f"{name}_{i}"
            p = os.path.join(png_dir, an + ".png")
            cell.save(p)
            frame_files.append((an, p))

    # Convert all PNGs to LVGL .bin (12B header + payload) in the chosen format.
    bin_dir = os.path.join(tmp, "bin"); os.makedirs(bin_dir)
    subprocess.run([sys.executable, args.lvgl_image_py, "--ofmt", "BIN",
                    "--cf", args.cf, "-o", bin_dir, png_dir], check=True)

    # Assemble asset list: frames + manifest + index.json
    assets = []  # (name, payload_bytes, w, h)
    for an, _ in frame_files:
        b = open(os.path.join(bin_dir, an + ".bin"), "rb").read()
        assets.append((an, b, tw, th))
    manifest_bytes = json.dumps(manifest).encode()
    assets.append(("pet_manifest.json", manifest_bytes, 0, 0))
    # Minimal index.json: version only. Omitting text_font keeps the built-in
    # font; omitting emoji_collection keeps the built-in emoji as fallback so
    # LvglStrategy::Apply neither overrides text rendering nor the emoji set.
    index = {"version": 1}
    assets.append(("index.json", json.dumps(index).encode(), 0, 0))

    # Build table + data region
    table = b""
    data = b""
    offset = 0
    for name, payload, w, h in assets:
        nb = name.encode()
        assert len(nb) < NAME_LEN, f"name too long: {name}"
        table += nb + b"\x00"*(NAME_LEN-len(nb))
        table += struct.pack("<IIHH", len(payload), offset, w, h)
        data += b"ZZ" + payload
        offset += 2 + len(payload)

    body = table + data
    checksum = sum(body) & 0xFFFF   # unsigned byte sum
    header = struct.pack("<III", len(assets), checksum, len(body))
    out = header + body
    with open(args.output, "wb") as f:
        f.write(out)

    print(f"pet='{args.pet}' cf={args.cf} scale={args.scale} "
          f"max_frames={args.max_frames or 'all'}")
    print(f"output={args.output} size={len(out)} bytes ({len(out)/1024/1024:.2f} MB)")

    # Per-animation frame counts (packed vs available in the spritesheet).
    print(f"frames per animation ({len(manifest['animations'])} animations, "
          f"cell {tw}x{th}):")
    total = 0
    for a in manifest["animations"]:
        total += a["frames"]
        note = "" if a["frames"] == a["available"] else \
               f"  (subsampled from {a['available']})"
        print(f"  {a['name']:<14} {a['frames']:>2} frames @ {a['interval_ms']}ms{note}")
    print(f"  {'TOTAL':<14} {total:>2} frames")


if __name__ == "__main__":
    main()

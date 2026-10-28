#!/usr/bin/env python3
# make-icon-sheet.py —— 小蜜蜂图标候选对照拼版
#
# 用途：定稿前把「同一个参考图 × 几种底板/占比/是否反色」并排摊开给人看。
# 每一列都是同一份数据（同��张参考图、同一套尺寸梯），只有处理方式不同，
# 所以差异只能来自处理方式本身 —— 这正是对照图比文字描述有效的地方。
#
# 用法：
#   python tools\build\make-icon-sheet.py --source resources\bee-source.png \
#          --baseline resources\app.ico --out build\icon-compare.png
#
# 「现状」列从现行 app.ico 里抽帧，没有就整列省掉。
#
# 本脚本只出图，不碰 resources\app.ico。

import argparse
import importlib.util
import io
import os
import struct
import sys

# 加载同目录的 render-bee-art.py 时不要生成 __pycache__ ——
# 本仓库按目录逐条列 .gitignore，放一个 Python 缓存目录进来只会污染 git status。
sys.dont_write_bytecode = True

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def _load_rba():
    """render-bee-art.py 的文件名带连字符，不能直接 import —— 按路径加载。"""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "render-bee-art.py")
    spec = importlib.util.spec_from_file_location("render_bee_art", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rba = _load_rba()

BG = (0xEE, 0xF0, 0xF4)
CARD = (0xFF, 0xFF, 0xFF)
BORDER = (0xDC, 0xE0, 0xE6)
DARK = (0x24, 0x28, 0x30)
GREY = (0x6B, 0x72, 0x7D)
ACCENT = (0xC0, 0x7A, 0x12)

# 候选列。plate 为 None 表示透明底。
CANDIDATES = [
    {"title": "藏青底板 · 占 80%", "note": "沿用现行参数",
     "plate": rba.NAVY, "coverage": 0.80, "invert": False},
    {"title": "藏青底板 · 占 88%", "note": "蜂更大，小尺寸更清楚",
     "plate": rba.NAVY, "coverage": 0.88, "invert": False},
    {"title": "纯黑底板 · 占 88%", "note": "对比最强，但丢了品牌色",
     "plate": (0x10, 0x11, 0x14), "coverage": 0.88, "invert": False},
    {"title": "反色黑蜂 + 浅底板", "note": "白蜂翻成黑蜂，条纹翻成白",
     "plate": rba.LIGHT, "coverage": 0.88, "invert": True},
]

ROWS = [
    {"size": 256, "zoom": 1, "label": "256 px"},
    {"size": 64, "zoom": 1, "label": "64 px"},
    {"size": 32, "zoom": 1, "label": "32 px"},
    {"size": 16, "zoom": 1, "label": "16 px"},
    {"size": 16, "zoom": 8, "label": "16 px x8 最近邻"},
    {"size": 32, "zoom": 4, "label": "32 px x4 最近邻"},
]

SIZES = (16, 24, 32, 48, 64, 128, 256)


def load_font(size, bold=False):
    """中文字体：优先微软雅黑，找不到就退回 Segoe UI / DejaVu。"""
    cands = [("C:/Windows/Fonts/msyhbd.ttc" if bold else "C:/Windows/Fonts/msyh.ttc"),
             ("C:/Windows/Fonts/msyh.ttc",),
             ("C:/Windows/Fonts/segoeui.ttf",),
             ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",)]
    for c in cands:
        for path in c:
            if os.path.exists(path):
                try:
                    return ImageFont.truetype(path, size)
                except OSError:
                    continue
    return ImageFont.load_default()


def invert_rgba(rgba):
    """只反转前景像素的 RGB，alpha 不动；透明区保持 0。"""
    out = rgba.copy()
    rgb = rgba[:, :, :3].astype(np.uint16)
    out[:, :, :3] = np.where(rgba[:, :, 3:4] > 0, (255 - rgb).astype(np.uint8), 0)
    return out


def render_candidate(rgba, plate, coverage, invert, sizes):
    """返回 {size: RGBA Image}。"""
    art = invert_rgba(rgba) if invert else rgba
    out = {}
    for s in sizes:
        pm, a = rba.fit_into(art, s, coverage)
        if plate is None:
            safe = np.maximum(a, 1e-6)
            rgb = np.where(a[:, :, None] > 0, pm * 255.0 / safe[:, :, None], 0.0)
            arr = np.dstack([rgb, a])
        else:
            pa = rba.plate_mask(s)
            prgb = np.zeros((s, s, 3))
            prgb[:, :, 0], prgb[:, :, 1], prgb[:, :, 2] = plate
            arr = rba.compose(pm, a, prgb, pa)
        out[s] = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
    return out


def read_ico_frames(path):
    """从 .ico 里抽出各尺寸的帧，返回 {size: RGBA Image}。"""
    data = open(path, "rb").read()
    _res, typ, n = struct.unpack("<HHH", data[:6])
    if typ != 1:
        raise ValueError("not an icon file: %s" % path)
    frames = {}
    for i in range(n):
        off = 6 + 16 * i
        w, _h, _c, _r, _p, _b, size, offset = struct.unpack("<BBBBHHII", data[off:off + 16])
        dim = w or 256
        frames[dim] = Image.open(io.BytesIO(data[offset:offset + size])).convert("RGBA")
    return frames


def rounded_card(draw, box, radius=14):
    draw.rounded_rectangle(box, radius=radius, fill=CARD, outline=BORDER, width=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baseline", default="")
    ap.add_argument("--title", default="小蜜蜂图标候选对照")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--footer", default="看图选一列：选哪一列，app.ico 就按哪一列生成。")
    args = ap.parse_args()

    rgba, fg = rba.matte(args.source)
    rgba = rba.square_canvas(rba.crop_to_content(rgba, fg))

    columns = []
    if args.baseline and os.path.exists(args.baseline):
        columns.append({"title": "现状：现行图标", "note": "对照基线",
                        "frames": read_ico_frames(args.baseline)})
    for c in CANDIDATES:
        columns.append({
            "title": c["title"], "note": c["note"],
            "frames": render_candidate(rgba, c["plate"], c["coverage"],
                                       c["invert"], SIZES)})

    cell = 256
    gutter = 104
    padX = 18
    padY = 20
    headH = 96
    gap = 14
    footH = 56

    row_heights = [max(int(r["size"] * r["zoom"]), 46) for r in ROWS]
    body_h = sum(row_heights) + gap * (len(ROWS) + 1)
    card_top = padY + headH
    card_h = body_h + 10
    width = gutter + padX + len(columns) * (cell + padX)
    height = card_top + card_h + padY + footH

    sheet = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(sheet)

    f_title = load_font(21, bold=True)
    f_col = load_font(15, bold=True)
    f_note = load_font(12)
    f_row = load_font(12)

    d.text((padX, padY), args.title, font=f_title, fill=DARK)
    if args.subtitle:
        d.text((padX, padY + 30), args.subtitle, font=f_note, fill=GREY)

    for c, col in enumerate(columns):
        x = gutter + padX + c * (cell + padX)
        rounded_card(d, (x, card_top, x + cell, card_top + card_h))
        d.text((x, card_top - 42), col["title"], font=f_col, fill=DARK)
        d.text((x, card_top - 21), col["note"], font=f_note, fill=GREY)

    y = card_top + 8
    for r, rh in zip(ROWS, row_heights):
        d.text((padX - 6, y + rh / 2 - 12), r["label"], font=f_row, fill=GREY)
        for c, col in enumerate(columns):
            x = gutter + padX + c * (cell + padX)
            img = col["frames"].get(r["size"])
            if img is None:
                continue
            w = r["size"] * r["zoom"]
            dx = x + (cell - w) / 2
            dy = y + (rh - w) / 2
            if r["zoom"] > 1:
                # 最近邻放大：像素块要方，不能被插值抹圆。
                img = img.resize((w, w), Image.NEAREST)
            sheet.paste(img, (int(dx), int(dy)), img)
        y += rh + gap

    d.text((padX, card_top + card_h + 10), args.footer, font=f_note, fill=ACCENT)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)
    sheet.save(args.out)
    print("[OK] %s = %d bytes (%d columns)" % (args.out, os.path.getsize(args.out), len(columns)))


if __name__ == "__main__":
    main()

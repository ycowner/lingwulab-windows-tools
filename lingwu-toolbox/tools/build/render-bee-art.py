#!/usr/bin/env python3
# render-bee-art.py —— 把参考图处理成小蜜蜂图标的各尺寸 PNG
#
# 为什么不直接拿参考图当图标：
#   参考图是 1254x1254、白底、无 alpha 通道的位图。直接塞进 .ico 的话，
#   16 px 档就是一团糊，而且底下一块白方砖在深色任务栏上非常刺眼。
#   所以这里先把白底**抠掉**（而不是简单地按亮度抠），再合成到品牌藏青底板上。
#
# 【抠图为什么要这么绕】
#   白色在这张图里出现了两次：背景，以及**眼睛里的高光**。
#   按亮度键控会把高光也一起抠穿；按区域 flood fill 又会把边缘的
#   抗锯齿过渡像素（半透明混色）误判成背景，边缘留下一圈白毛边。
#   这里的做法是：
#     ① 从画面四角 flood fill 出「连通到边界」的背景区域 —— 眼睛高光是内部孤岛，不会被填到；
#     ② 背景区域再向内腐蚀 1 px，剩下的就是纯前景；
#     ③ 前景与纯前景之间的那一圈就是抗锯齿过渡带，逐像素解出它的覆盖率 alpha，
#        并用最近的纯前景颜色反算出不带背景色的原色。
#   这样边缘是干净的半透明，而不是白边。
#
# 缩放用的是**预乘 alpha** 的做法（先乘 alpha 再缩放，最后再合成），
# 否则透明区在缩放时会把黑色拖进边缘，出现黑边。
#
# 用法：
#   python tools\build\render-bee-art.py --source resources\bee-source.png --outdir build\bee-art
#
# 本脚本只做「位图 -> 各尺寸 PNG」，打包成 .ico 由 make-icon.ps1 完成
# （打包段是仓库约定，两条产线共用同一段代码）。

import argparse
import os
import sys
import warnings

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

NAVY = (0x1E, 0x22, 0x2B)
LIGHT = (0xF3, 0xF4, 0xF6)


def shift(a, dy, dx, fill=0.0):
    """把数组平移 (dy, dx)，越界处补 fill。"""
    out = np.full_like(a, fill)
    h, w = a.shape[:2]
    ys0, ys1 = max(0, dy), min(h, h + dy)
    xs0, xs1 = max(0, dx), min(w, w + dx)
    yd0, yd1 = max(0, -dy), min(h, h - dy)
    xd0, xd1 = max(0, -dx), min(w, w - dx)
    out[yd0:yd1, xd0:xd1] = a[ys0:ys1, xs0:xs1]
    return out


def matte(src_path):
    """返回 (rgba uint8 HxWx4, fg 掩码)。背景被抠成透明。

    背景色**不假定是白色**：从左上角像素取实测值当背景色。
    2026-10-28 换成黑白剪影参考图后，背景是纯黑 —— 沿用「背景 = 255」的
    覆盖率解法会把整圈抗锯齿边缘算成不透明（边缘发硬），所以这里把
    背景色做成变量，明/暗底同一套代码都能解。
    """
    im = Image.open(src_path).convert("RGB")
    w, h = im.size

    # 背景色：取四角里出现最多的那个（参考图是纯色底，四角必然一致）。
    corners = [im.getpixel(p) for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
    bg_color = np.array(max(set(corners), key=corners.count), dtype=np.float64)

    # ① 从左上角 flood fill 连通到边界的背景
    work = im.copy()
    ImageDraw.floodfill(work, (0, 0), (255, 0, 255), thresh=40)
    arr = np.asarray(work)
    bg = (arr[:, :, 0] == 255) & (arr[:, :, 1] == 0) & (arr[:, :, 2] == 255)
    rgb = np.asarray(im).astype(np.float64)
    fg = ~bg

    # ② 纯前景 = 前景再往里收 1 px
    solid = np.asarray(Image.fromarray((fg * 255).astype(np.uint8))
                       .filter(ImageFilter.MinFilter(3))) > 127
    ring = fg & ~solid

    # ③ 过渡带里的原色：取最近一圈纯前景邻居的平均色（逐轮向外扩散 3 轮）
    known = solid.copy()
    color = rgb.copy()
    offsets = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    for _ in range(3):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(fg.shape, dtype=np.float64)
        for dy, dx in offsets:
            m = shift(known.astype(np.float64), dy, dx)
            acc += m[:, :, None] * shift(rgb, dy, dx)
            cnt += m
        grow = (~known) & ring & (cnt > 0)
        if not grow.any():
            break
        color[grow] = acc[grow] / cnt[grow][:, None]
        known |= grow

    # ④ 逐通道解覆盖率：观测值 = alpha*C + (1-alpha)*BG
    #    BG 是实测背景色（白底 255 / 黑底 0），所以明底暗底同一套公式。
    denom = color - bg_color[None, None, :]
    num = rgb - bg_color[None, None, :]
    usable = np.abs(denom) > 25
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(usable, num / np.where(usable, denom, 1.0), np.nan)
    # 有些像素三个通道都不可用（颜色接近纯白），整行都是 NaN，
    # nanmedian 会对这种切片发一条 RuntimeWarning。这些像素随后被
    # 「NaN -> 1.0」兜住，所以这里直接静音，别让构建日志里挂一条假警告。
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        with np.errstate(invalid="ignore"):
            alpha = np.nanmedian(ratio, axis=2)
    alpha = np.where(np.isnan(alpha), 1.0, alpha)
    alpha = np.clip(alpha, 0.0, 1.0)
    alpha = np.where(solid, 1.0, alpha)
    alpha = np.where(bg, 0.0, alpha)

    out_rgb = np.where(solid[:, :, None], rgb, color)
    out_rgb = np.where(bg[:, :, None], color, out_rgb)

    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[:, :, :3] = np.clip(out_rgb, 0, 255).astype(np.uint8)
    rgba[:, :, 3] = (alpha * 255).astype(np.uint8)
    return rgba, fg


def crop_to_content(rgba, fg):
    ys, xs = np.nonzero(fg)
    if len(xs) == 0:
        return rgba
    pad = 4
    x0 = max(0, xs.min() - pad)
    x1 = min(rgba.shape[1], xs.max() + 1 + pad)
    y0 = max(0, ys.min() - pad)
    y1 = min(rgba.shape[0], ys.max() + 1 + pad)
    return rgba[y0:y1, x0:x1]


def square_canvas(rgba):
    """补成正方形，保持比例（蜂比宽，高方向补白）。"""
    h, w = rgba.shape[:2]
    side = max(h, w)
    out = np.zeros((side, side, 4), dtype=np.uint8)
    oy = (side - h) // 2
    ox = (side - w) // 2
    out[oy:oy + h, ox:ox + w] = rgba
    return out


def plate_mask(size, radius_ratio=0.225, inset_ratio=0.02):
    """圆角方块底板（4 倍超采样画完再缩回来，边缘才是抗锯齿的）。"""
    s = size * 4
    m = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(m)
    inset = s * inset_ratio
    d.rounded_rectangle([inset, inset, s - inset, s - inset],
                        radius=s * radius_ratio, fill=255)
    return np.asarray(m.resize((size, size), Image.LANCZOS)).astype(np.float64)


def resize_premul(rgba, size):
    """预乘 alpha 缩放：先 rgb*a，再缩放 rgb 与 a 两张图，避免透明区把黑拖进边缘。"""
    if isinstance(size, int):
        size = (size, size)
    a = rgba[:, :, 3].astype(np.float64)
    rgb = rgba[:, :, :3].astype(np.float64)
    pm = (rgb * a[:, :, None] / 255.0)
    pm_img = Image.fromarray(np.clip(pm, 0, 255).astype(np.uint8))
    a_img = Image.fromarray(a.astype(np.uint8))
    pm_r = np.asarray(pm_img.resize(size, Image.LANCZOS)).astype(np.float64)
    a_r = np.asarray(a_img.resize(size, Image.LANCZOS)).astype(np.float64)
    return pm_r, a_r


def compose(bee_pm, bee_a, plate_rgb, plate_a):
    """over 合成：蜂 叠在 底板 上。"""
    inv = 1.0 - bee_a / 255.0
    out_pm = bee_pm + plate_rgb * plate_a[:, :, None] * inv[:, :, None] / 255.0
    out_a = bee_a + plate_a * inv
    safe = np.maximum(out_a, 1e-6)
    out_rgb = np.where(out_a[:, :, None] > 0, out_pm * 255.0 / safe[:, :, None], 0.0)
    out = np.zeros(bee_pm.shape[:2] + (4,), dtype=np.uint8)
    out[:, :, :3] = np.clip(out_rgb, 0, 255).astype(np.uint8)
    out[:, :, 3] = np.clip(out_a, 0, 255).astype(np.uint8)
    return out


def render_variant(rgba, size, plate):
    """plate: None 表示透明底，否则是 (r,g,b)。返回 RGBA numpy。"""
    bee_pm, bee_a = resize_premul(rgba, size)
    if plate is None:
        mask = np.full((size, size), 255.0)
        plate_rgb = np.zeros((size, size, 3))
        plate_a = mask
    else:
        plate_a = plate_mask(size)
        plate_rgb = np.zeros((size, size, 3))
        plate_rgb[:, :, 0] = plate[0]
        plate_rgb[:, :, 1] = plate[1]
        plate_rgb[:, :, 2] = plate[2]
    return compose(bee_pm, bee_a, plate_rgb, plate_a)


def fit_into(rgba, size, coverage=0.84):
    """把蜂缩放到目标画布里（保留比例），返回 RGBA numpy。"""
    h, w = rgba.shape[:2]
    side = int(round(size * coverage))
    scale = side / max(h, w)
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    pm, a = resize_premul(rgba, (nw, nh))
    out_pm = np.zeros((size, size, 3))
    out_a = np.zeros((size, size))
    ox = (size - nw) // 2
    oy = (size - nh) // 2
    out_pm[oy:oy + nh, ox:ox + nw] = pm
    out_a[oy:oy + nh, ox:ox + nw] = a
    return out_pm, out_a


PLATES = {
    "navy": NAVY,
    "light": LIGHT,
    "black": (0x10, 0x11, 0x14),
    "none": None,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--coverage", type=float, default=0.80)
    # 只出指定底板；不给就三档都出（保持旧调用方式）。
    ap.add_argument("--plate", choices=sorted(PLATES), default="")
    # 反色：白蜂变黑蜂（黑条纹/眼睛变白）。只动前景 RGB，alpha 不动。
    ap.add_argument("--invert", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    rgba, fg = matte(args.source)
    rgba = square_canvas(crop_to_content(rgba, fg))
    if args.invert:
        # 只反转前景像素；透明区保持 0，避免半透明像素被翻成脏色。
        rgb = rgba[:, :, :3].astype(np.uint16)
        rgba[:, :, :3] = np.where(
            rgba[:, :, 3:4] > 0, (255 - rgb).astype(np.uint8), 0)

    plates = ([args.plate] if args.plate else ["navy", "light", "none"])
    # 反色时文件名加 "-inv" 后缀：同一目录里白蜂版与黑蜂版不能同名，
    # 否则第二次渲染会把第一次的结果覆盖掉（安静地少一张图）。
    for size in (16, 24, 32, 48, 64, 128, 256):
        for name in plates:
            plate = PLATES[name]
            pm, a = fit_into(rgba, size, args.coverage)
            if plate is None:
                # 透明底：保存前要解除预乘，否则半透明边缘会发暗
                safe = np.maximum(a, 1e-6)
                out_pm = np.where(a[:, :, None] > 0, pm * 255.0 / safe[:, :, None], 0.0)
                out_a = a
            else:
                pa = plate_mask(size)
                prgb = np.zeros((size, size, 3))
                prgb[:, :, 0], prgb[:, :, 1], prgb[:, :, 2] = plate
                out = compose(pm, a, prgb, pa)
                out_pm, out_a = out[:, :, :3].astype(np.float64), out[:, :, 3].astype(np.float64)
            img = Image.fromarray(np.dstack([
                np.clip(out_pm, 0, 255).astype(np.uint8),
                np.clip(out_a, 0, 255).astype(np.uint8)]))
            prefix = ("%s-inv" % name) if args.invert else name
            img.save(os.path.join(args.outdir, "%s-%d.png" % (prefix, size)))

    print("[OK] art rendered -> %s (plate=%s, coverage=%.2f, invert=%s)"
          % (args.outdir, ",".join(plates), args.coverage, args.invert))


if __name__ == "__main__":
    main()

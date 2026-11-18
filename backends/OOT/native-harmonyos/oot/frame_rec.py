"""Read a frame recording of the phone's screen (ohos/anative_window_ohos.c frame_rec: TOTK_OHOS_FRAME_REC) and measure
its flashes frame by frame.

usage: python native-harmonyos/oot/frame_rec.py <frame-rec.raw> [--out <dir>] [--webp] [--label <text>]
Prints per frame the mean luma and the share of near-white pixels (all channels >= 235), marks a frame as a flash when
its near-white share is above both neighbours' median by --jump (default 0.05 = 5 points of the picture) or its mean luma
jumps by --lum (default 25 of 255) against the median of the 6 frames around it, and writes:
  <out>/<label>-stats.tsv   frame, ms, mean luma, near-white share, flash
  <out>/<label>-flashes.png the flash frames (up to 8) next to the frame before each, for a look
  <out>/<label>.webp        the recording as an animated WebP at its real timing (--webp; half size)
The recording holds the whole presented picture, overlays included; the fps overlay's corner is left out of the stats."""
import argparse
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def frames(path):
    data = Path(path).read_bytes()
    o, out = 0, []
    while o + 24 <= len(data):
        magic, idx, t, w, h = struct.unpack_from('<IIQII', data, o)
        if magic != 0x314d5246:
            raise SystemExit(f'{path}: bad frame header at byte {o}')
        n = w * h * 3
        if o + 24 + n > len(data):
            break
        px = np.frombuffer(data, np.uint8, n, o + 24).reshape(h, w, 3)
        out.append((idx, t / 1000.0, px))
        o += 24 + n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('raw'); ap.add_argument('--out', default=None); ap.add_argument('--label', default=None)
    ap.add_argument('--webp', action='store_true'); ap.add_argument('--jump', type=float, default=0.05); ap.add_argument('--lum', type=float, default=25)
    ap.add_argument('--sheet', type=int, default=0, help='also write <label>-sheet.jpg with every n-th frame')
    ap.add_argument('--hud', default=None, help="HUD presence check against a reference recording's medians ('-': this recording's own)")
    ap.add_argument('--ref', default=None, help='a baseline recording of the same view: count frames whose HUD / scene regions differ from its median frame')
    ap.add_argument('--region-diff', type=float, default=18.0, help='mean absolute difference a channel (0-255) above which a region counts as different')
    ap.add_argument('--blob', action='store_true', help='isolated white-blob frames (white foliage / doorway for one frame)')
    ap.add_argument('--blob-min', type=int, default=48, help='--blob: the fewest near-white grid samples an event needs')
    a = ap.parse_args()
    fr = frames(a.raw)
    if not fr:
        raise SystemExit('no frames')
    out = Path(a.out) if a.out else Path(a.raw).parent
    label = a.label or Path(a.raw).stem
    h, w = fr[0][2].shape[:2]
    # the picture without the fps overlay (top right: 28% of the width, 16% of the height) and the touch buttons' columns
    mask = np.ones((h, w), bool); mask[: int(h * 0.16), int(w * 0.72):] = False
    lum, white = [], []
    for _, _, px in fr:
        p = px.astype(np.float32)
        y = 0.299 * p[..., 0] + 0.587 * p[..., 1] + 0.114 * p[..., 2]
        lum.append(float(y[mask].mean()))
        white.append(float((px.min(axis=2)[mask] >= 235).mean()))
    lum, white = np.array(lum), np.array(white)
    flash = np.zeros(len(fr), bool)
    for i in range(len(fr)):
        lo, hi = max(0, i - 3), min(len(fr), i + 4)
        around = np.concatenate([white[lo:i], white[i + 1:hi]]); around_l = np.concatenate([lum[lo:i], lum[i + 1:hi]])
        if len(around) and (white[i] - np.median(around) > a.jump or lum[i] - np.median(around_l) > a.lum):
            flash[i] = True
    dur = (fr[-1][1] - fr[0][1]) / 1000.0
    fps = (len(fr) - 1) / dur if dur > 0 else 0
    with open(out / f'{label}-stats.tsv', 'w', encoding='utf-8') as f:
        f.write('frame\tms\tmean_luma\tnear_white\tflash\n')
        for (idx, ms, _), l, wv, fl in zip(fr, lum, white, flash):
            f.write(f'{idx}\t{ms:.1f}\t{l:.1f}\t{wv:.4f}\t{int(fl)}\n')
    print(f'{label}: {len(fr)} frames {w}x{h} over {dur:.2f} s ({fps:.1f} fps); mean luma {lum.mean():.1f} (min {lum.min():.1f}, max {lum.max():.1f}), '
          f'near-white share mean {white.mean() * 100:.2f}% (max {white.max() * 100:.1f}%); flash frames {int(flash.sum())} '
          f'({flash.sum() / max(dur, 1e-9):.2f} a second)')
    idx = [i for i in np.nonzero(flash)[0]][:8]
    if idx:
        tiles = []
        for i in idx:
            prev = fr[i - 1][2] if i > 0 else fr[i][2]
            tiles.append(np.concatenate([prev, fr[i][2]], axis=1))
        sheet = Image.fromarray(np.concatenate(tiles, axis=0)); sheet.save(out / f'{label}-flashes.png')
        print(f'  flash frames {", ".join(str(fr[i][0]) for i in idx)}: {out / (label + "-flashes.png")} (each row: the frame before, the flash)')
    if a.ref:   # the frames against a reference recording of the same view (its per-pixel median frame), by region
        ref = np.median(np.stack([px for _, _, px in frames(a.ref)]), axis=0).astype(np.float32)
        H, W = ref.shape[:2]
        regions = {'hearts': (0.02, 0.12, 0.09, 0.20), 'minimap': (0.68, 0.92, 0.65, 0.95), 'prompts': (0.86, 0.99, 0.36, 0.64),
                   'scene': (0.18, 0.85, 0.12, 0.88)}   # (y0, y1, x0, x1) as fractions of the frame
        bad = {k: 0 for k in regions}; worst = {k: 0.0 for k in regions}
        for _, _, px in fr:
            d = np.abs(px.astype(np.float32) - ref).mean(axis=2)
            for k, (y0, y1, x0, x1) in regions.items():
                v = float(d[int(y0 * H):int(y1 * H), int(x0 * W):int(x1 * W)].mean())
                worst[k] = max(worst[k], v)
                if v > a.region_diff:
                    bad[k] += 1
        print('  against ' + Path(a.ref).name + ': frames differing by more than %g a channel: ' % a.region_diff +
              ', '.join(f'{k} {bad[k]} (worst {worst[k]:.0f})' for k in regions))
    if a.hud:   # HUD presence independent of the background (walking runs): red heart pixels top left, map pixels bottom right
        def hud_counts(px):
            H2, W2 = px.shape[:2]
            hp = px[int(0.02 * H2):int(0.12 * H2), int(0.09 * W2):int(0.20 * W2)].astype(np.int16)
            hearts = int(((hp[..., 0] > 170) & (hp[..., 1] < 100) & (hp[..., 2] < 100)).sum())
            mp = px[int(0.68 * H2):int(0.92 * H2), int(0.70 * W2):int(0.90 * W2)].astype(np.int16)
            mapc = int(((mp[..., 0] > 150) & (mp[..., 1] > 130) & (mp[..., 2] < 120) & (mp[..., 0] - mp[..., 2] > 60)).sum())
            return hearts, mapc
        def prompt_text(px):   # light grey glyph pixels of the button prompts, bottom centre (~230 when "Move / Change View" shows)
            H2, W2 = px.shape[:2]
            p = px[int(0.86 * H2):int(0.99 * H2), int(0.30 * W2):int(0.70 * W2)].astype(np.int16)
            mn, mx = p.min(axis=2), p.max(axis=2)
            return int(((mn > 170) & (mx - mn < 45)).sum())
        cnt = np.array([hud_counts(px) for _, _, px in fr])
        refc = np.median(np.array([hud_counts(px) for _, _, px in frames(a.hud)]), axis=0) if a.hud != '-' else np.median(cnt, axis=0)
        bad_h = int(((cnt[:, 0] < 0.5 * refc[0]) | (cnt[:, 0] > 2.0 * refc[0] + 20)).sum())
        bad_m = int(((cnt[:, 1] < 0.5 * refc[1]) | (cnt[:, 1] > 2.0 * refc[1] + 50)).sum())
        # walking baselines show under 15 text pixels in 7-10 of 900 frames (fades between prompts); a lost glyph upload
        # (a123y-wlo5b) left the prompt bar empty for 636 frames
        txt = np.array([prompt_text(px) for _, _, px in fr]); no_txt = int((txt < 15).sum())
        print(f'  HUD presence (red heart pixels / map pixels per frame against the reference medians {refc[0]:.0f} / {refc[1]:.0f}): '
              f'hearts off in {bad_h} frames, minimap off in {bad_m} frames, prompt text missing in {no_txt} frames')
    if a.blob:
        # White blobs: near-white samples (all channels >= 235) on every 2nd pixel of every 2nd row, the fps overlay left
        # out; frame i is an event when its count is >= max(blob_min, S/1000) and >= 4x both neighbours' + S/2000. It
        # finds the one-frame white foliage / doorway at the treehouse camera cut of the walking route (oot-a126: 5 of 6
        # baseline walks, frames ~515-650) and the standing-view foliage blobs (a122bv-v2 #298, a122c-warmB #8); a fade
        # or a bright scene is not isolated and does not count.
        ys, xs = np.mgrid[0:h:2, 0:w:2]
        keep = ~((xs > 0.72 * w) & (ys < 0.16 * h)); S = int(keep.sum())
        bc = np.array([int(((px[0:h:2, 0:w:2].min(axis=2) >= 235) & keep).sum()) for _, _, px in fr])
        ev = [i for i in range(1, len(bc) - 1) if bc[i] >= max(a.blob_min, S / 1000) and bc[i] >= 4 * max(bc[i - 1], bc[i + 1]) + S / 2000]
        print(f'  white blobs: {len(ev)} isolated frames' + (': ' + ', '.join(f'{fr[i][0]} ({bc[i - 1]}->{bc[i]}->{bc[i + 1]} of {S})' for i in ev[:8]) if ev else ''))
    if a.sheet:   # every sheet-th frame in a grid of 4 columns, for a look at a whole run
        pick = list(range(0, len(fr), a.sheet))
        cols = 4; rows = (len(pick) + cols - 1) // cols
        tw, th = w // 2, h // 2
        sheet = Image.new('RGB', (tw * cols, th * rows))
        for n, i in enumerate(pick):
            sheet.paste(Image.fromarray(fr[i][2]).resize((tw, th)), ((n % cols) * tw, (n // cols) * th))
        sheet.save(out / f'{label}-sheet.jpg', quality=80)
        print(f'  sheet of every {a.sheet}th frame: {out / (label + "-sheet.jpg")}')
    if a.webp:
        ims =[Image.fromarray(px).resize((w // 2, h // 2)) for _, _, px in fr]
        durs = [max(20, int(round(fr[i + 1][1] - fr[i][1]))) for i in range(len(fr) - 1)] + [70]
        ims[0].save(out / f'{label}.webp', save_all=True, append_images=ims[1:], duration=durs, loop=0, quality=70)
        print(f'  animation: {out / (label + ".webp")}')


if __name__ == '__main__':
    main()

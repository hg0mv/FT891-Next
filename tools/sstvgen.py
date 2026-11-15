#!/usr/bin/env python3
"""SSTV-jel generátor teszthez: 48 kHz, 16 bit, nyers hang, pontos fázisfolytonos tónusokkal.
Használat: sstvgen.py MÓD ki.raw ki.rgb [--ofs Hz] [--ppm N] [--noise X]"""
import argparse
import numpy as np

SC = lambda scan: dict(k='S', w=320, h=256, scan=scan, sync=9, porch=1.5, sep=1.5)
MA = lambda scan: dict(k='M', w=320, h=256, scan=scan, sync=4.862, porch=.572, sep=.572)
PD = lambda scan, w, h: dict(k='P', w=w, h=h, scan=scan, sync=20, porch=2.08)
MODES = {'S1': (60, SC(138.24)), 'S2': (56, SC(88.064)), 'SDX': (76, SC(345.6)), 'M1': (44, MA(146.432)), 'M2': (40, MA(73.216)),
         'R36': (8, dict(k='R36', w=320, h=240, scan=88, sync=9, porch=3, sep=4.5)),
         'R72': (12, dict(k='R72', w=320, h=240, scan=138, sync=9, porch=3, sep=4.5)),
         'PD50': (93, PD(91.52, 320, 256)), 'PD120': (95, PD(121.6, 640, 496))}

def pattern(w, h):
    """Színsávok, alatta szürke ékek, ferde vonal a torzulás ellenőrzésére"""
    im = np.zeros((h, w, 3), dtype=np.uint8)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (30, 30, 30)]
    for i, c in enumerate(bars):
        im[:h // 2, i * w // 8:(i + 1) * w // 8] = c
    g = (np.arange(w) * 255 // (w - 1)).astype(np.uint8)
    for c in range(3): im[h // 2:, :, c] = g
    return im

class Tx:
    def __init__(s, ppm): s.f = []; s.d = []; s.r = 48000 * (1 + ppm * 1e-6)
    def t(s, freq, ms): s.f.append(np.full(1, float(freq))); s.d.append(ms)
    def px(s, vals, total_ms):
        n = len(vals)
        s.f.append(1500 + np.asarray(vals, float) / 255 * 800); s.d.append(total_ms)
    def samples(s, ofs):
        out = []; acc = 0.0
        for fr, ms in zip(s.f, s.d):
            exact = acc + ms * s.r / 1000
            n = int(round(exact)) - int(round(acc))
            acc = exact
            idx = np.minimum((np.arange(n) * len(fr) / max(n, 1)).astype(int), len(fr) - 1)
            out.append(fr[idx])
        return np.concatenate(out) + ofs

def to_yuv(a):
    r, g, b = [a[..., i].astype(float) for i in range(3)]
    y = .299 * r + .587 * g + .114 * b
    return y, np.clip((r - y) / 1.402 + 128, 0, 255), np.clip((b - y) / 1.772 + 128, 0, 255)

def build(mode, im, ppm):
    vis, m = MODES[mode]; T = Tx(ppm); h, w = im.shape[:2]
    T.t(1900, 300); T.t(1200, 10); T.t(1900, 300); T.t(1200, 30)
    par = 0
    for b in range(7):
        v = vis >> b & 1; par ^= v; T.t(1100 if v else 1300, 30)
    T.t(1100 if par else 1300, 30); T.t(1200, 30)
    k = m['k']; sc = m['scan']; sy = m['sync']; po = m['porch']; sep = m.get('sep', 0)
    if k == 'S': T.t(1200, 9)
    if k in ('S', 'M'):
        for l in im:
            r, g, b = l[:, 0], l[:, 1], l[:, 2]
            if k == 'S':
                T.t(1500, sep); T.px(g, sc); T.t(1500, sep); T.px(b, sc); T.t(1200, sy); T.t(1500, po); T.px(r, sc)
            else:
                T.t(1200, sy); T.t(1500, po); T.px(g, sc); T.t(1500, sep); T.px(b, sc); T.t(1500, sep); T.px(r, sc); T.t(1500, sep)
    elif k == 'R36':
        y, u, v = to_yuv(im)
        for i in range(h):
            T.t(1200, sy); T.t(1500, po); T.px(y[i], sc)
            T.t(1500 if i % 2 == 0 else 2300, sep); T.t(1900, 1.5); T.px((u if i % 2 == 0 else v)[i], sc / 2)
    elif k == 'R72':
        y, u, v = to_yuv(im)
        for i in range(h):
            T.t(1200, sy); T.t(1500, po); T.px(y[i], sc)
            T.t(1500, sep); T.t(1900, 1.5); T.px(u[i], sc / 2); T.t(2300, sep); T.t(1900, 1.5); T.px(v[i], sc / 2)
    else:
        y, u, v = to_yuv(im)
        for i in range(0, h, 2):
            T.t(1200, sy); T.t(1500, po); T.px(y[i], sc); T.px((u[i] + u[i + 1]) / 2, sc); T.px((v[i] + v[i + 1]) / 2, sc); T.px(y[i + 1], sc)
    return T

def main():
    a = argparse.ArgumentParser()
    a.add_argument('mode'); a.add_argument('raw'); a.add_argument('rgb')
    a.add_argument('--ofs', type=float, default=0); a.add_argument('--ppm', type=float, default=0); a.add_argument('--noise', type=float, default=0)
    a.add_argument('--no-vis', action='store_true')
    p = a.parse_args()
    w, h = MODES[p.mode][1]['w'], MODES[p.mode][1]['h']
    im = pattern(w, h); open(p.rgb, 'wb').write(im.tobytes())
    f = build(p.mode, im, p.ppm).samples(p.ofs)
    ph = np.cumsum(2 * np.pi * f / 48000)
    x = .35 * np.sin(ph)
    if p.noise: x = x + np.random.normal(0, p.noise, len(x))
    if p.no_vis: x = x[int(.91 * 48000):]
    lead = np.random.normal(0, .02, 24000)
    x = np.concatenate([lead, x, np.random.normal(0, .02, 24000)])
    (np.clip(x, -1, 1) * 32767).astype('<i2').tofile(p.raw)

if __name__ == '__main__':
    main()

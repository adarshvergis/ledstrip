"""Host-side DSP helpers: firmware biquad response and spectrum interpolation.

biquad_db mirrors dsp_biquad_design (firmware/Interface/signal_processing/dsp.c, RBJ Audio EQ
Cookbook), so the filter overlay shows what the device really applies.
"""

from __future__ import annotations

import cmath
import math


def pchip(xs: list[float], ys: list[float], xq: list[float]) -> list[float]:
    """Monotone cubic (Fritsch-Carlson) interpolation; xs and xq ascending. No overshoot."""
    n = len(xs)
    if n < 2:
        return [ys[0] if ys else 0.0] * len(xq)
    h = [xs[i + 1] - xs[i] for i in range(n - 1)]
    d = [(ys[i + 1] - ys[i]) / h[i] for i in range(n - 1)]
    m = [d[0]] + [0.0] * (n - 2) + [d[-1]]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] > 0:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
    out, j = [], 0
    for x in xq:
        if x <= xs[0]:
            out.append(ys[0])
            continue
        if x >= xs[-1]:
            out.append(ys[-1])
            continue
        while xs[j + 1] < x:
            j += 1
        t = (x - xs[j]) / h[j]
        t2, t3 = t * t, t * t * t
        out.append((2 * t3 - 3 * t2 + 1) * ys[j] + (t3 - 2 * t2 + t) * h[j] * m[j]
                   + (-2 * t3 + 3 * t2) * ys[j + 1] + (t3 - t2) * h[j] * m[j + 1])
    return out


def biquad_db(ftype: int, f0: float, q: float, gain_db: float, fs: float, freqs: list[float]) -> list[float]:
    """Magnitude response [dB] of the firmware biquad (dsp_biquad_design, RBJ cookbook).

    ftype follows the filter stage's enum: off|lowpass|highpass|bandpass|notch|peak|lowshelf|highshelf.
    """
    if ftype <= 0 or fs <= 0 or not 0 < f0 < fs / 2 or q <= 0:
        return [0.0] * len(freqs)
    A = 10 ** (gain_db / 40)
    w0 = 2 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    al = sw / (2 * q)
    s = 2 * math.sqrt(A) * al
    b, a = {
        1: (((1 - cw) / 2, 1 - cw, (1 - cw) / 2), (1 + al, -2 * cw, 1 - al)),
        2: (((1 + cw) / 2, -(1 + cw), (1 + cw) / 2), (1 + al, -2 * cw, 1 - al)),
        3: ((al, 0.0, -al), (1 + al, -2 * cw, 1 - al)),
        4: ((1.0, -2 * cw, 1.0), (1 + al, -2 * cw, 1 - al)),
        5: ((1 + al * A, -2 * cw, 1 - al * A), (1 + al / A, -2 * cw, 1 - al / A)),
        6: ((A * ((A + 1) - (A - 1) * cw + s), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - s)),
            ((A + 1) + (A - 1) * cw + s, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - s)),
        7: ((A * ((A + 1) + (A - 1) * cw + s), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - s)),
            ((A + 1) - (A - 1) * cw + s, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - s)),
    }.get(ftype, ((1.0, 0.0, 0.0), (1.0, 0.0, 0.0)))
    out = []
    for f in freqs:
        z1 = cmath.exp(-1j * 2 * math.pi * f / fs)
        z2 = z1 * z1
        hm = abs((b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2))
        out.append(20 * math.log10(max(hm, 1e-6)))
    return out


def spectrum_edges(bins: int = 128, points: int = 72) -> list[int]:
    """Point layout of the firmware's spectrum taps (spectrum_taps.c init_edges): log spaced
    over FFT bins 1..bins, at least one bin per point. Used by the simulator; the host app
    takes the layout from the device's LIVE_SUBSCRIBE response."""
    e = [1]
    for i in range(1, points + 1):
        k = max(round((bins + 1) ** (i / points)), e[-1] + 1)
        e.append(min(k, bins + 1 - (points - i)))
    return e


def reduce_bins(bin_db: list[float], edges: list[int]) -> list[float]:
    """Mean power per point (bin_db[k - 1] = bin k in dB), as the firmware does."""
    out = []
    for a, b in zip(edges, edges[1:]):
        p = sum(10 ** (bin_db[k - 1] / 10) for k in range(a, b)) / (b - a)
        out.append(10 * math.log10(max(p, 1e-30)))
    return out

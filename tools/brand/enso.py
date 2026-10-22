#!/usr/bin/env python3
"""Draws the Utsuro mark: an ensō, the one-stroke circle of emptiness (utsuro = hollow), as a filled SVG path."""
import math, sys

def enso(cx=526, cy=512, r=292, start=-52.0, sweep=318.0, wmax=150.0, wmin=10.0, n=240):
    outer, inner = [], []
    for i in range(n + 1):
        t = i / n
        a = math.radians(start - sweep * t)            # counter-clockwise on screen
        rr = r + 10 * math.sin(math.pi * 2 * t * 0.9)  # a hand-drawn wobble
        w = wmin + (wmax - wmin) * (1 - t) ** 0.85 * (0.82 + 0.18 * math.sin(math.pi * min(1.0, t * 2.5)))
        outer.append((cx + (rr + w / 2) * math.cos(a), cy + (rr + w / 2) * math.sin(a)))
        inner.append((cx + (rr - w / 2) * math.cos(a), cy + (rr - w / 2) * math.sin(a)))
    # Round head at the start of the stroke (bulging back, against the direction it travels), a point at its tail.
    a0 = math.radians(start); w0 = wmin + (wmax - wmin) * 0.82
    nx, ny = math.cos(a0), math.sin(a0)    # outward
    bx, by = -math.sin(a0), math.cos(a0)   # backwards along the stroke
    head = []
    for k in range(1, 32):
        phi = math.pi * (1 - k / 32)       # inner edge round the back to the outer edge
        head.append((cx + r * nx + w0 / 2 * (math.cos(phi) * nx + math.sin(phi) * bx),
                     cy + r * ny + w0 / 2 * (math.cos(phi) * ny + math.sin(phi) * by)))
    pts = outer + inner[::-1] + head
    return "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts) + " Z"

if __name__ == "__main__":
    print(enso())

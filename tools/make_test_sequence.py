#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a synthetic .dgs test asset: N splats oscillating sinusoidally.

Pure stdlib. Writes the exact v1 format the runtime expects (fp32 positions,
docs/dynamic-4dgs.md spec): 64-byte header, frame offset table, position blocks.
"""
import argparse
import math
import os
import struct


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='assets/demo_test.dgs')
    ap.add_argument('--frames', type=int, default=60)
    ap.add_argument('--splats', type=int, default=2000)
    ap.add_argument('--fps', type=float, default=30.0)
    ap.add_argument('--amplitude', type=float, default=0.05)
    args = ap.parse_args()

    frames, splats, fps = args.frames, args.splats, args.fps

    # deterministic base positions on a grid-ish cloud
    base = []
    for i in range(splats):
        base.append((math.sin(i * 0.017) * 2.0,
                     math.cos(i * 0.023) * 2.0,
                     -3.0 + (i % 97) / 97.0 * 3.0))

    header = struct.pack('<4sHHIIf11I', b'DGS1', 1, 0, frames, splats, fps, *([0] * 11))
    offset_table_pos = len(header)
    file_pos = offset_table_pos + 8 * frames
    offsets = []

    out = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out, exist_ok=True)

    for f in range(frames):
        offsets.append(file_pos)
        phase = f / frames * 2.0 * math.pi
        vals = [0.0] * (splats * 3)
        for i, (x, y, z) in enumerate(base):
            vals[i * 3 + 0] = x + args.amplitude * math.sin(phase + i * 0.001)
            vals[i * 3 + 1] = y + args.amplitude * math.cos(phase + i * 0.001)
            vals[i * 3 + 2] = z
        file_pos += len(struct.pack('<%df' % (splats * 3), *vals))

    # single-pass write (simpler + faster than appending per frame)
    with open(args.out, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<%dQ' % frames, *offsets))
        for f_idx in range(frames):
            phase = f_idx / frames * 2.0 * math.pi
            vals = [0.0] * (splats * 3)
            for i, (x, y, z) in enumerate(base):
                vals[i * 3 + 0] = x + args.amplitude * math.sin(phase + i * 0.001)
                vals[i * 3 + 1] = y + args.amplitude * math.cos(phase + i * 0.001)
                vals[i * 3 + 2] = z
            f.write(struct.pack('<%df' % (splats * 3), *vals))

    size = os.path.getsize(args.out)
    print(f'wrote {args.out}: {size:,} bytes, {frames} frames x {splats} splats @ {fps} fps')
    return 0


if __name__ == '__main__':
    sys_exit = main()
    raise SystemExit(sys_exit)

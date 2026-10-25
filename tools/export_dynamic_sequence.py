#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Export a directory of per-frame 3DGS .ply files into a .dgs keyframe pack.

The .dgs format (v1, see docs/dynamic-4dgs.md) stores ONLY the per-frame
positions (SH/scale/rotation stay canonical and keep loading through the
static .ply/.csgs path):

    Header   (64 B): magic 'DGS1', version, flags(bit0=fp16), frameCount,
                     splatCount, fps, reserved
    Offsets  (frameCount x u64): byte offset of each frame block
    Blocks   : frameCount x splatCount x packed xyz scalars (f32 by default;
               optional f16 is not yet supported by the runtime; no vec3 padding)

Usage:
    python tools/export_dynamic_sequence.py \
        --in  assets/sequence_frames/      # frame_0000.ply, frame_0001.ply, ...
        --out assets/bicycle.dgs --fps 30

Pure standard library; numpy is used automatically when available (much faster).
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

PLY_TYPES = {  # byte sizes for the types that appear in 3DGS PLY files
    'char': 1, 'int8': 1, 'uchar': 1, 'uint8': 1,
    'short': 2, 'int16': 2, 'ushort': 2, 'uint16': 2,
    'int': 4, 'int32': 4, 'uint': 4, 'uint32': 4,
    'float': 4, 'float32': 4, 'double': 8, 'float64': 8,
}


def parse_ply_header(path: str):
    """Return (vertex_count, stride, xyz_offsets) for a binary_little_endian PLY."""
    with open(path, 'rb') as f:
        header = bytearray()
        while True:
            line = f.readline()
            if not line:
                raise ValueError(f'{path}: header ended without end_header')
            header += line
            if line.strip() == b'end_header':
                break
        data_start = f.tell()

    text = header.decode('ascii', errors='replace')
    fmt = None
    vertex_count = 0
    properties = []  # (name, byte_size)
    in_vertex = False
    for line in text.splitlines():
        parts = line.strip().split()
        if not parts:
            continue
        if parts[0] == 'format':
            fmt = parts[1]
        elif parts[0] == 'element' and len(parts) >= 3 and parts[1] == 'vertex':
            in_vertex = True
            vertex_count = int(parts[2])
        elif parts[0] == 'element':
            in_vertex = False
        elif parts[0] == 'property' and in_vertex:
            ptype = parts[1]
            if ptype == 'list':
                raise ValueError(f'{path}: list properties are not supported')
            if ptype not in PLY_TYPES:
                raise ValueError(f'{path}: unsupported property type {ptype}')
            if len(parts) >= 3 and parts[2] in ('x', 'y', 'z') and ptype not in ('float', 'float32'):
                raise ValueError(f'{path}: position property {parts[2]} must be float32, got {ptype}')
            properties.append((parts[2], PLY_TYPES[ptype]))

    if fmt != 'binary_little_endian':
        raise ValueError(f'{path}: only binary_little_endian PLY is supported')
    stride = sum(size for _, size in properties)
    names = [name for name, _ in properties]
    missing = [c for c in ('x', 'y', 'z') if c not in names]
    if missing:
        raise ValueError(f'{path}: missing position properties {missing}')
    offsets = {}
    off = 0
    for name, size in properties:
        if name in ('x', 'y', 'z'):
            offsets[name] = off
        off += size
    return vertex_count, stride, offsets, data_start


def extract_positions(path: str, vertex_count: int, stride: int, offsets, use_numpy: bool,
                      fp16: bool) -> bytes:
    """Return packed little-endian RUB xyz triples for one frame.

    `offsets` are byte offsets of x/y/z within each vertex record.
    Positions may be non-contiguous inside the vertex record, so they are
    gathered property-by-property before packing. Standard 3DGS PLY is RDF;
    the runtime stores centers in RUB, so y and z are flipped here.
    """
    with open(path, 'rb') as f:
        header_end = 0
        while True:
            line = f.readline()
            header_end += len(line)
            if line.strip() == b'end_header':
                break
        f.seek(header_end)
        blob = f.read(stride * vertex_count)

    return pack_positions(blob, vertex_count, stride, offsets, use_numpy, fp16)


def pack_positions(blob: bytes, vertex_count: int, stride: int, offsets, use_numpy: bool, fp16: bool) -> bytes:
    """Gather packed xyz from vertex records and convert RDF coordinates to RUB."""
    n = vertex_count
    if use_numpy:
        import numpy as np
        view = np.empty((n, 3), dtype='<f4')
        for k, off in enumerate((offsets['x'], offsets['y'], offsets['z'])):
            view[:, k] = np.ndarray((n,), dtype='<f4', buffer=blob, offset=off, strides=(stride,))
        view[:, 1:] *= -1.0
        return view.astype('<f2' if fp16 else '<f4').tobytes()

    vals = [0.0] * (n * 3)
    for k, off in enumerate((offsets['x'], offsets['y'], offsets['z'])):
        vals[k::3] = [struct.unpack_from('<f', blob, i * stride + off)[0] for i in range(n)]
    vals[1::3] = [-v for v in vals[1::3]]
    vals[2::3] = [-v for v in vals[2::3]]
    tail = 'e' if fp16 else 'f'
    return struct.pack('<' + tail * (n * 3), *vals)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--in', dest='in_dir', required=True, help='directory of per-frame .ply files')
    ap.add_argument('--out', dest='out_file', required=True, help='output .dgs path')
    ap.add_argument('--fps', type=float, default=30.0)
    ap.add_argument('--fp16', action='store_true',
                    help='store positions as f16 (v1 runtime requires f32; kept for future use)')
    ap.add_argument('--start', type=int, default=0, help='first frame file index (after sorting)')
    ap.add_argument('--count', type=int, default=0, help='max frames (0 = all)')
    args = ap.parse_args()

    global _FP16
    _FP16 = bool(args.fp16)

    try:
        import numpy  # noqa: F401
        use_numpy = True
    except ImportError:
        use_numpy = False

    frames = sorted(fn for fn in os.listdir(args.in_dir) if fn.lower().endswith('.ply'))
    frames = frames[args.start:]
    if args.count > 0:
        frames = frames[:args.count]
    if len(frames) < 2:
        print('need at least 2 frames', file=sys.stderr)
        return 1
    paths = [os.path.join(args.in_dir, fn) for fn in frames]

    ref_count, ref_stride, _, _ = parse_ply_header(paths[0])
    print(f'frames={len(paths)} splats={ref_count} stride={ref_stride} '
          f'fp16={_FP16} numpy={use_numpy}')

    frame_bytes = ref_count * 3 * (2 if _FP16 else 4)
    header = struct.pack('<4sHHIIf11I', b'DGS1', 1, 0 if not _FP16 else 1,
                         len(paths), ref_count, args.fps,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    offset_table_pos = len(header)
    file_pos = offset_table_pos + 8 * len(paths)
    offsets = []
    blobs = []
    for i, p in enumerate(paths):
        count, stride, offs, start = parse_ply_header(p)
        if count != ref_count:
            raise ValueError(f'{p}: vertex count {count} != {ref_count} (all frames must match)')
        with open(p, 'rb') as f:
            f.seek(start)
            blob = f.read(stride * count)
        block = pack_positions(blob, count, stride, offs, use_numpy, _FP16)
        offsets.append(file_pos)
        blobs.append(block)
        file_pos += len(block)
        if (i + 1) % 20 == 0 or i + 1 == len(paths):
            print(f'  {i + 1}/{len(paths)} frames packed', flush=True)

    with open(args.out, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<' + 'Q' * len(offsets), *offsets))
        for block in blobs:
            f.write(block)

    print(f'wrote {args.out}: {os.path.getsize(args.out):,} bytes, '
          f'{len(paths)} frames x {ref_count} splats, frame block = {frame_bytes:,} B')
    return 0


_FP16 = True

if __name__ == '__main__':
    sys.exit(main())

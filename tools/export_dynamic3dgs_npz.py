#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Convert a Deformable-3DGS params.npz into CurvedStereoGS dynamic assets.

Outputs:
  <out>_canonical.ply   timestep-0 full Gaussian attributes (standard 3DGS PLY,
                        loaded through the existing static path)
  <out>.dgs             all-timestep position stream (v1 dynamic format,
                        fp32, docs/dynamic-4dgs.md)

The colors/rotations stay at their timestep-0 values in v1 (positions are the
only dynamic attribute); colors export as f_dc with f_rest = 0.
"""
from __future__ import annotations

import argparse
import os
import struct

import numpy as np

SH_C0 = 0.28209479177387814


def write_gaussian_ply(path: str, means: np.ndarray, rgb: np.ndarray, logit_opacities: np.ndarray,
                       log_scales: np.ndarray, quats: np.ndarray) -> None:
    n = means.shape[0]
    f_dc = (rgb - 0.5) / SH_C0
    quats = quats / (np.linalg.norm(quats, axis=1, keepdims=True) + 1e-9)

    props = [('x', means[:, 0]), ('y', means[:, 1]), ('z', means[:, 2]),
             ('nx', np.zeros(n, '<f4')), ('ny', np.zeros(n, '<f4')), ('nz', np.zeros(n, '<f4'))]
    for c in range(3):
        props.append((f'f_dc_{c}', f_dc[:, c].astype('<f4')))
    for c in range(45):
        props.append((f'f_rest_{c}', np.zeros(n, '<f4')))
    props.append(('opacity', logit_opacities[:, 0].astype('<f4')))
    for c in range(3):
        props.append((f'scale_{c}', log_scales[:, c].astype('<f4')))
    for c in range(4):
        props.append((f'rot_{c}', quats[:, c].astype('<f4')))

    names = [p[0] for p in props]
    header = ['ply', 'format binary_little_endian 1.0', f'element vertex {n}']
    header += [f'property float {name}' for name in names]
    header.append('end_header')
    dtype = np.dtype([(name, '<f4') for name in names])

    arr = np.empty(n, dtype=dtype)
    for name, values in props:
        arr[name] = values
    with open(path, 'wb') as f:
        f.write(('\n'.join(header) + '\n').encode('ascii'))
        f.write(arr.tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--npz', required=True, help='Deformable-3DGS params.npz')
    ap.add_argument('--out-prefix', required=True, help='output prefix: <prefix>_canonical.ply + <prefix>.dgs')
    ap.add_argument('--fps', type=float, default=30.0, help='playback rate for the .dgs header')
    args = ap.parse_args()

    data = np.load(args.npz, allow_pickle=True)
    means_raw = data['means3D']        # (T, N, 3) float32, RDF 训练坐标（Y 向下）
    rgb = data['rgb_colors']           # (T, N, 3)
    logit_opacities = data['logit_opacities']  # (N, 1)
    log_scales = data['log_scales']    # (N, 3)
    quats = data['unnorm_rotations']   # (T, N, 4)

    # canonical PLY 必须保持原始 RDF：静态加载路径会对它做 RDF->RUB 翻转
    # （ply_loader_async convertCoordinates，flip = (1,-1,-1)）。
    # .dgs 位置流则要与"加载后"的缓冲区同空间（RUB），所以这里单独翻转——
    # 注意只翻转 .dgs 的拷贝，canonical 千万不能翻（否则 loader 二次翻转 =
    # 镜像回到 RDF，animate 时与 .dgs 相差一个 y/z 镜像，画面全乱）。
    means_rub = means_raw.copy()
    means_rub[:, :, 1] *= -1.0
    means_rub[:, :, 2] *= -1.0
    means = means_raw

    t_count, n, _ = means.shape
    print(f'loaded: {t_count} timesteps x {n} splats')
    if t_count < 2:
        print('need at least 2 timesteps', file=sys.stderr)
        return 1

    canonical_path = args.out_prefix + '_canonical.ply'
    dgs_path = args.out_prefix + '.dgs'
    write_gaussian_ply(canonical_path, means[0], rgb[0], logit_opacities, log_scales, quats[0])
    print(f'wrote {canonical_path}: {os.path.getsize(canonical_path):,} bytes (canonical, timestep 0)')

    # .dgs: fp32 positions, all timesteps
    header = struct.pack('<4sHHIIf11I', b'DGS1', 1, 0, t_count, n, args.fps, *([0] * 11))
    offset_table_pos = len(header)
    block = n * 3 * 4
    file_pos = offset_table_pos + 8 * t_count
    offsets = [file_pos + i * block for i in range(t_count)]

    with open(dgs_path, 'wb') as f:
        f.write(header)
        f.write(struct.pack('<%dQ' % t_count, *offsets))
        for i in range(t_count):
            f.write(np.ascontiguousarray(means_rub[i], dtype='<f4').tobytes())
            if (i + 1) % 30 == 0 or i + 1 == t_count:
                print(f'  {i + 1}/{t_count} frames written', flush=True)

    print(f'wrote {dgs_path}: {os.path.getsize(dgs_path):,} bytes, '
          f'{t_count} frames x {n} splats @ {args.fps} fps '
          f'({t_count / args.fps:.2f} s loop)')
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())

# Third-party dependencies

This directory contains the vendored source dependencies needed to build CurvedStereoGS. Each library retains its own license and upstream notices; the root `LICENSE` does not replace them. See [NOTICE.md](../NOTICE.md) for the redistribution notice.

| Directory | Purpose | Recorded source revision |
| --- | --- | --- |
| `nvpro_core2/` | NVIDIA sample framework, Vulkan helpers, and its own nested dependencies | Bundled source snapshot; exact revision not recorded |
| `vrdx/` | Vulkan radix sort | `c4ce1c3688bad92d23c15afd729842054af19ad0` (local CMake/code changes) |
| `spz/` | SPZ scene loading | `b39fe5b90083d520ab77a5f9dcbf6e19c861bf05` (local CMake changes) |
| `miniply/` | PLY parsing | `1a235c70390fadf789695c9ccbf285ae712416b3` |

Do not move individual files out of these directories without updating their build paths and preserving their license files. Generated shader headers in `vrdx/src/generated/` are local build output and are not committed.

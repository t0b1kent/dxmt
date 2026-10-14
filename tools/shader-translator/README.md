<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
<!-- Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md -->
# Offline DXIL -> Metal shader translator

The translator turns Direct3D 12 shaders (DXIL, shader model 6) into Metal libraries for the opt-in shader-source seam of the
Direct3D 12 layer (`MACRUNNER_DX12_SHADER_PATH=own`, see `CHANGES-MACRUNNER.md`). The layer writes every shader it could not
serve into a request directory (`MACRUNNER_DX12_OWN_SHADER_REQUEST_DIR`: `<sha1>.<stage>.dxbc`, `<root>.rts0`, `requests.txt`);
`translate_requests.py` turns the requests into `<sha1>[.<root>].<stage>.metallib` plus `.desc` files that the layer reads from
`MACRUNNER_DX12_OWN_SHADER_DIR`. Nothing is translated inside the layer and nothing from a game is stored here.

## What is in this directory

| Path | What |
|---|---|
| `lane/` | the translator sources, as written by the project, in the directory layout the code expects (`lane/*.py` passes, `lane/takeover/admission` single stages, `lane/takeover/tess-gs` tessellation and geometry pairs). Unchanged except three files listed under `edited_files` in `inputs.lock.json` (no project build gate, no corpus path). |
| `patches/spirv-cross/` | four patches on SPIRV-Cross `4b7bcb7e`: MSL base fixes, root-signature ABI (`--msl-root-abi`), descriptor fields, tess/gs field. Variants: base = 1, admission = 1-3, tess-gs = 1-4. |
| `patches/dxil-spirv/` | two patches that turn upstream `dxil_spirv.cpp` into the command line front end `dxil-spirv-logical` (logical binding report, unbounded ranges). |
| `inputs.lock.json` | every pin: dxil-spirv `31ecc70` (v2.66.0) with its submodule revisions, SPIRV-Cross `4b7bcb7e`, the patch lists, sha256 of every source file and of the patched SPIRV-Cross files, the two open test shaders. |
| `build.py` | `fetch`, `dxil`, `frontend`, `cross`, `writer`, `all`, `verify`. Nothing is read from the network except the pinned revisions. |
| `tests/` | `test_offline.py` (no tools, no network) and `test_smoke.py` (translates two open vkd3d-proton shaders through the built tools). |

## Build and test (macOS, Apple silicon, Xcode with the Metal compiler, cmake, ninja, Homebrew `spirv-tools`)

```
python3 build.py all            # clones the pinned upstream revisions into _build/ and builds the tools into lane/
python3 -I tests/test_offline.py
python3 -I tests/test_smoke.py
```

`.github/workflows/repro110-translator-macos15.yml` runs the same steps in the cloud as separate jobs.

## Provenance, in one table

| Component | Upstream | Revision | Reproduced how |
|---|---|---|---|
| dxil-spirv | `HansKristian-Work/dxil-spirv` (MIT) | `31ecc70656cb...` (2.66.0) | the project's copy equals it file for file |
| front end CLI | same, `dxil_spirv.cpp` | + 2 patches | result equals the project's `dxil_spirv_frontend.cpp` byte for byte |
| SPIRV-Cross | `KhronosGroup/SPIRV-Cross` (Apache-2.0) | `4b7bcb7e5cf7...` + 4 patches | each variant equals the project's tree byte for byte |
| spirv-val | Homebrew `spirv-tools` | 1.4.350.0 | validation only; not part of any output |

## Limits (what is not claimed)

* The sources in `lane/` are a snapshot of work in progress (see `snapshot` in `inputs.lock.json`); the tessellation/geometry
  modules were still changing when it was taken. Layout and a few path constants still mirror the project's working tree.
* "Translated" means: the Metal compiler accepted the text and a library was built. It does not say the pixels are right.
* The tessellation/geometry line (`tess-gs`) is a sibling of the later single-stage changes in the layer series; see
  `build/repro110layer/README.md`.
* Game shaders are not part of this repository and are not needed to build or test it.

Licenses: this directory is LGPL-2.1-or-later like the rest of the fork; dxil-spirv is MIT, SPIRV-Cross is Apache-2.0
(their sources are fetched, not copied; the patches are derivative works of them and keep their licenses).

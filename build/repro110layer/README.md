<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
<!-- Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md -->
# Direct3D 12 layer: rebuilding `d3d12.dll` from this repository

This recipe builds the x86_64 PE `d3d12.dll` of the MacRunner Direct3D 12 layer with the pinned llvm-mingw
compiler and compares it with the file the project runs for Elden Ring (build v6, sha256
`0bec3f8d4fd985c06b4bc59f5cbaf6d575520a2172b04d71f956edc824f97810`).

## What is pinned

`inputs.lock.json` lists everything the build downloads: the llvm-mingw 20260505 archive (size and sha256), the
mingw-directx-headers revision (`include/native/directx` of the DXMT tree), Meson 1.11.1 and Ninja 1.13.0. The build needs
neither a built Wine nor LLVM 15: `d3d12.dll` links the import library of `winemetal.dll`, which depends only on that DLL's
exports, so the Wine libraries `winemetal.dll` itself links against are stood in by `winecrt0_stub.c` and the mingw-w64
`libntdll`/`libdbghelp`; `native_llvm_path` only has to exist. Building with the real Wine and LLVM gives the same file.

## Source state

`ER-V6-SOURCE.json` lists the sha256 of the 379 files of the tree the working `d3d12.dll` was built from. The repository
files carry authorship notice lines (`// SPDX-License-Identifier`, `// Added 2026 by the MacRunner project`, ` * Modified 2026
...`) that this tree did not have; they shift `__LINE__`, which `SetRecordingError(..., __LINE__)` bakes into seven of the
nineteen objects. Therefore two modes:

* `faithful`: a copy of the tree with exactly the listed notice lines removed, every file checked against its recorded
  sha256, `git describe` fixed to `266e503` (`DXMT_VERSION` is part of the file). On the baseline commit the 19 objects and the
  whole `d3d12.dll` equal the working file (the COFF and debug-directory timestamps are set to the working file's before
  hashing; they are the only bytes two links of the same objects differ in).
* `plain`: the tree as committed. Twelve objects are identical, seven differ by the notice lines; the DLL is reported, not
  compared.

The one thing outside the recipe's control is the Metal compiler: `d3d12.dll` embeds `dxmt_command.metallib` compiled from
`src/dxmt/*.metal` by the runner's Xcode. The report says whether the embedded library equals the working file's; if it does
not, the file differs from the working file in that region and everything after it, and the 18 objects are the evidence.

## Run

```
python3 build/repro110layer/layer_build.py all --source <checkout of the sources> --work <dir> --mode faithful
```

Steps: `fetch`, `tree`, `configure`, `units`, `build` (`--chain` runs the preceding ones). Reports are written to
`<dir>/reports/<step>-<mode>.json`. In the cloud use `.github/workflows/repro110-layer-d3d12.yml`
(`workflow_dispatch`; `phase=steps` gives one job per step and mode so that one run lists every failing step).

## The shader source seam

The commits after the baseline are the opt-in seam through which the layer can take Metal libraries produced by the offline
DXIL translator (`tools/shader-translator`): `MACRUNNER_DX12_SHADER_PATH` unset leaves the layer unchanged. `series/` holds
the patches in application order.

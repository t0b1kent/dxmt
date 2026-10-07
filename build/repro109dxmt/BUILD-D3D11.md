# Historical D3D11 source recipe

This recipe reconstructs the accepted historical source inputs. It does not claim
that a complete MacRunner bundle or the Direct3D 12 shader path has been accepted.

The immutable public source basis is `t0b1kent/dxmt` revision
`ff18f9381c411401087fb5ea474a682d72f02605`, Git tree
`530bb92064133126e968b997f425d309ccbd761c`.
The retained basis has 336 files. 236 files, including both PNG test textures,
come from unchanged Git blobs; 100 files use pinned text postimages.
All 118 additional files in the public basis are outside this historical source
selection. They are not copied into the compilation tree.
Copyright notices and license files in the selected basis are retained.

One stored header has a transport LF explicitly declared in `git-basis.lock.json`.
The loader removes that one byte and checks the original size and SHA256.
No binary assets are text encoded or downloaded from a private release.
The reconstructed historical basis, native basis and PE basis retain their
original complete inventories. The source archives are not recipe inputs.

Use a full checkout of the public DXMT repository that contains this recipe;
its history must include the pinned basis revision. Clone the public Wine
repository at the immutable delivered recipe revision:

```sh
git clone https://github.com/t0b1kent/macrunner-wine.git macrunner-wine
git -C macrunner-wine checkout --detach 00cd5eaad9a1a4b5c3679d82edf62ce3835e9f63
export MACRUNNER_WINE_RECIPE_ROOT="$PWD/macrunner-wine/build"
export MACRUNNER_DXMT_SOURCE_REPOSITORY="$PWD/dxmt"
python3 -I -B dxmt/build/repro109dxmt/verify_sources.py
python3 -I -B dxmt/build/repro109dxmt/build_dxmt.py --check-inputs
```

Python >=3.9 and Git are required for these read-only checks. No compiler or
third-party source code runs. The sibling Wine binding verifies every one of
the 91 delivered recipe files; neither a machine-local fallback nor a prebuilt
engine is admitted.

For source-built Wine27 inputs, select `--profile github-xcode27-arm64`
on an ARM64 Xcode27.0/27A266a, SDK27.0 cloud machine. This exact profile
comes from the pinned sibling Wine recipe, including deployment target14.0.
The parent build CLI and its DirectX-Headers child accept the same profile.
The default legacy profile remains explicit for earlier diagnostic inputs.
Reuse the source-built LLVM15 prefix and sealed RESULT from run37567821557;
full Wine/install, imports and composed dependencies are also mandatory.
The Wine stage matrix alone does not provide these compiler inputs.

Before a full compiler run, dispatch
`repro109-dxmt-source-matrix-macos15-arm64.yml`. Its five separate jobs are
`native-source`, `pe-source`, `directx-headers`, `llvm-source`, and `toolchain`.
The scheduler uses `fail-fast: false`, with at most four concurrent jobs.
The two source jobs reuse the preparation and collision controls. Header and
LLVM downloads run only after the existing cloud guard; URLs, revisions and
archive SHA256 pins come from the existing source producers. The SDK job checks
the selected Xcode 26.3/17C529 through the existing dependency preflight.
Driver Python is 3.13.7; all GitHub Actions are pinned by full revision.
Each job retains its own complete bounded logs and receipts, including failures.
LLVM archive bodies and unpacked source trees are excluded from uploaded reports.

Only after all five jobs pass, dispatch
`repro109-llvm15-macos15-arm64.yml` with the pinned Wine recipe revision above.
This uses the existing full LLVM15, CMake, Ninja, static zstd and libunwind producer.
The source matrix does not constitute acceptance of those compiler stages.

The final D3D11 compiler remains `build_dxmt.py --build`. Supply the source-built
Wine prefix, Wine RESULT/files inventories, composed dependency prefix/manifest,
LLVM prefix/RESULT and all four explicit receipt SHA256 values. Its `--help`
lists the exact arguments. Missing or foreign inputs fail before compilation.
The three existing scenarios remain `native-r2`, `native-pe`, and `pe`.
Wine admission checks ARM64EC metadata counts and archive architectures.
Source ownership checks cover every configured compiler source and native C input.

Acceptance remains open until the real cloud compiler outputs are compared by
functions and sections against the selected release reference and pass the
existing stands. The Direct3D 12 shader closure, Vulkan/Indiana flow, complete
application composition, signing and notarization are separate acceptance work.

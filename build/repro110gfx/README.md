# MacRunner 1.1.1 c33r graphics source inputs

The frozen candidate uses three source profiles. Their selection is part of the
build: `c17` for x86-64 frontends, `native` for ARM64EC D3D12/DXGI/winemetal and
ARM64 winemetal/AIR conversion, and `hbdx9` for the i386 frontends plus the DX9
v13j carrier, shim and ARM64 core. Substituting one profile for another changes
the candidate. The retained version strings are `2061fd1` and `65bf3a7`.

Unchanged files come from immutable public Git blobs. Changed files are sealed
text postimages. New HBDX9/DXSO files reside under the repository's `src/` and are
selected through hash-checked `public_source` records. The three modifications
to shared upstream files remain profile postimages so that the default upstream
source tree is not silently altered. LGPL and DXVK DXSO Zlib notices accompany
the corresponding sources. DirectX and zlib checkouts must match both the Git
revisions and individual file hashes in `source.lock.json`. Declared transport
LFs are removed before the exact original source bytes are verified.

`prepare_source.py --variant c17|native|hbdx9 --headers PATH --zlib PATH [--out PATH]`
verifies or reconstructs a profile without a compiler. Output paths must be new.
`build.py --variant x64|ec|native|i386 --stage source|configure|compile` builds the
selected DXMT modules. `build_hbdx9.py --kind pe|native` provides the separate
carrier/shim and native core recipes with the same three stages. Both drivers
require `--headers`, `--zlib`, and a fresh `--work`; configuration/compilation also
require the compiler, source-built Wine SDK, and appropriate build tools. Run
with low priority and at most two build workers. No driver executes Wine or a
game, installs outputs, or signs a release.

PE compilation uses llvm-mingw 20260908 / LLVM 23.1.1, with its archive pin in the
lock. LLVM **15.0.7** remains the separate native AIR conversion dependency; use
the source-built prefix from `llvm15.py`, including static zstd and unwind. A
Homebrew prefix is not a reproducible release input. Native compilation targets
macOS 26.5. The pinned `ldpfilter23.py` splits selected AArch64 paired loads and
retains the accepted compiler/startup-object handling. It wraps the EC and
native DXMT compilers, and only the six direct native HBDX9 core objects. The
HBDX9 airconv/DXBCParser static libraries use the unwrapped Apple compiler; the
x64/i386 PE compilers are unwrapped. The historical LLVM22 supplemental stack
probe is retained as historical source but is not added to LLVM23 builds.

The HBDX9 shim uses SSE2 scalar arithmetic and the pinned pre-generated stubs;
it does not regenerate stubs from an arbitrary installed SDK. The carrier is
marked with the source-built native `winebuild --builtin` tool. Carrier PE
timestamps may vary; the shim explicitly suppresses its insertion timestamp.

`candidate_reference_files` records the frozen c33r engine payload hashes.
`reference_files` is explicitly historical (1.1.0), not a c33r acceptance claim.
Source reconstruction and recipe tests do not establish native binary identity
or game correctness. Cloud rebuild/comparison, bottle/PSO migration, the rare
exit SIGKILL investigation, and visible/game/package gates remain separate.
This prepared source export is not a publication or signing authorization.

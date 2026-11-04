# MacRunner 1.1.0 graphics source inputs

This recipe retains the two source lines selected by the c32 engine. The `c17`
line builds the x86-64 D3D11 modules and ARM64EC D3D12 modules. The `native` line
builds ARM64 `winemetal.so` and the AIR conversion command. The selection is
explicit; building every component from either line would change the candidate.

Unchanged files come from immutable public Git blobs. Changed files are sealed
text postimages. The shared DirectX headers come from the public commit named in
`source.lock.json`; every selected header is checked again. A declared transport
LF restores the exact original source bytes before compilation. Tests and NVAPI
are disabled, and their binary fixtures are not source inputs.
The unchanged puff header and authorship notice come from the pinned public zlib
source checkout, preserving the complete upstream attribution and licence.

`prepare_source.py` verifies or reconstructs either line. `build.py` builds one
selected architecture with the source-built Wine SDK, LLVM15 prefix, and pinned
LLVM-MinGW tools supplied by the caller. It retains exact command logs and a
result receipt. A source build is not release acceptance: the assembled engine
still needs its CPU, graphics, game-corpus and package gates.

The ARM64EC stack-probe assembly rebuilds the 28 text bytes of the helper used by
the accepted c17 graphics build. Its compiled text is checked before every EC or
native configuration. No prebuilt helper object is published here.

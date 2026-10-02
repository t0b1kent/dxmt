# Graphics trace format and own synthetic example

The stream is a directory of sequence-numbered property-list records, command packets and explicitly referenced payload files. The wire definitions are the `wmt_trace_*` headers in this DXMT source tree.

- Event fields: `schema`, `sequence`, `event`, object identifier and a typed `fields` dictionary. Object identifiers denote trace-local generations, not host pointers.
- Command packets: encoder kind/id and typed commands. Pointer payloads and object relocations use explicit range/size contracts; unknown external pointers are rejected.
- `commit-inputs` records: readable-buffer snapshots with byte ranges and relocations. `unknown-readable-buffers` must be empty for qualified replay. Resource lifetime/synchronization is part of the stream.
- Frame records: completed command-buffer/frame relationship and the readback resource. Preserve raw sample depth and alpha. A frame without all of its producer work is not an immutable replay prefix.
- `PREFIX.json`: nonempty `files` entries identify sealed relative paths with sizes/hashes. Replay validates the immutable prefix; associated truth frames remain external inputs. Device name, unified-memory and family/capability identity constrain qualification; registryID is retained as evidence rather than a reboot-stable identity.

`examples/synthetic-events.json` is an own seven-event buffer example. Hex payloads are documentary JSON representations; `make_synthetic_stream.py` converts them to plist bytes only when explicitly given a new destination. The example has no draw or frame and cannot qualify pixels. Its `--check` mode performs an in-memory plist roundtrip without Wine, Metal or file output.

For a pixel example use the own codec and native test sources in this tree, with synthetic shader/clear data. Build/run those in your own diagnostic environment. No generated plist, image, binary or application trace is distributed here.

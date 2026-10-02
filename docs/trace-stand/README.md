# Graphics recording and replay stand

Prepared October 2, 2026. This stand records winemetal API objects, command streams, resources, ownership and completed frames. Its native player reconstructs a bounded Direct3D 11 trace without Wine and compares raw PNG8/16 RGBA samples, including alpha.

The source is included directly in the `macrunner-trace-stand` branch of the DXMT fork, on top of `macrunner-base`. See [FILES.md](FILES.md) for the 78 source targets and exclusions. This is a DXMT modification under [LGPL-2.1-or-later](LICENSE); see [COPYING.LIB](COPYING.LIB). No DXMT code is included in HyperBridge. Generated ABI headers and our native test sources are included; private coverage/evidence declarations and the generator requiring them are excluded.

## Results and limits

The October 2 check replayed 300 Hollow Knight, 191 Divinity and 248 ABZU frames, three repeats each in two full series: 4,434 exact comparisons on macOS 27.0.1 against truth recorded on 27.0. Ten identity and six negative controls behaved as expected. One earlier Divinity frame differed in four RGB samples by ±1 (0.00027%); six subsequent repeats did not reproduce it. Its cause remains open and the truth/threshold were not relaxed.

These recordings are not included. Selected-frame equality is not whole-game compatibility, gameplay FPS, Vulkan, DirectX 12 or ray-tracing validation. The A/A timing check had 6.64% spread / 2.37% CV with readback included; it is not a product speed result. Historical device capabilities were incompletely recorded. No new runtime qualification was performed while preparing this source publication.

## Dependencies and preparation

Apple Silicon macOS with the tested Metal capabilities, Xcode Command Line Tools, a compatible separately built DXMT/Meson/Ninja tree and its external dependencies are required. Python 3.10+, NumPy and Pillow are used for comparison tools; the source includes our PNG full-depth decoder. No compiled files or third-party dependency bundles are supplied.

In your local checkout of this fork:

```sh
git switch macrunner-trace-stand
python3 docs/trace-stand/examples/make_synthetic_stream.py --check
```

The example performs an in-memory format check only and needs no Wine or GPU. Prepare DXMT through its normal [build instructions](../../README.md). From the checkout root, using a separately built compatible DXMT:

```sh
WMT_TRACE_CORPUS="$OWN_CORPUS" python3 tools/wmt_trace_stand.py --quick --dxmt "$DXMT_BUILD" --prepare-only --out "$NEW_RESULT"
```

This relinks the native backend/player from the selected build; it is not a pixel qualification. `--quick`/`--full` without `--prepare-only` retain the original multi-title corpus contract. No qualification corpus is supplied, so missing traces must fail. For your own small trace, use the prepared player directly: `wmt_trace_native_player --inspect "$OWN_TRACE"`, then `--check "$OWN_TRACE" "$NEW_FRAMES"` with `MACRUNNER_WMT_NATIVE_BACKEND` pointing to the prepared backend. Native replay/GPU execution was not run during publication preparation.

## Record your own corpus

Run only your own application/content. With the diagnostic recorder enabled, set `MACRUNNER_WMT_RECORD` to a new private output directory. Stop at a bounded completed-frame prefix, freeze all referenced event/payload files and truth frames, and seal names/sizes/SHA-256 values in `PREFIX.json`. Preserve device identity/capabilities and pixel depth. Incomplete command buffers, ownership or readable-buffer snapshots remain failures. Never regenerate truth automatically when a candidate differs.

[DATA_FORMAT.md](DATA_FORMAT.md) describes the protocol. The synthetic JSON/stream generator uses no application data and contains no draw/frame. Small codec/native test sources and `tools/wmt_trace_stream_fixture.py` use our own synthetic inputs. Keep every resulting corpus, frame and payload private unless you own the content and separately choose to distribute it.

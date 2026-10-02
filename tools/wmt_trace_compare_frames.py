"""Compare decoded RGBA against producer truth and three independent replays.
PNG metadata/compression are excluded. Missing/extra frames fail explicitly.
Usage: compare_frames.py TRUTH REPLAY1 REPLAY2 REPLAY3 RESULT.json
"""
import hashlib
import json
import math
from pathlib import Path
import sys
from PIL import Image

def frames(directory):
    root = Path(directory)
    if (root / "FAILED.txt").exists():
        raise ValueError(f"FAILED marker: {root}")
    return {p.name: p for p in root.glob("frame-*.png")}

def pixels(path):
    with Image.open(path) as image:
        return image.size, image.convert("RGBA").tobytes()

def compare(truth_directory, replay_directories):
    truth = frames(truth_directory)
    replays = [frames(directory) for directory in replay_directories]
    if not truth or len(replays) != 3 or any(set(run) != set(truth) for run in replays):
        raise ValueError("EMPTY or missing/extra frames; three replays required")
    records = []
    for name in sorted(truth):
        size, expected = pixels(truth[name])
        hashes, runs = [], []
        for replay in replays:
            actual_size, actual = pixels(replay[name])
            if actual_size != size or len(actual) != len(expected):
                raise ValueError(f"dimension mismatch: {replay[name]}")
            mse = sum((a - b) ** 2 for a, b in zip(actual, expected)) / len(expected)
            psnr = None if mse == 0 else 10 * math.log10(255 ** 2 / mse)
            hashes.append(hashlib.sha256(actual).hexdigest())
            runs.append(dict(exact=mse == 0, psnr_db=psnr, pass_threshold=mse == 0 or psnr >= 60.0))
        records.append(dict(frame=name, width=size[0], height=size[1],
                            repeatable=len(set(hashes)) == 1, replay_rgba_sha256=hashes, comparisons=runs))
    passed = all(row["repeatable"] and all(run["pass_threshold"] for run in row["comparisons"])
                 for row in records)
    return dict(schema=1, threshold_db=60.0, psnr_null_means_exact=True,
                frame_count=len(records), minimum_hk_frames=300, pass_=passed,
                goal_frame_count_met=len(records) >= 300, frames=records)

if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit("usage: compare_frames.py TRUTH REPLAY1 REPLAY2 REPLAY3 NEW_RESULT.json")
    result = compare(sys.argv[1], sys.argv[2:5])
    with open(sys.argv[5], "x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(f"frames={result['frame_count']} threshold_db=60.0 repeatable_and_matching={result['pass_']}")
    raise SystemExit(0 if result["pass_"] else 1)

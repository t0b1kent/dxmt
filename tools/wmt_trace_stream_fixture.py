"""Offline deterministic stream: constructors, CPU snapshot, blit, commit, byte check.
No Metal device, Wine or subprocess. Destination must be new.
"""
import pathlib
import plistlib
import sys

destination = pathlib.Path(sys.argv[1])
destination.mkdir(exist_ok=False)
sequence = 0

def event(name, identifier, fields):
    global sequence
    record = dict(schema=1, sequence=sequence, event=name, object=identifier, fields=fields)
    (destination / f"event-{sequence:08d}.plist").write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_BINARY))
    sequence += 1

event("queue", 1, dict(maxCommandBuffers=8))
event("buffer", 2, dict(length=1024, options=0))
event("buffer", 3, dict(length=1024, options=0))
event("command-buffer", 4, dict(queue=1))
event("blit-encoder", 5, dict(commandBuffer=4))
# The codec-specific commands are taken from an existing codec fixture, not guessed.
commands = plistlib.loads(pathlib.Path(sys.argv[2]).read_bytes())
record = dict(schema=1, sequence=sequence, **{"encoder-kind": 1}, encoder=5, commands=commands)
# Fixture's object IDs 1/2 refer to src/dst, while stream IDs are 2/3.
for command in commands:
    command["objects"] = [identifier + 1 if identifier else 0 for identifier in command["objects"]]
record["commands"] = commands
(destination / f"commands-{sequence:08d}.plist").write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_BINARY))
sequence += 1
event("encoder-end", 5, {})
event("commit-inputs", 4, {"unknown-readable-buffers": [], "snapshots": [
    dict(object=identifier, offset=0, length=1024, bytes=bytes(1024), relocations=[])
    for identifier in (2, 3)]})
event("command-buffer-commit", 4, {})
event("command-buffer-wait", 4, {})
for identifier, start, end in ((2, 128, 640), (3, 256, 768)):
    event("diagnostic-buffer-check", identifier,
          dict(offset=0, bytes=bytes(0xA7 if start <= i < end else 0 for i in range(1024))))
if len(sys.argv) == 4:
    render_pass = plistlib.loads(pathlib.Path(sys.argv[3]).read_bytes())
    render_pass["colors"][0]["attachment"]["texture"] = 6
    render_pass["colors"][0]["clear"] = [0.25, 0.5, 0.75, 1.0]
    event("texture", 6, dict(textureType=2, pixelFormat=80, width=128, height=128,
                            depth=1, arrayLength=1, mipmapLevelCount=1, sampleCount=1,
                            usage=4, resourceOptions=32))
    event("command-buffer", 7, dict(queue=1))
    event("render-encoder", 8, {"commandBuffer": 7, "pass": render_pass})
    event("encoder-end", 8, {})
    event("frame-readback", 7, dict(texture=6, frame=0))
    event("command-buffer-commit", 7, {})
    event("command-buffer-wait", 7, {})
print(f"fixture records={sequence} destination={destination}")

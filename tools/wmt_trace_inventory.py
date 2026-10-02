#!/usr/bin/env python3
"""Read-only source inventory. This is not a recorder or proof of replay coverage."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def structures(text):
    return {m.group(1): m.group(2) for m in re.finditer(
        r"struct\s+(\w+)\s*\{([^{}]*)\};", text, re.S)}


def inventory(root):
    paths = ["src/winemetal/unix/winemetal_unix.c",
             "src/winemetal/winemetal_thunks.h", "src/winemetal/winemetal.h"]
    source, thunks, header = [(root / p).read_text() for p in paths]
    table_source = source
    if 'const void *__wine_unix_call_funcs[]' not in source:
        generated = root/'src/winemetal/unix/wmt_trace_api_dispatch.h'
        table_source = generated.read_text()
        paths.append(str(generated.relative_to(root)))
    start = table_source.index("const void *__wine_unix_call_funcs[]")
    end = table_source.index("};", start)
    # NULL occupies an ABI ordinal too. Filtering it shifts every later call.
    raw_entries = table_source[start:end].split("{", 1)[1].split(",")
    entries = []
    for raw in raw_entries:
        raw = raw.strip()
        if not raw:
            continue
        if raw == "NULL":
            entries.append(None)
        elif re.fullmatch(r"&[A-Za-z0-9_]+", raw):
            wrapper = raw[1:]
            if wrapper.startswith('wmt_trace_api_dispatch_'):
                match = re.search(r'static int '+re.escape(wrapper)+r'\(.*?int status = (\w+)\(params\);',table_source,re.S)
                if not match:raise ValueError('Missing generated wrapper '+wrapper)
                entries.append(match.group(1))
            else:entries.append(wrapper)
        else:
            raise ValueError(f"Unparsed dispatch initializer: {raw!r}")
    abi = structures(thunks)
    nodes = structures(header)
    calls = []
    for ordinal, name in enumerate(entries):
        if name is None:
            calls.append({"ordinal": ordinal, "entry": None, "line": None,
                          "params": None, "pointer_fields": [], "objc_messages": [],
                          "classification": "RESERVED_SLOT"})
            continue
        match = re.search(r"\b" + re.escape(name) + r"\(void \*\w+\)\s*\{", source)
        body = ""
        if match:
            pos = match.end()
            depth = 1
            limit = pos
            while depth and limit < len(source):
                depth += (source[limit] == "{") - (source[limit] == "}")
                limit += 1
            body = source[pos:limit - 1]
        param = re.search(r"struct (\w+) \*params\s*=", body)
        param_type = param.group(1) if param else None
        pointer_fields = [line.strip() for line in abi.get(param_type, "").splitlines()
                          if re.search(r"Pointer|buffer_ptr", line)]
        calls.append({"ordinal": ordinal, "entry": name,
                      "line": source.count("\n", 0, match.start()) + 1 if match else None,
                      "params": param_type, "pointer_fields": pointer_fields,
                      "objc_messages": re.findall(r"\[([^\n]+)", body),
                      "classification": "REQUIRES_SEMANTIC_REVIEW"})
    commands = [{"struct": name,
                 "pointer_fields": [line.strip() for line in body.splitlines()
                                    if "Pointer" in line],
                 "object_fields": [line.strip() for line in body.splitlines()
                                   if "obj_handle_t" in line]}
                for name, body in nodes.items() if name.startswith("wmtcmd_")]
    return {"status": "STATIC_INVENTORY_NOT_RUNTIME_COVERAGE", "root": str(root),
            "sources": {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
            "dispatch_count": len(calls),
            "occupied_dispatch_count": sum(c["entry"] is not None for c in calls),
            "reserved_ordinals": [c["ordinal"] for c in calls if c["entry"] is None],
            "command_struct_count": len(commands),
            "calls": calls, "commands": commands}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()
    result = inventory(args.root)
    if args.summary:
        print(json.dumps({k: v for k, v in result.items() if k not in ("calls", "commands")}, indent=2))
        print("unresolved dispatch bodies:", [c["entry"] for c in result["calls"]
                                             if c["entry"] is not None and c["line"] is None])
    else:
        print(json.dumps(result, indent=2))

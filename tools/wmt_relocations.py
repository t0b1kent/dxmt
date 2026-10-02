"""Typed replay relocation core; only producer-declared fields are rewritten.

This is not wired into winemetal yet. IDs must be assigned at object creation,
and declarations emitted at argument-table writes, never inferred from bytes.
"""
from dataclasses import dataclass
import struct


@dataclass(frozen=True)
class Field:
    offset: int
    kind: str  # address or resource_id
    object_id: int
    addend: int = 0


@dataclass(frozen=True)
class Resource:
    address: int
    length: int
    resource_id: int


def relocate(snapshot: bytes, fields: list[Field], objects: dict[int, Resource]) -> bytes:
    """Validate the entire ledger before returning any rewritten bytes.

    object_id=0 encodes null. Missing objects, overlapping fields, unaligned
    offsets, and out-of-range address addends invalidate the replay snapshot.
    Non-pointer siblings (length, stride, metadata, LOD bias) remain byte-exact.
    """
    writes = {}
    for field in fields:
        if field.offset < 0 or field.offset % 8 or field.offset + 8 > len(snapshot):
            raise ValueError("invalid field offset")
        if field.offset in writes:
            raise ValueError("duplicate relocation field")
        if field.kind not in ("address", "resource_id"):
            raise ValueError("unknown field kind")
        if field.object_id == 0:
            if field.addend:
                raise ValueError("null with addend")
            value = 0
        else:
            resource = objects[field.object_id]
            if field.kind == "address":
                if not 0 <= field.addend < resource.length:
                    raise ValueError("address outside resource")
                value = resource.address + field.addend
            else:
                if field.addend:
                    raise ValueError("resource ID with addend")
                value = resource.resource_id
        if not 0 <= value < 1 << 64:
            raise ValueError("value overflow")
        writes[field.offset] = value
    result = bytearray(snapshot)
    for offset, value in writes.items():
        struct.pack_into("<Q", result, offset, value)
    return bytes(result)


def selftest():
    objects = {1: Resource(0x100000, 4096, 0x777), 2: Resource(0x200000, 4, 0x888)}
    # Vertex: address, stride, length; scalar deliberately resembles an address.
    vertex = struct.pack("<QQQ", 0xdead, 0x100000, 4096)
    actual = relocate(vertex, [Field(0, "address", 1, 64)], objects)
    assert struct.unpack("<QQQ", actual) == (0x100040, 0x100000, 4096)
    # SRV texture + metadata; UAV buffer + length + counter; sampler + bias.
    cases = [
        ([Field(0, "resource_id", 1)], (0x777, 0x100000, 3)),
        ([Field(0, "address", 1), Field(16, "address", 2)], (0x100000, 0x100000, 0x200000)),
        ([Field(0, "resource_id", 1), Field(8, "resource_id", 2)], (0x777, 0x888, 3)),
        ([Field(0, "address", 0)], (0, 0x100000, 3)),
    ]
    original = struct.pack("<QQQ", 99, 0x100000, 3)
    for fields, expected in cases:
        assert struct.unpack("<QQQ", relocate(original, fields, objects)) == expected
    for fields in ([Field(0, "address", 1, 4096)], [Field(1, "address", 1)],
                   [Field(0, "address", 1), Field(0, "address", 1)],
                   [Field(0, "resource_id", 1, 1)], [Field(0, "address", 0, 1)]):
        try:
            relocate(original, fields, objects)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid ledger accepted")
    try:
        relocate(original, [Field(0, "address", 999)], objects)
    except KeyError:
        pass
    else:
        raise AssertionError("missing object accepted")
    assert original == struct.pack("<QQQ", 99, 0x100000, 3)
    print("PASS: vertex/SRV/UAV/counter/sampler/null; scalar preservation; 6 rejection cases")


if __name__ == "__main__":
    selftest()

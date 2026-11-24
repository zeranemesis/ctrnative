"""Cheats that patch the game's code (ASM cheats): the console runs a cheat's writes into the code each frame, but
here the code runs translated, so those writes are applied once to the code before it is translated, as a
code.ips would be. Only what a cheat writes into the code without a condition (writes 0-2 and E, with the offset
set by D3/DC/D2): a code reading memory or keys for its code writes cannot be known ahead, it is left out."""
import re
import struct


def parse(text):
    """{name: [(word, word)]} of a cheats file"""
    codes, current = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            codes[current] = []
        elif current is not None:
            m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})", line)
            if m:
                codes[current].append((int(m.group(1), 16), int(m.group(2), 16)))
    return codes


def code_writes(lines, start, end):
    """the (address, bytes) a code writes into [start, end) for sure, and whether it writes there at all under a
    condition or a read (those are left out)"""
    writes, uncertain, offset, depth = [], False, 0, 0
    i = 0
    while i < len(lines):
        a, b = lines[i]
        t, top = a >> 28, a >> 24
        at = (a & 0x0FFFFFFF) + (offset or 0)
        if 3 <= t <= 0xA or top == 0xDD:
            depth += 1
        elif top in (0xD0, 0xD1):
            depth = max(0, depth - 1)
        elif top == 0xD2:
            depth, offset = 0, 0
        elif top == 0xD3:
            offset = b
        elif top == 0xDC and offset is not None:
            offset += b
        elif t == 0xB:  # an offset from memory: the data the game made (the host writes those as it runs)
            offset = None
        elif t in (0, 1, 2):
            size = 4 >> t
            if offset is not None and start <= at < end:
                if depth:
                    uncertain = True
                else:
                    writes.append((at, (b & ((1 << (8 * size)) - 1)).to_bytes(size, "little")))
        elif t == 0xE:
            data = b""
            left = b
            while left > 0 and i + 1 < len(lines):
                i += 1
                data += struct.pack("<II", *lines[i])
                left -= 8
            data = data[:b]
            if offset is not None and start <= at < end:
                if depth:
                    uncertain = True
                else:
                    writes.append((at, data))
        i += 1
    return writes, uncertain


def patch(code, base, writes):
    """the code (from base) with the writes applied"""
    out = bytearray(code)
    for at, data in writes:
        o = at - base
        if 0 <= o and o + len(data) <= len(out):
            out[o:o + len(data)] = data
    return bytes(out)


if __name__ == "__main__":
    # a check: a static code write and an E code are patches; one behind a key is left out
    lines = [(0xD3000000, 0x00100000), (0x00000010, 0xE3A00001), (0xE0000020, 0x00000004), (0x11223344, 0),
             (0xD2000000, 0), (0xDD000000, 0x00000001), (0x00100030, 0xFFFFFFFF), (0xD0000000, 0)]
    w, uncertain = code_writes(lines, 0x100000, 0x200000)
    assert w == [(0x100010, bytes.fromhex("0100a0e3")), (0x100020, bytes.fromhex("44332211"))], w
    assert uncertain
    code = patch(bytes(0x100), 0x100000, w)
    assert code[0x10:0x14] == bytes.fromhex("0100a0e3") and code[0x20:0x24] == bytes.fromhex("44332211")
    print("ok")

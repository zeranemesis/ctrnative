"""Runs a 3DS game natively: python tools/ctrn.py <decrypted rom.3ds> [options]
The first run translates the game's code with 3dsrecomp and compiles it (a few minutes);
the module is kept in a cache, by title ID and code, for the next runs.
Options:
  --3dsrecomp PATH   3dsrecomp's executable (default: CTRN_3DSRECOMP, else on PATH)
  --compiler PATH    clang (default: clang on PATH)
  --cache DIR        where modules are kept (default: %LOCALAPPDATA%/ctrnative or ~/.cache/ctrnative)
  --host PATH        ctrnative's executable (default: build/ctrnative next to this repository's tools)
  --build-only       translate and compile, do not run
The game's saves go to CTRN_USER_DIR (default: <cache>/user)."""

import argparse, hashlib, os, re, shutil, struct, subprocess, sys, time
from pathlib import Path

import cheats  # cheats that patch the code (tools/cheats.py)
import romfs  # a mod's RomFS (tools/romfs.py)

HERE = Path(__file__).resolve().parent


def game_identity(rom):
    """(title ID, digest of the exheader and the ExeFS code) of a decrypted .3ds/.cci/.cxi"""
    with open(rom, "rb") as f:
        header = f.read(0x200)
        ncch = 0
        if header[0x100:0x104] == b"NCSD":
            ncch = struct.unpack_from("<I", header, 0x120)[0] * 0x200
            f.seek(ncch)
            header = f.read(0x200)
        if header[0x100:0x104] != b"NCCH":
            sys.exit(f"{rom}: not a 3DS ROM")
        if not header[0x18F] & 4:
            sys.exit(f"{rom}: the ROM is encrypted, decrypt it first")
        title_id = struct.unpack_from("<Q", header, 0x108)[0]
        f.seek(ncch + 0x200)
        digest = hashlib.sha256(f.read(0x800))
        exefs = ncch + struct.unpack_from("<I", header, 0x1A0)[0] * 0x200
        f.seek(exefs)
        table = f.read(0x200)
        for i in range(10):
            name, offset, size = table[i * 16:i * 16 + 8].rstrip(b"\0"), *struct.unpack_from("<II", table, i * 16 + 8)
            if name == b".code":
                f.seek(exefs + 0x200 + offset)
                digest.update(f.read(size))
                return title_id, digest.hexdigest()
    sys.exit(f"{rom}: no code in its ExeFS")


def read_code(rom):
    """(exheader, the ExeFS code decompressed) of a decrypted ROM"""
    with open(rom, "rb") as f:
        header, ncch = f.read(0x200), 0
        if header[0x100:0x104] == b"NCSD":
            ncch = struct.unpack_from("<I", header, 0x120)[0] * 0x200
            f.seek(ncch)
            header = f.read(0x200)
        f.seek(ncch + 0x200)
        exheader = f.read(0x800)
        exefs = ncch + struct.unpack_from("<I", header, 0x1A0)[0] * 0x200
        f.seek(exefs)
        table = f.read(0x200)
        for i in range(10):
            offset, size = struct.unpack_from("<II", table, i * 16 + 8)
            if table[i * 16:i * 16 + 8].rstrip(b"\0") == b".code":
                f.seek(exefs + 0x200 + offset)
                code = f.read(size)
    return exheader, (decompress(code) if exheader[0x0D] & 1 else code)


def decompress(data):
    """the ExeFS code's backwards LZ (as the host's rom.cpp): the footer gives where it starts and ends"""
    bounds, extra = struct.unpack_from("<II", data, len(data) - 8)
    out = bytearray(data) + bytearray(extra)
    source, stop, target = len(data) - (bounds >> 24), len(data) - (bounds & 0xFFFFFF), len(out)
    while source > stop:
        source -= 1
        control = out[source]
        for _ in range(8):
            if source <= stop:
                break
            if control & 0x80:
                source -= 2
                pair = out[source] | out[source + 1] << 8
                length, distance = (pair >> 12) + 3, (pair & 0xFFF) + 2
                for _ in range(length):
                    out[target - 1] = out[target + distance]
                    target -= 1
            else:
                source -= 1
                target -= 1
                out[target] = out[source]
            control = control << 1 & 0xFF
    return bytes(out)


# the SDK's __cpp_initialize__aeabi_ calls each entry of a table of relative words (the word plus
# its own address): ldr r0, [r4] / add lr, pc, #4 / add r0, r0, r4 / mov pc, r0; the table's
# bounds come before, each an "ldr rN, [pc, #i]" then "add rN, pc, rN"
CALL_LOOP = struct.pack("<4I", 0xE5940000, 0xE28FE004, 0xE0800004, 0xE1A0F000)


# the same loop in Thumb (older SDKs, NSMB2): ldr r0, [r4] / adds r0, r0, r4 / blx r0 / adds r4, #4 / cmp r4, r5,
# the bounds each a Thumb "ldr rN, [pc, #i]" then "add rN, pc"
THUMB_CALL_LOOP = struct.pack("<5H", 0x6820, 0x1900, 0x4780, 0x1D24, 0x42AC)


def constructor_hints(code, base):
    """the C++ constructors the game runs at start: 3dsrecomp finds no call or pointer to them"""
    bounds = {}
    at = code.find(CALL_LOOP)
    if at >= 0 and not at % 4:
        for p in range(max(0, at - 0x40), at, 4):
            ldr, add = struct.unpack_from("<II", code, p) if p + 8 <= len(code) else (0, 0)
            reg = (ldr >> 12) & 0xF
            if ldr & 0xFFFF0000 == 0xE59F0000 and add == 0xE08F0000 | reg << 16 | reg << 12 | reg:
                literal = struct.unpack_from("<i", code, p + 8 + (ldr & 0xFFF))[0]
                bounds[reg] = base + p + 4 + 8 + literal  # the add's pc reads 8 ahead
    at = code.find(THUMB_CALL_LOOP)
    if not bounds and at >= 0 and not at % 2:
        for p in range(max(0, at - 0x20), at, 2):
            ldr, add = struct.unpack_from("<HH", code, p)
            reg = ldr >> 8 & 7
            if ldr & 0xF800 == 0x4800 and add == 0x4478 | reg:  # Thumb pc: 4 ahead, the ldr's aligned down
                literal = struct.unpack_from("<i", code, (p + 4 & ~3) + (ldr & 0xFF) * 4)[0]
                bounds[reg] = base + p + 2 + 4 + literal
    if 4 not in bounds or 5 not in bounds or not 0 < bounds[5] - bounds[4] < len(code):
        return []
    start, end = bounds[4], bounds[5]
    return [start + i * 4 + struct.unpack_from("<i", code, start - base + i * 4)[0] for i in range((end - start) // 4)]


def rotated_immediate(word):
    value, rotate = word & 0xFF, (word >> 8 & 0xF) * 2
    return (value >> rotate | value << (32 - rotate)) & 0xFFFFFFFF


def jump_table_hints(code, base):
    """hand-written assembly's jump tables (video decoders): "add pc, rA, rB" where one of the two was set a few
    instructions before by "sub/add rX, pc, #i", the table, and the other loaded from it: each word of the table
    is a target relative to it, which 3dsrecomp does not follow"""
    words = struct.unpack_from(f"<{len(code) // 4}I", code)
    end = base + len(words) * 4
    found = set()
    for i, w in enumerate(words):
        if w & 0xFFFFF000 == 0xE28FE000:  # add lr, pc, #i: a call made by hand, which returns there
            target = base + i * 4 + 8 + rotated_immediate(w)
            if target < end and not target % 4:
                found.add(target)
            continue
        if w & 0xFFF0F000 != 0xE080F000 or w & 0xFF0:  # add pc, rA, rB, unshifted
            continue
        regs = {w >> 16 & 0xF, w & 0xF}
        for j in range(i - 1, max(-1, i - 13), -1):
            p = words[j]
            if p & 0xFFFF0000 not in (0xE24F0000, 0xE28F0000) or p >> 12 & 0xF not in regs:
                continue
            pc = base + j * 4 + 8
            table = pc - rotated_immediate(p) if p & 0x00F00000 == 0x00400000 else pc + rotated_immediate(p)
            k = table
            while base <= k < end:  # the table ends at its first word that is not a target in the code
                target = table + struct.unpack_from("<i", code, k - base)[0]
                if target % 4 or not base <= target < end:
                    break
                found.add(target)
                k += 4
            break
    return sorted(found)


def romfs_files(f, rom_header, ncch):
    """(name, offset in the ROM, size) of each file of a decrypted ROM's RomFS (its IVFC level 3)"""
    ivfc = ncch + struct.unpack_from("<I", rom_header, 0x1B0)[0] * 0x200
    f.seek(ivfc)
    header = f.read(0x60)
    if header[:4] != b"IVFC":
        return
    master, log2 = struct.unpack_from("<I", header, 0x08)[0], struct.unpack_from("<I", header, 0x4C)[0]
    level3 = ivfc + ((0x60 + master + (1 << log2) - 1) & ~((1 << log2) - 1))
    f.seek(level3)
    table = struct.unpack("<10I", f.read(0x28))
    f.seek(level3 + table[7])
    meta, data, at = f.read(table[8]), level3 + table[9], 0
    while at + 0x20 <= len(meta):
        offset, size, length = *struct.unpack_from("<QQ", meta, at + 8), struct.unpack_from("<I", meta, at + 0x1C)[0]
        yield meta[at + 0x20:at + 0x20 + length].decode("utf-16-le", "replace"), data + offset, size
        at += 0x20 + ((length + 3) & ~3)


def unpacked(data):
    """a whole file compressed as 3DS games do (Nintendo's LZ10/LZ11, Yaz0, Grezzo's LzS) unpacked, else None"""
    out = bytearray()
    if data[:4] == b"Yaz0":  # flags most significant bit first; a copy is 2 or 3 bytes
        size, i = struct.unpack_from(">I", data, 4)[0], 16
        while len(out) < size and i < len(data):
            flags = data[i]
            i += 1
            for b in range(7, -1, -1):
                if len(out) >= size or i >= len(data):
                    break
                if flags >> b & 1:
                    out.append(data[i])
                    i += 1
                    continue
                b0, b1 = data[i], data[i + 1]
                i += 2
                n = b0 >> 4
                if n == 0:
                    n, i = data[i] + 0x12, i + 1
                else:
                    n += 2
                start = len(out) - (((b0 & 0xF) << 8 | b1) + 1)
                for k in range(n):
                    out.append(out[start + k])
        return bytes(out)
    if data[:4] == b"LzS\x01":  # LZSS: flags least significant bit first, a 4 KiB window starting at 0xFEE
        size, i, ring, r = struct.unpack_from("<I", data, 8)[0], 16, bytearray(4096), 0xFEE
        while len(out) < size and i < len(data):
            flags = data[i]
            i += 1
            for b in range(8):
                if len(out) >= size or i >= len(data):
                    break
                if flags >> b & 1:
                    out.append(data[i])
                    ring[r], r, i = data[i], (r + 1) & 0xFFF, i + 1
                    continue
                lo, hi = data[i], data[i + 1]
                i += 2
                for k in range((hi & 0xF) + 3):
                    c = ring[((hi & 0xF0) << 4 | lo) + k & 0xFFF]
                    out.append(c)
                    ring[r], r = c, (r + 1) & 0xFFF
        return bytes(out)
    if len(data) > 8 and data[0] in (0x10, 0x11):  # Nintendo's LZ: flags most significant bit first
        size, i = struct.unpack_from("<I", data, 0)[0] >> 8, 4
        if size == 0:
            size, i = struct.unpack_from("<I", data, 4)[0], 8
        if not len(data) < size <= 16 * len(data):  # what a compressed file would be
            return None
        try:
            while len(out) < size:
                flags = data[i]
                i += 1
                for b in range(7, -1, -1):
                    if len(out) >= size:
                        break
                    if not flags >> b & 1:
                        out.append(data[i])
                        i += 1
                        continue
                    b0, b1 = data[i], data[i + 1]
                    if data[0] == 0x10 or b0 >> 4 > 1:
                        n, distance, i = (b0 >> 4) + (3 if data[0] == 0x10 else 1), ((b0 & 0xF) << 8 | b1) + 1, i + 2
                    elif b0 >> 4 == 0:
                        n, distance, i = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11, ((b1 & 0xF) << 8 | data[i + 2]) + 1, i + 3
                    else:
                        b2, b3 = data[i + 2], data[i + 3]
                        n, distance, i = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111, ((b2 & 0xF) << 8 | b3) + 1, i + 4
                    start = len(out) - distance
                    if start < 0:
                        return None
                    for k in range(n):
                        out.append(out[start + k])
        except IndexError:
            return None
        return bytes(out)
    return None


def shader_binaries(data):
    """the shader binaries (DVLB: a header, the program DVLP, then each shader's DVLE) in a block of bytes"""
    at = data.find(b"DVLB")
    while at >= 0:
        try:
            count = struct.unpack_from("<I", data, at + 4)[0]
            dvlp = at + 8 + 4 * count
            if 0 < count <= 64 and data[dvlp:dvlp + 4] == b"DVLP":
                code, words, descriptors, n = struct.unpack_from("<4I", data, dvlp + 8)
                end = max(dvlp + code + 4 * words, dvlp + descriptors + 8 * n)
                for i in range(count):
                    dvle = at + struct.unpack_from("<I", data, at + 8 + 4 * i)[0]
                    if data[dvle:dvle + 4] != b"DVLE":
                        raise struct.error
                    # constants, labels, outputs, uniforms (offset, count), then the symbols (offset, bytes)
                    tables = struct.unpack_from("<10I", data, dvle + 0x18)
                    end = max(end, dvle + 0x40, *(dvle + tables[k] + tables[k + 1] * size
                                                  for k, size in ((0, 20), (2, 16), (4, 8), (6, 8), (8, 1))))
                if end <= len(data):
                    yield data[at:end]
        except struct.error:
            pass
        at = data.find(b"DVLB", at + 4)


def extract_shaders(rom, code, folder):
    """every shader binary of a game, from its code and RomFS files (whole, or unpacked), into folder (by content)"""
    folder.mkdir(parents=True, exist_ok=True)
    found = {}

    def keep(data, where):
        for binary in shader_binaries(data):
            found.setdefault(hashlib.sha256(binary).hexdigest()[:16], (binary, where))

    keep(code, "code")
    with open(rom, "rb") as f:
        header, ncch = f.read(0x200), 0
        if header[0x100:0x104] == b"NCSD":
            ncch = struct.unpack_from("<I", header, 0x120)[0] * 0x200
            f.seek(ncch)
            header = f.read(0x200)
        for name, offset, size in list(romfs_files(f, header, ncch)):
            f.seek(offset)
            data = f.read(size)
            keep(data, name)
            if inner := unpacked(data):
                keep(inner, name + " (unpacked)")
    for digest, (binary, where) in found.items():
        (folder / f"{digest}.shbin").write_bytes(binary)
    places = sorted({where for _, where in found.values()})
    print(f"[ctrn] {len(found)} shader binaries in {len(places)} places: {', '.join(places[:8])}"
          f"{'...' if len(places) > 8 else ''}", flush=True)


# the graphics libraries' functions done natively instead of making PICA200 commands (docs/RENDU-NATIF.md): the
# number the host knows each by (src/host/gfx_native.hpp), what it is, and its first instructions; a pc-relative load's
# offset and a branch's target are left out, as they move from one build to the next
GFX_NATIVES = [
    (1, "nngx: a prepared list copied into the current one (SDK 2012)",
     [0xE92D4FFF, 0xE200201F, 0xE24DD004, 0xE3A04000, 0xE3A09001, 0xE59F1000, 0xE59D8038, 0xE59DB040,
      0xE0812102, 0xE591609C, 0xE59F1000, 0xE592701C, 0xE5915000, 0xE3570000, 0x0A000000, 0xE5971000]),
    (2, "nw::lyt (NSMB2's drawer): a batch of rectangles drawn",
     [0xE92D41F0, 0xE1A04000, 0xE2800088, 0xE5D41024, 0xE590300C, 0xE59F8180, 0xE59F6180, 0xE1A02101,
      0xE5803008, 0xE1A0C102, 0xE1A03A02, 0xE2432601, 0xE1822008, 0xE0801201, 0xE580200C, 0xE3A07000,
      0xE581700C, 0xE28C5010, 0xE1A01000, 0xE5960000, 0xE1A02005, 0xEBFE0982, 0xE5960000, 0xE3C51003,
      0xE0800001, 0xE5860000, 0xE5D40025, 0xE3500000, 0x11A00004, 0x1B005C36, 0xE5D40026, 0xE3500000,
      0x0A000014, 0xE1A02100, 0xE594C454, 0xE1A05A02, 0xE1A03102, 0xE2452601, 0xE2841B01, 0xE1822008,
      0xE2811048, 0xE5842454, 0xE0810200, 0xE584C450, 0xE2835010, 0xE580700C, 0xE5960000, 0xE1A02005,
      0xEBFE0967, 0xE5960000, 0xE3C51003, 0xE0800001, 0xE5860000, 0xE5C47026, 0xE5D40024, 0xE0800080,
      0xE1A05080, 0xE1A01005, 0xE1A00004, 0xEB0C070B, 0xE5962000, 0xE59F30A8, 0xE594C69C, 0xE1A01000,
      0xE5823004, 0xE2820008, 0xE282300C, 0xE582C000, 0xE3811102, 0xE5863000, 0xE5801000, 0xE5960000,
      0xE59F2080, 0xE2803004, 0xE5863000, 0xE5802000, 0xE5960000, 0xE2821001, 0xE2802004, 0xE5862000,
      0xE5805000, 0xE5960000, 0xE2802004, 0xE5862000, 0xE5801000, 0xE1A00004, 0xE3A00040, 0xE1A05000,
      0xE1A00004, 0xEB0C06EA, 0xE1A01000, 0xE5960000, 0xE1A02005, 0xEBFE093A, 0xE1A00004, 0xE320F000,
      0xE3A00040, 0xE5961000, 0xE3C00003, 0xE0800001, 0xE5860000, 0xE5C47024, 0xE8BD81F0, 0x000F02C1,
      0x005F1DC0, 0x000F02B1, 0x000F0227]),
    (3, "nw::lyt (NSMB2's drawer): a material set up",
     [0xE92D4FF0, 0xE1A04000, 0xE24DD024, 0xE1A05001, 0xE3A09001, 0xE5D1004D, 0xE3100002, 0x0A00000A,
      0xE5D406A2, 0xE3500000, 0x13500001, 0x0A00007A, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x0A000076,
      0xE28DD024, 0xE8BD4FF0, 0xEA0002BB, 0xE595002C, 0xE59FB348, 0xE59FA348, 0xE59F8348, 0xE3100C02,
      0xE3A06000, 0xE2847C06, 0x0A000021, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B0002AF, 0xE3A00000,
      0xE5C406A1, 0xE5981000, 0xE3A06001, 0xE2810004, 0xE4819008, 0xE580A000, 0xE1A00005, 0xE5881000,
      0xEB0BF6E8, 0xE59F1308, 0xEDD04A01, 0xED9F5ABE, 0xED9F0ABE, 0xECD10A08, 0xEE040A85, 0xECCD0A08,
      0xE5D02000, 0xEEBC0AC0, 0xE5981000, 0xE79D2102, 0xE281C008, 0xE1A03202, 0xEE102A10, 0xE20220FF,
      0xE1832402, 0xE3822001, 0xE8810804, 0xE588C000, 0xEA000011, 0xE5D700A1, 0xE3500000, 0x1A00000E,
      0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B00028A, 0xE5C496A1, 0xE5981000, 0xE3A02061, 0xE3A06001,
      0xE2810004, 0xE4819008, 0xE580A000, 0xE2810004, 0xE4812008, 0xE580B000, 0xE5881000, 0xE595002C,
      0xE3100B01, 0x0A000012, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B000278, 0xE3A00000, 0xE3560000,
      0xE5C406A0, 0x1A000004, 0xE5981000, 0xE2810004, 0xE4819008, 0xE580A000, 0xE5881000, 0xE1A01005,
      0xE1A00004, 0xEB00016F, 0xE320F000, 0xE320F000, 0xEA000015, 0xE5D700A0, 0xE3500000, 0x1A000012,
      0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B000262, 0xE3560000, 0xE5C496A0, 0x1A000004, 0xE5981000,
      0xE2810004, 0xE4819008, 0xE580A000, 0xE5881000, 0xE59FC1E0, 0xE5981000, 0xE59C0000, 0xE99C100C,
      0xE2816010, 0xE881100D, 0xE5886000, 0xE5950030, 0xE3100D07, 0x0A00000A, 0xE5D41024, 0xE1A00004,
      0xE3510000, 0x1B00024C, 0xE1A01005, 0xE1A00004, 0xEB00037B, 0xE3A00005, 0xE5C406A2, 0xE28DD024,
      0xE8BD8FF0, 0xE59F6190, 0xE2100003, 0x0A000010, 0xE3500001, 0x0A000040, 0xE3500002, 0x0A000088,
      0xE3500003, 0x1AFFFFF4, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B000238, 0xE1A01005, 0xE1A00004,
      0xEB0002EE, 0xE3A00003, 0xE5C406A2, 0xE28DD024, 0xE8BD8FF0, 0xE5950014, 0xE58D0000, 0xE5D406A2,
      0xE3500000, 0x0A000016, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B000228, 0xE59D1000, 0xE2865018,
      0xE3A02000, 0xE5861020, 0xE5980000, 0xE5963010, 0xE596C014, 0xE5966024, 0xE280A010, 0xE89500A0,
      0xE2809018, 0xE88A0042, 0xE8801008, 0xE2800008, 0xE88000A0, 0xE5889000, 0xE5C426A2, 0xE28DD024,
      0xE8BD8FF0, 0xE59F00D4, 0xEB0BE031, 0xE1A05000, 0xE1A0000D, 0xEB0BE02E, 0xE1550000, 0xE320F000,
      0x0AFFFFC5, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B000209, 0xE59D1000, 0xE59F30A4, 0xE5861020,
      0xE5982000, 0xE5821000, 0xE2821008, 0xE5823004, 0xE5881000, 0xE28DD024, 0xE8BD8FF0, 0xE5950014,
      0xE58D0000, 0xE5950010, 0xE58D0004, 0xE5D406A2, 0xE3500001, 0x0A00001C, 0xE5D41024, 0xE1A00004,
      0xE3510000, 0x1B0001F4, 0xE59D0004, 0xE59F1054, 0xE3A02048, 0xE5860038, 0xE59D0000, 0xE5860050,
      0xE5980000, 0xEBFE0B85, 0xE5980000, 0xE2800048, 0xE5880000, 0xE5C496A2, 0xE28DD024, 0xE8BD8FF0,
      0x000F0104, 0x00000111, 0x005F1DC0, 0x437F0000, 0x3F000000, 0x0058E66C, 0x0058E4CC, 0x005F4920,
      0x005F4940, 0x000F00FB, 0x005F4948, 0xE51F500C, 0xE2850010, 0xEB0BDFF6, 0xE1A07000, 0xE28D0004,
      0xEB0BDFF3, 0xE1570000, 0xE320F000, 0x1A000007, 0xE2850028, 0xEB0BDFEE, 0xE1A07000, 0xE1A0000D,
      0xEB0BDFEB, 0xE1570000, 0xE320F000, 0x0AFFFF82, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B0001C6,
      0xE59D1004, 0xE59FC060, 0xE5861038, 0xE59D0000, 0xE28C3018, 0xE5860050, 0xE5982000, 0xE8821002,
      0xE2821008, 0xE595C028, 0xE282200C, 0xE2820004, 0xE581C000, 0xE5823000, 0xE5880000, 0xE28DD024,
      0xE8BD8FF0, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x1B0001B1, 0xE1A01005, 0xE1A00004, 0xEB000219,
      0xE3A00002, 0xE320F000, 0xEAFFFF76, 0x000F00DB]),
    (4, "nw::lyt (NSMB2's drawer): a material's textures set",
     [0xE92D4FF7, 0xE1B07001, 0xE59FA194, 0x0A00005A, 0xE59D0000, 0xE3A09001, 0xE3A08000, 0xE1A05008,
      0xE5D00024, 0xE1A04008, 0xE3500000, 0xE5970030, 0x13A09000, 0xE2006003, 0xE3560003, 0xE3A00003,
      0x81A06000, 0xE3560000, 0x83A0B001, 0x9A000018, 0xE185541B, 0xE1A00007, 0xE5900034, 0xE0800284,
      0xE08A1284, 0xE5903004, 0xE5912008, 0xE15201A3, 0x1A000003, 0xE5912018, 0xE5903014, 0xE1520003,
      0x0A000008, 0xE5B02004, 0xE2800010, 0xE3A08001, 0xEC900A03, 0xE1A001A2, 0xEDC10A00, 0xE5810008,
      0xED811A04, 0xED810A06, 0xE2844001, 0xE1540006, 0x3AFFFFE6, 0xE59F70EC, 0xE1994008, 0x0A00000D,
      0xE59D0000, 0xE5D01024, 0xE3510000, 0x1B000137, 0xE59F20D4, 0xE5970000, 0xE2823014, 0xE5921010,
      0xE592201C, 0xE8931008, 0xE280A010, 0xE580200C, 0xE880100A, 0xE587A000, 0xE3590000, 0xE3852A11,
      0x0A00000A, 0xE5970000, 0xE59FC0A0, 0xE1A05405, 0xE2801004, 0xE4805008, 0xE59F3094, 0xE581C000,
      0xE2801004, 0xE4802008, 0xE5813000, 0xE5870000, 0xE3540000, 0x0A000005, 0xE5971000, 0xE59F3074,
      0xE2810004, 0xE4812008, 0xE5803000, 0xE5871000, 0xE3580000, 0x0A000006, 0xE59F1044, 0xE5970000,
      0xE1A02286, 0xEBFE0AAA, 0xE5970000, 0xE0800286, 0xE5870000, 0xE28DD00C, 0xE8BD8FF0, 0xE3A00000,
      0xE58A0008, 0xE58A0028, 0xE3520000, 0xE58A0048, 0x0AFFFFF7, 0xE59D0000, 0xE28DD00C, 0xE8BD4FF0,
      0xEA005F74, 0x005F4990, 0x005F1DC0, 0x0058E4CC, 0x0002006F, 0x000B0080, 0x00040080]),
    (5, "nw::font (NSMB2's drawer): pending matrices and colors sent",
     [0xE92D41F0, 0xE1A04000, 0xE2800F8E, 0xE5D41025, 0xE59F3058, 0xE590C00C, 0xE3A07000, 0xE1A02101,
      0xE0801201, 0xE1A05A02, 0xE1A06102, 0xE2452601, 0xE1822003, 0xE2865010, 0xE580200C, 0xE59F6030,
      0xE580C008, 0xE581700C, 0xE1A01000, 0xE5960000, 0xE1A02005, 0xEBFDAD2D, 0xE5960000, 0xE3C51003,
      0xE0800001, 0xE5860000, 0xE5C47025, 0xE8BD81F0, 0x000F02C1, 0x005F1DC0]),
    (6, "nw::font (NSMB2's drawer): a text's matrix sent",
     [0xE59F3058, 0xE92D0030, 0xE59F4054, 0xE3A0518E, 0xE5932000, 0xE59FC04C, 0xE2821004, 0xE4825008,
      0xE5814000, 0xE5832000, 0xE5904000, 0xE2821008, 0xE8821010, 0xE2802004, 0xE2800024, 0xEC920A08,
      0xE2812040, 0xEC810A08, 0xE2811020, 0xEC900A08, 0xEC810A08, 0xE5832000, 0xE8BD0030, 0xE12FFF1E,
      0x005F1DC0, 0x000F02C0, 0x00FF02C1]),
    (7, "nw::font (NSMB2's drawer): a text's colors sent",
     [0xE59FC054, 0xE92D03F0, 0xE3520000, 0xE59C6000, 0xE59F5048, 0xE2862004, 0x0A000007, 0xE5861000,
      0xE9820021, 0xE285800A, 0xE282900C, 0xE5828000, 0xE58C9000, 0xE8BD03F0, 0xE12FFF1E, 0xE59F8020,
      0xE5860000, 0xE9820022, 0xE282900C, 0xE5828000, 0xE58C9000, 0xE8BD03F0, 0xE12FFF1E, 0x005F1DC0,
      0x000F00F3, 0x000F00DB]),
    (8, "nw::font (NSMB2's drawer): a text's list built (then translated once)",
     [0xE92D4FF7, 0xE24DD058, 0xE1A05000, 0xE3A00000, 0xE3A04001, 0xE59D105C, 0xE5918044, 0xE1D8B0B4,
      0xE1D810B6, 0xE5880018, 0xE5880020, 0xE15B0001, 0x81A0B001, 0xE35B0000, 0xE5C84008, 0x0A0001C8,
      0xE2850B01, 0xED9F0ABA, 0xE58D0030, 0xE3A01000, 0xED800A96, 0xE5C51690, 0xE59F12D8, 0xE2852F9E,
      0xE3A0018E, 0xE8820003, 0xE3A02060, 0xE5981018, 0xE5980010, 0xE0800101, 0xE59F12BC, 0xEBFDAAF1,
      0xE5980018, 0xE28D1028, 0xE2800018, 0xE5880018, 0xE59D005C, 0xE5900018, 0xE58D0028, 0xE59D005C,
      0xE590001C, 0xE58D002C, 0xE59D005C, 0xE5D5C027, 0xE5953674, 0xE5D02020, 0xE083020C, 0xEB00061B,
      0xE5D50027, 0xE2800003, 0xEE000A10, 0xE59D0030, 0xEEB80A40, 0xED800A99, 0xE5D50027, 0xE2801004,
      0xE3A00001, 0xE5C51027, 0xE5C50690, 0xE28B0007, 0xE3C00801, 0xE598600C, 0xE1A011A0, 0xE3510000,
      0xDA00000B, 0xE3110001, 0xE0860001, 0x0A000001, 0xE3A02000, 0xE5602001, 0xE1B010C1, 0x13A02000,
      0x0A000003, 0xE5402001, 0xE2511001, 0xE5602002, 0x1AFFFFFB, 0xE3A00000, 0xE58D0004, 0xE58D0034,
      0xE3E00000, 0xE58D0044, 0xE58D0020, 0xE3A00000, 0xE58D0038, 0xE2880024, 0xE58D003C, 0xE3A00000,
      0xE35B0000, 0xE58D001C, 0x82850E67, 0x858D0040, 0x9A000151, 0xE59D4038, 0xE1A00FC4, 0xE0840EA0,
      0xE08611C0, 0xE3C00007, 0xE5D11000, 0xE0440000, 0xE1A00031, 0xE3100001, 0x12844001, 0x1AFFFFF5,
      0xE2841001, 0xE58D1038, 0xE59D103C, 0xE0840084, 0xE0800184, 0xE081A100, 0xE59D1004, 0xE59A0028,
      0xE5900008, 0xE1510000, 0x0A0000AA, 0xE58D0004, 0xE59D001C, 0xE3500000, 0x15D50024, 0x13500000,
      0x0A000002, 0xE1A01008, 0xE1A00005, 0xEB000185, 0xE59A0028, 0xE5D07010, 0xE59D0044, 0xE1570000,
      0x0A000058, 0xE357000B, 0xE59D1044, 0x13570008, 0x03A00001, 0x13A00000, 0xE3710001, 0x0A00000D,
      0xE351000B, 0x13510008, 0x03A01001, 0x13A01000, 0xE1510000, 0x0A000028, 0xE3500000, 0x0A000017,
      0xE59F00F8, 0xE59D1060, 0xE7903101, 0xE59F00F0, 0xE7909101, 0xEA000017, 0xE3500000, 0x159F00E4,
      0x059F00E4, 0xE59D1060, 0xE7903101, 0x159F00DC, 0x059F00DC, 0xE7909101, 0xE5981018, 0xE5980010,
      0xE1A02009, 0xE0800101, 0xE1A01003, 0xEBFDAA6D, 0xE5980018, 0xE0800129, 0xE5880018, 0xEA000016,
      0xE59F00B0, 0xE59D1060, 0xE7903101, 0xE59F10A8, 0xE59D0060, 0xE7919100, 0xE5981018, 0xE5980010,
      0xE1A02009, 0xE0800101, 0xE1A01003, 0xEBFDAA5D, 0xE5980018, 0xE0800129, 0xE5880018, 0xE59F007C,
      0xE5982018, 0xE5981010, 0xEC900A04, 0xE0810102, 0xE2821004, 0xEC800A04, 0xE5881018, 0xE5983010,
      0xE598C018, 0xE58D7044, 0xE59F0054, 0xE3A02004, 0xE783710C, 0xE5983018, 0xE5981010, 0xE0822103,
      0xE7810002, 0xE5980018, 0xE2800002, 0xE5880018, 0xEA000014, 0x00000000, 0x000F02C0, 0x00588610,
      0x005F2160, 0x0058867C, 0x005F2178, 0x005F216C, 0x00588694, 0x00588688, 0x005F2154, 0x00588670,
      0x00588600, 0x000F008E, 0xE51F0010, 0xE5982018, 0xE5981010, 0xEC900A04, 0xE0810102, 0xE2821004,
      0xEC800A04, 0xE5881018, 0xE5981010, 0xE5982018, 0xE59F03D4, 0xE3A07004, 0xE7810102, 0xE5982018,
      0xE5981010, 0xE59F03C4, 0xE0872102, 0xE7810002, 0xE5980018, 0xE5981010, 0xE59D2004, 0xE2800002,
      0xE5880018, 0xE7812100, 0xE5982018, 0xE5981010, 0xE59F039C, 0xE0872102, 0xE7810002, 0xE5980018,
      0xE2802002, 0xE5882018, 0xE59A0028, 0xE59D1034, 0xE590000C, 0xE1500001, 0x0A00000A, 0xE58D0034,
      0xE598C010, 0xE59F336C, 0xE78C0102, 0xE5981018, 0xE5980010, 0xE0871101, 0xE7803001, 0xE5980018,
      0xE2800002, 0xE5880018, 0xE59A0028, 0xE5900004, 0xE5901000, 0xE5911064, 0xE12FFF31, 0xE59D1020,
      0xE1510000, 0x0A00000B, 0xE5982010, 0xE598C018, 0xE58D0020, 0xE59F3320, 0xE782010C, 0xE5981018,
      0xE5980010, 0xE0871101, 0xE7803001, 0xE5980018, 0xE2800002, 0xE5880018, 0xE154000B, 0x2A00008B,
      0xE1A00FC4, 0xE0840EA0, 0xE3C01007, 0xE08691C0, 0xE044A001, 0xE5D90000, 0xE1A00A30, 0xE3100001,
      0x1A00007F, 0xE0840084, 0xE0801184, 0xE59D003C, 0xE59D2004, 0xE0800101, 0xE5900028, 0xE5900008,
      0xE1500002, 0x1A000076, 0xE0880101, 0xE2807024, 0xE59D0028, 0xE1A02800, 0xE1A03C00, 0xE1A02C22,
      0xE1833802, 0xE1A02400, 0xE1A02C22, 0xE1832402, 0xE1821C20, 0xE5970010, 0xE1A0C000, 0xE1A02800,
      0xE1A03C00, 0xE1A02C22, 0xE1833802, 0xE1A02400, 0xE1A02C22, 0xE1832402, 0xE1820C20, 0xE1510000,
      0x1A000013, 0xE59D002C, 0xE1A02800, 0xE1A03C00, 0xE1A02C22, 0xE1833802, 0xE1A02400, 0xE1A02C22,
      0xE1832402, 0xE1821C20, 0xE5970014, 0xE1A02800, 0xE1A03C00, 0xE1A02C22, 0xE1833802, 0xE1A02400,
      0xE1A02C22, 0xE1832402, 0xE1820C20, 0xE1510000, 0x0A000014, 0xE58DC028, 0xE5970014, 0xE59D205C,
      0xE28D1028, 0xE58D002C, 0xE5D53027, 0xE5950674, 0xE5D22020, 0xE0800203, 0xEB0004E4, 0xE5D50027,
      0xE3A02001, 0xE2800003, 0xEE000A10, 0xE59D0030, 0xEEB80A40, 0xED800A99, 0xE5D50027, 0xE2800004,
      0xE5C50027, 0xE5C52690, 0xE5D53026, 0xE59D0030, 0xE2871018, 0xEE003A10, 0xEEB80A40, 0xED800A98,
      0xE5D50026, 0xE5952670, 0xEC910A04, 0xE2800001, 0xE20000FF, 0xE0822203, 0xE5C50026, 0xEC820A04,
      0xEE000A10, 0xE59D1030, 0xEEB80A40, 0xED810A97, 0xE59D1040, 0xE5D52026, 0xEC970A04, 0xE2822001,
      0xE5913000, 0xE5C52026, 0xE3A01001, 0xE0830200, 0xE1A01A11, 0xEC800A04, 0xE5D90000, 0xE1C00001,
      0xE1800001, 0xE5C90000, 0xE59D2030, 0xE5D51024, 0xE5950668, 0xE2822F96, 0xE8921288, 0xE0800201,
      0xE59D201C, 0xE2822001, 0xE58D201C, 0xE8801288, 0xE5D50024, 0xE2800001, 0xE20000FF, 0xE3500019,
      0xE5C50024, 0x95D50026, 0x9350001E, 0x95D50027, 0x93500018, 0x9A000002, 0xE1A01008, 0xE1A00005,
      0xEB000058, 0xE2844001, 0xE154000B, 0x3AFFFF73, 0xE59D001C, 0xE150000B, 0x3AFFFEAD, 0xE5D50024,
      0xE3500000, 0x0A000002, 0xE1A01008, 0xE1A00005, 0xEB00004C, 0xE5D80009, 0xE3A01000, 0xE5C51027,
      0xE3100002, 0x0A00001E, 0xE5982018, 0xE5980010, 0xE3A03008, 0xE0800102, 0xE0832102, 0xE312000F,
      0x0A00000E, 0xE1A02E02, 0xE1A02F22, 0xE262C004, 0xE35C0000, 0xDA000009, 0xE2402004, 0xE31C0001,
      0x15A21004, 0xE1B030CC, 0x0A000003, 0xE5821004, 0xE2533001, 0xE5A21008, 0x1AFFFFFB, 0xE080010C,
      0xE3A01001, 0xEBFE6209, 0xE5982010, 0xE0400002, 0xE1A00140, 0xE5880018, 0xE1A01100, 0xE1A00002,
      0xEB0E5B2C, 0xE28DD064, 0xE8BD8FF0, 0x00011001, 0x00040080, 0x000F0085, 0x000F0082, 0x000F0083,
      0xE92D4070, 0xE1A04000, 0xE5900000, 0xE1A05001, 0xE3A01000, 0xE590200C, 0xE1A00004, 0xE12FFF32,
      0xE2851004, 0xE1A00004, 0xE8BD4070, 0xE1A00000, 0xE92D4010, 0xE1A04000, 0xE2800034, 0xEBFDCB3D,
      0xE594C034, 0xE59F1034, 0xE2840028, 0xE5A4C030, 0xE3A03000, 0xE5841004, 0xE5843054, 0xE59F4020,
      0xE1A01000, 0xE3A02060, 0xE5940000, 0xEBFDA915, 0xE5940000, 0xE2800060, 0xE5840000, 0xE8BD8010,
      0x013F02C1, 0x005F1DC0]),
    (9, "nw::lyt (NSMB2's drawer): drawing begun",
     [0xE92D4070, 0xE1A04000, 0xE3A00000, 0xE5840698, 0xE1A05001, 0xE584069C, 0xE3A01006, 0xE5840694,
      0xE5C416A2, 0xE3A02001, 0xE5C426A0, 0xE1A02000, 0xE1A01000, 0xE1A00004, 0xEBFF9C9A, 0xE1A01005,
      0xE1A00004, 0xE8BD4070, 0xE1A00000, 0xE92D41F0, 0xE3110001, 0xE1A04000, 0xE3A05000, 0x1A00000F,
      0xE59FC040, 0xE59F2040, 0xE59F7040, 0xE3A06A01, 0xE59C1000, 0xE5812004, 0xE2812008, 0xE5815000,
      0xE59F102C, 0xE2820018, 0xEC910A04, 0xEC820A04, 0xE1C261F0, 0xE58C0000, 0xE2840004, 0xEB0E55FE,
      0xE5845684, 0xE8BD81F0]),
    (10, "nw::lyt (NSMB2's drawer): drawing ended",
     [0xE92D4070, 0xE1A05001, 0xE5D01024, 0xE1A04000, 0xE3510000, 0x1BFF9E1E, 0xE1A01005, 0xE1A00004,
      0xE8BD4070, 0xE1A00000, 0xE3110001, 0x1A000001, 0xE2800014, 0xEA0E5627, 0xE12FFF1E]),
    (11, "nw::lyt (NSMB2's drawer): drawing begun with a projection",
     [0xE92D4070, 0xE1A04000, 0xE5900000, 0xE1A05001, 0xE3A01000, 0xE590200C, 0xE1A00004, 0xE12FFF32,
      0xE2851004, 0xE1A00004, 0xE8BD4070, 0xE1A00000, 0xE92D4010, 0xE1A04000, 0xE2800034, 0xEBFDCB3D,
      0xE594C034, 0xE59F1034, 0xE2840028, 0xE5A4C030, 0xE3A03000, 0xE5841004, 0xE5843054, 0xE59F4020,
      0xE1A01000, 0xE3A02060, 0xE5940000, 0xEBFDA915, 0xE5940000, 0xE2800060, 0xE5840000, 0xE8BD8010,
      0x013F02C1, 0x005F1DC0]),
    (12, "nngx: a block of commands added, copied or not (SDK 2012)",
     [0xE92D41F0, 0xE1A06001, 0xE1A08000, 0xE59F50C4, 0xE59F70C4, 0xE3520000, 0xE595409C, 0xE5970000,
      0xE5941004, 0xE0401001, 0xE584100C, 0x0A000007, 0xE1A02006, 0xE1A01008, 0xEBEF5172, 0xE5970000]),
    (13, "nngx: a block {address, size, copied} added (SDK 2012)",
     [0xE5D0100C, 0xE3510000, 0x13A02001, 0x03A02000, 0xE1C000D0, 0xE1A00000, 0xE92D41F0, 0xE1A06001,
      0xE1A08000, 0xE59F50C4, 0xE59F70C4, 0xE3520000, 0xE595409C, 0xE5970000, 0xE5941004, 0xE0401001]),
    (14, "NSMB2: a viewport's commands written",
     [0xE92D01F0, 0xE1A03000, 0xE5900008, 0xED9F1A5F, 0xEE000A10, 0xEEB80A40, 0xEE200A01, 0xEE102A10,
      0xE3D2C102, 0x0A000006, 0xE1A0C082, 0xE1A0CC2C, 0xE24CC040, 0xE35C0000, 0xB1A02FA2, 0xB1A0CB82,
      0xBA000004, 0xE1A04482, 0xE1A02FA2, 0xE1A04824, 0xE184C80C, 0xE18CCB82, 0xEE000A10, 0xEDDF0A4C,
      0xEEF81A40, 0xEE800AA1, 0xEE102A10, 0xE3D20102, 0x01A04482, 0x01A044A4, 0x0A000008, 0xE1A00082,
      0xE1A04482, 0xE1A00C20, 0xE2400040, 0xE3500000, 0xB1A00FA2, 0xE1A044A4, 0xB1A00F00, 0xBA000002,
      0xE1A02FA2, 0xE1840B80, 0xE1800F02, 0xE1A05080, 0xE593000C, 0xEE000A10, 0xEEB80A40, 0xEE200A01,
      0xEE102A10, 0xE3D24102, 0x0A000006, 0xE1A04082, 0xE1A04C24, 0xE2444040, 0xE3540000, 0xB1A02FA2,
      0xB1A04B82, 0xBA000004, 0xE1A06482, 0xE1A02FA2, 0xE1A06826, 0xE1864804, 0xE1844B82, 0xEE000A10,
      0xEEB81A40, 0xEE800A81, 0xEE102A10, 0xE3D20102, 0x01A06482, 0x01A064A6, 0x0A000008, 0xE1A00082,
      0xE1A06482, 0xE1A00C20, 0xE2400040, 0xE3500000, 0xB1A00FA2, 0xE1A064A6, 0xB1A02F00, 0xBA000002,
      0xE1A02FA2, 0xE1860B80, 0xE1802F02, 0xE59F8044, 0xE59F7044, 0xE2810008, 0xE581C000, 0xE5818004,
      0xE8A000A0, 0xE1886947, 0xE1A01082, 0xE8A00050, 0xE2872002, 0xE8A00006, 0xE282C024, 0xE8930006,
      0xE1811802, 0xE8A01002, 0xE8BD01F0, 0xE12FFF1E, 0x3F000000, 0x40000000, 0x00070041, 0x000F0042]),
    (15, "NSMB2: a scissor's commands written",
     [0xE92D0030, 0xE2802004, 0xE280300C, 0xE8921004, 0xE8930018, 0xE5D05000, 0xE0822003, 0xE08C3004,
      0xE3550000, 0xE2434001, 0xE242C001, 0xE59F2078, 0x13A03003, 0x03A03000, 0xE4813004, 0xE4812004,
      0xE5902004, 0xE3520000, 0xB3A02000, 0xBA000002, 0xE5903014, 0xE1530002, 0xD2432001, 0xE5903008,
      0xE3530000, 0xB3A00000, 0xBA000003, 0xE5900018, 0xE1500003, 0xD2403001, 0xE1A00803, 0xE1800002,
      0xE35C0000, 0xE4810004, 0xB3A00000, 0xBA000002, 0xE3540000, 0xB3A04000, 0xE18C0804, 0xE5810000,
      0xE8BD0030, 0xE2810004, 0xE12FFF1E, 0x802F0065]),
    # the C library's copies (the ARM compiler's runtime): a block copied into the command list becomes its
    # operation (translated once, checked by its content), the rest copies as before without asking the host
    (16, "__aeabi_memcpy (unaligned): a block copied into the command list",
     [0xE3520008, 0x8A00002F, 0x0A000029, 0xE3520004, 0x8A00000D, 0x0A000019, 0xE3520002, 0x3A000005,
      0x0A000013, 0xE0D120B2, 0xE4D13001, 0xE0C020B2]),
    (17, "__aeabi_memcpy4: a block copied into the command list",
     [0xE92D47F0, 0xE2522020, 0x3A000009, 0xE8B10078, 0xF5D1F040, 0xE8B10780, 0xE8A00078, 0xE2522020,
      0xE8A00780, 0x28B10078, 0xF5D1F040, 0x28B10780]),
    (18, "__aeabi_memcpy: a block copied into the command list",
     [0xE3520003, 0x9A000017, 0xE210C003, 0x0A000008, 0xE4D13001, 0xE35C0002, 0xE082200C, 0x94D1C001,
      0xE4C03001, 0x34D13001, 0x94C0C001, 0xE2422004]),
    (19, "nw::gfx: a camera's matrices sent",
     [0xE92D41F0, 0xE1A04001, 0xE5901028, 0xE1510004, 0x03520000, 0x0A00002A, 0xE59F50B0, 0xE5804028,
      0xE59F60A4, 0xE59F809C, 0xE5950000, 0xE59F20A0, 0xE2841F52, 0xE5806004, 0xE5808000, 0xE2800008]),
    (20, "nw::gfx: a shader's booleans and integers sent (6 words copied)",
     [0xE92D0E30, 0xE8B1003C, 0xE8B11800, 0xE8A0003C, 0xE8A01800, 0xE8BD0E30, 0xE12FFF1E]),
    (21, "nw::gfx: a 3x4 matrix written after its index command",
     [0xECB10A0C, 0xE5802004, 0xED800A04, 0xEDC00A03, 0xED801A02, 0xEDC01A00, 0xED802A08, 0xEDC02A07,
      0xED803A06, 0xEDC03A05, 0xED804A0C, 0xEDC04A0B, 0xED805A0A, 0xEDC05A09, 0xE12FFF1E]),
    (22, "nw::gfx: a 4x4 matrix written after its index command",
     [0xECB10A10, 0xE5802004, 0xED800A04, 0xEDC00A03, 0xED801A02, 0xEDC01A00, 0xED802A08, 0xEDC02A07,
      0xED803A06, 0xEDC03A05, 0xED804A0C, 0xEDC04A0B, 0xED805A0A, 0xEDC05A09, 0xED806A10, 0xEDC06A0F]),
    (23, "nw::gfx: a vector written after its index command",
     [0xECB10A03, 0xE3A03000, 0xE5803000, 0xED800A03, 0xEDC00A02, 0xED801A01, 0xE12FFF1E]),
    # notices (the host returns no, the game's own function runs): what an operation's mark lives in
    (24, "nngx: a recording started (SDK 2012)",
     [0xE59F3074, 0xE92D0030, 0xE59F1070, 0xE593C09C, 0xE5910000, 0xE59C2004, 0xE0402002, 0xE3120008,
      0xE58C200C, 0x0A000009, 0xE59F2054, 0xE5922000]),
    (25, "nngx: a recording stopped (SDK 2012)",
     [0xE92D03F0, 0xE3A07000, 0xE59FC088, 0xE59F4088, 0xE5CC7013, 0xE59C609C, 0xE5945000, 0xE5968004,
      0xE0458008, 0xE3180008, 0xE586800C, 0x0A000007]),
    (26, "nngx: a list cleared (SDK 2012)",
     [0xE59F007C, 0xE92D4010, 0xE590409C, 0xE3540000, 0x0A00001A, 0xE5940004, 0xE3500000, 0x1A000002,
      0xE5940018, 0xE3500000, 0x0A000014, 0xE5941020]),
    (27, "nw::gfx: a 3x4 matrix written in a palette (no header)",
     [0xECB10A0C, 0xED800A03, 0xEDC00A02, 0xED801A01, 0xEDC01A00, 0xED802A07, 0xEDC02A06, 0xED803A05, 0xEDC03A04, 0xED804A0B, 0xEDC04A0A, 0xED805A09, 0xEDC05A08, 0xE12FFF1E]),
    (28, "nw::gfx: a vec4 written after its index command",
     [0xECB10A04, 0xED800A03, 0xEDC00A02, 0xED801A01, 0xEDC01A00, 0xE12FFF1E]),
    (29, "NSMB2: an indexed draw's commands written",
     [0xE92D00F0, 0xE3A03001, 0xE5D20008, 0xE59F6080, 0xE3500000, 0xE59F0074, 0x13A05000, 0x03A05102,
      0xE2464038, 0xE590C000, 0xE5920000, 0xE8810048, 0xE26CC000, 0xE080C18C, 0xE2810008, 0xE185700C,
      0xE4807004, 0xE59FC04C, 0xE4804004, 0xE5922004, 0xE0845003, 0xE3A01000, 0xE4802004, 0xE1844183,
      0xE4805004, 0xE0842083, 0xE8A01002, 0xE8A00018, 0xE8A01008, 0xE4803004, 0xE4802004, 0xE59F2018,
      0xE8A00006, 0xE8A00006, 0xE8BD00F0, 0xE12FFF1E, 0x005F204C, 0x000F025F, 0x00010245, 0x0008025E]),
    (30, "NSMB2: a vertex format's commands written (translated after)",
     [0xE92D4FF0, 0xE24DD05C, 0xE1A08000, 0xE1A03001, 0xE3A02000, 0xE59FC4FC, 0xE59F04FC, 0xE59F54FC,
      0xE5A1C004, 0xE2816004, 0xE2811008, 0xE28D7024, 0xE8810005, 0xE2811004, 0xE0854840, 0xE58D1004,
      0xE2810004, 0xE8870048, 0xE1A01002, 0xE4805004, 0xE59FC4CC, 0xE8A00014, 0xE2803008, 0xE3A05000,
      0xE59CC000, 0xE58D3048, 0xE280300C, 0xE58D3030, 0xE480C004, 0xE58D004C, 0xE5802004, 0xE2803008,
      0xE280000C, 0xE1A06005, 0xE58D3034, 0xE58DC050, 0xE0662186, 0xE088C202, 0xE5BC2160, 0xE3520000,
      0x0A000075, 0xE3A02000, 0xE1A03002, 0xE1A07002, 0xE3A04001, 0xE2855001, 0xE58D2008, 0xE58D200C,
      0xE08C9002, 0xE5D99004, 0xE2499002, 0xE359000E, 0x379FF109, 0xEA000016, 0x00487FB0, 0x00487FC4,
      0x00487FD4, 0x00487FD4, 0x00487FB0, 0x00487FC4, 0x00487FD4, 0x00487FD4, 0x00487FB0, 0x00487FC4,
      0x00487FD4, 0x00487FD4, 0x00487FB0, 0x00487FC4, 0xE3540002, 0xE2833001, 0xE3C33001, 0xB3A04002,
      0xEA000003, 0xE3540004, 0xE2833003, 0xE3C33003, 0xB3A04004, 0xE08C9102, 0xE58D9044, 0xE5999010,
      0xE3590000, 0x0A00003C, 0xE59DA044, 0xE0833009, 0xE59AA040, 0xE35A0000, 0xE1A0AFC2, 0xBA000027,
      0xE082BEAA, 0xE28D9008, 0xE1A0A1CB, 0xE3CBB007, 0xE042B00B, 0xE7B9A10A, 0xE1A0B10B, 0xE18AAB11,
      0xE589A000, 0xE1A09FC1, 0xE08CA002, 0xE0819EA9, 0xE5DAB004, 0xE1A0A1C9, 0xE3C9E007, 0xE59D9048,
      0xE089910A, 0xE041A00E, 0xE599E000, 0xE1A0A10A, 0xE18EBA1B, 0xE589B000, 0xE3A0B00F, 0xE3C19007,
      0xE1A0EA1B, 0xE58D902C, 0xE59DB004, 0xE2811001, 0xE79B9009, 0xE1C9B00E, 0xE59D902C, 0xE59DE004,
      0xE78EB009, 0xE59D9044, 0xE5999040, 0xE18BBA19, 0xE59D902C, 0xE59DA004, 0xE78AB009, 0xEA00000A,
      0xE082AEAA, 0xE3A0E00B, 0xE1A0B1CA, 0xE3CAA007, 0xE08EE129, 0xE28D9008, 0xE042A00A, 0xE7B9B10B,
      0xE1A0A10A, 0xE18BAA1E, 0xE589A000, 0xE2822001, 0xE352000C, 0xE2877001, 0x3AFFFFA0, 0xE59D2050,
      0xE59CC000, 0xE0833004, 0xE2622000, 0xE2439001, 0xE2444001, 0xE08C3182, 0xE1C92004, 0xE4803004,
      0xE1A03802, 0xE59D2008, 0xE4802004, 0xE1832E07, 0xE59D300C, 0xE1822003, 0xE4802004, 0xE2866001,
      0xE356000C, 0x3AFFFF81, 0xE0853085, 0xE59F2294, 0xE1A03A03, 0xE2833602, 0xE1833002, 0xE59D204C,
      0xE3150001, 0xE5823000, 0x0A000001, 0xE3A02000, 0xE4802004, 0xE59F6270, 0xE3A0C00B, 0xE3A04000,
      0xE3A07001, 0xE3A0500F, 0xE08C210C, 0xE0882102, 0xE5F236A0, 0xE3530000, 0x0A00007D, 0xE8A00042,
      0xED920A03, 0xEE103A10, 0xE3D39102, 0x0A000006, 0xE1A09083, 0xE1A09C29, 0xE2499040, 0xE3590000,
      0xB1A03FA3, 0xB1A03B83, 0xBA000004, 0xE1A0A483, 0xE1A0BFA3, 0xE1A0382A, 0xE1833809, 0xE1833B8B,
      0xED920A04, 0xE1A03403, 0xE1A0AC23, 0xEE103A10, 0xE3D39102, 0x0A000006, 0xE1A09083, 0xE1A09C29,
      0xE2499040, 0xE3590000, 0xB1A03FA3, 0xB1A03B83, 0xBA000004, 0xE1A0B483, 0xE1A03FA3, 0xE1A0B82B,
      0xE18B9809, 0xE1893B83, 0xE18A3403, 0xE4803004, 0xED920A03, 0xEE103A10, 0xE3D39102, 0x0A000006,
      0xE1A09083, 0xE1A09C29, 0xE2499040, 0xE3590000, 0xB1A03FA3, 0xB1A03B83, 0xBA000004, 0xE1A0A483,
      0xE1A0BFA3, 0xE1A0382A, 0xE1833809, 0xE1833B8B, 0xED920A02, 0xE1A09803, 0xEE103A10, 0xE3D3A102,
      0x0A000006, 0xE1A0A083, 0xE1A0AC2A, 0xE24AA040, 0xE35A0000, 0xB1A03FA3, 0xB1A03B83, 0xBA000004,
      0xE1A0B483, 0xE1A03FA3, 0xE1A0B82B, 0xE18BA80A, 0xE18A3B83, 0xE1A03423, 0xE6833019, 0xE4803004,
      0xED920A01, 0xEE103A10, 0xE3D39102, 0x0A000006, 0xE1A09083, 0xE1A09C29, 0xE2499040, 0xE3590000,
      0xB1A03FA3, 0xB1A03B83, 0xBA000004, 0xE1A0A483, 0xE1A03FA3, 0xE1A0A82A, 0xE18A9809, 0xE1893B83,
      0xED920A02, 0xE3C394FF, 0xEE102A10, 0xE3D23102, 0x0A000006, 0xE1A03082, 0xE1A03C23, 0xE2433040,
      0xE3530000, 0xB1A02FA2, 0xB1A02B82, 0xBA000004, 0xE1A0A482, 0xE1A02FA2, 0xE1A0A82A, 0xE18A3803,
      0xE1832B82, 0xE1892C02, 0xE8A00014, 0xE1A02FC1, 0xE59D3004, 0xE0812EA2, 0xE3C22007, 0xE0833002,
      0xE0412002, 0xE5939000, 0xE1A02102, 0xE1C99215, 0xE189221C, 0xE5832000, 0xE59D2030, 0xE2813010,
      0xE2811001, 0xE5922000, 0xE1823317, 0xE59D2030, 0xE5823000, 0xE25CC001, 0x5AFFFF7A, 0xE3A0320F,
      0xE2412001, 0xE0833E01, 0xE59D1024, 0xE382C20A, 0xE581C000, 0xE59D1028, 0xE5812000, 0xE59D1034,
      0xE5911000, 0xE1812003, 0xE59D1034, 0xE5812000, 0xE28DD05C, 0xE8BD8FF0, 0x000B02B9, 0x00010242,
      0x000F02BB, 0x005F204C]),
    (31, "nw::gfx: a material activated (simple activator) (translated around)",
     [0xE92D4FF7, 0xE24DD078, 0xE59D007C, 0xE2806040, 0xE5D000E8, 0xE5D610AB, 0xE3500000, 0x05D600A9, 0x03500000, 0x13A00001, 0xE58D0064, 0xE59D007C]),
    (32, "nw::gfx: a material activated (translated around)",
     [0xE92D4FF7, 0xE24DD070, 0xE1A08002, 0xE59D0074, 0xE2805040, 0xE5D010E8, 0xE5906008, 0xE3510000, 0x05D500A9, 0xE5D510AB, 0x03500000, 0x13A00001]),
    (33, "nw::gfx: lighting tables' inputs set (translated around)",
     [0xE92D4FF0, 0xE3A03002, 0xE24DD00C, 0xE1A04001, 0xE3A09000, 0xE5902000, 0xE59F63EC, 0xE59F73EC, 0xE1D33002, 0x03A0B001, 0x13A0B000, 0xE3A03004]),
    (34, "nw::gfx: texture coordinates set (translated around)",
     [0xE92D4FF7, 0xE1A07001, 0xE1A06002, 0xE59F03C4, 0xE59F93C8, 0xE59FA3C8, 0xE2801008, 0xED2D8B06, 0xE24DDE11, 0xE28AB501, 0xE5904010, 0xE5903004]),
    (35, "nw::gfx: light environment set (translated around)",
     [0xE92D4FFF, 0xE24DD024, 0xE2821020, 0xE3A09000, 0xE59FA3B0, 0xE58D2004, 0xE59F43A4, 0xE3A02156, 0xE59A0000, 0xE8A00014, 0xEBFE4A2A, 0xE59A0000]),
    (36, "NSMB2: a render target set (translated around)",
     [0xE92D4070, 0xE1A04000, 0xE5D0005C, 0xE1A05002, 0xE3500000, 0xE3A00000, 0x0A00000D, 0xE5D4005D,
      0xE3500000, 0x0A000002, 0xE3500001, 0x0A000004, 0xEA000006, 0xE5D4005E, 0xE3500000, 0x0A000067,
      0xEA000002, 0xE5D4005E, 0xE3500001, 0x0A000063, 0xE3A00001, 0xE59F218C, 0xE8A10005, 0xE3A02000,
      0xE1A00004, 0xEB001163, 0xE5D41010, 0xE3510000, 0x0A00000B, 0xE3A02739, 0xE59F116C, 0xE4802004,
      0xE282381D, 0xE59F2164, 0xE4801004, 0xE4803004, 0xE0221841, 0xE4802004, 0xE5D42011, 0xE4802004,
      0xE4801004, 0xE1A01000, 0xE3A03001, 0xE3A02000, 0xE2840018, 0xEB001208, 0xE5D41030, 0xE5D43032,
      0xE5D4C031, 0xE3510000, 0x13A02001, 0x03A02000, 0xE1822203, 0xE59F1118, 0xE182240C, 0xE4802004,
      0xE2846040, 0xE4801004, 0xE5D41038, 0xE5D4203C, 0xE5D43039, 0xE8961040, 0xE3510000, 0x13A01001,
      0xE1811202, 0xE1811403, 0xE1811806, 0xE1812C0C, 0xE59F10E0, 0xE4802004, 0xE4801004, 0xE5D42049,
      0xE5D4304A, 0xE5D41048, 0xE1A02202, 0xE1822403, 0xE1822001, 0xE59F10C0, 0xE4802004, 0xE4801004,
      0xE5D41054, 0xE5D43056, 0xE3510000, 0xE5941058, 0x13A02001, 0x03A02000, 0xE1822203, 0xE5D11050,
      0xE3110001, 0x13A03C01, 0x03A03000, 0xE3110002, 0xE1822003, 0x13A03C02, 0x03A03000, 0xE3110004,
      0xE1822003, 0x13A03B01, 0x03A03000, 0xE3110008, 0xE1821003, 0xE5D43055, 0x13A02B02, 0x03A02000,
      0xE3530000, 0xE1811002, 0x13A02A01, 0x03A02000, 0xE1822001, 0xE59F1044, 0xE4802004, 0xE4801004,
      0xE1A01000, 0xE2840064, 0xEB00113B, 0xE1A01000, 0xE1A02005, 0xE284007C, 0xE8BD4070, 0xEA002257,
      0xE3A00002, 0xEAFFFF9A, 0x000F0040, 0x00030100, 0x000F0101, 0x00030104, 0x000F0105, 0x000F0106,
      0x000F0107]),
    (37, "nw::gfx: dummy commands written (translated around)",
     [0xE92D0E30, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xF5D1F040,
      0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00,
      0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00,
      0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C,
      0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C,
      0xE8A01E00, 0xE8B1003C, 0xF5D1F040, 0xE8B11E00, 0xE8A0003C, 0xE8A01E00, 0xE8B1003C, 0xE8B11800,
      0xE8A0003C, 0xE8A01800, 0xE8BD0E30, 0xE12FFF1E]),
    (38, "nngx: the list split (SDK 2012) (translated around)",
     [0xE92D41F0, 0xE59F5190, 0xE59F0190, 0xE595409C, 0xE5901000, 0xE5942004, 0xE0412002, 0xE584200C, 0xE5943014, 0xE1530002, 0x0A00005A, 0xE59F3170]),
    (39, "nw::gfx: a light's parameters set (translated around)",
     [0xE92D00F0, 0xE3A0C000, 0xE5913000, 0xE59F4110, 0xE5912004, 0xE2135102, 0xE1A06483, 0xE1A07083, 0xE1A064A6, 0x13A05902, 0xE3D33102, 0xE1A07C27]),
    (40, "nw::gfx: fog set (translated around)",
     [0xE92D4070, 0xE59000A0, 0xE3500000, 0x0A00005B, 0xE5901008, 0xEDDF0A5A, 0xE3A00000, 0xE28130C4, 0xED911A2D, 0xE591C0C4, 0xED9F0A56, 0xEEB41AE0]),
    (41, "NSMB2: an effect's matrix set (translated around)",
     [0xE92D4070, 0xE1A05001, 0xE1A04002, 0xE59F10A0, 0xE3A02F52, 0xE1A00005, 0xEBF28D0C, 0xE59F0098, 0xE59F3090, 0xE3A02000, 0xE5850154, 0xE5852150]),
]
# natives the host sees after the game's own function ran, with its arguments (what it made, translated once)
AFTER_NATIVES = {8, 30, 36}
# copies: the host is asked only when the destination is where the game's next command goes
LIST_COPY_NATIVES = {16, 17, 18}
# natives around the game's function: it runs, then the host takes what it wrote at the cursor (r2: where the cursor
# was, r3: where it is) as one operation; the game's code computes, the host sets, picanative reads nothing there
AROUND_NATIVES = (set(range(31, 39)) - {36, 37}) | {39, 40, 41}
NNGX_ADD_3D_COMMAND = 12  # its literal at +0x10 is that place's address

# The natives checked game by game: tools/ab_natives.py ran the game with and without them, the captures identical
# byte for byte. A native right on one game can be wrong on another (a function the same, used otherwise): a game
# gets only those checked on it, the others run as the game's own code (CTRN_ALL_NATIVES=1: all, to check them).
ALL_NATIVES = {n for n, _, _ in GFX_NATIVES}
CHECKED_NATIVES = {
    0x000400000007AF00: ALL_NATIVES,  # NSMB2 (EUR): menus, world map, level 1-1, 158 captures of 158 (2026-11-23)
    # MK7 (EUR): to the first race, 3 600 frames; 20 (a 6-word block MK7 changes in the list) and 21 (3x4 matrices
    # held too briefly: marks naming none) change its pictures (2026-11-23)
    0x0004000000030700: ALL_NATIVES - {20, 21},
}


def command_cursor(found, code, base):
    """the address of the word nngx keeps the next command's place in, read from nngxAdd3DCommand's code"""
    for address, number, _ in found:
        if number == NNGX_ADD_3D_COMMAND:
            at = address + 0x10 - base
            word = struct.unpack_from("<I", code, at)[0]
            return struct.unpack_from("<I", code, at + 8 + (word & 0xFFF))[0]
    return None


def instruction_mask(word):
    if word & 0x0F7F0000 == 0x051F0000:  # ldr rN, [pc, #i]
        return 0xFFFFF000
    if word & 0x0E000000 == 0x0A000000:  # b, bl
        return 0xFF000000
    return 0xFFFFFFFF


def gfx_natives(code, base):
    """where each of GFX_NATIVES is in the code: (address, number, what)"""
    words = struct.unpack_from(f"<{len(code) // 4}I", code)
    found = []
    for number, what, signature in GFX_NATIVES:
        masks = [instruction_mask(w) for w in signature]
        first, mask0 = signature[0] & masks[0], masks[0]
        for i in range(len(words) - len(signature)):
            if words[i] & mask0 == first and all(words[i + k] & masks[k] == signature[k] & masks[k]
                                                 for k in range(1, len(signature))):
                found.append((base + i * 4, number, what))
    return found


def natives_source(found, cursor=None):
    """the overrides that hand each found function to the host (svc 0xF0, r12: its number); the host says no by
    setting r12 to 0, and the game's own function runs"""
    out = ['#include "overrides.h"\n']
    for address, number, what in found:
        if number in LIST_COPY_NATIVES:
            if cursor is None:
                continue
            out.append(f"/* {what} */\n"
                       f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                       # inside the current list only: from the cursor to its end (the word after the cursor's)
                       f"    const uint32_t cursor_ = mem_read32_in(FASTMEM(ctx), 0x{cursor:08X}u);\n"
                       f"    const uint32_t end_ = mem_read32_in(FASTMEM(ctx), 0x{cursor + 4:08X}u);\n"
                       f"    if (cursor_ <= ctx->r[0] && ctx->r[0] < end_ && ctx->r[2] <= end_ - ctx->r[0]) {{\n"
                       f"        ctx->r[12] = {number}u;\n"
                       f"        SVC(0x{address:08X}u, 0xF0u);\n"
                       f"        if (ctx->r[12] != 0) RETURN_TO(ctx->r[14]);\n"
                       f"    }}\n"
                       f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                       f"}}\n")
            continue
        if number in AROUND_NATIVES:
            if cursor is None:
                continue
            out.append(f"/* {what} */\n"
                       f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                       f"    const uint32_t a0 = ctx->r[0], a1 = ctx->r[1], before = mem_read32_in(FASTMEM(ctx), 0x{cursor:08X}u);\n"
                       f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                       f"    const uint32_t back = ctx->r[15] | (ctx->thumb ? 1u : 0u), result = ctx->r[0], r1 = ctx->r[1];\n"
                       f"    ctx->r[0] = a0, ctx->r[1] = a1, ctx->r[2] = before;\n"
                       f"    ctx->r[3] = mem_read32_in(FASTMEM(ctx), 0x{cursor:08X}u);\n"
                       f"    ctx->r[12] = {number}u;\n"
                       f"    SVC(0x{address:08X}u, 0xF0u);\n"
                       f"    ctx->r[0] = result, ctx->r[1] = r1;\n"
                       f"    RETURN_TO(back);\n"
                       f"}}\n")
            continue
        if number in AFTER_NATIVES:
            out.append(f"/* {what} */\n"
                       f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                       f"    const uint32_t a0 = ctx->r[0], a1 = ctx->r[1], a2 = ctx->r[2], a3 = ctx->r[3];\n"
                       f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                       f"    const uint32_t back = ctx->r[15] | (ctx->thumb ? 1u : 0u), result = ctx->r[0];\n"
                       f"    ctx->r[0] = a0, ctx->r[1] = a1, ctx->r[2] = a2, ctx->r[3] = result; (void)a3;\n"
                       f"    ctx->r[12] = {number}u;\n"
                       f"    SVC(0x{address:08X}u, 0xF0u);\n"
                       f"    ctx->r[0] = result;\n"
                       f"    RETURN_TO(back);\n"
                       f"}}\n")
            continue
        out.append(f"/* {what} */\n"
                   f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                   f"    ctx->r[12] = {number}u;\n"
                   f"    SVC(0x{address:08X}u, 0xF0u);\n"
                   f"    if (ctx->r[12] == 0) {{\n"
                   f"        CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                   f"        return;\n"
                   f"    }}\n"
                   f"    RETURN_TO(ctx->r[14]);\n"
                   f"}}\n")
    return "\n".join(out)


def default_cache():
    base = os.environ.get("LOCALAPPDATA")
    return Path(base) / "ctrnative" if base else Path.home() / ".cache" / "ctrnative"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--3dsrecomp", dest="recomp", default=os.environ.get("CTRN_3DSRECOMP", "3dsrecomp"))
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--cache", type=Path, default=default_cache())
    parser.add_argument("--host", type=Path,
                        default=HERE.parent / "build" / ("ctrnative.exe" if os.name == "nt" else "ctrnative"))
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    rom = args.rom.resolve()
    title_id, digest = game_identity(rom)
    # a mod, as Luma3DS has them: <mods>/luma/titles/<id>/ (or <mods>/<id>/), its romfs folder and its code.ips;
    # <mods> is CTRN_MODS_DIR or %LOCALAPPDATA%/ctrnative/mods; CTRN_NO_MODS=1 plays the game as it is
    mod_dir, mod_digest = None, ""
    if not os.environ.get("CTRN_NO_MODS"):
        mods_root = Path(os.environ.get("CTRN_MODS_DIR") or default_cache() / "mods")
        for candidate in (mods_root / "luma" / "titles" / f"{title_id:016X}", mods_root / f"{title_id:016X}"):
            if (candidate / "romfs").is_dir() or (candidate / "code.ips").is_file():
                mod_dir = candidate
                break
    if mod_dir:
        h = hashlib.sha256()
        for p in sorted(mod_dir.rglob("*")):
            if p.is_file():
                h.update(str(p.relative_to(mod_dir)).encode() + b"\0" + str(p.stat().st_size).encode() + b"\0" +
                         str(p.stat().st_mtime_ns).encode())
        mod_digest = h.hexdigest()[:8]
        print(f"[ctrn] mod {mod_dir} ({mod_digest})", flush=True)
    # the cheats on (the codes named in <cheats>/<title ID>.on, or CTRN_CHEATS_ON): what they write into the code
    # without a condition is patched in before translation (the code runs translated: written as the game runs,
    # it would change nothing); their writes to data the host makes each frame
    cheats_dir = Path(os.environ.get("CTRN_CHEATS_DIR") or default_cache() / "cheats")
    cheats_file = next((p for p in cheats_dir.glob(f"{title_id:016X}.*") if p.suffix.lower() == ".txt"), None)
    on_file = cheats_dir / f"{title_id:016X}.on"
    cheats_on = [n for n in os.environ.get("CTRN_CHEATS_ON", "").split(";") if n.strip()]
    if not cheats_on and on_file.is_file():
        cheats_on = [l.strip() for l in on_file.read_text(encoding="utf-8").splitlines() if l.strip()]
    cheat_writes, cheat_digest = [], ""
    if cheats_file and cheats_on:
        exheader0, _ = read_code(rom)
        code_start = struct.unpack_from("<I", exheader0, 0x10)[0]
        ro_address, ro_pages = struct.unpack_from("<II", exheader0, 0x20)
        all_codes = cheats.parse(cheats_file.read_text(encoding="utf-8", errors="replace"))
        for name, lines in all_codes.items():
            if "*" not in cheats_on and name not in cheats_on:
                continue
            writes, uncertain = cheats.code_writes(lines, code_start, ro_address + ro_pages * 0x1000)
            cheat_writes += writes
            if uncertain:
                print(f"[ctrn] cheat {name!r}: its writes into the code depend on a condition (a key, memory): those "
                      f"cannot be patched ahead, they do nothing", flush=True)
        if cheat_writes:
            cheat_digest = hashlib.sha256(repr(cheat_writes).encode()).hexdigest()[:8]
            print(f"[ctrn] cheats patch the code in {len(cheat_writes)} places ({cheat_digest})", flush=True)
    # a mod that changes code (code.ips, modules) has a cache of its own; one of data files shares the game's
    code_mod = bool(mod_dir) and ((mod_dir / "code.ips").is_file() or any(
        p.suffix.lower() in (".cro", ".crs", ".crr") for p in (mod_dir / "romfs").rglob("*")))
    game = args.cache / "games" / (f"{title_id:016X}-{digest[:12]}" + (f"-mod{mod_digest}" if code_mod else "") +
                                   (f"-asm{cheat_digest}" if cheat_writes else ""))
    module = game / ("module.dll" if os.name == "nt" else "module.so")

    hints = game / "hints.txt"  # function starts 3dsrecomp cannot find, one address a line
    modules_dir = game / "modules"  # modules the game unpacked itself, saved by the host when it loaded them

    def build():
        recomp = shutil.which(args.recomp) or args.recomp
        if not Path(recomp).is_file():
            sys.exit("3dsrecomp not found: give --3dsrecomp or CTRN_3DSRECOMP (https://github.com/fearkov/3dsrecomp)")
        print(f"[ctrn] {title_id:016X}: translating the code", flush=True)
        start = time.monotonic()
        # the files are named after their first function: one a hint moves would stay beside its successor and
        # link twice; build_module keeps the objects by content, so nothing compiled is lost
        # what the last generation wrote: a file it would not write again (a cut moved, an override renamed) would
        # be compiled with the new ones
        for old in [*(game / "recomp").glob("code_*.c"), *(game / "recomp" / "hand").glob("*.c")]:
            old.unlink()
        overrides = ["--overrides", str(natives_dir)] if (natives_dir / "gfx_natives.c").is_file() else []
        # modules the game loaded that its RomFS has not as files (saved by the host when it met them)
        modules = ["--modules", str(modules_dir)] if any(modules_dir.glob("*.cro")) else []
        mod_args = []
        if (code_mod or cheat_writes) and (game / "mod-code.bin").is_file():
            mod_args += ["--code", str(game / "mod-code.bin")]
        if code_mod and (mod_dir / "romfs").is_dir():
            mod_args += ["--romfs-dir", str(mod_dir / "romfs")]
        subprocess.run([recomp, "generate", str(rom), str(game / "recomp"), "--hints", str(hints), *overrides, *modules,
                        *mod_args], check=True)
        translated = time.monotonic()
        print(f"[ctrn] translated in {translated - start:.0f} s; compiling it (a few minutes the first time)", flush=True)
        subprocess.run([sys.executable, str(HERE / "build_module.py"), str(game / "recomp"), str(module),
                        "--compiler", args.compiler], check=True)
        print(f"[ctrn] compiled in {time.monotonic() - translated:.0f} s", flush=True)

    exheader, code = read_code(rom)
    text_start, text_pages = struct.unpack_from("<II", exheader, 0x10)
    # a variant of the game (a mod's code, cheats patching it) starts from what the game's own cache learned
    # (function starts, unpacked modules): the same code but for a few words, no learning all over again
    base_game = args.cache / "games" / f"{title_id:016X}-{digest[:12]}"
    if game != base_game and not game.exists() and base_game.is_dir():
        game.mkdir(parents=True)
        if (base_game / "hints.txt").is_file():
            shutil.copy2(base_game / "hints.txt", game / "hints.txt")
        if (base_game / "modules").is_dir():
            shutil.copytree(base_game / "modules", game / "modules")
    game.mkdir(parents=True, exist_ok=True)
    # the mod's code: its IPS patch over the game's (decompressed, as Luma patches it); its RomFS: the game's with
    # the mod's files in place, built once into the cache
    if mod_dir and (mod_dir / "code.ips").is_file():
        code = romfs.apply_ips(code, (mod_dir / "code.ips").read_bytes())
    if cheat_writes:
        code = cheats.patch(code, text_start, cheat_writes)
    if (mod_dir and (mod_dir / "code.ips").is_file()) or cheat_writes:
        (game / "mod-code.bin").write_bytes(code)
    mod_romfs = game / f"romfs-{mod_digest}.bin"
    if mod_dir and (mod_dir / "romfs").is_dir() and not mod_romfs.is_file():
        start = time.monotonic()
        at, _ = romfs.level3(rom)
        tree = romfs.read_tree(str(rom), at)
        count = romfs.overlay(tree, str(mod_dir / "romfs"))
        romfs.write(tree, str(mod_romfs))
        print(f"[ctrn] the mod's RomFS: {count} files over the game's, built in {time.monotonic() - start:.0f} s",
              flush=True)
    hints.touch()  # 3dsrecomp refuses a missing --hints file, and a game may have none
    # what the code's own patterns say 3dsrecomp cannot find; a cache made before a pattern was known gets it too
    known = {int(word, 16) for word in hints.read_text().split()} if hints.is_file() else set()
    constructors, tables = constructor_hints(code, text_start), jump_table_hints(code, text_start)
    new = [a for a in dict.fromkeys(constructors + tables) if a not in known]
    if new:
        print(f"[ctrn] {len(constructors)} C++ constructors, {len(tables)} jump table targets in the code: "
              f"{len(new)} new hints", flush=True)
        with hints.open("a") as f:
            f.write("".join(f"{a:08X}\n" for a in new))
    # the graphics functions done natively: a change of them (a new one known) makes the module again
    natives_dir = game / "natives"
    found = gfx_natives(code, text_start)
    cursor = command_cursor(found, code, text_start)
    if cursor is not None:  # where nngx keeps the next command's place (the host's natives write marks there)
        os.environ["CTRN_COMMAND_CURSOR"] = f"{cursor:08X}"
    # only the natives checked on this game (a game works first): CTRN_ALL_NATIVES=1 takes them all, to check them
    checked = CHECKED_NATIVES.get(title_id, set())
    if not os.environ.get("CTRN_ALL_NATIVES"):
        left = sorted({n for _, n, _ in found} - set(checked))
        found = [f for f in found if f[1] in checked]
        if left:
            print(f"[ctrn] graphics natives not checked on this game, left out: {left} (tools/ab_natives.py)",
                  flush=True)
    natives_changed = False
    source = natives_source(found, cursor) if found else '#include "overrides.h"\n'
    # CTRN_FORCE_ARGS=addr:reg,... (hex): a test reaching a place a player would take hours to (a stage, a level):
    # that function entered with that register set to the host's CTRN_FORCE_<ADDR> (hex, read when it runs: one
    # module for every value; unset, the game's own). A diagnosis: it makes the module again
    for text in filter(None, os.environ.get("CTRN_FORCE_ARGS", "").split(",")):
        address, reg = (int(x, 16) for x in text.split(":")[:2])
        source += (f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                   f"    const uint32_t r12 = ctx->r[12];\n"
                   f"    ctx->r[12] = ctx->r[{reg}];\n"
                   f"    SVC(0x{address:08X}u, 0xF2u);\n"
                   f"    ctx->r[{reg}] = ctx->r[12];\n"
                   f"    ctx->r[12] = r12;\n"
                   f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                   f"}}\n")
    # CTRN_TRACE_FUNCS=addr,addr (hex): those functions run as always, then the host is told their first three
    # arguments, result and return address (svc 0xF1; it prints the calls that returned 0, or all with
    # CTRN_TRACE_ALL=1) - a diagnosis that makes the module again, so best with a cache of its own
    for text in filter(None, os.environ.get("CTRN_TRACE_FUNCS", "").split(",")):
        address = int(text, 16)
        source += (f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                   f"    const uint32_t a0 = ctx->r[0], a1 = ctx->r[1], a2 = ctx->r[2], lr = ctx->r[14];\n"
                   f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                   f"    const uint32_t back = ctx->r[15] | (ctx->thumb ? 1u : 0u), result = ctx->r[0], r1 = ctx->r[1];\n"
                   f"    const uint32_t r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];\n"
                   f"    ctx->r[0] = a0, ctx->r[1] = a1, ctx->r[2] = a2, ctx->r[3] = result, ctx->r[4] = lr;\n"
                   f"    SVC(0x{address:08X}u, 0xF1u);\n"
                   f"    ctx->r[0] = result, ctx->r[1] = r1, ctx->r[2] = r2, ctx->r[3] = r3, ctx->r[4] = r4;\n"
                   f"    RETURN_TO(back);\n"
                   f"}}\n")
    natives_dir.mkdir(exist_ok=True)
    old = (natives_dir / "gfx_natives.c").read_text() if (natives_dir / "gfx_natives.c").is_file() else ""
    if old != source:
        (natives_dir / "gfx_natives.c").write_text(source)
        natives_changed = True
    if found:
        print(f"[ctrn] graphics natives: {', '.join(f'{a:08X} ({n})' for a, n, _ in found)}", flush=True)
    if new or natives_changed or not module.is_file():
        build()
    # the GPU programs, translated ahead (picanative loads them at start); again when picanative changes
    shaders, translator = game / "shaders", args.host.parent / ("pn_shaders.exe" if os.name == "nt" else "pn_shaders")
    if not shaders.is_dir():
        start = time.monotonic()
        extract_shaders(rom, code, shaders)
        print(f"[ctrn] shaders found in {time.monotonic() - start:.0f} s", flush=True)
    translated = game / "shaders.pnvs"
    if translator.is_file() and (not translated.is_file() or translated.stat().st_mtime < translator.stat().st_mtime):
        subprocess.run([str(translator), str(translated), str(shaders)], check=True)
    if args.build_only:
        print(module)
        return
    if not args.host.is_file():
        sys.exit(f"{args.host}: build ctrnative first (README)")
    env = dict(os.environ)
    env.setdefault("CTRN_USER_DIR", str(args.cache / "user"))
    # the system's shared font: a dump of the console's (<cache>/sysdata/shared_font.bin), else a stand-in made once
    dump, stand_in = args.cache / "sysdata" / "shared_font.bin", args.cache / "shared_font_stand_in.bin"
    if not dump.is_file() and not stand_in.is_file():
        subprocess.run([sys.executable, str(HERE / "shared_font.py"), str(stand_in)], check=True)
    env.setdefault("CTRN_SHARED_FONT", str(dump if dump.is_file() else stand_in))
    env["CTRN_MODULES_DIR"] = str(modules_dir)
    # cheats, as Luma3DS has them: <cheats>/<title ID>.txt (Action Replay codes under [names]), <cheats> being
    # CTRN_CHEATS_DIR or %LOCALAPPDATA%/ctrnative/cheats; the codes on are the names in <title ID>.on, one a line
    # (or CTRN_CHEATS_ON=name;name); none by default
    if "CTRN_CHEATS" not in env and cheats_file:
        env["CTRN_CHEATS"] = str(cheats_file)
        if "CTRN_CHEATS_ON" not in env and cheats_on:
            env["CTRN_CHEATS_ON"] = ";".join(cheats_on)
    if mod_dir and mod_romfs.is_file():
        env["CTRN_ROMFS"] = str(mod_romfs)  # the host serves it in place of the ROM's
    # a run that reaches code 3dsrecomp did not find (a call through a computed address) stops there:
    # that address becomes a hint, the module is rebuilt (only what changed) and the game starts again
    for attempt in range(80):  # each new hint or module costs a rebuild and a fresh start
        # the code patched (a mod's code.ips, cheats that patch it): the host runs that code, with the ROM's
        # exheader and RomFS (CTRN_ROM)
        game_args = [str(rom)]
        if (code_mod or cheat_writes) and (game / "mod-code.bin").is_file():
            (game / "exheader.bin").write_bytes(exheader)
            env["CTRN_ROM"] = str(rom)
            game_args = [str(game / "mod-code.bin"), str(game / "exheader.bin")]
        run = subprocess.Popen([str(args.host), str(module), *game_args], env=env, stdout=subprocess.PIPE,
                               text=True, errors="replace")
        missing = None
        saved = None
        for line in run.stdout:
            print(line, end="", flush=True)
            if m := re.search(r"stopped after \d+ frames: no recompiled code at ([0-9a-f]{8})", line):
                missing = int(m.group(1), 16)
            if m := re.search(r"stopped after \d+ frames: module (\S+) saved for translation", line):
                saved = m.group(1)
        code_returned = run.wait()
        if saved is not None:
            print(f"[ctrn] module {saved}: translating it with the others, rebuilding", flush=True)
            build()
            continue
        if missing is None or not text_start <= missing < text_start + text_pages * 0x1000:
            sys.exit(code_returned)
        if missing in [int(word, 16) for word in hints.read_text().split()]:
            sys.exit(f"[ctrn] {missing:08X} is a hint already and still not translated: 3dsrecomp needs a fix")
        print(f"[ctrn] {missing:08X} was not translated: added to the hints, rebuilding", flush=True)
        with hints.open("a") as f:
            f.write(f"{missing:08X}\n")
        build()
    sys.exit(1)


if __name__ == "__main__":
    main()

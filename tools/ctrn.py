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
      0xE5803008, 0xE1A0C102, 0xE1A03A02, 0xE2432601, 0xE1822008, 0xE0801201, 0xE580200C, 0xE3A07000]),
    (3, "nw::lyt (NSMB2's drawer): a material set up",
     [0xE92D4FF0, 0xE1A04000, 0xE24DD024, 0xE1A05001, 0xE3A09001, 0xE5D1004D, 0xE3100002, 0x0A00000A,
      0xE5D406A2, 0xE3500000, 0x13500001, 0x0A00007A, 0xE5D41024, 0xE1A00004, 0xE3510000, 0x0A000076]),
    (4, "nw::lyt (NSMB2's drawer): a material's textures set",
     [0xE92D4FF7, 0xE1B07001, 0xE59FA194, 0x0A00005A, 0xE59D0000, 0xE3A09001, 0xE3A08000, 0xE1A05008,
      0xE5D00024, 0xE1A04008, 0xE3500000, 0xE5970030, 0x13A09000, 0xE2006003, 0xE3560003, 0xE3A00003]),
    (5, "nw::font (NSMB2's drawer): pending matrices and colors sent",
     [0xE92D41F0, 0xE1A04000, 0xE2800F8E, 0xE5D41025, 0xE59F3058, 0xE590C00C, 0xE3A07000, 0xE1A02101,
      0xE0801201, 0xE1A05A02, 0xE1A06102, 0xE2452601, 0xE1822003, 0xE2865010, 0xE580200C, 0xE59F6030]),
    (6, "nw::font (NSMB2's drawer): a text's matrix sent",
     [0xE59F3058, 0xE92D0030, 0xE59F4054, 0xE3A0518E, 0xE5932000, 0xE59FC04C, 0xE2821004, 0xE4825008,
      0xE5814000, 0xE5832000, 0xE5904000, 0xE2821008, 0xE8821010, 0xE2802004, 0xE2800024, 0xEC920A08]),
    (7, "nw::font (NSMB2's drawer): a text's colors sent",
     [0xE59FC054, 0xE92D03F0, 0xE3520000, 0xE59C6000, 0xE59F5048, 0xE2862004, 0x0A000007, 0xE5861000,
      0xE9820021, 0xE285800A, 0xE282900C, 0xE5828000, 0xE58C9000, 0xE8BD03F0, 0xE12FFF1E, 0xE59F8020]),
    (8, "nw::font (NSMB2's drawer): a text's list built (then translated once)",
     [0xE92D4FF7, 0xE24DD058, 0xE1A05000, 0xE3A00000, 0xE3A04001, 0xE59D105C, 0xE5918044, 0xE1D8B0B4,
      0xE1D810B6, 0xE5880018, 0xE5880020, 0xE15B0001, 0x81A0B001, 0xE35B0000, 0xE5C84008, 0x0A0001C8]),
    (9, "nw::lyt (NSMB2's drawer): drawing begun",
     [0xE92D4070, 0xE1A04000, 0xE3A00000, 0xE5840698, 0xE1A05001, 0xE584069C, 0xE3A01006, 0xE5840694,
      0xE5C416A2, 0xE3A02001, 0xE5C426A0, 0xE1A02000, 0xE1A01000, 0xE1A00004, 0xEBFF9C9A, 0xE1A01005]),
    (10, "nw::lyt (NSMB2's drawer): drawing ended",
     [0xE92D4070, 0xE1A05001, 0xE5D01024, 0xE1A04000, 0xE3510000, 0x1BFF9E1E, 0xE1A01005, 0xE1A00004,
      0xE8BD4070, 0xE1A00000, 0xE3110001, 0x1A000001, 0xE2800014, 0xEA0E5627, 0xE12FFF1E, 0xE92D4070]),
    (11, "nw::lyt (NSMB2's drawer): drawing begun with a projection",
     [0xE92D4070, 0xE1A04000, 0xE5900000, 0xE1A05001, 0xE3A01000, 0xE590200C, 0xE1A00004, 0xE12FFF32,
      0xE2851004, 0xE1A00004, 0xE8BD4070, 0xE1A00000, 0xE92D4010, 0xE1A04000, 0xE2800034, 0xEBFDCB3D]),
    (12, "nngx: a block of commands added, copied or not (SDK 2012)",
     [0xE92D41F0, 0xE1A06001, 0xE1A08000, 0xE59F50C4, 0xE59F70C4, 0xE3520000, 0xE595409C, 0xE5970000,
      0xE5941004, 0xE0401001, 0xE584100C, 0x0A000007, 0xE1A02006, 0xE1A01008, 0xEBEF5172, 0xE5970000]),
    (13, "nngx: a block {address, size, copied} added (SDK 2012)",
     [0xE5D0100C, 0xE3510000, 0x13A02001, 0x03A02000, 0xE1C000D0, 0xE1A00000, 0xE92D41F0, 0xE1A06001,
      0xE1A08000, 0xE59F50C4, 0xE59F70C4, 0xE3520000, 0xE595409C, 0xE5970000, 0xE5941004, 0xE0401001]),
    (14, "NSMB2: a viewport's commands written",
     [0xE92D01F0, 0xE1A03000, 0xE5900008, 0xED9F1A5F, 0xEE000A10, 0xEEB80A40, 0xEE200A01, 0xEE102A10,
      0xE3D2C102, 0x0A000006, 0xE1A0C082, 0xE1A0CC2C, 0xE24CC040, 0xE35C0000, 0xB1A02FA2, 0xB1A0CB82]),
    (15, "NSMB2: a scissor's commands written",
     [0xE92D0030, 0xE2802004, 0xE280300C, 0xE8921004, 0xE8930018, 0xE5D05000, 0xE0822003, 0xE08C3004,
      0xE3550000, 0xE2434001, 0xE242C001, 0xE59F2078, 0x13A03003, 0x03A03000, 0xE4813004, 0xE4812004]),
]
# natives the host sees after the game's own function ran, with its arguments (what it made, translated once)
AFTER_NATIVES = {8}


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


def natives_source(found):
    """the overrides that hand each found function to the host (svc 0xF0, r12: its number); the host says no by
    setting r12 to 0, and the game's own function runs"""
    out = ['#include "overrides.h"\n']
    for address, number, what in found:
        if number in AFTER_NATIVES:
            out.append(f"/* {what} */\n"
                       f"RECOMP_OVERRIDE(0x{address:08X}) {{\n"
                       f"    const uint32_t a0 = ctx->r[0], a1 = ctx->r[1], a2 = ctx->r[2], a3 = ctx->r[3];\n"
                       f"    CALL(RECOMP_ORIGINAL(0x{address:08X}));\n"
                       f"    const uint32_t back = ctx->r[15] | (ctx->thumb ? 1u : 0u), result = ctx->r[0];\n"
                       f"    ctx->r[0] = a0, ctx->r[1] = a1, ctx->r[2] = a2, ctx->r[3] = a3;\n"
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
    game = args.cache / "games" / f"{title_id:016X}-{digest[:12]}"
    module = game / ("module.dll" if os.name == "nt" else "module.so")

    hints = game / "hints.txt"  # function starts 3dsrecomp cannot find, one address a line

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
        subprocess.run([recomp, "generate", str(rom), str(game / "recomp"), "--hints", str(hints), *overrides], check=True)
        translated = time.monotonic()
        print(f"[ctrn] translated in {translated - start:.0f} s; compiling it (a few minutes the first time)", flush=True)
        subprocess.run([sys.executable, str(HERE / "build_module.py"), str(game / "recomp"), str(module),
                        "--compiler", args.compiler], check=True)
        print(f"[ctrn] compiled in {time.monotonic() - translated:.0f} s", flush=True)

    exheader, code = read_code(rom)
    text_start, text_pages = struct.unpack_from("<II", exheader, 0x10)
    game.mkdir(parents=True, exist_ok=True)
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
    natives_changed = False
    if found:
        source = natives_source(found)
        natives_dir.mkdir(exist_ok=True)
        old = (natives_dir / "gfx_natives.c").read_text() if (natives_dir / "gfx_natives.c").is_file() else ""
        if old != source:
            (natives_dir / "gfx_natives.c").write_text(source)
            natives_changed = True
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
    # a run that reaches code 3dsrecomp did not find (a call through a computed address) stops there:
    # that address becomes a hint, the module is rebuilt (only what changed) and the game starts again
    for attempt in range(20):
        run = subprocess.Popen([str(args.host), str(module), str(rom)], env=env, stdout=subprocess.PIPE,
                               text=True, errors="replace")
        missing = None
        for line in run.stdout:
            print(line, end="", flush=True)
            if m := re.search(r"stopped after \d+ frames: no recompiled code at ([0-9a-f]{8})", line):
                missing = int(m.group(1), 16)
        code_returned = run.wait()
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

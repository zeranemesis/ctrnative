"""A stand-in for the console's shared font (APT GetSharedFont), for when no dump of it is there.
usage: python tools/shared_font.py <out.bin>
Its characters come from a font for Japanese the host has (Windows: Yu Gothic, Meiryo, MS Gothic), drawn into the
console's font block: a status word, then a CFNU font (FINF, 4-bit glyph sheets in TGLP, CWDH widths, CMAP maps),
its pointers laid out for address 0 (the host moves them to where it maps the block). The layout follows Zakuro's
(fearkov, MIT) services/shared_font.rs. A dump of the console's own font is better: the host takes it first."""
import os, struct, sys
from pathlib import Path

FONT_OFFSET = 0x80     # the font after the status word and the rest of the block's header
SECTION_BODY = 8       # section pointers point past a section's magic and size
CELL_W = CELL_H = 24
STRIDE = CELL_W + 1    # cells a pixel apart on the sheet
SHEET_W = SHEET_H = 256
COLUMNS, ROWS = SHEET_W // STRIDE, SHEET_H // STRIDE
SHEET_FORMAT_A4 = 11   # 4-bit alpha, as the console's sheets
SHEET_ALIGN = 0x80
BLOCK_LIMIT = 0x332000 # the console's font block: no title maps more
DIRECT_RUN = 16        # characters in a row that get a map of their own
EM, BASELINE = 22.0, 20


def characters():
    """JIS X 0208's characters (with the NEC row of circled numbers), the Latin of European titles, full and half
    width forms: near enough what the console's font has"""
    found = set()
    for high in range(0xA1, 0xFF):
        for low in range(0xA1, 0xFF):
            try:
                found.update(bytes([high, low]).decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    for low in range(0x40, 0xFD):  # cp932's row 13: circled numbers and the like
        try:
            found.update(bytes([0x87, low]).decode("cp932"))
        except UnicodeDecodeError:
            pass
    ranges = [(0x20, 0x7E), (0xA0, 0x17F), (0x2010, 0x2027), (0x2030, 0x203B), (0x20AC, 0x20AC), (0x2122, 0x2122),
              (0x2190, 0x2199), (0x2460, 0x2473), (0x3000, 0x30FF), (0xFF01, 0xFF9F), (0xFFE0, 0xFFE6)]
    for first, last in ranges:
        found.update(map(chr, range(first, last + 1)))
    return sorted(c for c in found if len(c) == 1 and ord(c) <= 0xFFFF)


def host_font():
    windows = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"
    for name in ("YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"):
        if (windows / name).is_file():
            return windows / name
    sys.exit("no font for Japanese on this host (Yu Gothic, Meiryo or MS Gothic)")


def glyphs():
    """each character the font has: (code, 4-bit alpha of its cell, left, drawn width, advance)"""
    from PIL import Image, ImageDraw, ImageFont
    path = host_font()
    font = ImageFont.truetype(str(path), size=int(EM), index=0)
    out = []
    for c in characters():
        if c != " " and not font.getmask(c).getbbox():  # nothing drawn: the font lacks it
            continue
        advance = max(0, min(255, round(font.getlength(c))))
        image = Image.new("L", (CELL_W, CELL_H), 0)
        left, top, right, bottom = font.getbbox(c, anchor="ls")
        # drawn at the baseline, its left edge at the cell's, as the console's renderer adds `left` back
        ImageDraw.Draw(image).text((-left, BASELINE), c, font=font, fill=255, anchor="ls")
        alpha = [min(15, round(p * 15 / 255)) for p in image.tobytes()]
        out.append((ord(c), alpha, max(-128, min(127, left)), max(0, min(CELL_W, right - left)), advance))
    if not any(code == 0x3042 for code, *_ in out):
        sys.exit(f"{path} has no Japanese")
    return out, max(1, round(font.getlength("\u3042")))


def morton(x, y, width):
    """a texel's index in a PICA-tiled image: 8x8 tiles in raster order, a Z-order curve inside each"""
    inside = 0
    for bit in range(3):
        inside |= ((x % 8) >> bit & 1) << (2 * bit) | ((y % 8) >> bit & 1) << (2 * bit + 1)
    return ((y // 8) * (width // 8) + x // 8) * 64 + inside


def maps(codes):
    """runs of characters in a row each a direct map, the rest in one scanned list after them"""
    result, rest, start = [], [], 0
    while start < len(codes):
        end = start + 1
        while end < len(codes) and codes[end] == codes[end - 1] + 1:
            end += 1
        if end - start >= DIRECT_RUN:
            result.append(("direct", codes[start], codes[end - 1], start))
        else:
            rest += [(codes[i], i) for i in range(start, end)]
        start = end
    if rest or not result:
        result.append(("scan", rest))
    return result


def align(value, to):
    return (value + to - 1) // to * to


def build():
    faces, default_advance = glyphs()
    count = len(faces)
    per_sheet = COLUMNS * ROWS
    sheets = max(1, -(-count // per_sheet))
    sheet_size = SHEET_W * SHEET_H // 2
    all_maps = maps([code for code, *_ in faces])
    payload = lambda m: 2 if m[0] == "direct" else 2 + len(m[1]) * 4
    finf = FONT_OFFSET + 0x14
    tglp = finf + 0x20
    sheet_at = align(tglp + 0x20, SHEET_ALIGN)
    cwdh = sheet_at + sheets * sheet_size
    cwdh_size = 0x10 + align(count * 3, 4)
    cmaps, end = [], cwdh + cwdh_size
    for m in all_maps:
        cmaps.append(end)
        end += 0x14 + align(payload(m), 4)
    total = end
    if total > BLOCK_LIMIT:
        print(f"warning: the stand-in takes {total:#x} bytes, more than a title may map", file=sys.stderr)
    out = bytearray(total)
    p32 = lambda at, v: struct.pack_into("<I", out, at, v & 0xFFFFFFFF)
    p16 = lambda at, v: struct.pack_into("<H", out, at, v & 0xFFFF)
    # the status word (loaded), the region's font (the standard one), the font's size
    p32(0, 2), p32(4, 1), p32(8, total - FONT_OFFSET)
    # CFNU: the font as the console keeps it, pointers already absolute
    out[FONT_OFFSET:FONT_OFFSET + 4] = b"CFNU"
    p16(FONT_OFFSET + 4, 0xFEFF), p16(FONT_OFFSET + 6, 0x14)
    p32(FONT_OFFSET + 8, 0x03000000), p32(FONT_OFFSET + 12, total - FONT_OFFSET), p32(FONT_OFFSET + 16, 3 + len(all_maps))
    unknown = next((i for i, (code, *_) in enumerate(faces) if code == ord("?")), 0)
    out[finf:finf + 4] = b"FINF"
    p32(finf + 4, 0x20)
    out[finf + 8], out[finf + 9] = 1, CELL_H + 1  # glyph sheets; line feed
    p16(finf + 10, unknown)
    out[finf + 12], out[finf + 13], out[finf + 14], out[finf + 15] = 0, CELL_W, default_advance, 1  # UTF-16
    p32(finf + 16, tglp + SECTION_BODY), p32(finf + 20, cwdh + SECTION_BODY), p32(finf + 24, cmaps[0] + SECTION_BODY)
    out[finf + 28], out[finf + 29], out[finf + 30] = CELL_H, CELL_W, BASELINE
    out[tglp:tglp + 4] = b"TGLP"
    p32(tglp + 4, cwdh - tglp)
    out[tglp + 8], out[tglp + 9], out[tglp + 10] = CELL_W, CELL_H, BASELINE
    out[tglp + 11] = max([CELL_W] + [f[4] for f in faces])
    p32(tglp + 12, sheet_size), p16(tglp + 16, sheets), p16(tglp + 18, SHEET_FORMAT_A4)
    p16(tglp + 20, COLUMNS), p16(tglp + 22, ROWS), p16(tglp + 24, SHEET_W), p16(tglp + 26, SHEET_H)
    p32(tglp + 28, sheet_at)
    for index, (code, alpha, *_rest) in enumerate(faces):
        sheet, cell = divmod(index, per_sheet)
        cx, cy = (cell % COLUMNS) * STRIDE + 1, (cell // COLUMNS) * STRIDE + 1
        for y in range(CELL_H):
            for x in range(CELL_W):
                a = alpha[y * CELL_W + x]
                if a:
                    texel = morton(cx + x, cy + y, SHEET_W)
                    out[sheet_at + sheet * sheet_size + texel // 2] |= a << (4 * (texel & 1))  # low nibble first
    out[cwdh:cwdh + 4] = b"CWDH"
    p32(cwdh + 4, cwdh_size), p16(cwdh + 8, 0), p16(cwdh + 10, max(0, count - 1)), p32(cwdh + 12, 0)
    for index, (_, _, left, width, advance) in enumerate(faces):
        at = cwdh + 0x10 + index * 3
        out[at], out[at + 1], out[at + 2] = left & 0xFF, width, advance
    for i, (m, at) in enumerate(zip(all_maps, cmaps)):
        out[at:at + 4] = b"CMAP"
        p32(at + 4, 0x14 + align(payload(m), 4))
        p32(at + 16, cmaps[i + 1] + SECTION_BODY if i + 1 < len(cmaps) else 0)
        if m[0] == "direct":
            p16(at + 8, m[1]), p16(at + 10, m[2]), p16(at + 12, 0), p16(at + 20, m[3])
        else:
            pairs = m[1]
            p16(at + 8, pairs[0][0] if pairs else 0), p16(at + 10, pairs[-1][0] if pairs else 0), p16(at + 12, 2)
            p16(at + 20, len(pairs))
            for n, (code, index) in enumerate(pairs):
                p16(at + 22 + n * 4, code), p16(at + 24 + n * 4, index)
    return bytes(out), count


def glyph_of(font, code):
    """the glyph a character finds, the way a title's font library does: down the maps from FINF's first"""
    body = struct.unpack_from("<I", font, FONT_OFFSET + 0x14 + 24)[0]
    while body:
        at = body - SECTION_BODY
        begin, end, method = struct.unpack_from("<HHH", font, at + 8)
        if begin <= code <= end:
            if method == 0:
                return struct.unpack_from("<H", font, at + 20)[0] + code - begin
            pairs = dict(struct.unpack_from("<HH", font, at + 22 + n * 4) for n in range(struct.unpack_from("<H", font, at + 20)[0]))
            if code in pairs:
                return pairs[code]
        body = struct.unpack_from("<I", font, at + 16)[0]
    return None


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    font, count = build()
    # self-check: the sections where FINF says, characters that find their glyphs
    finf = FONT_OFFSET + 0x14
    for offset, magic in ((16, b"TGLP"), (20, b"CWDH"), (24, b"CMAP")):
        at = struct.unpack_from("<I", font, finf + offset)[0] - SECTION_BODY
        assert font[at:at + 4] == magic, magic
    for c in "A?あア漢字é！":
        assert glyph_of(font, ord(c)) is not None, c
    assert glyph_of(font, 0xFFFF) is None
    Path(sys.argv[1]).write_bytes(font)
    print(f"{sys.argv[1]}: {count} characters, {len(font):#x} bytes")

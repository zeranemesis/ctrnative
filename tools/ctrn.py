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
import argparse, hashlib, os, re, shutil, struct, subprocess, sys
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


def constructor_hints(code, base):
    """the C++ constructors the game runs at start: 3dsrecomp finds no call or pointer to them"""
    at = code.find(CALL_LOOP)
    if at < 0 or at % 4:
        return []
    bounds = {}
    for p in range(max(0, at - 0x40), at, 4):
        ldr, add = struct.unpack_from("<II", code, p) if p + 8 <= len(code) else (0, 0)
        reg = (ldr >> 12) & 0xF
        if ldr & 0xFFFF0000 == 0xE59F0000 and add == 0xE08F0000 | reg << 16 | reg << 12 | reg:
            literal = struct.unpack_from("<i", code, p + 8 + (ldr & 0xFFF))[0]
            bounds[reg] = base + p + 4 + 8 + literal  # the add's pc reads 8 ahead
    if 4 not in bounds or 5 not in bounds or not 0 < bounds[5] - bounds[4] < len(code):
        return []
    start, end = bounds[4], bounds[5]
    return [start + i * 4 + struct.unpack_from("<i", code, start - base + i * 4)[0] for i in range((end - start) // 4)]


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
        subprocess.run([recomp, "generate", str(rom), str(game / "recomp"), "--hints", str(hints)], check=True)
        print("[ctrn] compiling it (a few minutes the first time)", flush=True)
        subprocess.run([sys.executable, str(HERE / "build_module.py"), str(game / "recomp"), str(module),
                        "--compiler", args.compiler], check=True)

    exheader, code = read_code(rom)
    text_start, text_pages = struct.unpack_from("<II", exheader, 0x10)
    if not module.is_file():
        game.mkdir(parents=True, exist_ok=True)
        if not hints.is_file():
            found = constructor_hints(code, text_start)
            print(f"[ctrn] {len(found)} C++ constructors found in the code", flush=True)
            hints.write_text("".join(f"{a:08X}\n" for a in found))
        build()
    if args.build_only:
        print(module)
        return
    if not args.host.is_file():
        sys.exit(f"{args.host}: build ctrnative first (README)")
    env = dict(os.environ)
    env.setdefault("CTRN_USER_DIR", str(args.cache / "user"))
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
        print(f"[ctrn] {missing:08X} was not translated: added to the hints, rebuilding", flush=True)
        with hints.open("a") as f:
            f.write(f"{missing:08X}\n")
        build()
    sys.exit(1)


if __name__ == "__main__":
    main()

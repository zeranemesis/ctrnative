"""Checks the graphics natives against the game's own code: the same run twice in fixed step (CTRN_FIXED_STEP, a
run that replays the same), once with the natives and once without (all, or those given), captures compared byte for
byte. Any difference is a native that does not do what the game's function did.

    python tools/ab_natives.py game.3ds --frames 2400 [--off 3,17] [--cache DIR] [--every 150]

Needs what tools/ctrn.py needs (3dsrecomp, clang); prints the captures that differ, and exits 1 if any does.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(rom, cache, out, frames, every, off, extra_env):
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CTRN_HEADLESS="1", CTRN_FIXED_STEP="1", CTRN_FRAMES=str(frames),
               CTRN_SHOT_EVERY=str(every), CTRN_SHOT_DIR=str(out), CTRN_USER_DIR=str(out / "user"), **extra_env)
    env.pop("CTRN_NO_NATIVES", None)
    if off:
        env["CTRN_NO_NATIVES"] = off
    with open(out / "run.log", "w", encoding="utf-8", errors="replace") as log:
        subprocess.run([sys.executable, str(HERE / "ctrn.py"), str(rom), "--cache", str(cache)], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=False)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("rom", type=Path)
    p.add_argument("--frames", type=int, default=2400)
    p.add_argument("--every", type=int, default=150, help="a capture every n frames")
    p.add_argument("--off", default=",".join(str(n) for n in range(1, 64)),
                   help="the natives left to the game in the second run (default: all)")
    p.add_argument("--cache", type=Path, default=None)
    p.add_argument("--out", type=Path, default=None)
    p.add_argument("--press-a", action="store_true", help="CTRN_PRESS_A in both runs (the menus go on)")
    args = p.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="ctrn-ab-"))
    cache = args.cache or (Path(os.environ.get("LOCALAPPDATA", Path.home())) / "ctrnative")
    extra = {"CTRN_PRESS_A": "1"} if args.press_a else {}
    run(args.rom, cache, out / "with", args.frames, args.every, "", extra)
    run(args.rom, cache, out / "without", args.frames, args.every, args.off, extra)
    shots = sorted((out / "with").glob("*.ppm"))
    if not shots:
        sys.exit(f"no captures in {out / 'with'}: see its run.log")
    differ = 0
    for shot in shots:
        other = out / "without" / shot.name
        if not other.is_file():
            print(f"{shot.name}: missing without the natives")
            differ += 1
            continue
        a, b = shot.read_bytes(), other.read_bytes()
        if a != b:
            bytes_off = sum(1 for x, y in zip(a, b) if x != y)
            print(f"{shot.name}: {bytes_off} bytes differ")
            differ += 1
    print(f"{len(shots) - differ} of {len(shots)} captures identical byte for byte ({out})")
    sys.exit(1 if differ else 0)


if __name__ == "__main__":
    main()

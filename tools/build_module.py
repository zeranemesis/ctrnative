"""Compiles a game's 3dsrecomp output (generate <rom> <dir>) into the module ctrnative loads.
usage: python tools/build_module.py <recomp dir> <module.dll|.so> [--compiler clang] [--jobs n]
Objects are kept next to the module and rebuilt only when their C file or the flags change."""
import argparse, concurrent.futures, hashlib, json, os, subprocess, sys
from pathlib import Path

FLAGS = ["-O2", "-ffp-contract=off", "-fno-math-errno", "-w"]  # the game's float results, bit for bit


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recomp", type=Path)
    parser.add_argument("module", type=Path)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    recomp, module = args.recomp.resolve(), args.module.resolve()
    if not (recomp / "entries.c").is_file():
        sys.exit(f"{recomp}: no entries.c, run 3dsrecomp generate first")
    objects = module.parent / (module.stem + "-objects")
    objects.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(json.dumps([args.compiler, FLAGS, digest(recomp / "recomp.h")]).encode()).hexdigest()
    cache_path = objects / "cache.json"
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    known = cache.get("files", {}) if cache.get("key") == key else {}
    # hand/: functions written by hand that 3dsrecomp put in place of its own (overrides)
    sources = sorted(recomp.glob("*.c")) + sorted(recomp.glob("hand/*.c"))
    todo, files = [], {}
    for source in sources:
        obj, h = objects / (source.stem + ".o"), digest(source)
        if known.get(source.name) == h and obj.is_file():
            files[source.name] = h
        else:
            todo.append((source, obj, h))
    print(f"{len(files)} objects kept, {len(todo)} to compile", flush=True)

    def compile_one(item):
        source, obj, h = item
        log = obj.with_suffix(".log")
        with log.open("w", encoding="utf-8") as out:
            subprocess.run([args.compiler, *FLAGS, "-I", str(recomp), "-c", str(source), "-o", str(obj)],
                           stdout=out, stderr=subprocess.STDOUT, check=True)
        return source.name, h

    failed = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for job in concurrent.futures.as_completed([pool.submit(compile_one, t) for t in todo]):
            try:
                name, h = job.result()
                files[name] = h
                print("compiled", name, flush=True)
            except subprocess.CalledProcessError as error:
                failed.append(Path(error.cmd[-3]).name)
    cache_path.write_text(json.dumps({"key": key, "files": files}, indent=1))  # what compiled is kept
    if failed:
        sys.exit("failed (see the .log next to each object): " + " ".join(failed))
    rsp = objects / "objects.rsp"
    rsp.write_text("\n".join('"' + (objects / (s.stem + ".o")).as_posix() + '"' for s in sources))
    temporary = module.with_name("pending-" + module.name)
    subprocess.run([args.compiler, *FLAGS, "-shared", "-o", str(temporary), "@" + str(rsp)], check=True)
    os.replace(temporary, module)  # a module in use stays whole: the new one only lands complete
    print("built", module)


if __name__ == "__main__":
    main()

"""A game's RomFS (its level 3, the file system the game reads) as a tree, and a tree written back: what a mod's
romfs folder (Luma3DS's luma/titles/<id>/romfs) needs, its files in place of the game's or added to them (the
console's LayeredFS). The data is copied from the ROM as it is written, never all in memory (Smash's is 1.4 GB)."""
import os
import struct

INVALID = 0xFFFFFFFF


class Dir:
    def __init__(self, name):
        self.name, self.dirs, self.files = name, {}, {}  # files: name -> (path, offset, size) of the bytes


def level3(rom):
    """(where the RomFS's level 3 starts in the ROM, its size) of a decrypted .3ds"""
    with open(rom, "rb") as f:
        header, ncch = f.read(0x200), 0
        if header[0x100:0x104] == b"NCSD":
            ncch = struct.unpack_from("<I", header, 0x120)[0] * 0x200
            f.seek(ncch)
            header = f.read(0x200)
        ivfc = ncch + struct.unpack_from("<I", header, 0x1B0)[0] * 0x200
        size = struct.unpack_from("<I", header, 0x1B4)[0] * 0x200
        f.seek(ivfc)
        h = f.read(0x60)
        if h[:4] != b"IVFC":
            raise ValueError("no RomFS (an encrypted ROM?)")
        master, log2 = struct.unpack_from("<I", h, 0x08)[0], struct.unpack_from("<I", h, 0x4C)[0]
        start = (0x60 + master + (1 << log2) - 1) & ~((1 << log2) - 1)
        return ivfc + start, size - start


def read_tree(path, at):
    """the tree of the level 3 at offset at of the file at path"""
    with open(path, "rb") as f:
        f.seek(at)
        table = struct.unpack("<10I", f.read(0x28))
        f.seek(at + table[3])
        dirs = f.read(table[4])
        f.seek(at + table[7])
        files = f.read(table[8])
    data = at + table[9]

    def name(meta, offset, length):
        return meta[offset:offset + length].decode("utf-16-le")

    def walk(offset, node):
        child, first_file = struct.unpack_from("<II", dirs, offset + 8)
        while first_file != INVALID:
            sibling, data_offset, size, length = (struct.unpack_from("<I", files, first_file + 4)[0],
                                                  *struct.unpack_from("<QQ", files, first_file + 8),
                                                  struct.unpack_from("<I", files, first_file + 0x1C)[0])
            node.files[name(files, first_file + 0x20, length)] = (path, data + data_offset, size)
            first_file = sibling
        while child != INVALID:
            length = struct.unpack_from("<I", dirs, child + 0x14)[0]
            sub = Dir(name(dirs, child + 0x18, length))
            node.dirs[sub.name] = sub
            walk(child, sub)
            child = struct.unpack_from("<I", dirs, child + 4)[0]

    root = Dir("")
    walk(0, root)
    return root


def overlay(root, folder):
    """the files of a folder over the tree: in place of the game's at the same path, or added. Returns how many."""
    count = 0
    for top, _, names in os.walk(folder):
        node = root
        for part in os.path.relpath(top, folder).split(os.sep):
            if part in (".", ""):
                continue
            node = node.dirs.setdefault(part, Dir(part))
        for n in names:
            p = os.path.join(top, n)
            node.files[n] = (p, 0, os.path.getsize(p))
            count += 1
    return count


def _hash(parent, name):
    h = parent ^ 123456789
    encoded = name.encode("utf-16-le")
    for ch in struct.unpack(f"<{len(encoded) // 2}H", encoded):  # as the console: one step a UTF-16 unit
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= ch
    return h


def _buckets(n):
    n = max(n, 1)
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def write(root, out_path):
    """the tree as a level 3 into out_path"""
    # every directory and file a place in its table, in a walk's order (each directory's files, then its children)
    dirs, files = [], []

    def place(node, parent):
        node._at = sum(0x18 + ((len(d.name.encode("utf-16-le")) + 3) & ~3) for d, _ in dirs)
        dirs.append((node, parent))
        for n in sorted(node.files):
            files.append((n, node))
        for n in sorted(node.dirs):
            place(node.dirs[n], node)

    place(root, root)
    file_at, offset = {}, 0
    for n, parent in files:
        file_at[(id(parent), n)] = offset
        offset += 0x20 + ((len(n.encode("utf-16-le")) + 3) & ~3)
    dir_buckets, file_buckets = _buckets(len(dirs)), _buckets(len(files))
    dir_hash, file_hash = [INVALID] * dir_buckets, [INVALID] * file_buckets
    dir_meta, file_meta = bytearray(), bytearray()
    for node, parent in dirs:
        b = _hash(parent._at, node.name) % dir_buckets
        children = [node.dirs[n] for n in sorted(node.dirs)]
        own_files = sorted(node.files)
        siblings = [parent.dirs[n] for n in sorted(parent.dirs)] if node is not root else []
        nxt = INVALID
        if node in siblings and siblings.index(node) + 1 < len(siblings):
            nxt = siblings[siblings.index(node) + 1]._at
        encoded = node.name.encode("utf-16-le")
        dir_meta += struct.pack("<6I", parent._at, nxt, children[0]._at if children else INVALID,
                                file_at[(id(node), own_files[0])] if own_files else INVALID, dir_hash[b], len(encoded))
        dir_hash[b] = node._at
        dir_meta += encoded + b"\0" * (((len(encoded) + 3) & ~3) - len(encoded))
    data_offsets, data_size = {}, 0
    for n, parent in files:
        data_size = (data_size + 15) & ~15
        data_offsets[(id(parent), n)] = data_size
        data_size += parent.files[n][2]
    for n, parent in files:
        b = _hash(parent._at, n) % file_buckets
        names = sorted(parent.files)
        nxt = file_at[(id(parent), names[names.index(n) + 1])] if names.index(n) + 1 < len(names) else INVALID
        encoded = n.encode("utf-16-le")
        file_meta += struct.pack("<IIQQII", parent._at, nxt, data_offsets[(id(parent), n)], parent.files[n][2],
                                 file_hash[b], len(encoded))
        file_hash[b] = file_at[(id(parent), n)]
        file_meta += encoded + b"\0" * (((len(encoded) + 3) & ~3) - len(encoded))
    dir_hash_at = 0x28
    dir_meta_at = dir_hash_at + 4 * dir_buckets
    file_hash_at = dir_meta_at + len(dir_meta)
    file_meta_at = file_hash_at + 4 * file_buckets
    data_at = (file_meta_at + len(file_meta) + 15) & ~15
    with open(out_path + ".part", "wb") as out:
        out.write(struct.pack("<10I", 0x28, dir_hash_at, 4 * dir_buckets, dir_meta_at, len(dir_meta), file_hash_at,
                              4 * file_buckets, file_meta_at, len(file_meta), data_at))
        out.write(struct.pack(f"<{dir_buckets}I", *dir_hash) + dir_meta)
        out.write(struct.pack(f"<{file_buckets}I", *file_hash) + file_meta)
        out.write(b"\0" * (data_at - out.tell()))
        handles = {}
        for n, parent in files:
            out.write(b"\0" * (data_at + data_offsets[(id(parent), n)] - out.tell()))
            path, at, size = parent.files[n]
            src = handles.get(path) or handles.setdefault(path, open(path, "rb"))
            src.seek(at)
            while size:
                chunk = src.read(min(size, 1 << 22))
                if not chunk:
                    raise IOError(f"{path}: short read")
                out.write(chunk)
                size -= len(chunk)
        for h in handles.values():
            h.close()
    os.replace(out_path + ".part", out_path)


def apply_ips(code, patch):
    """an IPS patch (as Luma's code.ips) applied to the decompressed code"""
    if patch[:5] != b"PATCH":
        raise ValueError("not an IPS patch")
    out, i = bytearray(code), 5
    while patch[i:i + 3] != b"EOF":
        at = int.from_bytes(patch[i:i + 3], "big")
        size = int.from_bytes(patch[i + 3:i + 5], "big")
        i += 5
        if size:
            chunk = patch[i:i + size]
            i += size
        else:  # run-length: a count and a byte
            count, value = int.from_bytes(patch[i:i + 2], "big"), patch[i + 2]
            i += 3
            chunk = bytes([value]) * count
        if at + len(chunk) > len(out):
            out.extend(b"\0" * (at + len(chunk) - len(out)))
        out[at:at + len(chunk)] = chunk
    return bytes(out)


if __name__ == "__main__":
    # a check: a ROM's RomFS read and written back must hold the same files, the same bytes
    import sys, hashlib, tempfile
    rom = sys.argv[1]
    at, _ = level3(rom)
    tree = read_tree(rom, at)
    out = os.path.join(tempfile.gettempdir(), "romfs_check.bin")
    write(tree, out)
    again = read_tree(out, 0)

    def listing(node, prefix=""):
        for n, (p, o, s) in node.files.items():
            with open(p, "rb") as f:
                f.seek(o)
                yield prefix + n, hashlib.sha1(f.read(s)).hexdigest()
        for n, d in node.dirs.items():
            yield from listing(d, prefix + n + "/")

    a, b = sorted(listing(tree)), sorted(listing(again))
    assert a == b, "the RomFS written back differs"
    print(f"ok: {len(a)} files the same")
    os.remove(out)

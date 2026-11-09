"""Maintainer tool: identify an x64 lock wrapper on disk. Never opens a process.

python analyze_build.py --bin-dir <Win64Steam> [--report <new-file>]
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


class FormatError(Exception):
    pass


def require(ok):
    if not ok:
        raise FormatError("PE structure or parser limit")


class Image:
    def __init__(self, data):
        self.data = data
        require(data[:2] == b"MZ")
        pe = self.get(60)
        require(data[pe:pe + 4] == b"PE\0\0" and self.get(pe + 4, "H") == 0x8664)
        opt = pe + 24
        n, size = self.get(pe + 6, "H"), self.get(pe + 20, "H")
        require(0 < n <= 96 and size >= 144 and self.get(opt, "H") == 0x20B)
        require(self.get(opt + 108) >= 4)
        self.timestamp, self.image_size = self.get(pe + 8), self.get(opt + 56)
        self.directories = opt + 112
        self.sections = []
        for i in range(n):
            s = opt + size + 40 * i
            virtual_size, va, length, offset = struct.unpack_from("<IIII", data, s + 8)
            require(offset + length <= len(data) and va + max(length, virtual_size) <= self.image_size)
            self.sections.append((va, max(length, virtual_size), offset, length, self.get(s + 36)))

    def get(self, off, kind="I"):
        require(off >= 0 and off + struct.calcsize("<" + kind) <= len(self.data))
        return struct.unpack_from("<" + kind, self.data, off)[0]

    def span(self, rva, size, raw=True):
        require(size > 0 and 0 <= rva < self.image_size)
        found = [(off + rva - va, flags) for va, vs, off, rs, flags in self.sections
                 if va <= rva and rva + size <= va + (rs if raw else vs)]
        require(len(found) == 1)
        return found[0]

    def word(self, rva, kind):
        return self.get(self.span(rva, struct.calcsize("<" + kind))[0], kind)

    def name(self, rva):
        off, _ = self.span(rva, 1)
        end = self.data.find(b"\0", off, off + 512)
        require(end >= off)
        self.span(rva, end - off + 1)
        return self.data[off:end].decode("ascii").lower()

    def directory(self, i):
        rva, length = struct.unpack_from("<II", self.data, self.directories + i * 8)
        return self.span(rva, length)[0], length

    def locks(self):
        p, size = self.directory(1)
        require(size <= 20 * 4096)
        slots = {}
        for q in range(p, p + size - 19, 20):
            names, _, _, dll, iat = struct.unpack_from("<IIIII", self.data, q)
            if not any((names, dll, iat)):
                break
            if self.name(dll) != "msvcp140.dll":
                continue
            require(names and iat)
            for i in range(8192):
                thunk = self.word(names + i * 8, "Q")
                if not thunk:
                    break
                if thunk >> 63:
                    continue
                symbol = self.name(thunk + 2)
                if symbol in ("_mtx_lock", "_mtx_unlock"):
                    require(symbol not in slots)
                    self.span(iat + 8 * i, 8, raw=False)
                    slots[symbol] = iat + 8 * i
            else:
                raise FormatError("unterminated import table")
        else:
            raise FormatError("unterminated import descriptors")
        return slots

    def candidates(self, slots):
        if len(slots) != 2:
            return []
        p, size = self.directory(3)
        require(size % 12 == 0 and size <= 12 * 400000)
        hits = []
        for start, end, _ in struct.iter_unpack("<III", self.data[p:p + size]):
            if not 39 <= end - start <= 160:
                continue
            off, flags = self.span(start, end - start)
            if not flags & 0x20000000:
                continue
            f = self.data[off:off + 39]
            # Decode one known wrapper shape from its function entry. The two jumps
            # must land on the unlock call and its return. Displacements vary by build.
            if (f[:7] != bytes.fromhex("48 83 ec 28 48 8b 05") or
                f[11:25] != bytes.fromhex("f6 c1 01 48 63 ca 48 8b 0c c8 74 08 ff 15") or
                f[29:33] != bytes.fromhex("eb 06 ff 15") or f[37:39] != bytes.fromhex("85 c0")):
                continue
            vec = start + 11 + self.get(off + 7, "i")
            lock = start + 29 + self.get(off + 25, "i")
            unlock = start + 37 + self.get(off + 33, "i")
            if (lock, unlock) != (slots["_mtx_lock"], slots["_mtx_unlock"]):
                continue
            _, attrs = self.span(vec, 16, raw=False)
            if vec % 8 or attrs & 0x20000000 or not attrs & 0x40000000:
                continue
            hits.append({"unlockIatRva": unlock, "unlockReturnRva": start + 37, "lockVectorRva": vec})
        return hits


def inspect(path):
    with path.open("rb") as f:
        data = f.read(128 * 1024 * 1024 + 1)
    require(0 < len(data) <= 128 * 1024 * 1024)
    image = Image(data)
    hits = image.candidates(image.locks())
    return {"file": path.name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest().upper(),
            "peTimestamp": image.timestamp, "imageSize": image.image_size, "matches": len(hits),
            "candidate": hits[0] if len(hits) == 1 else None,
            "requiresValidation": ["service slots and state fields", "runtime mutex layout",
                                   "abnormal and normal calls", "in-game result and cancellation"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--report", type=Path, help="create a new UTF-8 report; never overwrite")
    args = parser.parse_args()
    records = []
    for name in ("CivilizationVI.exe", "CivilizationVI_DX12.exe"):
        try:
            records.append(inspect(args.bin_dir / name))
        except (OSError, FormatError, struct.error, UnicodeError) as error:
            records.append({"file":name, "candidate":None, "error":type(error).__name__})
    report = json.dumps({"schema":1,"mode":"offline-candidate-only","images":records}, ensure_ascii=False, indent=2)
    if args.report:
        with args.report.open("x", encoding="utf-8") as f:
            print(report, file=f)
    else:
        print(report)
    return 0 if all(r.get("candidate") is not None for r in records) else 2


if __name__ == "__main__":
    sys.exit(main())

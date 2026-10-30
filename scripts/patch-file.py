#!/usr/bin/env python3
"""Applies the signature patches of shim/rhv_patch.c to a file: patch-file.py <in> <out> vendor gla
Same patterns as the shim; each must match exactly once (or already be applied)."""
import re, sys
PAT = {
 "vendor": ("48 b9 6e 65 49 6e 74 65 6c 00  48 33 4d ??  48 09 c1  0f 85 ?? ?? ?? ??",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??  90 90 90 90 90 90"),
 "gla":    ("48 3b 9d ?? ?? ?? ??  4c 8b 7d ??  0f 84 ?? ?? ?? ??  48 83 ec 08  48 8d 3d",
            "?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  90 e9 ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??"),
}
def parse(s): return [None if t == "??" else int(t, 16) for t in s.split()]
def rx(p): return re.compile(b"".join(b"." if b is None else re.escape(bytes([b])) for b in p), re.S)
data = bytearray(open(sys.argv[1], "rb").read())
for name in sys.argv[3:]:
    f, p = parse(PAT[name][0]), parse(PAT[name][1])
    done = [fb if pb is None else pb for fb, pb in zip(f, p)]
    orig = [m.start() for m in rx(f).finditer(bytes(data))]
    already = [m.start() for m in re.compile(b"".join(b"." if b is None else re.escape(bytes([b])) for b in
              [None if pb is None and fb is None else (fb if pb is None else pb) for fb, pb in zip(f, p)]), re.S).finditer(bytes(data))]
    if len(orig) + len(already) != 1:
        sys.exit(f"{name}: {len(orig)} original and {len(already)} patched matches, expected exactly one")
    if orig:
        at = orig[0]
        for i, pb in enumerate(p):
            if pb is not None: data[at + i] = pb
        print(f"{name}: patched at {at:#x}")
    else:
        print(f"{name}: already patched at {already[0]:#x}")
open(sys.argv[2], "wb").write(data)

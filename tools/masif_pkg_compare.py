#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare two FM-1 packages (.fwsc) part by part: what does installing B after A change on the device?

A package is flash.bin (the head [0, 0x4000): flash header, the SPL uboot.boot, isd_config; then the app area
from 0x4000: app.bin, cfg_tool.bin, eq_cfg_hw.bin, the region descriptors) and ota.bin (the update loader).
The loader writes the app area only (firmware/loader/ldr_core.c), never the head. The app area is enciphered
by position, so equal bytes at equal offsets are equal contents.

  masif_pkg_compare.py REFERENCE.fwsc OURS.fwsc      exit 0: only app.bin (and what is derived from it) differs
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install as fi  # noqa: E402
import fm1pkg_make as mk  # noqa: E402

APP_LO, APP_OFF = 0x4000, 0x120


def parts(path):
    raw = Path(path).read_bytes()
    product, image = fi.load_package(str(path), False)
    hdr = fi._dec(image[0:0x40])
    count = int.from_bytes(hdr[8:10], "little")
    files = {}
    for i in range(count):
        e = fi._dec(image[0x40 + i * 0x50:0x40 + (i + 1) * 0x50])
        off, size = int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little")
        files[e[0x40:0x50].split(b"\0")[0].decode("ascii", "replace")] = image[off:off + size]
    return product, len(raw), files


def diff_ranges(a, b):
    n, out, i = min(len(a), len(b)), [], 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and (a[j] != b[j] or a[j:j + 16] != b[j:j + 16]):
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def main(ref, ours):
    pa, na, fa = parts(ref)
    pb, nb, fb = parts(ours)
    print(f"reference  {ref}: identity {pa}, {na} B, files {', '.join(f'{k} {len(v)}' for k, v in fa.items())}")
    print(f"ours       {ours}: identity {pb}, {nb} B, files {', '.join(f'{k} {len(v)}' for k, v in fb.items())}")
    bad = 0

    def check(ok, what):
        nonlocal bad
        bad += not ok
        print(f"  {'same     ' if ok else 'DIFFERENT'}  {what}")

    check(pa == pb, f"package identity ({pa} / {pb})")
    check(sorted(fa) == sorted(fb), "the files in the package")
    ota = [k for k in fa if k != "flash.bin"]
    for k in ota:
        check(fa[k] == fb.get(k), f"{k}: the update loader that writes the firmware")
    A, B = fa["flash.bin"], fb["flash.bin"]
    check(len(A) == len(B), f"flash.bin's size ({len(A)} / {len(B)})")
    check(A[:APP_LO] == B[:APP_LO], "flash head 0x0-0x3FFF (flash header, SPL uboot.boot, isd_config): never written by the loader")
    app_end = APP_LO + APP_OFF + mk.APP_SLOT
    check(A[app_end:] == B[app_end:], f"after app.bin, from 0x{app_end:x}: cfg_tool.bin, the region descriptors")
    d = diff_ranges(A[APP_LO:app_end], B[APP_LO:app_end])
    in_app = [r for r in d if r[0] >= APP_OFF]
    in_head = [r for r in d if r[0] < APP_OFF]
    print(f"  app area head 0x{APP_LO:x}-0x{APP_LO + APP_OFF - 1:x} (entries with app.bin's CRC, eq_cfg_hw.bin): "
          f"{sum(j - i for i, j in in_head)} B differ in {len(in_head)} places"
          + (": " + ", ".join(f"+0x{i:x}..0x{j - 1:x}" for i, j in in_head[:8]) if in_head else ""))
    print(f"  app.bin (the firmware itself): {sum(j - i for i, j in in_app)} B of {mk.APP_SLOT} differ")
    print("\nonly the app (and its checksums) differs: the same SPL, loader and configuration as the reference"
          if not bad else f"\n{bad} part(s) besides the app differ")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))

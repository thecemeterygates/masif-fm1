#!/bin/sh
# Read-only MASIF/JieLi build dependency preflight. Does not build or flash.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TC=${JIELI_TOOLCHAIN:-"$HOME/.jieli/toolchain"}
SDK=${AC79_SDK:-"$HOME/fw-AC79_AIoT_SDK"}
fail=0
printf 'MASIF target-build preflight\nsource: %s\n' "$ROOT"
printf 'host: '; uname -srm
if [ -x "$TC/pi32v2/bin/clang" ] && [ -x "$TC/pi32v2/bin/ld" ]; then
  printf 'PASS toolchain found: %s\n' "$TC"
else
  printf 'MISSING JieLi toolchain: %s\n' "$TC"
  printf '  Install with: cd "%s" && tools/get_toolchain.sh\n' "$ROOT"
  fail=1
fi
if [ -f "$SDK/cpu/wl82/tools/uboot.boot" ] && [ -f "$SDK/cpu/wl82/tools/cfg_tool.bin" ] && [ -f "$SDK/cpu/wl82/tools/cfg/eq_cfg_hw.bin" ]; then
  printf 'PASS required SDK assets found: %s\n' "$SDK"
  python3 - "$ROOT" "$SDK" <<'PY'
import hashlib, pathlib, sys
root, sdk = map(pathlib.Path, sys.argv[1:])
ns = {}
text=(root/'tools/build.py').read_text()
import re
m=re.search(r'SDK_SHA256\s*=\s*\{(.*?)\n\}', text, re.S)
if m:
    for rel, sha in re.findall(r'"([^"]+)"\s*:\s*"([0-9a-f]{64})"', m.group(1)):
        p=sdk/'cpu/wl82/tools'/rel
        if not p.exists():
            print(f'FAIL missing {p}'); continue
        got=hashlib.sha256(p.read_bytes()).hexdigest()
        print(('PASS' if got==sha else 'WARN')+f' SHA256 {rel}: {got}'+(' (expected)' if got==sha else ' (differs from build.py reference)'))
PY
else
  printf 'MISSING SDK assets under: %s\n' "$SDK"
  printf '  SDK branch expected by this source build: AC79NN_SDK_V1.2.1_2023-12-13\n'
  fail=1
fi
python3 - <<'PY'
try:
    import PIL
    print('PASS Python Pillow available')
except Exception:
    print('MISSING Python Pillow (install with: python3 -m pip install Pillow)')
    raise SystemExit(1)
PY
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    printf 'PASS Docker available for linux/amd64 tool execution\n'
  else
    printf 'MISSING running Docker Desktop/daemon (required on macOS or non-x86_64 hosts)\n'
    fail=1
  fi
fi
if [ "$fail" -ne 0 ]; then
  printf '\nPreflight incomplete. No build or flash was attempted.\n'
  exit 1
fi
printf '\nPASS dependency preflight. Next: ./build.sh (builds a package; do NOT flash it until SPL/recovery compatibility is resolved).\n'

#!/usr/bin/env python3
"""Dry run of tools/fm1_install.py (the Felucca family's installer, unchanged) with a package of this tree and no FM-1:
a simulated device on a fake MIDI backend, so neither mido nor hardware is needed. The device is modelled on
the tool's own wire format (it cannot prove what a real FM-1 does): the running firmware answers the
handshake, reads a few parts of the package and switches to the loader; the loader reads the whole package
and restarts as the package's identity. Checked: the tool serves exactly the package's logical image, ends
with exit 0, and refuses a damaged package before it talks to the device.

  python3 tests/fm1_install_test.py [build/zp12.fwsc]
"""
import io
import queue
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as fi  # noqa: E402

for k in fi.DELAY:                                   # no waiting in a simulation
    fi.DELAY[k] = 0.0 if k not in ("hs", "poll", "wait_loader", "wait_reboot", "idle_check", "idle_write") else 0.2


def identity(text):
    d = bytearray(34)
    d[0:3] = b"\x00\x59\x11"
    d[6:6 + len(text)] = text.encode()
    return b"\xF0" + fi.pack7(d) + b"\xF7"


def request(addr, n, fl=0):
    u = bytearray(b"\x00\x59\x30\x00\x00\x00") + bytes([fl]) + addr.to_bytes(4, "little") + n.to_bytes(3, "little")
    u.append(~sum(u[6:14]) & 0xFF)
    return b"\xF0" + fi.pack7(u) + b"\xF7"


class Device:
    """mode app -> (check) -> ota -> (write) -> new"""

    def __init__(self, stock, product, size):
        self.mode, self.stock, self.product, self.size = "app", stock, product, size
        self.got = bytearray(size)
        self.plan, self.talked, self.writes = [], 0, 0

    def ident(self):
        return {"app": self.stock, "ota": "ota-" + self.product, "new": self.product}[self.mode]

    def on(self, pkt, rx):
        self.talked += 1
        if pkt == fi.HS_QUERY:
            rx.put(identity(self.ident()))
        elif pkt == fi.UPGRADE:
            if self.mode == "app":                    # the header, the file list, a bit of the end
                self.plan = [(0, 0x40), (0x40, 0x1E0), (self.size - 100, 100), (fi.FINISH_CHECK, 0)]
            else:
                self.plan = [(a, min(fi.MAXDATA, self.size - a)) for a in range(0, self.size, fi.MAXDATA)]
                self.plan.append((fi.FINISH_WRITE, 0))
            rx.put(request(*self.plan[0]))
        else:                                         # an answer to the read at the plan's head
            u = fi.unpack7(pkt[1:-1])
            addr, n = self.plan.pop(0)
            assert u[:3] == b"\x00\x59\x30" and int.from_bytes(u[7:11], "little") == addr, "answer to another read"
            if addr in (fi.FINISH_CHECK, fi.FINISH_WRITE):
                assert bytes(u[14:22]) == b"success\0"
                self.mode = "ota" if addr == fi.FINISH_CHECK else "new"
                return
            assert int.from_bytes(u[11:13], "little") == n and ~sum(u[6:14 + n]) & 0xFF == u[14 + n], "length / sum"
            if self.mode == "ota":
                self.got[addr:addr + n] = u[14:14 + n]
                self.writes += 1
            rx.put(request(*self.plan[0]))


class Link:
    lost = False

    def __init__(self, dev):
        self.dev, self.q = dev, queue.Queue()

    def send(self, pkt):
        self.dev.on(bytes(pkt), self.q)
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        pass


class Backend:
    def __init__(self, dev):
        self.dev = dev

    def input_names(self):
        return ["FM-1 MIDI 1"]

    output_names = input_names

    def open(self, in_name, out_name):
        return Link(self.dev)


def main():
    pkg = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build" / "zp12.fwsc")
    raw = pkg.read_bytes()
    product, image = fi.product_of(raw), fi.logical_image(raw)
    bad = 0

    def check(ok, what):
        nonlocal bad
        bad += not ok
        print(f"install: {what:<86} {'ok' if ok else 'FAIL'}")

    dev = Device("FM-1_015", product, len(image))
    out = io.StringIO()
    rc = fi.main([str(pkg), "--yes"], backend=Backend(dev), out=out)
    check(rc == 0 and dev.mode == "new", f"{pkg.name} ({product}, {len(raw)} B) installs on the simulated FM-1_015: exit {rc}")
    check(bytes(dev.got) == image and dev.writes == -(-len(image) // fi.MAXDATA),
          f"the loader was served the whole logical image ({len(image)} B in {dev.writes} reads), byte for byte")
    check(f"done: the FM-1 runs {product}" in out.getvalue(), "the tool reports the package's identity after the restart")

    dev = Device("FM-1_015", product, len(image))
    rc = fi.main(["--info"], backend=Backend(dev), out=io.StringIO())
    check(rc == 0 and dev.mode == "app" and not dev.plan, "--info only asks who is there")

    hurt = bytearray(raw)
    hurt[len(hurt) // 2] ^= 0x10                      # one bit in the app
    tmp = pkg.with_name(pkg.stem + ".damaged-test.fwsc")
    tmp.write_bytes(hurt)
    try:
        dev = Device("FM-1_015", product, len(image))
        err, sys.stderr = sys.stderr, io.StringIO()
        try:
            rc = fi.main([str(tmp), "--yes"], backend=Backend(dev), out=io.StringIO())
        finally:
            sys.stderr = err
        check(rc == 2 and dev.talked == 0, "a package with one flipped bit is refused before the device is spoken to")
    finally:
        tmp.unlink()

    dev = Device("XY-9_001", product, len(image))
    err, sys.stderr = sys.stderr, io.StringIO()
    try:
        rc = fi.main([str(pkg), "--yes"], backend=Backend(dev), out=io.StringIO())
    finally:
        sys.stderr = err
    check(rc == 6 and dev.mode == "app" and not dev.plan, "another model is refused: no update is started")

    print("\nALL OK" if not bad else f"\n{bad} FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

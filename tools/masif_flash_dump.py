#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only dump of the FM-1's data flash (0x93000..0xFFFFF) over zp12's USB link (firmware/src/sp_link.c),
and a report of what the sectors hold.

  dump     python3 tools/masif_flash_dump.py dump OUT.bin [--port /dev/snd/midiC1D0] [--sim build/host/zp12_link_host]
  analyze  python3 tools/masif_flash_dump.py analyze OUT.bin [--unverified]
  tests    python3 tests/masif_flash_dump_test.py     (needs tests/zp12_link_host.c built: see that file)

Transport: USB MIDI SysEx through Linux ALSA raw MIDI (/dev/snd/midiC*D*, no modules needed), one request at a
time: F0 pack7(7D 'Z' 'P' cmd addr:3 len:2 sum) F7, 512 bytes a READ, 872 READs a pass. Elsewhere there is only
web/backup.html, which does not read this whole range.

What is sent: HELLO (1) and READ (2) inside 0x93000..0xFFFFF, nothing else, and that is checked twice (frame()
builds only these; the port refuses any other bytes). No HOLD, ERASE, WRITE, REBOOT, RELOAD, ASSIGN, PADS.

What that does not promise:
 - The FM-1 is not left untouched. Every link message holds zp12's autosave off for 10 s (zp12.c: zl_busy), so
   nothing is autosaved while the dump runs; each READ reads the flash with interrupts off, which may be heard.
   The sequencer is not stopped (that would be HOLD): stop it by hand, and do not press SAVE or use the web
   editor meanwhile. A store written between the passes makes them differ, and then nothing is saved.
 - The link's checksum is 8 bits. A damaged reply is asked again, and the range is read twice and compared, so
   a dump is saved only if both passes agree byte for byte; an error that falls the same way twice is not seen.
 - Only a firmware that answers HELLO as "zp12 ..." is read. The port is found by the ALSA card name "Felucca",
   which SLOOP, sloopDX and the update loader carry too: with more than one such card, --port must be given.
 - Nothing below 0x93000 (boot code, the app) can be read: the firmware refuses it, and so does this tool.

Files: OUT.bin (the 446464 bytes) and OUT.bin.json (range, size, SHA-256, what HELLO said, retries, a CRC-32
per sector), both new, mode 0600: the dump holds the vendor's 'key_mac' and 'BTIF' sectors, keep it private.
Neither is ever written over; OUT.bin.json is written last and is what says the dump is whole. `analyze`
checks OUT.bin against it and refuses a file without one (--unverified: analyze it all the same, marked so).
The report names sectors by address and kind only: no flash bytes are printed.

The kinds: 'verified' (a structure whose own checksum holds, checked as the firmware checks it), 'referenced'
(named by a verified structure, with no checksum of its own), 'blank' (all 0xFF now) and 'unidentified'
(everything else). None of them means free or safe to erase."""
import argparse, errno, glob, hashlib, json, os, re, select, stat, struct, subprocess, sys, time, zlib

LO, HI, SECT, CHUNK, PASSES = 0x93000, 0x100000, 0x1000, 512, 2
HELLO, READ = 1, 2
ALLOWED = frozenset((HELLO, READ))                   # the whole of what this tool may send
WIRE_MAX = (11 + CHUNK) * 8 // 7 + 2                 # a reply's 7-bit bytes at most
assert (HI - LO) % SECT == 0 and SECT % CHUNK == 0


class DumpError(Exception):
    pass


def pack7(b):
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= x << nb; nb += 8
        while nb >= 7:
            out.append(acc & 0x7F); acc >>= 7; nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(w):
    out, acc, nb = bytearray(), 0, 0
    for x in w:
        acc |= (x & 0x7F) << nb; nb += 7
        if nb >= 8:
            out.append(acc & 0xFF); acc >>= 8; nb -= 8
    return bytes(out)


def allowed(cmd, addr, n):                           # HELLO as it is, or a READ wholly inside the range
    if type(cmd) is not int or type(addr) is not int or type(n) is not int:
        return False
    if cmd == HELLO:
        return addr == 0 and n == 0
    return cmd == READ and LO <= addr < HI and 1 <= n <= CHUNK and addr + n <= HI


def frame(cmd, addr=0, n=0):                         # the only place a message is built
    if not allowed(cmd, addr, n) or cmd not in ALLOWED:
        raise DumpError(f"refused: command {cmd!r} at {addr!r} + {n!r} is not a read inside {LO:#x}..{HI - 1:#x}")
    m = bytes([0x7D, 0x5A, 0x50, cmd, addr & 255, (addr >> 8) & 255, (addr >> 16) & 255, n & 255, n >> 8])
    return b"\xF0" + pack7(m + bytes([~sum(m[3:]) & 255])) + b"\xF7"


def wire_guard(b):                                   # the port's own look at what is about to go out
    m = unpack7(b[1:-1]) if len(b) >= 3 else b""
    if (len(b) < 3 or b[0] != 0xF0 or b[-1] != 0xF7 or any(x & 0x80 for x in b[1:-1]) or len(m) != 10 or
            m[:3] != b"\x7DZP" or (~sum(m[3:9]) & 255) != m[9] or
            not allowed(m[3], m[4] | m[5] << 8 | m[6] << 16, m[7] | m[8] << 8)):
        raise DumpError("refused: these bytes are not one HELLO or one READ inside the range; nothing was sent")
    return b


def parse(w):                                        # a reply: 7D 'Z' 'P' cmd rc addr:3 len:2 data sum, and no more
    m = unpack7(w)
    if len(m) < 11 or m[:3] != b"\x7DZP":
        return None
    n = m[8] | m[9] << 8
    if n > CHUNK or len(m) != 11 + n or (~sum(m[3:10 + n]) & 255) != m[10 + n]:
        return None
    return {"cmd": m[3], "rc": m[4], "addr": m[5] | m[6] << 8 | m[7] << 16, "data": m[10:10 + n]}


class RawMidi:                                       # /dev/snd/midiC*D*: the bytes as they are
    def __init__(self, path):
        real = os.path.realpath(path)                # (a plain file given as the port would be written into)
        try:
            is_chr = stat.S_ISCHR(os.stat(real).st_mode)
        except OSError as e:
            raise DumpError(f"{path}: {e.strerror}")
        if not is_chr or not re.fullmatch(r"/dev/snd/midiC\d+D\d+", real):
            raise DumpError(f"{path} is not an ALSA raw MIDI device (/dev/snd/midiC*D*): not opened")
        try:
            self.fd = os.open(real, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError as e:
            raise DumpError(f"{path}: {e.strerror} (is another program using the port?)")

    def send(self, b):
        b, end = wire_guard(b), time.monotonic() + 2.0
        while b:
            if time.monotonic() > end:
                raise DumpError("the MIDI port does not take data")
            select.select([], [self.fd], [], 0.5)
            try:
                b = b[os.write(self.fd, b):]
            except BlockingIOError:
                pass
            except OSError as e:
                raise DumpError(f"the MIDI port failed: {e.strerror} (unplugged?)")

    def recv(self, timeout):
        if not select.select([self.fd], [], [], timeout)[0]:
            return b""
        try:
            d = os.read(self.fd, 4096)
        except BlockingIOError:
            return b""
        except OSError as e:
            raise DumpError(f"the MIDI port failed: {e.strerror} (unplugged?)")
        if not d:
            raise DumpError("the MIDI port closed (unplugged?)")
        return d

    def close(self):
        os.close(self.fd)


class Sim:                                           # tests/zp12_link_host.c: hex lines
    def __init__(self, exe):
        try:
            self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        except OSError as e:
            raise DumpError(f"{exe}: {e.strerror}")
        self.rx = b""

    def _put(self, line):
        try:
            self.p.stdin.write(line + b"\n")
        except OSError:
            raise DumpError("the simulator ended")

    def _line(self, timeout):
        while b"\n" not in self.rx:
            if not select.select([self.p.stdout], [], [], timeout)[0]:
                return None
            d = os.read(self.p.stdout.fileno(), 1 << 16)
            if not d:
                raise DumpError("the simulator ended")
            self.rx += d
        line, self.rx = self.rx.split(b"\n", 1)
        return line.strip()

    def send(self, b):
        self._put(wire_guard(b).hex().encode())

    def recv(self, timeout):
        line = self._line(timeout)
        return bytes.fromhex(line.decode()) if line else b""

    def ctl(self, text):                             # the simulator's own lines (DUMP, STATS): no FM-1 has them
        self._put(text.encode())
        return self._line(10.0)

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class Link:                                          # one request at a time; a lost or damaged reply: asked again
    def __init__(self, io, timeout=1.5, tries=4, clock=time.monotonic):
        self.io, self.timeout, self.tries, self.clock = io, timeout, tries, clock
        self.buf, self.retries, self.ignored = None, 0, 0

    def _frames(self, data):                         # the replies in these bytes (None: a damaged one)
        for x in data:
            if x == 0xF0:
                self.buf = bytearray()
            elif x == 0xF7:
                if self.buf is not None:
                    w, self.buf = self.buf, None
                    yield parse(w)
            elif x >= 0xF8:
                pass                                 # (real time: may come inside a SysEx)
            elif x >= 0x80:
                self.buf = None                      # another status ends an unfinished SysEx
            elif self.buf is not None:
                self.buf.append(x)
                if len(self.buf) > WIRE_MAX:
                    self.buf = None

    def request(self, cmd, addr=0, n=0):
        req = frame(cmd, addr, n)
        self.buf = None
        for _ in range(64):                          # what is left of earlier answers goes
            if not self.io.recv(0):
                break
        for t in range(self.tries):
            self.retries += t > 0
            self.io.send(req)
            end = self.clock() + self.timeout
            while (left := end - self.clock()) > 0:
                for r in self._frames(self.io.recv(left)):
                    if r and r["cmd"] == cmd and r["addr"] == addr:
                        return r
                    self.ignored += 1
        raise DumpError(f"no answer from the FM-1 (command {cmd} at {addr:#x}, {self.tries} tries): is zp12 1.3+ running?")

    def hello(self):
        r = self.request(HELLO)
        d = r["data"]
        z = d.find(0, 1)
        if r["rc"] or len(d) < 4 or z < 0 or z + 1 >= len(d) or not d[1:z].startswith(b"zp12 ") or d[0] < 1 or \
                any(x < 32 or x > 126 for x in d[1:z]):
            raise DumpError("HELLO was answered, but not as zp12 answers it: this firmware is not read")
        return {"proto": d[0], "version": d[1:z].decode("ascii"), "kit": d[z + 1]}

    def read(self, addr, n):
        r = self.request(READ, addr, n)
        if r["rc"] or len(r["data"]) != n:
            raise DumpError(f"READ {addr:#x}: error {r['rc']} ({len(r['data'])} of {n} bytes)")
        return r["data"]


def find_port():
    try:
        cards = open("/proc/asound/cards").read().split("\n")
    except OSError:
        raise DumpError("no ALSA here (/proc/asound/cards): this tool needs Linux, or give --port")
    ports = []
    for c in cards:
        if "felucca" in c.lower() and c.strip()[:1].isdigit():
            ports += sorted(glob.glob(f"/dev/snd/midiC{int(c.split()[0])}D*"))[:1]
    if len(ports) > 1:
        raise DumpError("more than one ALSA card named Felucca (" + ", ".join(ports) + "): give --port")
    if not ports:
        raise DumpError("no FM-1 found (no ALSA card named Felucca): plug it in with zp12 running, or give --port")
    return ports[0]


def dump(link, progress=None):                       # -> what HELLO said, the passes, the sectors that differ
    info = link.hello()
    passes = []
    for k in range(PASSES):
        b = bytearray()
        for a in range(LO, HI, CHUNK):
            b += link.read(a, CHUNK)
            if len(b) != a + CHUNK - LO:
                raise DumpError(f"READ {a:#x}: the pass is {len(b)} bytes long here, not {a + CHUNK - LO}")
            if progress and a % 0x8000 == 0:
                print(f"\rpass {k + 1}/{PASSES} {a:#x}", end="", file=progress, flush=True)
        passes.append(bytes(b))
    if progress:
        print(file=progress)
    if link.hello() != info:
        raise DumpError("HELLO at the end is not what it was at the start (restarted? another firmware?). Nothing saved.")
    diff = [LO + s for s in range(0, HI - LO, SECT) if any(p[s:s + SECT] != passes[0][s:s + SECT] for p in passes)]
    return info, passes, diff


def write_new(path, data):                           # a whole new file or none: never over one that is there
    tmp = path + ".part"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    except FileExistsError:
        raise DumpError(f"{tmp} exists (left by a run that was cut off?): look at it, remove it, then again")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        try:
            os.link(tmp, path)                       # (fails if path is there; a rename would replace it)
        except FileExistsError:
            raise DumpError(f"{path} exists: a dump is never written over")
        except OSError as e:
            if e.errno not in (errno.EPERM, errno.ENOTSUP, errno.EOPNOTSUPP):
                raise
            try:                                     # no hard links here (FAT): still only a new file
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
            except FileExistsError:
                raise DumpError(f"{path} exists: a dump is never written over")
            try:
                with os.fdopen(fd, "wb") as f:
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
            except BaseException:
                os.unlink(path)
                raise
    finally:
        os.unlink(tmp)


def cmd_dump(io, path, simulated, progress=None):
    side = path + ".json"
    for p in (path, side, path + ".part", side + ".part"):   # before a byte goes to the FM-1
        if os.path.lexists(p):
            raise DumpError(f"{p} exists: a dump is never written over")
    link = Link(io)
    t0 = time.time()
    info, passes, diff = dump(link, progress)
    img = passes[0]
    if diff or len(img) != HI - LO or any(p != img for p in passes):
        raise DumpError("the passes differ at " + ", ".join(f"{x:#x}" for x in diff) +
                        ": the FM-1 wrote meanwhile (stop it, do not save) or the link is unsound. Nothing saved.")
    meta = {"tool": "masif_flash_dump", "format": 2, "complete": True, "range": [LO, HI], "size": len(img),
            "sha256": hashlib.sha256(img).hexdigest(), "passes": PASSES, "hello": info, "retries": link.retries,
            "ignored_frames": link.ignored, "seconds": round(time.time() - t0, 1),
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "simulated": bool(simulated),
            "sector_crc32": [zlib.crc32(img[s:s + SECT]) for s in range(0, len(img), SECT)]}
    made = []
    try:
        write_new(path, img); made.append(path)
        write_new(side, (json.dumps(meta, indent=1) + "\n").encode()); made.append(side)
        if open(path, "rb").read() != img:
            raise DumpError(f"{path} does not read back as it was written. Nothing kept.")
    except BaseException:
        for p in made:                               # (ours: made new just above)
            os.unlink(p)
        raise
    print(f"{path}: {len(img)} bytes from {info['version']} (protocol {info['proto']}), sha256 {meta['sha256']}, "
          f"{link.retries} retries, {link.ignored} replies ignored, {meta['seconds']} s")
    return meta


def load_dump(path, unverified=False):               # -> the image and what its .json says (None: there is none)
    try:
        img = open(path, "rb").read()
    except OSError as e:
        raise DumpError(f"{path}: {e.strerror}")
    try:
        meta = json.load(open(path + ".json"))
    except FileNotFoundError:
        meta = None
    except (OSError, ValueError) as e:
        raise DumpError(f"{path}.json cannot be read ({e}): not a dump this tool finished")
    if meta is None:
        if not unverified:
            raise DumpError(f"{path}.json is missing: nothing says this file is a whole dump (--unverified to look at it anyway)")
    elif (meta.get("complete") is not True or meta.get("range") != [LO, HI] or meta.get("size") != len(img) or
          meta.get("sha256") != hashlib.sha256(img).hexdigest()):
        raise DumpError(f"{path} is not the dump {path}.json describes (size, range or SHA-256): cut off or changed")
    if len(img) != HI - LO:
        raise DumpError(f"a dump is {HI - LO} bytes, {path} is {len(img)}: not a whole dump")
    return img, meta


# ---- what the sectors hold. A structure is named only where its own checksum holds, checked as the firmware
# that reads it checks it; the rest is "unidentified", and that never means free: a room is given by the
# firmware that owns it, not by how its bytes look.
ZS_BASE, ZS_SLOT, ZS_MAGIC, ZS_VER = 0xC4000, 0xA000, 0x3231505A, 6       # sp_store.c
ZU_DIR0, ZU_MAGIC, ZU_SLOTS = 0xD8000, 0x4D53505A, 24                     # sp_samples.c
ZU_ROOMS = ((0xA0000, 0xC4000), (0xDA000, 0xDC000), (0xE5000, 0xE9000), (0xEA000, 0xFC000))   # zu_room()
OTA_AREA, OTA_AREA_LEN = 0xE0000, 0x5000                                  # ota.c
FSMP_SLOT = 80 * 1024                                                     # web/EDITOR_PROTOCOL.md


def crc16(p):                                        # ota.c ota_crc16: poly 0x1021, init 0
    c = 0
    for x in p:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


def analyze(img):
    if len(img) != HI - LO:
        raise DumpError(f"a dump is {HI - LO} bytes, this is {len(img)}")
    at = lambda a, n: img[a - LO:a - LO + n]
    proof, ref, hint = {}, {}, {}                    # sector -> checksum held / named by such / looks like, unproven

    def mark(d, lo, hi, what):
        for a in range(lo & ~0xFFF, hi, SECT):
            d.setdefault(a, []).append(what)

    stores = []
    for i in range(2):                               # sp_store.c zs_load: magic "ZP12", ver, len, CRC32 of the body
        a = ZS_BASE + i * ZS_SLOT
        magic, ver, gen, ln, crc = struct.unpack("<5I", at(a, 20))
        if magic != ZS_MAGIC:
            continue
        if ver < 1 or ln > ZS_SLOT - 20:
            mark(hint, a, a + 1, f"the zp12 store's magic (copy {i}), but not a version / length it has")
        elif zlib.crc32(at(a + 20, ln)) != crc:
            mark(hint, a, a + 1, f"the zp12 store's head (copy {i}, gen {gen}), but its CRC-32 fails: a save cut off, or damage")
        else:
            stores.append({"copy": i, "ver": ver, "gen": gen, "len": ln})
            mark(proof, a, a + 20 + ln, f"zp12 store copy {i}: ver {ver}, gen {gen}, {ln} bytes, CRC-32 ok" +
                 (f" (newer than this tree's ZS_VER {ZS_VER}: it would not load it)" if ver > ZS_VER else "") +
                 "; the fields inside are not checked")
    best, dirs = None, []
    for i in range(2):                               # sp_samples.c zu_load: the newer good copy counts
        a = ZU_DIR0 + i * SECT
        magic, gen, flags, crc = struct.unpack("<4I", at(a, 16))
        if magic != ZU_MAGIC:
            continue
        if zlib.crc32(at(a + 16, ZU_SLOTS * 20)) != crc:
            mark(hint, a, a + 1, f"the zp12 sample directory's magic (copy {i}), but its CRC-32 fails")
            continue
        dirs.append({"copy": i, "gen": gen, "flags": flags})
        mark(proof, a, a + 16 + ZU_SLOTS * 20, f"zp12 sample directory copy {i}: gen {gen}, flags {flags}, CRC-32 ok")
        if best is None or gen > best[0]:
            best = (gen, a, flags, i)
    samples, ignored = [], []
    if best:
        for s in range(ZU_SLOTS):
            off, n, rate, fl, _, _ = struct.unpack("<IIHBB8s", at(best[1] + 16 + s * 20, 20))
            size = (n + 1) // 2 * 3
            if not off:
                continue
            if n < 2 or n > 400000 or rate < 8000 or rate > 48000 or not any(lo <= off and off + size <= hi for lo, hi in ZU_ROOMS):
                ignored.append(s)                    # (zu_load skips it too: nothing is claimed for it)
                continue
            samples.append({"slot": s, "off": off, "size": size})
            mark(ref, off, off + size, f"zp12 own sample, slot {s}: {size} bytes from {off:#x}, named by directory copy "
                 f"{best[3]}; the sample data has no checksum")
    records = []
    for end in range(LO + SECT, HI + 1, SECT):       # ldr_core.c ldr_records_drop: an update record the SPL scans for
        r = at(end - 0x100, 80)
        if r[6:8] == b"\x41\x54" and (r[0] | r[1] << 8) and (r[0] | r[1] << 8) == crc16(r[2:80]):
            records.append(end - 0x100)
            mark(proof, end - SECT, end, f"UPDATE RECORD at {end - 0x100:#x}, CRC-16 ok (the SPL would start the loader)")
    for a in range(LO, HI, SECT):                    # headers that carry a checksum of their own
        h = at(a, 32)
        if h[:4] == b"FSMP":                         # web/EDITOR_PROTOCOL.md: SLOOP's user sample slot
            _, ver, zones, pad, _, dlen, crc = struct.unpack("<4sHBB8sII", h[:24])
            if (ver == 1 and 1 <= zones <= 16 and not pad and 0 < dlen <= FSMP_SLOT - 512 and a + 512 + dlen <= HI and
                    zlib.crc32(at(a + 512, dlen)) == crc):
                mark(proof, a, a + 512 + dlen, f"SLOOP user sample slot 'FSMP' from {a:#x}: {dlen} data bytes, CRC-32 ok")
            else:
                mark(hint, a, a + 1, "starts with 'FSMP' (a SLOOP user sample slot), but its fields or CRC-32 do not hold")
        if h[16:32] == b"LOADER.BIN".ljust(16, b"\0"):   # ota.c ota_stage_head
            total = struct.unpack("<I", h[8:12])[0]
            if crc16(h[2:32]) == (h[0] | h[1] << 8) and h[12] == 0x41:
                mark(proof, a, a + 1, "staged update loader head 'LOADER.BIN', CRC-16 of the head ok")
                if struct.unpack("<I", h[4:8])[0] == a == OTA_AREA and 32 < total <= OTA_AREA_LEN - 0x100:
                    mark(ref, a, a + total, f"the staged loader's {total} bytes, as its head says; not checked here")
            else:
                mark(hint, a, a + 1, "named 'LOADER.BIN' (a staged update loader), but its head's CRC-16 fails")

    rows, tot = [], {"verified": 0, "referenced": 0, "blank": 0, "unidentified": 0}
    for a in range(LO, HI, SECT):
        s = at(a, SECT)
        kind = "verified" if a in proof else "referenced" if a in ref else "blank" if s == b"\xFF" * SECT else "unidentified"
        tot[kind] += 1
        rows.append({"addr": a, "kind": kind, "crc32": zlib.crc32(s), "used": SECT - s.count(0xFF),
                     "what": proof.get(a, []) + ref.get(a, []), "hint": hint.get(a, []) if kind == "unidentified" else []})
    live = max((s for s in stores if s["ver"] <= ZS_VER), key=lambda s: (s["gen"], -s["copy"]), default=None)
    return {"sectors": rows, "totals": tot, "stores": stores, "live_store": live and live["copy"], "dirs": dirs,
            "live_dir": best and best[3], "samples": samples, "ignored_slots": ignored, "records": records,
            "bank_flag": bool(best and best[2] & 1)}


REGIONS = [  # who is said to write where, in this repository's sources; said of the address, not read from the dump
    (0x93000, 0x97000, "start of the vendor's 'VM' region (fm1pkg_make.py: 0x93000..0xE8FFF); no reader or writer in this tree"),
    (0x97000, 0xA0000, "Felucca / SLOOP main store (fm1_flash.h FL_DATA 0x97000..0xDFFFF); its layout is not in this tree"),
    (0xA0000, 0xC4000, "sloopDX's DX7 banks + MY KIT (sp_link.c), or zp12 samples if chosen (directory flag bit 0)"),
    (0xC4000, 0xD8000, "zp12 store, two 40 KiB copies (sp_store.c)"),
    (0xD8000, 0xDA000, "zp12 sample directory, two copies (sp_samples.c)"),
    (0xDA000, 0xDC000, "zp12 sample room (sp_samples.c)"),
    (0xDC000, 0xE0000, "inside FL_DATA; tests/zp12_link_host.c calls it sloopDX's presets; the link refuses ERASE / WRITE; layout not in this tree"),
    (0xE0000, 0xE5000, "update loader staging (ota.c OTA_AREA); its record at 0xE4F00"),
    (0xE5000, 0xE9000, "zp12 sample room (sp_samples.c)"),
    (0xE9000, 0xEA000, "vendor 'BTIF' region (fm1pkg_make.py); the link refuses ERASE / WRITE"),
    (0xEA000, 0xFC000, "zp12 sample room (sp_samples.c), the vendor's 'USR' region (fm1pkg_make.py)"),
    (0xFC000, 0xFF000, "Felucca superblock / globals (fm1_flash.h FL_GLOB)"),
    (0xFF000, 0x100000, "vendor 'key_mac' (fm1pkg_make.py)"),
]
KINDS = ("verified", "referenced", "blank", "unidentified")


def report(a, meta=None, out=None):
    out = out or sys.stdout
    p = lambda *x: print(*x, file=out)
    rows = a["sectors"]
    if meta is None:
        p("UNVERIFIED FILE: no .json beside it, so nothing says this is a whole dump of an FM-1.")
    else:
        p(f"dump of {meta['range'][0]:#x}..{meta['range'][1] - 1:#x}, {meta['size']} bytes, SHA-256 as its .json says; "
          f"from '{meta['hello']['version']}', {meta['utc']}, {meta['retries']} retries"
          + (" -- SIMULATED (--sim): not an FM-1's flash" if meta.get("simulated") else ""))
    p("totals:", ", ".join(f"{a['totals'][k]} {k}" for k in KINDS), f"of {len(rows)} sectors of 4 KiB")
    for s in a["stores"]:
        p(f"zp12 store copy {s['copy']}: ver {s['ver']}, gen {s['gen']}" + (" (the one zs_load takes, if its fields unpack)" if s["copy"] == a["live_store"] else ""))
    for d in a["dirs"]:
        p(f"zp12 sample directory copy {d['copy']}: gen {d['gen']}" + (" (the one zu_load takes)" if d["copy"] == a["live_dir"] else ""))
    if a["dirs"]:
        p(f"own samples: {len(a['samples'])} listed" + (f"; slots {a['ignored_slots']} are filled in but fail zu_load's checks" if a["ignored_slots"] else "")
          + ("; the bank room is given to samples (flag bit 0)" if a["bank_flag"] else ""))
    for lo, hi, who in REGIONS:
        part = [r for r in rows if lo <= r["addr"] < hi]
        c = {k: sum(r["kind"] == k for r in part) for k in KINDS}
        p(f"\n{lo:#07x}-{hi - 1:#07x}  {who}\n    " + ", ".join(f"{c[k]} {k.upper() if k == 'unidentified' else k}" for k in KINDS))
        run = None
        for r in part + [None]:                      # runs of sectors that are the same kind for the same reason
            key = r and (r["kind"], tuple(r["what"]), tuple(r["hint"]))
            if run and key != run[0]:
                k, what, hints = run[0]
                tail = "; ".join(what) if what else ("LOOKS LIKE, NOT PROVEN: " + "; ".join(hints)) if hints else ""
                p(f"    {run[1]:#07x}-{run[2] + SECT - 1:#07x} {k:12} {tail}".rstrip())
                run = None
            if r:
                run = [key, run[1] if run else r["addr"], r["addr"]]
    if a["records"]:
        p("\nWARNING: update record(s) at", ", ".join(f"{x:#x}" for x in a["records"]),
          "- the boot code starts the update loader while one is there.")
    p("\nNothing here is a licence to erase. 'verified': the structure's own checksum holds, as its firmware checks it."
      "\n'referenced': named by a verified structure, no checksum of its own. 'blank': all 0xFF now, not a promise that"
      "\nno firmware will write there. 'unidentified': data whose owner this tool cannot prove, in use for all it knows."
      "\nThe line beside each range says who writes there in this repository's sources, not what the dump shows.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["dump", "analyze"])
    ap.add_argument("file")
    ap.add_argument("--port", help="the ALSA raw MIDI device (default: the one card named Felucca)")
    ap.add_argument("--sim", help="tests/zp12_link_host.c built on the host, in place of an FM-1")
    ap.add_argument("--unverified", action="store_true", help="analyze: a file with no .json beside it, marked so")
    o = ap.parse_args(argv)
    io, saved = None, False
    try:
        if o.mode == "dump":
            if o.sim and o.port:
                raise DumpError("--sim or --port, not both")
            for p in (o.file, o.file + ".json"):     # (before the port is opened)
                if os.path.lexists(p):
                    raise DumpError(f"{p} exists: a dump is never written over")
            io = Sim(o.sim) if o.sim else RawMidi(o.port or find_port())
            cmd_dump(io, o.file, bool(o.sim), sys.stderr)
            saved = True
        img, meta = load_dump(o.file, o.unverified and o.mode == "analyze")
        report(analyze(img), meta)
    except DumpError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted" + ("" if saved else ": no dump was saved"), file=sys.stderr)
        return 130
    finally:
        if io:
            io.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

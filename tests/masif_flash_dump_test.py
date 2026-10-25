#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/masif_flash_dump.py without an FM-1: against the firmware's own link code (tests/zp12_link_host.c:
sp_link.c on a flash in RAM), with the replies lost, damaged and forged on the way. No clock, no chance: the
faults fall on numbered requests and the timeouts run on a counted clock.
  cc -O2 -Wall -Wno-unused-function -Ifirmware/src -o build/host/zp12_link_host tests/zp12_link_host.c -lm
  python3 tests/masif_flash_dump_test.py [build/host/zp12_link_host]"""
import contextlib, hashlib, io, json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import masif_flash_dump as fd

SIM = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/host/zp12_link_host"))
TOOL = os.path.join(ROOT, "tools/masif_flash_dump.py")
LO, HI, SECT, CHUNK = fd.LO, fd.HI, fd.SECT, fd.CHUNK
NREQ = (HI - LO) // CHUNK                            # READs a pass; request 0 is HELLO, pass 2 starts at NREQ + 1
bad = 0


def check(ok, what):
    global bad
    print(f"dump: {what:88} {'ok' if ok else 'FAIL'}")
    bad += not ok


def raw_frame(cmd, addr=0, n=0, data=b""):           # any message at all: only ever given to the simulator
    m = bytes([0x7D, 0x5A, 0x50, cmd, addr & 255, (addr >> 8) & 255, (addr >> 16) & 255, n & 255, n >> 8]) + data
    return b"\xF0" + fd.pack7(m + bytes([~sum(m[3:]) & 255])) + b"\xF7"


def reply(cmd, rc, addr, data):                      # a reply as the FM-1 would sign it
    m = bytes([0x7D, 0x5A, 0x50, cmd, rc, addr & 255, (addr >> 8) & 255, (addr >> 16) & 255, len(data) & 255, len(data) >> 8]) + data
    return b"\xF0" + fd.pack7(m + bytes([~sum(m[3:]) & 255])) + b"\xF7"


def body(w):                                         # a wire frame's bytes, unpacked
    return bytearray(fd.unpack7(w[1:-1]))


def wire(m):
    return b"\xF0" + fd.pack7(bytes(m)) + b"\xF7"


def resign(w, **f):                                  # a reply changed and signed again: the checksum will not show it
    m = body(w)
    r = {"cmd": m[3], "rc": m[4], "addr": m[5] | m[6] << 8 | m[7] << 16, "data": bytes(m[10:-1])}
    r.update(f)
    return reply(r["cmd"], r["rc"], r["addr"], r["data"])


class Clock:                                         # time that passes only when a wait finds nothing
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class Faulty:                                        # the simulator behind a wire that does as the plan says
    def __init__(self, sim, clock, plan=None):
        self.sim, self.clock, self.plan, self.n, self.q, self.sent = sim, clock, plan or {}, -1, [], []
        self.tries = {}

    def send(self, b):
        self.sent.append(bytes(b))
        self.sim.send(b)
        r = self.sim.recv(5.0)
        if not self.sent[:-1] or self.sent[-2] != b:
            self.n += 1                              # (a retry is the same request)
        t = self.tries[self.n] = self.tries.get(self.n, 0) + 1
        act = self.plan.get(self.n)
        out = act(r, t) if act else [r]
        self.q += [x for x in out if x]

    def recv(self, timeout):
        if self.q:
            return self.q.pop(0)
        self.clock.t += timeout
        return b""


def first(f):                                        # the fault on the first try only: the retry gets the truth
    return lambda r, t: f(r) if t == 1 else [r]


def run(plan=None, tries=4):
    sim, clock = fd.Sim(SIM), Clock()
    io_ = Faulty(sim, clock, plan)
    link = fd.Link(io_, timeout=1.5, tries=tries, clock=clock)
    try:
        info, passes, diff = fd.dump(link)
        err = None
    except fd.DumpError as e:
        info, passes, diff, err = None, None, None, str(e)
    stats = sim.ctl("STATS").split()
    truth = bytes.fromhex(sim.ctl(f"DUMP {LO:x} {HI - LO:x}").decode())
    sim.close()
    return {"info": info, "passes": passes, "diff": diff, "err": err, "link": link, "io": io_, "stats": stats, "truth": truth}


def sent_ok(sent):                                   # every message that went out, decoded apart from the tool
    reads = []
    for b in sent:
        m = fd.unpack7(b[1:-1])
        if b[0] != 0xF0 or b[-1] != 0xF7 or len(m) != 10 or m[:3] != b"\x7DZP" or (~sum(m[3:9]) & 255) != m[9]:
            return None
        cmd, addr, n = m[3], m[4] | m[5] << 8 | m[6] << 16, m[7] | m[8] << 8
        if cmd == 1 and addr == 0 and n == 0:
            continue
        if cmd != 2 or addr < LO or addr + n > HI or n != CHUNK:
            return None
        reads.append(addr)
    return reads


# ---- 1. the clean dump, and what went over the wire
r = run()
check(r["err"] is None and r["info"] == {"proto": 3, "version": "zp12 1.5", "kit": 27}, "HELLO: protocol 3, 'zp12 1.5', 27 kit waves")
check(r["passes"] and all(p == r["truth"] and len(p) == HI - LO for p in r["passes"]) and not r["diff"],
      f"READ {LO:#x}..{HI - 1:#x}: {HI - LO} bytes, twice, as the flash itself")
reads = sent_ok(r["io"].sent)
want = list(range(LO, HI, CHUNK))
check(reads == want + want and len(r["io"].sent) == 2 * NREQ + 2,
      f"sent: HELLO, {NREQ} READs of {CHUNK} from {LO:#x} to {HI - CHUNK:#x} without a gap, again, HELLO; no more")
check(r["stats"] == [b"STATS", b"0", b"0"] and r["link"].retries == 0 and r["link"].ignored == 0, "nothing erased, nothing written, no retry")

# ---- 2. what the tool will not build or send
def refused(f):
    try:
        f()
        return False
    except fd.DumpError:
        return True


check(all(refused(lambda c=c: fd.frame(c, 0xA0000, 0)) for c in (0, 3, 4, 5, 6, 7, 8, 9, 10, 0x7F, 255)) and refused(lambda: fd.frame(2.0, LO, 512)),
      "frame(): ERASE, WRITE, HOLD, REBOOT, RELOAD, ASSIGN, PADS and unknown commands refused")
edges = ((LO - 1, 2), (LO - CHUNK, CHUNK), (0, 1), (HI - CHUNK + 1, CHUNK), (HI, 1), (HI - 1, 2), (LO, 0), (LO, CHUNK + 1),
         (LO + 0x1000000, CHUNK), (-1, 1), (LO, -1), (0x4000, CHUNK))
check(all(refused(lambda a=a, n=n: fd.frame(fd.READ, a, n)) for a, n in edges) and refused(lambda: fd.frame(fd.HELLO, LO, 0)),
      "frame(): a READ that leaves the range by a byte, wraps 24 bits or is longer than 512 refused")
check(fd.frame(fd.READ, LO, 1) and fd.frame(fd.READ, HI - CHUNK, CHUNK) and fd.frame(fd.READ, HI - 1, 1), "frame(): the range's first and last bytes are allowed")
forged = [raw_frame(c, 0xA0000, 0) for c in (3, 5, 6, 7, 8, 9)] + [raw_frame(4, 0xA0000, 4, b"\0\0\0\0"), raw_frame(2, LO - 1, 2),
          raw_frame(2, HI - 1, 2), raw_frame(2, 0x4000, 512), fd.frame(fd.HELLO) * 2, fd.frame(fd.HELLO)[:-1], b"\x90\x40\x7F", b"",
          fd.frame(fd.HELLO) + b"\xF0", b"\xF0" + bytes([0x80]) + fd.frame(fd.HELLO)[2:]]
check(all(refused(lambda b=b: fd.wire_guard(b)) for b in forged) and fd.wire_guard(fd.frame(fd.READ, LO, CHUNK)),
      "port guard: forged ERASE / WRITE / HOLD / REBOOT / out-of-range bytes refused before the port")
sim = fd.Sim(SIM)
check(refused(lambda: sim.send(raw_frame(3, 0xA0000))) and refused(lambda: sim.send(raw_frame(4, 0xA0000, 1, b"\0"))) and
      sim.ctl("STATS").split() == [b"STATS", b"0", b"0"], "the simulator's port: a forged ERASE and WRITE never reach it")
rd, wr = os.pipe()                                   # the real port class on a pipe: its send() must refuse too
port = fd.RawMidi.__new__(fd.RawMidi)
port.fd = wr
ok = refused(lambda: port.send(raw_frame(3, 0xA0000))) and refused(lambda: port.send(raw_frame(6)))
port.send(fd.frame(fd.READ, LO, CHUNK))
os.set_blocking(rd, False)
check(ok and os.read(rd, 4096) == fd.frame(fd.READ, LO, CHUNK), "RawMidi.send: only the allowed frame's bytes reach the device")
os.close(rd), os.close(wr)
src = open(TOOL).read()
check(len(re.findall(r"\.send\(", src)) == 1 and len(re.findall(r"os\.write\(", src)) == 1 and "frame(cmd, addr, n)" in src and
      set(re.findall(r"\.request\((\w+)", src)) == {"HELLO", "READ"},
      "source: one send, of what frame() built; request() is called with HELLO and READ only")

# ---- 3. the firmware's side of the range (asked of the simulator alone, past the tool's guard)
for a, n in ((0x92FFF, 2), (0x92000, 512), (0xFFE01, 512), (0x100000, 1), (0xFFFFFF, 512), (0x4000, 16)):
    sim.p.stdin.write(raw_frame(2, a, n).hex().encode() + b"\n")
    m = body(sim.recv(5.0))
    check(m[3] == 2 and m[4] == 1 and m[8] == 0 and m[9] == 0, f"firmware: READ {a:#x} + {n} refused (rc 1, no data)")
sim.close()

# ---- 4. replies lost and damaged: asked again, the dump still the flash
def cut(w): return [w[:len(w) // 2] + b"\xF7"]
def sumbad(w): m = body(w); m[-1] ^= 0x01; return [wire(m)]
def databad(w): m = body(w); m[200] ^= 0x40; return [wire(m)]
def longer(w): return [w[:-1] + b"\x00\x00\xF7"]
def magic(w): m = body(w); m[1] = ord("Y"); return [wire(m)]
def lenbad(w): m = body(w); m[8] = 0xFF; return [wire(m)]              # says 511 bytes, carries 512: the frame is off
def huge(w): return [b"\xF0" + bytes(2000) + b"\xF7"]
def noend(w): return [w[:-1]]
def note(w): return [w[:300] + b"\x90\x40\x7F" + w[300:]]             # a note inside the SysEx ends it
K = 5                                                # the request a fault falls on: READ at LO + 4 * 512
A = LO + (K - 1) * CHUNK
recover = [
    ("no reply (timeout)", lambda w: []),
    ("reply cut short", cut),
    ("checksum wrong", sumbad),
    ("a data bit turned, checksum stale", databad),
    ("two bytes too many", longer),
    ("not 7D 'Z' 'P'", magic),
    ("length field not the frame's length", lenbad),
    ("2000 bytes of SysEx", huge),
    ("reply without F7", noend),
    ("a note-on inside the reply", note),
    ("another command's reply (signed)", lambda w: [resign(w, cmd=9)]),
    ("HELLO's reply to a READ (signed)", lambda w: [reply(1, 0, 0, b"\x03zp12 1.5\x00\x1b")]),
    ("another address's reply (signed)", lambda w: [resign(w, addr=A + CHUNK)]),
    ("the address before (signed)", lambda w: [resign(w, addr=A - CHUNK)]),
    ("noise, no SysEx at all", lambda w: [b"\x90\x40\x7F\x80\x40\x00"]),
]
for name, f in recover:
    r = run({K: first(f)})
    check(r["err"] is None and all(p == r["truth"] for p in r["passes"]) and not r["diff"] and r["link"].retries == 1 and
          sent_ok(r["io"].sent) == want[:K] + [A] + want[K:] + want, f"fault: {name}: asked once more, the dump whole")
r = run({K: lambda w, t: [] if t < 4 else [w], 2 * NREQ: lambda w, t: sumbad(w) if t < 3 else [w]})
check(r["err"] is None and r["passes"][0] == r["truth"] == r["passes"][1] and r["link"].retries == 5, "fault: three timeouts, then two bad sums in pass 2: 5 retries, the dump whole")
r = run({K: lambda w, t: [w[:100], w[100:400], w[400:]]})
check(r["err"] is None and r["passes"][0] == r["truth"] and r["link"].retries == 0, "a reply in three pieces: taken, no retry")
r = run({K: lambda w, t: [w[:250] + b"\xF8\xFE" + w[250:]]})
check(r["err"] is None and r["passes"][0] == r["truth"] and r["link"].retries == 0, "MIDI clock and active sensing inside a reply: taken, no retry")
r = run({K: lambda w, t: [w, w, w]})
check(r["err"] is None and r["passes"][0] == r["truth"] and r["link"].retries == 0, "a reply three times: the spare ones dropped before the next request")
r = run({K: lambda w, t: [resign(w, addr=A - CHUNK, data=bytes(CHUNK)), w]})
check(r["err"] is None and r["passes"][0] == r["truth"] and r["link"].retries == 0 and r["link"].ignored == 1,
      "a late reply for the chunk before, then the right one: the late one's data not taken")

# ---- 5. what ends the dump with nothing
fatal = [
    ("no reply, four times", {K: lambda w, t: []}, "no answer"),
    ("checksum wrong, four times", {K: lambda w, t: sumbad(w)}, "no answer"),
    ("wrong address, four times (signed)", {K: lambda w, t: [resign(w, addr=A + CHUNK)]}, "no answer"),
    ("the FM-1 refuses (rc 1, signed)", {K: lambda w, t: [resign(w, rc=1, data=b"")]}, "error 1"),
    ("flash not ready (rc 9, signed)", {K: lambda w, t: [resign(w, rc=9, data=b"")]}, "error 9"),
    ("511 bytes for 512 (signed)", {K: lambda w, t: [resign(w, data=bytes(body(w)[10:-2]))]}, "511 of 512"),
    ("no bytes, rc 0 (signed)", {K: lambda w, t: [resign(w, data=b"")]}, "0 of 512"),
    ("in the last READ of pass 2", {2 * NREQ: lambda w, t: []}, f"{HI - CHUNK:#x}"),
    ("HELLO never answered", {0: lambda w, t: []}, "no answer"),
    ("HELLO from another firmware", {0: lambda w, t: [reply(1, 0, 0, b"\x03SLOOP 2.3\x00\x1b")]}, "not as zp12"),
    ("HELLO with no text", {0: lambda w, t: [reply(1, 0, 0, b"")]}, "not as zp12"),
    ("HELLO with an error code", {0: lambda w, t: [resign(w, rc=7)]}, "not as zp12"),
    ("another version at the end", {2 * NREQ + 1: lambda w, t: [reply(1, 0, 0, b"\x03zp12 1.6\x00\x1b")]}, "at the end"),
]
for name, plan, msg in fatal:
    r = run(plan)
    check(r["passes"] is None and r["err"] and msg in r["err"] and sent_ok(r["io"].sent) is not None and r["stats"][1:] == [b"0", b"0"],
          f"fatal: {name}: stopped, no image")


def swap(w):                                         # two bytes change places: the 8-bit sum is the same
    m = body(w); m[10], m[11] = m[11], m[10]; return [wire(m)]


def flip(w):
    m = body(w); return [resign(w, data=bytes([m[10] ^ 0xFF]) + bytes(m[11:-1]))]


D = (0xA0000 - LO) // CHUNK + 1                      # a chunk with data in it (the simulator's banks)
for name, plan in (("pass 2 reads another byte (signed)", {NREQ + D: lambda w, t: flip(w)}),
                   ("pass 1 has two bytes swapped (the sum cannot see it)", {D: lambda w, t: swap(w)}),
                   ("the last chunk of pass 2 differs (signed)", {2 * NREQ: lambda w, t: flip(w)})):
    r = run(plan)
    at = HI - SECT if 2 * NREQ in plan else 0xA0000
    check(r["err"] is None and r["link"].retries == 0 and r["diff"] == [at], f"passes: {name}: sector {at:#x} reported")

# ---- 6. files: never a part of a dump, never over one that is there, nothing of the flash on the screen
tmp = tempfile.mkdtemp(prefix="masif_dump_test_")
out = os.path.join(tmp, "a.bin")


def cli(*a):
    p = subprocess.run([sys.executable, TOOL, *a], capture_output=True)
    return p.returncode, p.stdout + p.stderr


def quiet_dump(plan, path):                          # cmd_dump as the CLI runs it, through a faulty wire
    sim, clock = fd.Sim(SIM), Clock()
    real, err = fd.Link, None
    fd.Link = lambda io_: real(Faulty(io_, clock, plan), clock=clock)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            fd.cmd_dump(sim, path, True)
    except (fd.DumpError, KeyboardInterrupt) as e:
        err = e
    finally:
        fd.Link = real
        sim.close()
    return err


def boom(w, t):
    raise KeyboardInterrupt


for name, plan in (("the passes differ", {NREQ + D: lambda w, t: flip(w)}), ("the link dies in pass 2", {NREQ + 300: lambda w, t: []}),
                   ("Ctrl-C in pass 2", {NREQ + 300: boom}), ("the simulator ends", {40: lambda w, t: (_ for _ in ()).throw(fd.DumpError("the simulator ended"))})):
    e = quiet_dump(plan, out)
    check(e is not None and os.listdir(tmp) == [], f"files: {name}: an error, and no file at all")
real_write, calls = fd.write_new, []


def fail_second(path, data):                         # the disk fills between the image and its .json
    calls.append(path)
    if len(calls) == 2:
        raise OSError(28, "No space left on device")
    real_write(path, data)


fd.write_new = fail_second
try:
    quiet_dump(None, out)
    e = None
except OSError as x:
    e = x
fd.write_new = real_write
check(e is not None and calls == [out, out + ".json"] and os.listdir(tmp) == [], "files: the .json cannot be written: the image is taken away again")

rc, text = cli("dump", out, "--sim", SIM)
img = open(out, "rb").read() if os.path.exists(out) else b""
meta = json.load(open(out + ".json")) if os.path.exists(out + ".json") else {}
check(rc == 0 and sorted(os.listdir(tmp)) == ["a.bin", "a.bin.json"] and len(img) == HI - LO and img == r["truth"], "files: a dump through the CLI: the image and its .json, no .part left")
check(meta.get("sha256") == hashlib.sha256(img).hexdigest() and meta.get("complete") is True and meta.get("range") == [LO, HI] and
      meta.get("size") == HI - LO and meta.get("simulated") is True and meta.get("passes") == 2 and
      meta.get("sector_crc32") == [zlib.crc32(img[s:s + SECT]) for s in range(0, len(img), SECT)], "files: the .json: range, size, SHA-256, a CRC-32 per sector, 'simulated'")
check(all(os.stat(p).st_mode & 0o777 == 0o600 for p in (out, out + ".json")), "files: both mode 0600")
check(b"SIMULATED" in text, "report: a simulated dump is called so")


def leaks(text):                                     # 6 bytes of any sector that is not blank, raw or as hex
    low = text.lower().replace(b" ", b"").replace(b":", b"")
    for s in range(0, len(img), SECT):
        sec = img[s:s + SECT]
        if sec != b"\xFF" * SECT:
            for o in (0, 16, 256, 2048):
                if sec[o:o + 6] == b"LOADER":            # (ota.c's fixed file name: the report says it by name)
                    continue
                if sec[o:o + 6] in text or sec[o:o + 6].hex().encode() in low:
                    print(f"      leak: sector {LO + s:#x} + {o}")
                    return True
    return False


rc2, text2 = cli("analyze", out)
check(rc2 == 0 and not leaks(text) and not leaks(text2) and not leaks(json.dumps({k: v for k, v in meta.items()}).encode()),
      "no flash bytes in what dump and analyze print, raw or hex, nor in the .json")
before = {p: (open(os.path.join(tmp, p), "rb").read(), os.stat(os.path.join(tmp, p)).st_mtime_ns) for p in os.listdir(tmp)}
same = lambda: before == {p: (open(os.path.join(tmp, p), "rb").read(), os.stat(os.path.join(tmp, p)).st_mtime_ns) for p in os.listdir(tmp)}
rc, text = cli("dump", out, "--sim", SIM)
check(rc == 1 and b"never written over" in text and same(), "files: a dump that exists is not written over (exit 1)")
only_json = os.path.join(tmp, "b.bin")
open(only_json + ".json", "w").write("{}")
rc, text = cli("dump", only_json, "--sim", SIM)
check(rc == 1 and not os.path.exists(only_json) and open(only_json + ".json").read() == "{}", "files: a .json that exists is not written over either")
os.unlink(only_json + ".json")
open(only_json + ".part", "w").write("x")
e = quiet_dump(None, only_json)
check(e is not None and sorted(os.listdir(tmp)) == ["a.bin", "a.bin.json", "b.bin.part"] and open(only_json + ".part").read() == "x", "files: a .part left by another run is not touched, and stops the dump")
os.unlink(only_json + ".part")
link_to = os.path.join(tmp, "c.bin")
os.symlink(out, link_to)                             # a name that leads to the dump
rc, text = cli("dump", link_to, "--sim", SIM)
check(rc == 1 and os.path.islink(link_to) and open(out, "rb").read() == img, "files: a symlink to a dump is not written through")
os.unlink(link_to)
check(refused(lambda: fd.write_new(out, b"x")) and open(out, "rb").read() == img and not os.path.exists(out + ".part"), "files: write_new() itself will not replace a file")
rc, text = cli("dump", os.path.join(tmp, "d.bin"), "--port", out)
check(rc == 1 and b"not an ALSA raw MIDI device" in text and open(out, "rb").read() == img and not os.path.exists(os.path.join(tmp, "d.bin")),
      "files: --port given a plain file (the dump itself): not opened, not written into")
rc, text = cli("dump", os.path.join(tmp, "d.bin"), "--port", "/dev/null")
check(rc == 1 and b"not an ALSA raw MIDI device" in text, "files: --port /dev/null: not opened")

# a capture that is not whole must not pass for one
part = os.path.join(tmp, "p.bin")
open(part, "wb").write(img[:300000])
rc, text = cli("analyze", part)
check(rc == 1 and b"totals" not in text, "analyze: a file with no .json is refused")
rc, text = cli("analyze", part, "--unverified")
check(rc == 1 and b"not a whole dump" in text and b"totals" not in text, "analyze: a short file is refused even with --unverified")
shutil.copy(out + ".json", part + ".json")
rc, text = cli("analyze", part)
check(rc == 1 and b"totals" not in text, "analyze: a short file beside a whole dump's .json is refused")
changed = bytearray(img); changed[0x1234] ^= 1
open(part, "wb").write(changed)
rc, text = cli("analyze", part)
check(rc == 1 and b"totals" not in text, "analyze: one bit changed since the dump: refused (SHA-256)")
os.unlink(part + ".json")
rc, text = cli("analyze", part, "--unverified")
check(rc == 0 and b"UNVERIFIED FILE" in text, "analyze --unverified: a file of the right length is read, and called unverified")
m2 = dict(meta); m2["complete"] = False
open(part, "wb").write(img); json.dump(m2, open(part + ".json", "w"))
rc, text = cli("analyze", part)
check(rc == 1, "analyze: a .json that does not say 'complete' is refused")
open(part + ".json", "w").write("{")
rc, text = cli("analyze", part)
check(rc == 1 and b"Traceback" not in text, "analyze: a damaged .json is refused, plainly")
shutil.rmtree(tmp)

# ---- 7. the report: only what a checksum proves is named
def blank():
    return bytearray(b"\xFF" * (HI - LO))


def put(b, a, d):
    b[a - LO:a - LO + len(d)] = d


def store(b, i, gen, n, ver=6, good=True):
    d = bytes((x * 7 + i) & 255 for x in range(n))
    put(b, 0xC4000 + i * 0xA000, struct.pack("<5I", 0x3231505A, ver, gen, n, zlib.crc32(d) ^ (0 if good else 1)) + d)


def directory(b, i, gen, slots, flags=0, good=True):
    s = b"".join(struct.pack("<IIHBB8s", *x) for x in slots).ljust(24 * 20, b"\0")
    put(b, 0xD8000 + i * 0x1000, struct.pack("<4I", 0x4D53505A, gen, flags, zlib.crc32(s) ^ (0 if good else 1)) + s)


def kinds(a):
    return {r["addr"]: r["kind"] for r in a["sectors"]}


b = blank()
k = kinds(fd.analyze(bytes(b)))
check(set(k.values()) == {"blank"} and len(k) == (HI - LO) // SECT, "analyze: an erased flash: 109 sectors, all blank")
store(b, 0, 7, 9000); store(b, 1, 8, 5000)
put(b, 0xC4000 + 0x4000, b"old tail of a longer save")            # inside copy 0's 40 KiB, past its record
a = fd.analyze(bytes(b)); k = kinds(a)
check([k[x] for x in (0xC4000, 0xC5000, 0xC6000, 0xC7000, 0xC8000)] == ["verified"] * 3 + ["blank", "unidentified"] and
      k[0xCE000] == k[0xCF000] == "verified" and k[0xD0000] == "blank" and a["live_store"] == 1,
      "analyze: store copies with a good CRC verified to their length; old data behind one stays unidentified")
b2 = blank(); store(b2, 0, 7, 9000, good=False); store(b2, 1, 3, 100, ver=0); a = fd.analyze(bytes(b2)); k = kinds(a)
check(k[0xC4000] == k[0xC5000] == k[0xCE000] == "unidentified" and not a["stores"] and a["sectors"][(0xC4000 - LO) // SECT]["hint"] and a["live_store"] is None,
      "analyze: a store head whose CRC or version fails: unidentified, with a hint, not verified")
b2 = blank(); store(b2, 0, 7, 0xA000); k = kinds(fd.analyze(bytes(b2)))
check(k[0xC4000] == "unidentified", "analyze: a store length past its 40 KiB: unidentified")
slots = [(0xDA000, 2000, 26040, 0, 0, b"KICK\0\0\0\0"),        # 3000 bytes: one sector
         (0xEA800, 6000, 27500, 1, 0, b"SECRETNM"),            # 9000 bytes from mid-sector: three sectors
         (0xDC000, 2000, 26040, 0, 0, b"OUTSIDE\0"),           # not a room zu_load allows
         (0xFB000, 4000, 26040, 0, 0, b"TOOLONG\0"),           # runs past 0xFC000
         (0xE5000, 2000, 5000, 0, 0, b"RATE\0\0\0\0"),         # a rate zu_load refuses
         (0xE6000, 1, 26040, 0, 0, b"ONE\0\0\0\0\0")]
directory(b, 0, 4, slots); directory(b, 1, 5, slots[:2], flags=1)
put(b, 0xDC000, b"\x5A" * 100); put(b, 0xFB000, b"\x11" * 100); put(b, 0xDA000, b"\x22" * 3000); put(b, 0xEA800, b"\x33" * 9000)
a = fd.analyze(bytes(b)); k = kinds(a)
check(k[0xD8000] == k[0xD9000] == "verified" and a["live_dir"] == 1 and a["bank_flag"] and [s["slot"] for s in a["samples"]] == [0, 1] and
      [k[x] for x in (0xDA000, 0xDB000, 0xEA000, 0xEB000, 0xEC000, 0xED000)] == ["referenced", "blank", "referenced", "referenced", "referenced", "blank"],
      "analyze: the newer good directory counts; its samples' sectors are 'referenced', not 'verified'")
directory(b, 1, 3, slots)
a = fd.analyze(bytes(b)); k = kinds(a)
check(a["live_dir"] == 0 and a["ignored_slots"] == [2, 3, 4, 5] and k[0xDC000] == k[0xFB000] == "unidentified" and k[0xE5000] == k[0xE6000] == "blank",
      "analyze: slots zu_load would skip (room, length, rate) claim nothing: their sectors stay unidentified")
directory(b, 0, 9, slots, good=False)
a = fd.analyze(bytes(b)); k = kinds(a)
check(k[0xD8000] == "unidentified" and a["live_dir"] == 1 and len(a["dirs"]) == 1, "analyze: a directory whose CRC fails is unidentified; the other copy counts")
rec = bytearray(80); rec[2:8] = bytes([0x0D, 0x5A, 0x01, 0x5A, 0x41, 0x54]); rec[8:20] = b"ota-FM-1_015"; rec[72:76] = struct.pack("<I", 0xE0000)
rec[0:2] = struct.pack("<H", fd.crc16(rec[2:80]))
put(b, 0xE4F00, rec)
bad_rec = bytearray(rec); bad_rec[30] ^= 1
put(b, 0xF7F00, bad_rec)
a = fd.analyze(bytes(b)); k = kinds(a)
check(a["records"] == [0xE4F00] and k[0xE4000] == "verified" and k[0xF7000] == "unidentified", "analyze: an update record with a good CRC-16 is found; one with a bad CRC is not called one")
head = bytearray(32); head[2:4] = b"\xFF\xFF"; head[4:8] = struct.pack("<I", 0xE0000); head[8:12] = struct.pack("<I", 0x3000 + 32)
head[12] = 0x41; head[13] = 0xFF; head[14:16] = b"\xFF\xFF"; head[16:26] = b"LOADER.BIN"; head[0:2] = struct.pack("<H", fd.crc16(head[2:32]))
put(b, 0xE0000, head + b"\x77" * 0x3000)
bad_head = bytearray(head); bad_head[8] ^= 4
put(b, 0xF2000, bad_head)
a = fd.analyze(bytes(b)); k = kinds(a)
check([k[x] for x in (0xE0000, 0xE1000, 0xE2000, 0xE3000)] == ["verified", "referenced", "referenced", "referenced"] and k[0xF2000] == "unidentified",
      "analyze: LOADER.BIN head with a good CRC-16 verified, its body referenced; a bad head unidentified")
data = bytes(range(256)) * 20
fs = struct.pack("<4sHBB8sII", b"FSMP", 1, 1, 0, b"USR1\0\0\0\0", len(data), zlib.crc32(data)).ljust(512, b"\0") + data
put(b, 0x98000, fs); put(b, 0x9D000, fs[:600])
a = fd.analyze(bytes(b)); k = kinds(a)
check(k[0x98000] == k[0x99000] == "verified" and k[0x9A000] == "blank" and k[0x9D000] == "unidentified", "analyze: an 'FSMP' slot is verified only with its data's CRC-32")
put(b, 0x93000, b"VM\0 some vendor file system"); put(b, 0xFF000, b"\xDE\xAD\xBE\xEF\x12\x34\x56\x78 key and mac")
put(b, 0xE9000, b"BTIF\x00\x11\x22\x33\x44\x55\x66"); put(b, 0xB0000, bytes(4096))
a = fd.analyze(bytes(b)); k = kinds(a)
check(k[0x93000] == k[0xFF000] == k[0xE9000] == k[0xB0000] == "unidentified", "analyze: vendor sectors, and a sector of zeros, are unidentified (not blank, not free)")
check(sum(a["totals"].values()) == (HI - LO) // SECT and all(r["kind"] in fd.KINDS for r in a["sectors"]) and
      all(bool(r["what"]) == (r["kind"] in ("verified", "referenced")) for r in a["sectors"]), "analyze: every sector has one kind, and a reason if it is named")
text = io.StringIO()
fd.report(a, None, text)
text = text.getvalue()
img = bytes(b)
check(not leaks(text.encode()) and "SECRETNM" not in text and "KICK" not in text and "deadbeef" not in text.lower(),
      "report: no sector bytes and no sample names, with every recogniser at work")
low = text.lower()
check("unverified file" in low and "nothing here is a licence to erase" in low and not re.search(r"\b(safe|free) to (erase|use|write)", low) and
      "looks like, not proven" in low and "warning: update record" in low, "report: unverified, unproven and the update record are said; nothing is called safe to erase")
check(refused(lambda: fd.analyze(img[:-1])) and refused(lambda: fd.analyze(img + b"\0")), "analyze: an image of another length is refused")

# ---- 8. the numbers above are the firmware's
def has(path, *pats):
    s = open(os.path.join(ROOT, path)).read()
    return all(re.search(p, s) for p in pats)


check(has("firmware/src/sp_link.c", r"#define ZL_MAX 512u", r"case 1: \{", r"case 2:\n\s+if \(!\(FL_IN\(a, len, 0x93000u, 0x100000u\) \|\| ZL_APP_READ\(a, len\)\)"),
      "source: sp_link.c still has HELLO = 1, READ = 2 of up to 512 bytes inside 0x93000..0xFFFFF (and MASIF's app-area room)")
check(has("firmware/src/sp_store.c", r"#define ZS_BASE 0xC4000u", r"#define ZS_SLOT 0xA000u", r"#define ZS_MAGIC 0x3231505Au", rf"#define ZS_VER {fd.ZS_VER}u",
          r"typedef struct \{ uint32_t magic, ver, gen, len, crc; \} zs_head_t;"), "source: sp_store.c's head, rooms and ZS_VER are the analyzer's")
check(has("firmware/src/sp_samples.c", r"#define ZU_DIR0 0xD8000u", r"#define ZU_MAGIC 0x4D53505Au", r"uint32_t off, n; uint16_t rate; uint8_t flags, rsv; char name\[8\];",
          r"s->n < 2u \|\| s->n > 400000u \|\| s->rate < 8000u \|\| s->rate > 48000u",
          r"0xA0000u, 0xC4000u\) \|\| FL_IN\(off, bytes, 0xDA000u, 0xDC000u\) \|\|\s+FL_IN\(off, bytes, 0xE5000u, 0xE9000u\) \|\| FL_IN\(off, bytes, 0xEA000u, 0xFC000u\)") and
      has("tests/zp12_link_host.c", r"#define SP_NUSER 24"), "source: sp_samples.c's directory, slot checks and rooms are the analyzer's")
check(has("firmware/src/ota.c", r"#define OTA_AREA 0x000E0000u", r"#define OTA_AREA_LEN 0x5000u", r"ota_wr16\(parm \+ 6, 0x5441u\)", r"ota_crc16\(parm \+ 2, 78, 0\)",
          r'NAME\[16\] = "LOADER.BIN"', r"ota_crc16\(h \+ 2, 30, 0\)") and
      has("firmware/loader/ldr_core.c", r"ota_rd16\(r \+ 6\) == 0x5441u && ota_rd16\(r\) && ota_rd16\(r\) == ota_crc16\(r \+ 2, 78, 0\)"),
      "source: ota.c's and ldr_core.c's update record and loader head are the analyzer's")
check(has("firmware/src/zp12.c", r"quiet = !sq.playing && !zl_busy\(\)") and has("firmware/src/sp_link.c", r"zl_last = fm1_ms \? fm1_ms : 1u;\n\s+if \(!flash_ok"),
      "source: every link message still holds the autosave off, as the tool's text says")

print(f"\n{'ALL OK' if not bad else f'{bad} FAILED'}")
sys.exit(1 if bad else 0)

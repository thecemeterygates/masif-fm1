// SPDX-License-Identifier: GPL-3.0-only
// MASIF's side of the USB link without an FM-1: web/zp12link.js against the firmware's link code built with
// MASIF_ENABLE_TRACKER=1 (tests/zp12_link_host.c: sp_link.c + sp_samples.c on a flash in RAM). What MASIF changes:
// the sample rooms are all of the data flash but zp12's store, the update loader's staging and the vendor's areas;
// nothing that looks like an update record may be written.
//   cc -O2 -Wall -Wno-unused-function -DMASIF_ENABLE_TRACKER=1 -Ifirmware/src -o build/host/masif_link_host tests/zp12_link_host.c -lm
//   node tests/masif_link_test.mjs
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import readline from "node:readline";
const require = createRequire(import.meta.url);
const Z = require("../web/zp12link.js");

const dev = spawn("build/host/masif_link_host", [], { stdio: ["pipe", "pipe", "inherit"] });
const lines = readline.createInterface({ input: dev.stdout });
let hook = null;
const hex = (b) => b.map((x) => x.toString(16).padStart(2, "0")).join("");
const link = new Z.Link((bytes) => dev.stdin.write(hex(bytes) + "\n"), { timeout: 300 });
lines.on("line", (l) => {
  if (l === "REBOOT") return;
  if (hook) { const h = hook; hook = null; h(l); return; }
  link.receive(l.match(/../g).map((x) => parseInt(x, 16)));
});
const raw = (cmd) => new Promise((ok) => { hook = ok; dev.stdin.write(cmd + "\n"); });
const dump = async (a, n) => Uint8Array.from((await raw(`DUMP ${a.toString(16)} ${n.toString(16)}`)).match(/../g).map((x) => parseInt(x, 16)));
let bad = 0;
const check = (ok, what) => { console.log(`masif link: ${what.padEnd(86)} ${ok ? "ok" : "FAIL"}`); bad += !ok; };
const refused = async (f) => { try { await f(); return false; } catch (e) { return /error 2/.test(e.message); } };

const info = await link.hello();
check(info.proto === 3 && info.rooms === 1 && info.kit === 27, `HELLO says these are MASIF's rooms (rooms ${info.rooms}, "${info.version}")`);

// the rooms: the data flash's 268 KiB and the app area past the firmware (here past 84 KiB of it: 480 KiB)
check(info.app && info.app.lo === 0x1a000 && info.app.hi === 0x92000 && !info.app.ok, "HELLO: the app area's room, 0x1A000-0x92000 (its marker not there yet)");
let d = await Z.readDir(link);
const r0 = Z.room(d, "masif");
check(r0.total === (268 + 480) * 1024 && r0.free === r0.total, `room: ${r0.total / 1024} KiB for samples, all free (268 of the data flash, 480 of the app area)`);
check(Math.abs(r0.longest / 26040 - 12.58) < 0.01, `the longest single sample: ${(r0.longest / 26040).toFixed(2)} s at 26.04 kHz (the app area's room)`);
for (const [a, what] of [[0x93000, "the first data sector"], [0x9f000, "SLOOP's old store"], [0xdc000, "sloopDX's old presets"], [0xdf000, "the sector below the loader's staging"],
                         [0xfc000, "Felucca's old globals"]]) {
  await link.erase(a);
  await link.write(a, Array(512).fill(0x33));
  check((await dump(a, 512)).every((x) => x === 0x33), `ERASE + WRITE at 0x${a.toString(16)} (${what}): MASIF's now`);
  await link.erase(a);
}
for (const [a, what] of [[0x92000, "the firmware itself"], [0xe0000, "the update loader's staging"], [0xe4000, "its last sector"],
                         [0xe9000, "the vendor's BTIF"], [0xff000, "the vendor's key"]]) {
  const before = await dump(a, 512).catch(() => null);
  const no = await refused(() => link.erase(a)) && await refused(() => link.write(a, Array(512).fill(0)));
  const after = await dump(a, 512).catch(() => null);
  check(no && (before === null || before.every((x, i) => x === after[i])), `ERASE and WRITE at 0x${a.toString(16)} (${what}): refused, nothing changed`);
}

// samples: more than zp12's rooms hold, placed by the web tool, taken by the firmware
const y = Int16Array.from({ length: 120000 }, (_, i) => Math.round(1500 * Math.sin(i / 9)));      // 4.6 s
const s0 = await Z.upload(link, d, { name: "BREAK", y, rate: 26040 }, "masif");
let w = (await raw("WAVES")).trim().split(" ");
check(d.slots[s0].off === 0x93000 && w.includes("BREAK") && +w[2] === y.length, `a ${(y.length / 26040).toFixed(1)} s sample: stored from 0x93000, the firmware plays it`);
const s1 = await Z.upload(link, d, { name: "HIT", y: y.subarray(0, 9000), rate: 26040 }, "masif");
check(d.slots[s1].off === 0xbf000, `a short one after it: into the smallest piece that fits (0x${d.slots[s1].off.toString(16)})`);
const back = await Z.fetchSample(link, d.slots[s0]);
check(back.length === y.length && back.every((v, i) => v === y[i]), "read back: as uploaded");
let e = null;
try { await Z.upload(link, d, { name: "TOOBIG", y: new Int16Array(330000) }, "masif"); } catch (x) { e = x; }
check(e && /not enough room/.test(e.message), "one that fits no piece: refused before anything is written");
const r1 = Z.room(d, "masif");
check(r1.free === r0.free - (Math.ceil(Z.bytesOf(y.length) / 4096) + Math.ceil(Z.bytesOf(9000) / 4096)) * 4096, `room left: ${(r1.free / 1024).toFixed(0)} KiB`);

// the app area's room: a sample only it holds; its marker goes in first; a firmware update takes both
{
  const y10 = Int16Array.from({ length: 260400 }, (_, i) => Math.round(1200 * Math.sin(i / 7)));    // 10 s
  const sa = await Z.upload(link, d, { name: "LONG", y: y10, rate: 26040 }, "masif");
  const mk = await dump(0x19000, 16), ww = (await raw("WAVES")).trim().split(" ");
  check(d.slots[sa].off >= 0x1a000 && d.slots[sa].off + Z.bytesOf(y10.length) <= 0x92000 && ww.includes("LONG"),
        `a 10 s sample: into the app area (0x${d.slots[sa].off.toString(16)}), the firmware plays it`);
  check(String.fromCharCode(...mk.subarray(0, 4)) === "MSAR" && (await link.hello()).app.ok, "its marker in the sector before the room; HELLO says so");
  const back10 = await Z.fetchSample(link, d.slots[sa]);
  check(back10.length === y10.length && back10.every((v, i) => v === y10[i]), "read back from the app area: as uploaded");
  for (const [a, what] of [[0x19000, "the marker's sector"], [0x18000, "the firmware below the room"]]) {
    const before = await dump(a, 512);
    const no = await refused(() => link.erase(a)) && await refused(() => link.write(a, Array(512).fill(0)));
    check(no && (await dump(a, 512)).every((x, i) => x === before[i]), `ERASE and WRITE at 0x${a.toString(16)} (${what}): refused`);
  }
  const fa = await Z.backup(link, ["masif"]);
  check(Z.readBackup(fa).sectors.has(d.slots[sa].off), "a backup holds the app area's room too");
  await raw("UPDATE");                                                        // (as a firmware update: the app area rewritten)
  await link.reload();
  const wu = (await raw("WAVES")).trim().split(" ");
  check(!wu.includes("LONG") && wu.includes("BREAK") && !(await link.hello()).app.ok, "after an update: its marker gone, the sample there is not played, the others are");
  const du = await Z.readDir(link);
  check(!du.slots[sa] && du.slots[s0], "and the web app's directory leaves it out (SYNC writes it again)");
  await Z.restore(link, fa, ["masif"]);
  await link.reload();
  check((await raw("WAVES")).trim().split(" ").includes("LONG") && (await link.hello()).app.ok, "the backup restored: it is back, marker and all");
  d = await Z.readDir(link);
  await Z.remove(link, d, sa);
}

// a backup of everything MASIF keeps, and back
const f = await Z.backup(link, ["masif"]);
await Z.remove(link, d, s0);
await link.erase(0x93000);
await Z.restore(link, f, ["masif"]);
await link.reload();
w = (await raw("WAVES")).trim().split(" ");
check(Z.readBackup(f).parts.has("masif") && w.includes("BREAK") && w.includes("HIT"), "backup of MASIF's part + restore: the samples come back");

// 8 bit: a byte a sample (flags bit 2), half as much time again in the same room
{
  const r8 = Z.room(d, "masif");
  const y8 = Int16Array.from({ length: 40000 }, (_, i) => Math.round(1900 * Math.sin(i / 11)));
  const s8 = await Z.upload(link, d, { name: "EIGHT", y: y8, rate: 26040, bits: 8 }, "masif");
  const wv = (await raw("WAVES")).trim().split(" "), k = wv.indexOf("EIGHT");
  check(k > 0 && +wv[k - 2] === y8.length && +wv[k + 1] === 4 && Z.bitsOf(d.slots[s8]) === 8, "an 8-bit sample: the firmware takes it, its flag says 8 bit");
  const at8 = await dump(d.slots[s8].off, y8.length);
  check(at8.every((b, i) => ((b ^ 0x80) - 0x80) === Math.max(-128, Math.min(127, Math.round(y8[i] / 16)))), "in the flash: one signed byte a sample");
  const b8 = await Z.fetchSample(link, d.slots[s8]);
  check(b8.length === y8.length && b8.every((v, i) => Math.abs(v - y8[i]) <= 8), "read back: the same sound within 8 bit's step");
  const r9 = Z.room(d, "masif");
  check(r8.free - r9.free === Math.ceil(40000 / 4096) * 4096 && r9.longest8 === Math.floor(r9.longest * 1.5) + (r9.longest8 - Math.floor(r9.longest * 1.5)),
        `it took ${(r8.free - r9.free) / 1024} KiB (12 bit would have taken ${Math.ceil(Z.bytesOf(40000) / 4096) * 4}); the longest sample that still fits: ${(r9.longest8 / 26040).toFixed(2)} s in 8 bit, ${(r9.longest / 26040).toFixed(2)} s in 12`);
  await Z.remove(link, d, s8);
}

// songs: 16 slots on the device, saved from and loaded into the song it has in memory
const sum = async (cmd) => (await raw("SONG " + cmd)).split(" ");
let sl = await link.songs();
check(sl.slots.length === 16 && sl.slots.every((x) => x === null) && sl.free === 13, `SONG list: 16 empty slots, ${sl.free} free sectors of the song store`);
const demo = await sum("demo");
await link.songSave(2, "my demo!x");
sl = await link.songs();
check(sl.slots[2] && sl.slots[2].name === "MY DEMO!" && sl.slots[2].sectors === 1 && sl.slots[2].bytes > 800 && sl.free === 12,
      `SONG save: the demo song in slot 3 as "${sl.slots[2]?.name}", ${sl.slots[2]?.bytes} B`);
const cleared = await sum("clear");
await link.songLoad(2);
const loaded = await sum("sum");
check(cleared[1] !== demo[1] && loaded[1] === demo[1] && +loaded[2] === +cleared[2] + 1, "SONG load: the song in memory is the saved one again, and the player is told");
e = null;
try { await link.songLoad(5); } catch (x) { e = x; }
check(e && /error 15/.test(e.message) && (await sum("sum"))[1] === demo[1], "SONG load of an empty slot: refused, the song in memory stays");
await raw("SONG play");
let busy = 0;
for (const f of [() => link.songSave(4, "X"), () => link.songLoad(2), () => link.songDelete(2)]) { try { await f(); } catch (x) { busy += /error 6/.test(x.message); } }
check(busy === 3 && (await link.songs()).slots[2] !== null, "while it plays: save, load and delete are refused (the list is not)");
await raw("SONG stop");
const fs = await Z.backup(link, ["masif"]);
await link.songDelete(2);
check((await link.songs()).slots[2] === null, "SONG delete: the slot is empty");
await Z.restore(link, fs, ["masif"]);
await link.reload();
sl = await link.songs();
check(sl.slots[2] && sl.slots[2].name === "MY DEMO!", "a backup of MASIF's part holds the songs too: restore + RELOAD brings the song back");

// nothing that the SPL would take for an update record: 80 bytes, 256 below a 4 KiB boundary
const crc16 = (b) => { let c = 0; for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; } return c; };
const rec = Array.from({ length: 80 }, (_, i) => (i * 37 + 5) & 255);
rec[6] = 0x41; rec[7] = 0x54;
const c = crc16(rec.slice(2));
rec[0] = c & 255; rec[1] = c >> 8;
const sect = 0xea000, at = sect + 0x1000 - 0x200;                    // the 512-byte chunk that holds bytes -256..-176
const chunk = Array(512).fill(0x77);
rec.forEach((x, i) => { chunk[256 + i] = x; });
await link.erase(sect);
await link.write(sect, Array(512).fill(0x11));                       // (something else in the sector, before)
e = null;
try { await link.write(at, chunk); } catch (x) { e = x; }
check(e && /error 5/.test(e.message), "a WRITE that would leave an update record's look-alike in the flash: refused (error 5)");
check((await dump(sect, 0x1000)).every((x) => x === 0xff), "... and its sector is erased: nothing of it is there at the next start");
const near = chunk.slice();
near[256 + 20] ^= 1;                                                 // (its CRC no longer fits: just data)
await link.write(at, near);
check((await dump(at, 512)).every((x, i) => x === near[i]), "the same bytes with one bit different are just data: written");
await link.erase(sect);

// a sample's own loop and play mode (the web app's): after the directory, read by the firmware, read back
{
  d = await Z.readDir(link);
  const yl = Int16Array.from({ length: 6000 }, (_, i) => Math.round(1000 * Math.sin(i / 7)));
  const sl = await Z.upload(link, d, { name: "LOOPED", y: yl, rate: 26040, loop: [1234, 5678], play: 4, taper: [32, 64] }, "masif");
  const lp = (await raw("LOOPS")).trim().split(" ").map(Number);
  const k = lp.findIndex((x, i) => i % 6 === 0 && x === sl);
  check(k >= 0 && lp[k + 1] === 1234 && lp[k + 2] === 5678 && lp[k + 3] === 4 && lp[k + 4] === 32 && lp[k + 5] === 64,
        "a loop, a play mode and the loop's tapers with the sample: the firmware has them, to the sample");
  const d2 = await Z.readDir(link);
  check(d2.slots[sl].loop?.[0] === 1234 && d2.slots[sl].loop?.[1] === 5678 && d2.slots[sl].play === 4 && d2.slots[sl].taper?.[1] === 64 && !d2.slots.some((s, i) => s && i !== sl && s.loop),
        "read back from the directory: the loop where it was, the others none");
  d2.slots[sl].loop = [10, 99999];                                    // (past the sample's end: no loop)
  await Z.writeDir(link, d2);
  const lp2 = (await raw("LOOPS")).trim().split(" ").map(Number);
  check(lp2[lp2.findIndex((x, i) => i % 6 === 0 && x === sl) + 2] === 0, "a loop past the sample's end is not written (none)");
  await Z.remove(link, d2, sl);
}

dev.stdin.end();
console.log(bad ? "MASIF LINK TEST FAILED" : "masif link test passed");
process.exit(bad ? 1 : 0);

// The samples' directory as the FM-1 writes it itself (firmware/src/masif_rec.c, MASIF's sampler page) is the one
// the web app reads and writes (web/zp12link.js parseDir / buildDir): host-tests/test_sampler.c records, renames and
// deletes on its RAM flash and leaves the directory's two sectors in a file; here the web app reads them back.
//   node tests/masif_pool_dir_test.mjs        (needs build/host/test_sampler)
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const Z = require("../web/zp12link.js");
const out = "build/host/pool_dir.bin";
execFileSync("build/host/test_sampler", [out], { stdio: "ignore" });
const b = new Uint8Array(readFileSync(out));
let bad = 0;
const check = (ok, what) => { console.log(`pool dir: ${what.padEnd(80)} ${ok ? "ok" : "FAIL"}`); bad += !ok; };
const copy = (c) => b.subarray(c * 4096, c * 4096 + Z.DIRLEN + Z.LOOPLEN);
const ds = [0, 1].map((c) => Z.parseDir(copy(c)));
check(ds[0] && ds[1], "both copies are good directories with their loops");
const c = ds[0].gen > ds[1].gen ? 0 : 1, d = ds[c];
check(Math.abs(ds[0].gen - ds[1].gen) === 1, "their generations one apart (each write into the older)");
const names = d.slots.map((s) => s && s.name);
check(!d.slots[0] && names[1] === "LOOP A" && names[2] === "REC03" && names[3] === "REC04", "deleted, renamed, recorded: as the device left them");
check(d.slots[1].rate === 44100 && d.slots[2].flags === 4 && d.slots[3].rate === 11025, "rates and the 8-bit flag");
// (the device writes the loops' block always, the web app only when a sample has a loop: either reads both)
const re = Z.buildDir({ ...d, gen: d.gen });
check(Buffer.compare(Buffer.from(re.subarray(0, Z.DIRLEN)), Buffer.from(copy(c).subarray(0, Z.DIRLEN))) === 0,
      "the web app's buildDir of it: the device's directory, byte for byte");
// 4-bit ADPCM: what the device recorded, the web app decodes as the device does (its 12-bit values: the 16-bit / 16);
// and what the web app encodes, the same decoder gives back near what went in
{
  const f = new Uint8Array(readFileSync(out + ".adpcm")), n = new DataView(f.buffer).getUint32(0, true);
  const bytes = f.subarray(4, 4 + Z.bytesOf(n, 4)), dev = new Int16Array(f.slice(4 + bytes.length).buffer);
  const y = Z.unpackAdpcm(bytes, n);
  check(n > 1000 && dev.length === n && y.every((v, i) => v === Math.max(-2048, Math.min(2047, Math.round(dev[i] / 16)))),
        "the device's ADPCM take: the web app's values, each the device's");
  const sine = Int16Array.from({ length: 5000 }, (_, i) => Math.round(1500 * Math.sin(i * 2 * Math.PI / 60)));
  const back = Z.unpackAdpcm(Z.packAdpcm(sine), sine.length);
  let sig = 0, err = 0;
  sine.forEach((v, i) => { if (i > 100) { sig += v * v; err += (back[i] - v) ** 2; } });
  check(Z.packAdpcm(sine).length === Math.ceil(5000 / 128) * 68 && sig / err > 1000, `packAdpcm: 68 bytes a block, back within -30 dB (${(10 * Math.log10(sig / err)).toFixed(1)} dB)`);
  check(Z.bitsOf({ flags: 8 }) === 4 && Z.flagsOfBits(4) === 8 && Z.bitsOf({ flags: 4 }) === 8, "flags bit 3: 4 bit");
}
console.log(bad ? `MASIF pool dir tests: ${bad} FAILED` : "MASIF pool dir tests: PASS");
process.exit(bad ? 1 : 0);

// SPDX-License-Identifier: GPL-3.0-only
// The web app's sampler without a browser: web/masif/masif-dsp.js (the processing, the FM-1's form, the room,
// SYNC POOL) against the firmware's link code built with MASIF_ENABLE_TRACKER=1 (tests/zp12_link_host.c).
//   cc -O2 -Wall -Wno-unused-function -DMASIF_ENABLE_TRACKER=1 -Ifirmware/src -o build/host/masif_link_host tests/zp12_link_host.c -lm
//   node tests/masif_web_test.mjs
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import readline from "node:readline";
const require = createRequire(import.meta.url);
const Z = require("../web/zp12link.js");
const M = require("../web/masif/masif-dsp.js");

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
let bad = 0;
const check = (ok, what) => { console.log(`masif web: ${what.padEnd(96)} ${ok ? "ok" : "FAIL"}`); bad += !ok; };
const waves = async () => (await raw("WAVES")).trim().split(" ").filter(Boolean).reduce((a, v, i, all) => (i % 6 ? a : [...a, all.slice(i, i + 6)]), []);
const loops = async () => (await raw("LOOPS")).trim().split(" ").filter(Boolean).map(Number);
const tone = (n, f, rate = 44100) => Float32Array.from({ length: n }, (_, i) => 0.5 * Math.sin(2 * Math.PI * f * i / rate));

await link.hello();

// ---- the processing
{
  const e = M.entry(Float32Array.from([0, 0, 0, ...tone(44100, 220), 0, 0]), 44100, "sine a3");
  check(e.start === 4 && e.end === 44103 && e.name === "SINE A3", "a new entry: the silence before and after off, its name as the FM-1 writes it");
  e.loopOn = true; e.la = 10000; e.lb = 30000;
  let r = M.render(e);
  check(r.x.length === 44099 && r.la === 9996 && r.lb === 29996, "as it is: the part, its loop inside it");
  e.proc.stretch = 100;                              // twice as long
  r = M.render(e);
  check(Math.abs(r.x.length - 88198) < 2 && Math.abs(r.la - 19992) < 2 && Math.abs(r.lb - 59992) < 2, "STRETCH x2: twice as long, the loop with it");
  e.proc.stretch = 50; e.proc.pitch = 100;           // an octave up: half as long
  r = M.render(e);
  check(Math.abs(r.x.length - 22049) < 2 && Math.abs(r.la - 4998) < 2, "PITCH +12: half as long, the loop with it");
  e.proc.pitch = 50; e.rev = true;
  r = M.render(e);
  check(r.la === 44099 - 29996 && r.lb === 44099 - 9996, "REVERSE: the loop mirrored");
  e.rev = false; e.fadeOn = true; e.tin = 2000; e.tout = 1000;
  r = M.render(e);
  check(r.ti === 2000 && r.to === 1000 && Math.abs(r.x[r.la]) + Math.abs(r.x[r.la + 1]) > 0, "FADE: the loop's tapers go with it as numbers, the sound is not faded");
  const tx = M.taper(Float32Array.from(r.x), r.la, r.lb, r.ti, r.to);
  check(tx[r.la] === 0 && Math.abs(tx[r.la + 1000]) <= Math.abs(r.x[r.la + 1000]) * 0.51 && tx[r.lb - 1] === 0, "... heard on the page as the FM-1 plays them");
  const st = M.encode(e, Z);
  check(st.taper && st.taper[0] === Math.round(2000 / 20000 * 256) && st.taper[1] === Math.round(1000 / 20000 * 256), `... and on the FM-1 in 1/256 of the loop (${st.taper})`);
  e.proc.gain = 100;
  r = M.render(e);
  const pk = r.x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  const pk0 = M.render(e, false).x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  check(pk > 7 && Math.abs(pk0 - 0.5) < 1e-3, "GAIN +24 dB; as it came: the faders' work left out");
  e.proc = { ...M.PROC0 }; e.fadeOn = false;
  const s = M.encode(e, Z);
  check(Math.abs(s.y.length - 26040) < 3 && s.loop && Math.abs(s.loop[0] - 9996 * 26040 / 44100) < 2 && s.play === 2,
        "the FM-1's form: at 26.04 kHz, its loop in its samples, LOOP on: a looping play mode");
  e.speed = 2; e.bits = 8;
  const s2 = M.encode(e, Z), z = M.size(e, Z);
  check(Math.abs(s2.y.length - 13020) < 3 && s2.oct && Math.abs(z.n - s2.y.length) < 3 && z.bytes === z.n, "×2 and 8 bit: half the samples, a byte each; the size known without encoding");
  check(M.fits([120, 48, 18], Z) && !M.fits([121], Z) && M.fits([60, 60, 49, 18], Z) && !M.fits([61, 60, 49, 18], Z) && !M.fits([120, 120], Z),
        "the room: what fits the three pieces (49 and 18 sectors of the data flash, 120 of the app area), each sample in one");
}

// ---- SOURCE: which input LINE IN, MIC and USB mean, by the names browsers give them
{
  const D = (label, deviceId = label) => ({ kind: "audioinput", label, deviceId });
  const win = [D("Default - Microphone (Realtek(R) Audio)", "default"), D("Communications - Microphone (Realtek(R) Audio)", "communications"),
               D("Microphone (Realtek(R) Audio)"), D("Line In (Realtek(R) Audio)"), D("Microphone (Scarlett 2i2 USB)"), D("Microphone Array (Intel Smart Sound)")];
  const mac = [D("MacBook Pro Microphone"), D("External Microphone"), D("M-VAVE FM-1"), D("BlackHole 2ch")];
  const lin = [D("Built-in Audio Analog Stereo"), D("Line In - Built-in Audio"), D("USB Audio Device Analog Stereo")];
  const p = (devs, k) => (M.pickInput(devs, k) || {}).label;
  check(p(win, "line") === "Line In (Realtek(R) Audio)" && p(win, "mic") === "Microphone (Realtek(R) Audio)" && p(win, "usb") === "Microphone (Scarlett 2i2 USB)",
        "Windows: LINE IN, MIC, USB (an interface's input called Microphone is USB)");
  check(p(mac, "mic") === "MacBook Pro Microphone" && p(mac, "line") === "External Microphone" && p(mac, "usb") === "M-VAVE FM-1",
        "macOS: the built-in mic, the 1/8\" jack (External Microphone), the FM-1 as USB");
  check(p(lin, "mic") === "Built-in Audio Analog Stereo" && p(lin, "line") === "Line In - Built-in Audio" && p(lin, "usb") === "USB Audio Device Analog Stereo",
        "Linux: the built-in input as MIC, Line In, a USB device");
  check(M.pickInput([D("Microphone (Realtek(R) Audio)")], "usb") === null && M.pickInput([D("Microphone (Realtek(R) Audio)")], "line") === null &&
        M.pickInput([D("Default", "default")], "mic").deviceId === "default", "none fits: USB and LINE IN say so, MIC takes the default input");
}

// ---- SYNC POOL against the firmware's link code
{
  const a = M.entry(tone(30000, 440), 44100, "AAA"), b = M.entry(tone(20000, 330), 44100, "BBB"), c = M.entry(tone(10000, 550), 44100, "CCC");
  a.loopOn = true; a.la = 5000; a.lb = 25000; a.fadeOn = true; a.tin = 2000; a.tout = 4000;
  await M.sync(link, Z, [a, b]);
  let w = await waves(), lp = await loops();
  check(w.length === 2 && w[0][4] === "AAA" && w[1][4] === "BBB" && w[0][0] === "0" && w[1][0] === "1", "two ticked: the FM-1's samples 0 and 1, in the list's order");
  check(Math.abs(lp[1] - 5000 * 26040 / 44100) < 2 && Math.abs(lp[2] - 25000 * 26040 / 44100) < 2 && lp[3] === 2 && lp[8] === 0,
        "the first one's loop and play mode (LOOP on: FWDLOOP) in the firmware, the second none");
  const offA = a.dev.off;
  await M.sync(link, Z, [b, c, a]);
  w = await waves();
  check(w.map((x) => x[4]).join() === "BBB,CCC,AAA" && a.dev.off === offA, "reordered and one more: the new order; what was there stays where it is (not written again)");
  a.ver++; a.name = "AAB";
  await M.sync(link, Z, [a, c]);
  w = await waves(); lp = await loops();
  check(w.map((x) => x[4]).join() === "AAB,CCC" && lp[3] === 2 && lp[4] === 26 && lp[5] === 51, "B unticked: taken off; A changed: written again, with its loop");
  // fill the rooms: what fits around what stays, else everything again
  const big = [0, 1, 2].map((k) => { const e = M.entry(tone(254000, 200 + k), 44100, "BIG" + k); return e; });
  const sz = big.map((e) => M.size(e, Z).sectors);
  check(M.fits([...sz.slice(0, 2), M.size(a, Z).sectors, M.size(c, Z).sectors], Z), `(two big ones of ${sz[0]} sectors fit beside the others)`);
  await M.sync(link, Z, [a, c, big[0], big[1]]);
  w = await waves();
  check(w.length === 4 && w[3][4] === "BIG1", "two big ones as well: all four on the FM-1");
  let err = null;
  try { await M.sync(link, Z, [a, c, ...big]); } catch (x) { err = x; }
  check(err && /do not fit/.test(err.message) && (await waves()).length === 4, "three big ones: refused before anything is changed");
  // what is on the FM-1 and in no entry comes back as entries (another computer's samples)
  const d = await Z.readDir(link);
  const got = await M.adopt(link, Z, d, [c]);
  check(got.length === 3 && got[0].name === "AAB" && got[0].loopOn && got[0].play === 2 && got[0].rate === 26040 && got[0].x.length === d.slots[0].n,
        "adopted from the FM-1: the ones no entry has, with their loop and play mode");
  const skip = await M.adopt(link, Z, d, [c], () => {}, [{ off: d.slots[0].off, n: d.slots[0].n }]);
  check(skip.length === 2 && !skip.some((e) => e.name === "AAB"), "one taken out of the list on purpose (gone): not fetched back");
  await M.sync(link, Z, [got[1], c]);
  w = await waves();
  check(w.map((x) => x[4]).join() === "BIG0,CCC" && got[1].dev.off === d.slots[2].off, "an adopted one synced as it is: not written again");
  await M.sync(link, Z, []);
  check((await waves()).length === 0, "nothing ticked: the pool is empty");
}

dev.stdin.end();
console.log(bad ? "MASIF WEB TEST FAILED" : "masif web test passed");
process.exit(bad ? 1 : 0);

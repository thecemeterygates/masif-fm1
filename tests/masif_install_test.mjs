// SPDX-License-Identifier: GPL-3.0-only
// INSTALL on the web page against a simulated FM-1 (as tests/fm1_install_test.py models one: the running firmware
// answers the handshake, reads parts of the package, becomes the update loader, which reads the whole image and
// restarts as the package's identity), Web MIDI replaced in the page. Checked: the dialog offers the page's own
// package, the loader receives exactly its logical image, the dialog ends INSTALLED. Then the way back: FROM FILE…
// refuses a changed official V15 and, given M-VAVE's unmodified FM-1.fwsc (from ../trial, never in the repo; the
// step is skipped without it), writes its image and ends on FM-1_015.
// (Based on the browser test below.)
// The web app in a real browser: a headless Firefox with its fake microphone (a steady tone) is driven over
// WebDriver BiDi through the sampler's SOURCE and REC: MIC records into a new sample, LINE IN and USB say there is
// none (the fake one is called "Default Audio Device"), the controls are where the grid puts them.
//   (cd web && python3 -m http.server 8000) &      then      node --experimental-websocket tests/masif_browser_test.mjs
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync, existsSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PORT = 9400 + Math.floor(Math.random() * 400), URL0 = process.argv[2] || "http://localhost:8000/masif/index.html?skin=med";
const prof = mkdtempSync(join(tmpdir(), "masif-ff-"));
writeFileSync(join(prof, "user.js"), ["media.navigator.streams.fake", "media.navigator.permission.disabled"].map((k) => `user_pref("${k}", true);`).join("\n") +
  '\nuser_pref("media.autoplay.default", 0);\nuser_pref("media.autoplay.block-webaudio", false);\nuser_pref("remote.active-protocols", 1);\n');
const ff = spawn("firefox", ["--headless", "--no-remote", "--profile", prof, "--remote-debugging-port", String(PORT), "--window-size=1440,810"], { stdio: "ignore" });
const done = (code) => { ff.kill(); setTimeout(() => { rmSync(prof, { recursive: true, force: true }); process.exit(code); }, 500); };
setTimeout(() => { console.log("masif install: no answer from Firefox (or too slow)"); done(1); }, 150000);

let ws = null;
for (let t = 0; t < 40 && !ws; t++) {
  await new Promise((r) => setTimeout(r, 500));
  ws = await new Promise((ok) => { const w = new WebSocket(`ws://127.0.0.1:${PORT}/session`); w.onopen = () => ok(w); w.onerror = () => ok(null); });
}
if (!ws) { console.log("masif browser: Firefox did not open its remote port"); done(1); }
let id = 0;
const wait = new Map();
ws.onmessage = (m) => { const r = JSON.parse(m.data); const w = wait.get(r.id); if (w) { wait.delete(r.id); r.type === "error" ? w.no(new Error(r.message)) : w.ok(r.result); } };
const send = (method, params) => new Promise((ok, no) => { const i = ++id; wait.set(i, { ok, no }); ws.send(JSON.stringify({ id: i, method, params })); });
await send("session.new", { capabilities: {} });
const ctx = (await send("browsingContext.getTree", {})).contexts[0].context;
await send("browsingContext.navigate", { context: ctx, url: URL0, wait: "complete" });
const run = async (f, arg = null) => {
  const r = await send("script.evaluate", { expression: `(${f})(${JSON.stringify(arg)})`, target: { context: ctx }, awaitPromise: true });
  return r.type === "exception" ? { error: r.exceptionDetails.text } : JSON.parse(r.result.value);
};

let bad = 0;
const check = (ok, what) => { console.log(`masif install: ${what.padEnd(96)} ${ok ? "ok" : "FAILED"}`); if (!ok) bad++; };
const out = await run(async () => {
  const $ = (q) => document.querySelector(q), sleep = (t) => new Promise((r) => setTimeout(r, t)), o = {};
  const P = await import("../fm1pkg.js"), O = await import("../fm1ota.js");
  const m = await (await fetch("firmware/latest.json")).json();
  const pkg = new Uint8Array(await (await fetch("firmware/" + m.file)).arrayBuffer());
  let image = P.logicalImage(pkg);
  // the device
  const dev = window.dev = { mode: "app", stock: "FM-1_015", product: m.product, got: new Uint8Array(image.length), plan: [], writes: 0 };
  dev.next = (bytes, from, to) => { image = P.logicalImage(bytes); Object.assign(dev, { mode: "app", stock: from, product: to, got: new Uint8Array(image.length), writes: 0, image }); };
  const ident = () => ({ app: dev.stock, ota: "ota-" + dev.product, new: dev.product })[dev.mode];
  const ID = (t) => { const d = new Array(34).fill(0); d[1] = 0x59; d[2] = 0x11; [...t].forEach((c, i) => { d[6 + i] = c.charCodeAt(0); }); return [0xF0, ...O.pack7(d), 0xF7]; };
  const REQ = (a, n) => { const u = [0, 0x59, 0x30, 0, 0, 0, 0, a & 255, (a >>> 8) & 255, (a >>> 16) & 255, (a >>> 24) & 255, n & 255, (n >> 8) & 255, 0]; let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u.push(~s & 255); return [0xF0, ...O.pack7(u), 0xF7]; };
  const input = { name: "MASIF FM-1", state: "connected", onmidimessage: null, open: async () => {} };
  const reply = (b) => setTimeout(() => input.onmidimessage && input.onmidimessage({ data: Uint8Array.from(b) }), 1);
  const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7], UP = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
  const output = { name: "MASIF FM-1", state: "connected", open: async () => {}, send(b) {
    b = Array.from(b);
    if (same(b, HS)) return reply(ID(ident()));
    if (same(b, UP)) {
      if (dev.mode === "app") dev.plan = [[0, 0x40], [0x40, 0x1E0], [image.length - 100, 100], [0xE0000000, 0]];
      else { dev.plan = []; for (let a = 0; a < image.length; a += 512) dev.plan.push([a, Math.min(512, image.length - a)]); dev.plan.push([0xF0000000, 0]); }
      return reply(REQ(...dev.plan[0]));
    }
    const u = O.unpack7(b.slice(1, -1)), [a, n] = dev.plan.shift(), addr = (u[7] | u[8] << 8 | u[9] << 16 | u[10] << 24) >>> 0;
    if (addr !== a) { o.wrong = true; return; }
    if (a >= 0xE0000000) { dev.mode = a === 0xE0000000 ? "ota" : "new"; return; }
    if (dev.mode === "ota") { dev.got.set(u.slice(14, 14 + n), a); dev.writes++; }
    reply(REQ(...dev.plan[0]));
  } };
  navigator.requestMIDIAccess = async () => ({ inputs: new Map([["i", input]]), outputs: new Map([["o", output]]), onstatechange: null });
  // INSTALL: the dialog with the page's package, then INSTALL in it
  $("#install").click(); await sleep(1500);
  o.offer = $("#inst-dlg").open && $("#inst-what").textContent;
  o.enabled = !$("#inst-yes").disabled;
  $("#inst-yes").click();
  const t0 = Date.now();
  while (Date.now() - t0 < 80000 && !/INSTALLED|NOT INSTALLED/.test($("#inst-prog").textContent)) await sleep(250);
  o.prog = $("#inst-prog").textContent; o.wrong = !!o.wrong; o.mode = dev.mode; o.writes = dev.writes;
  o.image = same(Array.from(dev.got), Array.from(image));
  o.closeShown = !$("#inst-close").hidden;
  return JSON.stringify(o);
});
check(/MASIF 0\.\d+ \(FM-1_96\d\) goes onto the FM-1/.test(out.offer || "") && out.enabled, `INSTALL: the dialog offers the page's package (${out.offer})`);
check(!out.wrong && out.mode === "new", "the device went app -> updater -> the new identity, every answer to its own request");
check(out.image && out.writes > 1000, `the updater received exactly the package's logical image (${out.writes} pieces)`);
check(/^INSTALLED: MASIF/.test(out.prog) && out.closeShown, `the dialog: ${out.prog}`);

// back to the official V15, from the MASIF just installed
const V15 = new URL("../../trial/FM-1_015-V15-official.fwsc", import.meta.url);
const v15 = existsSync(V15) ? readFileSync(V15).toString("base64") : null;
const back = await run(async (b64) => {
  const $ = (q) => document.querySelector(q), sleep = (t) => new Promise((r) => setTimeout(r, t)), o = {}, dev = window.dev;
  const pick = async (bytes) => {
    const dt = new DataTransfer(); dt.items.add(new File([bytes], "FM-1.fwsc"));
    $("#fwfile").files = dt.files; $("#fwfile").dispatchEvent(new Event("change")); await sleep(1500);
  };
  $("#inst-ok").click(); await sleep(300); $("#install").click(); await sleep(1000);
  if (b64) {
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0)), bent = bytes.slice(); bent[0x50000] ^= 1;
    await pick(bent); o.bent = $("#inst-prog").textContent;
    await pick(bytes); o.offer = $("#inst-what").textContent; o.label = $("#inst-yes").textContent;
    dev.next(bytes, dev.product, "FM-1_015");
    $("#inst-yes").click();
    const t0 = Date.now();
    while (Date.now() - t0 < 80000 && !/INSTALLED|NOT INSTALLED/.test($("#inst-prog").textContent)) await sleep(250);
    o.prog = $("#inst-prog").textContent; o.mode = dev.mode; o.image = dev.got.every((x, i) => x === dev.image[i]); o.writes = dev.writes;
  }
  return JSON.stringify(o);
}, v15);
if (!v15) console.log("masif install: (no ../trial/FM-1_015-V15-official.fwsc: the way back is not tested)");
else {
  check(/NOT THE UNMODIFIED OFFICIAL V15/.test(back.bent || ""), `FROM FILE refuses a changed V15 (${back.bent})`);
  check(/official V15 \(FM-1_015\) goes back/.test(back.offer || "") && back.label === "INSTALL V15", `FROM FILE takes the official V15 (${back.label})`);
  check(back.mode === "new" && back.image && back.writes > 1000, `the updater received the official image (${back.writes} pieces), FM-1_015 after it`);
  check(/^INSTALLED: M-VAVE'S OFFICIAL V15/.test(back.prog || ""), `the dialog: ${back.prog}`);
}
await send("session.end", {}).catch(() => {});
console.log(bad ? "MASIF INSTALL TEST FAILED" : "masif install test passed");
done(bad ? 1 : 0);

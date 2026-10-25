// SPDX-License-Identifier: GPL-3.0-only
// The web app in a real browser: a headless Firefox with its fake microphone (a steady tone) is driven over
// WebDriver BiDi through the sampler's SOURCE and REC: MIC records into a new sample, LINE IN and USB say there is
// none (the fake one is called "Default Audio Device"), the controls are where the grid puts them.
//   (cd web && python3 -m http.server 8000) &      then      node --experimental-websocket tests/masif_browser_test.mjs
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PORT = 9400 + Math.floor(Math.random() * 400), URL0 = process.argv[2] || "http://localhost:8000/masif/index.html?skin=med";
const prof = mkdtempSync(join(tmpdir(), "masif-ff-"));
writeFileSync(join(prof, "user.js"), ["media.navigator.streams.fake", "media.navigator.permission.disabled"].map((k) => `user_pref("${k}", true);`).join("\n") +
  '\nuser_pref("media.autoplay.default", 0);\nuser_pref("media.autoplay.block-webaudio", false);\nuser_pref("remote.active-protocols", 1);\n');
const ff = spawn("firefox", ["--headless", "--no-remote", "--profile", prof, "--remote-debugging-port", String(PORT), "--window-size=1440,810"], { stdio: "ignore" });
const done = (code) => { ff.kill(); setTimeout(() => { rmSync(prof, { recursive: true, force: true }); process.exit(code); }, 500); };
setTimeout(() => { console.log("masif browser: no answer from Firefox"); done(1); }, 90000);

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
const run = async (f) => {
  const r = await send("script.evaluate", { expression: `(${f})()`, target: { context: ctx }, awaitPromise: true });
  return r.type === "exception" ? { error: r.exceptionDetails.text } : JSON.parse(r.result.value);
};

const out = await run(async () => {
  const $ = (q) => document.querySelector(q), sleep = (t) => new Promise((r) => setTimeout(r, t)), o = {};
  const txt = (q) => $(q).querySelector("b").textContent, lit = (q) => $(q).classList.contains("on");
  await sleep(800);
  const faders = () => [...document.querySelectorAll("#faders .fader")].map((f) => f.querySelector("output").textContent + "@" + f.querySelector("input").value).join();
  o.empty = faders();
  o.options = [...$("#src").options].map((x) => x.textContent).join();
  o.keys = [...document.querySelectorAll(".deck .keys .key")].map((k) => k.id).join();
  o.srcFits = $("#src").scrollWidth <= $("#src").clientWidth + 1;
  o.old = ["#m-play", "#m-roll", "#thr", "#lvl", "#zoom", "#p-rate"].filter((q) => $(q)).join();
  // the keys that go round
  const z = []; for (let i = 0; i < 3; i++) { $("#k-zoom").click(); z.push(txt("#k-zoom") + (lit("#k-zoom") ? "*" : "")); }
  o.zoom = z.join();
  $("#src").value = "mic"; $("#src").dispatchEvent(new Event("change"));
  // ARM: the input on the wave, then off
  $("#k-arm").click(); await sleep(1200);
  o.armed = lit("#k-arm") && /ARMED/.test($("#pool-msg").textContent) && $("#mk-start").style.display === "none";
  $("#k-arm").click(); await sleep(200);
  o.disarmed = !lit("#k-arm");
  // REC from MIC: a new sample
  $("#k-rec").click(); await sleep(2000);
  o.during = $("#pool-msg").textContent;
  $("#k-rec").click(); await sleep(600);
  o.after = $("#pool-msg").textContent; o.name = $("#s-name").textContent;
  o.loaded = faders();
  for (const k of ["line", "usb"]) { $("#src").value = k; $("#k-rec").click(); await sleep(700); o[k] = $("#pool-msg").textContent; if ($("#k-rec.on")) $("#k-rec").click(); }
  // SPEED, BITS, RATE on the new sample; FADE only with LOOP, its lines at the loop's
  $("#k-bits").click(); o.bits = txt("#k-bits") + (lit("#k-bits") ? "*" : "");
  $("#k-speed").click(); $("#k-speed").click(); o.speed = txt("#k-speed") + (lit("#k-speed") ? "*" : "");
  $("#k-rate").click(); o.rate = txt("#k-rate") + (lit("#k-rate") ? "*" : "");
  o.fadeOff = $("#k-fade").classList.contains("dim");
  $("#m-loop").click(); await sleep(100); $("#k-fade").click(); await sleep(100);
  o.fadeOn = lit("#k-fade") && $("#mk-ta").style.display !== "none" && Math.abs(parseFloat($("#mk-ta").style.left) - parseFloat($("#mk-la").style.left)) < 2;
  return JSON.stringify(o);
});
let bad = 0;
const check = (ok, what) => { console.log(`masif browser: ${what.padEnd(84)} ${ok ? "ok" : "FAIL"}`); bad += !ok; };
if (out.error) { console.log("masif browser: the page threw: " + out.error); done(1); }
const ZERO = "0@50,0%@0,0%@0,0@0,0%@50,0@0,0@50,0@0";
check(out.empty === ZERO, `PROCESS with nothing open: every fader at its zero (${out.empty})`);
check(out.loaded === ZERO, `PROCESS on a new sample: every fader at its zero (${out.loaded})`);
check(out.options === "FILE,LINE IN,MIC,USB" && out.srcFits, `SOURCE: ${out.options}, the choice shown whole`);
check(out.keys === "k-home,k-play,k-stop,k-arm,k-rec,m-loop,k-fade,k-zoom,k-speed,k-bits,k-rate" && !out.old, "the keys in their order; no PLAY mode, FADE / MS, THRESH, LEVEL, sliders");
check(out.zoom === "2×*,4×*,1×", `ZOOM goes round, lit when in (${out.zoom})`);
check(out.armed && out.disarmed, "ARM: the input on the wave (the lines hidden), ARM again: off");
check(/RECORDING|WAITING/.test(out.during) && /RECORDED/.test(out.after) && /REC \d+/.test(out.name), `REC from MIC: a new sample (${out.after}, ${out.name})`);
check(out.line === "NO LINE INPUT FOUND" && /NO USB AUDIO INPUT FOUND/.test(out.usb), "LINE IN and USB with no such input: said so, nothing recorded");
check(out.bits === "8*" && out.speed === "×2*" && out.rate === "27.5K*", `BITS, SPEED, RATE go round, lit off their first (${out.bits} ${out.speed} ${out.rate})`);
check(out.fadeOff && out.fadeOn, "FADE: nothing without LOOP; with it lit, its lines beginning on the loop's");
// CLEAR: the pop-up, CANCEL keeps the sample open, CONFIRM CLEAR empties the sampler (the list as it was)
const c1 = await run(async () => {
  const $ = (q) => document.querySelector(q);
  const o = { n: $("#list").children.length, name: $("#s-name").textContent, left: $("#p-clear").nextElementSibling.id };
  $("#p-clear").click();
  o.open = $("#clear-dlg").open && /CONFIRM CLEAR/.test($("#clear-yes").textContent);
  return JSON.stringify(o);
});
{
  const shot = await send("browsingContext.captureScreenshot", { context: ctx }).catch(() => null);
  if (shot) writeFileSync("build/host/masif-clear.png", Buffer.from(shot.data, "base64"));
}
const c2 = await run(async () => {
  const $ = (q) => document.querySelector(q), sleep = (t) => new Promise((r) => setTimeout(r, t)), o = {};
  $("#clear-no").click(); await sleep(100);
  o.kept = !$("#clear-dlg").open && $("#s-name").textContent;
  $("#p-clear").click(); $("#clear-yes").click(); await sleep(100);
  o.cleared = !$("#clear-dlg").open && $("#s-name").textContent;
  o.n = $("#list").children.length; o.sel = $("#list .sel") ? 1 : 0;
  o.faders = [...document.querySelectorAll("#faders .fader")].map((f) => f.querySelector("output").textContent + "@" + f.querySelector("input").value).join();
  return JSON.stringify(o);
});
check(c1.left === "p-new" && c1.open, "CLEAR, left of SAVE AS NEW: the CONFIRM CLEAR pop-up");
check(c2.kept === c1.name, `CANCEL: the sample still open (${c2.kept})`);
check(c2.cleared === "DROP A SOUND HERE" && c2.n === c1.n && !c2.sel && c2.faders === ZERO, "CONFIRM CLEAR: the sampler empty, the faders at zero, the list as it was");
// the GUIDE tab: a click shows it (the sampler hidden), the contents scroll its text, SAMPLER brings the sampler back
const g = await run(async () => {
  const $ = (q) => document.querySelector(q), sleep = (t) => new Promise((r) => setTimeout(r, t)), o = {};
  const shown = (q) => getComputedStyle($(q)).display !== "none";
  $('#mode button[data-v="guide"]').click(); await sleep(150);
  o.guide = shown("#guide") && !shown("#sampler") && $('#mode button[data-v="guide"]').classList.contains("on");
  $('#toc a[href="#g-sync"]').click(); await sleep(300);
  o.scrolled = $("#guide-text").scrollTop > 0 && $("#toc a.on")?.getAttribute("href") === "#g-sync";
  $('#mode button[data-v="sampler"]').click(); await sleep(150);
  o.back = shown("#sampler") && !shown("#guide");
  return JSON.stringify(o);
});
check(g.guide, "GUIDE: a click shows the guide, the sampler hidden");
check(g.scrolled, "the contents: a click scrolls the text there and marks it");
check(g.back, "SAMPLER: back to the sampler");
await send("session.end", {}).catch(() => {});
console.log(bad ? "MASIF BROWSER TEST FAILED" : "masif browser test passed");
done(bad ? 1 : 0);

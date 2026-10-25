// SPDX-License-Identifier: GPL-3.0-only
// MASIF's web app, the page: the sampler's three windows and the side boxes. The library (the computer's samples,
// each with what is to be done with it) is kept in this browser (IndexedDB); a tick puts a sample in the FM-1's
// pool at the next SYNC POOL. The sound's work and the sync are masif-dsp.js's (M), the link ../zp12link.js's (Z).
// ?skin=med opens it in a skin (else a random one); ?help=src shows that one's help; ?demo=1 fills an empty library with made-up sounds (for pictures of it).
"use strict";
(() => {
  const Z = window.ZP12Link, M = window.MasifDSP;
  // the firmware on the FM-1 by its name: MASIF's rooms are MASIF whatever its HELLO says (before 2026-10-21 it said zp12)
  const devName = (h) => (h.rooms === 1 ? "MASIF " + h.version.replace(/^\S+\s*/, "") : h.version).trim().toUpperCase();
  const $ = (q) => document.querySelector(q), $$ = (q) => [...document.querySelectorAll(q)];
  const hex = (v, n = 2) => v.toString(16).toUpperCase().padStart(n, "0");
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  // ---- the skins: a name, its family of shapes (masif.css)
  // (LIQUID and CHROME, the hyper family, are out for now: the sampler's new controls broke them (the user,
  // 2026-10-14). Their styles are still in masif.css: ["liquid", "hyper"], ["chrome", "hyper"] back here brings them.)
  // (GADGET is out for now too: "several things wrong" (the user, 2026-10-14). Back: ["gadget", "blob"].)
  // (their names, the user's, 2026-10-14: the styles keep the old keys: med VICE, handheld RETRO, pro MONO, sid STEEL)
  const SKINS = [["med", "box", "VICE"], ["handheld", "box", "RETRO"], ["pro", "box", "MONO"], ["sid", "box", "STEEL"]];
  const THEMES = ["WHITE ON BLACK", "LCD GREEN", "AMBER"];          // the FM-1's own palettes (masif_ui.c)

  const lib = [];                                  // the library, in the list's order
  const S = { sel: -1, tab: "pool", link: null, dir: null, kit: +(localStorage.getItem("masif-kit") || 0), dirty: false, busy: false,
              zoom: 1, v0: 0, play: null, rec: null };
  const cur = () => lib[S.sel] || null;
  // the FM-1's rooms before it is connected: as the last one had them (its app area's room: MASIF from 2026-10-21)
  try { const a = JSON.parse(localStorage.getItem("masif-app") || "null"); if (a) Z.useApp(a); } catch (x) { /* none kept */ }
  const msg = (t) => { $("#pool-msg").textContent = t; };
  const status = (t) => { $("#status").textContent = t; };

  // ---- kept in this browser: every entry, and the list's order
  let db = null;
  function idb() {
    if (db || !window.indexedDB) return Promise.resolve(db);
    return new Promise((ok) => {
      const r = indexedDB.open("masif", 1);
      r.onupgradeneeded = () => { r.result.createObjectStore("lib", { keyPath: "id" }); r.result.createObjectStore("meta"); };
      r.onsuccess = () => { db = r.result; ok(db); };
      r.onerror = () => ok(null);
    });
  }
  const tx = (store, mode, f) => idb().then((d) => d && new Promise((ok) => { const t = d.transaction(store, mode); f(t.objectStore(store)); t.oncomplete = ok; t.onerror = ok; }));
  const saveTimers = new Map();
  const DEMO = new URLSearchParams(location.search).has("demo");
  function save(e) {                               // (a moment after the last change: dragging makes many)
    if (DEMO) return;
    clearTimeout(saveTimers.get(e.id));
    saveTimers.set(e.id, setTimeout(() => tx("lib", "readwrite", (s) => s.put(e)), 400));
  }
  const saveOrder = () => !DEMO && tx("meta", "readwrite", (s) => { s.put(lib.map((e) => e.id), "order"); s.put(S.dirty, "dirty"); });
  async function load() {
    const d = await idb();
    if (!d) return;
    const [all, order, dirty] = await new Promise((ok) => {
      const t = d.transaction(["lib", "meta"]), r = [], m = t.objectStore("meta");
      t.objectStore("lib").getAll().onsuccess = (ev) => r.push(...ev.target.result);
      const o = m.get("order"), dd = m.get("dirty");
      t.oncomplete = () => ok([r, o.result || [], !!dd.result]);
    });
    const by = new Map(all.map((e) => [e.id, e]));
    for (const id of order) if (by.has(id)) { lib.push(by.get(id)); by.delete(id); }
    lib.push(...by.values());
    S.dirty = dirty;
  }
  // a change to the open entry: its FM-1 form is another now (ver), it is kept, the pool is not synced
  function changed(e = cur(), data = true) {
    if (!e) return;
    if (data) e.ver++;
    if (e.ticked) S.dirty = true;
    save(e); saveOrder();
  }

  // ---- the display: the wave, the view (zoom, where), the marks
  const wave = $("#wave"), box = $("#wavebox");
  const span = () => { const e = cur(); return e ? e.x.length : 1; };
  const view = () => { const n = span(), w = n / S.zoom; S.v0 = clamp(S.v0, 0, Math.max(0, n - w)); return [S.v0, S.v0 + w]; };
  const css = (v) => getComputedStyle(document.body).getPropertyValue(v).trim();
  function draw() {
    const e = cur(), dpr = window.devicePixelRatio || 1;
    wave.width = Math.max(200, Math.round(wave.clientWidth * dpr)); wave.height = Math.max(60, Math.round(wave.clientHeight * dpr));
    const g = wave.getContext("2d"), W = wave.width, H = wave.height;
    g.clearRect(0, 0, W, H);
    g.fillStyle = css("--lcd-dim");
    g.fillRect(0, H >> 1, W, 1);
    if (S.rec) {                                     // armed: what comes in (the last ~1.5 s), and the threshold
      const r = S.rec, n = r.mon.length;
      g.fillStyle = css("--lcd-ink");
      for (let px = 0; px < W; px++) {
        const v = Math.min(1, r.mon[(r.at + Math.floor(px * n / W)) % n]);
        g.fillRect(px, H / 2 - v * H / 2, 1, Math.max(1, v * H));
      }
      g.fillStyle = css("--arm") || "#ff3040";
      if (thrOff()) g.fillRect(0, (H >> 1) - 1, W, 3);
      else { const t = Math.pow(10, S.thr / 20) * H / 2; g.fillRect(0, H / 2 - t - 1, W, 3); g.fillRect(0, H / 2 + t - 1, W, 3); }
      marks();
      return;
    }
    if (!e) { marks(); return; }
    const [v0, v1] = view(), x = e.x, per = (v1 - v0) / W;
    if (e.loopOn) {                                  // the loop: a band under the wave, faded where its tapers are
      const a = (e.la - v0) / per, b = (e.lb - v0) / per, hot = css("--lcd-hot");
      const gr = g.createLinearGradient(a, 0, b, 0), w = Math.max(1, b - a);
      const ti = e.fadeOn ? clamp(e.tin * (W / (v1 - v0)) / w, 0, 0.5) : 0, to = e.fadeOn ? clamp(e.tout * (W / (v1 - v0)) / w, 0, 0.5) : 0;
      gr.addColorStop(0, ti ? "transparent" : hot); gr.addColorStop(ti, hot); gr.addColorStop(1 - to, hot); gr.addColorStop(1, to ? "transparent" : hot);
      g.globalAlpha = 0.3; g.fillStyle = gr; g.fillRect(a, 0, b - a, H); g.globalAlpha = 1;
    }
    for (let px = 0; px < W; px++) {
      const a = Math.floor(v0 + px * per), b = Math.max(a + 1, Math.floor(v0 + (px + 1) * per));
      let lo = 1, hi = -1;
      for (let i = a; i < b && i < x.length; i += Math.max(1, (b - a) >> 6)) { const v = x[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
      if (hi < lo) continue;
      g.fillStyle = a >= e.start && a < e.end ? css("--lcd-ink") : css("--lcd-dim");
      g.fillRect(px, H / 2 - hi * H / 2, 1, Math.max(1, (hi - lo) * H / 2));
    }
    marks();
  }
  function marks() {                               // the four lines over the wave, where they are in the view
    const e = cur(), [v0, v1] = view(), w = box.clientWidth;
    const put = (id, pos, on) => {
      const m = $(id), x = (pos - v0) / (v1 - v0) * w;
      m.style.display = on && x >= -1 && x <= w + 1 ? "" : "none";
      m.style.left = `${x}px`;
    };
    const on = !!e && !S.rec;                        // (armed: the wave is the input's; no lines to drag)
    put("#mk-start", e ? e.start : 0, on); put("#mk-end", e ? e.end : 0, on);
    put("#mk-la", e ? e.la : 0, on && e.loopOn); put("#mk-lb", e ? e.lb : 0, on && e.loopOn);
    put("#mk-ta", e ? e.la + e.tin : 0, on && e.loopOn && e.fadeOn); put("#mk-tb", e ? e.lb - e.tout : 0, on && e.loopOn && e.fadeOn);
    const sc = $("#w-scroll"), n = span();
    sc.style.display = S.zoom > 1 && e && !S.rec ? "" : "none";
    sc.value = n > v1 - v0 ? Math.round(S.v0 / (n - (v1 - v0)) * 1000) : 0;
  }
  // dragging a line: START / END inside the sound, the loop inside the part (a loop of 64 samples at the least)
  function drag(id, set) {
    const m = $(id);
    m.onpointerdown = (ev) => {
      if (!cur()) return;
      ev.preventDefault(); m.setPointerCapture(ev.pointerId); m.classList.add("on");
      m.onpointermove = (mv) => {
        const r = box.getBoundingClientRect(), [v0, v1] = view();
        set(cur(), Math.round(v0 + clamp((mv.clientX - r.left) / r.width, 0, 1) * (v1 - v0)));
        fit(cur()); draw(); fields();
      };
    };
    m.onpointerup = m.onpointercancel = () => { m.onpointermove = null; m.classList.remove("on"); changed(); list(); };
  }
  function fit(e) {                                // every mark where it may be
    const n = e.x.length;
    e.start = clamp(e.start, 0, n - 64); e.end = clamp(e.end, e.start + 64, n);
    e.la = clamp(e.la, e.start, e.end - 64); e.lb = clamp(e.lb, e.la + 64, e.end);
    e.tin = clamp(e.tin || 0, 0, (e.lb - e.la) >> 1); e.tout = clamp(e.tout || 0, 0, (e.lb - e.la) >> 1);
  }
  drag("#mk-start", (e, p) => { e.start = clamp(p, 0, e.end - 64); if (e.la < e.start) e.la = e.start; });
  drag("#mk-end", (e, p) => { e.end = clamp(p, e.start + 64, e.x.length); if (e.lb > e.end) e.lb = e.end; });
  drag("#mk-la", (e, p) => { e.la = clamp(p, e.start, e.lb - 64); });
  drag("#mk-lb", (e, p) => { e.lb = clamp(p, e.la + 64, e.end); });
  // the tapers: dragged in from the loop's ends, half the loop each at the most
  drag("#mk-ta", (e, p) => { e.tin = clamp(p - e.la, 0, (e.lb - e.la) >> 1); });
  drag("#mk-tb", (e, p) => { e.tout = clamp(e.lb - p, 0, (e.lb - e.la) >> 1); });
  wave.addEventListener("wheel", (ev) => {
    if (!cur()) return;
    ev.preventDefault();
    if (ev.ctrlKey) { zoomTo(ev.deltaY < 0 ? Math.min(4, S.zoom * 2) : Math.max(1, S.zoom / 2)); return; }
    const [v0, v1] = view();
    S.v0 = v0 + (ev.deltaX || ev.deltaY) / 400 * (v1 - v0);
    draw();
  }, { passive: false });
  function zoomTo(z) {
    const [v0, v1] = view(), mid = (v0 + v1) / 2;
    S.zoom = z >= 4 ? 4 : z >= 2 ? 2 : 1;
    keyset("#k-zoom", `${S.zoom}×`, S.zoom > 1);
    S.v0 = mid - span() / S.zoom / 2;
    draw();
  }
  $("#k-zoom").onclick = () => zoomTo(S.zoom === 4 ? 1 : S.zoom * 2);
  function keyset(id, text, lit) { const k = $(id); k.querySelector("b").textContent = text; k.classList.toggle("on", lit); }
  $("#w-scroll").oninput = () => { const [v0, v1] = view(); S.v0 = +$("#w-scroll").value / 1000 * (span() - (v1 - v0)); draw(); };

  // ---- the fields under the display and in PROCESS: what the open entry has
  const PROCESS = [["gain", "GAIN", "Louder or softer: -24 to +24 dB.", (v) => { const d = Math.round((v - 50) / 50 * 24); return `${d > 0 ? "+" : ""}${d}`; }],
    ["fin", "FADE IN", "The start comes up from silence, over up to half its length.", (v) => `${Math.round(v / 2)}%`],
    ["fout", "FADE OUT", "The end goes down to silence, over up to half its length.", (v) => `${Math.round(v / 2)}%`],
    ["comp", "COMP", "Compression: the loud parts held down (NORMALIZE brings the whole up again).", (v) => `${v}`],
    ["stretch", "STRETCH", "Old-school time stretching: half to twice as long at the same pitch, with the grain's grit (-50% to +100%).",
     (v) => { const p = Math.round((M.stretchOf({ stretch: v }) - 1) * 100); return `${p > 0 ? "+" : ""}${p}%`; }],
    ["grain", "GRAIN", "The stretch's grain: 0 the shortest (10 ms, metallic) to 100 the longest (200 ms, smoother).", (v) => `${v}`],
    ["pitch", "PITCH", "Higher or lower by up to 12 semitones (and shorter or longer).", (v) => { const s = Math.round((v - 50) / 50 * 12); return `${s > 0 ? "+" : ""}${s}`; }],
    ["drive", "DRIVE", "Into a soft clip.", (v) => `${v}`]];
  $("#faders").replaceChildren(...PROCESS.map(([k, name, help, fmt]) => {
    const f = el("label", "fader"), i = el("input"), o = el("output");
    i.type = "range"; i.id = "f-" + k; f.dataset.help = help;
    i.oninput = () => { const e = cur(); o.textContent = fmt(+i.value); if (e) { e.proc[k] = +i.value; changed(e); list(); } };
    i.show = (v) => { i.value = v; o.textContent = fmt(v); };
    f.append(o, i, el("span", "", name));
    return f;
  }));
  function fields() {                              // the open entry into the fields (and the LCD's words)
    const e = cur();
    $("#s-name").textContent = e ? e.name : "DROP A SOUND HERE";   // (no number: that is the FM-1's SAMPLE, once ticked)
    if (!e) {                                      // (empty: the faders at their zero too)
      ["#s-len", "#s-inst"].forEach((q) => { $(q).textContent = "--"; });
      PROCESS.forEach(([k]) => $("#f-" + k).show(M.PROC0[k]));
      return;
    }
    const z = M.size(e, Z), slot = slotOf(S.sel);
    $("#s-rate").textContent = `${+(e.rate / 1000).toFixed(1) % 1 ? (e.rate / 1000).toFixed(1) : Math.round(e.rate / 1000)} KHZ${e.speed ? " " + M.SPEEDS[e.speed] : ""}`;
    $("#s-bits").textContent = `${e.bits} BIT`;
    $("#s-len").textContent = `${z.secs.toFixed(2)} S`;
    $("#s-inst").textContent = e.ticked ? `SAMPLE ${hex(S.kit + slot)}` : "NOT ON FM-1";
    if (!S.play) $("#s-time").textContent = clock(z.secs);
    $("#m-loop").classList.toggle("on", e.loopOn);
    $("#k-fade").classList.toggle("on", e.loopOn && e.fadeOn); $("#k-fade").classList.toggle("dim", !e.loopOn);
    PROCESS.forEach(([k]) => $("#f-" + k).show(e.proc[k]));
    $("#p-rev").classList.toggle("on", e.rev); $("#p-norm").classList.toggle("on", e.norm);
    keyset("#k-speed", M.SPEEDS[e.speed], e.speed !== 0);
    keyset("#k-bits", String(e.bits), e.bits !== 12);
    keyset("#k-rate", RATES.find((r) => r[0] === e.rate)?.[1] || `${Math.round(e.rate / 1000)}K`, e.rate !== 26040);
  }
  const clock = (s) => `${String(Math.floor(s / 60)).padStart(2, "0")}:${(s % 60).toFixed(2).padStart(5, "0")}`;
  const RATES = [[26040, "26K"], [27500, "27.5K"], [13020, "13K"]];
  const field = (id, f, data = true) => { $(id).oninput = $(id).onchange = () => { const e = cur(); if (!e) return; f(e, $(id)); fit(e); changed(e, data); fields(); draw(); list(); }; };
  const toggle = (id, f) => { $(id).onclick = () => { const e = cur(); if (!e) return; f(e); fit(e); changed(e); fields(); draw(); list(); }; };
  toggle("#m-loop", (e) => {                       // on: the loop's lines where they were, or round the part's middle half
    e.loopOn = !e.loopOn;
    if (e.loopOn && (e.la < e.start || e.lb > e.end || e.lb - e.la < 64)) { e.la = e.start + ((e.end - e.start) >> 2); e.lb = e.end; }
    e.play = M.playOf(e);
  });
  toggle("#k-fade", (e) => { if (e.loopOn) e.fadeOn = !e.fadeOn; });     // (the tapers' lines begin on the loop's)
  // SPEED, BITS, RATE: keys that go round their values, lit off the first
  toggle("#k-speed", (e) => { e.speed = (e.speed + 1) % 4; });
  toggle("#k-bits", (e) => { e.bits = e.bits === 12 ? 8 : e.bits === 8 ? 4 : 12; });   // (4: ADPCM)
  toggle("#k-rate", (e) => { const i = RATES.findIndex((r) => r[0] === e.rate); e.rate = RATES[(i + 1) % RATES.length][0]; });
  toggle("#p-rev", (e) => { e.rev = !e.rev; });
  toggle("#p-norm", (e) => { e.norm = !e.norm; });
  // the result baked: as it will be, at its own rate, the faders back to zero
  function bake(e) {
    const r = M.render(e), n = M.entry(r.x, r.rate, e.name);
    Object.assign(n, { start: 0, end: r.x.length, loopOn: e.loopOn, la: r.la, lb: r.lb, play: e.play, fadeOn: e.fadeOn, tin: r.ti, tout: r.to,
                       norm: e.norm, rate: e.rate, speed: e.speed, bits: e.bits });
    return n;
  }
  // CLEAR: after CONFIRM CLEAR the sampler empty (the sample stays in the list, nothing deleted)
  $("#p-clear").onclick = () => { if (cur()) $("#clear-dlg").showModal(); };
  $("#clear-no").onclick = () => $("#clear-dlg").close();
  $("#clear-yes").onclick = () => { $("#clear-dlg").close(); open(-1); };
  $("#p-new").onclick = () => { const e = cur(); if (!e) return; const n = bake(e); lib.splice(S.sel + 1, 0, n); save(n); saveOrder(); open(S.sel + 1); };
  $("#p-over").onclick = () => {
    const e = cur(); if (!e) return;
    const n = bake(e);
    Object.assign(e, { x: n.x, start: 0, end: n.end, la: n.la, lb: n.lb, tin: n.tin, tout: n.tout, proc: { ...M.PROC0 }, rev: false });
    changed(e); open(S.sel);
  };

  // ---- playing it: as the FM-1 will (its rate, bits, the stored speed, its loop until STOP), or as it came (A / B)
  let ac = null;
  const audio = () => { ac = ac || new AudioContext(); if (ac.state === "suspended") ac.resume().catch(() => {}); return ac; };
  function stop() {
    if (S.play) { try { S.play.node.stop(); } catch (x) { /* (it ended) */ } S.play = null; }
    $("#ph").style.display = "none";
    fields();
  }
  function play() {
    const e = cur();
    if (!e) return;
    stop();
    const a = audio();
    let buf, ls = 0, le = 0, rev = false;
    {
      const s = M.encode(e, Z), y = s.bits === 12 ? s.y : Z.unpackBits(Z.packBits(s.y, s.bits), s.y.length, s.bits);
      let o = Z.zoh(y, s.rate, s.slow, s.oct);
      const k = o.length / y.length, pm = s.play;
      rev = pm % 2 === 1 && pm < 9;
      if (s.loop) {
        let la = Math.round(s.loop[0] * k), lb = Math.round(s.loop[1] * k);
        if (s.taper) { o = Float32Array.from(o); M.taper(o, la, lb, Math.round(s.taper[0] / 256 * (lb - la)), Math.round(s.taper[1] / 256 * (lb - la))); }   // (as the FM-1 plays them)
        if (pm >= 6 && pm <= 8) { o = o.slice(la, lb); la = 0; lb = o.length; }       // OSC: the loop alone
        if (rev) { o = o.slice().reverse(); [la, lb] = [o.length - lb, o.length - la]; }
        if ([4, 5, 8, 10, 12].includes(pm)) {         // ping-pong: the loop forwards, then backwards
          const back = o.slice(la, lb).reverse(), p = new Float32Array(lb + back.length);
          p.set(o.subarray(0, lb)); p.set(back, lb); o = p; lb = o.length;
        }
        ls = la / 44100; le = lb / 44100;
      } else if (rev) o = o.slice().reverse();
      buf = a.createBuffer(1, o.length, 44100); buf.copyToChannel(o, 0);
    }
    const node = a.createBufferSource();
    node.buffer = buf;
    if (le > ls) { node.loop = true; node.loopStart = ls; node.loopEnd = le; }
    node.connect(a.destination);
    node.onended = () => { if (S.play && S.play.node === node) stop(); };
    node.start();
    S.play = { node, t0: a.currentTime, dur: buf.duration, ls, le, rev };
    (function head() {                               // where it is: the time, and a line over the wave
      const p = S.play;
      if (!p || p.node !== node) return;
      let t = a.currentTime - p.t0;
      if (p.le > p.ls && t > p.le) t = p.ls + (t - p.ls) % (p.le - p.ls);
      $("#s-time").textContent = clock(t);
      const f = clamp(t / p.dur, 0, 1), pos = p.rev ? e.end - f * (e.end - e.start) : e.start + f * (e.end - e.start), [v0, v1] = view();
      const ph = $("#ph"), x = (pos - v0) / (v1 - v0) * box.clientWidth;
      ph.style.display = x >= 0 && x <= box.clientWidth ? "" : "none"; ph.style.left = `${x}px`;
      requestAnimationFrame(head);
    })();
  }
  $("#k-play").onclick = play;
  $("#k-stop").onclick = () => { if (S.rec) (S.rec.capturing ? recStop("") : disarm()); stop(); };
  $("#k-home").onclick = () => { stop(); S.v0 = 0; draw(); };

  // ---- sounds in: files (chosen, or dropped anywhere on the page)
  async function addFiles(files) {
    let first = -1;
    for (const f of files) {
      try {
        const b = await audio().decodeAudioData(await f.arrayBuffer());
        const m = new Float32Array(b.length);
        for (let c = 0; c < b.numberOfChannels; c++) { const d = b.getChannelData(c); for (let i = 0; i < m.length; i++) m[i] += d[i] / b.numberOfChannels; }
        const e = M.entry(m, b.sampleRate, f.name.replace(/\.[^.]*$/, ""));
        lib.push(e); save(e);
        if (first < 0) first = lib.length - 1;
      } catch (x) { msg(`${f.name}: NOT A SOUND THIS BROWSER READS`); }
    }
    saveOrder();
    if (first >= 0) { open(first); msg("TICK IT TO PUT IT ON THE FM-1"); }
  }
  $("#l-add").onclick = () => $("#file").click();
  $("#file").onchange = () => { addFiles([...$("#file").files]); $("#file").value = ""; };
  document.addEventListener("dragover", (ev) => { if (ev.dataTransfer.types.includes("Files")) { ev.preventDefault(); document.body.classList.add("dropping"); } });
  document.addEventListener("dragleave", (ev) => { if (!ev.relatedTarget) document.body.classList.remove("dropping"); });
  document.addEventListener("drop", (ev) => { ev.preventDefault(); document.body.classList.remove("dropping"); if (ev.dataTransfer.files.length) addFiles([...ev.dataTransfer.files]); });

  // ---- sounds in: recorded (zp12's sampling: an input, a threshold to start at, 20 ms before it kept, until STOP
  // or until the FM-1's room is full). ARM listens without recording: the wave shows what comes in, with the
  // threshold as a solid line across it to drag; REC (armed or not) records from where the sound passes it
  const WORKLET = `registerProcessor("masif-rec", class extends AudioWorkletProcessor {
    process(ins) { const i = ins[0]; if (i && i.length) { const m = new Float32Array(i[0].length);
      for (const c of i) for (let k = 0; k < m.length; k++) m[k] += c[k] / i.length; this.port.postMessage(m, [m.buffer]); } return true; } });`;
  let recNo = +(localStorage.getItem("masif-recno") || 0);
  S.thr = +(localStorage.getItem("masif-thr") || -40);
  const thrOff = () => S.thr <= -61;               // (dragged down to the middle: at once)
  // SOURCE: FILE opens the file browser at once (and at REC); LINE IN, MIC, USB are this computer's inputs as
  // masif-dsp.js pickInput sorts them by name
  $("#src").onchange = () => {
    try { localStorage.setItem("masif-src", $("#src").value); } catch (x) { /* (only for now) */ }
    if (S.rec && !S.rec.capturing) disarm();
    if ($("#src").value === "file") $("#file").click();
  };
  try { const v = localStorage.getItem("masif-src"); if (v && [...$("#src").options].some((o) => o.value === v)) $("#src").value = v; } catch (x) { /* (the first) */ }
  async function inputOf(kind) {                   // the device for LINE IN / MIC / USB (names come once the browser allowed one)
    let devs = await navigator.mediaDevices.enumerateDevices();
    if (!devs.some((d) => d.kind === "audioinput" && d.label)) {
      const t = await navigator.mediaDevices.getUserMedia({ audio: true });
      t.getTracks().forEach((k) => k.stop());
      devs = await navigator.mediaDevices.enumerateDevices();
    }
    return M.pickInput(devs, kind);
  }
  function roomSecs() {                            // what a new 26 kHz 12-bit sample may take of the room left
    const ticked = lib.filter((e) => e.ticked).map((e) => M.size(e, Z).sectors);
    if (ticked.length >= Z.NSLOT) return 0;
    let s = M.ROOM_SECTORS(Z);
    while (s > 0 && !M.fits([...ticked, s], Z)) s--;
    return s * Z.SECT / 1.5 / 26040;
  }
  async function arm() {                           // the SOURCE open and heard on the wave; false: it could not be
    const src = $("#src").value;
    if (src === "file") { $("#file").click(); return false; }
    let stream;
    try {
      const d = await inputOf(src);
      if (!d) { msg({ line: "NO LINE INPUT FOUND", usb: "NO USB AUDIO INPUT FOUND: PLUG IT IN" }[src] || "NO INPUT FOUND"); return false; }
      stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false,
                                                                   deviceId: d.deviceId === "default" ? undefined : { exact: d.deviceId } } });
      S.recFrom = (d.label || "THE DEFAULT INPUT").toUpperCase();
    } catch (x) { msg(x.name === "NotAllowedError" ? "NOT ALLOWED BY THE BROWSER" : `NO INPUT: ${x.message}`.toUpperCase()); return false; }
    stop();
    const ctx = new AudioContext(), url = URL.createObjectURL(new Blob([WORKLET], { type: "text/javascript" }));
    ctx.resume().catch(() => {});                    // (made after awaits: a browser may start it paused)
    await ctx.audioWorklet.addModule(url);
    URL.revokeObjectURL(url);
    const node = new AudioWorkletNode(ctx, "masif-rec"), mute = ctx.createGain();
    mute.gain.value = 0;
    ctx.createMediaStreamSource(stream).connect(node);
    node.connect(mute).connect(ctx.destination);       // (pulled, not heard)
    S.rec = { ctx, stream, chunks: [], n: 0, capturing: false, on: false, pre: [], max: 0, peak: 0, mon: new Float32Array(512), at: 0 };
    node.port.onmessage = (ev) => recFeed(ev.data);
    stream.getAudioTracks()[0].onended = () => S.rec && (S.rec.capturing ? recStop("") : disarm());
    $("#k-arm").classList.add("on");
    msg(`ARMED: ${S.recFrom}`);
    (function watch() {                              // the wave shows what comes in, ~60 times a second
      const r = S.rec;
      if (!r) { draw(); return; }
      if (r.capturing && r.on) { $("#s-time").textContent = clock(r.n / r.ctx.sampleRate); msg(`RECORDING ${(r.n / r.ctx.sampleRate).toFixed(1)} OF ${(r.max / r.ctx.sampleRate).toFixed(1)} S FROM ${S.recFrom}`); }
      draw();
      requestAnimationFrame(watch);
    })();
    return true;
  }
  function disarm() {
    const r = S.rec;
    S.rec = null;
    if (r) { r.stream.getTracks().forEach((t) => t.stop()); r.ctx.close(); }
    $("#k-arm").classList.remove("on"); $("#k-rec").classList.remove("on");
    draw();
  }
  async function recStart() {
    if ($("#src").value === "file") { $("#file").click(); return; }
    const max = roomSecs();
    if (max < 0.05) { msg("THE FM-1'S ROOM IS FULL: UNTICK ONE"); return; }
    if (!S.rec && !(await arm())) return;
    const r = S.rec;
    Object.assign(r, { capturing: true, on: thrOff(), chunks: [], n: 0, pre: [], max: Math.floor(max * r.ctx.sampleRate) });
    $("#k-rec").classList.add("on");
    msg(`${r.on ? "RECORDING" : "WAITING FOR THE SOUND"} FROM ${S.recFrom}`);
  }
  function recFeed(m) {
    const r = S.rec;
    if (!r) return;
    let pk = 0;
    for (let i = 0; i < m.length; i++) pk = Math.max(pk, Math.abs(m[i]));
    r.mon[r.at] = pk; r.at = (r.at + 1) % r.mon.length;
    if (!r.capturing) return;
    if (!r.on) {                                     // waiting: 20 ms kept before the sound passed the threshold
      r.pre.push(m);
      while (r.pre.length * m.length > r.ctx.sampleRate / 50) r.pre.shift();
      if (20 * Math.log10(pk + 1e-9) < S.thr) return;
      r.on = true; r.chunks = r.pre; r.n = r.pre.reduce((s, c) => s + c.length, 0);
      return;
    }
    r.chunks.push(m); r.n += m.length;
    if (r.n >= r.max) recStop("THE FM-1'S ROOM IS FULL");
  }
  function recStop(why) {
    const r = S.rec;
    if (!r) return;
    const got = r.capturing && r.on && r.n;
    disarm();
    if (!got) { msg("NOTHING RECORDED"); return; }
    const x = new Float32Array(Math.min(r.n, r.max));
    let o = 0;
    for (const c of r.chunks) { if (o >= x.length) break; x.set(c.subarray(0, x.length - o), o); o += c.length; }
    localStorage.setItem("masif-recno", ++recNo);
    const e = M.entry(x, r.ctx.sampleRate, `REC ${recNo}`);
    e.ticked = tickable(e);
    lib.push(e); save(e); saveOrder();
    if (e.ticked) S.dirty = true;
    open(lib.length - 1);
    msg(`${(x.length / r.ctx.sampleRate).toFixed(2)} S RECORDED${why ? ": " + why : ""}`);
  }
  $("#k-rec").onclick = () => (S.rec && S.rec.capturing ? recStop("") : recStart());
  $("#k-arm").onclick = () => (S.rec ? (S.rec.capturing ? recStop("") : disarm()) : arm());
  // the threshold: dragged on the wave while armed (up: louder before REC starts; to the middle: at once)
  wave.addEventListener("pointerdown", (ev) => {
    if (!S.rec) return;
    const set = (mv) => {
      const r = wave.getBoundingClientRect(), a = Math.abs((mv.clientY - r.top) / r.height * 2 - 1);
      S.thr = a < 0.0012 ? -62 : clamp(Math.round(20 * Math.log10(a)), -60, -1);
      try { localStorage.setItem("masif-thr", S.thr); } catch (x) { /* (only for now) */ }
      msg(thrOff() ? "THRESHOLD: AT ONCE" : `THRESHOLD: ${S.thr} DB`);
    };
    wave.setPointerCapture(ev.pointerId); set(ev);
    wave.onpointermove = set;
    wave.onpointerup = () => { wave.onpointermove = null; };
  });

  // ---- the pool: the list, its ticks, the gauge. The ticked ones in the list's order are the FM-1's samples
  // (after the firmware's own waves: S.kit), what an instrument's SAMPLE is set to
  const slotOf = (i) => lib.slice(0, i).filter((e) => e.ticked).length;
  const roomOf = (list) => list.reduce((s, e) => s + M.size(e, Z).sectors, 0);
  function tickable(e) {
    const on = lib.filter((x) => x.ticked && x !== e);
    return on.length < Z.NSLOT && M.fits([...on, e].map((x) => M.size(x, Z).sectors), Z);
  }
  function tick(i) {                               // never past the room or the places: the gauge says no
    const e = lib[i];
    if (!e.ticked && !tickable(e)) {
      const g = $("#gauge");
      g.classList.remove("no"); void g.offsetWidth; g.classList.add("no");
      const big = Math.max(...Z.ROOMS_MASIF.map(([lo, hi]) => hi - lo)) / (Z.bytesOf(1280, e.bits) / 1280) / (e.rate || 26040) * M.speedOf(e.speed);
      msg(lib.filter((x) => x.ticked).length >= Z.NSLOT ? "ALL 24 PLACES TAKEN"
          : M.size(e, Z).secs > big ? `TOO LONG: ONE SAMPLE IS ${big.toFixed(2)} S AT THE MOST${S.link ? "" : " (CONNECT: MORE ON MASIF'S NEWER FIRMWARE)"}`
          : `NO ROOM: ${e.name} NEEDS ${M.size(e, Z).secs.toFixed(2)} S`);
      return;
    }
    e.ticked = !e.ticked;
    S.dirty = true; save(e); saveOrder();
    list(); fields();
  }
  function gauge() {
    const on = lib.filter((e) => e.ticked), sec = (s) => (s * Z.SECT / 1.5 / 26040).toFixed(1), all = M.ROOM_SECTORS(Z), used = roomOf(on);
    $("#room-used").textContent = sec(used); $("#room-all").textContent = sec(all); $("#room-n").textContent = on.length;
    $("#room-bar").style.width = `${Math.min(100, used / all * 100)}%`;
    $("#gauge").classList.toggle("full", used / all > 0.92);
  }
  function list() {
    const rows = [];
    $("#pool-title").textContent = { pool: "SAMPLE POOL", songs: "SONGS ON THE FM-1", themes: "THEMES" }[S.tab];
    if (S.tab === "pool") {
      lib.forEach((e, i) => {
        const li = el("li", (e.ticked ? "" : "off") + (i === S.sel ? " sel" : "")), t = el("span", "tick" + (e.ticked ? " on" : "")), z = M.size(e, Z);
        const there = e.ticked && e.dev && e.dev.ver === e.ver;
        t.onclick = (ev) => { ev.stopPropagation(); tick(i); };
        li.append(t, el("span", "slot", e.ticked ? `SMP ${hex(S.kit + slotOf(i))}${there ? "" : "*"}` : "--"), el("span", "nm", e.name),
                  el("span", "", e.loopOn ? "L" : " "), el("span", "", `${Math.round(e.rate / 1000)}K`), el("span", "", `${e.bits}B`), el("span", "", z.secs.toFixed(2)));
        li.onclick = () => open(i);
        rows.push(li);
      });
      if (!lib.length) rows.push(el("li", "off", "DROP SOUND FILES HERE, + ADD, OR RECORD"));
      if (!S.busy && S.dirty && lib.some((e) => e.ticked || e.dev)) msg("NOT SYNCED YET (*)");
    } else if (S.tab === "songs") {
      if (!S.songs) rows.push(el("li", "off", S.link ? "READING..." : "CONNECT TO SEE THE FM-1'S SONGS"));
      else S.songs.slots.forEach((s, i) => {
        const li = el("li", s ? "" : "off");
        li.append(el("span", "slot", `SONG ${String(i + 1).padStart(2, "0")}`), el("span", "nm", s ? s.name || "(NO NAME)" : "(EMPTY)"), el("span", "", s ? `${(s.bytes / 1024).toFixed(1)}K` : ""));
        rows.push(li);
      });
    } else {
      THEMES.forEach((n) => { const li = el("li"); li.append(el("span", "slot", "FM-1"), el("span", "nm", n)); rows.push(li); });
      SKINS.forEach(([n, , t]) => { const li = el("li"); li.append(el("span", "slot", "WEB"), el("span", "nm", t)); li.onclick = () => setSkin(n); rows.push(li); });
    }
    $("#list").replaceChildren(...rows);
    gauge();
  }
  function open(i) { stop(); S.sel = i; S.v0 = 0; zoomTo(1); fields(); draw(); list(); }
  // the FM-1's samples taken out of the list on purpose: not fetched back at the next SYNC, which takes them off it
  let gone = [];
  try { gone = JSON.parse(localStorage.getItem("masif-gone") || "[]"); } catch (x) { /* none kept */ }
  const keepGone = () => { try { localStorage.setItem("masif-gone", JSON.stringify(gone)); } catch (x) { /* (only for now) */ } };
  $("#l-rm").onclick = () => {
    const e = cur();
    if (!e || !confirm(`${e.name} out of the list${e.dev ? " (and off the FM-1 at the next SYNC)" : ""}?`)) return;
    if (e.dev) { gone.push({ off: e.dev.off, n: e.dev.n }); keepGone(); }
    lib.splice(S.sel, 1); tx("lib", "readwrite", (s) => s.delete(e.id));
    if (e.ticked || e.dev) S.dirty = true;
    saveOrder(); open(Math.min(S.sel, lib.length - 1));
  };
  const move = (d) => { const i = S.sel, j = i + d; if (!cur() || j < 0 || j >= lib.length) return; [lib[i], lib[j]] = [lib[j], lib[i]]; if (lib[i].ticked || lib[j].ticked) S.dirty = true; saveOrder(); open(j); };
  $("#l-up").onclick = () => move(-1); $("#l-dn").onclick = () => move(1);
  $("#l-name").onclick = () => { const e = cur(); if (!e) return; const n = prompt("Its name (8 letters on the FM-1):", e.name); if (n) { e.name = Z.cleanName(n); changed(e, false); fields(); list(); } };
  function download(u8, name) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([u8])); a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }
  $("#l-wav").onclick = () => { const e = cur(); if (!e) return; const s = M.encode(e, Z); download(Z.wav(s.y, Math.round(s.rate / M.speedOf(e.speed))), e.name.toLowerCase().replace(/\W+/g, "-") + ".wav"); };

  // ---- the FM-1: connect (its samples come into the list), SYNC POOL, BACKUP / RESTORE, its songs
  async function busy(what, f) {
    if (S.busy) return;
    S.busy = true; document.body.classList.add("busy");
    try { await f(); } catch (x) { msg(String(x.message || x).toUpperCase()); status("ERROR"); }
    S.busy = false; document.body.classList.remove("busy");
    status(S.link ? `CONNECTED ${devName(S.link.info)} · ${(M.ROOM_SECTORS(Z) * Z.SECT / 1.5 / 26040).toFixed(1)} S` : "NOT CONNECTED");
    list();
  }
  $("#connect").onclick = () => busy("connect", async () => {
    if (!navigator.requestMIDIAccess) { msg("THIS BROWSER HAS NO WEB MIDI: CHROME OR EDGE"); return; }
    const access = await navigator.requestMIDIAccess({ sysex: true });
    const isF = (p) => /felucca|fm-?1|zp12|masif/i.test(p.name || "") && p.state !== "disconnected";
    const input = [...access.inputs.values()].find(isF), output = [...access.outputs.values()].find(isF);
    if (!input || !output) { msg("NO FM-1 FOUND: PLUG IT IN, CLOSE OTHER MIDI APPS"); return; }
    const l = new Z.Link((d) => output.send(d));
    input.onmidimessage = (ev) => l.receive(ev.data);
    const h = await l.hello();
    if (h.rooms !== 1) { msg(`THIS FM-1 RUNS ${devName(h)}: INSTALL MASIF FIRST`); return; }
    S.link = l; S.kit = h.kit; localStorage.setItem("masif-kit", h.kit);
    try { localStorage.setItem("masif-app", JSON.stringify(h.app ? { lo: h.app.lo, hi: h.app.hi } : null)); } catch (x) { /* (only a convenience) */ }
    $("#lamp").classList.add("on");
    access.onstatechange = () => { if (!isF(input)) { S.link = null; S.songs = null; $("#lamp").classList.remove("on"); status("DISCONNECTED"); } };
    const got = await pull(l);
    const order = S.dir.slots.filter(Boolean).map((s) => lib.find((e) => e.dev && e.dev.off === s.off && e.dev.n === s.n));
    const same = order.every((e, i) => e && e.ticked && e.dev.ver === e.ver && lib.filter((x) => x.ticked)[i] === e) && lib.filter((x) => x.ticked).length === order.length;
    S.dirty = !same; saveOrder();
    msg(got.length ? `${got.length} SAMPLE${got.length > 1 ? "S" : ""} FROM THE FM-1 INTO THE LIST` : same ? "IN SYNC" : "NOT SYNCED YET (*)");
    if (S.sel < 0 && lib.length) open(0); else { fields(); list(); }   // (list: the gauge with the FM-1's rooms)
    S.songs = await l.songs().catch(() => null);
  });
  // what the FM-1 has and the list has not (made on the FM-1: its sampler) comes into the list, ticked: at connect,
  // and again first at every SYNC, so a sync never takes off what was recorded on the device meanwhile
  async function pull(l) {
    status("READING THE POOL");
    S.dir = await Z.readDir(l);
    for (const e of lib) if (e.dev && !S.dir.slots.some((s) => s && s.off === e.dev.off && s.n === e.dev.n)) e.dev = null;
    gone = gone.filter((g) => S.dir.slots.some((s) => s && s.off === g.off && s.n === g.n)); keepGone();
    const got = await M.adopt(l, Z, S.dir, lib, (f, n) => status(`FETCHING ${n} ${Math.round(f * 100)}%`), gone);
    for (const e of got) { lib.push(e); save(e); }
    if (got.length) saveOrder();
    return got;
  }
  $("#sync").onclick = () => busy("sync", async () => {
    if (!S.link) { msg("CONNECT FIRST"); return; }
    stop();
    const got = await pull(S.link);
    const want = lib.filter((e) => e.ticked);
    await M.sync(S.link, Z, want, (f, n) => status(`SYNC ${n} ${Math.round(f * 100)}%`));
    lib.forEach((e) => { if (!e.ticked) e.dev = null; save(e); });
    gone = []; keepGone();
    S.dirty = false; saveOrder();
    msg(`${got.length ? `${got.length} FROM THE FM-1 INTO THE LIST · ` : ""}IN SYNC: ${want.length} SAMPLE${want.length === 1 ? "" : "S"} ON THE FM-1`);
    fields();
  });
  $("#backup").onclick = () => busy("backup", async () => {
    if (!S.link) { msg("CONNECT FIRST"); return; }
    const f = await Z.backup(S.link, ["masif"], (a, b) => status(`BACKUP ${Math.round(a / b * 100)}%`));
    download(f, `masif-${new Date().toISOString().slice(0, 10)}.masifbak`);
    msg("BACKUP SAVED");
  });
  $("#restore").onclick = () => { if (!S.link) { msg("CONNECT FIRST"); return; } $("#bakfile").click(); };
  $("#bakfile").onchange = () => {
    const f = $("#bakfile").files[0];
    $("#bakfile").value = "";
    if (!f || !confirm("Everything on the FM-1 (samples, songs) becomes what this file has. Go on?")) return;
    busy("restore", async () => {
      await Z.restore(S.link, new Uint8Array(await f.arrayBuffer()), ["masif"], (a, b) => status(`RESTORE ${Math.round(a / b * 100)}%`));
      S.link = null; $("#lamp").classList.remove("on");
      msg("RESTORED: THE FM-1 STARTS AGAIN. CONNECT ONCE IT HAS");
    });
  };

  // ---- the skins, the tabs, the help box
  function switcher(id, then) {
    $$(`#${id} button`).forEach((b) => b.onclick = () => {
      $$(`#${id} button`).forEach((x) => x.classList.toggle("on", x === b));
      then(b.dataset.v);
    });
  }
  function setSkin(n) {
    const k = SKINS.find((s) => s[0] === n || s[2] === String(n).toUpperCase()) || SKINS[0];   // (by key or name; one no longer here: the first)
    document.body.dataset.skin = k[0]; document.body.dataset.family = k[1];
    $$("#skin button").forEach((b) => b.classList.toggle("on", b.dataset.v === k[0]));
    try { localStorage.setItem("masif-skin", k[0]); } catch (x) { /* (no storage: only for now) */ }
    draw();
  }
  $("#skin").replaceChildren(...SKINS.map(([n, , t]) => {
    const b = el("button");
    b.dataset.v = n; b.title = t; b.dataset.help = `Theme: ${t}.`;
    document.body.dataset.skin = n; b.style.background = css("--sw");
    b.onclick = () => setSkin(n);
    return b;
  }));
  switcher("tabs", (v) => { S.tab = v; list(); });
  // SAMPLER / GUIDE (the user, 2026-10-24): the guide's contents scroll its text, and say where it is
  function setMode(v) {
    document.body.dataset.mode = v;
    $$("#mode button").forEach((b) => b.classList.toggle("on", b.dataset.v === v));
    if (v === "sampler") draw();
  }
  switcher("mode", setMode);
  const guide = $("#guide-text"), heads = $$("#guide-text h1[id]");
  $$("#toc a").forEach((a) => a.onclick = (ev) => {
    ev.preventDefault();
    const h = $(a.getAttribute("href"));
    if (h) guide.scrollTo({ top: h.offsetTop - 10 });
    history.replaceState(null, "", a.getAttribute("href"));
  });
  $$(".guide-link").forEach((a) => a.onclick = (ev) => {    // (the ABOUT box: the guide at its credits)
    ev.preventDefault();
    setMode("guide");
    const h = $(a.getAttribute("href"));
    if (h) guide.scrollTo({ top: h.offsetTop - 10 });
  });
  guide.addEventListener("scroll", () => {
    let at = heads[0];
    for (const h of heads) if (h.offsetTop - guide.scrollTop <= 40) at = h;
    $$("#toc a").forEach((a) => a.classList.toggle("on", a.getAttribute("href") === "#" + at.id));
  });
  // what the mouse is over does: in the top bar. One size for every help: the largest the longest one fits the
  // box at (the box never changes size, so nothing on the page moves when a help shows)
  function sizeHelp() {
    const h = $("#help"), was = h.textContent;
    const longest = $$("[data-help]").map((x) => x.dataset.help).reduce((a, b) => (b.length > a.length ? b : a), "");
    h.textContent = longest;
    let px = 22;
    for (; px > 9; px -= 0.5) { h.style.fontSize = `${px}px`; if (h.scrollHeight <= h.clientHeight + 1 && h.scrollWidth <= h.clientWidth + 1) break; }
    h.textContent = was;
  }
  function fitHelp(t) { const h = $("#help"); h.classList.toggle("on", !!t); if (h.textContent !== t) h.textContent = t; }
  document.addEventListener("mouseover", (ev) => { const h = ev.target.closest("[data-help]"); fitHelp(h ? h.dataset.help : ""); });
  addEventListener("resize", sizeHelp);

  // ---- begin
  (async () => {
    const q = new URLSearchParams(location.search);
    // a different skin every time the page opens (the user, 2026-10-14): any but the last one; ?skin= picks one
    let last = null;
    try { last = localStorage.getItem("masif-skin"); } catch (x) { /* (no storage: any) */ }
    const others = SKINS.filter((k) => k[0] !== last);
    setSkin(q.get("theme") || q.get("skin") || others[Math.floor(Math.random() * others.length)][0]);
    if (q.has("guide") || location.hash.startsWith("#g-")) {    // (?guide or a link to one of its parts: the guide)
      setMode("guide");
      const h = location.hash.startsWith("#g-") && $(location.hash);
      if (h) guide.scrollTo({ top: h.offsetTop - 10 });
    }
    if (!q.get("demo")) await load();
    else {                                           // made-up sounds, for pictures of the page (not kept)
      const R = 44100, mk = (n, f) => Float32Array.from({ length: n }, (_, i) => f(i / R, i));
      const demo = [["AMENCHOP", mk(R * 1.5, (t) => (Math.random() * 2 - 1) * Math.exp(-((t * 6) % 1) * 7) * 0.8)],
                    ["SUB C1", mk(R * 0.8, (t) => Math.sin(2 * Math.PI * 32.7 * t) * Math.exp(-t * 2))],
                    ["REESE", mk(R * 1.2, (t) => (((t * 55) % 1) + ((t * 55.6) % 1) - 1) * 0.6)],
                    ["STAB DM", mk(R * 0.5, (t) => (Math.sin(2 * Math.PI * 293 * t) + Math.sin(2 * Math.PI * 349 * t) + Math.sin(2 * Math.PI * 440 * t)) / 3 * Math.exp(-t * 6))],
                    ["VOX AH", mk(R * 0.9, (t) => Math.sin(2 * Math.PI * 220 * t + 3 * Math.sin(2 * Math.PI * 5 * t)) * Math.min(1, t * 8) * 0.7)]];
      demo.forEach(([n, x], i) => { const e = M.entry(x, R, n); e.ticked = i < 3; if (i === 2) { e.loopOn = true; e.play = 2; e.la = Math.round(R * 0.3); e.lb = Math.round(R * 1.0); e.fadeOn = true; e.tin = Math.round(R * 0.08); e.tout = Math.round(R * 0.12); } lib.push(e); });
    }
    sizeHelp();
    if (lib.length) open(clamp(+(q.get("sel") || 0), 0, lib.length - 1)); else { fields(); draw(); list(); }
    if (q.get("zoom")) zoomTo(+q.get("zoom"));
    if (q.get("help") && $("#" + q.get("help"))) fitHelp($("#" + q.get("help")).closest("[data-help]").dataset.help);   // (pictures: one's help shown)
    addEventListener("resize", draw);
  })();
})();

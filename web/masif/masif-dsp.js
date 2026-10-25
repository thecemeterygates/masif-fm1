// SPDX-License-Identifier: GPL-3.0-only
// MASIF's web app, the part without a page: a sample as the library keeps it (the sound as it came, mono, at its
// own rate, and what is to be done with it), what the processing makes of it, the FM-1's form of it, the room it
// takes, and SYNC POOL (the ticked samples onto the FM-1, in the list's order). Used by masif.js in the browser and
// by tests/masif_web_test.mjs in node against the firmware's link code. Needs ../zp12link.js (Z).
"use strict";
(function (root) {
  // the play modes, in the firmware's order (masif_tracker.h masif_play_t); the first two do not loop
  const PLAY = ["FWD", "REV", "FWDLOOP", "REVLOOP", "FWD PP", "REV PP", "OSC", "OSC REV", "OSC PP", "REPITCH", "REP PP", "BPM", "BPM PP"];
  const SPEEDS = ["1×", "45→33", "×2", "×2.7"];       // stored fast, played slow (zp12): flags bit 0 45->33, bit 1 x2
  // every fader at its zero: nothing done (GAIN, STRETCH, PITCH: 0 in their middle; the rest at the bottom)
  const PROC0 = { gain: 50, fin: 0, fout: 0, comp: 0, stretch: 50, grain: 0, pitch: 50, drive: 0 };
  const speedOf = (sp) => (sp & 1 ? 45 / 33 : 1) * (sp & 2 ? 2 : 1);

  // a new entry of the library: x mono -1..1 at `rate`; the silence before and after off (as zp12's editor)
  function entry(x, rate, name) {
    let a = 0, b = x.length;
    while (a < b && Math.abs(x[a]) < 0.003) a++;
    while (b > a && Math.abs(x[b - 1]) < 0.003) b--;
    if (b - a < 64) { a = 0; b = x.length; }
    return { id: Math.random().toString(36).slice(2) + Date.now().toString(36), name: String(name || "SAMPLE").toUpperCase().replace(/[^ -~]/g, "").trim().slice(0, 8) || "SAMPLE",
             x, srcRate: rate, start: a, end: b, loopOn: false, la: a + ((b - a) >> 2), lb: b, play: 0, fadeOn: false, tin: 0, tout: 0,
             proc: { ...PROC0 }, rev: false, norm: true, rate: 26040, speed: 0, bits: 12, ticked: false, dev: null, ver: 1 };
  }

  // ---- the processing (the PROCESS window's faders: 0..100, 50 the middle)
  const stretchOf = (p) => Math.pow(2, (p.stretch - 50) / 50);          // 0.5 .. 2 times as long
  const pitchOf = (p) => Math.pow(2, Math.round((p.pitch - 50) / 50 * 12) / 12);   // -12 .. +12 semitones, whole ones
  function stretch(x, f, grain, rate) {             // old-school: Hann grains laid down further apart (or closer)
    if (Math.abs(f - 1) < 0.01) return x;
    const g = Math.max(64, Math.round((0.01 + grain / 100 * 0.19) * rate)), ho = g >> 1, hi = ho / f;
    const y = new Float32Array(Math.max(1, Math.round(x.length * f))), ws = new Float32Array(y.length), w = new Float32Array(g);
    for (let i = 0; i < g; i++) w[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / g);
    for (let k = 0; k * ho < y.length; k++) {
      const ip = Math.round(k * hi), op = k * ho;
      for (let i = 0; i < g && op + i < y.length; i++) { const s = ip + i < x.length ? x[ip + i] : 0; y[op + i] += s * w[i]; ws[op + i] += w[i]; }
    }
    for (let i = 0; i < y.length; i++) if (ws[i] > 1e-3) y[i] /= ws[i];
    return y;
  }
  function repitch(x, r) {                           // r > 1: higher and shorter (straight interpolation)
    if (Math.abs(r - 1) < 1e-4) return x;
    const y = new Float32Array(Math.max(1, Math.floor(x.length / r)));
    for (let i = 0; i < y.length; i++) { const t = i * r, k = Math.floor(t), f = t - k; y[i] = (x[k] || 0) * (1 - f) + (x[k + 1] || 0) * f; }
    return y;
  }
  function compress(x, amt, rate) {                  // a peak follower: the loud parts held down
    const thr = Math.pow(10, (-6 - 30 * amt) / 20), ratio = 1 + 7 * amt, att = Math.exp(-1 / (0.003 * rate)), rel = Math.exp(-1 / (0.1 * rate));
    let env = 0;
    for (let i = 0; i < x.length; i++) {
      const a = Math.abs(x[i]);
      env = a > env ? att * env + (1 - att) * a : rel * env + (1 - rel) * a;
      if (env > thr) x[i] *= Math.pow(env / thr, 1 / ratio - 1);
    }
  }
  // the entry as it will be (at its own rate, before the FM-1's form): the part between start and end, processed,
  // its loop where it is then (la, lb inside it) and its tapers (ti, to: samples after its start, before its end;
  // not in the sound: the FM-1 plays them, `taper` lets the page hear them). proc false: as it came
  function render(e, proc = true) {
    let x = Float32Array.from(e.x.subarray(e.start, e.end));
    const L0 = x.length;
    let la = Math.max(0, Math.min(L0, e.la - e.start)), lb = Math.max(0, Math.min(L0, e.lb - e.start)), scale = 1;
    let ti = e.loopOn && e.fadeOn ? e.tin : 0, to = e.loopOn && e.fadeOn ? e.tout : 0;
    if (proc && e.rev) { x.reverse(); [la, lb] = [L0 - lb, L0 - la]; [ti, to] = [to, ti]; }
    const p = e.proc;
    if (proc) {
      const g = Math.pow(10, (p.gain - 50) / 50 * 24 / 20);
      if (g !== 1) for (let i = 0; i < x.length; i++) x[i] *= g;
      if (p.drive) { const d = 1 + p.drive / 100 * 9, n = Math.tanh(d); for (let i = 0; i < x.length; i++) x[i] = Math.tanh(d * x[i]) / n; }
      if (p.comp) compress(x, p.comp / 100, e.srcRate);
      const f = stretchOf(p), r = pitchOf(p);
      x = repitch(stretch(x, f, p.grain, e.srcRate), r);
      scale = x.length / L0;
      const fi = Math.round(p.fin / 100 * 0.5 * x.length), fo = Math.round(p.fout / 100 * 0.5 * x.length);
      for (let i = 0; i < fi; i++) x[i] *= i / fi;
      for (let i = 0; i < fo; i++) x[x.length - 1 - i] *= i / fo;
    }
    la = Math.round(la * scale); lb = Math.min(x.length, Math.round(lb * scale));
    return { x, rate: e.srcRate, la, lb, ti: Math.round(ti * scale), to: Math.round(to * scale) };
  }
  // the loop's tapers on a sound (as the FM-1 plays them: the level rises over ti samples after la, falls over to
  // before lb, every time round)
  function taper(x, la, lb, ti, to) {
    for (let i = 0; i < ti && la + i < lb; i++) x[la + i] *= i / ti;
    for (let i = 0; i < to && lb - 1 - i >= la; i++) x[lb - 1 - i] *= i / to;
    return x;
  }
  // a taper as the FM-1 keeps it: 0..128 in 1/256 of the loop (half the loop at the most)
  const t256 = (t, len) => (len > 0 ? Math.max(0, Math.min(128, Math.round(t / len * 256))) : 0);
  // the FM-1's form: 12-bit values at its rate (Z.encode: the low-pass, normalised if wanted), its loop in them
  function encode(e, Z) {
    const r = render(e), x = r.x;
    const fi = e.start > 0 ? Math.min(x.length >> 2, Math.round(e.srcRate * 0.001)) : 0;          // trimmed inside the sound: no click
    const fo = e.end < e.x.length ? Math.min(x.length >> 2, Math.round(e.srcRate * 0.002)) : 0;
    for (let i = 0; i < fi; i++) x[i] *= i / fi;
    for (let i = 0; i < fo; i++) x[x.length - 1 - i] *= i / fo;
    const y = Z.encode(x, e.srcRate, { rate: e.rate, slow: !!(e.speed & 1), oct: !!(e.speed & 2), normalize: e.norm, fade: false });
    let loop = null, tp = null;
    if (e.loopOn && x.length) {
      const a = Math.round(r.la * y.length / x.length), b = Math.min(y.length, Math.round(r.lb * y.length / x.length));
      if (b > a + 1) loop = [a, b];
      if (loop && (r.ti || r.to)) tp = [t256(r.ti, r.lb - r.la), t256(r.to, r.lb - r.la)];
    }
    return { y, rate: e.rate, slow: !!(e.speed & 1), oct: !!(e.speed & 2), bits: e.bits, loop, taper: tp, play: playOf(e), name: e.name };
  }
  // the play mode an instrument begins with: once (FWD), or FWDLOOP with LOOP on (the user, 2026-10-14: no
  // play-mode choice on the web; the instrument page on the FM-1 has all of them)
  const playOf = (e) => (e.loopOn ? 2 : 0);

  // ---- SOURCE: which of this computer's audio inputs LINE IN, MIC and USB mean, by the names the browser gives
  // them (Windows "Line In (Realtek ...)", "Microphone Array", "Microphone (Scarlett 2i2 USB)"; macOS "MacBook Pro
  // Microphone", "External Microphone" (the 1/8" jack); Linux "Built-in Audio Analog Stereo", "Line In"). A USB
  // device is USB whatever else its name says. devs: enumerateDevices()'s audio inputs. null: none fits
  const USB_RE = /usb|fm-?1|felucca|m-?vave|interface|scarlett|focusrite|clarett|behringer|u-?phoria|umc ?\d|steinberg|ur ?\d\d|motu|presonus|audiobox|studio ?\d|audient|evo ?\d|tascam|zoom|roland|rubix|yamaha|ag0\d|m-?audio|komplete|native instruments|universal audio|volt|apollo|ssl ?\d|\bid ?\d\d?\b|arturia|minifuse|mackie|\brode|blue|yeti|shure|elgato|apogee|rme|babyface|antelope|irig|ik multimedia|maono|fifine|hyperx|razer|samson/i;
  const LINE_RE = /line|external mic|aux|jack|rear|front|analog in/i;
  const MIC_RE = /mic|internal|built-?in|array|webcam|camera|headset|default|analog/i;
  function kindOf(d) {
    const l = d.label || "";
    return USB_RE.test(l) ? "usb" : LINE_RE.test(l) ? "line" : MIC_RE.test(l) ? "mic" : "line";
  }
  function pickInput(devs, kind) {
    const real = devs.filter((d) => d.kind === "audioinput" && d.deviceId && d.deviceId !== "default" && d.deviceId !== "communications");
    const fit = real.filter((d) => kindOf(d) === kind);
    if (fit.length) return fit[0];
    if (kind === "mic") return devs.find((d) => d.kind === "audioinput" && d.deviceId === "default") || real.find((d) => kindOf(d) !== "usb") || null;
    return null;
  }
  // what it takes on the FM-1 without encoding it: its samples there, its bytes, whole sectors
  function size(e, Z) {
    const p = e.proc, L = (e.end - e.start) * stretchOf(p) / pitchOf(p);
    const n = Math.floor(L * e.rate / (e.srcRate * speedOf(e.speed)));
    const bytes = Z.bytesOf(n, e.bits);
    return { n, bytes, sectors: Math.ceil(bytes / Z.SECT), secs: n / e.rate * speedOf(e.speed) };
  }
  // do these fit the FM-1's sample rooms, each in one piece? (as the upload places them: the biggest first, each
  // into the smallest piece left that holds it)
  function fits(sectors, Z) {
    const free = Z.ROOMS_MASIF.map(([lo, hi]) => (hi - lo) / Z.SECT);
    for (const s of [...sectors].sort((a, b) => b - a)) {
      let k = -1;
      free.forEach((f, i) => { if (f >= s && (k < 0 || f < free[k])) k = i; });
      if (k < 0) return false;
      free[k] -= s;
    }
    return true;
  }
  const ROOM_SECTORS = (Z) => Z.ROOMS_MASIF.reduce((s, [lo, hi]) => s + (hi - lo) / Z.SECT, 0);

  // ---- SYNC POOL: the FM-1's pool becomes the ticked entries, in the list's order (its samples 0, 1 ... after the
  // firmware's own waves). What is there already and unchanged stays where it is; what is gone is taken off first
  // (so its room is free), then the new ones are written, then the directory in the list's order. If they do not
  // fit around what stays, everything is written again from the start of the rooms.
  async function sync(link, Z, want, progress = () => {}) {
    if (want.length > Z.NSLOT) throw new Error(`the FM-1 has ${Z.NSLOT} places`);
    let d = await Z.readDir(link);
    const same = (e, s) => s && e.dev && s.off === e.dev.off && s.n === e.dev.n && e.dev.ver === e.ver;
    let keep = want.map((e) => d.slots.find((s) => same(e, s)) || null);
    const enc = new Map();
    for (const e of want) if (!keep[want.indexOf(e)]) enc.set(e, encode(e, Z));
    const plan = (kept) => {                         // where the new ones go around `kept` (null: they do not fit)
      const t = { slots: Array(Z.NSLOT).fill(null) };
      kept.forEach((s, i) => { if (s) t.slots[i] = s; });
      const todo = want.map((e, i) => [e, i]).filter(([, i]) => !kept[i]).map(([e]) => e);
      todo.sort((a, b) => Z.bytesOf(enc.get(b).y.length, enc.get(b).bits) - Z.bytesOf(enc.get(a).y.length, enc.get(a).bits));
      for (const e of todo) {
        const s = enc.get(e), at = Z.alloc(t, Z.bytesOf(s.y.length, s.bits), "masif"), k = t.slots.indexOf(null);
        if (at < 0 || k < 0) return null;
        t.slots[k] = { off: at, n: s.y.length, flags: Z.flagsOfBits(s.bits) };
      }
      return todo;
    };
    let todo = plan(keep);
    if (!todo) {
      keep = want.map(() => null);
      for (const e of want) if (!enc.has(e)) enc.set(e, encode(e, Z));
      todo = plan(keep);
      if (!todo) throw new Error("they do not fit the FM-1, each in one piece: untick one");
    }
    // 1. off the FM-1: what is not kept (its room is free from here)
    const kept = new Set(keep.filter(Boolean));
    if (d.slots.some((s) => s && !kept.has(s)) || d.slots.some((s, i) => s && kept.has(s) && keep.indexOf(s) !== i)) {
      d.slots = d.slots.map((s) => (s && kept.has(s) ? s : null));
      await Z.writeDir(link, d);
    }
    // 2. the new ones (each into the smallest piece that holds it, the biggest first: as planned)
    const total = todo.reduce((n, e) => n + Math.ceil(Z.bytesOf(enc.get(e).y.length, enc.get(e).bits) / Z.SECT), 0) || 1;
    let done = 0;
    const placed = new Map();
    for (const e of todo) {
      const s = enc.get(e), sect = Math.ceil(Z.bytesOf(s.y.length, s.bits) / Z.SECT);
      const k = await Z.upload(link, d, { ...s, dirLater: true }, "masif", (a, b) => progress((done + sect * a / b) / total, e.name));
      placed.set(e, d.slots[k]);
      done += sect;
    }
    // 3. the directory in the list's order, with every sample's name, loop and play mode as they are now
    const slots = want.map((e, i) => {
      const s = keep[i] || placed.get(e), en = enc.get(e);
      const o = { off: s.off, n: s.n, rate: s.rate, flags: s.flags, name: Z.cleanName(e.name) };
      const lp = en ? en.loop : e.dev.loop, pl = en ? en.play : e.dev.play, tp = en ? en.taper : e.dev.taper;
      if (lp) o.loop = lp;
      if (pl) o.play = pl;
      if (tp) o.taper = tp;
      return o;
    });
    d.slots = [...slots, ...Array(Z.NSLOT - slots.length).fill(null)];
    await Z.writeDir(link, d);
    want.forEach((e, i) => { const s = slots[i]; e.dev = { off: s.off, n: s.n, ver: e.ver, loop: s.loop || null, play: s.play || 0, taper: s.taper || null }; });
    progress(1, "");
    return d;
  }
  // what is on the FM-1 and in no entry: entries of it (the sound fetched back, at the rate it plays at)
  // (gone: the FM-1's samples taken out of the list on purpose, {off, n}: they stay out and leave at the next SYNC)
  async function adopt(link, Z, d, lib, progress = () => {}, gone = []) {
    const mine = (s) => lib.some((e) => e.dev && e.dev.off === s.off && e.dev.n === s.n) || gone.some((g) => g.off === s.off && g.n === s.n);
    const out = [], todo = d.slots.map((s, i) => [s, i]).filter(([s]) => s && !mine(s));
    for (let k = 0; k < todo.length; k++) {
      const [s] = todo[k], y = await Z.fetchSample(link, s), x = Float32Array.from(y, (v) => v / 2048);
      const sp = s.flags & 3, e = entry(x, s.rate / speedOf(sp), s.name);
      const la = s.loop ? s.loop[0] : 0, lb = s.loop ? s.loop[1] : x.length, tp = s.taper || [0, 0];
      Object.assign(e, { start: 0, end: x.length, rate: s.rate, speed: sp, bits: Z.bitsOf(s), norm: false, ticked: true,
                         loopOn: !!s.loop, la, lb, play: s.play || 0, fadeOn: !!(tp[0] || tp[1]),
                         tin: Math.round(tp[0] / 256 * (lb - la)), tout: Math.round(tp[1] / 256 * (lb - la)) });
      e.dev = { off: s.off, n: s.n, ver: e.ver, loop: s.loop || null, play: s.play || 0, taper: s.taper || null };
      out.push(e);
      progress((k + 1) / todo.length, s.name);
    }
    return out;
  }

  const api = { PLAY, SPEEDS, PROC0, kindOf, pickInput, taper, t256, speedOf, stretchOf, pitchOf, entry, render, encode, size, fits, ROOM_SECTORS, sync, adopt, playOf };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.MasifDSP = api;
})(typeof self !== "undefined" ? self : this);

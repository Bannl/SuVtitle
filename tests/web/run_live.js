// run_live.js -- the headless runner of SuVtitle's LIVE window (app/web/live.html), in the style of run_ui.js (the
// player's): the page's single inline <script> is lifted out, the DOM is built FROM ITS OWN MARKUP (domstub.js
// buildFromMarkup: every id the script looks up exists exactly as written), and the script runs in a vm in WebView2
// mode -- the REAL bridge path is exercised (window.chrome.webview present), the design mock stays asleep -- on a
// VIRTUAL clock (setTimeout and performance.now are the same clock: "the meter empties 1.2 s after the last level" is
// a fact of the test, not a race).
// L3, 2026-10-25. The clock and matchMedia are run_ui.js's, copied (this runner must not move when the player's does).
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const { El, makeDocument, buildFromMarkup, makeEl } = require("./domstub.js");
const { WEB, lf } = require("./paths.js");

// THE PAGE. SUV_LIVE_PAGE=<file> points the harness at a COPY (the CRLF / LF proof, kept out of the repository).
const LIVE_PAGE = process.env.SUV_LIVE_PAGE ? path.resolve(process.env.SUV_LIVE_PAGE) : path.join(WEB, "live.html");
const readLivePage = () => lf(fs.readFileSync(LIVE_PAGE, "utf8"));

// A virtual clock: setTimeout/clearTimeout that queue rather than fire, plus pump(ms) to advance time. Callbacks
// scheduled BY a callback are picked up by the same pump, so a two-beat sequence runs in one call.
function makeClock() {
  let now = 0, seq = 0;
  const q = new Map();                                   // id -> { due, fn, args }
  const setT = (fn, ms, ...args) => {
    const id = ++seq;
    q.set(id, { due: now + Math.max(0, Number(ms) || 0), fn, args });
    return id;
  };
  const clearT = (id) => { q.delete(id); };
  const pump = (ms) => {
    const target = now + Math.max(0, Number(ms) || 0);
    let fired = 0;
    for (;;) {
      let pick = null;
      q.forEach((t, id) => { if (t.due <= target && (!pick || t.due < pick.t.due)) pick = { id, t }; });
      if (!pick) break;
      q.delete(pick.id);
      now = pick.t.due;                                  // the callback sees the time it was due
      pick.t.fn(...pick.t.args);
      fired++;
    }
    now = target;
    return fired;
  };
  return { setTimeout: setT, clearTimeout: clearT, pump, pending: () => q.size, now: () => now };
}

// window.matchMedia, the smallest form: "(prefers-reduced-motion: reduce)" answers a live box (a test may turn the
// setting on mid-run), anything else false.
function makeMedia() {
  const box = { reduceMotion: false };
  const matchMedia = (q) => ({
    media: String(q),
    matches: /prefers-reduced-motion:\s*reduce/.test(String(q)) && box.reduceMotion,
    addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
  });
  return { box, matchMedia };
}

// opts.html: ANOTHER version of the page. opts.mutate(script) -> script: a MUTANT (it must change the script).
// opts.ls: what the profile's localStorage holds at boot. opts.noWebView: a plain BROWSER (the design mock runs);
// opts.hash: its address's #fragment. opts.noClock: a host with no performance.now (the typewriter keeps its seed).
function runLive(opts = {}) {
  const html = lf(typeof opts.html === "string" ? opts.html : fs.readFileSync(LIVE_PAGE, "utf8"));
  const m = html.match(/<script>([\s\S]*?)<\/script>/);
  if (!m) throw new Error("live.html: no inline <script> to run");
  let script = m[1];
  if (opts.mutate) {
    const mutated = opts.mutate(script);
    if (typeof mutated !== "string" || mutated === script)
      throw new Error("mutation did not apply" + (opts.name ? ": " + opts.name : ""));
    script = mutated;
  }

  const { root, ids } = buildFromMarkup(html, makeEl);
  El._active = null;                       // a new page: nothing of the previous one keeps the focus
  const doc = makeDocument(ids);
  doc.documentElement.appendChild(root);
  // document.body is the stub's root element; the page's <body class="..."> classes are carried over to it
  const walk = (n) => (n.tagName === "BODY" ? n : n.children.map(walk).find(Boolean) || null);
  const bodyEl = walk(root);
  if (bodyEl) bodyEl.classList.set.forEach((c) => doc.body.classList.add(c));
  doc.createElement = (t) => makeEl(String(t).toLowerCase());

  const outbox = [];
  let inbound = null;
  const winL = {};
  const webview = { postMessage: (msg) => outbox.push(JSON.parse(JSON.stringify(msg))),   // what crosses: JSON
                    addEventListener: (t, fn) => { if (t === "message") inbound = fn; } };
  const win = {
    addEventListener(t, fn) { (winL[t] ||= []).push(fn); },
    removeEventListener(t, fn) { if (winL[t]) winL[t] = winL[t].filter((f) => f !== fn); },
    dispatch(t, ev = {}) {
      const e = { type: t, defaultPrevented: false, target: null, ...ev };
      e.preventDefault = () => { e.defaultPrevented = true; };
      e.stopPropagation = () => {};
      (winL[t] || []).slice().forEach((fn) => fn(e));
      return e;
    },
    chrome: { webview },
    location: { hash: opts.hash || "" },
  };
  if (opts.noWebView) win.chrome = undefined;
  const lsStore = Object.assign({}, opts.ls || {});
  const localStorage = {
    getItem: (k) => (k in lsStore ? lsStore[k] : null),
    setItem: (k, v) => { lsStore[k] = String(v); },
    removeItem: (k) => { delete lsStore[k]; },
  };
  win.localStorage = localStorage;
  const clock = makeClock();
  const media = makeMedia();
  win.matchMedia = media.matchMedia;
  const performance = opts.noClock ? undefined : { now: () => clock.now() };
  win.performance = performance;
  win.innerWidth = opts.width || 900;
  win.innerHeight = opts.height || 640;
  const sandbox = {
    window: win, document: doc, console, localStorage, performance,
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    Math, JSON, Date, Object, Array, Number, String, Boolean, parseInt, parseFloat, isNaN,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, { filename: "live.html" });

  // App -> page, as WebView2 hands it over (a JSON string or an object: the page takes both)
  const send = (msg) => { if (!inbound) throw new Error("the page registered no message listener"); inbound({ data: msg }); };
  const sent = (type) => outbox.filter((x) => x && x.type === type);
  const last = (type) => { const o = sent(type); return o[o.length - 1]; };
  return { doc, ids, outbox, send, win, lsStore, sent, last, count: (type) => sent(type).length,
           pumpTimers: (ms) => clock.pump(ms), now: () => clock.now(),
           setReduceMotion: (on) => { media.box.reduceMotion = !!on; },
           pendingTimers: () => clock.pending(), script, html };
}

module.exports = { runLive, makeClock, makeMedia, LIVE_PAGE, readLivePage };

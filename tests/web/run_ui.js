// run_ui.js -- the headless runner of SuVtitle's page (app/web/index.html). PORTED from AudioPB's
// scratchpad/ui_test/run_ui.js (the frozen copy of 641f1aa's era, SuVtitle/scratchpad/from_audiopb_641f1aa/ui_test/),
// 2026-10-23: its runTranslatorWindow() + the virtual clock and media queries it uses. Gone with the desk: run() and
// buildDom() (app.js, the patchbay -- AudioPB's main window, which SuVtitle does not have).
//
// The page is a standalone file with LOGIC in it; a twin that is only ever regex-matched is a twin nobody has run. So
// its single inline <script> is lifted out, the DOM is built FROM ITS OWN MARKUP (domstub.js buildFromMarkup: every id
// the script looks up exists exactly as written), and the script runs in a vm in WebView2 mode -- the REAL bridge
// protocol path is exercised (window.chrome.webview present), the design mock stays asleep.
"use strict";
const fs = require("fs");
const vm = require("vm");
const { El, makeDocument, buildFromMarkup, makeEl, VTTCueStub } = require("./domstub.js");
const { PAGE, lf } = require("./paths.js");

// A virtual clock for a sandbox: setTimeout/clearTimeout that queue rather than fire, plus pump(ms) to advance time.
// Callbacks scheduled BY a callback are picked up by the same pump, so a two-beat sequence runs in one call.
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
  // The clock's own reading, for the page's performance.now: the page never sees the wall, so "the hold expires after
  // 10 s" is a fact of the test, not a race.
  return { setTimeout: setT, clearTimeout: clearT, pump, pending: () => q.size, now: () => now };
}

// window.matchMedia, in the smallest form: "(prefers-reduced-motion: reduce)" answers a live box (a test may turn the
// setting on mid-run), anything else false -- a machine with no such preference.
function makeMedia() {
  const box = { reduceMotion: false };
  const matchMedia = (q) => ({
    media: String(q),
    matches: /prefers-reduced-motion:\s*reduce/.test(String(q)) && box.reduceMotion,
    addEventListener() {}, removeEventListener() {},
    addListener() {}, removeListener() {},
  });
  return { box, matchMedia };
}

// opts.html: ANOTHER version of the page (a mutant built by hand, the 641f1aa original...).
// opts.mutate(script) -> script: run a MUTANT of the page (the mutation sentinels); it must actually change the script,
//   or the sentinel would pass by testing the original.
// opts.noAdditionalObjects: a runtime without postMessageWithAdditionalObjects.
// opts.noAudioTracks (WH): a runtime without HTMLMediaElement.audioTracks.
// opts.audioNonExclusive (WH): a runtime where several sound tracks may be enabled at once (the spec's letter; Chromium
//   enables one and silently disables the rest -- measured, domstub.js).
// opts.ls: what this profile's localStorage already holds when the page boots.
// opts.width / opts.height: the window's size (layoutClasses reads it).
// opts.blockPlay: play() is REFUSED (no autoplay switch). opts.noWebView: the page in a plain BROWSER (its design mock).
function runTranslatorWindow(opts = {}) {
  // (LF whatever the checkout -- RV4-9: the sentinels' mutations are written against LF)
  const html = lf(typeof opts.html === "string" ? opts.html : fs.readFileSync(PAGE, "utf8"));
  const m = html.match(/<script>([\s\S]*?)<\/script>/);
  if (!m) throw new Error("index.html: no inline <script> to run");
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
  // The stub's document.body is its root element (makeDocument); the page's <body class="in-file"> is an element of
  // the built tree -- its classes are carried over, so `document.body.classList` reads what the markup says.
  const walk = (n) => (n.tagName === "BODY" ? n : n.children.map(walk).find(Boolean) || null);
  const bodyEl = walk(root);
  if (bodyEl) bodyEl.classList.set.forEach((c) => doc.body.classList.add(c));
  doc.createElement = (t) => makeEl(String(t).toLowerCase());
  // document-level listeners are REAL here (drag & drop, visibility): recorded and dispatchable.
  const docL = {};
  doc.addEventListener = (t, fn) => { (docL[t] ||= []).push(fn); };
  doc.removeEventListener = (t, fn) => { if (docL[t]) docL[t] = docL[t].filter((f) => f !== fn); };
  const dispatchDoc = (t, ev = {}) => {
    const e = { type: t, defaultPrevented: false, target: null, ...ev };
    e.preventDefault = () => { e.defaultPrevented = true; };
    e.stopPropagation = () => {};
    (docL[t] || []).slice().forEach((fn) => fn(e));
    return e;
  };
  doc.visibilityState = "visible";
  doc.hidden = false;
  doc.fullscreenElement = null;
  doc.exitFullscreen = () => { doc._exitedFullscreen = (doc._exitedFullscreen || 0) + 1; doc.fullscreenElement = null; return Promise.resolve(); };

  const outbox = [];
  const objs = [];           // the additional objects of every postMessageWithAdditionalObjects
  let inbound = null;
  const winL = {};
  const webview = { postMessage: (msg) => outbox.push(msg),
                    addEventListener: (t, fn) => { if (t === "message") inbound = fn; } };
  if (!opts.noAdditionalObjects)
    webview.postMessageWithAdditionalObjects = (msg, list) => { outbox.push(msg); objs.push(list); };
  const win = {
    addEventListener(t, fn) { (winL[t] ||= []).push(fn); },
    removeEventListener(t, fn) { if (winL[t]) winL[t] = winL[t].filter((f) => f !== fn); },
    dispatch(t, ev = {}) {
      const e = { type: t, preventDefault() {}, stopPropagation() {}, target: null, ...ev };
      (winL[t] || []).slice().forEach((fn) => fn(e));
    },
    // The page decides "am I inside WebView2?" from this object. Present = the REAL bridge path is exercised and the
    // design mock stays asleep, which is what a test wants.
    chrome: { webview },
  };
  const lsStore = Object.assign({}, opts.ls || {});
  const localStorage = {
    getItem: (k) => (k in lsStore ? lsStore[k] : null),
    setItem: (k, v) => { lsStore[k] = String(v); },
    removeItem: (k) => { delete lsStore[k]; },
  };
  win.localStorage = localStorage;

  const clock = makeClock();
  // WH: each media stub's audioTracks queues its `change` on THIS clock (Blink fires it from a task, never inside the
  // `enabled` setter): pumpTimers(0) delivers it.
  [ids.fileVideo, ids.fileAudio].forEach((me) => {
    if (!me) return;
    if (opts.noAudioTracks) { delete me.audioTracks; return; }
    if (me.audioTracks) {
      me.audioTracks.schedule = (fn) => clock.setTimeout(fn, 0);
      if (opts.audioNonExclusive) me.audioTracks.exclusive = false;
    }
  });
  const media = makeMedia();
  win.matchMedia = media.matchMedia;
  const performance = { now: () => clock.now() };
  win.performance = performance;
  win.innerWidth = opts.width || 760;
  win.innerHeight = opts.height || 560;
  // the animation-frame queue of the player's overlay: frames run only when a test PUMPS them (pumpRaf), like a
  // browser that paints when it paints -- the timeupdate path must hold alone.
  const rafQ = [];
  let rafSeq = 0;
  const requestAnimationFrame = (fn) => { rafQ.push(fn); return ++rafSeq; };
  const cancelAnimationFrame = () => {};
  // a play() the browser REFUSES (no autoplay switch): a synchronous thenable, so the refusal lands inside the call.
  if (opts.blockPlay) {
    [ids.fileVideo, ids.fileAudio].forEach((me) => {
      if (!me) return;
      me.play = () => {
        me.playCalls++;
        const err = { name: "NotAllowedError", message: "play() failed because the user didn't interact" };
        return { then(ok, bad) { if (bad) bad(err); return this; }, catch(fn) { fn(err); return this; } };
      };
    });
  }
  if (opts.noWebView) win.chrome = undefined;
  const URLstub = { createObjectURL: (f) => "blob:mock/" + encodeURIComponent(String((f && f.name) || "")), revokeObjectURL() {} };
  const sandbox = {
    window: win, document: doc, console, localStorage, performance, URL: URLstub,
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, setInterval, clearInterval,
    requestAnimationFrame, cancelAnimationFrame,
    Math, JSON, Date, Object, Array, Number, String, Boolean, parseInt, parseFloat, isNaN,
    // WC built VTTCues; WE draws its own overlay -- the stub stays so a page that went back to a TextTrack would be
    // SEEN doing so (the checks assert there is none).
    VTTCue: VTTCueStub,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, { filename: "index.html" });

  const send = (msg) => inbound({ data: msg });
  const pumpRaf = (n = 1) => { for (let i = 0; i < n; i++) rafQ.splice(0).forEach((f) => f(clock.now())); };
  return { doc, ids, outbox, objs, send, win, lsStore, dispatchDoc, pumpTimers: (ms) => clock.pump(ms),
           now: () => clock.now(), pumpRaf, rafPending: () => rafQ.length,
           setReduceMotion: (on) => { media.box.reduceMotion = !!on; },
           pendingTimers: () => clock.pending(), script };
}

module.exports = { runTranslatorWindow, makeClock, makeMedia };

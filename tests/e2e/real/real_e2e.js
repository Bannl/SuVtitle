// real_e2e.js -- the REAL product: build\x64-Debug\SuVtitle.exe run as a TEST INSTANCE, driven like the user drives it.
// PORTED 2026-10-23 (S-HOST) from AudioPB's scratchpad/filemode_2026-10-10/wg_real/wg_real.js (641f1aa; frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\). What changed: SuVtitle is ONE window (no desk, no tray) -- AudioPB's R12/R13
// (the desk's footer button and drops on the desk) went with the desk, and three SuVtitle checks came instead: R12 the
// FIRST-RUN IMPORT of AudioPB's player data (seeded in the test profile: the imported record IS the R1 cache hit), R13
// the window as the app (+ the ENGINE spawned, its pair echoed to the page), R16 the REAL ENGINE analysing a clip on the
// CPU (FILE -> cues -> one .srt per language). Names: SuVtitle's (class SuVtitle.MainWindow.<tag>, test registry root
// HKCU\Software\SuVtitle-Test, ProgIDs NEXTALE.SuVtitle.*), the origin https://suvtitle.local/.
// FIX ROUND 2026-10-23 (S-HOST): R17's pin = the window's REAL always-on-top state told to the page (RV4-7); R19 = what
// SuVtitle serves (RV2-4: a miss is its own 404, never the network; every answer under the page's CSP; the real page
// runs under it without one violation); R20 = the viewer's subtitle file beats the page's AUTOMATIC default (RV4-1,
// SPLIT_PLAN ADDENDUM 2: the page asks it after the file, the app ignores it).
//
// --test-instance suvr1: its own single-instance names, a window that never takes the foreground, an engine on the CPU,
// on an ISOLATED profile (APPDATA / LOCALAPPDATA -> work\profile: the product refuses to start as a test instance
// otherwise), its WebView2 reachable through a debugging port (CDP) -- WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS overrides the
// product's own switches, so it repeats ALL THREE of them. The window on the SECOND screen (app.txt: -1880,40).
// Nothing of the user's: his real %APPDATA% / %LOCALAPPDATA% AudioPB and SuVtitle folders and his registry are only READ
// (before and after, to prove nothing was touched). His films are not used at all.
// Run: run_real.cmd (builds real_tools.exe first). Results: runs\R.jsonl, runs\R*.png, exit 0 = all ok.
"use strict";
const fs = require("fs"), path = require("path"), cp = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..", "..");
const EXE = path.join(REPO, "build", "x64-Debug", "SuVtitle.exe");
const TOOLS = path.join(HERE, "work", "real_tools.exe");
const TAG = "suvr1";
const PORT = 9343;
const WORK = path.join(HERE, "work", "run"), PROF = path.join(HERE, "work", "profile"), RUNS = path.join(HERE, "runs");
const ROAM = path.join(PROF, "Roaming"), LOCAL = path.join(PROF, "Local");
const SUV_ROAM = path.join(ROAM, "SuVtitle"), SUV_FILES = path.join(LOCAL, "SuVtitle", "Files");
const APB_ROAM = path.join(ROAM, "AudioPB"), APB_FILES = path.join(LOCAL, "AudioPB", "Translator files");
// the clips: NOT in the repository -- SUVTITLE_E2E_MEDIA names a folder with this layout, copied into work\ (never written there):
//   A\E01 Pilot.mp4 (10 s, h264 + aac, WITH SPEECH: R16 has the real engine analyse it) | A\E01 Pilot.fr.srt | A\Interview.mp3
//   B\E02 The Bridge.mp4 | Season 1\E03.mp4 | Season 1\E04.mp3
// (tests\e2e\make_media.js generates the same layout without speech: enough for every check but R16; the maintainer's default is a
// frozen copy of AudioPB's we_e2e media kept in the git-ignored scratchpad\)
const SRC_MEDIA = process.env.SUVTITLE_E2E_MEDIA
  || path.join(REPO, "scratchpad", "from_audiopb_641f1aa", "filemode_2026-10-10", "we_e2e", "media");
const MEDIA = path.join(WORK, "media"), SEASON = path.join(WORK, "Season 1"), SUBS = path.join(WORK, "subs");
const E01 = path.join(MEDIA, "E01 Pilot.mp4");
const EP2 = path.join(MEDIA, "Épisode 2 (test) été.mp4");
const INTERVIEW = path.join(MEDIA, "Interview.mp3");
const E03 = path.join(SEASON, "E03.mp4");
const EXT_SRT = path.join(SUBS, "E01 Pilot.fr.srt");
const SRT_EP2 = path.join(SUBS, "Ep2.fr.srt"), SRT_LOSSY = path.join(SUBS, "Lossy.fr.srt"), SRT_PLAIN = path.join(SUBS, "Plain.fr.srt");
const CANARY = path.join(WORK, "canary.exe");
const TEST_ROOT = "HKCU\\Software\\SuVtitle-Test";
const MY_ROOT = TEST_ROOT + "\\" + TAG;
const REAL_APPDATA = process.env.APPDATA, REAL_LOCAL = process.env.LOCALAPPDATA;
const MAIN_CLASS = "SuVtitle.MainWindow." + TAG;
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays"
               + " --enable-blink-features=AudioVideoTracks";   // = main.cpp createWebViewEnv, all three
const ANALYSIS_MS = +(process.env.SUVTITLE_E2E_ANALYSIS_MS || 420000);   // R16: the CPU analysis of a 10 s clip

const results = [];
const logPath = path.join(RUNS, "R.jsonl");
function log(step, ok, what, detail) {
  const r = { step, ok: !!ok, what, detail: detail === undefined ? null : detail };
  results.push(r);
  fs.appendFileSync(logPath, JSON.stringify(r) + "\n");
  console.log((ok ? "  ok    " : "  FAIL  ") + step + "  " + what + (ok || detail === undefined ? "" : "  -> " + JSON.stringify(detail).slice(0, 400)));
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const under = (p, root) => path.resolve(p).toLowerCase().startsWith(path.resolve(root).toLowerCase() + path.sep);

// ---- the isolated environment ----------------------------------------------------------------------------------------
function testEnv() {
  const e = {};
  for (const k of Object.keys(process.env)) {
    const u = k.toUpperCase();
    if (u === "APPDATA" || u === "LOCALAPPDATA" || u === "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") continue;
    e[k] = process.env[k];
  }
  e.APPDATA = ROAM;
  e.LOCALAPPDATA = LOCAL;
  // the variable OVERRIDES the product's own options: the debugging port + ALL the product's switches
  e.WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=" + PORT + " " + SWITCHES;
  return e;
}
const ENV = testEnv();
function tool(args, env) {
  const r = cp.spawnSync(TOOLS, args, { env: env || process.env, encoding: "utf8", windowsHide: true, timeout: 60000 });
  const line = String(r.stdout || "").trim().split(/\r?\n/).pop() || "{}";
  try { return Object.assign(JSON.parse(line), { status: r.status }); } catch { return { ok: false, raw: line, status: r.status }; }
}
function reg(args) {
  const r = cp.spawnSync("reg", args, { encoding: "latin1", windowsHide: true, timeout: 30000 });
  return { status: r.status, out: String(r.stdout || "") + String(r.stderr || "") };
}
const regExists = (key, value) => reg(["query", key].concat(value === undefined ? [] : value === "" ? ["/ve"] : ["/v", value])).status === 0;
function regSz(key, value) {
  const r = reg(["query", key].concat(value === "" ? ["/ve"] : ["/v", value]));
  if (r.status !== 0) return null;
  const m = /REG_SZ\s+(.*)$/m.exec(r.out);
  return m ? m[1].replace(/\r$/, "") : null;
}
const esc = (s) => s.replace(/\\/g, "\\\\").replace(/\t/g, "\\t").replace(/\n/g, "\\n").replace(/\r/g, "\\r");   // TrPipe.h esc

// ---- the user's REAL side: read only ---------------------------------------------------------------------------------
function realKeysAbsent() {
  return {
    progVideo: !regExists("HKCU\\Software\\Classes\\NEXTALE.SuVtitle.Video"),
    progAudio: !regExists("HKCU\\Software\\Classes\\NEXTALE.SuVtitle.Audio"),
    vendor: !regExists("HKCU\\Software\\NEXTALE\\SuVtitle"),
    registeredApp: !regExists("HKCU\\Software\\RegisteredApplications", "SuVtitle"),
    openWithMp4: !regExists("HKCU\\Software\\Classes\\.mp4\\OpenWithProgids", "NEXTALE.SuVtitle.Video"),
    openWithMp3: !regExists("HKCU\\Software\\Classes\\.mp3\\OpenWithProgids", "NEXTALE.SuVtitle.Audio"),
  };
}
const realSuvAbsent = () => !fs.existsSync(path.join(REAL_APPDATA, "SuVtitle")) && !fs.existsSync(path.join(REAL_LOCAL, "SuVtitle"));
function snapshotTree(roots, skipDirs) {
  const out = {};
  const walk = (dir, depth) => {
    let names = [];
    try { names = fs.readdirSync(dir); } catch { return; }
    for (const n of names) {
      const p = path.join(dir, n);
      let st;
      try { st = fs.statSync(p); } catch { continue; }
      if (st.isDirectory()) { if (depth > 0 && !(skipDirs || []).includes(n)) walk(p, depth - 1); }
      else out[p] = st.size + "@" + st.mtimeMs;
    }
  };
  for (const r of roots) walk(r, 3);
  return out;
}
const snapshotReal = () => snapshotTree([path.join(REAL_APPDATA, "AudioPB"), path.join(REAL_LOCAL, "AudioPB")], ["WebView2", "EBWebView"]);
// Any of the user's real files that mentions this test = isolation broken.
function realMentionsTest() {
  const hits = [];
  const needles = ["e2e\\real", "SuVtitle-Test", "suvr1"].flatMap((s) => [Buffer.from(s, "utf8"), Buffer.from(s, "utf16le")]);
  const look = (p) => {
    let b;
    try { b = fs.readFileSync(p); } catch { return; }
    if (needles.some((n) => b.includes(n))) hits.push(p);
  };
  for (const n of ["player.txt", "player-resume.txt", "player-audio.txt", "translator.txt", "settings.txt"]) look(path.join(REAL_APPDATA, "AudioPB", n));
  const tf = path.join(REAL_LOCAL, "AudioPB", "Translator files");
  let names = [];
  try { names = fs.readdirSync(tf); } catch { /* none */ }
  for (const n of names) if (/\.(cues|txt)$/i.test(n)) look(path.join(tf, n));
  return hits;
}

// ---- the subtitle files: their texts, cue by cue ---------------------------------------------------------------------
function srtTexts(p) {
  let t;
  try { t = fs.readFileSync(p, "utf8"); } catch { return null; }
  t = t.replace(/^\uFEFF/, "").replace(/\r\n/g, "\n");
  return t.split(/\n\n+/).map((b) => b.trim()).filter(Boolean).map((b) => {
    const ls = b.split("\n");
    const at = ls.findIndex((l) => /-->/.test(l));
    return { time: at >= 0 ? ls[at] : "", text: ls.slice(at + 1).join(" ").trim() };
  });
}

// ---- CDP -------------------------------------------------------------------------------------------------------------
async function targets() {
  try { const r = await fetch("http://127.0.0.1:" + PORT + "/json/list"); return await r.json(); } catch { return null; }
}
class Cdp {
  constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); this.errors = []; this.net = []; }
  open() {
    return new Promise((res, rej) => {
      this.ws = new WebSocket(this.url);
      this.ws.onopen = () => res();
      this.ws.onerror = (e) => rej(new Error("ws error " + (e && e.message)));
      this.ws.onmessage = (ev) => {
        const m = JSON.parse(typeof ev.data === "string" ? ev.data : Buffer.from(ev.data).toString("utf8"));
        if (m.id && this.pending.has(m.id)) {
          const p = this.pending.get(m.id);
          this.pending.delete(m.id);
          if (m.error) p.rej(new Error(m.error.message || JSON.stringify(m.error))); else p.res(m.result);
        } else if (m.method === "Runtime.exceptionThrown") {
          this.errors.push({ kind: "exception", text: JSON.stringify(m.params.exceptionDetails).slice(0, 500) });
        } else if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "warning")) {
          this.errors.push({ kind: "console." + m.params.type, text: (m.params.args || []).map((a) => a.value || a.description || "").join(" ").slice(0, 500) });
        } else if (m.method && m.method.indexOf("Network.") === 0) {
          this.net.push(m);                                        // (R19: only while the Network domain is on)
          if (this.net.length > 4000) this.net.shift();
        } else if (m.method === "Log.entryAdded" && m.params.entry.level === "error"
                   && !/favicon\.ico|r19-missing-asset/.test((m.params.entry.url || "") + " " + (m.params.entry.text || ""))) {
          this.errors.push({ kind: "log", text: (m.params.entry.text + " " + (m.params.entry.url || "")).slice(0, 500) });
        }
      };
    });
  }
  send(method, params) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params: params || {} }));
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej });
      setTimeout(() => { if (this.pending.has(id)) { this.pending.delete(id); rej(new Error("timeout " + method)); } }, 20000);
    });
  }
  async eval(expr) {
    const r = await this.send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true, userGesture: true });
    if (r.exceptionDetails) throw new Error("eval: " + JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result.value;
  }
  close() { try { this.ws.close(); } catch { /* gone */ } }
}
const STATE = "(() => { const $ = (id) => document.getElementById(id); const v = $('fileVideo');"
  + " const rows = Array.from($('fileList').children);"
  + " return { mode: $('filePane').hidden ? 'live' : 'file', status: $('fileStatus').textContent, statusTitle: $('fileStatus').title,"
  + "  vsrc: v ? decodeURIComponent(v.currentSrc || v.src || '') : '', vt: v ? +v.currentTime.toFixed(2) : -1, vPaused: v ? v.paused : null,"
  + "  vErr: v && v.error ? v.error.code : 0, rows: rows.map((r) => r.children[1] ? r.children[1].textContent : ''),"
  + "  selRow: rows.findIndex((r) => r.classList.contains('is-sel')), lines: Array.from($('subOverlay').children).map((l) => l.textContent),"
  + "  toast: $('fileToast').hidden ? '' : $('fileToast').textContent, edShown: !$('edPane').hidden, edState: $('edState').textContent,"
  + "  edMsg: $('edMsg').textContent, edSrc: $('edSrc').value, edDst: $('edDst').value,"
  + "  engine: { shown: !$('fileEngine').hidden, text: $('fileEngineText').textContent },"
  + "  queue: Array.from(document.querySelectorAll('#queueList .q-name')).map((e) => e.textContent), body: document.body.className }; })()";
let cdp = null;
async function state() { return cdp.eval(STATE); }
async function waitFor(pred, ms, label) {
  const t0 = Date.now();
  let last = null, err = null;
  while (Date.now() - t0 < ms) {
    try { last = await state(); if (pred(last)) return last; } catch (e) { err = e; }
    await sleep(150);
  }
  throw Object.assign(new Error("timeout: " + label), { last, err: err && err.message });
}
async function waitToast(re, ms, label) {
  const s = await waitFor((x) => re.test(x.toast), ms, label);
  return s.toast;
}
async function shot(name) {
  try {
    const r = await cdp.send("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync(path.join(RUNS, name + ".png"), Buffer.from(r.data, "base64"));
    return true;
  } catch (e) { return false; }
}
async function key(k, code, vk) {
  const p = { key: k, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: 0 };
  await cdp.send("Input.dispatchKeyEvent", Object.assign({}, p, { type: "keyDown", text: k, unmodifiedText: k }));
  await cdp.send("Input.dispatchKeyEvent", Object.assign({}, p, { type: "keyUp" }));
}
const typeEnter = (id, text) => cdp.eval("(() => { const t = document.getElementById(" + JSON.stringify(id) + "); t.focus(); t.value = "
  + JSON.stringify(text) + "; t.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return 'typed'; })()");
// What the bridge says to the page (a listener of our own on the page's channel; a reloaded page needs it again).
const HOOK = "(() => { if (!window.__suvMsgs) { window.__suvMsgs = []; chrome.webview.addEventListener('message', (e) => {"
  + " const d = e.data; window.__suvMsgs.push(typeof d === 'string' ? d : JSON.stringify(d)); if (window.__suvMsgs.length > 4000) window.__suvMsgs.shift(); }); }"
  + " return window.__suvMsgs.length; })()";
// What the PAGE says to the app (R20: its automatic requests), recorded on top of the page's own postMessage.
const OUTHOOK = "(() => { if (!window.__suvOut) { window.__suvOut = []; const wv = window.chrome.webview; const o = wv.postMessage.bind(wv);"
  + " try { wv.postMessage = (m) => { window.__suvOut.push(typeof m === 'string' ? m : JSON.stringify(m)); return o(m); }; }"
  + " catch (e) { window.__suvOut.push('HOOK FAILED ' + e); } } return window.__suvOut.length; })()";
async function pageSays(type) {
  const all = await cdp.eval("(window.__suvOut || []).slice()");
  return all.map((s) => { try { return JSON.parse(s); } catch { return null; } }).filter((m) => m && m.type === type);
}
// R19: every CSP violation of a document, from its first byte (a DevTools script: not itself under the page's policy)
const CSPV = "window.__cspv = []; document.addEventListener('securitypolicyviolation', (e) => window.__cspv.push(e.violatedDirective + ' ' + e.blockedURI));";
// The policy main.cpp sends (kWebCsp), word for word -- tests\cpp\fixround_pins.py holds the same string.
const CSP = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; font-src 'self'; img-src data:; "
  + "media-src https://*.suvtitle-media.example https://suvtitle-proxy.example; base-uri 'none'; form-action 'none'; "
  + "frame-ancestors 'none'; object-src 'none'";
async function bridgeSays(type) {
  const all = await cdp.eval("(window.__suvMsgs || []).slice()");
  return all.map((s) => { try { return JSON.parse(s); } catch { return null; } }).filter((m) => m && m.type === type);
}

let testPid = 0;
function focusCheck(step) {
  const f = tool(["fg"]);
  log(step, f.ok && f.pid !== testPid, "the foreground is not the test instance's (" + f.cls + ")", f);
}
function windowsCheck(step) {
  const w = tool(["wins", String(testPid)]);
  const vis = (w.wins || []).filter((x) => x.visible);
  const me = vis.find((x) => x.cls === MAIN_CLASS);
  log(step, me && vis.length === 1 && me.title === "SuVtitle" && me.noactivate && me.x < -1000,
      "ONE window, " + MAIN_CLASS + " \"SuVtitle\", on the SECOND screen (x " + (me ? me.x : "?") + "), WS_EX_NOACTIVATE (test), no tray", w.wins);
}

// ---- setup -----------------------------------------------------------------------------------------------------------
let seeded = {};
function setup() {
  if (!fs.existsSync(path.join(SRC_MEDIA, "A", "E01 Pilot.mp4"))) {
    console.log("real_e2e: the clips are not there (" + SRC_MEDIA + "): set SUVTITLE_E2E_MEDIA to a folder holding A\\E01 Pilot.mp4 (10 s WITH SPEECH), "
      + "A\\E01 Pilot.fr.srt, A\\Interview.mp3, B\\E02 The Bridge.mp4, Season 1\\E03.mp4, Season 1\\E04.mp3 (see the top of real_e2e.js). Nothing was run.");
    process.exit(2);
  }
  for (const d of [WORK, PROF]) {
    if (!under(d, HERE)) throw new Error("refusing to clear " + d);
    fs.rmSync(d, { recursive: true, force: true });
  }
  fs.mkdirSync(RUNS, { recursive: true });
  for (const f of fs.readdirSync(RUNS)) if (/^R.*\.(png|jsonl)$/.test(f)) fs.unlinkSync(path.join(RUNS, f));
  for (const d of [MEDIA, SEASON, SUBS, SUV_ROAM, APB_ROAM, APB_FILES, LOCAL]) fs.mkdirSync(d, { recursive: true });
  fs.copyFileSync(path.join(SRC_MEDIA, "A", "E01 Pilot.mp4"), E01);
  fs.copyFileSync(path.join(SRC_MEDIA, "B", "E02 The Bridge.mp4"), EP2);
  fs.copyFileSync(path.join(SRC_MEDIA, "A", "Interview.mp3"), INTERVIEW);
  fs.copyFileSync(path.join(SRC_MEDIA, "Season 1", "E03.mp4"), E03);
  fs.copyFileSync(path.join(SRC_MEDIA, "Season 1", "E04.mp3"), path.join(SEASON, "E04.mp3"));
  fs.copyFileSync(path.join(SRC_MEDIA, "A", "E01 Pilot.fr.srt"), EXT_SRT);
  fs.writeFileSync(path.join(WORK, "notes.txt"), "not a video\n");
  const twoCues = (t1, t2) => "1\r\n00:00:00,500 --> 00:00:02,800\r\n" + t1 + "\r\n\r\n2\r\n00:00:03,000 --> 00:00:05,500\r\n" + t2 + "\r\n";
  fs.writeFileSync(SRT_EP2, twoCues("Le deuxième épisode.", "Il commence ici."));
  fs.writeFileSync(SRT_LOSSY, twoCues("<font color=\"#ffff00\">Jaune</font> et normal.", "Deuxième ligne."));
  fs.writeFileSync(SRT_PLAIN, twoCues("Une ligne simple.", "Une autre."));
  fs.copyFileSync(TOOLS, CANARY);
  // SuVtitle's own config, as the user's would be: the window on the SECOND screen (app.txt, the app's), the pair
  // French <-> English (engine.txt: the ENGINE's file -- S-ENG's header; the app only reads its pair)
  fs.writeFileSync(path.join(SUV_ROAM, "app.txt"), "# SuVtitle app v1\nWINPOS\t-1880\t40\t1000\t720\t0\t0\n");
  fs.writeFileSync(path.join(SUV_ROAM, "engine.txt"), "# SuVtitle Engine config v1\nLANGA\tfr\nLANGB\ten\nMODE\tauto\n");
  // AudioPB 1.0.6's player data, IN THE TEST PROFILE (the first-run import brings it over): a queue with one item, a
  // resume entry, a sound-track choice, the .srt registry; the records are written by mkrecord ... audiopb in main()
  const iv = tool(["ident", INTERVIEW]);
  seeded.queue = "# AudioPB player v1\nOPT\ttranslate\t1\nOPT\trepeat\toff\nOPT\tshuffle\t0\nCUR\t0\n"
    + "ITEM\t" + esc(INTERVIEW) + "\t" + iv.size + "\t" + iv.mtime + "\t10000\nEND\t1\n";
  seeded.resume = "# AudioPB player resume v1\nPOS\t" + esc("d:\\films\\the long one.mkv|123456789|132000000000000000") + "\t1800000\t6000000\t1760000000\n";
  seeded.audio = "# AudioPB player audio v1\nAUD\t" + "0123456789abcdef" + "\t1\t3\t1760000000\n";
  seeded.reg = "# AudioPB Translator: subtitle files written by AudioPB (path, size, write time, media)\n"
    + esc("d:\\films\\the long one.fr.srt") + "\t10\t20\t" + esc("d:\\films\\the long one.mkv") + "\n";
  fs.writeFileSync(path.join(APB_ROAM, "player.txt"), seeded.queue);
  fs.writeFileSync(path.join(APB_ROAM, "player-resume.txt"), seeded.resume);
  fs.writeFileSync(path.join(APB_ROAM, "player-audio.txt"), seeded.audio);
  fs.writeFileSync(path.join(APB_ROAM, "translator.txt"), "# AudioPB Translator config v2\nLANGA\tde\nLANGB\tja\n");   // NOT imported
  fs.writeFileSync(path.join(APB_FILES, "srt-written.txt"), seeded.reg);
}

async function main() {
  if (!fs.existsSync(EXE)) throw new Error("no Debug build: " + EXE);
  if (!fs.existsSync(path.join(path.dirname(EXE), "SuVtitleEngine.exe"))) throw new Error("no engine next to the app");
  if (under(REAL_APPDATA, HERE) || under(REAL_LOCAL, HERE)) throw new Error("the real profile is inside the kit?");
  fs.mkdirSync(RUNS, { recursive: true });
  const preTargets = await targets();
  if (preTargets) throw new Error("port " + PORT + " already answers: another debug browser is running");
  const realBefore = snapshotReal();
  const keysBefore = realKeysAbsent();
  const suvAbsentBefore = realSuvAbsent();
  setup();
  fs.writeFileSync(logPath, "");
  log("R0", Object.values(keysBefore).every(Boolean), "before: none of SuVtitle's player keys in the user's registry", keysBefore);
  log("R0", !regExists(TEST_ROOT), "before: no test root " + TEST_ROOT);
  log("R0", suvAbsentBefore, "before: the user has no %APPDATA%\\SuVtitle nor %LOCALAPPDATA%\\SuVtitle (never created by a test)");
  // AudioPB's records, in AudioPB's folder of the TEST profile: E01 complete (this cue engine), Episode 2 EDITED (the
  // engine before) -- both reach SuVtitle by the IMPORT; E03 a 1.0.6 record (no ENGINE line) straight in SuVtitle's cache
  const rec = tool(["mkrecord", E01, "fr", "en", "cur", "0", "audiopb"], ENV);
  const recOld = tool(["mkrecord", EP2, "fr", "en", "old", "1", "audiopb"], ENV);
  const recNone = tool(["mkrecord", E03, "fr", "en", "none"], ENV);
  log("R0", rec.ok && under(rec.record, APB_FILES) && rec.engine === rec.current && rec.engineLine,
      "a COMPLETE record for E01, of THIS cue engine (ENGINE " + rec.current + "), in AudioPB's folder of the TEST profile", rec);
  log("R0", recOld.ok && recOld.engine === recOld.current - 1 && recOld.edited && under(recOld.record, APB_FILES)
      && recNone.ok && !recNone.engineLine && under(recNone.record, SUV_FILES),
      "ENGINE: an EDITED record of the engine before (Épisode 2, AudioPB's folder) and a 1.0.6 record without the line (E03, SuVtitle's)", { recOld, recNone });
  const suvRec = (r) => path.join(SUV_FILES, path.basename(r.record));   // where the import puts AudioPB's record
  const apbBefore = snapshotTree([APB_ROAM, path.join(LOCAL, "AudioPB")]);
  const apbRecText = fs.readFileSync(rec.record, "utf8");
  const fg0 = tool(["fg"]);

  let proc = null, exitCode = null, enginePid = 0, inflight = [];
  try {
    // ---- R1: Explorer's --play, no SuVtitle running: the window opens on the file ---------------------------------
    proc = cp.spawn(EXE, ["--test-instance", TAG, "--play", E01], { env: ENV, stdio: "ignore", windowsHide: false });
    testPid = proc.pid;
    proc.on("exit", (c) => { exitCode = c; });
    let target = null;
    for (let t0 = Date.now(); Date.now() - t0 < 45000 && !target; await sleep(300)) {
      const l = await targets();
      if (exitCode !== null) throw new Error("the test instance exited at once: " + exitCode);
      target = (l || []).find((x) => x.type === "page" && /^https:\/\/suvtitle\.local\/index\.html/.test(x.url));
    }
    if (!target) throw new Error("no SuVtitle page");
    cdp = new Cdp(target.webSocketDebuggerUrl);
    await cdp.open();
    await cdp.send("Runtime.enable");
    await cdp.send("Page.enable");
    await cdp.send("Log.enable");
    let s = await waitFor((x) => x.mode === "file" && x.rows.length === 4 && /E01 Pilot\.mp4/.test(x.vsrc), 30000, "R1 E01 + its 4 cached lines");
    log("R1", true, "cold --play: the window opened with the clip and its 4 cached lines -- AudioPB's record, IMPORTED (a hit, nothing analysed)", { rows: s.rows, vsrc: s.vsrc });
    s = await waitFor((x) => x.vPaused === false && x.vt > 0.3, 15000, "R1 playing");
    log("R1", true, "...and it PLAYS by itself (no click)", { vt: s.vt });
    windowsCheck("R1");
    focusCheck("R1");
    const fr = path.join(MEDIA, "E01 Pilot.fr.srt"), en = path.join(MEDIA, "E01 Pilot.en.srt");
    for (let t0 = Date.now(); Date.now() - t0 < 10000 && !(fs.existsSync(fr) && fs.existsSync(en)); ) await sleep(200);
    const frT = srtTexts(fr), enT = srtTexts(en);
    log("R1", frT && enT && frT.map((c) => c.text).join("|") === "Le courant finira par revenir avant la nuit.|Soit tu restes, soit on te laisse.|C'est quoi, cette ombre dans le noir ?|On sait tous les deux que ce serait très compliqué."
        && enT.map((c) => c.text).join("|") === "The power will eventually come back.|Either you stay, or we move you.|What's that shadow in the dark?|We both know it would be very complicated.",
        "ONE FILE PER LANGUAGE next to the clip, written by the bridge: E01 Pilot.fr.srt = the French, E01 Pilot.en.srt = the English", { fr: frT, en: enT });
    const recText = fs.existsSync(suvRec(rec)) ? fs.readFileSync(suvRec(rec), "utf8") : "";
    log("R1", (recText.match(/^SRTL\t/gm) || []).length === 2 && !/^EDIT\t1/m.test(recText),
        "SuVtitle's copy of the record names both files (2 SRTL lines), not edited yet");
    const regPath = path.join(SUV_FILES, "srt-written.txt");
    const regText = fs.existsSync(regPath) ? fs.readFileSync(regPath, "utf8").toLowerCase() : "";
    log("R1", regText.includes("\\\\media\\\\e01 pilot.fr.srt\t") && regText.includes("\\\\media\\\\e01 pilot.en.srt\t")
        && regText.includes("\\\\media\\\\e01 pilot.mp4") && regText.includes("the long one.fr.srt"),
        "both files in the TEST profile's ownership registry (SuVtitle\\Files\\srt-written.txt: AudioPB's line imported, ours added)");
    s = await state();
    log("R1", /saved \.fr\.srt \+ \.en\.srt/.test(s.status), "the strip: \"… saved .fr.srt + .en.srt\"", s.status);
    log("R1", !s.engine.shown && !/older engine/.test(s.status), "ENGINE: this engine's record -- nothing asked, nothing said", s.status);
    log("R1", s.queue.includes("Interview.mp3") && s.queue.includes("E01 Pilot.mp4"),
        "the queue: AudioPB's (Interview.mp3, IMPORTED) + the file Explorer handed over", s.queue);
    await shot("R1_cold_play");

    // ---- R12: THE FIRST-RUN IMPORT, as it happened (the marker = its log) -------------------------------------------
    const marker = path.join(SUV_ROAM, "audiopb-import.txt");
    const mk = fs.existsSync(marker) ? fs.readFileSync(marker, "utf8") : "";
    log("R12", /^# SuVtitle: AudioPB's player data imported/.test(mk) && /^COPIED\tplayer\.txt$/m.test(mk)
        && /^COPIED\tplayer-resume\.txt$/m.test(mk) && /^COPIED\tplayer-audio\.txt$/m.test(mk) && /^COPIED\tsrt-written\.txt$/m.test(mk)
        && mk.includes("COPIED\t" + path.basename(rec.record)) && mk.includes("COPIED\t" + path.basename(recOld.record))
        && /^DONE\tcopied 6\tkept 0\tfailed 0$/m.test(mk), "the import ran once, its marker lists the 6 files COPIED (3 player files, the registry, 2 records)", mk);
    log("R12", fs.existsSync(suvRec(recOld)) && fs.readFileSync(path.join(SUV_ROAM, "player-audio.txt"), "utf8") === seeded.audio
        && !fs.existsSync(path.join(SUV_ROAM, "translator.txt")),
        "...byte for byte (player-audio.txt), the edited record too; AudioPB's translator.txt NOT imported (the pair is the engine's)");
    log("R12", fs.readFileSync(rec.record, "utf8") === apbRecText, "AudioPB's own record unchanged while SuVtitle wrote SRTL lines in its COPY");
    focusCheck("R12");

    // ---- R13: the window IS the app; the ENGINE is its child, and speaks ----------------------------------------------
    {
      const w = (tool(["wins", String(testPid)]).wins || []).find((x) => x.cls === MAIN_CLASS) || {};
      log("R13", w.caption && w.sysmenu && w.minbox && w.maxbox && !w.owned,
          "a NORMAL window: caption, system menu, minimise + maximise boxes, unowned (a taskbar button for the product)", w);
      const kids = tool(["kids", String(testPid)]).kids || [];
      const eng = kids.find((k) => /^SuVtitleEngine\.exe$/i.test(k.exe));
      enginePid = eng ? eng.pid : 0;
      log("R13", !!eng, "the engine SuVtitleEngine.exe runs as the app's CHILD (spawned by sibling path)", kids);
      await cdp.eval(HOOK);
      await cdp.eval("(() => { chrome.webview.postMessage({ type: 'translatorReplay' }); return 1; })()");
      await sleep(1200);
      const st = (await bridgeSays("translatorState")).pop() || {};
      log("R13", st.active === true && st.running === true && st.langA === "fr" && st.langB === "en" && /^(listening|loading|analyzing)$/.test(st.status),
          "the page hears the engine: running, status " + st.status + ", the pair fr <-> en (engine.txt, echoed by the engine)", st);
      const elog = path.join(LOCAL, "SuVtitle", "engine.log");
      const el = fs.existsSync(elog) ? fs.readFileSync(elog, "utf8") : "";
      log("R13", el.includes("parent " + testPid) && /every child on the CPU/.test(el),
          "the engine's own log (in the TEST profile): its parent is this app, every child on the CPU (a test never takes the GPU)", el.slice(0, 400));
    }

    // ---- R2: an edit of SuVtitle's subtitles -> the core -> 2 s later the record (EDIT 1) and the files ---------------
    await cdp.eval("(() => { const v = document.getElementById('fileVideo'); v.pause(); v.currentTime = 3.0; return 1; })()");
    await waitFor((x) => x.vPaused === true && x.vt >= 2.9 && x.vt <= 3.1, 8000, "R2 paused at 0:03");
    await sleep(300);
    await key("e", "KeyE", 69);
    s = await waitFor((x) => x.edShown && x.selRow === 1, 5000, "R2 the editor open on line 2");
    log("R2", true, "E (a real key event): the editor, on the line under the playhead", { edDst: s.edDst });
    await typeEnter("edDst", "Stay, or be moved.");
    s = await waitFor((x) => x.rows[1] === "Stay, or be moved." && /edited/.test(x.edState), 8000, "R2 the core's answer");
    log("R2", / · edited/.test(s.status) || /edited/.test(s.status), "the REAL core answered: the line changed, 'edited' in the pane", { edState: s.edState, status: s.status });
    await sleep(3200);
    const enT2 = srtTexts(en), frT2 = srtTexts(fr);
    const recText2 = fs.readFileSync(suvRec(rec), "utf8");
    log("R2", enT2 && enT2[1] && enT2[1].text === "Stay, or be moved." && enT2.length === 4 && frT2 && frT2[1].text === "Soit tu restes, soit on te laisse.",
        "2 s later: E01 Pilot.en.srt holds the edit, E01 Pilot.fr.srt unchanged", { en: enT2 && enT2.map((c) => c.text) });
    log("R2", /^EDIT\t1/m.test(recText2) && recText2.includes("Stay, or be moved."), "...and SuVtitle's record is EDITED (EDIT 1) and holds it");
    log("R2", fs.readFileSync(rec.record, "utf8") === apbRecText, "...AudioPB's original record still untouched (copied, never shared)");
    await shot("R2_edit_persisted");
    focusCheck("R2");

    // ---- R3: a .srt DROPPED on the PLAYING video (Chromium's real drag path -> the bridge) --------------------------
    await cdp.eval("(() => { const v = document.getElementById('fileVideo'); v.currentTime = 0.2; v.play(); return 1; })()");
    await sleep(300);
    const c = await cdp.eval("(() => { const r = document.getElementById('fileVideo').getBoundingClientRect(); return [Math.round(r.left + r.width / 2), Math.round(r.top + r.height / 2)]; })()");
    const data = { items: [], files: [EXT_SRT], dragOperationsMask: 1 };
    for (const type of ["dragEnter", "dragOver", "drop"]) await cdp.send("Input.dispatchDragEvent", { type, x: c[0], y: c[1], data });
    const t3 = await waitToast(/^Subtitles loaded: E01 Pilot\.fr\.srt/, 8000, "R3 the toast");
    s = await waitFor((x) => x.rows.length === 2 && /niveau des dialogues/.test(x.rows[0]), 5000, "R3 the file's lines");
    log("R3", true, "the bridge read the DROPPED file: \"" + t3 + "\", its 2 lines are the list", s.rows);
    s = await waitFor((x) => x.vt >= 0.6 && x.vt <= 2.7 && x.lines.join(" ").includes("Le niveau des dialogues est bon."), 6000, "R3 on the picture");
    log("R3", s.vPaused === false, "...on the picture while it plays (the dropped file is the active source)", { lines: s.lines, vt: s.vt });
    await shot("R3_srt_dropped_real_bridge");
    focusCheck("R3");

    // ---- R4: an edit of the DROPPED file -> saved back to it, the original kept ONCE as .bak --------------------------
    const original = fs.readFileSync(EXT_SRT);
    await cdp.eval("(() => { document.getElementById('fileVideo').pause(); document.getElementById('fileList').children[0].click(); return 1; })()");
    s = await waitFor((x) => x.edShown && x.selRow === 0, 5000, "R4 line 1 selected");
    await typeEnter("edSrc", "Le niveau des <i>voix</i> est bon.");
    const t4 = await waitToast(/^Saved E01 Pilot\.fr\.srt · the original is kept as E01 Pilot\.fr\.srt\.bak/, 9000, "R4 saved");
    const bak = path.join(SUBS, "E01 Pilot.fr.srt.bak");
    const after1 = fs.readFileSync(EXT_SRT, "utf8");
    log("R4", fs.existsSync(bak) && fs.readFileSync(bak).equals(original) && after1.includes("Le niveau des <i>voix</i> est bon.") && after1.includes("On baisse la musique"),
        "the file SAVED BACK with the edit (its markup), the original kept byte for byte as E01 Pilot.fr.srt.bak: \"" + t4 + "\"");
    await cdp.eval("(() => { document.getElementById('fileList').children[1].click(); return 1; })()");
    await waitFor((x) => x.selRow === 1, 5000, "R4 line 2 selected");
    await typeEnter("edSrc", "On baisse la musique de trois décibels ?");
    await sleep(3500);
    const after2 = fs.readFileSync(EXT_SRT, "utf8");
    log("R4", fs.readFileSync(bak).equals(original) && after2.includes("trois décibels") && after2.includes("<i>voix</i>"),
        "a second edit: saved too, the .bak still the ORIGINAL (one backup, the first time only)");
    await shot("R4_dropped_file_edited_saved_bak");
    focusCheck("R4");

    // ---- R5: a SECOND --play (a folder) while the instance runs: forwarded through WM_COPYDATA, then it quits ----------
    const r5 = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", SEASON], { env: ENV, timeout: 30000 });
    log("R5", r5.status === 0, "the second process handed the folder over and quit (exit 0 = sent)", r5.status);
    s = await waitFor((x) => /E03\.mp4/.test(x.vsrc), 20000, "R5 E03 plays");
    log("R5", true, "the instance scanned the folder and plays its first file (E03.mp4)", { vsrc: s.vsrc });
    s = await waitFor((x) => x.queue.includes("E03.mp4") && x.queue.includes("E04.mp3"), 8000, "R5 the queue");
    log("R5", true, "the folder's two files are in the queue", s.queue);
    await sleep(800);
    s = await state();
    const recNoneText = fs.readFileSync(recNone.record, "utf8");
    log("R5", s.rows.length === 0 && !/analysed earlier/.test(s.status) && !s.engine.shown && !/ENGINE/.test(recNoneText),
        "ENGINE: E03's 1.0.6 record (no ENGINE line) is a MISS in the real product -- none of its lines shown, nothing asked "
        + "(SuVtitle's engine is there: it is analysed again, R16 lets it finish)", { rows: s.rows.length, status: s.status });
    await shot("R5_forwarded_folder");
    focusCheck("R5");

    // ---- R6: "Make SuVtitle the default player…" -> the TEST root, nothing of the user's ------------------------------
    await cdp.eval("(() => { document.getElementById('tMore').click(); return 1; })()");
    await sleep(300);
    const clicked = await cdp.eval("(() => { const b = Array.from(document.getElementById('morePanel').children).find((x) => /default player/.test(x.textContent)); if (!b) return false; b.click(); return true; })()");
    const t6 = await waitToast(/^Registered under HKEY_CURRENT_USER\\Software\\SuVtitle-Test\\suvr1 \(a test instance: Settings not opened/, 6000, "R6 registered");
    log("R6", clicked, "the ⋯ item clicked; the bridge says: \"" + t6 + "\"");
    const cmd = regSz(MY_ROOT + "\\Classes\\NEXTALE.SuVtitle.Video\\shell\\open\\command", "");
    log("R6", cmd === "\"" + EXE + "\" --play \"%1\"", "the TEST ProgID's command is exactly \"<exe>\" --play \"%1\"", cmd);
    const all = reg(["query", MY_ROOT, "/s"]);
    const nvals = (all.out.match(/\bREG_(SZ|NONE)\b/g) || []).length;
    log("R6", nvals === 78, "78 values under the test root (2 ProgIDs x 5, 32 OpenWithProgids, 3 + 32 Capabilities, RegisteredApplications)", nvals);
    log("R6", regSz(MY_ROOT + "\\RegisteredApplications", "SuVtitle") === "Software\\SuVtitle-Test\\" + TAG + "\\NEXTALE\\SuVtitle\\Capabilities"
        && regSz(MY_ROOT + "\\NEXTALE\\SuVtitle\\Capabilities\\FileAssociations", ".mkv") === "NEXTALE.SuVtitle.Video"
        && regSz(MY_ROOT + "\\NEXTALE\\SuVtitle\\Capabilities\\FileAssociations", ".flac") === "NEXTALE.SuVtitle.Audio"
        && regSz(MY_ROOT + "\\NEXTALE\\SuVtitle\\Capabilities", "ApplicationName") === "SuVtitle"
        && regExists(MY_ROOT + "\\Classes\\.mp4\\OpenWithProgids", "NEXTALE.SuVtitle.Video"),
        "RegisteredApplications \"SuVtitle\" -> its Capabilities; .mkv video, .flac audio; .mp4 \"Open with\"");
    const keysMid = realKeysAbsent();
    log("R6", Object.values(keysMid).every(Boolean), "the user's own registry: still none of it", keysMid);
    await shot("R6_registered_on_the_test_key");
    focusCheck("R6");

    // ---- R7: the REGISTERED command line, executed as Explorer executes it (CreateProcess, %1 = the file) -----------
    const line = cmd.replace("%1", EP2) + " --test-instance " + TAG;   // (the test switch: never the user's own SuVtitle)
    const r7 = tool(["run", line], ENV);
    log("R7", r7.ok && r7.exited && r7.exit === 0, "Explorer's command line (a name with spaces, accents, parentheses) -> forwarded, exit 0", r7);
    s = await waitFor((x) => x.vsrc.includes("Épisode 2 (test) été.mp4"), 15000, "R7 Episode 2 plays");
    log("R7", true, "the instance plays Épisode 2 (test) été.mp4", { vsrc: s.vsrc });
    s = await waitFor((x) => x.rows.length === 4 && x.engine.shown, 8000, "R7 the older engine's edited result + the question");
    log("R7", s.engine.text === "Subtitles from an older engine — re-analyse? (your edits would be lost)" && /edited · older engine/.test(s.status),
        "ENGINE: its EDITED result of the engine before (imported from AudioPB) is KEPT (4 lines) and the strip ASKS: \"" + s.engine.text + "\"", s.status);
    await shot("R7_explorer_command_line_older_engine_asked");
    await cdp.eval("(() => { document.getElementById('fileEngineNo').click(); return 1; })()");
    s = await waitFor((x) => !x.engine.shown, 3000, "R7 kept");
    log("R7", s.rows.length === 4 && fs.readFileSync(suvRec(recOld), "utf8").includes("\nEDIT\t1"),
        "ENGINE: \"Keep my edits\": the question goes, the lines and the record stay as they were");
    focusCheck("R7");

    // ---- R8: what --play refuses: an .exe, a missing file, a relative path, a wildcard, a text file ------------------
    const queueBefore = (await state()).queue;
    const r8 = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", CANARY, path.join(MEDIA, "missing.mp4"), "media\\E01 Pilot.mp4",
                                  path.join(MEDIA, "*.mp4"), path.join(WORK, "notes.txt")], { env: ENV, timeout: 30000 });
    log("R8", r8.status === 0, "forwarded (exit 0)", r8.status);
    const t8 = await waitToast(/^5 items were not a video, audio or subtitle file \(or not found\) and were left out\./, 8000, "R8 refused");
    await sleep(800);
    s = await state();
    log("R8", true, "all five refused, said: \"" + t8 + "\"");
    log("R8", s.vsrc.includes("Épisode 2 (test) été.mp4") && JSON.stringify(s.queue) === JSON.stringify(queueBefore),
        "nothing changed: the same file plays, the same queue", { vsrc: s.vsrc, queue: s.queue });
    log("R8", !fs.existsSync(path.join(WORK, "canary_ran.txt")), "the .exe handed to --play was NEVER run (its canary file is absent)");
    await shot("R8_refused_paths");
    focusCheck("R8");

    // ---- R14: files DROPPED ON THE PLAYER (the bridge path executed) --------------------------------------------------
    const center = () => cdp.eval("(() => { const r = document.getElementById('fileVideo').getBoundingClientRect(); return [Math.round(r.left + r.width / 2), Math.round(r.top + r.height / 2)]; })()");
    const dropOn = async (files, x, y) => {
      const d = { items: [], files, dragOperationsMask: 1 };
      for (const type of ["dragEnter", "dragOver", "drop"]) await cdp.send("Input.dispatchDragEvent", { type, x, y, data: d });
    };
    let vc = await center();
    await dropOn([SEASON], vc[0], vc[1]);
    s = await waitFor((x) => /E03\.mp4/.test(x.vsrc), 15000, "R14 a folder");
    log("R14", true, "a FOLDER dropped on the player: scanned, its first file plays (E03.mp4)", { vsrc: s.vsrc });
    vc = await center();
    await dropOn([EP2, SRT_EP2], vc[0], vc[1]);
    s = await waitFor((x) => x.vsrc.includes("Épisode 2 (test) été.mp4") && /^Subtitles loaded: Ep2\.fr\.srt/.test(x.toast), 15000, "R14 media + srt");
    log("R14", true, "a VIDEO + a SUBTITLE FILE dropped together: the video plays, the subtitles go with it (\"" + s.toast + "\")");
    vc = await center();
    const vNow = s.vsrc;
    await dropOn([path.join(WORK, "notes.txt")], vc[0], vc[1]);
    s = await waitFor((x) => x.toast === "That is not a video, audio or subtitle file.", 8000, "R14 a .txt");
    log("R14", s.vsrc === vNow, "a .txt dropped on the player: refused, said, nothing changed");
    await shot("R14_player_drops");
    focusCheck("R14");

    // ---- R15: the review's #3 -- a file SuVtitle cannot write back whole is never rewritten ---------------------------
    const lossyBytes = fs.readFileSync(SRT_LOSSY);
    await dropOn([SRT_LOSSY], vc[0], vc[1]);
    await waitFor((x) => /^Subtitles loaded: Lossy\.fr\.srt/.test(x.toast) && x.rows.length === 2, 8000, "R15 lossy loaded");
    if (!(await state()).edShown) { await key("e", "KeyE", 69); await waitFor((x) => x.edShown, 4000, "R15 editor"); }
    await cdp.eval("(() => { document.getElementById('fileVideo').pause(); document.getElementById('fileList').children[0].click(); return 1; })()");
    await waitFor((x) => x.selRow === 0, 4000, "R15 line 1");
    await typeEnter("edSrc", "Jaune corrigé et normal.");
    const t15 = await waitToast(/^Your edits are saved as Lossy\.fr\.edited\.srt\. The file holds what SuVtitle does not keep/, 9000, "R15 copy");
    const copy15 = path.join(SUBS, "Lossy.fr.edited.srt");
    log("R15", fs.readFileSync(SRT_LOSSY).equals(lossyBytes) && fs.existsSync(copy15) && fs.readFileSync(copy15, "utf8").includes("Jaune corrigé et normal.")
        && !fs.existsSync(SRT_LOSSY + ".bak"),
        "a LOSSY file (a <font> tag) edited: the original left BYTE FOR BYTE, the edits in Lossy.fr.edited.srt, said: \"" + t15 + "\"");
    await dropOn([SRT_PLAIN], vc[0], vc[1]);
    await waitFor((x) => /^Subtitles loaded: Plain\.fr\.srt/.test(x.toast) && x.rows.length === 2, 8000, "R15 plain loaded");
    await cdp.eval("(() => { document.getElementById('fileList').children[0].click(); return 1; })()");
    await waitFor((x) => x.selRow === 0, 4000, "R15 plain line 1");
    await typeEnter("edSrc", "Une ligne simple, corrigée.");
    await waitToast(/^Saved Plain\.fr\.srt · the original is kept as Plain\.fr\.srt\.bak/, 9000, "R15 in place");
    const inPlace = fs.readFileSync(SRT_PLAIN, "utf8");
    fs.appendFileSync(SRT_PLAIN, "\r\n3\r\n00:00:06,000 --> 00:00:07,000\r\nAjouté par un autre éditeur.\r\n");
    const external = fs.readFileSync(SRT_PLAIN);
    await cdp.eval("(() => { document.getElementById('fileList').children[1].click(); return 1; })()");
    await waitFor((x) => x.selRow === 1, 4000, "R15 plain line 2");
    await typeEnter("edSrc", "Une autre, corrigée.");
    const t15b = await waitToast(/^Your edits are saved as Plain\.fr\.edited\.srt\. The file changed on disk since it was opened/, 9000, "R15 changed on disk");
    log("R15", inPlace.includes("Une ligne simple, corrigée.") && fs.readFileSync(SRT_PLAIN).equals(external)
        && fs.readFileSync(path.join(SUBS, "Plain.fr.edited.srt"), "utf8").includes("Une autre, corrigée."),
        "a plain file: saved IN PLACE (+ .bak); changed on disk by another editor -> the next save goes to Plain.fr.edited.srt, "
        + "the other editor's change kept: \"" + t15b + "\"");
    await shot("R15_lossy_and_changed_saved_to_copies");
    focusCheck("R15");

    // ---- R16: THE REAL ENGINE, end to end: E03 (its 1.0.6 record a miss) analysed on the CPU -------------------------
    // FILE -> FPROG / FCUE / FDONE through the pipe -> the core: the cues on the page, one .srt per language next to the
    // clip, the record rewritten under THIS cue engine, complete. A 10 s clip of English speech; pair fr <-> en.
    {
      const r16 = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", E03], { env: ENV, timeout: 30000 });
      s = await waitFor((x) => /E03\.mp4/.test(x.vsrc), 20000, "R16 E03 plays");
      await cdp.eval(HOOK);
      await cdp.eval("(() => { const v = document.getElementById('fileVideo'); v.pause(); return 1; })()");
      // U1 (02/11/2026): a file opened is played, not analysed -- the viewer's Translate starts it (this file only)
      await cdp.eval("(() => { document.getElementById('fileTranslate').click(); return 1; })()");
      const t0 = Date.now();
      let done = null, sawAnalyzing = false, kidsSeen = [];
      while (Date.now() - t0 < ANALYSIS_MS) {
        await sleep(2000);
        try { await cdp.eval(HOOK); } catch { /* reloading */ }
        const ek = enginePid ? (tool(["kids", String(enginePid)]).kids || []) : [];
        for (const k of ek) if (!kidsSeen.includes(k.exe)) kidsSeen.push(k.exe);
        const st = (await bridgeSays("translatorState")).pop();
        if (st && st.status === "analyzing") sawAnalyzing = true;
        const files = (await bridgeSays("translatorFile")).filter((f) => /E03\.mp4$/.test(f.name || ""));
        const last = files.pop();
        if (last && (last.complete || /^(error|cancelled)$/.test(last.phase))) { done = last; break; }
      }
      const elapsed = Math.round((Date.now() - t0) / 1000);
      s = await state();
      const recNew = fs.existsSync(recNone.record) ? fs.readFileSync(recNone.record, "utf8") : "";
      log("R16", r16.status === 0 && done && done.complete && !done.err && s.rows.length > 0,
          "the REAL engine analysed E03 on the CPU in " + elapsed + " s: FILE -> cues -> complete, " + s.rows.length + " line(s) on the page",
          { done: done && { phase: done.phase, complete: done.complete, err: done.err, count: done.count, asr: done.asrModel, mt: done.mtModel }, rows: s.rows.slice(0, 4) });
      log("R16", sawAnalyzing && kidsSeen.length > 0, "...the engine said \"analyzing\" and ran its model servers as ITS children: " + kidsSeen.join(", "), kidsSeen);
      log("R16", /\nENGINE\t3\n/.test(recNew) && /\nDONE\t1\n/.test(recNew) && /\nFCUE\t/.test(recNew),
          "the record was rewritten under THIS cue engine (ENGINE 3), complete (DONE 1), with its cues", recNew.slice(0, 300));
      const e03fr = srtTexts(path.join(SEASON, "E03.fr.srt")), e03en = srtTexts(path.join(SEASON, "E03.en.srt"));
      log("R16", (e03fr && e03fr.length > 0) || (e03en && e03en.length > 0),
          "one .srt per language next to the clip: E03.en.srt " + (e03en ? e03en.length : 0) + " cue(s), E03.fr.srt " + (e03fr ? e03fr.length : 0),
          { en: e03en && e03en.slice(0, 2), fr: e03fr && e03fr.slice(0, 2) });
      await shot("R16_engine_analysed_E03");
      focusCheck("R16");
    }

    // ---- R17: FULL SCREEN IS THE WINDOW (WE D3) and always-on-top (the pin) -- main.cpp's own paths ---------------------
    {
      await cdp.eval(HOOK);
      const win = () => (tool(["wins", String(testPid)]).wins || []).find((x) => x.cls === MAIN_CLASS) || {};
      const appTxt = () => { try { return fs.readFileSync(path.join(SUV_ROAM, "app.txt"), "utf8"); } catch { return ""; } };
      const before = win();
      await cdp.eval("(() => { chrome.webview.postMessage({ type: 'translatorFullscreen', on: true }); return 1; })()");
      let fsW = null;
      for (let t0 = Date.now(); Date.now() - t0 < 6000 && !fsW; await sleep(150)) { const w = win(); if (w.w >= 1900 && !w.caption) fsW = w; }
      await sleep(400);
      const told = (await bridgeSays("translatorWindow")).pop() || {};
      log("R17", fsW && fsW.x <= -1900 && fsW.h >= 1000 && fsW.noactivate && told.fullscreen === true,
          "translatorFullscreen{on}: the WINDOW covers ITS monitor (borderless " + (fsW ? fsW.w + " x " + fsW.h + " at " + fsW.x : "?")
          + ", the second screen), never activated; the page is told fullscreen:true", { fsW, told });
      await shot("R17_full_screen");
      await cdp.eval("(() => { chrome.webview.postMessage({ type: 'translatorFullscreen', on: false }); return 1; })()");
      let back = null;
      for (let t0 = Date.now(); Date.now() - t0 < 6000 && !back; await sleep(150)) { const w = win(); if (w.caption && w.x === before.x && w.w === before.w && w.h === before.h) back = w; }
      await sleep(400);
      const told2 = (await bridgeSays("translatorWindow")).pop() || {};
      log("R17", !!back && told2.fullscreen === false && /^WINPOS\t-1880\t40\t1000\t720\t0\t0$/m.test(appTxt()),
          "...and back EXACTLY where it was (framed, " + before.x + "," + before.y + " " + before.w + " x " + before.h + "), the page told fullscreen:false, app.txt unchanged", { back, told2 });
      const nWin = (await bridgeSays("translatorWindow")).length;
      await cdp.eval("(() => { chrome.webview.postMessage({ type: 'translatorTopmost', value: true }); return 1; })()");
      let top = null;
      for (let t0 = Date.now(); Date.now() - t0 < 5000 && !top; await sleep(150)) { const w = win(); if (w.topmost) top = w; }
      await sleep(300);
      const wTop = win(), toldTop = (await bridgeSays("translatorWindow")).slice(nWin).pop() || {};
      const appTop = (appTxt().match(/^WINPOS\t-1880\t40\t1000\t720\t([01])\t0$/m) || [])[1];
      await cdp.eval("(() => { chrome.webview.postMessage({ type: 'translatorCtl', alwaysOnTop: false }); return 1; })()");
      let untop = null;
      for (let t0 = Date.now(); Date.now() - t0 < 5000 && !untop; await sleep(150)) { const w = win(); if (w.cls && !w.topmost) untop = w; }
      await sleep(300);
      const toldUntop = (await bridgeSays("translatorWindow")).pop() || {};
      // (!) Whether Windows GRANTS the topmost is not ours to check: it may refuse it to a window that was never
      // activated while another app holds the foreground (measured 23/10: SetWindowPos(HWND_TOPMOST) from another
      // process ignored every time; the app's own pin granted in 3 probes, refused in one E2E run while Explorer was in
      // front). What the app now does with the answer IS checked (RV4-7): the REAL state -- granted or refused -- told
      // to the page (translatorWindow.topmost: its pin follows it) and kept in app.txt; then off, everywhere.
      log("R17", toldTop.type === "translatorWindow" && toldTop.topmost === !!wTop.topmost && appTop === (wTop.topmost ? "1" : "0")
          && !!untop && toldUntop.topmost === false && /^WINPOS\t-1880\t40\t1000\t720\t0\t0$/m.test(appTxt()),
          "the pin (RV4-7): translatorTopmost{value:true} -> the window's REAL state (WS_EX_TOPMOST " + !!wTop.topmost
          + ") told to the page (topmost:" + toldTop.topmost + ") and kept in app.txt (" + appTop + "); translatorCtl{alwaysOnTop:false} -> off: told false, app.txt 0",
          { granted: !!wTop.topmost, toldTop, appTop, untop: !!untop, toldUntop });
      focusCheck("R17");
    }

    // ---- R19: WHAT SUVTITLE SERVES (RV2-4) -- a miss on its origin is ITS 404, never the network; every answer under
    //      the page's Content-Security-Policy; the real page runs under it (its film, its font) without one violation --
    {
      const PROBE = "https://suvtitle.local/r19-missing-asset.css", INDEX = "https://suvtitle.local/index.html";
      const hdr = (r, k) => { for (const [n, v] of Object.entries((r && r.headers) || {})) if (n.toLowerCase() === k) return v; return ""; };
      const answer = async (url, from, ms) => {           // the response to `url` (or its failure), from the Network events
        for (let t0 = Date.now(); Date.now() - t0 < ms; await sleep(100)) {
          const ev = cdp.net.slice(from), ids = new Set(ev.filter((m) => m.method === "Network.requestWillBeSent"
                                                                   && m.params.request.url === url).map((m) => m.params.requestId));
          const resp = ev.find((m) => m.method === "Network.responseReceived" && m.params.response.url === url);
          const fail = ev.find((m) => m.method === "Network.loadingFailed" && ids.has(m.params.requestId));
          if (resp || fail) return { resp: resp && resp.params.response, fail: fail && fail.params.errorText };
        }
        return {};
      };
      const before19 = await state();
      await cdp.send("Network.enable");
      await cdp.send("Page.addScriptToEvaluateOnNewDocument", { source: CSPV });
      let from = cdp.net.length;
      await cdp.send("Page.navigate", { url: PROBE });
      const miss = await answer(PROBE, from, 8000);
      log("R19", miss.resp && miss.resp.status === 404 && !miss.fail && hdr(miss.resp, "content-security-policy") === CSP
          && /no-store/.test(hdr(miss.resp, "cache-control")),
          "a MISS on https://suvtitle.local/ is SuVtitle's OWN 404 (no-store, under the CSP) -- never the network (before: a DNS lookup of suvtitle.local, ERR_NAME_NOT_RESOLVED)",
          { status: miss.resp && miss.resp.status, fail: miss.fail, headers: miss.resp && miss.resp.headers });
      from = cdp.net.length;
      await cdp.send("Page.navigate", { url: INDEX });
      const idx = await answer(INDEX, from, 8000);
      log("R19", idx.resp && idx.resp.status === 200 && hdr(idx.resp, "content-security-policy") === CSP,
          "index.html is served under EXACTLY the page's policy: " + CSP, { status: idx.resp && idx.resp.status, csp: hdr(idx.resp, "content-security-policy") });
      s = await waitFor((x) => x.mode === "file" && x.vsrc === before19.vsrc && x.rows.length === before19.rows.length, 20000, "R19 the page back");
      await cdp.eval(HOOK);
      let media = null;
      for (let t0 = Date.now(); Date.now() - t0 < 10000; await sleep(200)) {
        media = await cdp.eval("(() => { const v = document.getElementById('fileVideo'); return { rs: v.readyState, err: v.error ? v.error.code : 0,"
          + " host: (v.currentSrc.match(/^https:\\/\\/[^/]+/) || [''])[0] }; })()");
        if (media.rs >= 1 || media.err) break;
      }
      // the title's face: Bahnschrift, a Windows system font (no font file ships; document.fonts.check() would say true for ANY
      // family name, so it is measured): the computed family names it first AND a canvas sets the same string differently
      // in it than in the generic fallback -- i.e. it is installed and is what the title uses
      const font = await cdp.eval("(() => { const n = document.querySelector('.brand-name'); if (!n) return false;"
        + " const ff = getComputedStyle(n).fontFamily; const c = document.createElement('canvas').getContext('2d'); const t = 'SuVtitle WQ1ilm';"
        + " c.font = '600 40px monospace'; const generic = c.measureText(t).width; c.font = '600 40px Bahnschrift, monospace';"
        + " return /^\"?Bahnschrift/.test(ff) && c.measureText(t).width !== generic; })()");
      const v19 = await cdp.eval("(window.__cspv || []).slice()");
      log("R19", media && media.rs >= 1 && !media.err && /^https:\/\/f[^.]+\.suvtitle-media\.example$/.test(media.host) && font && v19.length === 0,
          "...and the real page runs under it: back with its film (" + before19.rows.length + " lines), the film's media from its "
          + (media && media.host) + " host (readyState " + (media && media.rs) + "), the title's system face (Bahnschrift), the CSS's data: chevrons -- NO violation",
          { media, font, violations: v19 });
      await cdp.send("Network.disable");
      focusCheck("R19");
    }

    // ---- R20: the VIEWER's subtitle file beats the page's AUTOMATIC default (RV4-1, SPLIT_PLAN ADDENDUM 2). The app posts
    //      the opening's state, then the file the user opened with the film, in ONE task: the page's default (a sidecar,
    //      translation off) is asked when the state arrives -- i.e. AFTER the file. The app IGNORES it now; before, it
    //      answered, and the sidecar replaced the user's own file.
    {
      // a sidecar of the USER's next to E01 (the .fr/.en.srt SuVtitle wrote there in R1 are its own: never offered)
      const SIDECAR = path.join(MEDIA, "E01 Pilot.de.srt");
      fs.writeFileSync(SIDECAR, "1\r\n00:00:00,500 --> 00:00:02,800\r\nEin Untertitel neben dem Film.\r\n\r\n"
                              + "2\r\n00:00:03,000 --> 00:00:05,500\r\nZweite Zeile.\r\n");
      await cdp.eval(HOOK);
      await cdp.eval(OUTHOOK);
      // (U1, 02/11/2026: no switch to turn off any more -- a file opened is not analysed unless asked, its default stands)
      await sleep(1000);
      const nSubs = (await bridgeSays("playerSubs")).length;
      const r20 = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", E01, SRT_PLAIN], { env: ENV, timeout: 30000 });
      s = await waitFor((x) => /E01 Pilot\.mp4/.test(x.vsrc) && /^Subtitles loaded: Plain\.fr\.srt/.test(x.toast), 15000, "R20 E01 + Plain.fr.srt");
      await sleep(2500);                                  // the page's default, asked at once, has long reached the app
      s = await state();
      const asked = await pageSays("playerSubGet");
      const sent = (await bridgeSays("playerSubs")).slice(nSubs);
      const files = new Set(sent.map((m) => m.id));
      const st20 = (await bridgeSays("translatorFile")).pop() || {};
      const side = (st20.sidecars || []).find((x) => x.name === "E01 Pilot.de.srt") || {};
      const plain = (srtTexts(SRT_PLAIN) || []).map((c) => c.text);       // (as R15 left it: edited, then a 3rd line)
      log("R20", r20.status === 0 && side.id > 0 && asked.some((m) => m.auto === true && m.id === side.id) && files.size === 1
          && !files.has(side.id) && st20.extSubId === [...files][0] && plain.length === 3 && JSON.stringify(s.rows) === JSON.stringify(plain),
          "--play E01 + Plain.fr.srt, translation off, E01 Pilot.de.srt beside it: the page asked that sidecar as its DEFAULT "
          + "(playerSubGet auto:true, after the file) -- IGNORED: one subtitle file sent (Plain.fr.srt, the one named), its "
          + plain.length + " lines are the list",
          { exit: r20.status, asked, files: [...files], extSubId: st20.extSubId, sidecars: st20.sidecars, rows: s.rows, plain, toast: s.toast });
      await shot("R20_viewer_file_wins");
      await sleep(1000);
      try { fs.unlinkSync(SIDECAR); } catch { /* (the test's own file) */ }
      focusCheck("R20");
    }

    log("R*", cdp.errors.length === 0, "no JS exception / console error in the page", cdp.errors);

    // ---- R18: the window will be closed (R9) WHILE AN ANALYSIS RUNS: E04.mp3 has no record -> a FILE job -------------
    {
      const r18 = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", path.join(SEASON, "E04.mp3")], { env: ENV, timeout: 30000 });
      // U1 (02/11/2026): opened = played; Translate (the press, this file) is what starts its analysis
      await waitFor((x) => /E04\.mp3/.test(x.statusTitle || ""), 20000, "R18 E04 opens").catch(() => null);   // (the strip's tooltip names it)
      await cdp.eval("(() => { document.getElementById('fileTranslate').click(); return 1; })()");
      let servers = [];
      for (let t0 = Date.now(); Date.now() - t0 < 90000 && !servers.length; await sleep(500)) {
        servers = enginePid ? (tool(["kids", String(enginePid)]).kids || []).filter((k) => /server/i.test(k.exe)) : [];
      }
      inflight = servers;
      log("R18", r18.status === 0 && servers.length > 0,
          "E04.mp3 (no record) is being analysed -- the engine's model servers are up (" + servers.map((k) => k.exe).join(", ")
          + "): the window is closed NOW, mid-analysis (R9)", servers);
    }
  } catch (e) {
    log("R!", false, "the run stopped: " + e.message, { last: e.last, err: e.err });
  } finally {
    // ---- R9: closing the window QUITS the app (state saved, the engine stopped) ---------------------------------------
    if (cdp) cdp.close();
    if (testPid) {
      const cl = tool(["close", MAIN_CLASS, String(testPid)]);
      log("R9", cl.ok && cl.exited && cl.exit === 0, "WM_CLOSE: the test instance quit by itself (exit 0) -- a normal app: closing its window quits it", cl);
      if (!(cl.ok && cl.exited)) cp.spawnSync("taskkill", ["/PID", String(testPid), "/T", "/F"], { windowsHide: true });
      await sleep(1500);
      log("R9", enginePid && !tool(["alive", String(enginePid)]).alive, "...and its engine is gone with it (no orphan)", enginePid);
      if (inflight.length) {
        let left = inflight;
        for (let t0 = Date.now(); Date.now() - t0 < 10000 && left.length; await sleep(500)) left = left.filter((k) => tool(["alive", String(k.pid)]).alive);
        log("R9", left.length === 0, "...and so are the model servers of the analysis that was RUNNING (" + inflight.map((k) => k.exe + " " + k.pid).join(", ") + ")", left);
        for (const k of left) cp.spawnSync("taskkill", ["/PID", String(k.pid), "/F"], { windowsHide: true });   // (ours: never left behind)
      }
    }
  }
  // what the exit left: the per-language files, the record, the queue, the window's place -- all in the TEST profile
  const enT3 = srtTexts(path.join(MEDIA, "E01 Pilot.en.srt"));
  log("R9", enT3 && enT3[1] && enT3[1].text === "Stay, or be moved.", "after the exit: the English file still holds the edit");
  const pq = path.join(SUV_ROAM, "player.txt");
  const pqText = fs.existsSync(pq) ? fs.readFileSync(pq, "utf8") : "";
  log("R9", pqText.includes("E04.mp3") && pqText.includes("Interview.mp3"), "the queue saved in the TEST profile's SuVtitle\\player.txt (AudioPB's imported item kept)");
  const pr = path.join(SUV_ROAM, "player-resume.txt");
  log("R9", fs.existsSync(pr) && fs.readFileSync(pr, "utf8").includes("the long one.mkv"), "the imported resume entry survived SuVtitle's own rewrites");
  const app = fs.existsSync(path.join(SUV_ROAM, "app.txt")) ? fs.readFileSync(path.join(SUV_ROAM, "app.txt"), "utf8") : "";
  log("R9", /^WINPOS\t-1880\t40\t1000\t720\t0\t0$/m.test(app), "app.txt: the window's place kept (the second screen)", app);

  // ---- R10: --unregister-player on the test root, then the test root deleted ---------------------------------------
  const u = cp.spawnSync(EXE, ["--test-instance", TAG, "--unregister-player"], { env: ENV, timeout: 30000 });
  const left = reg(["query", MY_ROOT, "/s"]);
  const leftVals = (left.out.match(/\bREG_(SZ|NONE)\b/g) || []).length;
  log("R10", u.status === 0 && leftVals === 0, "--unregister-player removed every value it wrote (0 left under the test root)", { status: u.status, left: leftVals });
  // SuVtitle's --register-player (new: what an installer or a script runs): the same 78 values, no window, then gone again
  const rg = cp.spawnSync(EXE, ["--test-instance", TAG, "--register-player"], { env: ENV, timeout: 30000 });
  const regVals = (reg(["query", MY_ROOT, "/s"]).out.match(/\bREG_(SZ|NONE)\b/g) || []).length;
  const regCmd = regSz(MY_ROOT + "\\Classes\\NEXTALE.SuVtitle.Audio\\shell\\open\\command", "");
  const u2 = cp.spawnSync(EXE, ["--test-instance", TAG, "--unregister-player"], { env: ENV, timeout: 30000 });
  const left2 = (reg(["query", MY_ROOT, "/s"]).out.match(/\bREG_(SZ|NONE)\b/g) || []).length;
  log("R10", rg.status === 0 && regVals === 78 && regCmd === "\"" + EXE + "\" --play \"%1\"" && u2.status === 0 && left2 === 0,
      "--register-player (new): 78 values with the same command, exit 0, no window; --unregister-player: 0 again",
      { status: rg.status, values: regVals, cmd: regCmd, left: left2 });
  reg(["delete", TEST_ROOT, "/f"]);
  log("R10", !regExists(TEST_ROOT), "the test root deleted: " + TEST_ROOT + " is gone");

  // ---- R11: the user's real side, after; and AudioPB's side of the TEST profile: never written ----------------------
  const keysAfter = realKeysAbsent();
  log("R11", Object.values(keysAfter).every(Boolean), "after: none of SuVtitle's player keys in the user's registry", keysAfter);
  log("R11", realSuvAbsent(), "after: still no %APPDATA%\\SuVtitle nor %LOCALAPPDATA%\\SuVtitle of the user's");
  const hits = realMentionsTest();
  log("R11", hits.length === 0, "none of the user's real AudioPB files mentions this test (player files, translator.txt, settings, the cache, its registry)", hits);
  const realAfter = snapshotReal();
  const changed = Object.keys(Object.assign({}, realBefore, realAfter)).filter((k) => realBefore[k] !== realAfter[k]);
  log("R11", true, "the user's real AudioPB folders: " + changed.length + " file(s) changed meanwhile (his own AudioPB's -- none mentions the test, above)", changed);
  const apbAfter = snapshotTree([APB_ROAM, path.join(LOCAL, "AudioPB")]);
  const apbChanged = Object.keys(Object.assign({}, apbBefore, apbAfter)).filter((k) => apbBefore[k] !== apbAfter[k]);
  log("R11", apbChanged.length === 0, "AudioPB's folders of the TEST profile: byte for byte, time for time as seeded (the import COPIES)", apbChanged);
  const fg1 = tool(["fg"]);
  log("R11", true, "foreground before / after: " + fg0.cls + " / " + fg1.cls);
  const bad = results.filter((r) => !r.ok);
  console.log(bad.length ? "SOME R CHECKS FAILED (" + bad.length + ")" : "ALL R CHECKS PASSED (" + results.length + ")");
  return bad.length ? 1 : 0;
}

main().then((code) => process.exit(code), (e) => { console.error(e); process.exit(2); });

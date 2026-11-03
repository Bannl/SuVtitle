// live_real_e2e.js -- the REAL SuVtitle.exe (Debug build) as a TEST INSTANCE with the REAL ENGINE (CPU models) and the real capture hub fed by
// `wav:` test inputs, driven through CDP like the user drives it. 2026-10-25, wave L4 (test agent). Sibling of live_e2e.js (L2: the same app against
// the FAKE engine); same discipline: --test-instance suvl4 (its own mutex / window classes / registry root), an ISOLATED profile (APPDATA /
// LOCALAPPDATA / TEMP -> <SUV_L4>\profile; the product refuses a test instance on the real one), the windows on the SECOND screen (app.txt WINPOS /
// LIVEPOS at x < 0), never activated, CDP port 9384 (WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS overrides the product's own switches: it repeats all three),
// kills only the processes it started (by pid).
//
//   SUV_L4=<folder> SUVTITLE_PAYLOAD=<dev payload> SUVTITLE_TEST_MEDIA=<media> node tests\e2e\real\live_real_e2e.js [live | film | smoke | all]
//     <SUV_L4>\bin must hold real_tools.exe (run_live_real_e2e.cmd builds it); SuVtitle.exe, WebView2Loader.dll, web\ and SuVtitleEngine.exe (the REAL
//     engine) are COPIED from build\x64-Debug on every run (never run the shared build folder's exes: a relink must not find them in use).
//     live  = R-L1: --live alone, two wav lanes (Alice / Bob), Listen, labels per lane, translations, meters, Listen off, close.
//     film  = R-L2 + R-L3: the player analyses a film on the CPU; Listen in the Live window cancels the job, Listen off resumes it with `have`; then
//             closing the Live window while listening, and closing the player.
//     smoke = the harness without a model (--live, the strips injected through the page, no Listen): no CPU gate.
//     filmplain = diagnostic: the same film cancelled by the user's own Cancel and re-analysed by his Analyse, no live in between (is a hole in the resumed cues the live's doing?).
//     filmbase = the same film analysed uninterrupted (the yardstick of the resumed one; `film` and `all` run it first, SUV_L4_NOBASE=1 skips it).
//     all   = film then live (default). Results: <SUV_L4>\work\L4_<mode>.jsonl, shots\, logs\ (engine.log copies, message traces).
// The CPU gate (3 samples < 35 % ~20 s apart, nobody else's whisper-server / llama-server / SuVtitleEngine, no other session's test instance) runs
// before each model run; SUV_L4_GATE_MIN (default 12) = minutes it may wait for a calm machine. ONE model run at a time (the modes run in sequence).
// Env for the film: AUDIOPB_TR_TEST_FILE_ASR (default ggml-base.bin: the test engine reads it only under --cpu) -- the MT stays the shipping 4B so the
// llama server is KEPT across the switch to the live path (the case the `priority:` lines are about).
// Exit 0 = every check passed.
"use strict";
const fs = require("fs"), path = require("path"), cp = require("child_process"), os = require("os");

const REPO = path.resolve(__dirname, "..", "..", "..");
const L4 = path.resolve(process.env.SUV_L4 || path.join(REPO, "scratchpad", "l4"));
const BIN = path.join(L4, "bin"), BUILD = path.join(REPO, "build", "x64-Debug");
const EXE = path.join(BIN, "SuVtitle.exe"), TOOLS = path.join(BIN, "real_tools.exe");
const PROF = path.join(L4, "profile"), ROAM = path.join(PROF, "Roaming"), LOCAL = path.join(PROF, "Local"), TMPD = path.join(PROF, "Temp");
const SUV_ROAM = path.join(ROAM, "SuVtitle"), SUV_LOCAL = path.join(LOCAL, "SuVtitle");
const APPTXT = path.join(SUV_ROAM, "app.txt"), ENGTXT = path.join(SUV_ROAM, "engine.txt"), JOURNALS = path.join(SUV_ROAM, "Live sessions");
const ENGLOG = path.join(SUV_LOCAL, "engine.log"), SUV_FILES = path.join(SUV_LOCAL, "Files");
const SHOTS = path.join(L4, "shots"), WORKD = path.join(L4, "work"), LOGS = path.join(L4, "logs"), MEDIA = path.join(L4, "media");
const TAG = "suvl4", PORT = 9384;
const LIVE_CLASS = "SuVtitle.LiveWindow." + TAG, MAIN_CLASS = "SuVtitle.MainWindow." + TAG;
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays --enable-blink-features=AudioVideoTracks";
const REAL_APPDATA = process.env.APPDATA, REAL_LOCAL = process.env.LOCALAPPDATA;
const PAYLOAD = process.env.SUVTITLE_PAYLOAD || "", TESTMEDIA = process.env.SUVTITLE_TEST_MEDIA || "";
const MODE = (process.argv[2] || "all").toLowerCase();

// ---- the two lanes: Alice (English, plays once then a quiet microphone) and Bob (Chinese, his first period "unplugged": he speaks AFTER Alice) ----
const EN_WAV = path.join(TESTMEDIA, "bench_en.wav"), ZH_WAV = path.join(TESTMEDIA, "bench_zh.wav");
function wavSeconds(p) {                                                  // PCM WAV: the duration of the data chunk
  const b = fs.readFileSync(p);
  let off = 12, rate = 0, ch = 1, bits = 16, data = 0;
  while (off + 8 <= b.length) {
    const id = b.toString("latin1", off, off + 4), sz = b.readUInt32LE(off + 4);
    if (id === "fmt ") { ch = b.readUInt16LE(off + 10); rate = b.readUInt32LE(off + 12); bits = b.readUInt16LE(off + 22); }
    if (id === "data") { data = sz; break; }
    off += 8 + sz + (sz & 1);
  }
  return rate && data ? data / (ch * (bits / 8)) / rate : 0;
}
let KEY_ALICE = "", KEY_BOB = "", ZH_SEC = 0, EN_SEC = 0;
function laneKeys() {
  EN_SEC = wavSeconds(EN_WAV); ZH_SEC = wavSeconds(ZH_WAV);
  const gap = (Math.ceil(ZH_SEC * 100) / 100).toFixed(2);               // the end of his first period, rounded UP to the hub's 10 ms packet
  KEY_ALICE = "wav:" + EN_WAV;
  KEY_BOB = "wav:" + ZH_WAV + "@loop@gap=0:" + gap;
}

// ---- results ----
const results = [];
let logPath = path.join(WORKD, "L4.jsonl");
function log(step, ok, what, detail) {
  results.push({ step, ok: !!ok });
  try { fs.appendFileSync(logPath, JSON.stringify({ step, ok: !!ok, what, detail: detail === undefined ? null : detail }) + "\n"); } catch { /* the folder is made by main */ }
  console.log((ok ? "  ok    " : "  FAIL  ") + step + "  " + what + (ok || detail === undefined ? "" : "  -> " + JSON.stringify(detail).slice(0, 700)));
}
function note(step, what, detail) {                                      // a measurement, not a verdict
  try { fs.appendFileSync(logPath, JSON.stringify({ step, note: true, what, detail: detail === undefined ? null : detail }) + "\n"); } catch { /* ok */ }
  console.log("  note  " + step + "  " + what + (detail === undefined ? "" : "  = " + JSON.stringify(detail).slice(0, 600)));
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Post-fix verification (26/10): the mute through the PAGE's own storage. connect() returns as soon as the CDP target's URL names the page; on a FRESH
// profile (the first launch of a user-data folder; 2 of 3 fresh folders here) the document the evaluation lands in may still be the initial
// about:blank for a moment -- "SecurityError: Failed to read the 'localStorage' property ... Access is denied for this document" -- and the kit died on
// it (FATAL) at its first storage access. Waited for (a retry every 200 ms, 20 s at most), and SAID when it happened, with the page's own state.
async function muteViaStorage(player) {
  let retried = 0, last = "", where = null;
  for (let i = 0; i < 100; i++) {
    try {
      await player.ev("localStorage.setItem('audiopb.tr.muted', '1')");
      if (retried) note("A1", "the player page's storage was not readable at first (the initial document of a fresh profile): waited " + (retried * 0.2).toFixed(1) + " s", { retries: retried, firstError: last.slice(0, 110), pageThen: where });
      return;
    } catch (e) {
      last = String((e && e.message) || e);
      if (!/SecurityError|Access is denied/i.test(last)) throw e;
      if (!where) where = await player.ev("({ href: location.href, readyState: document.readyState, origin: String(location.origin), secure: isSecureContext })").catch(() => null);
      retried++;
      await sleep(200);
    }
  }
  throw new Error("the player page's localStorage stayed denied for 20 s: " + last.slice(0, 200) + " page=" + JSON.stringify(where));
}
const under = (p, root) => path.resolve(p).toLowerCase().startsWith(path.resolve(root).toLowerCase() + path.sep);
async function until(fn, ms, step = 100) {                               // the value fn() returns when truthy, else null after ms
  const end = Date.now() + ms;
  for (;;) {
    let v = null;
    try { v = await fn(); } catch { v = null; }
    if (v) return v;
    if (Date.now() > end) return null;
    await sleep(step);
  }
}
const readText = (p) => { try { return fs.readFileSync(p, "utf8"); } catch { return ""; } };
const hasCJK = (s) => /[\u3400-\u9fff]/.test(s || "");
const hasLatin = (s) => /[A-Za-z]{2,}/.test(s || "");
const fmtS = (ms) => (ms / 1000).toFixed(1) + " s";

// ---- inline PowerShell (never a .ps1: Bitdefender) ----
function ps(cmd, timeout = 40000) {
  const r = cp.spawnSync("powershell", ["-NoProfile", "-NonInteractive", "-Command", cmd], { encoding: "utf8", timeout, windowsHide: true });
  return r.stdout || "";
}
function psJson(cmd) { try { const j = JSON.parse(ps(cmd).trim() || "null"); return j === null ? [] : Array.isArray(j) ? j : [j]; } catch { return []; } }
function prioOf(pids) {                                                  // the REAL priority class of each pid (independent of the engine's own log)
  if (!pids.length) return {};
  const rows = psJson("Get-Process -Id " + pids.join(",") + " -ErrorAction SilentlyContinue | Select-Object Id, @{n='P';e={[string]$_.PriorityClass}} | ConvertTo-Json -Compress");
  const m = {};
  for (const r of rows) m[r.Id] = r.P;
  return m;
}

// ---- the CPU gate and the monitor ----
async function cpuPct(ms) {
  const a = os.cpus();
  await sleep(ms);
  const b = os.cpus();
  let idle = 0, tot = 0;
  for (let i = 0; i < a.length; i++) {
    const d = (k) => b[i].times[k] - a[i].times[k];
    idle += d("idle"); tot += d("user") + d("nice") + d("sys") + d("idle") + d("irq");
  }
  return tot ? 100 * (1 - idle / tot) : 0;
}
function foreignModelProcs() {
  return psJson("Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(whisper-server|llama-server|SuVtitleEngine)\\.exe$' -or ($_.Name -eq 'SuVtitle.exe' -and $_.CommandLine -match '--test-instance (suva|suvb|suvr1)') } | Select-Object ProcessId, Name | ConvertTo-Json -Compress");
}
async function gate(label) {
  if (process.env.SUV_L4_NOGATE === "1") { note("GATE", label + ": SKIPPED (SUV_L4_NOGATE=1)"); return true; }
  const limit = Date.now() + (+(process.env.SUV_L4_GATE_MIN || 12)) * 60000;
  let attempt = 0;
  while (Date.now() < limit) {
    attempt++;
    const samples = [];
    let bad = null;
    for (let i = 0; i < 3 && !bad; i++) {
      if (i) await sleep(18000);
      const pct = await cpuPct(2000);
      samples.push(+pct.toFixed(1));
      const fp = foreignModelProcs();
      if (pct >= 35) bad = "CPU " + pct.toFixed(1) + " % >= 35";
      else if (fp.length) bad = "foreign process(es): " + fp.map((x) => x.Name + ":" + x.ProcessId).join(",");
    }
    if (!bad) { note("GATE", label + ": calm (attempt " + attempt + ")", { samples }); return true; }
    note("GATE", label + ": not calm (attempt " + attempt + "): " + bad, { samples });
    await sleep(15000);
  }
  return false;
}
function startMon() {
  const s = [];
  let prev = os.cpus();
  const t = setInterval(() => {
    const now = os.cpus();
    let idle = 0, tot = 0;
    for (let i = 0; i < now.length; i++) {
      const d = (k) => now[i].times[k] - prev[i].times[k];
      idle += d("idle"); tot += d("user") + d("nice") + d("sys") + d("idle") + d("irq");
    }
    prev = now;
    if (tot) s.push(100 * (1 - idle / tot));
  }, 4000);
  return { stop() { clearInterval(t); const n = s.length; return { n, mean: n ? +(s.reduce((a, b) => a + b, 0) / n).toFixed(1) : 0, max: n ? +Math.max(...s).toFixed(1) : 0 }; } };
}

// ---- the isolated environment ----
function testEnv() {
  const e = {};
  for (const k of Object.keys(process.env)) {
    const u = k.toUpperCase();
    if (["APPDATA", "LOCALAPPDATA", "TEMP", "TMP", "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"].includes(u)) continue;
    e[k] = process.env[k];
  }
  e.APPDATA = ROAM; e.LOCALAPPDATA = LOCAL; e.TEMP = TMPD; e.TMP = TMPD;
  e.WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=" + PORT + " " + SWITCHES;
  if (e.AUDIOPB_TR_TEST_FILE_ASR === undefined) e.AUDIOPB_TR_TEST_FILE_ASR = "ggml-base.bin";
  return e;
}
function tool(...args) {
  const r = cp.spawnSync(TOOLS, args, { encoding: "utf8", timeout: 30000, windowsHide: true });
  try { return JSON.parse((r.stdout || "").trim().split("\n").pop()); } catch { return { ok: false, err: "no JSON", raw: r.stdout, code: r.status }; }
}
const winsOf = (pid) => (tool("wins", String(pid)).wins || []);
const alive = (pid) => !!tool("alive", String(pid)).alive;
const kidsOf = (pid) => (tool("kids", String(pid)).kids || []);
const launched = [];
function launch(args) {
  const child = cp.spawn(EXE, ["--test-instance", TAG, ...args], { env: testEnv(), stdio: "ignore", windowsHide: false, detached: false });
  launched.push(child);
  child.exited = null;
  child.on("exit", (code) => { child.exited = code; });
  return child;
}
function prepProfile(extraApp) {
  if (!under(PROF, L4) || path.resolve(ROAM).toLowerCase() === String(REAL_APPDATA).toLowerCase()) throw new Error("profile isolation broken");
  fs.rmSync(PROF, { recursive: true, force: true });                      // ours (no junction in it)
  fs.mkdirSync(SUV_ROAM, { recursive: true }); fs.mkdirSync(SUV_LOCAL, { recursive: true }); fs.mkdirSync(TMPD, { recursive: true });
  fs.copyFileSync(path.join(BUILD, "SuVtitle.exe"), EXE);
  fs.copyFileSync(path.join(BUILD, "WebView2Loader.dll"), path.join(BIN, "WebView2Loader.dll"));
  fs.copyFileSync(path.join(BUILD, "SuVtitleEngine.exe"), path.join(BIN, "SuVtitleEngine.exe"));
  fs.rmSync(path.join(BIN, "web"), { recursive: true, force: true }); fs.cpSync(path.join(BUILD, "web"), path.join(BIN, "web"), { recursive: true });
  // both windows on the second screen (-1920,0 1920x1080): the player 1100 x 720, the Live window 900 x 780
  fs.writeFileSync(APPTXT, "# SuVtitle app v1\nWINPOS\t-1880\t40\t1100\t720\t0\t0\nLIVEPOS\t-1900\t60\t900\t780\t0\t0\n" + (extraApp || ""));
  // the engine's file, seeded (its first start would otherwise seed the defaults): the player's pair AND the live pair en <-> zh
  fs.writeFileSync(ENGTXT, "# SuVtitle Engine config v1\nPRESET\tbalanced\nMODE\tauto\nLANGA\ten\nLANGB\tzh\nLIVEMODE\tauto\nLIVELANGA\ten\nLIVELANGB\tzh\n");
}

// ---- CDP (Node's own WebSocket) ----
async function targets() { return (await fetch("http://127.0.0.1:" + PORT + "/json")).json(); }
const HOOK_JS = `(() => { let fresh = false; if (!window.__m) { fresh = true; window.__m = []; window.__lv = {};
  chrome.webview.addEventListener('message', (e) => { let d = e.data; if (typeof d === 'string') { try { d = JSON.parse(d); } catch { d = { type: '?', raw: d }; } }
    const t = Date.now();
    if (d && d.type === 'liveLevels') { const L = d.levels || {}; for (const k of Object.keys(L)) (window.__lv[k] = window.__lv[k] || []).push([t, L[k]]); return; }
    if (window.__m.length < 30000) window.__m.push({ t, d }); }); }
  return { fresh, len: window.__m.length }; })()`;
async function connect(urlPart, ms = 20000) {
  const t = await until(async () => (await targets()).find((x) => x.type === "page" && x.url.includes(urlPart)), ms, 250);
  if (!t) return null;
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("ws error")); });
  let id = 0;
  const pending = new Map(), listeners = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); d.error ? p.rej(new Error(JSON.stringify(d.error))) : p.res(d.result); }
    else listeners.forEach((f) => f(d));
  };
  const send = (method, params = {}) => new Promise((res, rej) => {
    const n = ++id; pending.set(n, { res, rej }); ws.send(JSON.stringify({ id: n, method, params }));
    setTimeout(() => { if (pending.has(n)) { pending.delete(n); rej(new Error("timeout " + method)); } }, 30000);
  });
  const page = {
    send, close: () => { try { ws.close(); } catch { /* gone */ } }, msgs: [], mIdx: 0, errors: [], url: t.url,
    async ev(expr) {
      const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error("page threw: " + JSON.stringify(r.exceptionDetails).slice(0, 300));
      return r.result.value;
    },
    async shot(name) {
      try { const r = await send("Page.captureScreenshot", { format: "png" }); fs.writeFileSync(path.join(SHOTS, name + ".png"), Buffer.from(r.data, "base64")); } catch { /* a hidden page */ }
    },
    readyMsg: null, rehooks: 0, lastErr: "",
    async hook() { const r = await page.ev(HOOK_JS); if (r.fresh) page.mIdx = 0; return r; },
    async pull() {                                                       // what the app said to the page since the last pull (timestamped by the page)
      try {
        const r = await page.hook();                                      // a reloaded document has no hook: put it back, and ask the app for its state again
        if (r.fresh) { page.rehooks++; if (page.readyMsg) await page.ev("window.chrome.webview.postMessage(" + JSON.stringify(page.readyMsg) + ")"); }
      } catch (e) { page.lastErr = String(e).slice(0, 200); return 0; }
      const arr = await page.ev("window.__m ? window.__m.slice(" + page.mIdx + ") : []").catch((e) => { page.lastErr = String(e).slice(0, 200); return []; });
      page.mIdx += arr.length;
      for (const x of arr) page.msgs.push(x);
      return arr.length;
    },
    levels: () => page.ev("window.__lv || {}"),
  };
  listeners.push((d) => {
    if (d.method === "Runtime.exceptionThrown") page.errors.push("exception: " + JSON.stringify(d.params.exceptionDetails).slice(0, 300));
    else if (d.method === "Runtime.consoleAPICalled" && (d.params.type === "error" || d.params.type === "warning"))
      page.errors.push("console." + d.params.type + ": " + d.params.args.map((x) => x.value || x.description || "").join(" ").slice(0, 300));
    else if (d.method === "Log.entryAdded" && (d.params.entry.level === "error" || d.params.entry.level === "warning"))
      page.errors.push("log." + d.params.entry.level + ": " + String(d.params.entry.text).slice(0, 300) + " " + (d.params.entry.url || ""));
  });
  await send("Runtime.enable"); await send("Page.enable"); await send("Log.enable");
  return page;
}
const ofType = (page, type, from = 0) => page.msgs.slice(from).filter((m) => m.d && m.d.type === type);

// ---- the pages' own readings (the Live page: the L2 kit's) ----
const Q = {
  strips: () => `Array.from(document.querySelectorAll('#strips .strip')).map(s => ({ name: s.querySelector('.strip-name').value, src: s.querySelector('.strip-src-name').textContent,
      key: s.dataset.key, chan: s.dataset.chan, lamp: s.querySelector('.strip-lamp').className, why: s.querySelector('.strip-why').hidden ? '' : s.querySelector('.strip-why').textContent,
      lvl: s.querySelector('.meter').style.getPropertyValue('--lvl'), mt: s.querySelector('.meter').title }))`,
  lines: () => `Array.from(document.querySelectorAll('#log .tr-para')).map(p => ({ who: (p.querySelector('.tr-who-name')||{}).textContent || '', key: p.dataset.spk || '', n: p.querySelectorAll('.tr-line').length,
      dst: Array.from(p.querySelectorAll('.tr-dst')).map(x => x.textContent).join(' | ') }))`,
  listen: () => `({ pressed: document.getElementById('listenBtn').getAttribute('aria-pressed'), lit: document.getElementById('onAir').classList.contains('is-lit'), pending: document.getElementById('listenBtn').classList.contains('is-pending') })`,
  notice: () => `({ hidden: document.getElementById('liveNotice').hidden, text: document.getElementById('liveNotice').textContent, onAir: document.getElementById('liveBtn').classList.contains('is-on-air') })`,
  // (post-fix verification, 26/10) the drain: LIVE 0 = stop listening THEN finish -- the page says so until STATE live=0 (live_e2e.js C20 reads the same ids)
  finishing: () => `({ st: document.getElementById('state').textContent, fin: document.getElementById('onAir').classList.contains('is-finishing'), lit: document.getElementById('onAir').classList.contains('is-lit'), pressed: document.getElementById('listenBtn').getAttribute('aria-pressed') })`,
};
const clickListen = (live) => live.ev("document.getElementById('listenBtn').click()");
function meterSampler(live, n) {                                         // the strips' DOM meters, polled in the background: the highest --lvl each reached
  const max = new Array(n).fill(0);
  let stop = false;
  const loop = (async () => {
    while (!stop) {
      try { const st = await live.ev(Q.strips()); st.forEach((x, i) => { const v = parseFloat(x.lvl); if (i < n && v > max[i]) max[i] = v; }); } catch { /* a reload */ }
      await sleep(250);
    }
  })();
  return { max, async stop() { stop = true; await loop; } };
}
async function injectStrips(live, strips) {
  const cfg = { type: "liveConfig", inputs: strips.map((s) => ({ key: s.key, label: s.label, chan: 0, gainDb: 0 })) };
  await live.ev("window.chrome.webview.postMessage(" + JSON.stringify(cfg) + ")");
  return until(async () => { const s = await live.ev(Q.strips()); return s.length === strips.length && s.every((x, i) => x.name === strips[i].label) ? s : null; }, 10000);
}
const stripLines = () => readText(APPTXT).replace(/\r\n/g, "\n").split("\n").filter((l) => l.startsWith("LIVEIN\t"));

// ---- the engine's log (test profile) ----
function elogLines() {
  return readText(ENGLOG).split("\n").filter(Boolean).map((l) => {
    const m = /^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3}) (.*)$/.exec(l);
    return m ? { t: new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6], +m[7]).getTime(), text: m[8] } : { t: 0, text: l };
  });
}
const ROOTS = [PROF, L4, REPO, path.dirname(REPO), TESTMEDIA, PAYLOAD].filter(Boolean).sort((x, y) => y.length - x.length);
const scrub = (s) => {                                                   // no user path in a report (the roots first: they hold spaces)
  let t = String(s);
  for (const r of ROOTS) t = t.split(r).join("<root>");
  return t.replace(/wav:[^;\n]*?(\.wav)(@[^;,\s]*)*/gi, "wav:<path>$1$2").replace(/[A-Za-z]:\\[^\s;,"]+/g, "<path>");
};
const prioLines = (since = 0) => elogLines().filter((l) => l.t >= since && /\] priority: /.test(l.text)).map((l) => ({ t: l.t, text: scrub(l.text.replace(/^\[SuVtitleEngine\] /, "")) }));
function archiveEngineLog(name) {
  try { fs.mkdirSync(LOGS, { recursive: true }); fs.writeFileSync(path.join(LOGS, name + "_engine.log"), scrub(readText(ENGLOG))); } catch { /* ok */ }
}
function saveTrace(page, name) {
  try { fs.mkdirSync(LOGS, { recursive: true }); fs.writeFileSync(path.join(LOGS, name + "_msgs.jsonl"), page.msgs.map((m) => JSON.stringify(m)).join("\n")); } catch { /* ok */ }
}

// ---- a LINE's verdicts ----
function laneVerdict(m) {                                                // what lane / label / translation a final line must carry, from ITS OWN text
  const d = m.d, spk = d.spk || {};
  const zhSrc = hasCJK(d.src);
  const wantKey = zhSrc ? KEY_BOB : KEY_ALICE, wantName = zhSrc ? "Bob" : "Alice";
  const labelOk = new RegExp("^" + wantName + "( . (voice )?\\d+)?$").test(String(spk.label || ""));   // " · voice n" since RESUME 11
  const laneOk = spk.slot === wantKey;
  const dstOk = String(d.dst || "").trim().length > 0 && (zhSrc ? (!hasCJK(d.dst) && hasLatin(d.dst)) : hasCJK(d.dst));
  const langOk = zhSrc ? d.lang === "zh" : d.lang === "en";
  const dstLangOk = zhSrc ? d.dstLang === "en" : d.dstLang === "zh";
  return { who: wantName, zhSrc, labelOk, laneOk, dstOk, langOk, dstLangOk, label: spk.label, slot: spk.slot === KEY_ALICE ? "<alice>" : spk.slot === KEY_BOB ? "<bob>" : scrub(spk.slot), lang: d.lang, dstLang: d.dstLang, src: String(d.src || "").slice(0, 60), dst: String(d.dst || "").slice(0, 60) };
}
function levelStats(levels) {                                            // {lane: [[t, dB], ...]} -> the meters' story
  const out = {};
  for (const [name, key] of [["alice", KEY_ALICE], ["bob", KEY_BOB]]) {
    const arr = (levels[key] || []).filter((x) => typeof x[1] === "number" && isFinite(x[1]));
    let peak = -999, tPeak = 0;
    for (const [t, v] of arr) if (v > peak) { peak = v; tPeak = t; }
    out[name] = { n: (levels[key] || []).length, peak: +peak.toFixed(1), tPeak, loud: arr.filter((x) => x[1] > -50).length, first: arr.length ? arr[0][0] : 0 };
  }
  return out;
}

// ============================================================================================================================
// RUN 1 -- R-L1: --live alone, two wav lanes
// ============================================================================================================================
async function runLive(withListen) {
  const R = withListen ? "L1" : "S";
  logPath = path.join(WORKD, "L4_" + (withListen ? "live" : "smoke") + ".jsonl"); fs.writeFileSync(logPath, "");
  prepProfile();
  if (withListen && !(await gate("live"))) { log(R + ".gate", false, "the CPU never got calm within the gate's time: NOT RUN"); return; }
  const mon = startMon();
  const a = launch(["--live"]);
  const pid = a.pid;
  const live = await connect("live.html");
  log(R + ".1", !!live, "--live: the Live page is served and loaded (CDP target live.html)");
  if (!live) return;
  await live.hook();
  await live.ev("window.chrome.webview.postMessage({ type: 'ready' })");   // the app answers with the full state (the page's own first `ready` went before the hook)
  const wins = await until(() => { const w = winsOf(pid); return w.find((x) => x.cls === LIVE_CLASS && x.visible) ? w : null; }, 15000);
  const lw = (wins || []).find((x) => x.cls === LIVE_CLASS), pw = (wins || []).find((x) => x.cls === MAIN_CLASS);
  log(R + ".2", lw && lw.visible && lw.x === -1900 && lw.y === 60 && lw.noactivate && pw && !pw.visible && tool("fg").pid !== pid,
    "the Live window is up ALONE on the second screen (LIVEPOS), never activated, the player's window not shown, the foreground is not ours", { lw, pwVisible: pw && pw.visible });
  const eng = await until(() => kidsOf(pid).find((k) => /^SuVtitleEngine\.exe$/i.test(k.exe)), 20000, 300);
  const enginePid = eng && eng.pid;
  log(R + ".3", !!enginePid, "the REAL engine (SuVtitleEngine.exe) is the app's child");
  const st0 = await until(async () => { await live.pull(); return ofType(live, "translatorState").pop(); }, 30000, 300);
  log(R + ".4", st0 && st0.d.running === true && st0.d.langA === "en" && st0.d.langB === "zh" && st0.d.live && st0.d.live.on === false,
    "the engine's first STATE reaches the Live page: running, the LIVE pair en <-> zh, not listening", st0 && { status: st0.d.status, running: st0.d.running, langA: st0.d.langA, langB: st0.d.langB, live: st0.d.live && st0.d.live.on });
  // the strips, THROUGH THE PAGE (the host accepts a wav: key only for a test instance; the picker never lists one)
  const strips = await injectStrips(live, [{ key: KEY_ALICE, label: "Alice" }, { key: KEY_BOB, label: "Bob" }]);
  log(R + ".5", !!strips && strips[0].key === KEY_ALICE && strips[1].key === KEY_BOB, "liveConfig with two wav: keys is ACCEPTED: the page shows Alice and Bob (the engine echo)", strips && strips.map((s) => s.name + ":" + s.chan));
  await until(() => stripLines().length === 2, 5000);
  const lin = stripLines();
  log(R + ".6", lin.length === 2 && lin[0].split("\t")[2] === "Alice" && lin[1].split("\t")[2] === "Bob", "...and persisted in app.txt (LIVEIN x2)", lin.length);
  if (!withListen) {
    log(R + ".7", live.errors.length === 0, "the Live page logged no console error / CSP violation", live.errors);
    tool("post-close", LIVE_CLASS, String(pid));
    const gone = await until(() => (a.exited !== null ? true : null), 20000, 200);
    log(R + ".8", !!gone && !alive(pid) && (!enginePid || !alive(enginePid)), "closing the Live window (the player never shown): the app exits and the engine goes with it", { exit: a.exited });
    mon.stop(); archiveEngineLog("smoke");
    return;
  }

  // ---- Listen ----
  await live.shot("L1_before_listen");
  await live.pull();
  const nState = live.msgs.length;
  const preAsr = ((ofType(live, "translatorState").pop() || { d: {} }).d.models || {}).asr || "";     // the models STATE names while idle (a FILE job's)
  const tClick = Date.now();
  const sampler = meterSampler(live, 2);
  await clickListen(live);
  const lit = await until(async () => { const l = await live.ev(Q.listen()); return l.lit && l.pressed === "true" ? l : null; }, 60000, 150);
  const tLit = Date.now();
  log(R + ".9", !!lit, "Listen: the on-air lamp lights from the engine's echo (" + (lit ? fmtS(tLit - tClick) : "never") + " after the click)", lit);
  // the ANNOUNCEMENT (RESUME 7): once the live pair is up, STATE says `listening` with the models of THAT pair -- not the idle ones ("preAsr") that the
  // first echo of LIVE 1 still carries for a moment (status `listening`, live.on, then at once `loading`)
  const sListening = await until(async () => { await live.pull(); return ofType(live, "translatorState", nState).find((m) => m.d.live && m.d.live.on && m.d.status === "listening" && m.d.models && m.d.models.asr && m.d.models.asr !== preAsr); }, 180000, 300);
  const sm = sListening && sListening.d;
  const echo1 = ofType(live, "translatorState", nState).find((m) => m.d.live && m.d.live.on);
  note(R + ".t", "STATE live=1 (the first echo) / STATE listening with the live pair's models", (echo1 ? fmtS(echo1.t - tClick) : "NEVER") + " / " + (sListening ? fmtS(sListening.t - tClick) : "NEVER") + " after the click");
  log(R + ".10", !!sm && sm.mode === "auto" && sm.langA === "en" && sm.langB === "zh" && sm.preset === "balanced" && /^(ggml-)?small(\.bin)?$/.test(sm.models.asr) && /Qwen3-4B/.test(sm.models.mt),
    "STATE says listening with the LIVE pair (en <-> zh) and the models of the live path (balanced = small + the 4B; the idle STATE said " + preAsr + ")", sm && { status: sm.status, preset: sm.preset, mode: sm.mode, langA: sm.langA, langB: sm.langB, models: sm.models, gpu: sm.gpu, spk: sm.spk });
  const ls = elogLines().find((l) => l.t >= tClick && /live: started -- 2 input\(s\)/.test(l.text));
  log(R + ".11", !!ls && /pair en\/zh/.test(ls.text), "engine.log: \"live: started -- 2 input(s)\" with the pair en/zh", ls && scrub(ls.text));
  const kidsLive = enginePid ? kidsOf(enginePid) : [];
  note(R + ".kids", "the engine's children once the live pair is up", kidsLive.map((k) => k.exe));

  // ---- the conversation ----
  const states = { alice: new Set(), bob: new Set() };
  const tLoop = Date.now();
  let finals = [];
  let firstLine = null;
  for (;;) {
    await live.pull();
    finals = ofType(live, "translatorLine", nState);
    if (finals.length && !firstLine) firstLine = finals[0];
    const s = await live.ev(Q.strips());
    const stS = ofType(live, "translatorState", nState).pop();
    if (stS && stS.d.live) stS.d.live.inputs.forEach((i) => { (i.key === KEY_ALICE ? states.alice : states.bob).add(i.state); });
    const nA = finals.filter((m) => !hasCJK(m.d.src)).length, nB = finals.filter((m) => hasCJK(m.d.src)).length;
    if ((nA >= 3 && nB >= 3) || Date.now() - tLoop > 300000) break;
    await sleep(500);
  }
  const nA = finals.filter((m) => !hasCJK(m.d.src)).length, nB = finals.filter((m) => hasCJK(m.d.src)).length;
  await sampler.stop();
  const meterMax = sampler.max;
  note(R + ".t", "first LINE after the Listen click", firstLine ? fmtS(firstLine.t - tClick) : "NEVER");
  note(R + ".n", "final lines in " + fmtS(Date.now() - tClick) + " of listening", { alice: nA, bob: nB, total: finals.length });
  await live.shot("L1_listening");
  log(R + ".12", nA >= 3 && nB >= 3, "LINEs arrive for BOTH lanes (English lines " + nA + ", Chinese lines " + nB + ")", { nA, nB });
  const verdicts = finals.map(laneVerdict);
  const bad = (k) => verdicts.filter((v) => !v[k]);
  log(R + ".13", verdicts.length > 0 && bad("labelOk").length === 0 && bad("laneOk").length === 0,
    "every line carries the label AND the lane of its OWN strip: English text -> Alice, Chinese text -> Bob", bad("labelOk").concat(bad("laneOk")).slice(0, 4));
  log(R + ".14", verdicts.length > 0 && bad("dstOk").length === 0 && bad("langOk").length === 0 && bad("dstLangOk").length === 0,
    "translations are present, in the other language of the pair (English -> Chinese script, Chinese -> English letters), lang / dstLang right", bad("dstOk").concat(bad("langOk"), bad("dstLangOk")).slice(0, 4));
  note(R + ".lines", "the lines", verdicts.map((v) => v.who + "/" + v.label + " [" + v.lang + ">" + v.dstLang + "] " + v.src + " => " + v.dst));
  const dom = await live.ev(Q.lines());
  log(R + ".15", dom.some((p) => p.who === "Alice") && dom.some((p) => p.who === "Bob") && dom.every((p) => /^(Alice|Bob)( . (voice )?\d+)?$/.test(p.who)) && dom.every((p) => p.dst.trim().length > 0),
    "the PAGE shows them: paragraphs named Alice and Bob only, each with text", dom.map((p) => p.who));
  const lv = levelStats(await live.levels());
  note(R + ".levels", "per-lane meters (dBFS peak / samples / samples above -50)", lv);
  log(R + ".16", lv.alice.peak > -50 && lv.bob.peak > -50 && lv.alice.tPeak < lv.bob.tPeak && meterMax[0] > 0 && meterMax[1] > 0,
    "the meters MOVE per lane: Alice and Bob each peak above -50 dBFS (Alice first), and both strips' DOM meters left zero", { lv, meterMax });
  const lvAll = await live.levels();
  const bobEarly = (lvAll[KEY_BOB] || []).filter((x) => x[0] < lv.alice.first + 10000);
  log(R + ".17", bobEarly.length > 0 && bobEarly.every((x) => typeof x[1] !== "number" || !isFinite(x[1]) || x[1] < -60),
    "...and Bob's lane stays quiet while Alice speaks (the lanes are not mixed up)", bobEarly.slice(0, 5));
  log(R + ".18", states.alice.has("open") && states.bob.has("open"), "INPUT: both strips reached `open` (Bob's first period is the test 'unplugged' gap)", { alice: [...states.alice], bob: [...states.bob] });

  // ---- Listen off ----
  const warns = ofType(live, "warn").map((m) => m.d.text);
  const nOff = live.msgs.length, tOff = Date.now();
  await clickListen(live);
  // Post-fix verification (26/10): LIVE 0 is now "stop listening, THEN finish" (the drain, L1a RESUME 9): the lamp stops being lit AT THE CLICK (it blinks,
  // is-finishing, the page says "Finishing the last sentences..."), the queued finals are still translated and delivered, and STATE live=0 comes only when
  // the drain is done -- tens of seconds on the CPU (a 57 s listen left 7 LINEs and a 46 s drain here; the selftest's [C8b] measured 82.6 s). This check
  // encoded the old stop (a 30 s wait for STATE live=0, "dark" judged at the click, which the blinking lamp satisfied at once, and L1.21 counting the
  // drain's own LINEs as late): it now waits for the drain (180 s, [C8b]'s bound), sees the page SAY it is finishing, and judges the lamp after live=0.
  const finishing = await until(async () => { const r = await live.ev(Q.finishing()); return /^Finishing the last sentences/.test(r.st) && r.fin && !r.lit && r.pressed === "false" ? r : null; }, 5000, 100);
  const stOff = await until(async () => { await live.pull(); return ofType(live, "translatorState", nOff).find((m) => m.d.live && m.d.live.on === false); }, 180000, 200);
  const dark = await until(async () => { const r = await live.ev(Q.finishing()); return !r.lit && !r.fin && r.pressed === "false" && !/^Finishing/.test(r.st) ? r : null; }, 15000, 150);
  const drainMs = stOff ? stOff.t - tOff : 0;
  log(R + ".19", !!dark && !!stOff && (drainMs < 3000 || !!finishing),
    "Listen off: the page says \"Finishing the last sentences...\" while the engine drains, STATE live=0 comes back when it is done (" + (stOff ? fmtS(drainMs) : "never") + " after the click), the lamp is dark then",
    { finishing: !!finishing, dark: !!dark, status: stOff && stOff.d.status });
  const inFlight1 = live.msgs.filter((m) => m.t > tOff && stOff && m.t <= stOff.t + 300 && m.d && m.d.type === "translatorLine");
  note(R + ".inflight", "the drain: LINEs delivered between the Listen-off click and STATE live=0 (" + inFlight1.length + ")", inFlight1.map((m) => fmtS(m.t - tOff) + " after the click, " + ((stOff.t - m.t) / 1000).toFixed(2) + " s before live=0"));
  const stp = await until(() => elogLines().find((l) => l.t >= tOff && /live: stopped/.test(l.text)), 20000, 200);
  log(R + ".20", !!stp, "engine.log: \"live: stopped\"", stp && scrub(stp.text));
  await sleep(7000);
  await live.pull();
  const tStop = stOff ? stOff.t : tOff;
  const lateLines = live.msgs.filter((m) => m.t > tStop && m.d && (m.d.type === "translatorLine" || m.d.type === "translatorPartial"));
  log(R + ".21", lateLines.length === 0, "no LINE / PART after STATE live=0 (7 s watched)", lateLines.map((m) => m.d.type));
  const kidsAfter = enginePid ? kidsOf(enginePid) : [];
  log(R + ".22", !kidsAfter.some((k) => /whisper/i.test(k.exe)), "the live whisper children are gone after the stop", { before: kidsLive.map((k) => k.exe), after: kidsAfter.map((k) => k.exe) });
  const stIdle = ofType(live, "translatorState", nOff).pop();
  note(R + ".idle", "STATE after the stop", stIdle && { status: stIdle.d.status, live: stIdle.d.live && stIdle.d.live.on });
  note(R + ".warn", "warn messages seen by the Live page", warns);
  log(R + ".23", live.errors.length === 0, "the Live page raised no exception / console error / CSP violation during the run", live.errors);
  const jr = fs.existsSync(JOURNALS) ? fs.readdirSync(JOURNALS).filter((f) => f.endsWith(".jsonl")) : [];
  log(R + ".24", jr.length === 1 && readText(path.join(JOURNALS, jr[0])).split("\n").filter(Boolean).length >= finals.length, "the session journal holds the lines (one file)", { files: jr.length });
  await live.shot("L1_stopped");
  saveTrace(live, "live");

  // ---- close ----
  tool("post-close", LIVE_CLASS, String(pid));
  const gone = await until(() => (a.exited !== null ? true : null), 30000, 200);
  const engGone = await until(() => (!enginePid || !alive(enginePid) ? true : null), 15000, 200);
  log(R + ".25", !!gone && !!engGone && !alive(pid), "closing the Live window (the player never shown): the app exits and the engine goes with it", { exit: a.exited });
  const cs = mon.stop();
  note(R + ".cpu", "CPU during the run (total, own run included)", cs);
  archiveEngineLog("live");
}

// ============================================================================================================================
// RUN 2 -- R-L2 + R-L3: a film analysed on the CPU, the live path takes the machine and gives it back; then the stop and the quit
// ============================================================================================================================
function parseRecord(text) {
  const o = { rng: "", done: null, cues: [] };
  for (const l of text.split("\n")) {
    const t = l.split("\t");
    if (t[0] === "RNG") o.rng = t[1] || "";
    else if (t[0] === "DONE") o.done = t[1];
    else if (t[0] === "FCUE") o.cues.push({ idx: +t[2], s: +t[3], e: +t[4], src: t[8], dst: t[9] });
  }
  return o;
}
const parseRanges = (s) => (s || "").split(",").filter(Boolean).map((x) => x.split("-").map(Number));
const rangesTotal = (r) => r.reduce((a, x) => a + (x[1] - x[0]), 0);
function findRecord(clipName) {
  if (!fs.existsSync(SUV_FILES)) return null;
  const files = fs.readdirSync(SUV_FILES).map((f) => path.join(SUV_FILES, f)).filter((f) => fs.statSync(f).isFile());
  const hit = files.map((f) => ({ f, text: readText(f) })).filter((x) => x.text.includes("\nSRC\t") && x.text.includes(clipName) && x.text.includes("\nRNG\t"));
  return hit.length ? hit.sort((a, b) => fs.statSync(b.f).mtimeMs - fs.statSync(a.f).mtimeMs)[0] : null;
}

let BASE_CUES = null;
async function runFilmBase() {                                              // the SAME film, analysed from start to end with no live in between: the yardstick of the resumed one
  const R = "B";
  logPath = path.join(WORKD, "L4_filmbase.jsonl"); fs.writeFileSync(logPath, "");
  prepProfile();
  fs.rmSync(MEDIA, { recursive: true, force: true }); fs.mkdirSync(MEDIA, { recursive: true });
  const clip = path.join(MEDIA, "Meeting.wav");
  fs.copyFileSync(path.join(TESTMEDIA, "filemode", "meeting.wav"), clip);
  if (!(await gate("filmbase"))) { log(R + ".gate", false, "the CPU never got calm within the gate's time: NOT RUN"); return; }
  const mon = startMon();
  const a = launch([]);
  const pid = a.pid;
  const player = await connect("index.html");
  log(R + ".1", !!player, "baseline: a plain start, the player page is served");
  if (!player) return;
  await player.hook();
  const eng = await until(() => kidsOf(pid).find((k) => /^SuVtitleEngine\.exe$/i.test(k.exe)), 20000, 300);
  const enginePid = eng && eng.pid;
  await muteViaStorage(player);
  await player.send("Page.reload", { ignoreCache: true });
  await until(async () => (await player.ev("document.readyState")) === "complete", 15000);
  player.readyMsg = { type: "ready", view: "file" };
  await player.hook(true);
  const nP0 = player.msgs.length, tPlay = Date.now();
  cp.spawnSync(EXE, ["--test-instance", TAG, "--play", clip], { env: testEnv(), timeout: 30000, windowsHide: true });
  const quiet = () => player.ev("document.querySelectorAll('#fileVideo,#fileAudio').forEach(e => { try { e.pause(); e.muted = true; } catch (x) {} }); 1").catch(() => 0);
  const done = await until(async () => { await player.pull(); await quiet(); return ofType(player, "translatorFile", nP0).find((m) => m.d.complete && /Meeting\.wav$/i.test(m.d.name || "")); }, 420000, 400);
  log(R + ".2", !!done, "the uninterrupted analysis completes (" + (done ? fmtS(done.t - tPlay) : "never") + ")", done && { count: done.d.count, total: done.d.totalMs });
  await sleep(1500);
  const rec = findRecord("Meeting.wav");
  const r = rec ? parseRecord(rec.text) : null;
  BASE_CUES = r ? r.cues.slice().sort((x, y) => x.s - y.s) : null;
  if (BASE_CUES) fs.writeFileSync(path.join(WORKD, "base_cues.json"), JSON.stringify(BASE_CUES));
  note(R + ".jobs", "engine.log FILE job lines", elogLines().filter((l) => /FILE job \d+ (ok|cancelled|error)/.test(l.text)).map((l) => l.text));
  tool("post-close", MAIN_CLASS, String(pid));
  const exited = await until(() => (a.exited !== null ? true : null), 30000, 200);
  const engGone = await until(() => (!enginePid || !alive(enginePid) ? true : null), 15000, 200);
  log(R + ".3", !!exited && !!engGone, "closing the player ends the app and the engine", { exit: a.exited });
  log(R + ".4", player.errors.length === 0, "the player page logged no error", player.errors.slice(0, 3));
  note(R + ".cpu", "CPU during the run (total, own run included)", mon.stop());
  archiveEngineLog("filmbase");
}

async function runFilmPlain() {                                            // diagnostic: is a hole in the resumed cues the LIVE's doing, or any resume's?
  const R = "P";
  logPath = path.join(WORKD, "L4_filmplain.jsonl"); fs.writeFileSync(logPath, "");
  prepProfile();
  fs.rmSync(MEDIA, { recursive: true, force: true }); fs.mkdirSync(MEDIA, { recursive: true });
  const clip = path.join(MEDIA, "Meeting.wav");
  fs.copyFileSync(path.join(TESTMEDIA, "filemode", "meeting.wav"), clip);
  if (!(await gate("filmplain"))) { log(R + ".gate", false, "the CPU never got calm within the gate's time: NOT RUN"); return; }
  const mon = startMon();
  const a = launch([]);
  const pid = a.pid;
  const player = await connect("index.html");
  log(R + ".1", !!player, "plain cancel / resume: a plain start, the player page is served");
  if (!player) return;
  await player.hook();
  const eng = await until(() => kidsOf(pid).find((k) => /^SuVtitleEngine\.exe$/i.test(k.exe)), 20000, 300);
  const enginePid = eng && eng.pid;
  await muteViaStorage(player);
  await player.send("Page.reload", { ignoreCache: true });
  await until(async () => (await player.ev("document.readyState")) === "complete", 15000);
  player.readyMsg = { type: "ready", view: "file" };
  await player.hook(true);
  const nP0 = player.msgs.length, tPlay = Date.now();
  cp.spawnSync(EXE, ["--test-instance", TAG, "--play", clip], { env: testEnv(), timeout: 30000, windowsHide: true });
  const quiet = () => player.ev("document.querySelectorAll('#fileVideo,#fileAudio').forEach(e => { try { e.pause(); e.muted = true; } catch (x) {} }); 1").catch(() => 0);
  const fileMsgs = (from) => ofType(player, "translatorFile", from).filter((m) => /Meeting\.wav$/i.test(m.d.name || ""));
  const minDone = +(process.env.SUV_L4_MIN_DONE_MS || 30000);
  const at = await until(async () => { await player.pull(); await quiet(); return fileMsgs(nP0).filter((m) => m.d.phase === "analyzing" && !m.d.complete && rangesTotal(m.d.ranges || []) >= minDone).pop(); }, 240000, 250);
  log(R + ".2", !!at, "the analysis reached " + (minDone / 1000) + " s (" + (at ? fmtS(at.t - tPlay) + " after --play" : "never") + ")");
  if (!at) { archiveEngineLog("filmplain"); return; }
  const tCancel = Date.now();
  await player.ev("window.chrome.webview.postMessage({ type: 'translatorFileCancel', jobId: " + at.d.jobId + " })");     // (the app ignores a Cancel that names another generation)
  const cancelled = await until(() => elogLines().find((l) => l.t >= tCancel && /FILE job \d+ cancelled/.test(l.text)), 90000, 300);
  log(R + ".3", !!cancelled, "the user's Cancel ends the job `cancelled` (" + (cancelled ? fmtS(cancelled.t - tCancel) + " after the click" : "never") + ")", cancelled && cancelled.text);
  await sleep(1500); await player.pull();
  const last = fileMsgs(nP0).pop();
  const ranges1 = last ? last.d.ranges : [];
  note(R + ".cut", "what the player holds after the Cancel", { phase: last && last.d.phase, ranges: ranges1, cues: last && last.d.count });
  const nP2 = player.msgs.length, tAn = Date.now();
  await player.ev("window.chrome.webview.postMessage({ type: 'translatorFileAnalyse', force: false })");
  const done = await until(async () => { await player.pull(); await quiet(); return ofType(player, "translatorFile", nP2).find((m) => m.d.complete && /Meeting\.wav$/i.test(m.d.name || "")); }, 420000, 400);
  log(R + ".4", !!done, "the user's Analyse resumes it and it completes (" + (done ? fmtS(done.t - tAn) + " after the click" : "never") + ")");
  await sleep(1500);
  const rec = findRecord("Meeting.wav");
  const cs = rec ? parseRecord(rec.text).cues.slice().sort((x, y) => x.s - y.s) : [];
  const jobs = elogLines().filter((l) => l.t >= tPlay - 1000 && /FILE job \d+ (ok|cancelled|error)/.test(l.text)).map((l) => l.text);
  note(R + ".jobs", "engine.log FILE job lines", jobs);
  const maxGap = (cues) => { const c = cues.slice().sort((x, y) => x.s - y.s); let g = 0, atMs = 0; for (let i = 1; i < c.length; i++) if (c[i].s - c[i - 1].e > g) { g = c[i].s - c[i - 1].e; atMs = c[i - 1].e; } return (g / 1000).toFixed(1) + " s after " + (atMs / 1000).toFixed(1) + " s"; };
  const readJson = (p) => { try { return JSON.parse(fs.readFileSync(p, "utf8")); } catch { return null; } };
  const base = readJson(path.join(WORKD, "base_cues.json")), liveRes = readJson(path.join(WORKD, "resumed_cues.json"));
  note(R + ".gaps", "the longest stretch without a cue", { plainResumed: maxGap(cs), uninterrupted: base ? maxGap(base) : "n/a", liveResumed: liveRes ? maxGap(liveRes) : "n/a" });
  note(R + ".cues", "cue counts", { plainResumed: cs.length, uninterrupted: base && base.length, liveResumed: liveRes && liveRes.length });
  fs.writeFileSync(path.join(WORKD, "plain_cues.json"), JSON.stringify(cs));
  tool("post-close", MAIN_CLASS, String(pid));
  const exited = await until(() => (a.exited !== null ? true : null), 30000, 200);
  const engGone = await until(() => (!enginePid || !alive(enginePid) ? true : null), 15000, 200);
  log(R + ".5", !!exited && !!engGone, "closing the player ends the app and the engine", { exit: a.exited });
  note(R + ".cpu", "CPU during the run (total, own run included)", mon.stop());
  archiveEngineLog("filmplain");
}

async function runFilm(withModel = true) {
  const R = withModel ? "L2" : "F";
  logPath = path.join(WORKD, withModel ? "L4_film.jsonl" : "L4_filmsmoke.jsonl"); fs.writeFileSync(logPath, "");
  prepProfile();
  fs.rmSync(MEDIA, { recursive: true, force: true }); fs.mkdirSync(MEDIA, { recursive: true });
  const srcClip = path.join(TESTMEDIA, "filemode", "meeting.wav"), clip = path.join(MEDIA, "Meeting.wav");
  fs.copyFileSync(srcClip, clip);                                          // a COPY: the .srt files land next to it, never in the test media
  const clipSec = wavSeconds(clip);
  if (withModel && !(await gate("film"))) { log(R + ".gate", false, "the CPU never got calm within the gate's time: NOT RUN"); return; }
  const mon = startMon();
  const a = launch([]);                                                    // a plain start: the PLAYER shows (second screen)
  const pid = a.pid;
  const player = await connect("index.html");
  log(R + ".1", !!player, "a plain start: the player page is served and loaded");
  if (!player) return;
  await player.hook();
  const pwin = await until(() => winsOf(pid).find((x) => x.cls === MAIN_CLASS && x.visible), 15000);
  log(R + ".2", !!pwin && pwin.x < 0 && pwin.noactivate && tool("fg").pid !== pid, "the PLAYER shows on the second screen, never activated, not the foreground", pwin && { x: pwin.x, noact: pwin.noactivate });
  const eng = await until(() => kidsOf(pid).find((k) => /^SuVtitleEngine\.exe$/i.test(k.exe)), 20000, 300);
  const enginePid = eng && eng.pid;
  log(R + ".3", !!enginePid, "the REAL engine is the app's child");
  // the sound off BEFORE the film loads (the page's own mute, persisted by it): the film plays nothing aloud
  await muteViaStorage(player);
  await player.send("Page.reload", { ignoreCache: true });
  await until(async () => (await player.ev("document.readyState")) === "complete", 15000);
  player.readyMsg = { type: "ready", view: "file" };                      // what a (re)loading player page says: the app then replays its state
  await player.hook(true);
  const muted = await until(async () => (await player.ev("document.getElementById('tMute').classList.contains('is-muted')")) ? true : null, 8000);
  log(R + ".4", !!muted, "the player page is muted (its own mute button state, persisted) before the film is opened");
  // the Live window: opened by the player's own button (openLive{}); the strips injected through ITS page
  await player.ev("document.getElementById('liveBtn').click()");
  const live = await connect("live.html");
  const lwin = await until(() => winsOf(pid).find((x) => x.cls === LIVE_CLASS && x.visible), 15000);
  log(R + ".5", !!live && !!lwin && lwin.x < 0 && lwin.noactivate, "the player's Live button opens the Live window (second screen, never activated)", lwin && { x: lwin.x, noact: lwin.noactivate });
  if (!live) return;
  await live.hook();
  await live.ev("window.chrome.webview.postMessage({ type: 'ready' })");
  await until(async () => { await live.pull(); return ofType(live, "translatorState").pop(); }, 20000, 300);
  const strips = await injectStrips(live, [{ key: KEY_ALICE, label: "Alice" }, { key: KEY_BOB, label: "Bob" }]);
  log(R + ".6", !!strips, "the two wav strips (Alice / Bob) are accepted by the host and shown", strips && strips.map((s) => s.name));
  if (!withModel) {                                                         // the harness without a model: no film opened, no Listen (no CPU gate needed)
    // the hook survives a RELOAD of the page (opening a film from a new folder reloads the player: the folder is mapped before the page navigates)
    const before = player.msgs.length, reh = player.rehooks;
    await player.send("Page.reload", { ignoreCache: true });
    await sleep(2500);
    const got = await until(async () => { await player.pull(); return player.rehooks > reh && player.msgs.length > before ? true : null; }, 20000, 300);
    log(R + ".6b", !!got, "the page hook survives a reload: put back by the next pull, and the app replays its state to it", { rehooks: player.rehooks - reh, newMsgs: player.msgs.length - before, err: player.lastErr });
    tool("post-close", LIVE_CLASS, String(pid));
    await sleep(1500);
    const w = winsOf(pid);
    log(R + ".7", a.exited === null && w.some((x) => x.cls === MAIN_CLASS && x.visible) && w.some((x) => x.cls === LIVE_CLASS && !x.visible), "closing the Live window with the player shown: the app STAYS, the Live window is hidden", w.map((x) => x.cls.replace("SuVtitle.", "") + ":" + x.visible));
    tool("post-close", MAIN_CLASS, String(pid));
    const exited = await until(() => (a.exited !== null ? true : null), 30000, 200);
    const engGone = await until(() => (!enginePid || !alive(enginePid) ? true : null), 15000, 200);
    log(R + ".8", !!exited && !!engGone && !alive(pid), "closing the player: the app exits and the engine goes with it", { exit: a.exited });
    log(R + ".9", [...player.errors, ...live.errors].length === 0, "neither page logged an error", [...player.errors, ...live.errors].slice(0, 4));
    mon.stop(); archiveEngineLog("filmsmoke");
    return;
  }

  // ---- the film: forwarded --play on the running instance; the analysis starts by itself (a cache miss) ----
  const nP0 = player.msgs.length;
  const tPlay = Date.now();
  const fwd = cp.spawnSync(EXE, ["--test-instance", TAG, "--play", clip], { env: testEnv(), timeout: 30000, windowsHide: true });
  log(R + ".7", fwd.status === 0, "--play <the film> is forwarded to the running instance (exit 0)", fwd.status);
  const quiet = () => player.ev("document.querySelectorAll('#fileVideo,#fileAudio').forEach(e => { try { e.pause(); e.muted = true; } catch (x) {} }); 1").catch(() => 0);
  const fileMsgs = () => ofType(player, "translatorFile", nP0).filter((m) => /Meeting\.wav$/i.test(m.d.name || ""));
  const first = await until(async () => { await player.pull(); await quiet(); return fileMsgs().find((m) => m.d.phase === "loading" || m.d.phase === "analyzing"); }, 60000, 300);
  log(R + ".8", !!first, "the film opens and its analysis starts by itself (translatorFile: " + (first ? first.d.phase : "nothing") + ")", first ? { phase: first.d.phase, total: first.d.totalMs } : { msgs: player.msgs.length, rehooks: player.rehooks, err: player.lastErr });
  const prio1 = await until(() => {
    const L = prioLines(tPlay);
    const w = L.find((x) => /whisper-server \(FILE\).*BELOW_NORMAL/.test(x.text)), m = L.find((x) => /llama-server \(MT\).*BELOW_NORMAL/.test(x.text));
    return w && m ? { w, m } : null;
  }, 180000, 500);
  log(R + ".9", !!prio1, "engine.log: the FILE job's whisper AND the MT run BELOW_NORMAL (priority: lines)", prio1 && [prio1.w.text, prio1.m.text]);
  const servers = (pidOf) => (pidOf ? kidsOf(pidOf) : []).filter((k) => /whisper|llama/i.test(k.exe));     // the model servers (the engine's other children, if any, are not the question)
  const kidsFile = servers(enginePid);
  const pr = prioOf(kidsFile.map((k) => k.pid));
  const prFile = kidsFile.map((k) => k.exe + "=" + pr[k.pid]);
  log(R + ".10", kidsFile.length >= 2 && kidsFile.every((k) => pr[k.pid] === "BelowNormal"), "...and the processes REALLY are BelowNormal (Get-Process) while the job runs", prFile);
  const minDone = Math.min(+(process.env.SUV_L4_MIN_DONE_MS || 30000), Math.round(0.3 * ((first && first.d.totalMs) || clipSec * 1000)));   // the Listen comes when this much is analysed
  const firstRange = await until(async () => {
    await player.pull(); await quiet();
    return fileMsgs().filter((m) => m.d.phase === "analyzing" && !m.d.complete && rangesTotal(m.d.ranges || []) >= minDone).pop();
  }, 240000, 250);
  const rg1 = firstRange && firstRange.d;
  const done1 = rg1 ? rangesTotal(rg1.ranges) : 0;
  note(R + ".t", "the analysis reached " + Math.round(minDone / 1000) + " s of the film", firstRange ? fmtS(firstRange.t - tPlay) + " after --play" : "NEVER");
  log(R + ".11", !!rg1 && done1 < rg1.totalMs, "the job is MID-WAY (ranges out, the film not done) when the Listen comes: " + (rg1 ? (done1 / 1000).toFixed(1) + " s of " + Math.round(rg1.totalMs / 1000) + " s, " + rg1.count + " cue(s)" : "no range"),
    rg1 && { ranges: rg1.ranges, count: rg1.count, totalMs: rg1.totalMs });
  if (!rg1 || done1 >= rg1.totalMs) { saveTrace(player, "film_player"); archiveEngineLog("film"); return; }
  await player.shot("L2_analysing");

  // ---- LISTEN while the job analyses ----
  await live.pull(); await player.pull();
  const nP1 = player.msgs.length, nL1 = live.msgs.length;
  const preAsr2 = ((ofType(live, "translatorState").pop() || { d: {} }).d.models || {}).asr || "";     // while the job runs STATE names the JOB's models
  const tListen = Date.now();
  await clickListen(live);
  const phaseLive = await until(async () => { await player.pull(); return ofType(player, "translatorFile", nP1).find((m) => m.d.phase === "live"); }, 30000, 100);
  log(R + ".12", !!phaseLive, "the player says phase \"live\" (" + (phaseLive ? fmtS(phaseLive.t - tListen) + " after the click" : "never") + "): a notice, not an error", phaseLive && { err: phaseLive.d.err, count: phaseLive.d.count, ranges: phaseLive.d.ranges });
  const notice = await until(async () => { const n = await player.ev(Q.notice()); return !n.hidden ? n : null; }, 15000, 200);
  log(R + ".13", !!notice && /Live translation is on/.test(notice.text), "the player shows the live notice at once (the app's gate: the film waits, nothing is sent for it)", notice);
  const cancelled = await until(() => elogLines().find((l) => l.t >= tListen && /FILE job \d+ cancelled/.test(l.text)), 90000, 300);
  log(R + ".14", !!cancelled, "engine.log: the job ends `cancelled` (" + (cancelled ? fmtS(cancelled.t - tListen) + " after the click" : "never") + ")", cancelled && cancelled.text);
  const liveStarted = await until(() => elogLines().find((l) => l.t >= tListen && /live: started -- 2 input\(s\)/.test(l.text)), 60000, 300);
  log(R + ".15", !!liveStarted, "engine.log: \"live: started -- 2 input(s)\"" + (liveStarted && cancelled ? " (" + fmtS(liveStarted.t - cancelled.t) + " after the cancel)" : ""), liveStarted && scrub(liveStarted.text));
  const litL = await until(async () => { const l = await live.ev(Q.listen()); return l.lit && l.pressed === "true" ? l : null; }, 120000, 200);
  log(R + ".16", !!litL, "the Live window's on-air lamp is lit (the engine's echo, " + (litL ? fmtS(Date.now() - tListen) : "never") + " after the click -- the job had to give way first)");
  const onAirBtn = await until(async () => { const n = await player.ev(Q.notice()); return n.onAir ? n : null; }, 15000, 200);
  log(R + ".16b", !!onAirBtn, "...and with the engine's echo in, the player's Live button is ON AIR too", onAirBtn);
  const sLis = await until(async () => { await live.pull(); return ofType(live, "translatorState", nL1).find((m) => m.d.live && m.d.live.on && m.d.status === "listening" && m.d.models && m.d.models.asr && m.d.models.asr !== preAsr2); }, 180000, 300);
  note(R + ".t", "STATE listening (the live pair's models) after the Listen click", sLis ? fmtS(sLis.t - tListen) : "NEVER");
  log(R + ".17", !!sLis && /^(ggml-)?small(\.bin)?$/.test(sLis.d.models.asr) && /Qwen3-4B/.test(sLis.d.models.mt), "STATE names the LIVE pair's models (balanced = small + the 4B), not the film's (" + preAsr2 + ")", sLis && sLis.d.models);
  const prio2 = await until(() => prioLines(tListen).find((x) => /llama-server \(MT\).*-> NORMAL/.test(x.text)), 60000, 500);
  log(R + ".18", !!prio2, "engine.log: the kept MT goes back to NORMAL at the live start (priority: line)", prio2 && prio2.text);
  // ---- the live lines
  let finals = [];
  const tL = Date.now();
  for (;;) {
    await live.pull();
    finals = ofType(live, "translatorLine", nL1);
    if (finals.length >= 3 || Date.now() - tL > 240000) break;
    await sleep(500);
  }
  note(R + ".t", "first live LINE after the Listen click (job running before it)", finals.length ? fmtS(finals[0].t - tListen) : "NEVER");
  const verd = finals.map(laneVerdict);
  log(R + ".19", finals.length >= 3 && verd.every((v) => v.labelOk && v.laneOk && v.dstOk && v.langOk && v.dstLangOk), "live lines flow (" + finals.length + ") with the right label / lane / translation while the film waits", verd.filter((v) => !(v.labelOk && v.laneOk && v.dstOk)).slice(0, 3));
  const kidsLive = servers(enginePid);
  const prL = prioOf(kidsLive.map((k) => k.pid));
  const prLive = kidsLive.map((k) => k.exe + "=" + prL[k.pid]);
  log(R + ".20", kidsLive.length >= 2 && kidsLive.every((k) => prL[k.pid] === "Normal"), "...and the processes REALLY are Normal during the live (Get-Process)", prLive);
  const pFile = player.msgs.slice(nP1).filter((m) => m.d && m.d.type === "translatorFile" && /Meeting\.wav$/i.test(m.d.name || ""));
  log(R + ".21", !pFile.some((m) => m.d.phase === "error" || m.d.err), "the cancelled job was never marked failed / errored on the player during the live", pFile.map((m) => m.d.phase + (m.d.err ? ":" + m.d.err : "")).filter((v, i, ar) => ar.indexOf(v) === i));
  await live.shot("L2_live_lines");
  const rec1 = findRecord("Meeting.wav");
  const r1 = rec1 ? parseRecord(rec1.text) : null;
  note(R + ".rec1", "the record at the cancel", r1 && { rng: r1.rng, done: r1.done, cues: r1.cues.length });

  // ---- Listen OFF: the job resumes ----
  await player.pull(); await live.pull();
  const nP2 = player.msgs.length, nL2 = live.msgs.length;
  // the baseline of the cancelled job = the LAST thing the player knew before the resume (the job may finish one more window after the click)
  const live1 = player.msgs.slice(nP1, nP2).filter((m) => m.d && m.d.type === "translatorFile" && /Meeting\.wav$/i.test(m.d.name || "")).pop();
  const base1 = (live1 ? live1.d : phaseLive ? phaseLive.d : rg1);
  const ranges1 = base1.ranges || [], count1 = base1.count;
  note(R + ".base", "what the player holds when the live ends (the cancelled job's result)", { phase: base1.phase, ranges: ranges1, cues: count1, doneMs: base1.doneMs, totalMs: base1.totalMs });
  const tOff = Date.now();
  await clickListen(live);
  // (post-fix verification, 26/10: the drain -- see R-L1's .19. STATE live=0 comes after the queued finals are done: waited for up to 180 s, then the lamp is judged)
  const stOff = await until(async () => { await live.pull(); return ofType(live, "translatorState", nL2).find((m) => m.d.live && m.d.live.on === false); }, 180000, 200);
  const dark = await until(async () => { const r = await live.ev(Q.finishing()); return !r.lit && !r.fin && r.pressed === "false" && !/^Finishing/.test(r.st) ? r : null; }, 15000, 150);
  log(R + ".22", !!dark && !!stOff, "Listen off: STATE live=0 once the drain is done (" + (stOff ? fmtS(stOff.t - tOff) : "never") + " after the click), the lamp dark then");
  const resumed = await until(async () => { await player.pull(); return ofType(player, "translatorFile", nP2).find((m) => (m.d.phase === "loading" || m.d.phase === "analyzing") && /Meeting\.wav$/i.test(m.d.name || "")); }, 90000, 200);
  log(R + ".23", !!resumed, "the film's job RESUMES by itself (" + (resumed ? "phase " + resumed.d.phase + ", " + fmtS(resumed.t - tOff) + " after the click" : "never") + ")", resumed && { phase: resumed.d.phase, ranges: resumed.d.ranges, count: resumed.d.count });
  const noticeGone = await until(async () => { const n = await player.ev(Q.notice()); return n.hidden && !n.onAir ? n : null; }, 15000, 200);
  log(R + ".24", !!noticeGone, "the player's live notice goes away with the listening");
  const prio3 = await until(() => {
    const L = prioLines(tOff);
    const w = L.find((x) => /whisper-server \(FILE\).*BELOW_NORMAL/.test(x.text)), m = L.find((x) => /llama-server \(MT\).*BELOW_NORMAL/.test(x.text));
    return w && m ? { w, m } : null;
  }, 120000, 500);
  log(R + ".25", !!prio3, "the resumed job's whisper and the MT run BELOW_NORMAL again (priority: lines)", prio3 && [prio3.w.text, prio3.m.text]);
  const done = await until(async () => { await player.pull(); await quiet(); return ofType(player, "translatorFile", nP2).find((m) => m.d.complete && /Meeting\.wav$/i.test(m.d.name || "")); }, 420000, 400);
  const tDone = done ? done.t : 0;
  log(R + ".26", !!done, "the film's analysis COMPLETES (" + (done ? fmtS(done.t - tOff) + " after the Listen off" : "never") + ")", done && { phase: done.d.phase, count: done.d.count, err: done.d.err });
  await player.shot("L2_done");
  // monotonic ranges, cues only grow
  const seq = ofType(player, "translatorFile", nP1).filter((m) => /Meeting\.wav$/i.test(m.d.name || ""));
  let mono = true, prevTot = 0, prevCnt = 0;
  for (const m of seq) { const tot = rangesTotal(m.d.ranges || []); if (m.d.complete) continue; if (tot < prevTot - 1 || (m.d.count || 0) < prevCnt) mono = false; prevTot = Math.max(prevTot, tot); prevCnt = Math.max(prevCnt, m.d.count || 0); }
  log(R + ".27", mono, "across the live and the resume the analysed ranges and the cue count only GROW (never shrink, never reset)", { messages: seq.length });
  const jobLines = elogLines().filter((l) => l.t >= tPlay - 1000 && /FILE job \d+ (ok|cancelled|error)/.test(l.text)).map((l) => {
    const m = /FILE job (\d+) (\w+): (\d+) cue\(s\), (\d+) window\(s\).*, ([\d.]+) s of audio/.exec(l.text);
    return m ? { job: +m[1], result: m[2], cues: +m[3], windows: +m[4], sec: +m[5], text: l.text } : { text: l.text };
  });
  note(R + ".jobs", "engine.log FILE job lines", jobLines.map((j) => j.text));
  const jc = jobLines.filter((j) => j.result === "cancelled"), jo = jobLines.filter((j) => j.result === "ok");
  const totalSec = done ? done.d.totalMs / 1000 : clipSec;
  const sumSec = jobLines.reduce((s, j) => s + (j.sec || 0), 0);
  log(R + ".28", jc.length === 1 && jo.length === 1 && jobLines.length === 2, "exactly two FILE jobs: one `cancelled`, then one `ok`", jobLines.map((j) => j.job + ":" + j.result));
  log(R + ".29", jc.length === 1 && jo.length === 1 && Math.abs(sumSec - totalSec) <= 1.0 && jo[0].sec <= totalSec - jc[0].sec + 0.6,
    "NO RANGE ANALYSED TWICE: the two jobs' own audio (" + jc.map((j) => j.sec).join(" + ") + " + " + jo.map((j) => j.sec).join(" ") + " = " + sumSec.toFixed(1) + " s) equals the film (" + totalSec.toFixed(1) + " s)", { sumSec, totalSec });
  // the record + the cues
  await sleep(1500);
  const rec2 = findRecord("Meeting.wav");
  const r2 = rec2 ? parseRecord(rec2.text) : null;
  const rg2 = r2 ? parseRanges(r2.rng) : [];
  log(R + ".30", !!r2 && r2.done === "1" && rg2.length === 1 && rg2[0][0] === 0 && !!done && Math.abs(rg2[0][1] - done.d.totalMs) <= 2, "the record: DONE 1 and ONE range covering the whole film", r2 && { rng: r2.rng, done: r2.done, total: done && done.d.totalMs });
  if (r2) {
    const cs = r2.cues.slice().sort((x, y) => x.s - y.s);
    let overlap = 0, dup = 0;
    for (let i = 1; i < cs.length; i++) { if (cs[i].s < cs[i - 1].e - 1) overlap++; if (cs[i].s === cs[i - 1].s && cs[i].src === cs[i - 1].src) dup++; }
    const inFirst = cs.filter((c) => ranges1.some((r) => c.s >= r[0] && c.s < r[1])).length;
    log(R + ".31", cs.length > 0 && overlap === 0 && dup === 0 && inFirst === count1 && cs.length >= count1 && done && cs.length === done.d.count,
      "the cues CONTINUE: " + cs.length + " cues, none overlapping or twice, the " + count1 + " made before the cancel are all there (" + inFirst + " start inside the first ranges), the page and the record agree",
      { total: cs.length, overlap, dup, inFirst, count1, pageCount: done && done.d.count, ranges1 });
    note(R + ".cues", "first cues (start ms / src)", cs.slice(0, 3).map((c) => c.s + " " + String(c.src).slice(0, 40)));
    fs.writeFileSync(path.join(WORKD, "resumed_cues.json"), JSON.stringify(cs));
    if (BASE_CUES) {
      const bag = (cues) => { const m = new Map(); for (const c of cues) for (const w of String(c.src).toLowerCase().replace(/[^a-z0-9' ]+/g, " ").split(/\s+/).filter(Boolean)) m.set(w, (m.get(w) || 0) + 1); return m; };
      const A = bag(BASE_CUES), B = bag(cs);
      let total = 0, missing = 0, extra = 0; const lost = [], added = [];
      for (const [w, n] of A) { total += n; const k = B.get(w) || 0; if (n > k) { missing += n - k; lost.push(w + (n - k > 1 ? "x" + (n - k) : "")); } }
      for (const [w, n] of B) { const k = A.get(w) || 0; if (n > k) { extra += n - k; added.push(w + (n - k > 1 ? "x" + (n - k) : "")); } }
      note(R + ".vsbase", "words of the UNINTERRUPTED analysis missing from / added to the resumed one", { baseWords: total, missing, extra, lost, added, baseCues: BASE_CUES.length, resumedCues: cs.length });
      const maxGap = (cues) => { const c = cues.slice().sort((x, y) => x.s - y.s); let g = 0, at = 0; for (let i = 1; i < c.length; i++) if (c[i].s - c[i - 1].e > g) { g = c[i].s - c[i - 1].e; at = c[i - 1].e; } return (g / 1000).toFixed(1) + " s after " + (at / 1000).toFixed(1) + " s"; };
      note(R + ".gaps", "the longest stretch without a cue: uninterrupted / resumed (a weak test model -- whisper base on the CPU -- loses a window now and then, either way)", { uninterrupted: maxGap(BASE_CUES), resumed: maxGap(cs) });
      const join = ranges1.length ? ranges1[ranges1.length - 1][1] : 0;
      const wordsOf = (cues) => cues.flatMap((c) => String(c.src).toLowerCase().replace(/[^a-z0-9' ]+/g, " ").split(/\s+/).filter(Boolean));
      const around = (cues) => cues.filter((c) => c.e > join - 5000 && c.s < join + 5000);
      const wa = wordsOf(around(BASE_CUES)), wb = new Map();
      for (const w of wordsOf(around(cs))) wb.set(w, (wb.get(w) || 0) + 1);
      let common = 0; for (const w of wa) { const k = wb.get(w) || 0; if (k > 0) { common++; wb.set(w, k - 1); } }
      log(R + ".31b", wa.length > 0 && common / wa.length >= 0.85, "the JOIN is clean: the cues within 5 s of the cut at " + (join / 1000).toFixed(2) + " s say what the uninterrupted analysis says there (" + common + " of " + wa.length + " words)", { resumed: around(cs).map((c) => c.s + " " + String(c.src).slice(0, 50)), uninterrupted: around(BASE_CUES).map((c) => c.s + " " + String(c.src).slice(0, 50)) });
      const near = (cues) => cues.slice().sort((x, y) => x.s - y.s).filter((c) => c.e > join - 4000 && c.s < join + 4000).map((c) => c.s + "-" + c.e + " " + String(c.src).slice(0, 70));
      note(R + ".join", "the cues around the join at " + (join / 1000).toFixed(2) + " s: resumed / uninterrupted", { resumed: near(cs), uninterrupted: near(BASE_CUES) });
    }
  }
  const srts = fs.readdirSync(MEDIA).filter((f) => /\.srt$/i.test(f));
  const srtCues = srts.map((f) => (readText(path.join(MEDIA, f)).match(/-->/g) || []).length);
  log(R + ".32", srts.length >= 2 && srtCues.every((n) => n > 0), "one .srt per language next to the film (a COPY of it), each with cues", srts.map((f, i) => f + ":" + srtCues[i]));

  // ---- R-L3: Listen again, then close the Live window while listening ----
  const nL3 = live.msgs.length;
  const tL3 = Date.now();
  await clickListen(live);
  const lit3 = await until(async () => { const l = await live.ev(Q.listen()); return l.lit && l.pressed === "true" ? l : null; }, 120000, 200);
  const line3 = await until(async () => { await live.pull(); return ofType(live, "translatorLine", nL3)[0]; }, 180000, 300);
  note(R + ".t", "second Listen: first LINE", line3 ? fmtS(line3.t - tL3) : "NEVER");
  const part3 = await until(async () => { await live.pull(); return ofType(live, "translatorPartial", nL3).find((m) => m.t > (line3 ? line3.t : 0)); }, 2500, 200);   // (a busy CPU never gets to draft: the pipeline is mid-utterance anyway)
  const jrDir = fs.existsSync(JOURNALS) ? fs.readdirSync(JOURNALS).filter((f) => f.endsWith(".jsonl")).map((f) => path.join(JOURNALS, f)) : [];
  const sizeJ = () => jrDir.reduce((s, f) => s + (fs.existsSync(f) ? fs.statSync(f).size : 0), 0);
  const linesBeforeClose = ofType(live, "translatorLine", nL3).length;
  const kidsBeforeQuit = enginePid ? kidsOf(enginePid) : [];
  const tClose = Date.now();
  const closeRes = tool("post-close", LIVE_CLASS, String(pid));
  const stopLog = await until(() => elogLines().find((l) => l.t >= tClose - 500 && /live: stopped/.test(l.text)), 180000, 200);   // (26/10: "live: stopped" now closes the DRAIN -- the CPU backlog took 21.7 s here; 180 s = [C8b]'s bound)
  log(R + ".33", !!lit3 && !!line3 && closeRes.ok === true && !!stopLog, "closing the Live window WHILE LISTENING (a line out" + (part3 ? ", a partial of the next one in flight" : "") + "): LIVE 0 reaches the engine (\"live: stopped\" " + (stopLog ? fmtS(stopLog.t - tClose) : "never") + " after the close)", { lit3: !!lit3, line3: !!line3, part3: !!part3 });
  await sleep(1500);
  const jSize0 = sizeJ();
  await sleep(9000);
  await live.pull().catch(() => 0);
  const ack = stopLog ? stopLog.t : tClose;                                  // the engine's acknowledgement of LIVE 0
  const afterAck = live.msgs.filter((m) => m.t > ack + 300 && m.d && (m.d.type === "translatorLine" || m.d.type === "translatorPartial"));
  const inFlight = live.msgs.filter((m) => m.t > tClose && m.t <= ack + 300 && m.d && m.d.type === "translatorLine");
  note(R + ".inflight", "LINEs delivered between the close of the window and the engine's acknowledgement (the utterance in flight when LIVE 0 arrived)", inFlight.map((m) => fmtS(m.t - tClose) + " after the close, " + ((ack - m.t) / 1000).toFixed(2) + " s before the ack"));
  log(R + ".34", afterAck.length === 0 && sizeJ() === jSize0, "NO LINE / PART after the engine acknowledged LIVE 0 (9 s watched): none reaches the page, the session journal stops growing", { afterAck: afterAck.length, journalGrew: sizeJ() - jSize0 });
  const w = winsOf(pid);
  log(R + ".35", a.exited === null && w.some((x) => x.cls === MAIN_CLASS && x.visible) && w.some((x) => x.cls === LIVE_CLASS && !x.visible), "the app STAYS: the player is shown, the Live window hidden (not destroyed)", w.map((x) => x.cls.replace("SuVtitle.", "") + ":" + x.visible));
  const n3 = await player.ev(Q.notice()).catch(() => null);
  log(R + ".36", !!n3 && n3.hidden && !n3.onAir, "the player's live notice is gone (nothing listens)", n3);
  const kidsStopped = enginePid ? kidsOf(enginePid) : [];
  note(R + ".kids", "engine children: while listening / after the stop", { listening: kidsBeforeQuit.map((k) => k.exe), stopped: kidsStopped.map((k) => k.exe) });
  log(R + ".37", !kidsStopped.some((k) => /whisper/i.test(k.exe)), "the live whisper children are gone after the stop", kidsStopped.map((k) => k.exe));
  saveTrace(live, "film_live"); saveTrace(player, "film_player");

  // ---- close the player: the app exits, the engine and its children with it ----
  const family = [pid, ...(enginePid ? [enginePid] : []), ...kidsStopped.map((k) => k.pid), ...kidsBeforeQuit.map((k) => k.pid)];
  const tQuit = Date.now();
  tool("post-close", MAIN_CLASS, String(pid));
  const exited = await until(() => (a.exited !== null ? true : null), 45000, 200);
  const allDead = await until(() => (family.every((p) => !alive(p)) ? true : null), 30000, 300);
  log(R + ".38", !!exited && !!allDead, "closing the PLAYER (the Live window hidden): the app exits (" + (exited ? fmtS(Date.now() - tQuit) : "never") + ") and the engine and every child are gone", { exit: a.exited, alive: family.filter((p) => alive(p)) });
  const errs = [...player.errors, ...live.errors];
  log(R + ".39", errs.length === 0, "neither page raised an exception / console error / CSP violation", errs.slice(0, 5));
  const cs = mon.stop();
  note(R + ".cpu", "CPU during the run (total, own run included)", cs);
  archiveEngineLog("film");
}

// ============================================================================================================================
async function main() {
  for (const d of [WORKD, SHOTS, LOGS, BIN, MEDIA]) fs.mkdirSync(d, { recursive: true });
  if (!PAYLOAD || !fs.existsSync(PAYLOAD) || !TESTMEDIA || !fs.existsSync(TESTMEDIA)) { console.log("live_real_e2e: NOT RUN -- set SUVTITLE_PAYLOAD and SUVTITLE_TEST_MEDIA (LIVE_PLAN section 6)"); process.exit(2); }
  if (!fs.existsSync(TOOLS)) { console.log("live_real_e2e: NOT RUN -- " + TOOLS + " is not built (run_live_real_e2e.cmd builds it)"); process.exit(2); }
  laneKeys();
  logPath = path.join(WORKD, "L4_pre.jsonl"); fs.writeFileSync(logPath, "");
  log("A0", under(PROF, L4) && path.resolve(ROAM).toLowerCase() !== String(REAL_APPDATA).toLowerCase() && path.resolve(LOCAL).toLowerCase() !== String(REAL_LOCAL).toLowerCase()
    && EN_SEC > 10 && ZH_SEC > 10, "the isolated profile is under L4 (not the user's); the two benches are read (" + EN_SEC.toFixed(2) + " s and " + ZH_SEC.toFixed(2) + " s)");
  const realSnap = () => [path.join(REAL_APPDATA, "SuVtitle"), path.join(REAL_LOCAL, "SuVtitle")].map((p) => { try { return fs.readdirSync(p).sort().join(","); } catch { return "absent"; } });
  const realBefore = realSnap();
  // a stale instance of OUR tag (an earlier aborted run) would swallow this run's launch: only suvl4 is ours
  const stale = psJson("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'SuVtitle.exe' -and $_.CommandLine -match '--test-instance suvl4' } | Select-Object ProcessId | ConvertTo-Json -Compress");
  for (const s of stale) cp.spawnSync("taskkill", ["/PID", String(s.ProcessId), "/T", "/F"], { windowsHide: true });
  if (stale.length) note("A0", "killed a stale suvl4 instance of an earlier aborted run", stale.map((s) => s.ProcessId));
  if (MODE === "smoke") await runLive(false);
  if (MODE === "filmsmoke") await runFilm(false);
  if ((MODE === "film" || MODE === "all") && process.env.SUV_L4_NOBASE !== "1") await runFilmBase();
  if (MODE === "filmbase") await runFilmBase();
  if (MODE === "filmplain") await runFilmPlain();
  if (MODE === "film" || MODE === "all") await runFilm(true);                // the long run first (the gate is the scarce thing)
  if (MODE === "live" || MODE === "all") await runLive(true);
  logPath = path.join(WORKD, "L4_post.jsonl"); fs.writeFileSync(logPath, "");
  const realAfter = realSnap();
  log("Z1", JSON.stringify(realBefore) === JSON.stringify(realAfter), "the user's %APPDATA%\\SuVtitle and %LOCALAPPDATA%\\SuVtitle hold exactly the same file NAMES as before", { realBefore, realAfter });
}

process.on("exit", () => { for (const c of launched) if (c.exited === null && c.pid) cp.spawnSync("taskkill", ["/PID", String(c.pid), "/T", "/F"], { windowsHide: true }); });
main().catch((e) => { log("FATAL", false, String(e && e.stack || e)); }).finally(() => {
  const bad = results.filter((r) => !r.ok);
  console.log("\nlive_real_e2e (" + MODE + "): " + results.length + " checks, " + bad.length + " failed" + (bad.length ? ": " + bad.map((r) => r.step).join(", ") : ""));
  process.exit(bad.length ? 1 : 0);
});

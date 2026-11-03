// live_e2e.js -- the REAL SuVtitle.exe (Debug build) run as a TEST INSTANCE with --live, against the FAKE engine's `live` mode
// (tests\cpp\pipe\fake_engine.cpp, built as SuVtitleEngine.exe next to the exe copy), driven through CDP like the user drives it.
// 2026-10-25, L2 step 8 (test agent). Same discipline as real_e2e.js: --test-instance suvl2 (its own mutex / window classes /
// registry root), an ISOLATED profile (APPDATA / LOCALAPPDATA -> <SUV_L2>\profile: the product refuses a test instance on the
// real one), windows on the SECOND screen (app.txt: LIVEPOS / WINPOS at x < 0), never activated (WS_EX_NOACTIVATE), the Save
// dialog of the Export driven by MESSAGES (real_tools savedlg: no focus), CDP port 9381 (WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
// overrides the product's own switches: it repeats all three). Kills only the processes it started (by pid).
//
//   SUV_L2=<folder> node tests\e2e\real\live_e2e.js        <SUV_L2>\bin must hold: SuVtitleEngine.exe (the fake, built from
//                                                          tests\cpp\pipe\fake_engine.cpp) and real_tools.exe (tests\e2e\real\
//                                                          real_tools.cpp); SuVtitle.exe, WebView2Loader.dll and web\ are copied
//                                                          from build\x64-Debug on every run. Results: <SUV_L2>\work\L.jsonl,
//                                                          screenshots <SUV_L2>\shots\*.png, exports <SUV_L2>\export\.
// The Export (D1-D7, F5) is answered by the TEST INSTANCE's hook SUVTITLE_TEST_EXPORT (fix round 26/10, R5: honoured only by a test
// instance, never read by the product) -- so no run raises an OS window. SUV_E2E_EXPORT=1 runs the real OS Save dialog instead (driven
// by messages; Windows may bring it to the foreground when the user has been idle). Everything else never raises an OS window.
// FIX ROUND (26/10, docs\REVIEW_RV-LB.md + LIVE_PLAN.md "ADDENDUM FIX ROUND"): C20-C22 the drain (Finishing..., the lines still come,
// a press resumes), E1-E2b the last window closed while listening (hidden at once, the drain waited for, then QUIT; the lines journaled),
// E4b / F4-F5 the session survives the restart (replay + the whole export), F6 a refused LIVE, G8 the browser crash with the Live window
// MINIMIZED (left minimized, no foreground), H10 a relaunch during the drain's wait, H11 the 15 s bound. The fake engine drains
// (SUV_FAKE_DRAIN_LINES / SUV_FAKE_DRAIN_MS) and refuses a strip labelled "refuse".
// Exit 0 = every check passed.
"use strict";
const fs = require("fs"), path = require("path"), cp = require("child_process"), zlib = require("zlib");

const REPO = path.resolve(__dirname, "..", "..", "..");
const L2 = path.resolve(process.env.SUV_L2 || path.join(REPO, "scratchpad", "l2"));
const BIN = path.join(L2, "bin"), BUILD = path.join(REPO, "build", "x64-Debug");
const EXE = path.join(BIN, "SuVtitle.exe"), TOOLS = path.join(BIN, "real_tools.exe"), FAKELOG = path.join(BIN, "fake_engine.log");
const PROF = path.join(L2, "profile"), ROAM = path.join(PROF, "Roaming"), LOCAL = path.join(PROF, "Local");
const SUV_ROAM = path.join(ROAM, "SuVtitle"), APPTXT = path.join(SUV_ROAM, "app.txt"), JOURNALS = path.join(SUV_ROAM, "Live sessions");
const SHOTS = path.join(L2, "shots"), WORKD = path.join(L2, "work"), OUT = path.join(L2, "export");
const TAG = "suvl2", PORT = 9381;
const LIVE_CLASS = "SuVtitle.LiveWindow." + TAG, MAIN_CLASS = "SuVtitle.MainWindow." + TAG;
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays --enable-blink-features=AudioVideoTracks";
const REAL_APPDATA = process.env.APPDATA, REAL_LOCAL = process.env.LOCALAPPDATA;

const results = [];
const logPath = path.join(WORKD, "L.jsonl");
function log(step, ok, what, detail) {
  results.push({ step, ok: !!ok });
  fs.appendFileSync(logPath, JSON.stringify({ step, ok: !!ok, what, detail: detail === undefined ? null : detail }) + "\n");
  console.log((ok ? "  ok    " : "  FAIL  ") + step + "  " + what + (ok || detail === undefined ? "" : "  -> " + JSON.stringify(detail).slice(0, 500)));
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const under = (p, root) => path.resolve(p).toLowerCase().startsWith(path.resolve(root).toLowerCase() + path.sep);
async function until(fn, ms, step = 100) {                       // the value fn() returns when truthy, else null after ms
  const end = Date.now() + ms;
  for (;;) {
    let v = null;
    try { v = await fn(); } catch { v = null; }
    if (v) return v;
    if (Date.now() > end) return null;
    await sleep(step);
  }
}

// ---- the isolated environment ----------------------------------------------------------------------------------------------
function testEnv(extra) {
  const e = {};
  for (const k of Object.keys(process.env)) {
    const u = k.toUpperCase();
    if (u === "APPDATA" || u === "LOCALAPPDATA" || u === "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS" || u === "SUVTITLE_TEST_EXPORT"
        || u === "SUV_FAKE_DRAIN_LINES" || u === "SUV_FAKE_DRAIN_MS") continue;
    e[k] = process.env[k];
  }
  e.APPDATA = ROAM; e.LOCALAPPDATA = LOCAL;
  e.WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=" + PORT + " " + SWITCHES;
  e.SUV_FAKE_MODE = "live";                                       // the fake engine's mode (it is spawned with no --fake)
  // fix round (R5): the Export answered WITHOUT the OS Save dialog -- a test instance only (the product never reads it) --
  // unless SUV_E2E_EXPORT=1 asks for the real dialog (driven by messages, D1-D7); the fake's drain length per instance
  return Object.assign(e, extra || {});
}
function tool(...args) {
  const r = cp.spawnSync(TOOLS, args, { encoding: "utf8", timeout: 30000, windowsHide: true });
  try { return JSON.parse((r.stdout || "").trim().split("\n").pop()); } catch { return { ok: false, err: "no JSON", raw: r.stdout, code: r.status }; }
}
const winsOf = (pid) => (tool("wins", String(pid)).wins || []);
const alive = (pid) => !!tool("alive", String(pid)).alive;
const launched = [];                                             // every instance this run started: killed (by pid) at the end if still up
function launch(args, extraEnv) {
  const child = cp.spawn(EXE, ["--test-instance", TAG, ...args], { env: testEnv(extraEnv), stdio: "ignore", windowsHide: false, detached: false });
  launched.push(child);
  child.exited = null;
  child.on("exit", (code) => { child.exited = code; });
  return child;
}
// (fix round: retried -- a read that meets the fake engine's append or the antivirus's scan failed ONCE in run 1 of 26/10 and
// made a whole log look empty; a file that does not exist is "" at once)
function readText(p) {
  for (let i = 0; i < 6; i++) {
    try { return fs.readFileSync(p, "utf8"); }
    catch (e) { if (e && e.code === "ENOENT") return ""; Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 25); }
  }
  return "";
}
const fakeLines = () => readText(FAKELOG).split("\n").filter(Boolean);

// ---- CDP (Node's own WebSocket) --------------------------------------------------------------------------------------------
async function targets() {
  const r = await fetch("http://127.0.0.1:" + PORT + "/json");
  return r.json();
}
async function connect(urlPart) {
  const t = await until(async () => (await targets()).find((x) => x.type === "page" && x.url.includes(urlPart)), 20000, 250);
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
    setTimeout(() => { if (pending.has(n)) { pending.delete(n); rej(new Error("timeout " + method)); } }, 20000);
  });
  const page = {
    send, on: (f) => listeners.push(f), close: () => { try { ws.close(); } catch { /* gone */ } },
    async ev(expr) {
      const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error("page threw: " + JSON.stringify(r.exceptionDetails).slice(0, 300));
      return r.result.value;
    },
    async shot(name) {
      const r = await send("Page.captureScreenshot", { format: "png" });
      fs.writeFileSync(path.join(SHOTS, name + ".png"), Buffer.from(r.data, "base64"));
    },
    async key(k, vk) {
      await send("Input.dispatchKeyEvent", { type: "keyDown", key: k, code: k, windowsVirtualKeyCode: vk });
      await send("Input.dispatchKeyEvent", { type: "keyUp", key: k, code: k, windowsVirtualKeyCode: vk });
    },
  };
  // what the page complains about: an uncaught exception, a console error / warning, a log entry (a CSP violation arrives so)
  page.errors = [];
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

// ---- the page's own readings -----------------------------------------------------------------------------------------------
const Q = {
  state: () => "document.getElementById('state').textContent",
  strips: () => `Array.from(document.querySelectorAll('#strips .strip')).map(s => ({ name: s.querySelector('.strip-name').value, src: s.querySelector('.strip-src-name').textContent,
      kind: s.querySelector('.strip-kind').textContent, chan: s.dataset.chan, key: s.dataset.key, lamp: s.querySelector('.strip-lamp').className, why: s.querySelector('.strip-why').hidden ? '' : s.querySelector('.strip-why').textContent,
      db: s.querySelector('.strip-db').textContent, ch: s.querySelector('.strip-ch').value, lvl: s.querySelector('.meter').style.getPropertyValue('--lvl'), mt: s.querySelector('.meter').title }))`,
  lines: () => `Array.from(document.querySelectorAll('#log .tr-para')).map(p => ({ who: (p.querySelector('.tr-who-name')||{}).textContent || '', key: p.dataset.spk || '', style: p.style.getPropertyValue('--spk'), n: p.querySelectorAll('.tr-line').length }))`,
  listen: () => `({ pressed: document.getElementById('listenBtn').getAttribute('aria-pressed'), lit: document.getElementById('onAir').classList.contains('is-lit'), pending: document.getElementById('listenBtn').classList.contains('is-pending') })`,
  pickerRows: () => `Array.from(document.querySelectorAll('#pickerList .pk-sec')).map(s => ({ title: s.querySelector('.pk-sec-title').textContent, rows: Array.from(s.querySelectorAll('.pk-row')).map(r => ({ name: (r.querySelector('.pk-name')||{}).textContent, off: r.classList.contains('is-off'), why: (r.querySelector('.pk-why')||{}).textContent || '', badges: Array.from(r.querySelectorAll('.pk-badge')).map(b => b.textContent), chips: Array.from(r.querySelectorAll('.pk-ch')).map(b => b.textContent + (b.disabled ? '(x)' : '')) })) }))`,
};
async function clickPickerRow(page, name, chip) {              // chip: "Mix" | "1".. = a channel chip of that row; undefined = the row's own button
  return page.ev(`(() => { const r = Array.from(document.querySelectorAll('#pickerList .pk-row')).find(x => (x.querySelector('.pk-name')||{}).textContent === ${JSON.stringify(name)});
    if (!r) return 'no row'; const b = ${chip === undefined ? "r.querySelector('.pk-add')" : `Array.from(r.querySelectorAll('.pk-ch')).find(c => c.textContent === ${JSON.stringify(chip)})`};
    if (!b) return 'no button'; if (b.disabled) return 'disabled'; b.click(); return 'ok'; })()`);
}
async function openPicker(page) {
  await page.ev("document.getElementById('addSource').click()");
  return until(async () => { const p = await page.ev(Q.pickerRows()); return p && p.length >= 3 && p[0].rows.length ? p : null; }, 8000);
}
async function nameStrip(page, idx, text) {                    // the scribble strip: select, type, Enter (commits liveConfig)
  await page.ev(`(() => { const n = document.querySelectorAll('#strips .strip-name')[${idx}]; n.focus(); n.select(); })()`);
  await page.send("Input.insertText", { text });
  await page.key("Enter", 13);
}
function appTxt() { return readText(APPTXT).replace(/\r\n/g, "\n"); }
function stripLines() { return appTxt().split("\n").filter((l) => l.startsWith("LIVEIN\t")); }
// fix round: the session app.txt remembers (LIVESESSION <file name>), its journal's records, a dialog of a pid (R5: none)
function liveSession() { const l = appTxt().split("\n").find((x) => x.startsWith("LIVESESSION\t")); return l ? l.slice(12) : ""; }
function journalRecords(name) { return name ? readText(path.join(JOURNALS, name)).split("\n").filter((l) => l.length >= 2) : []; }
const dialogOf = (pid) => winsOf(pid).find((x) => x.cls === "#32770" && x.visible);
const R5 = (file) => (process.env.SUV_E2E_EXPORT === "1" ? {} : { SUVTITLE_TEST_EXPORT: path.join(OUT, file) });

// ---- a minimal zip reader (the .docx) --------------------------------------------------------------------------------------
function zipEntries(buf) {
  const eocd = buf.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  if (eocd < 0) return null;
  const n = buf.readUInt16LE(eocd + 10);
  let p = buf.readUInt32LE(eocd + 16);
  const out = {};
  for (let i = 0; i < n; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) return null;
    const method = buf.readUInt16LE(p + 10), csize = buf.readUInt32LE(p + 20);
    const nl = buf.readUInt16LE(p + 28), el = buf.readUInt16LE(p + 30), cl = buf.readUInt16LE(p + 32), lo = buf.readUInt32LE(p + 42);
    const name = buf.slice(p + 46, p + 46 + nl).toString("utf8");
    const ln = buf.readUInt16LE(lo + 26), le = buf.readUInt16LE(lo + 28);
    const data = buf.slice(lo + 30 + ln + le, lo + 30 + ln + le + csize);
    out[name] = method === 0 ? data : method === 8 ? zlib.inflateRawSync(data) : null;
    p += 46 + nl + el + cl;
  }
  return out;
}

// ============================================================================================================================
async function main() {
  fs.mkdirSync(WORKD, { recursive: true }); fs.mkdirSync(SHOTS, { recursive: true }); fs.mkdirSync(OUT, { recursive: true });
  fs.writeFileSync(logPath, "");
  // the isolation, proven before anything starts: the profile is ours, under L2, and is NOT the real one
  log("A0", under(PROF, L2) && path.resolve(ROAM).toLowerCase() !== String(REAL_APPDATA).toLowerCase() && path.resolve(LOCAL).toLowerCase() !== String(REAL_LOCAL).toLowerCase()
    && fs.existsSync(TOOLS) && fs.existsSync(path.join(BIN, "SuVtitleEngine.exe")), "the isolated profile is under L2 (not the user's); the fake engine and real_tools are built");
  const realSnap = () => [path.join(REAL_APPDATA, "SuVtitle"), path.join(REAL_LOCAL, "SuVtitle")].map((p) => { try { return fs.readdirSync(p).sort().join(","); } catch { return "absent"; } });   // (names only: an INSTALLED SuVtitle left running rewrites its own files)
  const realBefore = realSnap();
  // a clean profile, the exe + web copied fresh, the fake engine's log emptied
  fs.rmSync(PROF, { recursive: true, force: true });
  fs.mkdirSync(SUV_ROAM, { recursive: true }); fs.mkdirSync(LOCAL, { recursive: true });
  fs.copyFileSync(path.join(BUILD, "SuVtitle.exe"), EXE); fs.copyFileSync(path.join(BUILD, "WebView2Loader.dll"), path.join(BIN, "WebView2Loader.dll"));
  fs.rmSync(path.join(BIN, "web"), { recursive: true, force: true }); fs.cpSync(path.join(BUILD, "web"), path.join(BIN, "web"), { recursive: true });
  fs.rmSync(OUT, { recursive: true, force: true }); fs.mkdirSync(OUT, { recursive: true });
  try { fs.unlinkSync(FAKELOG); } catch { /* none */ }
  // both windows on the second screen (-1920,0 1920x1080): the Live window 900 x 780, the player 1100 x 720
  fs.writeFileSync(APPTXT, "# SuVtitle app v1\nWINPOS\t-1880\t40\t1100\t720\t0\t0\nLIVEPOS\t-1900\t60\t900\t780\t0\t0\n");

  // ---------------- A. --live alone: the Live window opens ALONE ----------------------------------------------------------
  const a = launch(["--live"], R5("Live export A.docx"));
  const pid = a.pid;
  const live = await connect("live.html");
  log("A1", !!live, "--live: the Live page is served and loaded (CDP target live.html)");
  if (!live) return;
  const wins = await until(() => { const w = winsOf(pid); return w.find((x) => x.cls === LIVE_CLASS && x.visible) ? w : null; }, 15000);
  const lw = (wins || []).find((x) => x.cls === LIVE_CLASS), pw = (wins || []).find((x) => x.cls === MAIN_CLASS);
  log("A2", lw && lw.visible && lw.title === "SuVtitle Live" && lw.x === -1900 && lw.y === 60 && lw.noactivate && !lw.owned && lw.caption,
    "the Live window is up: class SuVtitle.LiveWindow.suvl2, title \"SuVtitle Live\", where LIVEPOS put it on the second screen, WS_EX_NOACTIVATE, not owned", lw);
  log("A3", pw && !pw.visible && (wins || []).filter((x) => x.visible && x.cls !== "IME" && /SuVtitle/.test(x.cls)).length === 1,
    "...and it is ALONE: the player's window exists but is NOT visible (no taskbar button); no other SuVtitle window shows", (wins || []).map((x) => x.cls + ":" + x.visible));
  const fg = tool("fg");
  log("A4", fg.pid !== pid, "the test instance did not take the foreground (the foreground window is another process's)", fg);
  const kids = tool("kids", String(pid));
  log("A5", JSON.stringify(kids).toLowerCase().includes("suvtitleengine.exe"), "the engine (the fake) was started by the app as its child", kids);
  await until(() => fakeLines().some((l) => l.includes("HELLO_SENT")), 8000);
  const vis = await live.ev("document.visibilityState + '|' + document.title");
  log("A6", /^visible\|/.test(vis), "the Live page is visible; title: " + vis);
  const st0 = await live.ev(Q.state());
  const lang = await live.ev("({ a: document.getElementById('langA').value, b: document.getElementById('langB').value, mode: ['modeAuto','modeA2B','modeB2A'].map(i => document.getElementById(i).getAttribute('aria-pressed') || document.getElementById(i).className).join('|') })");
  log("A7", lang.a === "en" && lang.b === "fr", "the Live window shows the LIVE pair (en / fr, the fake engine's), not the player's (en / zh)", lang);
  const l0 = await live.ev(Q.listen());
  log("A8", l0.pressed === "false" && !l0.lit, "not listening at start (D-L8): the Listen button is off, the on-air lamp dark", l0);
  const strips0 = await live.ev(Q.strips());
  log("A9", strips0.length === 0, "a clean profile: no strip", strips0);
  await live.shot("A_empty");

  // the player's page (hidden) was told it is not shown
  const tl = await targets();
  const playerT = tl.find((x) => x.type === "page" && x.url.includes("index.html"));
  let playerShown = null;
  if (playerT) {
    const pp = await connect("index.html");
    playerShown = await pp.ev("document.visibilityState");
    pp.close();
  }
  log("A10", playerT && playerShown === "hidden", "the player's page exists but is hidden (visibilityState hidden): its view is invisible, throttled", playerShown);

  // CSP: reload the Live page with Network on and read the response headers of live.html
  const hdr = await (async () => {
    const got = {};
    live.on((d) => { if (d.method === "Network.responseReceived" && /live\.html$/.test(d.params.response.url)) got.h = d.params.response; });
    await live.send("Network.enable");
    await live.send("Page.reload", { ignoreCache: true });
    await until(() => got.h, 8000);
    return got.h;
  })();
  const csp = hdr && Object.entries(hdr.headers).find(([k]) => k.toLowerCase() === "content-security-policy");
  log("A11", hdr && hdr.status === 200 && csp && /default-src 'none'/.test(csp[1]) && !/media-src/.test(csp[1]) && !/connect-src/.test(csp[1]),
    "live.html is served 200 under the Live CSP: default-src 'none', NO media-src", csp && csp[1]);
  const cache = hdr && Object.entries(hdr.headers).find(([k]) => k.toLowerCase() === "cache-control");
  log("A12", cache && /no-store/.test(cache[1]), "...and Cache-Control: no-store", cache && cache[1]);
  await until(async () => (await live.ev("document.readyState")) === "complete", 8000);
  const st1 = await until(async () => { const s = await live.ev(Q.state()); return s || null; }, 5000);
  log("A13", await live.ev("document.getElementById('langB').value") === "fr", "after the reload the page's `ready` got its state back (the live pair again)", st1);

  // ---------------- B. the picker, the strips ------------------------------------------------------------------------------
  const pk = await openPicker(live);
  log("B1", !!pk && pk.map((s) => s.title).join("|") === "Inputs|Sound of an output|Applications", "the picker opens with a fresh list: sections Inputs / Sound of an output / Applications", pk && pk.map((s) => s.title));
  const flat = pk ? pk.flatMap((s) => s.rows) : [];
  const mic = flat.find((r) => r.name === "Microphone (USB Audio)");
  log("B2", mic && !mic.off && mic.badges.join(",") === "Default,Communications", "the mic row: name, Default + Communications badges, addable", mic);
  const bad = ["Webcam mic", "Headphones (BT)", "Microsoft Teams"].map((n) => flat.find((r) => r.name === n));
  log("B3", bad.every((r) => r && r.off && r.why), "absent / busy / denied rows are greyed and SAY why", bad);
  const ur = flat.find((r) => r.name === "Line 1-4 (UR44C)");
  log("B4", ur && ur.chips.length === 5 && ur.chips[0].startsWith("Mix"), "a 4-channel interface offers Mix + 4 channel chips", ur && ur.chips);
  log("B5", fakeLines().some((l) => /RX SRCLIST/.test(l)), "the engine was asked for the list (SRCLIST) when the picker opened");
  await live.shot("B_picker");
  const r1 = await clickPickerRow(live, "Microphone (USB Audio)");
  const s1 = await until(async () => { const s = await live.ev(Q.strips()); return s.length === 1 ? s : null; }, 5000);
  log("B6", r1 === "ok" && s1 && s1[0].name === "Microphone (USB Audio)" && s1[0].chan === "0" && s1[0].key.startsWith("in:"), "adding the mic: one strip, named after the source, on the mix", s1);
  await until(() => stripLines().length === 1, 5000);
  log("B7", stripLines().length === 1 && stripLines()[0].startsWith("LIVEIN\tin:"), "...persisted at once in app.txt (LIVEIN)", stripLines());
  await nameStrip(live, 0, "Alice");
  await until(() => stripLines().some((l) => l.includes("\tAlice\t")), 5000);
  log("B8", stripLines().some((l) => l.includes("\tAlice\t")), "naming the strip (Enter) is persisted", stripLines());
  // two channels of ONE interface = two strips (R1 lanes)
  await openPicker(live);
  const c1 = await clickPickerRow(live, "Line 1-4 (UR44C)", "1");
  await until(async () => (await live.ev(Q.strips())).length === 2, 5000);
  await nameStrip(live, 1, "Bob");
  await openPicker(live);
  const c2 = await clickPickerRow(live, "Line 1-4 (UR44C)", "2");
  const s3 = await until(async () => { const s = await live.ev(Q.strips()); return s.length === 3 ? s : null; }, 5000);
  await nameStrip(live, 2, "Carol");
  await until(() => stripLines().length === 3 && stripLines().some((l) => l.includes("\tCarol\t")), 5000);
  log("B9", c1 === "ok" && c2 === "ok" && s3 && s3[1].chan === "1" && s3[2].chan === "2" && s3[1].key === s3[2].key, "two channels of one interface = two strips on the same key (channels 1 and 2)", s3 && s3.map((s) => s.name + "@" + s.chan));
  const pk2 = await openPicker(live);
  const ur2 = pk2 && pk2.flatMap((s) => s.rows).find((r) => r.name === "Line 1-4 (UR44C)");
  log("B10", ur2 && ur2.chips[0].includes("(x)") && ur2.chips[1].includes("(x)") && ur2.chips[2].includes("(x)") && !ur2.chips[3].includes("(x)"),
    "the picker's rules: the interface's Mix and channels 1-2 are taken, 3-4 stay open", ur2 && ur2.chips);
  await live.ev("document.getElementById('pickerClose').click()");
  const lin = stripLines();
  log("B11", lin.length === 3 && lin[0].split("\t")[2] === "Alice" && lin[1].split("\t")[2] === "Bob" && lin[2].split("\t")[2] === "Carol" && lin[1].split("\t")[3] === "1" && lin[2].split("\t")[3] === "2",
    "app.txt LIVEIN holds the 3 strips in order with their labels and channels", lin);
  // a trim, committed (change): -6 dB on Bob
  await live.ev(`(() => { const t = document.querySelectorAll('#strips .strip-trim')[1]; t.value = '-6'; t.dispatchEvent(new Event('input', { bubbles: true })); t.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  await until(() => stripLines().some((l) => l.startsWith("LIVEIN\t") && l.endsWith("-6.0")), 5000);
  log("B12", stripLines().some((l) => l.endsWith("\t-6.0")), "a trim let go (-6 dB) is persisted with the wire's 0.1 dB precision", stripLines()[1]);
  // a liveConfig the host must REFUSE: a key the engine never listed
  const sBefore = await live.ev(Q.strips());
  await live.ev(`window.chrome.webview.postMessage({ type: 'liveConfig', inputs: [{ key: 'in:{bogus}', label: 'x', chan: 0, gainDb: 0 }] })`);
  const warned = await until(async () => { const s = await live.ev(Q.state()); return /not in the list/.test(s) ? s : null; }, 5000);
  const sAfter = await live.ev(Q.strips());
  log("B13", !!warned && JSON.stringify(sAfter.map((s) => s.name + s.chan)) === JSON.stringify(sBefore.map((s) => s.name + s.chan)) && stripLines().length === 3,
    "a liveConfig naming a key the engine never listed is REFUSED: a warn on the page, the strips as they were, app.txt untouched", warned);
  await live.shot("B_strips");

  // ---------------- C. Listen ----------------------------------------------------------------------------------------------
  const nLive1 = () => fakeLines().filter((l) => /RX LIVE\t1\t/.test(l.replace(/\|/g, "\t"))).length;
  await live.ev("document.getElementById('listenBtn').click()");
  const on = await until(async () => { const l = await live.ev(Q.listen()); return l.lit && l.pressed === "true" ? l : null; }, 10000);
  log("C1", !!on, "Listen: the on-air lamp lights from the engine's echo (STATE live=1)", on);
  const liveMsg = fakeLines().find((l) => /RX LIVE\|1\|/.test(l));
  log("C2", liveMsg && liveMsg.split("|")[2] === "3", "the engine received LIVE 1 with the 3 strips", liveMsg);
  const fields = liveMsg ? liveMsg.split("|").slice(3) : [];
  log("C3", fields.length >= 12 && fields[1] === "Alice" && fields[5] === "Bob" && fields[6] === "1" && fields[7] === "-6.0" && fields[9] === "Carol" && fields[10] === "2" && fields[4] === fields[8],
    "...with each strip's key, label, channel and trim (Bob ch 1 at -6.0 dB, Carol ch 2: the SAME device key)", fields.slice(0, 12));
  const lamps = await until(async () => { const s = await live.ev(Q.strips()); return s.every((x) => /is-open/.test(x.lamp)) ? s : null; }, 6000);
  log("C4", !!lamps, "INPUT (per DEVICE) lit the lamps of ALL three strips, the two channel strips of one interface together", lamps && lamps.map((s) => s.lamp));
  const fills = await until(async () => { const s = await live.ev(Q.strips()); return s.every((x) => parseFloat(x.lvl) > 0) ? s : null; }, 6000);
  const f2 = await live.ev(Q.strips());
  log("C5", !!fills && new Set(f2.map((x) => x.mt)).size >= 2, "LEVELS by lane reach each strip's meter (--lvl > 0 on all three, and the three meters read different levels)", f2.map((x) => x.lvl + " " + x.mt));
  // the player's page (hidden, never shown under --live) is told too: its Live button says on-air -- and (fix round, RVB-10) NO
  // notice, since no film waits for the Live window ("this film's subtitles will be made when it stops" would be untrue)
  const pp0 = await connect("index.html");
  const notice = pp0 && await until(async () => { const r = await pp0.ev("({ hidden: document.getElementById('liveNotice').hidden, on: document.getElementById('liveBtn').classList.contains('is-on-air'), text: document.getElementById('liveNotice').textContent })"); return r.on ? r : null; }, 6000);
  log("C5b", !!notice && notice.on && notice.hidden === true, "the PLAYER's page (hidden) is told the live mode listens: its Live button is on-air; no notice (no film waits: RVB-10)", notice);
  if (pp0) pp0.close();
  // the conversation: wait for the 4th utterance (Alice's second voice) -- ~13 s
  // (26/10, after the user's test: the second voice reads "Alice . voice 2" -- LIVE_PLAN.md, test_host_pins_live B3)
  const conv = await until(async () => { const l = await live.ev(Q.lines()); return l.some((x) => /Alice . voice 2/.test(x.who)) ? l : null; }, 30000, 500);
  const lines = conv || (await live.ev(Q.lines()));
  const whos = lines.map((x) => x.who);
  log("C6", whos.includes("Alice") && whos.includes("Bob") && whos.includes("Carol"), "final lines arrive named after THEIR strips (Alice, Bob, Carol -- Bob and Carol on two channels of one interface)", whos);
  log("C7", !!conv && whos.some((w) => /^Alice . voice 2$/.test(w)) && !whos.some((w) => /^Bob . /.test(w)), "the label rule: Alice's second voice is \"Alice . voice 2\"; the strips with one voice carry no number", whos);
  const aliceColors = new Set(lines.filter((x) => /^Alice/.test(x.who)).map((x) => x.style));
  const colorsOf = (n) => lines.filter((x) => x.who === n).map((x) => x.style)[0];
  log("C8", colorsOf("Alice") !== colorsOf("Bob") && colorsOf("Bob") !== colorsOf("Carol") && aliceColors.size >= 2, "...three strips = three colours, and Alice's two voices differ", { alice: colorsOf("Alice"), bob: colorsOf("Bob"), carol: colorsOf("Carol") });
  const journals = fs.existsSync(JOURNALS) ? fs.readdirSync(JOURNALS).filter((f) => f.endsWith(".jsonl")) : [];
  log("C9", journals.length === 1, "the session journal exists: Live sessions\\<date>.jsonl, one file", journals);
  await live.shot("C_listening");
  // renaming a strip while listening re-sends LIVE 1; the lines already shown keep their label
  const nBefore = nLive1();
  await nameStrip(live, 1, "Bobby");
  const re = await until(() => nLive1() > nBefore, 6000);
  log("C10", !!re && fakeLines().some((l) => /RX LIVE\|1\|3\|.*\|Bobby\|/.test(l)), "renaming a strip while listening re-sends LIVE 1 with the new label (the set is replaced in place)");
  await sleep(200);
  const l2 = await live.ev(Q.lines());
  log("C11", l2.some((x) => x.who === "Bob") , "...and the lines already shown keep their old label (Bob)", l2.map((x) => x.who));
  const bobby = await until(async () => { const l = await live.ev(Q.lines()); return l.some((x) => x.who === "Bobby") ? l : null; }, 20000, 500);
  log("C12", !!bobby, "...while the NEXT line of that strip carries the new one (Bobby)", bobby && bobby.map((x) => x.who));
  // the live pair: the language B select -> CTL scope=live, never the player's pair
  const ctlBefore = fakeLines().filter((l) => /RX CTL/.test(l)).length;
  await live.ev(`(() => { const s = document.getElementById('langB'); s.value = 'es'; s.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  const ctl = await until(() => fakeLines().filter((l) => /RX CTL/.test(l)).slice(ctlBefore).find((l) => /\|es\|live/.test(l)), 6000);
  log("C13", !!ctl, "changing language B on the Live page sends CTL ... scope=live (en/es): the LIVE pair", ctl);
  const allCtl = fakeLines().filter((l) => /RX CTL/.test(l));
  log("C14", allCtl.every((l) => /\|live$/.test(l) || !/\|es\|/.test(l)) && !allCtl.some((l) => /\|es$/.test(l)), "...and no scope-less CTL carried it: the player's pair is untouched", allCtl);
  const echoB = await until(async () => (await live.ev("document.getElementById('langB').value")) === "es", 5000);
  log("C15", !!echoB, "the engine's STATE echo (liveLangB=es) is what the page then shows");
  // Clear: CTL clear=1 scope=live; the page empties; the NEXT line starts a new journal
  await live.ev("document.getElementById('clearBtn').click()");
  const clr = await until(() => fakeLines().find((l) => /RX CTL\|.*\|1\|.*\|live$/.test(l)), 6000);
  const emptied = await until(async () => (await live.ev(Q.lines())).length === 0 ? true : null, 4000);
  log("C16", !!clr && !!emptied, "Clear: CTL with clear=1 scope=live reaches the engine and the page's log empties", clr);
  const newJ = await until(() => { const j = fs.readdirSync(JOURNALS).filter((f) => f.endsWith(".jsonl")); return j.length === 2 ? j : null; }, 25000, 500);
  log("C17", !!newJ, "the next line after a Clear starts a NEW journal file (two sessions, not one .docx with two meetings)", newJ);
  // the pin
  await live.ev("document.getElementById('pinToggle').click()");
  await sleep(2500);
  const topNow = !!winsOf(pid).find((x) => x.cls === LIVE_CLASS).topmost;
  const pinNow = (await live.ev("document.getElementById('pinToggle').getAttribute('aria-pressed')")) === "true";
  log("C18", topNow === pinNow, "the pin shows the window's REAL state (RV4-7): WS_EX_TOPMOST=" + topNow + ", page pin=" + pinNow + (topNow ? "" : " (Windows refused it to this never-activated window: the page came back to the truth)"));
  await live.ev("document.getElementById('pinToggle').click()");
  await sleep(2000);
  const top2 = !!winsOf(pid).find((x) => x.cls === LIVE_CLASS).topmost;
  const pin2 = (await live.ev("document.getElementById('pinToggle').getAttribute('aria-pressed')")) === "true";
  log("C19", top2 === pin2 && !top2, "...clicked again: not topmost, and the page agrees");
  // THE DRAIN (fix round, the ADDENDUM: LIVE 0 = stop listening, then FINISH): Listen off -> the engine stops capturing but
  // still says live 1 while its queued sentences come out (the fake: the open utterance cut + 3 queued, one per 700 ms) ->
  // "Finishing the last sentences…", the lamp blinking on its way off, the lines still arriving; then live 0 -> not listening
  const rowsNow = () => live.ev("document.querySelectorAll('#log .tr-line').length");
  const fin0 = fakeLines().length, rows0 = await rowsNow();
  await live.ev("document.getElementById('listenBtn').click()");
  const finishing = await until(async () => {
    const r = await live.ev("({ st: document.getElementById('state').textContent, fin: document.getElementById('onAir').classList.contains('is-finishing'), lit: document.getElementById('onAir').classList.contains('is-lit'), pressed: document.getElementById('listenBtn').getAttribute('aria-pressed') })");
    return /^Finishing the last sentences/.test(r.st) && r.fin && !r.lit && r.pressed === "false" ? r : null;
  }, 5000, 50);
  // (the page says it at once -- the app stopped wanting -- the engine hears LIVE 0 a moment later: waited for, not raced)
  const engineDraining = await until(() => fakeLines().slice(fin0).some((l) => /RX LIVE\|0/.test(l)) && fakeLines().slice(fin0).some((l) => /DRAIN_START/.test(l)), 5000, 50);
  log("C20", !!finishing && !!engineDraining,
    "Listen off while the engine still finishes: LIVE 0 sent, the page says \"Finishing the last sentences…\", the ON AIR lamp blinks (is-finishing, not lit), Listen is up", finishing);
  await live.shot("C_finishing");
  const drained = await until(async () => (/^not listening/.test(await live.ev(Q.state())) ? true : null), 10000, 100);
  const rows1 = await rowsNow();
  const drainLog = fakeLines().slice(fin0).map((l) => l.replace(/^\d+ \d+ /, ""));
  log("C21", !!drained && rows1 > rows0 && drainLog.some((l) => /^DRAIN_DONE/.test(l))
    && !(await live.ev("document.getElementById('onAir').classList.contains('is-finishing') || document.getElementById('onAir').classList.contains('is-lit')")),
    "...the drained sentences arrive on the page (" + (rows1 - rows0) + " more rows), then STATE live 0: \"not listening\", the lamp dark", drainLog.filter((l) => /DRAIN|LIVE/.test(l)));
  // a press DURING the drain = listen again: the engine resumes (no new start)
  await live.ev("document.getElementById('listenBtn').click()");
  await until(async () => (await live.ev(Q.listen())).lit, 8000);
  const res0 = fakeLines().length;
  await live.ev("document.getElementById('listenBtn').click()");
  await until(async () => /^Finishing/.test(await live.ev(Q.state())), 5000, 50);
  // (the second press only once the ENGINE is finishing: pressed faster, off + on meet in the supervisor -- one LIVE 1, no drain)
  await until(() => fakeLines().slice(res0).some((l) => /DRAIN_START/.test(l)), 5000, 50);
  await live.ev("document.getElementById('listenBtn').click()");
  const resumed = await until(async () => { const l = await live.ev(Q.listen()); return l.lit && l.pressed === "true" && !l.pending ? l : null; }, 8000);
  await until(() => fakeLines().slice(res0).some((l) => /DRAIN_RESUMED/.test(l)), 5000, 50);
  log("C22", !!resumed && fakeLines().slice(res0).some((l) => /DRAIN_RESUMED/.test(l)) && /^(listening|transcribing)/.test(await live.ev(Q.state())),
    "Listen pressed again WHILE it finishes: LIVE 1 resumes the engine (DRAIN_RESUMED), the lamp lit, listening again", fakeLines().slice(res0).map((l) => l.replace(/^\d+ \d+ /, "")).filter((l) => /DRAIN|LIVE/.test(l)));

  // ---------------- D. Export ----------------------------------------------------------------------------------------------
  // OPT-IN (SUV_E2E_EXPORT=1): the Export raises the OS Save dialog, and Windows may bring that dialog to the foreground when the
  // user has been idle (a game pad is not keyboard / mouse input): the dialog's owner -- the Live window -- then stays the active window
  // after it closes, until the app ends. Seen in 2 of the 6 runs of 25/10 (the user idle 150-230 s); D1-D6 passed in all six.
  // FIX ROUND (R5): by default the Export is answered by the TEST INSTANCE's hook (SUVTITLE_TEST_EXPORT, never read by the
  // product): the whole Export runs -- the document built from the journal, written, acknowledged -- and NO OS window is raised.
  if (process.env.SUV_E2E_EXPORT !== "1") {
    await sleep(3500);                                        // a line or two into the new session
    const exportPath = path.join(OUT, "Live export A.docx");
    const fgBefore = tool("fg");
    await live.ev("document.getElementById('exportBtn').click()");
    let sawDialog = false;
    const saved = await until(() => { if (dialogOf(pid)) sawDialog = true; return fs.existsSync(exportPath) && fs.statSync(exportPath).size > 1000 ? true : null; }, 10000, 50);
    log("D1", !!saved && !sawDialog && !fs.existsSync(exportPath + ".tmp"),
      "Export (R5, a test instance): the .docx is written where SUVTITLE_TEST_EXPORT names it, no .tmp left -- and NO OS Save dialog appeared", { saved: !!saved, sawDialog });
    const ent = saved ? zipEntries(fs.readFileSync(exportPath)) : null;
    const doc = ent && ent["word/document.xml"] ? ent["word/document.xml"].toString("utf8") : "";
    log("D4", ent && !!ent["[Content_Types].xml"] && /SuVtitle Live/.test(doc) && /20\d\d-\d\d-\d\d/.test(doc), "a real .docx: [Content_Types].xml, word/document.xml titled \"SuVtitle Live ... <date>\"", doc.slice(0, 200));
    log("D5", /Alice|Bob|Carol/.test(doc), "...holding the session's lines with their speakers", (doc.match(/<w:t[^>]*>[^<]*<\/w:t>/g) || []).slice(0, 6));
    const ack = await until(async () => { const s = await live.ev(Q.state()); return /Saved/.test(s) ? s : null; }, 5000);
    log("D6", !!ack, "the page acknowledges: \"Saved . <file>\"", ack);
    const fg2 = tool("fg");
    log("D7", fgBefore.pid !== pid && fg2.pid !== pid && !dialogOf(pid), "the foreground was never the test instance's (no dialog to take it)", { fgBefore, after: fg2 });
  } else {
  await sleep(3500);                                        // a line or two into the new session
  const exportPath = path.join(OUT, "Live export test.docx");
  const fgBefore = tool("fg"), idle = tool("idle");
  await live.ev("document.getElementById('exportBtn').click()");
  const dlg = tool("savedlg", String(pid), exportPath, "10000");
  log("D1", dlg.ok === true && dlg.text === exportPath, "Export opens the Save dialog; its file name set (by messages) to a path under L2 and Save clicked", dlg);
  log("D2", dlg.x !== undefined && dlg.x < 0, "...the dialog opened on the second screen (x < 0)", dlg);
  const saved = await until(() => (fs.existsSync(exportPath) && fs.statSync(exportPath).size > 1000 ? true : null), 10000);
  log("D3", !!saved, "the .docx was written (temp + rename, no .tmp left)", { size: saved ? fs.statSync(exportPath).size : 0, tmp: fs.existsSync(exportPath + ".tmp") });
  const ent = saved ? zipEntries(fs.readFileSync(exportPath)) : null;
  const doc = ent && ent["word/document.xml"] ? ent["word/document.xml"].toString("utf8") : "";
  log("D4", ent && !!ent["[Content_Types].xml"] && /SuVtitle Live/.test(doc) && /20\d\d-\d\d-\d\d/.test(doc), "a real .docx: [Content_Types].xml, word/document.xml titled \"SuVtitle Live ... <date>\"", doc.slice(0, 200));
  log("D5", /Alice|Bob|Carol/.test(doc) && /en|English/.test(doc), "...holding the session's lines with their speakers", (doc.match(/<w:t[^>]*>[^<]*<\/w:t>/g) || []).slice(0, 6));
  const ack = await until(async () => { const s = await live.ev(Q.state()); return /Saved/.test(s) ? s : null; }, 5000);
  log("D6", !!ack, "the page acknowledges: \"Saved . <file>\"", ack);
  await live.shot("D_exported");
  const fg2 = tool("fg");
  // The product's OWN windows never activate (WS_EX_NOACTIVATE, SW_SHOWNOACTIVATE: A4, H3). The OS Save dialog is not the product's to
  // keep in the background: Windows decides whether a process that is not the foreground one may bring it up. When it did (the dialog was
  // the foreground window while up), Windows re-activates the dialog's OWNER when it closes -- seen in 2 of 6 runs, both with the user
  // idle 150-230 s: the foreground after the export is then the Live window. That is the OS's rule, reported (not hidden); fails only if the foreground is ours
  // WITHOUT the dialog having had it (something of ours activated by itself), or was ours before the export.
  const dialogHadForeground = dlg.fgpid === pid;
  log("D7", fgBefore.pid !== pid && (fg2.pid !== pid || dialogHadForeground),
    fg2.pid !== pid ? "the test instance never took the foreground (not even for its Save dialog)"
      : "NOTE: the OS gave the Save dialog the foreground (Windows' foreground rules allowed it; the user had been idle " + Math.round((idle.idleMs || 0) / 1000) + " s) and re-activated its owner on close -- not an activation by the product",
    { fgBefore, atDialog: { pid: dlg.fgpid, cls: dlg.fgcls }, after: fg2, idle });
  }
  log("D8", live.errors.length === 0, "the Live page raised no exception and logged no console error / CSP violation during the whole session (A-D)", live.errors);

  // ---------------- E. close the Live window -------------------------------------------------------------------------------
  // FIX ROUND (the ADDENDUM): it is the LAST window and the engine is LISTENING -- hidden at once, LIVE 0, the app waits for the
  // engine to FINISH (STATE live 0: the queued sentences into the journal), THEN tells it to QUIT and ends (15 s at most)
  const logBefore = fakeLines().length;
  const sessA = liveSession(), recA = journalRecords(sessA).length;
  const t0 = Date.now();
  const closeRes = tool("post-close", LIVE_CLASS, String(pid));
  const hidAt = await until(() => (!winsOf(pid).some((x) => (x.cls === LIVE_CLASS || x.cls === MAIN_CLASS) && x.visible) ? Date.now() - t0 : null), 3000, 50);
  const aliveWhileDraining = a.exited === null;
  const gone = await until(() => (a.exited !== null ? true : null), 25000, 200);
  const after = fakeLines().slice(logBefore).map((l) => l.replace(/^\d+ \d+ /, ""));
  log("E1", closeRes.ok === true && hidAt !== null && aliveWhileDraining && !!gone,
    "closing the LAST window while listening: every window hidden at once (" + hidAt + " ms), the app alive while the engine finishes, then it ends (exit " + a.exited + ", in " + (Date.now() - t0) + " ms)", { closeRes, hidAt, exited: a.exited });
  const iOff = after.findIndex((l) => /^RX LIVE\|0/.test(l)), iDone = after.findIndex((l) => /^DRAIN_DONE/.test(l)), iQuit = after.findIndex((l) => l === "RX QUIT");
  log("E2", iOff >= 0 && iDone > iOff && iQuit > iDone,
    "...LIVE 0 first, the engine FINISHED (DRAIN_DONE: STATE live 0), and only THEN QUIT (QUIT drops what is not finished)", after.filter((l) => /^(RX (LIVE|QUIT)|DRAIN)/.test(l)));
  const recA2 = journalRecords(sessA);
  const drainedInJournal = recA2.slice(recA).filter((l) => /"segId":9\d\d/.test(l)).length;
  log("E2b", recA2.length > recA && drainedInJournal >= 1,
    "...and the sentences it finished are in the session's journal (" + (recA2.length - recA) + " more records, " + drainedInJournal + " of the queued ones) -- the export will have them", { sessA, before: recA, after: recA2.length });
  log("E3", !alive(pid), "no SuVtitle process of ours is left");
  const p3 = appTxt();
  log("E4", /LIVEPOS\t-?\d+\t\d+\t\d+\t\d+\t[01]\t[01]/.test(p3) && /LIVEPOS\t-1\d{3}\t/.test(p3) && stripLines().length === 3, "app.txt still holds LIVEIN strips and LIVEPOS (second screen)", p3.split("\n").filter((l) => /^(LIVEPOS|WINPOS)/.test(l)));
  log("E4b", !!sessA && liveSession() === sessA, "...and LIVESESSION: the session the app ended in is remembered (RVB-1: it survives the restart until a Clear)", liveSession());

  // ---------------- F. a second start: the strips come back, nothing listens ---------------------------------------------
  try { fs.unlinkSync(FAKELOG); } catch { /* none */ }
  const b = launch(["--live"], R5("Live export after restart.docx"));
  const live2 = await connect("live.html");
  const s4 = live2 && await until(async () => { const s = await live2.ev(Q.strips()); return s.length === 3 ? s : null; }, 10000);
  log("F1", !!s4 && s4.map((s) => s.name).join(",") === "Alice,Bobby,Carol", "restart with --live: the 3 strips are back with their names", s4 && s4.map((s) => s.name));
  const lw2 = winsOf(b.pid).find((x) => x.cls === LIVE_CLASS);
  log("F2", lw2 && lw2.visible && lw2.x === -1900 && lw2.y === 60, "...the Live window is back where it was (second screen)", lw2);
  await sleep(2500);
  log("F3", !fakeLines().some((l) => /RX LIVE\|1/.test(l)) && (await live2.ev(Q.listen())).pressed === "false", "...and NOTHING listens at launch (no LIVE 1 sent, Listen off)");
  // FIX ROUND (RVB-1): THE SESSION SURVIVED THE RESTART -- its last lines replayed, its export the WHOLE file (R5: no dialog)
  const recF = journalRecords(sessA).length;
  const replayRows = await until(async () => { const n = await live2.ev("document.querySelectorAll('#log .tr-line').length"); return n >= Math.min(200, recF) ? n : null; }, 10000);
  const tooltip = await live2.ev("document.getElementById('exportBtn').title");
  log("F4", replayRows === Math.min(200, recF) && new RegExp("— " + recF + " lines").test(tooltip),
    "after the restart the session is BACK: the page replays its last " + replayRows + " lines and the Export's tooltip counts the whole journal (" + recF + " lines)", { replayRows, recF, tooltip });
  const exportB = path.join(OUT, "Live export after restart.docx");
  if (process.env.SUV_E2E_EXPORT !== "1") {
    let sawDlg = false;
    await live2.ev("document.getElementById('exportBtn').click()");
    const savedB = await until(() => { if (dialogOf(b.pid)) sawDlg = true; return fs.existsSync(exportB) && fs.statSync(exportB).size > 1000 ? true : null; }, 10000, 50);
    const docB = savedB ? ((zipEntries(fs.readFileSync(exportB)) || {})["word/document.xml"] || Buffer.from("")).toString("utf8") : "";
    const nB = (/(\d+) lines/.exec(docB) || [])[1];
    log("F5", !!savedB && !sawDlg && Number(nB) === recF && /Alice|Bob|Carol/.test(docB),
      "...and Export (R5: no OS dialog) exports the WHOLE session -- the lines from BEFORE the restart included (" + nB + " of " + recF + ")", { nB, recF, sawDlg });
  } else {
    log("F5", true, "SKIPPED with SUV_E2E_EXPORT=1 (the OS dialog path: D1-D7)");
  }
  // FIX ROUND (RVB-5): a LIVE the engine REFUSES -- said, and never "starting to listen…" for ever (the fake refuses a strip
  // labelled "refuse", the engine's way: one STATE whose err says why, the live state unchanged)
  await nameStrip(live2, 0, "Alice refuse");
  await until(() => stripLines().some((l) => l.includes("\tAlice refuse\t")), 5000);
  const ref0 = fakeLines().length;
  await live2.ev("document.getElementById('listenBtn').click()");
  const saidWhy = await until(async () => { const s = await live2.ev(Q.state()); return /could not open these inputs/.test(s) ? s : null; }, 6000, 50);
  const afterWarn = await until(async () => { const s = await live2.ev(Q.state()); const l = await live2.ev(Q.listen()); return /^not listening/.test(s) && !l.pending && l.pressed === "false" ? s : null; }, 9000);
  log("F6", !!saidWhy && !!afterWarn && fakeLines().slice(ref0).some((l) => /LIVE refused \(err\)/.test(l)),
    "a REFUSED LIVE: the engine's sentence is shown, then \"not listening\" (never \"starting to listen…\" for ever), Listen up", { saidWhy, afterWarn });
  await nameStrip(live2, 0, "Alice");
  await until(() => stripLines().some((l) => l.includes("\tAlice\t")), 5000);
  // ---------------- G. the engine dies while listening: the supervisor brings it back and re-sends LIVE 1 ------------------
  await live2.ev("document.getElementById('listenBtn').click()");
  await until(async () => (await live2.ev(Q.listen())).lit, 8000);
  const eng = tool("kids", String(b.pid));
  const ep = (eng.kids || []).find((k) => /suvtitleengine/i.test(k.exe || ""));
  const epid = ep && ep.pid;
  if (epid) cp.spawnSync("taskkill", ["/PID", String(epid), "/F"], { windowsHide: true });
  const back = await until(() => fakeLines().filter((l) => /START/.test(l)).length >= 2 && fakeLines().filter((l) => /RX LIVE\|1\|3/.test(l)).length >= 2, 25000, 250);
  log("G1", !!epid && !!back, "the engine killed while listening: the supervisor respawns it and RE-SENDS the wanted LIVE 1 (the meeting resumes by itself)", { epid, starts: fakeLines().filter((l) => /START/.test(l)).length });
  const relit = await until(async () => (await live2.ev(Q.listen())).lit, 15000);
  log("G2", !!relit, "...and the page's on-air lamp lights again from the new engine's echo");
  await live2.shot("G_after_respawn");
  // ProcessFailed (F1): the Live view's RENDERER dies while listening -> the app reloads it by itself; the page's `ready` brings
  // back its state (the strips, the on-air lamp) and the scrollback replay (the lines it had)
  const linesBeforeCrash = (await until(async () => { const l = await live2.ev(Q.lines()); return l.length >= 2 ? l : null; }, 15000) || []).length;
  try { await live2.send("Page.crash"); } catch { /* the socket may close under the call */ }
  live2.close();
  await sleep(1500);
  const live2b = await connect("live.html");
  const s6 = live2b && await until(async () => { const s = await live2b.ev(Q.strips()); return s.length === 3 ? s : null; }, 20000);
  const l6 = live2b && await until(async () => { const l = await live2b.ev(Q.lines()); return l.length >= Math.min(2, linesBeforeCrash) ? l : null; }, 15000);
  const on6 = live2b && await until(async () => (await live2b.ev(Q.listen())).lit, 15000);
  log("G4", !!live2b && !!s6 && !!l6 && !!on6, "the Live renderer crashed (CDP Page.crash) while listening: the app reloaded the view by itself -- the strips are back, the scrollback replayed (" + (l6 ? l6.length : 0) + " of " + linesBeforeCrash + " paragraphs), the on-air lamp lit", { strips: !!s6, lines: l6 && l6.length, on: !!on6 });
  const liveWin6 = winsOf(b.pid).find((x) => x.cls === LIVE_CLASS);
  log("G5", !!liveWin6 && liveWin6.visible && b.exited === null, "...and the app and its Live window were never gone (no rebuild needed for a renderer death)");
  // ProcessFailed (F3): the WebView2 BROWSER process itself dies (killed here by pid: it is our own child) -> the app rebuilds the
  // environment and BOTH views by itself; the Live window comes back with its page, its strips and its on-air lamp -- the engine and
  // the journal never noticed (no new START of the engine)
  const startsBefore = fakeLines().filter((l) => /START/.test(l)).length;
  const wvq = cp.spawnSync("powershell", ["-NoProfile", "-NonInteractive", "-Command",
    "Get-CimInstance Win32_Process -Filter \"ParentProcessId=" + b.pid + "\" | Where-Object { $_.Name -eq 'msedgewebview2.exe' } | Select-Object ProcessId, CommandLine | ConvertTo-Json -Compress"],
    { encoding: "utf8", timeout: 30000, windowsHide: true });
  let wvList = [];
  try { const j = JSON.parse(wvq.stdout); wvList = Array.isArray(j) ? j : [j]; } catch { wvList = []; }
  const browserProc = wvList.find((p) => p && p.CommandLine && !/--type=/.test(p.CommandLine));
  // FIX ROUND (RVB-3): the Live window MINIMIZED (pinned over a game, say) when the browser dies -- the recovery must give it its
  // view back WITHOUT restoring it, without taking the foreground (before: openLiveWindow restored it, even in a test instance)
  const minRes = tool("minimize", LIVE_CLASS, String(b.pid));
  const fgBeforeCrash = tool("fg");
  if (browserProc) { live2b && live2b.close(); cp.spawnSync("taskkill", ["/PID", String(browserProc.ProcessId), "/F"], { windowsHide: true }); }
  await sleep(2500);
  const live2c = browserProc && await connect("live.html");
  const s7 = live2c && await until(async () => { const s = await live2c.ev(Q.strips()); return s.length === 3 ? s : null; }, 30000);
  const on7 = live2c && await until(async () => (await live2c.ev(Q.listen())).lit, 20000);
  const lines7 = live2c && await until(async () => { const l = await live2c.ev(Q.lines()); return l.length >= 1 ? l : null; }, 15000);
  log("G6", !!browserProc && !!live2c && !!s7 && !!on7 && !!lines7 && b.exited === null,
    "the WebView2 BROWSER process killed while listening: the app rebuilt the environment and both views -- the Live page is back with its strips, the scrollback and the on-air lamp; the app lived on",
    { browser: !!browserProc, page: !!live2c, strips: !!s7, on: !!on7, lines: lines7 && lines7.length, appAlive: b.exited === null });
  const liveWin7 = winsOf(b.pid).find((x) => x.cls === LIVE_CLASS);
  log("G7", !!liveWin7 && liveWin7.visible && fakeLines().filter((l) => /START/.test(l)).length === startsBefore,
    "...the Live window is the same window, visible; the engine was NOT restarted (the view died, not the engine)", { visible: liveWin7 && liveWin7.visible, starts: fakeLines().filter((l) => /START/.test(l)).length, before: startsBefore });
  const fgAfterCrash = tool("fg");
  log("G8", minRes.iconic === true && !!liveWin7 && liveWin7.iconic === true && fgBeforeCrash.pid !== b.pid && fgAfterCrash.pid !== b.pid,
    "RVB-3: the recovery left the MINIMIZED Live window minimized and took NO foreground (before and after the rebuild: another process's window"
    + (fgAfterCrash.hwnd === fgBeforeCrash.hwnd ? ", the very same one)" : "; the user moved meanwhile)"),
    { minimized: minRes, iconicAfter: liveWin7 && liveWin7.iconic, fgBefore: fgBeforeCrash, fgAfter: fgAfterCrash });
  tool("restore", LIVE_CLASS, String(b.pid));
  if (live2c) { await live2c.shot("G_after_browser_rebuild"); live2c.close(); }
  tool("post-close", LIVE_CLASS, String(b.pid));
  await until(() => (b.exited !== null ? true : null), 20000, 200);
  log("G3", b.exited !== null && !alive(b.pid), "closing the Live window (listening) ends the app again", b.exited);

  // ---------------- H. --live forwarded to a running instance; the last-window rule ---------------------------------------
  const c = launch([]);                                      // a plain start: the PLAYER shows (second screen)
  const pw3 = await until(() => winsOf(c.pid).find((x) => x.cls === MAIN_CLASS && x.visible), 15000);
  log("H1", !!pw3 && !winsOf(c.pid).some((x) => x.cls === LIVE_CLASS && x.visible), "a plain start shows the PLAYER only (no Live window)", pw3 && { x: pw3.x, noact: pw3.noactivate });
  await until(async () => (await targets()).some((x) => x.url.includes("index.html")), 15000);
  const t1 = Date.now();
  const f = launch(["--live"]);                              // forwarded to the running instance
  const fExit = (await until(() => (f.exited !== null ? { code: f.exited } : null), 15000, 100) || {}).code;
  const lw3 = await until(() => winsOf(c.pid).find((x) => x.cls === LIVE_CLASS && x.visible), 15000);
  log("H2", fExit === 0 && !!lw3, "a second launch with --live is FORWARDED: it exits 0 (" + (Date.now() - t1) + " ms) and the running instance opens its Live window", { fExit, lw3: !!lw3 });
  const fg4 = tool("fg");
  log("H3", fg4.pid !== c.pid, "...without taking the foreground (a test instance)", fg4);
  const live3 = await connect("live.html");
  const s5 = live3 && await until(async () => { const s = await live3.ev(Q.strips()); return s.length === 3 ? s : null; }, 10000);
  log("H4", !!s5, "...its page loaded with the saved strips", s5 && s5.map((s) => s.name));
  // listening, then closing the Live window while the player is shown: listening STOPS (LIVE 0), the app STAYS, the Live window hides
  try { fs.unlinkSync(FAKELOG); } catch { /* none */ }
  await live3.ev("document.getElementById('listenBtn').click()");
  const lit3 = await until(async () => (await live3.ev(Q.listen())).lit, 10000);
  tool("post-close", LIVE_CLASS, String(c.pid));
  const off3 = await until(() => fakeLines().find((l) => /RX LIVE\|0/.test(l)), 6000);
  await sleep(1200);
  const w4 = winsOf(c.pid);
  log("H5", !!lit3 && !!off3 && c.exited === null && w4.some((x) => x.cls === MAIN_CLASS && x.visible) && w4.some((x) => x.cls === LIVE_CLASS && !x.visible),
    "listening, then closing the Live window with the player shown: LIVE 0 reaches the engine, the app STAYS, the Live window is hidden (not destroyed)", { lit3: !!lit3, off3, wins: w4.map((x) => x.cls + ":" + x.visible) });
  const f2x = launch(["--live"]);
  await until(() => (f2x.exited !== null ? true : null), 15000, 100);
  const lw4 = await until(() => winsOf(c.pid).find((x) => x.cls === LIVE_CLASS && x.visible), 10000);
  const live4 = lw4 && await connect("live.html");
  const l4 = live4 && await until(async () => { const l = await live4.ev(Q.listen()); return l.pressed === "false" && !l.lit ? l : null; }, 8000);
  log("H6", !!lw4 && !!l4, "a forwarded --live brings the hidden Live window back at once, and it still says NOT listening (the close stopped it)", l4);
  // the player's own Live button (openLive{}): hide the Live window again first; the notice is gone (the close stopped the listening)
  tool("post-close", LIVE_CLASS, String(c.pid));
  await sleep(1500);
  const hiddenAgain = winsOf(c.pid).some((x) => x.cls === LIVE_CLASS && !x.visible);
  const pp = await connect("index.html");
  const noticeGone = pp && await pp.ev("document.getElementById('liveNotice').hidden && !document.getElementById('liveBtn').classList.contains('is-on-air')");
  if (pp) await pp.ev("document.getElementById('liveBtn').click()");
  const lw5 = await until(() => winsOf(c.pid).find((x) => x.cls === LIVE_CLASS && x.visible), 8000);
  log("H6b", hiddenAgain && !!noticeGone && !!lw5, "the player's Live button (openLive{}) brings the hidden Live window up; its notice had gone with the listening", { hiddenAgain, noticeGone, up: !!lw5 });
  if (pp) pp.close();
  // close the PLAYER while the Live window is open: it only hides
  tool("post-close", MAIN_CLASS, String(c.pid));
  await sleep(1500);
  const w5 = winsOf(c.pid);
  log("H7", c.exited === null && w5.some((x) => x.cls === MAIN_CLASS && !x.visible) && w5.some((x) => x.cls === LIVE_CLASS && x.visible), "closing the PLAYER while the Live window is open: the player HIDES, the app stays (last-window rule)", w5.map((x) => x.cls + ":" + x.visible));
  const g = launch([]);                                       // a plain second launch: the hidden player comes back
  const gExit = (await until(() => (g.exited !== null ? { code: g.exited } : null), 15000, 100) || {}).code;
  const pw5 = await until(() => winsOf(c.pid).find((x) => x.cls === MAIN_CLASS && x.visible), 10000);
  log("H8", gExit === 0 && !!pw5, "a plain launch while the player is hidden brings the player back (an empty --play) and exits 0", { gExit });
  const laterErrors = [...live2.errors, ...(live2b ? live2b.errors : []), ...(live2c ? live2c.errors : []), ...live3.errors, ...(live4 ? live4.errors : []),
                       ...live.errors];
  log("H8b", laterErrors.length === 0, "...and none of the later Live pages (restart, respawned engine, reloaded after a renderer crash, rebuilt after a browser crash, forwarded --live) complained either", laterErrors);
  // FIX ROUND (the ADDENDUM): a RELAUNCH while the app waits for the engine to finish before ending -- it brings the app BACK
  // (never a second instance racing the one that is ending): the player hidden, listening, the Live window closed = the last
  // window (hidden at once, LIVE 0, the drain ~3 s) and a --live launched at once
  tool("post-close", MAIN_CLASS, String(c.pid));
  await until(() => winsOf(c.pid).some((x) => x.cls === MAIN_CLASS && !x.visible), 5000);
  const live5 = await connect("live.html");
  if (live5) await live5.ev("document.getElementById('listenBtn').click()");
  const lit5 = live5 && await until(async () => (await live5.ev(Q.listen())).lit, 10000);
  const rl0 = fakeLines().length;
  tool("post-close", LIVE_CLASS, String(c.pid));
  const hidden5 = await until(() => (!winsOf(c.pid).some((x) => (x.cls === LIVE_CLASS || x.cls === MAIN_CLASS) && x.visible) ? true : null), 3000, 50);
  const r5 = launch(["--live"]);
  const r5Exit = (await until(() => (r5.exited !== null ? { code: r5.exited } : null), 15000, 100) || {}).code;
  await sleep(6000);                                         // well past the drain: an end not cancelled would have happened
  const back5 = winsOf(c.pid).find((x) => x.cls === LIVE_CLASS && x.visible);
  const st5 = live5 && await until(async () => { const s = await live5.ev(Q.state()); return /^not listening/.test(s) ? s : null; }, 5000);
  log("H10", !!lit5 && !!hidden5 && r5Exit === 0 && c.exited === null && !!back5 && !!st5
    && fakeLines().slice(rl0).some((l) => /RX LIVE\|0/.test(l)) && !fakeLines().slice(rl0).some((l) => /RX QUIT/.test(l)),
    "a relaunch (--live) while the app waited for the drain before ending: forwarded (exit 0), the app STAYS (no QUIT), its Live window back, not listening",
    { lit5: !!lit5, hidden5, r5Exit, alive: c.exited === null, back: !!back5, st5 });
  // close the last window again (not listening now): the app ends at once
  tool("post-close", LIVE_CLASS, String(c.pid));
  const ended = await until(() => (c.exited !== null ? true : null), 20000, 200);
  log("H9", !!ended, "the last window closed (not listening): the app quits", c.exited);
  // FIX ROUND (the ADDENDUM): the drain is waited for 15 s AT MOST -- an engine still finishing then is told to QUIT
  try { fs.unlinkSync(FAKELOG); } catch { /* none */ }
  const d = launch(["--live"], { SUV_FAKE_DRAIN_LINES: "40", SUV_FAKE_DRAIN_MS: "700" });   // a drain of ~28 s
  const live6 = await connect("live.html");
  await until(async () => (await live6.ev(Q.strips())).length === 3, 10000);
  await live6.ev("document.getElementById('listenBtn').click()");
  const lit6 = await until(async () => (await live6.ev(Q.listen())).lit, 10000);
  await sleep(1500);
  const t6 = Date.now();
  tool("post-close", LIVE_CLASS, String(d.pid));
  const end6 = await until(() => (d.exited !== null ? Date.now() - t6 : null), 30000, 200);
  const log6 = fakeLines().map((l) => l.replace(/^\d+ \d+ /, ""));
  const q6 = log6.indexOf("RX QUIT"), done6 = log6.findIndex((l) => /^DRAIN_DONE/.test(l));
  log("H11", !!lit6 && end6 !== null && end6 >= 14000 && end6 <= 22000 && q6 >= 0 && done6 < 0 && log6.filter((l) => /^DRAIN_LINE/.test(l)).length >= 10,
    "a drain longer than 15 s: the app waited ~15 s (" + end6 + " ms), then told the engine to QUIT while it was still finishing (QUIT does not drain: the rest is lost, by the director's bound)",
    { end6, quit: q6 >= 0, drainDone: done6 >= 0, drained: log6.filter((l) => /^DRAIN_LINE/.test(l)).length });
  const lastErrors = [...(live5 ? live5.errors : []), ...(live6 ? live6.errors : [])];
  log("H12", lastErrors.length === 0, "...and the Live pages of the drain / relaunch runs raised no exception, console error or CSP violation", lastErrors);

  // ---------------- Z. nothing of the user's was touched -------------------------------------------------------------------
  const realAfter = realSnap();
  log("Z1", JSON.stringify(realBefore) === JSON.stringify(realAfter), "the user's %APPDATA%\\SuVtitle and %LOCALAPPDATA%\\SuVtitle hold exactly the same files as before (names; this instance cannot even start on them)", { realBefore, realAfter });
}

process.on("exit", () => { for (const c of launched) if (c.exited === null && c.pid) cp.spawnSync("taskkill", ["/PID", String(c.pid), "/T", "/F"], { windowsHide: true }); });
main().catch((e) => { log("FATAL", false, String(e && e.stack || e)); }).finally(() => {
  const bad = results.filter((r) => !r.ok);
  console.log("\nlive_e2e: " + results.length + " checks, " + bad.length + " failed" + (bad.length ? ": " + bad.map((r) => r.step).join(", ") : ""));
  process.exit(bad.length ? 1 : 0);
});

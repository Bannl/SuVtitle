// native_real.js -- N1 (2026-10-26): the REAL SuVtitle with its NATIVE VIDEO ENGINE (mpv under a visual-hosted page), run as a
// TEST INSTANCE and driven like the user drives it -- the house way of tests\e2e\real\real_e2e.js and live_e2e.js:
// --test-instance suvn1 (its own class / mutex names, never the foreground), an ISOLATED profile (APPDATA / LOCALAPPDATA under
// work\real\profile: the product refuses a test instance otherwise), its WebView2 reachable through CDP (port 9391; the variable
// OVERRIDES the product's switches, so it repeats all three), the window on the SECOND screen (app.txt WINPOS), the fake engine
// (no model, no CPU analysis), mpv SILENT (a test instance is ao=null). Input is POSTED to the app's own window (native_tool) --
// the real mouse is never moved -- and the page is read through CDP (its read-only window.__suvNative, the DOM).
//   R1 the window: second screen, never activated          R2 the engine available, the --play file opened IN MPV
//   R3 the picture: a disabled child at the page's rect x dpr, mpv's own window inside it (another thread)
//   R4 it plays (a POSTED click on the page's Play button)  R5 the page draws OVER the picture (a capture shows both)
//   R6 forwarded input reaches the page (move, right-click, wheel, keys, click)
//   R7 full screen (a posted double-click): the picture fills the screen; the idle cursor hides (logged; the user's real cursor
//      never) and comes back on a move; Escape leaves it -- < 25 s of full screen in all
//   R8 the zoom (Ctrl + wheel): the picture follows the page's devicePixelRatio
//   R9 a file DROPPED through the window's drop target plays (the page's drop zone, File objects)
//   R10 a real mouse over the picture would reach OUR window (hit test)   R11 a sound file: no picture window
//   R13 UI Automation from the window reaches the page's controls         R12 closing: the app ends at once, mpv terminated
//   R14 no libmpv-2.dll: the page's <video> plays, visual-hosted          R15 (fix round MEDIUM4) started IN a folder planted with
//       DLLs (Explorer's --play: CWD = the film's folder): it plays in mpv, no module of the app / WebView2 comes from it
// Run: run_native_real.cmd. Results: work\real\runs\N.jsonl + PNGs. Exit 0 = every check passed.
"use strict";
const fs = require("fs"), path = require("path"), cp = require("child_process");

const HERE = __dirname, REPO = path.resolve(HERE, "..", "..", "..");
const BUILD = path.join(REPO, "build", process.env.SUV_N1_CONFIG || "x64-Debug");
const WORK = path.join(HERE, "work", "real"), BIN = path.join(WORK, "bin"), PROF = path.join(WORK, "profile");
const RUNS = path.join(WORK, "runs"), MEDIA = path.join(WORK, "media");
const ROAM = path.join(PROF, "Roaming"), LOCAL = path.join(PROF, "Local");
const EXE = path.join(BIN, "SuVtitle.exe"), TOOL = path.join(BIN, "native_tool.exe");
const NLOG = path.join(LOCAL, "SuVtitle", "native.log");
const TAG = "suvn1", PORT = 9391;
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays"
               + " --enable-blink-features=AudioVideoTracks";   // = main.cpp createWebViewEnv, all three
const CLIP = path.join(MEDIA, "real720.mkv"), DROP = path.join(MEDIA, "drop720.mkv"), TONE = path.join(MEDIA, "tone.flac");

const results = [];
function log(step, ok, what, detail) {
  results.push({ step, ok: !!ok, what, detail: detail === undefined ? null : detail });
  fs.appendFileSync(path.join(RUNS, "N.jsonl"), JSON.stringify({ step, ok: !!ok, what, detail }) + "\n");
  console.log((ok ? "  ok    " : "  FAIL  ") + step + "  " + what + (detail === undefined ? "" : "  -> " + JSON.stringify(detail).slice(0, 500)));
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const under = (p, root) => path.resolve(p).toLowerCase().startsWith(path.resolve(root).toLowerCase() + path.sep);
function tool(args) {
  const r = cp.spawnSync(TOOL, args.map(String), { encoding: "utf8", windowsHide: true, timeout: 30000 });
  const line = String(r.stdout || "").trim().split(/\r?\n/).pop() || "{}";
  try { return JSON.parse(line); } catch { return { ok: false, raw: line }; }
}
const nlog = () => { try { return fs.readFileSync(NLOG, "utf8"); } catch { return ""; } };

// ---- CDP (tests\e2e\real\real_e2e.js's) ----------------------------------------------------------------------------------
class Cdp {
  constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); this.errors = []; }
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
          this.errors.push(JSON.stringify(m.params.exceptionDetails).slice(0, 400));
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
    const r = await this.send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error("eval: " + JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result.value;
  }
  close() { try { this.ws.close(); } catch { /* gone */ } }
}
let cdp = null, pid = 0;
async function connect() {
  for (let t0 = Date.now(); Date.now() - t0 < 30000; await sleep(300)) {
    let list = null;
    try { list = await (await fetch("http://127.0.0.1:" + PORT + "/json/list")).json(); } catch { /* not yet */ }
    const t = (list || []).find((x) => x.type === "page" && /^https:\/\/suvtitle\.local\/index\.html/.test(x.url || ""));
    if (t) { const c = new Cdp(t.webSocketDebuggerUrl); await c.open(); await c.send("Runtime.enable"); return c; }
  }
  throw new Error("no CDP target for the player's page");
}
async function waitFor(fn, ms, label) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < ms) {
    try { last = await fn(); if (last) return last; } catch (e) { last = { err: e.message }; }
    await sleep(100);
  }
  if (label) console.log("    (timeout: " + label + ", last " + JSON.stringify(last).slice(0, 300) + ")");
  return null;
}
const N = (expr) => cdp.eval("(() => { const n = window.__suvNative; return n ? (" + expr + ") : null; })()");
const info = () => tool(["info", pid, TAG]);
const post = (what, x, y, d, ctrl) => tool(["post", pid, TAG, what, Math.round(x), Math.round(y), d || 0, ctrl ? 1 : 0]);
const key = (vk) => tool(["key", pid, TAG, vk.toString(16)]);
async function click(x, y) { post("ldown", x, y); await sleep(40); post("lup", x, y); }
// the page's picture rectangle (CSS px) x dpr = the client pixels the picture window must cover
async function pageRect() {
  const v = await cdp.eval("(() => { const n = window.__suvNative; const r = n ? String(n.rect() || '') : '';"
    + " return { r, dpr: window.devicePixelRatio, iw: innerWidth, ih: innerHeight }; })()");
  const p = String(v.r || "").split(",").map(Number);
  if (p.length !== 4 || p.some((x) => !Number.isFinite(x))) return null;
  const s = v.dpr;
  return { css: p, dpr: s, px: [Math.round(p[0] * s), Math.round(p[1] * s), Math.round((p[0] + p[2]) * s), Math.round((p[1] + p[3]) * s)], iw: v.iw, ih: v.ih };
}
const near = (a, b, tol) => a && b && a.length === 4 && b.length === 4 && a.every((v, i) => Math.abs(v - b[i]) <= tol);
async function elRect(id) {
  return cdp.eval("(() => { const e = document.getElementById(" + JSON.stringify(id) + "); if (!e) return null; const r = e.getBoundingClientRect();"
    + " const s = window.devicePixelRatio; return { x: (r.left + r.width / 2) * s, y: (r.top + r.height / 2) * s, w: r.width * s, h: r.height * s }; })()");
}

// ---- setup ---------------------------------------------------------------------------------------------------------------
function ffmpeg(args) { const r = cp.spawnSync("ffmpeg", ["-y", "-hide_banner", "-loglevel", "error", ...args], { windowsHide: true, timeout: 180000 }); return r.status === 0; }
function setup() {
  for (const f of ["SuVtitle.exe", "WebView2Loader.dll", "web", path.join("mpv", "libmpv-2.dll")])
    if (!fs.existsSync(path.join(BUILD, f))) { console.log("native_real: " + path.join(BUILD, f) + " is missing (build with SUVTITLE_MPV_DIR set)"); process.exit(2); }
  if (!under(PROF, HERE)) throw new Error("refusing to clear " + PROF);
  fs.rmSync(PROF, { recursive: true, force: true });
  fs.rmSync(RUNS, { recursive: true, force: true });
  for (const d of [RUNS, MEDIA, path.join(ROAM, "SuVtitle"), path.join(LOCAL, "SuVtitle"), path.join(BIN, "mpv")]) fs.mkdirSync(d, { recursive: true });
  fs.copyFileSync(path.join(BUILD, "SuVtitle.exe"), EXE);
  fs.copyFileSync(path.join(BUILD, "WebView2Loader.dll"), path.join(BIN, "WebView2Loader.dll"));
  fs.rmSync(path.join(BIN, "web"), { recursive: true, force: true });
  fs.cpSync(path.join(BUILD, "web"), path.join(BIN, "web"), { recursive: true });
  const dll = path.join(BIN, "mpv", "libmpv-2.dll");
  if (!fs.existsSync(dll)) { try { fs.linkSync(path.join(BUILD, "mpv", "libmpv-2.dll"), dll); } catch { fs.copyFileSync(path.join(BUILD, "mpv", "libmpv-2.dll"), dll); } }
  // the window on the SECOND screen (the app's own app.txt, in the isolated profile)
  fs.writeFileSync(path.join(ROAM, "SuVtitle", "app.txt"), "# SuVtitle app v1\nWINPOS\t-1880\t40\t1100\t720\t0\t0\n");
  if (!fs.existsSync(CLIP) && !ffmpeg(["-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=60", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=60",
      "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-c:a", "aac", CLIP])) throw new Error("ffmpeg (clip)");
  if (!fs.existsSync(DROP) && !ffmpeg(["-f", "lavfi", "-i", "smptehdbars=s=1280x720:r=30:d=30", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=30",
      "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-c:a", "aac", DROP])) throw new Error("ffmpeg (drop)");
  if (!fs.existsSync(TONE) && !ffmpeg(["-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000:duration=30", "-c:a", "flac", TONE])) throw new Error("ffmpeg (tone)");
}
function testEnv() {
  const e = {};
  for (const k of Object.keys(process.env)) {
    const u = k.toUpperCase();
    if (u === "APPDATA" || u === "LOCALAPPDATA" || u === "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") continue;
    e[k] = process.env[k];
  }
  e.APPDATA = ROAM;
  e.LOCALAPPDATA = LOCAL;
  e.WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=" + PORT + " " + SWITCHES;
  e.SUV_FAKE_MODE = "live";                    // the fake engine: answers a FILE at once (no model, no CPU analysis)
  e.SUVTITLE_TEST_NO_MOUSELEAVE = "1";         // posted moves while the real cursor is elsewhere (main.cpp, test instances only)
  return e;
}

async function main() {
  setup();
  const child = cp.spawn(EXE, ["--test-instance", TAG, "--play", CLIP], { env: testEnv(), stdio: "ignore", windowsHide: false });
  pid = child.pid;
  let exited = false;
  child.on("exit", () => { exited = true; });
  try {
    // ---- R1 ----
    const w0 = await waitFor(() => { const i = info(); return i.ok ? i : null; }, 20000, "the window");
    log("R1", w0 && w0.windowRect[0] < -1000 && !w0.foreground && (parseInt(w0.exStyle, 16) & 0x08000000) !== 0,
        "the player's window: on the SECOND screen, WS_EX_NOACTIVATE, never the foreground", w0 && { rect: w0.windowRect, fg: w0.foreground, ex: w0.exStyle });
    cdp = await connect();
    // ---- R2 ----
    const av = await waitFor(() => N("n.available() && n.backend() === 'native' && n.gen() > 0 ? { gen: n.gen(), backend: n.backend() } : null"), 25000, "native");
    log("R2", !!av, "the engine is available and the --play file plays IN MPV (backend native)", av);
    // ---- R3 ----
    const placed = await waitFor(async () => {
      const i = info(), r = await pageRect();
      return i.ok && i.video && i.video.visible && r && r.css[2] > 0 && near(i.video.rect, r.px, 1) ? { i, r } : null;
    }, 15000, "the picture placed");
    const i3 = placed ? placed.i : info();
    log("R3", !!placed && i3.video.disabled, "the picture window: DISABLED, at the page's rectangle x devicePixelRatio (+-1 px)",
        placed ? { video: i3.video.rect, page: placed.r.px, css: placed.r.css, dpr: placed.r.dpr } : i3.video);
    log("R3", !!(i3.mpv && i3.mpv.otherThread), "mpv's own window lives inside it, on mpv's thread", i3.mpv);
    // ---- R4 ----
    if (await N("n.paused()")) { const b = await elRect("tPlay"); if (b) await click(b.x, b.y); }
    const t0 = await N("n.time()");
    const playing = await waitFor(async () => { const t = await N("n.time()"); return !(await N("n.paused()")) && t > t0 + 600 ? t : null; }, 8000, "playing");
    log("R4", !!playing, "it PLAYS (a click POSTED to the window, on the page's Play button): the clock advances", { from: t0, to: playing });
    const vidIdle = await cdp.eval("(() => { const v = document.getElementById('fileVideo'); return v ? { src: v.currentSrc || '', paused: v.paused } : null; })()");
    log("R4", vidIdle && !vidIdle.src, "...and the page's own <video> is idle (no source): the picture is mpv's", vidIdle);
    // ---- R5: the page OVER the picture ----
    const r5 = await pageRect();
    const mx = r5.css[0] + r5.css[2] / 2 - 40, my = r5.css[1] + r5.css[3] / 2 - 25;
    await cdp.eval("(() => { const d = document.createElement('div'); d.id = '__n1mark'; d.style.cssText = 'position:fixed;left:" + mx + "px;top:" + my
      + "px;width:80px;height:50px;background:#ff00ff;z-index:2147483647;pointer-events:none'; document.body.appendChild(d); return 1; })()");
    await sleep(400);
    const s = r5.dpr, pic = r5.px;
    const cap = tool(["cap", pid, TAG, path.join(RUNS, "R5_windowed.png"),
      Math.round(mx * s) + 4, Math.round(my * s) + 4, Math.round(80 * s) - 8, Math.round(50 * s) - 8,     // the page's marker
      pic[0] + 8, pic[1] + 8, Math.round((pic[2] - pic[0]) * 0.3), Math.round((pic[3] - pic[1]) * 0.3),   // the picture, beside it
      0, pic[3] + 4, Math.min(600, w0.client[0]), Math.max(8, Math.min(40, w0.client[1] - pic[3] - 4))]);    // the page's bar under it
    await cdp.eval("(() => { const d = document.getElementById('__n1mark'); if (d) d.remove(); return 1; })()");
    const [mk, pc, bar] = cap.regions || [];
    log("R5", cap.ok && mk && mk.magenta > 0.9 && pc && pc.colourful > 0.05 && pc.sd > 20,
        "a capture of the window shows BOTH: the page's opaque marker drawn OVER mpv's colourful picture", { marker: mk, picture: pc });
    log("R5", bar && bar.colourful < 0.05, "...and the page's own interface around it (the bar: not the picture)", bar);
    // ---- R6: forwarded input ----
    await cdp.eval("(() => { const c = window.__n1in = { move: 0, down: 0, up: 0, ctx: 0, wheel: 0, key: 0, click: 0, last: {} };"
      + " const on = (t, k) => window.addEventListener(t, (e) => { c[k]++; c.last[k] = [e.clientX, e.clientY, e.button, e.key || '', e.deltaY || 0, !!e.ctrlKey]; }, true);"
      + " on('mousemove', 'move'); on('mousedown', 'down'); on('mouseup', 'up'); on('contextmenu', 'ctx'); on('wheel', 'wheel'); on('keydown', 'key'); on('click', 'click');"
      + " return 1; })()");
    const px = pic[0] + Math.round((pic[2] - pic[0]) * 0.3), py = pic[1] + Math.round((pic[3] - pic[1]) * 0.4);
    post("move", px, py);
    const mv = await waitFor(() => cdp.eval("window.__n1in.move > 0 ? window.__n1in.last.move : null"), 3000, "move");
    log("R6", mv && Math.abs(mv[0] - px / s) <= 1 && Math.abs(mv[1] - py / s) <= 1, "a POSTED move reaches the page at the same point (client px / dpr)", { posted: [px, py], page: mv });
    post("rdown", px, py); await sleep(40); post("rup", px, py);
    const cx = await waitFor(() => cdp.eval("window.__n1in.ctx > 0 ? window.__n1in.last.ctx : null"), 3000, "contextmenu");
    log("R6", cx && Math.abs(cx[0] - px / s) <= 1, "a POSTED right-click reaches the page: its contextmenu (the page's own menu) at the pointer", cx);
    key(0x1b);                                                     // Escape: the page's menu closes
    const k1 = await waitFor(() => cdp.eval("window.__n1in.key > 0 ? window.__n1in.last.key : null"), 3000, "key");
    log("R6", k1 && k1[3] === "Escape", "a key POSTED to the view's input window reaches the page (Escape)", k1);
    post("wheel", px, py, -120);
    const wh = await waitFor(() => cdp.eval("window.__n1in.wheel > 0 ? window.__n1in.last.wheel : null"), 3000, "wheel");
    log("R6", wh && wh[4] > 0, "a POSTED wheel (screen coordinates) reaches the page (deltaY > 0 for a turn down)", wh);
    const p0 = await N("n.paused()");
    await click(px, py);
    const flipped = await waitFor(async () => ((await N("n.paused()")) !== p0 ? true : null), 3000, "click toggles");
    log("R6", flipped && (await cdp.eval("window.__n1in.click")) > 0, "a POSTED click on the picture is the page's: play / pause toggles", { before: p0 });
    if (await N("n.paused()")) { await click(px, py); await waitFor(async () => (!(await N("n.paused()")) ? true : null), 3000); }
    // ---- R7: full screen, the idle cursor ----
    const logBefore = nlog().length;
    const fsT0 = Date.now();
    post("ldown", px, py); await sleep(30); post("lup", px, py); await sleep(30); post("ldbl", px, py); await sleep(30); post("lup", px, py);
    const fsOn = await waitFor(() => { const i = info(); return i.ok && i.fullscreenRect ? i : null; }, 4000, "full screen");
    log("R7", !!fsOn, "a POSTED double-click on the picture: full screen (the window = its monitor)", fsOn && { rect: fsOn.windowRect, monitor: fsOn.monitor });
    if (fsOn) {
      const full = await waitFor(() => { const i = info(); return i.video && i.video.visible && near(i.video.rect, [0, 0, i.client[0], i.client[1]], 1) ? i : null; }, 4000, "fs picture");
      log("R7", !!full, "the picture fills the screen (the picture window = the whole client)", (full || info()).video);
      const fcap = tool(["cap", pid, TAG, path.join(RUNS, "R7_fullscreen.png"), Math.round(fsOn.client[0] * 0.3), Math.round(fsOn.client[1] * 0.2),
        Math.round(fsOn.client[0] * 0.4), Math.round(fsOn.client[1] * 0.4)]);
      log("R7", fcap.ok && fcap.regions[0].colourful > 0.05, "a capture in full screen shows the picture", fcap.regions && fcap.regions[0]);
      // still for 2.5 s while playing: the page's idle state -- the cursor hidden (logged; a test instance keeps the arrow)
      const hid = await waitFor(() => (nlog().slice(logBefore).includes("cursor HIDDEN") ? true : null), 6000, "idle hide");
      const realCur = info().cursor;
      log("R7", !!hid, "idle in full screen: the page's cursor HIDDEN (native.log; CursorChanged or the page's word)", nlog().slice(logBefore).split(/\r?\n/).filter((l) => /cursor/.test(l)).slice(-3));
      log("R7", realCur && realCur.showing, "...the user's REAL cursor never hidden by this test instance (CURSOR_SHOWING)", realCur);
      const logMid = nlog().length;
      post("move", px + 7, py + 5);
      const back = await waitFor(() => (nlog().slice(logMid).includes("cursor SHOWN") ? true : null), 4000, "cursor back");
      log("R7", !!back, "a move: the cursor comes BACK (the page left its idle state)", nlog().slice(logMid).split(/\r?\n/).filter((l) => /cursor/.test(l)).slice(-2));
      key(0x1b);                                                   // Escape: out of full screen
      const fsOff = await waitFor(() => { const i = info(); return i.ok && !i.fullscreenRect ? i : null; }, 4000, "fs off");
      const fsSecs = (Date.now() - fsT0) / 1000;
      log("R7", fsOff && fsOff.windowRect[0] === w0.windowRect[0] && fsOff.windowRect[1] === w0.windowRect[1]
          && fsOff.windowRect[2] === w0.windowRect[2] && fsOff.windowRect[3] === w0.windowRect[3],
          "Escape: back EXACTLY where it was", fsOff && { now: fsOff.windowRect, before: w0.windowRect });
      log("R7", fsSecs < 25, "full screen lasted under 25 s (the user's Claude window lives on that screen)", fsSecs.toFixed(1) + " s");
      const back2 = await waitFor(async () => { const i = info(), r = await pageRect(); return i.video && r && near(i.video.rect, r.px, 1) ? { i, r } : null; }, 4000, "windowed rect");
      log("R7", !!back2, "...and the picture is back at the page's windowed rectangle", back2 && { video: back2.i.video.rect, page: back2.r.px });
    }
    // ---- R8: Ctrl + wheel -- the modifier is forwarded. It is the PAGE's gesture (the subtitles' size: it calls preventDefault,
    //      so the browser zoom never happens and the view's ZoomFactor stays 1): the picture must not move. (The scale path --
    //      RasterizationScaleChanged / ZoomFactorChanged -> setDpiScale -- is not reachable here: both screens are 96 DPI and
    //      the page blocks the zoom; the CSS -> pixel maths at 150 % is the smoke's [PLACE].) ----
    const wc0 = await cdp.eval("window.__n1in.wheel");
    post("wheel", px, py, 120, true);
    const zw = await waitFor(() => cdp.eval("window.__n1in.wheel > " + wc0 + " ? window.__n1in.last.wheel : null"), 3000, "ctrl wheel");
    await sleep(300);
    const r8 = await pageRect(), i8 = info();
    log("R8", zw && zw[5] === true && r8 && r8.dpr === s && i8.video && near(i8.video.rect, r8.px, 1),
        "Ctrl + wheel reaches the page WITH Ctrl (its own gesture, the subtitles' size: no browser zoom) -- devicePixelRatio and the picture unchanged",
        { event: zw, dpr: r8 && r8.dpr, video: i8.video && i8.video.rect, page: r8 && r8.px });
    post("wheel", px, py, -120, true);                            // (the subtitles' size back)
    // ---- R9: a dropped file ----
    const g9 = await N("n.gen()");
    const lb9 = nlog().length;
    const dr = tool(["drop", pid, TAG, DROP]);
    const handed = await waitFor(() => (nlog().slice(lb9).includes("a file drop handed to the page") ? true : null), 5000, "drop handed");
    const g9b = await waitFor(async () => { const g = await N("n.gen()"); return g && g !== g9 && (await N("n.backend()")) === "native" ? g : null; }, 15000, "drop opened");
    // the dropped file is the queue's CURRENT item (its row is in the DOM even with the fold-out closed) -- since U1 (02/11)
    // the strip no longer prints the name (its tooltip does): visible text is not the test
    const named = !!(await waitFor(() => cdp.eval("(() => { const r = document.querySelector('.q-row.is-cur .q-name'), s = document.getElementById('fileStatus');"
      + " return (r && r.textContent.indexOf('drop720') >= 0) || (s && String(s.title || '').indexOf('drop720') >= 0) ? true : null; })()"), 5000, "the dropped file's name"));
    log("R9", dr.accepted && handed && g9b && named, "a file DROPPED through the window's drop target (the shell's data object): the page's drop zone takes it as the playing item, it plays in mpv",
        { accepted: dr.accepted, handed: !!handed, gen: [g9, g9b], named, log: nlog().slice(lb9).split(/\r?\n/).filter((l) => /drop|open item/.test(l)) });
    // ---- R10 ----
    const r10 = await pageRect();
    const h = tool(["hit", pid, TAG, Math.round((r10.px[0] + r10.px[2]) / 2), Math.round((r10.px[1] + r10.px[3]) / 2)]);
    if (h.rootIsOurs) log("R10", h.who === "main", "a REAL mouse over the picture would reach OUR window (the picture's window is disabled: never mpv's)", h);
    else log("R10", true, "the hit test is NOT CHECKED: another program's window covers ours at that point", h);
    // ---- R11: a sound file ----
    const g11 = await N("n.gen()");
    tool(["drop", pid, TAG, TONE]);
    const snd = await waitFor(async () => { const g = await N("n.gen()"); const i = info(); return g && g !== g11 && i.video && !i.video.visible && !(await N("n.picture()")) ? { g, v: i.video } : null; }, 15000, "sound file");
    log("R11", !!snd, "a SOUND file (dropped): it plays in mpv with no picture window (the page says no picture)", snd);
    // ---- R13: UI Automation (a visual-hosted view's tree is handed out by the host's window: WM_GETOBJECT) ----
    const ua = tool(["uia", pid, TAG, "Volume"]);
    log("R13", ua.ok && ua.found, "UI Automation from the player's window reaches the page's controls (its \"Volume\" slider)", ua);
    // ---- the page's own errors ----
    log("R*", cdp.errors.length === 0, "no exception in the page during the run", cdp.errors.slice(0, 3));
    log("R*", !info().foreground, "the test instance never took the foreground");
  } catch (e) {
    log("R!", false, "the run stopped: " + e.message, String(e.stack || "").split("\n").slice(0, 4));
  }
  // ---- R12: the end ----
  if (cdp) cdp.close();
  const t12 = Date.now();
  tool(["close", pid, TAG]);
  for (let t = 0; t < 150 && !exited; ++t) await sleep(100);
  const endMs = Date.now() - t12;
  const term = (nlog().match(/mpv terminated [^\r\n]*/g) || []).pop() || "";
  log("R12", exited && endMs < 15000 && /terminated\s+in/.test(term), "closing: the app ends at once, mpv terminated cleanly", { ms: endMs, term });
  if (!exited) cp.spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], { windowsHide: true });   // (ours only)
  await fallbackRun();
  await plantedRun();
  const bad = results.filter((r) => !r.ok);
  console.log("NATIVE_REAL " + (bad.length ? "FAILED" : "OK") + " -- " + (results.length - bad.length) + " passed, " + bad.length + " failed");
  process.exit(bad.length ? 1 : 0);
}
// ---- R14: the SAME app without libmpv-2.dll: the automatic fallback -- the page's own <video> shows the picture, under the
// same visual hosting (the page's transparent default background must not leave a hole), no picture window at all ----------
async function fallbackRun() {
  const dll = path.join(BIN, "mpv", "libmpv-2.dll"), off = dll + ".off";
  try { fs.renameSync(dll, off); } catch (e) { log("R14", false, "could not set the DLL aside: " + e.message); return; }
  const child = cp.spawn(EXE, ["--test-instance", TAG, "--play", CLIP], { env: testEnv(), stdio: "ignore", windowsHide: false });
  pid = child.pid;
  let exited = false;
  child.on("exit", () => { exited = true; });
  try {
    await waitFor(() => { const i = info(); return i.ok ? i : null; }, 20000, "the window (fallback)");
    cdp = await connect();
    const role = await waitFor(() => N("!n.available() && n.backend() && n.backend() !== 'native' ? { backend: n.backend(), why: true } : null"), 25000, "fallback role");
    log("R14", !!role, "no libmpv-2.dll: the engine says unavailable and the page plays with its own <video>", role);
    if (await cdp.eval("(() => { const v = document.getElementById('fileVideo'); return !!v && v.paused; })()")) {
      const b = await elRect("tPlay");
      if (b) await click(b.x, b.y);
    }
    const plays = await waitFor(() => cdp.eval("(() => { const v = document.getElementById('fileVideo'); return v && !v.paused && v.currentTime > 0.5 ? v.currentTime : null; })()"), 10000, "video plays");
    const i = info();
    const vr = await cdp.eval("(() => { const r = document.getElementById('fileVideo').getBoundingClientRect(); const s = devicePixelRatio;"
      + " return [Math.round(r.left * s), Math.round(r.top * s), Math.round(r.width * s), Math.round(r.height * s)]; })()");
    const cap = tool(["cap", pid, TAG, path.join(RUNS, "R14_fallback.png"), vr[0] + Math.round(vr[2] * 0.25), vr[1] + Math.round(vr[3] * 0.2),
      Math.round(vr[2] * 0.5), Math.round(vr[3] * 0.5)]);
    log("R14", plays && !i.video && cap.ok && cap.regions[0].colourful > 0.05,
        "...it plays, the window captured shows the <video>'s picture (visual hosting draws it), and there is no picture window",
        { t: plays, video: i.video, region: cap.regions && cap.regions[0] });
  } catch (e) {
    log("R14", false, "the fallback run stopped: " + e.message);
  }
  if (cdp) cdp.close();
  tool(["close", pid, TAG]);
  for (let t = 0; t < 150 && !exited; ++t) await sleep(100);
  if (!exited) cp.spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], { windowsHide: true });
  log("R14", exited, "...and it closes");
  // (put back -- Bitdefender may hold the file a moment after the process ends: a few tries, 20 ms apart)
  for (let k = 0; k < 5 && !fs.existsSync(dll); ++k) { try { fs.renameSync(off, dll); } catch { await sleep(20); } }
}

// ---- R15 (fix round MEDIUM4): started as Explorer starts it for --play -- the CURRENT DIRECTORY = the film's folder -- in a
// folder PLANTED with DLLs under the names libmpv / FFmpeg / the GPU stacks load BY NAME at run time and most PCs lack (the
// reviewer's list, docs\REVIEW_RV-NH.md MEDIUM4; harmless copies of System32's msimg32.dll): the film plays in mpv and NOT ONE
// module of SuVtitle.exe, of any WebView2 process or of the engine comes from that folder; native.log says the search is locked.
// (The mechanism's positive control -- a planted DLL IS loaded from the CWD before the lock -- is mpv_smoke's [DLL].)
const PLANT_NAMES = ["OpenCL", "VSScript", "avisynth", "d3dcompiler_43", "d3dcompiler_46", "dxcompiler", "dxil", "libEGL", "libGLESv2",
  "libbluray", "libmfx64-gen", "libmfx64", "libplacebo-349", "libshaderc_shared", "libvpl", "libx265", "libx265_main",
  "libx265_main12", "loadall", "nvEncodeAPI64", "nvcuda", "nvcuvid", "shaderc_shared", "vapoursynth"];
function modulesUnder(dir) {
  // every module of SuVtitle.exe, SuVtitleEngine.exe and EVERY msedgewebview2.exe (another app's would never load from here)
  const ps = "$d = '" + dir.replace(/'/g, "''") + "\\'; $n = 0; $hits = @(); "
    + "Get-Process -Name SuVtitle,SuVtitleEngine,msedgewebview2 -ErrorAction SilentlyContinue | ForEach-Object { "
    + "  try { foreach ($m in $_.Modules) { $n++; if ($m.FileName -and $m.FileName.StartsWith($d, [System.StringComparison]::OrdinalIgnoreCase)) "
    + "  { $hits += ($_.ProcessName + ':' + $m.FileName) } } } catch { } }; "
    + "Write-Output ('MODULES ' + $n); $hits | ForEach-Object { Write-Output ('HIT ' + $_) }";
  const r = cp.spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", ps], { encoding: "utf8", windowsHide: true, timeout: 60000 });
  const out = String(r.stdout || "");
  const m = /MODULES (\d+)/.exec(out);
  return { modules: m ? +m[1] : -1, hits: out.split(/\r?\n/).filter((l) => l.startsWith("HIT ")).map((l) => l.slice(4)) };
}
async function plantedRun() {
  const plant = path.join(WORK, "plant");
  try {
    fs.mkdirSync(plant, { recursive: true });
    const src = path.join(process.env.SystemRoot || "C:\\Windows", "System32", "msimg32.dll");
    for (const n of PLANT_NAMES) { const d = path.join(plant, n + ".dll"); if (!fs.existsSync(d)) fs.copyFileSync(src, d); }
    const film = path.join(plant, "film.mkv");
    if (!fs.existsSync(film)) { try { fs.linkSync(CLIP, film); } catch { fs.copyFileSync(CLIP, film); } }
    const logBefore = nlog().length;
    const child = cp.spawn(EXE, ["--test-instance", TAG, "--play", film], { env: testEnv(), cwd: plant, stdio: "ignore", windowsHide: false });
    pid = child.pid;
    let exited = false;
    child.on("exit", () => { exited = true; });
    try {
      await waitFor(() => { const i = info(); return i.ok ? i : null; }, 20000, "the window (planted CWD)");
      cdp = await connect();
      const av = await waitFor(() => N("n.available() && n.backend() === 'native' && n.gen() > 0 ? n.gen() : null"), 25000, "native (planted CWD)");
      if (await N("n.paused()")) { const b = await elRect("tPlay"); if (b) await click(b.x, b.y); }
      const t0 = await N("n.time()");
      const playing = await waitFor(async () => { const t = await N("n.time()"); return !(await N("n.paused()")) && t > t0 + 1500 ? t : null; }, 10000, "playing (planted CWD)");
      const mods = modulesUnder(plant);
      const said = nlog().slice(logBefore).split(/\r?\n/).filter((l) => /DLL search/.test(l));
      log("R15", !!av && !!playing && mods.modules > 50 && mods.hits.length === 0,
          "started IN a folder planted with " + PLANT_NAMES.length + " DLLs (Explorer's --play: CWD = the film's folder): the film plays in mpv, NO module of the app / WebView2 / the engine comes from it",
          { gen: av, t: playing, modulesSeen: mods.modules, fromPlant: mods.hits.slice(0, 5) });
      log("R15", said.some((l) => /DLL search locked/.test(l)), "...native.log says the DLL search is locked", said.slice(-1));
    } catch (e) {
      log("R15", false, "the planted-CWD run stopped: " + e.message);
    }
    if (cdp) cdp.close();
    tool(["close", pid, TAG]);
    for (let t = 0; t < 150 && !exited; ++t) await sleep(100);
    if (!exited) cp.spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], { windowsHide: true });   // (ours only)
    log("R15", exited, "...and it closes");
  } catch (e) {
    log("R15", false, "the planted folder could not be made: " + e.message);
  }
}
main().catch((e) => { console.log("native_real: " + e.stack); if (pid) cp.spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], { windowsHide: true }); process.exit(1); });

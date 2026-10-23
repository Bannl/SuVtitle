// check_we.js <run>... -- judges the we_e2e runs mechanically (runs\<run>.jsonl + its screenshots). Default: all 14 runs
// (V C X P K A N W F T H D R Z -- 12 of them were missing until T-E2E, RV4-11).
// Every run: a clean exit 0, no JS exception / console error / waitfor timeout / dead process, focus NEVER stolen, the
// window on the SECOND screen (found by POSITION: a non-primary monitor holds the whole window -- the device name changes
// across reboots), every screenshot written (a whole PNG, from this run), rendered on the GPU unless WE_CPU=1, the product's
// three browser switches present (Z: absent, it is the product's retry-without environment). Then what each run is for.
// Exit 0 when every run passed.
// PORTED from AudioPB 641f1aa's scratchpad\filemode_2026-10-10\we_e2e\check_we.py (frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\), in node.
//   S-WEB 2026-10-23: runs V and F. F2's faded transport is 0.01, not 0 (AudioPB 1.0.6, user report 11/10: at 0 the compositor
//   drops the bar's quads and, on his AMD driver, the subtitles over the hardware video plane stop being presented).
//   T-E2E 2026-10-23 (RV4-11): C X P K A N W T H D R Z. Changed: the hosts are read from runs\hosts.json (make_we.js computes
//   them with hosts.js = the product's formula; they were hard-coded); C also checks the SIZE (the python clicked buttons the
//   page no longer has); T drives the ⋯ menu's theme toggle and the reload; D's "no path" check names the dropped paths.
//   DROPPED with the runs: L / LM ("Live stays Live" -- no Live mode in SuVtitle).
const fs = require("fs");
const path = require("path");

const HERE = __dirname;
// WE_RUNS=<folder>: judge the runs of another folder (mutants_we.js writes the mutant pages' runs elsewhere); hosts.json stays the kit's
const RUNS = process.env.WE_RUNS ? path.resolve(process.env.WE_RUNS) : path.join(HERE, "runs");
const HOSTS = (() => { try { return JSON.parse(fs.readFileSync(path.join(HERE, "runs", "hosts.json"), "utf8")); } catch { return {}; } })();
const E01_HOST = HOSTS.E01 || "(runs\\hosts.json missing: run make_we.js)";
const E02_HOST = HOSTS.E02 || "(runs\\hosts.json missing: run make_we.js)";
const ALL = ["V", "C", "X", "P", "K", "A", "N", "W", "F", "T", "H", "D", "R", "Z"];
const SWITCHES = ["--autoplay-policy=no-user-gesture-required", "--disable-direct-composition-video-overlays", "--enable-blink-features=AudioVideoTracks"];

function load(name) {
  const recs = [], page = [], evals = {};
  fs.readFileSync(path.join(RUNS, name + ".jsonl"), "utf8").split("\n").forEach((line, i) => {
    line = line.trim();
    if (!line) return;
    const o = JSON.parse(line);
    o._i = i;
    recs.push(o);
    if (!("__host" in o)) page.push(o);
    else if (o.__host === "eval" && typeof o.result === "string" && o.result.indexOf(" {") >= 0) {
      const at = o.result.indexOf(" ");
      try { evals[o.result.slice(0, at)] = JSON.parse(o.result.slice(at + 1)); } catch { /* not a state line */ }
    }
  });
  return { recs, page, evals };
}

function pngWhole(p) {
  let b;
  try { b = fs.readFileSync(p); } catch { return false; }
  const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  return b.length > 1000 && b.subarray(0, 8).equals(sig) && b.readUInt32BE(b.length - 12) === 0
    && b.subarray(b.length - 8, b.length - 4).toString("latin1") === "IEND";
}

function generic(name, recs) {
  const out = [];
  const host = (k) => recs.filter((r) => r.__host === k);
  const start = host("start");
  out.push(["started", start.length > 0]);
  if (start.length) {
    const s = start[0];
    const sec = (s.monitors || []).filter((m) => !m.primary);
    // the whole window inside one NON-primary monitor (by position, never by device name), at the placement the tool was asked for
    const inside = sec.some((m) => s.x >= m.x && s.y >= m.y && s.x + s.w <= m.x + m.w && s.y + s.h <= m.y + m.h);
    out.push(["on the SECOND screen, by position (window " + s.w + "x" + s.h + " at " + s.x + "," + s.y + ", placement " + s.placement + ", monitors "
              + (s.monitors || []).map((m) => m.x + "," + m.y + " " + m.w + "x" + m.h + (m.primary ? " primary" : "")).join(" | ") + ")",
              s.placement === "pos" && inside]);
    const args = s.browserArgs || "";
    const cpu = args.indexOf("--disable-gpu") >= 0, wantCpu = process.env.WE_CPU === "1";
    out.push(["rendering: " + (cpu ? "software (WE_CPU=1)" : "GPU"), cpu === wantCpu]);
    if (name === "Z") {
      out.push(["Z: NONE of the product's switches (app\\main.cpp's retry-without environment), and never --disable-direct-composition",
                SWITCHES.every((w) => args.indexOf(w) < 0) && !/--disable-direct-composition(\s|$)/.test(args)]);
    } else {
      out.push(["the product's switches (autoplay, video overlays off, AudioVideoTracks; never --disable-direct-composition alone)",
                SWITCHES.every((w) => args.indexOf(w) >= 0) && !/--disable-direct-composition(\s|$)/.test(args)]);
    }
  }
  const view = host("view");
  out.push(["muted (the test WebView never makes a sound)", view.length > 0 && view[0].muted === true]);
  const ex = host("exit");
  out.push(["exit 0", ex.length > 0 && ex[ex.length - 1].code === 0]);
  const foc = host("focus");
  out.push(["focus never stolen (" + foc.length + " checks)", foc.length > 0 && foc.every((f) => f.stolen === false)]);
  const bad = [...host("exception"), ...host("waitfor-timeout"), ...host("timeout"), ...host("process-failed"), ...host("env-error")];
  out.push(["no exception / waitfor timeout / dead process" + (bad.length ? " -> " + JSON.stringify(bad).slice(0, 300) : ""), bad.length === 0]);
  const errs = host("console").filter((r) => r.level === "error" || r.level === "warning")
    .concat(host("log").filter((r) => r.level === "error" && !String(r.url || "").endsWith("/favicon.ico")));
  out.push(["no console/log error (favicon 404 aside)" + (errs.length ? " -> " + JSON.stringify(errs).slice(0, 300) : ""), errs.length === 0]);
  // a gesture that threw (a stale selector: the python kit's C run clicked buttons the page no longer had and nobody saw), found
  // nothing ("NOT FOUND"), or an eval that came back null (it threw) would be a run that did less than it says
  const lost = [];
  host("gesture").forEach((r) => {
    const res = r.result || {}, v = res.result && res.result.value;
    if (r.hr || res.exceptionDetails || (typeof v === "string" && /^NOT FOUND/.test(v))) lost.push(r);
  });
  host("eval").forEach((r) => { if (r.hr || typeof r.result !== "string") lost.push(r); });
  host("cdp").forEach((r) => { if (r.hr) lost.push(r); });
  out.push(["every gesture / eval / cdp call of the feed worked and found its target" + (lost.length ? " -> " + JSON.stringify(lost).slice(0, 300) : ""), lost.length === 0]);
  const jsonlMtime = fs.statSync(path.join(RUNS, name + ".jsonl")).mtimeMs;
  for (const sh of host("shot")) {
    const p = path.resolve(HERE, sh.path || "");
    const fresh = fs.existsSync(p) && fs.statSync(p).mtimeMs >= jsonlMtime - 180000;
    out.push(["screenshot " + path.basename(p), sh.ok === true && pngWhole(p) && fresh]);
  }
  return out;
}

const langs = (e) => [...new Set((e.lines || []).map((l) => l.lang))].sort();
const has = (a, x) => Array.isArray(a) && a.indexOf(x) >= 0;
const px = (s) => parseFloat(s) || 0;

function expect(name, recs, page, ev) {
  const E = (k) => ev[k] || {};
  const c = [];
  if (name === "V") {
    c.push(["V1: the player, E01 on its folder host, decoded (videoWidth 1280, no media error)",
            E("V1").mode === "file" && (E("V1").vsrc || "").indexOf(E01_HOST) >= 0 && E("V1").vw === 1280 && E("V1").vErr === 0]);
    c.push(["V1: OUR overlay -- the translation (zh-CN) over the original (en)", JSON.stringify(langs(E("V1"))) === JSON.stringify(["en", "zh-CN"])]);
    c.push(["V1: the queue: 4 rows, the current one marked", (E("V1").queue || []).length === 4 && E("V1").queue[0].endsWith("*")]);
    c.push(["V3 playing, V4 paused (the transport)", E("V3").vPaused === false && E("V4").vPaused === true]);
  } else if (name === "P") {
    c.push(["P1: armed, not playing: the card says what it waits for (+ Play now)",
            E("P1").vPaused === true && (E("P1").ready || "").indexOf("[Play now]") >= 0]);
    c.push(["P2: 3 of 15 s prepared, still waiting", E("P2").vPaused === true && (E("P2").ready || "").indexOf("3 of 15") >= 0]);
    c.push(["P3: covered to the end -> it STARTED BY ITSELF, the card gone", E("P3").vPaused === false && E("P3").ready === ""]);
  } else if (name === "K") {
    c.push(["K1: played up to the end of the subtitles -> PAUSED, 'Translating ahead'",
            E("K1").vPaused === true && (E("K1").ready || "").indexOf("Translating ahead") >= 0]);
    c.push(["K2: the subtitles grew -> playing again, the card gone", E("K2").vPaused === false && E("K2").ready === ""]);
  } else if (name === "R") {
    c.push(["R0: E01 from folder A", (E("R0").vsrc || "").indexOf(E01_HOST) >= 0]);
    c.push(["R1: after the RELOAD, E02 from folder B's host, resumed and PLAYING",
            (E("R1").vsrc || "").indexOf(E02_HOST) >= 0 && E("R1").resume === "Resumed at 0:04" && E("R1").vPaused === false && E("R1").vErr === 0]);
    c.push(["R1: the queue's current item moved to E02", (E("R1").queue || []).some((q) => q.startsWith("E02") && q.endsWith("*"))]);
    c.push(["R: the reloaded page said `ready` with its tab (view:file)", page.some((p) => p.type === "ready" && p.view === "file")]);
    c.push(["R: the page loaded three times and said `ready` each time (boot, E01's folder, then the reload for E02's folder B)",
            page.filter((p) => p.type === "ready").length >= 3]);
  } else if (name === "D") {
    const mo = recs.filter((r) => r.__host === "msg-objects");
    const drops = page.filter((p) => p.type === "translatorFileDrop");
    c.push(["D: a FOLDER + a .srt dropped on the picture -> their REAL paths reach the host, target play",
            mo.length >= 1 && (mo[0].files || []).length === 2 && mo[0].files[0].endsWith("Season 1")
            && mo[0].files[1].endsWith("E01 Pilot.fr.srt") && drops.length > 0 && drops[0].target === "play"]);
    c.push(["D: a file dropped on the docked queue -> target queue (its real path)",
            mo.length >= 2 && ((mo[1].files || [""])[0] || "").endsWith("E02 The Bridge.mp4") && drops.length >= 2 && drops[1].target === "queue"]);
    const dropped = mo.flatMap((m) => m.files || []);
    c.push(["D: no path in the page's own messages (the page never names a file: only type / quality / target, no backslash, no leaf)",
            drops.length >= 2 && drops.every((p) => {
              const s = JSON.stringify(p);
              return Object.keys(p).filter((k) => k !== "_i").every((k) => ["type", "quality", "target"].indexOf(k) >= 0) && s.indexOf("\\") < 0
                && dropped.every((f) => s.toLowerCase().indexOf(path.basename(f).toLowerCase()) < 0);
            })]);
  } else if (name === "C") {
    const f0 = px(E("C0").subFont), f2 = px(E("C2").subFont);
    c.push(["C1: the CC panel open, both languages chosen (the first source row)", Array.isArray(E("C1").cc) && E("C1").cc.length > 0 && E("C1").cc[0].endsWith("*")]);
    c.push(["C2: the custom style applied: yellow text, at the TOP of the picture",
            E("C2").subTextColor === "rgb(255, 225, 77)" && (E("C2").ovBox || [0, 999])[1] < (E("C2").viewBox || [0, 0])[1] + 40]);
    c.push(["C2: size L -- the slider at 7 % against 4 %: the subtitle grew by about 7/4 (" + f0 + "px -> " + f2 + "px)",
            f0 > 0 && f2 / f0 > 1.6 && f2 / f0 < 1.9]);
  } else if (name === "X") {
    const k = (E("X1").lines || []).map((l) => l.kids || []);
    c.push(["X1: the .srt parsed by the app: italic + bold runs, the overlap (2 cues = 3 lines)",
            k.length === 3 && has(k[0], "SPAN.r-i") && has(k[2], "SPAN.r-b")]);
    c.push(["X1: picked from the CC sources (the page asked playerSubGet 31), its encoding in the toast",
            page.some((p) => p.type === "playerSubGet" && p.id === 31) && (E("X1").toast || "").indexOf("Windows-1252") >= 0]);
  } else if (name === "A") {
    c.push(["A1: a sound file -> the <audio>, the card, no picture",
            (E("A1").asrc || "").indexOf("Interview.mp3") >= 0 && E("A1").vHidden === true && E("A1").bigHidden === false]);
  } else if (name === "N") {
    c.push(["N1: 420x320, nothing scrolls sideways, the drawer closed, the transport inside the window",
            E("N1").w === 420 && E("N1").overflowX === false && E("N1").panelShown === false
            && ((E("N1").transport || [0, 0, 0, 999])[1] + (E("N1").transport || [0, 0, 0, 999])[3]) <= 320]);
    c.push(["N2: the more menu holds the rest (speed ... Cancel the analysis) and the default-player + theme items",
            has(E("N2").more, "Cancel the analysis") && (E("N2").more || []).some((x) => /^Speed/.test(x)) && has(E("N2").more, "Light theme")
            && has(E("N2").more, "Make SuVtitle the default player…")]);
  } else if (name === "W") {
    c.push(["W1: 1280 wide: the queue docked beside the picture, two ranges on the seek bar",
            E("W1").panelShown === true && E("W1").tabQueue === "true" && (E("W1").ranges || []).length === 2
            && (E("W1").viewBox || [0, 0, 9999])[2] < 900]);
  } else if (name === "F") {
    const asks = page.filter((p) => p.type === "translatorFullscreen").map((p) => p.on);
    c.push(["F: the page ASKS the window full screen (F) and back (Esc)", JSON.stringify(asks.slice(0, 2)) === JSON.stringify([true, false])]);
    c.push(["F1: granted: bar, strip and panel gone", (E("F1").body || "").indexOf("is-full") >= 0 && E("F1").barShown === false]);
    c.push(["F2: playing and still -> the transport fades (to 0.01: still there for the compositor)",
            E("F2").vPaused === false && E("F2").transportOpacity === "0.01"]);
    c.push(["F3: left full screen", (E("F3").body || "x").indexOf("is-full") < 0]);
  } else if (name === "T") {
    c.push(["T0: dark by default, nothing stored yet", E("T0").theme === "dark" && E("LS0").theme === null && E("LS0").attr === "dark"]);
    c.push(["T0m: the ⋯ menu offers \"Light theme\" (and the default-player item)",
            has(E("T0m").more, "Light theme") && !has(E("T0m").more, "Dark theme") && has(E("T0m").more, "Make SuVtitle the default player…")]);
    c.push(["T1: a click on \"Light theme\" -> data-theme light, remembered (localStorage patchbus.theme = light)",
            E("T1").theme === "light" && E("LS1").theme === "light" && E("LS1").attr === "light"]);
    c.push(["T1m: the item now reads \"Dark theme\"", has(E("T1m").more, "Dark theme") && !has(E("T1m").more, "Light theme")]);
    c.push(["T2: after a page RELOAD (what a new folder costs) the theme is still light", E("T2").theme === "light" && E("LS2").theme === "light"]);
    c.push(["T3: \"Dark theme\" brings dark back, remembered", E("T3").theme === "dark" && E("LS3").theme === "dark" && E("LS3").attr === "dark"]);
  } else if (name === "H") {
    c.push(["H1: '?' opens the keys' help", E("H1").help === true]);
  } else if (name === "Z") {
    c.push(["Z1: no autoplay switch: play() refused -> 'Ready to play [Play]', not silence",
            E("Z1").vPaused === true && E("Z1").ready === "Ready to play [Play]"]);
    c.push(["Z2: the click plays", E("Z2").vPaused === false && E("Z2").ready === ""]);
  }
  return c;
}

let allOk = true;
const names = process.argv.slice(2).length ? process.argv.slice(2) : ALL;
for (const name of names) {
  let run;
  try { run = load(name); } catch (e) { console.log("== " + name + ": no run (" + e.message + ")"); allOk = false; continue; }
  const checks = generic(name, run.recs).concat(expect(name, run.recs, run.page, run.evals));
  const ok = checks.every(([, v]) => v);
  console.log("== " + name + ": " + (ok ? "PASS" : "FAIL"));
  for (const [n, v] of checks) console.log("   " + (v ? "ok  " : "FAIL") + "  " + n);
  allOk = allOk && ok;
}
console.log("CHECK_WE " + (allOk ? "OK" : "FAILED"));
process.exit(allOk ? 0 : 1);

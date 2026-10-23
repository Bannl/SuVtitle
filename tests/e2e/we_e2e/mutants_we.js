// mutants_we.js -- the we_e2e judges must be ABLE TO FAIL (T-E2E, 2026-10-23, RV4-11). For each mutant: a COPY of app\web with ONE guard of
// the page undone (the replacement must apply exactly once), the run that exercises that guard replayed on it in a real WebView2
// (wv2probe, 2nd screen, muted, never focused -- the same command line as run_we.cmd), and check_we.js required to FAIL on the check
// named. A run that cannot be made to fail by its own guard is not testing it. Nothing of runs\ is touched: the mutant runs live
// in scratchpad\t-e2e\we_mut\ (git-ignored; WE_MUT_DIR overrides) and are judged there (WE_RUNS).
//   node mutants_we.js [id...]        needs runs\feed_*.jsonl (run make_we.js first) and ..\wv2probe\wv2probe.exe. ~7 s per mutant.
// The mutants are the page's own words, not test-only switches: the page served is app\web with a string replaced in a scratch copy.
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..", "..");
const WEBSRC = path.join(REPO, "app", "web");
const WV = path.join(HERE, "..", "wv2probe", "wv2probe.exe");
const SCRATCH = process.env.WE_MUT_DIR ? path.resolve(process.env.WE_MUT_DIR) : path.join(REPO, "scratchpad", "t-e2e", "we_mut");
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays --enable-blink-features=AudioVideoTracks";
const SIZE = { N: "420x320", W: "1280x800", D: "1280x800", F: "1600x900" };

// id, the run, the page replacement [from, to] (exactly once) or an environment override, and the check that must FAIL (substring)
const MUTANTS = [
  { id: "V-lang", run: "V", why: "the overlay's lines lose their lang attribute (the right CJK face on screen)", edit: ['if (t) node.setAttribute("lang", t); else node.removeAttribute("lang");', 'node.removeAttribute("lang");'], fails: "V1: OUR overlay" },
  { id: "F-ask", run: "F", why: "the F key asks for full screen OFF (the page never asks the window)", edit: ['send({ type: "translatorFullscreen", on: !isFull });', 'send({ type: "translatorFullscreen", on: false });'], fails: "F: the page ASKS" },
  { id: "F-fade", run: "F", why: "the still transport fades to 0, not 0.01 (the 1.0.6 AMD bug: the compositor drops the subtitles over the video plane)", edit: ["body.is-full .file-stage.is-idle .transport { opacity: 0.01; pointer-events: none; }", "body.is-full .file-stage.is-idle .transport { opacity: 0; pointer-events: none; }"], fails: "F2:" },
  { id: "P-lead", run: "P", why: "play-when-ready lead 15 s -> 1 s: the film starts with 3 s prepared", edit: ["const READY_LEAD_MS = 15000;", "const READY_LEAD_MS = 1000;"], fails: "P2:" },
  { id: "K-stall", run: "K", why: "keep-ahead never stalls", edit: ["if (leadAt(F.ranges, t) < STALL_MS) {", "if (false) {"], fails: "K1:" },
  { id: "C-place", run: "C", why: "the Place buttons do nothing", edit: ['segClick(el.ccPlace, "place");', ""], fails: "C2: the custom style" },
  { id: "C-size", run: "C", why: "the size slider does nothing", edit: ['el.ccSize.addEventListener("input", () => setSubK(el.ccSize.value));', ""], fails: "C2: size L" },
  { id: "X-italic", run: "X", why: "an italic run of a subtitle file is drawn plain", edit: ['r.flags & 1 ? "r-i" : ""', 'r.flags & 1 ? "" : ""'], fails: "X1: the .srt parsed" },
  { id: "H-help", run: "H", why: "? no longer opens the help", edit: ['if (k === "?") { e.preventDefault(); togglePop(el.helpPanel, el.tHelp); return; }', ""], fails: "H1:" },
  { id: "N-narrow", run: "N", why: "the narrow layout never engages", edit: ['document.body.classList.toggle("is-narrow", W <= 560 || H <= 420);', 'document.body.classList.toggle("is-narrow", W <= 1);'], fails: "N2:" },
  { id: "W-wide", run: "W", why: "the panel never docks", edit: ['document.body.classList.toggle("is-wide", W >= 1100 && H >= 600);', 'document.body.classList.toggle("is-wide", W >= 99999);'], fails: "W1:" },
  { id: "D-queue", run: "D", why: "a drop on the queue says target play", edit: ['target: queue ? "queue" : "play"', 'target: "play"'], fails: "D: a file dropped on the docked queue" },
  { id: "R-resume", run: "R", why: "the resume toast's words change", edit: ['"Resumed at " + fmtClock(F.startMs)', '"Resuming at " + fmtClock(F.startMs)'], fails: "R1: after the RELOAD" },
  { id: "A-card", run: "A", why: "the sound card is never shown", edit: ['el.fileBig.hidden = role === "video";', "el.fileBig.hidden = true;"], fails: "A1:" },
  { id: "T-persist", run: "T", why: "the theme is not remembered", edit: ['try { localStorage.setItem(THEME_KEY, t); } catch { /* private mode: for this session only */ }', ""], fails: "T1:" },
  { id: "Z-switches", run: "Z", why: "ENVIRONMENT, not the page: Z started WITH the product's switches (autoplay allowed: no refused play())", args: SWITCHES, fails: "Z1:" },
];

const only = process.argv.slice(2);
fs.mkdirSync(SCRATCH, { recursive: true });
let caught = 0, missed = 0;
for (const m of MUTANTS) {
  if (only.length && only.indexOf(m.id) < 0) continue;
  const dir = path.join(SCRATCH, "web_" + m.id), cwd = path.join(SCRATCH, "cwd_" + m.id);
  fs.rmSync(dir, { recursive: true, force: true }); fs.rmSync(cwd, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true }); fs.mkdirSync(path.join(cwd, "runs"), { recursive: true });
  for (const f of fs.readdirSync(WEBSRC)) fs.copyFileSync(path.join(WEBSRC, f), path.join(dir, f));
  if (m.edit) {
    const page = fs.readFileSync(path.join(dir, "index.html"), "utf8").replace(/\r\n/g, "\n");
    const n = page.split(m.edit[0]).length - 1;
    if (n !== 1) { console.log("FAIL   " + m.id + ": the replacement applies " + n + " times (must be exactly 1): " + m.edit[0].slice(0, 70)); missed++; continue; }
    fs.writeFileSync(path.join(dir, "index.html"), page.replace(m.edit[0], () => m.edit[1]), "utf8");
  }
  const bargs = m.args !== undefined ? m.args : SWITCHES;
  const argv = ["--url", "https://suvtitle.local/index.html", "--serve", "suvtitle.local=" + dir, "--product-settings", "--no-smartscreen",
    "--feed", path.join(HERE, "runs", "feed_" + m.run + ".jsonl"), "--out", path.join(cwd, "runs", m.run + ".jsonl"), "--udf", path.join(cwd, "udf"),
    "--size", SIZE[m.run] || "1000x720", "--timeout", "90000", "--quiet"];
  if (bargs) argv.push("--browser-args", bargs);
  const run = spawnSync(WV, argv, { cwd, encoding: "utf8", timeout: 120000 });
  const judged = spawnSync(process.execPath, [path.join(HERE, "check_we.js"), m.run], { encoding: "utf8", env: Object.assign({}, process.env, { WE_RUNS: path.join(cwd, "runs") }) });
  const failing = (judged.stdout || "").split("\n").filter((l) => /^ {3}FAIL /.test(l)).map((l) => l.replace(/^ {3}FAIL\s+/, "").trim());
  const hit = failing.filter((n) => n.indexOf(m.fails) >= 0);
  if (judged.status === 1 && hit.length) { caught++; console.log("caught " + m.id.padEnd(11) + "[" + m.run + "] " + m.why + "  -> FAIL " + JSON.stringify(hit[0].slice(0, 90))); }
  else { missed++; console.log("FAIL   " + m.id.padEnd(11) + "[" + m.run + "] SURVIVES (probe exit " + run.status + ", judge exit " + judged.status + "; failing: " + JSON.stringify(failing).slice(0, 200) + ")"); }
}
console.log((missed ? "MUTANTS_WE FAILED" : "MUTANTS_WE OK") + " -- " + caught + " caught, " + missed + " not");
process.exit(missed ? 1 : 0);

// mutants_wg.js -- the wg_e2e judges must be ABLE TO FAIL (T-WG, 2026-10-23; the pattern of ..\we_e2e\mutants_we.js). For each mutant: a COPY of
// app\web with ONE guard of the page undone (every replacement must apply exactly once), the run that exercises that guard replayed on it in
// a real WebView2 (wv2probe, 2nd screen, muted, never focused -- the same command line as run_wg.cmd), and check_wg.js required to FAIL on the
// check named. Before a run's first mutant, the UNMODIFIED copy of the page is replayed through the very same harness and must PASS in full
// (a control: a "caught" is then the mutation's doing, not the harness's -- a missing screenshot, a wrong cwd...). Nothing of runs\ is touched:
// the mutant runs live in scratchpad\t-wg\wg_mut\ (git-ignored; WG_MUT_DIR overrides) and are judged there (WG_RUNS).
//   node mutants_wg.js [id...]      needs runs\feed_*.jsonl (run make_wg.js first) and ..\wv2probe\wv2probe.exe. ~7 s per mutant.
//   node mutants_wg.js --dry        applies every replacement to the page text only (no WebView): each must apply exactly once
//   node mutants_wg.js --list       the mutants
// The mutants are the page's own words, not test-only switches: the page served is app\web with a string replaced in a scratch copy
// (or, for the ENVIRONMENT / FEED mutants of the judge's own generic block -- no switches, Chromium off the GPU, an unmuted WebView, a gesture
// that throws, a console error, a screenshot that cannot be written -- the unmodified page run another way). The mutants use the page's
// real CSS and code lines as they stand: when a page edit renames one, `--dry` says which (and the mutant is to be re-anchored).
// The layout sentinels G2M / G8M (run_wg.cmd, check_wg.js) are a second, older proof for G2's and G8's layout checks; the G2-times /
// G2-panel / G2-list / G8-wrap mutants here prove the same checks against the page's real CSS, one rule at a time.
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..", "..");
const WEBSRC = path.join(REPO, "app", "web");
const WV = path.join(HERE, "..", "wv2probe", "wv2probe.exe");
const SCRATCH = process.env.WG_MUT_DIR ? path.resolve(process.env.WG_MUT_DIR) : path.join(REPO, "scratchpad", "t-wg", "wg_mut");
const SWITCHES = "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays --enable-blink-features=AudioVideoTracks";
const SIZE = { G5: "420x320", G9: "1280x800" };
const U = {                                       // the glyphs of the page's words, by code point (no raw special character in this file)
  dot: "\u00b7", dots: "\u2026" };

// id, the run, why, the page replacement(s) [from, to] (each exactly once) and/or a feed transform / an environment override,
// and the check that must FAIL (substring of its name)
const MUTANTS = [
  // ---- G1: the picture first ----
  { id: "G1-bar", run: "G1", why: "the bar is no longer one slim row", edit: ["body.in-file #bar { padding: 5px 10px 6px; gap: 6px 8px; align-items: center; }", "body.in-file #bar { padding: 40px 10px 40px; gap: 6px 8px; align-items: center; }"], fails: "G1a: File mode's bar is ONE slim row" },
  { id: "G1-fold", run: "G1", why: "the list opens unfolded by default", edit: ["open: p.open === true }; })();", "open: p.open !== false }; })();"], fails: "G1a: the list FOLDED" },
  { id: "G1-fade", run: "G1", why: "the bar fades after 2.5 s still (the Live window's idle fade the user did not want)", edit: ["#bar::after {", "#bar { animation: mut-fade 0.2s 2.5s forwards; } @keyframes mut-fade { to { opacity: 0.2; } } #bar::after {"], fails: "G1b: 4 s later" },
  { id: "G1-picture", run: "G1", why: "the strip balloons: the picture no longer comes first", edit: ["flex: 0 0 auto; padding: 4px 8px 4px 10px; font-size: 13px; color: var(--dim);", "flex: 0 0 auto; padding: 160px 8px 4px 10px; font-size: 13px; color: var(--dim);"], fails: "G1a: the picture first" },
  { id: "G1-brand", run: "G1", why: "the page is not SuVtitle's (its title)", edit: ["<title>SuVtitle</title>", "<title>AudioPB Translator</title>"], fails: "G1a: it IS the SuVtitle page" },
  // ---- G2: the editor ----
  { id: "G2-key", run: "G2", why: "E no longer opens the editor", edit: ["{ e.preventDefault(); edToggle(); return; }", "{ e.preventDefault(); return; }"], fails: "G2a: E (a real key) opened the editor" },
  { id: "G2-target", run: "G2", why: "an edit of the analysed set is sent to the wrong target", edit: ['const target = edIsExt() ? "ext" : "audiopb";', 'const target = edIsExt() ? "ext" : "set";'], fails: "G2: the edit SENT to the core" },
  { id: "G2-apply", run: "G2", why: "the core's answer is never applied", edit: ["applyEdited(msg);", "/* mutant: the core's answer is dropped */"], fails: "G2b: the core's answer applied" },
  { id: "G2-times", run: "G2", why: "Start and End stack instead of sitting side by side (the pre-fix CSS, one rule)", edit: [".ed-times, .ed-texts { display: flex; flex-wrap: wrap; gap: 5px 22px; min-width: 0; }", ".ed-times, .ed-texts { display: block; }"], fails: "G2a: Start and End side by side" },
  { id: "G2-panel", run: "G2", why: "the docked panel keeps its 34 % while editing: the editor scrolls in itself (the pre-fix CSS)", edit: ["body.is-editing:not(.is-narrow):not(.is-wide) .file-panel { flex: 0 0 auto; max-height: 62%; }", "body.is-editing:not(.is-narrow):not(.is-wide) .file-panel { flex: 0 1 34%; }"], fails: "G2a: in a 1000 x 720 window the WHOLE editor" },
  { id: "G2-list", run: "G2", why: "the list under the editor is squeezed to nothing", edit: ["body.is-editing .file-list { flex: 1 1 0; min-height: 96px; }", "body.is-editing .file-list { flex: 0 0 0; min-height: 0; }"], fails: "G2a: the list keeps >= 90 px" },
  // ---- G3: a .srt dropped on the playing video ----
  { id: "G3-target", run: "G3", why: "a drop on the picture says target queue", edit: ['target: queue ? "queue" : "play" }, take)', 'target: "queue" }, take)'], fails: "G3: a .srt dropped on the PLAYING video" },
  { id: "G3-nameless", run: "G3", why: "the page's drop message names the dropped file (a path/leaf leak)", edit: ['target: queue ? "queue" : "play" }, take)', 'target: queue ? "queue" : "play", name: take[0].name }, take)'], fails: "G3: the page itself names no file" },
  { id: "G3-italic", run: "G3", why: "an italic run of a subtitle file is drawn plain", edit: ['r.flags & 1 ? "r-i" : ""', 'r.flags & 1 ? "" : ""'], fails: "G3a: 'Subtitles loaded" },
  { id: "G3-toast", run: "G3", why: "the toast no longer says 'Subtitles loaded:'", edit: ['showFileToast("Subtitles loaded: " + said', 'showFileToast("Subtitle file: " + said'], fails: "G3a: 'Subtitles loaded" },
  { id: "G3-pause", run: "G3", why: "loading a subtitle file pauses the film", edit: ['showFileToast("Subtitles loaded: " + said', 'el.fileVideo.pause(); showFileToast("Subtitles loaded: " + said'], fails: "G3a: still playing" },
  // ---- G4: stacked cues ----
  { id: "G4-stack", run: "G4", why: "the overlay shows one line at most", edit: ["STACK_MAX = 3;", "STACK_MAX = 1;"], fails: "G4a: the user's real short cues STACK" },
  { id: "G4-hold", run: "G4", why: "no cue is held for reading time", edit: ["READ_LATIN_CPS = 17, READ_CJK_CPS = 7, READ_MIN_MS = 1000", "READ_LATIN_CPS = 1000, READ_CJK_CPS = 1000, READ_MIN_MS = 1"], fails: "G4b: at 3.6 s" },
  { id: "G4-orig", run: "G4", why: "the original is no longer drawn under the current line", edit: ["if (cur && t.sub) lines.push(", "if (false && cur && t.sub) lines.push("], fails: "G4a: the current line's original" },
  // ---- G5: the narrow drawer ----
  { id: "G5-narrow", run: "G5", why: "the narrow layout never engages", edit: ['document.body.classList.toggle("is-narrow", W <= 560 || H <= 420);', 'document.body.classList.toggle("is-narrow", W <= 1);'], fails: "G5a: 420 x 320" },
  { id: "G5-drawer", run: "G5", why: "the drawer never opens", edit: ["if (isNarrow()) { drawerOpen = !!open;", "if (isNarrow()) { drawerOpen = false;"], fails: "G5a: 420 x 320" },
  { id: "G5-scroll", run: "G5", why: "the editor is cut instead of scrolling inside the drawer", edit: ["body.is-editing .ed-pane { flex: 0 1 auto; min-height: 0; overflow-y: auto; }", "body.is-editing .ed-pane { flex: 0 1 auto; min-height: 0; overflow-y: hidden; }"], fails: "G5a: every row of the editor reachable" },
  // ---- G6: the default-player action ----
  { id: "G6-item", run: "G6", why: "the more menu loses the default-player item", edit: ['items.push(["Make SuVtitle the default player' + U.dots + '", () => send({ type: "playerSetDefault" })]);', ""], fails: "G6a: " },
  { id: "G6-send", run: "G6", why: "the item does nothing", edit: ['() => send({ type: "playerSetDefault" })]);', "() => {}]);"], fails: "G6: the click asks the app" },
  { id: "G6-toast", run: "G6", why: "the app's notice is never shown", edit: ["showFileToast(cleanNotice(text), !msg.info);", "/* mutant: the notice is dropped */"], fails: "G6b: the app's answer said in the toast" },
  // ---- G7: a subtitle file in the editor ----
  { id: "G7-target", run: "G7", why: "an edit of a subtitle FILE goes to the wrong target", edit: ['const target = edIsExt() ? "ext" : "audiopb";', 'const target = edIsExt() ? "file" : "audiopb";'], fails: "G7: a subtitle FILE's edit sent to the app" },
  { id: "G7-bak", run: "G7", why: "the save toast stops saying the original is kept", edit: [" " + U.dot + " the original is kept as ", " " + U.dot + " backup: "], fails: "G7a: applied" },
  { id: "G7-saveas", run: "G7", why: "no Save as... for a subtitle file", edit: ["el.edSaveAs.hidden = !ext_ || ro;", "el.edSaveAs.hidden = true;"], fails: "G7a: a file has ONE text" },
  { id: "G7-markup", run: "G7", why: "the field shows a file's line without its .srt markup", edit: ['const edTextOf = (c) => (typeof c.t === "string" ? c.t : runsToTagged(c.r));', "const edTextOf = (c) => (c.r || []).map((x) => x[0]).join(\"\");"], fails: "G7a: the field shows the edited line AS .srt markup" },
  // ---- G8: the long film name ----
  { id: "G8-wrap", run: "G8", why: "the status wraps to a second row again (the pre-fix CSS, one rule)", edit: [".file-status { flex: 1 1 160px; min-width: 0; color: var(--text); font-weight: 500;", ".file-status { white-space: normal !important; overflow: visible !important; flex: 1 1 160px; min-width: 0; color: var(--text); font-weight: 500;"], fails: "G8a: the real film's long name while analysing" },
  { id: "G8-short", run: "G8", why: "the strip names the subtitle files by their whole names", edit: ['nm.startsWith(base + ".") ? nm.slice(base.length) : nm', "nm"], fails: "G8b: done + edited" },
  { id: "G8-tip", run: "G8", why: "the tooltip loses 'filled in as it goes'", edit: ['" (filled in as it goes): "', '": "'], fails: "G8a: the tooltip holds the whole" },
  { id: "G8-edited", run: "G8", why: "the strip stops saying 'edited'", edit: ['if (F.edited) text += " ' + U.dot + ' edited";', 'if (F.edited) text += "";'], fails: "G8b: done + edited" },
  // ---- G9: the wide window ----
  { id: "G9-wide", run: "G9", why: "the panel never docks beside the picture", edit: ['document.body.classList.toggle("is-wide", W >= 1100 && H >= 600);', 'document.body.classList.toggle("is-wide", W >= 99999);'], fails: "G9a: 1280 x 800: the panel DOCKED" },
  { id: "G9-list", run: "G9", why: "the list under the editor is squeezed to nothing", edit: ["body.is-editing .file-list { flex: 1 1 0; min-height: 96px; }", "body.is-editing .file-list { flex: 0 0 0; min-height: 0; }"], fails: "G9a: the list keeps >= 96 px" },
  { id: "G9-editor", run: "G9", why: "the editor is cut to 60 px and scrolls in itself in the docked column", edit: ["body.is-editing .ed-pane { flex: 0 1 auto; min-height: 0; overflow-y: auto; }", "body.is-editing .ed-pane { flex: 0 1 auto; min-height: 0; overflow-y: auto; max-height: 60px; }"], fails: "G9a: the WHOLE editor on screen" },
  { id: "G9-picture", run: "G9", why: "the docked panel takes 70 % of the width: the picture loses its room", edit: ["body.is-wide .file-panel { flex: 0 0 min(380px, 34%); }", "body.is-wide .file-panel { flex: 0 0 70%; }"], fails: "G9a: the picture keeps >= 60 %" },
  // ---- the "nothing scrolls sideways" clause of three runs, and two more checks of G2 / G8
  { id: "G1-scrollx", run: "G1", why: "something 3000 px wide makes the page scroll sideways", edit: ["#bar::after {", 'body::after { content: ""; position: absolute; left: 0; top: 0; width: 3000px; height: 1px; } #bar::after {'], fails: "G1: nothing scrolls sideways" },
  { id: "G5-scrollx", run: "G5", why: "something 3000 px wide makes the narrow page scroll sideways", edit: ["#bar::after {", 'body::after { content: ""; position: absolute; left: 0; top: 0; width: 3000px; height: 1px; } #bar::after {'], fails: "G5a: nothing scrolls sideways" },
  { id: "G9-scrollx", run: "G9", why: "something 3000 px wide makes the wide page scroll sideways", edit: ["#bar::after {", 'body::after { content: ""; position: absolute; left: 0; top: 0; width: 3000px; height: 1px; } #bar::after {'], fails: "G9a: nothing scrolls sideways" },
  { id: "G2-strip", run: "G2", why: "the strip balloons while editing (the picture and the editor are pushed)", edit: ["flex: 0 0 auto; padding: 4px 8px 4px 10px; font-size: 13px; color: var(--dim);", "flex: 0 0 auto; padding: 160px 8px 4px 10px; font-size: 13px; color: var(--dim);"], fails: "G2b: the strip still ONE row" },
  { id: "G8-tip2", run: "G8", why: "the done tooltip loses the subtitle files' names", edit: ['+ cleanLong(F.srtName) : "");', '+ "" : "");'], fails: "G8b: the tooltip names both files whole" },
  { id: "G7-undo", run: "G7", why: "the editor's undo/redo row is not on screen", edit: ["#bar::after {", "#edUndo { display: none !important; } #bar::after {"], fails: "G7a: the editor reachable" },
  { id: "G8-picture", run: "G8", why: "the strip balloons: the picture keeps less than 70 % of the height", edit: ["flex: 0 0 auto; padding: 4px 8px 4px 10px; font-size: 13px; color: var(--dim);", "flex: 0 0 auto; padding: 160px 8px 4px 10px; font-size: 13px; color: var(--dim);"], fails: "G8: the picture keeps >= 70 %" },
  // ---- the judge's own generic block (an environment and a feed mutant: the page is the unmodified one) ----
  { id: "G-switches", run: "G1", why: "ENVIRONMENT: the run started WITHOUT the product's switches", args: "", fails: "the product's switches" },
  { id: "G-gesture", run: "G1", why: "FEED: a gesture that throws (a stale selector) -- the run would silently do less than it says",
    feed: (lines) => { const at = lines.findIndex((l) => l.__exit !== undefined); lines.splice(at, 0, { __gesture: "document.getElementById('no-such-element').click()" }); return lines; },
    fails: "every gesture / eval / cdp call of the feed worked" },
  { id: "G-gpu", run: "G1", why: "ENVIRONMENT: Chromium kept off the GPU while the kit says GPU (rendering is judged)", args: SWITCHES + " --disable-gpu", fails: "rendering: " },
  { id: "G-muted", run: "G1", why: "ENVIRONMENT: the WebView is not muted (G1 plays nothing: no sound is made)", extra: ["--sound"], fails: "muted (the test WebView never makes a sound)" },
  { id: "G-console", run: "G1", why: "FEED: the page logs a console error",
    feed: (lines) => { const at = lines.findIndex((l) => l.__exit !== undefined); lines.splice(at, 0, { __eval: "console.error('mutant: a console error'), 'logged'" }); return lines; },
    fails: "no console/log error" },
  { id: "G-shot", run: "G1", why: "FEED: a screenshot that cannot be written (its path is a directory -- wv2probe creates a missing folder, so a missing one would not do)",
    feed: (lines) => { const at = lines.findIndex((l) => l.__shot !== undefined); lines[at] = { __shot: "runs" }; return lines; },
    fails: "screenshot runs" },
];

// the replacement(s) of a mutant as [[from, to]...]
const editsOf = (m) => (!m.edit ? [] : Array.isArray(m.edit[0]) ? m.edit : [m.edit]);
const lf = (t) => t.replace(/\r\n/g, "\n");
function mutate(page, m) {
  let out = page;
  const bad = [];
  for (const [from, to] of editsOf(m)) {
    const n = out.split(from).length - 1;
    if (n !== 1) { bad.push("applies " + n + " times (must be exactly 1): " + from.slice(0, 70)); continue; }
    out = out.replace(from, () => to);
  }
  return { page: out, bad };
}

const args = process.argv.slice(2);
if (args.includes("--list")) { MUTANTS.forEach((m) => console.log(m.id.padEnd(11) + "[" + m.run + "] " + m.why + "  -> FAIL " + JSON.stringify(m.fails))); process.exit(0); }
const dry = args.includes("--dry");
const only = args.filter((a) => !a.startsWith("--"));

if (dry) {
  const page = lf(fs.readFileSync(path.join(WEBSRC, "index.html"), "utf8"));
  let bad = 0;
  for (const m of MUTANTS) {
    if (only.length && only.indexOf(m.id) < 0) continue;
    const r = mutate(page, m);
    if (r.bad.length) { bad++; console.log("FAIL   " + m.id + ": " + r.bad.join(" | ")); }
    else if (editsOf(m).length && r.page === page) { bad++; console.log("FAIL   " + m.id + ": the replacement changed nothing"); }
    else console.log("ok     " + m.id.padEnd(11) + "[" + m.run + "] " + (editsOf(m).length ? editsOf(m).length + " replacement(s), each once" : m.args !== undefined || m.extra ? "environment mutant" : "feed mutant"));
  }
  console.log(bad ? "MUTANTS_WG --dry FAILED -- " + bad + " bad" : "MUTANTS_WG --dry OK -- every replacement applies exactly once");
  process.exit(bad ? 1 : 0);
}

fs.mkdirSync(SCRATCH, { recursive: true });
let caught = 0, missed = 0, baselinesOk = 0, baselinesBad = 0;
const baselined = {};

// one replay: a scratch copy of the page (mutated or not), the run's feed (transformed or not), wv2probe, then the judge on that folder
function replay(tag, run, pageText, bargs, feedFn, extra) {
  const dir = path.join(SCRATCH, "web_" + tag), cwd = path.join(SCRATCH, "cwd_" + tag);
  fs.rmSync(dir, { recursive: true, force: true }); fs.rmSync(cwd, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true }); fs.mkdirSync(path.join(cwd, "runs"), { recursive: true });
  for (const f of fs.readdirSync(WEBSRC)) fs.copyFileSync(path.join(WEBSRC, f), path.join(dir, f));
  fs.writeFileSync(path.join(dir, "index.html"), pageText, "utf8");
  let feedPath = path.join(HERE, "runs", "feed_" + run + ".jsonl");
  if (feedFn) {
    const lines = fs.readFileSync(feedPath, "utf8").split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));
    feedPath = path.join(cwd, "feed.jsonl");
    fs.writeFileSync(feedPath, feedFn(lines).map((l) => JSON.stringify(l)).join("\n") + "\n", "utf8");
  }
  const argv = ["--url", "https://suvtitle.local/index.html", "--serve", "suvtitle.local=" + dir, "--product-settings", "--no-smartscreen",
    "--feed", feedPath, "--out", path.join(cwd, "runs", run + ".jsonl"), "--udf", path.join(cwd, "udf"),
    "--size", SIZE[run] || "1000x720", "--timeout", "90000", "--quiet"];
  if (bargs) argv.push("--browser-args", bargs);
  if (extra) argv.push(...extra);
  const probe = spawnSync(WV, argv, { cwd, encoding: "utf8", timeout: 120000 });
  const judged = spawnSync(process.execPath, [path.join(HERE, "check_wg.js"), run], { encoding: "utf8", env: Object.assign({}, process.env, { WG_RUNS: path.join(cwd, "runs") }) });
  const failing = (judged.stdout || "").split("\n").filter((l) => /^ {3}FAIL /.test(l)).map((l) => l.replace(/^ {3}FAIL\s+/, "").trim());
  // the WebView2 profile (~9 MB per replay) is of no use after the judge; the run's out file and screenshots stay in cwd_<id>\runs for a look
  try { fs.rmSync(path.join(cwd, "udf"), { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch { /* a profile still held: left, git-ignored scratch */ }
  return { probe, judged, failing };
}

const realPage = lf(fs.readFileSync(path.join(WEBSRC, "index.html"), "utf8"));
for (const m of MUTANTS) {
  if (only.length && only.indexOf(m.id) < 0) continue;
  const mut = mutate(realPage, m);
  if (mut.bad.length) { console.log("FAIL   " + m.id + ": " + mut.bad.join(" | ")); missed++; continue; }
  if (!baselined[m.run]) {                  // the control: the unmodified page through the same harness passes in full
    const b = replay("base_" + m.run, m.run, realPage, SWITCHES, null);
    baselined[m.run] = b.judged.status === 0;
    if (baselined[m.run]) baselinesOk++; else baselinesBad++;
    console.log((baselined[m.run] ? "base   " : "FAIL   BASELINE ") + m.run + (baselined[m.run] ? " (the unmodified page passes in full through this harness)"
      : " does not pass unmodified (probe exit " + b.probe.status + ", judge exit " + b.judged.status + ", failing: " + JSON.stringify(b.failing).slice(0, 300) + ")"));
  }
  const r = replay(m.id, m.run, mut.page, m.args !== undefined ? m.args : SWITCHES, m.feed || null, m.extra || null);
  const hit = r.failing.filter((n) => n.indexOf(m.fails) >= 0);
  if (baselined[m.run] && r.judged.status === 1 && hit.length) {
    caught++;
    // what ELSE failed: the other content checks are the mutation's own consequences; a GENERIC check (no exception, exit 0, every gesture
    // worked...) failing too is shown in capitals -- it is legitimate only where the page change stops the feed from going on (a missing
    // item, an edit that is never sent), and is read by hand in the log (T-WG, 2026-10-23)
    const others = r.failing.filter((n) => n.indexOf(m.fails) < 0).map((n) => (/^G\d/.test(n) ? n.split(":")[0] : "GENERIC[" + n.slice(0, 40).trim() + "]"));
    console.log("caught " + m.id.padEnd(11) + "[" + m.run + "] " + m.why + "  -> FAIL " + JSON.stringify(hit[0].slice(0, 90))
      + (others.length ? "   (also failing: " + [...new Set(others)].join(", ") + ")" : ""));
  } else {
    missed++;
    console.log("FAIL   " + m.id.padEnd(11) + "[" + m.run + "] SURVIVES (probe exit " + r.probe.status + ", judge exit " + r.judged.status + "; failing: " + JSON.stringify(r.failing).slice(0, 260) + ")");
  }
}
console.log((missed || baselinesBad ? "MUTANTS_WG FAILED" : "MUTANTS_WG OK") + " -- " + caught + " caught, " + missed + " not; controls: " + baselinesOk + " unmodified runs passed, " + baselinesBad + " did not");
process.exit(missed || baselinesBad ? 1 : 0);

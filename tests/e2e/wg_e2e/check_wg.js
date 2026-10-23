// check_wg.js <run>... -- judges the WG end-to-end runs mechanically (runs\<run>.jsonl + their screenshots). Default: G1 G2 G2M G3 G4 G5
// G6 G7 G8 G8M G9. Exit 0 when every run passed.
// Every run: a clean exit 0, no JS exception / console error / waitfor timeout / dead process, focus NEVER stolen, the window on the
// SECOND screen (found by POSITION: a non-primary monitor holds the whole window -- the device name changes across reboots), muted,
// rendered on the GPU unless WE_CPU=1, the product's three browser switches present, every gesture / eval / cdp call of the feed worked
// and found its target, every screenshot a whole PNG from this run. Then what each run is for (expect() below).
// PORTED (T-WG, 2026-10-23) from AudioPB 641f1aa's scratchpad\filemode_2026-10-10\we_e2e\check_wg.py (frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\), in node. The python imported check_we.py's load()/generic(); here that generic block is
// copied from ..\we_e2e\check_we.js (T-E2E's node port of it) so the two kits stay independent (a change there cannot silently change
// what these runs are judged on) -- with one difference: screenshots are looked up next to the runs folder (WG_RUNS=<folder>: judge
// the runs of another folder, mutants_wg.js writes the mutant pages' runs elsewhere, and their screenshots land beside them).
//   Changed from the python: the words (SuVtitle, not AudioPB: the default-player item and notice), the toast "Subtitles loaded:
//   <name> . <encoding>", the ones the page has said since (the editor's field shows the line as .srt markup: G7), the strip's door
//   spelled with \u escapes; "G3: the page names no file" is the closed-list form T-E2E used for D (type / quality / target, no
//   backslash, no leaf of the dropped file).
//   DROPPED: G10 (AudioPB's desk: no desk in SuVtitle); in G1b the "no is-idle class on #bar" half (the Live window's idle fade is not in
//   SuVtitle: nothing sets or styles that class, so it could never fail -- the computed opacity is what stays judged).
const fs = require("fs");
const path = require("path");

const HERE = __dirname;
const RUNS = process.env.WG_RUNS ? path.resolve(process.env.WG_RUNS) : path.join(HERE, "runs");
const KIT = path.dirname(RUNS);        // where wv2probe ran: the feeds' relative screenshot paths (runs/<name>.png) resolve against it
const ALL = ["G1", "G2", "G2M", "G3", "G4", "G5", "G6", "G7", "G8", "G8M", "G9"];
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

// what check_we.py's generic() demanded, as check_we.js (T-E2E) keeps it
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
    out.push(["the product's switches (autoplay, video overlays off, AudioVideoTracks; never --disable-direct-composition alone)",
              SWITCHES.every((w) => args.indexOf(w) >= 0) && !/--disable-direct-composition(\s|$)/.test(args)]);
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
  // a gesture that threw, found nothing ("NOT FOUND"), or an eval that came back null (it threw) would be a run that did less than it says
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
    const p = path.resolve(KIT, sh.path || "");
    const fresh = fs.existsSync(p) && fs.statSync(p).mtimeMs >= jsonlMtime - 180000;
    out.push(["screenshot " + path.basename(p), sh.ok === true && pngWhole(p) && fresh]);
  }
  return out;
}

// ---- the geometry helpers of the python kit ----------------------------------------------------------------------------------
const boxBottom = (b) => (b && b.length === 4 ? b[1] + b[3] : null);
// the strip: padding 4 + 28 px row + 4 + borders; two rows of buttons are >= 60 px
const stripOneRow = (e) => { const h = (e.stripBox || [0, 0, 0, 999])[3]; return h > 0 && h <= 42; };
// The editor's last row (undo/redo) is ON SCREEN inside the panel, or the editor scrolls to it.
function editorReachable(e) {
  const act = e.edActBox, panel = e.panelBox, ed = e.edBox;
  const [sh, ch, ov] = e.edScroll || [0, 0, ""];
  if (!act || !panel || !ed) return false;
  const visible = act[3] > 0 && boxBottom(act) <= Math.min(boxBottom(panel), boxBottom(ed)) + 1;
  const scrolls = sh > ch && (ov === "auto" || ov === "scroll");
  return visible || scrolls;
}
// The WHOLE editor on screen, no scrolling needed (a normal window has the room).
function editorWhole(e) {
  const act = e.edActBox, panel = e.panelBox;
  const [sh, ch] = e.edScroll || [1, 0, ""];
  return !!(act && panel) && act[3] > 0 && boxBottom(act) <= boxBottom(panel) && sh <= ch + 1;
}
const z4 = [0, 0, 0, 0];
const dropKeysOk = (p) => Object.keys(p).filter((k) => k !== "_i").every((k) => ["type", "quality", "target"].indexOf(k) >= 0);

function expect(name, recs, page, ev) {
  const c = [];
  const E = (k) => ev[k] || {};
  if (name === "G1") {
    const a = E("G1a"), b = E("G1b");
    c.push(["G1a: it IS the SuVtitle page (the title, the bar's lockup)", a.title === "SuVtitle" && a.brand === "SuVtitle"]);
    c.push(["G1a: File mode's bar is ONE slim row (<= 44 px)", (a.barRect || [0, 0, 0, 999])[3] > 0 && (a.barRect || [0, 0, 0, 999])[3] <= 44]);
    c.push(["G1a: the list FOLDED by default, its door says so (\u25b8 Subtitle list)",
            a.panelShown === false && (a.listBtn || "").startsWith("\u25b8") && a.listBtnOn === false]);
    c.push(["G1a: the picture first -- the video box >= 70 % of the window's height", (a.viewBox || z4)[3] >= 0.70 * (a.h || 9999)]);
    c.push(["G1a: the strip is ONE row", stripOneRow(a)]);
    // (the python also required the class is-idle to be absent from #bar: that class was the Live window's idle fade, which SuVtitle
    //  does not have -- no CSS or script of the page touches it (RV4-14) -- so that half could not fail for a user-visible reason and
    //  is dropped; what a fade would change, the computed opacity measured in Chromium after 4 s still, stays: mutant G1-fade)
    c.push(["G1b: 4 s later, still: the bar has NOT faded (computed opacity 1, windowed File mode)", b.barOpacity === "1"]);
    c.push(["G1: nothing scrolls sideways", a.overflowX === false && b.overflowX === false]);
  } else if (name === "G2") {
    const a = E("G2a"), b = E("G2b");
    const sent = page.filter((p) => p.type === "subEdit");
    c.push(["G2a: E (a real key) opened the editor: the panel unfolded on Subtitles, a line selected",
            a.edShown === true && a.panelShown === true && (a.selRow === undefined ? -1 : a.selRow) >= 0 && (a.listBtn || "").startsWith("\u25be")]);
    c.push(["G2a: every row of the editor reachable (the undo/redo row on screen, or the editor scrolls to it)", editorReachable(a)]);
    c.push(["G2a: in a 1000 x 720 window the WHOLE editor is on screen (no scrolling in it)", editorWhole(a)]);
    c.push(["G2a: Start and End side by side in a 1000 px window (one row of times)", (a.edTimesBox || [0, 0, 0, 999])[3] > 0 && (a.edTimesBox || [0, 0, 0, 999])[3] <= 34]);
    c.push(["G2a: the list keeps >= 90 px under the editor", (a.listBox || z4)[3] >= 90]);
    c.push(["G2: the edit SENT to the core (audiopb, gen 2, rev 1: remove 1, add the new text)",
            sent.length === 1 && sent[0].target === "audiopb" && sent[0].gen === 2 && sent[0].rev === 1
            && JSON.stringify(sent[0].remove) === "[1]" && !!sent[0].add && sent[0].add[0] && sent[0].add[0].dst === "Stay, or be moved."]);
    c.push(["G2b: the core's answer applied: the row, the selection, 'edited' in the pane and the strip",
            (b.rowTexts || ["", ""])[1] === "Stay, or be moved." && b.selRow === 1 && b.edState === "edited \u00b7 saved by itself"
            && (b.status || "").indexOf(" \u00b7 edited") >= 0]);
    c.push(["G2b: the strip still ONE row with the longer status", stripOneRow(b)]);
    c.push(["G2b: the editor still reachable", editorReachable(b)]);
    c.push(["G2b: the WHOLE editor still on screen", editorWhole(b)]);
  } else if (name === "G3") {
    const a = E("G3a");
    const mo = recs.filter((r) => r.__host === "msg-objects");
    const drops = page.filter((p) => p.type === "translatorFileDrop");
    const kids = ((a.lines || [{}])[0] || {}).kids || [];
    const dropped = mo.flatMap((m) => m.files || []);
    c.push(["G3: a .srt dropped on the PLAYING video -> its REAL path reaches the host (target play)",
            mo.length >= 1 && ((mo[0].files || [""])[0] || "").endsWith("E01 Pilot.fr.srt") && drops.length > 0 && drops[0].target === "play"]);
    c.push(["G3: the page itself names no file (only type / quality / target, no backslash, no leaf of the dropped file)",
            drops.length > 0 && drops.every((p) => {
              const s = JSON.stringify(p);
              return dropKeysOk(p) && s.indexOf("\\") < 0 && dropped.length > 0 && dropped.every((f) => s.toLowerCase().indexOf(path.basename(f).toLowerCase()) < 0);
            })]);
    c.push(["G3a: 'Subtitles loaded: E01 Pilot.fr.srt' said, the file's cue on screen with its italics",
            (a.toast || "").startsWith("Subtitles loaded: E01 Pilot.fr.srt") && kids.indexOf("SPAN.r-i") >= 0]);
    c.push(["G3a: still playing", a.vPaused === false]);
  } else if (name === "G4") {
    const a = E("G4a"), b = E("G4b");
    const mains = (a.lines || []).filter((l) => (l.cls || "").indexOf("sub-main") >= 0);
    const mainsB = (b.lines || []).filter((l) => (l.cls || "").indexOf("sub-main") >= 0);
    c.push(["G4a: the user's real short cues STACK: 3 lines, the previous ones above (sub-prev), oldest first",
            mains.length === 3 && mains.slice(0, 2).every((l) => l.cls.indexOf("sub-prev") >= 0) && mains[2].cls.indexOf("sub-prev") < 0
            && JSON.stringify(mains.map((l) => l.idx)) === "[1,2,3]"]);
    c.push(["G4a: the current line's original under it (Both)", (a.lines || []).some((l) => (l.cls || "").indexOf("sub-orig") >= 0 && l.idx === 3)]);
    c.push(["G4b: at 3.6 s: two stacked (1 held for reading + the current)",
            mainsB.length === 2 && mainsB[0].cls.indexOf("sub-prev") >= 0 && JSON.stringify(mainsB.map((l) => l.idx)) === "[1,2]"]);
  } else if (name === "G5") {
    const a = E("G5a");
    c.push(["G5a: 420 x 320: the editor in the narrow DRAWER (is-narrow, is-editing, the drawer open)",
            a.w === 420 && (a.body || "").indexOf("is-narrow") >= 0 && (a.body || "").indexOf("is-editing") >= 0 && a.panelShown === true && a.edShown === true]);
    c.push(["G5a: every row of the editor reachable (it scrolls inside the drawer)", editorReachable(a)]);
    c.push(["G5a: nothing scrolls sideways", a.overflowX === false]);
  } else if (name === "G6") {
    const a = E("G6a"), b = E("G6b");
    c.push(["G6a: \u22ef holds 'Make SuVtitle the default player\u2026' in a NORMAL window",
            (a.more || []).indexOf("Make SuVtitle the default player\u2026") >= 0 && a.w === 1000]);
    c.push(["G6: the click asks the app (playerSetDefault)", page.some((p) => p.type === "playerSetDefault")]);
    c.push(["G6b: the app's answer said in the toast", (b.toast || "").startsWith("SuVtitle is registered")]);
  } else if (name === "G7") {
    const a = E("G7a");
    const sent = page.filter((p) => p.type === "subEdit");
    const kids = ((a.lines || [{}])[0] || {}).kids || [];
    c.push(["G7: a subtitle FILE's edit sent to the app (target ext, id 40), its italics as .srt markup",
            sent.length === 1 && sent[0].target === "ext" && sent[0].id === 40 && !!sent[0].add && sent[0].add[0]
            && sent[0].add[0].text === "Le niveau des <i>voix</i> est bon."]);
    c.push(["G7a: applied (the row, the picture's italics) and SAVED with its one backup, said",
            (a.rowTexts || [""])[0] === "Le niveau des voix est bon." && kids.indexOf("SPAN.r-i") >= 0
            && a.toast === "Saved E01 Pilot.fr.srt \u00b7 the original is kept as E01 Pilot.fr.srt.bak"
            && (a.edState || "").indexOf("E01 Pilot.fr.srt.bak") >= 0]);
    c.push(["G7a: the field shows the edited line AS .srt markup (text in a field, never markup on the page)",
            a.edSrc === "Le niveau des <i>voix</i> est bon."]);
    c.push(["G7a: a file has ONE text (no translation row) and offers Save as\u2026", a.edDstShown === false && a.edSaveAs === true]);
    c.push(["G7a: the editor reachable", editorReachable(a)]);
  } else if (name === "G8") {
    const a = E("G8a"), b = E("G8b");
    const film = "Long.Sample.Film.2026.FRENCH.CA.1080p.WEB.H264-TEAM";
    c.push(["G8a: the real film's long name while analysing: the strip ONE row", stripOneRow(a) && (a.status || "").startsWith("Analysing")]);
    c.push(["G8a: ...its words cut short ('\u2026'), not wrapped", a.statusCut === true]);
    c.push(["G8a: the tooltip holds the whole: both files, 'filled in as it goes'",
            (a.statusTitle || "").indexOf("Subtitle files (filled in as it goes): " + film + ".fr.srt + " + film + ".en.srt") >= 0]);
    c.push(["G8b: done + edited: '\u2026 \u00b7 edited \u00b7 saved .fr.srt + .en.srt', still ONE row",
            stripOneRow(b) && (b.status || "").endsWith(" \u00b7 edited \u00b7 saved .fr.srt + .en.srt")]);
    c.push(["G8b: the tooltip names both files whole", (b.statusTitle || "").indexOf("Subtitle files: " + film + ".fr.srt + " + film + ".en.srt") >= 0]);
    c.push(["G8: the picture keeps >= 70 % of the height", (a.viewBox || z4)[3] >= 0.70 * (a.h || 9999)]);
  } else if (name === "G9") {
    const a = E("G9a");
    const vb = a.viewBox || [0, 0, 9999, 0];
    c.push(["G9a: 1280 x 800: the panel DOCKED BESIDE the picture (is-wide), the editor open on the playhead's line",
            (a.body || "").indexOf("is-wide") >= 0 && a.panelShown === true && a.edShown === true && (a.panelBox || z4)[0] > vb[0] + vb[2] - 5 && a.selRow === 1]);
    c.push(["G9a: the WHOLE editor on screen in the docked column", editorWhole(a)]);
    c.push(["G9a: the list keeps >= 96 px under it", (a.listBox || z4)[3] >= 96]);
    c.push(["G9a: the picture keeps >= 60 % of the width", (a.viewBox || z4)[2] >= 0.6 * (a.w || 9999)]);
    c.push(["G9a: nothing scrolls sideways", a.overflowX === false]);
  } else {
    c.push(["unknown run", false]);
  }
  return c;
}

// A sentinel: the same run on the PRE-FIX layout (a style sheet put back by the feed) -- a clean run, the other checks still true, and
// EVERY check named here FAILS (a check that passes the old layout proves nothing about the fix)
const SENTINELS = {
  G2M: ["G2", ["G2a: every row of the editor reachable (the undo/redo row on screen, or the editor scrolls to it)",
               "G2a: in a 1000 x 720 window the WHOLE editor is on screen (no scrolling in it)",
               "G2a: Start and End side by side in a 1000 px window (one row of times)",
               "G2a: the list keeps >= 90 px under the editor",
               "G2b: the editor still reachable", "G2b: the WHOLE editor still on screen"]],
  G8M: ["G8", ["G8a: the real film's long name while analysing: the strip ONE row",
               "G8a: ...its words cut short ('\u2026'), not wrapped"]],
};

let allOk = true;
const names = process.argv.slice(2).length ? process.argv.slice(2) : ALL;
for (const name of names) {
  let run;
  try { run = load(name); } catch (e) { console.log("== " + name + ": no run (" + e.message + ")"); allOk = false; continue; }
  if (SENTINELS[name]) {
    const [base, mustFail] = SENTINELS[name];
    const gen = generic(name, run.recs);
    const beh = expect(base, run.recs, run.page, run.evals);
    const layout = beh.filter(([w]) => mustFail.indexOf(w) >= 0);
    const rest = beh.filter(([w]) => mustFail.indexOf(w) < 0);
    const ok = gen.every(([, v]) => v) && rest.every(([, v]) => v) && layout.length === mustFail.length && !layout.some(([, v]) => v);
    allOk = allOk && ok;
    console.log("== " + name + " (" + base + " on the PRE-FIX layout -- these checks must FAIL): " + (ok ? "PASS" : "FAIL"));
    for (const [w, v] of gen.concat(rest)) console.log("   " + (v ? "ok  " : "FAIL") + "  " + w);
    for (const [w, v] of layout) console.log("   " + (!v ? "fails as it must" : "PASSED (sentinel broken)") + "  " + w);
    continue;
  }
  const checks = generic(name, run.recs).concat(expect(name, run.recs, run.page, run.evals));
  const ok = checks.every(([, v]) => v);
  allOk = allOk && ok;
  console.log("== " + name + ": " + (ok ? "PASS" : "FAIL"));
  for (const [w, v] of checks) console.log("   " + (v ? "ok  " : "FAIL") + "  " + w);
}
console.log(allOk ? "CHECK_WG OK" : "CHECK_WG FAILED");
process.exit(allOk ? 0 : 1);

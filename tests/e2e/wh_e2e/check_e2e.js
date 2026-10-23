// wh_e2e judge: node check_e2e.js [runs\out_tracks.jsonl] -- reads what the REAL page posted and what the probe read from
// the REAL WebView2 (Chromium's audioTracks + the page's DOM) at each step of runs\feed_tracks.jsonl, and checks it.
// PORTED (S-WEB, 2026-10-23) from AudioPB 641f1aa's scratchpad\ui_test\wh_e2e\check_e2e.js (frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\wh_e2e\). Changed: S4b's toast is the WH fix round's (review WHP-8: an
// embedded track is announced in the CC panel's own words, no encoding); S7 -- measured but never judged there -- is now
// a check: Blink's `change` events for the same writes must equal what the harness's stub (tests\web\domstub.js) fires,
// so a WebView2 runtime that changes the rule shows here before the headless harness starts lying.
const fs = require("fs");
const path = require("path");
const { AudioTrackListStub } = require(path.join(__dirname, "..", "..", "web", "domstub.js"));
const file = process.argv[2] || path.join(__dirname, "runs", "out_tracks.jsonl");
const lines = fs.readFileSync(file, "utf8").split("\n").filter((l) => l.trim()).map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
const page = lines.filter((o) => !o.__host);
const evals = {};
lines.filter((o) => o.__host === "eval" && typeof o.result === "string").forEach((o) => {
  let r = o.result;
  try { r = JSON.parse(r); } catch { /* a plain string */ }
  const m = /^(S\d[a-z]?) (\{[\s\S]*\})$/.exec(String(r));
  if (m) evals[m[1]] = JSON.parse(m[2]);
});
let pass = 0, fail = 0;
const ok = (name, cond, info) => { if (cond) pass++; else fail++; console.log((cond ? "  PASS  " : "  FAIL  ") + name + (cond ? "" : "  -- " + JSON.stringify(info))); };
const on = (s) => (s && s.a ? s.a.filter((x) => x.on).map((x) => x.id).join() : "?");
const sets = page.filter((m) => m.type === "playerSet" && m.audioOrd !== undefined).map((m) => JSON.stringify(m));
const S = evals;

ok("the run reached its end, no JS exception, focus never taken",
   lines.some((o) => o.__host === "exit" && o.code === 0) && !lines.some((o) => o.__host === "exception")
   && lines.filter((o) => o.__host === "focus").length > 0
   && lines.filter((o) => o.__host === "focus").every((o) => o.stolen === false), lines.filter((o) => o.__host === "exception"));
ok("the audioTracks API is there (the AudioVideoTracks switch)", !!(S.S1a && S.S1a.api));
ok("S1 mkv_2a_def2: Chromium lists both (its own ids 2, 3) and the page ENFORCED the tray's ord 1 (Chinese) over Chromium's first",
   !!S.S1a && S.S1a.a.length === 2 && on(S.S1a) === "3" && S.S1a.rows.join("|") === "English speech (AAC 2.0)|Chinese speech (AAC 2.0)*", S.S1a);
ok("...the panel is \"Audio & subtitles\", the embedded text tracks listed", !!S.S1a && S.S1a.label === "Audio & subtitles"
   && S.S1a.subs.indexOf("French ASS (embedded · forced)") >= 0, S.S1a && S.S1a.subs);
ok("...a click on English: heard (entry 2), playerSet{audioOrd:0}, the toast, the sound still decoding",
   !!S.S1b && on(S.S1b) === "2" && sets[0] === JSON.stringify({ type: "playerSet", audioOrd: 0 }) && S.S1b.toast === "Audio: English speech · AAC 2.0"
   && S.S1b.dec > S.S1a.dec && !S.S1b.paused, { s1b: S.S1b, sets });
ok("S2 mkv_dts_first: Chromium lists ONE entry (the AAC, id 2): the DTS row disabled, and the tray told {audioOrd:1, auto:true} once",
   !!S.S2 && S.S2.a.length === 1 && S.S2.a[0].lang === "chi" && on(S.S2) === "2"
   && S.S2.rows.join("|") === "English (DTS 5.1 · can't be played here)!|Chinese (AAC 2.0)*"
   && sets.filter((x) => x === JSON.stringify({ type: "playerSet", audioOrd: 1, auto: true })).length === 1, { s2: S.S2, sets });
ok("S3 mp4_2a_movtext: Chromium's labels are the hdlr names (\"SoundHandler\"): the rows say the languages; ord 1 enforced",
   !!S.S3a && S.S3a.a.every((x) => x.label === "SoundHandler") && on(S.S3a) === "3" && S.S3a.rows.join("|") === "English (AAC 2.0)|Chinese (AAC 2.0)*", S.S3a);
ok("...the key A: the other track heard, told to the tray, said", !!S.S3b && on(S.S3b) === "2" && S.S3b.toast === "Audio: English · AAC 2.0"
   && sets.filter((x) => x === JSON.stringify({ type: "playerSet", audioOrd: 0 })).length === 2 && S.S3b.dec > S.S3a.dec, { s3b: S.S3b, sets });
ok("S4 translation off, pair en<->fr, no file next to it: the film's French track asked for (D10), \"Loading the subtitle track…\" said",
   page.some((m) => m.type === "playerSubGet" && m.id === 42) && !!S.S4a && S.S4a.toast === "Loading the subtitle track…", S.S4a);
ok("...the answer (embedded): on the picture, chosen in the panel, the editor READ-only, the toast in the panel's words (WHP-8)",
   !!S.S4b && S.S4b.overlay === "Bonjour à tous" && S.S4b.subs.indexOf("French ASS (embedded · forced)*") >= 0
   && S.S4b.ed === "Embedded subtitles are read-only | split true | ro true"
   && S.S4b.toast === "Subtitles loaded: French ASS (embedded · forced)", S.S4b);
ok("S5 translation on, pair en<->fr, the Chinese track heard: the strip says so (D11)",
   !!S.S5 && S.S5.note === "This sound track is in Chinese — the pair is English ↔ French." && on(S.S5) === "3", S.S5);
// S6: everything the page posted after the "S6 pick" mark
const pickAt = lines.findIndex((o) => o.__host === "mark" && o.value === "S6 pick");
const afterPick = pickAt < 0 ? [] : lines.slice(pickAt).filter((o) => !o.__host && o.type === "playerSet");
ok("S6 DEGRADE: nothing maps -> Chromium's own names, the first heard (its default), the panel says the subtitles keep their track",
   !!S.S6a && S.S6a.rows.join("|") === "English speech*|Chinese speech" && on(S.S6a) === "2"
   && S.S6a.anote === "Switching here changes only what you hear: the subtitles keep their sound track.", S.S6a);
ok("...a click switches the SOUND (entry 3, still decoding) and tells the tray nothing",
   !!S.S6b && on(S.S6b) === "3" && afterPick.length === 0 && S.S6b.toast === "Audio: Chinese speech" && S.S6b.dec > S.S6a.dec,
   { s6b: S.S6b, afterPick });
// S7: the same two tasks of writes on the harness's stub, from the state S6b read
function stubCounts(startOn) {
  const list = new AudioTrackListStub(), queued = [];
  list.schedule = (fn) => queued.push(fn);
  list._set([{ id: "2" }, { id: "3" }]);
  list._t.forEach((t, i) => { t._enabled = !!startOn[i]; });
  queued.length = 0;                                   // (the load's addtrack events: not counted by S7's listener)
  let chg = 0, sync = 0, inTask = true;
  list.addEventListener("change", () => { chg++; if (inTask) sync++; });
  let on0 = list[0].enabled; list[0].enabled = !on0; list[1].enabled = on0; inTask = false;
  queued.splice(0).forEach((fn) => fn());
  const afterFirstTask = chg;
  on0 = list[0].enabled; list[0].enabled = !on0; list[1].enabled = on0;
  queued.splice(0).forEach((fn) => fn());
  return { afterFirstTask, sync, afterSecondTask: chg };
}
const want7 = S.S6b && S.S6b.a.length === 2 ? stubCounts(S.S6b.a.map((x) => x.on)) : null;
ok("S7 Blink's `change` events (two writes in a task, then two more) = the harness stub's, none inside the setter (WHP-11)",
   !!S.S7 && !!want7 && JSON.stringify(S.S7) === JSON.stringify(want7) && S.S7.sync === 0, { blink: S.S7, stub: want7 });
console.log("\n" + (fail === 0 ? "ALL " + pass + " E2E CHECKS PASSED" : pass + " passed, " + fail + " FAILED"));
process.exit(fail === 0 ? 0 : 1);

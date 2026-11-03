// test_rv4.js -- the page's fix round after REVIEW RV-4 (docs/REVIEW_RV4_Page.md, 23/10/2026): one check per defect the
// review proved, on the REAL page with a fake app, and a MUTATION SENTINEL per rule (the page with that rule taken out
// must fail its check). Required by test_ui.js (same ok/section counters). RV4-9 (the harness on a CRLF page) is proved
// by running the whole harness on a CRLF and an LF copy (scratchpad/s-web/rv4/eol_proof.js); RV4-11..13 are not the
// page's (tests/e2e, the host's pins).
"use strict";
module.exports = function rv4Checks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, countType, html, script, P, MARKUP_SINK, codeOnly } = ctx;

  // ---- helpers -------------------------------------------------------------------------------------------------------
  const RLO = String.fromCharCode(0x202e), LRI = String.fromCharCode(0x2066), BOM = String.fromCharCode(0xfeff);
  const JUNK = [[0x00, 0x1f], [0x7f, 0x9f], [0x61c, 0x61c], [0x200e, 0x200f], [0x2028, 0x2029], [0x202a, 0x202e], [0x2066, 0x2069], [0xfeff, 0xfeff]];
  const junk = (s) => Array.from(String(s)).some((c) => JUNK.some(([a, b]) => c.codePointAt(0) >= a && c.codePointAt(0) <= b));
  const toast = (w) => (w.ids.fileToast.hidden ? "" : w.ids.fileToast.textContent);
  const overlay = (w) => w.ids.subOverlay.textContent;
  const subGets = (w) => w.outbox.filter((m) => m && m.type === "playerSubGet");
  const cc = (w) => { if (w.ids.ccPanel.hidden) w.ids.tCC.click(); return w; };
  const pick = (w, k) => { cc(w); const b = w.ids.ccSources.children.find((x) => x.dataset.key === k); if (!b) throw new Error("no source " + k); b.click(); };
  const onRow = (w) => { cc(w); const b = w.ids.ccSources.children.find((x) => x.classList.contains("is-on")); return b ? b.dataset.key : ""; };
  const subsMsg = (id, name, text, o) => Object.assign({ type: "playerSubs", id, name, enc: 65001, encName: "UTF-8", forced: false,
    truncated: false, part: 0, parts: 1, cues: [{ i: 0, s: 0, e: 600000, r: [[text, 0]] }] }, o);
  const key = (w, o) => w.win.dispatch("keydown", Object.assign({ target: w.doc.body }, o));
  const state = (w, o) => w.send(Object.assign({ type: "translatorState", active: true, running: true, status: "listening", preset: "quality",
    mode: "auto", langA: "en", langB: "zh", gpu: false, slot: "", models: { asr: "", mt: "" } }, o));
  // (the page is LF here -- paths.js readPage -- so a mutation is written once, against LF)
  const R = (a, b) => (s) => s.replace(a, () => b);
  const judge = (name, mutate, probe) => {
    if (mutate(script) === script) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    let w;
    try { w = runTranslatorWindow({ mutate, name }); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
    let healthy = true, threw = "";
    try { healthy = !!probe(w); } catch (e) { threw = String((e && e.message) || e); }
    if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
    ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
  };

  // ---- RV4-1: the subtitle file the user opened WITH the film is never replaced by the sidecar default -----------------
  const SIDE = [{ id: 5, name: "film.en.srt", lang: "en" }, { id: 6, name: "film.zh.srt", lang: "zh" }];
  const offMsg = (o) => fileMsg(Object.assign({ jobId: 2, seq: 2, itemId: 2, phase: "off", translate: false, sidecars: SIDE }, o));
  // (CONTRACT ADDENDUM 2, 23/10: every request of the DEFAULT rule carries auto:true -- the app IGNORES it, no answer, when
  //  the item already has a file the user named or loaded; a request without auto is the viewer's. The fake app below
  //  plays the app's side: it answers nothing to an auto request once the user's file is loaded.)
  const allAuto = (w) => subGets(w).length > 0 && subGets(w).every((m) => m.auto === true);
  // no reload (the folder already mapped): app\bridge.cpp opens the film -- its state posted at once, no file named -- then,
  // in the SAME task, loads the user's file: its document, then the state naming it (TrPlayer.h showExt). The page's
  // default (asked on the first state) reaches the app after that: auto, so ignored -- and the user's file stays
  const userFileNoReload = (w) => {
    state(w);
    w.send(offMsg({ fresh: true, extSubId: 0 }));
    metaOk(w.ids.fileVideo);
    const asked = subGets(w).map((m) => JSON.stringify(m)).join();
    w.send(subsMsg(9, "Film (VF).srt", "Bonjour"));
    w.send(offMsg({ fresh: false, extSubId: 9 }));
    w.pumpTimers(2000);
    w.ids.fileVideo._seek(1);
    return asked === JSON.stringify({ type: "playerSubGet", id: 6, auto: true }) && subGets(w).length === 1
        && overlay(w) === "Bonjour" && onRow(w) === "x:9";
  };
  // first launch by "Open with" on the film + its file (or a page reloaded for a new folder): the state names the file at
  // once, its document comes after (a big file is read in parts): nothing is asked at all -- the named file is awaited
  const userFileFirstLaunch = (w) => {
    state(w);
    w.send(offMsg({ fresh: true, extSubId: 9 }));
    metaOk(w.ids.fileVideo);
    w.pumpTimers(1500);
    const none = subGets(w).length === 0;
    w.send(subsMsg(9, "Film (VF).srt", "Bonjour"));
    w.ids.fileVideo._seek(1);
    return none && subGets(w).length === 0 && overlay(w) === "Bonjour";
  };
  // ...a file named while its parts are still arriving: the default's one request was auto (ignored by the app), nothing
  // more is asked, and the user's file takes the screen when its last part lands
  const userFileInParts = (w) => {
    state(w);
    w.send(offMsg({ fresh: true, extSubId: 0 }));
    metaOk(w.ids.fileVideo);
    w.send(subsMsg(9, "Film (VF).srt", "Bonjour", { part: 0, parts: 2 }));
    w.send(offMsg({ fresh: false, extSubId: 9 }));
    w.pumpTimers(1500);
    const quiet = allAuto(w) && subGets(w).length === 1;
    w.send(subsMsg(9, "Film (VF).srt", "Bonjour", { part: 1, parts: 2, cues: [{ i: 1, s: 600000, e: 600500, r: [["Fin", 0]] }] }));
    w.ids.fileVideo._seek(1);
    return quiet && subGets(w).length === 1 && overlay(w) === "Bonjour" && onRow(w) === "x:9";
  };
  // ...with no file named: the pair's sidecar IS the default -- asked at once, AUTO
  const defaultIsAuto = (w) => {
    state(w);
    w.send(offMsg({ fresh: true }));
    return JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 6, auto: true }]);
  };
  // ...D10's default (no file next to the film: the film's own track in the pair's language) is AUTO too
  const T_FR = { state: "ready", container: "mkv", video: { codec: "V_MPEG4/ISO/AVC", w: 640, h: 360 },
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true }],
    subs: [{ id: 72, track: 8, name: "", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: false, ok: true }] };
  const embeddedDefaultIsAuto = (w) => {
    state(w, { langA: "en", langB: "fr" });
    w.send(offMsg({ fresh: true, sidecars: [], tracks: T_FR, audioOrd: 0, langA: "en", langB: "fr" }));
    return JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 72, auto: true }]);
  };
  // ...and never once the app has NAMED a file (the tracks read after the opening, the user's file named meanwhile)
  const embeddedDefaultNotOnceNamed = (w) => {
    state(w, { langA: "en", langB: "fr" });
    w.send(offMsg({ fresh: true, sidecars: [], tracks: { state: "probing" }, audioOrd: -1, langA: "en", langB: "fr" }));
    w.send(subsMsg(9, "Film (VF).srt", "Bonjour", { part: 0, parts: 2 }));
    w.send(offMsg({ fresh: false, sidecars: [], extSubId: 9, tracks: { state: "probing" }, audioOrd: -1, langA: "en", langB: "fr" }));
    w.send(offMsg({ fresh: false, sidecars: [], extSubId: 9, tracks: T_FR, audioOrd: 0, langA: "en", langB: "fr" }));
    return subGets(w).length === 0;
  };
  // ...while a VIEWER's choice never carries it: a file next to the film, a track of the film, Off -> a sidecar again
  const viewerPickNotAuto = (w) => {
    state(w);
    w.send(offMsg({ fresh: true, tracks: T_FR, audioOrd: 0 }));
    w.outbox.length = 0;
    pick(w, "x:5");
    pick(w, "x:72");
    return JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 5 }, { type: "playerSubGet", id: 72 }]);
  };

  // ---- RV4-2 / RV4-4: the engine pill ------------------------------------------------------------------------------------
  const modelsFromObject = (w) => {
    state(w, { models: { asr: "ggml-large-v3-turbo.bin", mt: "Qwen3-4B-Instruct-2507-Q5_K_M.gguf" } });
    return w.ids.state.title === "engine ready\nModels: large-v3-turbo · Qwen3-4B";
  };
  const modelsFromString = (w) => {
    state(w, { models: "whisper · qwen" });
    return w.ids.state.title === "engine ready\nModels: whisper · qwen";
  };
  // the app's state for an engine that is NOT running (its first state, a death, a spawn that keeps failing:
  // TranslatorManager.cpp markDown -- running false, status "loading")
  const engineDownSaid = (w) => {
    state(w, { running: false, status: "loading" });
    const starting = w.ids.state.textContent === "engine starting…" && w.ids.state.classList.contains("is-loading");
    w.pumpTimers(16000);
    return starting && w.ids.state.textContent === "engine not running" && w.ids.state.classList.contains("is-error");
  };
  const engineLoadingOnlyRunning = (w) => {
    state(w, { running: true, status: "loading" });
    const loading = w.ids.state.textContent === "engine: loading models…" && w.ids.state.classList.contains("is-loading");
    w.pumpTimers(30000);                                       // a running engine may load its models for long
    return loading && w.ids.state.textContent === "engine: loading models…";
  };
  const engineBackUp = (w) => {
    state(w, { running: false, status: "loading" });
    w.pumpTimers(16000);
    const down = w.ids.state.textContent === "engine not running";
    state(w, { running: true, status: "listening" });
    const up = w.ids.state.textContent === "engine ready";
    state(w, { running: false, status: "loading" });          // it dies again: starting first, not "not running" at once
    return down && up && w.ids.state.textContent === "engine starting…";
  };
  const engineErrorWords = (w) => {
    state(w, { running: false, status: "error", err: "whisper-server.exe was not found" + RLO + "x" });
    return w.ids.state.textContent === "engine error — whisper-server.exe was not found x" && w.ids.state.classList.contains("is-error");
  };

  // ---- RV4-3: a sidecar whose read FAILED -------------------------------------------------------------------------------
  const FAIL_FILE = "That subtitle file could not be read.";
  const withSidecar = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], sidecars: [{ id: 5, name: "film.fr.srt", lang: "fr" }] }));
    metaOk(w.ids.fileVideo);
    return w;
  };
  const failedSidecarAskable = (w) => {
    withSidecar(w);
    pick(w, "x:5");
    w.send({ type: "translatorFileNotice", text: FAIL_FILE });
    const said = toast(w) === FAIL_FILE;
    pick(w, "x:5");                                            // the viewer tries again
    return said && subGets(w).map((m) => m.id).join() === "5,5";
  };
  // ...and a file next to the film is never "on its way" in the page's eyes (the app reads it at once): picked again
  // before its answer, it is asked again -- WHP-3's guard is for an embedded EXTRACTION, which a new ask would restart
  const sidecarNeverPending = (w) => {
    withSidecar(w);
    pick(w, "x:5");
    pick(w, "x:5");
    return subGets(w).map((m) => m.id).join() === "5,5";
  };
  const failedSidecarNotChosen = (w) => {
    withSidecar(w);
    w.send(subsMsg(9, "film.en.srt", "Hello (file 9)"));
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], fresh: false, extSubId: 9,
                     sidecars: [{ id: 5, name: "film.fr.srt", lang: "fr" }] }));
    pick(w, "x:5");
    w.send({ type: "translatorFileNotice", text: FAIL_FILE });
    w.ids.tCC.click(); w.ids.tCC.click();                     // the panel closed and opened again
    w.ids.fileVideo._seek(1);
    return onRow(w) === "x:9" && overlay(w) === "Hello (file 9)";
  };

  // ---- RV4-5: no control / bidi / format character, bounded, wherever a FILE provides the words -------------------------
  const evilName = (stem, ext) => stem + RLO + "4pm.exe " + LRI + "x" + BOM + ext;
  const queueClean = (w) => {
    w.send({ type: "playerQueue", cur: 1, loaded: false, repeat: "off", shuffle: false, translate: true,
             items: [{ id: 1, name: evilName("ep1 " + "A".repeat(200), ".mkv"), dir: "D:\\" + RLO + "seires", state: "none", pct: 0, durMs: 0, resumeMs: 0 }] });
    const row = w.ids.queueList.children[0];
    const nm = row.children[2].children[0], dir = row.children[2].children[1];
    return !junk(nm.textContent) && Array.from(nm.textContent).length <= 120 && !junk(nm.title) && !junk(dir.textContent);
  };
  const cardClean = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, name: evilName("song", ".mp3"), hasVideo: false, videoKnown: true }));
    metaOk(w.ids.fileAudio);
    return w.ids.fileBigName.textContent !== "" && !junk(w.ids.fileBigName.textContent);
  };
  const stripClean = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], name: evilName("film", ".mkv"),
                     srtPath: "D:\\v\\x.srt", srtName: "Other" + RLO + "crs.fr.srt", srtNote: "the .srt files were" + RLO + " not all written",
                     srtFiles: [{ lang: "fr", name: "Other" + RLO + "crs.fr.srt" }] }));
    w.send({ type: "translatorFileCue", jobId: 2, idx: 0, s: 1000, e: 3000, lang: "en", dstLang: "fr", pass: false, src: "Hi", dst: "Salut" });
    return /saved/.test(w.ids.fileStatus.textContent) && !junk(w.ids.fileStatus.textContent) && !junk(w.ids.fileStatus.title.replace(/\n/g, " "));
  };
  const savedClean = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    metaOk(w.ids.fileVideo);
    w.send(subsMsg(9, "notes" + RLO + "trs.fr.srt", "Bonjour"));
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false, fresh: false, extSubId: 9 }));
    key(w, { key: "e" });                                       // the editor: its state line names the file
    const ed0 = w.ids.edState.textContent;
    w.send({ type: "playerSubsSaved", id: 9, ok: true, name: "notes" + RLO + "trs.fr.srt", bak: "notes" + RLO + ".bak" });
    return /notes/.test(toast(w)) && !junk(toast(w)) && !junk(w.ids.edState.textContent) && !junk(ed0);
  };
  const noticeClean = (w) => {
    w.send({ type: "translatorFileNotice", text: "Not found: ep" + RLO + "1.mkv" });
    return /^Not found: ep/.test(toast(w)) && !junk(toast(w));
  };
  const exportClean = (w) => {
    w.send({ type: "translatorFileExported", ok: true, format: "srt", which: "dst", path: "D:\\v\\a" + RLO + "b.srt", count: 1 });
    return /^Saved · a/.test(toast(w)) && !junk(toast(w));
  };

  // ---- RV4-6: the extraction's own parse sentence is a subtitle-TRACK failure -------------------------------------------
  const T_FILM = { state: "ready", container: "mkv", video: { codec: "V_MPEGH/ISO/HEVC", w: 1920, h: 1040 },
    audio: [{ ord: 0, track: 2, name: "Studio 5.1 Surround", lang: "eng", codec: "A_AAC", ch: 6, def: true, play: true }],
    subs: [{ id: 72, track: 8, name: "", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: false, ok: true }] };
  const parseAsQuiet = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 60000, ranges: [[0, 60000]], tracks: T_FILM, audioOrd: 0 }));
    w.ids.fileVideo._setAudioTracks([{ id: "2", kind: "main", label: "Studio 5.1 Surround", language: "eng" }]);
    metaOk(w.ids.fileVideo);
    pick(w, "x:72");                                           // extracted (seconds, on a long film)
    w.pumpTimers(900);
    pick(w, "a:both");                                         // abandoned: playerSubGet{id:0}
    w.pumpTimers(300);
    w.send({ type: "translatorFileNotice", text: "No subtitles were found in this track." });   // its late failure
    return toast(w) === "";
  };

  // ---- RV4-7: the pin follows the window ---------------------------------------------------------------------------------
  const pinFollowsWindow = (w) => {
    w.send({ type: "translatorWindow", shown: true, fullscreen: false, topmost: true });
    const on = w.ids.pinToggle.classList.contains("is-on") && w.ids.pinToggle.getAttribute("aria-pressed") === "true";
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });           // absent: left as it is
    const kept = w.ids.pinToggle.classList.contains("is-on");
    w.outbox.length = 0;
    w.ids.pinToggle.click();                                   // the user's first click UNPINS (it used to re-pin)
    const sent = w.outbox.filter((m) => m && m.type === "translatorCtl").map((m) => m.alwaysOnTop);
    return on && kept && sent.join() === "false";
  };

  // ---- RV4-8: a French keyboard -------------------------------------------------------------------------------------------
  const k = (w) => w.ids.fileView.style["--sub-k"];
  const ctrl0Azerty = (w) => {
    for (let i = 0; i < 5; i++) key(w, { key: "=", code: "Equal", ctrlKey: true });
    const up = k(w);
    key(w, { key: "à", code: "Digit0", ctrlKey: true });       // AZERTY: the top-row 0 types "à" without Shift
    const back = k(w);
    for (let i = 0; i < 3; i++) key(w, { key: "=", code: "Equal", ctrlKey: true });
    key(w, { key: "0", code: "Numpad0", ctrlKey: true });
    return up !== "4.000" && back === "4.000" && k(w) === "4.000";
  };
  const bracketsAltGr = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    key(w, { key: "[", code: "Digit5", ctrlKey: true, altKey: true });     // AZERTY: AltGr+5 = Ctrl+Alt on Windows
    const slower = w.ids.tRate.textContent;
    key(w, { key: "]", code: "Minus", ctrlKey: true, altKey: true });
    key(w, { key: "]", code: "Minus", ctrlKey: true, altKey: true });
    const faster = w.ids.tRate.textContent;
    key(w, { key: "v", code: "KeyV", ctrlKey: true, altKey: true });       // (another Ctrl+Alt key is still not the player's)
    return slower !== "1×" && faster !== slower && faster !== "1×";
  };

  // ---- RV4-14: the theme from the empty window -----------------------------------------------------------------------------
  const themeFromEmpty = (w) => {
    const empty = w.ids.fileEmpty.hidden === false && w.ids.filePlayer.hidden === true;
    const b = w.ids.emptyTheme;
    const t0 = b.textContent;
    b.click();
    const light = w.doc.documentElement.getAttribute("data-theme") === "light" && b.textContent === "Dark theme"
               && w.win.localStorage.getItem("patchbus.theme") === "light";
    b.click();
    return empty && t0 === "Light theme" && light && w.doc.documentElement.getAttribute("data-theme") === "dark" && b.textContent === "Light theme";
  };

  section("RV4 -- the page's fix round after REVIEW RV-4 (23/10): each defect it proved, checked, and its sentinel");
  {
    ok("RV4-1 + ADDENDUM 2: translation off, the user opens the film WITH his own subtitle file (a drop / \"Open with\", no reload): the "
       + "sidecar default's request is AUTO (the app ignores it: no answer) -- his file shown, chosen", userFileNoReload(runTranslatorWindow()));
    ok("...first launch / a reload: the file named at the opening, its document arriving late (a big file): nothing asked at all, then shown",
       userFileFirstLaunch(runTranslatorWindow()));
    ok("...a file named while its parts are still arriving: only the default's auto request went, then the file takes the screen",
       userFileInParts(runTranslatorWindow()));
    ok("ADDENDUM 2: with no file named, the pair's sidecar IS the default -- asked at once, playerSubGet{id, auto:true}", defaultIsAuto(runTranslatorWindow()));
    ok("...D10's embedded default too (no file next to the film): auto", embeddedDefaultIsAuto(runTranslatorWindow()));
    ok("...and D10 asks nothing once the app has named a file (its tracks read after the opening)", embeddedDefaultNotOnceNamed(runTranslatorWindow()));
    ok("...a VIEWER's choice never carries auto (a file next to the film, a track of the film)", viewerPickNotAuto(runTranslatorWindow()));
    ok("...nor the other requests: the cancel {id:0} and the Encoding re-read are not default choices",
       /send\(\{ type: "playerSubGet", id: 0 \}\);/.test(script) && /send\(\{ type: "playerSubGet", id: ext\.id, enc: Number\(chosen\) \|\| 0 \}\);/.test(script)
       && (script.match(/auto: true \}/g) || []).length === 2);
    ok("RV4-2: the engine's models in the pill's tooltip, from the app's OBJECT {asr, mt} (app\\bridge.cpp), shortened -- never \"[object Object]\"",
       modelsFromObject(runTranslatorWindow()));
    ok("...a string still read as it is", modelsFromString(runTranslatorWindow()));
    const bridge = P.appContractSources().text;
    ok("...and the app does send an object (the fixture's shape is the app's: \\\"models\\\":{\\\"asr\\\":...,\\\"mt\\\":...})",
       /\\"models\\":\{\\"asr\\":\\""\s*\+\s*jsonEscape\(trAsrModel_\)/.test(bridge) && /\\",\\"mt\\":\\""\s*\+\s*jsonEscape\(trMtModel_\)/.test(bridge));
    ok("RV4-4: an engine the app reports NOT running (running false, status \"loading\": its first state, a death, a spawn that keeps failing) "
       + "is \"engine starting…\" (amber), then \"engine not running\" (red) after 15 s -- never \"loading models…\" for ever",
       engineDownSaid(runTranslatorWindow()));
    ok("...\"loading models…\" is said of a RUNNING engine only, for as long as it loads", engineLoadingOnlyRunning(runTranslatorWindow()));
    ok("...up again: \"engine ready\"; down again: \"starting…\" first (the clock restarts)", engineBackUp(runTranslatorWindow()));
    ok("...its error in its own words, cleaned (RV4-5)", engineErrorWords(runTranslatorWindow()));
    const tm = P.appContractSources().text;
    ok("...the app's words for it are what the pill reads: markDown sets running false + status \"loading\" (TranslatorManager.cpp)",
       /auto markDown = \[&\] \{[\s\S]{0,200}running_ = false;[\s\S]{0,200}state_\.status = "loading";/.test(tm));
    ok("RV4-3: a file next to the film whose read FAILED can be asked again (WHP-3's \"on its way\" guard is for an EMBEDDED extraction only)",
       failedSidecarAskable(runTranslatorWindow()));
    ok("...and the panel does not mark the failed file chosen while the picture shows the previous one", failedSidecarNotChosen(runTranslatorWindow()));
    ok("...a file next to the film is never \"on its way\" for WHP-3's guard: picked again before its answer, asked again", sidecarNeverPending(runTranslatorWindow()));
    ok("RV4-5: WHP-8's rule (no control / bidi / format character, 120 + \"…\") on the QUEUE's rows (name, tooltip, folder)", queueClean(runTranslatorWindow()));
    ok("...the sound card's name", cardClean(runTranslatorWindow()));
    ok("...the strip and its tooltip (the subtitle files' names, the app's note)", stripClean(runTranslatorWindow()));
    ok("...the save toast and the editor's state line", savedClean(runTranslatorWindow()));
    ok("...an app notice that names a file (\"Not found: <name>\")", noticeClean(runTranslatorWindow()));
    ok("...the export's toast", exportClean(runTranslatorWindow()));
    ok("RV4-6: parseAs's \"No subtitles were found in this track.\" -- an abandoned extraction's late failure -- is kept quiet like the others",
       parseAsQuiet(runTranslatorWindow()));
    const sr = P.cppSource("ui/SubtitleReader.h");
    const body = (/inline Result parseAs\([\s\S]*?\n\}\n/.exec(sr.text) || [""])[0];
    const parseAsSentences = [...body.matchAll(/r\.err = "([^"]+)"( \+[^;]*)?;/g)].map((m) => m[1] + (m[2] ? "UTF-8." : ""));
    const failRe = (() => { const m = /const SUB_TRACK_FAIL = (\/.*\/[a-z]*);/.exec(script); try { return m ? new Function("return " + m[1])() : null; } catch (e) { return null; } })();
    ok("...the parity: SUB_TRACK_FAIL matches each of SubtitleReader.h parseAs's sentences (" + parseAsSentences.length + ", " + sr.from + ")",
       !!failRe && parseAsSentences.length >= 4 && parseAsSentences.every((x) => failRe.test(x)), parseAsSentences.filter((x) => !failRe || !failRe.test(x)));
    ok("RV4-7: translatorWindow{topmost} (CONTRACT ADDITION) paints the pin; absent, the pin stays; the first click then UNPINS",
       pinFollowsWindow(runTranslatorWindow()));
    ok("RV4-8: Ctrl+0 by the physical key -- AZERTY's top row (\"à\") and the numpad -- back to 4.0 %", ctrl0Azerty(runTranslatorWindow()));
    ok("...[ and ] typed with AltGr (Ctrl+Alt on Windows) change the speed; another Ctrl+Alt key does nothing here", bracketsAltGr(runTranslatorWindow()));
    ok("RV4-14: the theme can be switched from the EMPTY window (no file, no queue: no ⋯ menu) -- the drop zone's \"Light theme\" / \"Dark theme\"",
       themeFromEmpty(runTranslatorWindow()));
    ok("...the dead list-font path is gone (641f1aa's File mode hid A-/A+: the list followed the LIVE window's size)",
       !/fileFontChanged\(\)\s*\{|KEY_FONT|audiopb\.tr\.font"/.test(script));

    // RV4-10: THE SECURITY RULE's lexer -- a sink hidden after a string / template / regex holding "//" or "/*" is SEEN
    const TOAST = "    el.fileToast.textContent = text;\n";
    const BS = String.fromCharCode(92), BT = String.fromCharCode(96);
    [["a string holding \" // \"", "el.fileToast.title = \"A // next track\"; el.fileToast.innerHTML = text;"],
     ["a string holding \"/*\"", "const g = \"/*\"; el.fileToast.innerHTML = text; /* end */"],
     ["a template holding \" // \"", "const t = " + BT + "x // y" + BT + "; el.fileToast.innerHTML = text;"],
     ["a regex holding \"//\"", "const re = /a" + BS + "/" + BS + "/b/; el.fileToast.innerHTML = text;"]]
      .forEach(([how, code]) => {
        const m = script.replace(TOAST, "    " + code + "\n");
        ok("MUTANT \"the toast written with innerHTML after " + how + " on the same line\" is caught by THE SECURITY RULE (RV4-10)",
           m !== script && MARKUP_SINK.test(codeOnly(m)));
      });
    ok("...while a sink only NAMED in a comment is not code (the rule reads code, not prose)",
       !MARKUP_SINK.test(codeOnly("// el.fileToast.innerHTML is never used\n/* nor outerHTML */ const x = 1;\n")));
  }

  section("RV4 -- mutation sentinels (index.html with one RV4 rule removed each)");
  {
    const probes = { userFileNoReload, userFileFirstLaunch, userFileInParts, defaultIsAuto, embeddedDefaultIsAuto, embeddedDefaultNotOnceNamed,
                     viewerPickNotAuto, modelsFromObject, modelsFromString,
                     engineDownSaid, engineLoadingOnlyRunning, engineBackUp, engineErrorWords, failedSidecarAskable, failedSidecarNotChosen, sidecarNeverPending,
                     queueClean, cardClean, stripClean, savedClean, noticeClean, exportClean, parseAsQuiet, pinFollowsWindow, ctrl0Azerty,
                     bracketsAltGr, themeFromEmpty };
    const sick = Object.keys(probes).filter((n) => { try { return !probes[n](runTranslatorWindow()); } catch (e) { return true; } });
    ok("(RV4's probes pass on the real page: " + Object.keys(probes).length + ")", sick.length === 0, sick);
    // RV4-1
    judge("RV4-1: 641f1aa's defaultExternal guard (on the no-sidecar branch only: a file named at the opening, not here yet, loses to a sidecar)",
      R("    if (F.extSubId) { if (ext && ext.id === F.extSubId) subSrc = \"ext\"; return; }",
        "    if (F.extSubId && ext && ext.id === F.extSubId) { subSrc = \"ext\"; return; }"), userFileFirstLaunch);
    judge("ADDENDUM 2: the auto flag MISSING on the sidecar default (the app would let it replace the user's file)",
      R("    askSub(best.id, false, true);", "    askSub(best.id, false);"), defaultIsAuto);
    judge("ADDENDUM 2: the auto flag MISSING on D10's embedded default", R("    if (s) askSub(s.id, true, true);", "    if (s) askSub(s.id, true);"),
      embeddedDefaultIsAuto);
    judge("ADDENDUM 2: the auto flag PRESENT on a viewer's pick (the app would ignore the viewer)",
      R("      else askSub(s.id, !!s.embedded);", "      else askSub(s.id, !!s.embedded, true);"), viewerPickNotAuto);
    judge("RV4-1: D10's embedded default asked once a file is named", R(" || wantExt || F.extSubId) return;", " || wantExt) return;"),
      embeddedDefaultNotOnceNamed);
    // RV4-2 / RV4-4
    judge("RV4-2: models read as a string (\"[object Object]\")", R("models: modelsOf(msg.models),", "models: String(msg.models || \"\"),"), modelsFromObject);
    judge("RV4-4: `running` ignored (641f1aa's pill: \"loading models…\" for ever)",
      R("    const down = !trState.running && s !== \"error\" && s !== \"off\";", "    const down = false;"), engineDownSaid);
    judge("RV4-4: never said \"not running\"", R("               : gone ? \"engine not running\" : down ? \"engine starting…\"", "               : down ? \"engine starting…\""),
      engineDownSaid);
    judge("RV4-4: the down clock never reset when the engine comes back",
      R("      engineDownSince = 0;\n      if (engineDownTimer)", "      if (engineDownTimer)"), engineBackUp);
    // RV4-3
    judge("RV4-3: WHP-3's guard on ANY file again (a failed sidecar's row dead)",
      R("    if (s.src === \"ext\" && s.embedded && embPending() && wantEmbId === s.id) return;",
        "    if (s.src === \"ext\" && wantExt === s.id && !(ext && ext.id === s.id && ext.got >= ext.parts)) return;"), sidecarNeverPending);
    judge("RV4-3: a failed sidecar's request kept (the panel marks it chosen)",
      // (P-CTX 25/10: the repaint is repaintCc() now -- the CC panel and the right-click menu, whichever is open)
      R("      else if (!msg.info && wantExt && wantExt !== wantEmbId) { wantExt = 0; repaintCc(); }\n", ""), failedSidecarNotChosen);
    // RV4-5
    judge("RV4-5: the queue's names raw", R("      name.textContent = cleanName(it.name);      // RV4-5", "      name.textContent = it.name;"), queueClean);
    judge("RV4-5: the sound card's name raw", R("    const bigName = cleanName(F.name);         // RV4-5", "    const bigName = F.name;"), cardClean);
    judge("RV4-5: the strip's note raw", R("      if (F.srtNote && n) text += \" · \" + cleanNotice(F.srtNote);", "      if (F.srtNote && n) text += \" · \" + F.srtNote;"), stripClean);
    judge("RV4-5: the save toast's names raw", R("      const nm = cleanName(msg.name), bak = cleanName(msg.bak);   // RV4-5",
      "      const nm = String(msg.name || \"\"), bak = String(msg.bak || \"\");"), savedClean);
    judge("RV4-5: app notices raw", R("      showFileToast(cleanNotice(text), !msg.info);", "      showFileToast(text, !msg.info);"), noticeClean);
    judge("RV4-5: the export toast raw", R("      showFileToast(\"Saved · \" + cleanName(p.split(/[\\\\/]/).pop() || p), false);   // RV4-5",
      "      showFileToast(\"Saved · \" + (p.split(/[\\\\/]/).pop() || p), false);"), exportClean);
    // RV4-6
    judge("RV4-6: parseAs's sentence not a track failure (641f1aa's filter)",
      R("|^The video file could not be read|^No subtitles were found in this track\\b/i;", "|^The video file could not be read/i;"), parseAsQuiet);
    // RV4-7 / RV4-8 / RV4-14
    judge("RV4-7: translatorWindow.topmost ignored (the pin boots off)", R("    if (typeof msg.topmost === \"boolean\") { onTop = msg.topmost; paintPin(); }", ""), pinFollowsWindow);
    judge("RV4-8: Ctrl+0 by e.key (AZERTY: nothing)",
      R("    else if (e.ctrlKey && (e.code === \"Digit0\" || e.code === \"Numpad0\" || e.key === \"0\")) {", "    else if (e.ctrlKey && e.key === \"0\") {"), ctrl0Azerty);
    judge("RV4-8: AltGr refused ([ and ] unreachable on AZERTY)",
      R("    if ((e.ctrlKey || e.altKey || e.metaKey) && !altGr) return;", "    if (e.ctrlKey || e.altKey || e.metaKey) return;"), bracketsAltGr);
    judge("RV4-14: the empty window's theme button not wired",
      R("  el.emptyTheme.addEventListener(\"click\", () => setTheme(themeNow() === \"light\" ? \"dark\" : \"light\"));\n", ""), themeFromEmpty);
  }
};

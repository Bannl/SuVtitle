// wg_e2e (SuVtitle): the WG page runs -- the REAL app\web\index.html in a real WebView2 (wv2probe), on the SECOND screen, never
// focused, muted, rendered on the GPU as the product is (WE_CPU=1 in run_wg.cmd: software), fed by a fake bridge that sends what
// app\bridge.cpp + app\TrPlayer.h send. Writes runs\feed_<run>.jsonl + runs\hosts.json; run_wg.cmd runs them, check_wg.js judges,
// check_hosts_wg.js proves the media host fed is the product's, mutants_wg.js proves the judges can fail.
//
// PORTED (T-WG, 2026-10-23) from AudioPB 641f1aa's scratchpad\filemode_2026-10-10\we_e2e\make_wg.py (frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\), in node, in the pattern of ..\we_e2e\make_we.js (T-E2E): the hosts come from
// ..\we_e2e\hosts.js (the product's formula, RV4-12), the media from ..\make_media.js (we\A\E01 Pilot.mp4).
//   G1  THE PICTURE FIRST: the slim one-row bar that does not fade (4 s still), the list folded, the strip's door in words
//   G2  the list unfolded, the EDITOR (E, a real key) and a text edit answered by the core        G2M = G2's layout sentinel
//   G3  a .srt DROPPED on the PLAYING video (Chromium's real drag path), then what the bridge answers
//   G4  STACKED short cues (the user's own, 3 lines at once, 35:52.6 of his film shifted to 0)
//   G5  the editor in a NARROW window (420 x 320): the drawer, taller while editing
//   G6  "Make SuVtitle the default player..." in the more menu, and the app's answer said in the toast
//   G7  a subtitle FILE in the editor: its italics as .srt markup in the field, an edit, saved with its ONE backup
//   G8  the user's REAL film name (long) while analysed, then done with both language files: the strip stays ONE row  G8M = sentinel
//   G9  the editor in a WIDE window (1280 x 800): the panel docked BESIDE the picture, the editor on top of the list
//   DROPPED: G10 (AudioPB's desk -- index.html, the main window: the footer's "Player . Translator" button, files dragged over the
//     desk, its "Drop to play in AudioPB Player" overlay, the mainFileDrop message). SuVtitle has no desk and no main window; the
//     SuVtitle equivalents of what G10 proved about drops (a real path reaches the host, the page names no file) are G3 here
//     and D in ..\we_e2e.
// Changed from the python kit: the page and origin (https://suvtitle.local/index.html), the media host (f<folderKey>.suvtitle-
// media.example, hosts.js), no `audiopb.tr.mode` storage (SuVtitle has no Live|File switch: the page boots on ready{view:"file"}),
// the state/window messages in the shape app\bridge.cpp sends (translatorState{active, gpu, slot, models:{asr,mt}}), "SuVtitle"
// for "AudioPB" in the words the app says (the default-player notice), every output under runs\.
const fs = require("fs");
const path = require("path");
const H = require("../we_e2e/hosts.js");

const HERE = __dirname;
const RUNS = path.join(HERE, "runs");
const MEDIA = path.join(HERE, "..", "media_gen", "we");
const A_DIR = path.join(MEDIA, "A");
fs.mkdirSync(RUNS, { recursive: true });

const need = (p) => { if (!fs.existsSync(p)) { console.error("make_wg: " + p + " missing -- run node ..\\make_media.js"); process.exit(1); } return p; };
const E01 = H.item(need(path.join(A_DIR, "E01 Pilot.mp4")));

// the sidecar subtitle of E01 (run G3's drag payload): the SAME file, with the same bytes, as ..\we_e2e\make_we.js writes (CRLF, like the
// python kit's), written only when it differs so its mtime stays put. The page never reads it -- Chromium needs a real file behind a dragged File
const SRT = path.join(A_DIR, "E01 Pilot.fr.srt");
const SRT_TEXT = "1\r\n00:00:00,500 --> 00:00:02,800\r\nLe niveau des <i>dialogues</i> est bon.\r\n\r\n"
  + "2\r\n00:00:03,000 --> 00:00:05,500\r\nOn baisse la musique de deux d\u00e9cibels ?\r\n";
let have = null;
try { have = fs.readFileSync(SRT, "utf8"); } catch { /* not there yet */ }
if (have !== SRT_TEXT) fs.writeFileSync(SRT, SRT_TEXT, "utf8");

const PAGE = "https://suvtitle.local/index.html";
const READY = { __waitfor: "\"ready\"", ms: 15000 };
const ASR = "ggml-large-v3-turbo.bin", MT = "Qwen3-4B-Instruct-2507-Q5_K_M.gguf";

// app\bridge.cpp sendTranslatorState: {active, running, status, preset, mode, langA, langB, gpu, slot, models:{asr,mt}} ("listening" = the engine's idle)
const state = (status = "listening", o = {}) => Object.assign({ type: "translatorState", active: true, running: true, status, preset: "balanced",
  mode: "auto", langA: "fr", langB: "en", gpu: false, slot: "", models: { asr: ASR, mt: MT } }, o);
const windowMsg = (fullscreen = false, o = {}) => Object.assign({ type: "translatorWindow", shown: true, fullscreen }, o);
// app\TrPlayer.h postStateNow: the translatorFile message (without `tracks`, which the page reads as "none yet")
function fmsg(it, o = {}) {
  return Object.assign({ type: "translatorFile", jobId: 2, seq: 1, fresh: true, phase: "done", itemId: 1,
    name: it.leaf, url: it.url, mediaKey: it.key, proxyUrl: "", proxyState: "", proxyErr: "",
    hasVideo: true, videoKnown: true, doneMs: 10000, totalMs: 10000, ranges: [[0, 10000]],
    complete: true, asrModel: ASR, mtModel: MT, langA: "fr", langB: "en", mode: "auto",
    quality: "auto", cached: true, count: 0, srtPath: "C:\\Films\\E01 Pilot.fr.srt",
    srtName: "E01 Pilot.fr.srt + E01 Pilot.en.srt",
    srtFiles: [{ lang: "fr", name: "E01 Pilot.fr.srt" }, { lang: "en", name: "E01 Pilot.en.srt" }],
    srtNote: "", edited: false, upgrade: false, oldEngine: false, err: "", cancelling: false, translate: true, autoplay: false,
    startMs: 0, resumed: false, sidecars: [], extSubId: 0 }, o);
}

// sample dialogue, French <-> English (invented; the shape of a real record: four lines laid on the 10 s clip)
const FILM = [
  [400, 2600, "Le courant finira par revenir avant la nuit.", "The power will eventually come back."],
  [2700, 4800, "Soit tu restes, soit on te laisse.", "Either you stay, or we move you."],
  [5000, 6900, "C'est quoi, cette ombre dans le noir ?", "What's that shadow in the dark?"],
  [7000, 9800, "On sait tous les deux que ce serait tr\u00e8s compliqu\u00e9.", "We both know it would be very complicated."],
];
const cues = (job = 2, rows = FILM, lang = "fr", dl = "en") => rows.map(([s, e, src, dst], n) =>
  ({ type: "translatorFileCue", jobId: job, idx: n, s, e, lang, dstLang: dl, pass: false, src, dst }));

// G4: sample cues where the overlay stacks 3 (invented text; the timings and line lengths of a real stretch of a film, shifted to 0)
const STACK = [
  [420, 2630, "J'adore cette vieille chanson.", "I love this old song."],
  [2710, 3410, "Il vous en reste combien de...", "How many do you have left..."],
  [3490, 3920, "C'est parfait.", "That's perfect."],
  [4000, 6920, "On sait tous les deux que ce sera assez, je pense,", "We both know it would be rather, I think,"],
  [7000, 9900, "assez compliqu\u00e9 pour toutes les personnes concern\u00e9es si quelque chose d'encore plus embarrassant,",
   "rather tricky for everyone concerned if something even more embarrassing,"],
];

// The page's observable state, one JSON string per __eval (make_wg.py's S; `mode` dropped -- SuVtitle has only the File mode; the page's
// title and lockup added so a run can say it IS the SuVtitle page)
const S = [
  "(() => { const $ = (id) => document.getElementById(id); const v = $('fileVideo'); const ov = $('subOverlay');",
  " const cs = (el) => getComputedStyle(el); const r4 = (r) => [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)];",
  " const lines = Array.from(ov.children).map((l) => ({ cls: l.className, text: l.textContent, idx: l.dataset.idx === undefined ? null : +l.dataset.idx,",
  "   kids: l.children[0] ? Array.from(l.children[0].childNodes).map((k) => k.nodeType === 3 ? '#text' : k.tagName + '.' + k.className) : [] }));",
  " const rows = Array.from($('fileList').children);",
  " const brand = $('bar').querySelector('.brand-name');",
  " return JSON.stringify({ title: document.title, brand: brand ? brand.textContent : '', status: $('fileStatus').textContent, body: document.body.className,",
  "  barRect: r4($('bar').getBoundingClientRect()), barOpacity: cs($('bar')).opacity, barIdle: $('bar').classList.contains('is-idle'),",
  "  viewBox: r4($('fileView').getBoundingClientRect()), stageBox: r4($('fileStage').getBoundingClientRect()),",
  "  panelShown: cs($('filePanel')).display !== 'none', panelBox: r4($('filePanel').getBoundingClientRect()),",
  "  listBtn: $('fileListBtn').textContent, listBtnOn: $('fileListBtn').classList.contains('is-on'),",
  "  edShown: !$('edPane').hidden, edWhat: $('edWhat').textContent, edState: $('edState').textContent, edMsg: $('edMsg').textContent,",
  "  edSrc: $('edSrc').value, edDst: $('edDst').value, edDstShown: !$('edDstRow').hidden, edSaveAs: !$('edSaveAs').hidden,",
  "  lines, rows: rows.length, rowTexts: rows.map((r) => r.children[1] ? r.children[1].textContent : ''),",
  "  selRow: rows.findIndex((r) => r.classList.contains('is-sel')), activeRow: rows.findIndex((r) => r.classList.contains('is-active')),",
  "  toast: $('fileToast').hidden ? '' : $('fileToast').textContent, vt: +v.currentTime.toFixed(2), vPaused: v.paused, vw: v.videoWidth,",
  "  more: $('morePanel').hidden ? null : Array.from($('morePanel').children).map((b) => b.textContent),",
  "  stripBox: r4($('fileStrip').getBoundingClientRect()), statusCut: $('fileStatus').scrollWidth > $('fileStatus').clientWidth,",
  "  statusTitle: $('fileStatus').title, edBox: r4($('edPane').getBoundingClientRect()),",
  "  edScroll: [$('edPane').scrollHeight, $('edPane').clientHeight, cs($('edPane')).overflowY],",
  "  edActBox: r4($('edUndo').getBoundingClientRect()), edTimesBox: r4($('edTimes').getBoundingClientRect()),",
  "  listBox: r4($('fileList').getBoundingClientRect()),",
  "  w: document.documentElement.clientWidth, h: document.documentElement.clientHeight,",
  "  overflowX: document.documentElement.scrollWidth > document.documentElement.clientWidth }); })()",
].join(" ");

const ev = (tag) => ({ __eval: "(" + JSON.stringify(tag) + " + ' ' + " + S + ")" });
const g = (js) => ({ __gesture: js });
const click = (id) => g("document.getElementById(" + JSON.stringify(id) + ").click(), 'click " + id + "'");
const seek = (sec) => g("document.getElementById('fileVideo').currentTime = " + sec + ", 'seek " + sec + "'");
// A REAL key press through Chromium's input path (CDP), not a synthetic event
function key(k, code, vk, mods = 0) {
  const p = { key: k, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods };
  const down = Object.assign({}, p, { type: "keyDown" });
  if (k.length === 1 && !mods) { down.text = k; down.unmodifiedText = k; }
  return [{ __cdp: "Input.dispatchKeyEvent", params: down }, { __cdp: "Input.dispatchKeyEvent", params: Object.assign({}, p, { type: "keyUp" }) }];
}
// Files dropped through Chromium's REAL drag path (CDP): File objects -> postMessageWithAdditionalObjects -> the host's paths
const drag = (paths, x, y) => ["dragEnter", "dragOver", "drop"].map((t) =>
  ({ __cdp: "Input.dispatchDragEvent", params: { type: t, x, y, data: { items: [], files: paths, dragOperationsMask: 1 } } }));
const shot = (name) => ({ __shot: "runs/" + name + ".png" });
function write(name, lines) {
  fs.writeFileSync(path.join(RUNS, "feed_" + name + ".jsonl"), lines.map((l) => JSON.stringify(l)).join("\n") + "\n", "utf8");
}
// The bridge's ORDER for an item whose folder the loaded page has not seen (TrPlayer.h ensureMapped): map its folder,
// RELOAD the page, and give everything back on its `ready`
const opened = (it) => [it.map, { __nav: PAGE }, READY];
const BOOT = [READY];

const QUEUE = { type: "playerQueue", cur: 1, loaded: true, repeat: "off", shuffle: false, translate: true,
  items: [{ id: 1, name: "E01 Pilot.mp4", dir: "A", state: "ready", pct: 100, durMs: 10000, resumeMs: 0 }] };

// ---- G1: THE PICTURE FIRST -- the slim one-row bar (no fade after 4 s still), the list folded, the strip's door ------
const G1 = [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(1.2), { __sleep: 600 }, ev("G1a"), shot("G1_picture_first_collapsed"),
  { __sleep: 4200 }, ev("G1b"), shot("G1b_still_after_4s_no_fade"), { __exit: 0 }];
write("G1", G1);

// ---- G2: the list unfolded, the EDITOR (E, a real key), a text edit answered by the core ------------------------------
const EDIT_ANSWER = { type: "translatorFileEdited", jobId: 2, rev: 1, ok: true, remove: [1], edited: true,
  add: [{ idx: 4, s: 2700, e: 4800, lang: "fr", dstLang: "en", pass: false, src: "Soit tu restes, soit on te laisse.", dst: "Stay, or be moved." }] };
const G2 = [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(3.0), { __sleep: 400 }, click("fileListBtn"), { __sleep: 300 }, ...key("e", "KeyE", 69),
  { __sleep: 500 }, ev("G2a"), shot("G2_editor_open"),
  g("(() => { const t = document.getElementById('edDst'); t.focus(); t.value = 'Stay, or be moved.'; "
    + "t.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return 'typed + Enter'; })()"),
  { __waitfor: "subEdit", ms: 4000 }, EDIT_ANSWER, fmsg(E01, { fresh: false, edited: true }), { __sleep: 500 },
  ev("G2b"), shot("G2b_editor_edit_applied"), { __exit: 0 }];
write("G2", G2);

// ---- G2M: the SENTINEL of G2's layout checks -- the same run with the PRE-FIX layout put back by a style sheet (the
//      status free to wrap, the editor's rows stacked, the 34 % panel, the editor cut instead of scrolling): check_wg.js
//      must FAIL its layout checks on it, or they prove nothing (the class names are SuVtitle's page's own) ------------------
const OLD_CSS = ".file-status { flex: 1 1 auto !important; white-space: normal !important; overflow: visible !important; } "
  + ".ed-times, .ed-texts { display: block !important; } "
  + "body.is-editing .ed-pane { flex: 0 0 auto !important; overflow-y: visible !important; } "
  + "body.is-editing .file-list { flex: 1 1 auto !important; min-height: 0 !important; } "
  + "body.is-editing:not(.is-narrow):not(.is-wide) .file-panel { flex: 0 1 34% !important; }";
// the style goes in right after the page (re)loaded for the clip = the LAST `ready` wait of the feed's opening
function withStyle(run, css, tagFrom, tagTo) {
  const out = run.slice();
  const at = out.lastIndexOf(READY);
  out.splice(at + 1, 0, { __eval: "(() => { const s = document.createElement('style'); s.textContent = " + JSON.stringify(css)
    + "; document.head.appendChild(s); return 'old layout put back'; })()" });
  return out.map((x) => ("__shot" in x ? Object.assign({}, x, { __shot: x.__shot.replace("runs/" + tagFrom, "runs/" + tagTo) }) : x));
}
write("G2M", withStyle(G2, OLD_CSS, "G2", "G2M"));

// ---- G3: a .srt DROPPED on the PLAYING video (CDP: Chromium's real drag path) -- then what the bridge answers ------------
const SUBS = { type: "playerSubs", id: 40, name: "E01 Pilot.fr.srt", enc: 65001, encName: "UTF-8", forced: false,
  truncated: false, part: 0, parts: 1, cues: [
    { i: 0, s: 500, e: 2800, r: [["Le niveau des ", 0], ["dialogues", 1], [" est bon.", 0]] },
    { i: 1, s: 3000, e: 5500, r: [["On baisse la musique de deux d\u00e9cibels ?", 0]] }] };
const G3 = [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(0.2), click("tPlay"), { __sleep: 400 }, ...drag([SRT], 480, 260),
  { __waitfor: "translatorFileDrop", ms: 6000 }, SUBS, fmsg(E01, { fresh: false, extSubId: 40 }),
  { __sleep: 700 }, ev("G3a"), shot("G3_srt_dropped_on_video"), click("tPlay"), { __exit: 0 }];
write("G3", G3);

// ---- G4: STACKED short cues -- the user's own, where his film needs 3 lines at once (35:52.6 -> 0) ------------------------
write("G4", [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(2, STACK), QUEUE,
  { __sleep: 900 }, seek(4.03), { __sleep: 600 }, ev("G4a"), shot("G4_stacked_short_cues"),
  seek(3.6), { __sleep: 500 }, ev("G4b"), shot("G4b_two_stacked"), { __exit: 0 }]);

// ---- G5: the editor in a NARROW window (420 x 320): the drawer, taller while editing ----------------------------------------
write("G5", [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(3.0), { __sleep: 400 }, ...key("e", "KeyE", 69), { __sleep: 500 }, ev("G5a"), shot("G5_narrow_editor_drawer"),
  { __exit: 0 }]);

// ---- G6: "Make SuVtitle the default player..." in the more menu (a normal window) -- and what the app answers ----------------
// (the app's own words: app\bridge.cpp handleSetDefaultPlayer -> postTranslatorFileNotice(..., info = true))
const REGISTERED = "SuVtitle is registered: in Settings > Default apps, choose it for the file types you want "
  + "(.mp4, .mkv, .mp3\u2026). It is also in every video's \"Open with\" menu.";
write("G6", [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(1.2), { __sleep: 300 }, click("tMore"), { __sleep: 400 }, ev("G6a"), shot("G6_more_default_player"),
  g("(() => { const b = Array.from(document.getElementById('morePanel').children).find((x) => /default player/.test(x.textContent)); "
    + "if (!b) return 'NOT FOUND: the default-player item'; b.click(); return 'click default'; })()"),
  { __waitfor: "playerSetDefault", ms: 3000 },
  { type: "translatorFileNotice", info: true, text: REGISTERED },
  { __sleep: 400 }, ev("G6b"), shot("G6b_registered_notice"), { __exit: 0 }]);

// ---- G7: a subtitle FILE in the editor -- its italics as .srt markup in the field, an edit, saved with its ONE backup ----------
const OFF = { translate: false, phase: "off", srtFiles: [], srtName: "", srtPath: "" };
write("G7", [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01, OFF), QUEUE,
  SUBS, fmsg(E01, Object.assign({}, OFF, { fresh: false, extSubId: 40 })),
  { __sleep: 900 }, seek(1.0), { __sleep: 300 }, click("fileListBtn"), { __sleep: 300 }, ...key("e", "KeyE", 69),
  { __sleep: 500 },
  g("(() => { const t = document.getElementById('edSrc'); t.focus(); t.value = 'Le niveau des <i>voix</i> est bon.'; "
    + "t.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return 'typed + Enter'; })()"),
  { __waitfor: "subEdit", ms: 4000 },
  { type: "playerSubsEdited", id: 40, rev: 1, ok: true, remove: [0],
    add: [{ i: 2, s: 500, e: 2800, t: "Le niveau des <i>voix</i> est bon.", r: [["Le niveau des ", 0], ["voix", 1], [" est bon.", 0]] }] },
  { __sleep: 300 },
  { type: "playerSubsSaved", id: 40, ok: true, name: "E01 Pilot.fr.srt", bak: "E01 Pilot.fr.srt.bak" },
  { __sleep: 500 }, ev("G7a"), shot("G7_file_edited_saved_backup"), { __exit: 0 }]);

// ---- G8: the user's REAL film name (long) while it is analysed: the strip stays ONE row, the words cut with "...",
//      the whole of them in the tooltip; then done with both language files -- still one row ------------------------------
const FILM_NAME = "Long.Sample.Film.2026.FRENCH.CA.1080p.WEB.H264-TEAM";
const LONG_FILES = [{ lang: "fr", name: FILM_NAME + ".fr.srt" }, { lang: "en", name: FILM_NAME + ".en.srt" }];
const LONG = { name: FILM_NAME + ".mkv", srtFiles: LONG_FILES, srtName: LONG_FILES.map((f) => f.name).join(" + "),
  srtPath: "C:\\Films\\" + LONG_FILES[0].name, totalMs: 6185000 };
const G8 = [...BOOT, ...opened(E01), state(), windowMsg(),
  fmsg(E01, Object.assign({ phase: "analyzing", complete: false, cached: false, doneMs: 2766000, ranges: [[0, 2766000]] }, LONG)), ...cues(), QUEUE,
  { __sleep: 900 }, seek(1.2), { __sleep: 400 }, ev("G8a"), shot("G8_long_name_analysing_one_row"),
  fmsg(E01, Object.assign({ fresh: false, edited: true, doneMs: 6185000, ranges: [[0, 6185000]] }, LONG)), { __sleep: 400 },
  ev("G8b"), shot("G8b_long_name_done_one_row"), { __exit: 0 }];
write("G8", G8);
// ---- G8M: G8's sentinel -- the status free to wrap again (the pre-fix CSS): the one-row checks must FAIL --------------------
write("G8M", withStyle(G8, ".file-status { flex: 1 1 auto !important; white-space: normal !important; overflow: visible !important; }", "G8", "G8M"));

// ---- G9: the editor in a WIDE window (1280 x 800): the panel docked BESIDE the picture, the editor on top of the list ----
write("G9", [...BOOT, ...opened(E01), state(), windowMsg(), fmsg(E01), ...cues(), QUEUE,
  { __sleep: 900 }, seek(3.0), { __sleep: 400 }, ...key("e", "KeyE", 69), { __sleep: 600 }, ev("G9a"), shot("G9_wide_editor_docked_right"),
  { __exit: 0 }]);

// hosts.json: the item the page is fed, with the exact host/URL/key (check_hosts_wg.js compares them with the product's C++)
const rec = (it) => ({ path: it.path, leaf: it.leaf, host: it.host, key: it.key, url: it.url });
fs.writeFileSync(path.join(RUNS, "hosts.json"), JSON.stringify({ E01: E01.host, url: E01.url, items: { E01: rec(E01) } }, null, 1) + "\n", "utf8");
console.log("feeds written: " + fs.readdirSync(RUNS).filter((f) => /^feed_.*\.jsonl$/.test(f)).sort().join(" "));
console.log("E01 host " + E01.host);

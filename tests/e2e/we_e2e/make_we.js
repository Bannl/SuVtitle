// we_e2e (SuVtitle): the REAL app\web\index.html (the player) in a real WebView2 (wv2probe), on the SECOND screen, never
// focused, muted, rendered on the GPU as the product is (WE_CPU=1 in run_we.cmd: software), driven by a fake bridge that
// sends what app\bridge.cpp + app\TrPlayer.h send (translatorFile, translatorFileCue, playerQueue, translatorWindow...).
// Writes runs\feed_<run>.jsonl + runs\hosts.json; run_we.cmd runs them, check_we.js judges, check_hosts.js proves the media hosts
// are the product's.
// PORTED from AudioPB 641f1aa's scratchpad\filemode_2026-10-10\we_e2e\make_we.py (frozen copy in
// SuVtitle\scratchpad\from_audiopb_641f1aa\), in node (the tests tree has one runtime; the python of that kit was the user's
// Whisper venv, not a SuVtitle dependency).
//   S-WEB, 2026-10-23: runs V (a video, our overlay, the queue, play/pause) and F (full screen asked by the page).
//   T-E2E, 2026-10-23 (RV4-11): the 12 runs S-WEB had left behind -- C (the CC panel and a custom style), X (a subtitle FILE),
//     P (play when ready), K (keep ahead), A (a sound file), N (420 x 320), W (1280 x 800 docked panel), T (the light theme), H
//     (the keys' help), D (drops through Chromium's real drag path), R (a page reloaded mid-queue), Z (no autoplay switch).
//   DROPPED (a feature SuVtitle does not have): L and LM ("Live stays Live": the tray's one-shot File tab, the Live tab, the window
//     state that must never bring the File tab back -- SuVtitle has no Live mode and the page boots on `ready{view:"file"}`).
// Changed from the python kit: the page and origin (https://suvtitle.local/index.html), the media hosts (f<folderKey>.suvtitle-
// media.example, hosts.js = the product's formula, RV4-12), the media (..\media_gen\we\, made by ..\make_media.js), every output
// under runs\; C: the size is a SLIDER (not the L/M/S buttons the python still clicked -- a no-op even at 641f1aa) and is checked;
// T: the theme is toggled where SuVtitle puts it (the ⋯ menu's "Light theme" / "Dark theme"), not by writing data-theme.
const fs = require("fs");
const path = require("path");
const H = require("./hosts.js");

const HERE = __dirname;
const RUNS = path.join(HERE, "runs");
const MEDIA = path.join(HERE, "..", "media_gen", "we");
const A_DIR = path.join(MEDIA, "A"), B_DIR = path.join(MEDIA, "B"), S_DIR = path.join(MEDIA, "Season 1");
fs.mkdirSync(RUNS, { recursive: true });

const need = (p) => { if (!fs.existsSync(p)) { console.error("make_we: " + p + " missing -- run node ..\\make_media.js"); process.exit(1); } return p; };
const E01 = H.item(need(path.join(A_DIR, "E01 Pilot.mp4")));
const MP3 = H.item(need(path.join(A_DIR, "Interview.mp3")));
const E02 = H.item(need(path.join(B_DIR, "E02 The Bridge.mp4")));
need(path.join(S_DIR, "E03.mp4")); need(path.join(S_DIR, "E04.mp3"));

// the sidecar subtitle of E01 (runs X and D), CRLF like the python kit's (written only when it differs: its mtime stays put)
const SRT = path.join(A_DIR, "E01 Pilot.fr.srt");
const SRT_TEXT = "1\r\n00:00:00,500 --> 00:00:02,800\r\nLe niveau des <i>dialogues</i> est bon.\r\n\r\n"
  + "2\r\n00:00:03,000 --> 00:00:05,500\r\nOn baisse la musique de deux décibels ?\r\n";
let have = null;
try { have = fs.readFileSync(SRT, "utf8"); } catch { /* not there yet */ }
if (have !== SRT_TEXT) fs.writeFileSync(SRT, SRT_TEXT, "utf8");

const PAGE = "https://suvtitle.local/index.html";
const READY = { __waitfor: "\"ready\"", ms: 15000 };
const ASR = "ggml-large-v3-turbo.bin", MT = "Qwen3-4B-Instruct-2507-Q5_K_M.gguf";

const state = (status = "analyzing", o = {}) => Object.assign({ type: "translatorState", running: true, status, preset: "balanced",
  models: "", mode: "auto", langA: "en", langB: "zh", sessionLines: 0 }, o);
const windowMsg = (fullscreen = false, o = {}) => Object.assign({ type: "translatorWindow", shown: true, fullscreen }, o);
function fmsg(it, o = {}) {
  const d = Object.assign({ type: "translatorFile", jobId: 2, seq: 1, fresh: true, phase: "analyzing", itemId: 1,
    name: it.leaf, url: it.url, mediaKey: it.key, proxyUrl: "", proxyState: "", proxyErr: "",
    hasVideo: it.leaf.endsWith(".mp4"), videoKnown: true, doneMs: 0, totalMs: 10000, ranges: [],
    complete: false, asrModel: ASR, mtModel: MT, langA: "en", langB: "zh", mode: "auto",
    quality: "auto", cached: false, count: 0, srtPath: "", srtName: "", srtNote: "", err: "",
    cancelling: false, translate: true, autoplay: false, startMs: 0, resumed: false,
    sidecars: [], extSubId: 0 }, o);
  if (d.ranges.length && !o.doneMs) d.doneMs = d.ranges.reduce((n, [s, e]) => n + e - s, 0);
  return d;
}
const CUES = [
  [500, 2800, "en", "zh", "The dialogue level is fine, but the reverb tail is too long.", "对白音量刚好，但混响尾音太长了。"],
  [3000, 5500, "en", "zh", "Can we bring the music down two decibels under the narration?", "旁白下面的音乐能再压低两分贝吗？"],
  [6000, 8000, "en", "zh", "Let's print this version and listen again tomorrow.", "这一版先导出，明天再听一遍。"],
  [8200, 9800, "zh", "en", "好的，我把混响缩短到一点二秒。", "OK, I'll shorten the reverb to 1.2 seconds."],
];
const cues = (job, which = [0, 1, 2, 3]) => which.map((i, n) => {
  const [s, e, lang, dl, src, dst] = CUES[i];
  return { type: "translatorFileCue", jobId: job, idx: n, s, e, lang, dstLang: dl, pass: false, src, dst };
});
const queue = (items, cur, loaded = true, o = {}) => Object.assign({ type: "playerQueue", cur, loaded, repeat: "off", shuffle: false,
  translate: true, items: items.map(([id, name, dir, st, pct, durMs, resumeMs]) => ({ id, name, dir, state: st, pct, durMs, resumeMs })) }, o);
const Q3 = [[1, "E01 Pilot.mp4", "A", "analysing", 60, 10000, 0],
            [2, "E02 The Bridge.mp4", "B", "partial", 35, 10000, 4200],
            [3, "E03.mp4", "Season 1", "none", 0, 0, 0],
            [4, "E04.mp3", "Season 1", "ready", 100, 10000, 0]];

// The page's observable state, one JSON string per __eval (make_we.py's S, unchanged)
const S = [
  "(() => { const $ = (id) => document.getElementById(id); const v = $('fileVideo'), a = $('fileAudio');",
  " const ov = $('subOverlay'); const cs = (el) => getComputedStyle(el); const r4 = (r) => [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)];",
  " const lines = Array.from(ov.children).map((l) => { const t = l.children[0]; return { cls: l.className, text: l.textContent,",
  "   lang: t ? t.getAttribute('lang') : null, kids: t ? Array.from(t.childNodes).map((k) => k.nodeType === 3 ? '#text' : k.tagName + '.' + k.className) : [] }; });",
  " const rows = Array.from($('fileList').children);",
  " const qrows = Array.from($('queueList').children).map((r) => (r.children[2] ? r.children[2].children[0].textContent : '?')",
  "   + ' [' + (r.children[3] ? r.children[3].textContent : '') + ']' + (r.classList.contains('is-cur') ? '*' : ''));",
  " const st = ov.querySelector('.sub-text');",
  " return JSON.stringify({ mode: $('filePane').hidden ? 'live' : 'file', status: $('fileStatus').textContent, body: document.body.className,",
  "  theme: document.documentElement.getAttribute('data-theme'), vsrc: v.getAttribute('src') || '', asrc: a.getAttribute('src') || '',",
  "  vHidden: v.hidden, bigHidden: $('fileBig').hidden, emptyHidden: $('fileEmpty').hidden, playerHidden: $('filePlayer').hidden,",
  "  lines, rows: rows.length, activeRow: rows.findIndex((r) => r.classList.contains('is-active')), queue: qrows,",
  "  ready: $('fileReady').hidden ? '' : $('fileReadyText').textContent + ' [' + $('fileReadyBtn').textContent + ']',",
  "  chip: $('fileChip').hidden ? '' : $('fileChip').textContent, notice: $('fileNotice').hidden ? '' : $('fileNotice').textContent,",
  "  toast: $('fileToast').hidden ? '' : $('fileToast').textContent, resume: $('resumeToast').hidden ? '' : $('resumeText').textContent,",
  "  time: $('tTime').textContent, ranges: Array.from($('seekRanges').children).map((s) => s.style.left + '+' + s.style.width),",
  "  vt: +v.currentTime.toFixed(2), at: +a.currentTime.toFixed(2), vPaused: v.paused, aPaused: a.paused, vw: v.videoWidth,",
  "  vReady: v.readyState, aReady: a.readyState, vErr: v.error ? v.error.code : 0,",
  "  subFont: cs(ov).fontSize, subTextColor: st ? cs(st).color : '', subTextBg: st ? cs(st).backgroundColor : '',",
  "  ovBox: r4(ov.getBoundingClientRect()), viewBox: r4($('fileView').getBoundingClientRect()),",
  "  transport: r4($('transport').getBoundingClientRect()), transportOpacity: cs($('transport')).opacity,",
  "  barShown: cs($('bar')).display !== 'none', stripShown: cs($('fileStrip')).display !== 'none', panelShown: cs($('filePanel')).display !== 'none',",
  "  panel: $('filePanel').className, tabQueue: $('tabQueue').getAttribute('aria-selected'),",
  "  cc: $('ccPanel').hidden ? null : Array.from($('ccSources').children).map((b) => b.textContent + (b.classList.contains('is-on') ? '*' : '')),",
  "  encRow: !$('ccEncRow').hidden, help: !$('helpPanel').hidden, more: $('morePanel').hidden ? null : Array.from($('morePanel').children).map((b) => b.textContent),",
  "  stage: $('fileStage').className, w: document.documentElement.clientWidth, h: document.documentElement.clientHeight,",
  "  overflowX: document.documentElement.scrollWidth > document.documentElement.clientWidth",
  " }); })()",
].join(" ");

const ev = (tag) => ({ __eval: "(" + JSON.stringify(tag) + " + ' ' + " + S + ")" });
const g = (js) => ({ __gesture: js });
const click = (id) => g("document.getElementById(" + JSON.stringify(id) + ").click(), 'click " + id + "'");
const seek = (sec) => g("document.getElementById('fileVideo').currentTime = " + sec + ", 'seek " + sec + "'");
// A REAL key press through Chromium's input path (CDP), not a synthetic event
function key(k, code, vk) {
  const p = { key: k, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
  const down = Object.assign({}, p, { type: "keyDown" });
  if (k.length === 1) { down.text = k; down.unmodifiedText = k; }
  return [{ __cdp: "Input.dispatchKeyEvent", params: down }, { __cdp: "Input.dispatchKeyEvent", params: Object.assign({}, p, { type: "keyUp" }) }];
}
// Files dropped through Chromium's REAL drag path (CDP): File objects -> postMessageWithAdditionalObjects -> the host's paths
const drag = (paths, x, y) => ["dragEnter", "dragOver", "drop"].map((t) =>
  ({ __cdp: "Input.dispatchDragEvent", params: { type: t, x, y, data: { items: [], files: paths, dragOperationsMask: 1 } } }));
const shot = (name) => ({ __shot: "runs/" + name + ".png" });
// The bridge's ORDER for an item whose folder the loaded page has not seen (TrPlayer.h ensureMapped): map its folder,
// RELOAD the page, and give everything back on its `ready`
const opened = (it) => [it.map, { __nav: PAGE }, READY];
const BOOT = [READY];
const replay = (it, msgs, full = false, q = true) => [state(), windowMsg(full), ...msgs].concat(q ? [queue(Q3, 1)] : []);
function write(name, lines) {
  fs.writeFileSync(path.join(RUNS, "feed_" + name + ".jsonl"), lines.map((l) => JSON.stringify(l)).join("\n") + "\n", "utf8");
}
// a click on the child of a segmented control / swatch row whose data-v is `val`; a range input set the way a drag does (input event)
const segv = (host, val) => g("Array.from(document.getElementById(" + JSON.stringify(host) + ").children).find((b) => b.dataset.v === "
  + JSON.stringify(val) + ").click(), 'set " + host + "=" + val + "'");
const slider = (id, val) => g("(() => { const e = document.getElementById(" + JSON.stringify(id) + "); e.value = " + JSON.stringify(String(val))
  + "; e.dispatchEvent(new Event('input', { bubbles: true })); return 'set " + id + "=" + val + "'; })()");
// a button of an open menu / panel, found by its words
const pickText = (host, re, what) => g("(() => { const b = Array.from(document.getElementById(" + JSON.stringify(host) + ").children).find((x) => " + re
  + ".test(x.textContent)); if (!b) return 'NOT FOUND: " + what + "'; b.click(); return 'click " + what + "'; })()");

// ---- V: a video, OUR overlay (both languages) on it, the strip, the queue panel -- dark ---------
write("V", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 6000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, seek(1.2), { __sleep: 600 }, click("tabQueue"), { __sleep: 300 }, ev("V1"), shot("V1_video_overlay_queue"),
  click("tabSubs"), { __sleep: 300 }, ev("V2"), shot("V2_subtitle_list"),
  click("tPlay"), { __sleep: 1500 }, ev("V3"), click("tPlay"), { __sleep: 300 }, ev("V4"), { __exit: 0 }]);

// ---- C: the CC panel and a custom style (size L = the slider at 7 %, top, yellow, outline) -----------------------------
write("C", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done" })]), ...cues(2),
  { __sleep: 900 }, seek(3.6), { __sleep: 500 }, ev("C0"), click("tCC"), { __sleep: 300 },
  slider("ccSize", 7), segv("ccPlace", "top"), segv("ccColor", "#ffe14d"), segv("ccEdge", "outline"),
  { __sleep: 400 }, ev("C1"), shot("C1_cc_panel_styled"),
  click("tCC"), { __sleep: 400 }, ev("C2"), shot("C2_style_top_yellow_L"), { __exit: 0 }]);

// ---- X: a subtitle FILE (parsed by the app: runs + flags) with italics, bold, an overlap -----------
const SUBS = { type: "playerSubs", id: 31, name: "E01 Pilot.fr.srt", enc: 1252, encName: "Western (Windows-1252)",
  forced: false, truncated: false, part: 0, parts: 1, cues: [
    { s: 500, e: 2800, r: [["Le niveau des ", 0], ["dialogues", 1], [" est bon.", 0]] },
    { s: 2400, e: 5500, r: [["On baisse la musique\nde ", 0], ["deux décibels", 2], [" ?", 0]] }] };
const SIDECARS_FR = [{ id: 31, name: "E01 Pilot.fr.srt", lang: "fr" }];
write("X", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done", sidecars: SIDECARS_FR })]),
  ...cues(2),
  { __sleep: 800 }, click("tCC"), { __sleep: 300 },
  g("Array.from(document.getElementById('ccSources').children).find((b) => /fr\\.srt/.test(b.textContent)).click(), 'pick the .srt'"),
  { __waitfor: "playerSubGet", ms: 4000 }, SUBS,
  fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done", fresh: false, sidecars: SIDECARS_FR, extSubId: 31 }),
  seek(2.6), { __sleep: 600 }, ev("X1"), shot("X1_srt_italic_bold_overlap_cc"),
  click("tCC"), { __sleep: 300 }, ev("X2"), shot("X2_srt_on_picture"), { __exit: 0 }]);

// ---- P: PLAY WHEN READY -- opened to play, it waits for 15 s of subtitles (or the end), then starts --
write("P", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { phase: "loading", autoplay: true })]),
  { __sleep: 1200 }, ev("P1"), shot("P1_armed_loading_models"),
  fmsg(E01, { fresh: false, autoplay: true, ranges: [[0, 3000]] }), cues(2, [0])[0], { __sleep: 500 }, ev("P2"),
  shot("P2_preparing_3_of_15"),
  fmsg(E01, { fresh: false, autoplay: true, ranges: [[0, 9600]] }), cues(2, [0, 1, 2, 3])[1], cues(2, [0, 1, 2, 3])[2],
  { __sleep: 1600 }, ev("P3"), shot("P3_started_by_itself"), { __exit: 0 }]);

// ---- K: KEEP AHEAD -- playing up to the end of what is subtitled pauses, and resumes at 8 s ---------
write("K", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 3000]] })]), ...cues(2, [0]),
  { __sleep: 900 }, click("tPlay"), { __sleep: 2200 }, ev("K1"), shot("K1_translating_ahead"),
  fmsg(E01, { fresh: false, ranges: [[0, 9600]] }), cues(2, [0, 1, 2])[1], cues(2, [0, 1, 2])[2],
  { __sleep: 1200 }, ev("K2"), shot("K2_resumed"), { __exit: 0 }]);

// ---- A: a SOUND file -- the card, the same overlay, the transport ------------------------------------
write("A", [...BOOT, ...opened(MP3), ...replay(MP3, [fmsg(MP3, { ranges: [[0, 10000]], complete: true, phase: "done", hasVideo: false })]), ...cues(2),
  { __sleep: 900 }, g("document.getElementById('fileAudio').currentTime = 3.6, 'seek a 3.6'"), { __sleep: 600 },
  ev("A1"), shot("A1_audio_card"), { __exit: 0 }]);

// ---- N: the narrowest window (420 x 320): compact transport, the rest behind ⋯ -----------------------
write("N", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 6000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, seek(1.2), { __sleep: 500 }, ev("N1"), shot("N1_narrow_420x320"),
  click("tMore"), { __sleep: 300 }, ev("N2"), shot("N2_narrow_more_menu"), { __exit: 0 }]);

// ---- W: a wide window (1280 x 800): the panel docks on the right ------------------------------------
write("W", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 4000], [6000, 8000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, seek(1.2), { __sleep: 500 }, click("tabQueue"), { __sleep: 300 }, ev("W1"), shot("W1_wide_docked_queue"),
  { __exit: 0 }]);

// ---- F: full screen (the WINDOW, D3): the page ASKS, the bridge answers, the controls go when still --
write("F", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done" })]), ...cues(2),
  { __sleep: 900 }, seek(0.6), { __sleep: 300 }, ...key("f", "KeyF", 70),
  // (the page's ask lands DURING the keyDown line, so a __waitfor after the keyUp would miss it: the out file shows it --
  //  translatorFullscreen{on:true} -- and the bridge's answer follows)
  { __sleep: 300 }, windowMsg(true), { __sleep: 500 }, ev("F1"), shot("F1_fullscreen_paused"),
  click("tPlay"), { __sleep: 3200 }, ev("F2"), shot("F2_fullscreen_playing_idle"),
  click("tPlay"), ...key("Escape", "Escape", 27),
  { __sleep: 300 }, windowMsg(false), { __sleep: 400 }, ev("F3"), { __exit: 0 }]);

// ---- T: the LIGHT theme, toggled where SuVtitle puts it -- the ⋯ menu ("Light theme" / "Dark theme"; SuVtitle has no main
// window, AudioPB's toggle was there). Real Chromium: the click, the data-theme, localStorage "patchbus.theme", and the
// choice surviving a page RELOAD (a new folder reloads the page: it must not flash back to dark).
const LS = (tag) => ({ __eval: "(" + JSON.stringify(tag) + " + ' ' + JSON.stringify({ theme: (() => { try { return localStorage.getItem('patchbus.theme'); } catch (e) { return 'ERR'; } })(), attr: document.documentElement.getAttribute('data-theme') }))" });
write("T", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 4000], [6000, 8000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, seek(1.2), { __sleep: 500 }, ev("T0"), LS("LS0"),
  click("tMore"), { __sleep: 300 }, ev("T0m"),
  pickText("morePanel", "/^Light theme$/", "Light theme"), { __sleep: 500 }, ev("T1"), LS("LS1"), shot("T1_light_theme"),
  click("tMore"), { __sleep: 300 }, ev("T1m"),
  // the page reloaded (what a folder the page has not seen costs: RELOAD, then everything given back on `ready`)
  { __nav: PAGE }, READY, state(), windowMsg(false), fmsg(E01, { ranges: [[0, 4000], [6000, 8000]] }), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, ev("T2"), LS("LS2"),
  click("tMore"), { __sleep: 300 }, pickText("morePanel", "/^Dark theme$/", "Dark theme"), { __sleep: 400 }, ev("T3"), LS("LS3"),
  { __exit: 0 }]);

// ---- H: the keys, and the help ----------------------------------------------------------------------
write("H", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done" })]), ...cues(2),
  { __sleep: 900 }, seek(1.2), { __sleep: 300 }, ...key("?", "Slash", 191),
  { __sleep: 400 }, ev("H1"), shot("H1_help_keys"), { __exit: 0 }]);

// ---- D: DROPS through Chromium's real drag path (CDP): a FOLDER + a .srt on the picture, a file on the QUEUE
write("D", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 6000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, ...drag([S_DIR, SRT], 400, 300),
  { __waitfor: "translatorFileDrop", ms: 6000 }, { __sleep: 800 },
  click("tabQueue"), { __sleep: 300 }, ...drag([E02.path], 1150, 420),
  { __waitfor: "\"target\":\"queue\"", ms: 6000 }, { __sleep: 800 }, ev("D1"), shot("D1_after_drops"), { __exit: 0 }]);

// ---- R: a page RELOADED mid-queue (the next item lives in another folder, D5) gets it all back -------
write("R", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 6000]] })]), ...cues(2, [0, 1, 2]),
  { __sleep: 900 }, ev("R0"),
  // Next: E02 is in folder B, which the loaded page has never seen -> map it, RELOAD, replay on `ready`
  E02.map, { __nav: PAGE }, READY,
  state(), windowMsg(false),
  fmsg(E02, { jobId: 5, seq: 2, itemId: 2, fresh: true, autoplay: true, ranges: [[0, 10000]], complete: true, phase: "done",
    startMs: 4200, resumed: true }),
  ...cues(5), queue(Q3, 2), { __sleep: 1800 }, ev("R1"), shot("R1_reloaded_next_folder_resumed"),
  { __exit: 0 }]);

// ---- Z: NO autoplay switch (main.cpp's retry-without: an environment with NONE of the switches): the refused play() becomes a
// big Play ---------------------------------------------------------------------------------------------------------------
write("Z", [...BOOT, ...opened(E01), ...replay(E01, [fmsg(E01, { ranges: [[0, 10000]], complete: true, phase: "done", autoplay: true })]), ...cues(2),
  { __sleep: 1500 }, ev("Z1"), shot("Z1_press_play_fallback"),
  click("fileReadyBtn"), { __sleep: 1200 }, ev("Z2"), { __exit: 0 }]);

// hosts.json: every item the page is fed, with the exact host/URL/key (check_hosts.js compares them with the product's C++)
const rec = (it) => ({ path: it.path, leaf: it.leaf, host: it.host, key: it.key, url: it.url });
fs.writeFileSync(path.join(RUNS, "hosts.json"), JSON.stringify({ E01: E01.host, E02: E02.host, MP3: MP3.host, url: E01.url,
  items: { E01: rec(E01), MP3: rec(MP3), E02: rec(E02) } }, null, 1) + "\n", "utf8");
console.log("feeds written: " + fs.readdirSync(RUNS).filter((f) => /^feed_.*\.jsonl$/.test(f)).sort().join(" "));
console.log("hosts: E01 " + E01.host + " | E02 " + E02.host);

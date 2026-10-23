// wh_e2e (the WH track checks), SuVtitle: the REAL app\web\index.html in a REAL WebView2 (wv2probe: second screen, never
// focused, muted), the fake bridge = this feed, the media = tests\e2e\media_gen\wh\ (node ..\make_media.js).
// PORTED (S-WEB, 2026-10-23) from AudioPB 641f1aa's scratchpad\ui_test\wh_e2e\make_feed.js (frozen copy:
// SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\wh_e2e\). Changed: the page (https://suvtitle.local/index.html), the
// media host (suvtitle-media.example), every output under runs\ (git-ignored). The messages are the bridge's, unchanged
// (the contract kept its names). Writes runs\feed_tracks.jsonl; check_e2e.js judges runs\out_tracks.jsonl.
const fs = require("fs");
const path = require("path");

const MEDIA_URL = "https://suvtitle-media.example/";
const vid = { codec: "V_MPEG4/ISO/AVC", w: 640, h: 360 };
const T_2A = { state: "ready", container: "mkv", video: vid,
  audio: [{ ord: 0, track: 2, name: "English speech", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true },
          { ord: 1, track: 3, name: "Chinese speech", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }],
  subs: [{ id: 41, track: 4, name: "", lang: "eng", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: false, ok: true },
         { id: 42, track: 5, name: "French ASS", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: true, ok: true },
         { id: 43, track: 6, name: "", lang: "ger", codec: "D_WEBVTT/SUBTITLES", kind: "text", def: false, forced: false, ok: true }] };
const T_DTS = { state: "ready", container: "mkv", video: vid, subs: [],
  audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_DTS", ch: 6, def: true, play: false },
          { ord: 1, track: 3, name: "", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }] };
const T_MP4 = { state: "ready", container: "mp4", video: { codec: "avc1", w: 640, h: 360 },
  audio: [{ ord: 0, track: 2, name: "SoundHandler", lang: "eng", codec: "mp4a", ch: 2, def: true, play: true },
          { ord: 1, track: 3, name: "SoundHandler", lang: "chi", codec: "mp4a", ch: 2, def: false, play: true }],
  subs: [{ id: 51, track: 4, name: "SubtitleHandler", lang: "eng", codec: "tx3g", kind: "text", def: false, forced: false, ok: true }] };

let job = 10;
const file = (seq, name, tracks, audioOrd, o) => Object.assign({ type: "translatorFile", jobId: ++job, seq, phase: "done", itemId: seq,
  name, url: MEDIA_URL + name, mediaKey: "mk" + seq, proxyUrl: "", proxyState: "", proxyErr: "",
  hasVideo: true, videoKnown: true, doneMs: 12000, totalMs: 12000, ranges: [[0, 12000]], complete: true, asrModel: "", mtModel: "",
  langA: "en", langB: "zh", mode: "auto", quality: "auto", cached: true, count: 0, srtPath: "", srtName: "", srtNote: "", err: "",
  cancelling: false, fresh: true, translate: true, autoplay: true, startMs: 0, resumed: false, sidecars: [], extSubId: 0,
  tracks, audioOrd }, o);
const state = (a, b) => ({ type: "translatorState", running: true, status: "listening", preset: "balanced", models: "", mode: "auto",
                           langA: a, langB: b, sessionLines: 0 });
// what the page shows and what Chromium has, in one line of the out file
const probe = "window.__wh = () => { const v = document.getElementById('fileVideo'); const L = v.audioTracks; const a = [];"
  + " if (L) for (let i = 0; i < L.length; i++) a.push({ i, id: L[i].id, label: L[i].label, lang: L[i].language, on: L[i].enabled });"
  + " const txt = (sel) => [...document.querySelectorAll(sel)].map((b) => b.textContent + (b.classList.contains('is-on') ? '*' : '') + (b.disabled ? '!' : ''));"
  + " const t = document.getElementById('fileToast'), n = document.getElementById('fileLangNote');"
  + " return JSON.stringify({ api: !!L, a, rows: txt('#ccAudio button'), subs: txt('#ccSources button'),"
  + " label: document.getElementById('ccPanel').getAttribute('aria-label'), cctitle: document.getElementById('tCC').title,"
  + " toast: t.hidden ? '' : t.textContent, note: n.hidden ? '' : document.getElementById('fileLangNoteText').textContent,"
  + " ed: document.getElementById('edPane').hidden ? '' : document.getElementById('edMsg').textContent + ' | split ' + document.getElementById('edSplit').disabled + ' | ro ' + document.getElementById('edSrc').readOnly,"
  + " anote: document.getElementById('ccAudioNote').hidden ? '' : document.getElementById('ccAudioNote').textContent,"
  + " overlay: document.getElementById('subOverlay').textContent, rs: v.readyState, dec: v.webkitAudioDecodedByteCount, t: v.currentTime,"
  + " paused: v.paused, err: v.error && v.error.message }); }; 'probe installed'";
const ev = (tag) => ({ __eval: "'" + tag + " ' + window.__wh()" });
const shot = (n) => ({ __shot: "runs/" + n + ".png" });

const L = [];
L.push({ __waitfor: "\"ready\"", ms: 15000 });
L.push({ __eval: probe });
L.push(state("en", "zh"));
// S1 mkv_2a_def2: the tray chose ord 1 (FlagDefault); Chromium enables its FIRST -> the page must switch to entry 1
L.push({ __mark: "S1 mkv_2a_def2 audioOrd 1" });
L.push(file(1, "mkv_2a_def2.mkv", T_2A, 1));
L.push({ __sleep: 3500 });
L.push({ __gesture: "document.getElementById('tCC').click(), 'cc open'" });
L.push({ __sleep: 300 });
L.push(ev("S1a"));
L.push(shot("S1_panel"));
L.push({ __gesture: "document.querySelectorAll('#ccAudio button')[0].click(), 'pick English'" });
L.push({ __waitfor: "\"audioOrd\":0", ms: 3000 });
L.push({ __sleep: 1200 });
L.push(ev("S1b"));
// S2 mkv_dts_first: the tray's choice (ord 0, DTS) can't be played here -> the AAC one, and the tray is told auto
L.push({ __mark: "S2 mkv_dts_first audioOrd 0 (DTS)" });
L.push(file(2, "mkv_dts_first.mkv", T_DTS, 0));
L.push({ __waitfor: "\"auto\":true", ms: 8000 });
L.push({ __sleep: 800 });
L.push({ __gesture: "document.getElementById('ccPanel').hidden && document.getElementById('tCC').click(), 'cc'" });
L.push({ __sleep: 300 });
L.push(ev("S2"));
L.push(shot("S2_dts"));
// S3 mp4_2a_movtext: generic handler names; the tray chose ord 1; then the key A
L.push({ __mark: "S3 mp4_2a_movtext audioOrd 1, then A" });
L.push(file(3, "mp4_2a_movtext.mp4", T_MP4, 1));
L.push({ __sleep: 3500 });
L.push({ __gesture: "document.getElementById('ccPanel').hidden && document.getElementById('tCC').click(), 'cc'" });
L.push({ __sleep: 300 });
L.push(ev("S3a"));
L.push({ __eval: "document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'a', bubbles: true })), 'key A'" });
L.push({ __waitfor: "\"audioOrd\":0", ms: 3000 });
L.push({ __sleep: 1200 });
L.push(ev("S3b"));
L.push(shot("S3_mp4_keyA"));
// S4 D10 + D7: translation OFF, pair en <-> fr, no file next to it -> the film's own French track (forced) is asked
// for; no answer -> "Loading the subtitle track…" after 700 ms; then the answer (embedded) -> shown, read-only
L.push({ __mark: "S4 translation off, embedded subtitles" });
L.push(state("en", "fr"));
L.push(file(4, "mkv_2a_def2.mkv", T_2A, 1, { phase: "off", translate: false, langA: "en", langB: "fr", autoplay: false }));
L.push({ __waitfor: "\"playerSubGet\"", ms: 5000 });
L.push({ __sleep: 1500 });
L.push(ev("S4a"));
L.push(shot("S4_loading"));
L.push({ type: "playerSubs", id: 42, name: "French ASS (track 5)", enc: 65001, encName: "UTF-8", forced: true, truncated: false,
         part: 0, parts: 1, embedded: true, track: 5,
         cues: [{ i: 0, s: 0, e: 6000, r: [["Bonjour à tous", 0]] }, { i: 1, s: 6000, e: 12000, r: [["Deuxième ligne", 1]] }] });
L.push({ __sleep: 600 });
L.push({ __eval: "document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'e', bubbles: true })), 'key E'" });
L.push({ __sleep: 600 });
L.push(ev("S4b"));
L.push(shot("S4_readonly"));
// S5 D11: translation ON, pair en <-> fr, the tray chose the Chinese track -> the strip says so
L.push({ __mark: "S5 the heard track outside the pair" });
L.push(file(5, "mkv_2a_def2.mkv", T_2A, 1, { langA: "en", langB: "fr" }));
L.push({ __sleep: 3000 });
L.push(ev("S5"));
L.push(shot("S5_langnote"));
// S6 DEGRADE: the tray's description contradicts what Chromium lists (other languages, other names) -> nothing maps:
// Chromium's own names, a click changes the sound only, the tray is told nothing, the panel says so
L.push({ __mark: "S6 degrade" });
const T_ODD = { state: "ready", container: "mkv", video: vid, subs: [],
  audio: [{ ord: 0, track: 2, name: "Commentary", lang: "fre", codec: "A_AAC", ch: 2, def: true, play: true },
          { ord: 1, track: 3, name: "Main", lang: "ger", codec: "A_AAC", ch: 2, def: false, play: true }] };
// (its own URL, as the bridge gives every item one -- ?v=<key>: the element reloads, Chromium enables its FIRST again)
L.push(file(6, "mkv_2a_def2.mkv", T_ODD, 0, { langA: "en", langB: "zh", url: MEDIA_URL + "mkv_2a_def2.mkv?v=6" }));
L.push(state("en", "zh"));
L.push({ __sleep: 3000 });
L.push({ __gesture: "document.getElementById('ccPanel').hidden && document.getElementById('tCC').click(), 'cc'" });
L.push({ __sleep: 300 });
L.push(ev("S6a"));
L.push({ __mark: "S6 pick" });
L.push({ __gesture: "document.querySelectorAll('#ccAudio button')[1].click(), 'pick 2nd'" });
L.push({ __sleep: 1200 });
L.push(ev("S6b"));
L.push(shot("S6_degrade"));
// S7 (review WHP-11): how many `change` events Blink fires for TWO `enabled` writes in one task, then one more in another
// task -- and whether any arrives synchronously (inside the setter). check_e2e.js replays the same writes on the harness's
// stub (tests\web\domstub.js) from the state S6b read: the two must agree.
L.push({ __mark: "S7 change events per flip" });
L.push({ __eval: "(() => { const L = document.getElementById('fileVideo').audioTracks; window.__chg = 0; window.__chgSync = 0;"
  + " let inTask = true; L.addEventListener('change', () => { window.__chg++; if (inTask) window.__chgSync++; });"
  + " const on0 = L[0].enabled; L[0].enabled = !on0; L[1].enabled = on0; inTask = false;"
  + " return 'two writes in one task: ' + L[0].enabled + ',' + L[1].enabled; })()" });
L.push({ __sleep: 500 });
L.push({ __eval: "(() => { window.__chg1 = window.__chg; const L = document.getElementById('fileVideo').audioTracks;"
  + " const on0 = L[0].enabled; L[0].enabled = !on0; L[1].enabled = on0; return 'two more writes'; })()" });
L.push({ __sleep: 500 });
L.push({ __eval: "'S7 ' + JSON.stringify({ afterFirstTask: window.__chg1, sync: window.__chgSync, afterSecondTask: window.__chg })" });
L.push({ __exit: 0 });

const dir = path.join(__dirname, "runs");
fs.mkdirSync(dir, { recursive: true });
const out = path.join(dir, "feed_tracks.jsonl");
fs.writeFileSync(out, "# generated by make_feed.js -- see run_e2e.cmd\n" + L.map((x) => JSON.stringify(x)).join("\n") + "\n", "utf8");
console.log("wrote " + out + " (" + L.length + " lines)");

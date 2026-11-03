// test_native.js -- THE NATIVE VIDEO ENGINE (1.1.0, 26/10/2026, agent N2): the page's MEDIA BACKEND for mpv
// (docs/NATIVE_CONTRACT.md §4 and §6). Required by test_ui.js (same ok/section counters). Every check runs the REAL page
// (app/web/index.html) with a fake app speaking the native messages -- nativeInfo / nativeState / nativeError in, nativeRect /
// nativeCmd / nativeCursor out -- on the harness's virtual clock (performance.now IS the test's clock, so "the playhead 950 ms
// after a state" is exact). Each rule has a MUTATION SENTINEL at the end: the page with that rule taken out must FAIL its probe.
// The bridge's half (bridge.cpp) is checked on its source the same way (a source pin + its mutants), and the contract both ways.
// What this proves: the backend choice, N0's interpolated clock and its holds, every transport command, the picture's
// rectangle, the transparent stage, the three fallbacks, the subtitles following mpv's clock, the cursor's safety net.
// What it cannot: mpv itself, the composition over its window, the real timing of a WebView2 (the real app: PROGRESS_N2.md).
// FIX ROUND (02/11, docs/FIXROUND_NATIVE.md "N2 items", review REVIEW_RV-NP.md): M1 the clock's hold when mpv is quiet (its
// cadences MEASURED in the real app), M2 the seek hold's own expiry + seekable:false, M3 a behaviour check for each mutant
// the review found surviving, L1 / L4 / L5 / L6 / L7 / L8 on the page, and the bridge's / core's halves of L2 / L3 / L6 / L7 /
// L9 as source pins (B6, B10, B11, C1, C2) with their mutants.
"use strict";
module.exports = function nativeChecks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, script, html, P } = ctx;
  const URL1 = "https://m1.suvtitle-media.example/film.mp4";
  const W = (o) => runTranslatorWindow(Object.assign({ resizeObserver: true }, o || {}));
  const info = (w, on, why) => w.send({ type: "nativeInfo", available: on !== false, why: why || "" });
  // mpv's state of opening 1, loaded, a 10-minute film with a picture -- override what a probe needs
  const ST = (o) => Object.assign({ type: "nativeState", gen: 1, timeMs: 0, durMs: 600000, paused: true, eof: false, seeking: false,
    speed: 1, volPct: 100, muted: false, ord: 0, tracks: 1, abA: -1, abB: -1, w: 1920, h: 1080, hwdec: "d3d11va", warp: false,
    seekable: true, loaded: true, video: true }, o);
  const FILE = (o) => fileMsg(Object.assign({ jobId: 2, seq: 1, itemId: 12, phase: "done", complete: true, ranges: [[0, 600000]] }, o));
  // the app has the engine, then opens item 1 (seq 1); `o` = the translatorFile's fields
  const open = (w, o) => { info(w); w.send(FILE(o)); return w; };
  const state = (w, o) => { w.send(ST(o)); return w; };
  const out = (w, type, cmd) => w.outbox.filter((m) => m && m.type === type && (!cmd || m.cmd === cmd));
  const cmds = (w, cmd) => out(w, "nativeCmd", cmd);
  const lastOf = (w, type, cmd) => { const o = out(w, type, cmd); return o[o.length - 1]; };
  const key = (w, k, o) => w.win.dispatch("keydown", Object.assign({ key: k, target: w.doc.body }, o || {}));
  // the playhead as the page knows it: a pause reports it, exactly (playerPos{reason:"pause"} -- the page's own word)
  const pauseAt = (w) => { w.outbox.length = 0; key(w, " "); const p = lastOf(w, "playerPos"); return p && p.reason === "pause" ? p.ms : NaN; };
  // one AudioPB cue a second: idx i spans [i s, i+1 s) -- the overlay names the second the playhead is in
  const cues = (w, from, to) => {
    const list = [];
    for (let i = from; i < to; i++) list.push({ idx: i, s: i * 1000, e: i * 1000 + 1000, lang: "en", dstLang: "zh", pass: false, src: "s" + i, dst: "d" + i });
    w.send({ type: "translatorFileCues", jobId: 2, cues: list });
  };
  const shown = (w) => { const l = w.ids.subOverlay.children[0]; return l ? Number(l.dataset.idx) : -1; };
  const rendered = (w) => (w.ids.fileView.classList.contains("render-plain") ? "plain"
    : w.ids.fileView.classList.contains("render-canvas") ? "canvas" : "blend");
  const TRACKS2 = { state: "ready", container: "mkv", video: { codec: "V_MPEGH/ISO/HEVC", w: 1920, h: 1080 },
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "", lang: "fre", codec: "A_DTS", ch: 6, def: false, play: false }], subs: [] };

  // ---- the probes (each is a check below AND the condition its mutation sentinel must see FAIL) -----------------------------
  // 1. THE BACKEND: the app has the engine -> mpv plays the item: the <video> stays idle (never given a src), the stage is
  //    see-through, the page's settings go to mpv for this opening (gen = translatorFile.seq)
  const nativeChosen = (w) => {
    open(w); state(w);
    return !w.ids.fileVideo.src && w.ids.fileVideo.loads === 0 && w.ids.fileVideo.hidden === true && !w.ids.fileAudio.src
      && w.doc.body.classList.contains("is-native") && cmds(w, "volume").length === 1 && cmds(w, "speed").length === 1
      && cmds(w).every((m) => m.gen === 1);
  };
  // 2. an app that never speaks of the engine (an older SuVtitle): the page plays as before, and says nothing native
  const olderApp = (w) => {
    w.send(FILE());
    return w.ids.fileVideo.src === URL1 && !w.outbox.some((m) => m && /^native/.test(String(m.type)))
      && !w.doc.body.classList.contains("is-native");
  };
  // 3. the engine NOT available (libmpv missing): the <video>, and no command to an engine that is not there
  const unavailable = (w) => {
    info(w, false, "no libmpv-2.dll"); w.send(FILE());
    return w.ids.fileVideo.src === URL1 && cmds(w).length === 0 && !w.doc.body.classList.contains("is-native");
  };
  // 4. THE CLOCK: a state says 10.000 s, playing; 950 ms later -- no state in between -- the playhead is 10.950 s
  const clockInterp = (w) => { open(w); state(w, { timeMs: 10000, paused: false }); w.pumpTimers(950); return pauseAt(w) === 10950; };
  // 5. ...x the speed mpv plays at (2x: 500 ms of wall time = 1 s of film)
  const clockSpeed = (w) => { open(w); state(w, { timeMs: 10000, paused: false, speed: 2 }); w.pumpTimers(500); return pauseAt(w) === 11000; };
  // 6. A SEEK HOLDS THE CLOCK at its target until the app confirms it: a state from BEFORE the seek (mpv still at the old
  //    place) does not drag the playhead back; the confirming one restarts it from mpv's own time
  const seekHold = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    w.outbox.length = 0;
    key(w, "ArrowRight");                                       // +5 s
    const s = lastOf(w, "nativeCmd", "seek");
    state(w, { timeMs: 10050, paused: false });                 // older than the seek
    w.pumpTimers(200);
    const held = s && s.ms === 15001 && s.exact === true;
    // (the pause reads the held clock; then mpv's answer and 300 ms more)
    const a = pauseAt(w);
    return held && a === 15001;
  };
  const seekConfirm = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    key(w, "ArrowRight");
    state(w, { timeMs: 15020, paused: false });                 // mpv's answer, near the target
    w.pumpTimers(300);
    return pauseAt(w) === 15320;
  };
  // 6b. THE CLOCK'S ERROR is measured as N0 measured it (steady playback only): what the real-app checks read
  const driftMeasure = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    w.pumpTimers(33); state(w, { timeMs: 10040, paused: false });     // the clock said 10033: +7
    w.pumpTimers(33); state(w, { timeMs: 10070, paused: false });     // the clock said 10073: -3
    w.pumpTimers(33); state(w, { timeMs: 10100, paused: true });      // a pause: not steady playback, not counted
    const d = w.win.__suvNative.drift(), e = w.win.__suvNative.drift();
    return d.n === 2 && d.mean === 2 && d.meanAbs === 5 && d.max === 7 && e.n === 0 && w.win.__suvNative.backend() === "native";
  };
  // ...and a state far off the clock (a stall, a jump) is COUNTED, not dropped silently (fix round, M1: `big`)
  const driftBig = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    w.pumpTimers(100); state(w, { timeMs: 10900, paused: false });   // 800 ms ahead of the clock
    const d = w.win.__suvNative.drift();
    return d.n === 0 && d.big === 1;
  };
  // 6c. M1 (fix round 02/11, review P1): mpv QUIET while it plays (a stall: a disk, a share, a decoder) -- the clock runs on
  //     1.5 s at most, then HOLDS (counted once); before, 30 s later the page read 40 s and the subtitles ran on
  const quietHold = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    w.pumpTimers(30000);
    const h = w.win.__suvNative.drift().holds;
    return pauseAt(w) === 11500 && h === 1;
  };
  //     ...so keep-ahead does not pause a film mpv has not played (review P1b: ranges to 16 s, mpv stuck at 10 s)
  const quietKeepAhead = (w) => {
    info(w); w.send(FILE({ phase: "analyzing", complete: false, ranges: [[0, 16000]] }));
    state(w, { timeMs: 10000, paused: false });
    w.outbox.length = 0;
    for (let i = 0; i < 40; i++) { w.pumpTimers(100); w.pumpRaf(); }   // 4 s, mpv silent
    return cmds(w, "pause").length === 0 && Math.abs(w.win.__suvNative.time() - 11500) < 1;
  };
  //     ...and it NEVER bites on a healthy file (MEASURED cadences, PROGRESS_N2): a 1 fps film at 0.25x speaks every 4.35 s
  //     (1.5 s / 0.25 = 6 s allowed) -- and a film's own pace is learnt: 1 fps at 1x, then a 3.5 s gap between two frames
  //     (4 x its 1 s allowed)
  const quietSlowFilm = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false, speed: 0.25 });
    let at = 0;
    for (let i = 1; i <= 3; i++) {
      w.pumpTimers(4350);
      at = w.win.__suvNative.time();
      state(w, { timeMs: 10000 + i * 1000, paused: false, speed: 0.25 });
    }
    return w.win.__suvNative.drift().holds === 0 && Math.abs(at - (12000 + 4350 * 0.25)) < 1;
  };
  const quietLearned = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    for (let i = 1; i <= 3; i++) { w.pumpTimers(1064); state(w, { timeMs: 10000 + i * 1000, paused: false }); }
    w.pumpTimers(3500);
    return w.win.__suvNative.time() === 16500 && w.win.__suvNative.drift().holds === 0;
  };
  // 7. mpv's own `seeking` (an A-B jump, a restart at the end) holds the clock too
  const mpvSeeking = (w) => { open(w); state(w, { timeMs: 10000, paused: false, seeking: true }); w.pumpTimers(400); return pauseAt(w) === 10000; };
  // 7b. M3 (the review's survivors): a speed asked holds against a state from before it (P10) -- the clock runs at 1.25x
  const speedHold = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    key(w, "]");
    state(w, { timeMs: 10010, paused: false, speed: 1 });     // posted before mpv read the speed
    w.pumpTimers(1000);
    return pauseAt(w) === 10010 + 1250;
  };
  //     ...the clock never runs past mpv's duration
  const clockCap = (w) => { open(w); state(w, { timeMs: 599500, paused: false }); w.pumpTimers(1000); return w.win.__suvNative.time() === 600000; };
  // 8. PLAY / PAUSE: the page's buttons -> play / pause for this opening; a state from before the ask does not flip it back
  const playPause = (w) => {
    open(w); state(w, { timeMs: 10000 });
    w.outbox.length = 0;
    w.ids.tPlay.click();
    const a = cmds(w, "play").length === 1 && w.ids.tPlay.classList.contains("is-playing");
    state(w, { timeMs: 10000, paused: true });                  // posted before mpv read the play
    const b = w.ids.tPlay.classList.contains("is-playing") && !out(w, "playerPos").some((p) => p.reason === "pause");
    w.ids.tPlay.click();
    return a && b && cmds(w, "pause").length === 1 && !w.ids.tPlay.classList.contains("is-playing");
  };
  // 9. SPEED, VOLUME, MUTE: one command per change -- ] = 1.25x, a volume step does not send the speed again
  const transport = (w) => {
    open(w); state(w);
    w.outbox.length = 0;
    key(w, "]");
    key(w, "ArrowDown");
    key(w, "m"); key(w, "m");
    const sp = cmds(w, "speed"), vo = cmds(w, "volume"), mu = cmds(w, "mute");
    return sp.length === 1 && sp[0].rate === 1.25 && vo.length === 1 && vo[0].pct === 95
      && mu.length === 2 && mu[0].on === true && mu[1].on === false;
  };
  // 10. A-B: L sets A (mpv told A alone), L sets B (the pair: mpv loops by itself, jumping to A), L clears (null, null)
  // (mpv speaks while it plays: a state a second -- the clock holds after 1.5 s of silence since the fix round's M1)
  const playOn = (w, from, ms) => { for (let t = 1000; t <= ms; t += 1000) { w.pumpTimers(1000); state(w, { timeMs: from + t, paused: false }); } };
  const abCmds = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    w.outbox.length = 0;
    key(w, "l"); playOn(w, 10000, 2000); key(w, "l"); key(w, "l");
    const ab = cmds(w, "ab");
    return ab.length === 3 && ab[0].aMs === 10000 && ab[0].bMs === null && ab[1].aMs === 10000 && ab[1].bMs === 12000
      && ab[2].aMs === null && ab[2].bMs === null;
  };
  // 11. ...and the PAGE never seeks at B itself (mpv's loop is frame-exact; a second jump would fight it): a state past B
  const abNoPageSeek = (w) => {
    open(w); state(w, { timeMs: 10000, paused: false });
    key(w, "l"); playOn(w, 10000, 2000); key(w, "l");
    w.outbox.length = 0;
    state(w, { timeMs: 12030, paused: false, abA: 10000, abB: 12000 });   // mpv's last state before its jump
    w.pumpRaf(); w.pumpTimers(16); w.pumpRaf();
    return cmds(w, "seek").length === 0;
  };
  // 12. ...while the playhead runs inside the loop, the clock WRAPS at B like mpv (its jump back lands with the next state)
  const abWrap = (w) => { open(w); state(w, { timeMs: 11000, paused: false, abA: 10000, abB: 12000 }); w.pumpTimers(1500); return pauseAt(w) === 10500; };
  // 12b. (M3) ...but NOT when the playhead was already past B (mpv seeked out of the loop): it runs on from there
  const abNoWrapPast = (w) => { open(w); state(w, { timeMs: 13000, paused: false, abA: 10000, abB: 12000 }); w.pumpTimers(500); return pauseAt(w) === 13500; };
  // 13. THE SOUND TRACKS: every track of the file is playable (mpv decodes DTS too); a pick -> audio{ord} at once, and the
  //     app's analysis follows it (playerSet{audioOrd}) once the viewer stopped choosing
  const audioPick = (w) => {
    open(w, { tracks: TRACKS2, audioOrd: 0 }); state(w, { ord: 0, tracks: 2 });
    w.ids.tCC.click();
    const rows = w.ids.ccAudio.children;
    const allOn = rows.length === 2 && rows.every((b) => !b.disabled) && rows[0].getAttribute("aria-checked") === "true";
    w.outbox.length = 0;
    rows[1].click();
    const a = cmds(w, "audio");
    w.pumpTimers(900);
    const ps = lastOf(w, "playerSet");
    return allOn && a.length === 1 && a[0].ord === 1 && a[0].gen === 1 && !!ps && ps.audioOrd === 1;
  };
  // 14. ...the app's choice is ENFORCED when mpv plays another (the tray chose track 1, mpv started on 0)
  const audioEnforce = (w) => { open(w, { tracks: TRACKS2, audioOrd: 1 }); state(w, { ord: 0, tracks: 2 }); const a = cmds(w, "audio"); return a.length === 1 && a[0].ord === 1; };
  // 15. ...and the right-click menu's rows are the same rows, sending the same command
  const audioCtx = (w) => {
    open(w, { tracks: TRACKS2, audioOrd: 0 }); state(w, { ord: 0, tracks: 2 });
    w.ids.fileStage.dispatch("contextmenu", { target: w.ids.fileView, clientX: 100, clientY: 100 });
    const row = w.ids.ctxMenu.children.find((b) => b.dataset && b.dataset.k === "a1/1");
    w.outbox.length = 0;
    if (row) row.click();
    const a = cmds(w, "audio");
    return !!row && !row.disabled && a.length === 1 && a[0].ord === 1;
  };
  // 15b. (M3) the track asked holds NATIVE_TRACK_MS against a state from before it -- then mpv's word (it refused the switch)
  const trackHold = (w) => {
    open(w, { tracks: TRACKS2, audioOrd: 0 }); state(w, { ord: 0, tracks: 2 });
    w.ids.tCC.click(); w.ids.ccAudio.children[1].click();
    state(w, { ord: 0, tracks: 2 });                          // posted before mpv switched
    const a = w.win.__suvNative.heard();
    w.pumpTimers(3100);
    state(w, { ord: 0, tracks: 2 });
    return a === 1 && w.win.__suvNative.heard() === 0;
  };
  // 15c. L5 (fix round, review P6): mpv DROPPED the track asked (no decoder, a broken stream: ord -1 once its choice is
  //      final) -> no row says "heard", and the page does not ask mpv for it again by itself (only the viewer may)
  const trackNone = (w) => {
    open(w, { tracks: TRACKS2, audioOrd: 0 }); state(w, { ord: 0, tracks: 2, restarted: true });
    w.ids.tCC.click(); w.ids.ccAudio.children[1].click();
    w.pumpTimers(4000);
    state(w, { ord: -1, tracks: 2, restarted: true });
    w.ids.tCC.click(); w.ids.tCC.click();
    const rows = w.ids.ccAudio.children.map((b) => b.getAttribute("aria-checked"));
    w.outbox.length = 0;
    w.send(FILE({ tracks: TRACKS2, audioOrd: 1 }));           // the app's next post: the page enforces nothing over mpv's "none"
    return rows.length === 2 && rows.every((r) => r !== "true") && w.win.__suvNative.heard() === -1 && cmds(w, "audio").length === 0;
  };
  //      ...but BEFORE mpv's first restart, -1 is "not told yet": the app's choice is enforced (track 1 asked)
  const trackNotYet = (w) => {
    open(w, { tracks: TRACKS2, audioOrd: 1 }); state(w, { ord: -1, tracks: 2, restarted: false });
    const a = cmds(w, "audio");
    return a.length === 1 && a[0].ord === 1;
  };
  // 15d. L6 (fix round): a container the app reads no tracks of (AVI, TS...) -- its list comes from MPV (the app adopts it,
  //      container "mpv"): the rows are mpv's tracks, none disabled, a pick -> audio{ord} and playerSet for the analysis
  const MPVTRACKS = { state: "ready", container: "mpv", video: { codec: "", w: 0, h: 0 },
    audio: [{ ord: 0, track: 1, name: "", lang: "eng", codec: "MP3", ch: 2, def: false, play: true },
            { ord: 1, track: 2, name: "Commentary", lang: "fre", codec: "AC-3", ch: 6, def: false, play: true }], subs: [] };
  const mpvList = (w) => {
    open(w, { name: "dual.avi", tracks: MPVTRACKS, audioOrd: 0 }); state(w, { tracks: 2, ord: 0 });
    w.ids.tCC.click();
    const rows = w.ids.ccAudio.children;
    const two = rows.length === 2 && rows.every((b) => !b.disabled) && /Commentary/.test(rows[1].textContent);
    w.outbox.length = 0;
    rows[1].click();
    const a = cmds(w, "audio");
    w.pumpTimers(900);
    const ps = lastOf(w, "playerSet");
    return two && a.length === 1 && a[0].ord === 1 && !!ps && ps.audioOrd === 1 && w.doc.body.classList.contains("is-native");
  };
  //      ...and A says "No other sound track" only when mpv has one too (review P7): two tracks in mpv, its list not
  //      adopted yet -> "not ready yet"
  const aviSentence = (w) => {
    open(w, { name: "dual.avi", tracks: { state: "none" } }); state(w, { tracks: 2, ord: 0 });
    key(w, "a");
    const two = w.ids.fileToast.textContent;
    const w2 = W();
    open(w2, { name: "one.avi", tracks: { state: "none" } }); state(w2, { tracks: 1, ord: 0 });
    key(w2, "a");
    return two === "Sound tracks are not ready yet." && w2.ids.fileToast.textContent === "No other sound track to switch to";
  };
  // 16. THE PICTURE'S RECTANGLE: the picture box's rect, CSS px with 2 decimals, at once; again on a resize (the observer),
  //     on full screen, on the panel; never twice the same; the hole follows it
  const setBox = (w, l, t, wd, h) => { w.ids.fileView._rect = { left: l, top: t, width: wd, height: h, right: l + wd, bottom: t + h }; };
  const rectFlow = (w) => {
    setBox(w, 8, 70.5, 744.25, 400.5);
    open(w); state(w);
    const r1 = lastOf(w, "nativeRect");
    const hole = w.ids.nativeHole.style;
    const a = !!r1 && r1.x === 8 && r1.y === 70.5 && r1.w === 744.25 && r1.h === 400.5 && hole.left === "8px" && hole.height === "400.5px";
    setBox(w, 8, 70.5, 600, 300);
    w.outbox.length = 0;
    w.resizeFire();
    const r2 = lastOf(w, "nativeRect");
    const b = !!r2 && r2.w === 600 && r2.h === 300;
    w.outbox.length = 0;
    w.resizeFire();                                              // the same box: nothing
    const c = out(w, "nativeRect").length === 0;
    return a && b && c && w.resizeObserved().indexOf(w.ids.fileView) >= 0;
  };
  const rectFull = (w) => {
    setBox(w, 8, 70, 744, 400);
    open(w); state(w);
    setBox(w, 0, 0, 760, 560);
    w.outbox.length = 0;
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });   // (no observer fired: the page's own call)
    const r = lastOf(w, "nativeRect");
    return !!r && r.x === 0 && r.y === 0 && r.w === 760 && r.h === 560;
  };
  const rectPanel = (w) => {
    setBox(w, 8, 70, 744, 400);
    open(w); state(w);
    setBox(w, 8, 70, 744, 250);
    w.outbox.length = 0;
    w.ids.tPanel.click();                                        // the docked panel opens under the picture
    const r = lastOf(w, "nativeRect");
    return !!r && r.h === 250;
  };
  // 16b. (M3) the app's FIRST word of the engine gets the rectangle at once -- nothing to show yet: w 0 (mpv's window hidden)
  const infoRect = (w) => { info(w); const r = out(w, "nativeRect"); return r.length === 1 && r[0].w === 0 && r[0].h === 0; };
  // 17. A SOUND (mpv: no picture at its first restart) -> the card's ground, the window hidden (w 0), the stage opaque again
  const soundOnly = (w) => {
    setBox(w, 8, 70, 744, 400);
    open(w); state(w, { loaded: true, w: 0, h: 0, video: true });
    const a = w.doc.body.classList.contains("is-native");
    state(w, { w: 0, h: 0, video: false });
    const r = lastOf(w, "nativeRect");
    return a && !w.doc.body.classList.contains("is-native") && w.ids.fileBig.hidden === false
      && w.ids.fileView.classList.contains("is-audio") && !!r && r.w === 0 && r.h === 0;
  };
  // 18. ...and a file the app knows is a sound (talk.mp3) is never made see-through, even before mpv says so
  const soundByName = (w) => { open(w, { name: "talk.mp3", hasVideo: false, videoKnown: false }); return !w.doc.body.classList.contains("is-native") && w.ids.fileBig.hidden === false; };
  // ...nor one the engine's analysis found has no picture (whatever its name)
  const soundByAnalysis = (w) => { open(w, { name: "interview.mkv", hasVideo: false, videoKnown: true }); return !w.doc.body.classList.contains("is-native") && w.ids.fileBig.hidden === false; };
  // 19. THE STAGE: nothing of the page's own drawing over the picture -- the render modes do not apply (a stored "canvas"
  //     draws nothing, no blend class), Ctrl+Alt+V says why instead of cycling, the freeze watch says "native"
  const renderNative = (w) => {
    open(w); state(w);
    const a = w.ids.fileCanvas.hidden === true && rendered(w) === "blend" && !w.ids.fileView.classList.contains("render-canvas");
    w.win.dispatch("keydown", { key: "v", ctrlKey: true, altKey: true, target: w.doc.body });
    return a && w.lsStore["audiopb.tr.render"] === "canvas" && /native engine/.test(w.ids.fileToast.textContent);
  };
  const diagNative = (w) => {
    w.pumpTimers(3000);                                          // (the watch counts from a frame after the clock's 0)
    open(w); state(w, { timeMs: 1000, paused: false });
    w.pumpRaf();
    w.outbox.length = 0;
    w.pumpTimers(1500); w.pumpRaf();
    const d = lastOf(w, "playerDiag");
    return !!d && d.render === "native" && d.playing === true && d.ms === 1500;
  };
  // 20. FALLBACK: mpv refuses THIS opening (nativeError) -> the <video> takes it at the same place, playing if it played;
  //     no sentence (the <video>'s own failure, if any, is the one the viewer reads); another opening's error is ignored
  const fallbackError = (w) => {
    open(w); state(w, { timeMs: 30000, paused: false });
    w.send({ type: "nativeError", gen: 7, text: "not this one" });
    const still = w.doc.body.classList.contains("is-native") && !w.ids.fileVideo.src;
    w.outbox.length = 0;
    w.send({ type: "nativeError", gen: 1, text: "mpv could not play this file" });
    const v = w.ids.fileVideo;
    const switched = v.src === URL1 && !w.doc.body.classList.contains("is-native");
    metaOk(v);
    const r = lastOf(w, "nativeRect");
    return still && switched && Math.abs(v.currentTime - 30) < 0.01 && v.paused === false && w.ids.fileToast.hidden === true
      && !!r && r.w === 0 && cmds(w, "fallback").length === 0;
  };
  // 21. ...mpv silent for 20 s (it never said it has the file) -> the page tells the app (fallback: mpv lets go) and plays it
  const fallbackTimeout = (w) => {
    open(w);
    w.pumpTimers(19000);
    const before = !w.ids.fileVideo.src;
    w.pumpTimers(1500);
    const f = lastOf(w, "nativeCmd", "fallback");
    return before && !!f && f.gen === 1 && w.ids.fileVideo.src === URL1 && out(w, "translatorFilePlayFailed").length === 0;
  };
  // 21b. ...a page RELOADED after such a fallback: the app says it again before the replay -> the <video> at once, no 20 s wait
  const fallbackReload = (w) => { info(w); w.send({ type: "nativeError", gen: 1, text: "" }); w.send(FILE()); return w.ids.fileVideo.src === URL1 && cmds(w).length === 0; };
  // 22. ...the engine lost while the film plays (nativeInfo false) -> the <video> carries on from there
  const fallbackInfo = (w) => {
    open(w); state(w, { timeMs: 42000, paused: false });
    info(w, false, "mpv stopped");
    const v = w.ids.fileVideo;
    metaOk(v);
    return v.src === URL1 && Math.abs(v.currentTime - 42) < 0.01 && v.paused === false;
  };
  // 23. ...and when the <video> fails too: the app is asked for the sound copy and ONE sentence says so
  const bothFail = (w) => {
    open(w); state(w);
    w.send({ type: "nativeError", gen: 1, text: "mpv could not play this file" });
    w.ids.fileVideo._fire("error", { error: { code: 4 } });
    const pf = lastOf(w, "translatorFilePlayFailed");
    return !!pf && pf.seq === 1 && /^This format can't be played here/.test(w.ids.fileNotice.textContent) && w.ids.fileNotice.hidden === false;
  };
  // 21c. L7 (fix round): the 20 s wait counts from mpv's START of the opening (mpv was busy before: the previous file's end
  //      on a sleeping share) -- 30 s after the opening, 15 s after its Start, still waiting; 20 s after its Start, the <video>
  const startedWait = (w) => {
    open(w);
    w.pumpTimers(15000);
    state(w, { timeMs: 0, loaded: false, started: true, durMs: -1, video: undefined });
    w.pumpTimers(15000);
    const before = cmds(w, "fallback").length === 0 && !w.ids.fileVideo.src;
    w.pumpTimers(5500);
    return before && cmds(w, "fallback").length === 1;
  };
  // 21d. (M3) the openings the <video> plays instead are remembered 64 at most (the oldest go)
  const failedCap = (w) => {
    info(w);
    for (let g = 1; g <= 70; g++) w.send({ type: "nativeError", gen: g, text: "" });
    const f = w.win.__suvNative.failed();
    return f.length === 64 && f[0] === 7 && f[63] === 70;
  };
  // 24. NO SOUND COPY while mpv plays: an item with no URL (its folder could not be served) asks the app for nothing
  const noProxy = (w) => { open(w, { url: "" }); state(w); w.pumpTimers(25000); return out(w, "translatorFilePlayFailed").length === 0 && w.ids.fileNotice.hidden === true; };
  //     ...and once mpv lets it go (L2), the <video> has nothing to load: the copy is asked then (the page's door; the app's
  //     own -- Player::mpvDropped -- asks it too: once, requestProxy's guard)
  const fallbackNoUrl = (w) => {
    open(w, { url: "" }); state(w);
    w.send({ type: "nativeError", gen: 1, text: "mpv could not play this file" });
    const pf = out(w, "translatorFilePlayFailed");
    return pf.length === 1 && pf[0].seq === 1;
  };
  // 25. THE SUBTITLES FOLLOW MPV'S CLOCK: between two states the overlay moves on by itself (the frame loop, the
  //     interpolated playhead), a seek shows the target's line at once, and with no frame at all the 4 Hz timeupdate does it
  const subsFollow = (w) => {
    open(w); cues(w, 0, 40);
    state(w, { timeMs: 9980, paused: false });
    w.pumpRaf();
    const a = shown(w) === 9;
    w.pumpTimers(40); w.pumpRaf();                               // 10.020 s: no state since 9.980
    const b = shown(w) === 10;
    key(w, "ArrowRight");                                        // 15.021 s, at once
    const c = shown(w) === 15;
    return a && b && c;
  };
  const subsTick = (w) => {
    open(w); cues(w, 0, 40);
    state(w, { timeMs: 20000, paused: false });
    w.pumpTimers(300);
    state(w, { timeMs: 21100, paused: false });                  // (no frame pumped: the timeupdate path alone)
    return shown(w) === 21;
  };
  // 26. A PAGE RELOADED UNDER A FILM THAT PLAYS ON (mpv kept it): the page ADOPTS it -- no seek back to the last reported
  //     position, playing as it plays
  const adopt = (w) => {
    info(w);
    state(w, { timeMs: 45000, paused: false });                  // the app's `ready` answer: before the replay
    w.send(FILE({ fresh: false, startMs: 40000 }));
    w.pumpTimers(0);
    return cmds(w, "seek").length === 0 && w.ids.tPlay.classList.contains("is-playing") && pauseAt(w) === 45000;
  };
  // 26b. L4 (fix round, review P3): ...a film mpv LOOPS: the A-B button and band say so (mpv is not told it again)
  const abAdopted = (w) => {
    info(w);
    state(w, { timeMs: 11000, paused: false, abA: 10000, abB: 12000 });
    w.send(FILE({ fresh: false, startMs: 11000 }));
    w.pumpTimers(0);
    return w.ids.tLoop.classList.contains("is-on") && w.ids.seekLoop.hidden === false && cmds(w, "ab").length === 0;
  };
  // 27. a HIDDEN window never plays on: mpv is paused with it
  const hiddenPauses = (w) => { open(w); state(w, { timeMs: 1000, paused: false }); w.outbox.length = 0; w.send({ type: "translatorWindow", shown: false }); return cmds(w, "pause").length === 1; };
  // 27b. L8 (fix round, review P4): ...nor a film ADOPTED playing by a page (re)loaded while the window is closed
  const hiddenAdopt = (w) => {
    info(w);
    state(w, { timeMs: 45000, paused: false });
    w.send({ type: "translatorWindow", shown: false });
    w.send(FILE({ fresh: false, startMs: 40000 }));
    w.pumpTimers(0);
    return cmds(w, "pause").length === 1 && w.win.__suvNative.paused() === true && !w.ids.tPlay.classList.contains("is-playing");
  };
  // 28. THE CURSOR (N1's safety net): full screen's idle hide -> nativeCursor{show:false}; a move -> {show:true}; never to an
  //     app that did not speak of the engine
  const cursorNet = (w) => {
    open(w); state(w, { timeMs: 1000, paused: false });
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.outbox.length = 0;
    w.pumpTimers(2600);
    const a = out(w, "nativeCursor");
    w.dispatchDoc("pointermove", { target: w.ids.fileView, clientX: 50, clientY: 50, timeStamp: w.now() });
    const b = out(w, "nativeCursor");
    // (M3) ...said ONCE per change: every move calls it, a second move says nothing
    w.dispatchDoc("pointermove", { target: w.ids.fileView, clientX: 60, clientY: 50, timeStamp: w.now() });
    const c = out(w, "nativeCursor");
    return a.length === 1 && a[0].show === false && b.length === 2 && b[1].show === true && c.length === 2;
  };
  const cursorQuiet = (w) => {
    w.send(FILE()); metaOk(w.ids.fileVideo);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.ids.tPlay.click();
    w.pumpTimers(2600);
    w.dispatchDoc("pointermove", { target: w.ids.fileView, clientX: 50, clientY: 50, timeStamp: w.now() });
    return w.ids.fileStage.classList.contains("is-idle") === false && out(w, "nativeCursor").length === 0;
  };
  // 29. a STALE state (another opening's: mpv still on the previous file) moves nothing
  const staleGen = (w) => { open(w); state(w, { timeMs: 10000, paused: false }); state(w, { gen: 3, timeMs: 500000, paused: false }); w.pumpTimers(100); return pauseAt(w) === 10100; };
  // 30. THE NEXT ITEM: a new opening gets the page's settings again, its own gen on every command; the old one's states die
  const nextItem = (w) => {
    open(w); state(w, { timeMs: 5000, paused: false });
    w.outbox.length = 0;
    w.send(FILE({ jobId: 3, seq: 2, itemId: 13, name: "next.mkv" }));
    const sent = cmds(w);
    const idleButton = !w.ids.tPlay.classList.contains("is-playing");   // (mpv opens it paused: the old "playing" is gone)
    state(w, { gen: 1, timeMs: 9000, paused: false });           // the old one's, late
    state(w, { gen: 2, timeMs: 0 });
    w.ids.tPlay.click();
    const p = lastOf(w, "nativeCmd", "play");
    return sent.length === 3 && sent.every((m) => m.gen === 2) && sent.some((m) => m.cmd === "volume") && !!p && p.gen === 2
      && idleButton;
  };
  // 30b. "FOLLOW THE PLAYHEAD" + the RESUME POSITION: the 5 s report while it plays and the seek's report carry mpv's time
  const followPlayhead = (w) => {
    open(w); state(w, { timeMs: 20000, paused: false });
    w.outbox.length = 0;
    playOn(w, 20000, 4000); w.pumpTimers(1000);                  // (mpv speaks every second; the 5 s report reads 25.000 s)
    const tick = lastOf(w, "playerPos");
    key(w, "ArrowRight");                                        // 25.000 + 5 = 30.001 s
    w.pumpTimers(300);
    const sk = lastOf(w, "playerPos");
    return !!tick && tick.reason === "tick" && tick.ms === 25000 && tick.playing === true && tick.gen === 2
      && !!sk && sk.reason === "seek" && sk.ms === 30001;
  };
  // 30c. THE EDITOR: "⇤ Here" starts the selected line at mpv's playhead (12.345 s)
  const editorHere = (w) => {
    open(w); cues(w, 0, 40);
    state(w, { timeMs: 12345, paused: true });
    key(w, "e");
    w.outbox.length = 0;
    w.ids.edStartHere.click();
    const e = lastOf(w, "subEdit");
    return !!e && e.target === "audiopb" && e.gen === 2 && e.remove[0] === 12 && e.add.length === 1 && e.add[0].s === 12345 && e.add[0].e === 13000;
  };
  // 31. THE END: mpv's eof (paused there: keep-open) -> one "ended" report (the app plays the next), however often it is said
  const endedOnce = (w) => {
    open(w); state(w, { timeMs: 599000, paused: false });
    w.outbox.length = 0;
    state(w, { timeMs: 600000, paused: true, eof: true });
    state(w, { timeMs: 600000, paused: true, eof: true });
    return out(w, "playerPos").filter((p) => p.reason === "ended").length === 1;
  };
  // 32. ...Play at the end starts from the top (MpvPlayer seeks there itself): a state still saying "the end" does not undo it
  const playAtEnd = (w) => {
    open(w); state(w, { timeMs: 600000, paused: true, eof: true });
    w.outbox.length = 0;
    w.ids.tPlay.click();
    state(w, { timeMs: 600000, paused: true, eof: true });       // posted before mpv read the play
    return cmds(w, "play").length === 1 && cmds(w, "seek").length === 0 && w.ids.tPlay.classList.contains("is-playing")
      && pauseAt(w) === 0;
  };
  // 32b. L1 (fix round, review P2): ...but a SEEK still on its way wins: ← at the end then Space at once plays from 595 s
  const playAfterSeekAtEnd = (w) => {
    open(w); state(w, { timeMs: 600000, paused: true, eof: true });
    w.outbox.length = 0;
    key(w, "ArrowLeft");
    key(w, " ");
    const sk = lastOf(w, "nativeCmd", "seek");
    return !!sk && sk.ms === 595001 && w.win.__suvNative.time() === 595001 && cmds(w, "play").length === 1;
  };
  // 32c. (M3) mpv's end, told before its keep-open pause is: the page stops there (no clock running past the end)
  const eofStops = (w) => {
    open(w); state(w, { timeMs: 599000, paused: false });
    state(w, { timeMs: 600000, paused: false, eof: true });
    return w.win.__suvNative.paused() === true && !w.ids.tPlay.classList.contains("is-playing");
  };
  // 33. M2 (fix round): THE SEEK HOLD EXPIRES ON ITS OWN -- mpv's last word taken again from a timer, no other state needed.
  //     A truncated file (review P8: the header says 600 s, the data ends at 500 s): the click at 90 % -> mpv's answer
  //     "500 s, the end", then silence -> 3 s later the page is at 500 s and "ended" is reported (the queue goes on)
  const clickBar = (w, x, width) => {
    w.ids.seekTrack._rect = { left: 0, top: 0, width, height: 10, right: width, bottom: 10 };
    w.ids.seek.dispatch("pointerdown", { button: 0, clientX: x, pointerId: 1, target: w.ids.seek });
    w.ids.seek.dispatch("pointerup", { button: 0, clientX: x, pointerId: 1, target: w.ids.seek });
  };
  const seekExpiresAtEnd = (w) => {
    open(w); state(w, { timeMs: 450000, paused: false });
    w.outbox.length = 0;
    clickBar(w, 900, 1000);
    const sk = cmds(w, "seek");
    state(w, { timeMs: 450100, paused: false, seeking: true });
    state(w, { timeMs: 500000, paused: false, eof: true });
    state(w, { timeMs: 500000, paused: true, eof: true });
    w.pumpTimers(2900);
    const held = out(w, "playerPos").filter((p) => p.reason === "ended").length === 0;
    w.pumpTimers(200);
    return sk.length === 1 && held && out(w, "playerPos").filter((p) => p.reason === "ended").length === 1
      && w.win.__suvNative.time() === 500000;
  };
  //     ...paused, mpv landed 2 s off an exact seek (an index-less AVI / TS: review P8b): the real position shows
  const seekExpiresPaused = (w) => {
    open(w); state(w, { timeMs: 100000, paused: true });
    clickBar(w, 200, 600);
    const sk = lastOf(w, "nativeCmd", "seek");
    state(w, { timeMs: 203000, paused: true, seeking: true });
    state(w, { timeMs: 203000, paused: true });
    w.pumpTimers(2900);
    const held = w.win.__suvNative.time();
    w.pumpTimers(200);
    return !!sk && held === sk.ms && w.win.__suvNative.time() === 203000;
  };
  //     ...a seek mpv never did (sent before its file was open: review P9): its own position again after the hold
  const seekLost = (w) => {
    info(w); w.send(FILE());
    state(w, { timeMs: 0, loaded: false, video: undefined, durMs: -1 });
    clickBar(w, 500, 1000);
    const sk = cmds(w, "seek");
    state(w, { timeMs: 0, loaded: true });
    w.pumpTimers(3100);
    return sk.length === 1 && w.win.__suvNative.time() === 0;
  };
  //     ...a file mpv says CANNOT be seeked (once it played: `restarted`): the seek is asked (its cache may allow it) but
  //     nothing is held -- the playhead stays mpv's; before its first restart `seekable` is not its word: held as any seek
  const notSeekable = (w) => {
    open(w); state(w, { timeMs: 10000, paused: true, seekable: false, restarted: true });
    w.outbox.length = 0;
    key(w, "ArrowRight");
    return cmds(w, "seek").length === 1 && w.win.__suvNative.time() === 10000;
  };
  const seekableNotYet = (w) => {
    open(w); state(w, { timeMs: 10000, paused: true, seekable: false, restarted: false });
    key(w, "ArrowRight");
    return w.win.__suvNative.time() === 15001;
  };
  // 34. (M3) a seek to where it already is (under 50 ms: the opening position) is not sent; the target is clamped to
  //     MPV's duration (the app may think the file longer: a header's duration)
  const seekSame = (w) => {
    open(w); state(w, { timeMs: 10000, paused: true });
    w.outbox.length = 0;
    clickBar(w, 10, 600);                                        // 10/600 of 600 s = 10.000 s (+1 ms)
    return cmds(w, "seek").length === 0;
  };
  const seekClamp = (w) => {
    open(w, { totalMs: 700000 }); state(w, { timeMs: 598000, paused: true });
    w.outbox.length = 0;
    key(w, "ArrowRight");
    const s = lastOf(w, "nativeCmd", "seek");
    return !!s && s.ms === 600000;
  };

  // ---- the checks ------------------------------------------------------------------------------------------------------------
  section("THE NATIVE VIDEO ENGINE (1.1.0) -- the backend: mpv when the app has it, the <video> otherwise");
  ok("the app has the engine: mpv plays the item -- the <video> never loaded, the stage see-through, the page's settings sent for opening 1", nativeChosen(W()));
  ok("an app that never speaks of it (an older SuVtitle): the <video> plays as before, and no native message goes out", olderApp(W()));
  ok("the engine NOT available (nativeInfo false): the <video>, and no command to an engine that is not there", unavailable(W()));
  section("THE NATIVE VIDEO ENGINE -- N0's interpolated clock");
  ok("a state says 10.000 s playing: 950 ms later, no state in between, the playhead is 10.950 s", clockInterp(W()));
  ok("...x mpv's speed (2x: 500 ms of wall time = 1 s of film)", clockSpeed(W()));
  ok("a seek HOLDS the clock at its target (15.001 s, exact) -- a state from before the seek does not drag it back", seekHold(W()));
  ok("...mpv's answer near the target restarts it from mpv's own time (15.020 + 0.300)", seekConfirm(W()));
  ok("mpv's own `seeking` holds the clock too (an A-B jump, a restart)", mpvSeeking(W()));
  ok("the clock's error is measured in steady playback only (+7, -3: mean 2, |mean| 5, max 7; a pause not counted) -- window.__suvNative", driftMeasure(W()));
  ok("...a state 800 ms off the clock is COUNTED (big: 1), never silently dropped", driftBig(W()));
  ok("a speed asked holds against a state from before it (] then a stale speed-1 state: the clock runs at 1.25x)", speedHold(W()));
  ok("the clock never runs past mpv's duration (599.500 + 1 s -> 600.000)", clockCap(W()));
  section("THE NATIVE VIDEO ENGINE -- M1 (fix round): mpv quiet while it plays = stalled, the clock HOLDS");
  ok("one playing state at 10 s, then 30 s of silence: the playhead stops at 11.5 s (1.5 s on), the hold counted once", quietHold(W()));
  ok("...so keep-ahead does not pause a film mpv has not played (ranges to 16 s, mpv stuck at 10 s: no pause, the page at 11.5 s)", quietKeepAhead(W()));
  ok("...never on a healthy slow film: 1 fps at 0.25x (a state every 4.35 s, measured) runs on, no hold", quietSlowFilm(W()));
  ok("...a film's own pace is learnt: 1 fps at 1x, then 3.5 s between two frames (< 4 x 1 s): no hold (16.5 s)", quietLearned(W()));
  section("THE NATIVE VIDEO ENGINE -- the transport: one command per change, for the current opening");
  ok("play / pause -> play / pause; a state posted before mpv read the play does not flip the button back", playPause(W()));
  ok("] -> speed 1.25; a volume step -> volume 95 (the speed not sent again); M M -> mute on, off", transport(W()));
  ok("A-B: L = A alone (10.000), L = the pair (10.000-12.000: mpv loops, jumping to A), L = cleared (null, null)", abCmds(W()));
  ok("...the page never seeks at B itself (a state past B: no seek -- mpv's loop is frame-exact)", abNoPageSeek(W()));
  ok("...inside the loop the clock wraps at B like mpv does (11.000 + 1.5 s -> 10.500)", abWrap(W()));
  ok("...but not when the playhead was already past B (13.000 + 0.5 s -> 13.500)", abNoWrapPast(W()));
  section("THE NATIVE VIDEO ENGINE -- the film's sound tracks (the CC panel, the right-click menu, A)");
  ok("every track is playable (DTS too: mpv decodes it); a pick -> audio{ord:1} at once, then playerSet{audioOrd:1} for the analysis", audioPick(W()));
  ok("the app's choice is ENFORCED when mpv plays another (tray: 1, mpv: 0 -> audio{ord:1})", audioEnforce(W()));
  ok("the right-click menu's rows are the CC panel's, and send the same command", audioCtx(W()));
  ok("the track asked holds 3 s against a state from before it; then mpv's word (it played 0 after all)", trackHold(W()));
  ok("L5: mpv drops the track asked (ord -1 once its choice is final): no row heard, and the page does not ask mpv again by itself", trackNone(W()));
  ok("...before mpv's first restart -1 is \"not told yet\": the app's choice (track 1) is enforced", trackNotYet(W()));
  ok("L6: an AVI's tracks from MPV (the app adopted them: container \"mpv\"): two rows, none disabled, a pick -> audio{ord:1} + playerSet", mpvList(W()));
  ok("...A says \"No other sound track\" only when mpv has one too (two in mpv, not adopted yet: \"not ready yet\")", aviSentence(W()));
  section("THE NATIVE VIDEO ENGINE -- the picture's rectangle, the see-through stage");
  ok("nativeRect = the picture box, CSS px with 2 decimals, at once; again when the observer says it changed; never twice the same; the hole follows", rectFlow(W()));
  ok("...full screen sends the new box (the page's own call, no observer needed)", rectFull(W()));
  ok("...the docked panel opening sends it too", rectPanel(W()));
  ok("...and the app's first word of the engine gets it at once (nothing to show: w 0, mpv's window hidden)", infoRect(W()));
  ok("a SOUND (no picture at mpv's first restart): the card's ground, the stage opaque again, nativeRect w 0 (the window hidden)", soundOnly(W()));
  ok("...a file the app knows is a sound (talk.mp3) is never made see-through", soundByName(W()));
  ok("...nor one the analysis found has no picture (interview.mkv: videoKnown, !hasVideo)", soundByAnalysis(W()));
  ok("the render modes do not apply over mpv: a stored \"canvas\" draws nothing, Ctrl+Alt+V says why and keeps the choice", renderNative(W({ ls: { "audiopb.tr.render": "canvas" } })));
  ok("the freeze watch says render \"native\", playing from mpv's state", diagNative(W()));
  {
    const css = (html.match(/<style>([\s\S]*?)<\/style>/) || ["", ""])[1];
    ok("CSS: body.is-native has no ground, the stage and the picture box none either, #nativeHole lays the theme's ground around the picture",
       /body\.is-native \{ background: transparent; \}/.test(css) && /body\.is-native \.file-stage, body\.is-native \.file-view \{ background: transparent; \}/.test(css)
       && /body\.is-native #nativeHole \{[^}]*box-shadow: 0 0 0 200vmax var\(--bg\);/.test(css) && /#nativeHole \{ display: none; \}/.test(css)
       && /<div id="nativeHole" aria-hidden="true"><\/div>/.test(html));
  }
  section("THE NATIVE VIDEO ENGINE -- the automatic fallback to the <video>");
  ok("nativeError for THIS opening -> the <video> at the same place (30 s), playing; another opening's error ignored; no sentence", fallbackError(W()));
  ok("mpv silent for 20 s -> nativeCmd fallback{gen:1} (mpv lets go) and the <video> plays it (no sound copy asked)", fallbackTimeout(W()));
  ok("a page reloaded after a fallback: the app's nativeError before the replay -> the <video> at once (no 20 s wait)", fallbackReload(W()));
  ok("the engine lost mid-film (nativeInfo false) -> the <video> carries on from 42 s", fallbackInfo(W()));
  ok("...the <video> failing too: the sound copy asked (translatorFilePlayFailed{seq}) and ONE sentence", bothFail(W()));
  ok("no sound copy while mpv plays: an item with no URL asks the app for nothing", noProxy(W()));
  ok("...mpv lets it go (nativeError): the <video> has nothing to load -- the copy is asked then (translatorFilePlayFailed{seq:1}, once)", fallbackNoUrl(W()));
  ok("L7: the 20 s wait counts from mpv's Start of the opening (`started`): 30 s after the opening still waiting, 20 s after its Start the <video>", startedWait(W()));
  ok("the openings the <video> plays instead are remembered 64 at most (70 refusals: 7..70 kept)", failedCap(W()));
  section("THE NATIVE VIDEO ENGINE -- the subtitles follow mpv's clock");
  ok("between two states the overlay moves on by itself (9.980 -> 10.020 s: line 9 -> line 10); a seek shows the target's line at once", subsFollow(W()));
  ok("...with no frame at all, the 4 Hz timeupdate moves it (line 21)", subsTick(W()));
  section("THE NATIVE VIDEO ENGINE -- the rest of the player on mpv");
  ok("a page reloaded under a film mpv plays on ADOPTS it: no seek back, playing, at mpv's time", adopt(W()));
  ok("L4: ...a film mpv LOOPS: the A-B button lit and the band shown (mpv not told it again)", abAdopted(W()));
  ok("a hidden window pauses mpv", hiddenPauses(W()));
  ok("L8: ...a film ADOPTED playing by a page (re)loaded in a closed window is paused too", hiddenAdopt(W()));
  ok("full screen's idle hide -> nativeCursor{show:false}; a move -> {show:true} (N1's cursor safety net)", cursorNet(W()));
  ok("...never sent to an app that did not speak of the engine", cursorQuiet(W()));
  ok("a state of another opening (mpv still on the previous file) moves nothing", staleGen(W()));
  ok("the next item: its opening gets volume / mute / speed again, every command carries gen 2, the old one's states die, the button says paused", nextItem(W()));
  ok("follow the playhead / the resume position: the 5 s report (25.000 s) and the seek's (30.001 s) carry mpv's clock", followPlayhead(W()));
  ok("the editor's \"⇤ Here\" starts the selected line at mpv's playhead (12.345 s)", editorHere(W()));
  ok("mpv's end -> ONE \"ended\" report (the app plays the next), however often the end is said", endedOnce(W()));
  ok("Play at the end starts from the top (no seek from the page: MpvPlayer does it); a stale \"end\" state does not undo it", playAtEnd(W()));
  ok("L1: ...but a seek still on its way wins: <- at the end then Space at once plays from 595.001 s", playAfterSeekAtEnd(W()));
  ok("mpv's end told before its keep-open pause: the page stops there", eofStops(W()));
  section("THE NATIVE VIDEO ENGINE -- M2 (fix round): the seek hold expires on its own; seekable:false honoured");
  ok("a truncated file: the seek answered 40 s off at its end, then silence -> 3 s later the page is at 500 s and \"ended\" is reported", seekExpiresAtEnd(W()));
  ok("...paused, mpv 2 s off an exact seek: the target for 3 s, then the real position (203 s)", seekExpiresPaused(W()));
  ok("...a seek mpv never did (before its file was open): mpv's position again after the hold", seekLost(W()));
  ok("a file mpv says cannot be seeked (after its first restart): the seek is asked, nothing held -- the playhead stays mpv's", notSeekable(W()));
  ok("...before its first restart `seekable` is not mpv's word: the seek is held as any (15.001 s)", seekableNotYet(W()));
  ok("a seek to where it already is is not sent (10.001 s at 10.000 s)", seekSame(W()));
  ok("the target is clamped to MPV's duration (the app thought 700 s: -> 600.000)", seekClamp(W()));

  // ---- the contract, both ways, and the app's half (bridge.cpp) --------------------------------------------------------------
  section("THE NATIVE VIDEO ENGINE -- the contract both ways, and the bridge's half (bridge.cpp)");
  const pageOnly = script.slice(0, script.indexOf("Design-time mock"));
  const bridge = P.cpp("ui/bridge.cpp");
  const sent = [...new Set([...pageOnly.matchAll(/type:\s*"(native\w+)"/g)].map((m) => m[1]))];
  ok("every native* message the page SENDS is handled by the bridge (nativeRect, nativeCmd, nativeCursor)",
     sent.length === 3 && sent.every((t) => new RegExp('type == L"' + t + '"').test(bridge)), sent);
  const emitted = [...new Set([...bridge.matchAll(/L"type",\s*L"(native\w+)"/g)].map((m) => m[1]))];
  ok("every native* message the bridge EMITS is consumed by the page (nativeInfo, nativeState, nativeError)",
     emitted.length === 3 && emitted.every((t) => pageOnly.indexOf('msg.type === "' + t + '"') >= 0), emitted);
  const nativeSends = [...pageOnly.matchAll(/send\(\{ type: "native\w+"[^}]*\}/g)].map((m) => m[0]).concat(
    [...pageOnly.matchAll(/cmd\("\w+"(?:, \{[^}]*\})?\)/g)].map((m) => m[0]));
  ok("no path, file, folder, url or name in any native message the page sends (mpv plays what the APP opened)",
     nativeSends.length >= 8 && nativeSends.every((s) => !/\b(path|file|folder|dir|url|uri|name)\s*:/i.test(s)), nativeSends);
  // the bridge's rules, as source pins (no C++ test runs the bridge): each with its mutants, which must make it FAIL
  const fnBody = (src, name) => {
    const at = src.indexOf(name + "(");
    if (at < 0) return "";
    const o = src.indexOf("{", at);
    let d = 0, i = o;
    for (; i < src.length; ++i) { if (src[i] === "{") d++; else if (src[i] === "}" && --d === 0) break; }
    return src.slice(o, i + 1);
  };
  const BPINS = [
    ["B1: the CURRENT item is opened in mpv from the core's session (path, start, sound track, seq) -- never from the page; once per opening",
      (s) => /native_->open\(s\.path, s\.startMs, s\.audioOrd, seq\);/.test(fnBody(s, "void Bridge::syncNative"))
        && /if \(seq == nativeSeq_ \|\| seq == nativeFellBack_\) return;/.test(fnBody(s, "void Bridge::syncNative"))
        && !/native_->open\(/.test(fnBody(s, "void Bridge::handleNativeMessage")),
      [["native_->open(s.path, s.startMs, s.audioOrd, seq);", "native_->open(s.path, 0, s.audioOrd, seq);"],
       ["if (seq == nativeSeq_ || seq == nativeFellBack_) return;", "if (seq == nativeFellBack_) return;"]]],
    ["B2: the sync runs after every page message, --play, the engine's drain (scans) and the 1 Hz tick; the item closed = mpv stops",
      (s) => /handleWebMessage\(json\);\s*\}\s*syncNative\(\);/.test(s) && /pollTranslatorOnce\(\);\s*syncNative\(\);/.test(s)
        && /left out\."\);\s*syncNative\(\);/.test(s) && /syncNative\(\);\s*if \(nativeDirty_\) postNativeState\(true, false\);/.test(fnBody(s, "void Bridge::uiTick"))
        && /if \(!seq\) \{\s*if \(nativeSeq_\) \{\s*if \(native_\) native_->stop\(\);/.test(fnBody(s, "void Bridge::syncNative")),
      [["                syncNative();   // 1.1.0 NATIVE: an item opened or closed by this message", "                // 1.1.0 NATIVE: an item opened or closed by this message"],
       ["            if (native_) native_->stop();\n            nativeSeq_ = 0;\n            resetNativeItem();\n        }\n        return;",
        "            nativeSeq_ = 0;\n            resetNativeItem();\n        }\n        return;"]]],
    ["B3: a command of ANOTHER opening (a stale gen) is dropped; the page's numbers are bounded",
      (s) => /if \(!v\.u64\(L"gen", gen\) \|\| gen == 0 \|\| gen != nativeSeq_\) return;/.test(fnBody(s, "void Bridge::handleNativeMessage"))
        && /nativeNum\(v\.get\(L"rate"\), 0\.25, 4\.0, d\)/.test(s) && /v\.u64\(L"pct", n\) && n <= 100/.test(s),
      [["if (!v.u64(L\"gen\", gen) || gen == 0 || gen != nativeSeq_) return;", "if (!v.u64(L\"gen\", gen) || gen == 0) return;"],
       ["nativeNum(v.get(L\"rate\"), 0.25, 4.0, d)", "nativeNum(v.get(L\"rate\"), 0, 1e9, d)"]]],
    ["B4: mpv's events of ANOTHER opening are dropped; an Error of this one = the page's <video> plays it (nativeError, mpv stops)",
      (s) => /if \(!nativeSeq_ \|\| e\.gen != nativeSeq_\) return;/.test(fnBody(s, "void Bridge::onNativeEvent"))
        && /case K::Error:\s*nativeFallBack\(nativeSeq_,/.test(fnBody(s, "void Bridge::onNativeEvent"))
        && /if \(seq == nativeSeq_\) \{\s*if \(native_\) native_->stop\(\);/.test(fnBody(s, "void Bridge::nativeFallBack"))
        && /L"type", L"nativeError"/.test(fnBody(s, "void Bridge::nativeFallBack")),
      [["if (!nativeSeq_ || e.gen != nativeSeq_) return;", "if (!nativeSeq_) return;"],
       ["        case K::Error:\n            nativeFallBack(nativeSeq_,", "        case K::Error:\n            if (false) nativeFallBack(nativeSeq_,"]]],
    ["B5: `ready` gets nativeInfo + mpv's state (+ a fallback the old page knew of) BEFORE the player's replay (the page must know which backend plays what it names)",
      (s) => { const r = s.slice(s.indexOf('if (type == L"ready") {')), body = fnBody(s, "void Bridge::replayNative");
        return r.indexOf("replayNative();") > 0 && r.indexOf("replayNative();") < r.indexOf("player_->windowReady();")
          && /postNativeInfo\(true\);\s*postNativeState\(true, true\);/.test(body)
          && /player_->session\(\)\.seq == nativeFellBack_[\s\S]*L"type", L"nativeError"/.test(body); },
      [["        replayNative();\n", ""],
       ["    if (nativeFellBack_ && player_ && player_->loaded() && player_->session().seq == nativeFellBack_ && webview_) {",
        "    if (false) {"]]],
    // (fix round L2 / L3, 02/11: the bridge's HOLD of the sound copy is gone -- the CORE asks none while mpv plays the item, and
    //  is told when it falls back; the old B6 pinned the hold)
    ["B6: L2 / L3 -- the SOUND COPY is the <video>'s: the core asks the bridge whether mpv plays the item (PlayerHost::mpvPlays = nativeWanted), sendProxy forwards (nothing held), and the core is told when the item falls back (mpvDropped: nativeFallBack, syncNative's not-usable branch)",
      (s) => /bool Bridge::PlayerHost::mpvPlays\(\) \{ return b\.nativeWanted\(\); \}/.test(s)
        && /void Bridge::PlayerHost::sendProxy\(const pbt::trpipe::WireProxy& p\) \{ if \(b\.translator_\) b\.translator_->requestProxy\(p\); \}/.test(s)
        && /if \(player_\) player_->mpvDropped\(seq\);/.test(fnBody(s, "void Bridge::nativeFallBack"))
        && /postNativeInfo\(false\);[\s\S]{0,400}player_->mpvDropped\(seq\);\s*return;/.test(fnBody(s, "void Bridge::syncNative"))
        && !/nativeProxy/.test(s),
      [["bool Bridge::PlayerHost::mpvPlays() { return b.nativeWanted(); }", "bool Bridge::PlayerHost::mpvPlays() { return false; }"],
       ["    if (player_) player_->mpvDropped(seq);\n}", "}"],
       ["        player_->mpvDropped(seq);\n        return;", "        return;"]]],
    ["B7: nativeState = MpvPlayer::state() of THIS opening (else the opening as asked), <= 40 Hz unless a discrete change; the speed a decimal",
      (s) => /const bool mine = s\.gen == nativeSeq_;/.test(s) && /postNativeState\(e\.kind != K::Time, false\);/.test(s)
        && /if \(!now && !force && t - nativePostedAt_ < kNativeStateEveryMs\)/.test(s) && /L",\\"speed\\":%lld\.%03lld\}"/.test(s),
      [["postNativeState(e.kind != K::Time, false);", "postNativeState(false, false);"],
       ["const bool mine = s.gen == nativeSeq_;", "const bool mine = true;"]]],
    ["B9: an item opened while the engine is NOT usable is the <video>'s (never opened later) and the page is ALWAYS told so; before the first attachNative nothing falls back",
      (s) => { const b = fnBody(s, "void Bridge::syncNative");
        return /if \(!nativeUsable\(\)\) \{[\s\S]*?if \(!nativeKnown_\) return;\s*nativeSeq_ = 0;\s*nativeFellBack_ = seq;[\s\S]*?JsonW j;\s*j\.obj\(\)\.kv\(L"type", L"nativeError"\)/.test(b)
          && !/if \(nativeInfoAvail_\)/.test(b); },
      [["        JsonW j;\n        j.obj().kv(L\"type\", L\"nativeError\").kv(L\"gen\", static_cast<long long>(seq))\n               .kv(L\"text\", w16(native_ ? native_->why() : nativeWhy_)).end();\n        postToTranslatorView(j.take());\n        postNativeInfo(false);",
        "        if (nativeInfoAvail_) {\n        JsonW j;\n        j.obj().kv(L\"type\", L\"nativeError\").kv(L\"gen\", static_cast<long long>(seq))\n               .kv(L\"text\", w16(native_ ? native_->why() : nativeWhy_)).end();\n        postToTranslatorView(j.take());\n        }\n        postNativeInfo(false);"],
       ["        if (!nativeKnown_) return;\n        nativeSeq_ = 0;", "        nativeSeq_ = 0;"]]],
    ["B8: the cursor safety net goes to main.cpp (kWmTranslatorWindow, wParam 5); the picture's rectangle to MpvPlayer::placeCss",
      (s) => /PostMessageW\(h, kWmTranslatorWindow, kWinNativeCursor, v\.flag\(L"show", true\) \? 1 : 0\)/.test(s) && /native_->placeCss\(x, y, w, hh\);/.test(s)
        && /static constexpr WPARAM kWinNativeCursor = 5;/.test(P.cpp("ui/bridge.h")),
      [["native_->placeCss(x, y, w, hh);", "(void)x;"]]],
    ["B10: L6 -- mpv's sound tracks go to the core for the CURRENT opening only (at Tracks / Loaded / Track / its first Restart, once per change), the file's strings cut and capped (title 4096, language 64, codec 64: nativeText), mpv's id as the track, the container \"mpv\"",
      (s) => { const ev = fnBody(s, "void Bridge::onNativeEvent"), of = fnBody(s, "void Bridge::offerNativeTracks");
        return /case K::Tracks:\s*nativeTracksSeen_ = nativeVideoKnown_ = true;\s*offerNativeTracks\(\);/.test(ev)
          && /case K::Loaded:\s*nativeLoaded_ = true; offerNativeTracks\(\); break;/.test(ev) && /case K::Track:\s*offerNativeTracks\(\); break;/.test(ev)
          && /if \(!nativeRestarted_\) \{ nativeRestarted_ = true; offerNativeTracks\(\); \}/.test(ev)
          && /player_->session\(\)\.seq != nativeSeq_\) return;/.test(of) && /if \(s\.gen != nativeSeq_\) return;/.test(of)
          && /t\.name = nativeText\(a\.title, 4096\);/.test(of) && /t\.lang = nativeText\(a\.lang, 64\);/.test(of)
          && /t\.codec = nativeCodecName\(nativeText\(a\.codec, 64\)\);/.test(of) && /if \(sig == nativeTracksSig_\) return;/.test(of)
          && /info->container = "mpv";/.test(of) && /if \(c == 0 \|\| o\.size\(\) >= cap\) break;\s*if \(c < 0x20 \|\| c == 0x7f\) continue;/.test(s); },
      [["    if (s.gen != nativeSeq_) return;\n    auto info", "    auto info"],
       ["t.name = nativeText(a.title, 4096);", "t.name = a.title;"],
       ["            nativeTracksSeen_ = nativeVideoKnown_ = true;\n            offerNativeTracks();", "            nativeTracksSeen_ = nativeVideoKnown_ = true;"],
       ["        if (c == 0 || o.size() >= cap) break;", "        if (o.size() >= cap) break;"]]],
    ["B11: nativeState says `started` (mpv's Start of THIS opening: the page's 20 s wait restarts, L7), `restarted` (its first Restart: seekable and ord -1 are mpv's word, M2 / L5) and `video` = the track list's hasVideo once Tracks came (L9: no sound-card flash, a cover is no picture), else the first frame's size",
      (s) => { const ps = fnBody(s, "void Bridge::postNativeState"), ev = fnBody(s, "void Bridge::onNativeEvent");
        return /j\.kv\(L"started", mine && nativeStarted_\);/.test(ps) && /j\.kv\(L"restarted", mine && nativeRestarted_\);/.test(ps)
          && /if \(mine && nativeVideoKnown_\) j\.kv\(L"video", nativeTracksSeen_ \? s\.hasVideo : s\.w > 0 && s\.h > 0\);/.test(ps)
          && /case K::Start:\s*resetNativeItem\(\); nativeStarted_ = true; break;/.test(ev)
          && /if \(!nativeTracksSeen_ && e\.w > 0 && e\.h > 0\) nativeVideoKnown_ = true;/.test(ev)
          && /if \(!nativeTracksSeen_\) nativeVideoKnown_ = true;/.test(ev); },
      [["j.kv(L\"video\", nativeTracksSeen_ ? s.hasVideo : s.w > 0 && s.h > 0);", "j.kv(L\"video\", s.w > 0 && s.h > 0);"],
       ["case K::Start:    resetNativeItem(); nativeStarted_ = true; break;", "case K::Start:    resetNativeItem(); break;"],
       ["j.kv(L\"started\", mine && nativeStarted_);", "j.kv(L\"started\", nativeStarted_);"]]],
  ];
  const pinAll = (src, PINS) => {
    for (const [name, test, muts] of PINS) {
      ok(name, test(src));
      const tag = name.slice(0, name.indexOf(":"));
      muts.forEach(([find, rep], i) => {
        const n = src.split(find).length - 1;
        const caught = n === 1 && !test(src.replace(find, () => rep));
        ok("  MUTANT " + tag + "." + (i + 1) + " is caught" + (n !== 1 ? " (the mutation applies " + n + " times)" : ""), caught);
      });
    }
  };
  pinAll(bridge, BPINS);
  // the CORE's half (TrPlayer.h, fix round 02/11) -- its behaviour is player_smoke's (tests\cpp: the adoption's "never restart
  // an analysis on aid=auto's pick" since U1); these pin the rules the bridge relies on, each with its mutants
  const core = P.cpp("ui/TrPlayer.h");
  const CPINS = [
    ["C1: L2 -- while mpv plays the item the core asks for NO sound copy (mapNow's no-URL path; a <video> failure that is stale), and asks it when mpv lets an item with no URL go (mpvDropped)",
      (s) => /s_\.url\.clear\(\);\s*if \(!host_\.mpvPlays\(\)\) requestProxy\(\);/.test(fnBody(s, "void mapNow"))
        && /if \(host_\.mpvPlays\(\)\) return;[^\n]*\n\s*requestProxy\(\);/.test(fnBody(s, "void playFailed"))
        && /s_\.mpvFinal = true;\s*mpvWaitDone\(\);\s*if \(s_\.url\.empty\(\)\) requestProxy\(\);/.test(fnBody(s, "void mpvDropped")),
      [["            if (!host_.mpvPlays()) requestProxy();", "            requestProxy();"],
       ["        if (host_.mpvPlays()) return;                  // 1.1.0 (L2)", "        // 1.1.0 (L2)"],
       ["        if (s_.url.empty()) requestProxy();            // (asked once per item", "        // (asked once per item"]]],
    ["C2: L6 -- mpv's list is adopted ONLY where MediaTracks has none (never over its list, never while its probe runs); the first time with no pick of the viewer's mpv's OWN choice is waited for; the job waits for mpv's list (the probe's wait) and names a track of an mpv list on the wire (\"0:<id>\", never \"\" = aid=auto)",
      (s) => { const ad = fnBody(s, "void adoptMpvTracks"), li = fnBody(s, "bool loadItem"), sj = fnBody(s, "uint64_t startJob");
        return /if \(s_\.tracksState == "probing"\) return;/.test(ad) && /if \(s_\.tracksState == "ready" && s_\.tracks && !s_\.tracksFromMpv\) return;/.test(ad)
          && /if \(first && !picked && !mpvChose && !s_\.mpvFinal\) return;/.test(ad) && /if \(a\.ord == t\.ord\) \{ a\.trackId = t\.track; return; \}/.test(ad)
          && /if \(!worthy && !adoptedBefore && host_\.mpvPlays\(\)\) s_\.probeWait = true;/.test(li) && /if \(s_\.probeWait && worthy\) requestProbe\(/.test(li)
          && /w\.audio = set->audio\.ord > 0 \|\| \(set->audio\.trackId != 0 && mpvListed\(set->mediaKey\)\) \? tp::packAudioSel\(set->audio\)/.test(sj); },
      [["        if (s_.tracksState == \"ready\" && s_.tracks && !s_.tracksFromMpv) return;   // a MediaTracks list: NEVER overridden\n", ""],
       ["        if (s_.tracksState == \"probing\") return;                            // MediaTracks is reading it: its answer first\n", ""],
       ["        if (first && !picked && !mpvChose && !s_.mpvFinal) return;\n", ""],
       ["        if (!worthy && !adoptedBefore && host_.mpvPlays()) s_.probeWait = true;\n", ""],
       ["        w.audio = set->audio.ord > 0 || (set->audio.trackId != 0 && mpvListed(set->mediaKey)) ? tp::packAudioSel(set->audio)",
        "        w.audio = set->audio.ord > 0 ? tp::packAudioSel(set->audio)"]]],
  ];
  pinAll(core, CPINS);

  // ---- MUTATION SENTINELS: each rule taken out of the page must be CAUGHT by its probe ----------------------------------------
  section("THE NATIVE VIDEO ENGINE -- mutation sentinels (index.html with one rule taken out each)");
  {
    const R = (a, b) => (s) => s.replace(a, () => b);
    const judge = (name, mutate, probe, opts) => {
      if (mutate(script) === script) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow(Object.assign({ resizeObserver: true, mutate, name }, opts || {})); }
      catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true, threw = "";
      try { healthy = !!probe(w); } catch (e) { threw = String((e && e.message) || e); }
      if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    judge("N1: mpv never chosen (pickRole without the native role)", R("    if (nativeFor(F.seq)) return \"native\";", ""), nativeChosen);
    judge("N1: the engine's availability ignored", R("const nativeFor = (seq) => NATIVE.available && seq > 0", "const nativeFor = (seq) => seq > 0"), unavailable);
    judge("N1: native messages sent to an app that never spoke of the engine", R("    if (!NATIVE.available) return;            // an app without the engine hears nothing of it\n", ""), olderApp);
    judge("N2: the clock does not interpolate (the last state's time only)", R("        t += Math.min(run, q) * speed;\n", ""), clockInterp);
    judge("N2: the clock ignores the speed", R("t += Math.min(run, q) * speed;", "t += Math.min(run, q);"), clockSpeed);
    // (fix round 02/11) M1: the hold, its speed allowance, its learning, its count; the drift's `big`
    judge("M1: mpv quiet for 30 s: the clock runs on (no hold)", R("t += Math.min(run, q) * speed;", "t += run * speed;"), quietHold);
    judge("M1: ...keep-ahead then pauses a film mpv has not played", R("t += Math.min(run, q) * speed;", "t += run * speed;"), quietKeepAhead);
    judge("M1: the hold not scaled by a slow speed (a 1 fps film at 0.25x held between its frames)",
      R("Math.max(NATIVE_QUIET_MS / Math.min(1, speed),", "Math.max(NATIVE_QUIET_MS,"), quietSlowFilm);
    judge("M1: the film's own pace not learnt (a 3.5 s gap of a 1 fps film held)", R("        if (mg > 0) gap = Math.max(gap, Math.min(mg, NATIVE_QUIET_GAP_CAP));\n", ""), quietLearned);
    judge("M1: the learnt pace allowed once, not 4 times", R("  const NATIVE_QUIET_FRAMES = 4;", "  const NATIVE_QUIET_FRAMES = 1;"), quietLearned);
    judge("M1: the hold never counted", R("if (run > q && holdAt !== anchor) { holdAt = anchor; holds++; }", ""), quietHold);
    judge("M1: a state far off the clock dropped silently", R("        else drift.big++;", ""), driftBig);
    judge("M3: a stale speed state undoes ] (no speed wish)", R("if (wish.speed !== undefined && now < wish.speedUntil && Math.abs(sp - wish.speed) > 1e-3) { /* from before */ }",
      "if (false) { /* from before */ }"), speedHold);
    judge("M3: the clock runs past mpv's duration", R("      return Math.max(0, d > 0 ? Math.min(t, d) : t);", "      return Math.max(0, t);"), clockCap);
    judge("N2: a state from before a seek accepted (no seek hold)", R("if (seek && Math.abs(tm - seek.ms) > NATIVE_SEEK_NEAR_MS && now < seek.until) { /* from before the seek */ }",
      "if (false) { /* from before the seek */ }"), seekHold);
    judge("N2: mpv's seeking not held", R("if (playing && !seek && !seeking && !eof && loaded)", "if (playing && !seek && !eof && loaded)"), mpvSeeking);
    judge("N2: the error counted across a pause (not steady playback)", R("st && st.paused === false && s.paused === false && playing", "st && playing"), driftMeasure);
    judge("N3: a state from before a play flips the button back (no wish)", R("if (wish.paused !== undefined && now < wish.until && p !== wish.paused) { /* from before the ask */ }",
      "if (false) { /* from before the ask */ }"), playPause);
    judge("N3: play never told to mpv", R("            cmd(\"play\");\n", ""), playPause);
    judge("N3: a seek sent as a keyframe seek (not exact)", R("        armSeekExpiry();\n        cmd(\"seek\", { ms, exact: true });", "        armSeekExpiry();\n        cmd(\"seek\", { ms, exact: false });"), seekHold);
    // (fix round 02/11) M2: the hold's expiry from a timer, its own `until`, seekable:false (after the first restart only)
    judge("M2: the seek hold's expiry never armed (a truncated file: no \"ended\", the target for good)", R("        seek = { ms, until: t0() + NATIVE_SEEK_HOLD_MS };\n        armSeekExpiry();\n",
      "        seek = { ms, until: t0() + NATIVE_SEEK_HOLD_MS };\n"), seekExpiresAtEnd);
    judge("M2: ...the expiry does not take mpv's last word again", R("        if (seek === s && st && Number(st.gen) === gen) apply(st);", "        if (seek === s && st && Number(st.gen) === gen) { /* nothing */ }"), seekExpiresPaused);
    judge("M2: ...the hold has no end of its own (until ignored)", R("if (seek && Math.abs(tm - seek.ms) > NATIVE_SEEK_NEAR_MS && now < seek.until)",
      "if (seek && Math.abs(tm - seek.ms) > NATIVE_SEEK_NEAR_MS)"), seekExpiresPaused);
    judge("M2: ...nor for a seek mpv never did", R("        armSeekExpiry();\n        cmd(\"seek\", { ms, exact: true });", "        cmd(\"seek\", { ms, exact: true });"), seekLost);
    judge("M2: seekable:false ignored (the target held for a file mpv cannot seek)",
      R("        if (st && st.seekable === false && st.restarted === true) { cmd(\"seek\", { ms, exact: true }); return; }\n", ""), notSeekable);
    judge("M2: seekable:false believed before mpv's first restart", R("if (st && st.seekable === false && st.restarted === true)", "if (st && st.seekable === false)"), seekableNotYet);
    judge("M3: a seek to where it already is sent", R("        if (!seek && Math.abs(ms - clock()) < NATIVE_SEEK_SAME_MS) return;\n", ""), seekSame);
    judge("M3: the seek target not clamped to mpv's duration", R("        if (d > 0) ms = Math.min(ms, d);\n        if (!seek && Math.abs(ms - clock())", "        if (!seek && Math.abs(ms - clock())"), seekClamp);
    judge("N3: the speed sent again with every setting (no one-per-change)", R("if (sent.rate !== want.rate) { sent.rate = want.rate;", "if (true) { sent.rate = want.rate;"), transport);
    judge("N3: mute never told to mpv", R("set muted(b) { want.mute = !!b; if (gen > 0) sendWants(); },", "set muted(b) { want.mute = !!b; },"), transport);
    judge("N4: the A-B loop never told to mpv", R("    if (role === \"native\") nativeMedia.loop(loopA, loopB);", ""), abCmds);
    judge("N4: the page seeks at B itself too", R("    if (role === \"native\") return;            // 1.1.0: mpv's own ab-loop runs it", "    // 1.1.0: mpv's own ab-loop runs it"), abNoPageSeek);
    judge("N4: the clock not wrapped by mpv's loop", R("if (ab.a >= 0 && ab.b > ab.a && base < ab.b && t >= ab.b) t = ab.a + ((t - ab.b) % (ab.b - ab.a));", ""), abWrap);
    judge("M3: the clock wrapped though the playhead was already past B", R("if (ab.a >= 0 && ab.b > ab.a && base < ab.b && t >= ab.b)", "if (ab.a >= 0 && ab.b > ab.a && t >= ab.b)"), abNoWrapPast);
    judge("L4: a reloaded page under mpv's A-B loop shows no loop", R("      if (ab.a >= 0) { loopA = ab.a; loopB = ab.b > ab.a ? ab.b : -1; paintLoop(); }\n", ""), abAdopted);
    judge("N5: a sound track pick not told to mpv", R("set enabled(v) { if (v) nativeMedia.select(t.ord); } }));", "set enabled(v) { } }));"), audioPick);
    judge("N5: the html5 mapping used for mpv (its rows disabled: the DTS track 'can't be played here')",
      R("    if (role === \"native\") {\n      if (!(Number(media.readyState) > 0) || st !== \"ready\" || !F.tracks.audio.length) return none;",
        "    if (false) {\n      if (!(Number(media.readyState) > 0) || st !== \"ready\" || !F.tracks.audio.length) return none;"), audioPick);
    judge("M3: a stale ord state flips the heard track back (no track wish)", R("if (wish.ord !== undefined && now < wish.ordUntil && o !== wish.ord) { /* from before */ }",
      "if (false) { /* from before */ }"), trackHold);
    judge("L5: mpv's \"no track\" (ord -1) ignored", R("(o >= 0 || (o === -1 && s.restarted !== false))", "(o >= 0)"), trackNone);
    judge("L5: ...-1 believed before mpv's first restart (the app's choice never enforced)", R("(o === -1 && s.restarted !== false)", "(o === -1)"), trackNotYet);
    judge("L5: the page asks mpv again for the track it dropped", R("    if (role === \"native\" && nativeMedia.none()) return;\n", ""), trackNone);
    judge("L6: A says \"No other sound track\" while mpv has two", R("const many = (F.tracks.state === \"ready\" && F.tracks.audio.length > 1) || nativeTracks() > 1;",
      "const many = F.tracks.state === \"ready\" && F.tracks.audio.length > 1;"), aviSentence);
    judge("N6: no observer on the picture box", R("new ResizeObserver(() => sendNativeRect()).observe(el.fileView);", "new ResizeObserver(() => sendNativeRect());"), rectFlow);
    judge("M3: the app's first word of the engine sends no rectangle (mpv's window left as it is)",
      R("    if (was !== NATIVE.available && F.phase !== \"idle\") { mount(); paintFile(); paintAudioUi(); }\n    sendNativeRect();\n",
        "    if (was !== NATIVE.available && F.phase !== \"idle\") { mount(); paintFile(); paintAudioUi(); }\n"), infoRect);
    judge("N6: the rectangle rounded to whole pixels", R("const r2 = (v) => Math.round((Number(v) || 0) * 100) / 100;", "const r2 = (v) => Math.round(Number(v) || 0);"), rectFlow);
    judge("N6: full screen does not send it", R("    sendNativeRect();                          // 1.1.0: mpv's picture follows (the ResizeObserver says it too)\n", ""), rectFull);
    judge("N6: the panel does not send it", R("    sendNativeRect();                          // 1.1.0: a docked panel opening or closing resizes the picture\n", ""), rectPanel);
    judge("N7: the stage never made see-through", R("try { document.body.classList.toggle(\"is-native\", pic); } catch { /* no body */ }", "/* no see-through */"), nativeChosen);
    judge("N7: mpv's \"no picture\" not followed (the sound's card never comes)", R("      if (video !== was.video) nativePictureMoved();\n", ""), soundOnly);
    judge("N7: a sound file made see-through until mpv says otherwise", R("    return AUDIO_EXT.indexOf(extOf(F.name)) < 0;\n  }\n  function nativePictureMoved()",
      "    return true;\n  }\n  function nativePictureMoved()"), soundByName);
    judge("N8: the stored render mode drawn over mpv (renderNow without \"native\")",
      R("  function renderNow() { return role === \"native\" ? \"native\" : isFull ? \"plain\" : renderMode; }", "  function renderNow() { return isFull ? \"plain\" : renderMode; }"),
      renderNative, { ls: { "audiopb.tr.render": "canvas" } });
    judge("N8: Ctrl+Alt+V cycles the modes over mpv", R("    if (role === \"native\") { showFileToast(NATIVE_RENDER_TEXT, false); return; }", ""), renderNative, { ls: { "audiopb.tr.render": "canvas" } });
    judge("N9: mpv's refusal not followed (no fallback on nativeError)", R("  function applyNativeError(msg) { nativeFail(Number(msg.gen) || 0, cleanNotice(msg.text || \"\"), false); }",
      "  function applyNativeError(msg) { }"), fallbackError);
    judge("N9: ANOTHER opening's error taken for this one's", R("nativeFail(Number(msg.gen) || 0, cleanNotice(msg.text || \"\"), false);",
      "nativeFail(F.seq, cleanNotice(msg.text || \"\"), false);"), fallbackError);
    judge("N9: mpv's silence reported as the <video>'s failure (sound copy asked, mpv never told)",
      R("    if (role === \"native\") { nativeFail(F.seq, NATIVE_TIMEOUT_TEXT, true); return; }\n", ""), fallbackTimeout);
    judge("N9: the engine lost mid-film and the page stays on it", R("    if (was !== NATIVE.available && F.phase !== \"idle\") { mount(); paintFile(); paintAudioUi(); }", ""), fallbackInfo);
    judge("L7: the 20 s wait not restarted at mpv's Start (the <video> takes it 20 s after the OPENING)",
      R("      if (msg.started === true && !nativeMedia.started() && metaTimer) {", "      if (false) {"), startedWait);
    judge("M3: the refusals remembered without a cap", R("    while (NATIVE.failed.size > 64) NATIVE.failed.delete(NATIVE.failed.keys().next().value);\n", ""), failedCap);
    judge("N9: a sentence said at mpv's refusal (the <video> may well play it)",
      R("  function applyNativeError(msg) { nativeFail(", "  function applyNativeError(msg) { showFileToast(\"The video engine could not play this file.\", true); nativeFail("), fallbackError);
    judge("N10: the <video>'s format notice said over mpv (\"can't be played here\")",
      R("    if (role === \"native\") return \"\";          // 1.1.0: mpv plays it", "    // 1.1.0: mpv plays it"), noProxy);
    judge("N7: a file the analysis found has no picture made see-through",
      R("    if (F.videoKnown && !F.hasVideo) return false;\n    if (F.tracks.state", "    if (F.tracks.state"), soundByAnalysis);
    judge("N10: mpv needs the item's URL (no URL = the sound copy asked)", R("    if (nativeFor(F.seq)) return \"native\";", "    if (nativeFor(F.seq) && F.url) return \"native\";"), noProxy);
    judge("N11: no timeupdate from mpv's states (a page without frames: the overlay stuck)",
      R("if (loaded && (now - tickAt >= NATIVE_TICK_MS || seeked || was.playing !== playing)) { tickAt = now; fire(\"timeupdate\"); }", ""), subsTick);
    judge("N12: a reloaded page seeks mpv back to the last reported position (no adoption)",
      R("        if (m === nativeMedia && nativeMedia.adopted()) pendingSeek = -1;\n", ""), adopt);
    judge("N13: a hidden window leaves mpv playing", R("[el.fileVideo, el.fileAudio, nativeMedia].forEach((m) => { try { m.pause(); } catch { /* idle */ } });",
      "[el.fileVideo, el.fileAudio].forEach((m) => { try { m.pause(); } catch { /* idle */ } });"), hiddenPauses);
    judge("N14: the cursor never told back (leaving the idle hide)", R("    nativeCursorIdle(false);                   // 1.1.0: the host shows the cursor again", "    // 1.1.0: the host shows the cursor again"), cursorNet);
    judge("N14: the cursor told to an app that never spoke of the engine", R("if (!NATIVE.seen || NATIVE.idle === idle) return;", "if (NATIVE.idle === idle) return;"), cursorQuiet);
    judge("M3: the cursor told again at every move (no dedupe)", R("if (!NATIVE.seen || NATIVE.idle === idle) return;", "if (!NATIVE.seen) return;"), cursorNet);
    judge("L8: a film adopted playing in a closed window plays on", R("    if (playing && !windowShown && media === nativeMedia) { nativeMedia.pause(); return; }\n", ""), hiddenAdopt);
    judge("N15: a state of another opening applied", R("if (role === \"native\" && nativeMedia.gen() === g) {", "if (role === \"native\") {"), staleGen);
    judge("N15: a new opening not given the page's settings", R("if (g !== gen) { reset(g); if (g > 0) sendWants(); }", "if (g !== gen) { reset(g); }"), nextItem);
    judge("N15: the previous item's \"playing\" left on the button", R("        if (m === nativeMedia) paintPlayButton(!m.paused);\n", ""), nextItem);
    judge("N16: \"ended\" said at every state of the end (the app skips files)", R("if (eof && !was.eof) fire(\"ended\");", "if (eof) fire(\"ended\");"), endedOnce);
    judge("N16: a stale end undoes Play at the end", R("if (!seek) eof = s.eof === true;", "eof = s.eof === true;"), playAtEnd);
    judge("L1: Play at the end restarts from 0 over a seek on its way (the seek does not leave the end)",
      R("        eof = false;                                           // (L1: a seek leaves the end", "        // (L1: a seek leaves the end"), playAfterSeekAtEnd);
    judge("M3: mpv's end told before its pause leaves the page playing", R("      if (eof && wish.paused === undefined) playing = false;\n", ""), eofStops);
  }
};

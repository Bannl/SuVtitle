// test_pctx.js -- P-CTX (25/10/2026): the user's two reports on SuVtitle 1.0.0, the PAGE's side. Required by test_ui.js (same
// ok/section counters). Every check runs the REAL page (app/web/index.html) with a fake app; each rule has a MUTATION
// SENTINEL at the end -- the page with that rule taken out must FAIL its probe.
//  1. THE RIGHT-CLICK MENU (« il faut qu'avec un clic droit sur la vidéo ou le fichier en train de jouer, on puisse avoir les
//     options liées au fichier »): on the picture and on the queue's playing row; its rows ARE the CC panel's (same labels,
//     hints, disabled rows and reasons, the current source marked) and its picks the CC panel's own functions (pickAudio /
//     pickSource: compared on twin windows, the panel's pick against the menu's); "Load a subtitle file…", the CC panel;
//     a menu's keyboard; where it opens (at the pointer, flipped at the edges); everything that closes it; its repaint.
//  2. FULL SCREEN'S IDLE HIDE, its LOGIC (« on ne voit plus la souris du tout, même quand on la bouge »): never under ANY
//     open popover (one list, popOpen), the delay restarted when one closes, a click or a wheel with no move is activity,
//     a change of full screen closes the popovers placed for the other layout. (The user then said the play BAR does not
//     come back either: the cause in full screen -- moves not delivered, a starved main thread, frames not presented -- is
//     the diagnosis agent's; nothing here claims it.)
// WHAT THIS STUB CANNOT PROVE (domstub.js): no CSS (no hit-testing, no cursor, no :focus-visible, no flex/overflow, no
// stacking: the menu's look and its place over the picture), no event propagation (a listener runs only on the node a test
// dispatches to -- the document's capture listeners only through dispatchDoc), no native button behaviour (a disabled
// button's click() still fires here: the page's own guard is what is tested), no real focus rules (a disabled button is
// focusable here), no layout (getBoundingClientRect is what a test sets), no WebView2 (does a right-click reach the page as
// `contextmenu` with the default menus off?), no OS cursor. Those are tests\e2e (the real app, CDP) or not testable at all.
"use strict";
module.exports = function pctxChecks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, html, script } = ctx;

  // ---- fixtures: a film the app has read (two sound tracks, four embedded subtitle tracks -- two unusable) + a sidecar --
  const A_ = (id, label, language) => ({ id: String(id), kind: "main", label, language });
  const vid = { codec: "V_MPEG4/ISO/AVC", w: 640, h: 360 };
  const T = { state: "ready", container: "mkv", video: vid,
    audio: [{ ord: 0, track: 2, name: "English speech", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 1, track: 3, name: "Chinese speech", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }],
    subs: [{ id: 41, track: 4, name: "", lang: "eng", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: false, ok: true },
           { id: 42, track: 5, name: "French ASS", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: true, ok: true },
           { id: 43, track: 6, name: "", lang: "kor", codec: "S_VOBSUB", kind: "image", def: false, forced: false, ok: true },
           { id: 44, track: 7, name: "", lang: "ger", codec: "S_TEXT/ASS", kind: "text", def: false, forced: false, ok: false }] };
  const C = [A_(2, "English speech", "eng"), A_(3, "Chinese speech", "chi")];
  const T_DTS = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_DTS", ch: 6, def: true, play: false },
            { ord: 1, track: 3, name: "", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }] };
  const C_DTS = [A_(2, "", "chi")];
  const SIDE = [{ id: 9, name: "film.fr.srt", lang: "fr" }];
  const MKV_URL = "https://f0123456789abcdef.suvtitle-media.example/film.mkv?v=mk1";
  const PROXY_URL = "https://suvtitle-proxy.example/mk1-1.wav";
  const PROXY_LINE = "This file plays through SuVtitle's sound copy: its first sound track only.";
  const film = (o) => fileMsg(Object.assign({ jobId: 2, seq: 1, itemId: 12, phase: "done", complete: true, ranges: [[0, 600000]],
    name: "film.mkv", url: MKV_URL, tracks: T, audioOrd: 1, sidecars: SIDE }, o));
  const open = (w, o, list) => { w.send(film(o)); w.ids.fileVideo._setAudioTracks(list || C); metaOk(w.ids.fileVideo); w.pumpTimers(0); return w; };
  const queue = (w) => w.send({ type: "playerQueue", items: [{ id: 11, name: "E01.mkv", dir: "S1", state: "ready", pct: 0, durMs: 0, resumeMs: 0 },
    { id: 12, name: "film.mkv", dir: "S1", state: "ready", pct: 0, durMs: 0, resumeMs: 0 }], cur: 12, loaded: true, repeat: "off",
    shuffle: false, translate: true });
  // the page's own script with a test's mutant, as a WINDOW of its own: the twin of a probe's window (panel vs menu)
  const twin = (w) => runTranslatorWindow({ html: html.replace(/<script>[\s\S]*?<\/script>/, () => "<script>" + w.script + "</script>") });

  // ---- helpers ------------------------------------------------------------------------------------------------------------
  const menu = (w) => w.ids.ctxMenu;
  const shown = (w) => menu(w).hidden === false;
  const rowsOf = (L) => L.filter((n) => n.classList && n.classList.contains("cc-src"))
    .map((b) => b.textContent + (b.classList.contains("is-on") ? "*" : "") + (b.disabled ? "!" : ""));
  const part = (w, title) => {                       // the menu's nodes under one of its titles
    const L = menu(w).children, i = L.findIndex((n) => n.className === "pop-title" && n.textContent === title);
    if (i < 0) return null;
    const out = [];
    for (let j = i + 1; j < L.length && L[j].className !== "pop-title" && L[j].className !== "ctx-sep"; j++) out.push(L[j]);
    return out;
  };
  const mAudio = (w) => rowsOf(part(w, "Audio") || []);
  const mSubs = (w) => rowsOf(part(w, "Subtitles") || []);
  const mNote = (w) => { const n = (part(w, "Audio") || []).find((x) => x.className === "cc-note"); return n ? n.textContent : ""; };
  const mActs = (w) => menu(w).children.filter((n) => n.tagName === "BUTTON" && !n.classList.contains("cc-src")).map((b) => b.textContent);
  const entry = (w, re) => menu(w).children.find((n) => n.tagName === "BUTTON" && re.test(n.textContent));
  // (a probe never THROWS on a mutant: an entry that is not there is a failed check, not a broken probe)
  const pickEntry = (w, re) => { const b = entry(w, re); if (!b) return false; b.click(); return true; };
  const cc = (w) => { if (w.ids.ccPanel.hidden) w.ids.tCC.click(); return w; };
  const pAudio = (w) => (w.ids.ccAudio.hidden ? [] : rowsOf(w.ids.ccAudio.children));
  const pSubs = (w) => rowsOf(w.ids.ccSources.children);
  const key = (w, k, o) => w.win.dispatch("keydown", Object.assign({ key: k, target: w.doc.body }, o || {}));
  const focused = (w) => { const a = w.doc.activeElement; return a && a.parentNode === menu(w) ? a.textContent : ""; };
  const toast = (w) => (w.ids.fileToast.hidden ? "" : w.ids.fileToast.textContent);
  const heard = (w) => w.ids.fileVideo.audioTracks.enabledIds().join();
  // a right-click as a browser delivers it: pointerdown (button 2: the document's capture listener, then the window's), then
  // `contextmenu` on what was clicked -- the stage for the picture, a row of the queue
  const rclick = (w, target, x, y, host) => {
    const ev = { target, button: 2, clientX: x, clientY: y };
    w.dispatchDoc("pointerdown", ev);
    w.win.dispatch("pointerdown", ev);
    (host || w.ids.fileStage).dispatch("contextmenu", ev);
  };
  const onPicture = (w, x, y) => { menu(w)._rect = { left: 0, top: 0, width: 280, height: 300, right: 280, bottom: 300 }; rclick(w, w.ids.fileVideo, x || 100, y || 120); return w; };
  const lclickPicture = (w) => {                     // a left click on the picture: pointerdown, then click
    w.dispatchDoc("pointerdown", { target: w.ids.fileVideo, button: 0 });
    w.win.dispatch("pointerdown", { target: w.ids.fileVideo, button: 0 });
    w.ids.fileView.dispatch("click", { target: w.ids.fileVideo });
  };
  const idle = (w) => w.ids.fileStage.classList.contains("is-idle");
  const fsPlaying = (w) => {                          // full screen, playing (WE-10's set-up), the bar idle after 2.6 s
    open(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.ids.tPlay.click();
    w.pumpTimers(2600);
    return w;
  };

  // ---- the probes (each is a check below AND the condition its mutation sentinel must see FAIL) ---------------------------
  // 1. the picture's right-click: the menu, at the pointer, its rows the CC panel's -- same labels, same hints, same disabled
  //    rows (the same reason in their hint), the same one marked; then "Load a subtitle file…" and "Subtitle settings…"
  const MENU_AUDIO = "English speech (AAC 2.0)|Chinese speech (AAC 2.0)*";
  // (U1, 02/11: a non-Latin language is named with its English name after it -- "简体中文 (Simplified Chinese)")
  const MENU_SUBS = "SuVtitle · both languages*|SuVtitle · English (original)|SuVtitle · 简体中文 (Simplified Chinese) (translation)|film.fr.srt (next to the file)"
    + "|English (embedded)|French ASS (embedded · forced)|Korean (image subtitles, not supported)!|German (can't be read)!|Off";
  const menuIsThePanel = (w) => {
    onPicture(open(w));
    const m = { audio: mAudio(w).join("|"), subs: mSubs(w).join("|"), acts: mActs(w).join("|"), role: menu(w).getAttribute("role"),
                at: menu(w).style.left + "," + menu(w).style.top };
    cc(w);
    const p = { audio: pAudio(w).join("|"), subs: pSubs(w).join("|") };
    return shown(w) === false && m.audio === MENU_AUDIO && m.subs === MENU_SUBS && m.audio === p.audio && m.subs === p.subs
        && m.acts === "Load a subtitle file…|Subtitle settings…" && m.role === "menu" && m.at === "100px,120px";
  };
  // ...and a sound's card (an mp3: no sound tracks of its own -- no Audio part, like the CC panel)
  const menuOnSoundCard = (w) => {
    w.send(fileMsg({ jobId: 2, seq: 1, itemId: 3, phase: "done", complete: true, name: "talk.mp3",
                     url: "https://f0123456789abcdef.suvtitle-media.example/talk.mp3", hasVideo: false, videoKnown: true }));
    metaOk(w.ids.fileAudio);
    const card = w.ids.fileBig.hidden === false;
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 200, right: 280, bottom: 200 };
    rclick(w, w.ids.fileBigName, 300, 200);
    return card && shown(w) && part(w, "Audio") === null && mSubs(w)[0] === "SuVtitle · both languages*" && mSubs(w).pop() === "Off"
        && mActs(w).join("|") === "Load a subtitle file…|Subtitle settings…";
  };
  // ...not on a CONTROL (a button, the volume, the seek bar), nor with nothing open; but the bar's empty space (over the
  //    picture in full screen) is the picture's
  const notOnControls = (w) => {
    open(w);
    const none = [w.ids.tPlay, w.ids.tVol, w.ids.seekKnob, w.ids.fileReadyBtn].every((t) => { rclick(w, t, 200, 200); return !shown(w); });
    const gap = w.ids.transport.children[1].children.find((n) => n.className === "t-gap");
    rclick(w, gap, 200, 200);
    return none && shown(w);
  };
  const noFileNoMenu = (w) => {
    queue(w);
    w.send(fileMsg({ jobId: 1, seq: 0, itemId: 0, phase: "idle", name: "", url: "" }));
    const player = w.ids.filePlayer.hidden === false;
    rclick(w, w.ids.fileVideo, 200, 200);
    return player && !shown(w);
  };
  // 2. the queue: the PLAYING row's menu is the file's (the same rows); another row's, its two actions
  const playingRow = (w) => {
    open(w);
    queue(w);
    const r12 = w.ids.queueList.children[1], r11 = w.ids.queueList.children[0];
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 300, right: 280, bottom: 300 };
    rclick(w, r12, 100, 100, r12);
    const fileMenu = shown(w) && mAudio(w).join("|") === MENU_AUDIO && mSubs(w).join("|") === MENU_SUBS;
    rclick(w, r11, 100, 60, r11);
    const rowMenu = shown(w) && mActs(w).join("|") === "Play|Remove from the queue" && part(w, "Subtitles") === null;
    w.outbox.length = 0;
    const p1 = pickEntry(w, /^Play$/);
    const played = p1 && JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerQueue", op: "play", id: 11 }]) && !shown(w);
    rclick(w, r11, 100, 60, r11);
    w.outbox.length = 0;
    const p2 = pickEntry(w, /^Remove from the queue$/);
    return p2 && fileMenu && rowMenu && played && JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerQueue", op: "remove", id: 11 }]);
  };
  // 3. ONE implementation: the menu's pick and the CC panel's pick, on twin windows, leave the SAME page and the SAME words
  //    to the app -- a sound track (heard at once, one playerSet after the viewer stopped choosing, the toast), an embedded
  //    track (asked by id, not auto), one language of SuVtitle's (the display rule, kept), Off
  // (what a viewer and the app can tell: the words sent, the track heard, the toast, the display rule kept, and what the CC
  //  panel marks as chosen -- the panel opened to read it)
  const seen = (x) => { cc(x); return JSON.stringify({ out: x.outbox, heard: heard(x), toast: toast(x), show: x.lsStore["audiopb.tr.show"] || "",
                                                       marks: pAudio(x).concat(pSubs(x)).filter((r) => /\*/.test(r)) }); };
  const sameAsPanel = (w) => {
    const picks = [[/^English speech/, "audio"], [/^French ASS/, "subs"], [/^SuVtitle · English/, "subs"], [/^Off$/, "subs"]];
    return picks.every(([re, where]) => {
      const a = open(twin(w)), b = open(twin(w));      // the same page twice: the menu's pick on a, the panel's on b
      const before = seen(a);
      onPicture(a);
      const e = entry(a, re);
      if (!e) return false;
      e.click();
      cc(b);
      const row = (where === "audio" ? b.ids.ccAudio : b.ids.ccSources).children.find((x) => re.test(x.textContent));
      if (!row) return false;
      row.click();
      a.pumpTimers(1000); b.pumpTimers(1000);
      const sa = seen(a);
      return !shown(a) && sa === seen(b) && sa !== before;
    });
  };
  // ...and the menu's source never touches a track itself: it lists audioState() / sources() with ccRow and picks with
  //    pickAudio / pickSource (a copy of the logic would show here first)
  const ctxSrc = (s) => { const i = s.indexOf("  function paintCtx() {"), j = s.indexOf("  // At the pointer, to its right", i); return i >= 0 && j > i ? s.slice(i, j) : ""; };
  const oneImplementation = (w) => {
    const src = ctxSrc(w.script);
    return !!src && /audioState\(\)/.test(src) && /sources\(\)/.test(src) && /ccRow\(/.test(src) && /pickAudio\(r\)/.test(src)
        && /pickSource\(s\)/.test(src) && /ccAudioNote\(S, show2\)/.test(src) && /loadSubFile\(\)/.test(src)
        && !/enableOnly|audioTracks|"playerSet"|"playerSubGet"|askSub\(|setShow\(|subSrc =|wantExt =/.test(src);
  };
  // 4. what cannot be used is DISABLED, with the CC panel's reason: picked, it does nothing; the keyboard steps over it
  const disabledSame = (w) => {
    onPicture(open(w));
    const kor = entry(w, /^Korean/), ger = entry(w, /^German/);
    if (!kor || !ger) return false;
    w.outbox.length = 0;
    kor.click(); ger.click();
    const inert = shown(w) && w.outbox.length === 0;
    let neverDisabled = true;
    for (let i = 0; i < 14; i++) { key(w, "ArrowDown"); const a = w.doc.activeElement; if (!a || a.disabled || a.parentNode !== menu(w)) neverDisabled = false; }
    return kor.disabled && ger.disabled && /\(image subtitles, not supported\)$/.test(kor.textContent) && /\(can't be read\)$/.test(ger.textContent)
        && inert && neverDisabled;
  };
  // ...a sound track this WebView cannot play (DTS): listed, disabled, "can't be played here" -- as the panel
  const dtsSame = (w) => {
    onPicture(open(w, { tracks: T_DTS, audioOrd: 1 }, C_DTS));
    const m = mAudio(w).join("|");
    cc(w);
    return m === "English (DTS 5.1 · can't be played here)!|Chinese (AAC 2.0)*" && m === pAudio(w).join("|");
  };
  // ...ONE sound track: nothing to choose -- no Audio part, as #ccAudio (hidden under two rows)
  const T_ONE = { state: "ready", container: "mkv", video: vid, subs: [], audio: [T.audio[0]] };
  const oneTrack = (w) => {
    onPicture(open(w, { tracks: T_ONE, audioOrd: 0 }, [C[0]]));
    const none = shown(w) && part(w, "Audio") === null && mSubs(w).length > 0;
    cc(w);
    return none && w.ids.ccAudio.hidden === true;
  };
  // ...played through SuVtitle's sound copy: every row disabled, the first heard, and the panel's one line under them
  const proxySame = (w) => {
    w.send(film({ tracks: T, audioOrd: 0, proxyUrl: PROXY_URL, proxyState: "ready" }));
    w.ids.fileAudio._setAudioTracks([A_(1, "", "")]);
    metaOk(w.ids.fileAudio);
    onPicture(w);
    const m = mAudio(w).join("|"), note = mNote(w);
    cc(w);
    return m === "English speech (AAC 2.0)*!|Chinese speech (AAC 2.0)!" && m === pAudio(w).join("|") && note === PROXY_LINE
        && note === w.ids.ccAudioNote.textContent;
  };
  // 5. the keyboard of a menu: ↓ from nothing = the first entry, ↑ = the last, round, Home / End; Enter and Space pick the
  //    entry the keyboard is on; nothing reaches the player while it is up (an arrow would seek or change the volume, Space
  //    pause, S cycle the subtitles)
  const keyboard = (w) => {
    open(w);
    w.ids.fileVideo.currentTime = 30;
    onPicture(w);
    const vol0 = String(w.ids.tVol.value), t0 = w.ids.fileVideo.currentTime, p0 = w.ids.fileVideo.paused;
    const mute0 = w.ids.tMute.classList.contains("is-muted"), toast0 = toast(w);
    key(w, "ArrowDown"); const first = focused(w);
    key(w, "End"); const lastE = focused(w);
    key(w, "ArrowDown"); const round = focused(w);
    key(w, "ArrowUp"); const back = focused(w);
    key(w, "Home"); const home = focused(w);
    ["ArrowRight", "ArrowLeft", "s", "m", "a", "PageDown"].forEach((k) => key(w, k));
    const still = shown(w) && focused(w) === home && String(w.ids.tVol.value) === vol0 && w.ids.fileVideo.currentTime === t0
               && w.ids.fileVideo.paused === p0 && w.ids.tMute.classList.contains("is-muted") === mute0 && toast(w) === toast0;
    for (let i = 0; i < 20 && focused(w) !== "Off"; i++) key(w, "ArrowDown");
    const reached = focused(w) === "Off";
    key(w, "Enter");
    const picked = !shown(w) && (cc(w), pSubs(w).pop() === "Off*");
    w.ids.tCC.click();                                    // (the panel closed again)
    onPicture(w);
    key(w, "ArrowDown");
    key(w, " ");                                          // Space on the first entry, "English speech": picked, the sound switched
    const space = !shown(w) && heard(w) === "2" && w.ids.fileVideo.paused === p0;
    return first === "English speech (AAC 2.0)" && lastE === "Subtitle settings…" && round === first && back === lastE && home === first
        && still && reached && picked && space;
  };
  // ...Esc and Tab close it; the focus goes back where it was (the queue row it was opened on)
  const escAndFocus = (w) => {
    open(w); queue(w);
    const r12 = w.ids.queueList.children[1];
    r12.focus();
    rclick(w, r12, 100, 100, r12);
    key(w, "ArrowDown");
    const inMenu = focused(w) !== "";
    key(w, "Escape");
    const esc = !shown(w) && w.doc.activeElement === r12;
    rclick(w, r12, 100, 100, r12);
    key(w, "Tab");
    return inMenu && esc && !shown(w) && w.ids.ccPanel.hidden;
  };
  // ...ONE selection for the hand and the keyboard (a desktop menu's): the pointer resting on an entry is where the keyboard
  //    is -- Enter picks what is under it, ↓ goes on from it; a disabled entry under the pointer takes nothing
  const hoverIsSelection = (w) => {
    onPicture(open(w));
    entry(w, /^Korean/).dispatch("pointermove", {});
    const notDisabled = focused(w) === "";
    const off = entry(w, /^Off$/);
    off.dispatch("pointermove", {});
    const on = focused(w) === "Off";
    key(w, "ArrowDown");
    const next = focused(w) === "Load a subtitle file…";
    key(w, "ArrowUp");
    key(w, "Enter");
    return notDisabled && on && next && !shown(w) && (cc(w), pSubs(w).pop() === "Off*");
  };
  // ...and it hears them FIRST: in the window's CAPTURE phase, stopped there -- in a browser a key reaches the focused element
  //    before any window listener, and the queue row the menu was opened on takes Enter for "play me". This stub has no
  //    propagation (a listener runs only where a test dispatches): a pin on the source, and tests\e2e (CDP) for the real one
  const keysCaptured = (w) => /window\.addEventListener\("keydown", \(e\) => \{\n    if \(ctxHeldKey\(e\) \|\| ctxKey\(e\)\) \{ ctxKeyEv = e; if \(e\.stopPropagation\) e\.stopPropagation\(\); \}\n  \}, true\);/.test(w.script)
    && /window\.addEventListener\("keyup", \(e\) => \{[\s\S]{0,260}?if \(e\.stopPropagation\) e\.stopPropagation\(\);\n  \}, true\);/.test(w.script);
  // 6. where it opens: at the pointer; near an edge, FLIPPED to the pointer's other side; never past the window
  const placed = (w) => {
    open(w);
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 300, right: 280, bottom: 300 };
    const at = (x, y) => { rclick(w, w.ids.fileVideo, x, y); return menu(w).style.left + "," + menu(w).style.top; };
    const a = at(100, 120), b = at(700, 500), c = at(10, 540);
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 544, right: 280, bottom: 544 };   // as tall as the window lets it be
    const d = at(300, 200);
    return a === "100px,120px" && b === "420px,200px" && c === "10px,240px" && d === "300px,8px";
  };
  // 7. what closes it: a pick (above), a click outside -- which is NOT also a click on the picture (no pause) --, a scroll
  //    (not inside it), a wheel outside, the window's blur, a resize, another file, a change of full screen, the window
  //    hidden; a right-click elsewhere moves it there
  const clickOutside = (w) => {
    open(w);
    w.ids.tPlay.click();
    const playing = w.ids.fileVideo.paused === false;
    onPicture(w);
    lclickPicture(w);
    const closedNoPause = !shown(w) && w.ids.fileVideo.paused === false;
    lclickPicture(w);                                    // the NEXT click is the picture's again: a pause
    const nextIsPause = w.ids.fileVideo.paused === true;
    onPicture(w);
    w.win.dispatch("pointerdown", { target: w.ids.fileVideo, button: 0 });   // pressed on the picture, let go elsewhere: no click
    lclickPicture(w);                                    // ...and the next real click is still the picture's: it plays
    return playing && closedNoPause && nextIsPause && !shown(w) && w.ids.fileVideo.paused === false;
  };
  const rightClickMoves = (w) => {
    onPicture(open(w), 100, 120);
    rclick(w, w.ids.fileVideo, 300, 160);
    return shown(w) && menu(w).style.left === "300px" && menu(w).style.top === "160px";
  };
  const closers = {
    "a wheel outside": (w) => w.win.dispatch("wheel", { target: w.ids.fileVideo, deltaY: 100 }),
    "the window's blur": (w) => w.win.dispatch("blur", {}),
    "a resize": (w) => w.win.dispatch("resize", {}),
    "another file": (w) => w.send(film({ jobId: 3, seq: 2, itemId: 11, name: "E01.mkv" })),
    "full screen": (w) => w.send({ type: "translatorWindow", shown: true, fullscreen: true }),
    "the window hidden": (w) => w.send({ type: "translatorWindow", shown: false }),
  };
  const closesOn = (name) => (w) => { onPicture(open(w)); const was = shown(w); closers[name](w); return was && !shown(w); };
  const leavingFull = (w) => {
    open(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    onPicture(w);
    const was = shown(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true, topmost: false });   // the same state again: stays
    const stays = shown(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    return was && stays && !shown(w);
  };
  const staysOnInside = (w) => {
    onPicture(open(w));
    w.dispatchDoc("scroll", { target: menu(w) });
    w.win.dispatch("wheel", { target: menu(w).children[1], deltaY: 100 });
    return shown(w);
  };
  // ...a scroll closes it when it MOVES what it was opened on (review RV-CTX L4): the queue list under a row's menu, the page
  //    itself -- never the subtitle list, which scrolls BY ITSELF (it follows the analysis then the playhead; Chromium's scroll
  //    anchoring shifts it when a line is inserted above the ones in view -- at any scrollTop) and moves neither the picture nor
  //    a queue row
  const scrollRule = (w) => {
    open(w, { phase: "analyzing", complete: false, doneMs: 1000, ranges: [[0, 1000]] });   // paused at 0: the list follows the analysis
    queue(w);
    const L = w.ids.fileList;
    onPicture(w);
    L.scrollHeight = 2000;
    w.send({ type: "translatorFileCue", jobId: 2, idx: 0, s: 0, e: 1000, lang: "en", dstLang: "zh", pass: false, src: "a", dst: "b" });
    const followed = L.scrollTop === 2000;
    w.dispatchDoc("scroll", { target: L });              // the browser's echo of the page's OWN move
    L.scrollTop = 1730;
    w.dispatchDoc("scroll", { target: L });              // ...scroll anchoring (a line inserted above the ones in view)
    w.dispatchDoc("scroll", { target: w.ids.queueList }); // ...the queue list: it does not move the picture
    const pictureStays = shown(w);
    const r11 = w.ids.queueList.children[0];
    rclick(w, r11, 100, 60, r11);
    w.dispatchDoc("scroll", { target: L });              // a row's menu: the subtitle list moves no row
    const rowStays = shown(w);
    w.dispatchDoc("scroll", { target: w.ids.queueList }); // ...its own list does
    const rowGoes = !shown(w);
    onPicture(w);
    w.dispatchDoc("scroll", { target: w.doc });          // the page itself scrolled: everything moved
    return followed && pictureStays && rowStays && rowGoes && !shown(w);
  };
  const rowLeaves = (w) => {
    open(w); queue(w);
    const r11 = w.ids.queueList.children[0];
    rclick(w, r11, 100, 60, r11);
    const was = shown(w);
    w.send({ type: "playerQueue", items: [{ id: 12, name: "film.mkv", dir: "S1", state: "ready", pct: 0, durMs: 0, resumeMs: 0 }], cur: 12,
             loaded: true, repeat: "off", shuffle: false, translate: true });
    return was && !shown(w);
  };
  // 8. its two other entries: "Load a subtitle file…" (the CC panel's button: the app's dialog) and the CC panel itself
  const loadAndSettings = (w) => {
    onPicture(open(w));
    w.outbox.length = 0;
    const l1 = pickEntry(w, /^Load a subtitle file…$/);
    const load = l1 && JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerSubLoad" }]) && !shown(w);
    onPicture(w);
    const l2 = pickEntry(w, /^Subtitle settings…$/);
    return l2 && load && !shown(w) && w.ids.ccPanel.hidden === false;
  };
  // 9. it is REPAINTED with the CC panel when what it lists moves -- the tracks arriving (the keyboard stays on its entry),
  //    a subtitle file the app hands over
  const repaintTracks = (w) => {
    open(w, { tracks: { state: "probing" }, audioOrd: -1 });
    onPicture(w);
    const before = part(w, "Audio") === null;
    key(w, "End"); key(w, "ArrowUp"); key(w, "ArrowUp");   // "Off"
    const on = focused(w);
    w.send(film({ tracks: T, audioOrd: 1 }));
    return before && on === "Off" && mAudio(w).join("|") === MENU_AUDIO && focused(w) === "Off" && shown(w);
  };
  const repaintSubs = (w) => {
    w.send(film({ translate: false, phase: "off" }));      // translation off: the sidecar is asked by itself (auto)
    metaOk(w.ids.fileVideo);
    onPicture(w);
    const before = mSubs(w).find((r) => /\*/.test(r));
    w.send({ type: "playerSubs", id: 9, name: "film.fr.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false,
             part: 0, parts: 1, cues: [{ i: 0, s: 0, e: 9000, r: [["Bonjour", 0]] }] });
    return before === "Off*" && mSubs(w).find((r) => /\*/.test(r)) === "film.fr.srt (next to the file)*" && shown(w);
  };
  // 10. FULL SCREEN: the menu works there and is an OPEN PANEL -- a right-click on a still, idle picture brings the cursor and
  //     the bar back (no move needed), and they stay while it is up; closed, the delay starts again from THEN
  const fullScreenMenu = (w) => {
    fsPlaying(w);
    const idle0 = idle(w);
    onPicture(w);
    const back = shown(w) && !idle(w);
    w.pumpTimers(10000);
    const held = shown(w) && !idle(w);
    key(w, "Escape");
    w.pumpTimers(2400);
    const notYet = !shown(w) && !idle(w);
    w.pumpTimers(300);
    return idle0 && back && held && notYet && idle(w);
  };
  // ==== THE REVIEW'S FIX ROUND (RV-CTX, 25/10: 0 HIGH, 2 MEDIUM, 8 LOW; the reviewer's probes P1-P8 are rebuilt here) ========
  // M1: up, it is placed AGAIN at every repaint, from the point it was opened at -- the tracks arriving while it is up took it
  //     from 7 entries to 13, and it ran past the window's bottom where it stood (P1: its bottom at 770 in a 560 window)
  const replacedOnRepaint = (w) => {
    open(w, { tracks: { state: "probing" }, audioOrd: -1 });
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 240, right: 280, bottom: 240 };   // small: no Audio part yet
    rclick(w, w.ids.fileVideo, 100, 300);
    const before = menu(w).style.left + "," + menu(w).style.top;        // below the pointer: 300 + 240 fits in 552
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 470, right: 280, bottom: 470 };   // what it measures once rebuilt
    w.send(film({ tracks: T, audioOrd: 1 }));                           // the tracks arrive: the Audio part + 4 embedded rows
    const after = menu(w).style.left + "," + menu(w).style.top;         // FLIPPED above the pointer: 300 - 470, held at 8
    return before === "100px,300px" && mAudio(w).length === 2 && after === "100px,8px" && shown(w);
  };
  // M2: the seek bar and the queue's grip answer the LEFT button only -- a right-click on the bar (it runs along the top of the
  //     transport, where the menu is asked for in full screen) SOUGHT there (P2: 30 s -> 450 s), a middle click too; a
  //     right-click on a grip started a drag
  const leftOnly = (w) => {
    open(w); queue(w);
    w.ids.fileVideo.currentTime = 30;
    w.ids.seekTrack._rect = { left: 0, top: 500, width: 760, height: 6, right: 760, bottom: 506 };
    const press = (button) => {
      const ev = { target: w.ids.seek, button, clientX: 600, clientY: 503, pointerId: 1 };
      w.ids.seek.dispatch("pointerdown", ev);
      w.ids.seek.dispatch("pointerup", ev);
      return w.ids.fileVideo.currentTime;
    };
    const right = press(2), middle = press(1);
    const g = w.ids.queueList.children[0].children[0];                  // the first row's grip
    w.outbox.length = 0;
    g.dispatch("pointerdown", { target: g, button: 2 });
    w.win.dispatch("pointermove", { clientY: 900 });
    w.win.dispatch("pointerup", {});
    const notDragged = w.outbox.length === 0 && !w.ids.queueList.children[0].classList.contains("is-drag");
    const left = press(0);                                              // the left button still seeks: 600 / 760 of 10:00
    return right === 30 && middle === 30 && notDragged && Math.abs(left - 600 * 600 / 760) < 0.01;
  };
  // L1: a DOUBLE-click on the picture with the menu up: its first click is eaten (it closed the menu) and so is the rest of the
  //     same double-click (e.detail > 1) -- the film went full screen PAUSED (P3); a later click is the picture's again
  const dblAfterDismiss = (w) => {
    open(w);
    w.ids.tPlay.click();
    onPicture(w);
    const click = (detail) => {
      w.dispatchDoc("pointerdown", { target: w.ids.fileVideo, button: 0 });
      w.win.dispatch("pointerdown", { target: w.ids.fileVideo, button: 0 });
      w.ids.fileView.dispatch("click", { target: w.ids.fileVideo, detail });
    };
    w.outbox.length = 0;
    click(1); click(2);
    w.ids.fileView.dispatch("dblclick", { target: w.ids.fileVideo });
    const fs = w.outbox.filter((m) => m.type === "translatorFullscreen").map((m) => m.on).join();
    const kept = w.ids.fileVideo.paused === false && !shown(w);
    click(1);                                             // a new click, later
    return kept && fs === "true" && w.ids.fileVideo.paused === true;
  };
  // L2: a MODIFIED key never reaches the page's shortcuts under the menu (P4: AltGr+] changed the speed; the editor's Alt+O ran)
  //     -- the subtitle size (Ctrl + / - / 0) is the one kept: it touches neither the menu nor the playback
  const modifiedKeys = (w) => {
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "fr", langB: "en" });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, langA: "fr", langB: "en", totalMs: 600000, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.send({ type: "translatorFileCue", jobId: 2, idx: 0, s: 20000, e: 24000, lang: "fr", dstLang: "en", pass: false, src: "Soit.", dst: "Either." });
    w.ids.fileVideo._seek(21);
    key(w, "e");                                          // the editor, on the line under the playhead
    const editing = w.ids.edPane.hidden === false;
    onPicture(w);
    w.outbox.length = 0;
    const rate0 = w.ids.tRate.textContent;
    // (whether the menu STOPS a key is recorded: this stub has no propagation -- the size listener, registered before the
    //  menu's capture listener, runs here whatever the menu decides; in a browser the stop is what keeps it from running)
    const stops = [];
    const mk = (k, o) => key(w, k, Object.assign({ stopPropagation() { stops.push(k); } }, o));
    mk("]", { ctrlKey: true, altKey: true });             // AltGr+] (AZERTY): the speed
    mk("o", { altKey: true });                            // the editor's Alt+O: the line's end at the playhead (a subEdit)
    mk("z", { ctrlKey: true });                           // the editor's Ctrl+Z
    const swallowed = w.ids.tRate.textContent === rate0 && w.outbox.length === 0 && shown(w) && stops.join() === "],o,z";
    const kOf = () => { const v = JSON.parse(w.lsStore["audiopb.tr.sub"] || "{}"); return v.k === undefined ? 4 : Number(v.k); };
    const k0 = kOf();
    mk("=", { ctrlKey: true });                           // Ctrl+= : the subtitle size -- kept, NOT stopped
    return editing && swallowed && stops.join() === "],o,z" && Math.abs(kOf() - (k0 + 0.2)) < 1e-9 && shown(w);
  };
  // L3: the key that CLOSED the menu, held: its repeats are swallowed until it is let go (P5: Enter's repeat landed on the
  //     queue row = "play" the file already playing = from the top; Space's = a pause; Esc's = leaving full screen)
  const heldKey = (w) => {
    open(w);
    w.ids.tPlay.click();
    onPicture(w);
    for (let i = 0; i < 20 && focused(w) !== "Off"; i++) key(w, "ArrowDown");
    key(w, " ");                                          // Space picks "Off": the menu closes
    const picked = !shown(w);
    key(w, " ", { repeat: true }); key(w, " ", { repeat: true });   // ...the key still held
    const heldOff = w.ids.fileVideo.paused === false;
    let dp = false;
    w.win.dispatch("keyup", { key: " ", target: w.doc.body, preventDefault() { dp = true; } });   // let go (its keyup taken)
    key(w, " ", { repeat: true });                        // the hold is over: a key again
    const after = w.ids.fileVideo.paused === true;
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    onPicture(w);
    w.outbox.length = 0;
    key(w, "Escape"); key(w, "Escape", { repeat: true }); // closed by Esc, held: its repeats do not leave full screen
    const stillFull = !shown(w) && w.outbox.filter((m) => m.type === "translatorFullscreen").length === 0;
    return picked && heldOff && dp && after && stillFull;
  };
  // L5: a KEYBOARD's menu (Shift+F10, the Menu key) -- told by the pointerdown it has NOT got: Chromium gives it the focused
  //     element's centre (P8) -- opens on its first entry, Enter right away picks it; after a right-click (its pointerdown used
  //     up by that menu) it is the keyboard's again; with NOTHING focused, Chromium aims it at the page: the picture's menu
  const keyboardOpening = (w) => {
    open(w); queue(w);
    const r11 = w.ids.queueList.children[0];
    r11._rect = { left: 400, top: 200, width: 300, height: 40, right: 700, bottom: 240 };
    r11.focus();
    menu(w)._rect = { left: 0, top: 0, width: 280, height: 100, right: 280, bottom: 100 };
    r11.dispatch("contextmenu", { target: r11, button: 0, clientX: 550, clientY: 220 });   // no pointerdown before it
    const first = focused(w) === "Play";
    w.outbox.length = 0;
    key(w, "Enter");
    const played = JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerQueue", op: "play", id: 11 }]);
    rclick(w, r11, 100, 60, r11);                         // a right-click: the mouse's menu, nothing focused in it
    const mouseNoFocus = shown(w) && focused(w) === "";
    key(w, "Escape");
    r11.dispatch("contextmenu", { target: r11, button: 0, clientX: 550, clientY: 220 });
    const secondByKey = focused(w) === "Play";
    key(w, "Escape");
    w.dispatchDoc("contextmenu", { target: w.doc.body, button: 0, clientX: 10, clientY: 10 });
    const page = shown(w) && focused(w) === "English speech (AAC 2.0)";
    return first && played && mouseNoFocus && secondByKey && page;
  };
  // L6: the click that dismisses ANY popover -- the CC panel (the menu's "Subtitle settings…" opens it), ⋯, the keys -- is not
  //     also the picture's (P6: it paused the film); a bar button's click while a panel is up still acts
  const eatsForAll = (w) => {
    open(w);
    w.ids.tPlay.click();
    const via = [() => { onPicture(w); pickEntry(w, /^Subtitle settings…$/); }, () => w.ids.tMore.click(), () => w.ids.tHelp.click()];
    const kept = via.every((go) => { go(); const up = anyUp(w); lclickPicture(w); return up && !anyUp(w) && w.ids.fileVideo.paused === false; });
    w.ids.tCC.click();
    const muted0 = w.ids.tMute.classList.contains("is-muted");
    w.win.dispatch("pointerdown", { target: w.ids.tMute, button: 0 });   // a bar button with the panel up: it closes the panel...
    w.ids.tMute.click();                                                  // ...and still acts
    lclickPicture(w);                                                     // ...and the picture's next click is its own: a pause
    return kept && w.ids.tMute.classList.contains("is-muted") !== muted0 && w.ids.fileVideo.paused === true;
  };
  // L7: the queue re-rendered while a row's menu is up (a badge, a duration: every row rebuilt) -- Esc gives the focus back to
  //     the row of the SAME item, not to the page (P7: the row it was opened on was gone, the focus fell to the body)
  const focusAfterRerender = (w) => {
    open(w); queue(w);
    const r11 = w.ids.queueList.children[0];
    r11.focus();
    rclick(w, r11, 100, 60, r11);
    key(w, "ArrowDown");
    queue(w);                                             // the same queue again: every row rebuilt
    const fresh = w.ids.queueList.children[0];
    key(w, "Escape");
    return fresh !== r11 && fresh.dataset.id === "11" && w.doc.activeElement === fresh;
  };

  // P1: ANY open popover holds the idle hide off (one list: popOpen), and its closing restarts the delay
  const POPS = {
    "the CC panel": (w) => w.ids.tCC.click(),
    "⋯": (w) => w.ids.tMore.click(),
    "the keys": (w) => w.ids.tHelp.click(),
    "Export": (w) => { w.send({ type: "translatorFileCue", jobId: 2, idx: 0, s: 1000, e: 2000, lang: "en", dstLang: "zh", pass: false, src: "a", dst: "b" }); w.ids.exportBtn.click(); },
    "the right-click menu": (w) => onPicture(w),
  };
  const anyUp = (w) => [w.ids.ccPanel, w.ids.morePanel, w.ids.helpPanel, w.ids.exportMenu, w.ids.ctxMenu].some((p) => !p.hidden);
  const heldBy = (name) => (w) => {
    fsPlaying(w);
    POPS[name](w);
    w.dispatchDoc("pointermove", { target: w.ids.fileVideo, clientX: 400, clientY: 200 });   // the hand on it: the bar and the cursor up
    const up = anyUp(w) && !idle(w);
    w.pumpTimers(8000);                                  // ...then still, reading it
    const held = !idle(w);
    key(w, "Escape");
    w.pumpTimers(2400);
    const notYet = !idle(w) && !anyUp(w);
    w.pumpTimers(300);
    return up && held && notYet && idle(w);
  };
  // ...closed any way: its own button, a ⋯ entry, a click outside
  const closeAnyWay = (w) => {
    fsPlaying(w);
    const ways = [
      () => { w.ids.tCC.click(); w.pumpTimers(5000); w.ids.tCC.click(); },
      () => { w.ids.tMore.click(); w.pumpTimers(5000); w.ids.morePanel.children.find((b) => /theme$/.test(b.textContent)).click(); },
      () => { w.ids.tCC.click(); w.pumpTimers(5000); w.win.dispatch("pointerdown", { target: w.ids.fileVideo, button: 0 }); },
    ];
    return ways.every((go) => {
      go();
      w.pumpTimers(2400);
      const notYet = !idle(w) && [w.ids.ccPanel, w.ids.morePanel].every((p) => p.hidden);
      w.pumpTimers(300);
      return notYet && idle(w);
    });
  };
  // P1: a click or a wheel with NO move is the hand on the mouse: the bar and the cursor come back
  const clickIsActivity = (w) => {
    fsPlaying(w);
    const idle0 = idle(w);
    w.dispatchDoc("pointerdown", { target: w.ids.fileVideo, button: 0 });
    const click = !idle(w);
    w.pumpTimers(2600);
    const again = idle(w);
    w.dispatchDoc("wheel", { target: w.ids.fileVideo, deltaY: 120 });
    return idle0 && click && again && !idle(w);
  };
  // P1: a change of full screen closes the popovers placed for the other layout (the CC panel over its button, Export under a
  //     bar full screen has not) -- the same state told again closes nothing
  const fsClosesPops = (w) => {
    open(w);
    w.ids.tCC.click();
    w.send({ type: "translatorWindow", shown: true, fullscreen: false, topmost: true });   // no change
    const kept = w.ids.ccPanel.hidden === false;
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    const inFull = w.ids.ccPanel.hidden === true;
    w.ids.tMore.click();
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    return kept && inFull && w.ids.morePanel.hidden === true;
  };
  // the markup and the look: the app's context menu (.menu, as Export's), outside the player's pane (it must rise over the
  // bar, like every popover), the CC panel's classes in it, no colour of its own
  const ctxCss = (() => { const m = /\/\* ---- THE RIGHT-CLICK MENU \(25\/10\)[\s\S]*?\*\/([\s\S]*?)\n\n/.exec(html); return m ? m[1] : ""; })();

  section("P-CTX 1 -- the right-click menu (25/10: « avec un clic droit sur la vidéo ou le fichier en train de jouer, ... les options liées au fichier »)");
  {
    ok("a right-click on the PICTURE opens the menu at the pointer: the film's sound tracks and its subtitles -- the CC panel's rows, word for word "
       + "(the current ones marked, the unusable ones disabled with the panel's reason) -- then \"Load a subtitle file…\", \"Subtitle settings…\"",
       menuIsThePanel(runTranslatorWindow()));
    ok("...on a sound's card too (an mp3: no Audio part, as in the CC panel)", menuOnSoundCard(runTranslatorWindow()));
    ok("...not on a control (a button, the volume, the seek bar, the Play-now card), but on the bar's empty space (over the picture in full screen)",
       notOnControls(runTranslatorWindow()));
    ok("...and nothing with no file open (a queue only)", noFileNoMenu(runTranslatorWindow()));
    ok("the queue: the PLAYING row's right-click is the file's menu; another row's, its own two actions (Play, Remove from the queue -- by id)",
       playingRow(runTranslatorWindow()));
    ok("ONE implementation: the menu's pick and the CC panel's, on twin windows -- a sound track, an embedded track, one language of SuVtitle's, "
       + "Off -- leave the same page and send the app the same words", sameAsPanel(runTranslatorWindow()));
    ok("...the menu's code lists audioState() / sources() with ccRow and picks with pickAudio / pickSource -- it never touches a track itself",
       oneImplementation(runTranslatorWindow()));
    ok("what cannot be used is DISABLED with the CC panel's reason (\"image subtitles, not supported\", \"can't be read\"): a click does nothing, "
       + "the keyboard steps over it", disabledSame(runTranslatorWindow()));
    ok("...a sound track this WebView cannot play (DTS): \"can't be played here\", disabled -- as the panel", dtsSame(runTranslatorWindow()));
    ok("...ONE sound track: no Audio part (as #ccAudio, hidden under two rows)", oneTrack(runTranslatorWindow()));
    ok("...played through the sound copy: every row disabled, the first heard, and the panel's sentence under them", proxySame(runTranslatorWindow()));
    ok("a menu's keyboard: ↓ / ↑ (round), Home / End, Enter and Space pick -- and nothing reaches the player while it is up (no seek, volume, "
       + "pause, S, M)", keyboard(runTranslatorWindow()));
    ok("Esc closes it and gives the focus back (the queue row it was opened on); Tab closes it", escAndFocus(runTranslatorWindow()));
    ok("...its keys are heard FIRST (the window's capture phase) and stopped there: the focused queue row never takes Enter for \"play me\" (pinned: no propagation here)",
       keysCaptured(runTranslatorWindow()));
    ok("ONE selection for the hand and the keyboard: the entry under the pointer is the keyboard's (Enter picks it, ↓ goes on); a disabled one takes nothing",
       hoverIsSelection(runTranslatorWindow()));
    ok("at the pointer; FLIPPED to the pointer's left / above it near the right / bottom edge; never past the window", placed(runTranslatorWindow()));
    ok("a click outside closes it -- and is not ALSO a click on the picture (no pause); the next one is", clickOutside(runTranslatorWindow()));
    ok("a right-click elsewhere moves it there", rightClickMoves(runTranslatorWindow()));
    Object.keys(closers).forEach((n) => ok("it closes on " + n, closesOn(n)(runTranslatorWindow())));
    ok("...on leaving full screen (the same state told again closes nothing)", leavingFull(runTranslatorWindow()));
    ok("...but not on a scroll or a wheel INSIDE it (a long menu scrolls)", staysOnInside(runTranslatorWindow()));
    ok("...and a row's menu goes when its row leaves the queue", rowLeaves(runTranslatorWindow()));
    ok("\"Load a subtitle file…\" = the CC panel's button (the app's dialog); \"Subtitle settings…\" opens the CC panel",
       loadAndSettings(runTranslatorWindow()));
    ok("REPAINTED with the CC panel: the tracks arriving while it is up appear in it (the keyboard stays on its entry)", repaintTracks(runTranslatorWindow()));
    ok("...and a subtitle file the app hands over is marked in it", repaintSubs(runTranslatorWindow()));
    ok("full screen: a right-click on a still, idle picture brings the bar and the cursor back with the menu; they stay while it is up; closed, "
       + "they go 2.5 s later (not before)", fullScreenMenu(runTranslatorWindow()));
    const mk = /<div id="ctxMenu" class="menu ctx-menu" role="menu" aria-label="This file" hidden><\/div>/.exec(html);
    ok("the markup: #ctxMenu is the app's context menu (.menu, role menu, hidden), with the other popovers OUTSIDE #filePane (it rises over the bar)",
       !!mk && html.indexOf("<section id=\"filePane\">") < html.indexOf("</section>") && html.indexOf("</section>") < mk.index);
    ok("the look: no colour of its own -- the tokens and the CC panel's classes only (.cc-src, .pop-title, .cc-note); the keyboard's row looks like a hovered one",
       !!ctxCss && !/#[0-9a-f]{3,8}\b|rgba?\(/i.test(ctxCss) && /\.ctx-menu \.cc-src \{/.test(ctxCss)
       && /\.ctx-sep \{ height: 1px; margin: 5px 6px; background: var\(--border-soft\); \}/.test(ctxCss)
       && /\.ctx-menu button:focus-visible \{ outline: none; background: var\(--accent-soft\); \}/.test(ctxCss)
       && /\.menu button:hover \{ background: var\(--accent-soft\); \}/.test(html), ctxCss.slice(0, 120));
  }

  section("P-CTX 3 -- the review's fix round (RV-CTX: 2 MEDIUM, 7 LOW fixed; L8 known, not fixed)");
  {
    ok("M1: up, it is placed AGAIN at every repaint from where it was opened -- the tracks arriving (7 -> 13 entries) flip it above the "
       + "pointer instead of running it past the window's bottom", replacedOnRepaint(runTranslatorWindow()));
    ok("M2: the seek bar and the queue's grip answer the LEFT button only (a right or middle click on the bar sought: 30 s -> 450 s; on a grip, a drag)",
       leftOnly(runTranslatorWindow()));
    ok("L1: a double-click on the picture with the menu up: the rest of the double-click is eaten with its first click -- full screen, still "
       + "playing; a later click is the picture's", dblAfterDismiss(runTranslatorWindow()));
    ok("L2: a MODIFIED key never reaches the page's shortcuts under the menu (AltGr+] the speed, the editor's Alt+O / Ctrl+Z); the subtitle "
       + "size (Ctrl + / - / 0) is kept", modifiedKeys(runTranslatorWindow()));
    ok("L3: the key that closed the menu, HELD, is swallowed until it is let go (Space's repeats do not pause, Esc's do not leave full "
       + "screen; its keyup taken too); after the keyup it is a key again", heldKey(runTranslatorWindow()));
    ok("L4: a scroll closes it only when it moves what it was opened on (the queue list under a row's menu, the page) -- never the "
       + "subtitle list's own scrolls (following the playhead, scroll anchoring)", scrollRule(runTranslatorWindow()));
    ok("L5: a KEYBOARD's menu is told by the pointerdown it has not got (not by coordinates): on its first entry, Enter picks it; "
       + "after a right-click it is the keyboard's again; with nothing focused, the picture's", keyboardOpening(runTranslatorWindow()));
    ok("L6: the click that dismisses ANY popover (CC -- from the menu --, ⋯, the keys) is not also the picture's; a bar button's click "
       + "still acts", eatsForAll(runTranslatorWindow()));
    ok("L7: the queue re-rendered while a row's menu is up: Esc gives the focus back to the row of the SAME item", focusAfterRerender(runTranslatorWindow()));
  }

  section("P-CTX 2 -- full screen's idle hide, its LOGIC (« on ne voit plus la souris du tout, même quand on la bouge »)");
  {
    Object.keys(POPS).forEach((n) => ok("never under an open popover -- " + n + ": the bar and the cursor stay while it is up, and go 2.5 s after it closes",
                                       heldBy(n)(runTranslatorWindow())));
    ok("...closed ANY way (its own button, a ⋯ entry, a click outside): the delay starts again from then", closeAnyWay(runTranslatorWindow()));
    ok("a click or a wheel with NO move is the hand on the mouse: the bar and the cursor come back", clickIsActivity(runTranslatorWindow()));
    ok("a change of full screen closes the popovers placed for the other layout; the same state told again closes nothing",
       fsClosesPops(runTranslatorWindow()));
  }

  // ---- MUTATION SENTINELS: each rule taken out of the page must be CAUGHT by its probe ----------------------------------------
  section("P-CTX -- mutation sentinels (index.html with one rule taken out each)");
  {
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
    // (sanity: every probe passes on the real page -- they ran above, one by one)
    judge("the menu's sound track picked by a COPY of the logic (the element switched, no toast, the app never told)",
      R("r.on, r.disabled, () => pickAudio(r)", "r.on, r.disabled, () => { const A = audioEntries(media); A.forEach((a, i) => { a.enabled = i === r.idx; }); }"),
      sameAsPanel);
    judge("the menu's subtitle source picked by a copy (subSrc set, the app never asked)",
      R("!!s.disabled, () => pickSource(s)", "!!s.disabled, () => { subSrc = s.src === \"off\" ? \"off\" : s.src; lastSubKey = null; paintSubs(); }"),
      sameAsPanel);
    judge("...a copy that sets the variables itself (the source check)", R("!!s.disabled, () => pickSource(s)", "!!s.disabled, () => { wantExt = 0; pickSource(s); }"),
      oneImplementation);
    judge("the current source not marked in the menu", R("s.key === cur, !!s.disabled, () => pickSource(s)", "false, !!s.disabled, () => pickSource(s)"),
      menuIsThePanel);
    judge("an unusable subtitle track choosable in the menu", R("s.key === cur, !!s.disabled, () => pickSource(s)", "s.key === cur, false, () => pickSource(s)"),
      disabledSame);
    judge("an unusable sound track choosable in the menu", R("r.label, r.hint, r.on, r.disabled, () => pickAudio(r)", "r.label, r.hint, r.on, false, () => pickAudio(r)"),
      dtsSame);
    judge("the panel's note missing from the menu", R("        if (note) out.push(line(\"cc-note\", note, \"presentation\"));\n", ""), proxySame);
    judge("the menu's Audio part shown for ONE track too (its own rule, not #ccAudio's)",
      R("const S = audioState(), show2 = S.rows.length >= 2;", "const S = audioState(), show2 = S.rows.length >= 1;"), oneTrack);
    judge("a disabled entry's click obeyed", R("      if (node.disabled || !ctxShown()) return;\n", "      if (!ctxShown()) return;\n"), disabledSame);
    judge("the keyboard landing on disabled entries", R("=== \"BUTTON\" && !n.disabled);", "=== \"BUTTON\");"), disabledSame);
    const CAP = "    if (ctxHeldKey(e) || ctxKey(e)) { ctxKeyEv = e; if (e.stopPropagation) e.stopPropagation(); }";
    judge("the keyboard not the menu's (the player under it answers)", R("    if (ctxHeldKey(e) || ctxKey(e)) { ctxKeyEv = e;", "    if (false) { ctxKeyEv = e;"), keyboard);
    judge("the player's keydown acting on a key the menu took (an arrow moved the menu AND the volume)",
      R("    if (e === ctxKeyEv) return;                // (25/10)", "    // (25/10)"), keyboard);
    judge("the menu's keys heard in the BUBBLE phase (the focused queue row hears Enter first)", R(CAP + "\n  }, true);", CAP + "\n  });"), keysCaptured);
    judge("...or not stopped there", R(CAP, "    if (ctxHeldKey(e) || ctxKey(e)) { ctxKeyEv = e; }"), keysCaptured);
    judge("the hand and the keyboard two selections (Enter ignores the entry under the pointer)",
      R("      if (node.disabled || document.activeElement === node) return;\n      try { node.focus({ preventScroll: true }); } catch { /* fine */ }\n",
        "      if (node.disabled || document.activeElement === node) return;\n"), hoverIsSelection);
    judge("...a disabled entry under the pointer taking the keyboard", R("      if (node.disabled || document.activeElement === node) return;\n      try { node.focus",
      "      if (document.activeElement === node) return;\n      try { node.focus"), hoverIsSelection);
    judge("Enter / Space not picking", R("!a.disabled) { ctxHeld = keyId(e); a.click(); }", "!a.disabled) { ctxHeld = keyId(e); }"), keyboard);
    judge("no round trip at the ends", R(": (i + d + L.length) % L.length;", ": clamp(i + d, 0, L.length - 1);"), keyboard);
    judge("the focus not given back on closing", R("typeof back.focus === \"function\") back.focus();", "typeof back.focus === \"function\") a.blur();"),
      escAndFocus);
    judge("Tab leaves it open", R("    } else if (k === \"Escape\" || k === \"Tab\") { ctxHeld = keyId(e); closePops(); }",
      "    } else if (k === \"Escape\") { ctxHeld = keyId(e); closePops(); }"), escAndFocus);
    judge("no flip at the right edge", R("    const left = x + w <= W - PAD ? x : x - w;", "    const left = x;"), placed);
    judge("no flip at the bottom edge", R("    const top = y + h <= H - PAD ? y : y - h;", "    const top = y;"), placed);
    judge("the dismissing click ALSO a click on the picture (a pause)", R("    if (eatClick) { eatClick = false; ateClick = true; return; }", "    if (false) return;"),
      clickOutside);
    judge("every later click on the picture eaten (the flag never cleared)", R("  window.addEventListener(\"pointerdown\", (e) => {\n    eatClick = false;\n",
      "  window.addEventListener(\"pointerdown\", (e) => {\n"), clickOutside);
    // (U2: the models menu joined the list -- the literal re-pointed, the rule unchanged)
    judge("a click outside leaves it open (not in the popovers' list)", R("[el.ccPanel, el.morePanel, el.helpPanel, el.exportMenu, el.ctxMenu, el.modelsMenu];",
      "[el.ccPanel, el.morePanel, el.helpPanel, el.exportMenu, el.modelsMenu];"), clickOutside);
    judge("a right-click on a control opens it", R("if (tag === \"BUTTON\" || tag === \"INPUT\" || tag === \"SELECT\" || n === el.seek) return;", "if (false) return;"),
      notOnControls);
    judge("a menu with no file open", R("    if (F.phase === \"idle\") return;           // nothing open", "    if (false) return;           // nothing open"), noFileNoMenu);
    judge("the playing row taken for another row", R("const playing = F.phase !== \"idle\" && F.itemId > 0 && it.id === F.itemId;", "const playing = false;"),
      playingRow);
    judge("every row taken for the playing one", R("const playing = F.phase !== \"idle\" && F.itemId > 0 && it.id === F.itemId;", "const playing = true;"),
      playingRow);
    judge("a row's Play by position", R("() => send({ type: \"playerQueue\", op: \"play\", id })", "() => send({ type: \"playerQueue\", op: \"play\", id: 1 })"),
      playingRow);
    const SCROLL = "    if (ctxShown() && (t === document || t === document.documentElement || holds(t, ctxAnchor))) closeCtx();";
    judge("it stays on a scroll (a row's menu on its list's)", R(SCROLL, ""), scrollRule);
    judge("it closes on a scroll INSIDE it", R(SCROLL, SCROLL.replace("holds(t, ctxAnchor)))", "holds(t, ctxAnchor) || insideCtx(t)))")), staysOnInside);
    judge("it stays on a wheel outside", R("window.addEventListener(\"wheel\", (e) => { if (ctxShown() && !insideCtx(e && e.target)) closeCtx(); }, { passive: true });", ""),
      closesOn("a wheel outside"));
    judge("it stays on the window's blur", R("  window.addEventListener(\"blur\", () => { ctxHeld = \"\"; if (ctxShown()) closeCtx(); });\n",
      "  window.addEventListener(\"blur\", () => { ctxHeld = \"\"; });\n"), closesOn("the window's blur"));
    judge("it stays on a resize", R("  window.addEventListener(\"resize\", () => { if (ctxShown()) closeCtx(); });", ""), closesOn("a resize"));
    judge("it survives another file", R("      closeCtx();                              // (25/10: the right-click menu spoke of the old one)\n", ""),
      closesOn("another file"));
    judge("it survives a change of full screen", R("    if (was !== isFull) closePops();\n", ""), closesOn("full screen"));
    judge("it closes on the same full-screen state told again", R("    if (was !== isFull) closePops();\n", "    closePops();\n"), leavingFull);
    judge("it survives the window hidden", R("    closeCtx();                                // (25/10) the right-click menu does not wait", "    // (25/10) the right-click menu does not wait"),
      closesOn("the window hidden"));
    judge("a row's menu outlives its row", R("    if (ctxFor && ctxFor.kind === \"row\" && !Q.items.some((x) => x.id === ctxFor.id)) closeCtx();\n", ""), rowLeaves);
    judge("\"Load a subtitle file…\" not the CC panel's (no dialog asked)", R("out.push(act(\"load\", \"Load a subtitle file…\", () => loadSubFile()));",
      "out.push(act(\"load\", \"Load a subtitle file…\", () => {}));"), loadAndSettings);
    judge("\"Subtitle settings…\" opening nothing", R("() => { if (el.ccPanel.hidden) togglePop(el.ccPanel, el.tCC); }", "() => {}"), loadAndSettings);
    judge("not repainted when the tracks arrive", R("    if (!el.ctxMenu.hidden) paintCtx();        // (25/10: the right-click menu lists the same rows)\n", ""), repaintTracks);
    judge("the keyboard thrown off its entry by a repaint", R("      try { if (b) b.focus(); else if", "      try { if (false) b.focus(); else if"), repaintTracks);
    judge("not repainted when a subtitle file is handed over (repaintCc = the CC panel only)",
      R("  function repaintCc() {\n    if (!el.ccPanel.hidden) paintCc();\n    if (!el.ctxMenu.hidden) paintCtx();\n",
        "  function repaintCc() {\n    if (!el.ccPanel.hidden) paintCc();\n"), repaintSubs);
    judge("full screen: the menu opened over a hidden cursor (no showControls on opening, no click-activity)",
      (s) => R("    showControls();                            // up: full screen's bar and cursor stay", "    // (gone)")(
             R("  [\"pointerdown\", \"wheel\"].forEach((t) => document.addEventListener(t, (e) => { noteInput(e); if (isFull) showControls(); },", "  [].forEach((t) => document.addEventListener(t, (e) => { noteInput(e); if (isFull) showControls(); },")(s)),
      fullScreenMenu);
    // P1
    judge("P1: the idle hide back to its three popovers (CC, ⋯, keys)", R("      if (isFull && media && !media.paused && !popOpen())",
      "      if (isFull && media && !media.paused && el.ccPanel.hidden && el.morePanel.hidden && el.helpPanel.hidden)"), heldBy("the right-click menu"));
    judge("P1: ...Export forgotten", R("[el.ccPanel, el.morePanel, el.helpPanel, el.exportMenu, el.ctxMenu, el.modelsMenu];", "[el.ccPanel, el.morePanel, el.helpPanel, el.ctxMenu, el.modelsMenu];"),
      heldBy("Export"));
    judge("P1: the delay not restarted when a popover closes", R("    if (was && !popOpen()) showControls();\n", ""), heldBy("the CC panel"));
    judge("P1: a ⋯ entry closing its panel behind closePops' back", R("b.addEventListener(\"click\", () => { closePops(); fn(); });", "b.addEventListener(\"click\", () => { el.morePanel.hidden = true; fn(); });"),
      closeAnyWay);
    judge("P1: a click with no move not activity", R("  [\"pointerdown\", \"wheel\"].forEach((t) =>", "  [\"wheel\"].forEach((t) =>"), clickIsActivity);
    judge("P1: a wheel with no move not activity", R("  [\"pointerdown\", \"wheel\"].forEach((t) =>", "  [\"pointerdown\"].forEach((t) =>"), clickIsActivity);
    judge("P1: a change of full screen leaves the popovers open", R("    if (was !== isFull) closePops();\n", ""), fsClosesPops);
    // the review's fix round (RV-CTX)
    judge("M1: not placed again on a repaint (it runs past the window's edge when the tracks arrive)",
      R("    if (!m.hidden && ctxAt) placeCtx(ctxAt.x, ctxAt.y);\n", ""), replacedOnRepaint);
    judge("M2: a right / middle click on the seek bar seeks", R("    if (!hasSrc() || Number(e && e.button) > 0) return;\n    seekDrag = true;",
      "    if (!hasSrc()) return;\n    seekDrag = true;"), leftOnly);
    judge("M2: a right-click on a queue grip drags", R("        if (Number(e && e.button) > 0) return;   // (review RV-CTX M2", "        // (review RV-CTX M2"), leftOnly);
    judge("L1: the rest of a double-click whose first click was eaten toggles the playback", R("    if (ateClick && Number(e && e.detail) > 1) return;\n", ""),
      dblAfterDismiss);
    judge("L2: modified keys let through under the menu (the 25/10 rule)", R("    if (e.ctrlKey || e.altKey || e.metaKey) return !subSizeKey(e);",
      "    if (e.ctrlKey || e.altKey || e.metaKey) return false;"), modifiedKeys);
    judge("L2: the subtitle size swallowed too", R("    if (e.ctrlKey || e.altKey || e.metaKey) return !subSizeKey(e);",
      "    if (e.ctrlKey || e.altKey || e.metaKey) return true;"), modifiedKeys);
    judge("L2: the editor's keydown acting on a key the menu took", R("    if (mode !== \"file\" || !ed.on || e === ctxKeyEv) return;",
      "    if (mode !== \"file\" || !ed.on) return;"), modifiedKeys);
    judge("L3: the held key's repeats let through", R("    if (!ctxHeld || keyId(e) !== ctxHeld) return false;\n    if (!e.repeat)",
      "    if (true) return false;\n    if (!e.repeat)"), heldKey);
    judge("L3: the hold never ends at the keyup", R("    if (!ctxHeld || keyId(e) !== ctxHeld) return;\n    ctxHeld = \"\";\n",
      "    if (!ctxHeld || keyId(e) !== ctxHeld) return;\n"), heldKey);
    judge("L3: Esc closing it not held (its repeats leave full screen)", R("    } else if (k === \"Escape\" || k === \"Tab\") { ctxHeld = keyId(e); closePops(); }",
      "    } else if (k === \"Escape\" || k === \"Tab\") { closePops(); }"), heldKey);
    judge("L4: every scroll closes it (the subtitle list's own and its anchoring too)", R(SCROLL, "    if (ctxShown()) closeCtx();"), scrollRule);
    judge("L5: a keyboard's menu told by its coordinates again", R("    const byKey = !(nowMs() - lastPointerDown < CTX_POINTER_MS);",
      "    const byKey = !(Number(e && e.clientX) > 0 || Number(e && e.clientY) > 0);"), keyboardOpening);
    judge("L5: the right-click's pointerdown not used up (a Shift+F10 after it taken for the mouse's)",
      R("    lastPointerDown = -1e12;\n    ctxRestore = document.activeElement", "    ctxRestore = document.activeElement"), keyboardOpening);
    judge("L5: nothing focused: no menu at all", R("    if (t !== document.body && t !== document.documentElement) return;\n", "    return;\n"), keyboardOpening);
    judge("L6: only the right-click menu's dismissing click eaten (the 25/10 rule)", R("    if (!(Number(e && e.button) > 0)) eatClick = true;",
      "    if (!el.ctxMenu.hidden && !(Number(e && e.button) > 0)) eatClick = true;"), eatsForAll);
    judge("L7: the focus given back to a row a re-render took out (it falls to the body)",
      R("    if (back && !inDoc(back) && ctxRestoreId) back = kids(el.queueList).find((r) => r.dataset && r.dataset.id === ctxRestoreId) || null;\n", ""),
      focusAfterRerender);
    judge("P1: ...every translatorWindow closes them (a topmost change too)", R("    if (was !== isFull) closePops();\n", "    closePops();\n"), fsClosesPops);
  }
};

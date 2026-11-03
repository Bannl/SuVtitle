// test_wh.js -- wave WH, the PAGE side: the film's OWN tracks in app/web/index.html -- the sound track heard (the CC
// panel's Audio section, the key A, the mapping D4 from the app's tracks to Chromium's audioTracks, the enforcement D3),
// the embedded subtitle tracks (D7: listed with the files next to the film, read-only in the editor), the default D10,
// the languages D11 -- and the WH-P FIX ROUND (REVIEW_WH_P.md, WHP-1..12, the director's decisions in WH_PROGRESS.md
// "WH-P FIX ROUND"). Required by test_ui.js (same ok/section counters). Every check runs the REAL page against the FROZEN
// contract (WH_PLAN_pistes.md section 2 + its addenda) with a fake app; each rule has a MUTATION SENTINEL at the end.
// PORTED (S-WEB, 2026-10-23) from AudioPB's scratchpad/ui_test/test_wh.js of the 641f1aa era. At 641f1aa the fix round was
// cut in the middle (the user's stop, 11/10 ~23:15): the PAGE half had landed (WHP-1..9, 12; the stub's WHP-11 in
// domstub.js), the HARNESS half had not -- 20 checks failed (wh\harness_fails_at_stop.txt). This file is that half:
// round 1's checks re-pointed to the decided behaviours, WHP-10's missing checks and sentinels, one check + one sentinel
// per WHP rule, and the parity of the page's failure filter with the app's MediaTracks.h sentences.
module.exports = function whChecks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, rows, countType, html, P } = ctx;

  // ---- the fixtures: the director's test media (WH_PLAN "Test media") and the user's film (F4) -----------------------
  const A_ = (id, label, language) => ({ id: String(id), kind: "main", label, language });
  const vid = { codec: "V_MPEG4/ISO/AVC", w: 640, h: 360 };
  // mkv_2a_def2.mkv: audio #2 eng "English speech" (FlagDefault 0) + #3 chi "Chinese speech" (default) -> the app
  // chooses ord 1; subs #4 S_TEXT/UTF8 eng, #5 S_TEXT/ASS fre "French ASS" FORCED, #6 D_WEBVTT ger
  const T_2A = { state: "ready", container: "mkv", video: vid,
    audio: [{ ord: 0, track: 2, name: "English speech", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 1, track: 3, name: "Chinese speech", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }],
    subs: [{ id: 41, track: 4, name: "", lang: "eng", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: false, ok: true },
           { id: 42, track: 5, name: "French ASS", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: true, ok: true },
           { id: 43, track: 6, name: "", lang: "ger", codec: "D_WEBVTT/SUBTITLES", kind: "text", def: false, forced: false, ok: true }] };
  // ...as Chromium lists it (F3): both decodable, ids of its OWN (the video is "1"), the FIRST one enabled
  const C_2A = [A_(2, "English speech", "eng"), A_(3, "Chinese speech", "chi")];
  // mkv_dts_first.mkv: #2 A_DTS eng -- ABSENT from Chromium's list --, #3 AAC chi, listed with id "2"
  const T_DTS = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_DTS", ch: 6, def: true, play: false },
            { ord: 1, track: 3, name: "", lang: "chi", codec: "A_AAC", ch: 2, def: true, play: true }] };
  const C_DTS = [A_(2, "", "chi")];
  // mp4_2a_movtext.mp4: both hdlr names "SoundHandler" (generic), mdhd eng / chi; a mov_text track
  const T_MP4 = { state: "ready", container: "mp4", video: { codec: "avc1", w: 640, h: 360 },
    audio: [{ ord: 0, track: 2, name: "SoundHandler", lang: "eng", codec: "mp4a", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "SoundHandler", lang: "chi", codec: "mp4a", ch: 2, def: false, play: true }],
    subs: [{ id: 51, track: 4, name: "SubtitleHandler", lang: "eng", codec: "tx3g", kind: "text", def: false, forced: false, ok: true }] };
  const C_MP4 = [A_(2, "SoundHandler", "eng"), A_(3, "SoundHandler", "chi")];
  // the user's film (F4, mkvmerge): AAC #2 eng "Studio 5.1 Surround", #3 eng "Dubbing-House Stereo", #4 fre, #5 jpn, #6 nor;
  // S_TEXT/ASS #7 (no Language = eng) #8 fre #9 fin #10 rum #11 cze; S_VOBSUB #12 kor #13 chi #14 chi
  const ss = (id, track, lang, kind, o) => Object.assign({ id, track, name: "", lang, codec: kind === "image" ? "S_VOBSUB" : "S_TEXT/ASS",
                                                            kind, def: false, forced: false, ok: true }, o);
  const T_FILM = { state: "ready", container: "mkv", video: { codec: "V_MPEGH/ISO/HEVC", w: 1920, h: 1040 },
    audio: [{ ord: 0, track: 2, name: "Studio 5.1 Surround", lang: "eng", codec: "A_AAC", ch: 6, def: true, play: true },
            { ord: 1, track: 3, name: "Dubbing-House Stereo", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 2, track: 4, name: "", lang: "fre", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 3, track: 5, name: "", lang: "jpn", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 4, track: 6, name: "", lang: "nor", codec: "A_AAC", ch: 2, def: false, play: true }],
    subs: [ss(71, 7, "eng", "text"), ss(72, 8, "fre", "text"), ss(73, 9, "fin", "text"), ss(74, 10, "rum", "text"), ss(75, 11, "cze", "text"),
           ss(76, 12, "kor", "image"), ss(77, 13, "chi", "image"), ss(78, 14, "chi", "image")] };
  const C_FILM = [A_(2, "Studio 5.1 Surround", "eng"), A_(3, "Dubbing-House Stereo", "eng"), A_(4, "", "fre"), A_(5, "", "jpn"), A_(6, "", "nor")];
  const copy = (o) => JSON.parse(JSON.stringify(o));
  const MKV_URL = "https://f0123456789abcdef.suvtitle-media.example/film.mkv?v=mk1";
  const PROXY_URL = "https://suvtitle-proxy.example/mk1-1.wav";
  const PROXY_LINE = "This file plays through SuVtitle's sound copy: its first sound track only.";
  const FAIL_TRACK = "This subtitle track is compressed in a way SuVtitle can't read.";   // the app's MediaTracks.h errCompressed()

  // ---- helpers ---------------------------------------------------------------------------------------------------
  const v = (w) => w.ids.fileVideo;
  const heard = (w, m) => (m || v(w)).audioTracks.enabledIds().join();
  const tMsg = (o) => fileMsg(Object.assign({ jobId: 2, phase: "analyzing", doneMs: 60000, ranges: [[0, 60000]], tracks: T_2A, audioOrd: 1 }, o));
  // a file whose tracks the app has read, then the metadata (Chromium's list first, as it is built before the event)
  const open = (w, o, list) => { w.send(tMsg(o)); v(w)._setAudioTracks(list || C_2A); metaOk(v(w)); return w; };
  const pair = (w, a, b, mode) => w.send({ type: "translatorState", running: true, status: "listening", mode: mode || "auto", langA: a, langB: b });
  const audioRows = (w) => w.ids.ccAudio.children.map((b) => b.textContent + (b.classList.contains("is-on") ? "*" : "") + (b.disabled ? "!" : ""));
  const srcRows = (w) => w.ids.ccSources.children.map((b) => b.textContent + (b.classList.contains("is-on") ? "*" : "") + (b.disabled ? "!" : ""));
  const cc = (w) => { if (w.ids.ccPanel.hidden) w.ids.tCC.click(); return w; };
  const sets = (w) => w.outbox.filter((m) => m && m.type === "playerSet" && m.audioOrd !== undefined);
  const setOrds = (w) => sets(w).map((s) => s.audioOrd).join();
  const subGets = (w) => w.outbox.filter((m) => m && m.type === "playerSubGet");
  const key = (w, k, o) => w.win.dispatch("keydown", Object.assign({ key: k, target: w.doc.body }, o || {}));
  const toast = (w) => (w.ids.fileToast.hidden ? "" : w.ids.fileToast.textContent);
  const pick = (w, k) => { cc(w); const b = w.ids.ccSources.children.find((x) => x.dataset.key === k); if (!b) throw new Error("no source " + k); b.click(); };
  const onSrc = (w) => { cc(w); const b = w.ids.ccSources.children.find((x) => x.classList.contains("is-on")); return b ? b.dataset.key : ""; };
  const SETTLE = 1000;                                          // past the page's 800 ms "the viewer stopped choosing" (WHP-6)
  const embSubs = (o) => Object.assign({ type: "playerSubs", id: 72, name: "French (track 8)", enc: 65001, encName: "UTF-8", forced: true,
    truncated: false, part: 0, parts: 1, embedded: true, track: 8,
    cues: [{ i: 0, s: 1000, e: 3000, r: [["Bonjour", 0]] }, { i: 1, s: 4000, e: 6000, r: [["Au revoir", 0]] }] }, o);
  const sideSubs = (o) => Object.assign({ type: "playerSubs", id: 9, name: "film.fr.srt", enc: 65001, encName: "UTF-8", forced: false,
    truncated: false, part: 0, parts: 1, cues: [{ i: 0, s: 1000, e: 3000, r: [["Salut", 0]] }] }, o);
  const pageScript = (html.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
  // the page's pure language/name functions, lifted out of a script (the REAL one, or a mutant's: w.script)
  const lift = (src, from, to) => { const i = src.indexOf(from), j = src.indexOf(to, i + 1); return i >= 0 && j > i ? src.slice(i, j) : ""; };
  const libOf = (src) => {
    try {
      return new Function(lift(src, "  const TR_LANGS = [", "  /* A line's language as an HTML `lang` value")
        + lift(src, "  // ---- D11: a track's language, in whatever form", "  // ---- a sound track in a player's words")
        + lift(src, "  // ---- a sound track in a player's words", "  // A row's words:")
        + "\nreturn { langInfo, langFamily, pairLangEn, speechInfo, codecName, chName, trackName, cleanName };")();
    } catch (e) { return null; }
  };

  // ---- the probes (each is a check below AND the condition its mutation sentinel must see FAIL) ---------------------
  // D3: the app's choice (ord 1, FlagDefault) is ENFORCED at the metadata -- Chromium had enabled its first (F3)
  const enforcedOnMeta = (w) => { open(w); return heard(w) === "3"; };
  // D4: one entry enabled, never two -- on a runtime that keeps several enabled (the spec's letter), the page's own
  // "the others off" is what keeps one (Chromium does it by itself: measured, domstub.js)
  const exactlyOne = (w) => {
    open(w);
    const a = v(w).audioTracks.enabledIds().length === 1;
    cc(w); w.ids.ccAudio.children[0].click();
    return a && heard(w) === "2";
  };
  // ...and only what differs is written: a translatorFile a second costs nothing (Chromium: the enabled one written,
  // the other disabled by Chromium itself; the spec's letter: both written once)
  const idempotentOn = (w, writes) => {
    open(w);
    const n = v(w).audioTracks.enabledLog.length;
    for (let i = 0; i < 5; i++) w.send(tMsg({ doneMs: 60000 + i * 1000 }));
    return n === writes && v(w).audioTracks.enabledLog.length === n;
  };
  const idempotent = (w) => idempotentOn(w, 1);
  // D3: the sound switches BEFORE anything plays (a file opened to play by itself)
  const beforePlay = (w) => {
    let atPlay = "";
    const play0 = v(w).play;
    v(w).play = () => { atPlay = heard(w); return play0(); };
    w.send(tMsg({ phase: "done", complete: true, autoplay: true, ranges: [[0, 600000]] }));
    v(w)._setAudioTracks(C_2A);
    metaOk(v(w));
    return v(w).playCalls === 1 && atPlay === "3";
  };
  // D3: the app's tracks arrive AFTER the metadata (still probing): enforced on that translatorFile
  const readyLater = (w) => {
    open(w, { tracks: { state: "probing" }, audioOrd: -1 });
    const untouched = heard(w) === "2" && v(w).audioTracks.enabledLog.length === 0;
    w.send(tMsg({ audioOrd: 1 }));
    return untouched && heard(w) === "3";
  };
  // D4: an undecodable track (DTS, absent from Chromium's list) is listed DISABLED; the AAC one is Chromium's [0]
  const dtsListed = (w) => {
    cc(open(w, { tracks: T_DTS, audioOrd: 1 }, C_DTS));
    return audioRows(w).join("|") === "English (DTS 5.1 · can't be played here)!|Chinese (AAC 2.0)*" && heard(w) === "2" && sets(w).length === 0;
  };
  // D4: the app's choice cannot be played here -> the first mapped one plays, the app is told ONCE (auto: true)
  const autoOnce = (w) => {
    open(w, { tracks: T_DTS, audioOrd: 0 }, C_DTS);
    for (let i = 0; i < 4; i++) w.send(tMsg({ tracks: T_DTS, audioOrd: 0, doneMs: 61000 + i * 1000 }));
    w.pumpTimers(10001);                                         // the hold is over, and the app STILL says 0 (no echo)
    for (let i = 0; i < 3; i++) w.send(tMsg({ tracks: T_DTS, audioOrd: 0, doneMs: 70000 + i * 1000 }));
    const s = sets(w);
    w.send(tMsg({ tracks: T_DTS, audioOrd: 1 }));               // the app's echo: the analysis follows what is heard
    return heard(w) === "2" && s.length === 1 && JSON.stringify(s[0]) === JSON.stringify({ type: "playerSet", audioOrd: 1, auto: true })
        && sets(w).length === 1;
  };
  // D5 + WHP-6: a choice is heard AT ONCE and said; the app hears of it once the viewer STOPPED choosing (800 ms), once
  const pickSends = (w) => {
    cc(open(w));
    w.outbox.length = 0;
    w.ids.ccAudio.children[0].click();
    const atOnce = heard(w) === "2" && sets(w).length === 0 && toast(w) === "Audio: English speech · AAC 2.0";
    w.pumpTimers(799);
    const notYet = sets(w).length === 0;
    w.pumpTimers(2);
    return atOnce && notYet && JSON.stringify(sets(w)) === JSON.stringify([{ type: "playerSet", audioOrd: 0 }]);
  };
  // A: the next PLAYABLE track (the unplayable one is skipped) -- the film with its French dub made undecodable
  const T_FILM_AC3 = (() => { const t = copy(T_FILM); t.audio[2].codec = "A_DTS"; t.audio[2].ch = 6; t.audio[2].play = false; return t; })();
  const C_FILM_AC3 = C_FILM.filter((a) => a.language !== "fre");
  // WHP-6: three presses of A within 0.3 s (on the way to the 4th track): heard each time, ONE playerSet -- the last
  const burstOne = (w) => {
    open(w, { tracks: T_FILM_AC3, audioOrd: 0 }, C_FILM_AC3);
    w.outbox.length = 0;
    const seen = [];
    for (let i = 0; i < 3; i++) { key(w, "a"); seen.push(heard(w)); w.pumpTimers(150); }
    const none = sets(w).length === 0;
    w.pumpTimers(SETTLE);
    return seen.join() === "3,5,6" && none && setOrds(w) === "4";
  };
  // WHP-6: a burst that comes BACK to the app's own track sends nothing, and holds nothing (the app's next word rules)
  const burstBack = (w) => {
    cc(open(w));                                                // the app: ord 1 (Chinese) heard
    w.outbox.length = 0;
    w.ids.ccAudio.children[0].click();                          // English...
    w.ids.ccAudio.children[1].click();                          // ...no: Chinese again, at once
    w.pumpTimers(SETTLE);
    const quiet = sets(w).length === 0 && heard(w) === "3";
    w.send(tMsg({ audioOrd: 0, jobId: 3 }));                    // the app changes its own choice: followed at once
    return quiet && heard(w) === "2";
  };
  // D5 + WHP-4: a translatorFile already on its way with the OLD audioOrd does not flip the sound back; the ANSWER to the
  // send (a new generation on that track) releases it to the app
  const holdsChoice = (w) => {
    cc(open(w));
    w.ids.ccAudio.children[0].click();
    w.pumpTimers(SETTLE);                                        // ...sent: playerSet{0}
    w.send(tMsg({ audioOrd: 1, doneMs: 62000 }));               // in flight before the app read it
    const held = heard(w) === "2";
    w.send(tMsg({ audioOrd: 0, jobId: 3 }));                    // the answer: a NEW generation on track 0 (D5)
    const echoed = heard(w) === "2";
    w.send(tMsg({ audioOrd: 1, jobId: 4 }));                    // the app's later choice rules again
    return held && echoed && heard(w) === "3" && setOrds(w) === "0";
  };
  // WHP-4 (R3): two choices sent, then a STALE post carrying the last one's ord: it answers nothing -- the app's late
  // answer to the FIRST choice must not flip the sound back (641f1aa round 1 heard 3,2,3,3,2,3)
  const staleNoRelease = (w) => {
    cc(open(w));
    const seq = [heard(w)];
    w.ids.ccAudio.children[0].click(); seq.push(heard(w)); w.pumpTimers(SETTLE);    // English, sent
    w.ids.ccAudio.children[1].click(); seq.push(heard(w)); w.pumpTimers(SETTLE);    // Chinese again, sent too (one unanswered)
    w.send(tMsg({ audioOrd: 1, doneMs: 61000 })); seq.push(heard(w));               // a post made before the app read either
    w.send(tMsg({ audioOrd: 0, jobId: 3 })); seq.push(heard(w));                    // the answer to playerSet{0}
    w.send(tMsg({ audioOrd: 1, jobId: 4 })); seq.push(heard(w));                    // ...then to playerSet{1}
    return seq.join() === "3,2,3,3,3,3" && setOrds(w) === "0,1";
  };
  // ...a hold the app never answers runs out (10 s after its last send): the subtitles must match what is heard
  const holdExpires = (w) => {
    cc(open(w));
    w.ids.ccAudio.children[0].click();
    w.pumpTimers(9000);
    w.send(tMsg({ audioOrd: 1, doneMs: 63000 }));
    const stillHeld = heard(w) === "2";                          // 9 s after the click, 8.2 s after the send: held
    w.pumpTimers(2000);
    w.send(tMsg({ audioOrd: 1, doneMs: 64000 }));
    return stillHeld && heard(w) === "3";
  };
  // the open panel is NOT rebuilt by a mere progress post (a rebuild under the pointer can swallow a click); it is when the
  // app's choice moves
  const panelStable = (w) => {
    cc(open(w));
    const b0 = w.ids.ccAudio.children[0];
    for (let i = 0; i < 3; i++) w.send(tMsg({ doneMs: 64000 + i * 1000 }));
    const kept = w.ids.ccAudio.children[0] === b0;
    w.send(tMsg({ audioOrd: 0, jobId: 3 }));
    return kept && w.ids.ccAudio.children[0] !== b0 && audioRows(w)[0] === "English speech (AAC 2.0)*";
  };
  // DEGRADE: nothing maps -- Chromium's list as it names it, a choice changes only the sound, the app hears nothing
  const T_ODD = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "Commentary", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "Main", lang: "fre", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const C_ODD = [A_(2, "Dub", "jpn"), A_(3, "Original", "kor")];
  const degradeSilent = (w) => {
    cc(open(w, { tracks: T_ODD, audioOrd: 0 }, C_ODD));
    const listed = audioRows(w).join("|") === "Dub (Japanese)*|Original (Korean)" && heard(w) === "2";
    w.ids.ccAudio.children[1].click();
    w.pumpTimers(SETTLE);                                        // (past the send delay: nothing may go out later either)
    return listed && heard(w) === "3" && sets(w).length === 0 && w.ids.ccAudioNote.hidden === false
        && w.ids.ccAudioNote.textContent === "Switching here changes only what you hear: the subtitles keep their sound track."
        && toast(w) === "Audio: Original · Korean";
  };
  // labels: an MP4's generic handler names name nothing -- the language does
  const genericHidden = (w) => {
    cc(open(w, { tracks: T_MP4, audioOrd: 0 }, C_MP4));
    return audioRows(w).join("|") === "English (AAC 2.0)*|Chinese (AAC 2.0)" && heard(w) === "2";
  };
  // no audioTracks (another runtime): no Audio section, nothing breaks, A says so
  const noApi = (w) => {
    cc(open(w));
    const none = w.ids.ccAudio.hidden === true && w.ids.ccAudioTitle.hidden === true && w.ids.ccPanel.getAttribute("aria-label") === "Subtitles";
    key(w, "a");
    return none && toast(w) === "Sound tracks can't be switched here" && sets(w).length === 0;
  };
  // WHP-2: the PROXY (FPROXY's copy of the FIRST sound track, D12): the section SHOWN, every row disabled, ord 0 heard, the
  // reason in one line; nothing written on the <audio>, nothing sent (the app forces ord 0 itself: the WH-P addendum)
  const proxyOpen = (w, o) => {
    w.send(tMsg(Object.assign({ name: "film.mkv", url: MKV_URL, tracks: T_2A, audioOrd: 0, proxyUrl: PROXY_URL, proxyState: "ready" }, o)));
    w.ids.fileAudio._setAudioTracks([A_(1, "", "")]);           // the proxy: a WAV, ONE track
    metaOk(w.ids.fileAudio);
    return w;
  };
  const proxyOneTrack = (w) => {
    cc(proxyOpen(w));
    return w.ids.ccAudio.hidden === false && audioRows(w).join("|") === "English speech (AAC 2.0)*!|Chinese speech (AAC 2.0)!"
        && w.ids.ccAudioNote.hidden === false && w.ids.ccAudioNote.textContent === PROXY_LINE
        && sets(w).length === 0 && w.ids.fileAudio.audioTracks.enabledLog.length === 0;
  };
  const proxyKeyA = (w) => { proxyOpen(w); w.outbox.length = 0; key(w, "a"); return toast(w) === PROXY_LINE && sets(w).length === 0; };
  // ...and the D11 line speaks of what the proxy plays (ord 0), never of the track the app named before forcing 0
  const T_JF = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "jpn", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "", lang: "fre", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const proxyLangNote = (w) => {
    pair(w, "en", "fr");
    proxyOpen(w, { tracks: T_JF, audioOrd: 1, langA: "en", langB: "fr" });
    return w.ids.fileLangNote.hidden === false
        && w.ids.fileLangNoteText.textContent === "This sound track is in Japanese — the pair is English ↔ French.";
  };
  // the panel's name: "Audio & subtitles" with 2+ tracks (and the CC button says so), "Subtitles" with one
  const titled = (w) => {
    cc(open(w));
    const two = w.ids.ccPanel.getAttribute("aria-label") === "Audio & subtitles" && w.ids.tCC.title === "Audio & subtitles (S, A)"
             && w.ids.ccAudioTitle.hidden === false && w.ids.ccAudioTitle.textContent === "Audio";
    const one = copy(T_2A); one.audio = one.audio.slice(0, 1);
    w.send(tMsg({ tracks: one, audioOrd: 0, seq: 2, itemId: 2, url: "https://f1.suvtitle-media.example/other.mkv", mediaKey: "mk2" }));
    v(w)._setAudioTracks(C_2A.slice(0, 1));
    metaOk(v(w));
    return two && w.ids.ccAudio.hidden === true && w.ids.ccPanel.getAttribute("aria-label") === "Subtitles" && w.ids.tCC.title === "Subtitles (S)";
  };
  // A: each press its own choice (a pause after each) -- told to the app once the viewer stops (WHP-6), round to the first
  const keyA = (w) => {
    open(w, { tracks: T_FILM_AC3, audioOrd: 0 }, C_FILM_AC3);
    const seen = [];
    for (let i = 0; i < 4; i++) { key(w, "a"); seen.push(heard(w)); w.pumpTimers(SETTLE); }
    return seen.join() === "3,5,6,2" && setOrds(w) === "1,3,4,0" && toast(w) === "Audio: Studio 5.1 Surround · English · AAC 5.1";
  };
  // D7: the film's subtitle tracks after the files next to it; image ones and unreadable ones listed DISABLED; twins say
  // which they are by their rank among the film's subtitles (WHP-12c: "track 7", "track 8" -- never the container's #13/#14)
  const filmSubs = (w) => {
    const t = copy(T_FILM); t.subs[4].ok = false;               // #11 cze: compressed in a way SuVtitle can't read
    cc(open(w, { tracks: t, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    return srcRows(w).slice(3).join("|") === "film.fr.srt (next to the file)|English (embedded)|French (embedded)|Finnish (embedded)"
      + "|Romanian (embedded)|Czech (can't be read)!|Korean (image subtitles, not supported)!"
      + "|Chinese (image subtitles, not supported · track 7)!|Chinese (image subtitles, not supported · track 8)!|Off";
  };
  const imageRefused = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    w.outbox.length = 0;
    const kor = w.ids.ccSources.children.find((b) => b.dataset.key === "x:76");
    if (kor) kor.click();
    return !!kor && kor.disabled === true && subGets(w).length === 0;
  };
  const unreadableRefused = (w) => {
    const t = copy(T_FILM); t.subs[4].ok = false;
    cc(open(w, { tracks: t, audioOrd: 0 }, C_FILM));
    w.outbox.length = 0;
    const cze = w.ids.ccSources.children.find((b) => b.dataset.key === "x:75");
    if (cze) cze.click();
    return !!cze && cze.disabled === true && subGets(w).length === 0;
  };
  // S never lands on a source that can't be chosen: from the last TEXT track, the next is Off
  const sSkips = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    w.ids.ccSources.children.find((b) => b.dataset.key === "x:75").click();
    w.send(embSubs({ id: 75, name: "Czech (track 11)" }));
    key(w, "s");
    return toast(w) === "Subtitles: Off";
  };
  // D7: chosen BY ID like a file; past 700 ms with no answer "Loading the subtitle track…" (not before), until the answer
  // -- whose toast says the PANEL's words for the track (WHP-8), not the app's raw name
  const loadingSaid = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    w.outbox.length = 0;
    w.ids.ccSources.children.find((b) => b.dataset.key === "x:72").click();
    const asked = JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 72 }]);
    w.pumpTimers(699);
    const before = toast(w) !== "Loading the subtitle track…";
    w.pumpTimers(2);
    const at = toast(w) === "Loading the subtitle track…";
    w.pumpTimers(5000);
    const stays = toast(w) === "Loading the subtitle track…";
    w.send(embSubs());
    return asked && before && at && stays && toast(w) === "Subtitles loaded: French (embedded)";
  };
  // ...the row says it too, while it lasts (WHP-10: the wait made VISIBLE, hence checkable) -- and stops saying it on the answer
  const loadingRowEnds = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    const during = srcRows(w).indexOf("French (embedded · loading…)") >= 0;
    w.send(embSubs());
    return during && srcRows(w).indexOf("French (embedded)*") >= 0 && !srcRows(w).some((r) => /loading…/.test(r));
  };
  // ...a failure (the app's notice, the existing path) ends the wait: its sentence is not covered by "Loading…"
  const failureEndsWait = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    w.ids.ccSources.children.find((b) => b.dataset.key === "x:72").click();
    w.pumpTimers(300);
    w.send({ type: "translatorFileNotice", text: FAIL_TRACK });
    w.pumpTimers(1000);
    return toast(w) === FAIL_TRACK;
  };
  // ...and drops the request: the panel no longer shows the failed track as chosen (back to the file it had: WHP-10 M10)
  const failureDropsRequest = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9"); w.send(sideSubs());                         // the file next to the film, shown
    pick(w, "x:72");                                            // the film's French track asked...
    const asked = onSrc(w) === "x:72";
    w.send({ type: "translatorFileNotice", text: FAIL_TRACK }); // ...and it fails
    return asked && onSrc(w) === "x:9";
  };
  // D7: an embedded track is READ-ONLY in the editor: said, every action off, nothing sent -- keys included
  const readOnly = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, phase: "off", translate: false }, C_FILM));
    w.ids.ccSources.children.find((b) => b.dataset.key === "x:72").click();
    w.send(embSubs());
    v(w)._seek(1.5);
    key(w, "e");
    w.outbox.length = 0;
    key(w, "s", { altKey: true });                              // split
    key(w, "Delete", { ctrlKey: true });                        // delete
    w.ids.edSplit.click();
    return w.ids.edPane.hidden === false && w.ids.edMsg.textContent === "Embedded subtitles are read-only"
        && w.ids.edSplit.disabled === true && w.ids.edDelete.disabled === true && w.ids.edNew.disabled === true
        && w.ids.edSrc.readOnly === true && w.ids.edSrc.disabled === false && w.ids.edSrc.value === "Bonjour"
        && w.ids.edSaveAs.hidden === true && w.ids.edState.textContent === "read-only" && countType(w, "subEdit") === 0;
  };
  // ...no Encoding row for it: the app extracts it as UTF-8
  const noEncRow = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.send(embSubs());
    return w.ids.ccEncRow.hidden === true;
  };
  // D10: translation OFF, no file next to it -> the track in the pair's TARGET (B = French) -- full before forced
  const tFr = (() => { const t = copy(T_2A); t.subs.push({ id: 44, track: 7, name: "", lang: "fra", codec: "S_TEXT/UTF8", kind: "text",
                                                                 def: false, forced: false, ok: true }); return t; })();
  const offOpen = (w, tracks, o) => w.send(tMsg(Object.assign({ phase: "off", translate: false, tracks, audioOrd: 1 }, o)));
  // (D10's choice is the DEFAULT rule's: auto -- CONTRACT ADDENDUM 2)
  const d10Target = (w) => { pair(w, "en", "fr"); offOpen(w, tFr); return JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 44, auto: true }]); };
  // ...no track in B: a FORCED text track
  const d10Forced = (w) => { pair(w, "en", "ja"); offOpen(w, T_2A); return subGets(w).map((m) => m.id).join() === "42"; };
  // ...never an image track (B = Korean: the VobSub track is not taken; nothing forced -> off)
  const d10NeverImage = (w) => { pair(w, "en", "ko"); offOpen(w, T_FILM); return subGets(w).length === 0; };
  // ...never a track for its FlagDefault alone
  const tDef = (() => { const t = copy(T_2A); t.subs = [Object.assign({}, t.subs[0], { def: true }), Object.assign({}, t.subs[2], { def: true })]; return t; })();
  const d10NotByDefault = (w) => { pair(w, "en", "fr"); offOpen(w, tDef); return subGets(w).length === 0; };
  // ...the tracks still being read when it opened: decided when they are -- unless the viewer chose meanwhile
  const d10Later = (w) => {
    pair(w, "en", "fr");
    offOpen(w, { state: "probing" }, { audioOrd: -1 });
    const none = subGets(w).length === 0;
    w.send(tMsg({ phase: "off", translate: false, tracks: tFr, audioOrd: 1 }));
    return none && subGets(w).map((m) => m.id).join() === "44";
  };
  const d10UserFirst = (w) => {
    pair(w, "en", "fr");
    offOpen(w, { state: "probing" }, { audioOrd: -1 });
    metaOk(v(w));
    cc(w);
    w.ids.ccSources.children.find((b) => b.dataset.key === "off").click();   // the viewer: Off
    w.send(tMsg({ phase: "off", translate: false, tracks: tFr, audioOrd: 1 }));
    return subGets(w).length === 0;
  };
  // ...never while translation is on -- also when it was turned on before the tracks were read
  const d10NotWhenOn = (w) => {
    pair(w, "en", "fr");
    offOpen(w, { state: "probing" }, { audioOrd: -1 });
    w.ids.fileTranslate.click();                               // the viewer presses Translate (U1: it was the switch)
    w.send(tMsg({ jobId: 3, phase: "loading", translate: true, tracks: tFr, audioOrd: 1 }));
    w.send(tMsg({ jobId: 3, phase: "loading", translate: true, tracks: tFr, audioOrd: 1, fresh: false }));
    return subGets(w).length === 0 && /^a:/.test(onSrc(w));   // (U1: and what is shown is the analysis being made)
  };
  // WHP-7 (R5): the forced fallback prefers the language that WILL be heard -- the app's audioOrd, not what Chromium plays
  // before the enforcement (the metadata came before the tracks); two forced tracks, nothing in B (Japanese)
  const tForced2 = (() => { const t = copy(T_2A);
    t.subs = [{ id: 61, track: 4, name: "", lang: "eng", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: true, ok: true },
              { id: 62, track: 5, name: "", lang: "chi", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: true, ok: true }];
    return t; })();
  const d10HeardIsApps = (w) => {
    pair(w, "en", "ja");
    w.send(tMsg({ phase: "off", translate: false, tracks: { state: "probing" }, audioOrd: -1 }));
    v(w)._setAudioTracks(C_2A); metaOk(v(w));                  // Chromium: English enabled
    w.send(tMsg({ phase: "off", translate: false, tracks: tForced2, audioOrd: 1 }));   // the app: Chinese
    return subGets(w).map((m) => m.id).join() === "62" && heard(w) === "3";
  };
  // WHP-12d: B = zh-tw -- an explicit Traditional tag first, then a legacy "chi" whose NAME says Traditional, then the rest
  const tZh = (subs) => { const t = copy(T_2A); t.subs = subs.map((s, i) => Object.assign({ id: 81 + i, track: 4 + i, codec: "S_TEXT/UTF8",
                                                                     kind: "text", def: false, forced: false, ok: true }, s)); return t; };
  const d10TradNamed = (w) => { pair(w, "en", "zh-tw"); offOpen(w, tZh([{ name: "Simplified", lang: "chi" }, { name: "Traditional Chinese", lang: "chi" }]));
                                return subGets(w).map((m) => m.id).join() === "82"; };
  const d10TradExact = (w) => { pair(w, "en", "zh-tw");
                                offOpen(w, tZh([{ name: "", lang: "chi" }, { name: "Traditional Chinese", lang: "chi" }, { name: "", lang: "zh-Hant" }]));
                                return subGets(w).map((m) => m.id).join() === "83"; };
  // D11: translation ON, the heard track's language outside the JOB's pair: one line in the strip
  const NOTE_JA = "This sound track is in Japanese — the pair is English ↔ French.";
  const langNote = (w) => {
    pair(w, "en", "fr");
    open(w, { tracks: T_FILM, audioOrd: 3, langA: "en", langB: "fr" }, C_FILM);
    const said = w.ids.fileLangNote.hidden === false && w.ids.fileLangNoteText.textContent === NOTE_JA;
    cc(w); w.ids.ccAudio.children[2].click();                  // French: in the pair
    const gone = w.ids.fileLangNote.hidden === true;
    return said && gone;
  };
  const langNoteOff = (w) => {                                 // translation OFF: no analysis, nothing to say
    pair(w, "en", "fr");
    open(w, { tracks: T_FILM, audioOrd: 3, langA: "en", langB: "fr", phase: "off", translate: false }, C_FILM);
    return w.ids.fileLangNote.hidden === true;
  };
  const chineseFamily = (w) => {                               // a "chi" track with an en <-> zh-tw pair: in the pair
    pair(w, "en", "zh-tw");
    open(w, { tracks: T_2A, audioOrd: 1, langA: "en", langB: "zh-tw" });
    return heard(w) === "3" && w.ids.fileLangNote.hidden === true;
  };
  // WHP-10 (M14): the JOB's pair, not the one running now (the viewer may have changed the pair since)
  const langNoteJobPair = (w) => {
    pair(w, "en", "ja");                                        // running now: English <-> Japanese
    open(w, { tracks: T_FILM, audioOrd: 3, langA: "en", langB: "fr" }, C_FILM);   // the job: English <-> French
    return w.ids.fileLangNote.hidden === false && w.ids.fileLangNoteText.textContent === NOTE_JA;
  };
  // WHP-12a: one way (a2b / b2a) -- anything but the SOURCE gets the line: French under an English -> French job is in the
  // pair and still the wrong track to analyse
  const langNoteOneWay = (w) => {
    pair(w, "en", "fr", "a2b");
    open(w, { tracks: T_FILM, audioOrd: 2, langA: "en", langB: "fr", mode: "a2b" }, C_FILM);
    return w.ids.fileLangNote.hidden === false
        && w.ids.fileLangNoteText.textContent === "This sound track is in French — the pair translates English → French.";
  };
  // D11 through the page: ISO 639-2/B and /T codes alike
  const isoRows = (w, langs) => {
    const t = { state: "ready", container: "mkv", video: vid, subs: [],
      audio: langs.map((l, i) => ({ ord: i, track: i + 2, name: "", lang: l, codec: "A_AAC", ch: 2, def: i === 0, play: true })) };
    cc(open(w, { tracks: t, audioOrd: 0 }, langs.map((l, i) => A_(i + 2, "", l))));
    return audioRows(w).map((r) => r.replace(/ \(AAC 2\.0\)\*?$/, "")).join("|");
  };
  const isoBT = (w) => isoRows(w, ["fre", "ger", "cze", "rum", "gre", "dut", "chi"]) === "French|German|Czech|Romanian|Greek|Dutch|Chinese";
  // WHP-12b: a SOUND track is named by the language spoken -- "Chinese" for a Traditional-tagged dub (its subtitle track keeps
  // its script); a region stays (Brazilian Portuguese is another dub)
  const T_SPEECH = { state: "ready", container: "mkv", video: vid,
    audio: [{ ord: 0, track: 2, name: "", lang: "zh-Hant", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "", lang: "pt-BR", codec: "A_AAC", ch: 2, def: false, play: true }],
    subs: [{ id: 91, track: 4, name: "", lang: "zh-Hant", codec: "S_TEXT/UTF8", kind: "text", def: false, forced: false, ok: true }] };
  const speechNames = (w) => {
    cc(open(w, { tracks: T_SPEECH, audioOrd: 0 }, [A_(2, "", "zh-Hant"), A_(3, "", "pt-BR")]));
    return audioRows(w).join("|") === "Chinese (AAC 2.0)*|Portuguese (Brazil) (AAC 2.0)"
        && srcRows(w).indexOf("Chinese (Traditional) (embedded)") >= 0;
  };
  // WHP-12c + WHP-10 (M13): ONE numbering -- two sound tracks that would read the same say "track n" = their rank (ord + 1),
  // like the "Track n" of a track with no language at all
  const T_TWINS = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 7, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 9, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true },
            { ord: 2, track: 11, name: "", lang: "", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const audioTwins = (w) => {
    cc(open(w, { tracks: T_TWINS, audioOrd: 0 }, [A_(2, "", "eng"), A_(3, "", "eng"), A_(4, "", "")]));
    return audioRows(w).join("|") === "English (AAC 2.0 · track 1)*|English (AAC 2.0 · track 2)|Track 3 (AAC 2.0)";
  };
  // D4 refinements: counts equal but CONTRADICTED by the languages -> the alignment; the tie to a codec every WebView
  // decodes; a play:false track only on positive evidence
  const T_WRONG = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_AC3", ch: 6, def: true, play: true },     // not decodable HERE
            { ord: 1, track: 3, name: "", lang: "fre", codec: "A_MPEG/L2", ch: 2, def: false, play: false },  // decodable after all
            { ord: 2, track: 4, name: "", lang: "jpn", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const contradicted = (w) => {
    cc(open(w, { tracks: T_WRONG, audioOrd: 2 }, [A_(2, "", "fre"), A_(3, "", "jpn")]));
    return audioRows(w).join("|") === "English (AC-3 5.1 · can't be played here)!|French (MP2 2.0)|Japanese (AAC 2.0)*" && heard(w) === "3";
  };
  const T_TIE = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_EAC3", ch: 6, def: true, play: true },
            { ord: 1, track: 3, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const tieToSure = (w) => {
    cc(open(w, { tracks: T_TIE, audioOrd: 1 }, [A_(2, "", "eng")]));
    return audioRows(w).join("|") === "English (E-AC-3 5.1 · can't be played here)!|English (AAC 2.0)*" && sets(w).length === 0;
  };
  const T_NOEV = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "", lang: "", codec: "A_MPEG/L2", ch: 2, def: true, play: false },
            { ord: 1, track: 3, name: "", lang: "", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const noEvidence = (w) => {
    cc(open(w, { tracks: T_NOEV, audioOrd: 1 }, [A_(2, "", ""), A_(3, "", "")]));
    return audioRows(w).join("|") === "Track 1 (MP2 2.0 · can't be played here)!|Track 2 (AAC 2.0)*" && heard(w) === "3";
  };
  // WHP-5: NAMES are evidence in Matroska / WebM only. An MP4's tray name may be udta/name while Chromium's label is the
  // hdlr name -- two fields: differing, they prove nothing, and the mapping holds (the app's saved pick, ord 1, is heard)
  const T_MP4N = { state: "ready", container: "mp4", video: { codec: "avc1", w: 640, h: 360 }, subs: [],
    audio: [{ ord: 0, track: 2, name: "English 5.1", lang: "eng", codec: "mp4a", ch: 6, def: true, play: true },
            { ord: 1, track: 3, name: "Director's commentary", lang: "eng", codec: "mp4a", ch: 2, def: false, play: true }] };
  const mp4NamesNoContradiction = (w) => {
    cc(open(w, { tracks: T_MP4N, audioOrd: 1 }, [A_(2, "Surround", "eng"), A_(3, "Stereo", "eng")]));
    return heard(w) === "3" && w.ids.ccAudioNote.hidden === true && audioRows(w)[1].endsWith("*") && /^Director's commentary/.test(audioRows(w)[1]);
  };
  // ...while in Matroska both sides read the same Name element: two different names ARE a contradiction (-> degrade)
  const T_MKVN = { state: "ready", container: "mkv", video: vid, subs: [],
    audio: [{ ord: 0, track: 2, name: "Commentary", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true },
            { ord: 1, track: 3, name: "Main", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true }] };
  const mkvNamesCount = (w) => {
    cc(open(w, { tracks: T_MKVN, audioOrd: 1 }, [A_(2, "Dub", "eng"), A_(3, "Original", "eng")]));
    return w.ids.ccAudioNote.hidden === false && audioRows(w).join("|") === "Dub (English)*|Original (English)" && heard(w) === "2";
  };
  // WHP-10 (M15 / M18), on a NON-default track first -- Chromium's own default (ord 0) would make them pass for nothing:
  // the app heard on ord 1, then -1 ("none known") and "" (not an integer): what plays is left alone
  const noneKnownKeeps = (w) => {
    open(w);                                                    // ord 1 heard ("3")
    const a = heard(w) === "3";
    w.send(tMsg({ audioOrd: -1, jobId: 3 }));
    const b = heard(w) === "3";
    w.send(tMsg({ audioOrd: "", jobId: 4 }));
    return a && b && heard(w) === "3";
  };

  // ---- WHP-1: the embedded extraction the viewer ABANDONS (the director's addendum: playerSubGet{id:0} = cancel) ----
  // translation on: French (embedded) asked, "Loading…", the viewer goes back to SuVtitle's subtitles: the app is told to
  // stop (playerSubGet{id:0}), and what it had already posted for that track -- its document, then a state naming it -- is
  // not shown, nor said
  const abandonCancels = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.pumpTimers(1500);
    pick(w, "a:both");
    const cancel = subGets(w).map((m) => m.id).join() === "72,0" && toast(w) === "";
    w.pumpTimers(3000);
    w.send(embSubs());                                          // late: the document...
    w.send(tMsg({ tracks: T_FILM, audioOrd: 0, doneMs: 64000, extSubId: 72 }));   // ...then the state naming it
    v(w)._seek(1.5);
    return cancel && onSrc(w) === "a:both" && w.ids.subOverlay.textContent !== "Bonjour" && toast(w) === "";
  };
  // ...also over a FILE the viewer goes back to (the late document must not replace the one shown: the page's own guard)
  const abandonForSidecar = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9"); w.send(sideSubs());
    pick(w, "x:72");
    pick(w, "x:9");                                             // back to the file next to the film, at once
    const cancel = subGets(w).filter((m) => m.id === 0).length === 1;
    w.send(embSubs());                                          // the abandoned track lands
    v(w)._seek(1.5);
    return cancel && onSrc(w) === "x:9" && w.ids.subOverlay.textContent === "Salut";
  };
  // ...and its FAILURE, posted before the app read the cancel, is not said over the new choice -- for 3 s, and only a
  // subtitle-TRACK failure (another notice is said; after 3 s, a track failure is somebody else's and is said)
  const abandonFailureQuiet = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.pumpTimers(1500);
    pick(w, "a:both");
    w.pumpTimers(1000);
    w.send({ type: "translatorFileNotice", text: FAIL_TRACK });
    const quiet = toast(w) === "";
    w.send({ type: "translatorFileNotice", text: "That is not a video, audio or subtitle file." });
    const other = toast(w) === "That is not a video, audio or subtitle file.";
    w.pumpTimers(6500);                                         // the toast goes, and the window with it
    w.send({ type: "translatorFileNotice", text: FAIL_TRACK });
    return quiet && other && toast(w) === FAIL_TRACK;
  };
  // ...another FILE with the extraction still running: cancelled with it
  const newItemCancels = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.outbox.length = 0;
    w.send(tMsg({ tracks: T_FILM, audioOrd: 0, seq: 2, itemId: 2, url: "https://f1.suvtitle-media.example/next.mkv", mediaKey: "mk2" }));
    return JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 0 }]);
  };
  // ...asked AGAIN after the abandon: welcome again (its answer shown)
  const reaskWelcome = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72"); w.pumpTimers(300);
    pick(w, "a:both");
    pick(w, "x:72");
    w.send(embSubs());
    v(w)._seek(1.5);
    return subGets(w).map((m) => m.id).join() === "72,0,72" && onSrc(w) === "x:72" && w.ids.subOverlay.textContent === "Bonjour";
  };
  // ...never for a file next to the film (read at once by the app: there is no extraction to stop)
  const sidecarNoCancel = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9");                                             // asked, not answered yet
    pick(w, "a:both");
    return subGets(w).map((m) => m.id).join() === "9";
  };
  // WHP-3: the track being extracted, picked again (an impatient viewer): asked ONCE (each ask restarts the extraction)
  const repickOnce = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72"); w.pumpTimers(2000);
    pick(w, "x:72"); w.pumpTimers(2000);
    pick(w, "x:72");
    return subGets(w).filter((m) => m.id === 72).length === 1;
  };
  // ...and by the file shown, read again in another code page (a new request: the app cancels the extraction on it)
  const encodingEndsWait = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9"); w.send(sideSubs());                         // the file next to the film, shown: its Encoding row is there
    pick(w, "x:72");
    w.pumpTimers(800);
    const shown = toast(w) === "Loading the subtitle track…";
    w.outbox.length = 0;
    w.ids.ccEnc.value = "1252"; w.ids.ccEnc.dispatch("change");
    return shown && toast(w) === "" && !srcRows(w).some((r) => /loading…/.test(r))
        && JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 0 }, { type: "playerSubGet", id: 9, enc: 1252 }]);
  };
  // ...and by a subtitle FILE taking the screen meanwhile (dropped / loaded: the app's loadExtSub cancels the extraction)
  const loadedFileAbandons = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.pumpTimers(800);
    w.outbox.length = 0;
    w.send(sideSubs({ id: 30, name: "dropped.fr.srt", cues: [{ i: 0, s: 1000, e: 3000, r: [["Dropped", 0]] }] }));
    w.send(tMsg({ tracks: T_FILM, audioOrd: 0, doneMs: 61000, extSubId: 30 }));   // the app names it the file's subtitles
    const said = toast(w) === "Subtitles loaded: dropped.fr.srt · UTF-8" && !srcRows(w).some((r) => /loading…/.test(r))
              && JSON.stringify(subGets(w)) === JSON.stringify([{ type: "playerSubGet", id: 0 }]);
    w.send(embSubs());                                          // the cancelled extraction's late result
    v(w)._seek(1.5);
    return said && w.ids.subOverlay.textContent === "Dropped" && onSrc(w) === "x:30";
  };
  // ...the wait is ended by another pick (Off): its toast goes, the row stops saying "loading…"
  const offEndsWait = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.pumpTimers(800);
    const shown = toast(w) === "Loading the subtitle track…";
    pick(w, "off");
    return shown && toast(w) === "" && !srcRows(w).some((r) => /loading…/.test(r));
  };
  // (code points, never escapes in this file: a tool on the way turned "\u202E" into the character itself once)
  const JUNK_CP = [[0x80, 0x9f], [0x61c, 0x61c], [0x200e, 0x200f], [0x2028, 0x2029], [0x202a, 0x202e], [0x2066, 0x2069], [0xfeff, 0xfeff]];
  // WHP-8: names from the FILE are shown clean and bounded: no control / bidi / format character (an unclosed RIGHT-TO-LEFT
  // OVERRIDE reversed the rest of its row), 120 characters at most
  const evil = String.fromCharCode(0x202e) + "hsilgne" + String.fromCharCode(0x2069, 0x2028, 0x85) + "x";   // RLO .. PDI LS NEL
  const bidiClean = (w) => {
    const T = copy(T_2A); T.audio[0].name = evil; T.audio[1].name = "y".repeat(8000);
    cc(open(w, { tracks: T, audioOrd: 1 }, [A_(2, evil, "eng"), A_(3, "y".repeat(8000), "chi")]));
    const r0 = w.ids.ccAudio.children[0].textContent;
    return r0 === "hsilgne x (English · AAC 2.0)" && !Array.from(r0).some((c) => JUNK_CP.some(([a, b]) => c.codePointAt(0) >= a && c.codePointAt(0) <= b));
  };
  const nameBounded = (w) => {
    const T = copy(T_2A); T.audio[1].name = "y".repeat(8000);
    cc(open(w, { tracks: T, audioOrd: 1 }, [A_(2, "English speech", "eng"), A_(3, "y".repeat(8000), "chi")]));
    const label = w.ids.ccAudio.children[1].children[0].textContent;
    return Array.from(label).length === 120 && label.endsWith("…");
  };
  // ...and the embedded track's toast says the panel's words for it -- not the app's raw name (4 KB, an RLO in it)
  const embToastWords = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    pick(w, "x:72");
    w.send(embSubs({ name: "Track 8 (fre)" + String.fromCharCode(0x202e) + "z".repeat(4000) }));
    return toast(w) === "Subtitles loaded: French (embedded)";
  };
  // WHP-9: A while the film's tracks are known but not yet playable (no metadata), or still being read: "not ready yet"
  const aNotReady = (w) => {
    w.send(tMsg({ tracks: T_FILM, audioOrd: 0 }));            // the metadata not loaded yet
    key(w, "a");
    const a = toast(w) === "Sound tracks are not ready yet.";
    w.send(tMsg({ tracks: { state: "probing" }, audioOrd: -1, seq: 2, itemId: 2, url: "https://f1.suvtitle-media.example/next.mkv", mediaKey: "mk2" }));
    key(w, "a");
    return a && toast(w) === "Sound tracks are not ready yet." && sets(w).length === 0;
  };
  // the page's spoken families (one speech for "in the pair?": zh = zh-tw = yue, nb = nn = no), read off a script
  const families = (w) => {
    const lib = libOf(w.script);
    return !!lib && lib.langFamily("zh-tw") === "zh" && lib.langFamily("yue") === "zh" && lib.langFamily("nb") === "no"
        && lib.langFamily("nn") === "no" && lib.langFamily("fr") === "fr";
  };
  // S-HOST 23/10 (a defect already at 641f1aa, reproduced on the real app by scratchpad\s-host\probe_extswitch.js --queued):
  // ANOTHER ITEM opened WITHOUT a page reload (its folder already mapped) -- the previous item's subtitle FILE leaves with
  // it: not on the picture, not in the list (its lines stayed among the new item's cues), not among the panel's sources
  const NEXT_ITEM = { jobId: 3, seq: 2, itemId: 2, tracks: T_FILM, audioOrd: 0, url: "https://f1.suvtitle-media.example/next.mkv", mediaKey: "mk2" };
  const NEXT_CUE = { type: "translatorFileCue", jobId: 3, idx: 0, s: 1000, e: 3000, lang: "en", dstLang: "zh", pass: false, src: "Next one", dst: "Next one zh" };
  const listText = (w) => w.ids.fileList.children.map((r) => r.textContent).join(" | ");
  const switchDropsFile = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9"); w.send(sideSubs());                         // the file next to the film: on the picture, in the list
    v(w)._seek(1.5);
    const before = w.ids.subOverlay.textContent === "Salut" && w.ids.fileList.children.length === 1 && /Salut/.test(listText(w));
    w.send(tMsg(NEXT_ITEM)); w.send(NEXT_CUE);                  // the app: a new session, no subtitle file named for it
    v(w)._setAudioTracks(C_FILM); metaOk(v(w)); v(w)._seek(1.5);
    return before && w.ids.fileList.children.length === 1 && !/Salut/.test(listText(w)) && /Next one/.test(listText(w))
        && !/Salut/.test(w.ids.subOverlay.textContent) && !srcRows(w).some((r) => /film\.fr\.srt/.test(r));
  };
  // ...unless the app names it again for the new item (its document re-sent): then it is this item's -- shown, listed, chosen
  const switchKeepsNamed = (w) => {
    cc(open(w, { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    pick(w, "x:9"); w.send(sideSubs());
    w.send(tMsg(Object.assign({}, NEXT_ITEM, { extSubId: 9, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] })));
    w.send(sideSubs());
    v(w)._setAudioTracks(C_FILM); metaOk(v(w)); v(w)._seek(1.5);
    return w.ids.subOverlay.textContent === "Salut" && w.ids.fileList.children.length === 1 && /Salut/.test(listText(w)) && onSrc(w) === "x:9";
  };

  // ---- the harness's own stub, first: it must behave like Chromium or every check below tests a fiction --------------
  section("WH-0 -- the harness: the fake <video>'s audioTracks behaves like Chromium's (F3, and the WHP-11 measurement)");
  {
    const w = runTranslatorWindow();
    const L = v(w).audioTracks;
    v(w)._setAudioTracks(C_2A);
    ok("Chromium's list: length + [i] + getTrackById, its own ids, the FIRST entry enabled (FlagDefault ignored)",
       L.length === 2 && L[1].label === "Chinese speech" && L.getTrackById("3") === L[1] && L.enabledIds().join() === "2");
    w.pumpTimers(0);
    let changes = 0, inside = 0, writing = false;
    L.addEventListener("change", () => { changes++; if (writing) inside++; });
    // wh_e2e\make_measure.js in WebView2 154 (11/10): six tasks of writes on a 2-track list, the events counted per task
    const tasks = [() => { L[0].enabled = true; L[1].enabled = false; }, () => { L[0].enabled = false; L[1].enabled = true; },
                   () => { L[0].enabled = true; L[1].enabled = false; }, () => { L[1].enabled = true; },
                   () => { L[1].enabled = false; L[1].enabled = true; L[1].enabled = false; }, () => { L[0].enabled = false; L[1].enabled = true; }];
    const counts = tasks.map((t) => { const c0 = changes; writing = true; t(); writing = false; w.pumpTimers(0); return changes - c0; });
    ok("`change`: ONE per write that flips a track, queued (never inside the setter); enabling one silently disables the other "
       + "-- the measured counts 0,2,1,1,3,1 (wh_e2e out_measure.jsonl)", counts.join() === "0,2,1,1,3,1" && inside === 0, counts);
    v(w).load();
    ok("a load drops the list (a new resource) -- and the element knows nothing of the new one: HAVE_NOTHING, no duration, no size",
       L.length === 0 && L[0] === undefined && v(w).readyState === 0 && Number.isNaN(v(w).duration) && v(w).videoWidth === 0);
    ok("opts.noAudioTracks = a runtime without the API", runTranslatorWindow({ noAudioTracks: true }).ids.fileVideo.audioTracks === undefined);
    const nx = runTranslatorWindow({ audioNonExclusive: true });
    nx.ids.fileVideo._setAudioTracks(C_2A);
    nx.ids.fileVideo.audioTracks[1].enabled = true;
    ok("opts.audioNonExclusive = the spec's letter: enabling one leaves the other enabled (the page must switch it off itself)",
       nx.ids.fileVideo.audioTracks.enabledIds().join() === "2,3");
  }

  section("WH-1 -- the CC panel's Audio section (D5): labels, hints, the playable ones, the panel's name");
  {
    const w = cc(open(runTranslatorWindow(), { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    ok("the user's film: the five sound tracks -- a NAME when it has one (with its language in the hint), else the language; "
       + "codec + channels quiet; the one heard on",
       audioRows(w).join("|") === "Studio 5.1 Surround (English · AAC 5.1)*|Dubbing-House Stereo (English · AAC 2.0)|French (AAC 2.0)"
       + "|Japanese (AAC 2.0)|Norwegian (AAC 2.0)", audioRows(w));
    ok("...the section is titled Audio, ABOVE the subtitle sources", w.ids.ccAudioTitle.hidden === false && w.ids.ccAudio.hidden === false
       && w.ids.ccPanel.children.indexOf(w.ids.ccAudio) < w.ids.ccPanel.children.indexOf(w.ids.ccSources));
    ok("the panel is \"Audio & subtitles\" with 2+ sound tracks, \"Subtitles\" with one (its name and the CC button's tooltip)", titled(runTranslatorWindow()));
    ok("MP4's generic handler names (\"SoundHandler\") name nothing: the language does", genericHidden(runTranslatorWindow()));
    ok("ISO 639-2/B codes name their languages (fre ger cze rum gre dut chi)", isoBT(runTranslatorWindow()));
    ok("...and the /T codes the same (fra deu ces ron ell nld zho)",
       isoRows(runTranslatorWindow(), ["fra", "deu", "ces", "ron", "ell", "nld", "zho"]) === "French|German|Czech|Romanian|Greek|Dutch|Chinese");
    ok("a track with no language and a generic name: \"Track n\"", noEvidence(runTranslatorWindow()));
    const one = runTranslatorWindow();
    const t1 = copy(T_2A); t1.audio = t1.audio.slice(0, 1);
    cc(open(one, { tracks: t1, audioOrd: 0 }, C_2A.slice(0, 1)));
    ok("ONE sound track: no Audio section", one.ids.ccAudio.hidden === true && one.ids.ccAudioTitle.hidden === true);
    const wav = runTranslatorWindow();
    wav.send(tMsg({ tracks: { state: "none" }, audioOrd: -1, name: "talk.wav", url: "https://f1.suvtitle-media.example/talk.wav" }));
    wav.ids.fileAudio._setAudioTracks(C_2A);
    metaOk(wav.ids.fileAudio);
    cc(wav);
    ok("a file that is no container (tracks.state none: wav, mp3...): no section, nothing touched",
       wav.ids.ccAudio.hidden === true && wav.ids.fileAudio.audioTracks.enabledLog.length === 0);
    const pr = runTranslatorWindow();
    cc(open(pr, { tracks: { state: "probing" }, audioOrd: -1 }));
    ok("while the app is still reading the container (probing): no section yet", pr.ids.ccAudio.hidden === true);
    ok("no audioTracks API (another runtime): no section, nothing breaks, A says it can't switch",
       noApi(runTranslatorWindow({ noAudioTracks: true })));
    const wo = cc(open(runTranslatorWindow()));
    wo.send(tMsg({ audioOrd: 0, jobId: 3 }));
    ok("the open panel follows a change of the heard track (the app's)", audioRows(wo).join("|") === "English speech (AAC 2.0)*|Chinese speech (AAC 2.0)", audioRows(wo));
    ok("...and is NOT rebuilt by a mere progress post (a rebuild under the pointer can swallow the click landing on it)",
       panelStable(runTranslatorWindow()));
  }

  section("WH-2 -- the mapping D4: the app's tracks onto Chromium's list (it hides what it cannot decode, renumbers the rest)");
  {
    ok("mkv_2a_def2: counts equal -> by order; the app's choice (ord 1, FlagDefault) is the one heard", enforcedOnMeta(runTranslatorWindow()));
    ok("mkv_dts_first: the DTS track listed DISABLED (\"can't be played here\"), the AAC one = Chromium's entry 0", dtsListed(runTranslatorWindow()));
    ok("counts equal but the LANGUAGES say otherwise (the app's codec table wrong both ways): aligned on them", contradicted(runTranslatorWindow()));
    ok("two English tracks, one entry: the AAC one (decoded by every WebView), not the E-AC-3 one", tieToSure(runTranslatorWindow()));
    ok("no language, no name: a track the app thinks unplayable is never mapped on no evidence", noEvidence(runTranslatorWindow()));
    const g = runTranslatorWindow();
    const T_STEAL = { state: "ready", container: "mkv", video: vid, subs: [],
      audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_AC3", ch: 6, def: true, play: true },
              { ord: 1, track: 3, name: "", lang: "fre", codec: "A_AAC", ch: 2, def: false, play: true },
              { ord: 2, track: 4, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: false, play: true }] };
    cc(open(g, { tracks: T_STEAL, audioOrd: 2 }, [A_(2, "", "fre"), A_(3, "", "eng")]));
    ok("the alignment is in ORDER: an early track never steals a later one's entry (eng AC-3 absent: fre -> 0, eng AAC -> 1)",
       audioRows(g).join("|") === "English (AC-3 5.1 · can't be played here)!|French (AAC 2.0)|English (AAC 2.0)*" && heard(g) === "3", audioRows(g));
    ok("nothing maps -> DEGRADE: Chromium's list as it names it, a choice changes only the sound (no playerSet, not even later), and the panel says so",
       degradeSilent(runTranslatorWindow()));
    const nd = runTranslatorWindow();
    const T_2DTS = { state: "ready", container: "mkv", video: vid, subs: [],
      audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_DTS", ch: 6, def: true, play: false },
              { ord: 1, track: 3, name: "", lang: "fre", codec: "A_TRUEHD", ch: 8, def: false, play: false }] };
    cc(open(nd, { tracks: T_2DTS, audioOrd: 0 }, []));
    ok("Chromium lists NOTHING (only DTS/TrueHD): the app's tracks, each \"can't be played here\", no auto sent",
       audioRows(nd).join("|") === "English (DTS 5.1 · can't be played here)!|French (TrueHD 7.1 · can't be played here)!" && sets(nd).length === 0, audioRows(nd));
    ok("WHP-5: NAMES are evidence in Matroska only -- an MP4's udta name vs Chromium's hdlr label (two fields) contradict nothing: "
       + "the app's ord 1 is heard, no degrade", mp4NamesNoContradiction(runTranslatorWindow()));
    ok("...in Matroska both sides read the same Name: two different names ARE a contradiction (nothing maps: degrade)",
       mkvNamesCount(runTranslatorWindow()));
  }

  section("WH-3 -- the enforcement D3: the app chooses, the page makes it heard");
  {
    ok("at the metadata: exactly ONE entry enabled -- on Chromium (which disables the rest itself)", exactlyOne(runTranslatorWindow()));
    ok("...and on a runtime that would keep several (the spec's letter): the page switches the others off", exactlyOne(runTranslatorWindow({ audioNonExclusive: true })));
    ok("BEFORE anything plays (a file opened to play by itself): the first sound heard is the app's track", beforePlay(runTranslatorWindow()));
    ok("the tracks read AFTER the metadata (probing): enforced on that translatorFile; untouched before", readyLater(runTranslatorWindow()));
    ok("a translatorFile a second costs nothing: only what differs is written (Chromium: the one enabled; the spec's letter: + the other off)",
       idempotent(runTranslatorWindow()) && idempotentOn(runTranslatorWindow({ audioNonExclusive: true }), 2));
    const w = open(runTranslatorWindow());
    w.send(tMsg({ audioOrd: 0, jobId: 3 }));
    ok("the app changes its choice (another generation): heard at once", heard(w) === "2");
    ok("audioOrd -1 (none known) or not an integer (\"\"): what plays is left alone -- checked on the app's NON-default track (ord 1), "
       + "where taking it for ord 0 would show (WHP-10)", noneKnownKeeps(runTranslatorWindow()));
    ok("the app's choice can't be played here: the first playable one plays and the app is told ONCE -- {audioOrd, auto:true} -- "
       + "not once per translatorFile", autoOnce(runTranslatorWindow()));
    const n = open(runTranslatorWindow(), { tracks: T_FILM, audioOrd: 3 }, C_FILM);
    ok("...the user's film, Japanese chosen by the app: entry 3 heard, nothing sent", heard(n) === "5" && sets(n).length === 0);
    n.send(tMsg({ tracks: T_FILM, audioOrd: 1, seq: 2, itemId: 2, url: "https://f1.suvtitle-media.example/film2.mkv", mediaKey: "mk2" }));
    const dropped = n.ids.fileVideo.audioTracks.length === 0;
    n.ids.fileVideo._setAudioTracks(C_FILM);
    metaOk(n.ids.fileVideo);
    ok("another FILE: its list dropped at the load, then its OWN choice (no carry-over of the previous one's)", dropped && heard(n) === "3");
  }

  section("WH-4 -- the viewer's choice D5: the panel or A; heard at once, then the app (WHP-4/6: one send per burst, the hold)");
  {
    ok("a click: heard at once and said (\"Audio: English speech · AAC 2.0\"); playerSet{audioOrd} (no auto) once the viewer has "
       + "stopped choosing -- 800 ms, not before", pickSends(runTranslatorWindow()));
    ok("three presses of A in 0.3 s: each heard at once, ONE playerSet -- the last (each one is a re-track of the analysis)", burstOne(runTranslatorWindow()));
    ok("a burst that comes back to the app's own track sends NOTHING and holds nothing (the app's next choice is heard at once)", burstBack(runTranslatorWindow()));
    ok("an in-flight translatorFile with the OLD audioOrd does not flip it back; the ANSWER (a new generation on that track) releases it",
       holdsChoice(runTranslatorWindow()));
    ok("WHP-4 (R3): two choices sent, a STALE post with the last one's ord answers nothing -- the late answer to the first choice "
       + "does not flip the sound (heard 3,2,3,3,3,3)", staleNoRelease(runTranslatorWindow()));
    ok("...an app that never answers is followed again once the hold runs out (10 s after the send: the subtitles must match the sound)",
       holdExpires(runTranslatorWindow()));
    const s = cc(open(runTranslatorWindow()));
    s.outbox.length = 0;
    s.ids.ccAudio.children[1].click();
    s.pumpTimers(SETTLE);
    ok("clicking the track already heard sends nothing", sets(s).length === 0 && heard(s) === "3");
    const d = cc(open(runTranslatorWindow(), { tracks: T_DTS, audioOrd: 1 }, C_DTS));
    d.outbox.length = 0;
    d.ids.ccAudio.children[0].click();
    d.pumpTimers(SETTLE);
    ok("a disabled row (DTS) does nothing", sets(d).length === 0 && heard(d) === "2");
    ok("A: the next PLAYABLE track (the DTS one skipped), each choice told to the app, round to the first again", keyA(runTranslatorWindow()));
    const one = runTranslatorWindow();
    const t1 = copy(T_2A); t1.audio = t1.audio.slice(0, 1);
    open(one, { tracks: t1, audioOrd: 0 }, C_2A.slice(0, 1));
    key(one, "a");
    ok("A with one sound track: said, nothing sent", toast(one) === "No other sound track to switch to" && sets(one).length === 0);
    ok("WHP-9: A before the film can switch (the metadata not in yet, or the tracks still being read): \"Sound tracks are not ready yet.\"",
       aNotReady(runTranslatorWindow()));
    const f = open(runTranslatorWindow());
    f.outbox.length = 0;
    f.win.dispatch("keydown", { key: "a", target: f.ids.ccFont });
    f.win.dispatch("keydown", { key: "a", ctrlKey: true, target: f.doc.body });
    f.pumpTimers(SETTLE);
    ok("A typed in a field, or Ctrl+A, is not the player's", sets(f).length === 0 && heard(f) === "3");
    const aLines = pageScript.split("\n").filter((ln) => /(?:\bk|\bkey|\bl|e\.key)\s*===\s*"[aA]"|e\.code\s*===\s*"KeyA"|toLowerCase\(\)\s*===\s*"a"/.test(ln));
    ok("A was FREE: ONE line of the page answers a/A (any modifier, any handler, by key, code or lower case), and it is cycleAudio's",
       aLines.length === 1 && /cycleAudio\(\)/.test(aLines[0]), aLines);
    ok("the help lists A", /<kbd>A<\/kbd><span>Next sound track/.test(html));
  }

  section("WH-5 -- the film's own subtitles D7: listed with the files next to it, extracted on demand, read-only");
  {
    ok("the user's film: text tracks choosable (\"French (embedded)\"), the image ones (VobSub) and the unreadable one listed DISABLED "
       + "with the reason; twins say their rank (track 7, track 8 -- WHP-12c)", filmSubs(runTranslatorWindow()));
    ok("a click on an IMAGE track sends nothing", imageRefused(runTranslatorWindow()));
    ok("a click on an UNREADABLE track sends nothing", unreadableRefused(runTranslatorWindow()));
    ok("chosen like a file BY ID (playerSubGet{id}); \"Loading the subtitle track…\" past 700 ms, not before, until the answer -- "
       + "\"Subtitles loaded: French (embedded)\", the panel's words (WHP-8)", loadingSaid(runTranslatorWindow()));
    ok("...the row says \"loading…\" while it lasts, and stops saying it with the answer (WHP-10: the wait made visible)", loadingRowEnds(runTranslatorWindow()));
    ok("...the app's failure sentence (the existing notice path) ends the wait and stays readable", failureEndsWait(runTranslatorWindow()));
    ok("...and drops the request: the panel shows again what was chosen before (WHP-10)", failureDropsRequest(runTranslatorWindow()));
    const qa = cc(open(runTranslatorWindow(), { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    qa.ids.ccSources.children.find((b) => b.dataset.key === "x:72").click();
    qa.pumpTimers(300);
    qa.send(embSubs());
    qa.pumpTimers(1000);
    ok("...answered within 700 ms (a cache hit, a short film): the wait is never said", toast(qa) === "Subtitles loaded: French (embedded)");
    const w = cc(open(runTranslatorWindow(), { tracks: T_FILM, audioOrd: 0 }, C_FILM));
    w.ids.ccSources.children.find((b) => b.dataset.key === "x:72").click();
    w.send(embSubs());
    v(w)._seek(1.5);
    ok("the answer (playerSubs embedded:true): shown on the picture and in the list, chosen in the panel",
       w.ids.subOverlay.children.length === 1 && w.ids.subOverlay.textContent === "Bonjour" && rows(w).length === 2
       && srcRows(w).indexOf("French (embedded)*") >= 0, srcRows(w));
    ok("...no Encoding row: the app extracts it as UTF-8", noEncRow(runTranslatorWindow()));
    ok("S skips what can't be chosen (from the last text track: Off, not the image ones)", sSkips(runTranslatorWindow()));
    ok("the editor: \"Embedded subtitles are read-only\" -- fields READ-only (copyable), every action off, nothing sent (keys included)",
       readOnly(runTranslatorWindow()));
    const sc = cc(open(runTranslatorWindow(), { tracks: T_FILM, audioOrd: 0, sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }, C_FILM));
    sc.ids.ccSources.children.find((b) => b.dataset.key === "x:9").click();
    sc.pumpTimers(5000);
    ok("a file NEXT to the film is not \"extracted\": no Loading toast for it", toast(sc) === "");
  }

  section("WH-6 -- the default D10 (translation off): the files next to it, then the film's own text tracks");
  {
    ok("no file next to it: the TEXT track in the pair's target language (B = French; the full one, not the forced one)", d10Target(runTranslatorWindow()));
    ok("...none in B: a FORCED text track", d10Forced(runTranslatorWindow()));
    ok("...two forced ones: the one in the language that WILL be heard -- the app's audioOrd, not Chromium's first (WHP-7, R5)",
       d10HeardIsApps(runTranslatorWindow()));
    ok("...never an IMAGE track, even in B (Korean VobSub): off", d10NeverImage(runTranslatorWindow()));
    ok("...never a track for its FlagDefault alone", d10NotByDefault(runTranslatorWindow()));
    ok("...the tracks still being read at the opening: decided when they are", d10Later(runTranslatorWindow()));
    ok("...unless the viewer chose meanwhile (Off stays Off)", d10UserFirst(runTranslatorWindow()));
    ok("...never while translation is on (turned on before the tracks were read too)", d10NotWhenOn(runTranslatorWindow()));
    ok("WHP-12d: B = 繁體中文 and only legacy \"chi\" tracks: the one whose NAME says Traditional, not the first", d10TradNamed(runTranslatorWindow()));
    ok("...an EXPLICIT Traditional tag (zh-Hant) before both -- the exact code before the family (WHP-10, M12)", d10TradExact(runTranslatorWindow()));
    const s = runTranslatorWindow();
    pair(s, "en", "fr");
    offOpen(s, tFr, { sidecars: [{ id: 9, name: "film.de.srt", lang: "de" }] });
    s.pumpTimers(700);                                         // (nothing pending: asked at once, auto -- ADDENDUM 2)
    ok("...a file next to the film still comes first (the existing best-sidecar rule)", subGets(s).map((m) => m.id).join() === "9");
    const on = runTranslatorWindow();
    pair(on, "en", "fr");
    on.send(tMsg({ tracks: tFr, audioOrd: 1 }));
    ok("translation ON: SuVtitle's own subtitles, nothing asked", subGets(on).length === 0);
  }

  section("WH-7 -- languages D11: the containers' codes named, the heard track against the pair");
  {
    const lib = libOf(pageScript);
    const L = (s) => { const x = lib.langInfo(s); return x.code + "=" + x.name; };
    const vec = [["eng", "en=English"], ["fre", "fr=French"], ["fra", "fr=French"], ["ger", "de=German"], ["deu", "de=German"],
      ["chi", "zh=Chinese"], ["zho", "zh=Chinese"], ["jpn", "ja=Japanese"], ["kor", "ko=Korean"], ["spa", "es=Spanish"],
      ["ita", "it=Italian"], ["por", "pt=Portuguese"], ["rus", "ru=Russian"], ["ara", "ar=Arabic"], ["nld", "nl=Dutch"],
      ["dut", "nl=Dutch"], ["pol", "pl=Polish"], ["tur", "tr=Turkish"], ["swe", "sv=Swedish"], ["nor", "no=Norwegian"],
      ["nob", "nb=Norwegian Bokmål"], ["dan", "da=Danish"], ["fin", "fi=Finnish"], ["ces", "cs=Czech"], ["cze", "cs=Czech"],
      ["ron", "ro=Romanian"], ["rum", "ro=Romanian"], ["hun", "hu=Hungarian"], ["ell", "el=Greek"], ["gre", "el=Greek"],
      ["heb", "he=Hebrew"], ["hin", "hi=Hindi"], ["tha", "th=Thai"], ["vie", "vi=Vietnamese"], ["ind", "id=Indonesian"],
      ["ukr", "uk=Ukrainian"], ["EN", "en=English"], ["en-US", "en=English"], ["pt-BR", "pt=Portuguese (Brazil)"],
      ["es-419", "es=Spanish (Latin America)"], ["fr-CA", "fr=French (Canada)"], ["zh-Hant", "zh-tw=Chinese (Traditional)"],
      ["zh-TW", "zh-tw=Chinese (Traditional)"], ["zh-Hant-HK", "zh-tw=Chinese (Traditional)"], ["zh-Hans", "zh=Chinese (Simplified)"],
      ["zh-CN", "zh=Chinese (Simplified)"], ["yue", "yue=Cantonese"], ["zh-yue", "yue=Cantonese"], ["cmn", "zh=Chinese"],
      ["sr-Latn-RS", "sr=Serbian"], ["iw", "he=Hebrew"], ["und", "="], ["", "="], ["mis", "="], ["qaa", "="], ["xx", "="],
      ["zxx", "=No dialogue"], ["mul", "=Several languages"], ["en_GB", "en=English"]];
    const miss = lib ? vec.filter(([s, want]) => L(s) !== want).map(([s, want]) => s + " -> " + L(s) + " (want " + want + ")") : ["the functions could not be lifted"];
    ok("ISO 639-1, 639-2/B and /T, BCP-47 (Chinese script/region, a few regions) -> the translator's code + an English name; "
       + "und/mis/qaa/garbage -> nothing (" + vec.length + " vectors)", miss.length === 0, miss);
    ok("...the doors' alias table is NOT where this lives (the three-table parity stands): LANG_ALIAS has no ISO 639-2/B row",
       !/"fre":|"ger":|"jpn":/.test(lift(pageScript, "  const LANG_ALIAS = {", "  const normLang")));
    ok("one spoken family for \"in the pair?\": zh = zh-tw = yue, nb = nn = no", families({ script: pageScript }));
    ok("the pair in an English sentence: English, French, Chinese (Simplified), Chinese (Traditional)",
       !!lib && ["en", "fr", "zh", "zh-tw"].map(lib.pairLangEn).join("|") === "English|French|Chinese (Simplified)|Chinese (Traditional)");
    const codecs = [["A_AAC/MPEG4/LC", "AAC"], ["mp4a", "AAC"], ["A_EAC3", "E-AC-3"], ["ec-3", "E-AC-3"], ["A_AC3", "AC-3"],
      ["ac-3", "AC-3"], ["A_DTS/EXPRESS", "DTS"], ["dtsc", "DTS"], ["A_TRUEHD", "TrueHD"], ["mlpa", "TrueHD"], ["A_FLAC", "FLAC"],
      ["fLaC", "FLAC"], ["A_OPUS", "Opus"], ["Opus", "Opus"], ["A_VORBIS", "Vorbis"], ["A_MPEG/L3", "MP3"], ["A_MPEG/L2", "MP2"],
      ["A_PCM/INT/LIT", "PCM"], ["lpcm", "PCM"], ["sowt", "PCM"], ["A_ALAC", "ALAC"], ["alac", "ALAC"], ["A_MS/ACM", "A_MS/ACM"], ["", ""]];
    const cmiss = lib ? codecs.filter(([s, want]) => lib.codecName(s) !== want).map(([s, want]) => s + " -> " + lib.codecName(s)) : ["no lib"];
    ok("codecs in a player's words, whether the app sends Matroska ids, MP4 fourccs or names (" + codecs.length + " vectors)", cmiss.length === 0, cmiss);
    ok("channels: mono, 2.0, 5.1, 7.1 (0 = unknown, said nothing)",
       !!lib && [1, 2, 6, 8, 0, 12].map(lib.chName).join("|") === "mono|2.0|5.1|7.1||12 ch");
    const generic = ["SoundHandler", "Sound Media Handler", "Core Media Audio", "GPAC ISO Audio Handler", "L-SMASH Audio Handler",
                     "Bento4 Sound Handler", "Mainconcept MP4 Sound Media Handler", "Apple Sound Media Handler", "VideoHandler",
                     "SubtitleHandler", "ISO Media file produced by Google Inc.", "Lavf58.76.100", "Track 2", "Audio", "  "];
    const real = ["Studio 5.1 Surround", "Dubbing-House Stereo", "Commentary", "French ASS", "Stereo", "VFF", "Director's commentary"];
    ok("generic names (MP4 handlers, encoder tags, \"Track 2\") name nothing; real names stay",
       !!lib && generic.every((s) => lib.trackName(s) === "") && real.every((s) => lib.trackName(s) === s),
       lib && generic.filter((s) => lib.trackName(s) !== "").concat(real.filter((s) => lib.trackName(s) !== s)));
    ok("translation ON, the heard track (Japanese) outside the pair: \"" + NOTE_JA + "\" -- gone once French is chosen", langNote(runTranslatorWindow()));
    ok("...against the JOB's pair, not the one running now (the viewer moved it since: WHP-10)", langNoteJobPair(runTranslatorWindow()));
    ok("...one way (English → French): French heard is in the pair and still not the source -- \"…the pair translates English → French.\" (WHP-12a)",
       langNoteOneWay(runTranslatorWindow()));
    ok("...translation OFF: no line (no analysis to warn about)", langNoteOff(runTranslatorWindow()));
    ok("...a Chinese track with an English <-> 繁體中文 pair: in the pair (one spoken language), no line", chineseFamily(runTranslatorWindow()));
    const u = runTranslatorWindow();
    pair(u, "en", "fr");
    const tUnd = copy(T_MP4); tUnd.audio.forEach((t) => { t.lang = "und"; });
    open(u, { tracks: tUnd, audioOrd: 1, langA: "en", langB: "fr" }, [A_(2, "SoundHandler", "und"), A_(3, "SoundHandler", "und")]);
    ok("...an unknown language (und) says nothing", heard(u) === "3" && u.ids.fileLangNote.hidden === true);
    const na = runTranslatorWindow({ noAudioTracks: true });
    pair(na, "en", "fr");
    open(na, { tracks: T_FILM, audioOrd: 3, langA: "en", langB: "fr" }, C_FILM);
    ok("...without the audioTracks API: the line speaks of the track the app analyses", na.ids.fileLangNoteText.textContent === NOTE_JA);
    ok("WHP-12b: a SOUND track is named by the language spoken (\"Chinese\" for a Traditional-tagged dub; its subtitle track keeps "
       + "\"Chinese (Traditional)\"); a region stays (\"Portuguese (Brazil)\")", speechNames(runTranslatorWindow()));
    ok("WHP-12c: ONE numbering -- twins say \"track n\" = their rank (ord + 1), like a nameless track's \"Track n\"", audioTwins(runTranslatorWindow()));
  }

  section("WH-8 -- the WH-P FIX ROUND (REVIEW_WH_P.md): the abandoned extraction (WHP-1), the proxy (WHP-2), the re-pick (WHP-3), names (WHP-8)");
  {
    ok("WHP-1: the viewer leaves an embedded track still being extracted (back to SuVtitle's): the app is told to stop -- "
       + "playerSubGet{id:0} -- the wait's toast goes, and the late document + the state naming it are NOT shown", abandonCancels(runTranslatorWindow()));
    ok("...nor do they replace the subtitle FILE he went back to", abandonForSidecar(runTranslatorWindow()));
    ok("...its failure, posted before the app read the cancel, is not said over his choice -- for 3 s, and only a subtitle-TRACK "
       + "failure (any other notice is said; after 3 s a track failure is said)", abandonFailureQuiet(runTranslatorWindow()));
    ok("...another FILE while it is extracted: cancelled with it", newItemCancels(runTranslatorWindow()));
    ok("...asked again after the abandon: welcome again (shown)", reaskWelcome(runTranslatorWindow()));
    ok("...never for a file next to the film (nothing is extracted: nothing to stop)", sidecarNoCancel(runTranslatorWindow()));
    ok("...the wait ends with another pick (Off): the toast and the row's \"loading…\" go", offEndsWait(runTranslatorWindow()));
    ok("...and with a subtitle FILE that takes the screen meanwhile (dropped, loaded): the extraction abandoned, its late result ignored (S-WEB 23/10)",
       loadedFileAbandons(runTranslatorWindow()));
    ok("...and with the file shown read again in another code page (the app cancels the extraction on a new request: told, the wait ends -- S-WEB 23/10)",
       encodingEndsWait(runTranslatorWindow()));
    // the parity of the failure filter: the app's own sentences for a subtitle TRACK it could not read (MediaTracks.h err*)
    const mt = P.cppSource("ui/MediaTracks.h");
    const sentences = [...mt.text.matchAll(/inline const char\* err\w+\(\)\s*\{\s*return "([^"]+)";/g)].map((m) => m[1]);
    const unknownKind = (/return "(SuVtitle can't read this kind of subtitle track \()" \+/.exec(mt.text) || [])[1];
    const failRe = (() => { const m = /const SUB_TRACK_FAIL = (\/.*\/[a-z]*);/.exec(pageScript); try { return m ? new Function("return " + m[1])() : null; } catch (e) { return null; } })();
    const all = sentences.concat(unknownKind ? [unknownKind + "S_HDMV/TEXTST)."] : []);
    ok("the page knows the app's failure sentences for a subtitle TRACK: SUB_TRACK_FAIL matches each of MediaTracks.h's ("
       + all.length + ", " + mt.from + ")", !!failRe && all.length >= 8 && all.every((s) => failRe.test(s)), all.filter((s) => !failRe || !failRe.test(s)));
    const others = ["That is not a video, audio or subtitle file.", "5 items were not a video, audio or subtitle file (or not found) and were left out.",
                    "This format can't be played here.", "A subtitle file goes with a video: open the video first.",
                    "Subtitle files need the app: the design mock cannot read them.", "This sound track (DTS) can't be decoded for the analysis. Choose another sound track."];
    ok("...and nothing else the app says (a dropped file, a format, a sound track)", !!failRe && others.every((s) => !failRe.test(s)),
       others.filter((s) => failRe && failRe.test(s)));
    ok("WHP-2: the PROXY (one sound track: the first): the Audio section SHOWN, every row disabled, ord 0 heard, the line \"" + PROXY_LINE
       + "\"; nothing written, nothing sent", proxyOneTrack(runTranslatorWindow()));
    ok("...A says the same", proxyKeyA(runTranslatorWindow()));
    ok("...the D11 line speaks of what the proxy plays (ord 0: Japanese), not of the track named before (French)", proxyLangNote(runTranslatorWindow()));
    ok("WHP-3: the row being extracted, clicked again (and again): asked ONCE -- each ask would restart the extraction", repickOnce(runTranslatorWindow()));
    ok("WHP-8: a name from the FILE loses its control / bidi / format characters (an unclosed RLO reversed its row)", bidiClean(runTranslatorWindow()));
    ok("...and is bounded: 120 characters, the last an ellipsis (a Matroska Name may be 4 KB)", nameBounded(runTranslatorWindow()));
    ok("...the embedded track's toast says the panel's words, not the app's raw name", embToastWords(runTranslatorWindow()));
    const noJunk = /const NAME_JUNK = \/\[\\u0000-\\u001f\\u007f-\\u009f\\u061c\\u200e\\u200f\\u2028\\u2029\\u202a-\\u202e\\u2066-\\u2069\\ufeff\]\/g;/.test(pageScript);
    ok("...the characters taken out: C0, DEL, C1, ALM, LRM/RLM, LS/PS, LRE..RLO, LRI..PDI, BOM (NAME_JUNK)", noJunk);
    ok("S-HOST 23/10: another item opened without a page reload (its folder already mapped): the previous item's subtitle FILE "
       + "leaves with it -- off the picture, out of the list (its lines stayed among the new cues: a defect already at 641f1aa), out of the panel",
       switchDropsFile(runTranslatorWindow()));
    ok("...unless the app names it again for the new item (its document re-sent): shown, listed, chosen", switchKeepsNamed(runTranslatorWindow()));
  }

  // ---- mutation sentinels -----------------------------------------------------------------------------------------
  section("WH -- mutation sentinels (index.html with one WH rule removed each)");
  {
    const judge = (name, mutate, probe, opts) => {
      if (mutate(html) === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow(Object.assign({ mutate, name }, opts || {})); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true, threw = "";
      try { healthy = !!probe(w); } catch (e) { threw = String((e && e.message) || e); }
      if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    const NX = { audioNonExclusive: true };
    const probes = { enforcedOnMeta, exactlyOne, idempotent, beforePlay, readyLater, dtsListed, autoOnce, pickSends, burstOne, burstBack,
                     holdsChoice, staleNoRelease, holdExpires, degradeSilent, genericHidden, proxyOneTrack, proxyKeyA, proxyLangNote, titled,
                     keyA, filmSubs, imageRefused, unreadableRefused, sSkips, loadingSaid, loadingRowEnds, failureEndsWait,
                     failureDropsRequest, readOnly, noEncRow, d10Target, d10Forced, d10HeardIsApps, d10NeverImage, d10NotByDefault,
                     d10Later, d10UserFirst, d10NotWhenOn, d10TradNamed, d10TradExact, langNote, langNoteJobPair, langNoteOneWay,
                     langNoteOff, chineseFamily, isoBT, speechNames, audioTwins, contradicted, tieToSure, noEvidence,
                     mp4NamesNoContradiction, mkvNamesCount, noneKnownKeeps, panelStable, abandonCancels, abandonForSidecar,
                     abandonFailureQuiet, newItemCancels, reaskWelcome, sidecarNoCancel, repickOnce, offEndsWait, bidiClean,
                     nameBounded, embToastWords, aNotReady, families, encodingEndsWait, loadedFileAbandons, switchDropsFile,
                     switchKeepsNamed };
    const sick = Object.keys(probes).filter((k) => { try { return !probes[k](runTranslatorWindow()); } catch (e) { return true; } });
    ok("(WH's probes pass on the real page: " + Object.keys(probes).length + ")", sick.length === 0, sick);
    ok("(...and the no-API probe on a runtime without it, exactlyOne and idempotent on a non-exclusive one)",
       noApi(runTranslatorWindow({ noAudioTracks: true })) && exactlyOne(runTranslatorWindow(NX)) && idempotentOn(runTranslatorWindow(NX), 2));
    // (the harness reads the page as LF whatever the checkout -- paths.js readPage, RV4-9 -- so the patterns are LF; R()
    //  also maps a pattern written with CRLF to LF)
    const R = (a, b) => (s) => s.replace(a, b).replace(a.replace(/\r\n/g, "\n"), b.replace(/\r\n/g, "\n"));
    // ---- D3 / D4: the enforcement ----
    judge("D3: the app's choice not enforced at the metadata (Chromium's first track heard)",
      R("    enforceAudio();\n    paintAudioUi();\n    paintRanges();", "    paintAudioUi();\n    paintRanges();"), enforcedOnMeta);
    judge("D3: ...enforced only AFTER the pending play started (the first sound heard is Chromium's: review M1)",
      R("    enforceAudio();\n    paintAudioUi();\n    paintRanges();                             // the duration may only be known from here\n"
        + "    if (pendingPlay) { pendingPlay = false; startPlay(); }\n    else if (armed) checkArmed();\n",
        "    paintAudioUi();\n    paintRanges();\n    if (pendingPlay) { pendingPlay = false; startPlay(); }\n    else if (armed) checkArmed();\n    enforceAudio();\n"),
      beforePlay);
    judge("D3: ...nor on a translatorFile (the tracks read after the metadata stay unenforced)",
      R("    enforceAudio();                            // WH (D3): what the tray wants heard", "    // (removed)"), readyLater);
    judge("D4: two entries left enabled (the others never switched off) -- on the spec's-letter runtime",
      R("A.forEach((a, i) => { if (i !== idx && a.enabled) { a.enabled = false; changed = true; } });", ""), exactlyOne, NX);
    judge("D3: every translatorFile rewrites the tracks (no 'only what differs')",
      R("if (!A[idx].enabled) { A[idx].enabled = true; changed = true; }", "{ A[idx].enabled = true; changed = true; }"), idempotent);
    judge("D4: the mapping counts the undecodable tracks too (raw position = Chromium's index)",
      R("P.forEach((t, i) => map.set(t.ord, i));", "T.forEach((t, i) => map.set(t.ord, i));"), dtsListed);
    judge("D4: counts equal = by order, whatever the languages say",
      R(" && P.every((t, i) => verdict(t, AK[i], names) >= 0)", ""), contradicted);
    judge("D4: a track the app thinks unplayable mapped on NO evidence",
      R("if (v < 0 || (v === 0 && !t.play)) return 0;", "if (v < 0) return 0;"), noEvidence);
    judge("D4: a tie not given to the codec every WebView decodes",
      R(" + (t.play && t.sure ? 1 : 0);", ";"), tieToSure);
    judge("D4: the auto fallback sent on every translatorFile (no once-per-file guard)",
      R("if (audioAutoFor !== itemKey() + \"/\" + want) {", "if (true) {"), autoOnce);
    judge("D4: the auto fallback not marked auto (the app would persist it as the viewer's)",
      R("send({ type: \"playerSet\", audioOrd: first.ord, auto: true });", "send({ type: \"playerSet\", audioOrd: first.ord });"), autoOnce);
    judge("D4: degrade TELLS the app a track it cannot name truthfully",
      R("    if (S.mode === \"mapped\") {\n      holdAudio(r.ord);", "    if (true) {\n      holdAudio(r.ord);"), degradeSilent);
    judge("WHP-5: names taken as evidence in an MP4 too (udta vs hdlr: a false contradiction, degrade)",
      R("const names = F.tracks.container === \"mkv\" || F.tracks.container === \"webm\";", "const names = true;"), mp4NamesNoContradiction);
    judge("WHP-5: names ignored in Matroska too (a real contradiction mapped by order)",
      R("const names = F.tracks.container === \"mkv\" || F.tracks.container === \"webm\";", "const names = false;"), mkvNamesCount);
    judge("no API: the section shown anyway (the app's rows, all unplayable)",
      R("if (!audioApi(media) || !(Number(media.readyState) > 0)) return none;", "if (!(Number(media.readyState) > 0)) return none;"), noApi, { noAudioTracks: true });
    judge("contract: audioOrd -1 ('none known') enforced as ord 0 (review M18)",
      R("    if (!(want >= 0)) return;                  // -1: none known yet\n", ""), noneKnownKeeps);
    judge("contract: a non-integer audioOrd taken as a number (\"\" -> 0: review M15)",
      R("audioOrd: Number.isInteger(msg.audioOrd) && msg.audioOrd >= 0 ? msg.audioOrd : -1,", "audioOrd: Number(msg.audioOrd) >= 0 ? Number(msg.audioOrd) : -1,"), noneKnownKeeps);
    // ---- WHP-2: the proxy ----
    judge("WHP-2: the proxy's <audio> taken for a mapped list (its rows choosable, the enforcement on it)",
      R("    if (role === \"proxy\") {\n      const T = st === \"ready\" ? F.tracks.audio : [];", "    if (false) {\n      const T = st === \"ready\" ? F.tracks.audio : [];"), proxyOneTrack);
    judge("WHP-2: the proxy's line not said in the panel",
      R("const note = !show2 ? \"\" : S.mode === \"proxy\" ? PROXY_LINE", "const note = !show2 ? \"\" : S.mode === \"proxy\" ? \"\""), proxyOneTrack);
    judge("WHP-2: A under the proxy says \"none can be played\"",
      R("    if (S.mode === \"proxy\") { showFileToast(PROXY_LINE, false); return; }\n", ""), proxyKeyA);
    judge("WHP-2: the D11 line under the proxy speaks of the track named before (not of ord 0, which plays)",
      R("    if (S.mode === \"proxy\") { const t = S.T.find((x) => x.ord === 0); return t ? speechInfo(langInfo(t.lang)) : nothing; }\n", ""), proxyLangNote);
    // ---- D5 + WHP-4/6: the viewer's choice ----
    judge("D5: a choice never told to the app", R("    send({ type: \"playerSet\", audioOrd: p.ord });", "    /* not sent */"), pickSends);
    judge("WHP-6: the choice sent at ONCE (no 800 ms: one re-track per press)",
      R("      audioPendTimer = setTimeout(sendAudioChoice, AUDIO_SEND_MS);", "      sendAudioChoice();"), pickSends);
    judge("WHP-6: no debounce left (AUDIO_SEND_MS = 0: a playerSet per press)", R("const AUDIO_SEND_MS = 800;", "const AUDIO_SEND_MS = 0;"), burstOne);
    judge("WHP-6: a burst back to the app's own track sent anyway",
      R("    if (p.ord === F.audioOrd && !(audioSent && audioSent.item === p.item)) {", "    if (false) {"), burstBack);
    judge("D5: no hold -- an in-flight translatorFile flips the choice back",
      R("if (audioHeld && audioHeld.item === itemKey() && (!Number.isFinite(now) || now < audioHeld.until)) return audioHeld.ord;", ""), holdsChoice);
    judge("WHP-4: any post with the held ord releases the hold (a STALE one too: R3)",
      R("    && (F.jobId > audioSent.job || audioSent.before !== audioSent.ord);", ";"), staleNoRelease);
    judge("D5: the hold never expires (review M2)", R("const AUDIO_HOLD_MS = 10000;", "const AUDIO_HOLD_MS = 1e12;"), holdExpires);
    judge("D5: the app's answer never releases the hold (review M3)",
      R("      if (!audioPend) audioHeld = null;        // (unless a newer choice is still waiting to be sent)", ""), holdsChoice);
    judge("D5: the key A not bound", R("    if (k === \"a\" || k === \"A\") { e.preventDefault(); cycleAudio(); return; }", ""), keyA);
    judge("D5: A lands on a track that can't be played (stuck there)",
      R("const rows = S.rows.filter((r) => !r.disabled);", "const rows = S.rows;"), keyA);
    judge("WHP-9: A says nothing about tracks not ready yet",
      R("showFileToast(F.tracks.state === \"probing\" || (many && S.mode === \"none\" && audioApi(media)) ? \"Sound tracks are not ready yet.\"",
        "showFileToast(false ? \"Sound tracks are not ready yet.\""), aNotReady);
    judge("the open panel rebuilt by every progress post", R("    if (newItem || audioMoved) paintAudioUi();", "    paintAudioUi();"), panelStable);
    judge("the panel keeps its name \"Subtitles\" with an Audio section",
      R("const name = withAudio ? \"Audio & subtitles\" : \"Subtitles\";", "const name = \"Subtitles\";"), titled);
    judge("generic handler names shown as labels (\"SoundHandler\")",
      R("    if (/^(sound|audio|video|subtitle|text)handler$/.test(l.replace(/\\s/g, \"\"))) return \"\";\n", ""), genericHidden);
    judge("D11: an ISO 639-2/B code unknown (fre)", R("eng: \"en\", fre: \"fr\", fra: \"fr\",", "eng: \"en\", fra: \"fr\","), isoBT);
    // ---- D7: the embedded subtitles ----
    judge("D7: an IMAGE subtitle track choosable",
      R("disabled: s.kind !== \"text\" || !s.ok, track: s.track, num: i + 1 };", "disabled: !s.ok, track: s.track, num: i + 1 };"), imageRefused);
    judge("D7: an UNREADABLE track choosable",
      R("disabled: s.kind !== \"text\" || !s.ok, track: s.track, num: i + 1 };", "disabled: s.kind !== \"text\", track: s.track, num: i + 1 };"), unreadableRefused);
    judge("D7: twins not told apart (two \"Chinese\" image tracks read the same)",
      R("    rows.forEach((r) => { if (n[k(r)] > 1) r.hint", "    rows.forEach((r) => { if (false) r.hint"), filmSubs);
    judge("WHP-10 (M13): the SOUND tracks' twins not told apart (one shared untwin, the same mutant seen from the Audio section)",
      R("    rows.forEach((r) => { if (n[k(r)] > 1) r.hint", "    rows.forEach((r) => { if (false) r.hint"), audioTwins);
    judge("WHP-12c: twins numbered by the CONTAINER's track (\"track 13\") instead of their rank",
      R("r.hint = (r.hint ? r.hint + \" · \" : \"\") + \"track \" + r.num;", "r.hint = (r.hint ? r.hint + \" · \" : \"\") + \"track \" + r.track;"), filmSubs);
    judge("D7: S lands on what can't be chosen", R("const list = sources().filter((s) => !s.disabled);", "const list = sources();"), sSkips);
    judge("D7: \"Loading…\" at once (no 700 ms)", R("const SUB_WAIT_MS = 700,", "const SUB_WAIT_MS = 0,"), loadingSaid);
    judge("D7: the answer does not end the wait (the row keeps \"loading…\": review M4)",
      R("    if (subWaitId && ext.id === subWaitId) subWaitEnd();   // WH: the extraction waited for is here",
        "    if (false) subWaitEnd();   // WH: the extraction waited for is here"), loadingRowEnds);
    judge("D7: the failure notice does not end the wait (\"Loading…\" covers the sentence)",
      R("      if (subWaitId || subWaitShown) { if (!msg.info) { wantExt = 0; wantEmbId = 0; } subWaitEnd(); }",
        "      if (false) { if (!msg.info) { wantExt = 0; wantEmbId = 0; } subWaitEnd(); }"), failureEndsWait);
    judge("D7: the request dropped AFTER the wait's repaint (the open panel keeps the failed track chosen: S-WEB 23/10)",
      R("      if (subWaitId || subWaitShown) { if (!msg.info) { wantExt = 0; wantEmbId = 0; } subWaitEnd(); }",
        "      if (subWaitId || subWaitShown) { subWaitEnd(); if (!msg.info) { wantExt = 0; wantEmbId = 0; } }"), failureDropsRequest);
    judge("D7: a failure keeps the request (the failed track still shown as chosen: review M10)",
      R("if (!msg.info) { wantExt = 0; wantEmbId = 0; }", "if (!msg.info) { wantEmbId = 0; }"), failureDropsRequest);
    judge("D7: the wait not ended by another pick (both of its ends removed: review M5)",
      (s) => R("    if (embPending()) abandonEmb();\n    subWaitEnd();\n", "    if (embPending()) abandonEmb();\n")(
             R("    subWaitEnd();\n    send({ type: \"playerSubGet\", id: 0 });", "    send({ type: \"playerSubGet\", id: 0 });")(s)), offEndsWait);
    judge("D7: an embedded track EDITABLE", R("const edRO = () => edIsExt() && !!ext.embedded;", "const edRO = () => false;"), readOnly);
    judge("D7: the Encoding row shown for an embedded track (review M6)",
      R("const encOn = subSrc === \"ext\" && !!ext && !ext.embedded;", "const encOn = subSrc === \"ext\" && !!ext;"), noEncRow);
    // ---- WHP-1: the abandon ----
    judge("WHP-1: leaving a track still extracted tells the app nothing (it lands anyway)",
      R("    if (embPending()) abandonEmb();\n", ""), abandonCancels);
    judge("WHP-1: the abandon sends no cancel",
      R("    subWaitEnd();\n    send({ type: \"playerSubGet\", id: 0 });", "    subWaitEnd();"), abandonCancels);
    judge("WHP-1: the abandoned track's late result accepted (both doors open: its document, the state naming it)",
      (s) => R("    if (dropIds.has(id)) return;", "")(R("        && !dropIds.has(F.extSubId))", ")")(s)), abandonCancels);
    judge("WHP-1: its late DOCUMENT accepted (over the file the viewer went back to)",
      R("    if (dropIds.has(id)) return;", ""), abandonForSidecar);
    judge("WHP-1: its late failure said over the new choice",
      R("      else if (!msg.info && dropIds.size && abandonedLately() && SUB_TRACK_FAIL.test(text)) return;\n", ""), abandonFailureQuiet);
    judge("WHP-1: the failure window never closes (a later track failure swallowed too)",
      R("const ABANDON_MS = 3000;", "const ABANDON_MS = 1e12;"), abandonFailureQuiet);
    judge("WHP-1: any notice swallowed after an abandon (not only a subtitle-track failure)",
      R("const SUB_TRACK_FAIL = /\\bsubtitle track\\b|^Subtitles inside |^The video file could not be read|^No subtitles were found in this track\\b/i;",
        "const SUB_TRACK_FAIL = /./;"), abandonFailureQuiet);
    judge("WHP-1: another file does not cancel the extraction",
      R("      if (embPending()) send({ type: \"playerSubGet\", id: 0 });\n", ""), newItemCancels);
    judge("WHP-1: a track asked again stays abandoned (its answer ignored)",
      R("    if (embedded) dropIds.delete(id);         // wanted again: its result is welcome again\n", ""), reaskWelcome);
    judge("WHP-1: a file next to the film 'abandoned' too (a cancel for nothing)",
      R("const embPending = () => wantEmbId > 0 && wantExt === wantEmbId;", "const embPending = () => wantExt > 0;"), sidecarNoCancel);
    judge("WHP-1: the Encoding change leaves the embedded wait up (a minute of \"Loading…\" for a cancelled extraction)",
      R(["    if (embPending()) abandonEmb();", "    subWaitEnd();", "    el.ccEnc.value = chosen;", ""].join("\n"), ["    el.ccEnc.value = chosen;", ""].join("\n")),
      encodingEndsWait);
    judge("WHP-1: a file loaded meanwhile leaves the extraction pending (its row 'loading…', its late result shown over the file)",
      R("    else if (embPending() && ext.id !== wantEmbId) abandonEmb();", ""), loadedFileAbandons);
    judge("WHP-3: the row being extracted re-asked at every click",
      R("    if (s.src === \"ext\" && s.embedded && embPending() && wantEmbId === s.id) return;\n", ""), repickOnce);
    // ---- WHP-8: names ----
    judge("WHP-8: only C0 controls taken out of names (bidi overrides kept)",
      R("const NAME_JUNK = /[\\u0000-\\u001f\\u007f-\\u009f\\u061c\\u200e\\u200f\\u2028\\u2029\\u202a-\\u202e\\u2066-\\u2069\\ufeff]/g;",
        "const NAME_JUNK = /[\\u0000-\\u001f\\u007f]/g;"), bidiClean);
    judge("WHP-8: names unbounded", R("    if (cps.length > n) s = cps.slice(0, n - 1).join(\"\").trim() + \"…\";", ""), nameBounded);
    judge("WHP-8: the embedded toast says the app's raw name",
      R("const said = ext.embedded ? (embeddedWords(ext.id) || cleanName(ext.name)) :", "const said = ext.embedded ? ext.name :"), embToastWords);
    // ---- D10: the default ----
    judge("D10: the target language ignored (A taken for B)", R("const b = pair().b, code = (s) => langInfo(s.lang).code;", "const b = pair().a, code = (s) => langInfo(s.lang).code;"), d10Target);
    judge("D10: no forced fallback", R(" || subs.find((s) => s.forced) || null;", " || null;"), d10Forced);
    judge("D10: the forced fallback ignores the heard language (review M8)",
      R(" || subs.find((s) => s.forced && !!heard && langFamily(code(s)) === heard)", ""), d10HeardIsApps);
    judge("WHP-7: the heard language read off Chromium's pre-enforcement track, not the app's audioOrd",
      R("const want = wantedOrd(), wt = F.tracks.audio.find((x) => x.ord === want);", "const want = -1, wt = null;"), d10HeardIsApps);
    judge("D10: an IMAGE track taken", R("const subs = F.tracks.subs.filter((s) => s.kind === \"text\" && s.ok);", "const subs = F.tracks.subs.filter((s) => s.ok);"), d10NeverImage);
    judge("D10: a track taken for its FlagDefault", R(" || subs.find((s) => s.forced) || null;", " || subs.find((s) => s.forced) || subs.find((s) => s.def) || null;"), d10NotByDefault);
    judge("D10: never decided when the tracks arrive after the opening",
      R("      if (F.tracks.state === \"ready\") applyEmbeddedDefault(); else subDefaultArmed = \"\";", "      subDefaultArmed = \"\";"), d10Later);
    judge("D10: the viewer's own choice overridden",
      R("if (subPicked === itemKey() || subSrc !== \"off\" || wantExt || F.extSubId) return;", "if (wantExt || F.extSubId) return;"), d10UserFirst);
    // (U1: what keeps D10 away once the file is translated is now the PRESS itself -- Translate puts SuVtitle's subtitles on
    //  screen (useOwnSubs: the source switched, the armed default dropped); embeddedDefault's own `F.translate` test stays as
    //  a second wall, unreachable behind it: its old mutant could no longer fail)
    judge("D10: applied with translation on (U1: the press leaves the screen to the default rule)",
      R("    if (subSrc === \"audiopb\") return;\n    subPicked = itemKey();", "    return;\n    subPicked = itemKey();"), d10NotWhenOn);
    judge("WHP-12d: a \"chi\" track whose NAME says Traditional not preferred for a 繁體中文 B",
      R(" || subs.find((s) => named(s) && !s.forced)", ""), d10TradNamed);
    judge("D10: the exact code not before the family (review M12)",
      R("    return subs.find((s) => exact(s) && !s.forced) || ", "    return "), d10TradExact);
    // ---- D11: the line, the names ----
    judge("D11: said for a language IN the pair", R("} else if (fam !== langFamily(a) && fam !== langFamily(b))", "} else if (true)"), langNote);
    judge("D11: said with translation off", R("if (F.phase !== \"idle\" && F.translate) {\n      const L = heardLang();", "if (F.phase !== \"idle\") {\n      const L = heardLang();"), langNoteOff);
    judge("D11: the two Chinese scripts taken for two languages", R("(code === \"zh-tw\" || code === \"yue\" ? \"zh\"", "(code === \"yue\" ? \"zh\""), chineseFamily);
    judge("D11: nb / nn not Norwegian's family (review M7)", R("code === \"nb\" || code === \"nn\" ? \"no\"", "code === \"nb\" ? \"no\""), families);
    judge("D11: the RUNNING pair instead of the job's (review M14)",
      R("const a = normLang(F.langA) || pair().a, b = normLang(F.langB) || pair().b;", "const a = pair().a, b = pair().b;"), langNoteJobPair);
    judge("WHP-12a: one way taken for both ways (a French track under English → French: no line)",
      R("        if (dir === \"a2b\" || dir === \"b2a\") {", "        if (false) {"), langNoteOneWay);
    judge("WHP-12b: a sound track named by its SCRIPT (\"Chinese (Traditional)\" for speech)",
      R("const speechInfo = (L) => (L.code === \"zh\" || L.code === \"zh-tw\" ? { code: L.code, name: TRACK_LANG_NAMES.zh } : L);",
        "const speechInfo = (L) => L;"), speechNames);
    // ---- S-HOST 23/10: an item switch ----
    judge("S-HOST 23/10: the list not rebuilt on an item switch (641f1aa's page: the previous item's subtitle FILE stays listed)",
      R("    if (relist || (newItem && subSrc === \"ext\")) renderList();", ""), switchDropsFile);
  }
};

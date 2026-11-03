// test_u3.js -- WAVE U3 (1.1.0, 02/11/2026): the TRANSCRIPTION in the PLAYER (docs\PROGRESS_U3.md). Required by test_ui.js (same
// ok/section counters). Every check runs the REAL page (app/web/index.html) through run_ui.js, and each rule has a MUTATION SENTINEL
// at the end: the page with that rule taken out must FAIL its probe. The app's half (TrPlayer.h: two wishes on one analysis, the
// voice pass, the Word file, the names, the source timecode, Transcribe all) is RUN by tests\cpp\player_smoke.cpp [U3] (sentinels:
// tests\cpp\cmut_u3.py), the bridge's (where the Word file goes, the dialog, the doors) PINNED by tests\web\test_host_pins.js
// (U3-1..U3-3).
//   1 THE TRANSCRIBE SLOT beside Translate's: Transcribe / Cancel / Re-transcribe, one at a time, by ITS wish (translatorFile
//     .transcribe / .tphase); Translate's slot follows ITS wish (an analysis the transcript alone runs leaves Translate offered).
//   2 "Transcribe all" and the rows' SECOND badge (tstate / tpct); Cancel all for either batch; the words count both.
//   3 THE TRANSCRIPT TAB: the app's translatorTranscript drawn whole (only while it shows; an older generation ignored); a click on
//     a turn plays from there; a click on a NAME renames that speaker in place (Enter / leaving saves, Esc cancels, 64 bytes,
//     no control character -- said under the field); Save .docx...; the "older" Word file said.
//   4 the Export menu's two Word files; a narrow window's ⋯ (the Transcribe slot's other two, the models); the contract and the
//     bridge name every message; textContent only; the theme's tokens only.
"use strict";
module.exports = function u3Checks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, qMsg, qItem, html, script, MARKUP_SINK, codeOnly, bridgeSrc } = ctx;
  const J = JSON.stringify;
  const sent = (w, t) => w.outbox.filter((m) => m && m.type === t);
  const vis = (w, ...ids) => ids.map((i) => (w.ids[i].hidden ? "-" : i)).join(",");
  const TURNS = [
    { s: 1000, e: 4000, spk: 1, name: "Speaker 1", tc: "[00:00:01]", text: "Good morning everyone." },
    { s: 5000, e: 6383, spk: 2, name: "Marie", tc: "[00:00:05]", text: "I agree." },
    { s: 6383, e: 9000, spk: 1, name: "Speaker 1", tc: "[00:00:06]", text: "So let's move on." },
    { s: 20000, e: 24000, spk: 2, name: "Marie", tc: "[00:00:20]", text: "Thanks for coming." },
  ];
  const txMsg = (o) => Object.assign({ type: "translatorTranscript", jobId: 1, state: "done", pct: 0, speakers: 2,
    names: [{ id: 2, label: "Marie" }], why: "", langs: ["en"], complete: true, unmatched: 0, suggest: false,
    tc: "from the start of the file", tcSource: false, file: "film - transcript.docx", older: false, note: "", durMs: 600000,
    turns: TURNS, cut: false }, o);

  // ---- the probes (each a check here AND the condition its sentinel must see fail) ------------------------------------------------
  // 1a: nothing asked: Transcribe offered; pressed = translatorTranscribe{force:false, quality} (this file only)
  const slotIdle = (w) => {
    w.send(fileMsg({ phase: "off", translate: false, transcribe: false, tphase: "off" }));
    const shown = vis(w, "fileTranscribe", "fileTrCancel", "fileRetranscribe") === "fileTranscribe,-,-";
    w.outbox.length = 0;
    w.ids.fileTranscribe.click();
    return shown && J(sent(w, "translatorTranscribe")) === J([{ type: "translatorTranscribe", force: false, quality: "auto" }])
      && /speakers named by voice, and save it as a Word file/.test(w.ids.fileTranscribe.title);
  };
  // 1b: the transcript wished and its analysis running: Transcribe's slot = Cancel (its own: translatorTranscriptCancel{jobId});
  //     Translate's slot stays TRANSLATE (the translation is not asked: a press joins the analysis)
  const slotRunning = (w) => {
    w.send(fileMsg({ jobId: 4, phase: "analyzing", translate: false, transcribe: true, tphase: "waiting", tpct: 12 }));
    const slots = vis(w, "fileTranslate", "fileCancel", "fileReanalyse", "fileTranscribe", "fileTrCancel", "fileRetranscribe")
      === "fileTranslate,-,-,-,fileTrCancel,-";
    w.outbox.length = 0;
    w.ids.fileTrCancel.click();
    return slots && J(sent(w, "translatorTranscriptCancel")) === J([{ type: "translatorTranscriptCancel", jobId: 4 }]);
  };
  // 1c: BOTH wished on one analysis: two Cancels, each its own
  const slotBoth = (w) => {
    w.send(fileMsg({ jobId: 5, phase: "analyzing", translate: true, transcribe: true, tphase: "waiting" }));
    const slots = vis(w, "fileTranslate", "fileCancel", "fileTranscribe", "fileTrCancel") === "-,fileCancel,-,fileTrCancel";
    w.outbox.length = 0;
    w.ids.fileCancel.click();
    return slots && J(w.outbox.map((m) => m.type)) === J(["translatorFileCancel"]);
  };
  // 1d: the voice pass: Cancel names its %; then done: Re-transcribe (force:true); the translation done: Re-analyse beside it
  const slotDone = (w) => {
    w.send(fileMsg({ jobId: 6, phase: "done", complete: true, translate: false, transcribe: true, tphase: "voices", tpct: 61 }));
    const voices = !w.ids.fileTrCancel.hidden && /61 %/.test(w.ids.fileTrCancel.title);
    w.send(fileMsg({ jobId: 6, phase: "done", complete: true, translate: false, transcribe: true, tphase: "done", fresh: false }));
    const done = vis(w, "fileReanalyse", "fileTranscribe", "fileTrCancel", "fileRetranscribe") === "fileReanalyse,-,-,fileRetranscribe";
    w.outbox.length = 0;
    w.ids.fileRetranscribe.click();
    return voices && done && J(sent(w, "translatorTranscribe")) === J([{ type: "translatorTranscribe", force: true, quality: "auto" }]);
  };
  // 2: the rows' second badge, Transcribe all, Cancel all for either batch, the words counting both
  // (FX re-point, F7 02/11: Cancel all follows the rows the LIST has -- the app's `batch`: E02-E04 are Transcribe all's)
  const batch = (w) => {
    w.send(qMsg([qItem(11, "E01.mkv", { state: "ready", pct: 100, tstate: "done" }),
                 qItem(12, "E02.mkv", { state: "ready", pct: 100, tstate: "voices", tpct: 61, batch: true }),
                 qItem(13, "E03.mkv", { state: "analysing", pct: 12, tstate: "analysing", tpct: 12, batch: true }),
                 qItem(14, "E04.mkv", { state: "none", tstate: "queued", batch: true }),
                 qItem(15, "E05.mkv", { state: "ready", pct: 100, tstate: "error" }),
                 qItem(16, "E06.mkv", { state: "none", tstate: "" })], { cur: 11, loaded: false }));
    const rows = w.ids.queueList.children, tb = (i) => rows[i].children[4];
    const badges = rows.map((r, i) => tb(i).textContent).join("|") === "¶ ✓|¶ voices 61 %|¶ 12 %|¶ queued|¶ ⚠|"
      && tb(0).className === "q-tbadge st-done" && tb(1).classList.contains("st-voices") && tb(5).className === "q-tbadge";
    // (FX2 re-point, 03/11: ONE transcript done here -- E01 -- is "1 transcript done", no more "1 transcripts done")
    const btns = !w.ids.qTranscribeAll.disabled && !w.ids.qCancelAll.hidden
      && /\b1 transcript done/.test(w.ids.qBatchText.textContent) && /recognising voices/.test(w.ids.qBatchText.textContent);
    w.outbox.length = 0;
    w.ids.qTranscribeAll.click();
    w.send({ type: "playerItem", id: 14, state: "none", pct: 0, tstate: "voices", tpct: 5 });
    return badges && btns && J(sent(w, "translatorTranscribeAll")) === J([{ type: "translatorTranscribeAll", quality: "auto" }])
      && tb(3).textContent === "¶ voices 5 %";
  };
  // 3a: the tab: hidden = nothing drawn (the message kept); opened = drawn whole: the head, the state, a row per turn
  //     (stamp, a NAME button, the text); a click on a turn plays from its FILE position; an older generation ignored
  const tabDrawn = (w) => {
    w.send(fileMsg({ jobId: 7, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.send(txMsg({ jobId: 7 }));
    const hiddenNothing = w.ids.txList.children.length === 0;
    w.ids.tabTranscript.click();
    const rows = w.ids.txList.children;
    const drawn = !w.ids.transcriptPane.hidden && w.ids.fileList.hidden && rows.length === 4
      && rows[1].children.map((c) => c.className + ":" + c.textContent).join("|") === "tx-tc:[00:00:05]|tx-name:Marie|tx-text:I agree."
      && /2 speakers/.test(w.ids.txHead.textContent) && /film - transcript\.docx/.test(w.ids.txHead.textContent)
      && w.ids.txSave.disabled === false && JSON.parse(w.lsStore["audiopb.tr.panel2"] || "{}").tab === "transcript";
    if (!drawn || rows.length < 4) return false;                          // (a mutant may draw nothing)
    rows[3].click();
    const seek = Math.abs(w.ids.fileVideo.currentTime - 20.001) < 1e-6 && rows[3].classList.contains("is-active");
    w.send(txMsg({ jobId: 6, turns: [TURNS[0]] }));                     // an older generation's
    const stale = w.ids.txList.children.length === 4;
    w.send(txMsg({ jobId: 7, state: "done", older: true }));
    const older = /older than this transcript/.test(w.ids.txState.textContent);
    w.outbox.length = 0;
    w.ids.txSave.click();
    return hiddenNothing && drawn && seek && stale && older && J(w.outbox) === J([{ type: "translatorTranscriptSave" }]);
  };
  // 3c: while the analysis runs, the tab's figure is the STRIP's (translatorFile's doneMs / totalMs) and follows it at every
  //     progress report -- the transcript itself is posted when its text grows (a long stretch with no speech: no post)
  const tabPct = (w) => {
    w.send(fileMsg({ jobId: 9, phase: "analyzing", doneMs: 30000, totalMs: 100000, translate: false, transcribe: true, tphase: "waiting" }));
    w.ids.tabTranscript.click();
    w.send(txMsg({ jobId: 9, state: "analysing", pct: 10, speakers: 0, names: [], file: "", complete: false }));
    const first = /\(30 %\)/.test(w.ids.txState.textContent);
    w.send(fileMsg({ jobId: 9, phase: "analyzing", doneMs: 60000, totalMs: 100000, translate: false, transcribe: true, tphase: "waiting" }));
    return first && /\(60 %\)/.test(w.ids.txState.textContent) && w.ids.txList.children.length === 4;
  };
  // 3b: RENAME in place: a click on a name = a field (the name in it); Enter saves (translatorSpeakerName{jobId, id, label}, trimmed);
  //     Esc cancels (nothing sent); a control character or > 64 bytes: refused UNDER the field (nothing sent, the field kept); a
  //     click on the name never seeks
  const rename = (w) => {
    w.send(fileMsg({ jobId: 8, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.ids.tabTranscript.click();
    w.send(txMsg({ jobId: 8, vrev: 3 }));                                // (FX re-point, F8: its speakers' numbering)
    const nameBtn = () => w.ids.txList.children[1].children[1];
    const t0 = w.ids.fileVideo.currentTime;
    nameBtn().click();
    const f = w.ids.txList.children[1].children[1];
    const field = f.tagName === "INPUT" && f.value === "Marie" && w.ids.fileVideo.currentTime === t0;
    w.outbox.length = 0;
    f.value = "a" + String.fromCharCode(1) + "b";
    f.dispatch("keydown", { key: "Enter" });
    const refused = w.outbox.length === 0 && /control character/.test(w.ids.txList.children[1].children[2].textContent)
      && w.ids.txList.children[1].children[1] === f;
    f.value = "é".repeat(33);                                            // 66 bytes of UTF-8
    f.dispatch("keydown", { key: "Enter" });
    const tooLong = w.outbox.length === 0 && /64 bytes/.test(w.ids.txList.children[1].children[2].textContent);
    f.value = "  Marie Curie ";
    f.dispatch("keydown", { key: "Enter" });
    // (FX re-point, F8 02/11: the message also names the speakers' numbering the field was opened on -- vrev)
    const saved = J(w.outbox) === J([{ type: "translatorSpeakerName", jobId: 8, id: 2, label: "Marie Curie", vrev: 3 }])
      && w.ids.txList.children[1].children[1].tagName === "BUTTON";
    nameBtn().click();
    const g = w.ids.txList.children[1].children[1];
    w.outbox.length = 0;
    g.value = "Someone else";
    g.dispatch("keydown", { key: "Escape" });
    return field && refused && tooLong && saved && w.outbox.length === 0 && w.ids.txList.children[1].children[1].tagName === "BUTTON";
  };
  // 4a: the Export menu: the bilingual Word file by its own name, and "Transcript (.docx)" = the app's Save (translatorTranscriptSave)
  const exportTwo = (w) => {
    w.send(fileMsg({ jobId: 9, phase: "done", complete: true, count: 1 }));
    w.send({ type: "translatorFileCue", jobId: 9, idx: 0, s: 1000, e: 2000, lang: "en", dstLang: "zh", pass: false, src: "Hi.", dst: "你好。" });
    w.ids.exportBtn.click();
    const items = w.ids.exportMenu.children;
    w.outbox.length = 0;
    items[5].click();
    return items.length === 6 && items[4].textContent === "Bilingual subtitles (.docx)" && items[5].textContent === "Transcript (.docx)"
      && J(w.outbox) === J([{ type: "translatorTranscriptSave" }]);
  };
  // 4b: 420 x 320: the models leave the strip for ⋯ ("Models: …"), with the Transcribe slot's Cancel when it runs
  const narrow = (w) => {
    w.send(fileMsg({ jobId: 10, phase: "analyzing", translate: true, transcribe: true, tphase: "waiting" }));
    w.ids.tMore.click();
    const labels = w.ids.morePanel.children.map((b) => b.textContent);
    // (U2b re-point: G1, the user's answer 02/11 -- "Cancel transcript", the strip's own words)
    return labels.indexOf("Cancel transcript") >= 0 && labels.some((l) => /^Models: .+…$/.test(l))
      && /body\.is-narrow #stripModelSlot \{ display: none; \}/.test(html);
  };

  // ---- FX (02/11/2026, the adversarial review's fix round -- docs\PROGRESS_FIX.md): F2 F3 F6 F7 F8, the page's half --------------
  // FX-F7: Cancel all ONLY while the LIST has work (the app's `batch`, U2's rule): a file pressed alone ("analysing" but not the
  //        list's) -> hidden; a row Transcribe all queued -> shown; that row out of the list again -> hidden
  const fxCancelRule = (w) => {
    w.send(qMsg([qItem(21, "P01.mkv", { state: "analysing", pct: 30, tstate: "analysing", tpct: 30, batch: false }),
                 qItem(22, "P02.mkv", { state: "none" })], { cur: 21, loaded: true }));
    const pressedHidden = w.ids.qCancelAll.hidden === true;
    w.send(qMsg([qItem(21, "P01.mkv", { state: "analysing", pct: 30, tstate: "analysing", tpct: 30, batch: false }),
                 qItem(22, "P02.mkv", { state: "none", tstate: "queued", batch: true })], { cur: 21, loaded: true }));
    const listShown = w.ids.qCancelAll.hidden === false;               // (the whole list says it)
    w.send({ type: "playerItem", id: 22, state: "none", pct: 0, tstate: "cancelled", tpct: 0, batch: false });
    const leftHidden = w.ids.qCancelAll.hidden === true;
    w.send({ type: "playerItem", id: 22, state: "none", pct: 0, tstate: "queued", tpct: 0, batch: true });
    return pressedHidden && listShown && leftHidden && w.ids.qCancelAll.hidden === false;   // (and a row's own update)
  };
  // FX-F3: its tooltip says U2's rule -- what BOTH lists started (the transcripts too); a file pressed yourself goes on
  const fxCancelTip = (w) => {
    const t = String(w.ids.qCancelAll.title || "");
    return /^Stop what Translate all and Transcribe all started/.test(t) && /transcribed/.test(t)
      && /A file you pressed Translate or Transcribe on yourself goes on\.$/.test(t) && !/every analysis/.test(t);
  };
  // FX-F6: a transcribed row says where its Word file is when it is NOT next to it, or why there is none (a line under its name,
  //        whole in its tooltip); its ¶ badge's tooltip says "next to it" ONLY when it is (tdoc ""); a playerItem moves it
  const fxRowDoc = (w) => {
    w.send(qMsg([qItem(31, "D01.mkv", { state: "ready", pct: 100, tstate: "done", tdoc: "" }),
                 qItem(32, "D02.mkv", { state: "ready", pct: 100, tstate: "done", tdoc: "Word file in Documents\\SuVtitle Transcripts" }),
                 qItem(33, "D03.mkv", { state: "ready", pct: 100, tstate: "nonames",
                   tdoc: "The Word file was not saved: every transcript name for this file is taken by other files." })]));
    const rows = () => w.ids.queueList.children;
    const line = (i) => Array.from(rows()[i].children[2].children || []).find((c) => c.classList && c.classList.contains("q-tdoc")) || null;
    const tip = (i) => String(rows()[i].children[4].title || "");
    const next = !line(0) && /^Transcribed, speakers named \(its Word file next to it\)$/.test(tip(0));
    const away = !!line(1) && line(1).textContent === "Word file in Documents\\SuVtitle Transcripts" && line(1).title === line(1).textContent
      && /Documents\\SuVtitle Transcripts/.test(tip(1)) && !/next to it/.test(tip(1));
    const none = !!line(2) && /every transcript name/.test(line(2).textContent) && /^Transcribed, without speaker names -- /.test(tip(2))
      && !/next to it/.test(tip(2));
    w.send({ type: "playerItem", id: 32, state: "ready", pct: 100, tstate: "done", tpct: 0, tdoc: "" });
    return next && away && none && !line(1) && /\(its Word file next to it\)$/.test(tip(1));
  };
  // FX-F8: the name field names a speaker of the transcript it was OPENED on --
  //   (a) Enter sends THAT transcript's generation and vrev (not the strip's generation of the moment)
  //   (b) another file starts (a new generation) while it is open: it closes, nothing sent, the toast says the name was not kept
  //   (c) a Re-transcribe ends (the same generation, a new vrev): the same; the same transcript posted again keeps it open
  const fxOpen = (w, fileJob, txJob) => {
    w.send(fileMsg({ jobId: fileJob, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.ids.tabTranscript.click();
    w.send(txMsg({ jobId: txJob, vrev: 3 }));
    w.ids.txList.children[1].children[1].click();                      // "Marie" -> its field
    const f = w.ids.txList.children[1].children[1];
    if (!f || f.tagName !== "INPUT") return null;
    f.value = "Paul";
    w.outbox.length = 0;
    return f;
  };
  const fxRenameOpenTime = (w) => {
    const f = fxOpen(w, 7, 8);                                         // (the transcript of generation 8 before the strip's post)
    if (!f) return false;
    f.dispatch("keydown", { key: "Enter" });
    return J(sent(w, "translatorSpeakerName")) === J([{ type: "translatorSpeakerName", jobId: 8, id: 2, label: "Paul", vrev: 3 }]);
  };
  const fxRenameNewFile = (w) => {
    const f = fxOpen(w, 8, 8);
    if (!f) return false;
    w.send(fileMsg({ jobId: 9, seq: 2, itemId: 2, phase: "off" }));   // the next file starts (auto-advance, --play)
    f.dispatch("keydown", { key: "Enter" });
    return sent(w, "translatorSpeakerName").length === 0 && /was not renamed: the transcript changed/.test(w.ids.fileToast.textContent || "")
      && !Array.from(w.ids.txList.children).some((r) => Array.from(r.children || []).indexOf(f) >= 0);
  };
  const fxRenameRenumbered = (w) => {
    const f = fxOpen(w, 8, 8);
    if (!f) return false;
    w.send(txMsg({ jobId: 8, vrev: 3, older: true }));                 // the same transcript posted again: the field stays
    const kept = w.ids.txList.children[1].children[1] === f;
    w.send(txMsg({ jobId: 8, vrev: 4 }));                              // a Re-transcribe ended: its speakers numbered again
    f.dispatch("keydown", { key: "Enter" });
    return kept && sent(w, "translatorSpeakerName").length === 0 && /was not renamed/.test(w.ids.fileToast.textContent || "");
  };
  // FX-F2 (the user's rule 02/11: ONE Word file per media AND sound track): this track's Word file holds another analysis'
  //        transcript -- the tab says so in one sentence, never "changed outside SuVtitle", and names no Word file of its own
  const fxOther = (w) => {
    w.send(fileMsg({ jobId: 12, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.ids.tabTranscript.click();
    w.send(txMsg({ jobId: 12, file: "", other: true }));
    const st = String(w.ids.txState.textContent || "");
    return /The Word file of this sound track holds another transcript of it \(another language pair or model\): it is kept \(Save \.docx… writes this one to a new file\)\./.test(st)
      && !/changed outside SuVtitle/.test(st) && !/transcript\.docx/.test(String(w.ids.txHead.textContent || ""));
  };

  // ---- FX2 (03/11/2026, the round after the user's installer test -- docs\PROGRESS_FX2.md) ------------------------------------------
  // The core now says Transcribe all's ONE notice with `info: true` (TrPlayer.h notice(text, info)): the page's own rule for an `info`
  // notice -- a toast that is not red, and a subtitle the viewer asked for is NOT dropped (a notice WITHOUT `info` is the failure
  // path: it ends that request -- WH / RV4). (Kept as it was, said in the log: any notice ends the "Loading..." wait -- subWaitEnd.)
  const FX2_NOTICE = "Transcribe all: 1 file has no up-to-date Word file next to it -- its row in the Queue tab says why.";
  const FX2_TRACKS = { state: "ready", container: "mkv", video: { codec: "V_MPEG4/ISO/AVC", w: 640, h: 360 },
    audio: [{ ord: 0, track: 2, name: "", lang: "eng", codec: "A_AAC", ch: 2, def: true, play: true }],
    subs: [{ id: 72, track: 8, name: "", lang: "fre", codec: "S_TEXT/ASS", kind: "text", def: false, forced: false, ok: true }] };
  const fx2Film = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 60000, ranges: [[0, 60000]], tracks: FX2_TRACKS, audioOrd: 0,
                     sidecars: [{ id: 9, name: "film.fr.srt", lang: "fr" }] }));
    w.ids.fileVideo._setAudioTracks([{ id: "2", kind: "main", label: "", language: "eng" }]);
    metaOk(w.ids.fileVideo);
    if (w.ids.ccPanel.hidden) w.ids.tCC.click();
  };
  const fx2Pick = (w, k) => { const b = w.ids.ccSources.children.find((x) => x.dataset.key === k); if (b) b.click(); return !!b; };
  const fx2On = (w) => { const b = w.ids.ccSources.children.find((x) => x.classList.contains("is-on")); return b ? b.dataset.key : ""; };
  const fx2Subs = (id, o) => Object.assign({ type: "playerSubs", id, name: id === 9 ? "film.fr.srt" : "French (track 8)", enc: 65001,
    encName: "UTF-8", forced: false, truncated: false, part: 0, parts: 1, cues: [{ i: 0, s: 1000, e: 3000, r: [["Salut", 0]] }] }, o);
  // (a) the film's own subtitle track being extracted ("Loading the subtitle track" said): Transcribe all's notice arrives -- the
  //     request KEPT (still chosen; shown when it comes), the toast says the notice, not in red
  const fx2InfoKeepsTrack = (w) => {
    fx2Film(w);
    if (!fx2Pick(w, "x:9")) return false;
    w.send(fx2Subs(9));                                                // the file next to the film, shown
    fx2Pick(w, "x:72");                                                // the film's French track asked (the app extracts it)...
    w.pumpTimers(800);                                                 // (the wait said)
    w.send({ type: "translatorFileNotice", text: FX2_NOTICE, info: true });   // ...Transcribe all's notice meanwhile
    const kept = fx2On(w) === "x:72";
    const toast = !w.ids.fileToast.hidden && w.ids.fileToast.textContent === FX2_NOTICE && !w.ids.fileToast.classList.contains("is-error");
    w.send(fx2Subs(72, { embedded: true, track: 8 }));                 // ...then the track
    return kept && toast && fx2On(w) === "x:72";
  };
  // (b) a subtitle FILE next to the film asked, not here yet (no wait said; a file is marked chosen once it is here): the same --
  //     shown when it comes (a notice without `info` drops that request: the file, when it comes, is not shown -- measured)
  const fx2InfoKeepsFile = (w) => {
    fx2Film(w);
    if (!fx2Pick(w, "x:9")) return false;
    w.send({ type: "translatorFileNotice", text: FX2_NOTICE, info: true });
    w.send(fx2Subs(9));
    return fx2On(w) === "x:9";
  };
  // the plural: Transcribe all's words count the transcripts -- "1 transcript queued / done / without names / failed", "2
  // transcripts ..."; "recognising voices" says a state, not a number of transcripts: unchanged
  const DOT = " · ";
  const fx2Words = (w) => {
    w.send(qMsg([qItem(41, "S01.mkv", { state: "ready", pct: 100, tstate: "done" }),
                 qItem(42, "S02.mkv", { state: "ready", pct: 100, tstate: "nonames" }),
                 qItem(43, "S03.mkv", { state: "ready", pct: 100, tstate: "error" }),
                 qItem(44, "S04.mkv", { state: "ready", pct: 100, tstate: "queued", batch: true }),
                 qItem(45, "S05.mkv", { state: "ready", pct: 100, tstate: "voices", tpct: 5, batch: true })]));
    const one = w.ids.qBatchText.textContent;
    w.send(qMsg([qItem(51, "T01.mkv", { state: "ready", pct: 100, tstate: "done" }), qItem(52, "T02.mkv", { state: "ready", pct: 100, tstate: "done" }),
                 qItem(53, "T03.mkv", { state: "ready", pct: 100, tstate: "nonames" }), qItem(54, "T04.mkv", { state: "ready", pct: 100, tstate: "nonames" }),
                 qItem(55, "T05.mkv", { state: "ready", pct: 100, tstate: "error" }), qItem(56, "T06.mkv", { state: "ready", pct: 100, tstate: "error" }),
                 qItem(57, "T07.mkv", { state: "ready", pct: 100, tstate: "queued", batch: true }),
                 qItem(58, "T08.mkv", { state: "ready", pct: 100, tstate: "queued", batch: true }),
                 qItem(59, "T09.mkv", { state: "ready", pct: 100, tstate: "voices", tpct: 5, batch: true }),
                 qItem(60, "T10.mkv", { state: "ready", pct: 100, tstate: "voices", tpct: 9, batch: true })]));
    const two = w.ids.qBatchText.textContent;
    return one === ["5 done", "1 recognising voices", "1 transcript queued", "1 transcript done", "1 transcript without names", "1 transcript failed"].join(DOT)
      && two === ["10 done", "2 recognising voices", "2 transcripts queued", "2 transcripts done", "2 transcripts without names", "2 transcripts failed"].join(DOT);
  };

  section("U3-1 -- the TRANSCRIBE slot beside Translate's: Transcribe / Cancel / Re-transcribe, each slot by its own wish");
  ok("nothing asked: Transcribe offered (its tooltip says what it makes); pressed = translatorTranscribe{force:false, quality}",
     slotIdle(runTranslatorWindow()));
  ok("the transcript wished, its analysis running: its slot = Cancel (translatorTranscriptCancel{jobId}); Translate's stays "
     + "TRANSLATE (not asked: a press joins the one analysis)", slotRunning(runTranslatorWindow()));
  ok("both wished on one analysis: two Cancels, Translate's sends ITS cancel only", slotBoth(runTranslatorWindow()));
  ok("the voice pass: Cancel names its %; complete: Re-transcribe (force:true), Re-analyse beside it", slotDone(runTranslatorWindow()));
  ok("the slot sits right after Translate's, before the pair (one place: the start of the strip)",
     // (U2b re-point: G1 -- its Cancel reads "Cancel transcript")
     /<button id="fileReanalyse"[^>]*>Re-analyse<\/button>\s*(?:<!--[\s\S]*?-->\s*)?<button id="fileTranscribe"[^>]*class="tr-go"[^>]*>Transcribe<\/button>\s*<button id="fileTrCancel"[^>]*hidden>Cancel transcript<\/button>\s*<button id="fileRetranscribe"[^>]*hidden>Re-transcribe<\/button>\s*<span id="stripPairSlot"/.test(html));
  section("U3-2 -- \"Transcribe all\" and the rows' second badge; Cancel all for either batch");
  ok("the transcript's badge beside the translation's (¶ ✓ / ¶ voices 61 % / ¶ 12 % / ¶ queued / ¶ ⚠ / nothing), "
     + "Transcribe all = translatorTranscribeAll{quality}, Cancel all while either batch works, the words count transcripts too",
     batch(runTranslatorWindow()));
  section("U3-3 -- THE TRANSCRIPT TAB: drawn while it shows, a click plays from a turn, a click on a NAME renames that speaker");
  ok("hidden: nothing drawn (the message kept); opened (remembered): its head (speakers, the Word file), a row per turn "
     + "(stamp, NAME, text); a click on a turn plays from its file position; an older generation ignored; \"older\" said; "
     + "Save .docx… = translatorTranscriptSave", tabDrawn(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("RENAME in place: a field with the name; a control character or more than 64 bytes refused UNDER it (nothing sent, the field "
     + "kept); Enter saves (trimmed: translatorSpeakerName{jobId, id, label}); Esc cancels; a click on the name never seeks",
     rename(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("while it is analysed, the tab's \"Analysing… (n %)\" is the strip's figure and follows it at every progress report (the "
     + "transcript itself is posted only when its text grows)", tabPct(runTranslatorWindow({ width: 1280, height: 800 })));
  section("U3-4 -- the Export menu's two Word files; a narrow window; the contract, the bridge, the markup, the theme");
  ok("Export: \"Bilingual subtitles (.docx)\" (the old Word file, its own name) and \"Transcript (.docx)\" = the app's Save",
     exportTwo(runTranslatorWindow()));
  ok("420 x 320: both action slots keep their words, the models move into ⋯ (\"Models: …\"), with the transcript's Cancel",
     narrow(runTranslatorWindow({ width: 420, height: 320 })));
  {
    const u3 = script.slice(script.indexOf("// ---- U3: THE TRANSCRIBE SLOT"), script.indexOf("function exportItems()"));
    ok("the transcript's code builds no markup (textContent only)", u3.length > 4000 && !MARKUP_SINK.test(codeOnly(u3)));
    const contract = ["translatorTranscribe{force", "translatorTranscriptCancel{jobId}", "translatorTranscribeAll{quality}",
                      "translatorTranscriptSave{}", "translatorSpeakerName{jobId, id, label}", "translatorTranscript{jobId",
                      ".tphase", "tstate"];
    ok("the page's contract block names every U3 message", contract.every((c) => html.indexOf(c) >= 0),
       contract.filter((c) => html.indexOf(c) < 0));
    const doors = ["translatorTranscribe", "translatorTranscriptCancel", "translatorTranscribeAll", "translatorTranscriptSave",
                   "translatorSpeakerName"];
    ok("the app handles every message the page sends for it (bridge.cpp's doors)",
       doors.every((d) => bridgeSrc.indexOf('type == L"' + d + '"') >= 0), doors.filter((d) => bridgeSrc.indexOf('type == L"' + d + '"') < 0));
    const css = (html.match(/\.q-tbadge[^{]*\{[^}]*\}|\.tx-[a-z-]+[^{]*\{[^}]*\}/g) || []).join("\n");
    ok("both themes: the transcript's CSS takes its colours from the theme's tokens only (var(--…))",
       css.length > 800 && !/#[0-9a-fA-F]{3,8}\b/.test(css) && !/rgb\(/.test(css), css.length);
  }
  section("FX -- the review's fix round (02/11): Cancel all = the list's work only (F7, F3), each row's Word file (F6), a rename names its own transcript (F8), one Word file per sound track (F2)");
  ok("FX-F7: Cancel all HIDDEN for a file pressed alone (analysing, not the list's), shown while a row is the list's (`batch`), hidden "
     + "again when it leaves the list", fxCancelRule(runTranslatorWindow()));
  ok("FX-F3: Cancel all's tooltip says U2's rule: what Translate all and Transcribe all started (transcripts too); a file pressed "
     + "yourself goes on", fxCancelTip(runTranslatorWindow()));
  ok("FX-F6: a transcribed row says where its Word file is when not next to it (\"Word file in Documents\\SuVtitle Transcripts\"), or "
     + "why there is none -- a line, whole in its tooltip; the ¶ tooltip says \"next to it\" only when it is; a playerItem moves it",
     fxRowDoc(runTranslatorWindow()));
  ok("FX-F8: Enter sends the jobId and vrev of the transcript the field was OPENED on (not the strip's generation of the moment)",
     fxRenameOpenTime(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("FX-F8: another file starts while a name is being typed: the field closes, NOTHING sent, the toast says the name was not kept",
     fxRenameNewFile(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("FX-F8: a Re-transcribe ends while a name is being typed (a new vrev): the field closes, nothing sent; the same transcript "
     + "posted again keeps it open", fxRenameRenumbered(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("FX-F2: this sound track's Word file holds another analysis' transcript: the tab says so in one sentence (kept; Save .docx… "
     + "writes this one to a new file), never \"changed outside SuVtitle\", no Word file of its own named",
     fxOther(runTranslatorWindow({ width: 1280, height: 800 })));
  section("FX2 -- the round after the user's test (03/11): Transcribe all's notice is information (`info`) for the page too");
  ok("FX2: an `info` notice (Transcribe all's) while the film's subtitle track is being extracted: the request KEPT (still chosen, "
     + "shown when it comes); the toast says the notice, not in red", fx2InfoKeepsTrack(runTranslatorWindow()));
  ok("FX2: ...while a subtitle FILE asked is not here yet: the request KEPT (the file shown when it comes)",
     fx2InfoKeepsFile(runTranslatorWindow()));
  ok("FX2: Transcribe all's words: \"1 transcript queued / done / without names / failed\" (one), \"2 transcripts ...\" (more); "
     + "\"recognising voices\" unchanged", fx2Words(runTranslatorWindow()));

  // ---- MUTATION SENTINELS --------------------------------------------------------------------------------------------------------
  section("U3 -- mutation sentinels (index.html with one U3 rule taken out each)");
  {
    const judge = (name, mutate, probe, opts) => {
      if (mutate(script) === script) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow(Object.assign({ mutate, name }, opts || {})); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true, threw = "";
      try { healthy = !!probe(w); } catch (e) { threw = String((e && e.message) || e); }
      if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    const once = (a, b) => (s) => (s.split(a).length === 2 ? s.split(a).join(b) : s);   // (applies exactly once, else not at all)
    const wide = { width: 1280, height: 800 };
    judge("U3-1: Transcribe sends nothing", once("    send({ type: \"translatorTranscribe\", force: false, quality: fileQuality });\n", ""), slotIdle);
    judge("U3-1: its Cancel never shown", once("    el.fileTrCancel.hidden = quiet || !run;", "    el.fileTrCancel.hidden = true;"), slotRunning);
    judge("U3-1: its Cancel cancels the TRANSLATION", once("send({ type: \"translatorTranscriptCancel\", jobId: F.jobId })",
      "send({ type: \"translatorFileCancel\", jobId: F.jobId })"), slotRunning);
    judge("U3-1: Translate's slot ignores its own wish (an analysis the transcript runs shows Translate's Cancel)",
      once("    const run = (fileRunning() || p === \"live\") && F.translate;", "    const run = fileRunning() || p === \"live\";"), slotRunning);
    judge("U3-1: Re-transcribe re-analyses nothing new (force false)",
      once("send({ type: \"translatorTranscribe\", force: true, quality: fileQuality })", "send({ type: \"translatorTranscribe\", force: false, quality: fileQuality })"), slotDone);
    judge("U3-1: the transcript's phase never read", once("tphase: String(msg.tphase || \"off\")", "tphase: \"off\""), slotDone);
    judge("U3-2: the second badge never painted", once("      tb.textContent = tbadgeText(ts, it.tpct);", "      tb.textContent = \"\";"), batch);
    judge("U3-2: Transcribe all sends nothing", once("  btn(el.qTranscribeAll, () => send({ type: \"translatorTranscribeAll\", quality: fileQuality }));", ""), batch);
    judge("U3-2: a row's update forgets the transcript", once("    if (msg.tstate !== undefined) { it.tstate = String(msg.tstate || \"\"); it.tpct = Number(msg.tpct) || 0; }", ""), batch);
    judge("U3-3: the tab draws while hidden", once("    if (txShown()) renderTranscript();\n  }\n  // the head line", "    renderTranscript();\n  }\n  // the head line"), tabDrawn, wide);
    judge("U3-3: an older generation's transcript drawn", once("    if (j < F.jobId) return;                   // an older generation's (the file left)", ""), tabDrawn, wide);
    judge("U3-3: a click on a turn plays nothing", once("      row.addEventListener(\"click\", () => seekTo(t.s));", ""), tabDrawn, wide);
    judge("U3-3: the tab is never remembered", once("    const t = tab === \"queue\" || tab === \"transcript\" ? tab : \"subs\";", "    const t = tab === \"queue\" ? \"queue\" : \"subs\";"), tabDrawn, wide);
    judge("U3-3: a name refused for nothing (no check)", once("        if (why && !blurred) { note.textContent = why; return; }", ""), rename, wide);
    judge("U3-3: the tab's figure is the transcript post's (behind the strip's)", once("    const pct = TX.state === \"analysing\" && F.jobId === TX.jobId && F.totalMs > 0\n      ? Math.min(100, Math.floor(F.doneMs * 100 / F.totalMs)) : TX.pct;", "    const pct = TX.pct;"), tabPct, wide);
    judge("U3-3: the tab's figure follows the transcript posts only", once("    if (TX.state === \"analysing\" && txShown() && !txEdit) {", "    if (false) {"), tabPct, wide);
    judge("U3-3: Esc saves", once("end(false, false); }", "end(true, false); }"), rename, wide);
    judge("U3-3: a click on a name also seeks", once("txRename(row, nb, t); });", "txRename(row, nb, t); seekTo(t.s); });"), rename, wide);
    judge("U3-4: \"Transcript (.docx)\" exports the bilingual file", once("        if (format === \"transcript\") send({ type: \"translatorTranscriptSave\" });\n        else ", "        "), exportTwo);
    judge("U3-4: no models in ⋯ when narrow", once("      if (F.phase !== \"idle\") items.push([\"Models: \"", "      if (false) items.push([\"Models: \""), narrow, { width: 420, height: 320 });
    // ---- FX (02/11/2026): each rule of the fix round taken out -----------------------------------------------------------------
    const judgeMarkup = (name, mutateHtml, probe, opts) => {
      const m = mutateHtml(html);
      if (m === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow(Object.assign({ html: m }, opts || {})); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true;
      try { healthy = !!probe(w); } catch (e) { healthy = true; }
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    judge("FX F7: Cancel all back on U1's rule (any busy row: a pressed file shows it)", once("    el.qCancelAll.hidden = !Q.items.some((x) => x.batch);",
      "    el.qCancelAll.hidden = Q.items.filter((x) => x.state === \"analysing\" || x.state === \"queued\").length === 0;"), fxCancelRule);
    judge("FX F7: a row's `batch` never follows a playerItem", once("    if (msg.batch !== undefined) it.batch = msg.batch === true;", ""), fxCancelRule);
    judge("FX F7: the list's `batch` never read", once("        batch: x.batch === true, tdoc:", "        batch: false, tdoc:"), fxCancelRule);
    judgeMarkup("FX F3: U1's tooltip back (\"Stop every analysis\")", once("title=\"Stop what Translate all and Transcribe all started: the files still waiting are neither analysed nor transcribed, and the list's analysis or voice pass running now stops (what is done is kept). A file you pressed Translate or Transcribe on yourself goes on.\"",
      "title=\"Stop every analysis: the files still waiting are not analysed (what is done is kept)\""), fxCancelTip);
    judge("FX F6: the row's line never drawn", once("        main.appendChild(doc);\n", ""), fxRowDoc);
    judge("FX F6: \"next to it\" whatever the app says", once("(it.tdoc ? \" -- \" + cleanNotice(it.tdoc) : \" (its Word file next to it)\")", "\" (its Word file next to it)\""), fxRowDoc);
    judge("FX F6: a row's tdoc never follows a playerItem", once("    if (msg.tdoc !== undefined) it.tdoc = String(msg.tdoc || \"\");", ""), fxRowDoc);
    judge("FX F8: Enter sends the strip's generation of the moment", once("jobId: at.jobId, id: t.spk", "jobId: F.jobId, id: t.spk"), fxRenameOpenTime, wide);
    judge("FX F8: no vrev sent", once(", label: v.trim(), vrev: at.vrev });", ", label: v.trim() });"), fxRenameOpenTime, wide);
    judge("FX F8: the field outlives a new generation (another file)", once("    txDrop();                                  // (FX F8: a name field of the older transcript closes, nothing sent)\n", ""), fxRenameNewFile, wide);
    judge("FX F8: the field outlives a renumbering (vrev ignored)", once("if (txEdit && (txEdit.jobId !== TX.jobId || txEdit.vrev !== TX.vrev)) txDrop();", "if (txEdit && txEdit.jobId !== TX.jobId) txDrop();"), fxRenameRenumbered, wide);
    judge("FX F8: a dropped name never said", once("    if (changed) showFileToast(", "    if (false) showFileToast("), fxRenameNewFile, wide);
    judge("FX F2: the tab never says another analysis holds the track's Word file", once("    if (TX.other) out.push(", "    if (false) out.push("), fxOther, wide);
    judge("FX F2: `other` never read", once("vrev: Number(msg.vrev) || 0, other: !!msg.other,", "vrev: Number(msg.vrev) || 0, other: false,"), fxOther, wide);
    // ---- FX2 (03/11/2026): the page's rule for an `info` notice, now that the core says Transcribe all's with it ----------------
    judge("FX2: an info notice drops the track being extracted", once("if (!msg.info) { wantExt = 0; wantEmbId = 0; }", "{ wantExt = 0; wantEmbId = 0; }"), fx2InfoKeepsTrack);
    judge("FX2: an info notice drops the file asked", once("      else if (!msg.info && wantExt && wantExt !== wantEmbId) { wantExt = 0; repaintCc(); }",
      "      else if (wantExt && wantExt !== wantEmbId) { wantExt = 0; repaintCc(); }"), fx2InfoKeepsFile);
    judge("FX2: an info notice shown as a failure (red)", once("      showFileToast(cleanNotice(text), !msg.info);", "      showFileToast(cleanNotice(text), true);"), fx2InfoKeepsTrack);
    judge("FX2: the plural back (\"1 transcripts done\")", once(".map(([k, one, more]) => k + \" \" + (k === 1 ? one : more));",
      ".map(([k, one, more]) => k + \" \" + more);"), fx2Words);
    judge("FX2: the singular for every count (\"2 transcript done\")", once(".map(([k, one, more]) => k + \" \" + (k === 1 ? one : more));",
      ".map(([k, one, more]) => k + \" \" + one);"), fx2Words);
  }
};

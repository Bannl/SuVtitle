// test_u2.js -- WAVE U2 (1.1.0, 02/11/2026): "Import model…" -- the user's own AI models in the PLAYER (docs\PROGRESS_U2.md).
// Required by test_ui.js (same ok/section counters). Every check runs the REAL page (app/web/index.html) through run_ui.js, and
// each rule has a MUTATION SENTINEL at the end: the page with that rule taken out must FAIL its probe. The app's half (TrPlayer.h:
// the FILE names the models, their own cache key, the fall back; the user's Cancel-all rule) is RUN by tests\cpp\player_smoke.cpp
// [U2] (sentinels: tests\cpp\cmut_u2.py), the bridge's (the dialog, app.txt, no path from the page) PINNED by
// tests\web\test_host_pins.js (U2-1..U2-3).
//   1 ONE compact models button (U1's one-option <select> gave way to it): the transcription and the translation model in use,
//     short; its menu: Transcription (large-v3-turbo (default) + the imported ones), Translation (Qwen3-4B (default) + the
//     imported ones), a file that is gone greyed "— not found" and not choosable, then "Import model…".
//   2 the page never names a file: a pick is translatorModelPick{kind, id}, an import translatorModelImport{} (the app's dialog).
//   3 the ONE node moves with the pair: the drop zone while nothing is loaded, next to Translate in the strip.
//   4 the subtitles on screen made with other models than the ones chosen now: the strip's banner says so (modelsStale) and
//     its button asks for the chosen models' analysis (translatorFileAnalyse{force:false}).
// U2b (02/11/2026, docs\PROGRESS_U2b.md -- the user: remove an imported model "avec une boîte de dialogue qui ... demande si on
// supprime le model en plus"; a batch file whose imported model failed is redone ONCE with the default; G1 / G2):
//   5 the × on each IMPORTED row (a gone file's too), never on SuVtitle's own; disabled while an analysis uses it (inUse).
//   6 the dialog: the file named (kind · size · folder), the Recycle Bin said, Remove from the list / Remove and delete the file /
//     Cancel; the focus on Cancel, no default; Esc, the backdrop = Cancel; Tab stays inside; no key reaches the player; a gone
//     file: the list only; translatorModelRemove{kind, id, recycle} -- never a path; the app's list moving under it closes it.
//   7 the strip and the rows say a batch's redo ("redone with the default model"); the Transcript tab says why its Word file
//     could not follow a rename (docxErr) and that a file changed outside SuVtitle is left as it is (edited).
"use strict";
module.exports = function u2Checks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, html, script, MARKUP_SINK, codeOnly, bridgeSrc } = ctx;
  const J = JSON.stringify;
  const sent = (w, t) => w.outbox.filter((m) => m && m.type === t);
  const rows = (w) => w.ids.modelsMenu.children;
  // (U2b re-point: an IMPORTED model's row is a .models-row -- its entry + its × -- the entry is read through it; "[x]" marks the
  //  ×, "[x-]" a disabled one)
  const entryOf = (r) => (r && r.className === "models-row" ? r.children[0] : r);
  const rowText = (w) => rows(w).map((r) => {
    const e = entryOf(r);
    return (e.className === "pop-title" ? "#" : "") + e.textContent + (e.classList && e.classList.contains("is-on") ? "*" : "")
      + (e.disabled ? "!" : "")
      + (r.className === "models-row" ? (r.children[1] ? "[x" + (r.children[1].disabled ? "-" : "") + "]" : "[none]") : "");
  }).join("|");
  const MODELS = {
    type: "translatorModels",
    asr: { sel: 2, items: [{ id: 0, name: "large-v3-turbo", def: true, found: true },
                           { id: 1, name: "ggml-medium.bin", path: "D:\\Models\\ggml-medium.bin", found: false },
                           { id: 2, name: "ggml-base.bin", path: "D:\\Models\\ggml-base.bin", found: true }] },
    mt:  { sel: 0, items: [{ id: 0, name: "Qwen3-4B", def: true, found: true },
                           { id: 1, name: "gemma-3-4b-it-Q4_K_M.gguf", path: "E:\\LLM\\gemma-3-4b-it-Q4_K_M.gguf", found: true }] },
  };

  // ---- the probes (each a check here AND the condition its sentinel must see fail) ----------------------------------------
  // 1a: before the app's first word: SuVtitle's own two, on the button and in the menu; the menu ends with "Import model…",
  //     which asks the APP for its dialog (nothing else: no path); a second click on the button closes it
  const defaultsShown = (w) => {
    const btn = w.ids.modelsBtn.textContent === "large-v3-turbo·Qwen3-4B" && w.ids.modelsMenu.hidden === true
      && /Transcription: large-v3-turbo/.test(w.ids.modelsBtn.title) && /Translation: Qwen3-4B/.test(w.ids.modelsBtn.title);
    w.ids.modelsBtn.click();
    const open = w.ids.modelsMenu.hidden === false && w.ids.modelsBtn.getAttribute("aria-expanded") === "true"
      && rowText(w) === "#Transcription|large-v3-turbo (default)*|#Translation|Qwen3-4B (default)*||Import model…";
    w.ids.modelsBtn.click();
    const closed = w.ids.modelsMenu.hidden === true && w.ids.modelsBtn.getAttribute("aria-expanded") === "false";
    w.ids.modelsBtn.click();
    w.outbox.length = 0;
    const imp = rows(w)[rows(w).length - 1];
    imp.click();
    return btn && open && closed && imp.textContent === "Import model…" && J(w.outbox) === J([{ type: "translatorModelImport" }])
      && w.ids.modelsMenu.hidden === true;
  };
  // 1b: the app's list: the one in use on the button (short); the menu -- the imported ones under their kind, the one in use
  //     checked, a file that is gone greyed "— not found" (not choosable), each row's tooltip its path; a pick = {kind, id}
  const listShown = (w) => {
    w.send(MODELS);
    const btn = w.ids.modelsBtn.textContent === "base·Qwen3-4B" && /Transcription: ggml-base\.bin/.test(w.ids.modelsBtn.title);
    w.ids.modelsBtn.click();
    // (U2b re-point: each imported row carries its ×)
    const menu = rowText(w) === "#Transcription|large-v3-turbo (default)|ggml-medium.bin — not found![x]|ggml-base.bin*[x]"
      + "|#Translation|Qwen3-4B (default)*|gemma-3-4b-it-Q4_K_M.gguf[x]||Import model…";
    const row = (i) => entryOf(rows(w)[i]) || { title: "", getAttribute: () => null, click: () => {} };   // (a mutant may build fewer rows)
    const tip = row(6).title === "E:\\LLM\\gemma-3-4b-it-Q4_K_M.gguf" && row(3).getAttribute("aria-checked") === "true";
    w.outbox.length = 0;
    row(2).click();                                      // gone: nothing
    w.ids.modelsBtn.click();
    row(3).click();                                      // the one in use: nothing
    w.ids.modelsBtn.click();
    row(6).click();                                      // gemma: picked
    return btn && menu && tip && J(w.outbox) === J([{ type: "translatorModelPick", kind: "mt", id: 1 }]) && w.ids.modelsMenu.hidden;
  };
  // 1c: the app says the chosen file is gone (sel names a row not found): SuVtitle's own is shown in use
  const goneNotInUse = (w) => {
    w.send(Object.assign({}, MODELS, { asr: { sel: 1, items: MODELS.asr.items } }));
    w.ids.modelsBtn.click();
    return w.ids.modelsAsr.textContent === "large-v3-turbo" && rows(w)[1].classList.contains("is-on")
      && !entryOf(rows(w)[2]).classList.contains("is-on");   // (U2b re-point: the gone file's entry, inside its row)
  };
  // 3: ONE node: the drop zone with nothing loaded, the strip next to the pair once a file is there, back when it goes
  const moved = (w) => {
    const node = w.ids.trModels;
    const empty = node.parentNode === w.ids.emptyModelSlot;
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    const strip = node.parentNode === w.ids.stripModelSlot && w.ids.stripModelSlot.parentNode === w.ids.stripPairSlot.parentNode;
    w.send({ type: "translatorFile", jobId: 3, seq: 0, phase: "idle" });
    return empty && strip && node.parentNode === w.ids.emptyModelSlot && w.ids.trModels === node;
  };
  // 4: the subtitles shown were made with other models (modelsStale): the banner says the MODEL changed (the languages win
  //    when they moved too) and its button asks for the chosen models' analysis
  const staleSaid = (w) => {
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "zh" });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, translate: false, count: 1, doneMs: 600000, ranges: [[0, 600000]] }));
    w.send(cueMsg(2, 0, 1000, 5000, "en", "zh", "Hello", "你好"));
    metaOk(w.ids.fileVideo);
    const fresh = w.ids.fileStale.hidden === true;
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, translate: false, count: 1, doneMs: 600000, ranges: [[0, 600000]], modelsStale: true }));
    const said = w.ids.fileStale.hidden === false && w.ids.fileStaleText.textContent === "Model changed since this analysis";
    w.outbox.length = 0;
    w.ids.fileStaleBtn.click();
    const asks = J(w.outbox) === J([{ type: "translatorFileAnalyse", force: false, quality: "auto" }]);
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "fr" });
    const both = w.ids.fileStale.hidden === false && w.ids.fileStaleText.textContent === "Languages changed since this analysis";
    return fresh && said && asks && both;
  };

  // ---- U2b probes (02/11/2026) -------------------------------------------------------------------------------------------
  const MODELS_B = {
    type: "translatorModels",
    asr: { sel: 2, items: [{ id: 0, name: "large-v3-turbo", def: true, found: true },
                           { id: 1, name: "ggml-medium.bin", path: "D:\\Models\\ggml-medium.bin", found: false, size: 0 },
                           { id: 2, name: "ggml-base.bin", path: "D:\\Models\\ggml-base.bin", found: true, size: 147951465 }] },
    mt:  { sel: 1, items: [{ id: 0, name: "Qwen3-4B", def: true, found: true },
                           { id: 1, name: "gemma-3-4b-it-Q4_K_M.gguf", path: "E:\\LLM\\gemma-3-4b-it-Q4_K_M.gguf", found: true,
                             size: 2489757856, inUse: true }] },
  };
  const copyOf = (o) => JSON.parse(J(o));
  const xOf = (w, i) => { const r = rows(w)[i]; return r && r.className === "models-row" ? r.children[1] : null; };
  const dlgShown = (w) => w.ids.modelRemove.hidden === false && w.ids.modelRemoveBack.hidden === false;
  const dlgText = (w) => [w.ids.mrName.textContent, w.ids.mrWhat.textContent, w.ids.mrBin.hidden ? "-" : "bin",
    w.ids.mrBusy.hidden ? "-" : "busy", w.ids.mrList.hidden ? "-" : (w.ids.mrList.disabled ? "list!" : "list"),
    w.ids.mrDelete.hidden ? "-" : (w.ids.mrDelete.disabled ? "delete!" : "delete")].join("|");
  const removes = (w) => w.outbox.filter((m) => m && m.type === "translatorModelRemove");
  // 5: the × on the IMPORTED rows only (a gone file's too), none on SuVtitle's own; the one an analysis uses disabled -- its
  //    tooltip says why, a click does nothing
  const xOnImported = (w) => {
    w.send(MODELS_B);
    w.ids.modelsBtn.click();
    const shape = rowText(w) === "#Transcription|large-v3-turbo (default)|ggml-medium.bin — not found![x]|ggml-base.bin*[x]"
      + "|#Translation|Qwen3-4B (default)|gemma-3-4b-it-Q4_K_M.gguf*[x-]||Import model…";
    const busyTip = !!xOf(w, 6) && /being used by an analysis/.test(xOf(w, 6).title);
    const label = !!xOf(w, 3) && xOf(w, 3).getAttribute("aria-label") === "Remove ggml-base.bin…";
    w.outbox.length = 0;
    if (xOf(w, 6)) xOf(w, 6).click();
    return shape && busyTip && label && !dlgShown(w) && w.outbox.length === 0;
  };
  // 6a: the × of a model that is there: the dialog -- its name (its path in the tooltip), its kind · size · folder, the Recycle
  //     Bin said, three buttons, the focus on CANCEL (no default), the menu closed; "Remove from the list" = translatorModelRemove
  //     {kind, id, recycle:false} -- no path; closed, the focus back on the models button
  const dialogList = (w) => {
    w.send(MODELS_B);
    w.ids.modelsBtn.click();
    xOf(w, 3).click();
    const shown = dlgShown(w) && w.ids.modelsMenu.hidden === true
      && dlgText(w) === "ggml-base.bin|Transcription model · 141 MB · in D:\\Models|bin|-|list|delete"
      && w.doc.activeElement === w.ids.mrCancel && w.ids.mrName.title === "D:\\Models\\ggml-base.bin";
    w.outbox.length = 0;
    w.ids.mrList.click();
    return shown && J(w.outbox) === J([{ type: "translatorModelRemove", kind: "asr", id: 2, recycle: false }]) && !dlgShown(w)
      && w.doc.activeElement === w.ids.modelsBtn;
  };
  // 6b: "Remove and delete the file" = {kind, id, recycle:true} (the app sends the file to the Recycle Bin)
  const dialogDelete = (w) => {
    const m = copyOf(MODELS_B);
    m.mt.items[1].inUse = false;
    w.send(m);
    w.ids.modelsBtn.click();
    xOf(w, 6).click();
    const text = dlgText(w) === "gemma-3-4b-it-Q4_K_M.gguf|Translation model · 2.3 GB · in E:\\LLM|bin|-|list|delete";
    w.outbox.length = 0;
    w.ids.mrDelete.click();
    return text && J(w.outbox) === J([{ type: "translatorModelRemove", kind: "mt", id: 1, recycle: true }]) && !dlgShown(w);
  };
  // 6c: Cancel, Esc and the backdrop send NOTHING; Tab stays among its buttons (both ways); a key while it is up never reaches the
  //     player under it (Space does not play)
  const dialogCancel = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, translate: false, count: 1, doneMs: 600000, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.send(MODELS_B);
    w.outbox.length = 0;
    const open = () => { w.ids.modelsBtn.click(); if (xOf(w, 3)) xOf(w, 3).click(); return dlgShown(w); };
    const o1 = open(); w.ids.mrCancel.click(); const c1 = !dlgShown(w);
    const o2 = open(); w.win.dispatch("keydown", { key: "Escape" }); const c2 = !dlgShown(w);
    const o3 = open(); w.ids.modelRemoveBack.click(); const c3 = !dlgShown(w);
    const o4 = open();
    w.win.dispatch("keydown", { key: "Tab" });
    const t1 = w.doc.activeElement === w.ids.mrList;               // (from Cancel, the last: round to the first)
    w.win.dispatch("keydown", { key: "Tab", shiftKey: true });
    const t2 = w.doc.activeElement === w.ids.mrCancel;
    const played = w.ids.fileVideo.playCalls || 0, paused = w.ids.fileVideo.paused;
    w.win.dispatch("keydown", { key: " ", code: "Space" });
    const noPlayer = (w.ids.fileVideo.playCalls || 0) === played && w.ids.fileVideo.paused === paused && dlgShown(w);
    w.win.dispatch("keydown", { key: "Escape" });
    return o1 && c1 && o2 && c2 && o3 && c3 && o4 && t1 && t2 && noPlayer && !dlgShown(w) && removes(w).length === 0;
  };
  // 6d: a file that is GONE: the list only -- no delete button, no Recycle Bin line; its text says where it was
  const dialogGone = (w) => {
    w.send(MODELS_B);
    w.ids.modelsBtn.click();
    xOf(w, 2).click();
    const text = dlgText(w) === "ggml-medium.bin|Transcription model · the file is not found any more (it was in D:\\Models)|-|-|list|-";
    w.outbox.length = 0;
    w.ids.mrDelete.click();                                      // (hidden -- a script's click still sends nothing)
    const none = w.outbox.length === 0 && dlgShown(w);
    w.ids.mrList.click();
    return text && none && J(w.outbox) === J([{ type: "translatorModelRemove", kind: "asr", id: 1, recycle: false }]);
  };
  // 6e: the app's list moving under the open dialog: its model now IN USE -> both buttons disabled, said; another file under its
  //     id (or none) -> the dialog closes, nothing sent
  const dialogFollows = (w) => {
    w.send(MODELS_B);
    w.ids.modelsBtn.click();
    xOf(w, 3).click();
    const busyMsg = copyOf(MODELS_B);
    busyMsg.asr.items[2].inUse = true;
    w.send(busyMsg);
    const busy = dlgShown(w) && /\|bin\|busy\|list!\|delete!$/.test(dlgText(w));
    w.outbox.length = 0;
    w.ids.mrList.click();
    w.ids.mrDelete.click();
    const nothing = w.outbox.length === 0 && dlgShown(w);
    const moved = copyOf(MODELS_B);
    moved.asr.items[2].path = "D:\\Other\\ggml-base.bin";
    w.send(moved);
    return busy && nothing && !dlgShown(w) && w.outbox.length === 0;
  };
  // 7a: the strip says a batch's REDO (loading / analysing / done: "· redone with the default model"), the whole sentence in its
  //     tooltip; nothing said without it
  const REDO = "The transcription model \"x.bin\" could not be used: it would not start. Redone with SuVtitle's own transcription "
    + "model (large-v3-turbo).";
  const redoSaid = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true, doneMs: 60000, totalMs: 600000 }));
    const plain = !/redone/.test(w.ids.fileStatus.textContent);
    w.send(fileMsg({ jobId: 3, phase: "analyzing", translate: true, doneMs: 60000, totalMs: 600000, redo: REDO }));
    const said = /· redone with the default model$/.test(w.ids.fileStatus.textContent) && w.ids.fileStatus.title.indexOf(REDO) >= 0;
    w.send(fileMsg({ jobId: 3, phase: "done", complete: true, translate: false, count: 1, doneMs: 600000, ranges: [[0, 600000]], redo: REDO }));
    return plain && said && /redone with the default model/.test(w.ids.fileStatus.textContent);
  };
  // 7b: the rows: a file a batch REDID says so under its name; the others nothing; a playerItem moves it
  const rowRedo = (w) => {
    w.send({ type: "playerQueue", items: [{ id: 1, name: "a.mp4", dir: "Films", state: "analysing", pct: 10, redo: true },
                                          { id: 2, name: "b.mp4", dir: "Films", state: "queued", pct: 0 }], cur: 0, loaded: false });
    const qrows = w.ids.queueList.children;
    const note = (r) => (r && r.children[2] ? r.children[2].children.find((c) => c.className === "q-redo") || null : null);
    const first = !!note(qrows[0]) && note(qrows[0]).textContent === "Redone with the default model" && !note(qrows[1]);
    w.send({ type: "playerItem", id: 2, state: "analysing", pct: 5, redo: true });
    const second = !!note(qrows[1]);
    w.send({ type: "playerItem", id: 1, state: "ready", pct: 100, redo: false });
    return first && second && !note(qrows[0]);
  };
  // 7c (G2): the Transcript tab -- a Word file that could not follow a rename says why (with "older"); one changed outside SuVtitle
  //     says it is left as it is; the reason never said once it is current
  const tabG2 = (w) => {
    w.send(fileMsg({ jobId: 4, phase: "done", complete: true, translate: false, count: 1, doneMs: 600000, ranges: [[0, 600000]] }));
    w.ids.tabTranscript.click();
    const base = { type: "translatorTranscript", jobId: 4, state: "done", pct: 100, speakers: 1, names: [], langs: ["en"],
                   complete: true, file: "a - transcript.docx",
                   turns: [{ s: 0, e: 1000, spk: 1, name: "Speaker 1", tc: "[00:00:00]", text: "Hi." }] };
    const LOCK = "The Word file is open in another program: close it to update it.";
    w.send(Object.assign({}, base, { older: true, docxErr: LOCK }));
    const locked = /older than this transcript/.test(w.ids.txState.textContent) && w.ids.txState.textContent.indexOf(LOCK) >= 0;
    w.send(Object.assign({}, base, { older: false, edited: true }));
    const edited = /changed outside SuVtitle: it is left as it is/.test(w.ids.txState.textContent)
      && w.ids.txState.textContent.indexOf(LOCK) < 0;
    w.send(Object.assign({}, base, { older: false, docxErr: LOCK }));
    return locked && edited && w.ids.txState.textContent.indexOf(LOCK) < 0;
  };
  // G1 (the user's answer 02/11): each Cancel says WHICH work it stops, always -- one running or both; the tooltips too
  const cancelWords = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true, transcribe: false, doneMs: 60000, totalMs: 600000 }));
    const one = !w.ids.fileCancel.hidden && w.ids.fileCancel.textContent === "Cancel translation" && w.ids.fileTrCancel.hidden;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true, transcribe: true, tphase: "waiting", doneMs: 60000, totalMs: 600000 }));
    const both = w.ids.fileCancel.textContent === "Cancel translation" && !w.ids.fileTrCancel.hidden
      && w.ids.fileTrCancel.textContent === "Cancel transcript";
    return one && both && /translation/.test(w.ids.fileCancel.title) && /transcript/.test(w.ids.fileTrCancel.title);
  };
  // G1: a narrow window's ⋯ -- the same words
  const cancelWordsNarrow = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true, transcribe: true, tphase: "waiting", doneMs: 60000, totalMs: 600000 }));
    w.ids.tMore.click();
    const labels = w.ids.morePanel.children.map((b) => b.textContent);
    return labels.indexOf("Cancel translation") >= 0 && labels.indexOf("Cancel transcript") >= 0
      && !labels.some((l) => /^Cancel the /.test(l));
  };

  section("U2-1 -- ONE compact models button and its menu: SuVtitle's own, the imported ones of each kind, \"Import model…\"");
  ok("before the app's word: large-v3-turbo · Qwen3-4B on the button; the menu: Transcription / Translation, each default checked, "
     + "then \"Import model…\" -- which asks the app for its dialog (translatorModelImport{}, no path); the button toggles it",
     defaultsShown(runTranslatorWindow()));
  ok("the app's list: the model in use on the button (short); in the menu the imported ones under their kind, the one in use "
     + "checked, a file that is gone greyed \"— not found\" and not choosable, each row's tooltip its path; a pick sends {kind, id}",
     listShown(runTranslatorWindow()));
  ok("the app names a gone file as chosen: SuVtitle's own is shown in use, never the gone one", goneNotInUse(runTranslatorWindow()));
  section("U2-2 -- the page names no file: a pick is an id the app minted, an import the app's own dialog");
  {
    const code = codeOnly(script);
    const block = code.slice(code.indexOf("const MODEL_DEFAULTS"), code.indexOf("el.modelsBtn.addEventListener(\"click\", toggleModelsMenu);"));
    ok("the models code builds no markup (textContent only) and sends only translatorModelPick{kind, id} / translatorModelImport{}",
       block.length > 1500 && !MARKUP_SINK.test(block) && !/innerHTML|\.path\b[^\n]*send|send\([^)]*path/.test(block)
       && /send\(\{ type: "translatorModelPick", kind, id: x\.id \}\)/.test(block) && /send\(\{ type: "translatorModelImport" \}\)/.test(block));
    ok("the app handles both (bridge.cpp) and emits translatorModels; the page's contract block names all three",
       /type == L"translatorModelImport"/.test(bridgeSrc) && /type == L"translatorModelPick"/.test(bridgeSrc)
       && /L"translatorModels"/.test(bridgeSrc) && /translatorModelImport\{\}/.test(html) && /translatorModelPick\{kind/.test(html)
       && /translatorModels\{asr:\{sel, items/.test(html));
    ok("U1's one-option <select id=\"fileQuality\"> is gone; the quality the app is sent stays \"auto\" (QUALITIES = [\"auto\"])",
       !/id="fileQuality"/.test(html) && /const QUALITIES = \["auto"\];/.test(script));
    const css = (html.match(/\/\* U2: THE MODELS[\s\S]*?\.models-menu \.models-import \{[^}]*\}/) || [""])[0];
    ok("both themes: the models' CSS takes its colours from the theme's tokens only (var(--…)); the chevron is the drop zone's",
       css.length > 500 && !/#[0-9a-fA-F]{3,8}\b(?![^"]*%3C)/.test(css.replace(/url\("data:[^"]*"\)/g, "")) && !/rgb\(/.test(css)
       && /var\(--accent\)/.test(css) && /var\(--bg-elev\)/.test(css));
  }
  section("U2-3 -- the ONE models node moves with the pair: the drop zone, the strip next to Translate");
  ok("the drop zone while nothing is loaded, the strip (next to the pair, in Translate's group) once a file is there, back after",
     moved(runTranslatorWindow()));
  section("U2-4 -- subtitles made with other models: the strip says so (modelsStale), its button asks for the chosen models'");
  ok("a film made with the default, an imported model now chosen: \"Model changed since this analysis\" -- its button = "
     + "translatorFileAnalyse{force:false} (the chosen models' cache, else their analysis); the languages moved too: they are said",
     staleSaid(runTranslatorWindow()));

  // ---- U2b (02/11/2026) -------------------------------------------------------------------------------------------------------
  section("U2b-5 -- the × of an IMPORTED model (its removal), never on SuVtitle's own, disabled while an analysis uses it");
  ok("each imported row carries a × (a file that is gone too), SuVtitle's two none; the one an analysis uses: disabled, its tooltip "
     + "says why, a click does nothing", xOnImported(runTranslatorWindow()));
  section("U2b-6 -- the dialog: the file named, its three choices, the Recycle Bin said; Esc / its backdrop = Cancel; no path sent");
  ok("the × opens it: the name (the path as its tooltip), \"Transcription model · 141 MB · in D:\\Models\", the Recycle Bin line, the "
     + "focus on Cancel (no default); Remove from the list = translatorModelRemove{kind, id, recycle:false}; the focus back",
     dialogList(runTranslatorWindow()));
  ok("Remove and delete the file = translatorModelRemove{kind, id, recycle:true} (\"Translation model · 2.3 GB · in E:\\LLM\")",
     dialogDelete(runTranslatorWindow()));
  ok("Cancel, Esc and the backdrop send nothing; Tab stays among its buttons (both ways); no key reaches the player under it",
     dialogCancel(runTranslatorWindow()));
  ok("a file that is GONE: only \"Remove from the list\" (no delete button, no Recycle Bin line), its text says where it was",
     dialogGone(runTranslatorWindow()));
  ok("the app's list moving under it: its model now in use -> both buttons disabled and said; another file under its id -> closed, "
     + "nothing sent", dialogFollows(runTranslatorWindow()));
  {
    const code = codeOnly(script);
    const blk = code.slice(code.indexOf("function fmtBytes"), code.indexOf("}, true);", code.indexOf("function fmtBytes")));
    ok("the removal code builds no markup (textContent only) and sends ONE message: translatorModelRemove{kind, id, recycle} -- "
       + "never a path",
       blk.length > 1500 && !MARKUP_SINK.test(blk) && !/innerHTML/.test(blk) && !/send\([^)]*path/.test(blk)
       && (blk.match(/send\(/g) || []).length === 1
       && /send\(\{ type: "translatorModelRemove", kind: m\.kind, id: m\.id, recycle \}\);/.test(blk));
    ok("the app handles it (bridge.cpp, a kind + an id + a flag, the path from ITS list); the page's contract block names it",
       /type == L"translatorModelRemove"/.test(bridgeSrc) && /handleModelRemove\(kind, id, recycle\);/.test(bridgeSrc)
       && /translatorModelRemove\{kind:"asr"\|"mt", id, recycle\}/.test(html));
    const cssB = (html.match(/\/\* U2b: REMOVING AN IMPORTED MODEL[\s\S]*?\.model-remove \.mr-btns button:disabled \{[^}]*\}/) || [""])[0];
    ok("both themes: the ×'s and the dialog's CSS take their colours from the theme's tokens only (var(--…))",
       cssB.length > 800 && !/#[0-9a-fA-F]{3,8}\b/.test(cssB) && !/rgba?\(/.test(cssB) && /var\(--danger\)/.test(cssB)
       && /var\(--bg\)/.test(cssB));
  }
  section("U2b-7 -- a batch's redo said (the strip, the rows); G2 -- the Transcript tab says why its Word file did not follow");
  ok("the strip: \"· redone with the default model\" while a batch redoes the file and once it is done, the whole sentence in its "
     + "tooltip; nothing without it", redoSaid(runTranslatorWindow()));
  ok("the rows: \"Redone with the default model\" under the name of a file a batch redid (playerQueue / playerItem `redo`)",
     rowRedo(runTranslatorWindow()));
  ok("G2: the tab -- a Word file OPEN elsewhere: why, beside \"older\"; one changed outside SuVtitle: left as it is; the reason "
     + "never said once the file is current", tabG2(runTranslatorWindow()));
  section("U2b G1 -- each Cancel says which work it stops: \"Cancel translation\" / \"Cancel transcript\", always");
  ok("one running or both: \"Cancel translation\" and \"Cancel transcript\" (never two bare \"Cancel\"), their tooltips say which work",
     cancelWords(runTranslatorWindow()));
  ok("a narrow window's ⋯: the same words", cancelWordsNarrow(runTranslatorWindow({ width: 420, height: 320 })));

  // ---- MUTATION SENTINELS --------------------------------------------------------------------------------------------------
  section("U2 -- mutation sentinels (index.html with one U2 rule taken out each)");
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
    judge("U2-1: no \"Import model…\" in the menu", once("    el.modelsMenu.appendChild(imp);\n", ""), defaultsShown);
    judge("U2-1: \"Import model…\" sends nothing", once("closeModelsMenu(); send({ type: \"translatorModelImport\" }); });",
      "closeModelsMenu(); });"), defaultsShown);
    judge("U2-1: the button never closes its menu", once("    if (!el.modelsMenu.hidden) { closeModelsMenu(); return; }\n", ""), defaultsShown);
    judge("U2-1: a pick sends nothing", once("          if (!on && x.found) send({ type: \"translatorModelPick\", kind, id: x.id });\n", ""), listShown);
    judge("U2-1: a gone file choosable", once("        b.disabled = !x.found;", "        b.disabled = false;"), listShown);
    judge("U2-1: a gone file picked anyway", once("          if (!on && x.found) send(", "          if (!on) send("), listShown);
    judge("U2-1: a gone file not said", once(" : x.found ? x.name : x.name + \" — not found\";", " : x.name;"), listShown);
    judge("U2-1: the app's list ignored", once("      applyModels(msg);                   // U2:", "      void msg;                   // U2:"), listShown);
    judge("U2-1: the button never follows the model in use", once("    el.modelsAsr.textContent = a.def ? a.name : shortModel(a.name) || a.name;",
      "    el.modelsAsr.textContent = MODEL_DEFAULTS.asr;"), listShown);
    judge("U2-1: the row in use not checked", once("        b.className = \"cc-src\" + (on ? \" is-on\" : \"\");", "        b.className = \"cc-src\";"), listShown);
    judge("U2-1: a gone file kept in use", once("      models[kind] = { sel: items.some((x) => x.id === sel && x.found) ? sel : 0, items };",
      "      models[kind] = { sel, items };"), goneNotInUse);
    judge("U2-3: the models never move to the strip", once("    if (el.trModels.parentNode !== mslot) mslot.appendChild(el.trModels);\n", ""), moved);
    judge("U2-4: the banner ignores the models", once("    return pairStale() || F.modelsStale;", "    return pairStale();"), staleSaid);
    judge("U2-4: modelsStale never read", once("      modelsStale: !!msg.modelsStale,", "      modelsStale: false,"), staleSaid);
    judge("U2-4: the banner always says the languages", once(": \"Model changed since this analysis\");", ": \"Languages changed since this analysis\");"), staleSaid);
    // ---- U2b ----
    judge("U2b-5: no × on the imported rows", once("        row.appendChild(del);\n", ""), xOnImported);
    judge("U2b-5: a × on SuVtitle's own models too", once("        if (x.def) { el.modelsMenu.appendChild(b); return; }\n", ""), xOnImported);
    judge("U2b-5: a model in use removable", once("        del.disabled = x.inUse;", "        del.disabled = false;"), xOnImported);
    judge("U2b-5: the disabled × opens the dialog anyway", once("          if (del.disabled) return;\n", ""), xOnImported);
    judge("U2b-6: the removal sends the PATH", once("send({ type: \"translatorModelRemove\", kind: m.kind, id: m.id, recycle });",
      "send({ type: \"translatorModelRemove\", kind: m.kind, id: m.id, recycle, path: m.path });"), dialogList);
    judge("U2b-6: \"Remove from the list\" deletes the file", once("  el.mrList.addEventListener(\"click\", () => removeModel(false));",
      "  el.mrList.addEventListener(\"click\", () => removeModel(true));"), dialogList);
    judge("U2b-6: \"Remove and delete the file\" never deletes", once("  el.mrDelete.addEventListener(\"click\", () => removeModel(true));",
      "  el.mrDelete.addEventListener(\"click\", () => removeModel(false));"), dialogDelete);
    judge("U2b-6: the focus starts on the delete button (a default)", once("    try { el.mrCancel.focus(); } catch { /* fine */ }",
      "    try { el.mrDelete.focus(); } catch { /* fine */ }"), dialogList);
    judge("U2b-6: the size not said", once("what + \" · \" + fmtBytes(mr.size) + \" · in \" + folderOf(mr.path)", "what + \" · in \" + folderOf(mr.path)"), dialogList);
    judge("U2b-6: the Recycle Bin not said", once("    el.mrBin.hidden = !mr.found;", "    el.mrBin.hidden = true;"), dialogList);
    judge("U2b-6: Esc does not cancel", once("    if (k === \"Escape\") { if (e.preventDefault) e.preventDefault(); closeModelRemove(); return; }\n", ""), dialogCancel);
    judge("U2b-6: the backdrop does not cancel", once("  el.modelRemoveBack.addEventListener(\"click\", () => closeModelRemove());\n", ""), dialogCancel);
    judge("U2b-6: Tab leaves the dialog", once("    if (k !== \"Tab\") return;\n", "    return;\n"), dialogCancel);
    judge("U2b-6: a key reaches the player under it", once("    ctxKeyEv = e;\n    if (e.stopPropagation) e.stopPropagation();\n    const k = String(e.key || \"\");",
      "    const k = String(e.key || \"\");"), dialogCancel);
    judge("U2b-6: a gone file deletable", once("    el.mrDelete.hidden = !mr.found;", "    el.mrDelete.hidden = false;"), dialogGone);
    judge("U2b-6: a gone file's file \"deleted\" by a script's click", once("    if (!mr || mr.inUse || (recycle && !mr.found)) return;",
      "    if (!mr || mr.inUse) return;"), dialogGone);
    judge("U2b-6: the dialog ignores the app's word (in use)", once("      else { mr.found = x.found; mr.size = x.size; mr.inUse = x.inUse; paintModelRemove(); }",
      "      else { }"), dialogFollows);
    judge("U2b-6: another file under its id keeps the dialog", once("      if (!x || x.def || x.path !== mr.path) closeModelRemove();",
      "      if (!x) closeModelRemove();"), dialogFollows);
    judge("U2b-6: in use, its buttons act", once("    el.mrList.disabled = mr.inUse;\n", "    el.mrList.disabled = false;\n"), dialogFollows);
    judge("U2b-7: the redo never said in the strip", once("    if (redo) text += \" · redone with the default model\";\n", ""), redoSaid);
    judge("U2b-7: translatorFile.redo never read", once("      redo: String(msg.redo || \"\"),", "      redo: \"\","), redoSaid);
    judge("U2b-7: the row never says the redo", once("        main.appendChild(note);\n", ""), rowRedo);
    judge("U2b-7: a row's redo never follows a playerItem", once("    if (msg.redo !== undefined) it.redo = msg.redo === true;", ""), rowRedo);
    judge("U2b G2: the tab never says why the Word file did not follow", once("    if (TX.older && TX.docxErr) out.push(cleanNotice(TX.docxErr));\n", ""), tabG2);
    judge("U2b G2: an edited Word file not said", once("    if (TX.edited) out.push(", "    if (false) out.push("), tabG2);
    judge("U2b G2: the reason said even once current", once("    if (TX.older && TX.docxErr)", "    if (TX.docxErr)"), tabG2);
    // (a MARKUP rule: the whole page mutated, run as it is)
    const judgeMarkup = (name, mutateHtml, probe, opts) => {
      const m = mutateHtml(html);
      if (m === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow(Object.assign({ html: m }, opts || {})); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true;
      try { healthy = !!probe(w); } catch (e) { healthy = true; }
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    judgeMarkup("U2b G1: a bare \"Cancel\" for the translation again", once(">Cancel translation</button>", ">Cancel</button>"), cancelWords);
    judgeMarkup("U2b G1: a bare \"Cancel\" for the transcript again", once(">Cancel transcript</button>", ">Cancel</button>"), cancelWords);
    judge("U2b G1: ⋯ says the old words", once("items.push([\"Cancel translation\"", "items.push([\"Cancel the analysis\""), cancelWordsNarrow,
      { width: 420, height: 320 });
  }
};

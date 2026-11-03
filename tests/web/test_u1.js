// test_u1.js -- WAVE U1 (1.1.0, 02/11/2026): the user's own interface requests for the PLAYER (docs\PROGRESS_U1.md).
// Required by test_ui.js (same ok/section counters). Every check runs the REAL page (app/web/index.html) through run_ui.js,
// and each rule has a MUTATION SENTINEL at the end: the page with that rule taken out must FAIL its probe (a check that
// cannot fail is not a check). The app's half (TrPlayer.h: nothing analysed unless asked, "Translate all", Cancel all) is
// RUN by tests\cpp\player_smoke.cpp [U1] and PINNED by tests\web\test_host_pins.js (U1-1..U1-3).
//   1 Translate is a BUTTON: this file only; a film already analysed shows its subtitles without it; Cancel while it runs,
//     Re-analyse once complete -- one slot, the start of the strip. A file loads as a plain player (nothing asked).
//   2 "Translate all" in the queue panel: each row's state (queued / analysing n% / done / failed / cancelled), Cancel all.
//   3 Live -> "Translate Live" (the button, its tooltips, the notice's link).
//   4 the language pair left the bar: next to Translate in the strip; next to the model in the drop zone (ONE node).
//   5 no Translate switch in the empty state; the model menu keeps ONE option (large-v3-turbo); a stored "max" -> "auto".
//   6 a non-Latin language is followed by its English name ("日本語 (Japanese)") -- never in the export menu.
//   7 the lockup: "Transcription, Translation & Subtitles" right of the name, "by NEXTALE Audio" under it.
"use strict";
module.exports = function u1Checks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, ovLines, last, countType, qMsg, qItem, html, script,
          MARKUP_SINK, codeOnly, bridgeSrc, playerSrc } = ctx;
  const J = JSON.stringify;
  const pair = (w, a, b, mode) => w.send({ type: "translatorState", running: true, status: "listening", mode: mode || "auto", langA: a, langB: b });
  const sent = (w, t) => w.outbox.filter((m) => m && m.type === t);
  const ANALYSIS = ["translatorFileAnalyse", "translatorFileAnalyseAll", "translatorFileCancel", "translatorFileCancelAll"];
  const slot = (w) => [w.ids.fileTranslate, w.ids.fileCancel, w.ids.fileReanalyse].map((b) => (b.hidden ? "-" : b.textContent)).join("|");
  const overlay = (w) => ovLines(w).map((l) => l.text).join(" / ");
  const badgeOf = (w, i) => w.ids.queueList.children[i].children[3];

  // ---- the probes (each a check here AND the condition its sentinel must see fail) ----------------------------------------
  // 1a: a file nobody asked to translate -- a plain player: the strip says how to get subtitles, the slot is Translate,
  //     the page asks for nothing, and it PLAYS at once (no play-when-ready: no analysis is coming)
  const notAsked = (w) => {
    w.outbox.length = 0;
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false, autoplay: true }));
    metaOk(w.ids.fileVideo);
    w.pumpTimers(0);
    return w.ids.fileStatus.textContent === "Not translated yet · press Translate to make its subtitles"
      && slot(w) === "Translate|-|-" && ANALYSIS.every((t) => sent(w, t).length === 0)
      && !w.outbox.some((m) => m && m.type === "playerSet" && "translate" in m) && w.ids.fileVideo.paused === false;
  };
  // 1b: the press -- this file only (translatorFileAnalyse{force:false} once), and what the screen showed (the subtitle
  //     file next to the film, the default rule's) gives way to the subtitles being made
  const pressThisFile = (w) => {
    pair(w, "en", "zh");
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false, sidecars: [{ id: 8, name: "film.zh.srt", lang: "zh" }] }));
    metaOk(w.ids.fileVideo);
    w.send({ type: "playerSubs", id: 8, name: "film.zh.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false, part: 0, parts: 1,
             cues: [{ i: 0, s: 0, e: 600000, r: [["SIDECAR", 0]] }] });
    w.ids.fileVideo._seek(2);
    const before = overlay(w);
    w.outbox.length = 0;
    w.ids.fileTranslate.click();
    const asked = J(w.outbox) === J([{ type: "translatorFileAnalyse", force: false, quality: "auto" }]);
    w.send(fileMsg({ jobId: 3, phase: "analyzing", translate: true, fresh: false, sidecars: [{ id: 8, name: "film.zh.srt", lang: "zh" }] }));
    w.send(cueMsg(3, 0, 0, 600000, "en", "zh", "Hello", "你好"));
    w.ids.fileVideo._seek(3);
    return before === "SIDECAR" && asked && /你好/.test(overlay(w)) && !/SIDECAR/.test(overlay(w));
  };
  // 1c: running -> the slot is Cancel (it names the job); 1d: complete -> Re-analyse (force); a cancelled or failed file ->
  //     Translate again (it resumes from what is kept)
  const slotFollows = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true, doneMs: 60000 }));
    const running = slot(w) === "-|Cancel translation|-";   // (U2b re-point: G1, the user's answer 02/11 -- the Cancel says which work)
    w.outbox.length = 0;
    w.ids.fileCancel.click();
    const cancel = J(last(w, "translatorFileCancel")) === J({ type: "translatorFileCancel", jobId: 2 });
    w.send(fileMsg({ jobId: 2, phase: "cancelled", translate: false, doneMs: 60000 }));
    const cancelled = slot(w) === "Translate|-|-";
    w.send(fileMsg({ jobId: 3, phase: "done", complete: true, translate: false, doneMs: 600000, count: 1 }));
    const done = slot(w) === "-|-|Re-analyse";
    w.outbox.length = 0;
    w.ids.fileReanalyse.click();
    const re = J(last(w, "translatorFileAnalyse")) === J({ type: "translatorFileAnalyse", force: true, quality: "auto" });
    w.send(fileMsg({ jobId: 4, phase: "error", translate: true, err: "boom" }));
    return running && cancel && cancelled && done && re && slot(w) === "Translate|-|-";
  };
  // 1e: a film ALREADY analysed shows its subtitles without a press -- complete (done) or partial ("off": nothing asked),
  //     and nothing is asked for it
  const cacheShown = (w) => {
    w.outbox.length = 0;
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, translate: false, cached: true, count: 1, doneMs: 600000, ranges: [[0, 600000]] }));
    w.send(cueMsg(2, 0, 1000, 5000, "en", "zh", "Cached", "缓存"));
    metaOk(w.ids.fileVideo);
    w.ids.fileVideo._seek(2);
    const full = /缓存/.test(overlay(w));
    w.send(fileMsg({ jobId: 3, seq: 2, itemId: 2, phase: "off", translate: false, count: 1, doneMs: 120000, ranges: [[0, 120000]] }));
    w.send(cueMsg(3, 0, 1000, 5000, "en", "zh", "Partial", "部分"));
    metaOk(w.ids.fileVideo);
    w.ids.fileVideo._seek(2);
    return full && /部分/.test(overlay(w)) && ANALYSIS.every((t) => sent(w, t).length === 0)
      && w.ids.fileStatus.textContent === "1 subtitle · 20% of the file analysed · Translate finishes it";
  };
  // 2a: the queue's row -- Translate all / Cancel all, each row's state in words, where it stands
  // (FX re-point, F7 02/11: Cancel all follows the rows the LIST has -- the app's `batch` -- so the analysing and queued rows here
  //  are Translate all's: batch true)
  const Q6 = () => qMsg([qItem(1, "a.mkv", { state: "ready", pct: 100 }), qItem(2, "b.mkv", { state: "analysing", pct: 40, batch: true }),
                         qItem(3, "c.mkv", { state: "queued", pct: 10, batch: true }), qItem(4, "d.mkv", { state: "error" }),
                         qItem(5, "e.mkv", { state: "cancelled", pct: 25 }), qItem(6, "f.mkv", { state: "none" })]);
  const batchRow = (w) => {
    w.send(Q6());
    const words = [0, 1, 2, 3, 4, 5].map((i) => badgeOf(w, i).textContent).join("|");
    const shown = words === "✓ done|⟳ 40 %|queued|⚠ failed|cancelled|" && badgeOf(w, 2).classList.contains("st-queued")
      && badgeOf(w, 4).classList.contains("st-cancelled") && /Waiting for its turn/.test(badgeOf(w, 2).title);
    const row = w.ids.qTranslateAll.disabled === false && w.ids.qCancelAll.hidden === false
      && w.ids.qBatchText.textContent === "1 analysing · 1 queued · 1 done · 1 failed · 1 cancelled";
    w.outbox.length = 0;
    w.ids.qTranslateAll.click();
    w.ids.qCancelAll.click();
    return shown && row && J(w.outbox) === J([{ type: "translatorFileAnalyseAll", quality: "auto" }, { type: "translatorFileCancelAll" }]);
  };
  // 2b: nothing left to do -> Translate all disabled, no Cancel all, no words; a row's delta (playerItem) moves them
  const batchIdle = (w) => {
    w.send(qMsg([qItem(1, "a.mkv", { state: "ready", pct: 100 }), qItem(2, "b.mkv", { state: "ready", pct: 100 })]));
    const idle = w.ids.qTranslateAll.disabled === true && w.ids.qCancelAll.hidden === true && w.ids.qBatchText.textContent === "";
    w.send({ type: "playerItem", id: 2, state: "queued", pct: 0, batch: true });   // (FX re-point, F7: the list has it)
    return idle && w.ids.qCancelAll.hidden === false && w.ids.qBatchText.textContent === "1 queued · 1 done" && badgeOf(w, 1).textContent === "queued";
  };
  // 2c: the file playing taken by Translate all while NOTHING was on screen -> the subtitles being made show
  const batchTakesPlaying = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    metaOk(w.ids.fileVideo);
    w.send(fileMsg({ jobId: 3, phase: "loading", translate: true, fresh: false }));
    w.send(cueMsg(3, 0, 0, 600000, "en", "zh", "Batch", "批量"));
    w.ids.fileVideo._seek(1);
    return /批量/.test(overlay(w)) && sent(w, "translatorFileAnalyse").length === 0;
  };
  // 3: Translate Live -- the button, both tooltips, the notice's link
  const liveNamed = (w) => {
    const off = w.ids.liveBtn.textContent === "Translate Live" && /^Open Translate Live: /.test(w.ids.liveBtn.title);
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "zh", live: { on: true, inputs: [] } });
    return off && w.ids.liveBtn.title === "Translate Live is listening — show it" && w.ids.liveNoticeOpen.textContent === "Open Translate Live";
  };
  // 4: the pair -- ONE node: in the drop zone next to the model with nothing there, in the strip next to Translate once a
  //    file (or a queue) is there, back when it all goes
  const pairPlaced = (w) => {
    const node = w.ids.trPair, a = w.ids.langA;
    // (U2: "the model" is the models' slot now -- U1's one-option <select> gave way to the models menu, test_u2.js)
    const empty = node.parentNode === w.ids.emptyPairSlot && a.parentNode === node && node.parentNode.parentNode === w.ids.emptyModelSlot.parentNode;
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    const strip = node.parentNode === w.ids.stripPairSlot && w.ids.stripPairSlot.parentNode.children[0] === w.ids.fileTranslate;
    w.send({ type: "translatorFile", jobId: 3, seq: 0, phase: "idle" });
    const back = node.parentNode === w.ids.emptyPairSlot;
    w.send(qMsg([qItem(1, "a.mkv")]));
    const queued = node.parentNode === w.ids.stripPairSlot;
    return empty && strip && back && queued && w.ids.langA === a && !w.ids.bar.querySelector("select");
  };
  // 5: the empty state -- no switch, ONE model of SuVtitle's own (turbo), a stored "max" (1.0.x's large-v3) comes back as
  //    "auto" and is written so. (U2: the one-option <select> gave way to the models menu -- turbo is its default, the
  //    imported models join it: test_u2.js.)
  const modelOne = (w) => {
    const one = !w.ids.fileQuality && w.ids.modelsAsr.textContent === "large-v3-turbo";
    w.outbox.length = 0;
    w.ids.fileOpenBtn.click();
    return one && !w.ids.emptyTranslate && J(w.outbox) === J([{ type: "translatorFileOpen", quality: "auto" }])
      && w.lsStore["audiopb.tr.fileQuality"] === "auto";
  };
  // 6: the names -- "日本語 (Japanese)" in the selects, the hints, the CC rows; the bare name in the export menu
  const namesGlossed = (w) => {
    pair(w, "ja", "zh");
    const optA = w.ids.langA.children.map((o) => o.textContent);
    const sel = optA[optA.indexOf("日本語 (Japanese)")] && optA.indexOf("English") >= 0 && optA.indexOf("Français") >= 0
      && optA.indexOf("العربية (Arabic)") >= 0 && optA.indexOf("Русский (Russian)") >= 0 && optA.indexOf("한국어 (Korean)") >= 0
      && optA.indexOf("繁體中文 (Traditional Chinese)") >= 0 && optA.indexOf("简体中文 (Simplified Chinese)") >= 0 && optA.length === 13;
    const hint = w.ids.filePairHint.textContent === "日本語 (Japanese) ⇄ 简体中文 (Simplified Chinese): each sentence into the other language";
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, translate: false, count: 1, langA: "ja", langB: "zh" }));
    w.send(cueMsg(2, 0, 1000, 2000, "ja", "zh", "こんにちは", "你好"));
    w.ids.exportBtn.click();
    const menu = w.ids.exportMenu.children.map((b) => b.textContent);
    return sel && hint && menu.indexOf("Subtitles · 简体中文 (.srt)") >= 0 && menu.indexOf("Subtitles · 日本語, original (.srt)") >= 0
      && !menu.some((t) => /\((Japanese|Simplified Chinese)\)/.test(t)) && w.ids.langB.title === "简体中文 (Simplified Chinese)";
  };

  // ---- the checks ----------------------------------------------------------------------------------------------------------
  section("U1-1 -- Translate is a BUTTON (this file only); a film already analysed shows its subtitles without it; one slot: Translate | Cancel | Re-analyse");
  ok("a file nobody asked to translate is a PLAIN PLAYER: \"Not translated yet\", the slot is Translate, the page asks for nothing (no analysis, no playerSet{translate}) and it plays at once",
     notAsked(runTranslatorWindow()));
  ok("Translate asks for THIS file (translatorFileAnalyse{force:false, quality}, once) -- and the screen leaves the subtitle file next to the film for the subtitles being made",
     pressThisFile(runTranslatorWindow()));
  ok("the slot follows the file: Cancel while it runs (naming the job), Translate again once cancelled or failed, Re-analyse (force) once complete",
     slotFollows(runTranslatorWindow()));
  ok("a film ALREADY analysed shows its subtitles with no press -- complete, or partial (\"1 subtitle · 20% of the file analysed · Translate finishes it\") -- and nothing is asked",
     cacheShown(runTranslatorWindow()));
  ok("the slot sits at the START of the strip, before the pair, the dot and the status (one place; Full screen and Show in folder stay at the end)",
     // (U3 re-point: the TRANSCRIBE slot follows Translate's, before the pair) (U2b re-point: G1 -- "Cancel translation" /
     //  "Cancel transcript", and a comment before Translate's Cancel)
     /<span class="file-go">\s*<button id="fileTranslate"[^>]*class="tr-go"[^>]*>Translate<\/button>\s*(?:<!--[\s\S]*?-->\s*)?<button id="fileCancel"[^>]*>Cancel translation<\/button>\s*<button id="fileReanalyse"[^>]*>Re-analyse<\/button>\s*(?:<!--[\s\S]*?-->\s*)?<button id="fileTranscribe"[^>]*>Transcribe<\/button>\s*<button id="fileTrCancel"[^>]*>Cancel transcript<\/button>\s*<button id="fileRetranscribe"[^>]*>Re-transcribe<\/button>\s*<span id="stripPairSlot"/.test(html)
     && html.indexOf('id="fileGo"') < 0 && html.indexOf('class="file-go"') < html.indexOf('id="fileDot"') && html.indexOf('id="fileReveal"') > html.indexOf('id="fileStatus"'));
  ok("no switch anywhere any more: no role=\"switch\", no aria-checked on Translate, no .tr-switch rule",
     !/role="switch"/.test(html) && !/tr-switch/.test(html) && !w0has(runTranslatorWindow(), "aria-checked"));

  section("U1-2 -- \"Translate all\" in the queue panel: every file not done, one at a time; each row's state; Cancel all");
  ok("the queue's row: Translate all (enabled: there is a file to do), Cancel all (something waits or runs), and where it stands in words; "
     + "each row says queued / ⟳ 40 % / ✓ done / ⚠ failed / cancelled -- the two buttons send translatorFileAnalyseAll{quality} and translatorFileCancelAll{}",
     batchRow(runTranslatorWindow()));
  ok("nothing left to do: Translate all disabled, no Cancel all, no words -- and a row's delta (playerItem) moves them", batchIdle(runTranslatorWindow()));
  ok("the file playing taken by Translate all while NOTHING was on screen: the subtitles being made show (the page asks for nothing itself)",
     batchTakesPlaying(runTranslatorWindow()));
  ok("the row sits above the list in the Queue tab; its words are a status (role), its two buttons the app's own classes",
     html.indexOf('id="queueBatch"') > html.indexOf('id="queuePane"') && html.indexOf('id="queueBatch"') < html.indexOf('id="queueList"')
     && /<button id="qTranslateAll" type="button" class="tr-go"[^>]*>Translate all<\/button>/.test(html)
     && /<button id="qCancelAll" type="button"[^>]*hidden>Cancel all<\/button>/.test(html) && /<span id="qBatchText" class="q-batch-text" role="status">/.test(html)
     // (its display:flex would beat the browser's own [hidden] rule: the hide is the page's to say -- an empty queue shows no row)
     && /\.queue-batch button\[hidden\], \.queue-batch\[hidden\] \{ display: none; \}/.test(html) && /button\.tr-go\[hidden\] \{ display: none; \}/.test(html));

  section("U1-3 -- \"Translate Live\": the bar's button, its tooltips, the notice's link (the Live window keeps its own title)");
  ok("the button says Translate Live; its tooltip says what it opens, and that it listens when it does; the notice's link says it too", liveNamed(runTranslatorWindow()));

  section("U1-4 -- the language pair left the bar: next to Translate in the strip, next to the model in the drop zone (ONE set of controls)");
  ok("the pair is ONE node, moved: the drop zone with nothing there (beside the model), the strip once a file or a queue is there (after the slot), back when all goes -- the bar holds no select",
     pairPlaced(runTranslatorWindow()));
  ok("the pair still steers the ENGINE from its new place (one select sends the whole pair, the direction its mode)", (() => {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    w.outbox.length = 0;
    w.ids.langB.value = "fr"; w.ids.langB.dispatch("change");
    w.ids.dirBtn.click();
    return J(w.outbox) === J([{ type: "translatorCtl", langA: "en", langB: "fr" }, { type: "translatorCtl", mode: "a2b" }]);
  })());

  section("U1-5 -- the empty state: no Translate switch; the model menu keeps ONE option (large-v3-turbo), a stored \"max\" becomes \"auto\"");
  ok("ONE model of SuVtitle's own, turbo (U2: in the models menu, where imported models join it); a profile that had chosen large-v3 (\"max\") opens files with \"auto\" and is written so",
     modelOne(runTranslatorWindow({ ls: { "audiopb.tr.fileQuality": "max" } })));
  ok("...and the engine keeps knowing \"max\": the bridge still reads a quality token and the core still maps it (only the menu lost it)",
     /jsonFindString\(json, L"quality", q\)/.test(bridgeSrc) && /const std::string v = q == "max" \? "max" : "auto";/.test(playerSrc));

  section("U1-6 -- a non-Latin language is followed by its English name (\"日本語 (Japanese)\"), never in the export menu");
  ok("the selects, the hints and the tooltips say \"日本語 (Japanese)\" -- the six non-Latin scripts, the Latin seven as they were; the export menu says the bare names",
     namesGlossed(runTranslatorWindow()));
  ok("the table carries a third column for exactly the six non-Latin scripts (zh, zh-tw, ja, ko, ru, ar)", (() => {
    const tbl = (/const TR_LANGS = \[([\s\S]*?)\];/.exec(script) || ["", ""])[1];
    const rows = [...tbl.matchAll(/\["([a-z-]+)", "[^"]+"(?:, "([^"]+)")?\]/g)].map((m) => m[1] + (m[2] ? "=" + m[2] : ""));
    return rows.join("|") === "en|zh=Simplified Chinese|zh-tw=Traditional Chinese|fr|de|es|it|pt|ja=Japanese|ko=Korean|ru=Russian|nl|ar=Arabic";
  })());

  section("U1-7 -- the lockup: \"Transcription, Translation & Subtitles\" to the right of the name, \"by NEXTALE Audio\" under it");
  const lockupOk = (h) => /<div class="brand"[^>]*>\s*<span class="brand-line">\s*<span class="brand-name"><span class="brand-txt">Su<\/span><span class="brand-accent">V<\/span><span class="brand-txt">title<\/span><\/span>\s*<span class="brand-tag">Transcription, Translation &amp; Subtitles<\/span>\s*<\/span>\s*<span class="brand-by">by NEXTALE Audio<\/span>\s*<\/div>/.test(h)
    && /\.brand-tag \{\s*font-family: var\(--display\);/.test(h) && /\.brand-by \{[^}]*color: var\(--text-faint\);/.test(h)
    && /@media \(max-width: 980px\) \{ #bar \.brand-tag \{ display: none; \} \}/.test(h);
  ok("the bar's lockup: the name (Bahnschrift, unchanged), its legend to the right after a hairline, its maker small and faint under it; the legend gives way first on a narrow bar",
     lockupOk(html) && (() => { const w = runTranslatorWindow(); const q = (s) => (w.doc.querySelector(s) || {}).textContent;
       return q(".brand-name") === "SuVtitle" && q(".brand-tag") === "Transcription, Translation & Subtitles" && q(".brand-by") === "by NEXTALE Audio"; })());

  section("U1 -- the contract: the page's two new messages are the app's, the old switch's is gone both ways, the page builds no markup");
  ok("translatorFileAnalyseAll / translatorFileCancelAll are handled by the app; the bridge no longer reads playerSet{translate}; the page never sends it",
     /type == L"translatorFileAnalyseAll"/.test(bridgeSrc) && /type == L"translatorFileCancelAll"/.test(bridgeSrc)
     && !/jsonFindBool\(json, L"translate"/.test(bridgeSrc) && !/playerSet",\s*translate/.test(codeOnly(script)) && !/translate:\s*on\b/.test(codeOnly(script)));
  ok("the app says per FILE whether it is asked for (translatorFile.translate = wantedNow()) and no global switch in the queue",
     /j\.kv\(L"translate", wantedNow\(\)\);/.test(playerSrc) && !/j\.kv\(L"translate", translate_\)/.test(playerSrc));
  ok("U1's code builds no markup (textContent only)", !MARKUP_SINK.test(codeOnly(script)));

  // ---- MUTATION SENTINELS --------------------------------------------------------------------------------------------------
  section("U1 -- mutation sentinels (index.html with one U1 rule taken out each)");
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
    // a MARKUP / CSS rule: the check function on the mutated page text
    const judgeHtml = (name, mutateHtml, check) => {
      const m = mutateHtml(html);
      if (m === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      ok("MUTANT \"" + name + "\" is caught by its check", !check(m));
    };
    const R = (a, b) => (s) => s.split(a).join(b);
    const once = (a, b) => (s) => (s.split(a).length === 2 ? s.split(a).join(b) : s);   // (applies exactly once, else not at all)
    // 1
    // (a file nobody asked for PLAYS at once behind three walls -- armedGo's !F.translate, !fileRunning(), subSrc: no single
    //  mutant can fail it; its sentinel is the slot's own rule)
    judge("U1-1: no Translate on a file nobody asked for", once("    el.fileTranslate.hidden = quiet || run || p === \"done\";", "    el.fileTranslate.hidden = true;"), notAsked);
    judge("U1-1: the slot shows Translate while the analysis runs", once("    el.fileTranslate.hidden = quiet || run || p === \"done\";", "    el.fileTranslate.hidden = quiet || p === \"done\";"), slotFollows);
    judge("U1-1: no Cancel while it runs", once("    el.fileCancel.hidden = quiet || !run;", "    el.fileCancel.hidden = true;"), slotFollows);
    judge("U1-1: Re-analyse offered before the file is complete", once("    el.fileReanalyse.hidden = quiet || run || p !== \"done\";", "    el.fileReanalyse.hidden = quiet || run;"), slotFollows);
    judge("U1-1: the press leaves the subtitle file on screen", once("    if (F.phase === \"idle\") return;\n    useOwnSubs();\n", "    if (F.phase === \"idle\") return;\n"), pressThisFile);
    judge("U1-1: the press sends nothing", once("    send({ type: \"translatorFileAnalyse\", force: false, quality: fileQuality });\n  });", "  });"), pressThisFile);
    judge("U1-1: a cached film's subtitles NOT shown without a press (the old switch's rule)",
      once("      subSrc = ownSubs() ? \"audiopb\" : \"off\";", "      subSrc = F.translate ? \"audiopb\" : \"off\";"), cacheShown);
    judge("U1-1: what a film has is not counted as its own subtitles", once("  const ownSubs = () => F.translate || F.count > 0;", "  const ownSubs = () => F.translate;"), cacheShown);
    judge("U1-1: the strip names no way out for a partial film", once(" + \" · Translate finishes it\"", ""), cacheShown);
    // 2
    judge("U1-2: Translate all sends nothing", once("  btn(el.qTranslateAll, () => send({ type: \"translatorFileAnalyseAll\", quality: fileQuality }));", ""), batchRow);
    judge("U1-2: Cancel all sends nothing", once("  btn(el.qCancelAll, () => send({ type: \"translatorFileCancelAll\" }));", ""), batchRow);
    judge("U1-2: a queued row says nothing", once("                  queued: \"queued\", cancelled: \"cancelled\" };", "                  cancelled: \"cancelled\" };"), batchRow);
    // (U3 re-point: both batches; FX re-point, F7 02/11: the rows the list has -- `batch`)
    judge("U1-2: Cancel all never offered", once("    el.qCancelAll.hidden = !Q.items.some((x) => x.batch);", "    el.qCancelAll.hidden = true;"), batchRow);
    judge("U1-2: Translate all offered with nothing to do", once("    el.qTranslateAll.disabled = todo === 0;", "    el.qTranslateAll.disabled = false;"), batchIdle);
    judge("U1-2: a row's delta leaves the batch's words behind", once("    paintBatch();                              // (U1: the batch's words follow every row)\n", ""), batchIdle);
    judge("U1-2: the file playing taken by the batch keeps an empty screen",
      once("    if (!newItem && F.translate && !prevTranslate && subSrc === \"off\") useOwnSubs();", ""), batchTakesPlaying);
    // 3
    judge("U1-3: the listening tooltip still says \"the Live window\"", once("\"Translate Live is listening — show it\"", "\"The Live window is listening — show it\""), liveNamed);
    judgeHtml("U1-3: the button back to \"Live\"", R(">Translate Live</button>", ">Live</button>"), (h) => />Translate Live<\/button>/.test(h));
    // 4
    judge("U1-4: the pair never moves (left where the markup put it)", once("    placePair(showPlayer);", "    // placePair"), pairPlaced);
    judge("U1-4: the pair in the wrong place", once("    const slot = showPlayer ? el.stripPairSlot : el.emptyPairSlot;", "    const slot = showPlayer ? el.emptyPairSlot : el.stripPairSlot;"), pairPlaced);
    // 5
    judge("U1-5: a stored \"max\" kept (large-v3 asked for from a menu that no longer offers it)", once("  const QUALITIES = [\"auto\"];", "  const QUALITIES = [\"auto\", \"max\"];"), modelOne,
      { ls: { "audiopb.tr.fileQuality": "max" } });
    judge("U1-5: the stored \"max\" never written back", once("  if (read(KEY_FQUAL, \"\") === \"max\") write(KEY_FQUAL, fileQuality);\n", ""), modelOne,
      { ls: { "audiopb.tr.fileQuality": "max" } });
    // 6
    judge("U1-6: no English name after a non-Latin one", once("    return hit && hit[2] ? hit[1] + \" (\" + hit[2] + \")\" : langNative(c);", "    return langNative(c);"), namesGlossed);
    judge("U1-6: the export menu glossed too", once("    const tgt = langNative(fileTargetLang()), spoken = langNative(fileSpokenLang());",
      "    const tgt = langName(fileTargetLang()), spoken = langName(fileSpokenLang());"), namesGlossed);
    judge("U1-6: the selects keep the bare names", once("      o.value = code; o.textContent = langName(code);      // U1: \"日本語 (Japanese)\"", "      o.value = code; o.textContent = langNative(code);"), namesGlossed);
    // 7
    judgeHtml("U1-7: the legend gone", R("<span class=\"brand-tag\">Transcription, Translation &amp; Subtitles</span>", ""), lockupOk);
    judgeHtml("U1-7: the maker gone", R("<span class=\"brand-by\">by NEXTALE Audio</span>", ""), lockupOk);
    judgeHtml("U1-7: the legend never gives way on a narrow bar", R("@media (max-width: 980px) { #bar .brand-tag { display: none; } }", ""), lockupOk);
  }
};
// (a page's attribute anywhere in its markup tree: the stub's elements -- for "no switch left")
function w0has(w, attr) {
  const walk = (n) => (n.attrs && Object.prototype.hasOwnProperty.call(n.attrs, attr) && n.id === "fileTranslate") || (n.children || []).some(walk);
  return walk(w.doc.documentElement);
}

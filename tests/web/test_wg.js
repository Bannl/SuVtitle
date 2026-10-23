// test_wg.js -- wave WG (the user's first real test of the player, 11/10: seven requests). Required by test_ui.js
// (same ok/section counters): every check runs the REAL page (app/web/index.html) or reads SuVtitle's C++ (paths.js),
// and each guard of the wave has a MUTATION SENTINEL at the end -- the page with that guard removed must FAIL its probe.
// PORTED (S-WEB, 2026-10-23) from AudioPB's scratchpad/ui_test/test_wg.js of the 641f1aa era: WG-8 (the desk's
// "Player · Translator" button, drops on the main window) and the pins on AudioPB's bridge.cpp / main.cpp / installer
// stayed behind (docs/PROGRESS_S-WEB.md says which and why).
module.exports = function wgChecks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, ovLines, rows, last, countType, html, fs, path, showVia } = ctx;
  const sample = JSON.parse(fs.readFileSync(path.join(__dirname, "wg_sample_cues.json"), "utf8"));

  // ---- shared helpers ----------------------------------------------------------------------------------------
  const idxShown = (w) => w.ids.subOverlay.children.filter((l) => l.dataset.idx !== undefined)
    .map((l) => Number(l.dataset.idx)).filter((v, i, a) => a.indexOf(v) === i);
  const at = (w, sec) => { w.ids.fileVideo.currentTime = sec; w.pumpRaf(1); };
  const playing = (w, o) => {
    w.send(fileMsg(Object.assign({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }, o || {})));
    metaOk(w.ids.fileVideo);
    w.ids.tPlay.click();
    return w;
  };
  const cue = (idx, s, e, src, dst, o) => cueMsg(2, idx, s, e, (o && o.lang) || "en", (o && o.dstLang) || "fr", src, dst, o);
  // the page's own reading time, lifted from its script (the checks below assert against the page's formula AND
  // against hand-computed values, so a changed constant is seen)
  const pageScript = (html.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
  const grab = (re) => { const m = re.exec(pageScript); return m ? m[0] : ""; };
  let readMs = null;
  try {
    readMs = new Function(grab(/const READ_LATIN_CPS = [^\n]*/) + "\n" + grab(/const isCjkCode = [\s\S]*?;\r?\n/)
      + "\n" + grab(/function readMs\(text\) \{[\s\S]*?\n  \}/) + "\nreturn readMs;")();
  } catch (e) { readMs = null; }

  // ---- WG-1 probes (also the sentinels' probes) ----------------------------------------------------------------
  const shortCueHeld = (w) => {               // a 50 ms cue (the record's shortest) stays a second
    playing(w);
    w.send(cue(0, 1000, 1050, "Me", "Moins."));
    at(w, 1.02);
    const a = idxShown(w).join();
    at(w, 1.95);
    const b = idxShown(w).join();
    at(w, 2.02);
    return a === "0" && b === "0" && idxShown(w).length === 0;
  };
  const latinHeld = (w) => {                  // 48 Latin characters = 2.82 s, whatever the cue's own 0.9 s
    playing(w);
    w.send(cue(0, 10000, 10900, "x", "This sentence is long enough to need three seconds to read"));
    at(w, 12.8);
    const held = idxShown(w).join() === "0";
    at(w, 12.85);
    return held && idxShown(w).length === 0;
  };
  const cjkHeld = (w) => {                    // 14 Chinese characters at 7 a second = 2 s
    playing(w);
    w.send(cue(0, 20000, 20500, "x", "我们今天讨论的是混音里的对白", { dstLang: "zh" }));
    at(w, 21.9);
    const held = idxShown(w).join() === "0";
    at(w, 22.05);
    return held && idxShown(w).length === 0;
  };
  const stackOrder = (w) => {                 // previous ABOVE (smaller, main line only), current BELOW (+ the original)
    playing(w);
    w.send(cue(0, 0, 300, "La phrase longue d'avant", "This sentence is long enough to need three seconds to read"));
    w.send(cue(1, 500, 2500, "Court.", "Short."));
    at(w, 0.6);
    const L = ovLines(w);
    return L.length === 3 && L[0].text === "This sentence is long enough to need three seconds to read" && /sub-prev/.test(L[0].cls)
        && L[1].text === "Short." && !/sub-prev/.test(L[1].cls) && /sub-main/.test(L[1].cls)
        && L[2].text === "Court." && /sub-orig/.test(L[2].cls);
  };
  const maxThree = (w) => {                   // a burst of five: the newest THREE, never more
    playing(w);
    [0, 100, 200, 300, 400].forEach((s, i) => w.send(cue(i, s, s + 50, "m" + i, "w" + i)));
    at(w, 0.45);
    return idxShown(w).join() === "2,3,4" && w.ids.subOverlay.children.length === 4;   // 2 held + the current and its original
  };
  const extUntouched = (w) => {               // a subtitle FILE keeps its own timing (no hold)
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    metaOk(w.ids.fileVideo);
    w.send({ type: "playerSubs", id: 5, name: "x.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false,
             part: 0, parts: 1, cues: [{ s: 0, e: 100, r: [["Blink", 0]] }] });
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false, extSubId: 5 }));
    w.ids.fileVideo._seek(0.05);
    const there = ovLines(w).length === 1;
    w.ids.fileVideo._seek(0.15);
    return there && ovLines(w).length === 0;
  };

  // ---- WG-1 --------------------------------------------------------------------------------------------------
  section("WG-1 -- the overlay holds every line its READING time (17 Latin / 7 CJK chars a second, >= 1 s), stacks the overlap");
  {
    ok("the reading time is the page's own: >= 1 s, 17 Latin characters a second, 7 CJK ones (spaces not counted)",
       !!readMs && readMs("Me") === 1000 && readMs("This sentence is long enough to need three seconds to read") === 2824
       && readMs("我们今天讨论的是混音里的对白") === 2000 && readMs("a b") === 1000 && readMs("") === 1000,
       readMs && [readMs("Me"), readMs("This sentence is long enough to need three seconds to read"), readMs("我们今天讨论的是混音里的对白")]);
    ok("a 50 ms cue (the record's shortest: \"Me\") stays on the picture a full second", shortCueHeld(runTranslatorWindow()));
    ok("a long Latin line with a 0.9 s cue stays its reading time (2.8 s), then goes", latinHeld(runTranslatorWindow()));
    ok("a Chinese line is read at 7 characters a second (14 -> 2 s)", cjkHeld(runTranslatorWindow()));
    ok("held into the next one: STACKED -- the previous line above (smaller, its main line only), the current below with its original",
       stackOrder(runTranslatorWindow()));
    ok("a burst of five short cues: the newest three on screen, never more", maxThree(runTranslatorWindow()));
    ok("a subtitle FILE keeps its own timing (a 100 ms cue is gone at 150 ms)", extUntouched(runTranslatorWindow()));
    {
      const w = playing(runTranslatorWindow());
      w.send(cue(0, 0, 300, "a", "This sentence is long enough to need three seconds to read"));
      w.send(cue(1, 500, 2500, "b", "Short."));
      at(w, 0.6);
      ok("the subtitle list highlights the NEWEST line on screen", rows(w)[1].classList.contains("is-active") && !rows(w)[0].classList.contains("is-active"));
      showVia(w, "a");                         // Show A = English (the pair en/zh of fileMsg's state): the SOURCE texts
      at(w, 1.4);
      ok("the display rule changed (Show English: \"a\", 1 s): every hold is measured again on the text now shown",
         idxShown(w).join() === "1", idxShown(w));
    }
    // the sample cues (41 SYNTHETIC cues: the timings, languages and text lengths of a real record of 1632, the words invented): every one painted at least its reading time, at 60 fps
    {
      const w = runTranslatorWindow();
      w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "fr", langB: "en", sessionLines: 0 });
      w.send(fileMsg({ jobId: 2, phase: "done", complete: true, langA: "fr", langB: "en", totalMs: 4000000, ranges: [[0, 4000000]] }));
      metaOk(w.ids.fileVideo);
      w.send({ type: "translatorFileCues", jobId: 2, cues: sample.cues.map((c, i) => Object.assign({ idx: i }, c)) });
      w.ids.tPlay.click();
      const shown = new Array(sample.cues.length).fill(0);
      let maxStack = 0;
      const step = 1000 / 60;
      const spans = [[228000, 245000], [320000, 360000], [sample.cues[sample.cues.length - 1].s - 70000, sample.cues[sample.cues.length - 1].s + 15000]];
      spans.forEach(([a, b]) => { for (let t = a; t < b; t += step) { at(w, t / 1000); const s = idxShown(w); s.forEach((i) => { shown[i] += step; }); maxStack = Math.max(maxStack, s.length); } });
      const read = sample.cues.map((c) => readMs ? readMs(c.dst || c.src) : 0);
      const under = sample.cues.map((c, i) => i).filter((i) => shown[i] + step + 0.001 < read[i]);
      const nat = sample.cues.filter((c, i) => c.e - c.s < read[i]).length;
      ok("the sample cues (" + sample.cues.length + " synthetic cues shaped like a real record of 1632; " + nat + " of them shorter than their reading time): every one painted >= its reading time, <= 3 at once",
         !!readMs && nat >= 10 && under.length === 0 && maxStack >= 2 && maxStack <= 3, { under: under.map((i) => [sample.cues[i].s, Math.round(shown[i]), read[i]]), maxStack });
    }
    ok("the CSS: a held previous line is smaller and dimmer (.sub-prev)", /\.sub-line\.sub-prev \.sub-text \{ font-size: 0\.86em; opacity: 0\.74; \}/.test(html));
  }

  // ---- WG-2 (items 3 + 4): the picture first -------------------------------------------------------------------
  const panelClosedByDefault = (w) => {       // a 760 x 560 window: the list is a fold-out, closed until asked for
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    return w.ids.filePanel.classList.contains("is-closed") && !w.ids.fileListBtn.classList.contains("is-on")
        && w.ids.fileListBtn.getAttribute("aria-expanded") === "false";
  };
  // File mode: the bar stays, whatever the stillness. (RV4-14, review 23/10: this used to test the CLASS is-idle on #bar,
  // which no CSS styles any more since the Live window left -- a check that could not fail for a reason a viewer would
  // see. What fades or hides a bar is a STYLE: after 4 s of stillness while PLAYING, windowed, the bar must not be hidden,
  // carry an inline opacity 0 / visibility hidden / display none, or match one of the page's OWN rules that hide or fade
  // #bar -- read from its <style>: today only full screen's `body.is-full #bar { display: none; }`.)
  const BAR_HIDERS = (() => {
    const css = ((html.match(/<style>([\s\S]*?)<\/style>/) || ["", ""])[1]).replace(/\/\*[\s\S]*?\*\//g, " ");
    const out = [];
    for (const m of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
      if (!/(^|;)\s*(display\s*:\s*none|visibility\s*:\s*hidden|opacity\s*:\s*0(\.0+)?\s*(;|$))/.test(m[2])) continue;
      m[1].split(",").map((x) => x.trim()).filter((sel) => /(^|\s)#bar(\.[\w-]+)*$/.test(sel)).forEach((sel) => out.push(sel));
    }
    return out;
  })();
  // "body.a.b #bar.c": the classes the body and the bar need (a selector of any other shape counts as MATCHING: fail closed)
  const hiderApplies = (sel, w) => {
    const parts = sel.split(/\s+/), cls = (p) => (p.match(/\.[\w-]+/g) || []).map((c) => c.slice(1));
    const has = (node, p) => cls(p).every((c) => node.classList.contains(c));
    if (parts.length === 1) return has(w.ids.bar, parts[0]);
    if (parts.length === 2 && /^body(\.[\w-]+)*$/.test(parts[0])) return has(w.doc.body, parts[0]) && has(w.ids.bar, parts[1]);
    return true;
  };
  const noFadeInFile = (w) => {
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.ids.tPlay.click();
    w.win.dispatch("pointermove", {});
    w.pumpTimers(4000);
    const st = w.ids.bar.style || {};
    const inline = /^0(\.0+)?$/.test(String(st.opacity == null ? "" : st.opacity).trim()) || String(st.visibility || "") === "hidden"
                || String(st.display || "") === "none";
    return BAR_HIDERS.indexOf("body.is-full #bar") >= 0 && w.ids.fileVideo.paused === false && !w.ids.bar.hidden && !inline
        && !BAR_HIDERS.some((sel) => hiderApplies(sel, w));
  };
  section("WG-2 -- the picture first: a slim one-row bar that never fades in File mode, the list a fold-out (closed by default)");
  {
    ok("a 760 x 560 window in File mode: the subtitle list + queue panel is CLOSED by default (the picture takes the room)",
       panelClosedByDefault(runTranslatorWindow()));
    const w = runTranslatorWindow({ ls: { "audiopb.tr.panel": JSON.stringify({ tab: "subs", open: true }) } });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    ok("...WE's old \"open\" (audiopb.tr.panel) is not inherited: still closed", w.ids.filePanel.classList.contains("is-closed"));
    ok("the strip carries the door, in words: \"▸ Subtitle list\"", w.ids.fileListBtn.textContent === "▸Subtitle list"
       && w.ids.fileListBtn.title === "Show the subtitle list and the queue (☰)", [w.ids.fileListBtn.textContent, w.ids.fileListBtn.title]);
    w.ids.fileListBtn.click();
    ok("...a click opens it: ▾, pressed, and ☰ in the transport agrees",
       !w.ids.filePanel.classList.contains("is-closed") && w.ids.fileListBtn.classList.contains("is-on")
       && w.ids.fileListBtn.getAttribute("aria-expanded") === "true" && w.ids.fileListBtn.children[0].textContent === "▾"
       && w.ids.tPanel.classList.contains("is-on"));
    ok("...and the choice is remembered (audiopb.tr.panel2)", JSON.parse(w.lsStore["audiopb.tr.panel2"]).open === true);
    const w2 = runTranslatorWindow({ ls: { "audiopb.tr.panel2": w.lsStore["audiopb.tr.panel2"] } });
    w2.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    ok("...the next opening shows it open", !w2.ids.filePanel.classList.contains("is-closed") && w2.ids.fileListBtn.classList.contains("is-on"));
    w2.ids.tPanel.click();
    ok("☰ closes it again, and the strip's door says so", w2.ids.filePanel.classList.contains("is-closed")
       && !w2.ids.fileListBtn.classList.contains("is-on") && JSON.parse(w2.lsStore["audiopb.tr.panel2"]).open === false);
    const wd = runTranslatorWindow({ width: 1280, height: 800 });
    wd.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    ok("a wide window (1280 x 800): closed by default too; opened, it docks on the right (unchanged)",
       wd.ids.filePanel.classList.contains("is-closed") && wd.doc.body.classList.contains("is-wide"));
    const wn = runTranslatorWindow({ width: 420, height: 320 });
    wn.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    wn.ids.fileListBtn.click();
    ok("a narrow window (420 x 320): the strip's door opens the DRAWER (chevron only, by CSS)",
       !wn.ids.filePanel.classList.contains("is-closed") && /body\.is-narrow \.file-list-btn \.lbl-txt \{ display: none; \}/.test(html));
    ok("the bar does NOT fade after 3 s still while playing (the empty band over the picture, the user's screenshot; the fade was the Live window's) -- no style hides it: " + BAR_HIDERS.join(" | ") + " is full screen's",
       noFadeInFile(runTranslatorWindow()));
    ok("the bar is ONE slim row: no plate labels, no Show/Text plates at all (the CC panel has both), compact controls",
       /body\.in-file #bar \.lbl \{ display: none; \}/.test(html) && !/grp-show|grp-text|brand-sub/.test(html) && /<body class="in-file">/.test(html)
       && /body\.in-file #bar \.grp \{ flex-direction: row;/.test(html) && /body\.in-file #bar \{ padding: 5px 10px 6px;/.test(html));
    ok("full screen: the bar is gone and the transport auto-hides (unchanged)", /body\.is-full #bar \{ display: none; \}/.test(html)
       && /body\.is-full \.file-stage\.is-idle \.transport \{ opacity: 0\.01; pointer-events: none; \}/.test(html));
  }

  // ---- WG-3 (item 2): one subtitle file per language, written as the analysis goes ------------------------------
  section("WG-3 -- one .srt PER LANGUAGE (film.fr.srt + film.en.srt), written as the analysis goes; the shared original");
  {
    const store = ctx.P.cpp("ui/TrFileStore.h");
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, langA: "fr", langB: "en", srtPath: "D:\\F\\Feature.fr.srt", name: "Feature.mkv",
                     srtName: "Feature.fr.srt + Feature.en.srt", srtFiles: [{ lang: "fr", name: "Feature.fr.srt" }, { lang: "en", name: "Feature.en.srt" }] }));
    w.send(cueMsg(2, 0, 1000, 2000, "fr", "en", "Salut.", "Hi."));
    ok("done: the strip names BOTH files by what tells them apart (.fr.srt + .en.srt), the tooltip whole",
       /· saved \.fr\.srt \+ \.en\.srt$/.test(w.ids.fileStatus.textContent)
       && /\nSubtitle files: Feature\.fr\.srt \+ Feature\.en\.srt$/.test(w.ids.fileStatus.title), [w.ids.fileStatus.textContent, w.ids.fileStatus.title]);
    ok("the strip's status is ONE line cut short with an ellipsis, at every width (measured, G2b: it pushed the buttons to a 2nd row)",
       /\.file-status \{ flex: 1 1 160px; min-width: 0; color: var\(--text\); font-weight: 500;\s+white-space: nowrap; overflow: hidden; text-overflow: ellipsis; \}/.test(html));
    w.send(fileMsg({ jobId: 2, phase: "analyzing", langA: "fr", langB: "en", doneMs: 60000, ranges: [[0, 60000]], srtPath: "D:\\F\\Feature.fr.srt", name: "Feature.mkv",
                     srtName: "Feature.fr.srt + Feature.en.srt", srtFiles: [{ lang: "fr", name: "Feature.fr.srt" }, { lang: "en", name: "Feature.en.srt" }] }));
    ok("while analysing: the files exist already -- \"Show in folder\" is offered, and the tooltip says they fill in as it goes",
       w.ids.fileReveal.hidden === false && /Subtitle files \(filled in as it goes\): Feature\.fr\.srt \+ Feature\.en\.srt/.test(w.ids.fileStatus.title),
       w.ids.fileStatus.title);
    ok("the empty state says it: a subtitle file per language, as it goes", /a subtitle file per language \(film\.fr\.srt, film\.en\.srt…\) is saved next to it as it goes/.test(w.ids.emptyHint.textContent),
       w.ids.emptyHint.textContent);
    const jsCodes = ((/const TR_LANGS = \[([\s\S]*?)\];/.exec(html) || ["", ""])[1].match(/\["([a-z-]+)",/g) || []).map((x) => x.slice(2, -2));
    const cCodes = ((/static const char\* const k\[\] = \{ ("en"[^}]*)nullptr \};/.exec(store) || ["", ""])[1].match(/"([a-z-]+)"/g) || []).map((x) => x.slice(1, -1));
    ok("TrFileStore.h knownLangCodes() = the page's TR_LANGS, code for code (the shared original's candidates; " + ctx.P.cppSource("ui/TrFileStore.h").from + ")",
       jsCodes.length === 13 && jsCodes.join() === cCodes.join(), { js: jsCodes, cpp: cCodes });
  }

  // ---- WG-4 (item 7): THE SUBTITLE EDITOR ---------------------------------------------------------------------
  // The page only COMPUTES edits; here a fake core answers each subEdit the way TrPlayer.h editCues does (the real
  // core's checks are proved in player_smoke [14]): the removed idx go, the added cues get fresh idx.
  const answer = (w, o) => {
    const m = last(w, "subEdit");
    if (!m) return null;
    w.__nextIdx = w.__nextIdx || 100;
    const add = (m.add || []).map((c) => Object.assign({ idx: w.__nextIdx++ }, c));
    const msg = Object.assign({ type: "translatorFileEdited", jobId: m.gen, rev: m.rev, ok: true, remove: m.remove, add, edited: true }, o || {});
    if (msg.ok === false) { msg.remove = []; msg.add = []; }
    w.send(msg);
    return m;
  };
  const edOpen = (w) => {                     // a complete film, three lines, the editor open on the middle one
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "fr", langB: "en", sessionLines: 0 });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, langA: "fr", langB: "en", totalMs: 600000, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.send(cueMsg(2, 0, 10000, 12000, "fr", "en", "La nuit.", "The night."));
    w.send(cueMsg(2, 1, 20000, 24000, "fr", "en", "Soit tu restes, soit on te laisse.", "Either you stay, or we move you."));
    w.send(cueMsg(2, 2, 30000, 31000, "fr", "en", "F1.", "F1."));
    w.ids.fileVideo._seek(21);
    w.win.dispatch("keydown", { key: "e", target: w.doc.body });
    return w;
  };
  const sentFor = (w) => { const m = last(w, "subEdit"); return m ? JSON.stringify({ remove: m.remove, add: m.add }) : "none"; };
  const refusedLeavesList = (w) => {          // a REFUSED edit changes nothing on the page
    edOpen(w);
    w.ids.edDst.value = "Either stay or be moved.";
    w.ids.edDst.dispatch("keydown", { key: "Enter", shiftKey: false });
    answer(w, { ok: false, err: "That would overlap the subtitle at 0:30: not applied." });
    return rows(w).length === 3 && rows(w)[1].children[1].textContent === "Either you stay, or we move you."
        && w.ids.edMsg.textContent === "That would overlap the subtitle at 0:30: not applied." && w.ids.edMsg.classList.contains("is-error");
  };
  const clampAtNext = (w) => {                // an end pushed into the next line STOPS at it (and says so)
    edOpen(w);
    w.ids.edEnd.value = "0:35.000";
    w.ids.edEnd.dispatch("keydown", { key: "Enter" });
    const m = last(w, "subEdit");
    return !!m && m.add.length === 1 && m.add[0].e === 30000 && m.add[0].s === 20000;
  };
  const undoRedo = (w) => {                   // undo = the inverse edit; redo = it again
    edOpen(w);
    w.ids.edDst.value = "Stay, or be moved.";
    w.ids.edDst.dispatch("keydown", { key: "Enter" });
    answer(w);
    w.outbox.length = 0;
    w.ids.edUndo.click();
    const u = last(w, "subEdit");
    answer(w);
    const backOk = rows(w)[1].children[1].textContent === "Either you stay, or we move you.";
    w.ids.edRedo.click();
    const r = last(w, "subEdit");
    answer(w);
    return !!u && u.remove.length === 1 && u.add.length === 1 && u.add[0].dst === "Either you stay, or we move you." && backOk
        && !!r && r.add[0].dst === "Stay, or be moved." && rows(w)[1].children[1].textContent === "Stay, or be moved.";
  };
  const noFalseGap = (w) => {                 // a deletion, then the analysis' next cue: no "missing cue" replay asked
    edOpen(w);
    w.ids.edDelete.click();
    answer(w);
    w.outbox.length = 0;
    w.send(cueMsg(2, 3, 40000, 41000, "fr", "en", "Plus tard.", "Later."));
    return countType(w, "translatorFileReplay") === 0 && rows(w).length === 3;
  };
  const asksBeforeReanalysis = (w) => {       // an EDITED file: "Re-analyse" asks first
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, edited: true }));
    w.outbox.length = 0;
    w.ids.fileReanalyse.click();
    return countType(w, "translatorFileAnalyse") === 0 && w.ids.fileConfirm.hidden === false;
  };
  // ---- the REVIEW's fixes (adversarial review of WG, 11/10) -- probes, also the sentinels' --------------------------
  const keyWin = (w, target, o) => {          // a key as the browser delivers it: the field's handler, then the window's
    const ev = Object.assign({ target }, o);
    if (target && target.dispatch) target.dispatch("keydown", ev);
    w.win.dispatch("keydown", ev);
  };
  const staleFieldNotCarried = (w) => {       // #1: a fix typed in line 2, then Alt+down -> the fix goes to LINE 2
    edOpen(w);
    w.ids.edDst.focus();
    w.ids.edDst.value = "A fix for line two.";
    w.outbox.length = 0;
    keyWin(w, w.ids.edDst, { key: "ArrowDown", altKey: true });
    const m = last(w, "subEdit");
    const sentToTwo = !!m && JSON.stringify(m.remove) === "[1]" && m.add.length === 1 && m.add[0].dst === "A fix for line two.";
    const movedOn = rows(w)[2].classList.contains("is-sel") && w.ids.edDst.value === "F1.";
    answer(w);
    const stayed = rows(w)[2].classList.contains("is-sel") && rows(w)[1].children[1].textContent === "A fix for line two.";
    w.outbox.length = 0;
    w.ids.edDst.blur();
    return sentToTwo && movedOn && stayed && countType(w, "subEdit") === 0;
  };
  const newLineEmptyField = (w) => {          // #1: "+ New" with the text field focused: the new line's box is EMPTY
    edOpen(w);
    w.ids.fileVideo._seek(15);
    w.ids.edSrc.focus();
    w.ids.edNew.click();
    answer(w);
    const selKey = rows(w).findIndex((r) => r.classList.contains("is-sel"));
    return w.ids.edSrc.value === "" && w.ids.edDst.value === "" && selKey === 1 && w.ids.edStart.value === "0:15.000";
  };
  const undoTwice = (w) => {                  // #2: two nudges of one line, then TWO undos: the second names the LIVE key
    edOpen(w);
    const plus = w.ids.edTimes.children[0].children[4];       // Start +.1
    plus.click(); answer(w);
    plus.click(); answer(w);
    w.ids.edUndo.click();
    const u1 = last(w, "subEdit"); answer(w);
    w.ids.edUndo.click();
    const u2 = last(w, "subEdit"); answer(w);
    // the fake core gives idx 100, 101 to the nudges, 102 to the first undo's line: the 2nd undo must remove 102
    return !!u1 && JSON.stringify(u1.remove) === "[101]" && u1.add[0].s === 20100 && u1.restore === true
        && !!u2 && JSON.stringify(u2.remove) === "[102]" && u2.add[0].s === 20000 && w.ids.edStart.value === "0:20.000";
  };
  const altArrowNoNudge = (w) => {            // #6: Alt+down in the Start field = the next line, never a nudge
    edOpen(w);
    w.ids.edStart.focus();
    w.outbox.length = 0;
    keyWin(w, w.ids.edStart, { key: "ArrowDown", altKey: true });
    return countType(w, "subEdit") === 0 && rows(w)[2].classList.contains("is-sel");
  };
  const confirmBound = (w) => {               // #7: the "re-analyse over your edits?" bar dies with its file
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, edited: true }));
    w.ids.fileReanalyse.click();
    const asked = w.ids.fileConfirm.hidden === false;
    w.send(fileMsg({ jobId: 3, seq: 2, itemId: 2, phase: "done", complete: true, edited: true, name: "other.mp4", mediaKey: "mk2" }));
    const gone = w.ids.fileConfirm.hidden === true;
    w.outbox.length = 0;
    w.ids.fileConfirmYes.click();
    return asked && gone && countType(w, "translatorFileAnalyse") === 0;
  };
  const lateAnswerClears = (w) => {           // #13: an answer after the page gave up: applied, the history starts again
    edOpen(w);
    w.ids.edDst.value = "Late fix.";
    w.ids.edDst.dispatch("keydown", { key: "Enter" });
    w.pumpTimers(4100);
    const gaveUp = /No answer to that edit/.test(w.ids.edMsg.textContent);
    answer(w);
    return gaveUp && rows(w)[1].children[1].textContent === "Late fix." && w.ids.edUndo.disabled === true
        && /applied after all/.test(w.ids.edMsg.textContent);
  };
  section("WG-4 -- THE SUBTITLE EDITOR (texts, timing, split, merge, delete, new, shift, undo/redo; the core decides)");
  {
    const w = edOpen(runTranslatorWindow());
    ok("E opens the editor: the panel on Subtitles, the pane above the list, the line under the playhead selected",
       w.ids.tabEdit.classList.contains("is-on") && w.ids.edPane.hidden === false && !w.ids.filePanel.classList.contains("is-closed")
       && rows(w)[1].classList.contains("is-sel") && w.ids.edWhat.textContent === "0:20.000 → 0:24.000 · 4.00 s", w.ids.edWhat.textContent);
    ok("...its two texts, each labelled with its language (the original, the translation)",
       w.ids.edSrcLbl.textContent === "Français · original" && w.ids.edSrc.value === "Soit tu restes, soit on te laisse."
       && w.ids.edDstLbl.textContent === "English · translation" && w.ids.edDst.value === "Either you stay, or we move you."
       && w.ids.edSrc.getAttribute("lang") === "fr" && w.ids.edStart.value === "0:20.000" && w.ids.edEnd.value === "0:24.000");
    w.outbox.length = 0;
    w.ids.edDst.value = "Stay, or be moved.";
    w.ids.edDst.dispatch("keydown", { key: "Enter", shiftKey: false });
    ok("a TEXT edit (Enter): the page SENDS it -- remove the line, add it with its new text -- and changes nothing yet",
       sentFor(w) === JSON.stringify({ remove: [1], add: [{ s: 20000, e: 24000, lang: "fr", dstLang: "en", pass: false,
         src: "Soit tu restes, soit on te laisse.", dst: "Stay, or be moved." }] }) && last(w, "subEdit").gen === 2
       && last(w, "subEdit").target === "audiopb" && rows(w)[1].children[1].textContent === "Either you stay, or we move you."
       && w.ids.edState.textContent === "applying…", sentFor(w));
    answer(w);
    ok("...the core's answer applies it: the list, the selection (the new line), \"edited\" in the pane and the strip",
       rows(w).length === 3 && rows(w)[1].children[1].textContent === "Stay, or be moved." && rows(w)[1].classList.contains("is-sel")
       && w.ids.edState.textContent === "edited · saved by itself" && / · edited/.test(w.ids.fileStatus.textContent)
       && w.ids.edMsg.textContent === "Text changed", [w.ids.edState.textContent, w.ids.fileStatus.textContent]);
    // timing
    w.outbox.length = 0;
    w.ids.edPane.children[1].children[0].children[4].click();  // .ed-times > Start row > +.1
    ok("nudge: Start +0.1 s -> the line moved by 100 ms", last(w, "subEdit").add[0].s === 20100 && last(w, "subEdit").add[0].e === 24000, sentFor(w));
    answer(w);
    w.ids.edStart.value = "0:19.5";
    w.ids.edStart.dispatch("keydown", { key: "Enter" });
    ok("a TYPED start (0:19.5) -> 19 500 ms", last(w, "subEdit").add[0].s === 19500, sentFor(w));
    answer(w);
    w.ids.edStart.value = "1:2:3:4";
    w.ids.edStart.dispatch("keydown", { key: "Enter" });
    ok("...a typed thing that is not a time is refused on the page, said, sent nowhere",
       /That is not a time/.test(w.ids.edMsg.textContent) && last(w, "subEdit").add[0].s === 19500);
    ok("an end pushed INTO the next line stops at it (never an overlap), and says so", clampAtNext(edOpen(runTranslatorWindow()))
       && (() => { const x = edOpen(runTranslatorWindow()); x.ids.edEnd.value = "0:35"; x.ids.edEnd.dispatch("keydown", { key: "Enter" }); answer(x);
                   return /Stopped at the next line \(it starts at 0:30\.000\)/.test(x.ids.edMsg.textContent); })());
    const wq = edOpen(runTranslatorWindow());
    wq.ids.edStart.value = "0:23.95";
    wq.ids.edStart.dispatch("keydown", { key: "Enter" });
    ok("...a line squeezed under 0.1 s is refused, sent nowhere", /at least 0\.1 s/.test(wq.ids.edMsg.textContent) && countType(wq, "subEdit") === 0);
    // the playhead
    const wh = edOpen(runTranslatorWindow());
    wh.ids.fileVideo._seek(22.5);
    wh.win.dispatch("keydown", { key: "o", altKey: true, target: wh.doc.body });
    ok("Alt+O: the end at the playhead (22.5 s)", last(wh, "subEdit") && last(wh, "subEdit").add[0].e === 22500, sentFor(wh));
    // split
    const ws = edOpen(runTranslatorWindow());
    ws.ids.fileVideo._seek(22);
    ws.ids.edSplit.click();
    const sp = last(ws, "subEdit");
    ok("SPLIT at the playhead (0:22 of 0:20-0:24): two lines, each text cut at the space nearest the half",
       !!sp && sp.remove.join() === "1" && sp.add.length === 2 && sp.add[0].s === 20000 && sp.add[0].e === 22000 && sp.add[1].s === 22000
       && sp.add[1].e === 24000 && sp.add[0].src === "Soit tu restes," && sp.add[1].src === "soit on te laisse."
       && sp.add[0].dst === "Either you stay," && sp.add[1].dst === "or we move you.", sentFor(ws));
    const wc = edOpen(runTranslatorWindow());
    wc.ids.fileVideo._seek(22);
    wc.ids.edSrc.focus();                      // (the stub tracks the focus: document.activeElement)
    wc.ids.edSrc.selectionStart = 4;
    wc.ids.edSplit.click();
    ok("...in a text, at the CURSOR (Soit| tu restes...)", last(wc, "subEdit").add[0].src === "Soit" && last(wc, "subEdit").add[1].src === "tu restes, soit on te laisse.",
       sentFor(wc));
    // merge
    const wm = edOpen(runTranslatorWindow());
    wm.ids.edMerge.click();
    const mg = last(wm, "subEdit");
    ok("MERGE with the next: one line from the first start to the second end, the texts joined with a space",
       !!mg && mg.remove.join() === "1,2" && mg.add.length === 1 && mg.add[0].s === 20000 && mg.add[0].e === 31000
       && mg.add[0].src === "Soit tu restes, soit on te laisse. F1." && mg.add[0].dst === "Either you stay, or we move you. F1.", sentFor(wm));
    const wx = runTranslatorWindow();
    wx.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "fr", langB: "en", sessionLines: 0 });
    wx.send(fileMsg({ jobId: 2, phase: "done", complete: true, langA: "fr", langB: "en" }));
    metaOk(wx.ids.fileVideo);
    wx.send(cueMsg(2, 0, 1000, 2000, "en", "fr", "Meep!", "Bip !"));
    wx.send(cueMsg(2, 1, 3000, 4000, "fr", "en", "Oui.", "Yes."));
    wx.send(cueMsg(2, 2, 5000, 6000, "zh", "en", "我们", "We"));
    wx.send(cueMsg(2, 3, 7000, 8000, "zh", "en", "走吧", "go"));
    wx.ids.fileVideo._seek(1.5);
    wx.win.dispatch("keydown", { key: "e", target: wx.doc.body });
    wx.ids.edMerge.click();
    ok("...two lines in DIFFERENT languages are not merged (each language's file would lose one), and it says why",
       countType(wx, "subEdit") === 0 && /not in the same languages/.test(wx.ids.edMsg.textContent));
    [1, 2, 3].forEach(() => wx.win.dispatch("keydown", { key: "ArrowDown", altKey: true, target: wx.doc.body }));   // -> 走吧
    wx.win.dispatch("keydown", { key: "ArrowUp", altKey: true, target: wx.doc.body });                               // -> 我们
    wx.ids.edMerge.click();
    ok("...two Chinese lines join WITHOUT a space; Alt+↓ / Alt+↑ walk the lines", last(wx, "subEdit") && last(wx, "subEdit").add[0].src === "我们走吧"
       && last(wx, "subEdit").add[0].dst === "We go", sentFor(wx));
    // delete, new, shift
    const wd = edOpen(runTranslatorWindow());
    wd.ids.edDelete.click();
    answer(wd);
    ok("DELETE: the line goes, the next one is selected", rows(wd).length === 2 && rows(wd)[1].classList.contains("is-sel")
       && wd.ids.edWhat.textContent.indexOf("0:30.000") === 0, wd.ids.edWhat.textContent);
    const wn = edOpen(runTranslatorWindow());
    wn.ids.fileVideo._seek(15);
    wn.ids.edNew.click();
    const nw = last(wn, "subEdit");
    ok("NEW at the playhead (0:15, in a gap): 2 s long, in the languages around it, empty -- then its text is typed",
       !!nw && nw.remove.length === 0 && nw.add.length === 1 && nw.add[0].s === 15000 && nw.add[0].e === 17000
       && nw.add[0].lang === "fr" && nw.add[0].dstLang === "en" && nw.add[0].src === "" && nw.add[0].dst === "", sentFor(wn));
    wn.ids.fileVideo._seek(21);
    wn.outbox.length = 0;
    answer(wn);
    wn.ids.edNew.click();
    ok("...INSIDE a line: refused, \"split it instead\"", countType(wn, "subEdit") === 0 && /split it instead/.test(wn.ids.edMsg.textContent));
    const wf = edOpen(runTranslatorWindow());
    wf.ids.edShift.value = "+1.5";
    wf.ids.edShiftGo.click();
    const sh = last(wf, "subEdit");
    ok("SHIFT this line and all after by +1.5 s: one edit, every one moved", !!sh && sh.remove.join() === "1,2" && sh.add.length === 2
       && sh.add[0].s === 21500 && sh.add[1].e === 32500, sentFor(wf));
    const wg = edOpen(runTranslatorWindow());
    wg.ids.edShift.value = "-9";
    wg.ids.edShiftGo.click();
    ok("...back by 9 s would overlap the line before: refused on the page, said", countType(wg, "subEdit") === 0
       && /would overlap the line before \(it ends at 0:12\.000\)/.test(wg.ids.edMsg.textContent), wg.ids.edMsg.textContent);
    // undo/redo, refusal, pending, timeout
    ok("UNDO = the inverse edit (the old text back); REDO = the edit again", undoRedo(runTranslatorWindow()));
    ok("a REFUSED edit (the core's sentence) changes nothing, and says why", refusedLeavesList(runTranslatorWindow()));
    const wp = edOpen(runTranslatorWindow());
    wp.ids.edDelete.click();
    wp.ids.edSplit.click();
    ok("one edit at a time: a second one while the first waits for its answer is held back, and said",
       countType(wp, "subEdit") === 1 && /Wait: the last edit is being applied/.test(wp.ids.edMsg.textContent));
    wp.pumpTimers(4100);
    ok("...no answer within 4 s: given up, said, the editor usable again", /No answer to that edit/.test(wp.ids.edMsg.textContent)
       && wp.ids.edSplit.disabled === false);
    const wk = edOpen(runTranslatorWindow());
    wk.ids.edDst.value = "X.";
    wk.ids.edDst.dispatch("keydown", { key: "Enter" });
    answer(wk);
    wk.outbox.length = 0;
    wk.win.dispatch("keydown", { key: "z", ctrlKey: true, target: wk.doc.body });
    ok("Ctrl+Z undoes (outside a text field)", countType(wk, "subEdit") === 1 && last(wk, "subEdit").add[0].dst === "Either you stay, or we move you.");
    wk.outbox.length = 0;
    wk.win.dispatch("keydown", { key: "z", ctrlKey: true, target: wk.ids.edSrc });
    ok("...but inside a text field Ctrl+Z is the field's own", countType(wk, "subEdit") === 0);
    ok("after a deletion the analysis' next line is not taken for a gap (no replay asked)", noFalseGap(runTranslatorWindow()));
    ok("an EDITED file: Re-analyse asks first (\"Keep my edits\" / \"Re-analyse anyway\")", asksBeforeReanalysis(runTranslatorWindow()));
    const wr = runTranslatorWindow();
    asksBeforeReanalysis(wr);
    wr.ids.fileConfirmNo.click();
    const kept = countType(wr, "translatorFileAnalyse") === 0 && wr.ids.fileConfirm.hidden === true;
    wr.ids.fileReanalyse.click();
    wr.ids.fileConfirmYes.click();
    ok("...\"Keep my edits\" sends nothing; \"Re-analyse anyway\" re-analyses (force)", kept && JSON.stringify(last(wr, "translatorFileAnalyse"))
       === JSON.stringify({ type: "translatorFileAnalyse", force: true, quality: "auto" }));
    // a subtitle FILE
    const we = runTranslatorWindow();
    we.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    metaOk(we.ids.fileVideo);
    we.send({ type: "playerSubs", id: 31, name: "film.fr.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false, part: 0, parts: 1,
              cues: [{ i: 0, s: 500, e: 2800, r: [["Le niveau des ", 0], ["dialogues", 1], [" est bon.", 0]] }, { i: 1, s: 3000, e: 5500, r: [["On baisse ?", 0]] }] });
    we.send(fileMsg({ jobId: 2, phase: "off", translate: false, extSubId: 31 }));
    we.ids.fileVideo._seek(1);
    we.win.dispatch("keydown", { key: "e", target: we.doc.body });
    ok("a subtitle FILE in the editor: its one text, with its italics as .srt markup (TEXT in a field), Save as… offered",
       we.ids.edPane.hidden === false && we.ids.edSrcLbl.textContent === "Text" && we.ids.edSrc.value === "Le niveau des <i>dialogues</i> est bon."
       && we.ids.edDstRow.hidden === true && we.ids.edSaveAs.hidden === false, we.ids.edSrc.value);
    we.ids.edSrc.value = "Le niveau des <i>voix</i> est bon.";
    we.ids.edSrc.dispatch("keydown", { key: "Enter" });
    const xe = last(we, "subEdit");
    ok("...an edit goes to the BRIDGE by the file's id and the line's id (subEdit target ext)",
       !!xe && xe.target === "ext" && xe.id === 31 && xe.remove.join() === "0" && xe.add[0].text === "Le niveau des <i>voix</i> est bon."
       && xe.add[0].s === 500, JSON.stringify(xe));
    we.send({ type: "playerSubsEdited", id: 31, rev: xe.rev, ok: true, remove: [0],
              add: [{ i: 2, s: 500, e: 2800, t: "Le niveau des <i>voix</i> est bon.", r: [["Le niveau des ", 0], ["voix", 1], [" est bon.", 0]] }] });
    ok("...the bridge's answer: the list and the picture show it (italic by its flag, never by markup)",
       rows(we)[0].children[1].textContent === "Le niveau des voix est bon." && ovLines(we)[0].kids.join() === "#TEXT.,SPAN.r-i,#TEXT."
       && we.ids.edState.textContent === "not saved yet…", [rows(we)[0].children[1].textContent, we.ids.edState.textContent]);
    we.send({ type: "playerSubsSaved", id: 31, ok: true, name: "film.fr.srt", bak: "film.fr.srt.bak" });
    ok("...written back after a ONE-TIME backup: the pane and a toast say so",
       we.ids.edState.textContent === "saved · film.fr.srt (original kept as film.fr.srt.bak)"
       && /the original is kept as film\.fr\.srt\.bak/.test(we.ids.fileToast.textContent), we.ids.edState.textContent);
    we.outbox.length = 0;
    we.ids.edSaveAs.click();
    ok("...Save as… asks the bridge for its dialog (by the file's id)", JSON.stringify(we.outbox) === JSON.stringify([{ type: "subSaveAs", id: 31 }]));
    const wnarrow = edOpen(runTranslatorWindow({ width: 420, height: 320 }));
    ok("a narrow window: the editor opens the DRAWER, taller while editing", !wnarrow.ids.filePanel.classList.contains("is-closed")
       && wnarrow.doc.body.classList.contains("is-editing") && /body\.is-narrow\.is-editing \.file-panel \{ height: 88%; \}/.test(html));
    ok("the help lists the editor's keys", /<kbd>E<\/kbd><span>Edit the subtitles/.test(html) && /<kbd>Alt\+I Alt\+O<\/kbd>/.test(html)
       && /<kbd>Ctrl\+Z Ctrl\+Y<\/kbd>/.test(html));
  }

  // ---- WG-5 (item 5): a subtitle file DROPPED on the playing video -------------------------------------------------
  section("WG-R -- the adversarial review's fixes (11/10): stale fields, stable undo, Alt keys, the confirm bar, a late answer");
  ok("#1 a fix typed in line 2, then Alt+down: committed to LINE 2 (not carried to line 3); the selection stays where the user went",
     staleFieldNotCarried(runTranslatorWindow()));
  ok("#1 \"+ New\" with the text field focused: the new line's fields are EMPTY (not the previous line's text)",
     newLineEmptyField(runTranslatorWindow()));
  ok("#2 two nudges of one line, two undos: the second names the line's LIVE key (re-pointed after the first), marked restore",
     undoTwice(runTranslatorWindow()));
  ok("#6 Alt+down in the Start field: the next line -- never a 0.1 s nudge", altArrowNoNudge(runTranslatorWindow()));
  ok("#7 the \"re-analyse over your edits?\" bar is closed by another file, and its Yes then sends nothing", confirmBound(runTranslatorWindow()));
  ok("#13 an answer after the page gave up: applied (it is what the tray holds), the undo history cleared, said",
     lateAnswerClears(runTranslatorWindow()));
  {
    const w = edOpen(runTranslatorWindow());
    w.ids.edDst.value = "x";
    w.ids.edDst.dispatch("keydown", { key: "Enter" });
    ok("...an ordinary edit is not a restore (restore:false)", last(w, "subEdit").restore === false);
    w.send({ type: "translatorFileNotice", text: "5 items were not a video, audio or subtitle file (or not found) and were left out." });
    const up = w.ids.fileToast.hidden === false;
    w.send({ type: "translatorWindow", shown: false });
    ok("a toast does not outlive the window's closing (a hidden page's timers are throttled: it greeted the next opening, wg_real R13)",
       up && w.ids.fileToast.hidden === true);
  }

  section("WG-5 -- a subtitle file dropped on the video becomes the subtitles, and says \"Subtitles loaded: <name>\"");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.ids.tPlay.click();
    const srt = { name: "E01 Pilot.fr.srt", type: "" };
    w.dispatchDoc("drop", { target: w.ids.fileVideo, dataTransfer: { files: [srt], items: [{ kind: "file", webkitGetAsEntry: () => ({ isDirectory: false }) }] } });
    ok("the drop on the PLAYING video hands the .srt over (the bridge reads its real path), target play",
       JSON.stringify(last(w, "translatorFileDrop")) === JSON.stringify({ type: "translatorFileDrop", quality: "auto", target: "play" })
       && w.objs.length === 1 && w.objs[0][0] === srt && w.ids.fileVideo.paused === false);
    // the bridge's answer, in its order (loadExtSub): the file's parts, THEN the state naming it
    w.send({ type: "playerSubs", id: 40, name: "E01 Pilot.fr.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false, part: 0, parts: 1,
             cues: [{ i: 0, s: 0, e: 900000, r: [["Le niveau est bon.", 0]] }] });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], extSubId: 40 }));
    w.ids.fileVideo._seek(3);
    ok("...it becomes THE subtitles at once (picture + list) and says \"Subtitles loaded: E01 Pilot.fr.srt · UTF-8\"",
       w.ids.fileToast.textContent === "Subtitles loaded: E01 Pilot.fr.srt · UTF-8" && ovLines(w).length === 1 && ovLines(w)[0].text === "Le niveau est bon."
       && rows(w).length === 1, [w.ids.fileToast.textContent, ovLines(w)]);
    w.ids.tCC.click();
    ok("...and the CC panel shows it chosen", w.ids.ccSources.children.some((b) => b.classList.contains("is-on") && /E01 Pilot\.fr\.srt/.test(b.textContent)));
  }

  // ---- WG-6 (item 6): AudioPB as the Windows player ------------------------------------------------------------------
  const defaultInMore = (w) => {              // ⋯ holds "Make SuVtitle the default player…" in a NORMAL window too
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    w.ids.tMore.click();
    const b = w.ids.morePanel.children.find((x) => /^Make SuVtitle the default player/.test(x.textContent));
    if (!b) return false;
    w.outbox.length = 0;
    b.click();
    return JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerSetDefault" }]);
  };
  section("WG-6 -- SuVtitle as the Windows player: the page's half (⋯, the drop zone) + the launch rules (TrLaunch.h)");
  {
    const launch = ctx.P.cpp("ui/TrLaunch.h"), reader = ctx.P.cpp("ui/SubtitleReader.h");
    ok("the player's ⋯ offers \"Make SuVtitle the default player…\" in a normal window (it asks the app)", defaultInMore(runTranslatorWindow()));
    const we = runTranslatorWindow();
    we.outbox.length = 0;
    we.ids.fileDefaultBtn.click();
    ok("...and the empty player's drop zone too", JSON.stringify(we.outbox) === JSON.stringify([{ type: "playerSetDefault" }])
       && /Make SuVtitle the default player for videos/.test(we.ids.fileDefaultBtn.textContent));
    ok("TrLaunch.h (--play's paths): only ABSOLUTE ones, no wildcard or quote, existing, folder / media / subtitle -- never executed ("
       + ctx.P.cppSource("ui/TrLaunch.h").from + ")",
       /if \(!drive && !unc\) return Kind::Refused;/.test(launch) && /c == L'\*' \|\| c == L'\?'/.test(launch) && !/ShellExecute|CreateProcess|system\(/.test(launch));
    const sr = ((/inline bool isSubtitleExt[\s\S]*?return ([^;]+);/.exec(reader) || ["", ""])[1].match(/L"([a-z]+)"/g) || []).map((x) => x.slice(2, -1)).sort().join();
    const lr = ((/inline bool isSubtitleExtL[\s\S]*?return ([^;]+);/.exec(launch) || ["", ""])[1].match(/L"([a-z]+)"/g) || []).map((x) => x.slice(2, -1)).sort().join();
    ok("TrLaunch's subtitle extensions = SubtitleReader.h's (= the page's SUB_EXTS, WE-14)", sr === "ass,srt,ssa,vtt" && sr === lr, { reader: sr, launch: lr });
  }

  // ---- WG-7 (the coordinator, 11/10: the helper's new cue engine) -- THE CUE ENGINE'S VERSION ---------------------
  const ENGINE_Q = "Subtitles from an older engine — re-analyse? (your edits would be lost)";
  const oldEdited = (o) => fileMsg(Object.assign({ jobId: 6, phase: "done", complete: true, edited: true, oldEngine: true, cached: true,
                                                    mediaKey: "mkOld", langA: "fr", langB: "en" }, o || {}));
  const upgradeSaid = (w) => {                // an upgrade being analysed says so; a first analysis does not
    w.send(fileMsg({ jobId: 3, phase: "analyzing", upgrade: true, doneMs: 60000, totalMs: 600000, ranges: [[0, 60000]] }));
    const a = w.ids.fileStatus.textContent;
    w.send(fileMsg({ jobId: 5, phase: "analyzing", doneMs: 60000, totalMs: 600000, ranges: [[0, 60000]] }));
    return /^Improved subtitles — re-analysing · 10% · 1:00 of 10:00/.test(a) && /^Analysing · 10%/.test(w.ids.fileStatus.textContent);
  };
  const engineAsksOnlyEdited = (w) => {       // asked for an EDITED older result only -- not an unedited one, not mid-analysis
    w.send(oldEdited());
    const asked = w.ids.fileEngine.hidden === false && w.ids.fileEngineText.textContent === ENGINE_Q;
    w.send(oldEdited({ jobId: 7, edited: false }));
    const notUnedited = w.ids.fileEngine.hidden === true;
    w.send(oldEdited({ jobId: 8, phase: "analyzing", complete: false, doneMs: 1000, totalMs: 600000, ranges: [[0, 1000]] }));
    return asked && notUnedited && w.ids.fileEngine.hidden === true;
  };
  const engineKeepRemembered = (w) => {       // "Keep my edits" holds for that file and pair -- across a reload too
    w.send(oldEdited({ mediaKey: "mkKeep" }));
    w.outbox.length = 0;
    w.ids.fileEngineNo.click();
    const quiet = w.outbox.length === 0 && w.ids.fileEngine.hidden === true;
    const w2 = runTranslatorWindow({ ls: w.lsStore });
    w2.send(oldEdited({ mediaKey: "mkKeep" }));
    const still = w2.ids.fileEngine.hidden === true;
    w2.send(oldEdited({ jobId: 9, mediaKey: "mkOther" }));
    return quiet && still && w2.ids.fileEngine.hidden === false;
  };
  section("WG-7 -- ENGINE: an older cue engine's subtitles are analysed again, said (\"Improved subtitles — re-analysing\"); an EDITED result is ASKED about");
  {
    const store = ctx.P.cpp("ui/TrFileStore.h"), player = ctx.P.cpp("ui/TrPlayer.h");
    const pipe = fs.readFileSync(path.join(ctx.P.SUV, "common", "TrPipe.h"), "utf8").replace(/\r\n/g, "\n");
    ok("TrPipe.h's kFileEngineVersion is the one constant both ends compile (here: " + ((/inline constexpr int kFileEngineVersion = (\d+);/.exec(pipe) || ["", "?"])[1]) + ")",
       /inline constexpr int kFileEngineVersion = \d+;/.test(pipe) && /inline bool olderEngine\(int engine\) \{ return engine < tp::kFileEngineVersion; \}/.test(store)
       && /inline bool staleRecord\(int engine, bool edited\) \{ return olderEngine\(engine\) && !edited; \}/.test(store));
    ok("every record carries ENGINE (right after JOB, inside a peek's head), read by the full parse AND the peek",
       /o \+= "ENGINE\\t" \+ std::to_string\(r\.engine\) \+ "\\n";/.test(store)
       && (store.match(/\} else if \(t\[0\] == "ENGINE" && t\.size\(\) >= 2\) \{\n\s+(r|s)\.engine = engineOf\(t\[1\]\);/g) || []).length === 2);
    ok("the core: an older engine's record is a MISS where it is read (the session, the siblings, the badges) unless EDITED; a set saves ITS engine",
       /if \(pbtrf::staleRecord\(r\.engine, r\.edited\)\) \{ S\.upgrade = true; return false; \}\n\s+S\.engine = r\.engine;/.test(player)
       && /if \(pbtrf::staleRecord\(sm\.engine, sm\.edited\)\) continue;/.test(player)
       && /else if \(pbtrf::staleRecord\(s\.engine, s\.edited\)\) s = pbtrf::Summary\{\};/.test(player)
       && /r\.engine = S\.engine;/.test(player) && !/r\.engine = tp::kFileEngineVersion/.test(player));
    ok("the state tells the window: upgrade (analysed again for a better engine) and oldEngine (these cues are an older engine's)",
       /j\.kv\(L"upgrade", S \? S->upgrade : false\);/.test(player) && /j\.kv\(L"oldEngine", S \? pbtrf::olderEngine\(S->engine\) : false\);/.test(player));
    ok("an upgrade being analysed: \"Improved subtitles — re-analysing · 10% · …\" (a first analysis: \"Analysing · …\")", upgradeSaid(runTranslatorWindow()));
    const wl = runTranslatorWindow();
    wl.send(fileMsg({ jobId: 4, phase: "loading", upgrade: true, totalMs: 600000 }));
    ok("...while its models load too", /^Improved subtitles — re-analysing · loading the models… · 10:00 to analyse/.test(wl.ids.fileStatus.textContent),
       wl.ids.fileStatus.textContent);
    const we = runTranslatorWindow();
    we.send(oldEdited());
    we.send(cueMsg(6, 0, 1000, 2000, "fr", "en", "Salut.", "Hi."));
    ok("an EDITED result of an older engine: the strip ASKS -- \"" + ENGINE_Q + "\" -- and says \"· edited · older engine\"",
       we.ids.fileEngine.hidden === false && we.ids.fileEngineText.textContent === ENGINE_Q
       && / · edited · older engine/.test(we.ids.fileStatus.textContent), [we.ids.fileEngine.hidden, we.ids.fileStatus.textContent]);
    we.outbox.length = 0;
    we.ids.fileEngineYes.click();
    ok("...\"Re-analyse\": a FORCED analysis is asked of the tray (the core makes it an upgrade); the question goes",
       JSON.stringify(we.outbox) === JSON.stringify([{ type: "translatorFileAnalyse", force: true, quality: "auto" }]) && we.ids.fileEngine.hidden === true,
       we.outbox);
    ok("...asked only for an EDITED older result, never while it is analysed", engineAsksOnlyEdited(runTranslatorWindow()));
    ok("...\"Keep my edits\": nothing sent, not asked again for that file and pair (even after a reload); another file still is",
       engineKeepRemembered(runTranslatorWindow()));
    const wi = runTranslatorWindow();
    wi.send(oldEdited());
    wi.send(fileMsg({ jobId: 10, seq: 2, phase: "idle", itemId: 0, name: "", url: "", mediaKey: "" }));
    ok("...and nothing is asked with no file", wi.ids.fileEngine.hidden === true);
  }

  // ---- mutation sentinels of the wave -------------------------------------------------------------------------
  section("WG -- mutation sentinels (index.html with one WG guard removed each)");
  {
    const judge = (name, mutate, probe) => {
      if (mutate(html) === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let w;
      try { w = runTranslatorWindow({ mutate, name }); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true, threw = "";
      try { healthy = !!probe(w); } catch (e) { threw = String((e && e.message) || e); }
      if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    const probes = { shortCueHeld, latinHeld, cjkHeld, stackOrder, maxThree, extUntouched, panelClosedByDefault, noFadeInFile,
                     refusedLeavesList, clampAtNext, undoRedo, noFalseGap, asksBeforeReanalysis, defaultInMore,
                     upgradeSaid, engineAsksOnlyEdited, engineKeepRemembered,
                     staleFieldNotCarried, newLineEmptyField, undoTwice, altArrowNoNudge, confirmBound, lateAnswerClears };
    const sick = Object.keys(probes).filter((k) => { try { return !probes[k](runTranslatorWindow()); } catch (e) { return true; } });
    ok("(WG's probes pass on the real page)", sick.length === 0, sick);
    judge("WG-1: no hold (a cue ends at its own end again)",
      (s) => s.replace("c.hold = Math.max(c.e, c.s + readMs(cueTexts(c).main));", "c.hold = c.e;"), shortCueHeld);
    judge("WG-1: Chinese read at the Latin speed",
      (s) => s.replace("READ_CJK_CPS = 7", "READ_CJK_CPS = 17"), cjkHeld);
    judge("WG-1: no cap on the stack",
      (s) => s.replace("return out.length > STACK_MAX ? out.slice(-STACK_MAX) : out;", "return out;"), maxThree);
    judge("WG-1: the stack upside down (the current line above)",
      (s) => s.replace("if (ms < holdEnd(fcues[i])) out.unshift(i);", "if (ms < holdEnd(fcues[i])) out.push(i);"), stackOrder);
    judge("WG-1: the hold applied to subtitle FILES too",
      (s) => s.replace("for (let i = best; i >= 0 && i > best - 12; i--) if (ms < cs[i].e) out.unshift(cs[i]);",
                       "for (let i = best; i >= 0 && i > best - 12; i--) if (ms < Math.max(cs[i].e, cs[i].s + 1000)) out.unshift(cs[i]);"), extUntouched);
    judge("WG-2: the panel OPEN by default again",
      (s) => s.replace('open: p.open === true };', 'open: p.open !== false };'), panelClosedByDefault);
    judge("WG-4: a REFUSED edit applied anyway (the page trusting itself, not the core)",
      (s) => s.replace('if (!msg.ok) { edGiveBack(p); edSay(String(msg.err || "That edit was not applied."), true); edPaint(); return; }\r\n    const rm = new Set((Array.isArray(msg.remove) ? msg.remove : []).map(Number));\r\n    if (rm.size) {',
                       'if (!msg.ok) { edSay(String(msg.err || "That edit was not applied."), true); }\r\n    const rm = new Set((Array.isArray(msg.remove) ? msg.remove : []).map(Number));\r\n    if (rm.size) {')
              .replace('if (!msg.ok) { edGiveBack(p); edSay(String(msg.err || "That edit was not applied."), true); edPaint(); return; }\n    const rm = new Set((Array.isArray(msg.remove) ? msg.remove : []).map(Number));\n    if (rm.size) {',
                       'if (!msg.ok) { edSay(String(msg.err || "That edit was not applied."), true); }\n    const rm = new Set((Array.isArray(msg.remove) ? msg.remove : []).map(Number));\n    if (rm.size) {'), refusedLeavesList);
    judge("WG-4: no clamp at the next line (the page sends an overlap)",
      (s) => s.replace("if (next && (!ext_ || next.s >= c.e) && e > next.s) {", "if (false) {"), clampAtNext);
    judge("WG-4: an undo recorded on the UNDO stack (redo never comes back)",
      (s) => s.replace('if (p.kind === "undo") ed.redo.push(entry);', 'if (p.kind === "undo") ed.undo.push(entry);'), undoRedo);
    judge("WG-4: the gap rule back to the COUNT (a deletion asks for a full replay)",
      (s) => s.replace("Number(msg.idx) > maxIdxSeen + 1", "Number(msg.idx) > fcues.length"), noFalseGap);
    judge("WG-4: Re-analyse over an EDITED file without asking",
      (s) => s.replace("if (force && F.edited) { confirmJob = F.jobId; el.fileConfirm.hidden = false; return; }", ""), asksBeforeReanalysis);
    judge("WG-6: \"Make SuVtitle the default player…\" only in a NARROW window's ⋯",
      (s) => s.replace('    items.push(["Make SuVtitle the default player…", () => send({ type: "playerSetDefault" })]);',
                       '    if (isNarrow()) items.push(["Make SuVtitle the default player…", () => send({ type: "playerSetDefault" })]);'), defaultInMore);
    judge("WG-7 ENGINE: an upgrade reads as a first analysis (\"Analysing\")",
      (s) => s.replace('text = (F.upgrade ? UPGRADE_TEXT : "Analysing")', 'text = ("Analysing")'), upgradeSaid);
    judge("WG-7 ENGINE: an UNEDITED older result asked about too",
      (s) => s.replace("const engineAsks = () => F.oldEngine && F.edited && F.phase", "const engineAsks = () => F.oldEngine && F.phase"), engineAsksOnlyEdited);
    judge("WG-7 ENGINE: asked while it is being analysed",
      (s) => s.replace('F.phase !== "idle" && !fileRunning() && !engineKept();', 'F.phase !== "idle" && !engineKept();'), engineAsksOnlyEdited);
    judge("WG-7 ENGINE: \"Keep my edits\" forgotten (asked again and again)",
      (s) => s.replace("btn(el.fileEngineNo, () => { engineKeep(); el.fileEngine.hidden = true; });", "btn(el.fileEngineNo, () => { el.fileEngine.hidden = true; });"),
      engineKeepRemembered);
    judge("WG-R #1: a focused field keeps the PREVIOUS line's text when another line is selected",
      (s) => s.replace("const other = (focus, node) => focus !== node || ed.shown !== ed.sel;", "const other = (focus, node) => focus !== node;"),
      newLineEmptyField);
    judge("WG-R #1: the typed fix not committed before the selection moves",
      (s) => s.replace("function edSelect(key) { if (key !== ed.sel) edFlush(); ed.sel = key;", "function edSelect(key) { ed.sel = key;"),
      staleFieldNotCarried);
    judge("WG-R #1: the answer drags the selection back to the edited line",
      (s) => s.replace("if (ed.sel === p.selAt || ed.sel == null || edFind(ed.sel) < 0)", "if (true)"), staleFieldNotCarried);
    judge("WG-R #2: the history never re-pointed (the 2nd undo names a dead key)",
      (s) => s.replace("    if (!map.size) return;\n    const fix = (x) =>", "    return;\n    const fix = (x) =>")
              .replace("    if (!map.size) return;\r\n    const fix = (x) =>", "    return;\r\n    const fix = (x) =>"), undoTwice);
    judge("WG-R #6: Alt+arrows in a time field nudge it as well",
      (s) => s.replace("      if (e.altKey || e.ctrlKey || e.metaKey) return;   // Alt+", "      // Alt+"), altArrowNoNudge);
    judge("WG-R #7: the confirm bar outlives its file",
      (s) => s.replace("    if (!el.fileConfirm.hidden && confirmJob !== F.jobId) el.fileConfirm.hidden = true;", "")
              .replace("    if (confirmJob !== F.jobId) return;\n", "").replace("    if (confirmJob !== F.jobId) return;\r\n", ""), confirmBound);
    judge("WG-R #13: a late answer leaves the history as it was",
      (s) => s.replace("    if (p || !msg.ok) return;\n    ed.undo = [];", "    return;\n    ed.undo = [];")
              .replace("    if (p || !msg.ok) return;\r\n    ed.undo = [];", "    return;\r\n    ed.undo = [];"), lateAnswerClears);
    // (641f1aa's guard was a line of the Live window's fade -- "never in File mode"; the fade itself is gone now, so the
    //  sentinels put one back -- as a STYLE, the only thing a viewer sees: RV4-14 -- and the check must see each)
    judge("WG-2: a fade comes back (the bar's opacity to 0, 3 s after the boot)",
      (s) => s.replace("  function initPlayer() {\n", "  function initPlayer() {\n    setTimeout(() => { el.bar.style.opacity = \"0\"; }, 3000);\n"), noFadeInFile);
    judge("WG-2: ...or the page goes into its full-screen styling by itself (body.is-full hides the bar)",
      (s) => s.replace("  function initPlayer() {\n", "  function initPlayer() {\n    setTimeout(() => document.body.classList.add(\"is-full\"), 3000);\n"), noFadeInFile);
  }
};

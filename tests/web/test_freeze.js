// test_freeze.js -- THE FREEZE WATCH (25/10/2026): the page reports a freeze of its own to the app ({type:"playerDiag"}),
// which writes one line of %LOCALAPPDATA%\SuVtitle\player.log (bridge.cpp handlePlayerDiag). Required by test_ui.js (same
// ok/section counters). Every check runs the REAL page (app/web/index.html) with a fake app; each rule has a MUTATION
// SENTINEL at the end -- the page with that rule taken out must FAIL its probe.
// Why it exists: the user, SuVtitle 1.0.0 installed, full screen on his own screen -- « en plein écran presque tous [les
// sous-titres manquent] et certains même en fenêtré », and moving the mouse brought back neither the play bar nor the
// cursor. Not reproduced on the test screen (docs/DIAG_EMBEDDED_SUBS.md); its signature is the page stopping while the
// picture plays on. The next test installer carries this watch, so that his next freeze leaves a line saying what froze.
// What this proves: WHEN the page reports -- a frame gap over a second while a film plays (not across a pause, nor across a
// hidden page), a pointer event heard over a second late (once per 5 s), every visibility change -- and what it says. What
// it cannot: whether a real Chromium freezes on his machine, nor the line the app writes (the real app does: tests\e2e).
"use strict";
module.exports = function freezeChecks(ctx) {
  const { ok, section, runTranslatorWindow, fileMsg, metaOk, script } = ctx;

  const open = (w) => {
    w.send(fileMsg({ jobId: 2, seq: 1, itemId: 12, phase: "done", complete: true, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.pumpTimers(0);
    return w;
  };
  const diags = (w) => w.outbox.filter((m) => m && m.type === "playerDiag");
  const kinds = (w) => diags(w).map((m) => m.kind + (m.ms ? ":" + m.ms : "")).join(",");
  // a film playing, its frame loop running (one frame painted), the clock past the first seconds, the outbox emptied
  const playing = (w) => {
    open(w);
    w.pumpTimers(3000);
    w.ids.tPlay.click();
    w.pumpRaf();
    w.outbox.length = 0;
    return w;
  };
  const setVis = (w, v) => { w.doc.visibilityState = v; w.dispatchDoc("visibilitychange", { target: w.doc }); };
  const move = (w, lateMs) => w.dispatchDoc("pointermove", { target: w.ids.fileVideo, clientX: 50, clientY: 50, timeStamp: w.now() - lateMs });

  // ---- the probes (each is a check below AND the condition its mutation sentinel must see FAIL) ---------------------------
  // 1. a frame gap of 1.5 s while the film plays: ONE line, with its length and what the page knew
  const framesGap = (w) => {
    playing(w);
    w.pumpTimers(1500);
    w.pumpRaf();
    const d = diags(w);
    return d.length === 1 && d[0].kind === "frames" && d[0].ms === 1500 && d[0].playing === true && d[0].full === false
      && d[0].shown === true && d[0].render === "blend" && typeof d[0].sub === "string";
  };
  // 2. ...said in full screen as full screen, with the mode the picture is DRAWN in there ("plain": see 7)
  const framesFull = (w) => {
    playing(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.pumpRaf();
    w.outbox.length = 0;
    w.pumpTimers(2000);
    w.pumpRaf();
    const d = diags(w);
    return d.length === 1 && d[0].kind === "frames" && d[0].full === true && d[0].render === "plain";
  };
  // 3. a gap under a second is a page painting, not a freeze: nothing
  const shortGap = (w) => { playing(w); w.pumpTimers(800); w.pumpRaf(); w.pumpTimers(900); w.pumpRaf(); return kinds(w) === ""; };
  // 4. a PAUSE is no freeze: the loop stops (and forgets its last frame), the next play starts a new count
  const pauseGap = (w) => {
    playing(w);
    w.ids.tPlay.click();                     // pause
    w.pumpRaf();                             // the loop's last turn: paused, it stops
    w.pumpTimers(5000);
    w.ids.tPlay.click();                     // play again
    w.pumpRaf();
    w.pumpTimers(16);
    w.pumpRaf();
    return w.ids.fileVideo.paused === false && kinds(w) === "";
  };
  // 5. a HIDDEN page paints nothing: hidden then visible = those two lines, never a frame gap
  const hiddenGap = (w) => {
    playing(w);
    setVis(w, "hidden");
    w.pumpTimers(5000);
    setVis(w, "visible");
    w.pumpRaf();
    return kinds(w) === "hidden,visible";
  };
  // 6. an event on time: nothing; a pointer event heard 1.5 s after it happened: one "input" line; the next ones of the same
  //    freeze (within 5 s) nothing; a late one 6 s later its own line
  const inputLate = (w) => {
    playing(w);
    move(w, 10);
    const a = kinds(w);
    move(w, 1500);
    const b = kinds(w);
    move(w, 1400);
    const c = kinds(w);
    w.pumpTimers(6000);
    w.dispatchDoc("pointerdown", { target: w.ids.fileVideo, button: 0, timeStamp: w.now() - 2000 });
    return a === "" && b === "input:1500" && c === "input:1500" && kinds(w).startsWith("input:1500,input:2000");
  };

  // 7. THE STANDARD MODE IS DIRECT IN FULL SCREEN (25/10, measured live with the user: "blend" and "canvas" froze his screen
  //    in full screen, "plain" did not and kept the subtitles): the CHOICE stays "standard", full screen DRAWS "plain",
  //    leaving it draws "blend" again; nothing is written to the stored choice
  const rendered = (w) => (w.ids.fileView.classList.contains("render-plain") ? "plain"
    : w.ids.fileView.classList.contains("render-canvas") ? "canvas" : "blend");
  const fullDrawsPlain = (w) => {
    open(w);
    const a = rendered(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    const b = rendered(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    const c = rendered(w);
    return a === "blend" && b === "plain" && c === "blend" && !(w.lsStore["audiopb.tr.render"] || "").length;
  };
  // 8. ...and a mode the viewer CHOSE (Ctrl+Alt+V) too (26/10, his 1.1.0 install: « toujours pas le mode canvas, qui a le
  //    même problème de freeze »): canvas windowed, plain in full screen, canvas again on leaving -- his choice never rewritten
  const choiceKept = (w) => {
    open(w);
    w.win.dispatch("keydown", { key: "v", ctrlKey: true, altKey: true, target: w.doc.body });   // standard -> canvas
    const a = rendered(w);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    const b = rendered(w), hidden = w.ids.fileCanvas.hidden === true;
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    const c = rendered(w);
    return a === "canvas" && b === "plain" && hidden && c === "canvas" && w.lsStore["audiopb.tr.render"] === "canvas";
  };

  section("THE FREEZE WATCH (25/10) -- the page reports its own freezes (playerDiag -> player.log)");
  ok("a frame gap of 1.5 s while the film plays: ONE line (frames, 1500 ms, playing, windowed, shown, render blend)", framesGap(runTranslatorWindow({})));
  ok("...in full screen it says full screen", framesFull(runTranslatorWindow({})));
  ok("gaps under a second (0.8 s, 0.9 s): nothing", shortGap(runTranslatorWindow({})));
  ok("a pause of 5 s then play: no frame gap (the loop forgot its last frame when it stopped)", pauseGap(runTranslatorWindow({})));
  ok("hidden for 5 s then visible: the two visibility lines, no frame gap", hiddenGap(runTranslatorWindow({})));
  ok("a pointer event heard 1.5 s late: one input line; the rest of that freeze (within 5 s) and an on-time event nothing; a later one its own",
     inputLate(runTranslatorWindow({})));
  section("THE STANDARD MODE IS DIRECT IN FULL SCREEN (25/10, his screen froze in full screen with blend and canvas, not plain)");
  ok("standard: windowed draws blend, full screen draws plain, back to blend on leaving -- the stored choice untouched", fullDrawsPlain(runTranslatorWindow({})));
  ok("a mode chosen with Ctrl+Alt+V (canvas): canvas windowed, plain in full screen (its canvas hidden), canvas again -- the choice kept", choiceKept(runTranslatorWindow({})));

  // ---- MUTATION SENTINELS: each rule taken out of the page must be CAUGHT by its probe ----------------------------------------
  section("THE FREEZE WATCH -- mutation sentinels (index.html with one rule taken out each)");
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
    judge("F1: the frame loop never measured", R("    rafId = 0;\n    noteFrame();\n", "    rafId = 0;\n"), framesGap);
    judge("F1: the gap reported without its length", R("noteFreeze(\"frames\", now - freezeFrame)", "noteFreeze(\"frames\", 0)"), framesGap);
    judge("F2: full screen not said", R("ms: Math.max(0, Math.round(ms)), full: isFull,", "ms: Math.max(0, Math.round(ms)), full: false,"), framesFull);
    judge("F3: no threshold (every frame a freeze)", R("  const FREEZE_MS = 1000;", "  const FREEZE_MS = 0;"), shortGap);
    judge("F4: a pause counted as a freeze (the stopped loop keeps its last frame)",
      R("    else freezeFrame = 0;                      // the loop stops here: its next start is no frozen page\n", ""), pauseGap);
    judge("F5: a hidden page counted as a freeze", R("    freezeFrame = 0;                           // (a hidden page paints nothing: that gap is no freeze)\n", ""),
      hiddenGap);
    judge("F5: the visibility changes not said", R("    noteFreeze(pageVisible() ? \"visible\" : \"hidden\", 0);\n", ""), hiddenGap);
    judge("F6: every late event of one freeze a line of its own", R(" || now - inputNoted < 5000) return;", ") return;"), inputLate);
    judge("F6: a move heard late never said", R("    noteInput(e);                              // (THE FREEZE WATCH: a move the page heard late)\n", ""),
      inputLate);
    judge("F6: a click heard late never said", R("document.addEventListener(t, (e) => { noteInput(e); if (isFull) showControls(); },",
      "document.addEventListener(t, (e) => { if (isFull) showControls(); },"), inputLate);
    judge("F6: an event on time taken for a late one", R("now - t <= FREEZE_MS ||", "now - t < 0 ||"), inputLate);
    judge("F2: the line says the CHOSEN mode, not the one drawn", R("render: renderNow(), sub: subSrc", "render: renderMode, sub: subSrc"), framesFull);
    // (26/10, 1.1.0 THE NATIVE VIDEO ENGINE: renderNow says "native" first while mpv draws the picture -- the line these three
    //  mutate is that one now; test_native.js holds the native mode's own checks)
    const RN = "  function renderNow() { return role === \"native\" ? \"native\" : isFull ? \"plain\" : renderMode; }";
    judge("R1: full screen draws blend (the frozen screen)", R(RN,
      "  function renderNow() { return role === \"native\" ? \"native\" : renderMode; }"), fullDrawsPlain);
    judge("R1: entering full screen does not redraw", R("    applyRender();                             // (25/10: full screen draws \"plain\"", "    // (25/10: full screen draws \"plain\""),
      fullDrawsPlain);
    judge("R2: a canvas choice kept in full screen (26/10: it froze his screen like blend)", R("isFull ? \"plain\" : renderMode; }",
      "renderMode === \"blend\" && isFull ? \"plain\" : renderMode; }"), choiceKept);
    judge("R2: the canvas choice REWRITTEN by full screen (it must come back windowed)", R(RN,
      "  function renderNow() { if (role === \"native\") return \"native\"; if (isFull && renderMode !== \"plain\") { renderMode = \"plain\"; write(KEY_RENDER, renderMode); } return renderMode; }"), choiceKept);
  }
};

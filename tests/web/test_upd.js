// test_upd.js -- THE UPDATE BANNER (03/11/2026): the page's half (app/web/index.html, the section of that name). Required by
// test_ui.js (same ok/section counters) -- and runnable alone: node tests\web\test_upd.js. Every check runs the REAL page
// through run_ui.js with a fake app speaking update.status / update.opened, and each rule has a MUTATION SENTINEL at the end:
// the page with that rule taken out must FAIL its probe. The app's half is RUN by tests\cpp\update_smoke.cpp (NextaleUpdate.cpp,
// AppConfig.h; sentinels tests\cpp\cmut_upd.py) and PINNED by tests\web\test_host_pins.js UPD-1..UPD-6 (bridge.cpp).
//   1 nothing before the app speaks; the answer drawn (a strip at the top of the pane, never in full screen, never over the
//     picture), its words by textContent
//   2 "Download" = update.open{} and nothing else (the app builds what it opens: SuVtitle's page); update.opened said in the strip
//   3 Later (this run: a page reloaded stays told) and Skip
//   4 THE UPDATES DIALOG: ⋯ "Updates…" and the empty window's link; this version, a newer / skipped one with its Download and
//     "Offer it again"; the opt-out (update.enable{on}); its keyboard is its own (Esc, nothing reaches the player)
//   5 EXACTLY THEIR CONTROLS: the banner's three buttons; the dialog's title, version line, Download, "Offer it again", checkbox
//     and Close -- nothing more; every update.* message the page sends is its type alone (update.enable: + on)
//   6 the contract both ways, the theme's tokens, the design mock (on demand only)
"use strict";

function updChecks(ctx) {
  const { ok, section, runTranslatorWindow, html, script, bridgeSrc, fileMsg, metaOk } = ctx;
  const J = JSON.stringify;
  const PAGE_URL = "https://www.nextale-audio.com/suvtitle?update=1.2.0&from=1.1.0";
  const sent = (w, t) => w.outbox.filter((m) => m && m.type === t);
  const types = (w) => w.outbox.map((m) => m && m.type);
  const ST = (o) => Object.assign({ type: "update.status", enabled: true, available: false, current: "1.1.0" }, o);
  const NEWER = (o) => ST(Object.assign({ available: true, version: "1.2.0", notes: "Faster live mode", later: false }, o));
  const moreItems = (w) => { w.ids.tMore.click(); const items = w.ids.morePanel.children.map((b) => b.textContent); w.ids.tMore.click(); return items; };
  const moreItem = (w, re) => { w.ids.tMore.click(); return w.ids.morePanel.children.find((b) => re.test(b.textContent)) || null; };
  // under a node, in document order: every control (tag:type#id -- a button, a field, a list, a link) / every element's id
  const walkAll = (n, fn) => { if (!n) return; fn(n); (n.children || []).forEach((x) => walkAll(x, fn)); };
  const controlsOf = (n) => {
    const out = [];
    walkAll(n, (x) => {
      if (/^(BUTTON|INPUT|SELECT|TEXTAREA|A)$/i.test(String(x.tagName || "")))
        out.push(String(x.tagName).toLowerCase() + ":" + String(x.type || "") + "#" + String(x.id || ""));
    });
    return out.join(",");
  };
  const idsOf = (n) => { const out = []; walkAll(n, (x) => { if (x !== n && x.id) out.push(x.id); }); return out.join(","); };

  // ---- the probes (each a check here AND the condition its sentinel must see fail) --------------------------------------
  // 1a: the boot -- `ready` then update.check; nothing shown, no ⋯ entry, before the app answers
  const silentBoot = (w) => J(types(w)) === J(["ready", "update.check"]) && w.ids.updBanner.hidden && w.ids.updLink.hidden
    && !moreItems(w).some((t) => /Updates/.test(t));
  // 1b: "nothing newer": no banner; the link and ⋯ "Updates…" appear (the app answered)
  const nothingNewer = (w) => {
    w.send(ST());
    return w.ids.updBanner.hidden && !w.ids.updLink.hidden && w.ids.updLink.textContent === "Updates…"
      && moreItems(w).indexOf("Updates…") >= 0;
  };
  // 1c: a newer version: the strip, its words (textContent), "Download" (its tooltip: SuVtitle's page), its ⋯ entry names it
  const drawn = (w) => {
    w.send(NEWER({ notes: "Faster <b>live</b> mode" }));
    const t = w.ids.updText.textContent;
    return !w.ids.updBanner.hidden && t === "SuVtitle 1.2.0 is available — Faster <b>live</b> mode" && w.ids.updText.title === t
      && w.ids.updText.children.length === 0 && w.ids.updGet.textContent === "Download" && !w.ids.updGet.hidden
      && /^Opens SuVtitle's page on www\.nextale-audio\.com in your browser, to get SuVtitle 1\.2\.0/.test(w.ids.updGet.title)
      && moreItems(w).indexOf("Updates… (SuVtitle 1.2.0 is available)") >= 0 && w.ids.updLink.textContent === "Updates… (SuVtitle 1.2.0 is available)";
  };
  // 1d: the opt-out off (enabled:false): no banner whatever else is said
  const offSaysNothing = (w) => { w.send(NEWER({ enabled: false })); return w.ids.updBanner.hidden && !w.ids.updLink.hidden; };
  // 2a: "Download" = update.open{} -- exactly that, the message bare (the app builds what it opens)
  const download = (w) => {
    w.send(NEWER());
    w.outbox.length = 0;
    w.ids.updGet.click();
    return J(w.outbox) === J([{ type: "update.open" }]);
  };
  // 2b: update.opened, said in the strip: the page open / where it is (not opened: the url the app names, else the site);
  //     another version: its own news again
  const opened = (w) => {
    w.send(NEWER());
    w.send({ type: "update.opened", ok: true, url: PAGE_URL });
    const a = w.ids.updText.textContent === "SuVtitle's page is open in your browser: get SuVtitle 1.2.0 there, then run its installer — it replaces this version.";
    w.send({ type: "update.opened", ok: false, url: PAGE_URL });
    const b = w.ids.updText.textContent === "Your browser could not be opened: SuVtitle 1.2.0 is on " + PAGE_URL + ".";
    w.send({ type: "update.opened", ok: false, url: "" });
    const c = w.ids.updText.textContent === "Your browser could not be opened: SuVtitle 1.2.0 is on www.nextale-audio.com.";
    w.send(NEWER({ version: "1.3.0" }));                                    // another version: its own news again
    return a && b && c && /^SuVtitle 1\.3\.0 is available/.test(w.ids.updText.textContent);
  };
  // 3a: Later -> update.later{} and the strip gone
  const later = (w) => {
    w.send(NEWER());
    w.outbox.length = 0;
    w.ids.updLater.click();
    return J(w.outbox) === J([{ type: "update.later" }]) && w.ids.updBanner.hidden;
  };
  // 3a': a page RELOADED (a new folder: a fresh page) told later:true shows nothing -- ⋯ still names the version
  const laterReload = (w) => {
    w.send(NEWER({ later: true }));
    return w.ids.updBanner.hidden && moreItems(w).indexOf("Updates… (SuVtitle 1.2.0 is available)") >= 0;
  };
  // 3b: Skip -> update.skip{} and the strip gone
  const skip = (w) => {
    w.send(NEWER());
    w.outbox.length = 0;
    w.ids.updSkip.click();
    return J(w.outbox) === J([{ type: "update.skip" }]) && w.ids.updBanner.hidden;
  };
  // 4a: the dialog from ⋯ (the version line, its Download = update.open{}, said there once answered; the checkbox)
  const dialog = (w) => {
    w.send(NEWER());
    const item = moreItem(w, /^Updates/);
    if (!item) return false;
    item.click();
    const open = !w.ids.updDlg.hidden && !w.ids.updDlgBack.hidden && w.ids.morePanel.hidden && w.doc.activeElement === w.ids.udClose;
    const words = w.ids.udNow.textContent === "This is SuVtitle 1.1.0. SuVtitle 1.2.0 is available — Faster live mode"
      && !w.ids.udGetRow.hidden && !w.ids.udGet.hidden && w.ids.udGet.textContent === "Download" && w.ids.udUnskip.hidden
      && w.ids.udCheck.checked === true;
    w.outbox.length = 0;
    w.ids.udGet.click();
    const sentOpen = J(w.outbox) === J([{ type: "update.open" }]);
    w.send({ type: "update.opened", ok: true, url: PAGE_URL });
    return open && words && sentOpen && /^This is SuVtitle 1\.1\.0\. SuVtitle's page is open in your browser/.test(w.ids.udNow.textContent);
  };
  // 4e (review 03/11, S5): "Skip this version" is UNDONE from the dialog -- the skipped version is still NAMED there (never "no
  //     newer version"), with its Download and "Offer it again" = update.unskip{}; the app's answer brings the banner back
  const skipUndo = (w) => {
    w.send(NEWER());
    w.ids.updSkip.click();
    w.send(ST({ available: false, skipped: true, version: "1.2.0", notes: "Faster live mode", later: false }));
    const gone = w.ids.updBanner.hidden && w.ids.updLink.textContent === "Updates…";
    w.ids.updLink.click();
    const named = /SuVtitle 1\.2\.0 is available — you chose to skip it/.test(w.ids.udNow.textContent)
      && !/No newer version/.test(w.ids.udNow.textContent) && !w.ids.udGetRow.hidden && !w.ids.udGet.hidden && !w.ids.udUnskip.hidden;
    w.outbox.length = 0;
    w.ids.udUnskip.click();
    const asked = J(w.outbox) === J([{ type: "update.unskip" }]);
    w.send(NEWER());
    return gone && named && asked && !w.ids.updBanner.hidden && w.ids.udUnskip.hidden;
  };
  // 4f (S5): a skipped version is opened on purpose from the dialog: Download = update.open{}, its answer said there
  const skippedOpens = (w) => {
    w.send(ST({ available: false, skipped: true, version: "1.2.0", notes: "" }));
    w.ids.updLink.click();
    w.outbox.length = 0;
    w.ids.udGet.click();
    const sentOpen = J(w.outbox) === J([{ type: "update.open" }]);
    w.send({ type: "update.opened", ok: true, url: PAGE_URL });
    return sentOpen && /SuVtitle's page is open in your browser/.test(w.ids.udNow.textContent) && w.ids.updBanner.hidden;
  };
  // 4g (review 03/11, N3): ONE update.open at a time -- a double-click sends one; the buttons wait (disabled) for update.opened;
  //     answered, they work again; with no answer, 5 s later too
  const oneOpen = (w) => {
    w.send(NEWER());
    w.outbox.length = 0;
    w.ids.updGet.click();
    w.ids.updGet.click();                                                   // (the stub clicks a disabled button too: the guard must hold)
    const one = J(w.outbox) === J([{ type: "update.open" }]) && w.ids.updGet.disabled === true && w.ids.udGet.disabled === true;
    w.send({ type: "update.opened", ok: true, url: "" });
    const back = w.ids.updGet.disabled === false && w.ids.udGet.disabled === false;
    w.ids.updGet.click();
    const again = w.outbox.length === 2;
    w.pumpTimers(5100);                                                     // (no answer this time: usable again after 5 s)
    const free = w.ids.updGet.disabled === false;
    w.ids.updGet.click();
    return one && back && again && free && w.outbox.length === 3;
  };
  // 4b: the opt-out: unchecked -> update.enable{on:false}, the strip gone; the app's answer: "does not check"; checked again -> on:true
  const optOut = (w) => {
    w.send(NEWER());
    moreItem(w, /^Updates/).click();
    w.outbox.length = 0;
    w.ids.udCheck.checked = false;
    w.ids.udCheck.dispatch("change");
    const off = J(w.outbox) === J([{ type: "update.enable", on: false }]) && w.ids.updBanner.hidden;
    w.send(ST({ enabled: false }));
    const said = w.ids.udNow.textContent === "This is SuVtitle 1.1.0. SuVtitle does not check for updates." && w.ids.udCheck.checked === false;
    w.outbox.length = 0;
    w.ids.udCheck.checked = true;
    w.ids.udCheck.dispatch("change");
    return off && said && J(w.outbox) === J([{ type: "update.enable", on: true }]);
  };
  // 4d: its keyboard is ITS: a key pressed while it is up -- the focus on one of its buttons, where the player's keys would act --
  //     never reaches the player (f: no full screen, n: no next file); Esc closes it, nothing sent, the focus back where it came
  //     from; its backdrop closes it too
  const dialogKeys = (w) => {
    w.send(fileMsg({ jobId: 3, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.send(ST());
    moreItem(w, /^Updates/).click();
    w.outbox.length = 0;
    w.win.dispatch("keydown", { key: "f", target: w.ids.udClose });
    w.win.dispatch("keydown", { key: "n", target: w.ids.udClose });
    const quiet = w.outbox.length === 0;
    w.win.dispatch("keydown", { key: "Escape", target: w.ids.udClose });
    const closed = w.ids.updDlg.hidden && w.ids.updDlgBack.hidden && w.outbox.length === 0 && w.doc.activeElement === w.ids.tMore;
    moreItem(w, /^Updates/).click();
    w.ids.updDlgBack.click();
    return quiet && closed && w.ids.updDlg.hidden;
  };
  // 5: EXACTLY THEIR CONTROLS: the banner holds its words and three buttons (Download, Later, Skip this version); the dialog its
  //    title, its version line, the row of Download and "Offer it again", its checkbox and Close -- no other control, field or
  //    element; and after a whole session (Download and its answer, the dialog, Skip, the dialog on the skipped version, the
  //    opt-out) every update.* message the page sent is its type alone, update.enable with its `on`
  const controls = (w) => {
    const banner = controlsOf(w.ids.updBanner), dlg = controlsOf(w.ids.updDlg), ids = idsOf(w.ids.updDlg);
    w.send(NEWER());
    w.ids.updGet.click();
    w.send({ type: "update.opened", ok: true, url: PAGE_URL });
    w.ids.updLink.click();
    w.ids.udClose.click();
    w.ids.updSkip.click();
    w.send(ST({ available: false, skipped: true, version: "1.2.0", notes: "" }));
    w.ids.updLink.click();
    w.ids.udCheck.checked = false;
    w.ids.udCheck.dispatch("change");
    const msgs = w.outbox.filter((m) => m && /^update\./.test(m.type));
    return banner === "button:button#updGet,button:button#updLater,button:button#updSkip"
      && dlg === "button:button#udGet,button:button#udUnskip,input:checkbox#udCheck,button:button#udClose"
      && ids === "udTitle,udNow,udGetRow,udGet,udUnskip,udCheck,udClose"
      && J(msgs.map((m) => m.type)) === J(["update.check", "update.open", "update.skip", "update.enable"])
      && msgs.every((m) => Object.keys(m).every((k) => k === "type" || (m.type === "update.enable" && k === "on")));
  };
  // 6: the design mock (a plain browser): nothing newer unless asked; ?update=1.2.0 shows the banner and its Download answers
  //    as the app would
  const mockOnDemand = (run) => {
    const a = run({ noWebView: true });
    const b = run({ noWebView: true, location: { search: "?update=1.2.0" } });
    b.ids.updGet.click();
    return a.ids.updBanner.hidden && !a.ids.updLink.hidden && !b.ids.updBanner.hidden && b.ids.updGet.textContent === "Download"
      && /^SuVtitle's page is open in your browser/.test(b.ids.updText.textContent) && a.outbox.length === 0 && b.outbox.length === 0;
  };
  // the strip's place and full screen: the FIRST child of the pane, above the player (it pushes it down: never over the picture or
  // the subtitles); gone in full screen; the popovers' idle hold counts the dialog
  const placed = (h) => {
    const pane = (/<section id="filePane">([\s\S]*?)<\/section>/.exec(h) || ["", ""])[1];
    return pane.indexOf('id="updBanner"') >= 0 && pane.indexOf('id="updBanner"') < pane.indexOf('id="fileEmpty"')
      && pane.indexOf('id="updBanner"') < pane.indexOf('id="filePlayer"')
      && /\n    body\.is-full \.upd-banner \{ display: none; \}\n/.test(h) && /\.upd-banner\[hidden\] \{ display: none; \}/.test(h)
      && /const popOpen = \(\) => [^\n]*!el\.updDlg\.hidden/.test(h);
  };

  section("UPD-1 -- the banner: nothing before the app speaks; its answer drawn at the top of the pane, never in full screen");
  ok("boot: `ready`, THEN update.check (the app asks the site once per start, never during it); nothing shown, no ⋯ entry, before "
     + "the app answers", silentBoot(runTranslatorWindow()));
  ok("\"nothing newer\": no banner -- the empty window's \"Updates…\" link and ⋯ \"Updates…\" appear", nothingNewer(runTranslatorWindow()));
  ok("a newer version: the strip says \"SuVtitle 1.2.0 is available — <notes>\" (textContent: a <b> in the notes stays text; the whole "
     + "sentence is the tooltip), its button \"Download\" (its tooltip: SuVtitle's page on www.nextale-audio.com); ⋯ and the link name it",
     drawn(runTranslatorWindow()));
  ok("the check turned off (enabled:false): no banner, whatever else is said", offSaysNothing(runTranslatorWindow()));
  ok("the strip is the pane's FIRST child, above the drop zone and the player (it pushes them down: never over the picture or the "
     + "subtitles), gone in full screen (body.is-full), [hidden] honoured; the dialog holds full screen's idle hide (popOpen)", placed(html));
  section("UPD-2 -- \"Download\": update.open{} and nothing else (the app opens SuVtitle's page)");
  ok("\"Download\" = update.open{} -- exactly that, the message bare (the app builds what it opens)", download(runTranslatorWindow()));
  ok("update.opened said in the strip: the page open; where it is when the browser could not be opened (the app's url, else the "
     + "site); another version: its own news again", opened(runTranslatorWindow()));
  section("UPD-3 -- Later (this run) and Skip this version");
  ok("Later = update.later{} and the strip gone", later(runTranslatorWindow()));
  ok("...a page RELOADED (a new folder) told later:true shows nothing -- ⋯ still names the version", laterReload(runTranslatorWindow()));
  ok("Skip this version = update.skip{} and the strip gone", skip(runTranslatorWindow()));
  section("UPD-4 -- THE UPDATES DIALOG: ⋯ and the empty window's link; Download; the opt-out; its own keyboard");
  ok("from ⋯: open (⋯ closed, the focus on Close); this version and the newer one; \"Download\" = update.open{}, its answer said there; "
     + "the checkbox on", dialog(runTranslatorWindow()));
  ok("the opt-out: unchecked = update.enable{on:false} (the strip gone), the app's answer \"SuVtitle does not check for updates.\"; "
     + "checked again = on:true", optOut(runTranslatorWindow()));
  ok("its keyboard is its own: f and n pressed in it reach no player (no full screen, no next file); Esc closes it (nothing sent, "
     + "the focus back on ⋯); its backdrop closes it", dialogKeys(runTranslatorWindow({ width: 1280, height: 800 })));
  ok("(review 03/11, S5) Skip UNDONE: the skipped version stays NAMED in the dialog (\"you chose to skip it\", never \"no newer "
     + "version\") with Download and \"Offer it again\" = update.unskip{}; the app's answer brings the banner back", skipUndo(runTranslatorWindow()));
  ok("(S5) a skipped version is opened ON PURPOSE from the dialog: Download = update.open{}, its answer said there; the banner stays "
     + "away", skippedOpens(runTranslatorWindow()));
  ok("(review 03/11, N3) ONE update.open at a time: a double-click sends one, the buttons disabled until update.opened; answered, "
     + "they work again; unanswered, after 5 s", oneOpen(runTranslatorWindow()));
  section("UPD-5 -- exactly their controls; every update.* message bare");
  ok("the banner holds Download, Later and Skip this version; the dialog its title, version line, Download, \"Offer it again\", "
     + "checkbox and Close -- nothing more; after a whole session every update.* message the page sent is its type alone "
     + "(update.enable: + on)", controls(runTranslatorWindow()));
  section("UPD-6 -- the contract both ways, the theme, the design mock");
  {
    const doorsPage = [...new Set([...script.matchAll(/send\(\{\s*type:\s*"(update\.\w+)"/g)].map((m) => m[1]))];
    const handled = (t) => bridgeSrc.indexOf('type == L"' + t + '"') >= 0;
    ok("every update.* message the page sends is handled by the app (bridge.cpp's six doors)",
       doorsPage.length === 6 && doorsPage.every(handled), { sent: doorsPage, unhandled: doorsPage.filter((t) => !handled(t)) });
    const emitted = [...new Set([...bridgeSrc.matchAll(/L"type",\s*L"(update\.\w+)"/g)].map((m) => m[1]))];
    ok("every update.* message the app emits is consumed by the page (update.status, update.opened)",
       emitted.length === 2 && emitted.every((t) => script.indexOf('msg.type === "' + t + '"') >= 0), emitted);
    const contract = ["update.check{}", "update.open{}", "update.later{}", "update.skip{}", "update.unskip{}", "update.enable{on}",
                      "update.status{enabled, available, version, notes, later, current, skipped}", "update.opened{ok, url, test?}"];
    ok("the page's contract block names them", contract.every((c) => html.indexOf(c) >= 0), contract.filter((c) => html.indexOf(c) < 0));
    const css = ((/\/\* ---- THE UPDATE BANNER \(03\/11\/2026\)[\s\S]*?\/\* ---- \(end THE UPDATE BANNER\) ---- \*\//.exec(html)) || [""])[0];
    ok("both themes: the banner's and the dialog's CSS take their colours from the theme's tokens only (var(--…))",
       css.length > 2000 && !/#[0-9a-fA-F]{3,8}\b/.test(css.replace(/\/\*[\s\S]*?\*\//g, "")) && !/rgb\(/.test(css), css.length);
    ok("the design mock (a plain browser): nothing newer unless asked -- ?update=1.2.0 shows the banner, its \"Download\" answered as "
       + "the app would; the link shows; nothing is sent anywhere", mockOnDemand((o) => runTranslatorWindow(o)));
  }

  // ---- MUTATION SENTINELS --------------------------------------------------------------------------------------------------
  section("UPD -- mutation sentinels (index.html with one rule of the update banner taken out each)");
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
    const judgeHtml = (name, mutateHtml, probe) => {
      const m = mutateHtml(html);
      if (m === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      ok("MUTANT \"" + name + "\" is caught by its check", !probe(m));
    };
    const once = (a, b) => (s) => (s.split(a).length === 2 ? s.split(a).join(b) : s);   // (applies exactly once, else not at all)
    judge("UPD-1: update.check never sent at boot", once("  send({ type: \"update.check\" });\n})();", "})();"), silentBoot);
    const judgePage = (name, mutateHtml, probe) => {
      const m = mutateHtml(html);
      if (m === html) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let healthy = true;
      try { healthy = !!probe(runTranslatorWindow({ html: m })); } catch (e) { healthy = true; }
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    judgePage("UPD-1: the link shown before the app speaks", once('<button id="updLink" type="button" class="drop-link" hidden ', '<button id="updLink" type="button" class="drop-link" '), silentBoot);
    judge("UPD-1: the link never shown", once("    el.updLink.hidden = !UPD.known;", "    el.updLink.hidden = true;"), nothingNewer);
    judge("UPD-1: the banner without a newer version", once("    const show = UPD.available && !UPD.later;\n    if (show) {", "    const show = !UPD.later;\n    if (show) {"), nothingNewer);
    judge("UPD-1: the opt-out's enabled:false not read", once("    const newer = msg.enabled === true && v !== \"\" && (msg.available === true || msg.skipped === true);",
      "    const newer = v !== \"\" && (msg.available === true || msg.skipped === true);"), offSaysNothing);
    judge("UPD-1: the notes dropped from the sentence", once("is available\" + (UPD.notes ? \" — \" + UPD.notes : \"\");", "is available\";"), drawn);
    judgeHtml("UPD-1: shown in full screen", once("    body.is-full .upd-banner { display: none; }\n", ""), placed);
    judgeHtml("UPD-1: over the picture (inside the player, not above it)", (h) => {
      const m = /    <div id="updBanner"[\s\S]*?\n    <\/div>\n/.exec(h);
      if (!m) return h;
      return h.replace(m[0], "").replace("          <div id=\"fileView\" class=\"file-view\">\n", "          <div id=\"fileView\" class=\"file-view\">\n" + m[0]);
    }, placed);
    // Download: update.open{} and nothing else
    judge("UPD-2: Download sends the version (the message is not bare)", once("    send({ type: \"update.open\" });",
      "    send({ type: \"update.open\", version: UPD.version });"), download);
    judge("UPD-2: Download hands the app an address", once("    send({ type: \"update.open\" });",
      "    send({ type: \"update.open\", url: \"https://www.nextale-audio.com/suvtitle?update=\" + UPD.version });"), download);
    judge("UPD-2: Download sends nothing", once("btn(el.updGet, () => openUpdate());", "btn(el.updGet, () => {});"), download);
    judge("UPD-2: the page open never said", once("    if (o && o.ok) return", "    if (false) return"), opened);
    judge("UPD-2: where the new version is never said (the app's url dropped)", once("is on \" + (o.url || \"www.nextale-audio.com\") + \".\";",
      "is on www.nextale-audio.com.\";"), opened);
    judge("UPD-3: Later never told to the app (a reload brings it back)", once("btn(el.updLater, () => { UPD.later = true; paintUpdate(); send({ type: \"update.later\" }); });",
      "btn(el.updLater, () => { UPD.later = true; paintUpdate(); });"), later);
    judge("UPD-3: the app's later ignored (a reloaded page shows it again)", once("    UPD.later = UPD.available && msg.later === true;", "    UPD.later = false;"), laterReload);
    judge("UPD-3: Skip never told to the app", once("paintUpdate(); send({ type: \"update.skip\" }); });", "paintUpdate(); });"), skip);
    judge("UPD-4: ⋯ has no Updates entry", once("    if (UPD.known) items.push([updMenuWords(), () => openUpdDlg(el.tMore)]);\n", ""), dialog);
    judge("UPD-4: the dialog without its Download (no way to the page after Later)", once("    el.udGetRow.hidden = !(UPD.available || UPD.skipped);",
      "    el.udGetRow.hidden = true;"), dialog);
    judge("UPD-4: the dialog's Download sends nothing", once("btn(el.udGet, () => openUpdate());", "btn(el.udGet, () => {});"), dialog);
    judge("UPD-4: the opt-out never sent", once("    send({ type: \"update.enable\", on });\n", ""), optOut);
    judge("UPD-4: a key pressed in the dialog reaches the player", once("    ctxKeyEv = ev;                             // (the player's own listeners leave this one alone)\n", ""),
      dialogKeys, { width: 1280, height: 800 });
    judge("UPD-4: Esc does not close the dialog", once("    if (key === \"Escape\") { stop(); closeUpdDlg(); return; }", "    if (key === \"Escape\") { stop(); return; }"),
      dialogKeys, { width: 1280, height: 800 });
    // UPD-5: anything beyond their controls, or a message that carries something, must fail its probe
    judgePage("UPD-5: a field added to the dialog (any input)", once('    <label class="cc-check ud-check">',
      '    <input id="udExtra" type="text">\n    <label class="cc-check ud-check">'), controls);
    judgePage("UPD-5: a button added to the banner", once('        <button id="updSkip" ',
      '        <button id="updMore" type="button">More</button>\n        <button id="updSkip" '), controls);
    judgePage("UPD-5: an element added to the dialog", once('    <div id="udNow" class="mr-what" role="status"></div>',
      '    <div id="udNow" class="mr-what" role="status"></div>\n    <div id="udNote"></div>'), controls);
    judge("UPD-5: an update.* message carries more than its type", once("paintUpdate(); send({ type: \"update.skip\" }); });",
      "paintUpdate(); send({ type: \"update.skip\", version: UPD.version }); });"), controls);
    // (review 03/11) S5 -- the skip undone; N3 -- one update.open at a time
    judge("S5: no way back from a skip (\"Offer it again\" never shown)", once("    el.udUnskip.hidden = !UPD.skipped;", "    el.udUnskip.hidden = true;"), skipUndo);
    judge("S5: \"Offer it again\" sends nothing", once("btn(el.udUnskip, () => { if (UPD.skipped) send({ type: \"update.unskip\" }); });",
      "btn(el.udUnskip, () => { });"), skipUndo);
    judge("S5: the app's skipped never read (the dialog: \"No newer version is known\")", once("    UPD.skipped = newer && !UPD.available && msg.skipped === true;",
      "    UPD.skipped = false;"), skipUndo);
    judge("S5: a skipped version cannot be opened from the dialog", once("    if (UPD.opening || !(UPD.available || UPD.skipped)) return;",
      "    if (UPD.opening || !UPD.available) return;"), skippedOpens);
    judge("N3: a double-click opens twice (no wait for the answer)", once("    if (UPD.opening || !(UPD.available || UPD.skipped)) return;",
      "    if (!(UPD.available || UPD.skipped)) return;"), oneOpen);
    judge("N3: the buttons stay dead after the answer", once("    openDone();                                                // (the answer came: its buttons work again)\n", ""), oneOpen);
    judge("N3: no answer = dead for good (the 5 s safety gone)", once("    updOpenTimer = setTimeout(() => { updOpenTimer = null; UPD.opening = false; paintUpdate(); }, 5000);\n", ""), oneOpen);
    judge("N3: the banner's Download not held while the page opens", once("    el.updGet.disabled = UPD.opening;", "    el.updGet.disabled = false;"), oneOpen);
    {
      const mut = once("      if (v) mUpd.version = v[1];", "      mUpd.version = v ? v[1] : \"1.2.0\";");
      let healthy = true;
      try { healthy = mut(script) !== script && mockOnDemand((o) => runTranslatorWindow(Object.assign({ mutate: mut, name: "mock" }, o))); } catch (e) { healthy = true; }
      ok("MUTANT \"UPD-6: the mock shows the banner by itself\" is caught by its check", mut(script) !== script && !healthy);
    }
  }
}

module.exports = updChecks;

// ---- alone: node tests\web\test_upd.js -------------------------------------------------------------------------------------
if (require.main === module) {
  const P = require("./paths.js");
  const { runTranslatorWindow } = require("./run_ui.js");
  let pass = 0, fail = 0;
  const ok = (name, cond, extra) => {
    if (cond) { pass++; console.log("  PASS  " + name); }
    else { fail++; console.log("  FAIL  " + name + (extra !== undefined ? "  -> " + JSON.stringify(extra) : "")); }
  };
  const section = (s) => console.log("\n=== " + s + " ===");
  const html = P.readPage();
  const script = (html.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
  const bridgeSrc = P.appContractSources().text;
  // (test_ui.js's own two helpers, as it defines them)
  const fileMsg = (o) => Object.assign({ type: "translatorFile", jobId: 1, seq: 1, phase: "loading",
    itemId: 1, name: "film.mp4", url: "https://m1.suvtitle-media.example/film.mp4", mediaKey: "mk1", proxyUrl: "", proxyState: "",
    proxyErr: "", hasVideo: true, videoKnown: true, doneMs: 0, totalMs: 600000, ranges: [], complete: false,
    asrModel: "", mtModel: "", langA: "en", langB: "zh", mode: "auto",
    quality: "auto", cached: false, count: 0, srtPath: "", srtName: "", srtNote: "", err: "",
    cancelling: false, fresh: true, translate: true, autoplay: false, startMs: 0, resumed: false,
    sidecars: [], extSubId: 0 }, o);
  const metaOk = (m, wd) => m._fire("loadedmetadata", { readyState: 1, videoWidth: wd || 1280, videoHeight: 720, duration: 600 });
  updChecks({ ok, section, runTranslatorWindow, html, script, bridgeSrc, fileMsg, metaOk });
  console.log("\n" + (fail === 0 ? "ALL " + pass + " CHECKS PASSED" : pass + " passed, " + fail + " FAILED"));
  process.exit(fail === 0 ? 0 : 1);
}

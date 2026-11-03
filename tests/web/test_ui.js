// test_ui.js -- SuVtitle's page harness: behavioural checks of app/web/index.html (the player), run headless (run_ui.js),
// + the twins of its tables in SuVtitle's C++ and the app <-> page contract (paths.js), + a MUTATION SENTINEL per
// guard (the page with that guard removed must FAIL its probe -- a check that cannot fail is not a check).
// PORTED (S-WEB, 2026-10-23) from AudioPB's scratchpad/ui_test/test_ui.js of the 641f1aa era (frozen copy:
// SuVtitle/scratchpad/from_audiopb_641f1aa/ui_test/): its FILE-MODE sections -- WC (the file mode), WE (the player),
// their review rounds and sentinels -- with test_wg.js (WG) and test_wh.js (WH) required below. Dropped with the split
// (the desk, the Live window, AudioPB's bridge / main.cpp wiring): docs/PROGRESS_S-WEB.md lists them and says why.
// Run: node tests/web/test_ui.js   (exit 0 = ALL PASSED)
"use strict";
const { runTranslatorWindow } = require("./run_ui.js");
const fs = require("fs");
const path = require("path");
const P = require("./paths.js");

let pass = 0, fail = 0;
const ok  = (name, cond, extra) => {
  if (cond) { pass++; console.log("  PASS  " + name); }
  else { fail++; console.log("  FAIL  " + name + (extra !== undefined ? "  -> " + JSON.stringify(extra) : "")); }
};
const section = (s) => console.log("\n=== " + s + " ===");

{
  const WEB = P.WEB;
  const html = P.readPage();                               // LF, whatever the checkout (RV4-9)
  const script = (html.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
  // SuVtitle's C++ (app/ = S-HOST's port, engine/ = S-ENG's; the 641f1aa original until a file lands -- said at the end)
  const contract = P.appContractSources();
  const bridgeSrc = contract.text;                         // "the bridge" = every message the app handles or emits
  const engineSrc = P.cpp("translator/TrEngine.cpp");
  const playerSrc = P.cpp("ui/TrPlayer.h");
  const winStateSrc = P.cpp("ui/TrWindowState.h");
  const mainSrc = P.cppSource("ui/main.cpp");              // the app's window: its WebView2 switches (F1)
  const mainSrcWE = mainSrc.text;
  // THE SECURITY RULE's pattern, and the page's code without its comments (a comment may NAME a sink, code may not)
  const MARKUP_SINK = /\binnerHTML\b|\bouterHTML\b|\binsertAdjacentHTML\b|\bdocument\.write(ln)?\b|\beval\s*\(|\bnew\s+Function\s*\(/;
  // (RV4-10, review 23/10: a LEXER pass -- jslex.js -- not two regexes: a " // " or a "/*" inside a string, a template
  //  or a regex literal hid the code after it from the rule)
  const { stripComments } = require("./jslex.js");
  const codeOnly = (src) => stripComments(src);

  // ---- the port itself (SPLIT_PLAN S7): one page, the File mode only, branded, F1's lines byte for byte ---------------
  section("SuVtitle -- the port: ONE page, the File mode only, branded, the F1 lines byte for byte");
  {
    const crypto = require("crypto");
    const sha = (s) => crypto.createHash("sha256").update(s, "utf8").digest("hex").slice(0, 16);
    const f1css = /    \/\* WH \(11\/10\): an ISOLATED group on pure black[\s\S]*?    \.file-view\.render-plain #fileVideo \{ mix-blend-mode: normal; \}\n/.exec(html);
    const f1cv = /            <canvas id="fileCanvas" class="file-canvas" hidden><\/canvas>\n/.exec(html);
    const f1js = /  \/\* ---- VIDEO RENDERING \(wave WH, 11\/10\)[\s\S]*?\n  \}\);\n/.exec(html);
    // (the same three pieces of 641f1aa's ui/web/translator.html hash to these: scratchpad/s-web/port/build_index.js)
    // 25/10 -- ONE deliberate change to the VIDEO RENDERING block, measured live with the user (index.html: THE STANDARD
    // MODE IS DIRECT IN FULL SCREEN): renderNow() + applyRender() drawing it. The pin now proves "641f1aa's block + exactly
    // that change, nothing else": undo2510 takes the change back out, mechanically, and the result must hash as before.
    const undo2510 = (s) => s
      .replace(/  \/\* THE STANDARD MODE IS DIRECT IN FULL SCREEN[\s\S]*?\*\/\n  function renderNow\(\) \{[^\n]*\}\n/, "")
      .replace("  function applyRender() {\n    const m = renderNow();\n", "  function applyRender() {\n")
      .replace('toggle("render-canvas", m === "canvas")', 'toggle("render-canvas", renderMode === "canvas")')
      .replace('toggle("render-plain", m === "plain")', 'toggle("render-plain", renderMode === "plain")')
      .replace('el.fileCanvas.hidden = m !== "canvas";', 'el.fileCanvas.hidden = renderMode !== "canvas";')
      .replace('    if (m === "canvas") {', '    if (renderMode === "canvas") {');
    // 26/10 (1.1.0, THE NATIVE VIDEO ENGINE, N2) -- a SECOND deliberate change, two lines: mpv draws the picture, so the page
    // draws in no mode of its own ("native": no canvas, no blend class) and Ctrl+Alt+V says so instead of cycling. undoNative
    // takes them back out FIRST, mechanically; then undo2510; the result must hash as 641f1aa's block.
    const undoNative = (s) => s
      .replace('  function renderNow() { return role === "native" ? "native" : isFull ? "plain" : renderMode; }   // (1.1.0: mpv draws it)\n',
               '  function renderNow() { return isFull ? "plain" : renderMode; }\n')
      .replace("    if (role === \"native\") { showFileToast(NATIVE_RENDER_TEXT, false); return; }   // (1.1.0: the modes are the <video>'s)\n", "");
    ok("F1's HEVC lines came along BYTE FOR BYTE (the blend-mode CSS, #fileCanvas, the VIDEO RENDERING block = 641f1aa's + the 25/10 full-screen change + the 26/10 native change only)",
       !!f1css && !!f1cv && !!f1js && sha(f1css[0]) === "c7ea460daa621bdc" && sha(f1cv[0]) === "bc256d414d844e1c"
       && /function renderNow\(\)/.test(f1js[0]) && /role === "native" \? "native"/.test(f1js[0])
       && sha(undo2510(undoNative(f1js[0]))) === "88e719c89d277e42",
       [f1css && sha(f1css[0]), f1cv && sha(f1cv[0]), f1js && sha(undo2510(undoNative(f1js[0])))]);
    ok("nothing of the Live window is left: no live line / partial / end / session export, no log, no Live|File, no idle fade",
       !/translatorLine|translatorPartial|translatorEnd|"translatorExport"|translatorExported|translatorCtl",\s*clear|modeLive|modeFile|id="log"|tr-live|el\.bar\.classList\.add\("is-idle"\)|KEY_FONT|\bwake\(/.test(html));
    // (the markup and the CSS without their comments, the script through the lexer: an apostrophe in the markup is not
    //  a JS string -- RV4-10)
    const visible = (h) => h.replace(/<!--[\s\S]*?-->/g, " ")
      .replace(/<style>([\s\S]*?)<\/style>/g, (m0, css) => "<style>" + css.replace(/\/\*[\s\S]*?\*\//g, " ") + "</style>")
      .replace(/<script>([\s\S]*?)<\/script>/g, (m0, js) => "<script>" + codeOnly(js) + "</script>");
    ok("branded SuVtitle: no \"AudioPB\" a user can see (comments aside: the history stays in them)", !/AudioPB/.test(visible(html)),
       (visible(html).match(/.{0,40}AudioPB.{0,40}/) || [])[0]);
    ok("...its title, its lockup, the CC panel's rows, the default player, the sound copy",
       /<title>SuVtitle<\/title>/.test(html) && /label: "SuVtitle · both languages"/.test(script)
       && /Make SuVtitle the default player for videos…/.test(html) && /"Make SuVtitle the default player…"/.test(script)
       && /This file plays through SuVtitle's sound copy/.test(script));
    ok("the contract block says who answers now -- SuVtitle.exe -- with the names of 641f1aa and the WH-P addenda",
       /Contract with the bridge -- SuVtitle\.exe/.test(script) && /playerSubGet\{id:0\} = CANCEL/.test(script) && /PROXY = ONE sound track/.test(script));
  }
  const last = (w, type) => { const o = w.outbox.filter((m) => m && m.type === type); return o[o.length - 1]; };
  const countType = (w, type) => w.outbox.filter((m) => m && m.type === type).length;
  const rows = (w) => w.ids.fileList.children;
  const vTrack = (w) => w.ids.fileVideo.textTracks[0];
  const aTrack = (w) => w.ids.fileAudio.textTracks[0];
  const URL1 = "https://m1.suvtitle-media.example/film.mp4";
  // WE: the v2 state carries the player's fields too (the ranges, the item, the switch, autoplay...);
  // the defaults are a file NOT opened to play by itself, so v1's checks keep meaning what they meant.
  const fileMsg = (o) => Object.assign({ type: "translatorFile", jobId: 1, seq: 1, phase: "loading",
    itemId: 1, name: "film.mp4", url: URL1, mediaKey: "mk1", proxyUrl: "", proxyState: "", proxyErr: "",
    hasVideo: true, videoKnown: true, doneMs: 0, totalMs: 600000, ranges: [], complete: false,
    asrModel: "", mtModel: "", langA: "en", langB: "zh", mode: "auto",
    quality: "auto", cached: false, count: 0, srtPath: "", srtName: "", srtNote: "", err: "",
    cancelling: false, fresh: true, translate: true, autoplay: false, startMs: 0, resumed: false,
    sidecars: [], extSubId: 0 }, o);
  const cueMsg = (jobId, idx, s, e, lang, dstLang, src, dst, extra) =>
    Object.assign({ type: "translatorFileCue", jobId, idx, s, e, lang, dstLang, pass: false, src, dst }, extra || {});
  const metaOk = (m, w) => m._fire("loadedmetadata", { readyState: 1, videoWidth: w || 1280, videoHeight: 720, duration: 600 });
  // WE: what the subtitle overlay shows -- per line: its classes, its text, its language, and what its
  // text span holds (#TEXT. = a plain text node, SPAN.r-i = an italic run of a subtitle file...).
  const ovLines = (w) => w.ids.subOverlay.children.map((l) => {
    const t = l.children[0];
    return { cls: l.className, text: l.textContent, lang: t ? t.getAttribute("lang") : null,
             kids: t ? t.children.map((k) => k.tagName + "." + k.className) : [] };
  });
  // The display rule (Both / A / B) the way a viewer sets it in SuVtitle: the CC panel's SuVtitle rows (641f1aa: also the
  // Live window's Show plate, which stayed in AudioPB)
  const showVia = (w, v) => {
    const wasClosed = w.ids.ccPanel.hidden;
    if (wasClosed) w.ids.tCC.click();
    const b = w.ids.ccSources.children.find((x) => x.dataset.key === "a:" + v);
    if (b) b.click();
    if (wasClosed && !w.ids.ccPanel.hidden) w.ids.tCC.click();
    return !!b;
  };
  const qMsg = (items, o) => Object.assign({ type: "playerQueue", items, cur: 0, loaded: false, repeat: "off",
    shuffle: false, translate: true }, o);
  const qItem = (id, name, o) => Object.assign({ id, name, dir: "Season 1", state: "none", pct: 0, durMs: 0, resumeMs: 0 }, o);
  // the page WITHOUT its design mock (the mock plays the bridge, so it "sends" the bridge's messages)
  const pageOnly = script.slice(0, script.indexOf("Design-time mock"));
  // The normaliser PROBES. Each is a check on the real page below AND the condition its mutation
  // sentinel (the last section) must see FAIL on a mutant.
  //  - yue: a code that is three letters long and IS a language (the C++ rows keep it whole);
  //  - spa: a code that is NOT one of ours -- "" to the doors, so shown as it came, never tagged,
  //         never cut down to a two-letter "sp" that would read as a language.
  const winYue = (w) => {
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "yue", langB: "fr" });
    w.outbox.length = 0;
    w.ids.langB.value = "de";
    w.ids.langB.dispatch("change");
    const c = last(w, "translatorCtl");
    return !!c && c.langA === "yue" && c.langB === "de";
  };
  // (641f1aa read a NON-code off the Live log's badge; in SuVtitle it reaches the page as a cue's language: shown as it
  // came, never tagged with a language it is not -- a mutated normaliser would tag "spa" as "sp")
  const winNonCode = (w) => {
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "zh" });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    w.send(cueMsg(2, 0, 1000, 3000, "spa", "en", "hola", "hello"));
    const r = rows(w)[0];
    return !!r && r.querySelector(".file-sub").textContent === "hola" && r.querySelector(".file-sub").getAttribute("lang") === null
        && r.querySelector(".file-main").getAttribute("lang") === "en";
  };

  // ---- 1. the review of WA: ONE normaliser, the C++ doors' exact twin ---------------------------
  section("WC -- review of WA: the JS language normalisers mirror the C++ doors");
  {
    const cppRows = (src, fn) => {
      const blk = new RegExp(fn + "[\\s\\S]*?kAlias\\[\\] = \\{([\\s\\S]*?)\\};").exec(src);
      return blk ? [...blk[1].matchAll(/\{\s*L?"([^"]+)",\s*L?"([^"]+)"\s*\}/g)].map((m) => m[1] + ">" + m[2]).sort().join(",") : null;
    };
    const jsRows = (src, name) => {
      const blk = new RegExp("const " + name + " = \\{([\\s\\S]*?)\\};").exec(src);
      return blk ? [...blk[1].matchAll(/"([^"]+)":\s*"([^"]+)"/g)].map((m) => m[1] + ">" + m[2]).sort().join(",") : null;
    };
    const b = cppRows(bridgeSrc, "trLangCode"), e = cppRows(engineSrc, "normalizeLangCode");
    const t = jsRows(html, "LANG_ALIAS");
    ok("the three alias tables (the app's door, the engine's, the page's) hold the SAME rows (" + contract.from + ", engine/TrEngine.cpp)",
       !!b && b === e && b === t, { app: b, engine: e, page: t });
    ok("...and the page keeps no two-letter prefix fallback (`slice(0, 2)` on a code)", !/return s\.slice\(0, 2\)/.test(html));

    const w = runTranslatorWindow();
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "yue", langB: "fr" });
    w.outbox.length = 0;
    w.ids.langB.value = "de";
    w.ids.langB.dispatch("change");
    const ctl = last(w, "translatorCtl");
    ok("a running yue pair: picking the OTHER half sends yue back -- never \"yu\"",
       !!ctl && ctl.langA === "yue" && ctl.langB === "de", ctl);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", translate: true }));   // (U1: a file being translated lists SuVtitle's rows)
    w.ids.tCC.click();
    const rowA = w.ids.ccSources.children.find((x) => x.dataset.key === "a:a");
    ok("...and the page NAMES it by its code, not a blank (no table row for yue): \"SuVtitle · YUE\" in the CC panel",
       !!rowA && /^SuVtitle · YUE/.test(rowA.textContent), rowA && rowA.textContent);
    w.ids.tCC.click();
    w.outbox.length = 0;
    w.ids.langA.value = "";             // a select left on nothing (the yue case) picks nothing
    w.ids.langA.dispatch("change");
    ok("a select on NO code sends nothing at all", countType(w, "translatorCtl") === 0, w.outbox);
    // (641f1aa read these rows off the Live log's badges; SuVtitle's are a cue's lang attributes)
    const wb = runTranslatorWindow();
    wb.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "zh" });
    wb.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    wb.send(cueMsg(2, 0, 1000, 2000, "cmn", "en", "x", "y"));
    wb.send(cueMsg(2, 1, 3000, 4000, "eng", "zh", "x2", "y2"));
    wb.send(cueMsg(2, 2, 5000, 6000, "sv", "en", "x3", "y3"));
    const tags = rows(wb).map((r) => r.querySelector(".file-sub").getAttribute("lang"));
    ok("a cue's languages read through the same rows: cmn -> zh-CN, eng -> en, a plain 2-letter code (sv) -> sv",
       tags.join(",") === "zh-CN,en,sv", tags);
    ok("a NON-code (spa) is nothing to the page: shown as it came, no lang attribute, never \"sp\"",
       winNonCode(runTranslatorWindow()));
  }

  // ---- 2. the review of WA: every line's text carries its OWN language ----------------------
  section("WC -- review of WA: lang attributes (the right CJK face on screen)");
  {
    // (641f1aa checked the Live log's lines and its live line; SuVtitle's texts are a cue's -- the list and the picture)
    const w = runTranslatorWindow();
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "zh-tw" });
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], langA: "en", langB: "zh-tw" }));
    metaOk(w.ids.fileVideo);
    w.send(cueMsg(2, 0, 1000, 3000, "en", "zh-tw", "Hello.", "\u4f60\u597d\u3002"));
    w.send(cueMsg(2, 1, 4000, 6000, "zh", "en", "\u4f60\u597d", "Hi"));
    const r = rows(w);
    const tag = (i, cls) => r[i].querySelector(cls).getAttribute("lang");
    ok("a Traditional translation is tagged zh-TW, its English original en (the list)",
       tag(0, ".file-main") === "zh-TW" && tag(0, ".file-sub") === "en", [tag(0, ".file-main"), tag(0, ".file-sub")]);
    ok("a Simplified original is tagged zh-CN", tag(1, ".file-sub") === "zh-CN", tag(1, ".file-sub"));
    w.ids.fileVideo._seek(2);
    ok("...and on the picture the same (zh-TW translation, en original under it)",
       ovLines(w).length === 2 && ovLines(w)[0].lang === "zh-TW" && ovLines(w)[1].lang === "en", ovLines(w));
  }

  // ---- 2b. the review of WA: stale texts --------------------------------------------------------
  section("WC -- review of WA: stale texts (zh is 简体中文 now, the languages are thirteen)");
  {
    const docxH = P.cpp("ui/DocxWriter.h").replace(/\\u([0-9a-fA-F]{4})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));   // ASCII C++
    ok("DocxWriter.h: \"thirteen\", zh -> 简体中文, zh-tw -> 繁體中文, and no \"twelve\" or bare 中文 left",
       /thirteen/.test(docxH) && !/twelve/i.test(docxH) && /"zh" -> "简体中文"/.test(docxH)
       && /"zh-tw" -> "繁體中文"/.test(docxH) && !/(?<![体體])中文/.test(docxH));
    const pickDoc = (/((?:\s*\/\/[^\n]*\n)+)\s*function pickPair\(/.exec(html) || ["", ""])[1];
    ok("index.html: the comment over pickPair names 繁體中文, not a bare 中文",
       /繁體中文/.test(pickDoc) && !/(?<![体體])中文/.test(pickDoc), pickDoc);
    ok("index.html: the CSS comments name today's labels (no bare \"中文\" outside the why-not note)",
       (html.match(/(?<![体體])中文/g) || []).length === 1 && /no longer labelled "中文"/.test(html),
       (html.match(/.{0,30}(?<![体體])中文.{0,10}/g) || []));
  }

  // ---- 3. FILE MODE, the window ------------------------------------------------------------------
  // WE (2026-10-11) rewrote v1's checks for the player WITH THE SAME INTENT: the TextTrack, the native
  // controls and the strip's meter are gone -- the page draws the subtitles itself (D4), carries its
  // own transport (D6), and the seek bar shows what is subtitled (the ranges of v2).
  section("WC -- Translator window: FILE MODE (player, overlay, list, strip)");
  {
    const w = runTranslatorWindow();
    // (03/11/2026, THE UPDATE BANNER: + update.check AFTER `ready` -- the app asks the site once per start and answers each)
    ok("boot: two messages, `ready` -- view \"file\", SuVtitle's only view (the app replays the session on it) -- then update.check",
       w.outbox.length === 2 && w.outbox[0].type === "ready" && w.outbox[0].view === "file" && w.outbox[1].type === "update.check", w.outbox);
    ok("the page IS the File view: the drop zone, no player yet -- and nothing of the Live window (no log, no Live|File, no mode kept)",
       w.ids.filePane.hidden === false && w.ids.fileEmpty.hidden === false && w.ids.filePlayer.hidden === true
       && !w.ids.log && !w.ids.modeLive && !w.ids.modeFile && !w.ids.resume && w.lsStore["audiopb.tr.mode"] === undefined
       && countType(w, "playerView") === 0);
    ok("...Clear is hidden (no file to close) and Export is a disabled menu",
       w.ids.clearBtn.hidden === true && w.ids.exportBtn.disabled === true && w.ids.exportBtn.textContent === "Export ▾");
    ok("the drop zone says what the subtitles will be in (the running pair and direction)",
       w.ids.filePairHint.textContent.indexOf("English ⇄ 简体中文") === 0, w.ids.filePairHint.textContent);
    // U1 (the user, 02/11: "garder seulement le modèle turbo, mais garder le petit menu de sélection"): turbo alone, named as
    // the default; nothing promises "better" (WB measured large-v3 no more accurate). U2: U1's one-option <select> gave way to
    // the MODELS menu (the default two + "Import model…") -- its checks and sentinels are test_u2.js's; test_u1.js checks the
    // migration of a stored "max".
    const mtxt = w.ids.modelsBtn.textContent;
    ok("the models: SuVtitle's own two, large-v3-turbo and Qwen3-4B (U1: large-v3 left the menu; U2: the menu imports others) "
       + "-- nothing promises better (WB measured it)",
       mtxt === "large-v3-turbo·Qwen3-4B" && !w.ids.fileQuality && !/\bmax\b|better|best|higher quality/i.test(w.ids.modelsBtn.title), mtxt);
    ok("...and the quality the app is sent is the one there is", w.lsStore["audiopb.tr.fileQuality"] === undefined
       || w.lsStore["audiopb.tr.fileQuality"] === "auto");
    w.outbox.length = 0;
    w.ids.fileOpenBtn.click();
    w.ids.fileOpenFolderBtn.click();
    ok("Open files… / Open a folder… ask the bridge for its native dialogs, with the quality, nothing else",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "translatorFileOpen", quality: "auto" },
         { type: "playerOpen", what: "folder", quality: "auto" }]), w.outbox);

    w.send(fileMsg({ jobId: 3, phase: "loading" }));
    const v = w.ids.fileVideo;
    ok("a video plays in the <video>, from the bridge's mapped URL", v.src === URL1 && v.hidden === false
       && w.ids.fileAudio.hidden === true && w.ids.filePlayer.hidden === false && w.ids.fileEmpty.hidden === true);
    ok("...with NO subtitle track and NO native controls: the page draws the subtitles over the picture itself (D4)",
       v.textTracks.length === 0 && !("controls" in v.attrs) && w.ids.subOverlay.hidden === false
       && w.ids.subOverlay.parentNode === w.ids.fileView && v.parentNode === w.ids.fileView);
    ok("the strip: loading the models, and the length to analyse",
       w.ids.fileStatus.textContent === "Loading the models… · 10:00 to analyse", w.ids.fileStatus.textContent);
    ok("...Cancel offered, Re-analyse not", w.ids.fileCancel.hidden === false && w.ids.fileReanalyse.hidden === true);
    w.outbox.length = 0;
    w.ids.fileCancel.click();
    ok("Cancel names the job", JSON.stringify(last(w, "translatorFileCancel")) === JSON.stringify({ type: "translatorFileCancel", jobId: 3 }));
    metaOk(v);

    w.send(fileMsg({ jobId: 3, phase: "analyzing", doneMs: 60000, asrModel: "ggml-large-v3-turbo.bin",
                     mtModel: "Qwen3-4B-Instruct-2507-Q5_K_M.gguf" }));
    ok("analysing: the percentage, the frontier, the models -- no ETA from one sample",
       w.ids.fileStatus.textContent === "Analysing · 10% · 1:00 of 10:00 · large-v3-turbo · Qwen3-4B",
       w.ids.fileStatus.textContent);
    w.pumpTimers(20000);
    w.send(fileMsg({ jobId: 3, phase: "analyzing", doneMs: 120000, ranges: [[0, 120000]], asrModel: "ggml-large-v3-turbo.bin",
                     mtModel: "Qwen3-4B-Instruct-2507-Q5_K_M.gguf" }));
    ok("the ETA is MEASURED: 60 s of media in 20 s of wall clock, 480 s left -> about 3 min",
       w.ids.fileStatus.textContent.indexOf("about 3 min left") > 0, w.ids.fileStatus.textContent);
    const segs0 = w.ids.seekRanges.children;
    ok("the SEEK BAR shows what is subtitled: one green stretch, 0 to 20 % (the strip's meter is gone)",
       segs0.length === 1 && segs0[0].style.left === "0.00%" && segs0[0].style.width === "20.00%" && !w.ids.fileBarDone,
       segs0.map((s) => s.style.left + "+" + s.style.width));

    // the subtitles
    w.send(cueMsg(3, 0, 1000, 3500, "en", "zh", "Hello <b>world</b> & co", "你好 <i>世界</i> & 公司"));
    ok("a cue AWAY from the playhead (0:00; it starts at 0:01) puts nothing on the picture",
       w.ids.subOverlay.children.length === 0);
    v._seek(1.5);
    const L0 = ovLines(w);
    ok("under the playhead it is ON the picture at once: the translation, the original under it",
       L0.length === 2 && L0[0].cls === "sub-line sub-main" && L0[1].cls === "sub-line sub-orig", L0);
    ok("its texts are TEXT, never markup (an <i> in a subtitle is shown, not obeyed), each in its own language",
       L0.length === 2 && L0[0].text === "你好 <i>世界</i> & 公司" && L0[0].lang === "zh-CN"
       && L0[1].text === "Hello <b>world</b> & co" && L0[1].lang === "en"
       && L0[0].kids.join() === "#TEXT." && L0[1].kids.join() === "#TEXT.", L0);
    const r0 = rows(w)[0];
    ok("...and a ROW in the list: its time, its texts AS TEXT (never markup), its languages",
       rows(w).length === 1 && r0.querySelector(".file-t").textContent === "0:01"
       && r0.querySelector(".file-main").textContent === "你好 <i>世界</i> & 公司"
       && r0.querySelector(".file-main").getAttribute("lang") === "zh-CN"
       && r0.querySelector(".file-sub").getAttribute("lang") === "en");
    ok("Export becomes available", w.ids.exportBtn.disabled === false);

    w.send(cueMsg(2, 1, 4000, 5000, "en", "zh", "stale", "旧"));
    w.send(fileMsg({ jobId: 2, phase: "error", err: "an older job" }));
    ok("an OLDER generation's cue and state change nothing (STALE)", rows(w).length === 1
       && w.ids.fileStatus.textContent.indexOf("an older job") < 0);
    w.send(cueMsg(3, 0, 1000, 3500, "en", "zh", "dup", "dup"));
    ok("the same idx twice is applied once", rows(w).length === 1 && ovLines(w)[0].text === "你好 <i>世界</i> & 公司");
    w.outbox.length = 0;
    w.send(cueMsg(3, 3, 9000, 10000, "en", "zh", "three", "三"));
    w.send(cueMsg(3, 5, 15000, 16000, "en", "zh", "five", "五"));
    ok("a GAP in the idx asks for the whole set -- once per generation",
       countType(w, "translatorFileReplay") === 1, w.outbox);
    w.send({ type: "translatorFileCues", jobId: 3, cues: [
      { idx: 1, s: 4000, e: 5000, lang: "en", dstLang: "zh", pass: false, src: "one", dst: "一" },
      { idx: 2, s: 6000, e: 8000, lang: "en", dstLang: "zh", pass: false, src: "two", dst: "二" },
      { idx: 3, s: 9000, e: 10000, lang: "en", dstLang: "zh", pass: false, src: "three", dst: "三" },
      { idx: 4, s: 11000, e: 12000, lang: "zh", dstLang: "zh", pass: true, src: "好", dst: "好" } ] });
    ok("the bulk replay fills the holes, in START order, and adds no duplicate",
       rows(w).length === 6 && rows(w).map((r) => r.dataset.idx).join(",") === "0,1,2,3,4,5",
       rows(w).map((r) => r.dataset.idx));
    const passRow = rows(w).find((r) => r.dataset.idx === "4");
    v._seek(11.5);
    ok("a PASSTHROUGH shows ONCE (no original under itself), in the list and on the picture",
       !!passRow && passRow.querySelector(".file-sub").hidden === true && ovLines(w).length === 1
       && ovLines(w)[0].text === "好", ovLines(w));
    v._seek(1.5);

    // Show A / B / Both
    showVia(w, "a");
    ok("Show A (English): the picture shows the spoken English ALONE",
       ovLines(w).length === 1 && ovLines(w)[0].text === "Hello <b>world</b> & co" && ovLines(w)[0].lang === "en", ovLines(w));
    ok("...and the rows follow", rows(w)[0].querySelector(".file-main").textContent === "Hello <b>world</b> & co"
       && rows(w)[0].querySelector(".file-sub").hidden === true);
    showVia(w, "b");
    ok("Show B (Chinese): the translation alone",
       ovLines(w).length === 1 && ovLines(w)[0].text === "你好 <i>世界</i> & 公司", ovLines(w));
    showVia(w, "both");
    ok("Both again: two lines", ovLines(w).length === 2 && ovLines(w)[1].cls === "sub-line sub-orig" && rows(w).length === 6);

    // the playhead
    v._seek(200);
    ok("outside what is subtitled, a chip says so (not an empty screen)",
       w.ids.fileChip.hidden === false && w.ids.fileChip.textContent.indexOf("Not subtitled here yet") === 0,
       w.ids.fileChip.textContent);
    v._seek(1.5);
    ok("...gone when the playhead is back inside", w.ids.fileChip.hidden === true);
    ok("the subtitle under the playhead is the ACTIVE row", rows(w)[0].classList.contains("is-active")
       && !rows(w)[1].classList.contains("is-active"));
    rows(w)[2].click();
    ok("clicking a row SEEKS to it", Math.abs(v.currentTime - 6.001) < 1e-6 && rows(w)[2].classList.contains("is-active"),
       v.currentTime);

    // done
    w.send(fileMsg({ jobId: 3, phase: "done", doneMs: 600000, complete: true, ranges: [[0, 600000]],
                     srtPath: "D:\\Films\\film.zh.srt", srtName: "film.zh.srt" }));
    // (WG: the strip names a subtitle file by what follows the film's name -- ".zh.srt" -- the tooltip whole)
    ok("done: the count and the saved .srt", w.ids.fileStatus.textContent === "6 subtitles · saved .zh.srt"
       && /\nSubtitle files: film\.zh\.srt$/.test(w.ids.fileStatus.title), [w.ids.fileStatus.textContent, w.ids.fileStatus.title]);
    ok("...Show in folder and Re-analyse offered, Cancel gone",
       w.ids.fileReveal.hidden === false && w.ids.fileReanalyse.hidden === false
       && w.ids.fileReanalyse.textContent === "Re-analyse" && w.ids.fileCancel.hidden === true);
    w.outbox.length = 0;
    w.ids.fileReveal.click();
    w.ids.fileReanalyse.click();
    ok("Show in folder -> Reveal{srt}; Re-analyse -> Analyse{force, quality}",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "translatorFileReveal", what: "srt" },
         { type: "translatorFileAnalyse", force: true, quality: "auto" }]), w.outbox);

    // the pair moved since the analysis
    w.send({ type: "translatorState", running: true, status: "listening", mode: "auto", langA: "en", langB: "fr" });
    ok("the pair moved: \"Languages changed\" with its own Re-analyse", w.ids.fileStale.hidden === false);
    w.outbox.length = 0;
    w.ids.fileStaleBtn.click();
    ok("...which may come from the cache (force:false)",
       JSON.stringify(last(w, "translatorFileAnalyse")) === JSON.stringify({ type: "translatorFileAnalyse", force: false, quality: "auto" }));
    w.send({ type: "translatorState", running: true, status: "analyzing", mode: "auto", langA: "en", langB: "zh" });
    ok("...and the banner goes when the pair is back", w.ids.fileStale.hidden === true);

    // export
    w.ids.exportBtn.click();
    const items = w.ids.exportMenu.children;
    // (U3 re-point: SIX -- the bilingual Word file has its own name, "Transcript (.docx)" is the speaker transcript now)
    ok("Export opens a menu of six", w.ids.exportMenu.hidden === false && items.length === 6
       && items[4].textContent === "Bilingual subtitles (.docx)" && items[5].textContent === "Transcript (.docx)",
       items.map((b2) => b2.textContent));
    ok("...naming the languages", items[0].textContent === "Subtitles · 简体中文 (.srt)"
       && items[1].textContent === "Subtitles · English, original (.srt)", items.map((b2) => b2.textContent));
    w.outbox.length = 0;
    items[3].click();
    ok("an item sends its format and its text, and the menu closes",
       JSON.stringify(last(w, "translatorFileExport")) === JSON.stringify({ type: "translatorFileExport", format: "vtt", which: "dst" })
       && w.ids.exportMenu.hidden === true);
    w.send({ type: "translatorFileExported", ok: true, format: "vtt", which: "dst", path: "D:\\Films\\film.zh.vtt", count: 6 });
    ok("the ack: the file name", w.ids.fileToast.hidden === false && w.ids.fileToast.textContent === "Saved · film.zh.vtt");

    // the ENGINE's state line, in the bar (641f1aa: the Live window's status line, hidden in File mode)
    ok("the engine's state line, in the bar, says what the engine does: analysing (a green pulse)",
       w.ids.state.textContent === "engine: analysing" && w.ids.state.classList.contains("is-analyzing") && w.ids.state.hidden === false
       && w.ids.state.parentNode === w.ids.bar, w.ids.state.textContent);
    // (RV4-2: the app's own shape -- app\bridge.cpp sendTranslatorState: models is an OBJECT {asr, mt}; a string fixture
    //  made this check pass on a message the app never sends, while the real one read "Models: [object Object]")
    w.send({ type: "translatorState", active: true, running: true, status: "listening", preset: "quality", mode: "auto", langA: "en",
             langB: "zh", gpu: true, slot: "suvtitle.engine.1.1", models: { asr: "ggml-large-v3-turbo.bin", mt: "Qwen3-4B-Instruct-2507-Q5_K_M.gguf" } });
    ok("...ready (the engine's idle \"listening\": SuVtitle's has no live path), its models in the tooltip (the app's object, shortened)",
       w.ids.state.textContent === "engine ready" && w.ids.state.classList.contains("is-listening")
       && /\nModels: large-v3-turbo · Qwen3-4B$/.test(w.ids.state.title), [w.ids.state.textContent, w.ids.state.title]);
    w.send({ type: "translatorState", running: false, status: "error", err: "whisper-server.exe was not found", mode: "auto", langA: "en", langB: "zh" });
    ok("...an error in its own words and colour (the strip has no file to say it for)",
       w.ids.state.textContent === "engine error — whisper-server.exe was not found" && w.ids.state.classList.contains("is-error"),
       w.ids.state.textContent);
    w.send({ type: "translatorState", running: true, status: "analyzing", mode: "auto", langA: "en", langB: "zh" });

    // a re-analysis of the SAME file: a new cue set, the same media
    const loads = v.loads;
    w.send(fileMsg({ jobId: 4, phase: "loading" }));
    ok("a re-analysis clears the cue set (picture and list) but does NOT reload the media",
       rows(w).length === 0 && w.ids.subOverlay.children.length === 0 && v.loads === loads && v.src === URL1);

    // A+ on the subtitles
    w.send(cueMsg(4, 0, 0, 2000, "en", "zh", "a", "一"));
    const k0 = w.ids.fileView.style["--sub-k"];
    w.win.dispatch("keydown", { key: "=", ctrlKey: true, target: w.doc.body });
    ok("Ctrl+= (as Ctrl+wheel; 641f1aa: also the Live plate's A+) moves the subtitle SIZE slider (+0.2 % of the picture; user 11/10 asked for a slider), from the 4.0 % default",
       k0 === "4.000" && w.ids.fileView.style["--sub-k"] === "4.200" && w.ids.ccSizeVal.textContent === "4.2 %",
       [k0, w.ids.fileView.style["--sub-k"], w.ids.ccSizeVal.textContent]);
    w.win.dispatch("keydown", { key: "0", ctrlKey: true, target: w.doc.body });
    ok("...Ctrl+0 puts it back to 4.0 % (641f1aa reset the Live dialogue's size there, which SuVtitle has not)",
       w.ids.fileView.style["--sub-k"] === "4.000" && w.ids.ccSizeVal.textContent === "4.0 %", w.ids.fileView.style["--sub-k"]);
    ok("...as a share of the PICTURE's height (cqh on .file-view): it grows with the window, and in full screen",
       /\.file-view \{[^}]*container-type: size;/.test(html)
       && /\.sub-overlay \{[^}]*font-size: clamp\(9px, calc\(var\(--sub-k, 4\) \* 1cqh\), 160px\)/.test(html));

    // the window closed
    v.play();
    w.doc.fullscreenElement = v;
    w.send({ type: "translatorWindow", shown: false });
    ok("the window closed: playback PAUSES and any element full screen is left", v.paused === true && w.doc._exitedFullscreen === 1);

    // a format the WebView cannot play
    w.outbox.length = 0;
    v._fire("error", { error: { code: 4 } });
    v._fire("error", { error: { code: 4 } });
    ok("an unplayable file asks the bridge for its SOUND, exactly once",
       countType(w, "translatorFilePlayFailed") === 1
       && JSON.stringify(last(w, "translatorFilePlayFailed")) === JSON.stringify({ type: "translatorFilePlayFailed", seq: 1, code: 4 }));
    ok("...the failed element is let go, and the window says what is happening",
       v.src === "" && w.ids.fileNotice.hidden === false
       && w.ids.fileNotice.textContent === "This format can't be played here. Preparing its sound…", w.ids.fileNotice.textContent);
    w.send(fileMsg({ jobId: 4, phase: "analyzing", doneMs: 30000, ranges: [[0, 30000]],
                     proxyUrl: "https://suvtitle-proxy.example/1-5.wav", proxyState: "ready" }));
    const a = w.ids.fileAudio;
    ok("the proxy arrives: the <audio> plays it, the sound's card stands in for the picture",
       a.src === "https://suvtitle-proxy.example/1-5.wav" && a.hidden === false && w.ids.fileBig.hidden === false
       && v.hidden === true);
    ok("...the subtitles stay on the SAME overlay, in the card layout (is-audio): one renderer for both",
       w.ids.fileView.classList.contains("is-audio") && a.textTracks.length === 0);
    ok("...and the notice says what you hear", w.ids.fileNotice.textContent.indexOf("you hear its sound instead") > 0);
    a._seek(1.0);
    ok("the card shows the subtitle under the playhead, in its language",
       ovLines(w).length === 2 && ovLines(w)[0].text === "一" && ovLines(w)[0].lang === "zh-CN"
       && ovLines(w)[1].text === "a", ovLines(w));

    // close
    w.outbox.length = 0;
    ok("\"Close file\" in the bar, while a file is open", w.ids.clearBtn.textContent === "Close file" && w.ids.clearBtn.hidden === false);
    w.ids.clearBtn.click();
    ok("...it sends translatorFileClose", JSON.stringify(w.outbox) === JSON.stringify([{ type: "translatorFileClose" }]), w.outbox);
    w.send(fileMsg({ jobId: 5, phase: "idle", name: "", url: "", proxyUrl: "" }));
    ok("idle (and no queue): back to the drop zone, the media released, the list and the picture emptied",
       w.ids.fileEmpty.hidden === false && w.ids.filePlayer.hidden === true && a.src === "" && v.src === ""
       && rows(w).length === 0 && w.ids.subOverlay.children.length === 0 && w.ids.clearBtn.hidden === true);

    // a SOUND file by its extension, and a video with no picture
    w.send(fileMsg({ jobId: 6, seq: 2, phase: "loading", name: "talk.mp3", url: "https://m2.suvtitle-media.example/talk.mp3",
                     hasVideo: false, videoKnown: false }));
    ok("an .mp3 goes straight to the <audio> (nothing to try a <video> on)",
       a.src === "https://m2.suvtitle-media.example/talk.mp3" && v.src === "" && w.ids.fileBig.hidden === false);
    w.send(fileMsg({ jobId: 7, seq: 3, phase: "loading", name: "clip.mov", url: "https://m3.suvtitle-media.example/clip.mov",
                     hasVideo: true, videoKnown: true }));
    ok("a .mov tries the <video>", v.src === "https://m3.suvtitle-media.example/clip.mov" && a.src === "");
    v._fire("loadedmetadata", { readyState: 1, videoWidth: 0, videoHeight: 0, duration: 10 });
    ok("loaded with NO picture (ProRes): the same URL moves to the <audio>, and the window says why",
       a.src === "https://m3.suvtitle-media.example/clip.mov" && v.src === "" && w.ids.fileNotice.textContent
       === "The picture can't be displayed in this format: sound and subtitles only.", w.ids.fileNotice.textContent);
  }

  // ---- 3b. the strip's other endings (what the user reads when it did NOT just work) -------------
  section("WC -- Translator window: the strip's other endings");
  {
    const w = runTranslatorWindow();
    const say = (o) => { w.send(fileMsg(o)); return w.ids.fileStatus.textContent; };
    w.send(fileMsg({ jobId: 2, phase: "loading" }));
    metaOk(w.ids.fileVideo);
    w.send(cueMsg(2, 0, 0, 1000, "en", "zh", "a", "\u4e00"));
    const err = "large-v3 is not installed: ggml-large-v3.bin was not found in the models folder.";
    ok("an error (Max without its model): the helper's own sentence, the cues KEPT, Translate offered again (U1: it was \"Analyse again\")",
       say({ jobId: 2, phase: "error", quality: "max", err }) === "Couldn't finish: " + err + " · 1 subtitle kept"
       && w.ids.fileDot.classList.contains("is-error") && w.ids.fileTranslate.hidden === false
       && w.ids.fileReanalyse.hidden === true && w.ids.fileCancel.hidden === true,
       w.ids.fileStatus.textContent);
    ok("the app's own refusal reads the same way (641f1aa: no Translator card on the desk; here: the engine down)",
       say({ jobId: 3, phase: "error", err: "The SuVtitle engine is not running." })
       === "Couldn't finish: The SuVtitle engine is not running.");
    w.send(cueMsg(4, 0, 0, 1000, "en", "zh", "a", "\u4e00"));
    w.send(fileMsg({ jobId: 4, phase: "analyzing", doneMs: 150000 }));
    w.send(cueMsg(4, 0, 0, 1000, "en", "zh", "a", "\u4e00"));
    w.send(cueMsg(4, 1, 2000, 3000, "en", "zh", "b", "\u4e8c"));
    ok("cancelled: where it stopped, and what is kept", say({ jobId: 4, phase: "cancelled", doneMs: 150000 })
       === "Cancelled at 25% · 2 subtitles kept", w.ids.fileStatus.textContent);
    ok("a cancelling job says so, and offers no second Cancel",
       say({ jobId: 5, phase: "analyzing", doneMs: 1000, cancelling: true }) === "Cancelling…"
       && w.ids.fileCancel.hidden === true);
    w.send(fileMsg({ jobId: 6, phase: "done", doneMs: 600000, cached: true, srtPath: "C:\\Users\\me\\Documents\\SuVtitle Subtitles\\film.zh.srt",
                     srtName: "film.zh.srt", srtNote: "The file's folder could not be written to: saved in Documents\\SuVtitle Subtitles." }));
    w.send({ type: "translatorFileCues", jobId: 6, cues: [{ idx: 0, s: 0, e: 900, lang: "en", dstLang: "zh", pass: false, src: "a", dst: "\u4e00" }] });
    ok("from the cache, saved elsewhere: the strip says both, in words",
       w.ids.fileStatus.textContent === "1 subtitle · analysed earlier · saved .zh.srt · "
       + "The file's folder could not be written to: saved in Documents\\SuVtitle Subtitles.", w.ids.fileStatus.textContent);
    ok("no speech at all: said plainly, nothing to show in a folder",
       say({ jobId: 7, phase: "done", doneMs: 600000, srtPath: "", srtName: "", srtNote: "No speech was found in this file." })
       === "No speech was found in this file" && w.ids.fileReveal.hidden === true && w.ids.exportBtn.disabled === true);
  }

  // ---- 3c. the list follows the analysis, and holds still under the reader's hand ---------------
  // The stub fires no scroll event by itself -- like a browser whose scrollTop write did not move
  // anything. So the test fires the events a browser WOULD fire, and none where it would not.
  const listHold = (w) => {
    const L = w.ids.fileList;
    w.send(fileMsg({ jobId: 8, phase: "analyzing", doneMs: 1000 }));
    metaOk(w.ids.fileVideo);                                   // paused at 0: follow the ANALYSIS
    L.scrollHeight = 2000;
    w.send(cueMsg(8, 0, 0, 1000, "en", "zh", "a", "一"));
    const followed = L.scrollTop === 2000;
    L.dispatch("scroll");                                      // the browser's echo of OUR move
    w.send(cueMsg(8, 1, 1000, 2000, "en", "zh", "b", "二")); // same height: a write that moves nothing
    L.scrollTop = 800; L.dispatch("scroll");                   // ...then the READER scrolls up
    L.scrollHeight = 2100;
    w.send(cueMsg(8, 2, 2000, 3000, "en", "zh", "c", "三"));
    const held = L.scrollTop === 800;
    w.pumpTimers(5100);
    L.scrollHeight = 2200;
    w.send(cueMsg(8, 3, 3000, 4000, "en", "zh", "d", "四"));
    return followed && held && L.scrollTop === 2200;
  };
  section("WC -- Translator window: the list follows, and holds still under the reader's hand");
  ok("follows the newest subtitle; a hand scroll -- even right after a write that moved nothing -- holds it 5 s",
     listHold(runTranslatorWindow()));

  // ---- 4. drag and drop -----------------------------------------------------------------------
  section("WC -- Translator window: drag and drop");
  {
    const w = runTranslatorWindow();
    const over = w.dispatchDoc("dragover", { dataTransfer: { dropEffect: "" } });
    ok("dragover is taken (a drop can never navigate the window away)", over.defaultPrevented === true);
    const file = { name: "film.mkv", type: "" };
    const drop = w.dispatchDoc("drop", { dataTransfer: { files: [file] } });
    ok("a dropped file: default prevented (the File view is the page)",
       drop.defaultPrevented === true && w.ids.filePane.hidden === false);
    ok("...and the File OBJECT travels as an additional object -- the page sends no path (not on the queue: PLAY)",
       JSON.stringify(last(w, "translatorFileDrop")) === JSON.stringify({ type: "translatorFileDrop", quality: "auto", target: "play" })
       && w.objs.length === 1 && w.objs[0][0] === file);
    const n0 = countType(w, "translatorFileDrop");
    const txt = w.dispatchDoc("drop", { dataTransfer: { files: [{ name: "notes.txt", type: "text/plain" }] } });
    ok("a text file is refused here, with a word, and still never navigates",
       txt.defaultPrevented === true && countType(w, "translatorFileDrop") === n0
       && w.ids.fileToast.textContent === "That is not a video, audio or subtitle file.");
    const old = runTranslatorWindow({ noAdditionalObjects: true });
    old.dispatchDoc("drop", { dataTransfer: { files: [file] } });
    ok("a runtime without postMessageWithAdditionalObjects says to use Open files…",
       countType(old, "translatorFileDrop") === 0 && old.ids.fileToast.textContent.indexOf("Open files") > 0);
  }

  // ---- 4a. the window à l'image d'AudioPB (the user, 2026-10-10) ---------------------------------
  section("WC -- the page à l'image d'AudioPB (NEXTALE's language): tokens, theme, header, full screen");
  {
    const css = fs.readFileSync(path.join(__dirname, "ref", "tokens_641f1aa.css"), "utf8");   // AudioPB's style.css @641f1aa
    const tokensOf = (src, selector) => {
      const at = src.indexOf(selector + " {");
      if (at < 0) return null;
      const body = src.slice(src.indexOf("{", at) + 1, src.indexOf("}", at));
      const out = {};
      for (const m of body.matchAll(/(--[\w-]+)\s*:\s*([^;]+);/g)) out[m[1]] = m[2].replace(/\s+/g, " ").trim();
      return out;
    };
    const blocks = [":root", ':root[data-theme="dark"]', ':root[data-theme="light"]'];
    blocks.forEach((b) => {
      const master = tokensOf(css, b), copy = tokensOf(html, b);
      const names = Object.keys(master || {}).filter((k) => !/^--spk/.test(k));
      // the THEME blocks are copied WHOLE (the window follows the app's theme exactly); the shared
      // block is copied whole too -- every token the page reads comes from the master
      const missing = names.filter((k) => !(k in (copy || {})));
      const drift = names.filter((k) => k in (copy || {}) && copy[k] !== master[k]);
      ok("tokens " + b + ": every one of AudioPB's style.css (641f1aa) is in the page, with the SAME value (no drift)",
         !!master && !!copy && names.length >= 9 && missing.length === 0 && drift.length === 0,
         { missing, drift: drift.map((k) => k + ": " + copy[k] + " vs " + master[k]) });
    });
    ok("...and the page says where its tokens come from", /DESIGN TOKENS -- NEXTALE's, as AudioPB's ui\/web\/style\.css carried them at 641f1aa/.test(html));
    // the theme: the app's key, the app's attribute, followed live
    const wl = runTranslatorWindow({ ls: { "patchbus.theme": "light" } });
    ok("the theme kept in the profile (patchbus.theme = light, AudioPB's key) is the page's from its first paint",
       wl.doc.documentElement.getAttribute("data-theme") === "light" && /const THEME_KEY = "patchbus\.theme";/.test(script));
    wl.win.dispatch("storage", { key: "patchbus.theme", newValue: "dark" });
    ok("...and another page of the profile reaches it LIVE (the storage event)",
       wl.doc.documentElement.getAttribute("data-theme") === "dark");
    wl.win.dispatch("storage", { key: "patchbus.theme", newValue: "purple" });
    wl.win.dispatch("storage", { key: "audiopb.tr.font", newValue: "light" });
    ok("...while an unknown value, or another key, changes nothing", wl.doc.documentElement.getAttribute("data-theme") === "dark");
    ok("nothing chosen yet: dark (AudioPB's default)", runTranslatorWindow().doc.documentElement.getAttribute("data-theme") === "dark");
    const wt = runTranslatorWindow();
    wt.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    wt.ids.tMore.click();
    const tb = wt.ids.morePanel.children.find((x) => x.textContent === "Light theme");
    if (tb) tb.click();
    ok("SuVtitle has no main window: the ⋯ menu's \"Light theme\" switches it, kept in the profile (the same key)",
       !!tb && wt.doc.documentElement.getAttribute("data-theme") === "light" && wt.lsStore["patchbus.theme"] === "light");
    wt.ids.tMore.click();
    const tb2 = wt.ids.morePanel.children.find((x) => x.textContent === "Dark theme");
    if (tb2) tb2.click();
    ok("...and then offers \"Dark theme\": back to dark", !!tb2 && wt.doc.documentElement.getAttribute("data-theme") === "dark"
       && wt.lsStore["patchbus.theme"] === "dark");
    // the header: the lockup, the display face -- Bahnschrift, a font that ships with Windows (publication wave, 2026-10-24: AudioPB's
    // purchased face left the repository, no font file ships, no @font-face rule), labelled group plates
    const filesUnder = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? filesUnder(path.join(d, e.name)).map((f) => e.name + "/" + f) : [e.name]));
    const noFontFile = (h) => !/@font-face\s*\{/.test(h) && !/url\(\s*["']?[^)"']*\.(woff2?|ttf|otf|eot)\b/i.test(h);
    const shipped = filesUnder(WEB);
    ok("the header carries SuVtitle's lockup (Bahnschrift 600, its V in heliconia): the system face, no font file shipped, no @font-face rule, no url() to a font",
       /<span class="brand-name"><span class="brand-txt">Su<\/span><span class="brand-accent">V<\/span><span class="brand-txt">title<\/span><\/span>/.test(html)
       && runTranslatorWindow().doc.querySelector(".brand-name").textContent === "SuVtitle"
       && /--display: "Bahnschrift", "Segoe UI Variable Display", "Segoe UI", system-ui, sans-serif;/.test(html)
       && /\.brand-name \{ font-family: var\(--display\); font-size: 24px; font-weight: 600; font-stretch: normal;/.test(html)
       && noFontFile(html) && shipped.indexOf("index.html") >= 0 && shipped.every((f) => !/\.(woff2?|ttf|otf|eot)$/i.test(f)), shipped);
    const labels = [...html.matchAll(/<span class="lbl">([^<]+)<\/span>/g)].map((m) => m[1]);
    ok("the bar's controls sit in the app's group plates: Window -- the pair left for the strip (U1); the Live window's Source, Show and Text stayed in AudioPB",
       labels.join("|") === "Window", labels);
    ok("...which FOLD into plain rows on a narrow or short window (measured: at the 420 x 320 minimum the plates hid the player)",
       /@media \(max-width: 900px\), \(max-height: 560px\) \{[\s\S]{0,400}#bar \.grp \{ flex-direction: row;[^}]*border: none;[\s\S]{0,120}#bar \.lbl \{ display: none; \}/.test(html)
       && /@media \(max-width: 560px\) \{ #bar \.brand \{ display: none; \} \}/.test(html));
    ok("the controls are not selectable text, the subtitle list still is",
       /#bar, #state, \.drop, \.file-strip, \.menu, \.file-chip, \.file-notice, \.file-toast \{ user-select: none; \}/.test(html)
       && !/\.file-list[^{]*\{[^}]*user-select: none/.test(html));
    ok("the window is the app's own (native, movable): the page draws no drag region", !/app-region/.test(html));
    // full screen (WE, D3): the WINDOW, made borderless on its monitor by main.cpp -- the page ASKS
    // (translatorFullscreen), main.cpp decides and ANSWERS (translatorWindow{fullscreen}), and the page
    // lays itself out for the answer. A button in words next to the player, F, Esc, and a double-click
    // on the picture -- ours now: the native controls whose double-click it was are gone.
    const w = runTranslatorWindow();
    const v = w.ids.fileVideo, fb = w.ids.fileFullBtn;
    const lastFull = (x) => last(x, "translatorFullscreen");
    ok("full screen: no button before there is a file", !!fb && fb.hidden === true);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 1000 }));
    metaOk(v);
    ok("...a file: the strip shows \"Full screen\"", fb.hidden === false && fb.classList.contains("full-btn"));
    w.outbox.length = 0;
    fb.click();
    ok("...which asks main.cpp for the WINDOW in full screen -- no element full screen at all",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "translatorFullscreen", on: true }])
       && !v._fullscreen && !/requestFullscreen/.test(script), w.outbox);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    ok("...main.cpp's answer lays the page out for it: bar, strip and panel gone (body.is-full)",
       w.doc.body.classList.contains("is-full")
       && /body\.is-full #bar \{ display: none; \}/.test(html)
       && /body\.is-full \.file-strip, body\.is-full \.file-panel \{ display: none; \}/.test(html));
    w.outbox.length = 0;
    fb.click();
    ok("...and, in full screen, the same button leaves it",
       JSON.stringify(lastFull(w)) === JSON.stringify({ type: "translatorFullscreen", on: false }), w.outbox);
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    ok("...the page follows main.cpp's word (not its own guess)", !w.doc.body.classList.contains("is-full"));
    w.outbox.length = 0;
    w.win.dispatch("keydown", { key: "f", target: v });
    ok("F toggles it -- even with the focus on the <video> itself", !!lastFull(w) && lastFull(w).on === true);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.outbox.length = 0;
    w.win.dispatch("keydown", { key: "Escape", target: v });
    ok("Esc leaves it", !!lastFull(w) && lastFull(w).on === false);
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    w.outbox.length = 0;
    w.ids.fileView.dispatch("dblclick", { target: v });
    ok("a double-click on the picture toggles it (ours: there are no native controls to do it any more)",
       !!lastFull(w) && lastFull(w).on === true);
    w.outbox.length = 0;
    w.ids.fileView.dispatch("dblclick", { target: w.ids.fileReadyBtn });
    ok("...but not a double-click on a button sitting over the picture", countType(w, "translatorFullscreen") === 0);
    const wa = runTranslatorWindow();
    wa.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 1000, name: "talk.mp3", hasVideo: false, videoKnown: false }));
    wa.outbox.length = 0;
    wa.win.dispatch("keydown", { key: "f", target: wa.ids.fileAudio });
    ok("a sound-only file goes full screen too: its subtitle card fills the monitor (the window, not a picture)",
       wa.ids.fileFullBtn.hidden === false && !!lastFull(wa) && lastFull(wa).on === true);
  }

  // ---- 4b. the review of WC (2026-10-10): the window's half of the decided fixes ---------------
  // Each probe returns TRUE on correct behaviour; the mutation sentinels below re-run it on a page
  // with the fix removed and require it to fail.
  // [K] a replay of a session the page already showed is not NEWS: no opening (641f1aa: it left the user's Live tab alone;
  // SuVtitle has no tab to switch -- what news brings is the opening: the start, play when ready, the resume toast)
  const replayNotNews = (w) => {
    w.send(fileMsg({ jobId: 7, phase: "done", doneMs: 600000, complete: true, ranges: [[0, 600000]], fresh: false,
                     autoplay: true, resumed: true, startMs: 61000 }));
    metaOk(w.ids.fileVideo);
    return w.ids.resumeToast.hidden === true && w.ids.fileVideo.playCalls === 0 && w.ids.fileVideo.currentTime === 61;
  };
  const noUrlAsksSound = (w) => {            // [3] a file the bridge could not serve
    w.send(fileMsg({ jobId: 2, seq: 5, phase: "loading", url: "" }));
    w.send(fileMsg({ jobId: 2, seq: 5, phase: "analyzing", url: "", doneMs: 1000 }));
    const m = last(w, "translatorFilePlayFailed");
    return countType(w, "translatorFilePlayFailed") === 1 && !!m && m.seq === 5;
  };
  const txtDropRefused = (w) => {            // [4] judged by its EXTENSION: a .txt has no MIME type either
    w.dispatchDoc("drop", { dataTransfer: { files: [{ name: "notes.txt", type: "" }] } });
    return countType(w, "translatorFileDrop") === 0 && w.ids.fileToast.textContent === "That is not a video, audio or subtitle file.";
  };
  const closeStopsSwitch = (w) => {          // [8] a switch in flight when the window is closed
    w.send(fileMsg({ jobId: 2, phase: "analyzing", name: "clip.mov", url: "https://k0123456789abcdef.suvtitle-media.example/clip.mov" }));
    const v = w.ids.fileVideo, a = w.ids.fileAudio;
    v.play();                                                                    // the user plays it
    v._fire("loadedmetadata", { readyState: 1, videoWidth: 0, videoHeight: 0, duration: 10 });   // no picture -> <audio>
    const before = a.playCalls;
    w.send({ type: "translatorWindow", shown: false });
    w.send({ type: "translatorWindow", shown: true });   // reopened before the switch lands (C6's gate does not cover it)
    a._fire("loadedmetadata", { readyState: 1, duration: 10 });
    return a.src === "https://k0123456789abcdef.suvtitle-media.example/clip.mov" && a.playCalls === before && a.paused === true;
  };
  section("WC -- review of WC: the window's fixes (replay, no URL, drops, closing)");
  {
    ok("[K] the replay of a session this page has ALREADY shown is not news: no opening (no toast, nothing starts) -- its position kept",
       replayNotNews(runTranslatorWindow()));
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 3, phase: "done", doneMs: 600000, complete: true, ranges: [[0, 600000]], fresh: true,
                     autoplay: true, resumed: true, startMs: 61000 }));
    ok("[K] ...while news (a file just opened, straight from the cache) gets its opening: \"Resumed at 1:01\"",
       w.ids.resumeToast.hidden === false && w.ids.resumeText.textContent === "Resumed at 1:01");
    ok("[3] NO URL (the folder could not be served): the page asks for the sound at once, ONCE, by session",
       noUrlAsksSound(runTranslatorWindow()));
    const w2 = runTranslatorWindow();
    w2.send(fileMsg({ jobId: 2, seq: 6, phase: "loading", url: "", proxyState: "working" }));
    w2.send(fileMsg({ jobId: 4, seq: 7, phase: "idle", url: "", name: "" }));
    ok("[3] ...not when the bridge already asked (proxyState), and never for an idle window",
       countType(w2, "translatorFilePlayFailed") === 0);
    ok("[4] a .txt dropped with NO MIME type is refused before anything is sent (an .srt is a SUBTITLE file since WE)",
       txtDropRefused(runTranslatorWindow()));
    const w3 = runTranslatorWindow();
    w3.dispatchDoc("drop", { dataTransfer: { files: [{ name: "photo.jpg", type: "image/jpeg" }, { name: "Clip.MKV", type: "" }] } });
    ok("[4] ...an .MKV with no MIME type (common) is taken, and a mixed drop hands over its media only",
       countType(w3, "translatorFileDrop") === 1 && w3.objs.length === 1 && w3.objs[0].length === 1 && w3.objs[0][0].name === "Clip.MKV");
    const jsExts = (/const MEDIA_EXTS = \[([\s\S]*?)\];/.exec(html) || ["", ""])[1].match(/"([a-z0-9]+)"/g) || [];
    const cExts = (/static const wchar_t\* const k\[\] = \{([\s\S]*?)\};/.exec(P.cpp("ui/TrFileStore.h")) || ["", ""])[1]
      .match(/L"([a-z0-9]+)"/g) || [];
    const norm = (a) => a.map((x) => x.replace(/^L/, "").replace(/"/g, "")).sort().join(",");
    ok("[4] the page's media list is the app's, extension for extension (TrFileStore.h mediaExts, " + P.cppSource("ui/TrFileStore.h").from + ")",
       jsExts.length >= 30 && norm(jsExts) === norm(cExts), { page: norm(jsExts), bridge: norm(cExts) });
    ok("[8] closed while a picture-less video was moving to the <audio>: it does NOT start playing -- hidden, nor reopened",
       closeStopsSwitch(runTranslatorWindow()));
  }

  // ---- 5. the window's source, as a SECURITY property and as a contract -------------------------
  section("WC -- index.html: no markup from data (THE SECURITY RULE); the contract with the app");
  {
    // SPLIT_PLAN S7's brief: the build fails on innerHTML / outerHTML / insertAdjacentHTML / document.write in the page --
    // not one of them, read or written, anywhere in its CODE (its comments may name them), nor eval / new Function.
    // Each sink has its sentinel below: a page with it put back must fail this very rule.
    ok("THE SECURITY RULE: nothing in the page's code can build markup -- no innerHTML / outerHTML / insertAdjacentHTML / "
       + "document.write, no eval / new Function (the media folders are readable from this page)",
       !MARKUP_SINK.test(codeOnly(script)), (codeOnly(script).match(MARKUP_SINK) || [])[0]);
    ok("...nor in its markup: no inline event-handler attribute, no javascript: URL",
       !/<[a-z][^>]*\son[a-z]+\s*=/i.test(html.replace(/<script>[\s\S]*?<\/script>/, "")) && !/javascript:/i.test(html));
    [["innerHTML", "el.fileToast.innerHTML = text;"], ["outerHTML", "el.fileToast.outerHTML = text;"],
     ["insertAdjacentHTML", "el.fileToast.insertAdjacentHTML(\"beforeend\", text);"], ["document.write", "document.write(text);"]]
      .forEach(([sink, code]) => {
        const m = script.replace("    el.fileToast.textContent = text;\n", "    " + code + "\n");
        ok("MUTANT \"the toast written with " + sink + "\" is caught by THE SECURITY RULE", m !== script && MARKUP_SINK.test(codeOnly(m)));
      });
    ok("the media elements never carry `crossorigin` (it makes them unplayable under DENY_CORS)",
       !/<(video|audio)[^>]*crossorigin/i.test(html));
    ok("...no native controls at all (OUR transport, D6), and still no Download / picture-in-picture (no subtitles there)",
       /<video id="fileVideo"[^>]*controlslist="nodownload noremoteplayback"[^>]*disablepictureinpicture/.test(html)
       && /<audio id="fileAudio"[^>]*controlslist="nodownload noremoteplayback"/.test(html)
       && !/<(video|audio) id="file(Video|Audio)"[^>]*\scontrols[\s>]/.test(html));
    const looked = [...script.matchAll(/getElementById\("([^"]+)"\)/g)].map((m) => m[1]);
    ok("every id the script looks up exists in the markup",
       looked.every((id) => html.indexOf('id="' + id + '"') >= 0), looked.filter((id) => html.indexOf('id="' + id + '"') < 0));
    [".file-empty", ".file-player", "#fileVideo", ".file-big", ".file-chip", ".file-notice", ".file-toast",
     ".file-stale", ".menu", ".file-sub", "#filePane", "#state", "#bar button",
     ".file-ready", ".resume-toast", ".seek-tip", ".seek-loop", ".pop", ".queue-pane", ".queue-empty", ".file-list",
     ".cc-row", ".full-btn", ".t-btn"].forEach((sel) => {
      ok("index.html: " + sel + " really disappears when hidden ([hidden] guard present)",
         new RegExp(sel.replace(/[.#]/g, "\\$&") + "\\[hidden\\]").test(html));
    });
    ok("index.html: #fileAudio is never drawn (no controls: our transport drives it)", /#fileAudio \{ display: none; \}/.test(html));
    const sent = [...new Set([...script.matchAll(/type:\s*"(translatorFile\w*)"/g)].map((m) => m[1]))]
      .filter((t) => !/Exported|Cues?$|^translatorFile$|Notice/.test(t));
    const handled = (t) => new RegExp('type == L"' + t + '"').test(bridgeSrc);
    ok("every translatorFile* message the page SENDS is handled by the app (" + contract.from + ")",
       sent.length >= 9 && sent.every(handled), sent.filter((t) => !handled(t)));
    const emitted = [...new Set([...(bridgeSrc + playerSrc + winStateSrc).matchAll(/L"type",\s*L"(translatorFile\w*|translatorWindow)"/g)].map((m) => m[1]))];
    ok("every translatorFile* message the app or its player core (TrPlayer.h) EMITS is consumed by the page",
       emitted.length >= 6 && emitted.every((t) => script.indexOf('msg.type === "' + t + '"') >= 0),
       emitted.filter((t) => script.indexOf('msg.type === "' + t + '"') < 0));
  }

  // =============================================================================================
  // WAVE WE (2026-10-11) -- THE PLAYER. The user: start watching after a SHORT load, the translation
  // always AHEAD of the picture and completed in the background; translation on/off (a plain player
  // too); a good player around it (queue, folders, drag and drop, subtitle files, a subtitle style).
  // Every check runs the REAL page; the sentinels in section 7 re-run it with one guard removed each.
  // =============================================================================================
  // ---- the WE probes: each is a check below AND the condition its mutation sentinel must see FAIL --
  const WE_VECTORS = fs.readFileSync(path.join(__dirname, "we_vectors.txt"), "utf8").split(/\r?\n/)
    .filter((l) => l.trim() && l[0] !== "#");
  const pickFn = (src, name) => {             // a function declaration, braces balanced
    const at = src.indexOf("function " + name + "(");
    if (at < 0) return "";
    let i = src.indexOf("{", at), d = 0;
    for (; i < src.length; i++) { if (src[i] === "{") d++; else if (src[i] === "}" && --d === 0) break; }
    return src.slice(at, i + 1);
  };
  // the page's own range rules, lifted out of its script and run on we_vectors.txt (player_smoke.cpp runs
  // the same rows through ui/TrPlayer.h): returns the rows that disagree
  const vectorMisses = (src) => {
    const ready = Number((/const READY_LEAD_MS = (\d+);/.exec(src) || [])[1]);
    const t = new Function("READY_LEAD_MS", ["focusTarget", "leadAt", "coveredToEnd", "readyAt"].map((n) => pickFn(src, n)).join("\n")
      + "\nreturn { focusTarget, leadAt, coveredToEnd, readyAt };")(ready);
    return WE_VECTORS.filter((l) => {
      const [rs, P, D, T, Ld, Rd, At] = l.split("|");
      const r = rs ? rs.split(",").map((x) => x.split("-").map(Number)) : [];
      const p = Number(P), d = Number(D);
      const got = [t.focusTarget(r, p), t.leadAt(r, p), t.readyAt(r, p, d) ? 1 : 0, t.coveredToEnd(r, p, d) ? 1 : 0].join();
      return got !== [Number(T), Number(Ld), Number(Rd), Number(At)].join();
    });
  };
  const vectorsHold = (w) => WE_VECTORS.length >= 16 && vectorMisses(w.script).length === 0;
  const extTextStaysText = (w) => {           // a subtitle FILE's text: runs + flags, never markup
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    metaOk(w.ids.fileVideo);
    w.ids.fileVideo._seek(1);
    w.send({ type: "playerSubs", id: 5, name: "x.srt", enc: 65001, encName: "UTF-8", forced: true, truncated: true,
             part: 0, parts: 1, cues: [{ s: 0, e: 5000, r: [["<i>not italic</i> <b>&amp;</b>", 0]] }] });
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false, extSubId: 5 }));
    const L = ovLines(w);
    return L.length === 1 && L[0].text === "<i>not italic</i> <b>&amp;</b>" && L[0].kids.join() === "#TEXT.";
  };
  const readyWaits15 = (w) => {               // play when ready: 15 s subtitled ahead, not less
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 6000, ranges: [[0, 6000]] }));
    metaOk(v);
    const waited = v.playCalls === 0 && w.ids.fileReady.hidden === false;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 15000, ranges: [[0, 15000]] }));
    return waited && v.playCalls === 1 && v.paused === false;
  };
  const keepAheadHysteresis = (w) => {        // keep ahead: pause under 2 s, play again at 8 s (not 2)
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 10000, ranges: [[0, 10000]] }));
    metaOk(v);
    w.ids.tPlay.click();
    v.currentTime = 8.5; w.pumpRaf(1);
    const stalled = v.paused === true;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 15000, ranges: [[0, 15000]] }));   // 6.5 s ahead
    const held = v.paused === true;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 16500, ranges: [[0, 16500]] }));   // 8 s ahead
    return stalled && held && v.paused === false;
  };
  const queueById = (w) => {                  // the queue is edited by id, never by position
    w.send(qMsg([qItem(11, "a.mkv"), qItem(12, "b.mkv"), qItem(13, "c.mkv")], { cur: 11 }));
    w.outbox.length = 0;
    w.ids.queueList.children[2].dispatch("dblclick");
    const m = last(w, "playerQueue");
    return !!m && m.op === "play" && m.id === 13;
  };
  const dropOnQueue = (w) => {                // a drop on the queue ADDS, anywhere else PLAYS
    w.send(qMsg([qItem(11, "a.mkv")], { cur: 11 }));
    w.ids.tabQueue.click();
    w.dispatchDoc("drop", { target: w.ids.queueList, dataTransfer: { files: [{ name: "b.mkv", type: "" }] } });
    const onQueue = last(w, "translatorFileDrop");
    w.dispatchDoc("drop", { target: w.ids.fileView, dataTransfer: { files: [{ name: "c.mkv", type: "" }] } });
    const onPicture = last(w, "translatorFileDrop");
    return !!onQueue && onQueue.target === "queue" && !!onPicture && onPicture.target === "play";
  };
  const noAutoplayWithoutAsking = (w) => {    // only a file OPENED to play starts by itself
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]], autoplay: false }));
    metaOk(v);
    return v.playCalls === 0 && v.paused === true;
  };
  const closedStaysStill = (w) => {           // a window closed while catching up stays silent
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 10000, ranges: [[0, 10000]] }));
    metaOk(v);
    w.ids.tPlay.click();
    v.currentTime = 8.5; w.pumpRaf(1);         // keep ahead: paused, waiting for 8 s more
    const waiting = v.paused === true;
    w.send({ type: "translatorWindow", shown: false });
    w.send({ type: "translatorWindow", shown: true });   // reopened: the film stays paused (the user's Play starts it)
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 30000, ranges: [[0, 30000]] }));
    return waiting && v.paused === true && v.playCalls === 1;
  };

  // ---- WE-1 --------------------------------------------------------------------------------------
  section("WE-1 -- the player: OUR transport (play/pause, seek, time, volume, mute, speed, A-B)");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", doneMs: 600000, complete: true, ranges: [[0, 600000]] }));
    const v = w.ids.fileVideo;
    metaOk(v);
    ok("a file: the transport says 0:00 / 10:00", w.ids.tTime.textContent === "0:00 / 10:00", w.ids.tTime.textContent);
    w.outbox.length = 0;
    w.ids.tPlay.click();
    ok("Play plays it, the button turns into Pause, and the bridge hears the playhead (playerPos, reason play)",
       v.paused === false && w.ids.tPlay.classList.contains("is-playing") && w.ids.tPlay.title === "Pause (Space)"
       && !!last(w, "playerPos") && last(w, "playerPos").reason === "play" && last(w, "playerPos").gen === 2, w.outbox);
    v.currentTime = 90; w.pumpRaf(1);
    ok("while playing, each FRAME repaints the time and the played share",
       w.ids.tTime.textContent === "1:30 / 10:00" && w.ids.seekPlayed.style.width === "15.00%" && w.ids.seekKnob.style.left === "15.00%",
       [w.ids.tTime.textContent, w.ids.seekPlayed.style.width]);
    ok("...and asks for the next one (one frame at a time, never a pile)", w.rafPending() === 1);
    w.outbox.length = 0;
    w.pumpTimers(5000);
    ok("every 5 s of playing, a `tick` report (where to analyse next, where to resume later)",
       countType(w, "playerPos") === 1 && last(w, "playerPos").reason === "tick" && last(w, "playerPos").ms === 90000, w.outbox);
    w.win.dispatch("keydown", { key: " ", target: w.doc.body });
    ok("Space pauses", v.paused === true && !w.ids.tPlay.classList.contains("is-playing"));
    w.win.dispatch("keydown", { key: " ", target: w.ids.tPlay });
    ok("...but Space on a focused BUTTON is the button's own (no double toggle)", v.paused === true);
    // the seek bar: a press, a drag, one report when it is let go
    w.ids.seekTrack._rect = { left: 100, top: 0, width: 600, height: 10, right: 700, bottom: 10 };
    w.outbox.length = 0;
    w.ids.seek.dispatch("pointerdown", { clientX: 400, pointerId: 1 });
    w.ids.seek.dispatch("pointermove", { clientX: 550, pointerId: 1 });
    ok("dragging the seek bar moves the knob under the pointer and says the time and whether it is subtitled -- no seek yet",
       w.ids.seekKnob.style.left === "75.00%" && w.ids.seekTip.textContent === "7:30 · subtitled" && v.currentTime === 90,
       [w.ids.seekKnob.style.left, w.ids.seekTip.textContent]);
    w.ids.seek.dispatch("pointerup", { clientX: 550, pointerId: 1 });
    ok("...letting go seeks there", Math.abs(v.currentTime - 450.001) < 1e-6, v.currentTime);
    const before = countType(w, "playerPos");
    w.pumpTimers(300);
    ok("...and the bridge hears it ONCE, 250 ms later (a scrub is one report)",
       before === 0 && countType(w, "playerPos") === 1 && last(w, "playerPos").reason === "seek" && last(w, "playerPos").ms === 450001, w.outbox);
    w.win.dispatch("keydown", { key: "ArrowLeft", target: w.doc.body });
    ok("Left: back 5 s", Math.abs(v.currentTime - 445.002) < 1e-3, v.currentTime);
    w.win.dispatch("keydown", { key: "ArrowRight", shiftKey: true, target: w.doc.body });
    ok("Shift+Right: forward 30 s", Math.abs(v.currentTime - 475.003) < 1e-3, v.currentTime);
    // volume, mute
    w.ids.tVol.value = "40"; w.ids.tVol.dispatch("input");
    ok("the volume slider sets the element's volume, remembered", v.volume === 0.4 && w.lsStore["audiopb.tr.vol"] === "40");
    w.win.dispatch("keydown", { key: "m", target: w.doc.body });
    ok("M mutes (and the icon says so), remembered", v.muted === true && w.ids.tMute.classList.contains("is-muted")
       && w.lsStore["audiopb.tr.muted"] === "1");
    w.win.dispatch("keydown", { key: "ArrowUp", target: w.doc.body });
    ok("Up: +5 % -- and turning the volume up unmutes", v.volume === 0.45 && v.muted === false
       && w.ids.fileToast.textContent === "Volume 45 %", [v.volume, v.muted, w.ids.fileToast.textContent]);
    // speed
    w.ids.tRate.click();
    ok("the speed button steps through the rates (1x -> 1.25x), on the element, remembered",
       v.playbackRate === 1.25 && w.ids.tRate.textContent === "1.25×" && w.lsStore["audiopb.tr.rate"] === "1.25", v.playbackRate);
    w.win.dispatch("keydown", { key: "[", target: w.doc.body });
    w.win.dispatch("keydown", { key: "[", target: w.doc.body });
    ok("[ slows down (two steps: 0.75x)", v.playbackRate === 0.75, v.playbackRate);
    // the A-B loop
    v.currentTime = 100;
    w.win.dispatch("keydown", { key: "l", target: w.doc.body });
    v.currentTime = 104;
    w.win.dispatch("keydown", { key: "l", target: w.doc.body });
    ok("L sets A, then B: the loop band on the seek bar", w.ids.seekLoop.hidden === false && w.ids.tLoop.classList.contains("is-on"));
    v._seek(104.2);
    ok("...past B the playhead goes back to A", Math.abs(v.currentTime - 100) < 1e-6, v.currentTime);
    w.win.dispatch("keydown", { key: "l", target: w.doc.body });
    ok("...and L again clears it", w.ids.seekLoop.hidden === true && !w.ids.tLoop.classList.contains("is-on"));
    const w2 = runTranslatorWindow({ ls: { "audiopb.tr.vol": "30", "audiopb.tr.muted": "1", "audiopb.tr.rate": "1.5" } });
    w2.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    const v2 = w2.ids.fileVideo;
    ok("the volume, the mute and the speed come back at the next opening, on the next file's element",
       v2.volume === 0.3 && v2.muted === true && v2.playbackRate === 1.5 && w2.ids.tRate.textContent === "1.5×"
       && w2.ids.tVol.value === "30" && w2.ids.tMute.classList.contains("is-muted"));
  }

  // ---- WE-2 --------------------------------------------------------------------------------------
  section("WE-2 -- the subtitles on the picture: on time, in START order, the delay, the style");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 3, phase: "analyzing", doneMs: 30000, ranges: [[60000, 90000]] }));
    const v = w.ids.fileVideo;
    metaOk(v);
    // v2: the analysis follows the playhead, so a generation's cues arrive in ANY time order
    w.send(cueMsg(3, 0, 70000, 72000, "en", "zh", "seventy", "七十"));
    w.send(cueMsg(3, 1, 61000, 63000, "en", "zh", "sixty-one", "六十一"));
    w.send(cueMsg(3, 2, 65000, 66000, "en", "zh", "sixty-five", "六十五"));
    ok("cues arriving out of time order are kept in START order (the list: 1:01, 1:05, 1:10)",
       rows(w).map((r) => r.querySelector(".file-t").textContent).join(",") === "1:01,1:05,1:10", rows(w).map((r) => r.dataset.idx));
    v._seek(65.5);
    ok("the one under the playhead is on the picture", ovLines(w).length === 2 && ovLines(w)[0].text === "六十五", ovLines(w));
    w.ids.tPlay.click();
    v.currentTime = 66.2; w.pumpRaf(1);
    ok("ON TIME: at its end the next FRAME takes it off (no timeupdate needed)", ovLines(w).length === 0, ovLines(w));
    v.currentTime = 70.01; w.pumpRaf(1);
    ok("...and the next one is there on the frame it starts", ovLines(w).length === 2 && ovLines(w)[0].text === "七十");
    const n0 = w.ids.subOverlay.children[0];
    w.pumpRaf(1);
    ok("a frame that changes nothing touches nothing (the same line elements stay)", w.ids.subOverlay.children[0] === n0);
    w.ids.tPlay.click();
    // the delay (G earlier, H later), per FILE
    v._seek(71.5);
    for (let i = 0; i < 20; i++) w.win.dispatch("keydown", { key: "h", target: w.doc.body });
    ok("H x20: the subtitles 2.0 s LATER -- at 1:11.5 the picture shows what was said at 1:09.5 (nothing)",
       ovLines(w).length === 0 && w.ids.fileToast.textContent === "Subtitle delay +2.0 s", w.ids.fileToast.textContent);
    v._seek(72.5);
    ok("...and the 1:10 line at 1:12.5", ovLines(w).length === 2 && ovLines(w)[0].text === "七十", ovLines(w));
    ok("...remembered for THIS file, by its media key", JSON.parse(w.lsStore["audiopb.tr.subDelay"]).mk1.ms === 2000);
    w.ids.ccDelayReset.click();
    ok("Reset: back to 0, nothing kept for it", ovLines(w).length === 0 && !JSON.parse(w.lsStore["audiopb.tr.subDelay"]).mk1);
    const wd = runTranslatorWindow({ ls: { "audiopb.tr.subDelay": JSON.stringify({ mk9: { ms: -500, t: 1 } }) } });
    wd.send(fileMsg({ jobId: 2, phase: "done", complete: true, mediaKey: "mk9" }));
    wd.ids.tCC.click();
    ok("a file's delay comes back when that file is played again", wd.ids.ccDelayVal.textContent === "−0.5 s", wd.ids.ccDelayVal.textContent);
    // the style, from this viewer's storage
    const ws = runTranslatorWindow({ ls: { "audiopb.tr.sub": JSON.stringify({ size: "l", place: "top", offset: 12, bg: 0,
      color: "#ffe14d", edge: "shadow", font: "serif" }) } });
    const o = ws.ids.subOverlay, fv = ws.ids.fileView;
    ok("the subtitle style comes from storage: size L, at the top, 12 %, no background, yellow, shadow, serif",
       fv.style["--sub-k"] === "6.500" && o.classList.contains("sub-top") && !o.classList.contains("sub-bottom")
       && fv.style["--sub-offset"] === "12%" && fv.style["--sub-bg"] === "0.00" && fv.style["--sub-color"] === "#ffe14d"
       && o.classList.contains("edge-shadow") && o.classList.contains("font-serif"), [fv.style["--sub-k"], o.className]);
    const wg = runTranslatorWindow({ ls: { "audiopb.tr.sub": "{\"size\":\"huge\",\"color\":\"javascript:x\",\"offset\":999}" } });
    ok("...a value it does not know falls back (the default size 4.0 %, white) and an offset is clamped (40 %)",
       wg.ids.fileView.style["--sub-k"] === "4.000" && wg.ids.fileView.style["--sub-color"] === "#ffffff"
       && wg.ids.fileView.style["--sub-offset"] === "40%");
    const wk = runTranslatorWindow({ ls: { "audiopb.tr.sub": "{\"k\":99}" } });
    const wk2 = runTranslatorWindow({ ls: { "audiopb.tr.sub": "{\"k\":2.34}" } });
    ok("the size is a SLIDER value (user, 11/10): clamped to 1.5-10 %, kept to 0.1 % (99 -> 10.0, 2.34 -> 2.3)",
       wk.ids.fileView.style["--sub-k"] === "10.000" && wk2.ids.fileView.style["--sub-k"] === "2.300",
       [wk.ids.fileView.style["--sub-k"], wk2.ids.fileView.style["--sub-k"]]);
    ok("the slider goes well BELOW the old S (4.2 %): 1.5 % minimum, and the CSS floor lets a small size stay small (9 px)",
       /<input id="ccSize" type="range" min="1\.5" max="10" step="0\.1"/.test(html)
       && /font-size: clamp\(9px, calc\(var\(--sub-k, 4\) \* 1cqh\), 160px\)/.test(html));
    const wc = runTranslatorWindow();
    wc.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    wc.ids.tCC.click();
    const segBtn = (host, val) => host.children.find((b) => b.dataset.v === val);
    wc.ids.ccSize.value = "8.3"; wc.ids.ccSize.dispatch("input");
    segBtn(wc.ids.ccPlace, "top").click();
    segBtn(wc.ids.ccColor, "#7ee6ff").click();
    segBtn(wc.ids.ccEdge, "none").click();
    wc.ids.ccFont.value = "mono"; wc.ids.ccFont.dispatch("change");
    wc.ids.ccBg.value = "25"; wc.ids.ccBg.dispatch("input");
    const st = JSON.parse(wc.lsStore["audiopb.tr.sub"]);
    ok("the CC panel's Style: size 8.3 % (slider), top, cyan, no edge, monospace, 25 % background -- applied at once and remembered",
       st.k === 8.3 && st.place === "top" && st.color === "#7ee6ff" && st.edge === "none" && st.font === "mono" && st.bg === 25
       && wc.ids.fileView.style["--sub-k"] === "8.300" && wc.ids.subOverlay.classList.contains("edge-none")
       && wc.ids.subOverlay.classList.contains("font-mono") && wc.ids.ccBgVal.textContent === "25 %"
       && wc.ids.ccSizeVal.textContent === "8.3 %", st);
  }

  // ---- WE-3 --------------------------------------------------------------------------------------
  section("WE-3 -- what is subtitled: the green stretches, the chip, the strip's \"ahead\"");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 90000, ranges: [[0, 30000], [120000, 180000]] }));
    const v = w.ids.fileVideo;
    metaOk(v);
    const segs = w.ids.seekRanges.children.map((s) => s.style.left + "+" + s.style.width);
    ok("two subtitled stretches = two green segments, where they are", segs.join(",") === "0.00%+5.00%,20.00%+10.00%", segs);
    v._seek(10);
    ok("the strip says how far AHEAD of the playhead the subtitles go", /· 0:20 ahead/.test(w.ids.fileStatus.textContent),
       w.ids.fileStatus.textContent);
    v._seek(60);
    ok("outside them: the chip says it is not subtitled here YET -- and the strip drops \"ahead\"",
       w.ids.fileChip.hidden === false && w.ids.fileChip.textContent.indexOf("Not subtitled here yet") === 0
       && !/ahead/.test(w.ids.fileStatus.textContent), [w.ids.fileChip.textContent, w.ids.fileStatus.textContent]);
    v._seek(130);
    ok("...inside the second: no chip, 0:50 ahead", w.ids.fileChip.hidden === true && /· 0:50 ahead/.test(w.ids.fileStatus.textContent));
    w.ids.seekTrack._rect = { left: 0, top: 0, width: 600, height: 10, right: 600, bottom: 10 };
    w.ids.seek.dispatch("pointermove", { clientX: 60 });
    ok("hovering the bar: the time there, and \"not yet\" where nothing is subtitled",
       w.ids.seekTip.textContent === "1:00 · not yet" && w.ids.seekTip.hidden === false, w.ids.seekTip.textContent);
    w.ids.seek.dispatch("pointerleave", {});
    ok("...gone when the pointer leaves", w.ids.seekTip.hidden === true);
    w.send(fileMsg({ jobId: 2, phase: "done", doneMs: 600000, complete: true, ranges: [[0, 600000]] }));
    ok("complete: one stretch, the whole bar", w.ids.seekRanges.children.length === 1
       && w.ids.seekRanges.children[0].style.width === "100.00%");
  }

  // ---- WE-4 --------------------------------------------------------------------------------------
  section("WE-4 -- play when ready (15 s ahead) and keep ahead (pause under 2 s, again at 8 s)");
  {
    const w = runTranslatorWindow();
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 2, phase: "loading", autoplay: true }));
    ok("a file opened to PLAY while nothing is subtitled yet: armed, saying so -- not playing",
       w.ids.fileReady.hidden === false && w.ids.fileReadyText.textContent === "Loading the models…"
       && w.ids.fileReadyBtn.textContent === "Play now" && v.playCalls === 0, w.ids.fileReadyText.textContent);
    metaOk(v);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 6000, ranges: [[0, 6000]] }));
    ok("...the analysis running: \"Preparing subtitles · 6 of 15 s\", still waiting",
       w.ids.fileReadyText.textContent === "Preparing subtitles · 6 of 15 s" && v.paused === true && v.playCalls === 0,
       w.ids.fileReadyText.textContent);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 14999, ranges: [[0, 14999]] }));
    ok("...14.999 s is not 15", v.playCalls === 0);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 15000, ranges: [[0, 15000]] }));
    ok("...15 s ahead: it starts BY ITSELF, and the card goes", v.playCalls === 1 && v.paused === false && w.ids.fileReady.hidden === true);
    ok("(the probe: it waits for 15 s, then plays)", readyWaits15(runTranslatorWindow()));
    // keep ahead
    v.currentTime = 13.5; w.pumpRaf(1);
    ok("KEEP AHEAD: under 2 s of subtitles left at the playhead -> it pauses, and says why",
       v.paused === true && w.ids.fileReady.hidden === false && w.ids.fileReadyText.textContent === "Translating ahead…"
       && w.ids.fileReadyBtn.textContent === "Play without waiting", w.ids.fileReadyText.textContent);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 20000, ranges: [[0, 20000]] }));
    ok("...6.5 s ahead is not enough (hysteresis: it waits for 8 s)", v.paused === true);
    w.send(fileMsg({ jobId: 2, phase: "analyzing", autoplay: true, doneMs: 21500, ranges: [[0, 21500]] }));
    ok("...8 s ahead: it plays again by itself", v.paused === false && w.ids.fileReady.hidden === true);
    ok("(the probe: 6.5 s holds, 8 s plays)", keepAheadHysteresis(runTranslatorWindow()));
    v.currentTime = 20.5; w.pumpRaf(1);
    ok("...and pauses again when it catches up", v.paused === true && w.ids.fileReadyText.textContent === "Translating ahead…");
    w.ids.fileReadyBtn.click();
    ok("\"Play without waiting\": it plays -- and keep-ahead leaves THIS file alone from now on", v.paused === false);
    v.currentTime = 21.4; w.pumpRaf(1);
    ok("...(past the subtitles, still playing)", v.paused === false);
    ok("a window closed while waiting to catch up does NOT start again when the subtitles arrive", closedStaysStill(runTranslatorWindow()));
    const w3 = runTranslatorWindow({ ls: { "audiopb.tr.keepAhead": "0" } });
    const v3 = w3.ids.fileVideo;
    w3.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 10000, ranges: [[0, 10000]] }));
    metaOk(v3);
    w3.ids.tPlay.click();
    v3.currentTime = 9.5; w3.pumpRaf(1);
    w3.ids.tCC.click();
    ok("keep-ahead OFF (the CC panel's box, remembered): it plays through", v3.paused === false && w3.ids.ccKeepAhead.checked === false);
    const w4 = runTranslatorWindow();
    w4.send(fileMsg({ jobId: 2, phase: "off", translate: false, autoplay: true }));
    ok("translation OFF: no card at all -- nothing to wait for", w4.ids.fileReady.hidden === true);
    metaOk(w4.ids.fileVideo);
    ok("...a file opened to play plays as soon as it can (a plain player)", w4.ids.fileVideo.playCalls === 1);
    ok("only a file OPENED to play starts by itself (a replay, a re-analysis do not)", noAutoplayWithoutAsking(runTranslatorWindow()));
    const wb = runTranslatorWindow({ blockPlay: true });
    wb.send(fileMsg({ jobId: 2, phase: "done", complete: true, autoplay: true, ranges: [[0, 600000]] }));
    metaOk(wb.ids.fileVideo);
    ok("no autoplay switch (main.cpp's retry-without): the REFUSED play() becomes a big \"Play\" -- never silence",
       wb.ids.fileVideo.playCalls === 1 && wb.ids.fileReady.hidden === false && wb.ids.fileReadyText.textContent === "Ready to play"
       && wb.ids.fileReadyBtn.textContent === "Play", wb.ids.fileReadyText.textContent);
    ok("the app's WebView2 environment is created WITH --autoplay-policy=no-user-gesture-required (" + mainSrc.from + ")",
       /put_AdditionalBrowserArguments\(\s*L"--autoplay-policy=no-user-gesture-required"/.test(mainSrcWE));
    // User report 11/10 (1.0.6): subtitles vanished after a few seconds without a mouse move = the video promoted to a
    // hardware overlay plane once the scene was still, the page's layer over it no longer presented on his AMD driver.
    ok("...the SAME switched environment keeps the video out of DirectComposition overlays and turns audioTracks on",
       /put_AdditionalBrowserArguments\(\s*L"--autoplay-policy=no-user-gesture-required"\s*L" --disable-direct-composition-video-overlays"\s*L" --enable-blink-features=AudioVideoTracks"\)/.test(mainSrcWE));
    // WH (11/10): --disable-direct-composition fixed the full-screen subtitles but turned HEVC decoding OFF (measured: every
    // HEVC file DECODER_ERROR_NOT_SUPPORTED). It must never come back; the page's blend mode does that job now.
    ok("...and DirectComposition itself stays ON (without it this runtime has no HEVC decoder: measured 11/10)",
       !/L"[^"]*--disable-direct-composition"/.test(mainSrcWE) && !/--disable-direct-composition[ "](?!-)/.test(mainSrcWE.replace(/\/\/[^\n]*/g, "")));
    const trCss = html;
    ok("the picture is blended into an ISOLATED black group (never promoted to a hardware plane: the full-screen subtitle fix)",
       /\.file-view \{[^}]*isolation: isolate;[^}]*background: #000;/.test(trCss)
       && /#fileVideo \{[^}]*mix-blend-mode: plus-lighter;/.test(trCss)
       && /\.file-view\.render-plain #fileVideo \{ mix-blend-mode: normal; \}/.test(trCss));
    const wr = runTranslatorWindow();
    ok("rendering: standard by default (no canvas, no render class)",
       wr.ids.fileCanvas.hidden === true && !wr.ids.fileView.classList.contains("render-canvas") && !wr.ids.fileView.classList.contains("render-plain"));
    const keyV = () => wr.win.dispatch("keydown", { key: "v", ctrlKey: true, altKey: true, target: null });
    keyV();
    ok("Ctrl+Alt+V: the canvas rendering (canvas shown, the video invisible under it), kept per profile, said in a toast",
       wr.ids.fileCanvas.hidden === false && wr.ids.fileView.classList.contains("render-canvas")
       && wr.lsStore["audiopb.tr.render"] === "canvas" && /compatibility \(canvas\)/.test(wr.ids.fileToast ? wr.ids.fileToast.textContent : ""),
       wr.lsStore["audiopb.tr.render"]);
    keyV();
    ok("Ctrl+Alt+V again: the direct (diagnostic) rendering", wr.ids.fileView.classList.contains("render-plain") && wr.ids.fileCanvas.hidden === true
       && wr.lsStore["audiopb.tr.render"] === "plain");
    keyV();
    ok("...and back to standard", !wr.ids.fileView.classList.contains("render-plain") && !wr.ids.fileView.classList.contains("render-canvas")
       && wr.lsStore["audiopb.tr.render"] === "blend");
  }

  // ---- WE-5 --------------------------------------------------------------------------------------
  // U1 (02/11/2026): the On/Off SWITCH of WE-5 is gone -- a file is a plain player until Translate is pressed on it (or
  // Translate all queues it). What WE-5 proved of the "off" state still holds for a file nobody asked to translate; the
  // press, the batch and their sentinels are test_u1.js's.
  section("WE-5 -- a file nobody asked to translate: a plain player (U1: the switch became the Translate button, D9)");
  {
    const w = runTranslatorWindow();
    ok("the empty state carries NO switch any more (U1): the pair and the model only",
       !w.ids.emptyTranslate && !/id="emptyTranslate"/.test(html) && w.ids.trPair.parentNode === w.ids.emptyPairSlot);
    w.send(fileMsg({ jobId: 2, phase: "off", translate: false }));
    const v = w.ids.fileVideo;
    metaOk(v);
    ok("a file not translated: the strip says how to get its subtitles, Translate offered -- nothing to cancel or re-analyse",
       w.ids.fileStatus.textContent === "Not translated yet · press Translate to make its subtitles" && w.ids.fileReanalyse.hidden === true
       && w.ids.fileCancel.hidden === true && w.ids.fileTranslate.hidden === false, w.ids.fileStatus.textContent);
    w.ids.tCC.click();
    const on = w.ids.ccSources.children.filter((b) => b.classList.contains("is-on")).map((b) => b.textContent);
    ok("...and the subtitles are Off (the CC panel says so)", on.join() === "Off", on);
    w.ids.tCC.click();
    v._seek(42);
    w.outbox.length = 0;
    w.ids.fileTranslate.click();
    ok("Translate asks the bridge to analyse THIS file (translatorFileAnalyse{force:false}: from the playhead it has, what is kept resumed)",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "translatorFileAnalyse", force: false, quality: "auto" }]), w.outbox);
    const w2 = runTranslatorWindow();
    w2.send(fileMsg({ jobId: 3, phase: "off", translate: false, doneMs: 120000, ranges: [[0, 120000]], count: 1 }));
    w2.send({ type: "translatorFileCues", jobId: 3, cues: [{ idx: 0, s: 1000, e: 2000, lang: "en", dstLang: "zh", pass: false, src: "a", dst: "一" }] });
    ok("a PARTIAL analysis from before: the strip says what there is, how much of the file, and that Translate finishes it",
       w2.ids.fileStatus.textContent === "1 subtitle · 20% of the file analysed · Translate finishes it", w2.ids.fileStatus.textContent);
    w2.ids.tCC.click();
    const on2 = w2.ids.ccSources.children.filter((b) => b.classList.contains("is-on")).map((b) => b.textContent);
    ok("...and its subtitles are SHOWN without a press (U1) -- SuVtitle's, selectable in CC (D9)",
       w2.ids.ccSources.children.some((b) => /^SuVtitle · both languages/.test(b.textContent)) && /^SuVtitle/.test(on2.join()), on2);
    const w3 = runTranslatorWindow();
    w3.send(fileMsg({ jobId: 2, phase: "off", translate: false,
                      sidecars: [{ id: 7, name: "film.en.srt", lang: "en" }, { id: 8, name: "film.zh.srt", lang: "zh" }] }));
    ok("a file not translated with subtitle files next to it: the one in the pair's language is asked for, BY ID -- as the DEFAULT "
       + "rule's choice: auto (CONTRACT ADDENDUM 2: the app ignores it when the user opened his own file with the film)",
       JSON.stringify(last(w3, "playerSubGet")) === JSON.stringify({ type: "playerSubGet", id: 8, auto: true }), w3.outbox);
  }

  // ---- WE-6 --------------------------------------------------------------------------------------
  section("WE-6 -- the queue: the panel, the badges, play/remove/move/clear, repeat/shuffle, N/P, the end of a file");
  {
    const w = runTranslatorWindow();
    w.send(qMsg([qItem(11, "E01 Pilot.mkv", { state: "ready", durMs: 2530000 }),
                 qItem(12, "E02 The Bridge.mkv", { state: "partial", pct: 35, resumeMs: 751000, durMs: 2642000 }),
                 qItem(13, "E03.mkv", { state: "analysing", pct: 12 }), qItem(14, "E04.mkv", { state: "error" }),
                 qItem(15, "E05.mkv", { state: "missing" })], { cur: 11, loaded: false }));
    ok("a queue and nothing loaded: the PLAYER shows (a file to press play on), not the drop zone",
       w.ids.filePlayer.hidden === false && w.ids.fileEmpty.hidden === true && /5 files in the queue/.test(w.ids.fileStatus.textContent),
       w.ids.fileStatus.textContent);
    const qr = w.ids.queueList.children;
    const badge = (i) => qr[i].children[3];
    // (U3 re-point: + the transcript's badge after the translation's: seven children)
    ok("five rows, each: grip, number, name + folder, badge, the transcript's badge, duration or resume point, remove",
       qr.length === 5 && qr[0].children.length === 7 && qr[1].children[2].children[0].textContent === "E02 The Bridge.mkv"
       && qr[1].children[2].children[1].textContent === "Season 1" && qr[0].classList.contains("is-cur"));
    ok("badges: ✓ done (U1: his word), ◐ 35 % partly, ⟳ 12 % being made, ⚠ failed, missing (greyed)",
       badge(0).textContent === "✓ done" && badge(1).textContent === "◐ 35 %" && badge(2).textContent === "⟳ 12 %"
       && badge(3).textContent === "⚠ failed" && badge(4).textContent === "missing" && qr[4].classList.contains("is-missing")
       && badge(0).classList.contains("st-ready") && badge(1).classList.contains("st-partial"), qr.map((r, i) => badge(i).textContent));
    ok("a resume point shows as ▶ 12:31, else the duration", qr[1].children[5].textContent === "▶ 12:31"
       && qr[0].children[5].textContent === "42:10", [qr[1].children[5].textContent, qr[0].children[5].textContent]);
    ok("the tab counts them", w.ids.tabQueue.textContent === "Queue · 5", w.ids.tabQueue.textContent);
    w.outbox.length = 0;
    w.ids.tPlay.click();
    ok("Play with nothing loaded starts the queue at its current item (by id)",
       JSON.stringify(last(w, "playerQueue")) === JSON.stringify({ type: "playerQueue", op: "play", id: 11 }), w.outbox);
    w.outbox.length = 0;
    qr[2].dispatch("dblclick");
    qr[3].children[6].click();                                        // (U3 re-point: ✕ is the 7th now)
    ok("double-click plays a row, ✕ removes one -- BY ID, never by position",
       JSON.stringify(w.outbox.filter((m) => m.type === "playerQueue"))
       === JSON.stringify([{ type: "playerQueue", op: "play", id: 13 }, { type: "playerQueue", op: "remove", id: 14 }]), w.outbox);
    ok("(the probe: by id)", queueById(runTranslatorWindow()));
    qr.forEach((r, i) => { r._rect = { left: 0, top: i * 40, width: 300, height: 40, right: 300, bottom: i * 40 + 40 }; });
    w.outbox.length = 0;
    qr[0].children[0].dispatch("pointerdown", {});
    w.win.dispatch("pointermove", { clientY: 125 });
    ok("dragging a row by its grip marks where it would land", qr[3].classList.contains("drop-before") && qr[0].classList.contains("is-drag"));
    w.win.dispatch("pointerup", {});
    ok("...dropping it there MOVES it (by id, to its index once it has left its place)",
       JSON.stringify(last(w, "playerQueue")) === JSON.stringify({ type: "playerQueue", op: "move", id: 11, to: 2 })
       && !qr[3].classList.contains("drop-before") && !qr[0].classList.contains("is-drag"), w.outbox);
    w.outbox.length = 0;
    w.ids.qClear.click(); w.ids.qAddFiles.click(); w.ids.qAddFolder.click();
    ok("Clear, + Files…, + Folder… ask the bridge (it owns the queue and the dialogs)",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerQueue", op: "clear" }, { type: "playerAdd", what: "files" },
         { type: "playerAdd", what: "folder" }]), w.outbox);
    w.outbox.length = 0;
    w.ids.tRepeat.click(); w.ids.tRepeat.click(); w.ids.tRepeat.click(); w.ids.tShuffle.click();
    ok("Repeat cycles off -> the queue -> this file -> off; Shuffle toggles -- each one playerSet",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerSet", repeat: "all" }, { type: "playerSet", repeat: "one" },
         { type: "playerSet", repeat: "off" }, { type: "playerSet", shuffle: true }]), w.outbox);
    w.send(qMsg([qItem(11, "E01.mkv")], { cur: 11, loaded: true, repeat: "one", shuffle: true }));
    ok("...and the bridge's echo is what the buttons show (repeat ONE wears its \"1\")",
       w.ids.tRepeat.classList.contains("is-one") && w.ids.tShuffle.classList.contains("is-on") && w.ids.tabQueue.textContent === "Queue · 1");
    w.send(fileMsg({ jobId: 5, itemId: 11, phase: "done", complete: true, ranges: [[0, 600000]] }));
    const v = w.ids.fileVideo;
    metaOk(v);
    w.outbox.length = 0;
    v.currentTime = 4;
    w.win.dispatch("keydown", { key: "n", target: w.doc.body });
    w.win.dispatch("keydown", { key: "p", target: w.doc.body });
    ok("N = next, P = previous WITH the playhead (past 3 s the bridge restarts the file instead)",
       JSON.stringify(w.outbox.filter((m) => m.type === "playerQueue"))
       === JSON.stringify([{ type: "playerQueue", op: "next" }, { type: "playerQueue", op: "prev", ms: 4000 }]), w.outbox);
    w.outbox.length = 0;
    v._fire("ended", { paused: true });
    ok("the end of the file is told to the bridge (it plays the next one, or this one again)",
       !!last(w, "playerPos") && last(w, "playerPos").reason === "ended" && last(w, "playerPos").gen === 5, w.outbox);
    w.send({ type: "playerCmd", gen: 5, op: "restart", ms: 0 });
    ok("repeat ONE: the bridge's restart plays it again from the top", v.currentTime < 0.01 && v.paused === false);
    w.send({ type: "playerCmd", gen: 4, op: "pause" });
    ok("...a command for another generation is ignored", v.paused === false);
    w.send({ type: "playerItem", id: 11, state: "analysing", pct: 40 });
    ok("a badge delta (playerItem) repaints its row", w.ids.queueList.children[0].children[3].textContent === "⟳ 40 %");
  }

  // ---- WE-7 --------------------------------------------------------------------------------------
  section("WE-7 -- drops: files, FOLDERS and subtitle files; on the queue = added, anywhere else = played");
  {
    const w = runTranslatorWindow();
    const folder = { name: "Season 1", type: "", size: 0 }, film = { name: "E01.mkv", type: "" };
    const subs = { name: "E01.fr.srt", type: "" }, junk = { name: "cover.jpg", type: "image/jpeg" };
    const entry = (dir) => ({ kind: "file", webkitGetAsEntry: () => ({ isDirectory: dir }) });
    w.dispatchDoc("drop", { target: w.ids.fileView, dataTransfer: { files: [folder, film, subs, junk],
      items: [entry(true), entry(false), { kind: "string" }, entry(false), entry(false)] } });
    ok("a FOLDER, a film and its .srt dropped on the picture: the three handed over (the junk left out), target PLAY",
       JSON.stringify(last(w, "translatorFileDrop")) === JSON.stringify({ type: "translatorFileDrop", quality: "auto", target: "play" })
       && w.objs.length === 1 && w.objs[0].length === 3 && w.objs[0][0] === folder && w.objs[0][1] === film && w.objs[0][2] === subs,
       w.objs.map((l) => l.map((f) => f.name)));
    ok("on the QUEUE panel = added to the queue; anywhere else = played now", dropOnQueue(runTranslatorWindow()));
    const w2 = runTranslatorWindow();
    w2.dispatchDoc("drop", { target: null, dataTransfer: { files: [{ name: "Season 2", type: "" }] } });
    ok("a name with no extension (a folder the entry API could not name) is handed over: the APP asks the disk",
       countType(w2, "translatorFileDrop") === 1 && w2.objs[0][0].name === "Season 2" && w2.ids.filePane.hidden === false);
    const w3 = runTranslatorWindow();
    const over = w3.dispatchDoc("dragover", { target: w3.ids.queueList, dataTransfer: { dropEffect: "" } });
    ok("dragover over a CLOSED queue panel: taken, but no queue highlight (only an open Queue tab takes a drop into the queue)",
       over.defaultPrevented === true && w3.doc.body.classList.contains("is-dragging") && !w3.doc.body.classList.contains("drag-queue"));
    w3.dispatchDoc("dragleave", { relatedTarget: null });
    ok("...and it goes when the drag leaves the window", !w3.doc.body.classList.contains("is-dragging"));
  }

  // ---- WE-8 --------------------------------------------------------------------------------------
  section("WE-8 -- the CC panel: the sources, a subtitle FILE (runs, overlaps, encodings), Load…, S");
  {
    const w = runTranslatorWindow();
    const v = w.ids.fileVideo;
    const side = [{ id: 21, name: "film.fr.srt", lang: "fr" }];
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 30000, ranges: [[0, 30000]], sidecars: side }));
    metaOk(v);
    w.send(cueMsg(2, 0, 1000, 3000, "en", "zh", "Hello", "你好"));
    v._seek(2);
    w.ids.tCC.click();
    const src = () => w.ids.ccSources.children.map((b) => b.textContent + (b.classList.contains("is-on") ? "*" : ""));
    ok("the sources: SuVtitle both / English (original) / 简体中文 (Simplified Chinese) (translation), the .srt next to the film, Off -- SuVtitle both on",
       src().join("|") === "SuVtitle · both languages*|SuVtitle · English (original)|SuVtitle · 简体中文 (Simplified Chinese) (translation)|film.fr.srt (next to the file)|Off",
       src());
    w.ids.ccSources.children[2].click();
    ok("choosing one language sets THE display rule (one rule, D12; 641f1aa's Show plate too), kept", w.lsStore["audiopb.tr.show"] === "b"
       && ovLines(w).length === 1 && ovLines(w)[0].text === "你好");
    w.outbox.length = 0;
    w.ids.ccSources.children[3].click();
    ok("a file next to the film is asked for BY ID (its path never travels)",
       JSON.stringify(last(w, "playerSubGet")) === JSON.stringify({ type: "playerSubGet", id: 21 }), w.outbox);
    w.send({ type: "playerSubs", id: 21, name: "film.fr.srt", enc: 1252, encName: "Western (Windows-1252)", forced: false,
             truncated: false, part: 0, parts: 2, cues: [{ s: 1000, e: 3000, r: [["Bonjour ", 0], ["à tous", 1]] }] });
    ok("(the first of two parts changes nothing yet)", ovLines(w).length === 1 && ovLines(w)[0].text === "你好");
    w.send({ type: "playerSubs", id: 21, name: "film.fr.srt", enc: 1252, encName: "Western (Windows-1252)", forced: false,
             truncated: false, part: 1, parts: 2, cues: [{ s: 2500, e: 4000, r: [["Deux\nlignes", 2]] }] });
    const L = ovLines(w);
    ok("complete: shown -- its runs as spans, italic by a FLAG (r-i), never by reading the text",
       L.length === 1 && L[0].text === "Bonjour à tous" && L[0].kids.join() === "#TEXT.,SPAN.r-i", L);
    // WG re-point (item 5, the user's words): the toast says "Subtitles loaded: <name>", then the encoding as before
    ok("...the toast names it and its encoding: \"Subtitles loaded: film.fr.srt · Western (Windows-1252)\"",
       w.ids.fileToast.textContent === "Subtitles loaded: film.fr.srt · Western (Windows-1252)", w.ids.fileToast.textContent);
    v._seek(2.6);
    const L2 = ovLines(w);
    ok("a FILE's cues may overlap (two voices): both shown; a \\n is a new line; bold by its flag",
       L2.length === 3 && L2[0].text === "Bonjour à tous" && L2[1].text === "Deux" && L2[2].text === "lignes"
       && L2[1].kids.join() === "SPAN.r-b", L2);
    ok("the list shows the FILE's subtitles while it is the source", rows(w).length === 2
       && rows(w)[0].children[1].textContent === "Bonjour à tous");
    ok("the Encoding row appears for a FILE: \"Detect automatically\" + every code page SubtitleReader.h can force",
       w.ids.ccEncRow.hidden === false && w.ids.ccEnc.children.length === 20 && w.ids.ccEnc.value === "0"
       && w.ids.ccEnc.children[0].textContent === "Detect automatically");
    w.outbox.length = 0;
    w.ids.ccEnc.value = "936"; w.ids.ccEnc.dispatch("change");
    ok("choosing one re-reads THE SAME file in it (by id + code page)",
       JSON.stringify(last(w, "playerSubGet")) === JSON.stringify({ type: "playerSubGet", id: 21, enc: 936 }), w.outbox);
    w.ids.ccSources.children[0].click();
    ok("back to SuVtitle's: its subtitles on the picture and in the list again", ovLines(w)[0].text === "你好" && rows(w).length === 1);
    w.outbox.length = 0;
    w.ids.ccLoad.click();
    ok("\"Load a subtitle file…\" asks the bridge for its dialog, and closes the panel",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "playerSubLoad" }]) && w.ids.ccPanel.hidden === true, w.outbox);
    // the bridge's order for a NEW file: its cues, THEN the state that names it (bridge.cpp loadExtSub)
    w.send({ type: "playerSubs", id: 30, name: "other.ass", enc: 65001, encName: "UTF-8", forced: false, truncated: false,
             part: 0, parts: 1, cues: [{ s: 0, e: 9000, r: [["ASS line", 0]] }] });
    ok("(a new file's cues alone do not take over the AudioPB subtitles)", ovLines(w)[0].text === "你好");
    w.send(fileMsg({ jobId: 2, phase: "analyzing", doneMs: 30000, ranges: [[0, 30000]], sidecars: side, extSubId: 30 }));
    ok("...the state that NAMES it (extSubId) shows it", ovLines(w).length === 1 && ovLines(w)[0].text === "ASS line", ovLines(w));
    w.win.dispatch("keydown", { key: "s", target: w.doc.body });
    ok("S cycles to the next source (Off after the last file) and says which",
       w.ids.fileToast.textContent === "Subtitles: Off" && ovLines(w).length === 0, w.ids.fileToast.textContent);
    ok("a subtitle FILE's text is never markup: an \"<i>\" in it is shown, not obeyed (only the run flags style)",
       extTextStaysText(runTranslatorWindow()));
    const wx = runTranslatorWindow();
    extTextStaysText(wx);
    ok("...a file cut at 20 000 cues says so", /cut at 20000/.test(wx.ids.fileToast.textContent), wx.ids.fileToast.textContent);
  }

  // ---- WE-9 --------------------------------------------------------------------------------------
  section("WE-9 -- the keys, and the help that lists them");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    metaOk(w.ids.fileVideo);
    w.win.dispatch("keydown", { key: "?", target: w.doc.body });
    ok("? opens the list of keys", w.ids.helpPanel.hidden === false);
    w.outbox.length = 0;
    w.win.dispatch("keydown", { key: "Escape", target: w.doc.body });
    ok("Esc closes a popover first -- and only that", w.ids.helpPanel.hidden === true && countType(w, "translatorFullscreen") === 0);
    const keys = [...html.matchAll(/<kbd>([^<]+)<\/kbd>/g)].map((m) => m[1]);
    ok("the help names every key the player answers",
       ["Space", "← →", "↑ ↓", "M", "N", "P", "F", "S", "[ ]", "G H", "L", "?"].every((k) => keys.indexOf(k) >= 0), keys);
    w.outbox.length = 0;
    w.win.dispatch("keydown", { key: "n", target: w.ids.ccFont });
    w.win.dispatch("keydown", { key: " ", target: w.ids.tVol });
    ok("a key typed in a field (a select, a slider) is the field's", countType(w, "playerQueue") === 0 && w.ids.fileVideo.paused === true);
    w.win.dispatch("keydown", { key: "n", ctrlKey: true, target: w.doc.body });
    ok("...and Ctrl+key is never the player's", countType(w, "playerQueue") === 0);
  }

  // ---- WE-10 -------------------------------------------------------------------------------------
  section("WE-10 -- full screen: the transport and the cursor go after 2.5 s still, while playing");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, ranges: [[0, 600000]] }));
    metaOk(w.ids.fileVideo);
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.ids.tPlay.click();
    w.pumpTimers(2600);
    ok("full screen + playing + still: the stage goes idle (the CSS hides the transport and the cursor)",
       w.ids.fileStage.classList.contains("is-idle")
       && /body\.is-full \.file-stage\.is-idle \.transport \{ opacity: 0\.01; pointer-events: none; \}/.test(html)
       && /body\.is-full \.file-stage\.is-idle \{ cursor: none; \}/.test(html));
    w.ids.fileStage.dispatch("pointermove", {});
    ok("...a move brings them back", !w.ids.fileStage.classList.contains("is-idle"));
    // User, 11/10: "en plein écran, quand on bouge la souris vers le bas de l'écran, il faut faire apparaître la ligne de
    // lecture". ANY move in the window (not only over the stage) brings it back, and it STAYS while the pointer rests in
    // the bottom band where the bar lives; the arrows' jumps show it too.
    w.pumpTimers(2600);
    const idleAgain = w.ids.fileStage.classList.contains("is-idle");
    w.dispatchDoc("pointermove", { clientX: 600, clientY: 10 });
    const backFromDoc = !w.ids.fileStage.classList.contains("is-idle");
    w.dispatchDoc("pointermove", { clientX: 600, clientY: (w.win.innerHeight || 560) - 40 });
    w.pumpTimers(6000);
    const staysInBand = !w.ids.fileStage.classList.contains("is-idle");
    w.dispatchDoc("pointermove", { clientX: 600, clientY: 20 });
    w.pumpTimers(2600);
    const goesOutside = w.ids.fileStage.classList.contains("is-idle");
    ok("full screen: a move ANYWHERE in the window brings the bar back; it stays while the pointer rests at the bottom, and goes 2.5 s after it leaves",
       idleAgain && backFromDoc && staysInBand && goesOutside, [idleAgain, backFromDoc, staysInBand, goesOutside]);
    w.pumpTimers(2600);
    const idleBeforeKey = w.ids.fileStage.classList.contains("is-idle");
    w.win.dispatch("keydown", { key: "ArrowRight", target: w.doc.body, preventDefault() {} });
    ok("...an arrow jump shows it (where did it land?)", idleBeforeKey && !w.ids.fileStage.classList.contains("is-idle"),
       [idleBeforeKey, w.ids.fileStage.classList.contains("is-idle")]);
    ok("...and in full screen idle the bar is 1 % opaque, never 0 (user 11/10: at 0 the small subtitles vanished with it on his GPU)",
       /body\.is-full \.file-stage\.is-idle \.transport \{ opacity: 0\.01; pointer-events: none; \}/.test(html));
    w.ids.tPlay.click();
    w.pumpTimers(3000);
    ok("...and while paused they stay", !w.ids.fileStage.classList.contains("is-idle"));
    w.send({ type: "translatorWindow", shown: true, fullscreen: false });
    ok("main.cpp says it left full screen: the page's layout follows", !w.doc.body.classList.contains("is-full"));
    ok("the window's state carries `fullscreen` (TrWindowState.h): a page reloaded for a new folder is told it again",
       /kv\(L"fullscreen", fullscreen\)/.test(winStateSrc));
  }

  // ---- WE-11 -------------------------------------------------------------------------------------
  section("WE-11 -- the window's shapes: 420 x 320 (a drawer), 760 x 560, 1280 x 800 (docked)");
  {
    const n = runTranslatorWindow({ width: 420, height: 320 });
    ok("420 x 320: is-narrow -- the transport keeps prev/play/next/mute/CC/panel/full; the rest moves into ⋯",
       n.doc.body.classList.contains("is-narrow") && !n.doc.body.classList.contains("is-wide")
       && /body\.is-narrow \.t-opt, body\.is-narrow \.t-vol \{ display: none; \}/.test(html)
       // (WG re-point: ⋯ is in every File-mode window now -- it holds "Make AudioPB the default player…" -- and the
       //  narrow-only items are added by fillMore when narrow, see below and WG-6)
       && !/body:not\(\.is-narrow\) #tMore \{ display: none; \}/.test(html));
    n.send(fileMsg({ jobId: 2, phase: "done", complete: true }));
    n.ids.tMore.click();
    ok("...⋯ offers the speed, A-B, repeat, shuffle, the keys -- and the strip's buttons it has no room for (Re-analyse here); then (WG) the default player and the theme",
       n.ids.morePanel.hidden === false
       // (U3 re-point: + "Models: …" -- a narrow strip keeps both action slots in words, the models move here)
       && n.ids.morePanel.children.map((b) => b.textContent.split(/[ ·:]/)[0]).join(",") === "Speed,A-B,Repeat,Shuffle,Keyboard,Re-analyse,Models,Make,Light"
       && /body\.is-narrow \.file-strip-btns \{ display: none; \}/.test(html) && /body\.is-narrow \.file-strip-row \{ flex-wrap: nowrap; \}/.test(html),
       n.ids.morePanel.children.map((b) => b.textContent));
    ok("...there the panel is a DRAWER, closed until ☰ (open, it hid the transport and the subtitles: we_e2e N1)",
       n.ids.filePanel.classList.contains("is-closed") && !n.ids.tPanel.classList.contains("is-on"));
    n.ids.tPanel.click();
    ok("...☰ opens it over the lower stage", !n.ids.filePanel.classList.contains("is-closed")
       && /body\.is-narrow \.file-panel \{\s*position: absolute;[^}]*z-index: 30;/.test(html));
    n.ids.fileView.dispatch("click", { target: n.ids.fileVideo });
    ok("...a click on the picture closes it -- and does not ALSO start the playback",
       n.ids.filePanel.classList.contains("is-closed") && n.ids.fileVideo.paused === true);
    ok("...the docked panel's remembered state is not the drawer's",
       // WG: the docked panel is CLOSED by default now (key audiopb.tr.panel2): opening the drawer leaves it closed
       !!n.lsStore["audiopb.tr.panel2"] && JSON.parse(n.lsStore["audiopb.tr.panel2"]).open === false);
    ok("the narrow bar keeps the pair and the window's buttons; the engine's pill only when something is wrong",
       n.doc.body.classList.contains("in-file") && /body\.is-narrow #state:not\(\.is-error\) \{ display: none; \}/.test(html));
    ok("the subtitle and queue lists are their rows' offset parent (the follow-the-playhead scroll measures from the LIST: we_e2e A1)",
       /\.file-list \{\s*position: relative;/.test(html) && /\.queue-list \{ position: relative;/.test(html));
    const m = runTranslatorWindow();
    ok("760 x 560 (the default): neither", !m.doc.body.classList.contains("is-narrow") && !m.doc.body.classList.contains("is-wide"));
    const wd = runTranslatorWindow({ width: 1280, height: 800 });
    ok("1280 x 800: is-wide -- the panel docks on the right", wd.doc.body.classList.contains("is-wide")
       && /body\.is-wide \.file-player \{ flex-direction: row; \}/.test(html));
    wd.win.innerWidth = 500; wd.win.dispatch("resize");
    ok("...and a resize decides again", wd.doc.body.classList.contains("is-narrow") && !wd.doc.body.classList.contains("is-wide"));
    const wp = runTranslatorWindow({ ls: { "audiopb.tr.panel2": JSON.stringify({ tab: "queue", open: false }) } });
    ok("the panel's tab and whether it is open are remembered", wp.ids.filePanel.classList.contains("is-closed")
       && wp.ids.tabQueue.classList.contains("is-on") && wp.ids.queuePane.hidden === false && wp.ids.fileList.hidden === true);
    wp.ids.tPanel.click();
    ok("...☰ opens it again (and says so to the next opening)", !wp.ids.filePanel.classList.contains("is-closed")
       && JSON.parse(wp.lsStore["audiopb.tr.panel2"]).open === true);
  }

  // ---- WE-12 -------------------------------------------------------------------------------------
  section("WE-12 -- resume: \"Resumed at 12:31 · Start over\"");
  {
    const w = runTranslatorWindow();
    w.send(fileMsg({ jobId: 2, phase: "done", complete: true, startMs: 751000, resumed: true, autoplay: true,
                     totalMs: 3000000, ranges: [[0, 3000000]] }));
    const v = w.ids.fileVideo;
    ok("a file picked up where it was left: the toast says where", w.ids.resumeToast.hidden === false
       && w.ids.resumeText.textContent === "Resumed at 12:31", w.ids.resumeText.textContent);
    v._fire("loadedmetadata", { readyState: 1, videoWidth: 1280, videoHeight: 720, duration: 3000 });
    ok("...the element is put there as soon as it can be, and plays (complete: nothing to wait for)",
       v.currentTime === 751 && v.paused === false, v.currentTime);
    w.ids.resumeStartOver.click();
    ok("Start over: from the top, the toast gone", v.currentTime < 0.01 && w.ids.resumeToast.hidden === true);
    const w2 = runTranslatorWindow();
    w2.send(fileMsg({ jobId: 2, phase: "done", complete: true, startMs: 751000, resumed: true }));
    w2.pumpTimers(8100);
    ok("the toast goes by itself after 8 s", w2.ids.resumeToast.hidden === true);
  }

  // ---- WE-13 -------------------------------------------------------------------------------------
  section("WE-13 -- a page RELOADED (a new folder, D5; a reopened window) gets everything back on `ready`");
  {
    const w = runTranslatorWindow();
    ok("the page says `ready` with the tab it is on (then update.check: 03/11, the update banner)",
       JSON.stringify(w.outbox) === JSON.stringify([{ type: "ready", view: "file" }, { type: "update.check" }]), w.outbox);
    // the bridge's replay, in its order: the window, the state (seen before: fresh false), the cues, the subtitle file, the queue
    w.send({ type: "translatorWindow", shown: true, fullscreen: true });
    w.send(fileMsg({ jobId: 9, seq: 3, itemId: 12, phase: "analyzing", fresh: false, startMs: 61000, doneMs: 90000,
                     ranges: [[0, 90000]], extSubId: 40 }));
    w.send({ type: "translatorFileCues", jobId: 9, cues: [{ idx: 0, s: 60000, e: 62000, lang: "en", dstLang: "zh", pass: false, src: "x", dst: "叉" }] });
    w.send({ type: "playerSubs", id: 40, name: "film.fr.srt", enc: 65001, encName: "UTF-8", forced: false, truncated: false,
             part: 0, parts: 1, cues: [{ s: 60000, e: 62000, r: [["ligne", 0]] }] });
    w.send(qMsg([qItem(11, "E01.mkv"), qItem(12, "E02.mkv")], { cur: 12, loaded: true }));
    metaOk(w.ids.fileVideo);
    w.ids.fileVideo._seek(61);
    ok("...full screen again, the File tab, the queue (its current row), the position, the subtitle FILE on the picture",
       w.doc.body.classList.contains("is-full") && w.ids.filePane.hidden === false && w.ids.queueList.children.length === 2
       && w.ids.queueList.children[1].classList.contains("is-cur") && ovLines(w).length === 1 && ovLines(w)[0].text === "ligne",
       ovLines(w));
    ok("...the playhead where it was (startMs = the last position for a replay), not the top",
       w.ids.fileVideo.currentTime === 61, w.ids.fileVideo.currentTime);
    ok("...and a replay is not news: nothing starts by itself", w.ids.fileVideo.paused === true && w.ids.fileReady.hidden === true);
  }

  // ---- WE-14 -------------------------------------------------------------------------------------
  section("WE-14 -- parity: the page's twins and the bridge's, every message both ways, the player's wiring");
  {
    const miss = vectorMisses(script);
    ok("we_vectors.txt through the PAGE's own focusTarget/leadAt/readyAt/coveredToEnd: the C++ answers, row for row ("
       + WE_VECTORS.length + " rows; player_smoke.cpp runs the same file)",
       WE_VECTORS.length >= 16 && miss.length === 0 && /const READY_LEAD_MS = 15000;/.test(script)
       && /constexpr uint64_t kReadyLeadMs = 15000;/.test(playerSrc), miss);
    const smoke = path.join(P.SUV, "tests", "cpp", "player_smoke.cpp");
    ok("...and the C++ side reads the same file (" + path.relative(P.SUV, smoke) + ")", /we_vectors\.txt/.test(fs.readFileSync(smoke, "utf8")));
    const sr = P.cpp("ui/SubtitleReader.h");
    const cTable = [...((/static const EncName k\[\] = \{([\s\S]*?)\{ 0, nullptr \} \};/.exec(sr) || ["", ""])[1]
      .matchAll(/\{ (\d+), "([^"]+)" \}/g))].map((m) => m[1] + "=" + m[2]);
    const jTable = [...((/const ENC_TABLE = \[([\s\S]*?)\];/.exec(script) || ["", ""])[1]
      .matchAll(/\[(\d+), "([^"]+)"\]/g))].map((m) => m[1] + "=" + m[2]);
    ok("ENC_TABLE (the CC panel's Encoding list) is SubtitleReader.h's encodingTable(), in order, names included",
       cTable.length === 19 && cTable.join("|") === jTable.join("|"), { cpp: cTable, page: jTable });
    const cSub = ((/inline bool isSubtitleExt[\s\S]*?return ([^;]+);/.exec(sr) || ["", ""])[1].match(/L"([a-z]+)"/g) || [])
      .map((x) => x.slice(2, -1)).sort().join(",");
    const jSub = ((/const SUB_EXTS = \[([^\]]*)\];/.exec(script) || ["", ""])[1].match(/"([a-z]+)"/g) || []).map((x) => x.slice(1, -1)).sort().join(",");
    ok("SUB_EXTS (what a drop takes as subtitles) is isSubtitleExt()", cSub === "ass,srt,ssa,vtt" && cSub === jSub, { cpp: cSub, page: jSub });
    const sentP = [...new Set([...pageOnly.matchAll(/type:\s*"(player\w+|translatorFullscreen)"/g)].map((m) => m[1]))];
    const handledP = (t) => new RegExp('type == L"' + t + '"').test(bridgeSrc);
    ok("every player* message the page SENDS is handled by the app (Open, Add, Queue, Set, Pos, SubLoad, SubGet, translatorFullscreen...)",
       sentP.length >= 8 && sentP.every(handledP), { sent: sentP, unhandled: sentP.filter((t) => !handledP(t)) });
    const emittedP = [...new Set([...(bridgeSrc + playerSrc + winStateSrc).matchAll(/L"type",\s*L"(player\w+|translatorFile\w*|translatorWindow)"/g)].map((m) => m[1]))];
    ok("every player*/translatorFile*/translatorWindow message the app or the core EMITS is consumed by the page",
       emittedP.length >= 10 && emittedP.every((t) => pageOnly.indexOf('msg.type === "' + t + '"') >= 0),
       { emitted: emittedP, unconsumed: emittedP.filter((t) => pageOnly.indexOf('msg.type === "' + t + '"') < 0) });
    // (the app's half -- paths only through ICoreWebView2File, its dialogs, ids for the queue and the subtitle files; the
    // folder scans on a worker; FFOCUS through the core's rule; its own files -- is the app's to prove: tests/cpp, tests/e2e)
    ok("no PATH ever leaves the page: no message it sends carries a path, a file or a folder name (drops: File objects; the queue "
       + "and the subtitle files: ids)",
       !/send\(\{[^}]*\b(path|file|folder|dir)\s*:/.test(pageOnly) && !/postMessageWithAdditionalObjects\(\{[^}]*\bpath\s*:/.test(pageOnly));
  }

  // ---- WE: the design mock ---------------------------------------------------------------------------
  section("WE -- the design mock (a plain browser, no bridge): the player can be watched without the app");
  {
    const w = runTranslatorWindow({ noWebView: true });
    w.dispatchDoc("drop", { dataTransfer: { files: [{ name: "clip.mp4", type: "video/mp4" }, { name: "talk.mp3", type: "audio/mpeg" },
                                                      { name: "notes.txt", type: "text/plain" }] } });
    ok("a drop in a plain browser goes to the MOCK bridge: a queue of two (not the .txt), the first loaded from an object URL -- "
       + "and NOT translated by itself (U1)",
       w.ids.filePlayer.hidden === false && w.ids.queueList.children.length === 2 && w.ids.fileVideo.src === "blob:mock/clip.mp4"
       && /^Not translated yet/.test(w.ids.fileStatus.textContent), [w.ids.fileStatus.textContent, w.ids.fileVideo.src]);
    metaOk(w.ids.fileVideo);
    ok("...a plain player until asked: it plays at once", w.ids.fileVideo.paused === false);
    w.ids.fileTranslate.click();
    w.pumpTimers(1300);
    ok("...Translate: its 'analysis' lays subtitles AHEAD of the playhead -- a green range, cues in the list, a badge",
       w.ids.seekRanges.children.length === 1 && rows(w).length >= 5 && /⟳/.test(w.ids.queueList.children[0].children[3].textContent),
       [w.ids.seekRanges.children.length, rows(w).length, w.ids.fileVideo.paused]);
    w.ids.tNext.click();
    ok("...Next plays the mock queue's next item (a sound: the <audio>)", w.ids.fileAudio.src === "blob:mock/talk.mp3", w.ids.fileAudio.src);
  }

  // ---- WE review (REVIEW_WE.md): the defects an adversarial review found, as regressions ------------------
  // The reviewer's probes (we_review/probe_page*.js) re-run as checks: C5 (Off -> On re-posts the same file),
  // C6 (a closed window plays by itself), C1 (Live stays Live: the page's half -- the bridge's is in
  // player_smoke's WindowState checks), and the bridge's wiring for C1, C8, P2, P3, P6, P8 (source checks).
  const regenStaysPaused = (w) => {          // C5: the user PAUSED at 5:00; translation Off, then On
    const v = w.ids.fileVideo;
    w.send(fileMsg({ jobId: 1, seq: 1, phase: "analyzing", autoplay: true, resumed: true, startMs: 20000, doneMs: 400000, ranges: [[0, 400000]] }));
    metaOk(v);
    v.currentTime = 300; w.pumpRaf(1);
    w.ids.tPlay.click();                                  // the user pauses
    w.ids.resumeToast.hidden = true;                      // the first toast is long gone
    w.send(fileMsg({ jobId: 1, seq: 1, phase: "off", translate: false, autoplay: true, resumed: true, startMs: 300000,
                     fresh: false, doneMs: 400000, ranges: [[0, 400000]] }));
    const calls = v.playCalls;
    // the regen post as the bridge sends it now (startMs = the playhead, no autoplay, no resume) -- and as it
    // was sent before the fix (autoplay, resumed, the ORIGINAL start): the page must hold either way
    w.send(fileMsg({ jobId: 3, seq: 1, phase: "loading", translate: true, autoplay: true, resumed: true, startMs: 20000,
                     doneMs: 400000, ranges: [[0, 400000]] }));
    return v.paused === true && v.playCalls === calls && w.ids.resumeToast.hidden === true
        && Math.abs(v.currentTime - 300) < 1e-6 && w.ids.fileReady.hidden === true;
  };
  const closedNeverPlays = (w) => {          // C6: a folder scan lands after the window was closed
    const v = w.ids.fileVideo;
    w.send({ type: "translatorWindow", shown: false });
    w.send(fileMsg({ jobId: 2, seq: 2, itemId: 2, phase: "done", complete: true, autoplay: true, ranges: [[0, 600000]] }));
    metaOk(v);
    return v.playCalls === 0 && v.paused === true;
  };
  section("WE review -- regressions: C5 (a re-post of the same file), C6 (a closed window), C1 (the one-shot `file`)");
  {
    ok("C5: translation Off -> On on a film the user PAUSED: it stays paused where it is, no \"Resumed at\" toast, no card",
       regenStaysPaused(runTranslatorWindow()));
    ok("C6: a file opened to PLAY that lands while the window is CLOSED does not play there (sound from nowhere)",
       closedNeverPlays(runTranslatorWindow()));
    const w = runTranslatorWindow();
    closedNeverPlays(w);
    w.send({ type: "translatorWindow", shown: true });
    ok("C6: ...it starts when the window is shown again (still armed, play when ready)",
       w.ids.fileVideo.playCalls === 1 && w.ids.fileVideo.paused === false);
    // C1 (641f1aa: the tray's one-shot `file` showed the File tab once, then Live stayed Live): SuVtitle's page IS the File
    // view -- a translatorWindow{file:true} changes nothing and sends nothing; hide / show / full screen are followed
    const wl = runTranslatorWindow();
    wl.outbox.length = 0;
    [{ shown: true, fullscreen: false, file: true }, { shown: false, fullscreen: false }, { shown: true, fullscreen: true },
     { shown: true, fullscreen: false }].forEach((s) => wl.send(Object.assign({ type: "translatorWindow" }, s)));
    ok("C1: translatorWindow{file:true} (AudioPB's \"show the File tab\") changes nothing here; hide / show / full screen are followed",
       wl.ids.filePane.hidden === false && wl.outbox.length === 0 && !wl.doc.body.classList.contains("is-full"), wl.outbox);
  }

  // ---- WAVE WG (the user's first real test, 11/10): its sections and sentinels live in test_wg.js -------------
  require("./test_wg.js")({ ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, ovLines, rows, last, countType,
                            qMsg, qItem, html, script, pageOnly, bridgeSrc, playerSrc, mainSrcWE, P, WEB, fs, path, showVia });

  // ---- WAVE WH (the film's own tracks, 11/10): the page's side -- its sections and sentinels live in test_wh.js ------------
  require("./test_wh.js")({ ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, ovLines, rows, last, countType, html, P, fs, path,
                            showVia, MARKUP_SINK, codeOnly });

  // ---- REVIEW RV-4 (23/10): the page's fix round -- its checks and sentinels live in test_rv4.js ----------------------------
  require("./test_rv4.js")({ ok, section, runTranslatorWindow, fileMsg, metaOk, last, countType, html, script, P, MARKUP_SINK, codeOnly });

  // ---- P-CTX (25/10, the user on 1.0.0): the right-click menu + full screen's idle LOGIC -- checks and sentinels in test_pctx.js ----
  require("./test_pctx.js")({ ok, section, runTranslatorWindow, fileMsg, metaOk, html, script });

  // ---- THE FREEZE WATCH (25/10, his full-screen report, not reproduced): the page reports its own freezes -- test_freeze.js ----
  require("./test_freeze.js")({ ok, section, runTranslatorWindow, fileMsg, metaOk, script });

  // ---- 1.1.0 THE NATIVE VIDEO ENGINE (26/10, N2): mpv under the page -- the media backend, its clock, its fallbacks -- test_native.js ----
  require("./test_native.js")({ ok, section, runTranslatorWindow, fileMsg, metaOk, script, html, P });

  // ---- U1 (02/11/2026, the user's own requests): Translate is a button, Translate all, Translate Live, the pair beside
  //      Translate, one model, "日本語 (Japanese)", the lockup -- checks and sentinels in test_u1.js ----
  require("./test_u1.js")({ ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, ovLines, last, countType, qMsg, qItem, html, script,
                            MARKUP_SINK, codeOnly, bridgeSrc, playerSrc });

  // ---- U2 (02/11/2026, "Import model…"): the user's own models -- the models button and its menu, no path from the page, the
  //      node beside the pair, "Model changed since this analysis" -- checks and sentinels in test_u2.js ----
  require("./test_u2.js")({ ok, section, runTranslatorWindow, fileMsg, cueMsg, metaOk, html, script, MARKUP_SINK, codeOnly, bridgeSrc });
  // ---- U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md): the Transcribe slot, Transcribe all and the second badge, the
  //      Transcript tab (a click plays, a click on a name renames), the Export menu's two Word files -- checks and sentinels in test_u3.js
  require("./test_u3.js")({ ok, section, runTranslatorWindow, fileMsg, metaOk, qMsg, qItem, html, script, MARKUP_SINK, codeOnly, bridgeSrc });

  // ---- THE UPDATE BANNER (03/11/2026): the banner, the Updates dialog, the opt-out -- checks and sentinels in
  //      test_upd.js (also runnable alone: node tests\web\test_upd.js)
  require("./test_upd.js")({ ok, section, runTranslatorWindow, html, script, bridgeSrc, fileMsg, metaOk });

  // ---- 7. MUTATION SENTINELS: each guard removed must be CAUGHT ---------------------------------
  section("WC + WE -- mutation sentinels (index.html with one guard removed each)");
  {
    // A mutation that changes nothing would "test" the original page and could never be caught,
    // and a load that THROWS for that reason must not count as a catch either: so applicability is
    // checked first, on the source itself, and a mutant that does not apply is a FAILED sentinel.
    // A mutant that does not even LOAD (a syntax slip in the mutation) tests nothing either.
    const judge = (name, applies, load, probe) => {
      if (!applies) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
      let page;
      try { page = load(); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
      let healthy = true;
      // WE: caught means the probe RETURNED false. A probe that THROWS on a mutant proves only that the
      // probe broke -- it used to count as a catch; it is now named as what it is (none does today).
      let threw = "";
      try { healthy = !!probe(page); } catch (e) { threw = String((e && e.message) || e); }
      if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
      ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
    };
    const caught = (name, mutate, probe) =>
      judge(name, mutate(html) !== html, () => runTranslatorWindow({ mutate, name }), probe);
    const fresh = (w) => {
      w.send(fileMsg({ jobId: 5, phase: "analyzing", doneMs: 1000 }));
      metaOk(w.ids.fileVideo);
    };
    // the probes are the REAL checks' conditions, run on a fresh window
    const textStays = (w) => {             // v1's escCue sentinel, in D4's form: a cue's text never becomes markup
      fresh(w);
      w.send(cueMsg(5, 0, 0, 1000, "en", "zh", "A<B", "C&D <b>x</b>"));
      w.ids.fileVideo._seek(0.5);
      const L = ovLines(w);
      return L.length === 2 && L[0].text === "C&D <b>x</b>" && L[0].kids.join() === "#TEXT." && L[1].text === "A<B";
    };
    const staleCue = (w) => { fresh(w); w.send(cueMsg(4, 0, 0, 1000, "en", "zh", "old", "old")); return rows(w).length === 0; };
    const staleFile = (w) => {
      fresh(w);
      w.send(fileMsg({ jobId: 4, phase: "error", err: "older" }));
      return w.ids.fileStatus.textContent.indexOf("older") < 0;
    };
    // sanity: every probe PASSES on the real pages
    const probes = { textStays, staleCue, staleFile, winYue, winNonCode, extTextStaysText, vectorsHold, readyWaits15,
                     keepAheadHysteresis, queueById, dropOnQueue, noAutoplayWithoutAsking, closedStaysStill,
                     replayNotNews, noUrlAsksSound, txtDropRefused, closeStopsSwitch, listHold,
                     regenStaysPaused, closedNeverPlays };
    const sick = Object.keys(probes).filter((k) => { try { return !probes[k](runTranslatorWindow()); } catch (e) { return true; } });
    ok("(the window's probes pass on the real page)", sick.length === 0, sick);
    // the title face is a SYSTEM font (Bahnschrift): a font rule / file put back, or the stack changed, must fail the page rule
    {
      const noFontRule = (h) => !/@font-face\s*\{/.test(h) && !/url\(\s*["']?[^)"']*\.(woff2?|ttf|otf|eot)\b/i.test(h);
      const stackKept = (h) => /--display: "Bahnschrift", "Segoe UI Variable Display", "Segoe UI", system-ui, sans-serif;/.test(h)
                            && /\.brand-name \{ font-family: var\(--display\); font-size: 24px; font-weight: 600; font-stretch: normal;/.test(h);
      const withRule = html.replace("<style>", "<style>\n    @font-face { font-family: \"X\"; src: url(\"x.woff2\") format(\"woff2\"); }");
      judge("a @font-face rule is put back (a font file would ship again)", withRule !== html, () => withRule, noFontRule);
      const otherStack = html.replace("--display: \"Bahnschrift\", ", "--display: \"Syne\", ");
      judge("the display token stops naming Bahnschrift first", otherStack !== html, () => otherStack, stackKept);
      const lighter = html.replace("font-weight: 600; font-stretch: normal;", "font-weight: 700; font-stretch: normal;");
      judge("the lockup's weight drifts from 600", lighter !== html, () => lighter, stackKept);
      ok("(the system-face rules hold on the real page)", noFontRule(html) && stackKept(html));
    }
    caught("a cue's text becomes markup (v1's escCue sentinel, in D4's form)",
      (s) => s.replace("if (!r.flags) { span.appendChild(document.createTextNode(r.text)); return; }",
                       "if (!r.flags) { const b = document.createElement(\"b\"); b.textContent = r.text.replace(/<\\/?b>/g, \"\"); span.appendChild(b); return; }"),
      textStays);
    caught("cue of another generation accepted",
      (s) => s.replace("if (!F.jobId || jobId !== F.jobId) return false;", "if (!F.jobId) return false;"),
      staleCue);
    caught("older translatorFile accepted",
      (s) => s.replace("if (j < F.jobId) return;", ""),
      staleFile);
    const prefixBack = (s) => s.replace('return /^[a-z]{2}$/.test(s) ? s : "";', "return s.slice(0, 2);");
    const yueRowGone = (s) => s.replace('"cmn": "zh", "yue": "yue",', '"cmn": "zh",');
    // review of WC (2026-10-10): each window fix removed in turn
    caught("[K] the replay of a shown session is news again (fresh ignored): the opening replayed",
      (s) => s.replace('const news = fresh && msg.fresh === true && F.phase !== "idle";',
                       'const news = fresh && F.phase !== "idle";'),
      replayNotNews);
    caught("[3] no second door for a file with no URL",
      (s) => s.replace(/if \(!r && F\.phase !== "idle" && !F\.url && !F\.proxyUrl && !F\.proxyState && playFailedFor !== F\.seq\)\r?\n\s*reportPlayFailure\(\);/, "/* door removed */"),
      noUrlAsksSound);
    caught("[4] drops judged by their MIME type again (an empty type let anything through)",
      (s) => s.replace('const isMediaFile = (f) => MEDIA_EXTS.indexOf(extOf(String((f && f.name) || ""))) >= 0;',
                       'const isMediaFile = (f) => { const t = String((f && f.type) || ""); return !t || t.indexOf("audio/") === 0 || t.indexOf("video/") === 0; };'),
      txtDropRefused);
    caught("[8] a pending play survives the window closing",
      (s) => s.replace(/pendingPlay = false;\r?\n(\s*pendingSeek = -1;\r?\n\s*armed = false;)/, "$1"),
      closeStopsSwitch);
    caught("the list's \"next scroll is ours\" flag is back",
      (s) => s.replace("if (listOwnTop >= 0 && Math.abs((Number(el.fileList.scrollTop) || 0) - listOwnTop) < 2) return;",
                       "if (listOwnTop >= 0) { listOwnTop = -1; return; }"),
      listHold);
    caught("page: the two-letter prefix fallback is back", prefixBack, winNonCode);
    caught("page: the yue row is dropped from the aliases", yueRowGone, winYue);
    // WE (WE-15): the player's guards, removed in turn
    caught("WE: a subtitle FILE's text read as markup (an <i> in the TEXT obeyed)",
      (s) => s.replace("if (p) cur.runs.push({ text: p, flags: Number(run[1]) || 0 });",
                       "if (p) cur.runs.push({ text: p.replace(/<\\/?i>/g, \"\"), flags: (Number(run[1]) || 0) | (/<i>/.test(p) ? 1 : 0) });"),
      extTextStaysText);
    caught("WE: the lead forgets the playhead (leadAt = the target itself)",
      (s) => s.replace("function leadAt(r, p) { return focusTarget(r, p) - p; }", "function leadAt(r, p) { return focusTarget(r, p); }"),
      vectorsHold);
    caught("WE: play when ready starts at 1.5 s of subtitles instead of 15",
      (s) => s.replace("const READY_LEAD_MS = 15000;", "const READY_LEAD_MS = 1500;"), readyWaits15);
    caught("WE: keep-ahead without its hysteresis (plays again at 2 s, not 8)",
      (s) => s.replace("leadAt(F.ranges, t) >= RESUME_LEAD_MS", "leadAt(F.ranges, t) >= STALL_MS"), keepAheadHysteresis);
    caught("WE: a queue row played by its POSITION, not its id",
      (s) => s.replace('row.addEventListener("dblclick", () => send({ type: "playerQueue", op: "play", id: it.id }));',
                       'row.addEventListener("dblclick", () => send({ type: "playerQueue", op: "play", id: i + 1 }));'),
      queueById);
    caught("WE: a drop on the queue PLAYS (its target ignored)",
      (s) => s.replace('target: queue ? "queue" : "play"', 'target: "play"'), dropOnQueue);
    caught("WE: a file plays by itself without being opened to play (autoplay ignored)",
      (s) => s.replace("if (F.autoplay) { armed = true; armedFrom = F.startMs; }", "{ armed = true; armedFrom = F.startMs; }"),
      noAutoplayWithoutAsking);
    caught("WE: a window closed while catching up plays again when the subtitles arrive",
      (s) => s.replace(/(pendingSeek = -1;\r?\n\s*armed = false;\r?\n)\s*stalled = false;\r?\n/, "$1"),
      closedStaysStill);
    // the review of WE (REVIEW_WE.md): the page's halves of C5 and C6
    caught("WE review C5: a re-post of the SAME file is opened again (play when ready re-armed, the toast)",
      (s) => s.replace("if (news && newItem) {", "if (news) {"), regenStaysPaused);
    caught("WE review C6: a closed window plays what lands in it",
      (s) => s.replace("if (!windowShown) return;", ""), closedNeverPlays);
  }
}

console.log("\n" + (fail === 0 ? "ALL " + pass + " CHECKS PASSED" : pass + " passed, " + fail + " FAILED"));
if (P.fallbacks.length) console.log("NOTE: read from 641f1aa (not in SuVtitle yet): " + P.fallbacks.join(", "));
process.exit(fail === 0 ? 0 : 1);

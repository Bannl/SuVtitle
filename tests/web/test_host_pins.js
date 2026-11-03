// test_host_pins.js -- the HOST-SIDE SOURCE PINS of AudioPB's 641f1aa harness, re-homed on SuVtitle's app\*.cpp / *.h.
// T-E2E, 2026-10-23 (REVIEW_RV4_Page.md RV4-13).  Standalone node, nothing to install:
//   node tests\web\test_host_pins.js [--no-selftest] [--list]     exit 0 = every pin passes AND every mutant is caught
//   SUV_APP=<folder> node tests\web\test_host_pins.js            reads another copy of app\ (the CRLF / LF proof, see the log)
//
// WHY. AudioPB's test_ui.js did not only run the page: it also READ the C++ of the host (bridge.cpp, TrPlayer.h, main.cpp,
// TrWindowState.h, TranslatorManager.cpp, TrLaunch.h...) and pinned the wiring that no headless test can run -- "no PATH ever
// comes from the page's JSON", "the window state on every `ready`", "C1's order", "no SW_RESTORE of a full-screen window", "no
// drop while a dialog is up", the autoplay environment and its retry... S-WEB dropped them with the Live/desk sections ("the host
// is S-HOST's now") and S-HOST's tests (tests\cpp RUN the core and the pure parts; tests\e2e\real runs the real app) never took
// them over, so a wiring regression of these kinds was pinned nowhere. This file is where they live now.
//
// HOW. Every pin is a regular expression (or a few) over the NORMALISED source (CRLF / lone CR -> LF: a checkout may be either),
// scoped to one function body where it can be (fnBody / between). A pin must PASS on today's sources, and it must be ABLE TO
// FAIL: the self-test (default; --no-selftest skips it) applies to each pin one or more MUTANTS -- the pinned guard undone in the
// source text, the way a careless edit would -- and requires the pin to FAIL on every one of them. Each mutant must apply
// EXACTLY once, or the self-test itself fails (a mutant that no longer applies proves nothing); a pin with no mutant fails too.
// When a pin fails on a legitimate change of the host, EDIT THE PIN WITH THE CHANGE (and its mutant): the regex is the rule.
//
// WHERE THE PINS COME FROM ("from" in --list): "ui" = test_ui.js and "wg" = test_wg.js of the 641f1aa harness, frozen in
// scratchpad\from_audiopb_641f1aa\ui_test\ (WC 4c, WE-4, WE-10, WE-14, the WE review, WG-3, WG-6); "RV4-12 (new)" = M8, which
// ties the product's FNV basis to the we_e2e kit (tests\e2e\we_e2e\check_hosts.js). SuVtitle keeps the internal names (SPLIT_PLAN
// S3); what changed on the way: ui/ -> app/, ONE window (attachTranslatorView -> attach, translatorView_ -> webview_, g_trHwnd ->
// g_hwnd, g_trFullscreen -> g_fullscreen), the app's names (AudioPB -> SuVtitle, SuVtitle-Test registry root, kCopyPlay "SVTP").
// 52 pins: W window/ready/full screen/restore (W1-W8) - S WebView2 environment (S1-S3) - D drops, paths, files the app
// writes (D1-D12) - M mapping, URLs, proxy, FNV (M1-M8) - F the .srt files (F1-F7) - E engine supervisor (E1) - L --play, single
// instance, default player, test instance (L1-L13). + U1 (02/11/2026): nothing analysed unless asked, Translate all / Cancel
// all, nothing of 1.0.x's switch (U1-1..U1-3) -- 55 pins. + U2 (02/11/2026, "Import model..."): the dialog and the header, app.txt
// and the effective choice, Cancel all = the batch only and the models on every FILE (U2-1..U2-3) -- 58 pins. + U3 (02/11/2026,
// the TRANSCRIPTION): the transcript's Word file (the registry, never a clobber, no Documents in a test instance), its Save dialog,
// the five doors (one free text: a speaker's name, bounded, checked by the core) (U3-1..U3-3) -- 61 pins. U3 also re-pointed
// U1-1 / U1-2 / U2-3 (the analysis is wanted by EITHER wish; both batches) and scoped F1's / F2's mutants to the .srt writer
// (the Word file's writer shares its free-or-ours lines). (U2b and FX added theirs since: 68 pins.) + UPD (03/11/2026, THE UPDATE
// BANNER): its doors (update.open reads nothing and opens what the app built: SuVtitle's page), once per start off the UI thread
// and never from a test, what the page is told and what app.txt holds (closed lists), NextaleUpdate's constants / bounds / guard
// (UPD-1..UPD-4); + UPD-5 (the review of 03/11: S5 a skip undone -- update.unskip, the skipped version still named; N3 a repeated
// update.open re-answered, nothing opened twice); + UPD-6 (every address written in app\, the page included, is a known one --
// FILES gains `page`); with FX2's FX2-1 -- 75 pins.
//
// DROPPED -- a pin of the 641f1aa harness that tested something SuVtitle no longer has (one line each):
//  - ui P6 "a message from the MAIN view is never taken for the window's (trFromWindow_ cleared around it)": SuVtitle has ONE
//    view, no trFromWindow_, no main view -- nothing to confuse. (P1's replacement, "postMain is a no-op", is D12.)
//  - ui C1's `file`-flag one-shot as a BEHAVIOUR ("AudioPB Player..." shows the File tab ONCE, Live stays Live): the one-shot is
//    kept in TrWindowState.h for parity and its consumption is pinned (W2), but SuVtitle has no Live tab to be dragged back from
//    (its page boots on ready{view:"file"} and ignores `file`).
//  - ui WE-14 "every message the page sends is handled / every message the app emits is consumed" and "no PATH leaves the PAGE":
//    already pinned page-side in tests\web\test_ui.js (WE-14, re-pointed by S-WEB); the host side of "no path" is D1 here.
//  - ui WC "main.cpp: the window itself is untouched: saveTranslatorWinPos": AudioPB's translator.txt WINPOS is gone; the position
//    is app.txt's (AppConfig.h, saveWinPos) -- W6 pins the new rule with the 420 x 320 minimum.
//  - ui P3 "the player's pair is seeded from translator.txt (READ only)" and WE-14 "persistence: translator.txt is only READ":
//    SuVtitle's pair is its own (S11: page -> translatorCtl -> the ENGINE persists engine.txt; the app never writes it) -> D10.
//  - ui WG-6 "the Bridge builds the VST3 catalogue / spawns the rescan only in startup()" and the TEST INSTANCE pins on "no fabric,
//    no VST3 rescan, no licence/network, no tray icon, no global hotkey": none of those exist in SuVtitle (S5/S6); what remains of
//    them -- startup() only from attach / showPlayer / playPaths, the isolation (exit 3), the suffixed mutex and class, no
//    foreground, no shell broadcast -- is L7-L9.
//  - wg WG-6 the INSTALLER pins (.iss: Open-with / Capabilities / RegisteredApplications / --unregister-player at uninstall):
//    SuVtitle has no installer yet (installer\ is empty: the director's later wave) -- re-pin them when installer\*.iss exists.
//  - wg WG-8 (the desk: footer button, mainFileDrop, handleMainFileDrop, put_AllowExternalDrop under #ifdef PATCHBUS_TRANSLATOR).
//  - already ported by S-WEB into tests\web\test_ui.js / test_wg.js, so not repeated: the contract names, DocxWriter.h's texts,
//    the four language-alias tables, knownLangCodes() = TR_LANGS, the ENGINE record (WG-7), TrLaunch's subtitle extensions =
//    SubtitleReader's, we_vectors.txt's C++ side, ENC_TABLE / SUB_EXTS / MEDIA_EXTS twins.
"use strict";
const fs = require("fs");
const path = require("path");

const APP = process.env.SUV_APP ? path.resolve(process.env.SUV_APP) : path.resolve(__dirname, "..", "..", "app");
const FILES = {
  bridge: "bridge.cpp", bridgeH: "bridge.h", main: "main.cpp", player: "TrPlayer.h", winState: "TrWindowState.h", store: "TrFileStore.h",
  launch: "TrLaunch.h", assoc: "TrAssoc.h", mgr: "TranslatorManager.cpp", cmake: "CMakeLists.txt",
  appcfg: "AppConfig.h",   // (U2: app.txt's MODEL / MODELUSE)
  upd: "NextaleUpdate.cpp", updH: "NextaleUpdate.h",   // (03/11/2026: THE UPDATE BANNER)
  page: "web/index.html",                              // (03/11/2026: UPD-6 reads the page's addresses too)
};
// the whole text, line endings normalised (the repo stores LF; core.autocrlf on this machine checks CRLF out)
function readSources() {
  const S = {};
  for (const [k, f] of Object.entries(FILES)) {
    try { S[k] = fs.readFileSync(path.join(APP, f), "utf8").replace(/\r\n?/g, "\n"); } catch { S[k] = null; }
  }
  return S;
}
// the body of the function whose name+"(" first appears in src: from its first "{" to the matching "}" ("" when absent)
function fnBody(src, name) {
  if (src == null) return "";
  const at = src.indexOf(name + "(");
  if (at < 0) return "";
  const open = src.indexOf("{", at);
  if (open < 0) return "";
  let d = 0, i = open;
  for (; i < src.length; ++i) { if (src[i] === "{") d++; else if (src[i] === "}" && --d === 0) break; }
  return src.slice(open, i + 1);
}
// the text from the first occurrence of `from` to the first occurrence of `to` after it ("" when either is absent)
function between(src, from, to) {
  if (src == null) return "";
  const a = src.indexOf(from);
  if (a < 0) return "";
  const b = src.indexOf(to, a + from.length);
  return b < 0 ? "" : src.slice(a, b);
}
const count = (src, re) => (src == null ? 0 : (src.match(re) || []).length);
const codeOnly = (src) => (src == null ? "" : src.replace(/\/\/[^\n]*/g, ""));       // (line comments out: for "never X" pins)
// (U2b) every one of `parts` in src, in that order (each found after the previous one)
const ordered = (src, parts) => {
  let at = 0;
  for (const p of parts) { const i = (src || "").indexOf(p, at); if (i < 0) return false; at = i + p.length; }
  return true;
};

// ---- the pins --------------------------------------------------------------------------------------------------------------
// pin(id, from, name, test, mutants): test(S) -> true | false | [bool, detail]; mutants: [{ file, find, replace, why }]
const PINS = [];
const pin = (id, from, name, test, mutants) => PINS.push({ id, from, name, test, mutants: mutants || [] });

// ================= W: the window -- ready, state, full screen, restore =====================================================
pin("W1", "ui WE-10 + WE review C1", "the bridge says the window's state on EVERY `ready` (a page reloaded for a new folder stays full screen), AFTER marking its page ready -- and before the player's replay",
  (S) => /if \(type == L"ready"\) \{[\s\S]{0,900}trWin_\.ready\(\);\s*postTranslatorWindowState\(\);\s*if \(player_\) \{\s*player_->windowReady\(\);/.test(S.bridge)
      && /j\.obj\(\)\.kv\(L"type", L"translatorWindow"\)\.kv\(L"shown", shown\)\.kv\(L"fullscreen", fullscreen\)/.test(S.winState),   // (no `;`: RV4-7's `topmost` may chain after it)
  [{ file: "bridge", find: /trWin_\.ready\(\);\n(\s*)postTranslatorWindowState\(\);\n/, replace: "trWin_.ready();\n", why: "no window state on `ready`" },
   { file: "bridge", find: /(\s*)trWin_\.ready\(\);\n(\s*)postTranslatorWindowState\(\);/, replace: "$1postTranslatorWindowState();\n$2trWin_.ready();", why: "the state posted BEFORE the page is marked ready (canPost false: lost)" },
   { file: "winState", find: ".kv(L\"fullscreen\", fullscreen)", replace: "", why: "the post no longer carries fullscreen" }]);

pin("W2", "ui WE review C1", "the window state is posted only to a page that said `ready` (WindowState.canPost), and the one-shot `file` is consumed by the post that carries it",
  (S) => /if \(webview_ && trWin_\.canPost\(\)\) postToTranslatorView\(trWin_\.post\(\)\);/.test(S.bridge)
      && /bool canPost\(\) const \{ return pageReady; \}/.test(S.winState)
      && /const bool file = showFile && pageReady;/.test(S.winState) && /if \(file\) showFile = false;/.test(S.winState),
  [{ file: "bridge", find: "if (webview_ && trWin_.canPost()) postToTranslatorView", replace: "if (webview_) postToTranslatorView", why: "posted to a page that is still loading (lost) or reloading (the OLD document)" },
   { file: "winState", find: "if (file) showFile = false;", replace: "", why: "the one-shot never consumed (the original C1 bug)" }]);

pin("W3", "ui WE review C1", "...and the page is not `ready` again until its next `ready`: attached, reloading (before Reload()) and detached reset it, and the old flags (trShowFile_ / trPageReady_ / trShown_ / trFullscreen_) are gone",
  (S) => /trWin_\.attached\(\);/.test(S.bridge) && /b\.trWin_\.reloading\(\);[^\n]*\n\s*b\.webview_->Reload\(\);/.test(S.bridge)
      && /trWin_\.detached\(\);/.test(S.bridge) && !/trShowFile_|trPageReady_|trShown_|trFullscreen_/.test(S.bridge + S.bridgeH)
      && /void attached\(\)  \{ shown = true; pageReady = false; \}/.test(S.winState) && /void reloading\(\) \{ pageReady = false; \}/.test(S.winState),
  [{ file: "bridge", find: /b\.trWin_\.reloading\(\);[^\n]*\n/, replace: "", why: "the page still counts as ready while it reloads" },
   { file: "winState", find: "void reloading() { pageReady = false; }", replace: "void reloading() { }", why: "reloading() no longer clears readiness" }]);

pin("W4", "ui WE-10", "full screen: the PAGE asks (translatorFullscreen -> kWmTranslatorWindow wParam 2), the WINDOW decides and ALWAYS answers -- already so, or changed, the answer is posted",
  (S) => /else if \(wParam == 2\) setFullscreen\(lParam != 0\);/.test(S.main)
      && /if \(on == g_fullscreen\) \{ if \(g_bridge\) g_bridge->translatorFullscreenChanged\(g_fullscreen\); return; \}/.test(S.main)
      && /resizeView\(\);[\s\S]{0,400}if \(g_bridge\) g_bridge->translatorFullscreenChanged\(g_fullscreen\);\s*\}/.test(S.main)
      && /type == L"translatorFullscreen"[\s\S]{0,200}PostMessageW\(hostWnd_, kWmTranslatorWindow, 2, on \? 1 : 0\)/.test(S.bridge)
      && /void Bridge::translatorFullscreenChanged\(bool on\) \{\s*trWin_\.fullscreen = on;\s*postTranslatorWindowState\(\);/.test(S.bridge),
  [{ file: "main", find: "if (on == g_fullscreen) { if (g_bridge) g_bridge->translatorFullscreenChanged(g_fullscreen); return; }", replace: "if (on == g_fullscreen) return;", why: "an ask for the state the window already has is not answered (the page's idea drifts)" },
   { file: "bridge", find: "    trWin_.fullscreen = on;\n    postTranslatorWindowState();", replace: "    trWin_.fullscreen = on;", why: "the window's answer is never posted" }]);

pin("W5", "ui WC 4c [G]", "no SW_RESTORE of a window that is not minimised (it would un-maximise it / leave full screen): bringUp() and a second launch restore only an ICONIC window",
  (S) => {
    const lines = S.main.split("\n").filter((l) => /SW_RESTORE/.test(l) && !/^\s*\/\//.test(l));
    return /if \(::IsIconic\(g_hwnd\)\) ::ShowWindow\(g_hwnd, g_quiet \? SW_SHOWNOACTIVATE : SW_RESTORE\);/.test(S.main)
        && /if \(::IsIconic\(existing\)\) ::ShowWindow\(existing, SW_RESTORE\);/.test(S.main)
        && lines.length >= 2 && lines.every((l) => /IsIconic/.test(l));
  },
  [{ file: "main", find: "if (::IsIconic(g_hwnd)) ::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);", replace: "::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);", why: "bringUp restores a window that is not minimised" },
   { file: "main", find: "if (::IsIconic(existing)) ::ShowWindow(existing, SW_RESTORE);", replace: "::ShowWindow(existing, SW_RESTORE);", why: "a second launch un-maximises the running window" }]);

pin("W6", "ui WC (the window itself)", "the window keeps its minimum size (420 x 320 at 96 dpi) and its place is remembered (saveWinPos on WM_EXITSIZEMOVE / close / session end, from app.txt)",
  (S) => /ptMinTrackSize\.x = \(LONG\)\(420 \* dpi \/ 96\);/.test(S.main) && /ptMinTrackSize\.y = \(LONG\)\(320 \* dpi \/ 96\);/.test(S.main)
      && /case WM_EXITSIZEMOVE:\s*saveWinPos\(\);/.test(S.main)
      && /case WM_CLOSE:\s*if \(g_fullscreen\) setFullscreen\(false\);\s*saveWinPos\(\);/.test(S.main)
      && /case WM_QUERYENDSESSION:\s*saveWinPos\(\);/.test(S.main),
  [{ file: "main", find: "ptMinTrackSize.x = (LONG)(420 * dpi / 96);", replace: "ptMinTrackSize.x = (LONG)(200 * dpi / 96);", why: "the minimum width halved" },
   { file: "main", find: "if (g_fullscreen) setFullscreen(false);\n        saveWinPos();", replace: "saveWinPos();", why: "closed in full screen: the monitor's rectangle would be saved as the window's" },
   { file: "main", find: "case WM_EXITSIZEMOVE:\n        saveWinPos();      // where the user PUT it, remembered when they let go\n", replace: "case WM_EXITSIZEMOVE:\n", why: "a drag or a resize of the window is not remembered" }]);

pin("W7", "ui WC 4c [B]", "the view joins the bridge BEFORE its first navigation (attach maps the proxy folder + the player's folders; a mapping set after a page loaded is invisible to it) -- and the proxy folder is mapped once per view",
  (S) => {
    const attach = fnBody(S.bridge, "void Bridge::attach");
    const at = S.main.indexOf("g_bridge->attach("), nav = S.main.indexOf("g_webview->Navigate((std::wstring(kWebHost) + L\"index.html\").c_str());");
    return attach.indexOf("mapTranslatorProxyFolder();") > 0 && attach.indexOf("trMappedHosts_.clear();") > 0
        && attach.indexOf("mapTranslatorProxyFolder();") < attach.indexOf("trWin_.attached();")
        && at > 0 && nav > at && !/kTrProxyHost/.test(fnBody(S.bridge, "void Bridge::mapPlayerFolders"));
  },
  [{ file: "bridge", find: "    mapTranslatorProxyFolder();\n    trWin_.attached();", replace: "    trWin_.attached();", why: "the proxy folder is no longer mapped before the first navigation" },
   { file: "main", find: "g_bridge->attach(g_webview.Get(), hwnd);", replace: "/* attach moved */", why: "the view navigates before the bridge attached" }]);

pin("W8", "ui WC (CMake woff2 pin)", "the page ships with its neighbours: app\\CMakeLists.txt copies EVERY file of app\\web (GLOB_RECURSE) next to the exe, as a hard prerequisite of the exe (the page, and any asset or sub-folder a later version adds -- the title's font is a Windows system face now: no file)",
  (S) => /file\(GLOB_RECURSE SUVTITLE_WEB_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false "\$\{APP_DIR\}\/web\/\*"\)/.test(S.cmake)
      && /foreach\(_web_src IN LISTS SUVTITLE_WEB_SOURCES\)/.test(S.cmake) && fs.existsSync(path.join(APP, "web", "index.html")),
  [{ file: "cmake", find: "GLOB_RECURSE SUVTITLE_WEB_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false", replace: "GLOB SUVTITLE_WEB_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false", why: "only the top level of web\\ is copied (a sub-folder, an asset in it, stays behind)" }]);

// ================= S: the WebView2 environment ============================================================================
pin("S1", "ui WE-4 (main.cpp pins)", "the environment is created WITH the three switches and re-created WITHOUT them if that fails (sync AND async), and the comment says why it is safe",
  (S) => /put_AdditionalBrowserArguments\(\s*L"--autoplay-policy=no-user-gesture-required"/.test(S.main)
      && /createWebViewEnv\(hwnd, userDataFolder, \/\*withSwitches=\*\/true\);/.test(S.main)
      && count(S.main, /createWebViewEnv\(hwnd, userDataFolder, false\)/g) === 2
      && /if \(\(FAILED\(r\) \|\| !env\) && withSwitches\) \{\s*createWebViewEnv\(hwnd, userDataFolder, false\);/.test(S.main)
      && /if \(FAILED\(hr\) && withSwitches\) \{ createWebViewEnv\(hwnd, userDataFolder, false\); return; \}/.test(S.main)
      && /WHY IT IS SAFE: the view only ever shows OUR OWN local page/.test(S.main),
  [{ file: "main", find: "createWebViewEnv(hwnd, userDataFolder, false);   // once more, without the switches", replace: "(void)0;   // no retry", why: "no retry without the switches in the async failure path (a busy profile = a dead window)" },
   { file: "main", find: "createWebViewEnv(hwnd, userDataFolder, /*withSwitches=*/true);", replace: "createWebViewEnv(hwnd, userDataFolder, /*withSwitches=*/false);", why: "the environment never gets the switches" }]);

pin("S2", "ui WE-4 / user report 11/10", "...the SAME switched environment: autoplay, video out of DirectComposition overlays, AudioVideoTracks -- one string, in this order",
  (S) => /put_AdditionalBrowserArguments\(\s*L"--autoplay-policy=no-user-gesture-required"\s*L" --disable-direct-composition-video-overlays"\s*L" --enable-blink-features=AudioVideoTracks"\)/.test(S.main),
  [{ file: "main", find: 'L" --disable-direct-composition-video-overlays"', replace: 'L" --disable-gpu-vsync"', why: "the video overlay switch dropped (the subtitles vanish over a hardware plane on AMD)" },
   { file: "main", find: 'L" --enable-blink-features=AudioVideoTracks"', replace: 'L" --enable-blink-features=Foo"', why: "no audioTracks: the film's sound tracks cannot be switched" }]);

pin("S3", "ui WH (HEVC, measured 11/10)", "...and DirectComposition itself stays ON: --disable-direct-composition (without the -video-overlays suffix) never comes back (this runtime then has no HEVC decoder)",
  (S) => !/L"[^"]*--disable-direct-composition"/.test(S.main) && !/--disable-direct-composition[ "](?!-)/.test(codeOnly(S.main))
      && /--disable-direct-composition-video-overlays/.test(S.main),
  [{ file: "main", find: 'L" --disable-direct-composition-video-overlays"', replace: 'L" --disable-direct-composition"', why: "the HEVC-killing switch is back" }]);

// ================= D: drops and paths ======================================================================================
pin("D1", "ui WE-14", "NO PATH EVER COMES FROM THE PAGE'S JSON: drops arrive as ICoreWebView2File objects, dialogs run here, the queue and the subtitle files go by ids -- and no key the bridge reads names a path",
  (S) => {
    const handlers = between(S.bridge, 'if (type == L"translatorFileOpen" || type == L"playerOpen"', 'if (type == L"translatorFullscreen")');
    const attachDrop = between(S.bridge, 'else if (type == L"translatorFileDrop") {', "handleTranslatorFileDrop(paths, quality, target);");
    // EVERY key the bridge reads from the page's JSON -- jsonFind* (tokens, numbers, ids) and the editor's structured reader (JsonR.h:
    // v.str / u64 / get / flag) -- is on a CLOSED list (RV4-13: the 31 + 14 keys of 23/10/2026, none a path). A new key fails this pin: look
    // at it -- if it names a path / file / folder / url it must not exist; if it is a token, a number or an id, add it to the list below.
    // + the 5 of the Live window's liveConfig{inputs:[{key, label, chan, gainDb}]} (1.1.0 LIVE, 25/10, R2): none is a path -- `key` must be
    // one the ENGINE listed (or a strip's already: app.txt LIVEIN), a `wav:` test file only on a test instance; `label` is display text only.
    const flat = [...S.bridge.matchAll(/jsonFind(?:String|Number|Bool)\(\s*json\s*,\s*L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const structured = [...S.bridge.matchAll(/\b(?:v|o)\.(?:str|u64|get|flag)\(L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const keys = [...new Set([...flat, ...structured])];
    const KNOWN = ["type", "quality", "target", "mode", "preset", "langA", "langB", "clear", "alwaysOnTop", "value", "what", "force", "jobId", "format",
      "which", "seq", "op", "id", "to", "ms", "translate", "repeat", "shuffle", "audioOrd", "auto", "gen", "durMs", "playing", "reason", "enc", "on",
      "rev", "remove", "add", "restore", "s", "e", "text", "lang", "dstLang", "src", "dst", "pass",   // (the editor's edit: cue fields, free text, never a path)
      "inputs", "key", "label", "chan", "gainDb",   // (the Live strips: a key the engine listed, a display name, numbers)
      "open",                                       // (fix round 26/10, RVB-7: liveSources{open:false}, the picker closed -- a flag)
      "kind", "render", "sub", "full", "shown",    // (25/10 THE FREEZE WATCH, playerDiag: tokens of closed lists + flags, never a path)
      // (26/10, 1.1.0 THE NATIVE VIDEO ENGINE, N2: nativeRect{x,y,w,h} CSS px; nativeCmd{cmd, gen, ms, exact, rate, pct, on, aMs,
      //  bMs, ord, step} -- a token of a closed list, the opening's seq and numbers; nativeCursor{show} -- a flag. mpv plays the
      //  item the BRIDGE opened from the core's session: no path, no url, no name ever comes with them. Fix round 02/11 (L7, M2, L9):
      //  nativeState's `started` / `restarted` / `video` go TO the page -- the bridge reads no new key, so none is added here)
      "x", "y", "w", "h", "cmd", "exact", "rate", "pct", "aMs", "bMs", "ord", "step", "show",
      // (U2b, 02/11/2026: translatorModelRemove{kind, id, recycle} -- `recycle` a flag: the model's file to the Recycle Bin too; the
      //  path is the bridge's own list's, never the page's)
      "recycle",
      // (FX, 02/11/2026: translatorSpeakerName{jobId, id, label, vrev} -- `vrev` a NUMBER: the speakers' numbering the name was typed
      //  against, the core refuses a stale one; never a path)
      "vrev",
      // (THE UPDATE BANNER, 03/11/2026: update.enable{on} reads `on`, listed above; update.open reads NOTHING -- the app opens
      //  its own pageUrl)
    ];
    const unknown = keys.filter((k) => KNOWN.indexOf(k) < 0), pathy = keys.filter((k) => /path|file|folder|dir|url|uri|name/i.test(k));
    return [handlers.length > 2000 && !/L"path"/.test(handlers) && /The PATH is looked up here, never received\./.test(handlers)
            && /ComPtr<ICoreWebView2File> f;/.test(S.bridge) && /f->get_Path\(&p\)/.test(attachDrop) && flat.length >= 30 && structured.length >= 5
            && unknown.length === 0 && pathy.length === 0,
            { keys: keys.length, unknown, pathy }];
  },
  [{ file: "bridge", find: 'jsonFindString(json, L"quality", q);\n        handlePlayerOpen(', replace: 'jsonFindString(json, L"quality", q); jsonFindString(json, L"path", q);\n        handlePlayerOpen(', why: "a handler reads a `path` key from the page's JSON" },
   { file: "bridge", find: "The PATH is looked up here, never received.", replace: "(the path comes with the message)", why: "the comment that states the rule is gone (the rule with it)" },
   { file: "bridge", find: 'jsonFindString(json, L"quality", q);\n        handlePlayerOpen(', replace: 'jsonFindString(json, L"quality", q); jsonFindString(json, L"hint", q);\n        handlePlayerOpen(', why: "a NEW key with an innocent name (the closed list is the gate: is it a path?)" },
   { file: "bridge", find: 'in.key = u8(o.str(L"key"));', replace: 'in.key = u8(o.str(L"source"));', why: "the Live strips read a NEW key (a strip's source by another name: the closed list is the gate)" },
   { file: "bridge", find: 'const std::wstring cmd = v.str(L"cmd");', replace: 'const std::wstring cmd = v.str(L"cmd"); const std::wstring media = v.str(L"url");',
     why: "1.1.0: the native transport reads a url from the page (mpv would play what the PAGE names, not the core's item)" }]);

pin("D2", "ui WC 4c [C]", "no drop is taken while a file dialog is up (it would pull the session out from under the dialog) -- refused, and said",
  (S) => /type == L"translatorFileDrop" && trDialogBusy_/.test(S.bridge) && /A file dialog is open: finish it first, then drop the file again\./.test(S.bridge)
      && S.bridge.indexOf('type == L"translatorFileDrop" && trDialogBusy_') < S.bridge.indexOf('else if (type == L"translatorFileDrop") {'),
  [{ file: "bridge", find: 'if (type == L"translatorFileDrop" && trDialogBusy_) {', replace: 'if (type == L"translatorFileDrop" && false) {', why: "a drop under an open dialog is taken" }]);

pin("D3", "ui WC 4c [4]", "a dropped path is classified by the DISK and its extension (folder / media / subtitle) before the player is touched",
  (S) => {
    const drop = fnBody(S.bridge, "void Bridge::handleTranslatorFileDrop");
    return /if \(isDir\) folders\.push_back\(p\);\s*else if \(pbtrf::isMediaExt\(ext\)\) \{ media\.push_back\(p\); known\.push_back\([^;]*\); \}\s*else if \(pbsubr::isSubtitleExt\(ext\)\) subs\.push_back\(p\);/.test(drop)
        && drop.indexOf("isMediaExt") > 0 && drop.indexOf("isMediaExt") < drop.indexOf("player_->open(") && drop.indexOf("isMediaExt") < drop.indexOf("startPlayerScan(")
        && /if \(!pbtrf::fileIdentity\(p, s, m, &isDir\)\) \{ \+\+other; continue; \}/.test(drop);
  },
  [{ file: "bridge", find: "else if (pbtrf::isMediaExt(ext)) { media.push_back(p);", replace: "else if (true) { media.push_back(p);", why: "any dropped file is taken as media (a .txt would reach the player)" },
   { file: "bridge", find: "if (!pbtrf::fileIdentity(p, s, m, &isDir)) { ++other; continue; }", replace: "if (!pbtrf::fileIdentity(p, s, m, &isDir)) { }", why: "a path the disk does not know is classified anyway" }]);

pin("D4", "ui WC 4c [C]", "the export reads nothing of the session after the Save dialog (a file opened meanwhile REPLACES it): its cues are copied first, in START order, with the count and the media path",
  (S) => {
    const exp = fnBody(S.bridge, "void Bridge::handleTranslatorFileExport");
    const afterShow = exp.slice(exp.indexOf("dlg->Show("));
    return exp.indexOf("dlg->Show(") > 0 && !/\bS\.|\bse\.|player_->/.test(afterShow)
        && /std::vector<pbt::trpipe::WireCue> cues = S\.cues;\s*pbtrf::canonicalCues\(cues\);/.test(exp)
        && /const long long count = \(long long\)cues\.size\(\);/.test(exp) && /const std::wstring mediaPath = S\.path;/.test(exp);
  },
  [{ file: "bridge", find: /(std::vector<pbt::trpipe::WireCue> cues = S\.cues;\n\s*)pbtrf::canonicalCues\(cues\);/, replace: "$1", why: "the copy is no longer put in START order" },
   { file: "bridge", find: "const std::wstring mediaPath = S.path;", replace: "", why: "the media path is read from the session after the dialog" }]);

pin("D5", "ui WE review C8", "a subtitle file dropped WITH a folder rides the scan and is applied after the scan's files are opened (and for the current file when nothing new plays)",
  (S) => {
    const scan = between(S.bridge, "void Bridge::startPlayerScan(", "\nvoid Bridge::handlePlayerSubLoad");
    return /startPlayerScan\(folders, media, subs, play, false\);/.test(S.bridge) && /d\.subs = subs;/.test(scan)
        // (since SPLIT_PLAN ADDENDUM 2 / RV4-1 the viewer's file is announced first: `player_->userSubFile(); loadExtSub(...)`, so the
        //  page's AUTOMATIC default for the item is ignored -- the pin follows that shape; tests\cpp\fixround_pins.py pins the
        //  announcement before every file the viewer names)
        && /if \(d\.play\) player_->open\(d\.files, d\.force, &d\.known\);\s*else player_->add\(d\.files, &d\.known\);[\s\S]{0,260}if \(!d\.subs\.empty\(\)\) \{ player_->userSubFile\(\); loadExtSub\(d\.subs\.front\(\), 0, 0\); \}/.test(scan);
  },
  [{ file: "bridge", find: "        d.subs = subs;\n", replace: "", why: "the dropped subtitle file never rides the scan" },
   { file: "bridge", find: "        // the subtitle file dropped WITH the folder: for the file that plays NOW (C8) -- the viewer's (RV4-1)\n        if (!d.subs.empty()) { player_->userSubFile(); loadExtSub(d.subs.front(), 0, 0); }\n", replace: "", why: "the subtitle file is never applied after the scan's files are opened" }]);

pin("D6", "ui WE review P2", "the scan's identities come from its own listing (the worker), and open/add take them (no second stat on the UI thread)",
  (S) => {
    const scan = between(S.bridge, "void Bridge::startPlayerScan(", "\nvoid Bridge::handlePlayerSubLoad");
    return /d\.known\.push_back\(pbplayer::Player::Known\{ r\.sizes\[i\], r\.mtimes\[i\], true \}\);/.test(scan)
        && /out\.sizes\.push_back\(f\.size\);/.test(S.player) && /std::map<std::wstring, uint64_t> have = pathIndex\(\);/.test(S.player);
  },
  [{ file: "bridge", find: "d.known.push_back(pbplayer::Player::Known{ r.sizes[i], r.mtimes[i], true });", replace: "d.known.push_back(pbplayer::Player::Known{ 0, 0, false });", why: "the listing's identities are thrown away (the UI thread stats every file again)" }]);

pin("D7", "ui WE-14", "folder scans run on a WORKER thread (a whole drive must not freeze the window), capped and cancellable",
  (S) => {
    const scan = between(S.bridge, "void Bridge::startPlayerScan(", "\nvoid Bridge::handlePlayerSubLoad");
    return /std::thread\(\[box, folders, files, subs, play, force, h\]/.test(scan) && /pbplayer::scanFolder\(f, caps, r, &box->cancel\);/.test(scan)
        && /\}\)\.detach\(\);/.test(scan) && /if \(scanBox_\) scanBox_->cancel\.store\(true\);/.test(S.bridge);
  },
  [{ file: "bridge", find: "pbplayer::scanFolder(f, caps, r, &box->cancel);", replace: "pbplayer::scanFolder(f, caps, r, nullptr);", why: "the scan can no longer be cancelled (the app waits for a whole drive at exit)" }]);

pin("D8", "ui WE-14", "FFOCUS goes out ONLY through the core's rule (shouldFocus), never straight from a report",
  (S) => count(S.bridge, /focusFile\(/g) === 1 && /void Bridge::PlayerHost::sendFocus\([^)]*\) \{ if \(b\.translator_\) b\.translator_->focusFile\(/.test(S.bridge)
      && count(S.player, /host_\.sendFocus\(/g) === 2 && count(S.player, /if \(j && shouldFocus\([^\n]*\n\s*host_\.sendFocus\(/g) === 2,
  [{ file: "player", find: /if \(j && shouldFocus\(j->set->ranges, j->set->totalMs \? j->set->totalMs : s_\.durMs, ms, s_\.lastFocusMs, why\)\) \{/, replace: "if (j) {", why: "a focus is sent on every position report (the engine is flooded with FFOCUS)" }]);

pin("D9", "ui WE-14", "the page's `ready` names its tab and the bridge applies it AFTER the replay (pre-analysis runs on the File tab only): windowReady() then view(\"file\")",
  (S) => /player_->windowReady\(\);\s*player_->view\("file"\);/.test(S.bridge) && /void view\(const std::string& mode\) \{ view_ = mode == "file" \? "file" : "live";/.test(S.player),
  [{ file: "bridge", find: 'player_->windowReady();\n            player_->view("file");', replace: 'player_->view("file");\n            player_->windowReady();', why: "the tab is applied BEFORE the replay" }]);

pin("D10", "ui WE-14 + P3", "the app writes ONLY its own files (player.txt, player-resume.txt, player-audio.txt, app.txt via the AV-safe write): never engine.txt (the engine's, S11), never AudioPB's folders, never settings.txt; the core opens no file itself",
  (S) => {
    const bridge = codeOnly(S.bridge), main = codeOnly(S.main);
    return /appDataPath\(L"player\.txt"\)/.test(bridge) && /appDataPath\(L"player-resume\.txt"\)/.test(bridge) && /appDataPath\(L"player-audio\.txt"\)/.test(bridge)
        && !/writeFileAvSafe\([^;]*(engine|translator|settings)\.txt/.test(bridge + main) && !/(engine|translator|settings)\.txt/.test(codeOnly(S.player))
        && !/CreateFileW|ofstream|_wfopen/.test(codeOnly(S.player)) && /writeFileAvSafe\(f, body\)/.test(main)
        && !/L"AudioPB"/.test(bridge + main);
  },
  [{ file: "bridge", find: 'const std::wstring p = b.appDataPath(L"player.txt");', replace: 'const std::wstring p = b.appDataPath(L"engine.txt");\n    pbtrf::writeFileAvSafe(b.appDataPath(L"engine.txt"), bytes);', why: "the app writes the engine's config file" },
   { file: "player", find: "void flushFiles() {", replace: "void flushFiles() { std::ofstream bad(\"x\");", why: "the core opens a file itself (ofstream)" }]);

pin("D11", "ui WE review P8", "the Windows session's end (WM_QUERYENDSESSION / WM_ENDSESSION) writes the player's state, and persistPlayer() saves it",
  (S) => /case WM_QUERYENDSESSION:\s*saveWinPos\(\);\s*if \(g_bridge\) g_bridge->persistPlayer\(\);\s*return TRUE;/.test(S.main)
      && /case WM_ENDSESSION:\s*if \(wParam && g_bridge\) g_bridge->persistPlayer\(\);/.test(S.main)
      && /void Bridge::persistPlayer\(\) \{[\s\S]{0,300}if \(player_\) player_->persist\(\);/.test(S.bridge),
  [{ file: "main", find: "case WM_ENDSESSION:\n        if (wParam && g_bridge) g_bridge->persistPlayer();", replace: "case WM_ENDSESSION:\n        (void)wParam;", why: "the queue and positions are lost when Windows shuts down mid-play" }]);

pin("D12", "ui WE review P1", "while the window's page reloads nothing is posted to it (PlayerHost::postMain is a no-op: the reloading page gets everything back on `ready`), and the held notices are flushed on `ready`",
  (S) => /void Bridge::PlayerHost::postMain\(const std::wstring& json\) \{ \(void\)json; \}/.test(S.bridge)
      && /if \(windowBusy\(\)\) host_\.postMain\(msg\);[^\n]*\n\s*else host_\.postBoth\(msg\);/.test(S.player)
      && /void windowReady\(\) \{[\s\S]{0,200}for \(const auto& n : heldNotices_\) host_\.postWindow\(n\);\s*heldNotices_\.clear\(\);/.test(S.player),
  [{ file: "bridge", find: "void Bridge::PlayerHost::postMain(const std::wstring& json) { (void)json; }", replace: "void Bridge::PlayerHost::postMain(const std::wstring& json) { b.postToWeb(json); }", why: "state is posted into the OLD document of a page that is reloading (lost / shown twice)" }]);

// ================= M: folders, hosts, URLs, the proxy -- the page's reload for a new folder ============================================
pin("M1", "ui WC 4c [F]", "PlayFailed is matched on the SESSION (seq), not the job: a re-analysis started while the page was finding out it cannot play the file must not make its request look stale",
  (S) => /translatorFilePlayFailed"\) \{[\s\S]{0,400}jsonFindNumber\(json, L"seq"/.test(S.bridge)
      && /void playFailed\(uint64_t seq\) \{\s*if \(!loaded_ \|\| seq != s_\.seq\) return;/.test(S.player),
  [{ file: "player", find: "if (!loaded_ || seq != s_.seq) return;", replace: "if (!loaded_ || seq != s_.gen) return;", why: "matched on the job (a re-analysis makes the page's request look stale: silence forever)" },
   { file: "bridge", find: "long long seq = 0; jsonFindNumber(json, L\"seq\", seq);", replace: "long long seq = 0; jsonFindNumber(json, L\"jobId\", seq);", why: "the bridge reads the job, not the session" }]);

pin("M2", "ui WC 4c [3] + 1.1.0 fix round L2", "a folder that cannot be mapped (an odd UNC share, a vanished drive) -> no URL, and the core asks for the sound itself at once (a page with nothing to load waits forever) -- unless mpv plays the item (it reads the file itself; the copy is the <video>'s): then it is asked the moment mpv lets the item go (mpvDropped)",
  (S) => /s_\.url\.clear\(\);\s*if \(!host_\.mpvPlays\(\)\) requestProxy\(\);/.test(fnBody(S.player, "void mapNow"))
      && /if \(s_\.url\.empty\(\)\) requestProxy\(\);/.test(fnBody(S.player, "void mpvDropped")),
  [{ file: "player", find: "            s_.url.clear();\n            if (!host_.mpvPlays()) requestProxy();", replace: "            s_.url.clear();", why: "the URL is cleared and nobody asks for the sound: the page waits for ever" },
   { file: "player", find: "        if (s_.url.empty()) requestProxy();            // (asked once per item", replace: "        // (asked once per item", why: "mpv lets an item with no URL go and nobody asks for its sound: the page waits for ever" }]);

pin("M3", "ui WC 4c [B]", "a folder the loaded page has not seen is mapped, then the page RELOADED (and not `ready` until it is back); on its `ready` the committed set is what it navigated with and it gets everything back",
  (S) => {
    const ensure = fnBody(S.player, "bool ensureMapped");
    return /if \(committed_\.count\(lowerW\(s_\.folder\)\)\) return false;\s*mapNow\(\);[\s\S]{0,120}viewReady_ = false;[\s\S]{0,500}committed_ = mapped_;\s*host_\.reloadWindow\(\);/.test(ensure)
        && /void windowReady\(\) \{\s*viewLoaded_ = viewReady_ = true;\s*committed_ = mapped_;\s*replay\(\);/.test(S.player)
        && /void Bridge::PlayerHost::reloadWindow\(\) \{[\s\S]{0,200}b\.trWin_\.reloading\(\);[^\n]*\n\s*b\.webview_->Reload\(\);/.test(S.bridge);
  },
  [{ file: "player", find: "        committed_ = mapped_;\n        host_.reloadWindow();", replace: "        committed_ = mapped_;", why: "a mapping set after the page loaded is invisible to it, and nothing reloads it: the film never loads" },
   { file: "player", find: "viewLoaded_ = viewReady_ = true;\n        committed_ = mapped_;\n        replay();", replace: "viewLoaded_ = viewReady_ = true;\n        replay();", why: "after the reload the committed set is stale (a folder of the old set is loaded with no reload: sound only)" }]);

pin("M4", "ui WC 4c [B]", "a host is never RE-POINTED: one per FOLDER (f<folderKey>), one already mapped is left alone, the others cleared, DENY_CORS",
  (S) => {
    const mapF = fnBody(S.bridge, "void Bridge::mapPlayerFolders");
    return /if \(trMappedHosts_\.count\(host\)\) \{ now\.insert\(host\); continue; \}/.test(mapF) && /ClearVirtualHostNameToFolderMapping/.test(mapF)
        && /COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS/.test(mapF) && /L"f" \+ w16\(pbtrf::folderKey\(f\)\) \+ pbplayer::kMediaHostSuffix/.test(mapF);
  },
  [{ file: "bridge", find: "if (trMappedHosts_.count(host)) { now.insert(host); continue; }", replace: "", why: "a host already mapped is mapped again (re-pointed: the page keeps the OLD folder until a reload)" },
   { file: "bridge", find: "COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))\n            now.insert(host);", replace: "COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW)))\n            now.insert(host);", why: "the media folder is served with CORS allowed (any page could read the user's films)" }]);

pin("M5", "ui WC 4c [A]", "the media URL and the proxy's name come from the MEDIA's identity (folder key + ?v=<mediaKey>), never a per-run counter -- the two lines tests\\e2e\\we_e2e\\fnv_twin.cpp repeats",
  (S) => /n\.host = L"f" \+ w16\(pbtrf::folderKey\(it\.folder\)\) \+ kMediaHostSuffix;/.test(S.player)
      && /n\.url = L"https:\/\/" \+ n\.host \+ L"\/" \+ w16\(pbtrf::pctEncode\(u8\(it\.name\)\)\) \+ L"\?v=" \+ w16\(n\.mediaKey\);/.test(S.player)
      && /n\.mediaKey = pbtrf::mediaKey\(it\.path, size, mtime\);/.test(S.player)
      && /joinPath\(pd, w16\(mediaKey\) \+ L"-" \+ std::to_wstring\(id\) \+ L"\.wav"\)/.test(S.bridge) && !/L"m" \+ std::to_wstring\(s->seq\)/.test(S.bridge)
      && /const wchar_t\* const kMediaHostSuffix = L"\.suvtitle-media\.example";/.test(S.player),
  [{ file: "player", find: 'n.host = L"f" + w16(pbtrf::folderKey(it.folder)) + kMediaHostSuffix;', replace: 'n.host = L"f" + w16(pbtrf::folderKey(it.path)) + kMediaHostSuffix;', why: "a host per FILE: one reload per film instead of per folder (and the kit's hosts no longer match)" },
   { file: "player", find: '+ L"?v=" + w16(n.mediaKey);', replace: '+ L"?v=" + std::to_wstring(n.seq);', why: "a per-run counter in the URL: the HTTP cache serves another file's bytes" }]);

pin("M6", "ui WC 4c", "the first proxy of a run purges the proxy folder: old WAVs AND the helper's orphaned *.wav.tmp",
  (S) => /if \(!b\.trProxyPurged_\) \{[\s\S]{0,200}keepNewest\(pd, L"\*\.wav", 0\);[\s\S]{0,400}keepNewest\(pd, L"\*\.wav\.tmp", 0\);/.test(fnBody(S.bridge, "std::wstring Bridge::PlayerHost::proxyPath")),
  [{ file: "bridge", find: 'pbtrf::keepNewest(pd, L"*.wav.tmp", 0);', replace: "", why: "a half-written multi-GB .wav.tmp survives every run" }]);

pin("M7", "ui WC 4c [K]", "translatorFile carries `fresh` (the session's generation not yet seen), counted as seen only once the window page said `ready`",
  (S) => /const bool fresh = s_\.gen != windowSaw_;\s*if \(attached_ && viewReady_\) windowSaw_ = s_\.gen;/.test(S.player) && /j\.kv\(L"fresh", fresh\);/.test(S.player),
  [{ file: "player", find: "if (attached_ && viewReady_) windowSaw_ = s_.gen;", replace: "windowSaw_ = s_.gen;", why: "a state posted to a loading page counts as seen: its replay is `fresh:false` and the file never starts" }]);

pin("M8", "RV4-12 (new)", "the FNV-1a OFFSET BASIS is 1469598103934665603 (the textbook value with its last digit dropped -- an old typo that must STAY: every cache key, registry line and virtual host of a released version comes from it)",
  (S) => /inline uint64_t fnv1a64\(const std::string& s\) \{\s*uint64_t h = 1469598103934665603ULL;\s*for \(unsigned char c : s\) \{ h \^= c; h \*= 1099511628211ULL; \}/.test(S.store),
  [{ file: "store", find: "uint64_t h = 1469598103934665603ULL;", replace: "uint64_t h = 14695981039346656037ULL;", why: "someone 'fixes' the basis: every cache and every host changes (all analyses are redone)" }]);

// ================= F: the files the app writes ===============================================================================
pin("F1", "ui WC 4c [1] (WG)", "a cache hit CONFIRMS its files: one is kept only while it is OURS for THIS media (the registry) and already holds exactly these bytes, else written again",
  (S) => {
    const srtW = fnBody(S.bridge, "std::wstring Bridge::writeTranslatorSrt");
    return /const bool replace = exists\(p\);\s*\/\/ here only when it is ours for this media/.test(srtW) && /if \(replace\) \{[\s\S]{0,500}have == bytes/.test(srtW)
        && /return pbtrf::fileIdentity\(p, s, m\) && pbtrf::registryOwns\(reg, p, s, m, S\.path\);/.test(srtW)
        && /if \(writeSrts\(S\)\) host_\.writeCache\(S\.key, recordOf\(S\)\);/.test(S.player);
  },
  [{ file: "bridge", find: "return pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwns(reg, p, s, m, S.path);\n    };\n    auto exists = [](const std::wstring& p) { return pbtrf::fileExists(p); };\n    // (Documents", replace: "return pbtrf::fileIdentity(p, s, m);\n    };\n    auto exists = [](const std::wstring& p) { return pbtrf::fileExists(p); };\n    // (Documents", why: "a file of that name is taken for ours without asking the registry (a downloaded subtitle gets overwritten)" },
   { file: "bridge", find: "&& have == bytes &&", replace: "&& true &&", why: "an identical file is rewritten at every tick (write-time churn for a player watching it)" }]);

pin("F2", "ui WC 4c [1][2][D][E]", "the automatic .srt: free-or-ours name, no-replace when the name was ABSENT, identity as WRITTEN, registered with its media and the record that wrote it",
  (S) => {
    const srt = fnBody(S.bridge, "std::wstring Bridge::writeTranslatorSrt");
    return /chooseSrtPath\(dirs\[k\], base, ext, tag, sibling, exists, ownsForThisMedia\)/.test(srt) && /const bool replace = exists\(p\);/.test(srt)
        && /writeFileAvSafe\(p, bytes, replace, &id\)/.test(srt) && /trRegister\(p, s, m, S\.path, lang, fromKey, fromOrd\)/.test(srt) && /hasSiblingMedia\(/.test(srt);
  },
  [{ file: "bridge", find: "pbtrf::writeFileAvSafe(p, bytes, replace, &id)) {\n                if (!replace && exists(p)) continue;      // it appeared under us: the next free name\n                break;                                    // this folder refuses the write\n            }\n            uint64_t s = id.size", replace: "pbtrf::writeFileAvSafe(p, bytes, true, &id)) {\n                if (!replace && exists(p)) continue;      // it appeared under us: the next free name\n                break;                                    // this folder refuses the write\n            }\n            uint64_t s = id.size", why: "a file that appeared meanwhile is clobbered (replace is always on)" },
   { file: "bridge", find: "trRegister(p, s, m, S.path, lang, fromKey, fromOrd);", replace: "", why: "the new .srt is not registered: next time it counts as somebody else's and is never refreshed" }]);

pin("F3", "ui WC 4c [D8]", "the .srt is written only for a COMPLETE analysis from the finish; a partial one goes to the cache alone (savePartial writes no .srt)",
  (S) => /void savePartial\(CueSet& S\) \{\s*if \(S\.complete\) return;/.test(S.player) && !/writeSrt/.test(fnBody(S.player, "void savePartial")),
  [{ file: "player", find: "        host_.writeCache(S.key, recordOf(S));\n        S.dirty = false;\n        S.savedAt = host_.nowMs();\n        pbtrf::Summary sm;\n        sm.ok = true; sm.complete = false;", replace: "        writeSrts(S); host_.writeCache(S.key, recordOf(S));\n        S.dirty = false;\n        S.savedAt = host_.nowMs();\n        pbtrf::Summary sm;\n        sm.ok = true; sm.complete = false;", why: "every checkpoint of an unfinished film writes .srt files next to it" }]);

pin("F4", "wg WG-3", "the core writes a language's file from the MOST COMPLETE set holding it (bestSibling: complete > edited > coverage), never an empty one, and never shrinks it across a sound-track switch (the same-track keys + the record that wrote the file)",
  (S) => {
    const fileOf = (name) => { const at = S.player.indexOf(name); return at < 0 ? "" : S.player.slice(at, S.player.indexOf("\n    }\n", at) + 6); };
    const bs = fileOf("bool bestSibling(");
    return /const bool fromSib = bestSibling\(S, L, sib, &sibKey\);\s*const std::vector<tp::WireCue>& cues = fromSib \? sib\.cues : S\.cues;/.test(S.player)
        && /if \(pbtrf::countInLang\(cues, L\) == 0\)/.test(S.player)
        && /return std::make_tuple\(complete \? 1 : 0, edited \? 1 : 0, coverage\);/.test(S.player) && /if \(r > best\) \{ best = r; bestKey = k; \}/.test(bs)
        && /auto best = siblingRank\(S\.complete, S\.edited, pbtrf::coverage\(S\.complete, merged\(S\.confirmed, S\.ranges\)\)\);/.test(bs)
        // (U2: + S.models -- the candidates are the same track's AND the same models' analyses; another model's comes in
        //  through the registry's writer key, like another track's)
        && /pbtrf::siblingKeys\(S\.path, S\.size, S\.mtime, lang, \{ S\.langA, S\.langB \}, S\.audio\.ord, S\.models\)/.test(bs)
        && /const std::string writer = host_\.srtWriter\(S, lang\);/.test(bs) && /if \(writerExtra\) keys\.push_back\(writer\);/.test(bs)
        && /host_\.writeSrt\(S, L, cues, fromSib \? sibKey : S\.key, fromSib \? sib\.audioOrd : S\.audio\.ord,/.test(S.player)
        && /if \(audioOrd > 0\) k \+= "\\ta" \+ std::to_string\(audioOrd\);/.test(S.store)
        && /pbtrf::registryPut\(reg, srt, size, mtime, media, lang, key, ord\);/.test(S.bridge);
  },
  [{ file: "player", find: "return std::make_tuple(complete ? 1 : 0, edited ? 1 : 0, coverage);", replace: "return std::make_tuple(complete ? 1 : 0, coverage, edited ? 1 : 0);", why: "an edited set no longer outranks a bigger unedited one (the user's edits are overwritten)" },
   { file: "player", find: "if (pbtrf::countInLang(cues, L) == 0) {", replace: "if (false) {", why: "an empty file can be written for a language with no line yet" }]);

pin("F5", "wg WG-3", "the files are written within 30 s of the first cue they lack (tick), and at once on a pause / the end, a switch, the job's end and the app's end",
  (S) => {
    const fileOf = (name) => { const at = S.player.indexOf(name); return at < 0 ? "" : S.player.slice(at, S.player.indexOf("\n    }\n", at) + 6); };
    return /constexpr uint64_t kSrtEveryMs    = 30000;/.test(S.player) && /if \(S->srtDirty && now - S->srtDirtySince >= kSrtEveryMs\) writeSrts\(\*S\);/.test(S.player)
        && /if \(\(why == PosReason::Pause \|\| why == PosReason::Ended\) && s_\.set && s_\.set->srtDirty\) writeSrts\(\*s_\.set\);/.test(S.player)
        && /else if \(s_\.set && s_\.set->srtDirty\) writeSrts\(\*s_\.set\);/.test(fileOf("void endSession()"))
        && /if \(S\.srtDirty \|\| S\.editDirty\) writeSrts\(S\);/.test(S.player) && /void shutdown\(\) \{\s*flushFiles\(\);/.test(S.player);
  },
  [{ file: "player", find: "constexpr uint64_t kSrtEveryMs    = 30000;", replace: "constexpr uint64_t kSrtEveryMs    = 3000000;", why: "the files appear only at the end of the film (50 min instead of 30 s)" },
   { file: "player", find: "void shutdown() {\n        flushFiles();", replace: "void shutdown() {", why: "the app's end loses the files' last 30 s" }]);

pin("F6", "wg WG-3", "the bridge writes ONE language per call: its text of each cue (textInLang), tagged zh-CN / zh-TW, an identical file left alone",
  (S) => {
    const writer = between(S.bridge, "std::wstring Bridge::writeTranslatorSrt(", "\n}\n");
    return /pbsub::srt\(trLangCues\(cues, lang\), pbsub::Which::Dst\)/.test(writer) && /w16\(pbtrf::fileLangTag\(lang\)\)/.test(writer)
        && /have == bytes/.test(writer) && /if \(!pbtrf::textInLang\(c, lang, t\)\) continue;/.test(S.bridge);
  },
  [{ file: "bridge", find: "pbsub::srt(trLangCues(cues, lang), pbsub::Which::Dst)", replace: "pbsub::srt(trLangCues(cues, lang), pbsub::Which::Both)", why: "every language's file holds both languages (the user asked for one language per file)" }]);

pin("F7", "wg WG-3", "\"Show in folder\" selects every language's file together",
  (S) => /for \(const auto& f : player_->session\(\)\.set->srtFiles\) targets\.push_back/.test(S.bridge)
      && /SHOpenFolderAndSelectItems\(folder, static_cast<UINT>\(kids\.size\(\)\), kids\.data\(\), 0\)/.test(S.bridge),
  [{ file: "bridge", find: "for (const auto& f : player_->session().set->srtFiles) targets.push_back(w16(f.path));", replace: "if (!player_->session().set->srtFiles.empty()) targets.push_back(w16(player_->session().set->srtFiles.front().path));", why: "only the first language's file is selected" }]);

// ================= E: the engine's supervisor ================================================================================
pin("E1", "ui WC 4c [9]", "both write-failure paths (and the two others where the engine's pipe dies) read the helper's last words before declaring its death -- the message the user sees is the engine's own",
  (S) => count(S.mgr, /drainLastWords\(\);\s*(failCmds\(cmds, kFileEngineStopped\);\s*)?markDown\(\);/g) === 4,
  [{ file: "mgr", find: "// closed the pipe under this write must be reported as having finished it.\n                drainLastWords();\n", replace: "// closed the pipe under this write must be reported as having finished it.\n", why: "the write-failure path forgets the engine's last words (a finished job reported as lost)" },
   { file: "mgr", find: "drainLastWords();\n                failCmds(cmds, kFileEngineStopped);", replace: "failCmds(cmds, kFileEngineStopped);", why: "the pipe-broke-while-writing path fails the commands before reading the FCUEs / FDONE the engine had sent" }]);

// ================= L: SuVtitle as a Windows player (--play, one instance, the test instance) ================================
pin("L1", "wg WG-6", "playerSetDefault registers for the USER (TrAssoc entries under HKCU Software), tells Explorer, opens Settings on SuVtitle (fallback: Default apps)",
  (S) => /if \(type == L"playerSetDefault"\) \{ handleSetDefaultPlayer\(\); return; \}/.test(S.bridge)
      && /pbassoc::apply\(HKEY_CURRENT_USER, pbassoc::entries\(exe, root\), err\)/.test(S.bridge) && /pbassoc::notifyShell\(\);/.test(S.bridge)
      && /ShellExecuteW\(nullptr, L"open", pbassoc::kSettingsUri/.test(S.bridge) && /pbassoc::kSettingsFallback/.test(S.bridge),
  [{ file: "bridge", find: "pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, root), err)", replace: "pbassoc::apply(HKEY_LOCAL_MACHINE, pbassoc::entries(exe, root), err)", why: "registers machine-wide (needs admin, touches every user's profile)" }]);

pin("L2", "wg WG-6", "...a TEST instance registers under its OWN root (Software\\SuVtitle-Test\\<tag>), tells no one and opens no Settings",
  (S) => /const std::wstring root = test \? L"Software\\\\SuVtitle-Test\\\\" \+ pblaunch::testTag\(\) : std::wstring\(L"Software"\);/.test(S.bridge)
      && /if \(test\) \{[\s\S]{0,500}?return;\s*\}\s*pbassoc::notifyShell\(\);\s*const HINSTANCE r = ::ShellExecuteW/.test(S.bridge)
      && /const std::wstring regRoot = testInstance \? L"Software\\\\SuVtitle-Test\\\\" \+ pblaunch::testTag\(\) : std::wstring\(L"Software"\);/.test(S.main)
      && count(S.main, /if \(!testInstance\) pbassoc::notifyShell\(\);/g) === 2,
  [{ file: "bridge", find: 'const std::wstring root = test ? L"Software\\\\SuVtitle-Test\\\\" + pblaunch::testTag() : std::wstring(L"Software");', replace: 'const std::wstring root = std::wstring(L"Software");', why: "a test instance writes into the user's real file associations" },
   { file: "main", find: "if (!testInstance) pbassoc::notifyShell();\n        return 0;\n    }\n    if (args.registerPlayer)", replace: "pbassoc::notifyShell();\n        return 0;\n    }\n    if (args.registerPlayer)", why: "a test instance's --unregister-player broadcasts to the user's Explorer" }]);

pin("L3", "wg WG-6", "--play to a running SuVtitle goes through WM_COPYDATA (kCopyPlay, packPlay), with the foreground handed over (never from a test instance); then this process quits (0 sent / 2 not)",
  (S) => /cds\.dwData = pblaunch::kCopyPlay;/.test(S.main) && /pblaunch::packPlay\(args\.paths\)/.test(S.main)
      && /if \(pid && !testInstance\) ::AllowSetForegroundWindow\(pid\);/.test(S.main)
      && /SendMessageTimeoutW\(existing, WM_COPYDATA/.test(S.main) && /return sent \? 0 : 2;/.test(S.main),
  [{ file: "main", find: "if (pid && !testInstance) ::AllowSetForegroundWindow(pid);", replace: "if (pid) ::AllowSetForegroundWindow(pid);", why: "a test instance hands the user's foreground to a test window" },
   { file: "main", find: "return sent ? 0 : 2;", replace: "return 0;", why: "a forwarding that failed looks like success (Explorer shows nothing, the file never opens)" }]);

pin("L4", "wg WG-6", "...the running SuVtitle takes WM_COPYDATA only as kCopyPlay + a well-formed payload (unpackPlay), queues it, and plays it OFF the SendMessage (timer -> kWmPlayPending -> bringUp, showPlayer, playPaths)",
  (S) => /if \(!cds \|\| cds->dwData != pblaunch::kCopyPlay \|\| !pblaunch::unpackPlay\(cds->lpData, cds->cbData, paths\)\) return FALSE;/.test(S.main)
      && /::PostMessageW\(hwnd, kWmPlayPending, 0, 0\);/.test(S.main)
      && /case kWmPlayPending: \{[\s\S]{0,1200}bringUp\(\);[\s\S]{0,200}g_bridge->showPlayer\(\);\s*g_bridge->playPaths\(paths\);/.test(S.main),
  [{ file: "main", find: "|| !pblaunch::unpackPlay(cds->lpData, cds->cbData, paths)) return FALSE;", replace: ") return FALSE;", why: "a malformed WM_COPYDATA payload is taken (paths from anybody who can send a window message)" },
   { file: "main", find: "g_bridge->showPlayer();\n        g_bridge->playPaths(paths);", replace: "g_bridge->playPaths(paths);", why: "the window is not told to ask for the File view before the paths are played" }]);

pin("L5", "wg WG-6 review #11 #12", "...a burst of forwarders (Explorer's multi-select \"Open\") is ONE batch -- 300 ms after the last arrival -- in the natural order when several processes fed it; held while a file dialog of the player is up",
  (S) => /\+\+g_playSources;\s*if \(g_webview\) ::SetTimer\(hwnd, kPlayBatchTimer, 300, nullptr\);/.test(S.main)
      && /if \(g_bridge->dialogBusy\(\)\) \{ ::SetTimer\(hwnd, kPlayRetryTimer, 400, nullptr\); return 0; \}/.test(S.main)
      && /if \(g_playSources > 1\)\s*std::stable_sort\(paths\.begin\(\), paths\.end\(\),[\s\S]{0,160}::StrCmpLogicalW/.test(S.main)
      && /if \(wParam == kPlayBatchTimer \|\| wParam == kPlayRetryTimer\) \{[\s\S]{0,80}::KillTimer\(hwnd, wParam\);\s*::PostMessageW\(hwnd, kWmPlayPending, 0, 0\);/.test(S.main),
  [{ file: "main", find: "if (g_bridge->dialogBusy()) { ::SetTimer(hwnd, kPlayRetryTimer, 400, nullptr); return 0; }", replace: "", why: "a forwarded film replaces the session under an open file dialog" },
   { file: "main", find: "if (g_playSources > 1)", replace: "if (false)", why: "Episode 10 plays before Episode 2 (arrival order, not Explorer's)" }]);

pin("L6", "wg WG-6", "...no SuVtitle running: it starts, and plays the paths once the view exists (kWmPlayPending posted when the bridge has attached)",
  (S) => /if \(args\.play\) \{ g_pendingPlay = args\.paths; g_playRequested = true; g_playSources = 1; \}/.test(S.main)
      && /if \(g_playRequested \|\| !g_pendingPlay\.empty\(\)\) ::PostMessageW\(hwnd, kWmPlayPending, 0, 0\);/.test(S.main)
      && /case kWmPlayPending: \{\s*if \(!g_webview \|\| !g_bridge \|\| !g_playRequested\) return 0;/.test(S.main),
  [{ file: "main", find: "if (g_playRequested || !g_pendingPlay.empty()) ::PostMessageW(hwnd, kWmPlayPending, 0, 0);", replace: "", why: "a double-clicked film opens the window and then nothing (the paths wait for ever)" }]);

pin("L7", "wg WG-6 review #9", "...the Bridge loads the queue only in startup() -- called from attach() and the player's first use (showPlayer, playPaths), once -- never from its constructor",
  (S) => {
    const ctor = fnBody(S.bridge, "Bridge::Bridge");
    return /void Bridge::startup\(\) \{\s*if \(started_\) return;\s*started_ = true;/.test(S.bridge) && count(S.bridge, /player_->load\(q, r\);/g) === 1
        && !/player_->load\(/.test(ctor) && /void Bridge::showPlayer\(\) \{\s*startup\(\);/.test(S.bridge)
        && /void Bridge::playPaths\([^)]*\) \{\s*if \(!player_\) return;\s*startup\(\);/.test(S.bridge)
        && /startup\(\);\s*\/\/ the queue first \(review of WG #9: never on top of what is opened\)/.test(fnBody(S.bridge, "void Bridge::attach"));
  },
  [{ file: "bridge", find: "void Bridge::showPlayer() {\n    startup();", replace: "void Bridge::showPlayer() {", why: "showPlayer before the queue is loaded: the forwarded paths are replaced by the saved queue" }]);

pin("L8", "wg WG-6 review #16", "the TEST INSTANCE's isolation: the folders are compared as FILES (volume + id: an alias or a junction is caught; an unopenable folder counts as the same), and a test instance on the REAL profile exits 3 before anything is read or written",
  (S) => /inline bool sameDir\(const std::wstring& a, const std::wstring& b\)/.test(S.launch)
      && /same\(ad, rad\) \|\| same\(la, rla\)\s*\|\| sameDir\(ad, rad\) \|\| sameDir\(la, rla\)\) ::ExitProcess\(3\);/.test(S.launch)
      && /if \(!idOf\(a, x\) \|\| !idOf\(b, y\)\) return true;/.test(S.launch) && /if \(a\.badTag\) ::ExitProcess\(3\);/.test(S.launch)
      && /const bool testInstance = pblaunch::isTestInstance\(\);/.test(S.main),
  [{ file: "launch", find: "|| sameDir(ad, rad) || sameDir(la, rla)) ::ExitProcess(3);", replace: ") ::ExitProcess(3);", why: "a junction / 8.3 alias to the real profile is not caught: a test writes the user's real files" },
   { file: "launch", find: "if (!idOf(a, x) || !idOf(b, y)) return true;", replace: "if (!idOf(a, x) || !idOf(b, y)) return false;", why: "a folder that cannot be opened counts as 'different' (no isolation proven, the test goes on)" }]);

pin("L9", "wg WG-6", "the test instance has its own mutex and window class (it never joins nor disturbs the user's SuVtitle), is shown never activated, and never takes the foreground",
  (S) => /const std::wstring mutexName = pblaunch::suffixed\(kMutexName\);/.test(S.main) && /const std::wstring mainClass = pblaunch::suffixed\(kWindowClass\);/.test(S.main)
      && /\(g_quiet \? WS_EX_NOACTIVATE : 0\)/.test(S.main) && /if \(!g_quiet\) ::SetForegroundWindow\(g_hwnd\);/.test(S.main) && /if \(g_quiet\) show = SW_SHOWNOACTIVATE;/.test(S.main)
      && /if \(existing && !testInstance\) \{/.test(S.main) && /if \(pblaunch::isTestInstance\(\)\) translator_->setSpawnArgs\(L"--cpu"\);/.test(S.bridge),
  [{ file: "main", find: "const std::wstring mutexName = pblaunch::suffixed(kMutexName);", replace: "const std::wstring mutexName = kMutexName;", why: "a test instance takes the real single-instance mutex (and a second real launch is refused)" },
   { file: "bridge", find: 'if (pblaunch::isTestInstance()) translator_->setSpawnArgs(L"--cpu");', replace: "", why: "a test instance's engine takes the user's GPU" }]);

pin("L10", "wg WG-6", "--unregister-player (the uninstaller) and --register-player unregister / register and QUIT before the single-instance check: no window, no engine",
  (S) => {
    const mutex = S.main.indexOf("::CreateMutexW(nullptr, TRUE, mutexName.c_str())");
    const un = S.main.indexOf("if (args.unregister) {"), reg = S.main.indexOf("if (args.registerPlayer) {");
    return un > 0 && reg > un && mutex > reg && /pbassoc::unregister\(HKEY_CURRENT_USER, regRoot\);/.test(S.main);
  },
  [{ file: "main", find: "    if (args.unregister) {\n        pbassoc::unregister(HKEY_CURRENT_USER, regRoot);", replace: "    if (false) {\n        pbassoc::unregister(HKEY_CURRENT_USER, regRoot);", why: "the uninstaller starts a whole second SuVtitle instead of cleaning the registry" }]);

pin("L11", "wg WG-6", "the bridge checks every path of --play (TrLaunch.h classify): folders scanned, media played, a subtitle for what plays, the rest left out and said",
  (S) => /switch \(pblaunch::classify\(p\)\)/.test(S.bridge) && /if \(!folders\.empty\(\)\) startPlayerScan\(folders, media, subs, true, false\);/.test(S.bridge)
      && /not a video, audio or subtitle file \(or not found\)/.test(S.bridge),
  [{ file: "bridge", find: "switch (pblaunch::classify(p)) {", replace: "switch (pblaunch::Kind::Media) {", why: "every path of the command line is played without being looked at" }]);

pin("L12", "wg WG-6", "TrLaunch.h: only ABSOLUTE paths (drive or UNC, not \\\\?\\ or \\\\.\\), no wildcard / control character, existing, folder / media / subtitle -- and never executed (no ShellExecute / CreateProcess / system)",
  (S) => /if \(!drive && !unc\) return Kind::Refused;/.test(S.launch) && /c == L'\*' \|\| c == L'\?'/.test(S.launch) && /p\[2\] != L'\?' && p\[2\] != L'\.'/.test(S.launch)
      && !/ShellExecute|CreateProcess|system\(/.test(codeOnly(S.launch)),
  [{ file: "launch", find: "if (!drive && !unc) return Kind::Refused;", replace: "", why: "a relative path from the command line is resolved against whatever the working directory is" },
   { file: "launch", find: "const DWORD at = ::GetFileAttributesW(p.c_str());", replace: "const DWORD at = ::GetFileAttributesW(p.c_str()); ::ShellExecuteW(nullptr, L\"open\", p.c_str(), nullptr, nullptr, SW_SHOW);", why: "a path given to --play is EXECUTED" }]);

pin("L13", "wg WG-6", "TrAssoc.h: the ProgIDs run \"<exe>\" --play \"%1\" (the app's own command), every media type the player plays is registered as video or audio, and the audio list is a subset of the media list (32 types, 16 audio)",
  (S) => {
    const media = ((/mediaExts\(\) \{([\s\S]*?)nullptr/.exec(S.store) || ["", ""])[1].match(/L"([a-z0-9]+)"/g) || []).map((x) => x.slice(2, -1));
    const audio = ((/isAudioExt\(const std::wstring& e\) \{([\s\S]*?)nullptr/.exec(S.assoc) || ["", ""])[1].match(/L"([a-z0-9]+)"/g) || []).map((x) => x.slice(2, -1));
    return [/return L"\\"" \+ exe \+ L"\\" --play \\"%1\\"";/.test(S.assoc) && media.length === 32 && audio.length === 16 && audio.every((e) => media.indexOf(e) >= 0)
            && /for \(const wchar_t\* const\* p = pbtrf::mediaExts\(\); \*p; \+\+p\)\s*put\(std::wstring\(L"Classes\\\\\."\) \+ \*p/.test(S.assoc)
            && /kProgVideo = L"NEXTALE\.SuVtitle\.Video";/.test(S.assoc) && /kProgAudio = L"NEXTALE\.SuVtitle\.Audio";/.test(S.assoc),
            { media: media.length, audio: audio.length }];
  },
  [{ file: "assoc", find: 'L"mp3", L"wav", L"m4a"', replace: 'L"mp5", L"wav", L"m4a"', why: "an audio type that is not a media type (registered under a ProgID the player never opens)" },
   { file: "assoc", find: '--play \\"%1\\"', replace: '\\"%1\\"', why: "Explorer starts SuVtitle with the bare path (not --play): the forwarding never happens" }]);

// ================= U1 (02/11/2026, the user's requests -- docs\PROGRESS_U1.md): nothing analysed unless asked ==============
// (RUN by tests\cpp\player_smoke.cpp [6] [7] [U1]; these pin the wiring no headless test runs: the bridge's doors, the
//  core's one gate, and that nothing of 1.0.x's global switch survives)
pin("U1-1", "U1 (new)", "NOTHING IS ANALYSED UNLESS ASKED: the core's one door to a FILE for the playing item (ensureCurrentJob) asks wantedNow() -- the press (Session::wanted, set only by analyse() or open's analyseFirst) or \"Translate all\" (batchQ_) -- and every door of the bridge opens files with analyseFirst = false",
  (S) => {
    const ecj = fnBody(S.player, "void ensureCurrentJob"), wn = (/bool wantedNow\(\) const \{([^}]*)\}/.exec(S.player) || ["", ""])[1];
    const opens = [...S.bridge.matchAll(/player_->open\(([^;]*)\);/g)].map((m) => m[1]);
    // U3: the gate is analysisWanted() = the translation's wish (wantedNow) OR the transcript's (Transcribe, "Transcribe all")
    const aw = (/bool analysisWanted\(\) const \{([^}]*)\}/.exec(S.player) || ["", ""])[1];
    const tw = (/bool transcribeWantedNow\(\) const \{([^}]*)\}/.exec(S.player) || ["", ""])[1];
    return [/if \(!analysisWanted\(\)\) return;/.test(ecj) && ecj.indexOf("if (!analysisWanted()) return;") < ecj.indexOf("startJob(")
            && /return wantedNow\(\) \|\| transcribeWantedNow\(\);/.test(aw)
            && /return loaded_ && \(s_\.twanted \|\| tbatchQ_\.count\(s_\.itemId\) > 0\);/.test(tw)
            && count(codeOnly(S.player), /s_\.twanted = true;/g) === 1 && /s_\.twanted = true;/.test(fnBody(S.player, "void transcribe"))
            && /return loaded_ && \(s_\.wanted \|\| batchQ_\.count\(s_\.itemId\) > 0\);/.test(wn)
            && /n\.wanted = want;/.test(fnBody(S.player, "bool loadItem")) && /s_\.wanted = true;/.test(fnBody(S.player, "void analyse"))
            && count(codeOnly(S.player), /s_\.wanted = true;/g) === 1 && !/\btranslate_\b/.test(codeOnly(S.player))
            && opens.length === 4 && opens.every((a) => /^[^,]+, (false|d\.force)(, &[a-z.]+)?$/.test(a))
            && /startPlayerScan\(paths, \{\}, \{\}, play, false\);/.test(S.bridge) && /startPlayerScan\(folders, media, subs, play, false\);/.test(S.bridge)
            && /startPlayerScan\(folders, media, subs, true, false\);/.test(S.bridge),
            { opens }];
  },
  [{ file: "player", find: "        if (!analysisWanted()) return;                       // U1", replace: "        // U1", why: "a file loaded is analysed by itself (1.0.x's switch, always on)" },
   { file: "player", find: "    bool analysisWanted() const { return wantedNow() || transcribeWantedNow(); }", replace: "    bool analysisWanted() const { return loaded_; }", why: "every loaded file is analysed (U3: the either-wish gate always open)" },
   { file: "player", find: "    bool transcribeWantedNow() const { return loaded_ && (s_.twanted || tbatchQ_.count(s_.itemId) > 0); }", replace: "    bool transcribeWantedNow() const { return loaded_; }", why: "every loaded file is transcribed (so analysed)" },
   { file: "player", find: "        n.wanted = want;", replace: "        n.wanted = true;", why: "every opening counts as a press" },
   { file: "bridge", find: "else if (play) player_->open(paths, false);", replace: "else if (play) player_->open(paths, true);", why: "Open files… analyses what it opens" },
   { file: "player", find: "    bool wantedNow() const { return loaded_ && (s_.wanted || batchQ_.count(s_.itemId) > 0); }", replace: "    bool wantedNow() const { return loaded_; }", why: "every loaded file is wanted" }]);
pin("U1-2", "U1 (new)", "TRANSLATE ALL / CANCEL ALL: the bridge's two doors (a quality token, nothing else) call analyseAll / cancelAll; analyseAll queues every file not missing and not known complete (its failed and cancelled marks cleared); schedule() takes ONE job at a time, the playing file first (ensureCurrentJob), then the FIRST queued file in the LIST's order (items_, not the play order); Cancel all empties the queue",
  (S) => {
    const all = between(S.bridge, 'if (type == L"translatorFileAnalyseAll") {', 'if (type == L"translatorFileClose")');
    const aa = fnBody(S.player, "void analyseAll"), ca = fnBody(S.player, "void cancelAll"), sch = fnBody(S.player, "void schedule");
    return /jsonFindString\(json, L"quality", q\)\) player_->setQuality\(wideToUtf8\(q\)\);\s*player_->analyseAll\(\);/.test(all)
      && /if \(type == L"translatorFileCancelAll"\) \{ player_->cancelAll\(\); return; \}/.test(all)
      && /if \(it\.missing \|\| knownComplete\(it\)\) continue;\s*batchQ_\.insert\(it\.id\);\s*batchCancelled_\.erase\(it\.id\);\s*failedKeys_\.erase\(keyOf\(it\)\);/.test(aa)
      // (U2, the user's rule of 02/11: Cancel all stops the BATCH's jobs only -- U2-3 pins the rest)
      && /for \(uint64_t id : batchQ_\) batchCancelled_\.insert\(id\);\s*batchQ_\.clear\(\);\s*for \(uint64_t id : tbatchQ_\) tbatchCancelled_\.insert\(id\);[^\n]*\n\s*tbatchQ_\.clear\(\);\s*for \(Job& j : jobs_\) if \(j\.pre\) cancelJob\(j\);/.test(ca)
      && sch.indexOf("ensureCurrentJob(") < sch.indexOf("for (const Item& it : items_) {") && sch.indexOf("for (const Item& it : items_) {") > 0
      && /const bool tr = batchQ_\.count\(it\.id\) > 0, tx = tbatchQ_\.count\(it\.id\) > 0;[^\n]*\n\s*if \(\(!tr && !tx\) \|\| it\.id == curId\) continue;/.test(sch)
      && /for \(const Job& j : jobs_\) if \(j\.pre && !j\.cancelSent\) return;\s*\/\/ one at a time/.test(sch) && !/playOrder\(\)/.test(sch);
  },
  [{ file: "bridge", find: "        player_->analyseAll();\n", replace: "        player_->analyse(false);\n", why: "Translate all analyses the playing file only" },
   { file: "bridge", find: 'if (type == L"translatorFileCancelAll") { player_->cancelAll(); return; }', replace: 'if (type == L"translatorFileCancelAll") { return; }', why: "Cancel all does nothing" },
   { file: "player", find: "            batchCancelled_.erase(it.id);\n            failedKeys_.erase(keyOf(it));\n", replace: "            batchCancelled_.erase(it.id);\n", why: "a failed file is never asked again" },
   { file: "player", find: "        for (const Job& j : jobs_) if (j.pre && !j.cancelSent) return;                 // one at a time\n", replace: "", why: "the batch starts a second job while one runs" }]);
pin("U1-3", "U1 (new)", "NOTHING OF 1.0.x's SWITCH SURVIVES: the bridge no longer reads playerSet{translate}; player.txt's OPT translate is read and IGNORED (a 1.0.x file starts nothing) and never written; translatorFile.translate is THIS file's (wantedNow), the queue carries no global one",
  (S) => {
    const ps = between(S.bridge, 'if (type == L"playerSet") {', 'if (type == L"playerPos") {');
    return ps.length > 100 && !/L"translate"/.test(ps) && !/setTranslate/.test(S.bridge)
      && /if \(t\[1\] == "translate"\) \{ \/\* U1: [^*]*\*\/ \}/.test(S.player) && !/OPT\\ttranslate/.test(S.player)
      && /j\.kv\(L"translate", wantedNow\(\)\);/.test(S.player) && count(S.player, /j\.kv\(L"translate"/g) === 1;
  },
  [{ file: "bridge", find: "        bool b = false;\n        std::wstring rep;", replace: "        bool b = false;\n        if (jsonFindBool(json, L\"translate\", b) && b) player_->analyse(false);\n        std::wstring rep;", why: "the old switch's message still starts an analysis" },
   { file: "player", find: '    o += std::string("OPT\\trepeat\\t") + repeatName(q.repeat) + "\\n";', replace: '    o += "OPT\\ttranslate\\t1\\n";\n    o += std::string("OPT\\trepeat\\t") + repeatName(q.repeat) + "\\n";', why: "the old switch written back to player.txt" },
   { file: "player", find: '        j.kv(L"translate", wantedNow());', replace: '        j.kv(L"translate", true);', why: "every file said to be translated (the page waits for subtitles that are not coming)" }]);

// ================= U2 (02/11/2026, "Import model..."): the user's own models -- docs/PROGRESS_U2.md ===========================
pin("U2-1", "U2 (new)", "IMPORT MODEL...: the dialog runs in the APP (IFileOpenDialog owned by the player's window, filters *.bin;*.gguf / *.bin / *.gguf / *.*), the file's HEADER decides its kind (TrPipe.h modelFileKind, never the name) -- anything else refused in ONE notice; the page's two messages read a kind and an id only (no path, D1); a pick re-checks the file before it is chosen",
  (S) => {
    const imp = fnBody(S.bridge, "void Bridge::handleModelImport"), dlg = fnBody(S.bridge, "std::wstring Bridge::pickModelFile");
    const pick = fnBody(S.bridge, "void Bridge::handleModelPick");
    const doors = between(S.bridge, 'if (type == L"translatorModelImport")', 'if (type == L"translatorFileClose")');
    return /const std::wstring path = pickModelFile\(dialogOwner\(\)\);/.test(imp)
      && /const pbt::trpipe::ModelKind k = pbt::trpipe::modelFileKind\(path, &why\);\s*if \(k == pbt::trpipe::ModelKind::None\) \{\s*postTranslatorFileNotice\([^;]*;\s*return;\s*\}/.test(imp)
      && /if \(!pbapp::addImportedModel\(models_->m, kind, u\)\)/.test(imp) && /saveModels\(\);\s*applyModels\(\);\s*postModels\(\);/.test(imp)
      && /\{ L"AI models \(\*\.bin, \*\.gguf\)", L"\*\.bin;\*\.gguf" \}/.test(dlg) && /L"\*\.bin" \}/.test(dlg) && /L"\*\.gguf" \}/.test(dlg)
      && /\{ L"All files", L"\*\.\*" \}/.test(dlg) && /dlg->Show\(owner\)/.test(dlg) && /trDialogBusy_ = true;/.test(dlg)
      && /if \(type == L"translatorModelImport"\) \{ handleModelImport\(\); return; \}/.test(doors)
      && /jsonFindString\(json, L"kind", kind\);\s*jsonFindNumber\(json, L"id", id\);\s*handleModelPick\(kind, id\);/.test(doors)
      && !/L"path"|L"file"/.test(doors)
      && /const pbt::trpipe::ModelKind k = pbt::trpipe::modelFileKind\(w, &why\);/.test(pick) && /if \(k != want\) \{/.test(pick);
  },
  [{ file: "bridge", find: "    if (k == pbt::trpipe::ModelKind::None) {\n        postTranslatorFileNotice(L\"\\\"\" + leaf", replace: "    if (false) {\n        postTranslatorFileNotice(L\"\\\"\" + leaf", why: "a file that is no model (its header says so) imported anyway" },
   { file: "bridge", find: "pickModelFile(dialogOwner())", replace: "pickModelFile(nullptr)", why: "the dialog owned by no window (it can fall behind the player)" },
   { file: "bridge", find: 'L"*.bin;*.gguf" }', replace: 'L"*.*" }', why: "the dialog does not offer the model files first" },
   { file: "bridge", find: "        jsonFindNumber(json, L\"id\", id);\n        handleModelPick(kind, id);", replace: "        jsonFindNumber(json, L\"id\", id);\n        std::wstring file; jsonFindString(json, L\"file\", file);\n        handleModelPick(kind, id);", why: "the pick reads a FILE from the page" },
   { file: "bridge", find: "        if (k != want) {", replace: "        if (false) {", why: "a pick not checked again (a replaced or gone file chosen)" }]);
pin("U2-2", "U2 (new)", "app.txt REMEMBERS THEM: MODEL / MODELUSE (AppConfig.h) written through the AV-safe write and never rebuilt from an empty read, loaded at construction BEFORE the queue (the badges' keys); the EFFECTIVE choice = the chosen file when it is there (else the default); the menu on every `ready`; a model that failed forgets its choice only when it still is that one -- and never calls the core back",
  (S) => {
    const save = fnBody(S.bridge, "void Bridge::saveModels"), apply = fnBody(S.bridge, "void Bridge::applyModels");
    const fell = fnBody(S.bridge, "void Bridge::onModelFailed");
    return /models_ = std::make_unique<ModelsState>\(\);\s*loadModels\(\);\s*loadLiveConfig\(\);/.test(S.bridge)
      && /if \(!pbtrf::readFileAll\(cfg, before, 1ULL << 20\) && ::GetFileAttributesW\(cfg\.c_str\(\)\) != INVALID_FILE_ATTRIBUTES\) return;\s*const std::string after = pbapp::packModels\(before, models_->m\);\s*if \(after != before\) pbtrf::writeFileAvSafe\(cfg, after\);/.test(save)
      && /if \(!use\.empty\(\) && pbtrf::fileIdentity\(w, size, mtime, &isDir\) && !isDir\) \{\s*k\.path = use;\s*k\.ident = pbtrf::modelIdent\(w, size, mtime\);/.test(apply)
      && /player_->setModels\(pickOf\("asr"\), pickOf\("mt"\)\);/.test(apply)
      && /player_->view\("file"\);[^\n]*\n\s*postModels\(\);/.test(S.bridge)
      && /if \(pbapp::samePathCi\(models_->m\.use\(kind\), path\)\) \{\s*models_->m\.use\(kind\)\.clear\(\);\s*saveModels\(\);\s*\}/.test(fell)
      // (U2b re-point: the core says whether to say it -- `say`, once -- and which file a batch redoes)
      && !/player_|applyModels/.test(fell) && /b\.onModelFailed\(kind, path, sentence, say, redone\);/.test(S.bridge)
      && /inline std::string packModels\(/.test(S.appcfg) && /MODELUSE/.test(S.appcfg);
  },
  [{ file: "bridge", find: "    models_ = std::make_unique<ModelsState>();\n    loadModels();\n", replace: "    models_ = std::make_unique<ModelsState>();\n", why: "app.txt's models never read back (forgotten at every start)" },
   { file: "bridge", find: "return;\n    const std::string after = pbapp::packModels(before, models_->m);", replace: "(void)0;\n    const std::string after = pbapp::packModels(before, models_->m);", why: "an app.txt that could not be read rebuilt from nothing (the window's place and the strips lost)" },
   { file: "bridge", find: "    if (after != before) pbtrf::writeFileAvSafe(cfg, after);", replace: "    pbtrf::writeFileAvSafe(cfg, after);", why: "app.txt rewritten at every pick, unchanged or not (more antivirus windows)" },
   { file: "bridge", find: "if (!use.empty() && pbtrf::fileIdentity(w, size, mtime, &isDir) && !isDir) {", replace: "if (!use.empty()) {", why: "a chosen file that is gone still sent to the engine" },
   { file: "bridge", find: "            postModels();            // U2:", replace: "            // U2:", why: "a reloaded page never told the models" },
   { file: "bridge", find: "    if (pbapp::samePathCi(models_->m.use(kind), path)) {", replace: "    if (true) {", why: "a failure forgets a choice made since" },
   // (U2b re-point: the notice's condition is the core's `say` now)
   { file: "bridge", find: "    postModels();\n    if (say) postTranslatorFileNotice(", replace: "    applyModels();\n    postModels();\n    if (say) postTranslatorFileNotice(", why: "the bridge calls the core back from inside its onDone" }]);
pin("U2-3", "U2 (new)", "CANCEL ALL = THE BATCH ONLY (the user, 02/11): the queued files and the batch's jobs (pre); the PLAYING file's job only when the batch launched it (queued and not pressed); a playing file pressed keeps its job (the strip's Cancel stops it) -- and the chosen models reach every FILE (startJob names the set's paths) and every key (modelsTag_)",
  (S) => {
    const ca = fnBody(S.player, "void cancelAll"), sj = fnBody(S.player, "uint64_t startJob");
    return /const bool curBatch = loaded_ && batchQ_\.count\(s_\.itemId\) > 0;/.test(ca)
      && /for \(Job& j : jobs_\) if \(j\.pre\) cancelJob\(j\);/.test(ca)
      && /if \(curBatch && s_\.wanted\) \{\s*batchCancelled_\.erase\(s_\.itemId\);/.test(ca)
      && /\} else if \(curBatch && s_\.twanted\) \{\s*batchCancelled_\.erase\(s_\.itemId\);/.test(ca)
      && /\} else if \(\(curBatch \|\| \(curT && !s_\.wanted && !s_\.twanted\)\) && \(s_\.wire \|\| \(s_\.set && !s_\.set->complete\)\)\) \{[^\n]*\n\s*\/\/[^\n]*\n\s*if \(s_\.wire\) \{\s*if \(Job\* j = job\(s_\.wire\)\) cancelJob\(\*j\);/.test(ca)
      && /w\.asrModel = set->asrPath;/.test(sj) && /w\.mtModel = set->mtPath;/.test(sj)
      && count(S.player, /quality_, [a-zA-Z().]*ord, modelsTag_\)/g) === 4;
  },
  [{ file: "player", find: "        for (Job& j : jobs_) if (j.pre) cancelJob(j);          // the batches'", replace: "        for (Job& j : jobs_) cancelJob(j);          // the batches'", why: "Cancel all stops the pressed file's job too (U1's rule, before the user's)" },
   { file: "player", find: "        if (curBatch && s_.wanted) {", replace: "        if (curBatch && false) {", why: "a pressed playing file treated as the batch's" },
   { file: "player", find: "        w.mtModel = set->mtPath;\n", replace: "", why: "the FILE never names the imported translation model" },
   { file: "player", find: "quality_, audioFor(it).ord, modelsTag_);", replace: "quality_, audioFor(it).ord);", why: "the badges / the batch keyed without the models" }]);

// ================= U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md): the transcript's Word file, its dialog, its doors =====
// (RUN by tests\cpp\player_smoke.cpp [U3] -- the core's rules -- and tests\cpp\app_smoke.cpp [11] [12]; these pin the bridge's
//  wiring no headless test runs: where the Word file goes, what may be replaced, the dialog, the doors)
pin("U3-1", "U3 (new)", "THE TRANSCRIPT'S WORD FILE: a name is taken only when ABSENT or OURS for this media (the registry), created WITHOUT replace when it was absent, the file written before replaced in place only while still ours, registered as kind \"transcript\" with its media and analysis key; Documents' fallback never in a test instance",
  (S) => {
    const w = fnBody(S.bridge, "std::wstring Bridge::writeTranscriptDocx");
    // (FX re-point, 02/11/2026: "ours" is now for this media, this sound track AND this analysis -- registryOwnsFor, the user's F2
    //  rule; the choice takes the track's ord; the write says WHERE it failed -- &fail, the integrator's F4 criterion: FX-1 / FX-2)
    // (FX2 re-point, 03/11/2026: that test is named -- ownPrefer -- for F2's guard to skip it: FX2-1)
    return /return pbtrf::fileIdentity\(p, s, m\) && pbtrf::registryOwnsFor\(reg, p, s, m, S\.path, S\.key, S\.audio\.ord\);/.test(w)
      && /const bool ownPrefer = !t\.prefer\.empty\(\) && exists\(t\.prefer\) && ownsForThisAnalysis\(t\.prefer\);\s*if \(ownPrefer\) \{/.test(w)
      && /chooseTranscriptPath\(dirs\[k\], base, ext, sibling, exists, ownsForThisAnalysis, S\.audio\.ord\)/.test(w)
      && /const bool replace = exists\(p\);\s*\/\/ here only when it is ours for this analysis\s*pbtrf::WrittenId id;\s*if \(!pbtrf::writeFileAvSafe\(p, bytes, replace, &id, &fail\)\) \{/.test(w)
      && /trRegister\(p, s, m, S\.path, pbtrf::transcriptRegKind\(\), S\.key, S\.audio\.ord\);/.test(w)
      && /const std::wstring docs = pblaunch::isTestInstance\(\) \? std::wstring\(\) : trDocumentsTranscriptsDir\(\);/.test(w)
      && /inline const char\* transcriptRegKind\(\) \{ return "transcript"; \}/.test(S.store);
  },
  [{ file: "bridge", find: "    const bool ownPrefer = !t.prefer.empty() && exists(t.prefer) && ownsForThisAnalysis(t.prefer);", replace: "    const bool ownPrefer = !t.prefer.empty();", why: "the file written before is replaced although the user edited it since (or it is not ours any more)" },
   // (FX re-point: the line now carries `&fail`, and F4's refusal line sits between it and the folder's `break`)
   { file: "bridge", find: "            if (!pbtrf::writeFileAvSafe(p, bytes, replace, &id, &fail)) {", replace: "            if (!pbtrf::writeFileAvSafe(p, bytes, true, &id, &fail)) {", why: "a Word file that appeared meanwhile is clobbered" },
   { file: "bridge", find: "        trRegister(p, s, m, S.path, pbtrf::transcriptRegKind(), S.key, S.audio.ord);\n        return p;", replace: "        return p;", why: "the transcript is not registered: next time it counts as somebody else's and a second one is written beside it" },
   { file: "bridge", find: "    const std::wstring docs = pblaunch::isTestInstance() ? std::wstring() : trDocumentsTranscriptsDir();", replace: "    const std::wstring docs = trDocumentsTranscriptsDir();", why: "a test instance writes into the user's real Documents" }]);
pin("U3-2", "U3 (new)", "SAVE .DOCX...: the transcript AS IT IS (the core's transcriptForSave, a partial one says so), by default the file written for it while still ours, the overwrite prompt on, the file saved registered as ours (kind transcript) and kept by the core only for the SAME analysis (the dialog may let another file in); SUVTITLE_TEST_EXPORT read in a test instance only",
  (S) => {
    const h = fnBody(S.bridge, "void Bridge::handleTranscriptSave");
    return /if \(!player_ \|\| !player_->transcriptForSave\(t\)\)/.test(h)
      && /const DWORD tn = pblaunch::isTestInstance\(\) \? ::GetEnvironmentVariableW\(L"SUVTITLE_TEST_EXPORT", tv, MAX_PATH\) : 0;/.test(h)
      // (FX re-point, 02/11: a READ-ONLY file of its own is not the default -- F4's sentence promises a new one; the saved file keeps
      //  the timecode of its stamps -- transcriptSaved's `tc`, F1)
      && /FOS_OVERWRITEPROMPT/.test(h) && /if \(!t\.prefer\.empty\(\) && !trReadOnlyFile\(t\.prefer\)\) \{\s*startFolder = pbtrf::dirOf\(t\.prefer\);\s*name = pbtrf::leafOf\(t\.prefer\);/.test(h)
      && /trRegister\(target, s, m, mediaPath, pbtrf::transcriptRegKind\(\), key, ord\);/.test(h)
      && /player_->session\(\)\.set->key == key\)\s*player_->transcriptSaved\(target, s, m, t\.sig, t\.tc\);/.test(h)
      && /if \(type == L"translatorTranscriptSave"\) \{ handleTranscriptSave\(\); return; \}/.test(S.bridge);
  },
  [{ file: "bridge", find: "const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L\"SUVTITLE_TEST_EXPORT\", tv, MAX_PATH) : 0;\n        if (tn > 0 && tn < MAX_PATH) {\n            if (std::wstring(tv) == L\"cancel\")", replace: "const DWORD tn = ::GetEnvironmentVariableW(L\"SUVTITLE_TEST_EXPORT\", tv, MAX_PATH);\n        if (tn > 0 && tn < MAX_PATH) {\n            if (std::wstring(tv) == L\"cancel\")", why: "the product reads the test hook (an environment variable would write the transcript anywhere)" },
   { file: "bridge", find: "        if (player_ && player_->loaded() && player_->session().set && player_->session().set->key == key)\n", replace: "        if (player_ && player_->loaded())\n", why: "a file opened during the dialog takes the saved file as ITS transcript's" },
   { file: "bridge", find: "        trRegister(target, s, m, mediaPath, pbtrf::transcriptRegKind(), key, ord);\n", replace: "", why: "a transcript the user saved is not ours: a later automatic one goes beside it instead of replacing it" },
   { file: "bridge", find: "        startFolder = pbtrf::dirOf(t.prefer);\n        name = pbtrf::leafOf(t.prefer);", replace: "        startFolder = mediaFolder;\n        name = base + L\" - transcript.docx\";", why: "Save .docx... does not offer the file written for it" }]);
pin("U3-3", "U3 (new)", "THE TRANSCRIPT'S DOORS: tokens, numbers and ONE free text -- a speaker's NAME, read by JsonR (any character), the message bounded, checked by the core (TrTranscript.h nameOk: trimmed, <= 64 bytes, no control character) before it is kept, a number the transcript has; never a path; (U2b re-point: the user's answer 02/11) a rename schedules the Word file's IN-PLACE update (docxSoon), never a writeDocx (a new name)",
  (S) => {
    const doors = between(S.bridge, 'if (type == L"translatorTranscribe") {', 'if (type == L"translatorModelImport")');
    const rn = fnBody(S.player, "bool renameSpeaker");
    return /player_->transcribe\(force\);/.test(doors) && /player_->transcriptCancel\(id > 0 \? \(uint64_t\)id : 0\);/.test(doors)
      && /player_->transcribeAll\(\);/.test(doors)
      && /if \(json\.size\(\) > 4096 \|\| !pbjson::parse\(json, v, 4096\) \|\| !v\.isObj\(\) \|\| !v\.u64\(L"jobId", gen\) \|\| !v\.u64\(L"id", id\)/.test(doors)
      && /lv->kind != pbjson::Value::Kind::Str/.test(doors) && !/L"path"|L"file"|L"folder"/.test(doors)
      && /if \(!pbtrs::nameOk\(label, clean, err\)\)/.test(rn) && /if \(id == 0 \|\| id > V\.speakers\) return false;/.test(rn)
      // (U2b re-point: the user's answer 02/11 -- the Word file FOLLOWS a rename, in place, after the burst: docxSoon)
      && /host_\.writeVoices\(s_\.set->key, V\);\s*docxSoon\(s_\.set\);/.test(rn) && !/writeDocx/.test(rn);
  },
  [{ file: "player", find: "        if (!pbtrs::nameOk(label, clean, err)) {", replace: "        clean = label; if (false) {", why: "a typed name kept unchecked (control characters, any length)" },
   { file: "player", find: "        if (id == 0 || id > V.speakers) return false;", replace: "", why: "a speaker number the transcript does not have is renamed" },
   { file: "bridge", find: "        if (json.size() > 4096 || !pbjson::parse(json, v, 4096)", replace: "        if (!pbjson::parse(json, v, 16u << 20)", why: "an unbounded message parsed" },
   // (U2b re-point: the user's answer 02/11 -- two mutants: the rewrite through writeDocx (it may pick a NEW name), and none at all)
   { file: "player", find: "        host_.writeVoices(s_.set->key, V);\n        docxSoon(s_.set);", replace: "        host_.writeVoices(s_.set->key, V);\n        writeDocx(*s_.set);", why: "a rename writes the Word file through writeDocx (a NEW name when it was edited: never touched, the user's rule)" },
   { file: "player", find: "        host_.writeVoices(s_.set->key, V);\n        docxSoon(s_.set);", replace: "        host_.writeVoices(s_.set->key, V);", why: "a rename never updates the Word file (the user's answer, 02/11: it must, while SuVtitle's and unchanged)" }]);

// ================= U2b (02/11/2026, removing an imported model, G2 -- docs/PROGRESS_U2b.md) ==================================
// (RUN by tests\cpp\player_smoke.cpp [U2b] and app_smoke.cpp [13] -- the core's and app.txt's rules -- and by the real-app kit
//  scratchpad\u2b\real; these pin the bridge's wiring no headless test runs: the door, the Recycle Bin, the in-place rewrite)
pin("U2b-1", "U2b (new; fix round 02/11: decision 3 reversed)", "THE DOOR: translatorModelRemove reads a kind, an id and a flag (recycle) -- never a path; handleModelRemove takes THE PATH FROM ITS OWN LIST (app.txt, the kind's id-th imported file), refuses id 0 (SuVtitle's own) and an id it does not have, refuses a model an analysis uses (the core's modelInUse). \"Remove from the list\" = the line, whatever the file. \"Remove and delete the file\" = the RECYCLE BIN FIRST; the line (removeImportedModel + saveModels: the AV-safe write, applyModels, the menu) only when the file WENT and is truly gone, was TRULY gone already (not found in a folder that IS there), or is one of SuVtitle's own; any other outcome changes NOTHING (still listed, still chosen): the menu posted again, one sentence, return before the line",
  (S) => {
    const door = between(S.bridge, 'if (type == L"translatorModelRemove") {', 'if (type == L"translatorFileClose")');
    const h = fnBody(S.bridge, "void Bridge::handleModelRemove"), tg = fnBody(S.bridge, "static bool trTrulyGone");
    const iBin = h.indexOf("trRecycleFile(dialogOwner(), w, why)"), iLine = h.indexOf("pbapp::removeImportedModel(");
    return /jsonFindString\(json, L"kind", kind\);\s*jsonFindNumber\(json, L"id", id\);\s*if \(json\.find\(L"\\"recycle\\""\) != std::wstring::npos\) jsonFindBool\(json, L"recycle", recycle\);\s*handleModelRemove\(kind, id, recycle\);/.test(door)
      && !/L"path"|L"file"|L"folder"|L"name"/.test(door)
      && /if \(id < 1 \|\| id > static_cast<long long>\(list\.size\(\)\)\) \{ postModels\(\); return; \}/.test(h)
      && /const std::string path = list\[static_cast<size_t>\(id - 1\)\];/.test(h)
      && /if \(player_->modelInUse\(path\)\) \{/.test(h)
      // (U2b fix round, the director 02/11 -- the user: never half done) the Recycle Bin FIRST, the line ONCE and after it
      && iBin > 0 && iLine > iBin && count(h, /removeImportedModel\(/g) === 1 && count(h, /saveModels\(\);/g) === 1
      && ordered(h, ["if (recycle) {", "trRecycleFile(dialogOwner(), w, why)", "if (after.empty()) {", "postModels();",
                     "stays in the list: try again, or choose Remove from the list.", "return;",
                     "pbapp::removeImportedModel(models_->m, kind, path);", "saveModels();", "applyModels();", "postModels();"])
      && /if \(after\.empty\(\)\) \{[^}]*postModels\(\);[^}]*stays in the list: try again, or choose Remove from the list\."\);\s*return;\s*\}/.test(h)
      && /if \(at == INVALID_FILE_ATTRIBUTES && trTrulyGone\(w, err\)\) \{/.test(h)
      && /if \(r != 0 && !trTrulyGone\(w, errAfter\)\)/.test(h)
      && /else if \(r == 1\) after = L" Its file is in the Recycle Bin\.";/.test(h) && /else if \(r == 2\) after = /.test(h)
      && /if \(e != ERROR_FILE_NOT_FOUND\) return false;/.test(tg)
      && /return a != INVALID_FILE_ATTRIBUTES && \(a & FILE_ATTRIBUTE_DIRECTORY\) != 0;/.test(tg)
      && /inline bool removeImportedModel\(/.test(S.appcfg);
  },
  [{ file: "bridge", find: "if (id < 1 || id > static_cast<long long>(list.size())) { postModels(); return; }", replace: "if (id < 0 || id > static_cast<long long>(list.size())) { postModels(); return; }", why: "id 0 (SuVtitle's own model) reaches the list lookup" },
   { file: "bridge", find: "    if (player_->modelInUse(path)) {", replace: "    if (false) {", why: "a model an analysis uses is removed (and its file sent away) under it" },
   { file: "bridge", find: "        jsonFindNumber(json, L\"id\", id);\n        if (json.find(L\"\\\"recycle\\\"\")", replace: "        jsonFindNumber(json, L\"id\", id);\n        std::wstring file; jsonFindString(json, L\"file\", file);\n        if (json.find(L\"\\\"recycle\\\"\")", why: "the door reads a FILE from the page" },
   { file: "bridge", find: "    if (recycle) {\n        std::wstring why;\n        const DWORD at = ::GetFileAttributesW(w.c_str());", replace: "    if (true) {\n        std::wstring why;\n        const DWORD at = ::GetFileAttributesW(w.c_str());", why: "\"Remove from the list\" sends the file away too" },
   { file: "bridge", find: "    pbapp::removeImportedModel(models_->m, kind, path);\n    saveModels();\n", replace: "    pbapp::removeImportedModel(models_->m, kind, path);\n", why: "the removal never reaches app.txt (the model is back at the next start)" },
   // (U2b fix round, the director 02/11: decision 3 reversed -- each must be CAUGHT)
   { file: "bridge", find: "    std::wstring after;                                      // what the notice says of the file (asked to delete it)\n", replace: "    pbapp::removeImportedModel(models_->m, kind, path);\n    saveModels();\n    std::wstring after;\n", why: "the line removed before the recycle (a file Windows keeps leaves an orphan behind)" },
   { file: "bridge", find: "try again, or choose Remove from the list.\");\n            return;\n        }", replace: "try again, or choose Remove from the list.\");\n        }", why: "the line removed after a failed recycle (the model gone from the list, its file still there)" },
   { file: "bridge", find: "    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;", replace: "    (void)a;\n    return true;", why: "not-found with a missing parent read as gone (an unplugged drive's model leaves the list)" },
   { file: "bridge", find: "        if (at == INVALID_FILE_ATTRIBUTES && trTrulyGone(w, err)) {", replace: "        if (at == INVALID_FILE_ATTRIBUTES && err == ERROR_FILE_NOT_FOUND) {", why: "every not-found read as gone (the parent folder never asked)" },
   { file: "bridge", find: "            if (r != 0 && !trTrulyGone(w, errAfter))", replace: "            if (false)", why: "the shell's \"gone\" trusted while the file's drive left meanwhile (the line removed, the file still on it)" },
   { file: "bridge", find: "            else if (r == 1) after = L\" Its file is in the Recycle Bin.\";", replace: "            else after = L\" Its file is in the Recycle Bin.\";", why: "a failed recycle said as a success (r 0 taken for the Recycle Bin)" }]);
pin("U2b-2", "U2b (new)", "THE RECYCLE BIN, NEVER A PERMANENT DELETE OF OURS: trRecycleFile = IFileOperation owned by the player's window, FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI | FOFX_RECYCLEONDELETE (Windows' own warning decides when it cannot recycle), DeleteItem + PerformOperations, the sink's PostDeleteItem telling a recycle from a destruction; no DeleteFile / std::filesystem::remove / SHFileOperation on a model, and none of the last two anywhere",
  (S) => {
    const r = fnBody(S.bridge, "static int trRecycleFile"), h = fnBody(S.bridge, "void Bridge::handleModelRemove");
    const code = codeOnly(S.bridge);
    return /::CoCreateInstance\(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS\(&op\)\)/.test(r)
      && /op->SetOwnerWindow\(owner\)/.test(r)
      && /op->SetOperationFlags\(FOF_ALLOWUNDO \| FOF_NOCONFIRMATION \| FOF_WANTNUKEWARNING \| FOF_NOERRORUI\s*\| FOFX_RECYCLEONDELETE\)/.test(r)
      && /op->DeleteItem\(item, nullptr\)/.test(r) && /op->PerformOperations\(\)/.test(r)
      && /recycled = SUCCEEDED\(hrDelete\) && newlyCreated != nullptr;/.test(S.bridge)
      && /trRecycleFile\(dialogOwner\(\), w, why\)/.test(h)
      && !/DeleteFile|remove\(|SHFileOperation|MoveFile/.test(codeOnly(r) + codeOnly(h))
      && !/SHFileOperation|std::filesystem::remove|filesystem::remove/.test(code)
      // (U2b fix round, the director 02/11) a drive or folder that cannot be reached is SAID so -- in the why (after "in use",
      // before "you kept it") and before any recycle (handleModelRemove: the file's own attributes)
      && ordered(r, ["if (trFileHeld(path, DELETE)) why = L\"it is in use by another program.\";",
                     "else if (trUnreachable(HRESULT_FACILITY(e) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(e)) : 0))",
                     "why = L\"its drive or folder cannot be reached.\";", "else if (aborted ||"])
      && /why = trUnreachable\(err\) \? std::wstring\(L"its drive or folder cannot be reached\."\)/.test(h)
      && ["ERROR_PATH_NOT_FOUND", "ERROR_INVALID_DRIVE", "ERROR_NOT_READY", "ERROR_BAD_NETPATH", "ERROR_DEV_NOT_EXIST", "ERROR_NETNAME_DELETED",
          "ERROR_BAD_NET_NAME", "ERROR_NETWORK_UNREACHABLE"].every((c) => new RegExp("e == " + c + "\\b").test(fnBody(S.bridge, "static bool trUnreachable")));
  },
  [{ file: "bridge", find: "op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION", replace: "op->SetOperationFlags(FOF_NOCONFIRMATION", why: "no undo: the file DESTROYED instead of recycled" },
   { file: "bridge", find: "FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI", replace: "FOF_NOCONFIRMATION | FOF_NOERRORUI", why: "no nuke warning: a file that cannot be recycled is destroyed without Windows asking" },
   { file: "bridge", find: "                                                  | FOFX_RECYCLEONDELETE);", replace: "                                                  );", why: "IFileOperation not told to recycle (a plain delete)" },
   { file: "bridge", find: "    if (SUCCEEDED(hr)) hr = op->PerformOperations();", replace: "    if (SUCCEEDED(hr)) hr = ::DeleteFileW(path.c_str()) ? S_OK : E_FAIL;", why: "the model's file DELETED (gone for good) instead of recycled" },
   { file: "bridge", find: "    if (SUCCEEDED(hr)) hr = op->SetOwnerWindow(owner);", replace: "    if (SUCCEEDED(hr)) hr = op->SetOwnerWindow(nullptr);", why: "Windows' warning owned by no window (it can fall behind the player)" },
   // (U2b fix round, the director 02/11: the drives and folders that cannot be reached)
   { file: "bridge", find: "    return e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_DRIVE", replace: "    return e == ERROR_INVALID_DRIVE", why: "a missing parent folder (an unplugged drive's path) not said as unreachable" },
   { file: "bridge", find: " || e == ERROR_BAD_NET_NAME", replace: "", why: "an offline share not said as unreachable" },
   { file: "bridge", find: "    else if (trUnreachable(HRESULT_FACILITY(e) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(e)) : 0))\n        why = L\"its drive or folder cannot be reached.\";\n", replace: "", why: "a recycle that failed because the drive left is said as \"you kept it\" or an error code" },
   { file: "bridge", find: "            why = trUnreachable(err) ? std::wstring(L\"its drive or folder cannot be reached.\")", replace: "            why = false ? std::wstring(L\"its drive or folder cannot be reached.\")", why: "a model on an unplugged drive: \"cannot reach it (error 3)\" instead of the plain sentence" }]);
pin("U2b-3", "U2b (new)", "G2 (the user's answer 02/11): a rename's Word file is rewritten IN PLACE only -- the file written for it (t.prefer), only while the registry says it is OURS for this media, never chooseTranscriptPath / Documents; a refusal because it is OPEN elsewhere says so (a sharing / lock violation for WRITE), and the file written is registered again; Save .docx... never offers a file the user edited (a new name)",
  (S) => {
    const rw = fnBody(S.bridge, "bool Bridge::rewriteTranscriptDocx"), sv = fnBody(S.bridge, "void Bridge::handleTranscriptSave");
    // (FX re-point, 02/11/2026: ownership per ANALYSIS -- registryOwnsFor, the user's F2 rule; the refusal said by the integrator's
    //  mechanical criterion -- WriteFail::Target -> trDocxRefusal, whose held sentence is this pin's (FX-1 pins the function); Save's
    //  default name takes the track's ord)
    return /if \(t\.prefer\.empty\(\)\) return false;/.test(rw)
      && /if \(!pbtrf::fileIdentity\(t\.prefer, s0, m0\) \|\| !pbtrf::registryOwnsFor\(reg, t\.prefer, s0, m0, S\.path, S\.key, S\.audio\.ord\)\)\s*return false;/.test(rw)
      && /pbtrf::writeFileAvSafe\(t\.prefer, bytes, true, &id, &fail\)/.test(rw)
      && /why = fail == pbtrf::WriteFail::Target \? trDocxRefusal\(t\.prefer\) : std::wstring\(L"The Word file could not be updated\."\);/.test(rw)
      && /case pbtrf::TargetRefusal::Held:\s*return L"The Word file is open in another program: close it to update it\.";/.test(S.bridge)
      && /trRegister\(t\.prefer, s, m, S\.path, pbtrf::transcriptRegKind\(\), S\.key, S\.audio\.ord\);/.test(rw)
      && !/chooseTranscriptPath|trDocumentsTranscriptsDir|ensureDir/.test(rw)
      && /return e == ERROR_SHARING_VIOLATION \|\| e == ERROR_LOCK_VIOLATION;/.test(S.bridge)
      && /bool rewriteTranscript\([\s\S]{0,160}return b\.rewriteTranscriptDocx\(set, t, why, size, mtime\);/.test(S.bridge)
      && /const std::wstring p = pbtrf::chooseTranscriptPath\(mediaFolder, base, pbtrf::extOf\(S\.name\), sibling, exists, owns, ord\);\s*if \(!p\.empty\(\)\) name = pbtrf::leafOf\(p\);/.test(sv);
  },
  [{ file: "bridge", find: "    if (!pbtrf::fileIdentity(t.prefer, s0, m0) || !pbtrf::registryOwnsFor(reg, t.prefer, s0, m0, S.path, S.key, S.audio.ord))\n        return false;", replace: "    (void)s0; (void)m0; (void)reg;", why: "a Word file that is not SuVtitle's any more is overwritten by a rename" },
   { file: "bridge", find: "        why = fail == pbtrf::WriteFail::Target ? trDocxRefusal(t.prefer) : std::wstring(L\"The Word file could not be updated.\");", replace: "        why = std::wstring(L\"The Word file could not be updated.\");", why: "a file open in Word is never said (the tab says nothing useful)" },
   { file: "bridge", find: "    trRegister(t.prefer, s, m, S.path, pbtrf::transcriptRegKind(), S.key, S.audio.ord);\n    return true;", replace: "    return true;", why: "the rewritten file is not registered again: the next rename finds it \"not ours\"" },
   { file: "bridge", find: "            if (!p.empty()) name = pbtrf::leafOf(p);", replace: "            (void)p;", why: "Save .docx... offers the name of a file the user edited (Windows' prompt invites the overwrite)" }]);
pin("U2b-4", "U2b (new)", "SuVtitle's OWN FILES ARE NEVER SENT AWAY: \"Remove and delete the file\" on a model imported from one of SuVtitle's own folders -- the app's (the bundle), the dev payload (SUVTITLE_PAYLOAD), engine.txt's MODELS -- removes it from the list only, and says so; compared on FINAL paths (a junction or an alias of the folder is the folder), case-insensitively, on a folder boundary",
  (S) => {
    const h = fnBody(S.bridge, "void Bridge::handleModelRemove"), o = fnBody(S.bridge, "static bool trOwnModelFile");
    const fp = fnBody(S.bridge, "static std::wstring trFinalPath");
    return ordered(h, ["if (recycle) {", "} else if (trOwnModelFile(w, appDataPath(L\"engine.txt\"))) {", "it stays where it is (SuVtitle never deletes its own files).", "trRecycleFile(dialogOwner(), w, why)"])
      && /const std::wstring file = trFinalPath\(path\);/.test(o) && /std::wstring f = trFinalPath\(d\);/.test(o)
      && /::GetModuleFileNameW\(nullptr, exe, MAX_PATH\)/.test(o) && /if \(cut != std::wstring::npos\) dirs\.push_back\(e\.substr\(0, cut\)\);/.test(o)
      && /::GetEnvironmentVariableW\(L"SUVTITLE_PAYLOAD", env, 2048\)/.test(o) && /if \(m > 0 && m < 2048\) dirs\.push_back\(std::wstring\(env, m\)\);/.test(o)
      && /l\.compare\(0, 7, "MODELS\\t"\) == 0\) dirs\.push_back\(utf8ToWide\(l\.substr\(7\)\.c_str\(\)\)\);/.test(o)
      && /if \(f\.back\(\) != L'\\\\'\) f \+= L'\\\\';/.test(o)
      && /::CompareStringOrdinal\(file\.c_str\(\), static_cast<int>\(f\.size\(\)\), f\.c_str\(\), static_cast<int>\(f\.size\(\)\), TRUE\) == CSTR_EQUAL/.test(o)
      && /FILE_FLAG_BACKUP_SEMANTICS, nullptr\);/.test(fp) && /::GetFinalPathNameByHandleW\(h, &out\[0\]/.test(fp);
  },
  [{ file: "bridge", find: "        } else if (trOwnModelFile(w, appDataPath(L\"engine.txt\"))) {", replace: "        } else if (false) {", why: "a model imported from SuVtitle's own folder goes to the Recycle Bin (the default models, the Live presets gone)" },
   { file: "bridge", find: "        if (cut != std::wstring::npos) dirs.push_back(e.substr(0, cut));", replace: "        (void)cut;", why: "the app's own folder (the bundle's models) not counted" },
   { file: "bridge", find: "    if (m > 0 && m < 2048) dirs.push_back(std::wstring(env, m));", replace: "    (void)m;", why: "the dev payload (SUVTITLE_PAYLOAD) not counted" },
   { file: "bridge", find: "l.compare(0, 7, \"MODELS\\t\") == 0) dirs.push_back(", replace: "l.compare(0, 7, \"MODELZ\\t\") == 0) dirs.push_back(", why: "engine.txt's MODELS folder not counted" },
   { file: "bridge", find: "    const std::wstring file = trFinalPath(path);", replace: "    const std::wstring file = path;", why: "the file compared by its raw path (a junction or an alias of an own folder slips through)" },
   { file: "bridge", find: "static_cast<int>(f.size()), TRUE) == CSTR_EQUAL", replace: "static_cast<int>(f.size()), FALSE) == CSTR_EQUAL", why: "compared with its case (Windows' paths are not)" },
   { file: "bridge", find: "        if (f.back() != L'\\\\') f += L'\\\\';\n", replace: "", why: "no folder boundary: a file of \"<app> 2\\\" counted as the app's own, or the folder missed" },
   { file: "bridge", find: "                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);\n    if (h == INVALID_HANDLE_VALUE) return std::wstring();\n    std::wstring out(1024", replace: "                             FILE_ATTRIBUTE_NORMAL, nullptr);\n    if (h == INVALID_HANDLE_VALUE) return std::wstring();\n    std::wstring out(1024", why: "a FOLDER cannot be opened without backup semantics: no own folder ever resolved" }]);

// ================= FX (02/11/2026, the adversarial review's fix round -- docs/PROGRESS_FIX.md) ==================================
// (RUN by tests\cpp\app_smoke.cpp [FX] -- WriteFail / targetRefusal on REAL files, the track's names, the ownership -- and by
//  tests\cpp\player_smoke.cpp [FX] -- the core, against a host that follows these same rules; these pin the bridge's wiring)
pin("FX-1", "FX F4 (the integrator's criterion)", "A WORD FILE THAT REFUSED TO BE REPLACED IS NEVER A REFUSING FOLDER: writeTranscriptDocx -- the file written before (ours), and in the naming loop an existing file of ours -- returns with trDocxRefusal's sentence when the write failed AFTER its .tmp was written (WriteFail::Target), before any other name or Documents; Documents only after the folder itself refused the .tmp (WriteFail::Folder); trDocxRefusal = held (G2's sentence: a sharing / lock violation for WRITE, TrFileStore.h targetRefusal) / read-only (its own sentence, never \"another program\") / else \"could not be updated\"; Save .docx... never offers a read-only file",
  (S) => {
    const w = fnBody(S.bridge, "std::wstring Bridge::writeTranscriptDocx"), d = fnBody(S.bridge, "static std::wstring trDocxRefusal");
    const sv = fnBody(S.bridge, "void Bridge::handleTranscriptSave"), tr = fnBody(S.store, "inline TargetRefusal targetRefusal");
    const iPrefer = w.indexOf("if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }");
    const iLoop = w.indexOf("if (replace && fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(p); return std::wstring(); }");
    const iBreak = w.indexOf("break;                                    // this folder refuses the write"), iDocs = w.indexOf("const std::wstring docs = ");
    return [iPrefer > 0 && iLoop > 0 && iPrefer < iDocs && iLoop < iBreak
      && /if \(pbtrf::writeFileAvSafe\(t\.prefer, bytes, true, &id, &fail\)\) return finish\(t\.prefer, id\);/.test(w)
      && /case pbtrf::TargetRefusal::Held:\s*return L"The Word file is open in another program: close it to update it\.";/.test(d)
      && /case pbtrf::TargetRefusal::ReadOnly: return L"The Word file is read-only: it is left as it is\. Save \.docx\\x2026 writes a new one\.";/.test(d)
      && /default:\s*return L"The Word file could not be updated\.";/.test(d)
      && /if \(e == ERROR_SHARING_VIOLATION \|\| e == ERROR_LOCK_VIOLATION\) return TargetRefusal::Held;/.test(tr)
      && /if \(a != INVALID_FILE_ATTRIBUTES && \(a & FILE_ATTRIBUTE_READONLY\)\) return TargetRefusal::ReadOnly;/.test(tr)
      && /if \(h == INVALID_HANDLE_VALUE\) return failed\(WriteFail::Folder\);/.test(S.store)
      && /if \(!t\.prefer\.empty\(\) && !trReadOnlyFile\(t\.prefer\)\) \{/.test(sv) && /registryOwnsFor\(reg, p, s, m, mediaPath, key, ord\) && !trReadOnlyFile\(p\)/.test(sv),
      { iPrefer, iLoop, iBreak, iDocs }];
  },
  [{ file: "bridge", find: "        if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }\n", replace: "", why: "a Word file OPEN in Word (the .tmp written beside it) falls to the naming loop and to Documents with \"the media's folder could not be written to\"" },
   { file: "bridge", find: "                if (replace && fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(p); return std::wstring(); }   // F4\n", replace: "", why: "a held Word file of ours met in the naming loop breaks to Documents" },
   { file: "bridge", find: "    case pbtrf::TargetRefusal::ReadOnly: return L\"The Word file is read-only: it is left as it is. Save .docx\\x2026 writes a new one.\";", replace: "    case pbtrf::TargetRefusal::ReadOnly: return L\"The Word file is open in another program: close it to update it.\";", why: "a READ-ONLY Word file said \"open in another program\"" },
   { file: "bridge", find: "    case pbtrf::TargetRefusal::Held:     return L\"The Word file is open in another program: close it to update it.\";", replace: "    case pbtrf::TargetRefusal::Held:     return L\"The Word file could not be updated.\";", why: "a file open in Word never said as such" },
   { file: "store", find: "    if (h == INVALID_HANDLE_VALUE) return failed(WriteFail::Folder);", replace: "    if (h == INVALID_HANDLE_VALUE) return failed(WriteFail::Target);", why: "a folder that refuses the .tmp (a read-only card) taken for a file that refuses: never Documents for it" },
   { file: "store", find: "    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return TargetRefusal::Held;\n", replace: "", why: "a file held by Word said read-only or \"could not be updated\"" },
   { file: "bridge", find: "    if (!t.prefer.empty() && !trReadOnlyFile(t.prefer)) {", replace: "    if (!t.prefer.empty()) {", why: "Save .docx... offers a READ-ONLY file (its sentence promised a new one)" }]);
pin("FX-2", "FX F2 (the user's rule 02/11)", "ONE WORD FILE PER MEDIA AND SOUND TRACK: an automatic write replaces a file only when the registry says SuVtitle wrote it for this media, this track AND this analysis (registryOwnsFor -- an entry with no key owns nothing), in the track's own names (chooseTranscriptPath with its ord); ANOTHER analysis of the track holding its Word file -> nothing written (registryTrackDocx, before any name); G2 the same ownership; Save .docx... never offers another analysis' file and names the track's own; PlayerHost::docxWriter answers the core from the registry",
  (S) => {
    const w = fnBody(S.bridge, "std::wstring Bridge::writeTranscriptDocx"), rw = fnBody(S.bridge, "bool Bridge::rewriteTranscriptDocx");
    const sv = fnBody(S.bridge, "void Bridge::handleTranscriptSave"), dw = fnBody(S.bridge, "std::string Bridge::PlayerHost::docxWriter");
    const own = fnBody(S.store, "inline bool registryOwnsFor");
    const iBlock = w.indexOf("const std::string otherKey = pbtrf::registryTrackDocx(reg, S.path, S.audio.ord, exists);");
    return [iBlock > 0 && iBlock < w.indexOf("const std::wstring docs = ")
      && /if \(!otherKey\.empty\(\) && otherKey != S\.key\) \{\s*note = L"The Word file of this sound track holds another transcript of it \(another language pair or model\): it is kept\.";\s*return std::wstring\(\);/.test(w)
      && /chooseTranscriptPath\(dirs\[k\], base, ext, sibling, exists, ownsForThisAnalysis, S\.audio\.ord\)/.test(w)
      && /registryOwnsFor\(reg, t\.prefer, s0, m0, S\.path, S\.key, S\.audio\.ord\)/.test(rw)
      && /registryOwnsFor\(reg, p, s, m, mediaPath, key, ord\)/.test(sv)
      && /name = pbtrf::leafOf\(pbtrf::transcriptCandidates\(startFolder, base, pbtrf::extOf\(S\.name\), false, ord\)\.front\(\)\);/.test(sv)
      && /if \(key\.empty\(\)\) return false;/.test(own) && /r\.key == key && r\.ord == ord\) return true;/.test(own)
      && /return e \? e->key : std::string\(\);/.test(dw) && /registryTrackDocx\(reg, set\.path, set\.audio\.ord,/.test(dw)
      && /std::string docxWriter\(const pbplayer::CueSet& set, const std::wstring& path\) override;/.test(S.bridge), { iBlock }];
  },
  [{ file: "store", find: "    if (key.empty()) return false;\n    const std::string k = toUtf8(lowerW(path)), m = toUtf8(lowerW(media));\n    for (const auto& r : e)\n        if (r.path == k && r.size == size && r.mtime == mtime && r.media == m && r.key == key && r.ord == ord) return true;", replace: "    const std::string k = toUtf8(lowerW(path)), m = toUtf8(lowerW(media));\n    for (const auto& r : e)\n        if (r.path == k && r.size == size && r.mtime == mtime && r.media == m && r.key == key && r.ord == ord) return true;", why: "an older registry's KEYLESS entry owned by a caller with no key" },
   { file: "store", find: "r.key == key && r.ord == ord) return true;", replace: "r.ord == ord) return true;", why: "another analysis' Word file (another pair or model) replaced: ownership per media again" },
   { file: "bridge", find: "    if (!otherKey.empty() && otherKey != S.key) {", replace: "    if (false) {", why: "another analysis of the track writes \"(SuVtitle)\" beside the named Word file" },
   { file: "bridge", find: "exists, ownsForThisAnalysis, S.audio.ord);", replace: "exists, ownsForThisAnalysis);", why: "track 2's transcript takes the FIRST track's names" },
   { file: "bridge", find: "registryOwnsFor(reg, t.prefer, s0, m0, S.path, S.key, S.audio.ord)", replace: "registryOwns(reg, t.prefer, s0, m0, S.path)", why: "G2 in another analysis rewrites the track's Word file" },
   { file: "bridge", find: "registryOwnsFor(reg, p, s, m, mediaPath, key, ord) && !trReadOnlyFile(p)", replace: "registryOwns(reg, p, s, m, mediaPath) && !trReadOnlyFile(p)", why: "Save .docx... offers ANOTHER analysis' Word file" },
   { file: "bridge", find: "        return e ? e->key : std::string();", replace: "        return std::string();", why: "the core never learns that SuVtitle wrote the file for another analysis (its tab says \"changed outside SuVtitle\")" }]);
pin("FX-3", "FX F8", "A RENAME NAMES ITS OWN TRANSCRIPT: the door reads `vrev` (a number, required) beside jobId and hands both to the core -- renameSpeaker(gen, vrev, ...), which refuses another generation OR another numbering (a Re-transcribe that ended under the field) with one sentence, said in a notice",
  (S) => {
    const door = between(S.bridge, 'if (type == L"translatorSpeakerName") {', 'if (type == L"translatorModelImport")');
    const rn = fnBody(S.player, "bool renameSpeaker");
    return /\|\| id > 64 \|\| !v\.u64\(L"vrev", vrev\)\) return;/.test(door)
      && /player_->renameSpeaker\(gen, vrev, static_cast<uint32_t>\(id\), wideToUtf8\(lv->s\), &why\) && !why\.empty\(\)\)\s*postTranslatorFileNotice\(why, true\);/.test(door)
      && /bool renameSpeaker\(uint64_t gen, uint64_t vrev, uint32_t id, const std::string& label, std::wstring\* why = nullptr\)/.test(S.player)
      && /if \(gen != s_\.gen \|\| vrev != s_\.set->vrev\) \{\s*if \(why\) \*why = L"The speaker was not renamed: the transcript changed while the name was being typed\.";\s*return false;/.test(rn);
  },
  [{ file: "bridge", find: "            || id > 64 || !v.u64(L\"vrev\", vrev)) return;", replace: "            || id > 64) return;", why: "a rename with no numbering taken (vrev 0: a stale one passes for the first numbering)" },
   { file: "bridge", find: "player_->renameSpeaker(gen, vrev, ", replace: "player_->renameSpeaker(gen, 0, ", why: "the door drops the page's numbering" },
   { file: "player", find: "        if (gen != s_.gen || vrev != s_.set->vrev) {", replace: "        if (gen != s_.gen) {", why: "a Re-transcribe that ended under the field: the old number names whoever has it now" },
   { file: "player", find: "            if (why) *why = L\"The speaker was not renamed: the transcript changed while the name was being typed.\";\n", replace: "", why: "a stale rename refused in silence (the name typed is lost without a word)" }]);

// ================= FX2 (03/11/2026, the round after the user's installer test -- docs/PROGRESS_FX2.md) ============================
// (the core's half RUN by tests\cpp\player_smoke.cpp [FX2] A5, against a FakeHost that follows this same order)
pin("FX2-1", "FX2 A5 (new)", "AN ANALYSIS THAT OWNS ITS WORD FILE IS NEVER TOLD \"ANOTHER TRANSCRIPT\": writeTranscriptDocx asks registryTrackDocx (F2's guard -- another analysis of this sound track holds its Word file: nothing written) ONLY for an analysis that does NOT own t.prefer (ownsForThisAnalysis); one that owns it and whose folder refused its .tmp (WriteFail::Folder: a card write-locked since) goes on to the names, then Documents -- even when another analysis Saved a newer Word file of the track; a Target refusal still returns its sentence first (F4)",
  (S) => {
    const w = fnBody(S.bridge, "std::wstring Bridge::writeTranscriptDocx");
    return ordered(w, ["const bool ownPrefer = !t.prefer.empty() && exists(t.prefer) && ownsForThisAnalysis(t.prefer);",
                       "if (ownPrefer) {", "if (pbtrf::writeFileAvSafe(t.prefer, bytes, true, &id, &fail)) return finish(t.prefer, id);",
                       "if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }",
                       "if (!ownPrefer) {", "const std::string otherKey = pbtrf::registryTrackDocx(reg, S.path, S.audio.ord, exists);",
                       "const std::wstring docs = "])
      // (nothing returns on the FOLDER's refusal: the block ends right after the Target line -- comments aside -- and the guard follows)
      && /if \(fail == pbtrf::WriteFail::Target\) \{ note = trDocxRefusal\(t\.prefer\); return std::wstring\(\); \}\s*(?:\/\/[^\n]*\n\s*)*\}\s*(?:\/\/[^\n]*\n\s*)*if \(!ownPrefer\) \{\s*const std::string otherKey = /.test(w);
  },
  [{ file: "bridge", find: "    if (!ownPrefer) {\n", replace: "    if (true) {\n", why: "the F2 guard for an analysis that OWNS its Word file again (A5): its folder refusing the .tmp + another analysis' newer Save = \"holds another transcript\", never the Documents copy" },
   { file: "bridge", find: "    const bool ownPrefer = !t.prefer.empty() && exists(t.prefer) && ownsForThisAnalysis(t.prefer);", replace: "    const bool ownPrefer = !t.prefer.empty() && exists(t.prefer);", why: "a Word file the record names but this analysis no longer owns (another analysis Saved over it) skips the F2 guard" },
   { file: "bridge", find: "        if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }\n", replace: "        if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }\n        return std::wstring();\n", why: "the FOLDER's refusal of its own Word file a dead end: no other name, no Documents, nothing said" }]);

// ================= UPD (03/11/2026, THE UPDATE BANNER -- bridge.cpp's section of that name, NextaleUpdate.h/.cpp) ===================
// The C++ smoke (tests\cpp\update_smoke.cpp, its sentinels tests\cpp\cmut_upd.py) RUNS NextaleUpdate.cpp and AppConfig.h's
// lines against a loopback server; these pins tie the BRIDGE's wiring to the rules no headless test can run: what is opened
// and from where, once per start off the UI thread, what the page is told and what app.txt holds, every address app\ names.
pin("UPD-1", "UPD (new)", "THE DOORS: the bridge handles the six update.* messages the page sends; update.open reads NOTHING from the page and opens what the APP built -- SuVtitle's page pageUrl(the manifest's version, SUV_VERSION_STR), that version only ever the worker's answer -- through openInBrowser only; update.opened's url is that same address; a TEST INSTANCE never opens the browser",
  (S) => {
    const doors = ["update.check", "update.open", "update.later", "update.skip", "update.unskip", "update.enable"];
    const open = fnBody(S.bridge, "void Bridge::openUpdatePage"), code = codeOnly(S.bridge);
    return [doors.every((d) => S.bridge.indexOf('if (type == L"' + d + '")') >= 0)
      && /if \(type == L"update\.open"\)\s*\{ openUpdatePage\(\); return; \}/.test(S.bridge)
      && /void openUpdatePage\(\);/.test(S.bridgeH) && /void Bridge::openUpdatePage\(\) \{/.test(S.bridge)
      && open.indexOf("const std::string url = nextale::Update::pageUrl(updVersion_, SUV_VERSION_STR);") >= 0
      && open.indexOf("const bool test = pblaunch::isTestInstance();") >= 0
      && open.indexOf("const bool ok = !url.empty() && (test || nextale::Update::openInBrowser(url));") >= 0
      && open.indexOf('.kv(L"url", utf8ToWide(url.c_str()))') >= 0
      && count(code, /openInBrowser\(/g) === 1 && count(code, /pageUrl\(/g) === 1 && !/ShellExecute/.test(open)
      && count(code, /\bupdVersion_ = /g) === 1 && /updVersion_ = info\.version;/.test(code),
      { missing: doors.filter((d) => S.bridge.indexOf('if (type == L"' + d + '")') < 0) }];
  },
  [{ file: "bridge", find: "const bool ok = !url.empty() && (test || nextale::Update::openInBrowser(url));", replace: "const bool ok = !url.empty() && nextale::Update::openInBrowser(url);", why: "a TEST INSTANCE opens the user's browser" },
   { file: "bridge", find: "    if (type == L\"update.open\")   { openUpdatePage(); return; }", replace: "    if (type == L\"update.open\")   { std::wstring u; jsonFindString(json, L\"url\", u); openUpdatePage(); return; }", why: "update.open reads an address from the page" },
   { file: "bridge", find: "    if (type == L\"update.open\")   { openUpdatePage(); return; }", replace: "    if (type == L\"update.open\")   { std::wstring v; jsonFindString(json, L\"version\", v); updVersion_ = wideToUtf8(v); openUpdatePage(); return; }", why: "the version in the opened address comes from the page" },
   { file: "bridge", find: "const std::string url = nextale::Update::pageUrl(updVersion_, SUV_VERSION_STR);", replace: "const std::string url = updUrl_;", why: "the manifest's url is opened (not the app's own page address)" },
   { file: "bridgeH", find: "    void openUpdatePage();", replace: "    void openUpdatePage(const std::string& url);", why: "openUpdatePage opens an address its caller hands it" },
   { file: "bridge", find: '.kv(L"url", utf8ToWide(url.c_str()))', replace: '.kv(L"url", L"https://www.nextale-audio.com/")', why: "update.opened names another address than the one opened" }]);

pin("UPD-2", "UPD (new)", "ONCE PER START, OFF THE UI THREAD, NEVER FROM A TEST: the site is asked only at the page's update.check (or the opt-out turned back on) -- once (updStarted_), only when enabled; a test instance only asks a loopback SUVTITLE_TEST_UPDATE_URL, else nothing; the worker holds the box, the HWND and the url (never `this`), rings kWmTranslatorPoll unless the app is ending; pumpTranslator drains it; shutdown sets quit",
  (S) => {
    const st = fnBody(S.bridge, "void Bridge::startUpdateCheck"), pump = fnBody(S.bridge, "void Bridge::pumpTranslator");
    const shut = fnBody(S.bridge, "void Bridge::shutdown"), ctor = fnBody(S.bridge, "Bridge::Bridge"), att = fnBody(S.bridge, "void Bridge::attach");
    return /^\{\s*if \(updStarted_ \|\| !updEnabled_ \|\| !updBox_\) return;/.test(st)
      && ordered(st, ["if (pblaunch::isTestInstance()) {", 'pblaunch::envOf(L"SUVTITLE_TEST_UPDATE_URL")', "if (!nextale::Update::loopbackUrlOk(t)) return;", "url = t;", "updStarted_ = true;",
                      "std::thread([box, h, url] {", "nextale::Update::check(SUV_VERSION_STR, url);", "box->done = true;",
                      "if (h && !box->quit.load()) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);", "}).detach();"])
      && /drainPlayerTracks\(\);[^\n]*\n\s*drainUpdate\(\);/.test(pump) && /if \(updBox_\) updBox_->quit\.store\(true\);/.test(shut)
      && count(codeOnly(S.bridge), /startUpdateCheck\(\);/g) === 2 && !/startUpdateCheck|update\.check/.test(codeOnly(ctor + att));   // (code only)
  },
  [{ file: "bridge", find: "    if (updStarted_ || !updEnabled_ || !updBox_) return;", replace: "    if (!updEnabled_ || !updBox_) return;", why: "the site asked at every page boot (a reload for a new folder asks again)" },
   { file: "bridge", find: "    if (updStarted_ || !updEnabled_ || !updBox_) return;", replace: "    if (updStarted_ || !updBox_) return;", why: "the opt-out (UPDATECHECK 0) is ignored" },
   { file: "bridge", find: "    if (pblaunch::isTestInstance()) {\n        // A TEST NEVER ASKS THE SITE", replace: "    if (false) {\n        // A TEST NEVER ASKS THE SITE", why: "a TEST INSTANCE asks the real site" },
   { file: "bridge", find: "std::thread([box, h, url] {", replace: "std::thread([this, box, h, url] {", why: "the worker captures `this` (it may outlive the Bridge)" },
   { file: "bridge", find: "    if (updBox_) updBox_->quit.store(true);", replace: "", why: "a late answer rings a window that is gone" },
   { file: "bridge", find: "    drainUpdate();          // THE UPDATE BANNER: and the update check's answer\n", replace: "", why: "the answer is never drained by the wake" }]);

pin("UPD-3", "UPD (new)", "WHAT THE PAGE IS TOLD, WHAT app.txt HOLDS -- closed lists: update.status carries type, enabled, available, current and, for a known version, version, notes, later, skipped; update.opened type, ok, url, test; the update.* doors read nothing from the page but update.enable's `on`; UpdatePrefs holds the check and the skip; packUpdatePrefs replaces and writes UPDATECHECK / UPDATESKIP lines only -- every other line of app.txt is kept as it is",
  (S) => {
    const st = fnBody(S.bridge, "void Bridge::postUpdateStatus"), opened = fnBody(S.bridge, "void Bridge::openUpdatePage");
    const pack = codeOnly(fnBody(S.appcfg, "inline std::string packUpdatePrefs"));
    const prefs = (/struct UpdatePrefs \{([^}]*)\};/.exec(S.appcfg) || ["", ""])[1];
    const doors = between(S.bridge, 'if (type == L"update.check")', "// A language code as the wire wants it");
    const list = (src, re) => [...src.matchAll(re)].map((m) => m[1]).join(",");
    const status = list(st, /\.kv\(L"(\w+)"/g), answer = list(opened, /\.kv\(L"(\w+)"/g);
    const reads = list(doors, /jsonFind(?:String|Number|Bool)\(json, L"(\w+)"/g), members = list(prefs, /^\s*[\w:<>]+\s+(\w+)(?:\s*=[^;]*)?;/gm);
    const writes = list(pack, /with\.push_back\("(\w+)\\t/g), takes = list(pack, /updatePrefLine\(l, "(\w+)", v\)/g);
    return [status === "type,enabled,available,current,version,notes,later,skipped" && answer === "type,ok,url,test"
      && doors.length > 500 && reads === "on" && members === "check,skip" && writes === "UPDATECHECK,UPDATESKIP"
      && takes === "UPDATECHECK,UPDATESKIP", { status, answer, reads, members, writes, takes }];
  },
  [{ file: "bridge", find: '           .kv(L"current", utf8ToWide(SUV_VERSION_STR));', replace: '           .kv(L"current", utf8ToWide(SUV_VERSION_STR)).kv(L"user", false);', why: "update.status grows a member" },
   { file: "bridge", find: "    if (test) j.kv(L\"test\", true);\n    j.end();\n    updOpenAt_ = now;", replace: "    if (test) j.kv(L\"test\", true);\n    j.kv(L\"where\", 0);\n    j.end();\n    updOpenAt_ = now;", why: "update.opened grows a member" },
   { file: "bridge", find: "    if (type == L\"update.skip\") {\n", replace: "    if (type == L\"update.skip\") {\n        std::wstring v; jsonFindString(json, L\"version\", v);\n", why: "update.skip reads a version from the page" },
   { file: "appcfg", find: "    std::string skip;           // the skipped version (\"\" = none)\n", replace: "    std::string skip;           // the skipped version (\"\" = none)\n    std::string extra;          // a third value\n", why: "UpdatePrefs holds a third value" },
   { file: "appcfg", find: '    if (updateVersionToken(p.skip)) with.push_back("UPDATESKIP\\t" + p.skip);', replace: '    if (updateVersionToken(p.skip)) with.push_back("UPDATESKIP\\t" + p.skip);\n    with.push_back("UPDATEDAY\\t1");', why: "packUpdatePrefs writes a line of another tag" },
   { file: "appcfg", find: '        if (updatePrefLine(l, "UPDATECHECK", v) || updatePrefLine(l, "UPDATESKIP", v)) {', replace: '        if (updatePrefLine(l, "UPDATECHECK", v) || updatePrefLine(l, "UPDATESKIP", v) || updatePrefLine(l, "NOTE", v)) {', why: "packUpdatePrefs takes away a line that is not its own (another writer's, an unknown one)" }]);

pin("UPD-4", "UPD (new)", "WHAT IS OPENED, WHAT IS ASKED: SuVtitle's manifest (kManifestUrl); SuVtitle's page in ONE constant (kPageUrl), pageUrl = that constant + ?update=<new>&from=<this> with both versions versionOk checked; each WinHTTP step bounded (kTimeoutMs 10 s), 2xx only, a body over kBodyMax refused, plain http only on the loopback; the launch guard = the two site prefixes and nothing else (urlAllowed = siteUrl), checked AGAIN inside openInBrowser -- the only ShellExecute of the file; app.txt's lines through the AV-safe write, only when they change",
  (S) => {
    const ob = fnBody(S.upd, "bool openInBrowser"), save = fnBody(S.bridge, "void Bridge::saveUpdatePrefs");
    const allowed = fnBody(S.upd, "bool urlAllowed"), page = fnBody(S.upd, "std::string pageUrl");
    const lines = (src) => (src || "").split("\n").filter((l) => !/^\s*\/\//.test(l)).join("\n");   // (whole-line comments out: a URL's "//" stays)
    return /constexpr const wchar_t\* kManifestUrl = L"https:\/\/www\.nextale-audio\.com\/updates\/suvtitle\.json";/.test(S.updH)
      && /constexpr const char\* kPageUrl = "https:\/\/www\.nextale-audio\.com\/suvtitle";/.test(S.updH)
      && count(lines([S.upd, S.updH, S.bridge].join("\n")), /"https:\/\/(www\.)?nextale-audio\.com\/suvtitle/g) === 1
      && /^\{\s*return siteUrl\(url\);[^\n]*\n\}$/.test(allowed)
      && ordered(page, ["if (!versionOk(newVersion) || !versionOk(currentVersion)) return std::string();",
                        'const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
                        "return urlAllowed(u) ? u : std::string();"])
      && /constexpr unsigned long kTimeoutMs = 10000;/.test(S.updH) && /::WinHttpSetTimeouts\(ses, t, t, t, t\);/.test(S.upd)
      && /&& status >= 200 && status < 300\) \{/.test(S.upd) && /if \(got\.size\(\) \+ avail > kBodyMax\) break;/.test(S.upd)
      && /if \(!secure && !\(uc\.nScheme == INTERNET_SCHEME_HTTP && loopback\)\) return false;/.test(S.upd)
      && /return \(startsWith\(url, "https:\/\/www\.nextale-audio\.com\/"\) \|\| startsWith\(url, "https:\/\/nextale-audio\.com\/"\)\) && urlChars\(url\);/.test(S.upd)
      && ordered(ob, ["if (!urlAllowed(url)) return false;", "::ShellExecuteW("]) && count(codeOnly(S.upd), /ShellExecute/g) === 1
      && ordered(save, ["if (!pbtrf::readFileAll(cfg, before, 1ULL << 20) && ::GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES) return;",
                        "if (packed == before) return;", "pbtrf::writeFileAvSafe(cfg, packed);"]);
  },
  [{ file: "upd", find: "    if (!urlAllowed(url)) return false;\n    const std::wstring w(url.begin(), url.end());", replace: "    const std::wstring w(url.begin(), url.end());", why: "openInBrowser launches whatever it is handed" },
   { file: "upd", find: "                    && status >= 200 && status < 300) {", replace: "                    ) {", why: "an error page (the 404 of a manifest not published) is read as the manifest" },
   { file: "upd", find: "    ::WinHttpSetTimeouts(ses, t, t, t, t);", replace: "", why: "no timeouts: a silent server holds the worker for minutes" },
   { file: "upd", find: "    return siteUrl(url);   // (the site", replace: "    return siteUrl(url) || url.rfind(\"https://\", 0) == 0;   // (the site", why: "the launch guard widened beyond the site" },
   { file: "upd", find: "    if (!versionOk(newVersion) || !versionOk(currentVersion)) return std::string();\n", replace: "", why: "pageUrl takes any version (a parameter, a path, a fragment can ride in)" },
   { file: "upd", find: '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;', replace: '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion;', why: "the page's address loses from=" },
   { file: "bridge", find: "const std::string url = nextale::Update::pageUrl(updVersion_, SUV_VERSION_STR);", replace: "const std::string url = \"https://www.nextale-audio.com/suvtitle?update=\" + updVersion_;", why: "the page's address written a second time, outside its ONE constant" },
   { file: "bridge", find: "    if (packed == before) return;              // (unchanged: no write, no antivirus window)\n", replace: "", why: "app.txt rewritten when nothing changed" }]);

pin("UPD-5", "UPD review 03/11 (S5, N3)", "A SKIP CAN BE UNDONE AND THE PAGE IS OPENED ONCE: update.status names a SKIPPED version (version + notes + skipped:true, never available) so the dialog can say so; update.unskip clears UPDATESKIP (the AV-safe save) and Later; openUpdatePage opens a skipped version too (the dialog asks on purpose) -- and a repeat within 2 s gets the last update.opened again, nothing opened a second time",
  (S) => {
    const st = fnBody(S.bridge, "void Bridge::postUpdateStatus"), open = fnBody(S.bridge, "void Bridge::openUpdatePage");
    const unskip = between(S.bridge, 'if (type == L"update.unskip") {', 'if (type == L"update.enable")');
    return ordered(st, ["const bool known = updEnabled_ && !updVersion_.empty();", "const bool skipped = known && updVersion_ == updSkip_;",
                        "const bool avail = known && !skipped;", '.kv(L"available", avail)', "if (known)", '.kv(L"skipped", skipped);'])
      && /if \(!updSkip_\.empty\(\)\) \{ updSkip_\.clear\(\); saveUpdatePrefs\(\); \}\s*updLater_ = false;\s*postUpdateStatus\(\);\s*return;/.test(unskip)
      && /^\{\s*if \(!updEnabled_ \|\| updVersion_\.empty\(\)\) return;/.test(open)
      && ordered(open, ["if (updOpenAt_ != 0 && now - updOpenAt_ < 2000 && !updOpenJson_.empty()) {",
                        "postToTranslatorView(updOpenJson_);", "return;", "nextale::Update::openInBrowser(url)",
                        "updOpenAt_ = now;", "updOpenJson_ = j.take();", "postToTranslatorView(updOpenJson_);"]);
  },
  [{ file: "bridge", find: "    const bool skipped = known && updVersion_ == updSkip_;", replace: "    const bool skipped = false;", why: "a skipped version is offered in the banner again (the skip forgotten)" },
   { file: "bridge", find: "         .kv(L\"skipped\", skipped);", replace: "         ;", why: "the page never learns a version is skipped (the dialog: \"no newer version\")" },
   { file: "bridge", find: "        if (!updSkip_.empty()) { updSkip_.clear(); saveUpdatePrefs(); }\n        updLater_ = false;", replace: "        updLater_ = false;", why: "\"Offer it again\" leaves UPDATESKIP in app.txt" },
   { file: "bridge", find: "    if (!updEnabled_ || updVersion_.empty()) return;   // nothing known: nothing to open", replace: "    if (!updEnabled_ || updVersion_.empty() || updVersion_ == updSkip_) return;", why: "the dialog's Download refused for a skipped version" },
   { file: "bridge", find: "    if (updOpenAt_ != 0 && now - updOpenAt_ < 2000 && !updOpenJson_.empty()) {", replace: "    if (false) {", why: "a double-click opens two browser tabs" }]);

// every text file of app\ -- its C++, CMakeLists, the .rc, web\ -- for UPD-6; the files S holds are read from S (their mutants
// are seen), the others from the disk
const appTexts = (S) => {
  const out = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) { walk(p); continue; }
      if (!/\.(cpp|h|html|js|css|json|txt|rc|md)$/i.test(e.name)) continue;
      const rel = path.relative(APP, p).split(path.sep).join("/");
      const k = Object.keys(FILES).find((key) => FILES[key] === rel);
      out.push({ rel, text: k ? S[k] : fs.readFileSync(p, "utf8") });
    }
  };
  walk(APP);
  return out;
};
// UPD-6: the ONLY hosts an http(s) address written in app\ may name, each with its scheme and why it is there (the inventory of
// 03/11/2026: what app\ holds, nothing more -- a new host is a decision: add it here, with its reason)
const URL_HOSTS = {
  "www.nextale-audio.com": "https", "nextale-audio.com": "https",              // the site: the manifest, SuVtitle's page
  "127.0.0.1": "http", "localhost": "http",                                    // a TEST INSTANCE's loopback manifest (loopbackUrlOk)
  "suvtitle.local": "https", "patchbus.local": "https",                        // the page's own origin; AudioPB's (main.cpp names it)
  "*.suvtitle-media.example": "https", "suvtitle-proxy.example": "https",      // the media folders' and the sound copy's mappings
  "evil.example": "https", "www.nextale-audio.com.evil.example": "https",      // siteUrl's comment: the lookalikes it refuses
  "schemas.openxmlformats.org": "http", "www.w3.org": "http",                  // XML namespace names (the Word file, SVG): never fetched
  "www.nuget.org": "https",                                                    // CMakeLists.txt: where the build gets the WebView2 SDK
  "": "https",                                                                 // a scheme alone: a host the code appends (TrPlayer.h), a wrapped comment
};
// every http(s) address in app\ -- code, comments, the page -- with its host (after any user:password@, without its port)
const appUrls = (S) => {
  const out = [];
  for (const f of appTexts(S))
    for (const m of String(f.text || "").matchAll(/\b(https?):\/\/([^\s"'`<>()\\\/?#;,]*)/gi)) {
      const auth = m[2], host = auth.slice(auth.lastIndexOf("@") + 1).replace(/:\d*$/, "").toLowerCase();
      out.push({ rel: f.rel, scheme: m[1].toLowerCase(), host });
    }
  return out;
};
pin("UPD-6", "UPD (new)", "EVERY ADDRESS IN app\\ IS A KNOWN ONE: each http(s) URL written anywhere in app\\ -- its C++, CMakeLists, the page and the rest of web\\, comments included -- names the site (https://www.nextale-audio.com/, https://nextale-audio.com/), the documented loopback test forms (http://127.0.0.1:<port>/, http://localhost:<port>/), or one of the few other names app\\ really holds (the page's own origin, the media mappings, XML namespace names, the build's SDK source, siteUrl's documented lookalikes), each with its scheme; any other host fails",
  (S) => {
    const urls = appUrls(S), names = appTexts(S).map((f) => f.rel);
    const bad = urls.filter((u) => !Object.prototype.hasOwnProperty.call(URL_HOSTS, u.host) || URL_HOSTS[u.host] !== u.scheme)
      .map((u) => u.rel + ": " + u.scheme + "://" + u.host);
    const site = urls.filter((u) => u.host === "www.nextale-audio.com" && (u.rel === "NextaleUpdate.h" || u.rel === "web/index.html")).length;
    const loop = urls.filter((u) => u.rel === "NextaleUpdate.cpp" && (u.host === "127.0.0.1" || u.host === "localhost")).length;
    return [["bridge.cpp", "NextaleUpdate.h", "NextaleUpdate.cpp", "CMakeLists.txt", "web/index.html"].every((n) => names.indexOf(n) >= 0)
            && site >= 3 && loop === 2 && bad.length === 0, { urls: urls.length, site, loop, bad }];
  },
  [{ file: "updH", find: 'constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle";', replace: 'constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle";\nconstexpr const char* kOtherPage = "https://example.com/suvtitle";', why: "an address on another host is written in NextaleUpdate.h" },
   { file: "page", find: '        const page = "https://www.nextale-audio.com/suvtitle?update=" + mUpd.version + "&from=1.1.0";', replace: '        const page = "https://downloads.example.com/suvtitle?update=" + mUpd.version + "&from=1.1.0";', why: "the page names an address on another host" },
   { file: "bridge", find: "    if (type == L\"update.open\")   { openUpdatePage(); return; }", replace: "    if (type == L\"update.open\")   { openUpdatePage(); return; }   // (see https://example.net/)", why: "a comment names another host (comments count)" },
   { file: "upd", find: 'L"http://127.0.0.1:"', replace: 'L"https://127.0.0.1:"', why: "the loopback test form changes scheme" }]);

/*@@PINS-END@@*/

// ---- the runner --------------------------------------------------------------------------------------------------------------
function evalPin(p, S) {
  try { const r = p.test(S); return Array.isArray(r) ? { ok: !!r[0], extra: r[1] } : { ok: !!r }; }
  catch (e) { return { ok: false, extra: "threw: " + e.message }; }
}
// apply one mutant to a copy of the sources: it must apply EXACTLY once (a string, or a regular expression without the g flag)
function mutate(S, m) {
  const text = S[m.file];
  if (text == null) return { err: "app\\" + FILES[m.file] + " missing" };
  const n = typeof m.find === "string" ? text.split(m.find).length - 1 : (text.match(new RegExp(m.find.source, "g" + m.find.flags.replace("g", ""))) || []).length;
  if (n !== 1) return { err: "applies " + n + " times (must be exactly 1): " + String(m.find).slice(0, 80) };
  const out = Object.assign({}, S);
  out[m.file] = typeof m.find === "string" ? text.replace(m.find, () => m.replace) : text.replace(m.find, m.replace);
  return { S: out };
}

const argv = process.argv.slice(2);
const S0 = readSources();
const missing = Object.entries(S0).filter(([, v]) => v == null).map(([k]) => "app\\" + FILES[k]);
if (argv.includes("--list")) {
  PINS.forEach((p) => console.log(p.id.padEnd(4) + " [" + p.from + "] " + p.name));
  process.exit(0);
}
let pass = 0, fail = 0, mutPass = 0, mutFail = 0;
console.log("test_host_pins: " + PINS.length + " pins over " + Object.keys(FILES).length + " sources of " + APP);
if (missing.length) { console.log("  FAIL  sources missing: " + missing.join(", ")); fail++; }
for (const p of PINS) {
  const r = evalPin(p, S0);
  if (r.ok) { pass++; console.log("  PASS  " + p.id + "  " + p.name); }
  else { fail++; console.log("  FAIL  " + p.id + "  " + p.name + (r.extra ? "  -> " + JSON.stringify(r.extra).slice(0, 300) : "")); }
}
if (!argv.includes("--no-selftest")) {
  console.log("-- self-test: every pin must FAIL on each of its mutants (the guard undone in the source text)");
  for (const p of PINS) {
    if (!evalPin(p, S0).ok) continue;                       // a failing pin is reported above; its mutants prove nothing
    if (!p.mutants.length) { mutFail++; console.log("  FAIL  " + p.id + "  has no mutant (a pin that cannot be shown to fail is not a pin)"); continue; }
    for (const m of p.mutants) {
      const a = mutate(S0, m);
      if (a.err) { mutFail++; console.log("  FAIL  " + p.id + "  mutant \"" + m.why + "\": " + a.err); continue; }
      const r = evalPin(p, a.S);
      if (!r.ok) { mutPass++; console.log("  caught " + p.id + "  \"" + m.why + "\""); }
      else { mutFail++; console.log("  FAIL  " + p.id + "  mutant \"" + m.why + "\" SURVIVES: the pin did not notice"); }
    }
  }
}
const bad = fail + mutFail;
console.log((bad ? "TEST_HOST_PINS FAILED" : "TEST_HOST_PINS OK") + " -- " + pass + " pins passed, " + fail + " failed"
  + (argv.includes("--no-selftest") ? "" : "; " + mutPass + " mutants caught, " + mutFail + " not"));
process.exit(bad ? 1 : 0);

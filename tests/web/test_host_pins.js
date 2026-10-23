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
// instance, default player, test instance (L1-L13).
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
    const flat = [...S.bridge.matchAll(/jsonFind(?:String|Number|Bool)\(\s*json\s*,\s*L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const structured = [...S.bridge.matchAll(/\b(?:v|o)\.(?:str|u64|get|flag)\(L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const keys = [...new Set([...flat, ...structured])];
    const KNOWN = ["type", "quality", "target", "mode", "preset", "langA", "langB", "clear", "alwaysOnTop", "value", "what", "force", "jobId", "format",
      "which", "seq", "op", "id", "to", "ms", "translate", "repeat", "shuffle", "audioOrd", "auto", "gen", "durMs", "playing", "reason", "enc", "on",
      "rev", "remove", "add", "restore", "s", "e", "text", "lang", "dstLang", "src", "dst", "pass"];   // (the editor's edit: cue fields, free text, never a path)
    const unknown = keys.filter((k) => KNOWN.indexOf(k) < 0), pathy = keys.filter((k) => /path|file|folder|dir|url|uri|name/i.test(k));
    return [handlers.length > 2000 && !/L"path"/.test(handlers) && /The PATH is looked up here, never received\./.test(handlers)
            && /ComPtr<ICoreWebView2File> f;/.test(S.bridge) && /f->get_Path\(&p\)/.test(attachDrop) && flat.length >= 30 && structured.length >= 5
            && unknown.length === 0 && pathy.length === 0,
            { keys: keys.length, unknown, pathy }];
  },
  [{ file: "bridge", find: 'jsonFindString(json, L"quality", q);\n        handlePlayerOpen(', replace: 'jsonFindString(json, L"quality", q); jsonFindString(json, L"path", q);\n        handlePlayerOpen(', why: "a handler reads a `path` key from the page's JSON" },
   { file: "bridge", find: "The PATH is looked up here, never received.", replace: "(the path comes with the message)", why: "the comment that states the rule is gone (the rule with it)" },
   { file: "bridge", find: 'jsonFindString(json, L"quality", q);\n        handlePlayerOpen(', replace: 'jsonFindString(json, L"quality", q); jsonFindString(json, L"label", q);\n        handlePlayerOpen(', why: "a NEW key with an innocent name (the closed list is the gate: is it a path?)" }]);

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

pin("M2", "ui WC 4c [3]", "a folder that cannot be mapped (an odd UNC share, a vanished drive) -> no URL, and the core asks for the sound itself at once (a page with nothing to load waits forever)",
  (S) => /s_\.url\.clear\(\);\s*requestProxy\(\);/.test(fnBody(S.player, "void mapNow")),
  [{ file: "player", find: "            s_.url.clear();\n            requestProxy();", replace: "            s_.url.clear();", why: "the URL is cleared and nobody asks for the sound: the page waits for ever" }]);

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
  [{ file: "bridge", find: "return pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwns(reg, p, s, m, S.path);", replace: "return pbtrf::fileIdentity(p, s, m);", why: "a file of that name is taken for ours without asking the registry (a downloaded subtitle gets overwritten)" },
   { file: "bridge", find: "&& have == bytes &&", replace: "&& true &&", why: "an identical file is rewritten at every tick (write-time churn for a player watching it)" }]);

pin("F2", "ui WC 4c [1][2][D][E]", "the automatic .srt: free-or-ours name, no-replace when the name was ABSENT, identity as WRITTEN, registered with its media and the record that wrote it",
  (S) => {
    const srt = fnBody(S.bridge, "std::wstring Bridge::writeTranslatorSrt");
    return /chooseSrtPath\(dirs\[k\], base, ext, tag, sibling, exists, ownsForThisMedia\)/.test(srt) && /const bool replace = exists\(p\);/.test(srt)
        && /writeFileAvSafe\(p, bytes, replace, &id\)/.test(srt) && /trRegister\(p, s, m, S\.path, lang, fromKey, fromOrd\)/.test(srt) && /hasSiblingMedia\(/.test(srt);
  },
  [{ file: "bridge", find: "pbtrf::writeFileAvSafe(p, bytes, replace, &id)", replace: "pbtrf::writeFileAvSafe(p, bytes, true, &id)", why: "a file that appeared meanwhile is clobbered (replace is always on)" },
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
        && /pbtrf::siblingKeys\(S\.path, S\.size, S\.mtime, lang, \{ S\.langA, S\.langB \}, S\.audio\.ord\)/.test(bs)
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

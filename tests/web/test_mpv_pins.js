// test_mpv_pins.js -- the SOURCE PINS of the native video engine's HOST guards (1.1.0 fix round, N1, 26/10 -> 02/11/2026).
// Standalone node, nothing to install:
//   node tests\web\test_mpv_pins.js [--no-selftest] [--list]     exit 0 = every pin passes AND every mutant is caught
//   SUV_ROOT=<tree> node tests\web\test_mpv_pins.js               reads another copy of the tree (app\, deploy\)
//
// WHY. docs\REVIEW_RV-NH.md LOW13: N1's guards were covered by no pin and no mutant -- the test hooks' g_quiet checks, mpv's
// locked options, the LoadLibraryExW flags -- "removing any of them still passes every suite". What a headless suite cannot
// RUN (a hook only another process reaches, a DLL a planted folder would hand over, an option whose absence only shows with a
// hostile file) is pinned here, the way tests\web\test_host_pins.js pins the rest of the host. What CAN run is run as well:
// tests\cpp\mpv\mpv_smoke.cpp ([DLL] [NET] [AID] [HOLD] [EOF2] [TRACKS] [LOGCAP] [EARLY]) and its mutants (cmut_mpv.py).
//
// HOW (test_host_pins.js's way). Every pin is a regular expression (or a few) over the NORMALISED source (CRLF / lone CR -> LF),
// scoped to one function's DEFINITION where it can be (defBody: a forward declaration is skipped). A pin must PASS on today's
// sources and must be ABLE TO FAIL: the self-test (default; --no-selftest skips it) applies each of its MUTANTS -- the guard
// undone in the source text, the way a careless edit would -- and requires the pin to FAIL on every one. A mutant must apply
// EXACTLY once, or the self-test fails (a mutant that no longer applies proves nothing); a pin with no mutant fails too.
// When a pin fails on a legitimate change, EDIT THE PIN WITH THE CHANGE (and its mutants): the regex is the rule.
//
// 22 pins, 59 mutants: G the TEST INSTANCE's hooks (G1-G4) - O mpv's locked options (O1-O2) - L the LoadLibraryExW flags (L1-L4) -
// D the DLL search lock and the Vulkan loader's place (D1-D4) - A the item rules of the fix round (A1-A3) - T the track list's
// strings (T1) - H the host fixes no headless test reaches (H1-H4: LOW5, LOW6, LOW11, LOW12).
// deploy\make_bundle.js is N3's: only READ here (D4 ties its vkfallback folder to the one MpvPlayer pre-loads from).
"use strict";
const fs = require("fs");
const path = require("path");

const ROOT = process.env.SUV_ROOT ? path.resolve(process.env.SUV_ROOT) : path.resolve(__dirname, "..", "..");
const FILES = {
  mpv: "app/MpvPlayer.cpp", mpvH: "app/MpvPlayer.h", main: "app/main.cpp", cmake: "app/CMakeLists.txt",
  bundle: "deploy/make_bundle.js",
};
function readSources() {
  const S = {};
  for (const [k, f] of Object.entries(FILES)) {
    try { S[k] = fs.readFileSync(path.join(ROOT, f), "utf8").replace(/\r\n?/g, "\n"); } catch { S[k] = null; }
  }
  return S;
}
// the body of the DEFINITION of `sig` (e.g. "void testDrop", "bool MpvPlayer::attach"): from its "{" to the matching "}" --
// a forward declaration ("void testDrop();") is skipped; "" when absent
function defBody(src, sig) {
  if (src == null) return "";
  const re = new RegExp(sig.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "\\([^;{]*\\)\\s*(?:const\\s*)?(?:override\\s*)?\\{");
  const m = re.exec(src);
  if (!m) return "";
  const open = m.index + m[0].length - 1;
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
const codeOnly = (src) => (src == null ? "" : src.replace(/\/\/[^\n]*/g, ""));             // (C++ / JS line comments out)
const cmakeCode = (src) => (src == null ? "" : src.replace(/#[^\n]*/g, ""));             // (CMake comments out)

// ---- the pins --------------------------------------------------------------------------------------------------------------
// pin(id, from, name, test, mutants): test(S) -> true | false | [bool, detail]; mutants: [{ file, find, replace, why }]
const PINS = [];
const pin = (id, from, name, test, mutants) => PINS.push({ id, from, name, test, mutants: mutants || [] });

// ================= G: the TEST INSTANCE's hooks -- never reachable in the user's SuVtitle ===================================
pin("G1", "RV-NH LOW13", "the 'SVDT' drop (WM_COPYDATA kCopyTestDrop: a path dropped through the window's drop target) is refused by any instance that is not a TEST instance (g_quiet) -- and testDrop() checks again",
  (S) => /constexpr ULONG_PTR kCopyTestDrop = 0x53564454;/.test(S.main)
      && /if \(cds && cds->dwData == kCopyTestDrop\) \{\s*if \(!g_quiet \|\| !cds->lpData \|\| cds->cbData < sizeof\(wchar_t\) \|\| cds->cbData > 32768 \* sizeof\(wchar_t\)\s*\|\| \(cds->cbData % sizeof\(wchar_t\)\) != 0\) return FALSE;/.test(S.main)
      && /^\s*if \(!g_quiet \|\| !g_drops \|\| path\.empty\(\)\) return;/m.test(defBody(S.main, "void testDrop")),
  [{ file: "main", find: "if (!g_quiet || !cds->lpData ||", replace: "if (!cds->lpData ||", why: "any SuVtitle takes a path dropped by another process's WM_COPYDATA (a file opened without the user)" },
   { file: "main", find: "if (!g_quiet || !g_drops || path.empty()) return;", replace: "if (!g_drops || path.empty()) return;", why: "testDrop() no longer re-checks it runs in a test instance" }]);

pin("G2", "RV-NH LOW13", "SUVTITLE_TEST_NO_MOUSELEAVE (no TME_LEAVE) is read ONLY by a test instance: one read, inside `if (g_quiet) {`",
  (S) => count(codeOnly(S.main), /GetEnvironmentVariableW\(L"SUVTITLE_TEST_NO_MOUSELEAVE"/g) === 1
      && /if \(g_quiet\) \{\s*wchar_t v\[4\] = \{\};\s*g_noTrackLeave = ::GetEnvironmentVariableW\(L"SUVTITLE_TEST_NO_MOUSELEAVE", v, 4\) == 1 && v\[0\] == L'1';\s*\}/.test(S.main),
  [{ file: "main", find: "    if (g_quiet) {\n        wchar_t v[4] = {};", replace: "    if (true) {\n        wchar_t v[4] = {};", why: "the user's SuVtitle reads the test variable (an environment switches its mouse-leave off)" }]);

pin("G3", "RV-NH LOW13", "a test instance never takes the user's real mouse or cursor: its only SetCapture is skipped under g_quiet, and the arrow stays when the page hides the cursor",
  (S) => /if \(!g_quiet && ::GetCapture\(\) != g_hwnd\) ::SetCapture\(g_hwnd\);/.test(S.main)
      && count(codeOnly(S.main), /::SetCapture\(/g) === 1
      && /if \(g_pageCursorHidden && !g_viewsLost\) return g_quiet \? ::LoadCursorW\(nullptr, IDC_ARROW\) : nullptr;/.test(defBody(S.main, "HCURSOR cursorToSet")),
  [{ file: "main", find: "if (!g_quiet && ::GetCapture() != g_hwnd) ::SetCapture(g_hwnd);", replace: "if (::GetCapture() != g_hwnd) ::SetCapture(g_hwnd);", why: "a POSTED press captures the user's real mouse" },
   { file: "main", find: "return g_quiet ? ::LoadCursorW(nullptr, IDC_ARROW) : nullptr;", replace: "return nullptr;", why: "a test instance hides the user's cursor" }]);

pin("G4", "RV-NH LOW13", "a test instance is SILENT: startNative() passes silent = g_quiet, and MpvPlayer then sets ao=null",
  (S) => /o\.silent = g_quiet;/.test(defBody(S.main, "void startNative"))
      && /if \(I\.opt\.silent\) opt\("ao", "null"\);/.test(defBody(S.mpv, "bool MpvPlayer::attach")),
  [{ file: "main", find: "o.silent = g_quiet;", replace: "o.silent = false;", why: "a test instance plays out loud" },
   { file: "mpv", find: 'if (I.opt.silent) opt("ao", "null");', replace: "(void)0;", why: "silent no longer means ao=null" }]);

// ================= O: mpv's locked options ==================================================================================
const LOCKED = [["config", "no"], ["load-scripts", "no"], ["ytdl", "no"], ["access-references", "no"], ["input-default-bindings", "no"],
  ["input-builtin-bindings", "no"], ["input-vo-keyboard", "no"], ["resume-playback", "no"], ["save-position-on-quit", "no"],
  ["directory-mode", "ignore"], ["audio-file-auto", "no"], ["sub-auto", "no"]];
pin("O1", "RV-NH LOW13 + HIGH2", "mpv's LOCKED options, set in attach() between mpv_create and mpv_initialize: nothing of the user's mpv (mpv.conf, scripts, ytdl, key bindings, watch-later), NO reference a file names is followed (access-references=no: a local playlist / HLS manifest / EDL / cue sheet never makes mpv open a URL), no sibling file or folder loaded by itself",
  (S) => {
    const region = codeOnly(between(defBody(S.mpv, "bool MpvPlayer::attach"), "I.h = g_api.create();", "g_api.initialize(I.h)"));
    const missing = LOCKED.filter(([k, v]) => count(region, new RegExp('\\bopt\\("' + k + '", "' + v + '"\\);', "g")) !== 1);
    return [region.length > 0 && missing.length === 0, "missing: " + missing.map(([k]) => k).join(", ")];
  },
  LOCKED.map(([k, v]) => ({ file: "mpv", find: 'opt("' + k + '", "' + v + '");', replace: "(void)0;", why: k + "=" + v + " no longer set" }))
    .concat([{ file: "mpv", find: 'opt("access-references", "no");', replace: 'opt("access-references", "yes");', why: "references followed again (the review's HIGH2: GET /from_m3u.mkv)" }]));

pin("O2", "RV-NH LOW13", "the app never adds mpv options of its own: startNative() sets only silent + logPath (MpvOptions::extra, applied LAST, and forceWarp are the tests')",
  (S) => {
    const b = codeOnly(defBody(S.main, "void startNative"));
    return b.length > 0 && /o\.logPath = nativeLogPath\(\);/.test(b) && count(codeOnly(S.main), /\.extra\b|forceWarp/g) === 0;
  },
  [{ file: "main", find: "    o.logPath = nativeLogPath();\n", replace: "    o.logPath = nativeLogPath();\n    o.extra.push_back(\"access-references=yes\");\n", why: "the app passes an option after the locked ones (it would override them)" }]);

// ================= L: the LoadLibraryExW flags ==============================================================================
pin("L1", "RV-NH LOW13", "libmpv-2.dll is loaded by FULL PATH with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS (its imports: its own folder, the app's, System32 -- never the current directory or PATH)",
  (S) => {
    const b = defBody(S.mpv, "bool MpvPlayer::load");
    return /path \+= kDllName;/.test(b) && /HMODULE m = ::LoadLibraryExW\(path\.c_str\(\), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS\);/.test(b);
  },
  [{ file: "mpv", find: "::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)", replace: "::LoadLibraryExW(path.c_str(), nullptr, 0)", why: "libmpv's imports resolved by the standard search (the current directory, PATH)" },
   { file: "mpv", find: "::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)", replace: "::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)", why: "the altered search path (the current directory again)" }]);

pin("L2", "N3's Vulkan rule", "the Vulkan loader: the PC's own in System32 FIRST (then nothing is loaded); else <dllDir>\\vkfallback\\vulkan-1.dll by FULL PATH with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 -- and load() runs it BEFORE libmpv (whose import of vulkan-1.dll then binds to it)",
  (S) => {
    const b = defBody(S.mpv, "std::string preloadVulkanFallback"), l = defBody(S.mpv, "bool MpvPlayer::load");
    const sys = b.indexOf('return "the system\'s (System32)";');
    const ld = b.indexOf("::LoadLibraryExW(fb.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)");
    const pre = l.indexOf("I.vulkan = preloadVulkanFallback(dllDir);"), lib = l.indexOf("HMODULE m = ::LoadLibraryExW(path.c_str()");
    return [/::GetSystemDirectoryW\(sys, MAX_PATH\)/.test(b) && sys > 0 && ld > sys
      && /const std::wstring fb = d \+ L"\\\\vkfallback\\\\vulkan-1\.dll";/.test(b) && pre > 0 && lib > pre, { sys, ld, pre, lib }];
  },
  [{ file: "mpv", find: "::LoadLibraryExW(fb.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)", replace: "::LoadLibraryExW(fb.c_str(), nullptr, 0)", why: "the bundled loader's own imports searched in the current directory" },
   { file: "mpv", find: "        return \"the system's (System32)\";\n", replace: "        (void)0;\n", why: "the bundled loader loaded even where the PC has its own (it would beat the driver's)" },
   { file: "mpv", find: "    I.vulkan = preloadVulkanFallback(dllDir);\n", replace: "", why: "the fallback never pre-loaded (libmpv cannot start on a PC without a loader)" }]);

pin("L3", "RV-NH LOW13", "the GPU probe loads d3d11.dll from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32)",
  (S) => /HMODULE d3d = ::LoadLibraryExW\(L"d3d11\.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32\);/.test(S.mpv),
  [{ file: "mpv", find: '::LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)', replace: '::LoadLibraryW(L"d3d11.dll")', why: "d3d11.dll by name through the process's search" },
   { file: "mpv", find: '::LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)', replace: '::LoadLibraryExW(L"d3d11.dll", nullptr, 0)', why: "d3d11.dll without a search flag" }]);

pin("L4", "RV-NH LOW13", "no other library loading in the host's engine code: no LoadLibrary / LoadLibraryW / LoadLibraryA, and EVERY LoadLibraryExW names a LOAD_LIBRARY_SEARCH_* flag (MpvPlayer.cpp, main.cpp)",
  (S) => {
    const code = codeOnly(S.mpv) + "\n" + codeOnly(S.main);
    const ex = code.match(/LoadLibraryExW\([^;]*;/g) || [];
    return [count(code, /\bLoadLibrary[WA]?\s*\(/g) === 0 && ex.length >= 3 && ex.every((c) => /LOAD_LIBRARY_SEARCH_/.test(c)), ex];
  },
  [{ file: "mpv", find: '::LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)', replace: '::LoadLibraryW(L"d3d11.dll")', why: "a bare LoadLibraryW" },
   { file: "main", find: "    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();\n", replace: "    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();\n    ::LoadLibraryExW(L\"uiautomationcore.dll\", nullptr, 0);\n", why: "a LoadLibraryExW with no search flag" }]);

// ================= D: the DLL search, LOCKED; the Vulkan loader's place =====================================================
pin("D1", "RV-NH MEDIUM4", "lockDllSearch(): SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) -- the exe's folder, System32 and the added folders, NEVER the current directory -- then AddDllDirectory(<exe dir>\\mpv) only",
  (S) => {
    const b = defBody(S.mpv, "bool MpvPlayer::lockDllSearch");
    return /if \(!::SetDefaultDllDirectories\(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS\)\) return false;/.test(b)
        && /const std::wstring sub = exeDir\(\) \+ L"\\\\mpv";/.test(b)
        && /if \(a != INVALID_FILE_ATTRIBUTES && \(a & FILE_ATTRIBUTE_DIRECTORY\)\) ::AddDllDirectory\(sub\.c_str\(\)\);/.test(b);
  },
  [{ file: "mpv", find: "    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return false;\n", replace: "", why: "the search never locked (the current directory stays on it)" },
   { file: "mpv", find: "::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)", replace: "::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS)", why: "System32 dropped (WebView2, D3D, UIA loaded by name would fail)" },
   { file: "mpv", find: "    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ::AddDllDirectory(sub.c_str());\n", replace: "", why: "<exe dir>\\mpv\\ not searched" }]);

pin("D2", "RV-NH MEDIUM4", "the lock is wWinMain's FIRST statement (before anything is loaded by name: the DPI calls, OLE, WebView2...), and native.log says whether it held (startNative, once the log is open)",
  (S) => {
    const b = defBody(S.main, "int WINAPI wWinMain"), first = codeOnly(b.slice(1)).replace(/^\s+/, "");
    return [b.length > 0 && first.startsWith("g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();")
      && /\n\s*g_native->diag\(g_dllLocked \? "DLL search locked: /.test(defBody(S.main, "void startNative")), first.slice(0, 60)];
  },
  [{ file: "main", find: "    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();\n", replace: "", why: "never locked" },
   { file: "main", find: /(    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch\(\);\n)([\s\S]*?#endif\n)/, replace: "$2$1", why: "locked AFTER the DPI awareness (user32 / shcore are loaded by then)" },
   { file: "main", find: "    g_native->diag(g_dllLocked ? ", replace: "    if (false) g_native->diag(g_dllLocked ? ", why: "native.log never says whether the search is locked" }]);

pin("D3", "N3's Vulkan rule", "vkfallback\\ is NEVER a DLL search folder: no AddDllDirectory / SetDllDirectory names it (the added folders come BEFORE System32: a loader there would beat the PC's own); AddDllDirectory is called once in the host, with <exe dir>\\mpv",
  (S) => {
    const code = codeOnly(S.mpv) + "\n" + codeOnly(S.main);
    return count(code, /AddDllDirectory\(/g) === 1 && count(code, /SetDllDirectory/g) === 0
        && !/(Add|Set)DllDirector[^;]*vkfallback/.test(code) && /const std::wstring sub = exeDir\(\) \+ L"\\\\mpv";/.test(S.mpv);
  },
  [{ file: "mpv", find: 'const std::wstring sub = exeDir() + L"\\\\mpv";', replace: 'const std::wstring sub = exeDir() + L"\\\\mpv\\\\vkfallback";', why: "vkfallback\\ added to the search (the bundled loader beats the driver's)" },
   { file: "mpv", find: "    I.vulkan = preloadVulkanFallback(dllDir);\n", replace: "    ::AddDllDirectory((dllDir + L\"\\\\vkfallback\").c_str());\n    I.vulkan = preloadVulkanFallback(dllDir);\n", why: "vkfallback\\ given to AddDllDirectory before libmpv loads" },
   { file: "main", find: "    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();\n", replace: "    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();\n    ::SetDllDirectoryW(L\"mpv\\\\vkfallback\");\n", why: "SetDllDirectoryW on the fallback folder" }]);

pin("D4", "N3's Vulkan rule", "vulkan-1.dll never sits next to the exe or in <exe>\\mpv\\: the app's build copies ONLY libmpv-2.dll into mpv\\ (nothing named vulkan), and the bundle ships the loader only in mpv\\vkfallback\\ -- the very folder MpvPlayer pre-loads from (<exe>\\mpv\\vkfallback\\vulkan-1.dll)",
  (S) => {
    const cm = cmakeCode(S.cmake);
    return [!/vulkan/i.test(cm) && /COMMAND \$\{CMAKE_COMMAND\} -E copy_if_different "\$\{_mpv_dir\}\/libmpv-2\.dll" "\$\{_mpv_dst\}"/.test(cm)
      && /set\(_mpv_dst "\$\{CMAKE_BINARY_DIR\}\/mpv\/libmpv-2\.dll"\)/.test(cm)
      && /const MPV_DIR = 'mpv';/.test(S.bundle) && /const VKFB_DIR = 'mpv\\\\vkfallback';/.test(S.bundle)
      && /const std::wstring fb = d \+ L"\\\\vkfallback\\\\vulkan-1\.dll";/.test(S.mpv)
      && /const std::wstring sub = dir \+ L"\\\\mpv";/.test(defBody(S.mpv, "std::wstring MpvPlayer::defaultDllDir")), "cmake / bundle / MpvPlayer"];
  },
  [{ file: "cmake", find: '        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mpv_dir}/libmpv-2.dll" "${_mpv_dst}"\n', replace: '        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mpv_dir}/libmpv-2.dll" "${_mpv_dst}"\n        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mpv_dir}/vulkan-1.dll" "${CMAKE_BINARY_DIR}/mpv/vulkan-1.dll"\n', why: "the dev build puts a vulkan-1.dll in mpv\\ (a searched folder: it beats the driver's)" },
   { file: "bundle", find: "const VKFB_DIR = 'mpv\\\\vkfallback';", replace: "const VKFB_DIR = 'mpv';", why: "the bundle ships vulkan-1.dll next to libmpv" },
   { file: "mpv", find: 'const std::wstring fb = d + L"\\\\vkfallback\\\\vulkan-1.dll";', replace: 'const std::wstring fb = d + L"\\\\vulkan-1.dll";', why: "the host looks for the fallback next to libmpv (not where the bundle puts it)" }]);

// ================= A: the item rules of the fix round (their behaviour: mpv_smoke [AID] [HOLD] [EOF2]) ======================
pin("A1", "RV-NH HIGH1", "EVERY open() passes the sound track FILE-LOCALLY in loadfile's named options -- aid=auto when none is asked: a switch made during an item never outlives it",
  (S) => {
    const b = defBody(S.mpv, "void MpvPlayer::open");
    return /std::string opts = ord >= 0 \? fmt\("aid=%d", ord \+ 1\) : std::string\("aid=auto"\);/.test(b)
        && /const char\* keys\[4\] = \{"name", "url", "flags", "options"\};/.test(b)
        && /std::string vals\[4\] = \{"loadfile", url, "replace", opts\};/.test(b)
        && /mpv_node_list list\{4, values, const_cast<char\*\*>\(keys\)\};/.test(b);
  },
  [{ file: "mpv", find: ': std::string("aid=auto");', replace: ": std::string();", why: "no aid when none is asked: a runtime switch carries over to the next item (the review's HIGH1)" }]);

pin("A2", "RV-NH MEDIUM3", "asked while the item LOADS (mpv refuses a seek then): seek(), setAbLoop's jump to A and frameStep() go through askSeek / askFrame -- held while loadingGen (the LAST one wins), a seek done at THAT item's file-loaded, a frame step at its first restart (mpv ignores a back step during its start seek); a new open() drops what an older item held",
  (S) => {
    const ok1 = /impl_->askSeek\(/.test(defBody(S.mpv, "void MpvPlayer::seek")) && /I\.askSeek\(aMs, true\);/.test(defBody(S.mpv, "void MpvPlayer::setAbLoop"))
      && /impl_->askFrame\(step\);/.test(defBody(S.mpv, "void MpvPlayer::frameStep"));
    const ok2 = /if \(loadingGen\) \{\s*held = Act\{1, ms, exact, 0\};\s*return;\s*\}/.test(defBody(S.mpv, "void askSeek"))
      && /if \(loadingGen\) \{\s*held = Act\{2, 0, true, step\};\s*return;\s*\}/.test(defBody(S.mpv, "void askFrame"));
    const ok3 = /if \(loadingGen && curGen == loadingGen\) \{\s*loadingGen = 0;\s*if \(held\.kind == 2\) stepAtRestart = curGen;[^\n]*\n\s*else replayHeld\(\);\s*\}/.test(defBody(S.mpv, "void fileLoaded"))
      && /if \(stepAtRestart && stepAtRestart == curGen\) \{[^\n]*\n\s*stepAtRestart = 0;\s*replayHeld\(\);\s*\}/.test(S.mpv)
      && /I\.loadingGen = gen;\s*I\.held = Impl::Act\{\};\s*I\.stepAtRestart = 0;/.test(defBody(S.mpv, "void MpvPlayer::open"));
    return [ok1 && ok2 && ok3, { ok1, ok2, ok3 }];
  },
  [{ file: "mpv", find: "I.askSeek(aMs, true);", replace: "I.seekTo(aMs, true);", why: "setAbLoop's jump to A sent while the item loads (refused by mpv: lost)" },
   { file: "mpv", find: "            held = Act{1, ms, exact, 0};\n            return;\n", replace: "", why: "a seek during a load sent at once (lost)" },
   { file: "mpv", find: "            else replayHeld();\n", replace: "", why: "a held seek is never done" },
   { file: "mpv", find: "            if (held.kind == 2) stepAtRestart = curGen;", replace: "            if (false) stepAtRestart = curGen;", why: "a held frame step done at file-loaded (mpv ignores it during its start seek)" },
   { file: "mpv", find: "    I.loadingGen = gen;\n    I.held = Impl::Act{};\n", replace: "    I.loadingGen = gen;\n", why: "an older item's held seek is done on the new one" }]);

pin("A3", "RV-NP L1 (host half)", "play() at the end restarts from 0 only when NO seek was asked since that end (seekSinceEof: set by askSeek, cleared by a new eof) and nothing loads",
  (S) => /if \(I\.st\.eof && !I\.seekSinceEof && !I\.loadingGen\) I\.seekTo\(0, true\);/.test(defBody(S.mpv, "void MpvPlayer::play"))
      && /seekSinceEof = true;/.test(defBody(S.mpv, "void askSeek")) && /if \(v\) seekSinceEof = false;/.test(S.mpv),
  [{ file: "mpv", find: "if (I.st.eof && !I.seekSinceEof && !I.loadingGen) I.seekTo(0, true);", replace: "if (I.st.eof) I.seekTo(0, true);", why: "play() restarts from 0 over a seek asked at the end (the review's L1)" },
   { file: "mpv", find: "        seekSinceEof = true;\n", replace: "", why: "a seek no longer counts" }]);

// ================= T: the track list (the contract's audioList: the FILE's strings) =========================================
pin("T1", "fix round (contract)", "the track list's strings are the FILE's (untrusted): lang / title / codec go through fileText -- control characters out, capped (16 / 160 / 32 bytes) at a UTF-8 boundary; external tracks and cover art are not the item's",
  (S) => {
    const b = defBody(S.mpv, "void applyTracks");
    return /a\.lang = fileText\(n && n->format == MPV_FORMAT_STRING \? n->u\.string : nullptr, 16\);/.test(b)
        && /a\.title = fileText\(n && n->format == MPV_FORMAT_STRING \? n->u\.string : nullptr, 160\);/.test(b)
        && /a\.codec = fileText\(n && n->format == MPV_FORMAT_STRING \? n->u\.string : nullptr, 32\);/.test(b)
        && /if \(nodeFlag\(mapGet\(t, "external"\)\)\) continue;/.test(b) && /if \(type == "video" && !nodeFlag\(mapGet\(t, "albumart"\)\)\) picture = true;/.test(b)
        && /if \(c < 0x20 \|\| c == 0x7f\) continue;/.test(defBody(S.mpv, "std::string fileText"))
        && /for \(const char\* p = s; \*p && o\.size\(\) < cap; \+\+p\)/.test(defBody(S.mpv, "std::string fileText"));
  },
  [{ file: "mpv", find: "a.title = fileText(n && n->format == MPV_FORMAT_STRING ? n->u.string : nullptr, 160);", replace: "a.title = (n && n->format == MPV_FORMAT_STRING) ? std::string(n->u.string) : std::string();", why: "a track title taken raw from the file (any length, control characters)" },
   { file: "mpv", find: 'if (type == "video" && !nodeFlag(mapGet(t, "albumart"))) picture = true;', replace: 'if (type == "video") picture = true;', why: "an mp3's cover counted as a picture (a sound file shown as a video)" }]);

// ================= H: host fixes no headless test reaches (the real-app kit runs the windowed paths) ========================
pin("H1", "RV-NH LOW5", "a mouse-leave clears the tracking even while there is no view (a browser rebuild): set BEFORE forwardMouse's controller check, or TME_LEAVE is never armed again",
  (S) => /^\s*if \(msg == WM_MOUSELEAVE\) g_mouseTracked = false;\n\s*if \(!g_compController\) return false;/m.test(defBody(S.main, "bool forwardMouse")),
  [{ file: "main", find: "    if (msg == WM_MOUSELEAVE) g_mouseTracked = false;\n    if (!g_compController) return false;\n", replace: "    if (!g_compController) return false;\n    if (msg == WM_MOUSELEAVE) g_mouseTracked = false;\n", why: "a leave during views-lost keeps the tracking set for good" }]);

pin("H2", "RV-NH LOW6", "the WINDOWED view only when visual hosting is UNSUPPORTED (E_NOINTERFACE / E_NOTIMPL / ERROR_NOT_SUPPORTED); any other failure goes to onController's failure path (a rebuild retries); the windowed creation's own refusal is checked; the view's visual is removed from the topmost target + committed at the end",
  (S) => /return r == E_NOINTERFACE \|\| r == E_NOTIMPL \|\| r == HRESULT_FROM_WIN32\(ERROR_NOT_SUPPORTED\);/.test(S.main)
      && /if \(!c && egen == g_envGen && unsupported\(r3\)\) \{ windowed\(\); return S_OK; \}/.test(S.main)
      && /if \(FAILED\(made\)\) \{\s*if \(unsupported\(made\)\) windowed\(\);\s*else onController\(made, nullptr, nullptr\);\s*\}/.test(S.main)
      && /if \(FAILED\(w\)\) onController\(w, nullptr, nullptr\);/.test(S.main)
      && /if \(g_dcompRoot && g_dcompView\) g_dcompRoot->RemoveVisual\(g_dcompView\.Get\(\)\);\s*if \(g_dcomp\) g_dcomp->Commit\(\);\s*g_dcompView\.Reset\(\);/.test(S.main),
  [{ file: "main", find: "if (!c && egen == g_envGen && unsupported(r3)) { windowed(); return S_OK; }", replace: "if (!c && egen == g_envGen) { windowed(); return S_OK; }", why: "any failure (a transient one in a rebuild) switches the session to windowed and detaches mpv" },
   { file: "main", find: "                    else onController(made, nullptr, nullptr);\n", replace: "", why: "a synchronous failure leaves no view and the rebuild stuck" },
   { file: "main", find: "                    if (FAILED(w)) onController(w, nullptr, nullptr);\n", replace: "", why: "the windowed creation's refusal ignored" },
   { file: "main", find: "        if (g_dcompRoot && g_dcompView) g_dcompRoot->RemoveVisual(g_dcompView.Get());\n", replace: "", why: "the view's visual left in the topmost target at the end" }]);

pin("H3", "RV-NH LOW11", "the drag-image helper is told the PAGE's answer (*effect after the view's DragEnter / DragOver / Drop), not the effects the source allows",
  (S) => /const HRESULT hr = c3 \? c3->DragEnter\(data, keys, client\(at\), effect\) : S_OK;\s*if \(!c3\) \*effect = DROPEFFECT_NONE;\s*if \(helper_\) helper_->DragEnter\(hwnd_, data, &sp, \*effect\);/.test(S.main)
      && /const HRESULT hr = c3 \? c3->Drop\(data, keys, client\(at\), effect\) : S_OK;\s*if \(!c3\) \*effect = DROPEFFECT_NONE;\s*if \(helper_\) helper_->Drop\(data, &sp, \*effect\);/.test(S.main),
  [{ file: "main", find: "        if (helper_) helper_->DragEnter(hwnd_, data, &sp, *effect);\n", replace: "        if (helper_) helper_->DragEnter(hwnd_, data, &sp, DROPEFFECT_COPY | DROPEFFECT_LINK);\n", why: "the badge shows what the source allows, not what the page will do" }]);

pin("H4", "RV-NH LOW12", "a capture lost with a button down (Alt+Tab during a seek-bar drag) gives the page that button's UP: WM_CAPTURECHANGED to another window -> releaseHeldButtons()",
  (S) => /case WM_CAPTURECHANGED:\s*if \(reinterpret_cast<HWND>\(lParam\) != hwnd\) releaseHeldButtons\(\);/.test(S.main)
      && /g_compController->SendMouseInput\(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>\(u\.msg\),/.test(defBody(S.main, "void releaseHeldButtons")),
  [{ file: "main", find: "        if (reinterpret_cast<HWND>(lParam) != hwnd) releaseHeldButtons();\n", replace: "", why: "the page never gets the button-up: its seek bar drags on" }]);

/*@@PINS-END@@*/

// ---- the runner (test_host_pins.js's) ---------------------------------------------------------------------------------------
function evalPin(p, S) {
  try { const r = p.test(S); return Array.isArray(r) ? { ok: !!r[0], extra: r[1] } : { ok: !!r }; }
  catch (e) { return { ok: false, extra: "threw: " + e.message }; }
}
// one mutant on a copy of the sources: it must apply EXACTLY once (a string, or a regular expression without the g flag)
function mutate(S, m) {
  const text = S[m.file];
  if (text == null) return { err: FILES[m.file] + " missing" };
  const n = typeof m.find === "string" ? text.split(m.find).length - 1 : (text.match(new RegExp(m.find.source, "g" + m.find.flags.replace("g", ""))) || []).length;
  if (n !== 1) return { err: "applies " + n + " times (must be exactly 1): " + String(m.find).slice(0, 80) };
  const out = Object.assign({}, S);
  out[m.file] = typeof m.find === "string" ? text.replace(m.find, () => m.replace) : text.replace(m.find, m.replace);
  return { S: out };
}

const argv = process.argv.slice(2);
const S0 = readSources();
const missing = Object.entries(S0).filter(([, v]) => v == null).map(([k]) => FILES[k]);
if (argv.includes("--list")) {
  PINS.forEach((p) => console.log(p.id.padEnd(4) + " [" + p.from + "] " + p.name));
  process.exit(0);
}
let pass = 0, fail = 0, mutPass = 0, mutFail = 0;
console.log("test_mpv_pins: " + PINS.length + " pins over " + Object.keys(FILES).length + " sources of " + ROOT);
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
console.log((bad ? "TEST_MPV_PINS FAILED" : "TEST_MPV_PINS OK") + " -- " + pass + " pins passed, " + fail + " failed"
  + (argv.includes("--no-selftest") ? "" : "; " + mutPass + " mutants caught, " + mutFail + " not"));
process.exit(bad ? 1 : 0);

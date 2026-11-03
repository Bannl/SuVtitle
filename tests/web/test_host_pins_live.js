// test_host_pins_live.js -- the HOST-SIDE SOURCE PINS of SuVtitle 1.1.0's LIVE mode (docs\LIVE_PLAN.md 3.3 / 3.4 / 4.3).
// L2 step 7, 2026-10-25 (test agent).  Standalone node, nothing to install, the style of tests\web\test_host_pins.js:
//   node tests\web\test_host_pins_live.js [--no-selftest] [--list]     exit 0 = every pin passes AND every mutant is caught
//   SUV_APP=<folder> node tests\web\test_host_pins_live.js             reads another copy of app\ (the CRLF / LF proof, see the log)
//   (SUV_COMMON=<folder> names another common\ for TrPipe.h; by default the one next to SUV_APP)
//
// WHY. The page's harness (test_live.js) runs live.html against a stub host, and the C++ smokes run the PURE parts (AppConfig.h,
// TrLaunch.h, TranslatorManager, TrPlayer's gate). What nothing can run headless is the WIRING of the live contract inside the
// two files that need a window and a WebView2: bridge.cpp (who is told what, in which order, from which view) and main.cpp
// (the Live window's life, the CSP of each page, ProcessFailed, --live). A careless edit of those is the regression that costs a
// meeting: LINE sent to the wrong view, a lane named by its device, a page-supplied key reaching the engine, a refused
// liveConfig applied anyway, a closed window that goes on listening. This file pins those, as regular expressions or literal
// fragments over the NORMALISED source, scoped to one function body where it can be.
//
// HOW. Same machine as test_host_pins.js: a pin must PASS on today's sources and must be ABLE TO FAIL -- the self-test applies
// to each pin one or more MUTANTS (the pinned guard undone in the source text, as a careless edit would) and requires the pin
// to FAIL on every one; a mutant must apply EXACTLY once, or the self-test itself fails; a pin with no mutant fails too. When a
// pin fails on a legitimate change of the host, EDIT THE PIN WITH THE CHANGE (and its mutant): the fragment is the rule.
// Two differences from the older file: the fragments are compared WHITESPACE-INSENSITIVELY (has / ordered) on the source with
// its COMMENTS BLANKED (a comment can neither satisfy nor break a pin), and function bodies are found by a brace matcher that
// skips string and character literals (the JSON the bridge builds is full of unbalanced braces inside strings).
//
// 64 pins: R per-view routing - L lane ids (R1) - C liveConfig / Listen - B the label rule, colours, ASCII - E export + journal
// - P LIVEPOS / LIVEIN persistence - S CSP per page + the Live view - F ProcessFailed - V --live - W the Live window's life -
// T the engine supervisor's live half - G the player's live gate - X the FIX ROUND of 26/10 (docs\REVIEW_RV-LB.md, LIVE_PLAN.md
// "ADDENDUM FIX ROUND": the session kept, a refused LIVE, voices by incarnation, the picker's ask, late deaths, the journal's
// debt, the trim's debounce, the test-only Export answer, recovery without activation, the drain on the last window, '|', the
// environment generation that keeps two rebuilds from racing -- the crash the real run found).
// The C++ side of the same contract (the pure parts, the supervisor, the gate) is proved by RUNNING the smokes and by their
// mutants: tests\cpp\cmut_live.py.
//
// (!) TOOL TRAP: a backslash-u typed into an Edit/Write input is DECODED to the character by the editing tools, so this file
// never types one: the escapes the C++ holds (the middle dot, the em dash, the double arrow) are built from BS below.
"use strict";
const fs = require("fs");
const path = require("path");

const APP = process.env.SUV_APP ? path.resolve(process.env.SUV_APP) : path.resolve(__dirname, "..", "..", "app");
const COMMON = process.env.SUV_COMMON ? path.resolve(process.env.SUV_COMMON) : path.resolve(APP, "..", "common");
const FILES = {
  bridge: path.join(APP, "bridge.cpp"), bridgeH: path.join(APP, "bridge.h"), main: path.join(APP, "main.cpp"),
  player: path.join(APP, "TrPlayer.h"), mgr: path.join(APP, "TranslatorManager.cpp"), mgrH: path.join(APP, "TranslatorManager.h"),
  launch: path.join(APP, "TrLaunch.h"), config: path.join(APP, "AppConfig.h"), import: path.join(APP, "AudioPBImport.h"),
  store: path.join(APP, "TrFileStore.h"), live: path.join(APP, "web", "live.html"),
  pipe: path.join(COMMON, "TrPipe.h"),
};
const BS = String.fromCharCode(92);                 // one backslash
const DOT = BS + "u00B7", DASH = BS + "u2014", ARROW = BS + "u2194";   // the C++ escapes (the build passes no /utf-8)

function readSources() {
  const S = {};
  for (const [k, f] of Object.entries(FILES)) {
    try { S[k] = fs.readFileSync(f, "utf8").replace(/\r\n?/g, "\n"); } catch { S[k] = null; }   // (a checkout may be CRLF or LF)
  }
  return S;
}
// ---- the C++ reading helpers -------------------------------------------------------------------------------------------------
// the index after a string / character literal that starts at i (i itself when none does)
function skipLit(s, i) {
  const c = s[i];
  if (c === '"') { i++; while (i < s.length && s[i] !== '"') { if (s[i] === BS) i++; i++; } return i + 1; }
  if (c === "'") { i++; while (i < s.length && s[i] !== "'" && s[i] !== "\n") { if (s[i] === BS) i++; i++; } return i + 1; }
  return i;
}
// the same text with every comment blanked to spaces (same length, same lines): string literals are kept and respected
function blank(src) {
  const out = src.split(""), n = src.length;
  let i = 0;
  while (i < n) {
    const c = src[i], d = src[i + 1];
    if (c === "/" && d === "/") { while (i < n && src[i] !== "\n") out[i++] = " "; continue; }
    if (c === "/" && d === "*") {
      const e = src.indexOf("*/", i + 2), end = e < 0 ? n : e + 2;
      for (let k = i; k < end; k++) if (src[k] !== "\n") out[k] = " ";
      i = end; continue;
    }
    if (c === '"' || c === "'") { i = skipLit(src, i); continue; }
    i++;
  }
  return out.join("");
}
const blankCache = new Map();
const blankOf = (t) => { let b = blankCache.get(t); if (b === undefined) { b = blank(t); blankCache.set(t, b); } return b; };
const codeOf = (S) => { const C = {}; for (const k of Object.keys(S)) C[k] = S[k] == null ? null : blankOf(S[k]); return C; };

// the body of the function DEFINED as `sig(...) {`: sig = "void Bridge::handleLiveConfig". A declaration (`;`) is stepped over.
function fnBody(code, sig) {
  if (code == null) return "";
  let from = 0;
  for (;;) {
    const at = code.indexOf(sig + "(", from);
    if (at < 0) return "";
    from = at + sig.length;
    let i = from, d = 0;
    for (; i < code.length; ++i) {                                                 // the parameter list
      if (code[i] === '"' || code[i] === "'") { i = skipLit(code, i) - 1; continue; }
      if (code[i] === "(") d++; else if (code[i] === ")" && --d === 0) { i++; break; }
    }
    let j = i;
    for (;;) {                                                                     // blanks and qualifiers
      while (j < code.length && /\s/.test(code[j])) j++;
      const q = /^(?:const|noexcept)\b/.exec(code.slice(j, j + 9));
      if (q) { j += q[0].length; continue; }
      break;
    }
    if (code[j] !== "{") continue;                                                 // a declaration, or a call
    d = 0;
    for (i = j; i < code.length; ++i) {
      const c = code[i];
      if (c === '"' || c === "'") { i = skipLit(code, i) - 1; continue; }
      if (c === "{") d++; else if (c === "}" && --d === 0) break;
    }
    return code.slice(j, i + 1);
  }
}
const norm = (s) => s.replace(/\s+/g, " ").replace(/ ?([(){};,]) ?/g, "$1");        // whitespace-insensitive comparison
const has = (src, lit) => src != null && src !== "" && norm(src).includes(norm(lit));
const ordered = (src, lits) => {                                                    // every fragment, in this order
  if (src == null || src === "") return false;
  const n = norm(src);
  let at = -1;
  for (const l of lits) { const k = n.indexOf(norm(l), at + 1); if (k < 0) return false; at = k; }
  return true;
};
const count = (src, re) => (src == null ? 0 : (src.match(re) || []).length);
// the C string constant `name` (adjacent literals joined), or null
function cstr(code, name) {
  const m = new RegExp("constexpr wchar_t " + name + "\\[\\]\\s*=((?:\\s*L\"[^\"]*\")+)\\s*;").exec(code || "");
  return m ? [...m[1].matchAll(/L"([^"]*)"/g)].map((x) => x[1]).join("") : null;
}

// ---- the pins --------------------------------------------------------------------------------------------------------------
// pin(id, from, name, test, mutants): test(S, C) -> true | false | [bool, detail]  (S = the sources, C = the same, comments blanked)
const PINS = [];
const pin = (id, from, name, test, mutants) => PINS.push({ id, from, name, test, mutants: mutants || [] });
const B = (C) => C.bridge;

// ================= R: per-view routing (LIVE_PLAN 3.3) =====================================================================
pin("R1", "3.3 per-view routing", "LINE / PART / END / the meters go to the LIVE view ONLY: pollTranslatorOnce posts them with postToLive and never with postToWeb (the player's page has no live line)",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return poll.length > 3000
      && has(poll, 'postToLive(L"{\\"type\\":\\"translatorEnd\\",\\"segId\\":"')
      && has(poll, 'pj += L"}"; postToLive(pj);')
      && has(poll, "appendTranslatorSession(json); postToLive(json);")
      && has(poll, 'postToLive(j + L"}}");')
      && !/postToWeb\(/.test(poll);
  },
  [{ file: "bridge", find: "postToLive(pj);", replace: "postToWeb(pj);", why: "a draft (PART) reaches the PLAYER's page" },
   { file: "bridge", find: "appendTranslatorSession(json);\n        postToLive(json);", replace: "appendTranslatorSession(json);\n        postToWeb(json);", why: "a finished line goes to the player's page, the Live window hears nothing" },
   { file: "bridge", find: 'postToLive(j + L"}}");', replace: 'postToWeb(j + L"}}");', why: "the meters go to the player's page" },
   { file: "bridge", find: 'postToLive(L"{\\"type\\":\\"translatorEnd\\"', replace: 'postToWeb(L"{\\"type\\":\\"translatorEnd\\"', why: "the END of an utterance goes to the player's page: the live line never falls" }]);

pin("R2", "3.3 per-view routing", "...the picker's list, the scrollback replay, the export ack, the warnings and the window state go to the Live view too (postToLive) -- nothing of the live section posts to the player's view",
  (S, C) => {
    const live = C.bridge.slice(C.bridge.indexOf("void Bridge::startEngine"));
    return live.length > 20000
      && has(fnBody(C.bridge, "void Bridge::sendLiveSources"), 'postToLive(L"{\\"type\\":\\"liveSources\\",\\"gen\\":"')
      && has(fnBody(C.bridge, "void Bridge::replayTranslatorLog"), "for (const auto& line : translatorLog_) postToLive(line);")
      && has(fnBody(C.bridge, "void Bridge::postTranslatorExported"), "postToLive(j);")
      && has(fnBody(C.bridge, "void Bridge::postLiveWarn"), "postToLive(j.take());")
      && has(fnBody(C.bridge, "void Bridge::postLiveWindowState"), "postToLive(j.take());")
      && has(fnBody(C.bridge, "void Bridge::sendLiveState"), "postToLive(json);")
      && !/postToWeb\(/.test(live);
  },
  [{ file: "bridge", find: 'postToLive(L"{\\"type\\":\\"liveSources\\"', replace: 'postToWeb(L"{\\"type\\":\\"liveSources\\"', why: "the picker's list goes to the player's page" },
   { file: "bridge", find: "for (const auto& line : translatorLog_) postToLive(line);", replace: "for (const auto& line : translatorLog_) postToWeb(line);", why: "the scrollback is replayed into the player's page" },
   { file: "bridge", find: '    j += L"}";\n    postToLive(j);', replace: '    j += L"}";\n    postToWeb(j);', why: "the export's ack goes to the player's page: the Export button waits for ever" },
   { file: "bridge", find: 'j.obj().kv(L"type", L"warn").kv(L"text", text).end();\n    postToLive(j.take());', replace: 'j.obj().kv(L"type", L"warn").kv(L"text", text).end();\n    postToWeb(j.take());', why: "the Live window's warnings go to the player's page" },
   { file: "bridge", find: 'kv(L"topmost", liveTopmost_).end();\n    postToLive(j.take());', replace: 'kv(L"topmost", liveTopmost_).end();\n    postToWeb(j.take());', why: "the Live window's pin state is told to the player's page (its own pin flips)" },
   { file: "bridge", find: "lastLiveStateJson_ = json;\n    postToLive(json);", replace: "lastLiveStateJson_ = json;\n    postToWeb(json);", why: "the Live state (the LIVE pair) overwrites the player's page state" }]);

pin("R3", "3.3 `warn`", "the supervisor's warnings reach BOTH views (the player's notice and the Live window's warn{text}): a meeting must hear \"the engine stopped responding\"",
  (S, C) => has(fnBody(C.bridge, "void Bridge::pollTranslatorOnce"), "for (auto& w : translator_->pollWarnings()) { postTranslatorFileNotice(w); postLiveWarn(w); }"),
  [{ file: "bridge", find: "{ postTranslatorFileNotice(w); postLiveWarn(w); }", replace: "{ postTranslatorFileNotice(w); }", why: "the Live window never hears a warning" },
   { file: "bridge", find: "{ postTranslatorFileNotice(w); postLiveWarn(w); }", replace: "{ postLiveWarn(w); }", why: "the player's notice never hears a warning" }]);

pin("R4", "3.3 translatorCtl", "a translatorCtl from the LIVE view is the LIVE scope: setLiveMode / setLivePair (CTL scope=live), the shared preset, the Clear = askClear + a NEW session + `cleared` to the Live view -- and the player's CTL path never clears the engine",
  (S, C) => {
    const lc = fnBody(C.bridge, "void Bridge::handleLiveCtl"), pc = fnBody(C.bridge, "void Bridge::handleTranslatorCtl");
    return has(lc, "translator_->setLiveMode(wideToUtf8(m));") && has(lc, "translator_->setLivePair(wideToUtf8(na), wideToUtf8(nb));")
      && has(lc, "translator_->setPreset(wideToUtf8(preset));") && !/\bsetMode\(|\bsetPair\(/.test(lc)
      && has(lc, 'if (na != nb && !na.empty() && !nb.empty()) {')
      && has(lc, 'translator_->askClear(); endLiveSession(); postToLive(L"{\\"type\\":\\"translatorState\\",\\"cleared\\":true}"); }')
      && has(fnBody(C.bridge, "void Bridge::handleLiveMessage"), "handleLiveCtl(mode, preset, clear, langA, langB);")
      && has(pc, "translator_->setMode(wideToUtf8(m));") && has(pc, "translator_->setPair(wideToUtf8(na), wideToUtf8(nb));")
      && !/askClear/.test(pc) && count(C.bridge, /askClear\(\)/g) === 1;
  },
  [{ file: "bridge", find: "translator_->setLiveMode(wideToUtf8(m));", replace: "translator_->setMode(wideToUtf8(m));", why: "the Live window changes the PLAYER's direction" },
   { file: "bridge", find: "translator_->setLivePair(wideToUtf8(na), wideToUtf8(nb));", replace: "translator_->setPair(wideToUtf8(na), wideToUtf8(nb));", why: "the Live window changes the PLAYER's pair" },
   { file: "bridge", find: 'postToWeb(L"{\\"type\\":\\"translatorState\\",\\"cleared\\":true}");', replace: 'translator_->askClear();\n        postToWeb(L"{\\"type\\":\\"translatorState\\",\\"cleared\\":true}");', why: "the player's page clearing its screen wipes the live session's queue" },
   { file: "bridge", find: "translator_->askClear();\n        endLiveSession();", replace: "translator_->askClear();", why: "a Clear does not end the session: the next line is appended to the old journal (one .docx, two meetings)" },
   { file: "bridge", find: "if (na != nb && !na.empty() && !nb.empty()) {", replace: "if (!na.empty() && !nb.empty()) {", why: "the same language twice is sent as a pair (the engine refuses it, the mirror lies)" }]);

pin("R5", "3.3 / 4.3", "the Live view has its OWN handler (attachLiveView -> handleLiveMessage); the player's handlers never know liveConfig / liveListen / liveSources / the Live export, and handleLiveMessage has exactly ONE caller -- the view IS the scope",
  (S, C) => {
    const att = fnBody(C.bridge, "void Bridge::attachLiveView"), web = fnBody(C.bridge, "void Bridge::handleWebMessage"), pAtt = fnBody(C.bridge, "void Bridge::attach");
    return att.length > 100 && has(att, "handleLiveMessage(json);") && !/handleWebMessage/.test(att) && has(att, "startEngine();")
      && web.length > 1000 && pAtt.length > 500 && !/handleLiveMessage/.test(web + pAtt)
      && !/liveConfig|liveListen|liveSources|liveLevels|translatorExport/.test(web + pAtt)
      && count(C.bridge, /handleLiveMessage\(/g) === 2;                          // the definition + attachLiveView's call
  },
  [{ file: "bridge", find: "                handleLiveMessage(json);", replace: "                handleWebMessage(json);", why: "the Live view's messages are handled by the PLAYER's handler" },
   { file: "bridge", find: '    if (type == L"openLive") {', replace: '    if (type == L"liveListen") { bool on = false; jsonFindBool(json, L"on", on); handleLiveListen(on); return; }\n    if (type == L"openLive") {', why: "the PLAYER's page can start the capture (liveListen)" }]);

pin("R6", "3.3 `ready`", "`ready` (and an explicit translatorReplay) from the Live page: the page is marked ready, THEN its full state, the scrollback, the picker's list, the window's real pin -- the contract's order; a window state is posted only to a page that said `ready`",
  (S, C) => {
    const h = fnBody(C.bridge, "void Bridge::handleLiveMessage");
    return has(h, 'if (type == L"ready" || type == L"translatorReplay") {')
      && ordered(h, ["livePageReady_ = true;", "sendLiveState(true);", "replayTranslatorLog();", "sendLiveSources();", "postLiveWindowState();", "return;"])
      && has(fnBody(C.bridge, "void Bridge::postLiveWindowState"), "if (!liveView_ || !livePageReady_) return;")
      && has(fnBody(C.bridge, "void Bridge::detachLiveView"), "livePageReady_ = false;");
  },
  [{ file: "bridge", find: "        replayTranslatorLog();\n        sendLiveSources();\n        postLiveWindowState();", replace: "        sendLiveSources();\n        postLiveWindowState();\n        replayTranslatorLog();", why: "the replay arrives after the window state (the contract's order broken)" },
   { file: "bridge", find: "        livePageReady_ = true;\n        sendLiveState(/*force=*/true);", replace: "        sendLiveState(/*force=*/true);\n        livePageReady_ = true;", why: "the page is marked ready after its state is built (its window state is dropped)" },
   { file: "bridge", find: "sendLiveState(/*force=*/true);\n        replayTranslatorLog();", replace: "sendLiveState(/*force=*/false);\n        replayTranslatorLog();", why: "a reloaded page gets a DIFFED state: identical to the last one, so none at all" },
   { file: "bridge", find: "if (!liveView_ || !livePageReady_) return;", replace: "if (!liveView_) return;", why: "the window state is posted to a page that is still loading (lost)" }]);

pin("R7", "3.3 translatorState", "the Live view's translatorState is ITS OWN: the LIVE pair as mode / langA / langB (never the player's), the strips + lamps in `live`, diffed on its own (lastLiveStateJson_); the player's state carries `live` too and calls sendLiveState",
  (S, C) => {
    const ls = fnBody(C.bridge, "void Bridge::sendLiveState"), ts = fnBody(C.bridge, "void Bridge::sendTranslatorState");
    return has(ls, "jsonEscape(liveMode_)") && has(ls, "jsonEscape(liveLangA_)") && has(ls, "jsonEscape(liveLangB_)")
      && !/trMode_|trLangA_|trLangB_/.test(ls) && has(ls, 'L",\\"live\\":" + liveJson()')
      && has(ls, "if (!force && json == lastLiveStateJson_) return; lastLiveStateJson_ = json;")
      && has(ts, 'L",\\"live\\":" + liveJson()') && has(ts, "sendLiveState(force);")
      && has(ts, "if (!force && json == lastTranslatorStateJson_) return;");
  },
  [{ file: "bridge", find: "jsonEscape(liveMode_)", replace: "jsonEscape(trMode_)", why: "the Live window's direction is the PLAYER's" },
   { file: "bridge", find: "jsonEscape(liveLangA_)", replace: "jsonEscape(trLangA_)", why: "the Live window's language A is the PLAYER's" },
   { file: "bridge", find: "lastLiveStateJson_ = json;\n    postToLive(json);", replace: "lastTranslatorStateJson_ = json;\n    postToLive(json);", why: "the two states share one diff: one swallows the other" },
   { file: "bridge", find: "    sendLiveState(force);   // SUVTITLE LIVE: the Live view's own state (the LIVE pair), diffed on its own\n", replace: "", why: "the Live window never hears the engine's echo (the player's state is sent alone)" },
   { file: "bridge", find: 'both states carry.\n        + L",\\"live\\":" + liveJson()\n', replace: "both states carry.\n", why: "the player's page never learns that the live mode listens (no notice)" }]);

pin("R8", "3.3 liveLevels", "the meters: the NEWEST levels only (a latest-wins slot, polled -- never a queue), keyed by the lane exactly as the engine named it, posted only to a Live view and only when there is something to show",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return has(poll, "if (translator_->pollLevels(lv) && liveView_ && !lv.peaks.empty()) {")
      && has(poll, 'j += (first ? L"\\"" : L",\\"") + jsonEscape(utf8ToWide(p.first.c_str())) + L"\\":" + b;')
      && has(poll, 'std::wstring j = L"{\\"type\\":\\"liveLevels\\",\\"levels\\":{";')
      && !/MsgKind::Levels/.test(C.bridge);
  },
  [{ file: "bridge", find: "if (translator_->pollLevels(lv) && liveView_ && !lv.peaks.empty()) {", replace: "if (translator_->pollLevels(lv) && !lv.peaks.empty()) {", why: "the meters are posted with no Live view (a null view: a crash on the first LEVELS)" },
   { file: "bridge", find: "jsonEscape(utf8ToWide(p.first.c_str())) + L\"\\\":\" + b;", replace: "jsonEscape(utf8ToWide(p.first.substr(0, p.first.find('|')).c_str())) + L\"\\\":\" + b;", why: "the meter is keyed by the device again (a lane's channel cut off: two strips on one device share one meter)" }]);

// ================= L: lane ids (ADDENDUM R1/R2) ===========================================================================
pin("L1", "3.3 ADDENDUM R1", "translatorState.live.inputs[] names each strip by its LANE (liveLaneId(key, chan)) beside key / label / chan / gainDb / state / why; `on` is the engine's echo, `wanted` the app's wish",
  (S, C) => {
    const lj = fnBody(C.bridge, "std::wstring Bridge::liveJson");
    return has(lj, 'std::wstring(L"{\\"on\\":") + (trLive_ ? L"true" : L"false")') && has(lj, 'L",\\"wanted\\":" + (liveWanted_ ? L"true" : L"false")')
      && has(lj, 'jsonEscape(utf8ToWide(pbt::trpipe::liveLaneId(s.key, s.chan).c_str()))')
      && has(lj, 'std::wstring(L"{\\"key\\":\\"") + jsonEscape(utf8ToWide(s.key.c_str()))')
      && has(lj, 'L"\\",\\"chan\\":" + std::to_wstring(s.chan)') && has(lj, 'L",\\"state\\":\\"" + jsonEscape(utf8ToWide(s.state.c_str()))');
  },
  [{ file: "bridge", find: "pbt::trpipe::liveLaneId(s.key, s.chan).c_str()", replace: "s.key.c_str()", why: "the lane echoed to the page is the device key: two channel strips of one device cannot be told apart" },
   { file: "bridge", find: '(trLive_ ? L"true" : L"false")', replace: '(liveWanted_ ? L"true" : L"false")', why: "the on-air lamp shows the wish, not the engine's echo" },
   { file: "bridge", find: 'L",\\"wanted\\":" + (liveWanted_ ? L"true" : L"false")', replace: 'L",\\"wanted\\":false"', why: "\"starting to listen\" can never be said" }]);

pin("L2", "3.3 ADDENDUM R1", "a speaker's name comes from the strip whose LANE it is (never the first strip of the same device -- \"canal 1 = moi, canal 2 = l'invite\"), else the source's name (\"<name> . ch N\", BUILT from {key, c}, never parsed), else the bare name",
  (S, C) => {
    const f = fnBody(C.bridge, "std::wstring Bridge::stripLabelFor");
    return has(f, "if (pbt::trpipe::liveLaneId(s.key, s.chan) == id && !s.label.empty()) return w16(s.label);")
      && has(f, "if (src.first == id) return w16(src.second);")
      && has(f, "for (uint32_t c = 1; c <= pbt::trpipe::kLiveMaxChan; ++c)")
      && has(f, "if (pbt::trpipe::liveLaneId(src.first, c) == id) return w16(src.second) + L\" " + DOT + " ch \" + std::to_wstring(c);")
      && !/s\.key\s*==/.test(f) && !/find\(L'\|'\)|substr\(0,\s*lane\.find/.test(f);
  },
  [{ file: "bridge", find: "if (pbt::trpipe::liveLaneId(s.key, s.chan) == id && !s.label.empty()) return w16(s.label);", replace: "if (s.key == id && !s.label.empty()) return w16(s.label);", why: "the first strip with that key names every channel of the device" },
   { file: "bridge", find: "if (pbt::trpipe::liveLaneId(src.first, c) == id) return", replace: "if (id.compare(0, src.first.size(), src.first) == 0) return", why: "a channel lane is recognised by PARSING its id (a prefix compare)" },
   { file: "bridge", find: "for (uint32_t c = 1; c <= pbt::trpipe::kLiveMaxChan; ++c)", replace: "for (uint32_t c = 1; c <= 2; ++c)", why: "only channels 1-2 of a device get a name (a 4-channel interface's strips read as raw ids)" }]);

pin("L3", "3.3 ADDENDUM R1", "INPUT is keyed by the DEVICE: its state lights EVERY strip of that device (no break at the first), and a strip edited in liveConfig carries its device's lamp over",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return has(poll, "if (msg.kind == MsgKind::Input) { for (auto& s : liveStrips_) if (s.key == msg.input.key) { s.state = msg.input.state; s.why = msg.input.why; liveDirty = true; } continue; }")
      && !/break/.test(poll.slice(poll.indexOf("MsgKind::Input"), poll.indexOf("MsgKind::End")))
      && has(fnBody(C.bridge, "void Bridge::handleLiveConfig"), "for (const auto& old : liveStrips_) if (old.key == s.key) { s.state = old.state; s.why = old.why; }");
  },
  [{ file: "bridge", find: "s.why = msg.input.why; liveDirty = true; }", replace: "s.why = msg.input.why; liveDirty = true; break; }", why: "only the first strip of a device is lit/failed (the second channel strip lies)" },
   { file: "bridge", find: "for (const auto& old : liveStrips_) if (old.key == s.key) { s.state = old.state; s.why = old.why; }", replace: "(void)0;", why: "editing a trim blanks the strips' lamps until the next INPUT (which never comes)" }]);

pin("L4", "3.3 ADDENDUM R1 / 4.3", "a LINE's speaker key is built from the LANE as received -- {lane, voice cluster} or {lane, language} -- never from the label (renaming a strip changes the lines that FOLLOW, never a colour); spk.slot = the lane",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    const keys = [...poll.matchAll(/\bkey = [^;]*;/g)].map((m) => m[0]);
    return has(poll, "const std::wstring slot = utf8ToWide(l.spkSlot.c_str());") && has(poll, "std::wstring key, label = stripLabelFor(slot);")
      && has(poll, 'key = slot.empty() ? voice : (slot + L"#" + voice);') && has(poll, 'key = slot.empty() ? std::wstring() : (slot + L"|" + sLang);')
      && keys.length === 2 && keys.every((k) => !/label/.test(k))
      && has(poll, 'L"\\",\\"slot\\":\\"" + jsonEscape(slot)');
  },
  [{ file: "bridge", find: 'key = slot.empty() ? voice : (slot + L"#" + voice);', replace: 'key = slot.empty() ? voice : (label + L"#" + voice);', why: "the colour is held by the LABEL: renaming a strip recolours its speaker" },
   { file: "bridge", find: '(slot + L"|" + sLang)', replace: "(slot)", why: "a strip speaking two languages shares one colour/identity" },
   { file: "bridge", find: 'key = slot.empty() ? voice : (slot + L"#" + voice);', replace: "key = voice;", why: "the strip leaves the key: two strips' \"S1\" are one speaker (the voiceprint merges people in different places)" }]);

pin("L5", "3.3 ADDENDUM R1", "the lane rule has ONE definition (TrPipe.h liveLaneId: the key for a mix strip, key|chan for a one-channel strip, '|' being a character no key holds); the bridge never builds a lane by hand",
  (S, C) => has(fnBody(C.pipe, "inline std::string liveLaneId"), 'return chan ? key + "|" + std::to_string(chan) : key;')
    && count(C.bridge, /pbt::trpipe::liveLaneId\(/g) >= 3 && !/"\|"\s*\+\s*std::to_string\(|L"\|"\s*\+\s*std::to_wstring\(/.test(C.bridge),
  [{ file: "pipe", find: 'return chan ? key + "|" + std::to_string(chan) : key;', replace: 'return chan ? key + "#" + std::to_string(chan) : key;', why: "the lane's separator changes in the engine and the app but not on the page (every meter and speaker of a channel strip is lost)" },
   { file: "bridge", find: "pbt::trpipe::liveLaneId(s.key, s.chan).c_str()", replace: '(s.key + "|" + std::to_string(s.chan)).c_str()', why: "the bridge builds a lane by hand (a mix strip would be named \"key|0\")" }]);

// ================= C: liveConfig / Listen =================================================================================
pin("C1", "3.3 liveConfig", "liveConfig is checked WHOLE before anything moves: a bounded JSON (64 KiB), 0..4 strips, chan and gainDb well-typed, every key from the engine's last list or already a strip -- `wav:` (a PATH) only for a test instance -- then the engine's own rules (liveSetNormal)",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleLiveConfig");
    return has(b, "bool ok = json.size() <= (64u << 10) && pbjson::Reader(json).parse(v);")
      && has(b, "ok = list && list->isArr() && list->a.size() <= pbt::trpipe::kMaxLiveInputs;")
      && has(b, "if (!o.isObj() || !o.u64(L\"chan\", ch) || ch > 99 || !g || g->kind != pbjson::Value::Kind::Num) { ok = false; break; }")
      && has(b, "bool known = liveSrcKeys_.count(in.key) != 0;") && has(b, "for (const auto& s : liveStrips_) if (s.key == in.key) known = true;")
      && has(b, 'if (pbt::trpipe::liveKeyKind(in.key) == "wav") known = pblaunch::isTestInstance();')
      && has(b, "if (!known) { ok = false; unknownKey = true; break; }")
      && has(b, "if (ok && !pbapp::liveSetNormal(want, norm)) ok = false;");
  },
  [{ file: "bridge", find: 'known = pblaunch::isTestInstance();', replace: "known = true;", why: "a page can name a WAV path (a file of its choosing) outside a test instance" },
   { file: "bridge", find: "if (!known) { ok = false; unknownKey = true; break; }", replace: "if (false) { ok = false; unknownKey = true; break; }", why: "a key the engine never listed reaches the engine (no string of the page's own may)" },
   { file: "bridge", find: "bool known = liveSrcKeys_.count(in.key) != 0;", replace: "bool known = false;", why: "a source from the picker is refused as unknown" },
   { file: "bridge", find: "for (const auto& s : liveStrips_) if (s.key == in.key) known = true;", replace: "(void)0;", why: "a saved strip whose device is unplugged today cannot survive an edit of another strip" },
   { file: "bridge", find: "json.size() <= (64u << 10) && ", replace: "", why: "an unbounded liveConfig is parsed" },
   { file: "bridge", find: "if (ok && !pbapp::liveSetNormal(want, norm)) ok = false;", replace: "if (ok) norm = want;", why: "the engine's rules (5 strips, a 99 dB trim, the same key twice) are not applied: the engine refuses what the app persisted" },
   { file: "bridge", find: "list->a.size() <= pbt::trpipe::kMaxLiveInputs", replace: "list->a.size() <= 99", why: "no cap on the strips" }]);

pin("C2", "3.3 liveConfig", "a REFUSED liveConfig changes nothing: a `warn` (two sentences: unknown source / the rules), the previous strips re-sent as the page's echo, and a RETURN before anything is persisted or sent to the engine",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleLiveConfig");
    return has(b, "postLiveWarn(unknownKey ?") && has(b, "That source is not in the list any more: the change was not applied.")
      && has(b, "4 sources at most, a name of 64 bytes at most, a trim") && has(b, "lastLiveStateJson_.clear(); sendLiveState(true); return; }")
      && ordered(b, ["if (!ok) {", "postLiveWarn(", "return;", "std::vector<LiveStrip> next;", "liveStrips_ = next;", "saveLiveConfig();", "translator_->setLive(true, norm);", "sendTranslatorState(false);"]);
  },
  [{ file: "bridge", find: /sendLiveState\(\/\*force=\*\/true\);\s*\/\/ the previous strips, as the page's echo\n\s*return;/, replace: "sendLiveState(/*force=*/true);", why: "a refused list is applied anyway (no return)" },
   { file: "bridge", find: "        lastLiveStateJson_.clear();\n        sendLiveState(/*force=*/true);       // the previous strips", replace: "        sendLiveState(/*force=*/true);       // the previous strips", why: "the previous strips are not re-sent (a diffed state: nothing), the page keeps its refused edit" },
   { file: "bridge", find: "        postLiveWarn(unknownKey ?", replace: "        (void)unknownKey; (void)(unknownKey ?", why: "a refusal is silent" }]);

pin("C3", "3.3 liveConfig", "an ACCEPTED liveConfig is persisted (app.txt LIVEIN) and, while listening, sent as LIVE 1 with the engine-normalised set; removing the last strip while listening stops the listening (wish off, LIVE 0, the player's gate re-evaluated)",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleLiveConfig");
    return ordered(b, ["liveStrips_ = next;", "saveLiveConfig();", "if (liveWanted_ && translator_) {", "if (liveStrips_.empty()) {", "liveWanted_ = false;", "translator_->setLive(false, {});", "updateLiveGate();", "} else {", "translator_->setLive(true, norm);", "sendTranslatorState(false);"])
      && count(C.bridge, /\bsaveLiveConfig\(\);/g) === 2 && has(fnBody(C.bridge, "void Bridge::flushLiveConfig"), "liveCfgSaveDue_ = 0; saveLiveConfig();");
  },
  [{ file: "bridge", find: "else          { liveCfgSaveDue_ = 0; saveLiveConfig(); }", replace: "else          { liveCfgSaveDue_ = 0; }", why: "the strips are not persisted: they are gone at the next start" },
   { file: "bridge", find: "translator_->setLive(true, norm);", replace: "translator_->setLive(true, {});", why: "an edit while listening sends LIVE 1 with no input" },
   { file: "bridge", find: "            liveWanted_ = false;\n            translator_->setLive(false, {});", replace: "            translator_->setLive(false, {});", why: "the last strip removed: the wish stays on (a Listen button lit over nothing)" },
   { file: "bridge", find: "            translator_->setLive(false, {});\n            updateLiveGate();", replace: "            translator_->setLive(false, {});", why: "the player's gate stays closed after the last strip is removed" }]);

pin("C4", "3.3 liveListen / D-L8", "Listen: on with no strip = a hint (\"Add a source first.\") and nothing sent; on = LIVE 1 with the strips and the player's gate closed NOW (before the engine's echo); off = LIVE 0 and the lamps out; the wish starts false and is NEVER persisted (a microphone opens on a gesture, not at launch)",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleLiveListen");
    return has(b, 'if (on && liveStrips_.empty()) { postLiveWarn(L"Add a source first."); sendLiveState(true); return; }')
      && ordered(b, ["liveWanted_ = on;", "translator_->setLive(on, set);", "updateLiveGate();", "sendTranslatorState(false);"])
      && has(b, "for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }")
      && /bool\s+liveWanted_ = false;/.test(C.bridgeH) && !/liveWanted_/.test(fnBody(C.bridge, "void Bridge::saveLiveConfig"))
      && !/liveWanted_\s*=\s*true/.test(fnBody(C.bridge, "void Bridge::loadLiveConfig") + fnBody(C.bridge, "Bridge::Bridge"));
  },
  [{ file: "bridge", find: "if (on && liveStrips_.empty()) {", replace: "if (false) {", why: "Listen with no strip sends LIVE 1 with zero inputs (the engine refuses it, the button lies)" },
   { file: "bridge", find: "    translator_->setLive(on, set);\n    updateLiveGate();", replace: "    translator_->setLive(on, set);", why: "the player's gate opens/closes only on the engine's echo: a FILE races the LIVE 1" },
   { file: "bridgeH", find: "bool           liveWanted_ = false;", replace: "bool           liveWanted_ = true; ", why: "the app listens at launch (D-L8: a capture opens without a gesture)" },
   { file: "bridge", find: "        for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }\n    }\n    translator_->setLive(on, set);", replace: "    }\n    translator_->setLive(on, set);", why: "after Listen off the strips keep their lamps (\"open\" on a closed capture)" }]);

pin("C5", "3.3 / WE-14", "no PATH or free text comes from the Live page: everything the live section reads from its JSON is on a closed list (type, mode, preset, langA, langB, clear, alwaysOnTop, format, on, open, inputs, key, label, chan, gainDb)",
  (S, C) => {
    const live = C.bridge.slice(C.bridge.indexOf("void Bridge::startEngine"));
    const flat = [...live.matchAll(/jsonFind(?:String|Number|Bool)\(\s*json\s*,\s*L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const structured = [...live.matchAll(/\b(?:v|o|g)\.(?:str|u64|get|flag)\(L"([A-Za-z_]+)"/g)].map((m) => m[1]);
    const keys = [...new Set([...flat, ...structured])];
    const OK = ["type", "mode", "preset", "langA", "langB", "clear", "alwaysOnTop", "format", "on", "open", "inputs", "key", "label", "chan", "gainDb"];
    const unknown = keys.filter((k) => OK.indexOf(k) < 0);
    return [keys.length >= 10 && unknown.length === 0, { keys, unknown }];
  },
  [{ file: "bridge", find: 'jsonFindString(json, L"format", fmt);\n        exportOwner_ = liveHwnd_;', replace: 'jsonFindString(json, L"format", fmt); std::wstring p; jsonFindString(json, L"path", p);\n        exportOwner_ = liveHwnd_;', why: "the Live page can hand the host a path" },
   { file: "bridge", find: 'in.label = u8(o.str(L"label"));', replace: 'in.label = u8(o.str(L"label")); in.key = u8(o.str(L"file"));', why: "a strip's key read from a `file` field" }]);

// ================= B: the label rule, the colours, the escapes ============================================================
pin("B1", "4.3 label rule", "the label rule: a line is named after ITS strip; once a strip has produced lines from 2+ voice clusters its NEW lines carry \" . voice <n>\" (n = the voice's order of first appearance WITHIN that strip, from 1 -- the WORD since 26/10: \"Input 1/2 (...) . 2\" read as input 2); one voice = the strip's own label",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return has(poll, "std::vector<std::wstring>& voices = spkVoices_[slot];")
      && has(poll, "size_t n = 0; while (n < voices.size() && voices[n] != voice) ++n; if (n == voices.size()) voices.push_back(voice);")
      && has(poll, 'if (voices.size() >= 2) label = label.empty() ? clus : (label + L" ' + DOT + ' voice " + std::to_wstring(n + 1));')
      && has(poll, "else if (label.empty()) label = clus;");
  },
  [{ file: "bridge", find: "if (voices.size() >= 2) label", replace: "if (voices.size() >= 1) label", why: "every line is numbered, even a strip with one voice" },
   { file: "bridge", find: "std::to_wstring(n + 1));", replace: "std::to_wstring(n));", why: "the voices are numbered from 0" },
   { file: "bridge", find: 'L" ' + DOT + ' voice " + std::to_wstring(n + 1)', replace: 'L" ' + DOT + ' " + std::to_wstring(n + 1)', why: "the number without its word: \"Input 1/2 (3- Steinberg UR44C) . 2\" reads as INPUT 2 (26/10)" },
   { file: "bridge", find: "spkVoices_[slot];", replace: "spkVoices_[clus];", why: "voices are counted across ALL strips (a second strip's first voice is \" . voice 2\")" },
   { file: "bridge", find: "while (n < voices.size() && voices[n] != voice) ++n;", replace: "n = voices.size();", why: "a known voice is appended again at every line (its number climbs)" },
   { file: "bridge", find: "else if (label.empty()) label = clus;", replace: "", why: "a strip with no label and one voice shows an empty speaker" }]);

pin("B2", "4.3 colours", "colours: kSpeakerColors = the page's palette (--spk-1..N of live.html), assigned in ORDER OF APPEARANCE, never reassigned, and reset with the session (a Clear: the scrollback, the journal, the colours and each strip's voices)",
  (S, C) => {
    const palette = new Set([...(S.live || "").matchAll(/--spk-(\d+)\s*:/g)].map((m) => m[1])).size;
    return [has(C.bridgeH, "static constexpr int kSpeakerColors = 6;") && palette === 6
      && has(fnBody(C.bridge, "int Bridge::translatorSpeakerColor"), "auto it = spkColor_.find(key); if (it != spkColor_.end()) return it->second; const int c = spkNextColor_ % kSpeakerColors; ++spkNextColor_; spkColor_[key] = c; return c;")
      && has(fnBody(C.bridge, "void Bridge::endLiveSession"), "translatorLog_.clear(); trSessionPath_.clear(); trSessionLines_ = 0; spkColor_.clear(); spkNextColor_ = 0; spkVoices_.clear();"),
      { palette }];
  },
  [{ file: "bridgeH", find: "static constexpr int kSpeakerColors = 6;", replace: "static constexpr int kSpeakerColors = 8;", why: "colours 7-8 alias 1-2 on the page (1.1.0's mismatch)" },
   { file: "bridge", find: "if (it != spkColor_.end()) return it->second;", replace: "", why: "a speaker's colour is re-drawn at every line" },
   { file: "bridge", find: "    spkColor_.clear();\n    spkNextColor_ = 0;\n    spkVoices_.clear();", replace: "    spkColor_.clear();\n    spkNextColor_ = 0;", why: "a Clear keeps the strips' voices: the new meeting starts at \" . voice 3\"" },
   { file: "bridge", find: "    trSessionPath_.clear();\n    trSessionLines_ = 0;\n    spkColor_.clear();", replace: "    trSessionLines_ = 0;\n    spkColor_.clear();", why: "a Clear keeps appending to the old journal" }]);

pin("B3", "build flags", "the code of every file the live mode touched is ASCII (the build passes no /utf-8): the middle dot, the em dash and the double arrow are backslash-u ESCAPES (a typed character would be mojibake)",
  (S, C) => {
    const names = ["bridge", "bridgeH", "main", "player", "mgr", "mgrH", "launch", "config", "import"];
    const bad = names.filter((k) => /[^\x00-\x7F]/.test(C[k] || ""));
    const dots = count(S.bridge, new RegExp(BS + BS + "u00B7", "g")), dashes = count(S.bridge, new RegExp(BS + BS + "u2014", "g")), arrows = count(S.bridge, new RegExp(BS + BS + "u2194", "g"));
    return [bad.length === 0 && dots >= 2 && dashes >= 1 && arrows >= 1, { bad, dots, dashes, arrows }];
  },
  [{ file: "bridge", find: 'label + L" ' + DOT + ' voice " + std::to_wstring(n + 1)', replace: 'label + L" \xB7 voice " + std::to_wstring(n + 1)', why: "the label's middle dot typed as a character (mojibake without /utf-8)" },
   { file: "bridge", find: 'info.title = L"SuVtitle Live ' + DASH + ' "', replace: 'info.title = L"SuVtitle Live ' + String.fromCharCode(0x2014) + ' "', why: "the document title's em dash typed as a character" },
   { file: "main", find: 'constexpr wchar_t kLiveWindowTitle[] = L"SuVtitle Live";', replace: 'constexpr wchar_t kLiveWindowTitle[] = L"SuVtitle Live \xB7";', why: "a non-ASCII character in a window title literal" }]);

// ================= E: the export and the journal ==========================================================================
pin("E1", "4.3 export", "the Word export: document title \"SuVtitle Live <em dash> <date>\", a subtitle with the start time / line count / the live pair, the Save dialog's name \"SuVtitle Live YYYY-MM-DD HH-MM.docx\" in Documents, .docx the default and the only type",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleTranslatorExport");
    return has(b, 'info.title = L"SuVtitle Live ' + DASH + ' " + stampOf(t0, L"%04u-%02u-%02u", true);')
      && has(b, 'info.subtitle = L"Session started " + stampOf(t0, L"%02u:%02u", false) + L", " + std::to_wstring((long long)lines.size()) + L" lines, " + pbdocx::languageName(liveLangA_) + L" ' + ARROW + ' " + pbdocx::languageName(liveLangB_);')
      && has(b, 'L"SuVtitle Live %04u-%02u-%02u %02u-%02u.docx"') && has(b, 'dlg->SetDefaultExtension(L"docx");')
      && has(b, 'const COMDLG_FILTERSPEC filter[] = { { L"Word document (*.docx)", L"*.docx" } };')
      && has(b, "const std::wstring docs = trDocumentsDir();") && has(b, 'dlg->SetTitle(L"Save the translated session");');
  },
  [{ file: "bridge", find: 'info.title = L"SuVtitle Live', replace: 'info.title = L"AudioPB Live', why: "the document is titled with the old product's name" },
   { file: "bridge", find: 'L"SuVtitle Live %04u-%02u-%02u %02u-%02u.docx"', replace: 'L"Translation %04u-%02u-%02u %02u-%02u.docx"', why: "the suggested file name is not the contract's" },
   { file: "bridge", find: 'dlg->SetDefaultExtension(L"docx");     // a name typed without one', replace: 'dlg->SetDefaultExtension(L"txt");     // a name typed without one', why: "a name typed without an extension lands as .txt" },
   { file: "bridge", find: 'pbdocx::languageName(liveLangA_) + L" ' + ARROW + ' " + pbdocx::languageName(liveLangB_)', replace: 'pbdocx::languageName(trLangA_) + L" ' + ARROW + ' " + pbdocx::languageName(trLangB_)', why: "the document names the PLAYER's pair, not the live one" },
   { file: "bridge", find: "const std::wstring docs = trDocumentsDir();", replace: "const std::wstring docs;", why: "the dialog opens in whatever folder Windows remembers" }]);

pin("E2", "4.3 export / AV window", "the document is written TEMP + RENAME with the antivirus retry (pbtrf::writeFileAvSafe: <target>.tmp, MoveFileEx, 5 x 20 ms, then in place) -- never a plain CreateFile on the user's target; a failed write is said; the replace is the user's (he confirmed the overwrite)",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleTranslatorExport"), w = fnBody(C.store, "inline bool writeFileAvSafe");
    return has(b, "if (!pbtrf::writeFileAvSafe(target, bytes, true)) {") && has(b, 'postTranslatorExported(false, L"the document could not be saved there", 0);')
      && !/CreateFileW\(\s*target/.test(b)
      && has(w, 'const std::wstring tmp = path + L".tmp";') && has(w, "for (int t = 0; t < 5; ++t) {") && has(w, "::Sleep(20);")
      && has(w, "::MoveFileExW(tmp.c_str(), path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)") && has(w, "::DeleteFileW(tmp.c_str());");
  },
  [{ file: "bridge", find: "if (!pbtrf::writeFileAvSafe(target, bytes, true)) {", replace: "if (!pbtrf::writeFileAvSafe(target, bytes, false)) {", why: "an overwrite the user confirmed in the dialog is refused" },
   { file: "bridge", find: "if (!pbtrf::writeFileAvSafe(target, bytes, true)) {", replace: "HANDLE hh = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (hh == INVALID_HANDLE_VALUE) {", why: "the document is written straight into the target (a half-written .docx on a crash)" },
   { file: "store", find: "for (int t = 0; t < 5; ++t) {", replace: "for (int t = 0; t < 1; ++t) {", why: "the rename is tried once: Bitdefender's scan window loses the file" },
   { file: "store", find: "        ::Sleep(20);\n    }\n    ::DeleteFileW(tmp.c_str());", replace: "    }\n    ::DeleteFileW(tmp.c_str());", why: "the retries come back to back (inside the same scan window)" }]);

pin("E3", "3.3 translatorExport", "the Export dialog belongs to the window that asked (the Live window, even always-on-top), one at a time, only .docx (any other format REFUSED by name), \"nothing to export yet\" with no session, Cancel = ack err \"cancelled\" (the page's silence), and nothing is written before the user names a place",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleTranslatorExport"), h = fnBody(C.bridge, "void Bridge::handleLiveMessage");
    return has(h, "exportOwner_ = liveHwnd_; handleTranslatorExport(fmt); exportOwner_ = nullptr;")
      && has(b, "const HWND owner = (exportOwner_ && ::IsWindow(exportOwner_)) ? exportOwner_ : dialogOwner();")
      && has(b, 'if (trDialogBusy_) { postTranslatorExported(false, L"a file dialog is open: finish it first, then export again", 0); return; }') && has(b, 'if (!format.empty() && format != L"docx") {')
      && has(b, 'postTranslatorExported(false, L"only Word documents (.docx) can be exported for now", 0);')
      && has(b, 'if (target.empty()) { postTranslatorExported(false, L"cancelled", 0); return; }') && has(b, 'postTranslatorExported(false, L"nothing to export yet", 0);')
      && has(b, "dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);")
      && has(b, "FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE")
      && ordered(b, ["if (trSessionPath_.empty())", "::CreateFileW(trSessionPath_.c_str()", "pbdocx::buildTranscript(", "IFileSaveDialog* dlg", "dlg->Show(owner)", "pbtrf::writeFileAvSafe(target"]);
  },
  [{ file: "bridge", find: "const HWND owner = (exportOwner_ && ::IsWindow(exportOwner_)) ? exportOwner_ : dialogOwner();", replace: "const HWND owner = dialogOwner();", why: "the Save dialog opens BEHIND the always-on-top Live window" },
   { file: "bridge", find: 'postTranslatorExported(false, L"a file dialog is open: finish it first, then export again", 0);\n        return;', replace: "return;", why: "an Export while a player dialog is open is lost in silence (RVB-15)" },
   { file: "bridge", find: "        exportOwner_ = liveHwnd_;", replace: "", why: "the Live window's export has no owner" },
   { file: "bridge", find: '        postTranslatorExported(false, L"cancelled", 0);\n        return;', replace: 'postTranslatorExported(false, L"the dialog was closed", 0);', why: "Cancel is shown to the user as an error" },
   { file: "bridge", find: 'if (!format.empty() && format != L"docx") {', replace: "if (false) {", why: "a page asking for \"xlsx\" receives a Word file labelled as one" },
   { file: "bridge", find: "FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);\n    dlg->SetTitle(L\"Save the translated session\");", replace: "FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);\n    dlg->SetTitle(L\"Save the translated session\");", why: "an existing file is overwritten without asking" },
   { file: "bridge", find: "FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,\n                             OPEN_EXISTING", replace: "FILE_SHARE_READ, nullptr,\n                             OPEN_EXISTING", why: "the export locks the journal the engine is still appending to" }]);

pin("E4", "3.4 journal", "the journal: %APPDATA%\\SuVtitle\\Live sessions\\<date>.jsonl, created at the FIRST final line (an idle app leaves no file), appended with FILE_APPEND_DATA (no rename), counted only when the bytes landed; PART and END never reach it or the scrollback (200 lines)",
  (S, C) => {
    const j = fnBody(C.bridge, "void Bridge::appendTranslatorSession"), poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    const wj = fnBody(C.bridge, "bool Bridge::writeJournalPending");
    return has(j, 'const std::wstring dir = appDataPath(L"Live sessions");') && has(j, "if (trSessionPath_.empty()) {")
      && has(j, "trJournalOwed_ += wideToUtf8(lineJson)") && has(j, "writeJournalPending();")
      && has(wj, "FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS") && has(wj, "trSessionLines_ += static_cast<long long>(nl - seps);")
      && has(wj, "trJournalOwed_.erase(0, wr);") && !/MoveFile|rename/i.test(j) && !/MoveFile|rename/i.test(wj) && count(C.bridge, /appendTranslatorSession\(/g) === 2 && count(C.bridge, /translatorLog_\.push_back\(/g) === 1
      && has(poll, "while (translatorLog_.size() >= kTranslatorLogMax) translatorLog_.pop_front(); translatorLog_.push_back(json); appendTranslatorSession(json);")
      && has(C.bridgeH, "static constexpr size_t kTranslatorLogMax = 200;");
  },
  [{ file: "bridge", find: 'const std::wstring dir = appDataPath(L"Live sessions");\n        if (dir.empty()) { journalFailed', replace: 'const std::wstring dir = appDataPath(L"Sessions");\n        if (dir.empty()) { journalFailed', why: "the journal lands in a folder the contract does not name" },
   { file: "bridge", find: "::CreateFileW(trSessionPath_.c_str(), FILE_APPEND_DATA,", replace: "::CreateFileW(trSessionPath_.c_str(), GENERIC_WRITE,", why: "the journal is opened for plain write (it would be truncated/seeked, not appended)" },
   { file: "bridge", find: '            pj += L"}";\n            postToLive(pj);', replace: '            pj += L"}";\n            appendTranslatorSession(pj);\n            postToLive(pj);', why: "drafts are journalled (the export would contain half-sentences)" },
   { file: "bridgeH", find: "kTranslatorLogMax = 200;", replace: "kTranslatorLogMax = 20000;", why: "the replay grows without a bound" },
   { file: "bridge", find: "    trSessionLines_ += static_cast<long long>(nl - seps);", replace: "    trSessionLines_ += 1;", why: "the export's tooltip counts lines that never reached the file" },
   { file: "bridge", find: "trJournalOwed_.erase(0, wr);", replace: "trJournalOwed_.clear();", why: "a write that failed DROPS its lines (RVB-12: they are owed, written with the next line)" }]);

// ================= P: LIVEPOS / LIVEIN persistence (3.4) =================================================================
pin("P1", "3.4 LIVEIN", "the strips are persisted in app.txt (LIVEIN, fields escaped) only when they CHANGE, through the antivirus-safe write, and never from an app.txt that exists but could not be read (an empty read is not an empty file); read back at construction by the engine's own rules",
  (S, C) => {
    const sv = fnBody(C.bridge, "void Bridge::saveLiveConfig"), ld = fnBody(C.bridge, "void Bridge::loadLiveConfig");
    return has(sv, "if (!pbtrf::readFileAll(p, old, 1ULL << 20) && ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;")
      && has(sv, "const std::string body = pbapp::packLiveInputs(old, set); if (body != old) pbtrf::writeFileAvSafe(p, body);")
      && has(ld, "for (const auto& in : pbapp::parseLiveInputs(text)) {") && has(fnBody(C.bridge, "Bridge::Bridge"), "loadLiveConfig();")
      && has(fnBody(C.config, "inline std::string packLiveInputs"), 'with.push_back("LIVEIN\\t" + pbt::trpipe::esc(in.key) + "\\t" + pbt::trpipe::esc(in.label) + "\\t"')
      && has(fnBody(C.config, "inline std::string packLiveInputs"), 'return replaceTagged(old, "LIVEIN", with);');
  },
  [{ file: "bridge", find: "if (!pbtrf::readFileAll(p, old, 1ULL << 20) && ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;", replace: "pbtrf::readFileAll(p, old, 1ULL << 20);", why: "an unreadable app.txt is rebuilt from nothing (the Live position and the user's WINPOS lost)" },
   { file: "bridge", find: "if (body != old) pbtrf::writeFileAvSafe(p, body);", replace: "pbtrf::writeFileAvSafe(p, body);", why: "app.txt is rewritten at every edit (more AV windows, a lost write)" },
   { file: "config", find: "pbt::trpipe::esc(in.label)", replace: "in.label", why: "a label holding a tab splits its LIVEIN line (the strip is skipped at the next start)" },
   { file: "config", find: 'return replaceTagged(old, "LIVEIN", with);', replace: 'return replaceTagged(old, "LIVEPOS", with);', why: "the strips overwrite the Live window's position line" }]);

pin("P2", "3.4 LIVEPOS", "LIVEPOS is saved where the user LETS GO (WM_EXITSIZEMOVE), on maximise/restore, on close, on a pin change and at the end -- only for a VISIBLE window and only from a readable app.txt, through packLivePos + the antivirus-safe write when it changed",
  (S, C) => {
    const sv = fnBody(C.main, "void saveLivePos"), lw = fnBody(C.main, "LRESULT CALLBACK LiveWndProc"), wp = fnBody(C.main, "LRESULT CALLBACK WndProc");
    return has(sv, "if (!g_liveHwnd || !::IsWindowVisible(g_liveHwnd)) return;") && has(sv, "p.topmost = g_liveTopmost;")
      && has(sv, "if (!pbtrf::readFileAll(f, old, 1ULL << 20) && ::GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return;")
      && has(sv, "const std::string body = pbapp::packLivePos(old, p); if (body != old) pbtrf::writeFileAvSafe(f, body);")
      && has(lw, "case WM_EXITSIZEMOVE: saveLivePos(); return 0;") && has(lw, "if (wParam != lastLive) { lastLive = wParam; saveLivePos(); }")
      && has(fnBody(C.main, "void closeLiveWindow"), "saveLivePos(); if (g_bridge) g_bridge->liveWindowClosed();")
      && has(fnBody(C.main, "void setLiveTopmost"), "saveLivePos();") && has(wp, "if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }");
  },
  [{ file: "main", find: /case WM_EXITSIZEMOVE:\s*saveLivePos\(\);[^\n]*\n/, replace: "case WM_EXITSIZEMOVE:\n", why: "a drag or a resize of the Live window is not remembered" },
   { file: "main", find: "if (!g_liveHwnd || !::IsWindowVisible(g_liveHwnd)) return;", replace: "if (!g_liveHwnd) return;", why: "a hidden Live window saves a minimised/zero placement over the good one" },
   { file: "main", find: "if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }", replace: "if (g_liveHwnd) { ::DestroyWindow(g_liveHwnd); }", why: "the Live window's last position is not saved at the end" },
   { file: "main", find: "    p.topmost = g_liveTopmost;", replace: "    p.topmost = false;", why: "the Live window's pin is forgotten at the next start" }]);

pin("P3", "3.4 LIVEPOS / 4.3", "the Live window opens at LIVEPOS; else, ONCE, where AudioPB's Translator window was (read-only seed); else 760 x 560 at the system DPI -- a rectangle whose title bar is on no monitor is brought back onto the nearest one -- and LIVEPOS exists from the first show (the seed never runs again)",
  (S, C) => {
    const o = fnBody(C.main, "void openLiveWindow");
    return has(o, "bool have = !f.empty() && pbtrf::readFileAll(f, text, 1ULL << 20) && pbapp::parseLivePos(text, pos);")
      && has(o, "if (!have) have = pbimport::seedLivePos(pbimport::productRoots(), pos);")
      && has(o, "if (!::MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {")
      && has(o, "pos = pbapp::clampWinPos(pos, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);")
      && has(o, "have ? pos.w : (int)(760 * dpi / 96), have ? pos.h : (int)(560 * dpi / 96)")
      && ordered(o, ["pbimport::seedLivePos(", "clampWinPos(", "::CreateWindowExW(", "g_liveTopmost = realTopmost(g_liveHwnd);", "::ShowWindow(g_liveHwnd", "saveLivePos();", "createLiveView(true);"]);
  },
  [{ file: "main", find: "if (!have) have = pbimport::seedLivePos(", replace: "if (true) have = pbimport::seedLivePos(", why: "AudioPB's old position overrides the one the user set in SuVtitle, at every start" },
   { file: "main", find: "MonitorFromRect(&title, MONITOR_DEFAULTTONULL)", replace: "MonitorFromRect(&title, MONITOR_DEFAULTTONEAREST)", why: "a window left on an unplugged screen is never brought back (the user cannot grab it)" },
   { file: "main", find: /saveLivePos\(\);\s*\/\/ LIVEPOS exists from now on[^\n]*\n/, replace: "\n", why: "no LIVEPOS after the first show: the seed runs at every start" },
   { file: "main", find: "have ? pos.w : (int)(760 * dpi / 96)", replace: "have ? pos.w : (int)(760)", why: "the default size ignores the system DPI (a postage stamp on a 200 % screen)" }]);

pin("P4", "3.4 / 4.3 seed", "the seed from AudioPB's translator.txt is READ ONLY and bounded (1 MB): seedLivePos writes, copies, moves and deletes nothing",
  (S, C) => {
    const sd = fnBody(C.import, "inline bool seedLivePos");
    return has(sd, 'pbtrf::readFileAll(pbtrf::joinPath(r.fromRoaming, L"translator.txt"), text, 1ULL << 20)') && has(sd, "pbapp::parseWinPos(text, p)")
      && !/write|copy|move|delete|create|remove|rename/i.test(sd.replace(/readFileAll/g, ""));
  },
  [{ file: "import", find: "    if (r.fromRoaming.empty()) return false;\n    std::string text;\n    return pbtrf::readFileAll(pbtrf::joinPath(r.fromRoaming, L\"translator.txt\")", replace: "    if (r.fromRoaming.empty()) return false;\n    std::string text;\n    pbtrf::writeFileAvSafe(pbtrf::joinPath(r.fromRoaming, L\"translator.txt\"), text);\n    return pbtrf::readFileAll(pbtrf::joinPath(r.fromRoaming, L\"translator.txt\")", why: "the seed rewrites AudioPB's own file" },
   { file: "import", find: "text, 1ULL << 20)\n        && pbapp::parseWinPos(text, p);", replace: "text, 1ULL << 40)\n        && pbapp::parseWinPos(text, p);", why: "the seed reads a file of any size" }]);

// ================= S: the CSP of each page, the Live view ================================================================
pin("S1", "4.3 CSP per page", "a CSP PER PAGE: live.html is served under kLiveCsp = the player's policy WITHOUT media-src (no film, no sound copy, no media host), default-src 'none', no network, no eval -- the player's page and the 404 keep kWebCsp unchanged",
  (S, C) => {
    const web = cstr(C.main, "kWebCsp"), live = cstr(C.main, "kLiveCsp");
    return [web != null && live != null
      && live === web.replace(/media-src [^;]*; /, "") && /media-src/.test(web) && !/media-src|connect-src|frame-src|worker-src|unsafe-eval|https?:|data:[^;]*;[^;]*data:/.test(live)
      && /^default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; /.test(live)
      && has(C.main, 'if (livePage) headers += std::wstring(L"Content-Security-Policy: ") + kLiveCsp;')
      && has(C.main, 'else headers = headers + L"Content-Security-Policy: " + kWebCsp;')
      && has(C.main, "const bool livePage = ::_wcsicmp(pbapp::webRelPath(uri.substr(host.size())).c_str(), kLivePage) == 0;")
      && has(C.main, 'respondWeb(args, 200, L"OK", buf, webMimeFor(file), livePage);')
      && /respondWeb\(args, 404, L"Not Found", std::vector<BYTE>\(m, m \+ sizeof\(kMissing\) - 1\), L"text\/plain"\);/.test(C.main),
      { web, live }];
  },
  [{ file: "main", find: '    L"base-uri \'none\'; form-action \'none\'; frame-ancestors \'none\'; object-src \'none\'";', replace: '    L"media-src https://*.suvtitle-media.example; base-uri \'none\'; form-action \'none\'; frame-ancestors \'none\'; object-src \'none\'";', why: "the Live page is allowed to load media" },
   { file: "main", find: "constexpr wchar_t kLiveCsp[] =\n    L\"default-src 'none';", replace: "constexpr wchar_t kLiveCsp[] =\n    L\"default-src 'self';", why: "the Live page may load anything from its own origin by default" },
   { file: "main", find: "if (livePage) headers +=", replace: "if (false) headers +=", why: "live.html is served under the PLAYER's policy (media-src open)" },
   { file: "main", find: "kLivePage) == 0;", replace: "kLivePage) != 0;", why: "the policies are swapped: the player's page under the Live policy, live.html under the player's" },
   { file: "main", find: 'respondWeb(args, 404, L"Not Found", std::vector<BYTE>(m, m + sizeof(kMissing) - 1), L"text/plain");', replace: 'respondWeb(args, 404, L"Not Found", std::vector<BYTE>(m, m + sizeof(kMissing) - 1), L"text/plain", true);', why: "a miss is answered under the Live policy" }]);

pin("S2", "4.3 CSP / page", "...and the Live page needs nothing its policy forbids: live.html uses no media element, no network API, no external URL, no <base> / <form> / <iframe> / <object> / <embed> / <link>",
  (S, C) => {
    const t = (S.live || "").replace(/<!--[\s\S]*?-->/g, "").replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:\\])\/\/[^\n]*/g, "$1");
    return [t.length > 50000 && !/<(?:video|audio|iframe|object|embed|link|base|form|source|track)\b/i.test(t) && !/\bnew\s+Audio\s*\(/.test(t)
      && !/\b(?:fetch|XMLHttpRequest|WebSocket|EventSource|sendBeacon|importScripts)\b/.test(t) && !/\b(?:src|href|action)\s*=\s*["']?https?:/i.test(t),
      { len: t.length }];
  },
  [{ file: "live", find: "<body", replace: "<body><video src=\"x.mp4\"></video", why: "a <video> in the Live page (media-src is not allowed)" },
   { file: "live", find: "<body", replace: "<body><iframe src=\"https://example.com\"></iframe", why: "an iframe in the Live page" }]);

pin("S3", "4.3 the Live view", "the Live view: a second controller on the SAME environment with the player's lock-down (hardenView), the fresh no-store serving (its own WebResourceRequested handler), NO external drops, ProcessFailed watched, the bridge joined BEFORE the first navigation, then live.html",
  (S, C) => {
    const cv = fnBody(C.main, "void createLiveView");
    return has(cv, "g_environment->CreateCoreWebView2Controller(") && has(cv, "c4->put_AllowExternalDrop(FALSE);")
      && has(cv, 'g_liveWebview->AddWebResourceRequestedFilter((std::wstring(kWebHost) + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);')
      && has(cv, "serveFreshWebAsset(a);") && has(cv, "hardenView(g_liveWebview.Get(), &g_liveAllowStringNav);")
      && has(cv, "watchProcessFailed(g_liveWebview.Get(), true);") && has(cv, "if (g_bridge) g_bridge->attachLiveView(g_liveWebview.Get(), hwnd);")
      && has(cv, "g_liveWebview->Navigate((std::wstring(kWebHost) + kLivePage).c_str());")
      && ordered(cv, ["hardenView(", "attachLiveView(", "->Navigate("]) && has(C.main, 'constexpr wchar_t kLivePage[] = L"live.html";');
  },
  [{ file: "main", find: "c4->put_AllowExternalDrop(FALSE);", replace: "c4->put_AllowExternalDrop(TRUE);", why: "a file dropped on the Live window starts a file:// navigation (cancelled only by the guard)" },
   { file: "main", find: "hardenView(g_liveWebview.Get(), &g_liveAllowStringNav);", replace: "(void)g_liveAllowStringNav;", why: "the Live view keeps its context menu, devtools and navigation freedom" },
   { file: "main", find: "                            serveFreshWebAsset(a);\n                            return S_OK;\n                        }).Get(),\n                    &wrToken);\n                // the same lock-down", replace: "                            (void)a;\n                            return S_OK;\n                        }).Get(),\n                    &wrToken);\n                // the same lock-down", why: "the Live page is not served by us (a stale cached copy, or a network lookup of suvtitle.local)" },
   { file: "main", find: "(std::wstring(kWebHost) + kLivePage).c_str());\n                }", replace: "(std::wstring(kWebHost) + L\"index.html\").c_str());\n                }", why: "the Live window shows the player's page" },
   { file: "main", find: "                if (g_bridge) g_bridge->attachLiveView(g_liveWebview.Get(), hwnd);\n", replace: "", why: "the Live view's messages are never handled" }]);

// ================= F: ProcessFailed =======================================================================================
pin("F1", "4.3 ProcessFailed", "ProcessFailed (BOTH views): a dead or hung RENDERER is reloaded -- at most 3 per view per minute (a page that kills its renderer is not reloaded in a loop); a dead BROWSER process posts ONE rebuild; the other kinds (GPU, utility: Chromium restarts them itself) are left alone",
  (S, C) => {
    const w = fnBody(C.main, "void watchProcessFailed"), rv = fnBody(C.main, "void reloadView");
    return has(w, "if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) { if (g_hwnd) ::PostMessageW(g_hwnd, kWmRebuildViews, egen, 0); return S_OK; }")
      && has(w, "if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) return S_OK;")
      && has(w, "reloadView(live ? 1 : 0, sender);") && has(rv, "ULONGLONG* r = g_reloads[v ? 1 : 0];")
      && has(rv, "if (r[0] != 0 && now - r[0] < 60000) {") && has(rv, "r[0] = r[1]; r[1] = r[2]; r[2] = now;") && has(rv, "if (wv) wv->Reload();")
      && has(rv, "::SetTimer(g_hwnd, v ? kReloadTimer1 : kReloadTimer0, static_cast<UINT>(60000 - (now - r[0]) + 500), nullptr);")
      && has(fnBody(C.main, "LRESULT CALLBACK WndProc"), "reloadView(v, v ? g_liveWebview.Get() : g_webview.Get());")
      && has(fnBody(C.main, "void openLiveWindow"), "if (g_reloadOwed[1] && g_liveWebview) {");
  },
  [{ file: "main", find: "now - r[0] < 60000) {", replace: "now - r[0] < 0) {", why: "a page that kills its renderer every time is reloaded in a loop" },
   { file: "main", find: "ULONGLONG* r = g_reloads[v ? 1 : 0];", replace: "ULONGLONG* r = g_reloads[0];", why: "the two views share ONE reload budget (the player's crashes starve the Live window's)" },
   { file: "main", find: "            ::SetTimer(g_hwnd, v ? kReloadTimer1 : kReloadTimer0,", replace: "            (void)(g_hwnd, v ? kReloadTimer1 : kReloadTimer0,", why: "a fourth renderer death in a minute leaves the view on the crash page for good (RVB-4)" },
   { file: "main", find: "if (g_hwnd) ::PostMessageW(g_hwnd, kWmRebuildViews, egen, 0);", replace: "if (sender) sender->Reload();", why: "a dead BROWSER process is answered by a reload of a view that no longer exists" },
   { file: "main", find: /\s*&& kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE\)/, replace: ")", why: "a HUNG renderer (a frozen Live window) is never reloaded" }]);

pin("F2", "4.3 ProcessFailed", "...both views are watched: the player's (live=false) in createWebViewEnv, the Live window's (live=true) in createLiveView",
  (S, C) => has(fnBody(C.main, "void createWebViewEnv"), "watchProcessFailed(g_webview.Get(), false);")
    && has(fnBody(C.main, "void createLiveView"), "watchProcessFailed(g_liveWebview.Get(), true);") && count(C.main, /watchProcessFailed\(/g) === 4,   // 2 calls + the declaration + the definition
  [{ file: "main", find: "watchProcessFailed(g_webview.Get(), /*live=*/false);", replace: "", why: "the player's renderer can die for good (a blank window)" },
   { file: "main", find: "watchProcessFailed(g_liveWebview.Get(), /*live=*/true);", replace: "", why: "the Live window freezes for good on a renderer crash, in the middle of a meeting" }]);

pin("F3", "4.3 ProcessFailed", "rebuildViews (the browser process died): at most 3 per 5 minutes, off the event (posted), the bridge lets go of BOTH views BEFORE their controllers are closed, the Live window's view is asked for again if it was up, then the environment is made again",
  (S, C) => {
    const r = fnBody(C.main, "void rebuildViews"), env = fnBody(C.main, "void createWebViewEnv");
    return has(r, "if (g_environment || g_webview) {") && has(r, "} else if (g_rebuilding || !g_viewsLost) { return; }")
      && has(r, "if (g_rebuilds[0] != 0 && now - g_rebuilds[0] < 300000) {")
      && has(r, "::SetTimer(g_hwnd, kRebuildTimer, static_cast<UINT>(300000 - (now - g_rebuilds[0]) + 1000), nullptr);")
      && ordered(r, ["if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) g_liveViewOwed = true;", "g_bridge->detachLiveView(); g_bridge->detachView();", "g_liveController->Close();", "++g_liveGen;", "g_controller->Close();", "g_environment.Reset();", "g_viewsLost = true;", "g_rebuilding = true;", "createWebView(g_hwnd);"])
      && !/openLiveWindow|g_liveRequested|SetForegroundWindow|SW_RESTORE/.test(r)
      && has(env, "if (g_liveViewOwed) { g_liveViewOwed = false; g_liveViewTries = 0; createLiveView(false); }")
      && has(fnBody(C.main, "LRESULT CALLBACK WndProc"), "case kWmRebuildViews: rebuildViews(static_cast<unsigned>(wParam)); return 0;")
      && has(fnBody(C.bridge, "void Bridge::attach"), "if (webview_ || !webview) return;");
  },
  [{ file: "main", find: "now - g_rebuilds[0] < 300000) {", replace: "now - g_rebuilds[0] < 0) {", why: "a browser process that dies at once is rebuilt in an endless loop" },
   { file: "main", find: "        if (g_bridge) { g_bridge->detachLiveView(); g_bridge->detachView(); }\n        if (g_liveController) { g_liveController->Close(); g_liveController.Reset(); }", replace: "        if (g_liveController) { g_liveController->Close(); g_liveController.Reset(); }\n        if (g_bridge) { g_bridge->detachLiveView(); g_bridge->detachView(); }", why: "the bridge keeps a handler on a view that is already closed (a call on a dead object)" },
   { file: "main", find: "        if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) g_liveViewOwed = true;\n", replace: "", why: "the Live window comes back empty (no view) after a browser crash" },
   { file: "main", find: "if (g_liveViewOwed) { g_liveViewOwed = false; g_liveViewTries = 0; createLiveView(/*user=*/false); }", replace: "if (g_liveViewOwed) { g_liveViewOwed = false; g_liveViewTries = 0; openLiveWindow(); }", why: "the recovery restores and ACTIVATES the minimized, pinned Live window over the user's game (RVB-3)" },
   { file: "main", find: "        ::SetTimer(g_hwnd, kRebuildTimer, static_cast<UINT>(300000 - (now - g_rebuilds[0]) + 1000), nullptr);\n", replace: "", why: "a rebuild the limiter refused is dropped: the views dead for good (RVB-4)" }]);

// ================= V: --live ==============================================================================================
pin("V1", "4.3 --live (TrLaunch.h)", "--live is parsed; its WM_COPYDATA tag kCopyLive (\"SVTL\") is its own, distinct from --play's, and a live copy carries NOTHING (an empty payload only: anything else is refused)",
  (S, C) => has(C.launch, '} else if (x == L"--live") { a.live = true;') && has(C.launch, "constexpr ULONG_PTR kCopyLive = 0x4C545653;") && has(C.launch, "constexpr ULONG_PTR kCopyPlay = 0x50545653;")
    && has(fnBody(C.launch, "inline bool isLiveCopy"), "return tag == kCopyLive && bytes == 0;"),
  [{ file: "launch", find: "return tag == kCopyLive && bytes == 0;", replace: "return tag == kCopyLive;", why: "a live request may carry a payload that is then ignored (a message nobody should be able to send with a content)" },
   { file: "launch", find: "kCopyLive = 0x4C545653;", replace: "kCopyLive = 0x50545653;", why: "kCopyLive equals kCopyPlay: a play request opens the Live window" },
   { file: "launch", find: "a.live = true;", replace: "a.live = false;", why: "--live is parsed and ignored" }]);

pin("V2", "4.3 --live (running instance)", "the running instance takes kCopyLive ONLY with an empty payload (isLiveCopy), tested BEFORE --play's parse, and opens the Live window OFF the SendMessage (posted), never inside it",
  (S, C) => {
    const wp = fnBody(C.main, "LRESULT CALLBACK WndProc");
    return has(wp, "if (cds && cds->dwData == pblaunch::kCopyLive) { if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE; cancelQuit(); ::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0); return TRUE; }")
      && ordered(wp, ["pblaunch::kCopyLive", "pblaunch::kCopyPlay"])
      && has(wp, "else if (wParam == patchbus::Bridge::kWinOpenLive) openLiveWindow();");
  },
  [{ file: "main", find: "if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;", replace: "", why: "a live copy with any payload is accepted" },
   { file: "main", find: "::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0);\n            return TRUE;", replace: "openLiveWindow();\n            return TRUE;", why: "the Live window is created INSIDE the SendMessage of the other process (a modal wait on a window that opens a WebView2)" }]);

pin("V3", "4.3 --live (second launch)", "a second launch with --live hands the request over (kCopyLive, empty, the foreground handed over -- never from a test instance) and quits: 0 sent / 2 not; with --play too the paths follow and BOTH must have been taken",
  (S, C) => {
    const m = fnBody(C.main, "int WINAPI wWinMain");
    return has(m, "lcd.dwData = pblaunch::kCopyLive;") && has(m, "if (livePid && !testInstance) ::AllowSetForegroundWindow(livePid);")
      && has(m, "liveSent = ::SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&lcd), SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &lres) != 0 && lres == TRUE;")
      && has(m, "if (!args.play) return liveSent ? 0 : 2;") && has(m, "sent = sent && liveSent;")
      && ordered(m, ["if (args.live) {", "lcd.dwData = pblaunch::kCopyLive;", "if (args.play) {"]);
  },
  [{ file: "main", find: "if (!args.play) return liveSent ? 0 : 2;", replace: "if (!args.play) return 0;", why: "a forwarding that failed looks like success (the shortcut does nothing, silently)" },
   { file: "main", find: "if (livePid && !testInstance) ::AllowSetForegroundWindow(livePid);", replace: "if (livePid) ::AllowSetForegroundWindow(livePid);", why: "a test instance hands the user's foreground to a test window" },
   { file: "main", find: "sent = sent && liveSent;", replace: "sent = sent;", why: "--live --play reports success though the Live request was lost" }]);

pin("V4", "4.3 --live (first launch)", "--live OPENS ONLY THE LIVE WINDOW: the player's window is created but never shown (no taskbar button), its view invisible and its page told so; the Live window is requested and opened once the player's view exists -- the engine starts at that first attach; --live --play shows both",
  (S, C) => {
    const m = fnBody(C.main, "int WINAPI wWinMain"), env = fnBody(C.main, "void createWebViewEnv"), o = fnBody(C.main, "void openLiveWindow");
    return has(m, "if (args.live) g_liveRequested = true;")
      && has(m, "if (args.live && !args.play) { g_showMaxOnce = !g_quiet && haveSaved && pos.maximized; } else { ::ShowWindow(g_hwnd, show); ::UpdateWindow(g_hwnd); }")
      && has(env, "if (!::IsWindowVisible(hwnd)) g_controller->put_IsVisible(FALSE);")
      && has(env, "if (g_liveRequested) ::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0);")
      && ordered(o, ["cancelQuit();", "g_liveRequested = true;", "if (!g_environment || !g_webview || !g_bridge) return;", "g_liveRequested = false;"])
      && has(fnBody(C.bridge, "void Bridge::attach"), "trWin_.shown = ::IsWindowVisible(hwnd) != FALSE;");
  },
  [{ file: "main", find: "if (args.live && !args.play) {", replace: "if (false) {", why: "--live shows the player's window too (two taskbar buttons)" },
   { file: "main", find: "if (!::IsWindowVisible(hwnd)) g_controller->put_IsVisible(FALSE);", replace: "", why: "a hidden player's view stays visible (an invisible window decoding video at full rate)" },
   { file: "main", find: "                            if (g_liveRequested)\n                                ::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0);", replace: "", why: "--live at start never opens the Live window (no window at all)" },
   { file: "bridge", find: "trWin_.shown = ::IsWindowVisible(hwnd) != FALSE;", replace: "trWin_.shown = true;", why: "a hidden player's page believes it is on screen (it plays/decodes behind the Live window)" }]);

pin("V5", "4.3 --live (plain launch)", "a plain second launch while the player is HIDDEN means \"SuVtitle\": an empty --play brings the player back (and 0 sent / 2 not); only then the old \"bring the visible window up\" rule",
  (S, C) => {
    const m = fnBody(C.main, "int WINAPI wWinMain");
    return has(m, "if (existing && !::IsWindowVisible(existing)) { std::vector<wchar_t> none = pblaunch::packPlay({});") && has(m, "ecd.dwData = pblaunch::kCopyPlay;")
      && has(m, "return shownBack ? 0 : 2;") && ordered(m, ["if (args.play) {", "if (existing && !::IsWindowVisible(existing)) {", "if (existing && !testInstance) {"]);
  },
  [{ file: "main", find: "return shownBack ? 0 : 2;", replace: "return 0;", why: "the player never comes back and the launch says it did" },
   { file: "main", find: "if (existing && !::IsWindowVisible(existing)) {", replace: "if (false) {", why: "a second launch with the player hidden does nothing at all" }]);

// ================= W: the Live window's life ==============================================================================
pin("W1", "4.3 closing the Live window", "CLOSING the Live window = listening STOPS and the window HIDES (never destroyed: instant reopen, scrollback kept); if the player is not shown either, that was the LAST window and the app quits",
  (S, C) => {
    const c = fnBody(C.main, "void closeLiveWindow"), lw = fnBody(C.main, "LRESULT CALLBACK LiveWndProc"), br = fnBody(C.bridge, "void Bridge::liveWindowClosed");
    return ordered(c, ["if (!g_liveHwnd) return;", "saveLivePos();", "g_bridge->liveWindowClosed();", "g_liveController->put_IsVisible(FALSE);", "::ShowWindow(g_liveHwnd, SW_HIDE);", "if (!g_hwnd || !::IsWindowVisible(g_hwnd)) quitWhenDrained();"])
      && has(lw, "case WM_CLOSE: closeLiveWindow(); return 0;") && !/DestroyWindow/.test(lw.slice(0, lw.indexOf("case WM_DESTROY")))
      && has(br, "if (liveWanted_) handleLiveListen(false);");
  },
  [{ file: "main", find: "    if (g_bridge) g_bridge->liveWindowClosed();\n", replace: "", why: "closing the Live window leaves the microphone open (listening goes on, invisibly)" },
   { file: "main", find: "if (!g_hwnd || !::IsWindowVisible(g_hwnd)) quitWhenDrained();", replace: "if (false) quitWhenDrained();", why: "the last window closed and the app stays (an invisible process with the engine running)" },
   { file: "main", find: "    case WM_CLOSE:\n        closeLiveWindow();\n        return 0;", replace: "    case WM_CLOSE:\n        ::DestroyWindow(hwnd);\n        return 0;", why: "closing the Live window destroys it (no instant reopen, the listening not stopped by the bridge)" },
   { file: "bridge", find: "    if (liveWanted_) handleLiveListen(false);", replace: "    (void)0;", why: "the bridge is told the window closed and does nothing" }]);

pin("W2", "4.3 last-window rule", "the player's window closed while the Live window is open only HIDES (its page is told and pauses, its view goes invisible); bringUp() shows it again and tells it; with no Live window the close QUITS",
  (S, C) => {
    const wp = fnBody(C.main, "LRESULT CALLBACK WndProc"), h = fnBody(C.main, "void hidePlayerWindow"), b = fnBody(C.main, "void bringUp");
    return has(wp, "case WM_CLOSE: if (g_fullscreen) setFullscreen(false); saveWinPos(); if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) { hidePlayerWindow(); return 0; } quitWhenDrained(); return 0;")
      && ordered(h, ["::ShowWindow(g_hwnd, SW_HIDE);", "g_controller->put_IsVisible(FALSE);", "g_bridge->playerWindowShown(false);"])
      && has(b, "if (wasHidden) { if (g_controller) g_controller->put_IsVisible(TRUE); if (g_bridge) g_bridge->playerWindowShown(true); }")
      && has(fnBody(C.bridge, "void Bridge::playerWindowShown"), "if (trWin_.shown == on) return; trWin_.shown = on; postTranslatorWindowState(); if (player_) player_->windowShown(on);");
  },
  [{ file: "main", find: "if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) { hidePlayerWindow(); return 0; }", replace: "", why: "closing the player's window kills the app with the Live window (and the meeting) in it" },
   { file: "main", find: "    if (g_bridge) g_bridge->playerWindowShown(false);\n}", replace: "}", why: "a hidden player's page is not told: it goes on playing behind the Live window" },
   { file: "main", find: "if (g_bridge) g_bridge->playerWindowShown(true);", replace: "", why: "the player comes back but its page still believes it is hidden (paused)" },
   { file: "bridge", find: "if (trWin_.shown == on) return;", replace: "", why: "every tick re-posts the window state" }]);

pin("W3", "4.3 order of destruction", "the app's end: the player's WM_DESTROY destroys the Live window FIRST (its position saved, its view detached from the bridge, its controller closed), THEN shuts the bridge down -- a handler that captures the Bridge must not outlive it",
  (S, C) => {
    const wp = fnBody(C.main, "LRESULT CALLBACK WndProc"), lw = fnBody(C.main, "LRESULT CALLBACK LiveWndProc");
    return ordered(wp, ["case WM_DESTROY:", "if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }", "g_bridge->shutdown();", "g_controller->Close();", "::PostQuitMessage(0);"])
      && ordered(lw, ["case WM_DESTROY:", "g_bridge->detachLiveView();", "g_liveController->Close();", "g_liveHwnd = nullptr;"])
      && has(fnBody(C.bridge, "void Bridge::shutdown"), "detachLiveView();");
  },
  [{ file: "main", find: "        if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }\n        if (g_bridge) g_bridge->shutdown();", replace: "        if (g_bridge) g_bridge->shutdown();\n        if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }", why: "the bridge is gone while the Live view's handler can still fire (a call on a dead object)" },
   { file: "main", find: "        if (g_bridge) g_bridge->detachLiveView();\n        if (g_liveController)", replace: "        if (g_liveController)", why: "the Live window is destroyed with its handler still registered on the bridge" }]);

pin("W4", "4.3 the Live window", "the Live window: its own class SuVtitle.LiveWindow[.<tag>] (a test instance never joins the user's), a TOP-LEVEL window (not owned by the player's, so it keeps its own taskbar button), never activated for a test instance, 360 x 240 minimum, created only once the player's view exists",
  (S, C) => {
    const o = fnBody(C.main, "void openLiveWindow");
    return has(o, "liveClass = pblaunch::suffixed(kLiveWindowClass);") && has(C.main, 'constexpr wchar_t kLiveWindowClass[] = L"SuVtitle.LiveWindow";')
      && has(C.main, 'constexpr wchar_t kLiveWindowTitle[] = L"SuVtitle Live";')
      && has(o, "g_liveHwnd = ::CreateWindowExW((g_liveTopmost ? WS_EX_TOPMOST : 0) | (g_quiet ? WS_EX_NOACTIVATE : 0), liveClass.c_str(), kLiveWindowTitle, WS_OVERLAPPEDWINDOW,")
      && has(o, "nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);")
      && has(o, "::ShowWindow(g_liveHwnd, g_quiet ? SW_SHOWNOACTIVATE : (have && pos.maximized ? SW_SHOWMAXIMIZED : SW_SHOW));")
      && has(o, "if (::IsIconic(g_liveHwnd)) ::ShowWindow(g_liveHwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);") && has(o, "if (!g_quiet) ::SetForegroundWindow(g_liveHwnd);")
      && has(o, "if (!g_environment || !g_webview || !g_bridge) return;")
      && has(fnBody(C.main, "LRESULT CALLBACK LiveWndProc"), "mmi->ptMinTrackSize.x = (LONG)(360 * dpi / 96); mmi->ptMinTrackSize.y = (LONG)(240 * dpi / 96);");
  },
  [{ file: "main", find: "liveClass = pblaunch::suffixed(kLiveWindowClass);", replace: "liveClass = kLiveWindowClass;", why: "a test instance's Live window has the user's class (FindWindow finds the wrong one)" },
   { file: "main", find: /\| \(g_quiet \? WS_EX_NOACTIVATE : 0\),\s*liveClass\.c_str\(\)/, replace: ", liveClass.c_str()", why: "a test instance's Live window can take the foreground (the user may be gaming)" },
   { file: "main", find: "if (!g_quiet) ::SetForegroundWindow(g_liveHwnd);", replace: "::SetForegroundWindow(g_liveHwnd);", why: "a test instance takes the user's foreground" },
   { file: "main", find: "                                       nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);\n        if (!g_liveHwnd) return;", replace: "                                       g_hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);\n        if (!g_liveHwnd) return;", why: "the Live window is OWNED by the player's: it hides with it, and has no taskbar button of its own" },
   { file: "main", find: "mmi->ptMinTrackSize.y = (LONG)(240 * dpi / 96);", replace: "mmi->ptMinTrackSize.y = (LONG)(24 * dpi / 96);", why: "the Live window can be shrunk to a title bar (the plate and the last lines have no room)" }]);

pin("W5", "4.3 always-on-top / RV4-7", "the Live window's pin: the page's alwaysOnTop is posted to the window (kWinLiveTopmost), which asks Windows and keeps and TELLS what Windows made of it, every time (a refusal included); another program's change is followed (WM_WINDOWPOSCHANGED); the Live page's `ready` gets the real state; the player's Live button (openLive) posts kWinOpenLive",
  (S, C) => {
    const lw = fnBody(C.main, "LRESULT CALLBACK LiveWndProc"), hm = fnBody(C.bridge, "void Bridge::handleLiveMessage");
    return has(fnBody(C.main, "void setLiveTopmost"), "::SetWindowPos(g_liveHwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); g_liveTopmost = realTopmost(g_liveHwnd); saveLivePos(); if (g_bridge) g_bridge->liveTopmostChanged(g_liveTopmost);")
      && has(lw, "case WM_WINDOWPOSCHANGED: if (g_liveHwnd == hwnd && realTopmost(hwnd) != g_liveTopmost) { g_liveTopmost = !g_liveTopmost; saveLivePos(); if (g_bridge) g_bridge->liveTopmostChanged(g_liveTopmost); } break;")
      && has(hm, 'if (json.find(L"\\"alwaysOnTop\\"") != std::wstring::npos && jsonFindBool(json, L"alwaysOnTop", topOn) && hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, kWinLiveTopmost, topOn ? 1 : 0);')
      && has(C.main, "else if (wParam == patchbus::Bridge::kWinLiveTopmost) setLiveTopmost(lParam != 0);")
      && has(fnBody(C.bridge, "void Bridge::liveTopmostChanged"), "liveTopmost_ = on; postLiveWindowState();")
      && has(fnBody(C.bridge, "void Bridge::postLiveWindowState"), 'kv(L"type", L"translatorWindow").kv(L"topmost", liveTopmost_)')
      && has(C.bridge, 'if (type == L"openLive") { if (hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, kWinOpenLive, 0); return; }');
  },
  [{ file: "main", find: "    g_liveTopmost = realTopmost(g_liveHwnd);\n    saveLivePos();", replace: "    g_liveTopmost = on;\n    saveLivePos();", why: "the Live window's pin shows the wish, not what Windows made of it (a refusal is never told)" },
   { file: "main", find: "    saveLivePos();\n    if (g_bridge) g_bridge->liveTopmostChanged(g_liveTopmost);\n}", replace: "    saveLivePos();\n}", why: "the page's pin flipped when clicked and never comes back to the truth" },
   { file: "bridge", find: "kWinLiveTopmost, topOn ? 1 : 0);", replace: "1, topOn ? 1 : 0);", why: "the Live page's pin drives the PLAYER's always-on-top (wParam 1)" },
   { file: "bridge", find: "if (hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, kWinOpenLive, 0);", replace: "(void)0;", why: "the player's Live button does nothing" }]);

pin("W6", "4.3 the engine", "the ENGINE starts once, at the FIRST view's attach -- the player's or the Live window's, whichever comes first; a test instance's runs on the CPU (the user's GPU is his)",
  (S, C) => {
    const se = fnBody(C.bridge, "void Bridge::startEngine");
    return has(se, "if (engineStarted_) return; engineStarted_ = true;") && has(se, 'if (pblaunch::isTestInstance()) translator_->setSpawnArgs(L"--cpu");') && has(se, "translator_->start();")
      && has(fnBody(C.bridge, "void Bridge::attach"), "startEngine();") && has(fnBody(C.bridge, "void Bridge::attachLiveView"), "startEngine();")
      && count(C.bridge, /\btranslator_->start\(\)/g) === 1;
  },
  [{ file: "bridge", find: "    if (engineStarted_) return;\n    engineStarted_ = true;", replace: "    engineStarted_ = true;", why: "the second view's attach starts a second engine" },
   { file: "bridge", find: "    if (!liveView_) return;\n    startEngine();", replace: "    if (!liveView_) return;", why: "a Live window that attaches first has no engine behind it" },
   { file: "bridge", find: 'if (pblaunch::isTestInstance()) translator_->setSpawnArgs(L"--cpu");', replace: "", why: "a test instance's engine takes the user's GPU" }]);

pin("W7", "2 / 3.3 the player's gate", "the player's gate = the wish OR the engine's echo: closed the moment Listen is pressed (before the engine answers), opened only once the engine says it stopped; a wish the supervisor DROPPED (the engine keeps dying at LIVE 1) is withdrawn; a dead engine lights no lamp",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return has(fnBody(C.bridge, "void Bridge::updateLiveGate"), "const bool gate = liveWanted_ || trLive_; if (gate == liveGate_) return; liveGate_ = gate; if (player_) player_->setLive(gate);")
      && ordered(poll, ["if (translator_->pollLiveDropped()) { liveWanted_ = false; dirty = true; }", "if (translator_->pollLiveRefused(why)) {", "liveWanted_ = false;", "updateLiveGate();"])
      && has(poll, "if (st.live != trLive_) { trLive_ = st.live; if (!trLive_) for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); } }");
  },
  [{ file: "bridge", find: "const bool gate = liveWanted_ || trLive_;", replace: "const bool gate = trLive_;", why: "a FILE races the LIVE 1 (the gate waits for the echo)" },
   { file: "bridge", find: "if (translator_->pollLiveDropped()) { liveWanted_ = false; dirty = true; }", replace: "", why: "the Listen button stays on after the supervisor gave up (the player's analysis never resumes)" },
   { file: "bridge", find: "if (!trLive_) for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }", replace: "", why: "after the engine stops, the strips keep a lit lamp" }]);

// ================= T: the engine supervisor, live half ====================================================================
pin("T1", "4.3 TranslatorManager", "the control block writes, IN THIS ORDER and before any file command: the player's CTL (no scope, no clear) -> the LIVE CTL (scope=live, the live pair, the shared preset, the Clear) -> LIVE -> SRCLIST; a respawned engine is told the live pair, the wanted LIVE and the picker's ask again -- never a FILE",
  (S, C) => {
    const m = C.mgr, block = m.slice(m.indexOf("if (ctlPending_) {"), m.indexOf("if (liveCtlPending_ || clearPending_) {"));
    return ordered(m, ["if (ctlPending_) {", "out.push_back(tp::packCtl(c));", "if (liveCtlPending_ || clearPending_) {", "c.scope = tp::kCtlScopeLive;", "out.push_back(tp::packCtl(c));", "if (livePending_) {", "out.push_back(tp::packLive(live_));", "if (srcListPending_) {", "out.push_back(tp::packSrcList());", "for (const std::string& w : out) {", "std::deque<FileCmd> cmds;"])
      && block.length > 100 && !/clear|scope/.test(block) && has(m, "c.clear = clearPending_;") && has(m, "c.preset = preset_;")
      && has(m, "liveCtlPending_ = !liveMode_.empty() || !liveLangA_.empty() || !liveLangB_.empty(); livePending_ = live_.on; srcListPending_ = srcWanted_;");
  },
  [{ file: "mgr", find: /c\.scope\s*=\s*tp::kCtlScopeLive;/, replace: "", why: "the live CTL reaches the engine without a scope: it changes the PLAYER's pair" },
   { file: "mgr", find: /c\.clear\s*=\s*clearPending_;/, replace: "", why: "the Live window's Clear never reaches the engine (the queue keeps its old lines)" },
   { file: "mgr", find: "livePending_ = live_.on;", replace: "livePending_ = false;", why: "a respawned engine is not told to listen again (a meeting silently stops after any crash)" },
   { file: "mgr", find: "srcListPending_ = srcWanted_;", replace: "srcListPending_ = false;", why: "a respawned engine never re-sends the picker's list (the picker stays empty)" }]);

pin("T2", "4.3 RV2-5 on the live path", "an engine that dies within 30 s of a LIVE 1 is a FAILED START (back-off); the THIRD in a row DROPS the wanted LIVE (not re-sent, the user warned, the engine comes back for the player); setLive(true) -- the user trying again -- forgives; 30 s of listening forgives too",
  (S, C) => {
    const m = C.mgr;
    return has(m, "if (++liveFailStreak_ >= 3 && live_.on) { live_ = tp::WireLive{}; livePending_ = false; liveDropped_ = true; dropped = true; }")
      && has(m, 'if (dropped) warn(L"Live translation stopped: the SuVtitle engine kept stopping when it started listening.");')
      && has(m, "if (lastLiveAt != 0 && now - lastLiveAt < 30000) { lastLiveAt = 0; liveDeath(); failedStart(); return; }")
      && has(m, 'if (w.rfind("LIVE\\t1", 0) == 0) { if (!listening || wantDevs != liveDevs) lastLiveAt = ::GetTickCount64(); listening = true; liveDevs = wantDevs; } else { lastLiveAt = 0; listening = false; liveDevs.clear(); }')
      && has(m, "if (lastLiveAt != 0 && ::GetTickCount64() - lastLiveAt >= 30000) { lastLiveAt = 0; std::lock_guard<std::mutex> lk(mx_); liveFailStreak_ = 0; }")
      && has(fnBody(C.mgr, "void TranslatorManager::setLive"), "if (on && !live_.on) { liveFailStreak_ = 0; liveLateDeaths_.clear(); }");
  },
  [{ file: "mgr", find: "++liveFailStreak_ >= 3", replace: "++liveFailStreak_ >= 4", why: "the engine is asked to listen a fourth time into the same crash" },
   { file: "mgr", find: "        if (on && !live_.on) { liveFailStreak_ = 0; liveLateDeaths_.clear(); }\n", replace: "", why: "the user pressing Listen again is never forgiven (the drop is for ever)" },
   { file: "mgr", find: "if (on && !live_.on) { liveFailStreak_ = 0;", replace: "if (on) { liveFailStreak_ = 0;", why: "every strip edit while listening forgives the engine its live deaths (RVB-13: a trim drag resets the crash count)" },
   { file: "mgr", find: "now - lastLiveAt < 30000) {", replace: "now - lastLiveAt < 0) {", why: "a death right after the LIVE 1 is not counted: the crash loop goes on" },
   { file: "mgr", find: "                    } else {\n                        lastLiveAt = 0;\n                        listening = false;", replace: "                    } else {\n                        lastLiveAt = ::GetTickCount64();\n                        listening = false;", why: "a LIVE 0 starts the 30 s clock too (the stop is taken for a failed start)" },
   { file: "mgr", find: "if (!listening || wantDevs != liveDevs) lastLiveAt = ::GetTickCount64();", replace: "lastLiveAt = ::GetTickCount64();", why: "a trim or a rename restarts RV2-5's 30 s clock (RVB-13: a death later in the listen taken for a failed start)" }]);

pin("T3", "4.3 SRC lists", "the picker's list is assembled HERE, whole: a newer gen supersedes, an older gen is dropped, a short list is asked again at most twice (a third is delivered as it is), bounded at 512 sources, one message per list",
  (S, C) => {
    const m = C.mgr;
    return has(m, "constexpr size_t kSrcMax = 512;") && has(m, "if (s.gen > srcGen) { srcGen = s.gen; srcPend.clear(); srcOverflow = false; }") && has(m, "if (s.gen == srcGen) {")
      && has(m, "if (tp::unpackSrcEnd(t, e) && e.gen >= srcGen) {") && has(m, "const bool whole = !srcOverflow && srcPend.size() == e.n;")
      && has(m, "if (!whole && srcAsks < 2) { ++srcAsks;") && has(m, "m.kind = Msg::Kind::Sources; m.srcGen = e.gen; m.srcs = std::move(srcPend);");
  },
  [{ file: "mgr", find: "if (!whole && srcAsks < 2) {", replace: "if (!whole && srcAsks < 200) {", why: "a list that never completes is asked again for ever" },
   { file: "mgr", find: "if (s.gen == srcGen) {", replace: "if (true) {", why: "an OLDER list's lines are mixed into the newer one" },
   { file: "mgr", find: "const bool whole = !srcOverflow && srcPend.size() == e.n;", replace: "const bool whole = true;", why: "a list with lost lines is delivered as complete (a source silently missing from the picker)" },
   { file: "mgr", find: "constexpr size_t kSrcMax = 512;", replace: "constexpr size_t kSrcMax = 1u << 30;", why: "a hostile engine grows the list without a bound" }]);

pin("T4", "4.3 queues", "LEVELS are a latest-wins slot (~5 Hz, never queued behind a wedged UI thread); Sources and Input are NEVER capped (a lamp must not keep lying, a list lost is never said again); a dead engine lights no lamp (state.live=0)",
  (S, C) => {
    const lv = S.mgr.slice(S.mgr.indexOf('t[0] == "LEVELS"'), S.mgr.indexOf("// Anything else is ignored"));
    return lv.length > 100 && !/pushMsgLocked|msgs_/.test(lv.replace(/\/\/[^\n]*/g, "")) && has(C.mgr, "levels_ = std::move(l); levelsDirty_ = true;")
      && has(C.mgrH, "return k == Msg::Kind::Line || k == Msg::Kind::Partial || k == Msg::Kind::End;") && has(C.mgr, "state_.live = false;");
  },
  [{ file: "mgr", find: "                    levels_ = std::move(l);\n                    levelsDirty_ = true;", replace: "                    levels_ = std::move(l);\n                    levelsDirty_ = true; Msg qm; qm.kind = Msg::Kind::Input; pushMsgLocked(std::move(qm));", why: "every LEVELS frame is queued (a wedged UI thread grows the queue 5 times a second)" },
   { file: "mgrH", find: "return k == Msg::Kind::Line || k == Msg::Kind::Partial || k == Msg::Kind::End;", replace: "return k == Msg::Kind::Line || k == Msg::Kind::Partial || k == Msg::Kind::End || k == Msg::Kind::Input;", why: "a burst of lines drops an INPUT state: a lamp lies for ever" },
   { file: "mgr", find: "            state_.live = false;          // SUVTITLE LIVE: a dead engine listens to nothing (the WISH stays: live_)\n", replace: "", why: "a dead engine still shows the on-air lamp" }]);

// ================= G: the player's live gate ===============================================================================
// (U1, 02/11/2026: the global `translate_` went -- a file is analysed when WANTED, Translate or "Translate all"; the gate's
//  rule is the same and now holds the batch too: schedule's first line, `want` in phaseOf, `failed` named once in onDone)
pin("G1", "2 the live gate", "the player's gate (TrPlayer.h): closing it cancels every running job (their vouched part is kept), no FILE leaves while it is closed (ensureCurrentJob, schedule: \"Translate all\" too -- U1), an error caused by the live mode fails nothing, and the strip says \"live\" (a notice, not loading, not an error)",
  (S, C) => {
    const p = C.player;
    return has(p, "void setLive(bool on) { if (on == live_) return; live_ = on; if (on) for (Job& j : jobs_) { j.liveHit = true; cancelJob(j); } stateDirty_ = true; schedule(); flush(); }")
      && ordered(fnBody(p, "void ensureCurrentJob"), ["if (s_.probeWait) return;", "if (live_) return;", "if (!helper_) {"])
      && has(fnBody(p, "void schedule"), "if (!helper_ || live_) { cancelPre(); return; }")
      // (U3 re-point: scoped to the ANALYSIS' onDone -- the voice pass has the same rule, proved by player_smoke [U3])
      && has(fnBody(p, "void onDone"), "const bool liveErr = live_ || j.liveHit;") && has(fnBody(p, "void onDone"), 'const bool failed = d.result == "error" && !liveErr;')
      // (U2b re-point: a batch file its imported model failed on is REDONE, not failed -- the same live rule before it)
      && has(fnBody(p, "void onDone"), "if (failed && !redo) {") && has(p, "} else if (j.pre && failed && !redo) {")
      && has(fnBody(p, "std::string phaseOf"), 'if (live_ && want && s_.set && !s_.set->complete && !s_.userCancelled && !s_.failed) return "live";');
  },
  [{ file: "player", find: /\n\s*if \(live_\) return;[^\n]*/, replace: "", why: "a FILE is sent while the live mode listens (the engine refuses it, the item is marked failed)" },
   { file: "player", find: "if (!helper_ || live_) {", replace: "if (!helper_) {", why: "\"Translate all\" starts the next file while the live mode listens (U1)" },
   { file: "player", find: "const bool liveErr = live_ || j.liveHit;\n        const bool failed = d.result == \"error\" && !liveErr;", replace: "const bool liveErr = false;\n        const bool failed = d.result == \"error\" && !liveErr;", why: "the FDONE \"error\" the live mode caused marks the film as failed" },
   { file: "player", find: "{ j.liveHit = true; cancelJob(j); }", replace: "{ j.liveHit = true; }", why: "a running analysis is not cancelled when the live mode starts (two heavy jobs at once)" },
   { file: "player", find: '!s_.userCancelled && !s_.failed) return "live";', replace: '!s_.userCancelled && !s_.failed) return "loading";', why: "the page waits for subtitles that are not coming (\"play when ready\")" }]);

// ================= X: the FIX ROUND (2026-10-26: docs/REVIEW_RV-LB.md, LIVE_PLAN.md "ADDENDUM FIX ROUND") ===================
pin("X1", "FIX ROUND RVB-1", "the session survives a restart until Clear: the journal's FILE NAME is kept in app.txt (LIVESESSION) when a session starts and removed by a Clear; at construction the bridge takes it back -- its path (the next line appends, the Export exports the whole file), its line count, its last 200 lines (the replay), its speakers' colours and voices -- and a torn last line is closed first",
  (S, C) => {
    const ctor = fnBody(C.bridge, "Bridge::Bridge"), ap = fnBody(C.bridge, "void Bridge::appendTranslatorSession"), end = fnBody(C.bridge, "void Bridge::endLiveSession");
    const ra = fnBody(C.bridge, "void Bridge::reattachLiveSession"), tag = fnBody(C.bridge, "void Bridge::setLiveSessionTag");
    return ordered(ctor, ["loadLiveConfig();", "spkRun_ = run;", "reattachLiveSession();"])
      && ordered(ap, ["if (trSessionPath_.empty()) {", "setLiveSessionTag(wideToUtf8(name));", "trJournalOwed_ += wideToUtf8(lineJson)"])
      && has(end, "setLiveSessionTag(std::string());")
      && has(tag, "if (!pbtrf::readFileAll(txt, was, 1ULL << 20) && ::GetFileAttributesW(txt.c_str()) != INVALID_FILE_ATTRIBUTES) return;")
      && has(tag, "const std::string now = pbapp::packLiveSession(was, name); if (now != was) pbtrf::writeFileAvSafe(txt, now);")
      && ordered(ra, ["const std::string name = pbapp::parseLiveSession(text);", "OPEN_EXISTING", "record(part);", "trSessionPath_ = path;", "trSessionLines_ = count;",
                      "translatorLog_ = std::move(tail);", "spkColor_ = std::move(colors);", "spkNextColor_ = distinct;", "spkVoices_ = std::move(voices);"])
      && has(ra, "if (tail.size() > kTranslatorLogMax) tail.pop_front();")
      && has(ra, 'if (!part.empty() || longLine) { trJournalOwed_ = "\\n"; trJournalSeps_ = 1; }')
      && has(fnBody(C.config, "inline bool liveSessionNameOk"), "if (!((c >= '0' && c <= '9') || c == '-' || c == '_')) return false;");
  },
  [{ file: "bridge", find: "        setLiveSessionTag(wideToUtf8(name));\n", replace: "", why: "the session is not remembered: a restart starts an empty one -- the meeting's export lost with the window (RVB-1)" },
   { file: "bridge", find: "    setLiveSessionTag(std::string());\n}", replace: "}", why: "a Clear does not end the remembered session (a restart brings the cleared meeting back)" },
   { file: "bridge", find: "    reattachLiveSession();   //", replace: "    //", why: "the remembered session is never taken back" },
   { file: "bridge", find: "    trSessionPath_ = path;\n    trSessionLines_ = count;", replace: "    trSessionLines_ = count;", why: "the session is replayed but its file not reattached: Export says nothing to export, the next line starts another file" },
   { file: "bridge", find: "    spkColor_ = std::move(colors);\n    spkNextColor_ = distinct;", replace: "    spkNextColor_ = 0;", why: "after a restart the colours start over: a returning speaker wears someone else's" },
   { file: "bridge", find: '    if (!part.empty() || longLine) { trJournalOwed_ = "\\n"; trJournalSeps_ = 1; }', replace: "", why: "the next line is glued to a torn one (both lost to the export)" },
   { file: "config", find: "if (!((c >= '0' && c <= '9') || c == '-' || c == '_')) return false;", replace: "(void)c;", why: "a hand-edited LIVESESSION names a path (a drive, \"..\", a stream)" }]);

pin("X2", "FIX ROUND RVB-5", "a LIVE the engine REFUSED (a STATE with err, not live, status not \"error\", while the wish is on and no newer LIVE is on its way) DROPS the wish in the supervisor (never re-sent to a respawned engine) and in the bridge (the player's gate opens; the Live window says why); app.txt's wav: strips are a test instance's only",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce"), ld = fnBody(C.bridge, "void Bridge::loadLiveConfig");
    return has(C.mgr, 'if (!s.err.empty() && !s.live && s.status != "error" && live_.on && !livePending_) { live_ = tp::WireLive{}; liveRefused_ = s.err; }')
      && has(fnBody(C.mgr, "bool TranslatorManager::pollLiveRefused"), "if (liveRefused_.empty()) return false; why.swap(liveRefused_); liveRefused_.clear(); return true;")
      && ordered(poll, ["if (translator_->pollLiveRefused(why)) {", "liveWanted_ = false;", "postLiveWarn(utf8ToWide(why.c_str()));", "dirty = true;", "updateLiveGate();"])
      && has(ld, 'if (pbt::trpipe::liveKeyKind(in.key) == "wav" && !pblaunch::isTestInstance()) continue;');
  },
  [{ file: "mgr", find: "live_ = tp::WireLive{};\n                        liveRefused_ = s.err;", replace: "liveRefused_ = s.err;", why: "the refused wish stays in the supervisor: every respawned engine is asked again" },
   { file: "mgr", find: '!s.live && s.status != "error" && live_.on', replace: "!s.live && live_.on", why: "the engine's own error state (it retries its servers) is taken for a refusal: the user's Listen dropped" },
   { file: "bridge", find: "            liveWanted_ = false;\n            for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }\n            postLiveWarn(utf8ToWide(why.c_str()));", replace: "            postLiveWarn(utf8ToWide(why.c_str()));", why: "the app keeps WANTING to listen: \"starting to listen...\" for ever, the player's analysis held (RVB-5)" },
   { file: "bridge", find: "            postLiveWarn(utf8ToWide(why.c_str()));\n", replace: "", why: "the refusal is never said (the Listen button just goes off)" },
   { file: "bridge", find: 'if (pbt::trpipe::liveKeyKind(in.key) == "wav" && !pblaunch::isTestInstance()) continue;', replace: "", why: "a wav: strip in a hand-edited app.txt reaches the product's engine (refused there, every time)" }]);

pin("X3", "FIX ROUND RVB-6", "a VOICE is its cluster + this run + the engine incarnation that heard it (Msg::gen, +1 at each spawn): after a respawn or a restart the new S1 is a new voice -- a new colour, the next number -- never the colour or the number of whoever was S1",
  (S, C) => {
    const poll = fnBody(C.bridge, "void Bridge::pollTranslatorOnce");
    return has(poll, 'const std::wstring voice = clus + L"~" + spkRun_ + L"." + std::to_wstring(msg.gen);')
      && has(poll, "while (n < voices.size() && voices[n] != voice) ++n;")
      && has(C.mgr, "m.gen = incarnation;") && has(C.mgr, "++incarnation;") && has(C.mgrH, "uint32_t gen = 0;")
      && has(fnBody(C.bridge, "Bridge::Bridge"), "spkRun_ = run;");
  },
  [{ file: "bridge", find: 'const std::wstring voice = clus + L"~" + spkRun_ + L"." + std::to_wstring(msg.gen);', replace: "const std::wstring voice = clus;", why: "a respawned engine's S1 takes the colour and the number of the old S1 -- someone else (RVB-6)" },
   { file: "mgr", find: "            ++incarnation;", replace: "", why: "every engine is the same incarnation: a respawn's voices are taken for the old ones" },
   { file: "mgr", find: "m.gen = incarnation;", replace: "m.gen = 0;", why: "the LINEs do not say which engine heard them" }]);

pin("X4", "FIX ROUND RVB-7", "the list's ask lives only while the picker wants it: liveSources{open:false} (the picker closed), a (re)loaded page and the Live window's close all STOP it (no respawned engine is asked again); three deaths right after a SRCLIST drop it, said; requestSources (the user opening the picker) forgives",
  (S, C) => {
    const h = fnBody(C.bridge, "void Bridge::handleLiveMessage");
    return has(h, 'if (json.find(L"\\"open\\"") != std::wstring::npos) jsonFindBool(json, L"open", open);')
      && has(h, "if (!open) { if (translator_) translator_->stopSources(); return; }")
      && has(h, 'if (type == L"ready" && translator_) translator_->stopSources();')
      && has(fnBody(C.bridge, "void Bridge::liveWindowClosed"), "translator_->stopSources();")
      && has(fnBody(C.mgr, "void TranslatorManager::stopSources"), "srcWanted_ = false; srcListPending_ = false;")
      && has(fnBody(C.mgr, "void TranslatorManager::requestSources"), "srcFailStreak_ = 0;")
      && has(C.mgr, "if (++srcFailStreak_ >= 3 && srcWanted_) { srcWanted_ = false; srcListPending_ = false; dropped = true; }")
      && has(C.mgr, "if (lastSrcAt != 0 && now - lastSrcAt < kSrcDeathMs) srcDeath();")
      && has(C.mgr, "if (w == tp::packSrcList()) lastSrcAt = ::GetTickCount64();")
      && has(S.live, 'send({ type: "liveSources", open: false });');
  },
  [{ file: "bridge", find: "        if (!open) {\n            if (translator_) translator_->stopSources();\n            return;\n        }", replace: "", why: "the picker's close is ignored: every engine respawned for the rest of the session enumerates for nobody (RVB-7)" },
   { file: "bridge", find: '        if (type == L"ready" && translator_) translator_->stopSources();\n', replace: "", why: "a page that reloaded with its picker closed leaves the engine asked" },
   { file: "mgr", find: "if (++srcFailStreak_ >= 3 && srcWanted_) {", replace: "if (false) {", why: "an engine that crashes enumerating is respawned into the same crash for ever" },
   { file: "mgr", find: "    srcWanted_ = false;\n    srcListPending_ = false;\n}", replace: "    srcListPending_ = false;\n}", why: "stopSources keeps the wish: the next engine is asked again" }]);

pin("X5", "FIX ROUND RVB-8", "deaths LATER in a listen (> 30 s after its LIVE 1) are counted in a 10-minute window: the second waits 8 s before the respawn, the THIRD drops the wish (said; the engine comes back for the player); only the user's NEW wish forgives",
  (S, C) => {
    const m = C.mgr;
    return has(m, "static constexpr uint64_t kLiveLateWindowMs = 600000;") && has(m, "static constexpr size_t kLiveLateMax = 3;")
      && has(m, "while (!liveLateDeaths_.empty() && now - liveLateDeaths_.front() >= kLiveLateWindowMs) liveLateDeaths_.pop_front();")
      && has(m, "if (n >= kLiveLateMax && live_.on) { live_ = tp::WireLive{}; livePending_ = false; liveDropped_ = true; liveLateDeaths_.clear(); dropped = true; }")
      && has(m, "if (wasListening && liveLateDeath(now) == 2) nextSpawn = std::max<uint64_t>(respawnAt, now + kLiveLateBackoffMs);")
      && has(m, 'if (dropped) warn(L"Live translation stopped: the SuVtitle engine kept stopping while it listened.");');
  },
  [{ file: "mgr", find: "if (wasListening && liveLateDeath(now) == 2)", replace: "if (false && liveLateDeath(now) == 2)", why: "a crash later in each listen loops for ever (the models reloaded once a minute, RVB-8)" },
   { file: "mgr", find: "if (n >= kLiveLateMax && live_.on) {", replace: "if (false) {", why: "the late deaths are counted but the wish is never dropped" },
   { file: "mgr", find: "now - liveLateDeaths_.front() >= kLiveLateWindowMs", replace: "now - liveLateDeaths_.front() >= 0", why: "the window is emptied at every death (never three in it)" }]);

pin("X6", "FIX ROUND RVB-12", "a journal write that FAILS keeps its lines owed (written with the next line, in order, from where a partial write stopped; bounded at 8 MB) and is SAID once per run of failures (the Live window's warn + a line of live.log); the Export reads what is owed too",
  (S, C) => {
    const w = fnBody(C.bridge, "bool Bridge::writeJournalPending"), jf = fnBody(C.bridge, "void Bridge::journalFailed"), ex = fnBody(C.bridge, "void Bridge::handleTranslatorExport");
    return has(w, "journalFailed(err ? err : ERROR_WRITE_FAULT);") && has(w, "if (trJournalOwed_.size() > (8u << 20)) {")
      && has(jf, "if (trJournalWarned_) return; trJournalWarned_ = true;") && has(jf, 'postLiveWarn(L"The session file cannot be written (" + why')
      && has(jf, 'path = dir + L"\\\\live.log";') && has(ex, "raw += trJournalOwed_;");
  },
  [{ file: "bridge", find: "    if (trJournalWarned_) return;\n    trJournalWarned_ = true;", replace: "    trJournalWarned_ = true;", why: "every failed line warns again (a full disk = a warning per sentence)" },
   { file: "bridge", find: "    journalFailed(err ? err : ERROR_WRITE_FAULT);\n", replace: "", why: "a failing journal is never said: the export silently lacks the end of the meeting (RVB-12)" },
   { file: "bridge", find: "    raw += trJournalOwed_;", replace: "", why: "the export misses the lines a failing write still owes" }]);

pin("X7", "FIX ROUND RVB-13", "a TRIM being dragged (only gains changed) is persisted once it RESTS (1 s, uiTick) -- never five rewrites of app.txt a second on the UI thread; any other edit at once; a pending trim is written at the end (shutdown, the Windows session's end)",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleLiveConfig");
    return has(b, "trimOnly = next[i].key == liveStrips_[i].key && next[i].label == liveStrips_[i].label && next[i].chan == liveStrips_[i].chan;")
      && has(b, "if (trimOnly) liveCfgSaveDue_ = ::GetTickCount64() + 1000;")
      && has(fnBody(C.bridge, "void Bridge::uiTick"), "if (liveCfgSaveDue_ && ::GetTickCount64() >= liveCfgSaveDue_) flushLiveConfig();")
      && has(fnBody(C.bridge, "void Bridge::shutdown"), "flushLiveConfig();") && has(fnBody(C.bridge, "void Bridge::persistPlayer"), "flushLiveConfig();");
  },
  [{ file: "bridge", find: "    if (trimOnly) liveCfgSaveDue_ = ::GetTickCount64() + 1000;\n    else          { liveCfgSaveDue_ = 0; saveLiveConfig(); }", replace: "    liveCfgSaveDue_ = 0; saveLiveConfig();", why: "every 200 ms of a trim drag rewrites app.txt on the UI thread (RVB-13)" },
   { file: "bridge", find: "    if (liveCfgSaveDue_ && ::GetTickCount64() >= liveCfgSaveDue_) flushLiveConfig();", replace: "", why: "a trim let go is never written (lost at the next start)" },
   { file: "bridge", find: "    if (!trJournalOwed_.empty()) writeJournalPending();\n    flushLiveConfig();", replace: "    if (!trJournalOwed_.empty()) writeJournalPending();", why: "a trim made just before quitting is lost" }]);

pin("X8", "FIX ROUND R5", "a TEST INSTANCE (only) may answer the Export's Save dialog by SUVTITLE_TEST_EXPORT (a path, or \"cancel\") -- read through isTestInstance(), never by the product, and only once the document is built: no OS window in an automated run",
  (S, C) => {
    const b = fnBody(C.bridge, "void Bridge::handleTranslatorExport");
    return has(b, 'const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L"SUVTITLE_TEST_EXPORT", tv, MAX_PATH) : 0;')
      && ordered(b, ["pbdocx::buildTranscript(", 'L"SUVTITLE_TEST_EXPORT"', 'if (answer == L"cancel") { postTranslatorExported(false, L"cancelled", 0); return; }', "pbtrf::writeFileAvSafe(answer, bytes, true)", "IFileSaveDialog* dlg"])
      // (U3 re-point: TWO readers now -- this Live export's and the transcript's Save, test-instance only too: test_host_pins U3-2)
      && count(b, /SUVTITLE_TEST_EXPORT/g) === 1 && count(C.bridge, /SUVTITLE_TEST_EXPORT/g) === 2
      && has(fnBody(C.bridge, "void Bridge::handleTranscriptSave"), 'const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L"SUVTITLE_TEST_EXPORT", tv, MAX_PATH) : 0;');
  },
  [{ file: "bridge", find: "const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L\"SUVTITLE_TEST_EXPORT\", tv, MAX_PATH) : 0;\n        if (tn > 0 && tn < MAX_PATH) {\n            const std::wstring answer = tv;", replace: "const DWORD tn = true ? ::GetEnvironmentVariableW(L\"SUVTITLE_TEST_EXPORT\", tv, MAX_PATH) : 0;\n        if (tn > 0 && tn < MAX_PATH) {\n            const std::wstring answer = tv;", why: "the PRODUCT reads the test hook: an environment variable would write the transcript anywhere, with no dialog" },
   { file: "bridge", find: '            if (answer == L"cancel") { postTranslatorExported(false, L"cancelled", 0); return; }\n', replace: "", why: "the test's Cancel writes a file named \"cancel\"" }]);

pin("X9", "FIX ROUND RVB-3 / RVB-4", "RECOVERY never activates a window: rebuildViews and a failed rebuild never call openLiveWindow, SW_RESTORE, SetForegroundWindow, a MessageBox or DestroyWindow; a failed REBUILD is retried (2..64 s), never the startup's MessageBox-and-quit; a Live view's stale creation callback (g_liveGen) closes what it made and touches nothing else; only a creation the USER asked for may say it failed",
  (S, C) => {
    const rb = fnBody(C.main, "void rebuildViews"), rf = fnBody(C.main, "void rebuildFailed"), cl = fnBody(C.main, "void createLiveView"), env = fnBody(C.main, "void createWebViewEnv");
    const quiet = (t) => t.length > 100 && !/openLiveWindow|SetForegroundWindow|SW_RESTORE|MessageBox|DestroyWindow/.test(t);
    return quiet(rb) && quiet(rf)
      && has(rf, "g_rebuildRetry = true;") && has(rf, "::SetTimer(g_hwnd, kRebuildTimer, wait, nullptr);")
      && has(env, "if ((FAILED(r) || !env) && g_rebuilding) { rebuildFailed(); return S_OK; }")
      && has(env, "if ((FAILED(r2) || !controller) && g_rebuilding) { rebuildFailed(); return S_OK; }")
      && has(env, "if (FAILED(hr) && g_rebuilding) { rebuildFailed(); return; }")
      && ordered(cl, ["const unsigned gen = ++g_liveGen;", "if (gen != g_liveGen) { if (controller) controller->Close(); return S_OK; }", "g_liveCreating = false;"])
      && has(cl, "if (g_liveHwnd == hwnd && user && !g_quiet) ::MessageBoxW(")
      && has(fnBody(C.main, "LRESULT CALLBACK LiveWndProc"), "++g_liveGen;");
  },
  [{ file: "main", find: "                if ((FAILED(r) || !env) && g_rebuilding) { rebuildFailed(); return S_OK; }\n", replace: "", why: "a rebuild whose environment fails pops the startup's MessageBox and QUITS the app mid-meeting (RVB-4)" },
   { file: "main", find: "                if (gen != g_liveGen) {                       // stale: nothing of it is ours any more\n                    if (controller) controller->Close();\n                    return S_OK;\n                }\n", replace: "", why: "a stale creation callback resets the new creation's state and may pop a modal error (RVB-4)" },
   { file: "main", find: "if (g_liveHwnd == hwnd && user && !g_quiet)", replace: "if (g_liveHwnd == hwnd && !g_quiet)", why: "a recovery's failed creation pops a modal error out of nowhere (no user gesture, RVB-3)" },
   { file: "main", find: "    g_rebuildRetry = true;\n", replace: "", why: "a failed rebuild is never tried again (the windows stay dead)" }]);

pin("X10", "FIX ROUND -- the drain (the ADDENDUM)", "the LAST window closed while the engine is live: both windows hidden at once, LIVE 0, the app waits for STATE live 0 (liveDraining) 15 s at most, THEN ends (QUIT drops what is not finished); a relaunch meanwhile (an ACCEPTED WM_COPYDATA, bringUp, openLiveWindow) cancels the end -- a refused copy changes nothing; a second launch whose instance vanished waits for the mutex and becomes the app",
  (S, C) => {
    const q = fnBody(C.main, "void quitWhenDrained"), wp = fnBody(C.main, "LRESULT CALLBACK WndProc"), m = fnBody(C.main, "int WINAPI wWinMain");
    return ordered(q, ["hidePlayerWindow();", "::ShowWindow(g_liveHwnd, SW_HIDE);", "g_bridge->liveWindowClosed();", "if (!g_bridge || !g_bridge->liveDraining()) { ::DestroyWindow(g_hwnd); return; }",
                       "g_quitting = true;", "g_quitDeadline = ::GetTickCount64() + kQuitDrainMs;", "::SetTimer(g_hwnd, kQuitWaitTimer, 200, nullptr);"])
      && has(C.main, "constexpr ULONGLONG kQuitDrainMs = 15000;")
      && has(wp, "if (!g_bridge || !g_bridge->liveDraining() || ::GetTickCount64() >= g_quitDeadline) { ::KillTimer(hwnd, wParam); g_quitting = false; ::DestroyWindow(hwnd); }")
      && ordered(wp, ["if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;", "cancelQuit();", "kWinOpenLive",
                      "!pblaunch::unpackPlay(cds->lpData, cds->cbData, paths)) return FALSE;", "cancelQuit();", "g_pendingPlay.insert("])
      && has(fnBody(C.main, "void bringUp"), "cancelQuit();") && has(fnBody(C.main, "void openLiveWindow"), "cancelQuit();")
      && has(C.bridgeH, "bool liveDraining() const { return trLive_; }")
      && has(m, "if (handed == 0 || (existing && ::IsWindow(existing))) { ::CloseHandle(hSingle); return handed; }")
      && has(m, "const DWORD took = ::WaitForSingleObject(hSingle, 30000);");
  },
  [{ file: "main", find: "    if (!g_bridge || !g_bridge->liveDraining()) { ::DestroyWindow(g_hwnd); return; }\n", replace: "    ::DestroyWindow(g_hwnd); return;\n", why: "the last window closed while finishing: QUIT at once, the meeting's last sentences lost (RVA-1)" },
   { file: "main", find: "|| ::GetTickCount64() >= g_quitDeadline) {", replace: ") {", why: "an engine that never says live 0 keeps an invisible app alive for ever" },
   { file: "main", find: "            if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;\n            cancelQuit();\n", replace: "            if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;\n", why: "a --live relaunch during the drain's wait is taken, then the app ends under it (the user's window vanishes)" },
   { file: "main", find: "            if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;\n            cancelQuit();", replace: "            cancelQuit();\n            if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;", why: "a REFUSED copy cancels the end: an app with no window left alive, invisible" },
   { file: "main", find: "    if (::IsWindowVisible(g_hwnd)) hidePlayerWindow();\n", replace: "", why: "the player's window stays on screen while the app waits to end (the user's close not obeyed at once)" },
   { file: "main", find: "        const DWORD took = ::WaitForSingleObject(hSingle, 30000);\n        if (took != WAIT_OBJECT_0 && took != WAIT_ABANDONED) { ::CloseHandle(hSingle); return 2; }\n", replace: "        ::CloseHandle(hSingle); return 2;\n", why: "a relaunch that meets an instance ending is lost (nothing opens)" }]);

pin("X11", "FIX ROUND RVA-10", "a key holding '|' (the LANE separator) is refused by liveConfig -- as by TrPipe.h's unpackLive and the page: never a strip, never sent",
  (S, C) => has(fnBody(C.bridge, "void Bridge::handleLiveConfig"), "if (in.key.find('|') != std::string::npos) known = false;")
    && has(fnBody(C.pipe, "inline bool unpackLive"), "if (in.key.find('|') != std::string::npos) return false;"),
  [{ file: "bridge", find: "            if (in.key.find('|') != std::string::npos) known = false;\n", replace: "", why: "a key with '|' names two strips alike (\"in:X|1\" next to \"in:X\" channel 1)" }]);

pin("X12", "FIX ROUND -- the environment's generation (MEASURED: the real run's crash, 26/10)", "every browser-death notice and every creation callback carries the generation it belongs to: rebuildViews ignores a notice about views already let go (the OTHER view's notice of the same death), a stale environment / controller callback touches nothing (a controller it made is closed) -- two rebuilds never race (before: the bridge kept a released view, 0xC0000005 in Bridge::postToWeb)",
  (S, C) => {
    const env = fnBody(C.main, "void createWebViewEnv"), rb = fnBody(C.main, "void rebuildViews"), w = fnBody(C.main, "void watchProcessFailed");
    return has(env, "const unsigned egen = g_envGen;") && has(env, "if (egen != g_envGen) return S_OK;")
      && has(env, "if (egen != g_envGen) { if (controller) controller->Close(); return S_OK; }")
      && has(rb, "if (!g_hwnd || gen != g_envGen) return;") && ordered(rb, ["if (g_environment || g_webview) {", "++g_envGen;", "g_bridge->detachLiveView();"])
      && has(w, "[live, egen = g_envGen]") && has(w, "::PostMessageW(g_hwnd, kWmRebuildViews, egen, 0);")
      && has(fnBody(C.main, "LRESULT CALLBACK WndProc"), "case kWmRebuildViews: rebuildViews(static_cast<unsigned>(wParam)); return 0;")
      && has(fnBody(C.main, "void rebuildFailed"), "++g_envGen;");
  },
  [{ file: "main", find: "    if (!g_hwnd || gen != g_envGen) return;", replace: "    if (!g_hwnd) return;", why: "the other view's notice of the same death starts a SECOND rebuild while the first one's controller is on its way (the crash of run 1, 26/10)" },
   { file: "main", find: "        ++g_envGen;                                           // every creation", replace: "        //", why: "the dead generation's creations are not made stale" },
   { file: "main", find: "                            if (egen != g_envGen) {                  // stale (fix round)", replace: "                            if (false) {                  // stale (fix round)", why: "a stale controller replaces the live one under the bridge" },
   { file: "main", find: "                if (egen != g_envGen) return S_OK;   // stale", replace: "                // stale", why: "a stale environment callback sets the environment of a superseded rebuild" },
   { file: "main", find: "            [live, egen = g_envGen](ICoreWebView2* sender", replace: "            [live, egen = 0u](ICoreWebView2* sender", why: "the notices carry no generation: every one looks current" }]);

/*@@PINS-END@@*/

// ---- the runner --------------------------------------------------------------------------------------------------------------
function evalPin(p, S) {
  try { const r = p.test(S, codeOf(S)); return Array.isArray(r) ? { ok: !!r[0], extra: r[1] } : { ok: !!r }; }
  catch (e) { return { ok: false, extra: "threw: " + e.message }; }
}
// apply one mutant to a copy of the sources: it must apply EXACTLY once (a string, or a regular expression without the g flag)
function mutate(S, m) {
  const text = S[m.file];
  if (text == null) return { err: FILES[m.file] + " missing" };
  const n = typeof m.find === "string" ? text.split(m.find).length - 1 : (text.match(new RegExp(m.find.source, "g" + m.find.flags.replace("g", ""))) || []).length;
  if (n !== 1) return { err: "applies " + n + " times (must be exactly 1): " + String(m.find).slice(0, 90) };
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
console.log("test_host_pins_live: " + PINS.length + " pins over " + Object.keys(FILES).length + " sources of " + APP);
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
console.log((bad ? "TEST_HOST_PINS_LIVE FAILED" : "TEST_HOST_PINS_LIVE OK") + " -- " + pass + " pins passed, " + fail + " failed"
  + (argv.includes("--no-selftest") ? "" : "; " + mutPass + " mutants caught, " + mutFail + " not"));
process.exit(bad ? 1 : 0);

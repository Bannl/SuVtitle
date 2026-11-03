// =============================================================================
// SuVtitle - app/AppConfig.h   (NEW, S-HOST 2026-10-23: the app's own config file, %APPDATA%\SuVtitle\app.txt)
//
// SPLIT_PLAN S11: the window's geometry was the WINPOS line AudioPB's tray kept in translator.txt -- a file it shared
// with the helper, hence the read-modify-write dance and the "an empty read is not an empty file" guard in 641f1aa's
// main.cpp saveTranslatorWinPos. SuVtitle gives the app a file of its own: app.txt, written whole by the app only (the
// engine has engine.txt), through pbtrf::writeFileAvSafe (temp + rename with the antivirus retry).
//
//   # SuVtitle app v1
//   WINPOS <x> <y> <w> <h> <topmost 0|1> [<maximized 0|1>]      (tab-separated; the RESTORED rectangle, physical px)
//
// The line keeps AudioPB's five first fields in the same order (a reader that stops at topmost still reads it right);
// `maximized` is appended (new: AudioPB's translator window reopened restored even when it was closed maximized).
// Unknown lines are kept on a rewrite, so a later field never costs an older one.
// Also here, because it is pure and security-relevant: webRelPath, the rule that maps a request of the page's origin
// to a file under web\ (main.cpp serveFreshWebAsset). Pure: tests/cpp/app_smoke.cpp. ASCII only (no /utf-8).
//
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md section 3.4): the Live window's own two kinds of line,
//   LIVEPOS <x> <y> <w> <h> <topmost 0|1> [<maximized 0|1>]   the Live window's geometry (WINPOS's fields, same rules)
//   LIVEIN  <key> <label> <chan> <gainDb>                      one per strip, in strip order, kMaxLiveInputs at most
//   LIVESESSION <file name>                                    (fix round 26/10) the session's journal, until a Clear
// (key and label escaped with trpipe::esc: a label is the speaker's name, any character). A strip is checked by the
// SAME rules the engine applies to LIVE (trpipe::unpackLive, run on the set itself: liveSetNormal) -- a file edited by
// hand can never hand the engine a set it would refuse. Plus the engine.txt reader of the LIVE pair (the app mirrors it
// until the engine's first STATE) and clampWinPos (a saved window whose title bar is on no monitor comes back to one).
//
// U2 (2026-11-02, "Import model..."): the player's imported AI models, REFERENCED IN PLACE (never copied: they weigh GBs),
//   MODEL    <asr|mt> <path>      one per imported model of that kind, in import order (kMaxModelsPerKind at most)
//   MODELUSE <asr|mt> <path>      the one chosen for that kind (absent = SuVtitle's default model)
// (path escaped with trpipe::esc; asr = a whisper.cpp model, transcription; mt = a GGUF, translation -- the KIND was read
// from the file's header at the import, never from its name.) A path follows the FILE wire's own rule (trpipe::modelPathOk:
// absolute, no climbing, no stream); a line that breaks it, a duplicate (Windows compares names case-insensitively) or a
// MODELUSE naming no MODEL of its kind is skipped -- never the rest. Whether a file is still THERE is the bridge's to ask.
// U2b (2026-11-02, the user: remove an imported model from the menu, "et demande si on supprime le model en plus"):
// removeImportedModel takes a model OUT of its kind's list -- its MODEL line goes at the next packModels, and its MODELUSE too
// when it was the chosen one (that kind back to SuVtitle's default). The bridge names it by the LIST's own path (the page's id
// resolved against this list: never a path from the page); the FILE is the bridge's business (the Recycle Bin), never this one's.
//
// THE UPDATE BANNER (2026-11-03): the opt-out of the update check and "Skip this version" (bridge.cpp "THE UPDATE BANNER"):
//   UPDATECHECK 0              never ask www.nextale-audio.com whether a newer SuVtitle is published (absent, or any value
//                              but 0 / off / no / false: asked once per start)
//   UPDATESKIP  <version>      the version the user skipped: never offered again (a newer one is)
// (a tab OR spaces after the tag: the opt-out is meant to be typed by hand too; the first line of each tag counts)
// =============================================================================
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "TrPipe.h"   // SUVTITLE LIVE: trpipe::esc / unesc / split, WireLiveInput, packLive / unpackLive

namespace pbapp {

struct WinPos {
    int  x = 0, y = 0, w = 0, h = 0;
    bool topmost = false;
    bool maximized = false;
};

inline const char* appHeader() { return "# SuVtitle app v1"; }

inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string l = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

// False when there is no usable WINPOS (none, or a size no window could be: AudioPB's own floor, 200 x 150).
// SUVTITLE LIVE fix round (RVB-11): nor a PLACE no screen can be -- |x|, |y| beyond kWinPosMaxXY (2^20 px: a wall of
// screens is tens of thousands) is a garbled line, and near INT_MAX the callers' x + w would overflow (the title-bar test
// and the clamp then put the window nowhere, and it was saved there again).
constexpr int kWinPosMaxXY = 1 << 20;
inline bool parseWinPos(const std::string& text, WinPos& p) {
    for (const auto& l : lines(text)) {
        if (l.rfind("WINPOS\t", 0) != 0) continue;
        int x = 0, y = 0, w = 0, h = 0, tm = 0, mx = 0;
        const int n = ::sscanf_s(l.c_str() + 7, "%d\t%d\t%d\t%d\t%d\t%d", &x, &y, &w, &h, &tm, &mx);   // (_s: no C4996)
        if (n >= 4 && x > -kWinPosMaxXY && x < kWinPosMaxXY && y > -kWinPosMaxXY && y < kWinPosMaxXY
            && w > 200 && h > 150 && w < 100000 && h < 100000) {
            p.x = x; p.y = y; p.w = w; p.h = h;
            p.topmost = n >= 5 && tm != 0;
            p.maximized = n >= 6 && mx != 0;
            return true;
        }
    }
    return false;
}

// THE PAGE'S FILES (main.cpp serveFreshWebAsset): a request path under https://suvtitle.local/ -> a path RELATIVE to
// web\ ("" = refused: the handler declines and the request fails). AudioPB's handler kept the LAST segment only (its
// web\ was flat); SuVtitle's page may have sub-folders (S-WEB's call), so every segment is checked instead: letters,
// digits, '.', '-', '_' and spaces, 1-128 characters, never "." or ".." -- so no request can climb out of web\, name a
// drive, a device path, an alternate stream (':'), or reach anything but a file under web\. The query and the fragment
// are dropped; "" (the origin itself) is index.html.
inline std::wstring webRelPath(std::wstring path) {
    const size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path = path.substr(0, q);
    if (path.empty()) path = L"index.html";
    std::wstring rel;
    size_t at = 0;
    for (;;) {
        const size_t sl = path.find(L'/', at);
        const std::wstring seg = path.substr(at, sl == std::wstring::npos ? std::wstring::npos : sl - at);
        if (seg.empty() || seg == L"." || seg == L".." || seg.size() > 128) return std::wstring();
        // Win32 drops a trailing dot or space ("x." opens "x"): refused rather than reinterpreted
        if (seg.back() == L'.' || seg.back() == L' ') return std::wstring();
        for (wchar_t c : seg)
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
                  || c == L'.' || c == L'-' || c == L'_' || c == L' ')) return std::wstring();
        // a DOS device name in any folder is the device ("web\COM1", "web\NUL.txt"): never a file of ours
        {
            std::wstring b = seg.substr(0, seg.find(L'.'));
            for (auto& c : b) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
            while (!b.empty() && b.back() == L' ') b.pop_back();
            if (b == L"CON" || b == L"PRN" || b == L"AUX" || b == L"NUL" || b == L"CONIN$" || b == L"CONOUT$"
                || ((b.size() == 4) && (b.compare(0, 3, L"COM") == 0 || b.compare(0, 3, L"LPT") == 0)
                    && b[3] >= L'0' && b[3] <= L'9'))
                return std::wstring();
        }
        if (!rel.empty()) rel += L"\\";
        rel += seg;
        if (sl == std::wstring::npos) break;
        at = sl + 1;
    }
    return rel;
}

// The whole file: the header, the WINPOS line, and every other line `old` held (kept, in order; one header).
inline std::string packWinPos(const std::string& old, const WinPos& p) {
    char line[160];
    std::snprintf(line, sizeof(line), "WINPOS\t%d\t%d\t%d\t%d\t%d\t%d", p.x, p.y, p.w, p.h, p.topmost ? 1 : 0,
                  p.maximized ? 1 : 0);
    std::string body = std::string(appHeader()) + "\n" + line + "\n";
    for (const auto& l : lines(old)) {
        if (l[0] == '#' || l.rfind("WINPOS\t", 0) == 0) continue;
        body += l + "\n";
    }
    return body;
}

// =============================================================================
// SUVTITLE LIVE (1.1.0): LIVEPOS, LIVEIN, the live pair, the clamp
// =============================================================================

// The whole file with every `tag` line replaced by `with` (zero or more lines, without their newline): one header, the
// other lines kept in order, the new ones WHERE the first old one was (appended when there was none) -- so two writers
// of different lines (main.cpp's WINPOS / LIVEPOS, the bridge's LIVEIN) never reorder each other's, and a rewrite that
// changes nothing produces the same bytes (the callers write only when they differ).
inline std::string replaceTagged(const std::string& old, const char* tag, const std::vector<std::string>& with) {
    const std::string t = std::string(tag) + "\t";
    std::string body = std::string(appHeader()) + "\n";
    bool put = false;
    for (const auto& l : lines(old)) {
        if (l[0] == '#') continue;
        if (l.rfind(t, 0) == 0) {
            if (!put) { for (const auto& w : with) body += w + "\n"; put = true; }
            continue;
        }
        body += l + "\n";
    }
    if (!put) for (const auto& w : with) body += w + "\n";
    return body;
}

// LIVEPOS: WINPOS's reading rules exactly (the same fields, the same 200 x 150 floor) under its own tag.
inline bool parseLivePos(const std::string& text, WinPos& p) {
    std::string as;
    for (const auto& l : lines(text))
        if (l.rfind("LIVEPOS\t", 0) == 0) as += "WINPOS" + l.substr(7) + "\n";   // (parseWinPos takes the first usable one)
    return !as.empty() && parseWinPos(as, p);
}
inline std::string packLivePos(const std::string& old, const WinPos& p) {
    char line[160];
    std::snprintf(line, sizeof(line), "LIVEPOS\t%d\t%d\t%d\t%d\t%d\t%d", p.x, p.y, p.w, p.h, p.topmost ? 1 : 0,
                  p.maximized ? 1 : 0);
    return replaceTagged(old, "LIVEPOS", { line });
}

// A SET of strips checked -- and normalized (the gain to the 0.1 dB the wire carries, key and label as they arrive) --
// by the engine's own parser: packed into a LIVE 1 and read back by trpipe::unpackLive (the count, every field's range,
// the key's kind, "the same key twice only on two different channels >= 1"). Empty = a valid set (no strip yet). False =
// the engine would refuse it; `out` is then empty.
inline bool liveSetNormal(const std::vector<pbt::trpipe::WireLiveInput>& in, std::vector<pbt::trpipe::WireLiveInput>& out) {
    out.clear();
    if (in.empty()) return true;
    pbt::trpipe::WireLive l, back;
    l.on = true;
    l.inputs = in;
    if (!pbt::trpipe::unpackLive(pbt::trpipe::split(pbt::trpipe::packLive(l)), back)) return false;
    out = back.inputs;
    return true;
}

// The strips app.txt holds, in order. A line is taken only if the set WITH it is still one the engine accepts: a line
// edited by hand into a duplicate, a fifth strip or a 99 dB trim is skipped -- never the whole set.
inline std::vector<pbt::trpipe::WireLiveInput> parseLiveInputs(const std::string& text) {
    std::vector<pbt::trpipe::WireLiveInput> set;
    for (const auto& l : lines(text)) {
        if (l.rfind("LIVEIN\t", 0) != 0) continue;
        const std::vector<std::string> f = pbt::trpipe::split(l);
        if (f.size() != 5 || f[3].size() != 1 || f[3][0] < '0' || f[3][0] > '9' || f[4].empty()) continue;
        pbt::trpipe::WireLiveInput in;
        in.key = pbt::trpipe::unesc(f[1]);
        in.label = pbt::trpipe::unesc(f[2]);
        in.chan = static_cast<uint32_t>(f[3][0] - '0');
        char* end = nullptr;
        in.gainDb = std::strtod(f[4].c_str(), &end);
        if (!end || *end != '\0') continue;
        std::vector<pbt::trpipe::WireLiveInput> with = set, norm;
        with.push_back(in);
        if (liveSetNormal(with, norm)) set = norm;
    }
    return set;
}
// The LIVEIN lines replaced by `set` (checked by the caller: liveSetNormal).
inline std::string packLiveInputs(const std::string& old, const std::vector<pbt::trpipe::WireLiveInput>& set) {
    std::vector<std::string> with;
    for (const auto& in : set)
        with.push_back("LIVEIN\t" + pbt::trpipe::esc(in.key) + "\t" + pbt::trpipe::esc(in.label) + "\t"
                       + std::to_string(in.chan) + "\t" + pbt::trpipe::fmtGainDb(in.gainDb));
    return replaceTagged(old, "LIVEIN", with);
}

// LIVESESSION <file name> (fix round, RVB-1 / the ADDENDUM FIX ROUND): the live session's journal, in %APPDATA%\SuVtitle\
// Live sessions\ -- the session survives a restart until a Clear (which removes the line). A FILE NAME only, of the shape
// the bridge makes ("2026-10-26_143005.jsonl", "_<n>" before the extension when that second was taken): digits, '-' and
// '_', then ".jsonl" -- never a path, a drive, a stream or a dot more, so a line edited by hand can name nothing but a
// journal of that folder. "" = no session to take back (none, or not that shape).
inline bool liveSessionNameOk(const std::string& n) {
    if (n.size() <= 6 || n.size() > 40 || n.compare(n.size() - 6, 6, ".jsonl") != 0) return false;
    for (size_t i = 0; i + 6 < n.size(); ++i) {
        const char c = n[i];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}
inline std::string parseLiveSession(const std::string& text) {
    for (const auto& l : lines(text)) {
        if (l.rfind("LIVESESSION\t", 0) != 0) continue;
        const std::string n = l.substr(12);
        return liveSessionNameOk(n) ? n : std::string();
    }
    return std::string();
}
// The LIVESESSION line replaced ("" = removed: a Clear ends the session).
inline std::string packLiveSession(const std::string& old, const std::string& name) {
    std::vector<std::string> with;
    if (liveSessionNameOk(name)) with.push_back("LIVESESSION\t" + name);
    return replaceTagged(old, "LIVESESSION", with);
}

// The LIVE pair as the ENGINE persists it (engine.txt LIVELANGA / LIVELANGB / LIVEMODE -- the engine's file, READ only):
// the twin of TrPlayer.h pairFromTranslatorCfg for the player's pair. The codes are the caller's to normalize (bridge.cpp
// trLangCode); the mode is migrated and checked the same way. False = none of the three lines is there.
inline bool livePairFromEngineCfg(const std::string& text, std::string& a, std::string& b, std::string& mode) {
    bool any = false;
    for (const auto& l : lines(text)) {
        if (l[0] == '#') continue;
        const size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        const std::string tag = l.substr(0, tab), val = l.substr(tab + 1);
        if (tag == "LIVELANGA")      { a = val; any = true; }
        else if (tag == "LIVELANGB") { b = val; any = true; }
        else if (tag == "LIVEMODE")  { mode = val; any = true; }
    }
    if (mode == "en2zh") mode = "a2b";
    else if (mode == "zh2en") mode = "b2a";
    if (mode != "auto" && mode != "a2b" && mode != "b2a") mode = "auto";
    return any;
}

// =============================================================================
// U2: the imported models (MODEL / MODELUSE)
// =============================================================================
constexpr size_t kMaxModelsPerKind = 16;
struct ImportedModels {
    std::vector<std::string> asr, mt;      // UTF-8 absolute paths, import order
    std::string useAsr, useMt;             // the chosen one of each kind ("" = the default)
    std::vector<std::string>& list(const std::string& kind) { return kind == "mt" ? mt : asr; }
    const std::vector<std::string>& list(const std::string& kind) const { return kind == "mt" ? mt : asr; }
    std::string& use(const std::string& kind) { return kind == "mt" ? useMt : useAsr; }
    const std::string& use(const std::string& kind) const { return kind == "mt" ? useMt : useAsr; }
};
// Windows compares file names without case: two spellings of one path are ONE model (ASCII letters folded; enough for the
// duplicates a hand edit or a second import makes).
inline bool samePathCi(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}
inline bool listHasPath(const std::vector<std::string>& v, const std::string& p) {
    for (const auto& x : v) if (samePathCi(x, p)) return true;
    return false;
}
// Add `path` to its kind's list (true = it is there now: added, or already listed). False = refused: not a path the wire
// takes, or the list is full.
inline bool addImportedModel(ImportedModels& m, const std::string& kind, const std::string& path) {
    if ((kind != "asr" && kind != "mt") || !pbt::trpipe::modelPathOk(path)) return false;
    std::vector<std::string>& v = m.list(kind);
    if (listHasPath(v, path)) return true;
    if (v.size() >= kMaxModelsPerKind) return false;
    v.push_back(path);
    return true;
}
inline ImportedModels parseModels(const std::string& text) {
    ImportedModels m;
    std::vector<std::pair<std::string, std::string>> uses;
    for (const auto& l : lines(text)) {
        const bool model = l.rfind("MODEL\t", 0) == 0, use = l.rfind("MODELUSE\t", 0) == 0;
        if (!model && !use) continue;
        const std::vector<std::string> f = pbt::trpipe::split(l);
        if (f.size() != 3 || (f[1] != "asr" && f[1] != "mt")) continue;
        const std::string path = pbt::trpipe::unesc(f[2]);
        if (model) addImportedModel(m, f[1], path);
        else uses.emplace_back(f[1], path);
    }
    for (const auto& u : uses)                                   // (after every MODEL line: their order is free)
        if (pbt::trpipe::modelPathOk(u.second) && listHasPath(m.list(u.first), u.second)) m.use(u.first) = u.second;
    return m;
}
// U2b: `path` (the list's own spelling, or another case of it) out of `kind`'s list -- and that kind's choice back to the default
// when it named it. True = it was listed (gone now); false = not a kind word, or not listed (nothing changed).
inline bool removeImportedModel(ImportedModels& m, const std::string& kind, const std::string& path) {
    if (kind != "asr" && kind != "mt") return false;
    std::vector<std::string>& v = m.list(kind);
    bool gone = false;
    for (auto it = v.begin(); it != v.end();) {
        if (samePathCi(*it, path)) { it = v.erase(it); gone = true; }
        else ++it;
    }
    if (gone && samePathCi(m.use(kind), path)) m.use(kind).clear();
    return gone;
}
// The MODEL and MODELUSE lines replaced by `m` (where the first old one of each tag was; the other lines kept, in order).
inline std::string packModels(const std::string& old, const ImportedModels& m) {
    std::vector<std::string> models, uses;
    for (const char* kind : { "asr", "mt" }) {
        for (const auto& p : m.list(kind)) models.push_back(std::string("MODEL\t") + kind + "\t" + pbt::trpipe::esc(p));
        if (!m.use(kind).empty()) uses.push_back(std::string("MODELUSE\t") + kind + "\t" + pbt::trpipe::esc(m.use(kind)));
    }
    return replaceTagged(replaceTagged(old, "MODEL", models), "MODELUSE", uses);
}

// =============================================================================
// THE UPDATE BANNER (2026-11-03): UPDATECHECK / UPDATESKIP
// =============================================================================
struct UpdatePrefs {
    bool        check = true;   // ask the site once per start (the default: no line)
    std::string skip;           // the skipped version ("" = none)
};
// A version as UPDATESKIP keeps it: 1-32 characters of [0-9A-Za-z.-] (the release tool's alphabet; the bridge compares it to the
// manifest's, which NextaleUpdate's versionOk checked to the letter).
inline bool updateVersionToken(const std::string& v) {
    if (v.empty() || v.size() > 32) return false;
    for (const char c : v)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
    return true;
}
// `l` is a `tag` line: the tag, then a tab or spaces, then its value (trimmed). False = another line ("UPDATECHECKS 0" too).
inline bool updatePrefLine(const std::string& l, const char* tag, std::string& val) {
    const std::string t(tag);
    if (l.size() <= t.size() || l.compare(0, t.size(), t) != 0 || (l[t.size()] != '\t' && l[t.size()] != ' ')) return false;
    size_t a = t.size(), b = l.size();
    while (a < b && (l[a] == '\t' || l[a] == ' ')) ++a;
    while (b > a && (l[b - 1] == '\t' || l[b - 1] == ' ')) --b;
    val = l.substr(a, b - a);
    return true;
}
inline UpdatePrefs parseUpdatePrefs(const std::string& text) {
    UpdatePrefs p;
    bool seenCheck = false, seenSkip = false;
    for (const auto& l : lines(text)) {
        std::string v;
        if (!seenCheck && updatePrefLine(l, "UPDATECHECK", v)) {
            seenCheck = true;
            for (auto& c : v) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            p.check = !(v == "0" || v == "off" || v == "no" || v == "false");
        } else if (!seenSkip && updatePrefLine(l, "UPDATESKIP", v)) {
            seenSkip = true;
            if (updateVersionToken(v)) p.skip = v;
        }
    }
    return p;
}
// Every UPDATECHECK / UPDATESKIP line (tab or spaces: a hand-typed one too -- replaceTagged knows the tab only, and a line it
// left would come back at the next start) replaced by `p`'s, where the first of them was: "UPDATECHECK\t0" only when the check
// is OFF (on = no line), "UPDATESKIP\t<v>" only for a version. Every other line kept as it is, in order; one header.
inline std::string packUpdatePrefs(const std::string& old, const UpdatePrefs& p) {
    std::vector<std::string> with;
    if (!p.check) with.push_back("UPDATECHECK\t0");
    if (updateVersionToken(p.skip)) with.push_back("UPDATESKIP\t" + p.skip);
    std::string body = std::string(appHeader()) + "\n";
    bool put = false;
    for (const auto& l : lines(old)) {
        if (l[0] == '#') continue;
        std::string v;
        if (updatePrefLine(l, "UPDATECHECK", v) || updatePrefLine(l, "UPDATESKIP", v)) {
            if (!put) { for (const auto& w : with) body += w + "\n"; put = true; }
            continue;
        }
        body += l + "\n";
    }
    if (!put) for (const auto& w : with) body += w + "\n";
    return body;
}

// A saved window brought back onto a monitor's work area [l, r) x [t, b): shrunk to fit it (never below WINPOS's 200 x 150
// floor), then moved inside it. main.cpp calls it when the saved rectangle's TITLE BAR is on no monitor (a second screen
// unplugged, a resolution changed): a window the user cannot grab is a window he cannot use -- and the Live window is the
// one he puts on the other screen.
inline WinPos clampWinPos(WinPos p, int l, int t, int r, int b) {
    const int ww = r - l, wh = b - t;
    if (ww <= 0 || wh <= 0) return p;
    if (p.w > ww) p.w = ww > 201 ? ww : 201;
    if (p.h > wh) p.h = wh > 151 ? wh : 151;
    if (p.x + p.w > r) p.x = r - p.w;
    if (p.y + p.h > b) p.y = b - p.h;
    if (p.x < l) p.x = l;
    if (p.y < t) p.y = t;
    return p;
}

}   // namespace pbapp

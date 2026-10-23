// =============================================================================
// SuVtitle - app/SubtitleReader.h   (external subtitle files: encoding detection + SRT / WebVTT / ASS)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/SubtitleReader.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: "AudioPB" -> "SuVtitle" in its two error sentences.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// The subtitle files the PLAYER reads: a downloaded .srt sitting next to a film, a .vtt, an
// .ass/.ssa (read as plain text: no positioning, no karaoke). SubtitleWriter.h's other half, and
// like it header-only and proved by a smoke test (scratchpad/ui_test/subreader_smoke.cpp) -- the
// bridge only reads the file and hands the bytes in. Parsed in the TRAY, not in the page: the
// page never sees a path, and the encoding detection below needs Windows itself.
//
// The formats are the easy half. THE ENCODING IS THE HARD HALF: a subtitle file carries no
// declaration, and the ones the user meets most (Chinese, downloaded) come as GBK or Big5 as often
// as UTF-8. Read with the wrong code page they turn into mojibake on screen. The chain, in order:
//   1. a BOM (EF BB BF / FF FE / FE FF) -- the file says it;
//   2. UTF-16 WITHOUT a BOM: the timestamps make a subtitle file mostly ASCII, so a NUL byte on
//      every other position is unmistakable. Before UTF-8 on purpose: NUL is valid UTF-8;
//   3. STRICT UTF-8 (no overlong form, no surrogate, nothing cut short; pure ASCII passes);
//   4. MLang's IMultiLanguage2::DetectInputCodepage (Windows' own detector), every candidate
//      checked by decoding with MB_ERR_INVALID_CHARS -- and when it names GBK or Big5, a
//      byte-level tie-break (cjkGuess) settles which: MLang is weakest exactly there;
//   5. the system ANSI code page, then Windows-1252 as the last resort.
// The CC panel can force any code page of encodingTable() when the guess is wrong.
//
// NOTE: THIS FILE IS ASCII. The build does not pass /utf-8 (see DocxWriter.cpp's header): every
// non-ASCII character below is a numeric escape.
// =============================================================================
#pragma once

#include <windows.h>
#include <mlang.h>     // IMultiLanguage2 -- __uuidof only, so no extra import library

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pbsubr {

// ---------------------------------------------------------------------------
// What a parse returns. A cue is a list of RUNS: text with its style (the page builds one span
// per run, by textContent -- no markup ever crosses into the page). "\n" in a run's text is a
// line break.
// ---------------------------------------------------------------------------
enum : uint8_t { kItalic = 1, kBold = 2, kUnderline = 4 };
struct Run { std::string text; uint8_t flags = 0; };          // UTF-8
struct Cue { uint64_t startMs = 0, endMs = 0; std::vector<Run> runs; };
enum class Format { Unknown, Srt, Vtt, Ass };

struct Result {
    bool        ok = false;
    Format      format = Format::Unknown;
    unsigned    codepage = 0;        // 65001, 1200 (UTF-16 LE), 1201 (UTF-16 BE), else a Windows code page
    bool        bom = false;
    bool        forced = false;      // the code page was imposed by the caller (the CC panel)
    std::vector<Cue> cues;           // sorted by start; end > start
    size_t      fixed = 0;           // cues whose end was not after their start (given a 2 s end)
    size_t      skipped = 0;         // blocks with a broken timing or no text
    bool        truncated = false;   // more than kMaxCues: the rest was dropped
    // WG (review #3): the file holds something these cues do NOT keep -- a tag other than <i> <b> <u> (<font>, <c.x>,
    // <v>, <ruby>, timestamps), a {\...} block, WebVTT cue settings / ids / NOTE STYLE REGION blocks / header lines, a
    // line that is neither an index nor text. Written back from these cues, the file would lose it: the editor then
    // saves its edits to a COPY and leaves the file untouched.
    bool        dropped = false;
    std::string err;                 // "" or one sentence for a human
};

constexpr size_t   kMaxBytes = 20u << 20;     // the bridge refuses bigger files before reading them
constexpr size_t   kMaxCues  = 20000;
constexpr unsigned kCpUtf8 = 65001, kCpUtf16LE = 1200, kCpUtf16BE = 1201;

// The code pages the CC panel offers to FORCE, with their names (translator.html keeps the same
// numbers -- the harness compares the two lists).
struct EncName { unsigned cp; const char* name; };
inline const EncName* encodingTable() {
    static const EncName k[] = {
        { 65001, "UTF-8" }, { 1200, "UTF-16 LE" }, { 1201, "UTF-16 BE" },
        { 936, "Chinese Simplified (GBK)" }, { 950, "Chinese Traditional (Big5)" },
        { 932, "Japanese (Shift-JIS)" }, { 949, "Korean (EUC-KR)" },
        { 1252, "Western (Windows-1252)" }, { 1250, "Central European (Windows-1250)" },
        { 1251, "Cyrillic (Windows-1251)" }, { 1253, "Greek (Windows-1253)" },
        { 1254, "Turkish (Windows-1254)" }, { 1255, "Hebrew (Windows-1255)" },
        { 1256, "Arabic (Windows-1256)" }, { 1257, "Baltic (Windows-1257)" },
        { 874, "Thai (Windows-874)" }, { 1258, "Vietnamese (Windows-1258)" },
        { 20866, "Cyrillic (KOI8-R)" }, { 54936, "Chinese (GB18030)" }, { 0, nullptr } };
    return k;
}
inline std::string encodingName(unsigned cp) {
    for (const EncName* e = encodingTable(); e->name; ++e) if (e->cp == cp) return e->name;
    char b[32];
    std::snprintf(b, sizeof b, "Code page %u", cp);
    return b;
}
inline bool isOfferedEncoding(unsigned cp) {
    for (const EncName* e = encodingTable(); e->name; ++e) if (e->cp == cp) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Encoding detection
// ---------------------------------------------------------------------------
// Strict UTF-8 from `from`: well-formed sequences only (RFC 3629 table), nothing cut short.
inline bool isStrictUtf8(const std::string& s, size_t from = 0) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = from;
    while (i < n) {
        const unsigned c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t need = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF)      need = 1;
        else if (c == 0xE0)              { need = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) need = 2;
        else if (c == 0xED)              { need = 2; hi = 0x9F; }      // no UTF-16 surrogates
        else if (c >= 0xEE && c <= 0xEF) need = 2;
        else if (c == 0xF0)              { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4)              { need = 3; hi = 0x8F; }      // nothing past U+10FFFF
        else return false;
        if (n - i - 1 < need) return false;
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k <= need; ++k) if ((p[i + k] & 0xC0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

// UTF-8 with a STRAY byte or two -- an editor's slip, a paste cut inside a character: when at least
// 99.5 % of the multibyte sequences are well-formed, it IS UTF-8, and the bad bytes become U+FFFD
// (parse() decodes leniently when the strict decode fails). One bad byte used to send the whole file
// to the fallback code page, every accented letter turned to mojibake (review of WE, P9).
inline bool mostlyUtf8(const std::string& s, size_t from = 0) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t good = 0, bad = 0;
    for (size_t i = from; i < n; ) {
        const unsigned c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t need = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF)      need = 1;
        else if (c == 0xE0)              { need = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) need = 2;
        else if (c == 0xED)              { need = 2; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) need = 2;
        else if (c == 0xF0)              { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4)              { need = 3; hi = 0x8F; }
        bool ok = need > 0 && n - i - 1 >= need && p[i + 1] >= lo && p[i + 1] <= hi;
        for (size_t k = 2; ok && k <= need; ++k) if ((p[i + k] & 0xC0) != 0x80) ok = false;
        if (ok) { ++good; i += need + 1; } else { ++bad; ++i; }
    }
    return good > 0 && bad * 200 <= good + bad;   // >= 99.5 % well-formed
}

// UTF-16 WITHOUT a BOM, from the NUL bytes of the (mostly ASCII) timestamps: 0 = no, 1 = LE, 2 = BE.
inline int utf16Guess(const std::string& s, size_t from = 0) {
    const size_t avail = s.size() > from ? s.size() - from : 0;
    const size_t len = std::min<size_t>(avail, 4096) & ~static_cast<size_t>(1);
    const size_t pairs = len / 2;
    if (pairs < 8) return 0;
    size_t zEven = 0, zOdd = 0;
    for (size_t i = 0; i < len; ++i)
        if (s[from + i] == '\0') { if (i & 1) ++zOdd; else ++zEven; }
    if (zOdd * 10 >= pairs * 4 && zEven * 20 <= pairs) return 1;   // "1\0" -> LE
    if (zEven * 10 >= pairs * 4 && zOdd * 20 <= pairs) return 2;   // "\01" -> BE
    return 0;
}

// Bytes -> UTF-16 in `cp`. strict = MB_ERR_INVALID_CHARS (a byte sequence the code page does not
// define fails the whole decode -- which is what makes a wrong guess DETECTABLE).
inline bool decodeTo(const std::string& s, size_t from, unsigned cp, bool strict, std::wstring& out) {
    out.clear();
    const size_t n = s.size() > from ? s.size() - from : 0;
    if (n == 0) return true;
    if (cp == kCpUtf16LE || cp == kCpUtf16BE) {
        if ((n & 1) && strict) return false;
        out.resize(n / 2);
        for (size_t i = 0; i + 1 < n; i += 2) {
            const unsigned char a = static_cast<unsigned char>(s[from + i]), b = static_cast<unsigned char>(s[from + i + 1]);
            out[i / 2] = static_cast<wchar_t>(cp == kCpUtf16LE ? (a | (b << 8)) : (b | (a << 8)));
        }
        return true;
    }
    if (n > static_cast<size_t>(0x7FFFFFFF)) return false;
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int w = ::MultiByteToWideChar(cp, flags, s.data() + from, static_cast<int>(n), nullptr, 0);
    if (w <= 0) return false;
    out.resize(static_cast<size_t>(w));
    return ::MultiByteToWideChar(cp, flags, s.data() + from, static_cast<int>(n), &out[0], w) == w;
}
inline bool decodes(const std::string& s, size_t from, unsigned cp) {
    std::wstring w;
    return decodeTo(s, from, cp, true, w);
}

// The CJK double-byte tie-break. Among byte PAIRS whose first byte is >= 0x81:
//   * Shift-JIS puts the kana on leads 0x82 (hiragana) / 0x83 (katakana) -- Japanese text is full of them;
//   * GB2312, the set Simplified Chinese subtitles live in, has BOTH bytes in A1-FE: almost no trail byte < 0x80;
//   * Big5 puts about two characters in five on trail bytes 40-7E.
// Text in a single-byte code page (French accents, say) makes "pairs" of an accented letter and the
// ASCII after it; real double-byte text makes mostly HIGH-HIGH pairs -- under 40 % of those, no guess.
// 0 = not double-byte text, or not enough of it to say.
inline unsigned cjkGuess(const std::string& s, size_t from = 0) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t pairs = 0, lowTrail = 0, kana = 0;
    for (size_t i = from; i + 1 < n; ) {
        const unsigned c = p[i];
        if (c < 0x81) { ++i; continue; }
        ++pairs;
        if (p[i + 1] < 0x80) ++lowTrail;
        if (c == 0x82 || c == 0x83) ++kana;
        i += 2;
    }
    if (pairs < 4 || (pairs - lowTrail) * 10 < pairs * 4) return 0;
    if (kana * 10 >= pairs * 3) return 932;
    return lowTrail * 100 >= pairs * 15 ? 950u : 936u;
}

// Does the text look like REAL Chinese in that code page? Ordinary text lives in the frequent rows:
// GB2312's level 1 (leads B0-D7) and its punctuation/full-width rows (A1-A3); Big5's frequent block
// (A4-C6) and its symbol rows (A1-A3). Cyrillic or Greek bytes read as GBK land all over C0-FE
// instead. 60 % of the pairs in those rows = plausible. (932: the kana test in cjkGuess suffices.)
inline bool cjkPlausible(const std::string& s, unsigned cp, size_t from = 0) {
    if (cp == 932) return true;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t pairs = 0, frequent = 0;
    for (size_t i = from; i + 1 < s.size(); ) {
        const unsigned c = p[i];
        if (c < 0x81) { ++i; continue; }
        ++pairs;
        if (cp == 936 && ((c >= 0xB0 && c <= 0xD7) || (c >= 0xA1 && c <= 0xA3))) ++frequent;
        if (cp == 950 && c >= 0xA1 && c <= 0xC6) ++frequent;
        i += 2;
    }
    return pairs >= 4 && frequent * 10 >= pairs * 6;
}
// A single-byte code page (one byte, one character): MLang falls back to one of these on SHORT
// Chinese text, which every one of them "decodes".
inline bool singleByteCp(unsigned cp) {
    return (cp >= 1250 && cp <= 1258) || cp == 874 || (cp >= 28591 && cp <= 28605) || cp == 20866 || cp == 21866
        || cp == 20127 || cp == 437 || cp == 850;
}

// MLang's candidates, most confident first (empty when MLang is unavailable -- no COM on this
// thread, say -- or has no opinion). At most 256 KB is shown to it.
inline std::vector<unsigned> mlangCandidates(const std::string& s, size_t from) {
    std::vector<unsigned> out;
    if (s.size() <= from) return out;
    IMultiLanguage2* ml = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CMultiLanguage), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMultiLanguage2), reinterpret_cast<void**>(&ml))) || !ml)
        return out;
    INT len = static_cast<INT>(std::min<size_t>(s.size() - from, 256u * 1024u));
    DetectEncodingInfo info[8] = {};
    INT count = 8;
    std::vector<char> buf(s.data() + from, s.data() + from + len);   // the API takes a non-const pointer
    if (SUCCEEDED(ml->DetectInputCodepage(MLDETECTCP_NONE, 0, buf.data(), &len, info, &count)) && count > 0) {
        if (count > 8) count = 8;
        std::stable_sort(info, info + count, [](const DetectEncodingInfo& a, const DetectEncodingInfo& b) {
            return a.nConfidence > b.nConfidence;
        });
        for (INT i = 0; i < count; ++i) {
            const unsigned cp = info[i].nCodePage;
            if (std::find(out.begin(), out.end(), cp) == out.end()) out.push_back(cp);
        }
    }
    ml->Release();
    return out;
}

// The whole chain. `from` comes back past a BOM; `how` (optional) says which step decided.
inline unsigned detectCodepage(const std::string& s, size_t& from, bool& bom, std::string* how = nullptr) {
    from = 0;
    bom = false;
    auto said = [&](const char* w) { if (how) *how = w; };
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    if (s.size() >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { from = 3; bom = true; said("bom"); return kCpUtf8; }
    if (s.size() >= 2 && p[0] == 0xFF && p[1] == 0xFE) { from = 2; bom = true; said("bom"); return kCpUtf16LE; }
    if (s.size() >= 2 && p[0] == 0xFE && p[1] == 0xFF) { from = 2; bom = true; said("bom"); return kCpUtf16BE; }
    if (const int u16 = utf16Guess(s, 0)) { said("utf16-nul"); return u16 == 1 ? kCpUtf16LE : kCpUtf16BE; }
    if (isStrictUtf8(s, 0)) { said("utf8"); return kCpUtf8; }
    if (mostlyUtf8(s, 0)) { said("utf8-lenient"); return kCpUtf8; }
    const std::vector<unsigned> cands = mlangCandidates(s, 0);
    const unsigned guess = cjkGuess(s, 0);
    const unsigned top = cands.empty() ? 0u : cands.front();
    // MLang's weak spots: it names GBK for Big5 text and back, calls Chinese "Japanese", and on a
    // SHORT Chinese file falls back to a single-byte code page. The bytes decide when its top answer
    // is GBK/Big5, Japanese while the bytes carry no kana, a single-byte page while the bytes look
    // like real Chinese -- or when it has no answer at all (no COM). Never against Korean: hangul
    // sits in the same rows as GB2312's frequent characters, and MLang tells those apart, not us.
    const bool chineseTop = top == 936 || top == 950 || top == 54936 || top == 20936 || (top == 932 && guess != 932);
    const bool weakTop = top == 0 || singleByteCp(top);
    if (guess && (chineseTop || (weakTop && cjkPlausible(s, guess))) && decodes(s, 0, guess)) {
        said(top ? "mlang+cjk" : "cjk");
        return guess;
    }
    for (unsigned cp : cands) {
        if (cp == kCpUtf8 || cp == kCpUtf16LE || cp == kCpUtf16BE || cp == 20127 || cp == 0) continue;
        if (decodes(s, 0, cp)) { said("mlang"); return cp; }
    }
    if (guess && cjkPlausible(s, guess) && decodes(s, 0, guess)) { said("cjk"); return guess; }
    const unsigned acp = ::GetACP();
    if (acp != kCpUtf8 && decodes(s, 0, acp)) { said("acp"); return acp; }
    said("fallback");
    return 1252;
}

inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Text helpers (UTF-8 in, UTF-8 out)
// ---------------------------------------------------------------------------
inline char lowerA(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool startsWithI(const std::string& s, size_t at, const char* w) {
    for (size_t k = 0; w[k]; ++k) if (at + k >= s.size() || lowerA(s[at + k]) != lowerA(w[k])) return false;
    return true;
}
inline std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
inline std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '\r') { out.push_back(cur); cur.clear(); if (i + 1 < t.size() && t[i + 1] == '\n') ++i; }
        else if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}
inline void appendCodepoint(std::string& o, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                             o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
           o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// "01:02:03,456", "1:02:03.4", "02:03.456" (no hours: WebVTT allows it, broken SRTs do it) -> ms.
// `at` moves past the stamp. The fraction is a FRACTION: ",5" is 500 ms, ",45" 450 ms.
inline bool parseStamp(const std::string& t, size_t& at, uint64_t& ms) {
    size_t i = at;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    uint64_t g[3] = {0, 0, 0};
    int groups = 0;
    for (;;) {
        size_t d = 0;
        uint64_t v = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9' && d < 6) { v = v * 10 + static_cast<uint64_t>(t[i] - '0'); ++i; ++d; }
        if (d == 0) return false;
        g[groups++] = v;
        if (groups < 3 && i < t.size() && t[i] == ':') { ++i; continue; }
        break;
    }
    if (groups < 2) return false;
    uint64_t frac = 0;
    if (i < t.size() && (t[i] == ',' || t[i] == '.')) {
        ++i;
        uint64_t scale = 100;
        size_t d = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            if (d < 3) { frac += static_cast<uint64_t>(t[i] - '0') * scale; scale /= 10; }
            ++i; ++d;
        }
        if (d == 0) return false;
    }
    const uint64_t h = groups == 3 ? g[0] : 0, m = groups == 3 ? g[1] : g[0], s = groups == 3 ? g[2] : g[1];
    if (m >= 60 || s >= 60) return false;
    ms = ((h * 60 + m) * 60 + s) * 1000 + frac;
    at = i;
    return true;
}
// "<stamp> --> <stamp> [anything]" -> start/end.
inline bool parseTiming(const std::string& line, uint64_t& a, uint64_t& b) {
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    size_t i = 0;
    if (!parseStamp(line, i, a)) return false;
    while (i < arrow && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i != arrow) return false;
    i = arrow + 3;
    return parseStamp(line, i, b);
}

// ---------------------------------------------------------------------------
// Cue text -> runs. SRT/VTT tags: <i> <b> <u> become flags; every other <tag> goes (<font>,
// <c.x>, <v Name>, <lang>, <ruby>, timestamps); {\...} blocks go (ASS codes in SRTs). A '<' that
// does not open a tag is text. VTT also decodes its entities. ASS: {\i1}{\b1}{\u1} and their 0s,
// {\r} resets, \N \n line breaks, \h a non-breaking space, drawing mode ({\p1}) dropped.
// ---------------------------------------------------------------------------
struct RunBuilder {
    std::vector<Run> runs;
    uint8_t flags = 0;
    void put(const std::string& txt) {
        if (txt.empty()) return;
        if (!runs.empty() && runs.back().flags == flags) runs.back().text += txt;
        else { Run r; r.text = txt; r.flags = flags; runs.push_back(r); }
    }
    void set(uint8_t bit, bool on) { flags = on ? static_cast<uint8_t>(flags | bit) : static_cast<uint8_t>(flags & ~bit); }
};
// The text is clean: no control character but '\n', no trailing blanks on a line, no empty line at
// either end, no empty run.
inline std::vector<Run> tidyRuns(std::vector<Run> in) {
    std::vector<Run> out;
    for (Run& r : in) {
        std::string t;
        for (char c : r.text) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (c == '\t') t += ' ';
            else if (u < 0x20 && c != '\n') continue;
            else t += c;
        }
        r.text = t;
        if (!r.text.empty()) {
            if (!out.empty() && out.back().flags == r.flags) out.back().text += r.text;
            else out.push_back(r);
        }
    }
    // trailing blanks before each '\n' and at the very end; leading/trailing empty lines
    for (Run& r : out) {
        std::string t;
        for (size_t i = 0; i < r.text.size(); ++i) {
            if (r.text[i] == '\n') { while (!t.empty() && t.back() == ' ') t.pop_back(); }
            t += r.text[i];
        }
        r.text = t;
    }
    while (!out.empty()) {
        std::string& f = out.front().text;
        size_t k = 0;
        while (k < f.size() && (f[k] == '\n' || f[k] == ' ')) ++k;
        f.erase(0, k);
        if (f.empty()) out.erase(out.begin()); else break;
    }
    while (!out.empty()) {
        std::string& b = out.back().text;
        while (!b.empty() && (b.back() == '\n' || b.back() == ' ')) b.pop_back();
        if (b.empty()) out.pop_back(); else break;
    }
    return out;
}
inline bool runsEmpty(const std::vector<Run>& r) {
    for (const Run& x : r) for (char c : x.text) if (c != ' ' && c != '\n') return false;
    return true;
}

inline void decodeEntity(const std::string& t, size_t& i, std::string& o) {
    // at '&'; on no match the '&' stays text
    static const struct { const char* name; unsigned long cp; } k[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
        { "nbsp;", 0xA0 }, { "lrm;", 0x200E }, { "rlm;", 0x200F } };
    for (const auto& e : k) {
        if (startsWithI(t, i + 1, e.name)) { appendCodepoint(o, e.cp); i += 1 + std::strlen(e.name); return; }
    }
    if (i + 2 < t.size() && t[i + 1] == '#') {
        size_t j = i + 2;
        const bool hex = j < t.size() && (t[j] == 'x' || t[j] == 'X');
        if (hex) ++j;
        unsigned long v = 0;
        size_t d = 0;
        while (j < t.size() && d < 8) {
            const char c = t[j];
            int x = -1;
            if (c >= '0' && c <= '9') x = c - '0';
            else if (hex && c >= 'a' && c <= 'f') x = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') x = c - 'A' + 10;
            if (x < 0) break;
            v = v * (hex ? 16 : 10) + static_cast<unsigned long>(x);
            ++j; ++d;
        }
        if (d > 0 && j < t.size() && t[j] == ';') { appendCodepoint(o, v); i = j + 1; return; }
    }
    o += '&';
    ++i;
}

// The next `ch` at or after `from`, REMEMBERED: a run of openers with no closer scans the text once,
// not once per opener (review of WE, C7: one cue of 400 000 '<' took 1.08 s -- quadratic -- and a
// 20 MB file ~45 min of frozen tray: subtitle files are parsed on the UI thread). Callers ask with a
// `from` that never goes backwards, so each byte is looked at once.
struct NextOf {
    const std::string& s;
    char ch;
    size_t from0 = 0, at = std::string::npos;
    bool known = false;
    NextOf(const std::string& str, char c) : s(str), ch(c) {}
    size_t operator()(size_t from) {
        if (!known || from < from0 || (at != std::string::npos && from > at)) { at = s.find(ch, from); from0 = from; known = true; }
        return at;
    }
};

// `dropped` (optional): set when something is left out that the runs cannot carry (any tag but i/b/u, a {\...} block)
inline std::vector<Run> markupRuns(const std::string& text, bool vtt, bool* dropped = nullptr) {
    RunBuilder rb;
    std::string cur;
    auto flush = [&] { rb.put(cur); cur.clear(); };
    NextOf nextGt(text, '>'), nextBrace(text, '}');
    for (size_t i = 0; i < text.size(); ) {
        const char c = text[i];
        if (c == '<') {
            // a TAG only when it closes soon and looks like one: [/]name[.classes][ annotation]
            const size_t close = nextGt(i + 1);
            if (close != std::string::npos && close - i <= 96) {
                const std::string tag = text.substr(i + 1, close - i - 1);
                bool plausible = !tag.empty();
                for (char ch : tag) if (ch == '<' || ch == '\n') { plausible = false; break; }
                const char first = tag.empty() ? ' ' : tag[0];
                // A digit opens a tag only in WebVTT, and only as a timestamp (<00:01.000>): in an SRT,
                // "I <3 you >_<" is text.
                bool stamp = vtt && first >= '0' && first <= '9';
                for (char ch : tag) if (!((ch >= '0' && ch <= '9') || ch == ':' || ch == '.')) { stamp = false; break; }
                if (plausible && (stamp || first == '/' || (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) {
                    std::string name;
                    size_t k = (first == '/') ? 1 : 0;
                    while (k < tag.size() && tag[k] != ' ' && tag[k] != '.' && tag[k] != '\t') name += lowerA(tag[k++]);
                    const bool closing = first == '/';
                    uint8_t bit = 0;
                    if (name == "i") bit = kItalic; else if (name == "b") bit = kBold; else if (name == "u") bit = kUnderline;
                    // (an i/b/u tag with anything after its name -- <i class=x> -- loses that part too)
                    if (dropped && (!bit || tag.size() != name.size() + (closing ? 1u : 0u))) *dropped = true;
                    flush();
                    if (bit) rb.set(bit, !closing);
                    i = close + 1;
                    continue;
                }
            }
            cur += c; ++i; continue;
        }
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '\\') {   // an ASS override block inside an SRT
            const size_t close = nextBrace(i + 2);
            if (close != std::string::npos) { if (dropped) *dropped = true; i = close + 1; continue; }
        }
        if (vtt && c == '&') { decodeEntity(text, i, cur); continue; }
        cur += c; ++i;
    }
    flush();
    return tidyRuns(rb.runs);
}

inline std::vector<Run> assRuns(const std::string& text) {
    RunBuilder rb;
    std::string cur;
    bool drawing = false;
    auto flush = [&] { rb.put(cur); cur.clear(); };
    NextOf nextBrace(text, '}');
    for (size_t i = 0; i < text.size(); ) {
        const char c = text[i];
        if (c == '{') {
            const size_t close = nextBrace(i + 1);
            if (close == std::string::npos) { cur += c; ++i; continue; }
            flush();
            const std::string blk = text.substr(i + 1, close - i - 1);
            for (size_t k = 0; k < blk.size(); ++k) {
                if (blk[k] != '\\') continue;
                const size_t a = k + 1;
                auto num = [&](size_t from, long& v) {
                    size_t j = from; v = 0; size_t d = 0;
                    while (j < blk.size() && blk[j] >= '0' && blk[j] <= '9' && d < 5) { v = v * 10 + (blk[j] - '0'); ++j; ++d; }
                    return d > 0;
                };
                long v = 0;
                if (a < blk.size() && blk[a] == 'i' && num(a + 1, v)) rb.set(kItalic, v != 0);
                else if (a < blk.size() && blk[a] == 'b' && (a + 1 >= blk.size() || (blk[a + 1] >= '0' && blk[a + 1] <= '9')) && num(a + 1, v))
                    rb.set(kBold, v == 1 || v >= 600);
                else if (a < blk.size() && blk[a] == 'u' && num(a + 1, v)) rb.set(kUnderline, v != 0);
                else if (a < blk.size() && blk[a] == 'p' && num(a + 1, v)) drawing = v != 0;
                else if (a < blk.size() && blk[a] == 'r') rb.flags = 0;   // \r or \rStyle: back to plain
            }
            i = close + 1;
            continue;
        }
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == 'N' || n == 'n') { if (!drawing) cur += '\n'; i += 2; continue; }
            if (n == 'h') { if (!drawing) appendCodepoint(cur, 0xA0); i += 2; continue; }
        }
        if (!drawing) cur += c;
        ++i;
    }
    flush();
    return tidyRuns(rb.runs);
}

// ---------------------------------------------------------------------------
// The formats
// ---------------------------------------------------------------------------
inline Format sniff(const std::string& t) {
    size_t i = 0;
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB
        && static_cast<unsigned char>(t[2]) == 0xBF) i = 3;                     // a BOM left in the text
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
    if (startsWithI(t, i, "WEBVTT")) return Format::Vtt;
    for (size_t k = 0; k + 7 < t.size() && k < 200000; ++k) {
        if (t[k] != '[') continue;
        if (startsWithI(t, k, "[script info]") || startsWithI(t, k, "[events]")) return Format::Ass;
    }
    if (t.find("-->") != std::string::npos) return Format::Srt;
    return Format::Unknown;
}

inline bool allDigits(const std::string& s) {
    const std::string t = trimmed(s);
    if (t.empty()) return false;
    for (char c : t) if (c < '0' || c > '9') return false;
    return true;
}

// A WebVTT block that is not a cue: NOTE (a comment), STYLE, REGION -- the keyword alone on its
// line or followed by a blank (the keywords are case-sensitive in the spec), and no timing in it.
inline bool vttMetaBlock(const std::string& ln) {
    static const char* const k[] = { "NOTE", "STYLE", "REGION" };
    for (const char* w : k) {
        const size_t n = std::strlen(w);
        if (ln.compare(0, n, w) == 0 && (ln.size() == n || ln[n] == ' ' || ln[n] == '\t'))
            return ln.find("-->") == std::string::npos;
    }
    return false;
}

// SRT and WebVTT share their skeleton: a timing line, then text lines up to a blank line.
inline void parseTimed(const std::string& text, bool vtt, Result& r) {
    const std::vector<std::string> lines = splitLines(text);
    size_t i = 0;
    if (vtt) {                                   // the header block: WEBVTT and its metadata lines
        const size_t h0 = i;
        while (i < lines.size() && !trimmed(lines[i]).empty()) ++i;
        // (WG: "WEBVTT" alone is all the writer puts back -- a title after it, or metadata lines, would go)
        if (i > h0) {
            std::string h = trimmed(lines[h0]);
            if (h.compare(0, 3, "\xEF\xBB\xBF") == 0) h.erase(0, 3);
            if (i > h0 + 1 || h != "WEBVTT") r.dropped = true;
        }
    }
    uint64_t a = 0, b = 0;
    while (i < lines.size()) {
        const std::string ln = trimmed(lines[i]);
        if (vtt && vttMetaBlock(ln)) {
            r.dropped = true;                                              // a NOTE / STYLE / REGION block
            while (i < lines.size() && !trimmed(lines[i]).empty()) ++i;   // the whole block
            continue;
        }
        if (ln.find("-->") == std::string::npos) {                         // an index, an id, garbage
            if (!ln.empty() && (vtt || !allDigits(ln))) r.dropped = true;  // (an SRT index is written again; an id is not)
            ++i;
            continue;
        }
        if (!parseTiming(ln, a, b)) { ++r.skipped; ++i; continue; }
        {   // WG: anything after the end time (WebVTT cue settings "align:start line:0", SRT "X1:...") is not kept
            const size_t arrow = ln.find("-->");
            size_t k = arrow + 3;
            while (k < ln.size() && (ln[k] == ' ' || ln[k] == '\t')) ++k;
            while (k < ln.size() && ln[k] != ' ' && ln[k] != '\t') ++k;     // the end time
            while (k < ln.size() && (ln[k] == ' ' || ln[k] == '\t')) ++k;
            if (k < ln.size()) r.dropped = true;
        }
        ++i;
        std::string body;
        while (i < lines.size()) {
            const std::string& t = lines[i];
            if (trimmed(t).empty()) break;
            if (t.find("-->") != std::string::npos) break;                // the next cue, no blank between
            if (!vtt && allDigits(t) && i + 1 < lines.size() && lines[i + 1].find("-->") != std::string::npos) break;
            if (!body.empty()) body += '\n';
            body += t;
            ++i;
        }
        Cue c;
        c.startMs = a;
        c.endMs = b;
        c.runs = markupRuns(body, vtt, &r.dropped);
        if (runsEmpty(c.runs)) { ++r.skipped; continue; }
        r.cues.push_back(c);
    }
}

inline void parseAss(const std::string& text, Result& r) {
    const std::vector<std::string> lines = splitLines(text);
    bool inEvents = false;
    std::vector<std::string> cols;           // lower-cased Format: names
    int iStart = 1, iEnd = 2, iText = 9;     // the [V4+ Styles] default order, until a Format: says otherwise
    for (const std::string& raw : lines) {
        const std::string ln = trimmed(raw);
        if (ln.empty()) continue;
        if (ln[0] == '[') { inEvents = startsWithI(ln, 0, "[events]"); continue; }
        if (!inEvents) continue;
        if (startsWithI(ln, 0, "Format:")) {
            cols.clear();
            std::string cur;
            for (size_t k = 7; k <= ln.size(); ++k) {
                if (k == ln.size() || ln[k] == ',') { std::string n = trimmed(cur); for (char& ch : n) ch = lowerA(ch); cols.push_back(n); cur.clear(); }
                else cur += ln[k];
            }
            iStart = iEnd = iText = -1;
            for (size_t k = 0; k < cols.size(); ++k) {
                if (cols[k] == "start") iStart = static_cast<int>(k);
                else if (cols[k] == "end") iEnd = static_cast<int>(k);
                else if (cols[k] == "text") iText = static_cast<int>(k);
            }
            if (iText < 0) iText = static_cast<int>(cols.size()) - 1;
            continue;
        }
        if (!startsWithI(ln, 0, "Dialogue:")) continue;
        if (iStart < 0 || iEnd < 0 || iText < 0) { ++r.skipped; continue; }
        // split into the column count; the TEXT is the last column and keeps its commas
        const size_t ncol = cols.empty() ? 10u : cols.size();
        std::vector<std::string> f;
        size_t k = 9;
        std::string cur;
        for (; k < ln.size(); ++k) {
            if (ln[k] == ',' && f.size() + 1 < ncol) { f.push_back(cur); cur.clear(); }
            else cur += ln[k];
        }
        f.push_back(cur);
        if (static_cast<int>(f.size()) <= std::max(iText, std::max(iStart, iEnd))) { ++r.skipped; continue; }
        uint64_t a = 0, b = 0;
        size_t p0 = 0, p1 = 0;
        const std::string sa = trimmed(f[static_cast<size_t>(iStart)]), sb = trimmed(f[static_cast<size_t>(iEnd)]);
        if (!parseStamp(sa, p0, a) || !parseStamp(sb, p1, b)) { ++r.skipped; continue; }
        Cue c;
        c.startMs = a;
        c.endMs = b;
        c.runs = assRuns(f[static_cast<size_t>(iText)]);
        if (runsEmpty(c.runs)) { ++r.skipped; continue; }
        r.cues.push_back(c);
    }
}

// THE ENTRY POINT. `bytes` = the whole file (<= kMaxBytes). forcedCp != 0 = the CC panel's choice.
inline Result parse(const std::string& bytes, unsigned forcedCp = 0) {
    Result r;
    if (bytes.size() > kMaxBytes) { r.err = "This subtitle file is too large (over 20 MB)."; return r; }
    size_t from = 0;
    bool bom = false;
    unsigned cp = 0;
    if (forcedCp) {
        cp = forcedCp;
        r.forced = true;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
        if (cp == kCpUtf8 && bytes.size() >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { from = 3; bom = true; }
        if ((cp == kCpUtf16LE && bytes.size() >= 2 && p[0] == 0xFF && p[1] == 0xFE)
            || (cp == kCpUtf16BE && bytes.size() >= 2 && p[0] == 0xFE && p[1] == 0xFF)) { from = 2; bom = true; }
    } else {
        cp = detectCodepage(bytes, from, bom);
    }
    r.codepage = cp;
    r.bom = bom;
    std::wstring w;
    if (!decodeTo(bytes, from, cp, true, w) && !decodeTo(bytes, from, cp, false, w)) {
        r.err = "This subtitle file could not be read as " + encodingName(cp) + ".";
        return r;
    }
    const std::string text = toUtf8(w);
    r.format = sniff(text);
    if (r.format == Format::Vtt || r.format == Format::Srt) parseTimed(text, r.format == Format::Vtt, r);
    else if (r.format == Format::Ass) parseAss(text, r);
    else { r.err = "This is not a subtitle file SuVtitle can read (.srt, .vtt, .ass, .ssa)."; return r; }
    std::stable_sort(r.cues.begin(), r.cues.end(), [](const Cue& x, const Cue& y) { return x.startMs < y.startMs; });
    for (Cue& c : r.cues) if (c.endMs <= c.startMs) { c.endMs = c.startMs + 2000; ++r.fixed; }
    if (r.cues.size() > kMaxCues) { r.cues.resize(kMaxCues); r.truncated = true; }
    if (r.cues.empty()) { r.err = "No subtitles were found in this file."; return r; }
    r.ok = true;
    return r;
}

// WH (2026-10-11, additive): a document SuVtitle SYNTHESIZED itself from a subtitle track INSIDE a video
// (MediaTracks.h extract: SRT / ASS / WebVTT rebuilt from Matroska blocks or MP4 samples). Its format and code page
// are KNOWN: nothing is sniffed -- a cue whose text says "[Events]" must not turn a synthesized SRT into an ASS, as
// sniff() would -- and nothing is detected. The rest is parse() exactly (lenient decode, sort, fixes, caps).
inline Result parseAs(const std::string& bytes, unsigned cp, Format fmt) {
    Result r;
    if (bytes.size() > kMaxBytes) { r.err = "This subtitle track is too large (over 20 MB)."; return r; }
    r.codepage = cp;
    r.forced = true;
    size_t from = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    if (cp == kCpUtf8 && bytes.size() >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { from = 3; r.bom = true; }
    std::wstring w;
    if (!decodeTo(bytes, from, cp, true, w) && !decodeTo(bytes, from, cp, false, w)) {
        r.err = "This subtitle track could not be read as " + encodingName(cp) + ".";
        return r;
    }
    const std::string text = toUtf8(w);
    r.format = fmt;
    if (fmt == Format::Vtt || fmt == Format::Srt) parseTimed(text, fmt == Format::Vtt, r);
    else if (fmt == Format::Ass) parseAss(text, r);
    else { r.err = "This is not a subtitle track SuVtitle can read."; return r; }
    std::stable_sort(r.cues.begin(), r.cues.end(), [](const Cue& x, const Cue& y) { return x.startMs < y.startMs; });
    for (Cue& c : r.cues) if (c.endMs <= c.startMs) { c.endMs = c.startMs + 2000; ++r.fixed; }
    if (r.cues.size() > kMaxCues) { r.cues.resize(kMaxCues); r.truncated = true; }
    if (r.cues.empty()) { r.err = "No subtitles were found in this track."; return r; }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// Sidecars: the subtitle files that belong to a medium by NAME, as every player finds them --
// "<base>.srt", "<base>.en.srt", "<base>.English.forced.srt" (any middle part), for each of the four
// extensions. `hint` gets the middle part ("" for "<base>.srt"). Case-insensitive.
// ---------------------------------------------------------------------------
inline bool isSubtitleExt(const std::wstring& ext) {
    std::wstring e = ext;
    for (wchar_t& ch : e) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    return e == L"srt" || e == L"vtt" || e == L"ass" || e == L"ssa";
}
inline bool isSidecarFor(const std::wstring& mediaLeaf, const std::wstring& candLeaf, std::wstring& hint) {
    hint.clear();
    auto lower = [](std::wstring s) { for (wchar_t& ch : s) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a'); return s; };
    const size_t md = mediaLeaf.find_last_of(L'.');
    const std::wstring base = lower((md == std::wstring::npos || md == 0) ? mediaLeaf : mediaLeaf.substr(0, md));
    const std::wstring cand = lower(candLeaf);
    const size_t cd = cand.find_last_of(L'.');
    if (cd == std::wstring::npos || cd == 0 || !isSubtitleExt(cand.substr(cd + 1))) return false;
    const std::wstring stem = cand.substr(0, cd);
    if (stem == base) return true;
    if (stem.size() > base.size() + 1 && stem.compare(0, base.size(), base) == 0 && stem[base.size()] == L'.') {
        hint = candLeaf.substr(base.size() + 1, cd - base.size() - 1);   // original case
        return true;
    }
    return false;
}

}   // namespace pbsubr

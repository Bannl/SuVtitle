// =============================================================================
// SuVtitle - app/SubtitleWriter.h   (.srt / .vtt writer, pure)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/SubtitleWriter.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: nothing but this header.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// The subtitle FILES the Translator writes for a recording: SubRip (.srt) -- the one every
// player on earth loads by itself when it sits next to the video with the same base name --
// and WebVTT (.vtt). Header-only and PURE on purpose: no <windows.h>, no bridge, no wire
// types, UTF-8 in and bytes out, so the whole format (timestamps, line breaking, escaping)
// is proved by a 300-line smoke test (scratchpad/ui_test/subwriter_smoke.cpp) and adding
// it needed no CMake change.
//
// What a subtitle file has to get right, and where each rule lives below:
//   * the TIMESTAMP: "HH:MM:SS,mmm" for SRT, a full stop for VTT, hours UNBOUNDED (a lecture
//     recording runs past 99 h never, but past 9 h quite often) .................. stamp()
//   * the LINE BREAKS: a subtitle is two balanced lines, not one 90-character line and not a
//     long line over a stub. Latin breaks at spaces; CJK breaks between characters, never
//     before a closing mark nor after an opening one (kinsoku) ................... wrap()
//   * the TEXT: no control characters, no blank line INSIDE a cue (a blank line ENDS a cue in
//     both formats -- one stray "\n\n" would cut a subtitle in half and shift every number
//     after it), no "-->" in SRT text (a parser may read it as a timing line), and & < >
//     escaped in VTT (its cue text is markup) ........................... cleanText(), escVtt()
//   * the ENCODING: UTF-8 with a BOM and CRLF. The BOM is what makes the older Windows players
//     read a Chinese subtitle as UTF-8 instead of the ANSI code page; CRLF is what Notepad and
//     every SRT editor write.
//
// NOTE: THIS FILE IS ASCII. The build does not pass /utf-8 (see DocxWriter.cpp's header for the
// whole story): every non-ASCII character below is a numeric code point, never a typed glyph.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pbsub {

// One finished subtitle, as the Translator's file job produced it (a subset of the wire's
// FCUE, kept free of the wire types so this header compiles on its own).
struct Cue {
    uint64_t    startMs = 0, endMs = 0;   // MEDIA time
    std::string lang, dstLang;            // what was spoken / what dst is in
    bool        pass = false;             // dst merely repeats src (already in the target)
    std::string src, dst;                 // UTF-8
};

// Which text a file carries: the translation, the original, or both (translation first, the
// original under it in italics -- the bilingual layout language learners ask for).
enum class Which { Dst, Src, Both };

// ---------------------------------------------------------------------------
// UTF-8 <-> code points. Invalid input becomes U+FFFD rather than being dropped silently:
// a replacement character in a subtitle is visible and reportable, a vanished letter is not.
// ---------------------------------------------------------------------------
inline std::u32string decodeUtf8(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        char32_t cp = 0;
        int extra = 0;
        if      (c < 0x80)           { cp = c;        extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        bool ok = true;   // a sequence cut short by the end of the string fails here too
        for (int k = 1; k <= extra; ++k) {
            if (i + (size_t)k >= s.size()) { ok = false; break; }
            const unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        // Overlong forms, surrogates and values past U+10FFFF are not characters.
        const bool overlong = (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800)
                           || (extra == 3 && cp < 0x10000);
        if (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        out.push_back(cp);
        i += (size_t)extra + 1;
    }
    return out;
}

inline std::string encodeUtf8(const std::u32string& u) {
    std::string o;
    o.reserve(u.size() + 8);
    for (char32_t c : u) {
        if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) continue;   // not encodable
        if (c < 0x80) {
            o += (char)c;
        } else if (c < 0x800) {
            o += (char)(0xC0 | (c >> 6));
            o += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += (char)(0xE0 | (c >> 12));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        } else {
            o += (char)(0xF0 | (c >> 18));
            o += (char)(0x80 | ((c >> 12) & 0x3F));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Character classes for line breaking.
// ---------------------------------------------------------------------------
// East Asian WIDE / full-width: one of these takes the room of two Latin letters on screen,
// which is what every subtitle guideline counts (Netflix: 42 Latin characters a line, 16 for
// Chinese/Japanese -- i.e. the same width).
inline bool isWide(char32_t c) {
    return (c >= 0x1100 && c <= 0x115F)       // Hangul Jamo (leading consonants)
        || (c >= 0x2E80 && c <= 0x303E)       // CJK radicals .. CJK symbols and punctuation
        || (c >= 0x3041 && c <= 0x33FF)       // kana, Bopomofo, Hangul compat, enclosed CJK
        || (c >= 0x3400 && c <= 0x4DBF)       // CJK extension A
        || (c >= 0x4E00 && c <= 0x9FFF)       // CJK unified ideographs
        || (c >= 0xA000 && c <= 0xA4CF)       // Yi
        || (c >= 0xAC00 && c <= 0xD7A3)       // Hangul syllables
        || (c >= 0xF900 && c <= 0xFAFF)       // CJK compatibility ideographs
        || (c >= 0xFE10 && c <= 0xFE19)       // vertical forms
        || (c >= 0xFE30 && c <= 0xFE6F)       // CJK compatibility + small forms
        || (c >= 0xFF00 && c <= 0xFF60)       // full-width forms
        || (c >= 0xFFE0 && c <= 0xFFE6)       // full-width signs
        || (c >= 0x20000 && c <= 0x2FFFD)     // CJK extensions B..F, compatibility supplement
        || (c >= 0x30000 && c <= 0x3FFFD);    // CJK extension G..
}

// KINSOKU: what a line may not START with -- closing marks and the punctuation that belongs to
// the word before it. A line beginning with the full stop of the previous one is the classic
// CJK subtitling error, and the one a Taiwanese or Chinese reader notices first.
inline bool noLineStart(char32_t c) {
    switch (c) {
        case ',': case '.': case '!': case '?': case ':': case ';': case ')': case ']': case '}':
        case '%':
        case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0x30FB:   // ideographic , . full-width , . katakana middle dot
        case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF01: case 0x30FC:   // full-width : ; ? ! and the long-vowel mark
        case 0x300D: case 0x300F: case 0xFF09: case 0x3011: case 0x3015:   // closing corner brackets, paren, lenticular, tortoise
        case 0x3009: case 0x300B: case 0x3019: case 0x3017: case 0x301F:   // closing angle, double angle, white brackets, quote
        case 0x2019: case 0x201D: case 0xFF5D: case 0x301C: case 0x2026:   // right quotes, full-width }, wave dash, ellipsis
        case 0x2025: case 0x3005: case 0xFF05:                             // two-dot leader, iteration mark, full-width %
        case 0x00BB: case 0x203A:                                          // closing guillemets (French)
            return true;
        default:
            return false;
    }
}
// ...and what a line may not END with: the opening marks, which belong to what follows.
inline bool noLineEnd(char32_t c) {
    switch (c) {
        case '(': case '[': case '{':
        case 0x00AB: case 0x2039:                                          // opening guillemets (French)
        case 0x300C: case 0x300E: case 0xFF08: case 0x3010: case 0x3014:   // opening corner brackets, paren, lenticular, tortoise
        case 0x3008: case 0x300A: case 0x3018: case 0x3016: case 0x301D:   // opening angle, double angle, white brackets, quote
        case 0x2018: case 0x201C: case 0xFF5B:                             // left quotes, full-width {
            return true;
        default:
            return false;
    }
}
// A break right AFTER one of these reads naturally (it ends a clause), so it is preferred when
// the balance it costs is small.
inline bool isClauseEnd(char32_t c) {
    switch (c) {
        case ',': case '.': case '!': case '?': case ':': case ';':
        case 0x3001: case 0x3002: case 0xFF0C: case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF01:
        case 0x2026:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// One line of plain text, fit to be a subtitle: control characters out (XML-style controls
// are not merely ugly in a subtitle file, a stray \n in one of them ends the cue), every line
// break / tab / line separator a SPACE (the writer re-breaks the text itself), runs of spaces
// collapsed, ends trimmed.
// ---------------------------------------------------------------------------
inline std::string cleanText(const std::string& s) {
    const std::u32string u = decodeUtf8(s);
    std::u32string o;
    o.reserve(u.size());
    for (char32_t c : u) {
        if (c == '\t' || c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029 || c == 0x85) c = ' ';
        else if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) continue;
        if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
        o.push_back(c);
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return encodeUtf8(o);
}

namespace detail {

inline int widthOf(const std::u32string& u, size_t from, size_t to) {
    int w = 0;
    for (size_t i = from; i < to && i < u.size(); ++i) w += isWide(u[i]) ? 2 : 1;
    return w;
}

// The span [from, to) with spaces trimmed at both ends.
inline void trimSpan(const std::u32string& u, size_t& from, size_t& to) {
    while (from < to && u[from] == ' ') ++from;
    while (to > from && u[to - 1] == ' ') --to;
}

struct Break { size_t leftEnd = 0, rightBegin = 0; bool clause = false; };

// Every place [from, to) may be broken: at a SPACE (the space disappears), or BETWEEN two
// characters when at least one of them is wide (CJK has no spaces to break at) and kinsoku
// allows it. Never inside a run of non-wide characters: that would split a Latin word.
// Kinsoku holds at a space too: French sets a space BEFORE ? ! : ; and inside guillemets
// ("le realisateur ? Il..."), and a break there would open line 2 with the "?".
inline std::vector<Break> breaksIn(const std::u32string& u, size_t from, size_t to) {
    std::vector<Break> out;
    for (size_t i = from + 1; i < to; ++i) {
        if (u[i] == ' ') {
            size_t l = i, r = i;
            while (l > from && u[l - 1] == ' ') --l;
            while (r < to && u[r] == ' ') ++r;
            if (l > from && r < to && u[i - 1] != ' ' && !noLineStart(u[r]) && !noLineEnd(u[l - 1])) {
                Break b; b.leftEnd = l; b.rightBegin = r; b.clause = isClauseEnd(u[l - 1]);
                out.push_back(b);
            }
            continue;
        }
        if (u[i - 1] == ' ') continue;   // handled as a space break
        if (!(isWide(u[i - 1]) || isWide(u[i]))) continue;
        if (noLineStart(u[i]) || noLineEnd(u[i - 1])) continue;
        Break b; b.leftEnd = i; b.rightBegin = i; b.clause = isClauseEnd(u[i - 1]);
        out.push_back(b);
    }
    return out;
}

// Split [from, to) into `n` lines whose widths are as even as possible: the first break is
// chosen so the first line comes closest to an n-th of the remaining width (for n == 2 that
// is "minimise the longer line"), then the rest is split into n-1 the same way.
inline void splitInto(const std::u32string& u, size_t from, size_t to, int n, int limit,
                      std::vector<std::u32string>& lines) {
    trimSpan(u, from, to);
    if (from >= to) return;
    const int total = widthOf(u, from, to);
    if (n <= 1 || total <= limit) { lines.push_back(u.substr(from, to - from)); return; }
    const std::vector<Break> bs = breaksIn(u, from, to);
    if (bs.empty()) { lines.push_back(u.substr(from, to - from)); return; }   // one long word
    // A clause end is worth this much width: breaking after "Hello everyone," instead of one
    // word later reads better than a perfect balance does.
    const int kClauseBonus = 3;
    const double target = (double)total / (double)n;
    double bestScore = 1e18;
    Break best = bs.front();
    bool bestFits = false;
    for (const Break& b : bs) {
        const int wl = widthOf(u, from, b.leftEnd);
        const int wr = widthOf(u, b.rightBegin, to);
        // n == 2: the longer of the two lines; n > 2: how far line 1 is from its share.
        double score = (n == 2) ? (double)(wl > wr ? wl : wr)
                                : (wl > target ? wl - target : target - wl);
        if (b.clause) score -= kClauseBonus;
        const bool fits = wl <= limit && (n > 2 || wr <= limit);
        // A break that keeps every line inside the limit always beats one that does not.
        if ((fits && !bestFits) || (fits == bestFits && score < bestScore)) {
            bestScore = score; best = b; bestFits = fits;
        }
    }
    lines.push_back(u.substr(from, best.leftEnd - from));
    splitInto(u, best.rightBegin, to, n - 1, limit, lines);
}

}   // namespace detail

// ---------------------------------------------------------------------------
// The text of one cue, broken into subtitle lines. One line when it fits; otherwise two lines
// as even as the text allows; three or more only when the text is longer than two full lines.
// The limit is 42 half-width columns, or 32 (= 16 full-width characters) when at least half
// of the glyphs are wide -- the two numbers every broadcaster's guide gives, and the same
// screen width.
// ---------------------------------------------------------------------------
inline std::vector<std::string> wrap(const std::string& text, int latinMax = 42, int cjkMax = 32) {
    const std::u32string u = decodeUtf8(cleanText(text));
    std::vector<std::string> out;
    if (u.empty()) return out;
    int glyphs = 0, wide = 0;
    for (char32_t c : u) {
        if (c == ' ') continue;
        ++glyphs;
        if (isWide(c)) ++wide;
    }
    const int limit = (glyphs > 0 && wide * 2 >= glyphs) ? cjkMax : latinMax;
    const int total = detail::widthOf(u, 0, u.size());
    const int n = (total <= limit) ? 1 : (total + limit - 1) / limit;   // >= 2 when it is over
    std::vector<std::u32string> lines;
    detail::splitInto(u, 0, u.size(), n, limit, lines);
    for (const auto& l : lines) out.push_back(encodeUtf8(l));
    return out;
}

// The lines of one cue text: the writer's breaks -- and the USER's. A "\n" in the text is a line break somebody chose
// (the subtitle editor's Shift+Enter, wave WG): each part is wrapped on its own and the break stays. An analysis' text
// holds none, so its lines are exactly wrap()'s (review of WG #14: the typed breaks used to become spaces).
inline std::vector<std::string> wrapKeepingBreaks(const std::string& text, int latinMax = 42, int cjkMax = 32) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t nl = text.find('\n', at);
        for (auto& l : wrap(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at), latinMax, cjkMax))
            out.push_back(std::move(l));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

// "HH:MM:SS<sep>mmm" -- hours at least two digits and never capped.
inline std::string stamp(uint64_t ms, char sep) {
    const unsigned long long h = ms / 3600000ULL;
    const unsigned m = (unsigned)((ms / 60000ULL) % 60ULL);
    const unsigned s = (unsigned)((ms / 1000ULL) % 60ULL);
    const unsigned f = (unsigned)(ms % 1000ULL);
    char b[48];
    std::snprintf(b, sizeof b, "%02llu:%02u:%02u%c%03u", h, m, s, sep, f);
    return b;
}

// WebVTT cue text is MARKUP: & and < must be escaped or a sentence about "A<B" opens a tag,
// and > is escaped too so that no "-->" can ever appear inside a cue.
inline std::string escVtt(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if      (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else               o += c;
    }
    return o;
}

// SubRip has no escaping at all, so the one sequence a parser could mistake for a timing line
// is defused instead -- until NONE is left: shortening "--->" once re-forms "-->" one character
// to the left, so the search resumes there, not after the replacement.
inline std::string srtSafe(const std::string& s) {
    std::string o = s;
    size_t at = 0;
    while ((at = o.find("-->", at)) != std::string::npos) {
        o.replace(at, 3, "->");
        at = at > 0 ? at - 1 : 0;
    }
    return o;
}

// A language code as file names and players want it: "zh-tw" -> "zh-TW" (the region upper
// case, BCP 47 style -- Plex, Jellyfin and VLC all read it), a plain code lower case.
inline std::string langTag(const std::string& code) {
    std::string o;
    bool region = false;
    for (char c : code) {
        if (c == '-' || c == '_') { o += '-'; region = true; continue; }
        if (region) o += (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        else        o += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    return o;
}

// The two texts a cue contributes for `w`: `main` always, `sub` (the original under the
// translation) only for Both, and never when it would print the same sentence twice.
inline void textsFor(const Cue& c, Which w, std::string& main, std::string& sub) {
    const std::string dst = c.dst.empty() ? c.src : c.dst;
    sub.clear();
    if (w == Which::Src) { main = c.src.empty() ? dst : c.src; return; }
    main = dst;
    if (w == Which::Both && !c.pass && !c.src.empty() && cleanText(c.src) != cleanText(dst))
        sub = c.src;
}

namespace detail {
inline uint64_t endOf(const Cue& c) { return c.endMs > c.startMs ? c.endMs : c.startMs + 1; }
}

// ---------------------------------------------------------------------------
// SubRip. UTF-8 BOM, CRLF, numbered from 1 over the cues that have any text.
// ---------------------------------------------------------------------------
inline std::string srt(const std::vector<Cue>& cues, Which w) {
    std::string out = "\xEF\xBB\xBF";
    unsigned n = 0;
    for (const Cue& c : cues) {
        std::string m, s;
        textsFor(c, w, m, s);
        // (srtSafe per line: a "-->" defused; the user's line breaks kept -- wrapKeepingBreaks)
        std::vector<std::string> ml = wrapKeepingBreaks(m), sl = wrapKeepingBreaks(s);
        for (auto& l : ml) l = srtSafe(l);
        for (auto& l : sl) l = srtSafe(l);
        if (ml.empty() && sl.empty()) continue;   // a cue with no text would END itself at once
        out += std::to_string(++n);
        out += "\r\n";
        out += stamp(c.startMs, ',') + " --> " + stamp(detail::endOf(c), ',') + "\r\n";
        for (const auto& l : ml) { out += l; out += "\r\n"; }
        for (size_t i = 0; i < sl.size(); ++i) {
            if (i == 0) out += "<i>";
            out += sl[i];
            if (i + 1 == sl.size()) out += "</i>";
            out += "\r\n";
        }
        out += "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// WebVTT. Same cues, same numbering (as cue identifiers), "." in the stamps, text escaped, and
// the original of a bilingual cue in a CLASS span players may style (".orig") and in italics,
// which every player renders even when it ignores classes.
// ---------------------------------------------------------------------------
inline std::string vtt(const std::vector<Cue>& cues, Which w) {
    std::string out = "\xEF\xBB\xBF" "WEBVTT\r\n\r\n";
    unsigned n = 0;
    for (const Cue& c : cues) {
        std::string m, s;
        textsFor(c, w, m, s);
        const std::vector<std::string> ml = wrapKeepingBreaks(m);
        const std::vector<std::string> sl = wrapKeepingBreaks(s);
        if (ml.empty() && sl.empty()) continue;
        out += std::to_string(++n);
        out += "\r\n";
        out += stamp(c.startMs, '.') + " --> " + stamp(detail::endOf(c), '.') + "\r\n";
        for (const auto& l : ml) { out += escVtt(l); out += "\r\n"; }
        for (size_t i = 0; i < sl.size(); ++i) {
            if (i == 0) out += "<c.orig><i>";
            out += escVtt(sl[i]);
            if (i + 1 == sl.size()) out += "</i></c>";
            out += "\r\n";
        }
        out += "\r\n";
    }
    return out;
}

}   // namespace pbsub

// =============================================================================
// SuVtitle - engine/TrFileJob.cpp   (FILE MODE, wave WB -- see TrFileJob.h)
//
// SUVTITLE SPLIT (2026-10-23): ported VERBATIM from AudioPB's PatchBus repo, translator/TrFileJob.cpp at
// commit 641f1aa (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Only this header and the comments'
// non-ASCII characters changed (house rule: ASCII-only C++).
// =============================================================================
#include "TrFileJob.h"
#include "TrText.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>

namespace pbt {

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

bool isLetterOrDigitCp(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
        || (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0370 && cp <= 0x1FFF)
        || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF) || isCjkCp(cp);
}
bool endsWithComma(const std::string& w) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < w.size();) cps.push_back(nextCp(w, i));
    while (!cps.empty() && (cps.back() == ' ' || cps.back() == 0x3000)) cps.pop_back();
    if (cps.empty()) return false;
    const uint32_t c = cps.back();
    return c == ',' || c == ':' || c == 0xFF0C /*U+FF0C*/ || c == 0x3001 /*U+3001*/ || c == 0xFF1A /*U+FF1A*/;
}
std::string joinText(const std::vector<FileWord>& ws) {
    std::string s;
    for (const auto& w : ws) s += w.text;
    return oneLine(s);
}
// One word's script for the language-change rule: 1 = Han only, 2 = Latin only, 0 = neither or
// both (digits, punctuation, other alphabets, a mixed token).
int scriptOfText(const std::string& t) {
    const int h = hanChars(t), l = latinChars(t);
    if (h > 0 && l == 0) return 1;
    if (l > 0 && h == 0) return 2;
    return 0;
}
uint32_t upperCp(uint32_t cp, bool& isLower);
// Review P6: a word that OPENS A SPEAKER'S TURN -- a dialogue dash before it ("- Non !", "- Ouais", "-He") --
// never a hyphen that continues a word ("-t-il": a lowercase letter right after it).
bool opensTurn(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && t[i] == ' ') ++i;
    if (i >= t.size()) return false;
    const uint32_t c = nextCp(t, i);
    if (c != '-' && c != 0x2013 && c != 0x2014) return false;
    if (i >= t.size()) return true;
    bool low = false;
    upperCp(nextCp(t, i), low);
    return !low;
}

// ---- WF: words out of tokens --------------------------------------------------------------------
std::vector<uint32_t> cpsOf(const std::string& s) {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < s.size();) v.push_back(nextCp(s, i));
    return v;
}
// Written WITHOUT spaces between words: Han, kana, Thai, Lao, Myanmar, Khmer -- and the CJK / full-width
// punctuation that goes with them. A token there is a unit of its own, as before WF. (Review L2: Korean
// IS written with spaces between words -- Hangul tokens glue like any alphabet's.)
bool isHangulCp(uint32_t cp) {
    return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}
bool isWideCp(uint32_t cp) {
    return isCjkCp(cp) || (isKanaHangulCp(cp) && !isHangulCp(cp)) || (cp >= 0x0E00 && cp <= 0x0EFF)
        || (cp >= 0x1000 && cp <= 0x109F) || (cp >= 0x1780 && cp <= 0x17FF) || (cp >= 0x3000 && cp <= 0x303F)
        || (cp >= 0xFF00 && cp <= 0xFFEF);
}
bool isWordCp(uint32_t cp) { return isLetterOrDigitCp(cp) && !isWideCp(cp); }
// What OPENS the next word when it is written after a space: brackets, opening quotes, inverted
// marks, and a dialogue dash ("- Il est la ! - Pas ici !").
bool isOpeningCp(uint32_t cp) {
    return cp == '(' || cp == '[' || cp == '{' || cp == '"' || cp == '*' || cp == 0x00AB /*"*/ || cp == 0x00BF /*?*/
        || cp == 0x00A1 /*!*/ || cp == 0x201C || cp == 0x2018 || cp == 0x201E || cp == 0x201A
        || cp == '-' || cp == 0x2013 || cp == 0x2014;
}
// c' l' d' j' m' n' s' t' qu' jusqu' lorsqu' puisqu' quoiqu' -- the French elisions, whatever the case
// and whatever opens the word (""C'").
bool endsWithElision(const std::string& word) {
    std::vector<uint32_t> c = cpsOf(word);
    while (!c.empty() && c.back() == ' ') c.pop_back();
    if (c.empty() || (c.back() != '\'' && c.back() != 0x2019)) return false;
    c.pop_back();
    std::string w;
    for (size_t i = c.size(); i-- > 0;) {
        const uint32_t x = c[i];
        if (!isWordCp(x)) break;
        w.insert(w.begin(), (char)((x >= 'A' && x <= 'Z') ? x + 32 : (x < 0x80 ? x : '?')));
    }
    for (const char* e : { "c", "d", "j", "l", "m", "n", "s", "t", "qu", "jusqu", "lorsqu", "puisqu", "quoiqu" })
        if (w == e) return true;
    return false;
}
// A token's plausible spoken length: what a DTW start may not be earlier than, before its end.
double tokenDurSec(const std::string& t) {
    int n = 0;
    for (size_t i = 0; i < t.size();) if (isLetterOrDigitCp(nextCp(t, i))) ++n;
    return std::min(1.0, 0.12 + 0.07 * n);
}
// ... and a whole word's, generous (a dragged or sung word): the longest a word's DTW span may be.
double wordDurSec(const std::string& t) {
    int n = 0;
    for (size_t i = 0; i < t.size();) if (isLetterOrDigitCp(nextCp(t, i))) ++n;
    return std::min(2.0, 0.15 + 0.10 * n);
}

// ---- WF: scripts (foreignScript) -------------------------------------------------------------------
enum Script : uint32_t {
    kScLatin = 1, kScCyrillic = 2, kScGreek = 4, kScHebrew = 8, kScArabic = 16, kScDevanagari = 32,
    kScThai = 64, kScHan = 128, kScKana = 256, kScHangul = 512, kScOther = 1024
};
uint32_t scriptOfCp(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)
        || (cp >= 0x1E00 && cp <= 0x1EFF)) return kScLatin;
    if (cp >= 0x0400 && cp <= 0x052F) return kScCyrillic;
    if ((cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1F00 && cp <= 0x1FFF)) return kScGreek;
    if (cp >= 0x0590 && cp <= 0x05FF) return kScHebrew;
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F)) return kScArabic;
    if (cp >= 0x0900 && cp <= 0x097F) return kScDevanagari;
    if (cp >= 0x0E00 && cp <= 0x0E7F) return kScThai;
    if (isCjkCp(cp)) return kScHan;
    if ((cp >= 0x3040 && cp <= 0x30FF && cp != 0x30FB && cp != 0x30FC) || (cp >= 0x31F0 && cp <= 0x31FF)
        || (cp >= 0xFF66 && cp <= 0xFF9F)) return kScKana;
    if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) return kScHangul;
    if (cp >= 0x0370 && cp <= 0x1FFF) return kScOther;
    return 0;                                            // digits, punctuation, symbols: no script
}
// The scripts a language is written in. Latin is ALWAYS allowed (an English word inside any language's
// speech is ordinary); a code this table does not know allows everything (no check at all).
uint32_t scriptsOfLang(const std::string& l) {
    if (l.empty()) return 0xFFFFFFFFu;
    for (const char* c : { "ru", "uk", "bg", "sr", "mk", "be", "kk" }) if (l == c) return kScLatin | kScCyrillic;
    if (l == "el") return kScLatin | kScGreek;
    if (l == "he" || l == "yi") return kScLatin | kScHebrew;
    for (const char* c : { "ar", "fa", "ur", "ps" }) if (l == c) return kScLatin | kScArabic;
    for (const char* c : { "hi", "mr", "ne" }) if (l == c) return kScLatin | kScDevanagari;
    if (l == "th") return kScLatin | kScThai;
    if (l == "zh" || l == "zh-tw" || l == "yue") return kScLatin | kScHan;
    if (l == "ja") return kScLatin | kScHan | kScKana;
    if (l == "ko") return kScLatin | kScHangul | kScHan;
    // the languages whisper writes in the Latin alphabet
    for (const char* c : { "en", "fr", "de", "es", "it", "pt", "nl", "pl", "tr", "sv", "da", "no", "fi", "cs",
                           "ro", "hu", "ca", "ms", "tl", "id", "vi", "sk", "sl", "hr", "et", "lv", "lt", "is",
                           "ga", "cy", "eu", "gl", "af", "sw", "sq", "az", "uz", "la", "mt", "lb", "br", "oc" })
        if (l == c) return kScLatin;
    return 0xFFFFFFFFu;
}
// ---- WF: tags and credits -------------------------------------------------------------------------
// A letter folded to UPPER case for the tag stems (ASCII, Latin-1, Cyrillic, Greek); 0 = not cased.
uint32_t upperCp(uint32_t cp, bool& isLower) {
    isLower = false;
    if (cp >= 'a' && cp <= 'z') { isLower = true; return cp - 32; }
    if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) { isLower = true; return cp - 32; }
    if (cp >= 0x0430 && cp <= 0x044F) { isLower = true; return cp - 0x20; }
    if (cp >= 0x0450 && cp <= 0x045F) { isLower = true; return cp - 0x50; }
    if (cp >= 0x03B1 && cp <= 0x03C9 && cp != 0x03C2) { isLower = true; return cp - 0x20; }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7)
        || (cp >= 0x0400 && cp <= 0x042F) || (cp >= 0x0391 && cp <= 0x03A9)) return cp;
    return 0;
}
} // namespace

// =============================================================================
// Text helpers
// =============================================================================
int latinChars(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextCp(s, i);
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)) ++n;
    }
    return n;
}
int hanChars(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) if (isCjkCp(nextCp(s, i))) ++n;
    return n;
}
// THE WIDTH CLASSES the length limits count in (WB, after the review): every alphabet is NARROW
// like Latin (Cyrillic, Greek, Arabic...), Han, kana and Hangul are WIDE. For English and Chinese
// text these are latinChars / hanChars exactly; a Russian or Japanese cue used to count ZERO and
// was never split however long it ran.
int narrowChars(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextCp(s, i);
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)
            || (cp >= 0x0370 && cp <= 0x1FFF && !isKanaHangulCp(cp))) ++n;
    }
    return n;
}
int wideChars(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextCp(s, i);
        if (isCjkCp(cp) || isKanaHangulCp(cp)) ++n;
    }
    return n;
}
bool hasLetterOrDigit(const std::string& s) {
    for (size_t i = 0; i < s.size();) if (isLetterOrDigitCp(nextCp(s, i))) return true;
    return false;
}
std::string oneLine(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    bool space = true;                                  // also trims the front
    for (char c : in) {
        const char t = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        if (t == ' ') { if (!space) { s += ' '; space = true; } }
        else { s += t; space = false; }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// =============================================================================
// WF: tokens -> words (TrFileJob.h)
// =============================================================================
bool tokenContinuesWord(const std::string& cur, const std::string& tok) {
    if (cur.empty()) return false;
    size_t k = 0;
    while (k < tok.size() && tok[k] == ' ') ++k;
    if (k == tok.size()) return true;                       // a bare space: nothing to show, rides along
    const bool lead = k > 0;
    const std::vector<uint32_t> cc = cpsOf(tok.substr(k));
    std::vector<uint32_t> pc = cpsOf(cur);
    while (!pc.empty() && pc.back() == ' ') pc.pop_back();
    if (pc.empty()) return false;
    const uint32_t f = cc.front(), l = pc.back();
    if (!hasLetterOrDigit(tok)) {
        // punctuation rides on the word before it -- but an OPENING mark (or a dialogue dash) written
        // after a space opens the next word
        return !(lead && isOpeningCp(f));
    }
    // the word so far is only an opening mark (" / ( / a dialogue dash): this is its first word
    if (!hasLetterOrDigit(cur) && isOpeningCp(l)) return true;
    // no space: the same word -- unless either side is written without spaces at all (Han...)
    if (!lead) return !isWideCp(f) && !isWideCp(l);
    // across a space: an elision ("c' est"), or a hyphen written against its letters ("Etes- vous",
    // "va -t-il") -- before a LOWERCASE letter only (review P4): "Je ne sais pas si-" + " Non !" is an
    // interrupted word and the next speaker, never one word
    auto lowerAt = [](uint32_t cp) { bool low = false; upperCp(cp, low); return low; };
    if ((l == '\'' || l == 0x2019) && endsWithElision(cur)) return true;
    if (l == '-' && pc.size() >= 2 && isWordCp(pc[pc.size() - 2]) && lowerAt(f)) return true;
    if (f == '-' && cc.size() >= 2 && isWordCp(cc[1]) && lowerAt(cc[1]) && isWordCp(l)) return true;
    return false;
}

std::vector<WindowWord> windowWords(const AsrResult& a, double s0) {
    struct Range { size_t w0, w1; };
    std::vector<Range> rs;
    if (!a.segments.empty()) for (const auto& s : a.segments) rs.push_back({ s.w0, s.w1 });
    else if (!a.words.empty()) rs.push_back({ 0, a.words.size() });
    struct Grp { size_t seg; std::vector<size_t> toks; std::string text; };
    std::vector<Grp> g;
    for (size_t si = 0; si < rs.size(); ++si)
        for (size_t t = rs[si].w0; t < rs[si].w1 && t < a.words.size(); ++t) {
            const AsrWord& aw = a.words[t];
            if (aw.text.empty()) continue;
            // a segment may END inside a word ("...en c" | "royant"): the word goes on across it -- but only
            // a token written WITHOUT a space does; a segment that opens on " *Gasp*" is a word of its own
            // (its segment may be junk: it must not ride on the last word of the line before it)
            // (review L6: and never after a full stop -- "ciel." | "Bon" is two words)
            const bool segStart = g.empty() || g.back().seg != si;
            if (!g.empty() && (!segStart || (aw.text[0] != ' ' && !endsSentence(g.back().text)))
                && tokenContinuesWord(g.back().text, aw.text)) {
                g.back().text += aw.text;
                g.back().toks.push_back(t);
            } else {
                g.push_back(Grp{ si, { t }, aw.text });
            }
        }
    // DTW for the whole window, or not at all (never a mixture of the two clocks)
    bool timed = false;
    {
        size_t letters = 0, withDtw = 0;
        for (const auto& gr : g)
            for (size_t t : gr.toks)
                if (hasLetterOrDigit(a.words[t].text)) { ++letters; if (a.words[t].dtw >= 0.0) ++withDtw; }
        timed = letters > 0 && withDtw == letters;
    }
    std::vector<WindowWord> out;
    out.reserve(g.size());
    double prevEnd = -1.0;                                   // seconds into the request's audio
    for (const auto& gr : g) {
        std::vector<size_t> lt;                              // its LETTER tokens (punctuation adds no time)
        for (size_t t : gr.toks) if (hasLetterOrDigit(a.words[t].text)) lt.push_back(t);
        double st = 0.0, en = 0.0;
        if (lt.empty()) {
            st = en = prevEnd >= 0.0 ? prevEnd : a.words[gr.toks.front()].start;
        } else if (timed) {
            en = a.words[lt.back()].dtw;
            const double firstEnd = a.words[lt.front()].dtw;
            const double lo = firstEnd - tokenDurSec(a.words[lt.front()].text);
            st = prevEnd < 0.0 ? lo : std::max(std::min(prevEnd, firstEnd - 0.02), lo);
            // ... and never longer than the WHOLE word can take, counted back from its end -- the
            // anchor: over music the path may enter a window's first token anywhere (measured: " C" of
            // "C'est quoi..." ended 13.6 s into a window whose speech was its last 1.5 s)
            st = std::max(st, en - wordDurSec(gr.text));
            if (st < 0.0) st = 0.0;
            if (en < st + 0.04) en = st + 0.04;
            prevEnd = en;
        } else {
            st = a.words[gr.toks.front()].start;
            en = st;
            for (size_t t : gr.toks) en = std::max(en, a.words[t].end);
        }
        WindowWord ww;
        ww.seg = gr.seg;
        ww.w = FileWord{ s0 + st, s0 + en, gr.text, a.language, false, timed && !lt.empty() };
        out.push_back(std::move(ww));
    }
    return out;
}

// =============================================================================
// WF: what is never dialogue (TrFileJob.h)
// =============================================================================
bool isSoundTag(const std::string& text) {
    if (isAnnotationOnly(text)) return true;                      // [Musique], (rires), *music*, U+266A ... U+266A
    // the line's words, folded to upper case, whether all of its cased letters are capitals, and whether it
    // carries SENTENCE punctuation -- review L5: a spoken "Musique !" or "CHEERS!" is dialogue; whisper's
    // bare tags come without it ("MUSIC", "U+0422,0420,0415,0412,041E,0416,041D,0410,042F U+041C,0423,0417,042B,041A,0410", "TRIPT CREDITS")
    std::vector<std::string> words;
    std::string cur;
    int upper = 0, lower = 0;
    bool sentence = false;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = nextCp(text, i);
        bool low = false;
        const uint32_t u = upperCp(cp, low);
        if (u) { (low ? lower : upper)++; appendCp(cur, u); }
        else if (isLetterOrDigitCp(cp)) appendCp(cur, cp);
        else if (cp == '\'' || cp == 0x2019 || cp == '-') { /* inside a word */ }
        else {
            if (cp == '.' || cp == '!' || cp == '?' || cp == 0x2026 || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F)
                sentence = true;
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        }
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty() || sentence) return false;
    // a line that IS a tag word, in any case ("Music", "Musique", "U+97F3,4E50")
    static const char* kWhole[] = {
        "MUSIC", "MUSIQUE", "MUSIK", "MUSICA", "M\xc3\x9aSICA", "\xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90",
        "APPLAUSE", "APPLAUDISSEMENTS", "APLAUSOS", "APPLAUS", "LAUGHTER", "RIRES", "RIRE",
        "\xe9\x9f\xb3\xe4\xb9\x90", "\xe9\x9f\xb3\xe6\xa8\x82", "\xe6\x8e\x8c\xe5\xa3\xb0", "\xe6\x8e\x8c\xe8\x81\xb2",
        "\xe7\xac\x91\xe5\xa3\xb0", "\xe7\xac\x91\xe8\x81\xb2",
    };
    if (words.size() == 1)
        for (const char* k : kWhole) if (words[0] == k) return true;
    // an ALL-CAPS line of at most eight words holding a tag WORD of the vocabulary (exact words, inflections
    // listed: review L5 -- the stems also took "SIGHT" and "SOUNDS GOOD"), in any language
    if (lower > 0 || upper < 4 || words.size() > 8) return false;
    static const char* kTags[] = {
        "MUSIC", "MUSIQUE", "MUSIQUES", "MUSIK", "MUSICA", "M\xc3\x9aSICA",
        "\xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90", "\xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x98",   // U+041C,0423,0417,042B,041A,0410 U+041C,0423,0417,042B,041A,0418
        "APPLAUSE", "APPLAUDISSEMENTS", "APPLAUS", "APLAUSOS", "APPLAUSI",
        "\xd0\x90\xd0\x9f\xd0\x9b\xd0\x9e\xd0\x94\xd0\x98\xd0\xa1\xd0\x9c\xd0\x95\xd0\x9d\xd0\xa2\xd0\xab",       // U+0410,041F,041B,041E,0414,0418,0421,041C,0415,041D,0422,042B
        "LAUGHTER", "LAUGHS", "LAUGHING", "RIRES", "RIRE", "RISAS", "RISATE", "GEL\xc3\x84" "CHTER",
        "\xd0\xa1\xd0\x9c\xd0\x95\xd0\xa5",                                                                     // U+0421,041C,0415,0425
        "SILENCE", "SILENCIO", "\xd0\xa2\xd0\x98\xd0\xa8\xd0\x98\xd0\x9d\xd0\x90",                              // U+0422,0418,0428,0418,041D,0410
        "NOISE", "NOISES", "BRUIT", "BRUITS", "\xd0\xa8\xd0\xa3\xd0\x9c",                                       // U+0428,0423,041C
        "CREDITS", "CR\xc3\x89" "DITS", "G\xc3\x89" "N\xc3\x89RIQUE", "GENERIQUE",
        "\xd0\xa2\xd0\x98\xd0\xa2\xd0\xa0\xd0\xab",                                                             // U+0422,0418,0422,0420,042B
        "SIGH", "SIGHS", "SOUPIR", "SOUPIRS", "GASP", "GASPS", "CHEERING", "CHEERS", "ACCLAMATIONS", "OVATION",
        "INAUDIBLE", "INDISTINCT", "GUNSHOT", "GUNSHOTS", "GUNFIRE", "EXPLOSION", "EXPLOSIONS", "INSTRUMENTAL",
        "WHISTLING", "SIFFLEMENT", "SIFFLEMENTS",
    };
    for (const auto& w : words)
        for (const char* s : kTags)
            if (w == s) return true;
    return false;
}

// Review C2/C3: credits ANCHORED at the line's start (at most `maxExtra` more atoms), per pair language;
// kind 1 = a credits line whatever the context, 2 = a line a person could also say (the caller decides by
// context). Atoms = normWords (CJK / kana / Hangul one per character, punctuation dropped inside a word).
// SuVtitle 1.1.0 (RESUME 11): the line AND the phrases are folded to Simplified first (hanFoldSimplified), so each
// Chinese phrase is written ONCE and matches either script -- the zh-tw rows that doubled four of them are gone,
// and the phrases that had no Traditional row (U+4E2D,6587,5B57,5E55,5FD7,613F,8005...) now match a Traditional line too. (Kanji fold
// as well -- a Japanese row and a Japanese line alike, so they still meet.)
int creditsHallucination(const std::string& text) {
    std::vector<std::string> at = normWords(hanFoldSimplified(text));
    if (at.empty()) return 0;
    for (auto& a : at) {                                   // normWords leaves accented capitals: "REALISES"
        std::string f;
        for (size_t i = 0; i < a.size();) {
            const uint32_t cp = nextCp(a, i);
            appendCp(f, (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) ? cp + 0x20 : cp);
        }
        a = std::move(f);
    }
    for (const auto& a : at) if (a.find("amaraorg") != std::string::npos) return 1;   // "d'Amara.org", "Amara.org-Community"
    struct P { const char* phrase; int maxExtra; int kind; };
    static const P kP[] = {
        { "sous-titrage", 5, 1 },   // fr
        { "sous-titres r\xc3\xa9" "alis\xc3\xa9s par", 6, 1 },   // fr
        { "sous-titres par", 5, 1 },   // fr
        { "sous-titr\xc3\xa9 par", 5, 1 },   // fr
        { "sous-titr\xc3\xa9" "e par", 5, 1 },   // fr
        { "merci d'avoir regard\xc3\xa9 cette vid\xc3\xa9o", 2, 2 },   // fr
        { "abonnez-vous \xc3\xa0 la cha\xc3\xaene", 2, 2 },   // fr
        { "n'h\xc3\xa9sitez pas \xc3\xa0 vous abonner", 3, 2 },   // fr
        { "subtitles by", 4, 1 },   // en
        { "subtitled by", 4, 1 },   // en
        { "captions by", 4, 1 },   // en
        { "captioned by", 4, 1 },   // en
        { "transcription by", 4, 1 },   // en
        { "transcribed by", 4, 1 },   // en
        { "translation by", 4, 2 },   // en
        { "thanks for watching", 1, 2 },   // en
        { "thank you for watching", 1, 2 },   // en
        { "please subscribe", 3, 2 },   // en
        { "like and subscribe", 3, 2 },   // en
        { "don't forget to subscribe", 3, 2 },   // en
        { "for more information visit", 6, 2 },   // en
        { "untertitel im auftrag des zdf", 4, 1 },   // de
        { "untertitelung im auftrag des zdf", 4, 1 },   // de
        { "untertitel von stephanie geiges", 2, 1 },   // de
        { "untertitelung des zdf", 4, 1 },   // de
        { "danke f\xc3\xbcrs zuschauen", 2, 2 },   // de
        { "vielen dank f\xc3\xbcrs zuschauen", 2, 2 },   // de
        { "subtitulado por", 4, 1 },   // es
        { "subt\xc3\xadtulos por", 5, 1 },   // es
        { "subt\xc3\xadtulos realizados por", 6, 1 },   // es
        { "gracias por ver el video", 2, 2 },   // es
        { "gracias por ver", 1, 2 },   // es
        { "suscr\xc3\xad" "bete al canal", 2, 2 },   // es
        { "sottotitoli e revisione a cura di qtss", 2, 1 },   // it
        { "sottotitoli a cura di qtss", 2, 1 },   // it
        { "sottotitoli creati dalla comunit\xc3\xa0", 3, 1 },   // it
        { "grazie per la visione", 2, 2 },   // it
        { "grazie per aver guardato", 3, 2 },   // it
        { "legendas pela comunidade", 3, 1 },   // pt
        { "legendas por", 4, 1 },   // pt
        { "obrigado por assistir", 2, 2 },   // pt
        { "inscreva-se no canal", 2, 2 },   // pt
        { "ondertiteling door", 5, 1 },   // nl
        { "ondertiteld door", 5, 1 },   // nl
        { "bedankt voor het kijken", 2, 2 },   // nl
        { "\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd0\xba\xd1\x82\xd0\xbe\xd1\x80 \xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2", 6, 1 },   // ru
        { "\xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd1\x8b \xd1\x81\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0\xd0\xbb", 4, 1 },   // ru
        { "\xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd1\x8b \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbd\xd0\xb0\xd0\xbb", 4, 1 },   // ru
        { "\xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd1\x8b \xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xb2\xd0\xb0\xd0\xbb", 4, 1 },   // ru
        { "\xd0\xba\xd0\xbe\xd1\x80\xd1\x80\xd0\xb5\xd0\xba\xd1\x82\xd0\xbe\xd1\x80", 3, 1 },   // ru
        { "\xd0\xbf\xd1\x80\xd0\xbe\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82", 1, 2 },   // ru
        { "\xd8\xaa\xd8\xb1\xd8\xac\xd9\x85\xd8\xa9 \xd9\x86\xd8\xa7\xd9\x86\xd8\xb3\xd9\x8a \xd9\x82\xd9\x86\xd9\x82\xd8\xb1", 2, 1 },   // ar
        { "\xd8\xa7\xd8\xb4\xd8\xaa\xd8\xb1\xd9\x83\xd9\x88\xd8\xa7 \xd9\x81\xd9\x8a \xd8\xa7\xd9\x84\xd9\x82\xd9\x86\xd8\xa7\xd8\xa9", 3, 2 },   // ar
        { "\xd8\xb4\xd9\x83\xd8\xb1\xd8\xa7 \xd9\x84\xd9\x84\xd9\x85\xd8\xb4\xd8\xa7\xd9\x87\xd8\xaf\xd8\xa9", 2, 2 },   // ar
        { "\xe3\x81\x94\xe8\xa6\x96\xe8\x81\xb4\xe3\x81\x82\xe3\x82\x8a\xe3\x81\x8c\xe3\x81\xa8\xe3\x81\x86\xe3\x81\x94\xe3\x81\x96\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", 2, 2 },   // ja
        { "\xe3\x83\x81\xe3\x83\xa3\xe3\x83\xb3\xe3\x83\x8d\xe3\x83\xab\xe7\x99\xbb\xe9\x8c\xb2\xe3\x82\x88\xe3\x82\x8d\xe3\x81\x97\xe3\x81\x8f\xe3\x81\x8a\xe9\xa1\x98\xe3\x81\x84\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99", 2, 2 },   // ja
        { "\xec\x8b\x9c\xec\xb2\xad\xed\x95\xb4 \xec\xa3\xbc\xec\x85\x94\xec\x84\x9c \xea\xb0\x90\xec\x82\xac\xed\x95\xa9\xeb\x8b\x88\xeb\x8b\xa4", 2, 2 },   // ko
        { "\xea\xb5\xac\xeb\x8f\x85\xea\xb3\xbc \xec\xa2\x8b\xec\x95\x84\xec\x9a\x94 \xeb\xb6\x80\xed\x83\x81\xeb\x93\x9c\xeb\xa6\xbd\xeb\x8b\x88\xeb\x8b\xa4", 2, 2 },   // ko
        { "\xe8\xaf\xb7\xe4\xb8\x8d\xe5\x90\x9d\xe7\x82\xb9\xe8\xb5\x9e", 20, 1 },   // zh
        { "\xe5\xad\x97\xe5\xb9\x95\xe7\x94\xb1", 20, 1 },   // zh
        { "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe5\xb9\x95\xe5\xbf\x97\xe6\x84\xbf\xe8\x80\x85", 6, 1 },   // zh
        { "\xe5\xad\x97\xe5\xb9\x95\xe5\xbf\x97\xe6\x84\xbf\xe8\x80\x85", 6, 1 },   // zh
        { "\xe8\xb0\xa2\xe8\xb0\xa2\xe8\xa7\x82\xe7\x9c\x8b", 2, 2 },   // zh
        { "\xe6\x84\x9f\xe8\xb0\xa2\xe8\xa7\x82\xe7\x9c\x8b", 2, 2 },   // zh
        // RESUME 11: "please subscribe" (+ "to my channel"), and the broadcast "thanks for watching" -- in both
        // scripts, through the fold, like every row of this list. (U+8BF7,8BA2,9605 / U+8C22,8C22,6536,770B / U+611F,8C22,6536,770B)
        { "\xe8\xaf\xb7\xe8\xae\xa2\xe9\x98\x85", 4, 2 },   // zh
        { "\xe8\xb0\xa2\xe8\xb0\xa2\xe6\x94\xb6\xe7\x9c\x8b", 2, 2 },   // zh
        { "\xe6\x84\x9f\xe8\xb0\xa2\xe6\x94\xb6\xe7\x9c\x8b", 2, 2 },   // zh
    };
    struct Q { std::vector<std::string> atoms; int maxExtra; int kind; };
    static const std::vector<Q> kQ = [] {
        std::vector<Q> q;
        for (const auto& p : kP) q.push_back(Q{ normWords(hanFoldSimplified(p.phrase)), p.maxExtra, p.kind });
        return q;
    }();
    // RESUME 11: a CHAIN of them is one stock line too -- whisper strings its captions ("Thank you for watching.
    // Please subscribe.", U+8B1D,8B1D,89C0,770B U+8ACB,8A02,95B1), which a single phrase and its few extra atoms never covered. A phrase may follow a
    // phrase; the LAST one's extra atoms close the line; a credits phrase (kind 1) anywhere in the chain makes the whole
    // line a credits line. best[i] = the verdict on at[i..] read that way (0 = not one), from the line's end back to its
    // start -- so best[0] with no chain is exactly the single-phrase rule this was.
    const size_t n = at.size();
    std::vector<int> best(n + 1, 0);
    for (size_t i = n; i-- > 0;) {
        int b = 0;
        for (const auto& q : kQ) {
            if (q.atoms.empty() || n - i < q.atoms.size()) continue;
            if (!std::equal(q.atoms.begin(), q.atoms.end(), at.begin() + (std::ptrdiff_t)i)) continue;
            const size_t j = i + q.atoms.size();
            int k = (int)(n - j) <= q.maxExtra ? q.kind : 0;                      // this phrase closes the line
            if (j < n && best[j]) k = (k == 1 || q.kind == 1 || best[j] == 1) ? 1 : 2;   // ... or the next one goes on
            b = (b == 1 || k == 1) ? 1 : std::max(b, k);
        }
        best[i] = b;
    }
    return best[0];
}

// SuVtitle 1.1.0 (RESUME 11) -- whisper's LOOP inside one line, and the LIVE phantom net (TrFileJob.h).
std::string wholeLineRepeat(const std::string& text) {
    const std::vector<std::string> a = normWords(text);
    const size_t n = a.size();
    // the SHORTEST period, so a line that repeats one word ("non non", "no, no, no") has period 1 -- never a sentence
    size_t p = 1;
    for (; p <= n / 2; ++p) {
        bool periodic = true;
        for (size_t i = p; i < n && periodic; ++i) periodic = (a[i] == a[i - p]);
        if (periodic) break;
    }
    if (p > n / 2 || p < kRepeatMinAtoms) return std::string();
    // ONE repeat, as written: the shortest head of `text` that holds exactly its p atoms, then whatever closes it
    // (its punctuation) up to the blank -- or the character -- that begins the next repeat.
    const std::vector<std::string> one(a.begin(), a.begin() + (std::ptrdiff_t)p);
    size_t cut = 0;
    for (size_t i = 0; i < text.size() && !cut;) {
        nextCp(text, i);
        if (normWords(text.substr(0, i)) == one) cut = i;
    }
    if (!cut) return std::string();
    for (size_t i = cut; i < text.size();) {
        const uint32_t cp = nextCp(text, i);
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) break;
        if (normWords(text.substr(0, i)).size() > p) break;          // the next repeat has begun
        cut = i;
    }
    size_t b = 0;
    while (b < cut && (text[b] == ' ' || text[b] == '\t')) ++b;
    return text.substr(b, cut - b);
}

std::string heardWithDoubt(const HeardHow& h) {
    char b[80];
    if (h.outsidePair) return "a language outside the pair";
    if (h.langProb >= 0.0 && h.langProb < kUnsureLangProb) {
        std::snprintf(b, sizeof(b), "its language unsure (%.2f)", h.langProb);
        return b;
    }
    if (h.peakDbfs < kQuietGateDbfs) {
        std::snprintf(b, sizeof(b), "quiet (%d dBFS)", (int)h.peakDbfs);
        return b;
    }
    if (h.confidence < kLowConfidenceLogp) {
        std::snprintf(b, sizeof(b), "whisper unsure of its words (%.2f)", h.confidence);
        return b;
    }
    return std::string();
}

PhantomVerdict livePhantomNet(const std::string& text, const HeardHow& h) {
    PhantomVerdict v;
    v.text = text;
    const std::string doubt = heardWithDoubt(h);
    const int credits = creditsHallucination(text);
    if (credits == 1) {
        v.junk = true;
        v.why = "a credits line";
        return v;
    }
    if (credits == 2 && !doubt.empty()) {
        v.junk = true;
        v.why = "a stock outro, heard with doubt: " + doubt;
        return v;
    }
    const std::string one = wholeLineRepeat(text);
    if (one.empty()) return v;
    if (!doubt.empty()) {
        v.junk = true;
        v.why = "whisper's loop, heard with doubt: " + doubt;
        return v;
    }
    v.loop = true;
    v.text = one;
    return v;
}

void dropEdgeAnnotations(std::vector<FileWord>& ws) {
    auto closerOf = [](uint32_t o) -> uint32_t {
        switch (o) {
            case '[': return ']';
            case '(': return ')';
            case '*': return '*';
            case 0xFF08: return 0xFF09;     // U+FF08 U+FF09
            case 0x3010: return 0x3011;     // U+3010 U+3011
            default: return 0;
        }
    };
    // a word's codepoints without the blanks around it -- and, for the closer test, without the
    // sentence punctuation whisper may put after it ("*Gasp*.")
    auto core = [](const std::string& t, bool dropTrailPunct) {
        std::vector<uint32_t> c = cpsOf(t);
        while (!c.empty() && c.front() == ' ') c.erase(c.begin());
        while (!c.empty() && (c.back() == ' ' || (dropTrailPunct && (c.back() == '.' || c.back() == ','
               || c.back() == '!' || c.back() == '?' || c.back() == ';' || c.back() == ':')))) c.pop_back();
        return c;
    };
    const size_t kMaxRun = 8;
    for (bool again = true; again && !ws.empty();) {          // annotations OPENING the line
        again = false;
        const std::vector<uint32_t> f = core(ws.front().text, false);
        const uint32_t cl = f.empty() ? 0 : closerOf(f.front());
        if (!cl) break;
        for (size_t k = 0; k < ws.size() && k < kMaxRun; ++k) {
            const std::vector<uint32_t> c = core(ws[k].text, true);
            if (c.empty() || c.back() != cl || (k == 0 && c.size() < 2)) continue;
            ws.erase(ws.begin(), ws.begin() + (std::ptrdiff_t)(k + 1));
            again = true;
            break;
        }
    }
    for (bool again = true; again && !ws.empty();) {          // ... and CLOSING it
        again = false;
        const std::vector<uint32_t> l = core(ws.back().text, true);
        if (l.empty()) break;
        for (size_t k = ws.size(); k-- > 0 && ws.size() - k <= kMaxRun;) {
            const std::vector<uint32_t> c = core(ws[k].text, false);
            if (c.empty() || closerOf(c.front()) != l.back() || (k == ws.size() - 1 && c.size() < 2)) continue;
            ws.erase(ws.begin() + (std::ptrdiff_t)k, ws.end());
            again = true;
            break;
        }
    }
}

bool latinScriptLang(const std::string& lang) { return !lang.empty() && scriptsOfLang(lang) == kScLatin; }

bool foreignScript(const std::string& text, const std::string& langA, const std::string& langB,
                   const std::string& langC) {
    const uint32_t ok = scriptsOfLang(langA) | scriptsOfLang(langB) | (langC.empty() ? 0u : scriptsOfLang(langC));
    if (ok == 0xFFFFFFFFu) return false;
    int all = 0, foreign = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t sc = scriptOfCp(nextCp(text, i));
        if (!sc) continue;
        ++all;
        if (!(sc & ok)) ++foreign;
    }
    return foreign > 0 && foreign * 2 > all;
}

// =============================================================================
// EnergyTrack
// =============================================================================
void EnergyTrack::reset() {
    db_.clear();
    std::fill(hist_.begin(), hist_.end(), 0u);
    histN_ = 0;
    histAll_ = 0;
    carry_.clear();
    next_ = 0;
    carryAt_ = 0;
}

// One hop's level, at its place. A hop already known keeps its first value and is not counted
// again (v2: a region decoded twice -- a run re-entering audio another run decoded -- must not
// weigh twice in the floor).
void EnergyTrack::storeHop(size_t h, float d) {
    if (h >= db_.size()) db_.resize(h + 1, kFileUnknownDb);
    if (db_[h] > kFileUnknownDb) return;
    db_[h] = d;
    ++histAll_;
    // DIGITAL SILENCE IS NOT THE NOISE FLOOR (WB, after the review): a file with 10 % of
    // exact zeros (an edited intro, a lead we padded) put the floor at -120 and the active
    // threshold at -62, so its real room tone counted as speech and no pause was ever
    // found in it. Such hops are kept in the track (they are silence) but not in the floor.
    if (d > kFileDigitalSilenceDb) {
        int bin = (int)std::floor(((double)d + 120.0) * 2.0);
        bin = std::max(0, std::min(240, bin));
        ++hist_[(size_t)bin];
        ++histN_;
    }
}

void EnergyTrack::push(const float* x, size_t n) { pushAt(next_, x, n); }

void EnergyTrack::pushAt(uint64_t at, const float* x, size_t n) {
    const uint64_t hop = (uint64_t)kFileHop;
    const uint64_t end = at + n;
    if (at != next_ || (carry_.empty() && at % hop != 0)) {
        // A DISCONTINUITY (a seek): the partial hop carried belongs to audio whose rest will not come,
        // and a hop is only ever measured whole and on the grid -- start at the next boundary.
        carry_.clear();
        const uint64_t aligned = (at + hop - 1) / hop * hop;
        if (aligned >= end) { next_ = end; return; }
        x += (size_t)(aligned - at);
        n -= (size_t)(aligned - at);
        at = aligned;
    }
    if (carry_.empty()) carryAt_ = at;
    size_t i = 0;
    while (i < n) {
        const size_t take = std::min((size_t)kFileHop - carry_.size(), n - i);
        carry_.insert(carry_.end(), x + i, x + i + take);
        i += take;
        if (carry_.size() == (size_t)kFileHop) {
            double sum = 0.0;
            for (float v : carry_) sum += (double)v * v;
            const double rms = std::sqrt(sum / (double)kFileHop);
            const float d = rms > 1.0e-9 ? (float)(20.0 * std::log10(rms)) : -180.0f;
            storeHop((size_t)(carryAt_ / hop), d);
            carry_.clear();
            carryAt_ += hop;
        }
    }
    next_ = end;
}

double EnergyTrack::floorDb() const {
    if (!histN_) return -120.0;
    const size_t want = std::max<size_t>(1, histN_ / 10);
    size_t cum = 0;
    for (size_t b = 0; b < hist_.size(); ++b) {
        cum += hist_[b];
        if (cum >= want) return -120.0 + (double)b * 0.5;
    }
    return 0.0;
}

// Never above kFileActiveCeilDb: on a LOUD floor (music bed, a noisy room) floor + 6 would call
// quiet speech silence -- and silence is skipped, never transcribed. Erring towards "active"
// costs a missed pause; erring the other way costs speech (TrFileJob.h).
double EnergyTrack::activeDb() const {
    return std::min(kFileActiveCeilDb, std::max(-62.0, floorDb() + 6.0));
}

double EnergyTrack::peakDb(double t0, double t1) const {
    if (db_.empty()) return -180.0;
    size_t h0 = hopAt(t0), h1 = hopAt(t1) + 1;
    if (h0 >= db_.size()) h0 = db_.size() - 1;
    if (h1 > db_.size()) h1 = db_.size();
    float best = -180.0f;
    for (size_t h = h0; h < h1; ++h) best = std::max(best, db_[h]);
    return best;
}

double EnergyTrack::inactiveSec(double t0, double t1) const {
    if (t1 <= t0) return 0.0;
    const double thr = activeDb();
    size_t h0 = hopAt(t0), h1 = hopAt(t1);
    size_t n = 0;
    for (size_t h = h0; h < h1; ++h) if (!(h < db_.size() && db_[h] > thr)) ++n;
    return (double)n * kFileHop / kFileRate;
}

// =============================================================================
// planWindow
// =============================================================================
PlanVerdict planWindow(const EnergyTrack& en, uint64_t cursor, uint64_t avail, bool eof,
                       bool first, WindowPlan& out, uint64_t& skipTo, uint64_t limit) {
    const uint64_t hop = (uint64_t)kFileHop;
    // v2: a LIMIT inside what is decoded is an end like EOF -- nothing is planned past it.
    // (limit = ~0: end = avail, final = eof -- the v1 planner, untouched.)
    const bool     limited = limit <= avail;
    const uint64_t end     = limited ? limit : avail;
    const bool     final_  = eof || limited;
    const size_t availHops = std::min(en.hops(), (size_t)(end / hop));
    const double thr = en.activeDb();
    auto act = [&](size_t h) { return en.db(h) > thr; };

    size_t h = (size_t)(cursor / hop);
    while (h < availHops && !act(h)) ++h;
    if (h >= availHops) {
        if (final_) return PlanVerdict::Done;
        // All silent as far as decoded: the cursor may move on, keeping a pre-roll's worth of
        // hops behind it in case speech starts right at the edge of what is decoded.
        const size_t keep = (size_t)std::ceil(kFilePreroll * kFileRate / (double)hop) + 1;
        const uint64_t to = availHops > keep ? (uint64_t)(availHops - keep) * hop : cursor;
        if (to > cursor) { skipTo = to; return PlanVerdict::SkipSilence; }
        return PlanVerdict::NeedAudio;
    }
    const uint64_t pre = (uint64_t)(kFilePreroll * kFileRate);
    uint64_t s = (uint64_t)h * hop;
    s = s > pre ? s - pre : 0;
    if (s < cursor) s = cursor;
    const uint64_t winMax = (uint64_t)(kFileWinMaxSec * kFileRate);
    if (end <= s + winMax) {
        if (!final_) return PlanVerdict::NeedAudio;
        out.s = s; out.e = end; out.why = limited ? WindowPlan::kLimit : WindowPlan::kEof;
        return PlanVerdict::Plan;
    }
    // The LATEST pause (>= kFilePauseSec of inactive hops) starting in [s+a, s+b).
    const size_t pauseHops = (size_t)std::ceil(kFilePauseSec * kFileRate / (double)hop);
    const size_t searchEnd = std::min(availHops, (size_t)((s + winMax) / hop));
    size_t bs = (size_t)-1, be = 0;
    auto search = [&](double a, double b) {
        bs = (size_t)-1;
        const size_t h0 = (size_t)((s + (uint64_t)(a * kFileRate)) / hop);
        const size_t h1 = (size_t)((s + (uint64_t)(b * kFileRate)) / hop);
        for (size_t k = h0; k < h1 && k < searchEnd;) {
            if (act(k)) { ++k; continue; }
            size_t r = k;
            while (r < searchEnd && !act(r)) ++r;
            if (r - k >= pauseHops) { bs = k; be = r; }
            k = r;
        }
        return bs != (size_t)-1;
    };
    bool found = false;
    if (first) found = search(kFileFirstMin, kFileFirstMax);
    if (!found) found = search(kFileWinMinSec, kFileWinMaxSec - kFileTailPad);
    if (found) {
        const uint64_t ps = (uint64_t)bs * hop, pe = (uint64_t)be * hop;
        uint64_t e = std::min(ps + (uint64_t)(kFileTailPad * kFileRate), (ps + pe) / 2);
        if (e > s + winMax) e = s + winMax;
        out.s = s; out.e = e; out.why = WindowPlan::kPause;
        return PlanVerdict::Plan;
    }
    out.s = s; out.e = s + winMax; out.why = WindowPlan::kHard;
    return PlanVerdict::Plan;
}

// =============================================================================
// runStartCursor (v2, WD)
// =============================================================================
uint64_t runStartCursor(const EnergyTrack& en, uint64_t P, uint64_t lo, uint64_t loFar, bool& midSpeech) {
    midSpeech = false;
    if (loFar > lo) loFar = lo;
    if (P <= lo) return lo;
    const uint64_t hop = (uint64_t)kFileHop;
    const double thr = en.activeDb();
    auto act = [&](size_t h) { return en.db(h) > thr; };      // a hop never decoded is not active
    const size_t hLo = (size_t)((lo + hop - 1) / hop), hP = (size_t)(P / hop);
    if (hLo > hP) return lo;
    const size_t pauseHops = (size_t)std::ceil(kFilePauseSec * kFileRate / (double)hop);
    // The LATEST inactive run touching [from, to] of at least `want` hops -- its length counted to its
    // real end, which may lie past `to` (the playhead landed in a breath: the run starts where the
    // breath started). (size_t)-1 = none.
    auto latestPause = [&](size_t from, size_t to, size_t want) {
        size_t best = (size_t)-1;
        for (size_t k = from; k <= to;) {
            if (act(k)) { ++k; continue; }
            size_t r = k;
            while (r < en.hops() && !act(r)) ++r;
            if (r - k >= want) best = k;
            k = std::max(r, k + 1);
        }
        return best;
    };
    for (const size_t want : { pauseHops, (size_t)4 }) {
        const size_t best = latestPause(hLo, hP, want);
        if (best != (size_t)-1) return std::max<uint64_t>(lo, (uint64_t)best * hop);
    }
    // review P1: no breath within the look-back -- a sentence that began a little earlier is still
    // heard whole if a pause lies before it (up to `loFar`)
    const size_t hFar = (size_t)((loFar + hop - 1) / hop);
    if (hFar < hLo) {
        const size_t best = latestPause(hFar, hLo - 1, 4);
        if (best != (size_t)-1) return std::max<uint64_t>(loFar, (uint64_t)best * hop);
    }
    // continuous speech (or a music bed over -40 dBFS): the quietest hop -- a cut there breaks the
    // least, and the run's first window hears a pre-roll before it (fileWindowStep)
    size_t q = hLo;
    for (size_t k = hLo; k <= hP; ++k) if (en.db(k) < en.db(q)) q = k;
    midSpeech = true;
    return std::max<uint64_t>(lo, (uint64_t)q * hop);
}

// =============================================================================
// Junk
// =============================================================================
// WF: whisper loops on a stuttered word with HYPHENS too -- "fleu-fleu-fleu-...-" x 40, over seven segments
// (measured on the film, 9:14) -- and normWords keeps a hyphenated run as ONE token, so the trigram rule
// never saw it. A word made of six or more IDENTICAL pieces in a row is that loop. (Review L4: only inside
// one word -- splitting every hyphen for the trigram rule turned a chant, "Vas-y ! Vas-y ! Vas-y ! Vas-y !
// Vas-y !", into a loop, and "Vas-y, vas-y !" twice into a repeated segment.)
bool hyphenChainLoop(const std::string& text) {
    std::string word;
    auto chain = [](const std::string& w) {
        std::vector<std::string> piece;
        std::string p;
        for (size_t i = 0; i <= w.size(); ++i) {
            if (i == w.size() || w[i] == '-') {
                const std::vector<std::string> a = normWords(p);
                piece.push_back(a.empty() ? std::string() : a.front());
                p.clear();
            } else {
                p += w[i];
            }
        }
        size_t run = 1, best = 1;
        for (size_t i = 1; i < piece.size(); ++i) {
            run = (!piece[i].empty() && piece[i] == piece[i - 1]) ? run + 1 : 1;
            best = std::max(best, run);
        }
        return best >= 6;
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ' ') {
            if (!word.empty() && chain(word)) return true;
            word.clear();
        } else {
            word += text[i];
        }
    }
    return false;
}

bool isRepetitionLoop(const std::string& text) {
    if (hyphenChainLoop(text)) return true;
    const std::vector<std::string> a = normWords(text);
    if (a.size() < 9) return false;
    std::set<std::string> uniq;
    for (size_t i = 0; i + 2 < a.size(); ++i) uniq.insert(a[i] + "\x1f" + a[i + 1] + "\x1f" + a[i + 2]);
    const double total = (double)(a.size() - 2);
    return (double)uniq.size() / total < 0.5;
}

bool fileSegmentIsJunk(const std::string& text, double peakDb, double floorDb,
                       double avgLogprob, const std::string& prompt) {
    // A file has no absolute level -- a quiet recording is not a quiet room -- so "quiet" is
    // relative: the segment's loudest hop under floor + 10 dB, i.e. barely above the file's noise.
    const bool quiet = peakDb < floorDb + 10.0;
    if (isAnnotationOnly(text)) return true;
    // looksLikeHallucination's quiet branch (shortlist + prompt echo) keys on an absolute level;
    // hand it one on the right side of its gate.
    if (looksLikeHallucination(text, quiet ? kQuietGateDbfs - 8.0 : 0.0, prompt)) return true;
    if (isRepetitionLoop(text)) return true;
    if (quiet && avgLogprob < kLowConfidenceLogp) return true;
    return false;
}

// =============================================================================
// commitWindow
// =============================================================================
CommitVerdict commitWindow(const CommitInput& in) {
    CommitVerdict v;
    const AsrResult& a = *in.asr;
    // whisper's times count from what it HEARD (a pre-roll included -- review P1); the window's own
    // end, in those times, is what the hard-cut rule measures against
    const double s0 = (double)in.plan.audioS() / kFileRate;
    const double winLen = (double)(in.plan.e - in.plan.audioS()) / kFileRate;
    // P1, de-dup by time: only the words that START in [keepFrom, e) are this window's
    const double keepFromSec = in.plan.rolled ? (double)in.plan.keepFrom / kFileRate : -1.0;
    const double endSec      = (double)in.plan.e / kFileRate;
    const bool   post        = in.plan.postRolled();
    struct Seg { size_t w0, w1; double start, end; std::string text; double lp; };
    std::vector<Seg> segs;
    if (!a.segments.empty()) {
        for (const auto& s : a.segments) segs.push_back({ s.w0, s.w1, s.start, s.end, s.text, s.avgLogprob });
    } else if (!a.words.empty()) {
        segs.push_back({ 0, a.words.size(), a.words.front().start, a.words.back().end, a.text, 0.0 });
    }
    const double floorDb = in.en ? in.en->floorDb() : -120.0;
    // WF: the window's tokens as WORDS, timed (DTW when the server computed it) -- TrFileJob.h
    const std::vector<WindowWord> ww = windowWords(a, s0);
    for (const auto& x : ww) v.timed = v.timed || x.w.timed;
    std::vector<char> keep(segs.size(), 1);
    std::vector<char> tagged(segs.size(), 0);              // dropped as a sound tag / a credits line
    // F-RESUME: dropped as whisper's FAILURE -- junk, a repeat, a credits line -- so the audio under it was
    // not heard (CommitVerdict::heard); a sound tag (whisper's own "music") or another script was
    std::vector<char> unsaid(segs.size(), 0);
    std::vector<char> nonSpeech(segs.size(), 0);           // ... a sound tag: whisper SAYS what it heard is not speech
    std::vector<size_t> sayable;                           // review C2: credits a person could also say
    // A segment that goes on with the line before it, mid-word: no leading space, a lowercase letter first
    // ("...visit www.fema." | "org, visit www.fema.org", measured on the film's end credits). It is the
    // same line -- what was said of the one before goes for it (never Han / kana: written without spaces).
    auto continues = [&](size_t k) {
        if (k == 0 || segs[k].text.empty() || segs[k].text[0] == ' ') return false;
        size_t at = 0;
        bool low = false;
        upperCp(nextCp(segs[k].text, at), low);
        return low;
    };
    int loopSegs = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        const Seg& s = segs[i];
        // A segment of punctuation alone (whisper often closes a sentence with a lone "." segment)
        // is not junk to DROP: its full stop is what ends the sentence before it, and the cue
        // builder hangs it on the previous word without adding any time.
        if (!hasLetterOrDigit(s.text)) continue;
        if (isRepetitionLoop(s.text)) ++loopSegs;          // (WF: still junk below; counted for the retry)
        // WF: never dialogue, at any level -- a sound tag, a credits line, another script
        const int credits = creditsHallucination(s.text);
        const bool soundTag = isSoundTag(s.text);
        if (soundTag || credits == 1) {
            keep[i] = 0;
            tagged[i] = 1;
            if (!soundTag) unsaid[i] = 1;
            else nonSpeech[i] = 1;
            ++v.tags;
            ++v.junkSegments;
            v.sawJunk = true;
            continue;
        }
        if (credits == 2) sayable.push_back(i);           // decided below, by its context
        if (!in.langA.empty() && foreignScript(s.text, in.langA, in.langB, in.langC)) {
            keep[i] = 0;
            ++v.foreign;
            ++v.junkSegments;
            v.sawJunk = true;
            continue;
        }
        // its level where it is SAID: the DTW span of its words when there is one (whisper's segment
        // times can cover the music before the speech), else the segment's own times
        double a0 = s0 + s.start, a1 = s0 + std::max(s.end, s.start + 0.03);
        if (v.timed) {
            bool any = false;
            for (const auto& x : ww)
                if (x.seg == i && x.w.timed) {
                    if (!any) { a0 = x.w.start; any = true; }
                    a1 = std::max(x.w.end, a0 + 0.03);
                }
        }
        const double pk = in.en ? in.en->peakDb(a0, a1) : 0.0;
        if (fileSegmentIsJunk(s.text, pk, floorDb, s.lp, in.prompt)) {
            keep[i] = 0;
            unsaid[i] = 1;
            ++v.junkSegments;
            v.sawJunk = true;
        }
    }
    // Review C2: "Thank you for watching.", "U+041F,0440,043E,0434,043E,043B,0436,0435,043D,0438,0435 U+0441,043B,0435,0434,0443,0435,0442...", "U+3054,8996,8074,3042,308A,304C,3068,3046,3054,3056,3044,307E,3057,305F" -- whisper's
    // credits over music come ALONE (nothing else said in the window) or from a decode unsure of its
    // language; the same line among other speech, confidently heard, is a person saying it.
    for (size_t i : sayable) {
        if (!keep[i]) continue;
        size_t lineEnd = i + 1;                            // (its own mid-word continuation is the same line)
        while (lineEnd < segs.size() && continues(lineEnd)) ++lineEnd;
        bool alone = true;
        for (size_t k = 0; k < segs.size() && alone; ++k)
            if ((k < i || k >= lineEnd) && keep[k] && hasLetterOrDigit(segs[k].text)) alone = false;
        if (alone || (a.langProb >= 0.0 && a.langProb < kUnsureLangProb)) {
            keep[i] = 0;
            tagged[i] = 1;
            unsaid[i] = 1;
            ++v.tags;
            ++v.junkSegments;
            v.sawJunk = true;
        }
    }
    // ... and the rest of a line dropped as a tag or credits, carried on mid-word into the next segment, goes too
    for (size_t k = 1; k < segs.size(); ++k)
        if (keep[k] && tagged[k - 1] && continues(k) && hasLetterOrDigit(segs[k].text)) {
            keep[k] = 0;
            tagged[k] = 1;
            unsaid[k] = unsaid[k - 1];                     // (F-RESUME: the same line, the same verdict)
            ++v.tags;
            ++v.junkSegments;
            v.sawJunk = true;
        }
    // REPEATED SEGMENTS: whisper caught in its own conditioning repeats a whole sentence, segment
    // after segment. Any segment of four tokens or more that says exactly what an earlier kept
    // segment of this window said is dropped -- a person does not say the same full sentence
    // twice in thirty seconds nearly as often as whisper loops.
    {
        std::vector<std::string> seen;
        std::string all;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!keep[i]) continue;
            const std::vector<std::string> at = normWords(segs[i].text);
            std::string key;
            for (const auto& t : at) { key += t; key += '\x1f'; }
            all += segs[i].text;
            all += ' ';
            if (at.size() < kRepeatMinAtoms) continue;
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                keep[i] = 0;
                unsaid[i] = 1;
                ++v.repeats;
                v.sawJunk = true;
                continue;
            }
            seen.push_back(key);
        }
        const std::vector<std::string> wa = normWords(all);
        bool loopy = false;
        if (wa.size() >= 30) {
            std::set<std::string> uniq;
            for (size_t k = 0; k + 2 < wa.size(); ++k) uniq.insert(wa[k] + "\x1f" + wa[k + 1] + "\x1f" + wa[k + 2]);
            loopy = (double)uniq.size() / (double)(wa.size() - 2) < 0.5;
        }
        // WF: two or more segments that were each a loop on their own ("fleu-fleu-...-" x 6) are the
        // same symptom -- the decode is done again, the loop's DTW times having collapsed its neighbours
        v.degenerate = (v.repeats >= 2) || loopy || loopSegs >= 2;
    }
    // The commit rule. A pause-ended (or the last) window is complete. A HARD cut drops whisper's
    // last segment unless it ends well before the cut: that segment is the one whisper.cpp
    // re-decoded on the leftover after its own last timestamp -- the audio after the cut was
    // never heard, so its end is a guess.
    // Does the last segment run into a hard cut? Its own end -- or, timed, its last word's DTW end.
    // WF: the last segment that SAYS something -- whisper-server closes most windows with an EMPTY
    // segment ("0.00-0.00", measured: 326 of the film's 370 hard windows), and that one used to be
    // "the last": the rule never fired, and 32 sentences cut by the window's end were committed half-heard.
    const double backSec = winLen - kFileHardBack;           // in the request's own seconds
    size_t lastSeg = segs.size();
    if (in.plan.why == WindowPlan::kHard && !segs.empty()) {
        size_t ls = segs.size() - 1;
        while (ls > 0 && !hasLetterOrDigit(segs[ls].text)) --ls;
        double lastEnd = segs[ls].end;
        if (v.timed)
            for (const auto& x : ww) if (x.seg == ls && x.w.timed) lastEnd = x.w.end - s0;
        if (lastEnd > backSec) lastSeg = ls;
    }
    auto take = [&](size_t i, bool onlyEarly, std::vector<FileWord>& into) {
        std::vector<FileWord> seg;
        for (const auto& x : ww) {
            if (x.seg != i) continue;
            const FileWord& w = x.w;
            if (onlyEarly && w.end - s0 > backSec) continue;
            // heard in the pre-roll / the post-roll: a word of the stretch on the other side of the join
            if (in.plan.rolled && (w.start < keepFromSec || (post && w.start >= endSec))) continue;
            // WF: a word in a script outside the pair, inside a line that is not ("U+0410,041F,041B,041E,0414,0418,0421,041C,0415,041D,0422,042B L'essai
            // numero 1, 2,"): dropped on its own
            if (!in.langA.empty() && hasLetterOrDigit(w.text) && foreignScript(w.text, in.langA, in.langB, in.langC)) continue;
            seg.push_back(w);
        }
        // WF: an ANNOTATION before or after the speech of a line ("Meep meep! *Gasp*", "[Musique] Bon...")
        // goes on its own -- in the middle of a line a wrapped word may be emphasis ("I like *this* one")
        dropEdgeAnnotations(seg);
        into.insert(into.end(), seg.begin(), seg.end());
    };
    for (size_t i = 0; i < lastSeg; ++i) if (keep[i]) take(i, false, v.words);
    // Review C1: what comes before the dropped segment may be PUNCTUATION ALONE (" ..." | "- -" | " U+266A", or
    // a line whose words take() removed): that is no commit -- cleared HERE, so the hard-cut rules below
    // still decide what becomes of the dropped segment's speech (cleared after them, it was never committed
    // nor heard again: the next window started at the window's end)
    auto noLetters = [](const std::vector<FileWord>& ws) {
        return std::none_of(ws.begin(), ws.end(), [](const FileWord& w) { return hasLetterOrDigit(w.text); });
    };
    if (noLetters(v.words)) v.words.clear();
    if (in.plan.why == WindowPlan::kHard && v.words.empty()) {
        if (!v.timed) {
            // One segment covering the whole window (no complete segment before the cut): commit by
            // WORDS that end before the back-off, so the job still moves on.
            for (size_t i = 0; i < segs.size(); ++i) if (keep[i]) take(i, true, v.words);
            // Review C1: none of them ends before it (the speech began in the window's last second): without
            // DTW there is no place to come back to -- every word is committed, as before WF, never dropped
            if (noLetters(v.words)) {
                v.words.clear();
                for (size_t i = 0; i < segs.size(); ++i) if (keep[i]) take(i, false, v.words);
            }
        } else {
            // WF, B -- THE DUPLICATE'S ROOT. No complete segment before a hard cut: on the film, ONE
            // segment for a 30 s window of music whose only speech was its last 1.5 s -- its tokens
            // stretched over the whole window, committed by those times, then heard again whole in the
            // next window. Timed by DTW the words are where they are said, and: the early words are
            // committed up to the last sentence end among them; with none, a sentence that only began
            // in the second half of the window is NOT committed -- the next window starts on it (less a
            // breath) and hears it whole; only speech running from early on (no sentence end in 29 s)
            // commits its early words, so the job still moves on.
            std::vector<FileWord> cand;
            for (size_t i = 0; i < segs.size(); ++i) if (keep[i]) take(i, false, cand);
            // (punctuation opening the window is no sentence to come back to -- see below)
            while (!cand.empty() && !hasLetterOrDigit(cand.front().text)) cand.erase(cand.begin());
            size_t nEarly = 0;
            while (nEarly < cand.size() && cand[nEarly].end - s0 <= backSec) ++nEarly;
            size_t upTo = 0;
            for (size_t k = 0; k < nEarly; ++k) if (endsSentence(cand[k].text)) upTo = k + 1;
            if (upTo == 0 && nEarly > 0 && cand.front().start - s0 < winLen * 0.5) upTo = nEarly;
            v.words.assign(cand.begin(), cand.begin() + (std::ptrdiff_t)upTo);
            if (v.words.empty() && !cand.empty()) {
                uint64_t c = (uint64_t)std::llround(std::max(0.0, cand.front().start - 0.3) * kFileRate);
                c = std::max<uint64_t>(c, in.plan.s + (uint64_t)kFileHop);
                c = std::min<uint64_t>(c, in.plan.e);
                v.heldBack = true;
                v.nextCursor = c;
            }
        }
    }
    // WF: PUNCTUATION ALONE IS NOT PROGRESS. Over the end credits' music whisper answered a hard window
    // with ONE segment of ". . . . . . ." -- committed as a "word" at the window's start, so the next
    // window began a hop (30 ms) later, and again: 287 windows crawling over 30 s of credits, 356 s of
    // GPU on the film (half its ASR time). A commit with no letter in it commits nothing: the window was
    // heard and holds nothing to show, so a hard cut moves on to its end like a silent one.
    if (noLetters(v.words)) v.words.clear();
    // v2 (WD): a window stopped at a run's LIMIT is complete -- every word -- unless the speech runs
    // ACROSS the limit (active on both sides of it). Then the window normally HEARD past it (a
    // post-roll, review P1: the words starting before the limit are whole, the rest was dropped
    // above); only a window with no room for one (already 30 s long) heard its last words up to the
    // stop: those words are CUT, and the cue builder drops them (the stretch beyond has them).
    if (in.plan.why == WindowPlan::kLimit && in.en && !post) {
        const size_t hl = (size_t)(in.plan.e / (uint64_t)kFileHop);
        if (hl > 0 && in.en->active(hl - 1) && in.en->active(hl)) {
            const double eSec = (double)in.plan.e / kFileRate;
            for (FileWord& w : v.words) if (w.end > eSec - kFileCutTolSec) w.cut = true;
        }
    }
    if (in.plan.why != WindowPlan::kHard) {
        v.nextCursor = in.plan.e;
    } else if (v.heldBack) {
        // (set above: back to where the uncommitted sentence begins)
    } else if (!v.words.empty()) {
        // Right after the last COMMITTED word, and nowhere later (WB, after the review): a floor
        // of "at least 5 s" skipped the audio of the dropped segment's first seconds, i.e. lost
        // words. Progress is still certain -- a committed word ends after the window starts.
        uint64_t c = (uint64_t)std::llround((v.words.back().end + 0.02) * kFileRate);
        c = std::max<uint64_t>(c, in.plan.s + (uint64_t)kFileHop);
        c = std::min<uint64_t>(c, in.plan.e);
        v.nextCursor = c;
    } else {
        v.nextCursor = in.plan.e;        // whisper heard nothing usable (F-RESUME: hearCheck may send it back once)
    }
    // F-RESUME: where whisper WROTE what it is judged to have SAID (TrFileJob.h, hearCheck) -- each real word where
    // it is (a word with a letter or a digit; never a sound tag's, never one of a segment dropped as whisper's
    // failure); and a segment that SAYS "not speech" -- a sound tag, or punctuation alone (" .", ". . . ." over
    // the end credits' music) -- over its own span: the alignment of such tokens says nothing, the segment's
    // times are whisper's statement about that stretch. (A segment of TEXT is never taken at its own times:
    // measured, "0.00-30.00" for words that end 5.6 s into a 10.7 s window.)
    for (const auto& x : ww)
        if (x.seg < unsaid.size() && !unsaid[x.seg] && !nonSpeech[x.seg] && hasLetterOrDigit(x.w.text))
            v.heardReal.push_back({ x.w.start, x.w.end });
    auto hasNote = [](const std::string& t) {
        for (size_t k = 0; k < t.size();) { const uint32_t c = nextCp(t, k); if (c >= 0x2669 && c <= 0x266C) return true; }
        return false;
    };
    for (size_t i = 0; i < segs.size(); ++i) {
        const bool said = segs[i].text.find_first_not_of(' ') != std::string::npos;
        if (!said || unsaid[i] || (!nonSpeech[i] && hasLetterOrDigit(segs[i].text))) continue;   // (a line of text: above)
        const std::pair<double, double> span{ s0 + segs[i].start, s0 + std::max(segs[i].start, segs[i].end) };
        v.heardNonSpeech.push_back(span);
        // ... and an EXPLICIT sound tag -- wrapped ("[Applause]", "(rires)", "*music*") or a music note -- also says
        // what follows a line's last word (round 3, review LOW 4); punctuation alone never does (review F6)
        if ((nonSpeech[i] && isAnnotationOnly(segs[i].text)) || hasNote(segs[i].text)) v.heardTags.push_back(span);
    }
    return v;
}

// =============================================================================
// F-RESUME: audio whisper stopped writing in (TrFileJob.h)
// =============================================================================
double activeSecOutside(const EnergyTrack& en, double from, double to,
                        const std::vector<std::pair<double, double>>& cover, double pre, double post) {
    if (!(to > from) || en.hops() == 0) return 0.0;
    const double hopSec = (double)kFileHop / kFileRate;
    const double thr = en.activeDb();
    double sec = 0.0;
    const size_t h0 = EnergyTrack::hopAt(from), h1 = std::min(en.hops(), EnergyTrack::hopAt(to));
    for (size_t h = h0; h < h1; ++h) {
        const double t = EnergyTrack::timeOf(h);
        if (t < from - 1e-9 || !(en.db(h) > thr)) continue;   // (a hop never decoded is never active)
        const double c = t + hopSec * 0.5;
        bool in = false;
        for (const auto& s : cover) if (c >= s.first - pre && c < s.second + post) { in = true; break; }
        if (!in) sec += hopSec;
    }
    return sec;
}

HearCheck hearCheck(const EnergyTrack& en, const WindowPlan& plan, const CommitVerdict& v, const HearContext& ctx) {
    HearCheck hc;
    // a HARD cut that kept words is left to its own rule (it already starts the next window at its last word)
    if (plan.why == WindowPlan::kHard && (ctx.hadWords || v.heldBack)) return hc;
    const uint64_t hop = (uint64_t)kFileHop;
    // the stretch the verdict vouches for: from the window's own start (a pre-roll is the stretch before's) to its
    // next cursor -- and the last word whisper wrote that STARTS in it (a post-roll's words are the next stretch's)
    const uint64_t s0 = std::max<uint64_t>(plan.s, plan.rolled ? plan.keepFrom : 0);
    const double from = (double)s0 / kFileRate;
    const double to   = std::max(from, (double)v.nextCursor / kFileRate);
    const double pre  = v.timed ? kUnheardPadPre : kUnheardPadUntimed;
    const double post = v.timed ? kUnheardPadPost : kUnheardPadUntimed;
    for (const auto& w : v.heardReal)
        if (w.first >= from - 1e-9 && w.first < to) hc.lastWord = std::max(hc.lastWord, w.second);
    double lastKept = -1.0;                                    // what this request COMMITS (after the de-dup nets)
    for (const auto& w : v.words)
        if (hasLetterOrDigit(w.text) && w.start >= from - 1e-9 && w.start < to) lastKept = std::max(lastKept, w.end);
    // where an accepted second hearing stops: the end of what was heard before -- or its own last kept word, when
    // that lies further (round 3, review F9 / F9b: the audio beyond is on its FIRST hearing)
    auto acceptTo = [&](double kept) {
        uint64_t c = ctx.secondUpTo;
        if (kept >= 0.0) c = std::max<uint64_t>(c, (uint64_t)std::llround((kept + 0.02) * kFileRate));
        c = std::max<uint64_t>(c, plan.s + hop);
        return std::min<uint64_t>(c, v.nextCursor);
    };
    if (lastKept >= 0.0) {
        // AN EARLY STOP? the active audio after the last heard word, but for what an explicit sound tag covers
        const double last = std::max(hc.lastWord, lastKept);
        hc.activeSec = activeSecOutside(en, last + post, to, v.heardTags, pre, post);
        if (hc.activeSec < kUnheardTailSec - 1e-9) return hc;
        uint64_t c = (uint64_t)std::llround((last + 0.02) * kFileRate);
        c = std::max<uint64_t>(c, plan.s + hop);
        if (c >= v.nextCursor) return hc;                       // (nothing after it to send back)
        if (ctx.prevCut && lastKept < (double)ctx.prevCut / kFileRate + kUnheardTailSec) {
            // a second hearing that wrote little past the cut it follows (music with one hallucinated word, review
            // F12c): heard twice -- accepted, never cut again (no crawl)
            hc.accepted = true;
            hc.nextCursor = acceptTo(lastKept);
            return hc;
        }
        if (ctx.cutChain >= kMaxCutChain) { hc.capped = true; return hc; }   // the safety net: vouched as before
        hc.rehear = true;
        hc.nextCursor = c;
        return hc;
    }
    // NOTHING COMMITTED: the active audio after the last heard word (an explicit tag covering) -- or, no word heard
    // at all, the active audio no "not speech" statement of whisper's covers
    hc.activeSec = hc.lastWord >= 0.0 ? activeSecOutside(en, hc.lastWord + post, to, v.heardTags, pre, post)
                                      : activeSecOutside(en, from, to, v.heardNonSpeech, pre, post);
    if (hc.activeSec < kUnheardTailSec - 1e-9) return hc;
    if (ctx.secondUpTo > s0) {
        hc.accepted = true;                                    // heard twice, nothing either time: nothing to lose ...
        hc.nextCursor = acceptTo(-1.0);                        // ... but only what WAS heard twice
        return hc;
    }
    hc.rehear = true;
    hc.same = true;
    hc.nextCursor = plan.s;
    return hc;
}

bool shrinkToInnerPause(const EnergyTrack& en, WindowPlan& plan) {
    const uint64_t hop = (uint64_t)kFileHop;
    const double thr = en.activeDb();
    const uint64_t len = plan.e - plan.s;
    const size_t h0 = (size_t)((plan.s + len / 4) / hop), h1 = (size_t)((plan.s + 3 * len / 4) / hop);
    const size_t limit = std::min(en.hops(), (size_t)(plan.e / hop));
    const size_t pauseHops = (size_t)std::ceil(kFilePauseSec * kFileRate / (double)hop);
    size_t bs = (size_t)-1, be = 0, bestLen = 0;
    for (size_t k = h0; k < h1 && k < limit;) {
        if (en.db(k) > thr) { ++k; continue; }
        size_t r = k;
        while (r < limit && !(en.db(r) > thr)) ++r;
        if (r - k >= pauseHops && r - k > bestLen) { bs = k; be = r; bestLen = r - k; }
        k = r;
    }
    if (bs == (size_t)-1) return false;
    const uint64_t ps = (uint64_t)bs * hop, pe = (uint64_t)be * hop;
    plan.e = std::min(ps + (uint64_t)(kFileTailPad * kFileRate), (ps + pe) / 2);
    plan.why = WindowPlan::kPause;
    plan.he = 0;                         // a pause-ended window: no post-roll (a pre-roll stays)
    return true;
}

void dropRepeatedHead(std::vector<FileWord>& next, const std::vector<FileWord>& committedTail, double seamSec) {
    if (next.empty() || committedTail.empty()) return;
    std::vector<AsrWord> tw, hw;
    const size_t t0 = committedTail.size() > 12 ? committedTail.size() - 12 : 0;
    for (size_t i = t0; i < committedTail.size(); ++i) { AsrWord w; w.text = committedTail[i].text; tw.push_back(w); }
    for (size_t i = 0; i < next.size() && i < 12; ++i)   { AsrWord w; w.text = next[i].text; hw.push_back(w); }
    std::vector<std::string> ta, ha;
    std::vector<size_t> te, he;
    atomsOf(tw, ta, te);
    atomsOf(hw, ha, he);
    size_t best = 0;
    for (size_t k = std::min<size_t>(8, std::min(ta.size(), ha.size())); k >= 2; --k) {
        if (std::equal(ta.end() - (std::ptrdiff_t)k, ta.end(), ha.begin())) { best = k; break; }
    }
    if (!best) {
        // F-RESUME: ONE word heard twice at an EARLY STOP's seam -- the last committed word written again as the next
        // window's first, starting within kSeamCopySec of the seam: the same audio (its DTW end placed early, the
        // window after the seam starting inside it). The two atoms above and dropAlreadyCommitted's three never see
        // it. Further away, or at a hard cut, a word may be said twice (round 3, review LOW 3): it stays.
        if (seamSec < 0.0) return;
        size_t lw = committedTail.size(), fw = 0;
        while (lw > 0 && !hasLetterOrDigit(committedTail[lw - 1].text)) --lw;
        while (fw < next.size() && !hasLetterOrDigit(next[fw].text)) ++fw;
        if (lw == 0 || fw >= next.size()) return;
        const std::vector<std::string> a = normWords(committedTail[lw - 1].text), b = normWords(next[fw].text);
        if (!a.empty() && a == b && next[fw].start - seamSec <= kSeamCopySec)
            next.erase(next.begin(), next.begin() + (std::ptrdiff_t)(fw + 1));
        return;
    }
    size_t drop = 0;
    while (drop < he.size() && he[drop] <= best) ++drop;
    next.erase(next.begin(), next.begin() + (std::ptrdiff_t)drop);
}

// WF, B (TrFileJob.h)
size_t dropAlreadyCommitted(std::vector<FileWord>& fresh, const std::deque<FileWord>& recent) {
    if (fresh.empty() || recent.empty()) return 0;
    size_t dropped = 0;
    // 1. THE SAME AUDIO: DTW times are accurate to a tenth of a second (median 0.06 s) -- a word that
    //    starts before the last committed word ended was heard twice
    if (recent.back().timed) {
        const double lastEnd = recent.back().end;
        size_t k = 0;
        while (k < fresh.size() && fresh[k].timed && fresh[k].start < lastEnd - 0.15) ++k;
        if (k) { fresh.erase(fresh.begin(), fresh.begin() + (std::ptrdiff_t)k); dropped += k; }
        if (fresh.empty()) return dropped;
    }
    // 2. THE SAME TEXT across the SEAM: the new HEAD (up to its first sentence end, 12 words at most)
    //    against the last words committed. Review P5: only there -- a copy heard twice is the end of
    //    one window heard again at the start of the next, so the new head starts within kSeamSec of
    //    the last committed word and the old copy ends within kSeamSec before it; a refrain, a chant
    //    or a call repeated further away is a person saying it again, whatever its rate.
    constexpr double kSeamSec = 6.0;
    const double lastEnd = recent.back().end;
    if (fresh.front().start > lastEnd + kSeamSec) return dropped;
    size_t h = 0;
    while (h < fresh.size() && h < 12) { ++h; if (endsSentence(fresh[h - 1].text)) break; }
    std::vector<AsrWord> hw, rw;
    for (size_t i = 0; i < h; ++i) { AsrWord w; w.text = fresh[i].text; hw.push_back(w); }
    {
        // the seam: the committed words back to kSeamSec before the last one ended -- from the start of
        // the sentence holding the earliest of them (a badly timed old copy is spread over seconds)
        size_t r0 = recent.size();
        while (r0 > 0 && recent[r0 - 1].end > lastEnd - kSeamSec) --r0;
        while (r0 > 0 && !endsSentence(recent[r0 - 1].text) && recent.size() - r0 < 24) --r0;
        for (size_t i = r0; i < recent.size(); ++i) { AsrWord w; w.text = recent[i].text; rw.push_back(w); }
    }
    if (rw.empty()) return dropped;
    std::vector<std::string> ha, ra;
    std::vector<size_t> he, re;
    atomsOf(hw, ha, he);
    atomsOf(rw, ra, re);
    if (ha.size() < 3 || ra.empty()) return dropped;
    // the longest common subsequence of the head's atoms in the recent ones
    const size_t n = ha.size(), m = ra.size();
    std::vector<std::vector<uint16_t>> L(n + 1, std::vector<uint16_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t k = m; k-- > 0;)
            L[i][k] = ha[i] == ra[k] ? (uint16_t)(L[i + 1][k + 1] + 1) : std::max(L[i + 1][k], L[i][k + 1]);
    if ((double)L[0][0] < 0.8 * (double)n) return dropped;
    auto cps = [](const std::string& t) { return (double)(narrowChars(t) + wideChars(t)); };
    double nc = 0.0;
    for (size_t i = 0; i < h; ++i) nc += cps(fresh[i].text);
    const double nRate = nc / std::max(0.1, fresh[h - 1].end - fresh.front().start);
    // The new copy goes when IT is spread far beyond any speech rate (whatever the old one's timing:
    // two badly timed copies are worth no more than one); a well-timed new copy of a badly timed old
    // one stays -- that one is out already, and the viewer gets the sentence where it is said; two
    // plausible copies are a person repeating himself: kept.
    if (nRate < kSlowCps) {
        fresh.erase(fresh.begin(), fresh.begin() + (std::ptrdiff_t)h);
        dropped += h;
    }
    return dropped;
}

// =============================================================================
// CueBuilder
// =============================================================================
void CueBuilder::configure(const CueCfg& c, const EnergyTrack* en) {
    cfg_ = c;
    en_ = en;
    cur_.clear();
    closed_.clear();
    ready_.clear();
    lastEnd_ = -1.0;
    pendAt_ = kNoPend;
    lastHan_ = -1;
    limit_ = kInf;
    droppedCut_ = 0;
    cutWords_ = 0;
    merged_ = 0;
    lastWordEnd_ = -1.0;
}

void CueBuilder::beginRun(double fromSec, double limitSec, bool afterRange) {
    cur_.clear();
    closed_.clear();
    ready_.clear();
    pendAt_ = kNoPend;
    lastHan_ = -1;
    lastWordEnd_ = -1.0;
    // finalise() starts a cue no earlier than lastEnd_ + minGap. After an analysed stretch, whose
    // cues end at the latest where it ends: the usual gap after it. Inside a gap (the run starts on
    // a breath before the playhead, nothing analysed behind it): from `fromSec` itself -- the run
    // that later fills the gap before it ends its cues minGap before `fromSec` (its limit).
    if (fromSec <= 0.0 && !afterRange) lastEnd_ = -1.0;
    else lastEnd_ = afterRange ? fromSec : fromSec - cfg_.minGap;
    limit_ = limitSec;
}

double CueBuilder::snapStart(double t, bool timed) const {
    if (!en_ || en_->hops() == 0) return t;
    const size_t h = EnergyTrack::hopAt(t);
    if (h >= en_->hops()) return t;
    if (!en_->active(h)) {                     // starts in silence: the next onset, within 1.2 s
        const size_t lim = h + (size_t)(1.2 * kFileRate / kFileHop);
        size_t r = h;
        while (r < en_->hops() && r <= lim && !en_->active(r)) ++r;
        return (r < en_->hops() && r <= lim) ? std::max(t, EnergyTrack::timeOf(r)) : t;
    }
    // WF: a DTW start is where the word is (0.06 s on median) -- the onset of the energy run it sits in
    // is a WORSE guess: sentences said back to back are one active run, and snapping back moved a cue's
    // start 0.39 s into the sentence before it (measured: "Je ne comprends pas." left with 130 ms)
    if (timed) return t;
    // inside speech: back to the onset of this run, if it lies within 0.4 s
    const size_t lim = h > 13 ? h - 13 : 0;
    size_t l = h;
    while (l > lim && en_->active(l - 1)) --l;
    if (l > 0 && !en_->active(l - 1)) return std::min(t, EnergyTrack::timeOf(l));
    return t;
}

double CueBuilder::snapEnd(double t) const {
    if (!en_ || en_->hops() == 0) return t;
    size_t h = EnergyTrack::hopAt(t);
    if (h >= en_->hops()) h = en_->hops() - 1;
    if (en_->active(h)) return t;
    // ends in silence: back to the last active hop, within 1.0 s
    const size_t lim = h > 33 ? h - 33 : 0;
    size_t l = h;
    while (l > lim && !en_->active(l)) --l;
    if (en_->active(l)) return std::min(t, EnergyTrack::timeOf(l + 1));
    return t;
}

bool CueBuilder::shouldSplitBefore(const FileWord& w) const {
    if (cur_.empty()) return false;
    const FileWord& p = cur_.back();
    // (A change of SCRIPT is not decided here any more -- see addWords: it splits only after a
    // pause, and never isolates a short run of one script inside the other.)
    const double gap = w.start - p.end;
    if (gap <= 0.0) return false;
    // A sentence's FIRST word or two, alone before a pause, is whisper's timestamp artefact,
    // not a cue: it stamps that word right after the previous full stop while it is really
    // spoken after the pause (measured on bench_en: " The" at 5.20 s, really ~6.2 s, then
    // " dialogue" at 6.23 s -- a gap rule split it into a one-word cue "The"). It stays with
    // what follows, and the cue's start snaps forward onto the real onset (snapStart).
    if (cur_.size() <= 2 && p.end - cur_.front().start < 0.6) return false;
    double silent = en_ ? en_->inactiveSec(p.end, w.start) : gap;
    // WF: between two DTW-timed words the GAP is the pause -- under a film's music the energy never
    // says "silent", and "Soit tu restes, soit on te laisse" + 1.1 s + "La nuit..." became one cue
    if (p.timed && w.timed) silent = std::max(silent, gap);
    if (gap >= cfg_.gapSec && silent >= cfg_.gapSec - 0.2) return true;
    if (gap >= cfg_.softGapSec && silent >= cfg_.softGapSec - 0.05) {
        const double dur = p.end - cur_.front().start;
        const std::string t = joinText(cur_);
        if (dur >= cfg_.softAfterSec || narrowChars(t) >= cfg_.softLatin || wideChars(t) >= cfg_.softHan)
            return true;
    }
    return false;
}

void CueBuilder::closeCur() {
    if (cur_.empty()) { pendAt_ = kNoPend; return; }
    // A language change still PENDING when the cue closes: the new script ran to the end of the
    // cue -- it is not embedded between two runs of the other one -- so it is a turn of its own.
    if (pendAt_ != kNoPend) commitPending();
    closed_.push_back(std::move(cur_));
    cur_.clear();
}

void CueBuilder::commitPending() {
    const size_t at = pendAt_;
    pendAt_ = kNoPend;
    if (at == 0 || at >= cur_.size()) return;
    std::vector<FileWord> head(cur_.begin(), cur_.begin() + (std::ptrdiff_t)at);
    cur_.erase(cur_.begin(), cur_.begin() + (std::ptrdiff_t)at);
    closed_.push_back(std::move(head));
}

void CueBuilder::splitOverLimit() {
    while (cur_.size() >= 2) {
        const std::string t = joinText(cur_);
        const double dur = cur_.back().end - cur_.front().start;
        if (dur <= cfg_.maxSec && narrowChars(t) <= cfg_.maxLatin && wideChars(t) <= cfg_.maxHan) return;
        size_t k = (size_t)-1;
        // WF, E: never a FRAGMENT on either side. A comma, a pause, and BALANCE score the places that leave
        // at least a fifth of the text on each side (a third, without a comma): comma +3, a pause 4 x its
        // seconds (<= 1), minus 2 x the distance from an even split. Before WF the largest gap was the FIRST
        // of equals, and fluent speech (every gap 0 under DTW) was cut after its first word or two ("Je
        // vais" | "mettre..."); the old rule stays the fallback when no place qualifies.
        {
            const int total = std::max(1, narrowChars(t) + wideChars(t));
            int head = 0;
            double best = -1e300;
            for (size_t i = 0; i + 1 < cur_.size(); ++i) {
                head += narrowChars(cur_[i].text) + wideChars(cur_[i].text);
                const bool comma = endsWithComma(cur_[i].text);
                const int tail = total - head;
                if (comma ? (head * 5 < total || tail * 5 < total) : (head * 3 < total || tail * 3 < total)) continue;
                const double gap = std::max(0.0, std::min(1.0, cur_[i + 1].start - cur_[i].end));
                const double score = (comma ? 3.0 : 0.0) + 4.0 * gap
                                   - 2.0 * std::fabs((double)head / (double)total - 0.5);
                if (score > best) { best = score; k = i; }
            }
        }
        if (k == (size_t)-1) {
            double best = -1e300;
            for (size_t i = 0; i + 1 < cur_.size(); ++i) {
                const double g = cur_[i + 1].start - cur_[i].end;
                if (g > best) { best = g; k = i; }
            }
        }
        std::vector<FileWord> head(cur_.begin(), cur_.begin() + (std::ptrdiff_t)(k + 1));
        cur_.erase(cur_.begin(), cur_.begin() + (std::ptrdiff_t)(k + 1));
        closed_.push_back(std::move(head));
        if (pendAt_ != kNoPend) pendAt_ = pendAt_ > k ? pendAt_ - (k + 1) : kNoPend;
    }
}

void CueBuilder::addWords(const std::vector<FileWord>& words) {
    for (const FileWord& w0 : words) {
        FileWord w = w0;
        // WORDS STAMPED INTO SILENCE. whisper's token timestamps push the last words of a segment
        // towards the segment's end, i.e. into the pause after the utterance (measured: "for it"
        // of "...are compensating for it" stamped 0.7 s into the silence after the speaker
        // stopped). A word whose whole span is silent and that CONTINUES an unfinished sentence
        // is put back right after the word before it; a sentence's first word is left to the
        // leading-word rule and snapStart (shouldSplitBefore).
        // (WF: not for a DTW-timed word -- its place is measured, not guessed)
        if (en_ && !w.timed && !cur_.empty() && hasLetterOrDigit(w.text) && !endsSentence(cur_.back().text)) {
            const double dur = w.end - w.start;
            const FileWord& p = cur_.back();
            if (w.start > p.end + 0.05 && dur < 1.0
                && en_->inactiveSec(w.start, std::max(w.end, w.start + 0.03)) >= std::max(0.03, dur - 0.03)
                && !(cfg_.hanPair && (hanChars(w.text) > 0) != (hanChars(p.text) > 0))) {
                w.start = p.end;
                w.end   = p.end + std::min(dur, 0.3);
            }
        }
        if (!hasLetterOrDigit(w.text)) {
            // Punctuation alone rides on the word before it and adds no time: whisper once hung
            // a "." word over 5 s of music (10/10).
            if (!cur_.empty()) {
                cur_.back().text += w.text;
                if (endsSentence(cur_.back().text)) closeCur();
            }
            continue;
        }
        // A CHANGE OF LANGUAGE (WB, after the review). whisper emits a word per TOKEN, and Chinese
        // studio talk is full of English: "U+628AreverbU+5173,6389", "OKU+FF0C,6211,4EEC,5F00,59CB,5427" -- splitting on every
        // Han <-> Latin change cut those into three cues and sent "reverb" to the translator as
        // English. So a change of script splits only after a PAUSE (>= softGapSec of silence),
        // and even then lazily: the split waits until the new script has run for more than
        // kEmbeddedRun tokens; if the first script comes back sooner, that run was EMBEDDED in
        // the sentence and stays in it. (A full stop before the change closes the cue anyway.)
        if (cfg_.hanPair && !cur_.empty()) {
            const int ps = scriptOfText(cur_.back().text), wsc = scriptOfText(w.text);
            if (ps && wsc && ps != wsc) {
                if (pendAt_ != kNoPend) {
                    pendAt_ = kNoPend;                    // back within the run: it was embedded
                } else {
                    const FileWord& p = cur_.back();
                    const double gap = w.start - p.end;
                    double silent = en_ ? en_->inactiveSec(p.end, w.start) : gap;
                    if (p.timed && w.timed) silent = std::max(silent, gap);   // WF: the DTW gap is the pause
                    if (gap >= cfg_.softGapSec && silent >= cfg_.softGapSec - 0.05) pendAt_ = cur_.size();
                }
            }
        }
        if (shouldSplitBefore(w)) closeCur();
        cur_.push_back(w);
        if (pendAt_ != kNoPend && cur_.size() - pendAt_ > kEmbeddedRun) commitPending();
        // The limits FIRST, then the full stop: a sentence whose LAST word takes it past two
        // lines is still split at its comma, not closed as one over-long cue.
        splitOverLimit();
        if (endsSentence(w.text)) closeCur();
    }
    finalise(false, 0.0);
}

void CueBuilder::windowBoundary(double pauseSec) {
    if (pauseSec >= cfg_.gapSec) closeCur();
    finalise(false, 0.0);
}

void CueBuilder::finish(double mediaEndSec) {
    closeCur();
    finalise(true, mediaEndSec);
}

void CueBuilder::finalise(bool all, double mediaEndSec) {
    while (!closed_.empty()) {
        // The LAST closed cue waits for its successor (its reading time must not run into it)
        // unless this is the end of the file.
        if (closed_.size() == 1 && cur_.empty() && !all) break;
        std::vector<FileWord> ws = std::move(closed_.front());
        closed_.pop_front();
        if (ws.empty()) continue;
        // A FRAGMENT is not a subtitle: at most two words lasting under 0.35 s, with a successor
        // less than 0.6 s away, joins that successor instead of flashing on screen for a few
        // frames -- in the same script, or (WB) as a short run of the other script between two of
        // this one, which the language-change rule never isolates either. And never when the
        // merged cue would break the two-line limits (WB, after the review).
        // WF, E: and a LONGER fragment too -- at most two words, or under fragSec of speech: one that
        // does not end its sentence joins a successor less than fragGapSec away; failing that, any
        // fragment joins the cue before it when that one is still here, does NOT end its sentence
        // ("...soit on te" + "laisse.") and is as close. (On the film, before WF: 80 one-word cues and
        // 26 under 0.7 s in 25 min -- most of them pieces of words, which C now keeps whole.)
        const bool   sentEnd = endsSentence(ws.back().text);
        const double spoken  = ws.back().end - ws.front().start;
        const bool   tiny    = ws.size() <= 2 && spoken < 0.35;
        const bool   frag    = ws.size() <= 2 || spoken < cfg_.fragSec;
        if (tiny || (frag && !sentEnd)) {
            std::vector<FileWord>* succ = !closed_.empty() ? &closed_.front()
                                        : (!cur_.empty() ? &cur_ : nullptr);
            const double nearSec = tiny ? 0.6 : cfg_.fragGapSec;
            // (review P6: never into another speaker's turn, nor across two windows' languages -- the
            // merged cue takes ONE language label, the router's)
            if (succ && !succ->empty() && succ->front().start - ws.back().end < nearSec
                && succ->front().lang == ws.front().lang && !opensTurn(succ->front().text)) {
                const bool fH = hanChars(joinText(ws)) > 0, sH = hanChars(joinText(*succ)) > 0;
                const bool embedded = fH != sH && lastHan_ == (sH ? 1 : 0);
                std::vector<FileWord> merged(ws);
                merged.insert(merged.end(), succ->begin(), succ->end());
                const std::string mt = joinText(merged);
                const bool fits = narrowChars(mt) <= cfg_.maxLatin && wideChars(mt) <= cfg_.maxHan
                               && merged.back().end - merged.front().start <= cfg_.maxSec;
                // (the embedded-run exception is the tiny rule's only: a longer piece of the other script
                // after a pause is a turn of its own -- "U+597D,7684,FF0C" then "Perfect thanks.")
                if ((fH == sH || (tiny && embedded)) && fits) {
                    // (review L3: a language change pending in cur_ moves with its words)
                    if (succ == &cur_ && pendAt_ != kNoPend) pendAt_ += ws.size();
                    succ->insert(succ->begin(), ws.begin(), ws.end());
                    if (!tiny) ++merged_;
                    continue;
                }
            }
        }
        if (frag && !ready_.empty() && lastWordEnd_ >= 0.0 && !endsSentence(ready_.back().text)
            && ws.front().start - lastWordEnd_ < cfg_.fragGapSec
            && (hanChars(ready_.back().text) > 0) == (hanChars(joinText(ws)) > 0)
            && ready_.back().lang == ws.front().lang && !opensTurn(ws.front().text)) {
            FileCue& p = ready_.back();
            const std::string add = joinText(ws);
            const bool space = !ws.front().text.empty() && ws.front().text[0] == ' ';
            const std::string mt = p.text + (space ? " " : "") + add;
            const bool noCut = std::none_of(ws.begin(), ws.end(), [](const FileWord& w) { return w.cut; });
            if (noCut && narrowChars(mt) <= cfg_.maxLatin && wideChars(mt) <= cfg_.maxHan
                && ws.back().end - p.start <= cfg_.maxSec) {
                // the merged cue's end, by the same rules as any cue's (below)
                const double en = snapEnd(ws.back().end);
                double nextStart = kInf;
                if (!closed_.empty() && !closed_.front().empty()) nextStart = snapStart(closed_.front().front().start, closed_.front().front().timed);
                else if (!cur_.empty()) nextStart = snapStart(cur_.front().start, cur_.front().timed);
                double e2 = std::max(en + cfg_.readExtend, p.start + std::max(cfg_.minSec, readSec(mt)));
                if (nextStart < kInf) e2 = std::min(e2, nextStart - cfg_.minGap);
                if (all && mediaEndSec > 0.0) e2 = std::min(e2, std::max(mediaEndSec, ws.back().end));
                if (limit_ < kInf) e2 = std::min(e2, limit_ - cfg_.minGap);
                if (e2 >= p.end) {
                    p.text = mt;
                    p.end = e2;
                    lastEnd_ = e2;
                    lastWordEnd_ = ws.back().end;
                    ++merged_;
                    continue;
                }
            }
        }
        // WF, E: a line that cannot stay up minSec -- the next one starts too soon: a rapid exchange, a list
        // read out ("Avery." "Jones." "Saltaise." "Justice.") -- SHARES the next one's cue when the two fit
        // together (the two-line limits, the same script): what a subtitler does with "- Ouais. - Non."
        {
            std::vector<FileWord>* succ = !closed_.empty() ? &closed_.front() : (!cur_.empty() ? &cur_ : nullptr);
            // (review P6: never across two windows' languages -- one cue, one label for the router. ACROSS A
            // SPEAKER'S TURN it does share: refusing it was measured on the film -- 17 more cues under 1 s, down
            // to 0.2 s ("- Non." / "- OK." flashing between two lines). The subtitle convention for a rapid
            // exchange IS one subtitle, a dash per speaker; a cue is one line (WC), and the next line's dash,
            // kept in the text ("- Pas maintenant. - Est-ce qu'ils..."), is the turn's mark.)
            if (succ && !succ->empty() && succ->front().start - ws.back().end < cfg_.fragGapSec
                && succ->front().lang == ws.front().lang) {
                const double st0 = std::max(snapStart(ws.front().start, ws.front().timed), lastEnd_ + cfg_.minGap);
                const double ns0 = snapStart(succ->front().start, succ->front().timed);
                if (ns0 - cfg_.minGap - st0 < cfg_.minSec) {
                    const bool fH = hanChars(joinText(ws)) > 0, sH = hanChars(joinText(*succ)) > 0;
                    std::vector<FileWord> merged(ws);
                    merged.insert(merged.end(), succ->begin(), succ->end());
                    const std::string mt = joinText(merged);
                    const bool noCut = std::none_of(ws.begin(), ws.end(), [](const FileWord& w) { return w.cut; });
                    if (fH == sH && noCut && narrowChars(mt) <= cfg_.maxLatin && wideChars(mt) <= cfg_.maxHan
                        && merged.back().end - merged.front().start <= cfg_.maxSec) {
                        if (succ == &cur_ && pendAt_ != kNoPend) pendAt_ += ws.size();   // (review L3)
                        succ->insert(succ->begin(), ws.begin(), ws.end());
                        ++merged_;
                        continue;
                    }
                }
            }
        }
        // v2: a word CUT at the run's limit (heard only up to it -- the stretch beyond has it whole)
        // is dropped; its cue keeps the rest (review P1: v2 first dropped the WHOLE cue, i.e. every
        // sentence sharing it), and goes only when nothing readable is left.
        {
            size_t keep = 0;
            for (FileWord& w : ws) if (!w.cut) ws[keep++] = std::move(w);
            if (keep < ws.size()) {
                cutWords_ += (uint32_t)(ws.size() - keep);
                ws.resize(keep);
                if (ws.empty() || !hasLetterOrDigit(joinText(ws))) { ++droppedCut_; continue; }
            }
        }
        FileCue c;
        c.text = joinText(ws);
        c.lang = ws.front().lang;
        if (!hasLetterOrDigit(c.text) || isAnnotationOnly(c.text)) continue;   // never a subtitle
        lastHan_ = hanChars(c.text) > 0 ? 1 : 0;
        double st = snapStart(ws.front().start, ws.front().timed);
        double en = snapEnd(ws.back().end);
        if (en < st) en = st;
        double nextStart = kInf;
        if (!closed_.empty() && !closed_.front().empty()) nextStart = snapStart(closed_.front().front().start, closed_.front().front().timed);
        else if (!cur_.empty()) nextStart = snapStart(cur_.front().start, cur_.front().timed);
        if (st < lastEnd_ + cfg_.minGap) st = lastEnd_ + cfg_.minGap;
        // WF, E: at least minSec AND its reading time, into the gap after it (capped just below)
        double e2 = std::max(en + cfg_.readExtend, st + std::max(cfg_.minSec, readSec(c.text)));
        if (nextStart < kInf) e2 = std::min(e2, nextStart - cfg_.minGap);
        if (all && mediaEndSec > 0.0) e2 = std::min(e2, std::max(mediaEndSec, ws.back().end));
        // v2: never into the stretch past the run's limit; a cue with no room left before it is
        // one that starts on the limit itself -- dropped like a cut one.
        if (limit_ < kInf) {
            e2 = std::min(e2, limit_ - cfg_.minGap);
            if (e2 < st + 0.05) { ++droppedCut_; continue; }
        }
        if (e2 < st + 0.05) e2 = st + 0.05;
        c.start = st;
        c.end = e2;
        lastEnd_ = e2;
        lastWordEnd_ = ws.back().end;
        ready_.push_back(std::move(c));
    }
}

double CueBuilder::readSec(const std::string& t) const {
    int all = 0, hangul = 0;
    for (size_t i = 0; i < t.size();) { if (isHangulCp(nextCp(t, i))) ++hangul; ++all; }
    const int wide = wideChars(t) - hangul;
    // (review L2: Korean is read by spaced words -- a Hangul block at 12 a second, the usual Korean norm,
    // between the alphabets' 17 and Han's 7)
    return (double)(all - wide - hangul) / cfg_.readCps + (double)wide / cfg_.readCpsWide + (double)hangul / 12.0;
}

bool CueBuilder::pop(FileCue& c) {
    if (ready_.empty()) return false;
    c = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

double CueBuilder::holdFrom() const {
    double t = kInf;
    if (!closed_.empty() && !closed_.front().empty()) t = std::min(t, closed_.front().front().start);
    if (!cur_.empty()) t = std::min(t, cur_.front().start);
    return t;
}

double CueBuilder::holdFromStrict() const {
    const double t = holdFrom();
    if (!(t < kInf)) return t;
    // snapStart may move a cue back by up to 13 hops (0.39 s) onto its onset -- and the noise floor
    // that decides "onset" can still move -- so the bound is the word's start less that, and never
    // before where finalise() would put any cue anyway.
    return std::max(t - 0.45, lastEnd_ + cfg_.minGap);
}

} // namespace pbt

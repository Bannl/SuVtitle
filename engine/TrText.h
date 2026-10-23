// =============================================================================
// SuVtitle - engine/TrText.h   (the text helpers the FILE job cuts subtitles with)
//
// SUVTITLE SPLIT (2026-10-23): ported VERBATIM from AudioPB's PatchBus repo, translator/TrText.h at commit
// 641f1aa (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Only this header and the comments' non-ASCII
// characters changed (house rule: ASCII-only C++). "The live engine" below is AudioPB's, which stayed there.
//
// MOVED VERBATIM out of main_translator.cpp (wave WB), where the live commit engine (W3)
// grew them. The FILE job (TrFileJob.cpp) cuts subtitles out of the same timestamped
// words and must normalise, compare and join them EXACTLY the way the live engine does --
// a second copy of "what counts as the same word" would be a second opinion, and the two
// would drift. Header-only and inline: these are a few dozen lines with no state.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TrEngine.h"     // AsrWord

namespace pbt {

// UTF-8, one codepoint at a time. A local four-liner rather than an export from
// TrEngine.cpp: that file is not a string library and should not become one.
inline uint32_t nextCp(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return c; }
    uint32_t cp = 0;
    int n = 0;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 3; }
    else { ++i; return 0xFFFDu; }
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
    return cp;
}
inline bool isCjkCp(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)
        || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}
// Kana and Hangul (WB, after the review): written like Han -- no spaces between words -- so,
// like Han, ONE TOKEN PER CHARACTER. U+30FB (U+30FB) is punctuation and is not one of them.
inline bool isKanaHangulCp(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF && cp != 0x30FB) || (cp >= 0x31F0 && cp <= 0x31FF)
        || (cp >= 0xFF66 && cp <= 0xFF9F) || (cp >= 0xAC00 && cp <= 0xD7AF)
        || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}
// The other ALPHABETS (U+0370-1FFF: Greek, Cyrillic, Armenian, Hebrew, Arabic, the Indic
// scripts, Georgian...): letters of space-separated words, exactly like Latin. Cyrillic and Greek
// capitals are folded, as A-Z are.
inline uint32_t foldOtherLetter(uint32_t cp) {
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;            // U+0410-U+042F -> U+0430-U+044F
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;            // U+0400-U+040F -> U+0450-U+045F
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;   // U+0391-U+03A9 -> U+03B1-U+03C9
    return cp;
}
inline void appendCp(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Words -> the tokens two decodes can be expected to AGREE on, plus, per word, how many
// tokens the words up to and including it produced.
//
// Latin: one token per word, lowercased, punctuation and spaces stripped -- so "Good," and
// " good" are the same evidence. CJK: ONE TOKEN PER CHARACTER, and that is the trap this
// exists for: whisper's idea of a Chinese "word" moves between two decodes of the same
// audio (U+4ECA,5929 / U+4ECA + U+5929) while the characters never do, so comparing its words would find
// almost nothing stable in Chinese and the engine would silently only work in English.
// A word that normalises to nothing (a lone comma) produces no token and rides along with
// its neighbours, which is exactly what it is.
inline void atomsOf(const std::vector<AsrWord>& words, std::vector<std::string>& atoms,
                    std::vector<size_t>& atomEnd) {
    atoms.clear();
    atomEnd.assign(words.size(), 0);
    std::string run;
    for (size_t w = 0; w < words.size(); ++w) {
        const std::string& t = words[w].text;
        size_t i = 0;
        run.clear();
        while (i < t.size()) {
            const uint32_t cp = nextCp(t, i);
            if (isCjkCp(cp) || isKanaHangulCp(cp)) {
                if (!run.empty()) { atoms.push_back(run); run.clear(); }
                std::string one;
                appendCp(one, cp);
                atoms.push_back(one);
            } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
                run += (char)cp;
            } else if (cp >= 'A' && cp <= 'Z') {
                run += (char)(cp + 32);
            } else if (cp >= 0x00C0 && cp <= 0x024F) {
                appendCp(run, cp);      // accented Latin: kept, and NOT case-folded (the
                                        // fold is a table we do not need to be right about)
            } else if (cp >= 0x0370 && cp <= 0x1FFF) {
                // WB (after the review): Cyrillic, Greek, Arabic, Hebrew... are LETTERS. Before
                // this a Russian line normalised to NOTHING, and every rule built on these tokens
                // -- loop detection, repeated segments, the commit agreement -- silently never
                // fired outside Latin and Han. No en / zh text holds these code points.
                appendCp(run, foldOtherLetter(cp));
            } else if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) {
                // /!\ WHITESPACE ENDS A TOKEN, punctuation does NOT, and the difference is
                // not cosmetic. Whisper hands us one word at a time, so within a word the
                // only thing between letters is punctuation -- and "don't" must normalise
                // to the same token as "dont", or two decodes that disagree about an
                // apostrophe would agree about nothing after it. But the SAME function is
                // fed whole sentences by the selftest, and there a space is the only word
                // boundary there is: without this branch a sentence became ONE token and
                // the coverage assertion compared one word against one word, for ever.
                if (!run.empty()) { atoms.push_back(run); run.clear(); }
            }
            // everything else -- punctuation, CJK punctuation, symbols -- is not evidence
        }
        if (!run.empty()) atoms.push_back(run);
        atomEnd[w] = atoms.size();
    }
}

// The SAME normalisation, applied to a plain string. The selftest compares transcripts
// that came out of different calls (a committed line against a one-shot of the whole
// utterance), and it must compare them exactly the way the engine compares drafts --
// otherwise it would be asserting on a second opinion about what "the same word" means.
inline std::vector<std::string> normWords(const std::string& s) {
    std::vector<AsrWord> one(1);
    one[0].text = s;
    std::vector<std::string> atoms;
    std::vector<size_t>      atomEnd;
    atomsOf(one, atoms, atomEnd);
    return atoms;
}

inline size_t commonPrefixLen(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
}

// Does this word END a sentence? The eight marks the two languages actually use, read
// through any closing quote or bracket that follows them.
//
// /!\ It says yes to "Mr." and to "3.5" as well, and that is accepted rather than fought:
// the cost of a false positive here is a sentence split in two on screen, with every word
// still present and in order -- against the cost of a missed one, which is the delay this
// whole wave is about. The lookahead below is what stops it being frequent.
inline bool endsSentence(const std::string& word) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < word.size();) cps.push_back(nextCp(word, i));
    while (!cps.empty()) {
        const uint32_t c = cps.back();
        const bool skippable = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000
                                || c == '"' || c == '\'' || c == ')' || c == ']' || c == '}'
                                || c == 0x201D || c == 0x2019 || c == 0x300D || c == 0x300F
                                || c == 0xFF09 || c == 0x00BB);
        if (!skippable) break;
        cps.pop_back();
    }
    if (cps.empty()) return false;
    const uint32_t c = cps.back();
    return c == '.' || c == '?' || c == '!' || c == ';'
        || c == 0x3002 /*U+3002*/ || c == 0xFF1F /*U+FF1F*/ || c == 0xFF01 /*U+FF01*/ || c == 0xFF1B /*U+FF1B*/;
}

// Words [from..to] as they were spoken. Whisper's Latin words carry their own leading
// space and its Chinese ones carry none, so concatenation IS the sentence -- no separator
// to choose, and no separator to get wrong in the script that does not want one.
inline std::string joinWords(const std::vector<AsrWord>& w, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i <= to && i < w.size(); ++i) s += w[i].text;
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

} // namespace pbt

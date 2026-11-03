// =============================================================================
// SuVtitle - app/TrTranscript.h   (the TRANSCRIPT: pieces, speakers, turns, stamps -- U3)
//
// U3 (2026-11-02, docs/PROGRESS_U3.md -- the user: "un deuxieme bouton pour la Transcription", "un fichier propre utilisable
// par des transcripteurs professionnels", "les locuteurs reconnus a la voix", "un timecode a chaque prise de parole"). A file's
// transcript is its ANALYSIS' original text (the cues' `src`: the same analysis Translate makes) cut into PIECES, named by the
// voice pass (FVOICE / FSPK, engine/main_translator.cpp) and grouped into TURNS -- what the Transcript tab shows and the .docx
// holds. PURE (no disk, no window): TrPlayer.h and the bridge call it; tests/cpp/app_smoke.cpp [10] drives it.
//   * PIECES: a cue, or one LINE of a cue that holds a dialogue exchange -- its dialogue dashes ("- I agree. - So let's move
//     on.": whisper writes them when it hears a new speaker, the analysis' rapid-exchange rule keeps them in a shared cue).
//     A dash ('-', U+2013, U+2014) that OPENS A LINE: at the start, TrFileJob.cpp's opensTurn rule; inside the text, after a
//     space and not followed (after one optional space) by a lowercase letter or a dash (dashParts) -- so "Est-ce",
//     "va-t-il", "well-known", "it was - how to say - hard" and "going to --" never cut. A line's
//     sub-span is the cue's time shared by reading time (17 narrow / 7 wide characters a second, the analysis' CueCfg); the
//     engine moves each boundary to the pause between the two voices. A piece after another in the same cue, or a piece opened
//     by a dash right after a piece opened by a dash, is a KNOWN CHANGE of speaker (FVOICE's `changes`).
//   * SPEAKERS: each piece takes the voice segment it overlaps MOST (robust to the subtitle editor's edits and to a
//     re-analysis); none -> the previous piece's (the first ones: the next one's); a known change never keeps its
//     predecessor's speaker (the most recent other one, else no name). A speaker's display name is the user's rename, else
//     "Speaker <n>" (n = the voice pass' number, stable); two speakers given the SAME name (trimmed, ASCII case folded) are ONE
//     person: their consecutive pieces make one turn.
//   * TURNS: a new turn at every change of name; for one name, also after a pause >= 6 s or once it lasts 90 s (at its next
//     sentence end); with no names at all, a paragraph per pause >= 2 s (and the 90 s rule). Its stamp is its first piece's
//     start, through SourceTimecode.h (file-relative, or the file's SOURCE timecode). Its text: its pieces joined (no space
//     between two CJK ends).
// ASCII only, /W4 clean.
// =============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "TrPipe.h"
#include "TrFileStore.h"
#include "SourceTimecode.h"

namespace pbtrs {

namespace tp = pbt::trpipe;

struct Piece {
    uint64_t    s = 0, e = 0;        // ms, the media timeline (a dash line: its share of the cue)
    std::string text;                // its words (a dash line: the dash and its space removed)
    std::string lang;                // its cue's language
    size_t      cue = 0;             // its cue (canonical order)
    bool        dash = false;        // opened by a dialogue dash
    bool        change = false;      // a KNOWN change of speaker from the piece before it
};
struct Turn {
    uint64_t    s = 0, e = 0;        // ms: its first piece's start, its last piece's end
    uint32_t    spk = 0;             // its FIRST piece's speaker number (0 = no name) -- what a rename of this turn names
    std::string name;                // what it shows ("" = no name)
    std::string stamp;               // "[00:01:23]" / "[10:00:03:12]"
    std::string text;
    std::string lang;                // its first piece's language
};
struct TurnCfg {
    uint64_t speakerPauseMs = 6000;  // one speaker, a pause this long: a new turn (its own stamp)
    uint64_t paraMaxMs      = 90000; // one turn this long: cut at its next sentence end
    uint64_t noNamePauseMs  = 2000;  // no names: a paragraph per pause this long
};

// ---- UTF-8 ---------------------------------------------------------------------------------------------------------------
inline uint32_t cpAt(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp = c;
    size_t n = 0;
    if (c >= 0xF0) { cp = c & 0x07; n = 3; } else if (c >= 0xE0) { cp = c & 0x0F; n = 2; } else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
    ++i;
    for (size_t k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}
inline bool isWideCp(uint32_t cp) {          // written without spaces: Han, kana, Hangul syllables, CJK / full-width marks
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}
inline bool isLowerCp(uint32_t cp) {         // enough of Unicode's lowercase for the dash rule (Latin, Greek, Cyrillic)
    return (cp >= 'a' && cp <= 'z') || (cp >= 0xDF && cp <= 0xFF && cp != 0xF7)
        || (cp >= 0x100 && cp <= 0x17F && (cp & 1) == 1) || (cp >= 0x3AC && cp <= 0x3CE) || (cp >= 0x430 && cp <= 0x45F);
}
inline bool isDashCp(uint32_t cp) { return cp == '-' || cp == 0x2013 || cp == 0x2014; }
inline uint32_t lastCp(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) --i;
    if (i == 0) return 0;
    --i;
    return cpAt(s, i);
}
inline uint32_t firstCp(const std::string& s) { size_t i = 0; return s.empty() ? 0 : cpAt(s, i); }
inline std::string trimSpaces(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
// Two texts joined as the documents join them: a space, unless both sides are written without spaces (CJK).
inline void joinInto(std::string& acc, const std::string& next) {
    if (next.empty()) return;
    if (!acc.empty() && !(isWideCp(lastCp(acc)) && isWideCp(firstCp(next)))) acc += ' ';
    acc += next;
}
inline int narrowWide(const std::string& s, int& wide) {
    int narrow = 0;
    wide = 0;
    for (size_t i = 0; i < s.size();) { const uint32_t cp = cpAt(s, i); if (cp == ' ') continue; if (isWideCp(cp)) ++wide; else ++narrow; }
    return narrow;
}
inline bool endsSentence(const std::string& s) {
    std::string t = trimSpaces(s);
    while (!t.empty() && (t.back() == '"' || t.back() == '\'' || t.back() == ')')) t.pop_back();
    const uint32_t c = lastCp(t);
    return c == '.' || c == '!' || c == '?' || c == 0x2026 || c == 0x3002 || c == 0xFF01 || c == 0xFF1F;
}

// ---- THE LINES of a cue (its dialogue dashes) -------------------------------------------------------------------------
struct DashPart { std::string text; bool dash = false; };
// A dash OPENS A LINE: as the text's first character, TrFileJob.cpp's opensTurn (unless a lowercase letter or another dash
// follows it at once: "-oui", "--"); inside the text, after a space and followed -- after one optional space -- by something
// that is not a lowercase letter nor a dash ("I agree. - So": yes; "it was - how to say - hard", "going to --", a dash at the
// very end: no). Never inside a word ("Est-ce", "well-known").
inline std::vector<DashPart> dashParts(const std::string& text) {
    std::vector<DashPart> out;
    std::vector<size_t> cuts;                // byte offsets of the dashes that open a line
    std::vector<size_t> after;               // where that line's words start (the dash and one space skipped)
    bool leading = true;                     // only spaces so far
    for (size_t i = 0; i < text.size();) {
        const size_t at = i;
        const uint32_t cp = cpAt(text, i);
        if (cp == ' ') continue;
        const bool lead = leading;
        leading = false;
        if (!isDashCp(cp)) continue;
        if (!lead && text[at - 1] != ' ') continue;          // inside a word
        size_t j = i;
        if (!lead && j < text.size() && text[j] == ' ') ++j;
        if (j < text.size()) {
            size_t q = j;
            const uint32_t nx = cpAt(text, q);
            if (isLowerCp(nx) || isDashCp(nx)) continue;
        } else if (!lead) {
            continue;                                        // a dash ending the text opens nothing
        }
        if (lead && j < text.size() && text[j] == ' ') ++j;
        cuts.push_back(at);
        after.push_back(j);
    }
    size_t from = 0;
    bool dash = false;
    for (size_t c = 0; c <= cuts.size(); ++c) {
        const size_t to = c < cuts.size() ? cuts[c] : text.size();
        const std::string t = trimSpaces(text.substr(from, to - from));
        if (!t.empty()) out.push_back(DashPart{ t, dash });
        if (c < cuts.size()) { from = after[c]; dash = true; }
    }
    if (out.empty()) out.push_back(DashPart{ trimSpaces(text), false });
    return out;
}

// ---- THE PIECES of a transcript (from the analysis' cues, in any order) ------------------------------------------------
inline std::vector<Piece> piecesOf(std::vector<tp::WireCue> cues) {
    std::stable_sort(cues.begin(), cues.end(), [](const tp::WireCue& a, const tp::WireCue& b) { return a.startMs < b.startMs; });
    std::vector<Piece> out;
    for (size_t c = 0; c < cues.size(); ++c) {
        const tp::WireCue& q = cues[c];
        if (trimSpaces(q.src).empty()) continue;
        std::vector<DashPart> parts = dashParts(q.src);
        const uint64_t len = q.endMs > q.startMs ? q.endMs - q.startMs : 0;
        if (parts.size() > 1 && len < parts.size()) {        // no room for a millisecond each: one piece
            std::string all;
            for (const auto& p : parts) joinInto(all, p.text);
            parts.assign(1, DashPart{ all, parts.front().dash });
        }
        std::vector<double> w;
        double total = 0.0;
        for (const auto& p : parts) {
            int wide = 0;
            const int narrow = narrowWide(p.text, wide);
            const double sec = narrow / 17.0 + wide / 7.0 + 0.05;
            w.push_back(sec);
            total += sec;
        }
        double acc = 0.0;
        for (size_t k = 0; k < parts.size(); ++k) {
            Piece p;
            p.s = k == 0 ? q.startMs : out.back().e;
            acc += w[k];
            p.e = k + 1 == parts.size() ? q.endMs : q.startMs + (uint64_t)((double)len * acc / total);
            if (k + 1 < parts.size()) {
                const uint64_t lo = p.s + 1, hi = q.endMs - (uint64_t)(parts.size() - k - 1);
                p.e = std::max(lo, std::min(p.e, hi));
            }
            p.text = parts[k].text;
            p.lang = q.lang;
            p.cue = c;
            p.dash = parts[k].dash;
            p.change = k > 0 || (parts[k].dash && !out.empty() && out.back().dash);
            out.push_back(p);
        }
    }
    return out;
}
// FVOICE's two fields for those pieces: the spans (a piece of no length is not sent: it takes its neighbour's speaker) and
// the changes, as indexes into what is sent. `sent` = each piece's index in the FVOICE (-1: not sent).
inline void voiceFields(const std::vector<Piece>& pieces, std::string& spans, std::string& changes, std::vector<int>& sent) {
    std::vector<tp::MsRange> r;
    std::vector<uint32_t> ch;
    sent.assign(pieces.size(), -1);
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].e <= pieces[i].s || (!r.empty() && pieces[i].s < r.back().e)) continue;
        if (pieces[i].change && !r.empty()) ch.push_back((uint32_t)r.size());
        sent[i] = (int)r.size();
        r.push_back(tp::MsRange{ pieces[i].s, pieces[i].e });
    }
    spans = tp::packRanges(r);
    changes = tp::packChanges(ch);
}

// ---- THE SPEAKERS of the pieces ---------------------------------------------------------------------------------------
// Each piece's speaker: the voice segment it overlaps most; none -> the previous piece's (the first ones: the next one's);
// a known change never keeps its predecessor's (the most recent other speaker, else 0 = no name). `unmatchedMs` = the
// time of the pieces no segment overlaps (a re-analysis found speech the voice pass never heard).
inline std::vector<uint32_t> speakersOf(const std::vector<Piece>& pieces, const std::vector<pbtrf::VoiceSeg>& segs,
                                        uint64_t* unmatchedMs = nullptr, uint64_t* totalMs = nullptr) {
    std::vector<uint32_t> spk(pieces.size(), 0);
    std::vector<char> matched(pieces.size(), 0);
    uint64_t un = 0, tot = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        tot += p.e > p.s ? p.e - p.s : 0;
        auto it = std::lower_bound(segs.begin(), segs.end(), p.s, [](const pbtrf::VoiceSeg& g, uint64_t v) { return g.e <= v; });
        uint64_t best = 0;
        for (; it != segs.end() && it->s < std::max(p.e, p.s + 1); ++it) {
            const uint64_t ov = std::min(it->e, std::max(p.e, p.s + 1)) - std::max(it->s, p.s);
            if (it->e > p.s && ov > best && it->spk) { best = ov; spk[i] = it->spk; }
        }
        matched[i] = best > 0;
        if (!matched[i]) un += p.e > p.s ? p.e - p.s : 0;
    }
    // in order: an unheard piece takes the previous one's (as that one finally is), then the known-change rule
    for (size_t i = 1; i < pieces.size(); ++i) {
        if (!matched[i]) spk[i] = spk[i - 1];
        if (!pieces[i].change || !spk[i] || spk[i] != spk[i - 1]) continue;
        uint32_t other = 0;
        for (size_t j = i - 1; j-- > 0;) if (spk[j] && spk[j] != spk[i - 1]) { other = spk[j]; break; }
        spk[i] = other;
    }
    // the first pieces, unheard: the next one's -- unless that one is a known change from it
    for (size_t i = pieces.size(); i-- > 0;)
        if (!spk[i] && !matched[i] && i + 1 < pieces.size() && !pieces[i + 1].change) spk[i] = spk[i + 1];
    if (unmatchedMs) *unmatchedMs = un;
    if (totalMs) *totalMs = tot;
    return spk;
}

// ---- NAMES ------------------------------------------------------------------------------------------------------------
inline std::string speakerName(uint32_t spk, const std::map<uint32_t, std::string>& names) {
    if (!spk) return std::string();
    const auto f = names.find(spk);
    return f != names.end() ? f->second : "Speaker " + std::to_string(spk);
}
inline std::string foldName(const std::string& n) {
    std::string o = trimSpaces(n);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}
// A name the user typed, as the transcript keeps it: trimmed; "" = back to "Speaker <n>"; refused (false, `why`) when it holds
// a control character or broken UTF-8, or is longer than pbtrf::kSpeakerNameMax bytes.
inline bool nameOk(const std::string& in, std::string& clean, std::string& why) {
    clean = trimSpaces(in);
    why.clear();
    if (clean.size() > pbtrf::kSpeakerNameMax) { why = "A name is at most 64 bytes long."; return false; }
    for (size_t i = 0; i < clean.size();) {
        const unsigned char c = (unsigned char)clean[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > clean.size()) { why = "That name is not valid text."; return false; }
        for (size_t k = 1; k < n; ++k) if (((unsigned char)clean[i + k] & 0xC0) != 0x80) { why = "That name is not valid text."; return false; }
        size_t q = i;
        const uint32_t cp = cpAt(clean, q);
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029) { why = "A name cannot hold a control character."; return false; }
        i += n;
    }
    return true;
}

// ---- THE TURNS ---------------------------------------------------------------------------------------------------------
inline std::vector<Turn> buildTurns(const std::vector<Piece>& pieces, const std::vector<uint32_t>& spk,
                                    const std::map<uint32_t, std::string>& names, const pbtc::SourceTc& tc,
                                    const TurnCfg& cfg = TurnCfg{}) {
    std::vector<Turn> out;
    bool anyName = false;
    for (uint32_t s : spk) if (s) { anyName = true; break; }
    std::string curKey;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        const uint32_t who = i < spk.size() ? spk[i] : 0;
        const std::string name = speakerName(who, names);
        const std::string key = foldName(name);
        bool fresh = out.empty() || key != curKey;
        if (!fresh) {
            const Turn& t = out.back();
            const uint64_t gap = p.s > t.e ? p.s - t.e : 0;
            if (gap >= (anyName ? cfg.speakerPauseMs : cfg.noNamePauseMs)) fresh = true;
            else if (p.s >= t.s + cfg.paraMaxMs && endsSentence(t.text)) fresh = true;
        }
        if (fresh) {
            Turn t;
            t.s = p.s;
            t.e = p.e;
            t.spk = who;
            t.name = name;
            t.stamp = pbtc::formatTc(tc, p.s);
            t.text = p.text;
            t.lang = p.lang;
            out.push_back(t);
            curKey = key;
        } else {
            joinInto(out.back().text, p.text);
            out.back().e = std::max(out.back().e, p.e);
        }
    }
    return out;
}

// The languages heard, by their share of the text (characters), each >= 5 %, the largest first.
inline std::vector<std::string> languagesHeard(const std::vector<tp::WireCue>& cues) {
    std::map<std::string, uint64_t> by;
    uint64_t tot = 0;
    for (const auto& c : cues) {
        if (c.lang.empty()) continue;
        int wide = 0;
        const uint64_t n = (uint64_t)narrowWide(c.src, wide) + (uint64_t)wide * 3;   // a CJK character says as much as ~3 letters
        by[c.lang] += n;
        tot += n;
    }
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& kv : by) if (tot && kv.second * 20 >= tot) v.push_back({ kv.second, kv.first });
    std::stable_sort(v.begin(), v.end(), [](const std::pair<uint64_t, std::string>& a, const std::pair<uint64_t, std::string>& b) {
        return a.first > b.first;
    });
    std::vector<std::string> out;
    for (const auto& x : v) out.push_back(x.second);
    return out;
}

// What a written .docx holds, in one number: its stamps, names and texts ("older" = the transcript moved since).
inline uint64_t fingerprint(const std::vector<Turn>& turns) {
    std::string all;
    for (const auto& t : turns) { all += t.stamp; all += '\x1f'; all += t.name; all += '\x1f'; all += t.text; all += '\x1e'; }
    return pbtrf::fnv1a64(all);
}
// Distinct names in the turns (folded: two speakers given one name are one person).
inline uint32_t namedSpeakers(const std::vector<Turn>& turns) {
    std::vector<std::string> seen;
    for (const auto& t : turns) {
        if (t.name.empty()) continue;
        const std::string k = foldName(t.name);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) seen.push_back(k);
    }
    return (uint32_t)seen.size();
}

// ---- THE WORD FILE: its facts (TrPlayer.h fills them, the bridge writes the file: DocxWriter.cpp buildSpeakerTranscript) ----
struct TranscriptOut {
    std::wstring   name;                   // the media's file name: the document's title
    uint64_t       durMs = 0;
    uint32_t       speakers = 0;           // distinct names in the turns (0: no names)
    bool           namesOn = false;        // the voice pass named the speakers (else `why`)
    std::string    why;                    // ...why not
    std::vector<std::string> langs;        // the languages heard, the largest share first (codes)
    pbtc::SourceTc tc;                     // the stamps' timecode (found = the file's SOURCE timecode)
    bool           partial = false;        // a transcript of what is analysed so far (Save .docx... only, never written by itself)
    uint64_t       doneMs = 0;             // ...this much of durMs
    std::vector<Turn> turns;
    uint64_t       sig = 0;                // fingerprint(turns): what the file holds
    std::wstring   prefer;                 // the Word file written before for this transcript, still ours: replaced in place
};
// "1:44", "1:02:07"
inline std::string clockText(uint64_t ms, bool hours = false) {      // hours: "0:43:10" beside a "1:02:07"
    const unsigned long long s = ms / 1000;
    char b[32];
    if (s >= 3600 || hours) std::snprintf(b, sizeof b, "%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    else           std::snprintf(b, sizeof b, "%llu:%02llu", s / 60, s % 60);
    return b;
}
// A language as the documents name it: native, + the English name after a non-Latin script (zh: its native name + " (Simplified Chinese)"
// -- the page's U1 rule). `native` = the native name of a code (the bridge: pbdocx::languageName).
inline std::string languageLabel(const std::string& code, const std::function<std::string(const std::string&)>& native) {
    struct E { const char* code; const char* en; };
    static const E kEn[] = { { "zh", "Simplified Chinese" }, { "zh-tw", "Traditional Chinese" }, { "ja", "Japanese" },
                             { "ko", "Korean" }, { "ru", "Russian" }, { "ar", "Arabic" } };
    std::string n = native ? native(code) : code;
    for (const auto& e : kEn) if (code == e.code) return n + " (" + e.en + ")";
    return n;
}
// The document's text around the turns (UTF-8): the title, the subtitle ("Transcript - 1:44 - 4 speakers", a middle dot),
// the header lines (the date and the languages, the timecode, a partial's extent, why there are no names) and the italic
// note. `date` = "2026-11-02 14:31" (the bridge's local time), `version` = "1.1.0".
struct DocText { std::string title, subtitle, note; std::vector<std::string> header; };
inline DocText docTextOf(const TranscriptOut& t, const std::string& date, const std::string& version,
                         const std::function<std::string(const std::string&)>& native) {
    static const char* const kDot = " \xc2\xb7 ";          // " - " with a middle dot, U+00B7
    DocText d;
    d.title = pbtrf::toUtf8(t.name);
    d.subtitle = "Transcript";
    if (t.durMs) d.subtitle += kDot + clockText(t.durMs);
    if (t.speakers) d.subtitle += kDot + std::to_string(t.speakers) + (t.speakers == 1 ? " speaker" : " speakers");
    std::string first = "Date: " + date;
    if (!t.langs.empty()) {
        std::string l;
        for (const auto& c : t.langs) l += (l.empty() ? "" : ", ") + languageLabel(c, native);
        first += kDot + std::string("Languages: ") + l;
    }
    d.header.push_back(first);
    d.header.push_back("Timecode: " + pbtc::describeTc(t.tc) + ".");
    if (t.partial) {
        const bool h = t.durMs >= 3600000;
        d.header.push_back("Partial: " + clockText(t.doneMs, h) + " of " + clockText(t.durMs, h) + " analysed.");
    }
    if (!t.namesOn) d.header.push_back(t.why.empty() ? std::string("Speakers were not recognised.")
                                                     : "Speakers could not be recognised: " + t.why + ".");
    d.note = "Automatic transcription by SuVtitle " + version + ", to be checked.";
    if (t.namesOn) d.note += " Speakers recognised by voice.";
    return d;
}

}   // namespace pbtrs

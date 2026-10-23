// =============================================================================
// SuVtitle - app/DocxWriter.cpp   (the Word transcript, dependency-free)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/DocxWriter.cpp at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: comments made ASCII (the build has no /utf-8).
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// See DocxWriter.h for WHY this exists at all. Two halves live here:
//   1. the CONTAINER  -- a ZIP with STORED entries (local headers + central
//      directory + end record + a CRC-32 of our own);
//   2. the DOCUMENT   -- WordprocessingML, built from the session's lines.
//
// (!) THIS FILE IS ASCII ON PURPOSE. It is compiled WITHOUT /utf-8 (see
// build/CMakeLists.txt), so a typed em-dash inside a WIDE literal would be
// decoded as CP1252 and reach Word as mojibake -- the same trap bridge.cpp's
// speaker separator documents. Every non-ASCII character here is a \u escape.
// =============================================================================
#include "DocxWriter.h"

#include <cstdint>
#include <cstdio>
#include <ctime>

namespace pbdocx {

// ----------------------------------------------------------------------------
// UTF-16 -> UTF-8 by hand, surrogate pairs included.
//
// NOT MultiByteToWideChar: this writer is deliberately free of <windows.h> so the
// smoke test can compile it alone. The surrogate branch is not theory -- an emoji
// in a card name, or any CJK extension B character, arrives as a PAIR, and
// encoding each half separately produces the two U+FFFD Word shows as boxes.
// ----------------------------------------------------------------------------
std::string toUtf8(const std::wstring& w) {
    std::string o;
    o.reserve(w.size() * 3 / 2 + 8);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = (uint32_t)(uint16_t)w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
            const uint32_t lo = (uint32_t)(uint16_t)w[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        // A LONE surrogate is not a character and cannot be encoded; dropping it is
        // the only option that still yields well-formed XML.
        if (c >= 0xD800 && c <= 0xDFFF) continue;
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

// The five XML characters, plus the control range. A control character is not
// merely ugly here: XML 1.0 FORBIDS it outright, so one stray 0x01 from a
// mis-decoded subtitle would make Word refuse the whole document ("unreadable
// content"), not skip a line. Tab and newline become a space rather than being
// dropped -- a run has no structure to hold them (a real break is <w:br/>), and a
// word-join is the lesser lie.
std::string xmlEscape(const std::wstring& w) {
    std::wstring clean;
    clean.reserve(w.size());
    for (wchar_t c : w) {
        if (c == L'\t' || c == L'\n' || c == L'\r') clean += L' ';
        else if ((unsigned)c < 0x20 || c == 0x7F)   continue;
        else                                        clean += c;
    }
    std::string o = toUtf8(clean);
    std::string e;
    e.reserve(o.size() + 16);
    for (char c : o) {
        switch (c) {
            case '&':  e += "&amp;";  break;
            case '<':  e += "&lt;";   break;
            case '>':  e += "&gt;";   break;
            case '"':  e += "&quot;"; break;
            case '\'': e += "&apos;"; break;
            default:   e += c;        break;
        }
    }
    return e;
}

std::wstring languageName(const std::wstring& code) {
    // The SAME native names both UI surfaces offer (app.js TR_LANGS / translator.html's
    // twin) -- thirteen since wave WA, which split Chinese in two: zh is now NAMED
    // Simplified, because a document listing two languages both called "\u4e2d\u6587" would not
    // say which of them a paragraph is in.
    // \u escapes, per this file's header. (Until WA these names were TYPED characters,
    // which compiled right only because this machine's ANSI code page is UTF-8 -- on any
    // other build machine they would have reached Word as mojibake.)
    struct Pair { const wchar_t* code; const wchar_t* name; };
    static const Pair kNames[] = {
        { L"en",    L"English" },
        { L"zh",    L"\u7b80\u4f53\u4e2d\u6587" },            // Simplified Chinese
        { L"zh-tw", L"\u7e41\u9ad4\u4e2d\u6587" },            // Traditional Chinese (Taiwan)
        { L"fr",    L"Fran\u00e7ais" },
        { L"de",    L"Deutsch" },
        { L"es",    L"Espa\u00f1ol" },
        { L"it",    L"Italiano" },
        { L"pt",    L"Portugu\u00eas" },
        { L"ja",    L"\u65e5\u672c\u8a9e" },
        { L"ko",    L"\ud55c\uad6d\uc5b4" },
        { L"ru",    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439" },
        { L"nl",    L"Nederlands" },
        { L"ar",    L"\u0627\u0644\u0639\u0631\u0628\u064a\u0629" },
    };
    std::wstring lc;
    for (wchar_t c : code) lc += (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
    for (const auto& p : kNames) if (lc == p.code) return p.name;
    // An unknown code is UPPERCASED rather than invented: "[SV]" is honest, and a
    // guess at a language name in a saved document is not correctable by the reader.
    std::wstring up;
    for (wchar_t c : lc) up += (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
    return up;
}

// ============================================================================
// 1. THE CONTAINER: a ZIP of stored entries.
// ============================================================================
static uint32_t crc32Of(const std::string& s) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;   // single-threaded by contract (the UI thread builds the file)
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(std::string& o, uint32_t v) {
    o += (char)(v & 0xFF);
    o += (char)((v >> 8) & 0xFF);
}
static void put32(std::string& o, uint32_t v) {
    put16(o, v & 0xFFFF);
    put16(o, (v >> 16) & 0xFFFF);
}

// MS-DOS date/time, the only stamp a ZIP entry has. Taken from the wall clock so
// Explorer and Word show "today" rather than 1980 -- the epoch a zeroed field means.
static void dosStamp(uint32_t& date, uint32_t& timev) {
    date = ((1980u - 1980u) << 9) | (1u << 5) | 1u;   // 1980-01-01, the floor
    timev = 0;
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#if defined(_MSC_VER)
    if (localtime_s(&t, &now) != 0) return;
#else
    if (!localtime_r(&now, &t)) return;
#endif
    if (t.tm_year + 1900 < 1980) return;             // a clock set to 1970: keep the floor
    date  = (uint32_t)(((t.tm_year + 1900 - 1980) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
    timev = (uint32_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
}

std::string zipStore(const std::vector<Part>& parts) {
    uint32_t dosDate = 0, dosTime = 0;
    dosStamp(dosDate, dosTime);

    std::string out, central;
    for (const auto& p : parts) {
        const uint32_t crc    = crc32Of(p.data);
        const uint32_t size   = (uint32_t)p.data.size();
        const uint32_t offset = (uint32_t)out.size();

        // ---- local file header ----
        put32(out, 0x04034B50u);
        put16(out, 20);          // version needed: 2.0 (store + folders)
        put16(out, 0);           // flags: none. Names are ASCII, so no UTF-8 bit needed
        put16(out, 0);           // method 0 = STORED
        put16(out, dosTime);
        put16(out, dosDate);
        put32(out, crc);
        put32(out, size);        // compressed == uncompressed, by construction
        put32(out, size);
        put16(out, (uint32_t)p.name.size());
        put16(out, 0);           // extra field length
        out += p.name;
        out += p.data;

        // ---- its central-directory record ----
        put32(central, 0x02014B50u);
        put16(central, 20);      // version made by
        put16(central, 20);      // version needed
        put16(central, 0);
        put16(central, 0);
        put16(central, dosTime);
        put16(central, dosDate);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, (uint32_t)p.name.size());
        put16(central, 0);       // extra
        put16(central, 0);       // comment
        put16(central, 0);       // disk number start
        put16(central, 0);       // internal attributes
        put32(central, 0);       // external attributes
        put32(central, offset);
        central += p.name;
    }

    const uint32_t cdOffset = (uint32_t)out.size();
    const uint32_t cdSize   = (uint32_t)central.size();
    out += central;

    // ---- end of central directory ----
    put32(out, 0x06054B50u);
    put16(out, 0);                          // this disk
    put16(out, 0);                          // disk with the central directory
    put16(out, (uint32_t)parts.size());     // entries on this disk
    put16(out, (uint32_t)parts.size());     // entries total
    put32(out, cdSize);
    put32(out, cdOffset);
    put16(out, 0);                          // comment length
    return out;
}

// ============================================================================
// 2. THE DOCUMENT: WordprocessingML.
// ============================================================================
namespace {

struct Run {
    std::wstring   text;
    bool           bold   = false;
    bool           italic = false;
    int            halfPt = 0;             // 0 = inherit docDefaults (22 = 11 pt)
    const wchar_t* color  = nullptr;       // "7F7F7F" and friends; null = inherit
    // WA: this run is TRADITIONAL Chinese (zh-tw). It then wears the Traditional face and
    // says so (see kTradFace) whatever the document's default is -- which is what keeps a
    // document holding BOTH scripts (a zh <-> zh-tw session) right paragraph by paragraph.
    bool           trad   = false;
};

// The Traditional Chinese UI face, and why a second face exists at all: it is not coverage --
// both faces have both scripts (the 140 characters of the helper's script table, checked with
// GetGlyphIndices on this machine: 0 missing either way) -- it is the HAND. Each face draws to
// its own region's standard: YaHei sets \uff0c\u3002 in the bottom corner as the mainland writes them,
// JhengHei centres them as Taiwan does, and shared characters' shapes follow the same split.
// A Taiwanese reader handed YaHei reads his own language in a foreign hand.
const char* const kTradFace = "Microsoft JhengHei";

std::string runXml(const Run& r) {
    std::string x = "<w:r>";
    if (r.bold || r.italic || r.halfPt || r.color || r.trad) {
        x += "<w:rPr>";
        // rFonts FIRST: CT_RPr is an ordered sequence and Word is entitled to reject a run
        // whose properties come out of order.
        if (r.trad) { x += "<w:rFonts w:eastAsia=\""; x += kTradFace; x += "\"/>"; }
        if (r.bold)   x += "<w:b/>";
        if (r.italic) x += "<w:i/>";
        if (r.color)  { x += "<w:color w:val=\""; x += xmlEscape(r.color); x += "\"/>"; }
        if (r.halfPt) {
            char b[64];
            // szCs as well as sz: without it the EAST ASIAN half of a mixed line keeps
            // the default size and a "smaller grey original" comes out half small.
            std::snprintf(b, sizeof b, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>",
                          r.halfPt, r.halfPt);
            x += b;
        }
        // ...and w:lang LAST (it follows sz/szCs in the same sequence): proofing and Word's
        // East Asian line-break rules are per LANGUAGE, and zh-TW's are Taiwan's.
        if (r.trad) x += "<w:lang w:eastAsia=\"zh-TW\"/>";
        x += "</w:rPr>";
    }
    // xml:space="preserve" on EVERY text node: Word collapses leading/trailing
    // spaces without it, and the turn header's gap before the timestamp is exactly
    // such a space.
    x += "<w:t xml:space=\"preserve\">";
    x += xmlEscape(r.text);
    x += "</w:t></w:r>";
    return x;
}

std::string paraXml(const std::string& pPr, const std::vector<Run>& runs) {
    std::string x = "<w:p>";
    if (!pPr.empty()) { x += "<w:pPr>"; x += pPr; x += "</w:pPr>"; }
    for (const auto& r : runs) x += runXml(r);
    x += "</w:p>";
    return x;
}

// Does this codepoint belong to a script that writes WITHOUT spaces? The join
// below asks, because "\u597d\u7684\u3002 \u6211\u4eec" (a space between two
// mandarin sentences) is not typography, it is a bug the reader sees. Rough on
// purpose: the question is "would a space look wrong here", not "which script".
bool isCjk(wchar_t c) {
    const unsigned u = (unsigned)c;
    return (u >= 0x2E80 && u <= 0x9FFF)     // radicals, kana, CJK unified
        || (u >= 0xAC00 && u <= 0xD7AF)     // hangul syllables
        || (u >= 0xF900 && u <= 0xFAFF)     // compatibility ideographs
        || (u >= 0xFE30 && u <= 0xFE4F)     // CJK compatibility forms
        || (u >= 0xFF00 && u <= 0xFFEF);    // full-width forms
}

void joinInto(std::wstring& acc, const std::wstring& next) {
    if (next.empty()) return;
    if (!acc.empty() && !isCjk(acc.back()) && !isCjk(next.front())) acc += L' ';
    acc += next;
}

// One paragraph's text as RUNS, each carrying its own script (review of WA, 10/10): the
// Traditional face and lang are a property of a LINE, not of a turn -- a zh <-> zh-tw turn that
// mixes both must not set its Simplified sentences in JhengHei. Same joining rule as joinInto;
// consecutive pieces of the same script stay ONE run, so a document with no Traditional at all
// is written byte for byte as before.
struct Piece { std::wstring text; bool trad = false; };
void joinPiece(std::vector<Piece>& ps, const std::wstring& next, bool trad) {
    if (next.empty()) return;
    std::wstring t = next;
    if (!ps.empty() && !ps.back().text.empty() && !isCjk(ps.back().text.back()) && !isCjk(t.front()))
        t = L" " + t;
    if (!ps.empty() && ps.back().trad == trad) ps.back().text += t;
    else ps.push_back(Piece{ t, trad });
}

std::wstring clockOf(long long epochMs) {
    if (epochMs <= 0) return std::wstring();
    const std::time_t secs = (std::time_t)(epochMs / 1000);
    std::tm t{};
#if defined(_MSC_VER)
    if (localtime_s(&t, &secs) != 0) return std::wstring();
#else
    if (!localtime_r(&secs, &t)) return std::wstring();
#endif
    wchar_t b[16];
    std::swprintf(b, 16, L"%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

const wchar_t* const kGrey = L"7F7F7F";

// The journal carries the helper's canonical codes, so this is an exact match -- lower-cased
// only for a hand-edited file. NEVER a two-letter prefix: "zh-tw" cut to two is "zh", and the
// Traditional half of the session would quietly be set in the Simplified face.
bool isTradCode(const std::wstring& code) {
    std::wstring lc;
    for (wchar_t c : code) lc += (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
    return lc == L"zh-tw";
}
bool isSimpCode(const std::wstring& code) {
    return code == L"zh" || code == L"ZH";
}
// The language the TRANSLATION paragraph's share of a line is in: the source itself for a
// passthrough or a line with no translation (that is the text the paragraph takes), else the
// target -- read from dstLang, which every journal line carries (W2 wrote it before W5 existed).
const std::wstring& translatedLangOf(const Line& l) {
    return (l.pass || l.dst.empty()) ? l.lang : l.dstLang;
}

// WA's rule, shared by both documents: a document whose Chinese is all Traditional takes the
// Traditional face as its DEFAULT; one that also holds Simplified keeps YaHei and dresses its
// Traditional runs one by one (Run::trad).
bool tradDefaultOf(const std::vector<Line>& lines) {
    bool hasTrad = false, hasSimp = false;
    for (const Line& l : lines) {
        if (isTradCode(l.lang) || isTradCode(l.dstLang)) hasTrad = true;
        if (isSimpCode(l.lang) || isSimpCode(l.dstLang)) hasSimp = true;
    }
    return hasTrad && !hasSimp;
}

// MEDIA time, for a file transcript (WC): "[00:12:34]" -- hours always shown, so a column of
// stamps lines up whether the recording is ten minutes or three hours long.
std::wstring mediaStampOf(long long ms) {
    if (ms < 0) ms = 0;
    const long long s = ms / 1000;
    wchar_t b[32];
    std::swprintf(b, 32, L"[%02lld:%02lld:%02lld]", s / 3600, (s / 60) % 60, s % 60);
    return b;
}

// THE PACKAGE, shared by both documents (WC factored it out of buildTranscript unchanged -- the
// W5 smoke's parts hash the same before and after): the body closes on an A4 section, then the
// five parts of an OPC package, [Content_Types].xml first.
std::string packageDocx(std::string body, bool tradDefault) {
    // A4 with 2 cm margins. Word invents a section when one is missing, but it
    // invents US Letter -- and the user prints A4.
    body += "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
            "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\""
            " w:header=\"709\" w:footer=\"709\" w:gutter=\"0\"/></w:sectPr>";

    static const char* const kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

    std::string document = kDecl;
    document += "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                "<w:body>";
    document += body;
    document += "</w:body></w:document>";

    std::string contentTypes = kDecl;
    contentTypes += "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-"
        "officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-"
        "officedocument.wordprocessingml.styles+xml\"/>"
        "</Types>";

    std::string rels = kDecl;
    rels += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
        "relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";

    std::string docRels = kDecl;
    docRels += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
        "relationships/styles\" Target=\"styles.xml\"/></Relationships>";

    // eastAsia = Microsoft YaHei, and that is the whole reason this part exists: with
    // Calibri alone, Word falls back per-run for the mandarin half of a bilingual
    // transcript and the document reads in two different weights. (WA: JhengHei instead
    // when the document's Chinese is all Traditional -- see tradDefault.)
    std::string styles = kDecl;
    styles += "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"";
    styles += tradDefault ? kTradFace : "Microsoft YaHei";
    styles += "\" w:cs=\"Calibri\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
        "</w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\" w:line=\"259\" w:lineRule=\"auto\"/>"
        "</w:pPr></w:pPrDefault></w:docDefaults></w:styles>";

    // [Content_Types].xml FIRST: OPC requires it, and a reader that streams the
    // archive (Word does) gives up on a package whose types arrive last.
    std::vector<Part> parts = {
        { "[Content_Types].xml",          contentTypes },
        { "_rels/.rels",                  rels         },
        { "word/_rels/document.xml.rels", docRels      },
        { "word/styles.xml",              styles       },
        { "word/document.xml",            document     },
    };
    return zipStore(parts);
}

}   // namespace

bool buildTranscript(const Doc& info, const std::vector<Line>& lines, std::string& out) {
    if (lines.empty()) return false;

    std::string body;
    body += paraXml("<w:spacing w:after=\"0\"/>",
                    { Run{ info.title, /*bold=*/true, false, 32, nullptr } });
    if (!info.subtitle.empty())
        body += paraXml("<w:spacing w:after=\"240\"/>",
                        { Run{ info.subtitle, false, false, 18, kGrey } });

    // WA: WHICH CHINESE this document holds. A document whose Chinese is all Traditional (an
    // en <-> zh-tw session) takes the Traditional face as its DEFAULT -- headers, speaker
    // names and the [language] badges included. One that also holds Simplified (a zh <-> zh-tw
    // session, or a pair changed mid-meeting) keeps YaHei as the default and dresses its
    // Traditional paragraphs one by one (Run::trad). The two never fight: a Traditional run
    // names its own face whatever the default is.
    bool hasTrad = false, hasSimp = false;
    for (const Line& l : lines) {
        if (isTradCode(l.lang) || isTradCode(l.dstLang)) hasTrad = true;
        if (isSimpCode(l.lang) || isSimpCode(l.dstLang)) hasSimp = true;
    }
    const bool tradDefault = hasTrad && !hasSimp;

    // THE TURN is the unit, exactly as both screens render it: consecutive lines
    // sharing a speaker key are ONE exchange, and splitting them into one block per
    // sentence would turn a conversation into a list -- the very thing the dialogue
    // renderer exists to avoid. Grouped on the KEY, never on the label: two cables
    // can legitimately carry the same name, and a label is a display string.
    std::wstring prevTurnLang;
    size_t i = 0;
    while (i < lines.size()) {
        const size_t start = i;
        const std::wstring key = lines[i].spkKey;
        while (i < lines.size() && lines[i].spkKey == key) ++i;

        const Line& head = lines[start];
        std::wstring who = head.spkLabel;
        if (who.empty()) who = L"Speaker";          // an unattributed line still opens a turn

        std::vector<Run> hdr;
        hdr.push_back(Run{ L"\u2014 " + who, /*bold=*/true, false, 0, nullptr });
        const std::wstring clock = clockOf(head.tEpochMs);
        if (!clock.empty())
            hdr.push_back(Run{ L"   " + clock, false, false, 16, kGrey });
        // keepNext: a turn header alone at the foot of a page, with its words on the
        // next one, is the one page break a transcript must not have.
        body += paraXml("<w:spacing w:before=\"220\" w:after=\"40\"/><w:keepNext/>", hdr);

        // Pieces, not one string: each LINE keeps its own script (see joinPiece).
        std::vector<Piece> translated, original;
        for (size_t k = start; k < i; ++k) {
            const Line& l = lines[k];
            // A PASSTHROUGH said one thing and it was already in the target language:
            // its text belongs in the translation paragraph and NOWHERE else. Printing
            // it under "originals" too would show the same sentence twice and imply a
            // translation step that never ran.
            joinPiece(translated, l.dst.empty() ? l.src : l.dst, isTradCode(translatedLangOf(l)));
            if (!l.pass) joinPiece(original, l.src, isTradCode(l.lang));
        }

        if (!translated.empty()) {
            std::vector<Run> runs;
            for (const Piece& p : translated) {
                Run r{ p.text };
                r.trad = p.trad;
                runs.push_back(r);
            }
            body += paraXml("<w:spacing w:after=\"20\"/><w:ind w:left=\"283\"/>", runs);
        }

        if (!original.empty()) {
            // The language is named only when it CHANGES from the turn before. On a strictly
            // alternating two-language meeting that is every turn, which is correct -- the
            // italic paragraph is the ONLY place the source language shows, and a reader
            // scanning the document cannot tell Portuguese from Spanish by eye. What it
            // saves is the run of turns in ONE language (a monologue, a third party joining
            // in the same tongue), where repeating the badge would be pure noise.
            // The badge is in the script of the language it NAMES ("[\u7b80\u4f53\u4e2d\u6587]" is Simplified).
            if (!head.lang.empty() && head.lang != prevTurnLang) {
                const std::wstring badge = L"[" + languageName(head.lang) + L"] ";
                const bool bt = isTradCode(head.lang);
                if (original.front().trad == bt) original.front().text = badge + original.front().text;
                else original.insert(original.begin(), Piece{ badge, bt });
            }
            std::vector<Run> runs;
            for (const Piece& p : original) {
                Run r{ p.text, false, /*italic=*/true, 18, kGrey };
                r.trad = p.trad;
                runs.push_back(r);
            }
            body += paraXml("<w:spacing w:after=\"80\"/><w:ind w:left=\"283\"/>", runs);
        }
        if (!head.lang.empty()) prevTurnLang = head.lang;
    }

    out = packageDocx(std::move(body), tradDefault);
    return true;
}

// ============================================================================
// FILE MODE (WC): the transcript of a RECORDING. No speakers and no wall clock -- a recording
// has neither; what it has is a timeline. So every subtitle is its own short entry, stamped
// with the MEDIA time it is spoken at, the translation first and the original under it in
// grey italics (once only for a passthrough, which already IS in the target language). The
// spoken language is named where it changes, exactly as the meeting transcript does it.
// ============================================================================
bool buildFileTranscript(const Doc& info, const std::vector<Line>& lines, std::string& out) {
    if (lines.empty()) return false;

    std::string body;
    body += paraXml("<w:spacing w:after=\"0\"/>",
                    { Run{ info.title, /*bold=*/true, false, 32, nullptr } });
    if (!info.subtitle.empty())
        body += paraXml("<w:spacing w:after=\"240\"/>",
                        { Run{ info.subtitle, false, false, 18, kGrey } });

    const bool tradDefault = tradDefaultOf(lines);
    std::wstring prevLang;
    for (const Line& l : lines) {
        const std::wstring translated = l.dst.empty() ? l.src : l.dst;
        const bool hasOriginal = !l.pass && !l.src.empty() && l.src != translated;

        std::vector<Run> head;
        head.push_back(Run{ mediaStampOf(l.mediaMs) + L"  ", false, false, 16, kGrey });
        Run tr{ translated };
        tr.trad = isTradCode(translatedLangOf(l));
        head.push_back(tr);
        // keepNext only when an original follows: the two halves of one subtitle must not be
        // split across a page break.
        body += paraXml(hasOriginal ? "<w:spacing w:before=\"80\" w:after=\"0\"/><w:keepNext/>"
                                    : "<w:spacing w:before=\"80\" w:after=\"0\"/>", head);
        if (hasOriginal) {
            std::wstring text;
            if (!l.lang.empty() && l.lang != prevLang)
                text = L"[" + languageName(l.lang) + L"] ";
            text += l.src;
            Run r{ text, false, /*italic=*/true, 18, kGrey };
            r.trad = isTradCode(l.lang);
            body += paraXml("<w:spacing w:after=\"40\"/><w:ind w:left=\"1134\"/>", { r });
        }
        if (!l.lang.empty()) prevLang = l.lang;
    }

    out = packageDocx(std::move(body), tradDefault);
    return true;
}

}   // namespace pbdocx

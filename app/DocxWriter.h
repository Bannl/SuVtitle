// =============================================================================
// SuVtitle - app/DocxWriter.h   (the Word transcript, dependency-free)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/DocxWriter.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md). SuVtitle exports a film's transcript with buildFileTranscript;
// buildTranscript (AudioPB's live session, W5) stays: tests/cpp/filedocx_smoke.cpp proves both and they share every
// piece. Changed: comments made ASCII (the build has no /utf-8). Nothing else.
// --- the original header follows ---
//
// A .docx the tray writes ITSELF: an OOXML package is a ZIP of five small XML
// parts, and a transcript needs none of what a library would bring. Zero
// dependency is the point -- the tray already ships ~6 GB of models on the
// personal edition, and adding libzip/minizip to save a Word file would be the
// tail wagging the dog.
//
// Deliberately IGNORANT OF THE BRIDGE: it takes a vector of plain structs, so
// the whole document (turn grouping, joining, escaping, the ZIP) can be built
// and inspected from a 200-line smoke test with no WebView2, no fabric and no
// helper running -- which is exactly how W5 proved it (scratchpad/docx_smoke.cpp).
//
// (!) The entries are STORED (method 0, no compression). Word, LibreOffice and
// Explorer's own zip folder all accept it; a deflate stream we would have to
// write by hand is 400 lines of risk for ~40 KB on a document nobody stores by
// the thousand. If an .xlsx is ever wanted (the table shape the user might ask
// for next), it is the SAME container with a different part list: zipStore()
// is public for exactly that reason.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace pbdocx {

// ONE finished translator line, as %APPDATA%\AudioPB\Translator sessions\*.jsonl
// records it. A subset of the wire's translatorLine: what a transcript needs and
// nothing else (no segId, no seq, no latency -- a reader does not care which
// sentence of an utterance this was, only that it was said).
struct Line {
    long long    tEpochMs = 0;    // when the line was spoken (unix ms, local time on render)
    std::wstring lang;            // the language SPOKEN (ISO-639-1)
    std::wstring dstLang;         // the language `dst` is in
    std::wstring src;             // the original, verbatim
    std::wstring dst;             // the translation
    // PASSTHROUGH: the segment was already in the target language, so `dst` merely
    // repeats `src`. Such a line contributes its text ONCE (to the translation
    // paragraph) -- printing it twice would make the document claim a translation
    // that never happened.
    bool         pass = false;
    std::wstring spkKey;          // identity a turn is grouped on (never the label)
    std::wstring spkLabel;        // what the desk calls that voice ("Guest mic")
    // FILE MODE (WC): where in the RECORDING this subtitle is spoken (ms). -1 = a live line,
    // which has a wall clock (tEpochMs) instead. Only buildFileTranscript reads it.
    long long    mediaMs = -1;
};

// The document's own header: the two lines above the dialogue.
struct Doc {
    std::wstring title;      // "AudioPB Translator -- 2026-09-17"
    std::wstring subtitle;   // "Session started 15:40, 12 lines, English <-> \u7b80\u4f53\u4e2d\u6587"
};

// The whole .docx, as bytes. False = there was nothing to write (no lines): a
// caller must never hand the user an empty Word file and call it a transcript.
bool buildTranscript(const Doc& info, const std::vector<Line>& lines, std::string& out);

// FILE MODE (WC): the transcript of a RECORDING -- one entry per subtitle, stamped with its
// MEDIA time (Line::mediaMs), translation then original; no speaker turns (a recording's
// subtitles carry none). Same package, same fonts, same false-on-empty rule.
bool buildFileTranscript(const Doc& info, const std::vector<Line>& lines, std::string& out);

// ---- the pieces, public so a future .xlsx can reuse them --------------------
struct Part { std::string name; std::string data; };   // ASCII name, UTF-8 bytes
// [Content_Types].xml MUST be the first entry of an OPC package. The caller owns
// that ordering; this only writes what it is given, in the order it is given.
std::string zipStore(const std::vector<Part>& parts);

std::string  toUtf8(const std::wstring& w);            // surrogate pairs included
std::string  xmlEscape(const std::wstring& w);         // & < > " and control chars
// "en" -> "English", "zh" -> "\u7b80\u4f53\u4e2d\u6587", "zh-tw" -> "\u7e41\u9ad4\u4e2d\u6587". The SAME thirteen native
// names the two UI surfaces offer (ui/web/app.js TR_LANGS): a document that named a
// language differently from the window that produced it would read like another product.
std::wstring languageName(const std::wstring& code);

}   // namespace pbdocx

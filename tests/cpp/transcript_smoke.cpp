// =============================================================================
// transcript_smoke -- U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 6): app/DocxWriter.cpp's NEW builder,
// buildSpeakerTranscript, ALONE (no WebView2, no engine). The package is STORED (no compression: DocxWriter.h), so the XML
// of each part is plain in the bytes and this smoke checks it itself -- the named styles, the header block, one paragraph
// per turn ("[stamp] Name: text" in its Timecode / Speaker runs), no translation text, the escaping, the Traditional face, the
// false-on-empty rule -- then writes transcript_out.docx (+ transcript_tc_out.docx, a source timecode and no names) for
// transcript_check.py, which re-reads them as a ZIP with Python's own parsers. The two older builders are checked unchanged
// (their styles part has no named style). Sentinels: tests/cpp/cmut_u3.py (docx_u3). /W4, no /utf-8. ASCII only.
// =============================================================================
#include "../../app/DocxWriter.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}

// A STORED zip's entry `name`, as its bytes ("" = not there): the local headers walked.
static std::string partOf(const std::string& zip, const std::string& name) {
    size_t at = 0;
    while (at + 30 <= zip.size() && zip.compare(at, 4, "PK\x03\x04") == 0) {
        auto u16 = [&](size_t o) { return (size_t)(unsigned char)zip[o] | ((size_t)(unsigned char)zip[o + 1] << 8); };
        auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
        const size_t size = u32(at + 18), nlen = u16(at + 26), xlen = u16(at + 28);
        const std::string n = zip.substr(at + 30, nlen);
        const size_t data = at + 30 + nlen + xlen;
        if (n == name) return zip.substr(data, size);
        at = data + size;
    }
    return std::string();
}
static size_t count(const std::string& s, const std::string& x) {
    size_t n = 0;
    for (size_t at = s.find(x); at != std::string::npos; at = s.find(x, at + x.size())) ++n;
    return n;
}
static bool writeFile(const char* path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    return (bool)f;
}

int main() {
    std::printf("transcript_smoke: the speaker transcript (.docx) -- DocxWriter.cpp buildSpeakerTranscript\n");
    pbdocx::TranscriptDoc doc;
    doc.title = L"meeting.wav";
    doc.subtitle = L"Transcript \u00b7 1:44 \u00b7 4 speakers";
    doc.header = { L"Date: 2026-11-02 14:31 \u00b7 Languages: English, \u7b80\u4f53\u4e2d\u6587 (Simplified Chinese)",
                   L"Timecode: from the start of the file." };
    doc.note = L"Automatic transcription by SuVtitle 1.1.0, to be checked. Speakers recognised by voice.";
    std::vector<pbdocx::TranscriptTurn> turns = {
        { L"[00:00:00]", L"Speaker 1", L"Good morning everyone, thanks for joining on time.", L"en" },
        { L"[00:00:04]", L"Marie", L"Thanks, David. A & B < C > \"D\".", L"en" },
        { L"[00:00:19]", L"Speaker 3", L"\u5927\u5bb6\u65e9\u4e0a\u597d\uff0c\u6211\u8fd9\u8fb9\u51c6\u5907\u597d\u4e86\u3002", L"zh" },
        { L"[00:00:23]", L"Speaker 4", L"Hello\x01 everyone.", L"en" },
    };
    std::string bytes;
    ok("an empty transcript is NOT a document (false, no bytes)", !pbdocx::buildSpeakerTranscript(doc, {}, bytes) && bytes.empty());
    const bool built = pbdocx::buildSpeakerTranscript(doc, turns, bytes);
    ok("a transcript builds", built && bytes.size() > 1000);
    const std::string styles = partOf(bytes, "word/styles.xml"), document = partOf(bytes, "word/document.xml");
    ok("[Content_Types].xml is the FIRST entry (OPC)", bytes.compare(30, 19, "[Content_Types].xml") == 0);
    ok("the named styles: Normal (the default), Title, Subtitle, Speaker, Timecode -- Word's built-in names for the first three",
       count(styles, "w:styleId=\"Normal\"") == 1 && count(styles, "<w:name w:val=\"Title\"/>") == 1
       && count(styles, "<w:name w:val=\"Subtitle\"/>") == 1 && count(styles, "w:styleId=\"Speaker\"") == 1
       && count(styles, "w:styleId=\"Timecode\"") == 1 && styles.find("w:default=\"1\" w:styleId=\"Normal\"") != std::string::npos, styles);
    ok("the title and the subtitle in their styles", document.find("<w:pStyle w:val=\"Title\"/></w:pPr><w:r><w:t xml:space=\"preserve\">meeting.wav<") != std::string::npos
       && document.find("<w:pStyle w:val=\"Subtitle\"/>") != std::string::npos);
    ok("the header lines and the italic note", document.find("Timecode: from the start of the file.") != std::string::npos
       && document.find("<w:i/></w:rPr><w:t xml:space=\"preserve\">Automatic transcription by SuVtitle 1.1.0, to be checked.") != std::string::npos);
    ok("one paragraph per turn: a Timecode run, a Speaker run (\"Name: \"), the text",
       count(document, "<w:rStyle w:val=\"Timecode\"/>") == 4 && count(document, "<w:rStyle w:val=\"Speaker\"/>") == 4
       && document.find("<w:rStyle w:val=\"Timecode\"/></w:rPr><w:t xml:space=\"preserve\">[00:00:04] </w:t></w:r><w:r><w:rPr>"
                        "<w:rStyle w:val=\"Speaker\"/></w:rPr><w:t xml:space=\"preserve\">Marie: </w:t></w:r><w:r><w:t xml:space=\"preserve\">"
                        "Thanks, David.") != std::string::npos);
    ok("escaping: & < > and quotes", document.find("A &amp; B &lt; C &gt; &quot;D&quot;.") != std::string::npos);
    ok("a control character is dropped, the sentence kept", document.find("Hello everyone.") != std::string::npos
       && document.find('\x01') == std::string::npos);
    ok("the Chinese turn as written (UTF-8), no translation text anywhere", document.find("\xe5\xa4\xa7\xe5\xae\xb6") != std::string::npos
       && document.find("translation") == std::string::npos);
    // no names (the diarizer off): no Speaker run; a source timecode as given; an all-Traditional document's default face
    std::vector<pbdocx::TranscriptTurn> plain = { { L"[10:00:03:12]", L"", L"\u7e41\u9ad4\u4e2d\u6587\u3002", L"zh-tw" },
                                                  { L"[10:00:09;02]", L"", L"Second line.", L"en" } };
    pbdocx::TranscriptDoc d2 = doc;
    d2.header = { L"Timecode: source, 10:00:00:00 at 25 fps, read from the file." };
    std::string b2;
    const bool built2 = pbdocx::buildSpeakerTranscript(d2, plain, b2);
    const std::string doc2 = partOf(b2, "word/document.xml"), sty2 = partOf(b2, "word/styles.xml");
    ok("no names: no Speaker run, the stamps as given (a source timecode, a drop-frame one)",
       built2 && count(doc2, "<w:rStyle w:val=\"Speaker\"/>") == 0 && doc2.find("[10:00:03:12] </w:t>") != std::string::npos
       && doc2.find("[10:00:09;02] </w:t>") != std::string::npos);
    ok("a document whose Chinese is all Traditional takes JhengHei as its default face",
       sty2.find("w:eastAsia=\"Microsoft JhengHei\"") != std::string::npos);
    // the OLDER builders are unchanged: no named style in their styles part
    pbdocx::Doc old;
    old.title = L"film.mkv";
    pbdocx::Line l;
    l.mediaMs = 1000; l.lang = L"en"; l.dstLang = L"fr"; l.src = L"Hello."; l.dst = L"Bonjour.";
    std::string b3;
    pbdocx::buildFileTranscript(old, { l }, b3);
    const std::string sty3 = partOf(b3, "word/styles.xml");
    ok("the bilingual file transcript's styles part is as before (docDefaults only, no named style)",
       !sty3.empty() && sty3.find("<w:style ") == std::string::npos && sty3.find("</w:docDefaults></w:styles>") != std::string::npos);
    ok("transcript_out.docx and transcript_tc_out.docx written",
       writeFile("transcript_out.docx", bytes) && writeFile("transcript_tc_out.docx", b2));
    std::printf("\ntranscript_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

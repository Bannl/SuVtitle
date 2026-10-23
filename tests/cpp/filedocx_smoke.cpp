// =============================================================================
// SuVtitle tests/cpp/filedocx_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/filedocx_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include, the documents' titles (inputs: the checker reads the film's name). Same checks.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// filedocx_smoke -- WC: pbdocx::buildFileTranscript (the transcript of a RECORDING) compiled
// alone with ui/DocxWriter.cpp. Writes filedocx_out.docx next to itself; filedocx_check.py
// then opens it as a ZIP, parses every part as XML and checks the paragraphs.
// Non-ASCII literals are \u escapes (no /utf-8, like the product).
// =============================================================================
#include "../../app/DocxWriter.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::vector<pbdocx::Line> lines;
    auto add = [&](long long ms, const wchar_t* lang, const wchar_t* dl, const std::wstring& src,
                   const std::wstring& dst, bool pass) {
        pbdocx::Line l;
        l.mediaMs = ms; l.lang = lang; l.dstLang = dl; l.src = src; l.dst = dst; l.pass = pass;
        lines.push_back(l);
    };
    add(1000, L"en", L"zh-tw", L"Hello everyone.", L"\u5927\u5bb6\u597d\u3002", false);
    add(3723000, L"en", L"zh-tw", L"A & B < C > D, \"quoted\".", L"A & B < C > D\uff0c\u300c\u5f15\u865f\u300d\u3002", false);
    // A passthrough: already in the target language -> written ONCE, no original under it.
    add(3725500, L"zh-tw", L"zh-tw", L"\u597d\u7684\u3002", L"\u597d\u7684\u3002", true);
    // A control character smuggled into a subtitle: XML 1.0 forbids it, the writer must drop it.
    add(3727000, L"fr", L"zh-tw", L"Bonjour\x01 tout le monde.", L"\u5927\u5bb6\u597d\u3002", false);

    pbdocx::Doc info;
    info.title = L"film.mkv \u2014 SuVtitle";
    info.subtitle = L"Transcript \u00b7 1:02:07 \u00b7 4 subtitles \u00b7 English \u2192 \u7e41\u9ad4\u4e2d\u6587";
    std::string bytes;
    if (!pbdocx::buildFileTranscript(info, lines, bytes) || bytes.empty()) {
        std::printf("FAIL: buildFileTranscript returned nothing\n");
        return 1;
    }
    std::string none;
    if (pbdocx::buildFileTranscript(info, {}, none)) {
        std::printf("FAIL: an EMPTY transcript must be refused, never written\n");
        return 1;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, "filedocx_out.docx", "wb") != 0 || !f) { std::printf("FAIL: cannot write\n"); return 1; }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    std::printf("OK: %zu cues -> filedocx_out.docx (%zu bytes); empty input refused\n", lines.size(), bytes.size());

    // ---- the review of WA: Traditional per LINE, not per TURN (the LIVE transcript) ----------
    // A zh <-> zh-tw meeting, ONE speaker, two lines: the first spoken in Simplified and
    // translated into Traditional, the second the other way round. Each sentence must wear its
    // OWN script's face -- the Simplified ones never JhengHei, the Traditional ones always.
    std::vector<pbdocx::Line> live;
    auto addLive = [&](long long t, const wchar_t* lang, const wchar_t* dl, const std::wstring& src,
                       const std::wstring& dst) {
        pbdocx::Line l;
        l.tEpochMs = t; l.lang = lang; l.dstLang = dl; l.src = src; l.dst = dst;
        l.spkKey = L"s1|zh"; l.spkLabel = L"Host";
        live.push_back(l);
    };
    const std::wstring simp = L"\u7b80\u4f53\u53e5\u5b50\u3002";   // Simplified sentence
    const std::wstring trad = L"\u7e41\u9ad4\u53e5\u5b50\u3002";   // Traditional sentence
    const std::wstring simp2 = L"\u7b80\u4f53\u56de\u7b54\u3002";
    const std::wstring trad2 = L"\u7e41\u9ad4\u56de\u7b54\u3002";
    addLive(1789573212000LL, L"zh", L"zh-tw", simp, trad);         // said in zh  -> shown in zh-tw
    addLive(1789573216000LL, L"zh-tw", L"zh", trad2, simp2);       // said in zh-tw -> shown in zh
    pbdocx::Doc li;
    li.title = L"SuVtitle";
    li.subtitle = L"mixed scripts";
    std::string lb;
    if (!pbdocx::buildTranscript(li, live, lb) || lb.empty()) { std::printf("FAIL: live transcript\n"); return 1; }
    if (fopen_s(&f, "mixedturn_out.docx", "wb") != 0 || !f) { std::printf("FAIL: cannot write\n"); return 1; }
    std::fwrite(lb.data(), 1, lb.size(), f);
    std::fclose(f);
    std::printf("OK: a mixed zh/zh-tw turn -> mixedturn_out.docx (%zu bytes)\n", lb.size());
    return 0;
}

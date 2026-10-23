// =============================================================================
// SuVtitle tests/cpp/filestore_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/filestore_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include, the fallback .srt name (.suvtitle.srt). Same checks, same count.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// filestore_smoke -- the WC (file mode) unit check for ui/TrFileStore.h: the cache key, the
// cache record (and its torn-write guard), the SRT naming rule, the "did we write this .srt?"
// registry, the URL encoding and the AV-safe write -- the last two against the REAL disk, in a
// private folder under %TEMP% that is deleted at the end.
// Non-ASCII literals are UTF-8 bytes / \u escapes: the product compiles without /utf-8.
// =============================================================================
#include "../../app/TrFileStore.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = pbt::trpipe;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}

static tp::WireCue mkCue(uint32_t idx, uint64_t s, uint64_t e, const char* lang, const char* dl,
                         const std::string& src, const std::string& dst, bool pass = false) {
    tp::WireCue c;
    c.jobId = 77; c.idx = idx; c.startMs = s; c.endMs = e; c.lang = lang; c.dstLang = dl;
    c.src = src; c.dst = dst; c.pass = pass;
    return c;
}

int main() {
    std::printf("== cache key ==\n");
    {
        const std::wstring p = L"D:\\Films\\Mon film.mkv";
        const std::string k = pbtrf::cacheKey(p, 1000, 42, "en", "fr", "auto", "auto");
        ok("16 hex digits", k.size() == 16 && k.find_first_not_of("0123456789abcdef") == std::string::npos, k);
        ok("stable", k == pbtrf::cacheKey(p, 1000, 42, "en", "fr", "auto", "auto"));
        ok("case-insensitive path (Windows compares paths that way)",
           k == pbtrf::cacheKey(L"d:\\FILMS\\mon FILM.MKV", 1000, 42, "en", "fr", "auto", "auto"));
        ok("size changes it",    k != pbtrf::cacheKey(p, 1001, 42, "en", "fr", "auto", "auto"));
        ok("mtime changes it",   k != pbtrf::cacheKey(p, 1000, 43, "en", "fr", "auto", "auto"));
        ok("langA changes it",   k != pbtrf::cacheKey(p, 1000, 42, "de", "fr", "auto", "auto"));
        ok("langB changes it",   k != pbtrf::cacheKey(p, 1000, 42, "en", "zh-tw", "auto", "auto"));
        ok("mode changes it",    k != pbtrf::cacheKey(p, 1000, 42, "en", "fr", "a2b", "auto"));
        ok("quality changes it", k != pbtrf::cacheKey(p, 1000, 42, "en", "fr", "auto", "fast"));
        ok("another file changes it", k != pbtrf::cacheKey(L"D:\\Films\\Autre.mkv", 1000, 42, "en", "fr", "auto", "auto"));
    }

    std::printf("== cache record ==\n");
    const std::string nasty = "Tab\there, \"quotes\", back\\slash\\n, line\nbreak, "
                              "\xE7\xB9\x81\xE9\xAB\x94 & <tag>";
    pbtrf::Record r;
    r.path = "D:\\Films\\Mon film \xC3\xA9t\xC3\xA9.mkv"; r.size = 123456789012ULL; r.mtime = 133000000000000000ULL;
    r.langA = "en"; r.langB = "zh-tw"; r.mode = "auto"; r.quality = "auto";
    r.asrModel = "ggml-large-v3.bin"; r.mtModel = "Qwen3-4B-Instruct-2507-Q5_K_M.gguf";
    r.hasVideo = true; r.totalMs = 6130000; r.srtPath = "D:\\Films\\Mon film \xC3\xA9t\xC3\xA9.zh-TW.srt";
    r.srtSize = 4242; r.srtMtime = 133000000040000000ULL;
    r.cues.push_back(mkCue(0, 1000, 3500, "en", "zh-tw", "Hello.", "\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82"));
    r.cues.push_back(mkCue(1, 4000, 6000, "en", "zh-tw", nasty, nasty));
    r.cues.push_back(mkCue(2, 6000, 6000, "zh-tw", "zh-tw", "\xE5\xA5\xBD", "\xE5\xA5\xBD", true));
    const std::string packed = pbtrf::packRecord(r);
    {
        pbtrf::Record q;
        const bool parsed = pbtrf::parseRecord(packed, q);
        ok("round trip parses", parsed);
        bool same = parsed && q.path == r.path && q.size == r.size && q.mtime == r.mtime
                 && q.langA == r.langA && q.langB == r.langB && q.mode == r.mode && q.quality == r.quality
                 && q.asrModel == r.asrModel && q.mtModel == r.mtModel && q.hasVideo == r.hasVideo
                 && q.totalMs == r.totalMs && q.srtPath == r.srtPath && q.srtSize == r.srtSize
                 && q.srtMtime == r.srtMtime && q.cues.size() == r.cues.size();
        for (size_t i = 0; same && i < r.cues.size(); ++i) {
            const auto& a = r.cues[i]; const auto& b = q.cues[i];
            same = a.idx == b.idx && a.startMs == b.startMs && a.endMs == b.endMs && a.lang == b.lang
                && a.dstLang == b.dstLang && a.pass == b.pass && a.src == b.src && a.dst == b.dst;
        }
        ok("...every field and every cue survives, tabs/newlines/quotes/CJK included", same);
        ok("one cue = one line (escaping kept the record line-oriented; v2: + RNG and DONE; ENGINE: + its line; WH: + AUD)",
           std::count(packed.begin(), packed.end(), '\n') == 7 + 3 + 1 + 1);
    }
    std::printf("== cache record v2 (WE: partial results) ==\n");
    {
        pbtrf::Record q;
        ok("the header is v2 -- a 1.0.5 tray (v1 reader) MISSES it instead of reading a partial as done",
           packed.rfind(std::string(pbtrf::recordHeader()) + "\n", 0) == 0
           && std::string(pbtrf::recordHeader()) != pbtrf::recordHeaderV1()
           && packed.find(pbtrf::recordHeaderV1()) == std::string::npos);
        pbtrf::Record p = r;
        p.complete = false;
        p.ranges = { tp::MsRange{ 60000, 90000 }, tp::MsRange{ 0, 30000 }, tp::MsRange{ 30000, 31000 } };   // unsorted, touching
        p.cues.clear();
        p.cues.push_back(mkCue(5, 61000, 63000, "en", "fr", "late", "tard"));
        p.cues.push_back(mkCue(9, 1000, 3000, "en", "fr", "early", "t\xC3\xB4t"));
        const std::string pp = pbtrf::packRecord(p);
        ok("a PARTIAL record round-trips: ranges merged + sorted, DONE 0", pbtrf::parseRecord(pp, q) && !q.complete
           && q.ranges.size() == 2 && q.ranges[0].s == 0 && q.ranges[0].e == 31000 && q.ranges[1].s == 60000,
           pp.substr(0, 400));
        ok("...its cues come back in START order, idx renumbered 0..n-1", q.cues.size() == 2 && q.cues[0].src == "early"
           && q.cues[0].idx == 0 && q.cues[1].src == "late" && q.cues[1].idx == 1);
        std::string outside = pp;
        const size_t fc = outside.find("FCUE\t");
        outside.insert(fc, "FCUE\t1\t7\t45000\t46000\ten\tfr\t0\tgap\tgap\n");
        outside.replace(outside.rfind("END\t2"), 5, "END\t3");
        ok("a partial record with a cue NO range vouches for is a miss (it would come back twice)", !pbtrf::parseRecord(outside, q));
        auto withRng = [&](const std::string& rng) {
            std::string s = pp;
            const size_t a = s.find("RNG\t");
            s.replace(a, s.find('\n', a) - a, "RNG\t" + rng);
            return s;
        };
        ok("a malformed RNG is a miss (5-3, a-b, overlapping, junk)", !pbtrf::parseRecord(withRng("5-3"), q)
           && !pbtrf::parseRecord(withRng("a-b"), q) && !pbtrf::parseRecord(withRng("0-40000,30000-90000"), q)
           && !pbtrf::parseRecord(withRng("0-31000,60000-90000x"), q));
        ok("...and the canonical one parses", pbtrf::parseRecord(withRng("0-31000,60000-90000"), q));
        pbtrf::Record silent = p;           // a partial result with NO cue (the analysed part was silence)
        silent.cues.clear();
        std::string sp = pbtrf::packRecord(silent);
        ok("a cue-less partial round-trips with its ranges", pbtrf::parseRecord(sp, q) && q.ranges.size() == 2 && q.cues.empty());
        const size_t ra = sp.find("RNG\t");
        sp.replace(ra, sp.find('\n', ra) - ra, "RNG\t0-31000,5-1");
        ok("...and with a malformed RNG it is a MISS (not \"nothing analysed\")", !pbtrf::parseRecord(sp, q));
        std::string noDone = pp;
        noDone.erase(noDone.find("DONE\t"), 7);
        std::string noRng = pp;
        noRng.erase(noRng.find("RNG\t"), noRng.find('\n', noRng.find("RNG\t")) - noRng.find("RNG\t") + 1);
        ok("a v2 record without DONE or without RNG is a miss", !pbtrf::parseRecord(noDone, q) && !pbtrf::parseRecord(noRng, q));
        // a v1 record, as 1.0.5 wrote it
        std::string v1 = std::string(pbtrf::recordHeaderV1()) + "\n";
        v1 += "SRC\t" + tp::esc(r.path) + "\t" + std::to_string(r.size) + "\t" + std::to_string(r.mtime) + "\n";
        v1 += "JOB\ten\tzh-tw\tauto\tauto\tasr\tmt\t1\t6130000\nSRT\t\t0\t0\n";
        v1 += "FCUE\t1\t0\t1000\t3500\ten\tzh-tw\t0\ta\tb\nEND\t1\n";
        ok("a v1 record still reads -- as COMPLETE, covering [0, totalMs]", pbtrf::parseRecord(v1, q) && q.complete
           && q.ranges.size() == 1 && q.ranges[0].s == 0 && q.ranges[0].e == 6130000 && q.cues.size() == 1);
        ok("...and a v1 record carrying an RNG line ignores it (v1 had none)",
           pbtrf::parseRecord(v1.substr(0, v1.find("FCUE")) + "RNG\t5-3\n" + v1.substr(v1.find("FCUE")), q) && q.complete);
        // inRanges / cuesWithin: the boundary rule
        const std::vector<tp::MsRange> rg = { tp::MsRange{ 0, 1000 }, tp::MsRange{ 5000, 6000 } };
        ok("inRanges: [s, e) -- the start counts, the end does not; a gap is outside",
           pbtrf::inRanges(rg, 0) && pbtrf::inRanges(rg, 999) && !pbtrf::inRanges(rg, 1000) && !pbtrf::inRanges(rg, 3000)
           && pbtrf::inRanges(rg, 5000) && !pbtrf::inRanges(rg, 6000) && !pbtrf::inRanges({}, 0));
        std::vector<tp::WireCue> cs = { mkCue(0, 100, 900, "en", "fr", "in", "in"), mkCue(1, 2000, 2500, "en", "fr", "gap", "gap"),
                                        mkCue(2, 5500, 7000, "en", "fr", "in2", "in2") };
        const auto kept = pbtrf::cuesWithin(cs, rg);
        ok("cuesWithin keeps the cues whose START a range vouches for", kept.size() == 2 && kept[0].src == "in" && kept[1].src == "in2");
        // peek
        pbtrf::Summary s;
        ok("peek (complete record): ok, complete, totalMs, no cue read", pbtrf::peekRecordText(packed, s) && s.ok && s.complete
           && s.totalMs == 6130000 && s.path == r.path && s.size == r.size && s.hasVideo);
        ok("peek (partial record): ranges + DONE 0", pbtrf::peekRecordText(pp, s) && s.ok && !s.complete && s.ranges.size() == 2
           && tp::rangesTotal(s.ranges) == 61000);
        ok("peek (v1 record): complete, [0, totalMs]", pbtrf::peekRecordText(v1, s) && s.complete && s.ranges.size() == 1
           && s.ranges[0].e == 6130000);
        ok("peek refuses a foreign header and a malformed RNG", !pbtrf::peekRecordText("# nope\nSRC\tx\t1\t2\n", s)
           && !pbtrf::peekRecordText(withRng("9-1"), s));
        pbtrf::Record big = p;
        big.cues.clear();
        big.ranges = { tp::MsRange{ 0, 900000 } };
        for (uint32_t i = 0; i < 3000; ++i) big.cues.push_back(mkCue(i, 1000ULL + i * 290ULL, 1200ULL + i * 290ULL, "en", "fr", nasty, nasty));
        const std::string bigText = pbtrf::packRecord(big);
        ok("peek reads only the HEAD: a 16 KB window of a big record is enough",
           bigText.size() > 100000 && pbtrf::peekRecordText(bigText.substr(0, 16384), s) && s.ok && !s.complete
           && s.ranges.size() == 1 && s.ranges[0].e == 900000, std::to_string(bigText.size()));
        // the folder key
        const std::string fk = pbtrf::folderKey(L"D:\\Films\\Season 1");
        ok("folderKey: 16 hex, stable, case-insensitive, two folders differ, never a media key",
           fk.size() == 16 && fk == pbtrf::folderKey(L"d:\\films\\SEASON 1") && fk != pbtrf::folderKey(L"D:\\Films\\Season 2")
           && fk != pbtrf::mediaKey(L"D:\\Films\\Season 1", 0, 0));
    }
    {
        pbtrf::Record q;
        const std::string torn = packed.substr(0, packed.rfind("END\t"));
        ok("a TORN record (no END line) is a miss", !pbtrf::parseRecord(torn, q));
        std::string wrongCount = packed;
        wrongCount.replace(wrongCount.rfind("END\t3"), 5, "END\t4");
        ok("an END count that disagrees is a miss", !pbtrf::parseRecord(wrongCount, q));
        ok("a foreign header is a miss", !pbtrf::parseRecord("# something else\n" + packed.substr(packed.find('\n') + 1), q));
        ok("anything after END is a miss", !pbtrf::parseRecord(packed + "FCUE\t1\t9\t0\t1\ten\tfr\t0\ta\tb\n", q));
        std::string crlf;
        for (char c : packed) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
        ok("CRLF line endings are tolerated (a copy edited in Notepad)", pbtrf::parseRecord(crlf, q) && q.cues.size() == 3);
        ok("an unknown line is ignored, not fatal",
           pbtrf::parseRecord(packed.substr(0, packed.find("SRT\t")) + "FUTURE\tx\n" + packed.substr(packed.find("SRT\t")), q)
           && q.cues.size() == 3);
        std::string noId = packed;
        const size_t sAt = noId.find("SRT\t");
        noId.replace(sAt, noId.find('\n', sAt) - sAt, "SRT\t" + tp::esc(r.srtPath));
        ok("an SRT line WITHOUT the written identity reads as 0/0 (it can match no file: rewritten)",
           pbtrf::parseRecord(noId, q) && q.srtPath == r.srtPath && q.srtSize == 0 && q.srtMtime == 0);
        pbtrf::Record empty = r;
        empty.cues.clear();
        ok("a result with NO cues round-trips (\"no speech found\" is cached too)",
           pbtrf::parseRecord(pbtrf::packRecord(empty), q) && q.cues.empty());
    }

    std::printf("== target / spoken language ==\n");
    {
        std::vector<tp::WireCue> cs;
        cs.push_back(mkCue(0, 0, 1, "en", "fr", "a", "b"));
        cs.push_back(mkCue(1, 1, 2, "en", "fr", "a", "b"));
        cs.push_back(mkCue(2, 2, 3, "fr", "en", "a", "b"));
        ok("a2b -> B", pbtrf::targetLang("a2b", "en", "fr", cs) == "fr");
        ok("b2a -> A", pbtrf::targetLang("b2a", "en", "fr", cs) == "en");
        ok("auto -> the language most subtitles are IN", pbtrf::targetLang("auto", "fr", "en", cs) == "fr");
        std::vector<tp::WireCue> tie(cs.begin(), cs.begin() + 1);
        tie.push_back(mkCue(1, 1, 2, "fr", "en", "a", "b"));
        ok("a tie goes to B", pbtrf::targetLang("auto", "fr", "en", tie) == "en"
           && pbtrf::targetLang("auto", "en", "fr", tie) == "fr");
        ok("no cues -> B", pbtrf::targetLang("auto", "en", "zh-tw", {}) == "zh-tw");
        ok("spoken language = the majority of what was SAID", pbtrf::spokenLang(cs, "zz") == "en");
    }

    std::printf("== registry + SRT naming ==\n");
    {
        const std::wstring mp4 = L"D:\\Films\\Film.mp4", wav = L"D:\\Films\\Film.wav";
        std::vector<pbtrf::RegEntry> reg;
        pbtrf::registryPut(reg, L"D:\\Films\\Film.fr.srt", 100, 5, mp4);
        ok("ours when path, size, time AND media all match (case-insensitively on both paths)",
           pbtrf::registryOwns(reg, L"d:\\films\\FILM.FR.SRT", 100, 5, L"d:\\FILMS\\film.MP4"));
        ok("NOT ours once its size changed (the user edited it)", !pbtrf::registryOwns(reg, L"D:\\Films\\Film.fr.srt", 101, 5, mp4));
        ok("NOT ours once its write time changed", !pbtrf::registryOwns(reg, L"D:\\Films\\Film.fr.srt", 100, 6, mp4));
        ok("NOT ours FOR ANOTHER MEDIA (Film.wav must not claim Film.mp4's subtitles)",
           !pbtrf::registryOwns(reg, L"D:\\Films\\Film.fr.srt", 100, 5, wav));
        pbtrf::registryPut(reg, L"D:\\Films\\Film.fr.srt", 120, 9, mp4);
        ok("a re-write REPLACES the entry (one per path)", reg.size() == 1 && pbtrf::registryOwns(reg, L"D:\\Films\\Film.fr.srt", 120, 9, mp4));
        pbtrf::registryPut(reg, L"E:\\Tab\tname.srt", 1, 2, L"E:\\Tab\tname.mkv");
        std::vector<pbtrf::RegEntry> back = pbtrf::parseRegistry(pbtrf::packRegistry(reg));
        ok("pack/parse round trip (a tab in a path survives, the media too)", back.size() == 2
           && pbtrf::registryOwns(back, L"E:\\Tab\tname.srt", 1, 2, L"E:\\Tab\tname.mkv"));
        const std::vector<pbtrf::RegEntry> old = pbtrf::parseRegistry("# x\nd:\\films\\film.fr.srt\t100\t5\n");
        ok("an entry WITHOUT a media (older registry) owns nothing", old.size() == 1
           && !pbtrf::registryOwns(old, L"D:\\Films\\Film.fr.srt", 100, 5, mp4));
        std::vector<pbtrf::RegEntry> many;
        for (int i = 0; i < 1005; ++i) pbtrf::registryPut(many, L"C:\\s\\" + std::to_wstring(i) + L".srt", 1, 1, mp4);
        back = pbtrf::parseRegistry(pbtrf::packRegistry(many));
        ok("capped at 1000, keeping the NEWEST", back.size() == 1000
           && pbtrf::registryOwns(back, L"C:\\s\\1004.srt", 1, 1, mp4) && !pbtrf::registryOwns(back, L"C:\\s\\0.srt", 1, 1, mp4));

        auto no  = [](const std::wstring&) { return false; };
        auto yes = [](const std::wstring&) { return true; };
        ok("absent -> <base>.<tag>.srt",
           pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"zh-TW", false, no, no) == L"D:\\Films\\Film.zh-TW.srt");
        ok("present and OURS FOR THIS MEDIA -> the same name (a re-analysis overwrites our own file)",
           pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"fr", false, yes, yes) == L"D:\\Films\\Film.fr.srt");
        auto onlyPlain = [](const std::wstring& p) { return p.find(L".suvtitle") == std::wstring::npos; };
        ok("present and NOT this media's -> <base>.<tag>.suvtitle.srt (theirs is left alone)",
           pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"fr", false, onlyPlain, no) == L"D:\\Films\\Film.fr.suvtitle.srt");
        {
            // .suvtitle.srt is tested exactly like the plain name (review: it used to be overwritten
            // unconditionally): taken and not ours -> -2; -2 ours -> -2 again.
            auto taken2 = [](const std::wstring& p) { return p.find(L".suvtitle-") == std::wstring::npos; };
            ok("...and when .suvtitle.srt is taken too (not ours) -> .suvtitle-2.srt",
               pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"fr", false, taken2, no) == L"D:\\Films\\Film.fr.suvtitle-2.srt");
            auto ours2 = [](const std::wstring& p) { return p.find(L".suvtitle-2.") != std::wstring::npos; };
            ok("...-2 OURS for this media is re-used, not -3",
               pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"fr", false, yes, ours2) == L"D:\\Films\\Film.fr.suvtitle-2.srt");
            ok("every candidate taken and foreign -> NO name (never an overwrite)",
               pbtrf::chooseSrtPath(L"D:\\Films", L"Film", L"mp4", L"fr", false, yes, no).empty()
               && pbtrf::srtCandidates(L"D:\\Films", L"Film", L"mp4", L"fr", false).size() == 10);
        }
        ok("ANOTHER MEDIA with the same base in the folder -> the extension joins the base",
           pbtrf::chooseSrtPath(L"D:\\Films", L"Interview_01", L"wav", L"fr", true, no, no) == L"D:\\Films\\Interview_01.wav.fr.srt");
        ok("a drive root joins without a doubled backslash",
           pbtrf::chooseSrtPath(L"C:\\", L"Film", L"mp4", L"fr", false, no, no) == L"C:\\Film.fr.srt");
        const std::vector<std::wstring> folder = { L"Interview_01.WAV", L"Interview_01.mp4", L"Interview_01.fr.srt", L"notes.txt" };
        ok("sibling media: Interview_01.mp4 sees Interview_01.WAV (case-insensitive), and vice versa",
           pbtrf::hasSiblingMedia(folder, L"Interview_01.mp4") && pbtrf::hasSiblingMedia(folder, L"interview_01.wav"));
        ok("...a subtitle or a text file with the same base is NOT a sibling medium",
           !pbtrf::hasSiblingMedia({ L"Film.mp4", L"Film.fr.srt", L"Film.txt", L"Film.srt" }, L"Film.mp4"));
    }

    std::printf("== media list, media key, path identity ==\n");
    {
        ok("the media list: video and audio extensions, any case", pbtrf::isMediaExt(L"mp4") && pbtrf::isMediaExt(L"MKV")
           && pbtrf::isMediaExt(L"wav") && pbtrf::isMediaExt(L"aiff") && pbtrf::isMediaExt(L"mts"));
        ok("...and not subtitles, text, images", !pbtrf::isMediaExt(L"srt") && !pbtrf::isMediaExt(L"txt")
           && !pbtrf::isMediaExt(L"vtt") && !pbtrf::isMediaExt(L"jpg") && !pbtrf::isMediaExt(L""));
        const std::string k = pbtrf::mediaKey(L"D:\\Films\\Film.mp4", 10, 20);
        ok("media key: 16 hex, the same for the same identity (path case aside)",
           k.size() == 16 && k == pbtrf::mediaKey(L"d:\\films\\FILM.MP4", 10, 20));
        ok("...and a different one for another size, write time or file",
           k != pbtrf::mediaKey(L"D:\\Films\\Film.mp4", 11, 20) && k != pbtrf::mediaKey(L"D:\\Films\\Film.mp4", 10, 21)
           && k != pbtrf::mediaKey(L"D:\\Films\\Film.wav", 10, 20));
        ok("a record's SRC matches the file in hand case-insensitively (the key already does)",
           pbtrf::samePath("D:\\Films\\Mon film \xC3\x89t\xC3\xA9.mkv", L"d:\\films\\MON FILM \u00c9T\u00c9.MKV")
           && !pbtrf::samePath("D:\\Films\\A.mkv", L"D:\\Films\\B.mkv"));
    }

    std::printf("== paths ==\n");
    ok("leafOf", pbtrf::leafOf(L"D:\\a\\b c.mp4") == L"b c.mp4");
    ok("dirOf", pbtrf::dirOf(L"D:\\a\\b c.mp4") == L"D:\\a");
    ok("dirOf keeps a drive root's backslash", pbtrf::dirOf(L"C:\\film.mp4") == L"C:\\");
    ok("dirOf on a UNC path", pbtrf::dirOf(L"\\\\nas\\films\\x.mkv") == L"\\\\nas\\films");
    ok("stripExt drops the LAST extension only", pbtrf::stripExt(L"film.2024.mkv") == L"film.2024");
    ok("stripExt leaves a dot-name alone", pbtrf::stripExt(L".hidden") == L".hidden");
    ok("extOf lower-cases", pbtrf::extOf(L"CLIP.MP4") == L"mp4" && pbtrf::extOf(L"noext").empty());

    std::printf("== URL path encoding ==\n");
    ok("unreserved characters pass", pbtrf::pctEncode("a-Z_0.9~") == "a-Z_0.9~");
    ok("space # ? % are encoded (a bare # would end the URL)",
       pbtrf::pctEncode("Mon film #1 ?50%.mp4") == "Mon%20film%20%231%20%3F50%25.mp4",
       pbtrf::pctEncode("Mon film #1 ?50%.mp4"));
    ok("UTF-8 bytes, upper-case hex (e-acute, a CJK ideograph)",
       pbtrf::pctEncode("\xC3\xA9\xE4\xB8\xAD") == "%C3%A9%E4%B8%AD");
    ok("a path separator in a NAME is encoded too", pbtrf::pctEncode("a/b\\c") == "a%2Fb%5Cc");

    std::printf("== the disk ==\n");
    wchar_t tmpRoot[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmpRoot);
    const std::wstring dir = std::wstring(tmpRoot) + L"apb_filestore_smoke_" + std::to_wstring(::GetCurrentProcessId());
    ok("a private temp folder", pbtrf::ensureDir(dir));
    {
        const std::wstring f = pbtrf::joinPath(dir, L"Mon film \u00e9t\u00e9 \u7e41\u9ad4.zh-TW.srt");
        const std::string bytes = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\nhi\r\n\r\n";
        ok("AV-safe write lands (Unicode name)", pbtrf::writeFileAvSafe(f, bytes));
        std::string back;
        ok("...the bytes read back identical", pbtrf::readFileAll(f, back) && back == bytes);
        ok("...and no .tmp is left behind", !pbtrf::fileExists(f + L".tmp"));
        uint64_t sz = 0, mt = 0; bool isDir = true;
        ok("fileIdentity: size + write time, not a folder",
           pbtrf::fileIdentity(f, sz, mt, &isDir) && sz == bytes.size() && mt != 0 && !isDir);
        ok("a re-write replaces it", pbtrf::writeFileAvSafe(f, "x") && pbtrf::readFileAll(f, back) && back == "x");
        {
            // review of WC: the identity of what we wrote comes from US (size written, write time
            // SET on the handle), never from a path lookup after the rename -- and it must be the
            // identity the disk then reports, after the handle closed and the rename moved it.
            pbtrf::WrittenId id;
            ok("a replacing write reports its identity", pbtrf::writeFileAvSafe(f, bytes, true, &id) && id.valid
               && id.size == bytes.size());
            uint64_t s2 = 0, m2 = 0;
            const bool there = pbtrf::fileIdentity(f, s2, m2);   // BEFORE ok(): argument order is unspecified
            ok("...EXACTLY what the disk reports afterwards (size AND write time)",
               there && s2 == id.size && m2 == id.mtime, std::to_string(m2) + " vs " + std::to_string(id.mtime));
            ok("...on an even second (FAT32 keeps 2 s, exFAT 10 ms: the stamp survives both)", id.mtime % 20000000ULL == 0);
            const std::wstring g = pbtrf::joinPath(dir, L"new.fr.srt");
            pbtrf::WrittenId gid;
            ok("a NO-REPLACE write of an absent file lands, with its identity",
               pbtrf::writeFileAvSafe(g, "first", false, &gid) && gid.valid && pbtrf::fileIdentity(g, s2, m2)
               && s2 == gid.size && m2 == gid.mtime);
            ok("a NO-REPLACE write onto a file that APPEARED meanwhile fails and clobbers nothing",
               !pbtrf::writeFileAvSafe(g, "second", false) && pbtrf::readFileAll(g, back) && back == "first"
               && !pbtrf::fileExists(g + L".tmp"));
            ::DeleteFileW(g.c_str());
        }
        ok("a folder that does not exist refuses the write (-> the Documents fallback)",
           !pbtrf::writeFileAvSafe(pbtrf::joinPath(dir, L"no such folder\\x.srt"), "x"));
        bool isDir2 = false;
        ok("fileIdentity says FOLDER for a folder", pbtrf::fileIdentity(dir, sz, mt, &isDir2) && isDir2);
        ok("fileExists is false for a folder", !pbtrf::fileExists(dir));
        ::DeleteFileW(f.c_str());
    }
    {
        for (int i = 0; i < 5; ++i) {
            const std::wstring f = pbtrf::joinPath(dir, L"r" + std::to_wstring(i) + L".cues");
            pbtrf::writeFileAvSafe(f, "x");
            HANDLE h = ::CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME ft; ULARGE_INTEGER u; u.QuadPart = 133000000000000000ULL + (ULONGLONG)i * 10000000ULL;
            ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
            if (h != INVALID_HANDLE_VALUE) { ::SetFileTime(h, nullptr, nullptr, &ft); ::CloseHandle(h); }
        }
        const size_t gone = pbtrf::keepNewest(dir, L"*.cues", 2);
        ok("eviction keeps the NEWEST two records", gone == 3 && !pbtrf::fileExists(pbtrf::joinPath(dir, L"r0.cues"))
           && pbtrf::fileExists(pbtrf::joinPath(dir, L"r3.cues")) && pbtrf::fileExists(pbtrf::joinPath(dir, L"r4.cues")));
        for (int i = 0; i < 5; ++i) ::DeleteFileW(pbtrf::joinPath(dir, L"r" + std::to_wstring(i) + L".cues").c_str());
    }
    {
        // WE: the queue's badges peek at records ON DISK -- the head only.
        pbtrf::Record big = r;
        big.complete = false;
        big.ranges = { tp::MsRange{ 0, 120000 } };
        big.cues.clear();
        for (uint32_t i = 0; i < 2000; ++i) big.cues.push_back(mkCue(i, 100ULL + i * 50ULL, 140ULL + i * 50ULL, "en", "fr", nasty, nasty));
        const std::wstring f = pbtrf::joinPath(dir, L"peek.cues");
        pbtrf::Summary s;
        ok("peekRecord on disk: a 300 KB partial record, read by its first 16 KB",
           pbtrf::writeFileAvSafe(f, pbtrf::packRecord(big)) && pbtrf::peekRecord(f, s) && s.ok && !s.complete
           && s.ranges.size() == 1 && s.ranges[0].e == 120000 && s.totalMs == r.totalMs);
        ok("peekRecord on a missing file: false", !pbtrf::peekRecord(pbtrf::joinPath(dir, L"none.cues"), s) && !s.ok);
        ::DeleteFileW(f.c_str());
    }
    ::RemoveDirectoryW(dir.c_str());
    ok("the temp folder is gone again", ::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES);

    // ---- WG (the user's first real test, 11/10): one file per language, the edited flag, the shared original ----
    std::printf("== WG: per-language files, EDIT, siblings ==\n");
    {
        std::string t;
        const tp::WireCue fr = mkCue(0, 0, 1000, "fr", "en", "Bonjour.", "Hello.");
        const tp::WireCue en = mkCue(1, 1000, 2000, "en", "fr", "Meep!", "Bip !");
        const tp::WireCue ru = mkCue(2, 2000, 3000, "ru", "en", "Privet", "Hi there");
        const tp::WireCue ps = mkCue(3, 3000, 4000, "fr", "fr", "Oui.", "Oui.", true);
        ok("textInLang fr: a French source, the translation INTO French, a passthrough -- not a Russian line",
           pbtrf::textInLang(fr, "fr", t) && t == "Bonjour." && pbtrf::textInLang(en, "fr", t) && t == "Bip !"
           && pbtrf::textInLang(ps, "FR", t) && t == "Oui." && !pbtrf::textInLang(ru, "fr", t));
        ok("textInLang en: the English source, the translations into English", pbtrf::textInLang(fr, "en", t) && t == "Hello."
           && pbtrf::textInLang(en, "en", t) && t == "Meep!" && pbtrf::textInLang(ru, "en", t) && t == "Hi there"
           && !pbtrf::textInLang(ps, "en", t));
        tp::WireCue empty = mkCue(4, 4000, 5000, "fr", "en", "", "Nothing heard");
        ok("...an EMPTY text is no text (the other language's may still be)", !pbtrf::textInLang(empty, "fr", t)
           && pbtrf::textInLang(empty, "en", t) && t == "Nothing heard");
        ok("countInLang", pbtrf::countInLang({ fr, en, ru, ps }, "fr") == 3 && pbtrf::countInLang({ fr, en, ru, ps }, "en") == 3
           && pbtrf::countInLang({ ru }, "fr") == 0);
        ok("fileLangTag: zh -> zh-CN, zh-tw -> zh-TW (any case), the rest lower case",
           pbtrf::fileLangTag("zh") == "zh-CN" && pbtrf::fileLangTag("ZH-TW") == "zh-TW" && pbtrf::fileLangTag("En") == "en"
           && pbtrf::fileLangTag("ko") == "ko");
        ok("pairLangs: both halves once, lower case", pbtrf::pairLangs("FR", "en") == std::vector<std::string>({ "fr", "en" })
           && pbtrf::pairLangs("fr", "FR").size() == 1);
        // the record: SRTL lines and EDIT 1
        pbtrf::Record w;
        w.path = "D:\\F\\f.mkv"; w.size = 9; w.mtime = 8; w.langA = "fr"; w.langB = "en"; w.mode = "auto"; w.quality = "auto";
        w.totalMs = 60000; w.complete = true; w.ranges = { tp::MsRange{ 0, 60000 } }; w.edited = true;
        w.srtFiles.push_back(pbtrf::SrtFile{ "fr", "D:\\F\\f.fr.srt", 120, 1000 });
        w.srtFiles.push_back(pbtrf::SrtFile{ "zh-tw", "D:\\F\\f.zh-TW \xE7\xB9\x81.srt", 140, 1002 });
        w.cues = { fr, en };
        const std::string packedW = pbtrf::packRecord(w);
        pbtrf::Record back;
        ok("a record with two SRTL lines and EDIT 1 round-trips (paths with spaces and CJK)", pbtrf::parseRecord(packedW, back)
           && back.edited && back.srtFiles.size() == 2 && back.srtFiles[1].lang == "zh-tw" && back.srtFiles[1].path == w.srtFiles[1].path
           && back.srtFiles[0].size == 120 && back.srtFiles[1].mtime == 1002, packedW.substr(0, 400));
        pbtrf::Summary sm;
        ok("...the peek sees the pair and the edit (the head only)", pbtrf::peekRecordText(packedW, sm) && sm.ok && sm.edited
           && sm.langA == "fr" && sm.langB == "en");
        w.edited = false;
        ok("...not edited: no EDIT line at all (an older tray reads the same record)", pbtrf::packRecord(w).find("EDIT") == std::string::npos);
        std::string legacy = packedW;
        const size_t a0 = legacy.find("SRTL"), a1 = legacy.find("RNG");
        legacy.erase(a0, a1 - a0);
        ok("a WE record (no SRTL, no EDIT) still reads: no files, not edited", pbtrf::parseRecord(legacy, back) && back.srtFiles.empty() && !back.edited);
        // the siblings of "fr": every other analysis of the file whose pair holds fr
        const auto keys = pbtrf::siblingKeys(L"D:\\F\\f.mkv", 9, 8, "fr");
        bool hasFrZh = false, hasZhFr = false, hasFrFr = false, hasEnDe = false;
        for (const auto& k : keys) {
            if (k.langA == "fr" && k.langB == "zh" && k.key == pbtrf::cacheKey(L"D:\\F\\f.mkv", 9, 8, "fr", "zh", k.mode, k.quality)) hasFrZh = true;
            if (k.langA == "zh" && k.langB == "fr") hasZhFr = true;
            if (k.langA == "fr" && k.langB == "fr") hasFrFr = true;
            if (k.langA != "fr" && k.langB != "fr") hasEnDe = true;
        }
        ok("siblingKeys(fr): 12 other languages x 2 orders x 3 directions x 2 qualities = 144, both orders, never fr<->fr, all hold fr",
           keys.size() == 144 && hasFrZh && hasZhFr && !hasFrFr && !hasEnDe, std::to_string(keys.size()));
        ok("...a code beyond the table (the pair in use) joins the candidates", pbtrf::siblingKeys(L"D:\\F\\f.mkv", 9, 8, "fr", { "yue" }).size() == 156);
        ok("coverage: complete beats any partial; a partial = its ranges' total",
           pbtrf::coverage(true, {}) > pbtrf::coverage(false, { tp::MsRange{ 0, 99999999 } })
           && pbtrf::coverage(false, { tp::MsRange{ 0, 1000 }, tp::MsRange{ 5000, 6000 } }) == 2000);
        int known = 0;
        for (const char* const* p = pbtrf::knownLangCodes(); *p; ++p) ++known;
        ok("the language table holds the thirteen codes of translator.html's TR_LANGS", known == 13);
    }

    // ---- ENGINE (2026-10-11): the cue engine's version in every record; an older one is a miss unless EDITED ----------
    std::printf("== ENGINE: the cue engine's version (TrPipe.h kFileEngineVersion = %d) ==\n", tp::kFileEngineVersion);
    {
        pbtrf::Record e;
        e.path = "D:\\F\\g.mkv"; e.size = 7; e.mtime = 6; e.langA = "fr"; e.langB = "en"; e.mode = "auto"; e.quality = "auto";
        e.totalMs = 60000; e.complete = true; e.ranges = { tp::MsRange{ 0, 60000 } };
        e.cues = { mkCue(0, 0, 1000, "fr", "en", "Un.", "One.") };
        e.engine = tp::kFileEngineVersion;
        const std::string packedE = pbtrf::packRecord(e);
        pbtrf::Record back;
        const size_t atJob = packedE.find("\nJOB\t"), atEng = packedE.find("\nENGINE\t"), atCue = packedE.find("\nFCUE");
        ok("a record carries ENGINE <n>, right after JOB (inside what a peek reads, before the cues)",
           atEng != std::string::npos && atJob < atEng && atEng < atCue
           && packedE.find("\nENGINE\t" + std::to_string(tp::kFileEngineVersion) + "\n") != std::string::npos, packedE.substr(0, 300));
        ok("...and reads back (parse and peek)", pbtrf::parseRecord(packedE, back) && back.engine == tp::kFileEngineVersion);
        pbtrf::Summary sm;
        ok("...the peek sees it too", pbtrf::peekRecordText(packedE, sm) && sm.ok && sm.engine == tp::kFileEngineVersion);
        std::string noEng = packedE;
        noEng.erase(atEng + 1, packedE.find('\n', atEng + 1) - atEng);
        ok("a record WITHOUT the line (every record before it: 1.0.6, WG's first days) reads -- its engine 0",
           noEng.find("ENGINE") == std::string::npos && pbtrf::parseRecord(noEng, back) && back.engine == 0 && back.cues.size() == 1
           && pbtrf::peekRecordText(noEng, sm) && sm.engine == 0);
        auto withValue = [&](const char* v) {
            std::string s = packedE;
            const size_t a = s.find("\nENGINE\t") + 8;
            s.replace(a, s.find('\n', a) - a, v);
            return pbtrf::parseRecord(s, back) ? back.engine : -1;
        };
        ok("ENGINE values: digits only, 1 to 6 of them -- anything else reads as 0 (an older engine), the record still read",
           withValue("4") == 4 && withValue("12") == 12 && withValue("3x") == 0 && withValue("-1") == 0 && withValue("") == 0
           && withValue("1234567") == 0 && withValue(" 3") == 0);
        ok("pbtrf::engineOf directly", pbtrf::engineOf("3") == 3 && pbtrf::engineOf("0003") == 3 && pbtrf::engineOf("x") == 0);
        const int cur = tp::kFileEngineVersion;
        ok("olderEngine: below this build's engine (or 0 = no line) is older; this one and a NEWER one are not",
           pbtrf::olderEngine(0) && pbtrf::olderEngine(cur - 1) && !pbtrf::olderEngine(cur) && !pbtrf::olderEngine(cur + 1));
        ok("staleRecord (= a cache MISS): older AND not edited -- an EDITED older record is never a miss",
           pbtrf::staleRecord(0, false) && pbtrf::staleRecord(cur - 1, false) && !pbtrf::staleRecord(cur - 1, true)
           && !pbtrf::staleRecord(0, true) && !pbtrf::staleRecord(cur, false) && !pbtrf::staleRecord(cur + 1, false));
        // review of WG #5: the subtitle-file editor's overlap count
        using SP = std::vector<std::pair<uint64_t, uint64_t>>;
        ok("overlapPairs: none / touching is not overlapping / one pair / three spans all over each other = 3 pairs",
           pbtrf::overlapPairs(SP{ { 0, 10 }, { 20, 30 } }) == 0 && pbtrf::overlapPairs(SP{ { 0, 10 }, { 10, 20 } }) == 0
           && pbtrf::overlapPairs(SP{ { 0, 10 }, { 5, 15 } }) == 1 && pbtrf::overlapPairs(SP{ { 0, 100 }, { 10, 20 }, { 15, 30 } }) == 3);
        ok("...the same start, unsorted input, a long one over many short ones",
           pbtrf::overlapPairs(SP{ { 5, 9 }, { 5, 7 } }) == 1 && pbtrf::overlapPairs(SP{ { 30, 40 }, { 0, 35 }, { 50, 60 } }) == 1
           && pbtrf::overlapPairs(SP{ { 0, 1000 }, { 10, 20 }, { 30, 40 }, { 50, 60 } }) == 3);
        std::string v1 = "# AudioPB Translator file cache v1\nSRC\tD:\\\\F\\\\g.mkv\t7\t6\nJOB\tfr\ten\tauto\tauto\ta\tm\t1\t60000\nSRT\t\t0\t0\nEND\t0\n";
        ok("a v1 record (1.0.5) still reads -- as complete, engine 0 (older: analysed again)",
           pbtrf::parseRecord(v1, back) && back.complete && back.engine == 0 && pbtrf::staleRecord(back.engine, back.edited));
    }

    std::printf("== WH: the sound track (key, AUD marker, registry writer) ==\n");
    {
        const std::wstring P = L"D:\\Films\\clip.mkv";
        const std::string k0 = pbtrf::cacheKey(P, 9, 8, "en", "fr", "auto", "auto");
        ok("the key of ord 0 = the key before WH (\"\\ta<ord>\" only for ord > 0); ords 1 and 2 differ from it and each other",
           pbtrf::cacheKey(P, 9, 8, "en", "fr", "auto", "auto", 0) == k0 && pbtrf::cacheKey(P, 9, 8, "en", "fr", "auto", "auto", 1) != k0
           && pbtrf::cacheKey(P, 9, 8, "en", "fr", "auto", "auto", 2) != pbtrf::cacheKey(P, 9, 8, "en", "fr", "auto", "auto", 1));
        ok("siblingKeys of a track = 144 keys of THAT track (none of them a key of track 0)",
           [&] {
               const auto a = pbtrf::siblingKeys(P, 9, 8, "fr", {}, 1), b0 = pbtrf::siblingKeys(P, 9, 8, "fr", {}, 0);
               if (a.size() != 144 || b0.size() != 144) return false;
               for (const auto& x : a) for (const auto& y : b0) if (x.key == y.key) return false;
               return a[0].key == pbtrf::cacheKey(P, 9, 8, a[0].langA, a[0].langB, a[0].mode, a[0].quality, 1);
           }());
        pbtrf::Record w;
        w.path = "D:\\Films\\clip.mkv"; w.size = 9; w.mtime = 8; w.langA = "en"; w.langB = "fr"; w.mode = "auto"; w.quality = "auto";
        w.engine = tp::kFileEngineVersion; w.complete = true; w.totalMs = 1000; w.ranges.push_back(tp::MsRange{ 0, 1000 });
        w.audioOrd = 2; w.audioTrack = 4;
        const std::string pw = pbtrf::packRecord(w);
        pbtrf::Record back;
        pbtrf::Summary sm;
        ok("EVERY record carries its track marker (AUD <ord> <track>), right after ENGINE -- inside a peek's 16 KB",
           pw.find("\nENGINE\t" + std::to_string(tp::kFileEngineVersion) + "\nAUD\t2\t4\n") != std::string::npos
           && pbtrf::packRecord(pbtrf::Record{}).find("\nAUD\t0\t0\n") != std::string::npos);
        ok("...and reads back (audioOrd, audioTrack, trackKnown) in the record and the peek",
           pbtrf::parseRecord(pw, back) && back.audioOrd == 2 && back.audioTrack == 4 && back.trackKnown
           && pbtrf::peekRecordText(pw, sm) && sm.audioOrd == 2 && sm.trackKnown);
        std::string old = pw;
        old.erase(old.find("AUD\t2\t4\n"), 8);
        ok("a record WITHOUT the line (made before WH) reads: trackKnown false, ord 0 -- and is written back WITHOUT it",
           pbtrf::parseRecord(old, back) && !back.trackKnown && back.audioOrd == 0 && pbtrf::peekRecordText(old, sm) && !sm.trackKnown
           && pbtrf::packRecord(back).find("AUD") == std::string::npos);
        ok("unknownTrackMiss: track-less AND unedited AND a multi-track ISO-BMFF file -- nothing else",
           pbtrf::unknownTrackMiss(false, false, true) && !pbtrf::unknownTrackMiss(true, false, true)
           && !pbtrf::unknownTrackMiss(false, true, true) && !pbtrf::unknownTrackMiss(false, false, false));
        // the registry's writer fields
        std::vector<pbtrf::RegEntry> reg;
        pbtrf::registryPut(reg, L"D:\\Films\\clip.fr.srt", 10, 20, P, "fr", "aaaa", 0);
        pbtrf::registryPut(reg, L"D:\\Films\\clip.en.srt", 11, 21, P, "en", "bbbb", 1);
        pbtrf::registryPut(reg, L"D:\\Films\\clip.fr.srt", 12, 22, P, "fr", "cccc", 1);   // rewritten by another record
        pbtrf::registryPut(reg, L"D:\\Films\\export.srt", 13, 23, P);                     // an export: no writer
        const std::string pr = pbtrf::packRegistry(reg);
        const std::vector<pbtrf::RegEntry> rr = pbtrf::parseRegistry(pr);
        const pbtrf::RegEntry* wfr = pbtrf::registryWriter(rr, P, "fr");
        ok("registry: language + writer key + its track round-trip; the NEWEST writer of (media, language) wins; an export has none",
           rr.size() == 3 && wfr && wfr->key == "cccc" && wfr->ord == 1 && wfr->size == 12
           && pbtrf::registryWriter(rr, P, "en") && pbtrf::registryWriter(rr, P, "en")->key == "bbbb"
           && !pbtrf::registryWriter(rr, P, "de") && !pbtrf::registryWriter(rr, L"D:\\Films\\other.mkv", "fr")
           && pr.find("export.srt\t13\t23\td:\\\\films\\\\clip.mkv\n") != std::string::npos, pr);
        const std::string older = "# AudioPB Translator: subtitle files written by AudioPB (path, size, write time, media)\n"
                                  "d:\\\\films\\\\clip.fr.srt\t10\t20\td:\\\\films\\\\clip.mkv\n";
        const std::vector<pbtrf::RegEntry> ro = pbtrf::parseRegistry(older);
        ok("an OLDER registry (4 fields) still reads: the file is ours for its media, it just has no known writer",
           ro.size() == 1 && pbtrf::registryOwns(ro, L"D:\\Films\\clip.fr.srt", 10, 20, P) && !pbtrf::registryWriter(ro, P, "fr"));
    }

    std::printf("\n%s: %d passed, %d failed\n", g_fail ? "FILESTORE SMOKE FAILED" : "FILESTORE SMOKE OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// =============================================================================
// SuVtitle tests/cpp/tracks_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/tracks_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include, WHERE ITS DATA IS: <SUVTITLE_TEST_MEDIA>\wh\ (media, t\gen, t\ref,
// t\inflate_vectors.txt: the WH test set of AudioPB, not part of the repository; the same folder the engine's selftests read) and
// the maintainer's film from SUVTITLE_TEST_FILM. Without SUVTITLE_TEST_MEDIA the whole smoke says NOT CHECKED and exits 0 (a
// failure is a real defect, a missing private data set is not). Same checks, same count.
// FIX ROUND 2026-10-23 (REVIEW_RV1_MediaTracks.md): RV1-1 -- the Cues fast path only from a muxer known to index every
// subtitle block (mkvmerge / Lavf): gen_media.py's "wh-gen" files are walked now, their Cues path is checked on
// "Lavf60" copies made in memory; RV-1's crafted h1_cues_partial.mkv (embedded) must give both cues. RV1-2 -- the
// payload reported after a Cues extraction. The fuzz also runs over the Lavf60 copies and RV-1's two files.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// tracks_smoke -- wave WH (2026-10-11): ui/MediaTracks.h, the film's OWN tracks read by the tray.
//   [1] the raw inflate against Python's zlib (wh/t/inflate_vectors.txt: levels 0-9, fixed/Huffman-only/RLE/filtered,
//       small windows, raw streams; corrupt ones must fail)
//   [2] the four test files of WH_PLAN_pistes (wh/media): every track exactly as the plan lists it; every text
//       subtitle track extracted -- through the Cues AND by a forced walk -- equal, cue for cue (times, text, styles), to
//       SubtitleReader's reading of its SOURCE file (sub_en.srt / sub_fr.ass / sub_de.vtt)
//   [3] synthetic containers (wh/t/gen, gen_media.py): zlib / header stripping / encryption / bzlib, lacing, blocks
//       without duration, an unknown-size Segment and Clusters, a negative block timecode, TrackTimestampScale, SSA,
//       reordered ASS columns, Cues right / wrong / partial, Tracks after the clusters; MP4 co64 + stz2 + an edit list,
//       UTF-16 + styl, wvtt, Macintosh language codes, udta name, elng, QuickTime's two hdlr, fragmented
//   [4] the user's film (READ ONLY, F4): its 5 sound + 8 subtitle tracks; #7 and #8 by the Cues and by a forced walk,
//       identical, and equal to ffmpeg's own extraction (wh/t/ref); timings; #12 (VobSub) = pictures, not extracted;
//       the probe's bytes read; a cancelled walk returns at once
//   [5] HOSTILE INPUT (D9): every test file truncated at many offsets and with random bit flips -- no crash, no hang,
//       nothing read out of bounds (build_tracks_smoke.cmd runs this part again in an AddressSanitizer build)
// ASCII only (no /utf-8, like the product).
// =============================================================================
#include "../../app/MediaTracks.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace pbmedia;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}
static double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
static std::wstring g_wh;    // <SUVTITLE_TEST_MEDIA>\wh\ (the test data); "" = there is none
static std::wstring wh(const wchar_t* rel) { return g_wh + rel; }
static std::string a(const std::wstring& w) { return pbsubr::toUtf8(w); }
static bool readAll(const std::wstring& p, std::string& out) {
    FileSource f(p);
    if (!f.ok() || f.size() > (512ull << 20)) return false;
    out.resize(static_cast<size_t>(f.size()));
    return out.empty() || f.readAt(0, &out[0], out.size());
}
// a cue as one comparable line: "s-e:text" with a styled run as {flags}text, a line break as |
static std::string cueStr(const pbsubr::Cue& c) {
    std::string t = std::to_string(c.startMs) + "-" + std::to_string(c.endMs) + ":";
    for (const auto& r : c.runs) {
        if (r.flags) t += "{" + std::to_string(r.flags) + "}";
        for (char ch : r.text) t += ch == '\n' ? '|' : ch;
        if (r.flags) t += "{/}";
    }
    return t;
}
static std::string cuesStr(const std::vector<pbsubr::Cue>& v) {
    std::string o;
    for (const auto& c : v) o += cueStr(c) + "\n";
    return o;
}
static bool sameCues(const std::vector<pbsubr::Cue>& x, const std::vector<pbsubr::Cue>& y, std::string& why) {
    if (x.size() != y.size()) { why = std::to_string(x.size()) + " vs " + std::to_string(y.size()) + " cues"; return false; }
    for (size_t i = 0; i < x.size(); ++i)
        if (cueStr(x[i]) != cueStr(y[i])) { why = "#" + std::to_string(i) + ": " + cueStr(x[i]) + "  vs  " + cueStr(y[i]); return false; }
    return true;
}
static pbsubr::Result parseSource(const std::wstring& p) {
    std::string b;
    if (!readAll(p, b)) return pbsubr::Result{};
    return pbsubr::parse(b);
}
static std::string audioStr(const Info& i) {
    std::string o;
    for (const auto& x : i.audio)
        o += std::to_string(x.ord) + ":#" + std::to_string(x.track) + " '" + x.name + "' " + x.lang + " " + x.codec + " ch" + std::to_string(x.ch)
           + (x.def ? " def" : "") + (x.play ? " play" : "") + "\n";
    return o;
}
static std::string subsStr(const Info& i) {
    std::string o;
    for (const auto& x : i.subs)
        o += "#" + std::to_string(x.track) + " '" + x.name + "' " + x.lang + " " + x.codec + (x.image ? " image" : "") + (x.def ? " def" : "")
           + (x.forced ? " forced" : "") + (x.ok ? " ok" : " NO") + "\n";
    return o;
}
static SubResult extractPath(const std::wstring& path, const Info& info, uint32_t track, Path p) {
    SubResult r;
    extractFile(path, info, track, r, nullptr, p);
    return r;
}
// ---- the fix round (SuVtitle reviews, 2026-10-23) ----
// the same extraction over bytes in memory (RV-1's crafted files, the gen files renamed)
static SubResult extractMem(const std::string& bytes, uint32_t track, Path p) {
    MemSource ms(bytes.data(), bytes.size());
    const Info info = probe(ms);
    SubResult r;
    extractParsed(ms, info, track, r, nullptr, p);
    return r;
}
static std::string fromHex(const char* h) {
    std::string o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) o += static_cast<char>(std::stoi(std::string(h + i, 2), nullptr, 16));
    return o;
}
// A gen_media.py file with its muxer's name ("wh-gen", in MuxingApp AND WritingApp) replaced by ffmpeg's ("Lavf60"):
// the SAME bytes from a muxer whose Cues are believed (RV1-1). Same length: no size, no position moves. "" = not 2 names.
static std::string asLavf(const std::wstring& p) {
    std::string b;
    if (!readAll(p, b)) return std::string();
    int n = 0;
    for (size_t at = 0; (at = b.find("wh-gen", at)) != std::string::npos; at += 6) { b.replace(at, 6, "Lavf60"); ++n; }
    return n == 2 ? b : std::string();
}
// RV-1's crafted files (scratchpad/review/rv1/mk.py, 2026-10-23), byte for byte: one S_TEXT/UTF8 track, two blocks
// ("First" at 1 s, "Second" at 3 s), Cues BEFORE the Cluster with a CueRelativePosition each -- no MuxingApp/WritingApp.
//   h1_cues_partial.mkv: ONE cue point (the first block only) -- the index is partial
//   h1b_cues_full.mkv:   both blocks indexed (the control)
static const char* kH1Partial =
    "1a45dfa38b4282886d6174726f736b6118538067db1549a966872ad7b1830f42401654ae6b95ae93d78101838111868b535f544558542f55"
    "5446381c53bb6b91bb8fb38203e8b789f78101f1813cf081031f43b6759ae78100a3898103e8004669727374a38a810bb8005365636f6e64";
static const char* kH1bFull =
    "1a45dfa38b4282886d6174726f736b6118538067ec1549a966872ad7b1830f42401654ae6b95ae93d78101838111868b535f544558542f55"
    "5446381c53bb6ba2bb8fb38203e8b789f78101f1814df08103bb8fb3820bb8b789f78101f1814df0810e1f43b6759ae78100a3898103e800"
    "4669727374a38a810bb8005365636f6e64";
// A Source that counts what is read (the probe must stay in the file's head: D1).
struct Counting : Source {
    Source& s;
    uint64_t bytes = 0, calls = 0;
    explicit Counting(Source& x) : s(x) {}
    uint64_t size() const override { return s.size(); }
    bool readAt(uint64_t off, void* d, size_t n) override { bytes += n; ++calls; return s.readAt(off, d, n); }
};

// ---- [5] the fuzz: probe + every extraction of a (mutated) buffer, timed --------------------------------------------
struct Fuzz { size_t runs = 0, probed = 0, extracted = 0; double maxMs = 0; std::string slowest; };
static void fuzzRun(const std::string& bytes, Fuzz& fz, const std::string& what) {
    const auto t0 = std::chrono::steady_clock::now();
    MemSource ms(bytes.data(), bytes.size());
    const Info info = probe(ms);
    if (info.state == Info::State::Ready) ++fz.probed;
    for (const auto& s : info.subs) {
        for (int p = 0; p < 2; ++p) {
            SubResult r;
            extractParsed(ms, info, s.track, r, nullptr, p ? Path::Walk : Path::Auto);
            if (r.ok) ++fz.extracted;
        }
    }
    ++fz.runs;
    const double t = msSince(t0);
    if (t > fz.maxMs) { fz.maxMs = t; fz.slowest = what; }
}
static void fuzzBytes(const std::string& orig, const std::string& name, Fuzz& fz, bool heavy, uint32_t seed);
static void fuzzFile(const std::wstring& path, Fuzz& fz, bool heavy, uint32_t seed) {
    std::string orig;
    if (!readAll(path, orig)) { ok(("fuzz: read " + a(path)).c_str(), false); return; }
    fuzzBytes(orig, a(path), fz, heavy, seed);
}
static void fuzzBytes(const std::string& orig, const std::string& name, Fuzz& fz, bool heavy, uint32_t seed) {
    const size_t n = orig.size();
    // the offsets: every byte of the head and the tail (the indexes: EBML head, SeekHead, Info, Tracks / ftyp, moov),
    // a stride through the middle, and around every subtitle block of a clean extraction (block headers, BlockGroups)
    std::vector<size_t> cuts;
    const size_t edge = heavy ? 16384 : 2048;
    for (size_t k = 0; k < n && k < edge; ++k) cuts.push_back(k);
    for (size_t k = n > edge ? n - edge : 0; k < n; ++k) cuts.push_back(k);
    for (size_t k = edge; k + edge < n; k += heavy ? 997 : 9973) cuts.push_back(k);
    {
        MemSource ms(orig.data(), orig.size());
        const Info info = probe(ms);
        for (const auto& s : info.subs) {
            Extracted ex;
            extract(ms, info, s.track, ex, nullptr, Path::Walk);
            for (uint64_t p : ex.blockPos)
                for (int d = -48; d <= 48; ++d) {
                    const int64_t c = static_cast<int64_t>(p) + d;
                    if (c > 0 && static_cast<uint64_t>(c) < n) cuts.push_back(static_cast<size_t>(c));
                }
            if (info.cuesPos) for (int d = -16; d <= 64; ++d) if (info.cuesPos + d < n) cuts.push_back(static_cast<size_t>(info.cuesPos + d));
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t c : cuts) fuzzRun(orig.substr(0, c), fz, name + " cut@" + std::to_string(c));
    // flips: mostly in the head and tail (where the structure is), some anywhere
    std::mt19937 rng(seed);
    std::string buf = orig;
    const int flips = heavy ? 3000 : 300;
    for (int it = 0; it < flips; ++it) {
        std::vector<std::pair<size_t, uint8_t>> undo;
        const int k = 1 + static_cast<int>(rng() % 4);
        for (int j = 0; j < k && n; ++j) {
            size_t at;
            const unsigned zone = rng() % 4;
            if (zone == 0) at = rng() % n;
            else if (zone == 1 && n > edge) at = n - 1 - (rng() % edge);
            else at = rng() % std::min(n, edge);
            undo.push_back({ at, static_cast<uint8_t>(buf[at]) });
            buf[at] = static_cast<char>(static_cast<uint8_t>(buf[at]) ^ static_cast<uint8_t>(1u << (rng() % 8)));
        }
        fuzzRun(buf, fz, name + " flip#" + std::to_string(it));
        for (auto it2 = undo.rbegin(); it2 != undo.rend(); ++it2) buf[it2->first] = static_cast<char>(it2->second);
    }
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool fuzzOnly = false, heavy = false, noFuzz = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--fuzz") { fuzzOnly = true; heavy = true; }
        if (std::wstring(argv[i]) == L"--heavy") heavy = true;
        if (std::wstring(argv[i]) == L"--no-fuzz") noFuzz = true;    // (cmut's mutants: the checks only)
    }
    std::wstring FILM;                               // SUVTITLE_TEST_FILM: the maintainer's film ("" = none)
    {
        // The data is the WH test set of AudioPB (media, t\gen, t\ref, t\inflate_vectors.txt; ~25 MB, NOT in the repository):
        // <SUVTITLE_TEST_MEDIA>\wh\ (the same folder the engine's selftests read).
        wchar_t env[2048] = {};
        const DWORD ne = ::GetEnvironmentVariableW(L"SUVTITLE_TEST_MEDIA", env, 2048);
        if (ne > 0 && ne < 2048) {
            std::wstring r = env;
            while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
            const std::wstring cand = r + L"\\wh\\";
            if (!r.empty() && ::GetFileAttributesW((cand + L"media").c_str()) != INVALID_FILE_ATTRIBUTES) g_wh = cand;
        }
        wchar_t fenv[2048] = {};
        const DWORD nf = ::GetEnvironmentVariableW(L"SUVTITLE_TEST_FILM", fenv, 2048);
        if (nf > 0 && nf < 2048) FILM = fenv;
    }
    std::printf("tracks_smoke (ui/MediaTracks.h)%s\n", fuzzOnly ? " -- FUZZ ONLY" : "");
    if (g_wh.empty()) {
        std::printf("  NOT CHECKED (set SUVTITLE_TEST_MEDIA): the WH test set (<SUVTITLE_TEST_MEDIA>\\wh\\media, t\\gen, t\\ref, "
                    "t\\inflate_vectors.txt) is not there -- nothing was run\n");
        std::printf("\ntracks_smoke: 0 passed, 0 failed (NOT CHECKED)\n");
        return 0;
    }
    const std::wstring M2 = wh(L"media\\mkv_2a_def2.mkv"), MP = wh(L"media\\mp4_2a_movtext.mp4"), MD = wh(L"media\\mkv_dts_first.mkv"),
                       MW = wh(L"media\\webm_vtt.webm");
    const wchar_t* GEN[] = { L"t\\gen\\zlib_strip.mkv", L"t\\gen\\unknown_seg.mkv", L"t\\gen\\cues_ok.mkv", L"t\\gen\\cues_bad_rel.mkv",
                             L"t\\gen\\cues_no_rel.mkv", L"t\\gen\\tracks_at_end.mkv", L"t\\gen\\synth.mp4", L"t\\gen\\tx3g.mov",
                             L"t\\gen\\frag.mp4" };

    if (!fuzzOnly) {
        // ---- [1] inflate ------------------------------------------------------------------------------------------------
        std::printf("[1] the raw inflate vs Python's zlib\n");
        {
            std::ifstream in(wh(L"t\\inflate_vectors.txt"));
            std::string line, firstBad;
            int good = 0, bad = 0, neg = 0, negBad = 0, flips = 0;
            auto unhex = [](const std::string& h) {
                std::string o;
                for (size_t i = 0; i + 1 < h.size(); i += 2) o += static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
                return o;
            };
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                const size_t p1 = line.find('|'), p2 = line.find('|', p1 + 1);
                const std::string kind = line.substr(0, p1), f1 = unhex(line.substr(p1 + 1, p2 - p1 - 1)), f2 = line.substr(p2 + 1);
                if (kind == "Z" || kind == "R") {
                    const std::string comp = unhex(f2);
                    std::string out;
                    bool okk = kind == "Z" ? zlibInflate(comp, out, 1u << 24)
                                           : inflateRaw(reinterpret_cast<const uint8_t*>(comp.data()), comp.size(), out, 1u << 24);
                    if (okk && out == f1) ++good;
                    else { ++bad; if (firstBad.empty()) firstBad = line.substr(0, 120); }
                } else if (kind == "N") {
                    std::string out;
                    ++neg;
                    if (zlibInflate(f1, out, 1u << 24)) { ++negBad; if (firstBad.empty()) firstBad = "accepted: " + f2; }
                } else if (kind == "F") {
                    std::string out;
                    zlibInflate(f1, out, 1u << 24);       // fails or not: never crashes, never over the cap
                    ++flips;
                }
            }
            ok("every zlib/raw stream inflates to exactly Python's input (levels 0/1/6/9, fixed, Huffman-only, RLE, filtered, 512 B window)",
               good >= 80 && bad == 0, std::to_string(good) + " good, " + std::to_string(bad) + " bad, first: " + firstBad);
            ok("every corrupt stream is REFUSED (truncated, bad header, bad Adler, preset dictionary, CM 7, block type 3, empty)",
               neg >= 8 && negBad == 0, std::to_string(negBad) + " of " + std::to_string(neg) + " accepted");
            ok("40 randomly corrupted streams: no crash", flips == 40);
            ok("the output cap holds: a stream that inflates past the cap fails (100 000 zero bytes, cap 4096)", [&] {
                std::ifstream in2(wh(L"t\\inflate_vectors.txt"));
                std::string l;
                while (std::getline(in2, l)) {
                    if (l.compare(0, 2, "Z|") != 0) continue;
                    const size_t q1 = l.find('|'), q2 = l.find('|', q1 + 1);
                    if (q2 - q1 - 1 != 200000) continue;           // the 100 000 zero bytes (hex = 200 000 chars)
                    const std::string c = unhex(l.substr(q2 + 1));
                    std::string o;
                    return !zlibInflate(c, o, 4096) && zlibInflate(c, o, 100000) && o.size() == 100000;
                }
                return false;
            }());
        }

        // ---- [2] the four test files -------------------------------------------------------------------------------------
        std::printf("[2] the plan's four test files: tracks and every text subtitle\n");
        const pbsubr::Result srcEn = parseSource(wh(L"media\\sub_en.srt")), srcFr = parseSource(wh(L"media\\sub_fr.ass")),
                             srcDe = parseSource(wh(L"media\\sub_de.vtt"));
        ok("(the sources read: 3 English cues, 4 French (the {\\p1} drawing line dropped by the reader), 2 German)",
           srcEn.ok && srcEn.cues.size() == 3 && srcFr.ok && srcFr.cues.size() == 4 && srcDe.ok && srcDe.cues.size() == 2,
           cuesStr(srcFr.cues));
        {
            const Info i = probeFile(M2);
            ok("mkv_2a_def2: Matroska, H.264 640x360", i.state == Info::State::Ready && i.container == "mkv" && i.hasVideo
               && i.video.codec == "H.264" && i.video.w == 640 && i.video.h == 360);
            ok("mkv_2a_def2: audio #2 eng 'English speech' (FlagDefault 0) + #3 chi 'Chinese speech' (no FlagDefault = default), both AAC playable",
               audioStr(i) == "0:#2 'English speech' eng AAC ch1 play\n1:#3 'Chinese speech' chi AAC ch1 def play\n", audioStr(i));
            ok("mkv_2a_def2: subs #4 SRT eng, #5 ASS fre 'French ASS' FORCED, #6 WebVTT ger -- all readable",
               subsStr(i) == "#4 '' eng SRT ok\n#5 'French ASS' fre ASS forced ok\n#6 '' ger WebVTT ok\n", subsStr(i));
            std::string why;
            const SubResult a4 = extractPath(M2, i, 4, Path::Auto), w4 = extractPath(M2, i, 4, Path::Walk);
            ok("#4 (S_TEXT/UTF8) through the Cues == sub_en.srt, cue for cue (times, text, the <i>world</i>)",
               a4.ok && a4.ex.viaCues && sameCues(a4.parsed.cues, srcEn.cues, why), why + a4.err);
            ok("#4 by a forced walk: the same", w4.ok && !w4.ex.viaCues && sameCues(w4.parsed.cues, srcEn.cues, why), why + w4.err);
            const SubResult a5 = extractPath(M2, i, 5, Path::Auto), w5 = extractPath(M2, i, 5, Path::Walk);
            ok("#5 (S_TEXT/ASS) through the Cues == sub_fr.ass (header + Dialogue rebuilt in the Format order)",
               a5.ok && a5.ex.viaCues && sameCues(a5.parsed.cues, srcFr.cues, why), why + a5.err);
            ok("#5 by a forced walk: the same", w5.ok && sameCues(w5.parsed.cues, srcFr.cues, why), why + w5.err);
            bool sign = false, drawing = false;
            for (const auto& c : a5.parsed.cues) {
                if (c.startMs == 5000 && c.endMs == 6000 && cueStr(c) == "5000-6000:PANNEAU") sign = true;
                if (c.startMs == 7500) drawing = true;
            }
            ok("#5: the {\\pos} SIGN is kept as text (5000-6000 PANNEAU); the {\\p1} DRAWING line is dropped (nothing at 7500)",
               sign && !drawing && a5.ex.blocks == 5, cuesStr(a5.parsed.cues));
            const SubResult a6 = extractPath(M2, i, 6, Path::Auto), w6 = extractPath(M2, i, 6, Path::Walk);
            ok("#6 (D_WEBVTT/SUBTITLES: \"<id>\\n<settings>\\n<text>\") == sub_de.vtt, Cues and walk",
               a6.ok && w6.ok && sameCues(a6.parsed.cues, srcDe.cues, why) && sameCues(w6.parsed.cues, srcDe.cues, why), why + a6.err);
            ok("(read as UTF-8, never sniffed: the reader's code page 65001, format as synthesized)",
               a4.parsed.codepage == 65001 && a4.parsed.format == pbsubr::Format::Srt && a5.parsed.format == pbsubr::Format::Ass
               && a6.parsed.format == pbsubr::Format::Vtt);
        }
        {
            const Info i = probeFile(MP);
            ok("mp4_2a_movtext: MP4 (moov AFTER mdat), H.264 640x360", i.state == Info::State::Ready && i.container == "mp4"
               && i.video.codec == "H.264" && i.video.w == 640);
            ok("mp4_2a_movtext: audio eng (tkhd enabled) + chi (not enabled); hdlr 'SoundHandler' is no name",
               audioStr(i) == "0:#2 '' eng AAC ch1 def play\n1:#3 '' chi AAC ch1 play\n", audioStr(i));
            ok("mp4_2a_movtext: mov_text (tx3g) eng, readable", subsStr(i) == "#4 '' eng Timed Text def ok\n", subsStr(i));
            std::string why;
            const SubResult r = extractPath(MP, i, 4, Path::Auto);
            ok("tx3g == sub_en.srt, cue for cue -- its 'styl' box puts the italic back on \"world\"; empty samples are gaps",
               r.ok && sameCues(r.parsed.cues, srcEn.cues, why) && r.ex.blocks == 7, why + r.err);
        }
        {
            const Info i = probeFile(MD);
            ok("mkv_dts_first: audio #2 DTS eng (default, NOT playable) + #3 AAC chi (playable); no subtitles",
               audioStr(i) == "0:#2 '' eng DTS ch2 def\n1:#3 '' chi AAC ch1 play\n" && i.subs.empty(), audioStr(i));
        }
        {
            const Info i = probeFile(MW);
            ok("webm_vtt: WebM, VP9, audio Opus (FlagDefault 0), subs WebVTT ger",
               i.container == "webm" && i.video.codec == "VP9" && audioStr(i) == "0:#2 '' und Opus ch1 play\n"
               && subsStr(i) == "#3 '' ger WebVTT ok\n", audioStr(i) + subsStr(i));
            std::string why;
            const SubResult a3 = extractPath(MW, i, 3, Path::Auto), w3 = extractPath(MW, i, 3, Path::Walk);
            ok("webm #3 == sub_de.vtt through the Cues and by a walk", a3.ok && a3.ex.viaCues && w3.ok && !w3.ex.viaCues
               && sameCues(a3.parsed.cues, srcDe.cues, why) && sameCues(w3.parsed.cues, srcDe.cues, why), why + a3.err);
        }

        // ---- [3] synthetic containers ------------------------------------------------------------------------------------
        std::printf("[3] synthetic containers (wh/t/gen)\n");
        auto expectCues = [&](const char* name, const std::wstring& f, const Info& i, uint32_t track, const std::string& want,
                              bool wantViaCues, size_t laced = 0) {
            const SubResult r = extractPath(f, i, track, Path::Auto), w = extractPath(f, i, track, Path::Walk);
            const std::string got = cuesStr(r.parsed.cues), gotW = cuesStr(w.parsed.cues);
            ok(name, r.ok && w.ok && got == want && gotW == want && r.ex.viaCues == wantViaCues && r.ex.laced == laced,
               "auto[" + got + "] walk[" + gotW + "] viaCues=" + std::to_string(r.ex.viaCues) + " laced=" + std::to_string(r.ex.laced)
               + " " + r.err + w.err);
        };
        {
            const std::wstring f = wh(GEN[0]);
            const Info i = probeFile(f);
            ok("zlib_strip.mkv: audio DTS (default, unplayable) / Opus fre / PCM big-endian (FlagEnabled 0: not default) / AAC jpn default",
               audioStr(i) == "0:#2 'DTS 5.1' eng DTS ch6 def\n1:#3 'Opus FR' fre Opus ch2 play\n2:#4 '' ger PCM (big-endian) ch2\n"
                              "3:#5 '' jpn AAC ch2 def play\n", audioStr(i));
            const std::string subs = subsStr(i);
            ok("zlib_strip.mkv: zlib / strip / zlib-ASS readable; ENCRYPTED, bzlib, PGS listed but not readable; pt-BR from "
               "LanguageBCP47; 'Signs' forced",
               subs == "#10 '' eng SRT def ok\n#11 '' fre SRT def ok\n#12 '' ger SRT def NO\n#13 '' spa ASS def ok\n#14 '' ita SRT def NO\n"
                       "#15 '' jpn PGS image def NO\n#16 '' pt-BR SRT def ok\n#17 'nodur' eng SRT def ok\n#18 'laced' eng SRT def ok\n"
                       "#19 '' eng WebVTT def ok\n#20 '' eng WebVTT def ok\n#21 'Signs' eng SRT def forced ok\n", subs);
            const SubTrack* s12 = i.sub(12);
            const SubTrack* s14 = i.sub(14);
            const SubTrack* s15 = i.sub(15);
            ok("...with their sentences (encrypted / compressed / pictures)", s12 && s12->why == errEncrypted() && s14 && s14->why == errCompressed()
               && s15 && s15->why == errImage());
            SubResult r12 = extractPath(f, i, 12, Path::Auto);
            ok("an encrypted track is never extracted: its sentence comes back", !r12.ok && r12.err == errEncrypted(), r12.err);
            expectCues("#10 zlib frames (+ a blank line inside a block collapsed, so the cue is not cut in two)", f, i, 10,
                       "1000-2500:Hello {2}zlib{/}\n12000-13000:Second zlib|after a blank line\n", false);
            expectCues("#11 header stripping: the stripped 'Bon' goes back on", f, i, 11, "1000-3000:Bonjour strip\n", false);
            expectCues("#13 ASS whose CodecPrivate AND frames are zlib (scope 3)", f, i, 13, "1200-3000:{1}Hola{/} zlib, con coma\n", false);
            expectCues("#16 a NEGATIVE block timecode in an UNKNOWN-size cluster (10000 - 500)", f, i, 16,
                       "4000-5000:Ol\xC3\xA1 mundo\n9500-9900:Negative rel\n", false);
            expectCues("#17 no BlockDuration: until the next block (an EMPTY block ends one), the last capped at the file's end",
                       f, i, 17, "1000-2500:Nodur one\n2500-3000:Nodur two\n15000-20000:Nodur last\n", false);
            expectCues("#18 a LACED subtitle block is skipped and counted", f, i, 18, "5000-6000:After the laced one\n", false, 1);
            expectCues("#19 S_TEXT/WEBVTT (the text only)", f, i, 19, "1000-2000:Native VTT|second line\n", false);
            expectCues("#20 D_WEBVTT: id and settings lines dropped, a '-->' in the text defused", f, i, 20, "1000-2000:WebM VTT -> arrow\n", false);
        }
        {
            const std::wstring f = wh(GEN[1]);
            const Info i = probeFile(f);
            ok("unknown_seg.mkv: an UNKNOWN-size Segment, read", i.state == Info::State::Ready && i.subs.size() == 3, subsStr(i));
            expectCues("TrackTimestampScale 0.5 honoured, across two unknown-size Clusters", f, i, 1, "2000-3000:Half speed\n10000-10500:Second half speed\n", false);
            expectCues("SSA with no [Events] section: the default 'Marked' Format line", f, i, 2, "1000-2000:SSA {1}line{/}\n", false);
            expectCues("ASS with a REORDERED Format line (Layer, Style, Start, End...) and a [Fonts] section after [Events]", f, i, 3,
                       "3000-3500:Reordered columns\n", false);
        }
        // SuVtitle fix round (RV1-1): gen_media.py names its muxer "wh-gen" -- not one whose Cues are believed, so these
        // four are WALKED now (same cues); the Cues path itself is checked on their "Lavf60" copies just below.
        expectCues("cues_ok.mkv ('wh-gen': not a muxer whose Cues are believed): the walk, same cues", wh(GEN[2]), probeFile(wh(GEN[2])), 1,
                   "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        expectCues("cues_bad_rel.mkv: a CueRelativePosition on a Void -> refused, the walk (same result)", wh(GEN[3]), probeFile(wh(GEN[3])), 1,
                   "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        expectCues("cues_no_rel.mkv: a cue point WITHOUT CueRelativePosition -> the walk", wh(GEN[4]), probeFile(wh(GEN[4])), 1,
                   "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        expectCues("tracks_at_end.mkv: Tracks AFTER the clusters, found through the SeekHead (walked: 'wh-gen')", wh(GEN[5]), probeFile(wh(GEN[5])), 1,
                   "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        auto expectCuesMem = [&](const char* name, const std::string& bytes, uint32_t track, const std::string& want, bool wantViaCues) {
            const SubResult r = extractMem(bytes, track, Path::Auto), w = extractMem(bytes, track, Path::Walk);
            const std::string got = cuesStr(r.parsed.cues), gotW = cuesStr(w.parsed.cues);
            ok(name, !bytes.empty() && r.ok && w.ok && got == want && gotW == want && r.ex.viaCues == wantViaCues,
               "auto[" + got + "] walk[" + gotW + "] viaCues=" + std::to_string(r.ex.viaCues) + " " + r.err + w.err);
        };
        expectCuesMem("cues_ok.mkv as 'Lavf60': the Cues path, used", asLavf(wh(GEN[2])), 1,
                      "1000-2000:First indexed\n3000-4000:Second indexed\n", true);
        expectCuesMem("cues_bad_rel.mkv as 'Lavf60': a CueRelativePosition on a Void -> refused, the walk (same result)", asLavf(wh(GEN[3])), 1,
                      "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        expectCuesMem("cues_no_rel.mkv as 'Lavf60': a cue point WITHOUT CueRelativePosition -> the walk", asLavf(wh(GEN[4])), 1,
                      "1000-2000:First indexed\n3000-4000:Second indexed\n", false);
        expectCuesMem("tracks_at_end.mkv as 'Lavf60': Tracks AFTER the clusters, found through the SeekHead, the Cues", asLavf(wh(GEN[5])), 1,
                      "1000-2000:First indexed\n3000-4000:Second indexed\n", true);
        {
            // RV1-1 (MEDIUM): a PARTIAL index is never read as the whole track. RV-1's h1_cues_partial.mkv indexes one block
            // of two and names no muxer: before the fix the Cues path won with "First" alone, "Second" lost without a word.
            const std::string h1 = fromHex(kH1Partial), h1b = fromHex(kH1bFull);
            const SubResult a1 = extractMem(h1, 1, Path::Auto), w1 = extractMem(h1, 1, Path::Walk);
            ok("RV1-1: h1_cues_partial.mkv (one block of two indexed, no muxer named) gives BOTH cues, by the walk",
               a1.ok && w1.ok && a1.parsed.cues.size() == 2 && !a1.ex.viaCues && cuesStr(a1.parsed.cues) == cuesStr(w1.parsed.cues)
               && cueStr(a1.parsed.cues[0]).find(":First") != std::string::npos && cueStr(a1.parsed.cues[1]).find(":Second") != std::string::npos,
               "auto[" + cuesStr(a1.parsed.cues) + "] viaCues=" + std::to_string(a1.ex.viaCues) + " walk[" + cuesStr(w1.parsed.cues) + "]" + a1.err);
            const SubResult a2 = extractMem(h1b, 1, Path::Auto);
            ok("RV1-1: h1b_cues_full.mkv (the control, both indexed, no muxer named): both cues, walked",
               a2.ok && a2.parsed.cues.size() == 2 && !a2.ex.viaCues, cuesStr(a2.parsed.cues) + a2.err);
            // >>> new API (the fix round's proof strips this block to build the file over the code BEFORE the fix)
            ok("RV1-1: whose Cues are believed -- mkvmerge (libebml/libmatroska) and Lavf (ffmpeg, HandBrake), nothing else",
               cuesIndexEverySub("libebml v1.4.2 + libmatroska v1.6.4", "mkvmerge v63.0.0 ('Everything') 64-bit")
               && cuesIndexEverySub("Lavf60.3.100", "Lavf60.3.100") && cuesIndexEverySub("Lavf", "Lavf")
               && cuesIndexEverySub("Lavf59.27.100", "HandBrake 1.6.1 2023012300")
               && !cuesIndexEverySub("libebml v0.7.1 + libmatroska v0.7.3", "VirtualDubMod 1.5.10.2 (build 2540/release)")
               && !cuesIndexEverySub("libmakemkv v1.17.4 (1.3.10/1.5.2) win(x64-release)", "MakeMKV v1.17.4 win(x64-release)")
               && !cuesIndexEverySub("", "mkvmerge v63.0.0") && !cuesIndexEverySub("wh-gen", "wh-gen") && !cuesIndexEverySub("", "")
               && !cuesIndexEverySub("x Lavf60", "x"));
            const Info im = probeFile(M2), ig = probeFile(wh(GEN[2]));
            ok("RV1-1: the probe keeps both names (mkv_2a_def2: Lavf60.3.100 twice; gen: wh-gen twice)",
               im.muxingApp == "Lavf60.3.100" && im.writingApp == "Lavf60.3.100" && ig.muxingApp == "wh-gen" && ig.writingApp == "wh-gen",
               im.muxingApp + "|" + im.writingApp + "|" + ig.muxingApp + "|" + ig.writingApp);
            // <<< new API
            // RV1-2 (LOW): the bytes read are reported after a Cues extraction too (it read 0)
            const Info i2 = probeFile(M2);
            const SubResult c4 = extractPath(M2, i2, 4, Path::Auto), k4 = extractPath(M2, i2, 4, Path::Walk);
            ok("RV1-2: Extracted.payload after the Cues path == the walk's (> 0)",
               c4.ok && c4.ex.viaCues && k4.ok && c4.ex.payload > 0 && c4.ex.payload == k4.ex.payload,
               std::to_string(c4.ex.payload) + " vs " + std::to_string(k4.ex.payload));
        }
        {
            const std::wstring f = wh(GEN[6]);
            const Info i = probeFile(f);
            ok("synth.mp4: mp4a/esds 0x40 = AAC (enabled); 0x6B = MP3 with udta name 'Commentary' + elng en-GB; ac-3 (6 ch, fra); "
               "twos = big-endian PCM (not playable); 'Core Media Audio' is no name",
               audioStr(i) == "0:#2 '' eng AAC ch2 def play\n1:#3 'Commentary' en-GB MP3 ch2 play\n2:#4 '' fra AC-3 ch6 play\n"
                              "3:#5 '' eng PCM (big-endian) ch2\n", audioStr(i));
            ok("synth.mp4: tx3g eng (enabled) + wvtt with a MACINTOSH language code (2 = German)",
               subsStr(i) == "#6 '' eng Timed Text def ok\n#7 '' ger WebVTT ok\n", subsStr(i));
            expectCues("tx3g through co64 + stz2 + 2 samples in one chunk; the edit list's EMPTY edit (1.5 s) shifts it; UTF-16 text; "
                       "styl bold on 4 characters; an empty sample = a gap", f, i, 6, "2000-4000:{2}Bold{/} UTF-16\n4000-5500:plain \xC3\xA9\n", false);
            expectCues("wvtt: two vttc in one sample = two cues; vtte = nothing; payl markup read", f, i, 7,
                       "0-1000:First cue\n0-1000:Overlap cue\n1500-3000:Third {1}styled{/}\n", false);
        }
        {
            const Info i = probeFile(wh(GEN[7]));
            ok("tx3g.mov (ffmpeg, QuickTime): the media hdlr taken, not minf's data handler; Mac code 0 = English",
               i.container == "mov" && audioStr(i) == "0:#2 '' eng AAC ch1 def play\n1:#3 '' chi AAC ch1 play\n"
               && subsStr(i) == "#4 '' eng Timed Text def ok\n", audioStr(i) + subsStr(i));
            std::string why;
            const SubResult r = extractPath(wh(GEN[7]), i, 4, Path::Auto);
            ok("tx3g.mov: == sub_en.srt", r.ok && sameCues(r.parsed.cues, srcEn.cues, why), why + r.err);
            const Info fr = probeFile(wh(GEN[8]));
            const SubResult rf = extractPath(wh(GEN[8]), fr, 4, Path::Auto);
            ok("frag.mp4 (fragmented, ffmpeg): tracks listed, the subtitle NOT readable with its sentence",
               fr.fragmented && fr.audio.size() == 2 && fr.subs.size() == 1 && !fr.subs[0].ok && !rf.ok && rf.err == errFragmented(), rf.err);
        }
        {
            // hostile VOLUMES (the review of MediaTracks.h): neither may hold the worker
            const std::wstring st = wh(L"t\\gen\\same_time.mkv"), bu = wh(L"t\\gen\\budget.mp4");
            const Info is = probeFile(st), ib = probeFile(bu);
            auto t0 = std::chrono::steady_clock::now();
            const SubResult rs = extractPath(st, is, 1, Path::Auto);
            const double ms1 = msSince(t0);
            ok("150 000 blocks at ONE timestamp (no duration): read in < 2 s (the 'next block' is one backward pass, not a scan "
               "per block), the reader's 20 000-cue cap says truncated", rs.ok && rs.ex.blocks == 150000 && rs.parsed.truncated && ms1 < 2000.0,
               std::to_string(ms1) + " ms, " + std::to_string(rs.ex.blocks) + " blocks " + rs.err);
            t0 = std::chrono::steady_clock::now();
            const SubResult rb = extractPath(bu, ib, 1, Path::Auto);
            const double ms2 = msSince(t0);
            ok("100 000 MP4 samples all on the SAME megabyte: stopped by the 64 MB read budget (truncated) in < 2 s, "
               "the cues read so far kept", rb.ok && rb.ex.truncated && rb.ex.payload <= kMaxPayload && rb.ex.blocks < 100 && ms2 < 2000.0,
               std::to_string(ms2) + " ms, " + std::to_string(rb.ex.blocks) + " samples " + rb.err);
            std::printf("  (150 000 blocks at one time: %.1f ms; the budget file: %.1f ms)\n", ms1, ms2);
        }
        {
            // not a container this reads: None (no tracks section on the page), and never an error
            std::string wav = "RIFF\x24\x00\x00\x00WAVEfmt ";
            wav.resize(64, '\0');
            MemSource ms(wav.data(), wav.size());
            ok("a WAV (or anything not EBML / ISO-BMFF): state None", probe(ms).state == Info::State::None);
            ok("probeWorthyExt: mkv mka webm mp4 m4v m4a mov 3gp 3g2 -- not wav mp3 avi ts flac",
               probeWorthyExt(L"MKV") && probeWorthyExt(L"m4a") && probeWorthyExt(L"3gp") && !probeWorthyExt(L"wav") && !probeWorthyExt(L"mp3")
               && !probeWorthyExt(L"avi") && !probeWorthyExt(L"ts") && !probeWorthyExt(L"flac"));
            ok("the WebView codec table: AAC MP3 Opus Vorbis FLAC AC-3 E-AC-3 PCM yes; DTS TrueHD ALAC MP2 big-endian PCM no",
               webViewPlays("AAC") && webViewPlays("E-AC-3") && webViewPlays("PCM") && !webViewPlays("DTS") && !webViewPlays("TrueHD")
               && !webViewPlays("ALAC") && !webViewPlays("MP2") && !webViewPlays("PCM (big-endian)"));
        }

        // ---- [4] the maintainer's film (SUVTITLE_TEST_FILM, READ ONLY) ---------------------------------------------------
        // A multi-audio feature film that is NOT in the repository: 1920x1036 HEVC, 5 AAC sound tracks (eng 5.1, eng stereo, fre,
        // jpn, nor), 8 subtitle tracks (5 ASS, 3 VobSub). Another file fails these checks on its own shape, by design; without
        // the variable the part is skipped, never failed. The tracks' titles are blanked before they are compared: they
        // name the release, and only codec / language / channels / flags are the point here.
        std::printf("[4] the maintainer's film (SUVTITLE_TEST_FILM, READ ONLY)\n");
        uint64_t fs = 0;
        if (!FILM.empty()) {
            FileSource f(FILM);
            fs = f.ok() ? f.size() : 0;
        }
        auto untitled = [](std::string s) {                     // 'any title' -> ''
            for (size_t at = 0; (at = s.find('\'', at)) != std::string::npos; ) {
                const size_t end = s.find('\'', at + 1);
                if (end == std::string::npos) break;
                s.erase(at + 1, end - at - 1);
                at += 2;
            }
            return s;
        };
        if (!fs) {
            std::printf("  SKIP  (set SUVTITLE_TEST_FILM) the maintainer's film is not there\n");
        } else {
            FileSource f(FILM);
            Counting cnt(f);
            auto t0 = std::chrono::steady_clock::now();
            const Info i = probe(cnt);
            const double probeMs = msSince(t0);
            std::printf("  (probe: %.2f ms, %llu bytes in %llu reads, of a %llu-byte file)\n", probeMs, (unsigned long long)cnt.bytes,
                        (unsigned long long)cnt.calls, (unsigned long long)fs);
            ok("the probe reads the HEAD only (< 1 MB of the 2.09 GB) and takes < 200 ms", cnt.bytes < (1u << 20) && probeMs < 200);
            ok("F4: HEVC 1920x1036; audio #2 eng 5.1 (default), #3 eng stereo, #4 fre, #5 jpn, #6 nor -- all AAC (titles blanked)",
               i.video.codec == "HEVC" && i.video.w == 1920 && i.video.h == 1036
               && untitled(audioStr(i)) == "0:#2 '' eng AAC ch6 def play\n1:#3 '' eng AAC ch2 play\n"
                                           "2:#4 '' fre AAC ch2 play\n3:#5 '' jpn AAC ch2 play\n4:#6 '' nor AAC ch2 play\n", untitled(audioStr(i)));
            ok("F4: subs #7 ASS eng (no Language element = eng), #8 fre, #9 fin, #10 rum, #11 cze; #12 kor, #13 chi, #14 chi VobSub = pictures",
               subsStr(i) == "#7 '' eng ASS ok\n#8 '' fre ASS ok\n#9 '' fin ASS ok\n#10 '' rum ASS ok\n#11 '' cze ASS ok\n"
                             "#12 '' kor VobSub image NO\n#13 '' chi VobSub image NO\n#14 '' chi VobSub image NO\n", subsStr(i));
            for (uint32_t t : { 7u, 8u }) {
                double tc[2] = {}, tw[2] = {};
                SubResult rc, rw;
                for (int run = 0; run < 2; ++run) {
                    auto c0 = std::chrono::steady_clock::now();
                    rc = extractPath(FILM, i, t, Path::Cues);
                    tc[run] = msSince(c0);
                    auto w0 = std::chrono::steady_clock::now();
                    rw = extractPath(FILM, i, t, Path::Walk);
                    tw[run] = msSince(w0);
                }
                std::printf("  (#%u: Cues %.1f / %.1f ms, forced walk %.1f / %.1f ms (1st / 2nd run); %zu blocks, %zu cues)\n", t, tc[0], tc[1],
                            tw[0], tw[1], rc.ex.blocks, rc.parsed.cues.size());
                std::string why;
                const std::string nm = "#" + std::to_string(t);
                ok((nm + ": the Cues path and a forced walk give IDENTICAL cues (1031 / 1076 blocks)").c_str(),
                   rc.ok && rw.ok && rc.ex.viaCues && !rw.ex.viaCues && sameCues(rc.parsed.cues, rw.parsed.cues, why)
                   && rc.ex.blocks == (t == 7 ? 1031u : 1076u), why + rc.err + rw.err);
                const pbsubr::Result ref = parseSource(wh(t == 7 ? L"t\\ref\\film_t7.ass" : L"t\\ref\\film_t8.ass"));
                ok((nm + ": == ffmpeg's own extraction of the track (wh/t/ref), cue for cue").c_str(),
                   ref.ok && sameCues(rc.parsed.cues, ref.cues, why), why);
                ok((nm + ": the Cues path is faster than the walk (cold or warm)").c_str(), tc[1] < tw[1]);
            }
            SubResult r12 = extractPath(FILM, i, 12, Path::Auto);
            ok("#12 (S_VOBSUB, zlib): pictures -- not extracted, its sentence", !r12.ok && r12.err == errImage(), r12.err);
            // a walk CANCELLED 30 ms in returns at once, with no sentence
            {
                std::atomic<bool> stop{ false };
                SubResult r;
                double took = 0;
                std::thread th([&] {
                    auto c0 = std::chrono::steady_clock::now();
                    extractFile(FILM, i, 8, r, &stop, Path::Walk);
                    took = msSince(c0);
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                const auto s0 = std::chrono::steady_clock::now();
                stop.store(true);
                th.join();
                const double after = msSince(s0);
                std::printf("  (cancelled walk: returned %.1f ms after the flag, %.1f ms in all)\n", after, took);
                ok("a cancelled walk returns within 100 ms of the flag, no cue, no sentence", !r.ok && r.err.empty() && after < 100.0,
                   r.err + " " + std::to_string(after));
            }
        }
    }

    // ---- [5] hostile input -----------------------------------------------------------------------------------------------
    if (!noFuzz) std::printf("[5] hostile input: truncations + bit flips on every test file%s\n", heavy ? " (heavy)" : " (light; the ASan build runs it heavy)");
    if (!noFuzz) {
        Fuzz fz;
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t seed = 1;
        for (const auto& f : { M2, MP, MD, MW }) fuzzFile(f, fz, heavy, seed++);
        for (const wchar_t* g : GEN) fuzzFile(wh(g), fz, true, seed++);       // the small ones: every byte, always
        // (fix round: the Cues files as "Lavf60" -- the only way a gen file reaches the Cues path now -- and RV-1's two)
        for (int g = 2; g <= 5; ++g) fuzzBytes(asLavf(wh(GEN[g])), a(wh(GEN[g])) + " as Lavf60", fz, true, seed++);
        fuzzBytes(fromHex(kH1Partial), "h1_cues_partial.mkv", fz, true, seed++);
        fuzzBytes(fromHex(kH1bFull), "h1b_cues_full.mkv", fz, true, seed++);
        const double all = msSince(t0);
        std::printf("  (%zu runs: %zu probed, %zu extractions read; slowest %.1f ms (%s); %.1f s in all)\n", fz.runs, fz.probed, fz.extracted,
                    fz.maxMs, fz.slowest.c_str(), all / 1000.0);
        ok("every truncated / flipped file: no crash, every run bounded (slowest < 3 s)", fz.runs > 1000 && fz.maxMs < 3000.0,
           std::to_string(fz.maxMs) + " ms: " + fz.slowest);
    }

    std::printf("\ntracks_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

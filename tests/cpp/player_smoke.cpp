// =============================================================================
// SuVtitle tests/cpp/player_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/player_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the includes, the media/proxy hosts (suvtitle-*), the engine sentences of TrPlayer.h
// (no Translator card in SuVtitle). Same checks, same count.
// FIX ROUND 2026-10-23: + RV4-7 (translatorWindow.topmost on every post) and RV4-1 / ADDENDUM 2 (the page's AUTOMATIC
// playerSubGet never replaces the viewer's subtitle: acceptSubGet / userSubFile), after [12] C1 and WHP-1.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// player_smoke -- the WE (player v2) check for ui/TrPlayer.h: THE BRIDGE IN A BOX.
// The core is driven exactly as bridge.cpp drives it (page messages, helper messages, ticks), against
// a FAKE Host that records every action -- every FILE/FFOCUS/FCANCEL/FPROXY, every post (each one
// checked to be well-formed JSON), every mapping and reload, every cache/queue/resume write -- and
// keeps an in-memory disk (identities, cache records through the REAL pbtrf pack/parse).
// ASCII only (no /utf-8, like the product).
// =============================================================================
#include "../../app/TrPlayer.h"
#include "../../app/TrWindowState.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tp = pbt::trpipe;
using pbplayer::Player;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}
static std::string n2s(unsigned long long v) { return std::to_string(v); }
static std::string a(const std::wstring& w) { return pbtrf::toUtf8(w); }

// ---- a strict little JSON validator (every post must parse) --------------------------------------------
struct JV {
    const std::wstring& s; size_t i = 0;
    explicit JV(const std::wstring& x) : s(x) {}
    void ws() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\n' || s[i] == L'\r' || s[i] == L'\t')) ++i; }
    bool lit(const wchar_t* w) { size_t k = 0; while (w[k]) { if (i + k >= s.size() || s[i + k] != w[k]) return false; ++k; } i += k; return true; }
    bool str() {
        if (i >= s.size() || s[i] != L'"') return false;
        ++i;
        while (i < s.size() && s[i] != L'"') {
            if (s[i] < 0x20) return false;
            if (s[i] == L'\\') { ++i; if (i >= s.size()) return false; if (s[i] == L'u') { i += 4; if (i >= s.size()) return false; } }
            ++i;
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool num() { size_t b = i; if (i < s.size() && s[i] == L'-') ++i; while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i; return i > b; }
    bool val() {
        ws();
        if (i >= s.size()) return false;
        bool r;
        if (s[i] == L'{') {
            ++i; ws();
            if (i < s.size() && s[i] == L'}') { ++i; r = true; }
            else { r = false; for (;;) { ws(); if (!str()) break; ws(); if (i >= s.size() || s[i] != L':') break; ++i; if (!val()) break; ws();
                   if (i < s.size() && s[i] == L',') { ++i; continue; } if (i < s.size() && s[i] == L'}') { ++i; r = true; } break; } }
        } else if (s[i] == L'[') {
            ++i; ws();
            if (i < s.size() && s[i] == L']') { ++i; r = true; }
            else { r = false; for (;;) { if (!val()) break; ws(); if (i < s.size() && s[i] == L',') { ++i; continue; }
                   if (i < s.size() && s[i] == L']') { ++i; r = true; } break; } }
        } else if (s[i] == L'"') r = str();
        else if (lit(L"true") || lit(L"false") || lit(L"null")) r = true;
        else r = num();
        ws();
        return r;
    }
};
static bool validJson(const std::wstring& s) { JV v(s); return v.val() && v.i == s.size(); }

// first-occurrence field readers (top-level keys come first in every post)
static bool field(const std::wstring& j, const std::wstring& key, std::wstring& raw) {
    const std::wstring pat = L"\"" + key + L"\":";
    const size_t k = j.find(pat);
    if (k == std::wstring::npos) return false;
    size_t i = k + pat.size(), e = i;
    if (i < j.size() && j[i] == L'"') { e = i + 1; while (e < j.size() && j[e] != L'"') { if (j[e] == L'\\') ++e; ++e; } raw = j.substr(i + 1, e - i - 1); return true; }
    while (e < j.size() && j[e] != L',' && j[e] != L'}' && j[e] != L']') ++e;
    raw = j.substr(i, e - i);
    return true;
}
static std::string fs(const std::wstring& j, const wchar_t* k) { std::wstring r; return field(j, k, r) ? a(r) : std::string("<none>"); }
static long long fn(const std::wstring& j, const wchar_t* k) { std::wstring r; return field(j, k, r) ? _wtoi64(r.c_str()) : -1; }
static bool fb(const std::wstring& j, const wchar_t* k) { std::wstring r; return field(j, k, r) && r == L"true"; }
static std::string typeOf(const std::wstring& j) { return fs(j, L"type"); }

// ---- the fake Host --------------------------------------------------------------------------------------------
struct FakeHost : pbplayer::Host {
    std::vector<tp::WireFile> files;
    std::vector<std::pair<uint64_t, uint64_t>> focuses;
    std::vector<uint64_t> cancels;
    std::vector<tp::WireProxy> proxies;
    std::vector<std::wstring> both, window, mains;   // mains: the card only (while the window reloads)
    std::vector<std::vector<std::wstring>> maps;
    std::set<std::wstring> mapFail;
    int reloads = 0, opens = 0, identityCalls = 0;
    std::map<std::wstring, std::pair<uint64_t, uint64_t>> disk;   // path -> size, mtime
    std::map<std::string, std::string> cache;                      // key -> packed record
    int cacheWrites = 0, srtWrites = 0;
    // WG: the per-language files, as a little disk (path -> the L text of each cue, "start-end:text" lines), so a
    // file whose content would not change is NOT written again (the bridge compares bytes the same way)
    std::map<std::wstring, std::string> srtDisk;
    std::vector<std::pair<std::string, size_t>> srtLog;     // each real write: (language, lines)
    int peeks = 0;
    std::vector<std::wstring> deleted;
    std::map<std::wstring, std::vector<pbplayer::Sidecar>> sidecarMap;   // folder|leaf
    std::string queueBytes, resumeBytes;
    int queueSaves = 0, resumeSaves = 0;
    uint64_t now = 1000000, epoch = 1760000000;
    int badJson = 0;
    std::string lastBadJson;

    void sendFile(const tp::WireFile& f) override { files.push_back(f); }
    void sendFocus(uint64_t w, uint64_t ms) override { focuses.push_back({ w, ms }); }
    void sendCancel(uint64_t id) override { cancels.push_back(id); }
    void sendProxy(const tp::WireProxy& p) override { proxies.push_back(p); }
    void check(const std::wstring& j) { if (!validJson(j)) { ++badJson; lastBadJson = a(j).substr(0, 300); } }
    void postBoth(const std::wstring& j) override { check(j); both.push_back(j); window.push_back(j); }
    void postWindow(const std::wstring& j) override { check(j); window.push_back(j); }
    void postMain(const std::wstring& j) override { check(j); mains.push_back(j); }
    void openWindow() override { ++opens; }
    void mapFolders(const std::vector<std::wstring>& f, std::vector<std::wstring>& failed) override {
        maps.push_back(f);
        for (const auto& x : f) if (mapFail.count(pbtrf::lowerW(x))) failed.push_back(x);
    }
    // The page's `ready` after a reload: at once by default (autoReady) -- the tests about the reload
    // itself (nothing posted to the window until its ready: review of WE P1, C4) turn it off.
    Player* player = nullptr;
    bool autoReady = true;
    void reloadWindow() override { ++reloads; if (autoReady && player) player->windowReady(); }
    bool identity(const std::wstring& p, uint64_t& s, uint64_t& m) override {
        ++identityCalls;
        auto it = disk.find(pbtrf::lowerW(p));
        if (it == disk.end()) return false;
        s = it->second.first; m = it->second.second;
        return true;
    }
    bool readCache(const std::string& k, pbtrf::Record& r) override {
        auto it = cache.find(k);
        return it != cache.end() && pbtrf::parseRecord(it->second, r);
    }
    bool peekCache(const std::string& k, pbtrf::Summary& s) override {
        ++peeks;
        auto it = cache.find(k);
        return it != cache.end() && pbtrf::peekRecordText(it->second, s);
    }
    void writeCache(const std::string& k, const pbtrf::Record& r) override { cache[k] = pbtrf::packRecord(r); ++cacheWrites; }
    // WH: the registry's writer of each per-language file (path -> the record whose cues it holds, and its track)
    std::map<std::wstring, std::pair<std::string, uint32_t>> srtFrom;
    int writerAsks = 0;
    static std::wstring srtPathOf(const pbplayer::CueSet& set, const std::string& lang) {
        return pbtrf::joinPath(set.folder, pbtrf::stripExt(set.name) + L"." + pbtrf::fromUtf8(pbtrf::fileLangTag(lang)) + L".srt");
    }
    std::wstring writeSrt(const pbplayer::CueSet& set, const std::string& lang, const std::vector<tp::WireCue>& cues,
                          const std::string& fromKey, uint32_t fromOrd,
                          std::wstring& note, uint64_t& size, uint64_t& mtime) override {
        note.clear();
        std::string body, t;
        size_t n = 0;
        std::vector<tp::WireCue> sorted = cues;
        pbtrf::canonicalCues(sorted);
        for (const auto& c : sorted)
            if (pbtrf::textInLang(c, lang, t)) { body += std::to_string(c.startMs) + "-" + std::to_string(c.endMs) + ":" + t + "\n"; ++n; }
        const std::wstring p = srtPathOf(set, lang);
        size = body.size(); mtime = 777;
        auto it = srtDisk.find(p);
        if (it == srtDisk.end() || it->second != body) {
            srtDisk[p] = body; ++srtWrites; srtLog.push_back({ lang, n });
            srtFrom[p] = { fromKey, fromOrd };                  // (the bridge registers only a REAL write, too)
        }
        return p;
    }
    std::string srtWriter(const pbplayer::CueSet& set, const std::string& lang) override {
        ++writerAsks;
        auto it = srtFrom.find(srtPathOf(set, lang));
        return it == srtFrom.end() ? std::string() : it->second.first;
    }
    // WH: the film's own tracks -- answered at once by default (autoProbe; a path's Info from `probeMap`, else None: not
    // a container), or held for the test to answer (probeReqs); extractions are held (extracts) for the test to answer
    bool autoProbe = true;
    std::map<std::wstring, std::shared_ptr<const pbmedia::Info>> probeMap;   // lower-cased path
    std::vector<std::pair<uint64_t, std::wstring>> probeReqs;
    int probes = 0;
    struct Ex { uint64_t req; std::wstring path; uint32_t track; };
    std::vector<Ex> extracts;
    std::vector<uint64_t> extractCancels;
    std::string audioBytes;
    int audioSaves = 0;
    void probeTracks(uint64_t req, const std::wstring& path) override {
        ++probes;
        if (!autoProbe || !player) { probeReqs.push_back({ req, path }); return; }
        player->onTracks(req, probeFor(path));
    }
    std::shared_ptr<const pbmedia::Info> probeFor(const std::wstring& path) {
        auto it = probeMap.find(pbtrf::lowerW(path));
        if (it != probeMap.end()) return it->second;
        return std::make_shared<pbmedia::Info>();             // State::None
    }
    void extractSub(uint64_t req, const std::wstring& path, std::shared_ptr<const pbmedia::Info>, uint32_t track) override {
        extracts.push_back({ req, path, track });
    }
    void cancelExtract(uint64_t req) override { extractCancels.push_back(req); }
    void saveAudio(const std::string& b) override { audioBytes = b; ++audioSaves; }
    std::wstring proxyPath(const std::string& mk, uint64_t id) override { return L"C:\\proxy\\" + pbtrf::fromUtf8(mk) + L"-" + std::to_wstring(id) + L".wav"; }
    void deleteFile(const std::wstring& p) override { deleted.push_back(p); }
    std::vector<pbplayer::Sidecar> sidecars(const std::wstring& folder, const std::wstring& leaf) override {
        auto it = sidecarMap.find(pbtrf::lowerW(folder + L"|" + leaf));
        return it == sidecarMap.end() ? std::vector<pbplayer::Sidecar>() : it->second;
    }
    void saveQueue(const std::string& b) override { queueBytes = b; ++queueSaves; }
    void saveResume(const std::string& b) override { resumeBytes = b; ++resumeSaves; }
    uint64_t nowMs() override { return now; }
    uint64_t epochSec() override { return epoch; }

    void addFile(const std::wstring& p, uint64_t size = 1000, uint64_t mtime = 42) { disk[pbtrf::lowerW(p)] = { size, mtime }; }
    std::wstring lastOf(const std::string& type, bool both_ = false) const {
        const auto& v = both_ ? both : window;
        for (size_t i = v.size(); i-- > 0;) if (typeOf(v[i]) == type) return v[i];
        return std::wstring();
    }
    std::wstring firstOf(const std::string& type) const {
        for (const auto& j : window) if (typeOf(j) == type) return j;
        return std::wstring();
    }
    size_t countOf(const std::string& type) const { size_t n = 0; for (const auto& j : window) if (typeOf(j) == type) ++n; return n; }
    void clearPosts() { both.clear(); window.clear(); }
};

static tp::WireFileProg prog(uint64_t job, const char* phase, uint64_t total, const std::string& ranges) {
    tp::WireFileProg p;
    p.jobId = job; p.phase = phase; p.totalMs = total; p.ranges = ranges;
    p.asrModel = "ggml-large-v3-turbo.bin"; p.mtModel = "Qwen3-4B.gguf"; p.hasVideo = true;
    std::vector<tp::MsRange> r; tp::parseRanges(ranges, r); p.doneMs = tp::rangesTotal(r);
    return p;
}
static tp::WireCue cue(uint64_t job, uint32_t idx, uint64_t s, uint64_t e, const char* src = "src", const char* dst = "dst") {
    tp::WireCue c;
    c.jobId = job; c.idx = idx; c.startMs = s; c.endMs = e; c.lang = "en"; c.dstLang = "fr"; c.src = src; c.dst = dst;
    return c;
}
static tp::WireFileDone done(uint64_t job, const char* result, const char* err = "") {
    tp::WireFileDone d; d.jobId = job; d.result = result; d.err = err; return d;
}
static std::vector<tp::MsRange> rg(const std::string& s) { std::vector<tp::MsRange> r; tp::parseRanges(s, r); return r; }

// A player on a fresh host: helper up, window attached + loaded + shown on File, pair en/fr.
struct Rig {
    FakeHost h;
    Player p{ h };
    explicit Rig(bool autoReady = true) {
        h.player = &p;
        h.autoReady = autoReady;
        p.seedShuffle(12345);
        p.setPair("en", "fr", "auto");
        p.setHelperAvailable(true);
        p.windowAttached();
        p.windowReady();
        p.windowShown(true);
        p.view("file");
        h.clearPosts();
    }
};

int main() {
    // Unbuffered: a MUTANT that crashes the smoke (cmut) must still show the last check it reached.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("player_smoke (ui/TrPlayer.h, the core against a fake host)\n");

    // ---- 1. the range rules: the shared vectors (C++ side), then the FFOCUS rule ------------------------------
    std::printf("[1] range rules\n");
    {
        // next to the EXE (the smoke lives beside its vectors), whatever folder it was started from
        char exe[MAX_PATH] = {};
        std::string vec = "we_vectors.txt";
        if (::GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
            std::string e = exe;
            const size_t sl = e.find_last_of("\\/");
            if (sl != std::string::npos) vec = e.substr(0, sl + 1) + "we_vectors.txt";
        }
        std::ifstream in(vec);
        std::string line;
        int n = 0, bad = 0;
        std::string firstBad;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string x;
            while (std::getline(ss, x, '|')) f.push_back(x);
            if (line.back() == '|') f.push_back("");
            if (f.size() != 7) { ++bad; firstBad = "malformed: " + line; continue; }
            const auto r = rg(f[0]);
            const uint64_t P = std::stoull(f[1]), D = std::stoull(f[2]);
            const bool good = pbplayer::focusTarget(r, P) == std::stoull(f[3]) && pbplayer::leadAt(r, P) == std::stoull(f[4])
                           && pbplayer::readyAt(r, P, D) == (f[5] == "1") && pbplayer::coveredToEnd(r, P, D) == (f[6] == "1");
            if (!good && firstBad.empty()) firstBad = line;
            bad += good ? 0 : 1;
            ++n;
        }
        ok("we_vectors.txt: every vector right (target, lead, ready, atEnd) -- the page runs the same file",
           n >= 16 && bad == 0, n2s(n) + " vectors, first wrong: " + firstBad);
        using pbplayer::PosReason;
        using pbplayer::shouldFocus;
        ok("FFOCUS: a seek into a gap -> yes; the same with a pause -> no",
           shouldFocus(rg("0-30000"), 600000, 45000, 0, PosReason::Seek) && !shouldFocus(rg("0-30000"), 600000, 45000, 0, PosReason::Pause));
        ok("FFOCUS: inside the range the job is already extending (same target) -> no",
           !shouldFocus(rg("0-30000"), 600000, 10000, 0, PosReason::Seek));
        ok("FFOCUS: a seek into an EARLIER range whose end is near -> yes (seek and tick)",
           shouldFocus(rg("0-30000,60000-90000"), 600000, 65000, 0, PosReason::Seek)
           && shouldFocus(rg("0-30000,60000-90000"), 600000, 65000, 0, PosReason::Tick));
        ok("FFOCUS: plenty subtitled ahead (lead >= 60 s) -> no, even though the target moved",
           !shouldFocus(rg("0-300000"), 600000, 10000, 400000, PosReason::Seek));
        ok("FFOCUS: a tick with 20 s left -> yes; with 40 s left -> only a seek would",
           shouldFocus(rg("0-300000"), 600000, 280000, 350000, PosReason::Tick)
           && !shouldFocus(rg("0-300000"), 600000, 260000, 350000, PosReason::Tick)
           && shouldFocus(rg("0-300000"), 600000, 260000, 350000, PosReason::Seek));
        ok("FFOCUS: nothing left to analyse ahead -> never", !shouldFocus(rg("0-600000"), 600000, 100, 700000, PosReason::Seek));
    }

    const std::wstring A = L"D:\\Films\\Season 1\\Ep 1.mkv", B = L"D:\\Films\\Season 1\\Ep 2.mkv", C = L"D:\\Films\\Season 1\\Ep 10.mkv";
    const std::wstring X = L"E:\\Other\\talk #2 100%.mp3";

    // ---- 2. open, the first job, the mapping -----------------------------------------------------------------------
    std::printf("[2] open: the queue, the job, the URL, the mapping\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B); r.h.addFile(C); r.h.addFile(X, 77, 99);
        r.p.open({ A, B, C }, false);
        ok("three files queued, the first loaded", r.p.items().size() == 3 && r.p.current() == 0 && r.p.loaded());
        // (a new folder reloads the page; its `ready` -- at once here -- replays the session: that FIRST post is
        //  the opening one, `fresh`; the updates after it are not)
        ok("...its job: FILE with no `have`, focus 0, the pair snapshotted",
           r.h.files.size() == 1 && r.h.files[0].have.empty() && r.h.files[0].focusMs == 0 && r.h.files[0].langA == "en"
           && r.h.files[0].langB == "fr" && r.h.files[0].mode == "auto" && r.h.files[0].quality == "auto"
           && r.h.files[0].path == a(A));
        const std::wstring st = r.h.firstOf("translatorFile");
        ok("translatorFile: fresh, autoplay, loading, translate on",
           fb(st, L"fresh") && fb(st, L"autoplay") && fs(st, L"phase") == "loading" && fb(st, L"translate"), a(st).substr(0, 300));
        const std::string url = fs(st, L"url");
        const std::string fk = pbtrf::folderKey(L"D:\\Films\\Season 1"), mk = pbtrf::mediaKey(A, 5000, 11);
        ok("URL = the FOLDER's host + the leaf + ?v=<mediaKey>", url == "https://f" + fk + ".suvtitle-media.example/Ep%201.mkv?v=" + mk, url);
        ok("a new folder: mapped (the current one first) and the page RELOADED",
           !r.h.maps.empty() && !r.h.maps.back().empty()   // (the attach maps an EMPTY list: never index it blind)
           && pbtrf::lowerW(r.h.maps.back()[0]) == pbtrf::lowerW(L"D:\\Films\\Season 1") && r.h.reloads == 1);
        r.p.windowReady();
        const size_t files0 = r.h.files.size();
        r.p.queueOp("play", r.p.items()[1].id, 0, 0);
        ok("the next file IN THE SAME FOLDER: no new mapping, no reload", r.h.reloads == 1 && r.h.files.size() == files0 + 1);
        ok("...and the first job was cancelled (its FDONE will save the partial)", !r.h.cancels.empty() && r.h.cancels.back() == r.h.files[0].jobId);
        r.p.add({ X });
        r.p.queueOp("play", r.p.items()[3].id, 0, 0);
        ok("a file in ANOTHER folder: mapped and reloaded", r.h.reloads == 2);
        const std::wstring st2 = r.h.lastOf("translatorFile", true);
        ok("...its URL encodes '#', '%' and the space", fs(st2, L"url").find("/talk%20%232%20100%25.mp3?v=") != std::string::npos, fs(st2, L"url"));
        ok("every post was well-formed JSON", r.h.badJson == 0, r.h.lastBadJson);
    }

    // ---- 3. the analysis: cues, ranges, done, the cache --------------------------------------------------------------
    std::printf("[3] the analysis of the current file\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onProg(prog(job, "loading", 600000, ""));
        r.p.onCue(cue(job, 0, 1000, 2500, "Hello.", "Bonjour."));
        r.p.onCue(cue(job, 1, 3000, 4000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.flush();
        const std::wstring c0 = r.h.window.size() ? r.h.lastOf("translatorFileCue") : L"";
        ok("cues go to the window under the page's GENERATION, not the wire id",
           r.h.countOf("translatorFileCue") == 2 && fn(c0, L"jobId") == gen && fn(c0, L"idx") == 1);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("the state carries the ranges and the analysed total", fs(st, L"phase") == "analyzing" && fn(st, L"doneMs") == 30000
           && a(st).find("\"ranges\":[[0,30000]]") != std::string::npos, a(st).substr(0, 400));
        r.p.onCue(cue(job, 1, 3000, 4000));
        r.p.onCue(cue(99, 0, 5000, 6000));
        r.p.onCue(cue(job, 2, 3500, 4500));
        ok("a duplicate idx, another job's cue, an OVERLAPPING cue: none lands",
           r.h.countOf("translatorFileCue") == 2, n2s(r.h.countOf("translatorFileCue")));
        r.p.onCue(cue(job, 3, 31000, 33000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.p.onDone(done(job, "ok"), false);
        const std::wstring st2 = r.h.lastOf("translatorFile");
        ok("FDONE ok: done, complete, ONE FILE PER LANGUAGE written (en = the sources, fr = the translations) (WG)",
           fs(st2, L"phase") == "done" && fb(st2, L"complete") && r.h.srtWrites == 2
           && fs(st2, L"srtName") == "Ep 1.en.srt + Ep 1.fr.srt"
           && a(st2).find("\"srtFiles\":[{\"lang\":\"en\",\"name\":\"Ep 1.en.srt\"},{\"lang\":\"fr\",\"name\":\"Ep 1.fr.srt\"}]") != std::string::npos,
           a(st2).substr(0, 600));
        pbtrf::Record rec;
        const std::string key = pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto");
        ok("...the cache record is COMPLETE over [0, total] with the three cues",
           r.h.readCache(key, rec) && rec.complete && rec.ranges.size() == 1 && rec.ranges[0].e == 600000 && rec.cues.size() == 3);
        // opening it again: from the cache, no job
        Rig r2(false);                                       // the reload's `ready` comes when THIS test says
        r2.h.addFile(A, 5000, 11);
        r2.h.cache = r.h.cache;
        r2.p.open({ A }, false);
        ok("a new folder: the page reloads, and nothing is posted to it before its `ready`",
           r2.h.reloads == 1 && r2.h.countOf("translatorFileCues") == 0);
        r2.p.windowReady();                                 // the reloaded page says ready
        const std::wstring st3 = r2.h.lastOf("translatorFile");
        ok("re-opened: from the cache, done, cached, no FILE sent", r2.h.files.empty() && fs(st3, L"phase") == "done" && fb(st3, L"cached")
           && fn(st3, L"count") == 3, a(st3).substr(0, 300));
        ok("...every cue replayed to the window in one bulk message (start order, idx 0..2)",
           r2.h.countOf("translatorFileCues") == 1 && a(r2.h.lastOf("translatorFileCues")).find("\"idx\":2") != std::string::npos);
        ok("...its per-language files: the same content on disk, so NOTHING is rewritten (WG: the bytes are compared)",
           r2.h.srtWrites == 2 && r2.h.srtDisk.size() == 2, n2s(r2.h.srtWrites));   // (r2's disk was empty: its own two)
        Rig r3;
        r3.h.addFile(A, 5000, 11);
        r3.h.cache = r.h.cache;
        r3.h.srtDisk = r.h.srtDisk;
        r3.p.open({ A }, false);
        ok("a cache hit whose files are on disk and up to date: no write at all", r3.h.srtWrites == 0, n2s(r3.h.srtWrites));
        Rig r4;
        r4.h.addFile(A, 5000, 11);
        r4.h.cache = r.h.cache;
        r4.h.srtDisk = r.h.srtDisk;
        r4.h.srtDisk[pbtrf::joinPath(L"D:\\Films\\Season 1", L"Ep 1.en.srt")] = "stale";
        r4.p.open({ A }, false);
        ok("...one of them stale (the old mixed file of 1.0.6): that one is written again, the other left alone",
           r4.h.srtWrites == 1 && r4.h.srtLog.size() == 1 && r4.h.srtLog[0].first == "en", n2s(r4.h.srtWrites));
        ok("every post was well-formed JSON", r.h.badJson == 0 && r2.h.badJson == 0 && r3.h.badJson == 0, r.h.lastBadJson + r2.h.lastBadJson);
    }

    // ---- 4. TRUSTED RANGES: the partial cache ---------------------------------------------------------------------------
    std::printf("[4] the partial cache: only what is vouched for\n");
    const std::string keyA = pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(cue(job, 0, 1000, 2000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.onCue(cue(job, 1, 31000, 32000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.p.onCue(cue(job, 2, 61000, 62000));              // arrives AFTER the FPROG that will claim it (Q1 broken)
        r.p.onDone(done(job, "error", "The SuVtitle engine stopped during the analysis."), true);   // the helper DIED
        pbtrf::Record rec;
        ok("a SYNTHESIZED FDONE (the helper died): saved = the ranges a LATER message vouched for ([0,30000])",
           r.h.readCache(keyA, rec) && !rec.complete && rec.ranges.size() == 1 && rec.ranges[0].e == 30000,
           r.h.cache.count(keyA) ? r.h.cache[keyA].substr(0, 300) : "no record");
        ok("...with exactly the cues inside them (31000 and 61000 are re-analysed, never claimed)",
           rec.cues.size() == 1 && rec.cues[0].startMs == 1000);
        ok("...and the window says it failed, with the helper's sentence", fs(r.h.lastOf("translatorFile"), L"phase") == "error"
           && fs(r.h.lastOf("translatorFile"), L"err").find("stopped") != std::string::npos);
        // resuming it later
        Rig r2;
        r2.h.addFile(A, 5000, 11);
        r2.h.cache = r.h.cache;
        r2.p.open({ A }, false);
        ok("re-opened: the job RESUMES -- FILE have = the vouched ranges", r2.h.files.size() == 1 && r2.h.files[0].have == "0-30000",
           r2.h.files.empty() ? "no FILE" : r2.h.files[0].have);
        const uint64_t j2 = r2.h.files[0].jobId;
        r2.p.onCue(cue(j2, 0, 31000, 32000));
        const std::wstring c = r2.h.lastOf("translatorFileCue");
        ok("...its first new cue continues the session's numbering (idx 1, after the cached 0)", fn(c, L"idx") == 1, a(c));
        r2.p.onProg(prog(j2, "analyzing", 600000, "0-60000"));
        r2.p.onDone(done(j2, "cancelled"), false);         // the helper's OWN FDONE vouches for its last FPROG
        ok("a REAL FDONE cancelled: [0,60000] saved", r2.h.readCache(keyA, rec) && !rec.complete && rec.ranges.size() == 1
           && rec.ranges[0].e == 60000 && rec.cues.size() == 2);
        // checkpoint + shutdown
        Rig r3;
        r3.h.addFile(A, 5000, 11);
        r3.p.open({ A }, false);
        const uint64_t j3 = r3.h.files.back().jobId;
        r3.p.onProg(prog(j3, "analyzing", 600000, "0-30000"));
        r3.p.onCue(cue(j3, 0, 31000, 32000));
        r3.p.onProg(prog(j3, "analyzing", 600000, "0-60000"));
        const int w0 = r3.h.cacheWrites;
        r3.h.now += 60000; r3.p.tick();
        ok("no checkpoint before 120 s", r3.h.cacheWrites == w0);
        r3.h.now += 61000; r3.p.tick();
        ok("a running job's partial is CHECKPOINTED after 120 s (the vouched part: [0,30000], no cue yet)",
           r3.h.cacheWrites == w0 + 1 && r3.h.readCache(keyA, rec) && rec.ranges.size() == 1 && rec.ranges[0].e == 30000 && rec.cues.empty());
        r3.p.onProg(prog(j3, "analyzing", 600000, "0-90000"));
        r3.p.shutdown();
        ok("shutdown with the job running: saved up to the PREVIOUS report ([0,60000]) and its cue",
           r3.h.readCache(keyA, rec) && rec.ranges.size() == 1 && rec.ranges[0].e == 60000 && rec.cues.size() == 1);
        ok("every post well-formed", r.h.badJson + r2.h.badJson + r3.h.badJson == 0);
    }

    // ---- 5. FFOCUS through the core ------------------------------------------------------------------------------------------
    std::printf("[5] the playhead: FFOCUS\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.pos(gen, 5000, 600000, true, "play");
        ok("playing inside what the job is extending: no FFOCUS", r.h.focuses.empty());
        r.p.pos(gen, 300000, 600000, true, "seek");
        ok("a seek to 5:00 (not analysed): FFOCUS(job, 300000)", r.h.focuses.size() == 1 && r.h.focuses[0].first == job
           && r.h.focuses[0].second == 300000);
        r.p.pos(gen, 300400, 600000, true, "seek");
        ok("...a scrub 0.4 s further: no second FFOCUS", r.h.focuses.size() == 1);
        r.p.pos(gen - 1, 100000, 600000, true, "seek");
        ok("a report of another generation is ignored", r.h.focuses.size() == 1);
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000,300000-330000"));
        r.p.pos(gen, 2000, 600000, true, "seek");
        ok("back near the start, 28 s of subtitles ahead of it: FFOCUS (the job is working at 5:30)",
           r.h.focuses.size() == 2 && r.h.focuses[1].second == 2000);
        r.p.pos(gen, 3000, 600000, false, "pause");
        ok("a pause: nothing", r.h.focuses.size() == 2);
        ok("every post well-formed", r.h.badJson == 0);
    }

    // ---- 6. translation on/off ---------------------------------------------------------------------------------------------------
    std::printf("[6] translation off/on\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onCue(cue(job, 0, 1000, 2000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.setTranslate(false, 12000);
        ok("OFF: the job is cancelled", !r.h.cancels.empty() && r.h.cancels.back() == job);
        r.p.onDone(done(job, "cancelled"), false);
        ok("...after its FDONE: phase off, the subtitles already made kept", fs(r.h.lastOf("translatorFile"), L"phase") == "off"
           && fn(r.h.lastOf("translatorFile"), L"count") == 1);
        r.p.pos(gen, 15000, 600000, true, "tick");
        ok("...and playing on starts nothing", r.h.files.size() == 1);
        r.p.setTranslate(true, 20000);
        ok("ON again at 0:20: FILE resumes from the vouched ranges, focus at the playhead",
           r.h.files.size() == 2 && r.h.files[1].have == "0-30000" && r.h.files[1].focusMs == 20000, r.h.files.size() > 1 ? r.h.files[1].have : "");
        ok("translate is persisted in the queue file", (r.h.now += 5000, r.p.tick(), r.h.queueBytes.find("OPT\ttranslate\t1") != std::string::npos));
        // the card absent
        Rig r2;
        r2.p.setHelperAvailable(false);
        r2.h.addFile(A);
        r2.p.open({ A }, false);
        const std::wstring st = r2.h.lastOf("translatorFile");
        ok("no engine: the file loads (a URL), no FILE, and the window says why",
           r2.h.files.empty() && !fs(st, L"url").empty() && fs(st, L"phase") == "error"
           && fs(st, L"err") == "The SuVtitle engine is not running: files cannot be analysed.", a(st).substr(0, 300));
        r2.p.setHelperAvailable(true);
        ok("the card comes back: the job starts by itself", r2.h.files.size() == 1);
        const uint64_t j2 = r2.h.files[0].jobId;
        r2.p.onProg(prog(j2, "analyzing", 600000, "0-30000"));
        r2.p.setHelperAvailable(false);
        ok("the engine gone mid-job: error \"The SuVtitle engine was stopped.\", nothing pending",
           fs(r2.h.lastOf("translatorFile"), L"err") == "The SuVtitle engine was stopped." && r2.p.jobs().empty());
        ok("every post well-formed", r.h.badJson + r2.h.badJson == 0);
    }

    // ---- 7. pre-analysis (D14) ------------------------------------------------------------------------------------------------------
    std::printf("[7] pre-analysis of the next items\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
        r.p.open({ A, B, C }, false);
        r.p.windowReady();                                  // the page reloaded for the folder, and is back
        const uint64_t ja = r.h.files.back().jobId;
        ok("while the current file is analysed: nothing else", r.h.files.size() == 1);
        r.p.onProg(prog(ja, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(ja, "ok"), false);
        ok("the current file complete, shown, on File: the NEXT item is pre-analysed (focus 0)",
           r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].focusMs == 0);
        const uint64_t jb = r.h.files[1].jobId;
        const std::wstring item = r.h.lastOf("playerItem");
        ok("...its badge says analysing", fs(item, L"state") == "analysing", a(item));
        r.p.view("live");
        ok("the Live tab: the pre-analysis yields (FCANCEL)", !r.h.cancels.empty() && r.h.cancels.back() == jb);
        r.p.onProg(prog(jb, "analyzing", 500000, "0-40000"));
        r.p.onDone(done(jb, "cancelled"), false);
        r.p.view("file");
        ok("back on File: it RESUMES from its partial (have 0-40000)", r.h.files.size() == 3 && r.h.files[2].have == "0-40000"
           && r.h.files[2].path == a(B), r.h.files.size() > 2 ? r.h.files[2].have : "");
        const uint64_t jb2 = r.h.files[2].jobId;
        r.p.onCue(cue(jb2, 0, 41000, 43000));
        r.p.onProg(prog(jb2, "analyzing", 500000, "0-60000"));
        r.p.queueOp("next", 0, 0, 0);
        ok("switching TO the item being pre-analysed ADOPTS its job: no new FILE, no cancel of it",
           r.h.files.size() == 3 && (r.h.cancels.empty() || r.h.cancels.back() != jb2));
        const std::wstring st = r.h.lastOf("translatorFile", true);
        ok("...the window gets its cues at once (the adopted job's)", a(r.h.lastOf("translatorFileCues")).find("41000") != std::string::npos
           && fs(st, L"phase") == "analyzing", a(st).substr(0, 200));
        r.p.onDone(done(jb2, "error", "decoder choked"), false);
        ok("an adopted job's FDONE error is the CURRENT file's error", fs(r.h.lastOf("translatorFile"), L"phase") == "error");
        // the current file FAILED: it is what the user looks at, so the next ones wait
        Rig rf;
        rf.h.addFile(A, 5000, 11); rf.h.addFile(B, 6000, 12);
        rf.p.open({ A, B }, false);
        rf.p.windowReady();
        rf.p.onDone(done(rf.h.files.back().jobId, "error", "decoder choked"), false);
        rf.h.now += 5000; rf.p.tick();
        ok("the current file FAILED (incomplete, no job): no pre-analysis of the next one", rf.h.files.size() == 1);
        // repeat one: no pre-analysis
        Rig r2;
        r2.h.addFile(A, 5000, 11); r2.h.addFile(B, 6000, 12);
        r2.p.setRepeat(pbplayer::Repeat::One);
        r2.p.open({ A, B }, false);
        const uint64_t j = r2.h.files.back().jobId;
        r2.p.onProg(prog(j, "analyzing", 600000, "0-600000"));
        r2.p.onDone(done(j, "ok"), false);
        ok("repeat ONE: nothing to pre-analyse", r2.h.files.size() == 1);
        // a failing pre-analysis is not retried
        Rig r3;
        r3.h.addFile(A, 5000, 11); r3.h.addFile(B, 6000, 12); r3.h.addFile(C, 7000, 13);
        r3.p.open({ A, B, C }, false);
        const uint64_t j3 = r3.h.files.back().jobId;
        r3.p.onDone(done(j3, "ok"), false);
        const uint64_t jp = r3.h.files.back().jobId;
        r3.p.onDone(done(jp, "error", "bad file"), false);
        ok("a pre-analysis that FAILED is not retried: the one after it goes next",
           r3.h.files.size() == 3 && r3.h.files[2].path == a(C));
        bool sawError = false;
        const std::string bid = "\"id\":" + std::to_string(r3.p.items()[1].id) + ",";
        for (const auto& post : r3.h.window)
            if (typeOf(post) == "playerItem" && a(post).find(bid) != std::string::npos && fs(post, L"state") == "error") sawError = true;
        ok("...the failed item's badge says error", sawError);
        r3.p.windowShown(false);
        ok("the window CLOSED: the pre-analysis stops", !r3.h.cancels.empty() && r3.h.cancels.back() == r3.h.files[2].jobId);
        ok("every post well-formed", r.h.badJson + r2.h.badJson + r3.h.badJson == 0);
    }

    // ---- 8. the queue: order, repeat, shuffle, ended, prev, remove, move, clear ----------------------------------------------------
    std::printf("[8] the queue\n");
    {
        Rig r;
        r.p.setHelperAvailable(false);
        r.h.addFile(A); r.h.addFile(B); r.h.addFile(C); r.h.addFile(X);
        r.p.open({ A, B, C, X }, false);
        const auto id = [&](size_t i) { return r.p.items()[i].id; };
        long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(gen, 600000, 600000, false, "ended");
        ok("ENDED: the next item plays (autoplay)", r.p.current() == 1 && fb(r.h.lastOf("translatorFile"), L"autoplay"));
        r.p.queueOp("prev", 0, 0, 12000);
        ok("PREV 12 s in: the item restarts (playerCmd restart), it does not change",
           r.p.current() == 1 && fs(r.h.lastOf("playerCmd"), L"op") == "restart");
        r.p.queueOp("prev", 0, 0, 1000);
        ok("PREV at 1 s: the previous item", r.p.current() == 0);
        r.p.queueOp("play", id(3), 0, 0);
        gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(gen, 600000, 600000, false, "ended");
        ok("repeat OFF at the end of the queue: it stays on the last item", r.p.current() == 3);
        r.p.setRepeat(pbplayer::Repeat::All);
        r.p.pos(gen, 600000, 600000, false, "ended");
        ok("repeat ALL: wraps to the first", r.p.current() == 0);
        r.p.setRepeat(pbplayer::Repeat::One);
        gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        const size_t cmds = r.h.countOf("playerCmd");
        r.p.pos(gen, 600000, 600000, false, "ended");
        ok("repeat ONE: the same item restarts", r.p.current() == 0 && r.h.countOf("playerCmd") == cmds + 1);
        r.p.setRepeat(pbplayer::Repeat::Off);
        r.p.queueOp("move", id(0), 2, 0);
        ok("MOVE the playing item to 3rd: the order changes, the current follows it",
           r.p.items()[2].path == A && r.p.current() == 2);
        r.p.queueOp("remove", r.p.items()[0].id, 0, 0);
        ok("REMOVE another item: the current stays the same file", r.p.items().size() == 3 && r.p.items()[r.p.current()].path == A);
        r.p.queueOp("play", r.p.items()[r.p.current()].id, 0, 0);
        ok("PLAY the item already playing: restart, no reload of the session", fs(r.h.lastOf("playerCmd"), L"op") == "restart");
        r.p.queueOp("remove", r.p.items()[r.p.current()].id, 0, 0);
        ok("REMOVE the playing item: unloaded (idle), the queue keeps the rest", !r.p.loaded() && r.p.items().size() == 2
           && fs(r.h.lastOf("translatorFile"), L"phase") == "idle");
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        r.p.queueOp("clear", 0, 0, 0);
        ok("CLEAR: everything but the playing item", r.p.items().size() == 1 && r.p.loaded() && r.p.current() == 0);
        // shuffle
        Rig s;
        s.p.setHelperAvailable(false);
        std::vector<std::wstring> many;
        for (int i = 0; i < 12; ++i) { many.push_back(L"D:\\M\\t" + std::to_wstring(i) + L".mp3"); s.h.addFile(many.back()); }
        s.p.open(many, false);
        s.p.setShuffle(true);
        const auto order = s.p.playOrderIds();
        std::set<uint64_t> uniq(order.begin(), order.end());
        bool same = true;
        for (size_t i = 0; i < order.size(); ++i) same = same && order[i] == s.p.items()[i].id;
        ok("SHUFFLE: the current item first, then a permutation of the rest (not the queue's order)",
           order.size() == 12 && uniq.size() == 12 && order[0] == s.p.items()[0].id && !same);
        std::set<uint64_t> played;
        played.insert(s.p.items()[s.p.current()].id);
        for (int i = 0; i < 11; ++i) { s.p.queueOp("next", 0, 0, 0); played.insert(s.p.items()[s.p.current()].id); }
        ok("...NEXT eleven times visits all twelve exactly once", played.size() == 12);
        s.p.seedShuffle(12345);
        Rig s2;
        s2.p.setHelperAvailable(false);
        for (const auto& m : many) s2.h.addFile(m);
        s2.p.open(many, false);
        s2.p.setShuffle(true);
        ok("...the same seed draws the same order (deterministic under test)", s2.p.playOrderIds() == order);
        ok("every post well-formed", r.h.badJson + s.h.badJson == 0);
    }

    // ---- 9. missing files, the proxy, sidecars, external subtitles -----------------------------------------------------------------------
    std::printf("[9] missing files, the proxy, sidecars, external subtitles\n");
    {
        Rig r;
        r.h.addFile(A); r.h.addFile(B); r.h.addFile(C);
        r.p.open({ A, B, C }, false);
        r.h.disk.erase(pbtrf::lowerW(B));
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(gen, 600000, 600000, false, "ended");
        ok("a file that vanished is SKIPPED by the auto-advance (with a notice and a missing badge)",
           r.p.current() == 2 && r.h.countOf("translatorFileNotice") >= 1
           && a(r.h.lastOf("playerQueue")).find("\"state\":\"missing\"") != std::string::npos);
        const long long seq = fn(r.h.lastOf("translatorFile"), L"seq");
        r.p.playFailed(seq - 1);
        ok("a PlayFailed of another session: nothing", r.h.proxies.empty());
        r.p.playFailed(seq);
        ok("PlayFailed: FPROXY for this file", r.h.proxies.size() == 1 && r.h.proxies[0].src == a(C));
        tp::WireProxyDone pd; pd.jobId = r.h.proxies[0].jobId; pd.ok = true; pd.out = r.h.proxies[0].out;
        r.p.onProxyDone(pd);
        ok("FPROXYDONE ok: the proxy URL on the proxy host", fs(r.h.lastOf("translatorFile"), L"proxyUrl").rfind("https://suvtitle-proxy.example/", 0) == 0);
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        ok("leaving the file deletes its proxy", !r.h.deleted.empty() && r.h.deleted.back() == pbtrf::fromUtf8(r.h.proxies[0].out));
        // mapping failure -> the proxy at once
        Rig m;
        m.h.addFile(X);
        m.h.mapFail.insert(pbtrf::lowerW(L"E:\\Other"));
        m.p.open({ X }, false);
        ok("a folder that cannot be mapped: no URL, the sound asked for at once", fs(m.h.lastOf("translatorFile", true), L"url").empty()
           && m.h.proxies.size() == 1, a(m.h.lastOf("translatorFile", true)).substr(0, 200));
        // sidecars + external subtitles
        Rig s;
        s.h.addFile(A);
        pbplayer::Sidecar sc; sc.name = L"Ep 1.en.srt"; sc.hint = L"en"; sc.path = L"D:\\Films\\Season 1\\Ep 1.en.srt";
        s.h.sidecarMap[pbtrf::lowerW(L"D:\\Films\\Season 1|Ep 1.mkv")] = { sc };
        s.p.open({ A }, false);
        const std::wstring st = s.h.lastOf("translatorFile", true);
        ok("the sidecar is offered (name + language hint, an id -- never its path)",
           a(st).find("\"sidecars\":[{\"id\":") != std::string::npos && a(st).find("Ep 1.en.srt") != std::string::npos
           && a(st).find("Season 1\\\\Ep 1.en.srt") == std::string::npos, a(st).substr(0, 500));
        std::wstring path;
        const long long scid = std::stoll(a(st).substr(a(st).find("\"sidecars\":[{\"id\":") + 18));
        ok("...and resolved to its path by that id only (the bridge reads it)", s.p.sidecarPath(static_cast<uint64_t>(scid), path)
           && path == sc.path && !s.p.sidecarPath(999999, path));
        s.p.setExtSub(scid, { L"{\"type\":\"playerSubs\",\"id\":7,\"part\":0,\"parts\":1,\"cues\":[]}" });
        s.h.clearPosts();
        s.p.windowReady();
        ok("a page RELOAD gets the state, the cues, the queue and the external subtitle back",
           s.h.countOf("translatorFile") == 1 && s.h.countOf("playerQueue") == 1 && s.h.countOf("playerSubs") == 1);
        ok("every post well-formed", r.h.badJson + m.h.badJson + s.h.badJson == 0);
    }

    // ---- 10. resume, analyse, cancel, close ---------------------------------------------------------------------------------------------
    std::printf("[10] resume, analyse, cancel, close\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(gen, 751000, 3000000, true, "tick");
        r.p.close();
        ok("CLOSE: idle, a new generation, the queue kept", !r.p.loaded() && fs(r.h.lastOf("translatorFile", true), L"phase") == "idle"
           && fn(r.h.lastOf("translatorFile", true), L"jobId") > gen && r.p.items().size() == 1);
        r.h.files.clear();
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        const std::wstring st = r.h.lastOf("translatorFile", true);
        ok("played again: it RESUMES at 12:31 (startMs, resumed) and the job is focused there",
           fn(st, L"startMs") == 751000 && fb(st, L"resumed") && r.h.files.size() == 1 && r.h.files[0].focusMs == 751000, a(st).substr(0, 300));
        gen = fn(st, L"jobId");
        r.p.pos(gen, 2900000, 3000000, false, "pause");
        r.p.close();
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        ok("left near its end (97 %): it starts over (no resume)", fn(r.h.lastOf("translatorFile", true), L"startMs") == 0);
        r.h.now += 20000; r.p.tick();
        std::map<std::string, pbplayer::ResumeEntry> m;
        pbplayer::parseResume(r.h.resumeBytes, m);
        ok("the resume table is written and reads back (the finished entry gone)", r.h.resumeSaves >= 1 && m.empty(), r.h.resumeBytes);
        // cancel + analyse
        gen = fn(r.h.lastOf("translatorFile", true), L"jobId");
        const uint64_t job = r.h.files.back().jobId;
        r.p.onProg(prog(job, "analyzing", 3000000, "0-30000"));
        r.p.cancel(gen);
        ok("CANCEL: FCANCEL, \"cancelling\"", r.h.cancels.back() == job && fb(r.h.lastOf("translatorFile", true), L"cancelling"));
        r.p.onDone(done(job, "cancelled"), false);
        const size_t nf = r.h.files.size();
        r.p.pos(gen, 1000, 3000000, true, "tick");
        ok("...cancelled by the USER: never restarted by itself", fs(r.h.lastOf("translatorFile", true), L"phase") == "cancelled"
           && r.h.files.size() == nf);
        r.p.analyse(false);
        ok("ANALYSE AGAIN: a new generation, resuming from [0,30000]", r.h.files.size() == nf + 1 && r.h.files.back().have == "0-30000"
           && fn(r.h.lastOf("translatorFile", true), L"jobId") > gen);
        const uint64_t j2 = r.h.files.back().jobId;
        r.p.analyse(true);
        ok("RE-ANALYSE (force): the running job cancelled, a fresh FILE with no `have`",
           r.h.cancels.back() == j2 && r.h.files.back().have.empty() && r.h.files.size() == nf + 2);
        ok("every post well-formed", r.h.badJson == 0);
    }

    // ---- 11. the queue file, the scan --------------------------------------------------------------------------------------------------------
    std::printf("[11] the queue file, the folder scan\n");
    {
        pbplayer::QueueFile q;
        q.translate = false; q.repeat = pbplayer::Repeat::All; q.shuffle = true; q.cur = 1;
        pbplayer::QueueFile::It i1; i1.path = L"D:\\Films\\R\x00e9union #2 100%\\a b.mkv"; i1.size = 5; i1.mtime = 6; i1.durMs = 7;
        pbplayer::QueueFile::It i2; i2.path = L"\\\\nas\\share\\x\ty.mp3"; i2.size = 8;
        q.items = { i1, i2 };
        q.order = { 1, 0 };
        const std::string b = pbplayer::packQueue(q);
        pbplayer::QueueFile r;
        ok("player.txt round-trips (accents, #, %, spaces, a UNC path, a tab escaped)", pbplayer::parseQueue(b, r) && !r.translate
           && r.repeat == pbplayer::Repeat::All && r.shuffle && r.cur == 1 && r.items.size() == 2 && r.items[0].path == i1.path
           && r.items[1].path == i2.path && r.items[0].durMs == 7 && r.order.size() == 2 && r.order[0] == 1);
        ok("a torn queue file (no END) is an EMPTY queue, never half of one",
           !pbplayer::parseQueue(b.substr(0, b.rfind("END")), r) && r.items.empty());
        std::string badOrder = b;
        badOrder.replace(badOrder.find("ORDER\t1,0"), 9, "ORDER\t1,1");
        ok("an ORDER that is not a permutation is dropped (a fresh shuffle is drawn)", pbplayer::parseQueue(badOrder, r) && r.order.empty());
        // a Player restored from it
        Rig p;
        p.p.load(b, "");
        ok("a player LOADED from it: two items, the current one remembered, nothing playing",
           p.p.items().size() == 2 && p.p.current() == 1 && !p.p.loaded() && !p.p.translate() && p.p.repeat() == pbplayer::Repeat::All);
        // the scan, on the real disk
        wchar_t tmp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring(tmp) + L"apb_scan_" + std::to_wstring(::GetCurrentProcessId());
        auto mk = [&](const std::wstring& rel) { const std::wstring p2 = pbtrf::joinPath(root, rel); ::CreateDirectoryW(pbtrf::dirOf(p2).c_str(), nullptr);
                                                 HANDLE h = ::CreateFileW(p2.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); };
        ::CreateDirectoryW(root.c_str(), nullptr);
        ::CreateDirectoryW((root + L"\\Season 2").c_str(), nullptr);
        ::CreateDirectoryW((root + L"\\Season 10").c_str(), nullptr);
        ::CreateDirectoryW((root + L"\\.hidden").c_str(), nullptr);
        ::SetFileAttributesW((root + L"\\.hidden").c_str(), FILE_ATTRIBUTE_HIDDEN);
        mk(L"Ep 10.mkv"); mk(L"Ep 2.mkv"); mk(L"Ep 1.MP4"); mk(L"notes.txt"); mk(L"Ep 1.en.srt");
        mk(L"Season 2\\b.mp3"); mk(L"Season 10\\a.wav"); mk(L".hidden\\secret.mp4");
        const bool link = ::CreateSymbolicLinkW((root + L"\\loop").c_str(), root.c_str(),
                                                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
        pbplayer::ScanResult sr;
        pbplayer::scanFolder(root, pbplayer::ScanCaps{}, sr);
        std::string got;
        for (const auto& f : sr.files) got += a(f.substr(root.size() + 1)) + "|";
        ok("scan: media only, NATURAL order (Ep 1 < Ep 2 < Ep 10), files before subfolders (Season 2 < Season 10), hidden skipped",
           got == "Ep 1.MP4|Ep 2.mkv|Ep 10.mkv|Season 2\\b.mp3|Season 10\\a.wav|", got);
        std::printf("  INFO  directory symlink for the loop test: %s\n", link ? "created (skipped by the scan)" : "not creatable here (no developer mode)");
        pbplayer::ScanCaps caps; caps.maxMedia = 2;
        pbplayer::ScanResult sr2;
        pbplayer::scanFolder(root, caps, sr2);
        ok("the media cap: the first two, marked truncated", sr2.files.size() == 2 && sr2.truncated);
        // tidy up
        const wchar_t* files[] = { L"Ep 10.mkv", L"Ep 2.mkv", L"Ep 1.MP4", L"notes.txt", L"Ep 1.en.srt", L"Season 2\\b.mp3", L"Season 10\\a.wav", L".hidden\\secret.mp4" };
        for (const wchar_t* f : files) ::DeleteFileW(pbtrf::joinPath(root, f).c_str());
        ::RemoveDirectoryW((root + L"\\loop").c_str());
        ::SetFileAttributesW((root + L"\\.hidden").c_str(), FILE_ATTRIBUTE_NORMAL);
        ::RemoveDirectoryW((root + L"\\.hidden").c_str());
        ::RemoveDirectoryW((root + L"\\Season 2").c_str());
        ::RemoveDirectoryW((root + L"\\Season 10").c_str());
        ::RemoveDirectoryW(root.c_str());
        ok("the temp tree is gone", ::GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    // ---- 12. the adversarial review of WE (REVIEW_WE.md): each defect, as it was found ----------------------
    std::printf("[12] the review of WE: C1-C5, P1, P2, P4, P8\n");
    {
        // C1: "AudioPB Player..." is ONE-SHOT -- Live stays Live after hide / show / full screen / reload
        pbplayer::WindowState w;
        std::vector<std::wstring> posts;
        auto post = [&] { if (w.canPost()) posts.push_back(w.post()); };
        auto hasFile = [](const std::wstring& j) { return j.find(L"\"file\":true") != std::wstring::npos; };
        w.attached();
        w.askFile(); post();                                  // the tray item while the page still loads
        ok("C1: nothing is posted to a page that has not said `ready` (it would be lost, or reach the OLD document)",
           posts.empty());
        w.ready(); post();                                    // its `ready`: the bridge posts the state
        ok("C1: ...its `ready` carries file:true ONCE (the page shows its File tab)",
           posts.size() == 1 && hasFile(posts[0]) && validJson(posts[0]), posts.empty() ? std::string() : a(posts[0]));
        w.shown = false; post(); w.shown = true; post();      // the user goes back to Live, then: hide, show,
        w.fullscreen = true; post(); w.fullscreen = false; post();   // full screen in and out,
        w.reloading(); post(); w.ready(); post();             // a reload for a new folder
        bool anyFile = false;
        for (size_t i = 1; i < posts.size(); ++i) anyFile = anyFile || hasFile(posts[i]);
        ok("C1: LIVE STAYS LIVE -- no later post (hide, show, full screen, reload + ready) says file again",
           posts.size() == 6 && !anyFile, n2s(posts.size()));
        w.askFile(); post(); post();
        ok("C1: the tray item again, on a ready page: file:true once more, and only once",
           posts.size() == 8 && hasFile(posts[6]) && !hasFile(posts[7]));
        w.detached(); w.askFile(); post();
        ok("C1: a detached window gets nothing (the next one's `ready` will)", posts.size() == 8 && !w.pageReady);
    }
    {
        // SuVtitle FIX ROUND (2026-10-23, review RV4-7 = SPLIT_PLAN ADDENDUM): translatorWindow carries `topmost` -- the
        // window's REAL always-on-top state, read back by main.cpp -- on EVERY post; the page's pin follows it
        pbplayer::WindowState w;
        w.attached();
        w.ready();
        const std::wstring p0 = w.post();
        ok("RV4-7: every translatorWindow post carries topmost (false: the window is not on top)",
           p0.find(L"\"topmost\":false") != std::wstring::npos && validJson(p0), a(p0));
        // >>> new API (the fix round's proof strips this block to build the file over the code BEFORE the fix)
        w.topmost = true;
        const std::wstring p1 = w.post();
        w.askFile();
        const std::wstring p2 = w.post();
        w.fullscreen = true;
        const std::wstring p3 = w.post();
        ok("RV4-7: ...true when it is: on every post (the one-shot file request's, a full-screen change's)",
           p1.find(L"\"topmost\":true") != std::wstring::npos && p2.find(L"\"topmost\":true") != std::wstring::npos
           && p2.find(L"\"file\":true") != std::wstring::npos && p3.find(L"\"topmost\":true") != std::wstring::npos
           && p3.find(L"\"fullscreen\":true") != std::wstring::npos && validJson(p2), a(p2));
        // <<< new API
    }
    {
        // C2: open() of files partly queued AFTER the current one plays the FIRST of them, not what shifted in
        Rig r;
        const std::wstring Xr = L"E:\\Other\\x.mp3", A1 = L"D:\\Ar\\a1.mkv", A2 = L"D:\\Ar\\a2.mkv", A3 = L"D:\\Ar\\a3.mkv";
        r.h.addFile(Xr); r.h.addFile(A1); r.h.addFile(A2); r.h.addFile(A3);
        r.p.open({ Xr }, false);
        r.p.add({ A1, A2 });
        r.p.open({ A1, A2, A3 }, false);                      // the user drops folder Ar (a1 a2 a3) to PLAY it
        std::string q;
        for (const auto& it : r.p.items()) q += a(it.name) + " ";
        ok("C2: the folder's FIRST file plays (a1) -- not the item the insert shifted into its index (a3)",
           a(r.p.session().name) == "a1.mkv" && r.p.items().size() == 4, q + "| playing " + a(r.p.session().name));
        Rig r2;
        r2.h.addFile(A1); r2.h.addFile(A2);
        r2.p.open({ A1, A2, A1, A2 }, false);
        ok("C2: a path given twice in one call is queued once", r2.p.items().size() == 2 && a(r2.p.session().name) == "a1.mkv",
           n2s(r2.p.items().size()));
    }
    {
        // C3: a resumed run's first cue starting AT have.e is kept (the held one ends 1 ms past it: allowed)
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        r.p.open({ Ar }, false);
        uint64_t job = r.h.files.back().jobId;
        r.p.onProg(prog(job, "analyzing", 600000, ""));
        r.p.onCue(cue(job, 0, 1000, 2000));
        r.p.onCue(cue(job, 1, 29000, 30001));                 // ends 1 ms past its range (TrPipe.h PRECISIONS)
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.onDone(done(job, "cancelled"), false);            // the helper's own last word vouches 0-30000
        r.p.analyse(false);                                   // resume (have = 0-30000)
        job = r.h.files.back().jobId;
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.onCue(cue(job, 0, 30000, 31500));                 // starts AT have.e: touches the held cue by 1 ms
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        const auto& cs = r.p.session().set->cues;
        bool kept = false, clamped = false;
        for (const auto& c : cs) { if (c.startMs == 30000) kept = true; if (c.startMs == 29000 && c.endMs == 30000) clamped = true; }
        ok("C3: the resumed run's first cue (at have.e, touching the held one by 1 ms) is KEPT",
           kept && cs.size() == 3, n2s(cs.size()));
        ok("C3: ...and the touch is clamped away: the held cue now ends where the new one starts (no overlap left)", clamped);
        r.p.onCue(cue(job, 1, 31000, 32000));                 // a REAL overlap (500 ms) is still refused
        ok("C3: a REAL overlap (> 1 ms) is still refused", r.p.session().set->cues.size() == 3);
        r.p.onCue(cue(job, 2, 50000, 51000));                 // v2: a LATER cue may come first (runs interleave)
        r.p.onCue(cue(job, 3, 49000, 50001));                 // ...then one ending 1 ms into it
        bool nextClamped = false;
        for (const auto& c : r.p.session().set->cues) if (c.startMs == 49000 && c.endMs == 50000) nextClamped = true;
        ok("C3: ...and a newcomer ending 1 ms into the NEXT held cue is kept, its end clamped to that start",
           r.p.session().set->cues.size() == 5 && nextClamped);
    }
    {
        // C4: a switch DURING a reload is judged against what the reloading page will see
        Rig r(false);
        std::vector<std::wstring> v;
        for (int k = 0; k < 20; ++k) { std::wstring f = L"D:\\F" + std::to_wstring(k) + L"\\e.mkv"; r.h.addFile(f); v.push_back(f); }
        r.p.add(v);
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        r.p.windowReady();
        const int rl0 = r.h.reloads;
        r.p.queueOp("play", r.p.items()[17].id, 0, 0);       // F17: not committed -> map + reload
        r.p.queueOp("play", r.p.items()[14].id, 0, 0);       // before `ready`: F14, on the OLD page, not in the new mapping
        bool f14 = false;
        for (const auto& f : r.h.maps.back()) if (pbtrf::lowerW(f) == pbtrf::lowerW(L"D:\\F14")) f14 = true;
        ok("C4: a second switch during the reload, to a folder the NEW mapping lacks, maps it and reloads again",
           r.h.reloads == rl0 + 2 && f14, n2s(r.h.reloads - rl0));
    }
    {
        // C5: translation Off -> On mid-film re-posts the SAME file: no autoplay, no "Resumed", start = the playhead
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        std::string res = "# AudioPB player resume v1\nPOS\t" + tp::esc(pbplayer::resumeKey(Ar, 5000, 11)) + "\t20000\t600000\t1760000000\n";
        r.p.load(std::string(), res);
        r.p.open({ Ar }, false);
        const uint64_t job = r.h.files.back().jobId;
        const std::wstring st0 = r.h.lastOf("translatorFile");
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-90000"));
        r.p.onCue(cue(job, 0, 91000, 92000));                 // in flight: no FPROG covers it -> a regen later
        r.p.pos(static_cast<uint64_t>(fn(st0, L"jobId")), 300000, 600000, false, "pause");   // PAUSED at 5:00
        r.p.setTranslate(false, 300000);
        r.p.onDone(done(job, "cancelled"), false);
        r.h.clearPosts();
        r.p.setTranslate(true, 301000);
        const std::wstring st1 = r.h.lastOf("translatorFile");
        ok("C5: (opened: autoplay, resumed at 0:20)", fb(st0, L"autoplay") && fb(st0, L"resumed") && fn(st0, L"startMs") == 20000);
        ok("C5: Off -> On re-posts the SAME file as no opening: autoplay false, resumed false, start = the playhead",
           !st1.empty() && fn(st1, L"jobId") > fn(st0, L"jobId") && !fb(st1, L"autoplay") && !fb(st1, L"resumed")
           && fn(st1, L"startMs") == 301000, a(st1).substr(0, 260));
    }
    {
        // P1: while the window's page reloads NOTHING is posted to it (the OLD document would get it)
        Rig r(false);
        const std::wstring Ar = L"D:\\Ar\\a.mkv", Br = L"E:\\Br\\b.mkv";
        r.h.addFile(Ar); r.h.addFile(Br);
        r.p.open({ Ar }, false);
        r.p.windowReady();
        r.p.add({ Br });
        const size_t w0 = r.h.window.size();
        r.h.mains.clear();
        r.p.queueOp("play", r.p.items()[1].id, 0, 0);        // another folder: map + RELOAD
        r.p.notice(L"said during the reload");
        const size_t during = r.h.window.size() - w0;
        ok("P1: during the reload NOTHING reaches the window (no state, cue, queue, item, notice)", during == 0, n2s(during));
        ok("P1: ...the CARD still gets the state (the main view only)", !r.h.mains.empty() && typeOf(r.h.mains.back()) == "translatorFile");
        r.p.windowReady();
        bool notice = false;
        for (size_t i = w0; i < r.h.window.size(); ++i) if (typeOf(r.h.window[i]) == "translatorFileNotice") notice = true;
        ok("P1: its `ready` brings it all back -- the state, the queue, and the notice said meanwhile",
           !r.h.lastOf("translatorFile").empty() && !r.h.lastOf("playerQueue").empty() && notice);
    }
    {
        // P2: a folder scan's identities are not read again on the UI thread; duplicates dropped in O(n log n)
        Rig r;
        std::vector<std::wstring> paths;
        std::vector<Player::Known> known;
        for (int k = 0; k < 50; ++k) {
            const std::wstring f = L"\\\\nas\\share\\S1\\ep" + std::to_wstring(k) + L".mkv";
            r.h.addFile(f, 1000 + k, 42);
            paths.push_back(f);
            known.push_back(Player::Known{ static_cast<uint64_t>(1000 + k), 42, true });
        }
        r.h.identityCalls = 0;
        r.p.open(paths, false, &known);
        ok("P2: 50 files from a scan, with their identities: ONE disk call (the file that plays is checked), not 50",
           r.p.items().size() == 50 && r.h.identityCalls == 1, n2s(r.h.identityCalls));
        r.h.identityCalls = 0;
        r.p.add(paths, &known);
        ok("P2: ...the same 50 again: all known, none added, no disk call", r.p.items().size() == 50 && r.h.identityCalls == 0);
    }
    {
        // P4: a TICK never chases a playhead the job is heading to; a seek still refocuses
        const std::vector<tp::MsRange> r0 = rg("0-30000");
        ok("P4: a tick in a gap AHEAD of the last focus (the job is heading there) -> no FFOCUS",
           !pbplayer::shouldFocus(r0, 600000, 45000, 0, pbplayer::PosReason::Tick));
        ok("P4: ...a SEEK there -> FFOCUS (the user's own jump)", pbplayer::shouldFocus(r0, 600000, 45000, 0, pbplayer::PosReason::Seek));
        ok("P4: ...a tick in a gap BEHIND the last focus -> FFOCUS (the job is elsewhere)",
           pbplayer::shouldFocus(r0, 600000, 45000, 120000, pbplayer::PosReason::Tick));
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        r.p.open({ Ar }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onProg(prog(job, "analyzing", 600000, "0-20000"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-20000"));
        const size_t f0 = r.h.focuses.size();
        for (uint64_t t = 25000; t <= 60000; t += 5000) r.p.pos(static_cast<uint64_t>(gen), t, 600000, true, "tick");
        ok("P4: playing past the analysis (CPU), a tick every 5 s for 35 s: not ONE refocus (it used to be every tick)",
           r.h.focuses.size() == f0, n2s(r.h.focuses.size() - f0));
    }
    {
        // P8: the session ends -- the queue, the positions and the vouched analysis are written, nothing stopped
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        r.p.open({ Ar }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(cue(job, 0, 1000, 2000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        const int cw = r.h.cacheWrites, qs = r.h.queueSaves;
        const size_t cancels = r.h.cancels.size();
        r.p.persist();
        pbtrf::Record rec;
        ok("P8: persist() writes the vouched partial, the queue -- and stops nothing (no cancel)",
           r.h.cacheWrites == cw + 1 && r.h.queueSaves == qs + 1 && r.h.cancels.size() == cancels
           && r.h.readCache(pbtrf::cacheKey(Ar, 5000, 11, "en", "fr", "auto", "auto"), rec) && !rec.complete
           && rec.ranges.size() == 1 && rec.ranges[0].e == 30000);
    }
    {
        // P3: the pair from the helper's own translator.txt (read only), as TrConfig::load reads it
        std::string a1 = "en", b1 = "zh", m1 = "auto";
        const bool got = pbplayer::pairFromTranslatorCfg("# AudioPB Translator config v2\r\nPRESET\tbalanced\r\nMODE\ten2zh\r\n"
                                                         "LANGA\tfr\r\nLANGB\tde\r\nWINPOS\t1\t2\t3\t4\t0\r\n", a1, b1, m1);
        ok("P3: translator.txt's pair (fr/de) and its mode, migrated like the helper does (en2zh -> a2b)",
           got && a1 == "fr" && b1 == "de" && m1 == "a2b", a1 + "/" + b1 + "/" + m1);
        std::string a2 = "en", b2 = "zh", m2 = "auto";
        ok("P3: ...a file without them changes nothing", !pbplayer::pairFromTranslatorCfg("PRESET\tlight\n", a2, b2, m2)
           && a2 == "en" && b2 == "zh" && m2 == "auto");
    }

    // ---- 13. WG: ONE FILE PER LANGUAGE, written as the analysis goes; the shared original ----------------------
    std::printf("[13] WG: one .srt per language, progressive, the shared original\n");
    auto lc = [](uint64_t job, uint32_t idx, uint64_t s, uint64_t e, const char* lang, const char* dl, const char* src, const char* dst,
                 bool pass = false) {
        tp::WireCue c;
        c.jobId = job; c.idx = idx; c.startMs = s; c.endMs = e; c.lang = lang; c.dstLang = dl; c.src = src; c.dst = dst; c.pass = pass;
        return c;
    };
    const std::wstring Fm = L"D:\\Films\\Feature.mkv";
    const std::wstring fFr = L"D:\\Films\\Feature.fr.srt", fEn = L"D:\\Films\\Feature.en.srt", fZh = L"D:\\Films\\Feature.zh-CN.srt";
    {
        // what each file holds: its language's text of every cue -- the user's .en.srt held 109 FRENCH blocks
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Salut.", "Hi."));
        r.p.onCue(lc(job, 1, 3000, 4000, "en", "fr", "Meep meep!", "Bip bip !"));      // detected "en": translated INTO French
        r.p.onCue(lc(job, 2, 5000, 6000, "ru", "en", "TREVOZHNAYA", "Alarming music"));  // a third language: no French text
        r.p.onCue(lc(job, 3, 7000, 8000, "fr", "fr", "Ok.", "Ok.", true));               // a passthrough: once
        r.p.onProg(prog(job, "analyzing", 600000, "0-10000"));
        r.p.onDone(done(job, "ok"), false);
        ok("WG: the FRENCH file = each cue's French text: the French sources, the translation of the line heard as English, the passthrough once",
           r.h.srtDisk[fFr] == "1000-2000:Salut.\n3000-4000:Bip bip !\n7000-8000:Ok.\n", r.h.srtDisk[fFr]);
        ok("WG: the ENGLISH file = each cue's English text (the English source, the translations into English) -- no French in it",
           r.h.srtDisk[fEn] == "1000-2000:Hi.\n3000-4000:Meep meep!\n5000-6000:Alarming music\n", r.h.srtDisk[fEn]);
        ok("WG: the tags players read: zh -> zh-CN, zh-tw -> zh-TW, the rest as they are",
           pbtrf::fileLangTag("zh") == "zh-CN" && pbtrf::fileLangTag("zh-tw") == "zh-TW" && pbtrf::fileLangTag("FR") == "fr" && pbtrf::fileLangTag("ar") == "ar");
        pbtrf::Record rec;
        ok("WG: the record names both files (SRTL lines), and reads back",
           r.h.readCache(pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto"), rec) && rec.srtFiles.size() == 2
           && rec.srtFiles[0].lang == "fr" && pbtrf::samePath(rec.srtFiles[0].path, fFr) && rec.srtFiles[1].lang == "en");
    }
    {
        // PROGRESSIVE: written within 30 s of the first cue the files lack, and at once on a pause, a switch, the end
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.addFile(A, 5000, 11);
        r.p.open({ Fm }, false);
        r.p.add({ A });
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Un.", "One."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-5000"));
        r.h.now += 29000; r.p.tick();
        ok("WG progressive: 29 s after the first new cue, not written yet", r.h.srtWrites == 0, n2s(r.h.srtWrites));
        r.p.onCue(lc(job, 1, 6000, 7000, "fr", "en", "Deux.", "Two."));   // more arrive: the 30 s still count from the FIRST
        r.h.now += 1000; r.p.tick();
        ok("WG progressive: 30 s after the FIRST one (more came meanwhile): both files written, with everything so far",
           r.h.srtWrites == 2 && r.h.srtDisk[fFr] == "1000-2000:Un.\n6000-7000:Deux.\n", r.h.srtDisk[fFr]);
        r.h.now += 60000; r.p.tick();
        ok("WG progressive: nothing new, nothing written", r.h.srtWrites == 2);
        r.p.onCue(lc(job, 2, 8000, 9000, "fr", "en", "Trois.", "Three."));
        r.h.now += 5000;
        r.p.pos(static_cast<uint64_t>(gen), 8500, 600000, false, "pause");
        ok("WG progressive: a PAUSE writes at once (the user may look at the files now)",
           r.h.srtWrites == 4 && r.h.srtDisk[fEn] == "1000-2000:One.\n6000-7000:Two.\n8000-9000:Three.\n", r.h.srtDisk[fEn]);
        r.p.onCue(lc(job, 3, 10000, 11000, "fr", "en", "Quatre.", "Four."));
        r.p.queueOp("play", r.p.items()[1].id, 0, 0);                    // switch to the next file
        ok("WG progressive: a SWITCH writes the file left at once (and cancels its job)",
           r.h.srtWrites == 6 && r.h.srtDisk[fFr].find("10000-11000:Quatre.") != std::string::npos);
        r.p.onDone(done(job, "cancelled"), false);
        ok("WG progressive: the cancelled job's FDONE has nothing left to add (identical: no write)", r.h.srtWrites == 6, n2s(r.h.srtWrites));
    }
    {
        // the SHARED ORIGINAL: he analysed in Chinese first (fr<->zh, complete), then switched to English
        Rig r;
        r.h.addFile(Fm, 7000, 13);
        pbtrf::Record zh;
        zh.path = pbtrf::toUtf8(Fm); zh.size = 7000; zh.mtime = 13;
        zh.langA = "fr"; zh.langB = "zh"; zh.mode = "auto"; zh.quality = "auto"; zh.totalMs = 600000; zh.complete = true;
        zh.engine = tp::kFileEngineVersion;                  // (ENGINE: a record of THIS engine -- an older one is a miss)
        zh.ranges.push_back(tp::MsRange{ 0, 600000 });
        zh.cues.push_back(lc(1, 0, 1000, 2000, "fr", "zh", "Bonjour tout le monde.", "ZH1"));
        zh.cues.push_back(lc(1, 1, 3000, 4000, "fr", "zh", "La suite.", "ZH2"));
        zh.cues.push_back(lc(1, 2, 300000, 301000, "fr", "zh", "Bien plus loin.", "ZH3"));
        r.h.cache[pbtrf::cacheKey(Fm, 7000, 13, "fr", "zh", "auto", "auto")] = pbtrf::packRecord(zh);
        r.p.setPair("fr", "en", "auto");
        r.p.open({ Fm }, false);                          // fr<->en: nothing yet, a job starts
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Bonjour a tous.", "Hello everyone."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.h.peeks = 0;
        r.h.now += 31000; r.p.tick();
        ok("WG shared original: the FRENCH file comes from the more complete fr<->zh analysis -- NOT shrunk to the first minute of fr<->en",
           r.h.srtDisk[fFr] == "1000-2000:Bonjour tout le monde.\n3000-4000:La suite.\n300000-301000:Bien plus loin.\n", r.h.srtDisk[fFr]);
        ok("WG shared original: ...the English file from fr<->en, and the CHINESE file is never touched by fr<->en",
           r.h.srtDisk[fEn] == "1000-2000:Hello everyone.\n" && !r.h.srtDisk.count(fZh));
        ok("WG shared original: the candidates are a closed list (12 other languages x 2 orders x 3 directions x 2 qualities, per language)",
           r.h.peeks <= 2 * 144 && r.h.peeks >= 144, n2s(r.h.peeks));
        r.p.onCue(lc(job, 1, 61000, 62000, "fr", "en", "Plus tard.", "Later."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(job, "ok"), false);               // fr<->en complete: a tie with fr<->zh -> the set worked on wins
        ok("WG shared original: once fr<->en is complete (a tie), the French file is written from IT",
           r.h.srtDisk[fFr] == "1000-2000:Bonjour a tous.\n61000-62000:Plus tard.\n", r.h.srtDisk[fFr]);
        // back to Chinese: the zh file is that pair's, the fr file stays the most complete one's
        r.p.setPair("fr", "zh", "auto");
        r.p.analyse(false);                               // "Languages changed -> Re-analyse": the fr<->zh cache
        ok("WG shared original: back to fr<->zh: its Chinese file is written (from its own, complete, record)",
           r.h.srtDisk[fZh] == "1000-2000:ZH1\n3000-4000:ZH2\n300000-301000:ZH3\n", r.h.srtDisk[fZh]);
    }
    {
        // never an EMPTY file: a language with no line yet keeps what it had
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.srtDisk[fEn] = "an earlier analysis";
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "fr", "Que du francais.", "Que du francais.", true));
        r.h.now += 31000; r.p.tick();
        ok("WG: a language with no line yet (no English so far) is NOT written -- its file is not emptied",
           r.h.srtDisk[fEn] == "an earlier analysis" && r.h.srtDisk[fFr] == "1000-2000:Que du francais.\n");
    }

    // ---- 14. WG: THE SUBTITLE EDITOR -- edits checked by the core, applied, persisted ------------------------------
    std::printf("[14] WG: the subtitle editor (the core's checks, the record's EDIT, the files)\n");
    using EC = Player::EditCue;
    auto ec = [](uint64_t s, uint64_t e, const char* src, const char* dst, const char* lang = "fr", const char* dl = "en") {
        EC c; c.s = s; c.e = e; c.src = src; c.dst = dst; c.lang = lang; c.dstLang = dl; return c;
    };
    auto cueAtStart = [](const pbplayer::CueSet& S, uint64_t s) -> const tp::WireCue* {
        for (const auto& c : S.cues) if (c.startMs == s) return &c;
        return nullptr;
    };
    {
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        pbtrf::Record rec;                                   // a COMPLETE analysis in the cache (the user's case)
        rec.path = pbtrf::toUtf8(Fm); rec.size = 7000; rec.mtime = 13;
        rec.langA = "fr"; rec.langB = "en"; rec.mode = "auto"; rec.quality = "auto"; rec.totalMs = 600000; rec.complete = true;
        rec.engine = tp::kFileEngineVersion;                 // (ENGINE: this engine's -- an older one is a miss)
        rec.ranges.push_back(tp::MsRange{ 0, 600000 });
        rec.cues.push_back(lc(1, 0, 1000, 2000, "fr", "en", "Me", "Moins."));
        rec.cues.push_back(lc(1, 1, 2100, 2150, "fr", "en", "ep meep!", "Ep meep!"));
        rec.cues.push_back(lc(1, 2, 5000, 7000, "fr", "en", "La nuit.", "The night."));
        const std::string key = pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto");
        r.h.cache[key] = pbtrf::packRecord(rec);
        r.p.open({ Fm }, false);
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        const pbplayer::CueSet& S = *r.p.session().set;
        ok("WG editor: (a complete set from the cache: 3 cues, idx 0..2, not edited)", S.cues.size() == 3 && !S.edited && r.h.files.empty());
        r.h.clearPosts();
        // MERGE the "Me" + "ep meep!" fragments (the page computes it: remove 0 and 1, add one cue)
        bool ok1 = r.p.editCues(static_cast<uint64_t>(gen), 7, { 0, 1 }, { ec(1000, 2150, "Meep meep!", "Meep meep!") });
        const std::wstring e1 = r.h.lastOf("translatorFileEdited");
        ok("WG editor: an edit (a merge) is applied: the answer names the rev, the removed idx and the NEW cue with a fresh idx (3)",
           ok1 && fb(e1, L"ok") && fn(e1, L"rev") == 7 && a(e1).find("\"remove\":[0,1]") != std::string::npos
           && a(e1).find("\"idx\":3") != std::string::npos && fb(e1, L"edited") && S.cues.size() == 2 && S.edited, a(e1));
        ok("WG editor: ...the state says edited (the strip, the card)", fb(r.h.lastOf("translatorFile"), L"edited"));
        // REFUSALS, each with its sentence
        auto refused = [&](bool res, const char* has) {
            const std::wstring e = r.h.lastOf("translatorFileEdited");
            return !res && !fb(e, L"ok") && a(e).find(has) != std::string::npos;
        };
        ok("WG editor: an OVERLAP is refused, naming the subtitle in the way",
           refused(r.p.editCues(static_cast<uint64_t>(gen), 8, {}, { ec(4000, 5500, "x", "y") }), "would overlap the subtitle at 0:05"));
        ok("WG editor: a cue that does not END after it starts is refused (the 0.1 s minimum is the page's, for what the user makes)", refused(r.p.editCues(static_cast<uint64_t>(gen), 9, {}, { ec(3000, 3000, "x", "y") }), "end after it starts"));
        ok("WG editor: past the end of the file is refused", refused(r.p.editCues(static_cast<uint64_t>(gen), 10, {}, { ec(599950, 600100, "x", "y") }), "past the end"));
        ok("WG editor: a removed idx that is not there (an edit raced) is refused", refused(r.p.editCues(static_cast<uint64_t>(gen), 11, { 0 }, {}), "changed meanwhile"));
        ok("WG editor: another generation (another file meanwhile) is refused", refused(r.p.editCues(static_cast<uint64_t>(gen) + 99, 12, { 2 }, {}), "changed meanwhile"));
        ok("WG editor: a language that is not a code is refused", refused(r.p.editCues(static_cast<uint64_t>(gen), 13, {}, { ec(3000, 4000, "x", "y", "FR!", "en") }), "language"));
        ok("WG editor: ...and a refusal changes nothing", S.cues.size() == 2);
        // TOUCHING is fine: a new cue that starts where the previous ends
        ok("WG editor: a NEW cue touching its neighbours (2150..5000) is fine",
           r.p.editCues(static_cast<uint64_t>(gen), 14, {}, { ec(2150, 5000, "Ajout.", "Added.") }) && S.cues.size() == 3);
        // control characters cleaned; a passthrough keeps one text
        EC pass = ec(8000, 9000, "  Oui\x01\r\n.  ", "ignored", "fr", "fr");
        pass.pass = true;
        r.p.editCues(static_cast<uint64_t>(gen), 15, {}, { pass });
        const tp::WireCue* pc = cueAtStart(S, 8000);
        ok("WG editor: a text is cleaned (no control character, no blank at the ends); a passthrough's two texts are one",
           pc && pc->src == "Oui\n." && pc->dst == "Oui\n." && pc->pass, pc ? pc->src : std::string("none"));
        // PERSISTENCE: 2 s after the last edit, the record (EDIT 1, the edited cues) and the per-language files
        const int cw = r.h.cacheWrites;
        r.h.now += 1500; r.p.tick();
        ok("WG editor: not saved 1.5 s after the last edit (a burst of edits is one write)", r.h.cacheWrites == cw);
        r.h.now += 600; r.p.tick();
        pbtrf::Record back;
        ok("WG editor: saved 2 s after: the record is EDITED and holds the edits",
           r.h.cacheWrites == cw + 1 && r.h.readCache(key, back) && back.edited && back.complete && back.cues.size() == 4
           && back.cues[0].src == "Meep meep!", n2s(r.h.cacheWrites - cw));
        ok("WG editor: ...and the per-language files are rewritten with them",
           r.h.srtDisk[fFr].find("1000-2150:Meep meep!") == 0 && r.h.srtDisk[fEn].find("2150-5000:Added.") != std::string::npos, r.h.srtDisk[fFr]);
        // UNDO = the inverse edit (the page sends it): remove the merged cue, put the two fragments back
        const tp::WireCue* merged1 = cueAtStart(S, 1000);
        const uint32_t mi = merged1 ? merged1->idx : 999;
        ok("WG editor: an UNDO (the inverse edit) puts the fragments back, with fresh idx (never a reused one)",
           r.p.editCues(static_cast<uint64_t>(gen), 16, { mi }, { ec(1000, 2000, "Me", "Moins."), ec(2100, 2150, "ep meep!", "Ep meep!") })
           && S.cues.size() == 5 && cueAtStart(S, 2100) && cueAtStart(S, 2100)->idx > mi);
        // reopened: the edited set comes back from the cache, still EDITED
        Rig r2;
        r2.p.setPair("fr", "en", "auto");
        r2.h.addFile(Fm, 7000, 13);
        r2.h.cache = r.h.cache;
        r2.p.open({ Fm }, false);
        ok("WG editor: reopened, the file is still EDITED (the state says so; a re-analysis will ask first)",
           r2.p.session().set->edited && fb(r2.h.lastOf("translatorFile"), L"edited"));
        r2.p.analyse(true);                                  // the user confirmed: discard and analyse again
        ok("WG editor: a forced re-analysis starts a clean set (not edited)", !r2.p.session().set->edited && !r2.h.files.empty());
    }
    {
        // WHILE ANALYSING: an edit stays inside what is analysed (the helper never writes there again)
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Un.", "One."));
        r.p.onCue(lc(job, 1, 3000, 4000, "fr", "en", "Deux.", "Two."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        const pbplayer::CueSet& S = *r.p.session().set;
        ok("WG editor (analysing): inside the LAST report, not vouched for yet (no later message) -> refused: the partial "
           "record would not hold it, and the resume would analyse over it",
           !r.p.editCues(static_cast<uint64_t>(gen), 1, { 1 }, { ec(2500, 4500, "Deux !", "Two!") })
           && a(r.h.lastOf("translatorFileEdited")).find("not analysed yet") != std::string::npos);
        ok("WG editor (analysing): ...nor a line taken out there", !r.p.editCues(static_cast<uint64_t>(gen), 1, { 1 }, {}));
        r.p.onProg(prog(job, "analyzing", 600000, "0-35000"));   // the next report vouches for 0-30000
        ok("WG editor (analysing): inside the analysed stretch (vouched) -> applied",
           r.p.editCues(static_cast<uint64_t>(gen), 1, { 1 }, { ec(2500, 4500, "Deux !", "Two!") }));
        ok("WG editor (analysing): in 30-35 s (reported, not vouched yet) -> refused",
           !r.p.editCues(static_cast<uint64_t>(gen), 2, {}, { ec(32000, 33000, "x", "y") }));
        ok("WG editor (analysing): a cue in a stretch NOT analysed yet -> refused (the helper will write there)",
           !r.p.editCues(static_cast<uint64_t>(gen), 2, {}, { ec(40000, 41000, "x", "y") })
           && a(r.h.lastOf("translatorFileEdited")).find("not analysed yet") != std::string::npos);
        ok("WG editor (analysing): ...nor across the edge of the analysed stretch",
           !r.p.editCues(static_cast<uint64_t>(gen), 3, {}, { ec(29000, 31000, "x", "y") }));
        r.p.onCue(lc(job, 2, 5000, 6000, "fr", "en", "Cinq.", "Five."));    // the analysis goes on (2 cues + the edited one)
        r.p.editCues(static_cast<uint64_t>(gen), 4, { 0 }, {});      // a DELETION
        r.p.onCue(lc(job, 3, 31000, 32000, "fr", "en", "Trois.", "Three."));
        bool unique = true;
        std::set<uint32_t> seen;
        for (const auto& c : S.cues) { if (seen.count(c.idx)) unique = false; seen.insert(c.idx); }
        ok("WG editor (analysing): after an edit and a deletion the analysis' cues get FRESH idx (it used to be the size: a collision)",
           unique && S.cues.size() == 3, n2s(S.cues.size()));
    }
    {
        // WHAT WAITS IS WRITTEN AT THE END: the Windows session ending (persist, WM_QUERYENDSESSION) and the tray's exit
        // (shutdown) write the per-language files younger than 30 s and an edit younger than 2 s -- nothing is lost
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Un.", "One."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.h.now += 1000; r.p.tick();
        ok("WG end: (1 s after the first cue, nothing written yet)", r.h.srtWrites == 0, n2s(r.h.srtWrites));
        r.p.persist();
        ok("WG end: the Windows session ENDING (persist) writes the files that waited",
           r.h.srtWrites == 2 && r.h.srtDisk[fFr] == "1000-2000:Un.\n" && r.h.srtDisk[fEn] == "1000-2000:One.\n", n2s(r.h.srtWrites));
        r.p.onCue(lc(job, 1, 3000, 4000, "fr", "en", "Deux.", "Two."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-40000"));       // vouches for 0-30000
        ok("WG end: an edit (inside what is analysed and vouched for)",
           r.p.editCues(static_cast<uint64_t>(gen), 1, { 0 }, { ec(1000, 2000, "Un !", "One!") }));
        const int cw = r.h.cacheWrites;
        r.h.now += 500;
        r.p.shutdown();
        pbtrf::Record back;
        const std::string key = pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto");
        ok("WG end: the tray's EXIT (shutdown) writes the files with the new cue and the edit, 0.5 s after it",
           r.h.srtDisk[fEn] == "1000-2000:One!\n3000-4000:Two.\n", r.h.srtDisk[fEn]);
        ok("WG end: ...and the record, EDITED, holding the edit",
           r.h.cacheWrites > cw && r.h.readCache(key, back) && back.edited && !back.cues.empty() && back.cues[0].src == "Un !",
           n2s(r.h.cacheWrites - cw));
    }

    // ---- 15. ENGINE: a result from an OLDER cue engine is analysed again; an EDITED one is kept, and the window asks --
    std::printf("[15] ENGINE: the cue engine's version (TrPipe.h kFileEngineVersion = %d)\n", tp::kFileEngineVersion);
    const int kCur = tp::kFileEngineVersion;
    auto engRecord = [&](const std::wstring& media, const char* la, const char* lb, int engine, bool edited, bool complete) {
        pbtrf::Record rec;
        rec.path = pbtrf::toUtf8(media); rec.size = 7000; rec.mtime = 13;
        rec.langA = la; rec.langB = lb; rec.mode = "auto"; rec.quality = "auto"; rec.totalMs = 600000;
        rec.complete = complete; rec.edited = edited; rec.engine = engine;
        rec.ranges.push_back(tp::MsRange{ 0, complete ? 600000ULL : 300000ULL });
        rec.cues.push_back(lc(1, 0, 1000, 2000, la, lb, "Vieux un.", "Old one."));
        rec.cues.push_back(lc(1, 1, 3000, 4000, la, lb, "Vieux deux.", "Old two."));
        return pbtrf::packRecord(rec);
    };
    auto withoutEngineLine = [](std::string t) {         // exactly what 1.0.6 (and WG before ENGINE) wrote
        const size_t at = t.find("\nENGINE\t");
        if (at != std::string::npos) t.erase(at + 1, t.find('\n', at + 1) - at);
        return t;
    };
    const std::string keyFrEn = pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto");
    {
        // the user's film: COMPLETE, from 1.0.6 (no ENGINE line), never edited
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = withoutEngineLine(engRecord(Fm, "fr", "en", 0, false, true));
        r.p.open({ Fm }, false);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("ENGINE: a COMPLETE record of an OLDER engine (the user's film: 1.0.6, no ENGINE line) is a MISS -- analysed again from nothing (FILE have empty)",
           r.h.cache[keyFrEn].find("ENGINE") == std::string::npos && r.h.files.size() == 1 && r.h.files[0].have.empty()
           && fn(st, L"count") == 0 && !fb(st, L"cached"), a(st).substr(0, 300));
        ok("ENGINE: ...the state says WHY (upgrade: \"Improved subtitles -- re-analysing\"); no older engine's cue on screen",
           fb(st, L"upgrade") && !fb(st, L"oldEngine"));
        const uint64_t job = r.h.files[0].jobId;
        r.p.onCue(lc(job, 0, 1000, 1900, "fr", "en", "Nouveau un.", "New one."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.h.now += 31000; r.p.tick();
        ok("ENGINE: ...as it goes, the per-language files are rewritten from the NEW cues (no old one mixed in)",
           r.h.srtDisk[fFr] == "1000-1900:Nouveau un.\n" && r.h.srtDisk[fEn] == "1000-1900:New one.\n", r.h.srtDisk[fFr]);
        r.p.onCue(lc(job, 1, 3000, 4000, "fr", "en", "Nouveau deux.", "New two."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(job, "ok"), false);
        pbtrf::Record back;
        ok("ENGINE: ...done: the record rewritten under THIS engine (ENGINE line), with the new cues",
           r.h.readCache(keyFrEn, back) && back.engine == kCur && back.complete && back.cues.size() == 2 && back.cues[0].src == "Nouveau un."
           && r.h.cache[keyFrEn].find("\nENGINE\t" + std::to_string(kCur) + "\n") != std::string::npos);
        Rig r2;
        r2.p.setPair("fr", "en", "auto");
        r2.h.addFile(Fm, 7000, 13);
        r2.h.cache = r.h.cache;
        r2.p.open({ Fm }, false);
        ok("ENGINE: ...reopened: a HIT now (no job, \"analysed earlier\", no upgrade)", r2.h.files.empty()
           && fb(r2.h.lastOf("translatorFile"), L"cached") && !fb(r2.h.lastOf("translatorFile"), L"upgrade"));
    }
    for (int eng : { kCur - 1, kCur, kCur + 1 }) {
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", eng, false, true);
        r.p.open({ Fm }, false);
        const std::wstring st = r.h.lastOf("translatorFile");
        const bool hit = r.h.files.empty() && fn(st, L"count") == 2 && fb(st, L"cached") && !fb(st, L"upgrade") && !fb(st, L"oldEngine");
        if (eng < kCur) ok("ENGINE: engine N-1 (the one just before) is a miss too", !hit && r.h.files.size() == 1 && fb(st, L"upgrade"));
        else ok(eng == kCur ? "ENGINE: a record of THIS engine is a hit (no job, its cues)"
                            : "ENGINE: a NEWER engine's record (a downgraded tray) is a hit as well", hit, a(st).substr(0, 200));
    }
    {
        // EDITED by hand, from an older engine: never discarded silently
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur - 1, true, true);
        r.p.open({ Fm }, false);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("ENGINE: an EDITED record of an older engine LOADS (no job): the state says edited + oldEngine (the window asks)",
           r.h.files.empty() && fn(st, L"count") == 2 && fb(st, L"edited") && fb(st, L"oldEngine") && !fb(st, L"upgrade"), a(st).substr(0, 300));
        const long long gen = fn(st, L"jobId");
        ok("ENGINE: ...he edits it some more", r.p.editCues(static_cast<uint64_t>(gen), 1, { 0 }, { ec(1000, 2000, "Vieux un, corrige.", "Old one, fixed.") }));
        r.h.now += 2500; r.p.tick();
        pbtrf::Record back;
        ok("ENGINE: ...saved, STILL the older engine's (never stamped as this one -- the question stays until he answers)",
           r.h.readCache(keyFrEn, back) && back.edited && back.engine == kCur - 1 && !back.cues.empty() && back.cues[0].src == "Vieux un, corrige.",
           r.h.cache[keyFrEn].substr(0, 260));
        r.p.analyse(true);                                   // his answer: "Re-analyse"
        const std::wstring st2 = r.h.lastOf("translatorFile");
        ok("ENGINE: ...his \"Re-analyse\": analysed from nothing, said as an upgrade, the edits gone with the old analysis",
           r.h.files.size() == 1 && r.h.files[0].have.empty() && fb(st2, L"upgrade") && !fb(st2, L"edited") && !fb(st2, L"oldEngine"),
           a(st2).substr(0, 300));
    }
    {
        // PARTIAL: an older engine's half is not resumed (two engines in one set); this engine's is
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur - 1, false, false);
        r.p.open({ Fm }, false);
        ok("ENGINE: a PARTIAL record of an older engine is not resumed: analysed from nothing (FILE have empty), as an upgrade",
           r.h.files.size() == 1 && r.h.files[0].have.empty() && fb(r.h.lastOf("translatorFile"), L"upgrade"));
        Rig r2;
        r2.p.setPair("fr", "en", "auto");
        r2.h.addFile(Fm, 7000, 13);
        r2.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, false);
        r2.p.open({ Fm }, false);
        ok("ENGINE: ...the same partial of THIS engine RESUMES (FILE have = its ranges)",
           r2.h.files.size() == 1 && r2.h.files[0].have == "0-300000" && !fb(r2.h.lastOf("translatorFile"), L"upgrade"),
           r2.h.files.empty() ? std::string("no FILE") : r2.h.files[0].have);
    }
    for (bool editedSib : { false, true }) {
        // the SHARED ORIGINAL: a complete fr<->zh sibling of an older engine, and a half fr<->de of THIS engine (more
        // than the fr<->en being analysed, less than the complete one)
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[pbtrf::cacheKey(Fm, 7000, 13, "fr", "zh", "auto", "auto")] = engRecord(Fm, "fr", "zh", kCur - 1, editedSib, true);
        {
            pbtrf::Record de;
            de.path = pbtrf::toUtf8(Fm); de.size = 7000; de.mtime = 13;
            de.langA = "fr"; de.langB = "de"; de.mode = "auto"; de.quality = "auto"; de.totalMs = 600000;
            de.complete = false; de.engine = kCur;
            de.ranges.push_back(tp::MsRange{ 0, 300000 });
            de.cues.push_back(lc(1, 0, 1000, 2000, "fr", "de", "Bonjour (de).", "Hallo."));
            r.h.cache[pbtrf::cacheKey(Fm, 7000, 13, "fr", "de", "auto", "auto")] = pbtrf::packRecord(de);
        }
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Bonjour a tous.", "Hello everyone."));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.h.now += 31000; r.p.tick();
        if (!editedSib)
            ok("ENGINE: the shared original never comes from an older engine's analysis -- the complete fr<->zh of engine N-1 is passed "
               "over for THIS engine's half fr<->de (more than fr<->en so far)",
               r.h.srtDisk[fFr] == "1000-2000:Bonjour (de).\n", r.h.srtDisk[fFr]);
        else
            ok("ENGINE: ...but an EDITED one is the user's own text: it still gives the shared French file",
               r.h.srtDisk[fFr] == "1000-2000:Vieux un.\n3000-4000:Vieux deux.\n", r.h.srtDisk[fFr]);
    }
    {
        // the QUEUE: the next file's older record is not "ready" -- it is pre-analysed from nothing
        const std::wstring Fm2 = L"D:\\Films\\Feature 2.mkv";
        for (int eng : { kCur - 1, kCur }) {
            Rig r;
            r.p.setPair("fr", "en", "auto");
            r.h.addFile(Fm, 7000, 13);
            r.h.addFile(Fm2, 7000, 13);
            r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, true);
            r.h.cache[pbtrf::cacheKey(Fm2, 7000, 13, "fr", "en", "auto", "auto")] = engRecord(Fm2, "fr", "en", eng, false, true);
            r.p.open({ Fm, Fm2 }, false);
            const std::string q = a(r.h.lastOf("playerQueue"));
            if (eng < kCur)
                ok("ENGINE: the next file's OLDER record is not a \"ready\" badge: it is pre-analysed from nothing",
                   r.h.files.size() == 1 && r.h.files[0].path == a(Fm2) && r.h.files[0].have.empty()
                   && fs(r.h.lastOf("playerItem"), L"state") != "ready", q.substr(0, 400));
            else
                ok("ENGINE: ...this engine's record: \"ready\", nothing to pre-analyse",
                   r.h.files.empty() && q.find("\"state\":\"ready\",\"pct\":100") != std::string::npos, q.substr(0, 400));
        }
    }

    // ---- 16. the review of WG (11/10) ----------------------------------------------------------------------------------
    std::printf("[16] review of WG: #4 the user's edits keep the shared file, #15 analyse() saves a pending edit\n");
    auto rec2 = [&](const char* la, const char* lb, bool complete, bool edited, std::vector<tp::WireCue> cues) {
        pbtrf::Record x;
        x.path = pbtrf::toUtf8(Fm); x.size = 7000; x.mtime = 13;
        x.langA = la; x.langB = lb; x.mode = "auto"; x.quality = "auto"; x.totalMs = 600000;
        x.complete = complete; x.edited = edited; x.engine = kCur;
        x.ranges.push_back(tp::MsRange{ 0, complete ? 600000ULL : 300000ULL });
        x.cues = std::move(cues);
        return pbtrf::packRecord(x);
    };
    for (bool enComplete : { true, false }) {
        Rig r;
        r.h.addFile(Fm, 7000, 13);
        // fr<->en, EDITED by the user (his French fix), complete -- or only half analysed
        r.h.cache[keyFrEn] = rec2("fr", "en", enComplete, true, { lc(1, 0, 1000, 2000, "fr", "en", "Bonjour CORRIGE.", "Hello, fixed."),
                                                                   lc(1, 1, 3000, 4000, "fr", "en", "Deux.", "Two.") });
        r.p.setPair("fr", "zh", "auto");
        r.p.open({ Fm }, false);
        const uint64_t job = r.h.files.back().jobId;
        r.p.onCue(lc(job, 0, 1000, 2000, "fr", "zh", "Bonjour faute.", "ZH1"));
        r.p.onCue(lc(job, 1, 3000, 4000, "fr", "zh", "Deux.", "ZH2"));
        r.p.onCue(lc(job, 2, 400000, 401000, "fr", "zh", "Loin.", "ZH3"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(job, "ok"), false);
        if (enComplete)
            ok("#4: another pair (fr<->zh) finishing UNEDITED never reverts the user's French fix: the French file comes "
               "from his EDITED fr<->en (both complete)", r.h.srtDisk[fFr] == "1000-2000:Bonjour CORRIGE.\n3000-4000:Deux.\n", r.h.srtDisk[fFr]);
        else
            ok("#4: ...but a HALF-analysed edited set never shrinks a complete file: complete first (fr<->zh's French)",
               r.h.srtDisk[fFr] == "1000-2000:Bonjour faute.\n3000-4000:Deux.\n400000-401000:Loin.\n", r.h.srtDisk[fFr]);
        ok(enComplete ? "#4: ...the Chinese file is fr<->zh's own" : "#4: ...(the Chinese file fr<->zh's own)",
           r.h.srtDisk[fZh] == "1000-2000:ZH1\n3000-4000:ZH2\n400000-401000:ZH3\n", r.h.srtDisk[fZh]);
    }
    {
        // #15: "Languages changed -> Re-analyse" less than 2 s after an edit: the edit is written before the set goes
        Rig r;
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = rec2("fr", "en", true, false, { lc(1, 0, 1000, 2000, "fr", "en", "Un.", "One.") });
        r.p.setPair("fr", "en", "auto");
        r.p.open({ Fm }, false);
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        ok("#15: (an edit)", r.p.editCues(static_cast<uint64_t>(gen), 1, { 0 }, { ec(1000, 2000, "Un, corrige.", "One, fixed.") }));
        r.h.now += 500;
        r.p.setPair("fr", "zh", "auto");
        r.p.analyse(false);
        pbtrf::Record back;
        ok("#15: analyse() 0.5 s after it: the fr<->en record already holds the edit (EDIT 1) -- not lost with the set",
           r.h.readCache(keyFrEn, back) && back.edited && !back.cues.empty() && back.cues[0].src == "Un, corrige.",
           r.h.cache[keyFrEn].substr(0, 200));
    }

    // ---- 17. WH: THE FILM'S OWN TRACKS -- the sound track heard and analysed, embedded subtitles ----------------------
    std::printf("[17] WH: the sound track (D3 D5 D6), the probe, embedded subtitles (D7)\n");
    {
        using pbmedia::AudioTrack;
        using pbmedia::SubTrack;
        auto au = [](uint32_t ord, uint32_t track, const char* lang, const char* codec, bool def, bool play, const char* name = "") {
            AudioTrack a; a.ord = ord; a.track = track; a.lang = lang; a.codec = codec; a.def = def; a.play = play; a.name = name; a.ch = 2;
            return a;
        };
        auto sb = [](uint32_t track, const char* lang, const char* codec, bool image, bool okk, bool forced = false) {
            SubTrack s; s.track = track; s.lang = lang; s.codec = codec; s.image = image; s.ok = okk; s.forced = forced;
            if (image) s.why = pbmedia::errImage();
            return s;
        };
        auto mkInfo = [](std::vector<AudioTrack> audio, std::vector<SubTrack> subs, bool video = true) {
            auto i = std::make_shared<pbmedia::Info>();
            i->state = pbmedia::Info::State::Ready;
            i->container = "mkv";
            i->hasVideo = video;
            if (video) { i->video.codec = "H.264"; i->video.w = 640; i->video.h = 360; }
            i->audio = audio;
            i->subs = subs;
            return i;
        };
        // mkv_2a_def2 as the plan lists it: #2 eng (FlagDefault 0), #3 chi (default); subs #4 SRT, #5 ASS forced, #6 VTT
        // (+ a VobSub #12: pictures)
        const auto def2 = mkInfo({ au(0, 2, "eng", "AAC", false, true, "English speech"), au(1, 3, "chi", "AAC", true, true, "Chinese speech") },
                                 { sb(4, "eng", "SRT", false, true), sb(5, "fre", "ASS", false, true, true), sb(6, "ger", "WebVTT", false, true),
                                   sb(12, "kor", "VobSub", true, false) });
        const auto dtsFirst = mkInfo({ au(0, 2, "eng", "DTS", true, false), au(1, 3, "chi", "AAC", false, true) }, {});
        const auto single = mkInfo({ au(0, 1, "eng", "AAC", true, true) }, {});
        // D3, pure
        pbplayer::AudioPick pk0; pk0.ord = 0; pk0.trackId = 2;
        pbplayer::AudioPick pkBad; pkBad.ord = 0; pkBad.trackId = 99;
        pbplayer::AudioPick pkFar; pkFar.ord = 7; pkFar.trackId = 0;
        ok("D3: the default flag on track 2 -> ord 1 (Chromium would play ord 0: F3)", pbplayer::initialAudioOrd(*def2, nullptr) == 1);
        ok("D3: an UNPLAYABLE default first track (DTS) -> the first playable, ord 1", pbplayer::initialAudioOrd(*dtsFirst, nullptr) == 1);
        ok("D3: nothing playable -> ord 0; no sound track -> -1",
           pbplayer::initialAudioOrd(*mkInfo({ au(0, 2, "eng", "DTS", false, false), au(1, 3, "eng", "TrueHD", true, false) }, {}), nullptr) == 0
           && pbplayer::initialAudioOrd(*mkInfo({}, {}), nullptr) == -1);
        ok("D3: the user's choice first (ord 0 over the default); a choice whose track id no longer matches, or out of range, is ignored",
           pbplayer::initialAudioOrd(*def2, &pk0) == 0 && pbplayer::initialAudioOrd(*def2, &pkBad) == 1 && pbplayer::initialAudioOrd(*def2, &pkFar) == 1);

        // D6: the cache key of ord 0 is the key every record before WH has; ord 1 another one
        {
            const std::string old = pbtrf::hex16(pbtrf::fnv1a64(pbtrf::toUtf8(pbtrf::lowerW(A)) + "\t5000\t11\ten\tfr\tauto\tauto"));
            ok("D6: cacheKey(ord 0) == the pre-WH key, byte for byte; ord 1 is different (\"\\ta1\" appended)",
               pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto") == old && pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 0) == old
               && pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 1) != old
               && pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 1)
                  == pbtrf::hex16(pbtrf::fnv1a64(pbtrf::toUtf8(pbtrf::lowerW(A)) + "\t5000\t11\ten\tfr\tauto\tauto\ta1")));
        }
        const std::string key0 = pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 0), key1 = pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 1);
        auto tracksOf = [](const std::wstring& st) {
            const size_t a0 = st.find(L"\"tracks\":"), a1 = st.find(L",\"audioOrd\":");
            return (a0 == std::wstring::npos || a1 == std::wstring::npos) ? std::string() : pbtrf::toUtf8(st.substr(a0, a1 - a0));
        };
        {
            // the default rule at the opening: FILE on track 1, ONE job (it waited for the probe), its own key
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            ok("open: the probe was asked for (a .mkv), and ONE FILE went out -- on ord 1 (\"1:3\"), the default track",
               r.h.probes == 1 && r.h.files.size() == 1 && r.h.files[0].audio == "1:3", r.h.files.empty() ? "no FILE" : r.h.files[0].audio);
            const std::wstring st = r.h.lastOf("translatorFile");
            const std::string tr = tracksOf(st);
            ok("translatorFile: audioOrd 1, tracks ready (mkv, H.264 640x360), both sound tracks with ord/track/name/lang/codec/ch/def/play",
               fn(st, L"audioOrd") == 1 && tr.find("\"state\":\"ready\"") != std::string::npos && tr.find("\"container\":\"mkv\"") != std::string::npos
               && tr.find("\"video\":{\"codec\":\"H.264\",\"w\":640,\"h\":360}") != std::string::npos
               && tr.find("{\"ord\":0,\"track\":2,\"name\":\"English speech\",\"lang\":\"eng\",\"codec\":\"AAC\",\"ch\":2,\"def\":false,\"play\":true}") != std::string::npos
               && tr.find("{\"ord\":1,\"track\":3,\"name\":\"Chinese speech\",\"lang\":\"chi\",\"codec\":\"AAC\",\"ch\":2,\"def\":true,\"play\":true}") != std::string::npos,
               tr);
            ok("...and the subtitle tracks: text ones readable, the forced flag, the VobSub as an IMAGE not ok",
               tr.find("\"track\":5,\"name\":\"\",\"lang\":\"fre\",\"codec\":\"ASS\",\"kind\":\"text\",\"def\":false,\"forced\":true,\"ok\":true") != std::string::npos
               && tr.find("\"track\":12,\"name\":\"\",\"lang\":\"kor\",\"codec\":\"VobSub\",\"kind\":\"image\",\"def\":false,\"forced\":false,\"ok\":false") != std::string::npos,
               tr);
            const uint64_t job = r.h.files[0].jobId;
            r.p.onCue(cue(job, 0, 1000, 2000, "Ni hao", "Bonjour"));
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            ok("D6: track 1's result is cached under ITS key (\"\\ta1\"), with an AUD line; nothing under the ord-0 key",
               r.h.cache.count(key1) && r.h.cache[key1].find("\nAUD\t1\t3\n") != std::string::npos && !r.h.cache.count(key0));
            pbtrf::Record rec;
            ok("...and reads back with its track (audioOrd 1, track 3)", r.h.readCache(key1, rec) && rec.audioOrd == 1 && rec.audioTrack == 3);
            ok("every post was well-formed JSON (the tracks fragment spliced in)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // the first track: FILE `audio` "" and no AUD line -- the wire and the record exactly as before WH
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = single;
            r.p.open({ A }, false);
            ok("a one-track file: FILE audio \"\" (ord 0 -- what every FILE before WH said)", r.h.files.size() == 1 && r.h.files[0].audio.empty());
            const uint64_t job = r.h.files[0].jobId;
            r.p.onCue(cue(job, 0, 1000, 2000));
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            ok("...its record under the PRE-WH key -- now WITH its track marker (\"AUD 0 1\": every record this tray writes has one)",
               r.h.cache.count(key0) && r.h.cache[key0].find("\nAUD\t0\t1\n") != std::string::npos, r.h.cache.count(key0) ? r.h.cache[key0].substr(0, 300) : "");
            // a not-a-container file
            Rig w;
            const std::wstring W = L"D:\\Films\\talk.wav";
            w.h.addFile(W, 100, 1);
            w.p.open({ W }, false);
            const std::wstring st = w.h.lastOf("translatorFile");
            ok("a .wav: no probe at all, the FILE at once (audio \"\"), tracks.state none, audioOrd -1, video null",
               w.h.probes == 0 && w.h.files.size() == 1 && w.h.files[0].audio.empty() && fn(st, L"audioOrd") == -1
               && tracksOf(st).find("\"tracks\":{\"video\":null,\"state\":\"none\"") != std::string::npos, tracksOf(st));
        }
        {
            // the job WAITS for the probe; the probe answers late -> the job on the right track; the wait is bounded
            Rig r;
            r.h.autoProbe = false;
            r.h.addFile(A, 5000, 11);
            r.p.open({ A }, false);
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("a probe not answered yet: NO FILE (the track is not known), tracks.state probing, audioOrd -1",
               r.h.files.empty() && r.h.probeReqs.size() == 1 && tracksOf(st).find("\"state\":\"probing\"") != std::string::npos
               && fn(st, L"audioOrd") == -1, tracksOf(st));
            r.h.now += 1000;
            r.p.tick();
            ok("...1 s later: still waiting", r.h.files.empty());
            r.p.onTracks(r.h.probeReqs[0].first, def2);
            ok("...the answer: ONE FILE, on ord 1, a new generation for the page (the cues it was shown were track 0's)",
               r.h.files.size() == 1 && r.h.files[0].audio == "1:3" && fn(r.h.lastOf("translatorFile"), L"audioOrd") == 1);
            Rig t;
            t.h.autoProbe = false;
            t.h.addFile(A, 5000, 11);
            t.p.open({ A }, false);
            const long long gen0 = fn(t.h.lastOf("translatorFile"), L"jobId");
            t.h.now += 3000;
            t.p.tick();
            ok("a probe that never answers: after 3 s the job goes on the provisional track (ord 0, audio \"\")",
               t.h.files.size() == 1 && t.h.files[0].audio.empty());
            t.p.onTracks(t.h.probeReqs[0].first, def2);
            ok("...its LATE answer re-tracks: FCANCEL of that job, FILE on ord 1, a new generation",
               t.h.files.size() == 2 && t.h.files[1].audio == "1:3" && !t.h.cancels.empty() && t.h.cancels.back() == t.h.files[0].jobId
               && fn(t.h.lastOf("translatorFile"), L"jobId") > gen0);
            t.p.onTracks(t.h.probeReqs[0].first, def2);
            ok("...a second answer for the same request: ignored", t.h.files.size() == 2);
        }
        {
            // D5: the user's choice -- with translation ON a new generation on that track; persisted; reopened
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            const uint64_t job1 = r.h.files.back().jobId;
            const long long gen1 = fn(r.h.lastOf("translatorFile"), L"jobId");
            r.p.setAudio(0, false);
            ok("D5 translation ON: playerSet{audioOrd:0} -> FCANCEL of track 1's job, FILE on track 0 (audio \"\"), a NEW generation",
               r.h.files.size() == 2 && r.h.files[1].audio.empty() && !r.h.cancels.empty() && r.h.cancels.back() == job1
               && fn(r.h.lastOf("translatorFile"), L"jobId") > gen1 && fn(r.h.lastOf("translatorFile"), L"audioOrd") == 0);
            ok("...not written yet (2 s after the last change)", r.h.audioSaves == 0);
            r.h.now += 2000;
            r.p.tick();
            const std::string mk = pbtrf::mediaKey(A, 5000, 11);
            ok("...then persisted: player-audio.txt holds this file's choice (mediaKey, ord 0, track 2)",
               r.h.audioSaves == 1 && r.h.audioBytes.find("AUD\t" + mk + "\t0\t2\t") != std::string::npos, r.h.audioBytes);
            Rig r2;
            r2.h.addFile(A, 5000, 11);
            r2.h.probeMap[pbtrf::lowerW(A)] = def2;
            r2.p.loadAudio(r.h.audioBytes);
            r2.p.load("", "");
            r2.p.open({ A }, false);
            ok("reopened in another tray run: the USER'S track (ord 0) beats the default flag; one FILE, audio \"\"",
               r2.h.files.size() == 1 && r2.h.files[0].audio.empty() && fn(r2.h.lastOf("translatorFile"), L"audioOrd") == 0);
            // the same choice again: nothing moves
            const size_t files0 = r2.h.files.size();
            r2.p.setAudio(0, false);
            ok("choosing the track already heard: no new job", r2.h.files.size() == files0);
            // a REMEMBERED pick of track 1 -- but this time the file cannot be read (probe error): the page can enforce
            // nothing (no track list), the WebView's default plays -> the analysis is the first track's, not the pick's
            pbplayer::AudioPick p1; p1.ord = 1; p1.trackId = 3; p1.epoch = 5;
            std::map<std::string, pbplayer::AudioPick> m1;
            m1[mk] = p1;
            Rig r3;
            r3.h.autoProbe = false;
            r3.h.addFile(A, 5000, 11);
            r3.p.loadAudio(pbplayer::packAudioPicks(m1));
            r3.p.open({ A }, false);
            auto broken = std::make_shared<pbmedia::Info>();
            broken->state = pbmedia::Info::State::Error;
            r3.p.onTracks(r3.h.probeReqs[0].first, broken);
            ok("a remembered pick (ord 1) but the probe FAILS: the job on the FIRST track (audio \"\"), audioOrd -1, tracks error",
               r3.h.files.size() == 1 && r3.h.files[0].audio.empty() && fn(r3.h.lastOf("translatorFile"), L"audioOrd") == -1
               && tracksOf(r3.h.lastOf("translatorFile")).find("\"state\":\"error\"") != std::string::npos,
               r3.h.files.empty() ? "no FILE" : r3.h.files[0].audio);
        }
        {
            // auto: the page could not play the tray's choice -- the analysis follows what is heard, never persisted
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            r.p.setAudio(0, true);
            ok("auto: the analysis follows what is HEARD (FILE on ord 0)", r.h.files.size() == 2 && r.h.files[1].audio.empty());
            r.h.now += 5000;
            r.p.tick();
            r.p.shutdown();
            ok("...but an auto choice is NEVER persisted (no player-audio.txt write, even at the end)", r.h.audioSaves == 0 && r.p.audioPicks().empty());
            r.p.setAudio(5, false);
            r.p.setAudio(-1, false);
            ok("an out-of-range ord: ignored", r.h.files.size() == 2 && r.p.audioPicks().empty());
        }
        {
            // translation OFF: only what is heard changes; turned back ON: the analysis goes to that track
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            r.p.setTranslate(false, 0);
            const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
            const size_t files = r.h.files.size();
            r.p.setAudio(0, false);
            ok("translation OFF: a switch is NO new generation and no job -- only audioOrd moves",
               fn(r.h.lastOf("translatorFile"), L"jobId") == gen && r.h.files.size() == files && fn(r.h.lastOf("translatorFile"), L"audioOrd") == 0);
            r.p.setTranslate(true, 5000);
            ok("...translation back ON: a new generation on the track HEARD (ord 0), FILE audio \"\", focus at the playhead",
               fn(r.h.lastOf("translatorFile"), L"jobId") > gen && r.h.files.size() == files + 1 && r.h.files.back().audio.empty()
               && r.h.files.back().focusMs == 5000);
        }
        {
            // the new track's OWN record: resumed from what it has (`have`)
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            pbtrf::Record part;
            part.path = a(A); part.size = 5000; part.mtime = 11; part.langA = "en"; part.langB = "fr"; part.mode = "auto"; part.quality = "auto";
            part.complete = false; part.totalMs = 600000; part.engine = tp::kFileEngineVersion; part.audioOrd = 0;
            part.ranges.push_back(tp::MsRange{ 0, 30000 });
            part.cues.push_back(cue(1, 0, 1000, 2000, "Hi", "Salut"));
            r.h.cache[key0] = pbtrf::packRecord(part);
            r.p.open({ A }, false);
            r.p.setAudio(0, false);
            ok("D5: the track switched to has a partial record: its job RESUMES it (have 0-30000), its cue shown",
               r.h.files.size() == 2 && r.h.files[1].have == "0-30000" && r.h.lastOf("translatorFileCues").find(L"Salut") != std::wstring::npos);
            r.p.setAudio(1, false);
            ok("...and back to track 1 while its job's FDONE is still owed: no second FILE needed for nothing -- one FILE on ord 1",
               r.h.files.size() == 3 && r.h.files[2].audio == "1:3");
        }
        {
            // D6: the per-language file never shrinks because of a switch; it follows the new track once that one is as complete
            Rig r;
            r.p.setPair("fr", "en", "auto");
            r.h.addFile(Fm, 7000, 13);
            r.h.probeMap[pbtrf::lowerW(Fm)] = mkInfo({ au(0, 2, "fre", "AAC", true, true), au(1, 3, "eng", "AAC", false, true) }, {});
            r.p.open({ Fm }, false);
            uint64_t job = r.h.files.back().jobId;
            ok("(D6 setup: track 0 analysed)", r.h.files.back().audio.empty());
            r.p.onCue(lc(job, 0, 1000, 2000, "fr", "en", "Piste zero.", "Track zero."));
            r.p.onCue(lc(job, 1, 300000, 301000, "fr", "en", "Plus loin.", "Further."));
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            const std::string full0 = r.h.srtDisk[fFr];
            ok("track 0 complete: the French file holds its 2 lines, the registry knows it came from track 0's record",
               full0 == "1000-2000:Piste zero.\n300000-301000:Plus loin.\n"
               && r.h.srtFrom[fFr].first == pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto") && r.h.srtFrom[fFr].second == 0, full0);
            r.p.setAudio(1, false);
            job = r.h.files.back().jobId;
            ok("(switch to track 1: its job)", r.h.files.back().audio == "1:3");
            r.p.onCue(lc(job, 0, 1000, 2000, "en", "fr", "Track one.", "Piste un."));
            r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
            r.h.now += 31000;
            r.p.tick();
            ok("D6: track 1 half analysed -> the French file is NOT shrunk to its first minute (still track 0's, complete)",
               r.h.srtDisk[fFr] == full0, r.h.srtDisk[fFr]);
            r.p.onCue(lc(job, 1, 400000, 401000, "en", "fr", "Track one far.", "Piste un loin."));
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            ok("D6: track 1 complete (a tie -> the set worked on): the French file follows track 1 now, the registry says so",
               r.h.srtDisk[fFr] == "1000-2000:Piste un.\n400000-401000:Piste un loin.\n"
               && r.h.srtFrom[fFr].first == pbtrf::cacheKey(Fm, 7000, 13, "fr", "en", "auto", "auto", 1) && r.h.srtFrom[fFr].second == 1,
               r.h.srtDisk[fFr]);
            ok("D6: the sibling search stays bounded (144 same-track keys + 1 writer per language per write)", r.h.writerAsks > 0);
        }
        {
            // embedded subtitles: ids, extraction on request, the LRU, cancellation, refusals
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.sidecarMap[pbtrf::lowerW(L"D:\\Films\\Season 1|Ep 1.mkv")] = { pbplayer::Sidecar{ 0, L"Ep 1.fr.srt", L"fr", L"D:\\Films\\Season 1\\Ep 1.fr.srt" } };
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            const std::string tr = tracksOf(r.h.lastOf("translatorFile"));
            std::vector<long long> ids;
            for (size_t at = 0; (at = tr.find("{\"id\":", at)) != std::string::npos; at += 6) ids.push_back(std::atoll(tr.c_str() + at + 6));
            const long long side = fn(r.h.lastOf("translatorFile"), L"id");       // (the first "id" = the sidecar's)
            std::set<long long> uniq(ids.begin(), ids.end());
            uniq.insert(side);
            ok("D7: 4 embedded ids, from the SAME counter as the sidecar's: all different", ids.size() == 4 && uniq.size() == 5 && side > 0, tr);
            ok("isEmbeddedSub: an embedded id yes, the sidecar's no", r.p.isEmbeddedSub(static_cast<uint64_t>(ids[0])) && !r.p.isEmbeddedSub(static_cast<uint64_t>(side)));
            ok("playerSubGet of a sidecar id: not the core's (the bridge reads the file)", !r.p.subGet(static_cast<uint64_t>(side)));
            r.h.clearPosts();
            ok("playerSubGet of an embedded text track: an extraction asked for (#4), on a worker", r.p.subGet(static_cast<uint64_t>(ids[0]))
               && r.h.extracts.size() == 1 && r.h.extracts[0].track == 4 && r.h.extracts[0].path == A);
            r.p.subGet(static_cast<uint64_t>(ids[1]));
            ok("a second request before the first is done: the first CANCELLED, #5 asked", r.h.extracts.size() == 2 && r.h.extracts[1].track == 5
               && r.h.extractCancels.size() == 1 && r.h.extractCancels[0] == r.h.extracts[0].req);
            auto res = [](const char* text, uint64_t s, uint64_t e) {
                auto x = std::make_shared<pbmedia::SubResult>();
                x->ok = true;
                pbsubr::Cue c; c.startMs = s; c.endMs = e;
                pbsubr::Run run; run.text = text; run.flags = pbsubr::kItalic; c.runs.push_back(run);
                x->parsed.ok = true; x->parsed.cues.push_back(c);
                return std::shared_ptr<const pbmedia::SubResult>(x);
            };
            r.p.onSubExtracted(r.h.extracts[0].req, res("SRT line", 1000, 3000));
            ok("the cancelled one's answer: nothing posted", r.h.countOf("playerSubs") == 0);
            r.p.onSubExtracted(r.h.extracts[1].req, res("ASS line", 4000, 6000));
            const std::wstring ps = r.h.lastOf("playerSubs");
            ok("the current one's answer: playerSubs{id, embedded:true, track:5, encName UTF-8, cues with runs} + extSubId",
               fn(ps, L"id") == ids[1] && fb(ps, L"embedded") && fn(ps, L"track") == 5 && fs(ps, L"encName") == "UTF-8"
               && a(ps).find("\"cues\":[{\"i\":0,\"s\":4000,\"e\":6000,\"r\":[[\"ASS line\",1]]}]") != std::string::npos
               && fn(r.h.lastOf("translatorFile"), L"extSubId") == ids[1], a(ps));
            r.h.clearPosts();
            const size_t ex0 = r.h.extracts.size();
            r.p.subGet(static_cast<uint64_t>(ids[0]));
            ok("#4 asked again: the cancelled extraction's result was KEPT (LRU) -- posted at once, no new extraction",
               r.h.extracts.size() == ex0 && r.h.countOf("playerSubs") == 1 && a(r.h.lastOf("playerSubs")).find("SRT line") != std::string::npos);
            r.h.clearPosts();
            r.p.subGet(static_cast<uint64_t>(ids[3]));
            ok("an IMAGE track (VobSub): no extraction, its sentence through translatorFileNotice", r.h.extracts.size() == ex0
               && fs(r.h.lastOf("translatorFileNotice"), L"text") == pbmedia::errImage());
            r.p.subGet(static_cast<uint64_t>(ids[2]));
            auto fail = std::make_shared<pbmedia::SubResult>();
            fail->err = pbmedia::errCompressed();
            r.p.onSubExtracted(r.h.extracts.back().req, fail);
            ok("an extraction that fails: its sentence (the existing failure path), nothing else",
               fs(r.h.lastOf("translatorFileNotice"), L"text") == pbmedia::errCompressed() && r.h.countOf("playerSubs") == 0);
            r.p.subGet(static_cast<uint64_t>(ids[2]));
            const uint64_t pending = r.h.extracts.back().req;
            r.p.setExtSub(r.p.newSubId(), { L"{\"type\":\"playerSubs\",\"id\":1,\"cues\":[]}" });
            ok("a subtitle FILE loaded meanwhile: the pending extraction is cancelled", r.h.extractCancels.back() == pending);
            r.p.subGet(static_cast<uint64_t>(ids[2]));
            const uint64_t pending2 = r.h.extracts.back().req;
            r.h.addFile(B);
            r.p.add({ B });
            r.p.queueOp("play", r.p.items()[1].id, 0, 0);
            ok("another item played: the pending extraction is cancelled; the old ids are no longer embedded ids",
               r.h.extractCancels.back() == pending2 && !r.p.isEmbeddedSub(static_cast<uint64_t>(ids[0])));
            ok("every post was well-formed JSON", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // WHP-1: playerSubGet{id:0} = the viewer abandoned the pending request -- cancelled, and NOTHING said
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            const std::string tr = tracksOf(r.h.lastOf("translatorFile"));
            const uint64_t id4 = std::strtoull(tr.c_str() + tr.find("{\"id\":") + 6, nullptr, 10);
            const long long ext0 = fn(r.h.lastOf("translatorFile"), L"extSubId");
            r.p.subGet(id4);
            const uint64_t req = r.h.extracts.back().req;
            r.h.clearPosts();
            r.p.cancelSubRequest();
            ok("WHP-1: cancelled -> the worker is told (cancelExtract of that request)", !r.h.extractCancels.empty() && r.h.extractCancels.back() == req);
            auto fine = std::make_shared<pbmedia::SubResult>();
            fine->ok = true;
            pbsubr::Cue c; c.startMs = 1000; c.endMs = 2000; pbsubr::Run run; run.text = "late"; c.runs.push_back(run);
            fine->parsed.ok = true; fine->parsed.cues.push_back(c);
            r.p.onSubExtracted(req, fine);
            ok("WHP-1: ...its result arriving anyway: no playerSubs, extSubId untouched",
               r.h.countOf("playerSubs") == 0 && (r.h.lastOf("translatorFile").empty() || fn(r.h.lastOf("translatorFile"), L"extSubId") == ext0));
            r.p.subGet(id4 + 1);
            const uint64_t req2 = r.h.extracts.back().req;
            r.p.cancelSubRequest();
            auto bad = std::make_shared<pbmedia::SubResult>();
            bad->err = pbmedia::errDamaged();
            r.p.onSubExtracted(req2, bad);
            ok("WHP-1: ...a FAILURE of a cancelled request: no sentence either", r.h.countOf("translatorFileNotice") == 0);
            r.p.cancelSubRequest();
            ok("WHP-1: nothing pending: a cancel is harmless (nothing sent)", r.h.extractCancels.size() == 2);
        }
        // >>> new API (the fix round's proof strips this block to build the file over the code BEFORE the fix)
        {
            // SuVtitle FIX ROUND (2026-10-23, review RV4-1 = SPLIT_PLAN ADDENDUM 2): playerSubGet{auto:true} is the page's
            // AUTOMATIC default; it never replaces a subtitle the VIEWER named or loaded for the item. The bridge asks
            // acceptSubGet(id, auto) first and calls userSubFile() before every file he names (a drop, --play, Load
            // subtitles...) -- driven here exactly so, on the fake host.
            auto res = [](const char* text) {
                auto x = std::make_shared<pbmedia::SubResult>();
                x->ok = true;
                pbsubr::Cue c; c.startMs = 1000; c.endMs = 2000;
                pbsubr::Run run; run.text = text; c.runs.push_back(run);
                x->parsed.ok = true; x->parsed.cues.push_back(c);
                return std::shared_ptr<const pbmedia::SubResult>(x);
            };
            auto fileChunks = [](uint64_t id, const char* text) {
                return std::vector<std::wstring>{ L"{\"type\":\"playerSubs\",\"id\":" + std::to_wstring(id)
                    + L",\"part\":0,\"parts\":1,\"cues\":[{\"i\":0,\"s\":1000,\"e\":2000,\"r\":[[\"" + pbtrf::fromUtf8(text) + L"\",0]]}]}" };
            };
            auto embIds = [&](Rig& r) {
                const std::string tr = tracksOf(r.h.lastOf("translatorFile"));
                std::vector<uint64_t> ids;
                for (size_t at = 0; (at = tr.find("{\"id\":", at)) != std::string::npos; at += 6) ids.push_back(std::strtoull(tr.c_str() + at + 6, nullptr, 10));
                return ids;
            };
            {
                Rig r;
                r.h.addFile(A, 5000, 11);
                r.h.addFile(B);
                r.h.sidecarMap[pbtrf::lowerW(L"D:\\Films\\Season 1|Ep 1.mkv")] = { pbplayer::Sidecar{ 0, L"Ep 1.fr.srt", L"fr", L"D:\\Films\\Season 1\\Ep 1.fr.srt" } };
                r.h.probeMap[pbtrf::lowerW(A)] = def2;
                r.p.open({ A }, false);
                const std::vector<uint64_t> ids = embIds(r);
                const uint64_t side = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"id"));
                const size_t ex0 = r.h.extracts.size();
                ok("RV4-1: nothing chosen by the viewer: an AUTOMATIC request is honoured (its extraction asked)",
                   ids.size() == 4 && !r.p.userSubChosen() && r.p.acceptSubGet(ids[0], true) && r.p.subGet(ids[0]) && r.h.extracts.size() == ex0 + 1);
                // the viewer drops a subtitle file while that automatic extraction still runs
                const uint64_t autoReq = r.h.extracts.back().req;
                r.p.userSubFile();
                ok("RV4-1: a file the viewer names while an AUTOMATIC extraction runs: the item is his, that extraction cancelled",
                   r.p.userSubChosen() && !r.h.extractCancels.empty() && r.h.extractCancels.back() == autoReq);
                const uint64_t fileId = r.p.newSubId();
                r.p.setExtSub(fileId, fileChunks(fileId, "mine"));
                r.h.clearPosts();
                r.p.onSubExtracted(autoReq, res("automatic"));
                ok("RV4-1: ...the automatic result arriving late posts nothing: the viewer's file stays",
                   r.h.countOf("playerSubs") == 0 && r.p.session().extSubId == fileId);
                // the viewer's file applied, THEN the page's automatic default arrives (the RV4-1 order: open, file, answer)
                const size_t ex1 = r.h.extracts.size();
                ok("RV4-1: the viewer's file applied, then an AUTOMATIC request for an embedded track: IGNORED (no extraction, nothing posted)",
                   !r.p.acceptSubGet(ids[1], true) && r.h.extracts.size() == ex1 && r.h.countOf("playerSubs") == 0 && r.h.countOf("translatorFileNotice") == 0);
                ok("RV4-1: ...and an automatic request for the SIDECAR: ignored too (the bridge never reads it)", side > 0 && !r.p.acceptSubGet(side, true));
                ok("RV4-1: ...and an automatic 'abandon' (id 0) is ignored as well", !r.p.acceptSubGet(0, true));
                ok("RV4-1: the viewer's file is still the one shown", r.p.session().extSubId == fileId);
                // a request WITHOUT auto is the viewer's: always honoured
                ok("RV4-1: a request WITHOUT auto (the viewer's own pick) is honoured even then",
                   r.p.acceptSubGet(ids[1], false) && r.p.subGet(ids[1]) && r.h.extracts.size() == ex1 + 1);
                // another item: no mark
                r.p.add({ B });
                r.p.queueOp("play", r.p.items()[1].id, 0, 0);
                ok("RV4-1: another item played: no mark (an automatic request honoured again)",
                   r.p.items().size() == 2 && r.p.session().path == B && !r.p.userSubChosen() && r.p.acceptSubGet(1, true));
                ok("RV4-1: every post was well-formed JSON", r.h.badJson == 0, r.h.lastBadJson);
            }
            {
                // the viewer's FILE slow to come (named, not read yet) and the automatic request arriving FIRST
                Rig r;
                r.h.addFile(A, 5000, 11);
                r.h.probeMap[pbtrf::lowerW(A)] = def2;
                r.p.open({ A }, false);
                const std::vector<uint64_t> ids = embIds(r);
                r.p.userSubFile();                                   // a drop: the bridge marks, THEN reads the file
                const size_t ex0 = r.h.extracts.size();
                ok("RV4-1: the viewer's file named but not read yet, an AUTOMATIC request arrives first: ignored",
                   ids.size() == 4 && !r.p.acceptSubGet(ids[0], true) && r.h.extracts.size() == ex0);
                const uint64_t fileId = r.p.newSubId();
                r.h.clearPosts();
                r.p.setExtSub(fileId, fileChunks(fileId, "mine, slow"));
                ok("RV4-1: ...the viewer's file, read at last, is what shows", r.p.session().extSubId == fileId
                   && a(r.h.lastOf("playerSubs")).find("mine, slow") != std::string::npos);
            }
            {
                // the viewer's PICK of an embedded track (its extraction slow) and an automatic request arriving meanwhile
                Rig r;
                r.h.addFile(A, 5000, 11);
                r.h.probeMap[pbtrf::lowerW(A)] = def2;
                r.p.open({ A }, false);
                const std::vector<uint64_t> ids = embIds(r);
                ok("(the viewer picks #5: its extraction runs)", ids.size() == 4 && r.p.acceptSubGet(ids[1], false) && r.p.subGet(ids[1]));
                const uint64_t mine = r.h.extracts.back().req;
                const size_t c0 = r.h.extractCancels.size();
                ok("RV4-1: an AUTOMATIC request while the viewer's pick is still being read: ignored, his extraction NOT cancelled",
                   !r.p.acceptSubGet(ids[0], true) && r.h.extractCancels.size() == c0 && r.h.extracts.back().req == mine);
                r.h.clearPosts();
                r.p.onSubExtracted(mine, res("my pick"));
                ok("RV4-1: ...his pick lands, and is what shows", r.p.session().extSubId == ids[1]
                   && a(r.h.lastOf("playerSubs")).find("my pick") != std::string::npos);
            }
        }
        // <<< new API
        {
            // WHP-2: an item played through the FPROXY copy hears ONE track, the first
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            const uint64_t job1 = r.h.files.back().jobId;
            const std::wstring st0 = r.h.lastOf("translatorFile");
            ok("(WHP-2 setup: the default track 1 analysed)", r.h.files.size() == 1 && r.h.files[0].audio == "1:3" && fn(st0, L"audioOrd") == 1);
            r.p.playFailed(static_cast<uint64_t>(fn(st0, L"seq")));
            const std::wstring st1 = r.h.lastOf("translatorFile");
            ok("WHP-2: the page cannot play it -> FPROXY, audioOrd FORCED to 0, track 1's job cancelled, a new generation on track 0",
               r.h.proxies.size() == 1 && fn(st1, L"audioOrd") == 0 && r.h.files.size() == 2 && r.h.files[1].audio.empty()
               && !r.h.cancels.empty() && r.h.cancels.back() == job1 && fn(st1, L"jobId") > fn(st0, L"jobId"));
            r.p.setAudio(1, false);
            ok("WHP-2: playerSet{audioOrd:1} on the proxied item: IGNORED (no job, no choice stored)",
               r.h.files.size() == 2 && fn(r.h.lastOf("translatorFile"), L"audioOrd") == 0 && r.p.audioPicks().empty());
            r.h.now += 5000;
            r.p.tick();
            ok("WHP-2: ...and the forced 0 is never persisted as the user's", r.h.audioSaves == 0);
            // translation OFF: only audioOrd moves; ON again: the analysis on track 0
            Rig o;
            o.h.addFile(A, 5000, 11);
            o.h.probeMap[pbtrf::lowerW(A)] = def2;
            o.p.setTranslate(false, 0);
            o.p.open({ A }, false);
            const long long g0 = fn(o.h.lastOf("translatorFile"), L"jobId");
            o.p.playFailed(static_cast<uint64_t>(fn(o.h.lastOf("translatorFile"), L"seq")));
            ok("WHP-2 translation OFF: audioOrd 0, no job, no new generation", o.h.files.empty() && fn(o.h.lastOf("translatorFile"), L"audioOrd") == 0
               && fn(o.h.lastOf("translatorFile"), L"jobId") == g0);
            o.p.setTranslate(true, 0);
            ok("WHP-2: ...translation ON: the job on track 0 (audio \"\")", o.h.files.size() == 1 && o.h.files[0].audio.empty());
            // the probe answering AFTER the proxy request: its default rule does not win over the proxy
            Rig l;
            l.h.autoProbe = false;
            l.h.addFile(A, 5000, 11);
            l.p.open({ A }, false);
            l.p.playFailed(static_cast<uint64_t>(fn(l.h.lastOf("translatorFile"), L"seq")));
            l.p.onTracks(l.h.probeReqs[0].first, def2);
            ok("WHP-2: the probe answering after the proxy request: audioOrd 0 (not the default 1), ONE job, on track 0",
               fn(l.h.lastOf("translatorFile"), L"audioOrd") == 0 && l.h.files.size() == 1 && l.h.files[0].audio.empty());
        }
        {
            // the LRU keeps 8 extractions
            Rig r;
            std::vector<SubTrack> many;
            for (uint32_t k = 0; k < 10; ++k) many.push_back(sb(20 + k, "eng", "SRT", false, true));
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = mkInfo({ au(0, 2, "eng", "AAC", true, true) }, many);
            r.p.open({ A }, false);
            const std::string tr = tracksOf(r.h.lastOf("translatorFile"));
            std::vector<uint64_t> ids;
            for (size_t at = 0; (at = tr.find("{\"id\":", at)) != std::string::npos; at += 6) ids.push_back(std::strtoull(tr.c_str() + at + 6, nullptr, 10));
            auto okRes = std::make_shared<pbmedia::SubResult>();
            okRes->ok = true;
            for (size_t k = 0; k < 9 && k < ids.size(); ++k) {
                r.p.subGet(ids[k]);
                r.p.onSubExtracted(r.h.extracts.back().req, okRes);
            }
            const size_t n0 = r.h.extracts.size();
            r.p.subGet(ids[8]);
            const bool recentHit = r.h.extracts.size() == n0;
            r.p.subGet(ids[0]);
            ok("the LRU keeps the last 8 extractions: the 9th asked again is a hit, the 1st (evicted) is extracted again",
               ids.size() == 10 && recentHit && r.h.extracts.size() == n0 + 1);
        }
        {
            // the director (WH-H M2/Q1): a PRE-WH record (no AUD line) of an mp4/mov with 2+ sound tracks may hold another
            // track's cues (Media Foundation's own pick) -> a MISS, unless edited; mkv and one-track files are not affected
            const std::wstring P4 = L"D:\\Films\\Season 1\\clip.mp4", K4 = L"D:\\Films\\Season 1\\clip.mkv";
            auto mp4Info = [&](int n) {
                auto i = mkInfo(n == 2 ? std::vector<AudioTrack>{ au(0, 1, "eng", "AAC", true, true), au(1, 2, "fra", "AAC", false, true) }
                                       : std::vector<AudioTrack>{ au(0, 1, "eng", "AAC", true, true) }, {});
                i->container = "mp4";
                return i;
            };
            auto twoMkv = mkInfo({ au(0, 1, "eng", "AAC", true, true), au(1, 2, "fra", "AAC", false, true) }, {});
            // a complete record as 1.0.6 wrote it: no AUD line
            auto oldRecord = [&](const std::wstring& path, bool edited) {
                pbtrf::Record rec;
                rec.path = a(path); rec.size = 5000; rec.mtime = 11; rec.langA = "en"; rec.langB = "fr"; rec.mode = "auto"; rec.quality = "auto";
                rec.complete = true; rec.totalMs = 600000; rec.engine = tp::kFileEngineVersion; rec.edited = edited;
                rec.ranges.push_back(tp::MsRange{ 0, 600000 });
                rec.cues.push_back(cue(1, 0, 1000, 2000, "Old line", "Vieille ligne"));
                rec.trackKnown = false;
                const std::string bytes = pbtrf::packRecord(rec);
                return bytes;
            };
            ok("(a pre-WH record has no AUD line; parsed back: trackKnown false; a record written now has one)",
               oldRecord(P4, false).find("AUD") == std::string::npos && [&] {
                   pbtrf::Record back; const bool okk = pbtrf::parseRecord(oldRecord(P4, false), back); return okk && !back.trackKnown; }()
               && pbtrf::packRecord(pbtrf::Record{}).find("\nAUD\t0\t0\n") != std::string::npos);
            ok("unknownTrackMiss: only a track-less, UNEDITED record of a multi-track ISO-BMFF file",
               pbtrf::unknownTrackMiss(false, false, true) && !pbtrf::unknownTrackMiss(true, false, true) && !pbtrf::unknownTrackMiss(false, true, true)
               && !pbtrf::unknownTrackMiss(false, false, false));
            struct Case { const char* name; std::wstring path; std::shared_ptr<pbmedia::Info> info; bool edited, hit; };
            const Case cases[] = {
                { "a 2-track MP4's pre-WH record: a MISS -- analysed again from nothing (FILE, no `have`)", P4, mp4Info(2), false, false },
                { "a 2-track MKV's pre-WH record: a HIT (Media Foundation's pick was the first track there)", K4, twoMkv, false, true },
                { "a 1-track MP4's pre-WH record: a HIT", P4, mp4Info(1), false, true },
                { "an EDITED pre-WH record of a 2-track MP4: KEPT (never discarded silently), no job", P4, mp4Info(2), true, true },
            };
            for (const Case& c : cases) {
                Rig r;
                r.h.addFile(c.path, 5000, 11);
                r.h.probeMap[pbtrf::lowerW(c.path)] = c.info;
                const std::string k = pbtrf::cacheKey(c.path, 5000, 11, "en", "fr", "auto", "auto");
                r.h.cache[k] = oldRecord(c.path, c.edited);
                r.p.open({ c.path }, false);
                const std::wstring st = r.h.lastOf("translatorFile");
                const bool hit = r.h.files.empty() && fs(st, L"phase") == "done" && fn(st, L"count") == 1;
                const bool miss = r.h.files.size() == 1 && r.h.files[0].have.empty() && fn(st, L"count") == 0;
                ok(c.name, c.hit ? hit : miss, a(st).substr(0, 200));
            }
            {
                // the probe answers LATE: the old record was shown provisionally, then it turns out to be a 2-track mp4
                Rig r;
                r.h.autoProbe = false;
                r.h.addFile(P4, 5000, 11);
                r.h.cache[pbtrf::cacheKey(P4, 5000, 11, "en", "fr", "auto", "auto")] = oldRecord(P4, false);
                r.p.open({ P4 }, false);
                const long long g0 = fn(r.h.lastOf("translatorFile"), L"jobId");
                ok("(probe pending: the pre-WH record is shown provisionally, no job yet)", r.h.files.empty() && fn(r.h.lastOf("translatorFile"), L"count") == 1);
                r.p.onTracks(r.h.probeReqs[0].first, mp4Info(2));
                ok("...the probe says 2 sound tracks in an MP4: a NEW generation from nothing, and the job (no `have`)",
                   fn(r.h.lastOf("translatorFile"), L"jobId") > g0 && fn(r.h.lastOf("translatorFile"), L"count") == 0
                   && r.h.files.size() == 1 && r.h.files[0].have.empty());
            }
            {
                // the probe ALREADY KNOWN at the opening (the file played earlier in this tray run): the pre-WH record is a miss
                // at load time -- no late probe to catch it then
                Rig r;
                r.h.addFile(P4, 5000, 11);
                r.h.addFile(B);
                r.h.probeMap[pbtrf::lowerW(P4)] = mp4Info(2);
                const std::string k = pbtrf::cacheKey(P4, 5000, 11, "en", "fr", "auto", "auto");
                r.h.cache[k] = oldRecord(P4, false);
                r.p.open({ P4, B }, false);
                r.p.queueOp("play", r.p.items()[1].id, 0, 0);           // away (its job cancelled; the probe stays known)
                r.h.cache[k] = oldRecord(P4, false);                   // (as it was: the cancelled job wrote nothing yet)
                const size_t files0 = r.h.files.size(), probes0 = static_cast<size_t>(r.h.probes);
                r.p.queueOp("play", r.p.items()[0].id, 0, 0);           // back: no new probe -- known
                const std::wstring st = r.h.lastOf("translatorFile");
                ok("reopened with its probe KNOWN: no probe asked, and the 2-track MP4's pre-WH record is a MISS at load (FILE, no `have`)",
                   static_cast<size_t>(r.h.probes) == probes0 && r.h.files.size() == files0 + 1 && r.h.files.back().have.empty()
                   && fn(st, L"count") == 0, a(st).substr(0, 200));
            }
            {
                // the probe so late that the job already RESUMED a pre-WH partial: the late answer restarts it from nothing
                Rig r;
                r.h.autoProbe = false;
                r.h.addFile(P4, 5000, 11);
                pbtrf::Record part;
                part.path = a(P4); part.size = 5000; part.mtime = 11; part.langA = "en"; part.langB = "fr"; part.mode = "auto";
                part.quality = "auto"; part.complete = false; part.totalMs = 600000; part.engine = tp::kFileEngineVersion;
                part.trackKnown = false;
                part.ranges.push_back(tp::MsRange{ 0, 30000 });
                part.cues.push_back(cue(1, 0, 1000, 2000, "Maybe the other track", "Peut-etre l'autre piste"));
                r.h.cache[pbtrf::cacheKey(P4, 5000, 11, "en", "fr", "auto", "auto")] = pbtrf::packRecord(part);
                r.p.open({ P4 }, false);
                r.h.now += 3000;
                r.p.tick();
                ok("(no probe for 3 s: the job RESUMED the pre-WH partial, have 0-30000)", r.h.files.size() == 1 && r.h.files[0].have == "0-30000");
                r.p.onTracks(r.h.probeReqs[0].first, mp4Info(2));
                ok("...the probe then says a 2-track MP4: that job cancelled, a NEW one from NOTHING (no `have`), no old cue shown "
                   "(the cancelled job's set is not reused)",
                   r.h.files.size() == 2 && r.h.files[1].have.empty() && !r.h.cancels.empty() && r.h.cancels.back() == r.h.files[0].jobId
                   && fn(r.h.lastOf("translatorFile"), L"count") == 0, r.h.files.size() > 1 ? r.h.files[1].have : "no second FILE");
            }
            {
                // the kept edited one is written back WITHOUT a marker (never stamped as known)
                Rig r;
                r.h.addFile(P4, 5000, 11);
                r.h.probeMap[pbtrf::lowerW(P4)] = mp4Info(2);
                const std::string k = pbtrf::cacheKey(P4, 5000, 11, "en", "fr", "auto", "auto");
                r.h.cache[k] = oldRecord(P4, true);
                r.p.open({ P4 }, false);
                const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
                using EC2 = Player::EditCue;
                EC2 e; e.s = 1000; e.e = 2000; e.src = "Old line, fixed"; e.dst = "Vieille ligne"; e.lang = "en"; e.dstLang = "fr";
                ok("(an edit of the kept record)", r.p.editCues(static_cast<uint64_t>(gen), 1, { 0 }, { e }));
                r.h.now += 2500;
                r.p.tick();
                ok("...saved back with NO track marker: an unsure record is never stamped as the first track's",
                   r.h.cache[k].find("Old line, fixed") != std::string::npos && r.h.cache[k].find("AUD") == std::string::npos, r.h.cache[k].substr(0, 300));
            }
        }
        {
            // pre-analysis: the next container is PROBED before it is analysed ahead, and analysed on its own choice
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.addFile(B);
            r.h.probeMap[pbtrf::lowerW(A)] = single;
            r.h.probeMap[pbtrf::lowerW(B)] = def2;
            r.p.open({ A }, false);
            r.p.add({ B });
            const uint64_t job = r.h.files.back().jobId;
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            ok("pre-analysis of the next item (a .mkv): probed first, then its FILE on ITS default track (\"1:3\")",
               r.h.probes == 2 && r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].audio == "1:3",
               r.h.files.size() > 1 ? r.h.files[1].audio : "no pre-analysis");
            r.p.queueOp("next", 0, 0, 0);
            ok("...switching to it ADOPTS that job (same track): no new FILE", r.h.files.size() == 2);
        }
    }

    std::printf("\nplayer_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

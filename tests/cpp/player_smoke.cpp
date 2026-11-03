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
    std::vector<std::wstring> identityAsked;          // (FX2-A: every path asked, lower-cased -- which Word file a tick reads)
    std::map<std::wstring, uint64_t> slowStat;        // (FX2-B: lower-cased path -> how long each read of it takes, fake ms)
    bool identity(const std::wstring& p, uint64_t& s, uint64_t& m) override {
        ++identityCalls;
        identityAsked.push_back(pbtrf::lowerW(p));
        const auto sl = slowStat.find(pbtrf::lowerW(p));
        if (sl != slowStat.end()) now += sl->second;   // (a share that answers slowly: the clock moves during the read)
        auto it = disk.find(pbtrf::lowerW(p));
        if (it == disk.end()) return false;
        s = it->second.first; m = it->second.second;
        return true;
    }
    // FX2-B: the NETWORK as this fake machine has it -- a UNC path by its form (the core's own uncPath, as the real host's default),
    // a drive letter mapped to a share (`remoteDrives`, lower-case); every question counted (never asked in a tick)
    std::set<wchar_t> remoteDrives;
    int netAsks = 0;
    bool onNetwork(const std::wstring& p) override {
        ++netAsks;
        if (pbplayer::uncPath(p)) return true;
        const wchar_t d = pbplayer::driveOf(p);
        return d != 0 && remoteDrives.count(static_cast<wchar_t>(d | 0x20)) > 0;
    }
    bool hostDefaultNet(const std::wstring& p) { return pbplayer::Host::onNetwork(p); }   // (the bridge's: Host's own default)
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
    // U1 x L6: the native engine (mpv) plays the current item -- off by default (as before 1.1.0's L2), on for the adoption check
    bool mpvOn = false;
    bool mpvPlays() override { return mpvOn; }
    // U2: the core's word that an imported model failed (it has ALREADY gone back to the default for that kind). (U2b re-point:
    // `say` = the host says it in a notice -- only when this failure made the choice fall back and the strip does not say it;
    // `redone` = the file a batch redoes with the default)
    struct Fell { std::string kind, path; std::wstring sentence; bool say = false; std::wstring redone; };
    std::vector<Fell> fells;
    void modelFailed(const std::string& kind, const std::string& path, const std::wstring& sentence, bool say,
                     const std::wstring& redone) override {
        fells.push_back({ kind, path, sentence, say, redone });
    }
    // U2b: the models in use changed (the menu's x) -- counted; the core is asked what they are
    int inUseChanges = 0;
    void modelsInUseChanged() override { ++inUseChanges; }
    // U2b (G2): the Word file rewritten IN PLACE (`t.prefer`); docxLocked = open in another program (refused, said); docxNotOurs =
    // the registry says it is not ours any more (refused, nothing said)
    bool docxLocked = false, docxNotOurs = false;
    int rewrites = 0, rewriteTries = 0;
    // FX (02/11/2026, docs/PROGRESS_FIX.md): THE REGISTRY of the Word files, as the bridge keeps it -- `regMode` on: writeTranscript,
    // rewriteTranscript and docxWriter follow the bridge's FX order through TrFileStore.h's OWN pure functions (one Word file per
    // media AND sound track: the track's names, registryOwnsFor, registryTrackDocx; a held / read-only file refused with the bridge's
    // sentences, never another folder; a folder that refuses -> Documents). Off (the default): the U3 / U2b behaviour above.
    bool regMode = false;
    std::vector<pbtrf::RegEntry> reg;
    std::set<std::wstring> heldDocx, readOnlyDocx;      // lower-cased Word files another program holds / that are read-only
    std::set<std::wstring> awayMedia;                   // lower-cased media whose folder refuses the .tmp (-> Documents)
    std::wstring docsDir = L"C:\\Users\\me\\Documents\\SuVtitle Transcripts";   // (FX2-B: may be a redirected, network one)
    int writeTries = 0;
    static std::wstring heldSentence() { return L"The Word file is open in another program: close it to update it."; }
    static std::wstring readOnlySentence() { return L"The Word file is read-only: it is left as it is. Save .docx\x2026 writes a new one."; }
    std::wstring refusal(const std::wstring& p) const {
        const std::wstring l = pbtrf::lowerW(p);
        return heldDocx.count(l) ? heldSentence() : readOnlyDocx.count(l) ? readOnlySentence() : std::wstring();
    }
    bool regOwns(const pbplayer::CueSet& set, const std::wstring& p) const {
        auto it = disk.find(pbtrf::lowerW(p));
        return it != disk.end() && pbtrf::registryOwnsFor(reg, p, it->second.first, it->second.second, set.path, set.key, set.audio.ord);
    }
    std::wstring regWrite(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, const std::wstring& p, uint64_t& size, uint64_t& mtime) {
        size = 1000 + t.turns.size() + docx.size();         // (every write a new identity)
        mtime = 500 + docx.size();
        disk[pbtrf::lowerW(p)] = { size, mtime };
        docx.push_back({ p, t });
        pbtrf::registryPut(reg, p, size, mtime, set.path, pbtrf::transcriptRegKind(), set.key, set.audio.ord);
        return p;
    }
    int docxWriterCalls = 0;                            // (FX2-A: the registry asked -- never once a second)
    std::string docxWriter(const pbplayer::CueSet& set, const std::wstring& path) override {
        ++docxWriterCalls;
        if (!regMode) return std::string();
        if (!path.empty()) { const pbtrf::RegEntry* e = pbtrf::registryTranscriptEntry(reg, path); return e ? e->key : std::string(); }
        return pbtrf::registryTrackDocx(reg, set.path, set.audio.ord, [&](const std::wstring& p) { return disk.count(pbtrf::lowerW(p)) > 0; });
    }
    bool rewriteTranscript(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& why, uint64_t& size,
                           uint64_t& mtime) override {
        if (regMode) {                                   // (FX: the bridge's rewriteTranscriptDocx, ownership per analysis)
            ++rewriteTries;
            why.clear();
            if (t.prefer.empty() || !regOwns(set, t.prefer)) return false;
            why = refusal(t.prefer);
            if (!why.empty()) return false;
            ++rewrites;
            regWrite(set, t, t.prefer, size, mtime);
            return true;
        }
        (void)set;
        ++rewriteTries;
        why.clear();
        if (t.prefer.empty() || docxNotOurs) return false;
        if (docxLocked) { why = L"The Word file is open in another program: close it to update it."; return false; }
        size = 2000 + t.turns.size();
        mtime = 900 + docx.size();
        disk[pbtrf::lowerW(t.prefer)] = { size, mtime };
        docx.push_back({ t.prefer, t });
        ++rewrites;
        return true;
    }
    // U3: the transcript -- the FVOICEs sent, the voices records (key -> packed text, through the REAL pack/parse), the Word
    // files written (path + what they hold; each added to `disk`, so the core's identity check sees it), the timecode probes
    // (answered at once by default from `tcMap`, else held in tcReqs)
    std::vector<tp::WireVoice> voicesSent;
    std::map<std::string, std::string> voicesDisk;
    int voicesWrites = 0;
    struct Docx { std::wstring path; pbtrs::TranscriptOut t; };
    std::vector<Docx> docx;
    bool docxFails = false;
    bool autoTc = true;
    std::map<std::wstring, pbtc::SourceTc> tcMap;                            // lower-cased path
    std::vector<std::pair<uint64_t, std::wstring>> tcReqs;
    void sendVoice(const tp::WireVoice& v) override { voicesSent.push_back(v); }
    bool readVoices(const std::string& k, pbtrf::VoiceRec& v) override {
        auto it = voicesDisk.find(k);
        return it != voicesDisk.end() && pbtrf::parseVoicesText(it->second, v);
    }
    bool peekVoices(const std::string& k, pbtrf::VoiceRec& v) override {
        auto it = voicesDisk.find(k);
        return it != voicesDisk.end() && pbtrf::parseVoicesText(it->second, v, true);
    }
    void writeVoices(const std::string& k, const pbtrf::VoiceRec& v) override { voicesDisk[k] = pbtrf::packVoices(v); ++voicesWrites; }
    std::wstring writeTranscript(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& note, uint64_t& size,
                                 uint64_t& mtime) override {
        note.clear();
        ++writeTries;
        if (regMode) {
            // (FX: the bridge's writeTranscriptDocx, in its order) the file written before, ours for THIS analysis, in place -- a
            // held / read-only one refused and said, never elsewhere; ANOTHER analysis of this sound track holding its Word file ->
            // nothing; else the track's names (a folder that refuses the .tmp -> Documents\SuVtitle Transcripts)
            auto exists = [&](const std::wstring& p) { return disk.count(pbtrf::lowerW(p)) > 0; };
            auto owns = [&](const std::wstring& p) { return regOwns(set, p); };
            const bool away = awayMedia.count(pbtrf::lowerW(set.path)) > 0;
            // (FX2: a folder that refuses the .tmp refuses the file written before too, when it is in it -- WriteFail::Folder:
            //  the bridge goes on below; a held / read-only one is WriteFail::Target: said, never elsewhere)
            const bool ownPrefer = !t.prefer.empty() && exists(t.prefer) && owns(t.prefer);
            if (ownPrefer && !(away && pbtrf::lowerW(pbtrf::dirOf(t.prefer)) == pbtrf::lowerW(set.folder))) {
                note = refusal(t.prefer);
                if (!note.empty()) return std::wstring();
                return regWrite(set, t, t.prefer, size, mtime);
            }
            // (FX2 A5: the bridge asks F2's guard only for an analysis that does not own the file written before)
            const std::string other = ownPrefer ? std::string() : pbtrf::registryTrackDocx(reg, set.path, set.audio.ord, exists);
            if (!other.empty() && other != set.key) {
                note = L"The Word file of this sound track holds another transcript of it (another language pair or model): it is kept.";
                return std::wstring();
            }
            const std::wstring dir = away ? docsDir : set.folder;
            const std::wstring p = pbtrf::chooseTranscriptPath(dir, pbtrf::stripExt(set.name), pbtrf::extOf(set.name), false, exists, owns,
                                                               set.audio.ord);
            if (p.empty()) { note = L"The Word file was not saved: every transcript name for this file is taken by other files."; return std::wstring(); }
            if (exists(p)) { note = refusal(p); if (!note.empty()) return std::wstring(); }
            if (away) note = L"The media's folder could not be written to: the transcript is in Documents\\SuVtitle Transcripts.";
            return regWrite(set, t, p, size, mtime);
        }
        if (docxFails) { note = L"This folder does not take new files."; return std::wstring(); }
        const std::wstring p = !t.prefer.empty() ? t.prefer : pbtrf::joinPath(set.folder, pbtrf::stripExt(set.name) + L" - transcript.docx");
        size = 1000 + t.turns.size();
        mtime = 500 + docx.size();
        disk[pbtrf::lowerW(p)] = { size, mtime };
        docx.push_back({ p, t });
        return p;
    }
    void probeTimecode(uint64_t req, const std::wstring& path) override {
        if (!autoTc || !player) { tcReqs.push_back({ req, path }); return; }
        auto it = tcMap.find(pbtrf::lowerW(path));
        player->onTimecode(req, it == tcMap.end() ? pbtc::SourceTc{} : it->second);
    }
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
// FX (F8): the speakers' numbering a rename is typed against (the page sends translatorTranscript's vrev, taken when
// its field opens)
static uint64_t vrevOf(const Player& p) { return p.loaded() && p.session().set ? p.session().set->vrev : 0; }

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

// U1: the badge the window shows NOW for item `id` -- the newest playerItem or playerQueue that names it
static std::string badgeNow(const FakeHost& h, uint64_t id) {
    const std::string idk = "\"id\":" + std::to_string(id) + ",";
    for (auto it = h.window.rbegin(); it != h.window.rend(); ++it) {
        const std::string t = typeOf(*it);
        if (t != "playerItem" && t != "playerQueue") continue;
        const std::string s = a(*it);
        const size_t at = s.find(idk);
        if (at == std::string::npos) continue;
        const size_t st = s.find("\"state\":\"", at);
        if (st == std::string::npos) continue;
        const size_t b = st + 9, e = s.find('"', b);
        return e == std::string::npos ? std::string("<bad>") : s.substr(b, e - b);
    }
    return "<none>";
}

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
        r.p.open({ A, B, C }, true);
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
        ok("the next file IN THE SAME FOLDER: no new mapping, no reload -- and (U1) NO analysis by itself: nobody asked for it",
           r.h.reloads == 1 && r.h.files.size() == files0 && fs(r.h.lastOf("translatorFile", true), L"phase") == "off");
        ok("...and the first job was cancelled (a press is for the file while it plays; its FDONE will save the partial)",
           !r.h.cancels.empty() && r.h.cancels.back() == r.h.files[0].jobId);
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
        r.p.open({ A }, true);
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
        r2.p.open({ A }, true);
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
        r3.p.open({ A }, true);
        ok("a cache hit whose files are on disk and up to date: no write at all", r3.h.srtWrites == 0, n2s(r3.h.srtWrites));
        Rig r4;
        r4.h.addFile(A, 5000, 11);
        r4.h.cache = r.h.cache;
        r4.h.srtDisk = r.h.srtDisk;
        r4.h.srtDisk[pbtrf::joinPath(L"D:\\Films\\Season 1", L"Ep 1.en.srt")] = "stale";
        r4.p.open({ A }, true);
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
        r.p.open({ A }, true);
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
        r2.p.open({ A }, true);
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
        r3.p.open({ A }, true);
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
        r.p.open({ A }, true);
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

    // ---- 6. Cancel, then Translate again (U1: the 1.0.x On/Off switch became the Translate BUTTON) ----------------------------------
    std::printf("[6] Cancel, then Translate again\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, true);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onCue(cue(job, 0, 1000, 2000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.cancel(static_cast<uint64_t>(gen));
        ok("CANCEL: the job is cancelled", !r.h.cancels.empty() && r.h.cancels.back() == job);
        r.p.onDone(done(job, "cancelled"), false);
        ok("...after its FDONE: phase cancelled, the subtitles already made kept, the file no longer asked for",
           fs(r.h.lastOf("translatorFile"), L"phase") == "cancelled" && fn(r.h.lastOf("translatorFile"), L"count") == 1
           && !fb(r.h.lastOf("translatorFile"), L"translate"));
        r.p.pos(gen, 15000, 600000, true, "tick");
        ok("...and playing on starts nothing", r.h.files.size() == 1);
        r.p.pos(gen, 20000, 600000, false, "pause");
        r.p.analyse(false);
        ok("TRANSLATE again at 0:20: FILE resumes from the vouched ranges, focus at the playhead -- the same generation (nothing re-sent)",
           r.h.files.size() == 2 && r.h.files[1].have == "0-30000" && r.h.files[1].focusMs == 20000
           && fn(r.h.lastOf("translatorFile"), L"jobId") == gen, r.h.files.size() > 1 ? r.h.files[1].have : "");
        ok("the queue file no longer carries 1.0.x's switch (U1: OPT translate is never written)",
           (r.h.now += 5000, r.p.tick(), !r.h.queueBytes.empty() && r.h.queueBytes.find("OPT\ttranslate") == std::string::npos));
        // the card absent
        Rig r2;
        r2.p.setHelperAvailable(false);
        r2.h.addFile(A);
        r2.p.open({ A }, true);
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

    // ---- 7. no pre-analysis any more; "Translate all" (U1: only the batch analyses a file that is not playing) -----------------------
    std::printf("[7] no pre-analysis; Translate all\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
        r.p.open({ A, B, C }, true);
        r.p.windowReady();                                  // the page reloaded for the folder, and is back
        const uint64_t ja = r.h.files.back().jobId;
        ok("while the current file is analysed: nothing else", r.h.files.size() == 1);
        r.p.onProg(prog(ja, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(ja, "ok"), false);
        r.h.now += 5000; r.p.tick();
        ok("U1: the current file complete, shown, on File -- NOTHING else starts (the next items are not pre-analysed any more)",
           r.h.files.size() == 1);
        const uint64_t idB = r.p.items()[1].id, idC = r.p.items()[2].id;
        r.p.analyseAll();
        ok("TRANSLATE ALL: the first file not done in the LIST's order (Ep 2), from its start (focus 0) -- one job at a time",
           r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].focusMs == 0);
        const uint64_t jb = r.h.files[1].jobId;
        ok("...the rows say it: Ep 2 analysing, Ep 10 queued, Ep 1 done",
           badgeNow(r.h, idB) == "analysing" && badgeNow(r.h, idC) == "queued" && badgeNow(r.h, r.p.items()[0].id) == "ready",
           badgeNow(r.h, idB) + "/" + badgeNow(r.h, idC));
        r.p.onProg(prog(jb, "analyzing", 500000, "0-40000"));
        r.p.queueOp("play", idC, 0, 0);
        const uint64_t jc = r.h.files.back().jobId;
        ok("playing a QUEUED file: it goes FIRST (its FILE), the batch's job yields (FCANCEL) -- Ep 2 stays queued",
           r.h.files.size() == 3 && r.h.files[2].path == a(C) && !r.h.cancels.empty() && r.h.cancels.back() == jb);
        r.p.onDone(done(jb, "cancelled"), false);
        ok("...Ep 2's badge: queued again (what it has is kept)", badgeNow(r.h, idB) == "queued", badgeNow(r.h, idB));
        r.p.onProg(prog(jc, "analyzing", 700000, "0-700000"));
        r.p.onDone(done(jc, "ok"), false);
        ok("Ep 10 done: the batch goes on with Ep 2, RESUMED from its partial (have 0-40000)",
           r.h.files.size() == 4 && r.h.files[3].path == a(B) && r.h.files[3].have == "0-40000", r.h.files.size() > 3 ? r.h.files[3].have : "");
        const uint64_t jb2 = r.h.files[3].jobId;
        r.p.onCue(cue(jb2, 0, 41000, 43000));
        r.p.onProg(prog(jb2, "analyzing", 500000, "0-60000"));
        r.p.queueOp("play", idB, 0, 0);
        ok("switching TO the file the batch is analysing ADOPTS its job: no new FILE, no cancel of it",
           r.h.files.size() == 4 && (r.h.cancels.empty() || r.h.cancels.back() != jb2));
        const std::wstring st = r.h.lastOf("translatorFile", true);
        ok("...the window gets its cues at once (the adopted job's), the file asked for (translate)",
           a(r.h.lastOf("translatorFileCues")).find("41000") != std::string::npos && fs(st, L"phase") == "analyzing" && fb(st, L"translate"), a(st).substr(0, 200));
        r.p.onDone(done(jb2, "error", "decoder choked"), false);
        ok("an adopted job's FDONE error is the CURRENT file's error -- and its row says failed",
           fs(r.h.lastOf("translatorFile"), L"phase") == "error" && badgeNow(r.h, idB) == "error");
        // the current file FAILED, nothing asked for the others: nothing starts
        Rig rf;
        rf.h.addFile(A, 5000, 11); rf.h.addFile(B, 6000, 12);
        rf.p.open({ A, B }, true);
        rf.p.windowReady();
        rf.p.onDone(done(rf.h.files.back().jobId, "error", "decoder choked"), false);
        rf.h.now += 5000; rf.p.tick();
        ok("the current file FAILED (incomplete, no job): nothing analysed next", rf.h.files.size() == 1);
        // repeat one: nothing either
        Rig r2;
        r2.h.addFile(A, 5000, 11); r2.h.addFile(B, 6000, 12);
        r2.p.setRepeat(pbplayer::Repeat::One);
        r2.p.open({ A, B }, true);
        const uint64_t j = r2.h.files.back().jobId;
        r2.p.onProg(prog(j, "analyzing", 600000, "0-600000"));
        r2.p.onDone(done(j, "ok"), false);
        ok("repeat ONE: nothing else starts", r2.h.files.size() == 1);
        // a batch file that fails is not retried (until Translate all is pressed again); the window hidden does not stop it
        Rig r3;
        r3.h.addFile(A, 5000, 11); r3.h.addFile(B, 6000, 12); r3.h.addFile(C, 7000, 13);
        r3.p.open({ A, B, C }, true);
        const uint64_t j3 = r3.h.files.back().jobId;
        r3.p.onDone(done(j3, "ok"), false);
        r3.p.analyseAll();
        const uint64_t jp = r3.h.files.back().jobId;
        r3.p.onDone(done(jp, "error", "bad file"), false);
        ok("a batch file that FAILED is not retried: the one after it goes next",
           r3.h.files.size() == 3 && r3.h.files[2].path == a(C));
        ok("...the failed item's badge says error", badgeNow(r3.h, r3.p.items()[1].id) == "error", badgeNow(r3.h, r3.p.items()[1].id));
        const size_t cancels3 = r3.h.cancels.size();
        r3.p.windowShown(false);
        ok("the window HIDDEN: Translate all goes on (it was asked for -- the pre-analysis stopped here, the batch does not)",
           r3.h.cancels.size() == cancels3);
        ok("every post well-formed", r.h.badJson + r2.h.badJson + r3.h.badJson == 0);
    }

    // ---- 8. the queue: order, repeat, shuffle, ended, prev, remove, move, clear ----------------------------------------------------
    std::printf("[8] the queue\n");
    {
        Rig r;
        r.p.setHelperAvailable(false);
        r.h.addFile(A); r.h.addFile(B); r.h.addFile(C); r.h.addFile(X);
        r.p.open({ A, B, C, X }, true);
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
        s.p.open(many, true);
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
        s2.p.open(many, true);
        s2.p.setShuffle(true);
        ok("...the same seed draws the same order (deterministic under test)", s2.p.playOrderIds() == order);
        ok("every post well-formed", r.h.badJson + s.h.badJson == 0);
    }

    // ---- 9. missing files, the proxy, sidecars, external subtitles -----------------------------------------------------------------------
    std::printf("[9] missing files, the proxy, sidecars, external subtitles\n");
    {
        Rig r;
        r.h.addFile(A); r.h.addFile(B); r.h.addFile(C);
        r.p.open({ A, B, C }, true);
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
        m.p.open({ X }, true);
        ok("a folder that cannot be mapped: no URL, the sound asked for at once", fs(m.h.lastOf("translatorFile", true), L"url").empty()
           && m.h.proxies.size() == 1, a(m.h.lastOf("translatorFile", true)).substr(0, 200));
        // sidecars + external subtitles
        Rig s;
        s.h.addFile(A);
        pbplayer::Sidecar sc; sc.name = L"Ep 1.en.srt"; sc.hint = L"en"; sc.path = L"D:\\Films\\Season 1\\Ep 1.en.srt";
        s.h.sidecarMap[pbtrf::lowerW(L"D:\\Films\\Season 1|Ep 1.mkv")] = { sc };
        s.p.open({ A }, true);
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
        r.p.open({ A }, true);
        long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(gen, 751000, 3000000, true, "tick");
        r.p.close();
        ok("CLOSE: idle, a new generation, the queue kept", !r.p.loaded() && fs(r.h.lastOf("translatorFile", true), L"phase") == "idle"
           && fn(r.h.lastOf("translatorFile", true), L"jobId") > gen && r.p.items().size() == 1);
        r.h.files.clear();
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        const std::wstring st = r.h.lastOf("translatorFile", true);
        ok("played again: it RESUMES at 12:31 (startMs, resumed) -- and (U1) nothing is analysed by itself",
           fn(st, L"startMs") == 751000 && fb(st, L"resumed") && r.h.files.empty() && fs(st, L"phase") == "off", a(st).substr(0, 300));
        r.p.analyse(false);
        ok("...Translate: the job is focused there (12:31)", r.h.files.size() == 1 && r.h.files[0].focusMs == 751000);
        gen = fn(st, L"jobId");
        r.p.pos(gen, 2900000, 3000000, false, "pause");
        r.p.close();
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        ok("left near its end (97 %): it starts over (no resume)", fn(r.h.lastOf("translatorFile", true), L"startMs") == 0);
        r.h.now += 20000; r.p.tick();
        std::map<std::string, pbplayer::ResumeEntry> m;
        pbplayer::parseResume(r.h.resumeBytes, m);
        ok("the resume table is written and reads back (the finished entry gone)", r.h.resumeSaves >= 1 && m.empty(), r.h.resumeBytes);
        // cancel + analyse (U1: the file played again is asked for first -- Translate)
        r.p.analyse(false);
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
        ok("TRANSLATE again (U1: the press on what is shown): the SAME generation -- the subtitles on screen stay -- resuming from [0,30000]",
           r.h.files.size() == nf + 1 && r.h.files.back().have == "0-30000" && fn(r.h.lastOf("translatorFile", true), L"jobId") == gen);
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
        q.repeat = pbplayer::Repeat::All; q.shuffle = true; q.cur = 1;   // (U1: no `translate` any more)
        pbplayer::QueueFile::It i1; i1.path = L"D:\\Films\\R\x00e9union #2 100%\\a b.mkv"; i1.size = 5; i1.mtime = 6; i1.durMs = 7;
        pbplayer::QueueFile::It i2; i2.path = L"\\\\nas\\share\\x\ty.mp3"; i2.size = 8;
        q.items = { i1, i2 };
        q.order = { 1, 0 };
        const std::string b = pbplayer::packQueue(q);
        pbplayer::QueueFile r;
        ok("player.txt round-trips (accents, #, %, spaces, a UNC path, a tab escaped) -- and carries no OPT translate (U1)",
           pbplayer::parseQueue(b, r) && b.find("OPT\ttranslate") == std::string::npos
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
           p.p.items().size() == 2 && p.p.current() == 1 && !p.p.loaded() && !p.p.wanted() && p.p.repeat() == pbplayer::Repeat::All);
        // U1: a 1.0.x file says `OPT translate 1` (the old global switch): read, ignored -- it starts nothing
        std::string old = b;
        old.insert(old.find("OPT\trepeat"), "OPT\ttranslate\t1\n");
        Rig po;
        po.h.addFile(i1.path, 5, 6); po.h.addFile(i2.path, 8, 0);
        po.p.load(old, "");
        po.p.queueOp("play", po.p.items()[0].id, 0, 0);
        po.h.now += 5000; po.p.tick();
        ok("U1: a 1.0.x queue file's `OPT translate 1` is read and IGNORED -- its items load, playing one starts NO analysis",
           pbplayer::parseQueue(old, r) && r.items.size() == 2 && po.p.loaded() && po.h.files.empty() && !po.p.wanted());
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
        r.p.open({ Xr }, true);
        r.p.add({ A1, A2 });
        r.p.open({ A1, A2, A3 }, true);                      // the user drops folder Ar (a1 a2 a3) to PLAY it
        std::string q;
        for (const auto& it : r.p.items()) q += a(it.name) + " ";
        ok("C2: the folder's FIRST file plays (a1) -- not the item the insert shifted into its index (a3)",
           a(r.p.session().name) == "a1.mkv" && r.p.items().size() == 4, q + "| playing " + a(r.p.session().name));
        Rig r2;
        r2.h.addFile(A1); r2.h.addFile(A2);
        r2.p.open({ A1, A2, A1, A2 }, true);
        ok("C2: a path given twice in one call is queued once", r2.p.items().size() == 2 && a(r2.p.session().name) == "a1.mkv",
           n2s(r2.p.items().size()));
    }
    {
        // C3: a resumed run's first cue starting AT have.e is kept (the held one ends 1 ms past it: allowed)
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        r.p.open({ Ar }, true);
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
        // C5: Cancel -> Translate mid-film (U1: it was translation Off -> On) re-posts the SAME file: no autoplay, no "Resumed",
        // start = the playhead
        Rig r;
        const std::wstring Ar = L"D:\\Ar\\film.mkv";
        r.h.addFile(Ar, 5000, 11);
        std::string res = "# AudioPB player resume v1\nPOS\t" + tp::esc(pbplayer::resumeKey(Ar, 5000, 11)) + "\t20000\t600000\t1760000000\n";
        r.p.load(std::string(), res);
        r.p.open({ Ar }, true);
        const uint64_t job = r.h.files.back().jobId;
        const std::wstring st0 = r.h.lastOf("translatorFile");
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        r.p.onProg(prog(job, "analyzing", 600000, "0-90000"));
        r.p.onCue(cue(job, 0, 91000, 92000));                 // in flight: no FPROG covers it -> a regen later
        r.p.pos(static_cast<uint64_t>(fn(st0, L"jobId")), 300000, 600000, false, "pause");   // PAUSED at 5:00
        r.p.cancel(static_cast<uint64_t>(fn(st0, L"jobId")));
        r.p.onDone(done(job, "cancelled"), false);
        r.p.pos(static_cast<uint64_t>(fn(st0, L"jobId")), 301000, 600000, false, "seek");    // still paused, at 5:01
        r.h.clearPosts();
        r.p.analyse(false);
        const std::wstring st1 = r.h.lastOf("translatorFile");
        ok("C5: (opened: autoplay, resumed at 0:20)", fb(st0, L"autoplay") && fb(st0, L"resumed") && fn(st0, L"startMs") == 20000);
        ok("C5: Cancel -> Translate re-posts the SAME file as no opening: autoplay false, resumed false, start = the playhead",
           !st1.empty() && fn(st1, L"jobId") > fn(st0, L"jobId") && !fb(st1, L"autoplay") && !fb(st1, L"resumed")
           && fn(st1, L"startMs") == 301000, a(st1).substr(0, 260));
    }
    {
        // P1: while the window's page reloads NOTHING is posted to it (the OLD document would get it)
        Rig r(false);
        const std::wstring Ar = L"D:\\Ar\\a.mkv", Br = L"E:\\Br\\b.mkv";
        r.h.addFile(Ar); r.h.addFile(Br);
        r.p.open({ Ar }, true);
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
        r.p.open(paths, true, &known);
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
        r.p.open({ Ar }, true);
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
        r.p.open({ Ar }, true);
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);                          // fr<->en: nothing yet, a job starts
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r2.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r2.p.open({ Fm }, true);
        ok("ENGINE: ...reopened: a HIT now (no job, \"analysed earlier\", no upgrade)", r2.h.files.empty()
           && fb(r2.h.lastOf("translatorFile"), L"cached") && !fb(r2.h.lastOf("translatorFile"), L"upgrade"));
    }
    for (int eng : { kCur - 1, kCur, kCur + 1 }) {
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", eng, false, true);
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
        ok("ENGINE: a PARTIAL record of an older engine is not resumed: analysed from nothing (FILE have empty), as an upgrade",
           r.h.files.size() == 1 && r.h.files[0].have.empty() && fb(r.h.lastOf("translatorFile"), L"upgrade"));
        Rig r2;
        r2.p.setPair("fr", "en", "auto");
        r2.h.addFile(Fm, 7000, 13);
        r2.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, false);
        r2.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
        // the QUEUE: the next file's older record is not "ready" -- "Translate all" analyses it from nothing (U1: it was the
        // pre-analysis; nothing analyses a file that is not playing unless asked)
        const std::wstring Fm2 = L"D:\\Films\\Feature 2.mkv";
        for (int eng : { kCur - 1, kCur }) {
            Rig r;
            r.p.setPair("fr", "en", "auto");
            r.h.addFile(Fm, 7000, 13);
            r.h.addFile(Fm2, 7000, 13);
            r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, true);
            r.h.cache[pbtrf::cacheKey(Fm2, 7000, 13, "fr", "en", "auto", "auto")] = engRecord(Fm2, "fr", "en", eng, false, true);
            r.p.open({ Fm, Fm2 }, true);
            const size_t before = r.h.files.size();
            r.p.analyseAll();
            const std::string q = a(r.h.lastOf("playerQueue"));
            if (eng < kCur)
                ok("ENGINE: the next file's OLDER record is not a \"ready\" badge: Translate all analyses it from nothing (nothing before it)",
                   before == 0 && r.h.files.size() == 1 && r.h.files[0].path == a(Fm2) && r.h.files[0].have.empty()
                   && badgeNow(r.h, r.p.items()[1].id) != "ready", q.substr(0, 400));
            else
                ok("ENGINE: ...this engine's record: \"ready\", nothing for Translate all to do",
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
        r.p.open({ Fm }, true);
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
        r.p.open({ Fm }, true);
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
            r.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
            w.p.open({ W }, true);
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
            r.p.open({ A }, true);
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
            t.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
            r2.p.open({ A }, true);
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
            r3.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
            // a file NOT being analysed (U1: nothing asked -- it was "translation OFF"): only what is heard changes; Translate:
            // the analysis goes to that track
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.probeMap[pbtrf::lowerW(A)] = def2;
            r.p.open({ A }, false);
            const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
            const size_t files = r.h.files.size();
            r.p.setAudio(0, false);
            ok("a file not being analysed: a switch (to a track with no analysis) is NO new generation and no job -- only audioOrd moves",
               files == 0 && fn(r.h.lastOf("translatorFile"), L"jobId") == gen && r.h.files.size() == files
               && fn(r.h.lastOf("translatorFile"), L"audioOrd") == 0);
            r.p.pos(static_cast<uint64_t>(gen), 5000, 600000, false, "pause");
            r.p.analyse(false);
            ok("...Translate: a new generation on the track HEARD (ord 0), FILE audio \"\", focus at the playhead",
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
            r.p.open({ A }, true);
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
            r.p.open({ Fm }, true);
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
            r.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
                r.p.open({ A }, true);
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
                r.p.open({ A }, true);
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
                r.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
            // a file NOT being analysed (U1; it was "translation OFF"): only audioOrd moves; Translate: the analysis on track 0
            Rig o;
            o.h.addFile(A, 5000, 11);
            o.h.probeMap[pbtrf::lowerW(A)] = def2;
            o.p.open({ A }, false);
            const long long g0 = fn(o.h.lastOf("translatorFile"), L"jobId");
            o.p.playFailed(static_cast<uint64_t>(fn(o.h.lastOf("translatorFile"), L"seq")));
            ok("WHP-2 a file not being analysed: audioOrd 0, no job, no new generation", o.h.files.empty() && fn(o.h.lastOf("translatorFile"), L"audioOrd") == 0
               && fn(o.h.lastOf("translatorFile"), L"jobId") == g0);
            o.p.analyse(false);
            ok("WHP-2: ...Translate: the job on track 0 (audio \"\")", o.h.files.size() == 1 && o.h.files[0].audio.empty());
            // the probe answering AFTER the proxy request: its default rule does not win over the proxy
            Rig l;
            l.h.autoProbe = false;
            l.h.addFile(A, 5000, 11);
            l.p.open({ A }, true);
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
            r.p.open({ A }, true);
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
                r.p.open({ c.path }, true);
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
                r.p.open({ P4 }, true);
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
                r.p.open({ P4, B }, true);
                r.p.queueOp("play", r.p.items()[1].id, 0, 0);           // away (its job cancelled; the probe stays known)
                r.h.cache[k] = oldRecord(P4, false);                   // (as it was: the cancelled job wrote nothing yet)
                const size_t files0 = r.h.files.size(), probes0 = static_cast<size_t>(r.h.probes);
                r.p.queueOp("play", r.p.items()[0].id, 0, 0);           // back: no new probe -- known
                const std::wstring st = r.h.lastOf("translatorFile");
                r.p.analyse(false);                                     // (U1: a file played again is analysed when asked)
                ok("reopened with its probe KNOWN: no probe asked, and the 2-track MP4's pre-WH record is a MISS at load (count 0; Translate: FILE, no `have`)",
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
                r.p.open({ P4 }, true);
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
                r.p.open({ P4 }, true);
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
            // Translate all (U1: it was the pre-analysis): the next container is PROBED before it is analysed, and analysed on its
            // own choice
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.addFile(B);
            r.h.probeMap[pbtrf::lowerW(A)] = single;
            r.h.probeMap[pbtrf::lowerW(B)] = def2;
            r.p.open({ A }, true);
            r.p.add({ B });
            const uint64_t job = r.h.files.back().jobId;
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(job, "ok"), false);
            r.p.analyseAll();
            ok("Translate all, the next item (a .mkv): probed first, then its FILE on ITS default track (\"1:3\")",
               r.h.probes == 2 && r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].audio == "1:3",
               r.h.files.size() > 1 ? r.h.files[1].audio : "nothing for Translate all");
            r.p.queueOp("next", 0, 0, 0);
            ok("...switching to it ADOPTS that job (same track): no new FILE", r.h.files.size() == 2);
        }
    }

    // ---- 18. SUVTITLE LIVE (1.1.0): THE LIVE GATE (the study section 2.8) ---------------------------------------------
    std::printf("[18] LIVE: the gate -- no FILE while live, the job cancelled then resumed with `have`, never failed by it\n");
    {
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, true);
        const uint64_t job = r.h.files.back().jobId;
        const long long gen = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.onProg(prog(job, "loading", 600000, ""));
        r.p.onCue(cue(job, 0, 1000, 2500));
        r.p.onProg(prog(job, "analyzing", 600000, "0-30000"));
        r.p.onCue(cue(job, 1, 31000, 32000));
        r.p.onProg(prog(job, "analyzing", 600000, "0-60000"));
        const size_t files0 = r.h.files.size();
        r.p.setLive(true);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("the gate closes: the running job is CANCELLED (FCANCEL), no FILE, phase \"live\" (a notice, not an error)",
           !r.h.cancels.empty() && r.h.cancels.back() == job && r.h.files.size() == files0 && fs(st, L"phase") == "live"
           && fs(st, L"err").empty() && !fb(st, L"cancelling"), a(st).substr(0, 300));
        r.p.onDone(done(job, "cancelled"), false);
        r.p.pos(static_cast<uint64_t>(gen), 45000, 600000, true, "seek");
        r.h.now += 5000;
        r.p.tick();
        r.p.cancel(static_cast<uint64_t>(gen));             // (U1: Cancel drops the held wish...)
        r.p.analyse(false);                                 // (...and Translate asks again: still held)
        ok("while live NOTHING sends a FILE: the job's own FDONE, a seek, a tick, Cancel then Translate (U1: it was translation off/on)",
           r.h.files.size() == files0 && fs(r.h.lastOf("translatorFile"), L"phase") == "live", n2s(r.h.files.size()));
        r.p.analyse(false);
        ok("...nor \"Analyse again\" (the strip keeps saying why: live)",
           r.h.files.size() == files0 && fs(r.h.lastOf("translatorFile"), L"phase") == "live");
        r.p.setLive(false);
        ok("the gate opens: ONE FILE, resuming with have = the VOUCHED part (0-60000: the helper's own FDONE vouched the last report)",
           r.h.files.size() == files0 + 1 && r.h.files.back().have == "0-60000" && r.h.files.back().path == a(A),
           r.h.files.size() > files0 ? r.h.files.back().have : "no FILE");
        ok("...and the strip analyses again", fs(r.h.lastOf("translatorFile"), L"phase") == "loading");
        ok("every post was well-formed JSON", r.h.badJson == 0, r.h.lastBadJson);
    }
    {
        // a FILE that crossed the LIVE on the wire: the engine refuses it with an FDONE "error" -- never `failed`
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, true);
        const uint64_t j1 = r.h.files.back().jobId;
        r.p.setLive(true);
        r.p.onDone(done(j1, "error", "Live translation is on: files are analysed when it stops."), false);
        const std::wstring st = r.h.lastOf("translatorFile");
        const std::wstring item = r.h.lastOf("playerItem");
        ok("an FDONE error received while live: NOT failed (phase live, no err, the badge not an error)",
           fs(st, L"phase") == "live" && fs(st, L"err").empty() && fs(item, L"state") != "error", a(st).substr(0, 200) + " | " + a(item));
        r.p.setLive(false);
        ok("...the gate opens: the item is analysed (it was waiting, not failed)", r.h.files.size() == 2 && r.h.files.back().path == a(A));
        // the gate opens BEFORE the cancelled job's FDONE arrives -- and that FDONE says "error": still not failed
        Rig q;
        q.h.addFile(A, 5000, 11);
        q.p.open({ A }, true);
        const uint64_t k1 = q.h.files.back().jobId;
        q.p.setLive(true);
        q.p.setLive(false);
        ok("(open again before the dying job answered: no second FILE yet)", q.h.files.size() == 1);
        q.p.onDone(done(k1, "error", "Live translation is on: files are analysed when it stops."), false);
        ok("a job the gate CANCELLED answering \"error\" after the gate opened: not failed, the item resumes at once",
           fs(q.h.lastOf("translatorFile"), L"phase") == "loading" && q.h.files.size() == 2, fs(q.h.lastOf("translatorFile"), L"phase"));
        ok("...whereas a REAL error with the gate open still fails the item (the rule is the live mode's only)", [&] {
            q.p.onDone(done(q.h.files.back().jobId, "error", "decoder choked"), false);
            return fs(q.h.lastOf("translatorFile"), L"phase") == "error";
        }());
    }
    {
        // "Translate all" (U1: it was the pre-analysis): closed gate = none; a batch job's error while live is not remembered
        // as a failure
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
        r.p.open({ A, B }, true);
        r.p.windowReady();
        const uint64_t ja = r.h.files.back().jobId;
        r.p.onProg(prog(ja, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(ja, "ok"), false);
        r.p.analyseAll();
        ok("(Translate all: the next item is analysed)", r.h.files.size() == 2 && r.h.files[1].path == a(B));
        const uint64_t jb = r.h.files[1].jobId;
        r.p.setLive(true);
        ok("the gate closes: the BATCH job is cancelled too, and the complete current item says done (not live)",
           r.h.cancels.back() == jb && fs(r.h.lastOf("translatorFile"), L"phase") == "done");
        r.p.onDone(done(jb, "error", "Live translation is on: files are analysed when it stops."), false);
        r.h.now += 5000;
        r.p.tick();
        ok("...nothing of the batch while live (a tick, the batch job's refusal) -- its row still says queued",
           r.h.files.size() == 2 && badgeNow(r.h, r.p.items()[1].id) == "queued", badgeNow(r.h, r.p.items()[1].id));
        r.p.setLive(false);
        ok("the gate opens: Translate all goes on with the next item (its live refusal was not remembered as a failure)",
           r.h.files.size() == 3 && r.h.files[2].path == a(B));
    }
    {
        // a film opened during the live plays with the subtitles already made; its SOUND copy is still made (FPROXY)
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(X, 77, 99);
        r.p.setLive(true);
        r.p.open({ A }, true);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("opened during the live: no FILE, phase live, it may start playing (autoplay: the page does not wait)",
           r.h.files.empty() && fs(st, L"phase") == "live" && fb(st, L"autoplay"), a(st).substr(0, 200));
        r.p.playFailed(static_cast<uint64_t>(fn(st, L"seq")));
        ok("...a sound-only copy is still asked for (FPROXY is not a file analysis)", r.h.proxies.size() == 1 && r.h.files.empty());
        r.p.setLive(false);
        ok("...and analysed once the live stops (from nothing: have empty)", r.h.files.size() == 1 && r.h.files[0].have.empty());
        ok("every post was well-formed JSON", r.h.badJson == 0, r.h.lastBadJson);
    }

    // ---- U1 (02/11/2026, the user's requests): NOTHING ANALYSED UNLESS ASKED -- Translate (this file), Translate all, Cancel all -----
    std::printf("[U1] nothing analysed unless asked: Translate (this file), Translate all, Cancel all\n");
    {
        // 1. a file opened (or played next) is a PLAIN PLAYER: no FILE, phase off, translate false -- a seek, a tick: nothing
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
        r.p.open({ A, B }, false);
        const std::wstring st = r.h.lastOf("translatorFile");
        const long long gen = fn(st, L"jobId");
        r.p.pos(static_cast<uint64_t>(gen), 30000, 600000, true, "seek");
        r.h.now += 5000; r.p.tick();
        ok("U1: a file OPENED is played, not analysed: no FILE (a seek, a tick: still none), phase \"off\", translate false",
           r.h.files.empty() && fs(st, L"phase") == "off" && !fb(st, L"translate") && fs(r.h.lastOf("translatorFile"), L"phase") == "off",
           a(st).substr(0, 200));
        r.p.queueOp("next", 0, 0, 0);
        r.h.now += 5000; r.p.tick();
        ok("U1: ...the NEXT file played: nothing either (no pre-analysis, no automatic start)",
           r.h.files.empty() && fs(r.h.lastOf("translatorFile"), L"phase") == "off");
        // 2. Translate = THIS file, at the playhead; leaving it cancels its job (only the batch analyses a file that is not
        //    playing) and the next starts nothing; back to it, nothing starts by itself (the press was for it while it played)
        r.p.queueOp("play", r.p.items()[0].id, 0, 0);
        const long long gA = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.pos(static_cast<uint64_t>(gA), 42000, 600000, false, "pause");
        r.p.analyse(false);
        ok("U1: Translate = THIS file: one FILE, at the playhead (focus 0:42), the file asked for (translate), loading",
           r.h.files.size() == 1 && r.h.files[0].path == a(A) && r.h.files[0].focusMs == 42000 && fb(r.h.lastOf("translatorFile"), L"translate")
           && fs(r.h.lastOf("translatorFile"), L"phase") == "loading");
        const uint64_t jA = r.h.files[0].jobId;
        r.p.queueOp("next", 0, 0, 0);
        ok("U1: ...another file played: the pressed one's job is CANCELLED, the new one starts nothing",
           !r.h.cancels.empty() && r.h.cancels.back() == jA && r.h.files.size() == 1);
        r.p.onDone(done(jA, "cancelled"), false);
        r.p.queueOp("prev", 0, 0, 0);
        r.h.now += 5000; r.p.tick();
        ok("U1: ...back to it: nothing starts by itself (a new opening is a new session: not asked)",
           r.h.files.size() == 1 && fs(r.h.lastOf("translatorFile"), L"phase") == "off");
        ok("every post well-formed", r.h.badJson == 0, r.h.lastBadJson);
    }
    {
        // 3. a film ALREADY analysed: its cache SHOWN with no press -- complete (done) or partial ("off", its cues) -- no FILE
        Rig r;
        r.p.setPair("fr", "en", "auto");
        r.h.addFile(Fm, 7000, 13);
        r.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, true);
        r.p.open({ Fm }, false);
        const std::wstring st = r.h.lastOf("translatorFile");
        ok("U1: a film ALREADY analysed (complete): shown with no press -- phase done, its cues posted, no FILE",
           r.h.files.empty() && fs(st, L"phase") == "done" && fn(st, L"count") == 2
           && a(r.h.lastOf("translatorFileCues")).find("Vieux un.") != std::string::npos, a(st).substr(0, 200));
        Rig q;
        q.p.setPair("fr", "en", "auto");
        q.h.addFile(Fm, 7000, 13);
        q.h.cache[keyFrEn] = engRecord(Fm, "fr", "en", kCur, false, false);
        q.p.open({ Fm }, false);
        const std::wstring sq = q.h.lastOf("translatorFile");
        ok("U1: ...a PARTIAL one too: phase off (nothing asked), its cues posted, half of it analysed, no FILE",
           q.h.files.empty() && fs(sq, L"phase") == "off" && fn(sq, L"count") == 2 && fn(sq, L"doneMs") == 300000
           && a(q.h.lastOf("translatorFileCues")).find("Vieux deux.") != std::string::npos, a(sq).substr(0, 200));
        q.p.analyse(false);
        ok("U1: ...Translate finishes it: FILE with have = what it holds (0-300000), the same generation (the cues on screen stay)",
           q.h.files.size() == 1 && q.h.files[0].have == "0-300000" && fn(q.h.lastOf("translatorFile"), L"jobId") == fn(sq, L"jobId"),
           q.h.files.empty() ? "no FILE" : q.h.files[0].have);
    }
    {
        // 4. TRANSLATE ALL: the playing file FIRST, then the LIST's order, one job at a time; its rows; a failure; a file added
        //    after it; the press puts that one first; CANCEL ALL; Translate all again
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
        r.p.add({ A, B, C });
        r.p.queueOp("play", r.p.items()[1].id, 0, 0);
        ok("U1 (setup): a queue of three, Ep 2 playing, nothing analysed", r.h.files.empty());
        const uint64_t idA = r.p.items()[0].id, idB = r.p.items()[1].id, idC = r.p.items()[2].id;
        r.p.analyseAll();
        ok("U1: TRANSLATE ALL: the file PLAYING first (Ep 2), the others QUEUED -- one job",
           r.h.files.size() == 1 && r.h.files[0].path == a(B) && badgeNow(r.h, idA) == "queued" && badgeNow(r.h, idC) == "queued"
           && fb(r.h.lastOf("translatorFile"), L"translate"), badgeNow(r.h, idA) + "/" + badgeNow(r.h, idC));
        const uint64_t jB = r.h.files[0].jobId;
        r.p.onProg(prog(jB, "analyzing", 600000, "0-600000"));
        r.p.onDone(done(jB, "ok"), false);
        ok("U1: ...Ep 2 done: then the LIST's order -- Ep 1 (above it), from its start; the rows say done / analysing / queued",
           r.h.files.size() == 2 && r.h.files[1].path == a(A) && r.h.files[1].focusMs == 0 && badgeNow(r.h, idB) == "ready"
           && badgeNow(r.h, idA) == "analysing" && badgeNow(r.h, idC) == "queued");
        const uint64_t jA = r.h.files[1].jobId;
        r.p.onDone(done(jA, "error", "bad file"), false);
        ok("U1: ...Ep 1 FAILS: its row says so (not retried), Ep 10 goes next", r.h.files.size() == 3 && r.h.files[2].path == a(C)
           && badgeNow(r.h, idA) == "error", badgeNow(r.h, idA));
        const uint64_t jC = r.h.files[2].jobId;
        r.p.onProg(prog(jC, "analyzing", 700000, "0-100000"));
        const std::wstring D = L"D:\\Films\\Season 1\\Ep 11.mkv";
        r.h.addFile(D, 8000, 14);
        r.p.add({ D });
        const uint64_t idD = r.p.items()[3].id;
        r.p.queueOp("play", idD, 0, 0);
        ok("U1: a file added AFTER Translate all, played: not in the batch -- it starts nothing, Ep 10's job goes on",
           r.h.files.size() == 3 && (r.h.cancels.empty() || r.h.cancels.back() != jC) && fs(r.h.lastOf("translatorFile"), L"phase") == "off");
        r.p.analyse(false);
        ok("U1: ...Translate on it: it goes FIRST -- its FILE, the batch's job yields (FCANCEL)",
           r.h.files.size() == 4 && r.h.files[3].path == a(D) && !r.h.cancels.empty() && r.h.cancels.back() == jC);
        r.p.onDone(done(jC, "cancelled"), false);
        ok("U1: ...Ep 10's row: queued again (what it has is kept)", badgeNow(r.h, idC) == "queued", badgeNow(r.h, idC));
        const uint64_t jD = r.h.files[3].jobId;
        r.p.onDone(done(jD, "ok"), false);
        ok("U1: ...Ep 11 done: the batch takes Ep 10 back, RESUMED (have 0-100000)", r.h.files.size() == 5 && r.h.files[4].path == a(C)
           && r.h.files[4].have == "0-100000", r.h.files.size() > 4 ? r.h.files[4].have : "");
        const uint64_t jC2 = r.h.files[4].jobId;
        r.p.cancelAll();
        ok("U1: CANCEL ALL: the running job cancelled (FCANCEL), its row says cancelled; the file playing (complete) untouched",
           !r.h.cancels.empty() && r.h.cancels.back() == jC2 && badgeNow(r.h, idC) == "cancelled" && fs(r.h.lastOf("translatorFile"), L"phase") == "done",
           badgeNow(r.h, idC));
        r.p.onDone(done(jC2, "cancelled"), false);
        r.h.now += 5000; r.p.tick();
        ok("U1: ...and nothing starts again by itself (the job's FDONE, a tick)", r.h.files.size() == 5 && badgeNow(r.h, idC) == "cancelled");
        r.p.analyseAll();
        ok("U1: TRANSLATE ALL again: the failed AND the cancelled are asked again -- the LIST's order: Ep 1 first",
           r.h.files.size() == 6 && r.h.files[5].path == a(A) && badgeNow(r.h, idC) == "queued");
        ok("every post well-formed", r.h.badJson == 0, r.h.lastBadJson);
    }
    {
        // 5. Cancel on the playing file during the batch: ITS analysis stops (cancelled), the batch goes on with the next one
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
        r.p.open({ A, B }, false);
        r.p.analyseAll();
        const uint64_t jA = r.h.files.back().jobId;
        const long long gA = fn(r.h.lastOf("translatorFile"), L"jobId");
        r.p.cancel(static_cast<uint64_t>(gA));
        r.p.onDone(done(jA, "cancelled"), false);
        ok("U1: Cancel on the playing file during Translate all: its job stops, its row says cancelled, the NEXT file goes",
           r.h.files.size() == 2 && r.h.files[1].path == a(B) && badgeNow(r.h, r.p.items()[0].id) == "cancelled"
           && fs(r.h.lastOf("translatorFile"), L"phase") == "cancelled");
        // ...and a file removed from the queue leaves the batch (its job cancelled)
        const uint64_t jB = r.h.files[1].jobId;
        r.p.queueOp("remove", r.p.items()[1].id, 0, 0);
        ok("U1: a queued file REMOVED from the list: its batch job is cancelled (FCANCEL), nothing replaces it",
           !r.h.cancels.empty() && r.h.cancels.back() == jB && r.h.files.size() == 2);
    }
    {
        // 6. the director's rule (N2's mpv track adoption, kept): an analysis ALREADY running on the track mpv chose is never
        //    restarted by the adoption -- U1 changed only WHO starts it (Translate), not what the adoption does to it
        Rig r;
        r.h.mpvOn = true;
        const std::wstring V = L"D:\\Films\\clip.avi";
        r.h.addFile(V, 9000, 15);
        r.p.open({ V }, false);
        const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
        r.p.analyse(false);
        ok("U1 x L6: Translate on an AVI that mpv plays, mpv's track list not known yet: the job WAITS for it (no FILE)", r.h.files.empty());
        r.h.now += 4000; r.p.tick();                         // the wait runs out (kProbeWaitMs): the job on "" (aid=auto's pick)
        ok("...the wait runs out: the job starts on \"\" (mpv's own pick)", r.h.files.size() == 1 && r.h.files[0].audio.empty());
        auto info = std::make_shared<pbmedia::Info>();
        info->state = pbmedia::Info::State::Ready; info->container = "mpv"; info->hasVideo = true;
        pbmedia::AudioTrack t0; t0.ord = 0; t0.track = 1; t0.lang = "eng"; t0.codec = "mp3"; t0.ch = 2; t0.def = true; t0.play = true;
        pbmedia::AudioTrack t1 = t0; t1.ord = 1; t1.track = 2; t1.lang = "fre"; t1.def = false;
        info->audio = { t0, t1 };
        const size_t cancels0 = r.h.cancels.size();
        r.p.offerMpvTracks(seq, info, 0, true);              // mpv: it plays track 1 of 2 (ord 0) -- what the job decodes
        ok("...mpv's list comes late and its choice IS the track the job decodes: the job is NOT restarted (no FCANCEL, no new FILE)",
           r.h.files.size() == 1 && r.h.cancels.size() == cancels0, n2s(r.h.files.size()));
        ok("...the set is NAMED after it (a FILE sent again asks for it by id)", r.p.session().set && r.p.session().set->audio.trackId == 1);
        ok("every post well-formed", r.h.badJson == 0, r.h.lastBadJson);
    }
    {
        // 7. a QUEUED file left mid-analysis for one nobody asked for: its job goes ON as the batch's (no FCANCEL, no model
        //    reload) -- it yields only to a file that is asked for
        Rig r;
        r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
        r.p.open({ A }, false);
        r.p.analyseAll();
        const uint64_t jA = r.h.files.back().jobId, idA = r.p.items()[0].id;
        r.p.onProg(prog(jA, "analyzing", 600000, "0-60000"));
        r.p.add({ B });
        r.p.queueOp("play", r.p.items()[1].id, 0, 0);
        ok("U1: a QUEUED file left mid-analysis for one nobody asked for: its job goes ON (no FCANCEL, no new FILE), its row analysing",
           (r.h.cancels.empty() || r.h.cancels.back() != jA) && r.h.files.size() == 1 && badgeNow(r.h, idA) == "analysing"
           && fs(r.h.lastOf("translatorFile"), L"phase") == "off", badgeNow(r.h, idA));
        r.p.analyse(false);
        ok("U1: ...Translate on the file playing: it takes the engine (the batch's job yields: FCANCEL)",
           !r.h.cancels.empty() && r.h.cancels.back() == jA && r.h.files.size() == 2 && r.h.files[1].path == a(B));
    }

    // ---- U2 (02/11/2026, "Import model..."): THE USER'S OWN MODELS in the core -- and the user's rule for Cancel all ----------
    std::printf("[U2] the user's own models (the FILE names them, their own cache key, the fall back); Cancel all = the batch only\n");
    {
        using MP = Player::ModelPick;
        const std::string mAsr = "D:\\Models\\ggml-base.bin", mMt = "E:\\LLM\\qwen2.5-1.5b-instruct-q4_k_m.gguf";
        const std::string idAsr = pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 148000000, 77);
        const std::string idMt = pbtrf::modelIdent(L"E:\\LLM\\qwen2.5-1.5b-instruct-q4_k_m.gguf", 1100000000, 78);
        const std::string tagAsr = pbtrf::modelsTag(idAsr, "");
        auto completeRecord = [&](const std::wstring& path, uint64_t size, uint64_t mtime, const char* line) {
            pbtrf::Record rec;
            rec.path = a(path); rec.size = size; rec.mtime = mtime; rec.langA = "en"; rec.langB = "fr"; rec.mode = "auto"; rec.quality = "auto";
            rec.complete = true; rec.totalMs = 600000; rec.engine = tp::kFileEngineVersion;
            rec.ranges.push_back(tp::MsRange{ 0, 600000 });
            rec.cues.push_back(cue(1, 0, 1000, 2000, line, line));
            return pbtrf::packRecord(rec);
        };
        const std::string errAsr = std::string(tp::kFileModelErrAsr) + "ggml-base.bin\" could not be used: the speech recogniser would not start (GPU or CPU).";
        const std::string errMt = std::string(tp::kFileModelErrMt) + "qwen2.5-1.5b-instruct-q4_k_m.gguf\" could not be used: the translation model would not start (GPU or CPU).";
        {
            // 1. the DEFAULT: a film analysed before U2 is shown as it was (the pre-U2 key); an imported model chosen = nothing starts,
            //    the subtitles on screen stay, flagged; Translate = the imported model's OWN analysis; back to the default = its record
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.h.cache[pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto")] = completeRecord(A, 5000, 11, "DEFAULT");
            r.p.open({ A }, false);
            ok("U2: the DEFAULT models: a film analysed before U2 shows its subtitles as they were (the pre-U2 key, nothing re-analysed)",
               r.p.session().set && r.p.session().set->complete && r.h.files.empty() && !fb(r.h.lastOf("translatorFile"), L"modelsStale"));
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            ok("U2: an imported model chosen: nothing starts, the subtitles on screen stay -- said made with another model (modelsStale)",
               r.h.files.empty() && r.p.session().set->complete && fb(r.h.lastOf("translatorFile"), L"modelsStale") && r.p.modelsTag() == tagAsr);
            r.p.analyse(false);
            ok("U2: Translate then: ONE FILE naming the imported model (its absolute path; the MT field empty = the default), a set of "
               "its OWN key -- the default's record is not taken for it",
               r.h.files.size() == 1 && r.h.files[0].asrModel == mAsr && r.h.files[0].mtModel.empty()
               && r.p.session().set->key == pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 0, tagAsr)
               && !r.p.session().set->complete && r.p.session().set->cues.empty() && !fb(r.h.lastOf("translatorFile"), L"modelsStale"));
            const uint64_t j1 = r.h.files[0].jobId;
            r.p.setModels(MP{}, MP{});
            r.p.analyse(false);
            ok("U2: back to the default + Translate: the imported model's job stops (FCANCEL), the default's COMPLETE record is shown "
               "again -- no FILE",
               r.h.files.size() == 1 && !r.h.cancels.empty() && r.h.cancels.back() == j1 && r.p.session().set->complete
               && r.p.session().set->key == pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto"));
            ok("every post well-formed", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // 2. both models: the FILE names both; the TRANSLATION model fails to load -> the strip says the engine's sentence and
            //    that SuVtitle goes back to its own; THAT kind back to the default (the other kept); the host told once (persist +
            //    menu); the next press runs the default MT
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.p.setModels(MP{ mAsr, idAsr }, MP{ mMt, idMt });
            r.p.open({ A }, false);
            r.p.analyse(false);
            ok("U2: both models chosen: the FILE names both (absolute paths)",
               r.h.files.size() == 1 && r.h.files[0].asrModel == mAsr && r.h.files[0].mtModel == mMt);
            r.p.onDone(done(r.h.files[0].jobId, "error", errMt.c_str()), false);
            const std::wstring st = r.h.lastOf("translatorFile");
            // (U2b re-point: the host's `say` replaced `onScreen` -- on screen and pressed, the STRIP says it: no notice)
            ok("U2: an imported TRANSLATION model fails: the strip says the engine's sentence AND that SuVtitle goes back to its own "
               "translation model; the host told once (mt, the path, on screen: no notice of its own)",
               fs(st, L"phase") == "error" && fs(st, L"err").find("would not start") != std::string::npos
               && fs(st, L"err").find("goes back to its own translation model") != std::string::npos
               && r.h.fells.size() == 1 && r.h.fells[0].kind == "mt" && r.h.fells[0].path == mMt && !r.h.fells[0].say
               && r.h.fells[0].redone.empty(), fs(st, L"err"));
            ok("U2: ...the TRANSLATION model back to the default, the transcription one kept",
               r.p.mtModel().path.empty() && r.p.asrModel().path == mAsr && r.p.modelsTag() == tagAsr);
            r.p.analyse(false);
            ok("U2: ...the next Translate: the FILE names the imported transcription model and NO translation model (the default)",
               r.h.files.size() == 2 && r.h.files[1].asrModel == mAsr && r.h.files[1].mtModel.empty());
        }
        {
            // 3. a failure that is not an imported model's changes no choice; an imported model failing AFTER the user moved away
            //    from it: his choice stands
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.open({ A }, false);
            r.p.analyse(false);
            r.p.onDone(done(r.h.files.back().jobId, "error", "The file could not be opened."), false);
            ok("U2: a failure that is NOT an imported model's (the file): the choice stands, the host is not told",
               r.p.asrModel().path == mAsr && r.h.fells.empty());
            r.p.analyse(false);
            const uint64_t j2 = r.h.files.back().jobId;
            const std::string mAsr2 = "D:\\Models\\ggml-small.bin", idAsr2 = pbtrf::modelIdent(L"D:\\Models\\ggml-small.bin", 487000000, 79);
            r.p.setModels(MP{ mAsr2, idAsr2 }, MP{});
            r.p.onDone(done(j2, "error", errAsr.c_str()), false);
            ok("U2: an imported model failing after the user picked ANOTHER one: his choice stands (ggml-small), the host told (it "
               "forgets the choice only if it still is that model)",
               r.h.files.size() == 2 && r.p.asrModel().path == mAsr2 && r.h.fells.size() == 1 && r.h.fells[0].kind == "asr"
               && r.h.fells[0].path == mAsr);
        }
        {
            // 3b. a PARTIAL default analysis on screen (no job), an imported model chosen, Translate: the press is for the imported
            //     model -- a set of ITS key from nothing, its FILE naming it (never the default's partial set resumed)
            Rig r;
            r.h.addFile(B, 6000, 12);
            pbtrf::Record part;
            part.path = a(B); part.size = 6000; part.mtime = 12; part.langA = "en"; part.langB = "fr"; part.mode = "auto"; part.quality = "auto";
            part.complete = false; part.totalMs = 600000; part.engine = tp::kFileEngineVersion;
            part.ranges.push_back(tp::MsRange{ 0, 60000 });
            part.cues.push_back(cue(1, 0, 1000, 2000, "PART", "PART"));
            r.h.cache[pbtrf::cacheKey(B, 6000, 12, "en", "fr", "auto", "auto")] = pbtrf::packRecord(part);
            r.p.open({ B }, false);
            const bool shown = r.p.session().set && !r.p.session().set->cues.empty();
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.analyse(false);
            ok("U2: a PARTIAL default analysis on screen, an imported model chosen, Translate: the FILE names the imported model and "
               "starts from nothing (no `have`: the default's ranges are not its), a set of its own key",
               shown && r.h.files.size() == 1 && r.h.files[0].asrModel == mAsr && r.h.files[0].have.empty()
               && r.p.session().set->key == pbtrf::cacheKey(B, 6000, 12, "en", "fr", "auto", "auto", 0, tagAsr),
               r.h.files.empty() ? std::string("no FILE") : r.h.files[0].have);
        }
        {
            // 3c. the models changed while "Translate all" runs on a file NOT playing: its job stops and starts again on the new
            //     choice (as a pair change does)
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.h.cache[pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto")] = completeRecord(A, 5000, 11, "DONE");
            r.p.open({ A, B }, false);
            r.p.analyseAll();
            const uint64_t jB = r.h.files.back().jobId;
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            ok("U2: the models changed while Translate all runs on a file not playing: its job stops (FCANCEL), the file starts again "
               "naming the new model",
               !r.h.cancels.empty() && r.h.cancels.back() == jB && r.h.files.size() == 2 && r.h.files[1].path == a(B)
               && r.h.files[1].asrModel == mAsr);
        }
        {
            // 4. "Translate all" on an imported model: the playing file has that model's analysis (complete: skipped); a file NOT on
            //    screen fails on the model -> the host told NOT on screen (its notice), the default from now on, the batch goes on
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
            r.h.cache[pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto", 0, tagAsr)] = completeRecord(A, 5000, 11, "MINE");
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.open({ A, B, C }, false);
            ok("U2: the playing film's analysis made WITH the imported model is found under its own key (shown, complete)",
               r.p.session().set && r.p.session().set->complete && !fb(r.h.lastOf("translatorFile"), L"modelsStale"));
            r.p.analyseAll();
            ok("U2: Translate all: the next file's FILE names the imported model", r.h.files.size() == 1 && r.h.files[0].path == a(B)
               && r.h.files[0].asrModel == mAsr);
            r.p.onDone(done(r.h.files[0].jobId, "error", errAsr.c_str()), false);
            // (U2b re-point: the user's rule of 02/11 -- the batch REDOES that file once with the default model before it goes on)
            ok("U2: ...its model fails on a file NOT on screen: the host told so (said in a notice, the file redone), the default from "
               "now on, and the batch REDOES that file on the default -- naming no model -- before the NEXT one (U2b)",
               r.h.fells.size() == 1 && r.h.fells[0].say && r.h.fells[0].redone == L"Ep 2.mkv" && r.p.asrModel().path.empty()
               && r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].asrModel.empty());
            r.p.onDone(done(r.h.files[1].jobId, "ok"), false);
            ok("U2: ...then the NEXT file, on the default too (U2b re-point)",
               r.h.files.size() == 3 && r.h.files[2].path == a(C) && r.h.files[2].asrModel.empty());
            ok("every post well-formed", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // 5. THE USER'S RULE (02/11): Cancel all cancels ONLY what the list launched. The playing file PRESSED (Translate) and
            //    also queued by Translate all: its job goes on (the strip's Cancel is its); the files the list queued: cancelled
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
            r.p.open({ A, B, C }, false);
            r.p.analyse(false);
            const uint64_t jA = r.h.files.back().jobId;
            r.p.analyseAll();
            const size_t c0 = r.h.cancels.size(), f0 = r.h.files.size();
            r.p.cancelAll();
            ok("U2 RULE: Cancel all with the PLAYING file pressed: its job goes on (no FCANCEL), its row not cancelled; the files the "
               "list queued: cancelled",
               r.h.cancels.size() == c0 && r.h.files.size() == f0 && badgeNow(r.h, r.p.items()[0].id) != "cancelled"
               && badgeNow(r.h, r.p.items()[1].id) == "cancelled" && badgeNow(r.h, r.p.items()[2].id) == "cancelled"
               && fs(r.h.lastOf("translatorFile"), L"phase") != "cancelled" && fb(r.h.lastOf("translatorFile"), L"translate"),
               badgeNow(r.h, r.p.items()[0].id) + " " + fs(r.h.lastOf("translatorFile"), L"phase"));
            r.p.onDone(done(jA, "ok"), false);
            r.h.now += 5000; r.p.tick();
            ok("U2 RULE: ...the pressed file finishes, and nothing of the list starts again by itself", r.h.files.size() == f0);
        }
        {
            // 6. the playing file LAUNCHED BY the list (not pressed): Cancel all stops it too; the strip's Cancel would have as well
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.p.open({ A, B }, false);
            r.p.analyseAll();
            const uint64_t jA = r.h.files.back().jobId;
            r.p.cancelAll();
            r.p.onDone(done(jA, "cancelled"), false);
            ok("U2 RULE: the PLAYING file launched by Translate all: Cancel all stops it (FCANCEL), both rows cancelled, the strip too",
               !r.h.cancels.empty() && r.h.cancels.back() == jA && badgeNow(r.h, r.p.items()[0].id) == "cancelled"
               && badgeNow(r.h, r.p.items()[1].id) == "cancelled" && fs(r.h.lastOf("translatorFile"), L"phase") == "cancelled",
               fs(r.h.lastOf("translatorFile"), L"phase"));
        }
        {
            // 7. a batch job of a file NOT playing while the playing file was pressed and is complete: Cancel all stops the batch's
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.h.cache[pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto")] = completeRecord(A, 5000, 11, "DONE");
            r.p.open({ A, B }, false);
            r.p.analyseAll();
            const uint64_t jB = r.h.files.back().jobId;
            r.p.cancelAll();
            ok("U2 RULE: the batch's job of a file not playing: Cancel all stops it (FCANCEL), its row cancelled; the playing file "
               "(complete) untouched",
               r.h.files.size() == 1 && r.h.files[0].path == a(B) && !r.h.cancels.empty() && r.h.cancels.back() == jB
               && badgeNow(r.h, r.p.items()[1].id) == "cancelled" && fs(r.h.lastOf("translatorFile"), L"phase") == "done");
        }
    }

    // ---- U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md): two wishes on ONE analysis, the voice pass, the Word file,
    //      the names, the source timecode, "Transcribe all", the Cancels -- whisper runs once whatever is pressed
    std::printf("[U3] the transcript: Translate / Transcribe on one analysis, FVOICE, the Word file, names, timecodes, Transcribe all\n");
    {
        // the transcript badge the window shows NOW for item `id` (the newest playerItem / playerQueue naming it)
        auto tbadgeNow = [](const FakeHost& h, uint64_t id) -> std::string {
            const std::string idk = "\"id\":" + std::to_string(id) + ",";
            for (auto it = h.window.rbegin(); it != h.window.rend(); ++it) {
                const std::string t = typeOf(*it);
                if (t != "playerItem" && t != "playerQueue") continue;
                const std::string s = a(*it);
                const size_t at = s.find(idk);
                if (at == std::string::npos) continue;
                const size_t st = s.find("\"tstate\":\"", at);
                if (st == std::string::npos) continue;
                const size_t b = st + 10, e = s.find('"', b);
                return e == std::string::npos ? std::string("<bad>") : s.substr(b, e - b);
            }
            return "<none>";
        };
        auto vprog = [](uint64_t job, uint64_t doneMs, uint64_t totalMs) {
            tp::WireFileProg p;
            p.jobId = job; p.phase = "voices"; p.doneMs = doneMs; p.totalMs = totalMs;
            return p;
        };
        auto spk = [](uint64_t job, uint32_t n, std::vector<uint32_t> labels) {
            tp::WireSpk s;
            s.jobId = job; s.speakers = n; s.labels = std::move(labels);
            s.model = n ? "campplus" : "speaker diarization is off (SPKON 0)";
            return s;
        };
        auto transcriptRecord = [&](const std::wstring& path, uint64_t size, uint64_t mtime) {
            pbtrf::Record rec;
            rec.path = a(path); rec.size = size; rec.mtime = mtime; rec.langA = "en"; rec.langB = "fr"; rec.mode = "auto"; rec.quality = "auto";
            rec.complete = true; rec.totalMs = 600000; rec.engine = tp::kFileEngineVersion;
            rec.ranges.push_back(tp::MsRange{ 0, 600000 });
            rec.cues.push_back(cue(1, 0, 1000, 4000, "Good morning everyone."));
            rec.cues.push_back(cue(1, 1, 5000, 9000, "- I agree. - So let's move on."));
            rec.cues.push_back(cue(1, 2, 20000, 24000, "Thanks for coming."));
            return pbtrf::packRecord(rec);
        };
        const std::string keyT = pbtrf::cacheKey(A, 5000, 11, "en", "fr", "auto", "auto");
        const std::wstring docA = L"D:\\Films\\Season 1\\Ep 1 - transcript.docx";

        // 1. TRANSCRIBE alone, then TRANSLATE joins it: ONE FILE; the text grows; the voice pass; the Word file (Q2)
        Rig r;
        r.h.addFile(A, 5000, 11);
        r.p.open({ A }, false);
        const uint64_t g0 = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"jobId"));
        r.p.pos(g0, 42000, 600000, false, "pause");
        r.p.transcribe(false);
        std::wstring st = r.h.lastOf("translatorFile");
        ok("U3: TRANSCRIBE alone = the ANALYSIS first (one FILE, at the playhead 0:42): transcribe true, translate false, tphase waiting",
           r.h.files.size() == 1 && r.h.files[0].focusMs == 42000 && fb(st, L"transcribe") && !fb(st, L"translate")
           && fs(st, L"tphase") == "waiting" && fs(st, L"phase") == "loading" && r.h.voicesSent.empty(), a(st).substr(0, 300));
        const uint64_t jA = r.h.files[0].jobId;
        r.p.analyse(false);
        st = r.h.lastOf("translatorFile");
        ok("U3: TRANSLATE pressed while it runs JOINS it: still ONE FILE, no FCANCEL, both wished",
           r.h.files.size() == 1 && r.h.cancels.empty() && fb(st, L"translate") && fb(st, L"transcribe"));
        r.p.onProg(prog(jA, "analyzing", 600000, "0-600000"));
        const size_t posts0 = r.h.countOf("translatorTranscript");
        r.p.onCue(cue(jA, 0, 1000, 4000, "Good morning everyone."));
        r.p.flush();                                         // (B9: a drain per cue -- the bridge flushes after each drain)
        r.p.onCue(cue(jA, 1, 5000, 9000, "- I agree. - So let's move on."));
        r.p.flush();
        r.p.onCue(cue(jA, 2, 20000, 24000, "Thanks for coming."));
        r.p.flush();
        r.h.now += 3000;
        r.p.tick();
        const std::wstring tr0 = r.h.lastOf("translatorTranscript");
        ok("U3: while it is analysed the Transcript tab's text GROWS -- three cues, ONE coalesced post (state analysing), the "
           "dialogue cue's two lines without their dashes",
           r.h.countOf("translatorTranscript") == posts0 + 1 && fs(tr0, L"state") == "analysing"
           && a(tr0).find("\"text\":\"Good morning everyone. I agree. So let's move on.\"") != std::string::npos
           && a(tr0).find("- I agree") == std::string::npos, a(tr0).substr(0, 500));
        r.p.onDone(done(jA, "ok"), false);
        ok("U3: the analysis done -> ONE FVOICE on the file: its 4 pieces (the dialogue cue's two lines touching), the second line "
           "a KNOWN change (index 2); still ONE FILE",
           r.h.files.size() == 1 && r.h.voicesSent.size() == 1 && r.h.voicesSent[0].path == a(A)
           && r.h.voicesSent[0].spans.rfind("1000-4000,5000-", 0) == 0 && r.h.voicesSent[0].spans.find(",20000-24000") != std::string::npos
           && std::count(r.h.voicesSent[0].spans.begin(), r.h.voicesSent[0].spans.end(), ',') == 3 && r.h.voicesSent[0].changes == "2",
           r.h.voicesSent.empty() ? "no FVOICE" : r.h.voicesSent[0].spans + " / " + r.h.voicesSent[0].changes);
        const uint64_t v1 = r.h.voicesSent[0].jobId;
        r.p.onProg(vprog(v1, 300000, 600000));
        r.p.flush();
        st = r.h.lastOf("translatorFile");
        ok("U3: the voice pass runs: tphase voices, tpct 50; the translation is DONE (phase done)",
           fs(st, L"tphase") == "voices" && fn(st, L"tpct") == 50 && fs(st, L"phase") == "done", a(st).substr(0, 200));
        r.p.onSpk(spk(v1, 2, { 1, 2, 1, 2 }));
        r.p.onDone(done(v1, "ok"), false);
        pbtrf::VoiceRec vr;
        const bool rec = r.h.readVoices(keyT, vr);
        ok("U3: FSPK + FDONE ok -> the voices record (the analysis' key): on, 2 speakers, its 4 pieces and their labels",
           rec && vr.on && vr.speakers == 2 && vr.model == "campplus" && vr.segs.size() == 4 && vr.segs[1].spk == 2 && vr.segs[3].spk == 2,
           rec ? r.h.voicesDisk[keyT].substr(0, 300) : "no record");
        ok("U3: ...and the WORD FILE written at once, next to the media (Q2): \"Ep 1 - transcript.docx\", 4 turns named Speaker 1 / 2, "
           "file-relative stamps; the record remembers it",
           r.h.docx.size() == 1 && r.h.docx[0].path == docA && r.h.docx[0].t.turns.size() == 4 && r.h.docx[0].t.turns[1].name == "Speaker 2"
           && r.h.docx[0].t.turns[0].stamp == "[00:00:01]" && r.h.docx[0].t.namesOn && r.h.docx[0].t.speakers == 2
           && r.h.readVoices(keyT, vr) && vr.docxPath == a(docA) && vr.docxSig == r.h.docx[0].t.sig,
           r.h.docx.empty() ? "none" : a(r.h.docx[0].path));
        st = r.h.lastOf("translatorFile");
        std::wstring tr = r.h.lastOf("translatorTranscript");
        ok("U3: tphase done; the tab: state done, 2 speakers, its Word file named, not older, a turn per speaker change",
           fs(st, L"tphase") == "done" && fs(tr, L"state") == "done" && fn(tr, L"speakers") == 2 && fs(tr, L"file") == "Ep 1 - transcript.docx"
           && !fb(tr, L"older") && a(tr).find("\"name\":\"Speaker 2\",\"tc\":\"[00:00:05]\",\"text\":\"I agree.\"") != std::string::npos,
           a(tr).substr(0, 600));
        ok("U3: WHISPER ONCE: one FILE, one FVOICE for Transcribe + Translate", r.h.files.size() == 1 && r.h.voicesSent.size() == 1);

        // 2. RENAME: every turn of that speaker; the same name = ONE person; "" undoes it. (U2b re-point: the user's answer 02/11 --
        //    the Word file FOLLOWS a rename by itself, kDocxRenameMs after the LAST one of a burst: not yet here, and not said older)
        const uint64_t gen = r.p.session().gen;
        const bool rn1 = r.p.renameSpeaker(gen, vrevOf(r.p), 2, "  Marie ");
        tr = r.h.lastOf("translatorTranscript");
        ok("U3: RENAME speaker 2 \"Marie\" (trimmed): every turn of it, the record's NAME line; the Word file not rewritten YET (a "
           "burst is one write, after the last rename) and not said older meanwhile (U2b re-point)",
           rn1 && a(tr).find("\"name\":\"Marie\",\"tc\":\"[00:00:05]\"") != std::string::npos && a(tr).find("\"name\":\"Speaker 2\"") == std::string::npos
           && r.h.voicesDisk[keyT].find("\nNAME\t2\tMarie\n") != std::string::npos && r.h.docx.size() == 1 && !fb(tr, L"older")
           && r.h.rewriteTries == 0, a(tr).substr(0, 500));
        const bool rn2 = r.p.renameSpeaker(gen, vrevOf(r.p), 1, "marie");
        tr = r.h.lastOf("translatorTranscript");
        ok("U3: speaker 1 given the SAME name (case aside): ONE person -- the first three pieces make ONE turn, the last its own (11 s later)",
           rn2 && a(tr).find("\"text\":\"Good morning everyone. I agree. So let's move on.\"") != std::string::npos
           && a(tr).find("\"tc\":\"[00:00:20]\",\"text\":\"Thanks for coming.\"") != std::string::npos, a(tr).substr(0, 600));
        const bool rn3 = r.p.renameSpeaker(gen, vrevOf(r.p), 2, "Paul");
        tr = r.h.lastOf("translatorTranscript");
        ok("U3: renaming that person renames BOTH speakers sharing the name (still one person)",
           rn3 && r.h.voicesDisk[keyT].find("NAME\t1\tPaul\nNAME\t2\tPaul\n") != std::string::npos
           && a(tr).find("\"name\":\"Paul\",\"tc\":\"[00:00:01]\"") != std::string::npos, r.h.voicesDisk[keyT].substr(0, 300));
        const bool rn4 = r.p.renameSpeaker(gen, vrevOf(r.p), 1, "");
        tr = r.h.lastOf("translatorTranscript");
        ok("U3: \"\" sends each back to its own \"Speaker n\" (the merge undone) -- and the Word file is CURRENT again (what it holds)",
           rn4 && a(tr).find("\"name\":\"Speaker 2\",\"tc\":\"[00:00:05]\"") != std::string::npos && !fb(tr, L"older")
           && r.h.voicesDisk[keyT].find("NAME\t") == std::string::npos, a(tr).substr(0, 400));
        std::wstring why;
        ok("U3: refused: a control character (with its sentence), a speaker that does not exist, another session's -- nothing changed",
           !r.p.renameSpeaker(gen, vrevOf(r.p), 1, "a\x01" "b", &why) && !why.empty() && !r.p.renameSpeaker(gen, vrevOf(r.p), 3, "X") && !r.p.renameSpeaker(gen + 99, vrevOf(r.p), 1, "X")
           && r.h.voicesDisk[keyT].find("NAME\t") == std::string::npos);
        r.p.renameSpeaker(gen, vrevOf(r.p), 2, "Marie");
        // (U2b re-point, G2: the burst over -- five renames above -- ONE rewrite of the Word file, IN PLACE, holding the last names)
        r.h.now += pbplayer::kDocxRenameMs;
        r.p.tick();
        tr = r.h.lastOf("translatorTranscript");
        ok("U2b G2: the burst of renames over: ONE rewrite of the Word file, in place (the same path), holding \"Marie\"; the record "
           "keeps its new identity and contents -- current, not older",
           r.h.rewriteTries == 1 && r.h.rewrites == 1 && r.h.docx.size() == 2 && r.h.docx[1].path == docA
           && r.h.docx[1].t.turns.size() > 1 && r.h.docx[1].t.turns[1].name == "Marie" && r.h.readVoices(keyT, vr)
           && vr.docxSig == r.h.docx[1].t.sig && vr.docxSize == 2000 + r.h.docx[1].t.turns.size() && !fb(tr, L"older"),
           a(tr).substr(0, 300));

        // 3. REOPEN: the named transcript shown at once, no job
        Rig q;
        q.h.disk = r.h.disk;
        q.h.cache = r.h.cache;
        q.h.voicesDisk = r.h.voicesDisk;
        q.p.open({ A }, false);
        st = q.h.lastOf("translatorFile");
        tr = q.h.lastOf("translatorTranscript");
        // (U2b re-point: the Word file followed the renames -- G2 -- so it is current when the file comes back)
        ok("U3: the file OPENED again: its transcript shown at once with its names and its Word file -- no FILE, no FVOICE",
           q.h.files.empty() && q.h.voicesSent.empty() && fs(st, L"tphase") == "done" && fs(tr, L"state") == "done"
           && a(tr).find("\"name\":\"Marie\"") != std::string::npos && fs(tr, L"file") == "Ep 1 - transcript.docx" && !fb(tr, L"older"),
           a(tr).substr(0, 300));

        // 4. RE-TRANSCRIBE: the voice pass again, the names carried over BY TIME (the new pass numbers them the other way round),
        //    the Word file replaced IN PLACE
        r.p.transcribe(true);
        ok("U3: RE-TRANSCRIBE = the voice pass again (a second FVOICE), never the analysis (still one FILE)",
           r.h.voicesSent.size() == 2 && r.h.files.size() == 1 && fs(r.h.lastOf("translatorFile"), L"tphase") == "voices");
        const uint64_t v2 = r.h.voicesSent[1].jobId;
        r.p.onSpk(spk(v2, 2, { 2, 1, 2, 1 }));
        r.p.onDone(done(v2, "ok"), false);
        tr = r.h.lastOf("translatorTranscript");
        // (U2b re-point: the third Word file written -- the second was G2's rewrite after the renames)
        ok("U3: ...\"Marie\" goes to the NEW speaker that covers her time (now numbered 1); the Word file rewritten in place (the same path)",
           r.h.readVoices(keyT, vr) && vr.names.size() == 1 && vr.names[0].first == 1 && vr.names[0].second == "Marie"
           && a(tr).find("\"name\":\"Marie\",\"tc\":\"[00:00:05]\"") != std::string::npos && r.h.docx.size() == 3 && r.h.docx[2].path == docA
           && !fb(tr, L"older"), r.h.voicesDisk[keyT].substr(0, 300));

        // 5. TRANSCRIBE on a file whose analysis is complete (the cache): the voice pass ALONE (whisper not run again)
        Rig c;
        c.h.addFile(A, 5000, 11);
        c.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        c.p.open({ A }, false);
        ok("U3: a file analysed before (complete record): its text in the tab with NO press (state text), nothing runs",
           c.h.files.empty() && c.h.voicesSent.empty() && fs(c.h.lastOf("translatorTranscript"), L"state") == "text");
        c.p.transcribe(false);
        ok("U3: ...Transcribe = the voice pass ALONE: one FVOICE, no FILE", c.h.files.empty() && c.h.voicesSent.size() == 1);
        const uint64_t cv = c.h.voicesSent[0].jobId;
        c.p.transcriptCancel(c.p.session().gen);
        ok("U3: Transcribe's CANCEL during the voice pass: FCANCEL of the FVOICE; tphase cancelled; the translation untouched (done)",
           !c.h.cancels.empty() && c.h.cancels.back() == cv && fs(c.h.lastOf("translatorFile"), L"tphase") == "cancelled"
           && fs(c.h.lastOf("translatorFile"), L"phase") == "done");
        c.p.onDone(done(cv, "cancelled"), false);
        ok("U3: ...its FDONE cancelled: no record, no Word file, the tab cancelled", c.h.voicesDisk.empty() && c.h.docx.empty()
           && fs(c.h.lastOf("translatorTranscript"), L"state") == "cancelled");

        // 6. THE CANCELS never stop the other wish's work
        Rig k;
        k.h.addFile(A, 5000, 11);
        k.p.open({ A }, false);
        k.p.analyse(false);
        k.p.transcribe(false);
        ok("U3: Translate then Transcribe: ONE FILE (joined)", k.h.files.size() == 1 && k.h.cancels.empty());
        const uint64_t kj = k.h.files[0].jobId;
        k.p.cancel(k.p.session().gen);
        st = k.h.lastOf("translatorFile");
        ok("U3: Translate's CANCEL while Transcribe still wants the analysis: NO FCANCEL, it goes on (translate false, transcribe true)",
           k.h.cancels.empty() && !fb(st, L"translate") && fb(st, L"transcribe") && fs(st, L"phase") == "loading", a(st).substr(0, 200));
        k.p.transcriptCancel(k.p.session().gen);
        st = k.h.lastOf("translatorFile");
        ok("U3: ...then Transcribe's CANCEL: nobody wants it any more -> FCANCEL (cancelling), tphase cancelled",
           !k.h.cancels.empty() && k.h.cancels.back() == kj && fb(st, L"cancelling") && fs(st, L"tphase") == "cancelled");
        k.p.onDone(done(kj, "cancelled"), false);
        ok("U3: ...its FDONE: phase cancelled too (both slots offer their button again)",
           fs(k.h.lastOf("translatorFile"), L"phase") == "cancelled" && fs(k.h.lastOf("translatorFile"), L"tphase") == "cancelled");
        Rig k2;
        k2.h.addFile(A, 5000, 11);
        k2.p.open({ A }, false);
        k2.p.transcribe(false);
        k2.p.analyse(false);
        k2.p.transcriptCancel(k2.p.session().gen);
        st = k2.h.lastOf("translatorFile");
        ok("U3: Transcribe then Translate (joined), Transcribe's CANCEL: the analysis goes on for Translate (no FCANCEL)",
           k2.h.files.size() == 1 && k2.h.cancels.empty() && fb(st, L"translate") && !fb(st, L"transcribe") && fs(st, L"tphase") == "cancelled");
        k2.p.onDone(done(k2.h.files[0].jobId, "ok"), false);
        ok("U3: ...its analysis done: NO voice pass (the transcript was cancelled)", k2.h.voicesSent.empty());

        // 7. THE SOURCE TIMECODE: the file's own (25 fps, 10:00:00:00) -> every stamp, the tab's and the Word file's
        Rig t;
        t.h.addFile(A, 5000, 11);
        t.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        pbtc::SourceTc tc;
        tc.found = true; tc.origin = "bwf+ixml"; tc.hasRate = true; tc.fpsNum = 25; tc.fpsDen = 1; tc.base = 25; tc.startFrames = 900000;
        t.h.tcMap[pbtrf::lowerW(A)] = tc;
        t.p.open({ A }, false);
        tr = t.h.lastOf("translatorTranscript");
        ok("U3: a file that CARRIES a start timecode: the tab's stamps are SOURCE timecodes ([10:00:01:00]), said in its head",
           fb(tr, L"tcSource") && fs(tr, L"tc") == "source, 10:00:00:00 at 25 fps, read from the file"
           && a(tr).find("\"tc\":\"[10:00:01:00]\"") != std::string::npos && a(tr).find("\"s\":1000,") != std::string::npos, a(tr).substr(0, 400));
        t.p.transcribe(false);
        t.p.onSpk(spk(t.h.voicesSent[0].jobId, 2, { 1, 2, 1, 2 }));
        t.p.onDone(done(t.h.voicesSent[0].jobId, "ok"), false);
        ok("U3: ...and the Word file's: its stamps [10:00:05:00], its timecode the file's (the header names it)",
           t.h.docx.size() == 1 && t.h.docx[0].t.tc.found && t.h.docx[0].t.turns[1].stamp == "[10:00:05:00]",
           t.h.docx.empty() ? "none" : t.h.docx[0].t.turns[1].stamp);

        // 8. NO NAMES (the engine's diarizer off): the transcript stands, its Word file says why; NO SPEECH: no voice pass at all
        Rig n;
        n.h.addFile(A, 5000, 11);
        n.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        n.p.open({ A }, false);
        n.p.transcribe(false);
        n.p.onSpk(spk(n.h.voicesSent[0].jobId, 0, {}));
        n.p.onDone(done(n.h.voicesSent[0].jobId, "ok"), false);
        tr = n.h.lastOf("translatorTranscript");
        ok("U3: FSPK with no speaker (SPKON 0): tphase nonames, the tab says why, the Word file written without names",
           fs(n.h.lastOf("translatorFile"), L"tphase") == "nonames" && fs(tr, L"state") == "nonames"
           && fs(tr, L"why") == "speaker diarization is off (SPKON 0)" && n.h.docx.size() == 1 && !n.h.docx[0].t.namesOn
           && n.h.docx[0].t.turns[0].name.empty(), a(tr).substr(0, 300));
        Rig e;
        e.h.addFile(A, 5000, 11);
        pbtrf::Record empty;
        empty.path = a(A); empty.size = 5000; empty.mtime = 11; empty.langA = "en"; empty.langB = "fr"; empty.mode = "auto";
        empty.quality = "auto"; empty.complete = true; empty.totalMs = 600000; empty.engine = tp::kFileEngineVersion;
        empty.ranges.push_back(tp::MsRange{ 0, 600000 });
        e.h.cache[keyT] = pbtrf::packRecord(empty);
        e.p.open({ A }, false);
        e.p.transcribe(false);
        ok("U3: a file with NO SPEECH: no FVOICE, the tab says so (nospeech), no Word file, its record says why",
           e.h.voicesSent.empty() && e.h.docx.empty() && fs(e.h.lastOf("translatorTranscript"), L"state") == "nospeech"
           && e.h.voicesDisk[keyT].find("no speech was found") != std::string::npos, e.h.voicesDisk[keyT].substr(0, 200));

        // 9. THE ENGINE DIES in the voice pass (a synthesized FDONE error): the transcript failed, said; the live gate fails nothing
        Rig d;
        d.h.addFile(A, 5000, 11);
        d.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        d.p.open({ A }, false);
        d.p.transcribe(false);
        d.p.onDone(done(d.h.voicesSent[0].jobId, "error", "The SuVtitle engine stopped while recognising the voices."), true);
        tr = d.h.lastOf("translatorTranscript");
        ok("U3: the engine DIED in the voice pass: tphase error, the tab failed with the sentence, no record, nothing retried",
           fs(d.h.lastOf("translatorFile"), L"tphase") == "error" && fs(tr, L"state") == "failed"
           && fs(tr, L"why") == "The SuVtitle engine stopped while recognising the voices." && d.h.voicesDisk.empty() && d.h.voicesSent.size() == 1);
        Rig l;
        l.h.addFile(A, 5000, 11);
        l.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        l.p.open({ A }, false);
        l.p.transcribe(false);
        const uint64_t lv = l.h.voicesSent[0].jobId;
        l.p.setLive(true);
        ok("U3: LIVE starts during the voice pass: FCANCEL of it", !l.h.cancels.empty() && l.h.cancels.back() == lv);
        l.p.onDone(done(lv, "error", "The live translation is running: files wait for it."), false);
        ok("U3: ...its refusal while live fails nothing (tphase live)", fs(l.h.lastOf("translatorFile"), L"tphase") == "live");
        l.p.setLive(false);
        ok("U3: ...the gate opens: the voice pass again", l.h.voicesSent.size() == 2);

        // 10. TRANSCRIBE ALL: per file all its work, one job at a time, the list's order; a Word file up to date leaves it at once;
        //     with Translate all, ONE analysis per file; badges; CANCEL ALL
        Rig b;
        b.h.addFile(A, 5000, 11); b.h.addFile(B, 6000, 12); b.h.addFile(C, 7000, 13);
        const std::string keyB = pbtrf::cacheKey(B, 6000, 12, "en", "fr", "auto", "auto");
        const std::string keyC = pbtrf::cacheKey(C, 7000, 13, "en", "fr", "auto", "auto");
        b.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        b.h.cache[keyB] = transcriptRecord(B, 6000, 12);
        b.h.voicesDisk[keyT] = r.h.voicesDisk[keyT];               // Ep 1: transcribed, its Word file up to date
        b.h.disk[pbtrf::lowerW(docA)] = r.h.disk[pbtrf::lowerW(docA)];
        b.p.add({ A, B, C });                                      // (Ep 1 current, not playing)
        b.p.transcribeAll();
        const uint64_t idA = b.p.items()[0].id, idB = b.p.items()[1].id, idC = b.p.items()[2].id;
        ok("U3: TRANSCRIBE ALL: Ep 1 (transcribed, its Word file up to date) left alone; Ep 2 (analysed) its VOICE PASS first; Ep 10 "
           "queued -- one job, no Word file written",
           b.h.files.empty() && b.h.voicesSent.size() == 1 && b.h.voicesSent[0].path == a(B) && tbadgeNow(b.h, idA) == "done"
           && tbadgeNow(b.h, idB) == "voices" && tbadgeNow(b.h, idC) == "queued" && b.h.docx.empty(),
           tbadgeNow(b.h, idA) + "/" + tbadgeNow(b.h, idB) + "/" + tbadgeNow(b.h, idC) + " docx " + n2s(b.h.docx.size()));
        b.p.analyseAll();
        const uint64_t vb = b.h.voicesSent[0].jobId;
        b.p.onSpk(spk(vb, 1, { 1, 1, 1, 1 }));
        b.p.onDone(done(vb, "ok"), false);
        ok("U3: ...Ep 2 done (its Word file next to it); then Ep 10 -- its ANALYSIS, ONE for both batches (Translate all too)",
           b.h.docx.size() == 1 && b.h.docx[0].path == L"D:\\Films\\Season 1\\Ep 2 - transcript.docx" && b.h.files.size() == 1
           && b.h.files[0].path == a(C) && tbadgeNow(b.h, idB) == "done" && tbadgeNow(b.h, idC) == "analysing" && badgeNow(b.h, idC) == "analysing",
           tbadgeNow(b.h, idB) + "/" + tbadgeNow(b.h, idC) + "/" + badgeNow(b.h, idC));
        const uint64_t jc = b.h.files[0].jobId;
        b.p.onProg(prog(jc, "analyzing", 700000, "0-700000"));
        b.p.onCue(cue(jc, 0, 1000, 2000, "One."));
        b.p.onDone(done(jc, "ok"), false);
        ok("U3: ...Ep 10 analysed: its VOICE PASS next (no second FILE); its translation done", b.h.files.size() == 1
           && b.h.voicesSent.size() == 2 && b.h.voicesSent[1].path == a(C) && badgeNow(b.h, idC) == "ready" && tbadgeNow(b.h, idC) == "voices");
        b.p.cancelAll();
        ok("U3: CANCEL ALL: the batch's voice pass stopped (FCANCEL), Ep 10's transcript cancelled",
           !b.h.cancels.empty() && b.h.cancels.back() == b.h.voicesSent[1].jobId);
        b.p.onDone(done(b.h.voicesSent[1].jobId, "cancelled"), false);
        ok("U3: ...its row says cancelled, nothing starts again", tbadgeNow(b.h, idC) == "cancelled" && b.h.voicesSent.size() == 2
           && b.h.files.size() == 1, tbadgeNow(b.h, idC));
        b.p.transcribeAll();
        ok("U3: TRANSCRIBE ALL again: Ep 10 asked again (its voice pass), Ep 1 and Ep 2 up to date (nothing for them)",
           b.h.voicesSent.size() == 3 && b.h.voicesSent[2].path == a(C) && b.h.docx.size() == 1);

        // 11. THE WORD FILE's policy: EDITED by the user since -> never overwritten; GONE -> written again; a folder that refuses
        Rig w;
        w.h.addFile(A, 5000, 11);
        w.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        w.h.voicesDisk[keyT] = r.h.voicesDisk[keyT];
        w.h.disk[pbtrf::lowerW(docA)] = { 99999, 1 };              // the user edited it in Word
        w.p.add({ A });
        w.p.transcribeAll();
        ok("U3: a Word file the user EDITED since SuVtitle wrote it is never overwritten (Transcribe all leaves it)", w.h.docx.empty());
        w.h.disk.erase(pbtrf::lowerW(docA));
        w.p.transcribeAll();
        ok("U3: ...one that is GONE is written again", w.h.docx.size() == 1 && w.h.docx[0].path == docA);
        Rig f;
        f.h.addFile(A, 5000, 11);
        f.h.cache[keyT] = transcriptRecord(A, 5000, 11);
        f.h.docxFails = true;
        f.p.open({ A }, false);
        f.p.transcribe(false);
        f.p.onSpk(spk(f.h.voicesSent[0].jobId, 2, { 1, 2, 1, 2 }));
        f.p.onDone(done(f.h.voicesSent[0].jobId, "ok"), false);
        tr = f.h.lastOf("translatorTranscript");
        ok("U3: a Word file that cannot be written: the transcript is DONE all the same, the tab says why (note), no file named",
           fs(tr, L"state") == "done" && fs(tr, L"note") == "This folder does not take new files." && fs(tr, L"file").empty(), a(tr).substr(0, 300));
        // the Save .docx... view: the transcript as the file would hold it (a partial one says so)
        pbtrs::TranscriptOut so;
        ok("U3: Save .docx...'s view of the playing file: its turns, complete, names on",
           f.p.transcriptForSave(so) && so.turns.size() == 4 && !so.partial && so.namesOn && so.speakers == 2);
        ok("every post well-formed (U3)", r.h.badJson == 0 && b.h.badJson == 0 && t.h.badJson == 0 && k.h.badJson == 0,
           r.h.lastBadJson + b.h.lastBadJson);
    }

    // ---- U2b (02/11/2026, docs/PROGRESS_U2b.md): THE BATCH'S ONE RETRY with the default model (the user's rule), the message
    //      ONCE, the models IN USE (the menu's x), U1's double guard (F3), and G2 -- a rename (or an edit) updates the Word file
    //      by itself while it is SuVtitle's and unchanged. Sentinels: tests/cpp/cmut_u2.py (player_u2b), cmut_u3.py (player_u3).
    std::printf("[U2b] the batch's one retry (default model), the message once, models in use, F3, G2 (the Word file follows a rename)\n");
    {
        using MP = Player::ModelPick;
        const std::string mAsr = "D:\\Models\\ggml-base.bin", mMt = "E:\\LLM\\qwen2.5-1.5b-instruct-q4_k_m.gguf";
        const std::string idAsr = pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 148000000, 77);
        const std::string idMt = pbtrf::modelIdent(L"E:\\LLM\\qwen2.5-1.5b-instruct-q4_k_m.gguf", 1100000000, 78);
        const std::string errAsr = std::string(tp::kFileModelErrAsr) + "ggml-base.bin\" could not be used: the speech recogniser would not start (GPU or CPU).";
        const std::string errMt = std::string(tp::kFileModelErrMt) + "qwen2.5-1.5b-instruct-q4_k_m.gguf\" could not be used: the translation model would not start (GPU or CPU).";
        auto completeRec = [&](const std::wstring& path, uint64_t size, uint64_t mtime, const char* line, const std::string& models = std::string()) {
            pbtrf::Record rec;
            rec.path = a(path); rec.size = size; rec.mtime = mtime; rec.langA = "en"; rec.langB = "fr"; rec.mode = "auto"; rec.quality = "auto";
            rec.complete = true; rec.totalMs = 600000; rec.engine = tp::kFileEngineVersion;
            rec.ranges.push_back(tp::MsRange{ 0, 600000 });
            rec.cues.push_back(cue(1, 0, 1000, 4000, line, line));
            rec.cues.push_back(cue(1, 1, 5000, 9000, "I agree.", "I agree."));
            rec.cues.push_back(cue(1, 2, 20000, 24000, "Thanks for coming.", "Thanks for coming."));
            (void)models;
            return pbtrf::packRecord(rec);
        };
        auto keyDef = [](const std::wstring& p, uint64_t s, uint64_t m) { return pbtrf::cacheKey(p, s, m, "en", "fr", "auto", "auto"); };
        auto jobKey = [](const Player& p, uint64_t wire) -> std::string {
            for (const auto& j : p.jobs()) if (j.wire == wire) return j.set->key;
            return "<no job>";
        };
        auto itemRedo = [](const FakeHost& h, uint64_t id, bool queueOnly = false) -> int {   // the newest playerItem / playerQueue
            const std::string idk = "\"id\":" + std::to_string(id) + ",";                    // naming it: redo true 1 / false 0
            for (auto it = h.window.rbegin(); it != h.window.rend(); ++it) {
                const std::string t = typeOf(*it);
                if (t != "playerQueue" && (queueOnly || t != "playerItem")) continue;
                const std::string s = a(*it);
                const size_t at = s.find(idk);
                if (at == std::string::npos) continue;
                const size_t e = s.find('}', at), r = s.find("\"redo\":", at);
                if (r == std::string::npos || r > e) return -1;
                return s.compare(r + 7, 4, "true") == 0 ? 1 : 0;
            }
            return -2;
        };
        {
            // 1. "Translate all" on an imported model: a file NOT on screen fails on it -> REDONE ONCE with the default (its FILE names
            //    no model, its set the DEFAULT key: the normal cache), the row says it, ONE notice; the redo fails again -> a normal
            //    failure (never twice): the file leaves the batch, the batch goes on with the next file
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
            r.h.cache[keyDef(A, 5000, 11)] = completeRec(A, 5000, 11, "DEFAULT");
            r.p.open({ A, B, C }, false);                    // A plays (its default analysis complete)
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.analyseAll();
            const uint64_t idB = r.p.items()[1].id, idC = r.p.items()[2].id;
            const bool firstOk = r.h.files.size() == 1 && r.h.files[0].path == a(B) && r.h.files[0].asrModel == mAsr;
            r.p.onDone(done(r.h.files[0].jobId, "error", errAsr.c_str()), false);
            ok("U2b: Translate all -- a file NOT on screen fails on its imported model: REDONE at once with the default (its FILE names "
               "no model), its set of the DEFAULT key (no \"\\tm\" tag: the normal cache); the row says redone; the batch did NOT move on",
               firstOk && r.h.files.size() == 2 && r.h.files[1].path == a(B) && r.h.files[1].asrModel.empty() && r.h.files[1].mtModel.empty()
               && jobKey(r.p, r.h.files[1].jobId) == keyDef(B, 6000, 12) && itemRedo(r.h, idB) == 1 && itemRedo(r.h, idC) == 0
               && itemRedo(r.h, idB, true) == 1 && itemRedo(r.h, idC, true) == 0
               && badgeNow(r.h, idB) == "analysing" && badgeNow(r.h, idC) == "queued",
               r.h.files.size() > 1 ? r.h.files[1].path + " / " + jobKey(r.p, r.h.files[1].jobId) : std::string("no redo"));
            ok("U2b: ...the message ONCE: the host told to say it (a notice: the file is not on screen) and that the batch redoes "
               "\"Ep 2.mkv\"; the transcription model back to the default",
               r.h.fells.size() == 1 && r.h.fells[0].say && r.h.fells[0].redone == L"Ep 2.mkv" && r.h.fells[0].kind == "asr"
               && r.p.asrModel().path.empty());
            r.p.onProg(prog(r.h.files[1].jobId, "analyzing", 600000, "0-60000"));
            r.p.flush();
            std::string newestB;                             // the newest row message naming Ep 2 (a progress = a playerItem)
            for (auto it = r.h.window.rbegin(); it != r.h.window.rend() && newestB.empty(); ++it) {
                const std::string t = typeOf(*it);
                if ((t == "playerItem" || t == "playerQueue") && a(*it).find("\"id\":" + std::to_string(idB) + ",") != std::string::npos) newestB = t;
            }
            ok("U2b: ...the row's own update (a playerItem, the redo's progress) still says it was redone",
               newestB == "playerItem" && itemRedo(r.h, idB) == 1, newestB);
            r.p.onDone(done(r.h.files[1].jobId, "error", "The file could not be opened."), false);
            ok("U2b: ...the REDO fails too: a normal failure -- never a second redo; the file leaves the batch (failed), the batch goes "
               "on with the NEXT file (on the default); no second notice",
               r.h.files.size() == 3 && r.h.files[2].path == a(C) && r.h.files[2].asrModel.empty() && badgeNow(r.h, idB) == "error"
               && r.h.fells.size() == 1, badgeNow(r.h, idB));
            ok("every post well-formed (U2b 1)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // 2. NEVER TWICE on the other kind: both models imported; the TRANSCRIPTION model fails -> redone on the default whisper
            //    (the imported translation model kept); then the TRANSLATION model fails on that redo -> a normal failure, its own
            //    fall back and its own notice (a different model: said once too)
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.h.cache[keyDef(A, 5000, 11)] = completeRec(A, 5000, 11, "DEFAULT");
            r.p.open({ A, B }, false);
            r.p.setModels(MP{ mAsr, idAsr }, MP{ mMt, idMt });
            r.p.analyseAll();
            const uint64_t idB = r.p.items()[1].id;
            r.p.onDone(done(r.h.files.back().jobId, "error", errAsr.c_str()), false);
            const bool redoOk = r.h.files.size() == 2 && r.h.files[1].asrModel.empty() && r.h.files[1].mtModel == mMt;
            r.p.onDone(done(r.h.files[1].jobId, "error", errMt.c_str()), false);
            // (its row then follows the models chosen NOW -- U2's key rule: not "queued" any more, the default's state)
            ok("U2b: both imported, the whisper fails -> redone with the default whisper and the SAME translation model; that fails "
               "on the redo -> a normal failure (never a second redo: it left the batch), the translation model back to the "
               "default too, said once",
               redoOk && r.h.files.size() == 2 && !r.p.batchQueued().count(idB) && badgeNow(r.h, idB) != "queued"
               && r.p.asrModel().path.empty() && r.p.mtModel().path.empty()
               && r.h.fells.size() == 2 && r.h.fells[0].kind == "asr" && r.h.fells[0].say && r.h.fells[1].kind == "mt" && r.h.fells[1].say
               && r.h.fells[1].redone.empty(), badgeNow(r.h, idB));
        }
        {
            // 3. BOTH BATCHES on one file (Translate all + Transcribe all; the list's only file, current, not playing): ONE analysis
            //    fails -> ONE retry (one FILE, not two); the redo's analysis done -> its voice pass (the transcript goes on)
            Rig r;
            r.h.addFile(B, 6000, 12);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.add({ B });                                  // (queued: the first becomes current, not playing)
            r.p.analyseAll();
            r.p.transcribeAll();
            const size_t f0 = r.h.files.size();
            const bool firstImported = f0 == 1 && r.h.files[0].asrModel == mAsr;
            r.p.onDone(done(r.h.files.back().jobId, "error", errAsr.c_str()), false);
            const bool oneRedo = r.h.files.size() == f0 + 1 && r.h.files.back().path == a(B) && r.h.files.back().asrModel.empty();
            r.p.onProg(prog(r.h.files.back().jobId, "analyzing", 600000, "0-600000"));
            r.p.onCue(cue(r.h.files.back().jobId, 0, 1000, 4000, "Hello there."));
            r.p.onDone(done(r.h.files.back().jobId, "ok"), false);
            ok("U2b: BOTH batches on one file: ONE analysis failed -> ONE retry (one more FILE, not two); the redo done -> its VOICE "
               "PASS next (Transcribe all goes on), no third FILE",
               firstImported && oneRedo && r.h.files.size() == f0 + 1 && !r.h.voicesSent.empty() && r.h.voicesSent.back().path == a(B)
               && r.h.fells.size() == 1, n2s(r.h.files.size()) + " FILE(s), " + n2s(r.h.voicesSent.size()) + " FVOICE");
        }
        {
            // 4. A SINGLE PRESS (the playing file, not in a batch): U2's rule -- the strip says it, back to the default, NO redo
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.open({ A }, false);
            r.p.analyse(false);
            r.p.onDone(done(r.h.files[0].jobId, "error", errAsr.c_str()), false);
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("U2b: a SINGLE PRESS (no batch) keeps U2's rule: the strip says it (error, goes back to its own model), NO redo (one "
               "FILE), no notice of the host's, no `redo`",
               r.h.files.size() == 1 && fs(st, L"phase") == "error" && fs(st, L"err").find("goes back to its own transcription model") != std::string::npos
               && fs(st, L"redo").empty() && r.h.fells.size() == 1 && !r.h.fells[0].say && r.h.fells[0].redone.empty(), a(st).substr(0, 300));
        }
        {
            // 5. THE PLAYING FILE launched by the batch fails on its model: redone AT ONCE on the default -- a new generation, the
            //    DEFAULT key; the strip says it (translatorFile.redo, the engine's sentence + "Redone with ..."), not an error; the
            //    notice said (the strip shows the redo's progress, not the failure)
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});          // (chosen BEFORE the file opens: the set shown is the imported model's)
            r.p.open({ A, B }, false);
            r.p.analyseAll();
            const uint64_t g0 = r.p.session().gen, idA = r.p.items()[0].id;
            const bool firstA = r.h.files.size() == 1 && r.h.files[0].path == a(A) && r.h.files[0].asrModel == mAsr;
            r.p.onDone(done(r.h.files[0].jobId, "error", errAsr.c_str()), false);
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("U2b: the PLAYING file the batch launched fails on its model: redone at once (a FILE naming no model, a new generation, "
               "the DEFAULT key), said in the strip (`redo`: the engine's sentence + \"Redone with SuVtitle's own transcription model\"), "
               "phase loading -- not an error; the row says redone; the host says it in a notice",
               firstA && r.h.files.size() == 2 && r.h.files[1].path == a(A) && r.h.files[1].asrModel.empty() && r.p.session().gen != g0
               && r.p.session().set->key == keyDef(A, 5000, 11) && fs(st, L"phase") == "loading"
               && fs(st, L"redo").find("could not be used") != std::string::npos
               && fs(st, L"redo").find("Redone with SuVtitle's own transcription model (large-v3-turbo).") != std::string::npos
               && fs(st, L"err").empty() && itemRedo(r.h, idA) == 1 && r.h.fells.size() == 1 && r.h.fells[0].say
               && r.h.fells[0].redone == L"Ep 1.mkv", a(st).substr(0, 400));
            r.p.analyse(false);
            ok("U2b: ...the viewer's own press then: the strip's `redo` said no more (his press, not the batch's)",
               fs(r.h.lastOf("translatorFile"), L"redo").empty());
        }
        {
            // 6. THE CACHE MEETS THE REDO: the default model's COMPLETE analysis of that file exists -> no FILE for the redo, the file
            //    done (its record), the batch goes on
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12); r.h.addFile(C, 7000, 13);
            r.h.cache[keyDef(A, 5000, 11)] = completeRec(A, 5000, 11, "DEFAULT");
            r.h.cache[keyDef(B, 6000, 12)] = completeRec(B, 6000, 12, "B DEFAULT");
            r.p.open({ A, B, C }, false);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.analyseAll();
            const uint64_t idB = r.p.items()[1].id;
            const bool firstB = r.h.files.size() == 1 && r.h.files[0].path == a(B);
            r.p.onDone(done(r.h.files[0].jobId, "error", errAsr.c_str()), false);
            ok("U2b: the default model's COMPLETE analysis of the file is in the cache: the redo needs NO FILE -- the file done, the "
               "batch on with the next one",
               firstB && r.h.files.size() == 2 && r.h.files[1].path == a(C) && r.h.files[1].asrModel.empty() && badgeNow(r.h, idB) == "ready"
               && itemRedo(r.h, idB) == 1, badgeNow(r.h, idB));
        }
        {
            // 7. ANOTHER imported model chosen since the job started (not the default): the failure is NOT redone -- "redone with the
            //    default model" would not be true; and THE MESSAGE ONCE: a second job failing on a model that is no longer chosen
            //    is not said again
            Rig r;
            r.h.addFile(A, 5000, 11); r.h.addFile(B, 6000, 12);
            r.h.cache[keyDef(A, 5000, 11)] = completeRec(A, 5000, 11, "DEFAULT");
            r.p.open({ A, B }, false);
            r.p.setModels(MP{ mAsr, idAsr }, MP{});
            r.p.analyseAll();
            const uint64_t jB = r.h.files.back().jobId, idB = r.p.items()[1].id;
            const std::string mAsr2 = "D:\\Models\\ggml-small.bin", idAsr2 = pbtrf::modelIdent(L"D:\\Models\\ggml-small.bin", 487000000, 79);
            r.p.setModels(MP{ mAsr2, idAsr2 }, MP{});        // (the batch's job is cancelled -- its FDONE races the cancel)
            const size_t f1 = r.h.files.size();
            r.p.onDone(done(jB, "error", errAsr.c_str()), false);
            ok("U2b: another IMPORTED model chosen since: the old one's failure is NOT redone (not \"the default\"), not said in a "
               "notice (no longer the choice: nothing fell back), the file's row not marked redone",
               r.h.fells.size() == 1 && !r.h.fells[0].say && r.h.fells[0].redone.empty() && itemRedo(r.h, idB) == 0
               && r.p.asrModel().path == mAsr2 && r.h.files.size() == f1, n2s(r.h.files.size()) + "/" + n2s(f1));
        }
        {
            // 8. THE MODELS IN USE (the menu's x): a FILE naming an imported model -> in use (another case of its path too), the
            //    host told; still in use while cancelled and its FDONE owed; free at its FDONE (the host told again); a voice pass
            //    uses none
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.p.setModels(MP{ mAsr, idAsr }, MP{ mMt, idMt });
            r.p.open({ A }, false);
            const int c0 = r.h.inUseChanges;
            r.p.analyse(false);
            const bool used = r.p.modelInUse(mAsr) && r.p.modelInUse("d:\\MODELS\\GGML-BASE.BIN") && r.p.modelInUse(mMt)
                           && r.p.modelsInUse().size() == 2 && r.h.inUseChanges == c0 + 1 && !r.p.modelInUse("D:\\Models\\ggml-small.bin");
            r.p.cancel(r.p.session().gen);
            const bool stillUsed = r.p.modelInUse(mAsr) && r.h.inUseChanges == c0 + 1;
            r.p.onDone(done(r.h.files[0].jobId, "cancelled"), false);
            ok("U2b: MODELS IN USE: a FILE naming them -> in use (any case of the path), the host told once; cancelled with its FDONE "
               "owed -> still in use; its FDONE -> free, the host told again",
               used && stillUsed && !r.p.modelInUse(mAsr) && !r.p.modelInUse(mMt) && r.p.modelsInUse().empty() && r.h.inUseChanges == c0 + 2,
               n2s(r.h.inUseChanges - c0));
        }
        {
            // 9. F3 (U1's double guard, U2b): Cancel -> the FDONE cancelled -> nothing restarts by itself (the wish is over: the one
            //    guard left is cancel()'s `wanted = false`), the strip says cancelled; Translate pressed again -> it resumes
            Rig r;
            r.h.addFile(A, 5000, 11);
            r.p.open({ A }, false);
            r.p.analyse(false);
            const uint64_t j1 = r.h.files[0].jobId;
            r.p.cancel(r.p.session().gen);
            r.p.onDone(done(j1, "cancelled"), false);
            r.h.now += 5000; r.p.tick();
            r.h.now += 5000; r.p.tick();
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("U2b F3: a job the USER cancelled never starts again by itself (no FILE after its FDONE and two ticks); translate "
               "false, phase cancelled",
               r.h.files.size() == 1 && !fb(st, L"translate") && fs(st, L"phase") == "cancelled", fs(st, L"phase"));
            r.p.analyse(false);
            ok("U2b F3: ...Translate pressed again: the analysis resumes (a second FILE)", r.h.files.size() == 2);
        }
        // ---- G2 (the user's answer 02/11): THE WORD FILE FOLLOWS A RENAME -- while it is SuVtitle's and unchanged ----
        const std::wstring docA = L"D:\\Films\\Season 1\\Ep 1 - transcript.docx";
        const std::string keyT = keyDef(A, 5000, 11);
        auto transcribed = [&](Rig& r) {                     // A: analysed (complete), transcribed with 2 speakers, its Word file written
            r.h.addFile(A, 5000, 11);
            r.h.cache[keyT] = completeRec(A, 5000, 11, "Good morning everyone.");
            r.p.open({ A }, false);
            r.p.transcribe(false);
            const uint64_t v = r.h.voicesSent.back().jobId;
            tp::WireSpk s; s.jobId = v; s.speakers = 2; s.labels = { 1, 2, 1 }; s.model = "campplus";
            r.p.onSpk(s);
            r.p.onDone(done(v, "ok"), false);
            return r.p.session().gen;
        };
        {
            // 10. ours and unedited: ONE write for a BURST (three renames 300 ms apart), kDocxRenameMs after the LAST one -- not before
            Rig r;
            const uint64_t gen = transcribed(r);
            const size_t d0 = r.h.docx.size();
            r.p.renameSpeaker(gen, vrevOf(r.p), 1, "Anna");
            r.h.now += 300; r.p.tick();
            r.p.renameSpeaker(gen, vrevOf(r.p), 2, "Ben");
            r.h.now += 300; r.p.tick();
            r.p.renameSpeaker(gen, vrevOf(r.p), 2, "Bob");
            r.h.now += pbplayer::kDocxRenameMs - 1; r.p.tick();
            const bool notYet = r.h.rewriteTries == 0 && r.h.docx.size() == d0;
            r.h.now += 1; r.p.tick();
            pbtrf::VoiceRec vr;
            ok("U2b G2: a BURST of renames (300 ms apart): not written before kDocxRenameMs after the LAST one, then ONE write, IN PLACE "
               "(the same path), holding the last names (Anna, Bob); the record keeps its new identity",
               d0 == 1 && notYet && r.h.rewriteTries == 1 && r.h.rewrites == 1 && r.h.docx.size() == 2 && r.h.docx[1].path == docA
               && r.h.docx[1].t.turns[0].name == "Anna" && r.h.docx[1].t.turns[1].name == "Bob" && r.h.readVoices(keyT, vr)
               && vr.docxSig == r.h.docx[1].t.sig && !fb(r.h.lastOf("translatorTranscript"), L"older"),
               n2s(r.h.rewriteTries) + " tries, " + n2s(r.h.docx.size()) + " files");
            r.h.now += 5000; r.p.tick();
            ok("U2b G2: ...and nothing more afterwards (no write without a change)", r.h.rewriteTries == 1 && r.h.docx.size() == 2);
            // 10b. a subtitle EDIT of the transcribed file: the same rule (DECISION) -- the Word file follows the corrected text
            const std::vector<uint32_t> rm = { 0 };
            Player::EditCue ed;
            ed.s = 1000; ed.e = 4000; ed.lang = "en"; ed.dstLang = "fr"; ed.src = "Good morning, everybody."; ed.dst = "Bonjour a tous.";
            const bool edited = r.p.editCues(gen, 1, rm, { ed });
            r.h.now += pbplayer::kDocxRenameMs; r.p.tick();
            ok("U2b G2: a subtitle EDIT of that transcript: its Word file follows too (one rewrite in place, the corrected text)",
               edited && r.h.rewrites == 2 && r.h.docx.size() == 3 && r.h.docx[2].path == docA
               && r.h.docx[2].t.turns[0].text.find("Good morning, everybody.") != std::string::npos,
               r.h.docx.size() > 2 ? r.h.docx[2].t.turns[0].text : std::string("no rewrite"));
        }
        {
            // 11. NEVER when EDITED by the user (its identity changed since SuVtitle wrote it), NEVER when not ours (the registry):
            //     the tab says it -- edited: never touched; not ours: older, nothing said
            Rig r;
            const uint64_t gen = transcribed(r);
            r.h.disk[pbtrf::lowerW(docA)] = { 77777, 3 };    // the user saved it in Word
            r.p.renameSpeaker(gen, vrevOf(r.p), 1, "Anna");
            r.h.now += pbplayer::kDocxRenameMs; r.p.tick();
            const std::wstring tr = r.h.lastOf("translatorTranscript");
            ok("U2b G2: a Word file the user EDITED since: NEVER rewritten (no try), the tab says edited (not older, no error)",
               r.h.rewriteTries == 0 && r.h.docx.size() == 1 && fb(tr, L"edited") && !fb(tr, L"older") && fs(tr, L"docxErr").empty(),
               a(tr).substr(0, 300));
            Rig n;
            const uint64_t g2 = transcribed(n);
            n.h.docxNotOurs = true;                          // (the registry no longer says it is ours for this media)
            n.p.renameSpeaker(g2, vrevOf(n.p), 1, "Anna");
            n.h.now += pbplayer::kDocxRenameMs; n.p.tick();
            const std::wstring t2 = n.h.lastOf("translatorTranscript");
            ok("U2b G2: a Word file that is NOT OURS any more (the registry): asked once, refused -- nothing written, the tab says "
               "older, no error sentence",
               n.h.rewriteTries == 1 && n.h.rewrites == 0 && n.h.docx.size() == 1 && fb(t2, L"older") && fs(t2, L"docxErr").empty(),
               a(t2).substr(0, 300));
        }
        {
            // 12. THE LAST RENAME NEVER LOST: renamed, then at once another file opened / the file closed / the app's end -> written
            //     THEN (no wait), with the last name
            Rig r;
            r.h.addFile(B, 6000, 12);
            const uint64_t gen = transcribed(r);
            r.p.renameSpeaker(gen, vrevOf(r.p), 1, "Anna");
            r.p.open({ B }, false);                          // (another file opened: the session ends at once)
            const bool onSwitch = r.h.rewrites == 1 && r.h.docx.back().path == docA && r.h.docx.back().t.turns[0].name == "Anna";
            Rig c;
            const uint64_t g2 = transcribed(c);
            c.p.renameSpeaker(g2, vrevOf(c.p), 2, "Ben");
            c.p.close();
            const bool onClose = c.h.rewrites == 1 && c.h.docx.back().t.turns[1].name == "Ben";
            Rig s;
            const uint64_t g3 = transcribed(s);
            s.p.renameSpeaker(g3, vrevOf(s.p), 1, "Cleo");
            s.p.shutdown();
            const bool onQuit = s.h.rewrites == 1 && s.h.docx.back().t.turns[0].name == "Cleo";
            Rig e;
            const uint64_t g4 = transcribed(e);
            e.p.renameSpeaker(g4, vrevOf(e.p), 1, "Dora");
            e.p.persist();                                   // (the Windows session ends: WM_QUERYENDSESSION)
            const bool onEnd = e.h.rewrites == 1 && e.h.docx.back().t.turns[0].name == "Dora";
            ok("U2b G2: the LAST rename is never lost -- written at once when another file is opened, the file closed, the app quits, "
               "the Windows session ends",
               onSwitch && onClose && onQuit && onEnd, std::string(onSwitch ? "" : "switch ") + (onClose ? "" : "close ")
               + (onQuit ? "" : "quit ") + (onEnd ? "" : "session-end"));
        }
        {
            // 13. OPEN IN WORD (the write refused): the tab says so in one sentence and keeps "older"; the NEXT rename retries -> written,
            //     the sentence gone; and a Save (transcriptSaved) ends it too
            Rig r;
            const uint64_t gen = transcribed(r);
            r.h.docxLocked = true;
            r.p.renameSpeaker(gen, vrevOf(r.p), 1, "Anna");
            r.h.now += pbplayer::kDocxRenameMs; r.p.tick();
            const std::wstring tr = r.h.lastOf("translatorTranscript");
            ok("U2b G2: the Word file OPEN in another program: not written, the tab says \"The Word file is open in another program: "
               "close it to update it.\" and keeps older",
               r.h.rewriteTries == 1 && r.h.rewrites == 0 && fb(tr, L"older")
               && fs(tr, L"docxErr") == "The Word file is open in another program: close it to update it.", a(tr).substr(0, 400));
            r.h.now += 10000; r.p.tick();
            const bool noLoop = r.h.rewriteTries == 1;      // (never retried in a loop: the next change does)
            r.h.docxLocked = false;
            r.p.renameSpeaker(gen, vrevOf(r.p), 2, "Ben");
            r.h.now += pbplayer::kDocxRenameMs; r.p.tick();
            const std::wstring t2 = r.h.lastOf("translatorTranscript");
            ok("U2b G2: ...no retry in a loop; the NEXT rename retries: written in place (both names), the sentence gone, not older",
               noLoop && r.h.rewriteTries == 2 && r.h.rewrites == 1 && r.h.docx.back().path == docA
               && r.h.docx.back().t.turns[0].name == "Anna" && r.h.docx.back().t.turns[1].name == "Ben"
               && fs(t2, L"docxErr").empty() && !fb(t2, L"older"), a(t2).substr(0, 300));
            Rig v;
            const uint64_t g2 = transcribed(v);
            v.h.docxLocked = true;
            v.p.renameSpeaker(g2, vrevOf(v.p), 1, "Anna");
            v.h.now += pbplayer::kDocxRenameMs; v.p.tick();
            const bool said = fs(v.h.lastOf("translatorTranscript"), L"docxErr").size() > 10;
            pbtrs::TranscriptOut so;
            v.p.transcriptForSave(so);
            v.p.transcriptSaved(docA, 3333, 44, so.sig, so.tc);    // Save .docx... wrote it (once Word let go)
            ok("U2b G2: ...or Save .docx... writes it: the sentence gone, current", said
               && fs(v.h.lastOf("translatorTranscript"), L"docxErr").empty() && !fb(v.h.lastOf("translatorTranscript"), L"older"));
            ok("every post well-formed (U2b)", r.h.badJson == 0 && v.h.badJson == 0, r.h.lastBadJson + v.h.lastBadJson);
        }
    }

    // ---- N2, fix round 02/11 (merged into the tree's smoke by the director, PROGRESS_N2.md; mutants: cmut.py's player
    // target, "N2 ..."): the core's half of L2 (no sound copy while mpv plays the item) and L6 (mpv's sound
    // tracks for the containers MediaTracks does not read), driven exactly as the bridge drives them
    std::printf("[N2] L2 / L6: the sound copy while mpv plays, mpv's track list adopted\n");
    {
        auto mpvInfo = [](std::vector<std::pair<uint32_t, const char*>> ids, bool video = true) {
            auto i = std::make_shared<pbmedia::Info>();
            i->state = pbmedia::Info::State::Ready; i->container = "mpv"; i->hasVideo = video;
            for (size_t k = 0; k < ids.size(); ++k) {
                pbmedia::AudioTrack t; t.ord = static_cast<uint32_t>(k); t.track = ids[k].first; t.lang = ids[k].second;
                t.codec = "MP3"; t.ch = 2; t.def = false; t.play = true;
                i->audio.push_back(t);
            }
            return i;
        };
        const std::wstring V = L"D:\\Films\\Rec\\news.ts", W = L"D:\\Films\\Rec\\other.ts";
        const std::wstring XU = L"E:\\Other\\clip.avi";
        // L2 -- a folder that cannot be served (no URL) while mpv plays: NO copy at the opening, none for a stale <video>
        // failure; the moment mpv lets it go (mpvDropped) it is asked -- once
        {
            Rig m;
            m.h.mpvOn = true;
            m.h.addFile(XU);
            m.h.mapFail.insert(pbtrf::lowerW(L"E:\\Other"));
            m.p.open({ XU }, false);
            const uint64_t seq = static_cast<uint64_t>(fn(m.h.lastOf("translatorFile", true), L"seq"));
            ok("N2 L2: no URL, mpv plays it: NO sound copy asked at the opening", fs(m.h.lastOf("translatorFile", true), L"url").empty() && m.h.proxies.empty());
            m.p.playFailed(seq);
            ok("N2 L2: ...a <video> failure while mpv plays it (stale): still none", m.h.proxies.empty());
            m.h.mpvOn = false;                                   // (the bridge: the item fell back -- nativeFellBack_)
            m.p.mpvDropped(seq + 1);
            ok("N2 L2: ...mpvDropped of ANOTHER opening: nothing", m.h.proxies.empty());
            m.p.mpvDropped(seq);
            const bool byCore = m.h.proxies.size() == 1;         // (the CORE asks it -- before any word of the page)
            m.p.mpvDropped(seq);
            m.p.playFailed(seq);                                 // (the page's own door, after it)
            ok("N2 L2: mpv lets it go (mpvDropped): the core asks the copy itself, ONCE (the page's playFailed after it adds none)",
               byCore && m.h.proxies.size() == 1 && m.h.proxies[0].src == a(XU) && fs(m.h.lastOf("translatorFile", true), L"proxyState") == "working");
        }
        // L6 -- never over a MediaTracks list (an MKV the probe read): mpv's offer changes nothing
        {
            Rig r;
            r.h.mpvOn = true;
            const std::wstring K = L"D:\\Films\\k.mkv";
            r.h.addFile(K, 5000, 11);
            auto mk = std::make_shared<pbmedia::Info>();
            mk->state = pbmedia::Info::State::Ready; mk->container = "mkv"; mk->hasVideo = true;
            pbmedia::AudioTrack t0; t0.ord = 0; t0.track = 2; t0.lang = "eng"; t0.codec = "AAC"; t0.def = true; t0.play = true; t0.ch = 2;
            mk->audio = { t0 };
            r.h.probeMap[pbtrf::lowerW(K)] = mk;
            r.p.open({ K }, false);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.p.offerMpvTracks(seq, mpvInfo({ { 1, "eng" }, { 2, "fre" }, { 3, "ger" } }), 1, true);
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("N2 L6: a MediaTracks list is NEVER overridden (mkv, 1 track, still the probe's)",
               a(st).find("\"container\":\"mkv\"") != std::string::npos && r.p.session().tracks && r.p.session().tracks->audio.size() == 1
               && !r.p.session().tracksFromMpv, a(st).substr(0, 300));
        }
        // ...nor while the probe still runs; a probe that FAILS then leaves mpv's list in
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.autoProbe = false;
            const std::wstring K = L"D:\\Films\\broken.mkv";
            r.h.addFile(K, 5000, 11);
            r.p.open({ K }, false);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.p.offerMpvTracks(seq, mpvInfo({ { 1, "eng" }, { 2, "fre" } }), 0, true);
            const bool waited = r.p.session().tracksState == "probing" && !r.p.session().tracksFromMpv;
            auto err = std::make_shared<pbmedia::Info>();
            err->state = pbmedia::Info::State::Error; err->err = "cut short";
            r.p.onTracks(r.h.probeReqs.back().first, err);
            ok("N2 L6: an MKV whose probe still runs: mpv's list waits; the probe FAILS: mpv's list is the item's (2 tracks, mpv's choice 0)",
               waited && r.p.session().tracksFromMpv && r.p.session().tracksState == "ready" && r.p.session().tracks->audio.size() == 2
               && r.p.session().audioOrd == 0 && a(r.h.lastOf("translatorFile")).find("\"container\":\"mpv\"") != std::string::npos);
        }
        // ...a TS (MediaTracks reads none) that mpv plays, Translate pressed: the job WAITS for mpv's list AND its own choice
        // (its list can come before its selection); mpv chose the 2nd track (the system's language): the job starts THERE,
        // named ("1:12"), nothing cancelled
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15);
            r.p.open({ V }, true);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            const bool held0 = r.h.files.empty();
            r.p.offerMpvTracks(seq, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), -1, false);    // Tracks: no selection yet
            const bool held1 = r.h.files.empty() && r.p.session().tracksState == "none";
            r.p.offerMpvTracks(seq, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), 1, false);     // Track: mpv plays the 2nd
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("N2 L6: TS under mpv, Translate pressed: the job WAITS for mpv's list and its choice, then starts on THAT track, named (\"1:12\")",
               held0 && held1 && r.h.files.size() == 1 && r.h.files[0].audio == "1:12" && r.h.cancels.empty()
               && fn(st, L"audioOrd") == 1 && a(st).find("\"container\":\"mpv\"") != std::string::npos,
               r.h.files.empty() ? "no FILE" : r.h.files[0].audio);
            // the viewer picks the FIRST track: the analysis follows it, named "0:11" (never "" = aid=auto, which is the 2nd here)
            r.p.setAudio(0, false);
            ok("N2 L6: the viewer picks track 1: FCANCEL + a FILE named \"0:11\" (never \"\": for the engine's mpv that is aid=auto)",
               r.h.files.size() == 2 && r.h.files[1].audio == "0:11" && r.h.cancels.size() == 1, r.h.files.back().audio);
            r.h.now += 2000; r.p.tick();
            const std::string mk = pbtrf::mediaKey(V, 9000, 15);
            ok("N2 L6: ...the pick persisted like a Matroska file's (player-audio.txt: mediaKey, ord 0, mpv's id 11)",
               r.h.audioBytes.find("AUD\t" + mk + "\t0\t11\t") != std::string::npos, r.h.audioBytes);
            // another run: the pick comes back once mpv lists the tracks (no need to wait for mpv's own choice then)
            Rig r2;
            r2.h.mpvOn = true;
            r2.h.addFile(V, 9000, 15);
            r2.p.loadAudio(r.h.audioBytes);
            r2.p.load("", "");
            r2.p.open({ V }, true);
            const uint64_t seq2 = static_cast<uint64_t>(fn(r2.h.lastOf("translatorFile"), L"seq"));
            r2.p.offerMpvTracks(seq2, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), -1, false);
            ok("N2 L6: another run: the viewer's pick (track 1) is the item's as soon as mpv lists the tracks; the job on \"0:11\"",
               r2.h.files.size() == 1 && r2.h.files[0].audio == "0:11" && fn(r2.h.lastOf("translatorFile"), L"audioOrd") == 0,
               r2.h.files.empty() ? "no FILE" : r2.h.files[0].audio);
        }
        // ...the same run, the file opened AGAIN: its list is known at once (no wait), mpv's own first choice played again
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15); r.h.addFile(W, 9100, 16);
            r.p.open({ V, W }, true);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.p.offerMpvTracks(seq, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), 1, true);
            const size_t files1 = r.h.files.size();
            r.p.queueOp("play", r.p.items()[1].id, 0, 0);
            r.p.queueOp("play", r.p.items()[0].id, 0, 0);
            const std::wstring st = r.h.lastOf("translatorFile");
            ok("N2 L6: opened again in the run: the list known at once (container mpv), mpv's own first choice (ord 1) asked, no wait",
               fn(st, L"audioOrd") == 1 && a(st).find("\"container\":\"mpv\"") != std::string::npos && r.p.session().tracksFromMpv
               && !r.p.session().probeWait && files1 == 1, a(st).substr(0, 200));
        }
        // ...the wait ran out (a slow share) and the job started on "" (aid=auto); mpv then says its choice is the 2nd track:
        // that job's cues are the 2nd track's under the first's key -- said so (AUD) and moved to the 2nd's own set
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15);
            r.p.open({ V }, true);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.h.now += 4000; r.p.tick();
            const bool blank = r.h.files.size() == 1 && r.h.files[0].audio.empty();
            const uint64_t j1 = blank ? r.h.files[0].jobId : 0;
            r.p.onProg(prog(j1, "analyzing", 600000, "0-10000"));    // (it decoded aid=auto's pick -- the 2nd track -- for 10 s)
            r.p.onCue(cue(j1, 0, 1000, 2500, "Bonjour.", "Hello."));
            r.p.offerMpvTracks(seq, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), 1, true);
            ok("N2 L6: the wait ran out (job on \"\"), mpv's late choice is the 2nd track: the job moves to \"1:12\" (FCANCEL, a new FILE)",
               blank && r.h.files.size() == 2 && r.h.files[1].audio == "1:12" && !r.h.cancels.empty() && r.h.cancels.back() == j1,
               r.h.files.empty() ? "no FILE" : r.h.files.back().audio);
            r.p.onDone(done(j1, "cancelled"), false);            // the old job's partial is saved: under the FIRST track's key...
            int firstKeyAud = -2;
            for (const auto& kv : r.h.cache) {
                pbtrf::Record rec;
                if (pbtrf::parseRecord(kv.second, rec) && rec.path == a(V) && kv.first == pbtrf::cacheKey(V, 9000, 15, "en", "fr", "auto", "auto"))
                    firstKeyAud = static_cast<int>(rec.audioOrd);
            }
            ok("N2 L6: ...its cues are said to be the 2nd track's (AUD 1): never loaded as the first track's (loadCacheInto's guard)",
               firstKeyAud == 1 || firstKeyAud == -2, "AUD " + std::to_string(firstKeyAud));
            ok("N2 L6: ...(the partial WAS written: the check above is not empty)", firstKeyAud != -2);
        }
        // ...mpv lists no sound track at Loaded (or a host without the list): the wait ends, the job starts on ""
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15);
            r.p.open({ V }, true);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.p.offerMpvTracks(seq, mpvInfo({}), -1, false);
            ok("N2 L6: mpv lists no sound track: the wait ends at once, the job on \"\" (as before 1.1.0)",
               r.h.files.size() == 1 && r.h.files[0].audio.empty() && r.p.session().tracksState == "none");
        }
        // ...mpv lets the item go before its list came: the wait ends too
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15);
            r.p.open({ V }, true);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.h.mpvOn = false;
            r.p.mpvDropped(seq);
            ok("N2 L6: mpv lets the item go before its list: the wait ends, the job on \"\" (the <video>'s default track)",
               r.h.files.size() == 1 && r.h.files[0].audio.empty());
        }
        // a stale offer (another opening's seq) changes nothing
        {
            Rig r;
            r.h.mpvOn = true;
            r.h.addFile(V, 9000, 15);
            r.p.open({ V }, false);
            const uint64_t seq = static_cast<uint64_t>(fn(r.h.lastOf("translatorFile"), L"seq"));
            r.p.offerMpvTracks(seq + 7, mpvInfo({ { 11, "eng" }, { 12, "fre" } }), 1, true);
            ok("N2 L6: an offer for ANOTHER opening is dropped", r.p.session().tracksState == "none" && !r.p.session().tracksFromMpv);
            ok("every post well-formed (N2)", r.h.badJson == 0, r.h.lastBadJson);
        }
    }

    // ---- FX (02/11/2026, the adversarial review's fix round -- docs/PROGRESS_FIX.md; sentinels: tests/cpp/cmut_u3.py, "FX ..."):
    //      F1 the Word file keeps the timecode its stamps were made with; F2 the user's rule, ONE Word file per media AND sound track
    //      (FakeHost's regMode = the bridge's rules through TrFileStore.h's own functions); F4 a Word file held / read-only is said
    //      with "older", never written elsewhere; F5 "redone" once the redo STARTED; F6 each row's Word file and Transcribe all's ONE
    //      notice; F7 the rows the list has (`batch`); F8 a rename names a speaker of the transcript it was typed for
    std::printf("[FX] the fix round: F1 the Word file's timecode, F2 one Word file per sound track, F4 held, F5 redone, F6 rows, F7 batch, F8 renames\n");
    {
        auto fxSpk = [](uint64_t job, uint32_t n, std::vector<uint32_t> labels) {
            tp::WireSpk s; s.jobId = job; s.speakers = n; s.labels = std::move(labels); s.model = "campplus"; return s;
        };
        // FVOICE number `i` answered: 2 speakers, the pieces alternating (1, 2, 1... -- swapped: 2, 1, 2..., another numbering)
        auto fxVoices = [&](Rig& r, size_t i, bool swapped = false) {
            if (i >= r.h.voicesSent.size()) return false;
            const tp::WireVoice& v = r.h.voicesSent[i];
            std::vector<tp::MsRange> sp;
            tp::parseRanges(v.spans, sp);
            std::vector<uint32_t> labels;
            for (size_t k = 0; k < sp.size(); ++k) labels.push_back(static_cast<uint32_t>(swapped ? 2 - k % 2 : 1 + k % 2));
            r.p.onSpk(fxSpk(v.jobId, 2, labels));
            r.p.onDone(done(v.jobId, "ok"), false);
            return true;
        };
        // a complete analysis of three lines (en / fr), as the cache keeps it
        auto fxRecord = [&](const std::wstring& path, uint64_t size, uint64_t mtime) {
            pbtrf::Record rec;
            rec.path = a(path); rec.size = size; rec.mtime = mtime; rec.langA = "en"; rec.langB = "fr"; rec.mode = "auto"; rec.quality = "auto";
            rec.complete = true; rec.totalMs = 600000; rec.engine = tp::kFileEngineVersion;
            rec.ranges.push_back(tp::MsRange{ 0, 600000 });
            rec.cues.push_back(cue(1, 0, 1000, 4000, "Good morning everyone."));
            rec.cues.push_back(cue(1, 1, 5000, 9000, "I agree with you."));
            rec.cues.push_back(cue(1, 2, 20000, 24000, "Thanks for coming."));
            return pbtrf::packRecord(rec);
        };
        auto fxKey = [](const std::wstring& p, uint64_t s, uint64_t m) { return pbtrf::cacheKey(p, s, m, "en", "fr", "auto", "auto"); };
        // a field of the newest playerItem / playerQueue naming item `id` (its raw JSON text; "<none>" when absent); queueOnly: the
        // newest playerQueue only
        auto rowField = [](const FakeHost& h, uint64_t id, const char* key, bool queueOnly = false) -> std::string {
            const std::string idk = "\"id\":" + std::to_string(id) + ",";
            for (auto it = h.window.rbegin(); it != h.window.rend(); ++it) {
                const std::string t = typeOf(*it);
                if (t != "playerQueue" && (queueOnly || t != "playerItem")) continue;
                const std::string s = a(*it);
                const size_t at = s.find(idk);
                if (at == std::string::npos) continue;
                const std::string kk = std::string("\"") + key + "\":";
                const size_t e = s.find('}', at), k = s.find(kk, at);
                if (k == std::string::npos || k > e) return "<none>";
                size_t b = k + kk.size(), x = b;
                if (b < s.size() && s[b] == '"') {
                    x = ++b;
                    while (x < s.size() && s[x] != '"') { if (s[x] == '\\') ++x; ++x; }
                    return s.substr(b, x - b);
                }
                while (x < s.size() && s[x] != ',' && s[x] != '}') ++x;
                return s.substr(b, x - b);
            }
            return "<none>";
        };
        auto countNotice = [](const FakeHost& h, const std::string& part) {
            size_t n = 0;
            for (const auto& j : h.window) if (typeOf(j) == "translatorFileNotice" && a(j).find(part) != std::string::npos) ++n;
            return n;
        };
        pbtc::SourceTc tc25;                                  // a field recorder's BWF: 10:00:00:00 at 25 fps
        tc25.found = true; tc25.origin = "bwf+ixml"; tc25.hasRate = true; tc25.fpsNum = 25; tc25.fpsDen = 1; tc25.base = 25;
        tc25.startFrames = 900000; tc25.sampleRate = 48000; tc25.startSamples = 1728000000ull;

        // ---- F1: an UNKNOWN, unsettled or failed source timecode never makes a Word file "older" ----
        {
            const std::wstring T = L"D:\\Rushes\\take1.wav";
            const std::string keyT = fxKey(T, 5000, 11);
            Rig r1;                                           // run 1: the probe answers the file's timecode
            r1.h.addFile(T, 5000, 11);
            r1.h.cache[keyT] = fxRecord(T, 5000, 11);
            r1.h.tcMap[pbtrf::lowerW(T)] = tc25;
            r1.p.open({ T }, false);
            r1.p.transcribe(false);
            fxVoices(r1, 0);
            ok("[FX] F1: run 1 -- the Word file written with SOURCE stamps ([10:00:01:00]); its record keeps that timecode (DOCXTC)",
               r1.h.docx.size() == 1 && r1.h.docx[0].t.tc.found && r1.h.docx[0].t.turns[0].stamp == "[10:00:01:00]"
               && r1.h.voicesDisk[keyT].find("\nDOCXTC\tbwf+ixml\t1\t900000\t25\t1\t25\t0\t1728000000\t48000\n") != std::string::npos,
               r1.h.voicesDisk[keyT].substr(0, 400));
            auto run2 = [&](Rig& r, bool silent) {          // the same disk; silent = the probe never answers (a sleeping disk)
                r.h.disk = r1.h.disk; r.h.cache = r1.h.cache; r.h.voicesDisk = r1.h.voicesDisk;
                if (silent) r.h.autoTc = false;
            };
            Rig q;
            run2(q, true);
            q.p.open({ T }, false);
            const std::wstring tr0 = q.h.lastOf("translatorTranscript");
            ok("[FX] F1: run 2, its probe silent: the FIRST post is not \"older\" (no flash at the opening) and its stamps are already "
               "the source ones (the record's timecode), said as such",
               !tr0.empty() && !fb(tr0, L"older") && a(tr0).find("\"tc\":\"[10:00:01:00]\"") != std::string::npos && fb(tr0, L"tcSource"),
               a(tr0).substr(0, 400));
            Rig b;
            run2(b, true);
            b.p.add({ T });                                    // (current, not playing: Transcribe all's own path)
            b.p.transcribeAll();
            b.h.now += pbplayer::kTcWaitMs;
            b.p.tick();
            ok("[FX] F1: Transcribe all with the probe SILENT past kTcWaitMs (a sleeping disk, a slow share): the timecoded Word file "
               "is up to date -- NOTHING rewritten (never file-relative stamps over source ones); the file leaves the batch",
               b.h.docx.empty() && !b.p.tbatchQueued().count(b.p.items()[0].id), n2s(b.h.docx.size()) + " written");
            Rig f;
            run2(f, false);
            f.h.tcMap.clear();                                 // (the probe answers "none": the read FAILED -- the file held elsewhere)
            f.p.add({ T });
            f.p.transcribeAll();
            Rig fo;
            run2(fo, false);
            fo.h.tcMap.clear();
            fo.p.open({ T }, false);
            const std::wstring tf = fo.h.lastOf("translatorTranscript");
            ok("[FX] F1: a timecode read that FAILS (answered \"none\"): Transcribe all rewrites nothing, the tab is not \"older\" and "
               "keeps the source stamps -- a recorded source timecode beats a live \"none\" (the same file, by identity)",
               f.h.docx.empty() && !fb(tf, L"older") && a(tf).find("\"tc\":\"[10:00:01:00]\"") != std::string::npos,
               n2s(f.h.docx.size()) + " / " + a(tf).substr(0, 200));
            Rig g;
            run2(g, true);
            g.p.open({ T }, false);
            g.p.renameSpeaker(g.p.session().gen, vrevOf(g.p), 1, "Anna");
            g.h.now += pbplayer::kDocxRenameMs;
            g.p.tick();
            ok("[FX] F1: a rename while the probe is silent: G2 rewrites ONCE, in place, WITH the source stamps and timecode",
               g.h.rewrites == 1 && g.h.docx.size() == 1 && g.h.docx[0].t.tc.found && g.h.docx[0].t.turns[0].stamp == "[10:00:01:00]"
               && g.h.docx[0].t.turns[0].name == "Anna", g.h.docx.empty() ? std::string("no rewrite") : g.h.docx[0].t.turns[0].stamp);
            // kept on purpose: a Word file written while NO timecode was known (no DOCXTC) is "older" once one is READ (it lacks it)
            Rig u1;
            u1.h.addFile(T, 5000, 11);
            u1.h.cache[keyT] = fxRecord(T, 5000, 11);
            u1.p.open({ T }, false);
            u1.p.transcribe(false);
            fxVoices(u1, 0);
            const bool noTc = u1.h.voicesDisk[keyT].find("DOCXTC") == std::string::npos && u1.h.docx.size() == 1
                && u1.h.docx[0].t.turns[0].stamp == "[00:00:01]";
            Rig u2;
            u2.h.disk = u1.h.disk; u2.h.cache = u1.h.cache; u2.h.voicesDisk = u1.h.voicesDisk;
            u2.h.tcMap[pbtrf::lowerW(T)] = tc25;
            u2.p.open({ T }, false);
            const bool olderNow = fb(u2.h.lastOf("translatorTranscript"), L"older");
            u2.p.transcribeAll();
            ok("[FX] F1: kept -- a Word file written with file-relative stamps (no DOCXTC) is \"older\" once a timecode is READ (true: "
               "it lacks it); Transcribe all brings it up to date with the source stamps, and its record keeps them now",
               noTc && olderNow && u2.h.docx.size() == 1 && u2.h.docx[0].t.turns[0].stamp == "[10:00:01:00]"
               && u2.h.voicesDisk[keyT].find("\nDOCXTC\t") != std::string::npos, n2s(u2.h.docx.size()));
            Rig w2;                                           // ...and a G2 rewrite records the timecode it wrote, too
            w2.h.disk = u1.h.disk; w2.h.cache = u1.h.cache; w2.h.voicesDisk = u1.h.voicesDisk;
            w2.h.tcMap[pbtrf::lowerW(T)] = tc25;
            w2.p.open({ T }, false);
            w2.p.renameSpeaker(w2.p.session().gen, vrevOf(w2.p), 1, "Anna");
            w2.h.now += pbplayer::kDocxRenameMs;
            w2.p.tick();
            Rig w3;
            w3.h.disk = w2.h.disk; w3.h.cache = w2.h.cache; w3.h.voicesDisk = w2.h.voicesDisk;
            w3.h.autoTc = false;
            w3.p.open({ T }, false);
            ok("[FX] F1: a G2 rewrite records the timecode it wrote (DOCXTC): the next run, its probe silent, finds it up to date",
               w2.h.rewrites == 1 && w2.h.voicesDisk[keyT].find("\nDOCXTC\t") != std::string::npos
               && !fb(w3.h.lastOf("translatorTranscript"), L"older"), w2.h.voicesDisk[keyT].substr(0, 300));
            Rig s;                                            // Save .docx...: the file it saved keeps the timecode of its stamps
            s.h.addFile(T, 5000, 11);
            s.h.cache[keyT] = fxRecord(T, 5000, 11);
            s.h.tcMap[pbtrf::lowerW(T)] = tc25;
            s.p.open({ T }, false);
            s.p.transcribe(false);
            fxVoices(s, 0);
            pbtrs::TranscriptOut so;
            s.p.transcriptForSave(so);
            const std::wstring saved = L"D:\\Deliverables\\take1 - transcript.docx";
            s.h.disk[pbtrf::lowerW(saved)] = { 7777, 88 };
            s.p.transcriptSaved(saved, 7777, 88, so.sig, so.tc);
            pbtrf::VoiceRec vs;
            Rig s2;                                           // ...and over a record whose Word file had NO timecode (u1's): the
            s2.h.disk = u1.h.disk; s2.h.cache = u1.h.cache; s2.h.voicesDisk = u1.h.voicesDisk;   // Save records the one it holds NOW
            s2.h.tcMap[pbtrf::lowerW(T)] = tc25;
            s2.p.open({ T }, false);
            pbtrs::TranscriptOut so2;
            s2.p.transcriptForSave(so2);
            const std::wstring saved2 = L"D:\\Deliverables\\take1 - transcript (2).docx";
            s2.h.disk[pbtrf::lowerW(saved2)] = { 7778, 89 };
            s2.p.transcriptSaved(saved2, 7778, 89, so2.sig, so2.tc);
            pbtrf::VoiceRec vs2;
            const bool s2ok = s2.h.readVoices(keyT, vs2) && pbtrf::samePath(vs2.docxPath, saved2) && vs2.docxTc.found
                && vs2.docxTc.startFrames == 900000;
            ok("[FX] F1: Save .docx... -- the file saved becomes the Word file WITH the timecode of what it holds (DOCXTC) -- also over "
               "a record whose Word file had none",
               s.h.readVoices(keyT, vs) && pbtrf::samePath(vs.docxPath, saved) && vs.docxTc.found && vs.docxTc.startFrames == 900000 && s2ok);
            ok("every post well-formed (FX F1)", r1.h.badJson == 0 && q.h.badJson == 0 && b.h.badJson == 0 && g.h.badJson == 0,
               r1.h.lastBadJson + q.h.lastBadJson);
        }

        // ---- F2: THE USER'S RULE -- ONE Word file per media AND sound track (regMode: the bridge's rules) ----
        auto twoTracks = []() {
            auto i = std::make_shared<pbmedia::Info>();
            i->state = pbmedia::Info::State::Ready; i->container = "mkv"; i->hasVideo = true;
            i->video.codec = "H.264"; i->video.w = 640; i->video.h = 360;
            pbmedia::AudioTrack t0; t0.ord = 0; t0.track = 2; t0.lang = "fre"; t0.codec = "AAC"; t0.def = true; t0.play = true; t0.ch = 2;
            pbmedia::AudioTrack t1 = t0; t1.ord = 1; t1.track = 3; t1.lang = "eng"; t1.def = false;
            i->audio = { t0, t1 };
            return std::shared_ptr<const pbmedia::Info>(i);
        };
        // the playing file's analysis (ONE FILE: two lines, done) and then its voice pass, through the core's own path
        auto analyseTranscribe = [&](Rig& r) {
            r.p.transcribe(false);
            if (r.h.files.empty()) return false;
            const uint64_t job = r.h.files.back().jobId;
            r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
            r.p.onCue(cue(job, 0, 1000, 4000, "Good morning everyone."));
            r.p.onCue(cue(job, 1, 5000, 9000, "I agree with you."));
            r.p.onDone(done(job, "ok"), false);
            return fxVoices(r, r.h.voicesSent.size() - 1);
        };
        {
            const std::wstring Film2 = L"D:\\Films\\Film.mkv";
            const std::wstring plain = L"D:\\Films\\Film - transcript.docx", track2 = L"D:\\Films\\Film - transcript (track 2).docx";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(Film2, 7000, 13);
            r.h.probeMap[pbtrf::lowerW(Film2)] = twoTracks();
            r.p.open({ Film2 }, false);
            analyseTranscribe(r);                              // the first sound track (ord 0, the default one)
            const bool first = r.h.docx.size() == 1 && pbtrf::lowerW(r.h.docx[0].path) == pbtrf::lowerW(plain);
            const auto plainId = r.h.disk[pbtrf::lowerW(plain)];
            r.p.setAudio(1, false);                            // the user moves to track 2 (its own analysis, "1:3")
            analyseTranscribe(r);
            ok("[FX] F2: two sound tracks of one film = TWO Word files -- \"Film - transcript.docx\" (the first track) and \"Film - "
               "transcript (track 2).docx\" (N = the track menu's number); the first one untouched",
               first && r.h.docx.size() == 2 && pbtrf::lowerW(r.h.docx[1].path) == pbtrf::lowerW(track2) && r.h.files.back().audio == "1:3"
               && r.h.disk[pbtrf::lowerW(plain)] == plainId, r.h.docx.size() > 1 ? a(r.h.docx[1].path) : std::string("one file"));
            r.p.setAudio(0, false);                            // back on the first track
            const std::wstring back = r.h.lastOf("translatorTranscript");
            ok("[FX] F2: back on the first track: its Word file is current -- never \"changed outside SuVtitle\" because another track wrote",
               fs(back, L"file") == "Film - transcript.docx" && !fb(back, L"edited") && !fb(back, L"other") && !fb(back, L"older"),
               a(back).substr(0, 300));
            r.h.disk[pbtrf::lowerW(plain)] = { 8123, 45 };    // the user EDITS the first track's Word file in Word
            r.p.setAudio(1, false);
            r.p.setAudio(0, false);
            const std::wstring ed = r.h.lastOf("translatorTranscript");
            ok("[FX] F2: ...that Word file edited in Word: \"changed outside SuVtitle\" -- its OWN analysis wrote it, never \"another "
               "transcript\"", fb(ed, L"edited") && !fb(ed, L"other"), a(ed).substr(0, 300));
            ok("every post well-formed (FX F2 tracks)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            const std::wstring I = L"D:\\Interviews\\interview.wav", named = L"D:\\Interviews\\interview - transcript.docx";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(I, 4000, 21);
            r.p.setPair("fr", "en", "auto");
            r.p.open({ I }, false);
            analyseTranscribe(r);                              // the fr/en analysis and its Word file
            r.p.renameSpeaker(r.p.session().gen, vrevOf(r.p), 1, "Marie");
            r.p.renameSpeaker(r.p.session().gen, vrevOf(r.p), 2, "Paul");
            r.h.now += pbplayer::kDocxRenameMs;
            r.p.tick();                                        // (G2: the names in its Word file)
            const auto namedId = r.h.disk[pbtrf::lowerW(named)];
            const size_t d0 = r.h.docx.size();
            const int w0 = r.h.writeTries;
            const std::string keyPair1 = r.p.session().set->key;
            const bool namedOk = d0 == 2 && r.h.docx.back().t.turns[0].name == "Marie";
            r.p.setPair("fr", "zh", "auto");                   // another film made him change the pair
            analyseTranscribe(r);                              // Transcribe pressed again on the interview: the fr/zh analysis
            const std::wstring tr = r.h.lastOf("translatorTranscript");
            ok("[FX] F2: another analysis of the SAME track (the pair fr/zh) writes NOTHING -- not asked even (the core's own rule): the "
               "named Word file untouched (identity, \"Marie\"), its record's names kept, no new file",
               namedOk && r.h.docx.size() == d0 && r.h.writeTries == w0 && r.h.disk[pbtrf::lowerW(named)] == namedId
               && r.h.voicesDisk[keyPair1].find("\nNAME\t1\tMarie\n") != std::string::npos
               && countNotice(r.h, "Transcribe all:") == 0,              // (no Transcribe all here: never its notice)
               n2s(r.h.docx.size()) + " files, " + n2s(r.h.writeTries) + " writes asked, "
               + n2s(countNotice(r.h, "Transcribe all:")) + " Transcribe all notice(s)");
            ok("[FX] F2: ...its tab says so (`other`), names no Word file of its own, never \"changed outside SuVtitle\", never older",
               fb(tr, L"other") && fs(tr, L"file").empty() && !fb(tr, L"edited") && !fb(tr, L"older"), a(tr).substr(0, 300));
            const int rw0 = r.h.rewriteTries;
            r.p.renameSpeaker(r.p.session().gen, vrevOf(r.p), 1, "Zhang");
            r.h.now += pbplayer::kDocxRenameMs;
            r.p.tick();
            pbtrs::TranscriptOut so;
            r.p.transcriptForSave(so);
            ok("[FX] F2: a rename in that other analysis: G2 never touches the track's Word file; Save .docx... is not offered it",
               r.h.rewriteTries == rw0 && r.h.disk[pbtrf::lowerW(named)] == namedId && so.prefer.empty(), n2s(r.h.rewriteTries - rw0));
            r.p.transcribe(true);                              // Re-transcribe in the other analysis
            fxVoices(r, r.h.voicesSent.size() - 1);
            ok("[FX] F2: ...and its Re-transcribe writes nothing either (the named file stays as it is)",
               r.h.docx.size() == d0 && r.h.disk[pbtrf::lowerW(named)] == namedId);
            Rig n;                                            // a NEW run on fr/zh: the playing file's row says it from the registry
            n.h.regMode = true;
            n.h.disk = r.h.disk; n.h.cache = r.h.cache; n.h.voicesDisk = r.h.voicesDisk; n.h.reg = r.h.reg;
            n.p.setPair("fr", "zh", "auto");
            n.p.open({ I }, false);
            n.p.flush();
            const std::string nRow = n.p.items().empty() ? std::string("<no item>") : rowField(n.h, n.p.items()[0].id, "tdoc");
            ok("[FX] F2: a new run on the fr/zh analysis: the playing file's row says the track's Word file holds another transcript "
               "(never \"No Word file\")", nRow.find("holds another transcript") != std::string::npos, nRow);
            pbtrs::TranscriptOut sb;                           // Save .docx... in fr/zh: the user CHOSE the named file in its dialog
            r.p.transcriptForSave(sb);                         // (and said yes to Windows' overwrite prompt): written, registered fr/zh's
            r.h.disk[pbtrf::lowerW(named)] = { 6100, 61 };
            pbtrf::registryPut(r.h.reg, named, 6100, 61, I, pbtrf::transcriptRegKind(), r.p.session().set->key, 0);
            r.p.transcriptSaved(named, 6100, 61, sb.sig, sb.tc);
            const size_t d1 = r.h.docx.size();
            r.p.setPair("fr", "en", "auto");
            r.p.transcribe(false);                             // back on the fr/en analysis (its transcript complete)
            const std::wstring tA = r.h.lastOf("translatorTranscript");
            ok("[FX] F2: Save .docx... in fr/zh OVER the named file: back on fr/en, its tab says another analysis' transcript holds it -- "
               "never \"changed outside SuVtitle\" (SuVtitle wrote it) -- and nothing is rewritten",
               fb(tA, L"other") && !fb(tA, L"edited") && r.h.docx.size() == d1 && r.h.disk[pbtrf::lowerW(named)].first == 6100,
               a(tA).substr(0, 300));
            ok("every post well-formed (FX F2 pair)", r.h.badJson == 0 && n.h.badJson == 0, r.h.lastBadJson + n.h.lastBadJson);
        }
        {
            // Transcribe all after a pair change: no write for those files, each row says why, ONE notice counts them
            // (the PLAYING file keeps its SHOWN analysis in Transcribe all -- U3's decision 17 -- so the two files here are not it)
            const std::wstring X0 = L"D:\\Interviews\\zero.wav", X1 = L"D:\\Interviews\\one.wav", X2 = L"D:\\Interviews\\two.wav";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(X0, 4000, 30); r.h.addFile(X1, 4100, 31); r.h.addFile(X2, 4200, 32);
            r.p.setPair("fr", "en", "auto");
            r.p.open({ X0 }, false);
            r.p.add({ X1, X2 });
            size_t fAns = 0, vAns = 0;
            auto drive = [&]() {                               // answers every FILE and FVOICE the batch sends, until none is left
                for (int guard = 0; guard < 40; ++guard) {
                    if (fAns < r.h.files.size()) {
                        const uint64_t job = r.h.files[fAns++].jobId;
                        r.p.onProg(prog(job, "analyzing", 600000, "0-600000"));
                        r.p.onCue(cue(job, 0, 1000, 4000, "Good morning everyone."));
                        r.p.onCue(cue(job, 1, 5000, 9000, "I agree with you."));
                        r.p.onDone(done(job, "ok"), false);
                    } else if (vAns < r.h.voicesSent.size()) {
                        fxVoices(r, vAns++);
                    } else {
                        break;
                    }
                }
            };
            r.p.transcribeAll();
            drive();
            const size_t d0 = r.h.docx.size();
            const size_t n0 = countNotice(r.h, "Transcribe all:");
            r.p.setPair("fr", "zh", "auto");
            r.p.transcribeAll();
            drive();
            const uint64_t i1 = r.p.items()[1].id, i2 = r.p.items()[2].id;
            ok("[FX] F2 (rule 4): Transcribe all after a pair change -- two new analyses run, NO Word file written (each track's file "
               "holds the fr/en transcript), each row says so, and ONE notice counts them",
               d0 == 3 && n0 == 0 && r.h.files.size() == 5 && r.h.voicesSent.size() == 5 && r.h.docx.size() == d0 && rowField(r.h, i1, "tdoc").find("holds another transcript") != std::string::npos
               && rowField(r.h, i2, "tdoc").find("holds another transcript") != std::string::npos
               && countNotice(r.h, "Transcribe all: 2 files have no up-to-date Word file next to them") == 1 && countNotice(r.h, "Transcribe all:") == 1,
               n2s(r.h.docx.size()) + " files; " + n2s(r.h.files.size()) + " FILE, " + n2s(r.h.voicesSent.size()) + " FVOICE; d0 " + n2s(d0)
               + " n0 " + n2s(n0) + "; rows: [" + rowField(r.h, i1, "tdoc") + "] [" + rowField(r.h, i2, "tdoc") + "] tstate "
               + rowField(r.h, i1, "tstate") + "; notices " + n2s(countNotice(r.h, "Transcribe all:")));
            Rig n;                                            // a NEW run on fr/zh, Transcribe all again: both transcribed, nothing to
            n.h.regMode = true;                               // run -- their Word file rule meets the other analysis' files
            n.h.disk = r.h.disk; n.h.cache = r.h.cache; n.h.voicesDisk = r.h.voicesDisk; n.h.reg = r.h.reg;
            n.p.setPair("fr", "zh", "auto");
            n.p.add({ X1, X2 });
            n.p.transcribeAll();
            const std::string m1 = rowField(n.h, n.p.items()[0].id, "tdoc"), m2 = rowField(n.h, n.p.items()[1].id, "tdoc");
            ok("[FX] F2 (rule 4): ...a NEW run, Transcribe all again: nothing runs, NOTHING written, each row says why, ONE notice",
               n.h.files.empty() && n.h.voicesSent.empty() && n.h.docx.empty() && n.h.writeTries == 0
               && m1.find("holds another transcript") != std::string::npos && m2.find("holds another transcript") != std::string::npos
               && countNotice(n.h, "Transcribe all: 2 files have no up-to-date Word file next to them") == 1,
               n2s(n.h.files.size()) + " FILE, " + n2s(n.h.voicesSent.size()) + " FVOICE, " + n2s(n.h.docx.size()) + " written; [" + m1
               + "] [" + m2 + "]; notices " + n2s(countNotice(n.h, "Transcribe all:")));
            ok("every post well-formed (FX F2 batch)", r.h.badJson == 0 && n.h.badJson == 0, r.h.lastBadJson + n.h.lastBadJson);
        }
        {
            // the integrator's condition 2: a registry entry with NO key (an older registry) owns nothing -- never replaced by itself
            const std::wstring K = L"D:\\Old\\talk.wav", kd = L"D:\\Old\\talk - transcript.docx";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(K, 4300, 41);
            r.h.disk[pbtrf::lowerW(kd)] = { 999, 77 };
            pbtrf::RegEntry e;
            e.path = pbtrf::toUtf8(pbtrf::lowerW(kd)); e.size = 999; e.mtime = 77; e.media = pbtrf::toUtf8(pbtrf::lowerW(K));
            r.h.reg.push_back(e);
            r.h.cache[fxKey(K, 4300, 41)] = fxRecord(K, 4300, 41);
            r.p.open({ K }, false);
            r.p.transcribe(false);
            fxVoices(r, 0);
            ok("[FX] F2 (condition 2): a Word file an older registry lists WITHOUT a key is never replaced by itself -- the transcript "
               "takes \"talk - transcript (SuVtitle).docx\" beside it, the old one untouched",
               r.h.docx.size() == 1 && pbtrf::lowerW(r.h.docx[0].path) == pbtrf::lowerW(L"D:\\Old\\talk - transcript (SuVtitle).docx")
               && r.h.disk[pbtrf::lowerW(kd)].first == 999 && r.h.disk[pbtrf::lowerW(kd)].second == 77,
               r.h.docx.empty() ? std::string("none") : a(r.h.docx[0].path));
        }

        // ---- F4: a Word file HELD by another program (or read-only) is said with "older", never written elsewhere ----
        {
            const std::wstring H = L"D:\\Interviews\\meeting.wav", hd = L"D:\\Interviews\\meeting - transcript.docx";
            const std::string keyH = fxKey(H, 4400, 51);
            auto held = [&](Rig& r, bool readOnly) {
                r.h.regMode = true;
                r.h.addFile(H, 4400, 51);
                r.h.cache[keyH] = fxRecord(H, 4400, 51);
                r.h.tcMap[pbtrf::lowerW(H)] = tc25;
                r.p.open({ H }, false);
                r.p.transcribe(false);
                fxVoices(r, 0);
                (readOnly ? r.h.readOnlyDocx : r.h.heldDocx).insert(pbtrf::lowerW(hd));
                r.p.transcribe(true);                          // Re-transcribe (its Word file open in Word / read-only): the voices
                fxVoices(r, 1, true);                          // numbered the other way -- the transcript moved, its file is older
            };
            Rig r;
            held(r, false);
            const std::wstring tr = r.h.lastOf("translatorTranscript");
            pbtrf::VoiceRec vr;
            const bool rec = r.h.readVoices(keyH, vr);
            ok("[FX] F4: Re-transcribe while the Word file is OPEN in Word: nothing written anywhere else; the record still names the "
               "file next to the media (and its timecode); the tab: older + \"The Word file is open in another program: close it to "
               "update it.\" -- never \"could not be written to\"",
               r.h.docx.size() == 1 && rec && pbtrf::samePath(vr.docxPath, hd) && vr.docxTc.found && fb(tr, L"older")
               && fs(tr, L"docxErr") == "The Word file is open in another program: close it to update it." && fs(tr, L"note").empty(),
               a(tr).substr(0, 400));
            r.p.transcribeAll();
            const bool stillOne = r.h.docx.size() == 1;
            const std::string heldRow = rowField(r.h, r.p.items()[0].id, "tdoc");   // (F6: its row says why, too)
            r.h.heldDocx.clear();                              // Word closed it
            r.p.transcribeAll();
            const std::wstring t2 = r.h.lastOf("translatorTranscript");
            const std::string freeRow = rowField(r.h, r.p.items()[0].id, "tdoc");
            ok("[FX] F4: Transcribe all while it is held: nothing else written, its row says why; once released, Transcribe all "
               "rewrites it IN PLACE (the sentence gone from the tab AND the row, current)",
               stillOne && r.h.docx.size() == 2 && pbtrf::lowerW(r.h.docx.back().path) == pbtrf::lowerW(hd) && fs(t2, L"docxErr").empty()
               && !fb(t2, L"older") && heldRow.find("open in another program") != std::string::npos && freeRow.empty(),
               n2s(r.h.docx.size()) + " / [" + heldRow + "] -> [" + freeRow + "]");
            Rig o;
            held(o, true);
            const std::wstring to = o.h.lastOf("translatorTranscript");
            ok("[FX] F4: a READ-ONLY Word file: the same path (kept, said with older, nothing elsewhere) with its OWN sentence -- never "
               "\"open in another program\"",
               o.h.docx.size() == 1 && fb(to, L"older") && fs(to, L"docxErr").find("The Word file is read-only: it is left as it is.") == 0
               && fs(to, L"docxErr").find("another program") == std::string::npos, a(to).substr(0, 400));
            ok("every post well-formed (FX F4)", r.h.badJson == 0 && o.h.badJson == 0, r.h.lastBadJson + o.h.lastBadJson);
        }

        // ---- F5: "Redone with the default model" only once the redo STARTED (or ended) ----
        {
            using MP = Player::ModelPick;
            const std::string mAsr = "D:\\Models\\ggml-base.bin";
            const std::string idAsr = pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 148000000, 77);
            const std::string errAsr = std::string(tp::kFileModelErrAsr) + "ggml-base.bin\" could not be used: the speech recogniser would not start (GPU or CPU).";
            const std::wstring P1 = L"D:\\Batch\\one.wav", P2 = L"D:\\Batch\\two.wav";
            // the race the review's case rests on: B, Translate all's, on an imported model; A opened and PRESSED (it takes the engine:
            // B's job cancelled); B's FDONE "error" (its model failed) crosses the cancel -> its redo is granted, but it WAITS
            auto race = [&](Rig& r, uint64_t& idB) {
                r.h.addFile(P1, 1000, 1); r.h.addFile(P2, 2000, 2);
                r.p.setModels(MP{ mAsr, idAsr }, MP{});
                r.p.add({ P2 });
                r.p.analyseAll();
                idB = r.p.items()[0].id;
                const uint64_t jB = r.h.files.back().jobId;
                r.p.open({ P1 }, false);
                r.p.analyse(false);
                r.p.onDone(done(jB, "error", errAsr.c_str()), false);
            };
            Rig r;
            uint64_t idB = 0;
            race(r, idB);
            const size_t f0 = r.h.files.size();
            const bool waits = rowField(r.h, idB, "redo", true) == "false" && rowField(r.h, idB, "redo") == "false" && f0 == 2;
            r.p.cancelAll();
            ok("[FX] F5: a redo GRANTED but waiting (the engine is the pressed file's): its row does NOT say \"Redone with the default "
               "model\" -- nor after a Cancel all that came before it ran (cancelled, no redo said)",
               waits && rowField(r.h, idB, "redo") == "false" && badgeNow(r.h, idB) == "cancelled",
               rowField(r.h, idB, "redo") + " / " + badgeNow(r.h, idB) + " / " + n2s(f0));
            const size_t f1 = r.h.files.size();
            r.p.open({ P2 }, false);                           // ...then the user opens B and PRESSES Translate: his own analysis
            r.p.analyse(false);
            if (r.h.files.size() == f1) r.p.onDone(done(r.h.files[1].jobId, "cancelled"), false);   // (A's job dies first)
            const uint64_t jU = r.h.files.back().jobId;
            r.p.onProg(prog(jU, "analyzing", 600000, "0-600000"));
            r.p.onDone(done(jU, "ok"), false);
            ok("[FX] F5: ...B pressed by the user after that Cancel all, its analysis done: never \"Redone with the default model\" "
               "(the batch never ran its redo)",
               r.h.files.size() == f1 + 1 && r.p.session().set && r.p.session().set->complete && r.p.session().itemId == idB
               && rowField(r.h, idB, "redo") == "false",
               rowField(r.h, idB, "redo") + " / " + n2s(r.h.files.size() - f1) + " FILE");
            Rig s;
            uint64_t idS = 0;
            race(s, idS);
            s.p.onDone(done(s.h.files.back().jobId, "ok"), false);   // the pressed file done: the engine free
            ok("[FX] F5: ...the engine free, its redo STARTS (a FILE naming no model) -- only now the row says it",
               s.h.files.size() == 3 && s.h.files.back().asrModel.empty() && rowField(s.h, idS, "redo") == "true",
               rowField(s.h, idS, "redo") + " / " + n2s(s.h.files.size()));
            s.p.onDone(done(s.h.files.back().jobId, "error", "The analysis failed."), false);   // the redo's own analysis failed
            const bool saidAfter = rowField(s.h, idS, "redo") == "true";                      // (it WAS redone: still said)
            s.p.analyseAll();                                  // Translate all pressed again: a new press, a plain run on the default
            ok("[FX] F5: Translate all pressed again after that redo failed: its row no longer says \"Redone with the default model\" "
               "(the new press runs it on the default model: no redo)",
               saidAfter && s.h.files.size() == 4 && s.h.files.back().asrModel.empty() && rowField(s.h, idS, "redo") == "false",
               rowField(s.h, idS, "redo") + " / " + n2s(s.h.files.size()));
            Rig t;
            t.h.addFile(P1, 1000, 1);
            t.h.cache[fxKey(P1, 1000, 1)] = fxRecord(P1, 1000, 1);     // the DEFAULT model's complete analysis of it
            t.p.setModels(MP{ mAsr, idAsr }, MP{});
            t.p.open({ P1 }, false);
            t.p.analyseAll();                                   // (the playing file is the list's)
            const uint64_t idA = t.p.items()[0].id;
            const size_t g0 = t.h.files.size();
            t.p.onDone(done(t.h.files.back().jobId, "error", errAsr.c_str()), false);
            ok("[FX] F5: the PLAYING file's redo met by the default's complete analysis (no FILE): it ENDED at once -- its row says it",
               g0 == 1 && t.h.files.size() == 1 && rowField(t.h, idA, "redo") == "true", rowField(t.h, idA, "redo") + " / " + n2s(t.h.files.size()));
            ok("every post well-formed (FX F5)", r.h.badJson == 0 && s.h.badJson == 0 && t.h.badJson == 0, r.h.lastBadJson);
        }

        // ---- F6: each row says where its Word file is, or why there is none; ONE notice counts them for Transcribe all ----
        {
            const std::wstring D1 = L"D:\\Card\\a.wav", D2 = L"D:\\Card\\b.wav", D3 = L"D:\\Card\\c.wav";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(D1, 100, 1); r.h.addFile(D2, 200, 2); r.h.addFile(D3, 300, 3);
            r.h.cache[fxKey(D1, 100, 1)] = fxRecord(D1, 100, 1);
            r.h.cache[fxKey(D2, 200, 2)] = fxRecord(D2, 200, 2);
            r.h.cache[fxKey(D3, 300, 3)] = fxRecord(D3, 300, 3);
            r.h.awayMedia.insert(pbtrf::lowerW(D2));           // b's folder refuses the .tmp -> Documents\SuVtitle Transcripts
            for (const auto& p : pbtrf::transcriptCandidates(L"D:\\Card", L"c", L"wav", false))   // c's ten names: someone else's
                r.h.disk[pbtrf::lowerW(p)] = { 12, 34 };
            r.p.add({ D1, D2, D3 });
            r.p.transcribeAll();
            for (size_t v = 0; v < 6 && v < r.h.voicesSent.size(); ++v) fxVoices(r, v);   // each file's voice pass, in the list's order
            const uint64_t i1 = r.p.items()[0].id, i2 = r.p.items()[1].id, i3 = r.p.items()[2].id;
            const std::string t1 = rowField(r.h, i1, "tdoc"), t2 = rowField(r.h, i2, "tdoc"), t3 = rowField(r.h, i3, "tdoc");
            ok("[FX] F6: Transcribe all -- a's Word file next to it (its row says nothing: tdoc \"\"); b's in Documents (\"Word file in "
               "Documents\\SuVtitle Transcripts\"); c none (why: every name is taken)",
               r.h.voicesSent.size() == 3 && t1.empty() && t2.find("Word file in Documents") == 0 && t2.find("SuVtitle Transcripts") != std::string::npos
               && t3.find("every transcript name for this file is taken") != std::string::npos,
               "[" + t1 + "] [" + t2 + "] [" + t3 + "]");
            ok("[FX] F6: ...and ONE notice counts them when the list is done", countNotice(r.h, "Transcribe all:") == 1
               && countNotice(r.h, "Transcribe all: 2 files have no up-to-date Word file next to them -- their rows in the Queue tab say why.") == 1,
               n2s(countNotice(r.h, "Transcribe all:")));
            r.p.transcribeAll();                               // pressed again: a and b up to date, c still has no name -> 1
            ok("[FX] F6: pressed again: one new notice for that press (c's, \"1 file has no up-to-date Word file next to it\")",
               countNotice(r.h, "Transcribe all: 1 file has no up-to-date Word file next to it") == 1 && countNotice(r.h, "Transcribe all:") == 2,
               n2s(countNotice(r.h, "Transcribe all:")));
            Rig n;                                            // a NEW run on the same records: the rows know it from the records
            n.h.regMode = true;
            n.h.disk = r.h.disk; n.h.cache = r.h.cache; n.h.voicesDisk = r.h.voicesDisk; n.h.reg = r.h.reg;
            n.p.add({ D1, D2, D3 });
            n.p.flush();
            ok("[FX] F6: a new run: b's row still says where its Word file is (its record), c's \"No Word file\" (the why was that run's)",
               rowField(n.h, n.p.items()[1].id, "tdoc").find("Word file in Documents") == 0 && rowField(n.h, n.p.items()[2].id, "tdoc") == "No Word file"
               && rowField(n.h, n.p.items()[0].id, "tdoc").empty(),
               rowField(n.h, n.p.items()[1].id, "tdoc") + " | " + rowField(n.h, n.p.items()[2].id, "tdoc"));
            Rig e;                                            // a file with NO speech, Transcribe pressed (not the list): its row says why
            const std::wstring D4 = L"D:\\Card\\silence.wav";
            e.h.addFile(D4, 400, 4);
            pbtrf::Record quiet;
            quiet.path = a(D4); quiet.size = 400; quiet.mtime = 4; quiet.langA = "en"; quiet.langB = "fr"; quiet.mode = "auto";
            quiet.quality = "auto"; quiet.complete = true; quiet.totalMs = 600000; quiet.engine = tp::kFileEngineVersion;
            quiet.ranges.push_back(tp::MsRange{ 0, 600000 });
            e.h.cache[fxKey(D4, 400, 4)] = pbtrf::packRecord(quiet);
            e.p.open({ D4 }, false);
            e.p.transcribe(false);
            const std::string qRow = e.p.items().empty() ? std::string("<no item>") : rowField(e.h, e.p.items()[0].id, "tdoc");
            ok("[FX] F6: a file with NO speech, Transcribe pressed: its row says why there is no Word file -- and no Transcribe all "
               "notice outside Transcribe all", e.h.voicesSent.empty() && qRow == "No speech was found in this file."
               && countNotice(e.h, "Transcribe all:") == 0, "[" + qRow + "]");
            ok("every post well-formed (FX F6)", r.h.badJson == 0 && n.h.badJson == 0 && e.h.badJson == 0,
               r.h.lastBadJson + n.h.lastBadJson + e.h.lastBadJson);
        }

        // ---- F7: the rows say which files the LIST has (`batch`: what Cancel all acts on) ----
        {
            const std::wstring S1 = L"D:\\List\\a.wav", S2 = L"D:\\List\\b.wav", S3 = L"D:\\List\\c.wav";
            Rig r;
            r.h.addFile(S1, 100, 1); r.h.addFile(S2, 200, 2);
            r.p.open({ S1, S2 }, false);
            r.p.analyse(false);                                // S1 pressed alone
            const uint64_t i1 = r.p.items()[0].id, i2 = r.p.items()[1].id;
            const bool pressed = rowField(r.h, i1, "batch") == "false" && rowField(r.h, i2, "batch") == "false";
            r.p.analyseAll();                                  // the list: S1 (pressed AND the list's) and S2
            const bool listed = rowField(r.h, i1, "batch", true) == "true" && rowField(r.h, i2, "batch", true) == "true";
            r.p.onDone(done(r.h.files.back().jobId, "ok"), false);   // S1 done: the list goes on with S2 (a batch job)
            r.p.onProg(prog(r.h.files.back().jobId, "analyzing", 600000, "0-60000"));
            r.p.flush();
            const bool rowOwn = typeOf(r.h.window.back()) == "playerItem" && rowField(r.h, i2, "batch") == "true"
                && rowField(r.h, i1, "batch") == "false";      // (S2's progress: its row's own update; S1 left the list)
            r.p.cancelAll();
            ok("[FX] F7: each row says whether the LIST has it -- a file pressed alone: false; Translate all's: true (the whole list, "
               "and a row's own update); one done: false; after Cancel all: false",
               pressed && listed && rowOwn && rowField(r.h, i1, "batch") == "false" && rowField(r.h, i2, "batch") == "false",
               std::string(pressed ? "" : "pressed ") + (listed ? "" : "listed ") + (rowOwn ? "" : "rowOwn ") + rowField(r.h, i2, "batch"));
            Rig t;
            t.h.addFile(S3, 300, 3);
            t.p.add({ S3 });
            t.p.transcribeAll();                               // Transcribe all's file
            ok("[FX] F7: Transcribe all's files are the list's too", rowField(t.h, t.p.items()[0].id, "batch", true) == "true",
               rowField(t.h, t.p.items()[0].id, "batch", true));
        }

        // ---- F8: a rename names a speaker of the transcript it was TYPED for ----
        {
            const std::wstring T3 = L"D:\\Takes\\take3.wav", T4 = L"D:\\Takes\\take4.wav";
            const std::string k3 = fxKey(T3, 1300, 3), k4 = fxKey(T4, 1400, 4);
            Rig prep;                                         // both takes transcribed (their records), as a session would leave them
            prep.h.addFile(T3, 1300, 3); prep.h.addFile(T4, 1400, 4);
            prep.h.cache[k3] = fxRecord(T3, 1300, 3); prep.h.cache[k4] = fxRecord(T4, 1400, 4);
            prep.p.open({ T3 }, false); prep.p.transcribe(false); fxVoices(prep, 0);
            prep.p.open({ T4 }, false); prep.p.transcribe(false); fxVoices(prep, 1);
            Rig r;
            r.h.disk = prep.h.disk; r.h.cache = prep.h.cache; r.h.voicesDisk = prep.h.voicesDisk;
            r.p.open({ T3, T4 }, false);                       // take 3 plays; take 4 next
            const uint64_t g3 = r.p.session().gen, v3 = vrevOf(r.p);
            r.p.pos(g3, 59000, 60000, false, "ended");         // take 3 ends: take 4 starts by itself (auto-advance)
            const bool advanced = r.p.session().path == T4;
            const int rw0 = r.h.rewriteTries;
            std::wstring why;
            const bool took = r.p.renameSpeaker(g3, v3, 2, "Marie", &why);   // the name typed in take 3's field, Enter now
            r.h.now += pbplayer::kDocxRenameMs;
            r.p.tick();
            ok("[FX] F8: a name typed for take 3, sent after take 4 started: REFUSED with one sentence -- take 4's speakers and Word "
               "file untouched (nobody renamed \"Marie\", no rewrite)",
               advanced && !took && why == L"The speaker was not renamed: the transcript changed while the name was being typed."
               && r.h.voicesDisk[k4].find("Marie") == std::string::npos && r.h.voicesDisk[k3].find("Marie") == std::string::npos
               && r.h.rewriteTries == rw0, a(why));
            Rig s;
            s.h.disk = prep.h.disk; s.h.cache = prep.h.cache; s.h.voicesDisk = prep.h.voicesDisk;
            s.p.open({ T3 }, false);
            const uint64_t v0 = vrevOf(s.p);
            const bool posted0 = fn(s.h.lastOf("translatorTranscript"), L"vrev") == static_cast<long long>(v0);
            s.p.renameSpeaker(s.p.session().gen, v0, 1, "Anna");
            const bool keptByRename = vrevOf(s.p) == v0;
            s.p.transcribe(true);                              // Re-transcribe: the speakers numbered again at its end
            fxVoices(s, 0);
            const uint64_t v1 = vrevOf(s.p);
            std::wstring why2;
            const bool stale = s.p.renameSpeaker(s.p.session().gen, v0, 2, "Paul", &why2);
            const bool fresh = s.p.renameSpeaker(s.p.session().gen, v1, 2, "Paul");
            ok("[FX] F8: a Re-transcribe that ENDED while a name was typed (the same generation, a new vrev): the old numbering's rename "
               "REFUSED with the sentence; the new one taken; a rename never renumbers; translatorTranscript carries vrev",
               posted0 && keptByRename && v1 != v0 && !stale && why2.size() > 20 && fresh
               && fn(s.h.lastOf("translatorTranscript"), L"vrev") == static_cast<long long>(v1), n2s(v0) + " -> " + n2s(v1));
            ok("every post well-formed (FX F8)", r.h.badJson == 0 && s.h.badJson == 0, r.h.lastBadJson + s.h.lastBadJson);
        }

        // ---- FX2 (03/11/2026, the round after the user's installer test -- docs/PROGRESS_FX2.md; sentinels: tests/cpp/cmut_u3.py,
        //      "FX2 ..."): the PLAYING row says what the Transcript tab says of its Word file -- n7 another analysis' transcript in
        //      it, A4 gone -- by the same reading of its identity (never the fingerprint), and keeps it once the file stops playing
        //      (never a disk read for a row not playing)
        std::printf("[FX2] the rows say what the tab says (n7 other, A4 gone), kept once the file stops playing\n");
        const std::string fx2Other = "The Word file of this sound track holds another transcript of it (another language pair or model): it is kept.";
        {
            // n7 (the real-app check VFX, 03/11): another analysis of the same track did Save .docx... OVER the Word file this
            // analysis' record names -- the tab says `other`; the playing row said nothing ("next to it" in its tooltip)
            const std::wstring N = L"D:\\Talks\\talk.wav", named = L"D:\\Talks\\talk - transcript.docx", P = L"D:\\Talks\\pause.wav";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(N, 4600, 71);
            r.h.addFile(P, 4700, 72);                         // (a file with no transcript, played meanwhile)
            r.p.setPair("fr", "en", "auto");
            r.p.open({ N }, false);
            analyseTranscribe(r);                              // fr/en: its Word file next to it, its record names it
            const uint64_t iN = r.p.items()[0].id;
            const bool firstOk = r.h.docx.size() == 1 && pbtrf::lowerW(r.h.docx[0].path) == pbtrf::lowerW(named);
            r.p.setPair("fr", "zh", "auto");
            analyseTranscribe(r);                              // fr/zh: the track's Word file is fr/en's -- nothing written
            pbtrs::TranscriptOut sb;                           // Save .docx... in fr/zh OVER that file (chosen in its dialog)
            r.p.transcriptForSave(sb);
            r.h.disk[pbtrf::lowerW(named)] = { 6200, 62 };
            pbtrf::registryPut(r.h.reg, named, 6200, 62, N, pbtrf::transcriptRegKind(), r.p.session().set->key, 0);
            r.p.transcriptSaved(named, 6200, 62, sb.sig, sb.tc);
            r.p.setPair("fr", "en", "auto");
            r.p.transcribe(false);                             // back on fr/en (complete): its record names that file, written over
            const std::wstring tab = r.h.lastOf("translatorTranscript");
            const std::string row = rowField(r.h, iN, "tdoc");
            ok("[FX2] n7: another analysis' Save .docx... OVER this analysis' Word file -- the tab says `other` AND the playing row says "
               "the same sentence (it was silent: \"next to it\")",
               firstOk && fb(tab, L"other") && row == fx2Other, "[" + row + "] other " + (fb(tab, L"other") ? "true" : "false"));
            r.p.open({ P }, false);                            // another file plays: N's row is not the playing one any more
            const int id0 = r.h.identityCalls;
            r.p.setRepeat(pbplayer::Repeat::All);              // (the whole list posted again)
            const std::string kept = rowField(r.h, iN, "tdoc", true);
            ok("[FX2] n7: ...its row KEEPS saying it once the file stops playing -- and no disk is asked for a row not playing",
               kept == fx2Other && r.h.identityCalls == id0, "[" + kept + "] " + n2s(r.h.identityCalls - id0) + " identity read(s)");
            r.p.open({ N }, false);                            // N again: still another analysis' transcript in its Word file
            const std::string again = rowField(r.h, iN, "tdoc");
            r.p.windowAttached();                              // its page reloads: nothing is posted -- nor read -- until `ready`
            pbtrs::TranscriptOut sa;                           // ...meanwhile Save .docx... in fr/en OVER that same file (the user
            r.p.transcriptForSave(sa);                         //  chose it in the dialog): fr/en's own transcript is in it now
            r.h.disk[pbtrf::lowerW(named)] = { 6300, 63 };
            pbtrf::registryPut(r.h.reg, named, 6300, 63, N, pbtrf::transcriptRegKind(), r.p.session().set->key, 0);
            r.p.transcriptSaved(named, 6300, 63, sa.sig, sa.tc);
            r.p.open({ P }, false);                            // ...and another file plays before the page is back
            r.p.windowReady();
            const std::string after = rowField(r.h, iN, "tdoc", true);
            ok("[FX2] n7: ...and forgotten once a Save for that analysis lands (over that very file, the page reloading meanwhile): "
               "the row says nothing more (its own transcript next to it)",
               again == fx2Other && after.empty() && rowField(r.h, iN, "tstate", true) == "done", "[" + again + "] -> [" + after + "]");
            ok("every post well-formed (FX2 n7)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // A4: the user DELETED (or moved) the Word file -- the tab drops its name; the playing row said nothing ("next to it"
            // in its tooltip): now "No Word file" (F6's word); kept once it stops playing; forgotten by a write that lands for it
            // (Transcribe all writes it again) and when it plays again with its Word file back (the Recycle Bin: same identity)
            const std::wstring G = L"D:\\Gone\\gone.wav", gdoc = L"D:\\Gone\\gone - transcript.docx", Q = L"D:\\Gone\\quiet.wav";
            Rig r;
            r.h.addFile(G, 5100, 81);
            r.h.addFile(Q, 5200, 82);
            r.h.cache[fxKey(G, 5100, 81)] = fxRecord(G, 5100, 81);
            r.h.cache[fxKey(Q, 5200, 82)] = fxRecord(Q, 5200, 82);
            r.p.open({ G }, false);
            r.p.transcribe(false);
            fxVoices(r, 0);
            const uint64_t iG = r.p.items()[0].id;
            const bool written = r.h.docx.size() == 1 && pbtrf::lowerW(r.h.docx[0].path) == pbtrf::lowerW(gdoc)
                && rowField(r.h, iG, "tdoc").empty();
            const auto gid = r.h.disk[pbtrf::lowerW(gdoc)];
            r.h.disk.erase(pbtrf::lowerW(gdoc));               // the user deletes it
            r.p.windowReady();                                 // (the page asks again: the tab and the list posted again)
            const std::wstring tab = r.h.lastOf("translatorTranscript");
            const std::string row = rowField(r.h, iG, "tdoc", true);
            ok("[FX2] A4: the Word file DELETED -- the tab names no Word file and the playing row says \"No Word file\" (it said nothing: "
               "\"next to it\" in its tooltip)",
               written && fs(tab, L"file").empty() && !fb(tab, L"older") && !fb(tab, L"edited") && !fb(tab, L"other") && row == "No Word file",
               "[" + row + "] file [" + fs(tab, L"file") + "]");
            r.p.open({ Q }, false);                            // another file plays (its transcript not made yet: nothing read)
            const int id0 = r.h.identityCalls;
            r.p.setRepeat(pbplayer::Repeat::All);
            const std::string kept = rowField(r.h, iG, "tdoc", true);
            ok("[FX2] A4: ...its row KEEPS saying it once the file stops playing -- no disk asked for a row not playing",
               kept == "No Word file" && r.h.identityCalls == id0, "[" + kept + "] " + n2s(r.h.identityCalls - id0) + " identity read(s)");
            r.p.transcribe(false);                             // Q transcribed (playing: Transcribe all leaves it, current)
            fxVoices(r, 1);
            const size_t d0 = r.h.docx.size();
            r.p.transcribeAll();                               // G is the list's: its Word file is missing -> written again
            const std::string rewritten = rowField(r.h, iG, "tdoc");
            ok("[FX2] A4: ...forgotten once a write for it lands (Transcribe all writes it again, the same name): the row says nothing "
               "more (its Word file next to it)",
               d0 == 2 && r.h.docx.size() == 3 && pbtrf::lowerW(r.h.docx.back().path) == pbtrf::lowerW(gdoc) && rewritten.empty(),
               "[" + rewritten + "] " + n2s(r.h.docx.size()) + " written");
            const auto gid2 = r.h.disk[pbtrf::lowerW(gdoc)];
            r.h.disk.erase(pbtrf::lowerW(gdoc));               // deleted again...
            r.p.open({ G }, false);                            // ...seen while G plays
            const std::string seen = rowField(r.h, iG, "tdoc");
            r.h.disk[pbtrf::lowerW(gdoc)] = gid2;              // ...restored from the Recycle Bin (the same size and time)
            r.p.windowReady();                                 // (G still playing: read again -- back to normal)
            const std::string back = rowField(r.h, iG, "tdoc", true);
            r.p.open({ Q }, false);
            const int id1 = r.h.identityCalls;
            r.p.setRepeat(pbplayer::Repeat::Off);
            const std::string after = rowField(r.h, iG, "tdoc", true);
            ok("[FX2] A4: ...restored and read again while it plays: back to normal -- and forgotten: once another file plays its row "
               "says nothing (not \"No Word file\")",
               seen == "No Word file" && back.empty() && after.empty() && rowField(r.h, iG, "tstate", true) == "done" && gid.first > 0,
               "[" + seen + "] [" + back + "] [" + after + "]");
            ok("[FX2] (b): the whole list posted again -- ONE disk read: the PLAYING file's Word file (as its tab reads it); none for the "
               "transcribed row that is not playing (its record's peek only)", r.h.identityCalls - id1 == 1, n2s(r.h.identityCalls - id1));
            ok("every post well-formed (FX2 A4)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // A4: gone wins over this run's reason ("open in another program", G2's refusal) and over the folder ("Word file in
            // Documents\SuVtitle Transcripts")
            const std::wstring H2 = L"D:\\Gone\\held.wav", hdoc = L"D:\\Gone\\held - transcript.docx", W = L"D:\\Card2\\away.wav";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(H2, 5300, 83);
            r.h.cache[fxKey(H2, 5300, 83)] = fxRecord(H2, 5300, 83);
            r.p.open({ H2 }, false);
            r.p.transcribe(false);
            fxVoices(r, 0);
            r.h.heldDocx.insert(pbtrf::lowerW(hdoc));          // open in Word...
            r.p.renameSpeaker(r.p.session().gen, vrevOf(r.p), 1, "Anna");
            r.h.now += pbplayer::kDocxRenameMs;
            r.p.tick();                                        // ...G2 refused: its row says why
            const std::string heldRow = rowField(r.h, r.p.items()[0].id, "tdoc");
            r.h.heldDocx.clear();                              // Word closed, and the file deleted
            r.h.disk.erase(pbtrf::lowerW(hdoc));
            r.p.windowReady();
            const std::string goneRow = rowField(r.h, r.p.items()[0].id, "tdoc", true);
            ok("[FX2] A4: gone wins over this run's reason -- \"open in another program\" while it was held, \"No Word file\" once deleted",
               heldRow.find("open in another program") != std::string::npos && goneRow == "No Word file", "[" + heldRow + "] -> [" + goneRow + "]");
            Rig w;
            w.h.regMode = true;
            w.h.addFile(W, 5400, 84);
            w.h.cache[fxKey(W, 5400, 84)] = fxRecord(W, 5400, 84);
            w.h.awayMedia.insert(pbtrf::lowerW(W));            // its folder refuses: the Word file goes to Documents
            w.p.open({ W }, false);
            w.p.transcribe(false);
            fxVoices(w, 0);
            const std::string awayRow = rowField(w.h, w.p.items()[0].id, "tdoc");
            if (!w.h.docx.empty()) w.h.disk.erase(pbtrf::lowerW(w.h.docx.back().path));   // deleted there
            w.p.windowReady();
            const std::string awayGone = rowField(w.h, w.p.items()[0].id, "tdoc", true);
            ok("[FX2] A4: gone wins over the folder -- \"Word file in Documents\\SuVtitle Transcripts\" while it was there, \"No Word "
               "file\" once deleted", awayRow.find("Word file in Documents") == 0 && awayGone == "No Word file",
               "[" + awayRow + "] -> [" + awayGone + "]");
            ok("every post well-formed (FX2 A4 wins)", r.h.badJson == 0 && w.h.badJson == 0, r.h.lastBadJson + w.h.lastBadJson);
        }
        {
            // the red toast: Transcribe all's ONE notice is INFORMATION (`info: true`, as the bridge's own news says it -- the page
            // shows it as a failure without it, and a notice without it also drops a subtitle track the viewer is waiting for);
            // every other notice of the core stays an error (no `info`); a notice held while the page reloads keeps its flag
            const std::wstring Y = L"D:\\Shelf\\why.wav", Z = L"D:\\Shelf\\zed.wav";
            auto lastNotice = [](const FakeHost& h, const std::string& part) {
                for (size_t i = h.window.size(); i-- > 0;)
                    if (typeOf(h.window[i]) == "translatorFileNotice" && a(h.window[i]).find(part) != std::string::npos) return h.window[i];
                return std::wstring();
            };
            auto infoOf = [](const std::wstring& j) { return j.empty() ? std::string("<no notice>") : a(j).find("\"info\":") == std::string::npos ? std::string("absent") : fb(j, L"info") ? std::string("true") : std::string("not true"); };
            Rig r;
            r.h.docxFails = true;                              // (its folder takes no new file: no Word file next to it)
            r.h.addFile(Y, 6100, 91);
            r.h.cache[fxKey(Y, 6100, 91)] = fxRecord(Y, 6100, 91);
            r.p.add({ Y });
            r.p.transcribeAll();
            fxVoices(r, 0);                                    // its voice pass done -> its Word file refused -> the batch's notice
            const std::wstring tn = lastNotice(r.h, "Transcribe all: 1 file has no up-to-date Word file next to it");
            r.p.notice(L"Not found: gone.mkv");                // an error of the core's own (the queue's "Not found: ...")
            const std::wstring en = lastNotice(r.h, "Not found: gone.mkv");
            ok("[FX2] Transcribe all's ONE notice says `info: true` (news, not a failure); an error notice carries no `info` (as the "
               "bridge's own errors)", infoOf(tn) == "true" && infoOf(en) == "absent", "batch " + infoOf(tn) + ", error " + infoOf(en));
            Rig b;                                            // ...the same while the page reloads: held, then said on its `ready`
            b.h.docxFails = true;
            b.h.addFile(Z, 6200, 92);
            b.h.cache[fxKey(Z, 6200, 92)] = fxRecord(Z, 6200, 92);
            b.p.add({ Z });
            b.p.windowAttached();                              // (the page reloads: nothing posted until `ready`)
            b.p.transcribeAll();
            fxVoices(b, 0);
            b.p.notice(L"Not found: gone.mkv");
            const bool heldBack = lastNotice(b.h, "Transcribe all:").empty() && lastNotice(b.h, "Not found:").empty();
            b.p.windowReady();
            const std::wstring tb = lastNotice(b.h, "Transcribe all: 1 file has no up-to-date Word file next to it");
            const std::wstring eb = lastNotice(b.h, "Not found: gone.mkv");
            ok("[FX2] ...a notice HELD while the page reloads keeps its flag: Transcribe all's `info: true`, the error none",
               heldBack && infoOf(tb) == "true" && infoOf(eb) == "absent", "batch " + infoOf(tb) + ", error " + infoOf(eb)
               + (heldBack ? "" : " (posted during the reload)"));
            ok("every post well-formed (FX2 notice)", r.h.badJson == 0 && b.h.badJson == 0, r.h.lastBadJson + b.h.lastBadJson);
        }
        {
            // A5 (bridge.cpp writeTranscriptDocx; the FakeHost's regMode follows its order): this analysis OWNS its Word file next
            // to the media; the media's folder refuses its .tmp now (a card write-locked since the first write: WriteFail::Folder)
            // AND another analysis of the track Saved a NEW Word file meanwhile -- Re-transcribe goes on to the names, then to
            // Documents, and says so. (It said "holds another transcript": the F2 guard, for an analysis with no Word file of its
            // own, met that other analysis' newer file.)
            const std::wstring takeA5 = L"D:\\Card3\\take.wav", cdoc = L"D:\\Card3\\take - transcript.docx";
            const std::wstring zdoc = L"D:\\Card3\\take - transcript (SuVtitle).docx";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(takeA5, 6500, 95);
            r.p.setPair("fr", "en", "auto");
            r.p.open({ takeA5 }, false);
            analyseTranscribe(r);                              // fr/en: its Word file next to it (ours)
            const bool own = r.h.docx.size() == 1 && pbtrf::lowerW(r.h.docx[0].path) == pbtrf::lowerW(cdoc);
            r.p.setPair("fr", "zh", "auto");
            analyseTranscribe(r);                              // fr/zh: `other` -- nothing written
            pbtrs::TranscriptOut sz;                           // Save .docx... in fr/zh to a NEW file beside it
            r.p.transcriptForSave(sz);
            r.h.disk[pbtrf::lowerW(zdoc)] = { 6600, 66 };
            pbtrf::registryPut(r.h.reg, zdoc, 6600, 66, takeA5, pbtrf::transcriptRegKind(), r.p.session().set->key, 0);
            r.p.transcriptSaved(zdoc, 6600, 66, sz.sig, sz.tc);
            r.p.setPair("fr", "en", "auto");
            r.p.transcribe(false);                             // back on fr/en: its own Word file, current
            const bool current = !fb(r.h.lastOf("translatorTranscript"), L"older") && !fb(r.h.lastOf("translatorTranscript"), L"other");
            const auto cid = r.h.disk[pbtrf::lowerW(cdoc)];
            r.h.awayMedia.insert(pbtrf::lowerW(takeA5));       // the card write-locked now: its folder refuses every .tmp
            r.p.transcribe(true);                              // Re-transcribe: the voices numbered again -> its Word file older
            fxVoices(r, r.h.voicesSent.size() - 1, true);
            const std::wstring tab = r.h.lastOf("translatorTranscript");
            const std::string row = rowField(r.h, r.p.items()[0].id, "tdoc");
            ok("[FX2] A5: an analysis that OWNS its Word file, its folder refusing the .tmp, another analysis' newer Save of the track "
               "beside it: Re-transcribe writes to Documents\\SuVtitle Transcripts and says so (tab and row) -- never \"holds another "
               "transcript\"; its own file and the other one untouched",
               own && current && r.h.docx.size() == 2 && pbtrf::lowerW(r.h.docx.back().path).find(L"\\documents\\suvtitle transcripts\\") != std::wstring::npos
               && fs(tab, L"note").find("The media's folder could not be written to") == 0 && fs(tab, L"docxErr").empty()
               && !fb(tab, L"other") && row.find("Word file in Documents") == 0
               && r.h.disk[pbtrf::lowerW(zdoc)].first == 6600 && r.h.disk[pbtrf::lowerW(cdoc)] == cid,
               "note [" + fs(tab, L"note") + "] docxErr [" + fs(tab, L"docxErr") + "] row [" + row + "] " + n2s(r.h.docx.size()) + " written");
            ok("every post well-formed (FX2 A5)", r.h.badJson == 0, r.h.lastBadJson);
        }

        // ---- FX2-A (04/11/2026, the director's audit -- docs/PROGRESS_FX2.md; sentinels: tests/cpp/cmut_u3.py "FX2-A ..."): the
        //      PLAYING file's Word file WATCHED once a second (tick) -- deleted, re-created with other contents, restored: the row
        //      and the tab follow at the next tick, with NO other post (no windowReady() here); one stat a tick, the registry only
        //      on a change, nothing for a row not playing nor with no file loaded
        std::printf("[FX2-A] the playing file's Word file watched once a second (tick)\n");
        {
            const std::wstring WG = L"D:\\Watch\\film.wav", wdoc = L"D:\\Watch\\film - transcript.docx";
            const std::wstring WQ = L"D:\\Watch\\other.wav", qdoc = L"D:\\Watch\\other - transcript.docx";
            Rig r;
            r.h.regMode = true;
            r.h.addFile(WG, 7100, 101);
            r.h.addFile(WQ, 7200, 102);
            r.h.cache[fxKey(WG, 7100, 101)] = fxRecord(WG, 7100, 101);
            r.h.cache[fxKey(WQ, 7200, 102)] = fxRecord(WQ, 7200, 102);
            r.p.open({ WQ }, false);                           // the other file transcribed first: then a row not playing, its
            r.p.transcribe(false);                             //  Word file next to it
            fxVoices(r, 0);
            r.p.open({ WG }, false);                           // the film plays, transcribed: its Word file next to it
            r.p.transcribe(false);
            fxVoices(r, 1);
            auto idOf = [&](const std::wstring& p) {
                for (const auto& it : r.p.items()) if (pbtrf::lowerW(it.path) == pbtrf::lowerW(p)) return it.id;
                return uint64_t(0);
            };
            const uint64_t iG = idOf(WG);
            auto second = [&]() { r.h.now += 1000; r.p.tick(); };   // the bridge's 1 Hz tick (Bridge::uiTick)
            const bool setup = r.h.docx.size() == 2 && r.p.session().path == WG && r.h.disk.count(pbtrf::lowerW(wdoc)) > 0
                && r.h.disk.count(pbtrf::lowerW(qdoc)) > 0 && iG != 0;
            r.h.clearPosts();
            second();                                          // the watch's first reading of this Word file: kept, nothing posted
            const bool firstQuiet = r.h.countOf("translatorTranscript") == 0 && r.h.countOf("playerItem") == 0 && r.h.countOf("playerQueue") == 0;
            const auto wid = r.h.disk[pbtrf::lowerW(wdoc)];
            r.h.clearPosts();
            r.h.disk.erase(pbtrf::lowerW(wdoc));               // 1. DELETED in Explorer, the film open and idle
            second();
            const std::wstring t1 = r.h.lastOf("translatorTranscript");
            const std::string row1 = rowField(r.h, iG, "tdoc");
            ok("[FX2-A] 1: the playing file's Word file DELETED while the film is idle -- ONE tick later the row says \"No Word file\" "
               "and the tab names no Word file (no other post: the tick's own)",
               setup && firstQuiet && row1 == "No Word file" && !t1.empty() && fs(t1, L"file").empty(),
               std::string(setup ? "" : "setup ") + (firstQuiet ? "" : "first-tick-posted ") + "[" + row1 + "] file [" + fs(t1, L"file") + "]");
            r.h.clearPosts();
            r.h.disk[pbtrf::lowerW(wdoc)] = { wid.first + 77, wid.second + 5 };   // 2. RE-CREATED under that name, OTHER contents
            second();
            const std::wstring t2 = r.h.lastOf("translatorTranscript");
            const std::string row2 = rowField(r.h, iG, "tdoc");
            r.h.clearPosts();
            r.h.disk[pbtrf::lowerW(wdoc)] = { wid.first + 77, wid.second + 9 };   // ...saved again in Word: the same size, a new time
            second();
            const size_t timeOnly = r.h.countOf("translatorTranscript");
            r.h.clearPosts();
            r.h.disk[pbtrf::lowerW(wdoc)] = { wid.first + 80, wid.second + 9 };   // ...a copy put back: another size, the same time
            second();
            const size_t sizeOnly = r.h.countOf("translatorTranscript");
            r.p.open({ WQ }, false);                           // ...then another file plays: its row shows it was forgotten
            const std::string row2b = rowField(r.h, iG, "tdoc", true);
            ok("[FX2-A] 2: ...RE-CREATED under that name with OTHER contents (another size and time) -- one tick: the tab says `edited` "
               "(its name back), the row is back to \"\" (a Word file next to it), and the memo is forgotten (once another file plays: "
               "still \"\", not \"No Word file\"); a new time alone, or a new size alone, is seen too (one post each)",
               !t2.empty() && fb(t2, L"edited") && fs(t2, L"file") == "film - transcript.docx" && row2.empty() && row2b.empty()
               && timeOnly == 1 && sizeOnly == 1,
               "edited " + std::string(fb(t2, L"edited") ? "true" : "false") + " file [" + fs(t2, L"file") + "] row [" + row2 + "] then ["
               + row2b + "]; time only " + n2s(timeOnly) + ", size only " + n2s(sizeOnly));
            r.p.open({ WG }, false);                           // the film again
            r.h.clearPosts();
            second();                                          // (a new watch: its first reading kept, nothing posted)
            const bool againQuiet = r.h.countOf("translatorTranscript") == 0 && r.h.countOf("playerItem") == 0;
            r.h.disk[pbtrf::lowerW(wdoc)] = wid;               // 3. RESTORED with its exact old size and time (the Recycle Bin)
            second();
            const std::wstring t3 = r.h.lastOf("translatorTranscript");
            const std::string row3 = rowField(r.h, iG, "tdoc");
            ok("[FX2-A] 3: ...RESTORED with its exact old identity -- one tick: back to normal (its name, not edited, not older; the "
               "row \"\")",
               againQuiet && !t3.empty() && !fb(t3, L"edited") && !fb(t3, L"older") && fs(t3, L"file") == "film - transcript.docx"
               && row3.empty(), std::string(againQuiet ? "" : "first-tick-posted ") + "file [" + fs(t3, L"file") + "] row [" + row3 + "]");
            const size_t a0 = r.h.identityAsked.size();         // 4. THE COST, idle and unchanged
            const int w0 = r.h.docxWriterCalls;
            r.h.clearPosts();
            second(); second(); second();
            bool onlyG = r.h.identityAsked.size() == a0 + 3;
            for (size_t i = a0; i < r.h.identityAsked.size(); ++i) if (r.h.identityAsked[i] != pbtrf::lowerW(wdoc)) onlyG = false;
            const bool quiet = r.h.docxWriterCalls == w0 && r.h.window.empty();
            ok("[FX2-A] 4: idle and unchanged, three ticks = THREE identity reads, each of the PLAYING file's Word file -- none of the "
               "row not playing (its Word file next to it), NO registry read, nothing posted",
               onlyG && quiet, n2s(r.h.identityAsked.size() - a0) + " read(s), " + n2s(r.h.docxWriterCalls - w0) + " registry, "
               + n2s(r.h.window.size()) + " post(s)");
            r.p.close();                                       // ...and with no file loaded
            const size_t a1 = r.h.identityAsked.size();
            second(); second();
            ok("[FX2-A] 4: ...with no file loaded, ticks read nothing", r.h.identityAsked.size() == a1, n2s(r.h.identityAsked.size() - a1));
            r.p.open({ WG }, false);                           // 5. the film again; its Word file deleted, seen by the watch...
            second();
            r.h.disk.erase(pbtrf::lowerW(wdoc));
            second();
            const std::string gone5 = rowField(r.h, iG, "tdoc");
            r.p.open({ WQ }, false);                           // ...then ANOTHER file plays
            const std::string kept5 = rowField(r.h, iG, "tdoc", true);
            const size_t a2 = r.h.identityAsked.size();
            second(); second();
            bool noG = r.h.identityAsked.size() == a2 + 2;
            for (size_t i = a2; i < r.h.identityAsked.size(); ++i) if (r.h.identityAsked[i] != pbtrf::lowerW(qdoc)) noG = false;
            ok("[FX2-A] 5: another file plays -- the first file's Word file is read no more (each tick reads the PLAYING one's only), "
               "and the first file's row keeps \"No Word file\"",
               gone5 == "No Word file" && kept5 == "No Word file" && noG,
               "[" + gone5 + "] [" + kept5 + "] " + n2s(r.h.identityAsked.size() - a2) + " read(s)");
            ok("every post well-formed (FX2-A)", r.h.badJson == 0, r.h.lastBadJson);
        }
        {
            // 6. the pair changed WHILE the film plays: its shown transcript keeps the old pair's key, which names no row now -- its
            //    Word file deleted, one tick: the PLAYING row says it all the same (the watch marks the playing item, not a key)
            const std::wstring WP = L"D:\\Watch\\pair.wav", pdoc = L"D:\\Watch\\pair - transcript.docx";
            Rig r;
            r.h.addFile(WP, 7300, 103);
            r.h.cache[fxKey(WP, 7300, 103)] = fxRecord(WP, 7300, 103);
            r.p.open({ WP }, false);
            r.p.transcribe(false);
            fxVoices(r, 0);
            r.p.setPair("en", "de", "auto");                   // another pair: the transcript shown is still en/fr's
            const bool oldKey = r.p.session().set && r.p.session().set->key == fxKey(WP, 7300, 103) && r.h.docx.size() == 1;
            r.h.now += 1000; r.p.tick();                       // (the watch's first reading)
            r.h.clearPosts();
            r.h.disk.erase(pbtrf::lowerW(pdoc));
            r.h.now += 1000; r.p.tick();
            const std::string row6 = r.p.items().empty() ? std::string("<no item>") : rowField(r.h, r.p.items()[0].id, "tdoc");
            ok("[FX2-A] 6: the pair changed while the film plays (its shown transcript keeps the old key: no row has it) -- its Word "
               "file deleted, one tick: the playing row says \"No Word file\" all the same, the tab names no Word file",
               oldKey && row6 == "No Word file" && fs(r.h.lastOf("translatorTranscript"), L"file").empty(),
               std::string(oldKey ? "" : "not the old key ") + "[" + row6 + "] file [" + fs(r.h.lastOf("translatorTranscript"), L"file") + "]");
        }

        // ---- FX2-B (04/11/2026, the director's audit + the integrator's addition -- docs/PROGRESS_FX2.md; sentinels: cmut_u3.py
        //      "FX2-B ..."): THE WATCH'S GUARDS (its stat runs on the UI thread) -- never while the window is hidden (a change made
        //      meanwhile is posted at the first tick it shows); ONE slow read (>= 250 ms) stops it for that file until the next
        //      load; never for a file on the NETWORK (a UNC path, a mapped drive letter -- the media's, or the Word file's own:
        //      asked of the host where the path is SET, never in a tick); a local file still watched. The fake clock + tick() only.
        std::printf("[FX2-B] the watch's guards: the window hidden, a slow read, a file on the network\n");
        {
            const bool forms = pbplayer::uncPath(L"\\\\nas\\films\\a.mkv") && pbplayer::uncPath(L"//nas/films/a.mkv")
                && pbplayer::uncPath(L"\\\\?\\UNC\\nas\\films\\a.mkv") && pbplayer::uncPath(L"\\\\?\\unc\\nas\\films\\a.mkv")
                && !pbplayer::uncPath(L"\\\\?\\C:\\Films\\a.mkv") && !pbplayer::uncPath(L"\\\\.\\C:\\Films\\a.mkv")
                && !pbplayer::uncPath(L"C:\\Films\\a.mkv") && !pbplayer::uncPath(L"\\Films\\a.mkv")
                && pbplayer::driveOf(L"Z:\\Films\\a.mkv") == L'Z' && pbplayer::driveOf(L"\\\\?\\z:\\a.mkv") == L'z'
                && pbplayer::driveOf(L"\\\\nas\\films\\a.mkv") == 0 && pbplayer::driveOf(L"a.mkv") == 0;
            FakeHost fh;                                       // (the host's DEFAULT, as the bridge's inherits it: this PC's C: local)
            const bool dflt = fh.hostDefaultNet(L"\\\\nas\\films\\a.mkv") && fh.hostDefaultNet(L"\\\\?\\UNC\\nas\\f\\a.mkv")
                && !fh.hostDefaultNet(L"C:\\Windows\\win.ini") && !fh.hostDefaultNet(L"\\\\?\\C:\\Windows\\win.ini")
                && !fh.hostDefaultNet(L"\\\\.\\pipe\\x");
            ok("[FX2-B] on the network = a UNC path (\\\\server\\share, //server/share, \\\\?\\UNC\\ in any case) -- never \\\\?\\C:\\ nor "
               "\\\\.\\ -- or a remote drive letter (the host's default asks GetDriveTypeW: this PC's C: is local)",
               forms && dflt, std::string(forms ? "" : "forms ") + (dflt ? "" : "default"));
        }
        // a transcribed file played, its Word file written by the core; then the reads of `doc` (and the host's network questions)
        // made by `n` ticks of the bridge's 1 Hz clock
        auto watchedFx2b = [&](Rig& r, const std::wstring& media, uint64_t size, uint64_t mtime) {
            r.h.addFile(media, size, mtime);
            r.h.cache[fxKey(media, size, mtime)] = fxRecord(media, size, mtime);
            r.p.open({ media }, false);
            r.p.transcribe(false);
            if (!r.h.voicesSent.empty()) fxVoices(r, r.h.voicesSent.size() - 1);
            return r.h.docx.empty() ? std::wstring() : r.h.docx.back().path;
        };
        struct Fx2bReads { size_t doc = 0; int net = 0; };
        auto ticksFx2b = [&](Rig& r, int n, const std::wstring& doc) {
            const size_t a = r.h.identityAsked.size();
            const int q = r.h.netAsks;
            for (int i = 0; i < n; ++i) { r.h.now += 1000; r.p.tick(); }
            Fx2bReads x;
            for (size_t i = a; i < r.h.identityAsked.size(); ++i) if (r.h.identityAsked[i] == pbtrf::lowerW(doc)) ++x.doc;
            x.net = r.h.netAsks - q;
            return x;
        };
        auto said = [](const Fx2bReads& x) { return n2s(x.doc) + " read(s), " + n2s(static_cast<unsigned long long>(x.net)) + " net question(s)"; };
        {
            Rig r;                                            // CONTROL: a local file, the window shown -- watched
            const std::wstring doc = watchedFx2b(r, L"D:\\Local\\local.wav", 8100, 111);
            const Fx2bReads x = ticksFx2b(r, 3, doc);
            ok("[FX2-B] control: a LOCAL file, the window shown -- still watched: one read of its Word file a tick; no network question "
               "in a tick", !doc.empty() && x.doc == 3 && x.net == 0, said(x));
        }
        {
            Rig r;                                            // a SLOW read: one, then none -- until the file is LOADED again
            const std::wstring S1 = L"D:\\Slow\\slow.wav", S2 = L"D:\\Slow\\next.wav";
            const std::wstring doc = watchedFx2b(r, S1, 8200, 112);
            r.h.addFile(S2, 8300, 113);
            r.h.slowStat[pbtrf::lowerW(doc)] = 250;          // each read of it takes 250 ms (a share answering slowly)
            const Fx2bReads first = ticksFx2b(r, 1, doc), later = ticksFx2b(r, 3, doc);
            r.p.open({ S2 }, false);                          // another file...
            r.p.open({ S1 }, false);                          // ...then this one LOADED again
            const Fx2bReads again = ticksFx2b(r, 1, doc), againLater = ticksFx2b(r, 2, doc);
            ok("[FX2-B] a read that takes 250 ms or more STOPS the watch of that file: one read, then none -- until it is loaded again "
               "(one read, stopped again)", !doc.empty() && first.doc == 1 && later.doc == 0 && again.doc == 1 && againLater.doc == 0,
               said(first) + " / " + said(later) + " / " + said(again) + " / " + said(againLater));
        }
        {
            Rig r;                                            // the window HIDDEN: no read; shown again: read, the change posted
            const std::wstring doc = watchedFx2b(r, L"D:\\Hidden\\hidden.wav", 8400, 114);
            const uint64_t iH = r.p.items().empty() ? 0 : r.p.items()[0].id;
            ticksFx2b(r, 1, doc);                             // (its first reading)
            r.p.windowShown(false);
            const Fx2bReads hidden1 = ticksFx2b(r, 2, doc);
            r.h.disk.erase(pbtrf::lowerW(doc));               // its Word file deleted while the window is hidden
            r.h.clearPosts();
            const Fx2bReads hidden2 = ticksFx2b(r, 2, doc);
            const bool quiet = r.h.window.empty();
            r.p.windowShown(true);
            const Fx2bReads shown = ticksFx2b(r, 1, doc);
            const std::string row = rowField(r.h, iH, "tdoc");
            ok("[FX2-B] the window HIDDEN: no read at all (its Word file deleted meanwhile: nothing read, nothing posted); shown again: "
               "the first tick reads it and posts the change (the row \"No Word file\"; the posts read it too)",
               !doc.empty() && hidden1.doc == 0 && hidden2.doc == 0 && quiet && shown.doc >= 1 && row == "No Word file",
               said(hidden1) + " / " + said(hidden2) + (quiet ? "" : " (posted while hidden)") + " / " + said(shown) + " [" + row + "]");
        }
        {
            Rig r;                                            // a UNC media: its Word file (beside it) never read in a tick
            const std::wstring doc = watchedFx2b(r, L"\\\\nas\\films\\unc.wav", 8500, 115);
            const Fx2bReads x = ticksFx2b(r, 3, doc);
            ok("[FX2-B] a media on a share (\\\\nas\\films\\...): its Word file never read in a tick -- and no network question in a tick "
               "(asked at the load)", pbtrf::lowerW(doc).find(L"\\\\nas\\films\\") == 0 && x.doc == 0 && x.net == 0, said(x));
        }
        {
            Rig r;                                            // a media on a SHARE whose Word file went to the LOCAL Documents: not
            r.h.regMode = true;                               //  watched either -- the media's own answer, asked at the load
            const std::wstring U2 = L"\\\\nas\\films\\full.wav";
            r.h.awayMedia.insert(pbtrf::lowerW(U2));          // (the share's folder refuses the .tmp: Documents, on this PC)
            const std::wstring doc = watchedFx2b(r, U2, 8900, 119);
            const Fx2bReads x = ticksFx2b(r, 3, doc);
            ok("[FX2-B] a media on a SHARE whose Word file went to the LOCAL Documents: not watched either (the media's own answer, "
               "asked at the load)", pbtrf::lowerW(doc).find(L"c:\\users\\me\\documents\\") == 0 && x.doc == 0 && x.net == 0, said(x));
        }
        {
            Rig r;                                            // a media on a MAPPED drive letter (remote)
            r.h.remoteDrives.insert(L'z');
            const std::wstring doc = watchedFx2b(r, L"Z:\\Films\\mapped.wav", 8600, 116);
            const Fx2bReads x = ticksFx2b(r, 3, doc);
            ok("[FX2-B] a media on a MAPPED drive letter (Z:, remote): never read in a tick -- no network question in a tick",
               !doc.empty() && x.doc == 0 && x.net == 0, said(x));
        }
        {
            Rig r;                                            // a LOCAL media, its Word file in a REDIRECTED (network) Documents
            r.h.regMode = true;
            r.h.docsDir = L"\\\\nas\\home\\me\\Documents\\SuVtitle Transcripts";
            const std::wstring L5 = L"D:\\Card5\\local5.wav";
            r.h.awayMedia.insert(pbtrf::lowerW(L5));          // (its own folder refuses: the Word file goes to Documents)
            const std::wstring doc = watchedFx2b(r, L5, 8700, 117);
            const Fx2bReads x = ticksFx2b(r, 3, doc);
            Rig n;                                            // ...and a NEW run reading that record: not watched either
            n.h.regMode = true;
            n.h.disk = r.h.disk; n.h.cache = r.h.cache; n.h.voicesDisk = r.h.voicesDisk; n.h.reg = r.h.reg;
            n.p.open({ L5 }, false);
            const Fx2bReads y = ticksFx2b(n, 3, doc);
            ok("[FX2-B] a LOCAL media whose Word file went to a REDIRECTED (network) Documents: that Word file never read in a tick -- "
               "decided when its path is set (written; and read from its record by a new run)",
               pbtrf::lowerW(doc).find(L"\\\\nas\\home\\") == 0 && x.doc == 0 && x.net == 0 && y.doc == 0 && y.net == 0
               && n.p.session().set && n.p.session().set->hasVoices, said(x) + " / new run " + said(y));
        }
        {
            Rig r;                                            // Save .docx... to a SHARE: that Word file never read in a tick
            const std::wstring G7 = L"D:\\Saves\\save.wav";
            const std::wstring doc0 = watchedFx2b(r, G7, 8800, 118);
            const Fx2bReads before = ticksFx2b(r, 2, doc0);
            pbtrs::TranscriptOut so;
            r.p.transcriptForSave(so);
            const std::wstring net = L"\\\\nas\\deliver\\save - transcript.docx";
            r.h.disk[pbtrf::lowerW(net)] = { 9100, 91 };
            r.p.transcriptSaved(net, 9100, 91, so.sig, so.tc);
            const Fx2bReads after = ticksFx2b(r, 3, net);
            ok("[FX2-B] Save .docx... to a SHARE: the Word file saved there never read in a tick (decided when the Save sets it); read "
               "before, while it was the local one", before.doc == 2 && after.doc == 0 && after.net == 0, said(before) + " / " + said(after));
        }
    }

    std::printf("\nplayer_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

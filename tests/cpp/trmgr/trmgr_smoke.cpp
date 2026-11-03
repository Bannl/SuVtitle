// =============================================================================
// SuVtitle tests/cpp/trmgr/trmgr_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/trmgr/trmgr_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include, start() without a fabric rate, the fake is built as SuVtitleEngine.exe,
// the synthesized sentences ("The SuVtitle engine ..."). Same checks, same count.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// trmgr_smoke -- the WC (file mode) check of the REAL ui/TranslatorManager.cpp against a
// scripted helper (fake_helper.cpp, built as SuVtitleEngine.exe next to this exe, so the
// manager spawns and supervises it exactly as it does the real one).
//
// What it proves, mechanically:
//   1. a CTL queued before a FILE reaches the helper BEFORE it (the pair first, then the job);
//   2. FPROG / FCUE / FDONE arrive on the ordered queue, in order, FDONE last;
//   3. FCANCEL stops a running job; a FILE never sent is cancelled locally, exactly once;
//   4. proxies: one FPROXYDONE each; a newer one supersedes; FCANCEL on a proxy id cancels it;
//   5. file messages are NEVER dropped by the live-line cap (600 LINE + 50 FCUE, drained late);
//   6. a helper dying mid-job owes -- and gets -- a SYNTHESIZED FDONE error and FPROXYDONE,
//      and the respawned helper NEVER sees that FILE again;
//   7. a helper that wrote its real FDONE and THEN died is reported as having finished.
// SUVTITLE LIVE (1.1.0, L2 2026-10-25), each on a FRESH manager (and a fresh helper):
//   L1. the live CTL carries scope=live and the live pair, the player's CTL none; the echo; LIVE 1 -> INPUT on the
//       ordered queue; 500 LEVELS -> NOT queued, the newest in the slot; LIVE 0; an unchanged wish sends nothing; a Clear
//       ALONE (no pair change) still goes out on the live scope;
//   L2. SRCLIST -> ONE Sources message per complete list; a SHORT list (SRCEND counts more) is asked again, not delivered; a list
//       that STAYS short is asked again twice at most, then delivered as it is;
//   L3. a respawn RE-SENDS the live pair, the wanted LIVE and the list's ask -- never the FILE;
//   L4. RV2-5 on the live path: three deaths right after LIVE 1 -> the wanted LIVE is DROPPED (said once, the next
//       engine is not asked to listen), each such death is a failed start (back-off, "keeps failing" at the third), and the
//       user's next setLive(true) is sent again AND forgives: the next death after it is the first of a new run;
//   L5. AudioPB 1.1.0's rv5_drain bench (RV5-1), ported: 300 LINE written by a helper that dies at once -- the NEWEST
//       (segId 300) is delivered, with a normal wake and with a slow one (a worker that lags the helper).
// FIX ROUND (2026-10-26, docs/REVIEW_RV-LB.md):
//   L6a. RVB-5 a REFUSED LIVE (a STATE with err, not live) drops the wish once, with the engine's sentence -- never re-sent;
//   L6b. RVB-7 stopSources: a respawned engine is not asked for the list;
//   L6c. RVB-7 three deaths right after a SRCLIST drop the ask (said); the user's next ask forgives;
//   L6d. RVB-13 an edit of the strips while listening forgives nothing (the third death still drops the wish);
//   L6e. RVB-6 every LINE carries the engine incarnation that said it (1, then 2 after a respawn).
//   (RVB-8's late deaths -- 30 s and more into a listen -- and RVB-13's device clock are pinned, not run here: minutes each.)
// U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 9), on a FRESH manager:
//   V1. FVOICE -> FPROG voices, ONE FSPK (its labels, speakers, model), FDONE ok -- in that order; the FVOICE as written;
//   V2. ONE job slot: an FVOICE cancels a running analysis and a FILE a running voice pass (one FDONE cancelled each, no
//       FSPK for a cancelled pass); back-to-back starts of either kind: one FDONE cancelled each, the last runs;
//   V3. cancelFile on an unsent and on a running FVOICE: exactly one FDONE cancelled;
//   V4. a death mid-pass: a SYNTHESIZED FDONE error "...stopped while recognising the voices.", never re-sent, the next pass runs;
//   V5. a manager that is not running answers at once; an FVOICE the engine would refuse is never queued.
// =============================================================================
#include "../../../app/TranslatorManager.h"

#include <atomic>
#include <cstdio>
#include <share.h>
#include <string>
#include <vector>

using Mgr  = pbr::TranslatorManager;
using Kind = Mgr::Msg::Kind;
namespace tp = pbt::trpipe;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
    std::fflush(stdout);
}

static std::vector<Mgr::Msg> g_all;
static std::atomic<int> g_wakes{ 0 };

static void pump(Mgr& m) { for (auto& x : m.pollMessages()) g_all.push_back(std::move(x)); }
template <class P> static bool waitFor(Mgr& m, P pred, int ms) {
    for (int t = 0; t < ms; t += 20) { pump(m); if (pred()) return true; ::Sleep(20); }
    pump(m);
    return pred();
}
static int countKind(Kind k, uint64_t id) {
    int n = 0;
    for (const auto& x : g_all) {
        if (x.kind != k) continue;
        const uint64_t xid = k == Kind::FileDone ? x.fdone.jobId : k == Kind::ProxyDone ? x.pdone.jobId
                           : k == Kind::FileCue ? x.cue.jobId : k == Kind::FileProg ? x.fprog.jobId
                           : k == Kind::FileSpk ? x.spk.jobId : 0;
        if (xid == id) ++n;
    }
    return n;
}
static const Mgr::Msg* lastDone(uint64_t id) {
    const Mgr::Msg* r = nullptr;
    for (const auto& x : g_all) if (x.kind == Kind::FileDone && x.fdone.jobId == id) r = &x;
    return r;
}
static const Mgr::Msg* lastProxyDone(uint64_t id) {
    const Mgr::Msg* r = nullptr;
    for (const auto& x : g_all) if (x.kind == Kind::ProxyDone && x.pdone.jobId == id) r = &x;
    return r;
}

static std::wstring g_dir;
static std::string readLog() {
    std::string s;
    FILE* f = _wfsopen((g_dir + L"fake_rx.log").c_str(), L"rb", _SH_DENYNO);   // shared: see fake_helper.cpp
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
static size_t countIn(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}
static tp::WireFile file(uint64_t id, const char* path) {
    tp::WireFile f;
    f.jobId = id; f.path = path; f.langA = "en"; f.langB = "fr"; f.mode = "auto"; f.quality = "auto";
    return f;
}
static tp::WireProxy proxy(uint64_t id, const char* src, const std::wstring& out) {
    tp::WireProxy p;
    p.jobId = id; p.src = src;
    for (wchar_t c : out) p.out += (char)(c & 0x7F);   // the smoke's paths are ASCII
    return p;
}

int main() {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = self;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    ::DeleteFileW((g_dir + L"fake_rx.log").c_str());

    Mgr m;
    m.setWake([] { g_wakes.fetch_add(1); });
    std::printf("== start ==\n");
    m.start();
    ok("the manager spawns the (fake) helper and sees its HELLO", waitFor(m, [&] { return m.running(); }, 10000));

    std::printf("== 1-2. CTL before FILE; FPROG, FCUE, FDONE in order ==\n");
    m.setPair("en", "fr");
    m.startFile(file(1, "ok3"));
    ok("job 1 ends with exactly one FDONE ok, count 3",
       waitFor(m, [&] { return lastDone(1) != nullptr; }, 5000) && countKind(Kind::FileDone, 1) == 1
       && lastDone(1)->fdone.result == "ok" && lastDone(1)->fdone.count == 3);
    {
        std::vector<int> order;   // 0 prog, 1 cue, 2 done
        std::vector<uint32_t> idx;
        uint64_t loadingTotal = 0; bool loadingVideo = false;
        for (const auto& x : g_all) {
            if (x.kind == Kind::FileProg && x.fprog.jobId == 1) {
                order.push_back(0);
                if (x.fprog.phase == "loading") { loadingTotal = x.fprog.totalMs; loadingVideo = x.fprog.hasVideo; }
            }
            if (x.kind == Kind::FileCue && x.cue.jobId == 1) { order.push_back(1); idx.push_back(x.cue.idx); }
            if (x.kind == Kind::FileDone && x.fdone.jobId == 1) order.push_back(2);
        }
        ok("FPROG, FPROG, FCUE x3, FDONE -- in that order",
           order == std::vector<int>({ 0, 0, 1, 1, 1, 2 }));
        ok("cues 0,1,2 in emission order", idx == std::vector<uint32_t>({ 0, 1, 2 }));
        ok("the loading FPROG already carries totalMs + hasVideo", loadingTotal == 9000 && loadingVideo);
        const std::string log = readLog();
        const size_t ctl = log.find("RX CTL|"), fil = log.find("RX FILE|1|ok3|");
        ok("the CTL reached the helper BEFORE the FILE", ctl != std::string::npos && fil != std::string::npos && ctl < fil,
           std::to_string(ctl) + " vs " + std::to_string(fil));
        ok("the FILE carries the job's own pair, direction and quality", log.find("RX FILE|1|ok3|en|fr|auto|auto") != std::string::npos);
    }
    ok("the bridge was woken (no 1 Hz wait for a subtitle file)", g_wakes.load() > 0);

    std::printf("== 3. cancel ==\n");
    m.startFile(file(2, "hold"));
    ok("job 2 is running (its FPROG came back)", waitFor(m, [&] { return countKind(Kind::FileProg, 2) > 0; }, 5000));
    m.cancelFile(2);
    ok("FCANCEL stops it: one FDONE cancelled",
       waitFor(m, [&] { return lastDone(2) != nullptr; }, 5000) && countKind(Kind::FileDone, 2) == 1
       && lastDone(2)->fdone.result == "cancelled");
    {
        // Two starts back to back: the first is either still UNSENT (cancelled locally) or already
        // on the wire (the helper cancels it when the second arrives). Either way: ONE FDONE
        // cancelled for it, and the second runs.
        m.startFile(file(3, "hold"));
        m.startFile(file(4, "ok3"));
        ok("a superseded start gets exactly one FDONE cancelled",
           waitFor(m, [&] { return lastDone(3) && lastDone(4); }, 5000) && countKind(Kind::FileDone, 3) == 1
           && lastDone(3)->fdone.result == "cancelled");
        ok("...and the newer job completes", lastDone(4) && lastDone(4)->fdone.result == "ok");
        const bool local = readLog().find("RX FILE|3|") == std::string::npos;
        std::printf("        (job 3 took the %s path)\n", local ? "UNSENT/local" : "on-the-wire");
    }
    {
        // An id that names a FILE never sent: cancel it before the worker's next lap.
        m.startFile(file(5, "hold"));
        m.cancelFile(5);
        ok("cancelling a FILE right after starting it answers it exactly once",
           waitFor(m, [&] { return lastDone(5) != nullptr; }, 5000) && countKind(Kind::FileDone, 5) == 1
           && lastDone(5)->fdone.result == "cancelled");
        ::Sleep(300);
        pump(m);
        ok("...and nothing more ever arrives for it", countKind(Kind::FileDone, 5) == 1);
    }

    std::printf("== 4. proxies ==\n");
    const std::wstring out6 = g_dir + L"p6.wav", out7 = g_dir + L"p7.wav";
    ::DeleteFileW(out7.c_str());
    m.requestProxy(proxy(6, "C:\\x.avi", out6));
    ok("a proxy request gets one FPROXYDONE ok=1 with its path",
       waitFor(m, [&] { return lastProxyDone(6) != nullptr; }, 5000) && lastProxyDone(6)->pdone.ok
       && countKind(Kind::ProxyDone, 6) == 1);
    m.requestProxy(proxy(7, "proxyhold", out6));
    ok("a held proxy reaches the helper", waitFor(m, [&] { return readLog().find("RX FPROXY|7|") != std::string::npos; }, 5000));
    m.requestProxy(proxy(8, "C:\\y.avi", out7));
    ok("a newer FPROXY supersedes the running one: ok=0 \"superseded\"",
       waitFor(m, [&] { return lastProxyDone(7) && lastProxyDone(8); }, 5000) && !lastProxyDone(7)->pdone.ok
       && lastProxyDone(7)->pdone.err == "superseded" && lastProxyDone(8)->pdone.ok);
    m.requestProxy(proxy(9, "proxyhold", out6));
    ok("(held again)", waitFor(m, [&] { return readLog().find("RX FPROXY|9|") != std::string::npos; }, 5000));
    m.cancelFile(9);
    ok("FCANCEL on a PROXY's id cancels that proxy",
       waitFor(m, [&] { return lastProxyDone(9) != nullptr; }, 5000) && !lastProxyDone(9)->pdone.ok
       && lastProxyDone(9)->pdone.err == "cancelled" && countKind(Kind::ProxyDone, 9) == 1);

    std::printf("== 5. the cap never eats a cue ==\n");
    {
        g_all.clear();
        m.startFile(file(10, "flood"));
        ::Sleep(3000);   // let 600 LINEs pile up behind the cap before anybody drains
        waitFor(m, [&] { return lastDone(10) != nullptr; }, 5000);
        int lines = 0;
        for (const auto& x : g_all) if (x.kind == Kind::Line) ++lines;
        ok("all 50 cues arrived, in order", countKind(Kind::FileCue, 10) == 50 && lastDone(10)
           && lastDone(10)->fdone.count == 50, std::to_string(countKind(Kind::FileCue, 10)));
        uint32_t expect = 0; bool inOrder = true;
        for (const auto& x : g_all) if (x.kind == Kind::FileCue && x.cue.jobId == 10) inOrder = inOrder && x.cue.idx == expect++;
        ok("...0..49 with no gap", inOrder);
        ok("while the LIVE lines were capped (the oldest dropped)", lines > 0 && lines <= 256, std::to_string(lines));
    }

    std::printf("== 6. death mid-job: synthesized answers, never re-sent ==\n");
    {
        m.requestProxy(proxy(11, "proxyhold", out6));
        ok("(a proxy is held)", waitFor(m, [&] { return readLog().find("RX FPROXY|11|") != std::string::npos; }, 5000));
        m.startFile(file(12, "crash"));
        ok("the dead helper's job gets a synthesized FDONE error",
           waitFor(m, [&] { return lastDone(12) != nullptr; }, 8000) && lastDone(12)->fdone.result == "error"
           && lastDone(12)->fdone.err == "The SuVtitle engine stopped during the analysis.",
           lastDone(12) ? lastDone(12)->fdone.err : std::string("none"));
        ok("...and its held proxy a synthesized FPROXYDONE", lastProxyDone(11) && !lastProxyDone(11)->pdone.ok
           && lastProxyDone(11)->pdone.err == "The SuVtitle engine stopped.");
        ok("the helper is respawned", waitFor(m, [&] { return countIn(readLog(), "START ") >= 2 && m.running(); }, 12000));
        ::Sleep(500);
        const std::string log = readLog();
        const size_t second = log.rfind("START ");
        ok("the NEW helper never received the FILE that killed the old one",
           second != std::string::npos && log.find("RX FILE|12|", second) == std::string::npos);
        ok("exactly one answer each, never two", countKind(Kind::FileDone, 12) == 1 && countKind(Kind::ProxyDone, 11) == 1);
    }

    std::printf("== 7. FDONE then death: the real answer wins ==\n");
    {
        m.startFile(file(13, "doneexit"));
        waitFor(m, [&] { return lastDone(13) != nullptr; }, 8000);
        ok("the helper's own FDONE ok is read out of the pipe after it died",
           lastDone(13) && lastDone(13)->fdone.result == "ok" && countKind(Kind::FileCue, 13) == 2);
        ::Sleep(400);
        pump(m);
        ok("...and no synthesized error follows it", countKind(Kind::FileDone, 13) == 1);
    }

    std::printf("== WE. FFOCUS (player v2) and the synthesized flag ==\n");
    {
        auto sy = [](const Mgr::Msg* x) { return x ? (x->synth ? std::string("synth") : std::string("real")) : std::string("none"); };
        ok("the crash's FDONE was SYNTHESIZED (Msg.synth), the helper's own ones are not",
           lastDone(12) && lastDone(12)->synth && lastProxyDone(11) && lastProxyDone(11)->synth
           && lastDone(13) && !lastDone(13)->synth,
           "12:" + sy(lastDone(12)) + " p11:" + sy(lastProxyDone(11)) + " 13:" + sy(lastDone(13)));
        ok("(a running helper)", waitFor(m, [&] { return m.running(); }, 12000));
        m.startFile(file(50, "hold"));
        m.focusFile(50, 30000);                    // the FILE is still in the queue: patched, not a 2nd message
        ok("(the held job runs)", waitFor(m, [&] { return countKind(Kind::FileProg, 50) > 0; }, 5000));
        std::string log = readLog();
        ok("an FFOCUS for a FILE not yet sent is PATCHED into it (focus 30000, one message)",
           log.find("RX FILE|50|hold|en|fr|auto|auto||30000") != std::string::npos && log.find("RX FFOCUS|50|") == std::string::npos);
        m.focusFile(50, 1000);
        m.focusFile(50, 2000);
        m.focusFile(50, 3000);
        ok("FFOCUS reaches the running job, the LATEST playhead last",
           waitFor(m, [&] { return readLog().find("RX FFOCUS|50|3000") != std::string::npos; }, 5000));
        ::Sleep(300);
        log = readLog();
        ok("...a burst is coalesced while unsent (at most two of the three written)", countIn(log, "RX FFOCUS|50|") <= 2,
           std::to_string(countIn(log, "RX FFOCUS|50|")));
        m.cancelFile(50);
        ok("(cancelled)", waitFor(m, [&] { return lastDone(50) != nullptr; }, 5000));
        m.focusFile(50, 9999);                     // the job has ANSWERED: nothing to steer any more
        m.focusFile(777, 5);                       // a job this helper never had
        ::Sleep(400);
        log = readLog();
        ok("an FFOCUS for a job that answered, or never ran here, is never written",
           log.find("RX FFOCUS|50|9999") == std::string::npos && log.find("RX FFOCUS|777|") == std::string::npos);
        ok("...and the helper's own FDONE cancelled is not marked synthesized", lastDone(50) && !lastDone(50)->synth);
    }

    // 8. (review of WC) a WRITE that fails must read the helper's last words BEFORE declaring the
    //    death -- on both write paths: the file commands', and the CTL's. MEASURED (wc\pipefill\
    //    pipeclose.cpp): a server write PENDING on a full pipe completes SUCCESSFULLY when the
    //    client closes; the NEXT write fails (ERROR_NO_DATA); the client's last messages are still
    //    readable. So: the "deaf" helper stops reading; two 200 KB FILEs fill the pipe and the
    //    second one's write pends; the helper writes 2 FCUE + FDONE ok and closes; the pending write
    //    completes; the tray reads at most ONE message per lap; the next write -- an FPROXY (8a) or
    //    a CTL (8b) -- fails with the FDONE still unread. Before the fix it was thrown away and the
    //    finished job got a synthesized "engine stopped" error instead.
    const std::string big(200 * 1024, 'x');
    auto bigFile = [&](uint64_t id) { tp::WireFile f = file(id, "big"); f.path = big; return f; };
    auto restartWarned = [&] {
        for (const auto& w : m.pollWarnings()) if (w.find(L"stopped responding") != std::wstring::npos) return true;
        return false;
    };
    std::printf("== 8a. a FILE-command write fails: the last words are read first ==\n");
    {
        ok("(a running helper)", waitFor(m, [&] { return m.running(); }, 12000));
        m.pollWarnings();                          // only this section's warnings count below
        m.startFile(file(30, "deaf"));
        ok("(the deaf job started)", waitFor(m, [&] { return countKind(Kind::FileProg, 30) > 0; }, 5000));
        m.startFile(bigFile(31));                  // fills the pipe (nobody reads it any more)
        ::Sleep(250);
        m.startFile(bigFile(32));                  // this write PENDS until the helper closes
        m.requestProxy(proxy(33, "C:\\w.avi", out6));   // ...and this one meets the closed pipe
        waitFor(m, [&] { return lastDone(30) != nullptr && lastProxyDone(33) != nullptr; }, 9000);
        ok("the job the helper FINISHED is reported finished (its own FDONE ok, its 2 cues)",
           lastDone(30) && lastDone(30)->fdone.result == "ok" && countKind(Kind::FileCue, 30) == 2,
           lastDone(30) ? lastDone(30)->fdone.result + " " + lastDone(30)->fdone.err : std::string("none"));
        ok("...exactly once; what it never acted on is answered with an error",
           countKind(Kind::FileDone, 30) == 1 && lastProxyDone(33) && !lastProxyDone(33)->pdone.ok
           && lastDone(31) && lastDone(31)->fdone.result == "error");
        ok("(and it WAS the write path: the tray said the helper stopped responding)", restartWarned());
    }
    std::printf("== 8b. a CTL write fails: the last words are read first ==\n");
    {
        ok("(respawned)", waitFor(m, [&] { return m.running(); }, 12000));
        ::Sleep(300);
        m.pollWarnings();
        m.startFile(file(40, "deaf"));
        ok("(the deaf job started)", waitFor(m, [&] { return countKind(Kind::FileProg, 40) > 0; }, 5000));
        m.startFile(bigFile(41));                  // fills the pipe
        ::Sleep(250);
        m.startFile(bigFile(42));                  // PENDS until the helper closes
        ::Sleep(300);
        m.setPair("de", "fr");                     // a CTL waits behind it: the first write after the close
        waitFor(m, [&] { return lastDone(40) != nullptr && lastDone(42) != nullptr; }, 9000);
        ok("the CTL path too: the finished job keeps its real FDONE ok and its cues",
           lastDone(40) && lastDone(40)->fdone.result == "ok" && countKind(Kind::FileCue, 40) == 2,
           lastDone(40) ? lastDone(40)->fdone.result + " " + lastDone(40)->fdone.err : std::string("none"));
        ok("...exactly once", countKind(Kind::FileDone, 40) == 1);
        ok("(and it WAS the write path: the tray said the helper stopped responding)", restartWarned());
    }

    std::printf("== stop ==\n");
    m.stop();
    ok("stopped", !m.running());
    {
        Mgr idle;   // never started: a job asked of it is answered at once, not lost
        idle.startFile(file(20, "ok3"));
        idle.requestProxy(proxy(21, "C:\\z.avi", out6));
        const auto got = idle.pollMessages();
        bool d = false, p = false;
        for (const auto& x : got) {
            if (x.kind == Kind::FileDone && x.fdone.jobId == 20 && x.fdone.result == "error") d = true;
            if (x.kind == Kind::ProxyDone && x.pdone.jobId == 21 && !x.pdone.ok) p = true;
        }
        ok("a manager that is not running answers a job and a proxy at once", d && p);
    }

    // =========================================================================================================
    // U3 (2026-11-02, the TRANSCRIPTION): FVOICE, the voice pass -- the FILE job's slot; FSPK on the ordered queue.
    // On a FRESH manager (a fresh helper, no failed-start streak carried from the deaths above).
    // =========================================================================================================
    auto voice = [](uint64_t id, const char* path) {
        tp::WireVoice v;
        v.jobId = id; v.path = path; v.spans = "0-1500,1500-2600,4000-9000"; v.changes = "1";
        return v;
    };
    auto lastSpk = [&](uint64_t id) {
        const Mgr::Msg* r = nullptr;
        for (const auto& x : g_all) if (x.kind == Kind::FileSpk && x.spk.jobId == id) r = &x;
        return r;
    };
    {
        Mgr mv;
        mv.setWake([] { g_wakes.fetch_add(1); });
        mv.start();
        std::printf("== V1. FVOICE: FPROG voices, ONE FSPK, then FDONE -- in that order ==\n");
        ok("(a fresh manager runs its helper)", waitFor(mv, [&] { return mv.running(); }, 10000));
        mv.startVoice(voice(60, "voice3"));
        ok("the voice pass ends with exactly one FDONE ok, count 3",
           waitFor(mv, [&] { return lastDone(60) != nullptr; }, 5000) && countKind(Kind::FileDone, 60) == 1
           && lastDone(60)->fdone.result == "ok" && lastDone(60)->fdone.count == 3);
        std::vector<int> order;
        for (const auto& x : g_all) {
            if (x.kind == Kind::FileProg && x.fprog.jobId == 60) order.push_back(x.fprog.phase == "voices" ? 0 : 9);
            if (x.kind == Kind::FileSpk && x.spk.jobId == 60) order.push_back(1);
            if (x.kind == Kind::FileDone && x.fdone.jobId == 60) order.push_back(2);
        }
        ok("FPROG voices, FSPK, FDONE -- in that order, one each", order == std::vector<int>({ 0, 1, 2 }));
        const Mgr::Msg* s = lastSpk(60);
        ok("the FSPK: 2 speakers, one label per piece (1 2 1), the model", s && s->spk.speakers == 2
           && s->spk.labels == std::vector<uint32_t>({ 1, 2, 1 }) && s->spk.model == "campplus" && !s->synth);
        ok("the FVOICE on the wire: the id, the path, the pieces, the changes",
           readLog().find("RX FVOICE|60|voice3||0-1500,1500-2600,4000-9000|1") != std::string::npos);

        std::printf("== V2. ONE job slot: a FILE and an FVOICE supersede each other ==\n");
        mv.startFile(file(61, "hold"));
        ok("(an analysis is held)", waitFor(mv, [&] { return countKind(Kind::FileProg, 61) > 0; }, 5000));
        mv.startVoice(voice(62, "voicehold"));
        ok("an FVOICE cancels the running analysis (one FDONE cancelled) and runs",
           waitFor(mv, [&] { return lastDone(61) && countKind(Kind::FileProg, 62) > 0; }, 5000)
           && countKind(Kind::FileDone, 61) == 1 && lastDone(61)->fdone.result == "cancelled");
        mv.startFile(file(63, "ok3"));
        ok("a FILE cancels the running voice pass (one FDONE cancelled, no FSPK) and runs",
           waitFor(mv, [&] { return lastDone(62) && lastDone(63); }, 5000) && countKind(Kind::FileDone, 62) == 1
           && lastDone(62)->fdone.result == "cancelled" && !lastSpk(62) && lastDone(63)->fdone.result == "ok");
        // back to back, both kinds: each superseded one is unsent (cancelled HERE) or held (the helper cancels it) -- one
        // FDONE cancelled either way -- and the last one runs
        mv.startVoice(voice(64, "voicehold"));
        mv.startFile(file(65, "hold"));
        mv.startVoice(voice(66, "voicehold"));
        mv.startVoice(voice(67, "voice3"));
        ok("back-to-back starts of either kind: each superseded one gets exactly one FDONE cancelled, the last runs",
           waitFor(mv, [&] { return lastDone(64) && lastDone(65) && lastDone(66) && lastDone(67); }, 5000)
           && countKind(Kind::FileDone, 64) == 1 && lastDone(64)->fdone.result == "cancelled"
           && countKind(Kind::FileDone, 65) == 1 && lastDone(65)->fdone.result == "cancelled"
           && countKind(Kind::FileDone, 66) == 1 && lastDone(66)->fdone.result == "cancelled"
           && lastDone(67)->fdone.result == "ok" && lastSpk(67) != nullptr);

        std::printf("== V3. cancel a voice pass ==\n");
        mv.startVoice(voice(70, "voicehold"));
        mv.cancelFile(70);
        ok("cancelling an FVOICE right after starting it answers it exactly once",
           waitFor(mv, [&] { return lastDone(70) != nullptr; }, 5000) && lastDone(70)->fdone.result == "cancelled");
        ::Sleep(300);
        pump(mv);
        ok("...and nothing more ever arrives for it", countKind(Kind::FileDone, 70) == 1);
        mv.startVoice(voice(71, "voicehold"));
        ok("(the voice pass is held)", waitFor(mv, [&] { return countKind(Kind::FileProg, 71) > 0; }, 5000));
        mv.cancelFile(71);
        ok("FCANCEL stops a running voice pass: the helper's own FDONE cancelled, no FSPK",
           waitFor(mv, [&] { return lastDone(71) != nullptr; }, 5000) && countKind(Kind::FileDone, 71) == 1
           && lastDone(71)->fdone.result == "cancelled" && !lastDone(71)->synth && !lastSpk(71));

        std::printf("== V4. death during a voice pass: a synthesized FDONE with its own sentence, never re-sent ==\n");
        const size_t before = countIn(readLog(), "START ");
        mv.startVoice(voice(72, "voicecrash"));
        ok("the dead helper's voice pass gets a SYNTHESIZED FDONE error, its own sentence",
           waitFor(mv, [&] { return lastDone(72) != nullptr; }, 8000) && lastDone(72)->fdone.result == "error" && lastDone(72)->synth
           && lastDone(72)->fdone.err == "The SuVtitle engine stopped while recognising the voices.",
           lastDone(72) ? lastDone(72)->fdone.err : std::string("none"));
        ok("the helper is respawned", waitFor(mv, [&] { return countIn(readLog(), "START ") > before && mv.running(); }, 12000));
        ::Sleep(500);
        const std::string log = readLog();
        const size_t second = log.rfind("START ");
        ok("the NEW helper never received the FVOICE that killed the old one; exactly one answer",
           second != std::string::npos && log.find("RX FVOICE|72|", second) == std::string::npos && countKind(Kind::FileDone, 72) == 1);
        mv.startVoice(voice(73, "voice3"));
        ok("...and the next voice pass runs on it", waitFor(mv, [&] { return lastDone(73) != nullptr; }, 5000)
           && lastDone(73)->fdone.result == "ok" && lastSpk(73) != nullptr);
        mv.stop();
    }
    std::printf("== V5. an FVOICE asked of a manager that is not running / that the engine would refuse ==\n");
    {
        Mgr idle;
        idle.startVoice(voice(79, "voice3"));
        bool d = false;
        for (const auto& x : idle.pollMessages())
            if (x.kind == Kind::FileDone && x.fdone.jobId == 79 && x.fdone.result == "error" && x.synth) d = true;
        ok("a manager that is not running answers an FVOICE at once (FDONE error)", d);
        tp::WireVoice noId = voice(0, "voice3"), noPath = voice(78, ""), noPiece = voice(77, "voice3");
        noPiece.spans.clear();
        idle.startVoice(noId);
        idle.startVoice(noPath);
        idle.startVoice(noPiece);
        ok("...and one the engine would refuse (id 0, no path, no piece) is never queued nor answered", idle.pollMessages().empty());
    }

    // =========================================================================================================
    // SUVTITLE LIVE (1.1.0, L2): each section on a FRESH manager -- a fresh worker, a fresh helper, no streak carried
    // =========================================================================================================
    auto strip = [](const char* key, const char* label, uint32_t chan, double gain) {
        tp::WireLiveInput in;
        in.key = key; in.label = label; in.chan = chan; in.gainDb = gain;
        return in;
    };
    auto logFrom = [&](size_t from) { const std::string s = readLog(); return from < s.size() ? s.substr(from) : std::string(); };
    std::vector<Mgr::Msg> got;
    auto pumpTo = [&](Mgr& x) { for (auto& y : x.pollMessages()) got.push_back(std::move(y)); };
    auto waitOn = [&](Mgr& x, auto pred, int ms) {
        for (int t = 0; t < ms; t += 20) { pumpTo(x); if (pred()) return true; ::Sleep(20); }
        pumpTo(x);
        return pred();
    };
    auto kinds = [&](Kind k) { int n = 0; for (const auto& x : got) if (x.kind == k) ++n; return n; };

    std::printf("== L1. the live CTL (scope=live), LIVE, INPUT, LEVELS (latest wins), LIVE 0 ==\n");
    Mgr a;
    const size_t atA = readLog().size();
    {
        got.clear();
        a.setWake([] { g_wakes.fetch_add(1); });
        a.start();
        ok("L1 (a fresh manager and helper)", waitOn(a, [&] { return a.running(); }, 10000));
        a.setPair("es", "fr");
        a.setLivePair("fr", "zh-tw");
        a.setLiveMode("zh2en");                    // the old vocabulary: mapped to b2a, like setMode
        tp::WireState st;
        bool echoed = false;
        for (int t = 0; t < 5000 && !echoed; t += 20) {
            if (a.pollState(st) && st.liveLangA == "fr" && st.liveLangB == "zh-tw" && st.liveMode == "b2a") echoed = true;
            else ::Sleep(20);
        }
        const std::string log = logFrom(atA);
        ok("the player's CTL carries NO scope (unchanged on the wire): CTL|||0|es|fr", log.find("RX CTL|||0|es|fr\n") != std::string::npos);
        ok("the LIVE pair travels on its own CTL, scope=live (mode mapped to b2a)", log.find("RX CTL|b2a||0|fr|zh-tw|live\n") != std::string::npos,
           log.substr(0, 300));
        ok("...and the engine's echo of the LIVE pair comes back in STATE (liveMode / liveLangA / liveLangB)", echoed);
        a.setLive(true, { strip("in:{x}", "Me", 0, 0.0), strip("out:{y}", "levels", 0, -3.0) });
        bool live = false;
        for (int t = 0; t < 5000 && !live; t += 20) { pumpTo(a); if (a.pollState(st) && st.live) live = true; else ::Sleep(20); }
        ok("LIVE 1 is written whole (2 strips, labels, channels, trims) and the STATE echo says live",
           live && logFrom(atA).find("RX LIVE|1|2|in:{x}|Me|0|0.0|out:{y}|levels|0|-3.0\n") != std::string::npos);
        tp::WireLevels lv;
        bool newest = false;
        int lvPolls = 0;
        for (int t = 0; t < 4000 && !newest; t += 20) {
            pumpTo(a);
            if (a.pollLevels(lv)) { ++lvPolls; if (lv.peaks.size() == 1 && lv.peaks[0].first == "out:{y}" && lv.peaks[0].second == -12.5f) newest = true; }
            if (!newest) ::Sleep(20);
        }
        ::Sleep(200);
        pumpTo(a);
        ok("INPUT: one ordered message per strip (open), in order", kinds(Kind::Input) == 2 && got.size() >= 2
           && got[0].kind == Kind::Input && got[0].input.key == "in:{x}" && got[0].input.state == "open" && got[1].input.key == "out:{y}",
           std::to_string(kinds(Kind::Input)));
        ok("500 LEVELS: NOT queued (no message for them), the NEWEST in the slot (-12.5 dBFS)",
           newest && got.size() == 2, "msgs " + std::to_string(got.size()) + ", slot polls " + std::to_string(lvPolls));
        ok("...the slot is consumed (a second poll: nothing new)", !a.pollLevels(lv));
        a.setLive(false, {});
        bool off = false;
        for (int t = 0; t < 5000 && !off; t += 20) { if (a.pollState(st) && !st.live) off = true; else ::Sleep(20); }
        ok("LIVE 0 (no strip on the wire) and the echo says not live", off && logFrom(atA).find("RX LIVE|0|0\n") != std::string::npos);
        const size_t n0 = countIn(logFrom(atA), "RX LIVE|");
        a.setLive(false, {});
        ::Sleep(400);
        ok("an UNCHANGED wish sends nothing", countIn(logFrom(atA), "RX LIVE|") == n0);
        // a Clear with NO change of pair or mode: the live CTL is written for the Clear alone, carrying the live pair as it stands
        const size_t c0 = countIn(logFrom(atA), "RX CTL|");
        a.askClear();
        ok("a Clear ALONE (no pair or mode change) still reaches the engine: a CTL with clear=1 on the live scope",
           waitOn(a, [&] { return countIn(logFrom(atA), "RX CTL|") > c0; }, 4000) && logFrom(atA).find("RX CTL|b2a||1|fr|zh-tw|live\n") != std::string::npos,
           logFrom(atA).substr(logFrom(atA).size() > 200 ? logFrom(atA).size() - 200 : 0));
    }

    std::printf("== L2. SRCLIST -> one Sources per complete list; a short list is asked again ==\n");
    {
        got.clear();
        a.requestSources();
        waitOn(a, [&] { return kinds(Kind::Sources) >= 1; }, 5000);
        const Mgr::Msg* s1 = nullptr;
        for (const auto& x : got) if (x.kind == Kind::Sources) s1 = &x;
        ok("SRC x3 + SRCEND 3 -> ONE Sources message, the whole list in order (kinds, keys, flags)",
           s1 && s1->srcGen == 1 && s1->srcs.size() == 3 && s1->srcs[0].kind == "in" && s1->srcs[0].key == "in:{0.0.1.00000000}.{mic}"
           && s1->srcs[0].flags == "default,comm" && s1->srcs[1].kind == "out" && s1->srcs[2].key == "app:zoom.exe" && s1->srcs[0].rate == 48000);
        got.clear();
        a.requestSources();                         // the helper's 2nd list is SHORT (SRCEND says 4)
        waitOn(a, [&] { return kinds(Kind::Sources) >= 1; }, 5000);
        ::Sleep(400);
        pumpTo(a);
        const Mgr::Msg* s3 = nullptr;
        for (const auto& x : got) if (x.kind == Kind::Sources) s3 = &x;
        ok("a SHORT list is not delivered: asked again, and only the next (whole) one arrives",
           kinds(Kind::Sources) == 1 && s3 && s3->srcGen == 3 && s3->srcs.size() == 3 && countIn(logFrom(atA), "RX SRCLIST") == 3,
           std::to_string(kinds(Kind::Sources)) + " / " + std::to_string(countIn(logFrom(atA), "RX SRCLIST")));
        // every list from the helper's 4th on stays SHORT: asked again TWICE at most, then the third is delivered as it is
        got.clear();
        const size_t asked = countIn(logFrom(atA), "RX SRCLIST");
        a.requestSources();
        waitOn(a, [&] { return kinds(Kind::Sources) >= 1; }, 5000);
        ::Sleep(700);                               // (an unbounded manager would still be asking)
        pumpTo(a);
        const Mgr::Msg* s4 = nullptr;
        for (const auto& x : got) if (x.kind == Kind::Sources) s4 = &x;
        ok("a list that STAYS short is asked again twice at most, then delivered as it is (3 SRCLIST, ONE Sources of the 3 rows it has)",
           kinds(Kind::Sources) == 1 && s4 && s4->srcs.size() == 3 && countIn(logFrom(atA), "RX SRCLIST") == asked + 3,
           std::to_string(kinds(Kind::Sources)) + " / " + std::to_string(countIn(logFrom(atA), "RX SRCLIST") - asked));
    }

    std::printf("== L3. a respawn re-sends the live pair, the wanted LIVE and the list's ask -- never the FILE ==\n");
    {
        got.clear();
        a.setLive(true, { strip("in:{x}", "Me", 0, 0.0) });
        ok("(listening on one strip)", waitOn(a, [&] { return logFrom(atA).find("RX LIVE|1|1|in:{x}|Me|0|0.0\n") != std::string::npos; }, 5000));
        const size_t before = readLog().size();
        a.startFile(file(91, "crash"));             // kills this helper
        ok("(the helper died and a new one runs)",
           waitOn(a, [&] { return countIn(logFrom(before), "START ") >= 1 && a.running(); }, 12000));
        waitOn(a, [&] { return logFrom(before).find("RX LIVE|1|1|in:{x}|Me|0|0.0\n") != std::string::npos; }, 4000);
        ::Sleep(300);
        const std::string inc = logFrom(before);
        const std::string born = inc.substr(inc.find("START "));
        ok("the NEW engine is told the live pair (scope=live), the player's pair, to listen on the same strip, and asked for the list",
           born.find("RX CTL|b2a||0|fr|zh-tw|live\n") != std::string::npos && born.find("RX CTL|||0|es|fr\n") != std::string::npos
           && born.find("RX LIVE|1|1|in:{x}|Me|0|0.0\n") != std::string::npos && born.find("RX SRCLIST") != std::string::npos,
           born.substr(0, 400));
        ok("...and NEVER the FILE that killed the old one (its answer is synthesized)",
           born.find("RX FILE|91|") == std::string::npos
           && waitOn(a, [&] { for (const auto& x : got) if (x.kind == Kind::FileDone && x.fdone.jobId == 91) return x.synth; return false; }, 3000));
        a.stop();
    }

    std::printf("== L4. RV2-5 on the live path: three deaths right after LIVE 1 drop the wanted LIVE ==\n");
    {
        Mgr b;
        b.start();
        ok("(a fresh manager and helper)", waitOn(b, [&] { return b.running(); }, 10000));
        const size_t at4 = readLog().size();
        std::vector<std::wstring> warns;
        b.setLive(true, { strip("in:{c}", "crash", 0, 0.0) });   // the helper dies ~100 ms after each LIVE 1
        bool dropped = false;
        for (int t = 0; t < 30000 && !dropped; t += 50) {
            b.pollMessages();
            for (auto& w : b.pollWarnings()) warns.push_back(w);
            if (b.pollLiveDropped()) dropped = true; else ::Sleep(50);
        }
        ::Sleep(300);                               // (the death handler says the failed-start warning right after dropping the wish)
        for (auto& w : b.pollWarnings()) warns.push_back(w);
        int said = 0, keeps = 0;
        for (const auto& w : warns) {
            if (w.find(L"kept stopping when it started listening") != std::wstring::npos) ++said;
            if (w.find(L"keeps failing to start") != std::wstring::npos) ++keeps;
        }
        ok("the THIRD death within 30 s of LIVE 1 drops the wanted LIVE (and only then: 3 LIVE 1 written)",
           dropped && !b.liveWanted() && countIn(logFrom(at4), "RX LIVE|1|1|in:{c}|crash|0|0.0\n") == 3,
           std::to_string(countIn(logFrom(at4), "RX LIVE|1|1|in:{c}|crash")));
        ok("...said once, in words", said == 1, std::to_string(said));
        ok("...each such death was a FAILED START too: the back-off applied, and the engine's \"keeps failing to start\" was said at the third",
           keeps == 1, std::to_string(keeps));
        ok("...and a one-shot (pollLiveDropped)", !b.pollLiveDropped());
        ok("(the next engine runs -- after the failed-start back-off)",
           waitOn(b, [&] { return countIn(logFrom(at4), "START ") >= 3 && b.running(); }, 20000));
        ::Sleep(800);
        ok("...and is NOT asked to listen", countIn(logFrom(at4), "RX LIVE|1") == 3);
        b.setLive(true, { strip("in:{ok}", "Me", 0, 0.0) });
        ok("the user's next Listen is sent again (his gesture forgives the earlier deaths)",
           waitOn(b, [&] { return logFrom(at4).find("RX LIVE|1|1|in:{ok}|Me|0|0.0\n") != std::string::npos; }, 5000) && b.liveWanted());
        // ...and the forgiveness is REAL: the count restarted from 0, so the next death right after a LIVE 1 is the FIRST of a new
        // run -- the wish stays (an unforgiven count of 3 would drop it at once)
        b.setLive(true, { strip("in:{c2}", "crash", 0, 0.0) });
        waitOn(b, [&] { return logFrom(at4).find("RX LIVE|1|1|in:{c2}|crash|0|0.0\n") != std::string::npos; }, 5000);
        ::Sleep(1500);                              // the helper dies ~100 ms after that LIVE 1; the death is seen at once
        const bool kept = b.liveWanted();
        const bool droppedAgain = b.pollLiveDropped();
        ok("the next death after his gesture is the FIRST of a new run: the wish is NOT dropped (the earlier deaths were forgiven)",
           kept && !droppedAgain, std::string(kept ? "wanted" : "not wanted") + (droppedAgain ? ", dropped" : ""));
        b.stop();
    }

    std::printf("== L5. AudioPB 1.1.0's rv5_drain (RV5-1): the LAST line of a helper that dies at once is delivered ==\n");
    for (int slow = 0; slow < 2; ++slow) {
        Mgr d;
        std::atomic<int> wk{ 0 };
        if (slow) d.setWake([&] { wk.fetch_add(1); ::Sleep(2); });   // a worker that LAGS the helper (CPU contention)
        else      d.setWake([&] { wk.fetch_add(1); });
        d.start();
        bool up = false;
        for (int t = 0; t < 10000 && !up; t += 20) { d.pollMessages(); up = d.running(); if (!up) ::Sleep(20); }
        d.setLivePair("dr", "ai");                  // the helper's trigger: the live CTL with clear=1 and langA "dr"
        d.askClear();
        uint64_t maxSeg = 0;
        size_t lines = 0;
        for (int t = 0; t < 3500; t += 20) {        // under the 4 s respawn back-off
            for (auto& x : d.pollMessages())
                if (x.kind == Kind::Line) { ++lines; if (x.line.segId > maxSeg) maxSeg = x.line.segId; }
            ::Sleep(20);
        }
        d.stop();
        const std::string extra = std::to_string(lines) + " lines, newest segId " + std::to_string(maxSeg);
        if (slow) ok("L5 slow wake (2 ms): the newest line (segId 300 of 300) WAS DELIVERED", up && maxSeg == 300, extra);
        else      ok("L5 normal wake: the newest line (segId 300 of 300) WAS DELIVERED (the cap drops the OLDEST)", up && maxSeg == 300, extra);
        std::printf("        (%s)\n", extra.c_str());
    }

    // ---- FIX ROUND (2026-10-26, docs/REVIEW_RV-LB.md), each on a FRESH manager (a fresh failed-start count) ----------------
    std::printf("== L6a (fix round). RVB-5: a REFUSED LIVE drops the wish, and is never re-sent to a respawned engine ==\n");
    {
        Mgr r;
        r.start();
        ok("L6a (a fresh manager and helper)", waitOn(r, [&] { return r.running(); }, 10000));
        const size_t at = readLog().size();
        r.setLive(true, { strip("in:{r}", "refuse", 0, 0.0) });
        std::string why;
        bool refused = false;
        for (int t = 0; t < 5000 && !refused; t += 20) { r.pollMessages(); refused = r.pollLiveRefused(why); if (!refused) ::Sleep(20); }
        std::string again;
        ok("L6a the engine's refusal (a STATE with err, not live) DROPS the wish, once, with the engine's own sentence",
           refused && !r.liveWanted() && why == "The live translation could not open these inputs." && !r.pollLiveRefused(again), why);
        r.startFile(file(60, "crash"));             // the helper dies: does the next one hear the refused LIVE again?
        ok("L6a (the helper is respawned)", waitOn(r, [&] { return countIn(logFrom(at), "START ") >= 1 && r.running(); }, 15000));
        ::Sleep(800);
        ok("L6a ...the refused LIVE is NOT sent to the new engine", countIn(logFrom(at), "RX LIVE|1|1|in:{r}|refuse") == 1);
        r.setLive(true, { strip("in:{ok}", "Me", 0, 0.0) });
        ok("L6a ...and the user's next Listen goes out as usual",
           waitOn(r, [&] { return logFrom(at).find("RX LIVE|1|1|in:{ok}|Me|0|0.0\n") != std::string::npos; }, 5000) && r.liveWanted());
        r.stop();
    }
    std::printf("== L6b (fix round). RVB-7: the picker closed (stopSources) -- a respawned engine is not asked for the list ==\n");
    {
        Mgr s;
        s.start();
        ok("L6b (a fresh manager and helper)", waitOn(s, [&] { return s.running(); }, 10000));
        const size_t at = readLog().size();
        s.requestSources();
        ok("L6b (the list is asked for)", waitOn(s, [&] { return countIn(logFrom(at), "RX SRCLIST") >= 1; }, 5000));
        s.stopSources();
        const size_t at2 = readLog().size();
        s.startFile(file(61, "crash"));
        ok("L6b (respawned)", waitOn(s, [&] { return countIn(logFrom(at2), "START ") >= 1 && s.running(); }, 15000));
        ::Sleep(800);
        ok("L6b the picker closed: the respawned engine is NOT asked for the list (no enumeration for nobody)",
           countIn(logFrom(at2), "RX SRCLIST") == 0);
        s.stop();
    }
    std::printf("== L6c (fix round). RVB-7: three deaths right after a SRCLIST drop the list's ask (said once); the user's next ask forgives ==\n");
    {
        Mgr q;
        q.start();
        ok("L6c (a fresh manager and helper)", waitOn(q, [&] { return q.running(); }, 10000));
        const size_t at = readLog().size();
        const std::wstring flag = g_dir + L"srcdie.flag";
        { FILE* f = nullptr; if (_wfopen_s(&f, flag.c_str(), L"wb") == 0 && f) std::fclose(f); }
        q.pollWarnings();
        q.requestSources();
        int said = 0;
        for (int t = 0; t < 30000 && !said; t += 50) {
            q.pollMessages();
            for (const auto& w : q.pollWarnings()) if (w.find(L"list of sources could not be made") != std::wstring::npos) ++said;
            if (!said) ::Sleep(50);
        }
        // (post-fix verification, 26/10: the flag STAYS until the last check of this block -- the helper keeps dying at each SRCLIST)
        ok("L6c an engine that dies at each SRCLIST: the third death drops the ask, said in words",
           said == 1 && countIn(logFrom(at), "RX SRCLIST") == 3, std::to_string(countIn(logFrom(at), "RX SRCLIST")) + " SRCLIST");
        const size_t at2 = readLog().size();
        ok("L6c (the next engine comes up -- after the failed-start back-off)", waitOn(q, [&] { return countIn(logFrom(at2), "START ") >= 1 && q.running(); }, 25000));
        ::Sleep(800);
        ok("L6c ...and is NOT asked for the list (no crash loop)", countIn(logFrom(at2), "RX SRCLIST") == 0);
        q.requestSources();
        ok("L6c ...the user opening the picker again asks again (his gesture forgives)",
           waitOn(q, [&] { return countIn(logFrom(at2), "RX SRCLIST") >= 1; }, 5000));
        // Post-fix verification (26/10). The flag is STILL there: that SRCLIST killed this engine too. The mutant "the user's next ask does
        // not forgive" survived the check above, which wants ONE SRCLIST only (asked once all the same with srcFailStreak_ left at 3).
        // Forgiven = the streak started again at 0: this death is the FIRST of a new run, so the ask SURVIVES it and the next engine
        // (after the failed-start back-off, 16 s here) is asked a SECOND time, nothing said. Not forgiven = 3 + 1: dropped at once,
        // said again, no second SRCLIST.
        int saidAgain = 0;
        const bool second = waitOn(q, [&] {
            for (const auto& w : q.pollWarnings()) if (w.find(L"list of sources could not be made") != std::wstring::npos) ++saidAgain;
            return countIn(logFrom(at2), "RX SRCLIST") >= 2 || saidAgain > 0;
        }, 40000);
        ::DeleteFileW(flag.c_str());
        ok("L6c ...and the first death after his gesture does NOT drop the ask again (a 2nd SRCLIST follows, nothing said): the streak was zeroed",
           second && saidAgain == 0 && countIn(logFrom(at2), "RX SRCLIST") >= 2,
           std::to_string(countIn(logFrom(at2), "RX SRCLIST")) + " SRCLIST, said " + std::to_string(saidAgain));
        q.stop();
    }
    std::printf("== L6d (fix round). RVB-13: an edit of the strips while listening (a trim) forgives NOTHING ==\n");
    {
        Mgr e;
        e.start();
        ok("L6d (a fresh manager and helper)", waitOn(e, [&] { return e.running(); }, 10000));
        const size_t at = readLog().size();
        e.setLive(true, { strip("in:{c}", "crash", 0, 0.0) });    // the helper dies ~100 ms after each LIVE 1
        ok("L6d (two deaths right after LIVE 1)",
           waitOn(e, [&] { return countIn(logFrom(at), "RX LIVE|1|1|in:{c}|crash|0|0.0\n") >= 2; }, 12000));
        ::Sleep(500);                                // (the second death is seen)
        e.setLive(true, { strip("in:{c}", "crash", 0, -1.0) });   // a TRIM while the wish is on -- not the user trying again
        bool dropped = false;
        for (int t = 0; t < 12000 && !dropped; t += 50) { e.pollMessages(); e.pollWarnings(); dropped = e.pollLiveDropped(); if (!dropped) ::Sleep(50); }
        ok("L6d the third death right after a LIVE 1 drops the wish though a trim was sent between (an edit is no new attempt)",
           dropped && !e.liveWanted() && countIn(logFrom(at), "RX LIVE|1|1|in:{c}|crash|0|-1.0\n") == 1,
           std::string(dropped ? "dropped" : "not dropped") + ", " + std::to_string(countIn(logFrom(at), "RX LIVE|1|")) + " LIVE 1");
        e.stop();
    }
    std::printf("== L6e (fix round). RVB-6: every LINE says which engine incarnation said it ==\n");
    {
        Mgr g;
        g.start();
        ok("L6e (a fresh manager and helper)", waitOn(g, [&] { return g.running(); }, 10000));
        auto drainGens = [&](std::vector<uint32_t>& gens) {   // the rv5 trigger: 300 LINE, then the helper exits
            g.askClear();
            for (int t = 0; t < 2500; t += 20) {
                for (auto& x : g.pollMessages()) if (x.kind == Kind::Line) gens.push_back(x.gen);
                ::Sleep(20);
            }
        };
        g.setLivePair("dr", "ai");
        std::vector<uint32_t> one, two;
        drainGens(one);
        ok("L6e (respawned)", waitOn(g, [&] { return g.running(); }, 15000));
        ::Sleep(500);
        drainGens(two);
        bool allOne = !one.empty(), allTwo = !two.empty();
        for (uint32_t v : one) allOne = allOne && v == 1;
        for (uint32_t v : two) allTwo = allTwo && v == 2;
        ok("L6e the first engine's LINEs say 1, the respawned one's say 2 (the bridge keys a voice by it: a new S1 is a new voice)",
           allOne && allTwo, std::to_string(one.size()) + " + " + std::to_string(two.size()) + " lines");
        g.stop();
    }
    ::DeleteFileW(out6.c_str());
    ::DeleteFileW(out7.c_str());
    std::printf("\n%s: %d passed, %d failed\n", g_fail ? "TRMGR SMOKE FAILED" : "TRMGR SMOKE OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

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
                           : k == Kind::FileCue ? x.cue.jobId : k == Kind::FileProg ? x.fprog.jobId : 0;
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
    ::DeleteFileW(out6.c_str());
    ::DeleteFileW(out7.c_str());
    std::printf("\n%s: %d passed, %d failed\n", g_fail ? "TRMGR SMOKE FAILED" : "TRMGR SMOKE OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// =============================================================================
// SuVtitle tests/cpp/pipe/pipe_smoke.cpp -- S-HOST fix round (2026-10-23): the proof of REVIEW_RV2_App.md RV2-1
// (the engine pipe), RV2-2 (stop() during a start) and RV2-5 (a post-HELLO crash loop) on the REAL
// app\TranslatorManager.cpp, which spawns pipe\fake_engine.cpp built as SuVtitleEngine.exe next to this exe
// (tests\cpp\work\pipe\). Started from RV-2's harness (scratchpad\review\rv2\pipe\rv2_pipe.cpp + rv2_fake.cpp): the
// same attacks, the verdicts INVERTED. Every check below FAILED on the code before the fix (built over the pre-fix
// copy by scratchpad\s-host\fixproof\before.py pipe) except the two marked "(unchanged)".
//   P0  the pipe's security, read by the engine on its own handle: a protected DACL, this user only (no WD, no AN)
//   P1  a squatter created seq 1 first (unlimited instances, NULL DACL, listening): nobody ever connects to it, the
//       engine runs on the supervisor's own pipe (seq 2, server pid = this process), the "taken" sentence said once
//   P3  seq 1..4 squatted: the engine runs at once on seq 5 and a queued FILE reaches it (before: ~16 s, then failed)
//   P4  a process of the same user opens seq 1 first and says HELLO: refused, the engine is the one running
//   P5  an engine that dies 300 ms after each HELLO: "keeps failing to start" said once, the respawns slow down
//   P6  stop() while the engine has not connected / not said HELLO: < 300 ms, and its process is gone
// Pipes named after THIS process; the fake's log next to the exe. Nothing of the user's is touched. ASCII only.
// =============================================================================
#include "../../../app/TranslatorManager.h"

#include <aclapi.h>
#include <sddl.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

using Mgr = pbr::TranslatorManager;
namespace tp = pbt::trpipe;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) ++g_pass; else ++g_fail;
    std::printf("  %s  %s%s%s\n", cond ? "PASS" : "FAIL", name, extra.empty() ? "" : "  -> ", extra.c_str());
    std::fflush(stdout);
}
static void info(const std::string& s) { std::printf("  info  %s\n", s.c_str()); }

static std::wstring exeDir() {
    wchar_t b[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p(b);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}
static std::string narrow(const std::wstring& w) {                                    // (ASCII names only)
    std::string o;
    for (wchar_t c : w) o += static_cast<char>(c);
    return o;
}
static std::wstring wname(uint32_t seq) {
    const std::string n = tp::pipeName(::GetCurrentProcessId(), seq);
    return std::wstring(n.begin(), n.end());
}
static std::vector<std::string> logLines() {
    std::vector<std::string> v;
    std::ifstream f((exeDir() + L"fake_engine.log").c_str());
    std::string l;
    while (std::getline(f, l)) v.push_back(l);
    return v;
}
static size_t logSize() { return logLines().size(); }
static size_t countLog(size_t from, const char* what) {
    const auto v = logLines();
    size_t n = 0;
    for (size_t i = from; i < v.size(); ++i) if (v[i].find(what) != std::string::npos) ++n;
    return n;
}
static std::string firstLog(size_t from, const char* what) {
    const auto v = logLines();
    for (size_t i = from; i < v.size(); ++i) if (v[i].find(what) != std::string::npos) return v[i];
    return std::string();
}
// the fakes started since `from`: "<tick> <pid> START ..."
static std::vector<DWORD> startedPids(size_t from) {
    std::vector<DWORD> out;
    const auto v = logLines();
    for (size_t i = from; i < v.size(); ++i) {
        if (v[i].find(" START ") == std::string::npos) continue;
        const size_t a = v[i].find(' ');
        if (a != std::string::npos) out.push_back(static_cast<DWORD>(std::strtoul(v[i].c_str() + a + 1, nullptr, 10)));
    }
    return out;
}
static bool procGone(DWORD pid) {
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;                                           // no such process any more
    const bool gone = ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    ::CloseHandle(h);
    return gone;
}
static bool allGone(const std::vector<DWORD>& pids) {
    for (DWORD p : pids) if (!procGone(p)) return false;
    return true;
}
static uint64_t now() { return ::GetTickCount64(); }
// between the steps: every fake of the step gone (a live client end keeps a pipe NAME alive)
static void waitGone(size_t from, uint64_t ms) {
    const uint64_t t0 = now();
    while (now() - t0 < ms && !allGone(startedPids(from))) ::Sleep(20);
}
static bool waitRunning(Mgr& m, uint64_t ms) {
    const uint64_t t0 = now();
    while (now() - t0 < ms) { if (m.running()) return true; ::Sleep(10); }
    return false;
}
static size_t countWarn(const std::vector<std::wstring>& w, const wchar_t* what) {
    size_t n = 0;
    for (const auto& s : w) if (s.find(what) != std::wstring::npos) ++n;
    return n;
}
static std::string warnStr(const std::vector<std::wstring>& w) {
    std::string o;
    for (const auto& s : w) o += "[" + narrow(s) + "]";
    return o;
}
// A squatter's first instance: `maxInst` instances, and a NULL DACL (everyone may create instances / connect).
static HANDLE squat(const std::wstring& name, DWORD maxInst, bool nullDacl) {
    SECURITY_DESCRIPTOR sd;
    ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    ::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };
    return ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, maxInst, 1 << 16, 1 << 16, 0,
                              nullDacl ? &sa : nullptr);
}
struct Listen {
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    bool start(HANDLE pipe) {
        h = pipe;
        if (h == INVALID_HANDLE_VALUE) return false;
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (::ConnectNamedPipe(h, &ov)) { ::SetEvent(ov.hEvent); return true; }
        const DWORD e = ::GetLastError();
        if (e == ERROR_PIPE_CONNECTED) { ::SetEvent(ov.hEvent); return true; }
        return e == ERROR_IO_PENDING;
    }
    bool connected(DWORD ms) { return ov.hEvent && ::WaitForSingleObject(ov.hEvent, ms) == WAIT_OBJECT_0; }
    void close() {
        if (h != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(h, &ov);
            DWORD n = 0;
            ::GetOverlappedResult(h, &ov, &n, TRUE);
            ::CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        if (ov.hEvent) { ::CloseHandle(ov.hEvent); ov.hEvent = nullptr; }
    }
};
static std::string userSid() {
    std::string out;
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD n = 0;
    ::GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> b(n ? n : 1);
    LPSTR s = nullptr;
    if (n && ::GetTokenInformation(tok, TokenUser, b.data(), n, &n)
        && ::ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, &s)) {
        out = s;
        ::LocalFree(s);
    }
    ::CloseHandle(tok);
    return out;
}
static size_t countSub(const std::string& s, const char* what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("pipe_smoke (pid %lu): the REAL TranslatorManager.cpp vs a scripted SuVtitleEngine.exe\n", ::GetCurrentProcessId());
    ::DeleteFileW((exeDir() + L"fake_engine.log").c_str());
    const std::string me = std::to_string(::GetCurrentProcessId());

    // ---- P0 the pipe's security ----------------------------------------------------------------------------------------
    std::printf("[P0] the engine's pipe security, read by the engine on its own handle\n");
    {
        const size_t l0 = logSize();
        Mgr m;
        m.setSpawnArgs(L"--fake ok");
        m.start();
        const bool run = waitRunning(m, 5000);
        const std::string line = firstLog(l0, " SDDL ");
        const std::string sd = line.find("SDDL ") == std::string::npos ? line : line.substr(line.find("SDDL ") + 5);
        const std::string sid = userSid();
        info("the engine reads: " + sd + "   (this user: " + sid + ")");
        ok("P0 a PROTECTED DACL with ONE ace, this user's: no Everyone (WD), no Anonymous (AN) (before: the default DACL, "
           "WD and AN with read access)",
           run && !sid.empty() && sd.rfind("D:P(", 0) == 0 && sd.find(";;;" + sid + ")") != std::string::npos
           && sd.find(";;;WD)") == std::string::npos && sd.find(";;;AN)") == std::string::npos && countSub(sd, "(A;") == 1, sd);
        const std::string conn = firstLog(l0, "CONNECTED server_pid ");
        ok("P0 (unchanged) the engine's server is this process", conn.size() > me.size() && conn.compare(conn.size() - me.size(), me.size(), me) == 0, conn);
        m.stop();
        waitGone(l0, 5000);
    }

    // ---- P1 a squatter on seq 1 ----------------------------------------------------------------------------------------
    std::printf("[P1] seq 1 created FIRST by a squatter (unlimited instances, NULL DACL, listening)\n");
    {
        const size_t l0 = logSize();
        Listen sq;
        const bool sqOk = sq.start(squat(wname(1), PIPE_UNLIMITED_INSTANCES, true));
        Mgr m;
        m.setSpawnArgs(L"--fake ok");
        m.start();
        const bool run = waitRunning(m, 5000);
        ::Sleep(300);
        const bool hijacked = sq.connected(0);
        const auto w = m.pollWarnings();
        const std::string st = firstLog(l0, " START "), conn = firstLog(l0, "CONNECTED server_pid ");
        info("squatter connected: " + std::string(hijacked ? "YES" : "no") + "; engine: " + st + " / " + conn + "; warnings " + warnStr(w));
        ok("P1 nobody ever connects to the squatter's pipe (before: the engine said HELLO to the squatter)", sqOk && !hijacked);
        ok("P1 the engine runs on the supervisor's OWN pipe: seq 2, its server = this process",
           run && st.find(narrow(wname(2))) != std::string::npos && conn.size() > me.size()
           && conn.compare(conn.size() - me.size(), me.size(), me) == 0, st + " / " + conn);
        ok("P1 said once: another program had taken the name", countWarn(w, L"had taken the SuVtitle engine's connection name") == 1, warnStr(w));
        m.stop();
        sq.close();
        waitGone(l0, 5000);
    }
    ::Sleep(200);

    // ---- P3 four squatted names ----------------------------------------------------------------------------------------
    std::printf("[P3] seq 1..4 squatted (one instance each): a queued FILE job\n");
    {
        std::vector<HANDLE> sq;
        for (uint32_t s = 1; s <= 4; ++s) sq.push_back(squat(wname(s), 1, false));
        const size_t l0 = logSize();
        Mgr m;
        m.setSpawnArgs(L"--fake ok");
        m.start();
        tp::WireFile f;
        f.jobId = 7; f.path = "D:\\film.mkv"; f.langA = "fr"; f.langB = "en"; f.mode = "auto"; f.quality = "auto";
        m.startFile(f);
        const uint64_t t0 = now();
        const bool run = waitRunning(m, 10000);
        const uint64_t tRun = now() - t0;
        bool got = false;
        for (const uint64_t t1 = now(); !got && now() - t1 < 3000; ::Sleep(20)) got = countLog(l0, "RX FILE|7|") > 0;
        std::vector<std::wstring> w = m.pollWarnings();
        bool failed = false;
        for (auto& msg : m.pollMessages())
            if (msg.kind == Mgr::Msg::Kind::FileDone && msg.fdone.jobId == 7) failed = true;
        info("engine running after " + std::to_string(tRun) + " ms: " + firstLog(l0, " START ") + "; warnings " + warnStr(w));
        ok("P3 four taken names: the engine runs within 2 s, on seq 5 (before: ~16 s)",
           run && tRun < 2000 && firstLog(l0, " START ").find(narrow(wname(5))) != std::string::npos, std::to_string(tRun) + " ms");
        ok("P3 the queued FILE reaches the engine, one start, nothing 'keeps failing' (before: the job failed so)",
           got && !failed && countLog(l0, " START ") == 1 && countWarn(w, L"keeps failing") == 0, warnStr(w));
        m.stop();
        for (HANDLE h : sq) if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        waitGone(l0, 5000);
    }
    ::Sleep(200);

    // ---- P4 a client that is not the engine, first in line -----------------------------------------------------------------
    std::printf("[P4] a process of the same user opens seq 1 first and says HELLO (the engine connects 1.5 s later)\n");
    {
        const size_t l0 = logSize();
        std::atomic<bool> stopRogue{ false }, rogueIn{ false };
        HANDLE rogue = INVALID_HANDLE_VALUE;
        std::thread th([&] {
            const std::wstring n = wname(1);
            const uint64_t t0 = now();
            while (!stopRogue.load() && now() - t0 < 5000) {
                HANDLE c = ::CreateFileW(n.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (c != INVALID_HANDLE_VALUE) {
                    DWORD pm = PIPE_READMODE_MESSAGE;
                    ::SetNamedPipeHandleState(c, &pm, nullptr, nullptr);
                    const char hello[] = "HELLO\tevil";
                    DWORD wr = 0;
                    ::WriteFile(c, hello, sizeof(hello) - 1, &wr, nullptr);
                    rogue = c;
                    rogueIn = true;
                    break;
                }
                ::Sleep(2);
            }
        });
        Mgr m;
        m.setSpawnArgs(L"--fake slow");
        const uint64_t t0 = now();
        m.start();
        const bool run = waitRunning(m, 7000);
        const uint64_t tRun = now() - t0;
        stopRogue = true;
        th.join();
        const std::string slot = m.slotName();
        const auto w = m.pollWarnings();
        info("rogue in: " + std::string(rogueIn ? "yes" : "no") + "; running after " + std::to_string(tRun) + " ms as '" + slot + "'; warnings " + warnStr(w));
        ok("P4 the client that is not the engine is refused: the ENGINE runs (slot 'engine', never 'evil')",
           rogueIn && run && slot.find("engine") != std::string::npos && slot.find("evil") == std::string::npos, slot);
        ok("P4 ...within the engine's own start (< 4 s)", run && tRun < 4000, std::to_string(tRun) + " ms");
        ok("P4 said once: another program tried to connect", countWarn(w, L"tried to connect to the SuVtitle engine") == 1, warnStr(w));
        if (rogue != INVALID_HANDLE_VALUE) ::CloseHandle(rogue);
        m.stop();
        waitGone(l0, 5000);
    }
    ::Sleep(200);

    // ---- P5 the crash loop -----------------------------------------------------------------------------------------------
    std::printf("[P5] an engine that dies 300 ms after its HELLO, every time (20 s)\n");
    {
        const size_t l0 = logSize();
        Mgr m;
        m.setSpawnArgs(L"--fake crashloop");
        m.start();
        std::vector<std::wstring> w;
        uint64_t tWarn = 0;
        const uint64_t t0 = now();
        while (now() - t0 < 20000) {
            for (auto& x : m.pollWarnings()) {
                if (!tWarn && x.find(L"keeps failing to start") != std::wstring::npos) tWarn = now() - t0;
                w.push_back(x);
            }
            m.pollMessages();
            ::Sleep(50);
        }
        const size_t starts = countLog(l0, " START ");
        info("in 20 s: " + std::to_string(starts) + " engine starts; 'keeps failing' at " + std::to_string(tWarn) + " ms; warnings " + warnStr(w));
        ok("P5 'The SuVtitle engine keeps failing to start.' said ONCE, at the third quick death (before: never)",
           countWarn(w, L"The SuVtitle engine keeps failing to start.") == 1 && tWarn > 0 && tWarn < 12000, warnStr(w));
        ok("P5 ...and the respawns slow down: 3 or 4 starts in 20 s (4 s, 4 s, then 8 s; before: one every ~4.3 s = 5)",
           starts >= 3 && starts <= 4, std::to_string(starts));
        m.stop();
        waitGone(l0, 5000);
    }
    ::Sleep(200);

    // ---- P6 stop() latency -----------------------------------------------------------------------------------------------
    std::printf("[P6] stop() latency (the window's WM_DESTROY runs it on the UI thread)\n");
    auto timedStop = [&](const wchar_t* mode, bool waitRun, uint64_t& ms, bool& gone, size_t* late) {
        const size_t l0 = logSize();
        Mgr m;
        m.setSpawnArgs(std::wstring(L"--fake ") + mode);
        m.start();
        if (waitRun) waitRunning(m, 5000); else ::Sleep(400);
        const auto pids = startedPids(l0);
        const uint64_t t0 = now();
        m.stop();
        ms = now() - t0;
        gone = !pids.empty() && allGone(pids);
        if (late) { ::Sleep(3500); *late = countLog(l0, "QUIT_DONE_LATE"); }
        waitGone(l0, 5000);
    };
    uint64_t okMs = 0, ncMs = 0, nhMs = 0, sqMs = 0;
    bool okGone = false, ncGone = false, nhGone = false, sqGone = false;
    size_t late = 0;
    timedStop(L"noconnect", false, ncMs, ncGone, nullptr);
    timedStop(L"nohello", false, nhMs, nhGone, nullptr);
    timedStop(L"ok", true, okMs, okGone, nullptr);
    timedStop(L"slowquit", true, sqMs, sqGone, &late);
    info("stop(): not connected yet " + std::to_string(ncMs) + " ms | connected, no HELLO " + std::to_string(nhMs) + " ms | running "
         + std::to_string(okMs) + " ms | QUIT honoured in 3 s " + std::to_string(sqMs) + " ms (its QUIT finished: " + std::to_string(late) + ")");
    ok("P6 stop() while the engine has NOT CONNECTED yet: < 300 ms, its process gone (before: ~8 s on the UI thread)",
       ncMs < 300 && ncGone, std::to_string(ncMs) + " ms, gone " + std::to_string(ncGone));
    ok("P6 stop() while the engine is connected but has NOT SAID HELLO: < 300 ms, its process gone (before: ~8 s)",
       nhMs < 300 && nhGone, std::to_string(nhMs) + " ms, gone " + std::to_string(nhGone));
    ok("P6 (unchanged) a running engine: stop() < 1500 ms, its process gone", okMs < 1500 && okGone, std::to_string(okMs) + " ms");
    ok("P6 (unchanged) an engine needing > ~1 s to honour QUIT is terminated at ~1 s (its QUIT never completes)",
       sqMs < 2500 && sqGone && late == 0, std::to_string(sqMs) + " ms, late " + std::to_string(late));

    std::printf("\npipe_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// =============================================================================
// SuVtitle tests/cpp/pipe/fake_engine.cpp -- S-HOST fix round (2026-10-23). A scripted stand-in for SuVtitleEngine.exe,
// built as SuVtitleEngine.exe NEXT TO pipe_smoke.exe (tests\cpp\work\pipe\) so the REAL app\TranslatorManager.cpp
// spawns it exactly as it spawns the real engine. Started from RV-2's scratchpad\review\rv2\pipe\rv2_fake.cpp (same
// modes, same log); added: the SDDL of the pipe it connected to, read on its own handle (P0).
// Behaviour chosen by "--fake <mode>" (the manager's setSpawnArgs puts it on the command line):
//   ok          connect, HELLO, STATE listening, then read until QUIT / a broken pipe
//   crashloop   connect, HELLO, STATE, live 300 ms, then die (a post-HELLO crash: the restart loop)
//   noconnect   never connect (a slow first start: AV scan of a new exe, a sleeping disk), sleep 30 s
//   nohello     connect, never say HELLO, sleep 30 s
//   slow        wait 1500 ms, then like ok (lets another client be first in line)
//   slowquit    like ok, but QUIT takes 3 s to honour (the model servers' shutdown)
// Every event is appended to fake_engine.log next to the exe: "<tick> <pid> <event>". ASCII only.
// =============================================================================
#include "../../../common/TrPipe.h"

#include <aclapi.h>
#include <sddl.h>
#include <cstdio>
#include <share.h>
#include <string>

#pragma comment(lib, "advapi32.lib")

namespace tp = pbt::trpipe;
static std::wstring g_log;

static void logLine(const std::string& s) {
    FILE* f = nullptr;
    for (int i = 0; i < 25 && !f; ++i) { f = _wfsopen(g_log.c_str(), L"ab", _SH_DENYNO); if (!f) ::Sleep(20); }
    if (!f) return;
    std::string v = s;
    for (auto& c : v) if (c == '\t') c = '|';
    std::fprintf(f, "%llu %lu %s\n", (unsigned long long)::GetTickCount64(), ::GetCurrentProcessId(), v.c_str());
    std::fclose(f);
}

static std::string narrow(const std::wstring& w) {     // (ASCII: modes and pipe names)
    std::string o;
    for (wchar_t c : w) o += static_cast<char>(c);
    return o;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring pipeName, mode = L"ok";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--pipe") pipeName = argv[i + 1];
        if (std::wstring(argv[i]) == L"--fake") mode = argv[i + 1];
    }
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_log = self;
    g_log = g_log.substr(0, g_log.find_last_of(L"\\/") + 1) + L"fake_engine.log";
    logLine("START " + narrow(mode) + " " + narrow(pipeName));
    if (pipeName.empty()) return 2;
    if (mode == L"noconnect") { ::Sleep(30000); return 0; }
    if (mode == L"slow") ::Sleep(1500);

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 40 && pipe == INVALID_HANDLE_VALUE; ++i) {   // like the real engine: retry a busy pipe
        pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD e = ::GetLastError();
        logLine("CONNECT_RETRY err " + std::to_string(e));
        if (e == ERROR_PIPE_BUSY) ::WaitNamedPipeW(pipeName.c_str(), 500); else ::Sleep(50);
    }
    if (pipe == INVALID_HANDLE_VALUE) { logLine("CONNECT_FAIL"); return 3; }
    ULONG serverPid = 0;
    ::GetNamedPipeServerProcessId(pipe, &serverPid);
    logLine("CONNECTED server_pid " + std::to_string(serverPid));
    {
        // P0: the security of the pipe this engine was given, as Windows reports it on the engine's own handle
        PSECURITY_DESCRIPTOR sd = nullptr;
        PACL dacl = nullptr;
        std::string sddl = "?";
        const DWORD e = ::GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
        if (e == ERROR_SUCCESS) {
            LPSTR s = nullptr;
            if (::ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &s, nullptr)) {
                sddl = s;
                ::LocalFree(s);
            }
            ::LocalFree(sd);
        } else {
            sddl = "ERR " + std::to_string(e);
        }
        logLine("SDDL " + sddl);
    }
    DWORD pm = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(pipe, &pm, nullptr, nullptr);
    if (mode == L"nohello") { ::Sleep(30000); return 0; }

    tp::writeMessage(pipe, "HELLO\tengine", 3000);
    tp::WireState st;
    st.status = "listening";
    tp::writeMessage(pipe, tp::packState(st), 3000);
    logLine("HELLO_SENT");
    const ULONGLONG born = ::GetTickCount64();
    for (;;) {
        if (mode == L"crashloop" && ::GetTickCount64() - born > 300) { logLine("CRASH"); ::ExitProcess(9); }
        std::string msg;
        bool broken = false;
        if (!tp::readMessage(pipe, msg, 50, &broken)) {
            if (broken) { logLine("BROKEN"); return 0; }
            continue;
        }
        logLine("RX " + msg.substr(0, 120));
        if (msg == "QUIT") {
            if (mode == L"slowquit") { ::Sleep(3000); logLine("QUIT_DONE_LATE"); }
            return 0;
        }
    }
}

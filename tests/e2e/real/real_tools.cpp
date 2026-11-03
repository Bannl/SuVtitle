// real_tools.cpp -- the real-app E2E kit's Win32 helper (S-HOST, 2026-10-23). PORTED from AudioPB's
// scratchpad/filemode_2026-10-10/wg_real/wg_real_tools.cpp (641f1aa; frozen copy in SuVtitle\scratchpad\
// from_audiopb_641f1aa\). Changed: SuVtitle's TrFileStore.h (cacheDir = %LOCALAPPDATA%\SuVtitle\Files), `mkrecord ...
// audiopb` (the record written where AudioPB 1.0.6 kept it -- the first-run IMPORT brings it over), + `ident`, `kids`,
// `alive` (SuVtitle's engine is a child process). ASCII source, like the product.
//
//   real_tools mkrecord <media> <langA> <langB> [cur|old|none|<n>] [1] [audiopb]
//                                          a COMPLETE cache record for <media> (the user's 4 film lines, French ->
//                                          English), written with the product's own TrFileStore.h (cacheKey, packRecord,
//                                          writeFileAvSafe) into the TEST profile: %LOCALAPPDATA%\SuVtitle\Files, or with
//                                          "audiopb" into %LOCALAPPDATA%\AudioPB\Translator files (refuses the real
//                                          LOCALAPPDATA). The cue engine: this build's (cur), the one before (old), no
//                                          ENGINE line (none = 1.0.6), or a number; "1" = EDITED by the user
//   real_tools fg                          the foreground window: hwnd, pid, class
//   real_tools wins <pid>                  that process's top-level windows (class, title, rect, visible,
//                                          WS_EX_NOACTIVATE, caption / sysmenu / min-max boxes, WS_EX_APPWINDOW, owner,
//                                          WS_EX_TOPMOST)
//   real_tools run <command line>          CreateProcessW(NULL, <command line>) -- exactly what Explorer does with a
//                                          registered "shell\open\command" -- waits <= 30 s
//   real_tools close <class> <pid>         WM_CLOSE to the window of that EXACT class if it belongs to <pid> (never
//                                          another process's), waits for the exit
//   real_tools post-close <class> <pid>    WM_CLOSE without waiting;  place <class> <pid> x y w h: moved, NOT activated
//   real_tools ident <path>                size + write time (the player.txt / resume lines)
//   real_tools kids <pid>                  that process's child processes (pid, exe)
//   real_tools alive <pid>                 is that process still running
//   real_tools idle                        ms since the user's last keyboard / mouse input + the foreground-lock timeout
//   real_tools savedlg <pid> <path> [ms]   (LIVE, 25/10) that process's common Save dialog: the file name set to <path>, OK clicked,
//                                          by messages only (no focus); waits <= ms (default 8000) for the dialog to appear
//   a copy named canary.exe, started at all: writes canary_ran.txt next to itself (the refusal test hands its path to
//                                          --play: it must never run)
// Every answer is one JSON line on stdout (non-ASCII as \uXXXX).
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../../app/TrFileStore.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tp = pbt::trpipe;

static std::string js(const std::wstring& w) {
    std::string o = "\"";
    for (wchar_t c : w) {
        if (c == L'"' || c == L'\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c >= 0x20 && c < 0x7f) o += static_cast<char>(c);
        else { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c)); o += b; }
    }
    return o + "\"";
}

// engine: "cur" (this build's kFileEngineVersion, the default), "old" (the one before), "none" (no ENGINE line at all:
// what 1.0.6 wrote) or a number; edited: "1" = the user edited it (EDIT 1); audiopb: AudioPB's folder, not SuVtitle's
static int mkrecord(const std::wstring& media, const std::string& a, const std::string& b,
                    const std::string& engine, bool edited, bool audiopb) {
    // never the user's real cache: the test profile's LOCALAPPDATA only
    PWSTR real = nullptr;
    std::wstring realLa;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &real)) && real) realLa = real;
    if (real) ::CoTaskMemFree(real);
    wchar_t la[MAX_PATH] = {};
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (!la[0] || pbtrf::lowerW(la) == pbtrf::lowerW(realLa)) {
        std::printf("{\"ok\":false,\"err\":\"LOCALAPPDATA is the real one: refused\"}\n");
        return 3;
    }
    uint64_t size = 0, mtime = 0;
    if (!pbtrf::fileIdentity(media, size, mtime)) { std::printf("{\"ok\":false,\"err\":\"no media\"}\n"); return 1; }
    pbtrf::Record r;
    r.path = pbtrf::toUtf8(media);
    r.size = size; r.mtime = mtime;
    r.langA = a; r.langB = b; r.mode = "auto"; r.quality = "auto";
    r.asrModel = "ggml-large-v3-turbo.bin"; r.mtModel = "Qwen3-4B-Instruct-2507-Q5_K_M.gguf";
    r.hasVideo = true; r.totalMs = 10000; r.complete = true;
    r.ranges.push_back(tp::MsRange{ 0, 10000 });
    struct L { uint64_t s, e; const wchar_t* src; const wchar_t* dst; };
    const L lines[] = {   // sample dialogue (invented), laid on the 10 s clip -- like the E2E's FILM
        { 400, 2600, L"Le courant finira par revenir avant la nuit.", L"The power will eventually come back." },
        { 2700, 4800, L"Soit tu restes, soit on te laisse.", L"Either you stay, or we move you." },
        { 5000, 6900, L"C'est quoi, cette ombre dans le noir ?", L"What's that shadow in the dark?" },
        { 7000, 9800, L"On sait tous les deux que ce serait tr\u00e8s compliqu\u00e9.", L"We both know it would be very complicated." },
    };
    uint32_t idx = 0;
    for (const auto& l : lines) {
        tp::WireCue c;
        c.idx = idx++; c.startMs = l.s; c.endMs = l.e; c.lang = a; c.dstLang = b; c.pass = false;
        c.src = pbtrf::toUtf8(l.src); c.dst = pbtrf::toUtf8(l.dst);
        r.cues.push_back(c);
    }
    r.edited = edited;
    r.engine = engine == "old" ? tp::kFileEngineVersion - 1 : (engine == "cur" || engine == "none") ? tp::kFileEngineVersion
             : pbtrf::engineOf(engine);
    std::string text = pbtrf::packRecord(r);
    if (engine == "none") {                     // exactly a record from before the ENGINE line
        const size_t at = text.find("\nENGINE\t");
        if (at != std::string::npos) text.erase(at + 1, text.find('\n', at + 1) - at);
    }
    const std::string key = pbtrf::cacheKey(media, size, mtime, a, b, "auto", "auto");
    std::wstring dir;
    if (audiopb) {
        dir = std::wstring(la) + L"\\AudioPB";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\Translator files";
        if (!pbtrf::ensureDir(dir)) dir.clear();
    } else {
        dir = pbtrf::cacheDir();
    }
    if (dir.empty()) { std::printf("{\"ok\":false,\"err\":\"no cache dir\"}\n"); return 1; }
    const std::wstring path = pbtrf::joinPath(dir, pbtrf::fromUtf8(key) + L".cues");
    const bool ok = pbtrf::writeFileAvSafe(path, text, true);
    std::printf("{\"ok\":%s,\"key\":\"%s\",\"record\":%s,\"size\":%llu,\"mtime\":%llu,\"engine\":%d,\"current\":%d,\"edited\":%s,\"engineLine\":%s}\n",
                ok ? "true" : "false", key.c_str(), js(path).c_str(), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(mtime), engine == "none" ? 0 : r.engine, tp::kFileEngineVersion,
                edited ? "true" : "false", text.find("\nENGINE\t") != std::string::npos ? "true" : "false");
    return ok ? 0 : 1;
}

struct Win { HWND h; std::wstring cls, title; RECT r; bool visible, noact, caption, sysmenu, minbox, maxbox, appwindow, owned, topmost, iconic; };
static BOOL CALLBACK collect(HWND h, LPARAM lp) {
    auto* p = reinterpret_cast<std::pair<DWORD, std::vector<Win>*>*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != p->first) return TRUE;
    Win w{};
    w.h = h;
    wchar_t b[512] = {};
    ::GetClassNameW(h, b, 512); w.cls = b;
    b[0] = 0; ::GetWindowTextW(h, b, 512); w.title = b;
    ::GetWindowRect(h, &w.r);
    w.visible = ::IsWindowVisible(h) != FALSE;
    const LONG_PTR st = ::GetWindowLongPtrW(h, GWL_STYLE), ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE);
    w.noact = (ex & WS_EX_NOACTIVATE) != 0;
    w.caption = (st & WS_CAPTION) == WS_CAPTION;
    w.sysmenu = (st & WS_SYSMENU) != 0;
    w.minbox = (st & WS_MINIMIZEBOX) != 0;
    w.maxbox = (st & WS_MAXIMIZEBOX) != 0;
    w.appwindow = (ex & WS_EX_APPWINDOW) != 0;
    w.owned = ::GetWindow(h, GW_OWNER) != nullptr;
    w.topmost = (ex & WS_EX_TOPMOST) != 0;
    w.iconic = ::IsIconic(h) != FALSE;          // (fix round: a recovery must leave a minimized window minimized, RVB-3)
    p->second->push_back(w);
    return TRUE;
}

// savedlg's search: the file-name Edit (a child of the dialog's ComboBox) and the OK button (id 1) of a common Save dialog
struct FindKids { HWND edit = nullptr, ok = nullptr; };
static BOOL CALLBACK findKids(HWND h, LPARAM lp) {
    auto* k = reinterpret_cast<FindKids*>(lp);
    wchar_t cls[64] = {};
    ::GetClassNameW(h, cls, 64);
    if (!k->edit && !::_wcsicmp(cls, L"Edit")) {
        wchar_t pc[64] = {};
        if (::GetParent(h)) ::GetClassNameW(::GetParent(h), pc, 64);
        if (!::_wcsicmp(pc, L"ComboBox") || !::_wcsicmp(pc, L"ComboBoxEx32")) k->edit = h;
    }
    if (!k->ok && !::_wcsicmp(cls, L"Button") && ::GetDlgCtrlID(h) == IDOK) k->ok = h;
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    {
        const std::wstring self = argv[0];
        if (pbtrf::lowerW(pbtrf::leafOf(self)) == L"canary.exe" || pbtrf::lowerW(pbtrf::leafOf(self)) == L"canary") {
            wchar_t me[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, me, MAX_PATH);
            pbtrf::writeFileAvSafe(pbtrf::joinPath(pbtrf::dirOf(me), L"canary_ran.txt"), pbtrf::toUtf8(::GetCommandLineW()), true);
            return 7;
        }
    }
    if (argc < 2) { std::printf("{\"ok\":false,\"err\":\"usage\"}\n"); return 2; }
    const std::wstring cmd = argv[1];
    if (cmd == L"mkrecord" && argc >= 5) {
        bool edited = false, audiopb = false;
        for (int i = 6; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"1") edited = true;
            if (std::wstring(argv[i]) == L"audiopb") audiopb = true;
        }
        return mkrecord(argv[2], pbtrf::toUtf8(argv[3]), pbtrf::toUtf8(argv[4]), argc >= 6 ? pbtrf::toUtf8(argv[5]) : std::string("cur"),
                        edited, audiopb);
    }
    if (cmd == L"fg") {
        const HWND h = ::GetForegroundWindow();
        DWORD pid = 0;
        if (h) ::GetWindowThreadProcessId(h, &pid);
        wchar_t c[256] = {};
        if (h) ::GetClassNameW(h, c, 256);
        std::printf("{\"ok\":true,\"hwnd\":%llu,\"pid\":%lu,\"cls\":%s}\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)),
                    static_cast<unsigned long>(pid), js(c).c_str());
        return 0;
    }
    // idle: how long the USER has not touched the keyboard / mouse, and Windows' foreground-lock timeout (SPI_GETFOREGROUNDLOCKTIMEOUT):
    // once the user has been idle longer than that, ANY process may take the foreground (an OS Save dialog included) -- a test that
    // asserts "this process never took the foreground" is only meaningful while the user is not idle.
    if (cmd == L"idle") {
        LASTINPUTINFO li{}; li.cbSize = sizeof li;
        DWORD lock = 0;
        ::SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lock, 0);
        const bool got = ::GetLastInputInfo(&li) != FALSE;
        std::printf("{\"ok\":%s,\"idleMs\":%lu,\"lockTimeoutMs\":%lu}\n", got ? "true" : "false",
                    got ? static_cast<unsigned long>(::GetTickCount() - li.dwTime) : 0ul, static_cast<unsigned long>(lock));
        return 0;
    }
    if (cmd == L"wins" && argc >= 3) {
        std::vector<Win> v;
        std::pair<DWORD, std::vector<Win>*> p{ static_cast<DWORD>(_wtoi(argv[2])), &v };
        ::EnumWindows(collect, reinterpret_cast<LPARAM>(&p));
        std::string o = "{\"ok\":true,\"wins\":[";
        for (size_t i = 0; i < v.size(); ++i) {
            char b[512];
            std::snprintf(b, sizeof b, ",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"visible\":%s,\"noactivate\":%s,\"caption\":%s,"
                          "\"sysmenu\":%s,\"minbox\":%s,\"maxbox\":%s,\"appwindow\":%s,\"owned\":%s,\"topmost\":%s,\"iconic\":%s}",
                          v[i].r.left, v[i].r.top, v[i].r.right - v[i].r.left, v[i].r.bottom - v[i].r.top,
                          v[i].visible ? "true" : "false", v[i].noact ? "true" : "false", v[i].caption ? "true" : "false",
                          v[i].sysmenu ? "true" : "false", v[i].minbox ? "true" : "false", v[i].maxbox ? "true" : "false",
                          v[i].appwindow ? "true" : "false", v[i].owned ? "true" : "false", v[i].topmost ? "true" : "false",
                          v[i].iconic ? "true" : "false");
            o += (i ? "," : "") + std::string("{\"cls\":") + js(v[i].cls) + ",\"title\":" + js(v[i].title) + b;
        }
        std::printf("%s]}\n", o.c_str());
        return 0;
    }
    // savedlg <pid> <full path> [wait ms]: the common Save dialog of that process (class #32770, a top-level window of the pid --
    // the Live window's Export), driven by MESSAGES only: its file-name Edit gets the path (WM_SETTEXT), its OK button (id 1) a
    // BM_CLICK. No activation, no focus, no input injected. Waits up to <wait ms> (default 8000) for the dialog to exist.
    if (cmd == L"savedlg" && argc >= 4) {
        const DWORD want = static_cast<DWORD>(_wtoi(argv[2]));
        const ULONGLONG until = ::GetTickCount64() + (argc >= 5 ? static_cast<ULONGLONG>(_wtoi(argv[4])) : 8000ULL);
        for (;;) {
            std::vector<Win> v;
            std::pair<DWORD, std::vector<Win>*> p{ want, &v };
            ::EnumWindows(collect, reinterpret_cast<LPARAM>(&p));
            for (const auto& w : v) {
                if (w.cls != L"#32770" || !w.visible) continue;
                FindKids k;
                ::EnumChildWindows(w.h, findKids, reinterpret_cast<LPARAM>(&k));
                if (!k.edit || !k.ok) continue;
                const std::wstring path = argv[3];
                const LRESULT set = ::SendMessageW(k.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()));
                wchar_t got[1024] = {};
                ::SendMessageW(k.edit, WM_GETTEXT, 1024, reinterpret_cast<LPARAM>(got));
                // who has the foreground at this moment (before the click closes the dialog): the dialog itself, or someone else
                DWORD fgPid = 0;
                wchar_t fgCls[128] = {};
                if (const HWND fg = ::GetForegroundWindow()) { ::GetWindowThreadProcessId(fg, &fgPid); ::GetClassNameW(fg, fgCls, 128); }
                const BOOL clicked = (set && path == got) ? ::PostMessageW(k.ok, BM_CLICK, 0, 0) : FALSE;
                std::printf("{\"ok\":%s,\"title\":%s,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"text\":%s,\"fgpid\":%lu,\"fgcls\":%s}\n", clicked ? "true" : "false",
                            js(w.title).c_str(), w.r.left, w.r.top, w.r.right - w.r.left, w.r.bottom - w.r.top, js(got).c_str(),
                            static_cast<unsigned long>(fgPid), js(fgCls).c_str());
                return clicked ? 0 : 1;
            }
            if (::GetTickCount64() > until) { std::printf("{\"ok\":false,\"err\":\"no Save dialog for that pid\"}\n"); return 1; }
            ::Sleep(100);
        }
    }
    if (cmd == L"run" && argc >= 3) {
        std::wstring line = argv[2];
        std::vector<wchar_t> buf(line.begin(), line.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            std::printf("{\"ok\":false,\"err\":\"CreateProcess %lu\"}\n", ::GetLastError());
            return 1;
        }
        const DWORD w = ::WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 259;
        ::GetExitCodeProcess(pi.hProcess, &code);
        std::printf("{\"ok\":true,\"pid\":%lu,\"exited\":%s,\"exit\":%lu}\n", pi.dwProcessId, w == WAIT_OBJECT_0 ? "true" : "false", code);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return 0;
    }
    // minimize | restore <class> <pid> (fix round 26/10): ShowWindowAsync SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE from THIS process --
    // never an activation (the user's foreground is not ours): the RVB-3 check minimizes the Live window before a browser crash
    if ((cmd == L"minimize" || cmd == L"restore") && argc >= 4) {
        const DWORD want = static_cast<DWORD>(_wtoi(argv[3]));
        std::vector<Win> v;
        std::pair<DWORD, std::vector<Win>*> p{ want, &v };
        ::EnumWindows(collect, reinterpret_cast<LPARAM>(&p));
        HWND h = nullptr;
        for (const auto& w : v) if (w.cls == argv[2]) { h = w.h; break; }
        if (!h || !want) { std::printf("{\"ok\":false,\"err\":\"no such window for that pid\"}\n"); return 1; }
        const BOOL ok = ::ShowWindowAsync(h, cmd == L"minimize" ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        ::Sleep(300);
        std::printf("{\"ok\":%s,\"iconic\":%s}\n", ok ? "true" : "false", ::IsIconic(h) ? "true" : "false");
        return 0;
    }
    if ((cmd == L"post-close" && argc >= 4) || (cmd == L"place" && argc >= 8)) {
        const DWORD want = static_cast<DWORD>(_wtoi(argv[3]));
        std::vector<Win> v;
        std::pair<DWORD, std::vector<Win>*> p{ want, &v };
        ::EnumWindows(collect, reinterpret_cast<LPARAM>(&p));
        HWND h = nullptr;
        for (const auto& w : v) if (w.cls == argv[2]) { h = w.h; break; }
        if (!h || !want) { std::printf("{\"ok\":false,\"err\":\"no such window for that pid\"}\n"); return 1; }
        BOOL ok = FALSE;
        if (cmd == L"post-close") ok = ::PostMessageW(h, WM_CLOSE, 0, 0);
        else ok = ::SetWindowPos(h, nullptr, _wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[6]), _wtoi(argv[7]),
                                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        std::printf("{\"ok\":%s}\n", ok ? "true" : "false");
        return ok ? 0 : 1;
    }
    // topmost <class> <pid> 0|1: SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST, no activation) FROM THIS process -- what
    // Windows allows a background process to do (the product's pin runs in a window that is never foreground in a test)
    if (cmd == L"topmost" && argc >= 5) {
        const DWORD want = static_cast<DWORD>(_wtoi(argv[3]));
        std::vector<Win> v;
        std::pair<DWORD, std::vector<Win>*> p{ want, &v };
        ::EnumWindows(collect, reinterpret_cast<LPARAM>(&p));
        HWND h = nullptr;
        for (const auto& w : v) if (w.cls == argv[2]) { h = w.h; break; }
        if (!h || !want) { std::printf("{\"ok\":false,\"err\":\"no such window for that pid\"}\n"); return 1; }
        const BOOL r = ::SetWindowPos(h, _wtoi(argv[4]) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        const DWORD e = ::GetLastError();
        const bool now = (::GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        std::printf("{\"ok\":%s,\"err\":%lu,\"topmost\":%s}\n", r ? "true" : "false", e, now ? "true" : "false");
        return 0;
    }
    if (cmd == L"close" && argc >= 4) {
        const DWORD want = static_cast<DWORD>(_wtoi(argv[3]));
        const HWND h = ::FindWindowW(argv[2], nullptr);
        DWORD pid = 0;
        if (h) ::GetWindowThreadProcessId(h, &pid);
        if (!h || pid != want || !want) { std::printf("{\"ok\":false,\"err\":\"no such window for that pid\",\"pid\":%lu}\n", pid); return 1; }
        HANDLE p = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        ::PostMessageW(h, WM_CLOSE, 0, 0);
        DWORD code = 259, w = WAIT_FAILED;
        if (p) { w = ::WaitForSingleObject(p, 30000); ::GetExitCodeProcess(p, &code); ::CloseHandle(p); }
        std::printf("{\"ok\":true,\"exited\":%s,\"exit\":%lu}\n", w == WAIT_OBJECT_0 ? "true" : "false", code);
        return 0;
    }
    if (cmd == L"ident" && argc >= 3) {
        uint64_t s = 0, m = 0;
        const bool ok = pbtrf::fileIdentity(argv[2], s, m);
        std::printf("{\"ok\":%s,\"size\":%llu,\"mtime\":%llu}\n", ok ? "true" : "false", static_cast<unsigned long long>(s),
                    static_cast<unsigned long long>(m));
        return ok ? 0 : 1;
    }
    if (cmd == L"kids" && argc >= 3) {
        const DWORD parent = static_cast<DWORD>(_wtoi(argv[2]));
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        std::string o = "{\"ok\":true,\"kids\":[";
        bool first = true;
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
            for (BOOL more = ::Process32FirstW(snap, &pe); more; more = ::Process32NextW(snap, &pe)) {
                if (pe.th32ParentProcessID != parent || !parent) continue;
                char b[64];
                std::snprintf(b, sizeof b, "{\"pid\":%lu,\"exe\":", static_cast<unsigned long>(pe.th32ProcessID));
                o += (first ? "" : ",") + std::string(b) + js(pe.szExeFile) + "}";
                first = false;
            }
            ::CloseHandle(snap);
        }
        std::printf("%s]}\n", o.c_str());
        return 0;
    }
    if (cmd == L"alive" && argc >= 3) {
        HANDLE p = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(argv[2])));
        const bool alive = p && ::WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) ::CloseHandle(p);
        std::printf("{\"ok\":true,\"alive\":%s}\n", alive ? "true" : "false");
        return 0;
    }
    std::printf("{\"ok\":false,\"err\":\"usage\"}\n");
    return 2;
}

// =============================================================================
// SuVtitle - app/TrLaunch.h   (the command line: --play, --test-instance, --register-player, WM_COPYDATA)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TrLaunch.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: --register-player (new: registers and quits, like
// --unregister-player), --tray dropped (SuVtitle has no tray), the WM_COPYDATA tag "SVTP".
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md D-L1 / section 4.3): `--live` -- the "SuVtitle Live" shortcut --
// opens the Live window ONLY (the player stays hidden: the taskbar shows the Live window alone); a SuVtitle already
// running gets it through WM_COPYDATA (kCopyLive, an EMPTY payload: there is nothing to carry, so anything else is
// refused) and opens or brings up its Live window, the same way --play hands over its paths.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// AudioPB AS A WINDOWS PLAYER (the user, 11/10: "je veux la possibilite de choisir le lecteur d'AudioPB comme
// lecteur par defaut pour lire les videos dans Windows"). What that takes on the command line, header-only and pure
// enough to be proved by scratchpad/ui_test/wg_smoke.cpp:
//
//   * `AudioPBConfig.exe --play "<file|folder>" [...]` -- what Explorer runs for a double-click once AudioPB is
//     the player (TrAssoc.h writes that command). A tray already running gets the paths through WM_COPYDATA (one
//     AudioPB, ever: the single-instance rule) and opens its player with them; otherwise this process becomes the
//     tray, hidden, and opens the player itself.
//   * The paths are the SHELL's -- legitimate -- but still checked: absolute, existing, a FOLDER (its media are
//     queued), a MEDIA file (TrFileStore.h's list) or a SUBTITLE file (for what plays); nothing else, and nothing
//     is ever executed or handed to a shell (no command line is ever built from them).
//   * `--unregister-player` removes what the app registered for the user (the uninstaller runs it).
//   * `--test-instance <tag>` (TESTS ONLY): an AudioPBConfig that can run NEXT TO the user's own tray without
//     touching anything of his -- it refuses to start unless %APPDATA% and %LOCALAPPDATA% point AWAY from the real
//     folders (the test gives it a scratch profile), and its single-instance name, window class and registry root
//     are its own. bridge.cpp / main.cpp read isTestInstance() to also stay off the fabric, the VST3 rescan, the
//     licence, the tray icon and the foreground.
//
// ASCII only (the build has no /utf-8).
// =============================================================================
#pragma once

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TrFileStore.h"   // pbtrf::isMediaExt, extOf, leafOf

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace pblaunch {

// ---- the command line ------------------------------------------------------------------------------------------
struct Args {
    bool play = false;                  // --play was given (even with no path: "open the player")
    std::vector<std::wstring> paths;    // what follows --play, up to the next switch
    std::wstring testTag;               // --test-instance <tag>
    bool badTag = false;                // --test-instance with a tag that is not [a-z0-9-]{1,24}
    bool unregister = false;            // --unregister-player
    bool registerPlayer = false;        // --register-player (SuVtitle: registers for this user, then quits)
    bool live = false;                  // --live (SuVtitle 1.1.0): the Live window, alone
};
inline bool tagOk(const std::wstring& t) {
    if (t.empty() || t.size() > 24) return false;
    for (wchar_t c : t) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    return true;
}
inline Args parseArgs(const std::vector<std::wstring>& argv) {   // argv[0] = the exe
    Args a;
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::wstring& x = argv[i];
        if (x == L"--play") {
            a.play = true;
            while (i + 1 < argv.size() && argv[i + 1].rfind(L"--", 0) != 0) a.paths.push_back(argv[++i]);
        } else if (x == L"--test-instance") {
            if (i + 1 < argv.size()) { a.testTag = argv[++i]; if (!tagOk(a.testTag)) { a.badTag = true; a.testTag.clear(); } }
            else a.badTag = true;
        } else if (x == L"--unregister-player") {
            a.unregister = true;
        } else if (x == L"--register-player") {
            a.registerPlayer = true;
        } else if (x == L"--live") {
            a.live = true;
        }
    }
    return a;
}
inline std::vector<std::wstring> splitCommandLine(const wchar_t* cmd) {
    std::vector<std::wstring> v;
    int n = 0;
    LPWSTR* av = ::CommandLineToArgvW(cmd && *cmd ? cmd : L"x", &n);
    if (!av) return v;
    for (int i = 0; i < n; ++i) v.push_back(av[i]);
    ::LocalFree(av);
    return v;
}
inline const Args& processArgs() {
    static const Args a = parseArgs(splitCommandLine(::GetCommandLineW()));
    return a;
}

// ---- the TEST INSTANCE ---------------------------------------------------------------------------------------------
inline std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) s = p;
    if (p) ::CoTaskMemFree(p);
    return s;
}
inline std::wstring envOf(const wchar_t* name) {
    wchar_t b[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, b, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(b, n) : std::wstring();
}
// Two folders are the same when they are the same FILE -- volume and file id -- so a trailing slash, an 8.3 name, a
// "\\?\" prefix, a junction or a symlink to the real profile are all caught (review of WG #16: the check compared
// strings). A folder that cannot be opened counts as the same: no isolation is proven, the test instance refuses.
inline bool sameDir(const std::wstring& a, const std::wstring& b) {
    auto idOf = [](const std::wstring& p, BY_HANDLE_FILE_INFORMATION& fi) {
        if (p.empty()) return false;
        HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const BOOL ok = ::GetFileInformationByHandle(h, &fi);
        ::CloseHandle(h);
        return ok != FALSE;
    };
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    if (!idOf(a, x) || !idOf(b, y)) return true;
    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh
        && x.nFileIndexLow == y.nFileIndexLow;
}
// True when this process is a test instance: --test-instance <tag> AND a profile isolated from the user's. A test
// instance whose %APPDATA% or %LOCALAPPDATA% is the real one EXITS here, at its first question (static init: before
// the Bridge reads or writes anything) -- with code 3 and nothing done.
inline bool isTestInstance() {
    static const bool v = [] {
        const Args& a = processArgs();
        if (a.badTag) ::ExitProcess(3);
        if (a.testTag.empty()) return false;
        const std::wstring ad = envOf(L"APPDATA"), la = envOf(L"LOCALAPPDATA");
        const std::wstring rad = knownFolder(FOLDERID_RoamingAppData), rla = knownFolder(FOLDERID_LocalAppData);
        auto same = [](const std::wstring& x, const std::wstring& y) { return pbtrf::lowerW(x) == pbtrf::lowerW(y); };
        if (ad.empty() || la.empty() || rad.empty() || rla.empty() || same(ad, rad) || same(la, rla)
            || sameDir(ad, rad) || sameDir(la, rla)) ::ExitProcess(3);
        return true;
    }();
    return v;
}
inline const std::wstring& testTag() { static const std::wstring t = isTestInstance() ? processArgs().testTag : std::wstring(); return t; }
// The single-instance names: the product's, or the test instance's own.
inline std::wstring suffixed(const wchar_t* base) { return isTestInstance() ? std::wstring(base) + L"." + testTag() : std::wstring(base); }

// ---- the paths -----------------------------------------------------------------------------------------------------
inline bool isSubtitleExtL(const std::wstring& e) {   // TWIN of SubtitleReader.h isSubtitleExt (the harness compares)
    const std::wstring x = pbtrf::lowerW(e);
    return x == L"srt" || x == L"vtt" || x == L"ass" || x == L"ssa";
}
enum class Kind { Media, Subtitle, Folder, Refused };
// What one path given to play is. Refused: empty, too long, relative, a wildcard or a character no file name holds,
// missing, or neither a folder nor a media / subtitle file.
inline Kind classify(const std::wstring& p) {
    if (p.empty() || p.size() > 32767) return Kind::Refused;
    for (wchar_t c : p) if (c < 0x20 || c == L'*' || c == L'?' || c == L'<' || c == L'>' || c == L'|' || c == L'"') return Kind::Refused;
    const bool drive = p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
    const bool unc = p.size() >= 3 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.';
    if (!drive && !unc) return Kind::Refused;
    const DWORD at = ::GetFileAttributesW(p.c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return Kind::Refused;
    if (at & FILE_ATTRIBUTE_DIRECTORY) return Kind::Folder;
    const std::wstring ext = pbtrf::extOf(pbtrf::leafOf(p));
    if (pbtrf::isMediaExt(ext)) return Kind::Media;
    if (isSubtitleExtL(ext)) return Kind::Subtitle;
    return Kind::Refused;
}

// ---- WM_COPYDATA: the paths, from the process Explorer started to the tray already running ---------------------------
constexpr ULONG_PTR kCopyPlay = 0x50545653;      // "SVTP": SuVtitle, Play (AudioPB's was "ABPP")
constexpr size_t    kCopyMaxBytes = 1u << 20;    // 1 MB of UTF-16
constexpr size_t    kCopyMaxPaths = 2000;        // the queue's own cap
// UTF-16 strings, each NUL-terminated, the list closed by an EMPTY one (a double NUL). Paths beyond the caps are
// dropped (never a truncated path).
inline std::vector<wchar_t> packPlay(const std::vector<std::wstring>& paths) {
    std::vector<wchar_t> v;
    size_t n = 0;
    for (const auto& p : paths) {
        if (p.empty() || p.size() > 32767 || n >= kCopyMaxPaths) continue;
        if ((v.size() + p.size() + 2) * sizeof(wchar_t) > kCopyMaxBytes) break;
        v.insert(v.end(), p.begin(), p.end());
        v.push_back(L'\0');
        ++n;
    }
    v.push_back(L'\0');
    return v;
}
// The tray's side: anything malformed is refused WHOLE (a size that is not whole UTF-16, too big, no closing
// double NUL, an empty path inside, too many).
inline bool unpackPlay(const void* data, size_t bytes, std::vector<std::wstring>& out) {
    out.clear();
    if (!data || bytes < sizeof(wchar_t) || bytes > kCopyMaxBytes || bytes % sizeof(wchar_t)) return false;
    const wchar_t* w = static_cast<const wchar_t*>(data);
    const size_t n = bytes / sizeof(wchar_t);
    if (w[n - 1] != L'\0') return false;
    size_t at = 0;
    while (at < n) {
        size_t e = at;
        while (e < n && w[e] != L'\0') ++e;
        if (e == n) return false;
        if (e == at) return e == n - 1;                       // the closing empty string: the end, exactly
        if (e - at > 32767 || out.size() >= kCopyMaxPaths) return false;
        out.emplace_back(w + at, e - at);
        at = e + 1;
    }
    return false;                                             // no closing empty string
}

// ---- SUVTITLE LIVE (1.1.0): --live, from the process the shortcut started to the SuVtitle already running -----------
// The request IS the tag: "open the Live window" carries nothing, so the payload must be EMPTY -- any byte is refused
// (a message whose content would be ignored is a message nobody should be able to send with a content).
constexpr ULONG_PTR kCopyLive = 0x4C545653;      // "SVTL": SuVtitle, Live
inline bool isLiveCopy(ULONG_PTR tag, const void* data, size_t bytes) {
    (void)data;
    return tag == kCopyLive && bytes == 0;
}

}   // namespace pblaunch

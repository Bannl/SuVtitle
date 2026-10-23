// =============================================================================
// SuVtitle - app/AppConfig.h   (NEW, S-HOST 2026-10-23: the app's own config file, %APPDATA%\SuVtitle\app.txt)
//
// SPLIT_PLAN S11: the window's geometry was the WINPOS line AudioPB's tray kept in translator.txt -- a file it shared
// with the helper, hence the read-modify-write dance and the "an empty read is not an empty file" guard in 641f1aa's
// main.cpp saveTranslatorWinPos. SuVtitle gives the app a file of its own: app.txt, written whole by the app only (the
// engine has engine.txt), through pbtrf::writeFileAvSafe (temp + rename with the antivirus retry).
//
//   # SuVtitle app v1
//   WINPOS <x> <y> <w> <h> <topmost 0|1> [<maximized 0|1>]      (tab-separated; the RESTORED rectangle, physical px)
//
// The line keeps AudioPB's five first fields in the same order (a reader that stops at topmost still reads it right);
// `maximized` is appended (new: AudioPB's translator window reopened restored even when it was closed maximized).
// Unknown lines are kept on a rewrite, so a later field never costs an older one.
// Also here, because it is pure and security-relevant: webRelPath, the rule that maps a request of the page's origin
// to a file under web\ (main.cpp serveFreshWebAsset). Pure: tests/cpp/app_smoke.cpp. ASCII only (no /utf-8).
// =============================================================================
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace pbapp {

struct WinPos {
    int  x = 0, y = 0, w = 0, h = 0;
    bool topmost = false;
    bool maximized = false;
};

inline const char* appHeader() { return "# SuVtitle app v1"; }

inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string l = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

// False when there is no usable WINPOS (none, or a size no window could be: AudioPB's own floor, 200 x 150).
inline bool parseWinPos(const std::string& text, WinPos& p) {
    for (const auto& l : lines(text)) {
        if (l.rfind("WINPOS\t", 0) != 0) continue;
        int x = 0, y = 0, w = 0, h = 0, tm = 0, mx = 0;
        const int n = ::sscanf_s(l.c_str() + 7, "%d\t%d\t%d\t%d\t%d\t%d", &x, &y, &w, &h, &tm, &mx);   // (_s: no C4996)
        if (n >= 4 && w > 200 && h > 150 && w < 100000 && h < 100000) {
            p.x = x; p.y = y; p.w = w; p.h = h;
            p.topmost = n >= 5 && tm != 0;
            p.maximized = n >= 6 && mx != 0;
            return true;
        }
    }
    return false;
}

// THE PAGE'S FILES (main.cpp serveFreshWebAsset): a request path under https://suvtitle.local/ -> a path RELATIVE to
// web\ ("" = refused: the handler declines and the request fails). AudioPB's handler kept the LAST segment only (its
// web\ was flat); SuVtitle's page may have sub-folders (S-WEB's call), so every segment is checked instead: letters,
// digits, '.', '-', '_' and spaces, 1-128 characters, never "." or ".." -- so no request can climb out of web\, name a
// drive, a device path, an alternate stream (':'), or reach anything but a file under web\. The query and the fragment
// are dropped; "" (the origin itself) is index.html.
inline std::wstring webRelPath(std::wstring path) {
    const size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path = path.substr(0, q);
    if (path.empty()) path = L"index.html";
    std::wstring rel;
    size_t at = 0;
    for (;;) {
        const size_t sl = path.find(L'/', at);
        const std::wstring seg = path.substr(at, sl == std::wstring::npos ? std::wstring::npos : sl - at);
        if (seg.empty() || seg == L"." || seg == L".." || seg.size() > 128) return std::wstring();
        // Win32 drops a trailing dot or space ("x." opens "x"): refused rather than reinterpreted
        if (seg.back() == L'.' || seg.back() == L' ') return std::wstring();
        for (wchar_t c : seg)
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
                  || c == L'.' || c == L'-' || c == L'_' || c == L' ')) return std::wstring();
        // a DOS device name in any folder is the device ("web\COM1", "web\NUL.txt"): never a file of ours
        {
            std::wstring b = seg.substr(0, seg.find(L'.'));
            for (auto& c : b) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
            while (!b.empty() && b.back() == L' ') b.pop_back();
            if (b == L"CON" || b == L"PRN" || b == L"AUX" || b == L"NUL" || b == L"CONIN$" || b == L"CONOUT$"
                || ((b.size() == 4) && (b.compare(0, 3, L"COM") == 0 || b.compare(0, 3, L"LPT") == 0)
                    && b[3] >= L'0' && b[3] <= L'9'))
                return std::wstring();
        }
        if (!rel.empty()) rel += L"\\";
        rel += seg;
        if (sl == std::wstring::npos) break;
        at = sl + 1;
    }
    return rel;
}

// The whole file: the header, the WINPOS line, and every other line `old` held (kept, in order; one header).
inline std::string packWinPos(const std::string& old, const WinPos& p) {
    char line[160];
    std::snprintf(line, sizeof(line), "WINPOS\t%d\t%d\t%d\t%d\t%d\t%d", p.x, p.y, p.w, p.h, p.topmost ? 1 : 0,
                  p.maximized ? 1 : 0);
    std::string body = std::string(appHeader()) + "\n" + line + "\n";
    for (const auto& l : lines(old)) {
        if (l[0] == '#' || l.rfind("WINPOS\t", 0) == 0) continue;
        body += l + "\n";
    }
    return body;
}

}   // namespace pbapp

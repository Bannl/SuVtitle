// =============================================================================
// SuVtitle tests/cpp/mpv/native_tool.cpp -- N1 (26/10/2026): the real-app checks' Win32 helper for the NATIVE video engine
// (native_real.js). Ported from the N0 prototype's n0tool.cpp (scratchpad\n0\src\n0tool.cpp) for the product's window:
// SuVtitle.MainWindow.<tag> of a TEST INSTANCE, its picture window SuVtitle.Video, mpv's own window inside it, the view's
// input window. It NEVER moves or clicks the real mouse (no SendInput, no SetCursorPos) and never activates a window.
//
//   native_tool info  <pid> <tag>                        JSON: the main window (rect, client, monitor, dpi, full-screen-ness,
//                                                        foreground), the picture window (client rect, visible, DISABLED),
//                                                        mpv's window inside it (its thread vs the main window's), the view's
//                                                        input window, the real cursor (where, shown, on that monitor)
//   native_tool cap   <pid> <tag> <out.png> [x y w h]... PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) of the client,
//                                                        saved; per region (client px): mean, sd, "colourful" (saturated
//                                                        pixels), black and magenta fractions
//   native_tool post  <pid> <tag> <what> <x> <y> [delta] [ctrl]   a mouse message POSTED to the main window, client px:
//                                                        move|ldown|lup|ldbl|rdown|rup|wheel|leave (wheel: screen px inside)
//   native_tool key   <pid> <tag> <vk-hex>                WM_KEYDOWN + WM_KEYUP posted to the view's input window
//   native_tool drop  <pid> <tag> <path>                 WM_COPYDATA 'SVDT' (a TEST INSTANCE's drop) -- the app refuses it
//                                                        unless it is a test instance
//   native_tool hit   <pid> <tag> <x> <y>                WindowFromPoint at that client point: whose window takes a real
//                                                        mouse there (ours / a child of ours / another program's)
//   native_tool close <pid> <tag>                        WM_CLOSE posted to the main window
//   native_tool uia   <pid> <tag> <name>                 UI Automation from the main window: is an element of that NAME (the
//                                                        page's Play button) in its tree -- what a screen reader sees
// Every answer is one JSON line on stdout. ASCII source, /W4, like the product.
// =============================================================================
#include <windows.h>
#include <ole2.h>                 // (before uiautomation.h: WIN32_LEAN_AND_MEAN leaves the COM interface macros out)
#include <uiautomation.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Found { DWORD pid; std::wstring cls; HWND h; };
BOOL CALLBACK findProc(HWND h, LPARAM lp) {
    Found* f = reinterpret_cast<Found*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid) return TRUE;
    wchar_t cls[128] = {};
    ::GetClassNameW(h, cls, 128);
    if (f->cls != cls) return TRUE;
    f->h = h;
    return FALSE;
}
HWND findMain(DWORD pid, const std::wstring& tag) {
    Found f{pid, L"SuVtitle.MainWindow." + tag, nullptr};
    ::EnumWindows(findProc, reinterpret_cast<LPARAM>(&f));
    return f.h;
}
struct Kid { const wchar_t* prefix; HWND h; };
BOOL CALLBACK kidProc(HWND c, LPARAM lp) {
    Kid* k = reinterpret_cast<Kid*>(lp);
    wchar_t cls[128] = {};
    ::GetClassNameW(c, cls, 128);
    if (std::wcsncmp(cls, k->prefix, std::wcslen(k->prefix)) == 0) { k->h = c; return FALSE; }
    return TRUE;
}
HWND findKid(HWND main, const wchar_t* prefix) {
    Kid k{prefix, nullptr};
    ::EnumChildWindows(main, kidProc, reinterpret_cast<LPARAM>(&k));
    return k.h;
}
std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s += (*w < 128 && *w >= 32 && *w != L'"' && *w != L'\\') ? static_cast<char>(*w) : '?';
    return s;
}
RECT clientRectOf(HWND child, HWND main) {
    RECT r{};
    ::GetWindowRect(child, &r);
    ::MapWindowPoints(nullptr, main, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

bool savePng(const wchar_t* path, const std::vector<unsigned char>& bgra, int w, int h) {
    std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        bgr[i * 3 + 0] = bgra[i * 4 + 0];
        bgr[i * 3 + 1] = bgra[i * 4 + 1];
        bgr[i * 3 + 2] = bgra[i * 4 + 2];
    }
    ComPtr<IWICImagingFactory> f;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> s;
    if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> e;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) || FAILED(e->Initialize(s.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(e->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) ||
        FAILED(fr->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))))
        return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(fr->SetPixelFormat(&pf)) || pf != GUID_WICPixelFormat24bppBGR) return false;
    if (FAILED(fr->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w) * 3, static_cast<UINT>(bgr.size()), bgr.data()))) return false;
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
}

// the statistics of one client rectangle of a BGRA capture
std::string regionStats(const std::vector<unsigned char>& px, int w, int h, int x0, int y0, int rw, int rh) {
    const int x1 = std::min(w, x0 + rw), y1 = std::min(h, y0 + rh);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    double sum = 0, sum2 = 0;
    long long n = 0, colourful = 0, black = 0, magenta = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            const int b = p[0], g = p[1], r = p[2];
            const double l = 0.114 * b + 0.587 * g + 0.299 * r;
            sum += l;
            sum2 += l * l;
            ++n;
            const int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
            if (mx > 80 && (mx - mn) > mx / 2) ++colourful;           // saturated and not dark: a colour bar, not UI grey
            if (mx < 16) ++black;
            if (r >= 240 && g <= 16 && b >= 240) ++magenta;
        }
    const double mean = n ? sum / n : 0, sd = n ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 0;
    char b[300];
    std::snprintf(b, sizeof b, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"n\":%lld,\"mean\":%.1f,\"sd\":%.1f,\"colourful\":%.4f,"
                  "\"black\":%.4f,\"magenta\":%.4f}", x0, y0, x1 - x0, y1 - y0, n, mean, sd,
                  n ? double(colourful) / n : 0.0, n ? double(black) / n : 0.0, n ? double(magenta) / n : 0.0);
    return b;
}

int capture(HWND h, const wchar_t* out, const std::vector<int>& regions) {
    RECT cr{};
    ::GetClientRect(h, &cr);
    const int w = cr.right, ht = cr.bottom;
    if (w <= 0 || ht <= 0) { std::printf("{\"ok\":false,\"error\":\"empty client\"}\n"); return 1; }
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    const BOOL ok = ::PrintWindow(h, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    ::GdiFlush();
    std::vector<unsigned char> px(static_cast<size_t>(w) * ht * 4);
    std::memcpy(px.data(), bits, px.size());
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    const bool saved = savePng(out, px, w, ht);
    std::string regs;
    for (size_t i = 0; i + 3 < regions.size(); i += 4)
        regs += (regs.empty() ? "" : ",") + regionStats(px, w, ht, regions[i], regions[i + 1], regions[i + 2], regions[i + 3]);
    std::printf("{\"ok\":%s,\"saved\":%s,\"w\":%d,\"h\":%d,\"regions\":[%s]}\n", ok ? "true" : "false", saved ? "true" : "false",
                w, ht, regs.c_str());
    return ok ? 0 : 1;
}

int info(HWND h) {
    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    POINT o{0, 0};
    ::ClientToScreen(h, &o);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    const bool fs = ::EqualRect(&wr, &mi.rcMonitor) != FALSE;
    const DWORD mainTid = ::GetWindowThreadProcessId(h, nullptr);
    HWND video = findKid(h, L"SuVtitle.Video");
    HWND mpv = video ? ::GetWindow(video, GW_CHILD) : nullptr;
    HWND input = findKid(h, L"Chrome_WidgetWin");
    std::string v = "null", m = "null", in = "null";
    if (video) {
        const RECT r = clientRectOf(video, h);
        const LONG_PTR st = ::GetWindowLongPtrW(video, GWL_STYLE);
        char b[300];
        std::snprintf(b, sizeof b, "{\"rect\":[%ld,%ld,%ld,%ld],\"visible\":%s,\"disabled\":%s}", r.left, r.top, r.right, r.bottom,
                      ::IsWindowVisible(video) ? "true" : "false", (st & WS_DISABLED) ? "true" : "false");
        v = b;
    }
    if (mpv) {
        wchar_t cls[128] = {};
        ::GetClassNameW(mpv, cls, 128);
        const RECT r = clientRectOf(mpv, h);
        char b[300];
        std::snprintf(b, sizeof b, "{\"class\":\"%s\",\"rect\":[%ld,%ld,%ld,%ld],\"otherThread\":%s}", narrow(cls).c_str(), r.left,
                      r.top, r.right, r.bottom, ::GetWindowThreadProcessId(mpv, nullptr) != mainTid ? "true" : "false");
        m = b;
    }
    if (input) {
        const RECT r = clientRectOf(input, h);
        char b[200];
        std::snprintf(b, sizeof b, "{\"hwnd\":\"%p\",\"rect\":[%ld,%ld,%ld,%ld]}", static_cast<void*>(input), r.left, r.top, r.right, r.bottom);
        in = b;
    }
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    ::GetCursorInfo(&ci);
    const bool curOnMon = ::PtInRect(&mi.rcMonitor, ci.ptScreenPos) != FALSE;
    std::printf("{\"ok\":true,\"hwnd\":\"%p\",\"windowRect\":[%ld,%ld,%ld,%ld],\"clientOrigin\":[%ld,%ld],\"client\":[%ld,%ld],"
                "\"monitor\":[%ld,%ld,%ld,%ld],\"fullscreenRect\":%s,\"dpi\":%u,\"foreground\":%s,\"style\":\"0x%llx\","
                "\"exStyle\":\"0x%llx\",\"video\":%s,\"mpv\":%s,\"input\":%s,\"cursor\":{\"at\":[%ld,%ld],\"showing\":%s,"
                "\"onWindowMonitor\":%s}}\n",
                static_cast<void*>(h), wr.left, wr.top, wr.right, wr.bottom, o.x, o.y, cr.right, cr.bottom, mi.rcMonitor.left,
                mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, fs ? "true" : "false", ::GetDpiForWindow(h),
                ::GetForegroundWindow() == h ? "true" : "false",
                static_cast<unsigned long long>(::GetWindowLongPtrW(h, GWL_STYLE)),
                static_cast<unsigned long long>(::GetWindowLongPtrW(h, GWL_EXSTYLE)), v.c_str(), m.c_str(), in.c_str(),
                ci.ptScreenPos.x, ci.ptScreenPos.y, (ci.flags & CURSOR_SHOWING) ? "true" : "false", curOnMon ? "true" : "false");
    return 0;
}

int post(HWND h, const std::wstring& what, int x, int y, int d, bool ctrl) {
    UINT msg = 0;
    WPARAM wp = ctrl ? MK_CONTROL : 0;
    LPARAM lp = MAKELPARAM(x, y);
    if (what == L"move") msg = WM_MOUSEMOVE;
    else if (what == L"ldown") { msg = WM_LBUTTONDOWN; wp |= MK_LBUTTON; }
    else if (what == L"lup") msg = WM_LBUTTONUP;
    else if (what == L"ldbl") { msg = WM_LBUTTONDBLCLK; wp |= MK_LBUTTON; }
    else if (what == L"rdown") { msg = WM_RBUTTONDOWN; wp |= MK_RBUTTON; }
    else if (what == L"rup") msg = WM_RBUTTONUP;
    else if (what == L"leave") { msg = WM_MOUSELEAVE; lp = 0; }
    else if (what == L"wheel") {
        msg = WM_MOUSEWHEEL;
        POINT sp{x, y};
        ::ClientToScreen(h, &sp);                         // wheel messages carry SCREEN coordinates
        wp = MAKEWPARAM(ctrl ? MK_CONTROL : 0, static_cast<WORD>(static_cast<short>(d ? d : 120)));
        lp = MAKELPARAM(sp.x, sp.y);
    } else {
        std::printf("{\"ok\":false,\"error\":\"unknown input\"}\n");
        return 2;
    }
    const BOOL ok = ::PostMessageW(h, msg, wp, lp);
    std::printf("{\"ok\":%s,\"msg\":\"0x%04x\",\"x\":%d,\"y\":%d}\n", ok ? "true" : "false", msg, x, y);
    return ok ? 0 : 1;
}

int uia(HWND h, const wchar_t* name) {
    ComPtr<IUIAutomation> a;
    ComPtr<IUIAutomationElement> root, found;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&a))) || !a
        || FAILED(a->ElementFromHandle(h, &root)) || !root) {
        std::printf("{\"ok\":false,\"error\":\"no UI Automation element for the window\"}\n");
        return 1;
    }
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = ::SysAllocString(name);
    a->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
    ::VariantClear(&v);
    const HRESULT hr = cond ? root->FindFirst(TreeScope_Descendants, cond.Get(), &found) : E_FAIL;
    std::string ctl = "none";
    if (found) {
        CONTROLTYPEID t = 0;
        found->get_CurrentControlType(&t);
        ctl = t == UIA_ButtonControlTypeId ? "button" : std::to_string(t);
    }
    std::printf("{\"ok\":true,\"found\":%s,\"controlType\":\"%s\",\"hr\":\"0x%08lx\"}\n", found ? "true" : "false", ctl.c_str(),
                static_cast<unsigned long>(hr));
    return 0;
}

int hit(HWND h, int x, int y) {
    POINT sp{x, y};
    ::ClientToScreen(h, &sp);
    HWND w = ::WindowFromPoint(sp);
    wchar_t cls[128] = {};
    if (w) ::GetClassNameW(w, cls, 128);
    HWND root = w ? ::GetAncestor(w, GA_ROOT) : nullptr;
    const char* who = w == h ? "main" : (w && ::IsChild(h, w)) ? "child" : "other";
    std::printf("{\"ok\":true,\"class\":\"%s\",\"who\":\"%s\",\"rootIsOurs\":%s}\n", narrow(cls).c_str(), who,
                root == h ? "true" : "false");
    return 0;
}

}   // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc < 4) { std::fprintf(stderr, "native_tool info|cap|post|key|drop|hit|close <pid> <tag> ...\n"); return 2; }
    const std::wstring cmd = argv[1];
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
    HWND h = findMain(pid, argv[3]);
    if (!h) { std::printf("{\"ok\":false,\"error\":\"no SuVtitle.MainWindow.%s for pid %lu\"}\n", narrow(argv[3]).c_str(), pid); return 1; }
    if (cmd == L"info") return info(h);
    if (cmd == L"close") { const BOOL ok = ::PostMessageW(h, WM_CLOSE, 0, 0); std::printf("{\"ok\":%s}\n", ok ? "true" : "false"); return 0; }
    if (cmd == L"cap" && argc > 4) {
        std::vector<int> regions;
        for (int i = 5; i < argc; ++i) regions.push_back(_wtoi(argv[i]));
        return capture(h, argv[4], regions);
    }
    if (cmd == L"post" && argc > 6)
        return post(h, argv[4], _wtoi(argv[5]), _wtoi(argv[6]), argc > 7 ? _wtoi(argv[7]) : 0, argc > 8 && _wtoi(argv[8]) != 0);
    if (cmd == L"key" && argc > 4) {
        HWND in = findKid(h, L"Chrome_WidgetWin");
        if (!in) { std::printf("{\"ok\":false,\"error\":\"no input window\"}\n"); return 1; }
        const UINT vk = static_cast<UINT>(std::wcstoul(argv[4], nullptr, 16));
        const UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        const BOOL a = ::PostMessageW(in, WM_KEYDOWN, vk, 1 | (static_cast<LPARAM>(sc) << 16));
        const BOOL b = ::PostMessageW(in, WM_KEYUP, vk, 1 | (static_cast<LPARAM>(sc) << 16) | (1LL << 30) | (1LL << 31));
        std::printf("{\"ok\":%s}\n", a && b ? "true" : "false");
        return a && b ? 0 : 1;
    }
    if (cmd == L"drop" && argc > 4) {
        const std::wstring p = argv[4];
        COPYDATASTRUCT cds{};
        cds.dwData = 0x53564454;                          // 'SVDT'
        cds.cbData = static_cast<DWORD>(p.size() * sizeof(wchar_t));
        cds.lpData = const_cast<wchar_t*>(p.data());
        DWORD_PTR res = 0;
        const LRESULT sent = ::SendMessageTimeoutW(h, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                                   10000, &res);
        std::printf("{\"ok\":%s,\"accepted\":%s}\n", sent ? "true" : "false", res == TRUE ? "true" : "false");
        return sent && res == TRUE ? 0 : 1;
    }
    if (cmd == L"hit" && argc > 5) return hit(h, _wtoi(argv[4]), _wtoi(argv[5]));
    if (cmd == L"uia" && argc > 4) return uia(h, argv[4]);
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}

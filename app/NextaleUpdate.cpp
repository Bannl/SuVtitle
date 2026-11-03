// =============================================================================
// SuVtitle - app/NextaleUpdate.cpp   (NEW 03/11/2026 -- see NextaleUpdate.h)
//
// PORTED from AudioPB's ui/NextaleUpdate.cpp (PatchBus repo, branch APB_TRANSLATOR): WinHTTP GET of the site's manifest,
// AudioPB's version rule (isNewer, parseVer) and its URL guard, then SuVtitle's changes: a real JSON parser (JsonR.h), every
// field validated (an url off nextale-audio.com refuses the whole manifest), SuVtitle's page built from ONE constant with
// ?update=&from= (pageUrl; the site is the guard's only origin), a response capped in size and in time, plain http only on
// the loopback (the test server), and the timeouts a parameter (the smoke's slow server). ASCII only (no /utf-8).
// =============================================================================
#include "NextaleUpdate.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstring>
#include <string>

#include "JsonR.h"   // pbjson: the strict bounded JSON reader the bridge already uses for structured messages

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace nextale {
namespace Update {
namespace {

// "1.2.3-beta" -> v = {1, 2, 3}, pre = true. A missing part is 0; a group is read on 9 digits at most (no overflow, whatever
// the input -- versionOk refuses longer ones anyway).
void parseVer(const std::string& s, int v[3], bool& pre) {
    v[0] = v[1] = v[2] = 0;
    int idx = 0;
    size_t i = 0;
    while (i < s.size() && idx < 3) {
        if (s[i] >= '0' && s[i] <= '9') {
            int n = 0, d = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                if (d < 9) n = n * 10 + (s[i] - '0');
                ++d;
                ++i;
            }
            v[idx++] = n;
            if (i < s.size() && s[i] == '.') ++i;
        } else {
            break;
        }
    }
    pre = (s.find('-') != std::string::npos);   // a "-beta" / "-rc1" suffix = a prerelease
}

bool utf8ToWide(const std::string& s, std::wstring& w) {
    w.clear();
    if (s.empty()) return true;
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;                    // not UTF-8: not a manifest of ours
    w.assign(static_cast<size_t>(n), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n) == n;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// The notes as ONE line: ASCII control characters, U+0085, U+2028 and U+2029 become spaces, runs of spaces one, trimmed, then
// cut to kNotesMax bytes on a character boundary ("..." marks the cut). The page cleans what it shows again (cleanNotice).
std::string oneLine(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        bool space = c < 0x20 || c == 0x7F;
        if (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x85) { space = true; ++i; }
        else if (c == 0xE2 && i + 2 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x80
                 && (static_cast<unsigned char>(in[i + 2]) == 0xA8 || static_cast<unsigned char>(in[i + 2]) == 0xA9)) {
            space = true;
            i += 2;
        }
        if (space || c == ' ') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > kNotesMax) {
        size_t cut = kNotesMax - 3;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // never inside a character
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "...";
    }
    return out;
}

bool startsWith(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

// The characters an URL we open may hold: printable ASCII, no space, no fragment, none of " < > \ ^ ` { | }; 512 at most.
bool urlChars(const std::string& url) {
    if (url.size() > 512) return false;
    for (const char ch : url) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) return false;                  // controls, space, DEL, anything not ASCII
        if (std::strchr("\"#<>\\^`{|}", ch) != nullptr) return false;
    }
    return true;
}

bool isLoopbackHost(const std::wstring& h) {
    std::wstring l;
    for (wchar_t c : h) l += (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    return l == L"127.0.0.1" || l == L"localhost";
}

} // anonymous namespace

// ---- pure --------------------------------------------------------------------------------------------------------
bool versionOk(const std::string& v) {
    if (v.empty() || v.size() > 32) return false;
    size_t i = 0;
    int groups = 0;
    for (;;) {
        size_t d = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { ++i; ++d; }
        if (d == 0 || d > 9) return false;
        ++groups;
        if (i == v.size()) return true;
        if (v[i] == '.' && groups < 3) { ++i; continue; }
        break;
    }
    if (v[i] != '-' || i + 1 == v.size()) return false;
    for (++i; i < v.size(); ++i) {
        const char c = v[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
    }
    return true;
}

bool isNewer(const std::string& remote, const std::string& local) {
    int r[3], l[3];
    bool rpre = false, lpre = false;
    parseVer(remote, r, rpre);
    parseVer(local, l, lpre);
    for (int i = 0; i < 3; ++i)
        if (r[i] != l[i]) return r[i] > l[i];
    // Equal numbers: a release supersedes a same-numbered prerelease (beta -> 1.2.0).
    return lpre && !rpre;
}

bool isPrerelease(const std::string& v) { return v.find('-') != std::string::npos; }   // (parseVer's own test)

bool offered(const std::string& remote, const std::string& local) {
    return isNewer(remote, local) && (!isPrerelease(remote) || isPrerelease(local));   // a beta only to a beta build (N12)
}

bool siteUrl(const std::string& url) {
    // (!) the slash after the host is part of the prefix: "https://www.nextale-audio.com.evil.example/",
    // "https://www.nextale-audio.com@evil.example/" and "https://user:pw@www.nextale-audio.com/" do not start with it
    return (startsWith(url, "https://www.nextale-audio.com/") || startsWith(url, "https://nextale-audio.com/")) && urlChars(url);
}

bool urlAllowed(const std::string& url) {
    return siteUrl(url);   // (the site, nothing else)
}

// The ONE address the banner opens: the constant, then the two versions -- versionOk's alphabet is the URL's unreserved one,
// so a version that passes needs no escape and can add no parameter, path or fragment.
std::string pageUrl(const std::string& newVersion, const std::string& currentVersion) {
    if (!versionOk(newVersion) || !versionOk(currentVersion)) return std::string();
    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;
    return urlAllowed(u) ? u : std::string();   // (the launch guard's own rule, checked here too)
}

bool parseManifest(const std::string& body, std::string& version, std::string& url, std::string& notes) {
    version.clear(); url.clear(); notes.clear();
    std::string b = body;
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF && static_cast<unsigned char>(b[1]) == 0xBB
        && static_cast<unsigned char>(b[2]) == 0xBF)
        b.erase(0, 3);                                               // a BOM some editors and servers prepend
    std::wstring w;
    if (b.size() > kBodyMax || !utf8ToWide(b, w)) return false;
    pbjson::Value v;
    if (!pbjson::parse(w, v, kBodyMax) || !v.isObj()) return false;
    const pbjson::Value* ver = v.get(L"version");
    const pbjson::Value* u = v.get(L"url");
    const pbjson::Value* n = v.get(L"notes");
    if (!ver || ver->kind != pbjson::Value::Kind::Str || !u || u->kind != pbjson::Value::Kind::Str) return false;
    if (n && n->kind != pbjson::Value::Kind::Str && n->kind != pbjson::Value::Kind::Null) return false;
    version = wideToUtf8(ver->s);
    url = wideToUtf8(u->s);
    if (n && n->kind == pbjson::Value::Kind::Str) notes = wideToUtf8(n->s);
    return true;
}

Info evaluate(const std::string& body, const std::string& currentVersion) {
    Info info;
    std::string version, url, notes;
    if (!parseManifest(body, version, url, notes)) return info;
    // (a manifest whose page is not on the site is not offered at all: it is not one of ours -- its url is checked, never
    // opened: the banner opens pageUrl)
    if (!versionOk(version) || !siteUrl(url) || !offered(version, currentVersion)) return info;
    info.available = true;
    info.version = version;
    info.url = url;
    info.notes = oneLine(notes);
    return info;
}

bool loopbackUrlOk(const std::wstring& url) {
    if (url.size() > 512) return false;
    static const wchar_t* const kPrefixes[] = { L"http://127.0.0.1:", L"http://localhost:" };
    size_t at = 0;
    for (const wchar_t* p : kPrefixes) {
        const size_t n = std::wcslen(p);
        if (url.size() > n && url.compare(0, n, p) == 0) { at = n; break; }
    }
    if (at == 0) return false;
    size_t digits = 0;
    while (at < url.size() && url[at] >= L'0' && url[at] <= L'9') { ++at; ++digits; }
    if (digits == 0 || digits > 5 || at >= url.size() || url[at] != L'/') return false;   // a port, then the path
    for (const wchar_t c : url)
        if (c <= 0x20 || c >= 0x7F) return false;
    return true;
}

// ---- the network ----------------------------------------------------------------------------------------------------
bool fetch(const std::wstring& url, const std::wstring& userAgent, std::string& body, unsigned long timeoutMs) {
    body.clear();
    if (url.empty() || url.size() > 2048 || timeoutMs == 0) return false;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc) || !uc.lpszHostName || uc.dwHostNameLength == 0)
        return false;
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path = uc.lpszUrlPath && uc.dwUrlPathLength ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : std::wstring(L"/");
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = isLoopbackHost(host);
    // Plain http only on the loopback (the smoke's server, a test instance's SUVTITLE_TEST_UPDATE_URL) -- refused here,
    // before anything is opened, sent or resolved.
    if (!secure && !(uc.nScheme == INTERNET_SCHEME_HTTP && loopback)) return false;

    HINTERNET ses = ::WinHttpOpen(userAgent.empty() ? L"SuVtitle" : userAgent.c_str(),
                                  loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    if (loopback) {
        // (review 03/11, N7) an answer of the loopback (the smoke's server, a test instance's SUVTITLE_TEST_UPDATE_URL) never
        // sends this fetch OFF the machine: no redirect is followed there -- a 30x is just not a 2xx. (https to the site keeps
        // WinHTTP's default: redirects followed, never https -> http.) Not settable = not fetched.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!::WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never))) { ::WinHttpCloseHandle(ses); return false; }
    }
    const int t = timeoutMs > 600000UL ? 600000 : static_cast<int>(timeoutMs);
    ::WinHttpSetTimeouts(ses, t, t, t, t);                           // resolve, connect, send, receive
    const ULONGLONG deadline = ::GetTickCount64() + 3ULL * static_cast<ULONGLONG>(t);   // a server dripping bytes stops here
    bool ok = false;
    if (HINTERNET con = ::WinHttpConnect(ses, host.c_str(), uc.nPort, 0)) {
        if (HINTERNET req = ::WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)) {
            if (::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && ::WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                          &status, &len, WINHTTP_NO_HEADER_INDEX)
                    && status >= 200 && status < 300) {
                    std::string got;
                    bool whole = false;
                    for (;;) {
                        if (::GetTickCount64() > deadline) break;
                        DWORD avail = 0;
                        if (!::WinHttpQueryDataAvailable(req, &avail)) break;
                        if (avail == 0) { whole = true; break; }               // the end of the response
                        if (got.size() + avail > kBodyMax) break;              // not a manifest: refused, not truncated
                        const size_t at = got.size();
                        got.resize(at + avail);
                        DWORD read = 0;
                        if (!::WinHttpReadData(req, &got[at], avail, &read)) break;
                        got.resize(at + read);
                        if (read == 0) { whole = true; break; }
                    }
                    if (whole) { body.swap(got); ok = true; }
                }
            }
            ::WinHttpCloseHandle(req);
        }
        ::WinHttpCloseHandle(con);
    }
    ::WinHttpCloseHandle(ses);
    return ok;
}

Info check(const std::string& currentVersion, const std::wstring& manifestUrl, unsigned long timeoutMs) {
    std::wstring ua = L"SuVtitle";
    if (versionOk(currentVersion)) ua += L"/" + std::wstring(currentVersion.begin(), currentVersion.end());   // (ASCII)
    std::string body;
    if (!fetch(manifestUrl, ua, body, timeoutMs)) return Info{};   // offline, an HTTP error (404: not published), too big...
    return evaluate(body, currentVersion);
}

bool openInBrowser(const std::string& url) {
    // Checked AGAIN here, whatever the caller did: only ever an https page of our own site, never a scheme or a host the
    // manifest (or anything else) could slip in.
    if (!urlAllowed(url)) return false;
    const std::wstring w(url.begin(), url.end());                    // (ASCII: urlAllowed)
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace Update
} // namespace nextale

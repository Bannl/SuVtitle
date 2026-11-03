// =============================================================================
// update_smoke.cpp -- THE UPDATE BANNER (03/11/2026): app\NextaleUpdate.cpp and AppConfig.h's update lines, RUN.
// NEW. The network half runs against a LOOPBACK server this smoke starts (127.0.0.1, an ephemeral port): no test contacts
// nextale-audio.com or any other host (the one address that is not the loopback, 192.0.2.1 -- RFC 5737's documentation
// range, owned by no one --, is handed to fetch only to prove that plain http to it is refused before any packet: a mutant
// that takes the refusal away would try it, and the timing catches that).
//   [1] versionOk / isNewer -- AudioPB's rule (numbers, a release after its prerelease); offered -- a prerelease only to a
//       prerelease build (review 03/11, N12)
//   [2] siteUrl / urlAllowed -- EXACTLY https://www.nextale-audio.com/ + https://nextale-audio.com/, nothing else: lookalike
//       hosts, credentials, schemes, ports, case, characters, length, another host's page
//   [3] pageUrl -- what the banner's Download opens: EXACTLY kPageUrl + "?update=<new>&from=<this version>" (the constant,
//       the names, their order, the separators)
//   [4] pageUrl refuses a version that is not one, on either side ("" = nothing opened)
//   [5] parseManifest / evaluate -- the JSON read for real (escapes, a "version" inside the notes, a BOM, wrong types,
//       not UTF-8), the manifest's url ON THE SITE, the notes made one line and capped on a character boundary
//   [6] app.txt -- UPDATECHECK / UPDATESKIP: read (tab or spaces, the first counts, values checked), written (only what
//       differs from the default, where the first old line was, hand-typed lines replaced, every other line kept as it is)
//   [7] fetch / check against the loopback server: a newer manifest, the same / an older version, 404, 500, a body too big,
//       not JSON, an url off the site, a silent server (the timeout), a server that DRIPS its body (the overall deadline:
//       review 03/11, N8), a loopback 302 to a host OFF the machine (never followed: N7), the User-Agent and the path asked;
//       http refused off the loopback before anything is sent; loopbackUrlOk (the test instance's SUVTITLE_TEST_UPDATE_URL)
//   [8] the product's constants (the manifest, SuVtitle's page, the bounds)
// openInBrowser is NEVER called here (it would open the user's browser): its guard IS urlAllowed, run in [2].
// Build: tests\cpp\build_update_smoke.cmd (into tests\cpp\work\). /W4, no /utf-8, like the product. ASCII only.
// Mutation sentinels: tests\cpp\cmut_upd.py.
// =============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../app/NextaleUpdate.h"
#include "../../app/AppConfig.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace U = nextale::Update;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}

// ---- the loopback server ---------------------------------------------------------------------------------------------
// One thread accepts; each connection is answered on its own thread from `routes` (path -> status + body). "/slow" answers
// nothing (the client's timeout is what is tested). Every request's head is kept (the path, the User-Agent).
struct Route { int status = 200; std::string body; std::string headers; };   // (headers: extra lines, each ending in \r\n)
struct Server {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::atomic<bool> stop{ false };
    std::thread acceptor;
    std::mutex mx;
    std::vector<std::thread> conns;
    std::vector<std::string> heads;       // every request head received, in order
    std::map<std::string, Route> routes;

    bool start() {
        ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;                                        // ephemeral
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);            // 127.0.0.1 ONLY: never on a network (no firewall prompt)
        if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(ls, 16) != 0) return false;
        int len = sizeof(a);
        if (::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) != 0) return false;
        port = ntohs(a.sin_port);
        acceptor = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        for (;;) {
            SOCKET c = ::accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;                   // the listening socket was closed: the end
            if (stop.load()) { ::closesocket(c); return; }
            std::lock_guard<std::mutex> lk(mx);
            conns.emplace_back([this, c] { serve(c); });
        }
    }
    void serve(SOCKET c) {
        DWORD rto = 3000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
        std::string head;
        char buf[2048];
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384) {
            const int n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            head.append(buf, static_cast<size_t>(n));
        }
        std::string path;
        if (head.rfind("GET ", 0) == 0) {
            const size_t sp = head.find(' ', 4);
            if (sp != std::string::npos) path = head.substr(4, sp - 4);
        }
        Route r;
        bool known = false;
        {
            std::lock_guard<std::mutex> lk(mx);
            heads.push_back(head);
            auto it = routes.find(path);
            if (it != routes.end()) { r = it->second; known = true; }
        }
        if (path == "/slow") {                                 // says nothing (10 s): the client gives up on its own
            for (int i = 0; i < 200 && !stop.load(); ++i) ::Sleep(50);
            ::closesocket(c);
            return;
        }
        if (path == "/drip" && known) {                        // a body that DRIPS -- 4 bytes every 400 ms: each read is in time,
            const std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "   // the whole is not:
                                    + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";      // the deadline ends it
            bool alive = ::send(c, hdr.data(), static_cast<int>(hdr.size()), 0) > 0;
            for (size_t at = 0; alive && at < r.body.size() && !stop.load(); at += 4) {
                ::Sleep(400);
                const int n = static_cast<int>(r.body.size() - at < 4 ? r.body.size() - at : 4);
                alive = ::send(c, r.body.data() + at, n, 0) == n;
            }
            ::shutdown(c, SD_SEND);
            ::closesocket(c);
            return;
        }
        if (!known) { r.status = 404; r.body = "<html>not here</html>"; }
        const char* reason = r.status == 200 ? "OK" : r.status == 302 ? "Found" : r.status == 404 ? "Not Found" : "Internal Server Error";
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\nContent-Type: application/json\r\n" + r.headers
                          + "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
        size_t at = 0;
        while (at < out.size()) {
            const int n = ::send(c, out.data() + at, static_cast<int>(out.size() - at), 0);
            if (n <= 0) break;
            at += static_cast<size_t>(n);
        }
        ::shutdown(c, SD_SEND);
        ::closesocket(c);
    }
    size_t count() { std::lock_guard<std::mutex> lk(mx); return heads.size(); }
    std::string last() { std::lock_guard<std::mutex> lk(mx); return heads.empty() ? std::string() : heads.back(); }
    void end() {
        stop.store(true);
        if (ls != INVALID_SOCKET) { ::closesocket(ls); ls = INVALID_SOCKET; }
        if (acceptor.joinable()) acceptor.join();
        std::vector<std::thread> all;
        { std::lock_guard<std::mutex> lk(mx); all.swap(conns); }
        for (auto& t : all) if (t.joinable()) t.join();
    }
};

static long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("update_smoke (SuVtitle: NextaleUpdate.cpp, AppConfig.h's UPDATECHECK / UPDATESKIP)\n");

    // ---- [1] the version rule ------------------------------------------------------------------------------------------
    std::printf("[1] versionOk / isNewer\n");
    {
        const char* good[] = { "1", "1.2", "1.2.3", "10.20.30", "1.2.3-beta", "1.2.0-rc.1", "1.2.0-RC-2", "123456789.0.1" };
        const char* bad[] = { "", "v1.2.3", "1.2.3.4", "1..2", "1.2.", ".1", "1.2.3-", "1.2.3+build", "1 .2", "1.2 ", " 1.2",
                              "1234567890.0.0", "1.2.3-beta/../x", "1.2.3-b\xC3\xA9", "1.2.3-123456789012345678901234567" };
        bool allGood = true, allBad = true;
        std::string which;
        for (const char* v : good) if (!U::versionOk(v)) { allGood = false; which += std::string("[") + v + "] "; }
        for (const char* v : bad) if (U::versionOk(v)) { allBad = false; which += std::string("[") + v + "] "; }
        ok("[1] versionOk: 1-3 groups of 1-9 digits, an optional -prerelease of [0-9A-Za-z.-], 32 characters at most -- nothing else",
           allGood && allBad && U::versionOk("1.2.3-12345678901234567890123456") && !U::versionOk("1.2.3-123456789012345678901234567"),
           which);
        struct V { const char* r; const char* l; bool newer; };
        const V vs[] = {
            { "1.2.0", "1.1.0", true }, { "1.1.0", "1.1.0", false }, { "1.0.9", "1.1.0", false }, { "1.10.0", "1.9.0", true },
            { "2", "1.9.9", true }, { "1.1", "1.1.0", false }, { "1.1.1", "1.1", true }, { "1.1.0", "1.1.0-beta", true },
            { "1.1.0-beta", "1.1.0", false }, { "1.1.0-rc2", "1.1.0-rc1", false }, { "1.1.1-beta", "1.1.0", true },
            { "0.9.9", "1.0.0", false }, { "1.0.0", "0.9.9", true }, { "1.2.10", "1.2.9", true }, { "1.2.9", "1.2.10", false },
        };
        std::string miss;
        for (const V& v : vs) if (U::isNewer(v.r, v.l) != v.newer) miss += std::string(v.r) + " vs " + v.l + "; ";
        ok("[1] isNewer: numbers compared as numbers (1.10 after 1.9), a missing part is 0, a release after its own prerelease, two "
           "prereleases of one number equal", miss.empty(), miss);
        struct O { const char* r; const char* l; bool offered; };
        const O os[] = {
            { "1.2.0", "1.1.0", true }, { "1.2.0-beta", "1.1.0", false }, { "2.0.0-rc.1", "1.9.9", false },
            { "1.2.0-beta", "1.1.0-beta", true }, { "1.3.0-beta", "1.2.0-rc1", true }, { "1.2.0", "1.2.0-beta", true },
            { "1.2.0-rc2", "1.2.0-rc1", false }, { "1.1.0", "1.1.0", false }, { "1.0.0", "1.1.0-beta", false },
        };
        std::string omiss;
        for (const O& o : os) if (U::offered(o.r, o.l) != o.offered) omiss += std::string(o.r) + " to " + o.l + "; ";
        ok("[1] offered (review 03/11, N12): newer, AND a prerelease only to a prerelease build -- a release build is never offered a "
           "beta; a beta build is offered a newer beta and its own release",
           omiss.empty() && U::isPrerelease("1.2.0-beta") && !U::isPrerelease("1.2.0"), omiss);
    }

    // ---- [2] the guard ------------------------------------------------------------------------------------------------
    std::printf("[2] siteUrl / urlAllowed: the site, nothing else\n");
    {
        const char* site[] = { "https://www.nextale-audio.com/suvtitle", "https://nextale-audio.com/suvtitle",
                               "https://www.nextale-audio.com/suvtitle?update=1.2.0&from=1.1.0", "https://www.nextale-audio.com/",
                               "https://www.nextale-audio.com/a/b-c_d.e~f%20g" };
        const char* notSite[] = {
            "http://www.nextale-audio.com/suvtitle",              // not https
            "https://www.nextale-audio.com",                       // no slash after the host
            "https://www.nextale-audio.com.evil.example/",         // a lookalike host
            "https://www.nextale-audio.com@evil.example/",         // credentials: the host is evil.example
            "https://user:pw@www.nextale-audio.com/suvtitle",      // embedded credentials
            "https://evil.example/https://www.nextale-audio.com/", // the site in another host's path
            "https://www.nextale-audio.com:8443/suvtitle",         // another port
            "HTTPS://WWW.NEXTALE-AUDIO.COM/suvtitle",              // (the rule is exact, case included)
            "https://wwwnextale-audio.com/", "https://www-nextale-audio.com/", "https://nextale-audio.co/",
            "https://www.nextale-audio.com/a b", "https://www.nextale-audio.com/#top", "https://www.nextale-audio.com/\"x\"",
            "https://www.nextale-audio.com/<x>", "https://www.nextale-audio.com/a\\b", "https://www.nextale-audio.com/{x}",
            "https://www.nextale-audio.com/\x01", "https://www.nextale-audio.com/caf\xC3\xA9", "javascript:alert(1)",
            "file:///C:/Windows/notepad.exe", "", "https://example.com/suvtitle" };
        bool a = true, b = true;
        std::string which;
        for (const char* u : site) if (!U::siteUrl(u) || !U::urlAllowed(u)) { a = false; which += std::string("[") + u + "] "; }
        for (const char* u : notSite) if (U::siteUrl(u)) { b = false; which += std::string("{") + u + "} "; }
        const std::string longSite = "https://www.nextale-audio.com/" + std::string(512 - 30, 'a');
        ok("[2] siteUrl: https://www.nextale-audio.com/... or https://nextale-audio.com/... -- no lookalike host, credentials, other "
           "scheme / port / case, no space, fragment or \" < > \\ ^ ` { | }, ASCII, 512 characters at most",
           a && b && longSite.size() == 512 && U::siteUrl(longSite) && !U::siteUrl(longSite + "a"), which);
        // urlAllowed IS the site's rule: another host's page -- SuVtitle's own query on it too -- is refused like any address
        // off the site
        const char* otherHost[] = { "https://example.com/suvtitle?update=1.2.0&from=1.1.0", "https://downloads.example.com/suvtitle" };
        bool e = true;
        which.clear();
        for (const char* u : notSite) if (U::urlAllowed(u)) { e = false; which += std::string("{") + u + "} "; }
        for (const char* u : otherHost) if (U::urlAllowed(u)) { e = false; which += std::string("{") + u + "} "; }
        ok("[2] urlAllowed: the site and nothing else -- everything siteUrl refuses is refused, another host's page included "
           "(with or without SuVtitle's query)", e && !U::urlAllowed(longSite + "a"), which);
    }

    // ---- [3] SuVtitle's page: what Download opens ----------------------------------------------------------------------
    std::printf("[3] pageUrl: EXACTLY kPageUrl?update=<new>&from=<this version>\n");
    {
        const std::string p1 = U::pageUrl("1.2.0", "1.1.0");
        const std::string p2 = U::pageUrl("1.2.0-beta", "1.1.0-rc.1");
        const std::string p3 = U::pageUrl("2", "1.10.3");
        ok("[3] pageUrl: the constant, then \"?update=\" + the new version + \"&from=\" + this version -- those names, that order, "
           "those separators, nothing more; allowed by the guard",
           p1 == "https://www.nextale-audio.com/suvtitle?update=1.2.0&from=1.1.0"
           && p2 == "https://www.nextale-audio.com/suvtitle?update=1.2.0-beta&from=1.1.0-rc.1"
           && p3 == "https://www.nextale-audio.com/suvtitle?update=2&from=1.10.3" && U::urlAllowed(p1) && U::urlAllowed(p2),
           p1 + " | " + p2 + " | " + p3);
        const std::string v32 = "1.2.3-12345678901234567890123456";   // 32 characters: the longest version
        ok("[3] pageUrl takes the longest versions (32 characters on each side)",
           v32.size() == 32 && U::pageUrl(v32, v32) == std::string(U::kPageUrl) + "?update=" + v32 + "&from=" + v32);
    }

    // ---- [4] ... and nothing for what is not a version -----------------------------------------------------------------
    std::printf("[4] pageUrl: a version that is not one, on either side = \"\" (nothing opened)\n");
    {
        const std::string v33 = "1.2.3-123456789012345678901234567";   // 33 characters
        const char* bad[] = { "1.2.0&x=y", "", "1.2.0/../x", "1.2.0#top", "1.2.0?x=1", "1.2.0 ", " 1.2.0", "v1.2.0",
                              "1.2.0%26x=y", "https://evil.example/", "1.2.0\r\nx" };
        bool none = v33.size() == 33 && U::pageUrl(v33, "1.1.0").empty() && U::pageUrl("1.2.0", v33).empty();
        std::string which;
        for (const char* b : bad)
            if (!U::pageUrl(b, "1.1.0").empty() || !U::pageUrl("1.2.0", b).empty()) { none = false; which += std::string("[") + b + "] "; }
        ok("[4] pageUrl refuses -- \"\", nothing to open -- a version that is not one on EITHER side: \"1.2.0&x=y\" (a parameter "
           "more), \"\", 33 characters, \"1.2.0/../x\" (a path), a fragment, a query, spaces, a percent-escape, an address",
           none, which);
    }

    // ---- [5] the manifest ----------------------------------------------------------------------------------------------
    std::printf("[5] parseManifest / evaluate\n");
    {
        const std::string m1 = "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":\"Faster \\\"live\\\" mode\\nand caf\\u00e9\"}";
        const U::Info i1 = U::evaluate(m1, "1.1.0");
        ok("[5] a newer manifest: available, its version and page, the notes ONE line with their escapes decoded (\\\" \\n \\u00e9)",
           i1.available && i1.version == "1.2.0" && i1.url == "https://www.nextale-audio.com/suvtitle"
           && i1.notes == "Faster \"live\" mode and caf\xC3\xA9", i1.notes);
        ok("[5] the same version, an older one: nothing", !U::evaluate(m1, "1.2.0").available && !U::evaluate(m1, "1.3").available);
        const std::string beta = "{\"version\":\"1.2.0-beta\",\"url\":\"https://www.nextale-audio.com/suvtitle\"}";
        ok("[5] (N12) a PRERELEASE in the manifest: never offered to a release build (1.1.0), offered to a prerelease build (1.1.0-beta)",
           !U::evaluate(beta, "1.1.0").available && U::evaluate(beta, "1.1.0-beta").available);
        const std::string tricky = "{\"notes\":\"the \\\"version\\\":\\\"99.0.0\\\" of it\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"version\":\"1.0.0\"}";
        const U::Info i2 = U::evaluate(tricky, "1.1.0");
        std::string v2, u2, n2;
        const bool p2 = U::parseManifest(tricky, v2, u2, n2);
        ok("[5] a real JSON reader: a \"version\" INSIDE the notes is not the version (1.0.0 < 1.1.0: nothing offered)",
           p2 && v2 == "1.0.0" && !i2.available, v2);
        const std::string bom = "\xEF\xBB\xBF" + m1;
        ok("[5] a UTF-8 BOM before the JSON is read through; spaces and newlines around the members too",
           U::evaluate(bom, "1.1.0").available
           && U::evaluate("\n { \"version\" : \"1.2.0\" ,\r\n \"url\" : \"https://nextale-audio.com/suvtitle\" } \n", "1.1.0").available);
        const char* refused[] = {
            "", "not json", "[\"1.2.0\"]", "{\"version\":\"1.2.0\"}", "{\"url\":\"https://www.nextale-audio.com/suvtitle\"}",
            "{\"version\":1.2,\"url\":\"https://www.nextale-audio.com/suvtitle\"}",
            "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":5}",
            "{\"version\":\"v1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\"}",
            "{\"version\":\"1.2.0\",\"url\":\"https://evil.example/suvtitle\"}",
            "{\"version\":\"1.2.0\",\"url\":\"http://www.nextale-audio.com/suvtitle\"}",
            "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com.example.com/suvtitle\"}",
            "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\"",          // cut short
            "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\"} x",       // something after it
            "{\"version\":\"1.2.0\xFF\",\"url\":\"https://www.nextale-audio.com/suvtitle\"}" };  // not UTF-8
        bool none = true;
        std::string which;
        for (const char* r : refused) if (U::evaluate(r, "1.1.0").available) { none = false; which += std::string("{") + r + "} "; }
        ok("[5] refused, silently: not JSON, not an object, a missing / mistyped member, a version that is not one, an url off the "
           "SITE (another host, http, a lookalike host), cut short, trailing bytes, not UTF-8", none, which);
        ok("[5] JSON is UTF-8: notes in another encoding (a Latin-1 e-acute) make it not a manifest of ours -- refused, not guessed",
           !U::evaluate("{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":\"caf\xE9\"}", "1.1.0").available);
        ok("[5] notes: null = none; absent = none",
           U::evaluate("{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":null}", "1.1.0").available
           && U::evaluate("{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\"}", "1.1.0").notes.empty());
        // the notes: control characters and the Unicode line separators become spaces, runs of them one; capped on a boundary
        std::string longNotes;
        for (int i = 0; i < 300; ++i) longNotes += "\xC3\xA9";                                // 600 bytes of a 2-byte character
        const std::string m3 = "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":\" a\\t\\tb\\u2028c\\u0085d  \"}";
        const std::string m4 = "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":\"" + longNotes + "\"}";
        const U::Info i3 = U::evaluate(m3, "1.1.0"), i4 = U::evaluate(m4, "1.1.0");
        bool boundary = i4.notes.size() <= U::kNotesMax && i4.notes.size() >= U::kNotesMax - 5
                        && i4.notes.compare(i4.notes.size() - 3, 3, "...") == 0;
        for (size_t i = 0; boundary && i + 3 < i4.notes.size(); i += 2)
            boundary = i4.notes.compare(i, 2, "\xC3\xA9") == 0;                             // never half a character
        ok("[5] notes made ONE line (tabs, U+2028, U+0085 -> spaces, runs collapsed, trimmed) and capped at kNotesMax bytes on a "
           "character boundary, \"...\" marking the cut", i3.notes == "a b c d" && boundary, i3.notes);
        std::string huge = "{\"version\":\"1.2.0\",\"url\":\"https://www.nextale-audio.com/suvtitle\",\"notes\":\"";
        huge += std::string(U::kBodyMax, 'x') + "\"}";
        ok("[5] a body over kBodyMax is not a manifest (even well formed)", !U::evaluate(huge, "1.1.0").available);
    }

    // ---- [6] app.txt ---------------------------------------------------------------------------------------------------
    std::printf("[6] app.txt: UPDATECHECK / UPDATESKIP\n");
    {
        const pbapp::UpdatePrefs d = pbapp::parseUpdatePrefs("");
        ok("[6] nothing written: the check ON, nothing skipped", d.check && d.skip.empty());
        struct C { const char* text; bool check; };
        const C cs[] = { { "UPDATECHECK\t0\n", false }, { "UPDATECHECK 0\n", false }, { "UPDATECHECK   off  \r\n", false },
                         { "UPDATECHECK\tNO\n", false }, { "UPDATECHECK\tfalse\n", false }, { "UPDATECHECK\t1\n", true },
                         { "UPDATECHECK\tyes\n", true }, { "UPDATECHECK\t\n", true }, { "UPDATECHECKS\t0\n", true },
                         { "UPDATECHECK\n", true }, { "# UPDATECHECK\t0\n", true }, { "UPDATECHECK\t1\nUPDATECHECK\t0\n", true } };
        std::string miss;
        for (const C& c : cs) if (pbapp::parseUpdatePrefs(c.text).check != c.check) miss += std::string("[") + c.text + "] ";
        ok("[6] UPDATECHECK: 0 / off / no / false (any case; a tab OR spaces) turn the check off; anything else, another tag, a comment: "
           "on; the first line counts", miss.empty(), miss);
        const pbapp::UpdatePrefs s1 = pbapp::parseUpdatePrefs("WINPOS\t1\t2\t300\t400\t0\nUPDATESKIP  1.2.0 \n");
        const pbapp::UpdatePrefs s2 = pbapp::parseUpdatePrefs("UPDATESKIP\t../x\n");
        ok("[6] UPDATESKIP read (spaces trimmed); a value that is not a version is none",
           s1.skip == "1.2.0" && s1.check && s2.skip.empty());
        // every other line is another writer's, or one this app does not know: kept AS IT IS (spaces and all) -- a lookalike
        // tag (UPDATECHECKS) included -- in its place
        const std::string old = "# SuVtitle app v1\nWINPOS\t1\t2\t300\t400\t0\t0\nUPDATECHECK 0\nMODEL\tasr\tC:\\m.bin\nNOTE  kept as it is  \n"
                                "UPDATECHECKS\t0\nLIVEPOS\t5\t6\t700\t500\t0\t0\n";
        pbapp::UpdatePrefs p;
        p.check = true; p.skip = "1.2.0";
        const std::string a = pbapp::packUpdatePrefs(old, p);
        ok("[6] written: the hand-typed UPDATECHECK line (spaces) REPLACED -- on = no line --, UPDATESKIP where the first old line "
           "was, every other line -- another writer's, an unknown one with its spaces, a lookalike tag -- kept as it is, in order; "
           "one header",
           a == "# SuVtitle app v1\nWINPOS\t1\t2\t300\t400\t0\t0\nUPDATESKIP\t1.2.0\nMODEL\tasr\tC:\\m.bin\nNOTE  kept as it is  \n"
                "UPDATECHECKS\t0\nLIVEPOS\t5\t6\t700\t500\t0\t0\n", a);
        p.check = false; p.skip.clear();
        const std::string b = pbapp::packUpdatePrefs(a, p);
        ok("[6] off = \"UPDATECHECK\\t0\"; no skip = its line gone",
           b == "# SuVtitle app v1\nWINPOS\t1\t2\t300\t400\t0\t0\nUPDATECHECK\t0\nMODEL\tasr\tC:\\m.bin\nNOTE  kept as it is  \n"
                "UPDATECHECKS\t0\nLIVEPOS\t5\t6\t700\t500\t0\t0\n", b);
        const pbapp::UpdatePrefs back = pbapp::parseUpdatePrefs(a);
        p.check = true; p.skip = "1.2.0";
        ok("[6] read back what was written; written again: the same bytes (an unchanged pack is no write)",
           back.check && back.skip == "1.2.0" && pbapp::packUpdatePrefs(a, p) == a);
        p.skip = "../evil";
        const std::string c = pbapp::packUpdatePrefs("", p);
        ok("[6] a value that is not a version is never written", c == "# SuVtitle app v1\n", c);
        const std::string fresh = pbapp::packUpdatePrefs("WINPOS\t1\t2\t300\t400\t0\t0\n", pbapp::UpdatePrefs{ false, "1.2.0" });
        ok("[6] no old line: appended at the end", fresh == "# SuVtitle app v1\nWINPOS\t1\t2\t300\t400\t0\t0\nUPDATECHECK\t0\nUPDATESKIP\t1.2.0\n", fresh);
    }

    // ---- [7] the network, against the loopback ----------------------------------------------------------------------------
    std::printf("[7] fetch / check against a loopback server (127.0.0.1)\n");
    {
        WSADATA wsa{};
        const bool wsOk = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        Server srv;
        const bool up = wsOk && srv.start();
        ok("[7] the loopback server is up (127.0.0.1, an ephemeral port)", up);
        if (up) {
            const std::string page = "https://www.nextale-audio.com/suvtitle";
            srv.routes["/updates/suvtitle.json"] = { 200, "{\"version\":\"9.8.7\",\"url\":\"" + page + "\",\"notes\":\"Hello\"}" };
            srv.routes["/same"] = { 200, "{\"version\":\"1.1.0\",\"url\":\"" + page + "\"}" };
            srv.routes["/older"] = { 200, "{\"version\":\"1.0.6\",\"url\":\"" + page + "\"}" };
            srv.routes["/404"] = { 404, "{\"version\":\"9.9.9\",\"url\":\"" + page + "\"}" };      // a manifest's body, an error's status
            srv.routes["/500"] = { 500, "{\"version\":\"9.9.9\",\"url\":\"" + page + "\"}" };
            srv.routes["/big"] = { 200, std::string(U::kBodyMax + 1, 'x') };
            srv.routes["/notjson"] = { 200, "<html><body>Hello</body></html>" };
            srv.routes["/offsite"] = { 200, "{\"version\":\"9.9.9\",\"url\":\"https://evil.example/suvtitle\"}" };
            srv.routes["/drip"] = { 200, "{\"version\":\"9.9.9\",\"url\":\"" + page + "\",\"notes\":\"dripped, 4 bytes at a time\"}" };
            // (N7) a loopback answer that REDIRECTS off the machine -- to RFC 5737's documentation address, owned by no one
            srv.routes["/redirect"] = { 302, "", "Location: http://192.0.2.1:9/updates/suvtitle.json\r\n" };
            const std::wstring base = L"http://127.0.0.1:" + std::to_wstring(srv.port);

            const U::Info i = U::check("1.1.0", base + L"/updates/suvtitle.json", 5000);
            const std::string head = srv.last();
            ok("[7] a newer manifest: available, its version, page and notes", i.available && i.version == "9.8.7" && i.url == page
               && i.notes == "Hello");
            ok("[7] the request: GET of the manifest's path, User-Agent \"SuVtitle/1.1.0\" -- nothing about the user",
               head.rfind("GET /updates/suvtitle.json HTTP/1.1\r\n", 0) == 0 && head.find("\r\nUser-Agent: SuVtitle/1.1.0\r\n") != std::string::npos
               && head.find("Cookie") == std::string::npos, head.substr(0, 200));
            ok("[7] the same version, an older one: nothing",
               !U::check("1.1.0", base + L"/same", 5000).available && !U::check("1.1.0", base + L"/older", 5000).available);
            std::string body;
            ok("[7] 404 (the manifest not published yet: today's real answer) and 500: nothing -- the body of an error is never read as "
               "a manifest", !U::check("1.1.0", base + L"/404", 5000).available && !U::check("1.1.0", base + L"/500", 5000).available
               && !U::fetch(base + L"/404", L"SuVtitle/1.1.0", body, 5000) && body.empty());
            ok("[7] a path the server does not know: 404, nothing", !U::check("1.1.0", base + L"/nothing-here", 5000).available);
            body = "stale";
            ok("[7] a body over kBodyMax: refused (not truncated), the output empty",
               !U::fetch(base + L"/big", L"SuVtitle/1.1.0", body, 5000) && body.empty());
            ok("[7] not JSON, an url off the site: nothing",
               !U::check("1.1.0", base + L"/notjson", 5000).available && !U::check("1.1.0", base + L"/offsite", 5000).available);
            const auto t0 = std::chrono::steady_clock::now();
            const bool slow = U::check("1.1.0", base + L"/slow", 1500).available;
            const long long slowMs = msSince(t0);
            ok("[7] a server that says nothing: given up on at the timeout (1.5 s here; 10 s in the product), nothing",
               !slow && slowMs >= 1000 && slowMs < 6000, std::to_string(slowMs) + " ms");
            // (review 03/11, N8) the OVERALL deadline: every read of a dripping body comes in time (400 ms < the 1 s step), the
            // whole would take ~9 s -- dropped at 3 x the step timeout, the output empty (a valid manifest, had it been read)
            const auto td = std::chrono::steady_clock::now();
            std::string dripBody = "stale";
            const bool drip = U::fetch(base + L"/drip", L"SuVtitle/1.1.0", dripBody, 1000);
            const long long dripMs = msSince(td);
            ok("[7] a server that DRIPS its body (4 bytes every 400 ms: each read in time, the whole ~9 s) is dropped at the overall "
               "deadline, 3 x the step timeout (3 s here; 30 s in the product): nothing, the output empty",
               !drip && dripBody.empty() && dripMs >= 2500 && dripMs < 6000, std::to_string(dripMs) + " ms");
            // (review 03/11, N7) redirects are OFF on the loopback: the 302 itself ends the fetch, at once -- a fetch that followed
            // it would try 192.0.2.1 and wait for the connect timeout (3 s here)
            const auto tr = std::chrono::steady_clock::now();
            std::string redirBody = "stale";
            const bool redirFetched = U::fetch(base + L"/redirect", L"SuVtitle/1.1.0", redirBody, 3000);
            const bool redirOffered = U::check("1.1.0", base + L"/redirect", 3000).available;
            const long long redirMs = msSince(tr);
            ok("[7] a loopback 302 to a host OFF the machine is never followed (no redirect on the loopback): no update, nothing read, "
               "at once (no connection tried)", !redirFetched && redirBody.empty() && !redirOffered && redirMs < 1500,
               std::to_string(redirMs) + " ms");
            const size_t before = srv.count();
            const auto t1 = std::chrono::steady_clock::now();
            const bool plain = U::fetch(L"http://192.0.2.1:9/updates/suvtitle.json", L"SuVtitle/1.1.0", body, 4000);
            const long long plainMs = msSince(t1);
            const bool ftp = U::fetch(L"ftp://127.0.0.1:" + std::to_wstring(srv.port) + L"/updates/suvtitle.json", L"x", body, 4000);
            const bool rel = U::fetch(L"/updates/suvtitle.json", L"x", body, 4000) || U::fetch(L"", L"x", body, 4000);
            ok("[7] plain http OFF the loopback is refused at once (no connection tried: well under the 4 s timeout), as is another "
               "scheme or no URL", !plain && plainMs < 1000 && !ftp && !rel && srv.count() == before, std::to_string(plainMs) + " ms");
            srv.end();
        }
        if (wsOk) ::WSACleanup();
        const wchar_t* lgood[] = { L"http://127.0.0.1:8080/updates/suvtitle.json", L"http://localhost:1/x", L"http://127.0.0.1:65535/" };
        const wchar_t* lbad[] = { L"https://www.nextale-audio.com/updates/suvtitle.json", L"http://127.0.0.1/updates/suvtitle.json",
                                  L"http://127.0.0.1:/x", L"http://127.0.0.1:123456/x", L"http://127.0.0.1:80", L"http://127.0.0.1.evil.example:80/",
                                  L"http://localhost.evil.example:80/", L"http://127.0.0.2:80/", L"http://user@127.0.0.1:80/",
                                  L"http://127.0.0.1:80/a b", L"https://127.0.0.1:443/", L"", L"file:///C:/x" };
        bool lg = true, lb = true;
        for (const wchar_t* u : lgood) if (!U::loopbackUrlOk(u)) lg = false;
        for (const wchar_t* u : lbad) if (U::loopbackUrlOk(u)) lb = false;
        ok("[7] loopbackUrlOk (a test instance's SUVTITLE_TEST_UPDATE_URL): http://127.0.0.1:<port>/ or http://localhost:<port>/ "
           "only -- never the site, another host, credentials, no port", lg && lb);
    }

    // ---- [8] the product's constants ------------------------------------------------------------------------------------
    std::printf("[8] the product's constants\n");
    ok("[8] the manifest is SuVtitle's on the site, the page Download opens is SuVtitle's (ONE constant), 10 s steps, 64 KB",
       std::wstring(U::kManifestUrl) == L"https://www.nextale-audio.com/updates/suvtitle.json"
       && std::string(U::kPageUrl) == "https://www.nextale-audio.com/suvtitle" && U::kTimeoutMs == 10000
       && U::kBodyMax == 65536 && U::kNotesMax == 500);

    std::printf("\nupdate_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

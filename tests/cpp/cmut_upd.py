# cmut_upd.py -- MUTATION SENTINELS for THE UPDATE BANNER's C++ (SuVtitle, 03/11/2026): app/NextaleUpdate.cpp / .h (the version
# rule, the guard -- the site only --, SuVtitle's page address, the manifest's reader, the fetcher's bounds) and AppConfig.h's
# UPDATECHECK / UPDATESKIP -- each one-line change must make tests/cpp/update_smoke.cpp FAIL.
# The repo's cmut.py machinery (copy the smoke + the product files it compiles into a scratch tree with the SAME relative layout,
# apply ONE textual change, compile with the smoke's own flags, run: CAUGHT = the smoke exits non-zero; a mutant that does not
# compile, or applies other than exactly once, is a FAILED sentinel).
#
#   python -I cmut_upd.py [upd] [name-filter]      (run it from cmut_upd.cmd: cl on PATH)
#   python -I cmut_upd.py --dry                     every mutant applies exactly once? (no compile)
#
# The smoke's network half runs against ITS OWN loopback server (127.0.0.1); the one non-loopback address it names (192.0.2.1,
# RFC 5737's documentation range) is only ever tried by the mutant that takes "plain http only on the loopback" away -- that is
# how the mutant is caught (the connection attempt outlasts the refusal's bound). No mutant contacts nextale-audio.com.
# The bridge's wiring (what is opened, once per start, what the page is told) is pinned by tests/web/test_host_pins.js UPD-1..6.
# When a product line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I does not put the script's folder on the path)
import cmut

cmut.WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_upd")

U = "app/NextaleUpdate.cpp"
H = "app/NextaleUpdate.h"
C = "app/AppConfig.h"

TARGETS = {
    "upd": dict(
        smoke="tests/cpp/update_smoke.cpp", sources=[U],
        files=[H, "app/JsonR.h", C, "common/TrPipe.h"], inc=["common"],
        libs=["ws2_32.lib", "winhttp.lib", "shell32.lib", "user32.lib"], helper=None,
        mutants=[
            # [1] the version rule
            ("[1] a 10-digit group is a version", U, "        if (d == 0 || d > 9) return false;", "        if (d == 0) return false;"),
            ("[1] a fourth group is a version", U, "        if (v[i] == '.' && groups < 3) { ++i; continue; }", "        if (v[i] == '.') { ++i; continue; }"),
            ("[1] a release does not supersede its own prerelease", U, "    return lpre && !rpre;", "    return false;"),
            ("[1] older reads as newer", U, "        if (r[i] != l[i]) return r[i] > l[i];", "        if (r[i] != l[i]) return r[i] < l[i];"),
            ("[1] a group read on its last digit only (1.10 = 1.0)", U, "                if (d < 9) n = n * 10 + (s[i] - '0');", "                n = s[i] - '0';"),
            # (review 03/11, N12) a prerelease only to a prerelease build
            ("[1] a prerelease offered to a release build (every 1.1.0 user gets the beta)", U,
             "    return isNewer(remote, local) && (!isPrerelease(remote) || isPrerelease(local));   // a beta only to a beta build (N12)",
             "    return isNewer(remote, local);   // a beta only to a beta build (N12)"),
            ("[1] a prerelease build never offered a newer beta", U,
             "    return isNewer(remote, local) && (!isPrerelease(remote) || isPrerelease(local));   // a beta only to a beta build (N12)",
             "    return isNewer(remote, local) && !isPrerelease(remote);   // a beta only to a beta build (N12)"),
            # [2] the guard
            ("[2] the site's prefix without its slash (lookalike hosts, credentials pass)", U,
             'return (startsWith(url, "https://www.nextale-audio.com/") || startsWith(url, "https://nextale-audio.com/")) && urlChars(url);',
             'return (startsWith(url, "https://www.nextale-audio.com") || startsWith(url, "https://nextale-audio.com/")) && urlChars(url);'),
            ("[2] the forbidden characters let through (a fragment, quotes, < >)", U,
             '        if (std::strchr("\\"#<>\\\\^`{|}", ch) != nullptr) return false;\n', ""),
            ("[2] no length bound", U, "    if (url.size() > 512) return false;\n    for (const char ch : url) {", "    for (const char ch : url) {"),
            # the guard is the site's rule, nothing else
            ("[2] another host's page let through the guard", U,
             "    return siteUrl(url);   // (the site, nothing else)",
             '    return siteUrl(url) || startsWith(url, "https://example.com/");'),
            ("[2] the guard takes any https address", U,
             "    return siteUrl(url);   // (the site, nothing else)",
             '    return siteUrl(url) || startsWith(url, "https://");'),
            # [3] SuVtitle's page: EXACTLY kPageUrl + "?update=<new>&from=<this version>"
            ("[3] the page's ONE constant changed (another page of the site)", H,
             'constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle";',
             'constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle/";'),
            ("[3] the page's constant on the bare domain", H,
             'constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle";',
             'constexpr const char* kPageUrl = "https://nextale-audio.com/suvtitle";'),
            ("[3] a parameter renamed (update= -> version=)", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?version=" + newVersion + "&from=" + currentVersion;'),
            ("[3] the other parameter renamed (from= -> current=)", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&current=" + currentVersion;'),
            ("[3] the two parameters in the other order", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?from=" + currentVersion + "&update=" + newVersion;'),
            ("[3] a separator changed ('&' -> ';')", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + ";from=" + currentVersion;'),
            ("[3] the query's '?' lost", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "update=" + newVersion + "&from=" + currentVersion;'),
            ("[3] the two versions swapped", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?update=" + currentVersion + "&from=" + newVersion;'),
            ("[3] this version dropped (no from=)", U,
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion + "&from=" + currentVersion;',
             '    const std::string u = std::string(kPageUrl) + "?update=" + newVersion;'),
            # [4] nothing for what is not a version, on either side
            ("[4] the new version not checked (\"1.2.0&x=y\" adds a parameter)", U,
             "    if (!versionOk(newVersion) || !versionOk(currentVersion)) return std::string();",
             "    if (!versionOk(currentVersion)) return std::string();"),
            ("[4] this version not checked", U,
             "    if (!versionOk(newVersion) || !versionOk(currentVersion)) return std::string();",
             "    if (!versionOk(newVersion)) return std::string();"),
            # [5] the manifest
            ("[5] a manifest's url not checked at all (a page off the site taken)", U,
             "    if (!versionOk(version) || !siteUrl(url) || !offered(version, currentVersion)) return info;",
             "    if (!versionOk(version) || !offered(version, currentVersion)) return info;"),
            ("[5] the same version offered", U,
             "    if (!versionOk(version) || !siteUrl(url) || !offered(version, currentVersion)) return info;",
             "    if (!versionOk(version) || !siteUrl(url)) return info;"),
            ("[5] the manifest judged by isNewer alone (the prerelease rule bypassed)", U,
             "    if (!versionOk(version) || !siteUrl(url) || !offered(version, currentVersion)) return info;",
             "    if (!versionOk(version) || !siteUrl(url) || !isNewer(version, currentVersion)) return info;"),
            # (both conversions carry the flag: the mutant drops it from both -- one alone is caught by the other)
            ("[5] JSON in another encoding guessed (invalid UTF-8 replaced, both calls)", U,
             r"""    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;                    // not UTF-8: not a manifest of ours
    w.assign(static_cast<size_t>(n), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n) == n;""",
             r"""    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;                    // not UTF-8: not a manifest of ours
    w.assign(static_cast<size_t>(n), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n) == n;"""),
            ("[5] the notes keep their tabs and line breaks", U, "        bool space = c < 0x20 || c == 0x7F;", "        bool space = false;"),
            ("[5] the notes cut inside a character", U,
             "        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // never inside a character\n", ""),
            ("[5] notes that are not a string taken", U,
             "    if (n && n->kind != pbjson::Value::Kind::Str && n->kind != pbjson::Value::Kind::Null) return false;\n", ""),
            # [7] the fetcher
            ("[7] plain http to ANY host (not the loopback only)", U,
             "    if (!secure && !(uc.nScheme == INTERNET_SCHEME_HTTP && loopback)) return false;",
             "    if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP) return false;"),
            ("[7] an error page (404) read as the manifest", U, "                    && status >= 200 && status < 300) {", "                    ) {"),
            ("[7] a body of any size read", U, "                        if (got.size() + avail > kBodyMax) break;              // not a manifest: refused, not truncated\n", ""),
            ("[7] no timeouts (WinHTTP's defaults: 30 s and more)", U, "    ::WinHttpSetTimeouts(ses, t, t, t, t);                           // resolve, connect, send, receive\n", ""),
            # (review 03/11, N8) the overall deadline: a server that drips its body keeps every read in time
            ("[7] no overall deadline (a dripping server holds the worker as long as it likes)", U,
             "                        if (::GetTickCount64() > deadline) break;\n", ""),
            # (review 03/11, N7) no redirect on the loopback: a 302 from it could send the fetch off the machine
            ("[7] a loopback redirect followed (off the machine)", U,
             "        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;", "        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;"),
            ("[7] the User-Agent without the version", U, '    if (versionOk(currentVersion)) ua += L"/" + std::wstring(currentVersion.begin(), currentVersion.end());   // (ASCII)\n', ""),
            ("[7] a loopback override without its port / path check", U,
             "    if (digits == 0 || digits > 5 || at >= url.size() || url[at] != L'/') return false;   // a port, then the path",
             "    if (digits == 0) return false;"),
            # [6] app.txt
            ("[6] UPDATECHECK off / no / false ignored (only 0)", C, '            p.check = !(v == "0" || v == "off" || v == "no" || v == "false");', '            p.check = !(v == "0");'),
            ("[6] a hand-typed line (spaces) not read", C, "(l[t.size()] != '\\t' && l[t.size()] != ' ')) return false;", "l[t.size()] != '\\t') return false;"),
            ("[6] the LAST UPDATECHECK line wins", C, '        if (!seenCheck && updatePrefLine(l, "UPDATECHECK", v)) {', '        if (updatePrefLine(l, "UPDATECHECK", v)) {'),
            # every other line is kept as it is: a tag is matched whole, a foreign line is never dropped
            ("[6] a tag read as a prefix (UPDATECHECKS taken for UPDATECHECK)", C,
             "l.compare(0, t.size(), t) != 0 || (l[t.size()] != '\\t' && l[t.size()] != ' ')) return false;",
             "l.compare(0, t.size(), t) != 0) return false;"),
            ("[6] a foreign line dropped by the write (only the update lines kept)", C,
             'updatePrefLine(l, "UPDATESKIP", v)) {\n            if (!put) { for (const auto& w : with) body += w + "\\n"; put = true; }\n'
             '            continue;\n        }\n        body += l + "\\n";',
             'updatePrefLine(l, "UPDATESKIP", v)) {\n            if (!put) { for (const auto& w : with) body += w + "\\n"; put = true; }\n'
             '            continue;\n        }'),
            ("[6] the check written even when ON (a line nobody asked for)", C, '    if (!p.check) with.push_back("UPDATECHECK\\t0");',
             '    with.push_back(p.check ? "UPDATECHECK\\t1" : "UPDATECHECK\\t0");'),
        ]),
}


def dry():
    bad = 0
    for name, t in TARGETS.items():
        for (mname, rel, old, new) in t["mutants"]:
            text = open(os.path.join(cmut.REPO, rel), "rb").read().decode("utf-8").replace("\r\n", "\n")
            n = text.count(old)
            if n != 1:
                bad += 1
            print("  %s  [%s] %s -- applies %d time(s)" % ("ok  " if n == 1 else "FAIL", name, mname, n))
    print("CMUT_UPD --dry: %d mutant(s) do not apply exactly once" % bad)
    return 1 if bad else 0


def main():
    if "--dry" in sys.argv:
        sys.exit(dry())
    cmut.TARGETS.clear()
    cmut.TARGETS.update(TARGETS)
    cmut.main()


if __name__ == "__main__":
    main()

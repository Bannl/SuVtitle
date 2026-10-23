// =============================================================================
// SuVtitle tests/cpp/wg_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/wg_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the includes, SuVtitle's names (ProgIDs, vendor key, app name, Settings URI, the test root
// HKCU\Software\SuVtitle-Test\smoke-<pid>), --register-player in place of the gone --tray. Same
// checks, same count.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// wg_smoke -- wave WG's new headers, executed: ui/JsonR.h (the editor's structured messages), ui/TrLaunch.h
// (--play's command line, the paths' checks, the WM_COPYDATA payload, the test instance's names) and ui/TrAssoc.h
// (AudioPB as a Windows player: the registry it writes) -- the registry part ON A TEST KEY ONLY:
// HKCU\Software\SuVtitle-Test\smoke-<pid>, written, read back, unregistered, then deleted whole. Nothing of the user's
// registry is touched. /W4, no /utf-8, like the product. ASCII only.
// =============================================================================
#include "../../app/JsonR.h"
#include "../../app/TrLaunch.h"
#include "../../app/TrAssoc.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}
static std::string a(const std::wstring& w) { return pbtrf::toUtf8(w); }

static bool regString(HKEY hive, const std::wstring& key, const std::wstring& name, std::wstring& out) {
    wchar_t buf[2048] = {};
    DWORD cb = sizeof(buf), type = 0;
    if (::RegGetValueW(hive, key.c_str(), name.empty() ? nullptr : name.c_str(), RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, &type, buf, &cb) != ERROR_SUCCESS) return false;
    out = (type == REG_SZ) ? std::wstring(buf) : std::wstring();
    return true;
}
static bool regNone(HKEY hive, const std::wstring& key, const std::wstring& name) {
    DWORD type = 0, cb = 0;
    return ::RegGetValueW(hive, key.c_str(), name.c_str(), RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_NONE;
}
static bool keyExists(HKEY hive, const std::wstring& key) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(hive, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    ::RegCloseKey(k);
    return true;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("wg_smoke (JsonR.h, TrLaunch.h, TrAssoc.h on a TEST registry key)\n");

    // ---- 1. JsonR: the editor's edit, as WebView2 hands it over -------------------------------------------------
    std::printf("[1] JsonR\n");
    {
        const std::wstring msg = L"{\"type\":\"subEdit\",\"target\":\"audiopb\",\"gen\":12,\"rev\":3,\"remove\":[4,7],"
                                 L"\"add\":[{\"s\":1000,\"e\":2500,\"lang\":\"fr\",\"dstLang\":\"en\",\"pass\":false,"
                                 L"\"src\":\"Il a dit \\\"non\\\" \\\\ puis\\nparti\",\"dst\":\"He said \\u00abno\\u00bb\\t!\"}]}";
        pbjson::Value v;
        const bool parsed = pbjson::parse(msg, v);
        uint64_t gen = 0, rev = 0, s = 0, e = 0;
        const pbjson::Value* add = v.get(L"add");
        const pbjson::Value* rm = v.get(L"remove");
        ok("an edit message parses: its fields, its numbers, its arrays",
           parsed && v.str(L"type") == L"subEdit" && v.u64(L"gen", gen) && gen == 12 && v.u64(L"rev", rev) && rev == 3
           && rm && rm->isArr() && rm->a.size() == 2 && add && add->isArr() && add->a.size() == 1);
        const pbjson::Value& c = add->a[0];
        ok("...a text with an ESCAPED QUOTE, a backslash, a line break, \\u escapes and a tab reads exactly",
           c.u64(L"s", s) && s == 1000 && c.u64(L"e", e) && e == 2500 && c.str(L"src") == L"Il a dit \"non\" \\ puis\nparti"
           && c.str(L"dst") == L"He said \x00ABno\x00BB\t!" && c.flag(L"pass", true) == false);
        pbjson::Value bad;
        ok("malformed JSON is refused whole: an unterminated string, a trailing comma, a raw newline in a string, junk after",
           !pbjson::parse(L"{\"a\":\"x}", bad) && !pbjson::parse(L"{\"a\":1,}", bad) && !pbjson::parse(L"{\"a\":\"x\ny\"}", bad)
           && !pbjson::parse(L"{\"a\":1} x", bad) && !pbjson::parse(L"[1,2", bad) && !pbjson::parse(L"", bad));
        std::wstring deep;
        for (int i = 0; i < 40; ++i) deep += L"[";
        for (int i = 0; i < 40; ++i) deep += L"]";
        ok("...nesting deeper than 32 is refused (no recursion past it)", !pbjson::parse(deep, bad));
        ok("...a message over the size cap is refused before it is read", !pbjson::parse(std::wstring(100, L' ') + L"1", bad, 50));
        // review of WG #17: the number of VALUES is bounded too ("[0,0,...]" under the size cap used to build ~1 GB)
        auto zeros = [](size_t n) { std::wstring t = L"["; for (size_t i = 0; i < n; ++i) { if (i) t += L','; t += L'0'; } return t + L"]"; };
        ok("...a message holding more than 256 K values is refused (a bounded build, never a bad_alloc)",
           !pbjson::parse(zeros(pbjson::Reader::kMaxValues + 10), bad, 16u << 20));
        ok("...the largest real one (a Shift of 20 000 lines: ~200 000 values) still reads",
           pbjson::parse(zeros(200000), bad, 16u << 20) && bad.isArr() && bad.a.size() == 200000);
        uint64_t u = 0;
        pbjson::Value n;
        ok("u64 accepts whole non-negative numbers only (no -1, no 1.5, no 1e300)",
           pbjson::parse(L"{\"a\":-1,\"b\":1.5,\"c\":1e300,\"d\":42}", n) && !n.u64(L"a", u) && !n.u64(L"b", u) && !n.u64(L"c", u)
           && n.u64(L"d", u) && u == 42);
    }

    // ---- 2. TrLaunch: the command line, the paths, WM_COPYDATA -----------------------------------------------------
    std::printf("[2] TrLaunch\n");
    {
        const pblaunch::Args p1 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--play", L"C:\\v\\a.mkv", L"D:\\b.mp3", L"--register-player" });
        ok("--play takes the paths up to the next switch", p1.play && p1.paths.size() == 2 && p1.paths[1] == L"D:\\b.mp3" && p1.registerPlayer);
        const pblaunch::Args p2 = pblaunch::parseArgs({ L"x", L"--play" });
        ok("--play alone: the player, empty", p2.play && p2.paths.empty());
        const pblaunch::Args p3 = pblaunch::parseArgs({ L"x", L"--test-instance", L"wg-1", L"--unregister-player" });
        ok("--test-instance <tag> and --unregister-player", p3.testTag == L"wg-1" && !p3.badTag && p3.unregister);
        ok("...a tag outside [a-z0-9-]{1,24} is refused (the process would exit 3)",
           pblaunch::parseArgs({ L"x", L"--test-instance", L"WG" }).badTag && pblaunch::parseArgs({ L"x", L"--test-instance", L"a/b" }).badTag
           && pblaunch::parseArgs({ L"x", L"--test-instance" }).badTag && pblaunch::parseArgs({ L"x", L"--test-instance", std::wstring(25, L'a') }).badTag);
        const std::vector<std::wstring> split = pblaunch::splitCommandLine(L"\"C:\\Program Files\\NEXTALE\\SuVtitle\\SuVtitle.exe\" --play \"D:\\Films\\Le film & co (2026).mkv\"");
        ok("Explorer's command line (\"<exe>\" --play \"%1\"): a path with spaces, & and parentheses stays ONE argument",
           split.size() == 3 && split[2] == L"D:\\Films\\Le film & co (2026).mkv");
        // classify, on real files in a temp folder
        wchar_t tmp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring(tmp) + L"wg_smoke_" + std::to_wstring(::GetCurrentProcessId());
        ::CreateDirectoryW(dir.c_str(), nullptr);
        auto touch = [&](const std::wstring& leaf) { const std::wstring f = dir + L"\\" + leaf; HANDLE h = ::CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); return f; };
        const std::wstring mkv = touch(L"film & co.mkv"), srt = touch(L"film.fr.srt"), exe = touch(L"evil.exe"), txt = touch(L"notes.txt");
        using K = pblaunch::Kind;
        ok("classify: a media file, a subtitle file, a folder", pblaunch::classify(mkv) == K::Media && pblaunch::classify(srt) == K::Subtitle
           && pblaunch::classify(dir) == K::Folder);
        ok("classify REFUSES: an .exe, a .txt, a missing file, a relative path, a wildcard, a quote, a device path, empty",
           pblaunch::classify(exe) == K::Refused && pblaunch::classify(txt) == K::Refused && pblaunch::classify(dir + L"\\nope.mkv") == K::Refused
           && pblaunch::classify(L"film.mkv") == K::Refused && pblaunch::classify(dir + L"\\*.mkv") == K::Refused
           && pblaunch::classify(L"C:\\a\"b.mkv") == K::Refused && pblaunch::classify(L"\\\\?\\C:\\x.mkv") == K::Refused
           && pblaunch::classify(L"") == K::Refused);
        for (const auto& f : { mkv, srt, exe, txt }) ::DeleteFileW(f.c_str());
        ::RemoveDirectoryW(dir.c_str());
        {   // a RELATIVE path that DOES exist (in the current folder) is still refused: only absolute paths play
            const std::wstring rel = L"wg_rel_test_" + std::to_wstring(::GetCurrentProcessId()) + L".mkv";
            HANDLE h = ::CreateFileW(rel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            const bool made = h != INVALID_HANDLE_VALUE;
            if (made) ::CloseHandle(h);
            ok("classify REFUSES a relative path even when that file exists where the process stands",
               made && ::GetFileAttributesW(rel.c_str()) != INVALID_FILE_ATTRIBUTES && pblaunch::classify(rel) == K::Refused);
            ::DeleteFileW(rel.c_str());
        }
        {   // review of WG #16: the isolation check compares FOLDERS AS FILES (volume + id), not strings
            wchar_t tmpDir[MAX_PATH] = {};
            ::GetTempPathW(MAX_PATH, tmpDir);
            const std::wstring a = std::wstring(tmpDir) + L"wg_iso_a_" + std::to_wstring(::GetCurrentProcessId());
            const std::wstring b = std::wstring(tmpDir) + L"wg_iso_b_" + std::to_wstring(::GetCurrentProcessId());
            const std::wstring j = std::wstring(tmpDir) + L"wg_iso_j_" + std::to_wstring(::GetCurrentProcessId());
            ::CreateDirectoryW(a.c_str(), nullptr);
            ::CreateDirectoryW(b.c_str(), nullptr);
            ok("sameDir: a folder is itself -- with a trailing slash, in another case, through \\\\?\\",
               pblaunch::sameDir(a, a + L"\\") && pblaunch::sameDir(a, pbtrf::lowerW(a)) && pblaunch::sameDir(a, L"\\\\?\\" + a));
            ok("...two different folders are not the same", !pblaunch::sameDir(a, b));
            ok("...a folder that cannot be opened counts as the SAME (no isolation proven: the test instance refuses)",
               pblaunch::sameDir(a, a + L"\\no-such-folder") && pblaunch::sameDir(L"", b));
            // a JUNCTION to `a` (mklink /J needs no privilege): the same folder by another name
            const std::wstring cmd = L"cmd /c mklink /J \"" + j + L"\" \"" + a + L"\" >nul 2>&1";
            const int rc = _wsystem(cmd.c_str());
            if (rc == 0 && ::GetFileAttributesW(j.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ok("...a JUNCTION to the folder is the same folder (the alias a string compare missed)", pblaunch::sameDir(a, j));
                ::RemoveDirectoryW(j.c_str());
            } else {
                std::printf("  NOTE  no junction could be made here (mklink /J rc %d): that check skipped\n", rc);
            }
            ::RemoveDirectoryW(a.c_str());
            ::RemoveDirectoryW(b.c_str());
        }
        // the payload
        const std::vector<std::wstring> paths = { L"C:\\v\\a.mkv", L"D:\\\x00E9t\x00E9\\b \x7535\x5F71.mp4" };
        const std::vector<wchar_t> buf = pblaunch::packPlay(paths);
        std::vector<std::wstring> back;
        ok("WM_COPYDATA: the paths round-trip (UTF-16: accents, CJK)", pblaunch::unpackPlay(buf.data(), buf.size() * sizeof(wchar_t), back)
           && back == paths);
        const std::vector<wchar_t> none = pblaunch::packPlay({});
        ok("...an empty list (--play alone) round-trips too", pblaunch::unpackPlay(none.data(), none.size() * sizeof(wchar_t), back) && back.empty());
        std::vector<wchar_t> noEnd(buf.begin(), buf.end() - 1);
        std::vector<wchar_t> junk = buf;
        junk.push_back(L'x'); junk.push_back(L'\0');
        std::vector<wchar_t> hole = { L'a', L'\0', L'\0', L'b', L'\0', L'\0' };
        ok("...refused WHOLE: no closing empty string, junk after it, an odd byte count, too big, null",
           !pblaunch::unpackPlay(noEnd.data(), noEnd.size() * sizeof(wchar_t), back) && !pblaunch::unpackPlay(junk.data(), junk.size() * sizeof(wchar_t), back)
           && !pblaunch::unpackPlay(hole.data(), hole.size() * sizeof(wchar_t), back)
           && !pblaunch::unpackPlay(buf.data(), buf.size() * sizeof(wchar_t) - 1, back)
           && !pblaunch::unpackPlay(buf.data(), pblaunch::kCopyMaxBytes + 2, back) && !pblaunch::unpackPlay(nullptr, 8, back));
        std::vector<std::wstring> many(2500, L"C:\\x.mkv");
        const std::vector<wchar_t> capped = pblaunch::packPlay(many);
        ok("...at most 2000 paths go (the queue's cap)", pblaunch::unpackPlay(capped.data(), capped.size() * sizeof(wchar_t), back) && back.size() == 2000);
        ok("this smoke is NOT a test instance (no --test-instance): the product's names", !pblaunch::isTestInstance()
           && pblaunch::suffixed(L"SuVtitle.SingleInstance") == L"SuVtitle.SingleInstance");
        ok("the subtitle extensions are SubtitleReader.h's four", pblaunch::isSubtitleExtL(L"SRT") && pblaunch::isSubtitleExtL(L"ass")
           && pblaunch::isSubtitleExtL(L"ssa") && pblaunch::isSubtitleExtL(L"vtt") && !pblaunch::isSubtitleExtL(L"sub"));
    }

    // ---- 3. TrAssoc: the registration, on a TEST key -------------------------------------------------------------------
    std::printf("[3] TrAssoc (HKCU\\Software\\SuVtitle-Test\\smoke-<pid> only)\n");
    {
        // what the user's own registry holds BEFORE (compared after: this smoke must not change it)
        const bool userHadVideo = keyExists(HKEY_CURRENT_USER, L"Software\\Classes\\NEXTALE.SuVtitle.Video");
        const bool userHadVendor = keyExists(HKEY_CURRENT_USER, L"Software\\NEXTALE\\SuVtitle");
        const std::wstring exe = L"C:\\Program Files\\NEXTALE\\SuVtitle\\SuVtitle.exe";
        const auto es = pbassoc::entries(exe, L"Software");
        size_t owp = 0, fa = 0;
        for (const auto& e : es) { if (e.key.find(L"\\OpenWithProgids") != std::wstring::npos) ++owp; if (e.key.find(L"\\FileAssociations") != std::wstring::npos) ++fa; }
        ok("the product's list: 2 ProgIDs x 5 values, 32 OpenWithProgids, 3 capabilities + 32 associations, 1 RegisteredApplications",
           es.size() == 10 + 32 + 3 + 32 + 1 && owp == 32 && fa == 32, std::to_string(es.size()));
        ok("the command Explorer runs: the exe quoted, --play, the file quoted", pbassoc::openCommand(exe) == L"\"" + exe + L"\" --play \"%1\"");
        ok("video and audio families: .mkv -> Video, .flac -> Audio", std::wstring(pbassoc::progIdFor(L"mkv")) == pbassoc::kProgVideo
           && std::wstring(pbassoc::progIdFor(L"FLAC")) == pbassoc::kProgAudio);
        bool regOk = false;
        for (const auto& e : es) if (e.key == L"Software\\RegisteredApplications" && e.name == L"SuVtitle"
                                     && e.data == L"Software\\NEXTALE\\SuVtitle\\Capabilities") regOk = true;
        ok("RegisteredApplications points at the Capabilities key, relative to the hive (what Settings reads)", regOk);
        ok("Settings is opened on the registered name", std::wstring(pbassoc::kSettingsUri) == L"ms-settings:defaultapps?registeredAppUser=SuVtitle");

        const std::wstring testRoot = L"Software\\SuVtitle-Test\\smoke-" + std::to_wstring(::GetCurrentProcessId());
        std::wstring err;
        const bool applied = pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, testRoot), err);
        std::wstring v1, v2, v3, v4, v5;
        ok("written UNDER THE TEST KEY, read back: the ProgID's command, its icon, a capability, an association, the registered name",
           applied && regString(HKEY_CURRENT_USER, testRoot + L"\\Classes\\NEXTALE.SuVtitle.Video\\shell\\open\\command", L"", v1)
           && v1 == L"\"" + exe + L"\" --play \"%1\""
           && regString(HKEY_CURRENT_USER, testRoot + L"\\Classes\\NEXTALE.SuVtitle.Audio\\DefaultIcon", L"", v2) && v2 == exe + L",0"
           && regString(HKEY_CURRENT_USER, testRoot + L"\\NEXTALE\\SuVtitle\\Capabilities", L"ApplicationName", v3) && v3 == L"SuVtitle"
           && regString(HKEY_CURRENT_USER, testRoot + L"\\NEXTALE\\SuVtitle\\Capabilities\\FileAssociations", L".mkv", v4) && v4 == pbassoc::kProgVideo
           && regString(HKEY_CURRENT_USER, testRoot + L"\\RegisteredApplications", L"SuVtitle", v5) && v5 == testRoot + L"\\NEXTALE\\SuVtitle\\Capabilities",
           a(err + L" | " + v1));
        ok("...each media extension's OpenWithProgids holds our ProgID (a REG_NONE value)",
           regNone(HKEY_CURRENT_USER, testRoot + L"\\Classes\\.mp4\\OpenWithProgids", pbassoc::kProgVideo)
           && regNone(HKEY_CURRENT_USER, testRoot + L"\\Classes\\.mp3\\OpenWithProgids", pbassoc::kProgAudio));
        // someone else's value in a shared key must survive our unregister
        HKEY other = nullptr;
        ::RegCreateKeyExW(HKEY_CURRENT_USER, (testRoot + L"\\Classes\\.mp4\\OpenWithProgids").c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &other, nullptr);
        if (other) { ::RegSetValueExW(other, L"VLC.mp4", 0, REG_NONE, nullptr, 0); ::RegCloseKey(other); }
        pbassoc::unregister(HKEY_CURRENT_USER, testRoot);
        ok("unregister: the ProgIDs and the vendor key are gone, our OpenWithProgids and RegisteredApplications values gone",
           !keyExists(HKEY_CURRENT_USER, testRoot + L"\\Classes\\NEXTALE.SuVtitle.Video")
           && !keyExists(HKEY_CURRENT_USER, testRoot + L"\\Classes\\NEXTALE.SuVtitle.Audio")
           && !keyExists(HKEY_CURRENT_USER, testRoot + L"\\NEXTALE\\SuVtitle")
           && !regNone(HKEY_CURRENT_USER, testRoot + L"\\Classes\\.mp4\\OpenWithProgids", pbassoc::kProgVideo)
           && !regString(HKEY_CURRENT_USER, testRoot + L"\\RegisteredApplications", L"SuVtitle", v5));
        ok("...ANOTHER player's value in the same shared key is untouched (VLC.mp4)",
           regNone(HKEY_CURRENT_USER, testRoot + L"\\Classes\\.mp4\\OpenWithProgids", L"VLC.mp4"));
        // the test key goes, whole; and the user's own registry was never written
        const LONG del = ::RegDeleteTreeW(HKEY_CURRENT_USER, testRoot.c_str());
        ::RegDeleteKeyExW(HKEY_CURRENT_USER, testRoot.c_str(), KEY_WOW64_64KEY, 0);
        HKEY parent = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SuVtitle-Test", 0, KEY_READ | KEY_WOW64_64KEY, &parent) == ERROR_SUCCESS) {
            DWORD subkeys = 0, values = 0;
            ::RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            ::RegCloseKey(parent);
            if (subkeys == 0 && values == 0) ::RegDeleteKeyExW(HKEY_CURRENT_USER, L"Software\\SuVtitle-Test", KEY_WOW64_64KEY, 0);
        }
        ok("the test key is deleted whole (nothing left behind)", del == ERROR_SUCCESS && !keyExists(HKEY_CURRENT_USER, testRoot));
        ok("the USER's own registration keys are exactly as they were before this smoke (it writes under its test root only)",
           keyExists(HKEY_CURRENT_USER, L"Software\\Classes\\NEXTALE.SuVtitle.Video") == userHadVideo
           && keyExists(HKEY_CURRENT_USER, L"Software\\NEXTALE\\SuVtitle") == userHadVendor);
    }

    std::printf("\nwg_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

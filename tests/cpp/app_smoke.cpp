// =============================================================================
// app_smoke -- NEW (S-HOST, 2026-10-23): what SuVtitle added to the ported code, executed.
//   [1] AudioPBImport.h -- the FIRST-RUN IMPORT of AudioPB's player data, on scratch trees under %TEMP%: what is
//       copied (byte for byte, write time kept), what is NOT (proxy\, *.tmp, other files), never an overwrite, never a
//       write in AudioPB's folders, once only (the marker = the log), nothing to import, no destination.
//   [2] AppConfig.h -- app.txt (WINPOS: AudioPB's 5 fields + maximized; unknown lines kept; one header) and
//       webRelPath (the page's files: sub-folders yes, climbing out / devices / streams / odd names no).
//   [3] the pair the app reads from the ENGINE's config (engine.txt, S-ENG's "# SuVtitle Engine config v1" header):
//       pbplayer::pairFromTranslatorCfg, unchanged, reads it.
//   [4] TrAssoc.h -- SuVtitle's unregister keeps what is not its own under NEXTALE\SuVtitle (an installer's value) and
//       another NEXTALE product's key; drops the vendor key / NEXTALE when empty. ON A TEST KEY ONLY:
//       HKCU\Software\SuVtitle-Test\appsmoke-<pid>, deleted at the end.
//   [5] TrLaunch.h -- --register-player, the WM_COPYDATA tag "SVTP", the product's single-instance names.
// FIX ROUND 2026-10-23: [1] + RV2-7 (stray *.import-tmp swept at every start, marker or not; nothing else touched).
// /W4, no /utf-8, like the product. ASCII only.
// =============================================================================
#include "../../app/AudioPBImport.h"
#include "../../app/AppConfig.h"
#include "../../app/TrAssoc.h"
#include "../../app/TrLaunch.h"
#include "../../app/TrPlayer.h"     // pbplayer::pairFromTranslatorCfg (the bridge reads engine.txt with it)

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}

static bool put(const std::wstring& p, const std::string& bytes) {
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL r = ::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    ::CloseHandle(h);
    return r && w == bytes.size();
}
static std::string get(const std::wstring& p) {
    std::string s;
    pbtrf::readFileAll(p, s);
    return s;
}
static uint64_t mtimeOf(const std::wstring& p) {
    uint64_t s = 0, m = 0;
    return pbtrf::fileIdentity(p, s, m) ? m : 0;
}
// every file under a folder (recursive): name -> bytes + write time, to prove a tree untouched
static void snapshot(const std::wstring& dir, const std::wstring& rel, std::vector<std::pair<std::wstring, std::string>>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) snapshot(dir + L"\\" + n, rel + n + L"\\", out);
        else out.push_back({ rel + n, get(dir + L"\\" + n) + "|" + std::to_string(mtimeOf(dir + L"\\" + n)) });
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}
static void rmTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmTree(p);
            else { ::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); ::DeleteFileW(p.c_str()); }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(dir.c_str());
}
static bool keyExists(HKEY hive, const std::wstring& key) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(hive, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    ::RegCloseKey(k);
    return true;
}
static bool setValue(HKEY hive, const std::wstring& key, const std::wstring& name, const std::wstring& data) {
    HKEY k = nullptr;
    if (::RegCreateKeyExW(hive, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LONG r = ::RegSetValueExW(k, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                                    static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
static bool valueExists(HKEY hive, const std::wstring& key, const std::wstring& name) {
    DWORD type = 0, cb = 0;
    return ::RegGetValueW(hive, key.c_str(), name.c_str(), RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, &type, nullptr, &cb) == ERROR_SUCCESS;
}

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("app_smoke (SuVtitle: AudioPBImport.h, AppConfig.h, engine.txt pair, TrAssoc unregister, TrLaunch)\n");
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring base = std::wstring(tmp) + L"suv_app_smoke_" + std::to_wstring(::GetCurrentProcessId());
    rmTree(base);
    ::CreateDirectoryW(base.c_str(), nullptr);

    // ---- [1] the first-run import --------------------------------------------------------------------------------
    std::printf("[1] AudioPBImport (scratch trees under %%TEMP%%)\n");
    {
        pbimport::Roots r;
        r.fromRoaming = base + L"\\roaming\\AudioPB";
        r.fromFiles   = base + L"\\local\\AudioPB\\Translator files";
        r.toRoaming   = base + L"\\roaming\\SuVtitle";
        r.toFiles     = base + L"\\local\\SuVtitle\\Files";
        ::CreateDirectoryW((base + L"\\roaming").c_str(), nullptr);
        ::CreateDirectoryW((base + L"\\local").c_str(), nullptr);
        ::CreateDirectoryW(r.fromRoaming.c_str(), nullptr);
        ::CreateDirectoryW((base + L"\\local\\AudioPB").c_str(), nullptr);
        ::CreateDirectoryW(r.fromFiles.c_str(), nullptr);
        ::CreateDirectoryW((r.fromFiles + L"\\proxy").c_str(), nullptr);
        const std::string queue = "# AudioPB player v1\nOPT\ttranslate\t1\nCUR\t-1\nEND\t0\n";
        const std::string resume = "# AudioPB player resume v1\nPOS\td:\\\\f\\\\a.mkv\t7\t6\t20000\t600000\t1760000000\n";
        const std::string cuesA = std::string(pbtrf::recordHeader()) + "\nSRC\tD:\\\\F\\\\a.mkv\t7\t6\nEND\t0\n";
        const std::string cuesB = std::string(pbtrf::recordHeader()) + "\nSRC\tD:\\\\F\\\\b.mkv\t8\t9\nEND\t0\n";
        const std::string reg = "# AudioPB Translator: subtitle files written by AudioPB (path, size, write time, media)\n"
                                "d:\\\\f\\\\a.fr.srt\t10\t20\td:\\\\f\\\\a.mkv\n";
        bool made = put(r.fromRoaming + L"\\player.txt", queue) && put(r.fromRoaming + L"\\player-resume.txt", resume)
                 && put(r.fromRoaming + L"\\translator.txt", "# AudioPB Translator config v2\nLANGA\tfr\nLANGB\ten\n")
                 && put(r.fromRoaming + L"\\settings.txt", "BUFFER\t256\n")
                 && put(r.fromFiles + L"\\1b88fa707786df95.cues", cuesA) && put(r.fromFiles + L"\\448383df7fbdda98.cues", cuesB)
                 && put(r.fromFiles + L"\\srt-written.txt", reg) && put(r.fromFiles + L"\\0123456789abcdef.cues.tmp", "torn")
                 && put(r.fromFiles + L"\\notes.txt", "x") && put(r.fromFiles + L"\\proxy\\a-12.wav", "RIFF");
        // an older write time on one record: CopyFileW must carry it (the cache's keepNewest orders by it)
        {
            HANDLE h = ::CreateFileW((r.fromFiles + L"\\1b88fa707786df95.cues").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME ft{}; ft.dwHighDateTime = 0x01D9A000; ft.dwLowDateTime = 0x12345678;
            made = made && h != INVALID_HANDLE_VALUE && ::SetFileTime(h, nullptr, nullptr, &ft);
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        }
        ok("the AudioPB side is set up (2 player files, 2 records, the registry, + a .tmp, a .txt, a proxy WAV, translator.txt, settings.txt)", made);
        std::vector<std::pair<std::wstring, std::string>> before, after;
        snapshot(base + L"\\roaming\\AudioPB", L"", before);
        snapshot(base + L"\\local\\AudioPB", L"", before);

        const pbimport::Result res = pbimport::run(r);
        ok("first run: it RAN, copied 5 (player.txt, player-resume.txt, srt-written.txt, 2 records), kept 0, failed 0",
           res.ran && res.copied == 5 && res.kept == 0 && res.failed == 0,
           std::to_string(res.copied) + "/" + std::to_string(res.kept) + "/" + std::to_string(res.failed));
        ok("...byte for byte", get(r.toRoaming + L"\\player.txt") == queue && get(r.toRoaming + L"\\player-resume.txt") == resume
           && get(r.toFiles + L"\\1b88fa707786df95.cues") == cuesA && get(r.toFiles + L"\\448383df7fbdda98.cues") == cuesB
           && get(r.toFiles + L"\\srt-written.txt") == reg);
        ok("...a record keeps its WRITE TIME (the cache's age order survives the copy)",
           mtimeOf(r.toFiles + L"\\1b88fa707786df95.cues") == mtimeOf(r.fromFiles + L"\\1b88fa707786df95.cues")
           && mtimeOf(r.toFiles + L"\\1b88fa707786df95.cues") != 0);
        ok("NOT copied: player-audio.txt (absent), translator.txt + settings.txt (not the player's), *.cues.tmp, notes.txt, proxy\\",
           !pbtrf::fileExists(r.toRoaming + L"\\player-audio.txt") && !pbtrf::fileExists(r.toRoaming + L"\\translator.txt")
           && !pbtrf::fileExists(r.toRoaming + L"\\settings.txt") && !pbtrf::fileExists(r.toFiles + L"\\0123456789abcdef.cues.tmp")
           && !pbtrf::fileExists(r.toFiles + L"\\notes.txt") && ::GetFileAttributesW((r.toFiles + L"\\proxy").c_str()) == INVALID_FILE_ATTRIBUTES);
        std::vector<std::pair<std::wstring, std::string>> leftovers;
        snapshot(r.toFiles, L"", leftovers);
        snapshot(r.toRoaming, L"", leftovers);
        bool noTmp = true;
        for (const auto& f : leftovers) if (f.first.find(L".import-tmp") != std::wstring::npos) noTmp = false;
        ok("no .import-tmp left behind (each copy renamed into place)", noTmp && leftovers.size() == 6, std::to_string(leftovers.size()));
        snapshot(base + L"\\roaming\\AudioPB", L"", after);
        snapshot(base + L"\\local\\AudioPB", L"", after);
        ok("AudioPB's folders are EXACTLY as before (same files, same bytes, same write times: copied, never moved)", before == after);
        const std::string log = get(res.marker);
        ok("the marker IS the log: one line per file + DONE",
           log.rfind("# SuVtitle: AudioPB's player data imported", 0) == 0 && log.find("COPIED\tplayer.txt\n") != std::string::npos
           && log.find("COPIED\t1b88fa707786df95.cues\n") != std::string::npos && log.find("COPIED\tsrt-written.txt\n") != std::string::npos
           && log.find("DONE\tcopied 5\tkept 0\tfailed 0\n") != std::string::npos, log.substr(0, 300));

        // second run: the marker says it ran -- a new AudioPB file is NOT taken, nothing is touched
        put(r.fromRoaming + L"\\player-audio.txt", "# AudioPB player audio v1\n");
        put(r.toRoaming + L"\\player.txt", "edited by SuVtitle since\n");
        const pbimport::Result res2 = pbimport::run(r);
        ok("second run: does NOT run (the marker), takes nothing new, overwrites nothing",
           !res2.ran && !pbtrf::fileExists(r.toRoaming + L"\\player-audio.txt") && get(r.toRoaming + L"\\player.txt") == "edited by SuVtitle since\n");

        // FIX ROUND (2026-10-23, review RV2-7): a crash mid-copy left "<name>.import-tmp" files whose SOURCES are gone
        // since (AudioPB evicted them): swept at the next start, marker or not -- and nothing else is touched
        const bool strays = put(r.toFiles + L"\\0f0f0f0f0f0f0f0f.cues.import-tmp", "torn") && put(r.toRoaming + L"\\player.txt.import-tmp", "torn")
                         && put(r.toFiles + L"\\keep.import-tmp.txt", "not ours") && put(r.toFiles + L"\\keep.cues", "a record")
                         && ::CreateDirectoryW((r.toFiles + L"\\dir.import-tmp").c_str(), nullptr) != FALSE;
        const pbimport::Result resS = pbimport::run(r);
        ok("RV2-7: stray *.import-tmp (a record's, a player file's) SWEPT at the next start -- with the marker there (the import itself does not run)",
           strays && !resS.ran && !pbtrf::fileExists(r.toFiles + L"\\0f0f0f0f0f0f0f0f.cues.import-tmp")
           && !pbtrf::fileExists(r.toRoaming + L"\\player.txt.import-tmp"));
        ok("RV2-7: ...and only those: another suffix, a record, a FOLDER named *.import-tmp all stay",
           get(r.toFiles + L"\\keep.import-tmp.txt") == "not ours" && get(r.toFiles + L"\\keep.cues") == "a record"
           && (::GetFileAttributesW((r.toFiles + L"\\dir.import-tmp").c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0
           && ::GetFileAttributesW((r.toFiles + L"\\dir.import-tmp").c_str()) != INVALID_FILE_ATTRIBUTES);
        // >>> new API (the fix round's proof strips this block to build the file over the code BEFORE the fix)
        ok("RV2-7: ...Result.swept says 2", resS.swept == 2, std::to_string(resS.swept));
        // <<< new API
        ::DeleteFileW((r.toFiles + L"\\keep.import-tmp.txt").c_str());
        ::DeleteFileW((r.toFiles + L"\\keep.cues").c_str());
        ::RemoveDirectoryW((r.toFiles + L"\\dir.import-tmp").c_str());

        // never an overwrite: a SuVtitle file already there (no marker yet) stays, and says KEPT
        ::DeleteFileW(res.marker.c_str());
        const pbimport::Result res3 = pbimport::run(r);
        ok("no marker but SuVtitle files there: each one KEPT (never overwritten), only what is missing comes (player-audio.txt)",
           res3.ran && res3.copied == 1 && res3.kept == 5 && res3.failed == 0 && get(r.toRoaming + L"\\player.txt") == "edited by SuVtitle since\n"
           && get(r.toRoaming + L"\\player-audio.txt") == "# AudioPB player audio v1\n"
           && get(res3.marker).find("KEPT\tplayer.txt\n") != std::string::npos,
           std::to_string(res3.copied) + "/" + std::to_string(res3.kept));

        // nothing to import at all: it runs once, says so, and never again
        pbimport::Roots e;
        e.fromRoaming = base + L"\\empty\\AudioPB";
        e.fromFiles = base + L"\\empty\\AudioPB\\Translator files";
        e.toRoaming = base + L"\\empty_to";
        e.toFiles = base + L"\\empty_to_local\\Files";
        ::CreateDirectoryW((base + L"\\empty").c_str(), nullptr);
        const pbimport::Result r4 = pbimport::run(e);
        ok("no AudioPB data: it runs, copies nothing, and writes its marker (decided once)",
           r4.ran && r4.copied == 0 && r4.failed == 0 && pbtrf::fileExists(r4.marker)
           && get(r4.marker).find("DONE\tcopied 0\tkept 0\tfailed 0\n") != std::string::npos && !pbimport::run(e).ran);
        ok("no AudioPB folder created by the import (it only reads that side)",
           ::GetFileAttributesW(e.fromRoaming.c_str()) == INVALID_FILE_ATTRIBUTES);
        pbimport::Roots none;
        ok("no destination (no %APPDATA%): nothing runs", !pbimport::run(none).ran);
        ok("productRoots(): %APPDATA%\\AudioPB -> %APPDATA%\\SuVtitle, ...\\Translator files -> %LOCALAPPDATA%\\SuVtitle\\Files", [] {
            const pbimport::Roots p = pbimport::productRoots();
            auto ends = [](const std::wstring& s, const wchar_t* t) { const size_t n = ::wcslen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; };
            return ends(p.fromRoaming, L"\\AudioPB") && ends(p.toRoaming, L"\\SuVtitle")
                && ends(p.fromFiles, L"\\AudioPB\\Translator files") && ends(p.toFiles, L"\\SuVtitle\\Files");
        }());
    }

    // ---- [2] app.txt + the page's files -----------------------------------------------------------------------
    std::printf("[2] AppConfig (app.txt, webRelPath)\n");
    {
        pbapp::WinPos p;
        ok("AudioPB's 5-field WINPOS reads (topmost on, not maximized)",
           pbapp::parseWinPos("WINPOS\t-1880\t40\t1000\t720\t1\n", p) && p.x == -1880 && p.y == 40 && p.w == 1000 && p.h == 720
           && p.topmost && !p.maximized);
        pbapp::WinPos q{ 10, 20, 1100, 720, false, true };
        const std::string body = pbapp::packWinPos("# SuVtitle app v1\nWINPOS\t1\t2\t300\t300\t0\t0\nFUTURE\tkeep me\n# a stray header\n", q);
        pbapp::WinPos back;
        ok("pack: ONE header, ONE WINPOS (the new one), an unknown line kept", body == "# SuVtitle app v1\nWINPOS\t10\t20\t1100\t720\t0\t1\nFUTURE\tkeep me\n", body);
        ok("...and it reads back (maximized kept)", pbapp::parseWinPos(body, back) && back.x == 10 && back.w == 1100 && back.maximized && !back.topmost);
        ok("a size no window could be is refused (200 x 150 floor, absurd sizes)",
           !pbapp::parseWinPos("WINPOS\t0\t0\t150\t900\t0\n", p) && !pbapp::parseWinPos("WINPOS\t0\t0\t900\t100\t0\n", p)
           && !pbapp::parseWinPos("WINPOS\t0\t0\t999999\t900\t0\n", p) && !pbapp::parseWinPos("", p) && !pbapp::parseWinPos("WINPOS\tx\n", p));
        ok("webRelPath: the page and its files, sub-folders included",
           pbapp::webRelPath(L"") == L"index.html" && pbapp::webRelPath(L"index.html?v=3#top") == L"index.html"
           && pbapp::webRelPath(L"fonts/example.woff2") == L"fonts\\example.woff2" && pbapp::webRelPath(L"logo.png") == L"logo.png");
        ok("webRelPath REFUSES: climbing out, empty segments, a drive, a stream, a backslash, %-escapes, odd names, devices",
           pbapp::webRelPath(L"../x.html").empty() && pbapp::webRelPath(L"a/../../x").empty() && pbapp::webRelPath(L"./x").empty()
           && pbapp::webRelPath(L"a//b").empty() && pbapp::webRelPath(L"/x").empty() && pbapp::webRelPath(L"C:/x").empty()
           && pbapp::webRelPath(L"x.html:stream").empty() && pbapp::webRelPath(L"a\\b").empty() && pbapp::webRelPath(L"%2e%2e/x").empty()
           && pbapp::webRelPath(L"x.").empty() && pbapp::webRelPath(L"x ").empty() && pbapp::webRelPath(L"COM1").empty()
           && pbapp::webRelPath(L"nul.txt").empty() && pbapp::webRelPath(L"fonts/Con").empty() && pbapp::webRelPath(L"LPT9.html").empty()
           && pbapp::webRelPath(std::wstring(129, L'a')).empty());
        ok("...but a name that only STARTS like a device is a file (CONSOLE.html, COM10.png, nullify.js)",
           pbapp::webRelPath(L"CONSOLE.html") == L"CONSOLE.html" && pbapp::webRelPath(L"COM10.png") == L"COM10.png"
           && pbapp::webRelPath(L"nullify.js") == L"nullify.js");
    }

    // ---- [3] the pair from the ENGINE's config -------------------------------------------------------------------
    std::printf("[3] engine.txt's pair (S-ENG: \"# SuVtitle Engine config v1\")\n");
    {
        std::string la = "en", lb = "zh", m = "auto";
        const bool got = pbplayer::pairFromTranslatorCfg("# SuVtitle Engine config v1\r\nPRESET\tbalanced\r\nMODE\tb2a\r\n"
                                                         "LANGA\tfr\r\nLANGB\tzh-tw\r\nDEVICE\tcpu\r\n", la, lb, m);
        ok("LANGA / LANGB / MODE read under the engine's header (DEVICE and the rest ignored)", got && la == "fr" && lb == "zh-tw" && m == "b2a");
        std::string a2 = "en", b2 = "zh", m2 = "auto";
        ok("an engine.txt without the pair keeps the defaults (the engine's echo decides later)",
           !pbplayer::pairFromTranslatorCfg("# SuVtitle Engine config v1\nPRESET\tlight\n", a2, b2, m2) && a2 == "en" && b2 == "zh");
    }

    // ---- [4] TrAssoc: SuVtitle's unregister, on a TEST key -------------------------------------------------------
    std::printf("[4] TrAssoc unregister (HKCU\\Software\\SuVtitle-Test\\appsmoke-<pid> only)\n");
    {
        const std::wstring root = L"Software\\SuVtitle-Test\\appsmoke-" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring exe = L"C:\\Program Files\\NEXTALE\\SuVtitle\\SuVtitle.exe";
        std::wstring err;
        // round 1: an installer's own value under NEXTALE\SuVtitle, and another NEXTALE product next to it
        bool w = pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, root), err)
              && setValue(HKEY_CURRENT_USER, root + L"\\NEXTALE\\SuVtitle", L"InstallDir", L"C:\\Program Files\\NEXTALE\\SuVtitle")
              && setValue(HKEY_CURRENT_USER, root + L"\\NEXTALE\\AudioPB Player", L"x", L"y");
        pbassoc::unregister(HKEY_CURRENT_USER, root);
        ok("unregister: Capabilities gone, the vendor key KEPT for the installer's value, the other product untouched",
           w && !keyExists(HKEY_CURRENT_USER, root + L"\\NEXTALE\\SuVtitle\\Capabilities")
           && valueExists(HKEY_CURRENT_USER, root + L"\\NEXTALE\\SuVtitle", L"InstallDir")
           && keyExists(HKEY_CURRENT_USER, root + L"\\NEXTALE\\AudioPB Player"));
        // round 2: nothing else there -> the vendor key and NEXTALE go too
        ::RegDeleteTreeW(HKEY_CURRENT_USER, (root + L"\\NEXTALE").c_str());
        ::RegDeleteKeyExW(HKEY_CURRENT_USER, (root + L"\\NEXTALE").c_str(), KEY_WOW64_64KEY, 0);
        w = pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, root), err);
        pbassoc::unregister(HKEY_CURRENT_USER, root);
        ok("...nothing else there: NEXTALE\\SuVtitle and NEXTALE go with it", w && !keyExists(HKEY_CURRENT_USER, root + L"\\NEXTALE"));
        const LONG del = ::RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
        ::RegDeleteKeyExW(HKEY_CURRENT_USER, root.c_str(), KEY_WOW64_64KEY, 0);
        HKEY parent = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SuVtitle-Test", 0, KEY_READ | KEY_WOW64_64KEY, &parent) == ERROR_SUCCESS) {
            DWORD subkeys = 0, values = 0;
            ::RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            ::RegCloseKey(parent);
            if (subkeys == 0 && values == 0) ::RegDeleteKeyExW(HKEY_CURRENT_USER, L"Software\\SuVtitle-Test", KEY_WOW64_64KEY, 0);
        }
        ok("the test key is deleted whole", del == ERROR_SUCCESS && !keyExists(HKEY_CURRENT_USER, root));
        ok("SuVtitle's names: ProgIDs, vendor key, app name, Settings URI",
           std::wstring(pbassoc::kProgVideo) == L"NEXTALE.SuVtitle.Video" && std::wstring(pbassoc::kProgAudio) == L"NEXTALE.SuVtitle.Audio"
           && std::wstring(pbassoc::kVendorKey) == L"NEXTALE\\SuVtitle" && std::wstring(pbassoc::kAppName) == L"SuVtitle"
           && std::wstring(pbassoc::kSettingsUri) == L"ms-settings:defaultapps?registeredAppUser=SuVtitle");
    }

    // ---- [5] TrLaunch -------------------------------------------------------------------------------------------
    std::printf("[5] TrLaunch\n");
    {
        const pblaunch::Args r1 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--register-player" });
        const pblaunch::Args r2 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--tray", L"--play", L"D:\\a.mkv" });
        ok("--register-player parsed (and --unregister-player is not it); --tray means nothing any more",
           r1.registerPlayer && !r1.unregister && !r1.play && !r2.registerPlayer && r2.play && r2.paths.size() == 1);
        ok("the WM_COPYDATA tag is SuVtitle's own (\"SVTP\", not AudioPB's \"ABPP\")",
           pblaunch::kCopyPlay == 0x50545653u && pblaunch::kCopyPlay != 0x50504241u);
    }

    rmTree(base);
    ok("the scratch tree is gone", ::GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES);
    std::printf("\napp_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

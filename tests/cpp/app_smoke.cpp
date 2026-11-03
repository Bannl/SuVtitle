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
// SUVTITLE LIVE (1.1.0, L2 2026-10-25):
//   [6] AppConfig.h -- LIVEPOS (WINPOS's rules), LIVEIN (up to 4 strips, escaped, checked by the ENGINE's own LIVE
//       parser: a bad line is skipped, never the set), the three kinds of line never reordering each other, the LIVE
//       pair read from engine.txt, clampWinPos.
//   [7] TrLaunch.h -- --live (alone, with --play, after --play's paths), kCopyLive "SVTL" with an EMPTY payload only.
//   [8] AudioPBImport.h seedLivePos -- AudioPB's Translator window position read from a FIXTURE translator.txt (never
//       written), absent / unusable = false.
// U2 (02/11/2026, "Import model..." -- docs/PROGRESS_U2.md; sentinels: tests/cpp/cmut_u2.py):
//   [9] TrPipe.h -- the model HEADER classifier (whisper.cpp ggml / GGUF / junk / cut short; modelFileKind on real files: the
//       header, never the name) and FILE's asrModel / mtModel (the pre-U2 line byte for byte, both, one, the path rule);
//       AppConfig.h -- app.txt MODEL / MODELUSE; TrFileStore.h -- the cache key's model part (the default's key unchanged).
// U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md; sentinels: tests/cpp/cmut_u3.py):
//   [10] TrPipe.h -- FVOICE / FSPK (round trips, every refusal, an analysis' FILE unchanged).
//   [11] TrFileStore.h -- the voices record (round trip, STRICT, the 16 KB peek of a 20000-piece record) and the transcript's
//        Word file name (the candidates, the choice: never an overwrite of a file that is not ours).
//   [12] TrTranscript.h -- a dialogue cue's lines (the dash rule, both ways), the pieces (reading-time shares, the known
//        changes), FVOICE's fields, the speakers (most overlap, unheard pieces, the known-change rule), the turns (names, the
//        6 s / 2 s / 90 s rules, the SAME name = one person, the source timecode), typed names, the languages, the fingerprint.
// U2b (02/11/2026, removing an imported model -- docs/PROGRESS_U2b.md; sentinels: tests/cpp/cmut_u2.py):
//   [13] AppConfig.h -- removeImportedModel: the MODEL line gone, MODELUSE back to the default when it named it, the other lines
//        kept, another case of the path, refusals, the last model of a kind, a hand-edited app.txt.
// /W4, no /utf-8, like the product. ASCII only.
// =============================================================================
#include "../../app/AudioPBImport.h"
#include "../../app/AppConfig.h"
#include "../../app/TrAssoc.h"
#include "../../app/TrLaunch.h"
#include "../../app/TrPlayer.h"     // pbplayer::pairFromTranslatorCfg (the bridge reads engine.txt with it)
#include "../../app/TrTranscript.h" // U3: the transcript (pieces, speakers, turns)

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

    // ---- [6] SUVTITLE LIVE: app.txt's LIVEPOS / LIVEIN, the live pair, the clamp ------------------------------------
    std::printf("[6] LIVE: app.txt LIVEPOS / LIVEIN (AppConfig.h), engine.txt's live pair, clampWinPos\n");
    {
        using pbt::trpipe::WireLiveInput;
        auto strip = [](const char* key, const char* label, uint32_t chan, double gain) {
            WireLiveInput in; in.key = key; in.label = label; in.chan = chan; in.gainDb = gain; return in;
        };
        pbapp::WinPos lp;
        ok("LIVEPOS reads with WINPOS's rules (5 or 6 fields, the 200 x 150 floor) and is NOT the WINPOS line",
           pbapp::parseLivePos("WINPOS\t1\t2\t900\t700\t0\nLIVEPOS\t-1880\t40\t520\t380\t1\t0\n", lp) && lp.x == -1880 && lp.y == 40
           && lp.w == 520 && lp.h == 380 && lp.topmost && !lp.maximized
           && !pbapp::parseLivePos("WINPOS\t1\t2\t900\t700\t0\n", lp) && !pbapp::parseLivePos("LIVEPOS\t0\t0\t150\t900\t1\n", lp));
        // the three writers of app.txt, in turn: none reorders, drops or duplicates the others' lines
        std::string f = pbapp::packWinPos("", pbapp::WinPos{ 10, 20, 1100, 720, false, false });
        f = pbapp::packLivePos(f, pbapp::WinPos{ -1880, 40, 520, 380, true, false });
        const std::vector<WireLiveInput> two = { strip("in:{0.0.1.00000000}.{aaaa}", "Me", 1, 0.0),
                                                 strip("in:{0.0.1.00000000}.{aaaa}", "Guest\tB", 2, -3.25) };
        std::vector<WireLiveInput> norm;
        ok("liveSetNormal: one interface, channels 1 and 2 = a valid set; the gain normalized to the wire's 0.1 dB",
           pbapp::liveSetNormal(two, norm) && norm.size() == 2 && norm[1].label == "Guest\tB"
           && (norm[1].gainDb == -3.2 || norm[1].gainDb == -3.3));
        f = pbapp::packLiveInputs(f, norm);
        f = pbapp::packWinPos(f, pbapp::WinPos{ 11, 21, 1100, 720, false, true });
        const std::string f2 = pbapp::packLivePos(f, pbapp::WinPos{ -1880, 40, 520, 380, true, false });
        pbapp::WinPos w;
        ok("WINPOS, LIVEPOS and LIVEIN written in turn: one header, each line once, nothing reordered (a rewrite with the same "
           "values = the same bytes)",
           f2 == f && f.rfind("# SuVtitle app v1\nWINPOS\t11\t21\t1100\t720\t0\t1\nLIVEPOS\t-1880\t40\t520\t380\t1\t0\nLIVEIN\t", 0) == 0
           && pbapp::parseWinPos(f, w) && w.x == 11 && w.maximized && pbapp::parseLivePos(f, lp) && lp.x == -1880,
           f.substr(0, 240));
        ok("LIVEPOS keeps the pin (5th field) AND the maximized flag (6th) through a rewrite -- a window closed maximized reopens so",
           [&] {
               pbapp::WinPos m;
               const std::string g = pbapp::packLivePos(f, pbapp::WinPos{ -1880, 40, 520, 380, false, true });
               return pbapp::parseLivePos(g, m) && m.maximized && !m.topmost
                   && g.find("LIVEPOS\t-1880\t40\t520\t380\t0\t1\n") != std::string::npos;
           }());
        const auto back = pbapp::parseLiveInputs(f);
        ok("LIVEIN reads back: 2 strips, in order, the label's TAB escaped on disk and restored",
           back.size() == 2 && back[0].key == "in:{0.0.1.00000000}.{aaaa}" && back[0].label == "Me" && back[0].chan == 1
           && back[1].label == "Guest\tB" && back[1].chan == 2 && f.find("Guest\\tB") != std::string::npos);
        // hand-edited lines: each bad one SKIPPED, the rest kept (never the whole set lost)
        const std::string bad = "# SuVtitle app v1\n"
            "LIVEIN\tin:{x}\tA\t0\t0.0\n"
            "LIVEIN\tin:{x}\tSame key, its own mix again\t0\t0.0\n"     // the same key twice on the mix
            "LIVEIN\tusb:{y}\tUnknown kind\t0\t0.0\n"                   // a kind the engine does not know
            "LIVEIN\tout:{z}\tToo loud\t0\t99.0\n"                      // a trim out of range
            "LIVEIN\tout:{z}\tChannel 9\t9\t0.0\n"                      // a channel out of range
            "LIVEIN\tapp:zoom.exe\tZoom\t0\t-6.0\n"
            "LIVEIN\tout:{z}\tSpeakers\t0\t+0.5\n"
            "LIVEIN\tin:{w}\tFive\t0\t0\n"
            "LIVEIN\tin:{v}\tSix\t0\t0\n"                               // a fifth valid strip: over kMaxLiveInputs
            "LIVEIN\tin:{u}\ttorn\t0\n";                                // a field missing
        const auto kept = pbapp::parseLiveInputs(bad);
        ok("a hand-edited app.txt: duplicate, unknown kind, trim / channel out of range, a fifth strip, a torn line -- each "
           "SKIPPED, the 4 good ones kept in order",
           kept.size() == 4 && kept[0].key == "in:{x}" && kept[1].key == "app:zoom.exe" && kept[1].gainDb == -6.0
           && kept[2].key == "out:{z}" && kept[2].label == "Speakers" && kept[3].key == "in:{w}",
           std::to_string(kept.size()));
        std::vector<WireLiveInput> n2;
        ok("liveSetNormal REFUSES what unpackLive refuses: a 5th strip, a 65-byte label, a channel next to its own mix, wav: is a "
           "KIND (the bridge decides who may use it)",
           !pbapp::liveSetNormal({ strip("in:a", "", 0, 0), strip("in:b", "", 0, 0), strip("in:c", "", 0, 0), strip("in:d", "", 0, 0),
                                   strip("in:e", "", 0, 0) }, n2)
           && !pbapp::liveSetNormal({ strip("in:a", std::string(65, 'x').c_str(), 0, 0) }, n2)
           && pbapp::liveSetNormal({ strip("in:a", std::string(64, 'x').c_str(), 0, 0) }, n2)
           && !pbapp::liveSetNormal({ strip("in:a", "", 0, 0), strip("in:a", "", 1, 0) }, n2)
           && pbapp::liveSetNormal({ strip("wav:C:\\t\\a.wav", "", 0, 0) }, n2) && pbapp::liveSetNormal({}, n2) && n2.empty());
        ok("an EMPTY set is written as no LIVEIN line at all (the other lines kept)",
           pbapp::packLiveInputs(f, {}).find("LIVEIN") == std::string::npos && pbapp::packLiveInputs(f, {}).find("LIVEPOS\t-1880") != std::string::npos);
        std::string la = "", lb = "", lm = "";
        const bool got = pbapp::livePairFromEngineCfg("# SuVtitle Engine config v1\nLANGA\tes\nLANGB\tfr\nLIVELANGA\tfr\nLIVELANGB\tzh-tw\n"
                                                      "LIVEMODE\tzh2en\n", la, lb, lm);
        std::string pa = "en", pb = "zh", pm = "auto";
        pbplayer::pairFromTranslatorCfg("LANGA\tes\nLANGB\tfr\nLIVELANGA\tfr\nLIVELANGB\tzh-tw\n", pa, pb, pm);
        ok("engine.txt: the LIVE pair (LIVELANGA/B, LIVEMODE migrated) and the PLAYER's pair are read apart (es/fr vs fr/zh-tw)",
           got && la == "fr" && lb == "zh-tw" && lm == "b2a" && pa == "es" && pb == "fr");
        std::string x1 = "en", x2 = "zh", xm = "a2b";
        ok("...an engine.txt without the live pair: false, the caller keeps its own",
           !pbapp::livePairFromEngineCfg("LANGA\tes\nLANGB\tfr\n", x1, x2, xm) && x1 == "en" && x2 == "zh");
        const pbapp::WinPos c1 = pbapp::clampWinPos(pbapp::WinPos{ -1880, 40, 520, 380, true, false }, 0, 0, 1920, 1040);
        const pbapp::WinPos c2 = pbapp::clampWinPos(pbapp::WinPos{ 1700, 900, 3000, 2000, false, false }, 0, 0, 1920, 1040);
        ok("clampWinPos: a window left on an unplugged screen comes onto the work area; one too big is shrunk then placed",
           c1.x == 0 && c1.y == 40 && c1.w == 520 && c1.h == 380 && c1.topmost && c2.x == 0 && c2.y == 0 && c2.w == 1920 && c2.h == 1040,
           std::to_string(c1.x) + "," + std::to_string(c2.w));
        // FIX ROUND (RVB-11): a place no screen can be -- near INT_MAX, x + w overflowed the title-bar test and the clamp
        pbapp::WinPos farPos, nearMax, wall;   // (not `far`: a Windows macro)
        ok("RVB-11: LIVEPOS / WINPOS beyond 2^20 px (x or y) are garbled lines: refused (a window is never created nowhere)",
           !pbapp::parseLivePos("LIVEPOS\t2147483600\t100\t1000\t800\t1\t0\n", farPos)
           && !pbapp::parseWinPos("WINPOS\t100\t-2147483600\t1000\t800\t0\n", nearMax)
           && !pbapp::parseLivePos("LIVEPOS\t1048576\t0\t900\t700\t0\t0\n", farPos));
        ok("...while a wall of screens is kept (-1900 / 60, and 30 000 px to the right)",
           pbapp::parseLivePos("LIVEPOS\t-1900\t60\t900\t780\t0\t0\n", wall) && wall.x == -1900 && wall.y == 60
           && pbapp::parseWinPos("WINPOS\t30000\t-2000\t1100\t720\t0\n", nearMax) && nearMax.x == 30000 && nearMax.y == -2000);
        // FIX ROUND (RVB-1): LIVESESSION -- the session's journal, a FILE NAME of the bridge's own shape, kept until a Clear
        const std::string base6 = "# SuVtitle app v1\nWINPOS\t1\t2\t900\t700\t0\t0\nLIVEIN\tin:a\tMe\t0\t0.0\n";
        const std::string withS = pbapp::packLiveSession(base6, "2026-10-26_143005.jsonl");
        ok("RVB-1: LIVESESSION is written beside the other lines (kept in order) and read back",
           withS.find("LIVESESSION\t2026-10-26_143005.jsonl\n") != std::string::npos && withS.find("WINPOS\t1\t2") != std::string::npos
           && withS.find("LIVEIN\tin:a") != std::string::npos && pbapp::parseLiveSession(withS) == "2026-10-26_143005.jsonl"
           && pbapp::parseLiveSession(pbapp::packLiveSession(withS, "2026-10-26_143005_2.jsonl")) == "2026-10-26_143005_2.jsonl");
        ok("...a Clear removes it (\"\" = no session), and app.txt keeps everything else",
           pbapp::packLiveSession(withS, "").find("LIVESESSION") == std::string::npos && pbapp::parseLiveSession(pbapp::packLiveSession(withS, "")).empty()
           && pbapp::packLiveSession(withS, "").find("LIVEIN\tin:a") != std::string::npos);
        ok("...and only a journal's FILE NAME is ever taken: a path, '..', a drive, a stream, another extension -> no session",
           pbapp::parseLiveSession("LIVESESSION\t..\\..\\x.jsonl\n").empty() && pbapp::parseLiveSession("LIVESESSION\tC:\\x.jsonl\n").empty()
           && pbapp::parseLiveSession("LIVESESSION\ta.jsonl:s\n").empty() && pbapp::parseLiveSession("LIVESESSION\t2026.txt\n").empty()
           && pbapp::parseLiveSession("LIVESESSION\t.jsonl\n").empty() && pbapp::parseLiveSession("LIVESESSION\t20/26.jsonl\n").empty()
           && !pbapp::liveSessionNameOk("x.jsonl") && pbapp::liveSessionNameOk("2026-10-26_143005.jsonl")
           && pbapp::packLiveSession(base6, "..\\evil.jsonl").find("LIVESESSION") == std::string::npos);
    }

    // ---- [7] SUVTITLE LIVE: --live and its WM_COPYDATA ------------------------------------------------------------
    std::printf("[7] LIVE: --live, kCopyLive\n");
    {
        const pblaunch::Args l1 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--live" });
        const pblaunch::Args l2 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--play", L"D:\\a.mkv", L"--live" });
        const pblaunch::Args l3 = pblaunch::parseArgs({ L"SuVtitle.exe", L"--play", L"D:\\a.mkv" });
        ok("--live parsed alone; after --play's paths it ends them (and both are honoured); absent = not live",
           l1.live && !l1.play && l2.live && l2.play && l2.paths.size() == 1 && !l3.live);
        const char one = 'x';
        ok("kCopyLive is \"SVTL\" (not SVTP) and carries NOTHING: an empty payload only, any byte refused",
           pblaunch::kCopyLive == 0x4C545653u && pblaunch::kCopyLive != pblaunch::kCopyPlay
           && pblaunch::isLiveCopy(pblaunch::kCopyLive, nullptr, 0) && !pblaunch::isLiveCopy(pblaunch::kCopyLive, &one, 1)
           && !pblaunch::isLiveCopy(pblaunch::kCopyPlay, nullptr, 0));
        std::vector<std::wstring> none;
        const std::vector<wchar_t> empty = pblaunch::packPlay({});
        ok("an EMPTY --play forward (a plain second launch while the player is hidden) is a valid kCopyPlay: no path",
           pblaunch::unpackPlay(empty.data(), empty.size() * sizeof(wchar_t), none) && none.empty());
    }

    // ---- [8] SUVTITLE LIVE: the Live window's first place, from AudioPB's translator.txt (a FIXTURE) --------------
    std::printf("[8] LIVE: seedLivePos (a fixture of AudioPB's translator.txt, read only)\n");
    {
        pbimport::Roots r;
        r.fromRoaming = base + L"\\seed\\AudioPB";
        ::CreateDirectoryW((base + L"\\seed").c_str(), nullptr);
        ::CreateDirectoryW(r.fromRoaming.c_str(), nullptr);
        const std::string cfg = "# AudioPB Translator config v2\nPRESET\tbalanced\nWINPOS\t-1900\t60\t760\t560\t1\nMODE\tauto\n";
        const bool made = put(r.fromRoaming + L"\\translator.txt", cfg);
        std::vector<std::pair<std::wstring, std::string>> before, after;
        snapshot(r.fromRoaming, L"", before);
        pbapp::WinPos s;
        ok("AudioPB's WINPOS (5 fields, topmost on) becomes the Live window's first place",
           made && pbimport::seedLivePos(r, s) && s.x == -1900 && s.y == 60 && s.w == 760 && s.h == 560 && s.topmost);
        snapshot(r.fromRoaming, L"", after);
        ok("...AudioPB's folder exactly as before (read, never written)", before == after && before.size() == 1);
        put(r.fromRoaming + L"\\translator.txt", "# AudioPB Translator config v2\nPRESET\tlight\n");
        pbimport::Roots nowhere;
        nowhere.fromRoaming = base + L"\\seed\\NoSuchAudioPB";
        ok("no WINPOS, no translator.txt, no folder named: false (the Live window's default place)",
           !pbimport::seedLivePos(r, s) && !pbimport::seedLivePos(nowhere, s) && !pbimport::seedLivePos(pbimport::Roots{}, s)
           && ::GetFileAttributesW(nowhere.fromRoaming.c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    // ---- [9] U2 (02/11/2026): THE USER'S OWN MODELS -- the HEADER classifier and FILE's two fields (TrPipe.h: the ONE copy
    //      both ends compile), app.txt's MODEL / MODELUSE (AppConfig.h), the cache key's model part (TrFileStore.h) -------
    std::printf("[9] U2: the imported models (TrPipe.h header + FILE fields, app.txt MODEL / MODELUSE, the cache key)\n");
    {
        namespace tp = pbt::trpipe;
        auto put32 = [](std::string& b, uint32_t v) { for (int i = 0; i < 4; ++i) b += static_cast<char>((v >> (8 * i)) & 0xff); };
        auto whisperHead = [&](int vocab, int state, int head, int alayer, int tlayer, int mels, int nfft) {
            std::string b;
            put32(b, 0x67676d6cu);
            const int h[13] = { vocab, 1500, state, head, alayer, 448, state, head, tlayer, mels, 1, mels, nfft };
            for (int v : h) put32(b, static_cast<uint32_t>(v));
            return b;
        };
        auto ggufHead = [&](uint32_t ver, uint32_t tensors, uint32_t keys) {
            std::string b = "GGUF";
            put32(b, ver); put32(b, tensors); put32(b, 0); put32(b, keys); put32(b, 0);
            return b;
        };
        auto kindOf = [](const std::string& b, std::string* why = nullptr) {
            return tp::modelKindOfHeader(reinterpret_cast<const unsigned char*>(b.data()), b.size(), why);
        };
        using MK = tp::ModelKind;
        const std::string wBase = whisperHead(51865, 512, 8, 6, 6, 80, 201);
        std::string oldLlama;
        put32(oldLlama, 0x67676d6cu);
        for (int v : { 32000, 4096, 256, 32, 32, 128, 2, 0, 0, 0, 0, 0, 0 }) put32(oldLlama, static_cast<uint32_t>(v));
        std::string whyOld, whyCut, whyJunk, whyGguf;
        std::string ctx1499 = wBase, text447 = wBase;                // one hyper-parameter off each (the audio / text context)
        ctx1499[8] = static_cast<char>(0xDB);                         // 1500 = 0x05DC -> 1499
        text447[24] = static_cast<char>(0xBF);                        // 448 = 0x01C0 -> 447
        ok("[9] a whisper.cpp model by its header: base, large-v3 (128 mels, 51866), large-v3-turbo (4 text layers), tiny.en (51864), "
           "a distil-like decoder (fewer text layers)",
           kindOf(wBase) == MK::Asr && kindOf(whisperHead(51866, 1280, 20, 32, 32, 128, 201)) == MK::Asr
           && kindOf(whisperHead(51866, 1280, 20, 32, 4, 128, 201)) == MK::Asr && kindOf(whisperHead(51864, 384, 6, 4, 4, 80, 201)) == MK::Asr
           && kindOf(whisperHead(51866, 1280, 20, 32, 2, 128, 201)) == MK::Asr);
        ok("[9] a GGUF by its header: v3, v2",
           kindOf(ggufHead(3, 291, 24)) == MK::Mt && kindOf(ggufHead(2, 291, 24)) == MK::Mt);
        ok("[9] the SAME magic in llama.cpp's first files is NOT a whisper model (audio context 1500 / text 448 / mels / n_fft "
           "checked); an audio context of 1499, a text context of 447, 200-point filters, 81 mels, a 70-layer net, a 40000-word "
           "vocabulary: none",
           kindOf(oldLlama, &whyOld) == MK::None && whyOld.find("not a whisper.cpp model") != std::string::npos
           && kindOf(ctx1499) == MK::None && kindOf(text447) == MK::None
           && kindOf(whisperHead(51865, 512, 8, 6, 6, 80, 200)) == MK::None && kindOf(whisperHead(51865, 512, 8, 6, 6, 81, 201)) == MK::None
           && kindOf(whisperHead(51865, 512, 8, 70, 6, 80, 201)) == MK::None && kindOf(whisperHead(40000, 512, 8, 6, 6, 80, 201)) == MK::None);
        ok("[9] a GGUF llama.cpp does not read (v1, v4) or with no tensor / no key: none, said why",
           kindOf(ggufHead(1, 291, 24), &whyGguf) == MK::None && whyGguf.find("version") != std::string::npos
           && kindOf(ggufHead(4, 291, 24)) == MK::None && kindOf(ggufHead(3, 0, 24)) == MK::None && kindOf(ggufHead(3, 291, 0)) == MK::None);
        ok("[9] CUT SHORT (40 / 55 bytes of a whisper header, 20 of a GGUF, 2, none) and junk (a WAV, an older \"ggjt\" ggml): none, said why",
           kindOf(wBase.substr(0, 40), &whyCut) == MK::None && whyCut.find("cut short") != std::string::npos
           && kindOf(wBase.substr(0, 55)) == MK::None && kindOf(wBase.substr(0, 56)) == MK::Asr
           && kindOf(ggufHead(3, 291, 24).substr(0, 20)) == MK::None && kindOf(ggufHead(3, 291, 24).substr(0, 24)) == MK::Mt
           && kindOf("GG") == MK::None && kindOf(std::string()) == MK::None
           && kindOf(std::string("RIFF") + std::string(60, 'x'), &whyJunk) == MK::None && whyJunk.find("neither") != std::string::npos
           && kindOf(std::string("tjgg") + std::string(60, '\0')) == MK::None);
        // modelFileKind on real files: the header, never the name
        const std::wstring u2 = base + L"\\u2 models";
        ::CreateDirectoryW(u2.c_str(), nullptr);
        put(u2 + L"\\named.gguf", wBase + std::string(64, '\0'));             // a whisper model NAMED .gguf
        put(u2 + L"\\named.bin", ggufHead(3, 291, 24) + std::string(64, '\0'));   // a GGUF NAMED .bin
        put(u2 + L"\\short.bin", wBase.substr(0, 30));
        std::string whyGone, whyDir;
        ok("[9] modelFileKind READS the file: a whisper model named .gguf is a transcription model, a GGUF named .bin a translation "
           "one; cut short: none; a file that is gone: \"not there\"; a folder: none",
           tp::modelFileKind(u2 + L"\\named.gguf") == MK::Asr && tp::modelFileKind(u2 + L"\\named.bin") == MK::Mt
           && tp::modelFileKind(u2 + L"\\short.bin") == MK::None
           && tp::modelFileKind(u2 + L"\\gone.bin", &whyGone) == MK::None && whyGone == "the file is not there"
           && tp::modelFileKind(u2, &whyDir) == MK::None);
        // FILE's two fields
        tp::WireFile f, g;
        f.jobId = 7; f.path = "C:\\m\\film.mkv"; f.langA = "en"; f.langB = "fr"; f.mode = "auto"; f.quality = "auto";
        f.have = "0-5000"; f.focusMs = 1200;
        auto preU2 = [&](const tp::WireFile& w) {                   // packFile as it was before U2, by hand
            std::string o = "FILE\t" + std::to_string(w.jobId) + "\t" + tp::esc(w.path) + "\t" + tp::esc(w.langA) + "\t"
                          + tp::esc(w.langB) + "\t" + tp::esc(w.mode) + "\t" + tp::esc(w.quality) + "\t" + tp::esc(w.have)
                          + "\t" + std::to_string(w.focusMs);
            if (!w.audio.empty()) o += "\t" + tp::esc(w.audio);
            return o;
        };
        const bool same0 = tp::packFile(f) == preU2(f);
        f.audio = "1:4";
        const bool same1 = tp::packFile(f) == preU2(f);
        f.audio.clear();
        ok("[9] a FILE naming no model is the pre-U2 line BYTE FOR BYTE (with and without a sound track)", same0 && same1);
        f.asrModel = "D:\\My models\\ggml-base.bin";
        f.mtModel = "\\\\nas\\share\\Qwen2.5 1.5B\\qwen2.5-1.5b-instruct-q4_k_m.gguf";
        const std::vector<std::string> t12 = tp::split(tp::packFile(f));
        ok("[9] both models: fields 11 and 12 (an empty sound track before them), read back exactly (a drive path, a UNC path)",
           t12.size() == 12 && t12[9].empty() && tp::unpackFile(t12, g) && g.asrModel == f.asrModel && g.mtModel == f.mtModel && g.audio.empty());
        f.asrModel.clear();
        ok("[9] the translation model alone: field 11 empty, field 12 named",
           tp::unpackFile(tp::split(tp::packFile(f)), g) && g.asrModel.empty() && g.mtModel == f.mtModel);
        f.mtModel.clear();
        const char* bads[] = { "ggml-base.bin", "models\\ggml-base.bin", "C:ggml.bin", "C:\\a\\..\\b.bin", "C:\\a\\.\\b.bin",
                               "C:\\m\\x.bin:zone", "C:/m/x.bin", "\\\\?\\C:\\m\\x.bin", "\\\\.\\pipe\\x", "C:\\m\\", "C:\\m\\x|y.bin",
                               "C:\\m\\a\x01" "b.bin", "C:\\m\\\\x.bin", "\\\\", "C:\\" };
        int taken = 0;
        for (const char* b : bads) {
            tp::WireFile w = f;
            w.asrModel = b;
            if (tp::unpackFile(tp::split(tp::packFile(w)), g)) ++taken;
            w.asrModel.clear();
            w.mtModel = b;
            if (tp::unpackFile(tp::split(tp::packFile(w)), g)) ++taken;
            if (tp::modelPathOk(b)) ++taken;
        }
        const std::string longOk = "C:\\" + std::string(tp::kModelPathMax - 7, 'm') + ".bin";
        ok("[9] a model path that is not a full path to a file refuses the WHOLE FILE (15 forms, each field); 1024 bytes: taken, 1025: not",
           taken == 0 && longOk.size() == tp::kModelPathMax && tp::modelPathOk(longOk) && !tp::modelPathOk(longOk + "x")
           && tp::modelPathOk("\\\\nas\\share\\m.gguf") && tp::modelPathOk("c:\\m.bin"), std::to_string(taken));
        ok("[9] fileModelErrKind: an imported model's sentence says its kind; a shipping model's failure is none",
           tp::fileModelErrKind(std::string(tp::kFileModelErrAsr) + "x.bin\" could not be used: y.") == "asr"
           && tp::fileModelErrKind(std::string(tp::kFileModelErrMt) + "x.gguf\" could not be used: y.") == "mt"
           && tp::fileModelErrKind("ASR model missing: ggml-large-v3-turbo.bin").empty()
           && tp::fileModelErrKind("translation model missing: Qwen3-4B-Instruct-2507-Q5_K_M.gguf").empty());
        // app.txt: MODEL / MODELUSE
        const std::string A = "D:\\Models\\ggml-base.bin", B = "D:\\Models\\ggml-medium.bin", C = "E:\\LLM\\gemma-3-4b-it-Q4_K_M.gguf";
        pbapp::ImportedModels m;
        m.asr = { A, B };
        m.mt = { C };
        m.useAsr = B;
        m.useMt = C;
        const std::string old = "# SuVtitle app v1\nWINPOS\t10\t20\t900\t600\t0\t0\nLIVEIN\tin:{x}\tMe\t0\t0.0\nFUTURE\tkeep me\n";
        const std::string packed = pbapp::packModels(old, m);
        const pbapp::ImportedModels back = pbapp::parseModels(packed);
        ok("[9] app.txt MODEL / MODELUSE: the lists and the choices round-trip; every other line kept, in order, one header",
           back.asr == m.asr && back.mt == m.mt && back.useAsr == B && back.useMt == C
           && packed.find("WINPOS\t10\t20") != std::string::npos && packed.find("LIVEIN\tin:{x}") < packed.find("FUTURE\tkeep me")
           && packed.find("# SuVtitle app v1") == 0 && packed.find("# SuVtitle", 1) == std::string::npos, packed);
        ok("[9] ...the paths escaped (a backslash doubled in the file, never a raw tab), and a rewrite with nothing changed is the same bytes",
           packed.find("MODEL\tasr\t" + tp::esc(A)) != std::string::npos && pbapp::packModels(packed, back) == packed);
        const std::string hand = "# SuVtitle app v1\nMODELUSE\tasr\t" + tp::esc(B) + "\nMODEL\tasr\t" + tp::esc(A) + "\nMODEL\tasr\t"
                               + tp::esc(B) + "\nMODEL\tASR\t" + tp::esc("D:\\x.bin") + "\nMODEL\ttts\t" + tp::esc("D:\\y.bin")
                               + "\nMODEL\tasr\tggml-tiny.bin\nMODEL\tasr\t" + tp::esc("d:\\models\\GGML-BASE.BIN") + "\nMODEL\tmt\t"
                               + tp::esc(C) + "\textra\nMODELUSE\tmt\t" + tp::esc("E:\\LLM\\not-listed.gguf") + "\n";
        const pbapp::ImportedModels h = pbapp::parseModels(hand);
        ok("[9] a hand-edited app.txt: a MODELUSE before its MODEL is taken; a bad kind, a relative path, a 4-field line, a duplicate "
           "(another case) skipped -- never the rest; a MODELUSE naming no MODEL of its kind is the default",
           h.asr.size() == 2 && h.asr[0] == A && h.asr[1] == B && h.useAsr == B && h.mt.empty() && h.useMt.empty());
        pbapp::ImportedModels cap;
        int added = 0;
        for (int i = 0; i < 20; ++i) added += pbapp::addImportedModel(cap, "asr", "D:\\M\\m" + std::to_string(i) + ".bin") ? 1 : 0;
        pbapp::ImportedModels fresh;
        ok("[9] 16 models of a kind at most; an import of one already listed (another case) is \"there\", not a second line; a "
           "relative path or another kind word refused",
           added == 16 && cap.asr.size() == 16 && pbapp::addImportedModel(cap, "asr", "d:\\m\\M3.BIN") && cap.asr.size() == 16
           && !pbapp::addImportedModel(cap, "mt", "m.gguf") && cap.mt.empty()
           && !pbapp::addImportedModel(fresh, "llm", "D:\\M\\x.gguf") && fresh.asr.empty() && fresh.mt.empty());
        pbapp::ImportedModels none = back;
        none.useAsr.clear();
        const std::string noUse = pbapp::packModels(packed, none);
        ok("[9] a choice back to the default = its MODELUSE line gone (the file stays listed)",
           noUse.find("MODELUSE\tasr") == std::string::npos && noUse.find("MODELUSE\tmt") != std::string::npos
           && pbapp::parseModels(noUse).asr == m.asr && pbapp::parseModels(noUse).useAsr.empty());
        // the cache key: the model part ONLY when a model is not the default -- every key made before U2 unchanged
        const std::wstring film = L"D:\\Films\\Mon Film.MKV";
        auto preKey = [&](uint32_t ord) {                              // cacheKey as it was before U2, by hand
            std::string k = pbtrf::toUtf8(pbtrf::lowerW(film)) + "\t5000\t11\ten\tfr\tauto\tauto";
            if (ord > 0) k += "\ta" + std::to_string(ord);
            return pbtrf::hex16(pbtrf::fnv1a64(k));
        };
        const std::string tagA = pbtrf::modelsTag(pbtrf::modelIdent(L"D:\\Models\\GGML-base.bin", 148, 9), "");
        ok("[9] the cache key of the DEFAULT models is the pre-U2 key byte for byte (ord 0 and 1, no tag and an empty one): no film "
           "analysed again, no record orphaned",
           pbtrf::cacheKey(film, 5000, 11, "en", "fr", "auto", "auto") == preKey(0)
           && pbtrf::cacheKey(film, 5000, 11, "en", "fr", "auto", "auto", 0, "") == preKey(0)
           && pbtrf::cacheKey(film, 5000, 11, "en", "fr", "auto", "auto", 1, pbtrf::modelsTag("", "")) == preKey(1)
           && pbtrf::modelsTag("", "").empty());
        ok("[9] an imported model's analysis has its OWN key: never the default's, never another model's, never the same file "
           "replaced (size / write time); which half it is counts; where the file is does not",
           pbtrf::cacheKey(film, 5000, 11, "en", "fr", "auto", "auto", 0, tagA) != preKey(0)
           && pbtrf::cacheKey(film, 5000, 11, "en", "fr", "auto", "auto", 1, tagA) != preKey(1)
           && tagA != pbtrf::modelsTag(pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 149, 9), "")
           && tagA != pbtrf::modelsTag(pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 148, 10), "")
           && tagA != pbtrf::modelsTag("", pbtrf::modelIdent(L"D:\\Models\\ggml-base.bin", 148, 9))
           && tagA == pbtrf::modelsTag(pbtrf::modelIdent(L"E:\\elsewhere\\ggml-base.bin", 148, 9), ""), tagA);
        const auto sibDef = pbtrf::siblingKeys(film, 5000, 11, "fr", {}, 0), sibTag = pbtrf::siblingKeys(film, 5000, 11, "fr", {}, 0, tagA);
        bool sibOk = sibDef.size() == sibTag.size() && !sibDef.empty();
        for (size_t i = 0; sibOk && i < sibDef.size(); ++i)
            sibOk = sibDef[i].key == pbtrf::cacheKey(film, 5000, 11, sibDef[i].langA, sibDef[i].langB, sibDef[i].mode, sibDef[i].quality)
                 && sibTag[i].key == pbtrf::cacheKey(film, 5000, 11, sibTag[i].langA, sibTag[i].langB, sibTag[i].mode, sibTag[i].quality, 0, tagA)
                 && sibTag[i].key != sibDef[i].key;
        ok("[9] the shared original's candidates: the default set's are the pre-U2 keys; an imported model's set looks among ITS model's",
           sibOk);
    }

    // ---- [10] U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md): the voice pass on the wire --------------------------
    std::printf("[10] U3: the transcript's voice pass (FVOICE / FSPK), strict at both ends\n");
    {
        namespace tp = pbt::trpipe;
        tp::WireVoice v;
        v.jobId = 42; v.path = "D:\\Films\\an\tinterview.mkv"; v.audio = "1:2"; v.spans = "0-1500,1500-2600,4000-9000";
        v.changes = "1";
        tp::WireVoice r;
        const std::string line = tp::packVoice(v);
        ok("[10] FVOICE round trip: id, path (a tab escaped), track, pieces, changes",
           tp::unpackVoice(tp::split(line), r) && r.jobId == 42 && r.path == v.path && r.audio == "1:2" && r.spans == v.spans
           && r.changes == "1" && line.rfind("FVOICE\t42\t", 0) == 0, line);
        auto refused = [&](const char* jobId, const char* path, const char* audio, const char* spans, const char* changes) {
            std::string l = std::string("FVOICE\t") + jobId + "\t" + path + "\t" + audio + "\t" + spans;
            if (changes) l += std::string("\t") + changes;
            tp::WireVoice x;
            return !tp::unpackVoice(tp::split(l), x);
        };
        ok("[10] FVOICE refused WHOLE: id 0, no path, a bad track, no piece, pieces out of order / overlapping / empty / junk",
           refused("0", "D:\\a.wav", "", "0-10", "") && refused("7", "", "", "0-10", "") && refused("7", "D:\\a.wav", "x", "0-10", "")
           && refused("7", "D:\\a.wav", "", "", "") && refused("7", "D:\\a.wav", "", "10-20,0-5", "")
           && refused("7", "D:\\a.wav", "", "0-10,5-20", "") && refused("7", "D:\\a.wav", "", "10-10", "")
           && refused("7", "D:\\a.wav", "", "0-10,x", ""));
        ok("[10] FVOICE's changes: an index 0, past the last piece, not ascending, twice, junk, empty items -> refused; touching "
           "pieces and a 4-field line (no changes) accepted",
           refused("7", "D:\\a.wav", "", "0-10,10-20", "0") && refused("7", "D:\\a.wav", "", "0-10,10-20", "2")
           && refused("7", "D:\\a.wav", "", "0-10,10-20,20-30", "2,1") && refused("7", "D:\\a.wav", "", "0-10,10-20,20-30", "1,1")
           && refused("7", "D:\\a.wav", "", "0-10,10-20", "a") && refused("7", "D:\\a.wav", "", "0-10,10-20,20-30", "1,,2")
           && !refused("7", "D:\\a.wav", "", "0-10,10-20,20-30", "1,2") && !refused("7", "D:\\a.wav", "", "0-10,10-20", nullptr));
        std::string many;
        for (size_t i = 0; i <= tp::kVoiceSpanMax; ++i)
            many += (many.empty() ? "" : ",") + std::to_string(i * 10) + "-" + std::to_string(i * 10 + 5);
        const std::string atMax = many.substr(0, many.rfind(','));
        ok("[10] FVOICE: kVoiceSpanMax pieces accepted, one more refused",
           !refused("7", "D:\\a.wav", "", atMax.c_str(), "") && refused("7", "D:\\a.wav", "", many.c_str(), ""));
        std::vector<uint32_t> ch;
        ok("[10] parseChanges: the rule alone (n = 4)",
           tp::parseChanges("", 4, ch) && ch.empty() && tp::parseChanges("1,3", 4, ch) && ch.size() == 2 && ch[1] == 3
           && !tp::parseChanges("4", 4, ch) && ch.empty() && !tp::parseChanges("1,", 4, ch) && !tp::parseChanges("0001234567890", 9, ch)
           && tp::packChanges({ 1, 3 }) == "1,3" && tp::packChanges({}).empty());
        tp::WireSpk s;
        s.jobId = 42; s.speakers = 3; s.labels = { 1, 2, 1, 0, 3 }; s.model = "campplus";
        tp::WireSpk sr;
        ok("[10] FSPK round trip (a 0 = a piece no voice was named for)",
           tp::unpackSpk(tp::split(tp::packSpk(s)), sr) && sr.jobId == 42 && sr.speakers == 3 && sr.labels == s.labels
           && sr.model == "campplus", tp::packSpk(s));
        tp::WireSpk off;
        off.jobId = 9; off.speakers = 0; off.model = "speaker diarization is off (SPKON 0)";
        ok("[10] FSPK with no names: no label, the sentence why",
           tp::unpackSpk(tp::split(tp::packSpk(off)), sr) && sr.speakers == 0 && sr.labels.empty() && sr.model == off.model);
        auto spkRefused = [&](const std::string& l) { tp::WireSpk x; return !tp::unpackSpk(tp::split(l), x); };
        ok("[10] FSPK refused: id 0, a label past the speakers, labels with no speaker, no label with speakers, a number unused, "
           "junk, more than kVoiceSpeakersMax, an empty item",
           spkRefused("FSPK\t0\t1\t1\tcampplus") && spkRefused("FSPK\t5\t2\t1,3\tcampplus") && spkRefused("FSPK\t5\t0\t1\tx")
           && spkRefused("FSPK\t5\t2\t\tcampplus") && spkRefused("FSPK\t5\t2\t1,1\tcampplus") && spkRefused("FSPK\t5\t2\t1,b\tcampplus")
           && spkRefused("FSPK\t5\t65\t1\tcampplus") && spkRefused("FSPK\t5\t1\t1,\tcampplus") && !spkRefused("FSPK\t5\t2\t2,1,0\tcampplus"));
        tp::WireFile f;
        f.jobId = 3; f.path = "D:\\Films\\a.mkv"; f.langA = "en"; f.langB = "fr"; f.mode = "auto"; f.quality = "auto";
        ok("[10] an ANALYSIS' FILE line is byte for byte what it was (U3 adds messages, never a field to FILE)",
           tp::packFile(f) == "FILE\t3\tD:\\\\Films\\\\a.mkv\ten\tfr\tauto\tauto\t\t0", tp::packFile(f));
    }

    // ---- [11] U3: the voices record (<cache>\<key>.voices) and the transcript's Word file name ----------------------------
    std::printf("[11] U3: TrFileStore.h -- the voices record, strict; the transcript's .docx name\n");
    {
        pbtrf::VoiceRec v;
        v.path = "D:\\Films\\an\tinterview.mkv"; v.size = 123456789; v.mtime = 133000000000000000ull;
        v.audioOrd = 2; v.audioTrack = 3; v.on = true; v.model = "campplus"; v.speakers = 3;
        v.names = { { 1, "Marie" }, { 3, "\xe7\x8e\x8b\xe8\x8a\xb3 (Wang Fang)" } };
        v.docxPath = "D:\\Films\\an interview - transcript.docx"; v.docxSize = 4567; v.docxMtime = 133000000000000001ull;
        v.docxSig = 0xFEDCBA9876543210ull;
        v.segs = { { 0, 1500, 1 }, { 1500, 2600, 2 }, { 4000, 9000, 0 }, { 9000, 9100, 3 } };
        const std::string text = pbtrf::packVoices(v);
        pbtrf::VoiceRec r;
        ok("[11] the voices record round trip: SRC (a tab escaped), AUD, DIAR, the renames (UTF-8), DOCX, the pieces",
           pbtrf::parseVoicesText(text, r) && r.path == v.path && r.size == v.size && r.mtime == v.mtime && r.audioOrd == 2
           && r.audioTrack == 3 && r.on && r.model == "campplus" && r.speakers == 3 && r.names == v.names && r.docxPath == v.docxPath
           && r.docxSize == 4567 && r.docxMtime == v.docxMtime && r.docxSig == v.docxSig && r.segs.size() == 4 && r.segs[2].spk == 0
           && r.segs[3].e == 9100, text);
        pbtrf::VoiceRec off;
        off.path = "D:\\a.wav"; off.why = "no piece was long enough to recognise a voice";
        off.segs = { { 0, 400, 0 } };
        ok("[11] a record with no names: DIAR off, the sentence why, pieces with no speaker",
           pbtrf::parseVoicesText(pbtrf::packVoices(off), r) && !r.on && r.why == off.why && r.speakers == 0 && r.segs.size() == 1);
        auto lines = [&](const std::string& t) {
            std::vector<std::string> o;
            for (size_t a = 0; a < t.size();) {
                size_t n = t.find('\n', a);
                if (n == std::string::npos) n = t.size();
                o.push_back(t.substr(a, n - a));
                a = n + 1;
            }
            return o;
        };
        const std::vector<std::string> L = lines(text);         // 0 header, 1 SRC, 2 AUD, 3 DIAR, 4-5 NAME, 6 DOCX, 7-10 SEG, 11 END
        auto with = [&](size_t at, const std::string& line) { std::string o; for (size_t i = 0; i < L.size(); ++i) o += (i == at ? line : L[i]) + "\n"; return o; };
        auto without = [&](size_t at) { std::string o; for (size_t i = 0; i < L.size(); ++i) if (i != at) o += L[i] + "\n"; return o; };
        const bool strict = !pbtrf::parseVoicesText(with(0, "# SuVtitle voices v2"), r) && !pbtrf::parseVoicesText(without(1), r)
            && !pbtrf::parseVoicesText(without(3), r) && !pbtrf::parseVoicesText(with(3, "DIAR\ton\tcampplus\t0"), r)
            && !pbtrf::parseVoicesText(with(3, "DIAR\toff\twhy\t2"), r) && !pbtrf::parseVoicesText(with(3, "DIAR\tmaybe\tx\t3"), r)
            && !pbtrf::parseVoicesText(with(3, "DIAR\ton\tcampplus\t65"), r) && !pbtrf::parseVoicesText(with(4, "NAME\t0\tX"), r)
            && !pbtrf::parseVoicesText(with(5, "NAME\t1\tAgain"), r) && !pbtrf::parseVoicesText(with(4, "NAME\t1\t"), r)
            && !pbtrf::parseVoicesText(with(4, "NAME\t1\t" + std::string(65, 'x')), r) && !pbtrf::parseVoicesText(with(6, "DOCX\t\t1\t2\t3"), r)
            && !pbtrf::parseVoicesText(with(9, "SEG\t4000\t9000\t4"), r) && !pbtrf::parseVoicesText(with(9, "SEG\t4000\t4000\t1"), r)
            && !pbtrf::parseVoicesText(with(9, "SEG\t1000\t1200\t1"), r) && !pbtrf::parseVoicesText(with(9, "SEG\t4000\t9000\t-1"), r)
            && !pbtrf::parseVoicesText(with(11, "END\t5"), r) && !pbtrf::parseVoicesText(without(11), r)
            && !pbtrf::parseVoicesText(text + "SEG\t9100\t9200\t1\n", r) && !pbtrf::parseVoicesText(with(2, "WHO\t1\t2"), r)
            && !pbtrf::parseVoicesText(with(7, "SEG\t0\t1500\t1\t9"), r) && !pbtrf::parseVoicesText(std::string(), r)
            && pbtrf::parseVoicesText(with(4, "NAME\t1\t" + std::string(64, 'x')), r);
        ok("[11] STRICT: another version, no SRC / DIAR, on with 0 speakers, off with speakers, > 64 speakers, a rename 0 / twice / "
           "empty / > 64 bytes, a DOCX with no path, a piece past the speakers / empty / back in time / junk, END wrong or missing, "
           "a line after END, a line it does not know, an extra field -> a MISS; a 64-byte name kept", strict);
        // (B9: the DIAR line ALONE -- in the record above, its pieces' speakers refused "on 0" / "off 2" first: here the
        // pieces name nobody, so only the DIAR rule can refuse them)
        const std::vector<std::string> O = lines(pbtrf::packVoices(off));
        size_t di = 0;
        for (size_t i = 0; i < O.size(); ++i) if (O[i].rfind("DIAR\t", 0) == 0) di = i;
        auto withO = [&](const std::string& line) { std::string o; for (size_t i = 0; i < O.size(); ++i) o += (i == di ? line : O[i]) + "\n"; return o; };
        ok("[11] the DIAR line alone: \"on\" with 0 speakers, \"off\" with 2 -> a MISS, even with pieces that name nobody; as written, read",
           di > 0 && pbtrf::parseVoicesText(withO(O[di]), r) && !pbtrf::parseVoicesText(withO("DIAR\ton\tcampplus\t0"), r)
           && !pbtrf::parseVoicesText(withO("DIAR\toff\twhy\t2"), r), di ? O[di] : std::string("no DIAR line"));
        uint64_t big64 = 0;
        ok("[11] a number in the record: up to 18446744073709551615 (a fingerprint takes 20 digits), one more or junk refused",
           pbtrf::digitsU64("18446744073709551615", big64) && big64 == UINT64_MAX && !pbtrf::digitsU64("18446744073709551616", big64)
           && !pbtrf::digitsU64("99999999999999999999", big64) && !pbtrf::digitsU64("000000000000000000001", big64)
           && pbtrf::digitsU64("0", big64) && big64 == 0 && !pbtrf::digitsU64("", big64) && !pbtrf::digitsU64("+1", big64));
        // the PEEK: the head lines of a record with 20000 pieces (> 16 KB), its renames and its DOCX, without its pieces
        pbtrf::VoiceRec big = v;
        big.segs.clear();
        for (uint64_t i = 0; i < 20000; ++i) big.segs.push_back({ i * 1000, i * 1000 + 900, (uint32_t)(1 + i % 3) });
        const std::wstring vdir = base + L"\\u3";
        ::CreateDirectoryW(vdir.c_str(), nullptr);
        const std::wstring vf = vdir + L"\\big.voices";
        pbtrf::VoiceRec pk, full;
        const std::string bigText = pbtrf::packVoices(big);
        ok("[11] the peek (16 KB, the line it cuts dropped): the head of a 20000-piece record -- names, DOCX, no piece",
           bigText.size() > 300000 && put(vf, bigText) && pbtrf::peekVoices(vf, pk) && pk.names == v.names && pk.docxSig == v.docxSig
           && pk.speakers == 3 && pk.segs.empty() && pbtrf::parseVoicesText(get(vf), full) && full.segs.size() == 20000);
        ok("[11] the peek of a file that is not there / not a record: false",
           !pbtrf::peekVoices(vdir + L"\\none.voices", pk) && put(vdir + L"\\junk.voices", "hello\n") && !pbtrf::peekVoices(vdir + L"\\junk.voices", pk));
        big.segs.push_back({ 20000000, 20000100, 1 });
        ok("[11] more than kVoiceSpanMax pieces: a miss", !pbtrf::parseVoicesText(pbtrf::packVoices(big), full));
        // the .docx name
        const std::vector<std::wstring> c = pbtrf::transcriptCandidates(L"D:\\Films", L"interview", L"mkv", false);
        const std::vector<std::wstring> cs = pbtrf::transcriptCandidates(L"D:\\Films", L"interview", L"mkv", true);
        ok("[11] the names: \"<name> - transcript.docx\", then \"(SuVtitle)\", then \"(SuVtitle 2..9)\"; \"<name>.<ext>\" beside a namesake",
           c.size() == 10 && c[0] == L"D:\\Films\\interview - transcript.docx" && c[1] == L"D:\\Films\\interview - transcript (SuVtitle).docx"
           && c[2] == L"D:\\Films\\interview - transcript (SuVtitle 2).docx" && c[9] == L"D:\\Films\\interview - transcript (SuVtitle 9).docx"
           && cs[0] == L"D:\\Films\\interview.mkv - transcript.docx");
        std::vector<std::wstring> present, ours;
        auto choose = [&]() {
            return pbtrf::chooseTranscriptPath(L"D:\\Films", L"interview", L"mkv", false,
                [&](const std::wstring& p) { return std::find(present.begin(), present.end(), p) != present.end(); },
                [&](const std::wstring& p) { return std::find(ours.begin(), ours.end(), p) != ours.end(); });
        };
        const std::wstring first = choose();
        present = { c[0] };
        const std::wstring second = choose();
        present = { c[0], c[1] }; ours = { c[1] };
        const std::wstring oursAgain = choose();
        present = c; ours.clear();
        const std::wstring none = choose();
        ok("[11] the choice: absent -> the plain name; someone else's -> \"(SuVtitle)\"; ours for this media -> replaced; all ten "
           "someone else's -> no name (never an overwrite)",
           first == c[0] && second == c[1] && oursAgain == c[1] && none.empty());
        ok("[11] the registry's kind for a transcript: \"transcript\" (no language code can be it)",
           std::string(pbtrf::transcriptRegKind()) == "transcript");
    }

    // ---- [12] U3: the transcript itself (TrTranscript.h): lines, pieces, speakers, turns, names, stamps ---------------------
    std::printf("[12] U3: TrTranscript.h -- dialogue lines, pieces, the voice pass' fields, speakers, turns, names\n");
    {
        namespace tp = pbt::trpipe;
        auto parts = [](const std::string& t) {
            std::string o;
            for (const auto& p : pbtrs::dashParts(t)) o += std::string(p.dash ? "D:" : "-:") + p.text + "|";
            return o;
        };
        const std::string em = "\xe2\x80\x94";                    // U+2014
        ok("[12] a dialogue cue -> its lines, the dashes gone (\"- I agree. - So let's move on.\")",
           parts("- I agree. - So let's move on.") == "D:I agree.|D:So let's move on.|", parts("- I agree. - So let's move on."));
        ok("[12] em dashes, a dash then a digit, leading spaces, a dash glued to its word",
           parts(em + " Bonjour. " + em + " Salut.") == "D:Bonjour.|D:Salut.|" && parts("Ready? - 5 minutes.") == "-:Ready?|D:5 minutes.|"
           && parts("   - A. -B.") == "D:A.|D:B.|", parts("   - A. -B."));
        ok("[12] NEVER cut: \"Est-ce\", \"well-known\", \"Jean-Pierre\", \"COVID-19\", \"it was - how to say - hard\", \"going to --\", "
           "a dash at the end, \"-oui\"",
           parts("Est-ce que tu viens ?") == "-:Est-ce que tu viens ?|" && parts("a well-known fact") == "-:a well-known fact|"
           && parts("Jean-Pierre arrive.") == "-:Jean-Pierre arrive.|" && parts("le COVID-19 encore") == "-:le COVID-19 encore|"
           && parts("it was - how to say - hard") == "-:it was - how to say - hard|" && parts("I was going to --") == "-:I was going to --|"
           && parts("Wait -") == "-:Wait -|" && parts("-oui, bien sur.") == "-:-oui, bien sur.|" && parts("") == "-:|",
           parts("I was going to --") + parts("Wait -") + parts("-oui, bien sur."));
        ok("[12] the first line opened by \"- \" then a lowercase word still counts (TrFileJob's opensTurn: the character right after "
           "the dash is a space)", parts("- yes. - No.") == "D:yes.|D:No.|", parts("- yes. - No."));
        // pieces: cues out of order, a dialogue cue shared by reading time, an empty cue, a dash cue too short to share
        std::vector<tp::WireCue> cues(6);
        cues[0].startMs = 10000; cues[0].endMs = 14000; cues[0].src = "- Yes. - So let's move on now."; cues[0].lang = "en";
        cues[1].startMs = 0;     cues[1].endMs = 4000;  cues[1].src = "Good morning everyone."; cues[1].lang = "en";
        cues[2].startMs = 5000;  cues[2].endMs = 6000;  cues[2].src = "   "; cues[2].lang = "en";
        cues[3].startMs = 15000; cues[3].endMs = 15001; cues[3].src = "- A. - B."; cues[3].lang = "en";
        cues[4].startMs = 16000; cues[4].endMs = 17000; cues[4].src = "- No."; cues[4].lang = "en";
        cues[5].startMs = 17500; cues[5].endMs = 19000; cues[5].src = "\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd\xe3\x80\x82"; cues[5].lang = "zh";
        const std::vector<pbtrs::Piece> P = pbtrs::piecesOf(cues);
        // "Yes." = 4 narrow -> 4/17 + 0.05; "So let's move on now." = 17 narrow -> 1 + 0.05: the cut at 10000 + 4000 * 0.2853/1.3353
        const uint64_t cut = 10000 + (uint64_t)(4000.0 * (4.0 / 17 + 0.05) / (4.0 / 17 + 0.05 + 17.0 / 17 + 0.05));
        ok("[12] the pieces: start order, the blank cue gone, the dialogue cue in TWO pieces by reading time, touching",
           P.size() == 6 && P[0].text == "Good morning everyone." && P[1].text == "Yes." && P[2].text == "So let's move on now."
           && P[1].s == 10000 && P[1].e == cut && P[2].s == cut && P[2].e == 14000 && P[1].cue == 2 && P[2].cue == 2,
           P.size() > 2 ? std::to_string(P[1].e) + " vs " + std::to_string(cut) : std::to_string(P.size()));
        ok("[12] the KNOWN changes: the second line of a cue; a dash line right after a dash line; not a plain cue",
           !P[0].change && !P[1].change && P[2].change && P[3].text == "A. B." && P[3].change && P[4].text == "No." && P[4].dash
           && P[4].change && !P[5].change, std::to_string(P.size()));
        ok("[12] a dialogue cue with no room for a millisecond a line: ONE piece, its lines joined", P[3].s == 15000 && P[3].e == 15001);
        std::string spans, changes;
        std::vector<int> sent;
        std::vector<pbtrs::Piece> Q = P;
        Q[3].e = Q[3].s;                                          // a piece of no length: not sent
        Q.insert(Q.begin() + 5, Q[4]);                            // a piece overlapping the one before it: not sent
        Q[5].change = true;
        pbtrs::voiceFields(Q, spans, changes, sent);
        ok("[12] FVOICE's fields: the sent pieces' spans, the changes as indexes INTO WHAT IS SENT, a piece of no length or "
           "overlapping not sent",
           spans == "0-4000,10000-" + std::to_string(cut) + "," + std::to_string(cut) + "-14000,16000-17000,17500-19000"
           && changes == "2,3" && sent == std::vector<int>({ 0, 1, 2, -1, 3, -1, 4 }), spans + " / " + changes);
        // speakers: the segment overlapped MOST, none -> the previous (the first: the next), a known change never its predecessor's
        std::vector<pbtrs::Piece> S(6);
        const uint64_t se[6][2] = { { 0, 1000 }, { 1000, 4000 }, { 4000, 9000 }, { 9000, 9500 }, { 9500, 9800 }, { 20000, 21000 } };
        for (size_t i = 0; i < 6; ++i) { S[i].s = se[i][0]; S[i].e = se[i][1]; S[i].text = "w"; }
        S[4].change = true;
        const std::vector<pbtrf::VoiceSeg> segs = { { 1000, 5000, 1 }, { 5000, 9600, 2 } };
        uint64_t un = 0, tot = 0;
        const std::vector<uint32_t> sp = pbtrs::speakersOf(S, segs, &un, &tot);
        ok("[12] speakersOf: the first piece (no segment) the next one's; 4000-9000 -> speaker 2 (4000 ms vs 1000); 9500-9800 "
           "a KNOWN change from 2 -> the most recent OTHER (1); an unheard piece -> the previous one's; the unheard time",
           sp == std::vector<uint32_t>({ 1, 1, 2, 2, 1, 1 }) && un == 2000 && tot == 9800 + 1000,
           std::to_string(sp[0]) + std::to_string(sp[1]) + std::to_string(sp[2]) + std::to_string(sp[3]) + std::to_string(sp[4])
           + std::to_string(sp[5]) + " un " + std::to_string(un));
        std::vector<pbtrs::Piece> S2(2);
        S2[0].s = 0; S2[0].e = 1000; S2[1].s = 1000; S2[1].e = 2000; S2[1].change = true;
        ok("[12] a known change with no other speaker heard before: no name (never the predecessor's)",
           pbtrs::speakersOf(S2, { { 0, 2000, 1 } }) == std::vector<uint32_t>({ 1, 0 }));
        // turns
        std::vector<pbtrs::Piece> T(7);
        const uint64_t te[7][2] = { { 0, 2000 }, { 2500, 4000 }, { 4000, 6000 }, { 13000, 15000 }, { 15500, 17000 }, { 17000, 18000 },
                                    { 18500, 19000 } };
        const char* tt[7] = { "Hello.", "How are you?", "Fine.", "Later on.", "Me too.", "Still me.", "Ok." };
        for (size_t i = 0; i < 7; ++i) { T[i].s = te[i][0]; T[i].e = te[i][1]; T[i].text = tt[i]; T[i].lang = "en"; }
        const std::vector<uint32_t> ts = { 1, 1, 2, 2, 3, 4, 1 };
        std::map<uint32_t, std::string> names = { { 2, "Marie" } };
        std::vector<pbtrs::Turn> turns = pbtrs::buildTurns(T, ts, names, pbtc::SourceTc{});
        ok("[12] turns: a new one at each change of name, one speaker's pieces joined, a pause >= 6 s a new turn (its own stamp), "
           "\"Speaker <n>\" or the rename",
           turns.size() == 6 && turns[0].name == "Speaker 1" && turns[0].text == "Hello. How are you?" && turns[0].stamp == "[00:00:00]"
           && turns[1].name == "Marie" && turns[1].text == "Fine." && turns[2].name == "Marie" && turns[2].stamp == "[00:00:13]"
           && turns[3].name == "Speaker 3" && turns[4].name == "Speaker 4" && turns[5].name == "Speaker 1" && turns[1].spk == 2,
           std::to_string(turns.size()));
        names = { { 3, "Paul" }, { 4, " paul " } };
        turns = pbtrs::buildTurns(T, ts, names, pbtc::SourceTc{});
        ok("[12] two speakers given the SAME name (trimmed, case folded) are one person: their consecutive pieces make ONE turn",
           turns.size() == 5 && turns[3].name == "Paul" && turns[3].text == "Me too. Still me." && turns[3].spk == 3, std::to_string(turns.size()));
        pbtc::SourceTc src;
        src.found = true; src.hasRate = true; src.fpsNum = 25; src.fpsDen = 1; src.base = 25; src.startFrames = 900000;
        turns = pbtrs::buildTurns(T, ts, {}, src);
        ok("[12] a SOURCE timecode (10:00:00:00 at 25): the stamps are the file's -- the first [10:00:00:00], the third (13 s in) "
           "[10:00:13:00]", turns[0].stamp == "[10:00:00:00]" && turns[2].stamp == "[10:00:13:00]", turns[0].stamp + " " + turns[2].stamp);
        const std::vector<uint32_t> zero(7, 0);
        turns = pbtrs::buildTurns(T, zero, {}, pbtc::SourceTc{});
        ok("[12] no names at all: a paragraph per pause >= 2 s, no name shown",
           turns.size() == 2 && turns[0].name.empty() && turns[0].text == "Hello. How are you? Fine." && turns[1].stamp == "[00:00:13]",
           std::to_string(turns.size()));
        std::vector<pbtrs::Piece> L90;
        for (uint64_t k = 0; k < 30; ++k) { pbtrs::Piece p; p.s = k * 4000; p.e = k * 4000 + 3900; p.text = (k == 22) ? "and so" : "Sentence."; L90.push_back(p); }
        turns = pbtrs::buildTurns(L90, std::vector<uint32_t>(30, 1), {}, pbtc::SourceTc{});
        ok("[12] one speaker for 2 minutes: the turn is cut once it lasts 90 s, at its next sentence end",
           turns.size() == 2 && turns[1].s == 96000 && turns[0].name == "Speaker 1" && turns[1].name == "Speaker 1",
           turns.size() == 2 ? std::to_string(turns[1].s) : std::to_string(turns.size()));
        std::string a = "\xe5\xa4\xa7\xe5\xae\xb6", b = "Hello";
        pbtrs::joinInto(a, "\xe5\xa5\xbd");
        pbtrs::joinInto(b, "world");
        ok("[12] joined like the documents: no space between two CJK ends, a space otherwise",
           a == "\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd" && b == "Hello world");
        std::string clean, why;
        const bool names1 = pbtrs::nameOk("  Marie  ", clean, why) && clean == "Marie" && pbtrs::nameOk("", clean, why) && clean.empty()
            && pbtrs::nameOk("\xe7\x8e\x8b\xe8\x8a\xb3", clean, why) && pbtrs::nameOk(std::string(64, 'x'), clean, why);
        const bool names2 = !pbtrs::nameOk(std::string(65, 'x'), clean, why) && !why.empty() && !pbtrs::nameOk("a\x01" "b", clean, why)
            && !pbtrs::nameOk("a\xc3", clean, why) && !pbtrs::nameOk("\xc2\x85", clean, why) && !pbtrs::nameOk("a\xe2\x80\xa8" "b", clean, why)
            && !pbtrs::nameOk("a\x7f", clean, why) && !pbtrs::nameOk("\xc3\x28", clean, why);
        ok("[12] a typed name: trimmed, \"\" = back to \"Speaker n\", UTF-8 kept, 64 bytes at most; a control character, broken "
           "UTF-8, a line separator refused (with a sentence)", names1 && names2);
        std::vector<tp::WireCue> lc(3);
        lc[0].lang = "en"; lc[0].src = std::string(400, 'a');
        lc[1].lang = "zh"; lc[1].src = "\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd";
        lc[2].lang = "fr"; lc[2].src = "oui";
        const std::vector<std::string> heard = pbtrs::languagesHeard(lc);
        ok("[12] the languages heard: by their share (a CJK character ~3 letters), each >= 5 %, the largest first",
           heard == std::vector<std::string>({ "en", "zh" }), heard.empty() ? "" : heard[0]);
        turns = pbtrs::buildTurns(T, ts, { { 2, "Marie" } }, pbtc::SourceTc{});
        const uint64_t fp1 = pbtrs::fingerprint(turns);
        std::vector<pbtrs::Turn> t2 = turns;
        t2[1].name = "Marie Curie";
        std::vector<pbtrs::Turn> t3 = turns;
        t3[0].text += " ";
        ok("[12] the fingerprint: the same transcript the same number; a rename or a text edit another",
           fp1 == pbtrs::fingerprint(pbtrs::buildTurns(T, ts, { { 2, "Marie" } }, pbtc::SourceTc{})) && fp1 != pbtrs::fingerprint(t2)
           && fp1 != pbtrs::fingerprint(t3));
        std::vector<pbtrs::Turn> nm(4);
        nm[0].name = "Marie"; nm[1].name = " marie"; nm[2].name = "Paul"; nm[3].name = "";
        ok("[12] namedSpeakers: the distinct names (folded: one name twice = one person), no name not counted",
           pbtrs::namedSpeakers(nm) == 2 && pbtrs::namedSpeakers({}) == 0);
        // the Word file's text around the turns (docTextOf): what the bridge hands DocxWriter.cpp
        pbtrs::TranscriptOut to;
        to.name = L"meeting.wav"; to.durMs = 104000; to.speakers = 4; to.namesOn = true; to.langs = { "en", "zh" };
        const std::string zhName = "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87";
        auto native = [&](const std::string& c) { return c == "en" ? std::string("English") : c == "zh" ? zhName : c; };
        const pbtrs::DocText dt = pbtrs::docTextOf(to, "2026-11-02 14:31", "1.1.0", native);
        ok("[12] the Word file's header: the title, \"Transcript - 1:44 - 4 speakers\", the date and the languages (a non-Latin one "
           "with its English name), the timecode, the note",
           dt.title == "meeting.wav" && dt.subtitle == "Transcript \xc2\xb7 1:44 \xc2\xb7 4 speakers" && dt.header.size() == 2
           && dt.header[0] == "Date: 2026-11-02 14:31 \xc2\xb7 Languages: English, " + zhName + " (Simplified Chinese)"
           && dt.header[1] == "Timecode: from the start of the file."
           && dt.note == "Automatic transcription by SuVtitle 1.1.0, to be checked. Speakers recognised by voice.",
           dt.subtitle + " | " + (dt.header.empty() ? std::string() : dt.header[0]));
        to.namesOn = false; to.speakers = 0; to.why = "speaker diarization is off (SPKON 0)"; to.partial = true;
        to.doneMs = 2590000; to.durMs = 3727000; to.tc = src;
        const pbtrs::DocText d2 = pbtrs::docTextOf(to, "2026-11-02 14:31", "1.1.0", native);
        ok("[12] ...a PARTIAL one, no names, a source timecode: \"Partial: 0:43:10 of 1:02:07 analysed.\", why no names, the source TC",
           d2.subtitle == "Transcript \xc2\xb7 1:02:07" && d2.header.size() == 4
           && d2.header[1] == "Timecode: source, 10:00:00:00 at 25 fps, read from the file."
           && d2.header[2] == "Partial: 0:43:10 of 1:02:07 analysed."
           && d2.header[3] == "Speakers could not be recognised: speaker diarization is off (SPKON 0)."
           && d2.note == "Automatic transcription by SuVtitle 1.1.0, to be checked.", d2.header.size() > 2 ? d2.header[2] : "");
    }

    // ---- [13] U2b (02/11/2026, docs/PROGRESS_U2b.md -- the user: remove an imported model, "et demande si on supprime le model en
    //      plus"): AppConfig.h removeImportedModel -- the MODEL line gone, MODELUSE back to the default when it named it, every other
    //      line kept; the bridge resolves the page's id against THIS list (never a path from the page). Sentinels: cmut_u2.py.
    std::printf("[13] U2b: removing an imported model from app.txt (AppConfig.h removeImportedModel)\n");
    {
        namespace tp = pbt::trpipe;
        const std::string A = "D:\\Models\\ggml-base.bin", B = "D:\\Models\\ggml-medium.bin", C = "E:\\LLM\\gemma-3-4b-it-Q4_K_M.gguf";
        const std::string old = "# SuVtitle app v1\nWINPOS\t10\t20\t900\t600\t0\t0\nMODEL\tasr\t" + tp::esc(A) + "\nLIVEIN\tin:{x}\tMe\t0\t0.0\n"
                              + "MODEL\tasr\t" + tp::esc(B) + "\nMODEL\tmt\t" + tp::esc(C) + "\nMODELUSE\tasr\t" + tp::esc(B) + "\n"
                              + "MODELUSE\tmt\t" + tp::esc(C) + "\nFUTURE\tkeep me\n";
        pbapp::ImportedModels m = pbapp::parseModels(old);
        const bool r1 = pbapp::removeImportedModel(m, "asr", B);
        const std::string after = pbapp::packModels(old, m);
        const pbapp::ImportedModels back = pbapp::parseModels(after);
        ok("[13] the CHOSEN transcription model removed: its MODEL line gone, its MODELUSE gone (back to the default); the other "
           "model, the translation model and its choice kept",
           r1 && back.asr.size() == 1 && back.asr[0] == A && back.useAsr.empty() && back.mt.size() == 1 && back.mt[0] == C
           && back.useMt == C && after.find(tp::esc(B)) == std::string::npos, after);
        ok("[13] ...every other line of app.txt kept, in order, one header (WINPOS, LIVEIN, a line of a later version)",
           after.find("# SuVtitle app v1\n") == 0 && after.find("# SuVtitle", 1) == std::string::npos
           && after.find("WINPOS\t10\t20\t900\t600\t0\t0\n") != std::string::npos
           && after.find("LIVEIN\tin:{x}\tMe\t0\t0.0\n") != std::string::npos && after.find("FUTURE\tkeep me\n") != std::string::npos
           && after.find("WINPOS") < after.find("LIVEIN") && after.find("LIVEIN") < after.find("FUTURE"), after);
        pbapp::ImportedModels n = pbapp::parseModels(old);
        const bool r2 = pbapp::removeImportedModel(n, "asr", "d:\\MODELS\\GGML-BASE.BIN");   // another case of A: the same file
        ok("[13] a model that is NOT the chosen one (named in another case: Windows' rule) removed: the choice stands",
           r2 && n.asr.size() == 1 && n.asr[0] == B && n.useAsr == B && n.useMt == C);
        pbapp::ImportedModels k = pbapp::parseModels(old);
        const bool r3 = pbapp::removeImportedModel(k, "mt", A), r4 = pbapp::removeImportedModel(k, "llm", C),
                   r5 = pbapp::removeImportedModel(k, "asr", "D:\\Models\\not-listed.bin");
        ok("[13] refused, nothing changed: a model of the OTHER kind's list, a kind word that is not asr / mt, a path not listed -- "
           "and a rewrite with nothing changed is the same bytes",
           !r3 && !r4 && !r5 && k.asr.size() == 2 && k.mt.size() == 1 && k.useAsr == B && k.useMt == C
           && pbapp::packModels(after, back) == after);
        pbapp::ImportedModels last = pbapp::parseModels(old);
        pbapp::removeImportedModel(last, "mt", C);
        const std::string noMt = pbapp::packModels(old, last);
        ok("[13] the LAST model of a kind removed: no MODEL / MODELUSE line of that kind left; the other kind's lines kept",
           noMt.find("MODEL\tmt\t") == std::string::npos && noMt.find("MODELUSE\tmt\t") == std::string::npos
           && pbapp::parseModels(noMt).asr.size() == 2 && pbapp::parseModels(noMt).useAsr == B && pbapp::parseModels(noMt).mt.empty());
        // a HAND-EDITED app.txt: the MODELUSE before its MODEL, the same file twice in two cases (one model), a line of a bad kind
        const std::string hand = "# SuVtitle app v1\nMODELUSE\tasr\t" + tp::esc(A) + "\nMODEL\tasr\t" + tp::esc(A) + "\nMODEL\tasr\t"
                               + tp::esc("d:\\models\\GGML-BASE.BIN") + "\nMODEL\ttts\t" + tp::esc("D:\\y.bin") + "\nMODEL\tasr\t"
                               + tp::esc(B) + "\nWINPOS\t1\t2\t900\t600\t0\t0\n";
        pbapp::ImportedModels h = pbapp::parseModels(hand);
        const bool r6 = pbapp::removeImportedModel(h, "asr", A);
        const std::string handAfter = pbapp::packModels(hand, h);
        ok("[13] a HAND-EDITED app.txt (the choice before its model, the same file twice in two cases, a bad kind's line): removed "
           "= no MODEL line of that file in ANY case, no MODELUSE; the other model and WINPOS kept",
           r6 && h.asr.size() == 1 && h.asr[0] == B && h.useAsr.empty()
           && handAfter.find("ggml-base") == std::string::npos && handAfter.find("GGML-BASE") == std::string::npos
           && handAfter.find("MODELUSE") == std::string::npos && handAfter.find("WINPOS\t1\t2") != std::string::npos
           && pbapp::parseModels(handAfter).asr.size() == 1, handAfter);
    }

    // ---- [FX] the review's fix round (02/11/2026, docs/PROGRESS_FIX.md): TrFileStore.h -- F1 the Word file's timecode kept in the
    //      voices record (DOCXTC); F2 the user's rule, ONE Word file per media AND sound track (the track's names; registryOwnsFor:
    //      for this media, this track AND this analysis -- an entry with no key owns nothing; registryTrackDocx); F4 the integrator's
    //      MECHANICAL criterion, on REAL files in this smoke's scratch tree: WHERE an AV-safe write failed (WriteFail) and why a file
    //      refused (targetRefusal) -- a file held FILE_SHARE_READ the way Word holds a .docx, a read-only one, a folder that is not there
    std::printf("[FX] the fix round: DOCXTC, one Word file per sound track and its ownership, where a write failed (real files)\n");
    {
        auto sameTc = [](const pbtrf::VoiceTc& x, const pbtrf::VoiceTc& y) {
            return x.found == y.found && x.origin == y.origin && x.hasRate == y.hasRate && x.startFrames == y.startFrames
                && x.fpsNum == y.fpsNum && x.fpsDen == y.fpsDen && x.base == y.base && x.drop == y.drop
                && x.startSamples == y.startSamples && x.sampleRate == y.sampleRate;
        };
        pbtrf::VoiceRec v;
        v.path = "D:\\Rushes\\take1.wav"; v.size = 1000; v.mtime = 2000; v.on = true; v.model = "campplus"; v.speakers = 2;
        v.docxPath = "D:\\Rushes\\take1 - transcript.docx"; v.docxSize = 3000; v.docxMtime = 4000; v.docxSig = 5000;
        v.docxTc.found = true; v.docxTc.origin = "bwf+ixml"; v.docxTc.hasRate = true; v.docxTc.startFrames = 900000;
        v.docxTc.fpsNum = 25; v.docxTc.fpsDen = 1; v.docxTc.base = 25; v.docxTc.startSamples = 1728000000ull; v.docxTc.sampleRate = 48000;
        v.segs = { { 0, 1000, 1 }, { 1000, 2000, 2 } };
        const std::string text = pbtrf::packVoices(v);
        pbtrf::VoiceRec r;
        ok("[FX] F1: the Word file's timecode kept with it -- DOCXTC right after DOCX, read back field for field (10:00:00:00 at 25 fps)",
           text.find("\nDOCXTC\tbwf+ixml\t1\t900000\t25\t1\t25\t0\t1728000000\t48000\n") != std::string::npos
           && text.find("\nDOCXTC\t") > text.find("\nDOCX\t") && pbtrf::parseVoicesText(text, r) && sameTc(r.docxTc, v.docxTc), text);
        pbtrf::VoiceRec df = v, nr = v, rel = v, r2, r3, r4;
        df.docxTc = pbtrf::VoiceTc{};
        df.docxTc.found = true; df.docxTc.origin = "mov"; df.docxTc.hasRate = true; df.docxTc.startFrames = 107892;
        df.docxTc.fpsNum = 30000; df.docxTc.fpsDen = 1001; df.docxTc.base = 30; df.docxTc.drop = true;
        nr.docxTc = pbtrf::VoiceTc{};
        nr.docxTc.found = true; nr.docxTc.origin = "bwf"; nr.docxTc.startSamples = 1728000000ull; nr.docxTc.sampleRate = 48000;
        rel.docxTc = pbtrf::VoiceTc{};
        ok("[FX] F1: 29.97 drop-frame (a MOV) and a BWF with no frame rate read back the same; file-relative stamps write NO DOCXTC and "
           "read back as none (every record written before reads as it did)",
           pbtrf::parseVoicesText(pbtrf::packVoices(df), r2) && sameTc(r2.docxTc, df.docxTc)
           && pbtrf::parseVoicesText(pbtrf::packVoices(nr), r3) && sameTc(r3.docxTc, nr.docxTc)
           && pbtrf::packVoices(rel).find("DOCXTC") == std::string::npos && pbtrf::parseVoicesText(pbtrf::packVoices(rel), r4) && !r4.docxTc.found);
        auto withTc = [&](const std::string& line) {
            const size_t at = text.find("DOCXTC\t"), e = text.find('\n', at);
            return text.substr(0, at) + line + text.substr(e);
        };
        auto beforeDocx = [&]() {                           // the DOCXTC line moved in front of its DOCX
            std::string o = text;
            const size_t at = o.find("DOCXTC\t"), e = o.find('\n', at);
            const std::string line = o.substr(at, e - at + 1);
            o.erase(at, e - at + 1);
            return o.insert(o.find("DOCX\t"), line);
        };
        const bool strict = pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t25\t1\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(beforeDocx(), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t25\t1\t25\t0\t1728000000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tsmpte\t1\t900000\t25\t1\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t2\t900000\t25\t1\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t90x000\t25\t1\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t25\t0\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t25\t1\t0\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t24\t25\t25\t0\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf+ixml\t1\t900000\t25\t1\t25\t1\t1728000000\t48000"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf\t0\t0\t0\t1\t0\t0\t1728000000\t0"), r)
            && !pbtrf::parseVoicesText(withTc("DOCXTC\tbwf\t0\t0\t0\t1\t0\t1\t1728000000\t48000"), r);
        ok("[FX] F1: DOCXTC STRICT -> a miss: before its DOCX, 9 fields, an origin the reader never writes, a flag not 0 / 1, junk in a "
           "number, a rate with no denominator, no base, under 1 fps, drop-frame on 25 fps, no rate and no sample rate, drop-frame with "
           "no rate", strict);
        const std::wstring fxdir = base + L"\\fx";
        ::CreateDirectoryW(fxdir.c_str(), nullptr);
        pbtrf::VoiceRec pk;
        ok("[FX] F1: the head peek reads DOCXTC (it sits before the pieces)",
           put(fxdir + L"\\tc.voices", text) && pbtrf::peekVoices(fxdir + L"\\tc.voices", pk) && sameTc(pk.docxTc, v.docxTc));

        // F2: one Word file per sound track -- its names
        const std::vector<std::wstring> t0 = pbtrf::transcriptCandidates(L"D:\\Films", L"film", L"mkv", false, 0);
        const std::vector<std::wstring> t1 = pbtrf::transcriptCandidates(L"D:\\Films", L"film", L"mkv", false, 1);
        const std::vector<std::wstring> t2s = pbtrf::transcriptCandidates(L"D:\\Films", L"film", L"mkv", true, 2);
        ok("[FX] F2: the first sound track keeps \"<name> - transcript.docx\"; track N (ord + 1: the track menu's number) \"<name> - "
           "transcript (track N).docx\", then its own \"(track N) (SuVtitle)\" .. \"(SuVtitle 9)\"; \"<name>.<ext>\" beside a namesake",
           t0.size() == 10 && t0[0] == L"D:\\Films\\film - transcript.docx" && t0[1] == L"D:\\Films\\film - transcript (SuVtitle).docx"
           && t1.size() == 10 && t1[0] == L"D:\\Films\\film - transcript (track 2).docx"
           && t1[1] == L"D:\\Films\\film - transcript (track 2) (SuVtitle).docx" && t1[9] == L"D:\\Films\\film - transcript (track 2) (SuVtitle 9).docx"
           && t2s[0] == L"D:\\Films\\film.mkv - transcript (track 3).docx");
        std::vector<std::wstring> present, mine;
        auto chooseT = [&](uint32_t ord) {
            return pbtrf::chooseTranscriptPath(L"D:\\Films", L"film", L"mkv", false,
                [&](const std::wstring& p) { return std::find(present.begin(), present.end(), p) != present.end(); },
                [&](const std::wstring& p) { return std::find(mine.begin(), mine.end(), p) != mine.end(); }, ord);
        };
        const std::wstring c1 = chooseT(1);
        present = { t1[0] };
        const std::wstring c2 = chooseT(1);
        ok("[FX] F2: the choice on track 2: absent -> \"(track 2)\"; someone else's -> \"(track 2) (SuVtitle)\"; the first track's names "
           "never come into it", c1 == t1[0] && c2 == t1[1] && chooseT(0) == t0[0]);
        // registryOwnsFor / registryTrackDocx / registryTranscriptEntry
        const std::wstring media = L"D:\\Films\\Film.mkv", docx = L"D:\\Films\\film - transcript.docx";
        std::vector<pbtrf::RegEntry> reg;
        pbtrf::registryPut(reg, docx, 100, 200, media, pbtrf::transcriptRegKind(), "KEY-FR-EN", 0);
        pbtrf::RegEntry keyless;                            // an OLDER registry's entry: path, identity, media -- no key
        keyless.path = pbtrf::toUtf8(pbtrf::lowerW(L"D:\\Films\\film - transcript (SuVtitle).docx"));
        keyless.size = 300; keyless.mtime = 400; keyless.media = pbtrf::toUtf8(pbtrf::lowerW(media));
        keyless.lang = pbtrf::transcriptRegKind();         // (a transcript's: only its missing key may set it apart -- FX sentinel)
        reg.push_back(keyless);
        ok("[FX] F2: OURS FOR THIS ANALYSIS -- the path (any case), identity, media, key AND track; another analysis' key, another "
           "track, an identity changed since, another media: not ours",
           pbtrf::registryOwnsFor(reg, docx, 100, 200, media, "KEY-FR-EN", 0)
           && pbtrf::registryOwnsFor(reg, L"d:\\FILMS\\Film - Transcript.docx", 100, 200, L"d:\\films\\film.MKV", "KEY-FR-EN", 0)
           && !pbtrf::registryOwnsFor(reg, docx, 100, 200, media, "KEY-FR-ZH", 0) && !pbtrf::registryOwnsFor(reg, docx, 100, 200, media, "KEY-FR-EN", 1)
           && !pbtrf::registryOwnsFor(reg, docx, 101, 200, media, "KEY-FR-EN", 0) && !pbtrf::registryOwnsFor(reg, docx, 100, 201, media, "KEY-FR-EN", 0)
           && !pbtrf::registryOwnsFor(reg, docx, 100, 200, L"D:\\Films\\Other.mkv", "KEY-FR-EN", 0));
        ok("[FX] F2 (the integrator's condition 2): an entry with NO key owns nothing -- never replaced by itself; an empty key owns "
           "nothing either (the .srt rule, registryOwns, unchanged: it still counts the entry)",
           !pbtrf::registryOwnsFor(reg, L"D:\\Films\\film - transcript (SuVtitle).docx", 300, 400, media, "KEY-FR-EN", 0)
           && !pbtrf::registryOwnsFor(reg, L"D:\\Films\\film - transcript (SuVtitle).docx", 300, 400, media, "", 0)
           && pbtrf::registryOwns(reg, L"D:\\Films\\film - transcript (SuVtitle).docx", 300, 400, media));
        std::vector<std::wstring> there = { docx, L"D:\\Films\\film - transcript (SuVtitle).docx" };
        auto isThere = [&](const std::wstring& p) {
            for (const auto& x : there) if (pbtrf::lowerW(x) == pbtrf::lowerW(p)) return true;
            return false;
        };
        std::wstring got;
        const std::string k0 = pbtrf::registryTrackDocx(reg, media, 0, isThere, &got);
        const std::string k1 = pbtrf::registryTrackDocx(reg, media, 1, isThere);
        pbtrf::registryPut(reg, L"D:\\Films\\film - transcript (track 2).docx", 500, 600, media, pbtrf::transcriptRegKind(), "KEY-T2", 1);
        const std::string k1b = pbtrf::registryTrackDocx(reg, media, 1, isThere);        // (its file is not there)
        there.push_back(L"D:\\Films\\film - transcript (track 2).docx");
        const std::string k1c = pbtrf::registryTrackDocx(reg, media, 1, isThere);
        pbtrf::registryPut(reg, L"D:\\Films\\film.fr.srt", 700, 800, media, "fr", "KEY-FR-EN", 0);
        there.push_back(L"D:\\Films\\film.fr.srt");
        pbtrf::registryPut(reg, L"D:\\Films\\film - transcript (SuVtitle 2).docx", 900, 950, media, pbtrf::transcriptRegKind(), "KEY-FR-ZH", 0);
        const std::string k0b = pbtrf::registryTrackDocx(reg, media, 0, isThere);         // (the newest, but its file is not there)
        pbtrf::registryPut(reg, L"D:\\Films\\film - transcript (SuVtitle 3).docx", 960, 970, media, pbtrf::transcriptRegKind(), "KEY-FR-DE", 0);
        there.push_back(L"D:\\Films\\film - transcript (SuVtitle 3).docx");
        const std::string k0c = pbtrf::registryTrackDocx(reg, media, 0, isThere);         // (two of this track there: the NEWEST)
        ok("[FX] F2: WHICH analysis' transcript the sound track's Word file holds -- the newest transcript entry for this media and "
           "track whose file is there: a keyless one, another track's, a gone file, an .srt never count",
           k0 == "KEY-FR-EN" && pbtrf::lowerW(got) == pbtrf::lowerW(docx) && k1.empty() && k1b.empty() && k1c == "KEY-T2" && k0b == "KEY-FR-EN"
           && k0c == "KEY-FR-DE" && pbtrf::registryTrackDocx(reg, L"D:\\Films\\Other.mkv", 0, isThere).empty());
        const pbtrf::RegEntry* e = pbtrf::registryTranscriptEntry(reg, L"D:\\FILMS\\film - transcript.docx");
        ok("[FX] F2: the transcript entry of ONE file (whose transcript it holds now): its key; an .srt's, a keyless one, none: nothing",
           e && e->key == "KEY-FR-EN" && !pbtrf::registryTranscriptEntry(reg, L"D:\\Films\\film.fr.srt")
           && !pbtrf::registryTranscriptEntry(reg, L"D:\\Films\\film - transcript (SuVtitle).docx")
           && !pbtrf::registryTranscriptEntry(reg, L"D:\\Films\\nothing.docx"));

        // F4: the integrator's MECHANICAL criterion, on REAL files (this smoke's own scratch folder)
        const std::wstring held = fxdir + L"\\held - transcript.docx", ro = fxdir + L"\\read-only - transcript.docx";
        put(held, "OLD");
        put(ro, "OLD");
        pbtrf::WriteFail f1 = pbtrf::WriteFail::None, f2 = f1, f3 = f1, f4 = f1, f5 = f1;
        // Word's way: open for read AND write, sharing READ only
        HANDLE h = ::CreateFileW(held.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool heldNo = h != INVALID_HANDLE_VALUE && !pbtrf::writeFileAvSafe(held, "NEW", true, nullptr, &f1);
        const pbtrf::TargetRefusal hr = pbtrf::targetRefusal(held);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        ok("[FX] F4: a Word file OPEN in Word (held, sharing READ only): the .tmp was written beside it, the file refused -- "
           "WriteFail::Target and targetRefusal Held; the file intact, no .tmp left",
           heldNo && f1 == pbtrf::WriteFail::Target && hr == pbtrf::TargetRefusal::Held && get(held) == "OLD"
           && ::GetFileAttributesW((held + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        ::SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
        const bool roNo = !pbtrf::writeFileAvSafe(ro, "NEW", true, nullptr, &f2);
        const pbtrf::TargetRefusal rr = pbtrf::targetRefusal(ro);
        ::SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
        ok("[FX] F4: a READ-ONLY Word file in a writable folder: Target too, but targetRefusal ReadOnly -- never \"held\"; intact",
           roNo && f2 == pbtrf::WriteFail::Target && rr == pbtrf::TargetRefusal::ReadOnly && get(ro) == "OLD");
        const bool folderNo = !pbtrf::writeFileAvSafe(fxdir + L"\\no such folder\\x - transcript.docx", "NEW", true, nullptr, &f3);
        ok("[FX] F4: a folder that does not take the .tmp: WriteFail::Folder -- the ONLY \"the folder refuses\"",
           folderNo && f3 == pbtrf::WriteFail::Folder);
        const bool appeared = !pbtrf::writeFileAvSafe(held, "NEW", false, nullptr, &f4);
        const std::string afterAppeared = get(held);
        const bool written = pbtrf::writeFileAvSafe(held, "NEW", true, nullptr, &f5);
        ok("[FX] F4: a file there under a write that must not replace: Appeared (intact); free again: written, WriteFail::None; a free "
           "file is no refusal (Other)",
           appeared && f4 == pbtrf::WriteFail::Appeared && afterAppeared == "OLD" && written && f5 == pbtrf::WriteFail::None
           && get(held) == "NEW" && pbtrf::targetRefusal(held) == pbtrf::TargetRefusal::Other);
    }

    rmTree(base);
    ok("the scratch tree is gone", ::GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES);
    std::printf("\napp_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

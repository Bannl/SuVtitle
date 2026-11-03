// =============================================================================
// SuVtitle - app/AudioPBImport.h   (NEW, S-HOST 2026-10-23: the FIRST-RUN IMPORT of AudioPB's player data)
//
// SuVtitle was born on 2026-10-23 out of AudioPB_Translator 1.0.6 (commit 641f1aa): the player, its file analyses and
// its subtitles left AudioPB. The user has already watched films in AudioPB's player -- each analysis is minutes of
// whisper + Qwen per hour of film -- so SuVtitle's first run brings AudioPB's player data over (SPLIT_PLAN S4):
//
//   %LOCALAPPDATA%\AudioPB\Translator files\*.cues        -> %LOCALAPPDATA%\SuVtitle\Files\      the analyses (cache
//   %LOCALAPPDATA%\AudioPB\Translator files\srt-written.txt  ->  (same)                            records + the registry
//                                                                                                  of the .srt files it
//                                                                                                  wrote next to films)
//   %APPDATA%\AudioPB\player.txt, player-resume.txt, player-audio.txt -> %APPDATA%\SuVtitle\     the queue, where each
//                                                                                                  film was left, the
//                                                                                                  chosen sound tracks
//
// THE RULES, each one a promise to the user:
//   * COPY, NEVER MOVE, and nothing is ever written in AudioPB's folders: AudioPB keeps working exactly as before.
//   * NEVER OVERWRITE: a file already in SuVtitle's folders stays (it is newer than anything AudioPB has).
//   * ONCE: a marker file (%APPDATA%\SuVtitle\audiopb-import.txt) says it ran -- and IS the log of what it did, one
//     line per file. It is written even when there was nothing to import, so a later AudioPB never re-triggers it.
//   * NEVER FATAL: every failure is a line in the log, never an exception, never a refusal to start.
//   * Each copy lands under a temporary name first and is RENAMED into place (with the house's antivirus retry,
//     5 x 20 ms -- a rename right after a write fails under Bitdefender on this machine): a copy cut short by a crash
//     never sits under the real name. CopyFileW keeps the source's write time, which keepNewest() orders the cache by.
//   * FIX ROUND 2026-10-23 (review RV2-7): a "<name>.import-tmp" left by such a crash is swept on every start
//     (sweepTmp), whatever became of its source.
//
// The formats are byte-for-byte the same in SuVtitle (TrFileStore.h, TrPlayer.h keep their format tags, the cache key
// is unchanged), so an imported record is a HIT the first time the film is opened. Only the PAIR lives elsewhere: it
// is the engine's (engine.txt, S11), not imported here -- see docs/PROGRESS_S-HOST.md.
//
// Pure enough to be proved: run() takes the four folders, tests/cpp/app_smoke.cpp drives it on scratch trees.
// ASCII only (no /utf-8).
//
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md section 4.3): seedLivePos. The live translator leaves AudioPB for
// SuVtitle's Live window, and the user has PUT AudioPB's Translator window somewhere (its WINPOS line in AudioPB's
// translator.txt -- the other screen, facing the person he talks to). The Live window opens THERE the first time: the
// line is READ (never written: AudioPB's file), with WINPOS's own rules (AppConfig.h parseWinPos). Once only -- not by a
// marker but by construction: main.cpp asks only while app.txt has no LIVEPOS, and the Live window writes its LIVEPOS as
// soon as it exists. Nothing else of the live TUNING is imported (the user's translator.txt holds exactly 1.1.0's
// defaults, measured 25/10); the live PAIR is the engine's to seed (engine.txt, L1a).
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "TrFileStore.h"   // pbtrf::ensureDir, joinPath, fileExists, writeFileAvSafe, toUtf8
#include "AppConfig.h"     // SUVTITLE LIVE: pbapp::WinPos / parseWinPos (seedLivePos)

namespace pbimport {

struct Roots {
    std::wstring fromRoaming;   // %APPDATA%\AudioPB                          (READ ONLY)
    std::wstring fromFiles;     // %LOCALAPPDATA%\AudioPB\Translator files    (READ ONLY)
    std::wstring toRoaming;     // %APPDATA%\SuVtitle
    std::wstring toFiles;       // %LOCALAPPDATA%\SuVtitle\Files
};

struct Result {
    bool ran = false;           // false: the marker was there (already imported) or no destination
    int  copied = 0, kept = 0, failed = 0;
    int  swept = 0;             // stray *.import-tmp removed (RV2-7)
    std::wstring marker;        // the marker / log file
};

inline const wchar_t* markerName() { return L"audiopb-import.txt"; }

// The product's roots, from the environment (a test instance's isolated profile included -- that is what makes the
// import testable end to end without ever reading the user's own data).
inline Roots productRoots() {
    Roots r;
    wchar_t ad[MAX_PATH] = {}, la[MAX_PATH] = {};
    const DWORD na = ::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    const DWORD nl = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (na > 0 && na < MAX_PATH) {
        r.fromRoaming = std::wstring(ad) + L"\\AudioPB";
        r.toRoaming   = std::wstring(ad) + L"\\SuVtitle";
    }
    if (nl > 0 && nl < MAX_PATH) {
        r.fromFiles = std::wstring(la) + L"\\AudioPB\\Translator files";
        r.toFiles   = std::wstring(la) + L"\\SuVtitle\\Files";
    }
    return r;
}

// One file: src -> dst under a temporary name, then renamed (never replacing). "COPIED" / "KEPT" (dst there) /
// "FAILED <why>" / "" (no source: nothing to say).
inline std::string copyOne(const std::wstring& src, const std::wstring& dst) {
    const DWORD a = ::GetFileAttributesW(src.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return std::string();
    if (pbtrf::fileExists(dst)) return "KEPT";
    const std::wstring tmp = dst + L".import-tmp";
    ::DeleteFileW(tmp.c_str());                                  // a leftover of a run that crashed mid-copy
    if (!::CopyFileW(src.c_str(), tmp.c_str(), TRUE)) return "FAILED copy " + std::to_string(::GetLastError());
    for (int t = 0; t < 5; ++t) {
        if (::MoveFileExW(tmp.c_str(), dst.c_str(), 0)) return "COPIED";
        const DWORD e = ::GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) { ::DeleteFileW(tmp.c_str()); return "KEPT"; }
        ::Sleep(20);                                             // the antivirus's scan window after a write
    }
    const DWORD e = ::GetLastError();
    ::DeleteFileW(tmp.c_str());
    return "FAILED rename " + std::to_string(e);
}

// RV2-7 (2026-10-23): every "<name>.import-tmp" in `dir`, removed. copyOne only cleared the one it was about to copy
// AGAIN: a crash mid-copy, then AudioPB evicting that source before the next start, left it there for good (RV-2's
// A2). Our own suffix only (exactly: FindFirstFile's pattern also matches 8.3 aliases), files only, bounded.
inline int sweepTmp(const std::wstring& dir) {
    if (dir.empty()) return 0;
    int n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pbtrf::joinPath(dir, L"*.import-tmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    const std::wstring suffix = L".import-tmp";
    size_t seen = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring leaf = fd.cFileName;
        if (leaf.size() <= suffix.size() || pbtrf::lowerW(leaf.substr(leaf.size() - suffix.size())) != suffix) continue;
        if (::DeleteFileW(pbtrf::joinPath(dir, leaf).c_str())) ++n;
    } while (::FindNextFileW(h, &fd) && ++seen < 10000);
    ::FindClose(h);
    return n;
}

// The import. Idempotent (the marker), never fatal, never writes outside r.toRoaming / r.toFiles.
inline Result run(const Roots& r) {
    Result res;
    try {
        if (r.toRoaming.empty() || r.toFiles.empty()) return res;
        // on EVERY start, before the marker is looked at (two folder listings): a crash's leftovers never stay (RV2-7)
        res.swept = sweepTmp(r.toFiles) + sweepTmp(r.toRoaming);
        res.marker = pbtrf::joinPath(r.toRoaming, markerName());
        if (pbtrf::fileExists(res.marker)) return res;           // already done (or decided there was nothing)
        if (!pbtrf::ensureDir(r.toRoaming)) return res;          // nowhere to write: tried again next run
        res.ran = true;
        std::string log;
        auto note = [&](const std::string& verdict, const std::wstring& name) {
            if (verdict.empty()) return;
            if (verdict == "COPIED") ++res.copied;
            else if (verdict == "KEPT") ++res.kept;
            else ++res.failed;
            const size_t sp = verdict.find(' ');
            log += (sp == std::string::npos ? verdict : verdict.substr(0, sp)) + "\t" + pbtrf::toUtf8(name)
                 + (sp == std::string::npos ? std::string() : "\t" + verdict.substr(sp + 1)) + "\n";
        };
        // the player's own files (roaming)
        if (!r.fromRoaming.empty())
            for (const wchar_t* f : { L"player.txt", L"player-resume.txt", L"player-audio.txt" })
                note(copyOne(pbtrf::joinPath(r.fromRoaming, f), pbtrf::joinPath(r.toRoaming, f)), f);
        // the analyses + the registry (local): the top level only -- proxy\ (temporary sound copies) and *.tmp stay
        const DWORD fa = r.fromFiles.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(r.fromFiles.c_str());
        if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY)) {
            pbtrf::ensureDir(pbtrf::dirOf(r.toFiles));           // %LOCALAPPDATA%\SuVtitle first (ensureDir: one level)
            if (!pbtrf::ensureDir(r.toFiles)) {
                note("FAILED cannot create the cache folder", r.toFiles);
            } else {
                note(copyOne(pbtrf::joinPath(r.fromFiles, L"srt-written.txt"), pbtrf::joinPath(r.toFiles, L"srt-written.txt")),
                     L"srt-written.txt");
                WIN32_FIND_DATAW fd{};
                HANDLE h = ::FindFirstFileW(pbtrf::joinPath(r.fromFiles, L"*.cues").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    size_t seen = 0;
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const std::wstring leaf = fd.cFileName;
                        // FindFirstFile's "*.cues" also matches 8.3 aliases and "x.cues~": the exact extension only
                        if (leaf.size() < 6 || pbtrf::lowerW(leaf.substr(leaf.size() - 5)) != L".cues") continue;
                        note(copyOne(pbtrf::joinPath(r.fromFiles, leaf), pbtrf::joinPath(r.toFiles, leaf)), leaf);
                    } while (::FindNextFileW(h, &fd) && ++seen < 10000);
                    ::FindClose(h);
                }
            }
        }
        // the marker = the log (written even when nothing was there: the decision is made once)
        char stamp[32] = {};
        const std::time_t now = std::time(nullptr);
        std::tm lt{};
        if (::localtime_s(&lt, &now) == 0) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
        const std::string body = "# SuVtitle: AudioPB's player data imported (COPIED, never moved; nothing written in "
                                 "AudioPB's folders) on " + std::string(stamp) + "\n"
                               + "FROM\t" + pbtrf::toUtf8(r.fromRoaming) + "\t" + pbtrf::toUtf8(r.fromFiles) + "\n"
                               + log
                               + "DONE\tcopied " + std::to_string(res.copied) + "\tkept " + std::to_string(res.kept)
                               + "\tfailed " + std::to_string(res.failed) + "\n";
        pbtrf::writeFileAvSafe(res.marker, body);   // (if even this fails, the next run tries again: never overwrites)
    } catch (...) {
        // never fatal: SuVtitle starts with what it has (an import cut short leaves no half file under a real name)
    }
    return res;
}

// SUVTITLE LIVE (1.1.0): where AudioPB's Translator window was (r.fromRoaming\translator.txt, its WINPOS line), READ
// only and bounded (1 MB: a config file, never a reason to read a big one). False = no file, no usable line -- the Live
// window then opens at its default place. Never writes anything, anywhere (the caller writes LIVEPOS in app.txt).
inline bool seedLivePos(const Roots& r, pbapp::WinPos& p) {
    if (r.fromRoaming.empty()) return false;
    std::string text;
    return pbtrf::readFileAll(pbtrf::joinPath(r.fromRoaming, L"translator.txt"), text, 1ULL << 20)
        && pbapp::parseWinPos(text, p);
}

}   // namespace pbimport

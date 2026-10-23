// =============================================================================
// SuVtitle - app/TrAssoc.h   (SuVtitle as a Windows player: the registry half)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TrAssoc.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: ProgIDs NEXTALE.SuVtitle.Video / .Audio, vendor key
// NEXTALE\SuVtitle, application name "SuVtitle", the Settings URI; unregister() deletes the vendor key only once it
// holds nothing else (the installer may keep values of its own there one day).
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// AudioPB AS A WINDOWS PLAYER, the registry half (the user, 11/10: "choisir le lecteur d'AudioPB comme lecteur par
// defaut pour lire les videos dans Windows"). Windows does not let an application MAKE ITSELF the default (since
// Windows 8 the choice is the user's, in Settings > Default apps, protected by a hash). What an application does
// is REGISTER as a candidate, so that it appears in "Open with" and in Settings with its file types:
//
//   * two ProgIDs, NEXTALE.AudioPB.Player.Video / .Audio -- their name, icon and the command Explorer runs:
//       "<exe>" --play "%1"            (TrLaunch.h: the paths are checked, nothing is executed)
//   * OpenWithProgids on each media extension the player handles (TrFileStore.h's 32, video or audio family):
//     AudioPB in "Open with" for every one of them;
//   * Capabilities (ApplicationName "AudioPB Player", ApplicationDescription, ApplicationIcon, FileAssociations)
//     + its line under RegisteredApplications: AudioPB in Settings > Default apps, with its file types.
//
// The product registers MACHINE-wide from its installer (HKLM, AudioPB_Translator_Setup.iss -- the same keys,
// checked against this list by the harness) and PER USER from the player's "Set as default player..." (HKCU, then
// Settings opened on it: ms-settings:defaultapps?registeredAppUser=AudioPB%20Player). `root` is the subkey every
// key lives under: "Software" for the product, "Software\AudioPB-Test\<tag>" for a test (wg_smoke.cpp and the test
// instance: nothing of the user's registry is touched, and the test deletes its root).
//
// entries() is PURE (the list of writes); apply()/unregister() do them. ASCII only (no /utf-8).
// =============================================================================
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "TrFileStore.h"   // pbtrf::mediaExts

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace pbassoc {

const wchar_t* const kAppName  = L"SuVtitle";
const wchar_t* const kAppDesc  = L"Plays videos and audio files, with their subtitles transcribed and translated by SuVtitle.";
const wchar_t* const kProgVideo = L"NEXTALE.SuVtitle.Video";
const wchar_t* const kProgAudio = L"NEXTALE.SuVtitle.Audio";
const wchar_t* const kVendorKey = L"NEXTALE\\SuVtitle";                     // under root
const wchar_t* const kSettingsUri = L"ms-settings:defaultapps?registeredAppUser=SuVtitle";
const wchar_t* const kSettingsFallback = L"ms-settings:defaultapps";

// The audio family (the rest of TrFileStore.h's media list is video).
inline bool isAudioExt(const std::wstring& e) {
    static const wchar_t* const a[] = { L"mp3", L"wav", L"m4a", L"aac", L"flac", L"ogg", L"oga", L"opus", L"mka", L"wma",
                                        L"aif", L"aiff", L"aifc", L"ac3", L"eac3", L"amr", nullptr };
    const std::wstring x = pbtrf::lowerW(e);
    for (const wchar_t* const* p = a; *p; ++p) if (x == *p) return true;
    return false;
}
inline const wchar_t* progIdFor(const std::wstring& ext) { return isAudioExt(ext) ? kProgAudio : kProgVideo; }
// What Explorer runs: the exe, quoted, and the file, quoted. Never anything else.
inline std::wstring openCommand(const std::wstring& exe) { return L"\"" + exe + L"\" --play \"%1\""; }

struct Entry { std::wstring key, name, data; bool none = false; };   // REG_SZ, or REG_NONE (an OpenWithProgids value)

inline std::vector<Entry> entries(const std::wstring& exe, const std::wstring& root) {
    std::vector<Entry> v;
    auto put = [&](const std::wstring& key, const std::wstring& name, const std::wstring& data, bool none = false) {
        v.push_back(Entry{ root + L"\\" + key, name, data, none });
    };
    const std::wstring icon = exe + L",0";
    for (const wchar_t* prog : { kProgVideo, kProgAudio }) {
        const std::wstring c = std::wstring(L"Classes\\") + prog;
        const std::wstring friendly = std::wstring(kAppName) + (prog == kProgAudio ? L" audio" : L" video");
        put(c, L"", friendly);
        put(c, L"FriendlyTypeName", friendly);
        put(c + L"\\DefaultIcon", L"", icon);
        put(c + L"\\shell\\open", L"FriendlyAppName", kAppName);
        put(c + L"\\shell\\open\\command", L"", openCommand(exe));
    }
    for (const wchar_t* const* p = pbtrf::mediaExts(); *p; ++p)
        put(std::wstring(L"Classes\\.") + *p + L"\\OpenWithProgids", progIdFor(*p), L"", true);
    const std::wstring caps = std::wstring(kVendorKey) + L"\\Capabilities";
    put(caps, L"ApplicationName", kAppName);
    put(caps, L"ApplicationDescription", kAppDesc);
    put(caps, L"ApplicationIcon", icon);
    for (const wchar_t* const* p = pbtrf::mediaExts(); *p; ++p)
        put(caps + L"\\FileAssociations", std::wstring(L".") + *p, progIdFor(*p));
    put(L"RegisteredApplications", kAppName, root + L"\\" + caps);
    return v;
}

// Writes the list (KEY_WOW64_64KEY: the same view whatever the build). False + the failing key on an error.
inline bool apply(HKEY hive, const std::vector<Entry>& es, std::wstring& err) {
    for (const auto& e : es) {
        HKEY k = nullptr;
        if (::RegCreateKeyExW(hive, e.key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS) {
            err = e.key;
            return false;
        }
        LONG rc;
        if (e.none) rc = ::RegSetValueExW(k, e.name.c_str(), 0, REG_NONE, nullptr, 0);
        else rc = ::RegSetValueExW(k, e.name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(e.data.c_str()),
                                   static_cast<DWORD>((e.data.size() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(k);
        if (rc != ERROR_SUCCESS) { err = e.key + L" : " + e.name; return false; }
    }
    return true;
}

// Removes what entries() wrote: the two ProgIDs and the vendor key whole, our VALUE in each OpenWithProgids and in
// RegisteredApplications (those keys belong to everyone: only our value goes).
inline void unregister(HKEY hive, const std::wstring& root) {
    const REGSAM sam = KEY_WOW64_64KEY;
    for (const wchar_t* prog : { kProgVideo, kProgAudio }) {
        const std::wstring k = root + L"\\Classes\\" + prog;
        HKEY h = nullptr;
        if (::RegOpenKeyExW(hive, k.c_str(), 0, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | sam, &h) == ERROR_SUCCESS) {
            ::RegDeleteTreeW(h, nullptr);
            ::RegCloseKey(h);
        }
        ::RegDeleteKeyExW(hive, k.c_str(), sam, 0);
    }
    for (const wchar_t* const* p = pbtrf::mediaExts(); *p; ++p) {
        const std::wstring k = root + L"\\Classes\\." + *p + L"\\OpenWithProgids";
        HKEY h = nullptr;
        if (::RegOpenKeyExW(hive, k.c_str(), 0, KEY_SET_VALUE | sam, &h) == ERROR_SUCCESS) {
            ::RegDeleteValueW(h, progIdFor(*p));
            ::RegCloseKey(h);
        }
    }
    {
        // SuVtitle (2026-10-23): AudioPB's vendor key was the player's alone and went whole; SuVtitle's is the app's
        // own name, where an installer may one day keep values of its own -- so only Capabilities goes whole, and
        // the vendor key (then NEXTALE) only once nothing else is left in it.
        const std::wstring k = root + L"\\" + kVendorKey;
        const std::wstring c = k + L"\\Capabilities";
        HKEY h = nullptr;
        if (::RegOpenKeyExW(hive, c.c_str(), 0, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | sam, &h) == ERROR_SUCCESS) {
            ::RegDeleteTreeW(h, nullptr);
            ::RegCloseKey(h);
        }
        ::RegDeleteKeyExW(hive, c.c_str(), sam, 0);
        auto dropIfEmpty = [&](const std::wstring& key) {
            HKEY e = nullptr;
            if (::RegOpenKeyExW(hive, key.c_str(), 0, KEY_QUERY_VALUE | sam, &e) != ERROR_SUCCESS) return;
            DWORD subs = 1, vals = 1;
            const LONG q = ::RegQueryInfoKeyW(e, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &vals, nullptr, nullptr, nullptr, nullptr);
            ::RegCloseKey(e);
            if (q == ERROR_SUCCESS && subs == 0 && vals == 0) ::RegDeleteKeyExW(hive, key.c_str(), sam, 0);
        };
        dropIfEmpty(k);
        dropIfEmpty(root + L"\\NEXTALE");
    }
    {
        const std::wstring k = root + L"\\RegisteredApplications";
        HKEY h = nullptr;
        if (::RegOpenKeyExW(hive, k.c_str(), 0, KEY_SET_VALUE | sam, &h) == ERROR_SUCCESS) {
            ::RegDeleteValueW(h, kAppName);
            ::RegCloseKey(h);
        }
    }
}

// Explorer re-reads its associations.
inline void notifyShell() { ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

}   // namespace pbassoc

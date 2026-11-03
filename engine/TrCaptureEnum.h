// =============================================================================
// SuVtitle - engine/TrCaptureEnum.h   (the picker's list, and the process trees behind "one application")
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b (SUVTITLE_LIVE_PLAN section 4.1). What the Live window's picker offers:
//   * in   every ACTIVE capture endpoint (a microphone, a line, a virtual cable's output, "Stereo Mix"...),
//   * out  every ACTIVE render endpoint (its loopback: "the sound of an output"),
//   * app  every application that has an audio session on an active render endpoint, grouped by its ROOT process
//          (a browser, Teams, Discord play their sound from a CHILD process: the capture targets the root with its
//          whole tree), labelled by its FileDescription ("Zoom Workplace") or else its exe name, keyed by the exe
//          name ("app:zoom.exe", lower case: the key outlives the process and its pid).
// The picker NEVER opens a device (the director's rule: no microphone is opened without the user's gesture):
// endpoints are read from their property store, applications from the render endpoints' session lists.
// SuVtitle's own process tree (the app, this engine, WebView2's children) is never listed: capturing the player's
// own film through "an application" is what the output loopback is for.
// =============================================================================
#pragma once

#include "TrCaptureCore.h"

#include <mmdeviceapi.h>

#include <map>

namespace pbt {
namespace cap {

// The Windows build (RtlGetVersion: the manifest-independent one), 0 if unknown.
uint32_t windowsBuild();
// Process loopback (one application's sound) exists from Windows 10 build 20348 (AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS:
// "Minimum supported client: Windows 10 Build 20348").
bool processLoopbackSupported();

// ---------------------------------------------------------------------------
// A snapshot of the processes, with the parent links and the "root" rule.
// ---------------------------------------------------------------------------
class ProcessTable {
public:
    struct Proc { uint32_t pid = 0, parent = 0; std::wstring exe; };

    ProcessTable();                                           // takes the snapshot
    // TEST: a table made by hand -- each process and its creation time (0 = unreadable, as when OpenProcess is refused;
    // a pid with no entry reads as unreadable too) -- so the smoke proves the root rule on chains it cannot start.
    ProcessTable(const std::vector<Proc>& procs, const std::map<uint32_t, uint64_t>& created);

    const Proc* find(uint32_t pid) const;

    // The process a capture of `pid`'s sound should target (fix round RVA-2): walk up ONLY through parents that run
    // the SAME exe -- one multi-process application: a browser's audio child -> the browser -- and from a WebView2
    // process (msedgewebview2.exe) into the application that hosts it (new Teams' sound -> ms-teams.exe). Any other
    // parent ends the walk: a LAUNCHER (Steam, a browser that started Zoom, a file manager, a shell) is never the
    // application. A parent is followed only when BOTH creation times are readable and the parent is the older (RVA-4:
    // a recycled pid is not a parent, and an unreadable one cannot be told from one -- fail safe: the process is its own
    // root, and an app: key still finds it by its exe name).
    uint32_t rootOf(uint32_t pid) const;
    // Is `pid` `ancestor` or one of its descendants? Through ANY parent (the app's own tree: SuVtitle.exe and its
    // WebView2 children), each link proved the same way.
    bool inTreeOf(uint32_t pid, uint32_t ancestor) const;
    // The roots whose exe is `exeLower` (lower case), oldest first, minus those inside the `hidden` trees.
    std::vector<uint32_t> rootsNamed(const std::wstring& exeLower, const std::vector<uint32_t>& hidden) const;

private:
    uint64_t created(uint32_t pid) const;                     // FILETIME of its creation, 0 if unknown (memoised)
    bool     linked(uint32_t child, uint32_t parent) const;   // RVA-4: both times readable, the parent the older
    std::map<uint32_t, Proc> procs_;
    mutable std::map<uint32_t, uint64_t> created_;
    bool     synthetic_ = false;                              // the test's table: never OpenProcess
};

// A WebView2 runtime process (its lower-case exe name): it plays the sound of the application that hosts it.
bool isWebView2Process(const std::wstring& exeLower);

std::wstring lowerW(const std::wstring& s);
// Windows' own shells and hosts: never a root (a session of theirs is not "an application" to pick).
bool isShellProcess(const std::wstring& exeLower);
// An application's label: its exe's FileDescription, else the exe name.
std::wstring appLabel(uint32_t pid, const std::wstring& exe);

// ---------------------------------------------------------------------------
// The list. `en` belongs to the calling (MTA) thread. hiddenTrees = SuVtitle.exe's pid (HubOptions::appPid, 0 =
// unknown) and this engine's own pid.
// ---------------------------------------------------------------------------
std::vector<SourceInfo> enumerateSources(IMMDeviceEnumerator* en, const std::vector<uint32_t>& hiddenTrees,
                                         std::string* note);

} // namespace cap
} // namespace pbt

// =============================================================================
// SuVtitle - engine/TrMpvAudio.cpp   (1.1.0, wave N3 -- see TrMpvAudio.h)
//
// libmpv loaded at run time, headless; its "pcm" audio output writes 16 kHz mono float into a named pipe we serve.
// Everything below was MEASURED with this libmpv (shinchiro build 20261021, git e471496c56: mpv 0.41 dev, FFmpeg 8 dev)
// before it was written: tests/cpp/mpvaudio/ proves it again on every run, docs PROGRESS_N3.md has the numbers.
// ASCII only (no /utf-8 in this build).
// =============================================================================
#include "TrMpvAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>

namespace pbt {

// ---------------------------------------------------------------------------------------------------------------------
// libmpv's C API: the subset used, declared HERE (mpv/client.h, client API 2.x, ISC licence) so the engine builds with no
// mpv file at all. tests/cpp/mpvaudio/ compiles this file next to the REAL client.h and static_asserts every layout and
// value below against it (as tests/cpp/mpv does for the app's MpvPlayer.cpp): an ABI drift fails the smoke, never a user.
// ---------------------------------------------------------------------------------------------------------------------
namespace mpvabi {

struct mpv_handle;   // opaque

enum mpv_format : int {
    MPV_FORMAT_NONE = 0, MPV_FORMAT_STRING = 1, MPV_FORMAT_OSD_STRING = 2, MPV_FORMAT_FLAG = 3, MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5, MPV_FORMAT_NODE = 6, MPV_FORMAT_NODE_ARRAY = 7, MPV_FORMAT_NODE_MAP = 8, MPV_FORMAT_BYTE_ARRAY = 9
};
enum mpv_event_id : int {
    MPV_EVENT_NONE = 0, MPV_EVENT_SHUTDOWN = 1, MPV_EVENT_LOG_MESSAGE = 2, MPV_EVENT_START_FILE = 6, MPV_EVENT_END_FILE = 7,
    MPV_EVENT_FILE_LOADED = 8, MPV_EVENT_PLAYBACK_RESTART = 21
};
enum mpv_end_file_reason : int {
    MPV_END_FILE_REASON_EOF = 0, MPV_END_FILE_REASON_STOP = 2, MPV_END_FILE_REASON_QUIT = 3,
    MPV_END_FILE_REASON_ERROR = 4, MPV_END_FILE_REASON_REDIRECT = 5
};
enum mpv_error : int {
    MPV_ERROR_PROPERTY_UNAVAILABLE = -10, MPV_ERROR_LOADING_FAILED = -13, MPV_ERROR_AO_INIT_FAILED = -14,
    MPV_ERROR_NOTHING_TO_PLAY = -16, MPV_ERROR_UNKNOWN_FORMAT = -17
};

struct mpv_node_list;
struct mpv_byte_array;
struct mpv_node {
    union {
        char* string;
        int flag;
        int64_t int64;
        double double_;
        mpv_node_list* list;
        mpv_byte_array* ba;
    } u;
    mpv_format format;
};
struct mpv_node_list {
    int num;
    mpv_node* values;
    char** keys;
};
struct mpv_event_log_message {
    const char* prefix;
    const char* level;
    const char* text;
    int log_level;
};
struct mpv_event_end_file {
    mpv_end_file_reason reason;
    int error;
    int64_t playlist_entry_id;
    int64_t playlist_insert_id;
    int playlist_insert_num_entries;
};
struct mpv_event {
    mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void* data;
};

constexpr unsigned long kApiMajor = 2;   // MPV_CLIENT_API_VERSION >> 16 of the headers these declarations mirror

struct Api {
    HMODULE dll = nullptr;
    unsigned long (*client_api_version)() = nullptr;
    const char* (*error_string)(int) = nullptr;
    void (*free_node_contents)(mpv_node*) = nullptr;
    mpv_handle* (*create)() = nullptr;
    int (*initialize)(mpv_handle*) = nullptr;
    void (*terminate_destroy)(mpv_handle*) = nullptr;
    int (*set_option_string)(mpv_handle*, const char*, const char*) = nullptr;
    int (*command_node)(mpv_handle*, mpv_node*, mpv_node*) = nullptr;
    int (*get_property)(mpv_handle*, const char*, mpv_format, void*) = nullptr;
    int (*set_property_async)(mpv_handle*, uint64_t, const char*, mpv_format, void*) = nullptr;
    int (*request_log_messages)(mpv_handle*, const char*) = nullptr;
    mpv_event* (*wait_event)(mpv_handle*, double) = nullptr;
    void (*set_wakeup_callback)(mpv_handle*, void (*)(void*), void*) = nullptr;
};

}   // namespace mpvabi

namespace {

using namespace mpvabi;

constexpr wchar_t kDllName[] = L"libmpv-2.dll";
// The pipe's inbound quota: 4 s of 16 kHz float (mpv's own buffer holds 4 s more). The job reads 4 s at a time.
constexpr DWORD kPipeBytes = 256 * 1024;
// A session's opening (loadfile to the paused restart): a film on a sleeping USB disk or a network share can take seconds;
// past this the file is reported unreadable rather than waited on forever. The caller's `abort` is polled meanwhile.
constexpr ULONGLONG kOpenTimeoutMs = 60000;
// The pipe's connection after the restart (mpv opens it with its audio output, before file-loaded in every measurement).
constexpr ULONGLONG kConnectTimeoutMs = 10000;
// A read that gets neither a byte nor an event for this long: mpv is stuck (a dead network share...), say so.
constexpr ULONGLONG kStallMs = 60000;
constexpr size_t kLogKeep = 8;

std::mutex g_loadMx;
Api g_api;                       // resolved once (the DLL is never unloaded)
bool g_loadTried = false;
std::string g_loadWhy;           // why the load failed ("" = loaded)
std::wstring g_loadedDir;
std::wstring g_dirForTest;       // the smoke's override of the search ("" = the real order)
std::atomic<int> g_sysVulkanForTest{-1};
std::atomic<unsigned> g_pipeSeq{0};

std::string u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool fileExists(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exeDir() {
    std::wstring p(1024, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
        if (n == 0) return std::wstring();
        if (n < p.size()) { p.resize(n); break; }
        p.resize(p.size() * 2);
    }
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = ::GetEnvironmentVariableW(name, buf, 2048);
    return (n > 0 && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

std::wstring trimSlash(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

template <class F>
bool sym(HMODULE m, F& f, const char* name, std::string& missing) {
    f = reinterpret_cast<F>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
    if (!f && missing.empty()) missing = name;
    return f != nullptr;
}

const mpv_node* mapGet(const mpv_node& m, const char* key) {
    if (m.format != MPV_FORMAT_NODE_MAP || !m.u.list) return nullptr;
    const mpv_node_list* l = m.u.list;
    for (int i = 0; i < l->num; ++i)
        if (l->keys && l->keys[i] && std::strcmp(l->keys[i], key) == 0) return &l->values[i];
    return nullptr;
}
int64_t nodeInt(const mpv_node* n, int64_t def) {
    if (!n) return def;
    if (n->format == MPV_FORMAT_INT64) return n->u.int64;
    if (n->format == MPV_FORMAT_DOUBLE) return (int64_t)n->u.double_;
    return def;
}
bool nodeFlag(const mpv_node* n) { return n && n->format == MPV_FORMAT_FLAG && n->u.flag != 0; }
std::string nodeStr(const mpv_node* n) {
    return (n && n->format == MPV_FORMAT_STRING && n->u.string) ? std::string(n->u.string) : std::string();
}

// FFmpeg's codec name -> the plain words a sentence the user reads may carry (the Media Foundation reader's names where
// both know the format: "This sound track (DTS) can't be decoded..." reads the same whichever decoder said it).
std::string codecWords(const std::string& c) {
    auto starts = [&](const char* p) { return c.compare(0, std::strlen(p), p) == 0; };
    if (c == "aac" || c == "aac_latm")                  return "AAC";
    if (c == "ac3")                                     return "Dolby Digital (AC-3)";
    if (c == "eac3")                                    return "Dolby Digital Plus (E-AC-3)";
    if (c == "ac4")                                     return "Dolby AC-4";
    if (c == "dts")                                     return "DTS";
    if (c == "truehd" || c == "mlp")                    return "Dolby TrueHD";
    if (c == "mp3" || c == "mp3float")                  return "MP3";
    if (c == "mp2" || c == "mp1")                       return "MPEG audio";
    if (c == "flac")                                    return "FLAC";
    if (c == "opus")                                    return "Opus";
    if (c == "vorbis")                                  return "Vorbis";
    if (c == "alac")                                    return "ALAC";
    if (c == "tta")                                     return "TTA";
    if (c == "wavpack")                                 return "WavPack";
    if (c == "ape")                                     return "Monkey's Audio";
    if (c == "wmav1" || c == "wmav2")                   return "WMA";
    if (c == "wmapro")                                  return "WMA Pro";
    if (c == "wmalossless")                             return "WMA Lossless";
    if (c == "wmavoice")                                return "WMA Voice";
    if (c == "amr_nb")                                  return "AMR";
    if (c == "amr_wb")                                  return "AMR-WB";
    if (c == "pcm_alaw")                                return "A-law";
    if (c == "pcm_mulaw")                               return "mu-law";
    if (starts("pcm_"))                                 return "PCM";
    if (starts("adpcm_"))                               return "ADPCM";
    if (c == "cook" || starts("ra_") || c == "sipr")    return "RealAudio";
    if (c == "nellymoser")                              return "Nellymoser";
    if (c == "speex")                                   return "Speex";
    std::string s;                                      // anything else: the name itself, printable and short
    for (size_t k = 0; k < c.size() && s.size() < 24; ++k)
        if (c[k] >= 32 && c[k] <= 126) s += (char)std::toupper((unsigned char)c[k]);
    return s.empty() ? std::string("an unknown format") : s;
}

std::string endSentence(int error) {
    switch (error) {
    case MPV_ERROR_LOADING_FAILED: return "The file could not be opened (it may be locked, moved or not readable).";
    case MPV_ERROR_UNKNOWN_FORMAT: return "SuVtitle cannot read this file's format.";
    case MPV_ERROR_AO_INIT_FAILED: return "The sound of this file could not be handed to the analysis (the decoder's output failed).";
    case MPV_ERROR_NOTHING_TO_PLAY: return "This file has no audio track.";
    default: break;
    }
    const char* s = (g_api.error_string && error < 0) ? g_api.error_string(error) : nullptr;
    return std::string("The video engine could not read this file (") + (s ? s : "unknown error") + ").";
}

// Load libmpv once: the Vulkan fallback first (TrMpvAudio.h), then the DLL, then the symbols. Called under g_loadMx.
void loadLocked() {
    if (g_loadTried) return;
    g_loadTried = true;
    const std::wstring dir = mpvAudioDllDir();
    if (dir.empty()) {
        g_loadWhy = "libmpv-2.dll is not installed (looked in the mpv folder next to SuVtitleEngine.exe)";
        return;
    }
    const std::wstring path = dir + L"\\" + kDllName;
    // (review RV-NH 4, the engine's half) THE CURRENT DIRECTORY OFF THE DLL SEARCH PATH, before libmpv is in the process:
    // libmpv LoadLibrary's bare names at run time (nvcuda, d3dcompiler_4x, libEGL, avisynth...), and this engine inherits
    // the app's current directory -- the film's folder when Explorer started SuVtitle on it. Process-wide, from here on:
    // the application folder, System32 and the mpv folder. Every load of this engine names its folder anyway (sherpa:
    // AddDllDirectory + flags, TrSpeaker.cpp; Media Foundation: System32; libmpv and the Vulkan fallback: full paths),
    // and a CHILD process (whisper / llama) is untouched: it keeps the standard order, PATH (and our fallback) included.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ::AddDllDirectory(dir.c_str());
    (void)preloadVulkanFallback(dir);
    // DLL_LOAD_DIR first (what libmpv imports and ships beside it), then System32 & co. -- never the current directory.
    HMODULE m = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        const DWORD e = ::GetLastError();
        g_loadWhy = fmt("libmpv-2.dll could not be loaded (Windows error %lu%s)", e,
                        e == ERROR_MOD_NOT_FOUND ? ": a DLL it needs is missing, vulkan-1.dll for this build" : "");
        return;
    }
    Api a;
    a.dll = m;
    std::string missing;
    bool ok = true;
    ok = sym(m, a.client_api_version, "mpv_client_api_version", missing) && ok;
    ok = sym(m, a.error_string, "mpv_error_string", missing) && ok;
    ok = sym(m, a.free_node_contents, "mpv_free_node_contents", missing) && ok;
    ok = sym(m, a.create, "mpv_create", missing) && ok;
    ok = sym(m, a.initialize, "mpv_initialize", missing) && ok;
    ok = sym(m, a.terminate_destroy, "mpv_terminate_destroy", missing) && ok;
    ok = sym(m, a.set_option_string, "mpv_set_option_string", missing) && ok;
    ok = sym(m, a.command_node, "mpv_command_node", missing) && ok;
    ok = sym(m, a.get_property, "mpv_get_property", missing) && ok;
    ok = sym(m, a.set_property_async, "mpv_set_property_async", missing) && ok;
    ok = sym(m, a.request_log_messages, "mpv_request_log_messages", missing) && ok;
    ok = sym(m, a.wait_event, "mpv_wait_event", missing) && ok;
    ok = sym(m, a.set_wakeup_callback, "mpv_set_wakeup_callback", missing) && ok;
    if (!ok) {
        g_loadWhy = "libmpv-2.dll lacks " + missing;
        ::FreeLibrary(m);                       // (nothing of it ran: safe to let go)
        return;
    }
    const unsigned long ver = a.client_api_version();
    if ((ver >> 16) != kApiMajor) {
        g_loadWhy = fmt("libmpv-2.dll speaks client API %lu.%lu (2.x needed)", ver >> 16, ver & 0xffff);
        ::FreeLibrary(m);
        return;
    }
    g_api = a;
    g_loadedDir = dir;
    g_loadWhy.clear();
}

}   // namespace

// =====================================================================================================================
// libmpv, the Vulkan loader
// =====================================================================================================================
std::wstring mpvAudioDllDir() {
    if (!g_dirForTest.empty()) {
        const std::wstring d = trimSlash(g_dirForTest);
        return fileExists(d + L"\\" + kDllName) ? d : std::wstring();
    }
    const std::wstring exe = exeDir();
    if (!exe.empty()) {
        if (fileExists(exe + L"\\mpv\\" + kDllName)) return exe + L"\\mpv";
        if (fileExists(exe + L"\\" + kDllName)) return exe;
    }
    const std::wstring dev = trimSlash(envVar(L"SUVTITLE_MPV_DIR"));
    if (!dev.empty() && fileExists(dev + L"\\" + kDllName)) return dev;
    return std::wstring();
}

void setMpvAudioDllDirForTest(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(g_loadMx);
    if (!g_loadTried) g_dirForTest = dir;
}

bool mpvAudioAvailable(std::string& why) {
    std::lock_guard<std::mutex> lk(g_loadMx);
    loadLocked();
    why = g_loadWhy;
    return g_api.dll != nullptr;
}

bool systemVulkanLoaderPresent() {
    const int t = g_sysVulkanForTest.load();
    if (t >= 0) return t == 1;
    wchar_t sys[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);       // System32 for this (64-bit) process
    if (n == 0 || n >= MAX_PATH) return false;
    return fileExists(std::wstring(sys, n) + L"\\vulkan-1.dll");
}

void setSystemVulkanLoaderForTest(int present) { g_sysVulkanForTest.store(present < 0 ? -1 : (present ? 1 : 0)); }

std::wstring vulkanFallbackDir(const std::wstring& mpvDir) {
    const std::wstring base = trimSlash(mpvDir.empty() ? mpvAudioDllDir() : mpvDir);
    if (base.empty()) return std::wstring();
    const std::wstring d = base + L"\\" + kVulkanFallbackSubdir;
    return fileExists(d + L"\\vulkan-1.dll") ? d : std::wstring();
}

std::string preloadVulkanFallback(const std::wstring& mpvDir) {
    if (systemVulkanLoaderPresent()) return "the system's Vulkan loader (System32)";
    const std::wstring fb = vulkanFallbackDir(mpvDir);
    if (fb.empty()) return "no Vulkan loader on this PC and none bundled";
    const std::wstring dll = fb + L"\\vulkan-1.dll";
    // By FULL PATH, its own folder then System32 for what it imports. Kept loaded: libmpv's import of "vulkan-1.dll" then
    // binds to this module (a DLL of that name already in the process is used, wherever it came from).
    HMODULE m = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return fmt("the bundled Vulkan loader could not be loaded (Windows error %lu)", ::GetLastError());
    return "the bundled Vulkan loader (" + u8(dll) + ")";
}

std::vector<wchar_t> environmentWithVulkanFallback() {
    const std::wstring fb = vulkanFallbackDir();
    std::vector<wchar_t> block;
    if (fb.empty()) return block;
    LPWCH env = ::GetEnvironmentStringsW();
    if (!env) return block;
    // Copy the block (it is sorted by name, case-insensitively, as CreateProcess wants), the PATH entry extended -- or, a
    // process with no PATH at all, one inserted where the order puts it.
    std::vector<std::wstring> vars;
    for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) vars.emplace_back(p);
    ::FreeEnvironmentStringsW(env);
    bool found = false;
    for (std::wstring& v : vars) {
        if (v.size() > 5 && v[0] != L'=' && ::CompareStringOrdinal(v.c_str(), 5, L"PATH=", 5, TRUE) == CSTR_EQUAL) {
            if (v.back() != L';' && v.size() > 5) v += L';';
            v += fb;
            found = true;
            break;
        }
    }
    if (!found) {
        const std::wstring add = L"Path=" + fb;
        auto at = std::find_if(vars.begin(), vars.end(), [](const std::wstring& v) {
            return v[0] != L'=' && ::CompareStringOrdinal(v.c_str(), -1, L"Path=", -1, TRUE) == CSTR_GREATER_THAN;
        });
        vars.insert(at, add);
    }
    for (const std::wstring& v : vars) block.insert(block.end(), v.c_str(), v.c_str() + v.size() + 1);
    block.push_back(L'\0');
    return block;
}

// =====================================================================================================================
// The session
// =====================================================================================================================
struct MpvPcm::Impl {
    Facts f;
    mpv_handle* h = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE ioEvent = nullptr;          // the pipe's overlapped I/O (connect, then reads)
    HANDLE wake = nullptr;             // set by mpv's wakeup callback (any of its threads)
    OVERLAPPED ov{};
    bool ioPending = false, connected = false;
    bool srcEof = false;               // the writer closed the pipe: everything was delivered
    bool mpvEnded = false;             // mpv said end-file (the pipe still holds the tail)
    std::string endErr;                // an end-file ERROR in mid-stream: owed to the next read (the data before it stands)
    std::vector<unsigned char> buf;    // the overlapped read's target
    unsigned char carry[4] = {};       // a float split across two reads (never seen: kept exact anyway)
    size_t carryN = 0;
    std::deque<std::string> log;       // mpv's last warnings / errors

    static void wakeup(void* ctx) { ::SetEvent(static_cast<HANDLE>(ctx)); }

    ~Impl() { teardown(); }

    // Pipe FIRST (a write mpv is blocked in fails at once), then mpv.
    void teardown() {
        if (pipe != INVALID_HANDLE_VALUE) {
            if (ioPending) {
                ::CancelIoEx(pipe, &ov);
                DWORD n = 0;
                ::GetOverlappedResult(pipe, &ov, &n, TRUE);   // the cancelled I/O must finish before `ov` / `buf` go
                ioPending = false;
            }
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (h) {
            g_api.set_wakeup_callback(h, nullptr, nullptr);
            g_api.terminate_destroy(h);
            h = nullptr;
        }
        if (ioEvent) { ::CloseHandle(ioEvent); ioEvent = nullptr; }
        if (wake) { ::CloseHandle(wake); wake = nullptr; }
        connected = false;
        carryN = 0;
    }

    void keepLog(const mpv_event_log_message* m) {
        if (!m || !m->text) return;
        std::string line = std::string(m->prefix ? m->prefix : "") + ": " + m->text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        log.push_back(line);
        while (log.size() > kLogKeep) log.pop_front();
    }

    bool getDouble(const char* name, double& v) {
        return g_api.get_property(h, name, MPV_FORMAT_DOUBLE, &v) >= 0 && std::isfinite(v);
    }

    // One mpv event of a RUNNING session (never a blocking call into mpv: wait_event only reads our queue).
    void onRunningEvent(const mpv_event* ev) {
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            keepLog(static_cast<const mpv_event_log_message*>(ev->data));
        } else if (ev->event_id == MPV_EVENT_END_FILE) {
            const auto* ef = static_cast<const mpv_event_end_file*>(ev->data);
            mpvEnded = true;
            // "nothing to play" = nothing MORE to play (the sound ended before the start asked: measured) -- an end
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR && ef->error != MPV_ERROR_NOTHING_TO_PLAY)
                endErr = "The file is damaged or truncated here: the video engine stopped decoding its sound ("
                       + std::string(g_api.error_string(ef->error)) + ").";
        } else if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            mpvEnded = true;
        }
    }

    // mpv closed its writer: everything is in. Its end-file says WHY (measured: the pipe breaks a moment BEFORE the event
    // is queued), so it is waited for -- up to 1 s, on our own event queue: the writer is gone, mpv cannot be blocked on it.
    void writerClosed() {
        srcEof = true;
        carryN = 0;                    // (a partial float cannot be a sample: mpv writes whole ones -- never seen, but eof()
                                       //  waits for carryN == 0, and a reader must never wait for ever)
        const ULONGLONG t0 = ::GetTickCount64();
        while (!mpvEnded && ::GetTickCount64() - t0 < 1000) {
            mpv_event* ev = g_api.wait_event(h, 0.05);
            if (ev && ev->event_id != MPV_EVENT_NONE) onRunningEvent(ev);
        }
    }

    // file-loaded: the tracks, the duration, the picture. `ord` asked (-1 = mpv's choice). false = refused (`err`).
    // `wantAid` > 0 = the session must be re-opened on that id (mpv's numbering is not ord + 1: never measured, guarded).
    bool onLoaded(int ord, std::string& err, int64_t& wantAid) {
        wantAid = -1;
        double dur = 0.0;
        f.durationSec = getDouble("duration", dur) && dur > 0.0 ? dur : 0.0;
        mpv_node tl{};
        if (g_api.get_property(h, "track-list", MPV_FORMAT_NODE, &tl) < 0) {
            err = "The video engine could not list this file's tracks.";
            return false;
        }
        std::vector<std::pair<int64_t, const mpv_node*>> aud;   // (id, track) of the sound tracks
        int64_t selected = -1;
        if (tl.format == MPV_FORMAT_NODE_ARRAY && tl.u.list) {
            for (int i = 0; i < tl.u.list->num; ++i) {
                const mpv_node& t = tl.u.list->values[i];
                const std::string type = nodeStr(mapGet(t, "type"));
                if (nodeFlag(mapGet(t, "external"))) continue;
                if (type == "video") {
                    if (!nodeFlag(mapGet(t, "albumart")) && !nodeFlag(mapGet(t, "image"))) f.hasVideo = true;
                } else if (type == "audio") {
                    const int64_t id = nodeInt(mapGet(t, "id"), -1);
                    if (id < 0) continue;
                    aud.emplace_back(id, &t);
                    if (nodeFlag(mapGet(t, "selected"))) selected = id;
                }
            }
        }
        // mpv numbers each kind of track in the demuxer's order, which is the container's: the ord-th sound track is
        // the ord-th id in increasing order -- the player's very rule (MpvPlayer.cpp onTracks).
        std::sort(aud.begin(), aud.end(), [](const std::pair<int64_t, const mpv_node*>& a,
                                             const std::pair<int64_t, const mpv_node*>& b) { return a.first < b.first; });
        f.audioTracks = (int)aud.size();
        bool ok = true;
        if (aud.empty()) {
            err = "This file has no audio track.";
            ok = false;
        } else if (ord >= 0 && (size_t)ord >= aud.size()) {
            // WH: never another track instead (mpv itself, given an aid the file lacks, plays its default one: measured)
            err = fmt("The chosen sound track (number %d) is not in this file.", ord + 1);
            ok = false;
        } else {
            int pick = ord;
            if (ord < 0) {
                f.defaultPick = true;
                for (size_t k = 0; k < aud.size(); ++k)
                    if (aud[k].first == selected) pick = (int)k;
            }
            if (pick < 0) {
                err = "None of this file's sound tracks can be played.";
                ok = false;
            } else {
                const mpv_node& t = *aud[(size_t)pick].second;
                f.ord = pick;
                f.aid = aud[(size_t)pick].first;
                const int64_t src = nodeInt(mapGet(t, "src-id"), 0);
                f.srcId = src > 0 && src <= 0xFFFFFFFFLL ? (uint32_t)src : 0;
                const int64_t sr = nodeInt(mapGet(t, "demux-samplerate"), 0);
                const int64_t ch = nodeInt(mapGet(t, "demux-channel-count"), 0);
                f.rate = sr > 0 ? (uint32_t)sr : 0;
                f.channels = ch > 0 ? (uint32_t)ch : 0;
                f.codec = codecWords(nodeStr(mapGet(t, "codec")));
                f.lang = nodeStr(mapGet(t, "lang"));
                f.title = nodeStr(mapGet(t, "title"));
                if (ord >= 0 && f.aid != selected) wantAid = f.aid;   // mpv selected another one for aid=ord+1
            }
        }
        g_api.free_node_contents(&tl);
        return ok;
    }

    // The pipe's client connected? (polls the overlapped ConnectNamedPipe). Checks it is THIS process -- mpv runs in it.
    bool checkConnected(std::string& err) {
        if (connected) return true;
        DWORD n = 0;
        if (!::GetOverlappedResult(pipe, &ov, &n, FALSE)) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return true;            // not yet: fine
            err = fmt("The sound pipe of the video engine failed (Windows error %lu).", e);
            return false;
        }
        ioPending = false;
        ULONG pid = 0;
        if (!::GetNamedPipeClientProcessId(pipe, &pid) || pid != ::GetCurrentProcessId()) {
            err = "The sound pipe of the video engine was opened by another program: refused.";
            return false;
        }
        connected = true;
        return true;
    }
};

MpvPcm::MpvPcm() : d_(new Impl) {}
MpvPcm::~MpvPcm() = default;
void MpvPcm::close() { d_.reset(new Impl); }
const MpvPcm::Facts& MpvPcm::facts() const { return d_->f; }
bool MpvPcm::eof() const { return d_->srcEof && d_->carryN == 0 && d_->endErr.empty(); }

std::string MpvPcm::lastLog() const {
    std::string s;
    for (const std::string& l : d_->log) s += (s.empty() ? "" : " | ") + l;
    return s;
}

MpvPcm::Fail MpvPcm::lastFail() const { return fail_; }

bool MpvPcm::open(const std::wstring& path, double startSec, int ord, std::string& err,
                  const std::function<bool()>& abort) {
    err.clear();
    fail_ = Fail::None;
    // every way out of here that is not a success says what kind of failure it was (the reader's fallback rule)
    auto failed = [this, &err](Fail k, const std::string& sentence) {
        err = sentence;
        fail_ = k;
        close();
        return false;
    };
    int64_t aid = -1;                                    // (the guarded re-open: mpv's numbering is not ord + 1)
    for (int attempt = 0; attempt < 2; ++attempt) {
        close();
        Impl& d = *d_;
        std::string why;
        if (!mpvAudioAvailable(why)) return failed(Fail::NoLibrary, why);
        if (!std::isfinite(startSec) || startSec < 0.0) startSec = 0.0;
        d.f.startSec = startSec;
        const ULONGLONG t0 = ::GetTickCount64();

        // ---- the pipe: first instance (no squatter), inbound, local clients only, overlapped --------------------------
        wchar_t name[128];
        std::swprintf(name, 128, L"\\\\.\\pipe\\suvtitle.pcm.%lu.%u.%llu", ::GetCurrentProcessId(), ++g_pipeSeq,
                      (unsigned long long)::GetTickCount64());
        d.pipe = ::CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
                                    kPipeBytes, 0, nullptr);
        d.ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        d.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (d.pipe == INVALID_HANDLE_VALUE || !d.ioEvent || !d.wake)
            return failed(Fail::File, fmt("The sound pipe of the video engine could not be created (Windows error %lu).",
                                          ::GetLastError()));
        d.ov = OVERLAPPED{};
        d.ov.hEvent = d.ioEvent;
        if (::ConnectNamedPipe(d.pipe, &d.ov)) {
            d.ioPending = true;                           // (overlapped: completes through the event, checked below)
        } else {
            const DWORD e = ::GetLastError();
            if (e == ERROR_IO_PENDING) {
                d.ioPending = true;
            } else if (e == ERROR_PIPE_CONNECTED) {       // a client came before we listened: it must be us
                ULONG pid = 0;
                if (!::GetNamedPipeClientProcessId(d.pipe, &pid) || pid != ::GetCurrentProcessId())
                    return failed(Fail::File, "The sound pipe of the video engine was opened by another program: refused.");
                d.connected = true;
            } else {
                return failed(Fail::File, fmt("The sound pipe of the video engine failed (Windows error %lu).", e));
            }
        }

        // ---- mpv, headless and PAUSED ----------------------------------------------------------------------------------
        d.h = g_api.create();
        if (!d.h) return failed(Fail::File, "The video engine could not be created.");
        auto opt = [&d](const char* k, const char* v) { (void)g_api.set_option_string(d.h, k, v); };
        // nothing of a user's mpv (no mpv.conf, no scripts, no profiles, no resume), no picture, no subtitle, no window
        opt("config", "no");
        opt("load-scripts", "no");
        opt("ytdl", "no");
        // (review RV-NH 2) A LOCAL file must never make mpv fetch anything: a playlist renamed .mkv, an HLS manifest
        // renamed .mp4, an mpv EDL naming a URL -- measured (tests/cpp/mpvaudio [NET]): without this, a connection.
        opt("access-references", "no");
        opt("load-unsafe-playlists", "no");
        opt("osc", "no");
        opt("load-stats-overlay", "no");
        opt("load-console", "no");
        opt("load-auto-profiles", "no");
        opt("load-select", "no");
        opt("load-commands", "no");
        opt("load-positioning", "no");
        opt("load-context-menu", "no");
        opt("input-default-bindings", "no");
        opt("input-builtin-bindings", "no");
        opt("input-media-keys", "no");
        opt("media-controls", "no");                      // never in Windows' media overlay (the build has win32-smtc)
        opt("resume-playback", "no");
        opt("save-position-on-quit", "no");
        opt("directory-mode", "ignore");
        opt("audio-file-auto", "no");
        opt("sub-auto", "no");
        opt("cover-art-auto", "no");
        opt("vid", "no");
        opt("sid", "no");
        opt("audio-display", "no");
        // THE SOUND: mpv's raw PCM writer into our pipe, 16 kHz mono float (mpv's own resampler and downmix)
        const std::string pipeName = u8(name);
        opt("ao", "pcm");
        opt("ao-pcm-file", pipeName.c_str());
        opt("ao-pcm-waveheader", "no");
        opt("ao-pcm-append", "no");
        opt("audio-format", "float");
        opt("audio-samplerate", "16000");
        opt("audio-channels", "mono");
        opt("audio-pitch-correction", "no");
        // /!\ THE FORMAT IS PINNED BEFORE THE OUTPUT (02/11, found auditing the 27/10 build): mpv RE-CREATES its audio output
        // whenever the DECODED format changes -- a broadcast .ts switching stereo <-> 5.1, a file stitched from parts, 48 <->
        // 44.1 kHz -- whatever the forced output above; the new output re-opens the pipe, our single instance refuses it, and
        // the session dies ("audio output initialization failed": 0 s of a 12 s TS whose first part is stereo and second 5.1,
        // measured; mpv's log: "[af] [in] 48000Hz 5.1" then "Failed to open \\.\pipe\..."). This filter makes the chain's
        // output 16 kHz mono float WHATEVER comes in, so the output never changes and is never re-created (measured: one open,
        // the whole 12.1 s). The resampling and the downmix are FFmpeg's swresample either way.
        // THE TIMELINE OVER A GAP (02/11). mpv writes the sound CONTINUOUSLY while its clock follows the timestamps: across a
        // gap in them (a broadcast capture that lost its signal) time-pos jumps, and without the filler below every cue after
        // the gap would sit the gap's length too early (measured: 6.056 instead of 8.0 s after a 1.97 s gap). Media Foundation
        // filled such gaps with silence (TrMedia.cpp kGapTolSec / kMaxGapSec); so does this chain now -- mpv's lavfi bridge
        // hands the graph the frames' timestamps (measured):
        //  * asetpts makes them SAFE first. E = where the frame would start were the sound continuous (the previous output
        //    stamp + the previous frame's length, kept in the expression's register 0). A frame stamped more than 0.1 s BEFORE
        //    E (a timestamp reset) or more than 30 s after it (a clock reset) is restamped E: a continuation, Media Foundation's
        //    rule -- without it aresample DROPS the sound after a reset (measured: 5.0 s of a 10.1 s TS, the second half gone).
        //    The first frame of a graph keeps its stamp: the session's first sample is where the player has it (firstPts).
        //  * aresample async=1 then fills what is left -- a forward gap of 0.1 to 30 s -- with silence (hard compensation
        //    only: no stretching). Measured: 1.97 s gap in a TS and in its MKV remux -> the burst after it at 8.0001 / 8.0004 s
        //    (its place); a 20 s gap -> 26.0001; a 55 s jump -> counted on (6.056); a reset -> nothing dropped (10.069 s).
        //  * aformat pins the output (above).
        static const char kAudioChain[] =
            "lavfi=["
            "asetpts='st(1\\,if(eq(N\\,0)\\,PTS\\,if(lt(PTS\\,PREV_OUTPTS+ld(0)/SR/TB-0.1/TB)"
            "+gt(PTS\\,PREV_OUTPTS+ld(0)/SR/TB+30/TB)\\,PREV_OUTPTS+ld(0)/SR/TB\\,PTS)));st(0\\,NB_SAMPLES);ld(1)',"
            "aresample=16000:async=1,"
            "aformat=sample_fmts=flt:channel_layouts=mono]";
        opt("af", kAudioChain);
        // THE TIMELINE: an EXACT start, paused -- the first playback-restart then says where the first sample lies
        opt("hr-seek", "yes");
        opt("pause", "yes");
        opt("idle", "yes");
        opt("keep-open", "no");
        opt("gapless-audio", "no");
        g_api.set_wakeup_callback(d.h, &Impl::wakeup, d.wake);
        g_api.request_log_messages(d.h, "warn");
        const int ie = g_api.initialize(d.h);
        if (ie < 0) return failed(Fail::File, std::string("The video engine could not start (") + g_api.error_string(ie) + ").");
        // loadfile with NAMED arguments (the positional order changed in mpv 0.38), start + track per file
        std::string opts = fmt("start=%.6f,aid=", startSec);
        if (aid > 0) opts += std::to_string((long long)aid);
        else if (ord >= 0) opts += std::to_string(ord + 1);
        else opts += "auto";
        const std::string url = u8(path);
        {
            const char* keys[4] = {"name", "url", "flags", "options"};
            const std::string vals[4] = {"loadfile", url, "replace", opts};
            mpv_node values[4];
            for (int i = 0; i < 4; ++i) {
                values[i].format = MPV_FORMAT_STRING;
                values[i].u.string = const_cast<char*>(vals[i].c_str());
            }
            mpv_node_list list{4, values, const_cast<char**>(keys)};
            mpv_node root;
            root.format = MPV_FORMAT_NODE_MAP;
            root.u.list = &list;
            const int e = g_api.command_node(d.h, &root, nullptr);   // PAUSED: nothing is written, a sync call is safe
            if (e < 0)
                return failed(Fail::File, std::string("The video engine refused this file (") + g_api.error_string(e) + ").");
        }

        // ---- PHASE A: loaded, then the PAUSED restart that says where the first sample lies ------------------------------
        // The synchronous calls (track-list, duration, time-pos, audio-pts) are made only while mpv is paused: it writes
        // nothing then (measured), so the pipe cannot be full. A restart with NO audio position means the start lies past
        // the end of the sound (measured: a start in a video's silent tail, or past the end): mpv is let go at once, and
        // ends the file "nothing played" -- a session at its end, not an error. Until then only wait_event(0) is called.
        bool loaded = false, restarted = false, retry = false, running = false;
        auto unpause = [&d]() {
            int no = 0;
            return g_api.set_property_async(d.h, 0, "pause", MPV_FORMAT_FLAG, &no) >= 0;
        };
        for (;;) {
            if (abort && abort()) return failed(Fail::Aborted, std::string());
            if (!d.checkConnected(err)) return failed(Fail::File, err);
            if (d.f.endedAtOnce) break;
            if (restarted && d.connected) break;
            if (::GetTickCount64() - t0 > (restarted ? kOpenTimeoutMs + kConnectTimeoutMs : kOpenTimeoutMs))
                return failed(Fail::File, restarted ? "The video engine did not hand this file's sound over in time."
                                                    : "The video engine did not open this file in time.");
            const HANDLE hs[2] = {d.wake, d.ioEvent};
            ::WaitForMultipleObjects(d.ioPending ? 2 : 1, hs, FALSE, 50);
            for (;;) {
                mpv_event* ev = g_api.wait_event(d.h, 0);
                if (!ev || ev->event_id == MPV_EVENT_NONE) break;
                if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
                    d.keepLog(static_cast<const mpv_event_log_message*>(ev->data));
                } else if (ev->event_id == MPV_EVENT_FILE_LOADED && !loaded) {
                    loaded = true;
                    int64_t want = -1;
                    if (!d.onLoaded(ord, err, want)) return failed(Fail::Track, err);
                    if (want > 0 && attempt == 0) { aid = want; retry = true; break; }
                } else if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART && loaded && !restarted) {
                    restarted = true;
                    double pts = 0.0;
                    if (!d.getDouble("time-pos", pts)) pts = startSec;
                    d.f.firstPts = pts;
                    double apts = 0.0;
                    if (!d.getDouble("audio-pts", apts)) {          // no sound here: let mpv say how it ends
                        if (!unpause()) return failed(Fail::File, "The video engine would not start decoding this file.");
                        running = true;
                    }
                } else if (ev->event_id == MPV_EVENT_END_FILE) {
                    const auto* ef = static_cast<const mpv_event_end_file*>(ev->data);
                    const int code = ef ? ef->error : 0;
                    if ((ef && ef->reason == MPV_END_FILE_REASON_EOF) || (restarted && code == MPV_ERROR_NOTHING_TO_PLAY)) {
                        d.f.endedAtOnce = true;                // nothing to deliver from the start asked
                        d.srcEof = true;
                        break;
                    }
                    // a file whose sound track exists but would not decode ends "nothing to play" BEFORE any restart;
                    // one with no sound track at all ends so before even file-loaded: both are final (no fallback)
                    if (code == MPV_ERROR_NOTHING_TO_PLAY && loaded && d.f.audioTracks > 0)
                        return failed(Fail::Track, d.f.audioTracks >= 2
                                          ? "This sound track (" + d.f.codec + ") can't be decoded for the analysis. Choose another sound track."
                                          : "This file's audio (" + d.f.codec + ") cannot be decoded.");
                    if (code == MPV_ERROR_NOTHING_TO_PLAY) return failed(Fail::Track, endSentence(code));
                    return failed(Fail::File, endSentence(code));
                } else if (ev->event_id == MPV_EVENT_SHUTDOWN) {
                    return failed(Fail::File, "The video engine stopped while opening this file.");
                }
            }
            if (retry) break;
        }
        if (retry) continue;                                // re-open on mpv's own id for that track (guarded case)
        d.f.openMs = ::GetTickCount64() - t0;
        if (d.f.endedAtOnce) return true;                   // eof() at once; the caller says "end"

        // ---- PHASE B: let it run. From here on: NO blocking mpv call (the header's one rule) -----------------------------
        if (!running && !unpause()) return failed(Fail::File, "The video engine would not start decoding this file.");
        d.buf.resize(64 * 1024);
        return true;
    }
    return failed(Fail::Track, "The video engine could not select this file's sound track.");
}

bool MpvPcm::read(std::vector<float>& out, size_t maxFrames, std::string& err) {
    Impl& d = *d_;
    if (!d.h) { err = "internal: a read from a decode session that is not open"; return false; }
    if (d.srcEof || maxFrames == 0) {
        if (!d.endErr.empty()) { err = d.endErr; d.endErr.clear(); return false; }
        return true;
    }
    size_t got = 0;
    ULONGLONG lastProgress = ::GetTickCount64();
    auto consume = [&](const unsigned char* p, size_t n) {
        // bytes -> floats; a float split across two reads waits in `carry`
        if (d.carryN) {
            while (d.carryN < 4 && n) { d.carry[d.carryN++] = *p++; --n; }
            if (d.carryN == 4) {
                float v;
                std::memcpy(&v, d.carry, 4);
                out.push_back(v);
                ++got;
                d.carryN = 0;
            }
        }
        const size_t whole = n / 4;
        if (whole) {
            const size_t at = out.size();
            out.resize(at + whole);
            std::memcpy(&out[at], p, whole * 4);
            got += whole;
        }
        for (size_t k = whole * 4; k < n; ++k) d.carry[d.carryN++] = p[k];
        lastProgress = ::GetTickCount64();
    };
    while (got < maxFrames && !d.srcEof) {
        // mpv's news, never waiting for it
        for (;;) {
            mpv_event* ev = g_api.wait_event(d.h, 0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;
            lastProgress = ::GetTickCount64();
            d.onRunningEvent(ev);
        }
        if (!d.ioPending) {
            if (!d.connected) {                              // (open() leaves it connected; a guard, never seen)
                if (!d.checkConnected(err)) return false;
                if (!d.connected) { err = "The sound pipe of the video engine is not connected."; return false; }
            }
            const size_t wantBytes = std::min(d.buf.size(), (maxFrames - got) * 4 - d.carryN);
            ::ResetEvent(d.ioEvent);
            d.ov = OVERLAPPED{};
            d.ov.hEvent = d.ioEvent;
            DWORD n = 0;
            if (::ReadFile(d.pipe, d.buf.data(), (DWORD)std::max<size_t>(wantBytes, 1), &n, &d.ov)) {
                consume(d.buf.data(), n);
                continue;
            }
            const DWORD e = ::GetLastError();
            if (e == ERROR_BROKEN_PIPE) { d.writerClosed(); break; }     // mpv closed its writer: everything is here
            if (e != ERROR_IO_PENDING) {
                err = fmt("The video engine's sound could not be read (Windows error %lu).", e);
                return false;
            }
            d.ioPending = true;
        }
        const HANDLE hs[2] = {d.ioEvent, d.wake};
        const DWORD w = ::WaitForMultipleObjects(2, hs, FALSE, 1000);
        if (w == WAIT_OBJECT_0) {
            DWORD n = 0;
            d.ioPending = false;
            if (!::GetOverlappedResult(d.pipe, &d.ov, &n, FALSE)) {
                const DWORD e = ::GetLastError();
                if (e == ERROR_BROKEN_PIPE) { d.writerClosed(); break; }
                err = fmt("The video engine's sound could not be read (Windows error %lu).", e);
                return false;
            }
            consume(d.buf.data(), n);
        } else if (w == WAIT_TIMEOUT) {
            // mpv said end-file and its writer stays open (never seen: it closes within ~2 ms): the end, after 3 s
            if (d.mpvEnded && ::GetTickCount64() - lastProgress > 3000) { d.srcEof = true; d.carryN = 0; break; }
            if (::GetTickCount64() - lastProgress > kStallMs) {
                err = "The video engine stopped delivering this file's sound.";
                return false;
            }
        }
    }
    if (d.srcEof && got == 0 && !d.endErr.empty()) { err = d.endErr; d.endErr.clear(); return false; }
    return true;
}

}   // namespace pbt

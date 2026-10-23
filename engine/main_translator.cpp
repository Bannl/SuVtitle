// =============================================================================
// SuVtitle - engine/main_translator.cpp   ->   SuVtitleEngine.exe
//
// SUVTITLE SPLIT (2026-10-23): ported from AudioPB's PatchBus repo, translator/main_translator.cpp at commit
// 641f1aa (APB_TRANSLATOR "v1.0.6"). AudioPBTranslator.exe was the Translator card as a process: a fabric
// consumer, a live speech pipeline AND the FILE MODE that subtitles a recording. SuVtitle took the file half
// (SPLIT_PLAN S5): this is AudioPB's helper MINUS ITS LIVE PATH -- no fabric consumer (TrAudio, and the
// driver's shm / ring_buffer / async_src are not linked), no LINE / PART / END streaming, no diarization
// (TrSpeaker, sherpa), no live preset machinery, no live selftest. Kept whole: the two child servers and their
// lifecycle (GPU Vulkan, the CPU fallback, --cpu), FILE MODE v1 / v2 / WF / WH, FPROXY, FFOCUS, the language
// machinery (the pair, zh / zh-tw via Windows ELS), DTW, and every file selftest with its offline mode.
// Internal names stay AudioPB's on purpose (S3: pbt::, trpipe::, TrConfig...); what the user or Windows sees
// is SuVtitle's. Each removal leaves a "(SUVTITLE SPLIT: ...)" note where the code was, saying what and why;
// docs\PROGRESS_S-ENG.md has the list.
//
// Three things live in here:
//
//   * two CHILD SERVERS on 127.0.0.1 (whisper.cpp's ASR, llama.cpp's Qwen) that we
//     spawn, watch, restart and kill -- never install, never expose (TrEngine.h);
//   * the FILE JOB, a state machine run one bounded step per lap on the pipeline thread
//     (TrFileJob.h decides windows and cues, TrMedia.h reads the media), and the FPROXY worker;
//   * a duplex pipe servant that streams the job's messages to SuVtitle.exe (TrPipe.h).
//
// Modes:
//   SuVtitleEngine.exe --pipe <name> [--parent pid] [--preset quality|balanced|light|auto]
//                      [--mode auto|a2b|b2a] [--cpu] [--nofabric]
//   SuVtitleEngine.exe --selftest [offline]             the engine's whole proof (see runSelftest)
//   SuVtitleEngine.exe --selftest-filemode | -v2 | -wh [offline], --selftest-wf, --selftest-file <media>
//
// Exits when: QUIT arrives, the pipe breaks, or the parent dies (an app crash must
// not leave an orphan holding two child servers and their models).
// =============================================================================
#include <windows.h>
#include <shellapi.h>
#include <crtdbg.h>        // WH: --selftest-wh routes CRT asserts to stderr (quietCrashes)
#include <psapi.h>         // fix round RV3-2: [0s] bounds the bounded reads' memory (GetProcessMemoryInfo)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "EngineVersion.h"   // SUVTITLE_ENGINE_VERSION_STR -- one number, shared with engine.rc
#include "TrEngine.h"
#include "TrFileJob.h"
#include "TrMedia.h"
#include "TrPipe.h"          // ..\common\ (the app <-> engine contract; director-owned)
#include "TrText.h"
// (SUVTITLE SPLIT: TrAudio.h -- the fabric consumer -- and TrSpeaker.h -- diarization -- stayed in AudioPB.)

namespace {

using namespace pbt;

// Deadlines. The ASR on a CPU-only machine with the quality preset is legitimately
// slow (the bench measured 3.6 s on a GPU for 16 s of audio; CPU is several times
// that), so the leash is long. Past it, the server is wedged and killing it is right.
// (SUVTITLE SPLIT: the live final's kAsrTimeoutMs, the live pair's kRespawnBackoff and the `auto` probe's
// kProbeTimeoutMs went with the live path; a FILE window has its own kFileAsrTimeoutMs, below.)
constexpr DWORD kMtTimeoutMs    = 60000;
constexpr DWORD kReadyMs        = 180000;   // model load: large-v3-turbo is 1.6 GB off disk

// THE SELFTEST'S STAND-IN MODEL (WB). "" = every translation comes from the MT server -- the
// only value the helper ever runs with. The selftest sets it to a reply in the WRONG Chinese
// script to prove, deterministically, that the script nets (toTraditional / toSimplified) sit
// between the model and the screen on the live finals and on the file job alike: the real
// models rarely misbehave on cue, so without a stand-in the nets are only ever proved by luck.
std::string g_mtReplyForTest;

// TEST-ONLY: EVERY CHILD SERVER ON THE CPU, the GPU not even opened (WD, user 11/10: "utilise le cpu
// pour les tests, j'utilise le gpu"). `--cpu` or AUDIOPB_TR_FORCE_CPU=1. The shipping default stays
// GPU-first. /!\ The CPU FLAGS ALONE ARE NOT ENOUGH, measured (WMI GPUProcessMemory, RX 7700 XT):
// whisper-server -ng and llama-server -dev none still create a Vulkan device, 6 MB of VRAM each.
// GGML_DISABLE_VULKAN=1 keeps whisper.cpp's ggml from registering the backend at all, and
// VK_LOADER_DRIVERS_DISABLE=* makes the Vulkan loader expose no driver to anyone (llama.cpp loads
// ggml-vulkan.dll on its own and has no such switch): 0 MB, no GPU row at all, for both. Set in
// OUR environment, so every child inherits them.
bool g_forceCpu = false;
void forceCpuChildren() {
    g_forceCpu = true;
    ::SetEnvironmentVariableW(L"GGML_DISABLE_VULKAN", L"1");
    ::SetEnvironmentVariableW(L"VK_LOADER_DRIVERS_DISABLE", L"*");
}
// REVIEW P4: may the TEST environment (AUDIOPB_TR_FORCE_CPU, AUDIOPB_TR_TEST_FILE_ASR / _MT,
// AUDIOPB_TR_STANDIN_MARKS) act on this run? Only when its COMMAND LINE makes it a test: a selftest,
// --nofabric (a pipe test's helper), --cpu. The tray passes none of them.
inline bool testEnvAllowed(bool selftestAny, bool nofabric, bool cpuFlag) { return selftestAny || nofabric || cpuFlag; }

// Where a whisper child may start (review P3): the GPU then the CPU (the live children), or one of them.
constexpr int kAsrGpuThenCpu = 0, kAsrGpuOnly = 1, kAsrCpuOnly = 2;
// Review P3: does the FILE whisper keep DTW when it falls back to the CPU? DTW needs flash attention OFF,
// and what that costs on the CPU decides it. MEASURED (11/10, turbo -ng -t 4, the same 6 film windows,
// 168 s of audio): flash attention 226 s (0.74x real time), DTW 281 s (0.60x) = +24 % -- less than half of
// its price on the GPU (+55 %, accepted), for the same timing (a CPU job is slower than real time either
// way: it is analysed ahead, and then the timing is what the viewer gets). Kept.
constexpr bool kCpuDtw = true;

// WAVE WF: the FILE whisper's DTW preset -- whisper-server's `-dtw` name for the alignment heads of
// THIS model file ("ggml-large-v3-turbo.bin" -> large.v3.turbo; a quantised copy, -q5_0 / -q8_0 /
// -f16, has the same heads). "" = no preset known (a fine-tune, a renamed file): the job then runs
// on whisper's token times, as before WF -- wrong heads would be worse than none.
std::string dtwPresetFor(const std::string& modelFile) {
    std::string n;
    for (char c : modelFile) n += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    const size_t slash = n.find_last_of("\\/");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bin") == 0) n.resize(n.size() - 4);
    if (n.compare(0, 5, "ggml-") == 0) n = n.substr(5);
    for (const char* q : { "-q4_0", "-q4_1", "-q5_0", "-q5_1", "-q8_0", "-f16", "-f32" }) {
        const size_t l = std::strlen(q);
        if (n.size() > l && n.compare(n.size() - l, l, q) == 0) { n.resize(n.size() - l); break; }
    }
    static const char* kMap[][2] = {
        { "large-v3-turbo", "large.v3.turbo" }, { "large-v3", "large.v3" }, { "large-v2", "large.v2" },
        { "large-v1", "large.v1" }, { "medium", "medium" }, { "medium.en", "medium.en" },
        { "small", "small" }, { "small.en", "small.en" }, { "base", "base" }, { "base.en", "base.en" },
        { "tiny", "tiny" }, { "tiny.en", "tiny.en" },
    };
    for (const auto& m : kMap) if (n == m[0]) return m[1];
    return std::string();
}
// TEST-ONLY (WF): the FILE whisper WITHOUT DTW (the pre-WF token times) -- the selftest's A/B and the
// mutation that proves the DTW path is the one the cues come from. Never set by the helper itself.
bool g_fileNoDtwForTest = false;
// TEST-ONLY (WF review P3): the NEXT window's whisper call fails once as a transport error would (a DTW
// server dying mid-job) -- the job must go on WITHOUT DTW. Never set by the helper itself.
bool g_fileAsrFailOnceForTest = false;

// (SUVTITLE SPLIT: AudioPB's live-line constants and helpers were here -- kPartialTimeoutMs, kPopDeadlineMs,
// kPartialBackoffMs, kPartialMtDeltaBytes, kOutboxCap, waitMsFrom, textDistance -- then the W3 "commit as you
// go" engine: CommitCfg, kCommitMinSec, CommitPick, commitPick. They cut a LIVE stream into lines; a FILE job
// cuts its windows and cues in TrFileJob.*, which came along whole.)

std::wstring utf8ToWideL(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Everything printed by the selftest goes through ONE narrow, UTF-8 stream: mixing
// wprintf and printf orients the stream and silently swallows half the output --
// and half of what we print here is Chinese, which is exactly the half that matters.
std::string wideToUtf8L(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// A tray crash must not orphan us: its process handle going signaled IS our exit.
// (The child servers are in a KILL_ON_JOB_CLOSE job, so they go down with us.)
void watchParent(uint32_t pid) {
    if (!pid) return;
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return;
    ::CreateThread(nullptr, 0,
        [](LPVOID param) -> DWORD {
            ::WaitForSingleObject((HANDLE)param, INFINITE);
            ::ExitProcess(3);
        },
        h, 0, nullptr);
}

// ---------------------------------------------------------------------------
// THE ENGINE'S LOG (SUVTITLE, S4 -- new: AudioPB's helper only spoke to the debugger). Every line the
// helper sent to OutputDebugString still goes there, and -- in host mode -- is also appended, time-stamped,
// to %LOCALAPPDATA%\SuVtitle\engine.log, so a job that failed on the user's machine can be read back
// without a debugger attached. The folder comes from the ENVIRONMENT (like TrConfig's APPDATA): a test run
// points LOCALAPPDATA elsewhere and the log follows. NO FILE for a selftest or a --nofabric test engine
// (g_logPath stays empty): a test writes nothing in the user's folders. Past 1 MB at start, the old log
// becomes engine.log.1 (one generation: this is a diagnostic, not a history).
// ---------------------------------------------------------------------------
std::wstring g_logPath;                     // "" = no log file; set once, before any thread exists
std::mutex   g_logMx;
constexpr uint64_t kLogMaxBytes = 1u << 20;

void engineLog(const std::string& text) {
    ::OutputDebugStringA(text.c_str());
    if (g_logPath.empty()) return;
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u ", (unsigned)t.wYear,
                  (unsigned)t.wMonth, (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute,
                  (unsigned)t.wSecond, (unsigned)t.wMilliseconds);
    std::string line = stamp + text;
    if (line.back() != '\n') line += '\n';
    std::lock_guard<std::mutex> lk(g_logMx);
    // Opened per line, appended, shared: a few lines per job, and a second engine (a test instance next to
    // the user's) must never find the file locked.
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, line.data(), (DWORD)line.size(), &wr, nullptr);
    ::CloseHandle(h);
}

// Host mode only (runHost), before the pipeline thread exists.
void openEngineLog() {
    wchar_t la[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring dir = std::wstring(la) + L"\\SuVtitle";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\engine.log";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
        && ((((uint64_t)fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > kLogMaxBytes) {
        // the house's rename retry (Bitdefender's scan window, PB.md) -- cheap, and a failure only means
        // one more megabyte in the same file
        const std::wstring old = path + L".1";
        for (int t = 0; t < 5; ++t) {
            if (::MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) break;
            ::Sleep(20);
        }
    }
    g_logPath = path;
}

// ---------------------------------------------------------------------------
// The two child servers, kept up.
//
// GPU vs CPU is DECIDED HERE, by trying: spawn with the Vulkan flags, and if the
// server never answers /health, kill it and respawn with the explicit CPU flags.
// Both fallbacks were verified against these exact binaries (llama-server accepts
// "-dev none" and comes up on CPU; whisper-server has -ng). A machine with no
// Vulkan runtime therefore still works -- slowly, and honestly labelled.
// ---------------------------------------------------------------------------
// SUVTITLE SPLIT: AudioPB's Servers ran TWO kinds of children -- the LIVE pair (a draft and a final whisper,
// the MT, brought up by preset: ensureUp / bring, the `auto` probe, draftPort / finalPort / up / gpu /
// asrModel / resolvedPreset) and the FILE job's own whisper beside the same MT. Only the second came along:
// bringFile / endFile and what they need, verbatim, minus the line that killed the live pair before a job
// (there is none to make room for). The MT override (setMtOverride) went too: it only steered the live
// pair's MT; a FILE job has always named its own (makeFileJob: MTMODEL, else the 4B).
// "TWO ASR CHILDREN, NEVER TWO ASR CALLS" (AudioPB's rule here) holds a fortiori: one ASR child, called
// from the pipeline thread only.
class Servers {
public:
    uint16_t mtPort()     const { return mt_.port(); }
    std::string mtModel()  const { return mtFile_; }
    bool     mtGpu()      const { return mtGpu_; }       // SUVTITLE: STATE's gpu = fileGpu() && mtGpu()
    const std::string& lastError() const { return err_; }

    void setPaths(const TrPaths& p) { paths_ = p; }
    bool payloadFound() const { return paths_.ok(); }    // SUVTITLE: the idle STATE says it
    // ... and in which words: the sentence naming the missing part (fix round RV3-3, resolvePathsAt), else the
    // generic one (a TrPaths made by hand).
    std::string payloadSentence() const { return paths_.missing.empty() ? std::string(kPayloadMissing) : paths_.missing; }
    // SUVTITLE: the CONFIGURED preset (engine.txt PRESET, CTL) -- what AudioPB's ensureUp(preset) recorded as
    // `asked_`. It only matters to presetFinalModel below: no live pair is brought up for it.
    void setPreset(const std::string& p) { asked_ = p; }

    void killAll() {
        mt_.kill(); fileAsr_.kill();
        fileLoaded_.clear();
    }

    // ---- FILE MODE (wave WB) ----------------------------------------------------------
    // A FILE job runs on ITS OWN whisper child (the job's model: turbo, or large-v3 on `max`)
    // and the 4B MT. (AudioPB: the live children were killed first -- VRAM -- and came back after the
    // job through the pipeline's ensureUp; SuVtitle has none.) The MT is kept when it already is the
    // file the job wants (no reload), and after the job endFile() frees only the job's whisper -- so
    // the next job, often the next film of the queue, skips the MT's load. `abort` (FCANCEL / a
    // superseding FILE / QUIT) stops a model load within 200 ms.
    bool bringFile(const std::string& asrFile, const std::string& mtFile, bool verbose,
                   const std::function<bool()>& abort, bool allowDtw = true) {
        if (!paths_.ok()) {
            err_ = payloadSentence();
            return false;
        }
        if (mt_.alive() && mtFile != mtWanted_) mt_.kill();
        mtWanted_ = mtFile;
        if (!mt_.alive() && !startMt(mtFile, verbose, abort)) return false;
        if (fileAsr_.alive() && (fileLoaded_ != asrFile || (fileDtw_ && !allowDtw))) { fileAsr_.kill(); fileLoaded_.clear(); }
        if (!fileAsr_.alive()) {
            // WAVE WF: the FILE whisper times its words by DTW (TrEngine.h, AsrWord::dtw) -- the
            // model's own alignment heads, which this server only computes with flash attention
            // OFF (it disables DTW otherwise, and says so). Measured on the film (11/10): token
            // times off by up to 15.7 s over music -> 1.9 s at worst, 0.06 s on median; the price
            // is flash attention: +55 % ASR time (turbo, 25 film windows: 30.8 s -> 47.8 s; DTW
            // itself costs nothing). A model whose alignment heads this server has no preset for
            // runs without (the old token times), and so does a server binary that refuses the
            // flags. The LIVE children never got it -- their latency was the product.
            // Review P3, THE ORDER: GPU with DTW -> GPU without (flash attention off needs more VRAM:
            // a card that holds the models with it on but not off must stay on the GPU, ~10x faster
            // than any CPU) -> CPU with DTW only when kCpuDtw says it is cheap there -> CPU without.
            // A job whose DTW server failed mid-way asks for none (allowDtw, fileJobLap).
            const std::string preset = (g_fileNoDtwForTest || !allowDtw) ? std::string() : dtwPresetFor(asrFile);
            const std::wstring dtw = preset.empty() ? std::wstring()
                                   : L" -dtw " + utf8ToWideL(preset) + L" -nfa";
            auto aborted = [&] { return abort && abort(); };
            bool up = false;
            if (!dtw.empty()) {
                fileDtw_ = true;
                up = startAsr(fileAsr_, asrFile, fileGpu_, "asr/file", verbose, abort, dtw, kAsrGpuOnly);
                if (!up && aborted()) return false;
            }
            if (!up) {
                fileDtw_ = false;
                up = startAsr(fileAsr_, asrFile, fileGpu_, "asr/file", verbose, abort, std::wstring(), kAsrGpuOnly);
                if (!up && aborted()) return false;
            }
            if (!up && !dtw.empty() && kCpuDtw) {
                fileDtw_ = true;
                up = startAsr(fileAsr_, asrFile, fileGpu_, "asr/file", verbose, abort, dtw, kAsrCpuOnly);
                if (!up && aborted()) return false;
            }
            if (!up) {
                fileDtw_ = false;
                if (!startAsr(fileAsr_, asrFile, fileGpu_, "asr/file", verbose, abort, std::wstring(), kAsrCpuOnly))
                    return false;
            }
            fileLoaded_ = asrFile;
        }
        err_.clear();
        return true;
    }
    void endFile() { fileAsr_.kill(); fileLoaded_.clear(); }
    bool     fileUp()   const { return fileAsr_.alive() && mt_.alive(); }
    uint16_t filePort() const { return fileAsr_.port(); }
    bool     fileGpu()  const { return fileGpu_; }
    bool     fileDtw()  const { return fileDtw_; }
    bool     modelInstalled(const std::string& file) const { return modelPresent(file); }
    // The preset's FINAL model, capped by what is installed -- the file job's fallback when turbo is not
    // there. SUVTITLE: the CONFIGURED preset (no live pair runs, no probe resolves `auto`): `auto` (or none)
    // = the best installed, an explicit one = the best installed AT OR BELOW it. That is exactly what
    // AudioPB answered whenever its live pair had not come up as that preset, and the same model as its
    // "running preset" whenever that preset's models are installed.
    std::string presetFinalModel() const {
        return asrModelsFor(installedAtOrBelow(asked_.empty() || asked_ == "auto" ? "quality" : asked_)).final;
    }

    // The user-visible sentence of a missing payload (an FDONE error, the idle STATE's err).
    static constexpr const char* kPayloadMissing =
        "the SuVtitle engine's payload was not found (engine\\whisper, engine\\llama, models next to SuVtitleEngine.exe, "
        "or set SUVTITLE_PAYLOAD to a folder holding whisper, llama, models)";

private:
    bool modelPresent(const std::string& file) const {
        if (paths_.modelsDir.empty()) return false;
        return ::GetFileAttributesW((paths_.modelsDir + L"\\" + utf8ToWideL(file)).c_str())
               != INVALID_FILE_ATTRIBUTES;
    }
    // The best preset AT OR BELOW `p` whose two models are actually on disk. (In AudioPB only `auto`
    // used it for the live pair: an explicit choice there had to fail loudly with "ASR model missing".)
    std::string installedAtOrBelow(const std::string& p) const {
        static const char* kOrder[] = { "quality", "balanced", "light" };   // best first
        bool below = false;
        for (const char* q : kOrder) {
            if (!below && std::string(q) != p) continue;                    // skip past `p`
            below = true;
            const AsrModels m = asrModelsFor(q);
            if (modelPresent(m.draft) && modelPresent(m.final)) return q;
        }
        return p;   // nothing installed at all: let the load produce the honest error
    }

    // `extra` (WF): flags for this child only -- the FILE whisper's DTW (bringFile); "" = none.
    // `where` (review P3): kAsrGpuThenCpu, or the GPU / the CPU alone (bringFile orders its own tries).
    bool startAsr(ChildServer& child, const std::string& modelFile, bool& gpuFlag,
                  const char* tag, bool verbose, const std::function<bool()>& abort = {},
                  const std::wstring& extra = std::wstring(), int where = kAsrGpuThenCpu) {
        const std::wstring model = paths_.modelsDir + L"\\" + utf8ToWideL(modelFile);
        if (::GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err_ = "ASR model missing: " + modelFile;
            return false;
        }
        const uint16_t port = findFreePort();
        if (!port) { err_ = "no free loopback port for the speech recogniser"; return false; }
        const std::wstring dir = dirOf(paths_.whisperExe);
        if (verbose && !extra.empty()) std::printf("  [%s] with%s\n", tag, wideToUtf8L(extra).c_str());

        std::wstring common = L"--host 127.0.0.1 --port " + std::to_wstring(port)
                            + L" -m \"" + model + L"\" -l auto -t 4" + extra;
        if (!g_forceCpu && where != kAsrCpuOnly) {   // TEST-ONLY switch (or DEVICE cpu): straight to the CPU
            if (verbose) std::printf("  [%s] %s on :%u (GPU) ...\n", tag, modelFile.c_str(), (unsigned)port);
            if (child.spawn(paths_.whisperExe, dir, common, port, kReadyMs, abort)) { gpuFlag = true; return true; }
            if (abort && abort()) { err_ = "cancelled"; return false; }   // not a GPU failure
        }
        if (where == kAsrGpuOnly) {
            err_ = "the speech recogniser would not start on the GPU";
            return false;
        }

        if (verbose) std::printf("  [%s] %s on CPU (-ng) ...\n", tag, modelFile.c_str());
        const uint16_t port2 = findFreePort();
        if (!port2) { err_ = "no free loopback port for the speech recogniser"; return false; }
        common = L"--host 127.0.0.1 --port " + std::to_wstring(port2)
               + L" -m \"" + model + L"\" -l auto -t 4 -ng" + extra;
        if (child.spawn(paths_.whisperExe, dir, common, port2, kReadyMs, abort)) { gpuFlag = false; return true; }
        if (abort && abort()) { err_ = "cancelled"; return false; }

        err_ = "the speech recogniser would not start (GPU or CPU)";
        return false;
    }

    // The MT model file, resolved: a bare name lives in the models folder, anything with a
    // separator is taken as a path. A DEFAULT that is not installed falls back to the small
    // model SILENTLY -- it goes to the log, not to STATE: `err` paints the engine red and
    // means "the translator is broken", and a bundle that ships one model instead of two is
    // not broken, it is smaller.
    bool resolveMt(const std::string& file, std::wstring& path) const {
        const std::wstring w = utf8ToWideL(file);
        path = (file.find('\\') != std::string::npos || file.find('/') != std::string::npos
                || file.find(':') != std::string::npos)
             ? w : (paths_.modelsDir + L"\\" + w);
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool startMt(const std::string& wantFile, bool verbose,
                 const std::function<bool()>& abort = {}) {
        std::string file = wantFile;
        std::wstring model;
        if (!resolveMt(file, model)) {
            const std::string alt = kMtModelFileSmall;
            std::wstring altPath;
            if (file != alt && resolveMt(alt, altPath)) {
                const std::string note = "[SuVtitleEngine] " + file + " not in the models "
                                         "folder; falling back to " + alt + "\n";
                engineLog(note);
                if (verbose) std::printf("  [mt ] %s missing -> %s\n", file.c_str(), alt.c_str());
                file = alt;
                model = altPath;
            } else {
                err_ = "translation model missing: " + wantFile;
                return false;
            }
        }
        mtFile_ = file;                 // what STATE reports: the file actually loaded
        const uint16_t port = findFreePort();
        if (!port) { err_ = "no free loopback port for the translator model"; return false; }
        const std::wstring dir = dirOf(paths_.llamaExe);

        // -ngl 99 asks for full offload. On a Vulkan-less machine llama.cpp registers no
        // GPU device and simply runs on the CPU back-end -- but "simply" is a claim, and
        // this file does not ship claims: if it does not answer /health we respawn with
        // "-dev none", which forces the CPU path explicitly (verified working).
        std::wstring args = L"--host 127.0.0.1 --port " + std::to_wstring(port)
                          + L" -m \"" + model + L"\" -ngl 99 -c 4096 -t 4 --no-webui";
        if (!g_forceCpu) {           // TEST-ONLY switch (or DEVICE cpu): straight to the CPU (g_forceCpu)
            if (verbose) std::printf("  [mt ] %s on :%u (GPU) ...\n", file.c_str(), (unsigned)port);
            if (mt_.spawn(paths_.llamaExe, dir, args, port, kReadyMs, abort)) { mtGpu_ = true; return true; }
            if (abort && abort()) { err_ = "cancelled"; return false; }
        }

        if (verbose) std::printf("  [mt ] %s on CPU (-dev none) ...\n", file.c_str());
        const uint16_t port2 = findFreePort();
        if (!port2) { err_ = "no free loopback port for the translator model"; return false; }
        args = L"--host 127.0.0.1 --port " + std::to_wstring(port2)
             + L" -m \"" + model + L"\" -dev none -c 4096 -t 4 --no-webui";
        if (mt_.spawn(paths_.llamaExe, dir, args, port2, kReadyMs, abort)) { mtGpu_ = false; return true; }
        if (abort && abort()) { err_ = "cancelled"; return false; }

        err_ = "the translation model would not start (GPU or CPU)";
        return false;
    }

    static std::wstring dirOf(const std::wstring& p) {
        const size_t cut = p.find_last_of(L"\\/");
        return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
    }

    TrPaths      paths_;
    ChildServer  mt_;
    ChildServer  fileAsr_;                     // WB: the FILE job's whisper
    std::string  fileLoaded_;
    bool         fileGpu_ = true;
    bool         fileDtw_ = false;             // WF: the FILE whisper runs with -dtw (bringFile)
    std::string  mtWanted_;
    std::string  mtFile_ = kMtModelFileSmall;  // the file the MT child actually has open
    std::string  asked_;                       // the configured preset (setPreset)
    std::string  err_;
    bool         mtGpu_ = true;
};

// (SUVTITLE SPLIT: StageTimes -- the LIVE final's per-stage timings -- was here. hopPeakDbfs below was the
// live finals' onset gate; it stays, unused by the FILE path, because [0quater] (reviewFixTests, part of
// --selftest-filemode) asserts its any-grid measure -- parity of that selftest. kVadHopMs is TrAudio.h's live
// VAD hop, the same 30 ms as the file job's kFileHop.)
constexpr int kVadHopMs = 30;

// The loudest 30 ms hop of `mono16k`, in dBFS of hop RMS -- the SAME measure TrAudio's
// Segment::peakDbfs uses, computed over exactly the audio about to be transcribed.
// WHY (WB, after the WA review): a segment's peakDbfs is the loudest hop of the WHOLE utterance,
// and since W3 the audio a closing final transcribes can be only its TAIL -- a breath after the
// last committed sentence, carrying the peak of the sentences before it. The hallucination
// filter's quiet gate (shortlist + prompt echo, < -32 dBFS) could therefore never fire on a
// tail: and now that finals ARE prompted, a quiet tail is exactly where whisper hands the
// prompt -- i.e. the sentence just shown -- back.
// ON ANY GRID (WB, after the review): the 30 ms window is taken at every sample offset, so the
// loudest hop is found wherever the VAD's own grid fell -- a segment carries a pre-roll that is not a whole
// number of hops (3680 samples = 7 hops + 320), and a fixed grid from sample 0 straddled the very
// hop that opened the gate, measuring an utterance that opened at -37.x below -38.
double hopPeakDbfs(const std::vector<float>& mono16k) {
    const size_t hop = (size_t)(kAsrRate * kVadHopMs / 1000);
    if (mono16k.size() < hop) return -120.0;
    // Every 30 ms window, at EVERY sample offset (a running sum): exactly the loudest hop any grid
    // could have measured.
    double sum = 0.0, bestSum = -1.0;
    for (size_t i = 0; i < mono16k.size(); ++i) {
        const double v = mono16k[i];
        sum += v * v;
        if (i >= hop) { const double o = mono16k[i - hop]; sum -= o * o; }
        if (sum < 0.0) sum = 0.0;                                  // float drift
        if (i + 1 >= hop && sum > bestSum) bestSum = sum;
    }
    const double rms = std::sqrt(bestSum / (double)hop);
    const double db  = rms > 1.0e-9 ? 20.0 * std::log10(rms) : -180.0;
    return std::max(-120.0, db);
}

// ---------------------------------------------------------------------------
// WHERE A LINE GOES, and how much the models are told. One value, passed by hand into the
// two stage functions, so the selftest drives the shipping code with a different pair
// instead of a copy of it.
// ---------------------------------------------------------------------------
struct Routing {
    std::string mode      = "auto";   // auto | a2b | b2a -- a DIRECTION over the pair
    std::string langA     = "en";
    std::string langB     = "zh";
    std::string mtStyle   = "qwen";
    bool        asrPrompt = true;
    int         mtContext = 2;
};

// What the pipeline remembers BETWEEN utterances, and its one ownership rule: the pipeline
// thread writes it, FINALS only. A draft may read all of it and must never add to it -- a
// draft is a guess, and feeding the next call's prompt with our own hesitation is how a
// mis-heard word becomes the context that makes the next one mis-heard too.
struct TrContext {
    std::string        lastFinalLang;              // for the auto-mode prompt's language guess
    std::vector<std::pair<std::string, std::string>> lastFinalSrc;   // lang -> last final text
    std::deque<MtPair> pairs;                      // the last MTCONTEXT final pairs, oldest first

    const std::string* srcFor(const std::string& lang) const {
        for (const auto& p : lastFinalSrc) if (p.first == lang) return &p.second;
        return nullptr;
    }
    void rememberSrc(const std::string& lang, const std::string& text) {
        for (auto& p : lastFinalSrc) if (p.first == lang) { p.second = text; return; }
        lastFinalSrc.emplace_back(lang, text);
    }
};

// WHICH CHINESE a Han-script line is, given the pair (wave WA). Whisper cannot answer this --
// it calls Simplified and Traditional Mandarin the same "chinese" -- so the PAIR answers it:
//   * zh-tw in the pair and zh not  -> zh-tw (a Taiwanese session: every Han line is the
//     guest's, WHATEVER script whisper wrote it in -- the prompt asks for Traditional and
//     toTraditional makes sure of it, see finaliseAudio);
//   * zh in the pair and zh-tw not  -> zh, exactly as before this wave;
//   * BOTH (a zh <-> zh-tw session, i.e. a script conversion) -> the script decides, by vote
//     (hanScriptVotes); a line with no discriminating character ties, and a tie is the A half,
//     the same default the rest of the router falls back to;
//   * neither -> zh: a THIRD language, which is what it has always been.
std::string hanSourceLang(const std::string& text, const Routing& r) {
    const bool zh = (r.langA == "zh"    || r.langB == "zh");
    const bool tw = (r.langA == "zh-tw" || r.langB == "zh-tw");
    if (tw && !zh) return "zh-tw";
    if (zh && !tw) return "zh";
    if (!zh)       return "zh";
    int trad = 0, simp = 0;
    hanScriptVotes(text, trad, simp);
    if (trad > simp) return "zh-tw";
    if (simp > trad) return "zh";
    return r.langA;
}

// The SOURCE language of a transcript, as a code, given the pair we route between.
// whisper's label is the primary signal and the script is the corroboration: Han characters
// on screen are a fact where the label is an opinion, which is why the script test overrules
// a label whenever a Chinese (zh or zh-tw) is one of the two languages -- and why it is NOT
// consulted otherwise (kana and Hangul land in cjkRatio too, and a Japanese line must keep
// saying "ja").
// /!\ "zh" IS NOT "Chinese" ANY MORE, it is one of two. Every Han verdict below goes through
// hanSourceLang, because the shortcut this function used to take (`return "zh"`) is exactly
// how a Taiwanese session would end up translated as a mainland one.
std::string sourceLangOf(const AsrResult& a, const Routing& r) {
    const bool hanInPair = isHanLang(r.langA) || isHanLang(r.langB);
    if (hanInPair && a.chinese) return hanSourceLang(a.text, r);
    std::string L = a.language;
    if (L == "yue") L = "zh";                      // Cantonese: routed as Chinese
    // The converse of the corroboration: a "chinese" label on a transcript with not one Han
    // character in it is simply wrong, and then the only candidate left is the other half.
    if (L == "zh" && !a.chinese) L.clear();
    // (A "zh" that survives to here is a Han line on a pair with no Chinese in it -- the
    // third-language case, and hanSourceLang would say "zh" too.)
    if (L.empty()) L = (isHanLang(r.langA) && !isHanLang(r.langB)) ? r.langB : r.langA;
    return L;
}

// The target, and whether this line is a passthrough. A LOCK names the target and a segment
// already IN that target is passed through verbatim rather than round-tripped through the
// model (which would "translate" English into slightly different English and read as a bug).
struct Route { std::string dstLang; bool pass = false; };
Route routeFor(const Routing& r, const std::string& srcLang) {
    Route out;
    if (r.mode == "a2b")      { out.dstLang = r.langB; out.pass = (srcLang == r.langB); }
    else if (r.mode == "b2a") { out.dstLang = r.langA; out.pass = (srcLang == r.langA); }
    else if (srcLang == r.langA) out.dstLang = r.langB;
    else if (srcLang == r.langB) out.dstLang = r.langA;
    else                         out.dstLang = r.langB;   // a third language: into B, the
    if (out.pass) out.dstLang = srcLang;                  // "other" half of the session
    return out;
}

// The language we EXPECT to hear next, which is what the ASR prompt must be written in.
// Under a lock it is not a guess at all; in auto it is the language of the last finished
// line, because a conversation does not change language between two breaths nearly as often
// as whisper changes its mind about one.
std::string expectedLang(const Routing& r, const TrContext& c) {
    if (r.mode == "a2b") return r.langA;
    if (r.mode == "b2a") return r.langB;
    return c.lastFinalLang.empty() ? r.langA : c.lastFinalLang;
}

// The initial prompt: a domain glossary line in the expected language, plus the tail of the
// last thing actually said in it. ~200 bytes total -- whisper only keeps its last 224
// tokens of prompt anyway, and every byte of it is encoder work on `small`.
std::string asrPromptFor(const Routing& r, const TrContext& c, const std::string& lang) {
    if (!r.asrPrompt) return std::string();
    std::string p = asrGlossaryFor(lang);
    if (const std::string* last = c.srcFor(lang)) {
        const std::string tail = promptTail(*last, 150);
        if (!tail.empty()) { if (!p.empty()) p += " "; p += tail; }
    }
    return p;
}

// The prior pairs to replay, SAME DIRECTION ONLY and oldest first. A pair going the other
// way would teach the model to answer in the language it is being asked to translate from.
std::vector<MtPair> contextFor(const TrContext& c, const std::string& srcLang,
                               const std::string& dstLang, int want) {
    std::vector<MtPair> out;
    if (want <= 0) return out;
    for (const auto& p : c.pairs)
        if (p.srcLang == srcLang && p.dstLang == dstLang) out.push_back(p);
    if ((int)out.size() > want) out.erase(out.begin(), out.end() - want);
    return out;
}

// (SUVTITLE SPLIT: the LIVE stages were here -- finaliseAudio, processSegment, processCommitted, the drafts
// (PartialState, draftTranscribe, processPartial) and the commit engine's step (AgreeState, snapStale,
// trimCommitted, CommitOut, commitStep). They turned fabric audio into LINE / PART / END, and SuVtitle has no
// fabric. The FILE job never called them: it has its own window step (fileWindowStep) and cue translation
// (translateOneCue) below, on the same Routing / TrContext / script nets as above.)

// ---------------------------------------------------------------------------
// Shared state between the pipeline thread and the pipe servant.
// ---------------------------------------------------------------------------
// (SUVTITLE SPLIT: `Out` -- one LINE / PART / END for the servant, the live path's ONE ordered queue -- and
// `SeqState` -- the per-utterance line counter -- were here. A FILE job's messages have their own
// never-dropped queue, Shared::fileOut.)

// One FILE request as the servant queued it. `refuse` (WD) = the FILE named a job id but could not
// be read (a `have` list that does not parse, no path): it still cancels a running job -- as any FILE
// does -- and still gets its ONE FDONE, "error" with this sentence (TrPipe.h: exactly one per FILE).
struct FileReq {
    trpipe::WireFile f;
    std::string      refuse;
    FileReq(const trpipe::WireFile& w, std::string why = std::string()) : f(w), refuse(std::move(why)) {}
};

struct Shared {
    std::mutex                      mx;
    // (SUVTITLE SPLIT: AudioPB's `outbox` -- the live LINE / PART / END queue, pipeline -> servant -- was here.)
    trpipe::WireState               state;
    bool                            stateDirty = true;
    std::string                     mode   = "auto";
    std::string                     preset = "balanced";
    std::string                     langA  = "en";
    std::string                     langB  = "zh";
    bool                            presetChanged = false;
    // (... and `clearAsked`: CTL clear=1 dropped the live queue at once. Nothing queues here: ignored.)
    std::atomic<bool>               quit{ false };
    // W1: the servant's doorbell. Set ONCE, before the pipeline thread exists, and cleared
    // only after the join (runHost) -- so the pipeline thread never sees it change and it
    // is read without the mutex on purpose. AUTO-RESET: several
    // pushes before the servant looks collapse into one lap, and a push that lands while
    // it is already draining costs one harmless extra lap instead of a lost message.
    // nullptr is legal and means "nobody is listening" (the selftest drives the pipeline
    // stages directly and owns no pipe).
    HANDLE                          wake = nullptr;

    // ---- FILE MODE (wave WB) -------------------------------------------------------------
    // Requests IN (the servant appends, the pipeline drains at the top of every lap), and the
    // file messages OUT -- in their OWN queue, which is never trimmed: a dropped FCUE is a
    // permanent hole in the subtitles and a dropped FDONE hangs the tray's player, where a
    // dropped PART costs one frame of a live line. Bounded by BACK-PRESSURE on the producer
    // instead (kFileOutSoftCap): the job simply does not take its next step while the tray is
    // that far behind -- and a tray that stops reading altogether breaks the pipe at
    // writeMessage's 3 s deadline, which ends the helper. FPROXY requests go to the proxy
    // worker (runHost), never through the pipeline.
    std::deque<FileReq>             fileReqs;
    std::vector<uint64_t>           fileCancels;
    std::deque<std::string>         fileOut;            // packed FPROG/FCUE/FDONE/FPROXYDONE
    uint64_t                        runningJob = 0;     // the job the pipeline is running (0 = none)
    // v2 (WD): THE PLAYHEAD -- one slot, latest wins. The servant writes it for the running job or a
    // queued one (anything else is stale and dropped); the pipeline takes it at its next step
    // boundary, so a burst of FFOCUS while the user scrubs costs one slot write each and the job
    // only ever sees the last one. `focusTick` = when it arrived (the job waits for it to settle
    // before it moves -- kFocusSettleMs).
    uint64_t                        focusJob = 0, focusMs = 0, focusSeq = 0, focusTick = 0;
    // ... and how the playhead got there (review C1): the CHAIN -- the last FFOCUS (or the FILE) for
    // chainJob -- and `focusJump`, raised when an FFOCUS since the job last took the slot did NOT
    // follow the one before it like playback (kFocusChainGapMs, kFocusPlayRate).
    uint64_t                        chainJob = 0, chainMs = 0, chainTick = 0;
    bool                            focusJump = false;
    // The slot's seq and ms MIRRORED lock-free (review P3): a decode-to-seek polls them between two
    // decoded buffers -- a new playhead far from where the seek is going abandons it at once.
    std::atomic<uint64_t>           focusSeqLive{ 0 }, focusMsLive{ 0 }, focusJobLive{ 0 };
    // THE JOB THAT MUST STOP, BY ID: set by the servant (FCANCEL, a superseding FILE -- the
    // running job's id; QUIT -- kAbortAll) and polled INSIDE a model load (ChildServer::spawn's
    // abort), so a 3 GB load is not waited out. AN ID, NOT A FLAG (WB, after the review): a flag
    // raised for a job that was ending as the FCANCEL arrived stayed raised, and the NEXT job's
    // load was aborted on every lap for ever. A stale id can only ever name a finished job.
    std::atomic<uint64_t>           abortJob{ 0 };
};
constexpr uint64_t kAbortAll = ~0ull;    // abortJob on the way out: whatever is loading stops

// The file outbox's soft cap: past it, the JOB waits; nothing is ever dropped.
constexpr size_t kFileOutSoftCap = 256;

// Ring the servant's doorbell. ALWAYS called with sh.mx released: SetEvent can wake the
// servant before this thread returns, and the first thing the servant does is take sh.mx.
inline void wakeServant(const Shared& sh) { if (sh.wake) ::SetEvent(sh.wake); }

void setState(Shared& sh, const char* status, const std::string& err = std::string()) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        if (sh.state.status != status || sh.state.err != err) {
            sh.state.status = status;
            sh.state.err = err;
            sh.stateDirty = true;
            changed = true;
        }
    }
    if (changed) wakeServant(sh);   // outside the lock: see wakeServant
}

// (SUVTITLE SPLIT: spkConfigFrom -- the diarization stage's config -- and pushOut -- the live outbox's
// producer -- were here.)

// ===========================================================================
// FILE MODE (wave WB): subtitling a RECORDING. The wire contract is TrPipe.h's "FILE MODE"
// block; the decisions are TrFileJob.h's (windows, commit, cues) and TrMedia.h's (decoding,
// the timeline). What lives here is the SEQUENCING: a job is a state machine run on the
// PIPELINE thread -- the only thread allowed to call the servers, which is what keeps "one GPU
// call at a time" true by construction -- one bounded step per lap (one model call, or one
// model load that a cancel interrupts), so CTL / FCANCEL / QUIT are never more than one call
// away. While a job runs the live path is paused and its audio DROPPED (TrPipe.h).
// ===========================================================================
constexpr DWORD kFileAsrTimeoutMs = 300000;   // one 30 s window on a CPU-only machine
constexpr int   kFileRetries      = 2;        // server restarts a job may spend

// ---- FILE MODE v2 (WD): follow the playhead ----------------------------------------------------
// A focus JUMP (a new run elsewhere) waits until the playhead has been still this long: a user
// scrubbing sends FFOCUS by the dozen, and only where he lets go deserves a seek and a model call.
// A single click-seek pays it once; the decode-ahead after it is ~50-100 ms anyway.
constexpr uint64_t kFocusSettleMs = 250;
// ... and a focus a little ahead of the run in progress is no jump at all: the run gets there in
// about the time a new run needs for its first subtitle (one short window + one translation, ~1 s;
// the run moves ~10x real time), without leaving a hole behind it to fill later.
constexpr double   kFocusReachSec = 15.0;
// A gap between analysed stretches shorter than this holds no subtitle (a cue needs minGap on each
// side and a word in between): it is counted analysed without a model call. Rounding residue, mostly.
constexpr uint64_t kMinGapSamples = kFileRate / 10;           // 100 ms
constexpr uint64_t kNoLimit = ~0ull;
// THE PLAYHEAD PLAYING (review C1). The tray reports a playing playhead every 5 s (its tick) and sends
// an FFOCUS whenever the subtitled lead under it is short -- every tick while it plays in a gap. An
// FFOCUS that follows the one before it like PLAYBACK -- within kFocusChainGapMs of it, forward by at
// most kFocusPlayRate x the time between them + kFocusPlaySlackMs (the player's speeds go to 2x) -- is
// no seek: a run the playhead is moving through, or ahead of, carries on whatever its distance
// (applyFocus). A pause between two FFOCUS (no tick while paused) breaks the chain: what follows it is
// judged as a seek. Measured by the SERVANT on every FFOCUS, not by the job, which only sees the last.
constexpr uint64_t kFocusChainGapMs  = 12000;   // the tick's 5 s, one lost or late tick included
constexpr double   kFocusPlayRate    = 2.5;
constexpr uint64_t kFocusPlaySlackMs = 2000;
// TEST-ONLY (WD, review C1): the file job's CLOCK -- the focus settle, the chain, the FPROG cadence. Null
// = GetTickCount64. [8]'s CPU-speed simulation runs it virtually (a 55 s window costs no real minute).
std::function<uint64_t()> g_fileClockForTest;
inline uint64_t fileNowMs() { return g_fileClockForTest ? g_fileClockForTest() : ::GetTickCount64(); }

// Where a job's wire messages go: the servant's never-dropped file outbox (the helper), or a
// list the selftest reads back. Both receive the EXACT packed lines the tray is sent.
struct FileSink {
    virtual ~FileSink() = default;
    virtual void   push(const std::string& msg) = 0;
    virtual size_t backlog() const = 0;
};

struct SharedFileSink final : FileSink {
    Shared& sh;
    explicit SharedFileSink(Shared& s) : sh(s) {}
    void push(const std::string& msg) override {
        { std::lock_guard<std::mutex> lk(sh.mx); sh.fileOut.push_back(msg); }
        wakeServant(sh);
    }
    size_t backlog() const override {
        std::lock_guard<std::mutex> lk(sh.mx);
        return sh.fileOut.size();
    }
};

// What a finished job leaves behind for whoever wants the numbers (the selftest, the log).
struct FileJobStats {
    uint64_t    jobId = 0;
    std::string result, err, asrModel, mtModel;
    uint32_t    cues = 0, windows = 0, hardCuts = 0, junkSegments = 0, mtCalls = 0;
    uint32_t    degenerate = 0;           // decodes that looped and were done again
    uint32_t    srcNet = 0, dstNet = 0;   // texts the script net (zh-tw / zh) actually rewrote
    uint64_t    asrMs = 0, mtMs = 0, wallMs = 0, loadMs = 0;
    double      audioSec = 0.0, skippedSec = 0.0;
    bool        hasVideo = false;
    uint64_t    totalMs = 0;
    // v2 (WD)
    uint32_t    loads = 0;                // model loads (Q2: a FILE whose `have` covers it all: 0)
    uint32_t    runs = 0, retargets = 0, keeps = 0, cutCues = 0, seeks = 0;
    uint32_t    playKeeps = 0;            // keeps a playing playhead earned beyond kFocusReachSec (C1)
    uint32_t    seekAborts = 0;           // seeks abandoned half way (P3)
    uint32_t    cutWords = 0;             // words heard only up to a limit, dropped (P1's fallback)
    uint32_t    prerolls = 0, postrolls = 0;   // windows that heard past a join in speech (P1)
    std::vector<WindowPlan> plans;        // every window sent to whisper, in order (the selftest)
    // WF (film-grade cues)
    bool        dtw = false;              // the file whisper ran with DTW
    uint32_t    wordWindows = 0;          // accepted windows that committed real words (review P7) ...
    uint32_t    timedWindows = 0;         // ... and those of them whose words carried DTW times
    uint32_t    langRedecodes = 0;        // windows decoded again, forced into the pair (D)
    uint32_t    langEmpty = 0;            // ... and that then held no real text
    uint32_t    thirdLang = 0;            // windows kept in a confident THIRD language (review P1)
    uint32_t    langFallback = 0;         // forced decodes that kept the spoken language: the first one used
    uint32_t    dtwDropped = 0;           // DTW given up after the file whisper failed with it (review P3)
    uint32_t    foreignSegs = 0, tagSegs = 0;   // segments in another script / sound tags + credits (D)
    uint32_t    heldBack = 0;             // hard cuts that committed nothing and came back to the speech (B)
    uint32_t    dedupWords = 0;           // words dropped as already committed (B)
    uint32_t    mergedFrags = 0;          // fragments joined to a neighbour (E)
    // WH: the sound track analysed, as the reader found it (TrMedia.cpp "WHICH SOUND TRACK"); "" = never opened
    std::string track;
    int         trackOrd = -1;
    uint32_t    trackId = 0, trackStream = 0, trackStreams = 0;
    std::string trackLang, trackHow;
};

struct FileJob {
    enum class Phase { Open, Load, Run, Drain };
    Phase                 phase = Phase::Open;
    trpipe::WireFile      req;
    std::wstring          path;
    std::string           quality;
    // WH: the sound track FILE's `audio` names ("" = the first one) -- the reader keeps it across every
    // re-open a seek makes (TrMedia.h)
    AudioTrack            audio;
    Routing               r;                // the job's pair/mode SNAPSHOT (TrPipe.h)
    TrContext             ctx;              // the job's OWN memory: the live one is never touched
    std::string           asrModel, mtModel;
    MediaReader           reader;
    EnergyTrack           energy;
    CueBuilder            cues;
    std::vector<float>    buf;              // decoded 16 kHz mono audio, from bufStart
    uint64_t              bufStart = 0;
    uint64_t              cursor = 0;       // samples: everything before it has been analysed
    bool                  first = true;
    bool                  dropTail = false; // the last window held junk: next prompt = glossary only
    double                lastWordEnd = -1.0;
    std::vector<FileWord> lastTail;         // the last committed words (dedupe after a hard cut)
    bool                  lastHard = false;
    bool                  cuesFinished = false;
    std::deque<FileCue>   toTranslate;
    uint32_t              sent = 0;         // FCUEs sent = FDONE's count
    uint64_t              totalMs = 0;
    bool                  hasVideo = false;
    uint64_t              doneMs = 0;       // the frontier last sent
    uint64_t              progTick = 0;
    int                   retries = 0;
    WindowPlan            retryPlan;        // a DEGENERATE window being decoded again
    int                   retryAttempt = 0; // 0 = none, 1 = unprompted, 2 = shortened, 3 = language forced (WF)
    // WF, D: the current window's language when it is FORCED into the pair ("" = auto), the committed
    // words per pair language (the file's dominant one), and the last ~30 s of committed words (B)
    std::string           forcedLang;
    std::map<std::string, uint64_t> langWords;
    std::deque<FileWord>  recentWords;
    // review P1: the window's FIRST decode, while it is decoded again forced (the fallback when the forced
    // one keeps the spoken language); review P3: the file whisper failed WITH DTW -- restarted without it
    AsrResult             firstAsr;
    bool                  haveFirstAsr = false;
    bool                  noDtw = false;
    bool                  asrFailed = false;   // the last Transport came from the window's whisper call
    // THE FILE BROKE OFF (WB, after the review): the reader's sentence. From then on the reader
    // counts as ended -- the audio already decoded is still analysed and every cue still goes
    // out -- and the job's one FDONE is "error" with this text instead of "ok".
    std::string           failErr;
    FileJobStats          st;
    uint64_t              t0 = 0, loadT0 = 0;

    // ---- FILE MODE v2 (WD): FOLLOW THE PLAYHEAD (TrPipe.h) ------------------------------------
    // Positions are 16 kHz SAMPLES on the media timeline; ranges are trpipe::MsRange used in samples
    // here and converted to ms only on the wire (merged first, so contiguous never becomes a gap).
    bool                  opened = false;   // past Open: an ending sends its ranges (FPROG) first
    std::vector<trpipe::MsRange> have;      // the tray's ranges (FILE), merged
    std::vector<trpipe::MsRange> done;      // this job's finished stretches, merged
    uint64_t              fileEnd = 0;      // the file's end; 0 = not known yet (no duration, no EOF)
    // the playhead
    uint64_t              focusMs = 0;      // the focus the order of work follows
    bool                  focusNew = false; // a newer FFOCUS arrived: applied at the next step boundary
    bool                  focusNewJump = false;  // ... and it (or one before it, not yet applied) was a SEEK
    uint64_t              focusNewMs = 0, focusTick = 0, focusSeqSeen = 0;
    // the slot as the servant writes it, lock-free (Shared::focus*Live): a seek polls it (review P3)
    const std::atomic<uint64_t>* focusSeqLive = nullptr;
    const std::atomic<uint64_t>* focusMsLive = nullptr;
    const std::atomic<uint64_t>* focusJobLive = nullptr;
    // the current run: [runStart, runLimit), cursor inside it
    bool                  runActive = false;
    bool                  runDraining = false;   // its end reached: its last cues being translated
    bool                  runPending = false;    // a jump waiting for the playhead to settle
    bool                  runFocus = false;      // it starts at the playhead (short first window)
    bool                  runFirst = true;       // its first window not yet planned
    bool                  runPreroll = false;    // it starts mid-speech: its first window hears before it (P1)
    uint64_t              runStart = 0;          // its first cursor: its analysed stretch starts here
    uint64_t              runLimit = kNoLimit;   // the start of the next analysed stretch (~0 = none)
    uint64_t              runReported = 0;       // its frontier as last reported (never moves back)
    uint64_t              runSentEnd = 0;        // the end of its last cue SENT (a range covers its cues)
    // where its analysed stretch ENDS once every cue of it is out -- decided with its end: its limit, the
    // file's end, where the audio broke off (review C2), or its cursor when the playhead jumped away
    uint64_t              runEnd = 0;
    bool                  focusFresh = true;     // the next forward run is the playhead's own
    uint64_t              regionStart = 0;       // where the decode in `buf` began (a seek moves it)
    bool                  mustSeek = false;      // a seek was aborted: the reader is nowhere until the next
    std::string           lastProg;              // the last FPROG's ranges + done (dedupe a burst)
};

// ---- v2: ranges (samples) ------------------------------------------------------------------------
// (clamped at 2^40 ms -- 35 years: a garbage `have` / focus from the tray cannot wrap the product)
inline uint64_t msToSamples(uint64_t ms) { return std::min<uint64_t>(ms, 1ull << 40) * (kFileRate / 1000); }

// What is ANALYSED now: `have`, the job's finished stretches, and -- `withRun` -- the current run's
// stretch up to its frontier. Merged.
std::vector<trpipe::MsRange> analysedSet(const FileJob& j, bool withRun) {
    std::vector<trpipe::MsRange> a = j.have;
    a.insert(a.end(), j.done.begin(), j.done.end());
    if (withRun && j.runActive && j.runReported > j.runStart) a.push_back({ j.runStart, j.runReported });
    trpipe::mergeRanges(a);
    return a;
}

// The first point >= p that no analysed range covers (a merged set: one pass).
uint64_t firstGapAtOrAfter(const std::vector<trpipe::MsRange>& a, uint64_t p) {
    for (const auto& r : a) if (r.s <= p && p < r.e) p = r.e;
    return p;
}

// The gap holding p (p unanalysed): [start, end). end = the next range's start, or the file's end
// (kNoLimit while it is not known).
void gapAround(const FileJob& j, const std::vector<trpipe::MsRange>& a, uint64_t p, uint64_t& gs, uint64_t& ge) {
    gs = 0;
    ge = j.fileEnd ? j.fileEnd : kNoLimit;
    for (const auto& r : a) {
        if (r.e <= p) gs = std::max(gs, r.e);
        else if (r.s > p) { ge = std::min(ge, r.s); break; }
    }
}

// The wire's ranges: samples -> ms, a start rounded UP and an end DOWN (never claim more than was
// analysed), empties dropped.
std::vector<trpipe::MsRange> toWireMs(const std::vector<trpipe::MsRange>& a) {
    std::vector<trpipe::MsRange> o;
    const uint64_t k = kFileRate / 1000;
    for (const auto& r : a) {
        trpipe::MsRange m{ (r.s + k - 1) / k, r.e == kNoLimit ? r.e : r.e / k };
        if (m.e > m.s) o.push_back(m);
    }
    return o;
}

// ---- v2 (WD) selftest seams: inert when empty ------------------------------------------------------
// A stand-in WHISPER that knows where it is (the window [s, e) in samples, the prompt it was sent) and
// a stand-in MT that sees the context it was given: what lets [8] drive whole v2 jobs -- runs, jumps,
// limits, the context reset -- deterministically and without a GPU. NEVER set by the helper itself.
std::function<std::string(uint64_t, uint64_t, const std::string&)> g_fileAsrForTest;
// WF: the `language` field of the file job's LAST whisper request ("auto", or a pair language it was
// forced to) -- written before every decode, read by the selftest's stand-ins only.
std::string g_fileAsrLangSentForTest;
std::function<std::string(const std::string&, const std::string&, const std::string&, size_t)> g_fileMtForTest;
bool g_fileNoModelsForTest   = false;   // the Load phase brings nothing up (the stand-ins answer)
bool g_fileNoDurationForTest = false;   // the container's duration reads as unknown (WE's Q4)
// TEST-ONLY (WD, the CPU runs): the file job's models -- AUDIOPB_TR_TEST_FILE_ASR / _MT, read by main()
// only together with the CPU switch. "" = the shipping choice (chooseFileModels, makeFileJob).
std::string g_fileAsrModelForTest, g_fileMtModelForTest;

// v2: the current run's FRONTIER (samples). Everything of the run before it is FINAL and SENT -- WE's
// Q1: a range never claims a stretch whose cues are not all out yet, or a resume would skip it for
// ever. Strict: no later cue can start before it --
//   * the cursor less 0.45 s (a cue's start may snap back onto its onset by 0.39 s),
//   * the earliest start a held cue can still be given (CueBuilder::holdFromStrict),
//   * the first cue awaiting its translation;
// and never before the end of the last cue sent (a range covers its cues: the run that fills the
// stretch after it starts its cues past that end). Reporting it raises the cue builder's floor to it
// -- the backstop that makes it monotonic and strict by construction.
void reportFrontier(FileJob& j) {
    if (!j.runActive) return;
    double t = (double)j.cursor / kFileRate - 0.45;
    t = std::min(t, j.cues.holdFromStrict());
    if (!j.toTranslate.empty()) t = std::min(t, j.toTranslate.front().start);
    uint64_t f = t > 0.0 ? (uint64_t)std::floor(t * kFileRate) : 0;
    f = std::max(f, j.runSentEnd);
    f = std::max(f, j.runStart);
    if (j.runLimit != kNoLimit) f = std::min(f, j.runLimit);
    if (f > j.runReported) j.runReported = f;
    j.cues.raiseFloor((double)j.runReported / kFileRate);
}

// FPROG v2: `ranges` = what is analysed now (have + this job's, merged), doneMs = their total.
// `force` = send even when nothing changed (loading, a window, the end); otherwise an identical
// FPROG within 250 ms is not repeated (a burst of FFOCUS costs nothing but the last one).
void sendProg(FileJob& j, FileSink& sink, const char* phase, bool force = true) {
    reportFrontier(j);
    const std::vector<trpipe::MsRange> ms = toWireMs(analysedSet(j, true));
    trpipe::WireFileProg p;
    p.jobId    = j.req.jobId;
    p.phase    = phase;
    p.doneMs   = trpipe::rangesTotal(ms);
    p.totalMs  = j.totalMs;
    p.asrModel = j.asrModel;
    p.mtModel  = j.mtModel;
    p.hasVideo = j.hasVideo;
    p.ranges   = trpipe::packRanges(ms);
    const std::string key = std::string(phase) + "|" + p.ranges;
    const uint64_t now = fileNowMs();
    if (!force && key == j.lastProg && now - j.progTick < 250) return;
    j.lastProg = key;
    j.doneMs   = p.doneMs;
    j.progTick = now;
    sink.push(trpipe::packFileProg(p));
}

// Between windows (after a translation): an FPROG when the analysed total moved >= 1 s, or moved at
// all and 2 s have passed (v1's cadence).
void maybeSendProg(FileJob& j, FileSink& sink) {
    reportFrontier(j);
    const uint64_t f = trpipe::rangesTotal(toWireMs(analysedSet(j, true)));
    const uint64_t now = fileNowMs();
    if (f >= j.doneMs + 1000 || (f > j.doneMs && now - j.progTick >= 2000)) sendProg(j, sink, "analyzing");
}

// EXACTLY ONE FDONE per FILE, sent BEFORE anything is torn down, so the tray never waits on
// our cleanup or on the live models coming back. v2: a job that got past Open and does not end "ok"
// first sends its RANGES -- what is analysed now -- so the tray can resume it exactly there.
void finishJob(std::unique_ptr<FileJob>& job, Servers& srv, FileSink& sink, const char* result,
               const std::string& err, FileJobStats* out) {
    FileJob& j = *job;
    if (j.opened && std::strcmp(result, "ok") != 0)
        sendProg(j, sink, (j.phase == FileJob::Phase::Open || j.phase == FileJob::Phase::Load) ? "loading" : "analyzing");
    trpipe::WireFileDone d;
    d.jobId  = j.req.jobId;
    d.result = result;
    d.count  = j.sent;
    d.err    = err;
    sink.push(trpipe::packFileDone(d));
    j.st.jobId    = j.req.jobId;
    j.st.result   = result;
    j.st.err      = err;
    j.st.cues     = j.sent;
    j.st.asrModel = j.asrModel;
    j.st.mtModel  = j.mtModel;
    j.st.hasVideo = j.hasVideo;
    j.st.totalMs  = j.totalMs;
    j.st.wallMs   = ::GetTickCount64() - j.t0;
    // what THIS job analysed (its runs; not `have`) -- a buffer position means nothing after a seek
    {
        std::vector<trpipe::MsRange> mine = j.done;
        if (j.runActive && j.runReported > j.runStart) mine.push_back({ j.runStart, j.runReported });
        trpipe::mergeRanges(mine);
        j.st.audioSec = (double)trpipe::rangesTotal(mine) / kFileRate;
    }
    j.st.cutCues  = j.cues.droppedCut();
    j.st.cutWords = j.cues.cutWords();
    j.st.mergedFrags = j.cues.mergedFragments();
    j.st.dtw      = srv.fileDtw() && j.st.timedWindows > 0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[SuVtitleEngine] FILE job %llu %s: %u cue(s), %u window(s) (%u hard), %u run(s), "
                  "asr %llu ms, mt %llu ms, wall %llu ms, %.1f s of audio\n",
                  (unsigned long long)j.req.jobId, result, j.sent, j.st.windows, j.st.hardCuts, j.st.runs,
                  (unsigned long long)j.st.asrMs, (unsigned long long)j.st.mtMs,
                  (unsigned long long)j.st.wallMs, j.st.audioSec);
    engineLog(line);                      // SUVTITLE: + engine.log (host mode)
    if (out) *out = j.st;
    job.reset();          // closes the reader ON THIS THREAD, the one that opened it (COM/MF)
    srv.endFile();        // frees the job's whisper; the live pair comes back via ensureUp
}

// WH: the sentence for a FILE whose `audio` does not parse (TrPipe.h: refused whole, like a bad `have`).
// The field is echoed -- printable ASCII only, 24 characters at most: it came off the wire.
std::string badAudioSentence(const std::string& audio) {
    std::string shown;
    for (char c : audio) {
        if (shown.size() >= 24) { shown += "..."; break; }
        shown += (c >= 32 && c <= 126) ? c : '?';
    }
    return "The request names a sound track that could not be read (\"" + shown + "\").";
}

// A FILE request -> a job, or a refusal. /!\ A pair or a mode the helper cannot honour is an
// FDONE error and NEVER a substituted value (WC): the tray caches the result under the pair
// it asked for, so subtitles made under another one would be filed under the wrong name.
// "" means "the helper's current value", exactly as on a CTL.
bool makeFileJob(const trpipe::WireFile& f, const std::string& curA, const std::string& curB,
                 const std::string& curMode, const TrConfig& cfg, std::unique_ptr<FileJob>& job,
                 std::string& err) {
    const std::string a = f.langA.empty() ? curA : normalizeLangCode(f.langA);
    const std::string b = f.langB.empty() ? curB : normalizeLangCode(f.langB);
    if (a.empty()) { err = "Unsupported language code \"" + f.langA + "\"."; return false; }
    if (b.empty()) { err = "Unsupported language code \"" + f.langB + "\"."; return false; }
    if (a == b)    { err = "The two languages of the pair are the same (" + a + ")."; return false; }
    std::string mode = f.mode.empty() ? curMode : f.mode;
    if      (mode == "en2zh") mode = "a2b";        // the legacy tokens, read like unpackCtl does
    else if (mode == "zh2en") mode = "b2a";
    if (mode != "auto" && mode != "a2b" && mode != "b2a") {
        err = "Unknown direction \"" + f.mode + "\" (auto, a2b or b2a).";
        return false;
    }
    const std::string q = f.quality.empty() ? "auto" : f.quality;
    if (q != "auto" && q != "max" && q != "fast") {
        err = "Unknown quality \"" + f.quality + "\" (auto, max or fast).";
        return false;
    }
    // v2: what the tray already holds (unpackFile refused a list that does not parse).
    std::vector<trpipe::MsRange> haveMs;
    if (!trpipe::parseRanges(f.have, haveMs)) {
        err = "The request's list of already-subtitled stretches could not be read.";
        return false;
    }
    // WH: the sound track (unpackFile refused one that does not parse; --selftest-file builds its own FILE).
    trpipe::AudioSel sel;
    if (!trpipe::parseAudioSel(f.audio, sel)) {
        err = badAudioSentence(f.audio);
        return false;
    }
    job.reset(new FileJob);
    job->req       = f;
    job->path      = utf8ToWideL(f.path);
    job->quality   = q;
    job->audio.chosen  = !f.audio.empty();
    job->audio.ord     = sel.ord;
    job->audio.trackId = sel.trackId;
    job->r.mode    = mode;
    job->r.langA   = a;
    job->r.langB   = b;
    job->r.asrPrompt = cfg.asrPrompt;
    // More context than live: nothing is waiting, and a subtitle's pronoun is resolved by the
    // lines before it.
    job->r.mtContext = std::max(cfg.mtContext, 3);
    job->r.mtStyle   = cfg.mtStyle;              // "" = from the MT that actually loads (LOAD)
    // The 4B even when the live preset is light: MTMODEL wins, as it does live.
    job->mtModel     = cfg.mtModel.empty() ? std::string(kMtModelFile) : cfg.mtModel;
    if (!g_fileMtModelForTest.empty()) job->mtModel = g_fileMtModelForTest;   // TEST-ONLY (CPU runs)
    for (const auto& r : haveMs) job->have.push_back({ msToSamples(r.s), msToSamples(r.e) });
    trpipe::mergeRanges(job->have);
    job->focusMs = f.focusMs;
    job->t0 = ::GetTickCount64();
    return true;
}

// auto = fast = large-v3-turbo, max = large-v3 (TrPipe.h; measured, see the plan): turbo is
// 1.8-2x faster per call than large-v3 on this GPU, as accurate on EN/ZH clean and noisy, and
// better on mixed-language audio. Without turbo: the running preset's final model.
std::string g_fileLargeModelForTest;   // "" = ggml-large-v3.bin; ONLY the selftest sets it ([7c])
double      g_fileFailDecodeAtForTest = 0.0;   // > 0: the decode "breaks" there; ONLY the selftest
bool        g_fileFailSeekForTest = false;     // every seek fails (a file moved away); ONLY the selftest
std::deque<std::string> g_fileAsrRepliesForTest;   // stand-in window decodes, consumed in order; ONLY the selftest

bool chooseFileModels(FileJob& j, const Servers& srv, std::string& err) {
    static const char* kTurbo = "ggml-large-v3-turbo.bin";
    if (!g_fileAsrModelForTest.empty() && srv.modelInstalled(g_fileAsrModelForTest)) {
        j.asrModel = g_fileAsrModelForTest;
        return true;
    }
    const std::string kLarge = g_fileLargeModelForTest.empty() ? std::string("ggml-large-v3.bin")
                                                               : g_fileLargeModelForTest;
    if (j.quality == "max") {
        if (!srv.modelInstalled(kLarge)) {
            err = "ASR model missing: " + kLarge + " (the \"max\" quality needs it; the "
                  "installer does not ship it).";
            return false;
        }
        j.asrModel = kLarge;
    } else {
        j.asrModel = srv.modelInstalled(kTurbo) ? std::string(kTurbo) : srv.presetFinalModel();
    }
    return true;
}

enum class FileStep { Ok, Transport, Fatal };

// ONE cue through routing and the MT -- one model call at most.
FileStep translateOneCue(FileJob& j, Servers& srv, FileSink& sink, std::string& err) {
    const FileCue c = j.toTranslate.front();
    AsrResult a;
    a.text     = c.text;
    a.language = c.lang;
    a.chinese  = chineseByScript(c.text, c.lang);
    const std::string srcLang = sourceLangOf(a, j.r);
    const Route route = routeFor(j.r, srcLang);
    // The script nets on the FILE path too: a zh-tw line is SHOWN in Traditional (wave WA) and a zh
    // line in Simplified (WB), source and translation alike, whatever whisper or the model wrote.
    std::string src = scriptNet(c.text, srcLang);
    if (src != c.text) ++j.st.srcNet;
    std::string dst;
    if (route.pass) {
        dst = src;
    } else {
        uint32_t ms = 0;
        if (g_fileMtForTest) {
            // the selftest's stand-in model, told how much context it was given ([8]: the reset)
            dst = g_fileMtForTest(src, srcLang, route.dstLang,
                                  contextFor(j.ctx, srcLang, route.dstLang, j.r.mtContext).size());
        } else if (!g_mtReplyForTest.empty()) {
            dst = g_mtReplyForTest;       // the selftest's stand-in model (see its comment)
        } else {
            if (!mtTranslate(srv.mtPort(), src, srcLang, route.dstLang,
                             contextFor(j.ctx, srcLang, route.dstLang, j.r.mtContext),
                             j.r.mtStyle, dst, ms, kMtTimeoutMs, err))
                return FileStep::Transport;
            ++j.st.mtCalls;
            j.st.mtMs += ms;
        }
        const std::string t = scriptNet(dst, route.dstLang);
        if (t != dst) ++j.st.dstNet;
        dst = t;
    }
    src = oneLine(src);                       // WC: no hard line break inside a cue
    dst = oneLine(dst);
    j.toTranslate.pop_front();
    if (dst.empty()) return FileStep::Ok;     // nothing to show: not a subtitle
    j.ctx.lastFinalLang = srcLang;
    j.ctx.rememberSrc(srcLang, src);
    if (!route.pass) {
        j.ctx.pairs.push_back(MtPair{ src, dst, srcLang, route.dstLang });
        while (j.ctx.pairs.size() > 8) j.ctx.pairs.pop_front();
    }
    trpipe::WireCue w;
    w.jobId   = j.req.jobId;
    w.idx     = j.sent++;
    w.startMs = (uint64_t)std::llround(c.start * 1000.0);
    w.endMs   = std::max<uint64_t>((uint64_t)std::llround(c.end * 1000.0), w.startMs + 1);
    w.lang    = srcLang;
    w.dstLang = route.dstLang;
    w.pass    = route.pass;
    w.src     = src;
    w.dst     = dst;
    sink.push(trpipe::packCue(w));
    // v2: the run's ranges cover their cues (reportFrontier) -- AS SENT, in whole ms (review P5: the
    // cue's end rounded to the nearest ms could pass a range end floored from samples by 1 ms)
    j.runSentEnd = std::max<uint64_t>(j.runSentEnd, w.endMs * (kFileRate / 1000));
    return FileStep::Ok;
}

void trimFileBuf(FileJob& j) {
    // what lies behind the cursor is kept for a pre-roll (review P1: a run starting mid-speech hears
    // kFileJoinRollSec before it)
    const uint64_t back = (uint64_t)(kFileJoinRollSec * kFileRate);
    const uint64_t keepFrom = j.cursor > back ? j.cursor - back : 0;
    if (keepFrom > j.bufStart) {
        const size_t drop = (size_t)std::min<uint64_t>(keepFrom - j.bufStart, j.buf.size());
        j.buf.erase(j.buf.begin(), j.buf.begin() + (std::ptrdiff_t)drop);
        j.bufStart += drop;
    }
}

// The reader is over when it says so -- or when it FAILED (j.failErr): what was decoded before
// the break is still analysed, as if the file ended there.
bool readerOver(const FileJob& j) { return j.reader.eof() || !j.failErr.empty(); }

// Decode ~4 s more at the end of `buf` (and of the energy track, at the same stream position).
void decodeMore(FileJob& j) {
    std::vector<float> chunk;
    std::string derr;
    const uint64_t at = j.bufStart + j.buf.size();
    if (g_fileFailDecodeAtForTest > 0.0 && (double)at / kFileRate >= g_fileFailDecodeAtForTest) {
        j.failErr = "The file is damaged or truncated at 0:" + std::to_string((int)g_fileFailDecodeAtForTest)
                  + " (selftest).";
        return;
    }
    if (!j.reader.readFloat(chunk, (size_t)kFileRate * 4, derr)) { j.failErr = derr; return; }
    if (!chunk.empty()) {
        j.energy.pushAt(at, chunk.data(), chunk.size());
        j.buf.insert(j.buf.end(), chunk.begin(), chunk.end());
    }
}

// v2: make the decode cover the stream from sample D (hop-aligned): already in `buf`, a little
// ahead (decoded forward, contiguous), or a SEEK (TrMedia: exact, on the linear decode's timeline).
// The seek polls `abort` (review P3): Aborted = the reader is nowhere (mustSeek) and the caller gives
// the lap back. A seek that FAILS leaves `buf` empty AT D (review C2): audio decoded elsewhere is
// never taken for the stretch the run was going to analyse.
enum class Positioned { Ok, Failed, Aborted };
Positioned positionDecode(FileJob& j, uint64_t D, const std::function<bool()>& abort) {
    const uint64_t bufEnd = j.bufStart + j.buf.size();
    if (!j.mustSeek) {
        if (D >= j.bufStart && D <= bufEnd) return Positioned::Ok;
        if (D > bufEnd && D - bufEnd <= (uint64_t)(10 * kFileRate)) return Positioned::Ok;   // decodeMore gets there
    }
    std::string err;
    bool ok = false;
    if (g_fileFailSeekForTest) err = "The file could not be read at this point (selftest).";
    else                       ok = j.reader.seek(D, err, abort);
    j.buf.clear();
    j.bufStart = D;
    j.regionStart = D;
    if (!ok) {
        if (!g_fileFailSeekForTest && j.reader.lastSeekAborted()) { j.mustSeek = true; return Positioned::Aborted; }
        j.failErr = err.empty() ? std::string("The file could not be read at this point.") : err;
        return Positioned::Failed;
    }
    j.mustSeek = false;
    ++j.st.seeks;
    return Positioned::Ok;
}

// v2: choose and OPEN the next run -- the order of work (TrPipe.h): from the playhead forward to the
// end of the file, jumping over every analysed stretch, then the gaps left before it, earliest first.
// None = nothing is left to analyse (or the reader broke while getting there: failErr); Aborted = the
// seek there was abandoned (FCANCEL, QUIT, a new playhead far away): the next lap decides again.
enum class NextRun { Started, None, Aborted };
NextRun startNextRun(FileJob& j, const std::function<bool()>& abort) {
    uint64_t P = 0, gs = 0, ge = kNoLimit;
    bool found = false, forward = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        uint64_t p = pass == 0 ? msToSamples(j.focusMs) : 0;
        for (int guard = 0; guard < 1000000; ++guard) {
            const std::vector<trpipe::MsRange> a = analysedSet(j, false);
            p = firstGapAtOrAfter(a, p);
            if (j.fileEnd && p >= j.fileEnd) break;
            gapAround(j, a, p, gs, ge);
            if (ge != kNoLimit && ge - gs < kMinGapSamples) {
                // too short to hold a subtitle: analysed as it is (rounding residue, mostly)
                j.done.push_back({ gs, ge });
                trpipe::mergeRanges(j.done);
                continue;
            }
            P = p;
            found = true;
            forward = pass == 0;
            break;
        }
    }
    if (!found) return NextRun::None;
    // its LIMIT: the start of the next analysed stretch -- or none, when the gap runs to the end
    const uint64_t limit = (ge != kNoLimit && (j.fileEnd == 0 || ge < j.fileEnd)) ? ge : kNoLimit;
    // where it may begin: up to kFileRunLookback before P -- kFileRunLookbackFar on a pause when there
    // is none that close (review P1: a sentence begun before the playhead heard whole) -- never before
    // the gap (an analysed stretch)
    const uint64_t back = (uint64_t)(kFileRunLookback * kFileRate), backFar = (uint64_t)(kFileRunLookbackFar * kFileRate);
    const uint64_t lo    = std::max(gs, P > back ? P - back : 0);
    const uint64_t loFar = std::max(gs, P > backFar ? P - backFar : 0);
    // decoded from a pre-roll before that: the far pause search and a run starting mid-speech need it
    const uint64_t roll = (uint64_t)(kFileJoinRollSec * kFileRate);
    const uint64_t D = (loFar > roll ? loFar - roll : 0) / (uint64_t)kFileHop * (uint64_t)kFileHop;
    if (positionDecode(j, D, abort) == Positioned::Aborted) return NextRun::Aborted;
    while (!readerOver(j) && j.bufStart + j.buf.size() < P + kFileRate) decodeMore(j);
    if (!j.failErr.empty() && j.bufStart + j.buf.size() <= lo) return NextRun::None;   // broke before getting there
    bool midSpeech = false;
    const uint64_t c0 = P > lo ? runStartCursor(j.energy, P, lo, loFar, midSpeech) : lo;
    j.runActive   = true;
    j.runDraining = false;
    j.runPending  = false;
    // P1: a run that starts INSIDE speech -- no pause found before the playhead, or right where an
    // analysed stretch ends mid-sentence -- hears a pre-roll; one starting on a pause needs none
    j.runPreroll  = c0 > 0 && j.energy.active((size_t)(c0 / (uint64_t)kFileHop));
    j.runStart    = c0;
    j.runLimit    = limit;
    j.runReported = c0;
    j.runSentEnd  = 0;
    j.runEnd      = c0;
    j.runFocus    = forward && j.focusFresh;
    if (forward) j.focusFresh = false;
    j.runFirst    = j.runFocus;
    j.cursor      = c0;
    // the run's cues stay inside it (TrFileJob.h); after an analysed stretch, the usual gap after it
    const bool afterRange = (c0 == gs && gs > 0);
    j.cues.beginRun((double)c0 / kFileRate,
                    limit == kNoLimit ? std::numeric_limits<double>::infinity() : (double)limit / kFileRate,
                    afterRange);
    j.toTranslate.clear();
    j.cuesFinished = false;
    // A FRESH CONTEXT: the prompt's tail and the MT's pairs belong to another stretch of the film
    // (TrPipe.h). The language guess stays: it is the file's, not the stretch's.
    j.ctx.pairs.clear();
    j.ctx.lastFinalSrc.clear();
    j.dropTail = false;
    j.lastWordEnd = -1.0;
    j.lastTail.clear();
    j.lastHard = false;
    j.retryAttempt = 0;
    j.forcedLang.clear();
    j.haveFirstAsr = false;
    j.recentWords.clear();        // (WF) the language count stays: it is the FILE's, not the stretch's
    ++j.st.runs;
    trimFileBuf(j);
    return NextRun::Started;
}

// v2: the run ended -- every cue of it sent -- so its stretch is analysed: up to the end decided with
// its end (runEnd: its limit, the file's end, where the audio broke off, or where it stopped for a
// jump), and over every cue it sent (a range covers its cues).
void closeRun(FileJob& j) {
    const uint64_t end = std::max(j.runEnd, j.runSentEnd);
    if (end > j.runStart) {
        j.done.push_back({ j.runStart, end });
        trpipe::mergeRanges(j.done);
    }
    j.runActive = false;
    j.runDraining = false;
}

// v2: the playhead jumped elsewhere: the run STOPS at its cursor -- and nothing it already heard is
// thrown away (review C1: on a machine slower than real time, dropping a window's cues on every jump
// left the viewer with no subtitle at all, and on any machine it threw a model call away). Its held
// cues close there and join the ones waiting for their translation; the run DRAINS -- MT calls only,
// no window -- and its stretch up to the cursor is analysed once they are all out (closeRun). A
// degenerate window not yet accepted was not heard: dropped (the cursor is still its start).
void stopRun(FileJob& j) {
    j.retryAttempt = 0;
    j.forcedLang.clear();
    j.haveFirstAsr = false;
    j.cues.finish((double)j.cursor / kFileRate);
    FileCue c;
    while (j.cues.pop(c)) j.toTranslate.push_back(c);
    j.runEnd = j.cursor;
    j.runDraining = true;
}

// v2: a newer focus, at a step boundary -- i.e. after the model call in flight. KEEP the run when the
// first gap at or after the focus is where this run is going anyway -- from its frontier on, before
// its limit -- and either
//   * within kFocusReachSec of its cursor: a seek a few seconds within the text just subtitled, or
//     just ahead of the analysis -- the run gets there in about the time a new run needs; or
//   * the playhead got there by PLAYING (review C1: every FFOCUS since the last one taken followed the
//     one before it like playback -- the servant's chain, kFocusChainGapMs). A machine slower than
//     real time (the CPU fallback: ~55 s a window) falls behind a playing playhead whatever it does;
//     restarting at the playhead on every tick only threw its work away -- carrying on subtitles the
//     film in order, and a viewer who pauses gets them.
// Else JUMP: the run STOPS at its cursor and drains (stopRun: every cue it heard still goes out) and
// the next one starts at the new focus once the playhead has been still for kFocusSettleMs -- a scrub
// is only analysed where he lets go; a focus that lands while a run drains waits for it too (P5).
void applyFocus(FileJob& j, FileSink& sink) {
    if (!j.focusNew) return;
    j.focusNew = false;
    j.focusMs = j.focusNewMs;
    const bool played = !j.focusNewJump;
    j.focusNewJump = false;
    if (j.runActive && !j.runDraining) {
        reportFrontier(j);
        const uint64_t G = firstGapAtOrAfter(analysedSet(j, true), msToSamples(j.focusMs));
        const uint64_t reach = j.cursor + (uint64_t)(kFocusReachSec * kFileRate);
        if (G >= j.runReported && G < j.runLimit && !j.runPending && (G <= reach || played)) {
            ++j.st.keeps;
            if (G > reach) ++j.st.playKeeps;
            sendProg(j, sink, "analyzing", false);
            return;
        }
        ++j.st.retargets;
        stopRun(j);
    }
    // between runs, or a run finishing its last cues: the next run starts at the focus, settled
    j.focusFresh = true;
    j.runPending = true;
    sendProg(j, sink, "analyzing", false);
}

// ---- WF, D: the job's language stays INSIDE its pair -------------------------------------------------
// whisper's code -> the member of the pair it stands for ("" = outside the pair). whisper has ONE
// Mandarin ("zh", and "yue" for Cantonese): it stands for whichever Chinese the pair holds.
std::string pairLangOf(const std::string& code, const Routing& r) {
    if (code.empty()) return std::string();
    if (code == r.langA || code == r.langB) return code;
    if (code == "zh" || code == "yue") {
        if (isHanLang(r.langA)) return r.langA;
        if (isHanLang(r.langB)) return r.langB;
    }
    return std::string();
}
// The code whisper is FORCED with for a pair language (whisper knows no "zh-tw").
std::string whisperLangCode(const std::string& lang) { return isHanLang(lang) ? std::string("zh") : lang; }
// The most plausible pair language for a window whisper heard as another one (TrFileJob.h, D): the
// lock's source; else, in a file that SPEAKS ONE LANGUAGE (>= 90 % of its committed words so far, 20 at
// least), that one; else whichever of the two whisper ITSELF found clearly more likely (review P2: in a
// bilingual film an English line misheard as Welsh is forced to English, not to the language that leads);
// else the leading language (>= 20 words); else the previous line's; else the likelier of the two however
// slightly; else A. (MEASURED on the dubbed film, 11/10: with the odds first, 5 of its 8 forced windows --
// music, screams, the end credits -- went to English, whisper's prior: "The yacht! The yacht!", French
// text labelled en. Those odds are no evidence where the file is 96 % French.)
std::string plausiblePairLang(const FileJob& j, const AsrResult& a) {
    if (j.r.mode == "a2b") return j.r.langA;
    if (j.r.mode == "b2a") return j.r.langB;
    uint64_t best = 0, tot = 0;
    std::string dom;
    for (const auto& kv : j.langWords) { tot += kv.second; if (kv.second > best) { best = kv.second; dom = kv.first; } }
    if (tot >= 20 && !dom.empty() && (double)best >= 0.9 * (double)tot) return dom;
    double pa = -1.0, pb = -1.0;
    for (const auto& p : a.langProbs) {
        if (p.first == whisperLangCode(j.r.langA)) pa = p.second;
        if (p.first == whisperLangCode(j.r.langB)) pb = p.second;
    }
    if (pa != pb) {
        const double hi = std::max(pa, pb), lo = std::max(0.0, std::min(pa, pb));
        if (hi >= 0.05 && hi >= 3.0 * lo) return pa > pb ? j.r.langA : j.r.langB;
    }
    if (tot >= 20 && !dom.empty()) return dom;
    const std::string last = pairLangOf(j.ctx.lastFinalLang, j.r);
    if (!last.empty()) return last;
    if (pb > pa) return j.r.langB;
    return j.r.langA;
}

// Review P1: a window heard CONFIDENTLY in a third language (whisper's p >= kThirdLangProb) whose decode,
// judged with that language's script allowed, still holds real speech -- four words or more, three of
// them different ("E ai! E ai!" over music is not a scene) -- is that language's speech: kept, labelled
// with it, and routed into B like 1.0.6 did. Anything less sure is decoded again forced into the pair.
constexpr double kThirdLangProb = 0.8;
bool thirdLangSpeech(const CommitVerdict& v) {
    if (v.degenerate) return false;
    size_t n = 0;
    std::set<std::string> uniq;
    for (const auto& w : v.words) {
        if (!hasLetterOrDigit(w.text)) continue;
        ++n;
        for (const auto& t : normWords(w.text)) uniq.insert(t);
    }
    return n >= 4 && uniq.size() >= 3;
}
// The forced decode came back as the FIRST one -- turbo kept the spoken language (measured on the film:
// forced into French, it wrote "Oh no, stop, stop..." in English): >= 80 % of its atoms are the first's.
bool sameDecode(const AsrResult& a, const AsrResult& b) {
    const std::vector<std::string> x = normWords(a.text), y = normWords(b.text);
    if (x.size() < 3 || y.size() < 3) return false;
    size_t common = 0;
    std::multiset<std::string> ys(y.begin(), y.end());
    for (const auto& t : x) { auto it = ys.find(t); if (it != ys.end()) { ++common; ys.erase(it); } }
    return (double)common >= 0.8 * (double)std::max(x.size(), y.size());
}

// Decode-ahead, plan, and -- at most once -- transcribe ONE window of the current run. `done` = the
// run has nothing left (its limit or the end of the file); `windowed` = a model call was made.
FileStep fileWindowStep(FileJob& j, Servers& srv, std::string& err, bool& done, bool verbose, bool& windowed) {
    done = false;
    windowed = false;
    WindowPlan plan;
    if (j.retryAttempt > 0) {
        plan = j.retryPlan;          // a DEGENERATE window, decoded again (see the end)
    } else {
        double skipped = 0.0;
        bool planned = false;
        for (int guard = 0; guard < 4096 && !planned; ++guard) {
            const uint64_t bufEnd = j.bufStart + j.buf.size();
            // A minute ahead before the job's FIRST plan (the energy floor needs a sample of the
            // file), then 31 s past the cursor: a window may reach 30 s past where it starts.
            const uint64_t need = std::max<uint64_t>(j.cursor + (uint64_t)(31.0 * kFileRate),
                                                     j.first ? j.regionStart + (uint64_t)(60.0 * kFileRate) : 0);
            if (!readerOver(j) && bufEnd < need) {
                decodeMore(j);
                continue;
            }
            uint64_t skipTo = 0;
            const PlanVerdict v = planWindow(j.energy, j.cursor, bufEnd, readerOver(j), j.runFirst,
                                             plan, skipTo, j.runLimit);
            if (v == PlanVerdict::Done) { done = true; return FileStep::Ok; }
            if (v == PlanVerdict::NeedAudio) {
                if (readerOver(j)) { done = true; return FileStep::Ok; }
                decodeMore(j);
                continue;
            }
            if (v == PlanVerdict::SkipSilence) {
                // SILENCE COSTS NOTHING: no model call, and the frontier moves over it.
                const double sec = (double)(skipTo - j.cursor) / kFileRate;
                j.st.skippedSec += sec;
                skipped += sec;
                j.cursor = skipTo;
                j.runPreroll = false;                     // the run goes on from a silence: nothing cut
                trimFileBuf(j);
                if (skipped > 300.0) return FileStep::Ok;   // a lap stays bounded on a long silence
                continue;
            }
            planned = true;
        }
        if (!planned) return FileStep::Ok;
        // ---- A JOIN IN SPEECH (review P1, TrFileJob.h): what whisper hears beyond the window ------
        // A run that starts mid-speech: its first window hears kFileJoinRollSec before the cursor (the
        // words starting there are the stretch before's). A window ending at a limit the speech runs
        // across: it hears that much past the limit (the words starting there are the stretch after's).
        // Only the words starting in [keepFrom, e) are the window's; whisper hears 30 s at most.
        const uint64_t roll = (uint64_t)(kFileJoinRollSec * kFileRate), winMax = (uint64_t)(kFileWinMaxSec * kFileRate);
        if (j.runPreroll) {
            j.runPreroll = false;
            plan.rolled   = true;
            plan.hs       = std::max<uint64_t>(j.cursor > roll ? j.cursor - roll : 0, j.bufStart);
            plan.keepFrom = j.cursor;
            ++j.st.prerolls;
        }
        if (plan.why == WindowPlan::kLimit) {
            const size_t hl = (size_t)(plan.e / (uint64_t)kFileHop);
            if (hl > 0 && j.energy.active(hl - 1) && j.energy.active(hl)) {
                const uint64_t from = plan.rolled ? plan.hs : plan.s;
                const uint64_t want = std::min<uint64_t>(plan.e + roll, from + winMax);
                while (!readerOver(j) && j.bufStart + j.buf.size() < want) decodeMore(j);
                const uint64_t he = std::min<uint64_t>(want, j.bufStart + j.buf.size());
                if (he >= plan.e + (uint64_t)(0.3 * kFileRate)) {
                    // (else no room -- a 30 s window, the end of the file: the cut words, commitWindow)
                    if (!plan.rolled) { plan.rolled = true; plan.hs = plan.s; plan.keepFrom = 0; }
                    plan.he = he;
                    ++j.st.postrolls;
                }
            }
        }
        if (plan.rolled && plan.audioE() - plan.hs > winMax) plan.hs = plan.audioE() - winMax;
    }
    // ---- one window: ONE model call ----------------------------------------------------
    // (what whisper hears -- the window, and a join's pre/post-roll -- lies in `buf` by construction;
    // review C2 found one way it did not, a failed seek: any other is the job's FDONE error, never a
    // read outside the buffer)
    const uint64_t aS = plan.audioS(), aE = std::min<uint64_t>(plan.audioE(), j.bufStart + j.buf.size());
    if (aS < j.bufStart || aE <= aS || plan.e <= plan.s) {
        err = "internal: a window outside the decoded audio";
        return FileStep::Fatal;
    }
    const std::vector<float> win(j.buf.begin() + (std::ptrdiff_t)(aS - j.bufStart),
                                 j.buf.begin() + (std::ptrdiff_t)(aE - j.bufStart));
    const std::string expect = j.forcedLang.empty() ? expectedLang(j.r, j.ctx) : j.forcedLang;
    std::string prompt;
    if (j.r.asrPrompt && (j.retryAttempt == 0 || j.retryAttempt == 3)) {
        // The tail of what was said is the right prompt for what follows it -- unless the last
        // window was junk (do not feed whisper its own hallucination) or more than 10 s of
        // silence lie between them (a prompt invites noise to be "heard" as more of it). A
        // degenerate window is decoded again UNPROMPTED: the conditioning is what loops.
        const bool fresh = j.dropTail
                        || (j.lastWordEnd >= 0.0 && (double)plan.s / kFileRate - j.lastWordEnd > 10.0);
        prompt = fresh ? asrGlossaryFor(expect) : asrPromptFor(j.r, j.ctx, expect);
    }
    AsrOptions opt;
    // WF, D: auto -- or, decoding again a window heard outside the pair, the pair language it is forced to
    opt.language = j.forcedLang.empty() ? std::string("auto") : whisperLangCode(j.forcedLang);
    opt.prompt   = prompt;
    opt.audioCtx = 0;              // the FULL window: 768 frames would cut 30 s to 15.36
    opt.beamSize = 5;
    opt.bestOf   = 5;
    g_fileAsrLangSentForTest = opt.language;   // (read by the selftest's stand-in whisper only)
    if (g_fileAsrFailOnceForTest) {
        g_fileAsrFailOnceForTest = false;
        j.asrFailed = true;
        err = "test: the file whisper stopped answering";
        return FileStep::Transport;
    }
    AsrResult asr;
    if (g_fileAsrForTest) {
        // [8]'s stand-in WHISPER, which knows where the audio it hears is
        parseAsrResponse(g_fileAsrForTest(aS, aE, opt.prompt), asr);
        asr.promptSent = opt.prompt;
    } else if (!g_fileAsrRepliesForTest.empty()) {
        // The selftest's stand-in DECODES (a looping one, then a clean one: the degenerate retry).
        parseAsrResponse(g_fileAsrRepliesForTest.front(), asr);
        g_fileAsrRepliesForTest.pop_front();
        asr.promptSent = opt.prompt;
    } else if (!asrTranscribe(srv.filePort(), buildWav16k(win), asr, kFileAsrTimeoutMs, err, opt)) {
        j.asrFailed = true;            // (review P3: a file whisper that fails WITH DTW restarts without it)
        return FileStep::Transport;
    }
    windowed = true;
    ++j.st.windows;
    j.st.plans.push_back(plan);
    if (plan.why == WindowPlan::kHard) ++j.st.hardCuts;
    j.st.asrMs += asr.ms;
    CommitInput ci;
    ci.asr    = &asr;
    ci.plan   = plan;
    ci.en     = &j.energy;
    ci.prompt = asr.promptSent;
    ci.langA  = j.r.langA;           // WF, D: what is written in another script is not this job's speech
    ci.langB  = j.r.langB;
    // ---- WF, D: a window heard in a language OUTSIDE the pair is decoded AGAIN, forced into it -------
    // (measured on the film: "U+0420,0435,0434,0430,043A,0442,043E,0440 U+0441,0443,0431,0442,0438,0442,0440,043E,0432..." ru p=0.27, "U+0422,0420,0415,0412,041E,0416,041D,0410,042F U+041C,0423,0417,042B,041A,0410" p=0.45, "E ai! E ai!"
    // pt p=0.99 -- all music or whisper's training captions; whisper writes what it heard in the language
    // it is given, so a forced decode of music comes back as an annotation or nothing at all, and real
    // speech of the pair comes back as itself). One extra model call, next lap; its words are then
    // judged like any others -- a window that holds no real text in the pair language gives nothing.
    // Review P1: unless whisper is SURE of a third language and its decode holds real speech in it (a
    // Russian scene in a French film): that is kept as it was heard, in that language.
    if (j.forcedLang.empty() && pairLangOf(asr.language, j.r).empty()) {
        if (!asr.language.empty() && asr.langProb >= kThirdLangProb) {
            ci.langC = asr.language;
            if (!thirdLangSpeech(commitWindow(ci))) ci.langC.clear();
        }
        if (ci.langC.empty()) {
            j.forcedLang = plausiblePairLang(j, asr);
            ++j.st.langRedecodes;
            if (verbose) {
                // (review L8: never a UTF-8 sequence cut in two)
                size_t n = std::min<size_t>(asr.text.size(), 60);
                while (n > 0 && n < asr.text.size() && ((unsigned char)asr.text[n] & 0xC0) == 0x80) --n;
                std::printf("#   window %.2f-%.2f s heard as %s (p=%.2f): \"%s\" -> decoded again as %s\n",
                            (double)plan.s / kFileRate, (double)plan.e / kFileRate, asr.language.c_str(),
                            asr.langProb, asr.text.substr(0, n).c_str(), j.forcedLang.c_str());
            }
            j.firstAsr = asr;
            j.haveFirstAsr = true;
            j.retryPlan = plan;
            j.retryAttempt = 3;
            return FileStep::Ok;
        }
        ++j.st.thirdLang;
        if (verbose)
            std::printf("#   window %.2f-%.2f s heard as %s (p=%.2f): kept as a third language\n",
                        (double)plan.s / kFileRate, (double)plan.e / kFileRate, asr.language.c_str(), asr.langProb);
    }
    CommitVerdict cv = commitWindow(ci);
    if (verbose) {
        std::printf("#   window %.2f-%.2f s (%s%s) %u ms lang=%s p=%.2f prompt=%s: %zu word(s), "
                    "%d junk (%d tag, %d foreign), %d repeat(s)%s%s%s, next cursor %.2f s\n",
                    (double)plan.s / kFileRate, (double)plan.e / kFileRate,
                    plan.why == WindowPlan::kPause ? "pause" : plan.why == WindowPlan::kHard ? "hard"
                    : plan.why == WindowPlan::kLimit ? "limit" : "eof",
                    j.retryAttempt ? (j.retryAttempt == 3 ? ", forced" : ", retry") : "", asr.ms,
                    asr.language.c_str(), asr.langProb,
                    prompt.empty() ? "-" : promptTail(prompt, 24).c_str(), cv.words.size(),
                    cv.junkSegments, cv.tags, cv.foreign, cv.repeats, cv.degenerate ? " DEGENERATE" : "",
                    cv.timed ? " dtw" : "", cv.heldBack ? " HELD BACK" : "",
                    (double)cv.nextCursor / kFileRate);
        for (const auto& s2 : asr.segments)
            std::printf("#     seg %.2f-%.2f lp %.2f: %s\n", s2.start, s2.end, s2.avgLogprob, s2.text.c_str());
        if (std::getenv("WB_WORDS"))
            for (const auto& w2 : cv.words)
                std::printf("#       w %.2f-%.2f [%s]%s\n", w2.start, w2.end, w2.text.c_str(), w2.cut ? " CUT" : "");
    }
    // ---- a DEGENERATE decode is not accepted on the first try -----------------------------
    // (TrFileJob.h, CommitVerdict::degenerate): next lap, the same window UNPROMPTED; then, if
    // it loops again, the window cut at its largest inner pause. Each is one model call, so a
    // lap stays bounded. Only when both fail is the decode accepted -- its repeats removed.
    // (WF: a window forced into the pair -- attempt 3 -- keeps its language through these.)
    const int stage = j.retryAttempt == 1 ? 1 : (j.retryAttempt == 2 ? 2 : 0);
    if (cv.degenerate && stage < 2) {
        ++j.st.degenerate;
        if (stage == 0) {
            j.retryPlan = plan;
            j.retryAttempt = 1;
            return FileStep::Ok;
        }
        WindowPlan shorter = plan;
        if (shrinkToInnerPause(j.energy, shorter)) {
            j.retryPlan = shorter;
            j.retryAttempt = 2;
            return FileStep::Ok;
        }
    }
    // Review P1: the forced decode KEPT THE SPOKEN LANGUAGE -- written in a script outside the pair (every
    // segment of it dropped: a Russian scene whisper was unsure of), or the first decode's very words in
    // a Latin-script third language (turbo, forced into French, writes Spanish as Spanish) -- or it holds
    // NOTHING at all while the first decode, judged in its own language, is real speech (>= 4 words, 3 of
    // them different: thirdLangSpeech). MEASURED (meeting.wav's six Chinese lines, turbo forced into fr):
    // 1 kept in Chinese, 4 "translated" into French, 1 came back "Sous-titrage Societe Radio-Canada". Then
    // the FIRST decode stands, judged in its own language and labelled with it (1.0.6's route: into B).
    // (Music never qualifies: its first decodes are tags, credits, loops or "E ai! E ai!".)
    if (!j.forcedLang.empty() && j.haveFirstAsr && !j.firstAsr.language.empty() && j.retryAttempt != 2) {
        const bool lostToScript = cv.words.empty() && cv.foreign > 0;
        const bool lostAll      = cv.words.empty();
        const bool keptLatin = !cv.words.empty() && sameDecode(asr, j.firstAsr)
                            && latinScriptLang(j.firstAsr.language) && !foreignScript(j.firstAsr.text, "en", "en");
        if (lostAll || keptLatin) {
            CommitInput c2 = ci;
            c2.asr    = &j.firstAsr;
            c2.prompt = j.firstAsr.promptSent;
            c2.langC  = j.firstAsr.language;
            CommitVerdict v2 = commitWindow(c2);
            const bool words = !v2.degenerate && std::any_of(v2.words.begin(), v2.words.end(),
                                                             [](const FileWord& w) { return hasLetterOrDigit(w.text); });
            if (lostToScript || keptLatin ? words : thirdLangSpeech(v2)) {
                if (verbose)
                    std::printf("#     forced decode kept the spoken language (%s): the first decode (%s) stands\n",
                                lostToScript ? "another script" : keptLatin ? "the same words" : "nothing left",
                                j.firstAsr.language.c_str());
                cv = std::move(v2);
                ++j.st.langFallback;
            }
        }
    }
    if (!j.forcedLang.empty() && cv.words.empty()) ++j.st.langEmpty;
    j.retryAttempt = 0;
    j.forcedLang.clear();
    j.haveFirstAsr = false;
    j.st.junkSegments += (uint32_t)(cv.junkSegments + cv.repeats);
    j.st.foreignSegs  += (uint32_t)cv.foreign;
    j.st.tagSegs      += (uint32_t)cv.tags;
    // review P7: the windows that committed real words, and those of them timed by DTW -- the same set
    if (std::any_of(cv.words.begin(), cv.words.end(), [](const FileWord& w) { return hasLetterOrDigit(w.text); })) {
        ++j.st.wordWindows;
        if (cv.timed) ++j.st.timedWindows;
    }
    if (cv.heldBack) ++j.st.heldBack;
    if (j.lastHard) dropRepeatedHead(cv.words, j.lastTail);
    // WF, B: the SAFETY de-dup -- words already committed, heard (or decoded) a second time
    if (const size_t dd = dropAlreadyCommitted(cv.words, j.recentWords)) {
        j.st.dedupWords += (uint32_t)dd;
        if (verbose) std::printf("#     de-dup: %zu word(s) already committed, dropped\n", dd);
    }
    j.cues.addWords(cv.words);
    // WF: what was committed -- the last ~30 s of words (B) and the file's language count (D)
    for (const auto& w : cv.words) {
        j.recentWords.push_back(w);
        if (hasLetterOrDigit(w.text)) {
            const std::string pl = pairLangOf(w.lang, j.r);
            if (!pl.empty()) ++j.langWords[pl];
        }
    }
    while (!j.recentWords.empty()
           && (j.recentWords.size() > 400 || j.recentWords.front().end < j.recentWords.back().end - 40.0))
        j.recentWords.pop_front();
    if (!cv.words.empty()) {
        j.lastWordEnd = cv.words.back().end;
        j.lastTail.insert(j.lastTail.end(), cv.words.begin(), cv.words.end());
        if (j.lastTail.size() > 12) j.lastTail.erase(j.lastTail.begin(), j.lastTail.end() - 12);
    }
    if (plan.why == WindowPlan::kPause && j.lastWordEnd >= 0.0) {
        // How much silence follows the last word: the next ACTIVE hop after the cut.
        size_t h = EnergyTrack::hopAt((double)plan.e / kFileRate);
        while (h < j.energy.hops() && !j.energy.active(h)) ++h;
        const double nextOn = h < j.energy.hops() ? EnergyTrack::timeOf(h)
                                                 : (double)plan.e / kFileRate + 10.0;
        j.cues.windowBoundary(nextOn - j.lastWordEnd);
    }
    j.dropTail = cv.sawJunk;
    // (review L1: a hard cut that committed nothing -- held back, or nothing heard -- is not re-heard by the
    // next window: its head is never cut against the words before it)
    j.lastHard = plan.why == WindowPlan::kHard && !cv.heldBack && !cv.words.empty();
    if (cv.nextCursor > j.cursor) j.cursor = cv.nextCursor;
    j.first = false;
    j.runFirst = false;
    FileCue c;
    while (j.cues.pop(c)) j.toTranslate.push_back(c);
    trimFileBuf(j);
    return FileStep::Ok;
}

// ONE bounded step of the job (see the section header). The job is gone (FDONE sent) when
// `job` comes back empty. `abort` interrupts a model load; the caller turns an aborted load
// into the right FDONE (cancelled) when it processes the request that raised it.
void fileJobLap(std::unique_ptr<FileJob>& job, Servers& srv, FileSink& sink,
                const std::atomic<uint64_t>& abortJob, bool verbose, FileJobStats* stats) {
    FileJob& j = *job;
    std::string err;
    switch (j.phase) {
    case FileJob::Phase::Open: {
        // WH: THE SOUND TRACK the FILE names -- found, decoded and kept (seeks re-open it) by the reader;
        // one it cannot find or decode is this job's one FDONE error, its sentence naming the format.
        if (!j.reader.open(j.path, MediaReader::Out::Mono16k, j.audio, err)) {
            finishJob(job, srv, sink, "error", err, stats);
            return;
        }
        {
            const MediaInfo& mi = j.reader.info();
            char t[200];
            std::snprintf(t, sizeof(t), "ord %d / #%u, stream %u of %u, %s %u Hz %u ch, lang \"%s\", by %s",
                          mi.trackOrd, mi.trackId, mi.streamIndex, mi.audioStreams, mi.codec.c_str(),
                          mi.nativeRate, mi.nativeChannels, mi.trackLang.c_str(), mi.trackHow.c_str());
            j.st.track        = t;
            j.st.trackOrd     = mi.trackOrd;
            j.st.trackId      = mi.trackId;
            j.st.trackStream  = mi.streamIndex;
            j.st.trackStreams = mi.audioStreams;
            j.st.trackLang    = mi.trackLang;
            j.st.trackHow     = mi.trackHow;
            std::string line = "[SuVtitleEngine] FILE job " + std::to_string(j.req.jobId) + ": sound track "
                             + (j.req.audio.empty() ? std::string("\"\" (the first)") : "\"" + j.req.audio + "\"")
                             + " -> " + t;
            // the container's own id won over a place the tray counted differently (TrMedia.cpp: the id is exact)
            if (j.audio.chosen && mi.trackOrd >= 0 && (uint32_t)mi.trackOrd != j.audio.ord)
                line += " (NOTE: the tray counted it as ord " + std::to_string(j.audio.ord) + ")";
            engineLog(line + "\n");           // SUVTITLE: + engine.log (host mode)
        }
        j.totalMs  = g_fileNoDurationForTest ? 0 : j.reader.info().durationMs;
        j.hasVideo = j.reader.info().hasVideo;
        j.fileEnd  = msToSamples(j.totalMs);
        if (!chooseFileModels(j, srv, err)) {
            finishJob(job, srv, sink, "error", err, stats);
            return;
        }
        j.opened = true;
        CueCfg cc;
        cc.hanPair = isHanLang(j.r.langA) || isHanLang(j.r.langB);
        j.energy.reset();
        j.cues.configure(cc, &j.energy);
        // PROBED: the duration and the picture go out NOW, before a 3 GB model load (WC) -- with
        // the ranges the tray already holds (v2).
        sendProg(j, sink, "loading");
        // v2, WE's Q2: a FILE whose `have` already covers the whole file (to the gaps no subtitle
        // fits in) is done -- FDONE ok at once, no model loaded.
        if (j.fileEnd) {
            bool gap = false;
            for (uint64_t p = 0; !gap;) {
                const std::vector<trpipe::MsRange> a = analysedSet(j, false);
                p = firstGapAtOrAfter(a, p);
                if (p >= j.fileEnd) break;
                uint64_t gs = 0, ge = 0;
                gapAround(j, a, p, gs, ge);
                if (ge != kNoLimit && ge - gs < kMinGapSamples) {
                    j.done.push_back({ gs, ge });
                    trpipe::mergeRanges(j.done);
                    p = ge;
                } else {
                    gap = true;
                }
            }
            if (!gap) {
                sendProg(j, sink, "analyzing");
                finishJob(job, srv, sink, "ok", std::string(), stats);
                return;
            }
        }
        j.phase  = FileJob::Phase::Load;
        j.loadT0 = ::GetTickCount64();
        return;
    }
    case FileJob::Phase::Load: {
        ++j.st.loads;
        if (!g_fileNoModelsForTest) {
            // Stopped BY ID (Shared::abortJob): only an FCANCEL / superseding FILE for THIS job, or QUIT.
            const uint64_t me = j.req.jobId;
            const std::function<bool()> aborted = [&abortJob, me] {
                const uint64_t v = abortJob.load(std::memory_order_acquire);
                return v == me || v == kAbortAll;
            };
            if (!srv.bringFile(j.asrModel, j.mtModel, verbose, aborted, !j.noDtw)) {
                if (aborted()) return;                     // the request handler ends it
                finishJob(job, srv, sink, "error", srv.lastError(), stats);
                return;
            }
            j.mtModel = srv.mtModel();         // what actually loaded (the 1.5B if the 4B is absent)
        }
        j.st.loadMs += ::GetTickCount64() - j.loadT0;
        if (j.r.mtStyle.empty()) j.r.mtStyle = mtStyleFor(j.mtModel);
        j.phase = FileJob::Phase::Run;
        sendProg(j, sink, "analyzing");
        return;
    }
    case FileJob::Phase::Run: {
        // BACK-PRESSURE, never a drop: past the soft cap the job simply waits for the tray.
        if (sink.backlog() >= kFileOutSoftCap) { ::Sleep(20); return; }
        applyFocus(j, sink);                  // the playhead, at a step boundary (TrPipe.h: FFOCUS)
        FileStep r = FileStep::Ok;
        bool windowed = false;
        if (!j.toTranslate.empty()) {
            r = translateOneCue(j, srv, sink, err);
        } else if (j.runActive && !j.runDraining) {
            bool done = false;
            r = fileWindowStep(j, srv, err, done, verbose, windowed);
            if (r == FileStep::Ok && done) {
                // THE RUN'S END: its limit -- or the end of the file, which is then known -- or, the
                // file broken off, where its audio ended, its limit or not (review C2: a limited run
                // claimed the stretch it never heard up to its limit, and a resume skipped it for
                // ever). Every cue closes (none runs past the end), then translates.
                const uint64_t decoded = j.bufStart + j.buf.size();
                if (!j.failErr.empty()) {
                    j.runEnd = j.runLimit != kNoLimit ? std::min<uint64_t>(j.runLimit, decoded) : decoded;
                    if (j.runLimit == kNoLimit) j.fileEnd = decoded;
                } else if (j.runLimit != kNoLimit) {
                    j.runEnd = j.runLimit;
                } else {
                    j.fileEnd = std::max<uint64_t>(msToSamples(j.totalMs), decoded);
                    j.runEnd = j.fileEnd;
                }
                j.cues.finish((double)j.runEnd / kFileRate);
                FileCue c;
                while (j.cues.pop(c)) j.toTranslate.push_back(c);
                j.runDraining = true;
            }
        } else if (j.runActive) {
            // every cue of the run sent: its stretch is analysed
            closeRun(j);
            sendProg(j, sink, "analyzing");
            if (!j.failErr.empty()) j.phase = FileJob::Phase::Drain;
            return;
        } else {
            // BETWEEN RUNS: the next one -- once a jump's playhead has settled (a scrub is many FFOCUS)
            if (j.runPending && fileNowMs() - j.focusTick < kFocusSettleMs) { ::Sleep(20); return; }
            if (!j.failErr.empty()) { j.phase = FileJob::Phase::Drain; return; }
            // The seek there is ABORTABLE (review P3): FCANCEL / a superseding FILE / QUIT for this job,
            // or a new playhead far from the one this run is opened for (its target is moot) -- polled
            // between two decoded buffers, so a decode-to-seek never holds them for seconds.
            const uint64_t me = j.req.jobId;
            const FileJob* jp = &j;
            const std::function<bool()> seekAbort = [&abortJob, me, jp] {
                const uint64_t v = abortJob.load(std::memory_order_acquire);
                if (v == me || v == kAbortAll) return true;
                if (!jp->focusSeqLive || jp->focusSeqLive->load(std::memory_order_acquire) == jp->focusSeqSeen)
                    return false;
                if (!jp->focusJobLive || jp->focusJobLive->load(std::memory_order_acquire) != me) return false;
                const uint64_t ms = jp->focusMsLive->load(std::memory_order_acquire);
                const uint64_t d  = ms > jp->focusMs ? ms - jp->focusMs : jp->focusMs - ms;
                return d > (uint64_t)(kFocusReachSec * 1000.0);
            };
            const NextRun nr = startNextRun(j, seekAbort);
            if (nr == NextRun::Aborted) { ++j.st.seekAborts; return; }   // the next lap decides again
            j.runPending = false;
            if (nr == NextRun::None) j.phase = FileJob::Phase::Drain;
            return;
        }
        if (r == FileStep::Transport) {
            // Review P3: the WINDOW's whisper failed while it ran with DTW (flash attention off: more
            // memory, a code path the live children never take) -- the job goes on without it rather
            // than spend its retries on the same failure (the old token times, as before WF).
            if (j.asrFailed && srv.fileDtw() && !j.noDtw) {
                j.noDtw = true;
                ++j.st.dtwDropped;
                if (verbose) std::printf("#   the file whisper failed with DTW (%s): restarted without it\n", err.c_str());
            }
            j.asrFailed = false;
            if (j.retries++ < kFileRetries) {
                // A server died or wedged: rebuild both and redo the same step (nothing was
                // consumed -- the window was not committed, the cue not popped).
                srv.killAll();
                j.phase  = FileJob::Phase::Load;
                j.loadT0 = ::GetTickCount64();
                return;
            }
            const double at = (double)j.cursor / kFileRate;
            char where[32];
            std::snprintf(where, sizeof(where), "%d:%02d", (int)at / 60, (int)at % 60);
            finishJob(job, srv, sink, "error",
                      "The speech engine stopped answering at " + std::string(where) + " (" + err + ").",
                      stats);
            return;
        }
        if (r == FileStep::Fatal) {
            finishJob(job, srv, sink, "error", err, stats);
            return;
        }
        // WE's Q4: one FPROG per window at least (totalMs known or not); else v1's cadence.
        if (windowed) sendProg(j, sink, "analyzing");
        else          maybeSendProg(j, sink);
        return;
    }
    case FileJob::Phase::Drain: {
        // DONE. The last FPROG is everything analysed -- the whole file (or, after a decode error,
        // as far as the audio went) -- then the one FDONE: "error" with the reader's sentence when
        // the file broke off, but only AFTER every cue heard before the break went out (WB, review).
        if (!j.totalMs && j.fileEnd) j.totalMs = (j.fileEnd + kFileRate / 1000 - 1) / (kFileRate / 1000);
        sendProg(j, sink, "analyzing");
        if (j.failErr.empty()) finishJob(job, srv, sink, "ok", std::string(), stats);
        else                   finishJob(job, srv, sink, "error", j.failErr, stats);
        return;
    }
    }
}

// The requests the servant queued: superseding FILEs, FCANCELs. Run at the top of every
// pipeline lap (and by the drivers below). Order matters and is the wire's: a FILE then an
// FCANCEL for it in the same lap is a job started and cancelled, i.e. one FDONE "cancelled".
// v2: and the playhead (Shared::focus*), latest wins.
void fileRequests(Shared& sh, std::unique_ptr<FileJob>& job, Servers& srv, FileSink& sink,
                  const TrConfig& cfg, FileJobStats* stats) {
    std::deque<FileReq> reqs;
    std::vector<uint64_t> cancels;
    std::string curA, curB, curMode;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        reqs.swap(sh.fileReqs);
        cancels.swap(sh.fileCancels);
        curA = sh.langA; curB = sh.langB; curMode = sh.mode;
        // WD: the job about to be made is "running" from this instant: an FFOCUS for it landing while
        // this lap builds it must not be taken for a stale one (refused, it is reset below).
        if (!reqs.empty()) sh.runningJob = reqs.back().f.jobId;
    }
    while (!reqs.empty()) {
        const FileReq q = reqs.front();
        reqs.pop_front();
        if (job) finishJob(job, srv, sink, "cancelled", std::string(), stats);
        if (!reqs.empty()) {
            // Superseded before it could start: still EXACTLY one FDONE for it -- "error" with its
            // reason when it could not have started anyway (WD: a malformed FILE says so).
            trpipe::WireFileDone d;
            d.jobId = q.f.jobId; d.count = 0;
            d.result = q.refuse.empty() ? "cancelled" : "error";
            d.err = q.refuse;
            sink.push(trpipe::packFileDone(d));
            continue;
        }
        std::string err = q.refuse;
        if (err.empty() && makeFileJob(q.f, curA, curB, curMode, cfg, job, err)) continue;
        trpipe::WireFileDone d;
        d.jobId = q.f.jobId; d.result = "error"; d.count = 0; d.err = err;
        sink.push(trpipe::packFileDone(d));
    }
    for (uint64_t id : cancels)
        if (job && job->req.jobId == id) finishJob(job, srv, sink, "cancelled", std::string(), stats);
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.runningJob = job ? job->req.jobId : 0;
        if (job) {
            job->focusSeqLive = &sh.focusSeqLive;
            job->focusMsLive  = &sh.focusMsLive;
            job->focusJobLive = &sh.focusJobLive;
        }
        if (job && sh.focusJob == job->req.jobId && sh.focusSeq != job->focusSeqSeen) {
            job->focusSeqSeen = sh.focusSeq;
            // a SEEK among the FFOCUS since the last one taken -- or in one taken but not yet applied
            // (the job was loading) -- is not forgotten by a playback tick after it (C1)
            job->focusNewJump = (job->focusNew && job->focusNewJump) || sh.focusJump;
            sh.focusJump      = false;
            job->focusNew     = true;
            job->focusNewMs   = sh.focusMs;
            job->focusTick    = sh.focusTick;
        }
    }
}

// Every FILE still waiting in the queue gets its FDONE (QUIT, see pipelineMain's end): "cancelled" --
// or "error" with its reason for one that was refused at the door (as the supersede path answers it).
void cancelQueuedFiles(Shared& sh, FileSink& sink) {
    std::deque<FileReq> left;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        left.swap(sh.fileReqs);
        sh.fileCancels.clear();
    }
    for (const auto& q : left) {
        trpipe::WireFileDone d;
        d.jobId = q.f.jobId; d.count = 0;
        d.result = q.refuse.empty() ? "cancelled" : "error";
        d.err = q.refuse;
        sink.push(trpipe::packFileDone(d));
    }
}

// ---------------------------------------------------------------------------
// THE PIPELINE THREAD.
// SUVTITLE SPLIT: AudioPB's pipelineMain minus its live half. Kept as it was: the persistence of the pair /
// direction / preset (re-read first), the STATE echo of a CTL, the FILE job's laps, the clean exit (every
// running and queued FILE gets its one FDONE). Gone: TrAudio (no fabric), the live pair kept up at idle
// (ensureUp + kRespawnBackoff), the segments, the drafts, the commit engine, diarization, LINE / PART / END
// -- and with them the END that closed an utterance a job interrupted, and the live audio a job dropped.
// IDLE IS IDLE: nothing is loaded until a FILE comes; then bringFile loads the job's own models, as it did
// in AudioPB (minus killing a live pair to make room). The MT a job loaded stays up after FDONE, as it did:
// the next job -- often the next film of the queue -- skips its 3 GB load; endFile frees the job's whisper.
// STATE.status, here: "listening" = idle and READY (TrPipe.h is unchanged: the word stays, nothing listens);
// "analyzing" = a job owns the lap; "error" = the payload is missing (err = the sentence a job would fail
// with); "loading" only until the first lap. Never "transcribing" (that was a live final).
// ---------------------------------------------------------------------------
// The idle lap's pace: what AudioPB's idle laps blocked on (TrAudio::pop with kPopDeadlineMs, 20 ms) -- a FILE
// waits at most this long to start, against a model load measured in seconds.
constexpr DWORD kIdleLapMs = 20;

// STATE's model fields (SUVTITLE): what a FILE job runs on -- before the first one, what it WOULD load
// (AudioPB announced "the models we are ABOUT to load" the same way), then what the running job did load,
// and on which device (gpu = its whisper AND its MT came up on the GPU).
void stateModels(Shared& sh, const std::string& asr, const std::string& mt, bool gpu) {
    bool moved = false;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        if (sh.state.asrModel != asr || sh.state.mtModel != mt || sh.state.gpu != gpu) {
            sh.state.asrModel = asr;
            sh.state.mtModel  = mt;
            sh.state.gpu      = gpu;
            sh.stateDirty     = true;
            moved = true;
        }
    }
    if (moved) wakeServant(sh);
}

void pipelineMain(Shared& sh, Servers& srv, TrConfig cfg, bool persist = true) {
    // ---- FILE MODE (wave WB) ------------------------------------------------------
    std::unique_ptr<FileJob> fileJob;
    SharedFileSink           fileSink(sh);
    srv.setPreset(cfg.preset);
    {   // what a job WOULD run on at quality auto: chooseFileModels' auto branch, makeFileJob's MT
        const std::string asr = srv.modelInstalled("ggml-large-v3-turbo.bin") ? std::string("ggml-large-v3-turbo.bin")
                                                                                : srv.presetFinalModel();
        stateModels(sh, asr, cfg.mtModel.empty() ? std::string(kMtModelFile) : cfg.mtModel, !g_forceCpu);
    }

    for (;;) {
        if (sh.quit.load(std::memory_order_acquire)) break;

        std::string mode, preset, langA, langB;
        bool presetChanged = false;
        {
            std::lock_guard<std::mutex> lk(sh.mx);
            mode = sh.mode; preset = sh.preset;
            langA = sh.langA; langB = sh.langB;
            presetChanged = sh.presetChanged; sh.presetChanged = false;
        }
        // Persist mode/preset/pair -- with a RE-READ first, so a line this engine does not own (kept verbatim
        // in `extra`) is never lost to a stale in-memory copy. (In AudioPB the tray wrote its window geometry
        // into the same file; SuVtitle's app writes nothing there, S11 -- but a hand edit may.)
        if (cfg.mode != mode || cfg.langA != langA || cfg.langB != langB
            || (presetChanged && cfg.preset != preset)) {
            // WB: `persist` is false under --nofabric -- a TEST engine must never write the user's
            // engine.txt.
            if (persist) {
                TrConfig fresh;
                fresh.load();
                fresh.mode   = mode;
                fresh.preset = preset;
                fresh.langA  = langA;
                fresh.langB  = langB;
                fresh.save();
            }
            cfg.mode   = mode;
            cfg.preset = preset;
            cfg.langA  = langA;
            cfg.langB  = langB;
        }
        if (presetChanged) srv.setPreset(preset);       // the next job's fallback model (presetFinalModel)
        {
            bool moved = false;
            {
                std::lock_guard<std::mutex> lk(sh.mx);
                if (sh.state.mode != mode) { sh.state.mode = mode; sh.stateDirty = true; moved = true; }
                if (sh.state.langA != langA) { sh.state.langA = langA;
                                               sh.stateDirty = true; moved = true; }
                if (sh.state.langB != langB) { sh.state.langB = langB;
                                               sh.stateDirty = true; moved = true; }
                // SUVTITLE: the preset is echoed as CONFIGURED -- AudioPB echoed the live preset its pair was
                // resolved to (`auto` -> the probe's verdict), and there is no pair to resolve.
                if (sh.state.preset != preset) { sh.state.preset = preset; sh.stateDirty = true; moved = true; }
            }
            if (moved) wakeServant(sh);   // the echo of a CTL: the menu tick should not lag a poll
        }

        // ---- FILE MODE (wave WB): a job, when there is one, owns the lap ------------------
        fileRequests(sh, fileJob, srv, fileSink, cfg, nullptr);
        if (fileJob) {
            // (SUVTITLE SPLIT: AudioPB paused the live path here, every lap -- its fabric audio cleared, its
            // live line and agreement reset, an END for an utterance the job cut short. Nothing to pause.)
            setState(sh, "analyzing");
            fileJobLap(fileJob, srv, fileSink, sh.abortJob, /*verbose=*/false, nullptr);
            if (fileJob && fileJob->phase == FileJob::Phase::Run)     // its models are up: STATE says which
                stateModels(sh, fileJob->asrModel, fileJob->mtModel, srv.fileGpu() && srv.mtGpu());
            if (!fileJob) {
                std::lock_guard<std::mutex> lk(sh.mx);
                sh.runningJob = 0;
            }
            continue;
        }
        // ---- IDLE. Also the way OUT of a job, whatever ended it -- before its Load (an unreadable file,
        // "max" without its model, a cancel or a supersede in its first lap) or after: "analyzing" must never
        // outlive the job (WB, after the review). setState writes only a CHANGE, so this costs a compare.
        if (srv.payloadFound()) setState(sh, "listening");
        else                    setState(sh, "error", srv.payloadSentence());
        ::Sleep(kIdleLapMs);
    }
    // QUIT / a broken pipe with a job running: its one FDONE ("cancelled") still goes out --
    // runHost drains the file outbox one last time after this thread is joined.
    if (fileJob) finishJob(fileJob, srv, fileSink, "cancelled", std::string(), nullptr);
    // ...and so does every FILE still QUEUED, never started (WB, after the review): EXACTLY one
    // FDONE per FILE holds on the way out too, or the tray's player waits for ever.
    cancelQueuedFiles(sh, fileSink);
    srv.killAll();
}

// ---------------------------------------------------------------------------
// FPROXY (wave WB): ONE worker thread, CPU only, independent of the pipeline. A new FPROXY
// SUPERSEDES the running one (its FPROXYDONE says "superseded"): there is one player and one
// proxy, a queue would make the user wait for a file already closed, and a "busy" answer would
// push retry logic onto the tray. FCANCEL on a proxy's id cancels it ("cancelled"). Exactly one
// FPROXYDONE per FPROXY, QUIT included.
// ---------------------------------------------------------------------------
class ProxyWorker {
public:
    explicit ProxyWorker(FileSink& sink) : sink_(sink) {}
    ~ProxyWorker() { stop(); }
    void start() { th_ = std::thread([this] { run(); }); }
    void request(const trpipe::WireProxy& p) {
        std::lock_guard<std::mutex> lk(mx_);
        // Anything still queued never started: answer it now, superseded.
        for (const auto& q : queue_) done(q, false, "superseded");
        queue_.clear();
        if (curId_) { cancel_.store(true); curWhy_ = "superseded"; }
        queue_.push_back(p);
        cv_.notify_all();
    }
    void cancel(uint64_t id) {
        std::lock_guard<std::mutex> lk(mx_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (it->jobId == id) { done(*it, false, "cancelled"); queue_.erase(it); return; }
        if (curId_ == id) { cancel_.store(true); curWhy_ = "cancelled"; }
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (stop_) return;
            stop_ = true;
            for (const auto& q : queue_) done(q, false, "cancelled");
            queue_.clear();
            if (curId_) { cancel_.store(true); curWhy_ = "cancelled"; }
            cv_.notify_all();
        }
        if (th_.joinable()) th_.join();
    }
private:
    void done(const trpipe::WireProxy& p, bool ok, const std::string& err) {
        trpipe::WireProxyDone d;
        d.jobId = p.jobId;
        d.ok    = ok;
        d.out   = p.out;
        d.err   = err;
        sink_.push(trpipe::packProxyDone(d));
    }
    void run() {
        for (;;) {
            trpipe::WireProxy p;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                p = queue_.front();
                queue_.pop_front();
                curId_ = p.jobId;
                curWhy_.clear();
                cancel_.store(false);
            }
            std::string err;
            const ProxyResult r = writeProxyWav(utf8ToWideL(p.src), utf8ToWideL(p.out), cancel_, err);
            std::lock_guard<std::mutex> lk(mx_);
            if (r == ProxyResult::Ok)             done(p, true, std::string());
            else if (r == ProxyResult::Cancelled) done(p, false, curWhy_.empty() ? "cancelled" : curWhy_);
            else                                  done(p, false, err);
            curId_ = 0;
        }
    }
    FileSink&                     sink_;
    std::thread                   th_;
    std::mutex                    mx_;
    std::condition_variable       cv_;
    std::deque<trpipe::WireProxy> queue_;
    uint64_t                      curId_ = 0;
    std::string                   curWhy_;
    std::atomic<bool>             cancel_{ false };
    bool                          stop_ = false;
};

// ---------------------------------------------------------------------------
// ONE message from the tray, as the servant handles it (WD: out of runHost, so the selftest drives
// the very same code with no pipe). false = QUIT. `proxy` may be null (the selftest: FPROXY ignored).
// ---------------------------------------------------------------------------
bool servantHandle(Shared& sh, ProxyWorker* proxy, const std::vector<std::string>& t) {
    if (t.empty()) return true;
    if (t[0] == "QUIT") return false;
    trpipe::WireCtl c;
    trpipe::WireFile wf;
    trpipe::WireProxy wp;
    uint64_t cancelId = 0, focusId = 0, focusMs = 0;
    if (trpipe::unpackFile(t, wf)) {
        // FILE MODE (WB). Queued for the pipeline; a job already running is told to stop
        // at once (its model load included) -- its FDONE "cancelled" goes first.
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.fileReqs.push_back(FileReq(wf));
        if (sh.runningJob) sh.abortJob.store(sh.runningJob, std::memory_order_release);
        // the playhead's chain starts at the FILE's focus (review C1: the first tick follows it)
        sh.chainJob  = wf.jobId;
        sh.chainMs   = wf.focusMs;
        sh.chainTick = fileNowMs();
        sh.focusJump = false;
    } else if (t[0] == "FILE" && t.size() >= 2 && trpipe::jobIdOf(t[1]) != 0) {
        // WD: a FILE that names a job but cannot be read -- a `have` that does not parse, no path.
        // Refused, but never silently: like any FILE it stops the running job, and it gets its ONE
        // FDONE ("error") in the order the pipeline answers FILEs (TrPipe.h: exactly one per FILE).
        trpipe::WireFile bad;
        bad.jobId = trpipe::jobIdOf(t[1]);
        bad.path  = t.size() >= 3 ? trpipe::unesc(t[2]) : std::string();
        // WH: or a sound track that does not parse ("x", "1:", ":2", "1:2:3") -- the same refusal, its own
        // sentence (unpackFile checks `have` first, so a bad `have` keeps the sentence it always had)
        std::vector<trpipe::MsRange> chk;
        const bool haveOk = trpipe::parseRanges(t.size() >= 8 ? trpipe::unesc(t[7]) : std::string(), chk);
        const std::string why = bad.path.empty() ? std::string("The request names no file.")
            : !haveOk ? std::string("The request's list of already-subtitled stretches could not be read.")
            : badAudioSentence(t.size() >= 10 ? trpipe::unesc(t[9]) : std::string());
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.fileReqs.push_back(FileReq(bad, why));
        if (sh.runningJob) sh.abortJob.store(sh.runningJob, std::memory_order_release);
    } else if (trpipe::unpackFileFocus(t, focusId, focusMs)) {
        // WD: THE PLAYHEAD. For the running job or a queued one -- latest wins, one slot -- and a
        // queued FILE starts there; any other id is a finished or unknown job: dropped, silently.
        std::lock_guard<std::mutex> lk(sh.mx);
        bool known = sh.runningJob == focusId;
        for (auto& q : sh.fileReqs)
            if (q.f.jobId == focusId && q.refuse.empty()) { q.f.focusMs = focusMs; known = true; }
        if (known) {
            const uint64_t now = fileNowMs();
            // review C1: did it follow the one before it like PLAYBACK (kFocusChainGapMs, kFocusPlayRate)?
            // Judged here, on every FFOCUS -- the job only sees the last of a burst.
            const uint64_t dt = now - sh.chainTick;
            const bool played = sh.chainJob == focusId && dt <= kFocusChainGapMs
                             && focusMs + 1000 >= sh.chainMs
                             && focusMs <= sh.chainMs + (uint64_t)(kFocusPlayRate * (double)dt) + kFocusPlaySlackMs;
            if (!played) sh.focusJump = true;
            sh.chainJob  = focusId;
            sh.chainMs   = focusMs;
            sh.chainTick = now;
            sh.focusJob  = focusId;
            sh.focusMs   = focusMs;
            sh.focusTick = now;
            ++sh.focusSeq;
            sh.focusJobLive.store(focusId, std::memory_order_relaxed);
            sh.focusMsLive.store(focusMs, std::memory_order_relaxed);
            sh.focusSeqLive.store(sh.focusSeq, std::memory_order_release);   // last: it publishes the two
        }
    } else if (trpipe::unpackFileCancel(t, cancelId)) {
        // ONE id counter on the tray's side (TrPipe.h): the id names a job OR a proxy.
        {
            std::lock_guard<std::mutex> lk(sh.mx);
            sh.fileCancels.push_back(cancelId);
            if (sh.runningJob == cancelId) sh.abortJob.store(cancelId, std::memory_order_release);
        }
        if (proxy) proxy->cancel(cancelId);
    } else if (trpipe::unpackProxy(t, wp)) {
        if (proxy) proxy->request(wp);
    } else if (trpipe::unpackCtl(t, c)) {
        // The pair is VALIDATED here, at the door. A code the helper cannot honour
        // must never reach the pipeline (it would route every line into a language
        // no model has heard of); rejecting it silently means STATE keeps echoing
        // the pair that IS running, which is how the tray finds out it was refused.
        // (WA: "zh-tw" and its aliases pass, as zh-tw -- normalizeLangCode is the
        // door's whole vocabulary, so there is no second rule here to forget.)
        const std::string a = normalizeLangCode(c.langA);
        const std::string b = normalizeLangCode(c.langB);
        std::lock_guard<std::mutex> lk(sh.mx);
        if (!c.mode.empty()   && c.mode != sh.mode)     { sh.mode = c.mode; sh.stateDirty = true; }
        if (!c.preset.empty() && c.preset != sh.preset) { sh.preset = c.preset;
                                                          sh.presetChanged = true;
                                                          sh.stateDirty = true; }
        // Both halves are applied together or not at all: a pair is a pair, and
        // half of a swap (en/zh -> zh/zh) is a session where nothing is translated.
        const std::string wantA = a.empty() ? sh.langA : a;
        const std::string wantB = b.empty() ? sh.langB : b;
        if (wantA != wantB && (wantA != sh.langA || wantB != sh.langB)) {
            sh.langA = wantA;
            sh.langB = wantB;
            sh.stateDirty = true;
        }
        // (SUVTITLE SPLIT: `clear` -- "drop queued segments now" -- raised sh.clearAsked for the live queue. A
        // file-only engine queues no segment: ignored, as TrPipe.h lets a reader do with what it cannot use.)
    }
    return true;
}

// ---------------------------------------------------------------------------
// The engine's config at start -- host mode, and --selftest [0s], which drives this very function in an isolated
// APPDATA (E8, director 23/10). engine.txt; on the FIRST start (engine.txt absent) seeded from AudioPB's
// translator.txt (seedFromAudioPB: the pair the imported analyses are filed under); the command line on top; then
// materialised -- so the next start is not a first one, whatever AudioPB's file says by then. A TEST engine
// (--nofabric) reads its engine.txt as AudioPB's test helper read translator.txt, and seeds and writes NOTHING.
// `seedWhy` (optional): the seed's one line for the log on a first start, "" otherwise.
// ---------------------------------------------------------------------------
TrConfig startConfig(bool testEngine, const std::string& presetArg, const std::string& modeArg,
                     std::string* seedWhy = nullptr) {
    TrConfig cfg;
    const std::wstring own = TrConfig::path();
    const bool first = !testEngine && !own.empty() && ::GetFileAttributesW(own.c_str()) == INVALID_FILE_ATTRIBUTES;
    cfg.load();
    std::string why;
    if (first) seedFromAudioPB(cfg, why);         // all or nothing; never fatal
    if (seedWhy) *seedWhy = why;
    if (!presetArg.empty()) cfg.preset = presetArg;
    if (!modeArg.empty())   cfg.mode   = modeArg;
    if (!testEngine) cfg.save();                  // materialise the file on a first run
    return cfg;
}

// ---------------------------------------------------------------------------
// Host mode: the pipe servant.
// SUVTITLE SPLIT: AudioPB's runHost minus the fabric -- no TrAudio to start (it claimed the card's slot, and
// the helper exited 2 when it could not), no live outbox to drain (LINE / PART / END, each LINE's waitMs
// stamped at send). New here: the log file (host mode only), the first-start seed (E8), DEVICE cpu, HELLO
// "engine", --rate ignored.
// ---------------------------------------------------------------------------
int runHost(const std::string& pipePath, uint32_t parentPid,
            const std::string& presetArg, const std::string& modeArg, bool nofabric) {
    watchParent(parentPid);

    // /!\ --nofabric (WB, a TEST engine driven by a test pipe client) writes NOTHING of the user's: not engine.txt
    // (its materialisation, the first-start seed, the pipeline's persistence of a CTL), not the log. (SUVTITLE:
    // there is no fabric to stay off any more; the switch keeps its other meaning -- "this is a test engine" -- for
    // the test kits.)
    if (!nofabric) openEngineLog();
    std::string seedWhy;
    const TrConfig cfg = startConfig(nofabric, presetArg, modeArg, &seedWhy);
    // S11, DEVICE cpu: every child on the CPU and the GPU never opened -- the test switch's own mechanism.
    if (cfg.device == "cpu") forceCpuChildren();
    const TrPaths paths = resolvePaths(cfg);
    engineLog(std::string("[SuVtitleEngine] ") + SUVTITLE_ENGINE_VERSION_STR + " start: pid "
              + std::to_string((unsigned long)::GetCurrentProcessId()) + ", parent " + std::to_string((unsigned long)parentPid)
              + ", preset " + cfg.preset + ", mode " + cfg.mode + ", pair " + cfg.langA + "/" + cfg.langB
              + ", device " + cfg.device + (g_forceCpu ? " (every child on the CPU)" : "") + "\n");
    if (!seedWhy.empty()) engineLog("[SuVtitleEngine] first start (no engine.txt): " + seedWhy + "\n");
    engineLog("[SuVtitleEngine] payload: whisper "
              + (paths.whisperExe.empty() ? std::string("NOT FOUND") : wideToUtf8L(paths.whisperExe))
              + " | llama " + (paths.llamaExe.empty() ? std::string("NOT FOUND") : wideToUtf8L(paths.llamaExe))
              + " | models " + (paths.modelsDir.empty() ? std::string("NOT FOUND") : wideToUtf8L(paths.modelsDir))
              + (paths.devPayload ? " (the dev payload named by SUVTITLE_PAYLOAD: nothing staged next to the exe)" : " (the bundle)")
              + (paths.missing.empty() ? std::string() : " -- " + paths.missing) + "\n");

    // Connect to the app's pipe (it exists before we were spawned). OVERLAPPED on
    // both ends: neither side may ever block on the other (see TrPipe.h).
    const std::wstring wpipe = utf8ToWideL(pipePath);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100; ++i) {
        pipe = ::CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() == ERROR_PIPE_BUSY) ::WaitNamedPipeW(wpipe.c_str(), 500);
        else ::Sleep(50);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        engineLog("[SuVtitleEngine] the app's pipe could not be opened: exit 4\n");
        return 4;
    }
    DWORD pmode = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(pipe, &pmode, nullptr, nullptr);

    // HELLO <slotName>: AudioPB named its fabric slot here ("tr:Translator (pid)", the card's identity on the
    // desk). SuVtitle has no fabric, so the field means nothing any more -- and TrPipe.h is unchanged, so it
    // is still sent, with a FIXED value (SPLIT_PLAN's rule for a field without a meaning). The app reads
    // HELLO as "connected", whatever it names.
    trpipe::writeMessage(pipe, "HELLO\tengine");

    Shared sh;
    sh.mode = cfg.mode;
    sh.preset = cfg.preset;
    sh.langA = cfg.langA;
    sh.langB = cfg.langB;
    sh.state.preset = cfg.preset;
    sh.state.mode   = cfg.mode;
    sh.state.langA  = cfg.langA;
    sh.state.langB  = cfg.langB;
    // spkOn / spkModel / spkCount stay 0 / "" / 0 (WireState's defaults, fixed): no diarization stage, and
    // TrPipe.h makes that trio optional for a reader. asrModel / mtModel / gpu: the pipeline thread's first
    // act (stateModels).
    // W1: the doorbell, created BEFORE the pipeline thread so that thread never sees it
    // change. A creation failure is not fatal -- nullptr simply means the servant falls
    // back to its deadline, which is what it did before this wave existed.
    sh.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    Servers srv;
    srv.setPaths(paths);

    std::thread pipeline(pipelineMain, std::ref(sh), std::ref(srv), cfg, !nofabric);
    // FILE MODE (WB): the FPROXY worker, CPU only, posting into the same never-dropped outbox.
    SharedFileSink proxySink(sh);
    ProxyWorker    proxy(proxySink);
    proxy.start();

    uint64_t lastStateBeat = 0;
    const char* why = "QUIT";
    for (;;) {
        // 1) drain anything we owe the app
        std::vector<std::string> outbox;
        {
            std::lock_guard<std::mutex> lk(sh.mx);
            if (sh.stateDirty || ::GetTickCount64() - lastStateBeat > 5000) {
                outbox.push_back(trpipe::packState(sh.state));
                sh.stateDirty = false;
                lastStateBeat = ::GetTickCount64();
            }
            // The FILE messages, all of them, in order -- never dropped (Shared::fileOut).
            while (!sh.fileOut.empty()) {
                outbox.push_back(std::move(sh.fileOut.front()));
                sh.fileOut.pop_front();
            }
            // (SUVTITLE SPLIT: the live outbox -- LINE / PART / END, each LINE's waitMs stamped here, "as late
            // as it is possible to stamp it" -- was drained after the file messages.)
        }
        bool broken = false;
        for (auto& m : outbox)
            if (!trpipe::writeMessage(pipe, m)) { broken = true; break; }
        if (broken) { why = "a write to the app's pipe failed"; break; }

        // 2) listen for a control message. A 200 ms deadline that expires is the
        //    NORMAL case (nobody is turning knobs), so it must not look like a fault.
        //
        //    W1: ...and the deadline is no longer what decides when the next message
        //    goes out. `sh.wake` cuts this read short the instant the pipeline pushes
        //    anything, so an FCUE / FPROG / STATE crosses the pipe in microseconds instead of
        //    waiting out whatever was left of the 200 ms. Being woken is not an event in
        //    itself: the loop simply goes round, and step 1 above does the work.
        std::string msg;
        bool pipeBroken = false;
        if (trpipe::readMessage(pipe, msg, 200, &pipeBroken, sh.wake)) {
            if (!servantHandle(sh, &proxy, trpipe::split(msg))) break;     // QUIT
        } else if (pipeBroken) {
            why = "the app went away (the pipe broke)";
            break;                       // the app went away
        }
    }

    sh.quit.store(true, std::memory_order_release);
    sh.abortJob.store(kAbortAll, std::memory_order_release);   // a model load in progress stops now
    if (pipeline.joinable()) pipeline.join();
    proxy.stop();                     // its FPROXYDONE ("cancelled") is queued before this returns
    // THE LAST FILE MESSAGES: the FDONE / FPROXYDONE of whatever QUIT interrupted. Best effort
    // -- on QUIT the app may already have stopped reading, and on a broken pipe nothing can
    // be delivered -- so each write gets a short deadline instead of the usual 3 s.
    {
        std::deque<std::string> rest;
        {
            std::lock_guard<std::mutex> lk(sh.mx);
            rest.swap(sh.fileOut);
        }
        for (const auto& m : rest)
            if (!trpipe::writeMessage(pipe, m, 300)) break;
    }
    ::CloseHandle(pipe);
    // AFTER the join, never before: the pipeline thread rings this doorbell on its way out.
    if (sh.wake) { ::CloseHandle(sh.wake); sh.wake = nullptr; }
    engineLog(std::string("[SuVtitleEngine] exit: ") + why + "\n");
    return 0;
}

// ===========================================================================
// SELFTEST
// ===========================================================================
// (SUVTITLE SPLIT: WavFile / readWavMono -- the live selftest's bench reader -- and compressGaps -- its
// continuous-speech builder for the commit engine's section [4] -- were here. The file selftests read their
// media through the shipping reader, decodeAll16k below.)

// A millisecond clock with real resolution. GetTickCount64 moves in 15.6 ms steps,
// which is the same order as the things this file measures.
double qpcMs() {
    LARGE_INTEGER f{}, t{};
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&t);
    return f.QuadPart ? (double)t.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

// The SYSTEM's character-level Simplified -> Traditional map, for the selftest's ORACLE (WB,
// after the WA review): independent of toTraditional's word-level ELS conversion, so a shown
// zh-tw line that is not a fixed point of it still holds something the system calls Simplified.
std::string lcmapTraditional(const std::string& utf8) {
    const std::wstring w = utf8ToWideL(utf8);
    if (w.empty()) return utf8;
    const int n = ::LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return utf8;
    std::wstring o((size_t)n, L'\0');
    if (::LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, w.c_str(), (int)w.size(),
                        &o[0], n, nullptr, nullptr, 0) != n)
        return utf8;
    return wideToUtf8L(o);
}

// Goertzel: the energy at one frequency, so the resampler assertions can be about
// SIGNAL rather than about "it produced some samples".
double goertzel(const std::vector<float>& x, double freq, double rate) {
    if (x.empty()) return 0.0;
    const double w = 2.0 * 3.14159265358979323846 * freq / rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) { const double s0 = (double)v + c * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(p > 0.0 ? p : 0.0) * 2.0 / (double)x.size();
}

// ===========================================================================
// SELFTEST [7]: FILE MODE (wave WB)
//
//   [7a] the media reader, offline: formats, errors, the picture test, AIFF == WAV bit for bit,
//        the resampler, the TIMELINE (codec priming, edit lists, a late first sample), the
//        FPROXY copy (native rate, <= 2 ch, the same timeline, RF64, cancel);
//   [7b] the job's decisions, offline: the U+FFFD repair on a RECORDED response, the window
//        planner, the commit rule, loops, the cue builder's invariants, the junk filter;
//   [7c] whole jobs on the real servers through the SHIPPING request handler and job lap, read
//        back with the TRAY's unpackers: the wire's order and invariants, the words against the
//        bench text, Traditional under zh-tw, the edit list seen from the cues, cancel,
//        supersede, refusals, RTF turbo vs large-v3;
//   [7d] the job's whisper is gone afterwards (AudioPB's [7d] [7e] [7f] -- the live pair, the live
//        finals' script nets, the live onset gate -- went with the live path: SUVTITLE SPLIT).
// Fixtures: <SUVTITLE_TEST_MEDIA>\filemode\ (see testMediaRoot below). Missing = NOT CHECKED,
// printed as such -- never a silent pass. GPU decodes are not deterministic: every live claim
// is an invariant or a threshold, never an exact text.
// ===========================================================================

// ---------------------------------------------------------------------------
// THE PRIVATE TEST MEDIA (publication wave, 2026-10-24). The selftests that need real recordings (speech in two languages,
// a four-voice meeting, one clip per container) read them from the folder named by the environment variable
// SUVTITLE_TEST_MEDIA. That folder is NOT part of the repository (recordings, and a media set made of them); its layout:
//   <root>\filemode\       the file-mode fixtures: c.mp3 c.m4a v.mp4 v.mkv v.avi v.ts ... meeting.wav meeting_marks.txt ...
//   <root>\bench_en.wav    the two speech recordings the WH media were made of
//   <root>\bench_zh.wav
//   <root>\wh\             the WH media: media\mkv_2a_def2.mkv ... and h\gen\... (the container variants)
// and the maintainer's own film comes from SUVTITLE_TEST_FILM (a multi-audio .mkv, opened READ ONLY). Unset, or not a folder /
// not a file = every check that needs them prints "NOT CHECKED (set SUVTITLE_TEST_MEDIA)" (or _FILM) -- never a failure,
// and never a path. Dev/test runs of the payload itself use SUVTITLE_PAYLOAD (TrEngine.h).
// ---------------------------------------------------------------------------
std::wstring envWide(const wchar_t* name) {
    wchar_t buf[2048] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? std::wstring(buf, n) : std::wstring();
}
// SUVTITLE_TEST_MEDIA as a folder with its trailing backslash; "" when unset or not a folder.
std::wstring testMediaRoot() {
    std::wstring r = envWide(L"SUVTITLE_TEST_MEDIA");
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    if (r.empty()) return std::wstring();
    const DWORD a = ::GetFileAttributesW(r.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring();
    return r + L"\\";
}
// <root>\filemode\ ; "" when there is no test media.
std::wstring testFilemodeDir() {
    const std::wstring r = testMediaRoot();
    return r.empty() ? std::wstring() : r + L"filemode\\";
}
// The one fixture the job-lap checks need: <root>\filemode\c.mp3 when it is there, else "".
std::wstring testFixtureC() {
    const std::wstring f = testFilemodeDir();
    if (f.empty()) return std::wstring();
    const std::wstring c = f + L"c.mp3";
    const DWORD a = ::GetFileAttributesW(c.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? c : std::wstring();
}
// The maintainer's film (SUVTITLE_TEST_FILM), when the variable names a file; else "".
std::wstring testFilmPath() {
    const std::wstring f = envWide(L"SUVTITLE_TEST_FILM");
    if (f.empty()) return std::wstring();
    const DWORD a = ::GetFileAttributesW(f.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? f : std::wstring();
}

void printJobStats(const FileJobStats& st);    // below, with --selftest-file
void whSelftest(Servers& srv, const TrConfig& cfg, int& failures, bool live);   // below, with --selftest-wh

struct CollectSink final : FileSink {
    std::vector<std::string> lines;
    void   push(const std::string& m) override { lines.push_back(m); }
    size_t backlog() const override { return 0; }
};

// One job's wire, parsed back exactly as the tray would.
struct JobRun {
    std::vector<std::string>          kinds;        // message kinds, in order
    std::vector<trpipe::WireFileProg> progs;
    std::vector<trpipe::WireCue>      cues;
    std::vector<uint64_t>             cueFrontier;  // the last FPROG doneMs sent BEFORE each cue
    std::vector<trpipe::WireFileDone> dones;
    FileJobStats                      st;
};

JobRun parseJobRun(const std::vector<std::string>& lines) {
    JobRun r;
    uint64_t frontier = 0;
    for (const auto& m : lines) {
        const auto t = trpipe::split(m);
        if (t.empty()) continue;
        r.kinds.push_back(t[0]);
        trpipe::WireFileProg p; trpipe::WireCue c; trpipe::WireFileDone d;
        if (trpipe::unpackFileProg(t, p))      { r.progs.push_back(p); frontier = p.doneMs; }
        else if (trpipe::unpackCue(t, c))      { r.cues.push_back(c); r.cueFrontier.push_back(frontier); }
        else if (trpipe::unpackFileDone(t, d)) r.dones.push_back(d);
    }
    return r;
}

size_t countKind(const std::vector<std::string>& lines, const char* kind) {
    const std::string k = std::string(kind) + "\t";
    size_t n = 0;
    for (const auto& m : lines) if (m.compare(0, k.size(), k) == 0) ++n;
    return n;
}

// The wire invariants every finished job keeps, whatever the audio. Returns the failures.
int checkJobWire(const JobRun& r, uint64_t jobId, const char* label, bool expectOk) {
    int f = 0;
    auto fail = [&](const std::string& what) {
        std::printf("    ASSERT FAIL [%s]: %s\n", label, what.c_str());
        ++f;
    };
    if (r.dones.size() != 1) fail("exactly one FDONE per FILE (got " + std::to_string(r.dones.size()) + ")");
    if (r.kinds.empty() || r.kinds.back() != "FDONE") fail("FDONE is not the last message of the job");
    if (!r.dones.empty()) {
        const auto& d = r.dones.back();
        if (d.jobId != jobId) fail("FDONE carries another job's id");
        if (d.count != r.cues.size())
            fail("FDONE count " + std::to_string(d.count) + " != FCUEs sent " + std::to_string(r.cues.size()));
        if (expectOk && d.result != "ok") fail("result " + d.result + " (" + d.err + ")");
    }
    uint64_t totalMs = 0;
    if (expectOk) {
        if (r.kinds.empty() || r.kinds.front() != "FPROG" || r.progs.empty() || r.progs.front().phase != "loading")
            fail("the job did not open with FPROG loading");
        else if (r.progs.front().totalMs == 0)
            fail("FPROG loading without the duration (the file is probed before the model load)");
        else
            totalMs = r.progs.front().totalMs;
    }
    uint64_t last = 0;
    for (const auto& p : r.progs) {
        if (p.jobId != jobId) fail("an FPROG for another job");
        if (p.doneMs < last) fail("the frontier went BACKWARDS");
        last = p.doneMs;
    }
    for (size_t i = 0; i < r.cues.size(); ++i) {
        const auto& c = r.cues[i];
        const std::string at = " (FCUE " + std::to_string(i) + ")";
        if (c.jobId != jobId) fail("an FCUE for another job" + at);
        if (c.idx != i) fail("idx " + std::to_string(c.idx) + " at position " + std::to_string(i));
        if (c.endMs <= c.startMs) fail("ends before it starts" + at);
        if (i && c.startMs <= r.cues[i - 1].startMs) fail("starts not strictly increasing" + at);
        if (i && c.startMs < r.cues[i - 1].endMs) fail("overlaps the cue before" + at);
        if (c.src.find_first_of("\r\n") != std::string::npos || c.dst.find_first_of("\r\n") != std::string::npos)
            fail("a hard line break inside the text" + at);
        if (c.src.empty() || c.dst.empty()) fail("an empty text" + at);
        if (c.src.find("\xef\xbf\xbd") != std::string::npos || c.dst.find("\xef\xbf\xbd") != std::string::npos)
            fail("U+FFFD in the text" + at);
        if (totalMs && c.endMs > totalMs + 100) fail("ends past the end of the media" + at);
        // TrPipe.h: "Every cue ending before [the frontier] has already been sent".
        if (c.endMs < r.cueFrontier[i])
            fail("arrived AFTER a frontier (" + std::to_string(r.cueFrontier[i]) + " ms) its end lies before" + at);
    }
    return f;
}

// Run one FILE request to its FDONE through the SHIPPING request handler and job lap.
JobRun runFileJob(Servers& srv, const TrConfig& cfg, uint64_t jobId, const std::string& path,
                  const char* a, const char* b, const char* mode, const char* quality,
                  bool printCues) {
    Shared sh;
    CollectSink sink;
    trpipe::WireFile f;
    f.jobId = jobId; f.path = path; f.langA = a; f.langB = b; f.mode = mode; f.quality = quality;
    // Through the WIRE: packed by the tray's function, unpacked by the helper's.
    trpipe::WireFile g;
    trpipe::unpackFile(trpipe::split(trpipe::packFile(f)), g);
    { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(g); }
    std::unique_ptr<FileJob> job;
    FileJobStats st;
    fileRequests(sh, job, srv, sink, cfg, &st);
    while (job) fileJobLap(job, srv, sink, sh.abortJob, false, &st);
    JobRun r = parseJobRun(sink.lines);
    r.st = st;
    if (printCues)
        for (const auto& c : r.cues)
            std::printf("      %6llu-%6llu %s->%s%s  %s  |  %s\n", (unsigned long long)c.startMs,
                        (unsigned long long)c.endMs, c.lang.c_str(), c.dstLang.c_str(),
                        c.pass ? " (pass)" : "", c.src.c_str(), c.dst.c_str());
    return r;
}

// The whole 16 kHz decode of a file through the shipping reader.
bool decodeAll16k(const std::wstring& path, std::vector<float>& out, MediaInfo& info, std::string& err) {
    MediaReader r;
    out.clear();
    if (!r.open(path, MediaReader::Out::Mono16k, err)) return false;
    info = r.info();
    int stall = 0;
    while (!r.eof()) {
        const size_t was = out.size();
        if (!r.readFloat(out, 16000 * 8, err)) return false;
        if (out.size() == was && ++stall > 1000) { err = "the reader stopped making progress"; return false; }
    }
    return true;
}

double firstAbove(const std::vector<float>& x, float thr) {
    for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > thr) return (double)i / 16000.0;
    return -1.0;
}

double energyCentroid(const std::vector<float>& x, double t0, double t1) {
    double num = 0.0, den = 0.0;
    for (size_t i = (size_t)(t0 * 16000); i < x.size() && i < (size_t)(t1 * 16000); ++i) {
        const double e = (double)x[i] * x[i];
        num += e * (double)i / 16000.0;
        den += e;
    }
    return den > 0.0 ? num / den : -1.0;
}

// The FPROXY copy's header, read back independently of the writer.
struct WavFacts { bool ok = false, rf64 = false; uint32_t rate = 0, ch = 0, bits = 0; uint64_t frames = 0; std::string why; };
WavFacts readProxyHeader(const std::wstring& path) {
    WavFacts w;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { w.why = "no file"; return w; }
    unsigned char b[80] = {};
    DWORD got = 0;
    ::ReadFile(h, b, 80, &got, nullptr);
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    ::CloseHandle(h);
    auto u16 = [&](int o) { return (uint32_t)(b[o] | (b[o + 1] << 8)); };
    auto u32 = [&](int o) { return (uint32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24)); };
    auto u64 = [&](int o) { return (uint64_t)u32(o) | ((uint64_t)u32(o + 4) << 32); };
    if (got < 80) { w.why = "short header"; return w; }
    w.rf64 = std::memcmp(b, "RF64", 4) == 0;
    if (!w.rf64 && std::memcmp(b, "RIFF", 4)) { w.why = "not RIFF/RF64"; return w; }
    if (std::memcmp(b + 8, "WAVE", 4) || std::memcmp(b + 48, "fmt ", 4) || std::memcmp(b + 72, "data", 4)) {
        w.why = "chunk layout"; return w;
    }
    if (u16(56) != 1) { w.why = "not PCM"; return w; }
    w.ch = u16(58); w.rate = u32(60); w.bits = u16(70);
    const uint64_t dataBytes = w.rf64 ? u64(28) : u32(76);
    const uint64_t riff = w.rf64 ? u64(20) : u32(4);
    if (riff + 8 != (uint64_t)sz.QuadPart) { w.why = "RIFF size != file size"; return w; }
    if (dataBytes + 80 != (uint64_t)sz.QuadPart) { w.why = "data size != file size"; return w; }
    if (w.rf64 && (std::memcmp(b + 12, "ds64", 4) || u32(4) != 0xFFFFFFFFu || u32(76) != 0xFFFFFFFFu)) {
        w.why = "RF64 without its ds64 / sentinels"; return w;
    }
    w.frames = (w.ch && w.bits) ? dataBytes / (w.ch * (w.bits / 8)) : 0;
    w.ok = true;
    return w;
}

// [0ter] THE SIMPLIFIED NET (WB), offline: toSimplified and scriptNet by direct calls -- what
// they convert, what they must leave EXACTLY as written, and that a zh-tw label is never
// simplified. The live and file call sites are proved in [7c] / [7e] with a stand-in MT reply.
void simplifiedNetTests(int& failures) {
    std::printf("\n[0ter] Simplified Chinese net (toSimplified / scriptNet), offline\n");
    const int before = failures;
    auto ok = [&](bool cond, const char* what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
    };
    const char* kT = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe5\x80\x91\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xaa\xa2"
                     "\xe6\x9f\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\xa0\xb4\xe7\x9a\x84\xe6\x9c\x80\xe7\xb5\x82\xe6\xb7\xb7"
                     "\xe9\x9f\xb3\xe3\x80\x82";   // U+4ECA,5929,6211,5011,8981,4E00,8D77,6AA2,67E5,7B2C,4E8C,5834,7684,6700,7D42,6DF7,97F3,3002
    const char* kS = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80"
                     "\xe6\x9f\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7"
                     "\xe9\x9f\xb3\xe3\x80\x82";   // U+4ECA,5929,6211,4EEC,8981,4E00,8D77,68C0,67E5,7B2C,4E8C,573A,7684,6700,7EC8,6DF7,97F3,3002
    // WORD level: U+5F8C,4F86 / U+4EC0,9EBC are what the character map alone gets wrong (U+5F8C,6765, U+4EC0,9EBD).
    const char* kTw = "\xe5\xbe\x8c\xe4\xbe\x86\xe6\x88\x91\xe5\x80\x91\xe6\x89\x8d\xe7\x99\xbc\xe7\x8f\xbe\xe9\xa0\xad"
                      "\xe9\xab\xae\xe7\x9a\x84\xe5\x95\x8f\xe9\xa1\x8c\xe6\x98\xaf\xe4\xbb\x80\xe9\xba\xbc\xe3\x80\x82";
                      // U+5F8C,4F86,6211,5011,624D,767C,73FE,982D,9AEE,7684,554F,984C,662F,4EC0,9EBC,3002
    const char* kSw = "\xe5\x90\x8e\xe6\x9d\xa5\xe6\x88\x91\xe4\xbb\xac\xe6\x89\x8d\xe5\x8f\x91\xe7\x8e\xb0\xe5\xa4\xb4"
                      "\xe5\x8f\x91\xe7\x9a\x84\xe9\x97\xae\xe9\xa2\x98\xe6\x98\xaf\xe4\xbb\x80\xe4\xb9\x88\xe3\x80\x82";
                      // U+540E,6765,6211,4EEC,624D,53D1,73B0,5934,53D1,7684,95EE,9898,662F,4EC0,4E48,3002
    // CORRECT Simplified the character map would break: U+8457,540D / U+663E,8457 / U+77AD,671B keep U+8457 and U+77AD.
    const char* kKeep = "\xe8\x91\x97\xe5\x90\x8d\xe7\x9a\x84\xe4\xbd\x9c\xe5\xae\xb6\xef\xbc\x8c\xe6\x95\x88\xe6\x9e\x9c"
                        "\xe6\x98\xbe\xe8\x91\x97\xef\xbc\x8c\xe7\x9e\xad\xe6\x9c\x9b\xe8\xbf\x9c\xe6\x96\xb9\xe3\x80\x82";
                        // U+8457,540D,7684,4F5C,5BB6,FF0C,6548,679C,663E,8457,FF0C,77AD,671B,8FDC,65B9,3002
    const std::string kMixT = std::string("\xe9\x80\x99\xe5\x80\x8b") + " reverb " + "\xe5\xa4\xaa\xe5\xa4\x9a\xe4\xba\x86";
    const std::string kMixS = std::string("\xe8\xbf\x99\xe4\xb8\xaa") + " reverb " + "\xe5\xa4\xaa\xe5\xa4\x9a\xe4\xba\x86";
    const char* kJa = "\xe5\xad\xb8\xe6\xa0\xa1\xe3\x81\xae\xe6\x9c\x83\xe8\xad\xb0";   // U+5B78,6821,306E,6703,8B70
    bool c1 = false, c2 = true, c3 = true, c4 = true, c5 = false, c6 = false;
    ok(toSimplified(kT, &c1) == kS && c1, "toSimplified: a Traditional line comes back Simplified (changed=true)");
    const std::string w = toSimplified(kTw, &c5);
    ok(w == kSw && c5, "toSimplified: word-level (\xe5\xbe\x8c\xe4\xbe\x86 / \xe9\xa0\xad\xe9\xab\xae / \xe4\xbb\x80\xe9\xba\xbc -> \xe5\x90\x8e\xe6\x9d\xa5 / \xe5\xa4\xb4\xe5\x8f\x91 / \xe4\xbb\x80\xe4\xb9\x88), not character-level");
    if (w != kSw) std::printf("    got: %s\n", w.c_str());
    ok(toSimplified(kS, &c2) == kS && !c2, "toSimplified: Simplified text comes back untouched (changed=false)");
    ok(toSimplified("Set the levels.", &c3) == "Set the levels." && !c3, "toSimplified: Latin comes back untouched");
    ok(toSimplified(kKeep, &c4) == kKeep && !c4,
       "toSimplified: correct Simplified holding \xe8\x91\x97 / \xe7\x9e\xad (\xe8\x91\x97\xe5\x90\x8d, \xe6\x98\xbe\xe8\x91\x97, \xe7\x9e\xad\xe6\x9c\x9b) is NOT rewritten");
    ok(toSimplified(kMixT, &c6) == kMixS && c6 && toSimplified(kMixS) == kMixS,
       "toSimplified: a mixed line converts its Han and keeps ' reverb ' intact");
    ok(scriptNet(kT, "zh") == kS && scriptNet(kS, "zh-tw") == toTraditional(kS),
       "scriptNet: zh -> toSimplified, zh-tw -> toTraditional");
    ok(scriptNet(kT, "zh-tw") == kT && scriptNet(kTw, "zh-tw") == kTw,
       "scriptNet: a zh-tw line is NEVER simplified");
    ok(scriptNet(kT, "en") == kT && scriptNet(kJa, "ja") == kJa && scriptNet(kT, "yue") == kT && scriptNet(kS, "") == kS,
       "scriptNet: every other label (en, ja, yue, none) comes back exactly as written");
    std::printf("    %s\n", failures == before ? "toSimplified / scriptNet OK" : "SEE FAILURES ABOVE");
}

// [0quater] THE REVIEW'S FIXES that need no server (WB, after the adversarial review): the prompt's
// echo at ANY level, the kana marks Chinese also writes, the segment -> word ranges of a parsed
// response, and the alphabets the text rules used to ignore.
void reviewFixTests(int& failures) {
    std::printf("\n[0quater] review fixes, offline (echo at any level, kana marks, word ranges, scripts)\n");
    const int before = failures;
    auto ok = [&](bool cond, const char* what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
    };
    // -- the echo at ANY level --------------------------------------------------------------------
    const std::string pr = asrGlossaryFor("en") + " Today we are going to review the final mix of the second scene.";
    ok(looksLikeHallucination("Today we are going to review the final mix", -12.0, pr),
       "a LOUD final quoting >= 4 words of its prompt is junk (the prompt's echo, at any level)");
    ok(!looksLikeHallucination("The second scene.", -12.0, pr),
       "... a loud 3-word repeat is kept (a person repeating himself)");
    ok(looksLikeHallucination("Studio talk: levels, faders, reverb, monitoring", -12.0, ""),
       "a quotation of a glossary line is junk at any level, prompt or no prompt");
    ok(!looksLikeHallucination("Bring the faders up and check the reverb on the dialogue.", -12.0, pr),
       "a real sentence using glossary WORDS is not a quotation");
    ok(looksLikeHallucination("\xe5\xbd\x95\xe9\x9f\xb3\xe6\xa3\x9a\xe9\x87\x8c\xe7\x9a\x84\xe5\xaf\xb9\xe8\xaf\x9d"
                              "\xef\xbc\x9a\xe9\x9f\xb3\xe9\x87\x8f\xe3\x80\x81\xe6\x8e\xa8\xe5\xad\x90", -12.0, ""),
       "the zh glossary quoted back (8+ Han characters) is junk at any level");
    // -- U+30FB and U+30FC are Chinese too ----------------------------------------------------------------
    ok(!hasKanaOrHangul("\xe6\xaf\x94\xe8\xb5\x9b\xe3\x83\xbb\xe7\xbb\x93\xe6\x9e\x9c")                  // U+6BD4,8D5B,30FB,7ED3,679C
       && !hasKanaOrHangul("\xe5\x8d\xa1\xe6\x8b\x89OK\xe3\x83\xbc")                                       // U+5361,62C9OKU+30FC
       && hasKanaOrHangul("\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x92\xe3\x83\xbc")                              // U+30B3,30FC,30D2,30FC
       && chineseByScript("\xe6\xaf\x94\xe8\xb5\x9b\xe3\x83\xbb\xe7\xbb\x93\xe6\x9e\x9c\xe7\x9a\x84\xe5\x85\xac\xe5\xb8\x83", "zh"),
       "U+30FB / U+30FC are not Japanese evidence; real kana still is");
    // -- segment -> word ranges survive an empty word -------------------------------------------
    const char* kResp =
        "{\"task\":\"transcribe\",\"language\":\"english\",\"duration\":2.0,\"text\":\" Hello there. Good morning.\","
        "\"segments\":[{\"id\":0,\"text\":\" Hello there.\",\"start\":0.0,\"end\":1.0,\"tokens\":[],\"words\":["
        "{\"word\":\" Hello\",\"start\":0.0,\"end\":0.4,\"t_dtw\":-1,\"probability\":0.9},"
        "{\"word\":\"\",\"start\":0.4,\"end\":0.4,\"t_dtw\":-1,\"probability\":0.1},"
        "{\"word\":\" there.\",\"start\":0.4,\"end\":1.0,\"t_dtw\":-1,\"probability\":0.9}],"
        "\"temperature\":0.0,\"avg_logprob\":-0.1,\"no_speech_prob\":0.0},"
        "{\"id\":1,\"text\":\" Good morning.\",\"start\":1.0,\"end\":2.0,\"tokens\":[],\"words\":["
        "{\"word\":\" Good\",\"start\":1.0,\"end\":1.4,\"t_dtw\":-1,\"probability\":0.9},"
        "{\"word\":\" morning.\",\"start\":1.4,\"end\":2.0,\"t_dtw\":-1,\"probability\":0.9}],"
        "\"temperature\":0.0,\"avg_logprob\":-0.1,\"no_speech_prob\":0.0}],"
        "\"detected_language\":\"english\",\"detected_language_probability\":1.0}";
    AsrResult a;
    parseAsrResponse(kResp, a);
    bool joined = a.segments.size() == 2;
    for (const auto& s : a.segments) {
        std::string j;
        for (size_t i = s.w0; i < s.w1 && i < a.words.size(); ++i) j += a.words[i].text;
        if (oneLine(j) != oneLine(s.text)) { joined = false; std::printf("    joined: %s | text: %s\n", j.c_str(), s.text.c_str()); }
    }
    ok(joined, "each segment's word range still joins back to ITS text after an empty word was dropped");
    // -- the other alphabets are words, kana / Hangul one token per character --------------------
    const std::vector<std::string> ru = normWords("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\x9c\xd0\x98\xd0\xa0!");
    ok(ru.size() == 2 && ru[0] == "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82" && ru[1] == "\xd0\xbc\xd0\xb8\xd1\x80",
       "Cyrillic is tokenised (\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\x9c\xd0\x98\xd0\xa0! -> \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 | \xd0\xbc\xd0\xb8\xd1\x80)");
    ok(normWords("\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf").size() == 5
       && normWords("\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94").size() == 5,
       "kana and Hangul: one token per character");
    ok(normWords("Set the levels.") == std::vector<std::string>({ "set", "the", "levels" })
       && normWords("\xe5\xa4\xa7\xe5\xae\xb6\xe5\xa5\xbd").size() == 3,
       "Latin and Han tokens unchanged");
    // -- the onset gate's level, ON ANY GRID: one 30 ms burst straddling two fixed-grid hops --------
    {
        std::vector<float> b((size_t)kAsrRate, 0.0f);
        for (size_t i = 0; i < 480; ++i) b[240 + i] = (float)(0.1 * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / kAsrRate));
        const double want = 20.0 * std::log10(0.1 / std::sqrt(2.0)), got = hopPeakDbfs(b);
        std::printf("    a 30 ms burst at a half-hop offset: hopPeakDbfs %.2f dB (its own RMS %.2f dB)\n", got, want);
        ok(std::fabs(got - want) < 0.3, "hopPeakDbfs finds a hop's level whatever the grid (no 3 dB loss at a half-hop offset)");
    }
    std::printf("    %s\n", failures == before ? "echo / kana marks / ranges / scripts OK" : "SEE FAILURES ABOVE");
}

// The proxy's PCM, read back independently of the writer (80-byte header, 16-bit interleaved).
bool readProxyPcm(const std::wstring& path, uint32_t& rate, uint32_t& ch, std::vector<int16_t>& pcm) {
    const WavFacts w = readProxyHeader(path);
    if (!w.ok || w.bits != 16) return false;
    rate = w.rate; ch = w.ch;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    pcm.assign((size_t)(w.frames * w.ch), 0);
    LARGE_INTEGER at; at.QuadPart = 80;
    DWORD got = 0;
    const bool ok = ::SetFilePointerEx(h, at, nullptr, FILE_BEGIN)
                 && ::ReadFile(h, pcm.data(), (DWORD)(pcm.size() * 2), &got, nullptr) && got == pcm.size() * 2;
    ::CloseHandle(h);
    return ok;
}

bool writeBytes(const std::wstring& path, const std::vector<unsigned char>& b) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = ::WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr) && w == b.size();
    ::CloseHandle(h);
    return ok;
}

// ===========================================================================
// [7g] FILM-GRADE CUES (wave WF), offline. The user's first real film (11/10, a French feature, pair
// fr<->en): whisper's token times drifted over music (a word held 7 s, sentences squeezed into 1 s), a
// sentence was shown twice, cues were cut INSIDE words, Russian / Portuguese "speech" (whisper's
// training captions over music) was translated. Held here against RECORDED responses of that film
// (turbo + -dtw large.v3.turbo -nfa, the job's own windows): C tokens -> words, A the DTW clock, B the
// duplicate's root (a hard cut whose only segment is a late sentence) and the safety de-dup, D the
// language kept inside the pair (re-decode, tags, credits, other scripts), E reading time + fragments.
// ===========================================================================
void filmGradeTests(Servers& srv, const TrConfig& cfg, int& failures) {
    std::printf("\n[7g] FILE MODE -- film-grade cues (wave WF): recorded film responses, offline\n");
    const int before = failures;
    auto ok = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what.c_str()); ++failures; }
    };
    // kFilmHard: window 320.26-350.26 s of the film -- music, and only its last 1.5 s are speech; whisper
    // returned ONE segment over the whole window (token times 0.00-29.98, i.e. stretched over the music)
    static const char* kFilmHard =
        "{\"task\":\"transcribe\",\"language\":\"french\",\"duration\":30.0,\"text\":\" C'est quoi cette "
        "ombre dans le noir ?\\n\",\"segments\":[{\"id\":0,\"text\":\" C'est quoi cette ombre dans le noi"
        "r ?\",\"start\":0.0,\"end\":29.98,\"tokens\":[],\"words\":[{\"word\":\" C\",\"start\":0.0,\"end"
        "\":1.57,\"t_dtw\":1360,\"probability\":0.049},{\"word\":\"'\",\"start\":1.57,\"end\":3.12,\"t_dt"
        "w\":2844,\"probability\":0.969},{\"word\":\"est\",\"start\":3.12,\"end\":7.78,\"t_dtw\":2844,\"p"
        "robability\":0.969},{\"word\":\" quoi\",\"start\":7.79,\"end\":14.01,\"t_dtw\":2924,\"probabilit"
        "y\":0.242},{\"word\":\" cette\",\"start\":14.09,\"end\":21.82,\"t_dtw\":2942,\"probability\":0.6"
        "79},{\"word\":\" ombre\",\"start\":21.82,\"end\":29.64,\"t_dtw\":2964,\"probability\":0.95},{\"w"
        "ord\":\" dans\",\"start\":29.64,\"end\":29.74,\"t_dtw\":2976,\"probability\":0.857},{\"word\":\""
        " le\",\"start\":29.74,\"end\":29.79,\"t_dtw\":2986,\"probability\":0.978},{\"word\":\" noir\",\""
        "start\":29.79,\"end\":29.89,\"t_dtw\":2996,\"probability\":0.986},{\"word\":\" ?\",\"start\":29."
        "89,\"end\":29.98,\"t_dtw\":2996,\"probability\":0.906}],\"temperature\":0.0,\"avg_logprob\":-0.4"
        "36,\"no_speech_prob\":0.0}],\"detected_language\":\"french\",\"detected_language_probability\":0"
        ".9614,\"language_probabilities\":{\"fr\":0.9614,\"en\":0.0321,\"es\":0.0017,\"pt\":0.0011}}";
    // kFilmNext: window 347.76-369.79 s -- the same sentence heard whole, then three short lines
    static const char* kFilmNext =
        "{\"task\":\"transcribe\",\"language\":\"french\",\"duration\":22.03,\"text\":\" B-A-7, c'est quo"
        "i cette ombre dans le noir ?\\n J'en sais rien, mais je vais l'ouvrir.\\n La porte est ferm\xc3"
        "\xa9"
        "e.\\n Permission donn\xc3"
        "\xa9"
        "e.\\n\",\"segments\":[{\"id\":0,\"text\":\" B-A-7, c'est quoi cette ombre dans le noir ?\",\"sta"
        "rt\":0.92,\"end\":2.76,\"tokens\":[],\"words\":[{\"word\":\" B\",\"start\":0.92,\"end\":0.92,\"t"
        "_dtw\":116,\"probability\":0.808},{\"word\":\"-\",\"start\":0.92,\"end\":0.92,\"t_dtw\":116,\"pr"
        "obability\":0.238},{\"word\":\"A\",\"start\":0.92,\"end\":0.92,\"t_dtw\":126,\"probability\":0.8"
        "78},{\"word\":\"-\",\"start\":0.92,\"end\":0.92,\"t_dtw\":134,\"probability\":0.902},{\"word\":"
        "\"7\",\"start\":0.92,\"end\":0.92,\"t_dtw\":146,\"probability\":0.99},{\"word\":\",\",\"start\":"
        "0.92,\"end\":0.92,\"t_dtw\":152,\"probability\":0.95},{\"word\":\" c\",\"start\":0.92,\"end\":0."
        "92,\"t_dtw\":162,\"probability\":0.995},{\"word\":\"'\",\"start\":0.92,\"end\":0.92,\"t_dtw\":16"
        "2,\"probability\":1.0},{\"word\":\"est\",\"start\":0.92,\"end\":0.92,\"t_dtw\":164,\"probability"
        "\":1.0},{\"word\":\" quoi\",\"start\":0.92,\"end\":1.15,\"t_dtw\":176,\"probability\":1.0},{\"wo"
        "rd\":\" cette\",\"start\":1.15,\"end\":1.48,\"t_dtw\":194,\"probability\":0.967},{\"word\":\" om"
        "bre\",\"start\":1.48,\"end\":1.8,\"t_dtw\":214,\"probability\":0.999},{\"word\":\" dans\",\"star"
        "t\":1.81,\"end\":2.07,\"t_dtw\":226,\"probability\":0.993},{\"word\":\" le\",\"start\":2.07,\"en"
        "d\":2.2,\"t_dtw\":238,\"probability\":0.997},{\"word\":\" noir\",\"start\":2.2,\"end\":2.46,\"t_"
        "dtw\":254,\"probability\":1.0},{\"word\":\" ?\",\"start\":2.46,\"end\":2.76,\"t_dtw\":272,\"prob"
        "ability\":0.989}],\"temperature\":0.0,\"avg_logprob\":-0.118,\"no_speech_prob\":0.0},{\"id\":1,"
        "\"text\":\" J'en sais rien, mais je vais l'ouvrir.\",\"start\":2.76,\"end\":4.04,\"tokens\":[],"
        "\"words\":[{\"word\":\" J\",\"start\":2.76,\"end\":2.79,\"t_dtw\":288,\"probability\":0.956},{\""
        "word\":\"'\",\"start\":2.79,\"end\":2.82,\"t_dtw\":290,\"probability\":1.0},{\"word\":\"en\",\"s"
        "tart\":2.82,\"end\":2.89,\"t_dtw\":294,\"probability\":0.999},{\"word\":\" sais\",\"start\":2.89"
        ",\"end\":3.03,\"t_dtw\":306,\"probability\":0.999},{\"word\":\" rien\",\"start\":3.03,\"end\":3."
        "17,\"t_dtw\":326,\"probability\":1.0},{\"word\":\",\",\"start\":3.17,\"end\":3.24,\"t_dtw\":334,"
        "\"probability\":0.872},{\"word\":\" mais\",\"start\":3.24,\"end\":3.38,\"t_dtw\":344,\"probabili"
        "ty\":0.998},{\"word\":\" je\",\"start\":3.38,\"end\":3.45,\"t_dtw\":354,\"probability\":0.998},{"
        "\"word\":\" vais\",\"start\":3.45,\"end\":3.59,\"t_dtw\":362,\"probability\":1.0},{\"word\":\" l"
        "\",\"start\":3.59,\"end\":3.62,\"t_dtw\":370,\"probability\":0.966},{\"word\":\"'\",\"start\":3."
        "62,\"end\":3.64,\"t_dtw\":372,\"probability\":1.0},{\"word\":\"ou\",\"start\":3.65,\"end\":3.72,"
        "\"t_dtw\":382,\"probability\":1.0},{\"word\":\"vr\",\"start\":3.72,\"end\":3.82,\"t_dtw\":394,\""
        "probability\":1.0},{\"word\":\"ir\",\"start\":3.82,\"end\":3.89,\"t_dtw\":402,\"probability\":1."
        "0},{\"word\":\".\",\"start\":3.89,\"end\":4.04,\"t_dtw\":416,\"probability\":0.992}],\"temperatu"
        "re\":0.0,\"avg_logprob\":-0.015,\"no_speech_prob\":0.0},{\"id\":2,\"text\":\" La porte est ferm"
        "\xc3"
        "\xa9"
        "e.\",\"start\":4.04,\"end\":5.08,\"tokens\":[],\"words\":[{\"word\":\" La\",\"start\":4.04,\"end"
        "\":4.14,\"t_dtw\":436,\"probability\":1.0},{\"word\":\" p\",\"start\":4.14,\"end\":4.19,\"t_dtw"
        "\":452,\"probability\":0.999},{\"word\":\"orte\",\"start\":4.19,\"end\":4.4,\"t_dtw\":464,\"prob"
        "ability\":0.998},{\"word\":\" est\",\"start\":4.4,\"end\":4.56,\"t_dtw\":474,\"probability\":1.0"
        "},{\"word\":\" ferm\",\"start\":4.56,\"end\":4.72,\"t_dtw\":494,\"probability\":1.0},{\"word\":"
        "\"\xc3"
        "\xa9"
        "e\",\"start\":4.72,\"end\":4.88,\"t_dtw\":506,\"probability\":1.0},{\"word\":\".\",\"start\":4.8"
        "8,\"end\":5.08,\"t_dtw\":550,\"probability\":0.999}],\"temperature\":0.0,\"avg_logprob\":-0.001,"
        "\"no_speech_prob\":0.0},{\"id\":3,\"text\":\" Permission donn\xc3"
        "\xa9"
        "e.\",\"start\":5.08,\"end\":6.7,\"tokens\":[],\"words\":[{\"word\":\" Per\",\"start\":5.08,\"end"
        "\":5.31,\"t_dtw\":586,\"probability\":0.996},{\"word\":\"mis\",\"start\":5.31,\"end\":5.46,\"t_d"
        "tw\":596,\"probability\":1.0},{\"word\":\"sion\",\"start\":5.46,\"end\":5.99,\"t_dtw\":624,\"pro"
        "bability\":1.0},{\"word\":\" don\",\"start\":5.99,\"end\":6.22,\"t_dtw\":650,\"probability\":0.6"
        "88},{\"word\":\"n\xc3"
        "\xa9"
        "e\",\"start\":6.22,\"end\":6.45,\"t_dtw\":668,\"probability\":0.997},{\"word\":\".\",\"start\":6"
        ".45,\"end\":6.7,\"t_dtw\":668,\"probability\":0.915}],\"temperature\":0.0,\"avg_logprob\":-0.067"
        ",\"no_speech_prob\":0.0}],\"detected_language\":\"french\",\"detected_language_probability\":0.9"
        "982,\"language_probabilities\":{\"fr\":0.9982,\"en\":0.0012}}";
    // kFilmRu: window 21.34-33.79 s -- music; whisper's "speech" is a Russian subtitle credit
    // ("U+0420,0435,0434,0430,043A,0442,043E,0440 U+0441,0443,0431,0442,0438,0442,0440,043E,0432 U+0410.U+0421,0435,043C,043A,0438,043D U+041A,043E,0440,0440,0435,043A,0442,043E,0440 U+0410.U+0415,0433,043E,0440,043E,0432,0430"), heard as ru p=0.25
    static const char* kFilmRu =
        "{\"task\":\"transcribe\",\"language\":\"russian\",\"duration\":12.45,\"text\":\" \xd0\xa0\xd0\xb5\xd0\xb4\xd0\xb0\xd0\xba\xd1\x82\xd0\xbe\xd1\x80 "
        "\xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2 \xd0\x90.\xd0\xa1\xd0\xb5\xd0\xbc\xd0\xba\xd0\xb8\xd0\xbd "
        "\xd0\x9a\xd0\xbe\xd1\x80\xd1\x80\xd0\xb5\xd0\xba\xd1\x82\xd0\xbe\xd1\x80 \xd0\x90.\xd0\x95\xd0\xb3\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\\"
        "n\",\"segments\":[{\"id\":0,\"text\":\" \xd0\xa0\xd0\xb5\xd0\xb4\xd0\xb0\xd0\xba\xd1\x82\xd0\xbe\xd1\x80 "
        "\xd1\x81\xd1\x83\xd0\xb1\xd1\x82\xd0\xb8\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2 \xd0\x90.\xd0\xa1\xd0\xb5\xd0\xbc\xd0\xba\xd0\xb8\xd0\xbd "
        "\xd0\x9a\xd0\xbe\xd1\x80\xd1\x80\xd0\xb5\xd0\xba\xd1\x82\xd0\xbe\xd1\x80 \xd0\x90.\xd0\x95\xd0\xb3\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\""
        ",\"start\":0.0,\"end\":12.42,\"tokens\":[],\"words\":[{\"word\":\" \xd0\xa0\",\"start\":0.21,\"e"
        "nd\":0.48,\"t_dtw\":210,\"probability\":0.3},{\"word\":\"\xd0\xb5\xd0\xb4\",\"start\":0.48,\"end"
        "\":1.45,\"t_dtw\":210,\"probability\":0.966},{\"word\":\"\xd0\xb0\xd0\xba\",\"start\":1.45,\"end"
        "\":2.42,\"t_dtw\":210,\"probability\":1.0},{\"word\":\"\xd1\x82\xd0\xbe\xd1\x80\",\"start\":2.42"
        ",\"end\":3.88,\"t_dtw\":210,\"probability\":0.999},{\"word\":\" \xd1\x81\",\"start\":3.88,\"end\""
        ":4.36,\"t_dtw\":210,\"probability\":0.999},{\"word\":\"\xd1\x83\xd0\xb1\",\"start\":4.36,\"end\""
        ":5.33,\"t_dtw\":210,\"probability\":1.0},{\"word\":\"\xd1\x82\",\"start\":5.33,\"end\":5.81,\"t_"
        "dtw\":210,\"probability\":1.0},{\"word\":\"\xd0\xb8\xd1\x82\",\"start\":5.81,\"end\":6.78,\"t_dt"
        "w\":210,\"probability\":1.0},{\"word\":\"\xd1\x80\xd0\xbe\xd0\xb2\",\"start\":6.78,\"end\":8.24,"
        "\"t_dtw\":210,\"probability\":1.0},{\"word\":\" \xd0\x90\",\"start\":8.24,\"end\":8.72,\"t_dtw\""
        ":210,\"probability\":0.958},{\"word\":\".\",\"start\":8.72,\"end\":9.45,\"t_dtw\":210,\"probabil"
        "ity\":1.0},{\"word\":\"\xd0\xa1\",\"start\":9.45,\"end\":9.93,\"t_dtw\":210,\"probability\":0.99"
        "7},{\"word\":\"\xd0\xb5\xd0\xbc\",\"start\":9.93,\"end\":10.9,\"t_dtw\":210,\"probability\":0.93"
        "9},{\"word\":\"\xd0\xba\xd0\xb8\",\"start\":10.9,\"end\":11.77,\"t_dtw\":210,\"probability\":1.0"
        "},{\"word\":\"\xd0\xbd\",\"start\":11.77,\"end\":12.41,\"t_dtw\":210,\"probability\":1.0},{\"wor"
        "d\":\" \xd0\x9a\xd0\xbe\xd1\x80\",\"start\":12.41,\"end\":12.42,\"t_dtw\":212,\"probability\":0."
        "93},{\"word\":\"\xd1\x80\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":0.999},{\""
        "word\":\"\xd0\xb5\xd0\xba\xd1\x82\xd0\xbe\xd1\x80\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,"
        "\"probability\":1.0},{\"word\":\" \xd0\x90\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"proba"
        "bility\":0.998},{\"word\":\".\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":1.0}"
        ",{\"word\":\"\xd0\x95\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":0.994},{\"wo"
        "rd\":\"\xd0\xb3\xd0\xbe\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":1.0},{\"wo"
        "rd\":\"\xd1\x80\xd0\xbe\xd0\xb2\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":1."
        "0},{\"word\":\"\xd0\xb0\",\"start\":12.42,\"end\":12.42,\"t_dtw\":214,\"probability\":1.0}],\"te"
        "mperature\":0.0,\"avg_logprob\":-0.055,\"no_speech_prob\":0.0}],\"detected_language\":\"russian\""
        ",\"detected_language_probability\":0.2458,\"language_probabilities\":{\"ru\":0.2458,\"en\":0.200"
        "9,\"fr\":0.0947,\"pt\":0.0865}}";
    // the same documents as a server WITHOUT -dtw would have sent them (every t_dtw -1)
    auto noDtw = [](std::string s) {
        for (size_t at = 0; (at = s.find("\"t_dtw\":", at)) != std::string::npos;) {
            at += 8;
            size_t e = at;
            while (e < s.size() && ((s[e] >= '0' && s[e] <= '9') || s[e] == '-')) ++e;
            s.replace(at, e - at, "-1");
        }
        return s;
    };
    auto plan = [](double s, double e, WindowPlan::Why why) {
        WindowPlan p;
        p.s = (uint64_t)std::llround(s * kFileRate);
        p.e = (uint64_t)std::llround(e * kFileRate);
        p.why = why;
        return p;
    };
    // ---- the parse (A, D): t_dtw and the language probabilities ride along -------------------------
    AsrResult hard, next, ru;
    parseAsrResponse(kFilmHard, hard);
    parseAsrResponse(kFilmNext, next);
    parseAsrResponse(kFilmRu, ru);
    {
        double pfr = -1.0;
        for (const auto& p : next.langProbs) if (p.first == "fr") pfr = p.second;
        ok(!next.words.empty() && std::fabs(next.words.front().dtw - 1.16) < 1e-9 && std::fabs(pfr - 0.9982) < 1e-9
           && ru.langProbs.size() == 4 && ru.language == "ru",
           "verbose_json: t_dtw (centiseconds -> s) and language_probabilities are parsed");
        AsrResult flat;
        parseAsrResponse(noDtw(kFilmNext), flat);
        bool none = !flat.words.empty();
        for (const auto& w : flat.words) none = none && w.dtw < 0.0;
        ok(none, "a server without -dtw: every word's dtw stays -1");
    }
    ok(dtwPresetFor("ggml-large-v3-turbo.bin") == "large.v3.turbo" && dtwPresetFor("ggml-large-v3.bin") == "large.v3"
       && dtwPresetFor("ggml-large-v3-turbo-q5_0.bin") == "large.v3.turbo" && dtwPresetFor("ggml-small.bin") == "small"
       && dtwPresetFor("ggml-base.en.bin") == "base.en" && dtwPresetFor("D:\\x\\GGML-MEDIUM.BIN") == "medium"
       && dtwPresetFor("my-finetune.bin").empty() && dtwPresetFor("ggml-large-v3-turbo-german.bin").empty(),
       "the DTW preset follows the model file (quantised copies included); an unknown model gets none");
    // ---- C: whisper's TOKENS become WORDS -----------------------------------------------------------
    {
        struct TC { const char* cur; const char* tok; bool glue; };
        static const TC tc[] = {
            { " Me", "ep", true }, { " c", "'", true }, { " c'", "est", true }, { " c'", " est", true },
            { " qu'", " il", true }, { " jusqu'", " ici", true }, { " \xc3\x8ates", "-", true },
            { " \xc3\x8ates-", "vous", true }, { " \xc3\x8ates-", " vous", true }, { " va", " -t-il", true },
            { " croy", "ant", true }, { " ciel", " ?", true }, { " We", "'ll", true }, { " F", "-", true },
            { " -", " Il", true }, { " l\xc3\xa0 !", " -", false }, { " chose", " dans", false },
            { "\xe6\x8a\x8a", "re", false }, { " re", "verb", true }, { " reverb", "\xe5\x85\xb3", false },
            { "\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x8c", "OK", false }, { " \xd0\xa2", "\xd0\xa0\xd0\x95", true },
            { " students'", " books", false }, { " va", " (", false }, { " Bonjour", " \xc2\xab", false },
            { " \xc2\xab", "Bon", true },
            // review P4: an interrupted word, then the next speaker -- never one word; L2: Korean is spaced
            { " si-", " Non", false }, { " gonna-", " Stop!", false }, { " si-", " non", true },
            { " \xec\x95\x88\xeb\x85\x95", "\xed\x95\x98\xec\x84\xb8\xec\x9a\x94", true },
            { " \xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94", " \xec\x97\xac", false },
        };
        int bad = 0;
        for (const auto& t : tc)
            if (tokenContinuesWord(t.cur, t.tok) != t.glue) {
                ++bad;
                std::printf("    word rule: [%s] + [%s] -> %s (want %s)\n", t.cur, t.tok,
                            t.glue ? "split" : "glued", t.glue ? "glued" : "split");
            }
        ok(bad == 0, "C: a token with no space continues its word (any alphabet, never Han/kana); elisions and "
                     "hyphens glue across a space; punctuation rides; an opening mark / dialogue dash opens");
        const std::vector<WindowWord> nw = windowWords(next, 0.0);
        std::string all;
        for (const auto& x : nw) all += "|" + oneLine(x.w.text);
        std::printf("    kFilmNext words: %s\n", all.c_str());
        ok(nw.size() == 21 && oneLine(nw[0].w.text) == "B-A-7," && oneLine(nw[1].w.text) == "c'est"
           && all.find("|l'ouvrir.|") != std::string::npos && all.find("|ferm\xc3\xa9" "e.|") != std::string::npos
           && all.find("|Permission|donn\xc3\xa9" "e.") != std::string::npos && all.find("|noir ?|") != std::string::npos,
           "C: the recorded film tokens make whole words (B-A-7, | c'est | l'ouvrir. | ferm\xc3\xa9" "e. | Permission | noir ?)");
        // a segment ending INSIDE a word ("...une catapulte en c" | "royant atterrir"): one word across it
        AsrResult split;
        parseAsrResponse("{\"text\":\" en croyant atterrir\",\"language\":\"french\",\"segments\":["
                         "{\"id\":0,\"text\":\" en c\",\"start\":0.0,\"end\":1.0,\"words\":["
                         "{\"word\":\" en\",\"start\":0.0,\"end\":0.5,\"t_dtw\":-1,\"probability\":0.9},"
                         "{\"word\":\" c\",\"start\":0.5,\"end\":1.0,\"t_dtw\":-1,\"probability\":0.9}],\"temperature\":0.0},"
                         "{\"id\":1,\"text\":\"royant atterrir\",\"start\":1.0,\"end\":2.0,\"words\":["
                         "{\"word\":\"royant\",\"start\":1.0,\"end\":1.4,\"t_dtw\":-1,\"probability\":0.9},"
                         "{\"word\":\" atterrir\",\"start\":1.4,\"end\":2.0,\"t_dtw\":-1,\"probability\":0.9}],\"temperature\":0.0}]}",
                         split);
        const std::vector<WindowWord> sw = windowWords(split, 0.0);
        ok(sw.size() == 3 && oneLine(sw[1].w.text) == "croyant" && sw[1].seg == 0 && std::fabs(sw[1].w.end - 1.4) < 1e-9,
           "C: a segment that ends INSIDE a word still gives ONE word (croy|ant), owned by its first segment");
        // ... Cyrillic tokens too: the cache's "U+0422 | U+0420,0415 | U+0412,041E,0416 | U+041D,0410,042F | U+041C,0423,0417,042B,041A,0410" is two words
        AsrResult cy;
        parseAsrResponse("{\"text\":\" \xd0\xa2\xd0\xa0\xd0\x95\xd0\x92\xd0\x9e\xd0\x96\xd0\x9d\xd0\x90\xd0\xaf \xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90\","
                         "\"language\":\"russian\",\"segments\":[{\"id\":0,\"text\":\" \xd0\xa2\xd0\xa0\xd0\x95\xd0\x92\xd0\x9e\xd0\x96\xd0\x9d\xd0\x90\xd0\xaf "
                         "\xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90\",\"start\":0.0,\"end\":25.0,\"words\":["
                         "{\"word\":\" \xd0\xa2\",\"start\":0.0,\"end\":2.0,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\"\xd0\xa0\xd0\x95\",\"start\":2.1,\"end\":4.9,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\"\xd0\x92\xd0\x9e\xd0\x96\",\"start\":5.0,\"end\":10.3,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\"\xd0\x9d\xd0\x90\xd0\xaf\",\"start\":10.4,\"end\":15.0,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\" \xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90\",\"start\":15.2,\"end\":25.0,\"t_dtw\":-1,\"probability\":0.5}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.3}],\"detected_language\":\"russian\"}", cy);
        const std::vector<WindowWord> cw = windowWords(cy, 0.0);
        ok(cw.size() == 2 && isSoundTag(cy.segments.front().text),
           "C+D: \"\xd0\xa2\xd0\xa0\xd0\x95\xd0\x92\xd0\x9e\xd0\x96\xd0\x9d\xd0\x90\xd0\xaf \xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90\" is two words -- and a sound tag (never a subtitle)");
    }
    // ---- A: the DTW clock on the recorded film window ---------------------------------------------------
    {
        const std::vector<WindowWord> nw = windowWords(next, 0.0);
        bool timed = nw.size() == 21, mono = true;
        for (size_t i = 0; i < nw.size(); ++i) {
            timed = timed && nw[i].w.timed;
            mono = mono && nw[i].w.end > nw[i].w.start && (i == 0 || nw[i].w.start >= nw[i - 1].w.start);
        }
        const double cest = nw.size() > 1 ? nw[1].w.start : -1.0, jen = nw.size() > 8 ? nw[8].w.start : -1.0;
        std::printf("    DTW: \"c'est\" %.2f-%.2f s, \"J'en\" %.2f-%.2f s (the independent alignment: 1.48 / 2.68 into "
                    "this window)\n", cest, nw.size() > 1 ? nw[1].w.end : -1.0, jen, nw.size() > 8 ? nw[8].w.end : -1.0);
        ok(timed && mono && std::fabs(cest - 1.46) < 0.011 && std::fabs(nw[1].w.end - 1.64) < 0.011
           && std::fabs(jen - 2.69) < 0.011 && oneLine(nw[8].w.text) == "J'en",
           "A: words timed by DTW -- a word ENDS at its last token's t_dtw and starts where the one before "
           "ended (c'est 1.46-1.64 s, J'en from 2.69 s; t_dtw read as a START would be a token late)");
        const std::vector<WindowWord> hw = windowWords(hard, 0.0);
        std::printf("    DTW: \"%s\" %.2f-%.2f s (its first token's t_dtw: 13.60 s)\n", hw.empty() ? "" : oneLine(hw[0].w.text).c_str(),
                    hw.empty() ? -1.0 : hw[0].w.start, hw.empty() ? -1.0 : hw[0].w.end);
        ok(hw.size() == 7 && oneLine(hw[0].w.text) == "C'est" && hw[0].w.start > 27.8 && hw[0].w.start < 28.0,
           "A: a first token the path entered 13.6 s early (over music) cannot stretch its word: "
           "\"C'est\" starts within a word's length of its end (27.89 s, not 13.41 s)");
        const std::vector<WindowWord> uw = windowWords([&] { AsrResult r; parseAsrResponse(noDtw(kFilmHard), r); return r; }(), 0.0);
        ok(uw.size() == 7 && !uw[0].w.timed && std::fabs(uw[0].w.start - 0.0) < 1e-9 && std::fabs(uw[0].w.end - 7.78) < 1e-9,
           "A: without DTW the words keep whisper's own times (t0 of the first token, t1 of the last)");
    }
    // ---- B: the duplicate's ROOT -- a hard cut whose only segment is a sentence said at its very end ------
    {
        CommitInput ci;
        ci.asr = &hard;
        ci.plan = plan(320.26, 350.26, WindowPlan::kHard);
        ci.langA = "fr"; ci.langB = "en";
        const CommitVerdict cv = commitWindow(ci);
        const double resume = (double)cv.nextCursor / kFileRate;
        std::printf("    hard window 320.26-350.26 (one segment, its speech in the last 1.5 s): %zu word(s) committed, "
                    "held back=%d, next window from %.2f s\n", cv.words.size(), cv.heldBack ? 1 : 0, resume);
        ok(cv.timed && cv.words.empty() && cv.heldBack && resume > 347.4 && resume < 348.4,
           "B: a hard cut commits nothing of a sentence that only began late in the window -- the next window "
           "starts on it (less a breath) and hears it whole");
        // ... the cue stream of the two windows in order: the sentence ONCE, where it is said
        CueBuilder cb;
        CueCfg cc;
        cb.configure(cc, nullptr);
        cb.addWords(cv.words);
        CommitInput c2;
        c2.asr = &next;
        c2.plan = plan(347.76, 369.79, WindowPlan::kPause);
        c2.langA = "fr"; c2.langB = "en";
        const CommitVerdict v2 = commitWindow(c2);
        cb.addWords(v2.words);
        cb.finish(369.79);
        std::vector<FileCue> cues;
        FileCue c;
        while (cb.pop(c)) cues.push_back(c);
        int noir = 0;
        for (const auto& q : cues) {
            std::printf("    cue %7.2f-%7.2f  %s\n", q.start, q.end, q.text.c_str());
            for (size_t at = 0; (at = q.text.find("noir", at)) != std::string::npos; ++at) ++noir;
        }
        ok(cues.size() == 4 && noir == 1 && cues[0].text == "B-A-7, c'est quoi cette ombre dans le noir ?"
           && cues[0].start > 348.6 && cues[0].start < 349.0,
           "B: the sentence is shown ONCE, starting where it is said (348.77 s; the independent alignment: 348.88)");
        // what the pre-WF clock did with the same window (no assert: the record of the defect)
        AsrResult old;
        parseAsrResponse(noDtw(kFilmHard), old);
        CommitInput co = ci;
        co.asr = &old;
        const CommitVerdict ov = commitWindow(co);
        std::string ot;
        for (const auto& w : ov.words) ot += w.text;
        std::printf("    (the same window on whisper's token times: committed \"%s\" from %.2f s -- the first copy of "
                    "the user's duplicate)\n", oneLine(ot).c_str(), ov.words.empty() ? 0.0 : ov.words.front().start);
    }
    // ---- B: the SAFETY de-dup ----------------------------------------------------------------------------
    {
        auto W = [](double s, double e, const char* t, bool timed) { FileWord w{ s, e, t, "fr" }; w.timed = timed; return w; };
        std::deque<FileWord> recent = { W(349.9, 350.1, " le", true), W(350.1, 350.31, " noir ?", true) };
        std::vector<FileWord> fresh = { W(349.95, 350.1, " le", true), W(350.1, 350.3, " noir ?", true),
                                        W(350.46, 350.73, " J'en", true) };
        const size_t d1 = dropAlreadyCommitted(fresh, recent);
        ok(d1 == 2 && fresh.size() == 1 && oneLine(fresh[0].text) == "J'en",
           "B: DTW-timed words starting before the last committed word ended were heard twice -- dropped");
        const char* kSent[] = { " c'est", " quoi", " cette", " ombre", " dans", " le", " noir ?" };
        std::deque<FileWord> goodOld;
        std::vector<FileWord> slowNew;
        for (int i = 0; i < 7; ++i) {
            goodOld.push_back(W(348.9 + 0.2 * i, 349.1 + 0.2 * i, kSent[i], true));
            slowNew.push_back(W(352.0 + 4.0 * i, 355.9 + 4.0 * i, kSent[i], false));   // 28 letters over 28 s
        }
        std::vector<FileWord> a1 = slowNew;
        ok(dropAlreadyCommitted(a1, goodOld) == 7 && a1.empty(),
           "B: a second copy spread far beyond any speech rate (< 4 ch/s) of a sentence just committed -- dropped");
        std::deque<FileWord> slowOld(slowNew.begin(), slowNew.end());
        std::vector<FileWord> goodNew;
        for (int i = 0; i < 7; ++i) goodNew.push_back(W(381.0 + 0.2 * i, 381.2 + 0.2 * i, kSent[i], true));
        ok(dropAlreadyCommitted(goodNew, slowOld) == 0 && goodNew.size() == 7,
           "B: a WELL-TIMED copy of a badly timed one is kept (the viewer gets the sentence where it is said)");
        // a person repeating himself, both plausible: kept ("L'essai numero 1, 2, 3D... Je repete, l'essai 1, 2, 3D...")
        const char* kA[] = { " L'essai", " num\xc3\xa9ro", " 1,", " 2,", " 3D", " commence", " dans", " dix", " minutes." };
        const char* kB[] = { " Je", " r\xc3\xa9p\xc3\xa8te,", " l'essai", " 1,", " 2,", " 3D", " commence", " dans", " dix", " minutes." };
        std::deque<FileWord> ra;
        for (int i = 0; i < 9; ++i) ra.push_back(W(67.7 + 0.35 * i, 68.0 + 0.35 * i, kA[i], true));
        std::vector<FileWord> rb;
        for (int i = 0; i < 10; ++i) rb.push_back(W(71.5 + 0.32 * i, 71.8 + 0.32 * i, kB[i], true));
        ok(dropAlreadyCommitted(rb, ra) == 0 && rb.size() == 10, "B: a real repeat (both copies plausibly timed) is kept");
    }
    // ---- D: the language stays INSIDE the pair -------------------------------------------------------
    {
        ok(isCreditsHallucination("Sous-titrage Soci\xc3\xa9t\xc3\xa9 Radio-Canada") && isCreditsHallucination("Sous-titrage ST' 501")
           && isCreditsHallucination("Transcription by ESO. Translation by \xe2\x80\x94")
           && isCreditsHallucination("Merci d'avoir regard\xc3\xa9 cette vid\xc3\xa9o !") && isCreditsHallucination(ru.text)
           && !isCreditsHallucination("Le courant finira par revenir avant la nuit.")
           && !isCreditsHallucination("Sous le titre, il y a une date."),
           "D: whisper's training-set credits are never dialogue (FR / EN / RU lines from the film); real lines are");
        ok(isSoundTag("\xd0\xa2\xd0\xa0\xd0\x95\xd0\x92\xd0\x9e\xd0\x96\xd0\x9d\xd0\x90\xd0\xaf \xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90")
           && isSoundTag("\xd0\x90\xd0\x9f\xd0\x9b\xd0\x9e\xd0\x94\xd0\x98\xd0\xa1\xd0\x9c\xd0\x95\xd0\x9d\xd0\xa2\xd0\xab")
           && isSoundTag("MUSIQUE DOUCE") && isSoundTag("Music") && isSoundTag("Musique") && isSoundTag("\xe9\x9f\xb3\xe4\xb9\x90")
           && isSoundTag("[Musique]") && isSoundTag("*Mario's music*") && isSoundTag("TRIPT CREDITS TRIPT CREDITS")
           && !isSoundTag("ALPHA") && !isSoundTag("TNT") && !isSoundTag("NON !") && !isSoundTag("Bip bip !")
           && !isSoundTag("La musique est trop forte.") && !isSoundTag("Meep meep!") && isSoundTag("CHEERS")
           // review L5: SPOKEN -- sentence punctuation -- a tag word is dialogue; and exact words, never stems
           && !isSoundTag("Musique !") && !isSoundTag("CHEERS!") && !isSoundTag("Rires.") && !isSoundTag("SIGHT")
           && !isSoundTag("SOUNDS GOOD") && !isSoundTag("Music!"),
           "D: sound tags in any language (bare ALL-CAPS ones included) are dropped; ALPHA / TNT / NON ! / a sentence are "
           "not, nor a tag word SAID (\"Musique !\", \"CHEERS!\" -- review L5)");
        ok(foreignScript(ru.text, "fr", "en") && foreignScript("\xe4\xbd\xa0\xe6\x8a\x8a\xe5\xa5\xb9\xe9\x80\x83\xe9\x81\xbf\xe4\xba\x86", "fr", "en")
           && !foreignScript("C'est parti !", "fr", "en") && !foreignScript("\xe4\xbd\xa0\xe5\xa5\xbd", "zh", "en")
           && !foreignScript("reverb", "zh-tw", "en") && !foreignScript("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "ru", "en")
           && !foreignScript("anything", "xx", "en"),
           "D: a line written in a script neither language of the pair uses is not the job's speech (ru / zh in fr<->en)");
        CommitInput ci;
        ci.asr = &ru;
        ci.plan = plan(21.34, 33.79, WindowPlan::kPause);
        ci.langA = "fr"; ci.langB = "en";
        const CommitVerdict cv = commitWindow(ci);
        ok(cv.words.empty() && cv.junkSegments == 1 && (cv.tags + cv.foreign) == 1,
           "D: the recorded ru window commits nothing in a fr<->en job");
        // dropEdgeAnnotations: "Meep meep! *Gasp*" keeps the speech only
        std::vector<FileWord> ew = { FileWord{ 0.0, 0.3, " Meep", "en" }, FileWord{ 0.3, 0.6, " meep!", "en" },
                                     FileWord{ 0.7, 1.0, " *Gasp*", "en" } };
        dropEdgeAnnotations(ew);
        std::vector<FileWord> mid = { FileWord{ 0.0, 0.3, " I", "en" }, FileWord{ 0.3, 0.6, " like", "en" },
                                      FileWord{ 0.6, 0.9, " *this*", "en" }, FileWord{ 0.9, 1.2, " one.", "en" } };
        dropEdgeAnnotations(mid);
        std::vector<FileWord> lead = { FileWord{ 0.0, 0.3, " [Musique", "fr" }, FileWord{ 0.3, 0.6, " douce]", "fr" },
                                       FileWord{ 0.7, 1.0, " Bonjour.", "fr" } };
        dropEdgeAnnotations(lead);
        ok(ew.size() == 2 && mid.size() == 4 && lead.size() == 1 && oneLine(lead[0].text) == "Bonjour.",
           "D: an annotation opening or closing a line goes (\"*Gasp*\", \"[Musique douce]\"); emphasis inside a line stays");
        // the pair language a window heard outside the pair is decoded again in
        Routing rt;
        rt.mode = "auto"; rt.langA = "fr"; rt.langB = "en";
        ok(pairLangOf("ru", rt).empty() && pairLangOf("en", rt) == "en" && pairLangOf("", rt).empty(),
           "D: whisper's ru / pt / zh are outside a fr<->en pair");
        Routing rz;
        rz.mode = "auto"; rz.langA = "en"; rz.langB = "zh-tw";
        ok(pairLangOf("zh", rz) == "zh-tw" && pairLangOf("yue", rz) == "zh-tw" && whisperLangCode("zh-tw") == "zh",
           "D: whisper's one Mandarin stands for the pair's Chinese (zh-tw is forced as zh)");
        std::unique_ptr<FileJob> fj(new FileJob);
        fj->r = rt;
        const std::string firstGuess = plausiblePairLang(*fj, ru);         // nothing said yet: whisper's own odds
        fj->ctx.lastFinalLang = "fr";
        const std::string byLast = plausiblePairLang(*fj, ru);
        fj->langWords["fr"] = 40; fj->langWords["en"] = 3;
        fj->ctx.lastFinalLang = "en";
        const std::string byDominant = plausiblePairLang(*fj, ru);
        fj->r.mode = "b2a";
        const std::string byLock = plausiblePairLang(*fj, ru);
        ok(firstGuess == "en" && byLast == "fr" && byDominant == "fr" && byLock == "en",
           "D: the forced language = the lock's source; else a one-language file's language (>= 90 % of >= 20 words); "
           "else whisper's own pair odds when DECISIVE (>= 3x: not here, en 0.20 vs fr 0.09); else the leading "
           "language; else the previous line's; else the likelier pair language");
        // ... and through the SHIPPING job lap: the ru window is decoded AGAIN, forced, and its text is the cue
        const std::wstring cmp3 = testFixtureC();
        if (cmp3.empty()) {
            std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): the job-lap re-decode (fixture c.mp3)\n");
        } else {
            std::vector<std::string> sent;
            g_fileNoModelsForTest = true;
            g_fileAsrForTest = [&](uint64_t s, uint64_t, const std::string&) -> std::string {
                sent.push_back(g_fileAsrLangSentForTest);
                if (s > 5 * kFileRate) return "{\"text\":\"\",\"language\":\"english\",\"segments\":[]}";
                if (g_fileAsrLangSentForTest == "auto") return kFilmRu;   // heard as Russian
                const bool en = g_fileAsrLangSentForTest == "en";
                return std::string("{\"text\":\"") + (en ? " I will call for help." : " Je vais lancer un appel.")
                     + "\",\"language\":\"" + (en ? "english" : "french") + "\",\"segments\":[{\"id\":0,\"text\":\""
                     + (en ? " I will call for help." : " Je vais lancer un appel.") + "\",\"start\":0.2,\"end\":1.8,\"words\":["
                     "{\"word\":\"" + (en ? " I" : " Je") + "\",\"start\":0.2,\"end\":0.5,\"t_dtw\":50,\"probability\":0.9},"
                     "{\"word\":\"" + (en ? " will" : " vais") + "\",\"start\":0.5,\"end\":0.8,\"t_dtw\":80,\"probability\":0.9},"
                     "{\"word\":\"" + (en ? " call" : " lancer") + "\",\"start\":0.8,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                     "{\"word\":\"" + (en ? " for help." : " un appel.") + "\",\"start\":1.2,\"end\":1.8,\"t_dtw\":180,\"probability\":0.9}],"
                     "\"temperature\":0.0,\"avg_logprob\":-0.1}]}";
            };
            g_fileMtForTest = [](const std::string& src, const std::string&, const std::string&, size_t) { return "T(" + src + ")"; };
            const JobRun r = runFileJob(srv, cfg, 7701, wideToUtf8L(cmp3), "fr", "en", "auto", "auto", true);
            g_fileAsrForTest = nullptr;
            g_fileMtForTest = nullptr;
            g_fileNoModelsForTest = false;
            std::string seq;
            for (const auto& l : sent) seq += l + " ";
            std::printf("    job lap: languages sent: %s; %u forced; %zu cue(s)\n", seq.c_str(), r.st.langRedecodes, r.cues.size());
            const bool forcedThen = sent.size() >= 2 && sent[0] == "auto" && (sent[1] == "en" || sent[1] == "fr");
            bool cyr = false, line = false;
            for (const auto& q : r.cues) {
                cyr = cyr || foreignScript(q.src, "fr", "en");
                line = line || (q.lang == sent[1] && (q.src == "I will call for help." || q.src == "Je vais lancer un appel."));
            }
            ok(forcedThen && r.st.langRedecodes == 1 && line && !cyr && !r.dones.empty() && r.dones.back().result == "ok",
               "D: through the job lap, a window heard in Russian is decoded again FORCED into the pair, and only that "
               "decode's words become cues");
        }
    }
    // ---- E: readability -----------------------------------------------------------------------------------
    {
        auto T = [](double s, double e, const char* t) { FileWord w{ s, e, t, "fr" }; w.timed = true; return w; };
        auto cuesOf = [&](const std::vector<FileWord>& ws, double endSec) {
            CueBuilder b;
            CueCfg cc;
            b.configure(cc, nullptr);
            b.addWords(ws);
            b.finish(endSec);
            std::vector<FileCue> out;
            FileCue c;
            while (b.pop(c)) out.push_back(c);
            return out;
        };
        const std::vector<FileCue> c1 = cuesOf({ T(1.0, 1.2, " Oui."), T(6.0, 6.5, " Bon."), }, 20.0);
        // 72 letters + 13 spaces said in 2 s: its reading time is 5 s at 17 characters a second
        std::vector<FileWord> lw;
        const char* kLong[] = { " Avez-vous", " d\xc3\xa9j\xc3\xa0", " tir\xc3\xa9", " sur", " le", " cordon", " de", " votre",
                                " parachute", " et", " vu", " qu'il", " ne", " contenait", " que", " des", " ustensiles." };
        for (int i = 0; i < 17; ++i) lw.push_back(T(10.0 + 0.12 * i, 10.12 + 0.12 * i, kLong[i]));
        std::vector<FileWord> lw2 = lw;
        const std::vector<FileCue> c2 = cuesOf(lw, 40.0);
        lw2.push_back(T(13.2, 13.6, " Non."));                           // the next line 1 s after the last word
        const std::vector<FileCue> c3 = cuesOf(lw2, 40.0);
        const double read = c2.empty() ? 0.0 : (double)(c2[0].text.size()) / 17.0;
        for (const auto& q : c2) std::printf("    E: cue %5.2f-%5.2f (%zu bytes)  %s\n", q.start, q.end, q.text.size(), q.text.c_str());
        ok(c1.size() == 2 && c1[0].end - c1[0].start >= 1.0 - 1e-9 && c1[1].end - c1[1].start >= 1.0 - 1e-9,
           "E: a one-word line stays up at least 1 s when the gap allows");
        ok(c2.size() == 1 && c2[0].end - c2[0].start >= read - 0.3,
           "E: a long line said fast stays up for its reading time (17 characters a second)");
        ok(c3.size() == 2 && c3[0].end <= c3[1].start - 0.079 && c3[0].end > 13.0,
           "E: ... into the gap, never over the next cue");
        // fragments: forward ("Soit" + 0.8 s + the rest), backward ("...soit on te" + 0.9 s + "laisse."),
        // and a real one-word reply after a full stop stays alone
        const std::vector<FileCue> f1 = cuesOf({ T(0.0, 0.4, " Soit"), T(1.2, 1.4, " tu"), T(1.4, 1.8, " restes,"),
                                                 T(1.8, 2.0, " soit"), T(2.0, 2.1, " on"), T(2.1, 2.2, " te"),
                                                 T(2.2, 2.6, " laisse.") }, 10.0);
        const std::vector<FileCue> f2 = cuesOf({ T(0.0, 0.4, " Soit"), T(0.4, 0.6, " tu"), T(0.6, 1.0, " restes,"),
                                                 T(1.0, 1.2, " soit"), T(1.2, 1.3, " on"), T(1.3, 3.2, " te"),
                                                 T(4.1, 4.5, " laisse.") }, 10.0);
        const std::vector<FileCue> f3 = cuesOf({ T(0.0, 0.4, " C'est"), T(0.4, 0.8, " fini."), T(1.7, 2.0, " Oui."),
                                                 T(5.0, 5.3, " Bon.") }, 10.0);
        for (const auto* v : { &f1, &f2, &f3 })
            for (const auto& q : *v) std::printf("    E: fragment test cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
        ok(f1.size() == 1 && f1[0].text == "Soit tu restes, soit on te laisse.",
           "E: a fragment that does not end its sentence joins the line after it (gap < 1 s)");
        ok(f2.size() == 1 && f2[0].text == "Soit tu restes, soit on te laisse.",
           "E: a fragment ending the sentence of the cue before it joins that cue (gap < 1 s, still here)");
        ok(f3.size() == 3 && f3[1].text == "Oui.", "E: a one-word reply after a full stop is a cue of its own");
        // a two-word opening said slowly (0.7 s), then a 0.9 s pause: split off by the gap -- and joined again
        const std::vector<FileCue> f4 = cuesOf({ T(0.0, 0.35, " Alors"), T(0.35, 0.7, " donc"), T(1.6, 1.8, " on"),
                                                 T(1.8, 1.9, " y"), T(1.9, 2.2, " va"), T(2.2, 2.5, " tous"),
                                                 T(2.5, 3.1, " ensemble.") }, 10.0);
        for (const auto& q : f4) std::printf("    E: fragment test cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
        ok(f4.size() == 1 && f4[0].text == "Alors donc on y va tous ensemble.",
           "E: a two-word opening split off by a pause (no sentence end) joins the line after it");
        // a list read out, each name 0.5 s and the next one at once: ONE cue rather than four flashes
        const std::vector<FileCue> l4 = cuesOf({ T(0.0, 0.5, " Avery."), T(0.58, 1.08, " Jones."),
                                                 T(1.16, 1.66, " Saltaise."), T(1.74, 2.24, " Justice."),
                                                 T(6.0, 6.5, " Bon.") }, 10.0);
        for (const auto& q : l4) std::printf("    E: rapid-list cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
        bool l4ok = l4.size() == 3 && l4[2].text == "Bon.";
        for (const auto& q : l4) l4ok = l4ok && q.end - q.start >= 1.0 - 1e-9;
        ok(l4ok, "E: lines that cannot stay up 1 s (the next one at once) share a cue when they fit together "
                 "(four names read out: two cues of two, each >= 1 s)");
        // a long fluent sentence (no comma, no pause: every DTW gap 0) is never split after its first word
        std::vector<FileWord> fl;
        const char* kFl[] = { " Je", " vais", " mettre", " tous", " mes", " meilleurs", " avocats", " \xc3\xa0", " temps",
                              " plein", " l\xc3\xa0-dessus", " pour", " m'assurer", " que", " ton", " nom", " est", " encore",
                              " plus", " connu." };
        for (int i = 0; i < 20; ++i) fl.push_back(T(0.25 * i, 0.25 * (i + 1), kFl[i]));
        const std::vector<FileCue> fc = cuesOf(fl, 10.0);
        for (const auto& q : fc) std::printf("    E: long-line cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
        bool noFrag = fc.size() >= 2;
        for (const auto& q : fc) noFrag = noFrag && normWords(q.text).size() >= 4;
        ok(noFrag, "E: an over-long fluent sentence is split where both halves stay readable (never \"Je vais\" | ...)");
        // UNDER MUSIC (the energy active everywhere): the DTW gap is the pause, and a DTW start is not moved
        // back onto the energy run's onset (a short dip 0.3 s before it)
        {
            std::vector<float> x((size_t)(20 * kFileRate));
            uint32_t rng = 4242u;
            for (auto& v : x) { rng = rng * 1664525u + 1013904223u; v = ((float)(rng >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.15f; }
            EnergyTrack music;
            music.push(x.data(), x.size());
            for (size_t i = (size_t)(4.6 * kFileRate); i < (size_t)(4.7 * kFileRate); ++i) x[i] = 0.0f;
            EnergyTrack dip;
            dip.push(x.data(), x.size());
            auto cuesOn = [&](const EnergyTrack& en, const std::vector<FileWord>& ws) {
                CueBuilder b;
                CueCfg cc;
                b.configure(cc, &en);
                b.addWords(ws);
                b.finish(20.0);
                std::vector<FileCue> out;
                FileCue c;
                while (b.pop(c)) out.push_back(c);
                return out;
            };
            const std::vector<FileCue> g1 = cuesOn(music, { T(1.0, 1.3, " Soit"), T(1.3, 1.4, " tu"), T(1.4, 1.8, " restes,"),
                                                            T(1.8, 2.0, " soit"), T(2.0, 2.1, " on"), T(2.1, 2.2, " te"),
                                                            T(2.2, 2.6, " laisse"), T(3.7, 3.9, " La"), T(3.9, 4.3, " nuit"),
                                                            T(4.3, 4.6, " finira."), });
            const std::vector<FileCue> g2 = cuesOn(dip, { T(3.0, 3.4, " Je"), T(3.4, 3.6, " ne"), T(3.6, 4.0, " sais"),
                                                          T(4.0, 4.4, " pas"), T(4.45, 5.0, " pourquoi."),
                                                          T(5.0, 5.3, " Qu'est-ce"), T(5.3, 5.5, " que"), T(5.5, 5.8, " tu"),
                                                          T(5.8, 6.2, " fais ?") });
            for (const auto* v : { &g1, &g2 })
                for (const auto& q : *v) std::printf("    E: under music cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
            ok(g1.size() == 2 && g1[1].text == "La nuit finira.",
               "E: under music (no energy pause anywhere) a 1.1 s DTW gap still splits two sentences");
            ok(g2.size() == 2 && std::fabs(g2[1].start - 5.0) < 0.011,
               "E: a DTW start is not snapped back onto an energy onset inside the sentence before it");
        }
    }
    // ---- D, through commitWindow: an annotation inside a line, and a junk segment's tokens -------------
    {
        AsrResult gasp, two;
        parseAsrResponse("{\"text\":\" Meep meep! *Gasp*\",\"language\":\"english\",\"segments\":[{\"id\":0,\"text\":"
                         "\" Meep meep! *Gasp*\",\"start\":1.0,\"end\":3.0,\"words\":["
                         "{\"word\":\" Me\",\"start\":1.0,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.2,\"end\":1.4,\"t_dtw\":140,\"probability\":0.9},"
                         "{\"word\":\" me\",\"start\":1.4,\"end\":1.6,\"t_dtw\":160,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.6,\"end\":1.8,\"t_dtw\":180,\"probability\":0.9},"
                         "{\"word\":\"!\",\"start\":1.8,\"end\":1.9,\"t_dtw\":185,\"probability\":0.9},"
                         "{\"word\":\" *\",\"start\":2.0,\"end\":2.1,\"t_dtw\":210,\"probability\":0.9},"
                         "{\"word\":\"G\",\"start\":2.1,\"end\":2.3,\"t_dtw\":230,\"probability\":0.9},"
                         "{\"word\":\"asp\",\"start\":2.3,\"end\":2.6,\"t_dtw\":260,\"probability\":0.9},"
                         "{\"word\":\"*\",\"start\":2.6,\"end\":3.0,\"t_dtw\":300,\"probability\":0.9}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1}],\"detected_language\":\"english\"}", gasp);
        parseAsrResponse("{\"text\":\" Meep meep! *Gasp*\",\"language\":\"english\",\"segments\":[{\"id\":0,\"text\":"
                         "\" Meep meep!\",\"start\":1.0,\"end\":1.9,\"words\":["
                         "{\"word\":\" Me\",\"start\":1.0,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.2,\"end\":1.4,\"t_dtw\":140,\"probability\":0.9},"
                         "{\"word\":\" me\",\"start\":1.4,\"end\":1.6,\"t_dtw\":160,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.6,\"end\":1.8,\"t_dtw\":180,\"probability\":0.9},"
                         "{\"word\":\"!\",\"start\":1.8,\"end\":1.9,\"t_dtw\":185,\"probability\":0.9}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1},{\"id\":1,\"text\":\" *Gasp*\",\"start\":1.9,\"end\":12.0,\"words\":["
                         "{\"word\":\" *\",\"start\":2.0,\"end\":2.1,\"t_dtw\":210,\"probability\":0.9},"
                         "{\"word\":\"G\",\"start\":2.1,\"end\":2.3,\"t_dtw\":230,\"probability\":0.9},"
                         "{\"word\":\"asp\",\"start\":2.3,\"end\":2.6,\"t_dtw\":260,\"probability\":0.9},"
                         "{\"word\":\"*\",\"start\":2.6,\"end\":3.0,\"t_dtw\":300,\"probability\":0.9}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1}],\"detected_language\":\"english\"}", two);
        // ... and a junk segment opening on a token that is NOT an opener (a music note): its tokens must not
        // ride on the last word of the line before it either
        AsrResult note;
        parseAsrResponse("{\"text\":\" Meep meep! \xe2\x99\xaa La la la \xe2\x99\xaa\",\"language\":\"english\",\"segments\":["
                         "{\"id\":0,\"text\":\" Meep meep!\",\"start\":1.0,\"end\":1.9,\"words\":["
                         "{\"word\":\" Me\",\"start\":1.0,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.2,\"end\":1.4,\"t_dtw\":140,\"probability\":0.9},"
                         "{\"word\":\" me\",\"start\":1.4,\"end\":1.6,\"t_dtw\":160,\"probability\":0.9},"
                         "{\"word\":\"ep\",\"start\":1.6,\"end\":1.8,\"t_dtw\":180,\"probability\":0.9},"
                         "{\"word\":\"!\",\"start\":1.8,\"end\":1.9,\"t_dtw\":185,\"probability\":0.9}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1},{\"id\":1,\"text\":\" \xe2\x99\xaa La la la \xe2\x99\xaa\",\"start\":1.9,\"end\":9.0,\"words\":["
                         "{\"word\":\" \xe2\x99\xaa\",\"start\":2.0,\"end\":2.1,\"t_dtw\":210,\"probability\":0.9},"
                         "{\"word\":\" La\",\"start\":2.1,\"end\":2.5,\"t_dtw\":250,\"probability\":0.9},"
                         "{\"word\":\" la\",\"start\":2.5,\"end\":3.0,\"t_dtw\":300,\"probability\":0.9},"
                         "{\"word\":\" la\",\"start\":3.0,\"end\":3.5,\"t_dtw\":350,\"probability\":0.9},"
                         "{\"word\":\" \xe2\x99\xaa\",\"start\":3.5,\"end\":3.6,\"t_dtw\":360,\"probability\":0.9}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1}],\"detected_language\":\"english\"}", note);
        std::string wg, wt, wn;
        for (const AsrResult* a : { &gasp, &two, &note }) {
            CommitInput ci;
            ci.asr = a;
            ci.plan.s = 0; ci.plan.e = (uint64_t)(12 * kFileRate); ci.plan.why = WindowPlan::kPause;
            ci.langA = "fr"; ci.langB = "en";
            const CommitVerdict cv = commitWindow(ci);
            std::string& out = a == &gasp ? wg : (a == &two ? wt : wn);
            for (const auto& w : cv.words) out += "|" + oneLine(w.text);
        }
        std::printf("    \"Meep meep! *Gasp*\" in one segment -> %s ; *Gasp* a junk segment of its own -> %s ; "
                    "then a sung junk segment -> %s\n", wg.c_str(), wt.c_str(), wn.c_str());
        ok(wg == "|Meep|meep!" && wt == "|Meep|meep!" && wn == "|Meep|meep!",
           "D: \"*Gasp*\" / \"\xe2\x99\xaa La la la \xe2\x99\xaa\" never reach a cue -- closing a line, or as a junk segment whose tokens "
           "must not ride on the line before it");
        // whisper LOOPING on a stuttered word with hyphens ("fleu-fleu-fleu-...", the film at 9:14): junk, and
        // two such segments make the window degenerate (decoded again)
        const std::string fl = "fleu-fleu-fleu-fleu-fleu-fleu-fleu-fleu-fleu-fleu-fleu-fleu-";
        ok(isRepetitionLoop(fl) && !isRepetitionLoop("Oui, merci, j'aimerais commander des fleu-fleu-fleu-fleu-")
           && !isRepetitionLoop("\xc3\x8ates-vous d\xc3\xa9j\xc3\xa0 mont\xc3\xa9 dans une catapulte en croyant atterrir ?"),
           "D: a hyphenated stutter loop (six identical pieces or more in ONE word) is a repetition loop");
        AsrResult lp;
        {
            std::string segs, text;
            auto seg = [&](int id, double s, double e, const std::string& t) {
                char b[160];
                std::snprintf(b, sizeof(b), "%s{\"id\":%d,\"text\":\"%s\",\"start\":%.2f,\"end\":%.2f,\"words\":[{\"word\":\"%s\","
                              "\"start\":%.2f,\"end\":%.2f,\"t_dtw\":-1,\"probability\":0.9}],\"temperature\":0.0,\"avg_logprob\":-0.1}",
                              segs.empty() ? "" : ",", id, t.c_str(), s, e, t.c_str(), s, e);
                segs += b;
                text += t;
            };
            seg(0, 0.0, 1.0, " Oui, merci.");
            for (int k = 1; k <= 4; ++k) seg(k, 1.0 + 0.1 * k, 1.1 + 0.1 * k, fl);
            parseAsrResponse("{\"text\":\"" + text + "\",\"language\":\"french\",\"segments\":[" + segs + "]}", lp);
        }
        CommitInput ci;
        ci.asr = &lp;
        ci.plan.s = 0; ci.plan.e = (uint64_t)(12 * kFileRate); ci.plan.why = WindowPlan::kPause;
        const CommitVerdict cl = commitWindow(ci);
        std::printf("    four looped segments after \"Oui, merci.\": degenerate=%d, %zu word(s), %d junk\n",
                    cl.degenerate ? 1 : 0, cl.words.size(), cl.junkSegments);
        ok(cl.degenerate && cl.words.size() == 1 && cl.junkSegments == 4,
           "D: four looped segments: dropped, and the window is decoded again");
        // B (efficiency): a HARD window whose only segment is ". . . . ." (the film's end credits) commits
        // nothing and moves on to its end -- before, it committed the dots and crawled one hop per window
        AsrResult dots;
        parseAsrResponse("{\"text\":\" . . . . . .\",\"language\":\"english\",\"segments\":[{\"id\":0,\"text\":"
                         "\" . . . . . .\",\"start\":0.0,\"end\":30.0,\"words\":["
                         "{\"word\":\" .\",\"start\":0.0,\"end\":0.0,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\" .\",\"start\":0.0,\"end\":0.0,\"t_dtw\":-1,\"probability\":0.5},"
                         "{\"word\":\" .\",\"start\":0.0,\"end\":0.0,\"t_dtw\":-1,\"probability\":0.5}],"
                         "\"temperature\":0.0,\"avg_logprob\":-0.1},{\"id\":1,\"text\":\"\",\"start\":0.0,\"end\":0.0,"
                         "\"words\":[],\"temperature\":0.0,\"avg_logprob\":0.0}],\"detected_language\":\"english\"}", dots);
        CommitInput cd;
        cd.asr = &dots;
        cd.plan.s = (uint64_t)(6053.29 * kFileRate); cd.plan.e = cd.plan.s + (uint64_t)(30 * kFileRate);
        cd.plan.why = WindowPlan::kHard;
        cd.langA = "fr"; cd.langB = "en";
        const CommitVerdict cdv = commitWindow(cd);
        std::printf("    hard window of dots: %zu word(s), next window from %.2f s (window %.2f-%.2f)\n", cdv.words.size(),
                    (double)cdv.nextCursor / kFileRate, (double)cd.plan.s / kFileRate, (double)cd.plan.e / kFileRate);
        ok(cdv.words.empty() && cdv.nextCursor == cd.plan.e,
           "B: a hard window of punctuation alone commits nothing and the next one starts at its end (no 30 ms crawl)");
        // B: the segment running into a hard cut is dropped (heard again whole by the next window) even when
        // whisper-server closes the window with an EMPTY segment -- 326 of the film's 370 hard windows
        AsrResult tail;
        parseAsrResponse("{\"text\":\" Bonjour \xc3\xa0 tous. Et voici la suite qui\",\"language\":\"french\",\"segments\":["
                         "{\"id\":0,\"text\":\" Bonjour \xc3\xa0 tous.\",\"start\":0.5,\"end\":5.0,\"words\":["
                         "{\"word\":\" Bonjour\",\"start\":0.5,\"end\":1.0,\"t_dtw\":100,\"probability\":0.9},"
                         "{\"word\":\" \xc3\xa0\",\"start\":1.0,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                         "{\"word\":\" tous\",\"start\":1.2,\"end\":1.6,\"t_dtw\":160,\"probability\":0.9},"
                         "{\"word\":\".\",\"start\":1.6,\"end\":1.7,\"t_dtw\":165,\"probability\":0.9}],\"temperature\":0.0,\"avg_logprob\":-0.1},"
                         "{\"id\":1,\"text\":\" Et voici la suite qui\",\"start\":27.0,\"end\":29.9,\"words\":["
                         "{\"word\":\" Et\",\"start\":27.0,\"end\":27.5,\"t_dtw\":2830,\"probability\":0.9},"
                         "{\"word\":\" voici\",\"start\":27.5,\"end\":28.0,\"t_dtw\":2880,\"probability\":0.9},"
                         "{\"word\":\" la\",\"start\":28.0,\"end\":28.5,\"t_dtw\":2910,\"probability\":0.9},"
                         "{\"word\":\" suite\",\"start\":28.5,\"end\":29.0,\"t_dtw\":2950,\"probability\":0.9},"
                         "{\"word\":\" qui\",\"start\":29.0,\"end\":29.9,\"t_dtw\":2990,\"probability\":0.9}],\"temperature\":0.0,\"avg_logprob\":-0.1},"
                         "{\"id\":2,\"text\":\"\",\"start\":0.0,\"end\":0.0,\"words\":[],\"temperature\":0.0,\"avg_logprob\":0.0}],"
                         "\"detected_language\":\"french\"}", tail);
        CommitInput ct;
        ct.asr = &tail;
        ct.plan.s = 0; ct.plan.e = (uint64_t)(30 * kFileRate); ct.plan.why = WindowPlan::kHard;
        ct.langA = "fr"; ct.langB = "en";
        const CommitVerdict ctv = commitWindow(ct);
        std::string tw;
        for (const auto& w : ctv.words) tw += "|" + oneLine(w.text);
        std::printf("    hard window + trailing empty segment: committed %s, next window from %.2f s\n", tw.c_str(),
                    (double)ctv.nextCursor / kFileRate);
        ok(tw == "|Bonjour|\xc3\xa0|tous." && ctv.nextCursor < (uint64_t)(2 * kFileRate),
           "B: the segment running into a hard cut is dropped although an empty segment closes the window");
    }
    // ---- THE REVIEW ROUND (REVIEW_WF.md, 11/10) ----------------------------------------------------------
    {
        // a whisper-server document from plain sentences: each segment's words are its space-separated
        // tokens, evenly over [s, e]; `dtw` = t_dtw at each token's end (else -1, a server without DTW)
        struct Sg { std::string text, json; };
        auto sg = [](const std::string& text, double s, double e, bool dtw) {
            std::vector<std::string> w;
            {
                std::string cur;
                for (char c : text) {
                    if (c == ' ') { if (!cur.empty()) w.push_back(cur); cur.clear(); }
                    else cur += c;
                }
                if (!cur.empty()) w.push_back(cur);
            }
            std::string ws;
            const double d = w.empty() ? 0.0 : (e - s) / (double)w.size();
            for (size_t i = 0; i < w.size(); ++i) {
                char b[128];
                const double ts = s + d * (double)i, te = s + d * (double)(i + 1);
                std::snprintf(b, sizeof(b), "\",\"start\":%.2f,\"end\":%.2f,\"t_dtw\":%d,\"probability\":0.9}", ts, te,
                              dtw ? (int)std::lround(te * 100.0) : -1);
                ws += std::string(ws.empty() ? "" : ",") + "{\"word\":\" " + w[i] + b;
            }
            char h[128];
            std::snprintf(h, sizeof(h), "\",\"start\":%.2f,\"end\":%.2f,\"words\":[", s, e);
            Sg out;
            out.text = text.empty() ? std::string() : " " + text;
            out.json = std::string("\"text\":\"") + out.text + h + ws + "],\"temperature\":0.0,\"avg_logprob\":-0.1}";
            return out;
        };
        auto docOf = [](const char* lang, double p, const std::vector<Sg>& segs) {
            std::string all, text;
            for (size_t i = 0; i < segs.size(); ++i) {
                all += (i ? ",{\"id\":" : "{\"id\":") + std::to_string(i) + "," + segs[i].json;
                text += segs[i].text;
            }
            char tail[160];
            std::snprintf(tail, sizeof(tail), "],\"detected_language\":\"%s\",\"detected_language_probability\":%.4f}", lang, p);
            return std::string("{\"text\":\"") + text + "\",\"language\":\"" + lang + "\",\"segments\":[" + all + tail;
        };
        const Sg empty = sg("", 0.0, 0.0, false);
        auto commitOf = [&](const std::string& doc, double s, double e, WindowPlan::Why why, const char* langC = "") {
            AsrResult a;
            parseAsrResponse(doc, a);
            CommitInput ci;
            ci.asr = &a;
            ci.plan = plan(s, e, why);
            ci.langA = "fr"; ci.langB = "en"; ci.langC = langC;
            return commitWindow(ci);
        };
        auto textOf = [](const std::vector<FileWord>& ws) {
            std::string t;
            for (const auto& w : ws) t += w.text;
            return oneLine(t);
        };
        // C1 -- a HARD window whose first segment is punctuation alone, then speech running past the cut,
        // then whisper's empty closing segment: the speech is committed up to its sentence end (timed), or
        // the next window comes back to it (held back) -- never skipped to the window's end
        {
            const CommitVerdict a = commitOf(docOf("french", 0.99, {
                sg("...", 0.0, 2.0, true),
                sg("Bonjour \xc3\xa0 tous, voici la suite. Et puis encore une phrase qui", 2.0, 29.9, true), empty }),
                100.0, 130.0, WindowPlan::kHard);
            const CommitVerdict b = commitOf(docOf("french", 0.99, {
                sg("...", 0.0, 2.0, true), sg("Et voici la suite qui", 22.0, 29.9, true), empty }),
                100.0, 130.0, WindowPlan::kHard);
            std::printf("    C1: dots + speech past a hard cut: committed \"%s\", next %.2f s; late speech: held back=%d, "
                        "next %.2f s\n", textOf(a.words).c_str(), (double)a.nextCursor / kFileRate, b.heldBack ? 1 : 0,
                        (double)b.nextCursor / kFileRate);
            ok(textOf(a.words) == "Bonjour \xc3\xa0 tous, voici la suite." && a.nextCursor < (uint64_t)(117.0 * kFileRate)
               && b.words.empty() && b.heldBack && b.nextCursor > (uint64_t)(121.5 * kFileRate)
               && b.nextCursor < (uint64_t)(124.0 * kFileRate),
               "C1: a hard window opening on punctuation alone still commits / comes back to the speech after it "
               "(it used to clear the dots AFTER the hard-cut rules and jump to the window's end: speech lost)");
            // ... and WITHOUT DTW, speech that only begins in the window's last second is committed (as 1.0.6 did)
            const CommitVerdict c = commitOf(docOf("french", 0.99, {
                sg("...", 0.0, 2.0, false), sg("Attends, je reviens.", 29.1, 29.9, false), empty }),
                100.0, 130.0, WindowPlan::kHard);
            const CommitVerdict d = commitOf(docOf("french", 0.99, { sg("Attends, je reviens.", 29.1, 29.9, false), empty }),
                                             100.0, 130.0, WindowPlan::kHard);
            std::printf("    C1, no DTW: \"%s\" / \"%s\" committed\n", textOf(c.words).c_str(), textOf(d.words).c_str());
            ok(textOf(c.words).find("Attends, je reviens.") != std::string::npos && textOf(d.words) == "Attends, je reviens."
               && c.nextCursor > (uint64_t)(129.0 * kFileRate) && d.nextCursor > (uint64_t)(129.0 * kFileRate),
               "C1: without DTW a hard window's last-second sentence is committed, never dropped");
        }
        // C2 / C3 -- credits ANCHORED at the line's start; the 13 pair languages; Amara.org anywhere
        {
            ok(creditsHallucination("Thanks for watching the kids, I owe you one.") == 0
               && creditsHallucination("Thank you for watching over her.") == 0
               && creditsHallucination("Merci d'avoir regard\xc3\xa9 les enfants.") == 0
               && creditsHallucination("Abonnez-vous au journal !") == 0
               && creditsHallucination("It's a translation by Baudelaire.") == 0
               && creditsHallucination("Il faut que tu partes, sous-titrage ou pas.") == 0,
               "C2: real lines that CONTAIN a credits phrase are dialogue (anchored at the line's start, a few words more)");
            ok(creditsHallucination("Untertitel der Amara.org-Community") == 1
               && creditsHallucination("Sottotitoli creati dalla comunit\xc3\xa0 Amara.org") == 1
               && creditsHallucination("Subt\xc3\xadtulos realizados por la comunidad de Amara.org") == 1
               && creditsHallucination("\xe5\xad\x97\xe5\xb9\x95\xe7\x94\xb1" "Amara.org\xe7\xa4\xbe\xe5\x8c\xba\xe6\x8f\x90\xe4\xbe\x9b") == 1
               && creditsHallucination("Untertitel im Auftrag des ZDF f\xc3\xbcr funk, 2017") == 1
               && creditsHallucination("\xe8\xaf\xb7\xe4\xb8\x8d\xe5\x90\x9d\xe7\x82\xb9\xe8\xb5\x9e \xe8\xae\xa2\xe9\x98\x85 \xe8\xbd\xac\xe5\x8f\x91 "
                                       "\xe6\x89\x93\xe8\xb5\x8f\xe6\x94\xaf\xe6\x8c\x81\xe6\x98\x8e\xe9\x95\x9c\xe4\xb8\x8e\xe7\x82\xb9\xe7\x82\xb9\xe6\xa0\x8f\xe7\x9b\xae") == 1
               && creditsHallucination("SOUS-TITRES R\xc3\x89" "ALIS\xc3\x89S PAR LA COMMUNAUT\xc3\x89 D'AMARA") == 1
               && creditsHallucination("Sous-titres par J\xc3\xa9r\xc3\xa9my Diaz") == 1
               && creditsHallucination("\xe3\x81\x94\xe8\xa6\x96\xe8\x81\xb4\xe3\x81\x82\xe3\x82\x8a\xe3\x81\x8c\xe3\x81\xa8\xe3\x81\x86"
                                       "\xe3\x81\x94\xe3\x81\x96\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f") == 2
               && creditsHallucination("\xec\x8b\x9c\xec\xb2\xad\xed\x95\xb4 \xec\xa3\xbc\xec\x85\x94\xec\x84\x9c "
                                       "\xea\xb0\x90\xec\x82\xac\xed\x95\xa9\xeb\x8b\x88\xeb\x8b\xa4") == 2
               && creditsHallucination("\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 "
                                       "\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd0\xb5\xd1\x82...") == 2
               && creditsHallucination("Thank you for watching.") == 2,
               "C3: the training-set credits of the pair languages (de it es zh ja ko ru, Amara.org anywhere, accented "
               "capitals) are caught; the ones a person could say are flagged for their context");
            // a credits line a person could say: dropped ALONE in its window or when whisper is unsure of the
            // language; kept among other speech confidently heard
            const Sg thanks = sg("Thank you for watching.", 0.5, 2.0, true);
            const Sg more = sg("Now let's get back to work.", 2.5, 4.5, true);
            const CommitVerdict alone = commitOf(docOf("english", 0.95, { thanks, empty }), 10.0, 20.0, WindowPlan::kPause);
            const CommitVerdict among = commitOf(docOf("english", 0.95, { thanks, more, empty }), 10.0, 20.0, WindowPlan::kPause);
            const CommitVerdict unsure = commitOf(docOf("english", 0.55, { thanks, more, empty }), 10.0, 20.0, WindowPlan::kPause);
            std::printf("    C2: \"Thank you for watching.\" alone -> \"%s\"; among speech -> \"%s\"; unsure -> \"%s\"\n",
                        textOf(alone.words).c_str(), textOf(among.words).c_str(), textOf(unsure.words).c_str());
            ok(alone.words.empty() && alone.tags == 1 && textOf(among.words) == "Thank you for watching. Now let's get back to work."
               && textOf(unsure.words) == "Now let's get back to work.",
               "C2: a sayable credits line goes when it stands alone or the language is unsure, stays among real speech");
        }
        // P1 -- a CONFIDENT third language holding real speech is kept as itself; "E ai! E ai!" over music is not
        {
            const std::string ruSent = "\xd0\xaf \xd0\xbd\xd0\xb5 \xd0\xb7\xd0\xbd\xd0\xb0\xd1\x8e, \xd1\x87\xd1\x82\xd0\xbe "
                                       "\xd1\x82\xd1\x8b \xd0\xb7\xd0\xb4\xd0\xb5\xd1\x81\xd1\x8c \xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0\xd0\xb5\xd1\x88\xd1\x8c.";
            const std::string ruDoc = docOf("russian", 0.95, { sg(ruSent, 0.3, 2.3, true), empty });
            const std::string ptDoc = docOf("portuguese", 0.99, { sg("E a\xc3\xad! E a\xc3\xad!", 0.3, 2.3, true), empty });
            ok(thirdLangSpeech(commitOf(ruDoc, 0.0, 10.0, WindowPlan::kPause, "ru"))
               && !thirdLangSpeech(commitOf(ruDoc, 0.0, 10.0, WindowPlan::kPause))
               && !thirdLangSpeech(commitOf(ptDoc, 0.0, 10.0, WindowPlan::kPause, "pt")),
               "P1: real speech in a third language (its script allowed) qualifies; \"E a\xc3\xad! E a\xc3\xad!\" does not");
            const std::wstring cmp3 = testFixtureC();
            if (cmp3.empty()) {
                std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): the job-lap third language (fixture c.mp3)\n");
            } else {
                // whisper SURE of Russian (p 0.95); UNSURE (p 0.5) with a forced decode that kept the Russian; UNSURE
                // with a forced decode that came back as a credits line (measured on meeting.wav: one of six Chinese
                // lines forced into French did); and music's "E a\xc3\xad! E a\xc3\xad!" with the same forced credits
                for (int unsureRun = 0; unsureRun < 4; ++unsureRun) {
                    std::vector<std::string> sent;
                    const std::string unsureDoc = docOf("russian", 0.5, { sg(ruSent, 0.3, 2.3, true), empty });
                    const std::string keptDoc = docOf("french", 0.5, { sg(ruSent, 0.3, 2.3, true), empty });
                    const std::string creditsDoc = docOf("french", 0.5, { sg("Sous-titrage Soci\xc3\xa9t\xc3\xa9 Radio-Canada",
                                                                             0.3, 2.3, true), empty });
                    const std::string ptUnsure = docOf("portuguese", 0.6, { sg("E a\xc3\xad! E a\xc3\xad!", 0.3, 2.3, true), empty });
                    g_fileNoModelsForTest = true;
                    g_fileAsrForTest = [&](uint64_t s, uint64_t, const std::string&) -> std::string {
                        sent.push_back(g_fileAsrLangSentForTest);
                        if (s > 5 * kFileRate) return "{\"text\":\"\",\"language\":\"english\",\"segments\":[]}";
                        if (g_fileAsrLangSentForTest == "auto")
                            return unsureRun == 0 ? ruDoc : unsureRun == 3 ? ptUnsure : unsureDoc;
                        return unsureRun == 1 ? keptDoc : creditsDoc;   // forced: Russian kept / a credits line
                    };
                    g_fileMtForTest = [](const std::string& src, const std::string&, const std::string&, size_t) { return "T(" + src + ")"; };
                    const JobRun r = runFileJob(srv, cfg, 7702 + (uint64_t)unsureRun, wideToUtf8L(cmp3),
                                                "fr", "en", "auto", "auto", true);
                    g_fileAsrForTest = nullptr;
                    g_fileMtForTest = nullptr;
                    g_fileNoModelsForTest = false;
                    bool ru = false;
                    for (const auto& q : r.cues) ru = ru || (q.lang == "ru" && q.src == ruSent);
                    static const char* kRun[] = { "sure", "unsure, Russian kept", "unsure, credits back", "music, credits back" };
                    std::printf("    P1 job lap (%s): %zu decode(s), %u forced, %u third, %u fallback, %zu cue(s)\n",
                                kRun[unsureRun], sent.size(), r.st.langRedecodes, r.st.thirdLang, r.st.langFallback,
                                r.cues.size());
                    const bool done = !r.dones.empty() && r.dones.back().result == "ok";
                    if (unsureRun == 0)
                        ok(ru && done && r.st.thirdLang == 1 && r.st.langRedecodes == 0,
                           "P1: a window whisper is SURE is Russian, holding a real sentence, is kept in Russian (1.0.6's "
                           "route), with no second decode");
                    else if (unsureRun == 1)
                        ok(ru && done && r.st.langRedecodes == 1 && r.st.langFallback == 1,
                           "P1: an unsure window decoded again forced, whose forced decode kept the Russian, falls back to "
                           "the first decode -- the scene is subtitled, not dropped");
                    else if (unsureRun == 2)
                        ok(ru && done && r.st.langRedecodes == 1 && r.st.langFallback == 1,
                           "P1: ... and so does one whose forced decode came back as a credits line (nothing left): a "
                           "real sentence in the first decode stands");
                    else
                        ok(r.cues.empty() && done && r.st.langRedecodes == 1 && r.st.langFallback == 0,
                           "P1: music's \"E a\xc3\xad! E a\xc3\xad!\" never comes back that way (not real speech: no cue)");
                }
            }
        }
        // P2 -- whisper's own pair odds first, when decisive; the file's dominant language for a near tie
        {
            std::unique_ptr<FileJob> fj(new FileJob);
            fj->r.mode = "auto"; fj->r.langA = "fr"; fj->r.langB = "en";
            fj->langWords["fr"] = 40; fj->langWords["en"] = 25;            // a bilingual file (62 % French)
            AsrResult cy, tie;
            cy.langProbs = { { "cy", 0.55 }, { "en", 0.30 }, { "fr", 0.02 } };
            tie.langProbs = { { "cy", 0.70 }, { "en", 0.10 }, { "fr", 0.08 } };
            const std::string biCy = plausiblePairLang(*fj, cy), biTie = plausiblePairLang(*fj, tie);
            fj->langWords["en"] = 3;                                         // a French film (93 % French)
            const std::string monoCy = plausiblePairLang(*fj, cy);
            ok(biCy == "en" && biTie == "fr" && monoCy == "fr",
               "P2: in a bilingual film an English line misheard as Welsh is forced to English (its own odds, en 0.30 "
               "vs fr 0.02), a near tie to the leading language; a one-language film keeps its language (measured: "
               "the dubbed film's music windows have decisive English odds)");
        }
        // P5 -- a slow repeat FURTHER than the seam is a person saying it again: kept
        {
            auto W = [](double s, double e, const char* t, bool timed) { FileWord w{ s, e, t, "fr" }; w.timed = timed; return w; };
            const char* kR[] = { " On", " ira", " tous", " au", " paradis,", " tous,", " tous." };
            std::deque<FileWord> sung;
            std::vector<FileWord> again;
            for (int i = 0; i < 7; ++i) {
                sung.push_back(W(100.0 + 0.3 * i, 100.3 + 0.3 * i, kR[i], true));
                again.push_back(W(110.0 + 2.5 * i, 112.4 + 2.5 * i, kR[i], true));   // sung slowly: < 4 ch/s
            }
            ok(dropAlreadyCommitted(again, sung) == 0 && again.size() == 7,
               "P5: a refrain sung again slowly 8 s after the last committed word (beyond the seam) is kept");
        }
        // P6 -- no merge across a dialogue dash, nor across two windows' languages
        {
            auto TL = [](double s, double e, const char* t, const char* lang) { FileWord w{ s, e, t, lang }; w.timed = true; return w; };
            auto cuesOf = [](const std::vector<FileWord>& ws, double endSec) {
                CueBuilder b;
                CueCfg cc;
                b.configure(cc, nullptr);
                b.addWords(ws);
                b.finish(endSec);
                std::vector<FileCue> out;
                FileCue c;
                while (b.pop(c)) out.push_back(c);
                return out;
            };
            const std::vector<FileCue> dash = cuesOf({ TL(0.0, 0.25, " Oui.", "fr"), TL(0.5, 1.2, " - Non,", "fr"),
                                                       TL(1.2, 1.6, " attends.", "fr") }, 10.0);
            // (two words over 0.65 s: the leading-word rule does not hold them -- a 0.9 s gap splits them off)
            const std::vector<FileCue> turn = cuesOf({ TL(0.0, 0.35, " Attends,", "fr"), TL(0.35, 0.65, " voir", "fr"),
                                                       TL(1.55, 1.95, " - Non !", "fr") }, 10.0);
            const std::vector<FileCue> lang = cuesOf({ TL(0.0, 0.25, " Yes.", "en"), TL(0.5, 1.2, " Non,", "fr"),
                                                       TL(1.2, 1.6, " attends.", "fr") }, 10.0);
            const std::vector<FileCue> back = cuesOf({ TL(0.0, 0.4, " Soit", "fr"), TL(0.4, 0.6, " tu", "fr"),
                                                       TL(0.6, 1.0, " restes,", "fr"), TL(1.0, 1.2, " soit", "fr"),
                                                       TL(1.2, 1.3, " on", "fr"), TL(1.3, 3.2, " te", "fr"),
                                                       TL(4.1, 4.5, " push.", "en") }, 10.0);
            for (const auto* v : { &dash, &turn, &lang, &back })
                for (const auto& q : *v) std::printf("    P6: cue %5.2f-%5.2f %s  %s\n", q.start, q.end, q.lang.c_str(), q.text.c_str());
            ok(turn.size() == 2 && turn[0].text == "Attends, voir" && turn[1].text == "- Non !"
               && lang.size() == 2 && lang[0].lang == "en" && lang[1].lang == "fr" && lang[1].text == "Non, attends."
               && back.size() == 2 && back[1].lang == "en",
               "P6: a fragment never joins another speaker's turn (a dialogue dash), and nothing joins two windows' "
               "languages (forward, shared, backward)");
            ok(dash.size() == 1 && dash[0].text == "Oui. - Non, attends.",
               "P6: ... but a line that cannot stay up 1 s SHARES the next speaker's cue, his dash kept (measured: "
               "refusing it left 17 more sub-second cues on the film, down to 0.2 s)");
            // L3 -- a merge at the FRONT of the open cue moves its pending language change with it
            CueBuilder b;
            CueCfg cc;
            cc.hanPair = true;
            b.configure(cc, nullptr);
            b.addWords({ TL(0.0, 0.2, "\xe5\xa5\xbd\xe3\x80\x82", "zh"), TL(0.5, 0.8, "\xe6\x88\x91\xe4\xbb\xac", "zh"),
                         TL(0.8, 1.0, "\xe8\xb5\xb0", "zh"), TL(1.5, 1.7, " OK", "zh"), TL(1.7, 2.0, " go", "zh") });
            b.finish(10.0);
            std::vector<FileCue> hz;
            FileCue c;
            while (b.pop(c)) hz.push_back(c);
            for (const auto& q : hz) std::printf("    L3: cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
            ok(hz.size() == 2 && hz[0].text == "\xe5\xa5\xbd\xe3\x80\x82\xe6\x88\x91\xe4\xbb\xac\xe8\xb5\xb0" && hz[1].text == "OK go",
               "L3: a fragment merged in front of a cue whose language change is pending still splits at the change");
            // L2 -- Korean is read at 12 Hangul a second (24 Hangul: ~2.2 s, not the 3.6 s of Han's 7)
            const std::vector<FileCue> ko = cuesOf({
                TL(0.0, 0.25, " \xea\xb0\x80\xeb\x82\x98\xeb\x8b\xa4\xeb\x9d\xbc\xeb\xa7\x88\xeb\xb0\x94", "ko"),
                TL(0.25, 0.5, " \xec\x82\xac\xec\x95\x84\xec\x9e\x90\xec\xb0\xa8\xec\xb9\xb4\xed\x83\x80", "ko"),
                TL(0.5, 0.75, " \xed\x8c\x8c\xed\x95\x98\xea\xb0\x80\xeb\x82\x98\xeb\x8b\xa4\xeb\x9d\xbc", "ko"),
                TL(0.75, 1.0, " \xeb\xa7\x88\xeb\xb0\x94\xec\x82\xac\xec\x95\x84\xec\x9e\x90\xec\xb0\xa8.", "ko") }, 20.0);
            std::printf("    L2: Korean cue %.2f-%.2f (%.2f s)\n", ko.empty() ? 0.0 : ko[0].start, ko.empty() ? 0.0 : ko[0].end,
                        ko.empty() ? 0.0 : ko[0].end - ko[0].start);
            ok(ko.size() == 1 && ko[0].end - ko[0].start > 2.0 && ko[0].end - ko[0].start < 2.6,
               "L2: a Korean cue's reading time counts Hangul at 12 a second");
        }
        // C2 -- the rest of a credits line, carried on MID-WORD into the next segment (the film's end credits:
        // "For more information, visit www.fema." | "org, visit www.fema.org", p 0.39 -- the first went, the second
        // became a cue); and, heard confidently, the continuation does not make the credits line "not alone"
        {
            auto fema = [](double p) {
                char tail[96];
                std::snprintf(tail, sizeof(tail), "],\"detected_language\":\"english\",\"detected_language_probability\":%.2f}", p);
                return std::string("{\"text\":\" For more information, visit www.fema.org, visit www.fema.org, visit www.fema.org\","
                    "\"language\":\"english\",\"segments\":["
                    "{\"id\":0,\"text\":\" For more information, visit www.fema.org, visit www.fema.\",\"start\":0.0,\"end\":20.54,\"words\":["
                    "{\"word\":\" For\",\"start\":0.0,\"end\":0.3,\"t_dtw\":30,\"probability\":0.9},"
                    "{\"word\":\" more\",\"start\":0.3,\"end\":0.6,\"t_dtw\":60,\"probability\":0.9},"
                    "{\"word\":\" information,\",\"start\":0.6,\"end\":1.2,\"t_dtw\":120,\"probability\":0.9},"
                    "{\"word\":\" visit\",\"start\":1.2,\"end\":1.5,\"t_dtw\":150,\"probability\":0.9},"
                    "{\"word\":\" www.fema.org,\",\"start\":1.5,\"end\":2.5,\"t_dtw\":250,\"probability\":0.9},"
                    "{\"word\":\" visit\",\"start\":2.5,\"end\":2.8,\"t_dtw\":280,\"probability\":0.9},"
                    "{\"word\":\" www.fema.\",\"start\":2.8,\"end\":3.4,\"t_dtw\":340,\"probability\":0.9}],"
                    "\"temperature\":0.0,\"avg_logprob\":-0.73},"
                    "{\"id\":1,\"text\":\"org, visit www.fema.org\",\"start\":20.54,\"end\":29.38,\"words\":["
                    "{\"word\":\"org,\",\"start\":20.54,\"end\":20.9,\"t_dtw\":2090,\"probability\":0.9},"
                    "{\"word\":\" visit\",\"start\":20.9,\"end\":21.2,\"t_dtw\":2120,\"probability\":0.9},"
                    "{\"word\":\" www.fema.org\",\"start\":21.2,\"end\":22.0,\"t_dtw\":2200,\"probability\":0.9}],"
                    "\"temperature\":0.0,\"avg_logprob\":-0.13},"
                    "{\"id\":2,\"text\":\"\",\"start\":0.0,\"end\":0.0,\"words\":[],\"temperature\":0.0,\"avg_logprob\":0.0}")
                    + std::string(tail);
            };
            const CommitVerdict lo = commitOf(fema(0.39), 5894.48, 5923.75, WindowPlan::kPause);
            const CommitVerdict hi = commitOf(fema(0.95), 5894.48, 5923.75, WindowPlan::kPause);
            // ... while a sentence carried on mid-word after a KEPT segment stays (the film's "intention" | "nel, j'ai...")
            const CommitVerdict nel = commitOf("{\"text\":\" Ce n'etait pas intention nel.\",\"language\":\"french\",\"segments\":["
                "{\"id\":0,\"text\":\" Ce n'etait pas intention\",\"start\":0.0,\"end\":2.0,\"words\":["
                "{\"word\":\" Ce\",\"start\":0.0,\"end\":0.3,\"t_dtw\":30,\"probability\":0.9},"
                "{\"word\":\" pas\",\"start\":0.3,\"end\":0.8,\"t_dtw\":80,\"probability\":0.9},"
                "{\"word\":\" intention\",\"start\":0.8,\"end\":1.9,\"t_dtw\":190,\"probability\":0.9}],\"temperature\":0.0,\"avg_logprob\":-0.1},"
                "{\"id\":1,\"text\":\"nel.\",\"start\":2.0,\"end\":2.4,\"words\":["
                "{\"word\":\"nel.\",\"start\":2.0,\"end\":2.4,\"t_dtw\":240,\"probability\":0.9}],\"temperature\":0.0,\"avg_logprob\":-0.1}],"
                "\"detected_language\":\"french\",\"detected_language_probability\":0.99}", 0.0, 10.0, WindowPlan::kPause);
            std::printf("    C2 continuation: p 0.39 -> \"%s\" (%d tag); p 0.95 -> \"%s\" (%d tag); a real one -> \"%s\"\n",
                        textOf(lo.words).c_str(), lo.tags, textOf(hi.words).c_str(), hi.tags, textOf(nel.words).c_str());
            ok(lo.words.empty() && lo.tags == 2 && hi.words.empty() && hi.tags == 2 && textOf(nel.words) == "Ce pas intentionnel.",
               "C2: the rest of a credits line carried on mid-word into the next segment goes with it (and does not make "
               "it \"not alone\"); a real sentence carried on mid-word stays");
        }
        // L4 -- a chant said word by word is speech, not a loop; L6 -- no glue across a segment after a full stop
        {
            const CommitVerdict v = commitOf(docOf("french", 0.99, { sg("Vas-y, vas-y !", 0.5, 1.5, true),
                                                                      sg("Vas-y, vas-y !", 1.6, 2.6, true), empty }),
                                             0.0, 10.0, WindowPlan::kPause);
            ok(!isRepetitionLoop("Vas-y ! Vas-y ! Vas-y ! Vas-y ! Vas-y !") && v.repeats == 0 && !v.degenerate
               && textOf(v.words) == "Vas-y, vas-y ! Vas-y, vas-y !",
               "L4: a chant (\"Vas-y ! x5\", \"Vas-y, vas-y !\" twice) is kept -- not a loop, not a repeated segment");
            AsrResult two;
            parseAsrResponse("{\"text\":\" Le ciel.Bon, on y va.\",\"language\":\"french\",\"segments\":["
                             "{\"id\":0,\"text\":\" Le ciel.\",\"start\":0.0,\"end\":1.0,\"words\":["
                             "{\"word\":\" Le\",\"start\":0.0,\"end\":0.4,\"t_dtw\":40,\"probability\":0.9},"
                             "{\"word\":\" ciel\",\"start\":0.4,\"end\":0.9,\"t_dtw\":90,\"probability\":0.9},"
                             "{\"word\":\".\",\"start\":0.9,\"end\":1.0,\"t_dtw\":100,\"probability\":0.9}],\"temperature\":0.0},"
                             "{\"id\":1,\"text\":\"Bon, on y va.\",\"start\":1.2,\"end\":2.4,\"words\":["
                             "{\"word\":\"Bon\",\"start\":1.2,\"end\":1.5,\"t_dtw\":150,\"probability\":0.9},"
                             "{\"word\":\",\",\"start\":1.5,\"end\":1.6,\"t_dtw\":160,\"probability\":0.9},"
                             "{\"word\":\" on\",\"start\":1.6,\"end\":1.8,\"t_dtw\":180,\"probability\":0.9},"
                             "{\"word\":\" y\",\"start\":1.8,\"end\":1.9,\"t_dtw\":190,\"probability\":0.9},"
                             "{\"word\":\" va\",\"start\":1.9,\"end\":2.3,\"t_dtw\":230,\"probability\":0.9},"
                             "{\"word\":\".\",\"start\":2.3,\"end\":2.4,\"t_dtw\":240,\"probability\":0.9}],\"temperature\":0.0}]}", two);
            const std::vector<WindowWord> tw = windowWords(two, 0.0);
            ok(tw.size() == 6 && oneLine(tw[1].w.text) == "ciel." && oneLine(tw[2].w.text) == "Bon,",
               "L6: a segment opening without a space after a full stop is a new word (\"ciel.\" | \"Bon,\")");
        }
    }
    std::printf("    %s\n", failures == before ? "words / DTW / duplicate / de-dup / language / tags / readability OK"
                                               : "SEE FAILURES ABOVE");
}

void fileModeSelftest(Servers& srv, const TrConfig& cfg, int& failures, bool live = true) {
    auto ok = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what.c_str()); ++failures; }
    };
    const std::wstring F = testFilemodeDir();           // "" when SUVTITLE_TEST_MEDIA is not set: no fixtures, NOT CHECKED
    const std::string  Fu = wideToUtf8L(F);
    const DWORD fa = F.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(F.c_str());
    const bool haveFixtures = fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);

    // ---------------------------------------------------------------- [7a]
    std::printf("\n[7a] FILE MODE -- the media reader (offline)\n");
    if (!haveFixtures) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no file-mode fixtures (<SUVTITLE_TEST_MEDIA>\\filemode)\n");
    } else {
        const int before = failures;
        struct Probe { const wchar_t* f; int video; const char* err; };
        const Probe probes[] = {
            { L"c.mp3", 0, nullptr }, { L"c.m4a", 0, nullptr }, { L"v.mp4", 1, nullptr },
            { L"v.mkv", 1, nullptr }, { L"v.avi", 1, nullptr }, { L"v.ts", 1, nullptr },
            { L"v51.mp4", 1, nullptr }, { L"v_mjpeg.avi", 1, nullptr }, { L"art.mka", 0, nullptr },
            { L"s16.aiff", 0, nullptr }, { L"short44.wav", 0, nullptr },
            { L"garbage.mp4", -1, "cannot read this file's format" },
            { L"empty.wav", -1, "cannot read this file's format" },
            { L"videoonly.mp4", -1, "no audio track" },
            { L"does_not_exist.wav", -1, "not found" },
        };
        for (const auto& p : probes) {
            MediaReader r;
            std::string err;
            const bool opened = r.open(F + p.f, MediaReader::Out::Mono16k, err);
            const std::string name = wideToUtf8L(p.f);
            if (p.err) {
                std::printf("    %-18s -> \"%s\"\n", name.c_str(), err.c_str());
                ok(!opened && err.find(p.err) != std::string::npos,
                   name + ": the human error (\"" + p.err + "\"), got \"" + err + "\"");
                continue;
            }
            ok(opened, name + " opens (" + err + ")");
            if (!opened) continue;
            const MediaInfo& mi = r.info();
            std::printf("    %-14s %-20s %5u Hz %u ch  %7.3f s  video=%d  lead %.4f s  trim %.4f s\n",
                        name.c_str(), mi.codec.c_str(), mi.nativeRate, mi.nativeChannels,
                        mi.durationMs / 1000.0, mi.hasVideo ? 1 : 0, mi.leadSec, mi.trimSec);
            ok((int)mi.hasVideo == p.video, name + ": hasVideo=" + std::to_string(mi.hasVideo ? 1 : 0)
               + (p.video ? " (want 1)" : " (want 0: cover art is not a picture)"));
        }
        // The decoded length against the container's duration.
        for (const wchar_t* f : { L"c.m4a", L"v.mp4", L"v.mkv", L"c.mp3", L"short44.wav", L"s16.aiff" }) {
            std::vector<float> x; MediaInfo mi; std::string err;
            if (!decodeAll16k(F + f, x, mi, err)) { ok(false, wideToUtf8L(f) + " decodes (" + err + ")"); continue; }
            const double got = (double)x.size() / 16000.0, want = (double)mi.durationMs / 1000.0;
            std::printf("    %-12s decoded %.3f s, container says %.3f s\n", wideToUtf8L(f).c_str(), got, want);
            ok(std::fabs(got - want) <= 0.060, wideToUtf8L(f) + ": decoded length within 60 ms of the duration");
        }
        // AIFF == WAV, BIT FOR BIT: the same PCM through the same Resampler DSP (TrMedia.h).
        {
            std::vector<float> ref; MediaInfo mi; std::string err;
            if (!decodeAll16k(F + L"short44.wav", ref, mi, err)) ok(false, "short44.wav decodes (" + err + ")");
            for (const wchar_t* f : { L"s16.aiff", L"s24.aiff", L"ssowt.aifc", L"sfl32.aifc", L"sfl64.aifc" }) {
                std::vector<float> x; MediaInfo m2; std::string e2;
                if (!decodeAll16k(F + f, x, m2, e2)) { ok(false, wideToUtf8L(f) + " decodes (" + e2 + ")"); continue; }
                double mx = 0.0;
                for (size_t i = 0; i < std::min(x.size(), ref.size()); ++i)
                    mx = std::max(mx, (double)std::fabs(x[i] - ref[i]));
                std::printf("    %-12s %-22s %zu samples vs %zu, max |diff| %.2e\n", wideToUtf8L(f).c_str(),
                            m2.codec.c_str(), x.size(), ref.size(), mx);
                ok(!ref.empty() && x.size() == ref.size() && mx <= 1e-6,
                   wideToUtf8L(f) + " == short44.wav (same length, max |diff| <= 1e-6)");
            }
        }
        // The resampler as the shipping reader drives it: 9 kHz at 48 kHz folds onto 7 kHz if aliased.
        {
            std::vector<float> x; MediaInfo mi; std::string err;
            if (decodeAll16k(F + L"t9k_48000.wav", x, mi, err) && x.size() > 16000) {
                const std::vector<float> body(x.begin() + (std::ptrdiff_t)(x.size() / 10),
                                              x.end() - (std::ptrdiff_t)(x.size() / 10));
                double pk = 0.0;
                for (float v : body) pk = std::max(pk, (double)std::fabs(v));
                const double alias = goertzel(body, 7000.0, 16000.0);
                auto db = [](double a) { return a > 1e-12 ? 20.0 * std::log10(a / 0.5) : -240.0; };
                std::printf("    9 kHz @48k through the reader: alias at 7 kHz %.1f dB, steady peak %.1f dB\n",
                            db(alias), db(pk));
                ok(db(alias) < -60.0 && db(pk) < -40.0, "9 kHz is removed, not folded onto 7 kHz");
            } else {
                ok(false, "t9k_48000.wav decodes (" + err + ")");
            }
        }
        // THE TIMELINE.
        {
            std::vector<float> x; MediaInfo mi; std::string err;
            if (decodeAll16k(F + L"burst_44100.wav", x, mi, err)) {
                const double c = energyCentroid(x, 0.9, 1.1);
                std::printf("    burst at 1.000 s, WAV 44.1k -> %.4f s\n", c);
                ok(std::fabs(c - 1.0) < 0.002, "a WAV burst at 1.000 s lands at 1.000 s (+-2 ms)");
            } else ok(false, "burst_44100.wav decodes (" + err + ")");
            if (decodeAll16k(F + L"burst48.m4a", x, mi, err)) {
                const double c = energyCentroid(x, 0.9, 1.1);
                std::printf("    burst at 1.000 s, AAC m4a (edit list trim %.4f s) -> %.4f s\n", mi.trimSec, c);
                ok(std::fabs(c - 1.0) < 0.005, "AAC priming trimmed by the edit list: the burst lands at 1.000 s "
                                               "(+-5 ms; Media Foundation alone puts it ~21 ms late)");
            } else ok(false, "burst48.m4a decodes (" + err + ")");
            double lead = 0, trim = 0, lead2 = 0, trim2 = 0;
            const bool e1 = isoBmffAudioEdit(F + L"v_late.mp4", lead, trim);
            const bool e2 = isoBmffAudioEdit(F + L"v.mp4", lead2, trim2);
            std::printf("    elst: v_late.mp4 lead %.4f trim %.4f | v.mp4 lead %.4f trim %.4f\n", lead, trim, lead2, trim2);
            // ffmpeg -itsoffset 1.5 writes an EMPTY edit of 1.476 s and then the media from 0 --
            // priming included -- so the first audible sample is at 1.476 + 0.023 = 1.499 s.
            ok(e1 && std::fabs(lead - 1.476) < 0.001 && trim == 0.0, "v_late.mp4: an empty edit of 1.476 s, no trim");
            ok(e2 && lead2 == 0.0 && std::fabs(trim2 - 1024.0 / 44100.0) < 0.001,
               "v.mp4: no lead, the 1024-sample AAC priming trimmed");
            std::vector<float> a, b;
            MediaInfo ma, mb;
            if (decodeAll16k(F + L"v.mp4", a, ma, err) && decodeAll16k(F + L"v_late.mp4", b, mb, err)) {
                const double d = firstAbove(b, 0.01f) - firstAbove(a, 0.01f);
                const double want = (lead - trim) - (lead2 - trim2);
                std::printf("    v_late.mp4's audio starts %.4f s after v.mp4's (the two edit lists say %.4f)\n", d, want);
                ok(std::fabs(d - want) < 0.002, "the edit lists are APPLIED: the onsets differ by exactly what they say (+-2 ms)");
            } else ok(false, "v.mp4 / v_late.mp4 decode (" + err + ")");
            std::vector<float> ts, m4;
            MediaInfo mt, mm;
            if (decodeAll16k(F + L"v.ts", ts, mt, err) && decodeAll16k(F + L"c.m4a", m4, mm, err)) {
                const double d = firstAbove(ts, 0.01f) - firstAbove(m4, 0.01f);
                std::printf("    v.ts's audio starts %.3f s after c.m4a's (its first sample is stamped late)\n", d);
                ok(d > 0.70 && d < 0.83, "a first sample stamped late (MPEG-TS) is preceded by its silence");
                // ... and that silence is OURS, exact zeros up to the first stamp (0.757 s): the first
                // decoded packet (AAC priming, tiny but not zero) sits AT its stamp, not at 0 with the
                // gap filler catching up one packet later.
                size_t nz = 0;
                for (size_t i = 0; i < ts.size() && i < (size_t)(0.74 * 16000); ++i) if (ts[i] != 0.0f) ++nz;
                ok(nz == 0, "v.ts: digital silence before the first stamp (" + std::to_string(nz)
                            + " non-zero sample(s) in the first 0.74 s)");
            } else ok(false, "v.ts / c.m4a decode (" + err + ")");
        }
        // FPROXY: native rate, <= 2 channels, the SAME timeline as the analysis, RF64, cancel.
        {
            wchar_t tmpDir[MAX_PATH] = {};
            ::GetTempPathW(MAX_PATH, tmpDir);
            const std::wstring dir = std::wstring(tmpDir) + L"SuVtitle_selftest_proxy";
            ::CreateDirectoryW(dir.c_str(), nullptr);
            struct PCase { const wchar_t* src; const char* what; };
            for (const PCase& pc : { PCase{ L"v.avi", "AVI (MP3)" }, PCase{ L"s16.aiff", "AIFF" },
                                     PCase{ L"v51.mp4", "5.1 AAC" }, PCase{ L"v_late.mp4", "MP4 + edit list" },
                                     PCase{ L"v.ts", "MPEG-TS" } }) {
                const std::wstring out = dir + L"\\proxy.wav";
                std::atomic<bool> cancel{ false };
                std::string err;
                const ProxyResult pr = writeProxyWav(F + pc.src, out, cancel, err);
                const WavFacts w = readProxyHeader(out);
                std::vector<float> x; MediaInfo mi; std::string e2;
                decodeAll16k(F + pc.src, x, mi, e2);
                const double proxySec = w.rate ? (double)w.frames / w.rate : 0.0, sec16 = (double)x.size() / 16000.0;
                std::printf("    FPROXY %-15s -> %s %u Hz %u ch %u-bit, %.3f s (the 16 kHz analysis: %.3f s) %s\n",
                            pc.what, w.rf64 ? "RF64" : "RIFF", w.rate, w.ch, w.bits, proxySec, sec16,
                            w.ok ? "" : w.why.c_str());
                ok(pr == ProxyResult::Ok && w.ok && w.rate == 44100 && w.ch == 2 && w.bits == 16,
                   std::string("FPROXY ") + pc.what + ": a valid 16-bit WAV at the native rate, 2 channels (" + err + ")");
                ok(std::fabs(proxySec - sec16) < 0.05,
                   std::string("FPROXY ") + pc.what + ": on the SAME timeline as the analysis (+-50 ms)");
                ::DeleteFileW(out.c_str());
            }
            {   // RF64 without writing 4 GiB: the threshold is lowered for this one call.
                const std::wstring out = dir + L"\\proxy_rf64.wav";
                std::atomic<bool> cancel{ false };
                std::string err;
                setProxyRf64ThresholdForTest(64 * 1024);
                const ProxyResult pr = writeProxyWav(F + L"short44.wav", out, cancel, err);
                setProxyRf64ThresholdForTest(0);
                const WavFacts w = readProxyHeader(out);
                std::printf("    FPROXY past the RIFF limit -> %s, %llu frames %s\n", w.rf64 ? "RF64" : "RIFF",
                            (unsigned long long)w.frames, w.ok ? "" : w.why.c_str());
                ok(pr == ProxyResult::Ok && w.ok && w.rf64 && w.frames == 8 * 44100,
                   "FPROXY past the RIFF limit writes RF64 (ds64 sizes == the file, sentinels in place)");
                ::DeleteFileW(out.c_str());
            }
            {   // CANCEL: nothing left behind, not even the .tmp.
                const std::wstring out = dir + L"\\proxy_cancel.wav";
                std::atomic<bool> cancel{ true };
                std::string err;
                const ProxyResult pr = writeProxyWav(F + L"v.avi", out, cancel, err);
                ok(pr == ProxyResult::Cancelled
                   && ::GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES
                   && ::GetFileAttributesW((out + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
                   "a cancelled FPROXY answers Cancelled and leaves neither the copy nor its .tmp");
                std::string e3;
                ok(writeProxyWav(F + L"garbage.mp4", out, cancel, e3) == ProxyResult::Error
                   && e3.find("Convert it") != std::string::npos
                   && ::GetFileAttributesW((out + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
                   "an unreadable source: FPROXY Error with the human sentence, nothing left behind");
            }
            ::RemoveDirectoryW(dir.c_str());
        }
        // ---- WB, after the review: the reader's own defects -------------------------------------
        {
            wchar_t tmpDir[MAX_PATH] = {};
            ::GetTempPathW(MAX_PATH, tmpDir);
            const std::wstring tmp = tmpDir;
            // #1 NO MEDIA FOUNDATION (Windows N / KN): one sentence, from the reader and the proxy alike.
            setMediaFoundationDllForTest(L"mfplat_absent_selftest.dll");
            {
                MediaReader r;
                std::string err, e2;
                const bool opened = r.open(F + L"c.mp3", MediaReader::Out::Mono16k, err);
                std::atomic<bool> cancel{ false };
                const ProxyResult pr = writeProxyWav(F + L"v.avi", tmp + L"SuVtitle_selftest_nomf.wav", cancel, e2);
                setMediaFoundationDllForTest(L"");
                std::printf("    without Media Foundation -> \"%s\"\n", err.c_str());
                ok(!opened && err.find("Media Feature Pack") != std::string::npos
                   && pr == ProxyResult::Error && e2.find("Media Feature Pack") != std::string::npos,
                   "Media Foundation absent: the reader and FPROXY answer the Media Feature Pack sentence");
            }
            setMediaFoundationDllForTest(L"");
            // #9 A DAMAGED COMM claiming 65535 channels: refused, nothing allocated.
            {
                std::vector<unsigned char> b;
                auto put = [&](const char* s) { b.insert(b.end(), s, s + 4); };
                auto be32 = [&](uint32_t v) { for (int k = 3; k >= 0; --k) b.push_back((unsigned char)(v >> (8 * k))); };
                put("FORM"); be32(4 + 26 + 8 + 24); put("AIFF");
                put("COMM"); be32(18);
                b.push_back(0xFF); b.push_back(0xFF);                 // 65535 channels
                be32(1000);                                           // frames
                b.push_back(0); b.push_back(16);                      // bits
                const unsigned char rate[10] = { 0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0 };   // 44100
                b.insert(b.end(), rate, rate + 10);
                put("SSND"); be32(24); be32(0); be32(0);
                b.insert(b.end(), 16, 0);
                const std::wstring path = tmp + L"SuVtitle_selftest_bad.aiff";
                MediaReader r;
                std::string err;
                const bool opened = writeBytes(path, b) && r.open(path, MediaReader::Out::Pcm16Native, err);
                r.close();
                ::DeleteFileW(path.c_str());
                std::printf("    AIFF claiming 65535 channels -> \"%s\"\n", err.c_str());
                ok(!opened && err.find("implausible") != std::string::npos,
                   "a damaged AIFF header (65535 channels) is refused before any allocation");
            }
            // #10 A CRAFTED 64-bit BOX SIZE: the edit-list walk must end, at once.
            {
                std::vector<unsigned char> b = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
                                                 'i', 's', 'o', 'm', 'm', 'p', '4', '1' };
                const unsigned char big[16] = { 0, 0, 0, 1, 'f', 'r', 'e', 'e',
                                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE8 };   // 2^64 - 24: wraps back to 0
                b.insert(b.end(), big, big + 16);
                b.insert(b.end(), 64, 0);
                const std::wstring path = tmp + L"SuVtitle_selftest_box.mp4";
                writeBytes(path, b);
                std::atomic<int> state{ 0 };                       // 0 running, 1 false, 2 true
                std::thread t([&] { double l = 0, tr = 0; state = isoBmffAudioEdit(path, l, tr) ? 2 : 1; });
                for (int i = 0; i < 100 && state == 0; ++i) ::Sleep(20);
                const int s = state.load();
                if (s == 0) t.detach(); else t.join();
                if (s != 0) ::DeleteFileW(path.c_str());
                ok(s == 1, "a crafted 64-bit box size ends the edit-list walk at once (no loop, no edit)");
            }
            // #5 A LEAD OVER 30 s: padded on the analysis AND the proxy (no mid-stream cap on the first sample).
            {
                std::vector<float> x;
                MediaInfo mi;
                std::string err;
                if (decodeAll16k(F + L"late35.mp4", x, mi, err)) {
                    const double first = firstAbove(x, 0.05f), len = (double)x.size() / 16000.0;
                    std::atomic<bool> cancel{ false };
                    std::string e2;
                    const std::wstring out = tmp + L"SuVtitle_selftest_late35.wav";
                    const ProxyResult pr = writeProxyWav(F + L"late35.mp4", out, cancel, e2);
                    const WavFacts w = readProxyHeader(out);
                    const double proxySec = w.rate ? (double)w.frames / w.rate : 0.0;
                    ::DeleteFileW(out.c_str());
                    std::printf("    late35.mp4: elst lead %.3f s; first sound at %.3f s of %.3f s decoded (container %.3f s); proxy %.3f s\n",
                                mi.leadSec, first, len, mi.durationMs / 1000.0, proxySec);
                    ok(std::fabs(first - 35.0) < 0.1 && std::fabs(len - mi.durationMs / 1000.0) < 0.1,
                       "a 35 s lead is padded: the sound starts at 35 s of the analysis");
                    ok(pr == ProxyResult::Ok && std::fabs(proxySec - len) < 0.05,
                       "... and of the proxy (the same timeline)");
                } else {
                    ok(false, "late35.mp4 decodes (" + err + ")");
                }
            }
            // #8 THE FOLD: a 5.1 AIFF with a centre tone and an LFE tone -> L == R, centre in both, no LFE.
            {
                const std::wstring out = tmp + L"SuVtitle_selftest_fold.wav";
                std::atomic<bool> cancel{ false };
                std::string err;
                const ProxyResult pr = writeProxyWav(F + L"fold51.aiff", out, cancel, err);
                uint32_t rate = 0, ch = 0;
                std::vector<int16_t> pcm;
                const bool read = pr == ProxyResult::Ok && readProxyPcm(out, rate, ch, pcm);
                ::DeleteFileW(out.c_str());
                int maxLR = 0;
                std::vector<float> left, right;
                for (size_t i = 0; read && ch == 2 && i + 1 < pcm.size(); i += 2) {
                    maxLR = std::max(maxLR, std::abs((int)pcm[i] - (int)pcm[i + 1]));
                    left.push_back(pcm[i] / 32768.0f);
                    right.push_back(pcm[i + 1] / 32768.0f);
                }
                const double c440 = read ? goertzel(left, 440.0, rate) : 0.0;
                const double l60 = read ? goertzel(left, 60.0, rate) : 0.0, r60 = read ? goertzel(right, 60.0, rate) : 0.0;
                std::printf("    5.1 fold: %u ch, max |L-R| %d, centre 440 Hz %.3f, LFE 60 Hz L %.4f R %.4f\n",
                            ch, maxLR, c440, l60, r60);
                ok(read && ch == 2 && maxLR <= 1 && c440 > 0.12 && c440 < 0.17 && l60 < 0.002 && r60 < 0.002,
                   "5.1 -> stereo: the centre at -3 dB in BOTH ears (normalised), the LFE dropped");
            }
            // P3 THE FORMAT CHANGES MID-STREAM (a .ts going 2.0 -> 5.1): the proxy stays stereo, on the analysis' timeline.
            {
                std::vector<float> x;
                MediaInfo mi;
                std::string err, e2;
                const bool dec = decodeAll16k(F + L"tconcat.ts", x, mi, err);
                const std::wstring out = tmp + L"SuVtitle_selftest_tconcat.wav";
                std::atomic<bool> cancel{ false };
                const ProxyResult pr = writeProxyWav(F + L"tconcat.ts", out, cancel, e2);
                const WavFacts w = readProxyHeader(out);
                ::DeleteFileW(out.c_str());
                const double proxySec = w.rate ? (double)w.frames / w.rate : 0.0, sec16 = (double)x.size() / 16000.0;
                std::printf("    tconcat.ts (2.0 -> 5.1): analysis %.3f s, proxy %.3f s, %u ch %s%s\n", sec16, proxySec, w.ch,
                            err.c_str(), e2.c_str());
                // (to the ms -- WD, 11/10: setting the output type again at the change FLUSHED the decoder, and
                // the proxy ran 43-64 ms short of the analysis, a different amount on every run; +-50 ms hid it)
                ok(dec && pr == ProxyResult::Ok && w.ch == 2 && std::fabs(proxySec - sec16) < 0.001,
                   "a mid-stream 2.0 -> 5.1 change: the proxy stays a stereo copy of the same timeline, to the ms");
            }
        }
        std::printf("    %s\n", failures == before ? "reader / timeline / proxy OK" : "SEE FAILURES ABOVE");
    }

    // ---------------------------------------------------------------- [7b]
    std::printf("\n[7b] FILE MODE -- the job's decisions (offline)\n");
    {
        const int before = failures;
        // -- the U+FFFD repair, on a RECORDED response (bench_zh, turbo, 10/10) ---------------
        // whisper.cpp splits a Han character's UTF-8 across two tokens and serialises each half
        // as U+FFFD (U+68C0 and U+7EC8 here, eight words); the segment text has them whole.
        static const char* kZhResp =
        "{\"task\":\"transcribe\",\"language\":\"chinese\",\"duration\":17.23,\"text\":\"\xe4\xbb\x8a"
        "\xe5\xa4\xa9\xe6\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80\xe6\x9f"
        "\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7\xe9"
        "\x9f\xb3\xe4\xbd\x86\xe6\x98\xaf\xe6\x88\x91\xe8\xa7\x89\xe5\xbe\x97\xe8\x83\x8c\xe6\x99\xaf"
        "\xe7\x9a\x84\xe7\x8e\xaf\xe5\xa2\x83\xe5\xa3\xb0\xe7\xa8\x8d\xe5\xbe\xae\xe6\x9c\x89\xe7\x82"
        "\xb9\xe5\xa4\xaa\xe5\xa4\xa7\",\"segments\":[{\"id\":1,\"text\":\"\xe4\xbb\x8a\xe5\xa4\xa9\xe6"
        "\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80\xe6\x9f\xa5\xe7\xac\xac"
        "\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7\xe9\x9f\xb3\",\"st"
        "art\":1.4000000000000001,\"end\":5.96,\"tokens\":[],\"words\":[{\"word\":\"\xe4\xbb\x8a\xe5"
        "\xa4\xa9\",\"start\":1.6,\"end\":1.99,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe6\x88"
        "\x91\xe4\xbb\xac\",\"start\":1.99,\"end\":2.46,\"t_dtw\":-1,\"probability\":1.0},{\"word\":"
        "\"\xe8\xa6\x81\",\"start\":2.56,\"end\":2.72,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\""
        "\xe4\xb8\x80\xe8\xb5\xb7\",\"start\":3.46,\"end\":3.46,\"t_dtw\":-1,\"probability\":1.0},{\""
        "word\":\"\xef\xbf\xbd\",\"start\":3.59,\"end\":3.65,\"t_dtw\":-1,\"probability\":0.999},{\""
        "word\":\"\xef\xbf\xbd\",\"start\":3.65,\"end\":3.74,\"t_dtw\":-1,\"probability\":1.0},{\"wo"
        "rd\":\"\xe6\x9f\xa5\",\"start\":3.74,\"end\":4.08,\"t_dtw\":-1,\"probability\":0.994},{\"wo"
        "rd\":\"\xe7\xac\xac\xe4\xba\x8c\",\"start\":4.08,\"end\":4.55,\"t_dtw\":-1,\"probability\":"
        "0.971},{\"word\":\"\xe5\x9c\xba\",\"start\":4.55,\"end\":4.7700000000000005,\"t_dtw\":-1,\""
        "probability\":0.997},{\"word\":\"\xe7\x9a\x84\",\"start\":4.7700000000000005,\"end\":5.0,\""
        "t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe6\x9c\x80\",\"start\":5.01,\"end\":5.24,\"t_"
        "dtw\":-1,\"probability\":1.0},{\"word\":\"\xef\xbf\xbd\",\"start\":5.24,\"end\":5.39,\"t_dt"
        "w\":-1,\"probability\":0.999},{\"word\":\"\xef\xbf\xbd\",\"start\":5.39,\"end\":5.46,\"t_dt"
        "w\":-1,\"probability\":0.999},{\"word\":\"\xe6\xb7\xb7\",\"start\":5.46,\"end\":5.69,\"t_dt"
        "w\":-1,\"probability\":0.995},{\"word\":\"\xe9\x9f\xb3\",\"start\":5.69,\"end\":5.92,\"t_dt"
        "w\":-1,\"probability\":0.999}],\"temperature\":0.0,\"avg_logprob\":-0.0029,\"no_speech_prob"
        "\":0.0},{\"id\":3,\"text\":\"\xe4\xbd\x86\xe6\x98\xaf\xe6\x88\x91\xe8\xa7\x89\xe5\xbe\x97\xe8"
        "\x83\x8c\xe6\x99\xaf\xe7\x9a\x84\xe7\x8e\xaf\xe5\xa2\x83\xe5\xa3\xb0\xe7\xa8\x8d\xe5\xbe\xae"
        "\xe6\x9c\x89\xe7\x82\xb9\xe5\xa4\xaa\xe5\xa4\xa7\",\"start\":8.74,\"end\":12.94,\"tokens\":"
        "[],\"words\":[{\"word\":\"\xe4\xbd\x86\xe6\x98\xaf\",\"start\":8.94,\"end\":9.23,\"t_dtw\":"
        "-1,\"probability\":1.0},{\"word\":\"\xe6\x88\x91\xe8\xa7\x89\xe5\xbe\x97\",\"start\":9.23,\""
        "end\":9.97,\"t_dtw\":-1,\"probability\":0.999},{\"word\":\"\xe8\x83\x8c\",\"start\":9.97,\""
        "end\":10.06,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe6\x99\xaf\",\"start\":10.22,\""
        "end\":10.22,\"t_dtw\":-1,\"probability\":0.998},{\"word\":\"\xe7\x9a\x84\",\"start\":10.69,"
        "\"end\":10.69,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xef\xbf\xbd\",\"start\":10.85,"
        "\"end\":10.85,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xef\xbf\xbd\",\"start\":10.93,"
        "\"end\":10.93,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe5\xa2\x83\",\"start\":10.950"
        "000000000001,\"end\":11.11,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe5\xa3\xb0\",\"s"
        "tart\":11.19,\"end\":11.41,\"t_dtw\":-1,\"probability\":0.969},{\"word\":\"\xef\xbf\xbd\",\""
        "start\":11.41,\"end\":11.57,\"t_dtw\":-1,\"probability\":0.999},{\"word\":\"\xef\xbf\xbd\","
        "\"start\":11.57,\"end\":11.65,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe5\xbe\xae\","
        "\"start\":11.65,\"end\":11.89,\"t_dtw\":-1,\"probability\":0.997},{\"word\":\"\xe6\x9c\x89\""
        ",\"start\":11.89,\"end\":12.11,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe7\x82\xb9\""
        ",\"start\":12.18,\"end\":12.36,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe5\xa4\xaa\""
        ",\"start\":12.44,\"end\":12.59,\"t_dtw\":-1,\"probability\":1.0},{\"word\":\"\xe5\xa4\xa7\""
        ",\"start\":12.64,\"end\":12.94,\"t_dtw\":-1,\"probability\":1.0}],\"temperature\":0.0,\"avg"
        "_logprob\":-0.0023,\"no_speech_prob\":0.0}],\"detected_language\":\"chinese\",\"detected_la"
        "nguage_probability\":1.0}";
        AsrResult a;
        const bool parsed = parseAsrResponse(kZhResp, a);
        size_t fffd = 0;
        for (const auto& w : a.words) if (w.text.find("\xef\xbf\xbd") != std::string::npos) ++fffd;
        bool joined = parsed && !a.segments.empty();
        auto ltrim = [](std::string v) { while (!v.empty() && v.front() == ' ') v.erase(v.begin()); return v; };
        for (const auto& s : a.segments) {
            std::string j;
            for (size_t i = s.w0; i < s.w1 && i < a.words.size(); ++i) j += a.words[i].text;
            if (ltrim(j) != ltrim(s.text)) {
                joined = false;
                std::printf("      joined: %s\n      text  : %s\n", j.c_str(), s.text.c_str());
            }
        }
        std::printf("    recorded bench_zh response: %zu segment(s), %zu word(s) after the repair, %zu with U+FFFD\n",
                    a.segments.size(), a.words.size(), fffd);
        ok(parsed && fffd == 0, "no word carries U+FFFD after the repair");
        ok(joined, "each segment's repaired words join back to EXACTLY its text");

        // -- the window planner on a synthetic stream ----------------------------------------------
        {
            // 2 s of silence, five 6 s "sentences" (a tone) each followed by 0.6 s of silence,
            // 35 s without a pause, 5 s of silence: every rule of planWindow in one stream.
            std::vector<float> x;
            auto tone = [&](double sec) {
                const int n = (int)(sec * 16000);
                for (int i = 0; i < n; ++i) x.push_back(0.3f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / 16000.0));
            };
            auto sil = [&](double sec) { x.insert(x.end(), (size_t)(sec * 16000), 0.0f); };
            sil(2.0);
            for (int k = 0; k < 5; ++k) { tone(6.0); sil(0.6); }
            tone(35.0);
            sil(5.0);
            EnergyTrack en;
            en.push(x.data(), x.size());
            WindowPlan p;
            uint64_t skip = 0;
            PlanVerdict v = planWindow(en, 0, x.size(), true, true, p, skip);
            std::printf("    first window %.2f-%.2f s (%s)\n", p.s / 16000.0, p.e / 16000.0,
                        p.why == WindowPlan::kPause ? "pause" : p.why == WindowPlan::kHard ? "hard" : "eof");
            ok(v == PlanVerdict::Plan && std::fabs(p.s / 16000.0 - 1.8) < 0.04 && p.why == WindowPlan::kPause
               && p.e / 16000.0 > 8.0 && p.e / 16000.0 < 8.6,
               "the first window starts 0.2 s before the first sound and ends in the first pause after 5 s");
            v = planWindow(en, p.e, x.size(), true, false, p, skip);
            std::printf("    next window  %.2f-%.2f s (%s)\n", p.s / 16000.0, p.e / 16000.0,
                        p.why == WindowPlan::kPause ? "pause" : p.why == WindowPlan::kHard ? "hard" : "eof");
            ok(v == PlanVerdict::Plan && p.why == WindowPlan::kPause && p.e / 16000.0 > 34.4 && p.e / 16000.0 < 34.9,
               "a later window ends in the LATEST pause before 30 s (34.4 s, not 21.2 or 27.8)");
            v = planWindow(en, (uint64_t)(35.0 * 16000), x.size(), true, false, p, skip);
            ok(v == PlanVerdict::Plan && p.why == WindowPlan::kHard && p.e - p.s == 30 * 16000,
               "35 s without a pause: a HARD cut, 30 s long");
            v = planWindow(en, (uint64_t)(70.5 * 16000), x.size(), true, false, p, skip);
            ok(v == PlanVerdict::Done, "silence to the end of the file: Done, no model call");
            v = planWindow(en, 0, (uint64_t)(1.5 * 16000), false, true, p, skip);
            ok(v == PlanVerdict::SkipSilence && skip > 0 && skip <= (uint64_t)(1.3 * 16000),
               "silence so far, more to come: skipped, keeping a pre-roll's worth at the edge");
        }
        // -- the commit rule -------------------------------------------------------------------
        {
            struct SegSpec { double s, e; const char* t; };
            auto mk = [](const std::vector<SegSpec>& segs) {
                AsrResult r;
                r.language = "en";
                for (const auto& sp : segs) {
                    AsrSegment g;
                    g.start = sp.s; g.end = sp.e; g.text = sp.t;
                    g.w0 = r.words.size();
                    AsrWord w;
                    w.text = sp.t; w.start = sp.s; w.end = sp.e;
                    r.words.push_back(w);
                    g.w1 = r.words.size();
                    r.segments.push_back(g);
                }
                return r;
            };
            const AsrResult hard = mk({ { 0.5, 9.0, " First sentence here." }, { 9.5, 19.0, " Second sentence here." },
                                        { 19.5, 29.9, " third one cut by the" } });
            CommitInput ci;
            ci.asr = &hard;
            ci.plan.s = 16000; ci.plan.e = 16000 + 30 * 16000; ci.plan.why = WindowPlan::kHard;
            CommitVerdict cv = commitWindow(ci);
            ok(cv.words.size() == 2 && cv.nextCursor == (uint64_t)std::llround((1.0 + 19.0 + 0.02) * 16000),
               "a HARD cut commits all but the segment running into the cut, and the next window starts "
               "right after the last committed word");
            ci.plan.why = WindowPlan::kPause;
            cv = commitWindow(ci);
            ok(cv.words.size() == 3 && cv.nextCursor == ci.plan.e, "a PAUSE-ended window commits everything");
            const AsrResult loop = mk({ { 0.0, 5.0, " The next step is to start." }, { 5.0, 10.0, " The next step is to start." },
                                        { 10.0, 15.0, " The next step is to start." }, { 15.0, 20.0, " Something else entirely." } });
            ci.asr = &loop;
            cv = commitWindow(ci);
            ok(cv.degenerate && cv.repeats == 2 && cv.words.size() == 2,
               "a window REPEATING a sentence is degenerate, and its repeats are dropped");
            const AsrResult music = mk({ { 0.0, 4.0, " [Music]" }, { 4.0, 9.0, " *music*" }, { 9.0, 12.0, " Hello there." } });
            ci.asr = &music;
            cv = commitWindow(ci);
            ok(cv.words.size() == 1 && cv.junkSegments == 2 && cv.sawJunk,
               "annotations are junk at any level; the speech after them is kept");
            std::vector<FileWord> tail = { { 1, 2, " of", "en" }, { 2, 3, " the", "en" }, { 3, 4, " second", "en" }, { 4, 5, " scene.", "en" } };
            std::vector<FileWord> next = { { 5, 6, " the", "en" }, { 6, 7, " second", "en" }, { 7, 8, " scene.", "en" }, { 9, 10, " The", "en" } };
            dropRepeatedHead(next, tail);
            ok(next.size() == 1 && next[0].text == " The", "after a hard cut, the re-heard head is dropped");
        }
        // -- the cue builder ---------------------------------------------------------------------
        {
            CueBuilder cb;
            CueCfg cc;
            cc.hanPair = true;
            cb.configure(cc, nullptr);
            std::vector<FileWord> w;
            auto add = [&](double s, double e, const char* t) { w.push_back(FileWord{ s, e, t, "en" }); };
            add(0.0, 0.4, " Good"); add(0.4, 0.8, " morning"); add(0.8, 1.2, " everyone."); add(1.2, 1.3, ".");
            add(2.20, 2.25, " The");                     // whisper's early stamp, before a 0.95 s pause
            add(3.20, 3.6, " dialogue"); add(3.6, 3.9, " level"); add(3.9, 4.2, " feels"); add(4.2, 4.6, " right,");
            add(4.6, 4.8, " but"); add(4.8, 4.9, " I"); add(4.9, 5.2, " think"); add(5.2, 5.4, " the");
            add(5.4, 5.9, " ambient"); add(5.9, 6.3, " sound"); add(6.3, 6.5, " is"); add(6.5, 7.0, " slightly");
            add(7.0, 7.2, " too"); add(7.2, 7.5, " loud"); add(7.5, 7.6, " in"); add(7.6, 7.7, " the");
            add(7.7, 7.9, " busy");
            add(7.9, 8.6, " background.");               // 76 Latin letters before it, 86 with it
            w.push_back(FileWord{ 9.0, 9.4, "\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x8c", "zh" });   // U+597D,7684,FF0C
            // A change of language AFTER A PAUSE (0.4 s): a new speaker, a new cue.
            w.push_back(FileWord{ 9.8, 10.2, " Perfect", "en" });
            w.push_back(FileWord{ 10.2, 10.5, " thanks.", "en" });
            cb.addWords(w);
            cb.finish(11.0);
            std::vector<FileCue> cues;
            FileCue c;
            while (cb.pop(c)) cues.push_back(c);
            for (const auto& q : cues) std::printf("    cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
            bool inv = !cues.empty();
            for (size_t i = 0; i < cues.size(); ++i) {
                inv &= cues[i].end > cues[i].start;
                if (i) inv &= cues[i].start >= cues[i - 1].end + 0.079;
                inv &= cues[i].text.find('\n') == std::string::npos;
                inv &= latinChars(cues[i].text) <= cc.maxLatin && hanChars(cues[i].text) <= cc.maxHan;
                if (i + 1 < cues.size())
                    inv &= (cues[i].end - cues[i].start >= cc.minSec - 1e-9) || (cues[i + 1].start - cues[i].end < 0.081);
            }
            ok(inv, "cues: strictly increasing, >= 80 ms apart, one line, within the two-line limits, "
                    ">= 0.7 s unless the next one follows at once");
            ok(cues.size() == 5, "5 cues: a sentence, a long sentence split in two, a change of script");
            ok(cues.size() >= 2 && cues[1].text == "The dialogue level feels right,",
               "a sentence's lone first word, stamped before the pause, stays with its sentence; "
               "an over-long sentence is split at its comma -- even when its LAST word is the one over");
            ok(cues.size() >= 3 && cues[2].text.rfind("but I think", 0) == 0, "... and the rest follows as one cue");
            ok(cues.size() >= 5 && cues[3].text == "\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x8c" && cues[4].text == "Perfect thanks.",
               "a Han -> Latin change of script splits the cue");
            ok(!cues.empty() && cues.back().end <= 11.0, "the last cue never runs past the end of the media");
            // SYNC: a cue starts with its first word -- the cue before it gives up reading time, it
            // never pushes the next one late (each successor here follows its predecessor's last
            // word by under 0.6 s, i.e. inside that predecessor's reading extension).
            ok(cues.size() == 5 && std::fabs(cues[1].start - 2.20) < 0.01 && std::fabs(cues[2].start - 4.60) < 0.01
               && std::fabs(cues[3].start - 9.00) < 0.01 && std::fabs(cues[4].start - 9.80) < 0.01,
               "every cue starts on its first word (the one before it yields its reading time)");
            // A fragment is not a subtitle: two words under 0.35 s join their successor.
            CueBuilder cb2;
            cb2.configure(cc, nullptr);
            cb2.addWords({ FileWord{ 0.0, 0.2, " Yes.", "en" }, FileWord{ 0.5, 0.8, " Right", "en" },
                           FileWord{ 0.8, 1.1, " now.", "en" } });
            cb2.finish(2.0);
            std::vector<FileCue> c2;
            while (cb2.pop(c)) c2.push_back(c);
            ok(c2.size() == 1 && c2[0].text == "Yes. Right now.", "a 0.2 s fragment joins the cue after it");
        }
        // ---- WB, after the review: the job's own defects -----------------------------------------
        {
            CueCfg cz;
            cz.hanPair = true;
            auto cuesOf = [&](const std::vector<FileWord>& ws, double endSec) {
                CueBuilder b;
                b.configure(cz, nullptr);
                b.addWords(ws);
                b.finish(endSec);
                std::vector<FileCue> out;
                FileCue c;
                while (b.pop(c)) out.push_back(c);
                return out;
            };
            auto W = [](double s, double e, const char* t, const char* l) { return FileWord{ s, e, t, l }; };
            // #2 CODE-SWITCHING inside a Chinese sentence is ONE cue: U+628AreverbU+5173,6389,FF0C,7136,540E,518D,542C,4E00,904D,3002
            const std::vector<FileCue> c1 = cuesOf({
                W(0.0, 0.2, "\xe6\x8a\x8a", "zh"), W(0.2, 0.35, " re", "zh"), W(0.35, 0.5, "verb", "zh"),
                W(0.5, 0.6, "\xe5\x85\xb3", "zh"), W(0.6, 0.75, "\xe6\x8e\x89", "zh"), W(0.75, 0.8, "\xef\xbc\x8c", "zh"),
                W(0.8, 1.0, "\xe7\x84\xb6\xe5\x90\x8e", "zh"), W(1.0, 1.1, "\xe5\x86\x8d", "zh"),
                W(1.1, 1.2, "\xe5\x90\xac", "zh"), W(1.2, 1.4, "\xe4\xb8\x80\xe9\x81\x8d", "zh"),
                W(1.4, 1.45, "\xe3\x80\x82", "zh") }, 2.0);
            // ... OKU+FF0C,6211,4EEC,5F00,59CB,5427,3002
            const std::vector<FileCue> c2 = cuesOf({
                W(0.0, 0.3, "OK", "zh"), W(0.3, 0.32, "\xef\xbc\x8c", "zh"), W(0.35, 0.6, "\xe6\x88\x91\xe4\xbb\xac", "zh"),
                W(0.6, 0.9, "\xe5\xbc\x80\xe5\xa7\x8b", "zh"), W(0.9, 1.0, "\xe5\x90\xa7", "zh"), W(1.0, 1.05, "\xe3\x80\x82", "zh") }, 2.0);
            // ... a short English run between two pauses, inside Chinese: still ONE cue (never isolated).
            const std::vector<FileCue> c3 = cuesOf({
                W(0.0, 0.5, "\xe6\x88\x91\xe4\xbb\xac\xe5\x85\x88", "zh"), W(0.9, 1.1, " OK", "zh"),
                W(1.5, 1.8, "\xe7\xbb\xa7\xe7\xbb\xad", "zh"), W(1.8, 2.0, "\xe5\x90\xa7\xe3\x80\x82", "zh") }, 3.0);
            // ... while a real turn -- a pause, then four English tokens -- is a cue of its own.
            const std::vector<FileCue> c4 = cuesOf({
                W(0.0, 1.0, "\xe6\x88\x91\xe4\xbb\xac\xe5\xbc\x80\xe5\xa7\x8b\xe5\x90\xa7", "zh"),
                W(1.5, 1.8, " Perfect", "en"), W(1.8, 2.0, " thanks", "en"), W(2.0, 2.1, " for", "en"),
                W(2.1, 2.4, " coming", "en"), W(2.4, 2.8, " today.", "en") }, 4.0);
            for (const auto* v : { &c1, &c2, &c3, &c4 })
                for (const auto& q : *v) std::printf("    code-switch cue %5.2f-%5.2f  %s\n", q.start, q.end, q.text.c_str());
            ok(c1.size() == 1 && c2.size() == 1 && c3.size() == 1,
               "code-switching inside a Chinese sentence (\xe6\x8a\x8areverb\xe5\x85\xb3\xe6\x8e\x89 / OK\xef\xbc\x8c\xe6\x88\x91\xe4\xbb\xac\xe5\xbc\x80\xe5\xa7\x8b\xe5\x90\xa7 / a short English run between pauses) is ONE cue");
            ok(c4.size() == 2 && c4[1].text == "Perfect thanks for coming today.",
               "a change of language after a pause, running past 3 tokens, is a cue of its own");
            {   // ... and a RECORDED-SHAPE decode of U+628AreverbU+5173,6389 goes the zh -> en way as one cue.
                const char* kCs =
                    "{\"text\":\"\xe6\x8a\x8areverb\xe5\x85\xb3\xe6\x8e\x89\xef\xbc\x8c\xe7\x84\xb6\xe5\x90\x8e\xe5\x86\x8d"
                    "\xe5\x90\xac\xe4\xb8\x80\xe9\x81\x8d\xe3\x80\x82\",\"language\":\"chinese\",\"segments\":[{\"id\":0,"
                    "\"text\":\"\xe6\x8a\x8areverb\xe5\x85\xb3\xe6\x8e\x89\xef\xbc\x8c\xe7\x84\xb6\xe5\x90\x8e\xe5\x86\x8d"
                    "\xe5\x90\xac\xe4\xb8\x80\xe9\x81\x8d\xe3\x80\x82\",\"start\":0.0,\"end\":1.5,\"tokens\":[],\"words\":["
                    "{\"word\":\"\xe6\x8a\x8a\",\"start\":0.0,\"end\":0.2,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"re\",\"start\":0.2,\"end\":0.35,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"verb\",\"start\":0.35,\"end\":0.5,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe5\x85\xb3\",\"start\":0.5,\"end\":0.6,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe6\x8e\x89\",\"start\":0.6,\"end\":0.75,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xef\xbc\x8c\",\"start\":0.75,\"end\":0.8,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe7\x84\xb6\xe5\x90\x8e\",\"start\":0.8,\"end\":1.0,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe5\x86\x8d\",\"start\":1.0,\"end\":1.1,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe5\x90\xac\",\"start\":1.1,\"end\":1.2,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe4\xb8\x80\xe9\x81\x8d\",\"start\":1.2,\"end\":1.4,\"t_dtw\":-1,\"probability\":0.9},"
                    "{\"word\":\"\xe3\x80\x82\",\"start\":1.4,\"end\":1.45,\"t_dtw\":-1,\"probability\":0.9}],"
                    "\"temperature\":0.0,\"avg_logprob\":-0.1,\"no_speech_prob\":0.0}],\"detected_language\":\"chinese\","
                    "\"detected_language_probability\":0.98}";
                AsrResult a;
                parseAsrResponse(kCs, a);
                CommitInput ci;
                ci.asr = &a;
                ci.plan.s = 0; ci.plan.e = (uint64_t)(1.6 * 16000); ci.plan.why = WindowPlan::kPause;
                const CommitVerdict cv = commitWindow(ci);
                const std::vector<FileCue> cr = cuesOf(cv.words, 2.0);
                Routing rt;
                rt.mode = "auto"; rt.langA = "en"; rt.langB = "zh";
                std::string lang;
                if (cr.size() == 1) {
                    AsrResult x;
                    x.text = cr[0].text; x.language = cr[0].lang; x.chinese = chineseByScript(x.text, x.language);
                    lang = sourceLangOf(x, rt);
                }
                std::printf("    decoded \xe6\x8a\x8areverb\xe5\x85\xb3\xe6\x8e\x89... -> %zu cue(s), routed as %s\n", cr.size(), lang.c_str());
                ok(cr.size() == 1 && lang == "zh", "a decoded code-switched sentence: one cue, routed zh -> en (never 'reverb' as English)");
            }
            // P10 A MERGED FRAGMENT RESPECTS THE LIMITS: "Yes." does not join an 84-letter successor.
            {
                std::vector<FileWord> ws = { W(0.0, 0.2, " Yes.", "en") };
                for (int k = 0; k < 12; ++k)
                    ws.push_back(W(0.5 + 0.4 * k, 0.9 + 0.4 * k, k == 11 ? " Abcdefg." : " Abcdefg", "en"));
                const std::vector<FileCue> cv = cuesOf(ws, 6.0);
                bool within = !cv.empty();
                for (const auto& q : cv) within &= narrowChars(q.text) <= cz.maxLatin;
                ok(cv.size() == 2 && within, "a fragment never joins a successor it would push past the two-line limit");
            }
            // #6 OTHER SCRIPTS: limits, loops, repeats.
            {
                std::vector<FileWord> ru;
                for (int k = 0; k < 14; ++k)   // 14 x "U+0441,043B,043E,0432,043E" (5 letters) + spaces = 70 narrow, then 4 more words = 90 > 84
                    ru.push_back(W(0.3 * k, 0.3 * k + 0.25, " \xd1\x81\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xbe", "ru"));
                for (int k = 0; k < 4; ++k)
                    ru.push_back(W(4.2 + 0.3 * k, 4.45 + 0.3 * k, k == 3 ? " \xd1\x81\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xbe." : " \xd1\x81\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xbe", "ru"));
                const std::vector<FileCue> cr = cuesOf(ru, 7.0);
                bool within = cr.size() >= 2;
                for (const auto& q : cr) within &= narrowChars(q.text) <= cz.maxLatin;
                std::vector<FileWord> ja;
                for (int k = 0; k < 12; ++k)   // 12 x "U+3053,3093,306B,3061,306F" = 60 wide > 32
                    ja.push_back(W(0.4 * k, 0.4 * k + 0.35, k == 11 ? "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf\xe3\x80\x82"
                                                                   : "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf", "ja"));
                const std::vector<FileCue> cj = cuesOf(ja, 6.0);
                for (const auto& q : cj) within &= wideChars(q.text) <= cz.maxHan;
                ok(within && cj.size() >= 2, "a long Russian / Japanese sentence is split at the same two-line limits");
                const std::string loop = "\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb8\xd0\xb9 \xd1\x88\xd0\xb0\xd0\xb3 \xd1\x8d\xd1\x82\xd0\xbe ";
                ok(isRepetitionLoop(loop + loop + loop + loop), "a Russian repetition loop is junk (\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb8\xd0\xb9 \xd1\x88\xd0\xb0\xd0\xb3 \xd1\x8d\xd1\x82\xd0\xbe x4)");
                struct SegSpec { double s, e; const char* t; };
                AsrResult rr;
                rr.language = "ru";
                const char* kRu = " \xd0\x94\xd0\xb0\xd0\xb2\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb9\xd0\xb4\xd1\x91\xd0\xbc \xd0\xba \xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb5\xd0\xb9 \xd1\x82\xd0\xb5\xd0\xbc\xd0\xb5.";
                for (int k = 0; k < 3; ++k) {
                    AsrSegment g; g.start = 5.0 * k; g.end = 5.0 * k + 4.0; g.text = kRu; g.w0 = rr.words.size();
                    AsrWord aw; aw.text = kRu; aw.start = g.start; aw.end = g.end; rr.words.push_back(aw);
                    g.w1 = rr.words.size(); rr.segments.push_back(g);
                }
                CommitInput ci;
                ci.asr = &rr;
                ci.plan.s = 0; ci.plan.e = 16 * 16000; ci.plan.why = WindowPlan::kPause;
                const CommitVerdict cv = commitWindow(ci);
                ok(cv.degenerate && cv.repeats == 2, "a window repeating a RUSSIAN sentence is degenerate too");
            }
            // P7 A HARD CUT NEVER SKIPS UNCOMMITTED WORDS: the next window starts after the last committed one.
            {
                AsrResult h;
                h.language = "en";
                const char* t0 = " Short one.";
                const char* t1 = " and then a long run of speech that the cut of the window goes right through";
                AsrSegment g0; g0.start = 0.5; g0.end = 2.0; g0.text = t0; g0.w0 = 0;
                AsrWord a0; a0.text = t0; a0.start = 0.5; a0.end = 2.0; h.words.push_back(a0); g0.w1 = 1;
                AsrSegment g1; g1.start = 2.5; g1.end = 29.9; g1.text = t1; g1.w0 = 1;
                AsrWord a1; a1.text = t1; a1.start = 2.5; a1.end = 29.9; h.words.push_back(a1); g1.w1 = 2;
                h.segments = { g0, g1 };
                CommitInput ci;
                ci.asr = &h;
                ci.plan.s = 16000; ci.plan.e = 16000 + 30 * 16000; ci.plan.why = WindowPlan::kHard;
                const CommitVerdict cv = commitWindow(ci);
                ok(cv.words.size() == 1 && cv.nextCursor == (uint64_t)std::llround((1.0 + 2.0 + 0.02) * 16000),
                   "a hard cut resumes right after the last committed word (the dropped segment is heard again, not skipped)");
            }
            // #7 DIGITAL SILENCE IS NOT THE FLOOR: 30 s of exact zeros, then speech over -50 dB room tone.
            {
                std::vector<float> x((size_t)(30 * 16000), 0.0f);
                uint32_t rng = 777u;
                auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((float)(rng >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.0055f; };
                auto tone = [&](double sec) { const int n = (int)(sec * 16000); for (int i = 0; i < n; ++i) x.push_back(0.3f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / 16000.0) + noise()); };
                auto room = [&](double sec) { const int n = (int)(sec * 16000); for (int i = 0; i < n; ++i) x.push_back(noise()); };
                for (int k = 0; k < 5; ++k) { tone(6.0); room(0.6); }
                room(5.0);
                EnergyTrack en;
                en.push(x.data(), x.size());
                WindowPlan p;
                uint64_t skip = 0;
                const PlanVerdict v = planWindow(en, 0, x.size(), true, true, p, skip);
                std::printf("    30 s of zeros + speech over room tone: floor %.1f dB, active %.1f dB, first window %.2f-%.2f s (%s)\n",
                            en.floorDb(), en.activeDb(), p.s / 16000.0, p.e / 16000.0,
                            p.why == WindowPlan::kPause ? "pause" : p.why == WindowPlan::kHard ? "hard" : "eof");
                ok(v == PlanVerdict::Plan && p.why == WindowPlan::kPause && en.floorDb() > -60.0,
                   "the floor is the room tone, not the digital silence: the pauses in it are found");
            }
        }
        // -- the junk filter as the job applies it -------------------------------------------------
        ok(fileSegmentIsJunk("*music*", -20.0, -60.0, -0.2, "") && fileSegmentIsJunk(" [Music]", -20.0, -60.0, -0.2, ""),
           "file junk: annotations at any level");
        ok(fileSegmentIsJunk("Thank you.", -55.0, -60.0, -0.2, "") && !fileSegmentIsJunk("Thank you.", -20.0, -60.0, -0.2, ""),
           "file junk: a caption is junk only when barely above the file's floor");
        ok(fileSegmentIsJunk("recognis beaches", -55.0, -60.0, -2.5, "")
           && !fileSegmentIsJunk("recognise the beaches", -20.0, -60.0, -2.5, ""),
           "file junk: low confidence counts on quiet audio only");
        ok(isRepetitionLoop("The next step is to the next step is to the next step is to the next step is to.")
           && !isRepetitionLoop("Good morning everyone, today we review the final mix of the second scene."),
           "a repetition loop is junk; a normal sentence is not");
        std::printf("    %s\n", failures == before ? "repair / planner / commit / cues / junk OK" : "SEE FAILURES ABOVE");
    }
    filmGradeTests(srv, cfg, failures);   // [7g] WF: the film's recorded responses, offline

    // ---------------------------------------------------------------- [7c]
    std::printf("\n[7c] FILE MODE -- whole jobs on the servers (the shipping request handler + job lap)\n");
    if (!live) {
        std::printf("    NOT RUN: offline pass (--selftest-filemode offline)\n");
        return;
    }
    if (!haveFixtures) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no file-mode fixtures\n");
        return;
    }
    {
        const char* kEnRef = "Good morning everyone. Today we are going to review the final mix of the second scene. "
                             "The dialogue level feels right, but I think the ambient sound is slightly too loud in "
                             "the background. Let us listen to it together one more time and decide.";
        // U+5927,5BB6,65E9,4E0A,597D,3002,4ECA,5929,6211,4EEC,8981,4E00,8D77,68C0,67E5,7B2C,4E8C,573A,7684,6700,7EC8,6DF7,97F3,3002,5BF9,767D,7684,97F3,91CF,611F,89C9,4E0D,9519,FF0C,4F46,662F,6211,89C9,5F97,80CC,666F,7684,73AF,5883,58F0,7A0D,5FAE,6709,70B9,592A,5927,3002
        // U+6211,4EEC,518D,542C,4E00,904D,FF0C,7136,540E,4E00,8D77,51B3,5B9A,3002
        const char* kZhRef = "\xe5\xa4\xa7\xe5\xae\xb6\xe6\x97\xa9\xe4\xb8\x8a\xe5\xa5\xbd\xe3\x80\x82\xe4\xbb\x8a\xe5\xa4\xa9"
                             "\xe6\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80\xe6\x9f\xa5\xe7\xac\xac"
                             "\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7\xe9\x9f\xb3\xe3\x80\x82"
                             "\xe5\xaf\xb9\xe7\x99\xbd\xe7\x9a\x84\xe9\x9f\xb3\xe9\x87\x8f\xe6\x84\x9f\xe8\xa7\x89\xe4\xb8\x8d"
                             "\xe9\x94\x99\xef\xbc\x8c\xe4\xbd\x86\xe6\x98\xaf\xe6\x88\x91\xe8\xa7\x89\xe5\xbe\x97\xe8\x83\x8c"
                             "\xe6\x99\xaf\xe7\x9a\x84\xe7\x8e\xaf\xe5\xa2\x83\xe5\xa3\xb0\xe7\xa8\x8d\xe5\xbe\xae\xe6\x9c\x89"
                             "\xe7\x82\xb9\xe5\xa4\xaa\xe5\xa4\xa7\xe3\x80\x82\xe6\x88\x91\xe4\xbb\xac\xe5\x86\x8d\xe5\x90\xac"
                             "\xe4\xb8\x80\xe9\x81\x8d\xef\xbc\x8c\xe7\x84\xb6\xe5\x90\x8e\xe4\xb8\x80\xe8\xb5\xb7\xe5\x86\xb3"
                             "\xe5\xae\x9a\xe3\x80\x82";
        // Coverage, script-blind, normalised the engine's way: how much of the reference survived.
        auto coverage = [](const std::string& ref, const std::string& got) {
            const auto r = normWords(lcmapTraditional(ref)), g = normWords(lcmapTraditional(got));
            std::vector<bool> used(g.size(), false);
            size_t hit = 0;
            for (const auto& t : r)
                for (size_t k = 0; k < g.size(); ++k)
                    if (!used[k] && g[k] == t) { used[k] = true; ++hit; break; }
            return r.empty() ? 0.0 : (double)hit / (double)r.size();
        };
        uint64_t id = 7000;

        // 1. en <-> zh on an MP3: the wire, the words, the directions.
        std::printf("\n[7c] c.mp3  en<->zh, quality auto\n");
        JobRun r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "auto", "auto", true);
        failures += checkJobWire(r, id, "c.mp3", true);
        printJobStats(r.st);
        {
            std::string en, zh;
            int enCues = 0, zhCues = 0, badDst = 0;
            for (const auto& c : r.cues) {
                if (c.lang == "en") { en += c.src + " "; ++enCues; if (cjkRatio(c.dst) < 0.2 || c.dstLang != "zh") ++badDst; }
                if (c.lang == "zh") { zh += c.src; ++zhCues; if (!hasLatinLetters(c.dst) || c.dstLang != "en") ++badDst; }
            }
            const double ce = coverage(kEnRef, en), cz = coverage(kZhRef, zh);
            std::printf("    %d en cue(s), %d zh cue(s); %.0f%% of the English bench words and %.0f%% of the "
                        "Chinese bench characters; %d translation(s) in the wrong language\n",
                        enCues, zhCues, ce * 100, cz * 100, badDst);
            ok(ce >= 0.90, "c.mp3: >= 90 % of the English bench words are in the en cues");
            ok(cz >= 0.80, "c.mp3: >= 80 % of the Chinese bench characters are in the zh cues");
            ok(badDst == 0, "c.mp3: every cue is translated into the other half of the pair, in its script");
            ok(!r.progs.empty() && r.progs.front().asrModel == "ggml-large-v3-turbo.bin" && !r.progs.front().hasVideo,
               "quality auto = large-v3-turbo, announced in FPROG loading; an mp3 has no picture");
            // WF: the file whisper runs with DTW, and the server really computed it (flash attention off).
            // (review P7: counted on the SAME windows -- those that committed words; `windows` also counts
            // degenerate retries, forced decodes and windows of punctuation, whatever the GPU did that run)
            ok(r.st.dtw && r.st.wordWindows > 0 && r.st.timedWindows == r.st.wordWindows,
               "WF: the file whisper times every window's words by DTW (-dtw large.v3.turbo -nfa)");
        }

        // 1b. WF review P3: the file whisper FAILS mid-job while it runs with DTW -> the job goes on WITHOUT it
        // (one restart, whisper's token times), FDONE ok -- not the same failure on every retry, then an error.
        std::printf("\n[7c] sil_music_en48.wav  (its whisper fails once while it runs with DTW)\n");
        g_fileAsrFailOnceForTest = true;
        r = runFileJob(srv, cfg, ++id, Fu + "sil_music_en48.wav", "en", "zh", "auto", "auto", true);
        g_fileAsrFailOnceForTest = false;
        failures += checkJobWire(r, id, "dtw_drop", true);
        ok(r.st.dtwDropped == 1 && !r.st.dtw && r.st.wordWindows > 0 && r.st.timedWindows == 0 && !r.cues.empty()
           && !r.dones.empty() && r.dones.back().result == "ok",
           "P3: a DTW file whisper that fails mid-job is restarted WITHOUT DTW, and the job ends ok");

        // 2. Silence, then music, then speech: no cue before the speech.
        std::printf("\n[7c] sil_music_en48.wav  (3 s of digital silence + 5 s of music + speech)\n");
        r = runFileJob(srv, cfg, ++id, Fu + "sil_music_en48.wav", "en", "zh", "auto", "auto", true);
        failures += checkJobWire(r, id, "sil_music", true);
        ok(!r.cues.empty() && r.cues.front().startMs >= 7800, "no cue in the silence nor in the music (first cue >= 7.8 s)");

        // 2b. Nothing but silence: FDONE ok with count 0 -- the "no speech found" WC shows -- and
        //     whisper is never asked (a model call on silence is where captions get invented).
        std::printf("\n[7c] silence10.wav  (10 s of digital silence)\n");
        r = runFileJob(srv, cfg, ++id, Fu + "silence10.wav", "en", "zh", "auto", "auto", false);
        failures += checkJobWire(r, id, "silence", true);
        std::printf("    %zu cue(s), %u window(s) sent to whisper\n", r.cues.size(), r.st.windows);
        ok(r.cues.empty() && !r.dones.empty() && r.dones.back().result == "ok" && r.dones.back().count == 0
           && r.st.windows == 0,
           "a silent file: FDONE ok with count 0 (\"no speech found\"), whisper never called");

        // 3. en <-> zh-tw on a video: the Traditional net on the FILE path, both directions.
        std::printf("\n[7c] v.mp4  en<->zh-tw, quality auto\n");
        r = runFileJob(srv, cfg, ++id, Fu + "v.mp4", "en", "zh-tw", "auto", "auto", true);
        failures += checkJobWire(r, id, "v.mp4 zh-tw", true);
        {
            int srcTw = 0, dstTw = 0, bad = 0;
            for (const auto& c : r.cues) {
                const bool s = c.lang == "zh-tw", d = c.dstLang == "zh-tw";
                if (!s && !d) { ++bad; std::printf("      neither side is zh-tw: %s\n", c.src.c_str()); continue; }
                const std::string& han = s ? c.src : c.dst;
                if (s) ++srcTw;
                if (d) ++dstTw;
                int t = 0, simp = 0;
                hanScriptVotes(han, t, simp);
                if (simp != 0 || lcmapTraditional(han) != han) { ++bad; std::printf("      Simplified left: %s\n", han.c_str()); }
                if (s && c.dstLang != "en") ++bad;
            }
            std::printf("    zh-tw sources %d (the net rewrote %u), zh-tw translations %d (the net rewrote %u)\n",
                        srcTw, r.st.srcNet, dstTw, r.st.dstNet);
            ok(srcTw >= 2 && dstTw >= 2 && bad == 0,
               "zh-tw: every zh-tw text, the transcript (toTraditional(src)) AND the translation (toTraditional(dst)), "
               "is Traditional by the votes and by the system's map");
            ok(!r.progs.empty() && r.progs.front().hasVideo, "v.mp4: FPROG says hasVideo=1");
        }
        const uint64_t firstV = r.cues.empty() ? 0 : r.cues.front().startMs;
        // 3b. The SOURCE net, deterministically: under the zh-tw prompt whisper already writes
        //     Traditional, so the job above may never need it. A Simplified cue, handed to the
        //     shipping translateOneCue of an en<->zh-tw job, must come out Traditional (the MT the
        //     job above loaded is still up: endFile frees only the job's whisper).
        {
            std::unique_ptr<FileJob> tj;
            trpipe::WireFile tf;
            tf.jobId = ++id; tf.path = Fu + "c.mp3"; tf.langA = "en"; tf.langB = "zh-tw";
            std::string err;
            CollectSink sink;
            bool made = makeFileJob(tf, "en", "zh", "auto", cfg, tj, err);
            if (made) {
                tj->r.mtStyle = mtStyleFor(srv.mtModel());
                FileCue c;
                c.start = 1.0; c.end = 3.0; c.lang = "zh";
                c.text = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80"
                         "\xe6\x9f\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7"
                         "\xe9\x9f\xb3\xe3\x80\x82";      // U+4ECA,5929,6211,4EEC,8981,4E00,8D77,68C0,67E5,7B2C,4E8C,573A,7684,6700,7EC8,6DF7,97F3,3002(Simplified)
                tj->toTranslate.push_back(c);
                made = translateOneCue(*tj, srv, sink, err) == FileStep::Ok;
            }
            const JobRun tr = parseJobRun(sink.lines);
            int t = 0, simp = 0;
            if (!tr.cues.empty()) hanScriptVotes(tr.cues[0].src, t, simp);
            std::printf("    a Simplified zh cue in an en<->zh-tw job -> %s  |  %s\n",
                        tr.cues.empty() ? "(none)" : tr.cues[0].src.c_str(),
                        tr.cues.empty() ? err.c_str() : tr.cues[0].dst.c_str());
            ok(made && tr.cues.size() == 1 && tr.cues[0].lang == "zh-tw" && tr.cues[0].dstLang == "en"
               && simp == 0 && t > 0 && lcmapTraditional(tr.cues[0].src) == tr.cues[0].src
               && hasLatinLetters(tr.cues[0].dst),
               "the FILE path's source net: a zh-tw cue is shown in Traditional whatever whisper wrote");
            // 3c. The DESTINATION net, the same way: Qwen asked for zh-tw writes Traditional by itself
            //     (the job above: 0 rewrites), so a model that does NOT is stood in for -- the MT's
            //     reply replaced by a Simplified sentence -- and the cue must still come out Traditional.
            if (made) {
                const size_t before = sink.lines.size();
                FileCue c;
                c.start = 4.0; c.end = 6.0; c.lang = "en";
                c.text = "Today we check the final mix together.";
                tj->toTranslate.push_back(c);
                g_mtReplyForTest = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe4\xbb\xac\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80"
                                   "\xe6\x9f\xa5\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7\xe9\x9f\xb3\xe3\x80\x82";
                                   // U+4ECA,5929,6211,4EEC,4E00,8D77,68C0,67E5,6700,7EC8,6DF7,97F3,3002(Simplified)
                const bool okStep = translateOneCue(*tj, srv, sink, err) == FileStep::Ok;
                g_mtReplyForTest.clear();
                const JobRun tr2 = parseJobRun(std::vector<std::string>(sink.lines.begin() + (std::ptrdiff_t)before,
                                                                        sink.lines.end()));
                int t2 = 0, s2 = 0;
                if (!tr2.cues.empty()) hanScriptVotes(tr2.cues[0].dst, t2, s2);
                std::printf("    an en cue whose MT reply is Simplified -> %s\n",
                            tr2.cues.empty() ? "(none)" : tr2.cues[0].dst.c_str());
                ok(okStep && tr2.cues.size() == 1 && tr2.cues[0].lang == "en" && tr2.cues[0].dstLang == "zh-tw"
                   && s2 == 0 && t2 > 0 && lcmapTraditional(tr2.cues[0].dst) == tr2.cues[0].dst,
                   "the FILE path's destination net: a translation into zh-tw is shown in Traditional "
                   "whatever the model wrote");
            }
            // 3d. THE SIMPLIFIED NET (WB), the same two ways in an en<->zh job -- and a zh-tw line
            //     NEVER simplified. Every MT reply is stood in for, so all three are deterministic.
            const char* kTrad = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe5\x80\x91\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xaa\xa2"
                                "\xe6\x9f\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\xa0\xb4\xe7\x9a\x84\xe6\x9c\x80\xe7\xb5\x82\xe6\xb7\xb7"
                                "\xe9\x9f\xb3\xe3\x80\x82";   // U+4ECA,5929,6211,5011,8981,4E00,8D77,6AA2,67E5,7B2C,4E8C,5834,7684,6700,7D42,6DF7,97F3,3002
            const char* kSimp = "\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x88\x91\xe4\xbb\xac\xe8\xa6\x81\xe4\xb8\x80\xe8\xb5\xb7\xe6\xa3\x80"
                                "\xe6\x9f\xa5\xe7\xac\xac\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84\xe6\x9c\x80\xe7\xbb\x88\xe6\xb7\xb7"
                                "\xe9\x9f\xb3\xe3\x80\x82";   // U+4ECA,5929,6211,4EEC,8981,4E00,8D77,68C0,67E5,7B2C,4E8C,573A,7684,6700,7EC8,6DF7,97F3,3002
            const char* kEn = "Today we are going to check the final mix of the second scene together.";
            auto oneCue = [&](FileJob& job, const char* lang, const std::string& text, const std::string& reply,
                              trpipe::WireCue& got) -> bool {
                CollectSink s1;
                FileCue c;
                c.start = 20.0 + job.sent; c.end = c.start + 1.5; c.lang = lang; c.text = text;
                job.toTranslate.push_back(c);
                g_mtReplyForTest = reply;
                std::string e1;
                const bool stepOk = translateOneCue(job, srv, s1, e1) == FileStep::Ok;
                g_mtReplyForTest.clear();
                const JobRun jr = parseJobRun(s1.lines);
                if (!stepOk || jr.cues.size() != 1) return false;
                got = jr.cues[0];
                return true;
            };
            std::unique_ptr<FileJob> zj;
            trpipe::WireFile zf;
            zf.jobId = ++id; zf.path = Fu + "c.mp3"; zf.langA = "en"; zf.langB = "zh";
            std::string zerr;
            const bool zmade = makeFileJob(zf, "en", "zh", "auto", cfg, zj, zerr);
            trpipe::WireCue ca, cb, cc;
            const bool okA = zmade && oneCue(*zj, "zh", kTrad, kEn, ca);
            const bool okB = zmade && oneCue(*zj, "en", kEn, kTrad, cb);
            const bool okC = made && oneCue(*tj, "en", kEn, kTrad, cc);
            std::printf("    en<->zh: a Traditional zh transcript -> %s | a Traditional MT reply -> %s\n"
                        "    en<->zh-tw: a Traditional MT reply -> %s\n",
                        okA ? ca.src.c_str() : "(failed)", okB ? cb.dst.c_str() : "(failed)",
                        okC ? cc.dst.c_str() : "(failed)");
            ok(okA && ca.lang == "zh" && ca.dstLang == "en" && ca.src == kSimp,
               "the FILE path's Simplified net, source: a zh cue written in Traditional is shown Simplified");
            ok(okB && cb.lang == "en" && cb.dstLang == "zh" && cb.dst == kSimp,
               "the FILE path's Simplified net, destination: a translation into zh is shown Simplified "
               "whatever the model wrote");
            ok(okC && cc.dstLang == "zh-tw" && cc.dst == kTrad, "a zh-tw translation is NEVER simplified");
        }

        // 4. The edit list, seen from the cues.
        std::printf("\n[7c] v_late.mp4  (the same audio, 1.499 s later on the container's timeline)\n");
        r = runFileJob(srv, cfg, ++id, Fu + "v_late.mp4", "en", "zh-tw", "auto", "auto", false);
        failures += checkJobWire(r, id, "v_late.mp4", true);
        if (!r.cues.empty() && firstV) {
            const double d = ((double)r.cues.front().startMs - (double)firstV) / 1000.0;
            std::printf("    first cue %.3f s later than on v.mp4\n", d);
            ok(std::fabs(d - 1.499) < 0.07, "the cues follow the edit list (+1.499 s, +-70 ms)");
        } else {
            ok(false, "v.mp4 and v_late.mp4 both produced cues");
        }

        // 5. CANCEL mid-job, through the request queue: at the next step boundary.
        std::printf("\n[7c] FCANCEL after the first FCUE\n");
        {
            Shared sh;
            CollectSink sink;
            trpipe::WireFile f;
            f.jobId = ++id; f.path = Fu + "c.mp3"; f.langA = "en"; f.langB = "zh";
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(f); }
            std::unique_ptr<FileJob> job;
            fileRequests(sh, job, srv, sink, cfg, nullptr);
            for (int laps = 0; job && laps < 2000 && countKind(sink.lines, "FCUE") == 0; ++laps)
                fileJobLap(job, srv, sink, sh.abortJob, false, nullptr);
            const size_t cuesBefore = countKind(sink.lines, "FCUE");
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileCancels.push_back(f.jobId); }
            const double t0 = qpcMs();
            fileRequests(sh, job, srv, sink, cfg, nullptr);       // the next lap's first act
            const double ms = qpcMs() - t0;
            const JobRun cr = parseJobRun(sink.lines);
            std::printf("    FDONE %s after %zu cue(s), %.0f ms after the FCANCEL was taken\n",
                        cr.dones.empty() ? "-" : cr.dones.back().result.c_str(), cuesBefore, ms);
            ok(cuesBefore >= 1 && !job && cr.dones.size() == 1 && cr.dones.back().result == "cancelled"
               && cr.dones.back().count == cuesBefore && !cr.kinds.empty() && cr.kinds.back() == "FDONE",
               "FCANCEL: one FDONE 'cancelled', last, counting the cues sent");
            ok(ms < 1500.0, "the cancel costs no model call (FDONE within 1.5 s, the job's teardown included)");
        }

        // 6. SUPERSEDE: two FILEs taken in one lap -- the first never starts and still gets its FDONE.
        std::printf("\n[7c] FILE A + FILE B back to back\n");
        {
            Shared sh;
            CollectSink sink;
            trpipe::WireFile fa1, fb1;
            fa1.jobId = ++id; fa1.path = Fu + "c.mp3";
            fb1.jobId = ++id; fb1.path = Fu + "s16.aiff";
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(fa1); sh.fileReqs.push_back(fb1); }
            std::unique_ptr<FileJob> job;
            fileRequests(sh, job, srv, sink, cfg, nullptr);
            while (job) fileJobLap(job, srv, sink, sh.abortJob, false, nullptr);
            int doneA = 0, doneB = 0;
            std::string resA, resB;
            for (const auto& m : sink.lines) {
                trpipe::WireFileDone d;
                if (!trpipe::unpackFileDone(trpipe::split(m), d)) continue;
                if (d.jobId == fa1.jobId) { ++doneA; resA = d.result; }
                if (d.jobId == fb1.jobId) { ++doneB; resB = d.result; }
            }
            trpipe::WireFileDone first;
            const bool firstIsA = !sink.lines.empty() && trpipe::unpackFileDone(trpipe::split(sink.lines.front()), first)
                                  && first.jobId == fa1.jobId;
            std::printf("    A: %d FDONE (%s)   B: %d FDONE (%s), %zu cue(s)\n", doneA, resA.c_str(), doneB,
                        resB.c_str(), countKind(sink.lines, "FCUE"));
            ok(doneA == 1 && resA == "cancelled" && firstIsA, "a superseded FILE gets its one FDONE 'cancelled', first");
            ok(doneB == 1 && resB == "ok" && countKind(sink.lines, "FCUE") >= 1,
               "... and the FILE that superseded it (an AIFF) runs to 'ok' with cues");
        }

        // 7. REFUSALS: an FDONE error, never a substituted value (WC).
        std::printf("\n[7c] refusals\n");
        {
            r = runFileJob(srv, cfg, ++id, Fu + "garbage.mp4", "en", "zh", "auto", "auto", false);
            ok(r.dones.size() == 1 && r.dones.back().result == "error" && r.cues.empty() && r.progs.empty()
               && r.dones.back().err.find("Convert it") != std::string::npos,
               "an unreadable file: FDONE error with the human sentence");
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "xx-yy", "zh", "auto", "auto", false);
            ok(r.dones.size() == 1 && r.dones.back().result == "error" && r.progs.empty(),
               "an unsupported language code: FDONE error, nothing started");
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "en", "auto", "auto", false);
            ok(r.dones.size() == 1 && r.dones.back().result == "error", "a pair of one language: FDONE error");
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "sideways", "auto", false);
            ok(r.dones.size() == 1 && r.dones.back().result == "error", "an unknown direction: FDONE error");
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "auto", "ultra", false);
            ok(r.dones.size() == 1 && r.dones.back().result == "error", "an unknown quality: FDONE error");
            g_fileLargeModelForTest = "ggml-large-v3-NOT-INSTALLED.bin";
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "auto", "max", false);
            g_fileLargeModelForTest.clear();
            ok(r.dones.size() == 1 && r.dones.back().result == "error" && r.progs.empty()
               && r.dones.back().err.find("ggml-large-v3-NOT-INSTALLED.bin") != std::string::npos,
               "quality max without its model: FDONE error NAMING the missing file, before any load");
            for (const auto& d : r.dones) std::printf("    max, model absent -> %s: %s\n", d.result.c_str(), d.err.c_str());
        }

        // 7b. WB, after the review: the job's own defects, end to end.
        std::printf("\n[7c] review fixes: a decode that breaks off, a stale abort, no Media Foundation\n");
        {
            // P8 THE FILE BREAKS OFF at 20 s: every cue heard before goes out, THEN the FDONE error.
            g_fileFailDecodeAtForTest = 20.0;
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "auto", "auto", false);
            g_fileFailDecodeAtForTest = 0.0;
            uint64_t lastEnd = 0;
            for (const auto& c : r.cues) lastEnd = std::max(lastEnd, c.endMs);
            std::printf("    broken at 20 s -> FDONE %s \"%s\" after %zu cue(s), the last ending at %.2f s\n",
                        r.dones.empty() ? "-" : r.dones.back().result.c_str(), r.dones.empty() ? "" : r.dones.back().err.c_str(),
                        r.cues.size(), lastEnd / 1000.0);
            ok(r.dones.size() == 1 && r.dones.back().result == "error" && r.dones.back().err.find("damaged") != std::string::npos
               && r.cues.size() >= 4 && r.dones.back().count == r.cues.size() && lastEnd <= 24500
               && !r.kinds.empty() && r.kinds.back() == "FDONE",
               "a decode error mid-file: the cues already heard are sent, then ONE FDONE error with the reader's sentence");
            // P2 A STALE ABORT (an FCANCEL for a job that was ending) never touches the next job.
            {
                Shared sh;
                CollectSink sink;
                sh.abortJob.store(id, std::memory_order_release);          // the job above: over
                trpipe::WireFile f;
                f.jobId = ++id; f.path = Fu + "s16.aiff"; f.langA = "en"; f.langB = "zh";
                { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(f); }
                std::unique_ptr<FileJob> job;
                fileRequests(sh, job, srv, sink, cfg, nullptr);
                int laps = 0;
                for (; job && laps < 80; ++laps) fileJobLap(job, srv, sink, sh.abortJob, false, nullptr);
                if (job) finishJob(job, srv, sink, "cancelled", std::string(), nullptr);
                const JobRun sr = parseJobRun(sink.lines);
                std::printf("    with a stale abort for job %llu pending: job %llu -> %s in %d lap(s)\n",
                            (unsigned long long)(f.jobId - 1), (unsigned long long)f.jobId,
                            sr.dones.empty() ? "-" : sr.dones.back().result.c_str(), laps);
                ok(sr.dones.size() == 1 && sr.dones.back().result == "ok" && !sr.cues.empty(),
                   "an abort raised for a finished job never stops the next one's model load (abort by job id)");
            }
            // THE DEGENERATE RETRY, deterministically: the first decode of the first window LOOPS (a
            // sentence repeated, as turbo does under a prompt in the wrong language), the retry is
            // clean -- only the clean one may reach the screen. (Stand-in decodes: the real models
            // loop only on some runs, so the meeting fixture cannot be relied on to exercise this.)
            {
                auto seg = [](int id, double s, double e, const char* t) {
                    char b[512];
                    std::snprintf(b, sizeof(b),
                                  "{\"id\":%d,\"text\":\"%s\",\"start\":%.2f,\"end\":%.2f,\"tokens\":[],\"words\":["
                                  "{\"word\":\"%s\",\"start\":%.2f,\"end\":%.2f,\"t_dtw\":-1,\"probability\":0.9}],"
                                  "\"temperature\":0.0,\"avg_logprob\":-0.1,\"no_speech_prob\":0.0}", id, t, s, e, t, s, e);
                    return std::string(b);
                };
                auto doc = [](const std::string& text, const std::string& segs) {
                    return "{\"task\":\"transcribe\",\"language\":\"english\",\"duration\":5.0,\"text\":\"" + text
                         + "\",\"segments\":[" + segs + "],\"detected_language\":\"english\","
                           "\"detected_language_probability\":0.99}";
                };
                const char* kLoop = " The next step is to start.";
                g_fileAsrRepliesForTest.push_back(doc(" The next step is to start. x4",
                    seg(0, 0.3, 1.5, kLoop) + "," + seg(1, 1.5, 2.7, kLoop) + "," + seg(2, 2.7, 3.9, kLoop) + ","
                    + seg(3, 3.9, 5.0, kLoop)));
                g_fileAsrRepliesForTest.push_back(doc(" Good morning everyone. Today we are going to review the final mix.",
                    seg(0, 0.3, 1.9, " Good morning everyone.") + ","
                    + seg(1, 2.2, 5.0, " Today we are going to review the final mix.")));
                r = runFileJob(srv, cfg, ++id, Fu + "s16.aiff", "en", "zh", "auto", "auto", false);
                const size_t unused = g_fileAsrRepliesForTest.size();
                g_fileAsrRepliesForTest.clear();
                std::string all;
                for (const auto& c : r.cues) all += c.src + " | ";
                std::printf("    a looping first decode, then a clean retry -> %u re-decoded; cues: %s\n",
                            r.st.degenerate, all.c_str());
                ok(unused == 0 && r.st.degenerate >= 1 && all.find("Good morning") != std::string::npos
                   && all.find("next step") == std::string::npos && !r.dones.empty() && r.dones.back().result == "ok",
                   "a looping decode is decoded again, and only the clean retry reaches the screen");
            }
            // #1 NO MEDIA FOUNDATION: the FILE gets its FDONE error with the sentence -- and the helper lives on.
            setMediaFoundationDllForTest(L"mfplat_absent_selftest.dll");
            r = runFileJob(srv, cfg, ++id, Fu + "c.mp3", "en", "zh", "auto", "auto", false);
            setMediaFoundationDllForTest(L"");
            ok(r.dones.size() == 1 && r.dones.back().result == "error"
               && r.dones.back().err.find("Media Feature Pack") != std::string::npos && r.progs.empty(),
               "Media Foundation absent: FDONE error with the Media Feature Pack sentence, nothing started");
        }

        // 8. RTF on the bilingual meeting: turbo (auto) vs large-v3 (max) -- and how much of what
        //    was said survives, against the per-utterance transcripts (meeting_refs.txt: line 1 the
        //    18 English utterances, line 2 the 6 Chinese ones, each transcribed ALONE by turbo).
        std::printf("\n[7c] meeting.wav (104 s, 4 voices, English + Chinese) -- real-time factor\n");
        std::string refEn, refZh;
        if (FILE* fr = _wfopen((F + L"meeting_refs.txt").c_str(), L"rb")) {
            std::string all;
            char buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), fr)) > 0;) all.append(buf, n);
            std::fclose(fr);
            const size_t nl = all.find('\n');
            if (nl != std::string::npos) {
                refEn = all.substr(0, nl);
                refZh = all.substr(nl + 1);
                while (!refZh.empty() && (refZh.back() == '\n' || refZh.back() == '\r')) refZh.pop_back();
            }
        }
        for (const char* q : { "auto", "max" }) {
            if (std::string(q) == "max" && !srv.modelInstalled("ggml-large-v3.bin")) {
                std::printf("    max: NOT CHECKED (ggml-large-v3.bin is not installed)\n");
                continue;
            }
            r = runFileJob(srv, cfg, ++id, Fu + "meeting.wav", "en", "zh", "auto", q, false);
            failures += checkJobWire(r, id, q, true);
            printJobStats(r.st);
            int zhKept = 0;
            std::string en, zh;
            for (const auto& c : r.cues) {
                if (c.lang == "zh") { ++zhKept; zh += c.src; }
                else en += c.src + " ";
            }
            std::printf("    quality %s: %zu cue(s), %d of them Chinese (the meeting has 6 Chinese lines)\n",
                        q, r.cues.size(), zhKept);
            ok(r.cues.size() >= 20 && zhKept >= 4, std::string("meeting (") + q + "): >= 20 cues, >= 4 of them Chinese");
            if (refEn.empty() || refZh.empty()) {
                std::printf("    coverage: NOT CHECKED (no meeting_refs.txt)\n");
                continue;
            }
            const double ce = coverage(refEn, en), cz = coverage(refZh, zh);
            std::printf("    coverage: %.0f%% of the English words, %.0f%% of the Chinese characters\n", ce * 100, cz * 100);
            // 99 % / 97 % measured (turbo / large-v3); a decode loop accepted as is: 83 % (mutation F11).
            ok(ce >= 0.92 && cz >= 0.60, std::string("meeting (") + q + "): >= 92 % of the English words and "
                                         ">= 60 % of the Chinese characters survive");
        }

        // 9. After the jobs. SUVTITLE SPLIT: AudioPB checked here that its LIVE pair came back ([7d], ensureUp
        //    + a live final), then drove the live finals' script nets ([7e]: processSegment / processPartial
        //    with a stand-in MT reply) and the live onset gate ([7f]: processSegment on quiet speech and noise)
        //    -- the live path, which SuVtitle does not have (so [7f]'s CPU-only flake, pre-existing in AudioPB,
        //    cannot occur here). The FILE path's script nets are [7c]'s (translateOneCue, the zh-tw jobs) and
        //    [0ter]'s. What the FILE half of [7d] proved stays: after FDONE the job's whisper is gone (endFile).
        std::printf("\n[7d] after the jobs: the job's whisper is gone ([7e] [7f]: AudioPB's live path, not here)\n");
        ok(!srv.fileUp(), "after FDONE the job's whisper is gone (endFile)");
    }
}

// ===========================================================================
// SELFTEST [8]: FILE MODE v2 -- FOLLOW THE PLAYHEAD (wave WD)
//
//   [8a] the SEEK, offline: seek(T) then 1.5 s against the linear decode at T, on every container
//        family (wav 16k / 44.1k, aiff, mp3, avi, m4a, mp4 + edit list, mkv, mka without cues, ts,
//        a 35 s lead): the same frame (lag 0) and the same samples -- exactly for 16 kHz PCM, to the
//        resampler's float LSB for PCM / AIFF / MP3, within the AAC decoder's noise substitution
//        for AAC; position() lands on T; a target past the end is eof().
//   [8b] the planner's v2 rules, offline: a window never crosses a limit (kLimit), the run's start
//        on a breath before the playhead, words cut at a limit the speech runs across, cues kept
//        inside their run, the energy track by regions, the ranges arithmetic.
//   [8c] WHOLE v2 JOBS, offline and deterministic: the shipping request handler + job lap with a
//        stand-in whisper that knows where the window is (meeting.wav's 24 utterances, from
//        meeting_marks.txt) and a stand-in MT that sees its context. v1 compatibility (the windows
//        the v1 binary planned), `have`, the focus, FFOCUS mid-job, a straddling cue at a join,
//        the context reset, WE's Q1-Q4, a burst of FFOCUS, a malformed `have`, cancel + resume.
//   [8d] live (the real models): a focus in the middle of the meeting, and FFOCUS mid-job.
// ===========================================================================
struct UttMark { std::string name; double s = 0.0, e = 0.0; };

std::vector<UttMark> readMarks(const std::wstring& path) {
    std::vector<UttMark> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char name[64] = {};
        double s = 0, e = 0;
        if (std::sscanf(line, "%63s %lf %lf", name, &s, &e) == 3 && e > s) out.push_back({ name, s, e });
    }
    std::fclose(f);
    return out;
}

// A stand-in WHISPER for the window [s, e) (samples): every utterance it overlaps, as words of 0.4 s
// (" u<i><letter>"), cut at the window's end, a full stop on an utterance that ends inside the window.
// Times relative to the window, as whisper gives them. Deterministic, position-aware.
std::string standInWhisper(const std::vector<UttMark>& marks, uint64_t s, uint64_t e) {
    const double ws = (double)s / kFileRate, we = (double)e / kFileRate;
    std::string segs, text;
    int id = 0;
    char b[256];
    for (size_t i = 0; i < marks.size(); ++i) {
        const UttMark& u = marks[i];
        if (u.e <= ws || u.s >= we) continue;
        std::string words, stext;
        int k = 0;
        double first = -1, last = -1;
        for (double t = u.s; t < u.e - 1e-9; t += 0.4, ++k) {
            const double a = t, z = std::min(t + 0.4, u.e);
            if (a < ws - 1e-9) continue;                  // heard from the window's start on only
            if (a >= we - 0.02) break;                    // nothing past its end
            const double zz = std::min(z, we);
            const bool endsHere = (z >= u.e - 1e-9) && (u.e <= we - 0.05);
            std::string w = " u" + std::to_string(i) + (char)('a' + (k % 26)) + (k >= 26 ? std::to_string(k / 26) : std::string());
            if (endsHere) w += ".";
            std::snprintf(b, sizeof(b), "%s{\"word\":\"%s\",\"start\":%.2f,\"end\":%.2f,\"t_dtw\":-1,\"probability\":0.9}",
                          words.empty() ? "" : ",", w.c_str(), a - ws, zz - ws);
            words += b;
            stext += w;
            if (first < 0) first = a - ws;
            last = zz - ws;
        }
        if (words.empty()) continue;
        std::snprintf(b, sizeof(b), "%s{\"id\":%d,\"text\":\"", segs.empty() ? "" : ",", id++);
        segs += b + stext;
        std::snprintf(b, sizeof(b), "\",\"start\":%.2f,\"end\":%.2f,\"tokens\":[],\"words\":[", first, last);
        segs += b + words + "],\"temperature\":0.0,\"avg_logprob\":-0.1,\"no_speech_prob\":0.0}";
        text += stext;
    }
    return "{\"task\":\"transcribe\",\"language\":\"english\",\"duration\":" + std::to_string(we - ws)
         + ",\"text\":\"" + text + "\",\"segments\":[" + segs
         + "],\"detected_language\":\"english\",\"detected_language_probability\":0.99}";
}

struct AsrCall { uint64_t s = 0, e = 0; std::string prompt; size_t afterLines = 0; };
struct MtCall  { std::string src; size_t ctx = 0; size_t afterLines = 0; };

struct V2Run {
    std::vector<std::string> lines;
    FileJobStats             st;
    std::vector<AsrCall>     asr;      // s/e = the audio whisper HEARD (a join's pre/post-roll included)
    std::vector<MtCall>      mt;
};

// [8]'s VIRTUAL CLOCK (review C1): the job's clock (g_fileClockForTest) reads `now`, and every stand-in
// model call advances it by what that call costs on the machine simulated -- a CPU-only one: ~55 s a
// whisper window, ~3 s a 4B translation (WD's CPU log) -- so a minute of it costs no real minute.
struct V2Clock {
    uint64_t now = 1000000;
    uint64_t asrMs = 0, mtMs = 0;
};

// One FILE through the SHIPPING request handler + job lap, lap by lap, with the stand-ins installed.
// `perLap(sh, run, lap)` runs after every lap -- `run` as it stands (its wire, its model calls so far) --
// and may set the focus slot, queue an FCANCEL, a FILE. The FILE itself goes through servantHandle.
V2Run driveV2(Servers& srv, const TrConfig& cfg, const std::vector<UttMark>& marks, const trpipe::WireFile& f,
              const std::function<void(Shared&, const V2Run&, int)>& perLap = {},
              int maxLaps = 20000, V2Clock* clock = nullptr) {
    V2Run r;
    Shared sh;
    CollectSink sink;
    g_fileNoModelsForTest = true;
    if (clock) g_fileClockForTest = [clock] { return clock->now; };
    g_fileAsrForTest = [&](uint64_t s, uint64_t e, const std::string& prompt) {
        r.asr.push_back({ s, e, prompt, sink.lines.size() });
        if (clock) clock->now += clock->asrMs;
        return standInWhisper(marks, s, e);
    };
    g_fileMtForTest = [&](const std::string& src, const std::string&, const std::string&, size_t ctx) {
        r.mt.push_back({ src, ctx, sink.lines.size() });
        if (clock) clock->now += clock->mtMs;
        return std::string("T(") + src + ")";
    };
    servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(f)));    // through the WIRE and the door
    std::unique_ptr<FileJob> job;
    for (int lap = 0; lap < maxLaps; ++lap) {
        fileRequests(sh, job, srv, sink, cfg, &r.st);
        if (!job) {
            bool more = false;
            { std::lock_guard<std::mutex> lk(sh.mx); more = !sh.fileReqs.empty(); }
            if (!more) break;
            continue;
        }
        fileJobLap(job, srv, sink, sh.abortJob, false, &r.st);
        if (clock) clock->now += 20;            // a lap's own time (the real loop sleeps 20 ms when idle)
        if (perLap) { r.lines = sink.lines; perLap(sh, r, lap); }
    }
    if (job) finishJob(job, srv, sink, "cancelled", "selftest: lap budget", &r.st);
    g_fileAsrForTest = nullptr;
    g_fileMtForTest = nullptr;
    g_fileClockForTest = nullptr;
    g_fileNoModelsForTest = false;
    r.lines = sink.lines;
    return r;
}

// Set the playhead slot as the servant does (servantHandle), for the running job -- a SEEK (the chain
// broken: judged by the cursor rule) unless `played`. `settled` = it arrived long enough ago.
void injectFocus(Shared& sh, uint64_t ms, bool settled, bool played = false) {
    std::lock_guard<std::mutex> lk(sh.mx);
    const uint64_t now = fileNowMs();
    sh.focusJob  = sh.runningJob;
    sh.focusMs   = ms;
    sh.focusTick = now - (settled ? 10000 : 0);
    if (!played) sh.focusJump = true;
    sh.chainJob  = sh.runningJob;
    sh.chainMs   = ms;
    sh.chainTick = now;
    ++sh.focusSeq;
    sh.focusJobLive.store(sh.focusJob);
    sh.focusMsLive.store(ms);
    sh.focusSeqLive.store(sh.focusSeq);
}

bool rangesIntersect(uint64_t s, uint64_t e, const std::vector<trpipe::MsRange>& r) {
    for (const auto& x : r) if (s < x.e && x.s < e) return true;
    return false;
}

// The v2 wire invariants of one job (TrPipe.h "FILE MODE v2" + WE's Q1). Returns the failures.
int checkV2Wire(const std::vector<std::string>& lines, uint64_t jobId, const char* label,
                const std::vector<trpipe::MsRange>& have, bool expectOk, uint64_t totalMs,
                std::vector<trpipe::MsRange>* lastRanges = nullptr) {
    int f = 0;
    auto fail = [&](const std::string& what) { std::printf("    ASSERT FAIL [%s]: %s\n", label, what.c_str()); ++f; };
    std::vector<trpipe::WireCue> cues;
    std::vector<size_t> cueAt;
    std::vector<std::pair<size_t, std::vector<trpipe::MsRange>>> progs;
    int dones = 0;
    std::string result;
    uint32_t count = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto t = trpipe::split(lines[i]);
        trpipe::WireCue c; trpipe::WireFileProg p; trpipe::WireFileDone d;
        if (trpipe::unpackCue(t, c) && c.jobId == jobId) { cues.push_back(c); cueAt.push_back(i); }
        else if (trpipe::unpackFileProg(t, p) && p.jobId == jobId) {
            std::vector<trpipe::MsRange> r;
            if (!trpipe::parseRanges(p.ranges, r)) fail("an FPROG's ranges do not parse: " + p.ranges);
            if (p.doneMs != trpipe::rangesTotal(r)) fail("FPROG doneMs != the total of its ranges");
            progs.push_back({ i, r });
        } else if (trpipe::unpackFileDone(t, d) && d.jobId == jobId) {
            ++dones; result = d.result; count = d.count;
            if (i + 1 != lines.size()) {
                bool laterOwn = false;
                for (size_t k = i + 1; k < lines.size(); ++k) {
                    const auto u = trpipe::split(lines[k]);
                    if (u.size() >= 2 && trpipe::jobIdOf(u[1]) == jobId) laterOwn = true;
                }
                if (laterOwn) fail("a message of the job after its FDONE");
            }
        }
    }
    if (dones != 1) fail("exactly one FDONE (got " + std::to_string(dones) + ")");
    if (count != cues.size()) fail("FDONE count " + std::to_string(count) + " != FCUEs " + std::to_string(cues.size()));
    if (expectOk && result != "ok") fail("result " + result);
    for (size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].idx != i) fail("idx " + std::to_string(cues[i].idx) + " at position " + std::to_string(i));
        if (cues[i].endMs <= cues[i].startMs) fail("a cue ends before it starts");
        if (rangesIntersect(cues[i].startMs, cues[i].endMs, have))
            fail("FCUE " + std::to_string(i) + " [" + std::to_string(cues[i].startMs) + ", " + std::to_string(cues[i].endMs)
                 + ") overlaps a `have` range");
    }
    std::vector<trpipe::WireCue> sorted = cues;
    std::sort(sorted.begin(), sorted.end(), [](const trpipe::WireCue& a, const trpipe::WireCue& b) { return a.startMs < b.startMs; });
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].startMs < sorted[i - 1].endMs)
            fail("two cues overlap: [" + std::to_string(sorted[i - 1].startMs) + ", " + std::to_string(sorted[i - 1].endMs)
                 + ") and [" + std::to_string(sorted[i].startMs) + ", " + std::to_string(sorted[i].endMs) + ")");
    // ranges: they include `have`, they only GROW, and (Q1) no cue sent after an FPROG touches its ranges
    std::vector<trpipe::MsRange> prev;
    for (const auto& pr : progs) {
        std::vector<trpipe::MsRange> h = have;
        for (const auto& x : h) {
            bool in = false;
            for (const auto& y : pr.second) in = in || (y.s <= x.s && x.e <= y.e);
            if (!in) { fail("an FPROG's ranges do not include `have`"); break; }
        }
        for (const auto& x : prev) {
            bool in = false;
            for (const auto& y : pr.second) in = in || (y.s <= x.s && x.e <= y.e);
            if (!in) { fail("the ranges SHRANK between two FPROGs"); break; }
        }
        prev = pr.second;
        for (size_t c = 0; c < cues.size(); ++c)
            if (cueAt[c] > pr.first && rangesIntersect(cues[c].startMs, cues[c].endMs, pr.second)) {
                fail("Q1: FCUE " + std::to_string(c) + " [" + std::to_string(cues[c].startMs) + ", " + std::to_string(cues[c].endMs)
                     + ") was sent AFTER an FPROG whose ranges already covered it: " + trpipe::packRanges(pr.second));
                break;
            }
        // ... and the other way round: every cue already SENT lies inside this FPROG's ranges -- strictly, in whole
        // ms (review P5: a range end floored from samples sat 1 ms short of a cue end rounded to the nearest ms)
        for (size_t c = 0; c < cues.size(); ++c) {
            if (cueAt[c] > pr.first) continue;
            bool in = false;
            for (const auto& y : pr.second) in = in || (y.s <= cues[c].startMs && cues[c].endMs <= y.e);
            if (!in) {
                fail("FCUE " + std::to_string(c) + " [" + std::to_string(cues[c].startMs) + ", " + std::to_string(cues[c].endMs)
                     + ") is not inside the ranges of an FPROG sent after it: " + trpipe::packRanges(pr.second));
                break;
            }
        }
    }
    if (expectOk) {
        if (progs.empty()) fail("no FPROG");
        else {
            const auto& last = progs.back().second;
            if (last.size() != 1 || last[0].s != 0 || (totalMs && last[0].e < totalMs))
                fail("the last FPROG of an ok job does not cover the file: " + trpipe::packRanges(last));
        }
    }
    // every cue lies INSIDE the ranges of the last FPROG (the one before FDONE, whatever the result) --
    // STRICT, in whole ms (review P5: a cue's end rounded to the nearest ms used to pass its range by 1)
    if (!progs.empty() && (cueAt.empty() || progs.back().first > cueAt.back())) {
        const auto& last = progs.back().second;
        for (size_t c = 0; c < cues.size(); ++c) {
            bool in = false;
            for (const auto& y : last) in = in || (y.s <= cues[c].startMs && cues[c].endMs <= y.e);
            if (!in) {
                fail("FCUE " + std::to_string(c) + " [" + std::to_string(cues[c].startMs) + ", " + std::to_string(cues[c].endMs)
                     + ") lies outside the final ranges " + trpipe::packRanges(last));
                break;
            }
        }
    }
    if (lastRanges) *lastRanges = progs.empty() ? std::vector<trpipe::MsRange>() : progs.back().second;
    return f;
}

std::vector<trpipe::WireCue> cuesOfRun(const std::vector<std::string>& lines) {
    std::vector<trpipe::WireCue> out;
    for (const auto& m : lines) { trpipe::WireCue c; if (trpipe::unpackCue(trpipe::split(m), c)) out.push_back(c); }
    return out;
}

// WD_V2_DEBUG=1: the whole sequence of a [8c] job (model calls interleaved with the wire).
void dumpV2Run(const char* label, const V2Run& r) {
    wchar_t v[4] = {};
    if (!::GetEnvironmentVariableW(L"WD_V2_DEBUG", v, 4)) return;
    std::printf("    ---- %s: %zu line(s), %zu asr, %zu mt\n", label, r.lines.size(), r.asr.size(), r.mt.size());
    size_t ai = 0, mi = 0;
    for (size_t i = 0; i <= r.lines.size(); ++i) {
        while (ai < r.asr.size() && r.asr[ai].afterLines == i) {
            std::printf("      ASR %.2f-%.2f prompt[%zu]\n", r.asr[ai].s / 16000.0, r.asr[ai].e / 16000.0, r.asr[ai].prompt.size());
            ++ai;
        }
        while (mi < r.mt.size() && r.mt[mi].afterLines == i) { std::printf("      MT  ctx %zu: %s\n", r.mt[mi].ctx, r.mt[mi].src.c_str()); ++mi; }
        if (i < r.lines.size()) std::printf("      %s\n", r.lines[i].substr(0, 150).c_str());
    }
}

void fileModeV2Selftest(Servers& srv, const TrConfig& cfg, int& failures, bool live) {
    auto ok = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what.c_str()); ++failures; }
    };
    const std::wstring F = testFilemodeDir();           // "" when SUVTITLE_TEST_MEDIA is not set: no fixtures, NOT CHECKED
    const std::string  Fu = wideToUtf8L(F);
    const DWORD fa = F.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(F.c_str());
    const bool haveFixtures = fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);

    // ---------------------------------------------------------------- [8a]
    std::printf("\n[8a] FILE MODE v2 -- the seek, against the linear decode\n");
    if (!haveFixtures) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no file-mode fixtures (<SUVTITLE_TEST_MEDIA>\\filemode)\n");
    } else {
        const int before = failures;
        // tol: 0 = bit-exact; < 0 = AAC (max |diff| <= -tol and a residual under -40 dB)
        struct SCase { const wchar_t* f; double tol; std::vector<double> at; };
        const SCase cases[] = {
            { L"meeting.wav",  0.0,     { 61.4321, 12.0005, 87.77, 1.2345, 104.5 } },
            { L"short44.wav",  2.5e-7,  { 5.4321, 2.5003, 6.9, 0.3, 9.0 } },
            { L"s16.aiff",     2.5e-7,  { 5.4321, 2.5003, 6.9, 0.3, 9.0 } },
            { L"c.mp3",        2.5e-7,  { 25.5432, 9.1004, 31.77, 1.5, 40.0 } },
            { L"v.avi",        2.5e-7,  { 25.5432, 9.1004, 31.77 } },
            { L"c.m4a",       -6e-3,    { 25.5432, 9.1004, 31.77, 1.5, 40.0 } },
            { L"v_late.mp4",  -6e-3,    { 25.5432, 9.1004, 1.2, 2.9, 33.33 } },
            { L"v.mkv",       -6e-3,    { 25.5432, 9.1004, 31.77, 3.3 } },
            { L"art.mka",     -6e-3,    { 20.0001, 4.4444, 30.5 } },
            { L"v.ts",        -6e-3,    { 25.5432, 9.1004, 31.77, 0.5 } },
            { L"late35.mp4",  -6e-3,    { 10.0, 35.5, 36.6, 2.0 } },
        };
        for (const SCase& sc : cases) {
            std::vector<float> ref;
            MediaInfo mi;
            std::string err;
            if (!decodeAll16k(F + sc.f, ref, mi, err)) { ok(false, wideToUtf8L(sc.f) + " decodes (" + err + ")"); continue; }
            MediaReader r;
            if (!r.open(F + sc.f, MediaReader::Out::Mono16k, err)) { ok(false, wideToUtf8L(sc.f) + " opens"); continue; }
            std::string line = "    " + wideToUtf8L(sc.f) + " (" + mi.seekMode + "):";
            for (double tsec : sc.at) {
                const uint64_t T = (uint64_t)std::llround(tsec * kFileRate);
                const double t0 = qpcMs();
                const bool sk = r.seek(T, err);
                const double ms = qpcMs() - t0;
                if (!sk) { ok(false, wideToUtf8L(sc.f) + ": seek(" + std::to_string(tsec) + ") (" + err + ")"); continue; }
                char cell[96];
                if (T >= ref.size()) {
                    std::vector<float> x;
                    for (int k = 0; k < 4 && !r.eof(); ++k) r.readFloat(x, 16000, err);
                    std::snprintf(cell, sizeof(cell), " %.1f->eof %s", tsec, r.lastSeekHow());
                    line += cell;
                    ok(r.eof() && x.empty(), wideToUtf8L(sc.f) + ": a seek past the end leaves the reader at its end");
                    continue;
                }
                ok(r.position() == T, wideToUtf8L(sc.f) + ": position() lands on the target");
                std::vector<float> got;
                while (got.size() < 24000 && !r.eof()) if (!r.readFloat(got, 24000 - got.size(), err)) break;
                const size_t n = std::min(got.size(), ref.size() - (size_t)T);
                // the alignment: the residual is smallest at lag 0 (searched over +-8 samples)
                auto resid = [&](long long lag) {
                    double e = 0, s = 0;
                    for (size_t i = 800; i < n; ++i) {
                        const long long j = (long long)T + (long long)i + lag;
                        if (j < 0 || j >= (long long)ref.size()) continue;
                        const double d = (double)got[i] - ref[(size_t)j];
                        e += d * d; s += (double)ref[(size_t)j] * ref[(size_t)j];
                    }
                    return s > 1e-12 ? e / s : (e > 0 ? 1.0 : 0.0);
                };
                long long best = 0; double be = resid(0);
                for (long long lag = -8; lag <= 8; ++lag) { const double e = resid(lag); if (e < be - 1e-15) { be = e; best = lag; } }
                double mx = 0;
                for (size_t i = 0; i < n; ++i) mx = std::max(mx, (double)std::fabs(got[i] - ref[(size_t)T + i]));
                std::snprintf(cell, sizeof(cell), " %.1f(%s %.0fms d%.1e)", tsec, r.lastSeekHow(), ms, mx);
                line += cell;
                ok(n >= 8000 && best == 0, wideToUtf8L(sc.f) + ": seek(" + std::to_string(tsec) + ") lands on the linear decode's "
                   "frame (lag " + std::to_string(best) + ")");
                if (sc.tol >= 0)
                    ok(mx <= sc.tol, wideToUtf8L(sc.f) + ": seek(" + std::to_string(tsec) + ") samples == the linear decode's "
                       "(max |diff| " + std::to_string(mx) + ")");
                else
                    ok(mx <= -sc.tol && (be == 0.0 || 10.0 * std::log10(be) < -40.0),
                       wideToUtf8L(sc.f) + ": seek(" + std::to_string(tsec) + ") within the AAC decoder's noise (max |diff| "
                       + std::to_string(mx) + ", residual " + std::to_string(be > 0 ? 10.0 * std::log10(be) : -999.0) + " dB)");
            }
            std::printf("%s\n", line.c_str());
        }
        // The MP3 reason in one assert: Media Foundation's own MP3 seek is NOT used (TrMedia.h).
        {
            MediaReader r;
            std::string err;
            r.open(F + L"c.mp3", MediaReader::Out::Mono16k, err);
            r.seek((uint64_t)(20.0 * kFileRate), err);
            ok(std::string(r.info().seekMode) == "decode" && std::string(r.lastSeekHow()) == "forward",
               "MP3 seeks by decoding (Media Foundation's MP3 seek returns audio 9.4 s off its stamp at 1:20:00)");
        }
        // ---- after the review (2026-10-11) ---------------------------------------------------------
        // One seek checked against the linear decode: lands on its frame (lag 0 over +-8) within `tol`.
        auto seekCheck = [&](const std::wstring& f, double tsec, double tol, MediaReader& r, std::string& how,
                             double& decodedSec) {
            std::vector<float> ref;
            MediaInfo mi;
            std::string err;
            if (!decodeAll16k(F + f, ref, mi, err)) return false;
            const uint64_t T = (uint64_t)std::llround(tsec * kFileRate);
            if (!r.info().nativeRate && !r.open(F + f, MediaReader::Out::Mono16k, err)) return false;
            if (!r.seek(T, err) || r.position() != T || T >= ref.size()) return false;
            how = r.lastSeekHow();
            decodedSec = (double)r.lastSeekDecoded() / (mi.nativeRate ? mi.nativeRate : kFileRate);
            std::vector<float> got;
            while (got.size() < 16000 && !r.eof()) if (!r.readFloat(got, 16000 - got.size(), err)) break;
            const size_t n = std::min(got.size(), ref.size() - (size_t)T);
            long long best = 0;
            double be = 1e300;
            for (long long lag = -8; lag <= 8; ++lag) {
                double e = 0;
                for (size_t i = 800; i < n; ++i) {
                    const long long jx = (long long)T + (long long)i + lag;
                    if (jx >= 0 && jx < (long long)ref.size()) { const double d = (double)got[i] - ref[(size_t)jx]; e += d * d; }
                }
                if (e < be - 1e-18) { be = e; best = lag; }
            }
            double mx = 0;
            for (size_t i = 0; i < n; ++i) mx = std::max(mx, (double)std::fabs(got[i] - ref[(size_t)T + i]));
            return n >= 8000 && best == 0 && mx <= tol;
        };
        {
            // P2: MPEG-TS seeks by decoding now (a capture's lost packets / PTS resets put its stamps off the count)
            MediaReader r;
            std::string err;
            r.open(F + L"v.ts", MediaReader::Out::Mono16k, err);
            ok(r.info().seekMode == "decode", "P2: MPEG-TS is no longer a trusted seek container (seeks by decoding)");
        }
        {
            // P3: a target past the AUDIO's end inside the container (v_late.mp4: audio to 36.2 s, the file 38 s):
            // the reader is put at its end -- not re-opened to decode the whole file to find that out
            MediaReader r;
            std::string err;
            r.open(F + L"v_late.mp4", MediaReader::Out::Mono16k, err);
            const bool sk = r.seek((uint64_t)(37.0 * kFileRate), err);
            std::vector<float> x;
            r.readFloat(x, 16000, err);
            const double dec = (double)r.lastSeekDecoded() / r.info().nativeRate;
            std::printf("    v_late.mp4 seek(37.0) past its audio: %s, %.2f s decoded, eof %s\n", r.lastSeekHow(), dec,
                        r.eof() ? "yes" : "no");
            ok(sk && std::string(r.lastSeekHow()) == "end" && r.eof() && x.empty() && dec < 6.0,
               "P3: a seek past the audio's end lands at its end at once (no re-open + decode of the whole file)");
        }
        {
            // C3: an audio GAP across the first 3 s (gap25: AAC, 1 s of stamps missing at 2.5 s, 20 s). The bias
            // learning used to wait for a buffer RECORDED in [2.9, 3.0) s -- none is -- and decoded the whole
            // file into memory first. Bounded now; and the M4A's exact stamps still land on the linear frame.
            MediaReader r;
            std::string how;
            double dec = 0;
            const bool landed = seekCheck(L"gap25.m4a", 15.0, 6e-3, r, how, dec);
            std::printf("    gap25.m4a seek(15.0): %s, %.2f s decoded by the seek\n", how.c_str(), dec);
            ok(landed && how == "mf" && dec < 6.0,
               "C3: a 1 s stamp gap in the first 3 s: the seek's bias learning stops on what it SAW (< 6 s decoded, not the "
               "file), and lands on the linear decode's frame");
            // ... Matroska at 44.1 kHz: ms stamps, AAC priming -- the offset before the gap is -10 samples, and the
            // gap realigns the count on the stamps: the bias learnt before it no longer holds. DEMOTED: decoded, exact.
            MediaReader r2;
            std::string how2;
            double dec2 = 0;
            const bool landed2 = seekCheck(L"gap44.mkv", 15.0, 6e-3, r2, how2, dec2);
            std::printf("    gap44.mkv seek(15.0): %s (seek mode now %s)\n", how2.c_str(), r2.info().seekMode.c_str());
            ok(landed2 && how2 != "mf" && r2.info().seekMode == "decode",
               "P2: a Matroska file whose count is realigned by a gap is demoted -- its seek decodes, on the linear frame");
            // ... a frame LOST mid-stream (drop70.m4a: 64 ms of stamps missing at 6.1 s, under the 100 ms the linear
            // decode fills): the count and the stamps part there. Once the decode has met it the file is demoted, and
            // a seek past it decodes (the container's own would land 64 ms off the linear frame).
            MediaReader r3;
            std::string how3a, how3b;
            double d3 = 0;
            const bool met = seekCheck(L"drop70.m4a", 8.0, 6e-3, r3, how3a, d3);         // decoded forward, through 6.1 s
            const bool landed3 = seekCheck(L"drop70.m4a", 7.0, 6e-3, r3, how3b, d3);     // back past it: the container's?
            std::printf("    drop70.m4a seek(8.0): %s, then seek(7.0): %s\n", how3a.c_str(), how3b.c_str());
            ok(met && landed3 && how3b != "mf",
               "P2: a trusted file whose stamps leave the count (a lost frame) is demoted once met -- seeks past it stay exact");
        }
        {
            // THE DECODER'S FIRST SAMPLE (found on gap25.m4a, 11/10): Media Foundation flags it "type changed" with
            // the very type asked for; setting it again flushed the AAC decoder and lost 0-2 frames at random --
            // the same file decoded differently from one open to the next. Five opens, one decode.
            std::vector<float> first;
            bool same = true;
            for (int k = 0; k < 5 && same; ++k) {
                std::vector<float> x;
                MediaInfo mi;
                std::string err;
                same = decodeAll16k(F + L"gap25.m4a", x, mi, err) && !x.empty() && (k == 0 || x == first);
                if (k == 0) first = x;
            }
            ok(same, "an AAC stream decodes to the SAME samples on every open (its first sample's no-op type change "
                     "no longer flushes the decoder)");
        }
        {
            // P3: an ABORTED seek -- the predicate polled between decoded buffers -- returns at once, refuses a read,
            // and the next seek (re-opened) is exact again. Every decoding path: fastForward (c.mp3 to 30 s), the
            // container's seek (c.m4a), and a plain decode forward (c.mp3 to 8 s: discardTo).
            struct ACase { const wchar_t* f; double t; const char* path; };
            for (const ACase& ac : { ACase{ L"c.mp3", 30.0, "fastForward" }, ACase{ L"c.m4a", 30.0, "the container seek" },
                                     ACase{ L"c.mp3", 8.0, "decoded forward" } }) {
                MediaReader r;
                std::string err;
                r.open(F + ac.f, MediaReader::Out::Mono16k, err);
                int polls = 0;
                const std::function<bool()> stop = [&polls] { return ++polls >= 3; };
                const bool sk = r.seek((uint64_t)(ac.t * kFileRate), err, stop);
                std::vector<float> x;
                const bool readRefused = !r.readFloat(x, 1600, err);
                const bool aborted = !sk && r.lastSeekAborted() && polls == 3 && readRefused;
                std::string how;
                double dec = 0;
                const bool again = seekCheck(ac.f, ac.t, 6e-3, r, how, dec);
                std::printf("    %s seek(%.0f) [%s] aborted on the 3rd poll: %s; the next seek: %s %s\n", wideToUtf8L(ac.f).c_str(),
                            ac.t, ac.path, aborted ? "yes" : "NO", how.c_str(), again ? "exact" : "OFF");
                ok(aborted && again, "P3: " + wideToUtf8L(ac.f) + " (" + ac.path + "): an aborted seek stops at once, refuses "
                   "a read, and the next seek lands exactly");
            }
            // ... and a landing with NOTHING after it (v_late.mp4 at 37.9 s: 1.7 s past its audio, inside the container):
            // an earlier landing is tried, and the reader ends up at its end -- not re-opened to decode it all
            MediaReader r;
            std::string err;
            r.open(F + L"v_late.mp4", MediaReader::Out::Mono16k, err);
            const bool sk = r.seek((uint64_t)(37.9 * kFileRate), err);
            const double dec = (double)r.lastSeekDecoded() / r.info().nativeRate;
            std::printf("    v_late.mp4 seek(37.9), an empty landing: %s, %.2f s decoded, eof %s\n", r.lastSeekHow(), dec,
                        r.eof() ? "yes" : "no");
            ok(sk && std::string(r.lastSeekHow()) == "end" && r.eof() && dec < 6.0,
               "P3: a seek whose landing finds the stream over tries earlier ones and ends at the audio's end (no re-open)");
        }
        std::printf("    %s\n", failures == before ? "seek == linear decode OK" : "SEE FAILURES ABOVE");
    }

    // ---------------------------------------------------------------- [8b]
    std::printf("\n[8b] FILE MODE v2 -- the planner's rules (offline)\n");
    {
        const int before = failures;
        // A stream: 2 s silence, tone 4 s, 0.6 s silence, tone 6 s, silence 0.5 s, tone 40 s, silence 3 s.
        std::vector<float> x;
        auto tone = [&](double sec) {
            const int n = (int)(sec * 16000);
            for (int i = 0; i < n; ++i) x.push_back(0.3f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / 16000.0));
        };
        auto sil = [&](double sec) { x.insert(x.end(), (size_t)(sec * 16000), 0.0f); };
        sil(2.0); tone(4.0); sil(0.6); tone(6.0); sil(0.5); tone(40.0); sil(3.0);
        EnergyTrack en;
        en.push(x.data(), x.size());
        WindowPlan p;
        uint64_t skip = 0;
        // a LIMIT inside speech: the window ends ON it, kLimit
        PlanVerdict v = planWindow(en, 0, x.size(), true, false, p, skip, (uint64_t)(9.0 * 16000));
        ok(v == PlanVerdict::Plan && p.why == WindowPlan::kLimit && p.e == (uint64_t)(9.0 * 16000),
           "a window reaching a run's limit ends exactly ON it (kLimit)");
        // nothing past it, ever: the next plan from the limit is Done
        v = planWindow(en, (uint64_t)(9.0 * 16000), x.size(), true, false, p, skip, (uint64_t)(9.0 * 16000));
        ok(v == PlanVerdict::Done, "from the limit on: Done (the run is over), no window past it");
        // a limit 40 s away: ordinary windows (pause / hard), none reaching past it
        v = planWindow(en, (uint64_t)(13.0 * 16000), x.size(), true, false, p, skip, (uint64_t)(50.0 * 16000));
        ok(v == PlanVerdict::Plan && p.e <= (uint64_t)(50.0 * 16000) && p.why != WindowPlan::kLimit,
           "a far limit leaves the v1 planner alone");
        // a limit in SILENCE after the last speech before it: Done without a window
        v = planWindow(en, (uint64_t)(12.6 * 16000), x.size(), true, false, p, skip, (uint64_t)(13.0 * 16000));
        ok(v == PlanVerdict::Done || (v == PlanVerdict::Plan && p.e <= (uint64_t)(13.0 * 16000)),
           "a limit in silence: no window crosses it");
        // v1 untouched: no limit = the same plan as before
        WindowPlan p1, p2;
        planWindow(en, 0, x.size(), true, true, p1, skip);
        planWindow(en, 0, x.size(), true, true, p2, skip, ~0ull);
        ok(p1.s == p2.s && p1.e == p2.e && p1.why == p2.why, "planWindow without a limit == v1's");
        // runStartCursor: the playhead in speech, a pause 1.2 s before it -> the pause's start
        const uint64_t c1 = runStartCursor(en, (uint64_t)(7.2 * 16000), (uint64_t)(4.2 * 16000));
        ok(c1 == (uint64_t)(6.0 * 16000) / kFileHop * kFileHop || (c1 >= (uint64_t)(5.95 * 16000) && c1 <= (uint64_t)(6.05 * 16000)),
           "a run at the playhead starts on the pause before it (6.0 s), not mid-word (got " + std::to_string(c1 / 16000.0) + ")");
        // ... never before `lo` (an analysed stretch ends there)
        const uint64_t c2 = runStartCursor(en, (uint64_t)(7.2 * 16000), (uint64_t)(6.5 * 16000));
        ok(c2 >= (uint64_t)(6.5 * 16000), "... and never before the gap's start");
        // ... continuous speech for 3 s: the quietest hop, inside [lo, P]
        const uint64_t c3 = runStartCursor(en, (uint64_t)(30.0 * 16000), (uint64_t)(27.0 * 16000));
        ok(c3 >= (uint64_t)(27.0 * 16000) && c3 <= (uint64_t)(30.0 * 16000), "continuous speech: a cursor inside the look-back");
        // review P1: no pause within 3 s but one within 8 s -- the run starts THERE (the sentence heard whole)
        {
            bool mid = true;
            const uint64_t c4 = runStartCursor(en, (uint64_t)(11.0 * 16000), (uint64_t)(8.0 * 16000), (uint64_t)(3.0 * 16000), mid);
            ok(!mid && c4 >= (uint64_t)(5.95 * 16000) && c4 <= (uint64_t)(6.05 * 16000),
               "no breath within 3 s of the playhead (11 s): the pause 5 s before it (6.0 s) starts the run (got "
               + std::to_string(c4 / 16000.0) + ")");
            const uint64_t c5 = runStartCursor(en, (uint64_t)(40.0 * 16000), (uint64_t)(37.0 * 16000), (uint64_t)(32.0 * 16000), mid);
            ok(mid && c5 >= (uint64_t)(37.0 * 16000) && c5 <= (uint64_t)(40.0 * 16000),
               "none within 8 s either: mid-speech, inside the 3 s look-back (the run's first window gets a pre-roll)");
        }
        // commitWindow kLimit: speech ACROSS the limit -> the last words cut; silence at it -> none
        {
            AsrResult a;
            a.language = "en";
            const char* t1 = " One two three.";
            const char* t2 = " four five six";
            AsrSegment g1; g1.start = 0.2; g1.end = 1.5; g1.text = t1; g1.w0 = 0;
            AsrWord w1; w1.text = t1; w1.start = 0.2; w1.end = 1.5; a.words.push_back(w1); g1.w1 = 1;
            AsrSegment g2; g2.start = 1.8; g2.end = 3.0; g2.text = t2; g2.w0 = 1;
            AsrWord w2; w2.text = t2; w2.start = 1.8; w2.end = 3.0; a.words.push_back(w2); g2.w1 = 2;
            a.segments = { g1, g2 };
            CommitInput ci;
            ci.asr = &a; ci.en = &en;
            ci.plan.s = (uint64_t)(6.0 * 16000); ci.plan.e = (uint64_t)(9.0 * 16000); ci.plan.why = WindowPlan::kLimit;
            const CommitVerdict cv = commitWindow(ci);
            ok(cv.words.size() == 2 && !cv.words[0].cut && cv.words[1].cut && cv.nextCursor == ci.plan.e,
               "a limit the speech runs across: the word heard only up to it is CUT, the earlier one is not");
            ci.plan.s = (uint64_t)(2.0 * 16000); ci.plan.e = (uint64_t)(6.3 * 16000);    // the 0.6 s pause at 6.0-6.6
            const CommitVerdict cv2 = commitWindow(ci);
            ok(cv2.words.size() == 2 && !cv2.words[0].cut && !cv2.words[1].cut, "a limit in a pause: nothing is cut");
        }
        // review P1 -- A JOIN IN SPEECH: a POST-ROLLED limit window keeps every word that STARTS before the
        // limit, whole (no cut), and none that starts past it; a PRE-ROLLED one none that starts before
        // its cursor. Whisper's times count from what it heard.
        {
            auto word = [](const char* t, double s, double e) { AsrWord w; w.text = t; w.start = s; w.end = e; return w; };
            AsrResult a;
            a.language = "en";
            a.words = { word(" One", 0.2, 0.6), word(" two.", 0.6, 1.5), word(" four", 1.8, 2.4), word(" five", 2.4, 2.7),
                        word(" six", 2.7, 3.3), word(" seven", 3.4, 4.2) };
            AsrSegment g; g.start = 0.2; g.end = 4.2; g.text = " One two. four five six seven"; g.w0 = 0; g.w1 = a.words.size();
            a.segments = { g };
            CommitInput ci;
            ci.asr = &a; ci.en = &en;
            ci.plan.s = (uint64_t)(6.0 * 16000); ci.plan.e = (uint64_t)(9.0 * 16000); ci.plan.why = WindowPlan::kLimit;
            ci.plan.rolled = true; ci.plan.hs = ci.plan.s; ci.plan.he = (uint64_t)(11.0 * 16000); ci.plan.keepFrom = 0;
            const CommitVerdict cv = commitWindow(ci);
            bool noCut = true;
            for (const auto& w : cv.words) noCut = noCut && !w.cut;
            ok(cv.words.size() == 5 && noCut && std::fabs(cv.words.back().start - 8.7) < 1e-9 && std::fabs(cv.words.back().end - 9.3) < 1e-9
               && cv.nextCursor == ci.plan.e,
               "a post-rolled limit window: every word starting before the limit kept WHOLE (8.7-9.3 s crosses it), none cut, "
               "none starting past it");
            ci.plan.why = WindowPlan::kPause; ci.plan.he = 0;
            ci.plan.hs = (uint64_t)(4.0 * 16000); ci.plan.keepFrom = (uint64_t)(6.0 * 16000);
            const CommitVerdict cv2 = commitWindow(ci);       // the same words, heard from 4.0 s: 4.2 ... 8.2 s
            ok(cv2.words.size() == 3 && std::fabs(cv2.words.front().start - 6.4) < 1e-9,
               "a pre-rolled window: the words starting before its cursor (6.0 s) are the stretch before's -- dropped");
        }
        // CueBuilder::beginRun: cues stay inside the run; a cut word's cue is dropped
        {
            CueCfg cc;
            CueBuilder b;
            b.configure(cc, nullptr);
            b.beginRun(10.0, 20.0, true);
            b.addWords({ FileWord{ 9.9, 10.5, " Early", "en" }, FileWord{ 10.5, 11.0, " start.", "en" },
                         FileWord{ 12.0, 12.5, " Middle", "en" }, FileWord{ 12.5, 13.0, " one.", "en" },
                         FileWord{ 19.5, 19.9, " Last", "en" }, FileWord{ 19.9, 19.99, " words.", "en" } });
            b.finish(20.0);
            std::vector<FileCue> cs;
            FileCue c;
            while (b.pop(c)) cs.push_back(c);
            bool inside = !cs.empty();
            for (const auto& q : cs) inside = inside && q.start >= 10.0 + cc.minGap - 1e-9 && q.end <= 20.0 - cc.minGap + 1e-9;
            ok(inside, "a run's cues start after the stretch before it (+ the gap) and end before its limit (- the gap)");
            CueBuilder b2;
            b2.configure(cc, nullptr);
            b2.beginRun(0.0, 9.0, false);
            FileWord cutw{ 8.0, 9.0, " cut", "en" };
            cutw.cut = true;
            b2.addWords({ FileWord{ 1.0, 1.5, " Kept", "en" }, FileWord{ 1.5, 2.0, " sentence.", "en" },
                          FileWord{ 7.0, 8.0, " Half", "en" }, cutw });
            b2.finish(9.0);
            std::vector<FileCue> cs2;
            while (b2.pop(c)) cs2.push_back(c);
            ok(cs2.size() == 2 && cs2[0].text == "Kept sentence." && cs2[1].text == "Half" && b2.cutWords() == 1
               && b2.droppedCut() == 0,
               "a word cut at the limit is dropped -- its cue keeps the rest (review P1: the whole cue went)");
            CueBuilder b4;
            b4.configure(cc, nullptr);
            b4.beginRun(0.0, 9.0, false);
            FileWord c1w{ 8.2, 8.6, " only", "en" }, c2w{ 8.6, 8.99, " cut", "en" };
            c1w.cut = c2w.cut = true;
            b4.addWords({ FileWord{ 1.0, 1.5, " Alpha.", "en" }, c1w, c2w });
            b4.finish(9.0);
            std::vector<FileCue> cs4;
            while (b4.pop(c)) cs4.push_back(c);
            ok(cs4.size() == 1 && cs4[0].text == "Alpha." && b4.droppedCut() == 1 && b4.cutWords() == 2,
               "... and a cue made of cut words only is dropped");
            // v1 exactly: beginRun(0, inf, false) == configure alone
            CueBuilder v1b, v2b;
            v1b.configure(cc, nullptr);
            v2b.configure(cc, nullptr);
            v2b.beginRun(0.0, std::numeric_limits<double>::infinity(), false);
            const std::vector<FileWord> ws = { FileWord{ 0.0, 0.4, " Good", "en" }, FileWord{ 0.4, 0.9, " morning.", "en" },
                                               FileWord{ 1.5, 2.0, " Second", "en" }, FileWord{ 2.0, 2.4, " one.", "en" } };
            v1b.addWords(ws); v2b.addWords(ws); v1b.finish(3.0); v2b.finish(3.0);
            bool same = true;
            FileCue c1, c2;
            while (v1b.pop(c1)) { same = same && v2b.pop(c2) && c1.start == c2.start && c1.end == c2.end && c1.text == c2.text; }
            ok(same && !v2b.pop(c2), "beginRun(0, inf) builds exactly v1's cues");
            // holdFromStrict is a bound on where a held cue can start
            CueBuilder b3;
            b3.configure(cc, nullptr);
            b3.addWords({ FileWord{ 5.0, 5.5, " Held", "en" } });
            ok(b3.holdFromStrict() <= 5.0 && b3.holdFromStrict() >= 4.5, "holdFromStrict: the held word's start less the snap margin");
        }
        // EnergyTrack by regions: a jump leaves hops unknown (never active); a region decoded twice counts once
        {
            EnergyTrack e2;
            e2.pushAt(0, x.data(), 16000 * 5);
            const size_t k1 = e2.knownHops();
            e2.pushAt(16000 * 40, x.data() + 16000 * 40, 16000 * 3);
            ok(!e2.known(EnergyTrack::hopAt(20.0)) && !e2.active(EnergyTrack::hopAt(20.0)) && e2.known(EnergyTrack::hopAt(41.0)),
               "a seek leaves the hops between the regions UNKNOWN, and unknown is never active");
            const size_t k2 = e2.knownHops();
            e2.pushAt(16000 * 40, x.data() + 16000 * 40, 16000 * 3);
            ok(e2.knownHops() == k2 && k2 > k1, "a region decoded twice is counted once in the floor");
            e2.pushAt(16000 * 40 + 77, x.data() + 16000 * 40 + 77, 1000);      // not on the hop grid
            ok(e2.knownHops() >= k2, "a push off the hop grid re-aligns (no half hop measured)");
            EnergyTrack lin, reg;
            lin.push(x.data(), x.size());
            reg.pushAt(0, x.data(), 12345);
            reg.pushAt(12345, x.data() + 12345, x.size() - 12345);
            bool same = lin.hops() == reg.hops();
            for (size_t h = 0; same && h < lin.hops(); ++h) same = lin.db(h) == reg.db(h);
            ok(same && lin.floorDb() == reg.floorDb(), "contiguous pushAt == v1's push, hop for hop");
        }
        // ranges
        {
            FileJob j;
            j.have = { { 16000, 32000 }, { 48000, 64000 } };
            j.done = { { 64000, 70000 } };
            const auto a = analysedSet(j, false);
            ok(a.size() == 2 && a[1].s == 48000 && a[1].e == 70000, "analysedSet merges have and done");
            ok(firstGapAtOrAfter(a, 20000) == 32000 && firstGapAtOrAfter(a, 40000) == 40000 && firstGapAtOrAfter(a, 50000) == 70000,
               "firstGapAtOrAfter: inside a range -> its end; in a gap -> itself");
            ok(toWireMs({ { 17, 33 }, { 0, 15 } }).empty(), "toWireMs: pieces shorter than a whole ms claim nothing");
            const auto w2 = toWireMs({ { 16, 48 } });
            ok(w2.size() == 1 && w2[0].s == 1 && w2[0].e == 3, "toWireMs rounds a start up and an end down (never claims more)");
        }
        // review P5, the unit: a cue whose end carries a FRACTION of a ms (7.5896 s: 7590 on the wire, rounded). Once
        // it is sent, the stretch reported reaches 7590 -- the cue as SENT -- where the end taken from the samples
        // (ceil 121434, floored to 7589 ms) left it 1 ms outside its own range.
        {
            FileJob j;
            j.req.jobId = 77;
            j.r.mode = "auto"; j.r.langA = "en"; j.r.langB = "zh";
            j.runActive = true;
            j.runStart = 0;
            j.runReported = 0;
            j.cursor = (uint64_t)(7.6 * kFileRate);       // cursor - 0.45 s = 7.15 s: the sent cue's end is what holds
            FileCue c;
            c.start = 6.0; c.end = 7.5896; c.text = "Hello there."; c.lang = "en";
            j.toTranslate.push_back(c);
            CollectSink sink;
            g_fileMtForTest = [](const std::string&, const std::string&, const std::string&, size_t) { return std::string("T"); };
            std::string err;
            translateOneCue(j, srv, sink, err);
            g_fileMtForTest = nullptr;
            reportFrontier(j);
            const auto ms = toWireMs(analysedSet(j, true));
            trpipe::WireCue w;
            const bool sent = !sink.lines.empty() && trpipe::unpackCue(trpipe::split(sink.lines.back()), w);
            std::printf("    P5: a cue ending at 7.5896 s goes out ending %llu ms; the range reported then ends %llu ms\n",
                        sent ? (unsigned long long)w.endMs : 0ull, ms.empty() ? 0ull : (unsigned long long)ms.back().e);
            ok(sent && w.endMs == 7590 && !ms.empty() && ms.back().e >= w.endMs,
               "P5: once a cue is sent, the range reported covers it to the very ms it was sent with (no +1 ms tolerance)");
        }
        std::printf("    %s\n", failures == before ? "limit / run start / cut / cues inside runs / regions / ranges OK"
                                                  : "SEE FAILURES ABOVE");
    }

    // ---------------------------------------------------------------- [8c]
    std::printf("\n[8c] FILE MODE v2 -- whole jobs, stand-in whisper + MT (offline, deterministic)\n");
    const std::vector<UttMark> marks = haveFixtures ? readMarks(F + L"meeting_marks.txt") : std::vector<UttMark>();
    if (marks.size() != 24) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): meeting_marks.txt (24 utterances) missing\n");
    } else {
        const int before = failures;
        const std::string meeting = Fu + "meeting.wav";
        const uint64_t totalMs = 104018;
        uint64_t id = 8000;
        auto file = [&](uint64_t jid, const std::string& have, uint64_t focus) {
            trpipe::WireFile f;
            f.jobId = jid; f.path = meeting; f.langA = "en"; f.langB = "zh"; f.mode = "auto"; f.quality = "auto";
            f.have = have; f.focusMs = focus;
            return f;
        };
        auto haveOf = [](const std::string& s) { std::vector<trpipe::MsRange> r; trpipe::parseRanges(s, r); return r; };

        // J1 -- v1 COMPATIBILITY: no have, no focus = ONE run from 0, and the windows the v1 binary planned
        //       on this file (scratchpad\filemode_2026-10-10\wd\logs\base_v1\meeting.wav.log, 10/10).
        {
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", 0));
            failures += checkV2Wire(r.lines, id, "J1 v1", {}, true, totalMs);
            static const double kV1[5][2] = { { 0.00, 9.04 }, { 10.27, 34.30 }, { 35.56, 62.47 }, { 63.46, 93.24 }, { 93.28, 104.02 } };
            bool same = r.st.plans.size() == 5;
            for (size_t i = 0; same && i < 5; ++i)
                same = std::fabs((double)r.st.plans[i].s / kFileRate - kV1[i][0]) < 0.006
                    && std::fabs((double)r.st.plans[i].e / kFileRate - kV1[i][1]) < 0.006;
            std::printf("    J1 v1: %u run(s), %zu window(s):", r.st.runs, r.st.plans.size());
            for (const auto& w : r.st.plans) std::printf(" %.2f-%.2f", (double)w.s / kFileRate, (double)w.e / kFileRate);
            std::printf(", %zu cue(s)\n", cuesOfRun(r.lines).size());
            ok(r.st.runs == 1 && same, "a FILE without have/focus is ONE run from 0 with exactly v1's windows "
                                       "(0-9.04 10.27-34.30 35.56-62.47 63.46-93.24 93.28-104.02)");
            const auto cs = cuesOfRun(r.lines);
            bool inc = !cs.empty();
            for (size_t i = 1; i < cs.size(); ++i) inc = inc && cs[i].startMs > cs[i - 1].startMs;
            ok(inc && cs.size() >= 20, "... cues in increasing order (v1's single run)");
            ok(r.st.seeks == 0, "... and no seek at all");
        }
        // J2 -- HAVE respected: only the gaps are analysed, no cue in `have`, the union covers the file
        {
            const std::string have = "20000-45000,60000-75000";
            const V2Run r = driveV2(srv, cfg, marks, file(++id, have, 0));
            failures += checkV2Wire(r.lines, id, "J2 have", haveOf(have), true, totalMs);
            bool noWinInHave = true;
            for (const auto& w : r.st.plans)
                if (rangesIntersect(w.s / 16, w.e / 16, haveOf(have))) noWinInHave = false;
            std::printf("    J2 have %s: %u run(s), %zu window(s), %zu cue(s), %u cut\n", have.c_str(), r.st.runs,
                        r.st.plans.size(), cuesOfRun(r.lines).size(), r.st.cutCues);
            ok(noWinInHave, "no window is sent to whisper inside `have`");
            ok(r.st.runs == 3, "three gaps, three runs");
        }
        // J3 -- a FOCUS in the middle: the first cue at/after the run's start (a breath <= 3 s before the
        //       focus), the rest of the file forward, THEN the gap before it
        {
            const uint64_t focus = 50000;
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", focus));
            dumpV2Run("J3", r);
            failures += checkV2Wire(r.lines, id, "J3 focus", {}, true, totalMs);
            const auto cs = cuesOfRun(r.lines);
            ok(!cs.empty() && cs[0].startMs >= focus - 3000 && cs[0].startMs <= focus + 3000,
               "the first cue is at the focus (" + (cs.empty() ? std::string("none") : std::to_string(cs[0].startMs)) + " ms)");
            // the run's stretch starts on the breath at most 3 s before the focus (the first FPROG range
            // that is not `have`), and its first window opens on the next onset (here 50.5 s: the focus
            // fell in a breath, 49.29-50.7 s)
            uint64_t runFrom = ~0ull;
            for (const auto& m : r.lines) {
                trpipe::WireFileProg p;
                std::vector<trpipe::MsRange> rr;
                if (trpipe::unpackFileProg(trpipe::split(m), p) && trpipe::parseRanges(p.ranges, rr) && !rr.empty()) { runFrom = rr[0].s; break; }
            }
            ok(runFrom >= focus - 3000 && runFrom <= focus && !r.asr.empty() && r.asr[0].s >= runFrom * 16
               && r.asr[0].s <= (focus + 3000) * 16,
               "the run starts on the breath at most 3 s before the focus (" + std::to_string(runFrom)
               + " ms), its first window right after it");
            size_t firstBefore = cs.size();
            for (size_t i = 0; i < cs.size(); ++i) if (cs[i].startMs < focus - 3000) { firstBefore = i; break; }
            bool forwardFirst = firstBefore < cs.size();
            for (size_t i = 0; forwardFirst && i < firstBefore; ++i) forwardFirst = cs[i].startMs >= focus - 3000;
            for (size_t i = firstBefore; forwardFirst && i < cs.size(); ++i) forwardFirst = cs[i].startMs < focus;
            ok(forwardFirst, "ORDER OF WORK: everything from the focus to the end, THEN the gap before it");
            std::printf("    J3 focus %llu: %u run(s), first cue %llu ms, first cue before the focus at position %zu of %zu\n",
                        (unsigned long long)focus, r.st.runs, cs.empty() ? 0ull : (unsigned long long)cs[0].startMs,
                        firstBefore, cs.size());
            // the context reset: every run's first window is prompted with the glossary alone
            const std::string gloss = asrGlossaryFor("en");
            size_t runFirsts = 0, glossOnly = 0;
            for (size_t i = 0; i < r.asr.size(); ++i) {
                const bool jumped = i == 0 || r.asr[i].s < r.asr[i - 1].s;
                if (!jumped) continue;
                ++runFirsts;
                if (r.asr[i].prompt == gloss) ++glossOnly;
            }
            ok(runFirsts == 2 && glossOnly == 2, "each run's first window: a FRESH prompt (the glossary, no tail of another stretch)");
        }
        // J4 -- FFOCUS mid-job: the very next WINDOW is at the new focus (no window in between); the
        //       old run's cues already heard are NOT thrown away (review C1): they drain -- translated and
        //       sent -- before the new run's first window, exactly the cues the uninterrupted job made of
        //       that audio; the new run's first translation has NO context of the old stretch
        {
            const V2Run ref = driveV2(srv, cfg, marks, file(++id, "", 0));      // J1's job: the reference cues
            const auto refCues = cuesOfRun(ref.lines);
            size_t injectedAt = 0;
            bool injected = false;
            size_t asrAtInject = 0;
            uint64_t heardTo = 0;                     // the old run's cursor: the end of what it heard
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", 0), [&](Shared& sh, const V2Run& cur, int) {
                if (injected) return;
                if (countKind(cur.lines, "FCUE") >= 2) {
                    injectFocus(sh, 80000, true);
                    injected = true;
                    injectedAt = cur.lines.size();
                    asrAtInject = cur.asr.size();          // the calls made BEFORE the FFOCUS
                    heardTo = cur.asr.empty() ? 0 : cur.asr.back().e;
                }
            });
            dumpV2Run("J4", r);
            failures += checkV2Wire(r.lines, id, "J4 ffocus", {}, true, totalMs);
            const bool jumpNext = asrAtInject < r.asr.size() && r.asr[asrAtInject].s >= 77000ull * 16;
            const size_t newWinAt = asrAtInject < r.asr.size() ? r.asr[asrAtInject].afterLines : r.lines.size();
            std::printf("    J4 FFOCUS 80 s after 2 cues: next window %.2f s; %u retarget(s), %u keep(s)\n",
                        asrAtInject < r.asr.size() ? r.asr[asrAtInject].s / 16000.0 : -1.0, r.st.retargets, r.st.keeps);
            ok(injected && jumpNext && r.st.retargets == 1,
               "FFOCUS mid-job: the very next WINDOW is at the new focus (no window in between)");
            // the drain: between the FFOCUS and that window, only cues of what the old run had heard ...
            size_t drained = 0;
            bool onlyOld = true;
            for (size_t k = injectedAt; k < newWinAt && k < r.lines.size(); ++k) {
                trpipe::WireCue c;
                if (!trpipe::unpackCue(trpipe::split(r.lines[k]), c)) continue;
                ++drained;
                onlyOld = onlyOld && c.endMs * 16 <= heardTo + 16;
            }
            // ... and, all told, every cue the uninterrupted job made of that audio
            const auto cs = cuesOfRun(r.lines);
            size_t refOld = 0, missing = 0;
            for (const auto& rc : refCues) {
                if (rc.endMs * 16 > heardTo) continue;
                ++refOld;
                bool found = false;
                for (const auto& c : cs) found = found || (c.src == rc.src && c.startMs == rc.startMs);
                if (!found) ++missing;
            }
            std::printf("    J4 drain: %zu cue(s) of the old run sent after the FFOCUS, before the new window; "
                        "%zu of the %zu reference cue(s) of its audio missing\n", drained, missing, refOld);
            ok(onlyOld && drained >= 1 && refOld >= 2 && missing == 0,
               "no window's work is thrown away: the old run's heard cues drain before the new run (none missing)");
            // the new run's first translation: the first MT call after its first window
            const MtCall* firstNew = nullptr;
            for (const auto& m : r.mt) if (m.afterLines > newWinAt) { firstNew = &m; break; }
            ok(firstNew && firstNew->ctx == 0, "the new run's first translation is given NO context pair of the old stretch");
        }
        // J5 -- a JOIN IN SPEECH (review P1): `have` starts in the middle of an utterance (B_1, 4.66-9.52 s).
        //       The run before it stops ON 7.0 s but HEARS past it (a post-roll): B_1's head -- every word
        //       starting before 7.0 s, the one across it whole -- is a cue ending before the range; no
        //       word starting after 7.0 s, none cut. (v2 first dropped that cue whole: B_1 lost.)
        {
            const std::string have = "7000-20000";
            const V2Run r = driveV2(srv, cfg, marks, file(++id, have, 0));
            failures += checkV2Wire(r.lines, id, "J5 straddle", haveOf(have), true, totalMs);
            const auto cs = cuesOfRun(r.lines);
            const trpipe::WireCue* head = nullptr;
            for (const auto& c : cs) if (c.startMs >= 4500 && c.startMs < 7000) head = &c;
            bool limitWin = false, heardPast = false;
            for (size_t i = 0; i < r.st.plans.size(); ++i) {
                const WindowPlan& w = r.st.plans[i];
                if (w.why == WindowPlan::kLimit && w.e == 7000ull * 16) {
                    limitWin = true;
                    heardPast = w.postRolled() && i < r.asr.size() && r.asr[i].e > 7000ull * 16;
                }
            }
            // B_1's words: 0.4 s each from 4.66 s -- six start before 7.0 s (the sixth, 6.66-7.06, across it)
            int headWords = 0;
            if (head) for (size_t p = 0; (p = head->src.find("u", p)) != std::string::npos; ++p) ++headWords;
            std::printf("    J5 have %s: a window ending ON 7.0 s: %s, heard past it: %s; B_1's head: %s (%d word(s), "
                        "ends %llu ms); cut words %u\n", have.c_str(), limitWin ? "yes" : "no", heardPast ? "yes" : "no",
                        head ? head->src.c_str() : "NONE", headWords, head ? (unsigned long long)head->endMs : 0ull,
                        r.st.cutWords);
            ok(limitWin && heardPast && head && headWords == 6 && head->endMs <= 7000 - 80 && r.st.cutWords == 0
               && r.st.cutCues == 0,
               "a sentence across the start of an analysed range: its head is KEPT, heard whole up to the join "
               "(6 words, the last across it), ending before the range");
        }
        // J6 -- WE's Q2: `have` covers the file -> FPROG with the full ranges, FDONE ok, NO model load
        {
            const std::string have = "0-104018";
            const V2Run r = driveV2(srv, cfg, marks, file(++id, have, 0));
            failures += checkV2Wire(r.lines, id, "J6 Q2", haveOf(have), true, totalMs);
            ok(r.st.loads == 0 && r.st.windows == 0 && cuesOfRun(r.lines).empty(),
               "Q2: a FILE whose `have` covers the whole file: FDONE ok at once, no model load, no window");
            const std::string have2 = "0-50000,50050-104000";          // gaps under 100 ms: nothing fits in them
            const V2Run r2 = driveV2(srv, cfg, marks, file(++id, have2, 0));
            failures += checkV2Wire(r2.lines, id, "J6 Q2 residue", haveOf(have2), true, totalMs);
            ok(r2.st.loads == 0, "... gaps too short to hold a subtitle (50 ms, 18 ms) count as covered");
        }
        // J7 -- WE's Q3: FFOCUS while LOADING is kept and applied when the analysis starts
        {
            bool injected = false;
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", 0), [&](Shared& sh, const V2Run& cur, int lap) {
                // after Open (FPROG loading out), before the Load lap: the job is LOADING
                if (!injected && lap == 0 && countKind(cur.lines, "FPROG") == 1) { injectFocus(sh, 60000, true); injected = true; }
            });
            failures += checkV2Wire(r.lines, id, "J7 Q3", {}, true, totalMs);
            ok(injected && !r.asr.empty() && r.asr[0].s >= 57000ull * 16 && r.asr[0].s <= 60000ull * 16,
               "Q3: an FFOCUS received while the models load is applied when the analysis starts (first window at 57-60 s)");
            // ... and one for an unknown / finished job is ignored, through the servant's own handler
            Shared sh;
            sh.runningJob = 77;
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(76, 1234)));
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(9999, 1234)));
            const bool ignored = sh.focusSeq == 0;
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(77, 4321)));
            ok(ignored && sh.focusSeq == 1 && sh.focusJob == 77 && sh.focusMs == 4321,
               "Q3: FFOCUS for a finished or unknown job is ignored; for the running one it is taken");
            trpipe::WireFile qf = file(78, "", 0);
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(qf)));
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(78, 5555)));
            ok(!sh.fileReqs.empty() && sh.fileReqs.back().f.focusMs == 5555, "... and for a QUEUED FILE it becomes that FILE's focus");
        }
        // J8 -- WE's Q4: at least one FPROG per window, the duration unknown (0) included
        {
            g_fileNoDurationForTest = true;
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", 0));
            g_fileNoDurationForTest = false;
            failures += checkV2Wire(r.lines, id, "J8 Q4", {}, true, 0);
            // STRICT: the very message after each window's model call is an FPROG -- sent by that window's
            // own step, not by the translations that follow it (mutation M14 survived a looser check)
            size_t windowsWithout = 0;
            for (size_t i = 0; i < r.asr.size(); ++i) {
                const size_t at = r.asr[i].afterLines;
                if (at >= r.lines.size() || r.lines[at].rfind("FPROG\t", 0) != 0) ++windowsWithout;
            }
            bool zero = true;
            for (size_t k = 0; k + 1 < r.lines.size(); ++k) {
                trpipe::WireFileProg p;
                if (trpipe::unpackFileProg(trpipe::split(r.lines[k]), p) && p.totalMs != 0 && k + 2 < r.lines.size()) zero = false;
            }
            std::printf("    J8 Q4: %zu window(s), %zu without an FPROG after it; totalMs 0 until the end: %s\n",
                        r.asr.size(), windowsWithout, zero ? "yes" : "no");
            ok(!r.asr.empty() && windowsWithout == 0, "Q4: an FPROG after EVERY window, totalMs unknown or not");
        }
        // J9 -- a BURST of FFOCUS (a scrub: 20 in ~0.4 s): no model call until it stops, then ONE run at the last
        {
            int burst = 0;
            size_t asrBefore = 0, asrDuring = 0;
            static const uint64_t kScrub[20] = { 10000, 90000, 30000, 70000, 20000, 85000, 15000, 60000, 40000, 95000,
                                                 5000, 80000, 25000, 65000, 35000, 55000, 45000, 75000, 50000, 70000 };
            V2Run r;
            size_t burstEndLines = 0, asrAtFirst = 0, asrAtLast = 0;
            // One FFOCUS every THIRD lap (~60 ms apart, as a scrub sends them): without the settle a jump would
            // start its run on the next lap and make its model call on the one after -- between two FFOCUS
            // (mutation M20 survived a burst sent on every lap, where no run ever got that far).
            r = driveV2(srv, cfg, marks, file(++id, "", 0), [&](Shared& sh, const V2Run& cur, int lap) {
                if (burst < 20 && countKind(cur.lines, "FCUE") >= 1 && lap % 3 == 0) {
                    injectFocus(sh, kScrub[burst], false);                     // fresh: not settled
                    if (burst == 0) asrAtFirst = cur.asr.size();
                    ++burst;
                    if (burst == 20) { burstEndLines = cur.lines.size(); asrAtLast = cur.asr.size(); }
                }
            });
            (void)asrBefore; (void)asrDuring;
            dumpV2Run("J9", r);
            size_t firstCue = (size_t)-1;
            for (size_t k = 0; k < r.lines.size(); ++k) if (r.lines[k].rfind("FCUE\t", 0) == 0) { firstCue = k; break; }
            size_t after = 0;
            (void)firstCue; (void)burstEndLines;
            const size_t during = asrAtLast - asrAtFirst;      // model calls made while the scrub went on
            const AsrCall* next = asrAtLast < r.asr.size() ? &r.asr[asrAtLast] : nullptr;
            (void)after;
            failures += checkV2Wire(r.lines, id, "J9 burst", {}, true, totalMs);
            std::printf("    J9 burst of 20 FFOCUS: %zu model call(s) during it; then the first window at %.2f s\n",
                        during, next ? next->s / 16000.0 : -1.0);
            ok(burst == 20 && during <= 1 && next && next->s >= 67000ull * 16 && next->s <= 70000ull * 16,
               "a burst of FFOCUS costs nothing but the last one (<= 1 call during it -- the one in flight -- then 70 s)");
        }
        // J12 -- the KEEP rule: a seek a few seconds back into the text just subtitled, then one a few
        //        seconds ahead of the analysis, restart nothing -- same single run, v1's very windows
        {
            int step = 0;
            const V2Run r = driveV2(srv, cfg, marks, file(++id, "", 0), [&](Shared& sh, const V2Run& cur, int) {
                if (step == 0 && countKind(cur.lines, "FCUE") >= 1) { injectFocus(sh, 2000, true); ++step; }         // inside, behind
                else if (step == 1 && countKind(cur.lines, "FCUE") >= 3) { injectFocus(sh, 40000, true); ++step; } // just ahead
            });
            failures += checkV2Wire(r.lines, id, "J12 keep", {}, true, totalMs);
            static const double kV1[5][2] = { { 0.00, 9.04 }, { 10.27, 34.30 }, { 35.56, 62.47 }, { 63.46, 93.24 }, { 93.28, 104.02 } };
            bool same = r.st.plans.size() == 5;
            for (size_t i = 0; same && i < 5; ++i)
                same = std::fabs((double)r.st.plans[i].s / kFileRate - kV1[i][0]) < 0.006
                    && std::fabs((double)r.st.plans[i].e / kFileRate - kV1[i][1]) < 0.006;
            std::printf("    J12 focus 2 s (behind) then 40 s (ahead): %u keep(s), %u retarget(s), %u run(s)\n",
                        r.st.keeps, r.st.retargets, r.st.runs);
            ok(step == 2 && r.st.keeps == 2 && r.st.retargets == 0 && r.st.runs == 1 && same,
               "a focus inside the analysed text, or within reach ahead of the analysis, keeps the run (no restart churn)");
        }
        // J13 -- review C1: a machine SLOWER than real time, a viewer PLAYING without waiting (keep-ahead
        //        off, or "play without waiting"). The CPU fallback as WD's own CPU log measured it (turbo
        //        ~55 s a window, the 4B ~3 s a line), on the virtual clock; the TRAY replayed -- its 5 s
        //        tick and its FFOCUS rule (TrPlayer.h shouldFocus: lead under the playhead < 30 s, target
        //        moved > 1 s), each FFOCUS through the servant's own handler. The film: the meeting six
        //        times over (10.4 min, written to %TEMP%), played at 1x from 0. Before the fix every tick
        //        restarted the run at the playhead and dropped the window just heard: no cue until the
        //        film ended. Must hold: cues keep coming while it plays, no run is restarted, the windows
        //        are those of a job that got no FFOCUS at all -- and a real SEEK still jumps, at once,
        //        without throwing the cues already heard away.
        {
            std::vector<float> one;
            MediaInfo mi;
            std::string err;
            wchar_t tmpDir[MAX_PATH] = {};
            ::GetTempPathW(MAX_PATH, tmpDir);
            const std::wstring path6 = std::wstring(tmpDir) + L"SuVtitle_selftest_j13.wav";
            bool built = decodeAll16k(F + L"meeting.wav", one, mi, err);
            const double dur1 = (double)one.size() / kFileRate;
            std::vector<UttMark> marks6;
            if (built) {
                std::vector<float> six;
                six.reserve(one.size() * 6);
                for (int k = 0; k < 6; ++k) {
                    six.insert(six.end(), one.begin(), one.end());
                    for (const auto& m : marks) marks6.push_back({ m.name, m.s + k * dur1, m.e + k * dur1 });
                }
                const std::string wav = buildWav16k(six);
                built = writeBytes(path6, std::vector<unsigned char>(wav.begin(), wav.end()));
            }
            if (!built) {
                ok(false, "J13: the 10 min film could not be built in %TEMP% (" + err + ")");
            } else {
                const uint64_t total6 = (uint64_t)(one.size() * 6 * 1000 / kFileRate);     // ms, floored (the container's)
                auto file6 = [&](uint64_t jid, uint64_t focus) {
                    trpipe::WireFile f6 = file(jid, "", focus);
                    f6.path = wideToUtf8L(path6);
                    return f6;
                };
                // the tray's twin rules (TrPlayer.h focusTarget / shouldFocus). WE's FINAL tray (TrPlayer.h:116,
                // its review fix P4) sends NO FFOCUS for a 5 s tick while the playhead sits in a gap at or after the
                // last focus -- seeks, play starts and ticks inside subtitled text still send one. `oldTray` = the
                // rule before it: EVERY tick refocuses while the lead is short -- an older tray, or a user dragging
                // the playhead. The helper's keep rule must hold for both.
                auto target = [](const std::vector<trpipe::MsRange>& rr, uint64_t p) {
                    for (const auto& x : rr) { if (p < x.s) break; if (p < x.e) return x.e; }
                    return p;
                };
                auto trayFocus = [&](const std::vector<trpipe::MsRange>& rr, uint64_t p, uint64_t lastF, bool tick, bool oldTray) {
                    if (target(rr, p) + 500 >= total6) return false;
                    const uint64_t t = target(rr, p), lt = target(rr, lastF);
                    if (tick && !oldTray && t == p && lastF <= p) return false;
                    if ((t > lt ? t - lt : lt - t) <= 1000) return false;
                    return t - p < (tick ? 30000ull : 60000ull);
                };
                // A PLAYER: playing at 1x from `from` (virtual ms) at `play0`; `seekAt`/`seekTo` = one seek.
                struct Player {
                    uint64_t play0 = 0, from = 0, nextTick = 0, lastFocus = 0, seekAt = 0, seekTo = 0;
                    uint64_t pauseAt = 0, pauseMs = 0, seekBy = 0;     // a PAUSE, then a seek `seekBy` ahead
                    bool     ended = false, seeked = false, paused = false, oldTray = false;
                    std::vector<trpipe::MsRange> ranges;
                    size_t   seen = 0, focusSent = 0, cuesPlaying = 0;
                    uint64_t firstCueAt = 0, seekLines = 0;
                    std::vector<std::pair<size_t, uint64_t>> sent;     // (wire lines so far, ms) per FFOCUS
                };
                auto drivePlayer = [&](Player& pl, V2Clock& clk, uint64_t jid) {
                    return [&, jid](Shared& sh, const V2Run& cur, int) {
                        auto send = [&](uint64_t at, uint64_t p, bool tick) {
                            if (!trayFocus(pl.ranges, p, pl.lastFocus, tick, pl.oldTray)) return;
                            const uint64_t keep = clk.now;
                            clk.now = at;
                            servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(jid, p)));
                            clk.now = keep;
                            pl.lastFocus = p;
                            ++pl.focusSent;
                            pl.sent.push_back({ cur.lines.size(), p });
                        };
                        while (!pl.ended && pl.nextTick <= clk.now) {
                            if (pl.pauseMs && !pl.paused && pl.nextTick >= pl.pauseAt) {
                                // paused: no report at all (the tray ticks only while playing), then a seek ahead
                                pl.paused = true;
                                const uint64_t p = pl.from + (pl.pauseAt - pl.play0);
                                pl.seekAt = pl.pauseAt + pl.pauseMs;
                                pl.seekTo = p + pl.seekBy;
                                pl.nextTick = pl.seekAt;
                                continue;
                            }
                            if (pl.seekAt && !pl.seeked && pl.seekAt <= pl.nextTick) {
                                // the user drags the playhead: a "seek" report (debounced 250 ms)
                                pl.seeked = true;
                                pl.from = pl.seekTo; pl.play0 = pl.seekAt;
                                send(pl.seekAt + 250, pl.seekTo, false);
                                pl.seekLines = cur.lines.size();
                                pl.nextTick = pl.seekAt + 5000;
                                continue;
                            }
                            const uint64_t p = pl.from + (pl.nextTick - pl.play0);
                            if (p >= total6) { pl.ended = true; break; }
                            send(pl.nextTick, p, true);
                            pl.nextTick += 5000;
                        }
                        for (; pl.seen < cur.lines.size(); ++pl.seen) {
                            const auto t = trpipe::split(cur.lines[pl.seen]);
                            trpipe::WireFileProg pg;
                            trpipe::WireCue c;
                            if (trpipe::unpackFileProg(t, pg)) {
                                std::vector<trpipe::MsRange> rr;
                                if (trpipe::parseRanges(pg.ranges, rr)) pl.ranges = rr;
                            } else if (trpipe::unpackCue(t, c)) {
                                if (!pl.firstCueAt) pl.firstCueAt = clk.now;
                                if (!pl.ended) ++pl.cuesPlaying;
                            }
                        }
                    };
                };
                // the reference: the same film, the same machine, no FFOCUS at all
                V2Clock c0;
                c0.asrMs = 55000; c0.mtMs = 3000;
                const V2Run base = driveV2(srv, cfg, marks6, file6(++id, 0), {}, 20000, &c0);
                failures += checkV2Wire(base.lines, id, "J13 reference", {}, true, total6);
                for (int rule = 0; rule < 2; ++rule) {
                    const bool oldTray = rule == 1;
                    const std::string tray = oldTray ? "older tray, an FFOCUS every tick" : "WE's tray";
                    // played without waiting, from 0
                    V2Clock c1;
                    c1.asrMs = 55000; c1.mtMs = 3000;
                    Player pl;
                    pl.oldTray = oldTray;
                    pl.play0 = c1.now; pl.nextTick = c1.now + 5000;
                    const uint64_t jid1 = ++id;
                    const V2Run r = driveV2(srv, cfg, marks6, file6(jid1, 0), drivePlayer(pl, c1, jid1), 20000, &c1);
                    failures += checkV2Wire(r.lines, jid1, ("J13 playing, " + tray).c_str(), {}, true, total6);
                    bool same = r.st.plans.size() == base.st.plans.size();
                    for (size_t i = 0; same && i < r.st.plans.size(); ++i)
                        same = r.st.plans[i].s == base.st.plans[i].s && r.st.plans[i].e == base.st.plans[i].e;
                    std::printf("    J13 [%s] CPU-speed (55 s/window, 3 s/line), playing 10.4 min without waiting: %zu FFOCUS "
                                "sent, %u keep(s) (%u by playback), %u retarget(s); first cue %.0f s after play; %zu cue(s) "
                                "while playing, %zu in all; windows %s the no-FFOCUS job's (%zu)\n", tray.c_str(),
                                pl.focusSent, r.st.keeps, r.st.playKeeps, r.st.retargets,
                                pl.firstCueAt ? (pl.firstCueAt - pl.play0) / 1000.0 : -1.0, pl.cuesPlaying,
                                cuesOfRun(r.lines).size(), same ? "==" : "!=", base.st.plans.size());
                    if (oldTray)
                        ok(pl.focusSent >= 50 && r.st.playKeeps >= 1 && r.st.retargets == 0 && same,
                           "C1 [" + tray + "]: a playing playhead's FFOCUS (every 5 s, far ahead of a slow analysis) "
                           "restarts nothing -- the windows are those of a job that got no FFOCUS");
                    else
                        ok(r.st.retargets == 0 && same,
                           "C1 [" + tray + "]: playing through a slow analysis restarts nothing -- the windows are those of "
                           "a job that got no FFOCUS");
                    ok(pl.firstCueAt && pl.firstCueAt - pl.play0 <= 150000 && pl.cuesPlaying >= 30,
                       "C1 [" + tray + "]: the cues keep coming while it plays (the first within 150 s, >= 30 before the end)");
                    // ... and a real SEEK (3 min ahead, 200 s in) still jumps at once, the cues heard drained first
                    V2Clock c2;
                    c2.asrMs = 55000; c2.mtMs = 3000;
                    Player ps;
                    ps.oldTray = oldTray;
                    ps.play0 = c2.now; ps.nextTick = c2.now + 5000;
                    ps.seekAt = c2.now + 200000; ps.seekTo = 380000;
                    const uint64_t jid2 = ++id;
                    const V2Run rs = driveV2(srv, cfg, marks6, file6(jid2, 0), drivePlayer(ps, c2, jid2), 20000, &c2);
                    failures += checkV2Wire(rs.lines, jid2, ("J13 seek, " + tray).c_str(), {}, true, total6);
                    // the first window after the seek, and the playhead the job had then (the latest FFOCUS
                    // before that call: the playhead kept moving during the call in flight and the drain)
                    size_t firstAfterSeek = rs.asr.size();
                    for (size_t i = 0; i < rs.asr.size(); ++i) if (rs.asr[i].afterLines > ps.seekLines) { firstAfterSeek = i; break; }
                    uint64_t P = 0;
                    if (firstAfterSeek < rs.asr.size())
                        for (const auto& s : ps.sent) if (s.first <= rs.asr[firstAfterSeek].afterLines) P = s.second;
                    const uint64_t w0 = firstAfterSeek < rs.st.plans.size() ? rs.st.plans[firstAfterSeek].s : 0;
                    const bool jumped = P >= 380000 && w0 + 8000ull * 16 >= P * 16 && w0 <= P * 16 + 3000ull * 16;
                    std::printf("    J13 [%s] + a seek to 6:20 at 3:20: %u retarget(s); the first window after it %.2f s, "
                                "the playhead then %.2f s\n", tray.c_str(), rs.st.retargets, w0 / 16000.0, P / 1000.0);
                    ok(ps.seeked && rs.st.retargets == 1 && jumped,
                       "C1 [" + tray + "]: a seek is still a jump -- once, its first window at the playhead (the ticks "
                       "after it keep that run)");
                    // ... and a PAUSE breaks the chain: paused a minute at 1:40, then 40 s ahead -- a distance a minute
                    // of playback could explain, but nothing played: it is a seek (the cap on the time between two FFOCUS)
                    V2Clock c3;
                    c3.asrMs = 55000; c3.mtMs = 3000;
                    Player pp;
                    pp.oldTray = oldTray;
                    pp.play0 = c3.now; pp.nextTick = c3.now + 5000;
                    pp.pauseAt = c3.now + 100000; pp.pauseMs = 60000; pp.seekBy = 40000;
                    const uint64_t jid3 = ++id;
                    const V2Run rp = driveV2(srv, cfg, marks6, file6(jid3, 0), drivePlayer(pp, c3, jid3), 20000, &c3);
                    failures += checkV2Wire(rp.lines, jid3, ("J13 pause, " + tray).c_str(), {}, true, total6);
                    std::printf("    J13 [%s] + a 60 s pause at 1:40, then 40 s ahead: %u retarget(s), %u playback keep(s)\n",
                                tray.c_str(), rp.st.retargets, rp.st.playKeeps);
                    ok(pp.paused && pp.seeked && rp.st.retargets == 1,
                       "C1 [" + tray + "]: after a pause the chain is broken -- a seek a minute of playback could have "
                       "covered is still a jump");
                }
            }
            for (int t = 0; t < 5 && !::DeleteFileW(path6.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND; ++t) ::Sleep(20);
        }
        // J14 -- review C2: the file BREAKS OFF inside a LIMITED run (have = 60 s to the end, the decode
        //        fails at 0:30): the ranges end where the audio did -- [30, 60) s, never heard, is NOT
        //        claimed (a resume would have skipped it for ever) -- and FDONE says error. Then a SEEK that
        //        fails on the way back (the gap before a focus run; the file moved away): no stretch claimed
        //        without a window, the seek's own sentence -- never a window outside the decoded audio.
        {
            const std::string have = "60000-104018";
            g_fileFailDecodeAtForTest = 30.0;
            std::vector<trpipe::MsRange> last;
            const V2Run r = driveV2(srv, cfg, marks, file(++id, have, 0));
            g_fileFailDecodeAtForTest = 0.0;
            failures += checkV2Wire(r.lines, id, "J14 broken", haveOf(have), false, totalMs, &last);
            trpipe::WireFileDone d;
            const bool isErr = !r.lines.empty() && trpipe::unpackFileDone(trpipe::split(r.lines.back()), d) && d.result == "error";
            uint64_t endOfHeard = 0;
            for (const auto& x : last) if (x.s == 0) endOfHeard = x.e;
            std::printf("    J14 have %s, the decode broken at 0:30: last ranges %s, FDONE %s\n", have.c_str(),
                        trpipe::packRanges(last).c_str(), d.result.c_str());
            ok(isErr && endOfHeard >= 28000 && endOfHeard <= 33000 && !rangesIntersect(34000, 59000, last),
               "C2: a limited run the file breaks off in claims only what was decoded (not up to its limit)");
            // the failing seek: focus 80 s first, then the gap before it -- whose seek back to 0 fails
            bool armed = false;
            std::vector<trpipe::MsRange> last2;
            const V2Run r2 = driveV2(srv, cfg, marks, file(++id, "", 80000), [&](Shared&, const V2Run& cur, int) {
                if (!armed && countKind(cur.lines, "FCUE") >= 1) { g_fileFailSeekForTest = true; armed = true; }
            });
            g_fileFailSeekForTest = false;
            failures += checkV2Wire(r2.lines, id, "J14 seek", {}, false, totalMs, &last2);
            trpipe::WireFileDone d2;
            const bool isErr2 = !r2.lines.empty() && trpipe::unpackFileDone(trpipe::split(r2.lines.back()), d2)
                             && d2.result == "error" && d2.err.find("could not be read at this point") != std::string::npos;
            std::printf("    J14 focus 80 s, then the seek back fails: last ranges %s, FDONE %s \"%s\"\n",
                        trpipe::packRanges(last2).c_str(), d2.result.c_str(), d2.err.c_str());
            ok(armed && isErr2 && !last2.empty() && !rangesIntersect(0, 70000, last2),
               "C2: a seek that fails claims nothing it did not hear (no window outside the decoded audio): FDONE error, its sentence");
        }
        // J15 -- review P1, MEASURED: forced joins inside utterances (the tray's `have` starting and ending
        //        mid-sentence). Every word AT A JOIN (starting within 3 s of one) must come out EXACTLY ONCE
        //        -- in this job's cues, or inside `have` (the tray's) -- the word across it heard whole on one
        //        side. (Away from the joins the stand-in's 0.4 s word grid ignores the real audio: a word it
        //        puts where the energy says silence falls between two windows -- J1 loses B_1's last two the
        //        same way. That is the windowing, unchanged; the joins are what P1 changed.)
        {
            // the stand-in's words: utterance i, word k at u.s + 0.4 k (standInWhisper)
            auto wordName = [](size_t i, int k) {
                return "u" + std::to_string(i) + (char)('a' + (k % 26)) + (k >= 26 ? std::to_string(k / 26) : std::string());
            };
            struct JCase { const char* have; };
            int lost = 0, dup = 0, words = 0;
            uint32_t pres = 0, posts = 0;
            for (const JCase& jc : { JCase{ "7000-20000" }, JCase{ "33000-44000,65000-80000" }, JCase{ "7000-20000,33000-44000,65000-80000" } }) {
                const auto hv = haveOf(jc.have);
                const V2Run r = driveV2(srv, cfg, marks, file(++id, jc.have, 0));
                failures += checkV2Wire(r.lines, id, "J15 joins", hv, true, totalMs);
                std::map<std::string, int> got;
                for (const auto& c : cuesOfRun(r.lines)) {
                    std::string w;
                    for (char ch : c.src + " ") {
                        if (ch == ' ') { if (!w.empty()) ++got[w]; w.clear(); }
                        else if (ch != '.') w += ch;
                    }
                }
                int l = 0, dd = 0, n = 0;
                std::string which;
                for (size_t i = 0; i < marks.size(); ++i)
                    for (int k = 0; marks[i].s + 0.4 * k < marks[i].e - 1e-9; ++k) {
                        const double a = marks[i].s + 0.4 * k;
                        const uint64_t ams = (uint64_t)std::llround(a * 1000.0);
                        bool tray = false, atJoin = false;
                        for (const auto& x : hv) {
                            tray = tray || (x.s <= ams && ams < x.e);
                            atJoin = atJoin || (ams + 3000 >= x.s && ams <= x.s + 3000) || (ams + 3000 >= x.e && ams <= x.e + 3000);
                        }
                        if (tray || !atJoin) continue;
                        ++n;
                        const int c = got.count(wordName(i, k)) ? got[wordName(i, k)] : 0;
                        char wb[48];
                        std::snprintf(wb, sizeof(wb), " %s@%.2f", wordName(i, k).c_str(), a);
                        if (c == 0) { ++l; which += wb; }
                        if (c > 1) { dd += c - 1; which += std::string(wb) + "x2"; }
                    }
                std::printf("    J15 have %s: %d word(s) of this job's at the joins, %d lost, %d twice%s%s; %u post-roll(s), "
                            "%u pre-roll(s), %u cut word(s)\n", jc.have, n, l, dd, which.empty() ? "" : ":", which.c_str(),
                            r.st.postrolls, r.st.prerolls, r.st.cutWords);
                if (!which.empty()) dumpV2Run("J15", r);
                lost += l; dup += dd; words += n;
                pres += r.st.prerolls; posts += r.st.postrolls;
            }
            ok(pres >= 3 && posts >= 5, "P1: the joins in speech were heard past (" + std::to_string(pres) + " pre-roll(s), "
               + std::to_string(posts) + " post-roll(s))");
            ok(words > 40 && lost == 0 && dup == 0,
               "P1: at joins inside sentences, every word comes out exactly once (" + std::to_string(lost) + " lost, "
               + std::to_string(dup) + " twice of " + std::to_string(words) + ")");
        }
        // J18 -- review P5: cue times with a FRACTION of a millisecond (every stand-in time shifted by 0.6 ms; real
        //        whisper times carry any fraction). A cue's end goes on the wire rounded to the nearest ms; every
        //        FPROG sent after it must cover it to that very ms -- the stretch claimed reaches the end of the last
        //        cue SENT, as sent (v2 first took it from the samples: 1 ms short when the fraction was 0.5-0.94).
        {
            std::vector<UttMark> frac = marks;
            for (auto& m : frac) { m.s += 0.0006; m.e += 0.0006; }
            size_t atCueEnd = 0, cuesN = 0;
            for (const std::string& hv : { std::string(), std::string("20000-30000,61000-70000") }) {
                // (a translation costing 3 s on the job's clock: an FPROG follows nearly every cue -- v1's 2 s cadence)
                V2Clock ck;
                ck.asrMs = 1000; ck.mtMs = 3000;
                const V2Run r = driveV2(srv, cfg, frac, file(++id, hv, hv.empty() ? 0 : 50000), {}, 20000, &ck);
                failures += checkV2Wire(r.lines, id, "J18 fractional ms", haveOf(hv), true, totalMs);
                // how often an FPROG range ENDS exactly on a cue end already sent (the frontier held by that cue)
                std::vector<uint64_t> ends;
                for (const auto& m : r.lines) {
                    const auto t = trpipe::split(m);
                    trpipe::WireCue c;
                    trpipe::WireFileProg p;
                    std::vector<trpipe::MsRange> rr;
                    if (trpipe::unpackCue(t, c)) { ends.push_back(c.endMs); ++cuesN; }
                    else if (trpipe::unpackFileProg(t, p) && trpipe::parseRanges(p.ranges, rr))
                        for (const auto& x : rr) if (std::find(ends.begin(), ends.end(), x.e) != ends.end()) ++atCueEnd;
                }
            }
            std::printf("    J18 times + 0.6 ms: %zu cue(s); %zu FPROG range end(s) sitting exactly on a cue end sent\n",
                        cuesN, atCueEnd);
            ok(atCueEnd >= 3, "P5: the frontier did sit on a sent cue's end (so the strict per-FPROG check above bites)");
        }
        // J16 -- review P3: the decode-to-seek that opens a run is ABORTABLE (c.mp3 seeks by decoding; focus 30 s).
        //        The message lands while the Run lap runs (after that lap's request pass): (a) FCANCEL -- the seek
        //        stops at its first poll, the next request pass answers FDONE cancelled; (b) a new playhead far
        //        away -- the seek is abandoned, and the next run opens THERE.
        {
            for (int variant = 0; variant < 2; ++variant) {
                Shared sh;
                CollectSink sink;
                std::unique_ptr<FileJob> job;
                FileJobStats st;
                std::vector<uint64_t> firstS;
                g_fileNoModelsForTest = true;
                g_fileAsrForTest = [&](uint64_t s, uint64_t, const std::string&) {
                    firstS.push_back(s);
                    return std::string("{\"text\":\"\",\"segments\":[]}");
                };
                g_fileMtForTest = [](const std::string& src, const std::string&, const std::string&, size_t) { return src; };
                const uint64_t jid = 9600 + (uint64_t)variant;
                trpipe::WireFile f = file(jid, "", 30000);
                f.path = Fu + "c.mp3";
                servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(f)));
                fileRequests(sh, job, srv, sink, cfg, &st);
                for (int k = 0; k < 2 && job; ++k) fileJobLap(job, srv, sink, sh.abortJob, false, &st);   // Open, Load
                fileRequests(sh, job, srv, sink, cfg, &st);
                if (variant == 0) servantHandle(sh, nullptr, trpipe::split(trpipe::packFileCancel(jid)));
                else              servantHandle(sh, nullptr, trpipe::split(trpipe::packFileFocus(jid, 3000)));
                bool aborted = false;
                double decoded = -1.0;
                if (job) {
                    fileJobLap(job, srv, sink, sh.abortJob, false, &st);                 // Run: the run's seek
                    if (job) {
                        aborted = job->st.seekAborts == 1 && !job->runActive;
                        decoded = (double)job->reader.lastSeekDecoded() / std::max<uint32_t>(1, job->reader.info().nativeRate);
                    }
                }
                for (int lap = 0; lap < 20000 && job; ++lap) {
                    fileRequests(sh, job, srv, sink, cfg, &st);
                    if (job) fileJobLap(job, srv, sink, sh.abortJob, false, &st);
                }
                g_fileAsrForTest = nullptr;
                g_fileMtForTest = nullptr;
                g_fileNoModelsForTest = false;
                int dones = 0;
                std::string result;
                for (const auto& m : sink.lines) {
                    trpipe::WireFileDone d;
                    if (trpipe::unpackFileDone(trpipe::split(m), d) && d.jobId == jid) { ++dones; result = d.result; }
                }
                std::printf("    J16 %s during the run's seek: aborted %s (%.2f s decoded); FDONE %s x%d; first window %.2f s\n",
                            variant == 0 ? "FCANCEL" : "FFOCUS 3 s", aborted ? "yes" : "NO", decoded, result.c_str(), dones,
                            firstS.empty() ? -1.0 : firstS[0] / 16000.0);
                if (variant == 0)
                    ok(aborted && decoded < 0.5 && dones == 1 && result == "cancelled" && firstS.empty(),
                       "P3: FCANCEL during a decode-to-seek stops it at its first poll; ONE FDONE cancelled, no window");
                else
                    ok(aborted && decoded < 0.5 && dones == 1 && result == "ok" && !firstS.empty() && firstS[0] <= 3500ull * 16,
                       "P3: a new playhead far away during a decode-to-seek abandons it; the next run opens at the new playhead");
            }
        }
        // J17 -- review P4: the TEST environment acts on a run only when its COMMAND LINE makes it a test. The
        //        rule, and this very exe end to end: AUDIOPB_TR_FORCE_CPU=1 in its environment, no switch -> ignored.
        {
            ok(!testEnvAllowed(false, false, false) && testEnvAllowed(true, false, false) && testEnvAllowed(false, true, false)
               && testEnvAllowed(false, false, true), "P4: the test environment needs a selftest, --nofabric or --cpu");
            auto runSelf = [](const wchar_t* args) {
                wchar_t exe[MAX_PATH] = {};
                ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
                SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
                HANDLE rd = nullptr, wr = nullptr;
                std::string out;
                if (!::CreatePipe(&rd, &wr, &sa, 0)) return out;
                ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = wr;
                si.hStdError = wr;
                si.hStdInput = nullptr;
                PROCESS_INFORMATION pi{};
                std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + args;
                if (::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    ::CloseHandle(wr);
                    wr = nullptr;
                    char b[512];
                    DWORD n = 0;
                    while (::ReadFile(rd, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
                    ::WaitForSingleObject(pi.hProcess, 10000);
                    ::CloseHandle(pi.hThread);
                    ::CloseHandle(pi.hProcess);
                }
                if (wr) ::CloseHandle(wr);
                ::CloseHandle(rd);
                return out;
            };
            wchar_t keep[16] = {};
            const bool had = ::GetEnvironmentVariableW(L"AUDIOPB_TR_FORCE_CPU", keep, 16) > 0;
            ::SetEnvironmentVariableW(L"AUDIOPB_TR_FORCE_CPU", L"1");
            const std::string plain = runSelf(L"");
            const std::string cpu   = runSelf(L"--cpu");
            ::SetEnvironmentVariableW(L"AUDIOPB_TR_FORCE_CPU", had ? keep : nullptr);
            const bool ignored = plain.find("AUDIOPB_TR_FORCE_CPU ignored") != std::string::npos;
            const bool forced  = cpu.find("test environment: CPU forced") != std::string::npos;
            std::printf("    J17 AUDIOPB_TR_FORCE_CPU=1, this exe with no switch: %s; with --cpu: %s\n",
                        ignored ? "ignored" : "NOT IGNORED", forced ? "CPU forced" : "?");
            ok(ignored && forced, "P4: AUDIOPB_TR_FORCE_CPU alone does nothing to a run that is not a test (the shipping helper)");
        }
        // J10 -- item 5: a FILE whose id parses but whose `have` does not: exactly ONE FDONE error (servant path)
        {
            Shared sh;
            CollectSink sink;
            std::unique_ptr<FileJob> job;
            FileJobStats st;
            g_fileNoModelsForTest = true;
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(file(9001, "", 0))));
            fileRequests(sh, job, srv, sink, cfg, &st);
            for (int k = 0; k < 3 && job; ++k) fileJobLap(job, srv, sink, sh.abortJob, false, &st);
            servantHandle(sh, nullptr, trpipe::split("FILE\t9002\t" + trpipe::esc(meeting) + "\ten\tzh\tauto\tauto\t5000-1000"));
            servantHandle(sh, nullptr, trpipe::split("FILE\t9003\t\ten\tzh"));
            servantHandle(sh, nullptr, trpipe::split("FILE\t0\tC:\\x.wav"));
            fileRequests(sh, job, srv, sink, cfg, &st);
            g_fileNoModelsForTest = false;
            int d1 = 0, d2 = 0, d3 = 0;
            std::string r1, r2, e2, r3;
            for (const auto& m : sink.lines) {
                trpipe::WireFileDone d;
                if (!trpipe::unpackFileDone(trpipe::split(m), d)) continue;
                if (d.jobId == 9001) { ++d1; r1 = d.result; }
                if (d.jobId == 9002) { ++d2; r2 = d.result; e2 = d.err; }
                if (d.jobId == 9003) { ++d3; r3 = d.result; }
            }
            std::printf("    a running job, then FILE with have \"5000-1000\": 9001 %s x%d, 9002 %s x%d (\"%s\"), 9003 (no path) %s x%d\n",
                        r1.c_str(), d1, r2.c_str(), d2, e2.c_str(), r3.c_str(), d3);
            ok(d1 == 1 && r1 == "cancelled" && d2 == 1 && r2 == "error" && e2.find("could not be read") != std::string::npos
               && d3 == 1 && r3 == "error" && !job,
               "a FILE with a malformed `have` (or no path): it stops the running job and gets exactly ONE FDONE error");
        }
        // J11 -- FCANCEL -> the ranges in the last FPROG; a second FILE with have = them finishes the rest, no overlap
        {
            std::vector<trpipe::MsRange> partial;
            const uint64_t id1 = ++id;
            bool cancelled = false;
            const V2Run r1 = driveV2(srv, cfg, marks, file(id1, "", 30000), [&](Shared& sh, const V2Run& cur, int) {
                if (!cancelled && countKind(cur.lines, "FCUE") >= 4) {
                    std::lock_guard<std::mutex> lk(sh.mx);
                    sh.fileCancels.push_back(id1);
                    cancelled = true;
                }
            });
            failures += checkV2Wire(r1.lines, id1, "J11 cancel", {}, false, totalMs, &partial);
            trpipe::WireFileDone d;
            const bool isCancel = !r1.lines.empty() && trpipe::unpackFileDone(trpipe::split(r1.lines.back()), d) && d.result == "cancelled";
            const bool progBefore = r1.lines.size() >= 2 && r1.lines[r1.lines.size() - 2].rfind("FPROG\t", 0) == 0;
            const auto c1 = cuesOfRun(r1.lines);
            bool covered = !partial.empty();
            for (const auto& c : c1) {
                bool in = false;
                for (const auto& y : partial) in = in || (y.s <= c.startMs && c.endMs <= y.e);
                covered = covered && in;
            }
            const std::string have = trpipe::packRanges(partial);
            const uint64_t id2 = ++id;
            const V2Run r2 = driveV2(srv, cfg, marks, file(id2, have, 30000));
            failures += checkV2Wire(r2.lines, id2, "J11 resume", partial, true, totalMs);
            const auto c2 = cuesOfRun(r2.lines);
            bool cross = true;
            for (const auto& a : c1) for (const auto& b : c2) if (a.startMs < b.endMs && b.startMs < a.endMs) cross = false;
            std::printf("    FCANCEL after %zu cue(s): last FPROG ranges %s; the resume sent %zu more cue(s)\n",
                        c1.size(), have.c_str(), c2.size());
            ok(isCancel && progBefore && covered, "FCANCEL: the FPROG right before FDONE cancelled carries ranges covering every cue sent");
            ok(cross && !c2.empty(), "the second FILE (have = those ranges) finishes the rest: no cue overlaps the first job's");
        }
        std::printf("    %s\n", failures == before ? "v1 / have / focus / FFOCUS / straddle / reset / Q1-Q4 / burst / bad have / resume OK"
                                                  : "SEE FAILURES ABOVE");
    }

    // ---------------------------------------------------------------- [8d]
    std::printf("\n[8d] FILE MODE v2 -- live (the real models)\n");
    if (!live) { std::printf("    NOT RUN: offline pass\n"); return; }
    if (marks.size() != 24) { std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no file-mode fixtures\n"); return; }
    // The CPU switch (the user's GPU is busy): the smallest models that still exercise the path.
    const std::string keepAsr = g_fileAsrModelForTest, keepMt = g_fileMtModelForTest;
    if (g_forceCpu && g_fileAsrModelForTest.empty()) g_fileAsrModelForTest = "ggml-base.bin";
    if (g_forceCpu && g_fileMtModelForTest.empty())  g_fileMtModelForTest  = kMtModelFileSmall;
    if (g_forceCpu) std::printf("    CPU FORCED: asr %s, mt %s\n", g_fileAsrModelForTest.c_str(), g_fileMtModelForTest.c_str());
    {
        const int before = failures;
        const std::string meeting = Fu + "meeting.wav";
        uint64_t id = 8800;
        std::vector<trpipe::WireCue> wholeFile;          // the focus-50 job's cues: the whole meeting, the reference
        // a focus at 50 s: the subtitles start there, then the beginning
        {
            Shared sh;
            CollectSink sink;
            trpipe::WireFile f;
            f.jobId = ++id; f.path = meeting; f.langA = "en"; f.langB = "zh"; f.focusMs = 50000;
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(FileReq(f)); }
            std::unique_ptr<FileJob> job;
            FileJobStats st;
            fileRequests(sh, job, srv, sink, cfg, &st);
            while (job) { fileJobLap(job, srv, sink, sh.abortJob, false, &st); fileRequests(sh, job, srv, sink, cfg, &st); }
            failures += checkV2Wire(sink.lines, id, "8d focus", {}, true, 104018);
            const auto cs = cuesOfRun(sink.lines);
            wholeFile = cs;
            printJobStats(st);
            std::printf("    focus 50 s: %zu cue(s), first at %.2f s; %u run(s)\n", cs.size(),
                        cs.empty() ? -1.0 : cs[0].startMs / 1000.0, st.runs);
            ok(!cs.empty() && cs[0].startMs >= 47000 && cs[0].startMs <= 53000 && st.runs == 2 && cs.size() >= 20,
               "live: a FILE with focus 50 s subtitles from there first, then the start (2 runs, >= 20 cues)");
        }
        // FFOCUS mid-job: within one model call
        {
            Shared sh;
            CollectSink sink;
            trpipe::WireFile f;
            f.jobId = ++id; f.path = meeting; f.langA = "en"; f.langB = "zh";
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(FileReq(f)); }
            std::unique_ptr<FileJob> job;
            FileJobStats st;
            fileRequests(sh, job, srv, sink, cfg, &st);
            bool injected = false;
            size_t plansAt = 0;
            while (job) {
                fileJobLap(job, srv, sink, sh.abortJob, false, &st);
                if (!injected && job && countKind(sink.lines, "FCUE") >= 1) {
                    injectFocus(sh, 80000, true);
                    injected = true;
                    plansAt = job->st.plans.size();
                }
                fileRequests(sh, job, srv, sink, cfg, &st);
            }
            failures += checkV2Wire(sink.lines, id, "8d ffocus", {}, true, 104018);
            const bool jumped = st.plans.size() > plansAt && st.plans[plansAt].s >= 77000ull * 16;
            std::printf("    FFOCUS 80 s after the first cue: next window %.2f s\n",
                        st.plans.size() > plansAt ? st.plans[plansAt].s / 16000.0 : -1.0);
            ok(injected && jumped, "live: after FFOCUS the very next window is at the new focus");
        }
        // review P1, MEASURED LIVE (whisper + the MT): `have` starting and ending INSIDE utterances -- B_1 at 7.0 s,
        // D_1 at 20.0 s, D_4 at 65.0 s, B_6 at 80.0 s. Each join's cues, next to what the whole-file job above made
        // of the same seconds. A sentence across the START of an analysed stretch must keep its head here (the run
        // before it hears past the join and keeps the words that start before it) -- v2 first dropped that cue
        // whole; across its END, this job's run starts mid-sentence and shows the rest.
        {
            Shared sh;
            CollectSink sink;
            const std::string have = "7000-20000,65000-80000";
            trpipe::WireFile f;
            f.jobId = ++id; f.path = meeting; f.langA = "en"; f.langB = "zh"; f.have = have;
            { std::lock_guard<std::mutex> lk(sh.mx); sh.fileReqs.push_back(FileReq(f)); }
            std::unique_ptr<FileJob> job;
            FileJobStats st;
            fileRequests(sh, job, srv, sink, cfg, &st);
            while (job) { fileJobLap(job, srv, sink, sh.abortJob, false, &st); fileRequests(sh, job, srv, sink, cfg, &st); }
            std::vector<trpipe::MsRange> hv;
            trpipe::parseRanges(have, hv);
            failures += checkV2Wire(sink.lines, id, "8d joins", hv, true, 104018);
            const auto cs = cuesOfRun(sink.lines);
            std::printf("    joins inside speech, have %s: %zu cue(s), %u post-roll(s), %u pre-roll(s), %u cut word(s)\n",
                        have.c_str(), cs.size(), st.postrolls, st.prerolls, st.cutWords);
            int wanted = 0, kept = 0;
            for (const auto& x : hv) {
                for (const uint64_t b : { x.s, x.e }) {
                    const bool start = b == x.s;
                    const trpipe::WireCue* across = nullptr;
                    for (const auto& c : wholeFile) if (c.startMs + 1000 <= b && b + 300 < c.endMs) across = &c;
                    std::printf("      join %5.1f s (%s): whole-file %s\n", b / 1000.0, start ? "an analysed stretch starts" : "one ends",
                                across ? ("[" + std::to_string(across->startMs) + "-" + std::to_string(across->endMs) + "] "
                                          + across->src).c_str() : "-- no cue across it");
                    const trpipe::WireCue* side = nullptr;
                    for (const auto& c : cs) {
                        if (start && c.endMs <= b && c.endMs + 3000 >= b) side = &c;                // the head, before
                        if (!start && c.startMs >= b && c.startMs <= b + 3000 && !side) side = &c;   // the tail, after
                    }
                    std::printf("                         this job  %s\n",
                                side ? ("[" + std::to_string(side->startMs) + "-" + std::to_string(side->endMs) + "] "
                                        + side->src).c_str() : "-- none");
                    if (start && across) {
                        ++wanted;
                        if (side && side->startMs + 1500 >= across->startMs) ++kept;
                    }
                }
            }
            printJobStats(st);
            std::printf("    %d of %d sentence(s) across the start of an analysed stretch kept their head\n", kept, wanted);
            ok(kept == wanted, "live: a sentence across the start of an analysed stretch keeps its head (heard past the join)");
        }
        std::printf("    %s\n", failures == before ? "live focus / FFOCUS / joins OK" : "SEE FAILURES ABOVE");
    }
    g_fileAsrModelForTest = keepAsr;
    g_fileMtModelForTest  = keepMt;
}

// TEST-ONLY (WD): a --nofabric helper answering FILEs with [8]'s stand-ins (main(): the pipe client's
// deterministic run). Installed before any thread exists.
void installStandIns(const std::wstring& marksPath) {
    static std::vector<UttMark> marks;
    marks = readMarks(marksPath);
    if (marks.empty()) return;
    g_fileNoModelsForTest = true;
    g_fileAsrForTest = [](uint64_t s, uint64_t e, const std::string&) {
        ::Sleep(30);                                   // a little of a real call's latency
        return standInWhisper(marks, s, e);
    };
    g_fileMtForTest = [](const std::string& src, const std::string&, const std::string&, size_t) {
        return std::string("T(") + src + ")";
    };
}

// --selftest-v2 [offline]: section [8] alone. "offline" = [8a]-[8c] (no server at all).
int runSelftestV2(bool offline) {
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle Engine selftest: FILE MODE v2 only%s%s ===\n", offline ? " (offline)" : "",
                g_forceCpu ? " (CPU forced)" : "");
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    int failures = 0;
    fileModeV2Selftest(srv, cfg, failures, !offline);
    srv.killAll();
    if (failures) {
        std::printf("\nFILE MODE v2 SELFTEST FAILED (%d assertion(s))\n", failures);
        return 1;
    }
    std::printf("\nFILE MODE v2 SELFTEST PASSED\n");
    return 0;
}

// ===========================================================================
// [0s] THE FIRST-START SEED (SUVTITLE E8, director 23/10), offline -- through startConfig, the function runHost
// calls, in an ISOLATED APPDATA under %TEMP%: the process's APPDATA is pointed there for this section only and put
// back, so nothing here can reach the user's profile, nor AudioPB's.
// ===========================================================================
void seedSelftest(int& failures) {
    std::printf("\n[0s] first start: engine.txt seeded ONCE from AudioPB's translator.txt (an isolated APPDATA)\n");
    const int before = failures;
    auto ok = [&](bool cond, const char* what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
    };
    auto exists = [](const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto put = [](const std::wstring& p, const std::string& body) {
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wr = 0;
        const BOOL w = body.empty() ? TRUE : ::WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
        ::CloseHandle(h);
        return w && wr == body.size();
    };
    auto get = [](const std::wstring& p) {
        std::string s;
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return s;
        char b[4096];
        DWORD got = 0;
        while (::ReadFile(h, b, sizeof(b), &got, nullptr) && got) s.append(b, got);
        ::CloseHandle(h);
        return s;
    };
    auto isDefault = [](const TrConfig& c) {
        const TrConfig d;
        return c.mode == d.mode && c.langA == d.langA && c.langB == d.langB && c.preset == d.preset;
    };
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"SuVtitle_selftest_seed";
    const std::wstring appdata = root + L"\\appdata", apbDir = appdata + L"\\AudioPB", ownDir = appdata + L"\\SuVtitle";
    const std::wstring apb = apbDir + L"\\translator.txt", own = ownDir + L"\\engine.txt";
    ::CreateDirectoryW(root.c_str(), nullptr);
    ::CreateDirectoryW(appdata.c_str(), nullptr);
    ::CreateDirectoryW(apbDir.c_str(), nullptr);
    wchar_t keep[1024] = {}, keepLocal[1024] = {};
    const DWORD kn = ::GetEnvironmentVariableW(L"APPDATA", keep, 1024);
    const DWORD kl = ::GetEnvironmentVariableW(L"LOCALAPPDATA", keepLocal, 1024);
    ::SetEnvironmentVariableW(L"APPDATA", appdata.c_str());     // every TrConfig of this section reads HERE
    // ... and AudioPB's analyses (the fix round's second source) are looked for HERE: never the user's own
    const std::wstring local = root + L"\\local", recDir = local + L"\\AudioPB\\Translator files";
    ::CreateDirectoryW(local.c_str(), nullptr);
    ::SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    ok(TrConfig::path() == own, "TrConfig::path() follows APPDATA (the isolation this section relies on)");

    // 1. the first start: seeded -- the pair, the direction (zh2en read as b2a), the alias, the preset; ONLY those
    const std::string kApb = "# AudioPB Translator config v2\nPRESET\tquality\nMODE\tzh2en\nLANGA\ten\nLANGB\tzh-TW\n"
                             "MTCONTEXT\t3\nSPKON\t1\nWINPOS\t10 20 800 600\n";
    ::DeleteFileW(own.c_str());
    put(apb, kApb);
    std::string why;
    const TrConfig c1 = startConfig(false, "", "", &why);
    const std::string e1 = get(own);
    std::printf("    1st start: %s\n", why.c_str());
    ok(c1.mode == "b2a" && c1.langA == "en" && c1.langB == "zh-tw" && c1.preset == "quality",
       "first start: MODE / LANGA / LANGB / PRESET taken from AudioPB's file (zh2en -> b2a, zh-TW -> zh-tw)");
    ok(e1.rfind("# SuVtitle Engine config v1\n", 0) == 0 && e1.find("\nMODE\tb2a\n") != std::string::npos
       && e1.find("\nLANGA\ten\n") != std::string::npos && e1.find("\nLANGB\tzh-tw\n") != std::string::npos
       && e1.find("\nPRESET\tquality\n") != std::string::npos,
       "... engine.txt is written with them, under its own header");
    ok(e1.find("MTCONTEXT\t3") == std::string::npos && e1.find("WINPOS") == std::string::npos
       && e1.find("SPKON") == std::string::npos,
       "... and ONLY them: nothing else of AudioPB's file comes along (MTCONTEXT, WINPOS, SPKON)");
    ok(get(apb) == kApb, "AudioPB's translator.txt is untouched (read only)");

    // 2. the second start: engine.txt exists -> never again, whatever AudioPB's file says by then
    put(apb, "# AudioPB Translator config v2\nMODE\tauto\nLANGA\tfr\nLANGB\tde\nPRESET\tlight\n");
    const TrConfig c2 = startConfig(false, "", "", &why);
    ok(why.empty() && c2.langA == "en" && c2.langB == "zh-tw" && c2.mode == "b2a" && c2.preset == "quality"
       && get(own) == e1,
       "second start: NOT re-seeded (engine.txt exists) -- en/zh-tw b2a quality, the file byte for byte");

    // 3. an AudioPB file that does not read cleanly -> NO seed, the defaults (each from a fresh first start)
    struct Bad { const char* body; const char* what; };
    const Bad kBad[] = {
        { "LANGA\ten\nLANGB\tklingon\n",                 "a language code that does not read" },
        { "MODE\tsideways\nLANGA\ten\nLANGB\tfr\n",      "a direction that does not read" },
        { "LANGA\tfr\nLANGB\tFR\n",                      "a pair of one language" },
        { "PRESET\tturbo\nLANGA\ten\nLANGB\tja\n",       "a preset that does not read" },
        { "\x01\x02 not a config at all \xff\xfe\n",     "garbage" },
        { "",                                            "an empty file" },
    };
    for (const Bad& b : kBad) {
        ::DeleteFileW(own.c_str());
        put(apb, b.body);
        const TrConfig c = startConfig(false, "", "", &why);
        std::printf("    %-34s -> %s\n", b.what, why.c_str());
        ok(isDefault(c) && exists(own) && why.rfind("no seed:", 0) == 0,
           (std::string("malformed AudioPB file (") + b.what + "): no seed, the defaults, engine.txt materialised").c_str());
    }

    // 4. no AudioPB file at all -> the defaults
    ::DeleteFileW(own.c_str());
    ::DeleteFileW(apb.c_str());
    const TrConfig c4 = startConfig(false, "", "", &why);
    ok(isDefault(c4) && exists(own) && why.rfind("no seed:", 0) == 0, "no AudioPB file: the defaults, nothing seeded");

    // 5. a TEST engine (--nofabric) never seeds and never writes
    ::DeleteFileW(own.c_str());
    put(apb, kApb);
    const TrConfig c5 = startConfig(true, "", "", &why);
    ok(isDefault(c5) && !exists(own) && why.empty(), "a test engine: no seed, no engine.txt written");

    // 6. the command line goes ON TOP of the seed (an app that passes --mode), and that is what is written
    const TrConfig c6 = startConfig(false, "", "a2b", &why);
    ok(c6.mode == "a2b" && c6.langB == "zh-tw" && get(own).find("\nMODE\ta2b\n") != std::string::npos,
       "--mode on a first start: seeded pair, the command line's direction, both written");

    // 7. FIX ROUND RV2-3: the pair from AudioPB's NEWEST analysis when translator.txt has none (the user's real shape:
    //    MODE + PRESET, no LANGA/LANGB; his films analysed fr/zh-tw, fr/en, en/zh)
    auto record = [](const char* a, const char* b, const char* mode) {
        return std::string("# AudioPB Translator file cache v2\nSRC\tC:\\films\\x.mkv\t1000\t2000\nJOB\t") + a + "\t" + b
             + "\t" + mode + "\tauto\tggml-large-v3-turbo.bin\tQwen3-4B-Instruct-2507-Q5_K_M.gguf\t1\t60000\n"
               "ENGINE\t3\nAUD\t0\t0\nSRT\t\t0\t0\nRNG\t0-60000\nDONE\t1\nEND\t0\n";
    };
    auto setAge = [](const std::wstring& p, unsigned hoursAgo) {     // the last-write time, `hoursAgo` hours back
        HANDLE h = ::CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER u{};
        u.LowPart = now.dwLowDateTime;
        u.HighPart = now.dwHighDateTime;
        u.QuadPart -= (ULONGLONG)hoursAgo * 3600ull * 10000000ull;
        FILETIME t{};
        t.dwLowDateTime = u.LowPart;
        t.dwHighDateTime = u.HighPart;
        ::SetFileTime(h, nullptr, nullptr, &t);
        ::CloseHandle(h);
    };
    auto bigFile = [](const std::wstring& p, size_t bytes) {            // `bytes` of 'x', not one newline
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const std::string chunk((size_t)1 << 20, 'x');
        bool w = true;
        for (size_t left = bytes; left && w;) {
            const DWORD n = (DWORD)std::min(left, chunk.size());
            DWORD wr = 0;
            w = ::WriteFile(h, chunk.data(), n, &wr, nullptr) && wr == n;
            left -= n;
        }
        ::CloseHandle(h);
        return w;
    };
    auto peakMB = []() {
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
        return (double)pmc.PeakWorkingSetSize / 1048576.0;
    };
    ::CreateDirectoryW((local + L"\\AudioPB").c_str(), nullptr);
    ::CreateDirectoryW(recDir.c_str(), nullptr);
    const std::wstring rOld = recDir + L"\\0a11.cues", rNew = recDir + L"\\0b22.cues", rMid = recDir + L"\\0c33.cues";
    const std::string kOld = record("en", "ja", "auto"), kNew = record("fr", "zh-tw", "a2b"), kMid = record("fr", "en", "auto");
    put(rOld, kOld); setAge(rOld, 48);
    put(rNew, kNew); setAge(rNew, 1);                                  // the NEWEST
    put(rMid, kMid); setAge(rMid, 24);
    {   // a) records only (no translator.txt)
        ::DeleteFileW(own.c_str());
        ::DeleteFileW(apb.c_str());
        const TrConfig c = startConfig(false, "", "", &why);
        std::printf("    records only        -> %s\n", why.c_str());
        ok(c.langA == "fr" && c.langB == "zh-tw" && c.mode == "a2b" && c.preset == TrConfig{}.preset
           && why.find("seeded from the newest AudioPB analysis: fr/zh-tw") != std::string::npos
           && get(own).find("\nLANGB\tzh-tw\n") != std::string::npos,
           "RV2-3: no translator.txt -> the pair and direction of the NEWEST analysis (fr/zh-tw a2b), written");
    }
    {   // b) the user's shape: translator.txt MODE + PRESET, no pair
        ::DeleteFileW(own.c_str());
        put(apb, "# AudioPB Translator config v2\nPRESET\tlight\nMODE\tauto\nSPKON\t1\nWINPOS\t1 2 3 4\n");
        const TrConfig c = startConfig(false, "", "", &why);
        std::printf("    MODE+PRESET only    -> %s\n", why.c_str());
        ok(c.langA == "fr" && c.langB == "zh-tw" && c.mode == "a2b" && c.preset == "light",
           "RV2-3: translator.txt without a pair -> the newest analysis's pair + direction, translator.txt's PRESET");
    }
    {   // c) translator.txt HAS a pair: it wins over every analysis
        ::DeleteFileW(own.c_str());
        put(apb, "# AudioPB Translator config v2\nPRESET\tbalanced\nMODE\tb2a\nLANGA\ten\nLANGB\tde\n");
        const TrConfig c = startConfig(false, "", "", &why);
        ok(c.langA == "en" && c.langB == "de" && c.mode == "b2a" && why.find("translator.txt") != std::string::npos
           && why.find("newest") == std::string::npos,
           "RV2-3: a pair in translator.txt WINS over the analyses (en/de b2a)");
    }
    {   // d) the NEWEST record malformed (its language does not read), older valid ones -> the defaults: only the newest is asked
        ::DeleteFileW(own.c_str());
        ::DeleteFileW(apb.c_str());
        const std::string bad = record("fr", "klingon", "auto");
        put(rNew, bad); setAge(rNew, 1);
        const TrConfig c = startConfig(false, "", "", &why);
        std::printf("    newest malformed    -> %s\n", why.c_str());
        ok(isDefault(c) && why.rfind("no seed:", 0) == 0, "RV2-3: a malformed NEWEST analysis -> the defaults (never an older one)");
        put(rNew, kNew); setAge(rNew, 1);
    }
    {   // e) the newest record's JOB line lies beyond its first 16 KB -> not read -> the defaults
        ::DeleteFileW(own.c_str());
        const std::string lateJob = "# AudioPB Translator file cache v2\nSRC\tC:\\" + std::string(20000, 'p') + "\t1\t2\n"
                                  + "JOB\tfr\tzh-tw\ta2b\tauto\tx\ty\t1\t60000\n";     // (not `far`: a Windows macro)
        put(rNew, lateJob); setAge(rNew, 1);
        const TrConfig c = startConfig(false, "", "", &why);
        ok(isDefault(c) && why.find("first 16 KB") != std::string::npos, "RV3-2: a JOB line past a record's first 16 KB is never read");
    }
    {   // f) the newest "record" is 100 MB with no newline -> the defaults, BOUNDED (time and memory)
        ::DeleteFileW(own.c_str());
        ok(bigFile(rNew, (size_t)100 << 20), "a 100 MB newline-less record is written (test material)");
        setAge(rNew, 1);
        const double m0 = peakMB(), t0 = qpcMs();
        const TrConfig c = startConfig(false, "", "", &why);
        const double ms = qpcMs() - t0, grew = peakMB() - m0;
        std::printf("    100 MB record       -> %.1f ms, peak working set +%.1f MB: %s\n", ms, grew, why.c_str());
        ok(isDefault(c) && why.rfind("no seed:", 0) == 0 && ms < 2000.0 && grew < 32.0,
           "RV3-2: a 100 MB newline-less record -> the defaults, its head only (< 2 s, < 32 MB)");
        ::DeleteFileW(rNew.c_str());
    }
    ok(get(rOld) == kOld && get(rMid) == kMid, "AudioPB's analyses are untouched (read only)");
    ::DeleteFileW(rOld.c_str());
    ::DeleteFileW(rMid.c_str());

    // 8. FIX ROUND RV3-2: bounded reads of a 100 MB newline-less translator.txt and engine.txt
    {
        ::DeleteFileW(own.c_str());
        ok(bigFile(apb, (size_t)100 << 20), "a 100 MB newline-less translator.txt is written (test material)");
        const double m0 = peakMB(), t0 = qpcMs();
        const TrConfig c = startConfig(false, "", "", &why);
        const double ms = qpcMs() - t0, grew = peakMB() - m0;
        std::printf("    100 MB translator   -> %.1f ms, peak working set +%.1f MB: %s\n", ms, grew, why.c_str());
        ok(isDefault(c) && why.find("over 64 KB") != std::string::npos && ms < 2000.0 && grew < 32.0,
           "RV3-2: a 100 MB newline-less translator.txt -> no seed, 64 KB read (< 2 s, < 32 MB)");
        ::DeleteFileW(apb.c_str());
    }
    {
        ok(bigFile(own, (size_t)100 << 20), "a 100 MB newline-less engine.txt is written (test material)");
        const double m0 = peakMB(), t0 = qpcMs();
        const TrConfig c = startConfig(false, "", "", &why);           // engine.txt exists: not a first start
        const double ms = qpcMs() - t0, grew = peakMB() - m0;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        ::GetFileAttributesExW(own.c_str(), GetFileExInfoStandard, &fa);
        std::printf("    100 MB engine.txt   -> %.1f ms, peak working set +%.1f MB, rewritten to %lu bytes\n", ms, grew,
                    (unsigned long)fa.nFileSizeLow);
        ok(isDefault(c) && why.empty() && ms < 2000.0 && grew < 32.0 && fa.nFileSizeHigh == 0 && fa.nFileSizeLow < 8192,
           "RV3-2: a 100 MB newline-less engine.txt -> 1 MB read at most, the defaults, the file rewritten small");
    }

    ::DeleteFileW(own.c_str());
    ::DeleteFileW(apb.c_str());
    ::RemoveDirectoryW(recDir.c_str());
    ::RemoveDirectoryW((local + L"\\AudioPB").c_str());
    ::RemoveDirectoryW(local.c_str());
    ::RemoveDirectoryW(ownDir.c_str());
    ::RemoveDirectoryW(apbDir.c_str());
    ::RemoveDirectoryW(appdata.c_str());
    ::RemoveDirectoryW(root.c_str());
    ::SetEnvironmentVariableW(L"APPDATA", (kn > 0 && kn < 1024) ? keep : nullptr);
    ::SetEnvironmentVariableW(L"LOCALAPPDATA", (kl > 0 && kl < 1024) ? keepLocal : nullptr);
    std::printf("    %s\n", failures == before ? "seeded once / never again / malformed = defaults / test engine writes nothing / "
                                                 "the newest analysis / bounded reads OK"
                                               : "SEE FAILURES ABOVE");
}

// ===========================================================================
// [0p] THE PAYLOAD: THE BUNDLE OR THE DEV PAYLOAD (SUVTITLE_PAYLOAD), NEVER A MIX (fix round RV3-3; the dev payload replaced the
// author's built-in tree in the publication wave, 2026-10-24), offline -- resolvePathsAt on layouts built under %TEMP% (empty
// files stand for the servers: only their PRESENCE is resolved here).
// ===========================================================================
void payloadSelftest(int& failures) {
    std::printf("\n[0p] payload: the bundle next to the exe, else the dev payload (SUVTITLE_PAYLOAD) -- never a mix; a missing part is named\n");
    const int before = failures;
    auto ok = [&](bool cond, const char* what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
    };
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring base = std::wstring(tmp) + L"SuVtitle_selftest_payload";
    ::CreateDirectoryW(base.c_str(), nullptr);
    std::vector<std::wstring> made;                                     // every file / folder, removed in reverse
    auto mkdir = [&](const std::wstring& d) { if (::CreateDirectoryW(d.c_str(), nullptr)) made.push_back(d); };
    auto touch = [&](const std::wstring& f) {
        HANDLE h = ::CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); made.push_back(f); }
    };
    // one BUNDLE layout: w = engine\whisper (0 none, 1 the folder, 2 + the exe), l = the same for llama, m = models\ (0/1)
    auto layout = [&](const wchar_t* name, int w, int l, int m) {
        const std::wstring r = base + L"\\" + name;
        mkdir(r);
        if (w || l) mkdir(r + L"\\engine");
        if (w >= 1) mkdir(r + L"\\engine\\whisper");
        if (w >= 2) touch(r + L"\\engine\\whisper\\whisper-server.exe");
        if (l >= 1) mkdir(r + L"\\engine\\llama");
        if (l >= 2) touch(r + L"\\engine\\llama\\llama-server.exe");
        if (m) mkdir(r + L"\\models");
        return r + L"\\";
    };
    // one DEV PAYLOAD layout (SUVTITLE_PAYLOAD): <root>\whisper, <root>\llama, <root>\models, the same 0/1/2 codes. No trailing backslash:
    // that is how a person writes the variable.
    auto devLayout = [&](const wchar_t* name, int w, int l, int m) {
        const std::wstring r = base + L"\\" + name;
        mkdir(r);
        if (w >= 1) mkdir(r + L"\\whisper");
        if (w >= 2) touch(r + L"\\whisper\\whisper-server.exe");
        if (l >= 1) mkdir(r + L"\\llama");
        if (l >= 2) touch(r + L"\\llama\\llama-server.exe");
        if (m) mkdir(r + L"\\models");
        return r;
    };
    auto under = [](const std::wstring& p, const std::wstring& r) { return !p.empty() && p.compare(0, r.size(), r) == 0; };
    const TrConfig none;                                                // no WHISPERBIN / LLAMABIN / MODELS
    const std::wstring noPayload;                                       // SUVTITLE_PAYLOAD unset

    {   // nothing next to the exe, no dev payload -> not found, and the sentence says how to fix it
        const std::wstring r = layout(L"empty", 0, 0, 0);
        const TrPaths p = resolvePathsAt(none, r, noPayload);
        std::printf("    nothing next to the exe, no SUVTITLE_PAYLOAD -> %s\n", p.ok() ? "ok (WRONG)" : p.missing.c_str());
        ok(!p.ok() && !p.devPayload && p.missing.find("was not found") != std::string::npos
           && p.missing.find("SUVTITLE_PAYLOAD") != std::string::npos,
           "nothing staged, no dev payload: 'not found', naming SUVTITLE_PAYLOAD");
    }
    {   // SUVTITLE_PAYLOAD names a folder that holds none of the three -> not found, and the sentence says so
        const std::wstring r = layout(L"empty2", 0, 0, 0);
        const std::wstring d = devLayout(L"dev_empty", 0, 0, 0);
        const TrPaths p = resolvePathsAt(none, r, d);
        std::printf("    SUVTITLE_PAYLOAD holds none of the three -> %s\n", p.ok() ? "ok (WRONG)" : p.missing.c_str());
        ok(!p.ok() && !p.devPayload && p.missing.find("was not found") != std::string::npos
           && p.missing.find("holds none of") != std::string::npos, "an empty SUVTITLE_PAYLOAD folder: 'not found', 'holds none of'");
        const TrPaths ghost = resolvePathsAt(none, r, base + L"\\no_such_folder");
        ok(!ghost.ok() && !ghost.devPayload && ghost.missing.find("was not found") != std::string::npos,
           "SUVTITLE_PAYLOAD naming a folder that does not exist: 'not found'");
    }
    {   // the whole bundle -> the bundle, all three parts
        const std::wstring r = layout(L"full", 2, 2, 1);
        const TrPaths p = resolvePathsAt(none, r, noPayload);
        ok(p.ok() && !p.devPayload && under(p.whisperExe, r) && under(p.llamaExe, r) && under(p.modelsDir, r) && p.missing.empty(),
           "a full bundle: its own three parts");
    }
    const std::wstring fullDev = devLayout(L"dev_full", 2, 2, 1);
    {   // nothing next to the exe, a whole dev payload -> the dev payload, all three parts (also with a trailing backslash / slash)
        const std::wstring r = layout(L"empty3", 0, 0, 0);
        for (const wchar_t* tail : { L"", L"\\", L"/" }) {
            const TrPaths p = resolvePathsAt(none, r, fullDev + tail);
            ok(p.ok() && p.devPayload && under(p.whisperExe, fullDev + L"\\") && under(p.llamaExe, fullDev + L"\\")
               && under(p.modelsDir, fullDev + L"\\") && p.missing.empty(),
               "nothing staged, a full SUVTITLE_PAYLOAD: its own three parts (with or without a trailing separator)");
        }
        std::printf("    nothing next to the exe, a full SUVTITLE_PAYLOAD -> the dev payload\n");
    }
    {   // a bundle next to the exe AND a dev payload: the bundle wins, whole
        const std::wstring r = layout(L"full_and_dev", 2, 2, 1);
        const TrPaths p = resolvePathsAt(none, r, fullDev);
        ok(p.ok() && !p.devPayload && under(p.whisperExe, r) && under(p.llamaExe, r) && under(p.modelsDir, r),
           "a bundle next to the exe beats SUVTITLE_PAYLOAD (an installed copy never changes with an environment variable)");
    }
    struct Part { const wchar_t* name; int w, l, m; const char* missing; const char* what; };
    const Part kParts[] = {
        { L"no_whisper",  0, 2, 1, "engine\\whisper\\whisper-server.exe", "engine\\whisper missing" },
        { L"no_llama",    2, 0, 1, "engine\\llama\\llama-server.exe",     "engine\\llama missing" },
        { L"no_models",   2, 2, 0, "models",                              "models missing" },
        { L"only_models", 0, 0, 1, "engine\\whisper\\whisper-server.exe, engine\\llama\\llama-server.exe", "only models" },
        { L"empty_whdir", 1, 2, 1, "engine\\whisper\\whisper-server.exe", "an empty engine\\whisper folder" },
    };
    for (const Part& c : kParts) {      // a part-staged BUNDLE, with a whole dev payload to tempt it: refused, nothing borrowed
        const std::wstring r = layout(c.name, c.w, c.l, c.m);
        const TrPaths p = resolvePathsAt(none, r, fullDev);
        std::printf("    %-34s -> %s\n", c.what, p.ok() ? "ok (WRONG)" : p.missing.c_str());
        const bool noMix = !under(p.whisperExe, fullDev + L"\\") && !under(p.llamaExe, fullDev + L"\\") && !under(p.modelsDir, fullDev + L"\\");
        ok(!p.ok() && !p.devPayload && noMix && p.missing.find("incomplete") != std::string::npos
           && p.missing.find(c.missing) != std::string::npos,
           (std::string("part-staged bundle (") + c.what + "): refused, the part NAMED, nothing borrowed from SUVTITLE_PAYLOAD").c_str());
    }
    const Part kDevParts[] = {
        { L"dev_no_whisper",  0, 2, 1, "whisper\\whisper-server.exe", "dev: whisper missing" },
        { L"dev_no_llama",    2, 0, 1, "llama\\llama-server.exe",     "dev: llama missing" },
        { L"dev_no_models",   2, 2, 0, "models",                      "dev: models missing" },
        { L"dev_only_models", 0, 0, 1, "whisper\\whisper-server.exe, llama\\llama-server.exe", "dev: only models" },
        { L"dev_empty_whdir", 1, 2, 1, "whisper\\whisper-server.exe", "dev: an empty whisper folder" },
    };
    for (const Part& c : kDevParts) {   // a part-staged DEV PAYLOAD: the same rule inside it
        const std::wstring r = layout(L"empty4", 0, 0, 0);
        const std::wstring d = devLayout(c.name, c.w, c.l, c.m);
        const TrPaths p = resolvePathsAt(none, r, d);
        std::printf("    %-34s -> %s\n", c.what, p.ok() ? "ok (WRONG)" : p.missing.c_str());
        ok(!p.ok() && p.devPayload && p.missing.find("incomplete") != std::string::npos
           && p.missing.find(c.missing) != std::string::npos && p.missing.find("SUVTITLE_PAYLOAD") != std::string::npos,
           (std::string("part-staged SUVTITLE_PAYLOAD (") + c.what + "): refused, the part NAMED").c_str());
    }
    {   // engine.txt's overrides still win part by part (MODELS / WHISPERBIN), even over a part-staged bundle or dev payload
        TrConfig o;
        o.models = base + L"\\full\\models";
        const std::wstring r = layout(L"over_models", 2, 2, 0);
        const TrPaths p = resolvePathsAt(o, r, noPayload);
        ok(p.ok() && p.modelsDir == o.models && under(p.whisperExe, r), "MODELS in engine.txt fills the bundle's missing models");
        TrConfig w;
        w.whisperBin = base + L"\\full\\engine\\whisper";
        const std::wstring r2 = layout(L"over_whisper", 0, 2, 1);
        const TrPaths p2 = resolvePathsAt(w, r2, noPayload);
        ok(p2.ok() && under(p2.whisperExe, base + L"\\full\\") && under(p2.llamaExe, r2), "WHISPERBIN in engine.txt fills the bundle's missing whisper");
        const std::wstring d = devLayout(L"dev_over", 0, 2, 1);
        const TrPaths p3 = resolvePathsAt(w, layout(L"empty5", 0, 0, 0), d);
        ok(p3.ok() && p3.devPayload && under(p3.whisperExe, base + L"\\full\\") && under(p3.llamaExe, d + L"\\"),
           "WHISPERBIN in engine.txt fills the dev payload's missing whisper");
    }
    for (auto it = made.rbegin(); it != made.rend(); ++it)
        if (!::DeleteFileW(it->c_str())) ::RemoveDirectoryW(it->c_str());
    ::RemoveDirectoryW(base.c_str());
    std::printf("    %s\n", failures == before ? "bundle / dev payload / part-staged named / overrides OK" : "SEE FAILURES ABOVE");
}

// ===========================================================================
// THE ENGINE'S WHOLE SELFTEST:  --selftest [offline]
// SUVTITLE SPLIT: AudioPB's --selftest minus its LIVE sections -- [1] the decimator (the fabric front end),
// [2] the live servers, [2bis] diarization, [3] / [3bis] the live pipeline and its commit engine, [3ter] the
// `auto` probe, [3quater] the MT drivers on the live pair, [4] / [4bis] / [4ter] / [5] the live finals, [6]
// Traditional Chinese end to end through the live path. Left: what a FILE job stands on -- [0] the wire
// contract (the whole of TrPipe.h, its live messages included: the header is shared with the app and its
// packers are proved here), [0bis] [0ter] [0quater] the languages, scripts, routing and filters offline --
// then [7] [8] [9], the file selftests (each also runs alone: --selftest-filemode / -v2 / -wh).
// `offline` (new; AudioPB's --selftest had none): [7] [8] [9] stop where they would start a model, exactly
// as their own `offline` does.
// ===========================================================================
int runSelftest(bool offline) {
    ::SetConsoleOutputCP(CP_UTF8);        // the transcripts are UTF-8 and half of them are Han
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle Engine selftest%s%s ===\n", offline ? " (offline)" : "",
                g_forceCpu ? " (CPU forced)" : "");
    int failures = 0;

    // ---- 0. THE WIRE CONTRACT ------------------------------------------------
    // Cheap, offline, and first on purpose: every field added in this wave is OPTIONAL and
    // APPENDED, and the only way that claim stays true is if something asserts it. Two
    // things are proved per message: a full one survives the round trip, and a message
    // SHORTENED to its pre-wave shape still parses (which is what a tray built before this
    // helper sends, and what a tray built before it will be sent).
    std::printf("\n[0] Wire contract (new fields round-trip; old-shaped messages still parse)\n");
    {
        auto ok = [&](bool cond, const char* what) {
            if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
        };
        auto shorten = [](const std::string& msg, size_t fields) {
            std::vector<std::string> t = trpipe::split(msg);
            if (t.size() > fields) t.resize(fields);
            return t;
        };

        // ---- STATE ----------------------------------------------------------
        trpipe::WireState s;
        s.status = "listening"; s.preset = "quality"; s.mode = "a2b";
        s.asrModel = asrModelsLabel(asrModelsFor("quality")); s.mtModel = kMtModelFile;
        s.gpu = true; s.spkOn = true; s.spkModel = "campplus"; s.spkCount = 3;
        s.langA = "fr"; s.langB = "ja";
        const std::string mS = trpipe::packState(s);
        trpipe::WireState r;
        ok(trpipe::unpackState(trpipe::split(mS), r) && r.langA == "fr" && r.langB == "ja"
           && r.mode == "a2b" && r.spkCount == 3 && r.asrModel == s.asrModel,
           "STATE round-trip with langA/langB");
        trpipe::WireState rOld;
        ok(trpipe::unpackState(shorten(mS, 11), rOld) && rOld.spkCount == 3
           && rOld.langA == "en" && rOld.langB == "zh",
           "STATE without the pair parses, and keeps the DEFAULT pair (not an empty one)");
        trpipe::WireState rAncient;
        ok(trpipe::unpackState(shorten(mS, 8), rAncient) && rAncient.status == "listening"
           && !rAncient.spkOn,
           "pre-diarization STATE (8 fields) still parses");

        // ---- LINE -----------------------------------------------------------
        trpipe::WireLine l;
        l.tEpochMs = 1700000000123ull; l.lang = "zh";
        l.src = "one\ttab\nand a newline\\";      // the escaper's three cases, in one string
        l.dst = "deux"; l.ms = 42; l.spkSlot = "In 1"; l.spkLang = "zh";
        l.pass = false; l.spkLabel = "S2"; l.segId = 7; l.waitMs = 531; l.dstLang = "fr";
        l.seq = 3; l.closes = false;
        const std::string mL = trpipe::packLine(l);
        trpipe::WireLine rl;
        ok(trpipe::unpackLine(trpipe::split(mL), rl) && rl.src == l.src && rl.dstLang == "fr"
           && rl.waitMs == 531 && rl.segId == 7 && rl.spkLabel == "S2" && !rl.pass
           && rl.seq == 3 && !rl.closes,
           "LINE round-trip with seq/closes (and escaping intact)");
        trpipe::WireLine rlClose;
        trpipe::WireLine lc = l; lc.seq = 4; lc.closes = true;
        ok(trpipe::unpackLine(trpipe::split(trpipe::packLine(lc)), rlClose)
           && rlClose.seq == 4 && rlClose.closes,
           "LINE closes=1 round-trip");
        // /!\ THE BACK-COMPAT RULE OF THIS WAVE, asserted rather than believed: a LINE from a
        // helper that predates commit-as-you-go has no seq and no closes, and a UI built for
        // this wave must read it as "one line, and it ENDS the utterance" -- not as a line
        // that leaves a live line hanging for ever.
        trpipe::WireLine rlOld;
        ok(trpipe::unpackLine(shorten(mL, 13), rlOld) && rlOld.dstLang == "fr"
           && rlOld.seq == 0 && rlOld.closes,
           "LINE without seq/closes parses, and its line CLOSES its utterance (seq 0)");
        trpipe::WireLine rlSeqOnly;
        ok(trpipe::unpackLine(shorten(mL, 14), rlSeqOnly) && rlSeqOnly.seq == 3
           && rlSeqOnly.closes,
           "LINE with seq but no closes: the seq is read, the line still closes");
        trpipe::WireLine rlW2;
        ok(trpipe::unpackLine(shorten(mL, 12), rlW2) && rlW2.waitMs == 531
           && rlW2.dstLang == "en" && rlW2.closes,
           "LINE without dstLang parses and reconstructs it (zh source -> en)");
        trpipe::WireLine rlAncient;
        ok(trpipe::unpackLine(shorten(mL, 8), rlAncient) && rlAncient.segId == 0
           && rlAncient.waitMs == 0 && rlAncient.seq == 0 && rlAncient.closes,
           "pre-segId LINE (8 fields) still parses");

        // ---- END ------------------------------------------------------------
        trpipe::WireEnd e;
        e.segId = 11;
        trpipe::WireEnd re;
        ok(trpipe::unpackEnd(trpipe::split(trpipe::packEnd(e)), re) && re.segId == 11,
           "END round-trip");
        trpipe::WireEnd bad;
        ok(!trpipe::unpackEnd(trpipe::split("END"), bad)
           && !trpipe::unpackEnd(trpipe::split("END\t0"), bad)
           && !trpipe::unpackEnd(trpipe::split(mL), bad),
           "a truncated END, an END 0 and a LINE are all refused by unpackEnd");

        // ---- PART -----------------------------------------------------------
        trpipe::WirePartial p;
        p.segId = 9; p.tEpochMs = 1700000000456ull; p.lang = "en";
        p.src = "set the levels"; p.dst = "regle les niveaux"; p.dstLang = "fr";
        const std::string mP = trpipe::packPartial(p);
        trpipe::WirePartial rp;
        ok(trpipe::unpackPartial(trpipe::split(mP), rp) && rp.dstLang == "fr"
           && rp.dst == p.dst && rp.segId == 9,
           "PART round-trip with dstLang");
        trpipe::WirePartial rpOld;
        ok(trpipe::unpackPartial(shorten(mP, 6), rpOld) && rpOld.dst == p.dst
           && rpOld.dstLang == "zh",
           "PART without dstLang parses and reconstructs it (en source -> zh)");
        trpipe::WirePartial rpNoDst;
        ok(trpipe::unpackPartial(shorten(mP, 5), rpNoDst) && rpNoDst.dst.empty()
           && rpNoDst.dstLang.empty(),
           "PART with no draft translation has NO dstLang (a language nothing is in)");

        // ---- CTL, including the tokens the tray still sends today -----------
        trpipe::WireCtl c;
        c.mode = "b2a"; c.preset = "auto"; c.clear = true; c.langA = "en"; c.langB = "ja";
        trpipe::WireCtl rc;
        ok(trpipe::unpackCtl(trpipe::split(trpipe::packCtl(c)), rc) && rc.mode == "b2a"
           && rc.preset == "auto" && rc.clear && rc.langA == "en" && rc.langB == "ja",
           "CTL round-trip with the pair");
        trpipe::WireCtl legacy;
        ok(trpipe::unpackCtl(trpipe::split("CTL\ten2zh\t\t0"), legacy)
           && legacy.mode == "a2b" && legacy.preset.empty() && !legacy.clear
           && legacy.langA.empty() && legacy.langB.empty(),
           "legacy CTL en2zh -> a2b, no pair, nothing else disturbed");
        trpipe::WireCtl legacy2;
        ok(trpipe::unpackCtl(trpipe::split("CTL\tzh2en\tquality\t1"), legacy2)
           && legacy2.mode == "b2a" && legacy2.preset == "quality" && legacy2.clear,
           "legacy CTL zh2en -> b2a");
        // ---- FILE MODE (wave WB): the seven messages, through the shared packers ----------
        const std::string nasty = "D:\\Vid\xc3\xa9os\\tab\there\\line\nbreak\\\\x.mkv";
        trpipe::WireFile wf;
        wf.jobId = 42; wf.path = nasty; wf.langA = "en"; wf.langB = "zh-tw"; wf.mode = "a2b"; wf.quality = "max";
        trpipe::WireFile rwf;
        ok(trpipe::unpackFile(trpipe::split(trpipe::packFile(wf)), rwf) && rwf.jobId == 42 && rwf.path == nasty
           && rwf.langA == "en" && rwf.langB == "zh-tw" && rwf.mode == "a2b" && rwf.quality == "max",
           "FILE round-trip (a path with a tab, a newline, backslashes and UTF-8)");
        trpipe::WireFile rwf2;
        ok(trpipe::unpackFile(shorten(trpipe::packFile(wf), 3), rwf2) && rwf2.path == nasty
           && rwf2.langA.empty() && rwf2.langB.empty() && rwf2.mode.empty() && rwf2.quality.empty(),
           "FILE with only id + path parses (the rest = the helper's current values)");
        ok(!trpipe::unpackFile(trpipe::split("FILE\t0\tC:\\a.mp4"), rwf2)
           && !trpipe::unpackFile(trpipe::split("FILE\t7\t"), rwf2),
           "FILE with id 0 or no path is refused");
        uint64_t cid = 0;
        ok(trpipe::unpackFileCancel(trpipe::split(trpipe::packFileCancel(9)), cid) && cid == 9
           && !trpipe::unpackFileCancel(trpipe::split("FCANCEL\t0"), cid),
           "FCANCEL round-trip; id 0 refused");
        trpipe::WireProxy wpx;
        wpx.jobId = 3; wpx.src = nasty; wpx.out = "C:\\cache\\p.wav";
        trpipe::WireProxy rpx;
        ok(trpipe::unpackProxy(trpipe::split(trpipe::packProxy(wpx)), rpx) && rpx.jobId == 3
           && rpx.src == nasty && rpx.out == wpx.out,
           "FPROXY round-trip");
        trpipe::WireFileProg wfp;
        wfp.jobId = 5; wfp.phase = "analyzing"; wfp.doneMs = 123456789012ull; wfp.totalMs = 3600000;
        wfp.asrModel = "ggml-large-v3.bin"; wfp.mtModel = kMtModelFile; wfp.hasVideo = true;
        trpipe::WireFileProg rfp;
        ok(trpipe::unpackFileProg(trpipe::split(trpipe::packFileProg(wfp)), rfp) && rfp.jobId == 5
           && rfp.phase == "analyzing" && rfp.doneMs == 123456789012ull && rfp.totalMs == 3600000
           && rfp.asrModel == wfp.asrModel && rfp.mtModel == wfp.mtModel && rfp.hasVideo,
           "FPROG round-trip");
        trpipe::WireCue wcu;
        wcu.jobId = 8; wcu.idx = 17; wcu.startMs = 61000; wcu.endMs = 63500; wcu.lang = "en";
        wcu.dstLang = "zh-tw"; wcu.pass = false; wcu.src = "Level\tis \"fine\"\\n";
        wcu.dst = "\xe5\xb0\x8d\xe7\x99\xbd\n\xe9\x9f\xb3\xe9\x87\x8f";
        trpipe::WireCue rcu;
        ok(trpipe::unpackCue(trpipe::split(trpipe::packCue(wcu)), rcu) && rcu.jobId == 8 && rcu.idx == 17
           && rcu.startMs == 61000 && rcu.endMs == 63500 && rcu.lang == "en" && rcu.dstLang == "zh-tw"
           && !rcu.pass && rcu.src == wcu.src && rcu.dst == wcu.dst,
           "FCUE round-trip (escaping intact)");
        ok(!trpipe::unpackCue(trpipe::split("FCUE\t8\t1\t5000\t4000\ten\tfr\t0\ta\tb"), rcu),
           "an FCUE that ends before it starts is refused");
        trpipe::WireFileDone wfd;
        wfd.jobId = 11; wfd.result = "cancelled"; wfd.count = 640;
        trpipe::WireFileDone rfd;
        ok(trpipe::unpackFileDone(trpipe::split(trpipe::packFileDone(wfd)), rfd) && rfd.jobId == 11
           && rfd.result == "cancelled" && rfd.count == 640 && rfd.err.empty(),
           "FDONE round-trip");
        ok(trpipe::unpackFileDone(trpipe::split("FDONE\t11\tweird\t0\tx"), rfd) && rfd.result == "error",
           "an FDONE result nobody knows reads as an error, never as a success");
        trpipe::WireProxyDone wpd;
        wpd.jobId = 4; wpd.ok = false; wpd.out = "C:\\c\\x.wav"; wpd.err = "superseded";
        trpipe::WireProxyDone rpd;
        ok(trpipe::unpackProxyDone(trpipe::split(trpipe::packProxyDone(wpd)), rpd) && rpd.jobId == 4
           && !rpd.ok && rpd.out == wpd.out && rpd.err == "superseded",
           "FPROXYDONE round-trip");
        std::printf("    STATE/LINE/PART/CTL + FILE/FCANCEL/FPROXY/FPROG/FCUE/FDONE/FPROXYDONE: %s\n",
                    failures ? "SEE FAILURES ABOVE" : "round-trip + back-compat OK");
    }

    // ---- 0bis. TRADITIONAL CHINESE (wave WA): the parts that need no server -------------
    // Offline and up front, for the same reason [0] is: these are the decisions the live
    // section [6] depends on, and a routing mistake here would show up there only as "the
    // transcript came out in the wrong characters" -- true, and useless for finding it.
    std::printf("\n[0bis] Traditional Chinese: codes, script votes, routing, prompts\n");
    {
        const int before = failures;
        auto ok = [&](bool cond, const char* what) {
            if (!cond) { std::printf("    ASSERT FAIL: %s\n", what); ++failures; }
        };
        // -- the door's vocabulary: every alias in, ONE canonical code out --------------
        ok(normalizeLangCode("zh-tw") == "zh-tw" && normalizeLangCode("zh-TW") == "zh-tw"
           && normalizeLangCode(" ZH_TW ") == "zh-tw" && normalizeLangCode("zh-hant") == "zh-tw"
           && normalizeLangCode("zh_Hant") == "zh-tw" && normalizeLangCode("zh-HK") == "zh-tw"
           && normalizeLangCode("zh-mo") == "zh-tw" && normalizeLangCode("CHT") == "zh-tw",
           "normalizeLangCode: zh-tw and all seven aliases -> zh-tw");
        ok(normalizeLangCode("zh") == "zh" && normalizeLangCode("EN") == "en"
           && normalizeLangCode("eng") == "en" && normalizeLangCode("cmn") == "zh"
           && normalizeLangCode("yue") == "yue" && normalizeLangCode(" fr ") == "fr",
           "normalizeLangCode: the pre-WA vocabulary is untouched");
        // The trap itself, stated as a refusal: nothing is ever decided by its first two
        // letters, so a near-miss is REFUSED rather than quietly becoming zh.
        ok(normalizeLangCode("zh-cn").empty() && normalizeLangCode("zhtw").empty()
           && normalizeLangCode("zh-twx").empty() && normalizeLangCode("zh-").empty()
           && normalizeLangCode("").empty() && normalizeLangCode("e1").empty(),
           "normalizeLangCode: a near-miss is refused, never truncated to two letters");
        ok(isHanLang("zh") && isHanLang("zh-tw") && !isHanLang("ja") && !isHanLang("en")
           && !isHanLang("yue"), "isHanLang: exactly zh and zh-tw");
        {   // ...and the code crosses the pipe intact, both ways (STATE out, CTL in).
            trpipe::WireState s; s.langA = "en"; s.langB = "zh-tw";
            trpipe::WireState r;
            trpipe::WireCtl c; c.langA = "en"; c.langB = "zh-TW";
            trpipe::WireCtl rc;
            ok(trpipe::unpackState(trpipe::split(trpipe::packState(s)), r) && r.langB == "zh-tw"
               && trpipe::unpackCtl(trpipe::split(trpipe::packCtl(c)), rc)
               && normalizeLangCode(rc.langB) == "zh-tw",
               "zh-tw survives STATE and CTL (and the CTL alias lands canonical at the door)");
        }

        // -- the script vote ------------------------------------------------------------
        // The three Traditional lines are the 10/10 bench's OWN Qwen3-4B outputs; the
        // Simplified ones are the system converter's rendering of the first two.
        const char* kT1 = "\xe5\xa5\xbd\xef\xbc\x8c\xe6\x88\x91\xe5\x80\x91\xe9\x96\x8b\xe5\xa7\x8b"
                          "\xe5\x89\x8d\xe5\x85\x88\xe8\xa8\xad\xe5\xae\x9a\xe4\xb8\x80\xe4\xb8\x8b"
                          "\xe9\x9f\xb3\xe9\x87\x8f\xe3\x80\x82";   // U+597D,FF0C,6211,5011,958B,59CB,524D,5148,8A2D,5B9A,4E00,4E0B,97F3,91CF,3002
        const char* kT2 = "\xe5\xb0\x8d\xe7\x99\xbd\xe9\x9f\xb3\xe9\x87\x8f\xe5\x89\x9b\xe5\xa5\xbd"
                          "\xef\xbc\x8c\xe4\xbd\x86\xe7\xac\xac\xe4\xba\x8c\xe5\xa0\xb4\xe7\x9a\x84"
                          "\xe6\xae\x98\xe9\x9f\xbf\xe5\xa4\xaa\xe9\x95\xb7\xe4\xba\x86\xe3\x80\x82";
                                                            // U+5C0D,767D,97F3,91CF,525B,597D,FF0C,4F46,7B2C,4E8C,5834,7684,6B98,97FF,592A,9577,4E86,3002
        const char* kT3 = "\xe4\xbd\xa0\xe8\x83\xbd\xe4\xb8\x8d\xe8\x83\xbd\xe6\x8a\x8a\xe7\xac\xac"
                          "\xe5\x9b\x9b\xe6\xa2\x9d\xe9\x9d\x9c\xe9\x9f\xb3\xef\xbc\x8c\xe4\xbb\x8a"
                          "\xe6\x99\x9a\xe6\x8a\x8a\xe5\x88\x86\xe8\xbb\x8c\xe5\xaf\x84\xe7\xb5\xa6"
                          "\xe6\x88\x91\xef\xbc\x9f";               // U+4F60,80FD,4E0D,80FD,628A,7B2C,56DB,689D,975C,97F3,FF0C,4ECA,665A,628A,5206,8ECC,5BC4,7D66,6211,FF1F
        const char* kS1 = "\xe5\xa5\xbd\xef\xbc\x8c\xe6\x88\x91\xe4\xbb\xac\xe5\xbc\x80\xe5\xa7\x8b"
                          "\xe5\x89\x8d\xe5\x85\x88\xe8\xae\xbe\xe5\xae\x9a\xe4\xb8\x80\xe4\xb8\x8b"
                          "\xe9\x9f\xb3\xe9\x87\x8f\xe3\x80\x82";   // U+597D,FF0C,6211,4EEC,5F00,59CB,524D,5148,8BBE,5B9A,4E00,4E0B,97F3,91CF,3002
        const char* kS2 = "\xe5\xaf\xb9\xe7\x99\xbd\xe9\x9f\xb3\xe9\x87\x8f\xe5\x88\x9a\xe5\xa5\xbd"
                          "\xef\xbc\x8c\xe4\xbd\x86\xe7\xac\xac\xe4\xba\x8c\xe5\x9c\xba\xe7\x9a\x84"
                          "\xe6\xb7\xb7\xe5\x93\x8d\xe5\xa4\xaa\xe9\x95\xbf\xe4\xba\x86\xe3\x80\x82";
                                                            // U+5BF9,767D,97F3,91CF,521A,597D,FF0C,4F46,7B2C,4E8C,573A,7684,6DF7,54CD,592A,957F,4E86,3002
        const char* kTie = "\xe9\x9f\xb3\xe9\x87\x8f\xe5\xbe\x88\xe5\xa5\xbd\xe3\x80\x82";   // U+97F3,91CF,5F88,597D,3002
        auto votes = [](const char* s, int wantT, int wantS) {
            int t = -1, v = -1;
            hanScriptVotes(s, t, v);
            return t == wantT && v == wantS;
        };
        ok(votes(kT1, 3, 0) && votes(kT2, 5, 0) && votes(kT3, 3, 0),
           "hanScriptVotes: the bench's three Traditional lines vote 3/5/3 Traditional, 0 Simplified");
        ok(votes(kS1, 0, 3) && votes(kS2, 0, 5),
           "hanScriptVotes: their Simplified renderings vote the other way, by the same counts");
        ok(votes(kTie, 0, 0) && votes("Set the levels.", 0, 0) && votes("", 0, 0),
           "hanScriptVotes: shared characters and Latin cast no vote (a tie, not a guess)");
        {   // THE SAFETY NET converts what is Simplified, and NOTHING else -- which is the only
            // property that makes it safe to run on every zh-tw text unconditionally.
            bool chS = false, chT = true, chL = true, chA = true;
            const char* kAmbig = "\xe5\x90\x8e\xe9\x87\x8c";   // U+540E,91CC: Traditional characters too
            ok(toTraditional(kS1, &chS) == kT1 && chS,
               "toTraditional: the Simplified rendering comes back as the bench's Traditional line");
            ok(toTraditional(kT1, &chT) == kT1 && !chT
               && toTraditional("Set the levels.", &chL) == "Set the levels." && !chL,
               "toTraditional: Traditional text and Latin come back untouched (changed=false)");
            ok(toTraditional(kAmbig, &chA) == kAmbig && !chA,
               "toTraditional: U+540E and U+91CC (valid Traditional characters as well) are left alone");
            // ...and the WORD-level half (ELS), on the characters a per-character map gets wrong:
            // U+540E,6765 -> U+5F8C,4F86, U+5934,53D1 -> U+982D,9AEE (not U+982D,767C), U+4EC0,4E48 -> U+4EC0,9EBC (U+4E48 is never mapped by LCMapStringEx).
            // A regression to the character map alone fails here.
            bool chW = false;
            const char* kSw = "\xe5\x90\x8e\xe6\x9d\xa5\xe6\x88\x91\xe4\xbb\xac\xe6\x89\x8d\xe5\x8f\x91"
                              "\xe7\x8e\xb0\xe5\xa4\xb4\xe5\x8f\x91\xe7\x9a\x84\xe9\x97\xae\xe9\xa2\x98"
                              "\xe6\x98\xaf\xe4\xbb\x80\xe4\xb9\x88\xe3\x80\x82";   // U+540E,6765,6211,4EEC,624D,53D1,73B0,5934,53D1,7684,95EE,9898,662F,4EC0,4E48,3002
            const char* kTw = "\xe5\xbe\x8c\xe4\xbe\x86\xe6\x88\x91\xe5\x80\x91\xe6\x89\x8d\xe7\x99\xbc"
                              "\xe7\x8f\xbe\xe9\xa0\xad\xe9\xab\xae\xe7\x9a\x84\xe5\x95\x8f\xe9\xa1\x8c"
                              "\xe6\x98\xaf\xe4\xbb\x80\xe9\xba\xbc\xe3\x80\x82";   // U+5F8C,4F86,6211,5011,624D,767C,73FE,982D,9AEE,7684,554F,984C,662F,4EC0,9EBC,3002
            const std::string gotW = toTraditional(kSw, &chW);
            ok(gotW == kTw && chW,
               "toTraditional: word-level conversion (\xe5\x90\x8e\xe6\x9d\xa5/\xe5\xa4\xb4\xe5\x8f\x91/\xe4\xbb\x80\xe4\xb9\x88 -> \xe5\xbe\x8c\xe4\xbe\x86/\xe9\xa0\xad\xe9\xab\xae/\xe4\xbb\x80\xe9\xba\xbc), not character-level");
            if (gotW != kTw) std::printf("    got: %s\n", gotW.c_str());
        }
        {   // THE TABLE AGAINST THE SYSTEM'S OWN CONVERTER, every row, both directions -- and
            // each side a fixed point of its own script, which is what "used in that script
            // only" means mechanically. A row the converter disagrees with is a vote that lies.
            size_t n = 0;
            const HanPair* rows = hanScriptPairs(n);
            int bad = 0, unchecked = 0;
            auto map1 = [](uint32_t cp, DWORD flag, const wchar_t* loc) -> uint32_t {
                const wchar_t in[2] = { (wchar_t)cp, 0 };
                wchar_t out[8] = {};
                const int got = ::LCMapStringEx(loc, flag, in, 1, out, 8, nullptr, nullptr, 0);
                return got == 1 ? (uint32_t)out[0] : 0u;
            };
            for (size_t i = 0; i < n; ++i) {
                const uint32_t s2t = map1(rows[i].simp, LCMAP_TRADITIONAL_CHINESE, L"zh-TW");
                const uint32_t t2s = map1(rows[i].trad, LCMAP_SIMPLIFIED_CHINESE,  L"zh-CN");
                const uint32_t s2s = map1(rows[i].simp, LCMAP_SIMPLIFIED_CHINESE,  L"zh-CN");
                const uint32_t t2t = map1(rows[i].trad, LCMAP_TRADITIONAL_CHINESE, L"zh-TW");
                if (!s2t || !t2s || !s2s || !t2t) { ++unchecked; continue; }
                if (s2t != rows[i].trad || t2s != rows[i].simp || s2s != rows[i].simp
                    || t2t != rows[i].trad) {
                    std::printf("    row U+%04X/U+%04X: converter says S->T U+%04X, T->S U+%04X\n",
                                (unsigned)rows[i].simp, (unsigned)rows[i].trad,
                                (unsigned)s2t, (unsigned)t2s);
                    ++bad;
                }
            }
            std::printf("    script table: %zu pairs, %d disagree with LCMapStringEx, %d unchecked\n",
                        n, bad, unchecked);
            ok(n >= 60, "the script table has at least 60 pairs");
            ok(bad == 0, "every script-table row agrees with the system converter, both ways");
            ok(unchecked == 0, "the system converter answered for every row (else: NOT CHECKED)");
        }

        // -- routing: which Chinese a Han line is, and where it goes ----------------------
        auto asrOf = [](const char* text, const char* lang, bool chinese) {
            AsrResult a; a.text = text; a.language = lang; a.chinese = chinese; return a;
        };
        auto pairOf = [](const char* mode, const char* a, const char* b) {
            Routing r; r.mode = mode; r.langA = a; r.langB = b; return r;
        };
        const AsrResult hanT = asrOf(kT1, "zh", true), hanS = asrOf(kS1, "zh", true);
        const AsrResult hanTie = asrOf(kTie, "zh", true), en = asrOf("Set the levels.", "en", false);
        const AsrResult wrongLabel = asrOf("Set the levels.", "zh", false);   // label lies
        const AsrResult yue = asrOf(kT2, "yue", true);
        const Routing enTw = pairOf("auto", "en", "zh-tw"), twEn = pairOf("auto", "zh-tw", "en");
        const Routing enZh = pairOf("auto", "en", "zh"),    zhTw = pairOf("auto", "zh", "zh-tw");
        const Routing twZh = pairOf("auto", "zh-tw", "zh"), enFr = pairOf("auto", "en", "fr");
        ok(sourceLangOf(hanT, enTw) == "zh-tw" && sourceLangOf(hanS, enTw) == "zh-tw"
           && sourceLangOf(yue, enTw) == "zh-tw" && sourceLangOf(en, enTw) == "en",
           "en<->zh-tw: EVERY Han line is zh-tw (the pair decides, whatever the script)");
        ok(sourceLangOf(hanT, enZh) == "zh" && sourceLangOf(hanS, enZh) == "zh",
           "en<->zh: every Han line is zh, exactly as before WA");
        ok(sourceLangOf(hanT, zhTw) == "zh-tw" && sourceLangOf(hanS, zhTw) == "zh"
           && sourceLangOf(hanTie, zhTw) == "zh" && sourceLangOf(hanTie, twZh) == "zh-tw",
           "zh<->zh-tw: the script decides, and a tie is the A half");
        ok(sourceLangOf(hanT, enFr) == "zh" && sourceLangOf(hanS, enFr) == "zh",
           "no Chinese in the pair: a Han line is zh, a third language, as before");
        ok(sourceLangOf(wrongLabel, enTw) == "en" && sourceLangOf(wrongLabel, twEn) == "en",
           "a 'chinese' label with no Han text is the OTHER half -- zh-tw as A included");
        {
            const Route r1 = routeFor(enTw, "zh-tw"), r2 = routeFor(enTw, "en");
            const Route r3 = routeFor(pairOf("a2b", "en", "zh-tw"), "zh-tw");
            const Route r4 = routeFor(pairOf("b2a", "en", "zh-tw"), "zh-tw");
            ok(r1.dstLang == "en" && !r1.pass && r2.dstLang == "zh-tw" && !r2.pass
               && r3.pass && r3.dstLang == "zh-tw" && r4.dstLang == "en" && !r4.pass,
               "routeFor: zh-tw is an ordinary half (auto both ways, a2b passthrough, b2a)");
        }

        // -- what whisper is told: a zh-tw session prompts in TRADITIONAL ---------------
        {
            const std::string g = asrGlossaryFor("zh-tw");
            int t = 0, s = 0;
            hanScriptVotes(g, t, s);
            ok(t > 0 && s == 0 && g.find("\xef\xbc\x9a") != std::string::npos
               && g.size() >= 3 && g.compare(g.size() - 3, 3, "\xe3\x80\x82") == 0,
               "asrGlossaryFor(zh-tw): Traditional only, punctuated (full-width colon, full stop)");
            ok(g != asrGlossaryFor("zh") && !asrGlossaryFor("zh").empty(),
               "...and it is not the Simplified line");
            TrContext c;
            c.lastFinalLang = "zh-tw";
            // (The PROMPT BUILDER only: whether it reaches the decode is proved in [6], on the
            // prompt each final was actually sent -- WA review.)
            ok(asrPromptFor(enTw, c, expectedLang(enTw, c)) == g,
               "auto: after a zh-tw line, the prompt BUILT for the next decode is Traditional");
            TrContext fresh;
            ok(asrPromptFor(pairOf("b2a", "en", "zh-tw"), fresh,
                            expectedLang(pairOf("b2a", "en", "zh-tw"), fresh)) == g,
               "b2a (zh-tw into en): prompted in Traditional from the very first line");
            c.rememberSrc("zh-tw", kT2);
            const std::string withTail = asrPromptFor(enTw, c, "zh-tw");
            ok(withTail.compare(0, g.size(), g) == 0 && withTail.find(kT2) != std::string::npos,
               "...and the zh-tw prompt carries the last zh-tw line as its tail");
        }

        // -- what the translator is told ---------------------------------------------------
        {
            const std::string name = "Traditional Chinese as used in Taiwan ("
                                     "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87, "
                                     "\xe5\x8f\xb0\xe7\x81\xa3\xe7\x94\xa8\xe8\xaa\x9e)";
            ok(langPromptName("zh-tw") == name, "langPromptName(zh-tw) is the bench's phrasing");
            const std::string p1 = qwenSystemPrompt("en", "zh-tw");
            const std::string p2 = qwenSystemPrompt("zh-tw", "en");
            const std::string p3 = qwenSystemPrompt("en", "zh");
            const std::string p4 = qwenSystemPrompt("zh", "zh-tw");
            // The markers are the glossaries' own FADER entries, which is the one term the two
            // studios spell differently (U+63A8,687F / U+63A8,5B50) -- so the test cannot pass on the wrong one.
            const char* twFader  = "fader -> \\u63a8\\u687f";
            const char* zhFader  = "fader -> \\u63a8\\u5b50";
            const char* twFaderR = "\\u63a8\\u687f -> fader";
            const char* zhFaderR = "\\u63a8\\u5b50 -> fader";
            ok(p1.find(" into " + name + ".") != std::string::npos,
               "en->zh-tw: the target name reaches the prompt VERBATIM (raw UTF-8, not escaped)");
            ok(p1.find("\\\\") == std::string::npos && p2.find("\\\\") == std::string::npos,
               "...and nothing in either zh-tw prompt is double-escaped");
            ok(p1.find(twFader) != std::string::npos && p1.find(zhFader) == std::string::npos,
               "en->zh-tw: the TRADITIONAL glossary, and not the Simplified one");
            ok(p2.find("natural spoken English") != std::string::npos
               && p2.find(twFaderR) != std::string::npos && p2.find(zhFaderR) == std::string::npos,
               "zh-tw->en: the reverse Traditional glossary");
            ok(p3.find("Simplified Chinese (Mandarin)") != std::string::npos
               && p3.find(zhFader) != std::string::npos && p3.find(twFader) == std::string::npos,
               "en->zh: unchanged -- the Simplified name and glossary");
            ok(p4.find("Audio glossary") == std::string::npos,
               "zh->zh-tw (a script conversion): no glossary, both sides already are the terms");
            // WB (WA review): the two entries that turn everyday words into studio jargon are
            // gone from the Chinese -> English directions only; the forward entries stay.
            const std::string p5 = qwenSystemPrompt("zh", "en");
            ok(p2.find("\\u8f38\\u51fa ->") == std::string::npos
               && p2.find("-> delay") == std::string::npos
               && p5.find("-> delay") == std::string::npos,
               "zh-tw->en / zh->en: no 'output -> print' and no 'latency/delay -> delay' entry");
            ok(p1.find("print -> \\u8f38\\u51fa") != std::string::npos
               && p1.find("delay -> \\u5ef6\\u9072") != std::string::npos
               && p3.find("delay -> \\u5ef6\\u8fdf") != std::string::npos,
               "en->zh-tw / en->zh: the forward print and delay entries are kept");
        }

        // -- WB, after the WA review: Japanese / Korean are not Chinese --------------------
        {
            // U+5B66,6821,306E,4F1A,8B70,3092,59CB,3081,307E,3059 -- Han characters AND kana, labelled ja by whisper.
            const char* kJa = "\xe5\xad\xa6\xe6\xa0\xa1\xe3\x81\xae\xe4\xbc\x9a\xe8\xad\xb0\xe3\x82\x92"
                              "\xe5\xa7\x8b\xe3\x82\x81\xe3\x81\xbe\xe3\x81\x99";
            const char* kKo = "\xed\x9a\x8c\xec\x9d\x98\xeb\xa5\xbc \xec\x8b\x9c\xec\x9e\x91\xed\x95\xa9"
                              "\xeb\x8b\x88\xeb\x8b\xa4";                    // U+D68C,C758,B97C U+C2DC,C791,D569,B2C8,B2E4
            ok(!chineseByScript(kJa, "ja") && !chineseByScript(kJa, "zh")
               && !chineseByScript(kKo, "ko") && chineseByScript(kT1, "zh")
               && chineseByScript(kS1, "ja"),
               "chineseByScript: any kana / Hangul is NOT Chinese (whatever the label); kanji-only still is");
            AsrResult ja; ja.text = kJa; ja.language = "ja"; ja.chinese = chineseByScript(kJa, "ja");
            AsrResult ko; ko.text = kKo; ko.language = "ko"; ko.chinese = chineseByScript(kKo, "ko");
            const std::string jaLang = sourceLangOf(ja, enTw), koLang = sourceLangOf(ko, enTw);
            ok(jaLang == "ja" && koLang == "ko",
               "en<->zh-tw: a Japanese line stays ja and a Korean one ko -- never zh-tw");
            // ...which is what keeps the net off it: finaliseAudio converts the SOURCE only when
            // it is zh-tw. Stated on the text the net would have produced, so a regression that
            // routes it to zh-tw shows the damage it would do.
            ok(jaLang != "zh-tw" && toTraditional(kJa) != kJa,
               "...and that matters: the net WOULD rewrite this Japanese (\xe5\xad\xa6\xe6\xa0\xa1/\xe4\xbc\x9a\xe8\xad\xb0 -> \xe5\xad\xb8\xe6\xa0\xa1/\xe6\x9c\x83\xe8\xad\xb0)");
        }

        // -- WB, after the WA review: ELS keeps every byte it does not convert -------------
        {
            const char* kMix   = "\xe9\x80\x99\xe4\xb8\xaa reverb \xe5\xa4\xaa\xe5\xa4\x9a\xe4\xba\x86";   // U+9019,4E2A reverb U+592A,591A,4E86
            const char* kMixT  = "\xe9\x80\x99\xe5\x80\x8b reverb \xe5\xa4\xaa\xe5\xa4\x9a\xe4\xba\x86";   // U+9019,500B reverb U+592A,591A,4E86
            const char* kMix2  = "Set the EQ, \xe7\x84\xb6\xe5\x90\x8e\xe6\x8a\x8a\xe9\x9f\xb3\xe9\x87\x8f"
                                 "\xe8\xb0\x83\xe9\xab\x98 3 dB!";                                       // ..., U+7136,540E,628A,97F3,91CF,8C03,9AD8 3 dB!
            const char* kMix2T = "Set the EQ, \xe7\x84\xb6\xe5\xbe\x8c\xe6\x8a\x8a\xe9\x9f\xb3\xe9\x87\x8f"
                                 "\xe8\xaa\xbf\xe9\xab\x98 3 dB!";                                       // ..., U+7136,5F8C,628A,97F3,91CF,8ABF,9AD8 3 dB!
            const std::string m1 = toTraditional(kMix), m2 = toTraditional(kMix2);
            ok(m1 == kMixT, "toTraditional: a mixed line converts its Han and keeps ' reverb ' intact");
            ok(m2 == kMix2T, "toTraditional: Latin, digits and punctuation around Han survive the conversion");
            if (m1 != kMixT) std::printf("    got: %s\n", m1.c_str());
            if (m2 != kMix2T) std::printf("    got: %s\n", m2.c_str());
        }

        // -- WB: the hallucination filter -- structure (LIVE and file alike), Traditional junk,
        //    other alphabets, and the prompt's echo ----------------------------------------
        {
            // Wrapped annotations are never speech, at any level (-10 dBFS = loud).
            ok(looksLikeHallucination("*music*", -10.0) && looksLikeHallucination(" *Music* ", -10.0),
               "junk at any level: *music* (turbo's whole answer to a music chord, 10/10)");
            ok(looksLikeHallucination("\xe2\x99\xaa la la la \xe2\x99\xaa", -10.0)
               && looksLikeHallucination("\xe2\x99\xaa\xe2\x99\xaa", -10.0),
               "junk at any level: a line whisper presents as music (a note at either end)");
            ok(looksLikeHallucination("[Music] [Applause]", -10.0)
               && looksLikeHallucination("(Laughter).", -10.0)
               && looksLikeHallucination("\xef\xbc\x88\xe9\x9f\xb3\xe4\xb9\x90\xef\xbc\x89", -10.0),
               "junk at any level: [..] [..], (..) with trailing punctuation, full-width parentheses");
            ok(!looksLikeHallucination("(laughs) I agree (laughs)", -10.0)
               && !looksLikeHallucination("I like *this* one.", -10.0)
               && !looksLikeHallucination("Music is great.", -10.0),
               "...but speech AROUND an annotation, and the word 'music', are kept");
            ok(!looksLikeHallucination("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, "
                                       "\xd0\xba\xd0\xb0\xd0\xba \xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0?", -10.0),
               "a Russian line is speech (other alphabets were 'no letters' -> dropped before WB)");
            // Phonetic garbage: what `small` writes on a quiet noise tail (10/10), at any level.
            ok(looksLikeHallucination("\xca\xbb\xca\xbb\xc9\x94\xca\xbb\xc9\x94\xca\xbb\xc9\x94", -10.0)
               && looksLikeHallucination("recognis \xe1\xb4\x89 \xca\xb0\xe1\xb4\x97\xe1\xb4\x97 Cream", -10.0)
               && !looksLikeHallucination("Hawai\xca\xbbi is lovely.", -10.0),
               "phonetic symbols: two or more = junk ('recognis ... Cream'); one stray mark is kept");
            {   // whisper's own confidence, duration-weighted over its segments
                AsrResult c;
                c.segments.resize(2);
                c.segments[0].start = 0.0; c.segments[0].end = 1.0; c.segments[0].avgLogprob = -0.2;
                c.segments[1].start = 1.0; c.segments[1].end = 4.0; c.segments[1].avgLogprob = -2.0;
                ok(std::fabs(asrConfidence(c) - (-1.55)) < 1e-9 && asrConfidence(AsrResult{}) == 0.0,
                   "asrConfidence: duration-weighted mean of avg_logprob (0 = no segment, no verdict)");
            }
            // The Traditional junk captions: quiet = junk, loud = a person saying it.
            const char* kTwThanks = "\xe8\xac\x9d\xe8\xac\x9d\xe8\xa7\x80\xe7\x9c\x8b";   // U+8B1D,8B1D,89C0,770B
            ok(looksLikeHallucination(kTwThanks, -40.0) && !looksLikeHallucination(kTwThanks, -12.0)
               && looksLikeHallucination("\xe8\xac\x9d\xe8\xac\x9d", -40.0),
               "Traditional junk captions (\xe8\xac\x9d\xe8\xac\x9d\xe8\xa7\x80\xe7\x9c\x8b, \xe8\xac\x9d\xe8\xac\x9d) are filtered when quiet, kept when loud");
            // The prompt's echo, offline: a quiet decode handing the prompt's tail back.
            TrContext ec;
            ec.rememberSrc("en", "Let us listen to it together one more time and decide.");
            const std::string pr = asrPromptFor(enZh, ec, "en");
            // (WB, after the review: a quotation of the prompt -- four words or more -- is junk at ANY
            // level now; only a SHORT repeat still depends on the quiet gate. [0quater] has the rest.)
            ok(looksLikeHallucination("Let us listen to it together one more time and decide.", -40.0, pr)
               && looksLikeHallucination("Let us listen to it together one more time and decide.", -12.0, pr)
               && looksLikeHallucination("one more time and decide", -40.0, pr)
               && !looksLikeHallucination("And decide.", -12.0, pr),
               "the prompt's own tail handed back is junk (quiet or loud); a short loud repeat is kept");
        }
        std::printf("    %s\n", failures == before ? "codes / votes / routing / prompts OK"
                                                   : "SEE FAILURES ABOVE");
    }
    simplifiedNetTests(failures);           // [0ter] (WB): the mirror net, offline
    reviewFixTests(failures);               // [0quater] (WB): the review's offline fixes
    seedSelftest(failures);                 // [0s] (SUVTITLE E8 + fix round): the first-start seed, bounded reads
    payloadSelftest(failures);              // [0p] (SUVTITLE fix round RV3-3): the bundle or the dev payload, never a mix

    // (SUVTITLE SPLIT: sections [1] to [6] -- the live path -- were here; see the note above runSelftest.)

    // The servers for [7] [8] [9]: what AudioPB's [2] had brought up for the live sections, minus the live
    // pair -- the file sections only ever load their own (bringFile).
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    // ---- 7. FILE MODE (wave WB) ------------------------------------------------------------
    fileModeSelftest(srv, cfg, failures, !offline);
    // ---- 8. FILE MODE v2 (wave WD): follow the playhead ---------------------------------------
    fileModeV2Selftest(srv, cfg, failures, !offline);
    // ---- 9. THE FILM'S OWN SOUND TRACKS (wave WH): which one a FILE analyses ------------------
    whSelftest(srv, cfg, failures, !offline);

    srv.killAll();
    if (failures) {
        std::printf("\nENGINE SELFTEST FAILED (%d assertion(s))\n", failures);
        return 1;
    }
    std::printf("\nENGINE SELFTEST PASSED\n");
    return 0;
}

// (SUVTITLE SPLIT: the SPEAKER SELFTEST, --selftest-spk -- the diarization stage on the 24-utterance
// synthetic meeting, embedding + clustering only -- was here. No diarization in SuVtitle.)

// ===========================================================================
// FILE MODE BY HAND (wave WB):  --selftest-file <media> [--pair a:b] [--mode m] [--quality q]
// The SHIPPING job (fileJobLap) on one file, with no pipe and no tray: every wire line it
// would send is printed exactly as packed, then '#' lines with the numbers (RTF, calls).
// The live pair is never started -- the job brings its own models, as in the helper.
// ===========================================================================
struct PrintSink final : FileSink {
    std::vector<std::string> lines;
    bool echo = true;
    void push(const std::string& m) override {
        lines.push_back(m);
        if (echo) std::printf("%s\n", m.c_str());
    }
    size_t backlog() const override { return 0; }
};

void printJobStats(const FileJobStats& st) {
    const double wall = (double)st.wallMs / 1000.0, load = (double)st.loadMs / 1000.0;
    const double work = wall - load;
    std::printf("# job %llu %s: %u cue(s); %.1f s of media in %.1f s (model load %.1f s) -> "
                "%.1fx real time without the load, %.1fx with it\n",
                (unsigned long long)st.jobId, st.result.c_str(), st.cues, st.audioSec, wall, load,
                work > 0.0 ? st.audioSec / work : 0.0, wall > 0.0 ? st.audioSec / wall : 0.0);
    std::printf("#   asr %s: %.1f s over %u window(s) (%u hard cut, %u re-decoded as degenerate); "
                "mt %s: %.1f s over %u call(s); %.0f s of silence skipped; %u junk/repeated "
                "segment(s) removed%s%s\n",
                st.asrModel.c_str(), (double)st.asrMs / 1000.0, st.windows, st.hardCuts,
                st.degenerate, st.mtModel.c_str(), (double)st.mtMs / 1000.0, st.mtCalls,
                st.skippedSec, st.junkSegments, st.err.empty() ? "" : "; err: ", st.err.c_str());
    std::printf("#   WF: dtw %s (%u of %u word window(s) timed%s); %u window(s) forced into the pair (%u then empty, "
                "%u kept the spoken language); %u kept as a third language; "
                "%u tag/credits + %u foreign-script segment(s); %u hard cut(s) held back; %u word(s) de-duplicated; "
                "%u fragment(s) merged\n",
                st.dtw ? "on" : "OFF", st.timedWindows, st.wordWindows, st.dtwDropped ? ", DTW DROPPED after a failure" : "",
                st.langRedecodes, st.langEmpty, st.langFallback, st.thirdLang, st.tagSegs,
                st.foreignSegs, st.heldBack, st.dedupWords, st.mergedFrags);
    if (!st.track.empty()) std::printf("#   WH: sound track %s\n", st.track.c_str());
}

int runSelftestFile(const std::string& media, const std::string& pair, const std::string& mode,
                    const std::string& quality, const std::string& audio) {
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    trpipe::WireFile f;
    f.jobId   = 1;
    f.path    = media;
    f.mode    = mode;
    f.quality = quality;
    f.audio   = audio;                            // WH: --audio <ord>:<trackId> ("" = the first sound track)
    if (!pair.empty()) {
        const size_t c = pair.find(':');
        f.langA = pair.substr(0, c);
        f.langB = c == std::string::npos ? std::string() : pair.substr(c + 1);
    }
    PrintSink sink;
    std::unique_ptr<FileJob> job;
    std::string err;
    if (!makeFileJob(f, cfg.langA, cfg.langB, cfg.mode, cfg, job, err)) {
        trpipe::WireFileDone d;
        d.jobId = 1; d.result = "error"; d.err = err;
        sink.push(trpipe::packFileDone(d));
        return 1;
    }
    std::atomic<uint64_t> abort{ 0 };
    FileJobStats st;
    while (job) fileJobLap(job, srv, sink, abort, /*verbose=*/true, &st);
    srv.killAll();
    printJobStats(st);
    return st.result == "ok" ? 0 : 1;
}

// --selftest-wf: section [7g] alone (wave WF's film-grade cues; the WF mutation runs' fast path, no server).
int runSelftestWf() {
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle Engine selftest: [7g] film-grade cues only ===\n");
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    int failures = 0;
    filmGradeTests(srv, cfg, failures);
    srv.killAll();
    std::printf(failures ? "\nWF SELFTEST FAILED (%d assertion(s))\n" : "\nWF SELFTEST PASSED\n", failures);
    return failures ? 1 : 0;
}

// --selftest-filemode [offline]: section [7] alone (the mutation runs' fast path). "offline" stops
// after [7a]/[7b] -- no server is started. (+ [8], and [WH-a]/[WH-b]: the film's own sound tracks.)
int runSelftestFileMode(bool offline) {
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle Engine selftest: FILE MODE only%s ===\n", offline ? " (offline)" : "");
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    int failures = 0;
    simplifiedNetTests(failures);
    reviewFixTests(failures);
    fileModeSelftest(srv, cfg, failures, !offline);
    fileModeV2Selftest(srv, cfg, failures, !offline);
    whSelftest(srv, cfg, failures, !offline);        // WH: the film's own sound tracks
    srv.killAll();
    if (failures) {
        std::printf("\nFILE MODE SELFTEST FAILED (%d assertion(s))\n", failures);
        return 1;
    }
    std::printf("\nFILE MODE SELFTEST PASSED\n");
    return 0;
}

// ===========================================================================
// THE FILM'S OWN SOUND TRACKS (wave WH, 2026-10-11):  --selftest-wh [offline]
// The user: "choisir ... l'audio deja inclus dans le fichier video". FILE's `audio` names the track the
// viewer hears (TrPipe.h); the reader finds it by the rule MEASURED in TrMedia.cpp ("WHICH SOUND TRACK").
//   [WH-a] offline, no model: the wire (parseAudioSel / packAudioSel / unpackFile); a malformed `audio`
//          through the SHIPPING door -- exactly ONE FDONE error per FILE, also while a job runs; the
//          READER's choice on the test media (<SUVTITLE_TEST_MEDIA>\wh\media, + the variants the
//          rule was measured on, wh\h\gen -- NOT CHECKED when absent): which stream, which container id,
//          which edit list, and WHICH SPEECH it decodes -- proved without a model, the hop-energy envelope
//          of the decode against the two recordings the media were made of (bench_en / bench_zh); "" ==
//          "0:<id>"; a track that cannot be found or decoded = one sentence naming it, also as a FILE job;
//          seeks -- and the re-open a backward seek makes -- keep the track; FPROXY copies ord 0.
//   [WH-b] the real servers (pass --cpu: the user's GPU may be busy): whole FILE jobs through the shipping
//          door + job lap -- the cue LANGUAGES follow the track; and the maintainer's film (SUVTITLE_TEST_FILM),
//          READ ONLY, on a 30 s stretch (`have` covers the rest): its English and its French tracks.
// ===========================================================================

// The WH test media's folder: <SUVTITLE_TEST_MEDIA>\wh\ (testMediaRoot, above); "" when there is no test media.
std::wstring whMediaRoot() {
    const std::wstring r = testMediaRoot();
    return r.empty() ? std::wstring() : r + L"wh\\";
}

bool whExists(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The FILE field -> the reader's AudioTrack (as makeFileJob makes it).
AudioTrack whTrack(const std::string& field) {
    AudioTrack t;
    trpipe::AudioSel s;
    if (trpipe::parseAudioSel(field, s)) { t.chosen = !field.empty(); t.ord = s.ord; t.trackId = s.trackId; }
    return t;
}

// One sound track's 16 kHz decode through the shipping reader (maxSec 0 = all of it).
bool whDecode(const std::wstring& path, const AudioTrack& tr, std::vector<float>& out, MediaInfo& info,
              std::string& err, double maxSec = 0.0) {
    MediaReader r;
    out.clear();
    if (!r.open(path, MediaReader::Out::Mono16k, tr, err)) return false;
    info = r.info();
    const size_t cap = maxSec > 0.0 ? (size_t)(maxSec * 16000.0) : (size_t)-1;
    int stall = 0;
    while (!r.eof() && out.size() < cap) {
        const size_t was = out.size();
        if (!r.readFloat(out, 16000 * 4, err)) return false;
        if (out.size() == was && ++stall > 1000) { err = "the reader stopped making progress"; return false; }
    }
    if (out.size() > cap) out.resize(cap);
    return true;
}

// WHICH SPEECH a decode carries, with no model: its hop-energy envelope (30 ms hops, dB, floored 60 dB under
// its own peak) ...
std::vector<double> whEnvelope(const std::vector<float>& x) {
    std::vector<double> e;
    const size_t hop = 480;
    for (size_t i = 0; i + hop <= x.size(); i += hop) {
        double s = 0.0;
        for (size_t k = 0; k < hop; ++k) s += (double)x[i + k] * x[i + k];
        e.push_back(10.0 * std::log10(s / (double)hop + 1e-12));
    }
    double pk = -1e9;
    for (double v : e) pk = std::max(pk, v);
    for (double& v : e) v = std::max(v, pk - 60.0);
    return e;
}

// ... against a source's: the best Pearson correlation over lags of +-300 ms (a container's lead, a codec's
// priming), over the first 12 s.
double whEnvCorr(const std::vector<double>& a, const std::vector<double>& b) {
    double best = -1.0;
    const size_t n0 = std::min<size_t>(400, std::min(a.size(), b.size()));
    for (int lag = -10; lag <= 10; ++lag) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        size_t n = 0;
        for (size_t i = 0; i < n0; ++i) {
            const long long j = (long long)i + lag;
            if (j < 0 || j >= (long long)b.size()) continue;
            const double x = a[i], y = b[(size_t)j];
            sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y; ++n;
        }
        if (n < 50) continue;
        const double cov = sab - sa * sb / (double)n, va = saa - sa * sa / (double)n, vb = sbb - sb * sb / (double)n;
        if (va <= 0.0 || vb <= 0.0) continue;
        best = std::max(best, cov / std::sqrt(va * vb));
    }
    return best;
}

// A FILE through the WIRE and the door (servantHandle), then the shipping request handler and job lap.
std::vector<std::string> whRunJob(Servers& srv, const TrConfig& cfg, const trpipe::WireFile& f, FileJobStats& st) {
    Shared sh;
    CollectSink sink;
    servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(f)));
    std::unique_ptr<FileJob> job;
    fileRequests(sh, job, srv, sink, cfg, &st);
    while (job) { fileJobLap(job, srv, sink, sh.abortJob, false, &st); fileRequests(sh, job, srv, sink, cfg, &st); }
    return sink.lines;
}

// The job's FDONE (count of them, the last one's result and sentence) and its other messages.
struct WhDone { int dones = 0, others = 0; std::string result, err; };
WhDone whDoneOf(const std::vector<std::string>& lines, uint64_t id) {
    WhDone w;
    for (const auto& m : lines) {
        const auto t = trpipe::split(m);
        trpipe::WireFileDone d;
        if (trpipe::unpackFileDone(t, d)) {
            if (d.jobId == id) { ++w.dones; w.result = d.result; w.err = d.err; }
        } else if (t.size() >= 2 && trpipe::jobIdOf(t[1]) == id) {
            ++w.others;
        }
    }
    return w;
}

void whSelftest(Servers& srv, const TrConfig& cfg, int& failures, bool live) {
    auto ok = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("    ASSERT FAIL: %s\n", what.c_str()); ++failures; }
    };
    const std::wstring W = whMediaRoot();               // <SUVTITLE_TEST_MEDIA>\wh\ ; "" without the test media
    const std::wstring M = W + L"media\\", G = W + L"h\\gen\\";
    const std::wstring F = testFilemodeDir();
    const std::string  Mu = wideToUtf8L(M), Fu = wideToUtf8L(F);
    const bool haveMedia = !W.empty() && whExists(M + L"mkv_2a_def2.mkv") && whExists(M + L"mp4_2a_movtext.mp4")
                        && whExists(M + L"mkv_dts_first.mkv") && whExists(M + L"webm_vtt.webm");
    const bool haveGen = !W.empty() && whExists(G + L"mp4_4a.mp4");
    uint64_t id = 9400;

    // ---------------------------------------------------------------- the wire
    std::printf("\n[WH-a] the wire: parseAudioSel / packAudioSel / unpackFile (offline)\n");
    {
        struct V { const char* s; bool good; uint32_t ord, tid; };
        const V vs[] = {
            { "", true, 0, 0 }, { "0:0", true, 0, 0 }, { "0:2", true, 0, 2 }, { "1:3", true, 1, 3 },
            { "007:010", true, 7, 10 }, { "123456789:987654321", true, 123456789u, 987654321u },
            { "x", false, 0, 0 }, { "1:", false, 0, 0 }, { ":2", false, 0, 0 }, { "1:2:3", false, 0, 0 },
            { "-1:2", false, 0, 0 }, { "1:-2", false, 0, 0 }, { "+1:2", false, 0, 0 }, { " 1:2", false, 0, 0 },
            { "1:2 ", false, 0, 0 }, { "1 :2", false, 0, 0 }, { ":", false, 0, 0 }, { "1", false, 0, 0 },
            { "a:b", false, 0, 0 }, { "1:2x", false, 0, 0 }, { "0x1:2", false, 0, 0 }, { "1;2", false, 0, 0 },
            { "1234567890:1", false, 0, 0 }, { "1:1234567890", false, 0, 0 },
        };
        int wrong = 0;
        for (const V& v : vs) {
            trpipe::AudioSel a;
            a.ord = 77; a.trackId = 77;
            const bool r = trpipe::parseAudioSel(v.s, a);
            if (r != v.good || (v.good && (a.ord != v.ord || a.trackId != v.tid))) {
                std::printf("    WRONG \"%s\": %s, ord %u id %u\n", v.s, r ? "parsed" : "refused", a.ord, a.trackId);
                ++wrong;
            }
        }
        std::printf("    %zu vectors (6 good, 18 malformed), %d wrong\n", sizeof(vs) / sizeof(vs[0]), wrong);
        ok(wrong == 0, "parseAudioSel: \"\" and <1-9 digits>:<1-9 digits> parse, every other form is refused");
        trpipe::AudioSel s, back;
        s.ord = 4; s.trackId = 12;
        ok(trpipe::packAudioSel(s) == "4:12" && trpipe::parseAudioSel(trpipe::packAudioSel(s), back)
           && back.ord == 4 && back.trackId == 12, "packAudioSel -> parseAudioSel round trip");
        trpipe::WireFile f, g;
        f.jobId = 5; f.path = "C:\\x y\\film.mkv"; f.langA = "en"; f.langB = "zh"; f.audio = "1:3";
        const bool u1 = trpipe::unpackFile(trpipe::split(trpipe::packFile(f)), g) && g.audio == "1:3";
        f.audio.clear();
        const std::string line0 = trpipe::packFile(f);
        const bool u2 = trpipe::unpackFile(trpipe::split(line0), g) && g.audio.empty() && trpipe::split(line0).size() == 9;
        f.audio = "1:2:3";
        const bool u3 = !trpipe::unpackFile(trpipe::split(trpipe::packFile(f)), g);
        ok(u1 && u2 && u3, "FILE's 10th field: carried; absent for \"\" (the pre-WH line); a malformed one refuses the FILE");
    }

    // ---------------------------------------------------------------- the door
    std::printf("\n[WH-a] a malformed `audio`: exactly ONE FDONE error per FILE (the shipping door, offline)\n");
    if (!haveMedia) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH test media\n");
    } else {
        const char* bads[] = { "x", "1:", ":2", "1:2:3", "-1:2", "1:2 ", "1234567890:1" };
        int good = 0;
        for (const char* b : bads) {
            trpipe::WireFile f;
            f.jobId = ++id; f.path = Mu + "mkv_2a_def2.mkv"; f.langA = "en"; f.langB = "zh"; f.audio = b;
            FileJobStats st;
            const WhDone w = whDoneOf(whRunJob(srv, cfg, f, st), f.jobId);
            const bool g1 = w.dones == 1 && w.result == "error" && w.err.find("sound track") != std::string::npos
                         && w.others == 0 && st.track.empty();
            if (g1) ++good;
            std::printf("    \"%s\": FDONE %s x%d (\"%s\")%s\n", b, w.result.c_str(), w.dones, w.err.c_str(),
                        w.others ? ", + OTHER MESSAGES" : "");
        }
        ok(good == (int)(sizeof(bads) / sizeof(bads[0])),
           "every malformed `audio`: ONE FDONE error naming the sound track, nothing else, the file never opened");
        // ... and while a job runs: it stops (cancelled), the malformed FILE gets its ONE error (TrPipe.h)
        {
            Shared sh;
            CollectSink sink;
            std::unique_ptr<FileJob> job;
            FileJobStats st;
            g_fileNoModelsForTest = true;
            trpipe::WireFile f;
            f.jobId = ++id; f.path = Mu + "mkv_2a_def2.mkv"; f.langA = "en"; f.langB = "zh"; f.audio = "1:3";
            const uint64_t running = f.jobId;
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(f)));
            fileRequests(sh, job, srv, sink, cfg, &st);
            for (int k = 0; k < 2 && job; ++k) fileJobLap(job, srv, sink, sh.abortJob, false, &st);   // Open, Load
            const bool wasRunning = job != nullptr;
            f.jobId = ++id; f.audio = "1:";
            servantHandle(sh, nullptr, trpipe::split(trpipe::packFile(f)));
            fileRequests(sh, job, srv, sink, cfg, &st);
            g_fileNoModelsForTest = false;
            const WhDone a = whDoneOf(sink.lines, running), b = whDoneOf(sink.lines, f.jobId);
            std::printf("    a running job (\"1:3\"), then FILE \"1:\": %llu %s x%d, %llu %s x%d (\"%s\")\n",
                        (unsigned long long)running, a.result.c_str(), a.dones, (unsigned long long)f.jobId,
                        b.result.c_str(), b.dones, b.err.c_str());
            ok(wasRunning && a.dones == 1 && a.result == "cancelled" && b.dones == 1 && b.result == "error"
               && b.others == 0 && !job,
               "a malformed `audio` while a job runs: that job is cancelled, the FILE gets exactly ONE FDONE error");
        }
    }

    // ---------------------------------------------------------------- the reader's choice
    std::printf("\n[WH-a] the reader's choice: which stream, which container id, WHICH SPEECH (offline)\n");
    std::vector<double> envEn, envZh;
    {
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const std::wstring R = testMediaRoot();
        if (!R.empty() && whDecode(R + L"bench_en.wav", AudioTrack(), x, mi, err, 12.0)) envEn = whEnvelope(x);
        if (!R.empty() && whDecode(R + L"bench_zh.wav", AudioTrack(), x, mi, err, 12.0)) envZh = whEnvelope(x);
    }
    if (!haveMedia || envEn.empty() || envZh.empty()) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH test media (<SUVTITLE_TEST_MEDIA>\\wh\\media) or no "
                    "bench_en.wav / bench_zh.wav in <SUVTITLE_TEST_MEDIA>\n");
    } else {
        const int before = failures;
        // file, in the variants' folder?, the FILE field, the speech expected ("en" / "zh"; "" = no identity
        // check), the error expected (a substring; nullptr = it opens), then -- when it opens -- the container's
        // ord / id, the Source Reader index (-1 = any) and how it was found. The indexes are MEASURED (WH_PROGRESS
        // WH-H, M1/M2): an MP4 lists its sound traks BACKWARDS.
        struct C { const wchar_t* f; int where; const char* audio; const char* speech; const char* err;
                   int ord; uint32_t tid; int index; const char* how; };
        const C cases[] = {
            // MATROSKA: identifier = TrackNumber; subtitle tracks are not listed
            { L"mkv_2a_def2.mkv", 0, "",    "en", nullptr, 0, 2, 1, "mkv-first" },
            { L"mkv_2a_def2.mkv", 0, "0:2", "en", nullptr, 0, 2, 1, "mkv-id" },
            { L"mkv_2a_def2.mkv", 0, "1:3", "zh", nullptr, 1, 3, 2, "mkv-id" },
            { L"mkv_2a_def2.mkv", 0, "1:0", "zh", nullptr, 1, 3, 2, "mkv-ord" },
            { L"mkv_2a_def2.mkv", 0, "0:3", "zh", nullptr, 1, 3, 2, "mkv-id" },     // the id wins over the place
            { L"mkv_2a_def2.mkv", 0, "2:4", "",   "not in this file", 0, 0, -1, nullptr },   // #4 = its SRT track
            { L"mkv_2a_def2.mkv", 0, "5:0", "",   "not in this file", 0, 0, -1, nullptr },
            { L"mkv_dts_first.mkv", 0, "",    "", "This sound track (DTS) can't be decoded", 0, 0, -1, nullptr },
            { L"mkv_dts_first.mkv", 0, "0:2", "", "This sound track (DTS) can't be decoded", 0, 0, -1, nullptr },
            { L"mkv_dts_first.mkv", 0, "1:3", "zh", nullptr, 1, 3, 2, "mkv-id" },
            { L"webm_vtt.webm", 0, "",    "",   nullptr, 0, 2, 1, "mkv-first" },
            { L"webm_vtt.webm", 0, "0:3", "",   "not in this file", 0, 0, -1, nullptr },  // #3 = its WebVTT track
            // ISO-BMFF: the trak by OUR moov table, its stream by the stsd it carries
            { L"mp4_2a_movtext.mp4", 0, "",    "en", nullptr, 0, 2, 1, "iso-stsd" },
            { L"mp4_2a_movtext.mp4", 0, "0:2", "en", nullptr, 0, 2, 1, "iso-stsd" },
            { L"mp4_2a_movtext.mp4", 0, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_2a_movtext.mp4", 0, "1:0", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_2a_movtext.mp4", 0, "0:4", "",   "not in this file", 0, 0, -1, nullptr },   // track_ID 4 = mov_text
            { L"mp4_2a_movtext.mp4", 0, "2:0", "",   "not in this file", 0, 0, -1, nullptr },
            // the variants the rule was measured on
            { L"mp4_4a.mp4", 1, "",    "en", nullptr, 0, 2, 3, "iso-stsd" },     // Media Foundation's own pick: index 0
            { L"mp4_4a.mp4", 1, "1:3", "zh", nullptr, 1, 3, 2, "iso-stsd" },
            { L"mp4_4a.mp4", 1, "2:4", "en", nullptr, 2, 4, 1, "iso-stsd" },     // English speech, 22.05 kHz, "fre"
            { L"mp4_4a.mp4", 1, "3:5", "zh", nullptr, 3, 5, 0, "iso-stsd" },     // Chinese speech, 24 kHz stereo, "ger"
            { L"mp4_ids.mp4", 1, "0:7", "en", nullptr, 0, 7, 1, "iso-stsd" },    // track_IDs 10 / 7 / 3
            { L"mp4_ids.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_a_first.mp4", 1, "",    "en", nullptr, 0, 1, 1, "iso-stsd" },
            { L"mp4_a_first.mp4", 1, "1:2", "zh", nullptr, 1, 2, 0, "iso-stsd" },
            { L"mov_pcm_2a.mov", 1, "",    "en", nullptr, 0, 2, 1, "iso-order" }, // two IDENTICAL stsd boxes
            { L"mov_pcm_2a.mov", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-order" },
            { L"mp4_ac3_aac.mp4", 1, "",    "en", nullptr, 0, 2, 1, "iso-stsd" }, // MF's own pick: the AAC zh, index 0
            { L"mp4_ac3_aac.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_dts_first.mp4", 1, "",    "", "This sound track (DTS) can't be decoded", 0, 0, -1, nullptr },
            { L"mp4_dts_first.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_truehd_aac.mp4", 1, "0:2", "", "This sound track (Dolby TrueHD) can't be decoded", 0, 0, -1, nullptr },
            { L"mp4_truehd_aac.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mp4_aac_opus.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },   // Opus in an MP4
            // the 'alac' trak Media Foundation does not LIST: "" is that trak or this error -- never the AAC
            // under ord 0's name (FPROXY, below, still gets a sound)
            { L"mp4_alac_aac.mp4", 1, "",    "", "This sound track (ALAC) can't be decoded", 0, 0, -1, nullptr },
            { L"mp4_alac_aac.mp4", 1, "0:2", "", "This sound track (ALAC) can't be decoded", 0, 0, -1, nullptr },
            { L"mp4_alac_aac.mp4", 1, "1:3", "zh", nullptr, 1, 3, 0, "iso-stsd" },
            { L"mkv_zh_first.mkv", 1, "",    "zh", nullptr, 0, 2, 1, "mkv-first" },
            { L"mkv_zh_first.mkv", 1, "1:3", "en", nullptr, 1, 3, 2, "mkv-id" },
            { L"mkv_ac3_aac.mkv", 1, "0:2", "en", nullptr, 0, 2, 1, "mkv-id" },   // AC-3 decodes here
            { L"mkv_tta_wv.mkv", 1, "0:2", "en", nullptr, 0, 2, 1, "mkv-id" },
            { L"mkv_tta_wv.mkv", 1, "1:3", "",   "This sound track (TTA) can't be decoded", 0, 0, -1, nullptr },
            { L"mkv_tta_wv.mkv", 1, "2:4", "",   "This sound track (WavPack) can't be decoded", 0, 0, -1, nullptr },
            { L"mkv_truehd_aac.mkv", 1, "0:2", "", "This sound track (Dolby TrueHD) can't be decoded", 0, 0, -1, nullptr },
            { L"mkv_truehd_aac.mkv", 1, "1:3", "zh", nullptr, 1, 3, 2, "mkv-id" },
            // every other container: the first track as before WH, a later one refused
            { L"c.mp3", 2, "",    "", nullptr, 0, 0, -1, "first" },
            { L"c.mp3", 2, "0:0", "", nullptr, 0, 0, -1, "first" },
            { L"c.mp3", 2, "1:0", "", "Only the first sound track", 0, 0, -1, nullptr },
            { L"s16.aiff", 2, "",  "", nullptr, 0, 0, -1, "aiff" },
            { L"s16.aiff", 2, "1:0", "", "Only the first sound track", 0, 0, -1, nullptr },
            { L"v.mkv", 2, "",    "", nullptr, 0, 0, -1, "mkv-first" },
            { L"v.mp4", 2, "",    "", nullptr, 0, 0, -1, "iso-stsd" },
        };
        int run = 0, skipped = 0, identity = 0;
        for (const C& c : cases) {
            const std::wstring dir = c.where == 0 ? M : (c.where == 1 ? G : F);
            const std::wstring path = dir + c.f;
            const std::string name = wideToUtf8L(c.f) + " \"" + c.audio + "\"";
            if (!whExists(path)) { ++skipped; continue; }
            ++run;
            std::vector<float> x;
            MediaInfo mi;
            std::string err;
            const bool opened = whDecode(path, whTrack(c.audio), x, mi, err, 12.0);
            if (c.err) {
                std::printf("    %-34s -> \"%s\"\n", name.c_str(), err.c_str());
                ok(!opened && err.find(c.err) != std::string::npos, name + ": refused with \"" + c.err + "...\"");
                continue;
            }
            if (!opened) { ok(false, name + " opens (" + err + ")"); continue; }
            const std::vector<double> e = whEnvelope(x);
            const double ce = whEnvCorr(e, envEn), cz = whEnvCorr(e, envZh);
            std::printf("    %-34s -> stream %u/%u ord %d #%u by %-9s %-21s %5u Hz %u ch lang \"%s\" trim %.4f | en %.2f zh %.2f\n",
                        name.c_str(), mi.streamIndex, mi.audioStreams, mi.trackOrd, mi.trackId, mi.trackHow.c_str(),
                        mi.codec.c_str(), mi.nativeRate, mi.nativeChannels, mi.trackLang.c_str(), mi.trimSec, ce, cz);
            if (c.how) ok(mi.trackHow == c.how, name + ": found by " + c.how + " (got " + mi.trackHow + ")");
            if (c.where != 2) {
                ok(mi.trackOrd == c.ord && mi.trackId == c.tid,
                   name + ": ord " + std::to_string(c.ord) + " #" + std::to_string(c.tid) + " (got ord "
                   + std::to_string(mi.trackOrd) + " #" + std::to_string(mi.trackId) + ")");
                if (c.index >= 0)
                    ok(mi.streamIndex == (uint32_t)c.index, name + ": Source Reader stream " + std::to_string(c.index)
                       + " (got " + std::to_string(mi.streamIndex) + ")");
            }
            if (c.speech[0]) {
                ++identity;
                const bool en = !std::strcmp(c.speech, "en");
                const double want = en ? ce : cz, other = en ? cz : ce;
                ok(want >= 0.70 && want - other >= 0.25,
                   name + ": decodes the " + (en ? "ENGLISH" : "CHINESE") + " speech (envelope r " + std::to_string(want)
                   + " vs " + std::to_string(other) + ")");
            }
        }
        std::printf("    %d case(s) run, %d identity-checked, %d NOT CHECKED (file absent)\n", run, identity, skipped);
        // "" == "0:<id>": the very same stream, and the sound track's own edit list (the 22.05 / 24 kHz traks of
        // mp4_4a have their own priming: 1024 samples at THEIR rate -- pre-WH, the first trak's applied to all)
        {
            MediaReader a, b;
            std::string e1, e2;
            const bool o = a.open(M + L"mkv_2a_def2.mkv", MediaReader::Out::Mono16k, whTrack(""), e1)
                        && b.open(M + L"mkv_2a_def2.mkv", MediaReader::Out::Mono16k, whTrack("0:2"), e2);
            MediaReader c2, d2;
            const bool o2 = c2.open(M + L"mp4_2a_movtext.mp4", MediaReader::Out::Mono16k, whTrack(""), e1)
                         && d2.open(M + L"mp4_2a_movtext.mp4", MediaReader::Out::Mono16k, whTrack("0:2"), e2);
            ok(o && o2 && a.info().streamIndex == b.info().streamIndex && c2.info().streamIndex == d2.info().streamIndex,
               "\"\" and \"0:<id>\" decode the very same stream (mkv and mp4)");
        }
        if (haveGen) {
            double t[4] = { -1, -1, -1, -1 };
            const char* sel[4] = { "0:2", "1:3", "2:4", "3:5" };
            for (int k = 0; k < 4; ++k) {
                MediaReader r;
                std::string e;
                if (r.open(G + L"mp4_4a.mp4", MediaReader::Out::Mono16k, whTrack(sel[k]), e)) t[k] = r.info().trimSec;
            }
            std::printf("    mp4_4a edit lists applied: %.5f %.5f %.5f %.5f s (1024 samples at 16 / 16 / 22.05 / 24 kHz)\n",
                        t[0], t[1], t[2], t[3]);
            ok(std::fabs(t[0] - 1024.0 / 16000.0) < 1e-6 && std::fabs(t[1] - 1024.0 / 16000.0) < 1e-6
               && std::fabs(t[2] - 1024.0 / 22050.0) < 1e-6 && std::fabs(t[3] - 1024.0 / 24000.0) < 1e-6,
               "each sound track gets ITS OWN edit list (the decoded trak's priming, at its own rate)");
        } else {
            std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): the variants (<SUVTITLE_TEST_MEDIA>\\wh\\h\\gen) -- per-trak "
                        "edit lists, reverse order, iso-order\n");
        }
        std::printf("    %s\n", failures == before ? "the reader's choice OK" : "SEE FAILURES ABOVE");
    }

    // ---------------------------------------------------------------- jobs refused at Open (no model)
    std::printf("\n[WH-a] FILE jobs whose sound track cannot be had: ONE FDONE error, nothing loaded (offline)\n");
    if (!haveMedia) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH test media\n");
    } else {
        struct J { std::string path; const char* audio; const char* err; };
        const J js[] = {
            { Mu + "mkv_dts_first.mkv", "0:2", "This sound track (DTS) can't be decoded for the analysis" },
            { Mu + "mkv_dts_first.mkv", "",    "This sound track (DTS) can't be decoded for the analysis" },
            { Mu + "mkv_2a_def2.mkv",   "7:9", "is not in this file" },
            { Mu + "mkv_2a_def2.mkv",   "2:4", "is not in this file" },
            { Fu + "c.mp3",             "1:0", "Only the first sound track" },
            { wideToUtf8L(G) + "mp4_alac_aac.mp4", "", "This sound track (ALAC) can't be decoded" },
        };
        int good = 0, run = 0;
        // OFFLINE MEANS NO MODEL, even under a broken build: a job that wrongly gets past Open then fails fast on
        // a whisper that is not there, instead of loading turbo on the CPU (WH's mutation run: 100-300 s a mutant)
        g_fileNoModelsForTest = true;
        for (const J& j : js) {
            if (!whExists(utf8ToWideL(j.path))) continue;
            ++run;
            trpipe::WireFile f;
            f.jobId = ++id; f.path = j.path; f.langA = "en"; f.langB = "zh"; f.audio = j.audio;
            FileJobStats st;
            const std::vector<std::string> lines = whRunJob(srv, cfg, f, st);
            const WhDone w = whDoneOf(lines, f.jobId);
            const bool g1 = w.dones == 1 && w.result == "error" && w.err.find(j.err) != std::string::npos
                         && w.others == 0 && st.loads == 0;
            if (g1) ++good;
            std::printf("    %s \"%s\": FDONE %s x%d (\"%s\"), %d other message(s), %u model load(s)\n",
                        j.path.substr(j.path.find_last_of("\\") + 1).c_str(), j.audio, w.result.c_str(), w.dones,
                        w.err.c_str(), w.others, st.loads);
        }
        g_fileNoModelsForTest = false;
        ok(good == run && run >= 4, "a track that cannot be found or decoded: the job's ONE FDONE error names it; "
                                    "no FPROG, no model loaded");
    }

    // ---------------------------------------------------------------- seeks keep the track
    std::printf("\n[WH-a] seeks -- and the re-open a backward seek makes -- keep the sound track (offline)\n");
    if (!haveMedia) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH test media\n");
    } else {
        // (the other track carries the OTHER speech: mp4_4a's "0:2" is the same English as "2:4", re-encoded)
        struct S { std::wstring path; const char* audio; const char* other; };
        std::vector<S> ss = { { M + L"mkv_2a_def2.mkv", "1:3", "0:2" }, { M + L"mp4_2a_movtext.mp4", "1:3", "0:2" },
                              { M + L"mkv_2a_def2.mkv", "0:2", "1:3" } };
        if (haveGen) {
            ss.push_back({ G + L"mp4_4a.mp4", "2:4", "1:3" });          // 22.05 kHz: our resampler restarted
            ss.push_back({ G + L"mp4_4a.mp4", "3:5", "0:2" });          // 24 kHz stereo, the LAST trak (MF index 0)
            ss.push_back({ G + L"mov_pcm_2a.mov", "1:3", "0:2" });      // found by the reverse order
        }
        for (const S& s : ss) {
            const std::string name = wideToUtf8L(s.path.substr(s.path.find_last_of(L"\\") + 1)) + " \"" + s.audio + "\"";
            std::vector<float> lin, oth;
            MediaInfo mi, mo;
            std::string err;
            if (!whDecode(s.path, whTrack(s.audio), lin, mi, err) || !whDecode(s.path, whTrack(s.other), oth, mo, err)) {
                ok(false, name + ": linear decodes (" + err + ")");
                continue;
            }
            MediaReader r;
            if (!r.open(s.path, MediaReader::Out::Mono16k, whTrack(s.audio), err)) { ok(false, name + " opens"); continue; }
            auto at = [&](double sec, double len, std::string& how, double& dSame, double& dOther) {
                std::string e;
                dSame = dOther = 1e9;
                if (!r.seek((uint64_t)(sec * 16000.0), e)) return false;
                how = r.lastSeekHow();
                std::vector<float> y;
                for (int guard = 0; y.size() < (size_t)(len * 16000.0) && !r.eof() && guard < 10000; ++guard)
                    if (!r.readFloat(y, (size_t)(len * 16000.0) - y.size(), e)) return false;
                const size_t from = (size_t)(sec * 16000.0);
                dSame = dOther = 0.0;
                for (size_t k = 0; k < y.size(); ++k) {
                    if (from + k < lin.size()) dSame  = std::max(dSame,  (double)std::fabs(y[k] - lin[from + k]));
                    if (from + k < oth.size()) dOther = std::max(dOther, (double)std::fabs(y[k] - oth[from + k]));
                }
                return !y.empty();
            };
            // 9 s: decoded forward; 4 s: BACKWARDS past the start's 2 s -- the container's own seek, on the chosen
            // stream; 1 s: backwards near the start -- a RE-OPEN, which must find the same track; 6 s: forward again
            std::string h1, h2, h3, h4;
            double s1 = 0, o1 = 0, s2 = 0, o2 = 0, s3 = 0, o3 = 0, s4 = 0, o4 = 0;
            const bool a1 = at(9.0, 1.5, h1, s1, o1);
            const bool a2 = at(4.0, 2.0, h2, s2, o2);
            const bool a3 = at(1.0, 1.0, h3, s3, o3);
            const bool a4 = at(6.0, 2.0, h4, s4, o4);
            std::printf("    %-24s 9 s (%s) max|diff| %.1e vs the other %.2f; 4 s (%s) %.1e vs %.2f; 1 s (%s) %.1e vs %.2f; "
                        "6 s (%s) %.1e vs %.2f\n", name.c_str(), h1.c_str(), s1, o1, h2.c_str(), s2, o2, h3.c_str(), s3, o3,
                        h4.c_str(), s4, o4);
            // (decoded seeks are exact; the container's leaves an AAC decoder restarted mid-stream: up to 2.2e-3
            // measured here -- [8]'s own bound for a container seek is 6e-3 -- against 0.75-1.10 for the other track)
            ok(a1 && a2 && a3 && a4 && s1 <= 1e-6 && s2 <= 6e-3 && s3 <= 1e-6 && s4 <= 1e-6
               && o1 > 0.05 && o2 > 0.05 && o3 > 0.05 && o4 > 0.05,
               name + ": after every kind of seek the samples are the CHOSEN track's (== its linear decode), never "
                      "the other one's");
            ok(h3 == "start" || h3 == "reopen", name + ": the seek back to 1 s re-opened the file (got " + h3 + ")");
            ok(h2 == "mf", name + ": the seek back to 4 s is the container's own seek, on the chosen stream (got " + h2 + ")");
        }
    }

    // ---------------------------------------------------------------- FPROXY: the first track, ord 0
    std::printf("\n[WH-a] FPROXY copies the first sound track -- ord 0, the one \"\" analyses (offline)\n");
    if (!haveGen || envEn.empty()) {
        std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH variants\n");
    } else {
        wchar_t tmpDir[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tmpDir);
        const std::wstring out = std::wstring(tmpDir) + L"SuVtitle_selftest_wh_proxy.wav";
        std::atomic<bool> cancel{ false };
        std::string err;
        ProxyFacts pf;
        const ProxyResult pr = writeProxyWav(G + L"mp4_ac3_aac.mp4", out, cancel, err, &pf);
        std::vector<float> x;
        MediaInfo mi;
        double ce = -1, cz = -1;
        if (pr == ProxyResult::Ok && whDecode(out, AudioTrack(), x, mi, err, 12.0)) {
            const std::vector<double> e = whEnvelope(x);
            ce = whEnvCorr(e, envEn);
            cz = whEnvCorr(e, envZh);
        }
        for (int t = 0; t < 5 && !::DeleteFileW(out.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND; ++t) ::Sleep(20);
        std::printf("    mp4_ac3_aac.mp4 -> proxy %u Hz %u ch, %llu frames; envelope en %.2f zh %.2f (%s)\n", pf.rate,
                    pf.channels, (unsigned long long)pf.frames, ce, cz, err.c_str());
        ok(pr == ProxyResult::Ok && pf.rate == 32000 && ce >= 0.70 && ce - cz >= 0.25,
           "FPROXY of a two-track MP4: the FIRST sound trak (AC-3 English, 32 kHz) -- not Media Foundation's own pick "
           "(the AAC Chinese one)");
        // ... and where Windows cannot read the first trak at all (an MP4's 'alac', not even listed), the copy
        // still plays what Windows CAN read -- the pre-WH fallback (the FILE job refuses instead, above)
        if (whExists(G + L"mp4_alac_aac.mp4")) {
            ProxyFacts pf2;
            std::string e2;
            const ProxyResult pr2 = writeProxyWav(G + L"mp4_alac_aac.mp4", out, cancel, e2, &pf2);
            double ce2 = -1, cz2 = -1;
            if (pr2 == ProxyResult::Ok && whDecode(out, AudioTrack(), x, mi, e2, 12.0)) {
                const std::vector<double> e = whEnvelope(x);
                ce2 = whEnvCorr(e, envEn);
                cz2 = whEnvCorr(e, envZh);
            }
            for (int t = 0; t < 5 && !::DeleteFileW(out.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND; ++t) ::Sleep(20);
            std::printf("    mp4_alac_aac.mp4 (its first trak, ALAC, unlisted) -> proxy %u Hz, envelope en %.2f zh %.2f (%s)\n",
                        pf2.rate, ce2, cz2, e2.c_str());
            ok(pr2 == ProxyResult::Ok && cz2 >= 0.70 && cz2 - ce2 >= 0.25,
               "FPROXY when Windows cannot read the first trak: the one it can (the AAC), as before WH -- a sound to hear");
        }
    }

    // ---------------------------------------------------------------- [WH-b] live
    std::printf("\n[WH-b] whole FILE jobs on the real servers: the cue LANGUAGES follow the sound track\n");
    if (!live) { std::printf("    NOT RUN: offline pass (--selftest-wh offline)\n"); return; }
    if (!haveMedia) { std::printf("    NOT CHECKED (set SUVTITLE_TEST_MEDIA): no WH test media\n"); return; }
    const std::string keepAsr = g_fileAsrModelForTest, keepMt = g_fileMtModelForTest;
    if (g_forceCpu && g_fileAsrModelForTest.empty()) g_fileAsrModelForTest = "ggml-base.bin";
    if (g_forceCpu && g_fileMtModelForTest.empty())  g_fileMtModelForTest  = kMtModelFileSmall;
    std::printf("    %s: asr %s, mt %s\n", g_forceCpu ? "CPU FORCED" : "GPU (pass --cpu when the GPU is not yours)",
                g_fileAsrModelForTest.empty() ? "(shipping choice)" : g_fileAsrModelForTest.c_str(),
                g_fileMtModelForTest.empty() ? "(shipping choice)" : g_fileMtModelForTest.c_str());
    {
        const int before = failures;
        struct L { std::string path; const char* audio; const char* a; const char* b; const char* want; const char* err;
                   std::string have; };
        std::vector<L> ls = {
            { Mu + "mkv_2a_def2.mkv",    "",    "en", "zh", "en", nullptr, "" },
            { Mu + "mkv_2a_def2.mkv",    "0:2", "en", "zh", "en", nullptr, "" },
            { Mu + "mkv_2a_def2.mkv",    "1:3", "en", "zh", "zh", nullptr, "" },
            { Mu + "mp4_2a_movtext.mp4", "",    "en", "zh", "en", nullptr, "" },
            { Mu + "mp4_2a_movtext.mp4", "1:3", "en", "zh", "zh", nullptr, "" },
            { Mu + "mkv_dts_first.mkv",  "0:2", "en", "zh", "",   "This sound track (DTS) can't be decoded", "" },
            { Mu + "mkv_dts_first.mkv",  "1:3", "en", "zh", "zh", nullptr, "" },
        };
        // THE MAINTAINER'S FILM (SUVTITLE_TEST_FILM), READ ONLY: 30 s of dense dialogue (74:56 -- the densest stretch of its
        // subtitles), `have` covering the rest, so a CPU job hears 30 s: its English track (#2) vs its French one (#4). It
        // is a multi-audio feature film of the maintainer's, NOT in the repository: any other file must have the same
        // shape (English = container id 2, French = id 4, dialogue at 74:56, longer than 76 min) or be left out.
        const std::wstring film = testFilmPath();
        if (!film.empty()) {
            MediaReader r;
            std::string e;
            if (r.open(film, MediaReader::Out::Mono16k, e) && r.info().durationMs > 4600000) {
                const std::string have = "0-4496000,4526000-" + std::to_string(r.info().durationMs);
                ls.push_back({ wideToUtf8L(film), "0:2", "en", "fr", "en", nullptr, have });
                ls.push_back({ wideToUtf8L(film), "2:4", "en", "fr", "fr", nullptr, have });
            } else {
                std::printf("    NOT CHECKED: SUVTITLE_TEST_FILM does not open, or is shorter than 76 min\n");
            }
        } else {
            std::printf("    NOT CHECKED (set SUVTITLE_TEST_FILM): the maintainer's film\n");
        }
        for (const L& l : ls) {
            const std::string fname = l.path.substr(l.path.find_last_of("\\") + 1);
            const std::string label = (fname.size() > 40 ? fname.substr(0, 37) + "..." : fname) + " \"" + l.audio + "\"";
            std::printf("\n    -- %s, pair %s<->%s%s\n", label.c_str(), l.a, l.b, l.have.empty() ? "" : ", 30 s (have = the rest)");
            trpipe::WireFile f;
            f.jobId = ++id; f.path = l.path; f.langA = l.a; f.langB = l.b; f.mode = "auto"; f.quality = "auto";
            f.audio = l.audio; f.have = l.have;
            FileJobStats st;
            const std::vector<std::string> lines = whRunJob(srv, cfg, f, st);
            std::vector<trpipe::MsRange> hv;
            trpipe::parseRanges(l.have, hv);
            const WhDone w = whDoneOf(lines, f.jobId);
            if (l.err) {
                failures += checkV2Wire(lines, f.jobId, label.c_str(), hv, false, 0);
                std::printf("       FDONE %s (\"%s\"), %u model load(s)\n", w.result.c_str(), w.err.c_str(), st.loads);
                ok(w.dones == 1 && w.result == "error" && w.err.find(l.err) != std::string::npos && st.loads == 0,
                   label + ": ONE FDONE error naming the format, no model loaded");
                continue;
            }
            failures += checkV2Wire(lines, f.jobId, label.c_str(), hv, true, st.totalMs);
            const auto cs = cuesOfRun(lines);
            int want = 0;
            std::string langs;
            for (const auto& c : cs) {
                if (c.lang == l.want) ++want;
                if (langs.find(c.lang) == std::string::npos) langs += (langs.empty() ? "" : ",") + c.lang;
                std::printf("       %7llu-%7llu %s->%s  %s  |  %s\n", (unsigned long long)c.startMs,
                            (unsigned long long)c.endMs, c.lang.c_str(), c.dstLang.c_str(), c.src.c_str(), c.dst.c_str());
            }
            printJobStats(st);
            std::printf("       %zu cue(s), %d in %s (languages: %s); track %s\n", cs.size(), want, l.want, langs.c_str(),
                        st.track.c_str());
            ok(!cs.empty() && want * 5 >= (int)cs.size() * 4,
               label + ": the cues are in " + l.want + " (>= 80 %) -- the speech of THAT sound track");
        }
        std::printf("    %s\n", failures == before ? "the cue languages follow the sound track" : "SEE FAILURES ABOVE");
    }
    g_fileAsrModelForTest = keepAsr;
    g_fileMtModelForTest  = keepMt;
}

// A crash of a TEST run must never sit on the user's screen as a modal box -- the WH mutation run met one (a
// Debug "vector subscript out of range" assertion of a mutant, 11/10): CRT asserts and errors go to stderr, abort()
// writes nothing and calls no fault reporter, and Windows shows no fault dialog. The run simply fails.
void quietCrashes() {
#ifdef _DEBUG                // (in a Release build these four are no-op macros)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::SetErrorMode(::GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int runSelftestWh(bool offline) {
    quietCrashes();
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle Engine selftest: the film's own sound tracks (WH)%s%s ===\n",
                offline ? " (offline)" : "", g_forceCpu ? " (CPU forced)" : "");
    TrConfig cfg;
    cfg.load();                                   // read only: nothing here writes it
    Servers srv;
    srv.setPaths(resolvePaths(cfg));
    int failures = 0;
    const ULONGLONG t0 = ::GetTickCount64();
    whSelftest(srv, cfg, failures, !offline);
    srv.killAll();
    std::printf("\n(%.1f s)\n", (double)(::GetTickCount64() - t0) / 1000.0);
    std::printf(failures ? "\nWH SELFTEST FAILED (%d assertion(s))\n" : "\nWH SELFTEST PASSED\n", failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string pipePath, presetArg, modeArg, fileArg, pairArg, qualityArg, audioArg;
    uint32_t parentPid = 0;
    bool selftest = false, stOffline = false, nofabric = false, selftestFm = false, fmOffline = false;
    bool forceCpu = false, selftestV2 = false, v2Offline = false, selftestWf = false;
    bool selftestWh = false, whOffline = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        // (SUVTITLE SPLIT: --selftest-spk, the diarization proof, is not ported; an unknown switch is ignored.)
        if      (a == "--selftest-wf")  selftestWf = true;
        else if (a == "--selftest") {
            selftest = true;
            if (i + 1 < argc && std::string(argv[i + 1]) == "offline") { stOffline = true; ++i; }
        }
        else if (a == "--selftest-filemode") {
            selftestFm = true;
            if (i + 1 < argc && std::string(argv[i + 1]) == "offline") { fmOffline = true; ++i; }
        }
        else if (a == "--selftest-v2") {
            selftestV2 = true;
            if (i + 1 < argc && std::string(argv[i + 1]) == "offline") { v2Offline = true; ++i; }
        }
        else if (a == "--selftest-wh") {
            selftestWh = true;
            if (i + 1 < argc && std::string(argv[i + 1]) == "offline") { whOffline = true; ++i; }
        }
        else if (a == "--selftest-file") fileArg = next();
        else if (a == "--pair")     pairArg    = next();
        else if (a == "--quality")  qualityArg = next();
        else if (a == "--audio")    audioArg   = next();
        else if (a == "--nofabric") nofabric   = true;
        else if (a == "--cpu")      forceCpu   = true;
        else if (a == "--pipe")     pipePath  = next();
        else if (a == "--preset")   presetArg = next();
        else if (a == "--mode")     modeArg   = next();
        else if (a == "--rate")     (void)next();   // AudioPB's fabric rate: accepted (an AudioPB-shaped spawn line) and ignored
        else if (a == "--parent")   parentPid = (uint32_t)std::strtoul(next().c_str(), nullptr, 10);
    }

    // The same migration TrConfig::load performs, applied to the COMMAND LINE: an app built
    // before W2 spawned the helper with --mode en2zh (TrPipe.h says why this is read
    // rather than refused), and a mode the pipeline does not recognise would silently
    // become "auto" -- i.e. the user's lock would quietly stop working.
    if      (modeArg == "en2zh") modeArg = "a2b";
    else if (modeArg == "zh2en") modeArg = "b2a";

    // TEST-ONLY switches (WD). The CPU: `--cpu` or AUDIOPB_TR_FORCE_CPU=1 (forceCpuChildren). With it,
    // the file job may be pointed at smaller models (AUDIOPB_TR_TEST_FILE_ASR / _MT: a CPU run of
    // turbo + the 4B is minutes per file). And a --nofabric engine (a pipe test, never the app's) may
    // answer FILEs with [8]'s stand-ins (AUDIOPB_TR_STANDIN_MARKS = a meeting_marks.txt): the pipe,
    // the servant, the threads and the timing proved with no model at all.
    // (SUVTITLE: the AUDIOPB_TR_* names are kept on purpose -- internal, test-only, S3 -- and the test kits
    // that came along set them.)
    // /!\ REVIEW P4: the engine INHERITS the app's environment, and these variables live on the machine
    // the tests run on -- so they count only for a run that is a test BY ITS COMMAND LINE (a selftest,
    // --nofabric, --cpu): the app passes none of those, and a `setx AUDIOPB_TR_FORCE_CPU 1` left
    // behind can never turn the shipping engine CPU-only or swap its file models. (A USER who wants the
    // CPU says so in engine.txt: DEVICE cpu.)
    auto envOf = [](const wchar_t* name) {
        wchar_t v[1024] = {};
        const DWORD n = ::GetEnvironmentVariableW(name, v, 1024);
        return (n > 0 && n < 1024) ? wideToUtf8L(v) : std::string();
    };
    const bool testRun = testEnvAllowed(selftest || selftestFm || selftestV2 || selftestWf || selftestWh
                                        || !fileArg.empty(), nofabric, forceCpu);
    if (envOf(L"AUDIOPB_TR_FORCE_CPU") == "1") {
        if (testRun) forceCpu = true;
        else ::OutputDebugStringA("[SuVtitleEngine] AUDIOPB_TR_FORCE_CPU ignored: not a test run "
                                  "(--selftest*, --nofabric or --cpu)\n");
    }
    if (forceCpu) {
        forceCpuChildren();
        g_fileAsrModelForTest = envOf(L"AUDIOPB_TR_TEST_FILE_ASR");
        g_fileMtModelForTest  = envOf(L"AUDIOPB_TR_TEST_FILE_MT");
    }
    if (nofabric && !pipePath.empty()) {
        const std::string marks = envOf(L"AUDIOPB_TR_STANDIN_MARKS");
        if (!marks.empty()) installStandIns(utf8ToWideL(marks));
    }
    // TEST-ONLY (WF): the file whisper WITHOUT DTW (flash attention back on, whisper's token times) -- the
    // A/B that prices DTW on a real file. Like every AUDIOPB_TR_* variable, inert outside a test run.
    if (testRun && envOf(L"AUDIOPB_TR_TEST_NO_DTW") == "1") g_fileNoDtwForTest = true;
    // (WF review P3: the first window's whisper call fails once -- the job must go on without DTW)
    if (testRun && envOf(L"AUDIOPB_TR_TEST_ASR_FAIL_ONCE") == "1") g_fileAsrFailOnceForTest = true;

    if (selftestWf)  return runSelftestWf();
    if (selftest)    return runSelftest(stOffline);
    if (selftestFm)  return runSelftestFileMode(fmOffline);
    if (selftestV2)  return runSelftestV2(v2Offline);
    if (selftestWh)  return runSelftestWh(whOffline);
    if (!fileArg.empty()) return runSelftestFile(fileArg, pairArg, modeArg, qualityArg, audioArg);
    if (pipePath.empty()) {
        std::printf("SuVtitle Engine " SUVTITLE_ENGINE_VERSION_STR " -- the AI half of SuVtitle (whisper + llama, driven over a pipe)\n"
                    "SuVtitleEngine --pipe <pipe> [--parent pid] [--preset quality|balanced|light|auto]\n"
                    "               [--mode auto|a2b|b2a] [--nofabric] [--cpu]\n"
                    "               --selftest [offline] | --selftest-filemode [offline] | --selftest-v2 [offline]\n"
                    "               --selftest-wf (the film-grade cue rules, offline)\n"
                    "               --selftest-wh [offline] (the film's own sound tracks; live jobs: use --cpu)\n"
                    "               --selftest-file <media> [--pair a:b] [--mode m] [--quality auto|max|fast]\n"
                    "                               [--audio <ord>:<trackId>]\n"
                    "               [--cpu] (or AUDIOPB_TR_FORCE_CPU=1 with a test switch): every child server on the CPU,\n"
                    "               the GPU untouched. --nofabric: a TEST engine (writes nothing: no engine.txt, no log).\n"
                    "payload: <exe dir>\\engine\\whisper, \\engine\\llama, \\models; with none of them next to the exe (a build tree)\n"
                    "         the folder named by SUVTITLE_PAYLOAD (whisper\\, llama\\, models\\); engine.txt WHISPERBIN / LLAMABIN / MODELS win.\n"
                    "selftests read their private media from SUVTITLE_TEST_MEDIA (and SUVTITLE_TEST_FILM): NOT CHECKED when unset.\n"
                    "test environment: %s\n",
                    forceCpu ? "CPU forced"
                    : (envOf(L"AUDIOPB_TR_FORCE_CPU") == "1" ? "AUDIOPB_TR_FORCE_CPU ignored (not a test run)" : "none"));
        return 1;
    }
    return runHost(pipePath, parentPid, presetArg, modeArg, nofabric);
}

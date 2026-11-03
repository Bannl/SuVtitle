// =============================================================================
// SuVtitle tests/cpp/pipe/fake_engine.cpp -- S-HOST fix round (2026-10-23). A scripted stand-in for SuVtitleEngine.exe,
// built as SuVtitleEngine.exe NEXT TO pipe_smoke.exe (tests\cpp\work\pipe\) so the REAL app\TranslatorManager.cpp
// spawns it exactly as it spawns the real engine. Started from RV-2's scratchpad\review\rv2\pipe\rv2_fake.cpp (same
// modes, same log); added: the SDDL of the pipe it connected to, read on its own handle (P0).
// Behaviour chosen by "--fake <mode>" (the manager's setSpawnArgs puts it on the command line):
//   ok          connect, HELLO, STATE listening, then read until QUIT / a broken pipe
//   crashloop   connect, HELLO, STATE, live 300 ms, then die (a post-HELLO crash: the restart loop)
//   noconnect   never connect (a slow first start: AV scan of a new exe, a sleeping disk), sleep 30 s
//   nohello     connect, never say HELLO, sleep 30 s
//   slow        wait 1500 ms, then like ok (lets another client be first in line)
//   slowquit    like ok, but QUIT takes 3 s to honour (the model servers' shutdown)
//   live        (SuVtitle 1.1.0 LIVE, test agent 2026-10-25) a stand-in for the engine's LIVE half, for a REAL run of SuVtitle.exe
//               (the app spawns <its folder>\SuVtitleEngine.exe with no --fake: the mode comes from the environment variable
//               SUV_FAKE_MODE=live). No model, no capture: SRCLIST -> 7 SRC (in/out/app, one absent, one busy, one denied) +
//               SRCEND; LIVE 1 -> STATE live=1, INPUT per device (a strip LABELLED busy / denied / stalled gets that state),
//               LEVELS at 5 Hz per LANE, then a scripted conversation (PART, PART, LINE or END every 3.2 s, voices S1/S2 on
//               strip 0, spkSlot = the lane); LIVE 0 -> STATE live=0 and silence; CTL scope=live/file -> the echo; FILE ->
//               FDONE error. The live pair it reports is en/fr, the player's en/zh, so a page showing the wrong one is seen.
//               FIX ROUND (2026-10-26, the engine's new LIVE 0 -- PROGRESS_L1a RESUME 9): LIVE 0 = stop listening, then
//               FINISH: no LEVELS, no new utterance, STATE live stays 1 while the open utterance (cut: its LINE) and
//               SUV_FAKE_DRAIN_LINES queued finals (default 3) come out one every SUV_FAKE_DRAIN_MS (default 700), then STATE
//               live 0; a LIVE 1 meanwhile RESUMES. A LIVE 1 with a strip labelled "refuse" is REFUSED the engine's way:
//               one STATE whose err says why, the live state unchanged.
// Every event is appended to fake_engine.log next to the exe: "<tick> <pid> <event>". ASCII only.
// =============================================================================
#include "../../../common/TrPipe.h"

#include <aclapi.h>
#include <sddl.h>
#include <cmath>
#include <cstdio>
#include <share.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace tp = pbt::trpipe;
static std::wstring g_log;

static void logLine(const std::string& s) {
    FILE* f = nullptr;
    for (int i = 0; i < 25 && !f; ++i) { f = _wfsopen(g_log.c_str(), L"ab", _SH_DENYNO); if (!f) ::Sleep(20); }
    if (!f) return;
    std::string v = s;
    for (auto& c : v) if (c == '\t') c = '|';
    std::fprintf(f, "%llu %lu %s\n", (unsigned long long)::GetTickCount64(), ::GetCurrentProcessId(), v.c_str());
    std::fclose(f);
}

static std::string narrow(const std::wstring& w) {     // (ASCII: modes and pipe names)
    std::string o;
    for (wchar_t c : w) o += static_cast<char>(c);
    return o;
}

// ---- the live mode ---------------------------------------------------------------------------------------------------------
static uint64_t epochMs() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

namespace {
struct LiveSim {
    tp::WireState st;
    bool on = false;
    std::vector<tp::WireLiveInput> strips;
    ULONGLONG t0 = 0;           // the tick of the LIVE 1 that is running
    ULONGLONG nextLevels = 0;   // the tick of the next LEVELS frame
    int step = 0;               // how many of the current utterance's events were sent
    int utt = 0;                // the running utterance's number (0, 1, 2...: strip utt % strips)
    ULONGLONG uttAt = 0;        // the tick the running utterance started
    uint32_t gen = 0;           // the SRC list's generation
    uint64_t segBase = 100;
    // the drain (fix round): LIVE 0 while listening -> the capture stops (on = false), STATE live stays 1 until done
    bool draining = false;
    int  drainLeft = 0;         // queued finals still to come out
    ULONGLONG drainNext = 0;
    int  drainLines = 3;        // SUV_FAKE_DRAIN_LINES
    int  drainMs = 700;         // SUV_FAKE_DRAIN_MS
    uint64_t drainSeg = 900;    // the queued finals' segIds
};

const char* const kSayEn[] = { "Hello everyone, thanks for joining.", "Can you hear me well at the back?",
                               "He said \"ok\" and we all moved on.", "Let us start with the agenda.",
                               "The second point is the budget.", "Any questions before we close?" };
const char* const kSayFr[] = { "Bonjour a tous, merci d'etre la.", "Vous m'entendez bien au fond ?",
                               "Il a dit \"d'accord\" et on a continue.", "Commencons par l'ordre du jour.",
                               "Le deuxieme point est le budget.", "Des questions avant de terminer ?" };
const char* const kDraftEn[] = { "Hello every", "Can you hear", "He said ok", "Let us", "The second", "Any quest" };
const char* const kDraftFr[] = { "Bonjour a", "Vous m'ent", "Il a dit", "Com", "Le deux", "Des ques" };

void simSources(HANDLE pipe, LiveSim& s) {
    ++s.gen;
    struct Row { const char* kind; const char* key; const char* label; const char* state; uint32_t rate, ch; const char* flags; };
    static const Row rows[] = {
        { "in",  "in:{0.0.1.00000000}.{fake-mic-0001}",   "Microphone (USB Audio)", "ok",     48000, 2, "default,comm" },
        { "in",  "in:{0.0.1.00000000}.{fake-ur44c-0002}", "Line 1-4 (UR44C)",       "ok",     48000, 4, "" },
        { "in",  "in:{0.0.1.00000000}.{fake-webcam-0003}","Webcam mic",             "absent", 0,     0, "" },
        { "out", "out:{0.0.0.00000000}.{fake-spk-0004}",  "Speakers (Realtek)",     "ok",     48000, 2, "default" },
        { "out", "out:{0.0.0.00000000}.{fake-hp-0005}",   "Headphones (BT)",        "busy",   44100, 2, "comm" },
        { "app", "app:zoom.exe",                          "Zoom Meetings",          "ok",     0,     0, "" },
        { "app", "app:teams.exe",                         "Microsoft Teams",        "denied", 0,     0, "" },
    };
    uint32_t n = 0;
    for (const Row& r : rows) {
        tp::WireSrc w;
        w.gen = s.gen; w.kind = r.kind; w.key = r.key; w.label = r.label; w.state = r.state; w.rate = r.rate; w.ch = r.ch; w.flags = r.flags;
        tp::writeMessage(pipe, tp::packSrc(w), 3000);
        ++n;
    }
    tp::WireSrcEnd e;
    e.gen = s.gen; e.n = n;
    tp::writeMessage(pipe, tp::packSrcEnd(e), 3000);
}

bool hasWord(const std::string& label, const char* w) { return label.find(w) != std::string::npos; }

void simInputs(HANDLE pipe, const LiveSim& s) {
    std::vector<std::string> seen;           // one INPUT per DEVICE (TrPipe.h LANES)
    for (const auto& in : s.strips) {
        bool dup = false;
        for (const auto& k : seen) dup = dup || k == in.key;
        if (dup) continue;
        seen.push_back(in.key);
        tp::WireInput wi;
        wi.key = in.key; wi.state = "open";
        if (hasWord(in.label, "busy"))    { wi.state = "busy";    wi.why = "Another program has exclusive control of this device."; }
        if (hasWord(in.label, "denied"))  { wi.state = "denied";  wi.why = "Windows is blocking microphone access for desktop apps."; }
        if (hasWord(in.label, "stalled")) { wi.state = "stalled"; wi.why = "No audio for 5 seconds."; }
        tp::writeMessage(pipe, tp::packInput(wi), 3000);
    }
}

// one finished line of strip k, utterance `utt` (strip 0 has two voices over time: S1 then S2 -- the label rule's " . 2")
void sendLine(HANDLE pipe, const LiveSim& s, size_t k, int utt, uint64_t seg) {
    const size_t n = s.strips.size();
    const bool fr = (k % 2 == 1);                                                // odd strips speak French
    const size_t say = static_cast<size_t>(utt) % 6;
    tp::WireLine l;
    l.tEpochMs = epochMs(); l.lang = fr ? "fr" : "en"; l.dstLang = fr ? "en" : "fr";
    l.src = fr ? kSayFr[say] : kSayEn[say]; l.dst = fr ? kSayEn[say] : kSayFr[say];
    l.ms = 900; l.waitMs = 1100; l.spkSlot = tp::liveLaneId(s.strips[k].key, s.strips[k].chan); l.spkLang = l.lang;
    l.segId = seg; l.seq = 1; l.closes = true;
    l.spkLabel = (k == 0 && (utt / static_cast<int>(n)) % 2 == 1) ? "S2" : "S1";
    tp::writeMessage(pipe, tp::packLine(l), 3000);
}

// THE DRAIN (fix round: the engine's LIVE 0 = stop listening, then FINISH -- PROGRESS_L1a RESUME 9): one event every
// drainMs -- the utterance that was open (the stop cut it into a final: its LINE, or its END for a breath), then the queued
// finals -- and STATE live 0 only once they are all out. No LEVELS meanwhile (the capture is stopped).
void simDrain(HANDLE pipe, LiveSim& s) {
    const ULONGLONG now = ::GetTickCount64();
    if (now < s.drainNext) return;
    s.drainNext = now + static_cast<ULONGLONG>(s.drainMs);
    const size_t n = s.strips.size();
    if (s.step > 0 && n) {
        const uint64_t seg = s.segBase + static_cast<uint64_t>(s.utt);
        if (s.utt % 5 == 4) { tp::WireEnd e; e.segId = seg; tp::writeMessage(pipe, tp::packEnd(e), 3000); logLine("DRAIN_END"); }
        else { sendLine(pipe, s, static_cast<size_t>(s.utt) % n, s.utt, seg); logLine("DRAIN_LINE cut"); }
        s.step = 0;
        ++s.utt;
        return;
    }
    if (s.drainLeft > 0 && n) {
        --s.drainLeft;
        sendLine(pipe, s, static_cast<size_t>(s.utt) % n, s.utt, s.drainSeg++);
        ++s.utt;
        logLine("DRAIN_LINE queued");
        return;
    }
    s.draining = false;
    s.st.live = false;
    tp::writeMessage(pipe, tp::packState(s.st), 3000);
    logLine("DRAIN_DONE live 0");
}

// called every ~50 ms while the pipe is quiet: the meters at 5 Hz, and the scripted conversation's next event when it is due
void simTick(HANDLE pipe, LiveSim& s) {
    if (s.draining) { simDrain(pipe, s); return; }
    if (!s.on || s.strips.empty()) return;
    const ULONGLONG now = ::GetTickCount64();
    const size_t n = s.strips.size();
    if (now >= s.nextLevels) {
        s.nextLevels = now + 200;
        tp::WireLevels lv;
        for (size_t k = 0; k < n; ++k) {
            const double t = static_cast<double>(now - s.t0) / 1000.0;
            float peak = static_cast<float>(-38.0 + 24.0 * (0.5 + 0.5 * std::sin(t * 2.3 + static_cast<double>(k) * 1.7)));
            if (k == 0 && static_cast<long long>(t) % 9 == 8) peak = 0.0f;       // a clip now and then
            lv.peaks.push_back({ tp::liveLaneId(s.strips[k].key, s.strips[k].chan), peak });
        }
        tp::writeMessage(pipe, tp::packLevels(lv), 3000);
    }
    // the conversation: utterance `utt` is strip utt % n; at +0 and +700 ms a PART, at +1400 ms its LINE (or an END for every 5th)
    if (now < s.uttAt) return;
    const ULONGLONG at = now - s.uttAt;
    const size_t k = static_cast<size_t>(s.utt) % n;
    const bool fr = (k % 2 == 1);                                                // odd strips speak French
    const uint64_t seg = s.segBase + static_cast<uint64_t>(s.utt);
    const size_t say = static_cast<size_t>(s.utt) % 6;
    if (s.step == 0 || (s.step == 1 && at >= 700)) {
        tp::WirePartial p;
        p.segId = seg; p.tEpochMs = epochMs(); p.lang = fr ? "fr" : "en";
        p.src = (s.step == 0) ? (fr ? kDraftFr[say] : kDraftEn[say]) : (fr ? kSayFr[say] : kSayEn[say]);
        p.dst = (s.step == 0) ? "" : (fr ? kSayEn[say] : kSayFr[say]);
        p.dstLang = p.dst.empty() ? "" : (fr ? "en" : "fr");
        tp::writeMessage(pipe, tp::packPartial(p), 3000);
        ++s.step;
        return;
    }
    if (s.step == 2 && at >= 1400) {
        if (s.utt % 5 == 4) {
            tp::WireEnd e;
            e.segId = seg;
            tp::writeMessage(pipe, tp::packEnd(e), 3000);
        } else {
            sendLine(pipe, s, k, s.utt, seg);           // (strip 0 has two voices over time: S1 then S2)
        }
        ++s.utt;
        s.step = 0;
        s.uttAt = now + 1800;                      // the next utterance starts 1.8 s later: 3.2 s per slot in all
    }
}

// one message from the app; false = not a live-mode message (the caller's own handling goes on)
bool simHandle(HANDLE pipe, LiveSim& s, const std::string& msg) {
    const auto t = tp::split(msg);
    if (t.empty()) return false;
    if (t[0] == "SRCLIST") { simSources(pipe, s); return true; }
    if (t[0] == "LIVE") {
        tp::WireLive l;
        if (!tp::unpackLive(t, l)) { logLine("LIVE refused"); return true; }
        const ULONGLONG now = ::GetTickCount64();
        if (l.on) {
            // (fix round) the engine's REFUSAL: one STATE whose err says why, the live state unchanged
            for (const auto& in : l.inputs)
                if (hasWord(in.label, "refuse")) {
                    tp::WireState r = s.st;
                    r.err = "The live translation could not open these inputs.";
                    tp::writeMessage(pipe, tp::packState(r), 3000);
                    logLine("LIVE refused (err)");
                    return true;
                }
            const bool resume = s.draining;             // a LIVE 1 during the drain RESUMES (the fix round's engine)
            s.draining = false;
            s.on = true;
            s.strips = l.inputs;
            s.st.live = true;
            if (resume) { s.nextLevels = 0; s.step = 0; s.uttAt = now + 800; logLine("DRAIN_RESUMED"); }
            else { s.t0 = now; s.nextLevels = 0; s.step = 0; s.utt = 0; s.uttAt = s.t0 + 1200; }
            tp::writeMessage(pipe, tp::packState(s.st), 3000);
            simInputs(pipe, s);
            return true;
        }
        if (s.on) {                                     // LIVE 0 while listening: stop listening, then FINISH (live stays 1)
            s.on = false;
            s.draining = true;
            s.drainLeft = s.drainLines;
            s.drainNext = now + 300;
            logLine("DRAIN_START");
            return true;
        }
        if (s.draining) return true;                    // (already finishing)
        s.strips.clear();
        s.st.live = false;
        tp::writeMessage(pipe, tp::packState(s.st), 3000);
        return true;
    }
    if (t[0] == "CTL") {
        tp::WireCtl c;
        if (!tp::unpackCtl(t, c) || !tp::ctlScopeKnown(c)) return true;
        if (tp::ctlIsLive(c)) {
            if (!c.mode.empty()) s.st.liveMode = c.mode;
            if (!c.langA.empty() && !c.langB.empty()) { s.st.liveLangA = c.langA; s.st.liveLangB = c.langB; }
            if (c.clear) { s.utt = 0; s.step = 0; s.uttAt = ::GetTickCount64() + 1200; }
        } else {
            if (!c.mode.empty()) s.st.mode = c.mode;
            if (!c.langA.empty() && !c.langB.empty()) { s.st.langA = c.langA; s.st.langB = c.langB; }
        }
        if (!c.preset.empty()) s.st.preset = (c.preset == "auto") ? "balanced" : c.preset;
        tp::writeMessage(pipe, tp::packState(s.st), 3000);
        return true;
    }
    if (t[0] == "FILE") {
        tp::WireFile f;
        if (!tp::unpackFile(t, f)) return true;
        tp::WireFileDone d;
        d.jobId = f.jobId; d.result = "error"; d.count = 0; d.err = "the fake engine analyses no file";
        tp::writeMessage(pipe, tp::packFileDone(d), 3000);
        return true;
    }
    if (t[0] == "FPROXY") {
        tp::WireProxy p;
        if (!tp::unpackProxy(t, p)) return true;
        tp::WireProxyDone d;
        d.jobId = p.jobId; d.ok = false; d.err = "the fake engine makes no proxy";
        tp::writeMessage(pipe, tp::packProxyDone(d), 3000);
        return true;
    }
    return false;
}
}   // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipeName, mode = L"ok";
    bool modeGiven = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--pipe") pipeName = argv[i + 1];
        if (std::wstring(argv[i]) == L"--fake") { mode = argv[i + 1]; modeGiven = true; }
    }
    if (!modeGiven) {            // a real app spawns us with no --fake: the environment says which stand-in we are
        wchar_t env[64] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"SUV_FAKE_MODE", env, 64);
        if (n > 0 && n < 64) mode = env;
    }
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_log = self;
    g_log = g_log.substr(0, g_log.find_last_of(L"\\/") + 1) + L"fake_engine.log";
    logLine("START " + narrow(mode) + " " + narrow(pipeName));
    if (pipeName.empty()) return 2;
    if (mode == L"noconnect") { ::Sleep(30000); return 0; }
    if (mode == L"slow") ::Sleep(1500);

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 40 && pipe == INVALID_HANDLE_VALUE; ++i) {   // like the real engine: retry a busy pipe
        pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD e = ::GetLastError();
        logLine("CONNECT_RETRY err " + std::to_string(e));
        if (e == ERROR_PIPE_BUSY) ::WaitNamedPipeW(pipeName.c_str(), 500); else ::Sleep(50);
    }
    if (pipe == INVALID_HANDLE_VALUE) { logLine("CONNECT_FAIL"); return 3; }
    ULONG serverPid = 0;
    ::GetNamedPipeServerProcessId(pipe, &serverPid);
    logLine("CONNECTED server_pid " + std::to_string(serverPid));
    {
        // P0: the security of the pipe this engine was given, as Windows reports it on the engine's own handle
        PSECURITY_DESCRIPTOR sd = nullptr;
        PACL dacl = nullptr;
        std::string sddl = "?";
        const DWORD e = ::GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
        if (e == ERROR_SUCCESS) {
            LPSTR s = nullptr;
            if (::ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &s, nullptr)) {
                sddl = s;
                ::LocalFree(s);
            }
            ::LocalFree(sd);
        } else {
            sddl = "ERR " + std::to_string(e);
        }
        logLine("SDDL " + sddl);
    }
    DWORD pm = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(pipe, &pm, nullptr, nullptr);
    if (mode == L"nohello") { ::Sleep(30000); return 0; }

    tp::writeMessage(pipe, "HELLO\tengine", 3000);
    tp::WireState st;
    st.status = "listening";
    LiveSim sim;
    const bool liveMode = (mode == L"live");
    if (liveMode) {   // what the real engine reports at start: its models, the player's pair (en/zh), the live pair (en/fr)
        st.asrModel = "ggml-fake-small.bin"; st.mtModel = "fake-qwen3-4b.gguf"; st.spkOn = true; st.spkModel = "campplus"; st.spkCount = 2;
        st.liveMode = "auto"; st.liveLangA = "en"; st.liveLangB = "fr";
        sim.st = st;
        wchar_t v[32] = {};                             // (fix round) the drain's length, from the run's environment
        DWORD vn = ::GetEnvironmentVariableW(L"SUV_FAKE_DRAIN_LINES", v, 32);
        if (vn > 0 && vn < 32) sim.drainLines = _wtoi(v);
        vn = ::GetEnvironmentVariableW(L"SUV_FAKE_DRAIN_MS", v, 32);
        if (vn > 0 && vn < 32) sim.drainMs = _wtoi(v);
        if (sim.drainLines < 0) sim.drainLines = 0;
        if (sim.drainMs < 50) sim.drainMs = 50;
    }
    tp::writeMessage(pipe, tp::packState(st), 3000);
    logLine("HELLO_SENT");
    const ULONGLONG born = ::GetTickCount64();
    for (;;) {
        if (mode == L"crashloop" && ::GetTickCount64() - born > 300) { logLine("CRASH"); ::ExitProcess(9); }
        if (liveMode) simTick(pipe, sim);
        std::string msg;
        bool broken = false;
        if (!tp::readMessage(pipe, msg, 50, &broken)) {
            if (broken) { logLine("BROKEN"); return 0; }
            continue;
        }
        logLine("RX " + msg.substr(0, liveMode ? 800 : 120));   // (a LIVE with 4 strips is longer than 120 bytes)
        if (msg == "QUIT") {
            if (mode == L"slowquit") { ::Sleep(3000); logLine("QUIT_DONE_LATE"); }
            return 0;
        }
        if (liveMode && simHandle(pipe, sim, msg)) continue;
    }
}

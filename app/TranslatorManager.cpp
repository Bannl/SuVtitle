// =============================================================================
// SuVtitle - app/TranslatorManager.cpp   (supervisor of SuVtitleEngine.exe)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TranslatorManager.cpp at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). See TranslatorManager.h for what changed.
// FIX ROUND 2026-10-23 (docs/REVIEW_RV2_App.md, proofs in tests/cpp/pipe_smoke.cpp): RV2-1 createEnginePipe +
// the client check in spawnChild; RV2-2 quitEv_ + publishKill; RV2-5 failedStart().
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
// =============================================================================
#include "TranslatorManager.h"

#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <set>

#pragma comment(lib, "advapi32.lib")

namespace pbr {

namespace tp = pbt::trpipe;

// FILE MODE: the sentences a SYNTHESIZED answer carries. Spelled once, so the bridge and its
// harness can rely on them.
static const char* const kFileEngineStopped = "The SuVtitle engine stopped during the analysis.";
static const char* const kFileEngineAbsent  = "The SuVtitle engine is not running.";
static const char* const kFileEngineFailing = "The SuVtitle engine keeps failing to start.";
static const char* const kProxyEngineStopped = "The SuVtitle engine stopped.";

// Deadlines. The helper answers HELLO before it loads a single model (the pipe
// connect happens first, the 1.6 GB load happens on its pipeline thread), so the
// connect leash is short -- a slow model is NOT a slow handshake.
static constexpr DWORD kConnectDeadlineMs = 8000;
static constexpr DWORD kReadPollMs        = 150;    // "nothing to say" is the resting state
static constexpr DWORD kQuitDeadlineMs    = 2000;
static constexpr DWORD kRespawnBackoffMs  = 4000;
// RV2-5: from the third failed start in a row the wait doubles (8, 16, 32 s) up to this cap -- an engine that
// cannot live is not restarted every 4 s for ever (RV-2's P5 measured that: a model load per lap, nothing said).
static constexpr uint64_t kRespawnBackoffCapMs = 60000;
// RV2-1: names in a row that someone else created first, before a start counts as failed.
static constexpr int kMaxTakenNames = 16;
// Rolling cap on the manager's own message queue. The Bridge is now woken the moment
// anything lands in it (setWake) and drains from uiTick as well, so this only ever fills
// if the UI thread is wedged, in which case the OLDEST subtitles are the useless ones.
// Live lines share the cap because they share the queue (see Msg in the header); they
// are also the cheapest thing in it to lose, being superseded by design.
// FILE MODE: the cap counts and drops LIVE kinds only (pushMsgLocked). A file job's messages
// are never dropped: a lost cue is a hole in the .srt that nothing would ever fill.
static constexpr size_t kLineQueueCap = 256;

bool TranslatorManager::isTranslatorName(const std::wstring& n) {
    return n.rfind(L"tr:", 0) == 0;
}

bool TranslatorManager::running() const {
    std::lock_guard<std::mutex> lk(mx_);
    return running_;
}

std::string TranslatorManager::slotName() const {
    std::lock_guard<std::mutex> lk(mx_);
    return slotName_;
}

std::string TranslatorManager::mode() const {
    std::lock_guard<std::mutex> lk(mx_);
    return mode_;
}

std::string TranslatorManager::preset() const {
    std::lock_guard<std::mutex> lk(mx_);
    return preset_;
}

std::string TranslatorManager::langA() const {
    std::lock_guard<std::mutex> lk(mx_);
    return langA_;
}

std::string TranslatorManager::langB() const {
    std::lock_guard<std::mutex> lk(mx_);
    return langB_;
}

bool TranslatorManager::start() {
    if (active_.exchange(true)) return false;   // already on
    quit_.store(false);
    done_.store(false);
    if (!quitEv_) quitEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // RV2-2 (manual reset: every wait sees it)
    else ::ResetEvent(quitEv_);
    worker_ = std::thread(&TranslatorManager::workerMain, this);
    return true;
}

// Ask the worker to finish, give it a beat to do so GRACEFULLY (QUIT lets the helper
// free its fabric slot and kill its two child servers), then force it. Same discipline
// as FxBusManager::shutdownBus, including the kill through a DUPLICATE taken under mx_.
void TranslatorManager::stop() {
    if (!active_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
        return;
    }
    quit_.store(true);
    if (quitEv_) ::SetEvent(quitEv_);   // RV2-2: a start waiting for the engine's connect or HELLO ends NOW
    cv_.notify_all();
    for (int i = 0; i < 50 && !done_.load(std::memory_order_acquire); ++i)
        ::Sleep(20);   // ~1 s of grace: the helper has two child servers to take down
    if (!done_.load(std::memory_order_acquire)) {
        HANDLE dup = nullptr;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (hProcKill_)
                ::DuplicateHandle(::GetCurrentProcess(), hProcKill_,
                                  ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
        if (dup) { ::TerminateProcess(dup, 9); ::CloseHandle(dup); }
    }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lk(mx_);
        running_ = false;
        slotName_.clear();
        state_ = State{};
        stateDirty_ = true;   // the UI must see it go away
        msgs_.clear();
        liveCount_ = 0;
        // FILE MODE: commands nobody will ever send. The bridge ends its own file session when it
        // stops the manager (the card was removed), so no synthesized answer is owed here.
        fileCmds_.clear();
    }
}

void TranslatorManager::setMode(const std::string& m) {
    // W2 turned two hard-coded languages into a DIRECTION over a free pair. The new tokens are
    // auto/a2b/b2a and they pass through untouched; en2zh/zh2en are mapped defensively because
    // this value also travels on a COMMAND LINE (--mode, see spawnChild) where TrPipe's own
    // translation in unpackCtl never runs. A caller that still speaks the old vocabulary would
    // otherwise hand the helper a token it drops on the floor.
    std::string v = m;
    if      (v == "en2zh") v = "a2b";
    else if (v == "zh2en") v = "b2a";
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (mode_ == v) return;
        mode_ = v;
        ctlPending_ = true;
    }
    cv_.notify_all();
}

// One call for both halves: see the header -- a pair is one fact, and the helper refuses a
// degenerate one, so pushing them separately would mean pushing a change it throws away.
void TranslatorManager::setPair(const std::string& a, const std::string& b) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (langA_ == a && langB_ == b) return;
        langA_ = a;
        langB_ = b;
        ctlPending_ = true;
    }
    cv_.notify_all();
}

void TranslatorManager::setPreset(const std::string& p) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (preset_ == p) return;
        preset_ = p;
        ctlPending_ = true;
    }
    cv_.notify_all();
}

void TranslatorManager::askClear() {
    {
        std::lock_guard<std::mutex> lk(mx_);
        clearPending_ = true;
    }
    cv_.notify_all();
}

bool TranslatorManager::pollState(State& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!stateDirty_) return false;
    stateDirty_ = false;
    out = state_;
    return true;
}

std::vector<TranslatorManager::Msg> TranslatorManager::pollMessages() {
    std::vector<Msg> out;
    std::lock_guard<std::mutex> lk(mx_);
    out.assign(std::make_move_iterator(msgs_.begin()), std::make_move_iterator(msgs_.end()));
    msgs_.clear();
    liveCount_ = 0;
    return out;
}

// ---- FILE MODE --------------------------------------------------------------------
// The cap is for LIVE lines only (see Msg in the header). When it bites, the oldest LIVE message
// goes -- exactly what the old `pop_front` did while live lines were all there was -- and a file
// message is stepped over, never dropped and never counted.
void TranslatorManager::pushMsgLocked(Msg&& m) {
    if (isLiveKind(m.kind)) {
        while (liveCount_ >= kLineQueueCap) {
            auto it = std::find_if(msgs_.begin(), msgs_.end(),
                                   [](const Msg& q) { return isLiveKind(q.kind); });
            if (it == msgs_.end()) { liveCount_ = 0; break; }   // cannot happen; never spin
            msgs_.erase(it);
            --liveCount_;
        }
        ++liveCount_;
    }
    msgs_.push_back(std::move(m));
}

void TranslatorManager::pushDoneLocked(uint64_t jobId, const char* result, const std::string& err) {
    Msg m;
    m.kind = Msg::Kind::FileDone;
    m.fdone.jobId  = jobId;
    m.fdone.result = result;
    m.fdone.count  = 0;
    m.fdone.err    = err;
    m.synth        = true;             // made up HERE: it vouches for nothing the helper reported (WE)
    pushMsgLocked(std::move(m));
}

void TranslatorManager::pushProxyDoneLocked(uint64_t id, const std::string& err) {
    Msg m;
    m.kind = Msg::Kind::ProxyDone;
    m.pdone.jobId = id;
    m.pdone.ok    = false;
    m.pdone.err   = err;
    m.synth       = true;
    pushMsgLocked(std::move(m));
}

void TranslatorManager::startFile(const tp::WireFile& f) {
    if (f.jobId == 0 || f.path.empty()) return;   // unpackFile would refuse it on the other side
    bool answered = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        // An UNSENT older FILE is superseded right here: the helper would cancel it the moment
        // the new one arrived ("a FILE while a job is running CANCELS that job first"), so
        // sending it at all would only cost a model load. Its one FDONE is owed all the same.
        for (auto it = fileCmds_.begin(); it != fileCmds_.end(); ) {
            if (it->kind == FileCmd::Kind::File) {
                pushDoneLocked(it->id, "cancelled", std::string());
                it = fileCmds_.erase(it);
                answered = true;
            } else if (it->kind == FileCmd::Kind::Focus) {
                it = fileCmds_.erase(it);          // WE: aimed at a job that will not run now
            } else {
                ++it;
            }
        }
        if (!active_.load(std::memory_order_relaxed)) {
            pushDoneLocked(f.jobId, "error", kFileEngineAbsent);
            answered = true;
        } else {
            FileCmd c;
            c.kind = FileCmd::Kind::File;
            c.id   = f.jobId;
            c.wire = tp::packFile(f);
            c.file = f;
            fileCmds_.push_back(std::move(c));
        }
    }
    cv_.notify_all();
    // A synthesized answer must reach the UI now, not at the next tick of the 1 Hz safety net.
    if (answered) wake();
}

void TranslatorManager::cancelFile(uint64_t jobId) {
    if (jobId == 0) return;
    bool answered = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        // FILE and FPROXY ids come from the bridge's ONE counter, so an id names exactly one of
        // them, and FCANCEL on a proxy's id cancels that proxy (TrPipe.h).
        for (auto it = fileCmds_.begin(); it != fileCmds_.end(); ++it) {
            if (it->kind == FileCmd::Kind::File && it->id == jobId) {
                // Never sent: there is nothing to stop on the other side, only an answer to give.
                fileCmds_.erase(it);
                pushDoneLocked(jobId, "cancelled", std::string());
                answered = true;
                fileCmds_.erase(std::remove_if(fileCmds_.begin(), fileCmds_.end(), [jobId](const FileCmd& x) {
                    return x.kind == FileCmd::Kind::Focus && x.id == jobId; }), fileCmds_.end());
                break;
            }
            if (it->kind == FileCmd::Kind::Proxy && it->id == jobId) {
                fileCmds_.erase(it);
                pushProxyDoneLocked(jobId, "cancelled");
                answered = true;
                break;
            }
        }
        if (!answered && active_.load(std::memory_order_relaxed)) {
            FileCmd c;
            c.kind = FileCmd::Kind::Cancel;
            c.id   = jobId;
            c.wire = tp::packFileCancel(jobId);
            fileCmds_.push_back(std::move(c));
        }
    }
    cv_.notify_all();
    if (answered) wake();
}

void TranslatorManager::requestProxy(const tp::WireProxy& p) {
    if (p.jobId == 0 || p.src.empty() || p.out.empty()) return;
    bool answered = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        // Same rule as an unsent FILE, and the helper's own rule for a running proxy: a new
        // FPROXY SUPERSEDES the older one, which answers ok=0 err="superseded" (TrPipe.h).
        for (auto it = fileCmds_.begin(); it != fileCmds_.end(); ) {
            if (it->kind == FileCmd::Kind::Proxy) {
                pushProxyDoneLocked(it->id, "superseded");
                it = fileCmds_.erase(it);
                answered = true;
            } else {
                ++it;
            }
        }
        if (!active_.load(std::memory_order_relaxed)) {
            pushProxyDoneLocked(p.jobId, kFileEngineAbsent);
            answered = true;
        } else {
            FileCmd c;
            c.kind = FileCmd::Kind::Proxy;
            c.id   = p.jobId;
            c.wire = tp::packProxy(p);
            fileCmds_.push_back(std::move(c));
        }
    }
    cv_.notify_all();
    if (answered) wake();
}

void TranslatorManager::focusFile(uint64_t jobId, uint64_t ms) {
    if (jobId == 0) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        bool done = false;
        for (FileCmd& c : fileCmds_) {
            if (c.kind == FileCmd::Kind::File && c.id == jobId) {      // still unsent: patch it
                c.file.focusMs = ms;
                c.wire = tp::packFile(c.file);
                done = true;
                break;
            }
            if (c.kind == FileCmd::Kind::Focus && c.id == jobId) {     // an unsent FFOCUS: the latest wins
                c.wire = tp::packFileFocus(jobId, ms);
                done = true;
                break;
            }
        }
        if (!done && active_.load(std::memory_order_relaxed)) {
            FileCmd c;
            c.kind = FileCmd::Kind::Focus;
            c.id   = jobId;
            c.wire = tp::packFileFocus(jobId, ms);
            fileCmds_.push_back(std::move(c));
        }
    }
    cv_.notify_all();
}

std::vector<std::wstring> TranslatorManager::pollWarnings() {
    std::vector<std::wstring> out;
    std::lock_guard<std::mutex> lk(mx_);
    out.swap(warnings_);
    return out;
}

// =============================================================================
// The worker: owns the process + pipe, end to end.
// =============================================================================
namespace {

std::wstring exeDir() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut + 1);
}

struct Child {
    HANDLE process = nullptr;
    HANDLE pipe    = INVALID_HANDLE_VALUE;

    void kill() {
        if (pipe != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
        if (process) {
            ::TerminateProcess(process, 9);
            ::WaitForSingleObject(process, 3000);   // it has two child servers to reap
            ::CloseHandle(process);
            process = nullptr;
        }
    }
    bool alive() const {
        return process && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
};

// ---- the engine's pipe (review RV2-1, 2026-10-23) -------------------------------------------------------------
// BEFORE: CreateNamedPipeW with the DEFAULT security -- RV-2's P0 read Everyone and Anonymous in its DACL (read
// access) -- and without FILE_FLAG_FIRST_PIPE_INSTANCE, so a process that created suvtitle.engine.<pid>.<seq> first
// (a name anyone can predict) was JOINED as a second instance, and the engine, opening the name, landed on the
// SQUATTER's instance, said HELLO to it and took its commands (P2: proved with a CTL; a FILE / FPROXY would have
// made the engine read any path and write its proxy wherever `out` said). NOW:
//   * FILE_FLAG_FIRST_PIPE_INSTANCE: a name that already exists is REFUSED (ERROR_ACCESS_DENIED), so no engine is
//     ever started on a name this process does not own -- spawnChild takes the next seq at once instead;
//   * PIPE_REJECT_REMOTE_CLIENTS, and ONE instance (nobody can add a second one to ours);
//   * a PROTECTED DACL ("D:P", nothing inherited) granting GENERIC_ALL to this process's user SID and to nobody else:
//     no Everyone, no Anonymous. The SID comes from the token; no SID = no pipe, never the default DACL.
// The engine is spawned with this same token, so it is the one other process that may open the pipe -- and
// spawnChild still checks that the client IS that engine (P4: another process of the same user, first in line).
HANDLE createEnginePipe(const std::wstring& name) {
    std::wstring sddl;
    HANDLE tok = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0;
        ::GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> buf(n ? n : 1);
        LPWSTR sid = nullptr;
        if (n && ::GetTokenInformation(tok, TokenUser, buf.data(), n, &n)
            && ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
            ::LocalFree(sid);
        }
        ::CloseHandle(tok);
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (sddl.empty() || !::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        ::SetLastError(ERROR_INVALID_SECURITY_DESCR);
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    HANDLE pipe = ::CreateNamedPipeW(name.c_str(),
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, 1 << 18, 1 << 18, 0, &sa);
    const DWORD e = ::GetLastError();
    ::LocalFree(sd);
    ::SetLastError(e);
    return pipe;
}

// What one start met, said once by the worker (RV2-1): names someone else had created, clients that were not the engine.
struct SpawnReport { int taken = 0; int rogues = 0; };

// `seq` is advanced past every name tried. `quitEv` (RV2-2) ends every wait of the start; `publishKill` hands the
// process to stop()'s emergency brake from CreateProcess on, and takes it back before the handle is closed.
bool spawnChild(const std::wstring& extra, uint32_t& seq, const std::string& mode, const std::string& preset,
                HANDLE quitEv, const std::function<void(HANDLE)>& publishKill, Child& child, std::string& slotName,
                SpawnReport& rep) {
    // A name of our own: one that exists is someone else's (FILE_FLAG_FIRST_PIPE_INSTANCE), so the next one at once,
    // never an engine on it. Bounded: kMaxTakenNames in a row is a failed start like any other.
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::wstring wpipe;
    for (int k = 0; k < kMaxTakenNames && pipe == INVALID_HANDLE_VALUE; ++k) {
        const std::string pipePath = tp::pipeName(::GetCurrentProcessId(), ++seq);
        wpipe.assign(pipePath.begin(), pipePath.end());
        pipe = createEnginePipe(wpipe);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD e = ::GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_PIPE_BUSY) return false;
        ++rep.taken;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // SuVtitle (2026-10-23): the engine is the app's SIBLING (S2), and it has no fabric, hence no --rate.
    std::wstring cmd = L"\"" + exeDir() + L"SuVtitleEngine.exe\""
                     + L" --pipe " + wpipe
                     + L" --parent " + std::to_wstring(::GetCurrentProcessId());
    if (!extra.empty()) cmd += L" " + extra;   // the app's own switches (setSpawnArgs: --cpu for a test instance)
    // Only OVERRIDE when the user has actually chosen: an empty value means "read your
    // own translator.txt", which is where the persisted choice lives (TranslatorManager.h).
    // Both are single tokens from a closed vocabulary, so the naive widen is safe here.
    if (!mode.empty())   cmd += L" --mode "   + std::wstring(mode.begin(), mode.end());
    if (!preset.empty()) cmd += L" --preset " + std::wstring(preset.begin(), preset.end());
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(L'\0');
    if (!::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pipe);
        return false;
    }
    ::CloseHandle(pi.hThread);
    publishKill(pi.hProcess);                       // RV2-2: stop() can kill it from here on, whatever we wait for
    auto fail = [&] {
        ::CloseHandle(pipe);
        ::TerminateProcess(pi.hProcess, 9);
        ::WaitForSingleObject(pi.hProcess, 1000);   // gone before stop() returns: no orphan (P6)
        publishKill(nullptr);                       // BEFORE the close: stop() duplicates under mx_
        ::CloseHandle(pi.hProcess);
        return false;
    };

    // The CONNECT. Waits on the connect, the child's death and stop() (RV2-2) -- the first one wins. The client must
    // be THIS engine (RV2-1): another one is disconnected and the wait goes on (the engine retries a busy pipe).
    const ULONGLONG deadline = ::GetTickCount64() + kConnectDeadlineMs;
    bool connected = false, stopped = false;
    while (!connected && !stopped) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) break;
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) break;
        bool c = false;
        if (::ConnectNamedPipe(pipe, &ov)) c = true;
        else {
            const DWORD e = ::GetLastError();
            if (e == ERROR_PIPE_CONNECTED) c = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE waits[3] = { ov.hEvent, pi.hProcess, quitEv };
                const DWORD w = ::WaitForMultipleObjects(quitEv ? 3 : 2, waits, FALSE, static_cast<DWORD>(deadline - now));
                if (w == WAIT_OBJECT_0 + 2) stopped = true;
                if (w != WAIT_OBJECT_0) ::CancelIoEx(pipe, &ov);
                DWORD n = 0;
                // reaped before ov leaves scope (the kernel writes into it); a connect that raced the cancel counts
                c = ::GetOverlappedResult(pipe, &ov, &n, TRUE) != FALSE;
            }
        }
        ::CloseHandle(ov.hEvent);
        if (!c || stopped) break;
        ULONG cpid = 0;
        if (::GetNamedPipeClientProcessId(pipe, &cpid) && cpid == pi.dwProcessId) { connected = true; break; }
        ++rep.rogues;                               // not our engine: out, and listen again
        ::DisconnectNamedPipe(pipe);
    }
    if (!connected || stopped) return fail();

    // The HELLO. stop() is the read's wake (TrPipe.h readMessage): it ends the wait too.
    std::string hello;
    bool broken = false, woken = false;
    if (tp::readMessage(pipe, hello, kConnectDeadlineMs, &broken, quitEv, &woken)) {
        const auto t = tp::split(hello);
        if (t.size() >= 2 && t[0] == "HELLO") {
            slotName = tp::unesc(t[1]);
            child.process = pi.hProcess;
            child.pipe = pipe;
            return true;
        }
    }
    return fail();
}

} // namespace

void TranslatorManager::workerMain() {
    Child child;
    uint32_t spawnCount = 0;
    uint64_t nextSpawn = 0, lastSpawn = 0;
    // RV2-5: failed starts IN A ROW -- no HELLO, OR dead within 30 s of it. Before, a HELLO reset the count, so an
    // engine that died 300 ms after each HELLO respawned every ~4.3 s for ever and nothing was ever said (P5).
    int      failStreak = 0;
    bool     saidTaken = false, saidRogue = false;    // RV2-1: each said once
    auto publishKill = [this](HANDLE h) {
        std::lock_guard<std::mutex> lk(mx_);
        hProcKill_ = h;
    };

    // Every producer below follows the SAME two-step: fill the queue under mx_, then ring
    // the Bridge's bell with the lock RELEASED. Waking inside the lock would hand the UI
    // thread a poll that immediately blocks on the very mutex this thread is holding --
    // a self-inflicted stall on the thread that draws.
    auto warn = [&](const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            warnings_.push_back(text);
        }
        wake();
    };

    // FILE MODE: what THIS helper incarnation was given and has not answered yet. Worker-local on
    // purpose -- only this thread writes to the pipe and reads from it, so only it can know.
    std::set<uint64_t> openJobs, openProxies;

    // A helper that went away owes an answer for everything it held. It is given one here, and
    // the sets are emptied: the next incarnation starts with NOTHING (see startFile -- a file
    // that killed one helper must not be fed to the next).
    auto failOpen = [&] {
        if (openJobs.empty() && openProxies.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (uint64_t id : openJobs)    pushDoneLocked(id, "error", kFileEngineStopped);
            for (uint64_t id : openProxies) pushProxyDoneLocked(id, kProxyEngineStopped);
        }
        openJobs.clear();
        openProxies.clear();
        wake();
    };

    // The QUEUED commands, failed in place -- when no helper is coming to take them (it keeps
    // failing to start), or when the write of one of them just broke the pipe.
    auto failCmds = [&](std::deque<FileCmd>& cmds, const char* why) {
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (const FileCmd& c : cmds) {
                if (c.kind == FileCmd::Kind::File)       { pushDoneLocked(c.id, "error", why); any = true; }
                else if (c.kind == FileCmd::Kind::Proxy) { pushProxyDoneLocked(c.id, why);     any = true; }
            }
        }
        cmds.clear();
        if (any) wake();
    };

    // One more failed start. The third in a row says so (the same sentence for both kinds) and fails what is queued
    // -- a job must not wait for an engine that is not coming -- and from the third on the wait doubles to the cap.
    auto failedStart = [&] {
        ++failStreak;
        if (failStreak == 3)
            warn(L"The SuVtitle engine keeps failing to start.");
        if (failStreak >= 3) {
            std::deque<FileCmd> cmds;
            { std::lock_guard<std::mutex> lk(mx_); cmds.swap(fileCmds_); }
            failCmds(cmds, kFileEngineFailing);
        }
        const int shift = failStreak < 3 ? 0 : std::min(failStreak - 2, 4);
        nextSpawn = ::GetTickCount64() + std::min<uint64_t>(kRespawnBackoffCapMs, static_cast<uint64_t>(kRespawnBackoffMs) << shift);
    };
    // A death, from whichever path saw it FIRST -- the loop top (the process gone), a broken read, a failed write: a
    // crash is usually seen by the read (pipe_smoke P5). Within 30 s of its HELLO it is a FAILED START (failedStart);
    // after a life the streak is over and the path's own respawn time stands (`respawnAt`, as before the fix).
    auto died = [&](uint64_t respawnAt) {
        if (::GetTickCount64() - lastSpawn < 30000) {
            failedStart();
        } else {
            failStreak = 0;
            nextSpawn = respawnAt;
        }
    };

    auto markDown = [&] {
        {
            std::lock_guard<std::mutex> lk(mx_);
            running_ = false;
            slotName_.clear();
            hProcKill_ = nullptr;
            state_.status = "loading";    // the UI shows "starting", not a stale "listening"
            stateDirty_ = true;
        }
        wake();
        failOpen();                       // FILE MODE: every death path goes through here
    };

    // One message from the helper, routed onto the ordered queue. A lambda because it is called
    // from TWO places: the normal read below, and the last-words drain when the helper has died
    // (FILE MODE: a real FDONE it managed to write before going must win over a synthesized one).
    auto dispatch = [&](const std::string& msg) {
        const auto t = tp::split(msg);
        if (t.empty()) return;
        if (t[0] == "STATE") {
            State s;
            if (tp::unpackState(t, s)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    state_ = s;
                    stateDirty_ = true;
                }
                wake();
            }
        } else if (t[0] == "LINE") {
            Msg m;
            if (tp::unpackLine(t, m.line)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();
            }
        } else if (t[0] == "FPROG" || t[0] == "FCUE" || t[0] == "FDONE" || t[0] == "FPROXYDONE") {
            // FILE MODE. Same ordered queue as the live lines (an FDONE must never overtake the
            // last FCUE it closes), but never capped -- see Msg in the header. A message whose
            // id this helper was never given is still forwarded: staleness is decided by the
            // bridge, which knows which job is current; this side only knows what is OPEN.
            Msg m;
            bool ok = false;
            if (t[0] == "FPROG") {
                m.kind = Msg::Kind::FileProg;
                ok = tp::unpackFileProg(t, m.fprog);
            } else if (t[0] == "FCUE") {
                m.kind = Msg::Kind::FileCue;
                ok = tp::unpackCue(t, m.cue);
            } else if (t[0] == "FDONE") {
                m.kind = Msg::Kind::FileDone;
                ok = tp::unpackFileDone(t, m.fdone);
                if (ok) openJobs.erase(m.fdone.jobId);
            } else {
                m.kind = Msg::Kind::ProxyDone;
                ok = tp::unpackProxyDone(t, m.pdone);
                if (ok) openProxies.erase(m.pdone.jobId);
            }
            if (ok) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();
            }
        } else if (t[0] == "END") {
            // W3: "that utterance produced nothing more" -- sent INSTEAD of a closing LINE
            // when the tail after the last committed sentence was a breath. It rides the
            // SAME queue for the same reason a PART does: overtaking the last PART of its
            // segId, it would clear a live line the next message puts straight back.
            // unpackEnd refuses an END 0 (it names no utterance), so a malformed one is
            // dropped here rather than being forwarded as a clear-everything.
            Msg m;
            m.kind = Msg::Kind::End;
            if (tp::unpackEnd(t, m.end)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();
            }
        } else if (t[0] == "PART") {
            // A LIVE LINE. Queued alongside the finished ones so the order survives;
            // everything that makes it a draft rather than a subtitle -- no speaker, no
            // scrollback, replace-in-place -- is the Bridge's business, not ours.
            Msg m;
            m.kind = Msg::Kind::Partial;
            if (tp::unpackPartial(t, m.part)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();   // the whole point of W1: a draft reaches the screen at the
                          // cadence the helper produced it, not in a 1 Hz burst
            }
        }
        // Anything else is ignored on purpose: an unknown message must never be a fault
        // on a stream protocol (TrPipe.h).
    };

    // FILE MODE: whatever the helper wrote before it died is still in the pipe -- read it out
    // (bounded, no wait) before the death is declared, or a job that FINISHED and then lost its
    // helper would be reported as failed.
    auto drainLastWords = [&] {
        if (child.pipe == INVALID_HANDLE_VALUE) return;
        for (int i = 0; i < 4096; ++i) {
            std::string last;
            if (!tp::readMessage(child.pipe, last, 0)) break;
            dispatch(last);
        }
    };

    for (;;) {
        if (quit_.load(std::memory_order_acquire)) break;

        // ---- (re)spawn ----------------------------------------------------------
        if (!child.alive()) {
            if (child.process) {
                // The child DIED. Under 30 s of life = a crash LOOP (a missing model,
                // a GPU that will not initialise): back off, or the card's slot churns
                // with a new pid every lap and flashes in the UI. EXCEPT a deliberate
                // restart (the sample-rate change): that kill is ours.
                // RV2-5: and such a death is a FAILED START, counted with the ones that never said HELLO (died).
                drainLastWords();
                markDown();
                child.kill();
                if (restartAsap_.exchange(false, std::memory_order_acq_rel))
                    nextSpawn = ::GetTickCount64();
                else
                    died(::GetTickCount64());       // it lived: a one-off, respawned at once
            }
            if (::GetTickCount64() < nextSpawn) { ::Sleep(50); continue; }

            std::string wantMode, wantPreset;
            { std::lock_guard<std::mutex> lk(mx_); wantMode = mode_; wantPreset = preset_; }
            std::string slot;
            SpawnReport rep;
            const bool started = spawnChild(spawnArgs_, spawnCount, wantMode, wantPreset, quitEv_, publishKill, child, slot, rep);
            // RV2-1: said once each -- someone on this PC is in the way, and SuVtitle went around it
            if (rep.taken && !saidTaken) {
                saidTaken = true;
                warn(L"Another program had taken the SuVtitle engine's connection name; SuVtitle used another one.");
            }
            if (rep.rogues && !saidRogue) {
                saidRogue = true;
                warn(L"Another program tried to connect to the SuVtitle engine; it was refused.");
            }
            if (!started) {
                if (quit_.load(std::memory_order_acquire)) break;   // RV2-2: stop() ended the start, nothing failed
                // FILE MODE: from the third failure on, a queued job is told so instead of
                // waiting for a helper that is not coming (the bridge shows the sentence) -- failedStart.
                failedStart();
                continue;
            }
            lastSpawn = ::GetTickCount64();   // (RV2-5: the streak is NOT reset by a HELLO any more -- by 30 s of life)
            {
                std::lock_guard<std::mutex> lk(mx_);
                running_ = true;
                slotName_ = slot;
                hProcKill_ = child.process;
                // Re-push the desired model IF the user has chosen one; otherwise stay
                // quiet and let the helper's own config stand (see mode_/preset_). The PAIR is
                // in this test for the same reason and one more: it has no command-line switch
                // at all, so this CTL is the ONLY way a respawned helper learns it back.
                ctlPending_ = !mode_.empty() || !preset_.empty()
                              || !langA_.empty() || !langB_.empty();
                stateDirty_ = true;
            }
            wake();   // the card must go live NOW, not at the next tick of a 1 Hz timer
        }

        // ---- push any pending control ------------------------------------------
        {
            tp::WireCtl c;
            bool send = false;
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (ctlPending_ || clearPending_) {
                    c.mode   = mode_;
                    c.preset = preset_;
                    c.langA  = langA_;   // "" each = "leave that half alone" (TrPipe.h)
                    c.langB  = langB_;
                    c.clear  = clearPending_;
                    ctlPending_ = false;
                    clearPending_ = false;
                    send = true;
                }
            }
            if (send && !tp::writeMessage(child.pipe, tp::packCtl(c), 2000)) {
                warn(L"The SuVtitle engine stopped responding; restarting it.");
                // Its last words FIRST (review of WC): a helper that finished a job and then
                // closed the pipe under this write must be reported as having finished it.
                drainLastWords();
                markDown();
                child.kill();
                died(::GetTickCount64());           // (RV2-5)
                continue;
            }
        }

        // ---- FILE MODE: push the queued file commands --------------------------
        // AFTER the CTL above, in the same lap: a pair the user just chose is on the wire before
        // the job started with it. Each command is written once; a FILE/FPROXY that was written
        // is OPEN until its FDONE/FPROXYDONE, and a death answers for it (failOpen).
        {
            std::deque<FileCmd> cmds;
            { std::lock_guard<std::mutex> lk(mx_); cmds.swap(fileCmds_); }
            bool broke = false;
            while (!cmds.empty()) {
                FileCmd c = std::move(cmds.front());
                cmds.pop_front();
                // WE: an FFOCUS is only for a job THIS helper is running -- one that already answered,
                // or was never written to it (it died since), would be a stray it must not see.
                if (c.kind == FileCmd::Kind::Focus && !openJobs.count(c.id)) continue;
                if (!tp::writeMessage(child.pipe, c.wire, 2000)) {
                    // The pipe broke under this command: it and everything behind it will be
                    // answered below (written or not, no helper will ever act on them).
                    cmds.push_front(std::move(c));
                    broke = true;
                    break;
                }
                if (c.kind == FileCmd::Kind::File)       openJobs.insert(c.id);
                else if (c.kind == FileCmd::Kind::Proxy) openProxies.insert(c.id);
            }
            if (broke) {
                warn(L"The SuVtitle engine stopped responding; restarting it.");
                // Its last words FIRST (review of WC): the FCUEs and the FDONE ok of a job it
                // finished before the pipe broke are real answers, read before anything is
                // made up -- then the commands it never got, then whatever is still open.
                drainLastWords();
                failCmds(cmds, kFileEngineStopped);
                markDown();
                child.kill();
                died(::GetTickCount64());           // (RV2-5)
                continue;
            }
        }

        // ---- listen ------------------------------------------------------------
        // A deadline that expires is NORMAL (nobody is talking). Only a BROKEN pipe
        // -- or the process being gone, which the loop top re-checks -- is a fault.
        std::string msg;
        bool broken = false;
        if (!tp::readMessage(child.pipe, msg, kReadPollMs, &broken)) {
            if (broken) {
                drainLastWords();
                markDown();
                child.kill();
                died(::GetTickCount64() + kRespawnBackoffMs);   // (RV2-5: the path that sees a crash first)
            }
            continue;
        }
        dispatch(msg);
    }

    if (child.alive()) {
        tp::writeMessage(child.pipe, "QUIT", kQuitDeadlineMs);
        ::WaitForSingleObject(child.process, kQuitDeadlineMs);
    }
    {
        std::lock_guard<std::mutex> lk(mx_);
        running_ = false;
        hProcKill_ = nullptr;   // BEFORE the close: stop() duplicates under mx_
    }
    child.kill();
    done_.store(true, std::memory_order_release);
}

} // namespace pbr

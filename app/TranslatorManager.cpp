// =============================================================================
// SuVtitle - app/TranslatorManager.cpp   (supervisor of SuVtitleEngine.exe)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TranslatorManager.cpp at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). See TranslatorManager.h for what changed.
// FIX ROUND 2026-10-23 (docs/REVIEW_RV2_App.md, proofs in tests/cpp/pipe_smoke.cpp): RV2-1 createEnginePipe +
// the client check in spawnChild; RV2-2 quitEv_ + publishKill; RV2-5 failedStart().
// SUVTITLE LIVE (1.1.0, 2026-10-25; proofs in tests/cpp/trmgr/trmgr_smoke.cpp [L1]-[L6]): setLive / setLiveMode /
// setLivePair / requestSources / pollLevels / pollLiveDropped, and in the worker: the live CTL, LIVE and SRCLIST written
// between the player's CTL and the file commands, re-armed at every spawn; SRC/SRCEND/INPUT/LEVELS dispatched; died()
// counting a death soon after a LIVE 1 (liveDeath).
// U3 (2026-11-02, the TRANSCRIPTION; proofs in tests/cpp/trmgr/trmgr_smoke.cpp [V1]-[V5]): startVoice (FVOICE) -- a job of
// the FILE's slot (supersedes / is superseded while unsent, cancelFile, openJobs until its FDONE, a death answers it with
// kVoiceEngineStopped); FSPK dispatched as Msg::Kind::FileSpk.
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
static const char* const kVoiceEngineStopped = "The SuVtitle engine stopped while recognising the voices.";   // U3

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
// SUVTITLE LIVE (fix round, RVB-8): deaths WHILE LISTENING later than RV2-5's 30 s -- kLiveLateMax of them within the
// window drop the wish; the second one waits kLiveLateBackoffMs before the respawn (a crash that needs a long utterance
// must not reload the models once a minute for the rest of the meeting).
static constexpr uint64_t kLiveLateWindowMs  = 600000;
static constexpr size_t   kLiveLateMax       = 3;
static constexpr uint64_t kLiveLateBackoffMs = 8000;
// SUVTITLE LIVE (fix round, RVB-7): a death this soon after a SRCLIST is counted against the list's ask (the engine was
// enumerating); the third in a row drops the ask.
static constexpr uint64_t kSrcDeathMs = 10000;
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
        // SUVTITLE LIVE: a meter with no engine behind it shows nothing (the wanted LIVE itself stays: it is a wish)
        levels_ = Levels{};
        levelsDirty_ = false;
        srcListPending_ = false;
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

// ---- SUVTITLE LIVE (1.1.0) ------------------------------------------------------------------------------------------
void TranslatorManager::setLive(bool on, const std::vector<LiveIn>& inputs) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        tp::WireLive want;
        want.on = on;
        if (on) want.inputs = inputs;
        // the user's gesture -- a NEW wish to listen -- forgives the engine its earlier live deaths; a set edited while
        // listening (a trim dragged, a strip renamed) is not a new attempt (fix round, RVB-13)
        if (on && !live_.on) { liveFailStreak_ = 0; liveLateDeaths_.clear(); }
        const auto same = [](const LiveIn& x, const LiveIn& y) {
            return x.key == y.key && x.label == y.label && x.chan == y.chan && x.gainDb == y.gainDb;
        };
        if (want.on == live_.on && want.inputs.size() == live_.inputs.size()
            && std::equal(want.inputs.begin(), want.inputs.end(), live_.inputs.begin(), same)) return;
        live_ = want;
        livePending_ = true;
    }
    cv_.notify_all();
}

bool TranslatorManager::liveWanted() const {
    std::lock_guard<std::mutex> lk(mx_);
    return live_.on;
}

void TranslatorManager::setLiveMode(const std::string& m) {
    std::string v = m;   // (setMode's defensive mapping: the old vocabulary still means what it meant)
    if      (v == "en2zh") v = "a2b";
    else if (v == "zh2en") v = "b2a";
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (liveMode_ == v) return;
        liveMode_ = v;
        liveCtlPending_ = true;
    }
    cv_.notify_all();
}

void TranslatorManager::setLivePair(const std::string& a, const std::string& b) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (liveLangA_ == a && liveLangB_ == b) return;
        liveLangA_ = a;
        liveLangB_ = b;
        liveCtlPending_ = true;
    }
    cv_.notify_all();
}

std::string TranslatorManager::liveMode() const {
    std::lock_guard<std::mutex> lk(mx_);
    return liveMode_;
}

std::string TranslatorManager::liveLangA() const {
    std::lock_guard<std::mutex> lk(mx_);
    return liveLangA_;
}

std::string TranslatorManager::liveLangB() const {
    std::lock_guard<std::mutex> lk(mx_);
    return liveLangB_;
}

void TranslatorManager::requestSources() {
    {
        std::lock_guard<std::mutex> lk(mx_);
        srcWanted_ = true;
        srcListPending_ = true;
        srcFailStreak_ = 0;      // (fix round, RVB-7: the user opening the picker again is his gesture to try again)
    }
    cv_.notify_all();
}

// Fix round (RVB-7): the picker closed -- no later engine is asked for lists it nobody reads (the watch behind them
// enumerates every ~2 s); a SRCLIST not written yet is not written.
void TranslatorManager::stopSources() {
    std::lock_guard<std::mutex> lk(mx_);
    srcWanted_ = false;
    srcListPending_ = false;
}

bool TranslatorManager::pollLiveRefused(std::string& why) {
    std::lock_guard<std::mutex> lk(mx_);
    if (liveRefused_.empty()) return false;
    why.swap(liveRefused_);
    liveRefused_.clear();
    return true;
}

bool TranslatorManager::pollLevels(Levels& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!levelsDirty_) return false;
    levelsDirty_ = false;
    out = levels_;
    return true;
}

bool TranslatorManager::pollLiveDropped() {
    std::lock_guard<std::mutex> lk(mx_);
    const bool d = liveDropped_;
    liveDropped_ = false;
    return d;
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
        // U3: an unsent FVOICE too -- the same slot, the same rule (TrPipe.h FVOICE).
        for (auto it = fileCmds_.begin(); it != fileCmds_.end(); ) {
            if (it->kind == FileCmd::Kind::File || it->kind == FileCmd::Kind::Voice) {
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

// U3: the voice pass -- startFile's twin (the engine runs it in the same job slot).
void TranslatorManager::startVoice(const tp::WireVoice& v) {
    if (v.jobId == 0 || v.path.empty() || v.spans.empty()) return;   // unpackVoice would refuse it on the other side
    bool answered = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        for (auto it = fileCmds_.begin(); it != fileCmds_.end(); ) {
            if (it->kind == FileCmd::Kind::File || it->kind == FileCmd::Kind::Voice) {
                pushDoneLocked(it->id, "cancelled", std::string());
                it = fileCmds_.erase(it);
                answered = true;
            } else if (it->kind == FileCmd::Kind::Focus) {
                it = fileCmds_.erase(it);          // aimed at an analysis that will not run now
            } else {
                ++it;
            }
        }
        if (!active_.load(std::memory_order_relaxed)) {
            pushDoneLocked(v.jobId, "error", kFileEngineAbsent);
            answered = true;
        } else {
            FileCmd c;
            c.kind = FileCmd::Kind::Voice;
            c.id   = v.jobId;
            c.wire = tp::packVoice(v);
            fileCmds_.push_back(std::move(c));
        }
    }
    cv_.notify_all();
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
            if ((it->kind == FileCmd::Kind::File || it->kind == FileCmd::Kind::Voice) && it->id == jobId) {
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
    std::set<uint64_t> openVoices;     // U3: the open jobs that are FVOICEs (a subset of openJobs: their own sentence)

    // A helper that went away owes an answer for everything it held. It is given one here, and
    // the sets are emptied: the next incarnation starts with NOTHING (see startFile -- a file
    // that killed one helper must not be fed to the next).
    auto failOpen = [&] {
        if (openJobs.empty() && openProxies.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (uint64_t id : openJobs)    pushDoneLocked(id, "error", openVoices.count(id) ? kVoiceEngineStopped : kFileEngineStopped);
            for (uint64_t id : openProxies) pushProxyDoneLocked(id, kProxyEngineStopped);
        }
        openJobs.clear();
        openProxies.clear();
        openVoices.clear();
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
                else if (c.kind == FileCmd::Kind::Voice) {
                    pushDoneLocked(c.id, "error", why == kFileEngineStopped ? kVoiceEngineStopped : why);
                    any = true;
                }
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
    // SUVTITLE LIVE (1.1.0): when THIS incarnation was last written a LIVE 1 (0 = not listening, or a LIVE 0 since, or
    // listening for 30 s already -- the loop clears it then: a later death is not "right after a LIVE").
    uint64_t lastLiveAt = 0;
    // Fix round: whether THIS incarnation was told to listen (a LIVE 1 written, no LIVE 0 since -- RVB-8 counts its later
    // deaths), the devices of that LIVE 1 (RVB-13: RV2-5's clock restarts only when they change: a trim or a name opens no
    // new capture), when it was last asked for the list (RVB-7), and which incarnation this is (RVB-6: Msg::gen).
    bool        listening = false;
    std::string liveDevs;
    uint64_t    lastSrcAt = 0;
    uint32_t    incarnation = 0;
    // RV2-5 on the live path. A capture driver that brings the engine down at LIVE 1 would otherwise do it again at
    // every respawn -- the wanted LIVE is re-sent -- and the player's analyses with it. The THIRD such death in a row
    // drops the wish (the engine comes back idle, the bridge says why); setLive(true) -- the user trying again -- forgives.
    auto liveDeath = [&] {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (++liveFailStreak_ >= 3 && live_.on) {
                live_ = tp::WireLive{};
                livePending_ = false;
                liveDropped_ = true;
                dropped = true;
            }
        }
        if (dropped) warn(L"Live translation stopped: the SuVtitle engine kept stopping when it started listening.");
    };
    // Fix round (RVB-8): a death WHILE LISTENING, later than RV2-5's 30 s (a crash that needs the first long utterance, a
    // device event): counted in a sliding window -- the number of them in it is returned; the kLiveLateMax-th drops the
    // wish (the engine comes back for the player, idle), said once.
    auto liveLateDeath = [&](uint64_t now) -> size_t {
        bool dropped = false;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            while (!liveLateDeaths_.empty() && now - liveLateDeaths_.front() >= kLiveLateWindowMs) liveLateDeaths_.pop_front();
            liveLateDeaths_.push_back(now);
            n = liveLateDeaths_.size();
            if (n >= kLiveLateMax && live_.on) {
                live_ = tp::WireLive{};
                livePending_ = false;
                liveDropped_ = true;
                liveLateDeaths_.clear();
                dropped = true;
            }
        }
        if (dropped) warn(L"Live translation stopped: the SuVtitle engine kept stopping while it listened.");
        return n;
    };
    // Fix round (RVB-7): a death right after a SRCLIST (the engine was enumerating the devices, the sessions, the
    // processes): the third in a row drops the list's ask -- a respawn must not walk into the same crash for ever.
    auto srcDeath = [&] {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (++srcFailStreak_ >= 3 && srcWanted_) {
                srcWanted_ = false;
                srcListPending_ = false;
                dropped = true;
            }
        }
        if (dropped) warn(L"The list of sources could not be made: the SuVtitle engine stopped while making it.");
    };

    // A death, from whichever path saw it FIRST -- the loop top (the process gone), a broken read, a failed write: a
    // crash is usually seen by the read (pipe_smoke P5). Within 30 s of its HELLO it is a FAILED START (failedStart);
    // after a life the streak is over and the path's own respawn time stands (`respawnAt`, as before the fix).
    // SUVTITLE LIVE: within 30 s of a LIVE 1 it is a failed start too, whatever the engine's age (liveDeath counts it).
    // Fix round: right after a SRCLIST it counts against the list's ask (RVB-7); later in a listen, in RVB-8's window.
    auto died = [&](uint64_t respawnAt) {
        const uint64_t now = ::GetTickCount64();
        if (lastSrcAt != 0 && now - lastSrcAt < kSrcDeathMs) srcDeath();
        lastSrcAt = 0;
        const bool wasListening = listening;
        listening = false;
        liveDevs.clear();
        if (lastLiveAt != 0 && now - lastLiveAt < 30000) {
            lastLiveAt = 0;
            liveDeath();
            failedStart();
            return;
        }
        lastLiveAt = 0;
        if (now - lastSpawn < 30000) {
            failedStart();
        } else {
            failStreak = 0;
            nextSpawn = respawnAt;
            if (wasListening && liveLateDeath(now) == 2) nextSpawn = std::max<uint64_t>(respawnAt, now + kLiveLateBackoffMs);
        }
    };

    auto markDown = [&] {
        {
            std::lock_guard<std::mutex> lk(mx_);
            running_ = false;
            slotName_.clear();
            hProcKill_ = nullptr;
            state_.status = "loading";    // the UI shows "starting", not a stale "listening"
            state_.live = false;          // SUVTITLE LIVE: a dead engine listens to nothing (the WISH stays: live_)
            stateDirty_ = true;
        }
        wake();
        failOpen();                       // FILE MODE: every death path goes through here
    };

    // SUVTITLE LIVE: the picker's list being assembled (SRC lines of gen `srcGen`, until its SRCEND), and how many times
    // in a row a short one was asked again. Worker-local like openJobs; reset at each spawn (gens restart with an engine).
    constexpr size_t kSrcMax = 512;    // sources in one list: a machine has tens (a bound, not a guess at the count)
    uint32_t         srcGen = 0;
    std::vector<Src> srcPend;
    bool             srcOverflow = false;
    int              srcAsks = 0;

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
                    // Fix round (RVB-5): a LIVE the engine REFUSED -- TrPipe.h: one STATE whose err says why, the live state
                    // unchanged (so not listening), the status not "error" (that one is the engine's own trouble, retried
                    // by it) -- while the wish is to listen and no newer LIVE is on its way: the wish is DROPPED here (a
                    // respawned engine is never asked again) and the bridge is told why, once.
                    if (!s.err.empty() && !s.live && s.status != "error" && live_.on && !livePending_) {
                        live_ = tp::WireLive{};
                        liveRefused_ = s.err;
                    }
                }
                wake();
            }
        } else if (t[0] == "LINE") {
            Msg m;
            m.gen = incarnation;   // (fix round, RVB-6: this engine's voices are its own)
            if (tp::unpackLine(t, m.line)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();
            }
        } else if (t[0] == "FPROG" || t[0] == "FCUE" || t[0] == "FDONE" || t[0] == "FPROXYDONE" || t[0] == "FSPK") {
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
                if (ok) { openJobs.erase(m.fdone.jobId); openVoices.erase(m.fdone.jobId); }
            } else if (t[0] == "FSPK") {         // U3: the voice pass' names, just before its FDONE
                m.kind = Msg::Kind::FileSpk;
                ok = tp::unpackSpk(t, m.spk);
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
        // ---- SUVTITLE LIVE (1.1.0): the picker's list, the inputs' states, the meters ---------------------------
        } else if (t[0] == "SRC") {
            // One line of list `gen`: a newer gen starts a new list (it supersedes every older one, whole -- TrPipe.h),
            // an older one is dropped. Bounded: past kSrcMax the list is marked short.
            Src s;
            if (tp::unpackSrc(t, s)) {
                if (s.gen > srcGen) { srcGen = s.gen; srcPend.clear(); srcOverflow = false; }
                if (s.gen == srcGen) {
                    if (srcPend.size() < kSrcMax) srcPend.push_back(std::move(s));
                    else srcOverflow = true;
                }
            }
        } else if (t[0] == "SRCEND") {
            // The list closes: delivered WHOLE, as one message. A count that does not match what arrived means lines
            // were lost (TrPipe.h: "a reader that counted fewer lost some and asks again") -- asked again, twice at
            // most in a row; a third short list is delivered as it is (a picker with most of the sources beats an
            // empty one, and a newer list replaces it whole).
            tp::WireSrcEnd e;
            if (tp::unpackSrcEnd(t, e) && e.gen >= srcGen) {
                if (e.gen > srcGen) { srcGen = e.gen; srcPend.clear(); srcOverflow = false; }
                const bool whole = !srcOverflow && srcPend.size() == e.n;
                if (!whole && srcAsks < 2) {
                    ++srcAsks;
                    {
                        std::lock_guard<std::mutex> lk(mx_);
                        srcListPending_ = true;
                    }
                    cv_.notify_all();
                } else {
                    srcAsks = 0;
                    lastSrcAt = 0;               // (fix round, RVB-7: the list was made -- this engine survives making one)
                    Msg m;
                    m.kind = Msg::Kind::Sources;
                    m.srcGen = e.gen;
                    m.srcs = std::move(srcPend);
                    {
                        std::lock_guard<std::mutex> lk(mx_);
                        srcFailStreak_ = 0;
                        pushMsgLocked(std::move(m));
                    }
                    wake();
                }
                srcPend.clear();
                srcOverflow = false;
            }
        } else if (t[0] == "INPUT") {
            // A state CHANGE (sent once each): on the ordered queue, never capped -- a lamp must not keep lying.
            Msg m;
            m.kind = Msg::Kind::Input;
            if (tp::unpackInput(t, m.input)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    pushMsgLocked(std::move(m));
                }
                wake();
            }
        } else if (t[0] == "LEVELS") {
            // ~5 Hz, droppable by design: the newest REPLACES the slot, nothing queues behind a wedged UI thread.
            Levels l;
            if (tp::unpackLevels(t, l)) {
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    levels_ = std::move(l);
                    levelsDirty_ = true;
                }
                wake();
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
                // SUVTITLE LIVE (1.1.0): the live pair, the wanted LIVE and the list's ask come back the same way -- a
                // new engine knows none of them (it starts idle, on engine.txt's live pair, with no picker open). The
                // FILE is still never re-sent (above: "one crash into a crash loop").
                liveCtlPending_ = !liveMode_.empty() || !liveLangA_.empty() || !liveLangB_.empty();
                livePending_ = live_.on;
                srcListPending_ = srcWanted_;
                stateDirty_ = true;
            }
            lastLiveAt = 0;                   // (this incarnation has not been asked to listen yet)
            listening = false;
            liveDevs.clear();
            lastSrcAt = 0;
            ++incarnation;                    // (fix round, RVB-6: its LINEs say they are this engine's)
            srcGen = 0;                       // its list gens start again
            srcPend.clear();
            srcOverflow = false;
            srcAsks = 0;
            wake();   // the card must go live NOW, not at the next tick of a 1 Hz timer
        }
        // SUVTITLE LIVE: 30 s of listening = that LIVE 1 did not kill the engine; a later death is not "right after" it.
        if (lastLiveAt != 0 && ::GetTickCount64() - lastLiveAt >= 30000) {
            lastLiveAt = 0;
            std::lock_guard<std::mutex> lk(mx_);
            liveFailStreak_ = 0;
        }

        // ---- push any pending control ------------------------------------------
        // SUVTITLE LIVE (1.1.0): up to four messages, in THIS order and all before the file commands below -- the
        // player's CTL, the LIVE CTL (the live pair, and the Clear: it acts on the live path), LIVE, SRCLIST. So the live
        // pair reaches the engine before the LIVE 1 that starts listening with it, and a LIVE 0 before the FILE that
        // resumes the player's analysis (the player's gate opens on the engine's echo, but the order holds anyway).
        {
            std::vector<std::string> out;
            std::string wantDevs;             // (fix round, RVB-13: the devices of the LIVE 1 below, if one goes out)
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (ctlPending_) {
                    tp::WireCtl c;
                    c.mode   = mode_;
                    c.preset = preset_;
                    c.langA  = langA_;   // "" each = "leave that half alone" (TrPipe.h)
                    c.langB  = langB_;
                    ctlPending_ = false;
                    out.push_back(tp::packCtl(c));
                }
                if (liveCtlPending_ || clearPending_) {
                    tp::WireCtl c;
                    c.mode   = liveMode_;
                    c.preset = preset_;  // ONE preset whatever the scope (TrPipe.h)
                    c.langA  = liveLangA_;
                    c.langB  = liveLangB_;
                    c.clear  = clearPending_;
                    c.scope  = tp::kCtlScopeLive;
                    liveCtlPending_ = false;
                    clearPending_ = false;
                    out.push_back(tp::packCtl(c));
                }
                if (livePending_) {
                    out.push_back(tp::packLive(live_));
                    livePending_ = false;
                    for (const LiveIn& in : live_.inputs) wantDevs += tp::liveLaneId(in.key, in.chan) + "\n";
                }
                if (srcListPending_) {
                    out.push_back(tp::packSrcList());
                    srcListPending_ = false;
                }
            }
            bool sent = true;
            for (const std::string& w : out) {
                if (!tp::writeMessage(child.pipe, w, 2000)) { sent = false; break; }
                if (w.rfind("LIVE\t", 0) == 0) {
                    // RV2-5's clock: a LIVE 1 that opens NEW devices (the first, or another set of lanes) starts it; one
                    // that only re-trims or renames the same lanes does not (fix round, RVB-13); a LIVE 0 stops it.
                    if (w.rfind("LIVE\t1", 0) == 0) {
                        if (!listening || wantDevs != liveDevs) lastLiveAt = ::GetTickCount64();
                        listening = true;
                        liveDevs = wantDevs;
                    } else {
                        lastLiveAt = 0;
                        listening = false;
                        liveDevs.clear();
                    }
                }
                if (w == tp::packSrcList()) lastSrcAt = ::GetTickCount64();   // (fix round, RVB-7)
            }
            if (!sent) {
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
                else if (c.kind == FileCmd::Kind::Voice) { openJobs.insert(c.id); openVoices.insert(c.id); }
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

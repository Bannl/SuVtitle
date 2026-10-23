// =============================================================================
// SuVtitle - app/TranslatorManager.h   (the ENGINE SUPERVISOR: SuVtitleEngine.exe, its pipe, its restarts)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TranslatorManager.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: start() takes no fabric rate (no fabric in
// SuVtitle), the spawn names SuVtitleEngine.exe, a test instance's engine runs on the CPU (--cpu), the synthesized
// sentences say "the SuVtitle engine". Comments made ASCII.
// FIX ROUND 2026-10-23 (docs/REVIEW_RV2_App.md): RV2-1 the engine's pipe is the supervisor's ALONE (first instance,
// local clients only, a DACL granting this user only, the connecting client must be the spawned engine; a taken name
// = the next one at once); RV2-2 stop() interrupts a start in progress (quitEv_ in every wait of it, the kill handle
// published right after CreateProcess); RV2-5 an engine that dies within 30 s of its HELLO counts as a failed start
// (the same "keeps failing to start" sentence at the third in a row, and a growing wait). See TranslatorManager.cpp.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// The tray's supervisor for AudioPBTranslator.exe. FxBusManager's little brother:
// same shape (spawn, named pipe, watchdog, respawn, re-push the desired model),
// minus everything that made that one complicated.
//
// THREE deliberate differences from FxBusManager, each with a reason:
//   * ONE child, not N. There is one Translator card, so there is no name map, no
//     duplicate check and no rename-carries-the-chain dance.
//   * The pipe is a STREAM, not a request/reply. The helper SPEAKS -- subtitles
//     arrive when someone talks -- so the worker alternates "send any queued CTL"
//     and "read with a short deadline", and a deadline that expires is the normal
//     resting state, not a wedge. (See TrPipe.h for why that had to change.)
//   * The watchdog signal is therefore the PROCESS DYING, never a missing reply.
//
// THREADING: the public API is UI-THREAD ONLY, exactly like FxBusManager. One
// worker thread owns the process handle and the pipe end to end; the UI thread
// only touches the queues under mx_, and the Bridge drains them on the WAKE below
// (with uiTick left in as a safety net).
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TrPipe.h"

namespace pbr {

class TranslatorManager {
public:
    using State   = pbt::trpipe::WireState;
    using Line    = pbt::trpipe::WireLine;
    using Partial = pbt::trpipe::WirePartial;
    using End     = pbt::trpipe::WireEnd;
    // FILE MODE (wave WC, 2026-10-10): what the helper says about a recording it subtitles.
    using FileProg  = pbt::trpipe::WireFileProg;
    using Cue       = pbt::trpipe::WireCue;
    using FileDone  = pbt::trpipe::WireFileDone;
    using ProxyDone = pbt::trpipe::WireProxyDone;

    // One thing the helper said. Finished lines, LIVE lines and the END of an utterance
    // share ONE queue, and that is a correctness requirement rather than tidiness: a
    // draft must never reach the UI after the paragraph that replaces it, and two queues
    // drained one after the other reorder them the first time both have something in at
    // the same instant. (The helper's own outbox is single-queued for exactly this
    // reason -- TrPipe.h.) W3 made the ordering sharper still: an END overtaking the last
    // PART of its segId would clear a live line the next message puts straight back.
    //
    // (!) A KIND, NOT A BOOL. `isPartial` was a bool because there were exactly two things
    // on this queue. END is a THIRD, and a bool plus another bool is how a reader ends up
    // asking "is it a partial?" of a message that is neither -- and getting `false`, which
    // reads as "it is a line". One field, three values, no third state to forget.
    //
    // FILE MODE added four kinds, on the SAME queue for the same reason: a job's FDONE must never
    // reach the UI before the last FCUE it closes. They differ from the live three in ONE rule --
    // they are NEVER dropped by the cap below. A live line is superseded by design and the oldest
    // is the cheapest thing to lose when the UI thread is wedged; a cue is a subtitle of a FILE,
    // and losing one leaves a hole in the .srt that no later message will ever fill.
    struct Msg {
        enum class Kind { Line, Partial, End, FileProg, FileCue, FileDone, ProxyDone };
        Kind      kind = Kind::Line;
        Line      line;      // Kind::Line
        Partial   part;      // Kind::Partial
        End       end;       // Kind::End -- one field (the segId), and that is the point
        FileProg  fprog;     // Kind::FileProg
        Cue       cue;       // Kind::FileCue
        FileDone  fdone;     // Kind::FileDone  (real, or SYNTHESIZED -- see startFile)
        ProxyDone pdone;     // Kind::ProxyDone (ditto)
        // WE (player v2): true when this FDONE/FPROXYDONE was MADE UP here (the helper died, never
        // got the command, was never there) rather than said by the helper. The player's partial
        // cache depends on it: the helper's own last word vouches for what it reported before it,
        // a synthesized one vouches for nothing (ui/TrPlayer.h, TRUSTED RANGES).
        bool      synth = false;
    };

    TranslatorManager() = default;
    ~TranslatorManager() { stop(); if (quitEv_) ::CloseHandle(quitEv_); }

    TranslatorManager(const TranslatorManager&) = delete;
    TranslatorManager& operator=(const TranslatorManager&) = delete;

    // Idempotent. Spawning is asynchronous: the card shows up in the graph when the
    // helper claims its fabric slot, exactly like an FX bus.
    // SuVtitle (2026-10-23): no fabric, so no rate -- AudioPB's start(fabricRate), reconcile(rate) and the
    // sample-rate restart(rate) went with it. The engine is started once, at the window's attach, and stays.
    bool start();
    void stop();
    // SuVtitle: switches added to EVERY spawn's command line (set once, before start()). The app passes "--cpu" for a
    // test instance: a test never takes the user's GPU (SPLIT_PLAN section 4). Same rule as setWake: written before
    // any worker exists, read by the worker without a lock.
    void setSpawnArgs(const std::wstring& extra) { spawnArgs_ = extra; }

    bool        active()   const { return active_.load(std::memory_order_relaxed); }
    bool        running()  const;
    std::string slotName() const;

    // The desired model, re-pushed on every (re)spawn -- the same contract
    // FxBusManager's `desired` chain has, for the same reason.
    void setMode(const std::string& mode);
    void setPreset(const std::string& preset);
    // The desired PAIR (wave W2). Same contract as mode/preset -- EMPTY means "the helper's
    // own translator.txt decides" -- and for the same reason: the helper owns that file, so a
    // tray that shipped a pair at every spawn would silently undo the user's persisted choice
    // on every restart.
    //
    // BOTH HALVES TRAVEL TOGETHER, always. A pair is ONE fact: pushing half of it would leave
    // the helper briefly holding "en / en", which it refuses outright (main_translator.cpp
    // checks wantA != wantB) -- so a half-push is not a smaller change, it is a dropped one.
    void setPair(const std::string& a, const std::string& b);
    void askClear();

    // ---- FILE MODE (wave WC) -------------------------------------------------
    // Subtitle a RECORDING (TrPipe.h, "FILE MODE"). UI THREAD, like the rest of the API.
    //
    // QUEUED, and sent by the worker AFTER any pending CTL in the same lap -- so a pair the user
    // just picked reaches the helper before the job that was started with it (the job carries
    // its own snapshot of the pair anyway; the ordering keeps the two from ever disagreeing).
    //
    // (!) NEVER RE-SENT AFTER A RESPAWN. A helper that dies with a job open gets a SYNTHESIZED
    // `FDONE <id> error "The Translator engine stopped during the analysis."` on the queue, and
    // the next incarnation never hears of that FILE. That is deliberate: the likeliest reason a
    // helper dies in the middle of a file is THE FILE (a decoder choking on it), and re-sending
    // it would turn one crash into a crash loop. The user re-runs it if they want to.
    //
    // EXACTLY ONE FDONE per startFile() and ONE FPROXYDONE per requestProxy(), real or
    // synthesized -- which is the only thing the bridge needs to know to never wait forever.
    void startFile(const pbt::trpipe::WireFile& f);     // an UNSENT older FILE is dropped: FDONE cancelled
    // A FILE's id or an FPROXY's (one counter, so an id names one of them): unsent -> dropped and
    // answered "cancelled" here; sent -> FCANCEL, and the helper answers.
    void cancelFile(uint64_t id);
    // An UNSENT older FPROXY is dropped with FPROXYDONE err="superseded" -- the same answer the
    // helper gives a running one a new request supersedes.
    void requestProxy(const pbt::trpipe::WireProxy& p);
    // WE (player v2): FFOCUS -- the playhead moved, the job should analyse from there (TrPipe.h,
    // FILE MODE v2). A FILE still UNSENT simply has its focusMs patched (one message instead of
    // two); an FFOCUS still unsent is REPLACED (the latest playhead wins: a scrub is one message);
    // and an FFOCUS is never written for a job this helper is not running (answered, or never sent,
    // or a dead incarnation's) -- the worker checks that at the moment it writes.
    void focusFile(uint64_t jobId, uint64_t ms);

    std::string mode()   const;
    std::string preset() const;
    std::string langA()  const;
    std::string langB()  const;

    // ---- the WAKE (wave W1) ------------------------------------------------
    // Called by the WORKER thread, with no lock held, immediately after anything the UI
    // would want to see lands in a queue: a LINE, a PART, a STATE change, a warning.
    //
    // (!) WHY IT EXISTS. Before it, the Bridge drained these queues from uiTick alone -- a
    // 1 Hz timer -- so a subtitle waited 0-1000 ms here, and a run of live drafts arrived
    // in one BURST every second instead of at the cadence the helper produced them. That
    // burst, not the inference, is what made the live line read as jerky.
    //
    // The callback must be cheap and thread-safe: the Bridge posts ONE window message and
    // returns (it coalesces, so a burst of PARTs costs one post). Set it ONCE, before
    // start(); the worker reads it without a lock, which is only sound because it never
    // changes after a worker exists.
    void setWake(std::function<void()> fn) { wake_ = std::move(fn); }

    // ---- drained by the Bridge (on the wake, and from uiTick as a safety net) ----
    // True + fills `out` when the STATE changed since the last poll.
    bool pollState(State& out);
    std::vector<Msg>          pollMessages();
    std::vector<std::wstring> pollWarnings();

    static bool isTranslatorName(const std::wstring& fabricName);   // "tr:" prefix

private:
    void workerMain();
    void wake() const { if (wake_) wake_(); }   // worker thread, NEVER under mx_

    // One queued file command, already packed. `id` is the jobId (FILE/FCANCEL) or the proxy's
    // correlation id (FPROXY) -- kept apart from the wire string so cancelFile() can find an
    // unsent FILE without parsing anything.
    struct FileCmd {
        enum class Kind { File, Cancel, Proxy, Focus };
        Kind        kind = Kind::File;
        uint64_t    id = 0;
        std::string wire;
        pbt::trpipe::WireFile file;    // Kind::File: kept so an FFOCUS can patch it while unsent
    };
    static bool isLiveKind(Msg::Kind k) {
        return k == Msg::Kind::Line || k == Msg::Kind::Partial || k == Msg::Kind::End;
    }
    // mx_ HELD. The one door into msgs_: the cap applies to LIVE kinds only (Msg, above).
    void pushMsgLocked(Msg&& m);
    // mx_ HELD. The synthesized answers -- built in one place so a real one and a made-up one
    // can never differ in shape.
    void pushDoneLocked(uint64_t jobId, const char* result, const std::string& err);
    void pushProxyDoneLocked(uint64_t id, const std::string& err);

    std::function<void()> wake_;      // see setWake: written once, before any worker exists
    std::wstring          spawnArgs_; // see setSpawnArgs: the same rule
    std::atomic<bool>     active_{ false };
    std::atomic<bool>     restartAsap_{ false };

    mutable std::mutex      mx_;
    std::condition_variable cv_;
    bool                    running_ = false;
    std::string             slotName_;
    // EMPTY means "the helper's own translator.txt decides". The helper owns that file,
    // so the tray must not hand it a --mode/--preset at every spawn: that would silently
    // reset the user's choice on every tray restart. Once the user picks something, the
    // value lands here and travels on the spawn AND on the CTL.
    //
    // langA_/langB_ obey the same rule but travel ON THE CTL ONLY: the helper has no
    // --langa/--langb switch (it reads the pair from translator.txt, or takes one from a CTL),
    // so the re-push after a respawn is the pending CTL below rather than a command line.
    std::string             mode_, preset_, langA_, langB_;
    bool                    ctlPending_ = false;   // nothing to push until the user chooses
    bool                    clearPending_ = false;
    State                   state_;
    bool                    stateDirty_ = false;
    std::deque<Msg>         msgs_;
    size_t                  liveCount_ = 0;        // Line/Partial/End currently in msgs_ (the capped ones)
    std::deque<FileCmd>     fileCmds_;             // FILE/FCANCEL/FPROXY waiting for the worker
    std::vector<std::wstring> warnings_;
    HANDLE                  hProcKill_ = nullptr;  // stop()'s emergency brake (RV2-2: set from CreateProcess on)
    // RV2-2 (2026-10-23): manual-reset, SET by stop(). The start's connect and HELLO waits wait on it too, so a close
    // during a start returns at once (before: up to 8 s + 8 s on the UI thread, RV-2's P6). Created by the first start().
    HANDLE                  quitEv_ = nullptr;

    std::thread       worker_;
    std::atomic<bool> quit_{ false };
    std::atomic<bool> done_{ false };
    uint32_t          spawnSeq_ = 0;
};

} // namespace pbr

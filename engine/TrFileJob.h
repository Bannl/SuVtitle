// =============================================================================
// SuVtitle - engine/TrFileJob.h   (FILE MODE, wave WB: the job's pure logic)
//
// SUVTITLE SPLIT (2026-10-23): ported VERBATIM from AudioPB's PatchBus repo, translator/TrFileJob.h at commit
// 641f1aa (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Only this header and the comments' non-ASCII
// characters changed (house rule: ASCII-only C++).
//
// Everything the FILE job decides that does not need a server, a pipe or a thread: where a
// window starts and ends, which of whisper's words are final, how they become subtitles.
// Pure functions and two small classes, so the selftest proves every rule offline and the
// state machine in main_translator.cpp only sequences them.
//
// THE WINDOWING (measured 10/10, WB study -- see the phase-1 plan):
//   * windows are cut at PAUSES, up to 30 s: the latest pause of >= 0.3 s that starts between
//     s+8 and s+29.5 s ends the window. whisper.cpp already commits to the last complete
//     segment INSIDE a request, so a window that ends mid-speech costs it a second encoder
//     pass (3 runs instead of 2, +18 % time) whose output our commit rule would then throw
//     away; a pause-ended window is complete, and everything in it is committed. On a
//     bilingual meeting: 76-88 % word coverage vs 70 % for fixed 30 s windows (turbo);
//   * a window with no pause in sight (continuous speech or music) is cut at 30 s and keeps
//     the classic commit-to-last-complete-segment rule, and the next window starts at the last
//     committed word (its repeated head is dropped: dropRepeatedHead);
//   * silence costs nothing: a window starts at the first ACTIVE hop (0.2 s pre-roll), and a
//     stretch with no active hop is never sent to a model;
//   * the first window of a job prefers a pause between 5 and 12 s, so the first subtitle is
//     on screen ~1 s after the models are up, not after a whole 30 s decode.
//
// FILE MODE v2 (wave WD, "follow the playhead" -- TrPipe.h): the job no longer runs once from 0.
// It runs RUNS -- from a starting cursor to a LIMIT (the start of the next stretch already
// analysed, or the end of the file) -- in an order the playhead decides. What changes here:
//   * the energy track is filled REGION by REGION (a seek leaves hops unknown; unknown is never
//     "active", and the noise floor only counts each hop once);
//   * planWindow takes the run's limit: a window never crosses it -- it ends there, kLimit;
//   * A JOIN IN SPEECH (review P1). Where two runs meet inside speech, each window HEARS past the
//     join and keeps only the words that START on its own side (de-dup by time): a run that starts
//     mid-speech is given a PRE-ROLL (whisper hears 2 s before its cursor, words starting before it
//     are dropped), and a window ending at a limit the speech runs across a POST-ROLL (2 s past the
//     limit, words starting from it on dropped). A sentence across the join is then shown in two
//     cues, its head on one side and its tail on the other, every word heard whole -- where v2
//     first DROPPED the head's whole cue. Only when there is no room for a post-roll (a 30 s window)
//     are the words heard up to the limit flagged `cut`, and only THOSE words are dropped;
//   * a run's cues stay INSIDE the run: none starts before its first cursor, none ends after its
//     limit (CueBuilder::beginRun) -- which is what keeps cues of different runs, and of the
//     tray's `have`, from ever overlapping;
//   * runStartCursor: where a run that starts at the playhead begins -- at a pause shortly before
//     it (3 s, then 8 s), never inside an analysed stretch; mid-speech only when there is none.
// A job that is ONE run from 0 with no limit (a v1 FILE) goes through exactly the v1 code paths:
// same windows (asserted against windows recorded from the v1 binary), same cues.
// =============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "TrEngine.h"

namespace pbt {

constexpr uint32_t kFileRate      = 16000;
constexpr int      kFileHop       = 480;       // 30 ms, the live VAD's hop
constexpr double   kFileWinMaxSec = 30.0;      // whisper's window
constexpr double   kFileWinMinSec = 8.0;       // the earliest a pause may end a window
constexpr double   kFileFirstMin  = 5.0;       // ... and for the job's FIRST window
constexpr double   kFileFirstMax  = 12.0;
constexpr double   kFilePauseSec  = 0.30;      // a pause = this much with no active hop
constexpr double   kFilePreroll   = 0.20;      // audio kept before the first active hop
constexpr double   kFileTailPad   = 0.25;      // audio kept after a pause starts
constexpr double   kFileHardBack  = 1.0;       // a hard cut commits only what ends 1 s before it
constexpr double   kFileDigitalSilenceDb = -100.0;   // hops below: digital silence, kept out of the floor
constexpr double   kFileActiveCeilDb     = -40.0;    // the active threshold never goes above this
// ---- v2 (WD) -----------------------------------------------------------------------------------
constexpr float    kFileUnknownDb   = -999.0f;  // a hop of a region never decoded (below digital silence)
constexpr double   kFileRunLookback = 3.0;      // a run at the playhead may start this much before it ...
constexpr double   kFileRunLookbackFar = 8.0;   // ... or this much, on a pause, when none lies within 3 s
constexpr double   kFileCutTolSec   = 0.30;     // a word ending this close to a limit the speech crosses is cut
constexpr double   kFileJoinRollSec = 2.0;      // P1: what a window hears past a join in speech

// ---------------------------------------------------------------------------
// The energy track: hop RMS in dB over the WHOLE 16 kHz stream (1.4 MB for 3 h), and its
// floor -- the 10th percentile of every hop so far. "Active" is deliberately generous
// (floor + 6 dB, never below -62 dBFS): a missed pause costs a little time, a pause invented
// inside a word costs a broken word, and a stretch wrongly called silence costs SPEECH.
// ---------------------------------------------------------------------------
class EnergyTrack {
public:
    void   reset();
    void   push(const float* x, size_t n);    // append 16 kHz audio (a partial hop is carried)
    // v2 (WD): the audio at stream SAMPLE `at`. Contiguous with the last push = push(); anywhere
    // else (a seek) drops the partial hop carried and starts at the next hop boundary. A hop
    // already known is not counted twice in the floor.
    void   pushAt(uint64_t at, const float* x, size_t n);
    size_t hops() const { return db_.size(); }
    float  db(size_t h) const { return h < db_.size() ? db_[h] : -180.0f; }
    bool   known(size_t h) const { return h < db_.size() && db_[h] > kFileUnknownDb; }
    size_t knownHops() const { return histAll_; }
    double floorDb() const;
    double activeDb() const;
    bool   active(size_t h) const { return h < db_.size() && db_[h] > activeDb(); }
    double peakDb(double t0, double t1) const;           // loudest hop in [t0, t1) seconds
    double inactiveSec(double t0, double t1) const;      // how much of [t0, t1) is not active
    static size_t hopAt(double t) { return t <= 0.0 ? 0 : (size_t)(t * kFileRate / kFileHop); }
    static double timeOf(size_t h) { return (double)h * kFileHop / kFileRate; }
private:
    void   storeHop(size_t h, float d);
    std::vector<float>    db_;
    std::vector<uint32_t> hist_ = std::vector<uint32_t>(241, 0);   // 0.5 dB bins, -120..0 dBFS
    size_t                histN_ = 0;
    size_t                histAll_ = 0;          // hops known (digital silence included)
    std::vector<float>    carry_;
    uint64_t              next_ = 0;             // the stream sample the next contiguous push starts at
    uint64_t              carryAt_ = 0;          // the stream sample carry_[0] is
};

// ---------------------------------------------------------------------------
// Where the next window goes. Positions are 16 kHz SAMPLES from media time 0.
// ---------------------------------------------------------------------------
struct WindowPlan {
    uint64_t s = 0, e = 0;
    // kLimit (v2): the window ends at the run's LIMIT -- the start of a stretch already analysed.
    enum Why { kPause, kHard, kEof, kLimit } why = kHard;
    // v2, review P1 -- A JOIN IN SPEECH (see the header): what whisper HEARS when it is more than
    // [s, e) -- [hs, s) a pre-roll, [e, he) a post-roll -- and the words that are this window's: those
    // that START in [keepFrom, e) (the post-roll's are the stretch beyond's). `rolled` false = the v1
    // window exactly: whisper hears [s, e), every word is kept.
    bool     rolled = false;
    uint64_t hs = 0, he = 0, keepFrom = 0;
    uint64_t audioS() const { return rolled ? hs : s; }
    uint64_t audioE() const { return rolled && he > e ? he : e; }
    bool     postRolled() const { return rolled && he > e; }
};
enum class PlanVerdict {
    Plan,           // `out` is a window to transcribe
    NeedAudio,      // decode more first (nothing decided)
    SkipSilence,    // everything from the cursor to `skipTo` is silent: move the cursor there
    Done            // EOF (or the run's limit) and nothing active left
};
// `limit` (v2): no window reaches past it (a run's end; ~0 = none, the v1 behaviour exactly).
PlanVerdict planWindow(const EnergyTrack& en, uint64_t cursor, uint64_t avail, bool eof,
                       bool first, WindowPlan& out, uint64_t& skipTo, uint64_t limit = ~0ull);

// v2 (WD): where a run that starts at the playhead P begins. The latest PAUSE (>= kFilePauseSec of
// inactive hops) touching [lo, P] -- its start, or `lo` -- so the first window opens on a breath and
// not in the middle of a word; else a shorter one (>= 0.12 s); else (review P1) the latest such
// pause in [loFar, lo) -- a sentence begun a few seconds before the playhead is heard WHOLE; else
// the quietest hop in [lo, P], and `midSpeech` says so (the run's first window gets a pre-roll).
// Never before `loFar` (the caller passes max(the gap's start, P - lookback): never inside an
// analysed stretch). Samples.
uint64_t runStartCursor(const EnergyTrack& en, uint64_t P, uint64_t lo, uint64_t loFar, bool& midSpeech);
inline uint64_t runStartCursor(const EnergyTrack& en, uint64_t P, uint64_t lo) {
    bool mid = false;
    return runStartCursor(en, P, lo, lo, mid);
}

// ---------------------------------------------------------------------------
// What a window's decode commits.
// ---------------------------------------------------------------------------
struct FileWord {
    double      start = 0.0, end = 0.0;    // MEDIA seconds
    std::string text;
    std::string lang;                      // the language whisper detected for its WINDOW (a code)
    bool        cut = false;               // v2: heard only up to a run's limit -- the word is dropped
    bool        timed = false;             // WF: its times come from DTW (accurate), not t0/t1
};
struct CommitInput {
    const AsrResult*   asr = nullptr;
    WindowPlan         plan;
    const EnergyTrack* en = nullptr;
    std::string        prompt;              // what the decode was sent (echo detection)
    // WF: the job's PAIR. A segment WRITTEN in a script neither language uses ("U+0410,041F,041B,041E,0414,0418,0421,041C,0415,041D,0422,042B" in a
    // fr<->en film, whatever whisper called the window) is not this job's speech. "" = no check.
    std::string        langA, langB;
    // WF (review P1): a THIRD language whose script is allowed too -- the window's own language, when a
    // confident decode outside the pair is kept as real speech (1.0.6's route: translated into B).
    std::string        langC;
};

// ---------------------------------------------------------------------------
// WAVE WF -- whisper's TOKENS become WORDS before anything else sees them.
//
// whisper-server's verbose_json lists TOKENS under "words": " Me" + "ep", " c" + "'" + "est",
// " Etes" + "-" + "vous", " croy" + "ant" -- and a segment may even END inside a word ("...en c" |
// "royant atterrir", measured on the film). Every rule downstream (gaps, limits, the cue split)
// used to see those pieces, and cut subtitles INSIDE words. Now: a token with no leading space
// continues the word before it (any space-separated script, Korean included -- review L2; never
// across Han / kana / Thai, written without spaces, where each token stays a unit as before), but
// not across a segment boundary after a full stop (review L6: "ciel." | "Bon"); a French elision
// (c' l' d' j' m' n' s' t' qu' jusqu' lorsqu' puisqu' quoiqu') and a hyphen written against its
// letters ("Etes-" + " vous", " va" + " -t-il") glue even across a space -- the hyphen only before a
// LOWERCASE letter (review P4: "si-" + " Non !" is an interrupted word and the next speaker);
// punctuation rides on its word; an OPENING mark (" ( [ " ? ! ...) or a dialogue dash written
// after a space opens the next.
//
// TIMES (TrEngine.h, AsrWord::dtw): with DTW a word ENDS at its last letter token's t_dtw and STARTS
// where the word before it ended (the alignment path is continuous), but never earlier than its
// first token's end less that token's plausible length (a pause, music, or a new DTW pass sits
// before it) nor than its own end less the longest the whole word can take (over music the path
// may enter a window's first token anywhere); the first word of a window starts that way too.
// Without DTW (a server that ran without it): t0 of the first token, t1 of the last -- the pre-WF
// times. `timed` says which.
// ---------------------------------------------------------------------------
struct WindowWord {
    FileWord w;
    size_t   seg = 0;                       // the segment of its FIRST token (commit decisions)
};
// Every segment's tokens, in order, as words. `s0` = the media second the request's audio starts at.
std::vector<WindowWord> windowWords(const AsrResult& a, double s0);
// The rule alone: would `tok` continue the word whose text so far is `cur`? (the selftest's handle)
bool tokenContinuesWord(const std::string& cur, const std::string& tok);

// WF, D -- what is NEVER dialogue, at any level (the file job's structural filters):
//  * a SOUND TAG whisper writes over music: bracketed annotations (isAnnotationOnly) and also BARE
//    ones WITHOUT sentence punctuation -- an ALL-CAPS line of at most eight words holding a tag WORD of
//    the vocabulary, in any language ("U+0422,0420,0415,0412,041E,0416,041D,0410,042F U+041C,0423,0417,042B,041A,0410", "U+0410,041F,041B,041E,0414,0418,0421,041C,0415,041D,0422,042B", "MUSIQUE DOUCE", "TRIPT
//    CREDITS"), or a line that IS a tag word in any case ("Music", "Musique", "U+97F3,4E50"). (Review L5: a
//    spoken "Musique !" or "CHEERS!" carries sentence punctuation and is dialogue; exact words, no stems.)
//  * a CREDITS line from whisper's training data, in the 13 pair languages ("Sous-titrage Societe
//    Radio-Canada", "Transcription by ESO", "Untertitel im Auftrag des ZDF", "U+5B57,5E55,7531Amara.orgU+793E,533A,63D0,4F9B",
//    "U+0420,0435,0434,0430,043A,0442,043E,0440 U+0441,0443,0431,0442,0438,0442,0440,043E,0432 U+0410.U+0421,0435,043C,043A,0438,043D"...) -- ANCHORED (review C2): the line STARTS with the phrase and adds
//    at most a few words, so "Thanks for watching the kids, I owe you one." is dialogue; "Amara.org"
//    anywhere. creditsHallucination says 1 for a credits line in any context, 2 for one a person could
//    also say as a whole line ("Thank you for watching.", "U+041F,0440,043E,0434,043E,043B,0436,0435,043D,0438,0435 U+0441,043B,0435,0434,0443,0435,0442...", "U+3054,8996,8074,3042,308A,304C,3068,3046
//    U+3054,3056,3044,307E,3057,305F"): commitWindow drops those only when they stand ALONE in their window or the window's
//    language detection is unsure (< kUnsureLangProb) -- whisper's credits come over music, with nothing else said.
//    SuVtitle 1.1.0 (RESUME 11): the line and the phrases are compared FOLDED to Simplified (hanFoldSimplified: one
//    row per Chinese phrase, both scripts), and a CHAIN of phrases is one line ("Thank you for watching. Please
//    subscribe.": the last phrase's few extra words close it; a credits phrase anywhere makes it kind 1).
//    The rest of a tag / credits line carried on MID-WORD into the next segment ("...visit www.fema." |
//    "org, visit www.fema.org", measured on the film) is the same line: dropped with it, and never what
//    makes it "not alone". The quiet-gated caption list of the live path does not see them: a film plays
//    them LOUD;
//  * a line written in a SCRIPT none of the languages allowed (the pair, and a kept third one) uses.
bool isSoundTag(const std::string& text);
int  creditsHallucination(const std::string& text);
inline bool isCreditsHallucination(const std::string& text) { return creditsHallucination(text) != 0; }
// The detection probability under which a decode is UNSURE of its language (the credits rule above; the live
// phantom net below): over music and room tone, whisper's language detection is a guess.
inline constexpr double kUnsureLangProb = 0.8;
// SuVtitle 1.1.0 (RESUME 11) -- WHISPER'S LOOP INSIDE ONE LINE: a sentence said twice or more in a row, and nothing
// else ("I don't know what I'm saying. I don't know what I'm saying.", U+6211,4E0D,77E5,9053,6211,5728,8AAA,4EC0,9EBC twice). Returns ONE repeat, as
// written (its punctuation kept), or "" when `text` is not that: its atoms (normWords) must repeat with a SHORTEST
// period of kRepeatMinAtoms or more, at least twice over (the last repeat may be cut short) -- so "non non", "no, no,
// no" and U+597D,597D,597D,597D, a person insisting, are never one. kRepeatMinAtoms is commitWindow's own rule ("a segment of four
// tokens or more that says exactly what an earlier one said"), inside one line.
inline constexpr size_t kRepeatMinAtoms = 4;
std::string wholeLineRepeat(const std::string& text);
// ... and THE LIVE PHANTOM NET: this section's vocabulary on a LIVE line, which has no window around it to lend it a
// context -- what the live final knows of how its words were HEARD stands in for one. The user's first live session
// (26/10/2026, French <-> Taiwanese) put two lines nobody said on screen: "Thank you for watching. Please subscribe."
// off a mic and "I don't know what I'm saying." twice off a browser -- English, outside the pair, at a normal level.
struct HeardHow {
    double peakDbfs    = 0.0;     // the level the hallucination filter judged (hop RMS, dBFS)
    double confidence  = 0.0;     // asrConfidence (0 = no segment, no verdict)
    double langProb    = -1.0;    // whisper's language detection probability (< 0 = not reported)
    bool   outsidePair = false;   // the line's language is neither half of the session's pair
};
// Why a line was heard with DOUBT, "" when it was not: a language OUTSIDE the pair, the detection unsure (under
// kUnsureLangProb), quiet (under kQuietGateDbfs) or whisper's own confidence failed (under kLowConfidenceLogp).
std::string heardWithDoubt(const HeardHow& h);
struct PhantomVerdict {
    bool        junk = false;     // not a line
    bool        loop = false;     // whisper's loop: `text` is ONE of its repeats
    std::string text;             // what to show: the line as given, unless `loop`
    std::string why;              // for the log, when junk
};
// A CREDITS line (creditsHallucination 1) is never speech; a STOCK OUTRO a person could also say (2) is junk when
// heard with doubt; so is a LOOP -- and a loop heard cleanly is said ONCE. Anything else comes back as it was.
PhantomVerdict livePhantomNet(const std::string& text, const HeardHow& h);
bool foreignScript(const std::string& text, const std::string& langA, const std::string& langB,
                   const std::string& langC = std::string());
// Is `lang` (whisper's code) written in the Latin alphabet alone? (review P1: a forced decode that
// handed back the first one's very words is that language's speech only when the script agrees)
bool latinScriptLang(const std::string& lang);
// ... and an annotation OPENING or CLOSING a line of speech -- "Meep meep! *Gasp*", "[Musique] Bon" --
// is removed from it (words wrapped in [..] (..) *..* U+FF08..U+FF09 U+3010..U+3011, at most 8); in the middle of a line a
// wrapped word may be emphasis ("I like *this* one") and stays.
void dropEdgeAnnotations(std::vector<FileWord>& words);
struct CommitVerdict {
    std::vector<FileWord> words;            // final, in order, junk removed
    uint64_t              nextCursor = 0;   // where the next window may start (samples)
    int                   junkSegments = 0; // segments the filters removed (for the log)
    int                   repeats = 0;      // segments that repeated an earlier one of the window
    int                   foreign = 0;      // WF: segments written in a script outside the pair
    int                   tags = 0;         // WF: sound tags / credits lines removed
    bool                  timed = false;    // WF: the window's words carry DTW times
    bool                  heldBack = false; // WF: a hard cut committed nothing and comes back to the speech
    bool                  sawJunk = false;  // a reason to drop the prompt's tail next time
    // THE DECODE LOOPED (measured 10/10, turbo, a bilingual window prompted in English: seven
    // segments of "U+4ECE,4E2D,56FD,5E02,573A,6765,770B..." in place of nine different utterances). Two or more repeated
    // segments, or a whole window whose word trigrams are less than half unique. The caller
    // decodes the window again -- unprompted, then on a shorter window -- before accepting it.
    bool                  degenerate = false;
    // F-RESUME: where whisper WROTE something it is judged to have said (hearCheck). heardReal = the media-second
    // spans of every word holding a letter or a digit, in a segment that is neither dropped as whisper's FAILURE
    // (junk, a repeated segment, a credits line) nor a sound tag: the kept words, a line in another script, the
    // words a join gives to the stretch on its other side, words a de-dup net later drops as heard twice.
    // heardNonSpeech = the OWN span of every segment that SAYS "not speech": a sound tag, punctuation alone.
    // heardTags = the same for an EXPLICIT sound tag only -- bracketed, parenthesised or starred ("[Applause]",
    // "(rires)"), or a music note -- never punctuation alone (F-RESUME round 3, review LOW 4 / F6).
    std::vector<std::pair<double, double>> heardReal, heardNonSpeech, heardTags;
};
CommitVerdict commitWindow(const CommitInput& in);

// A degenerate window, cut shorter: its LARGEST inner pause (>= kFilePauseSec, starting in the
// middle half of the window) becomes its end. false = no such pause.
bool shrinkToInnerPause(const EnergyTrack& en, WindowPlan& plan);

// The junk verdict on ONE of whisper's segments, given the level of its audio: the shared
// structural filter (annotations, phonetic garbage), and -- when the audio is QUIET (its loudest
// hop under floor + 10 dB, files having no absolute level) -- the caption shortlist, the prompt's
// echo and whisper's own low confidence. Plus a repetition loop at any level ("the next step is
// to the next step..."), and a word of six or more identical hyphenated pieces ("fleu-fleu-fleu-...",
// the film at 9:14) -- never a chant said word by word (review L4: "Vas-y ! Vas-y ! Vas-y !" is speech).
// Exposed for the selftest.
bool fileSegmentIsJunk(const std::string& text, double peakDb, double floorDb,
                       double avgLogprob, const std::string& prompt);
bool isRepetitionLoop(const std::string& text);

// After a HARD cut (F-RESUME: or an early stop's cut) the next window re-hears the end of the last one: drop
// its first words when their tokens repeat the last committed ones (normalised the live engine's way, <= 8
// tokens). F-RESUME: at an EARLY STOP's seam (`seamSec` = where the window after it starts; < 0 = not one) a
// SINGLE repeated word goes too, but only a copy starting within kSeamCopySec of the seam -- the same audio
// heard twice (the last word's DTW end placed early, the window after the seam starting inside it). Further
// away it may be said twice ("...I said no." | "No, I won't" 0.3 s apart -- review LOW 3): it stays.
void dropRepeatedHead(std::vector<FileWord>& next, const std::vector<FileWord>& committedTail, double seamSec = -1.0);

// ---------------------------------------------------------------------------
// F-RESUME (2026-10-26; redesigned after its adversarial review) -- AUDIO WHISPER STOPPED WRITING IN IS NOT
// VOUCHED FOR.
//
// A window's verdict vouches for a stretch of the film: everything before its next cursor is "analysed"
// (the run's frontier follows the cursor, FPROG's ranges follow the frontier, FDONE ok makes the tray
// record the whole file DONE). Until this wave a PAUSE-ended (or the last, or a limit's) window vouched
// for all of itself, whatever whisper wrote. MEASURED (meeting.wav, whisper base, a FILE cancelled at
// 31.41 s then resumed): the window 68.80-98.64 s came back as two segments ending 9.00 s into it; the
// 20.8 s of speech after them (five utterances) were never transcribed, and the record said DONE over
// them. The same request sent again answered them all: whisper-server's answer depends on its HISTORY
// (the context and the temperature fallback carried from one request to the next -- replayed byte for
// byte), so the early stop is a sampling accident of that decode, not a property of the audio. The
// uninterrupted run lost the file's last line the same way (its EOF window stopped after "...the second
// topic of the day").
//
// THE RULES (hearCheck) -- the measured failures, and nothing else. A HEARING is one request for a stretch; the
// rules only ever give a stretch its SECOND hearing, never a third, and never drop a word a hearing produced:
//   * AN EARLY STOP: this request COMMITS a word (after the de-dup nets -- a request whose words were all heard
//     twice commits nothing, and never cuts: review round 3, F12c's crawl), and kUnheardTailSec of ACTIVE audio
//     or more follows the window's LAST heard word (CommitVerdict::heardReal, from kUnheardPadPost after its end),
//     no explicit sound tag of whisper's covering it (heardTags: "[Applause]" over an applause is heard -- review
//     LOW 4; punctuation alone never covers it: a " ." whose end runs to the window's end would hide the stop --
//     review F6). EVERYTHING heard is kept; the next window starts right after that last word, and its own fresh
//     request is the second hearing of the rest -- up to the end of this window (HearContext::secondUpTo).
//     In a request that already IS a second hearing a new cut needs its last kept word kUnheardTailSec past the
//     cut it follows (progress); otherwise it is accepted -- up to secondUpTo, or to its last kept word when
//     that lies further. At most kMaxCutChain cuts in a row: the next request is vouched for as before F-RESUME.
//   * NOTHING COMMITTED: no committed word, and kUnheardTailSec or more of active audio after the last heard
//     word -- or, no word heard at all, that no "not speech" statement covers (heardNonSpeech: a music intro
//     whisper answered " ." or "[Music]" over) -> a FIRST hearing: the SAME window once more, nothing committed;
//     a SECOND hearing: accepted, but only up to secondUpTo (review round 3, F9 / F9b: after a cut the window
//     reaches past what was heard before; the audio beyond is on its first hearing and gets its own once more).
//   * NEVER ACTED ON: a gap between two heard words, the head of a window before its first word, a short
//     tail -- music, effects, laughter between lines (review F3: a 1.5 s effect after each of 12 lines made
//     13 requests out of 3 when every such gap was sent back); and a HARD cut that kept words (its own rule
//     already starts the next window at its last committed word).
// BOUNDED: every request commits a word -- the cursor moves past it -- or is a stretch's one second hearing. While
// a second hearing waits, the cursor stays before the unheard audio: a job cancelled meanwhile never reports
// it analysed (review F7).
// KNOWN LIMIT (review LOW 5): the tail is measured from the LAST heard word; a last word DTW places far too late
// (over music after the stop) hides the stop before it.
// ---------------------------------------------------------------------------
// T, MEASURED (docs\PROGRESS_F-RESUME.md; whisper base, meeting.wav): the real early stops left 12.1 s (the
// measured hole), 4.1 s and 2.7 s (the file's last line, "Perfect, thank you very much everyone.") of active
// audio after the last heard word; the ordinary windows 0.0-0.5 s (base, turbo and large-v3: 31 windows); review
// F3's sound effects 1.5 s. 3 s would miss the 2.7 s line.
constexpr double kUnheardTailSec    = 2.5;
constexpr double kUnheardPadPre     = 0.3;   // a DTW-timed span covers from this much before its start ...
constexpr double kUnheardPadPost    = 0.5;   // ... to this much after its end
constexpr double kUnheardPadUntimed = 1.0;   // ... either side, with whisper's token times (no DTW)
constexpr double kSeamCopySec       = 0.2;   // dropRepeatedHead: a single word repeated this close to an early stop's seam
constexpr int    kMaxCutChain       = 2;     // early-stop cuts in a row, at most
// The ACTIVE seconds of [from, to) (media seconds) outside every span of `cover`, each widened by pre / post.
double activeSecOutside(const EnergyTrack& en, double from, double to,
                        const std::vector<std::pair<double, double>>& cover, double pre, double post);
// What the job knows about the request being judged.
struct HearContext {
    uint64_t secondUpTo = 0;    // this request is the SECOND hearing of the audio before this sample (0 = a first hearing)
    uint64_t prevCut = 0;       // the early-stop cut it follows (0 = none): a new cut must pass it by kUnheardTailSec
    int      cutChain = 0;      // early-stop cuts in a row before it
    bool     hadWords = false;  // the verdict held a word BEFORE the de-dup nets (a hard cut's own rule then applies)
};
struct HearCheck {
    bool     rehear = false;    // the verdict would vouch for audio whisper never wrote in: it does not
    bool     same = false;      // ... NOTHING committed on a first hearing: the same window once more
    uint64_t nextCursor = 0;    // an EARLY STOP: the next window starts here; an accepted second hearing: it stops here
    bool     accepted = false;  // a second hearing accepted (nothing more, or no progress): vouched up to nextCursor
    bool     capped = false;    // kMaxCutChain reached: no cut, vouched for as before F-RESUME
    double   lastWord = -1.0;   // the last heard word's end (media seconds); -1 = no heard word in the window
    double   activeSec = 0.0;   // the active audio after it -- or, no word, the active audio no statement covers
};
// `v` = the window's verdict with its words AFTER the de-dup nets (what this request commits) -- heardReal /
// heardNonSpeech / heardTags are taken BEFORE them: words whisper wrote twice still say where it was writing.
HearCheck hearCheck(const EnergyTrack& en, const WindowPlan& plan, const CommitVerdict& v, const HearContext& ctx);

// ---------------------------------------------------------------------------
// Words -> subtitles.
// ---------------------------------------------------------------------------
struct CueCfg {
    bool   hanPair    = false;   // the pair has a Han language: a Han<->Latin change AFTER A PAUSE
                                 // splits (never a short embedded run -- CueBuilder::addWords)
    double gapSec     = 0.8;     // a silent gap this long always splits
    double softGapSec = 0.3;     // ... this long splits once the cue is already long:
    double softAfterSec = 3.0;   //     >= 3 s, or
    int    softLatin  = 40;      //     >= 40 NARROW characters (any alphabet), or
    int    softHan    = 16;      //     >= 16 WIDE ones (Han, kana, Hangul)
    double maxSec     = 7.0;     // hard limits: split at the last comma, else the largest gap
    int    maxLatin   = 84;      //   (two lines of 42 narrow)
    int    maxHan     = 32;      //   (two lines of 16 wide)
    // WF, E -- READABILITY. A cue stays up at least minSec AND its reading time (readCps narrow /
    // readCpsWide wide characters a second -- the usual subtitle norms, 17 and 7), extended into the
    // gap after it, never over the next cue nor past a run's limit (0.7 s and no reading time before).
    double minSec     = 1.0;
    double readCps    = 17.0;
    double readCpsWide = 7.0;
    double readExtend = 0.6;     // reading time added after the last word, never over the next
    double minGap     = 0.08;    // between two cues (two frames at 25 fps)
    // WF, E -- A FRAGMENT (no sentence end, and at most two words or under fragSec of speech) joins
    // its successor when the gap to it is under fragGapSec -- or, with no successor that close, the
    // cue before it when that one is still here, unfinished, and as close. A cue that cannot stay up
    // minSec because the next one starts at once (a rapid exchange, a list read out) SHARES the next
    // one's cue when the two fit the limits; and an over-long sentence is split where both halves stay
    // readable (a comma, a pause, balance -- never after its first word or two). No merge ever joins two
    // windows' languages (review P6: a cue has one language label, the router's), and a FRAGMENT never
    // joins another speaker's turn (a dialogue dash: "- Non !"); a line that cannot stay up minSec does
    // share the next speaker's cue -- the subtitle convention for a rapid exchange, the dash kept as the
    // turn's mark on the one line a cue is (WC). Korean is read at 12 Hangul a second (review L2).
    double fragSec    = 0.7;
    double fragGapSec = 1.0;
};
struct FileCue {
    double      start = 0.0, end = 0.0;    // MEDIA seconds, final
    std::string text;                      // one line of text: no hard line breaks (WC)
    std::string lang;                      // its first word's window language (the router's label)
};

class CueBuilder {
public:
    void configure(const CueCfg& c, const EnergyTrack* en);
    // v2 (WD): a new RUN. Everything held is forgotten (an interrupted run's held words are analysed
    // again later), and the run's cues are kept INSIDE it: none starts before `fromSec` -- nor within
    // the gap between cues after it when `afterRange` (an analysed stretch, with its own cues, ends
    // right there) -- none ends after `limitSec` less that gap; a `cut` word is dropped from its cue
    // (the cue goes when nothing is left), and a cue that cannot fit before the limit is dropped.
    // fromSec <= 0 and limitSec = +inf = v1 exactly.
    void beginRun(double fromSec, double limitSec, bool afterRange);
    // v2: no cue made from now on starts before `sec` (a frontier already reported to the tray --
    // the backstop that keeps every range strict, TrPipe.h).
    void raiseFloor(double sec) { if (sec - cfg_.minGap > lastEnd_) lastEnd_ = sec - cfg_.minGap; }
    void addWords(const std::vector<FileWord>& words);
    // A window ended in a pause this long: an unterminated sentence may close here.
    void windowBoundary(double pauseSec);
    void finish(double mediaEndSec);       // EOF: everything closes and is finalised
    bool pop(FileCue& c);                  // the next FINISHED cue, in order
    bool empty() const { return cur_.empty() && closed_.empty() && ready_.empty(); }
    // The earliest media time not yet final here (a held cue or an open sentence); +inf if none.
    double holdFrom() const;
    // v2: the earliest START a cue still held here can be given (snapStart moves a cue back onto
    // the speech onset, up to 0.4 s) -- the strict bound an "analysed" range may claim (TrPipe.h:
    // every cue of a range is sent before the range is). +inf if none.
    double holdFromStrict() const;
    // v2: the end of the last cue made (the run's own; -1 = none yet).
    double lastEnd() const { return lastEnd_; }
    uint32_t droppedCut() const { return droppedCut_; }   // cues dropped at a limit (the log, the selftest)
    uint32_t cutWords() const { return cutWords_; }       // words dropped as `cut` (cue kept or not)
    uint32_t mergedFragments() const { return merged_; }  // WF: fragments joined to a neighbour
    // A run of the other script this short (tokens), between two runs of one script, is part of
    // the sentence -- "U+628AreverbU+5173,6389", "OKU+FF0C,6211,4EEC,5F00,59CB,5427" -- never a cue of its own.
    static constexpr size_t kEmbeddedRun = 3;
private:
    static constexpr size_t kNoPend = (size_t)-1;
    void closeCur();
    void commitPending();
    void finalise(bool all, double mediaEndSec);
    void splitOverLimit();
    bool shouldSplitBefore(const FileWord& w) const;
    double snapStart(double t, bool timed = false) const;
    double snapEnd(double t) const;
    double readSec(const std::string& t) const;    // WF: a text's reading time (CueCfg::readCps)
    CueCfg                            cfg_;
    const EnergyTrack*                en_ = nullptr;
    std::vector<FileWord>             cur_;
    std::deque<std::vector<FileWord>> closed_;
    std::deque<FileCue>               ready_;
    double                            lastEnd_ = -1.0;
    size_t                            pendAt_ = kNoPend;   // a language change waiting to prove itself
    int                               lastHan_ = -1;       // the last emitted cue: 1 Han, 0 not, -1 none
    double                            limit_ = std::numeric_limits<double>::infinity();   // v2: the run's limit
    uint32_t                          droppedCut_ = 0;
    uint32_t                          cutWords_ = 0;
    uint32_t                          merged_ = 0;
    double                            lastWordEnd_ = -1.0;   // WF: the last cue made: its last WORD's end
};

// WF, B -- THE SAFETY DE-DUP. A decode that hands back words ALREADY COMMITTED: `fresh` (the next
// window's words, in order) against `recent` (the job's last committed words). Two independent
// rules, both measured on the film's duplicate (the same sentence first committed with its tokens
// stretched over 28 s of music, then again, correctly timed):
//  * the SAME AUDIO: a DTW-timed word starting before the last committed word ended (less a
//    tolerance) was heard twice -- dropped;
//  * the SAME TEXT across the SEAM (review P5: a copy heard twice is the end of one window heard again
//    at the start of the next -- the new head starts within 6 s of the last committed word, and >= 80 %
//    of it (>= 3 words) is found in order in the committed words of the 6 s before that, from their
//    sentence's start): kept or dropped by TIMING -- a copy whose words are spread far beyond any
//    speech rate (< kSlowCps characters a second) is the badly timed one: the new copy is dropped when
//    IT is; kept when only the old one is (it is out already, and the viewer gets the sentence where
//    it is said); and when both are plausible it is a person repeating himself -- kept ("L'essai
//    numero 1, 2, 3D... Je repete, l'essai 1, 2, 3D..." is in the film). A refrain, a chant, a call
//    said again further away is never compared, however slowly it is said.
// Returns how many words were dropped.
constexpr double kSlowCps = 4.0;
size_t dropAlreadyCommitted(std::vector<FileWord>& fresh, const std::deque<FileWord>& recent);

// Helpers the cue rules are written in, exposed for the selftest.
int  latinChars(const std::string& s);
int  hanChars(const std::string& s);
int  narrowChars(const std::string& s);    // the limits' width classes: every alphabet...
int  wideChars(const std::string& s);      // ... and Han / kana / Hangul
bool hasLetterOrDigit(const std::string& s);
std::string oneLine(const std::string& s);     // \r \n \t -> spaces, runs collapsed, trimmed

} // namespace pbt

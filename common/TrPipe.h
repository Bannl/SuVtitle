// =============================================================================
// SuVtitle - common/TrPipe.h   (the control protocol between SuVtitle.exe and
//                               SuVtitleEngine.exe)
//
// SUVTITLE SPLIT (2026-10-23): this file is AudioPB's translator/TrPipe.h as of commit 641f1aa
// (APB_TRANSLATOR "v1.0.6"), carried over when the player and its file transcription/translation
// left AudioPB to become SuVtitle. SuVtitle uses the FILE MODE half (FILE / FCANCEL / FPROXY /
// FFOCUS <-> FPROG / FCUE / FDONE / FPROXYDONE, the WH `audio` field) plus the handshake and the
// STATE/CTL lines the engine needs; the LIVE half (fabric audio, LINE/PARTIAL/END streaming,
// speakers) is AudioPB's and is described below only until the engine port prunes it. In the text
// below read "Config"/"the tray" as SuVtitle.exe and "the exe"/"the helper" as SuVtitleEngine.exe.
// The pipe is named \\.\pipe\suvtitle.engine.<appPid>.<seq> (pipeName below).
// OWNED BY THE DIRECTOR: the engine and the app agents read it, never edit it.
//
// HEADER-ONLY and included by BOTH sides, so the two ends cannot drift apart.
// Deliberately a SIBLING of ui/router/FxPipe.h rather than a reuse of it: the two
// protocols differ in the one thing that matters most here.
//
// ⚠️ WHY THIS IS NOT FxPipe: FxPipe is strictly request -> reply (Config asks, the
// exe answers). A translator has NOTHING to answer -- it SPEAKS. Subtitles arrive
// when someone talks, not when the tray asks, so this pipe is a DUPLEX STREAM:
// either end may write a message at any moment and neither correlates replies.
// Both sides therefore run "read with a short deadline, write when we have
// something", never "write then block for the answer". Getting that wrong would
// deadlock the tray behind a speaker's silence.
//
// Transport: a MESSAGE-mode named pipe ("\\.\pipe\audiopb.tr.<configPid>.<seq>").
// Config is the server and creates it BEFORE spawning the exe; the exe connects as
// a client. Message mode means one WriteFile = one message = one ReadFile -- no
// framing protocol to get wrong. AUDIO NEVER RIDES THIS PIPE: it stays on the
// fabric, where the translator is an ordinary consumer. Only text does.
//
// Messages are UTF-8, tab-separated. Free text (transcripts!) is escaped so a tab
// or a newline in a transcript can never desynchronise the stream.
//
//   exe -> Config, on connect:  HELLO <slotName>
//   exe -> Config, whenever:
//     STATE <status> <preset> <mode> <asrModel> <mtModel> <gpu> <err>
//           [<spkOn> <spkModel> <spkCount> [<langA> <langB>]]
//         status = loading | listening | transcribing | analyzing | error
//                  (analyzing = a FILE job owns the GPU and the live path is paused)
//         preset/mode = what the helper is ACTUALLY running. The helper owns
//                  translator.txt, so this is how the tray learns the persisted
//                  choice instead of overwriting it at every spawn.
//                  mode = auto | a2b | b2a  (W2: a DIRECTION over the pair below,
//                  no longer two hard-coded languages; the old en2zh/zh2en tokens
//                  are still ACCEPTED on the way in, see unpackCtl.)
//         gpu    = 1 (Vulkan) | 0 (CPU fallback)
//         err    = "" or one human sentence
//         spkOn  = 1 when the diarization stage came up; 0 when it disabled itself
//                  (no sherpa runtime, no model) and speakers are named by CABLE
//         spkModel/spkCount = the model tag ("campplus") and how many distinct
//                  voices have been heard so far. THREE OPTIONAL FIELDS: a reader
//                  that stops at err is still correct, which is what lets a tray
//                  built before diarization talk to a helper built after it.
//         langA/langB = the PAIR actually running, as the helper's canonical codes:
//                  ISO-639-1 ("en", "zh"...) plus "zh-tw" (Traditional, normalizeLangCode
//                  in TrEngine.cpp is the one vocabulary). Two more
//                  optional fields, appended for the same reason: the tray asks for
//                  a pair with CTL and learns from here which one took effect (a
//                  code the helper could not honour never appears in this echo).
//     LINE <tEpochMs> <lang> <src> <dst> <ms> <spkSlot> <spkLang> [<pass> [<spkLabel>
//          [<segId> [<waitMs> [<dstLang> [<seq> [<closes>]]]]]]]
//         lang    = detected ISO-ish code of the SOURCE ("en", "zh", ...)
//         src/dst = the transcript and its translation (escaped)
//         ms      = THE INFERENCE, and only the inference: voiceprint + ASR + MT for
//                   this utterance, milliseconds. It was "VAD cut -> this line" until
//                   a real test showed a user reading 579 ms while waiting several
//                   seconds -- because the wait is SEGMENTATION, not inference, and
//                   this number never contained it. It is now the TOOLTIP; the number
//                   the UI shows is waitMs.
//         spkSlot = fabric slot NAME of the dominant fan-in input for this segment
//                   (the speaker attribution, per cable); "" if unattributable
//         spkLang = the language that pairs with spkSlot to make the speaker KEY
//                   when there is no voiceprint (the diarization stage is off)
//         pass    = 1 when the line was NOT translated because a manual mode lock
//                   was on and the segment was already in the TARGET language.
//                   dst then repeats src verbatim. OPTIONAL field: a reader that
//                   stops at spkLang is still correct.
//         spkLabel= "S1".."S8", the VOICE cluster this utterance was assigned to,
//                   or "" when the diarization stage is off. Global across cables;
//                   the tray pairs it with spkSlot to make the key, so two physical
//                   sources can never be merged into one speaker by a model.
//         segId   = the utterance this line finishes. The PART messages that led up
//                   to it carry the SAME id, which is how the UI knows which live
//                   line this paragraph replaces. Monotonic per helper process.
//         waitMs  = wall time from the VAD CLOSE to the moment this message went out
//                   on the pipe. THE HONEST NUMBER, and the one the UI shows: `ms`
//                   above measures the inference alone and is a tooltip. Two more
//                   OPTIONAL fields: a reader that stops at spkLabel is still correct.
//         dstLang = the language `dst` is IN. Not derivable from `lang` any more:
//                   with a free pair the target is whichever half `lang` is not, and
//                   a passthrough line (pass=1) repeats the source, so dstLang ==
//                   lang there. It is what lets the window show "only language X"
//                   without guessing.
//         seq     = 1, 2, 3... WITHIN this segId. An utterance is no longer one line
//                   (see THE SEQUENCE below); this is the order they go in, and the
//                   only thing that lets a UI rebuild a paragraph it received out of
//                   order. 0 = "an older helper, which had exactly one line".
//         closes  = 1 when this line ENDS its utterance (the VAD closed), 0 when the
//                   speaker is still talking and more lines for this segId are coming.
//                   Two more OPTIONAL fields; a reader that stops at dstLang is still
//                   correct, and MISSING MEANS closes=1 -- an old helper's LINE always
//                   finished its utterance, so an old helper and a new UI agree.
//     END <segId>
//         "That utterance produced nothing more." Sent INSTEAD of a closing LINE when
//         the tail left after the last committed sentence is a breath rather than a
//         sentence (shorter than the VAD's own minimum, or transcribed as silence /
//         caught by the hallucination filter). Without it the live line for that segId
//         would hang on screen until the UI's 20 s orphan timer swept it, i.e. long
//         after the conversation moved on. An older tray IGNORES this message, which is
//         exactly the old behaviour -- so it is safe to send to one.
//     PART <segId> <tEpochMs> <lang> <src> [<dst> [<dstLang>]]
//         The LIVE line, emitted while the speaker is still talking, so the screen
//         moves during a sentence instead of after it. Rules:
//           * REPLACE IN PLACE: a PART supersedes the previous PART of the same
//             segId. A closes=1 LINE (or an END) supersedes them all; a closes=0 LINE
//             does NOT -- it takes its sentence out of the live line and leaves the
//             rest standing, which is what the next PART then shows;
//           * NO SPEAKER. Attribution happens once, on the final -- a live line that
//             changed colour mid-sentence would be a bug, not a feature;
//           * dst is a DRAFT translation and may be absent (PARTIALMT 0, or the text
//             has not moved enough since the last draft to be worth a model call);
//           * a partial is SKIPPABLE by construction. It is never queued: the audio
//             side keeps ONE latest-wins slot, and the pipeline only ever picks it up
//             when no finished utterance is waiting. Missing partials are normal;
//           * its `src` is the UNCOMMITTED TAIL only (W3). Everything the helper has
//             already sent as a LINE has been cut off the front of the open segment, so
//             a PART never repaints text the user has finished reading.
//
//   THE SEQUENCE, and why it changed (W3, the "commit as you go" engine).
//   Until this wave an utterance was ONE line, emitted when the VAD closed the segment
//   -- which in continuous speech is the 8 s force-cut, i.e. two or three sentences
//   after the first of them ended. The unit of a FINAL is now the SENTENCE, so one
//   utterance produces:
//
//       LINE(segId, seq=1, closes=0)   the first sentence, finalised MID-UTTERANCE
//       PART(segId, ...)               the live line, now showing only what follows it
//       LINE(segId, seq=2, closes=0)   ... and so on, while the speaker keeps talking
//       PART(segId, ...)
//       LINE(segId, seq=3, closes=1)   the last sentence, at the VAD close
//     | END(segId)                     ... or this, when that tail was not a sentence
//
//   A UI therefore APPENDS a closes=0 line to the speaker's paragraph and keeps the
//   live line (the next PART replaces it with the remainder); closes=1 and END are the
//   two ways a paragraph ends, and exactly one of them arrives per utterance that
//   produced any line at all. Nothing is ever sent for a segId after that.
//   Config -> exe, whenever:
//     CTL <mode> <preset> <clear> [<langA> <langB>]
//                                       mode = auto|a2b|b2a (or "" = unchanged);
//                                         en2zh/zh2en are READ as a2b/b2a so a tray
//                                         built before this wave still steers a
//                                         helper built after it
//                                       preset = auto|quality|balanced|light (or "")
//                                       clear = 1 -> drop queued segments now
//                                       langA/langB = canonical codes (ISO-639-1 or
//                                         "zh-tw"), or "" each
//                                         for "leave that half alone"
//     QUIT                              -> the exe exits (no reply expected)
//
//   FILE MODE (2026-10-10). The second thing a translator can be asked to do: not follow a
//   conversation, but SUBTITLE A RECORDING -- a file the user loads into the Translator
//   window and plays there, with the translation shown as subtitles in sync. It is an OFFLINE
//   job and that is the whole point of it: nothing is waiting on the next word, so it runs the
//   expensive settings the live path cannot afford (the best ASR model installed, full 30 s
//   windows, beam search, more MT context) and its output is FINAL -- a cue is never revised,
//   so none of THE SEQUENCE's replace-in-place rules above apply to it.
//   Config -> exe:
//     FILE <jobId> <path> <langA> <langB> <mode> <quality>
//         jobId   = minted by the tray, > 0, never reused within one tray process. EVERY
//                   message about the job echoes it; one carrying another id is STALE (an
//                   older job's last words still in flight) and is dropped by the reader.
//         path    = the media file, absolute, UTF-8.
//         langA/langB/mode = the pair and direction FOR THIS JOB, snapshotted at its start: a
//                   CTL moving the live pair mid-job re-routes the live path, never a job
//                   already half-subtitled (the tray offers a re-analysis instead). "" = the
//                   helper's current value, validated exactly like a CTL's.
//         quality = auto | max | fast  ("" = auto)
//                   auto = fast = large-v3-turbo (the running preset's final model when it is
//                          absent). NOT large-v3, and measured rather than assumed (10/10, WB
//                          study): same accuracy as large-v3 on bench_en/bench_zh clean and at
//                          5 dB / 0 dB SNR, better at -3 dB Chinese and on mixed-language audio,
//                          and 1.8-2x faster per call.
//                   max  = large-v3, and an FDONE error naming the file when it is absent (the
//                          installer does not ship it)
//         A FILE while a job is running CANCELS that job first; its FDONE says "cancelled".
//     FCANCEL <jobId>   stop that job at the next step boundary (at most one model call away).
//                       The tray mints FILE and FPROXY ids from ONE counter, so an id names one
//                       or the other, never both: FCANCEL on an FPROXY's id cancels that proxy.
//     FPROXY <jobId> <srcPath> <outPath>
//         Write a PLAYABLE copy of srcPath's AUDIO to outPath (16-bit PCM WAV, the source's
//         rate, at most 2 channels; RF64 past the 4 GiB RIFF limit -- the WebView plays it,
//         probe 10/10), for the files the WebView cannot play itself (avi, wmv, mpg, aiff...):
//         the user still hears it under the subtitles. Its timeline is the MEDIA's, leading
//         silence included, so the cues line up on it exactly as on the original. Independent
//         of the analysis -- its own thread, CPU only -- and answered by exactly one FPROXYDONE.
//         jobId here is a correlation id, not a FILE's. A new FPROXY SUPERSEDES a running one,
//         which then answers FPROXYDONE ok=0 err="superseded".
//   exe -> Config:
//     FPROG <jobId> <phase> <doneMs> <totalMs> <asrModel> <mtModel> <hasVideo>
//         phase    = loading (models coming up -- large-v3 is 3 GB off disk) | analyzing
//         doneMs   = media time analysed so far: the FRONTIER. Every cue ending before it has
//                    already been sent, which is what lets a player say "not analysed yet"
//                    about a position past it instead of showing an empty screen.
//         totalMs  = the file's duration (0 while unknown)
//         asrModel/mtModel = the model files THIS JOB runs on (not the live preset's)
//         hasVideo = 1 when the file carries a picture -- how the UI tells an audio file
//                    from a video the WebView could not decode
//     FCUE <jobId> <idx> <startMs> <endMs> <lang> <dstLang> <pass> <src> <dst>
//         One finished subtitle. idx = 0,1,2... in emission order; startMs/endMs = MEDIA
//         time; cues arrive in increasing startMs and never overlap. lang/dstLang/pass mean
//         what they mean on LINE.
//     FDONE <jobId> <result> <count> <err>
//         result = ok | cancelled | error; count = cues sent; err = "" or one human sentence.
//         EXACTLY ONE per FILE, and the last message of its job.
//     FPROXYDONE <jobId> <ok> <outPath> <err>
//   While a job runs, STATE.status = "analyzing" and the LIVE pipeline is paused -- the GPU
//   is one resource (17/09 bench). Fabric audio arriving meanwhile is DROPPED, not queued: a
//   backlog translated after a ten-minute job would be subtitles for a conversation that is
//   over. The live path resumes on its own after FDONE.
//
//   FILE MODE v2 (2026-10-11) -- FOLLOW THE PLAYHEAD. The user WATCHES while it analyses
//   (user, 11/10: "que la traduction soit toujours en avance sur la vidéo, et que la complétion
//   du fichier sous-titre se fasse en fond pendant qu'on regarde"). v1 already streamed cues
//   from 0:00 at ~10x real time, so playing from the start was always covered; what v1 could not
//   do is follow a SEEK: jump to 40:00 while it is at 5:00 and the picture has no subtitles for
//   the next half hour. v2 makes the analysis go where the playhead goes. All fields APPENDED:
//   a v1 reader still works for a job that uses neither `have` nor `focusMs`.
//     FILE <jobId> <path> <langA> <langB> <mode> <quality> [<have> [<focusMs>]]
//         have    = media ranges ALREADY subtitled for this file under this pair/mode/quality --
//                   the tray holds their cues (its cache of an earlier job that was interrupted:
//                   the user closed the film half way). "s-e,s-e,..." in ms, ascending, non-
//                   overlapping (packRanges below); "" = none. The job analyses only the GAPS.
//         focusMs = where the playhead is (0 = the start).
//     FFOCUS <jobId> <ms>
//         The playhead moved (a seek, or play started elsewhere). After the model call in
//         flight, the job continues from the first GAP at or after `ms` -- so a focus inside an
//         analysed range means "carry on from the end of that range". Stale ids are ignored.
//   ORDER OF WORK: from the focus forward to the end of the file (jumping over every analysed
//   range), THEN the gaps left before the focus, earliest first. FDONE ok = the whole file is
//   covered; cancelled/error = partial, and the tray keeps the ranges + cues to resume later.
//   FPROG gains [<ranges>]: the ranges analysed NOW (have + this job's, merged), and doneMs
//   becomes their TOTAL duration, so doneMs/totalMs stays the share done. There is no single
//   frontier any more: a player asks "is the playhead inside a range?".
//   FCUE order: increasing startMs WITHIN one run (a focus to the next analysed range or the
//   end); a new focus starts a new run, so cues of different runs interleave in time. Cues
//   never overlap each other nor a cue of `have`. At a JOIN inside speech (a run reaching the
//   start of an analysed range) each side hears ~2 s past the join and keeps only the words
//   that START on its side: a sentence across the join keeps its head as a cue ending at or
//   before the range start - 80 ms, its tail belongs to the range; only when no post-roll fits
//   are the cut WORDS dropped (never a whole cue). idx stays unique.
//   PRECISIONS (WD, 2026-10-11 -- implemented and asserted helper-side, review round included):
//     * EVERY cue of a range lies INSIDE that range, strictly (`end <= range.e` in whole ms, for
//       every cue at every FPROG sent after it; asserted per FPROG). Readers may still allow 1 ms
//       of slack for a helper built before the review round. Every FCUE of a stretch is sent
//       BEFORE the FPROG whose `ranges` first includes it. So the tray must build `have` ONLY from
//       FPROG ranges whose cues it holds -- that is what makes "never overlap a cue of `have`"
//       true without the helper ever seeing those cues.
//     * A gap shorter than 100 ms counts as analysed (it appears inside the FPROG ranges).
//     * FFOCUS: the latest one wins (a burst costs only the last). Each one is compared with the
//       previous: arriving within 12 s and moved forward by at most 2.5x the elapsed time + 2 s,
//       it is PLAYBACK. The current run is KEPT when the first gap at/after the focus lies in the
//       run's stretch and is within 15 s of its cursor, or when every FFOCUS since the last one
//       taken was playback (a slow, CPU-only machine must not be restarted by a playhead that is
//       merely playing -- review C1). Otherwise, once the playhead has been still for 250 ms
//       (also when it lands during a drain), the run stops at its cursor, the cues already
//       transcribed are translated and sent (no new window -- never thrown away), and the new run
//       starts. The seek that opens a run is abortable (FCANCEL is at most one decoded buffer
//       away). An FFOCUS during the model load is kept and applied when analysis starts; one
//       naming a FILE still QUEUED becomes that FILE's focus; one for a finished/unknown job is
//       dropped.
//     * A FILE whose `have` covers the whole file (known duration) answers FPROG + FDONE ok at
//       once, with NO model load. A FILE whose `have` does not parse, or with no path, still
//       stops the running job and gets exactly one FDONE error (even when superseded in the
//       same lap).
//     * doneMs = the total of the ranges. For a FILE without have/focus it tracks v1's frontier
//       but may sit up to ~0.45 s BELOW it (ranges only advance past what has been sent) --
//       harmless for an older tray, which only shows it as a percentage.
//
//   FILE MODE + THE FILM'S OWN SOUND TRACKS (wave WH, 2026-10-11). The user: "choisir les
//   sous-titres et l'audio déjà inclus dans le fichier vidéo". A film with several sound tracks
//   (an MKV with English 5.1, French, Japanese...) is now analysed on the track the viewer
//   LISTENS to, not always on the first one. One field APPENDED:
//     FILE <jobId> <path> <langA> <langB> <mode> <quality> [<have> [<focusMs> [<audio>]]]
//         audio = "" -- the file's FIRST sound track in CONTAINER order (= ord 0, what the viewer
//                       HEARS: the WebView always plays the first one it can decode, measured).
//                       For Matroska/WebM and single-track files that is also what every FILE before
//                       WH analysed. NOT for an MP4/MOV with 2+ sound tracks: MF's own default pick
//                       there is often another trak (measured by WH-H: reverse moov order, its own
//                       language choice), so a pre-WH record of such a file may hold another track's
//                       cues -- the tray treats those records as stale (its own track marker).
//               | "<ord>:<trackId>" -- ord = the track's 0-based position among the CONTAINER's
//                       sound tracks, in container order, the ones nothing here can decode (DTS,
//                       TrueHD...) INCLUDED; trackId = the container's own id for it (Matroska
//                       TrackNumber, MP4/MOV track_ID), 0 when unknown. Both come from the tray's
//                       own reading of the file (ui/MediaTracks.h). The helper analyses THAT
//                       track (how it finds it in Media Foundation is its business, measured and
//                       documented helper-side); a track it cannot open or decode ends the job
//                       with exactly one FDONE error whose sentence names the format. "0:<id>"
//                       is the first track, same as "".
//         A malformed `audio` refuses the whole FILE, exactly like a malformed `have` (one FDONE
//         error): analysing the wrong sound track is worse than not analysing at all -- the
//         subtitles would be of a dialogue the viewer does not hear.
//
// A message neither side understands is IGNORED, never answered: silence on this
// pipe is normal (nobody is talking), so it can never be a failure signal. The
// supervisor's watchdog is the CHILD PROCESS DYING, not a missing reply.
// =============================================================================
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pbt { namespace trpipe {

inline std::string pipeName(uint32_t configPid, uint32_t seq) {
    char b[64];
    std::snprintf(b, sizeof(b), "\\\\.\\pipe\\suvtitle.engine.%lu.%lu",
                  (unsigned long)configPid, (unsigned long)seq);
    return b;
}

// ---------------------------------------------------------------------------
// The slot identity, "tr:<name> (pid)". ONE implementation, like FxPipe's, so the
// stable-key rules the whole ghost/persistence model relies on (strip " (pid)")
// keep matching. pid = the CALLING process (the helper exe).
// ---------------------------------------------------------------------------
inline std::string utf8Truncated(const std::wstring& w, size_t maxBytes) {
    if (w.empty() || maxBytes == 0) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    if (s.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    return s;
}

// The card's visible name. ONE translator per tray, so the label is fixed -- the
// slot still carries the pid, because the fabric identifies a participant by its
// full name and a respawn must not collide with the dying incarnation's slot.
inline const wchar_t* kTranslatorLabel = L"Translator";

inline std::string makeTranslatorSlotName(const std::wstring& label = kTranslatorLabel) {
    char pid[24];
    std::snprintf(pid, sizeof(pid), " (%lu)", (unsigned long)::GetCurrentProcessId());
    const size_t budget = 47 - 3 /*"tr:"*/ - std::strlen(pid);
    return "tr:" + utf8Truncated(label, budget) + pid;
}

// Does this fabric name belong to a translator card? (The tray filters on it.)
inline bool isTranslatorName(const std::string& n) { return n.rfind("tr:", 0) == 0; }

// ---------------------------------------------------------------------------
// Field packing. A transcript is arbitrary user speech -- it can contain anything
// a language allows -- so tabs/newlines/backslashes are ESCAPED (not stripped):
// the text must survive the wire intact for the log window, unlike FxPipe's
// plugin names where scrubbing was enough.
// ---------------------------------------------------------------------------
inline std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\t': o += "\\t";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

inline std::string unesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        switch (s[++i]) {
            case '\\': o += '\\'; break;
            case 't':  o += '\t'; break;
            case 'n':  o += '\n'; break;
            case 'r':  o += '\r'; break;
            default:   o += s[i]; break;
        }
    }
    return o;
}

inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> t;
    size_t at = 0;
    for (;;) {
        const size_t e = s.find('\t', at);
        t.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
        if (e == std::string::npos) break;
        at = e + 1;
    }
    return t;
}

// ---- STATE -----------------------------------------------------------------
struct WireState {
    std::string status = "loading";   // loading | listening | transcribing | analyzing | error
    std::string preset = "balanced";  // quality | balanced | light (what is RUNNING)
    std::string mode   = "auto";      // auto | a2b | b2a        (what is RUNNING)
    std::string asrModel, mtModel;    // the model FILENAMES actually loaded
    bool        gpu = false;          // true = Vulkan; false = the CPU fallback took over
    std::string err;                  // "" unless status == error
    bool        spkOn = false;        // the diarization stage is armed
    std::string spkModel;             // "campplus" when it is, "" when it is not
    int         spkCount = 0;         // distinct voices heard so far
    // The pair ACTUALLY running. Defaulted to the two languages this feature was built
    // for, so a reader that never sees these fields (an older helper) still has a
    // truthful answer rather than an empty one.
    std::string langA = "en";
    std::string langB = "zh";
};

inline std::string packState(const WireState& s) {
    return "STATE\t" + esc(s.status) + "\t" + esc(s.preset) + "\t" + esc(s.mode)
         + "\t" + esc(s.asrModel) + "\t" + esc(s.mtModel)
         + "\t" + (s.gpu ? "1" : "0") + "\t" + esc(s.err)
         + "\t" + (s.spkOn ? "1" : "0") + "\t" + esc(s.spkModel)
         + "\t" + std::to_string(s.spkCount)
         + "\t" + esc(s.langA) + "\t" + esc(s.langB);
}

inline bool unpackState(const std::vector<std::string>& t, WireState& s) {
    if (t.size() < 8 || t[0] != "STATE") return false;
    s.status   = unesc(t[1]);
    s.preset   = unesc(t[2]);
    s.mode     = unesc(t[3]);
    s.asrModel = unesc(t[4]);
    s.mtModel  = unesc(t[5]);
    s.gpu      = (t[6] == "1");
    s.err      = unesc(t[7]);
    s.spkOn    = (t.size() >= 9  && t[8] == "1");
    s.spkModel = (t.size() >= 10) ? unesc(t[9]) : std::string();
    s.spkCount = (t.size() >= 11) ? (int)std::strtol(t[10].c_str(), nullptr, 10) : 0;
    // Missing (an older helper) leaves the DEFAULT pair standing rather than blanking it:
    // an empty language code is not a fact about this session, it is an absence of one.
    if (t.size() >= 12 && !t[11].empty()) s.langA = unesc(t[11]);
    if (t.size() >= 13 && !t[12].empty()) s.langB = unesc(t[12]);
    return true;
}

// ---- LINE ------------------------------------------------------------------
struct WireLine {
    uint64_t    tEpochMs = 0;
    std::string lang;        // detected source language ("en", "zh", ...)
    std::string src, dst;    // transcript, translation
    uint32_t    ms = 0;      // VAD cut -> this line, wall milliseconds
    std::string spkSlot;     // dominant fan-in input's fabric slot name ("" = unknown)
    std::string spkLang;     // the language half of the speaker KEY (no-voiceprint fallback)
    bool        pass = false;   // mode lock: already in the target language, dst == src
    std::string spkLabel;    // "S1".."S8" from the voiceprint stage, "" when it is off
    uint64_t    segId = 0;   // the utterance; its PARTs carried the same id
    uint32_t    waitMs = 0;  // the moment the speech ENDED -> this message on the wire.
                             // The honest metric. For a closing line that moment is the
                             // VAD close; for a committed one it is the SENTENCE's own
                             // end, which is what makes the two comparable.
    std::string dstLang;     // the language `dst` is in ( == lang on a passthrough )
    uint32_t    seq = 0;     // 1,2,3... within segId (0 = an older helper: one line)
    // ⚠️ DEFAULTS TRUE, and that is the back-compat rule in one line: a message without
    // this field came from a helper whose every LINE finished its utterance.
    bool        closes = true;
};

inline std::string packLine(const WireLine& l) {
    return "LINE\t" + std::to_string(l.tEpochMs) + "\t" + esc(l.lang) + "\t" + esc(l.src)
         + "\t" + esc(l.dst) + "\t" + std::to_string(l.ms) + "\t" + esc(l.spkSlot)
         + "\t" + esc(l.spkLang) + "\t" + (l.pass ? "1" : "0") + "\t" + esc(l.spkLabel)
         + "\t" + std::to_string(l.segId) + "\t" + std::to_string(l.waitMs)
         + "\t" + esc(l.dstLang)
         + "\t" + std::to_string(l.seq) + "\t" + (l.closes ? "1" : "0");
}

inline bool unpackLine(const std::vector<std::string>& t, WireLine& l) {
    if (t.size() < 8 || t[0] != "LINE") return false;
    l.tEpochMs = ::_strtoui64(t[1].c_str(), nullptr, 10);
    l.lang     = unesc(t[2]);
    l.src      = unesc(t[3]);
    l.dst      = unesc(t[4]);
    l.ms       = (uint32_t)std::strtoul(t[5].c_str(), nullptr, 10);
    l.spkSlot  = unesc(t[6]);
    l.spkLang  = unesc(t[7]);
    l.pass     = (t.size() >= 9 && t[8] == "1");
    l.spkLabel = (t.size() >= 10) ? unesc(t[9]) : std::string();
    // The progressive-display pair, appended AFTER spkLabel for the same reason
    // spkLabel was appended after pass: every field is optional going forwards, so a
    // tray built before this wave still parses a line from a helper built after it.
    l.segId    = (t.size() >= 11) ? ::_strtoui64(t[10].c_str(), nullptr, 10) : 0ull;
    l.waitMs   = (t.size() >= 12) ? (uint32_t)std::strtoul(t[11].c_str(), nullptr, 10) : 0u;
    // W2's pair. Absent = an older helper, which could only ever have been running en<->zh:
    // the honest reconstruction is "the other language", and a passthrough repeats the source.
    l.dstLang  = (t.size() >= 13) ? unesc(t[12]) : std::string();
    if (l.dstLang.empty())
        l.dstLang = l.pass ? l.lang : (l.lang == "zh" ? "en" : "zh");
    // W3's pair, appended for the same reason every other pair was. The two DEFAULTS are
    // the whole compatibility story: an older helper sent one line per utterance, so its
    // line is seq 0 and it CLOSES -- which is precisely how a UI built for this wave must
    // treat it, and it needs no branch to do so.
    l.seq      = (t.size() >= 14) ? (uint32_t)std::strtoul(t[13].c_str(), nullptr, 10) : 0u;
    l.closes   = (t.size() >= 15) ? (t[14] == "1") : true;
    return true;
}

// ---- END -------------------------------------------------------------------
// "This utterance is over and produced no further line." The ONE field is the point:
// there is nothing else to say, and a message with nothing in it is what the live line
// needs in order to fall at the right instant instead of at the orphan timer.
struct WireEnd {
    uint64_t segId = 0;
};

inline std::string packEnd(const WireEnd& e) {
    return "END\t" + std::to_string(e.segId);
}

inline bool unpackEnd(const std::vector<std::string>& t, WireEnd& e) {
    if (t.size() < 2 || t[0] != "END") return false;
    e.segId = ::_strtoui64(t[1].c_str(), nullptr, 10);
    return e.segId != 0;      // END 0 names no utterance and can only be a malformed line
}

// ---- PART ------------------------------------------------------------------
// The live line. Deliberately NOT a WireLine with a flag: a partial has no speaker,
// no timing claim and no permanence, and giving it the final's struct would invite
// exactly the confusion where a draft ends up in the scrollback.
struct WirePartial {
    uint64_t    segId = 0;
    uint64_t    tEpochMs = 0;
    std::string lang;        // detected source language of the draft
    std::string src;         // the transcript SO FAR
    std::string dst;         // the draft translation, or "" (PARTIALMT 0 / not worth a call)
    std::string dstLang;     // the language `dst` is in; "" while there is no dst at all
};

inline std::string packPartial(const WirePartial& p) {
    return "PART\t" + std::to_string(p.segId) + "\t" + std::to_string(p.tEpochMs)
         + "\t" + esc(p.lang) + "\t" + esc(p.src) + "\t" + esc(p.dst)
         + "\t" + esc(p.dstLang);
}

inline bool unpackPartial(const std::vector<std::string>& t, WirePartial& p) {
    if (t.size() < 5 || t[0] != "PART") return false;
    p.segId    = ::_strtoui64(t[1].c_str(), nullptr, 10);
    p.tEpochMs = ::_strtoui64(t[2].c_str(), nullptr, 10);
    p.lang     = unesc(t[3]);
    p.src      = unesc(t[4]);
    p.dst      = (t.size() >= 6) ? unesc(t[5]) : std::string();
    p.dstLang  = (t.size() >= 7) ? unesc(t[6]) : std::string();
    // Same reconstruction as LINE's, and for the same reason -- but ONLY when there is a
    // draft to attach a language to: an empty dst has no language, and inventing one would
    // make a window filtering on "show me zh only" display an empty line.
    if (p.dstLang.empty() && !p.dst.empty())
        p.dstLang = (p.dst == p.src) ? p.lang : (p.lang == "zh" ? "en" : "zh");
    return true;
}

// ---- CTL -------------------------------------------------------------------
struct WireCtl {
    std::string mode;     // "" = unchanged, else auto | a2b | b2a
    std::string preset;   // "" = unchanged, else auto | quality | balanced | light
    bool        clear = false;
    std::string langA;    // "" = unchanged, else a canonical code (ISO-639-1 or "zh-tw")
    std::string langB;
};

inline std::string packCtl(const WireCtl& c) {
    return "CTL\t" + esc(c.mode) + "\t" + esc(c.preset) + "\t" + (c.clear ? "1" : "0")
         + "\t" + esc(c.langA) + "\t" + esc(c.langB);
}

inline bool unpackCtl(const std::vector<std::string>& t, WireCtl& c) {
    if (t.size() < 4 || t[0] != "CTL") return false;
    c.mode   = unesc(t[1]);
    c.preset = unesc(t[2]);
    c.clear  = (t[3] == "1");
    // ⚠️ THE TRANSLATION LIVES HERE AND NOWHERE ELSE. W2 turned two hard-coded languages
    // into a direction over a pair, and the tray that ships TODAY still says en2zh/zh2en.
    // Accepting those two tokens on READ is what lets the helper be updated first and the
    // tray second -- which is exactly how this branch is being built. Doing it in unpackCtl
    // rather than in the helper's CTL handler means every future reader of this protocol
    // gets the compatibility for free instead of re-deriving it (and forgetting it).
    if      (c.mode == "en2zh") c.mode = "a2b";
    else if (c.mode == "zh2en") c.mode = "b2a";
    c.langA  = (t.size() >= 5) ? unesc(t[4]) : std::string();
    c.langB  = (t.size() >= 6) ? unesc(t[5]) : std::string();
    return true;
}

// ---- the CUE ENGINE's version (2026-10-11) -----------------------------------
// Bumped whenever what a FILE job produces changes in CONTENT or TIMING for the same input --
// not the wire format (that is what the appended fields are for), the cues themselves. The tray
// stamps it into every cache record it writes and treats a record from an OLDER engine as a
// miss (re-analysed; a record the user EDITED by hand is never discarded silently -- he is
// asked), so an improved engine actually reaches films analysed before it. Both ends compile
// from this tree, so one constant is the whole handshake.
//   1 = v1 (2026-10-10)   2 = v2, follow the playhead (2026-10-11)
//   3 = WF (2026-10-11): DTW word times, no split inside a word, language confined to the pair,
//       sound tags / credits dropped, readable durations, de-duplicated sentences
inline constexpr int kFileEngineVersion = 3;

// ---- FILE MODE (see the protocol block at the top) --------------------------
// Seven messages, one struct each, packed and unpacked HERE so the tray and the helper
// cannot disagree about a field's position. Every reader rejects jobId 0: it names no job,
// so it can only be a malformed line, and accepting it would let a stray message attach
// itself to whatever job the reader happens to be showing.
inline uint64_t jobIdOf(const std::string& s) { return ::_strtoui64(s.c_str(), nullptr, 10); }

// ---- media RANGES (v2): "s-e,s-e,..." in ms ---------------------------------------------
// ONE format for `have` on FILE and `ranges` on FPROG, so the tray's cache and the helper can
// never disagree about what "analysed" means. parseRanges is STRICT (ascending, non-
// overlapping, s < e, no junk): a malformed list is refused whole rather than half-read,
// because a wrong range is a stretch of film that silently never gets subtitles.
struct MsRange { uint64_t s = 0, e = 0; };

inline std::string packRanges(const std::vector<MsRange>& r) {
    std::string o;
    for (const MsRange& x : r) {
        if (!o.empty()) o += ',';
        o += std::to_string(x.s) + "-" + std::to_string(x.e);
    }
    return o;
}

inline bool parseRanges(const std::string& s, std::vector<MsRange>& out) {
    out.clear();
    if (s.empty()) return true;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t comma = s.find(',', at);
        const std::string one = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const size_t dash = one.find('-');
        if (dash == std::string::npos || dash == 0 || dash + 1 >= one.size()) { out.clear(); return false; }
        for (size_t i = 0; i < one.size(); ++i)
            if (i != dash && (one[i] < '0' || one[i] > '9')) { out.clear(); return false; }
        MsRange r;
        r.s = ::_strtoui64(one.substr(0, dash).c_str(), nullptr, 10);
        r.e = ::_strtoui64(one.substr(dash + 1).c_str(), nullptr, 10);
        if (r.e <= r.s || (!out.empty() && r.s < out.back().e)) { out.clear(); return false; }
        out.push_back(r);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return true;
}

// Union, in place: sorts and merges touching or overlapping ranges ([0-5000] + [5000-9000] ->
// [0-9000]). Both ends use it before packing, so a list on the wire is always canonical.
inline void mergeRanges(std::vector<MsRange>& r) {
    std::vector<MsRange> v;
    for (const MsRange& x : r) if (x.e > x.s) v.push_back(x);
    for (size_t i = 1; i < v.size(); ++i)                        // insertion sort: lists are short
        for (size_t j = i; j > 0 && v[j].s < v[j - 1].s; --j) std::swap(v[j], v[j - 1]);
    r.clear();
    for (const MsRange& x : v) {
        if (!r.empty() && x.s <= r.back().e) { if (x.e > r.back().e) r.back().e = x.e; }
        else r.push_back(x);
    }
}

inline uint64_t rangesTotal(const std::vector<MsRange>& r) {
    uint64_t t = 0;
    for (const MsRange& x : r) t += x.e - x.s;
    return t;
}

struct WireFile {
    uint64_t    jobId = 0;
    std::string path;              // absolute, UTF-8
    std::string langA, langB;      // "" = the helper's current half
    std::string mode;              // "" = current; else auto | a2b | b2a
    std::string quality;           // "" = auto; else auto | max | fast
    std::string have;              // v2: packRanges() of what is already subtitled; "" = none
    uint64_t    focusMs = 0;       // v2: where the playhead is
    std::string audio;             // WH: "" = the first sound track; else "<ord>:<trackId>" (packAudioSel)
};

// WH: the sound track a FILE analyses. ord = 0-based among the container's sound tracks (container
// order, undecodable ones included); trackId = the container's id for it (0 = unknown).
struct AudioSel { uint32_t ord = 0; uint32_t trackId = 0; };

inline std::string packAudioSel(const AudioSel& a) {
    return std::to_string(a.ord) + ":" + std::to_string(a.trackId);
}

// STRICT, like parseRanges: "" -> the first track {0, 0}; "<digits>:<digits>" (each 1-9 digits)
// -> that track; anything else -> false (the FILE is refused whole).
inline bool parseAudioSel(const std::string& s, AudioSel& out) {
    out = AudioSel{};
    if (s.empty()) return true;
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
    if (colon > 9 || s.size() - colon - 1 > 9) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != colon && (s[i] < '0' || s[i] > '9')) return false;
    out.ord     = (uint32_t)std::strtoul(s.substr(0, colon).c_str(), nullptr, 10);
    out.trackId = (uint32_t)std::strtoul(s.substr(colon + 1).c_str(), nullptr, 10);
    return true;
}

inline std::string packFile(const WireFile& f) {
    std::string o = "FILE\t" + std::to_string(f.jobId) + "\t" + esc(f.path) + "\t" + esc(f.langA)
                  + "\t" + esc(f.langB) + "\t" + esc(f.mode) + "\t" + esc(f.quality)
                  + "\t" + esc(f.have) + "\t" + std::to_string(f.focusMs);
    if (!f.audio.empty()) o += "\t" + esc(f.audio);   // WH: appended only when it says something
    return o;
}

inline bool unpackFile(const std::vector<std::string>& t, WireFile& f) {
    if (t.size() < 3 || t[0] != "FILE") return false;
    f.jobId   = jobIdOf(t[1]);
    f.path    = unesc(t[2]);
    f.langA   = (t.size() >= 4) ? unesc(t[3]) : std::string();
    f.langB   = (t.size() >= 5) ? unesc(t[4]) : std::string();
    f.mode    = (t.size() >= 6) ? unesc(t[5]) : std::string();
    f.quality = (t.size() >= 7) ? unesc(t[6]) : std::string();
    f.have    = (t.size() >= 8) ? unesc(t[7]) : std::string();
    f.focusMs = (t.size() >= 9) ? ::_strtoui64(t[8].c_str(), nullptr, 10) : 0ull;
    f.audio   = (t.size() >= 10) ? unesc(t[9]) : std::string();
    // A `have` that does not parse is a refusal of the whole FILE, not "none": analysing a film
    // the tray believes half-done from scratch is merely slow, but the opposite mistake -- a
    // reader that skips what it cannot read -- leaves holes nobody will ever fill. The same for
    // a malformed `audio` (WH): the wrong sound track would be subtitles of another dialogue.
    std::vector<MsRange> chk;
    AudioSel a;
    return f.jobId != 0 && !f.path.empty() && parseRanges(f.have, chk) && parseAudioSel(f.audio, a);
}

// v2: the playhead moved. One number, and stale ids are the reader's to drop.
inline std::string packFileFocus(uint64_t jobId, uint64_t ms) {
    return "FFOCUS\t" + std::to_string(jobId) + "\t" + std::to_string(ms);
}

inline bool unpackFileFocus(const std::vector<std::string>& t, uint64_t& jobId, uint64_t& ms) {
    if (t.size() < 3 || t[0] != "FFOCUS") return false;
    jobId = jobIdOf(t[1]);
    ms    = ::_strtoui64(t[2].c_str(), nullptr, 10);
    return jobId != 0;
}

inline std::string packFileCancel(uint64_t jobId) {
    return "FCANCEL\t" + std::to_string(jobId);
}

inline bool unpackFileCancel(const std::vector<std::string>& t, uint64_t& jobId) {
    if (t.size() < 2 || t[0] != "FCANCEL") return false;
    jobId = jobIdOf(t[1]);
    return jobId != 0;
}

struct WireProxy {
    uint64_t    jobId = 0;
    std::string src, out;          // absolute, UTF-8
};

inline std::string packProxy(const WireProxy& p) {
    return "FPROXY\t" + std::to_string(p.jobId) + "\t" + esc(p.src) + "\t" + esc(p.out);
}

inline bool unpackProxy(const std::vector<std::string>& t, WireProxy& p) {
    if (t.size() < 4 || t[0] != "FPROXY") return false;
    p.jobId = jobIdOf(t[1]);
    p.src   = unesc(t[2]);
    p.out   = unesc(t[3]);
    return p.jobId != 0 && !p.src.empty() && !p.out.empty();
}

struct WireFileProg {
    uint64_t    jobId = 0;
    std::string phase = "loading"; // loading | analyzing
    uint64_t    doneMs = 0;        // v1: the frontier; v2: the TOTAL analysed (= rangesTotal)
    uint64_t    totalMs = 0;       // duration; 0 = not known yet
    std::string asrModel, mtModel; // what THIS job runs on
    bool        hasVideo = false;
    std::string ranges;            // v2: packRanges() of what is analysed now (have included)
};

inline std::string packFileProg(const WireFileProg& p) {
    return "FPROG\t" + std::to_string(p.jobId) + "\t" + esc(p.phase)
         + "\t" + std::to_string(p.doneMs) + "\t" + std::to_string(p.totalMs)
         + "\t" + esc(p.asrModel) + "\t" + esc(p.mtModel) + "\t" + (p.hasVideo ? "1" : "0")
         + "\t" + esc(p.ranges);
}

inline bool unpackFileProg(const std::vector<std::string>& t, WireFileProg& p) {
    if (t.size() < 5 || t[0] != "FPROG") return false;
    p.jobId    = jobIdOf(t[1]);
    p.phase    = unesc(t[2]);
    p.doneMs   = ::_strtoui64(t[3].c_str(), nullptr, 10);
    p.totalMs  = ::_strtoui64(t[4].c_str(), nullptr, 10);
    p.asrModel = (t.size() >= 6) ? unesc(t[5]) : std::string();
    p.mtModel  = (t.size() >= 7) ? unesc(t[6]) : std::string();
    p.hasVideo = (t.size() >= 8 && t[7] == "1");
    p.ranges   = (t.size() >= 9) ? unesc(t[8]) : std::string();
    return p.jobId != 0;
}

struct WireCue {
    uint64_t    jobId = 0;
    uint32_t    idx = 0;           // 0,1,2... in emission order
    uint64_t    startMs = 0, endMs = 0;   // MEDIA time
    std::string lang, dstLang;     // as on LINE
    bool        pass = false;      // as on LINE: dst repeats src
    std::string src, dst;
};

inline std::string packCue(const WireCue& c) {
    return "FCUE\t" + std::to_string(c.jobId) + "\t" + std::to_string(c.idx)
         + "\t" + std::to_string(c.startMs) + "\t" + std::to_string(c.endMs)
         + "\t" + esc(c.lang) + "\t" + esc(c.dstLang) + "\t" + (c.pass ? "1" : "0")
         + "\t" + esc(c.src) + "\t" + esc(c.dst);
}

inline bool unpackCue(const std::vector<std::string>& t, WireCue& c) {
    if (t.size() < 10 || t[0] != "FCUE") return false;
    c.jobId   = jobIdOf(t[1]);
    c.idx     = (uint32_t)std::strtoul(t[2].c_str(), nullptr, 10);
    c.startMs = ::_strtoui64(t[3].c_str(), nullptr, 10);
    c.endMs   = ::_strtoui64(t[4].c_str(), nullptr, 10);
    c.lang    = unesc(t[5]);
    c.dstLang = unesc(t[6]);
    c.pass    = (t[7] == "1");
    c.src     = unesc(t[8]);
    c.dst     = unesc(t[9]);
    // A cue that ends before it starts is not a subtitle a player can schedule.
    return c.jobId != 0 && c.endMs >= c.startMs;
}

struct WireFileDone {
    uint64_t    jobId = 0;
    std::string result = "error"; // ok | cancelled | error
    uint32_t    count = 0;         // cues sent
    std::string err;               // "" or one human sentence
};

inline std::string packFileDone(const WireFileDone& d) {
    return "FDONE\t" + std::to_string(d.jobId) + "\t" + esc(d.result)
         + "\t" + std::to_string(d.count) + "\t" + esc(d.err);
}

inline bool unpackFileDone(const std::vector<std::string>& t, WireFileDone& d) {
    if (t.size() < 4 || t[0] != "FDONE") return false;
    d.jobId  = jobIdOf(t[1]);
    d.result = unesc(t[2]);
    d.count  = (uint32_t)std::strtoul(t[3].c_str(), nullptr, 10);
    d.err    = (t.size() >= 5) ? unesc(t[4]) : std::string();
    // Anything but the three known words is read as an error: a job whose end the tray cannot
    // classify must not be shown as a success.
    if (d.result != "ok" && d.result != "cancelled") d.result = "error";
    return d.jobId != 0;
}

struct WireProxyDone {
    uint64_t    jobId = 0;
    bool        ok = false;
    std::string out, err;
};

inline std::string packProxyDone(const WireProxyDone& d) {
    return "FPROXYDONE\t" + std::to_string(d.jobId) + "\t" + (d.ok ? "1" : "0")
         + "\t" + esc(d.out) + "\t" + esc(d.err);
}

inline bool unpackProxyDone(const std::vector<std::string>& t, WireProxyDone& d) {
    if (t.size() < 4 || t[0] != "FPROXYDONE") return false;
    d.jobId = jobIdOf(t[1]);
    d.ok    = (t[2] == "1");
    d.out   = unesc(t[3]);
    d.err   = (t.size() >= 5) ? unesc(t[4]) : std::string();
    return d.jobId != 0;
}

// ---------------------------------------------------------------------------
// Message I/O. BOTH ends use the deadlined (overlapped) flavour here -- neither
// side may ever block on the other, because "the other side has nothing to say"
// is the NORMAL state of a subtitle pipe. Handles must be opened
// FILE_FLAG_OVERLAPPED on both ends. Both grow on ERROR_MORE_DATA.
//
// THE OPTIONAL `wake` HANDLE (wave W1) is what makes a read deadline a deadline
// rather than a LATENCY. The helper's servant alternates "drain the outbox" and
// "read a CTL with a deadline"; with nothing but a deadline, a subtitle finished
// one microsecond after the read began waits the WHOLE deadline before the loop
// comes back round to the outbox. Handing the read an event the producer signals
// turns that wait into microseconds. It is optional and defaults to nullptr, so
// every pre-W1 CALL SITE is unchanged.
//
// The call sites are unchanged; the BEHAVIOUR of the timeout path is not, and the
// difference is a fix rather than a regression. The cancel branch now honours what
// the reap says for EVERY caller, tray included: a message that completed in the
// window between the deadline expiring and CancelIoEx landing used to be thrown away
// (the old code returned false without looking), and it is now delivered. Same for
// the error the caller reads afterwards -- see ::SetLastError below.
// ---------------------------------------------------------------------------
inline bool overlappedStep(HANDLE pipe, bool isRead, void* buf, DWORD len, DWORD ms,
                           DWORD& transferred, bool& moreData,
                           HANDLE wake = nullptr, bool* woken = nullptr) {
    transferred = 0; moreData = false;
    if (woken) *woken = false;
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    const BOOL started = isRead ? ::ReadFile(pipe, buf, len, nullptr, &ov)
                                : ::WriteFile(pipe, buf, len, nullptr, &ov);
    if (!started) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_MORE_DATA) {                  // read completed with a partial message
            ::GetOverlappedResult(pipe, &ov, &transferred, FALSE);
            ::CloseHandle(ov.hEvent);
            moreData = true;
            return true;
        }
        if (e != ERROR_IO_PENDING) { ::CloseHandle(ov.hEvent); return false; }
        // Two ways to stop waiting: the deadline expires (the pipe's RESTING state, not a
        // fault) or `wake` fires because this thread has something to SAY. Identical
        // cleanup, because the I/O is in the same state either way -- still pending.
        // WaitForMultipleObjects returns the LOWEST signalled index, so an arriving
        // message always wins a tie against the wake: the CTL is never traded for it.
        DWORD w;
        if (wake) {
            HANDLE hs[2] = { ov.hEvent, wake };
            w = ::WaitForMultipleObjects(2, hs, FALSE, ms);
        } else {
            w = ::WaitForSingleObject(ov.hEvent, ms);
        }
        if (w != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe, &ov);
            DWORD got = 0;
            const BOOL done = ::GetOverlappedResult(pipe, &ov, &got, TRUE);   // reap the cancel
            const DWORD ge  = done ? 0u : ::GetLastError();
            ::CloseHandle(ov.hEvent);
            // ⚠️ A MESSAGE THAT RACED IN DURING THE CANCEL IS STILL A MESSAGE. CancelIoEx
            // cannot un-deliver one, so the reap's own verdict is honoured before we report
            // "nothing arrived" -- otherwise waking the servant to write would occasionally
            // swallow the CTL that was already on its way in. (The old deadline-only path
            // had this hole too; it was invisible because a 200 ms window is hit far less
            // often than a wake that fires on every subtitle.)
            if (done || ge == ERROR_MORE_DATA) {
                transferred = got;
                moreData = (ge == ERROR_MORE_DATA);
                return true;
            }
            // ⚠️ RESTORE THE REAP'S ERROR. ::CloseHandle above is a Win32 call like any
            // other and is entitled to overwrite the thread's last error -- so the caller
            // (readMessage, classifying "the tray went away" against "nothing arrived")
            // would be reading CloseHandle's verdict, not the I/O's. It succeeded far more
            // often than not, which is precisely why this never showed up as a bug: a
            // broken pipe would simply look like a timeout, forever.
            ::SetLastError(ge);
            // ...and "woken" means WOKEN, not merely "did not complete". A pipe that broke
            // while the wake happened to be signalled must be reported as BROKEN: the
            // supervisor's only exit condition is that verdict. ERROR_OPERATION_ABORTED is
            // what a reaped CancelIoEx returns, i.e. the one reap result that says the read
            // was still healthy when we took it away from it.
            if (woken && wake && w == WAIT_OBJECT_0 + 1 && ge == ERROR_OPERATION_ABORTED)
                *woken = true;
            return false;
        }
    }
    if (!::GetOverlappedResult(pipe, &ov, &transferred, TRUE)) {
        moreData = (::GetLastError() == ERROR_MORE_DATA);
        ::CloseHandle(ov.hEvent);
        return moreData;
    }
    ::CloseHandle(ov.hEvent);
    return true;
}

inline bool writeMessage(HANDLE pipe, const std::string& msg, DWORD ms = 3000) {
    DWORD n = 0; bool more = false;
    return overlappedStep(pipe, false, const_cast<char*>(msg.data()), (DWORD)msg.size(),
                          ms, n, more)
        && n == msg.size();
}

// Returns false on BOTH "nothing arrived within `ms`" and "the pipe broke"; the
// caller distinguishes them by checking whether the child is still alive (the
// supervisor) or whether the handle still works (the child). `broken` reports
// the difference when the caller cares.
//
// `wake` (W1, optional) cuts the wait short so the caller can go and WRITE; when it
// is what ended the wait, `*woken` says so and `*broken` stays false -- a wake is
// never a fault. Defaults keep every pre-W1 call site behaving exactly as before.
inline bool readMessage(HANDLE pipe, std::string& out, DWORD ms, bool* broken = nullptr,
                        HANDLE wake = nullptr, bool* woken = nullptr) {
    out.clear();
    if (broken) *broken = false;
    if (woken)  *woken  = false;
    char buf[8192];
    for (;;) {
        DWORD got = 0; bool more = false;
        // The wake is honoured only BEFORE the first chunk. Abandoning a message halfway
        // through an ERROR_MORE_DATA sequence would throw away the half already read, and
        // a transcript is exactly the kind of message that can outgrow the buffer.
        const HANDLE w = out.empty() ? wake : nullptr;
        if (!overlappedStep(pipe, true, buf, sizeof(buf), ms, got, more, w, woken)) {
            const DWORD e = ::GetLastError();
            if (broken && (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED
                           || e == ERROR_NO_DATA || e == ERROR_INVALID_HANDLE))
                *broken = true;
            return false;
        }
        out.append(buf, got);
        if (!more) return true;
    }
}

} } // namespace pbt::trpipe

// =============================================================================
// SuVtitle - engine/TrMedia.h   (FILE MODE, wave WB: a media file's audio, on its timeline)
//
// SUVTITLE SPLIT (2026-10-23): ported VERBATIM from AudioPB's PatchBus repo, translator/TrMedia.h at commit
// 641f1aa (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Only this header and the comments' non-ASCII
// characters changed (house rule: ASCII-only C++). "Our own Decimator" below is AudioPB's live front end
// (TrAudio.h), which did not come along.
//
// What the FILE job and the FPROXY worker read a recording with. Two decoders behind one
// reader, and ONE rule above them both: frame N of what comes out IS media time N / rate.
//
//   * MEDIA FOUNDATION for everything Windows can open (measured 10/10: wav mp3 m4a aac flac
//     wma ac3 ogg opus mka mp4 mov m4v 3gp mkv avi wmv mpg ts m2ts webm, 5.1 AAC included).
//     The Source Reader is asked for the format we want and does the conversion itself: for
//     the ASR that is float / 16 kHz / MONO, and its Resampler DSP measured better than our
//     own Decimator on every count -- flat to 7.2 kHz (-0.14 dB; ours -35.8 dB), alias
//     -105 dB (ours -102), zero delay, and 220-950x real time in SYSTEM code, so a Debug
//     build of this exe decodes a film as fast as a Release one. Video is deselected before
//     the first read and never decoded.
//   * AIFF / AIFC, which Media Foundation cannot open at all (0xC00D36C4 on all five
//     variants): our own parser, streaming SSND, feeding THE SAME Resampler DSP driven as a
//     bare MFT -- measured bit-identical to the Source Reader's output (max |diff| = 0), so an
//     AIFF and the WAV it came from produce the very same samples.
//
// THE TIMELINE (the cues must land where the player is):
//   * a first sample stamped after 0 is preceded by that much silence (measured: wma 46 ms,
//     avi 26 ms, mpg 529 ms, ts 757 ms, m2ts 774 ms);
//   * a forward gap in the stamps over 100 ms is filled with silence (30 s cap);
//   * /!\ MEDIA FOUNDATION IGNORES MP4/MOV EDIT LISTS. A track whose 'elst' starts with an
//     empty edit (audio that begins 1.476 s into the presentation, ffmpeg -itsoffset) came
//     out at 0, and AAC priming (21.3 ms) was kept. The reader reads the edit list of the sound
//     track it DECODES (WH; before, always the first one's) and applies it: lead = the empty edits,
//     trim = the first edit's media_time. Without it a video's subtitles run 1.5 s early -- "out
//     of sync".
//   * mp3 encoder delay (+36 ms, LAME tag) is NOT corrected: under one video frame.
// The FPROXY copy is written on this same timeline, so a cue lines up on the proxy exactly as
// on the original.
//
// SEEKING (FILE MODE v2, wave WD -- the job follows the playhead). seek(frame) puts the 16 kHz
// stream at that frame of THE SAME timeline a linear decode produces -- lead, edit list and gaps
// included -- measured 10/10 against linear decodes (selftest [8], 2 h files in the WD log):
//   * the Mono16k chain is Media Foundation at the NATIVE rate, mono, then OUR Resampler DSP
//     (the bare MFT the AIFF path already drives) anchored on the stream's first decoded sample.
//     Measured BIT-IDENTICAL to Media Foundation's own float/16k/mono, so a linear decode is
//     unchanged -- and it is what makes a seek exact: after a seek the resampler is restarted on
//     a PERIOD of the rate ratio (441 in / 160 out at 44.1 kHz) a few ms before the target, and
//     from the target on its output equals the linear one to the float LSB (<= 2.4e-7).
//     Media Foundation's own resampler restarts wherever the decoder lands: 0.05-0.14 error;
//   * WHERE Media Foundation lands is trusted only where it was measured exact: RIFF/WAVE,
//     ISO-BMFF (mp4/m4a/mov) and Matroska/WebM (stamps in ms: anchored by the midrange of 1 s of
//     stamps). /!\ ITS MP3 SEEK IS WRONG: on a 2 h VBR file, at 1:20:00 the audio it returned was
//     9.4 s later than its stamp said -- subtitles 9.4 s early. MP3 and every other container
//     (MPEG-TS/M2TS, ADTS, AVI, ASF, FLAC, Ogg...) therefore seek by DECODING: forward from where
//     the reader is, or re-opened and decoded from the start when the target is behind (2.9 s to
//     reach 1:20:00 in a 2 h MP3). Slower, exact by construction. MPEG-TS is decoded too (review
//     P2): a broadcast capture loses PES packets (21 ms each, under the 100 ms the linear decode
//     fills) and resets its PTS -- the linear decode counts, the container seek stamps, and the
//     two drift apart. A trusted file whose stamps leave the count the same way (a frame lost, a
//     reset, a jump past 30 s) is DEMOTED when the linear decode meets it: decoded from then on;
//   * AIFF (our parser) seeks by byte offset.
//   * A SEEK IS ABORTABLE (review P3): the caller's predicate is polled between decoded buffers
//     (20-100 ms of audio each), so a decode-to-seek never holds FCANCEL / QUIT / a new playhead
//     for seconds. An aborted seek leaves the reader to be seeked again (it re-opens).
//
// WHICH SOUND TRACK (wave WH, 2026-10-11 -- TrPipe.h, FILE's `audio`). A film carries several (the user's
// MKV: English 5.1, English stereo, French, Japanese, Norwegian) and the FILE job analyses the one the
// viewer HEARS. Media Foundation does not number them the way the container does -- an MP4 even lists its
// sound tracks BACKWARDS under numbers of its own -- so the reader finds the track the tray names by
// evidence measured container by container (TrMedia.cpp, "WHICH SOUND TRACK"), keeps it across every
// re-open a seek makes, and refuses -- one sentence naming the format -- a track it cannot find or decode:
// the subtitles of a dialogue nobody hears are worse than none.
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbt {

struct MediaInfo {
    uint64_t    durationMs     = 0;      // the container's duration; 0 = unknown
    bool        hasVideo       = false;  // a real PICTURE track (cover art does not count)
    uint32_t    nativeRate     = 0;      // the first audio stream, as stored
    uint32_t    nativeChannels = 0;
    std::string codec;                   // for logs: "AAC", "MP3", "AIFF twos 16-bit", ...
    bool        aiff           = false;  // read by our own parser, not Media Foundation
    double      leadSec        = 0.0;    // ISO-BMFF edit list: where the audio starts ...
    double      trimSec        = 0.0;    // ... and how much decoded audio precedes that
    // WD: how this file SEEKS (see the header): "mf" (the container's seek is trusted), "aiff"
    // (byte offset) or "decode" (decoded forward / re-opened -- MP3 and every other container).
    std::string seekMode;
    // WH: THE SOUND TRACK DECODED, as found (TrMedia.cpp "WHICH SOUND TRACK") -- the log, the selftest.
    // nativeRate / nativeChannels / codec / leadSec / trimSec above are ITS facts.
    uint32_t    audioStreams = 0;     // sound tracks Media Foundation lists in this file
    uint32_t    streamIndex  = 0;     // the Source Reader's index of the one decoded
    int         trackOrd     = -1;    // its place among the container's sound tracks (-1 = not known)
    uint32_t    trackId      = 0;     // its container id: Matroska TrackNumber / ISO-BMFF track_ID (0 = not known)
    std::string trackLang;            // MF_SD_LANGUAGE, ISO 639-1 ("en", "fr"; "" = none)
    std::string trackName;            // MF_SD_STREAM_NAME (the Matroska Name; "" = none)
    // "first" (MF_SOURCE_READER_FIRST_AUDIO_STREAM, the pre-WH reader), "mkv-first", "mkv-id", "mkv-ord",
    // "iso-stsd", "iso-order", "aiff"
    std::string trackHow;
};

// WH: which sound track a reader decodes. chosen = false: "the first sound track" -- FILE's "" and every
// caller before WH (FPROXY included). chosen = true: ord = 0-based among the CONTAINER's sound tracks in
// container order, the undecodable ones included; trackId = the container's own id for it (Matroska
// TrackNumber, ISO-BMFF track_ID), 0 = unknown. TrPipe.h's AudioSel, without the protocol.
struct AudioTrack {
    bool     chosen  = false;
    uint32_t ord     = 0;
    uint32_t trackId = 0;
};

class MediaReader {
public:
    // Mono16k: float, 16 kHz, 1 ch (the ASR). Pcm16Native: int16 interleaved, the source's
    // rate, min(channels, 2) (the FPROXY copy).
    enum class Out { Mono16k, Pcm16Native };

    MediaReader();
    ~MediaReader();
    MediaReader(const MediaReader&) = delete;
    MediaReader& operator=(const MediaReader&) = delete;

    // Opens AND PROBES the file: duration, hasVideo, the audio format. `err` is one human
    // sentence when it fails. Uses COM (MTA) and Media Foundation on the CALLING thread:
    // open, read and close must all happen on one thread. Decodes the FIRST sound track.
    bool open(const std::wstring& path, Out out, std::string& err);
    // WH: decodes the sound track `track` names (see AudioTrack), and every re-open a seek makes keeps
    // it. A chosen track that cannot be found, or cannot be decoded, is an error whose sentence names it
    // ("This sound track (DTS) can't be decoded for the analysis...") -- never another track instead.
    bool open(const std::wstring& path, Out out, const AudioTrack& track, std::string& err);
    void close();
    const MediaInfo& info() const;
    uint32_t outRate() const;
    uint32_t outChannels() const;

    // Appends up to `maxFrames` frames ON THE MEDIA TIMELINE (see the header comment).
    // false = a decode error (`err` set). eof() once everything has been handed out.
    bool readFloat(std::vector<float>& out, size_t maxFrames, std::string& err);    // Mono16k
    bool readPcm16(std::vector<int16_t>& out, size_t maxFrames, std::string& err);  // Pcm16Native
    bool     eof() const;
    uint64_t position() const;          // frames handed out so far = the timeline position

    // FILE MODE v2 (WD). Mono16k ONLY. Puts the stream at `frame` (16 kHz, media timeline): the
    // next readFloat returns that very frame, the samples a linear decode would have returned
    // there (see the header for the precision per container). false = a decode error (`err`),
    // a PCM16 reader -- or ABORTED: `abort` returned true between two decoded buffers
    // (lastSeekAborted(), `err` empty); the reader must then be seeked again before any read.
    // A frame past the end leaves the reader at eof().
    bool seek(uint64_t frame, std::string& err, const std::function<bool()>& abort = {});
    bool lastSeekAborted() const;
    // How the last seek got there, for the log and the selftest: "none" (already there),
    // "pending" (inside what was decoded), "mf", "aiff", "forward" (decoded forward), "reopen"
    // (re-opened, decoded from the start), "start" (a target in the first seconds: from the start),
    // "end" (the audio ends before the target: the reader is at its end, as a linear decode would be).
    const char* lastSeekHow() const;
    // Native frames the last seek DECODED to get there (the cost; the selftest bounds it).
    uint64_t lastSeekDecoded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// FPROXY: a playable copy of `src`'s audio at `out` -- 16-bit PCM WAV, the source's rate, at
// most 2 channels, on the MEDIA timeline (leading silence and edit lists included), RF64
// past the 4 GiB RIFF limit. Written to `out`.tmp then renamed (the house AV retry);
// `cancel` is polled every block (about one second of audio). Nothing is left behind on
// Cancelled or Error.
enum class ProxyResult { Ok, Cancelled, Error };
struct ProxyFacts { uint64_t frames = 0; uint32_t rate = 0, channels = 0; bool rf64 = false; };
ProxyResult writeProxyWav(const std::wstring& src, const std::wstring& out,
                          const std::atomic<bool>& cancel, std::string& err,
                          ProxyFacts* facts = nullptr);
// The RF64 threshold, overridable ONLY by the selftest (so it can prove the RF64 branch
// without writing 4 GiB). 0 = the real limit.
void setProxyRf64ThresholdForTest(uint64_t dataBytes);
// MEDIA FOUNDATION MAY BE ABSENT (Windows N / KN without the Media Feature Pack): mfplat.dll and
// mfreadwrite.dll are DELAY-loaded (build/CMakeLists.txt) and every reader first checks they
// exist, answering one human sentence instead of failing to start. The selftest names a DLL
// that does not exist here ("" = the real one) to prove the refusal.
void setMediaFoundationDllForTest(const std::wstring& dll);

// ISO-BMFF / QuickTime: the first SOUND track's edit list. true when the file is one of
// those and its edit list moves the audio (lead and/or trim > 0). (WH: the reader applies the
// edit list of the track it DECODES -- this one is the first sound track's, for the selftest.)
bool isoBmffAudioEdit(const std::wstring& path, double& leadSec, double& trimSec);
// Is this an AIFF / AIFC file (by content, not by name)?
bool isAiffFile(const std::wstring& path);

} // namespace pbt

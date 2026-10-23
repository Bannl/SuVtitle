// =============================================================================
// SuVtitle - engine/TrEngine.h   (the two child servers, and the text pipeline)
//
// SUVTITLE SPLIT (2026-10-23): ported from AudioPB's PatchBus repo, translator/TrEngine.h at commit 641f1aa
// (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe -- AudioPB's helper minus its LIVE path. Changed here:
// TrPaths lost the diarization runtime (sherpa, the speaker model: no voices to tell apart in a file job);
// TrConfig became %APPDATA%\SuVtitle\engine.txt and kept only what a FILE job still reads (+ DEVICE, S11);
// kAsrRate moved in from TrAudio.h (the fabric front end did not come along); the `auto` preset's live probe
// (probeAutoPreset) is gone with the live pair it timed. Everything else is 641f1aa's, comments included
// ("the live line", "a draft"... describe AudioPB, where those paths still run).
//
// The ASR and the MT are OTHER PEOPLE'S BINARIES (whisper.cpp's whisper-server and
// llama.cpp's llama-server). We DRIVE them, we do not vendor them: nothing of
// theirs is compiled into SuVtitle, the payload is assembled by the bundle script,
// and a new upstream release is a file copy rather than a merge.
//
// /!\ THE ENDPOINT CONVICTION, HONOURED AS FAR AS IT GOES: these are HTTP servers,
// which is not our preferred shape for anything. So they are bound to 127.0.0.1
// on an EPHEMERAL port we pick ourselves by bind-probing, spawned as our own
// children with CREATE_NO_WINDOW, put in a JOB OBJECT that kills them if this
// process dies for any reason (including a debugger stop), and killed on the way
// out. Nothing is installed, nothing is a service, nothing survives us, and
// nothing outside this machine can reach them.
//
// GPU vs CPU is DECIDED, not assumed. Both binaries are Vulkan builds that also
// carry CPU back-ends, and the honest way to know which one this machine can run
// is to start the server and see. So: spawn with the GPU flags, wait for /health;
// if that fails, kill and respawn with the explicit CPU flags (-ng for whisper,
// "-dev none" for llama -- both verified on this machine). Whichever came up is
// reported in STATE, so the UI can say "CPU mode" instead of just feeling slow.
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbt {

// Every ASR model we drive wants exactly this. (SUVTITLE SPLIT: was TrAudio.h's -- "one
// definition of 16 kHz, shared with the front end" -- and the live front end stayed in AudioPB.)
constexpr int kAsrRate = 16000;

// ---------------------------------------------------------------------------
// Where the binaries and models live. The DEPLOYED layout wins; a dev payload named by the environment variable
// SUVTITLE_PAYLOAD is the fallback, so this exe can run from a build tree with nothing bundled yet.
//   deployed (SuVtitle, 2026-10-23):  <exe dir>\engine\whisper\whisper-server.exe (+ its DLLs)
//                                      <exe dir>\engine\llama\llama-server.exe     (+ its DLLs)
//                                      <exe dir>\models\                           (ggml-*.bin, *.gguf)
//   dev payload (SUVTITLE_PAYLOAD=<root>, since the publication wave, 2026-10-24; deploy\make_bundle.js reads the SAME
//   layout from --payload / SUVTITLE_PAYLOAD):
//                                      <root>\whisper\whisper-server.exe (+ its DLLs)
//                                      <root>\llama\llama-server.exe     (+ its DLLs)
//                                      <root>\models\                    (ggml-*.bin, *.gguf)
// THE BUNDLE OR THE DEV PAYLOAD, NEVER A MIX (fix round RV3-3, 23/10): as soon as ANY of the three bundle folders
// is next to the exe, the bundle is the only source -- a part missing from it is NAMED (`missing`: STATE error,
// FDONE) instead of being borrowed from the dev payload, which would mix shipped binaries with dev models and hide a
// packaging bug on the one machine where it can be caught. The dev payload serves only an exe with NOTHING next to
// it (a build tree), and holds to the same rule inside itself: as soon as ANY of its three folders exists, it is the
// only source and a missing part is named. There is no built-in path to a payload any more (the author's own tree used to
// be one). engine.txt's WHISPERBIN / LLAMABIN / MODELS still win, part by part, when they resolve.
// ---------------------------------------------------------------------------
struct TrPaths {
    std::wstring whisperExe;    // ...\whisper-server.exe
    std::wstring llamaExe;      // ...\llama-server.exe
    std::wstring modelsDir;     // holds ggml-*.bin + the Qwen .gguf
    bool         devPayload = false;    // nothing of the bundle next to the exe: the SUVTITLE_PAYLOAD tree was used
    std::string  missing;               // "" when ok(); else the user-visible sentence naming what is missing
    bool ok() const { return !whisperExe.empty() && !llamaExe.empty() && !modelsDir.empty(); }
};

// Config file %APPDATA%\SuVtitle\engine.txt (S11). Tags: PRESET, MODE, LANGA, LANGB,
// WHISPERBIN, LLAMABIN, MODELS, MTMODEL, MTSTYLE, ASRPROMPT, MTCONTEXT, DEVICE. Written with
// the house's AV-safe temp+rename+retry dance.
//
// THE ENGINE OWNS THIS FILE (S11): the app never writes it -- it sends CTL, and the engine
// persists -- and reads it only for the pair. In AudioPB the tray wrote its window geometry
// (WINPOS) into the same file, which is why `extra` exists: it carries every line this struct
// does not understand, verbatim, straight back out on save. Kept, so a hand-edited line (or
// one of AudioPB's live tags, SPK* / PARTIAL* / COMMIT*, in a file copied over) is never lost.
struct TrConfig {
    // SUVTITLE: no live pair runs any more, so the preset no longer chooses what is loaded at idle.
    // It is kept because a FILE job still reads it when turbo is NOT installed: the job then runs on
    // this preset's FINAL model (Servers::presetFinalModel), exactly as it ran on the live preset's
    // in AudioPB. auto = the best preset whose models are installed.
    std::string  preset = "balanced";   // auto | quality | balanced | light
    std::string  mode   = "auto";       // auto | a2b | b2a  (en2zh/zh2en migrate on load)
    // ---- the PAIR (W2). The mode is a DIRECTION over these two, which is what turns a
    // hard-coded en<->zh feature into a translator: a2b means "A into B" whatever A and B
    // are. The defaults are the two languages the whole thing was benched on.
    std::string  langA  = "en";         // LANGA  ISO-639-1, or "zh-tw" (see normalizeLangCode)
    std::string  langB  = "zh";         // LANGB
    std::wstring whisperBin;            // explicit override of the bin FOLDER ("" = search)
    std::wstring llamaBin;
    std::wstring models;                // explicit override of the models FOLDER
    // ---- the MT driver (W2). MTMODEL empty = "whatever this preset calls for"
    // (mtModelFor below); set it and it wins for EVERY preset, which is what makes an A/B
    // of a bigger model a one-line edit of this file instead of a rebuild.
    std::string  mtModel;               // MTMODEL   bare filename (in the models dir) or abs path
    std::string  mtStyle;               // MTSTYLE   qwen | hy  ("" = deduced from the filename)
    bool         asrPrompt = true;      // ASRPROMPT feed whisper a glossary + the last final
    int          mtContext = 2;         // MTCONTEXT prior sentence PAIRS given to the MT (0-4;
                                        //           a FILE job takes at least 3)
    // ---- GPU / CPU (SUVTITLE, S11 -- new: AudioPB had no such key, the GPU was decided by
    // trying and only a TEST could force the CPU). auto = exactly that: each child server on
    // the GPU (Vulkan) when it comes up there, else on the CPU. cpu = every child on the CPU and
    // the GPU never even opened -- the same mechanism as the test switch --cpu (forceCpuChildren
    // in main_translator.cpp, measured 0 MB of VRAM), for a user who keeps his GPU for something
    // else. Read at start: a change takes effect at the engine's next start.
    std::string  device = "auto";       // DEVICE    auto | cpu
    // (SUVTITLE: AudioPB's live tags SPK* (diarization), PARTIAL* (the live line) and COMMIT*
    // (commit as you go) are gone with the live path; a file that holds them keeps them in
    // `extra`, untouched.)
    std::vector<std::string> extra;     // lines this struct does not own, written back verbatim

    static std::wstring path();
    void load();
    void save() const;
};

// FIRST START (SUVTITLE, director 23/10 -- S-HOST's Q1). The analyses SuVtitle.exe imports from AudioPB (its
// first-run import, S4) are filed under the language PAIR, and that pair lived in AudioPB's translator.txt. So the
// engine's FIRST engine.txt takes MODE / LANGA / LANGB, and PRESET (the one "quality" key a FILE job read: the
// turbo-absent fallback), from %APPDATA%\AudioPB\translator.txt -- READ ONLY on AudioPB's file. ALL OR NOTHING:
// a file that cannot be read, a present key whose value does not read cleanly (by the same vocabulary as
// TrConfig::load: normalizeLangCode, auto|a2b|b2a with en2zh/zh2en migrated, the four presets) or a pair of one
// language = no seed at all, the defaults -- a half-read pair would file the imports under a pair the user never
// had. Never fatal. The CALLER decides "first" (engine.txt absent) and that it is not a test engine.
// FIX ROUND (RV2-3, the user's REAL data, 23/10): his translator.txt holds MODE and PRESET but NO pair -- the pair
// went into each analysis instead -- so a translator.txt WITHOUT a valid pair hands MODE / LANGA / LANGB to the
// NEWEST of AudioPB's analyses (%LOCALAPPDATA%\AudioPB\Translator files\*.cues, by last-write time): the JOB line of
// its head, read the same way (all or nothing, read only); translator.txt's PRESET still comes along when valid.
// A pair translator.txt DOES give always wins. No valid analysis either: translator.txt's MODE / PRESET (as before).
// BOUNDED (RV3-2): translator.txt is read up to 64 KB (larger = not it: nothing from it), a record's head up to
// 16 KB -- never more, whatever the file.
// Returns true when `cfg` was seeded; `why` = one line for the log, either way.
bool seedFromAudioPB(TrConfig& cfg, std::string& why);

// Resolve the payload, honouring the config's overrides. Returns a TrPaths whose
// ok() is false when nothing usable was found (the caller reports that as an error
// STATE rather than crashing on a spawn) -- `missing` then names what is missing.
// The bundle is looked for next to the exe, the dev payload in the environment variable SUVTITLE_PAYLOAD.
TrPaths resolvePaths(const TrConfig& cfg);
// ... with the bundle looked for next to `root` (a folder, trailing backslash) instead of the exe, and the dev payload in
// `payload` (a folder; "" = none) instead of the environment: the checks build every layout under %TEMP% (RV3-3).
TrPaths resolvePathsAt(const TrConfig& cfg, const std::wstring& root, const std::wstring& payload);

// ---------------------------------------------------------------------------
// A PRESET IS A PAIR OF MODELS, not one (W2). The 17/09 bench is the reason: at
// audio_ctx=768 an ASR call costs the same whatever the audio length -- ~1.15 s on
// large-v3-turbo, ~0.3 s on small, ~0.2 s on base -- so a draft of 1.5 s of speech costs
// exactly what a final of 8 s costs. Running the live line on the SAME model as the
// finals therefore taxes every draft at the final's price. Two models, one cheap and one
// good, is the only shape that buys accuracy without buying latency with it.
//
// They are two SERVERS (never one server switched over: whisper-server holds one model),
// and they are used STRICTLY ONE AT A TIME from the pipeline thread -- the same bench
// measured two whisper-servers running concurrently as SLOWER than the same two calls in
// sequence, because the GPU is one resource and the scheduler is not ours.
// ---------------------------------------------------------------------------
struct AsrModels {
    std::string draft;   // the live line's model: cheap, wrong more often, never the record
    std::string final;   // the subtitle's model: what the user actually reads
    bool same() const { return draft == final; }
};
AsrModels   asrModelsFor(const std::string& preset);
// "small" when both halves are the same file, "small->large-v3-turbo" when they differ --
// ONE string, because STATE.asrModel is one field and the tray shows it in a tooltip.
std::string asrModelsLabel(const AsrModels& m);

// The MT model a preset calls for. quality/balanced get the 4B (measured 95 t/s on this
// GPU = 140-280 ms a sentence, still far from being the bottleneck); light keeps the 1.5B
// it was always given, because light exists for machines that have no GPU to spend.
// Returns the BARE FILENAME; the caller resolves it in the models dir and falls back to
// kMtModelFileSmall when the file is not there.
std::string mtModelFor(const std::string& preset);
inline const char* kMtModelFile      = "Qwen3-4B-Instruct-2507-Q5_K_M.gguf";
inline const char* kMtModelFileSmall = "qwen2.5-1.5b-instruct-q4_k_m.gguf";

// The MT prompt driver. Two models, two shapes -- and they are NOT interchangeable:
//   qwen = system prompt (interpreter role + the audio glossary) + optional context turns;
//   hy   = Tencent's bare instruction, NO system prompt and NO context (measured: HY-MT
//          translates the context along with the line, which is unusable as-is).
// Deduced from the filename when MTSTYLE is not set, so dropping a GGUF into the models
// folder and naming it in translator.txt is the whole A/B procedure.
std::string mtStyleFor(const std::string& modelFile);

// ---------------------------------------------------------------------------
// One supervised child HTTP server on 127.0.0.1.
// ---------------------------------------------------------------------------
class ChildServer {
public:
    ~ChildServer() { kill(); }

    // Spawn `exe` with `args` (the port is already inside args), then poll
    // http://127.0.0.1:<port>/health until it answers or `readyMs` elapses.
    // Returns false if the process died or never became ready.
    //
    // `abort` (wave WB, optional): polled with the /health probe, every ~200 ms. When it reads
    // true the child is killed and spawn returns false at once. A FILE job loads large-v3 --
    // 3 GB off disk -- and an FCANCEL or a QUIT must not wait for that load to finish before
    // it is honoured. nullptr (every live call site) is exactly the old behaviour.
    bool spawn(const std::wstring& exe, const std::wstring& workDir,
               const std::wstring& args, uint16_t port, DWORD readyMs,
               const std::function<bool()>& abort = {});
    void kill();

    bool     alive() const;
    uint16_t port()  const { return port_; }

private:
    HANDLE   proc_ = nullptr;
    HANDLE   job_  = nullptr;
    uint16_t port_ = 0;
};

// A free localhost TCP port, found by BINDING one (asking the OS is the only
// answer that is not a race with every other program on the machine). 0 on failure.
uint16_t findFreePort();

// ---------------------------------------------------------------------------
// Minimal WinHTTP client, localhost only, no proxy. Returns false on transport
// failure; `status` carries the HTTP code when it got that far.
// ---------------------------------------------------------------------------
bool httpPost(uint16_t port, const wchar_t* path, const wchar_t* contentType,
              const void* body, size_t bodyLen, std::string& out, DWORD timeoutMs,
              int* status = nullptr);
bool httpGet(uint16_t port, const wchar_t* path, std::string& out, DWORD timeoutMs,
             int* status = nullptr);

// ---------------------------------------------------------------------------
// The text pipeline. Both calls are BLOCKING and belong on the pipeline thread.
// ---------------------------------------------------------------------------

// 16 kHz mono float -> a 16-bit PCM WAV in memory, peak-normalised to -3 dBFS.
// Normalisation is not cosmetic: the mono downmix of a 32-lane matrix can land
// anywhere from -40 dBFS to clipping depending on how the user patched it, and an
// ASR fed either extreme transcribes noise.
std::string buildWav16k(const std::vector<float>& mono);

// ONE word, as whisper-server's verbose_json timestamps it. Seconds, relative to the WAV
// that was sent -- never to the segment, never to the clock: the caller knows where the
// WAV sat and is the only one who can place these on either.
//
// /!\ A "word" is whatever the tokenizer called one. In Latin scripts it carries its LEADING
// SPACE (" Good"); in Chinese it may be one ideograph or several, and the split moves
// between two decodes of the same audio -- which is exactly why the agreement that consumes
// these normalises per CHARACTER there (main_translator.cpp) instead of trusting the split.
struct AsrWord {
    std::string text;
    double      start = 0.0;   // seconds into the WAV
    double      end   = 0.0;
    double      prob  = 0.0;   // whisper's own confidence, 0-1 (carried, not yet used)
    // WAVE WF: the token's DTW time, seconds into the WAV; -1 = none (a server started without
    // `-dtw`, every LIVE server). /!\ It is the END of the token, not its start: whisper.cpp places
    // the jump of its alignment path INTO row k+1 on token k (row 0 is <|notimestamps|>, which
    // predicts the first text token). Measured on the film (11/10, 362 words against an
    // independent large-v3 alignment): read as a start it is +0.24 s late on median; read as an
    // end it is 0.06 s off on median, 0.12 s at p90 -- where t0/t1 (whisper's token-timestamp
    // heuristic) were 0.46 s at p90 and up to 15.7 s off over music.
    double      dtw   = -1.0;
};

// ONE of whisper's own segments (wave WB). The FILE job's commit rule is written in these:
// after a window cut mid-speech, everything but the last one is final (TrFileJob.h).
// Times are seconds into the WAV, like AsrWord's.
//
// /!\ noSpeech IS CARRIED, NOT TRUSTED. This whisper-server emits no_speech_prob per segment,
// and it measured 0.000 on 20 s of pink noise and on a music chord (10/10, turbo) -- the very
// inputs it exists to flag. Nothing may gate on it; the job's junk control is the energy gate
// plus the structural filter (looksLikeHallucination).
struct AsrSegment {
    double      start = 0.0, end = 0.0;
    std::string text;          // the segment's own text: INTACT UTF-8, unlike the raw words
    double      avgLogprob  = 0.0;
    double      noSpeech    = -1.0;   // -1 = absent
    double      temperature = 0.0;    // the fallback rung this segment was decoded at
    size_t      w0 = 0, w1 = 0;       // its words: AsrResult::words[w0, w1)
};

struct AsrResult {
    std::string text;         // the transcript, in the SOURCE language
    std::string language;     // whisper's own verdict, as a code ("en", "zh", ...)
    std::string languageRaw;  // exactly what the server said ("english", "chinese")
    bool        chinese = false;   // the decision the router actually uses
    uint32_t    ms = 0;
    // The timestamped words, flattened across whisper's own segments. EMPTY is a legal
    // answer (a server that does not emit them, or one whose shape we could not read),
    // and every consumer must degrade to the plain text when it is -- which is what makes
    // the commit engine an addition to this pipeline rather than a new requirement on it.
    //
    // /!\ REPAIRED, NOT RAW (wave WB). whisper-server writes each word as its TOKEN's text, and
    // a CJK character the tokenizer split across two tokens comes out as two words holding
    // U+FFFD each ("U+68C0" -> "\xef\xbf\xbd", "\xef\xbf\xbd": 4 of the 6 segments of bench_zh,
    // 10/10) -- while the segment's own text is intact. parseAsrResponse rebuilds every such
    // run from the segment text (one word, the real character, the run's timing), so a
    // sentence joined from these words -- W3's commit tail, a file cue -- never shows "U+FFFD,FFFD".
    std::vector<AsrWord> words;
    // Wave WB: whisper's segments (each owning a range of `words`) and the detection's own
    // confidence. Both may be empty / -1 on a server that does not emit them.
    std::vector<AsrSegment> segments;
    double      langProb = -1.0;
    // WAVE WF: whisper's probability for EVERY language it knows (verbose_json's
    // "language_probabilities", codes as whisper writes them: "en", "fr", "zh"...), as sent;
    // empty on a server that does not emit them. The FILE job reads the pair's two out of it.
    std::vector<std::pair<std::string, double>> langProbs;
    // The initial prompt asrTranscribe ACTUALLY PUT IN THE REQUEST ("" = none). Diagnostics
    // only -- and the selftest's proof that a final is prompted: until the WA review found it,
    // finaliseAudio built a prompt and then called asrTranscribe without it, so every final was
    // decoded unprompted while the code read as if it were not.
    std::string promptSent;
};

// The DRAFT decode's encoder context, in whisper frames of 20 ms. This is the ONE knob
// that moves the needle on a short clip, and the reason is structural: whisper zero-pads
// every input to a 30 s window, so a 1.5 s partial costs almost exactly what a 16 s
// segment costs. audio_ctx truncates that window.
//
// MEASURED on this build (whisper-server, ggml-small, Vulkan, RX 7700 XT), 3 s of audio:
//   default (1500) 660 ms | 1000 -> 496 | 768 -> 413 | 512 -> 327 | 256 -> 246
//   beam_size=1 + best_of=1 ALONE: 661 ms. Within noise -- the decoder is not the cost.
// 768 frames = 15.36 s of context, comfortably more than the 12 s a partial can ever
// carry (kPartialMaxMs), so NO AUDIO IS EVER TRUNCATED by it -- only the padding. Proved
// rather than assumed: at the 12 s cap the transcript is byte-identical to the default's.
inline constexpr int kDraftAudioCtx = 768;

// What to spend on ONE transcription, and what to tell the model before it starts.
// All three ride as ordinary form fields, and this server IGNORES fields it does not know
// -- verified by sending it a nonsense one and getting the same transcript back, which is
// what makes them safe to send to an older or newer whisper-server than the benched one.
struct AsrOptions {
    // A LIVE LINE: spend less. Greedy decoding (beam_size=1, best_of=1) and -- the one
    // that matters -- temperature_inc=0, which switches OFF whisper's fallback loop. That
    // loop re-decodes at rising temperatures when a decode looks bad, and on a draft it is
    // pure harm: it costs three to four times the wall clock to polish a guess that the
    // final for the same utterance is about to replace anyway (17/09 bench).
    bool        draft = false;
    // "auto", or an ISO code. The pipeline sends "auto" EVEN UNDER A LOCK (see the note in
    // processSegment): a lock names the target, and the passthrough of a line already in
    // that target needs the detection to stay alive. The code path exists for the probe
    // (a fixed language keeps its timing comparable) and for a future single-language mode.
    std::string language = "auto";
    // whisper's INITIAL PROMPT: text presented as "what was said just before". Two jobs
    // here -- domain vocabulary, and PUNCTUATION: a punctuated Chinese prompt is what makes
    // whisper emit U+3002 at all (without one the 17/09 bench produced no Chinese punctuation
    // whatsoever, which W3's sentence splitting would have had nothing to work with).
    // /!\ It also invites the model to ECHO the prompt on silence, which is why
    // looksLikeHallucination below takes it: see the note there.
    std::string prompt;
    // ---- wave WB: what the OFFLINE file job spends, and nothing else does ----------------
    // The DEFAULTS ARE THE LIVE REQUEST, byte for byte: audio_ctx is always sent at
    // kDraftAudioCtx (finals too, since the fluidity pass) and beam_size/best_of only on a
    // draft. A file window is up to 30 s long, so 768 frames (15.36 s) would TRUNCATE it:
    //   audioCtx = 0  -> the field is OMITTED = the server's full 1500-frame window;
    //   beamSize/bestOf > 0 -> sent (the file job: 5/5); 0 -> omitted (server default).
    // A draft still forces 1/1, whatever these say.
    int         audioCtx = kDraftAudioCtx;
    int         beamSize = 0;
    int         bestOf   = 0;
};

// POST the WAV to whisper-server with response_format=verbose_json -- the ONE format that
// returns the detected language alongside the text.
bool asrTranscribe(uint16_t port, const std::string& wav, AsrResult& out, DWORD timeoutMs,
                   std::string& err, const AsrOptions& opt = AsrOptions{});

// whisper's OWN verdict on a decode: the duration-weighted mean of its segments' avg_logprob,
// or 0 when the server emitted no segment (then nothing may be concluded from it).
// whisper.cpp itself calls a decode below -1.0 failed (its logprob_thold); on a QUIET audio
// that is the line between a soft sentence (-0.0 to -0.6 measured) and noise read as words
// ("U+04C0 beaches" -4.2, "....." -2.8; `small`, 10/10). The callers apply it in the quiet regime only.
double asrConfidence(const AsrResult& a);
inline constexpr double kQuietGateDbfs     = -32.0;   // looksLikeHallucination's quiet gate
inline constexpr double kLowConfidenceLogp = -1.0;    // whisper.cpp's own logprob_thold

// The response half of asrTranscribe, on its own (wave WB): verbose_json -> text, language,
// its confidence, segments and the REPAIRED words. Exposed so the selftest can hold the
// U+FFFD repair against a recorded response rather than against a live, non-deterministic
// decode. Returns false only when the document has no "text" at all. `ms` is left alone.
bool parseAsrResponse(const std::string& json, AsrResult& out);

// One prior utterance and its translation, as the MT is told about it.
struct MtPair {
    std::string src, dst;            // the two texts
    std::string srcLang, dstLang;    // ... and what they are, so only the SAME direction
};                                   //     is ever replayed as context

// POST to llama-server's OpenAI-compatible /v1/chat/completions at temperature 0.
// `style` = "qwen" | "hy" (mtStyleFor). `context` = prior pairs, OLDEST FIRST; the qwen
// driver replays them as user/assistant turns before the line, the hy driver ignores them.
// Temperature 0 everywhere and deliberately: a DRAFT must be deterministic, or the live
// line rewrites words the user has already read for no reason but sampling noise.
bool mtTranslate(uint16_t port, const std::string& text,
                 const std::string& srcLang, const std::string& dstLang,
                 const std::vector<MtPair>& context, const std::string& style,
                 std::string& out, uint32_t& ms, DWORD timeoutMs, std::string& err);

// ---------------------------------------------------------------------------
// Languages, in ONE place. Adding a language means adding a row and nothing else.
//
// /!\ NOT EVERY CODE IS TWO LETTERS ANY MORE (wave WA). "zh-tw" -- Traditional Chinese as
// written in Taiwan -- is the one code in the table that is not ISO-639-1, because it is not
// another LANGUAGE: whisper hears the same Mandarin and calls it "chinese" either way, and what
// differs is the SCRIPT the transcript and the translation are written in. So no code anywhere
// may be read as "its first two letters": cut to two, "zh-tw" IS "zh", and a Taiwanese session
// translates into Simplified without one error being raised anywhere. Every door a code comes
// through has ONE normaliser and only that one: normalizeLangCode here, trLangCode in the
// tray (ui/bridge.cpp), trNormLang / normLang on the two UI surfaces -- the same vocabulary in
// all four.
// ---------------------------------------------------------------------------
// The name to put in an MT prompt, in English ("Simplified Chinese (Mandarin)" for zh --
// the phrasing the bench validated, which is what keeps Qwen off traditional characters;
// "Traditional Chinese as used in Taiwan (...)" for zh-tw, the 10/10 bench's, which asks for
// Taiwan's everyday vocabulary as well as its characters). An unknown code comes back as
// itself: a wrong-but-honest prompt beats a silent default.
// /!\ RAW UTF-8 when it is not ASCII (zh-tw's carries U+7E41,9AD4,4E2D,6587 / U+53F0,7063,7528,8A9E) -- valid as-is inside a
// JSON string, and the one encoding that survives BOTH ways a caller may treat it: appended raw
// into a pre-escaped body, or passed through jsonEscapeC (which leaves bytes >= 0x80 alone).
std::string langPromptName(const std::string& code);
// The language's name IN CHINESE, for HY-MT's Chinese-template instruction.
// /!\ Returned as JSON \uXXXX ESCAPE TEXT, not as UTF-8 bytes. Its only consumer is an MT
// request body, which this file assembles as pre-escaped JSON and appends raw -- running
// it through jsonEscapeC would double the backslash and the model would read "U+82F1,8BED"
// as eleven ASCII characters. The ASR glossary below is the opposite case and says so.
std::string langChineseNameJson(const std::string& code);
// "EN" / " en " / "eng" -> "en"; "zh-TW" / "zh_hant" / "zh-hk" / "cht" -> "zh-tw"; anything
// that is not a plausible code -> "". EXACT spellings only, never a prefix (see above).
std::string normalizeLangCode(const std::string& s);
// The two codes whose speech whisper calls "chinese": zh (Simplified) and zh-tw (Traditional).
bool isHanLang(const std::string& code);

// The domain glossary line handed to whisper as an initial prompt for `lang`, as RAW UTF-8
// (it rides a multipart form field, where a JSON escape would arrive as six characters).
// "" for a language we have no line for -- an absent prompt is better than an English one.
// For zh-tw it is written in TRADITIONAL characters, and that is the first half of the
// Traditional ASR: whisper writes Mandarin in the script it was prompted in (10/10 bench on
// large-v3-turbo: no prompt, a Simplified or an English one -> Simplified; this one -> 100 %
// Traditional). The second half is toTraditional below, for the decodes this prompt never
// reaches or that do not obey it.
std::string asrGlossaryFor(const std::string& lang);
// The qwen driver's SYSTEM prompt for one direction: the interpreter role, the target's
// prompt name, and the audio glossary when the direction has one (en<->zh, en<->zh-tw).
// Exposed because the selftest asserts on it -- which glossary a direction gets, and that a
// target name carrying CJK arrives in the body unescaped and undoubled.
std::string qwenSystemPrompt(const std::string& srcLang, const std::string& dstLang);
// The TAIL of `s`, at most maxBytes, cut on a word boundary when the script has spaces and
// always on a codepoint boundary. The tail rather than the head: an initial prompt is
// "what was said just before", so the most recent words are the ones worth spending on.
std::string promptTail(const std::string& s, size_t maxBytes);

// ---------------------------------------------------------------------------
// Text helpers (exposed because the selftest asserts on them).
// ---------------------------------------------------------------------------
double  cjkRatio(const std::string& utf8);      // share of CJK ideographs among "letters"
bool    hasLatinLetters(const std::string& utf8);
// Kana (U+3040-30FF, U+31F0-31FF, U+FF66-FF9F) or Hangul (U+1100-11FF, U+3130-318F, U+AC00-D7AF).
// cjkRatio counts both as "CJK" -- they are certainly not Latin -- and that is exactly why it
// cannot be the Chinese test on its own: a Japanese line is 30-70 % kanji.
bool    hasKanaOrHangul(const std::string& utf8);
// THE CHINESE TEST, in one place (wave WB, after the WA review). Whisper's label is the opinion
// and the script the fact -- but the script fact must be about CHINESE: a line holding any kana
// or Hangul is Japanese or Korean, whatever its share of Han characters, and is NOT Chinese.
// Before this, "U+5B66,6821,306E,4F1A,8B70" was 80 % CJK, went down the Han path, was called zh-tw in an
// en<->zh-tw session, and the Traditional net REWROTE the speaker's Japanese (U+5B78,6821,306E,6703,8B70).
// `langCode` = whisper's label as a code ("zh", "yue", "ja", ...).
bool    chineseByScript(const std::string& text, const std::string& langCode);
// WHICH CHINESE SCRIPT a transcript is written in, by vote: every character of `utf8` found in
// the discriminating table counts once for its side. A Han character that is the same in both
// scripts (most of them: U+97F3, U+91CF, U+597D...) votes for neither, so a line can legitimately come out
// 0/0 -- the caller decides what a tie means (sourceLangOf: the pair's A half).
void    hanScriptVotes(const std::string& utf8, int& traditional, int& simplified);
// The table those votes are cast from: pairs of {Simplified, Traditional} codepoints, each one
// a character used in its own script only. Exposed so the selftest can hold it against the
// system's own converter (LCMapStringEx) rather than against this file's opinion of itself.
struct HanPair { uint32_t simp, trad; };
const HanPair* hanScriptPairs(size_t& count);
// Simplified -> Traditional by the SYSTEM's converters: LCMapStringEx (LCMAP_TRADITIONAL_CHINESE,
// character by character -- the same oracle the selftest holds the vote table against) DETECTS
// whether anything is Simplified at all, and Windows' word-level ELS transliteration converts it
// (the character map alone left 33 of 58 everyday words wrong: U+4EC0,4E48, U+540E,6765, U+5934,53D1 -> U+982D,767C...; see
// the .cpp). The character map is the fallback when ELS is unavailable.
// THE SAFETY NET UNDER THE PROMPT, and measured to be needed rather than assumed (10/10): the
// zh-tw prompt makes large-v3-turbo write 100 % Traditional, but in auto mode the first
// sentence of a Chinese turn is prompted in the PREVIOUS line's language, and under an English
// prompt turbo writes Simplified; `small` -- the balanced preset -- let a whole sentence through
// in Simplified under the Traditional prompt itself (selftest [6]); and Qwen3-4B lets the odd
// Simplified character through (U+89C9 in a Traditional line). So every text a zh-tw line DISPLAYS
// goes through this. Characters already
// Traditional, and everything that is not Chinese, come back untouched (the converter leaves
// what it does not map, and it maps the ambiguous U+540E/U+91CC to themselves); `utf8` is returned
// as-is when the converter is unavailable. `changed` (optional) says whether it did anything,
// which is how the selftest tells the prompt's work from the net's.
std::string toTraditional(const std::string& utf8, bool* changed = nullptr);
// Traditional -> Simplified (WB): THE MIRROR, for every text a line labelled zh (Simplified
// Chinese) displays. Whisper writes Mandarin in the script it was prompted in, and a decode the
// zh prompt does not reach -- or a model -- can hand a zh session a Traditional line (measured:
// the unprompted finals of the WA review wrote bench_zh in Traditional). The same shape as
// toTraditional: the character map (LCMapStringEx zh-CN, LCMAP_SIMPLIFIED_CHINESE) DETECTS,
// Windows' word-level ELS (Hant -> Hans) CONVERTS, the map is the fallback. Measured 10/10 why
// not the map alone: it leaves U+5F8C alone (U+5F8C,4F86 -> U+5F8C,6765, half converted), writes U+4EC0,9EBC as U+4EC0,9EBD (U+9EBD is
// not the standard U+4E48) -- and turns CORRECT Simplified wrong: U+8457,540D / U+8457,4F5C / U+663E,8457 -> U+7740..., U+77AD,671B ->
// U+4E86,671B, because the Simplified standard KEEPS U+8457 and U+77AD in those words. So the detector ignores
// those two characters: a line whose only map-changed characters are U+8457/U+77AD is Simplified already
// and comes back untouched. Untouched too: Simplified text, Latin, anything with no Han;
// `changed` as toTraditional's. (Measured residue, accepted: ELS also writes U+663E,8457 -> U+663E,7740 inside a
// line that IS Traditional; the map alone is far worse.)
std::string toSimplified(const std::string& utf8, bool* changed = nullptr);
// THE SCRIPT NET OF A LABEL, in one place: toTraditional for zh-tw, toSimplified for zh, and the
// text exactly as written for every other language -- a zh-tw line is NEVER simplified, and a
// ja / ko / yue line is never rewritten at all.
std::string scriptNet(const std::string& utf8, const std::string& lang, bool* changed = nullptr);
// `prompt` = the initial prompt this transcript was produced WITH, when there was one.
// Whisper's answer to silence is to repeat what you told it, so the one filter that
// existed before -- a shortlist of training-set captions -- stops seeing the junk the
// moment we start prompting: the junk becomes OUR OWN SENTENCE handed back. Passing it in
// is what keeps a quiet segment filtered exactly as it was before the prompt existed.
//
// Wave WB widened the STRUCTURAL gate (the one that ignores the level, LIVE and file alike):
// a line WRAPPED in an annotation -- [..], (..), the full-width U+FF08..U+FF09 and U+3010..U+3011, *..*, or a
// music note at either end (U+266A U+266B U+266C U+2669) -- is never speech. Measured: turbo answers a pure
// music chord with " *music*", which used to sail through (only [..] and (..) were caught).
// Wrapped means the WHOLE line: "(laughs) I agree" and "I like *this* one" are still speech.
// Same gate, same reason: two or more PHONETIC symbols (IPA, modifier letters, U+1D00-1DBF) --
// what `small` writes when it decodes noise ("recognis U+1D09 U+02B0,02CB,1D17,1D17,1D33,1D17 U+02B0,1D3E,1D07 Cream", 10/10) -- and the
// other alphabets (Cyrillic, Greek, Arabic...) now count as letters, so a Russian line is speech.
bool    looksLikeHallucination(const std::string& text, double segPeakDbfs,
                               const std::string& prompt = std::string());
// The structural half of the above on its own: true when `text` has no letter or digit at all,
// or is wrapped in an annotation as described. Level-free, prompt-free; exposed for the
// selftest and for the file job's per-cue filter.
bool    isAnnotationOnly(const std::string& text);
std::string langNameToCode(const std::string& name);   // "english" -> "en"; "" if unknown

// (SUVTITLE SPLIT: AudioPB's `auto` preset PROBE -- ProbeVerdict, kProbeSlowMs, probeAutoPreset: one timed
// transcription on the LIVE final server, to settle `auto` on quality / balanced / light -- is not ported.
// It timed the live pair, which SuVtitle does not run; a FILE job is analysed ahead of the picture and
// chooses its own models.)

} // namespace pbt

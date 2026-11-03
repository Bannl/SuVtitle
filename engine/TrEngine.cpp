// =============================================================================
// SuVtitle - engine/TrEngine.cpp
//
// SUVTITLE SPLIT (2026-10-23): ported from AudioPB's PatchBus repo, translator/TrEngine.cpp at commit 641f1aa
// (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Changed: the config file (%APPDATA%\SuVtitle\engine.txt,
// its own header, the FILE-relevant tags + DEVICE), the payload layout (<exe>\engine\whisper, <exe>\engine\llama,
// <exe>\models; the dev fallback is the folder named by SUVTITLE_PAYLOAD, no built-in path), no diarization runtime, no `auto` probe, the HTTP
// client's identity strings. The rest -- children, WinHTTP, ASR/MT drivers, languages, script nets -- is 641f1aa's.
// SuVtitle 1.1.0 (2026-10-25), the LIVE path back (TrEngine.h): the live pair and the SPK* / PARTIAL* / COMMIT* tags in
// engine.txt, the live pair's seed, the diarization runtime in the payload, the `auto` probe -- from AudioPB 50f79d6.
// =============================================================================
#include "TrEngine.h"
#include "TrSpeaker.h"   // kSpkModelFile: one definition of the diarization model's name
#include "TrMpvAudio.h"  // 1.1.0 (N3): the child servers' Vulkan loader fallback (environmentWithVulkanFallback)
// (kAsrRate: TrDecimator.h, through TrEngine.h.)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <elscore.h>     // Extended Linguistic Services: the word-level Hans -> Hant converter
#include <elssrvc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "elscore.lib")

namespace pbt {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring exeDir() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut + 1);
}
static bool fileExists(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring parentOf(const std::wstring& p) {
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

// ---------------------------------------------------------------------------
// BOUNDED READS (fix round RV3-2, 23/10). std::getline reads a WHOLE line before it returns, so a byte cap counted
// after it is no cap: a huge newline-less file was read whole into memory (a bad_alloc, on the first start). This
// reads at most `cap` bytes (+1, to know there was more), shared (whoever writes the file elsewhere is never
// disturbed), and hands back the COMPLETE lines inside them -- a last line the cap cut off is dropped, never half
// parsed. `truncated` = the file is longer than `cap`. false = the file could not be opened.
// ---------------------------------------------------------------------------
static bool readLinesBounded(const std::wstring& path, size_t cap, std::vector<std::string>& lines, bool& truncated) {
    lines.clear();
    truncated = false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string buf(cap + 1, '\0');
    size_t got = 0;
    while (got < buf.size()) {
        DWORD n = 0;
        const DWORD want = (DWORD)std::min<size_t>(buf.size() - got, (size_t)1 << 20);
        if (!::ReadFile(h, &buf[got], want, &n, nullptr) || n == 0) break;
        got += n;
    }
    ::CloseHandle(h);
    truncated = got > cap;
    buf.resize(std::min(got, cap));
    size_t at = 0;
    while (at < buf.size()) {
        const size_t nl = buf.find('\n', at);
        if (nl == std::string::npos) {
            if (!truncated) lines.push_back(buf.substr(at));   // the file's own last line, with no final newline
            break;
        }
        lines.push_back(buf.substr(at, nl - at));
        at = nl + 1;
    }
    for (auto& l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    return true;
}
// An engine.txt is ~1 KB; past 1 MB it is not one. Only the complete lines of its first 1 MB are loaded (and a
// save() then rewrites the file from them).
static constexpr size_t kConfigCap = (size_t)1 << 20;

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------
// %APPDATA%\SuVtitle\engine.txt, read from the ENVIRONMENT (not the shell's known folder) on purpose: a
// test run points APPDATA at a scratch folder and the engine follows it. SUVTITLE: the folder is created
// by save() only -- in AudioPB path() created it, so a read-only selftest made the user's data folder.
std::wstring TrConfig::path() {
    wchar_t ad[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH)) return std::wstring();
    return std::wstring(ad) + L"\\SuVtitle\\engine.txt";
}

void TrConfig::load() {
    const std::wstring p = path();
    if (p.empty()) return;
    // BOUNDED (fix round RV3-2): at most kConfigCap bytes are read, whatever the file holds.
    std::vector<std::string> lines;
    bool truncated = false;
    if (!readLinesBounded(p, kConfigCap, lines, truncated)) return;
    extra.clear();
    liveInFile = false;
    // (SUVTITLE: '#' lines are the file's own header and notes -- written by save(), never read.
    // AudioPB read its header for the v1 -> v2 PARTIALSEC migration: the default its v1 helper had
    // PERSISTED, 1.2, dropped from a "# AudioPB Translator config v1" file. No SuVtitle file was ever
    // written by that helper, so the migration did not come back with the PARTIALSEC tag in 1.1.0.)
    for (const std::string& line : lines) {
        if (!line.empty() && line[0] == '#') continue;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string tag = line.substr(0, tab), val = line.substr(tab + 1);
        if      (tag == "PRESET")     preset = val;
        else if (tag == "MODE")       mode   = val;
        else if (tag == "LANGA")      langA  = val;
        else if (tag == "LANGB")      langB  = val;
        else if (tag == "MTMODEL")    mtModel = val;
        else if (tag == "MTSTYLE")    mtStyle = val;
        else if (tag == "ASRPROMPT")  asrPrompt = (val != "0");
        else if (tag == "MTCONTEXT")  mtContext = (int)std::strtol(val.c_str(), nullptr, 10);
        else if (tag == "WHISPERBIN") whisperBin = utf8ToWide(val);
        else if (tag == "LLAMABIN")   llamaBin   = utf8ToWide(val);
        else if (tag == "MODELS")     models     = utf8ToWide(val);
        else if (tag == "DEVICE")     device     = val;
        else if (tag == "LIVEMODE")   { liveMode  = val; liveInFile = true; }   // SuVtitle 1.1.0: the live pair
        else if (tag == "LIVELANGA")  { liveLangA = val; liveInFile = true; }
        else if (tag == "LIVELANGB")  { liveLangB = val; liveInFile = true; }
        else if (tag == "SPKON")        spkOn        = (val != "0");
        else if (tag == "SPKMODEL")     spkModel     = utf8ToWide(val);
        else if (tag == "SPKTHRESH")    spkThresh    = std::strtod(val.c_str(), nullptr);
        else if (tag == "SPKMAX")       spkMax       = (int)std::strtol(val.c_str(), nullptr, 10);
        else if (tag == "SPKMINSEC")    spkMinSec    = std::strtod(val.c_str(), nullptr);
        else if (tag == "SPKRECLUSTER") spkRecluster = (int)std::strtol(val.c_str(), nullptr, 10);
        else if (tag == "PARTIALMT")    partialMt    = (val != "0");
        else if (tag == "COMMIT")          commit          = (val != "0");
        else if (tag == "COMMITLOOKAHEAD") commitLookahead = std::strtod(val.c_str(), nullptr);
        else if (tag == "COMMITPAUSE")     commitPause     = std::strtod(val.c_str(), nullptr);
        else if (tag == "COMMITMAXSEC")    commitMaxSec    = std::strtod(val.c_str(), nullptr);
        else if (tag == "PARTIALSEC") {
            // Parsed with an END POINTER on purpose. strtod turns anything unparseable
            // into 0.0, and 0.0 is a MEANINGFUL value here (it is "off") -- so a typo
            // would silently switch the live line off and look like a missing feature.
            // Nothing parsed = the line is noise = keep the default.
            char* end = nullptr;
            const double v = std::strtod(val.c_str(), &end);
            if (end != val.c_str() && v == v) partialSec = v;
        }
        else extra.push_back(line);   // a line this engine does not own -- keep it
    }
    // DEVICE: anything but the two words is the default -- a typo must never take the GPU away
    // silently, nor give it back to a user who asked for the CPU in a way we could read.
    if (device != "auto" && device != "cpu") device = "auto";
    if (preset != "auto" && preset != "quality" && preset != "balanced" && preset != "light")
        preset = "balanced";
    // ---- en2zh/zh2en -> a2b/b2a (wave W2) -------------------------------------------
    // The mode stopped naming two languages and started naming a DIRECTION over the pair
    // below. Every file written before this wave says en2zh or zh2en, and the pair those
    // files imply is exactly the default pair -- so the migration is a rename, not a guess,
    // and a user who had locked a direction keeps it locked after the update.
    if      (mode == "en2zh") mode = "a2b";
    else if (mode == "zh2en") mode = "b2a";
    if (mode != "auto" && mode != "a2b" && mode != "b2a") mode = "auto";
    langA = normalizeLangCode(langA);
    langB = normalizeLangCode(langB);
    if (langA.empty()) langA = TrConfig{}.langA;
    if (langB.empty()) langB = TrConfig{}.langB;
    // A pair of one language is not a pair: every line would be a passthrough and the
    // feature would look dead rather than misconfigured. Back to the defaults, both halves.
    if (langA == langB) { langA = TrConfig{}.langA; langB = TrConfig{}.langB; }
    if (!mtStyle.empty() && mtStyle != "qwen" && mtStyle != "hy") mtStyle.clear();
    if (mtContext < 0) mtContext = 0;
    if (mtContext > 4) mtContext = 4;   // four pairs is already ~600 tokens of prefix
    // THE LIVE PAIR (1.1.0): the player's vocabulary and the player's fallbacks, rule for rule -- a direction or a
    // code that does not read is the default, and a pair of one language is not a pair.
    if      (liveMode == "en2zh") liveMode = "a2b";
    else if (liveMode == "zh2en") liveMode = "b2a";
    if (liveMode != "auto" && liveMode != "a2b" && liveMode != "b2a") liveMode = "auto";
    liveLangA = normalizeLangCode(liveLangA);
    liveLangB = normalizeLangCode(liveLangB);
    if (liveLangA.empty()) liveLangA = TrConfig{}.liveLangA;
    if (liveLangB.empty()) liveLangB = TrConfig{}.liveLangB;
    if (liveLangA == liveLangB) { liveLangA = TrConfig{}.liveLangA; liveLangB = TrConfig{}.liveLangB; }
    // Clamp the diarization knobs to the range the bench actually explored. A hand-edited
    // 0.99 would put every utterance in its own cluster and read as a broken feature; a
    // hand-edited 0.0 would put a whole meeting in one. Out of range = back to the default.
    if (!(spkThresh > 0.05 && spkThresh < 0.99)) spkThresh = 0.50;
    if (spkMax < 1 || spkMax > 32)               spkMax = 8;
    if (!(spkMinSec >= 0.0 && spkMinSec <= 10.0)) spkMinSec = 1.2;
    if (spkRecluster < 0 || spkRecluster > 100000) spkRecluster = 32;
    // The commit knobs, clamped to ranges that keep the engine an engine. Out of range =
    // back to the default, never to the edge: a lookahead of 0 would commit whisper's most
    // revisable word (the last one it heard) and the text on screen would start changing
    // AFTER being read, which is the one promise this wave makes.
    if (!(commitLookahead >= 0.2 && commitLookahead <= 3.0)) commitLookahead = 0.7;
    if (!(commitPause     >= 0.1 && commitPause     <= 2.0)) commitPause     = 0.35;
    // Capped by the draft window itself (kPartialMaxMs): a pause rule that arms later than
    // the longest snapshot we can ever take would never arm at all.
    if (!(commitMaxSec    >= 1.0 && commitMaxSec    <= 10.0)) commitMaxSec    = 4.0;
}

void TrConfig::save() const {
    const std::wstring p = path();
    if (p.empty()) return;
    {   // the folder, now and only now (see path())
        const size_t cut = p.find_last_of(L"\\/");
        if (cut != std::wstring::npos) ::CreateDirectoryW(p.substr(0, cut).c_str(), nullptr);
    }
    std::string body =
        // The engine's OWN header line (S11). v1 = the first SuVtitle file; a later version that
        // must migrate something reads it the way AudioPB read its "v1" (the PARTIALSEC fix).
        "# SuVtitle Engine config v1\n"
        "# Written by SuVtitleEngine.exe, which owns this file: SuVtitle.exe sends its choices\n"
        "# over the engine's pipe and never writes here. Lines this engine does not know are kept.\n"
        "# PRESET  quality | balanced | light | auto. The LIVE translation's PAIR of speech\n"
        "#   models {draft, final}, because a draft costs what a final costs on this hardware\n"
        "#   and must therefore run on a cheaper model:\n"
        "#     quality  = small -> large-v3-turbo      balanced = small -> small\n"
        "#     light    = base  -> base                auto     = measured once, at the live start\n"
        "#   (auto brings the servers up as quality, times ONE transcription and settles on\n"
        "#   light, balanced or quality; the result is NOT written here). The best pair\n"
        "#   installed at or below the chosen one runs. And, for a FILE, the model it is\n"
        "#   analysed on when large-v3-turbo is NOT installed: the best installed AT OR BELOW\n"
        "#   this preset -- balanced = small, light = base (quality and auto: the best).\n"
        "# MODE    auto (both ways) | a2b (LANGA into LANGB) | b2a -- the PLAYER's direction\n"
        "#   The old en2zh / zh2en are read and migrated to a2b / b2a.\n"
        "# LANGA / LANGB  the PLAYER's language PAIR, ISO-639-1 (en zh fr de es it pt ja ko ru nl ar),\n"
        "#   plus zh-tw: zh = Simplified Chinese, zh-tw = Traditional Chinese as written in\n"
        "#   Taiwan (zh-TW, zh-Hant, zh-HK and cht are read as zh-tw).\n"
        "#   Defaults en / zh. Two identical codes are refused (every line would pass\n"
        "#   through untranslated), and so is anything that is not a plausible code.\n"
        "# MTMODEL the translation model FILE. Left out: Qwen3-4B-Instruct-2507-Q5_K_M.gguf\n"
        "#   (the 1.5B qwen2.5 when the 4B is not installed). Set it (bare filename in the\n"
        "#   models folder, or a full path) -- which is how a candidate model gets A/B'd\n"
        "#   without a rebuild.\n"
        "# MTSTYLE how that model is PROMPTED: qwen (system prompt + audio glossary +\n"
        "#   context) | hy (Tencent HY-MT's bare instruction, no system prompt, no context\n"
        "#   -- it translates the context too). Left out = deduced from the filename.\n"
        "# ASRPROMPT 1 = tell whisper the domain glossary and the previous sentence before\n"
        "#   each decode. Free on turbo, ~20 ms on small, and the ONLY thing that makes it\n"
        "#   punctuate Chinese at all. 0 turns it off.\n"
        "# MTCONTEXT 0-4 = how many previous sentence pairs the translator is shown, so a\n"
        "#   pronoun in one sentence still refers to the noun in the last one. 2 by default\n"
        "#   (a file is always given at least 3: nothing is waiting on the next word).\n"
        "# WHISPERBIN / LLAMABIN / MODELS override the payload search; leave them out to\n"
        "# use <exe dir>\\engine\\whisper, <exe dir>\\engine\\llama, <exe dir>\\models, and\n"
        "# then (a build tree with nothing staged) the folder named by the environment\n"
        "# variable SUVTITLE_PAYLOAD: <it>\\whisper, <it>\\llama, <it>\\models.\n"
        "# DEVICE  auto = each model on the GPU (Vulkan) when it comes up there, else on the\n"
        "#   CPU | cpu = every model on the CPU, the GPU never opened (slower; for a GPU kept\n"
        "#   for something else). Read when the engine starts.\n"
        "# LIVEMODE / LIVELANGA / LIVELANGB  the LIVE window's direction and pair: the same\n"
        "#   vocabulary as MODE / LANGA / LANGB, which stay the player's (one pair per mode).\n"
        "# SPK* = speaker diarization (who is talking, by voice, inside ONE input).\n"
        "#   SPKON 0 turns it off (back to naming the speaker by the loudest input).\n"
        "#   SPKTHRESH 0.50 / SPKMAX 8 / SPKMINSEC 1.2 / SPKRECLUSTER 32 are BENCHED\n"
        "#   values: 100 % clustering purity on the 4-voice bench across 0.40-0.70.\n"
        "# PARTIAL* = the LIVE LINE, which is what makes the screen move DURING a sentence\n"
        "#   instead of after it. A subtitle cannot exist before its segment closes, and a\n"
        "#   segment closes 400 ms after the speaker stops (8 s in continuous speech).\n"
        "#   PARTIALSEC 0.6 = seconds between draft transcriptions of the open segment\n"
        "#     (clamped to 0.5-5.0; 0 turns the live line off entirely).\n"
        "#   PARTIALMT 1   = also draft-translate each one. 0 keeps the live line in the\n"
        "#     source language only, which costs half as much on a slow machine.\n"
        "# COMMIT* = FINALISE BY SENTENCE instead of by VAD segment. Two consecutive drafts\n"
        "#   of the open segment agree on a prefix; the last agreed word that ENDS a\n"
        "#   sentence becomes a finished line while the speaker is still talking, and the\n"
        "#   audio behind it is never transcribed again. Without it a sentence waits for\n"
        "#   the segment to close -- 8 s in continuous speech, i.e. 2-3 sentences late.\n"
        "#   COMMIT 1 = on. 0 = exactly the behaviour before this existed (one line per\n"
        "#     VAD segment), which is the setting for a machine that cannot afford one\n"
        "#     extra final transcription per sentence.\n"
        "#   COMMITLOOKAHEAD 0.7 = seconds of audio that must follow a word before it may\n"
        "#     close a sentence (whisper revises the last thing it heard; 0.2-3.0).\n"
        "#   COMMITPAUSE 0.35 / COMMITMAXSEC 4.0 = the fallback for a speaker who never\n"
        "#     punctuates: past COMMITMAXSEC of uncommitted speech, a gap of COMMITPAUSE\n"
        "#     between two agreed words is taken as the end of a sentence.\n";
    body += "PRESET\t" + preset + "\n";
    body += "MODE\t"   + mode   + "\n";
    body += "LANGA\t"  + langA  + "\n";
    body += "LANGB\t"  + langB  + "\n";
    // Written only when SET. An absent MTMODEL means "the default model", and persisting
    // today's default would freeze this file's owner on it the day the default moves --
    // which is exactly the bug AudioPB's v1 -> v2 PARTIALSEC migration had to undo.
    if (!mtModel.empty()) body += "MTMODEL\t" + mtModel + "\n";
    if (!mtStyle.empty()) body += "MTSTYLE\t" + mtStyle + "\n";
    body += "ASRPROMPT\t" + std::string(asrPrompt ? "1" : "0") + "\n";
    body += "MTCONTEXT\t" + std::to_string(mtContext) + "\n";
    if (!whisperBin.empty()) body += "WHISPERBIN\t" + wideToUtf8(whisperBin) + "\n";
    if (!llamaBin.empty())   body += "LLAMABIN\t"   + wideToUtf8(llamaBin)   + "\n";
    if (!models.empty())     body += "MODELS\t"     + wideToUtf8(models)     + "\n";
    body += "DEVICE\t" + device + "\n";
    body += "LIVEMODE\t"  + liveMode  + "\n";
    body += "LIVELANGA\t" + liveLangA + "\n";
    body += "LIVELANGB\t" + liveLangB + "\n";
    body += "SPKON\t" + std::string(spkOn ? "1" : "0") + "\n";
    if (!spkModel.empty())   body += "SPKMODEL\t"   + wideToUtf8(spkModel)   + "\n";
    {   // %g, not %f: "0.5" reads like a decision, "0.500000" reads like a leak.
        char b[64];
        std::snprintf(b, sizeof(b), "SPKTHRESH\t%g\nSPKMAX\t%d\nSPKMINSEC\t%g\nSPKRECLUSTER\t%d\n",
                      spkThresh, spkMax, spkMinSec, spkRecluster);
        body += b;
    }
    {
        char b[64];
        std::snprintf(b, sizeof(b), "PARTIALSEC\t%g\nPARTIALMT\t%d\n",
                      partialSec, partialMt ? 1 : 0);
        body += b;
    }
    {
        char b[96];
        std::snprintf(b, sizeof(b),
                      "COMMIT\t%d\nCOMMITLOOKAHEAD\t%g\nCOMMITPAUSE\t%g\nCOMMITMAXSEC\t%g\n",
                      commit ? 1 : 0, commitLookahead, commitPause, commitMaxSec);
        body += b;
    }
    for (const auto& e : extra) body += e + "\n";   // lines we do not own, back out untouched

    // AV-safe write, the house dance (PB.md / Vst3Catalog / publishRuntimeFacts): a rename
    // issued right after the write sits in Bitdefender's on-write scan window and fails
    // EVERY time on this machine. Retry ~5x20 ms, then fall back to writing in place --
    // a torn line is recoverable (the reader skips it), an orphaned .tmp is not.
    const std::wstring tmp = p + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    const BOOL ok = ::WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    ::CloseHandle(h);
    if (!ok || wr != (DWORD)body.size()) { ::DeleteFileW(tmp.c_str()); return; }
    bool landed = false;
    for (int t = 0; t < 5 && !landed; ++t) {
        if (::MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) landed = true;
        else ::Sleep(20);
    }
    if (!landed) {
        ::DeleteFileW(tmp.c_str());
        HANDLE hd = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hd == INVALID_HANDLE_VALUE) return;
        DWORD wr2 = 0;
        ::WriteFile(hd, body.data(), (DWORD)body.size(), &wr2, nullptr);
        ::CloseHandle(hd);
    }
}

// ---------------------------------------------------------------------------
// FIRST START: the pair AudioPB had (see TrEngine.h). Every AudioPB file is OPENED FOR READING ONLY and shared, so a
// running AudioPB writing it at the same instant is never disturbed (a torn read simply fails a key = no seed).
// ---------------------------------------------------------------------------
// A value echoed into the log came from another program's file: printable ASCII, 24 characters at most.
static std::string shownValue(const std::string& v) {
    std::string s;
    for (char c : v) {
        if (s.size() >= 24) { s += "..."; break; }
        s += (c >= 32 && c <= 126) ? c : '?';
    }
    return "\"" + s + "\"";
}
// The mode vocabulary, AudioPB's W2 migration included; "" = does not read.
static std::string readMode(const std::string& v) {
    if (v == "en2zh") return "a2b";
    if (v == "zh2en") return "b2a";
    return (v == "auto" || v == "a2b" || v == "b2a") ? v : std::string();
}

// What translator.txt says, judged ALL OR NOTHING: `ok` = present, under 64 KB, and every key it holds reads.
struct ApbConfig {
    bool        ok = false, sawMode = false, sawA = false, sawB = false, sawPreset = false;
    std::string mode, langA, langB, preset;    // canonical when ok
    std::string why;                           // why not ok / what it lacked
};
static ApbConfig readAudioPBConfig(const std::wstring& appData) {
    ApbConfig t;
    const std::wstring src = appData + L"\\AudioPB\\translator.txt";
    if (!fileExists(src)) { t.why = "no AudioPB translator.txt"; return t; }
    std::vector<std::string> lines;
    bool truncated = false;
    if (!readLinesBounded(src, (size_t)64 << 10, lines, truncated)) { t.why = "AudioPB's translator.txt could not be read"; return t; }
    // AudioPB's file is a few kB; anything larger is not it (and only its first 64 KB were ever read)
    if (truncated) { t.why = "AudioPB's translator.txt is not a config file (over 64 KB)"; return t; }
    std::string mode, a, b, preset;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;             // AudioPB's own reader skipped such a line too
        const std::string tag = line.substr(0, tab), val = line.substr(tab + 1);
        if      (tag == "MODE")   { mode = val;   t.sawMode = true; }
        else if (tag == "LANGA")  { a = val;      t.sawA = true; }
        else if (tag == "LANGB")  { b = val;      t.sawB = true; }
        else if (tag == "PRESET") { preset = val; t.sawPreset = true; }
    }
    if (!t.sawMode && !t.sawA && !t.sawB && !t.sawPreset) {
        t.why = "AudioPB's translator.txt names no pair, direction or preset";
        return t;
    }
    // TrConfig::load's own vocabulary -- but a value that does not read REFUSES the source instead of falling back
    // to a default: the defaults are what the user gets either way, and a half-taken pair is one he never chose.
    if (t.sawMode && (t.mode = readMode(mode)).empty()) { t.why = "MODE " + shownValue(mode) + " does not read"; return t; }
    if (t.sawA && (t.langA = normalizeLangCode(a)).empty()) { t.why = "LANGA " + shownValue(a) + " does not read"; return t; }
    if (t.sawB && (t.langB = normalizeLangCode(b)).empty()) { t.why = "LANGB " + shownValue(b) + " does not read"; return t; }
    {
        const std::string ea = t.sawA ? t.langA : TrConfig{}.langA, eb = t.sawB ? t.langB : TrConfig{}.langB;
        if ((t.sawA || t.sawB) && ea == eb) { t.why = "the pair is one language, " + ea; return t; }
    }
    if (t.sawPreset) {
        if (preset != "auto" && preset != "quality" && preset != "balanced" && preset != "light") {
            t.why = "PRESET " + shownValue(preset) + " does not read";
            return t;
        }
        t.preset = preset;
    }
    t.ok = true;
    if (!(t.sawA && t.sawB)) t.why = "AudioPB's translator.txt holds no pair";
    return t;
}

// The NEWEST of AudioPB's analyses (fix round RV2-3): %LOCALAPPDATA%\AudioPB\Translator files\*.cues, by last-write
// time; its head only (16 KB: the header and the JOB line come first -- app\TrFileStore.h packRecord: the header,
// SRC, then JOB\t<langA>\t<langB>\t<mode>\t<quality>\t<asrModel>\t<mtModel>\t<hasVideo>\t<totalMs>). The NEWEST is
// the one asked, and the only one: an older analysis is an older choice. Its JOB fields went through AudioPB's esc(),
// and a code never holds a tab, a newline or a backslash, so the raw field is the value.
struct ApbJob { bool ok = false; std::string mode, langA, langB, name, why; };
static ApbJob readNewestAudioPBJob(const std::wstring& localAppData) {
    ApbJob j;
    const std::wstring dir = localAppData + L"\\AudioPB\\Translator files";
    WIN32_FIND_DATAW fd{};
    HANDLE ff = ::FindFirstFileW((dir + L"\\*.cues").c_str(), &fd);
    if (ff == INVALID_HANDLE_VALUE) { j.why = "no AudioPB analysis"; return j; }
    std::wstring best;
    FILETIME bestT{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || ::CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) { best = fd.cFileName; bestT = fd.ftLastWriteTime; }
    } while (::FindNextFileW(ff, &fd));
    ::FindClose(ff);
    if (best.empty()) { j.why = "no AudioPB analysis"; return j; }
    j.name = wideToUtf8(best);
    std::vector<std::string> lines;
    bool truncated = false;
    if (!readLinesBounded(dir + L"\\" + best, (size_t)16 << 10, lines, truncated)) {
        j.why = "the newest AudioPB analysis (" + shownValue(j.name) + ") could not be read";
        return j;
    }
    if (lines.empty() || (lines[0] != "# AudioPB Translator file cache v2" && lines[0] != "# AudioPB Translator file cache v1")) {
        j.why = "the newest AudioPB analysis (" + shownValue(j.name) + ") is not a cache record";
        return j;
    }
    for (const std::string& line : lines) {
        if (line.compare(0, 4, "JOB\t") != 0) continue;
        std::vector<std::string> t;
        for (size_t at = 0;;) {
            const size_t tab = line.find('\t', at);
            t.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        // the app's own reader (parseRecord) takes a JOB line of 9 fields or more; so does this one
        if (t.size() < 9) break;
        j.langA = normalizeLangCode(t[1]);
        j.langB = normalizeLangCode(t[2]);
        j.mode  = readMode(t[3]);
        if (j.langA.empty())  { j.why = "the newest AudioPB analysis: its language " + shownValue(t[1]) + " does not read"; return j; }
        if (j.langB.empty())  { j.why = "the newest AudioPB analysis: its language " + shownValue(t[2]) + " does not read"; return j; }
        if (j.mode.empty())   { j.why = "the newest AudioPB analysis: its direction " + shownValue(t[3]) + " does not read"; return j; }
        if (j.langA == j.langB) { j.why = "the newest AudioPB analysis: its pair is one language, " + j.langA; return j; }
        j.ok = true;
        return j;
    }
    j.why = "the newest AudioPB analysis (" + shownValue(j.name) + ") has no readable JOB line in its first 16 KB";
    return j;
}

bool seedFromAudioPB(TrConfig& cfg, std::string& why) {
    wchar_t ad[MAX_PATH] = {}, la[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { why = "no seed: APPDATA is not set"; return false; }
    const DWORD m = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    const ApbConfig t = readAudioPBConfig(ad);
    // 1. translator.txt's PAIR wins whenever it has one that reads (with its direction and preset, all or nothing)
    if (t.ok && t.sawA && t.sawB) {
        if (t.sawMode)   cfg.mode   = t.mode;
        if (t.sawPreset) cfg.preset = t.preset;
        cfg.langA = t.langA;
        cfg.langB = t.langB;
        why = "seeded from AudioPB's translator.txt (read only): MODE " + cfg.mode + ", LANGA " + cfg.langA
            + ", LANGB " + cfg.langB + ", PRESET " + cfg.preset;
        return true;
    }
    // 2. no pair there: the pair (and its direction) of AudioPB's NEWEST analysis -- the one its cache records are
    //    filed under; translator.txt's preset still comes along when that file reads
    ApbJob j;
    if (m == 0 || m >= MAX_PATH) j.why = "LOCALAPPDATA is not set";
    else                         j = readNewestAudioPBJob(la);
    if (j.ok) {
        cfg.mode  = j.mode;
        cfg.langA = j.langA;
        cfg.langB = j.langB;
        if (t.ok && t.sawPreset) cfg.preset = t.preset;
        why = "seeded from the newest AudioPB analysis: " + cfg.langA + "/" + cfg.langB + ", MODE " + cfg.mode
            + " (" + j.name + ", read only; " + t.why + ")"
            + ((t.ok && t.sawPreset) ? "; PRESET " + cfg.preset + " from AudioPB's translator.txt" : std::string());
        return true;
    }
    // 3. nothing to give a pair: translator.txt's direction / preset alone, when it reads
    if (t.ok && (t.sawMode || t.sawPreset)) {
        if (t.sawMode)   cfg.mode   = t.mode;
        if (t.sawPreset) cfg.preset = t.preset;
        why = "seeded from AudioPB's translator.txt (read only): MODE " + cfg.mode + ", PRESET " + cfg.preset
            + " -- no pair there, and " + j.why + ": the pair stays " + cfg.langA + "/" + cfg.langB;
        return true;
    }
    why = "no seed: " + t.why + "; " + j.why + " (the defaults)";
    return false;
}

// THE LIVE PAIR's seed (TrEngine.h): AudioPB's translator.txt pair -- the one its LIVE translator ran on -- all or
// nothing through readAudioPBConfig, else the player's pair. Read only, never fatal.
bool seedLivePair(TrConfig& cfg, std::string& why) {
    wchar_t ad[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    ApbConfig t;
    if (n == 0 || n >= MAX_PATH) t.why = "APPDATA is not set";
    else                         t = readAudioPBConfig(ad);
    if (t.ok && t.sawA && t.sawB) {
        cfg.liveLangA = t.langA;
        cfg.liveLangB = t.langB;
        cfg.liveMode  = t.sawMode ? t.mode : TrConfig{}.liveMode;
        why = "live pair seeded from AudioPB's translator.txt (read only): " + cfg.liveLangA + "/" + cfg.liveLangB
            + ", LIVEMODE " + cfg.liveMode;
        return true;
    }
    cfg.liveMode  = cfg.mode;
    cfg.liveLangA = cfg.langA;
    cfg.liveLangB = cfg.langB;
    why = "live pair = the player's, " + cfg.liveLangA + "/" + cfg.liveLangB + " LIVEMODE " + cfg.liveMode + " ("
        + t.why + ")";
    return false;
}

// ---------------------------------------------------------------------------
// Payload resolution
// ---------------------------------------------------------------------------
AsrModels asrModelsFor(const std::string& preset) {
    // /!\ `auto` RESOLVES TO QUALITY HERE, not to balanced. That is not a default, it is the
    // probe's starting position: auto must MEASURE the expensive model to be entitled to an
    // opinion about it, so it brings turbo up and then decides (probeAutoPreset, in AudioPB).
    // A caller that never probes -- SuVtitle's engine never does -- gets the best models.
    if (preset == "light")   return AsrModels{ "ggml-base.bin",  "ggml-base.bin" };
    if (preset == "balanced") return AsrModels{ "ggml-small.bin", "ggml-small.bin" };
    return AsrModels{ "ggml-small.bin", "ggml-large-v3-turbo.bin" };   // quality, and auto
}

// "ggml-small.bin" -> "small": the tag the user recognises, not the filename we load.
static std::string asrTag(const std::string& file) {
    std::string s = file;
    if (s.rfind("ggml-", 0) == 0) s = s.substr(5);
    const size_t dot = s.rfind(".bin");
    if (dot != std::string::npos && dot + 4 == s.size()) s.resize(dot);
    return s;
}

std::string asrModelsLabel(const AsrModels& m) {
    if (m.same()) return asrTag(m.draft);
    // "\xe2\x86\x92" is U+2192 RIGHTWARDS ARROW as UTF-8 BYTES, written out because this
    // file is compiled WITHOUT /utf-8: a literal arrow in the source would be read in the
    // system codepage and reach the tray as mojibake. Everything on this wire is UTF-8.
    return asrTag(m.draft) + "\xe2\x86\x92" + asrTag(m.final);
}

std::string mtModelFor(const std::string& preset) {
    // light is the preset for a machine that has no GPU to spend, so it keeps the small
    // model; everything else gets the 4B the 17/09 A/B settled on.
    return (preset == "light") ? kMtModelFileSmall : kMtModelFile;
}

std::string mtStyleFor(const std::string& modelFile) {
    // The filename decides, case-insensitively: the official GGUF is "HY-MT1.5-1.8B-Q8_0",
    // and a model that needs Tencent's bare-instruction shape needs it whatever it is named
    // in the config -- so MTSTYLE exists to override this, not to have to be set.
    std::string lower;
    for (char c : modelFile) lower += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    // A path, not a bare name? Only the leaf can carry the model's identity.
    const size_t cut = lower.find_last_of("\\/");
    if (cut != std::string::npos) lower = lower.substr(cut + 1);
    return (lower.rfind("hy-mt", 0) == 0) ? "hy" : "qwen";
}

// ---------------------------------------------------------------------------
// Languages: ONE table. A new language is a row here and nothing anywhere else.
//
// /!\ TWO ENCODINGS IN ONE TABLE, ON PURPOSE. `zh` (the Chinese name) is JSON \uXXXX ESCAPE
// TEXT because its only consumer is an MT request body, which this file assembles as
// pre-escaped JSON and appends RAW -- pushing it through jsonEscapeC would double the
// backslash and the model would receive eleven ASCII characters instead of two ideographs.
// The ASR glossary further down is raw UTF-8 for the mirror-image reason: it rides a
// multipart form field, where a JSON escape would reach whisper as six characters.
// ---------------------------------------------------------------------------
namespace {
struct LangRow {
    const char* code;
    const char* en;        // the English name
    const char* zhJson;    // the Chinese name, as JSON escapes (see above)
    const char* mtName;    // what goes in an MT prompt; nullptr = use `en`
};
const LangRow kLangs[] = {
    // mtName carries the phrasings the benches validated. "Chinese" alone lets Qwen drift
    // into traditional characters; "English" alone pulls it towards written register.
    { "en", "English",    "\\u82f1\\u8bed",                     "natural spoken English" },
    { "zh", "Chinese",    "\\u4e2d\\u6587",                     "Simplified Chinese (Mandarin)" },
    // zh-tw (wave WA). The prompt name is the one the 10/10 bench measured on Qwen3-4B, and
    // both halves of it ask for something: "Traditional Chinese" for the characters, "as used
    // in Taiwan (...U+53F0,7063,7528,8A9E)" for the everyday vocabulary. The studio terms are not left to
    // either -- the Traditional glossary below pins them (U+63A8,687F, not the mainland U+63A8,5B50).
    // /!\ THE ONE mtName THAT IS NOT ASCII, and it is RAW UTF-8 -- not JSON escape text like
    // zhJson -- on purpose: raw UTF-8 is valid inside a JSON string, so it is right whether a
    // caller appends it raw (both drivers do) or ever escapes it (jsonEscapeC passes bytes
    // >= 0x80 through untouched). Escape text would be right on the first path only.
    // "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87" = U+7E41,9AD4,4E2D,6587,
    // "\xe5\x8f\xb0\xe7\x81\xa3\xe7\x94\xa8\xe8\xaa\x9e" = U+53F0,7063,7528,8A9E.
    { "zh-tw", "Traditional Chinese", "\\u7e41\\u9ad4\\u4e2d\\u6587",
      "Traditional Chinese as used in Taiwan (\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87, "
      "\xe5\x8f\xb0\xe7\x81\xa3\xe7\x94\xa8\xe8\xaa\x9e)" },
    { "fr", "French",     "\\u6cd5\\u8bed",                     nullptr },
    { "de", "German",     "\\u5fb7\\u8bed",                     nullptr },
    { "es", "Spanish",    "\\u897f\\u73ed\\u7259\\u8bed",       nullptr },
    { "it", "Italian",    "\\u610f\\u5927\\u5229\\u8bed",       nullptr },
    { "pt", "Portuguese", "\\u8461\\u8404\\u7259\\u8bed",       nullptr },
    { "ja", "Japanese",   "\\u65e5\\u8bed",                     nullptr },
    { "ko", "Korean",     "\\u97e9\\u8bed",                     nullptr },
    { "ru", "Russian",    "\\u4fc4\\u8bed",                     nullptr },
    { "nl", "Dutch",      "\\u8377\\u5170\\u8bed",              nullptr },
    { "ar", "Arabic",     "\\u963f\\u62c9\\u4f2f\\u8bed",       nullptr },
};
const LangRow* langRow(const std::string& code) {
    for (const auto& r : kLangs) if (code == r.code) return &r;
    return nullptr;
}
} // namespace

std::string langPromptName(const std::string& code) {
    const LangRow* r = langRow(code);
    if (!r) return code;                       // unknown: name it by its code and be honest
    return r->mtName ? r->mtName : r->en;
}

std::string langChineseNameJson(const std::string& code) {
    const LangRow* r = langRow(code);
    return r ? r->zhJson : code;               // a code is ASCII, so it is JSON-safe as-is
}

std::string normalizeLangCode(const std::string& s) {
    std::string c;
    for (char ch : s) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        c += (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    }
    // Every spelling that is NOT already the canonical code, matched EXACTLY. Never by prefix
    // and never by length: the old "two letters or nothing" rule is precisely the one that
    // cannot tell zh-tw from zh, and a prefix rule ("zh-*" -> zh) would do the same silently.
    // /!\ TWIN of ui/bridge.cpp's trLangCode (the tray's door) -- keep the two rows-for-rows.
    struct Alias { const char* in; const char* out; };
    static const Alias kAlias[] = {
        // 639-2/3 codes the tray or a hand-edited translator.txt might carry
        { "eng", "en" }, { "zho", "zh" }, { "chi", "zh" }, { "cmn", "zh" }, { "yue", "yue" },
        // Traditional Chinese (wave WA): the canonical code, then the BCP-47-ish spellings a
        // person or a system locale would write -- Hong Kong and Macau included, both of
        // which write Traditional characters.
        { "zh-tw", "zh-tw" }, { "zh_tw", "zh-tw" }, { "zh-hant", "zh-tw" }, { "zh_hant", "zh-tw" },
        { "zh-hk", "zh-tw" }, { "zh-mo", "zh-tw" }, { "cht", "zh-tw" },
    };
    for (const auto& a : kAlias) if (c == a.in) return a.out;
    if (c.size() != 2) return std::string();
    for (char ch : c) if (ch < 'a' || ch > 'z') return std::string();
    return c;
}

bool isHanLang(const std::string& code) { return code == "zh" || code == "zh-tw"; }

// The speaker model under the name its publisher gave it (Hugging Face, csukuangfj/speaker-embedding-models): the
// same bytes as kSpkModelFile (TrSpeaker.h), which is the name AudioPB's bundle shipped it under (measured, LIVE study 6.1).
static const char* const kSpkModelFileHf = "3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx";

TrPaths resolvePaths(const TrConfig& cfg) {
    // The dev payload: SUVTITLE_PAYLOAD, read here and nowhere else (resolvePathsAt takes it as an argument so the
    // checks can build every layout under %TEMP%). Unset or empty = none.
    wchar_t env[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"SUVTITLE_PAYLOAD", env, (DWORD)(sizeof(env) / sizeof(env[0])));
    const std::wstring payload = (n > 0 && n < sizeof(env) / sizeof(env[0])) ? std::wstring(env, n) : std::wstring();
    return resolvePathsAt(cfg, exeDir(), payload);
}

TrPaths resolvePathsAt(const TrConfig& cfg, const std::wstring& root, const std::wstring& payloadRoot) {
    TrPaths p;
    // SUVTITLE LAYOUT (2026-10-23; the installer follows it): SuVtitle has a folder of its own, so the
    // payload sits at its root rather than under AudioPB's "Translator\bin\..." --
    //   <exe dir>\engine\whisper\whisper-server.exe   <exe dir>\engine\llama\llama-server.exe
    //   <exe dir>\models  (the ggml-*.bin and *.gguf files)
    const std::wstring bw = root + L"engine\\whisper", bl = root + L"engine\\llama", bm = root + L"models";
    // THE BUNDLE OR THE DEV PAYLOAD, NEVER A MIX (fix round RV3-3; TrEngine.h): any of the three folders next to the
    // exe makes the bundle the ONLY source. AudioPB fell back part by part, so a part-staged install on the dev
    // machine silently ran with the dev models under shipped binaries.
    const bool bundle = dirExists(bw) || dirExists(bl) || dirExists(bm);

    // The dev payload (SUVTITLE_PAYLOAD): <payload>\whisper, <payload>\llama, <payload>\models. Used only when nothing is
    // staged next to the exe, and with the same rule: any of its three folders makes it the ONLY source.
    std::wstring dev = payloadRoot;
    while (!dev.empty() && (dev.back() == L'\\' || dev.back() == L'/')) dev.pop_back();
    const std::wstring dw = dev + L"\\whisper", dl = dev + L"\\llama", dm = dev + L"\\models";
    const bool devSet = !bundle && !dev.empty();
    const bool devThere = devSet && (dirExists(dw) || dirExists(dl) || dirExists(dm));
    p.devPayload = devThere;
    const bool staged = bundle || devThere;          // some payload folder exists: it is the only source

    auto pickExe = [&](const std::wstring& override_, const std::wstring& bundledDir, const std::wstring& devDir,
                       const wchar_t* leaf) -> std::wstring {
        if (!override_.empty()) {                          // engine.txt WHISPERBIN / LLAMABIN: explicit, first
            const std::wstring c = override_ + L"\\" + leaf;
            if (fileExists(c)) return c;
            if (fileExists(override_)) return override_;   // they pointed at the exe itself
        }
        const std::wstring c = (bundle ? bundledDir : devDir) + L"\\" + leaf;
        return staged && fileExists(c) ? c : std::wstring();
    };

    p.whisperExe = pickExe(cfg.whisperBin, bw, dw, L"whisper-server.exe");
    p.llamaExe   = pickExe(cfg.llamaBin,   bl, dl, L"llama-server.exe");

    if (!cfg.models.empty() && dirExists(cfg.models))          p.modelsDir = cfg.models;
    else if (bundle) { if (dirExists(bm))                      p.modelsDir = bm; }
    else if (devThere) { if (dirExists(dm))                    p.modelsDir = dm; }

    // ---- diarization: the sherpa runtime + the speaker model (SuVtitle 1.1.0, AudioPB 50f79d6's resolution) ----
    // From the SAME source as the three parts above -- <exe>\engine\sherpa of the bundle, else <payload>\sherpa of the
    // dev payload, never one part from each -- and resolved SEPARATELY: both are optional (TrPaths), so a source
    // without them still runs, its lines named by input rather than by voice. The runtime is identified by its DLL
    // rather than by the folder existing, because half a copy is worse than none. No built-in path: AudioPB's
    // fallback for both (a folder of its author's machine) went the way of the others (the publication wave).
    {
        const std::wstring sd = bundle ? root + L"engine\\sherpa" : (devThere ? dev + L"\\sherpa" : std::wstring());
        if (!sd.empty() && fileExists(sd + L"\\sherpa-onnx-c-api.dll")) p.sherpaDir = sd;
        // SPKMODEL wins, path and all; else the models folder, under the name the bundle ships it with -- or under
        // Hugging Face's own name for the same file (3D-Speaker's checkpoint, renamed by AudioPB's bundle script),
        // so a model fetched by hand works as it is. A model that is not there keeps the bundle's name: the stage
        // then says "speaker model not found: <that path>", which is the sentence a user can act on.
        if (!cfg.spkModel.empty())      p.spkModel = cfg.spkModel;
        else if (!p.modelsDir.empty()) {
            p.spkModel = p.modelsDir + L"\\" + utf8ToWide(kSpkModelFile);
            const std::wstring hf = p.modelsDir + L"\\" + utf8ToWide(kSpkModelFileHf);
            if (!fileExists(p.spkModel) && fileExists(hf)) p.spkModel = hf;
        }
    }

    // The sentence a broken payload is reported with -- STATE's err at idle and the FDONE of any FILE -- NAMING the
    // missing part(s), so a part-staged install says which one. No path of the author's machine is ever in it: the
    // parts are named relative to the folder they were looked for in.
    if (!p.ok()) {
        std::string parts;
        auto add = [&](const char* s) { parts += (parts.empty() ? "" : ", "); parts += s; };
        const bool inDev = !bundle && devThere;
        if (p.whisperExe.empty()) add(inDev ? "whisper\\whisper-server.exe" : "engine\\whisper\\whisper-server.exe");
        if (p.llamaExe.empty())   add(inDev ? "llama\\llama-server.exe"     : "engine\\llama\\llama-server.exe");
        if (p.modelsDir.empty())  add("models");
        if (bundle)       p.missing = "the SuVtitle engine's payload is incomplete: " + parts + " missing next to SuVtitleEngine.exe";
        else if (inDev)   p.missing = "the SuVtitle engine's payload is incomplete: " + parts + " missing in the folder named by SUVTITLE_PAYLOAD";
        else if (devSet)  p.missing = "the SuVtitle engine's payload was not found (" + parts + " next to SuVtitleEngine.exe; "
                                      "the folder named by SUVTITLE_PAYLOAD holds none of whisper, llama, models)";
        else              p.missing = "the SuVtitle engine's payload was not found (" + parts + " next to SuVtitleEngine.exe, "
                                      "or set SUVTITLE_PAYLOAD to a folder holding whisper, llama, models)";
    }
    return p;
}

// ---------------------------------------------------------------------------
// A free loopback port -- by BINDING one. Any other method is a race with every
// other program on the machine.
// ---------------------------------------------------------------------------
static void wsaOnce() {
    static bool done = false;
    if (done) return;
    WSADATA w{};
    ::WSAStartup(MAKEWORD(2, 2), &w);
    done = true;
}

uint16_t findFreePort() {
    wsaOnce();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                                  // "you pick"
    uint16_t port = 0;
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
        sockaddr_in got{};
        int len = (int)sizeof(got);
        if (::getsockname(s, (sockaddr*)&got, &len) == 0) port = ::ntohs(got.sin_port);
    }
    ::closesocket(s);
    return port;
}

// ---------------------------------------------------------------------------
// WinHTTP, localhost, no proxy.
// ---------------------------------------------------------------------------
static bool httpDo(uint16_t port, const wchar_t* verb, const wchar_t* path,
                   const wchar_t* contentType, const void* body, size_t bodyLen,
                   std::string& out, DWORD timeoutMs, int* status) {
    out.clear();
    if (status) *status = 0;
    // NO_PROXY matters: a corporate proxy config would otherwise swallow a request to
    // our own loopback child and time out for reasons nobody would ever guess.
    HINTERNET ses = ::WinHttpOpen(L"SuVtitleEngine/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    ::WinHttpSetTimeouts(ses, 2000, 2000, (int)timeoutMs, (int)timeoutMs);
    HINTERNET con = ::WinHttpConnect(ses, L"127.0.0.1", port, 0);
    if (!con) { ::WinHttpCloseHandle(ses); return false; }
    HINTERNET req = ::WinHttpOpenRequest(con, verb, path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) { ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses); return false; }

    std::wstring headers;
    if (contentType) headers = std::wstring(L"Content-Type: ") + contentType;

    BOOL ok = ::WinHttpSendRequest(req,
                                   headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : (DWORD)-1L,
                                   const_cast<void*>(body), (DWORD)bodyLen, (DWORD)bodyLen, 0);
    if (ok) ok = ::WinHttpReceiveResponse(req, nullptr);
    if (ok && status) {
        DWORD code = 0, sz = sizeof(code);
        ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        *status = (int)code;
    }
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk((size_t)avail, '\0');
            DWORD got = 0;
            if (!::WinHttpReadData(req, &chunk[0], avail, &got)) break;
            out.append(chunk.data(), got);
            if (got == 0) break;
        }
    }
    ::WinHttpCloseHandle(req);
    ::WinHttpCloseHandle(con);
    ::WinHttpCloseHandle(ses);
    return ok == TRUE;
}

bool httpPost(uint16_t port, const wchar_t* path, const wchar_t* contentType,
              const void* body, size_t bodyLen, std::string& out, DWORD timeoutMs,
              int* status) {
    return httpDo(port, L"POST", path, contentType, body, bodyLen, out, timeoutMs, status);
}

bool httpGet(uint16_t port, const wchar_t* path, std::string& out, DWORD timeoutMs,
             int* status) {
    return httpDo(port, L"GET", path, nullptr, nullptr, 0, out, timeoutMs, status);
}

// ---------------------------------------------------------------------------
// ChildServer
// ---------------------------------------------------------------------------
bool ChildServer::alive() const {
    return proc_ && ::WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
}

void ChildServer::kill() {
    if (proc_) {
        ::TerminateProcess(proc_, 9);
        ::WaitForSingleObject(proc_, 3000);
        ::CloseHandle(proc_);
        proc_ = nullptr;
    }
    if (job_) { ::CloseHandle(job_); job_ = nullptr; }   // KILL_ON_JOB_CLOSE: the belt to our braces
    port_ = 0;
}

bool ChildServer::spawn(const std::wstring& exe, const std::wstring& workDir,
                        const std::wstring& args, uint16_t port, DWORD readyMs,
                        const std::function<bool()>& abort) {
    kill();
    port_ = port;

    // A job with KILL_ON_JOB_CLOSE is what guarantees no orphan servers: if this process
    // dies ANY way at all -- crash, kill, a debugger stop -- the handle closes and Windows
    // takes the children with it. Best-effort: a nested-job refusal is not fatal (the
    // helper also kills them explicitly on the way out).
    job_ = ::CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(L'\0');
    // SuVtitle 1.1.0 (N3, TrMpvAudio.h): the Vulkan builds of whisper.cpp / llama.cpp import vulkan-1.dll (whisper's HARD:
    // without a loader it does not even start, its CPU fallback included). The bundle's own loader folder
    // (<app>\mpv\vkfallback) is APPENDED to the child's PATH: PATH comes after System32 in the DLL search order, so a PC
    // with a loader keeps its own, and one without gets ours -- which, with no driver behind it, lists no device (the
    // servers then run on the CPU: measured with VK_LOADER_DRIVERS_DISABLE=*). No such folder: the environment as it is.
    std::vector<wchar_t> env = environmentWithVulkanFallback();
    // SUSPENDED so the child is inside the job BEFORE it can fork anything of its own.
    if (!::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                          env.empty() ? nullptr : env.data(), workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        if (job_) { ::CloseHandle(job_); job_ = nullptr; }
        port_ = 0;
        return false;
    }
    if (job_) ::AssignProcessToJobObject(job_, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    proc_ = pi.hProcess;

    // Readiness = /health answers. BOTH servers expose it (verified on this machine:
    // whisper-server 200, llama-server {"status":"ok"}), and both bind the port only
    // after the model is loaded -- so an answer means "ready", not "starting".
    const uint64_t deadline = ::GetTickCount64() + readyMs;
    while (::GetTickCount64() < deadline) {
        if (!alive()) break;                       // died on startup (bad model path, no VRAM...)
        // WB: a cancelled FILE job or a QUIT stops waiting HERE, not after a 3 GB load.
        if (abort && abort()) break;
        std::string body;
        int st = 0;
        if (httpGet(port_, L"/health", body, 1500, &st) && st == 200) return true;
        ::Sleep(200);
    }
    kill();
    return false;
}

// ---------------------------------------------------------------------------
// WAV building
// ---------------------------------------------------------------------------
static void put32(std::string& s, uint32_t v) {
    s += (char)(v & 0xff); s += (char)((v >> 8) & 0xff);
    s += (char)((v >> 16) & 0xff); s += (char)((v >> 24) & 0xff);
}
static void put16(std::string& s, uint16_t v) {
    s += (char)(v & 0xff); s += (char)((v >> 8) & 0xff);
}

std::string buildWav16k(const std::vector<float>& mono) {
    // Peak-normalise to -3 dBFS. The mono downmix of a user's 32x32 matrix can land
    // anywhere between -40 dBFS and clipping depending on how they patched it, and an
    // ASR fed either extreme transcribes noise. This is the one place that knows the
    // whole segment, so it is the one place that can normalise it honestly.
    float peak = 0.0f;
    for (float v : mono) { const float a = std::fabs(v); if (a > peak) peak = a; }
    const float target = 0.7079f;                    // -3 dBFS
    const float g = (peak > 1.0e-6f) ? (target / peak) : 1.0f;

    const uint32_t n     = (uint32_t)mono.size();
    const uint32_t bytes = n * 2u;
    std::string w;
    w.reserve(44 + bytes);
    w += "RIFF"; put32(w, 36u + bytes); w += "WAVE";
    w += "fmt "; put32(w, 16u);
    put16(w, 1);                        // PCM
    put16(w, 1);                        // mono
    put32(w, (uint32_t)kAsrRate);
    put32(w, (uint32_t)kAsrRate * 2u);   // byte rate
    put16(w, 2);                        // block align
    put16(w, 16);                       // bits
    w += "data"; put32(w, bytes);
    for (float v : mono) {
        float s = v * g;
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        const int16_t q = (int16_t)std::lround(s * 32767.0f);
        put16(w, (uint16_t)q);
    }
    return w;
}

// ---------------------------------------------------------------------------
// Tiny JSON reader: one string field by key. Handles \u escapes (surrogate pairs
// included) because a server is entitled to emit them even though ours do not.
// ---------------------------------------------------------------------------
static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static bool jsonString(const std::string& j, const char* key, std::string& out, size_t from = 0) {
    out.clear();
    const std::string pat = std::string("\"") + key + "\":";
    size_t k = j.find(pat, from);
    if (k == std::string::npos) return false;
    size_t i = k + pat.size();
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    if (i >= j.size() || j[i] != '"') return false;
    ++i;
    for (; i < j.size(); ++i) {
        const char c = j[i];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (++i >= j.size()) return false;
        switch (j[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/';  break;
            case 'u': {
                if (i + 4 >= j.size()) return false;
                uint32_t cp = (uint32_t)std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < j.size()
                    && j[i + 1] == '\\' && j[i + 2] == 'u') {
                    const uint32_t lo = (uint32_t)std::strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += j[i]; break;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Text inspection
// ---------------------------------------------------------------------------
static uint32_t nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp = c;
    int extra = 0;
    if      ((c & 0x80) == 0)    { extra = 0; cp = c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    return cp;
}

static bool isCjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF)      // CJK Unified Ideographs
        || (cp >= 0x3400 && cp <= 0x4DBF)      // Extension A
        || (cp >= 0xF900 && cp <= 0xFAFF)      // Compatibility Ideographs
        || (cp >= 0x3040 && cp <= 0x30FF)      // kana: not Chinese, but definitely not Latin
        || (cp >= 0xAC00 && cp <= 0xD7AF);     // Hangul, same reasoning
}
static bool isLatinLetter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
        || (cp >= 0x00C0 && cp <= 0x024F);     // accented Latin
}

double cjkRatio(const std::string& utf8) {
    size_t i = 0; size_t cjk = 0, letters = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodepoint(utf8, i);
        if (isCjk(cp))              { ++cjk; ++letters; }
        else if (isLatinLetter(cp)) { ++letters; }
        else if (cp >= 0x0370 && cp <= 0x1FFF) { ++letters; }   // Greek/Cyrillic/... still "letters"
    }
    return letters ? (double)cjk / (double)letters : 0.0;
}

bool hasLatinLetters(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) if (isLatinLetter(nextCodepoint(utf8, i))) return true;
    return false;
}

bool hasKanaOrHangul(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodepoint(utf8, i);
        // U+30FB (U+30FB) and U+30FC (U+30FC) are written in CHINESE too (U+6BD4,8D5B,30FB,7ED3,679C, U+5361,62C9OKU+30FB,30FC,30FC): not
        // evidence of Japanese (WB, after the review).
        if ((cp >= 0x3040 && cp <= 0x30FF && cp != 0x30FB && cp != 0x30FC)
            || (cp >= 0x31F0 && cp <= 0x31FF)                                    // kana
            || (cp >= 0xFF66 && cp <= 0xFF9F)                                    // half-width kana
            || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) // Hangul jamo
            || (cp >= 0xAC00 && cp <= 0xD7AF))                                   // Hangul syllables
            return true;
    }
    return false;
}

bool chineseByScript(const std::string& text, const std::string& langCode) {
    // Japanese and Korean FIRST: their Han characters would otherwise carry the ratio below.
    if (hasKanaOrHangul(text)) return false;
    const bool labelZh = (langCode == "zh" || langCode == "yue");
    return (cjkRatio(text) >= 0.20) || (labelZh && !hasLatinLetters(text));
}

// ---------------------------------------------------------------------------
// Simplified or Traditional? (wave WA)
//
// Asked in ONE situation only: the pair is zh <-> zh-tw, i.e. the session is a SCRIPT
// conversion, and whisper -- which calls both "chinese" -- cannot say which half was spoken.
// What was spoken is then decided by what was WRITTEN: a character that exists in one script
// only is a vote for that script.
//
// A TABLE OF PAIRS, not a full conversion map, because the question is a vote and not a
// conversion: ~140 of the commonest characters that differ (U+4EEC/U+5011, U+8FD9/U+9019, U+8BF4/U+8AAA...) put
// several votes into any spoken sentence, and a line too short to carry one ties -- which the
// caller resolves to the pair's A half, the documented default. Every row was checked against
// Windows' own converter (LCMapStringEx, both directions, and each side a fixed point of its
// own script) before it went in, and the selftest checks it again on every run: no row here
// is a character both scripts share, which is the one mistake that would make a vote lie.
// (U+540E/U+5F8C, U+91CC/U+88E1, U+53D1/U+9AEE... are absent ON PURPOSE: U+540E and U+91CC are also Traditional characters.)
// ---------------------------------------------------------------------------
namespace {
const HanPair kHanPairs[] = {
    { 0x4EEC, 0x5011 }, { 0x8FD9, 0x9019 }, { 0x4E2A, 0x500B }, { 0x6765, 0x4F86 }, { 0x65F6, 0x6642 }, { 0x8BF4, 0x8AAA },   // U+4EEC/U+5011 U+8FD9/U+9019 U+4E2A/U+500B U+6765/U+4F86 U+65F6/U+6642 U+8BF4/U+8AAA
    { 0x5BF9, 0x5C0D }, { 0x573A, 0x5834 }, { 0x542C, 0x807D }, { 0x58F0, 0x8072 }, { 0x68C0, 0x6AA2 }, { 0x7EC8, 0x7D42 },   // U+5BF9/U+5C0D U+573A/U+5834 U+542C/U+807D U+58F0/U+8072 U+68C0/U+6AA2 U+7EC8/U+7D42
    { 0x89C9, 0x89BA }, { 0x54CD, 0x97FF }, { 0x4E3A, 0x70BA }, { 0x4F1A, 0x6703 }, { 0x8FC7, 0x904E }, { 0x8FD8, 0x9084 },   // U+89C9/U+89BA U+54CD/U+97FF U+4E3A/U+70BA U+4F1A/U+6703 U+8FC7/U+904E U+8FD8/U+9084
    { 0x6CA1, 0x6C92 }, { 0x8BDD, 0x8A71 }, { 0x5417, 0x55CE }, { 0x70B9, 0x9EDE }, { 0x73B0, 0x73FE }, { 0x6837, 0x6A23 },   // U+6CA1/U+6C92 U+8BDD/U+8A71 U+5417/U+55CE U+70B9/U+9EDE U+73B0/U+73FE U+6837/U+6A23
    { 0x95EE, 0x554F }, { 0x9898, 0x984C }, { 0x5F00, 0x958B }, { 0x5173, 0x95DC }, { 0x957F, 0x9577 }, { 0x95E8, 0x9580 },   // U+95EE/U+554F U+9898/U+984C U+5F00/U+958B U+5173/U+95DC U+957F/U+9577 U+95E8/U+9580
    { 0x95F4, 0x9593 }, { 0x8BA9, 0x8B93 }, { 0x8BA4, 0x8A8D }, { 0x8BC6, 0x8B58 }, { 0x7ECF, 0x7D93 }, { 0x52A8, 0x52D5 },   // U+95F4/U+9593 U+8BA9/U+8B93 U+8BA4/U+8A8D U+8BC6/U+8B58 U+7ECF/U+7D93 U+52A8/U+52D5
    { 0x4E1C, 0x6771 }, { 0x8F66, 0x8ECA }, { 0x673A, 0x6A5F }, { 0x7535, 0x96FB }, { 0x8BED, 0x8A9E }, { 0x4E66, 0x66F8 },   // U+4E1C/U+6771 U+8F66/U+8ECA U+673A/U+6A5F U+7535/U+96FB U+8BED/U+8A9E U+4E66/U+66F8
    { 0x4E70, 0x8CB7 }, { 0x5356, 0x8CE3 }, { 0x5934, 0x982D }, { 0x5B9E, 0x5BE6 }, { 0x5B66, 0x5B78 }, { 0x5199, 0x5BEB },   // U+4E70/U+8CB7 U+5356/U+8CE3 U+5934/U+982D U+5B9E/U+5BE6 U+5B66/U+5B78 U+5199/U+5BEB
    { 0x89C1, 0x898B }, { 0x89C6, 0x8996 }, { 0x7EBF, 0x7DDA }, { 0x7ED9, 0x7D66 }, { 0x8BB2, 0x8B1B }, { 0x5E94, 0x61C9 },   // U+89C1/U+898B U+89C6/U+8996 U+7EBF/U+7DDA U+7ED9/U+7D66 U+8BB2/U+8B1B U+5E94/U+61C9
    { 0x8BE5, 0x8A72 }, { 0x8C01, 0x8AB0 }, { 0x8BF7, 0x8ACB }, { 0x8C22, 0x8B1D }, { 0x9519, 0x932F }, { 0x94B1, 0x9322 },   // U+8BE5/U+8A72 U+8C01/U+8AB0 U+8BF7/U+8ACB U+8C22/U+8B1D U+9519/U+932F U+94B1/U+9322
    { 0x53F7, 0x865F }, { 0x8FB9, 0x908A }, { 0x8FDB, 0x9032 }, { 0x9009, 0x9078 }, { 0x96BE, 0x96E3 }, { 0x6B22, 0x6B61 },   // U+53F7/U+865F U+8FB9/U+908A U+8FDB/U+9032 U+9009/U+9078 U+96BE/U+96E3 U+6B22/U+6B61
    { 0x6C14, 0x6C23 }, { 0x4E07, 0x842C }, { 0x4E0E, 0x8207 }, { 0x4E24, 0x5169 }, { 0x4ECE, 0x5F9E }, { 0x5F53, 0x7576 },   // U+6C14/U+6C23 U+4E07/U+842C U+4E0E/U+8207 U+4E24/U+5169 U+4ECE/U+5F9E U+5F53/U+7576
    { 0x8F68, 0x8ECC }, { 0x9759, 0x975C }, { 0x76D1, 0x76E3 }, { 0x73AF, 0x74B0 }, { 0x6D4B, 0x6E2C }, { 0x8BD5, 0x8A66 },   // U+8F68/U+8ECC U+9759/U+975C U+76D1/U+76E3 U+73AF/U+74B0 U+6D4B/U+6E2C U+8BD5/U+8A66
    { 0x8F93, 0x8F38 }, { 0x538B, 0x58D3 }, { 0x7F29, 0x7E2E }, { 0x8FDF, 0x9072 }, { 0x6302, 0x639B }, { 0x5F55, 0x9304 },   // U+8F93/U+8F38 U+538B/U+58D3 U+7F29/U+7E2E U+8FDF/U+9072 U+6302/U+639B U+5F55/U+9304
    { 0x8C03, 0x8ABF }, { 0x7EA7, 0x7D1A }, { 0x8282, 0x7BC0 }, { 0x9891, 0x983B }, { 0x7801, 0x78BC }, { 0x8BBE, 0x8A2D },   // U+8C03/U+8ABF U+7EA7/U+7D1A U+8282/U+7BC0 U+9891/U+983B U+7801/U+78BC U+8BBE/U+8A2D
    { 0x5907, 0x5099 }, { 0x952E, 0x9375 }, { 0x603B, 0x7E3D }, { 0x5BFC, 0x5C0E }, { 0x5904, 0x8655 }, { 0x5E26, 0x5E36 },   // U+5907/U+5099 U+952E/U+9375 U+603B/U+7E3D U+5BFC/U+5C0E U+5904/U+8655 U+5E26/U+5E36
    { 0x5BBD, 0x5BEC }, { 0x8F91, 0x8F2F }, { 0x7F16, 0x7DE8 }, { 0x8F6F, 0x8EDF }, { 0x521A, 0x525B }, { 0x5E2E, 0x5E6B },   // U+5BBD/U+5BEC U+8F91/U+8F2F U+7F16/U+7DE8 U+8F6F/U+8EDF U+521A/U+525B U+5E2E/U+5E6B
    { 0x9875, 0x9801 }, { 0x7F51, 0x7DB2 }, { 0x8054, 0x806F }, { 0x7EDF, 0x7D71 }, { 0x8BA1, 0x8A08 }, { 0x52A1, 0x52D9 },   // U+9875/U+9801 U+7F51/U+7DB2 U+8054/U+806F U+7EDF/U+7D71 U+8BA1/U+8A08 U+52A1/U+52D9
    { 0x4E1A, 0x696D }, { 0x5C06, 0x5C07 }, { 0x53D8, 0x8B8A }, { 0x5F20, 0x5F35 }, { 0x7ED3, 0x7D50 }, { 0x8BCD, 0x8A5E },   // U+4E1A/U+696D U+5C06/U+5C07 U+53D8/U+8B8A U+5F20/U+5F35 U+7ED3/U+7D50 U+8BCD/U+8A5E
    { 0x8BFB, 0x8B80 }, { 0x8BBA, 0x8AD6 }, { 0x8BC1, 0x8B49 }, { 0x8BB8, 0x8A31 }, { 0x8BB0, 0x8A18 }, { 0x89C2, 0x89C0 },   // U+8BFB/U+8B80 U+8BBA/U+8AD6 U+8BC1/U+8B49 U+8BB8/U+8A31 U+8BB0/U+8A18 U+89C2/U+89C0
    { 0x4F17, 0x773E }, { 0x533A, 0x5340 }, { 0x53CC, 0x96D9 }, { 0x65E0, 0x7121 }, { 0x7231, 0x611B }, { 0x4E50, 0x6A02 },   // U+4F17/U+773E U+533A/U+5340 U+53CC/U+96D9 U+65E0/U+7121 U+7231/U+611B U+4E50/U+6A02
    { 0x95FB, 0x805E }, { 0x961F, 0x968A }, { 0x9645, 0x969B }, { 0x968F, 0x96A8 }, { 0x987A, 0x9806 }, { 0x9886, 0x9818 },   // U+95FB/U+805E U+961F/U+968A U+9645/U+969B U+968F/U+96A8 U+987A/U+9806 U+9886/U+9818
    { 0x98CE, 0x98A8 }, { 0x98DE, 0x98DB }, { 0x9A6C, 0x99AC }, { 0x9C7C, 0x9B5A }, { 0x9EC4, 0x9EC3 }, { 0x5C14, 0x723E },   // U+98CE/U+98A8 U+98DE/U+98DB U+9A6C/U+99AC U+9C7C/U+9B5A U+9EC4/U+9EC3 U+5C14/U+723E
};
} // namespace

const HanPair* hanScriptPairs(size_t& count) {
    count = sizeof(kHanPairs) / sizeof(kHanPairs[0]);
    return kHanPairs;
}

void hanScriptVotes(const std::string& utf8, int& traditional, int& simplified) {
    traditional = simplified = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodepoint(utf8, i);
        if (cp < 0x4E00) continue;                       // not an ideograph: no vote, no scan
        for (const HanPair& p : kHanPairs) {
            if (cp == p.simp) { ++simplified;  break; }
            if (cp == p.trad) { ++traditional; break; }
        }
    }
}

// The PHRASE-AWARE converter: Windows' Extended Linguistic Services, "Microsoft Simplified
// Chinese to Traditional Chinese Transliteration" (elscore.dll, every Windows since 7).
// /!\ WHY NOT LCMapStringEx ALONE. LCMAP_TRADITIONAL_CHINESE maps one character at a time and
// cannot choose where Simplified merged several Traditional characters into one -- measured
// 10/10 on 58 everyday words it got 33 WRONG: U+4EC0,4E48/U+600E,4E48 stay Simplified (U+4E48 is never mapped),
// U+540E,6765/U+91CC,9762 stay U+540E/U+91CC, U+5934,53D1 -> U+982D,767C, U+5E72,51C0 -> U+5E72,51C8, U+4E00,53EA -> U+4E00,53EA, U+5173,7CFB -> U+95DC,7CFB, U+590D,6742 -> U+5FA9,96DC,
// U+51B2,7A81 -> U+6C96,7A81... ELS reads the WORD: 7 of 58 differed, and those were Taiwan usage rather than
// mistakes (U+77AD,89E3, U+5F59,5831, U+81FA,7063, U+8A08,7A0B,8ECA). A line a Taiwanese reader sees as half-converted is the
// failure this net exists to prevent, so the word-level service is the converter and the
// character map is only the detector below and the fallback when ELS is missing.
// The service handle is fetched ONCE and kept for the life of the process (it is a few bytes,
// and MappingGetServices is a registry walk we do not want on every subtitle).
static const MAPPING_SERVICE_INFO* elsHansToHant() {
    static PMAPPING_SERVICE_INFO svc = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        MAPPING_ENUM_OPTIONS eo{};
        eo.Size = sizeof(eo);
        GUID g = ELS_GUID_TRANSLITERATION_HANS_TO_HANT;
        eo.pGuid = &g;
        DWORD n = 0;
        if (FAILED(::MappingGetServices(&eo, &svc, &n)) || n == 0) svc = nullptr;
    });
    return svc;
}

// ... and the other direction (WB, toSimplified), fetched the same way, once.
static const MAPPING_SERVICE_INFO* elsHantToHans() {
    static PMAPPING_SERVICE_INFO svc = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        MAPPING_ENUM_OPTIONS eo{};
        eo.Size = sizeof(eo);
        GUID g = ELS_GUID_TRANSLITERATION_HANT_TO_HANS;
        eo.pGuid = &g;
        DWORD n = 0;
        if (FAILED(::MappingGetServices(&eo, &svc, &n)) || n == 0) svc = nullptr;
    });
    return svc;
}

static bool elsConvert(const MAPPING_SERVICE_INFO* svc, const std::wstring& in, std::wstring& out) {
    if (!svc) return false;
    // ONE CALLER AT A TIME on the shared service handle (WB, after the WA review): concurrent use
    // of a single MAPPING_SERVICE_INFO is not documented either way, and today only the
    // pipeline thread converts -- the lock is a few nanoseconds of insurance against the day a
    // second thread (an export, a proxy worker) does.
    static std::mutex elsMx;
    std::lock_guard<std::mutex> lk(elsMx);
    MAPPING_PROPERTY_BAG bag{};
    bag.Size = sizeof(bag);
    if (FAILED(::MappingRecognizeText(const_cast<PMAPPING_SERVICE_INFO>(svc), in.c_str(),
                                      (DWORD)in.size(), 0, nullptr, &bag)))
        return false;
    out.clear();
    // The service answers in RANGES of the input. Measured 10/10 it returns ONE range covering
    // everything, Latin included ("U+9019,4E2A reverb U+592A,591A,4E86" -> "U+9019,500B reverb U+592A,591A,4E86"), but nothing promises
    // that: any input it does NOT cover is SPLICED BACK verbatim, so a word of English inside a
    // Chinese line can never be dropped by the conversion. Ranges are taken in input order, and
    // each range's own trailing terminator (when it has one) is not part of the text.
    size_t next = 0;                                  // first input index not yet emitted
    for (DWORD r = 0; r < bag.dwRangesCount; ++r) {
        const MAPPING_DATA_RANGE& dr = bag.prgResultRanges[r];
        const size_t s = dr.dwStartIndex, e = dr.dwEndIndex;   // inclusive
        if (s < next || s >= in.size() || e < s) continue;     // out of order / empty: ignore
        if (s > next) out.append(in, next, s - next);           // an uncovered gap: verbatim
        std::wstring d;
        if (dr.pData) d.assign((const wchar_t*)dr.pData, dr.dwDataSize / sizeof(wchar_t));
        while (!d.empty() && d.back() == L'\0') d.pop_back();
        const size_t covEnd = (e + 1 < in.size()) ? e + 1 : in.size();
        if (d.empty()) out.append(in, s, covEnd - s);           // covered but no text: verbatim
        else           out += d;
        next = covEnd;
    }
    if (next < in.size()) out.append(in, next, std::wstring::npos);   // the uncovered tail
    ::MappingFreePropertyBag(&bag);
    return !out.empty();
}

std::string toTraditional(const std::string& utf8, bool* changed) {
    if (changed) *changed = false;
    if (utf8.empty()) return utf8;
    const std::wstring w = utf8ToWide(utf8);
    if (w.empty()) return utf8;
    // THE DETECTOR: the character map, which converts nothing that is not Simplified. If it
    // finds nothing to change, the line is already Traditional (or not Chinese) and is returned
    // EXACTLY AS WRITTEN. If it finds even ONE Simplified character, the WHOLE line goes through
    // ELS -- and ELS does not only change the script, it also localises VOCABULARY to Taiwan's
    // usage on the whole line (U+4E86,89E3 -> U+77AD,89E3, U+51FA,79DF,8F66 -> U+8A08,7A0B,8ECA, U+6C47,62A5 -> U+5F59,5831). That is accepted
    // product behaviour (WB, after the WA review): the reader asked for Taiwan Chinese, and a
    // line that is half mainland words in Traditional characters is the half-conversion this
    // net exists to prevent. On a SOURCE line (what the speaker said) it means the words shown
    // can be Taiwan's equivalents of the ones whisper heard; there is deliberately no second,
    // "least change" path.
    // zh-TW as the locale, explicitly: naming the region the text is FOR keeps the call honest.
    const int n = ::LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return utf8;                             // no converter: the prompt is all we have
    std::wstring lc((size_t)n, L'\0');
    if (::LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, w.c_str(), (int)w.size(),
                        &lc[0], n, nullptr, nullptr, 0) != n)
        return utf8;
    if (lc == w) return utf8;
    // Something IS Simplified: convert the whole line by WORD, falling back to the character map.
    std::wstring out;
    if (!elsConvert(elsHansToHant(), w, out)) out = lc;
    if (out == w) return utf8;
    if (changed) *changed = true;
    return wideToUtf8(out);
}

std::string toSimplified(const std::string& utf8, bool* changed) {
    if (changed) *changed = false;
    if (utf8.empty()) return utf8;
    const std::wstring w = utf8ToWide(utf8);
    if (w.empty()) return utf8;
    const int n = ::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return utf8;                             // no converter: the prompt is all we have
    std::wstring lc((size_t)n, L'\0');
    if (::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(),
                        &lc[0], n, nullptr, nullptr, 0) != n)
        return utf8;
    // THE DETECTOR: a character the map changes that is NOT one of the two the Simplified standard
    // keeps (U+8457 U+8457: U+8457,540D / U+8457,4F5C / U+663E,8457; U+77AD U+77AD: U+77AD,671B). A line whose only changes are those is
    // Simplified already -- see TrEngine.h -- and is returned exactly as written.
    bool traditional = false;
    if (lc.size() == w.size()) {
        for (size_t i = 0; i < w.size() && !traditional; ++i)
            if (lc[i] != w[i] && w[i] != (wchar_t)0x8457 && w[i] != (wchar_t)0x77AD) traditional = true;
    } else {
        traditional = (lc != w);                         // not one-for-one: trust the map's verdict
    }
    if (!traditional) return utf8;
    // Something IS Traditional: the whole line by WORD (U+5F8C,4F86 -> U+540E,6765, U+4EC0,9EBC -> U+4EC0,4E48), the map as the
    // fallback.
    std::wstring out;
    if (!elsConvert(elsHantToHans(), w, out)) out = lc;
    if (out == w) return utf8;
    if (changed) *changed = true;
    return wideToUtf8(out);
}

std::string scriptNet(const std::string& utf8, const std::string& lang, bool* changed) {
    if (lang == "zh-tw") return toTraditional(utf8, changed);
    if (lang == "zh")    return toSimplified(utf8, changed);
    if (changed) *changed = false;
    return utf8;
}

// SuVtitle 1.1.0 (RESUME 11): the COMPARISON fold (TrEngine.h). The character map alone, on purpose: its slips
// (U+8457 / U+77AD, U+4EC0,9EBC -> U+4EC0,9EBD -- see toSimplified) are the same on both sides of a comparison, so they cannot make
// two spellings of one phrase disagree; and nothing folded here is ever shown.
std::string hanFoldSimplified(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    const std::wstring w = utf8ToWide(utf8);
    if (w.empty()) return utf8;
    const int n = ::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return utf8;
    std::wstring lc((size_t)n, L'\0');
    if (::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(),
                        &lc[0], n, nullptr, nullptr, 0) != n)
        return utf8;
    return lc == w ? utf8 : wideToUtf8(lc);
}

static std::string normalizeForMatch(const std::string& s) {
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
        const uint32_t cp = nextCodepoint(s, i);
        if (isLatinLetter(cp)) appendUtf8(o, (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp);
        else if (isCjk(cp))    appendUtf8(o, cp);
        // /!\ The other alphabets are LETTERS too (wave WB), exactly the range cjkRatio already
        // counts as letters: Greek, Cyrillic, Hebrew, Arabic, Indic, Thai... Without this a
        // Russian or Arabic line normalised to NOTHING, and "nothing a person could have said"
        // is the first thing looksLikeHallucination drops -- every ru / ar line of a W2 pair
        // was silently thrown away as junk.
        else if (cp >= 0x0370 && cp <= 0x1FFF) appendUtf8(o, cp);
        else if (cp >= '0' && cp <= '9') o += (char)cp;
    }
    return o;
}

// The STRUCTURAL gate, level-free (wave WB widened it; see TrEngine.h). Three verdicts:
//   * nothing a person could have said: no letter and no digit at all ("...", "U+266A,266A");
//   * a music note at EITHER end, once the surrounding blanks and punctuation are gone:
//     whisper wraps what it hears as music in notes ("U+266A la la U+266A"), and a line it chose to
//     present as music is not dialogue -- the user asked for subtitles, not lyrics;
//   * nothing left once every WRAPPED span is removed: [..] (..) U+FF08..U+FF09 U+3010..U+3011 *..*. Removing
//     spans rather than testing the two ends is what keeps "(laughs) I agree (laughs)" and
//     "I like *this* one" -- real speech around an annotation -- on screen, while
//     "[Music]", "(Applause).", "*music*" and "[Music] [Applause]" all go.
// "*music*" is not hypothetical: it is turbo's whole answer to a pure music chord (10/10).
bool isAnnotationOnly(const std::string& text) {
    if (normalizeForMatch(text).empty()) return true;            // punctuation / dots / notes only
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size();) cps.push_back(nextCodepoint(text, i));
    auto blank = [](uint32_t c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000
            || c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || c == '-'
            || c == 0x3002 || c == 0xFF0C || c == 0xFF01 || c == 0xFF1F || c == 0xFF1B
            || c == 0xFF1A || c == 0x2026 || c == 0x2014 || c == 0x3001;
    };
    auto note = [](uint32_t c) { return c == 0x2669 || c == 0x266A || c == 0x266B || c == 0x266C; };
    size_t a = 0, b = cps.size();
    while (a < b && blank(cps[a]))     ++a;
    while (b > a && blank(cps[b - 1])) --b;
    if (a == b) return true;
    if (note(cps[a]) || note(cps[b - 1])) return true;
    // Remove every wrapped span; an opener with no closer is plain text, not an annotation.
    std::string rest;
    for (size_t i = a; i < b; ++i) {
        const uint32_t c = cps[i];
        uint32_t closer = 0;
        if      (c == '[')    closer = ']';
        else if (c == '(')    closer = ')';
        else if (c == 0xFF08) closer = 0xFF09;     // U+FF08 U+FF09
        else if (c == 0x3010) closer = 0x3011;     // U+3010 U+3011
        else if (c == '*')    closer = '*';
        if (closer) {
            size_t j = i + 1;
            while (j < b && cps[j] != closer) ++j;
            if (j < b) { i = j; continue; }        // skip the whole span, closer included
        }
        appendUtf8(rest, c);
    }
    return normalizeForMatch(rest).empty();
}

// Whisper on silence or room tone does not return nothing -- it returns its training
// set's most common caption. Everyone who ships whisper meets these, and a subtitle
// window that types "Thank you." at a quiet room is the single most embarrassing
// failure this feature can have. Two independent gates, both required to be cheap:
//   1. structural (isAnnotationOnly): no letters at all, or an annotation -> always junk;
//   2. the shortlist: only applied when the segment was QUIET, so a person who
//      actually says "thank you" out loud is still translated.
// PHONETIC GARBAGE (wave WB): characters from the IPA extensions (U+0250-02AF), the spacing
// modifier letters (U+02B0-02FF) and the phonetic extensions (U+1D00-1DBF). No transcript of any
// language this translator routes contains them -- and they are exactly what whisper writes when
// it decodes NOISE: "U+02BB,02BB,0254,02BB,0254,02BB,0254,02BB,0254,02BB", "recognis U+1D09 U+02B0,02CB,1D17,1D17,1D33,1D17 U+02B0,1D3E,1D07 Cream" (measured 10/10, `small` on a quiet
// tail). Two or more of them make a line junk, at any level; one stray mark does not.
static bool hasPhoneticGarbage(const std::string& text) {
    int n = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = nextCodepoint(text, i);
        if ((cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF)) ++n;
        if (n >= 2) return true;
    }
    return false;
}

// How much of a QUOTATION a transcript is (WB): its words -- any alphabet -- plus half a point per
// Han / kana / Hangul character (two of them carry about one English word).
static double echoWeight(const std::string& text) {
    double w = 0.0;
    bool inWord = false;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = nextCodepoint(text, i);
        const bool wide = isCjk(cp) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF);
        const bool letter = isLatinLetter(cp) || (cp >= 0x0370 && cp <= 0x1FFF) || (cp >= '0' && cp <= '9');
        if (wide)        { w += 0.5; inWord = false; }
        else if (letter) { if (!inWord) w += 1.0; inWord = true; }
        else if (cp != '\'' && cp != 0x2019) inWord = false;     // "don't" is one word
    }
    return w;
}
static constexpr double kEchoMinWeight = 4.0;

bool looksLikeHallucination(const std::string& text, double segPeakDbfs,
                            const std::string& prompt) {
    const std::string n = normalizeForMatch(text);
    if (n.empty()) return true;                                   // punctuation / dots only
    if (isAnnotationOnly(text)) return true;      // [BLANK_AUDIO], (musique), *music*, U+266A ... U+266A
    if (hasPhoneticGarbage(text)) return true;    // whisper decoding noise
    // THE ECHO AT ANY LEVEL (WB, after the review). Since finals are PROMPTED, whisper can hand
    // the prompt back at a NORMAL level too -- the mix playing in the room is "speech" -- and that
    // line would ship and then feed the next prompt. A transcript that is, normalised, a stretch
    // of the prompt it was decoded with, or of any glossary line, is junk WHATEVER its level once
    // it is long enough to be a quotation rather than a coincidence (kEchoMinWeight: four words,
    // or eight Han characters). A short real repeat ("Good morning everyone.") still answers only
    // to the quiet gate below.
    if (echoWeight(text) >= kEchoMinWeight) {
        if (!prompt.empty()) {
            const std::string p = normalizeForMatch(prompt);
            if (!p.empty() && p.find(n) != std::string::npos) return true;
        }
        for (const char* l : { "en", "zh", "zh-tw" }) {
            const std::string g = normalizeForMatch(asrGlossaryFor(l));
            if (!g.empty() && g.find(n) != std::string::npos) return true;
        }
    }

    static const char* kQuiet[] = {
        "thankyou", "thanksforwatching", "thankyouforwatching", "thankyouverymuch",
        "thanksforwatchingandsubscribe", "pleasesubscribe", "subscribe", "bye", "byebye",
        "you", "yeah", "okay", "ok", "uh", "um", "mm", "hmm", "oh", "so", "the",
        "silence", "music", "applause", "laughter", "outro", "intro",
        // The Mandarin equivalents, which turn up on any quiet Chinese-tuned decode. ONE spelling each, the
        // Simplified one (SuVtitle 1.1.0, RESUME 11): the line is folded to Simplified before it is compared
        // (hanFoldSimplified), so the TRADITIONAL junk the zh-tw prompt makes whisper write (WB, after the WA
        // review: U+8B1D,8B1D / U+8B1D,8B1D,89C0,770B / U+8ACB,8A02,95B1 / U+4E0B,6B21,518D,898B, which had rows of their own here) is caught by these
        // rows, and so is a line that mixes the two scripts. (U+5B57,5E55 is the same in both.)
        "\xe8\xb0\xa2\xe8\xb0\xa2", "\xe8\xb0\xa2\xe8\xb0\xa2\xe8\xa7\x82\xe7\x9c\x8b",
        "\xe8\xaf\xb7\xe8\xae\xa2\xe9\x98\x85", "\xe5\xad\x97\xe5\xb9\x95",
        "\xe4\xb8\x8b\xe6\xac\xa1\xe5\x86\x8d\xe8\xa7\x81",
    };
    // -32 dBFS of HOP RMS (not sample peak): 6 dB above the gate's own opening
    // threshold, and far below anything a person actually addressing the room produces
    // -- the selftest's real speech measured -9 to -13. So the shortlist can only fire
    // on a segment that barely tripped the gate at all.
    if (segPeakDbfs < kQuietGateDbfs) {
        // ONE ENTRY, OR SEVERAL GLUED TOGETHER (RESUME 11). Whisper strings its captions -- "Thank you for watching.
        // Please subscribe.", U+8B1D,8B1D,89C0,770B U+8ACB,8A02,95B1 -- and an exact comparison let every such pair through: the line must be the
        // list's entries end to end, nothing else (normalised, so spaces and punctuation between them do not count).
        static const std::vector<std::string> kQuietFolded = [] {
            std::vector<std::string> v;
            for (const char* k : kQuiet) v.push_back(hanFoldSimplified(k));
            return v;
        }();
        const std::string f = hanFoldSimplified(n);
        std::vector<char> reach(f.size() + 1, 0);       // reach[i]: f[0, i) is entries end to end
        reach[0] = 1;
        for (size_t i = 0; i < f.size(); ++i) {
            if (!reach[i]) continue;
            for (const std::string& k : kQuietFolded)
                if (!k.empty() && f.compare(i, k.size(), k) == 0) reach[i + k.size()] = 1;
        }
        if (reach[f.size()]) return true;
        // THE PROMPT'S OWN ECHO. Told what was said a moment ago and given nothing to
        // transcribe, whisper hands the prompt back -- so the instant W2 started prompting,
        // the shortlist above went blind: the junk on a quiet segment stopped being
        // "Thank you." and became the user's own previous sentence, which no shortlist can
        // ever contain. Same gate as the shortlist (QUIET only), so a speaker who really
        // does repeat himself is still translated.
        if (!prompt.empty()) {
            const std::string p = normalizeForMatch(prompt);
            if (!p.empty() && p.find(n) != std::string::npos) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// What whisper is told BEFORE it decodes.
// ---------------------------------------------------------------------------
std::string asrGlossaryFor(const std::string& lang) {
    // RAW UTF-8 (see the language table's note): this rides a multipart form field.
    if (lang == "zh") {
        // /!\ A PUNCTUATED CHINESE SENTENCE, and that is the whole point of this line. On the
        // 17/09 bench whisper produced NO Chinese punctuation at all until it was prompted
        // with some -- and the terms are the ones the room actually uses.
        // "U+5F55,97F3,68DA,91CC,7684,5BF9,8BDD,FF1A,97F3,91CF,3001,63A8,5B50,3001
        //  U+6DF7,54CD,3001,76D1,542C,3001,5BF9,767D,3001,73AF,5883,58F0,3002"
        //  = "A conversation in the studio: level, fader, reverb, monitoring, dialogue, ambience."
        return "\xe5\xbd\x95\xe9\x9f\xb3\xe6\xa3\x9a\xe9\x87\x8c\xe7\x9a\x84\xe5\xaf\xb9\xe8\xaf\x9d"
               "\xef\xbc\x9a\xe9\x9f\xb3\xe9\x87\x8f\xe3\x80\x81\xe6\x8e\xa8\xe5\xad\x90\xe3\x80\x81"
               "\xe6\xb7\xb7\xe5\x93\x8d\xe3\x80\x81\xe7\x9b\x91\xe5\x90\xac\xe3\x80\x81\xe5\xaf\xb9"
               "\xe7\x99\xbd\xe3\x80\x81\xe7\x8e\xaf\xe5\xa2\x83\xe5\xa3\xb0\xe3\x80\x82";
    }
    if (lang == "zh-tw") {
        // THE SAME SENTENCE, IN TRADITIONAL CHARACTERS AND TAIWAN'S STUDIO WORDS -- and this
        // line is not a nicety. Whisper has no "Traditional" switch: it writes Mandarin in the
        // script it was prompted in (10/10 bench, large-v3-turbo on bench_zh: no prompt or a
        // Simplified one -> Simplified, this one -> 100 % Traditional, and punctuated), and
        // only the prompt gets the characters a mechanical converter cannot choose (U+88E1 for U+91CC,
        // which the system's converter leaves alone). It is not the whole answer, though: in
        // auto mode the first sentence of a Chinese turn is prompted in the previous line's
        // language and turbo then writes Simplified, and `small` writes the odd sentence in
        // Simplified even under this line -- which is what toTraditional catches downstream
        // (main_translator.cpp, finaliseAudio).
        // "U+9304,97F3,5BA4,88E1,7684,5C0D,8A71,FF1A,97F3,91CF,3001,63A8,687F,3001,6B98,97FF,3001,76E3,807D,3001,5C0D,767D,3001,74B0,5883,97F3,3002"
        return "\xe9\x8c\x84\xe9\x9f\xb3\xe5\xae\xa4\xe8\xa3\xa1\xe7\x9a\x84\xe5\xb0\x8d\xe8\xa9\xb1"
               "\xef\xbc\x9a\xe9\x9f\xb3\xe9\x87\x8f\xe3\x80\x81\xe6\x8e\xa8\xe6\xa1\xbf\xe3\x80\x81"
               "\xe6\xae\x98\xe9\x9f\xbf\xe3\x80\x81\xe7\x9b\xa3\xe8\x81\xbd\xe3\x80\x81\xe5\xb0\x8d"
               "\xe7\x99\xbd\xe3\x80\x81\xe7\x92\xb0\xe5\xa2\x83\xe9\x9f\xb3\xe3\x80\x82";
    }
    if (lang == "en")
        return "Studio talk: levels, faders, reverb, monitoring, dialogue, ambience, stems, bus.";
    // Every other language: NOTHING. An English glossary in front of a French decode would
    // bias whisper towards English words, which is the opposite of what a prompt is for.
    return std::string();
}

std::string promptTail(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = s.size() - maxBytes;
    // Never start mid-codepoint: whisper's tokenizer would drop the whole prompt.
    while (cut < s.size() && ((unsigned char)s[cut] & 0xC0) == 0x80) ++cut;
    // Never start mid-word EITHER, where the script has words at all. Scanning forward at
    // most a quarter of the budget: a language without spaces (zh, ja) finds none and keeps
    // the codepoint cut, which is the correct answer there.
    const size_t limit = cut + maxBytes / 4;
    for (size_t i = cut; i < s.size() && i < limit; ++i) {
        if (s[i] == ' ') { cut = i + 1; break; }
    }
    return s.substr(cut);
}

// whisper.cpp's verbose_json reports the language by its FULL NAME. Map the ones a
// bilingual EN/ZH session can realistically produce; anything else comes back empty
// and the caller falls through to the script test, which is the real ground truth
// (with language=auto, whisper transcribes IN the source language, so the script IS
// the detection).
// /!\ "chinese" -> zh for BOTH scripts, and correctly so: whisper has one Mandarin, and which
// of zh / zh-tw a line is belongs to the pair being routed (sourceLangOf), not to the label.
std::string langNameToCode(const std::string& name) {
    struct Row { const char* name; const char* code; };
    static const Row kMap[] = {
        { "english", "en" }, { "chinese", "zh" }, { "cantonese", "yue" },
        { "japanese", "ja" }, { "korean", "ko" }, { "french", "fr" }, { "german", "de" },
        { "spanish", "es" }, { "italian", "it" }, { "portuguese", "pt" },
        { "russian", "ru" }, { "dutch", "nl" }, { "arabic", "ar" }, { "hindi", "hi" },
        { "vietnamese", "vi" }, { "thai", "th" }, { "indonesian", "id" },
        { "polish", "pl" }, { "turkish", "tr" }, { "swedish", "sv" }, { "danish", "da" },
        { "norwegian", "no" }, { "finnish", "fi" }, { "greek", "el" }, { "hebrew", "he" },
        { "ukrainian", "uk" }, { "czech", "cs" }, { "romanian", "ro" }, { "hungarian", "hu" },
        { "catalan", "ca" }, { "malay", "ms" }, { "tagalog", "tl" },
    };
    std::string lower;
    for (char c : name) lower += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    for (const auto& r : kMap) if (lower == r.name) return r.code;
    // Already a code? whisper's plain json path and any future server would give one.
    if (lower.size() == 2 || lower.size() == 3) return lower;
    return std::string();
}

// ---------------------------------------------------------------------------
// One NUMBER by key, from `from`. Sibling of jsonString and just as small: the only
// numbers this file ever reads are word timestamps, which are plain JSON doubles.
// Returns false when the key is not there or what follows it is not a number -- and the
// distinction matters, because "absent" and "0.0" mean very different things to a
// timestamp (the second is a legal instant).
// ---------------------------------------------------------------------------
static bool jsonNumber(const std::string& j, const char* key, double& out, size_t from = 0,
                       size_t limit = std::string::npos) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t k = j.find(pat, from);
    if (k == std::string::npos || (limit != std::string::npos && k >= limit)) return false;
    size_t i = k + pat.size();
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    const char* b = j.c_str() + i;
    char* e = nullptr;
    const double v = std::strtod(b, &e);
    if (e == b || !(v == v)) return false;
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
// The timestamped words, out of verbose_json.
//
// A TOLERANT WALKER, not a parser, and deliberately: the shape is known
//   "words":[{"word":" Good","start":0.14,"end":0.21,"t_dtw":-1,"probability":0.97}, ...]
// and it repeats once per whisper segment. So: start at the FIRST "words" key (everything
// before it is the document's own text/language, where a "word" key cannot appear), then
// take every "word" string and read the three numbers that follow it BEFORE the next one.
// Unknown fields are skipped by construction; an entry without both timestamps is skipped
// entirely rather than guessed at; and a response with no words at all simply yields none,
// which every caller is required to survive (AsrResult::words).
//
// Since wave WB this flat walker is the FALLBACK only -- for a document with no "segments"
// array -- and parseSegments below is the normal path, because only a segment's own text can
// repair what this server does to a split CJK character (see repairWords).
// ---------------------------------------------------------------------------
static void parseWords(const std::string& j, std::vector<AsrWord>& out, size_t from = 0,
                       size_t until = std::string::npos) {
    out.clear();
    size_t at = j.find("\"words\":", from);
    if (at == std::string::npos || (until != std::string::npos && at >= until)) return;
    const std::string key = "\"word\":";
    size_t p = j.find(key, at);
    while (p != std::string::npos && (until == std::string::npos || p < until)) {
        size_t nextP = j.find(key, p + key.size());
        if (until != std::string::npos && nextP != std::string::npos && nextP >= until)
            nextP = std::string::npos;
        // The window this entry owns: up to the next word, so a missing "end" can never be
        // silently borrowed from the NEXT word (or from the next whisper segment's own
        // start/end, which is the same mistake one object further out).
        const size_t limit = nextP == std::string::npos
                           ? (until == std::string::npos ? j.size() : until) : nextP;
        AsrWord w;
        double s = 0.0, e = 0.0, pr = 0.0;
        if (jsonString(j, "word", w.text, p)
            && jsonNumber(j, "start", s, p, limit)
            && jsonNumber(j, "end",   e, p, limit)) {
            w.start = s;
            w.end   = e;
            if (jsonNumber(j, "probability", pr, p, limit)) w.prob = pr;
            // WF: the DTW time (centiseconds; -1 = the server computed none) -- see AsrWord::dtw
            double dt = -1.0;
            if (jsonNumber(j, "t_dtw", dt, p, limit) && dt >= 0.0) w.dtw = dt / 100.0;
            // An end before its start is not a word, it is a decode that went wrong; and a
            // word with no text at all cannot be part of any sentence.
            if (!w.text.empty() && w.end >= w.start) out.push_back(std::move(w));
        }
        p = nextP;
    }
}

// U+FFFD as UTF-8, the server's stand-in for a token that is HALF a character.
static const char kReplacementUtf8[] = "\xef\xbf\xbd";

static std::string stripReplacement(std::string s) {
    for (size_t at; (at = s.find(kReplacementUtf8)) != std::string::npos;) s.erase(at, 3);
    return s;
}

// ---------------------------------------------------------------------------
// THE U+FFFD REPAIR (wave WB). whisper-server writes every word as its TOKEN's text, and the
// tokenizer is byte-level: a CJK character can be split across two tokens, each holding half
// of its UTF-8 bytes. Half a character is not valid UTF-8, so the server's JSON writer puts a
// U+FFFD in its place -- in the word list ONLY. The segment's own "text" was assembled from the
// raw bytes before that, and is intact. Measured on bench_zh (10/10): 4 of 6 segments carry
// such words (U+68C0 -> "U+FFFD","U+FFFD"; U+7EC8, U+73AF, U+7A0D, U+904D, U+51B3 likewise), and W3 builds its commit
// tail (a PART) by JOINING words -- which is how "U+6211,4EEC,518D,542C,4E00,FFFD,FFFD" could reach the screen.
//
// The words are the tokens IN ORDER, so the clean ones sit in the segment text one after the
// other. Walking them against it, every run of U+FFFD words is exactly the bytes between its
// two clean neighbours: that run becomes ONE word, with that text and the run's own timing
// (first start -> last end). Nothing is invented: when a clean word cannot be found where it
// must be, this segment's words are kept as they came and only the U+FFFD are stripped.
// ---------------------------------------------------------------------------
static void repairWords(const std::string& segText, std::vector<AsrWord>& words) {
    bool any = false;
    for (const auto& w : words) any |= (w.text.find(kReplacementUtf8) != std::string::npos);
    if (!any) return;

    std::vector<AsrWord> out;
    out.reserve(words.size());
    size_t pos = 0;
    bool   inRun = false;
    AsrWord run;
    auto closeRun = [&](size_t upTo) {
        if (!inRun) return;
        inRun = false;
        if (upTo > pos) {
            run.text = segText.substr(pos, upTo - pos);
            pos = upTo;
            if (!run.text.empty()) out.push_back(run);
        }
    };
    for (const auto& w : words) {
        if (w.text.find(kReplacementUtf8) != std::string::npos) {
            if (!inRun) { inRun = true; run = w; run.text.clear(); }
            else        { run.end = w.end; run.dtw = w.dtw; if (w.prob < run.prob) run.prob = w.prob; }
            continue;
        }
        // With a run open the clean word lies at least one byte further on (the run is at
        // least one whole character), which also keeps a repeated character -- a run of X
        // followed by a clean X -- from matching the run's own bytes.
        const size_t found = segText.find(w.text, inRun ? pos + 1 : pos);
        // The run must END where this clean word STARTS, and with no run the clean word must
        // follow its predecessor directly, blanks aside (anything else = alignment lost).
        bool adjacent = (found == pos);
        if (!inRun && found != std::string::npos && found > pos) {
            adjacent = true;
            for (size_t k = pos; k < found; ++k) adjacent &= (segText[k] == ' ');
        }
        if (found == std::string::npos || (!inRun && !adjacent)) {
            for (auto& x : words) x.text = stripReplacement(x.text);
            words.erase(std::remove_if(words.begin(), words.end(),
                                       [](const AsrWord& x) { return x.text.empty(); }),
                        words.end());
            return;
        }
        closeRun(found);
        out.push_back(w);
        pos = found + w.text.size();
    }
    closeRun(segText.size());
    words.swap(out);
}

// One segment's own string field, bounded to the segment (jsonString has no limit, and a
// segment with a missing key must not borrow its neighbour's).
static bool jsonStringIn(const std::string& j, const char* key, std::string& out,
                         size_t from, size_t limit) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t k = j.find(pat, from);
    if (k == std::string::npos || k >= limit) { out.clear(); return false; }
    return jsonString(j, key, out, k);
}

// whisper's segments, each with its OWN words (repaired) appended to `words`. Returns false
// when the document has no segment array, so the caller falls back to the flat walker.
//   "segments":[{"id":0,"text":"..","start":0.0,"end":3.41,"tokens":[..],"words":[..],
//                "temperature":0.0,"avg_logprob":-0.07,"no_speech_prob":9.9e-13}, ...]
static bool parseSegments(const std::string& j, std::vector<AsrSegment>& segs,
                          std::vector<AsrWord>& words) {
    segs.clear();
    words.clear();
    const size_t arr = j.find("\"segments\":");
    if (arr == std::string::npos) return false;
    const std::string idKey = "\"id\":";
    size_t p = j.find(idKey, arr);
    if (p == std::string::npos) return false;
    while (p != std::string::npos) {
        const size_t nextP = j.find(idKey, p + idKey.size());
        const size_t end   = nextP == std::string::npos ? j.size() : nextP;
        AsrSegment s;
        jsonStringIn(j, "text", s.text, p, end);
        size_t wordsAt = j.find("\"words\":", p);
        if (wordsAt == std::string::npos || wordsAt >= end) wordsAt = end;
        jsonNumber(j, "start", s.start, p, wordsAt);
        jsonNumber(j, "end",   s.end,   p, wordsAt);
        // The three segment-level numbers come AFTER the words array; no word carries them.
        jsonNumber(j, "temperature",    s.temperature, wordsAt, end);
        jsonNumber(j, "avg_logprob",    s.avgLogprob,  wordsAt, end);
        jsonNumber(j, "no_speech_prob", s.noSpeech,    wordsAt, end);
        std::vector<AsrWord> mine;
        if (wordsAt < end) {
            // The words array ends where the segment-level keys begin.
            size_t wordsEnd = j.find("\"temperature\":", wordsAt);
            if (wordsEnd == std::string::npos || wordsEnd > end) wordsEnd = end;
            parseWords(j, mine, wordsAt, wordsEnd);
        }
        repairWords(s.text, mine);
        // Cleaned HERE, per segment, BEFORE its range is recorded (WB, after the review): the
        // global erase that used to follow shifted every later segment's w0/w1 by the words it
        // removed, so a segment could be given its neighbour's first word.
        for (auto& w : mine)
            if (w.text.find(kReplacementUtf8) != std::string::npos) w.text = stripReplacement(w.text);
        mine.erase(std::remove_if(mine.begin(), mine.end(), [](const AsrWord& w) { return w.text.empty(); }),
                   mine.end());
        s.w0 = words.size();
        for (auto& w : mine) words.push_back(std::move(w));
        s.w1 = words.size();
        segs.push_back(std::move(s));
        p = nextP;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ASR
// ---------------------------------------------------------------------------
static std::string tidyTranscript(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    bool lastSpace = true;                       // also trims the leading space whisper adds
    for (char c : in) {
        const char t = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (t == ' ') { if (!lastSpace) { s += ' '; lastSpace = true; } }
        else { s += t; lastSpace = false; }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

double asrConfidence(const AsrResult& a) {
    double num = 0.0, den = 0.0;
    for (const auto& s : a.segments) {
        const double d = (s.end > s.start) ? (s.end - s.start) : 0.01;   // a zero-length segment still votes
        num += s.avgLogprob * d;
        den += d;
    }
    return den > 0.0 ? num / den : 0.0;
}

// The response half of asrTranscribe (wave WB: split out so the selftest can feed it a recorded
// document). Everything below was asrTranscribe's own tail, unchanged, except that the words now
// come per SEGMENT and repaired (parseSegments), with the flat walker as the fallback.
bool parseAsrResponse(const std::string& resp, AsrResult& out) {
    const uint32_t keepMs = out.ms;
    out = AsrResult{};
    out.ms = keepMs;
    // Top-level "text" and "language". They precede the "segments" array in this
    // server's output, so the FIRST occurrence of each key is the document-level one.
    std::string text, lang;
    jsonString(resp, "language", lang);
    jsonString(resp, "text", text);
    out.text        = tidyTranscript(text);
    out.languageRaw = lang;
    out.language    = langNameToCode(lang);
    // The words ride along at no cost -- verbose_json carries them whether or not anyone
    // reads them, so this is a parse, not a request. They are what the commit engine
    // measures a sentence with; a server that omits them costs that engine and nothing else.
    if (!parseSegments(resp, out.segments, out.words)) {
        // The flat fallback (no segment array): no ranges to keep, cleaned as one list. (The
        // segment path cleans per segment, in parseSegments.)
        parseWords(resp, out.words);
        for (auto& w : out.words) {
            // Belt and braces: a word the repair could not rebuild must still never carry U+FFFD.
            if (w.text.find(kReplacementUtf8) != std::string::npos) w.text = stripReplacement(w.text);
        }
        out.words.erase(std::remove_if(out.words.begin(), out.words.end(),
                                       [](const AsrWord& w) { return w.text.empty(); }),
                        out.words.end());
    }
    double lp = -1.0;
    if (jsonNumber(resp, "detected_language_probability", lp)) out.langProb = lp;
    // WF: "language_probabilities":{"en":0.87,"ru":0.026,...} -- the server's last top-level key
    // (rfind: a segment's text can never be mistaken for it). A tolerant walk, like the rest.
    {
        const std::string key = "\"language_probabilities\":";
        const size_t k = resp.rfind(key);
        size_t i = k == std::string::npos ? resp.size() : k + key.size();
        while (i < resp.size() && resp[i] == ' ') ++i;
        if (i < resp.size() && resp[i] == '{') {
            ++i;
            while (i < resp.size()) {
                while (i < resp.size() && (resp[i] == ' ' || resp[i] == ',')) ++i;
                if (i >= resp.size() || resp[i] != '"') break;
                const size_t q = resp.find('"', i + 1);
                if (q == std::string::npos || q - i > 16) break;
                const std::string code = resp.substr(i + 1, q - i - 1);
                i = q + 1;
                while (i < resp.size() && (resp[i] == ' ' || resp[i] == ':')) ++i;
                const char* b = resp.c_str() + i;
                char* e = nullptr;
                const double v = std::strtod(b, &e);
                if (e == b || !(v == v)) break;
                out.langProbs.emplace_back(code, v);
                i += (size_t)(e - b);
            }
        }
    }

    // The SCRIPT fact. The server's label is the PRIMARY signal; this is the
    // corroboration, and it wins when it is unambiguous -- with language=auto whisper
    // writes in the source language, so Han characters on screen are a fact where the
    // label is an opinion. W2 note: the router no longer consumes this boolean directly
    // (it routes on CODES, over a configurable pair) -- it consumes it as evidence about
    // zh, which is the only script this test can speak about. Kana and Hangul land in
    // cjkRatio too, which is exactly why `language` below is NOT overwritten from it:
    // a Japanese transcript must keep saying "ja" -- and (WB, after the WA review) why the
    // test itself is chineseByScript, which refuses any line holding kana or Hangul.
    out.chinese = chineseByScript(out.text, out.language);
    if (out.language.empty()) out.language = out.chinese ? "zh" : "en";

    return resp.find("\"text\":") != std::string::npos;
}

bool asrTranscribe(uint16_t port, const std::string& wav, AsrResult& out, DWORD timeoutMs,
                   std::string& err, const AsrOptions& opt) {
    out = AsrResult{};
    err.clear();
    const uint64_t t0 = ::GetTickCount64();

    // multipart/form-data by hand -- a handful of fields, one of them binary. The exact
    // shape was verified against this build of whisper-server with curl before it was
    // written.
    //   file            the 16 kHz mono WAV
    //   response_format verbose_json  <- the ONLY format that carries "language"
    //   language        auto, or the code a locked direction already knows
    static const char* kB = "----AudioPBTranslatorBoundary7d3f";
    std::string body;
    body.reserve(wav.size() + 512 + opt.prompt.size());
    auto field = [&](const char* name, const std::string& value) {
        body += "--"; body += kB; body += "\r\n";
        body += "Content-Disposition: form-data; name=\""; body += name; body += "\"\r\n\r\n";
        body += value; body += "\r\n";
    };
    body += "--"; body += kB; body += "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"segment.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n";
    field("response_format", "verbose_json");
    field("language", opt.language.empty() ? std::string("auto") : opt.language);
    field("temperature", "0.0");
    if (!opt.prompt.empty()) field("prompt", opt.prompt);
    if (opt.audioCtx > 0) {
        // audio_ctx applies to FINALS too since the fluidity pass: segments cap at 8 s and
        // kDraftAudioCtx (768) covers 15.36 s -- P1b PROVED the transcript byte-identical to
        // the full-window default at the 12 s draft cap, so this is a pure ~40% speedup of
        // the post-phrase wait (660 -> ~413 ms on the bench), not a quality trade.
        // (WB: the default IS kDraftAudioCtx, so every live request still sends exactly this;
        // only the file job asks for 0 = omitted = the full window its 30 s audio needs.)
        char ac[16];
        std::snprintf(ac, sizeof(ac), "%d", opt.audioCtx);
        field("audio_ctx", ac);
    }
    if (opt.draft) {
        // The live line's extra discount (greedy decode; measured no-ops on this server
        // but free where supported -- see kDraftAudioCtx in TrEngine.h).
        field("beam_size", "1");
        field("best_of",   "1");
        // ...and the one that is NOT a no-op. temperature_inc is whisper's fallback
        // ladder: a decode it judges bad is re-run at 0.2, 0.4, 0.6... and the 17/09 bench
        // measured three to four TIMES the wall clock when it triggers. On a draft that is
        // pure loss -- the final for this same utterance is seconds away and will say it
        // properly. Finals keep the ladder, which is the point of having two paths.
        field("temperature_inc", "0.0");
    } else {
        // WB: the offline file job's search (beam 5 / best_of 5). Omitted when 0, which is
        // every live final -- they keep the server's defaults, byte for byte.
        if (opt.beamSize > 0) field("beam_size", std::to_string(opt.beamSize));
        if (opt.bestOf   > 0) field("best_of",   std::to_string(opt.bestOf));
    }
    body += "--"; body += kB; body += "--\r\n";

    const std::wstring ct = std::wstring(L"multipart/form-data; boundary=") + utf8ToWide(kB);

    std::string resp;
    int st = 0;
    if (!httpPost(port, L"/inference", ct.c_str(), body.data(), body.size(), resp, timeoutMs, &st)) {
        err = "the speech recogniser did not answer";
        return false;
    }
    if (st != 200) {
        err = "the speech recogniser returned HTTP " + std::to_string(st);
        return false;
    }

    parseAsrResponse(resp, out);
    out.promptSent = opt.prompt;      // exactly what the "prompt" field above carried
    out.ms = (uint32_t)(::GetTickCount64() - t0);
    return true;
}

// ---------------------------------------------------------------------------
// MT
// ---------------------------------------------------------------------------
static std::string jsonEscapeC(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// The audio glossary, the reason a general model is usable at all here: left alone it
// renders "level" as U+6C34,5E73 ("horizontal"), which is exactly wrong in a mixing
// session, and the 17/09 A/B caught a rival model rendering "levels" as game LEVELS.
//
// /!\ ONE STABLE STRING PER DIRECTION, and it must stay that way. llama-server's prefix
// cache only hits on an unchanged PREFIX: the system prompt is the prefix of every single
// request, so assembling it out of pieces that vary per line (a name, a count, the time)
// would throw away the cache and add the whole prompt's prefill to every sentence. The
// target language name below is the only variable in it, and it varies per DIRECTION,
// not per line -- so the two directions each keep their own cached prefix.
static const char* kGlossEnToZh =
    "Audio glossary: level -> \\u97f3\\u91cf, mix -> \\u6df7\\u97f3, gain -> "
    "\\u589e\\u76ca, track -> \\u97f3\\u8f68, fader -> \\u63a8\\u5b50, mute -> "
    "\\u9759\\u97f3, reverb -> \\u6df7\\u54cd, monitor -> \\u76d1\\u542c, dialogue -> "
    "\\u5bf9\\u767d, ambience -> \\u73af\\u5883\\u58f0, take -> \\u4e00\\u6761, mic -> "
    "\\u8bdd\\u7b52, stems -> \\u5206\\u8f68, tone -> \\u6d4b\\u8bd5\\u97f3, print -> "
    "\\u5bfc\\u51fa, bus -> \\u603b\\u7ebf, EQ -> \\u5747\\u8861, compressor -> "
    "\\u538b\\u7f29\\u5668, delay -> \\u5ef6\\u8fdf, plugin -> \\u63d2\\u4ef6, cue -> "
    "\\u8fd4\\u9001, talkback -> \\u5bf9\\u8bb2, headphones -> \\u8033\\u673a.";

// /!\ NO "U+5EF6,8FDF -> delay" IN THIS DIRECTION (WB, after the WA review), though en->zh keeps
// "delay -> U+5EF6,8FDF". The Chinese word means BOTH the effect and LATENCY -- and in a studio,
// "the latency is too high" is said as often as "add some delay". A glossary entry is an
// order, not a hint: pinned to "delay" it would mistranslate every latency complaint, while
// left out the model reads the sentence and picks. The forward entry is safe because both
// English meanings ARE U+5EF6,8FDF in Chinese.
static const char* kGlossZhToEn =
    "Audio glossary: \\u97f3\\u91cf -> level, \\u6df7\\u97f3 -> mix, \\u589e\\u76ca -> "
    "gain, \\u97f3\\u8f68 -> track, \\u63a8\\u5b50 -> fader, \\u9759\\u97f3 -> mute, "
    "\\u6df7\\u54cd -> reverb, \\u76d1\\u542c -> monitor, \\u5bf9\\u767d -> dialogue, "
    "\\u73af\\u5883\\u58f0 -> ambience, \\u5206\\u8f68 -> stems, \\u6d4b\\u8bd5\\u97f3 -> "
    "tone, \\u5bfc\\u51fa -> print, \\u603b\\u7ebf -> bus, \\u5747\\u8861 -> EQ, "
    "\\u538b\\u7f29\\u5668 -> compressor, \\u63d2\\u4ef6 -> "
    "plugin, \\u8fd4\\u9001 -> cue, \\u5bf9\\u8bb2 -> talkback, \\u8033\\u673a -> "
    "headphones.";

// The SAME glossary for Traditional Chinese (wave WA) -- and not by converting the one above
// character for character, because the two studios do not use the same WORDS: a fader is a
// U+63A8,5B50 in Beijing and a U+63A8,687F in Taipei, a bus a U+603B,7EBF and a U+532F,6D41,6392, a plugin a U+63D2,4EF6 and a U+5916,639B,
// EQ U+5747,8861 and U+7B49,5316,5668. The Taiwan terms are the wave's design (its first thirteen are the very
// glossary the 10/10 bench handed Qwen3-4B). The same two rules as above, and one more:
//   * ONE STABLE STRING PER DIRECTION (the prefix cache, see kGlossEnToZh);
//   * pre-escaped JSON, appended raw;
//   * the reverse direction leaves out U+4E00,689D -> take, exactly as kGlossZhToEn leaves out its
//     U+4E00,6761: before it is a take, U+4E00,689D is the measure word in front of any long thin thing
//     (U+4E00,689D,8DEF, U+4E00,689D,8A0A,606F), and reversed it would turn every one of them into "a take";
//   * ...and, for the same reason (WB, after the WA review), U+8F38,51FA -> print and U+5EF6,9072 -> delay:
//     U+8F38,51FA is the everyday word for "output" (a print in Taiwan's studios, but an output
//     everywhere else), and U+5EF6,9072 is latency as much as the effect (see kGlossZhToEn). Both stay
//     in the en -> zh-tw direction, where they are right.
static const char* kGlossEnToZhTw =
    "Audio glossary: level -> \\u97f3\\u91cf, mix -> \\u6df7\\u97f3, gain -> "
    "\\u589e\\u76ca, track -> \\u97f3\\u8ecc, fader -> \\u63a8\\u687f, mute -> "
    "\\u975c\\u97f3, reverb -> \\u6b98\\u97ff, monitor -> \\u76e3\\u807d, dialogue -> "
    "\\u5c0d\\u767d, ambience -> \\u74b0\\u5883\\u97f3, take -> \\u4e00\\u689d, mic -> "
    "\\u9ea5\\u514b\\u98a8, stems -> \\u5206\\u8ecc, tone -> \\u6e2c\\u8a66\\u97f3, print -> "
    "\\u8f38\\u51fa, bus -> \\u532f\\u6d41\\u6392, EQ -> \\u7b49\\u5316\\u5668, compressor -> "
    "\\u58d3\\u7e2e\\u5668, delay -> \\u5ef6\\u9072, plugin -> \\u5916\\u639b, cue -> "
    "\\u8fd4\\u807d, talkback -> \\u5c0d\\u8b1b, headphones -> \\u8033\\u6a5f.";

static const char* kGlossZhTwToEn =
    "Audio glossary: \\u97f3\\u91cf -> level, \\u6df7\\u97f3 -> mix, \\u589e\\u76ca -> "
    "gain, \\u97f3\\u8ecc -> track, \\u63a8\\u687f -> fader, \\u975c\\u97f3 -> mute, "
    "\\u6b98\\u97ff -> reverb, \\u76e3\\u807d -> monitor, \\u5c0d\\u767d -> dialogue, "
    "\\u74b0\\u5883\\u97f3 -> ambience, \\u9ea5\\u514b\\u98a8 -> mic, \\u5206\\u8ecc -> "
    "stems, \\u6e2c\\u8a66\\u97f3 -> tone, \\u532f\\u6d41\\u6392 -> "
    "bus, \\u7b49\\u5316\\u5668 -> EQ, \\u58d3\\u7e2e\\u5668 -> compressor, "
    "\\u5916\\u639b -> plugin, \\u8fd4\\u807d -> cue, \\u5c0d\\u8b1b -> talkback, "
    "\\u8033\\u6a5f -> headphones.";

// The interpreter prompt. Two rules do all the work: "output ONLY the translation" (a chat
// model's instinct is to explain itself, which would fill the subtitle window with
// apologies) and the glossary above -- which is attached ONLY for en<->zh and en<->zh-tw,
// because those are the only pairs one was written for. A French line does not want Chinese
// terminology, and a zh<->zh-tw session (a script conversion) wants neither glossary: both
// sides of it already ARE the terms.
std::string qwenSystemPrompt(const std::string& srcLang, const std::string& dstLang) {
    std::string s =
        "You are a professional simultaneous interpreter in a live audio-production session. "
        "Translate the user's line into ";
    s += langPromptName(dstLang);
    s += ". Output ONLY the translation: no notes, no pinyin, no quotation marks, no "
         "explanation, no repetition of the source. Keep the natural spoken register of a "
         "studio conversation.";
    if      (srcLang == "en"    && dstLang == "zh")    { s += " "; s += kGlossEnToZh; }
    else if (srcLang == "zh"    && dstLang == "en")    { s += " "; s += kGlossZhToEn; }
    else if (srcLang == "en"    && dstLang == "zh-tw") { s += " "; s += kGlossEnToZhTw; }
    else if (srcLang == "zh-tw" && dstLang == "en")    { s += " "; s += kGlossZhTwToEn; }
    return s;
}

// Tencent HY-MT's own template, verbatim, because that is what the model was tuned on:
// a BARE instruction, no system prompt, and no history (measured on 17/09: given context
// it translates the context too). Kept even though the A/B did not adopt the model -- the
// driver is what makes the next candidate a config edit instead of a wave of work.
static std::string hyUserPrompt(const std::string& srcLang, const std::string& dstLang,
                                const std::string& text) {
    // Either Chinese, either side: zh-tw is a Chinese pair to Tencent's template as much as zh
    // is (langChineseNameJson names it U+7E41,9AD4,4E2D,6587 for the instruction).
    if (isHanLang(srcLang) || isHanLang(dstLang)) {
        // "U+5C06,4EE5,4E0B,6587,672C,7FFB,8BD1,4E3A{target}U+FF0C,6CE8,610F..."
        //  = "Translate the following text into {target}; output only the translation,
        //     with no extra explanation: {text}"
        std::string s = "\\u5c06\\u4ee5\\u4e0b\\u6587\\u672c\\u7ffb\\u8bd1\\u4e3a";
        s += langChineseNameJson(dstLang);
        s += "\\uff0c\\u6ce8\\u610f\\u53ea\\u9700\\u8981\\u8f93\\u51fa\\u7ffb\\u8bd1\\u540e"
             "\\u7684\\u7ed3\\u679c\\uff0c\\u4e0d\\u8981\\u989d\\u5916\\u89e3\\u91ca\\uff1a";
        s += jsonEscapeC(text);
        return s;
    }
    std::string s = "Translate the following segment into ";
    s += langPromptName(dstLang);
    s += ", without additional explanation. ";
    s += jsonEscapeC(text);
    return s;
}

bool mtTranslate(uint16_t port, const std::string& text,
                 const std::string& srcLang, const std::string& dstLang,
                 const std::vector<MtPair>& context, const std::string& style,
                 std::string& out, uint32_t& ms, DWORD timeoutMs, std::string& err) {
    out.clear();
    err.clear();
    const uint64_t t0 = ::GetTickCount64();

    // /!\ EVERY PIECE APPENDED BELOW IS ALREADY JSON-ESCAPED. The prompts carry \uXXXX
    // escape text (see the language table) and every piece of free text has been through
    // jsonEscapeC; nothing here may be escaped twice, and nothing may skip it.
    std::string body = "{\"model\":\"qwen\",\"temperature\":0,\"top_p\":1,\"stream\":false,"
                       "\"max_tokens\":320,\"messages\":[";
    if (style == "hy") {
        body += "{\"role\":\"user\",\"content\":\"";
        body += hyUserPrompt(srcLang, dstLang, text);
        body += "\"}";
    } else {
        body += "{\"role\":\"system\",\"content\":\"";
        body += qwenSystemPrompt(srcLang, dstLang);
        body += "\"}";
        // The conversation so far, OLDEST FIRST, as finished turns: a pronoun in this
        // sentence can only resolve against the noun in the last one if the model is shown
        // the last one. They sit between the system prompt and the line, which is also
        // where the prefix cache wants them -- the prefix only grows, it never changes.
        for (const auto& p : context) {
            body += ",{\"role\":\"user\",\"content\":\"";      body += jsonEscapeC(p.src); body += "\"}";
            body += ",{\"role\":\"assistant\",\"content\":\""; body += jsonEscapeC(p.dst); body += "\"}";
        }
        body += ",{\"role\":\"user\",\"content\":\"";
        body += jsonEscapeC(text);
        body += "\"}";
    }
    body += "]}";

    std::string resp;
    int st = 0;
    if (!httpPost(port, L"/v1/chat/completions", L"application/json",
                  body.data(), body.size(), resp, timeoutMs, &st)) {
        err = "the translator model did not answer";
        return false;
    }
    if (st != 200) {
        err = "the translator model returned HTTP " + std::to_string(st);
        return false;
    }
    if (!jsonString(resp, "content", out)) {
        err = "the translator model returned no content";
        return false;
    }
    out = tidyTranscript(out);
    // A chat model occasionally wraps its answer in quotes despite the instruction.
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
        out = out.substr(1, out.size() - 2);
    ms = (uint32_t)(::GetTickCount64() - t0);
    return true;
}

// ---------------------------------------------------------------------------
// The `auto` preset's probe (SuVtitle 1.1.0: back from AudioPB 50f79d6, with the live pair it times)
// ---------------------------------------------------------------------------
// Three seconds of QUIET ROOM TONE, written here rather than through buildWav16k because
// that one peak-normalises to -3 dBFS -- correct for a user's segment, wrong for this:
// a probe wants a noise FLOOR, not white noise at full level, or whisper starts inventing
// captions and the decode time we are trying to measure becomes a hallucination's length.
// Not digital silence either: several whisper builds short-circuit on an all-zero buffer,
// and a probe that measures the short-circuit measures nothing.
static std::string buildProbeWav(double seconds) {
    const uint32_t n = (uint32_t)(kAsrRate * seconds);
    const uint32_t bytes = n * 2u;
    std::string w;
    w.reserve(44 + bytes);
    w += "RIFF"; put32(w, 36u + bytes); w += "WAVE";
    w += "fmt "; put32(w, 16u);
    put16(w, 1); put16(w, 1);
    put32(w, (uint32_t)kAsrRate);
    put32(w, (uint32_t)kAsrRate * 2u);
    put16(w, 2); put16(w, 16);
    w += "data"; put32(w, bytes);
    uint32_t rng = 22695477u;                       // fixed seed: two runs are comparable
    for (uint32_t i = 0; i < n; ++i) {
        rng = rng * 1103515245u + 12345u;
        const int16_t s = (int16_t)((int32_t)((rng >> 16) & 0x1ff) - 256);   // ~ -42 dBFS
        put16(w, (uint16_t)s);
    }
    return w;
}

ProbeVerdict probeAutoPreset(uint16_t finalPort, bool gpu, DWORD timeoutMs) {
    ProbeVerdict v;
    if (!gpu) {
        // A machine that fell back to the CPU has already answered the question, and the
        // one thing it cannot afford is a three-second turbo transcription to confirm it.
        v.preset = "light";
        v.ms = 0;
        v.why = "the speech recogniser came up on the CPU fallback -> light";
        return v;
    }
    AsrOptions o;
    o.draft    = true;      // the cheap decode: we are timing the ENCODER, which is the cost
    o.language = "en";      // fixed, so language auto-detection cannot vary the measurement
    AsrResult r;
    std::string err;
    const std::string wav = buildProbeWav(3.0);
    const uint64_t t0 = ::GetTickCount64();
    const bool ok = asrTranscribe(finalPort, wav, r, timeoutMs, err, o);
    v.ms = (uint32_t)(::GetTickCount64() - t0);
    if (!ok) {
        // A probe that could not run is not evidence that the machine is slow. Balanced is
        // the answer that is wrong by the least on either side of the truth.
        v.preset = "balanced";
        v.why = "the probe call failed (" + err + ") -> balanced";
        return v;
    }
    if (v.ms > kProbeSlowMs) {
        v.preset = "balanced";
        v.why = "3 s on the final model took " + std::to_string(v.ms) + " ms (> "
              + std::to_string((unsigned)kProbeSlowMs) + ") -> balanced";
    } else {
        v.preset = "quality";
        v.why = "3 s on the final model took " + std::to_string(v.ms) + " ms -> quality";
    }
    return v;
}

} // namespace pbt

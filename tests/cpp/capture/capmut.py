# capmut.py -- MUTATION SENTINELS for the LIVE capture hub (wave L1b tests, 2026-10-25).
#
#   python -I capmut.py            run every mutant:  each is ONE textual change to ONE product file; the smoke
#                                  (capture_smoke.cpp --quick: virtual clock, no device, ~20 s) must FAIL on it
#   python -I capmut.py <filter>   only the mutants whose name contains <filter>
#   python -I capmut.py --check    only prove that every mutation applies exactly once to the sources (no compile)
#   (run it from capmut.cmd: it loads the Visual Studio environment and finds a Python; cl must be on PATH)
#
# It reuses tests/cpp/cmut.py's machinery (imported, not edited): the product sources and headers are copied into a
# scratch tree with the SAME relative layout (SUVTITLE_CMUT_WORK, else %TEMP%\suvtitle_cmut), the copy is mutated,
# compiled with cmut's flags and run with --quick. A mutant is CAUGHT when the smoke exits non-zero; one that does not
# COMPILE, or whose text does not apply exactly once, is a failed sentinel too (it would test nothing). The repository
# files are never touched. The mutants avoid anything that could crash the smoke (no dialog on the user's screen).
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))     # tests/cpp (python -I does not put the script's folder on sys.path)
import cmut                                      # noqa: E402

CORE_H, CORE_CPP = "engine/TrCaptureCore.h", "engine/TrCaptureCore.cpp"
HUB, WAV, DEC = "engine/TrCaptureHub.cpp", "engine/TrCaptureWav.cpp", "engine/TrDecimator.cpp"
ENUM = "engine/TrCaptureEnum.cpp"

MUTANTS = [
    # ---- the lane: arithmetic, guard, FIFO ------------------------------------------------------------------------
    ("guard: the +-4 clamp widened to +-40", CORE_H,
     "kGuardLimit     = 4.0f;", "kGuardLimit     = 40.0f;"),
    ("trim: dB converted as 10*log instead of 20*log", CORE_H,
     "return (float)std::pow(10.0, db / 20.0);", "return (float)std::pow(10.0, db / 10.0);"),
    ("channel pick: the wrong channel (chan % channels instead of chan - 1)", CORE_CPP,
     "const uint32_t c = chan - 1;", "const uint32_t c = chan % channels;"),
    ("mix strip: the MEAN of the channels instead of their SUM", CORE_CPP,
     "mono_[f] = guardSample(s * g);", "mono_[f] = guardSample(s * g / (float)channels);"),
    ("FIFO: a full FIFO drops the NEWEST, not the oldest", CORE_CPP,
     "        if (used + (uint64_t)n <= (uint64_t)kFifoCapacity) break;\n",
     "        if (used + (uint64_t)n <= (uint64_t)kFifoCapacity) break;\n"
     "        dropped_.fetch_add((uint64_t)n, std::memory_order_relaxed);\n        return;\n"),
    # ---- the fill rule --------------------------------------------------------------------------------------------
    ("slip: a source running fast is never trimmed (no dropped sample)", CORE_CPP,
     "if (ema_ > (double)(T + kSlipBandFrames) && avail >= n + 1) take = n + 1;", "if (false) take = n + 1;"),
    ("slip: a source running slow is never padded (no repeated sample)", CORE_CPP,
     "else if (ema_ < (double)(T - kSlipBandFrames) && n >= 2)   take = n - 1;", "else if (false)   take = n - 1;"),
    ("resync: a lane far above its target never jumps back", CORE_CPP,
     "if (++highRun_ >= kResyncBlocks) {", "if (++highRun_ >= 1000000000) {"),
    # ---- events and the hub's contract ----------------------------------------------------------------------------
    ("INPUT: reported on every call, not only on a change", CORE_CPP,
     "if (state == state_) return;", ";"),
    ("gate: close() leaves the gate open", CORE_CPP,
     "closed_.store(true);", "closed_.store(false);"),
    ("inputDelayMs: the decimator's group delay halved", CORE_CPP,
     "return (double)halfTaps * 1000.0 / (double)rate;", "return (double)halfTaps * 500.0 / (double)rate;"),
    ("LEVELS: every 100 ms instead of 200", HUB,
     "constexpr int kLevelFrames = kAsrRate / 5;", "constexpr int kLevelFrames = kAsrRate / 10;"),
    ("replacement: no lane is ever kept (all re-created)", HUB,
     "if (!slots[k] || next[k] || slots[k]->key != in.key || slots[k]->chan != in.chan) continue;", "if (true) continue;"),
    ("replacement: a trim change is not applied to the kept lane", HUB,
     "slots[k]->setGainDb(in.gainDb);", "(void)0;"),
    ("replacement: a kept source is opened again", HUB,
     "auto it = sources.find(kv.first);", "auto it = sources.end();"),
    ("start(): channel 9 accepted", HUB,
     "if (in.chan > trpipe::kLiveMaxChan) return false;", "if (false) return false;"),
    ("start(): a strip next to its own mix of the same key accepted", HUB,
     "(inputs[j].chan == in.chan || inputs[j].chan == 0 || in.chan == 0)", "(inputs[j].chan == in.chan)"),
    # ---- the test source and the decimator ------------------------------------------------------------------------
    ("wav: a source is 'stalled' only after 20 s of nothing", WAV,
     "constexpr double kStallSec = 2.0;", "constexpr double kStallSec = 20.0;"),
    ("wav: a gap never reports INPUT absent", WAV,
     'const char* state = gap ? "absent" : "error";', 'const char* state = gap ? "open" : "error";'),
    ("decimator: the cutoff pushed up to 8.8 kHz (9 kHz folds back)", DEC,
     "const double twoFc  = 2.0 * fcNorm;", "const double twoFc  = 2.6 * fcNorm;"),
    # ---- the fix round (RESUME 9, 2026-10-26) ----------------------------------------------------------------------
    ("RVA-8: every open resets the back-off ladder (a flapping device cycles at 0.2 s)", CORE_CPP,
     "    running_ = true;\n    openedAt_ = nowMs;\n", "    running_ = true;\n    openedAt_ = nowMs;\n    step_ = 0;\n"),
    ("RVA-8: a reopen inside a flapping episode is reported at once (INPUT flaps)", CORE_CPP,
     "    held_ = flapping_;\n", "    held_ = false;\n"),
    ("RVA-8: a healthy stream never resets the ladder (healthy = 50 s)", CORE_H,
     "static constexpr uint64_t kHealthyMs  = 5000;", "static constexpr uint64_t kHealthyMs  = 50000;"),
    ("RVA-2: the root climbs into ANY parent (a launcher becomes the application)", ENUM,
     "if (ce != pe && !isWebView2Process(ce)) return cur;", "if (false) return cur;"),
    ("RVA-2: a WebView2 process is not attributed to its host", ENUM,
     "if (ce != pe && !isWebView2Process(ce)) return cur;", "if (ce != pe) return cur;"),
    ("RVA-4: an unreadable creation time is trusted as a parent link", ENUM,
     "return cc && pc && pc <= cc;", "return !cc || !pc || pc <= cc;"),
    ("RVA-9: stop() ends the list watching (a pending request is lost)", HUB,
     "        closing = impl_->stopLocked();\n", "        closing = impl_->stopLocked();\n        impl_->endControl();\n"),
    ("RVA-10: a key holding '|' is accepted by start()", HUB,
     "if (in.key.find('|') != std::string::npos) return false;   // the lane separator (fix round RVA-10)",
     "if (false) return false;   // the lane separator (fix round RVA-10)"),
]

SOURCES = ["TrCaptureHub.cpp", "TrCaptureCore.cpp", "TrCaptureEnum.cpp", "TrCaptureWasapi.cpp", "TrCaptureWav.cpp",
           "TrDecimator.cpp"]
HEADERS = ["TrCaptureApi.h", "TrCaptureCore.h", "TrCaptureEnum.h", "TrCaptureWasapi.h", "TrCaptureWav.h", "TrDecimator.h"]

cmut.TARGETS["capture"] = dict(
    smoke="tests/cpp/capture/capture_smoke.cpp",
    sources=["engine/" + f for f in SOURCES],
    files=["engine/" + f for f in HEADERS] + ["common/TrPipe.h"],
    inc=["engine", "common"],
    libs=["ole32.lib", "avrt.lib", "mmdevapi.lib", "version.lib", "propsys.lib"],
    helper=None,
    args=["--quick"],
    mutants=MUTANTS,
)


def check():
    bad = 0
    for (name, rel, old, new) in MUTANTS:
        text = open(os.path.join(cmut.REPO, rel), "rb").read().decode("utf-8").replace("\r\n", "\n")
        n = text.count(old)
        print("  %s  applies %d time(s)  %s" % ("ok  " if n == 1 else "BAD ", n, name))
        bad += 0 if n == 1 else 1
    print("capmut --check: %d mutation(s) do not apply exactly once" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        sys.exit(check())
    sys.argv = [sys.argv[0], "capture"] + sys.argv[1:]
    cmut.main()

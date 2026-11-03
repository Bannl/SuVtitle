# cmut_live.py -- MUTATION SENTINELS for the LIVE parts of the C++ smokes (SuVtitle 1.1.0, L2 step 7, 2026-10-25).
# The repo's cmut.py machinery (copy the smoke + the product files it compiles into a scratch tree with the SAME relative layout,
# apply ONE textual change, compile with the smoke's own flags, run: CAUGHT = the smoke exits non-zero; a mutant that does not
# compile, or applies other than exactly once, is a FAILED sentinel), over the smokes L2 extended for the live mode:
#   trmgr_live   tests/cpp/trmgr/trmgr_smoke.cpp  [L1]-[L5]  the supervisor: live CTL scope, LIVE re-sent after a respawn, the SRC lists,
#                                                             the meters' slot, RV2-5 on the live path   (needs work\trmgr\SuVtitleEngine.exe:
#                                                             run build_trmgr_smoke.cmd first)
#   player_live  tests/cpp/player_smoke.cpp       [18]        the player's live gate
#   app_live     tests/cpp/app_smoke.cpp          [6]-[8]     LIVEPOS / LIVEIN / the live pair / the clamp / --live
#
#   python -I cmut_live.py <trmgr_live|player_live|app_live|all> [name-filter]      (run it from cmut_live.cmd: cl must be on PATH)
#   python -I cmut_live.py --dry                                                     every mutant applies exactly once? (no compile)
#
# The mutants are the live code's guards undone, the way a careless edit would; the text of each is the product's, verbatim. When
# a product line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I does not put the script's folder on the path)
import cmut

cmut.WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_live")

PLAYER_FILES = ["app/TrPlayer.h", "app/TrWindowState.h", "app/TrFileStore.h", "app/JsonW.h", "common/TrPipe.h",
                "app/MediaTracks.h", "app/SubtitleReader.h",
                "app/TrTranscript.h", "app/SourceTimecode.h"]   # (U3: TrPlayer.h includes them)

TARGETS = {
    "trmgr_live": dict(
        smoke="tests/cpp/trmgr/trmgr_smoke.cpp", sources=["app/TranslatorManager.cpp"],
        files=["app/TranslatorManager.h", "common/TrPipe.h"], inc=["app", "common"], libs=[],
        helper="tests/cpp/work/trmgr/SuVtitleEngine.exe",
        mutants=[
            # [L1] the live CTL, LIVE, INPUT, LEVELS, LIVE 0
            ("L1: the live CTL travels WITHOUT its scope (it would change the PLAYER's pair)", "app/TranslatorManager.cpp",
             "c.scope  = tp::kCtlScopeLive;", "(void)0;"),
            ("L1: LEVELS keep the FIRST frame (not the newest): the meters freeze", "app/TranslatorManager.cpp",
             "levels_ = std::move(l);", "if (!levelsDirty_) levels_ = std::move(l);"),
            ("L1: the meters' slot is never marked fresh (pollLevels says nothing new)", "app/TranslatorManager.cpp",
             "levelsDirty_ = true;", "levelsDirty_ = false;"),
            # [L2] the picker's list
            ("L2: a SHORT list (SRCEND counts more) is delivered at once instead of asked again", "app/TranslatorManager.cpp",
             "if (!whole && srcAsks < 2) {", "if (false) {"),
            ("L2: a short list is asked again for ever (no bound)", "app/TranslatorManager.cpp",
             "if (!whole && srcAsks < 2) {", "if (!whole && srcAsks < 2000) {"),
            ("L2: a list with lost lines counts as whole", "app/TranslatorManager.cpp",
             "const bool whole = !srcOverflow && srcPend.size() == e.n;", "const bool whole = true;"),
            ("L2: the SRCLIST ask is dropped (requestSources sets only the wish)", "app/TranslatorManager.cpp",
             "        srcWanted_ = true;\n        srcListPending_ = true;", "        srcWanted_ = true;"),
            # [L3] a respawn re-sends the live pair, the wanted LIVE and the list's ask
            ("L3: a respawned engine is not told to listen again (the wanted LIVE is lost with the crash)", "app/TranslatorManager.cpp",
             "livePending_ = live_.on;", "livePending_ = false;"),
            ("L3: a respawned engine never learns the picker is open (the list's ask is lost)", "app/TranslatorManager.cpp",
             "srcListPending_ = srcWanted_;", "srcListPending_ = false;"),
            ("L3: a respawned engine is not given the live pair back", "app/TranslatorManager.cpp",
             "liveCtlPending_ = !liveMode_.empty() || !liveLangA_.empty() || !liveLangB_.empty();", "liveCtlPending_ = false;"),
            # [L4] RV2-5 on the live path
            ("L4: the wanted LIVE is dropped only at the FOURTH death (one more crash into a capture driver)", "app/TranslatorManager.cpp",
             "if (++liveFailStreak_ >= 3 && live_.on) {", "if (++liveFailStreak_ >= 4 && live_.on) {"),
            ("L4: the user's next Listen is not forgiven (the drop is for ever)", "app/TranslatorManager.cpp",
             "if (on && !live_.on) { liveFailStreak_ = 0; liveLateDeaths_.clear(); }", "(void)0;"),
            ("L4: a death right after a LIVE 1 is not a failed start (the clock is never open)", "app/TranslatorManager.cpp",
             "now - lastLiveAt < 30000) {", "now - lastLiveAt < 0) {"),
            ("L4: a death right after LIVE 1 is counted as live but the back-off is skipped", "app/TranslatorManager.cpp",
             "            liveDeath();\n            failedStart();", "            liveDeath();"),
            # [L5] the 1.1.0 drain, on the live CTL's Clear
            ("L5: a Clear alone (no pair change) is not sent to the engine", "app/TranslatorManager.cpp",
             "if (liveCtlPending_ || clearPending_) {", "if (liveCtlPending_) {"),
            ("L5: the Clear flag is not carried by the live CTL", "app/TranslatorManager.cpp",
             "c.clear  = clearPending_;", "c.clear  = false;"),
            # [L6] the fix round of 26/10 (docs/REVIEW_RV-LB.md)
            ("L6a RVB-5: a refused LIVE does not drop the wish (re-sent to every engine)", "app/TranslatorManager.cpp",
             "live_ = tp::WireLive{};\n                        liveRefused_ = s.err;", "liveRefused_ = s.err;"),
            ("L6a RVB-5: the refusal is never told to the bridge", "app/TranslatorManager.cpp",
             "                        liveRefused_ = s.err;\n", ""),
            ("L6b RVB-7: stopSources keeps the wish (the respawned engine is asked again)", "app/TranslatorManager.cpp",
             "    srcWanted_ = false;\n    srcListPending_ = false;\n}", "    srcListPending_ = false;\n}"),
            ("L6c RVB-7: deaths right after a SRCLIST never drop the ask (the crash loop)", "app/TranslatorManager.cpp",
             "if (++srcFailStreak_ >= 3 && srcWanted_) {", "if (false) {"),
            ("L6c RVB-7: the user's next ask does not forgive", "app/TranslatorManager.cpp",
             "        srcFailStreak_ = 0;      // (fix round, RVB-7", "        (void)0;      // (fix round, RVB-7"),
            ("L6d RVB-13: every strip edit while listening forgives the live deaths", "app/TranslatorManager.cpp",
             "if (on && !live_.on) { liveFailStreak_ = 0;", "if (on) { liveFailStreak_ = 0;"),
            ("L6e RVB-6: the LINEs do not say which engine heard them", "app/TranslatorManager.cpp",
             "m.gen = incarnation;", "m.gen = 1;"),
            ("L6e RVB-6: a respawn is the same incarnation", "app/TranslatorManager.cpp",
             "            ++incarnation;", "            (void)0;"),
        ]),
    "player_live": dict(
        smoke="tests/cpp/player_smoke.cpp", sources=[], files=PLAYER_FILES + ["tests/web/we_vectors.txt"],
        inc=["common"], libs=["user32.lib", "shlwapi.lib", "ole32.lib"], helper=None,
        copies=[("tests/web/we_vectors.txt", "tests/cpp/we_vectors.txt")],
        mutants=[
            ("[18]: a FILE leaves while the live gate is closed (ensureCurrentJob)", "app/TrPlayer.h",
             "        if (live_) return;", "        (void)0;"),
            ("[18]: Translate all runs while the live mode listens (U1: it was the pre-analysis)", "app/TrPlayer.h",
             "if (!helper_ || live_) { cancelPre(); return; }", "if (!helper_) { cancelPre(); return; }"),
            ("[18]: closing the gate does not cancel the running job", "app/TrPlayer.h",
             "{ j.liveHit = true; cancelJob(j); }", "{ j.liveHit = true; }"),
            ("[18]: opening the gate does not restart the analysis", "app/TrPlayer.h",
             "        if (on) for (Job& j : jobs_) { j.liveHit = true; cancelJob(j); }\n        stateDirty_ = true;\n        schedule();",
             "        if (on) for (Job& j : jobs_) { j.liveHit = true; cancelJob(j); }\n        stateDirty_ = true;"),
            ("[18]: an error the live mode caused marks the CURRENT item failed", "app/TrPlayer.h",
             "const bool liveErr = live_ || j.liveHit;", "const bool liveErr = false;"),
            ("[18]: ...and a batch job refused by the live mode is remembered as a failure (U1: it was the pre-analysis)", "app/TrPlayer.h",
             # (U2b re-point: the user's rule 02/11 -- a failure the batch REDOES with the default model is not remembered: `&& !redo`;
             #  the mutant still takes the live mode's error for a failure)
             '} else if (j.pre && failed && !redo) {', '} else if (j.pre && d.result == "error" && !redo) {'),
            ("[18]: the strip says \"loading\" while the analysis is held by the live mode (the page waits for subtitles)", "app/TrPlayer.h",
             '!s_.userCancelled && !s_.failed) return "live";', '!s_.userCancelled && !s_.failed) return "loading";'),
        ]),
    "app_live": dict(
        smoke="tests/cpp/app_smoke.cpp", sources=[],
        files=["app/AudioPBImport.h", "app/AppConfig.h", "app/NextaleUpdate.h", "app/TrAssoc.h", "app/TrLaunch.h"] + PLAYER_FILES,
        inc=["common"], libs=["user32.lib", "shell32.lib", "shlwapi.lib", "ole32.lib", "advapi32.lib"], helper=None,
        mutants=[
            ("[6]: a rewritten tagged line goes to the END of the file (the lines reorder)", "app/AppConfig.h",
             # (03/11: the line above keeps it unique -- packUpdatePrefs has the same body; update_smoke pins that one)
             'if (l.rfind(t, 0) == 0) {\n            if (!put) { for (const auto& w : with) body += w + "\\n"; put = true; }',
             'if (l.rfind(t, 0) == 0) {\n            if (false) { for (const auto& w : with) body += w + "\\n"; put = true; }'),
            ("[6]: LIVEPOS reads the PLAYER's WINPOS line", "app/AppConfig.h",
             'if (l.rfind("LIVEPOS\\t", 0) == 0) as += "WINPOS" + l.substr(7) + "\\n";',
             'if (l.rfind("WINPOS\\t", 0) == 0) as += "WINPOS" + l.substr(7) + "\\n";'),
            ("[6]: the Live window's maximized flag is not written", "app/AppConfig.h",
             '"LIVEPOS\\t%d\\t%d\\t%d\\t%d\\t%d\\t%d", p.x, p.y, p.w, p.h, p.topmost ? 1 : 0,\n                  p.maximized ? 1 : 0);',
             '"LIVEPOS\\t%d\\t%d\\t%d\\t%d\\t%d\\t%d", p.x, p.y, p.w, p.h, p.topmost ? 1 : 0,\n                  0);'),
            ("[7]: liveSetNormal accepts what the engine refuses (no round trip through unpackLive)", "app/AppConfig.h",
             "if (!pbt::trpipe::unpackLive(pbt::trpipe::split(pbt::trpipe::packLive(l)), back)) return false;\n    out = back.inputs;",
             "back.inputs = in;\n    out = back.inputs;"),
            ("[7]: liveSetNormal returns the strips un-normalized (the trim not rounded to the wire's 0.1 dB)", "app/AppConfig.h",
             "    out = back.inputs;\n    return true;", "    out = in;\n    return true;"),
            ("[7]: ONE bad LIVEIN line drops the whole set", "app/AppConfig.h",
             "if (liveSetNormal(with, norm)) set = norm;", "if (liveSetNormal(with, norm)) set = norm; else return {};"),
            ("[7]: a strip's label is written to app.txt unescaped (a tab splits its line)", "app/AppConfig.h",
             "pbt::trpipe::esc(in.label)", "in.label"),
            ("[7]: the live pair is read from the PLAYER's LANGA", "app/AppConfig.h",
             'if (tag == "LIVELANGA")      { a = val; any = true; }', 'if (tag == "LANGA")      { a = val; any = true; }'),
            ("[7]: LIVEMODE is not migrated (en2zh stays en2zh)", "app/AppConfig.h",
             '    if (mode == "en2zh") mode = "a2b";\n    else if (mode == "zh2en") mode = "b2a";\n    if (mode != "auto" && mode != "a2b" && mode != "b2a") mode = "auto";\n    return any;',
             '    if (mode != "auto" && mode != "a2b" && mode != "b2a") mode = "auto";\n    return any;'),
            ("[8]: a window left on an unplugged screen is not brought back horizontally", "app/AppConfig.h",
             "    if (p.x + p.w > r) p.x = r - p.w;", "    (void)0;"),
            ("[8]: a window bigger than the work area is not shrunk", "app/AppConfig.h",
             "    if (p.w > ww) p.w = ww > 201 ? ww : 201;", "    (void)0;"),
            ("[8]: --live is parsed and ignored", "app/TrLaunch.h", "a.live = true;", "a.live = false;"),
            ("[8]: kCopyLive is --play's tag", "app/TrLaunch.h", "constexpr ULONG_PTR kCopyLive = 0x4C545653;", "constexpr ULONG_PTR kCopyLive = 0x50545653;"),
            ("[8]: a live copy may carry a payload", "app/TrLaunch.h", "return tag == kCopyLive && bytes == 0;", "return tag == kCopyLive;"),
            ("[8]: the seed reads another file than AudioPB's translator.txt", "app/AudioPBImport.h",
             'L"translator.txt"), text, 1ULL << 20)', 'L"settings.txt"), text, 1ULL << 20)'),
            # the fix round of 26/10 (docs/REVIEW_RV-LB.md)
            ("[6] RVB-11: a place near INT_MAX is taken (x + w overflows: the window created nowhere)", "app/AppConfig.h",
             "if (n >= 4 && x > -kWinPosMaxXY && x < kWinPosMaxXY && y > -kWinPosMaxXY && y < kWinPosMaxXY", "if (n >= 4"),
            ("[6] RVB-1: LIVESESSION takes any name (a path, '..', a drive, a stream)", "app/AppConfig.h",
             "if (!((c >= '0' && c <= '9') || c == '-' || c == '_')) return false;", "(void)c;"),
            ("[6] RVB-1: a Clear's empty name leaves the old LIVESESSION in place", "app/AppConfig.h",
             'return replaceTagged(old, "LIVESESSION", with);', 'return with.empty() ? old : replaceTagged(old, "LIVESESSION", with);'),
        ]),
}


def dry():
    bad = 0
    for name, t in TARGETS.items():
        for (mname, rel, old, new) in t["mutants"]:
            text = open(os.path.join(cmut.REPO, rel), "rb").read().decode("utf-8").replace("\r\n", "\n")
            n = text.count(old)
            if n != 1:
                bad += 1
            print("  %s  [%s] %s -- applies %d time(s)" % ("ok  " if n == 1 else "FAIL", name, mname, n))
    print("CMUT_LIVE --dry: %d mutant(s) do not apply exactly once" % bad)
    return 1 if bad else 0


def main():
    if "--dry" in sys.argv:
        sys.exit(dry())
    cmut.TARGETS.clear()
    cmut.TARGETS.update(TARGETS)
    cmut.main()


if __name__ == "__main__":
    main()

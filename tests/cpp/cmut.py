# cmut.py -- MUTATION SENTINELS for the C++ smokes (WC review, 2026-10-10).
# SuVtitle tests/cpp/cmut.py -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/cmut.py). Changed:
# the paths (app/, common/TrPipe.h, tests/cpp/, tests/web/we_vectors.txt copied next to the player smoke), the scratch
# folder (suvtitle_cmut), the trmgr helper (tests/cpp/work/trmgr/SuVtitleEngine.exe: run build_trmgr_smoke.cmd first).
# The mutants are AudioPB's, unchanged: each must apply exactly once to SuVtitle's sources.
#
#   python -I cmut.py <subwriter|filestore|trmgr|all>
#   (run it from cmut.cmd, which loads the Visual Studio environment (tools\vsenv.cmd) and finds a Python (tools\pyenv.cmd):
#   cl must be on PATH)
#
# Each mutant is ONE textual change to ONE product file (it must apply exactly once, or the
# sentinel FAILS: a mutation that changes nothing would "test" the original). The repo files are
# never touched: the smoke, the product sources it compiles and the headers they include are
# copied into a scratch tree with the SAME relative layout (so "../../ui/X.h" still resolves),
# the copy is mutated, compiled with the smoke's own flags, and run. A mutant is CAUGHT when the
# smoke exits non-zero; one that does not COMPILE is a failed sentinel too (it tests nothing).
import os, shutil, subprocess, sys, tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# (SuVtitle: SUVTITLE_CMUT_WORK names another scratch root, so two runs on different targets can go side by side)
WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut")
FLAGS = ["/nologo", "/std:c++17", "/EHsc", "/W4", "/MD", "/O2", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
         "/DUNICODE", "/D_UNICODE", "/D_CRT_SECURE_NO_WARNINGS"]

TARGETS = {
    "subwriter": dict(
        smoke="tests/cpp/subwriter_smoke.cpp", sources=[], files=["app/SubtitleWriter.h"],
        inc=[], libs=[], helper=None,
        mutants=[
            ("kinsoku ignored at spaces (the pre-review rule)", "app/SubtitleWriter.h",
             "u[i - 1] != ' ' && !noLineStart(u[r]) && !noLineEnd(u[l - 1])) {", "u[i - 1] != ' ') {"),
            ("srtSafe resumes AFTER the replacement (\"--->\" re-forms \"-->\")", "app/SubtitleWriter.h",
             "at = at > 0 ? at - 1 : 0;", "at += 2;"),
            # review of WG #14: the user's typed line breaks
            ("WG #14: a typed line break becomes a space again in the .srt", "app/SubtitleWriter.h",
             "std::vector<std::string> ml = wrapKeepingBreaks(m), sl = wrapKeepingBreaks(s);",
             "std::vector<std::string> ml = wrap(m), sl = wrap(s);"),
        ]),
    "filestore": dict(
        smoke="tests/cpp/filestore_smoke.cpp", sources=[],
        files=["app/TrFileStore.h", "common/TrPipe.h"], inc=["common"], libs=["user32.lib"], helper=None,
        mutants=[
            ("an .srt we wrote for ANOTHER media counts as ours", "app/TrFileStore.h",
             "r.mtime == mtime && !r.media.empty() && r.media == m) return true;", "r.mtime == mtime) return true;"),
            (".audiopb.srt taken unconditionally (the pre-review rule)", "app/TrFileStore.h",
             "    for (const auto& p : srtCandidates(dir, base, ext, tag, sibling))\n"
             "        if (!exists(p) || ownsForThisMedia(p)) return p;\n"
             "    return std::wstring();",
             "    const auto v = srtCandidates(dir, base, ext, tag, sibling);\n"
             "    if (!exists(v[0]) || ownsForThisMedia(v[0])) return v[0];\n"
             "    return v[1];"),
            ("a sibling medium does not change the name", "app/TrFileStore.h",
             "const std::wstring stem = (sibling && !ext.empty()) ? base + L\".\" + ext : base;",
             "const std::wstring stem = base;"),
            ("a subtitle/text file with the same base counts as a sibling medium", "app/TrFileStore.h",
             "if (lowerW(stripExt(l)) == base && isMediaExt(extOf(l))) return true;",
             "if (lowerW(stripExt(l)) == base) return true;"),
            ("a NO-REPLACE rename replaces anyway", "app/TrFileStore.h",
             "replace ? MOVEFILE_REPLACE_EXISTING : 0", "MOVEFILE_REPLACE_EXISTING"),
            ("the write time is not SET on the handle (identity guessed)", "app/TrFileStore.h",
             "return ::SetFileTime(h, nullptr, nullptr, &ft) ? t : 0;", "return t;"),
            ("a record's SRC compared case-sensitively", "app/TrFileStore.h",
             "return lowerW(fromUtf8(utf8)) == lowerW(w);", "return fromUtf8(utf8) == w;"),
            # WE (player v2): the partial record
            ("records still written under the v1 header (an old tray reads a partial as done)", "app/TrFileStore.h",
             'inline const char* recordHeader()   { return "# AudioPB Translator file cache v2"; }',
             'inline const char* recordHeader()   { return "# AudioPB Translator file cache v1"; }'),
            ("a partial record keeps a cue no range vouches for", "app/TrFileStore.h",
             "            for (const auto& c : r.cues) if (!inRanges(r.ranges, c.startMs)) return false;",
             "            for (const auto& c : r.cues) if (!inRanges(r.ranges, c.startMs)) {}"),
            ("a malformed RNG half-read instead of refused", "app/TrFileStore.h",
             "            if (!tp::parseRanges(t.size() >= 2 ? t[1] : std::string(), r.ranges)) return false;\n            haveRng = true;",
             "            tp::parseRanges(t.size() >= 2 ? t[1] : std::string(), r.ranges);\n            haveRng = true;"),
            # WG: one file per language
            ("WG: file L takes the TRANSLATION of a line spoken in L (the user's mixed .en.srt)", "app/TrFileStore.h",
             "    if (lowerAscii(c.lang) == L && !c.src.empty()) { out = c.src; return true; }\n", ""),
            ("WG: Simplified Chinese tagged plain zh (players expect zh-CN)", "app/TrFileStore.h",
             "    if (c == \"zh\") return \"zh-CN\";\n", ""),
            ("WG: the SRTL lines are not read back", "app/TrFileStore.h",
             "            if (!f.lang.empty() && !f.path.empty()) r.srtFiles.push_back(f);", "            (void)f;"),
            ("WG: the sibling candidates miss the reverse order (zh<->fr for fr)", "app/TrFileStore.h",
             "        for (int order = 0; order < 2; ++order) {", "        for (int order = 0; order < 1; ++order) {"),
            # ENGINE (the coordinator, 11/10): the cue engine's version -- THE COMPARISON and its exception
            ("ENGINE: THIS engine's record counted as older (<= instead of <)", "app/TrFileStore.h",
             "inline bool olderEngine(int engine) { return engine < tp::kFileEngineVersion; }",
             "inline bool olderEngine(int engine) { return engine <= tp::kFileEngineVersion; }"),
            ("ENGINE: a NEWER engine's record counted as older (!= instead of <)", "app/TrFileStore.h",
             "inline bool olderEngine(int engine) { return engine < tp::kFileEngineVersion; }",
             "inline bool olderEngine(int engine) { return engine != tp::kFileEngineVersion; }"),
            ("ENGINE: an OLDER engine's record counted as current (> instead of <)", "app/TrFileStore.h",
             "inline bool olderEngine(int engine) { return engine < tp::kFileEngineVersion; }",
             "inline bool olderEngine(int engine) { return engine > tp::kFileEngineVersion; }"),
            ("ENGINE: an EDITED older record is a miss too (edits discarded silently)", "app/TrFileStore.h",
             "inline bool staleRecord(int engine, bool edited) { return olderEngine(engine) && !edited; }",
             "inline bool staleRecord(int engine, bool edited) { (void)edited; return olderEngine(engine); }"),
            ("ENGINE: no ENGINE line written", "app/TrFileStore.h",
             '    o += "ENGINE\\t" + std::to_string(r.engine) + "\\n";\n', ""),
            ("ENGINE: the full parse ignores the line (every record reads as older)", "app/TrFileStore.h",
             "            r.engine = engineOf(t[1]);", "            (void)0;"),
            ("ENGINE: the peek ignores the line", "app/TrFileStore.h",
             "            s.engine = engineOf(t[1]);", "            (void)0;"),
            ("ENGINE: a value with junk after the digits read as a number", "app/TrFileStore.h",
             "        if (c < '0' || c > '9') return 0;", "        if (c < '0' || c > '9') break;"),
            # review of WG #5: the subtitle-file editor's overlap count
            ("WG #5: two lines that only TOUCH counted as overlapping", "app/TrFileStore.h",
             "        while (!open.empty() && open.top() <= x.first) open.pop();",
             "        while (!open.empty() && open.top() < x.first) open.pop();"),
            ("WG #5: the pairs never counted (every edit allowed)", "app/TrFileStore.h",
             "        n += open.size();", "        (void)n;"),
        ]),
    "trmgr": dict(
        smoke="tests/cpp/trmgr/trmgr_smoke.cpp", sources=["app/TranslatorManager.cpp"],
        files=["app/TranslatorManager.h", "common/TrPipe.h"], inc=["app", "common"], libs=[],
        helper="tests/cpp/work/trmgr/SuVtitleEngine.exe",
        mutants=[
            ("a failed FILE-command write declares the death WITHOUT reading the last words",
             "app/TranslatorManager.cpp",
             "                drainLastWords();\n                failCmds(cmds, kFileEngineStopped);",
             "                failCmds(cmds, kFileEngineStopped);"),
            ("a failed CTL write declares the death WITHOUT reading the last words",
             "app/TranslatorManager.cpp",
             "under this write must be reported as having finished it.\n                drainLastWords();",
             "under this write must be reported as having finished it."),
            # WE (player v2)
            ("an FFOCUS is written for a job that has answered (or never ran here)", "app/TranslatorManager.cpp",
             "if (c.kind == FileCmd::Kind::Focus && !openJobs.count(c.id)) continue;", "if (false) continue;"),
            ("an UNSENT FILE is not patched by an FFOCUS (its focus is lost)", "app/TranslatorManager.cpp",
             "                c.file.focusMs = ms;\n                c.wire = tp::packFile(c.file);\n", ""),
            ("a synthesized FDONE is not flagged (the player would trust a dead helper's last report)", "app/TranslatorManager.cpp",
             "    m.synth        = true;             // made up HERE", "    m.synth        = false;            // made up HERE"),
        ]),
    # SuVtitle FIX ROUND (2026-10-23, REVIEW_RV2_App.md): the engine pipe and the supervisor's starts -- pipe_smoke
    # (the REAL TranslatorManager.cpp vs pipeake_engine.cpp, ~45 s a run)
    "pipe": dict(
        smoke="tests/cpp/pipe/pipe_smoke.cpp", sources=["app/TranslatorManager.cpp"],
        files=["app/TranslatorManager.h", "common/TrPipe.h"], inc=["app", "common"], libs=["advapi32.lib"],
        helper="tests/cpp/work/pipe/SuVtitleEngine.exe",
        mutants=[
            ("RV2-1: a name someone created first is JOINED again (no FILE_FLAG_FIRST_PIPE_INSTANCE)", "app/TranslatorManager.cpp",
             "PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,", "PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,"),
            ("RV2-1: the default DACL again (Everyone and Anonymous may read)", "app/TranslatorManager.cpp",
             "1, 1 << 18, 1 << 18, 0, &sa);", "1, 1 << 18, 1 << 18, 0, nullptr); (void)sa;"),
            ("RV2-1: any client taken for the engine (no client check)", "app/TranslatorManager.cpp",
             "if (::GetNamedPipeClientProcessId(pipe, &cpid) && cpid == pi.dwProcessId) { connected = true; break; }",
             "{ (void)cpid; connected = true; break; }"),
            ("RV2-2: stop() cannot end the CONNECT wait", "app/TranslatorManager.cpp",
             "::WaitForMultipleObjects(quitEv ? 3 : 2, waits,", "::WaitForMultipleObjects(2, waits,"),
            ("RV2-2: stop() cannot end the HELLO wait", "app/TranslatorManager.cpp",
             "tp::readMessage(pipe, hello, kConnectDeadlineMs, &broken, quitEv, &woken)",
             "tp::readMessage(pipe, hello, kConnectDeadlineMs, &broken, nullptr, &woken)"),
            # (U2b re-point, 02/11/2026: the LIVE wave's supervisor reads the time once per lap -- `now - lastSpawn`; same intent)
            ("RV2-5: a death soon after HELLO is not a failed start (the crash loop says nothing again)", "app/TranslatorManager.cpp",
             "        if (now - lastSpawn < 30000) {\n            failedStart();",
             "        if (false) {\n            failedStart();"),
        ]),
    # WE (player v2): the player's CORE, driven against the fake host (the bridge in a box)
    "player": dict(
        smoke="tests/cpp/player_smoke.cpp", sources=[],
        files=["app/TrPlayer.h", "app/TrWindowState.h", "app/TrFileStore.h", "app/JsonW.h", "common/TrPipe.h",
               "tests/web/we_vectors.txt", "app/MediaTracks.h", "app/SubtitleReader.h",
               "app/TrTranscript.h", "app/SourceTimecode.h"],   # (U3: TrPlayer.h includes them)
        inc=["common"], libs=["user32.lib", "shlwapi.lib", "ole32.lib"], helper=None,
        copies=[("tests/web/we_vectors.txt", "tests/cpp/we_vectors.txt")],
        mutants=[
            # the adversarial review of WE (REVIEW_WE.md): each fix removed in turn
            ("C1: the tray's File-tab request is never consumed (it sticks to every later post)", "app/TrWindowState.h",
             "if (file) showFile = false;", "(void)0;"),
            ("C2: open() plays by an INDEX taken before the insert (the wrong item)", "app/TrPlayer.h",
             "        if (!add.empty()) {\n"
             "            const size_t at = (cur_ == npos) ? items_.size() : cur_ + 1;\n"
             "            if (insertItems(at, add) && !firstId) firstId = add.front().id;\n"
             "        }\n"
             "        if (firstId) loadItem(indexOf(firstId), true, -1, analyseFirst);",
             "        const size_t stale = firstId ? indexOf(firstId) : npos;\n"
             "        if (!add.empty()) {\n"
             "            const size_t at = (cur_ == npos) ? items_.size() : cur_ + 1;\n"
             "            if (insertItems(at, add) && !firstId) firstId = add.front().id;\n"
             "        }\n"
             "        if (firstId) loadItem(stale != npos ? stale : indexOf(firstId), true, -1, analyseFirst);"),
            ("C3: two cues TOUCHING by 1 ms taken for an overlap (the resumed run's first cue dropped)", "app/TrPlayer.h",
             "constexpr uint64_t kTouchMs       = 1;", "constexpr uint64_t kTouchMs       = 0;"),
            ("C4: a switch during a reload judged against the OLD page's folders", "app/TrPlayer.h",
             "        committed_ = mapped_;\n        host_.reloadWindow();", "        host_.reloadWindow();"),
            ("C5: a re-post of the SAME file keeps the opening's autoplay and resume", "app/TrPlayer.h",
             "            s_.startMs = s_.lastPosMs;\n            s_.autoplay = s_.resumed = false;", "            (void)0;"),
            ("P1: posts reach the window while its page reloads (the OLD document)", "app/TrPlayer.h",
             "bool windowBusy() const { return attached_ && !viewReady_; }", "bool windowBusy() const { return false; }"),
            ("P4: a tick chases a playhead the job is already heading to", "app/TrPlayer.h",
             "if (why == PosReason::Tick && t == p && lastFocus <= p) return false;", "(void)0;"),
            ("P2: a scan's identities ignored (one disk call per file on the UI thread)", "app/TrPlayer.h",
             "if (known && known->ok) { size = known->size; mtime = known->mtime; }\n        else if (!host_.identity(path, size, mtime)) return false;",
             "if (!host_.identity(path, size, mtime)) return false;"),
            ("P8: persist() leaves the running analysis unsaved", "app/TrPlayer.h",
             # (WG: persist() flushes the per-language files and the edits first -- the mutant keeps that, drops the save)
             # (U2b re-point: the user's answer 02/11 -- G2's flushDocxDue() now follows flushFiles(); the mutant keeps both)
             "    void persist() {\n        flushFiles();\n        flushDocxDue();                                      // U2b (G2): the last rename never lost\n        for (Job& j : jobs_) savePartial(*j.set);",
             "    void persist() {\n        flushFiles();\n        flushDocxDue();                                      // U2b (G2): the last rename never lost"),
            ("the latest FPROG trusted at once (no lag: a crash claims a window whose cues may be missing)", "app/TrPlayer.h",
             "S.confirmed = merged(S.confirmed, j->lastProg);\n            j->lastProg = rg;",
             "S.confirmed = merged(S.confirmed, rg);\n            j->lastProg = rg;"),
            ("a SYNTHESIZED FDONE vouches like the helper's own", "app/TrPlayer.h",
             "if (!synthesized) j.set->confirmed = merged(j.set->confirmed, j.lastProg);",
             "j.set->confirmed = merged(j.set->confirmed, j.lastProg);"),
            ("FFOCUS on every report (the rule bypassed)", "app/TrPlayer.h",
             "if (j && shouldFocus(j->set->ranges, j->set->totalMs ? j->set->totalMs : s_.durMs, ms, s_.lastFocusMs, why)) {",
             "if (j) {"),
            # U1 (02/11/2026, the user: nothing analysed unless asked -- docs/PROGRESS_U1.md); the pre-analysis's mutant went
            # with it ("pre-analysis while the current file is still analysed": there is no pre-analysis any more)
            ("U1: a file loaded is analysed by itself (the old switch's rule)", "app/TrPlayer.h",
             # (U3 re-point: the gate asks EITHER wish now -- analysisWanted)
             "        if (!analysisWanted()) return;                       // U1: never by itself -- Translate, or \"Translate all\" (U3: or\n                                                             // the transcript's wish: Transcribe, \"Transcribe all\")\n",
             ""),
            ("U1: a pressed file goes on being analysed once it is left (only the batch analyses a file not playing)", "app/TrPlayer.h",
             # (U3 re-point: kept for EITHER batch)
             "                if ((batchQ_.count(s_.itemId) || tbatchQ_.count(s_.itemId)) && !j->cancelSent) j->pre = true;\n                else cancelJob(*j);",
             "                j->pre = true;"),
            ("U1: a QUEUED file left mid-analysis is cancelled (its job restarts later from its partial)", "app/TrPlayer.h",
             "                if ((batchQ_.count(s_.itemId) || tbatchQ_.count(s_.itemId)) && !j->cancelSent) j->pre = true;",
             "                if (false) j->pre = true;"),   # (U3 re-point)
            ("U1: Translate all leaves the playing file out", "app/TrPlayer.h",
             "            if (it.missing || knownComplete(it)) continue;",
             "            if (it.missing || knownComplete(it) || (loaded_ && it.id == s_.itemId)) continue;"),
            ("U1: Cancel all leaves the batch queued (it starts again by itself)", "app/TrPlayer.h",
             "        for (uint64_t id : batchQ_) batchCancelled_.insert(id);\n        batchQ_.clear();",
             "        for (uint64_t id : batchQ_) batchCancelled_.insert(id);"),
            ("U1: a failed batch file retried at once (for ever)", "app/TrPlayer.h",
             # (U2b re-point: the user's rule 02/11 -- a failure the batch REDOES once with the default model stays queued:
             #  `failed && !redo`; the mutant keeps every failed file queued, as before)
             "        if (d.result == \"ok\" || (failed && !redo)) batchFinished(j.set->key, failed);",
             "        if (d.result == \"ok\") batchFinished(j.set->key, false);"),
            ("U1: the batch runs while Live listens", "app/TrPlayer.h",
             "        if (!helper_ || live_) { cancelPre(); return; }   // (SUVTITLE LIVE: the gate, setLive)",
             "        if (!helper_) { cancelPre(); return; }   // (SUVTITLE LIVE: the gate, setLive)"),
            ("U1: the press rebuilds the generation (the subtitles on screen re-sent)", "app/TrPlayer.h",
             "        if (!force && !s_.wire && s_.set && !s_.set->complete && s_.set->key == heardKey()) {",
             "        if (false) {"),
            ("U1: a failure no longer asked for reads \"off\" (not the error)", "app/TrPlayer.h",
             "        if (s_.failed) return \"error\";", "        (void)0;"),
            ("U1: the queued / cancelled badges never said", "app/TrPlayer.h",
             "        if (batchQ_.count(it.id)) state = \"queued\";\n        else if (batchCancelled_.count(it.id)) state = \"cancelled\";\n    }",
             "    }"),
            ("a running pre-analysis is NOT adopted (restarted instead)", "app/TrPlayer.h",
             "if (pj) { n.set = pj->set; n.wire = pj->wire; pj->pre = false; }", "if (false) { n.set = pj->set; n.wire = pj->wire; pj->pre = false; }"),
            ("a folder the page has not committed is never reloaded for", "app/TrPlayer.h",
             "if (committed_.count(lowerW(s_.folder))) return false;\n        mapNow();",
             "return false;\n        mapNow();"),
            ("the overlap guard is gone", "app/TrPlayer.h",
             "if (overlapsHeld(S, c.startMs, c.endMs)) return;", "if (false) return;"),
            # (U2b re-point, 02/11/2026 -- U1's double-guard rule: ensureCurrentJob's `s_.userCancelled ||` went (it could never be
            #  true behind its `!analysisWanted()` line: docs/PROGRESS_U2b.md, DECISIONS); the guard left is cancel()'s end of the wish)
            ("a job the USER cancelled restarts by itself", "app/TrPlayer.h",
             "        s_.wanted = false;\n", ""),
            ("the media URL loses its ?v=<mediaKey> (stale HTTP cache for a re-encoded file)", "app/TrPlayer.h",
             "+ L\"?v=\" + w16(n.mediaKey);", ";"),
            # WG (the user's first real test, 11/10): one .srt per language, written as the analysis goes
            ("WG: a language's file written from the set itself, never from a more complete analysis (the shared original shrinks)",
             "app/TrPlayer.h",
             "const bool fromSib = bestSibling(S, L, sib, &sibKey);",      # (WH re-point: the same line, D6)
             "const bool fromSib = false;"),
            ("WG: a language with no line yet gets an EMPTY file (an earlier one wiped)", "app/TrPlayer.h",
             "if (pbtrf::countInLang(cues, L) == 0) {", "if (false) {"),
            ("WG: the 30 s counted from the LAST new cue (a steady analysis never writes)", "app/TrPlayer.h",
             "        if (S.srtDirty) return;\n        S.srtDirty = true;", "        S.srtDirty = true;"),
            ("WG: no progressive write (the files only at the end)", "app/TrPlayer.h",
             "            if (S->srtDirty && now - S->srtDirtySince >= kSrtEveryMs) writeSrts(*S);", "            (void)S;"),
            ("WG: a pause does not write the files", "app/TrPlayer.h",
             "if ((why == PosReason::Pause || why == PosReason::Ended) && s_.set && s_.set->srtDirty) writeSrts(*s_.set);", "(void)0;"),
            ("WG: a switch leaves the files behind", "app/TrPlayer.h",
             "        else if (s_.set && s_.set->srtDirty) writeSrts(*s_.set);\n", ""),
            # WG: the subtitle editor's checks
            ("WG editor: an edit that OVERLAPS a line is applied", "app/TrPlayer.h",
             "            if (!all[i].fresh && !all[i - 1].fresh) continue;       // (not ours: the set's own business)",
             "            continue;"),
            ("WG editor: while analysing, an edit outside the analysed stretches is applied", "app/TrPlayer.h",
             "            if (!S.complete) {\n                bool inside = false;",
             "            if (false) {\n                bool inside = false;"),
            ("WG editor: idx = the size again (a deletion makes the next cue collide)", "app/TrPlayer.h",
             "        k.idx = S.nextIdx++;", "        k.idx = static_cast<uint32_t>(S.cues.size()); S.nextIdx++;"),
            ("WG editor: the record forgets it was edited", "app/TrPlayer.h",
             "        r.edited = S.edited;", "        r.edited = false;"),
            ("WG editor: a passthrough's two texts drift apart", "app/TrPlayer.h",
             "            if (c.pass) c.dst = c.src;", "            (void)0;"),
            ("WG editor: an edit in the LAST report (not vouched yet) applied -- the record would not hold it", "app/TrPlayer.h",
             "        if (!S.complete) zone = merged(S.confirmed);", "        if (!S.complete) zone = merged(S.confirmed, S.ranges);"),
            ("WG editor: a line taken out where the record does not hold it (the resume brings it back)", "app/TrPlayer.h",
             "                if (rm.count(c.idx) && !pbtrf::inRanges(zone, c.startMs)) return notYet(c.startMs);",
             "                if (false) return notYet(c.startMs);"),
            ("WG end: the tray's exit leaves the pending files and edit unwritten", "app/TrPlayer.h",
             "    void shutdown() {\n        flushFiles();", "    void shutdown() {"),
            ("WG end: the session's end leaves the pending files unwritten", "app/TrPlayer.h",
             "    void persist() {\n        flushFiles();", "    void persist() {"),
            # ENGINE (the coordinator, 11/10)
            ("ENGINE: an older engine's record LOADED (never analysed again)", "app/TrPlayer.h",
             "        if (pbtrf::staleRecord(r.engine, r.edited)) { S.upgrade = true; return false; }",
             "        if (false) { S.upgrade = true; return false; }"),
            ("ENGINE: an edited older set saved as THIS engine's (the question vanishes)", "app/TrPlayer.h",
             "        r.engine = S.engine;", "        r.engine = tp::kFileEngineVersion;"),
            ("ENGINE: the shared original taken from an older engine's analysis", "app/TrPlayer.h",
             "            if (pbtrf::staleRecord(sm.engine, sm.edited)) continue;", "            (void)0;"),
            ("ENGINE: a queued file's older record badged ready (never pre-analysed)", "app/TrPlayer.h",
             "        else if (pbtrf::staleRecord(s.engine, s.edited)) s = pbtrf::Summary{};", ""),
            ("ENGINE: his confirmed re-analysis not said as an upgrade", "app/TrPlayer.h",
             "            set->upgrade = keep && keep->key == set->key && pbtrf::olderEngine(keep->engine);",
             "            set->upgrade = false;"),
            ("ENGINE: the window never told the cues are an older engine's", "app/TrPlayer.h",
             '        j.kv(L"oldEngine", S ? pbtrf::olderEngine(S->engine) : false);', '        j.kv(L"oldEngine", false);'),
            ("ENGINE: a set loaded from an older record keeps THIS engine's number", "app/TrPlayer.h",
             "        S.engine = r.engine;\n", ""),
            # review of WG #4 / #15
            ("WG #4: the shared-language file ranks by coverage only (an unedited pair reverts his fixes)", "app/TrPlayer.h",
             "        return std::make_tuple(complete ? 1 : 0, edited ? 1 : 0, coverage);",
             "        return std::make_tuple(complete ? 1 : 0, 0, coverage);"),
            ("WG #4: an edited HALF set beats a complete one (the file shrinks)", "app/TrPlayer.h",
             "        return std::make_tuple(complete ? 1 : 0, edited ? 1 : 0, coverage);",
             "        return std::make_tuple(edited ? 1 : 0, complete ? 1 : 0, coverage);"),
            ("WG #15: analyse() drops an edit younger than 2 s", "app/TrPlayer.h",
             "        // the set going away first writes an edit not saved yet (the 2 s rule had not come: review of WG #15)\n"
             "        if (s_.set && s_.set->editDirty) saveEdits(*s_.set);\n",
             "        // the set going away first writes an edit not saved yet (the 2 s rule had not come: review of WG #15)\n"),
            # N2's fix round (02/11, PROGRESS_N2.md): the core's L2 (no sound copy while mpv plays) and L6 (mpv's sound
            # tracks adopted) -- caught by the smoke's [N2] block (merged by the director)
            ('N2 L2: mapNow asks the copy while mpv plays', "app/TrPlayer.h",
             '            if (!host_.mpvPlays()) requestProxy();',
             '            requestProxy();'),
            ('N2 L2: a stale <video> failure asks the copy while mpv plays', "app/TrPlayer.h",
             '        if (host_.mpvPlays()) return;                  // 1.1.0 (L2)',
             '        // 1.1.0 (L2)'),
            ('N2 L2: mpvDropped asks nothing', "app/TrPlayer.h",
             '        if (s_.url.empty()) requestProxy();            // (asked once per item',
             '        // (asked once per item'),
            ('N2 L6: a MediaTracks list overridden', "app/TrPlayer.h",
             '        if (s_.tracksState == "ready" && s_.tracks && !s_.tracksFromMpv) return;   // a MediaTracks list: NEVER overridden',
             ''),
            ('N2 L6: adopted while the probe runs', "app/TrPlayer.h",
             '        if (s_.tracksState == "probing") return;                            // MediaTracks is reading it: its answer first',
             ''),
            ("N2 L6: mpv's own choice not waited for", "app/TrPlayer.h",
             '        if (first && !picked && !mpvChose && !s_.mpvFinal) return;',
             ''),
            ("N2 L6: the job does not wait for mpv's list", "app/TrPlayer.h",
             '        if (!worthy && !adoptedBefore && host_.mpvPlays()) s_.probeWait = true;',
             ''),
            ('N2 L6: an mpv list\'s ord 0 sent as "" (aid=auto)', "app/TrPlayer.h",
             'w.audio = set->audio.ord > 0 || (set->audio.trackId != 0 && mpvListed(set->mediaKey)) ? tp::packAudioSel(set->audio)',
             'w.audio = set->audio.ord > 0 ? tp::packAudioSel(set->audio)'),
            ('N2 L6: the opening\'s set not named (a FILE sent again: "")', "app/TrPlayer.h",
             '        if (a.ord == t.ord) { a.trackId = t.track; return; }',
             '        if (a.ord == t.ord) { return; }'),
            ("N2 L6: a job on aid=auto's late non-first choice kept under the first's key", "app/TrPlayer.h",
             '        if (decodes >= 0 && static_cast<uint32_t>(decodes) != a.ord) {',
             '        if (false) {'),
            ("N2 L6: mpv's first choice not remembered (a later opening plays D3's)", "app/TrPlayer.h",
             '            if (first && mpvChose && !picked) pe.autoOrd = s_.mpvOrd;',
             ''),
            ('N2 L6: the wait never released by an adopted list', "app/TrPlayer.h",
             "        if (!s_.tracksFromMpv && s_.mpvTracks && !s_.mpvFinal) return;   // a list, mpv's own choice still to come",
             "        if (s_.mpvTracks && !s_.mpvFinal) return;   // a list, mpv's own choice still to come"),
            ('N2 L6: an offer of another opening taken', "app/TrPlayer.h",
             '        if (!loaded_ || !seq || s_.seq != seq || !info) return;\n        s_.mpvOrd = mpvOrd;',
             '        if (!loaded_ || !seq || !info) return;\n        s_.mpvOrd = mpvOrd;'),
            ('N2 L6: mpvDropped does not release the wait', "app/TrPlayer.h",
             '        s_.mpvFinal = true;\n        mpvWaitDone();\n        if (s_.url.empty())',
             '        s_.mpvFinal = true;\n        if (s_.url.empty())'),
        ]),
    # WG: the new headers -- the editor's JSON reader, --play / WM_COPYDATA, the player registration (test key only)
    "wg": dict(
        smoke="tests/cpp/wg_smoke.cpp", sources=[],
        files=["app/JsonR.h", "app/TrLaunch.h", "app/TrAssoc.h", "app/TrFileStore.h", "common/TrPipe.h"],
        inc=["common"], libs=["user32.lib", "shell32.lib", "ole32.lib", "advapi32.lib"], helper=None,
        mutants=[
            ("JsonR: an escaped quote ENDS the string (a text with a quote misread)", "app/JsonR.h",
             "                case L'\"': out += L'\"'; break;", "                case L'\"': return true;"),
            ("JsonR: no depth cap (a hostile nesting recurses)", "app/JsonR.h",
             "if (depth > kMaxDepth || i_ >= s_.size() || ++values_ > kMaxValues) return false;",
             "if (i_ >= s_.size() || ++values_ > kMaxValues) return false;"),
            ("JsonR (review #17): no cap on the number of values", "app/JsonR.h",
             "if (depth > kMaxDepth || i_ >= s_.size() || ++values_ > kMaxValues) return false;",
             "if (depth > kMaxDepth || i_ >= s_.size()) return false;"),
            ("TrLaunch (review #16): a folder that cannot be opened counts as ANOTHER folder (isolation assumed)", "app/TrLaunch.h",
             "    if (!idOf(a, x) || !idOf(b, y)) return true;", "    if (!idOf(a, x) || !idOf(b, y)) return false;"),
            ("TrLaunch (review #16): the folders compared by volume only (any two on one disk are 'the same')", "app/TrLaunch.h",
             "    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh\n"
             "        && x.nFileIndexLow == y.nFileIndexLow;",
             "    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber;"),
            ("TrLaunch: a RELATIVE path accepted (played from wherever the process stands)", "app/TrLaunch.h",
             "    if (!drive && !unc) return Kind::Refused;\n", ""),
            ("TrLaunch: junk after the closing empty string accepted", "app/TrLaunch.h",
             "        if (e == at) return e == n - 1;", "        if (e == at) return true;"),
            ("TrAssoc: the file argument unquoted (a path with spaces split by Explorer)", "app/TrAssoc.h",
             "return L\"\\\"\" + exe + L\"\\\" --play \\\"%1\\\"\";", "return L\"\\\"\" + exe + L\"\\\" --play %1\";"),
            ("TrAssoc: unregister leaves our OpenWithProgids value behind", "app/TrAssoc.h",
             "            ::RegDeleteValueW(h, progIdFor(*p));", "            (void)h;"),
        ]),
    # WH (2026-10-11): the film's own tracks in the CORE -- the sound track (D3 D5 D6), the probe, embedded subtitles
    # (D7), the director's pre-WH rule for multi-track ISO-BMFF records. Same smoke as "player" (its section [17]).
    "playerwh": dict(
        smoke='tests/cpp/player_smoke.cpp', sources=[],
        files=['app/TrPlayer.h', 'app/TrWindowState.h', 'app/TrFileStore.h', 'app/JsonW.h', 'common/TrPipe.h', 'tests/web/we_vectors.txt', 'app/MediaTracks.h', 'app/SubtitleReader.h',
               'app/TrTranscript.h', 'app/SourceTimecode.h'],   # (U3: TrPlayer.h includes them)
        inc=['common'], libs=['user32.lib', 'shlwapi.lib', 'ole32.lib'], helper=None,
        copies=[('tests/web/we_vectors.txt', 'tests/cpp/we_vectors.txt')],
        mutants=[
            ("WH D3: the default flag ignored (Chromium's own rule: the first playable)", 'app/TrPlayer.h',
             '    for (const auto& a : t.audio) if (a.play && a.def) return static_cast<int>(a.ord);\n',
             ''),
            ('WH D3: an UNPLAYABLE default track chosen (DTS)', 'app/TrPlayer.h',
             'for (const auto& a : t.audio) if (a.play && a.def) return static_cast<int>(a.ord);',
             'for (const auto& a : t.audio) if (a.def) return static_cast<int>(a.ord);'),
            ('WH: the job does not wait for the probe (it starts on the first track, then re-tracks)', 'app/TrPlayer.h',
             '        if (s_.probeWait) return;',
             '        (void)0;'),
            ('WH: a probe that never answers holds the job for ever (no timeout)', 'app/TrPlayer.h',
             'if (loaded_ && s_.probeWait && now - s_.probeAt >= kProbeWaitMs) {',
             'if (false) {'),
            # (U2b re-point, 02/11/2026: fix round L6 added a track of a list MPV gave to startJob's line -- the only line of its text
            #  since U3 -- same intent: ord 0 always named)
            ("WH: FILE's audio not empty for ord 0 (the wire before WH changed)", 'app/TrPlayer.h',
             'w.audio = set->audio.ord > 0 || (set->audio.trackId != 0 && mpvListed(set->mediaKey)) ? tp::packAudioSel(set->audio)',
             'w.audio = true ? tp::packAudioSel(set->audio)'),
            ("WH D6: ord 0's cache key changed too (every record before WH lost)", 'app/TrFileStore.h',
             'if (audioOrd > 0) k += "\\ta" + std::to_string(audioOrd);',
             'k += "\\ta" + std::to_string(audioOrd);'),
            ("WH D5: a user's switch with translation ON does not re-track", 'app/TrPlayer.h',
             'if (s_.set && s_.set->audio.ord != t.ord && followsSound(heardSel())) retrack(heardSel());',   # (U1 re-point)
             'if (false) retrack(heardSel());'),
            ("WH D5: an AUTO choice persisted as the user's", 'app/TrPlayer.h',
             'if (!autoPick) {',
             'if (true) {'),
            # (U1 re-point: "translation back ON" is the press -- analyse() builds its set on the track HEARD)
            ('WH D5: Translate does not follow the track HEARD', 'app/TrPlayer.h',
             '        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, heardSel());   // WH: the track HEARD now',
             '        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, s_.set ? s_.set->audio : heardSel());'),
            ("WH: the probe's choice not applied when it differs from the provisional track", 'app/TrPlayer.h',
             'if (s_.set && (want.ord != s_.set->audio.ord || unsure)) { retrack(want, unsure); return; }',
             'if (false) { retrack(want, unsure); return; }'),
            ('WH D6: the record that wrote the file is no candidate (a track switch SHRINKS the file)', 'app/TrPlayer.h',
             'if (writerExtra) keys.push_back(writer);',
             '(void)writerExtra;'),
            ('WH D6: the registry is not told which record wrote the file', 'app/TrPlayer.h',
             'fromSib ? sibKey : S.key',
             'std::string()'),
            ('WH D7: a new extraction does not cancel the one running', 'app/TrPlayer.h',
             '        cancelSubExtract();                                     // a new request supersedes',
             '        (void)0;                                     // a new request supersedes'),
            ('WH D7: no LRU (an extraction never kept)', 'app/TrPlayer.h',
             'subLru_.emplace_front(key, r);',
             '(void)key;'),
            ('WH D7: an image / unreadable track extracted anyway', 'app/TrPlayer.h',
             'if (!st->ok) {',
             'if (false) {'),
            ('WH D7: playerSubs of an embedded track without embedded:true', 'app/TrPlayer.h',
             '            j.kv(L"embedded", true);\n',
             ''),
            ('WH (director): a pre-WH record of a 2-track MP4 trusted', 'app/TrPlayer.h',
             '        if (pbtrf::unknownTrackMiss(r.trackKnown, r.edited, multiTrackIsoBmff(S.mediaKey))) return false;\n',
             ''),
            ('WH (director): an EDITED pre-WH record discarded', 'app/TrFileStore.h',
             'return !trackKnown && !edited && multiTrackIsoBmff;',
             'return !trackKnown && multiTrackIsoBmff;'),
            ("WH (director): an unsure record stamped with the first track's marker when saved back", 'app/TrPlayer.h',
             'r.trackKnown = S.trackKnown;',
             'r.trackKnown = true;'),
            ("WH (director): a distrusted pre-WH set re-tracked through the cancelled job's set (its cues come back)", "app/TrPlayer.h",
             "        else if (fresh) { /* nothing: a new analysis */ }", "        else if (false) { /* nothing: a new analysis */ }"),
            ('WHP-1: playerSubGet{id:0} does not cancel the pending extraction', 'app/TrPlayer.h',
             '    void cancelSubRequest() { cancelSubExtract(); }',
             '    void cancelSubRequest() {}'),
            ('WHP-2: a proxied item keeps its non-first track (the proxy plays the first)', 'app/TrPlayer.h',
             '        host_.sendProxy(w);\n        stateDirty_ = true;\n        proxyToFirstTrack();\n',
             '        host_.sendProxy(w);\n        stateDirty_ = true;\n'),
            ('WHP-2: another track accepted on a proxied item', 'app/TrPlayer.h',
             '        if (proxied() && ord != 0) return;                     // (WHP-2: the proxy carries the first track only)\n',
             ''),
            ("WHP-2: a late probe's default beats the proxy", 'app/TrPlayer.h',
             '        if (o > 0 && proxied()) o = 0;                         // (WHP-2: played through the proxy = the first track)\n',
             ''),
            ('WH: a remembered pick analysed though the probe failed (the page plays the default track)', 'app/TrPlayer.h',
             '        return tp::AudioSel{};\n    }\n    static bool probeWorthy(',
             '        return s_.set ? s_.set->audio : tp::AudioSel{};\n    }\n    static bool probeWorthy('),
            ('WH (director): a 2-track MKV treated like an MP4 (its good records discarded)', 'app/TrPlayer.h',
             'return (i.container == "mp4" || i.container == "mov") && i.audio.size() >= 2;',
             'return i.audio.size() >= 2;'),
            # SuVtitle FIX ROUND (2026-10-23): RV4-1 = SPLIT_PLAN ADDENDUM 2, RV4-7
            ("RV4-1: the page's AUTOMATIC request honoured over the viewer's file", 'app/TrPlayer.h',
             '        if (isAuto) return !s_.userSub;\n', '        if (isAuto) return true;\n'),
            ("RV4-1: the viewer's own pick does not mark the item (the next automatic request replaces it)", 'app/TrPlayer.h',
             '        if (id) s_.userSub = true;\n', '        (void)id;\n'),
            ("RV4-1: a file the viewer names does not cancel the extraction running (it lands on top of the file)", 'app/TrPlayer.h',
             '        s_.userSub = true;\n        cancelSubExtract();\n', '        s_.userSub = true;\n'),
            ("RV4-7: topmost left out of the window's post", 'app/TrWindowState.h',
             '.kv(L"topmost", topmost)', ''),
        ]),
    # WH (2026-10-11): ui/MediaTracks.h -- the probe, the extraction, the inflate (tracks_smoke, its checks only)
    "tracks": dict(
        smoke='tests/cpp/tracks_smoke.cpp', sources=[], args=['--no-fuzz'],
        files=['app/MediaTracks.h', 'app/SubtitleReader.h'],
        inc=[], libs=['ole32.lib'], helper=None,
        mutants=[
            ('inflate: the Adler-32 check skipped (a corrupt stream accepted)', 'app/MediaTracks.h',
             '    if (static_cast<uint32_t>(be(t, 4)) != adler32(out)) { out.clear(); return false; }\n',
             ''),
            ('Cues: a cue point WITHOUT CueRelativePosition accepted (the unindexed block lost)', 'app/MediaTracks.h',
             'if (!hasCl || !hasRel) { all = false; continue; }',
             'if (!hasCl || !hasRel) { continue; }'),
            ('Cues: a position holding no block of the track accepted (a Void)', 'app/MediaTracks.h',
             '            if (ex.blocks == blocksBefore) return false;              // the group held no block of this track\n        } else {\n            return false;\n        }\n',
             '            if (ex.blocks == blocksBefore) return false;              // the group held no block of this track\n        }\n'),
            ('walk: in an unknown-size Cluster the next (unknown-size) Cluster read as damage', 'app/MediaTracks.h',
             'const bool read = mkv::readElem(r, at, lim, k, true);',
             'const bool read = mkv::readElem(r, at, lim, k, false);'),
            ('header stripping not undone', 'app/MediaTracks.h',
             'data.insert(0, e.settings);',
             '(void)0;'),
            ('a LACED subtitle block read as one frame', 'app/MediaTracks.h',
             'if ((fl >> 1) & 3) { ++ex.laced; return true; }',
             '(void)fl;'),
            ('no BlockDuration: the next block does not end the cue (10 s each)', 'app/MediaTracks.h',
             'e = (nx > s) ? std::min<int64_t>(nx, s + static_cast<int64_t>(kNoDurCapMs)) : s + static_cast<int64_t>(kNoDurCapMs);',
             'e = s + static_cast<int64_t>(kNoDurCapMs);'),
            ('D_WEBVTT: the id and settings lines kept as text', 'app/MediaTracks.h',
             'if (p2 != std::string::npos) text = text.substr(p2 + 1);',
             '(void)p2;'),
            ('tx3g: the styl box ignored (the italic lost)', 'app/MediaTracks.h',
             'const uint8_t face = e[6] & 7;',
             'const uint8_t face = 0;'),
            ("MP4: the edit list's empty edit ignored (the track 1.5 s early)", 'app/MediaTracks.h',
             'const double v = static_cast<double>(ticks - st.mediaTime) * 1000.0 / ts + static_cast<double>(st.emptyMs);',
             'const double v = static_cast<double>(ticks - st.mediaTime) * 1000.0 / ts;'),
            ("QuickTime: minf's DATA handler taken for the media's", 'app/MediaTracks.h',
             '} else if (pt == fcc("mdia") && y == fcc("hdlr")) {',
             '} else if (y == fcc("hdlr")) {'),
            ("ASS: the block's ReadOrder taken for its first Format column", 'app/MediaTracks.h',
             'for (size_t k = 0; k < nonTime.size(); ++k) byCol[nonTime[k]] = f[k + 1];',
             'for (size_t k = 0; k < nonTime.size(); ++k) byCol[nonTime[k]] = f[k];'),
            ('Matroska: an absent FlagDefault read as 0 (spec: 1)', 'app/MediaTracks.h',
             'bool def = true, enabled = true, forced = false;',
             'bool def = false, enabled = true, forced = false;'),
            ('Matroska: an absent Language read as und (spec: eng)', 'app/MediaTracks.h',
             'std::string codecId, name, lang = "eng", bcp47, priv;',
             'std::string codecId, name, lang = "und", bcp47, priv;'),
            # SuVtitle fix round (2026-10-23, REVIEW_RV1_MediaTracks.md)
            ('RV1-1: the Cues believed from ANY muxer (a partial index read as the whole track)', 'app/MediaTracks.h',
             'const bool tryCues = path == Path::Cues || (path == Path::Auto && cuesIndexEverySub(info.muxingApp, info.writingApp));',
             'const bool tryCues = path != Path::Walk;'),
            ('RV1-1: mkvmerge believed without its libebml/libmatroska MuxingApp', 'app/MediaTracks.h',
             '    return starts(writingApp, "mkvmerge")\n        && (muxingApp.find("libebml") != std::string::npos || muxingApp.find("libmatroska") != std::string::npos);',
             '    return starts(writingApp, "mkvmerge");'),
            ('RV1-2: the payload not carried out of the Cues path (reads 0)', 'app/MediaTracks.h',
             '            ex.payload = tmp.payload;                               // (RV1-2: it read 0 after the Cues path)\n',
             ''),
        ]),
    # WE (player v2): the subtitle READER (encoding detection + formats)
    "subreader": dict(
        smoke="tests/cpp/subreader_smoke.cpp", sources=[],
        files=["app/SubtitleReader.h", "tests/cpp/subreader_lits.inc"],
        inc=[], libs=["ole32.lib"], helper=None,
        mutants=[
            ("the GBK/Big5 tie-break never overrides MLang", "app/SubtitleReader.h",
             "if (guess && (chineseTop || (weakTop && cjkPlausible(s, guess))) && decodes(s, 0, guess)) {",
             "if (false) {"),
            ("UTF-16 without a BOM is not looked for (NUL is valid UTF-8)", "app/SubtitleReader.h",
             "if (const int u16 = utf16Guess(s, 0))", "if (const int u16 = 0)"),
            ("strict UTF-8 accepts the UTF-16 surrogates", "app/SubtitleReader.h",
             "else if (c == 0xED)              { need = 2; hi = 0x9F; }      // no UTF-16 surrogates",
             "else if (c == 0xED)              { need = 2; }      // no UTF-16 surrogates"),
            ("a digit opens a tag in an SRT too (\"I <3 you >\" eaten)", "app/SubtitleReader.h",
             "if (plausible && (stamp || first == '/'", "if (plausible && (stamp || (first >= '0' && first <= '9') || first == '/'"),
            ("WebVTT entities left undecoded", "app/SubtitleReader.h",
             "if (vtt && c == '&') { decodeEntity(text, i, cur); continue; }", "if (false && c == '&') { decodeEntity(text, i, cur); continue; }"),
            ("an ASS line's TEXT split at its commas", "app/SubtitleReader.h",
             "if (ln[k] == ',' && f.size() + 1 < ncol) { f.push_back(cur); cur.clear(); }",
             "if (ln[k] == ',') { f.push_back(cur); cur.clear(); }"),
            ("Cyrillic read as Chinese when MLang is away (plausibility dropped)", "app/SubtitleReader.h",
             "    return pairs >= 4 && frequent * 10 >= pairs * 6;", "    return pairs >= 4;"),
            # the adversarial review of WE (REVIEW_WE.md)
            ("C7: the tag scan forgets where the next closer is (quadratic again)", "app/SubtitleReader.h",
             "if (!known || from < from0 || (at != std::string::npos && from > at)) { at = s.find(ch, from); from0 = from; known = true; }",
             "{ at = s.find(ch, from); from0 = from; known = true; }"),
            ("P9: one stray byte sends a UTF-8 file to the fallback code page", "app/SubtitleReader.h",
             "if (mostlyUtf8(s, 0)) { said(\"utf8-lenient\"); return kCpUtf8; }", "(void)0;"),
            # review of WG #3: what a rewrite from the cues would lose
            ("WG #3: a <font> (any tag but i/b/u) not flagged as dropped", "app/SubtitleReader.h",
             "                    if (dropped && (!bit || tag.size() != name.size() + (closing ? 1u : 0u))) *dropped = true;",
             "                    (void)dropped;"),
            ("WG #3: a VTT NOTE/STYLE/REGION block not flagged", "app/SubtitleReader.h",
             "            r.dropped = true;                                              // a NOTE / STYLE / REGION block",
             "            (void)0;"),
            ("WG #3: VTT cue settings after the end time not flagged", "app/SubtitleReader.h",
             "            if (k < ln.size()) r.dropped = true;", "            (void)k;"),
        ]),
}

def copy_rel(rel, root):
    dst = os.path.join(root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.join(REPO, rel), dst)
    return dst

def run_target(name, only=None):
    t = TARGETS[name]
    results = []
    for (mname, rel, old, new) in t["mutants"]:
        if only and only not in mname:
            continue
        root = os.path.join(WORK, name, str(len(results)))
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(root)
        smoke = copy_rel(t["smoke"], root)
        for f in t["files"] + t["sources"]:
            copy_rel(f, root)
        for (src, dst) in t.get("copies", []):   # SuVtitle: a file the smoke reads next to itself
            os.makedirs(os.path.dirname(os.path.join(root, dst)), exist_ok=True)
            shutil.copy2(os.path.join(REPO, src), os.path.join(root, dst))
        target = os.path.join(root, rel)
        # line endings normalised (the repo files are CRLF; the compiler does not care), so a
        # mutation is written with plain "\n" whatever the checkout
        text = open(target, "rb").read().decode("utf-8").replace("\r\n", "\n")
        n = text.count(old)
        if n != 1:
            results.append((mname, False, "mutation applies %d times (must be exactly 1)" % n))
            continue
        open(target, "wb").write(text.replace(old, new).encode("utf-8"))
        out_dir = os.path.dirname(smoke)
        exe = os.path.join(out_dir, "mut_smoke.exe")
        obj = os.path.join(root, "obj") + os.sep
        os.makedirs(obj, exist_ok=True)
        cmd = (["cl"] + FLAGS + ["/I" + os.path.join(root, i) for i in t["inc"]]
               + [smoke] + [os.path.join(root, s) for s in t["sources"]]
               + ["/Fe:" + exe, "/Fo:" + obj])
        if t["libs"]:
            cmd += ["/link"] + t["libs"]
        c = subprocess.run(cmd, cwd=out_dir, capture_output=True, text=True, errors="replace")
        if c.returncode != 0 or not os.path.exists(exe):
            tail = (c.stdout + c.stderr).strip().splitlines()[-3:]
            results.append((mname, False, "the mutant does not compile: " + " / ".join(tail)))
            continue
        if t["helper"]:
            shutil.copy2(os.path.join(REPO, t["helper"]), os.path.join(out_dir, os.path.basename(t["helper"])))
        try:
            r = subprocess.run([exe] + t.get("args", []), cwd=out_dir, capture_output=True, text=True, errors="replace", timeout=180)
            caught = r.returncode != 0
            fails = [l.strip() for l in r.stdout.splitlines() if l.strip().startswith("FAIL")]
            results.append((mname, caught, ("caught by: " + fails[0]) if fails else ("exit %d" % r.returncode)))
        except subprocess.TimeoutExpired:
            results.append((mname, True, "caught: the smoke hung (timeout)"))
    return results

def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    only = sys.argv[2] if len(sys.argv) > 2 else None
    names = list(TARGETS) if which == "all" else [which]
    bad = 0
    for n in names:
        if n == "tracks" and not os.path.isdir(os.path.join(os.environ.get("SUVTITLE_TEST_MEDIA", ""), "wh", "media")):
            # tracks_smoke needs the WH data set (not in the repository) and, without it, exits 0 having checked nothing: every mutant
            # would "survive". Say so instead of failing -- or passing -- on nothing.
            print("  NOT CHECKED (set SUVTITLE_TEST_MEDIA)  [tracks] the tracks mutants run tracks_smoke, which needs <SUVTITLE_TEST_MEDIA>/wh")
            continue
        for (mname, caught, why) in run_target(n, only):
            print("  %s  [%s] MUTANT \"%s\" -- %s" % ("PASS" if caught else "FAIL", n, mname, why))
            bad += 0 if caught else 1
    print("CMUT %s: %d sentinel(s) failed" % ("OK" if bad == 0 else "FAILED", bad))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

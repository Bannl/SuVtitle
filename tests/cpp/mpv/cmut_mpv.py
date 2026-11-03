# cmut_mpv.py -- MUTATION SENTINELS of tests/cpp/mpv/mpv_smoke.cpp (N1, 2026-10-26): the native video engine's wrapper,
# app/MpvPlayer.cpp. The repo's cmut.py way (tests/cpp/cmut.py): the smoke and the product files it compiles are copied into
# a scratch tree with the SAME relative layout, ONE textual change is applied to ONE product file (it must apply exactly once,
# or the sentinel FAILS: a mutation that changes nothing would "test" the original), the copy is compiled with the smoke's own
# flags and run: CAUGHT = the smoke exits non-zero (1: a check failed; a hang is caught too). A mutant that does not compile,
# or a smoke that could not run at all (exit 2), is a FAILED sentinel. The repo files are never touched.
#
#   python -I cmut_mpv.py [name-filter]       (from cmut_mpv.cmd: cl on PATH, SUVTITLE_MPV_DIR set, the media made once by
#   python -I cmut_mpv.py --dry               build_mpv_smoke.cmd in work\media) -- --dry: does every mutant apply exactly once?
#
# The mutants are the wrapper's guards undone, the way a careless edit would; the text of each is the product's, verbatim.
# When a product line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_mpv")
FLAGS = ["/nologo", "/std:c++17", "/EHsc", "/W4", "/MD", "/O2", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
         "/DUNICODE", "/D_UNICODE", "/D_CRT_SECURE_NO_WARNINGS"]
SMOKE = "tests/cpp/mpv/mpv_smoke.cpp"
FILES = ["app/MpvPlayer.cpp", "app/MpvPlayer.h"]
P = "app/MpvPlayer.cpp"

MUTANTS = [
    # [PCM] what is heard
    ("PCM: the volume goes to mpv as it is (mpv's cubic curve: a slider at 50 % gives an eighth)", P,
     "return 100.0 * std::cbrt(p);", "return 100.0 * p;"),
    # [TRACK] the sound track by ord
    ("TRACK: open() passes aid=ord (one too low: the item starts on the wrong track, then switches)", P,
     'ord >= 0 ? fmt("aid=%d", ord + 1) : std::string("aid=auto");', 'ord >= 0 ? fmt("aid=%d", ord) : std::string("aid=auto");'),
    # [AID] (fix round HIGH1) -- the 26/10 code, verbatim: no aid at all when none is asked
    ("AID: no aid when none is asked (a switch made during an item carries over to the next one)", P,
     '    std::string opts = ord >= 0 ? fmt("aid=%d", ord + 1) : std::string("aid=auto");\n'
     '    if (startMs > 0) opts += fmt(",start=%.3f", (double)startMs / 1000.0);',
     '    std::string opts = ord >= 0 ? fmt("aid=%d", ord + 1) : std::string();\n'
     '    if (startMs > 0) opts += fmt("%sstart=%.3f", opts.empty() ? "" : ",", (double)startMs / 1000.0);'),
    # [TRACKS] (fix round: the contract's track list)
    ("TRACKS: the list read at file-loaded is not told (no Tracks before Loaded)", P,
     "if (atLoad || (loadedSeen && changed)) out.push_back(ev(MpvEvent::Kind::Tracks));",
     "if (loadedSeen && changed) out.push_back(ev(MpvEvent::Kind::Tracks));"),
    ("TRACKS: cover art counted as a picture (an mp3 with a cover becomes a video)", P,
     'if (type == "video" && !nodeFlag(mapGet(t, "albumart"))) picture = true;', 'if (type == "video") picture = true;'),
    ("SIZE: the picture's size reset at each start (mpv never says an unchanged size again: a next item of the same size 0 x 0)", P,
     "        st.hwdec.clear();\n        audioIds.clear();", "        st.w = st.h = 0;\n        st.hwdec.clear();\n        audioIds.clear();"),
    # [HOLD] (fix round MEDIUM3)
    ("HOLD: a seek asked while the item loads is sent at once (mpv refuses it: lost)", P,
     "            held = Act{1, ms, exact, 0};\n            return;\n", ""),
    ("HOLD: a frame step asked while the item loads is sent at once (lost)", P,
     "            held = Act{2, 0, true, step};\n            return;\n", ""),
    ("HOLD: a held seek is never done at file-loaded", P,
     "            else replayHeld();\n", ""),
    ("HOLD: a held frame step done AT file-loaded (mpv ignores a back step during its start seek: measured 02/11)", P,
     "            if (held.kind == 2) stepAtRestart = curGen;", "            if (false) stepAtRestart = curGen;"),
    ("HOLD: a held frame step never done (its first restart forgets it)", P,
     "            if (stepAtRestart && stepAtRestart == curGen) {", "            if (false) {"),
    ("HOLD: open() keeps what an OLDER item held (done on the new one)", P,
     "    I.loadingGen = gen;\n    I.held = Impl::Act{};\n", "    I.loadingGen = gen;\n"),
    ("HOLD: a command mpv refuses is not logged", P,
     "if (e.error < 0) logf(\"%s refused by mpv: %s (item %llu)\"", "if (false) logf(\"%s refused by mpv: %s (item %llu)\""),
    # (no sentinel on checkOrd's "nothing selected at the first restart" branch: this libmpv, given an aid the item lacks,
    # selects its default track by itself -- measured 26/10 -- so the branch is a guard for other builds the smoke cannot
    # reach; the smoke checks the outcome, "never silence", whoever provides it)
    # (no sentinel any more on checkOrd's "a list that says NONE selected yet is no mismatch" guard: it was caught on 26/10,
    # when the list at file-loaded was the observed property's and could still say "none selected". Since the fix round the
    # list is ASKED at file-loaded (mpv_get_property_async) and its answer -- after mpv's selection -- always names the track:
    # measured 02/11, pass 3, the mutant "if (true)" leaves the smoke green. The guard stays for other builds / timings; the
    # smoke's [LOG] still checks the outcome: no second selection of the track mpv already plays.)
    ("TRACK: the ord heard is the track's position + 1", P,
     "if (ids[i] == selected) ord = (int)i;", "if (ids[i] == selected) ord = (int)i + 1;"),
    ("TRACK: selectAudio(ord) selects the first track whatever the ord", P,
     "if (ord < (int)I.audioIds.size()) I.setInt(\"aid\", I.audioIds[(size_t)ord]);",
     "if (ord < (int)I.audioIds.size()) I.setInt(\"aid\", I.audioIds[0]);"),
    # [GEN] the item generations
    ("GEN: the new item's generation is taken at open() (the old item's last events carry it)", P,
     "I.lastOpenGen = gen;", "I.lastOpenGen = gen;\n    I.curGen = gen;"),
    ("GEN: open() keeps the old item's A-B loop (mpv's ab-loop outlives a file)", P,
     'I.setStr("ab-loop-b", "no");', "(void)0;"),
    # [OPEN] / [EOF] / [STOP] / [ERROR]
    ("OPEN: open() does not pause (the item plays by itself, before the page asks)", P,
     'I.setFlag("pause", true);', "(void)0;"),
    ("EOF: play() at the end does not start again from the beginning", P,
     "if (I.st.eof && !I.seekSinceEof && !I.loadingGen) I.seekTo(0, true);", "(void)0;"),
    # [EOF2] (page review L1, host half)
    ("EOF2: play() at the end restarts from 0 even over a seek asked since that end", P,
     "if (I.st.eof && !I.seekSinceEof && !I.loadingGen) I.seekTo(0, true);", "if (I.st.eof) I.seekTo(0, true);"),
    ("EOF2: a NEW end does not clear 'sought since' (play() never restarts after a seek, even at a later end)", P,
     "if (v) seekSinceEof = false;", "(void)v;"),
    ("STOP: stop() does nothing (mpv keeps the closed item)", P,
     'I.cmd({"stop"});', "(void)0;"),
    ("ERROR: a file mpv refuses says nothing (the page never falls back)", P,
     "if (f->reason == MPV_END_FILE_REASON_ERROR) {", "if (false) {"),
    # [SEEK] [SPEED] [AB] [PLAY]
    ("SEEK: an exact seek is a keyframe seek", P,
     'cmd({"seek", t.c_str(), exact ? "absolute+exact" : "absolute+keyframes"});', 'cmd({"seek", t.c_str(), "absolute+keyframes"});'),
    ("SPEED: the speed is never sent", P,
     'impl_->setDouble("speed", std::min(10.0, std::max(0.1, rate)));', "(void)rate;"),
    ("AB: setting B no longer jumps to A (B behind the playhead never loops: N0's bug)", P,
     "if (aMs >= 0 && bMs > aMs && bMs != I.lastAbB) I.askSeek(aMs, true);", "(void)aMs;"),
    ("HOLD: setAbLoop's jump to A sent at once while the item loads (lost)", P,
     "if (aMs >= 0 && bMs > aMs && bMs != I.lastAbB) I.askSeek(aMs, true);",
     "if (aMs >= 0 && bMs > aMs && bMs != I.lastAbB) I.seekTo(aMs, true);"),
    # [NET] (fix round HIGH2)
    ("NET: references followed (a local playlist / manifest / EDL / cue sheet makes mpv open the URLs in it)", P,
     'opt("access-references", "no");', "(void)0;"),
    # [DLL] (fix round MEDIUM4) -- the smoke's own process is locked as wWinMain locks the app's
    ("DLL: the search is never locked (the current directory stays on it)", P,
     "    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return false;\n", ""),
    ("DLL: <exe dir>\\mpv\\ is not added to the search", P,
     "    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ::AddDllDirectory(sub.c_str());\n", ""),
    # [LOAD] the Vulkan loader (N3's rule) and the log opened FIRST (LOW9)
    ("LOAD: the bundled Vulkan loader preferred over the PC's own (System32 not checked first)", P,
     "        return \"the system's (System32)\";\n", "        (void)0;\n"),
    ("LOAD: native.log opened only at attach (a missing / refused DLL is said nowhere)", P,
     "    I.openLog();                                       // (LOW9: FIRST", "    // (LOW9: FIRST"),
    # [LOGCAP] (LOW9)
    ("LOGCAP: native.log's size checked only when it opens (a long session grows it without end)", P,
     "if (logBytes + line.size() > kLogCapBytes) {", "if (false) {"),
    # [EARLY] (LOW8) -- the 26/10 behaviour: the first open() waited for the GPU probe on the UI thread
    ("PROBE: the first open() waits for the GPU probe (the UI thread blocked)", P,
     "    // (LOW8: no wait for the GPU probe here -- its answer applies profile=fast whenever it comes)\n",
     "    if (I.probe) { ::WaitForSingleObject(I.probe, 3000); I.onProbe(); }\n"),
    ("PLAY: Time events are not coalesced (one per frame: 60 Hz)", P,
     "if (now - lastTimeEmit >= kTimeEveryMs) flushTime(out);", "if (true) flushTime(out);"),
    ("VOL: the Volume event does not say muted", P,
     "e.flag = st.muted;", "e.flag = false;"),
    # [PLACE]
    ("PLACE: the CSS rectangle's left edge is not scaled (a 150 % screen misplaces the picture)", P,
     "(LONG)std::lround(cx * scale)", "(LONG)std::lround(cx)"),
    ("PLACE: full screen does not fill the client before the page says so", P,
     "if (I.fs && I.fillPending) I.fill();", "if (false) I.fill();"),
    ("PLACE: out of full screen the full-screen rectangle stays (the windowed one not restored)", P,
     "if (I.savedValid) {", "if (false) {"),
    ("PLACE: a new DPI scale waits for the page's next rectangle", P,
     "if (!I.fillPending) I.applyCss();", "(void)0;"),
    # [WARP] [OPEN] the GPU probe
    ("WARP: forced software rendering is not told (state().warp)", P,
     "        I.st.warp = true;\n", ""),
    ("OPEN: the GPU probe calls every adapter software (WARP + profile=fast on a real GPU)", P,
     "const bool basic = d.VendorId == 0x1414 && d.DeviceId == 0x8c;", "const bool basic = true;"),
    # [END] the termination: NO sentinel on its message pump. Measured 26/10 (pass 2): with a plain blocking wait instead,
    # mpv_terminate_destroy did NOT deadlock while a picture played (mpv's window went without sending to our thread), so the
    # deadlock N0 guarded against is not reproduced with this libmpv -- the pump (sent messages only) stays as a guard for
    # other builds and drivers; the smoke checks the outcome (shutdown returns in < 3 s while playing and while loading).
]

def copy_rel(rel, root):
    dst = os.path.join(root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.join(REPO, rel), dst)
    return dst

def link_tree(src, dst):
    os.makedirs(dst, exist_ok=True)
    for n in os.listdir(src):
        s, d = os.path.join(src, n), os.path.join(dst, n)
        if os.path.isfile(s) and not os.path.exists(d):
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)

def main():
    dry = "--dry" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--dry"]
    only = args[0] if args else None
    mpv = os.environ.get("SUVTITLE_MPV_DIR", "")
    media = os.path.join(HERE, "work", "media")
    if not dry and (not os.path.isfile(os.path.join(mpv, "include", "mpv", "client.h"))
                    or not os.path.isfile(os.path.join(media, "main60.mkv"))):
        print("cmut_mpv: NOT RUN -- SUVTITLE_MPV_DIR (libmpv-2.dll + include\\mpv\\client.h) and work\\media (build_mpv_smoke.cmd) are needed")
        sys.exit(2)
    bad = 0
    for n, (name, rel, old, new) in enumerate(MUTANTS):
        if only and only not in name:
            continue
        text = open(os.path.join(REPO, rel), "rb").read().decode("utf-8").replace("\r\n", "\n")
        k = text.count(old)
        if k != 1:
            print('  FAIL  [mpv] MUTANT "%s" -- applies %d times (must be exactly 1)' % (name, k))
            bad += 1
            continue
        if dry:
            print('  ok    [mpv] MUTANT "%s" applies once' % name)
            continue
        root = os.path.join(WORK, str(n))
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(root)
        smoke = copy_rel(SMOKE, root)
        for f in FILES:
            copy_rel(f, root)
        target = os.path.join(root, rel)
        open(target, "wb").write(text.replace(old, new).encode("utf-8"))
        out = os.path.dirname(smoke)
        exe, obj = os.path.join(out, "mpv_smoke.exe"), os.path.join(root, "obj") + os.sep
        os.makedirs(obj, exist_ok=True)
        c = subprocess.run(["cl"] + FLAGS + ["/I" + os.path.join(mpv, "include"), smoke, "/Fe:" + exe, "/Fo:" + obj,
                            "/link", "user32.lib", "gdi32.lib", "ws2_32.lib"], cwd=out, capture_output=True, text=True, errors="replace")
        if c.returncode != 0 or not os.path.exists(exe):
            tail = (c.stdout + c.stderr).strip().splitlines()[-3:]
            print('  FAIL  [mpv] MUTANT "%s" -- the mutant does not compile: %s' % (name, " / ".join(tail)))
            bad += 1
            continue
        link_tree(media, os.path.join(out, "media"))
        try:
            r = subprocess.run([exe], cwd=out, capture_output=True, text=True, errors="replace", timeout=240)
            fails = [l.strip() for l in r.stdout.splitlines() if l.strip().startswith("FAIL")]
            caught = r.returncode == 1
            why = ("caught by: " + fails[0]) if fails else ("exit %d" % r.returncode)
            if r.returncode == 2:
                why = "the smoke could not run: " + r.stdout.strip().splitlines()[-1:][0] if r.stdout.strip() else "exit 2"
        except subprocess.TimeoutExpired:
            caught, why = True, "caught: the smoke hung (timeout)"
        print('  %s  [mpv] MUTANT "%s" -- %s' % ("PASS" if caught else "FAIL", name, why))
        sys.stdout.flush()
        bad += 0 if caught else 1
    print("CMUT_MPV %s: %d sentinel(s) failed" % ("OK" if bad == 0 else "FAILED", bad))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

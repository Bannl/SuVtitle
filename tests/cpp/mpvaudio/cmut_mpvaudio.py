# cmut_mpvaudio.py -- MUTATION SENTINELS of tests/cpp/mpvaudio/mpvaudio_smoke.cpp (N3, 2026-10-26): the analysis' sound through
# libmpv (engine/TrMpvAudio.cpp, the libmpv path of engine/TrMedia.cpp). The repo's cmut.py way (tests/cpp/cmut.py, cmut_mpv.py):
# the smoke and the product files it compiles are copied into a scratch tree with the SAME relative layout, ONE textual change is
# applied to ONE product file (it must apply exactly once, or the sentinel FAILS: a mutation that changes nothing would "test"
# the original), the copy is compiled with the smoke's own flags and run: CAUGHT = the smoke exits non-zero and not 2 (1: a
# check failed; a crash or a hang is caught too). A mutant that does not compile, or a smoke that could not run (exit 2), is a
# FAILED sentinel. The repo files are never touched.
#
#   python -I cmut_mpvaudio.py [name-filter]   (from cmut_mpvaudio.cmd: cl on PATH, SUVTITLE_MPV_DIR set, SUVTITLE_VULKAN_RT for
#   python -I cmut_mpvaudio.py --dry           the loader sentinels, the media made once by build_mpvaudio_smoke.cmd)
#
# The mutants are the guards undone, the way a careless edit would; the text of each is the product's, verbatim. When a product
# line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
# NOT sentinels (measured equivalent on this libmpv, kept as guards -- pass 1, 26/10, docs PROGRESS_N3):
#  * the re-open when mpv's id for the ord-th track is not ord + 1 (mpv always numbers them so: the guard never fires);
#  * open()'s own "nothing to play after the restart = the end" (phase A): the pipe connects BEFORE the restart (measured,
#    +3.5 ms), so a start in the sound's silent tail always reaches the running session, whose end-file decides; pass 1 also
#    found that the running session did not even READ that end-file (the pipe breaks a moment before it is queued): FIXED,
#    writerClosed() waits for it, and the sentinel "SEEK: 'nothing to play' ... is an error" now watches it. Its wait itself
#    is a guard for a real mid-stream error (end-file "error" after sound), which no clip here produces;
#  * "a TRACK-level refusal is final": with the fallback to Media Foundation forced, Media Foundation refuses the same track
#    and libmpv's sentence wins anyway (the rule when both fail) -- a file where Media Foundation would decode a track libmpv
#    refuses could not be built, so mutant "TRACK: a refused track falls back to Media Foundation" survived.
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_mpvaudio")
FLAGS = ["/nologo", "/std:c++17", "/EHsc", "/W4", "/MT", "/O2", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
         "/DUNICODE", "/D_UNICODE", "/D_CRT_SECURE_NO_WARNINGS"]
LIBS = ["mfplat.lib", "mfreadwrite.lib", "mfuuid.lib", "ole32.lib", "wmcodecdspuuid.lib", "ws2_32.lib", "user32.lib"]
SMOKE = "tests/cpp/mpvaudio/mpvaudio_smoke.cpp"
FILES = ["engine/TrMpvAudio.cpp", "engine/TrMpvAudio.h", "engine/TrMedia.cpp", "engine/TrMedia.h"]
A = "engine/TrMpvAudio.cpp"
M = "engine/TrMedia.cpp"

MUTANTS = [
    # [NET] review RV-NH 2
    ("NET: no access-references=no (a playlist / HLS manifest in a local file is followed)", A,
     'opt("access-references", "no");', "(void)0;"),
    # [TIME] the player's timeline
    ("TIME: the session's first sample is taken to lie at the start asked (the late sound's lead lost)", A,
     "d.f.firstPts = pts;", "d.f.firstPts = startSec;"),
    ("TIME: the reader ignores where the first sample lies (no lead, no trim)", M,
     "const int64_t first = (int64_t)std::llround(fx.firstPts * (double)kRate16k);", "const int64_t first = (int64_t)target;"),
    ("TIME: the session starts PLAYING (the restart's time-pos read late; blocking calls while mpv writes)", A,
     'opt("pause", "yes");', "(void)0;"),
    # [FMT] (02/11)
    ("FMT: the chain's output is not pinned (a decoded-format change re-creates mpv's output: the session dies)", A,
     '"aformat=sample_fmts=flt:channel_layouts=mono]";', '"anull]";'),
    # [GAP] (02/11)
    ("GAP: no silence for a timestamp gap (the cues after it a gap's length early)", A,
     '"aresample=16000:async=1,"', '"aresample=16000,"'),
    ("GAP: the stamps are not made safe (a timestamp reset makes aresample DROP the sound after it)", A,
     '            "asetpts=\'st(1\\\\,if(eq(N\\\\,0)\\\\,PTS\\\\,if(lt(PTS\\\\,PREV_OUTPTS+ld(0)/SR/TB-0.1/TB)"\n'
     '            "+gt(PTS\\\\,PREV_OUTPTS+ld(0)/SR/TB+30/TB)\\\\,PREV_OUTPTS+ld(0)/SR/TB\\\\,PTS)));st(0\\\\,NB_SAMPLES);ld(1)\',"\n',
     '            "anull,"\n'),
    ("GAP: a clock reset of minutes is filled with silence (the 30 s cap gone)", A,
     "PREV_OUTPTS+ld(0)/SR/TB+30/TB", "PREV_OUTPTS+ld(0)/SR/TB+3000/TB"),
    # [SEEK]
    ("SEEK: the landing / warm-up before the target is not dropped (the stream 1 s early)", M,
     "else mpTrim = (uint64_t)((int64_t)target - first);", "else mpTrim = 0;"),
    ("SEEK: 'nothing to play' after a start in the sound's silent tail is an error, not the end", A,
     "if (ef && ef->reason == MPV_END_FILE_REASON_ERROR && ef->error != MPV_ERROR_NOTHING_TO_PLAY)",
     "if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)"),
    # [TRACK]
    ("TRACK: mpv's own pick is reported as the first track", A,
     "if (aud[k].first == selected) pick = (int)k;", "if (aud[k].first == selected) pick = 0;"),
    ("TRACK: mpv's own pick is not said (trackHow 'mpv' for no pick)", A,
     "f.defaultPick = true;", "(void)0;"),
    ("TRACK: an ord the file lacks is not refused (mpv then plays its own default instead: measured)", A,
     '            err = fmt("The chosen sound track (number %d) is not in this file.", ord + 1);\n            ok = false;',
     "            (void)0;"),
    # [INFO]
    ("INFO: a moving picture is not seen", A,
     'if (!nodeFlag(mapGet(t, "albumart")) && !nodeFlag(mapGet(t, "image"))) f.hasVideo = true;', "(void)t;"),
    ("INFO: the duration is not read", A,
     'f.durationSec = getDouble("duration", dur) && dur > 0.0 ? dur : 0.0;', "f.durationSec = 0.0;"),
    # [ERR]
    ("ERR: libmpv's sentence is replaced by Media Foundation's when neither reads the file", M,
     "        if (!mpvErr.empty()) err = mpvErr;\n        return false;", "        return false;"),
    ("ERR: the pipe's client must NOT be this process (every session refused)", A,
     "if (!::GetNamedPipeClientProcessId(pipe, &pid) || pid != ::GetCurrentProcessId()) {",
     "if (!::GetNamedPipeClientProcessId(pipe, &pid) || pid == ::GetCurrentProcessId()) {"),
    # [CHILD vkfb] the Vulkan loader
    ("VKFB: the bundled loader is loaded even where System32 has one", A,
     'if (systemVulkanLoaderPresent()) return "the system\'s Vulkan loader (System32)";', "(void)0;"),
    ("VKFB: System32's loader is never seen", A,
     'return fileExists(std::wstring(sys, n) + L"\\\\vulkan-1.dll");', "return false;"),
    ("ENV: the children's PATH is not extended", A,
     "            v += fb;\n", "            (void)fb;\n"),
    # [CHILD dll] review RV-NH 4
    ("DLL: the current directory stays on the DLL search path", A,
     "    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);\n", ""),
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
                    or not os.path.isfile(os.path.join(media, "late_mkv.mkv"))):
        print("cmut_mpvaudio: NOT RUN -- SUVTITLE_MPV_DIR (libmpv-2.dll + include\\mpv\\client.h) and work\\media "
              "(build_mpvaudio_smoke.cmd) are needed")
        sys.exit(2)
    bad = 0
    for n, (name, rel, old, new) in enumerate(MUTANTS):
        if only and only not in name:
            continue
        text = open(os.path.join(REPO, rel), "rb").read().decode("ascii").replace("\r\n", "\n")
        k = text.count(old)
        if k != 1:
            print('  FAIL  [mpvaudio] MUTANT "%s" -- applies %d times (must be exactly 1)' % (name, k))
            bad += 1
            continue
        if dry:
            print('  ok    [mpvaudio] MUTANT "%s" applies once' % name)
            continue
        root = os.path.join(WORK, str(n))
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(root)
        smoke = copy_rel(SMOKE, root)
        for f in FILES:
            copy_rel(f, root)
        open(os.path.join(root, rel), "wb").write(text.replace(old, new).encode("ascii"))
        out = os.path.dirname(smoke)
        exe, obj = os.path.join(out, "mpvaudio_smoke.exe"), os.path.join(root, "obj") + os.sep
        os.makedirs(obj, exist_ok=True)
        c = subprocess.run(["cl"] + FLAGS + ["/I" + os.path.join(mpv, "include"), "/I" + os.path.join(root, "engine"), smoke,
                            os.path.join(root, "engine", "TrMedia.cpp"), "/Fe:" + exe, "/Fo:" + obj, "/link"] + LIBS,
                           cwd=out, capture_output=True, text=True, errors="replace")
        if c.returncode != 0 or not os.path.exists(exe):
            tail = (c.stdout + c.stderr).strip().splitlines()[-3:]
            print('  FAIL  [mpvaudio] MUTANT "%s" -- the mutant does not compile: %s' % (name, " / ".join(tail)))
            bad += 1
            continue
        link_tree(media, os.path.join(out, "media"))
        try:
            r = subprocess.run([exe], cwd=out, capture_output=True, text=True, errors="replace", timeout=300)
            fails = [l.strip() for l in r.stdout.splitlines() if l.strip().startswith("FAIL")]
            caught = r.returncode not in (0, 2)
            why = ("caught by: " + fails[0]) if fails else ("exit %d" % r.returncode)
            if r.returncode == 2:
                why = "the smoke could not run: " + (r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "exit 2")
        except subprocess.TimeoutExpired:
            caught, why = True, "caught: the smoke hung (timeout)"
        print('  %s  [mpvaudio] MUTANT "%s" -- %s' % ("PASS" if caught else "FAIL", name, why))
        sys.stdout.flush()
        bad += 0 if caught else 1
    print("CMUT_MPVAUDIO %s: %d sentinel(s) failed" % ("OK" if bad == 0 else "FAILED", bad))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

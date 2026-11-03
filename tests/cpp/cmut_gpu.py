# cmut_gpu.py -- MUTATION SENTINELS for the GPU wave (SuVtitle 1.1.0, wave GPU "the (GPU) label stops lying", 2026-11-02,
# docs/PROGRESS_GPU.md): the start's GPU probe (engine/main_translator.cpp, its GPU probe section).
# ENGINE mutants, the U3 way (PROGRESS_U3 B9): each = ONE change (one or more exact-once edits) to a COPY of
# engine/main_translator.cpp, compiled with the tree's build\x64-Debug flags (read from its build.ninja) and linked with the
# tree's OTHER engine objects (only read: run tools\buildlock.cmd x64-Debug --target suvtitle_engine first), then the copy's
# `--selftest-gpuprobe` runs in an isolated profile: CAUGHT = it is not PASSED. An unmutated copy is built and run the same way
# FIRST (the control: it must pass, or nothing below proves anything). A mutant that does not apply exactly once, does not
# build, or is REFUSED by the selftest's trap guard is a FAILED sentinel (it proves nothing).
#
#   py -3 -I cmut_gpu.py [name-filter]      cl / link: tools\vsenv.cmd is loaded when cl is not on PATH
#   py -3 -I cmut_gpu.py --dry              every mutant applies exactly once? (no compile)
#
# NEEDS the payload (SUVTITLE_PAYLOAD -- the selftest's [4] [5] run the REAL llama-server; without it they are NOT CHECKED and
# the runHost mutants would survive: refused here) and must run WITHOUT --cpu / AUDIOPB_TR_FORCE_CPU / VK_LOADER_DRIVERS_DISABLE /
# GGML_DISABLE_VULKAN (the selftest refuses them; they are removed from its environment here). SUVTITLE_TEST_GPU=listed|none is
# passed through (the author's PC: listed). Each run opens the GPU for ~0.2 s twice ([4] as it is, [5] (b)): the brief's gate
# runs before each -- typeperf, the 3D engine's load summed over every process EXCEPT AstraMinimalist.exe (the user's own live
# wallpaper, always on: the director's ruling, 02/11), busy above 20 % = wait 30 s and sample again (logged).
# Scratch: SUVTITLE_CMUT_WORK, else %TEMP%\suvtitle_cmut_gpu. Measured 02/11: ~25 s a mutant (build ~10 s, run ~14 s), G11
# ~2.5 min, ~9 min in all.
# When a product line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
import os, re, shutil, subprocess, sys, tempfile, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_gpu")
ENGINE = os.path.join(REPO, "engine", "main_translator.cpp")
BUILD = os.path.join(REPO, "build", "x64-Debug")

PROBE_CALL = "    probeGpuAtStart(paths.llamaExe);\n"
THREAD = "    std::thread pipeline(pipelineMain, std::ref(sh), std::ref(audio), std::ref(hub), std::ref(srv), cfg, !nofabric);\n"
HELLO = r'    trpipe::writeMessage(pipe, "HELLO\tengine");' + "\n"
FORCE = "        if (p.verdict == GpuProbe::kNone) forceCpuChildren();\n"

# (name, [(old, new), ...]) -- each `old` must occur EXACTLY ONCE in the text as the edits before it left it
MUTANTS = [
    ("G1 \"(none)\" read as NONE from a NON-ZERO exit (a crashed or failing llama-server forces the CPU)", [
        ('    if (exitCode != 0) { why = "exit code " + std::to_string((unsigned long)exitCode); return GpuProbe::kUnknown; }\n', ""),
    ]),
    ("G2 a header WITHOUT \"(none)\" under it read as NONE", [
        (r'    if (block.empty()) { why = "\"Available devices:\" with no line under it"; return GpuProbe::kUnknown; }',
         "    if (block.empty()) return GpuProbe::kNone;"),
    ]),
    ("G3 \"(none)\" with a device line read as NONE", [
        ('    if (block.size() == 1 && block[0] == "(none)") return GpuProbe::kNone;',
         '    if (block[0] == "(none)") return GpuProbe::kNone;'),
    ]),
    ("G4 NONE not forcing the CPU (the label lies again: STATE gpu 1, auto times turbo on the CPU)", [
        (FORCE, ""),
    ]),
    ("G5 the probe run under g_forceCpu (DEVICE cpu / --cpu: the GPU opened anyway)", [
        ('    if (g_forceCpu) {\n        p.why = "not run";', '    if (false) {\n        p.why = "not run";'),
    ]),
    ("G6 DEVICE cpu WRITTEN to engine.txt as well (a driver installed later never used again)", [
        (FORCE, '        if (p.verdict == GpuProbe::kNone) { TrConfig d; d.load(); d.device = "cpu"; d.save(); forceCpuChildren(); }\n'),
    ]),
    ("G7 DEVICE cpu written to engine.txt INSTEAD of the in-memory switch (cfg.device = cpu)", [
        (FORCE, '        if (p.verdict == GpuProbe::kNone) { TrConfig d; d.load(); d.device = "cpu"; d.save(); }\n'),
    ]),
    ("G8 the probe AFTER the pipeline thread starts (its first STATE announces gpu 1)", [
        (PROBE_CALL + THREAD, THREAD + PROBE_CALL),
    ]),
    ("G9 the probe BEFORE the HELLO (a slow driver vs the app's 8 s connect leash: a restart loop)", [
        (PROBE_CALL + THREAD, THREAD),
        (HELLO, PROBE_CALL + HELLO),
    ]),
    ("G10 the bound NOT killing the child (a hung llama-server left behind until the job's close)", [
        ("        ::TerminateProcess(pi.hProcess, kGpuProbeKilledExit);\n", ""),
    ]),
    ("G11 no bound at all on the read (a hung llama-server holds the engine's start) -- this run takes ~2.5 min", [
        ("        if (::GetTickCount64() >= deadline) break;\n        DWORD avail = 0;\n", "        DWORD avail = 0;\n"),
    ]),
    ("G12 stderr NOT on the probe's pipe (stdout alone)", [
        ("    si.StartupInfo.hStdError = out;\n", ""),
    ]),
    ("G13 the read stops at the kept 64 KB (a talkative child blocks on a full pipe, killed: no answer)", [
        ("                p.out.append(buf, std::min<size_t>(got, kGpuProbeKeepBytes - p.out.size()));\n            continue;\n",
         "                p.out.append(buf, std::min<size_t>(got, kGpuProbeKeepBytes - p.out.size()));\n"
         "            if (p.out.size() >= kGpuProbeKeepBytes) break;\n            continue;\n"),
    ]),
    # (NOT a sentinel, measured 02/11: "the bound looked at only when the pipe is empty" -- the loop as it was before the fix of
    #  PROGRESS_GPU S8 -- SURVIVES: one writer cannot keep the pipe full, the reader drains it between two writes and looks at its
    #  bound then: the `endless` stand-in was killed at 1641 ms for a 1500 ms bound, not missed. A check tighter than that would be
    #  a timing test. The fix stays: it costs nothing, and the bound no longer depends on a writer's pauses.)
]


def source():
    return open(ENGINE, "rb").read().decode("utf-8").replace("\r\n", "\n")


def apply(text, edits):
    for (old, new) in edits:
        n = text.count(old)
        if n != 1:
            return None, "an edit applies %d time(s) (must be exactly 1): %r" % (n, old[:80])
        text = text.replace(old, new)
    return text, ""


def dry():
    bad = 0
    text = source()
    for (name, edits) in MUTANTS:
        _, why = apply(text, edits)
        print("  %s  %s%s" % ("ok  " if not why else "FAIL", name, "" if not why else " -- " + why))
        bad += 1 if why else 0
    print("CMUT_GPU --dry: %d mutant(s) do not apply exactly once" % bad)
    return 1 if bad else 0


def vs_environment():
    if shutil.which("cl") and shutil.which("link"):
        return
    vsenv = os.path.join(REPO, "tools", "vsenv.cmd")
    r = subprocess.run('cmd /d /s /c "call "%s" >nul && set"' % vsenv, capture_output=True, text=True, errors="replace")
    for line in r.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            k, v = line.split("=", 1)
            os.environ[k] = v
    if not shutil.which("cl"):
        sys.exit("cmut_gpu: no cl.exe (tools\\vsenv.cmd found no Visual Studio)")


def ninja_build_flags():
    """The tree's Debug compile flags for main_translator.cpp and its link line, from build\\x64-Debug\\build.ninja."""
    text = open(os.path.join(BUILD, "build.ninja"), encoding="utf-8", errors="replace").read()

    def block(head):
        i = text.find(head)
        if i < 0:
            sys.exit("cmut_gpu: build.ninja has no %r (build the tree first)" % head)
        lines = text[i:].split("\n")
        first, kv = lines[0], {}
        for l in lines[1:]:
            if not l.startswith("  "):
                break
            k, _, v = l.strip().partition(" = ")
            kv[k] = v
        return first, kv

    _, comp = block("build engine\\CMakeFiles\\suvtitle_engine.dir\\main_translator.cpp.obj:")
    first, link = block("build SuVtitleEngine.exe:")
    objs = [t for t in first.split("||")[0].split()[3:] if t.endswith((".obj", ".res"))]
    others = [os.path.join(BUILD, t) for t in objs if not t.endswith("main_translator.cpp.obj")]
    for o in others:
        if not os.path.exists(o):
            sys.exit("cmut_gpu: the tree's object %s is missing (build the tree first)" % o)
    incs = ["/I" + p for p in re.findall(r'-I"([^"]+)"', comp.get("INCLUDES", ""))]
    cflags = comp.get("DEFINES", "").split() + comp.get("FLAGS", "").split() + incs
    lflags = [("/INCREMENTAL:NO" if f.upper() == "/INCREMENTAL" else f) for f in link.get("LINK_FLAGS", "").split()]
    return cflags, others, lflags, link.get("LINK_LIBRARIES", "").split()


def gpu_gate(log):
    """The brief's gate (see the header). True = idle; waits up to 10 min."""
    counter = "\\GPU Engine(*engtype_3D)\\Utilization Percentage"
    for attempt in range(20):
        r = subprocess.run(["typeperf", counter, "-sc", "3"], capture_output=True, text=True, errors="replace", timeout=60)
        lines = [l for l in r.stdout.splitlines() if l.startswith('"')]
        head = [c.strip('"') for c in lines[0].split('","')] if lines else []
        rows = [[c.strip('"') for c in l.split('","')] for l in lines[1:] if re.match(r'^"\d\d/\d\d/\d{4} ', l)]
        names = {}
        for i in range(1, len(head)):
            m = re.search(r"pid_(\d+)_", head[i])
            if m and m.group(1) not in names and any(_num(row, i) > 0 for row in rows):
                t = subprocess.run(["tasklist", "/FI", "PID eq " + m.group(1), "/FO", "CSV", "/NH"],
                                   capture_output=True, text=True, errors="replace")
                names[m.group(1)] = (t.stdout.split(",")[0].strip().strip('"').lower() or "?")
        others, wall = [], []
        for row in rows:
            s = w = 0.0
            for i in range(1, min(len(head), len(row))):
                v = _num(row, i)
                m = re.search(r"pid_(\d+)_", head[i])
                if v > 0 and m and names.get(m.group(1)) == "astraminimalist.exe":
                    w += v
                elif v > 0:
                    s += v
            others.append(s)
            wall.append(w)
        busy = not rows or max(others) > 20.0
        line = "gate %s: others %s %%, wallpaper (excluded) %s %% -> %s" % (
            time.strftime("%H:%M:%S"), [round(x, 1) for x in others], [round(x, 1) for x in wall], "BUSY" if busy else "idle")
        print("    " + line)
        log.write(line + "\n")
        if not busy:
            return True
        time.sleep(30)
    return False


def _num(row, i):
    try:
        return float(row[i])
    except (ValueError, IndexError):
        return 0.0


def run_one(tag, text, cflags, others, lflags, libs, log):
    d = os.path.join(WORK, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    src = os.path.join(d, "main_translator.cpp")
    open(src, "wb").write(text.encode("utf-8"))
    obj, exe = os.path.join(d, "main_translator.obj"), os.path.join(d, "SuVtitleEngine.exe")
    c = subprocess.run(["cl", "/nologo", "/TP"] + cflags + ["/Fo" + obj, "/Fd" + os.path.join(d, "mut.pdb"), "/FS", "-c", src],
                       cwd=d, capture_output=True, text=True, errors="replace")
    if c.returncode != 0:
        return False, "does not compile: " + " / ".join((c.stdout + c.stderr).strip().splitlines()[-3:])
    l = subprocess.run(["link", "/nologo", obj] + others + ["/out:" + exe, "/pdb:" + os.path.join(d, "SuVtitleEngine.pdb")]
                       + lflags + libs, cwd=d, capture_output=True, text=True, errors="replace")
    if l.returncode != 0 or not os.path.exists(exe):
        return False, "does not link: " + " / ".join((l.stdout + l.stderr).strip().splitlines()[-3:])
    if not gpu_gate(log):
        return False, "NOT RUN: the GPU stayed busy for 10 min"
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ("AUDIOPB_TR_FORCE_CPU", "VK_LOADER_DRIVERS_DISABLE", "GGML_DISABLE_VULKAN",
                                "APPDATA", "LOCALAPPDATA", "TEMP", "TMP")}
    for k in ("appdata", "localappdata", "temp"):
        os.makedirs(os.path.join(d, "env", k), exist_ok=True)
    env["APPDATA"], env["LOCALAPPDATA"] = os.path.join(d, "env", "appdata"), os.path.join(d, "env", "localappdata")
    env["TEMP"] = env["TMP"] = os.path.join(d, "env", "temp")
    t0 = time.time()
    try:
        r = subprocess.run([exe, "--selftest-gpuprobe"], cwd=d, env=env, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=600)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        rc = None
    open(os.path.join(d, "selftest.log"), "w", encoding="utf-8").write(out)
    secs = time.time() - t0
    if "REFUSED" in out:
        return False, "the selftest REFUSED to run (its trap guard): proves nothing"
    passed = rc == 0 and "GPUPROBE SELFTEST PASSED" in out
    fails = [x.strip() for x in out.splitlines() if "ASSERT FAIL" in x]
    if rc is None:
        return True, "the selftest hung past 600 s (%.0f s)" % secs
    return (not passed), ("PASSED" if passed else ("%s ... (%d assertion(s), %.0f s)" % (fails[0], len(fails), secs) if fails
                                                   else "exit %s (%.0f s)" % (rc, secs)))


def main():
    if "--dry" in sys.argv:
        sys.exit(dry())
    only = sys.argv[1] if len(sys.argv) > 1 else None
    if not os.environ.get("SUVTITLE_PAYLOAD"):
        sys.exit("cmut_gpu: set SUVTITLE_PAYLOAD (the selftest's [4] [5] need the real llama-server; without it the runHost "
                 "mutants survive)")
    vs_environment()
    cflags, others, lflags, libs = ninja_build_flags()
    os.makedirs(WORK, exist_ok=True)
    log = open(os.path.join(WORK, "cmut_gpu.log"), "a", encoding="utf-8")
    text = source()
    print("CONTROL: the unmutated engine, built and run the same way")
    ok, why = run_one("control", text, cflags, others, lflags, libs, log)
    print("  %s  [control] %s" % ("PASS" if why == "PASSED" else "FAIL", why))
    if why != "PASSED":
        sys.exit("cmut_gpu: the CONTROL does not pass -- nothing below would prove anything")
    bad = 0
    for k, (name, edits) in enumerate(MUTANTS):
        if only and only not in name:
            continue
        mtext, err = apply(text, edits)
        if mtext is None:
            caught, why = False, err
        else:
            caught, why = run_one("m%02d" % (k + 1), mtext, cflags, others, lflags, libs, log)
        line = "  %s  [gpu] MUTANT \"%s\" -- %s" % ("PASS" if caught else "FAIL", name, why)
        print(line)
        log.write(line + "\n")
        log.flush()
        bad += 0 if caught else 1
    print("CMUT_GPU %s: %d sentinel(s) failed" % ("OK" if bad == 0 else "FAILED", bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

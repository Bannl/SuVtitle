# fixround_pins.py -- S-HOST fix round (2026-10-23): SOURCE PINS for the wiring of the fixes that no unit test can
# execute (bridge.cpp and main.cpp need a WebView2 and a window; tests\e2e\real runs them for real, slowly). Each pin
# reads the code and fails if the fix is not there. Run over the pre-fix copy they fail (the proof), over app\ they pass:
#   python -I tests\cpp\fixround_pins.py [--root <folder holding the app sources>]
# The pins: RV2-1 the engine pipe's flags + DACL + client check (TranslatorManager.cpp; pipe_smoke runs it); RV2-4 the
# 404 of our own + the CSP on every answer (main.cpp); RV2-6 pumpTranslator's guard (bridge.cpp); RV2-7 the sweep
# before the marker (AudioPBImport.h; app_smoke runs it); RV4-1 playerSubGet's `auto` asked first + userSubFile()
# before every file the viewer names (bridge.cpp; player_smoke runs the core's half); RV4-7 the REAL topmost read back
# and told (main.cpp, TrWindowState.h; player_smoke runs the post). ASCII only.
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
root = os.path.normpath(os.path.join(HERE, "..", "..", "app"))
if "--root" in sys.argv:
    root = sys.argv[sys.argv.index("--root") + 1]

# the policy main.cpp must send, word for word (the E2E's R19 checks the header the real app sends against it too)
CSP = ("default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; font-src 'self'; img-src data:; "
       "media-src https://*.suvtitle-media.example https://suvtitle-proxy.example; base-uri 'none'; form-action 'none'; "
       "frame-ancestors 'none'; object-src 'none'")

passed = failed = 0


def ok(name, cond, extra=""):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS  " + name)
    else:
        failed += 1
        print("  FAIL  " + name + (("  -> " + extra) if extra else ""))


def read(name):
    try:
        with open(os.path.join(root, name), encoding="ascii", newline="") as f:
            return f.read().replace("\r\n", "\n")
    except OSError:
        return ""


def body(src, head):
    """The text of the function / case starting at `head`, to its matching closing brace."""
    at = src.find(head)
    if at < 0:
        return ""
    i = src.find("{", at)
    if i < 0:
        return ""
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[at:j + 1]
    return ""


def code(s):
    """`s` without its // comments (a pin must hold in the code, not in a comment about it)."""
    return "\n".join(l.split("//")[0] if "//" in l and '"' not in l.split("//")[0] else l for l in s.split("\n"))


print("fixround_pins over " + root)
tm, br, mn, tw, im = (read(n) for n in ("TranslatorManager.cpp", "bridge.cpp", "main.cpp", "TrWindowState.h", "AudioPBImport.h"))
ok("(the five sources read)", all((tm, br, mn, tw, im)))

# ---- RV2-1 -------------------------------------------------------------------------------------------------------------
create = re.findall(r"CreateNamedPipeW\((.*?)\);", code(tm), re.S)
ok("RV2-1: the engine pipe is created ONCE, with FILE_FLAG_FIRST_PIPE_INSTANCE + PIPE_REJECT_REMOTE_CLIENTS + our own "
   "security attributes (never the default DACL)",
   len(create) == 1 and "FILE_FLAG_FIRST_PIPE_INSTANCE" in create[0] and "PIPE_REJECT_REMOTE_CLIENTS" in create[0]
   and re.search(r",\s*&sa\s*$", create[0].strip()) is not None, str(create)[:300])
ok("RV2-1: the DACL is protected and names the token's user only (\"D:P(A;;GA;;;\" + the SID)",
   'L"D:P(A;;GA;;;" + std::wstring(sid) + L")"' in tm and "TokenUser" in tm)
ok("RV2-1: the client that connects must be the spawned engine (GetNamedPipeClientProcessId == pi.dwProcessId), "
   "else DisconnectNamedPipe",
   re.search(r"GetNamedPipeClientProcessId\(pipe, &cpid\) && cpid == pi\.dwProcessId", tm) is not None
   and "DisconnectNamedPipe(pipe)" in code(tm))

# ---- RV2-4 -------------------------------------------------------------------------------------------------------------
sfa = body(mn, "bool serveFreshWebAsset(")
after_origin = sfa.split("// not our origin", 1)[1] if "// not our origin" in sfa else ""
ok("RV2-4: a miss on our origin is answered HERE (404) -- no `return false` after the origin check",
   bool(after_origin) and "respondWeb(args, 404" in after_origin and "return false" not in code(after_origin), after_origin[:300])
m = re.search(r"constexpr wchar_t kWebCsp\[\] =\s*((?:L\"[^\"]*\"\s*)+);", mn)
policy = "".join(re.findall(r"L\"([^\"]*)\"", m.group(1))) if m else ""
ok("RV2-4: the Content-Security-Policy is exactly the page's (no blob:, no eval, no connect, no wildcard but the media hosts)",
   policy == CSP, policy)
rw = body(mn, "bool respondWeb(")
ok("RV2-4: EVERY answer carries it (respondWeb: the 200s and the 404) with no-store",
   'L"Content-Security-Policy: " + kWebCsp' in rw and "no-store" in rw and sfa.count("respondWeb(") == 2)

# ---- RV2-6 -------------------------------------------------------------------------------------------------------------
pt = code(body(br, "void Bridge::pumpTranslator()"))
first = [l.strip() for l in pt.split("\n")[1:] if l.strip()]
ok("RV2-6: pumpTranslator's FIRST statement is uiTick's guard (if (!running_.load()) return;)",
   bool(first) and first[0] == "if (!running_.load()) return;", first[0] if first else "")

# ---- RV2-7 -------------------------------------------------------------------------------------------------------------
run = code(body(im, "inline Result run(const Roots& r)"))
ok("RV2-7: the import sweeps *.import-tmp (both folders) BEFORE it looks at the marker",
   "sweepTmp(r.toFiles)" in run and "sweepTmp(r.toRoaming)" in run
   and run.find("sweepTmp(") < run.find("fileExists(res.marker)"))

# ---- RV4-1 (ADDENDUM 2) ----------------------------------------------------------------------------------------------------
psg = br[br.find('if (type == L"playerSubGet")'):]
psg = psg[:psg.find("\n    if (type == ", 10)] if psg else ""
psgc = code(psg)
gate = psgc.find("if (!player_->acceptSubGet(uid, isAuto)) return;")
ok("RV4-1: playerSubGet reads `auto` and asks acceptSubGet FIRST (before the cancel, the extraction, the file read)",
   'jsonFindBool(json, L"auto", isAuto)' in psgc and gate > 0
   and all(gate < psgc.find(x) for x in ("cancelSubRequest()", "player_->subGet(uid)", "loadExtSub(")), psg[:200])
user = re.findall(r"^.*loadExtSub\([^;]*, 0, 0\);.*$", code(br), re.M)
ok("RV4-1: every subtitle file the VIEWER names (5 calls: --play, the drop, the two scan paths, Load subtitles...) is "
   "marked first: player_->userSubFile(); loadExtSub(..., 0, 0)",
   len(user) == 5 and all("player_->userSubFile(); loadExtSub(" in l for l in user), "\n".join(user))

# ---- RV4-7 -------------------------------------------------------------------------------------------------------------
st = code(body(mn, "void setTopmost(bool on)"))
ok("RV4-7: setTopmost keeps what Windows MADE of the request (WS_EX_TOPMOST read back) and tells the page every time",
   "g_topmost = realTopmost(g_hwnd);" in st and "translatorTopmostChanged(g_topmost)" in st
   and st.find("SetWindowPos(") < st.find("realTopmost(") < st.find("translatorTopmostChanged("))
wp = code(mn[mn.find("case WM_WINDOWPOSCHANGED:"):][:600]) if "case WM_WINDOWPOSCHANGED:" in mn else ""
ok("RV4-7: a change made by another program (WM_WINDOWPOSCHANGED) is followed: the real state, app.txt, the page",
   "realTopmost(hwnd)" in wp and "saveWinPos()" in wp and "translatorTopmostChanged(g_topmost)" in wp)
ok("RV4-7: the start state is told before the page's first `ready` (right after CreateWindowExW)",
   re.search(r"CreateWindowExW\(.*?g_topmost = realTopmost\(g_hwnd\);[^\n]*\n\s*g_bridge->translatorTopmostChanged\(g_topmost\);",
             mn, re.S) is not None)
ok("RV4-7: TrWindowState's post carries topmost on EVERY post", '.kv(L"topmost", topmost)' in code(body(tw, "std::wstring post()")))

print("\nfixround_pins: %d passed, %d failed" % (passed, failed))
sys.exit(0 if failed == 0 else 1)

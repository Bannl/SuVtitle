# WC: the external half of filedocx_smoke. Run with the isolated interpreter:
#   python -I filedocx_check.py filedocx_out.docx   (build_filedocx_smoke.cmd finds the Python: tools\pyenv.cmd)
# Opens the .docx as a ZIP, parses every part as XML (Word refuses a package with one malformed
# part) and checks the file-transcript paragraphs.
import sys, zipfile
import xml.etree.ElementTree as ET

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
fails = 0
def ok(name, cond, extra=""):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + name + ("" if cond or not extra else "  -> " + str(extra)))
    if not cond: fails += 1

z = zipfile.ZipFile(sys.argv[1])
names = z.namelist()
ok("[Content_Types].xml is the FIRST entry (OPC)", names[0] == "[Content_Types].xml", names)
for n in names:
    try:
        ET.fromstring(z.read(n)); ok("well-formed XML: " + n, True)
    except ET.ParseError as e:
        ok("well-formed XML: " + n, False, e)
doc = ET.fromstring(z.read("word/document.xml"))
paras = []
for p in doc.iter(W + "p"):
    paras.append("".join(t.text or "" for t in p.iter(W + "t")))
print("  paragraphs:", len(paras))
ok("title + subtitle first", paras[0].startswith("film.mkv") and paras[1].startswith("Transcript"), paras[:2])
ok("media stamps, hours always shown", any(p.startswith("[00:00:01]") for p in paras)
   and any(p.startswith("[01:02:03]") for p in paras), paras)
ok("& < > and quotes come back intact (escaped in the XML, not lost)",
   any("A & B < C > D, \"quoted\"." in p for p in paras))
ok("the original is named by language the first time ([English] ...)",
   any(p.startswith("[English] Hello everyone.") for p in paras), paras)
pass_text = "\u597d\u7684\u3002"
ok("a PASSTHROUGH appears exactly once", sum(p.count(pass_text) for p in paras) == 1)
ok("the control character is gone, the sentence is not", any("Bonjour tout le monde." in p for p in paras)
   and not any("\x01" in p for p in paras))
ok("the spoken language is named again where it CHANGES ([Fran\u00e7ais])",
   any(p.startswith("[Fran\u00e7ais] Bonjour") for p in paras), paras)
styles = z.read("word/styles.xml").decode("utf-8")
ok("all-Traditional document -> JhengHei is the default East Asian face (WA's rule)",
   'w:eastAsia="Microsoft JhengHei"' in styles)

# ---- the review of WA: the script is a property of each LINE, not of the speaker's turn ----
import os
mixed = os.path.join(os.path.dirname(sys.argv[1]), "mixedturn_out.docx")
mz = zipfile.ZipFile(mixed)
mdoc = ET.fromstring(mz.read("word/document.xml"))
SIMP, TRAD = "简体", "繁體"     # the first two characters of each test sentence
def runs_with(prefix):
    out = []
    for r in mdoc.iter(W + "r"):
        text = "".join(t.text or "" for t in r.iter(W + "t"))
        if prefix in text:
            rf = r.find(W + "rPr/" + W + "rFonts")
            lang = r.find(W + "rPr/" + W + "lang")
            out.append((text, rf.get(W + "eastAsia") if rf is not None else None,
                        lang.get(W + "eastAsia") if lang is not None else None))
    return out
s_runs, t_runs = runs_with(SIMP), runs_with(TRAD)
ok("both scripts are in the mixed turn", len(s_runs) >= 2 and len(t_runs) >= 2, (s_runs, t_runs))
ok("every SIMPLIFIED sentence keeps the default face (no JhengHei, no zh-TW)",
   all(f is None and l is None for _, f, l in s_runs), s_runs)
ok("every TRADITIONAL sentence wears JhengHei and lang zh-TW",
   all(f == "Microsoft JhengHei" and l == "zh-TW" for _, f, l in t_runs), t_runs)
badge = [r for r in runs_with("[")]
ok("the [简体中文] badge is set in the Simplified face it names",
   any(("[简体中文]" in t) and f is None for t, f, _ in badge), badge)
mstyles = mz.read("word/styles.xml").decode("utf-8")
ok("a document holding BOTH scripts keeps YaHei as its default",
   'w:eastAsia="Microsoft YaHei"' in mstyles)
print("FILEDOCX CHECK " + ("OK" if fails == 0 else "FAILED (%d)" % fails))
sys.exit(1 if fails else 0)

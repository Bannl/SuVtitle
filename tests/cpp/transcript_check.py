# U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 6): the external half of transcript_smoke, and the checker
# of any SPEAKER TRANSCRIPT SuVtitle writes (the real run's too). Run with the isolated interpreter:
#   python -I transcript_check.py <a transcript .docx> [--turns N] [--speakers A,B,...] [--no-names] [--source-tc]
# Opens the .docx as a ZIP ([Content_Types].xml first), parses EVERY part as XML (Word refuses a package with one malformed
# part), checks the named styles and the paragraphs: the Title, the Subtitle, the header, then one paragraph per turn --
# a "Timecode" run "[hh:mm:ss] " (or a source timecode "[hh:mm:ss:ff] " / "[hh:mm:ss;ff] "), a "Speaker" run "Name: " (none
# with --no-names), the text -- and nothing else after the header (no translation line, no subtitle fragment). Standard
# library only. Prints TRANSCRIPT CHECK OK|FAILED (n).
import re, sys, zipfile
import xml.etree.ElementTree as ET

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
fails = 0
def ok(name, cond, extra=""):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + name + ("" if cond or not extra else "  -> " + str(extra)[:400]))
    if not cond: fails += 1

args = sys.argv[1:]
path = args[0]
want_turns = int(args[args.index("--turns") + 1]) if "--turns" in args else None
want_spk = args[args.index("--speakers") + 1].split(",") if "--speakers" in args else None
no_names = "--no-names" in args
source_tc = "--source-tc" in args

z = zipfile.ZipFile(path)
names = z.namelist()
ok("[Content_Types].xml is the FIRST entry (OPC)", names[0] == "[Content_Types].xml", names)
for n in names:
    try:
        ET.fromstring(z.read(n)); ok("well-formed XML: " + n, True)
    except ET.ParseError as e:
        ok("well-formed XML: " + n, False, e)
styles = ET.fromstring(z.read("word/styles.xml"))
ids = {s.get(W + "styleId"): s for s in styles.iter(W + "style")}
for sid, kind in (("Normal", "paragraph"), ("Title", "paragraph"), ("Subtitle", "paragraph"), ("Speaker", "character"), ("Timecode", "character")):
    ok("the named style %s (%s)" % (sid, kind), sid in ids and ids[sid].get(W + "type") == kind)
ok("Normal is the default paragraph style", "Normal" in ids and ids["Normal"].get(W + "default") == "1")

doc = ET.fromstring(z.read("word/document.xml"))
paras = []
for p in doc.iter(W + "p"):
    ps = p.find(W + "pPr/" + W + "pStyle")
    runs = []
    for r in p.iter(W + "r"):
        rs = r.find(W + "rPr/" + W + "rStyle")
        runs.append((rs.get(W + "val") if rs is not None else "", "".join(t.text or "" for t in r.iter(W + "t"))))
    paras.append((ps.get(W + "val") if ps is not None else "", runs))
print("  paragraphs:", len(paras))
ok("the Title first, then the Subtitle", len(paras) > 2 and paras[0][0] == "Title" and paras[1][0] == "Subtitle",
   [(p[0], "".join(t for _, t in p[1])[:40]) for p in paras[:3]])
text = ["".join(t for _, t in p[1]) for p in paras]
ok("the header says it is an automatic transcription, to be checked",
   any("Automatic transcription by SuVtitle" in t and "to be checked" in t for t in text[:8]), text[:8])
ok("the header names the timecode", any(t.startswith("Timecode: ") for t in text[:8]), text[:8])
stamp = re.compile(r"^\[\d\d:\d\d:\d\d([:;]\d\d)?\] $" if source_tc else r"^\[\d\d:\d\d:\d\d\] $")
turns = [p for p in paras if p[1] and p[1][0][0] == "Timecode"]
first = next((i for i, p in enumerate(paras) if p[1] and p[1][0][0] == "Timecode"), len(paras))
ok("after the header, EVERY paragraph is a turn (no translation line, no stray fragment)",
   len(turns) > 0 and all(p[1] and p[1][0][0] == "Timecode" for p in paras[first:]),
   [text[i][:60] for i in range(first, len(paras)) if not (paras[i][1] and paras[i][1][0][0] == "Timecode")][:5])
ok("every turn's stamp is a timecode run", all(stamp.match(p[1][0][1]) for p in turns), [p[1][0][1] for p in turns][:8])
if no_names:
    ok("no names: no Speaker run", all(len(p[1]) == 2 and p[1][1][0] == "" for p in turns))
else:
    ok("every turn names its speaker (\"Name: \") in a Speaker run, then its text",
       all(len(p[1]) >= 3 and p[1][1][0] == "Speaker" and p[1][1][1].endswith(": ") and p[1][2][1].strip() for p in turns),
       [[r for r in p[1]] for p in turns if not (len(p[1]) >= 3 and p[1][1][0] == "Speaker")][:3])
    seq = [p[1][1][1][:-2] for p in turns if len(p[1]) >= 2]
    if want_spk is not None:
        ok("the speakers, turn by turn", seq == want_spk, (seq, want_spk))
if want_turns is not None:
    ok("%d turns" % want_turns, abs(len(turns) - want_turns) <= 0, len(turns))
stamps = [p[1][0][1] for p in turns]
ok("the stamps never go back in time", stamps == sorted(stamps), stamps[:12])
print("TRANSCRIPT CHECK " + ("OK" if fails == 0 else "FAILED (%d)" % fails))
sys.exit(1 if fails else 0)

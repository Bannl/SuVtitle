# cmut_u2.py -- MUTATION SENTINELS for U2's C++ (SuVtitle 1.1.0, wave U2 "Import model...", 2026-11-02, docs/PROGRESS_U2.md).
# The repo's cmut.py machinery (copy the smoke + the product files it compiles into a scratch tree with the SAME relative layout,
# apply ONE textual change, compile with the smoke's own flags, run: CAUGHT = the smoke exits non-zero; a mutant that does not
# compile, or applies other than exactly once, is a FAILED sentinel), over the two smokes U2 extended:
#   app_u2     tests/cpp/app_smoke.cpp      [9]   TrPipe.h (the model HEADER classifier, FILE's asrModel / mtModel, the path rule,
#                                                 the failure sentence's kind), AppConfig.h (app.txt MODEL / MODELUSE),
#                                                 TrFileStore.h (the cache key's model part, modelIdent, the siblings)
#   player_u2  tests/cpp/player_smoke.cpp   [U2]  TrPlayer.h (the FILE names the set's models, the keys, the fall back after an
#                                                 imported model failed, modelsStale, the user's Cancel-all rule)
#   U2b (02/11/2026, docs/PROGRESS_U2b.md -- removing an imported model, the batch's one retry):
#   app_u2b    tests/cpp/app_smoke.cpp      [13]  AppConfig.h removeImportedModel (the line, the choice, Windows' case rule)
#   player_u2b tests/cpp/player_smoke.cpp   [U2b] TrPlayer.h (the retry ONCE, a batch's only, with the default; the message once;
#                                                 the redo said; the models in use)
#
#   python -I cmut_u2.py <app_u2|player_u2|app_u2b|player_u2b|all> [name-filter]      (run it from cmut_u2.cmd: cl on PATH)
#   python -I cmut_u2.py --dry                                      every mutant applies exactly once? (no compile)
#
# The engine's half (makeFileJob's checks, chooseFileModels, the servant door, bringFile's mtExact / lastFailKind) is asserted by
# the engine's own selftest ([0] u2ModelTests, --selftest offline); its mutants were run by hand on scratch builds (PROGRESS_U2).
# When a product line changes legitimately, EDIT THE MUTANT WITH IT (an inapplicable mutant fails: it proves nothing).
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I does not put the script's folder on the path)
import cmut

cmut.WORK = os.environ.get("SUVTITLE_CMUT_WORK") or os.path.join(tempfile.gettempdir(), "suvtitle_cmut_u2")

PLAYER_FILES = ["app/TrPlayer.h", "app/TrWindowState.h", "app/TrFileStore.h", "app/JsonW.h", "common/TrPipe.h",
                "app/MediaTracks.h", "app/SubtitleReader.h",
                "app/TrTranscript.h", "app/SourceTimecode.h"]   # (U3: TrPlayer.h includes them)

TARGETS = {
    "app_u2": dict(
        smoke="tests/cpp/app_smoke.cpp", sources=[],
        files=["app/AudioPBImport.h", "app/AppConfig.h", "app/NextaleUpdate.h", "app/TrAssoc.h", "app/TrLaunch.h"] + PLAYER_FILES,
        inc=["common"], libs=["user32.lib", "shell32.lib", "shlwapi.lib", "ole32.lib", "advapi32.lib"], helper=None,
        mutants=[
            # TrPipe.h -- the header classifier
            ("[9] a GGUF of any version taken (v1 / v4: llama.cpp reads neither)", "common/TrPipe.h",
             'if (v != 2 && v != 3) return none("it is a GGUF of a version llama.cpp does not read");', "(void)v;"),
            ("[9] a GGUF with no tensor / no key taken", "common/TrPipe.h",
             "if (tensors == 0 || tensors > (1ull << 20) || keys == 0 || keys > (1ull << 20))",
             "if (tensors > (1ull << 20) || keys > (1ull << 20))"),
            ("[9] the audio context unchecked (llama.cpp's first ggml files could pass for whisper)", "common/TrPipe.h",
             "h[1] == 1500 && h[5] == 448", "h[5] == 448"),
            ("[9] the text context unchecked", "common/TrPipe.h",
             "h[1] == 1500 && h[5] == 448", "h[1] == 1500"),
            ("[9] the mel filters' n_fft unchecked", "common/TrPipe.h",
             "&& h[11] == h[9] && h[12] == 201", "&& h[11] == h[9]"),
            ("[9] any mel count taken (81)", "common/TrPipe.h",
             "(h[9] == 80 || h[9] == 128)", "h[9] > 0"),
            ("[9] any audio layer count taken (70)", "common/TrPipe.h",
             "&& h[4] >= 1 && h[4] <= 64 &&", "&& h[4] >= 1 &&"),
            ("[9] any vocabulary taken (40 000)", "common/TrPipe.h",
             "h[0] >= 51000 && h[0] <= 60000 && ", ""),
            ("[9] a whisper header one byte short read as whole", "common/TrPipe.h",
             'if (n < 56) return none("the file is cut short (its whisper.cpp header ends early)");',
             'if (n < 55) return none("the file is cut short (its whisper.cpp header ends early)");'),
            # TrPipe.h -- FILE's two fields and the path rule
            ("[9] a FILE naming no model gets two empty fields (the pre-U2 line changes)", "common/TrPipe.h",
             "    const bool models = !f.asrModel.empty() || !f.mtModel.empty();", "    const bool models = true;"),
            ("[9] no empty sound-track field before the models (they land in `audio`)", "common/TrPipe.h",
             "if (!f.audio.empty() || models) o +=", "if (!f.audio.empty()) o +="),
            ("[9] unpackFile takes a malformed transcription-model path", "common/TrPipe.h",
             "        && modelFieldOk(f.asrModel) && modelFieldOk(f.mtModel);", "        && modelFieldOk(f.mtModel);"),
            ("[9] a '..' / '.' segment taken (a path that climbs)", "common/TrPipe.h",
             'if (seg.empty() || seg == "." || seg == "..") return false;', "if (seg.empty()) return false;"),
            ("[9] a RELATIVE model path taken", "common/TrPipe.h",
             "    if (!drive && !unc) return false;\n", ""),
            ("[9] an alternate stream (':' past the drive) taken", "common/TrPipe.h",
             "if (p.find(':', 2) != std::string::npos || p.back() == '\\\\') return false;",
             "if (p.back() == '\\\\') return false;"),
            ("[9] a translation model's failure not recognised as one (no fall back)", "common/TrPipe.h",
             '    if (err.rfind(kFileModelErrMt, 0) == 0) return "mt";\n', ""),
            # AppConfig.h -- app.txt MODEL / MODELUSE
            ("[9] a MODELUSE naming no MODEL of its kind taken", "app/AppConfig.h",
             "if (pbt::trpipe::modelPathOk(u.second) && listHasPath(m.list(u.first), u.second)) m.use(u.first) = u.second;",
             "if (pbt::trpipe::modelPathOk(u.second)) m.use(u.first) = u.second;"),
            ("[9] the same model listed twice", "app/AppConfig.h",
             "    if (listHasPath(v, path)) return true;\n", ""),
            ("[9] no cap on the imported models", "app/AppConfig.h",
             "    if (v.size() >= kMaxModelsPerKind) return false;\n", ""),
            ("[9] any kind word taken (a list nobody reads)", "app/AppConfig.h",
             'if ((kind != "asr" && kind != "mt") || !pbt::trpipe::modelPathOk(path)) return false;',
             "if (!pbt::trpipe::modelPathOk(path)) return false;"),
            ("[9] the choices never written (MODELUSE lost)", "app/AppConfig.h",
             'return replaceTagged(replaceTagged(old, "MODEL", models), "MODELUSE", uses);',
             'return replaceTagged(old, "MODEL", models);'),
            ("[9] a model path written unescaped", "app/AppConfig.h",
             "pbt::trpipe::esc(p));", "p);"),
            ("[9] two spellings of one path are two models (the case not folded)", "app/AppConfig.h",
             "        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);\n", ""),
            # TrFileStore.h -- the cache key
            ("[9] the model part on EVERY key (every analysis made before U2 orphaned)", "app/TrFileStore.h",
             '    if (!models.empty()) k += "\\tm" + models;', '    k += "\\tm" + models;'),
            ("[9] the models never in the key (an imported model's analysis taken for the default's)", "app/TrFileStore.h",
             '    if (!models.empty()) k += "\\tm" + models;', "    (void)models;"),
            ("[9] modelsTag not empty for the defaults", "app/TrFileStore.h",
             "    if (asrIdent.empty() && mtIdent.empty()) return std::string();\n", ""),
            ("[9] which half is imported does not count", "app/TrFileStore.h",
             '    return "asr=" + asrIdent + ";mt=" + mtIdent;', "    return asrIdent + mtIdent;"),
            ("[9] a model replaced at the same path is the same model (no size / write time)", "app/TrFileStore.h",
             ' + "/" + std::to_string(size) + "/" + std::to_string(mtime);', ";"),
            ("[9] where the model file is changes its identity (the folder kept)", "app/TrFileStore.h",
             "    if (cut != std::wstring::npos) leaf = leaf.substr(cut + 1);\n", ""),
            ("[9] the shared original's candidates ignore the set's models", "app/TrFileStore.h",
             "k.key = cacheKey(path, size, mtime, a, b, m, q, audioOrd, models);", "k.key = cacheKey(path, size, mtime, a, b, m, q, audioOrd);"),
        ]),
    "player_u2": dict(
        smoke="tests/cpp/player_smoke.cpp", sources=[], files=PLAYER_FILES + ["tests/web/we_vectors.txt"],
        inc=["common"], libs=["user32.lib", "shlwapi.lib", "ole32.lib"], helper=None,
        copies=[("tests/web/we_vectors.txt", "tests/cpp/we_vectors.txt")],
        mutants=[
            ("[U2] the FILE never names the transcription model", "app/TrPlayer.h",
             '        w.asrModel = set->asrPath;      // U2: the models this set is made with ("" = the default: the pre-U2 line)\n', ""),
            ("[U2] the FILE never names the translation model", "app/TrPlayer.h",
             "        w.mtModel = set->mtPath;\n", ""),
            ("[U2] a set does not remember its models (the FILE names none)", "app/TrPlayer.h",
             "        s->asrPath = asrPick_.path; s->mtPath = mtPick_.path; s->models = modelsTag_;   // U2\n",
             "        s->models = modelsTag_;   // U2\n"),
            ("[U2] a set's key ignores its models (the default's record taken for an imported model's)", "app/TrPlayer.h",
             "s->key = pbtrf::cacheKey(path, size, mtime, langA_, langB_, mode_, quality_, audio.ord, modelsTag_);",
             "s->key = pbtrf::cacheKey(path, size, mtime, langA_, langB_, mode_, quality_, audio.ord);"),
            ("[U2] the press ignores a model change (the default's partial set resumed for an imported model)", "app/TrPlayer.h",
             "        return pbtrf::cacheKey(s_.path, s_.size, s_.mtime, langA_, langB_, mode_, quality_, heardSel().ord, modelsTag_);",
             "        return pbtrf::cacheKey(s_.path, s_.size, s_.mtime, langA_, langB_, mode_, quality_, heardSel().ord);"),
            ("[U2] the batch's keys ignore the models", "app/TrPlayer.h",
             "        return pbtrf::cacheKey(it.path, it.size, it.mtime, langA_, langB_, mode_, quality_, audioFor(it).ord, modelsTag_);",
             "        return pbtrf::cacheKey(it.path, it.size, it.mtime, langA_, langB_, mode_, quality_, audioFor(it).ord);"),
            ("[U2] a model change leaves the batch's running job on the old model", "app/TrPlayer.h",
             "        modelsTag_ = pbtrf::modelsTag(a.ident, m.ident);\n        cancelPre();\n",
             "        modelsTag_ = pbtrf::modelsTag(a.ident, m.ident);\n"),
            ("[U2] the failed model stays chosen (no fall back)", "app/TrPlayer.h",
             "                pick = ModelPick{};\n", ""),
            ("[U2] the fall back drops the OTHER kind", "app/TrPlayer.h",
             '            ModelPick& pick = fellKind == "asr" ? asrPick_ : mtPick_;',
             '            ModelPick& pick = fellKind == "asr" ? mtPick_ : asrPick_;'),
            ("[U2] the fall back overrides a model chosen since", "app/TrPlayer.h",
             "            if (pick.path == fellPath) {", "            if (true) {"),
            # (U2b re-point, 02/11/2026: the host's call carries `say` and `redone` now)
            ("[U2] the host never told (app.txt keeps the failed model, the menu stale)", "app/TrPlayer.h",
             "        if (!fellKind.empty()) host_.modelFailed(fellKind, fellPath, w16(d.err), fellBack && (!current || redo), redoneName);\n", ""),
            ("[U2] any failure taken for an imported model's", "app/TrPlayer.h",
             "            const std::string k = tp::fileModelErrKind(d.err);", '            const std::string k = "asr";'),
            # (U2b re-point: a batch's redo says it in its own words, not this sentence)
            ("[U2] the strip never says SuVtitle goes back to its own model", "app/TrPlayer.h",
             "            if (current && !redo)\n                s_.err += fellKind", "            if (false)\n                s_.err += fellKind"),
            ("[U2] modelsStale never said", "app/TrPlayer.h",
             '        j.kv(L"modelsStale", S ? S->models != modelsTag_ : false);', '        j.kv(L"modelsStale", false);'),
            ("[U2 RULE] Cancel all stops every job again (U1's rule: the pressed file's too)", "app/TrPlayer.h",
             # (U3 re-point: the batches' jobs, FILE and FVOICE)
             "        for (Job& j : jobs_) if (j.pre) cancelJob(j);          // the batches' jobs (FILE and FVOICE) for files that are not playing",
             "        for (Job& j : jobs_) cancelJob(j);          // the batches' jobs (FILE and FVOICE) for files that are not playing"),
            ("[U2 RULE] a pressed playing file treated as the batch's", "app/TrPlayer.h",
             "        if (curBatch && s_.wanted) {", "        if (false) {"),
            ("[U2 RULE] the playing file the list launched survives Cancel all", "app/TrPlayer.h",
             # (U3 re-point: launched by EITHER batch, pressed for neither wish)
             "        } else if ((curBatch || (curT && !s_.wanted && !s_.twanted)) && (s_.wire || (s_.set && !s_.set->complete))) {",
             "        } else if (false) {"),
        ]),
    # ---- U2b (02/11/2026, docs/PROGRESS_U2b.md): removing an imported model (app.txt), the batch's ONE retry with the default model,
    #      the message once, the models in use -- app_smoke [13], player_smoke [U2b]
    "app_u2b": dict(
        smoke="tests/cpp/app_smoke.cpp", sources=[],
        files=["app/AudioPBImport.h", "app/AppConfig.h", "app/NextaleUpdate.h", "app/TrAssoc.h", "app/TrLaunch.h"] + PLAYER_FILES,
        inc=["common"], libs=["user32.lib", "shell32.lib", "shlwapi.lib", "ole32.lib", "advapi32.lib"], helper=None,
        mutants=[
            ("[13] a removed model's choice stays (MODELUSE names a model no longer listed)", "app/AppConfig.h",
             "    if (gone && samePathCi(m.use(kind), path)) m.use(kind).clear();\n", ""),
            ("[13] a removal never takes the line out", "app/AppConfig.h",
             "        if (samePathCi(*it, path)) { it = v.erase(it); gone = true; }", "        if (samePathCi(*it, path)) { ++it; gone = true; }"),
            ("[13] another case of the path is another model (Windows' rule broken)", "app/AppConfig.h",
             "        if (samePathCi(*it, path)) { it = v.erase(it); gone = true; }", "        if (*it == path) { it = v.erase(it); gone = true; }"),
            ("[13] a removal of a model that is not listed clears the choice", "app/AppConfig.h",
             "    if (gone && samePathCi(m.use(kind), path)) m.use(kind).clear();", "    if (samePathCi(m.use(kind), path) || !gone) m.use(kind).clear();"),
        ]),
    "player_u2b": dict(
        smoke="tests/cpp/player_smoke.cpp", sources=[], files=PLAYER_FILES + ["tests/web/we_vectors.txt"],
        inc=["common"], libs=["user32.lib", "shlwapi.lib", "ole32.lib"], helper=None,
        copies=[("tests/web/we_vectors.txt", "tests/cpp/we_vectors.txt")],
        mutants=[
            ("[U2b] no retry: a batch file its imported model failed on fails (U2's rule)", "app/TrPlayer.h",
             "        const bool redo = !batchIds.empty() && redoAllowed(fellKind, fellPath, batchIds);",
             "        const bool redo = false;"),
            ("[U2b] redone TWICE (the once-only mark ignored)", "app/TrPlayer.h",
             "        for (uint64_t id : ids) if (redone_.count(id)) return false;\n", ""),
            ("[U2b] a SINGLE press redone too (not only a batch's file)", "app/TrPlayer.h",
             "            if ((batchQ_.count(it.id) || tbatchQ_.count(it.id)) && it.size == S.size && it.mtime == S.mtime",
             "            if (it.size == S.size && it.mtime == S.mtime"),
            ("[U2b] redone with ANOTHER imported model (not the default)", "app/TrPlayer.h",
             "        if (!next.path.empty() && next.path != path) return false;\n", ""),
            ("[U2b] the message once per FILE (a model no longer chosen said again)", "app/TrPlayer.h",
             "w16(d.err), fellBack && (!current || redo), redoneName);", "w16(d.err), !current || redo, redoneName);"),
            ("[U2b] the redo keyed with the failed model (the default's cache never met)", "app/TrPlayer.h",
             "                modelsTag_ = pbtrf::modelsTag(asrPick_.ident, mtPick_.ident);\n", ""),
            ("[U2b] the playing file not redone at once", "app/TrPlayer.h",
             "                s_.redo = (d.err.empty() ? std::wstring() : w16(d.err) + L\" \") + redoSentence(fellKind);\n                runAnalysis(false);\n",
             "                s_.redo = (d.err.empty() ? std::wstring() : w16(d.err) + L\" \") + redoSentence(fellKind);\n"),
            ("[U2b] the strip never says the redo", "app/TrPlayer.h",
             '        j.kv(L"redo", s_.redo);', '        j.kv(L"redo", std::wstring());'),
            # (FX re-point, F5 -- the review 02/11: the rows say the redo once it STARTED -- redoRan_, not the grant redone_)
            ("[U2b] the queue never says the redo", "app/TrPlayer.h",
             '            j.kv(L"redo", redoRan_.count(it.id) > 0);', '            j.kv(L"redo", false);'),
            ("[U2b] a row's update never says the redo", "app/TrPlayer.h",
             '.kv(L"redo", redoRan_.count(id) > 0)', '.kv(L"redo", false)'),
            ("[U2b] a redo keeps no file in the batch (it leaves it as failed)", "app/TrPlayer.h",
             "        if (d.result == \"ok\" || (failed && !redo)) batchFinished(j.set->key, failed);",
             "        if (d.result == \"ok\" || failed) batchFinished(j.set->key, failed);"),
            ("[U2b] a cancelled job's model counted free while its FDONE is owed", "app/TrPlayer.h",
             "        for (const Job& j : jobs_) if (!j.voice) { add(j.set->asrPath); add(j.set->mtPath); }",
             "        for (const Job& j : jobs_) if (!j.voice && !j.cancelSent) { add(j.set->asrPath); add(j.set->mtPath); }"),
            ("[U2b] the host never told the models in use changed (the menu's x stale)", "app/TrPlayer.h",
             "        host_.modelsInUseChanged();\n", ""),
            ("[U2b] a model in use compared with its case (Windows' rule broken)", "app/TrPlayer.h",
             "            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);\n            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);\n            if (x != y) return false;",
             "            if (x != y) return false;"),
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
    print("CMUT_U2 --dry: %d mutant(s) do not apply exactly once" % bad)
    return 1 if bad else 0


def main():
    if "--dry" in sys.argv:
        sys.exit(dry())
    cmut.TARGETS.clear()
    cmut.TARGETS.update(TARGETS)
    cmut.main()


if __name__ == "__main__":
    main()

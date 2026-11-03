#!/usr/bin/env node
'use strict';
// =============================================================================
// SuVtitle - installer/gen_iss_registry.js
//
// GENERATES the player-registration block of installer/SuVtitle_Setup.iss ([Registry], HKLM) from the C++ lists that
// own the truth, so the installer and the application can never disagree about which files SuVtitle opens:
//   * app/TrFileStore.h  mediaExts()   -> the 32 media extensions (video + audio)
//   * app/TrAssoc.h      isAudioExt()  -> which of them are the audio family
//                        kAppName / kAppDesc / kProgVideo / kProgAudio / kVendorKey, the "audio" / "video" suffixes of the
//                        friendly names, openCommand() ("<exe>" --play "%1"), the icon index
// It writes the same registry the application writes per user from its own TrAssoc.h entries() (HKCU, "Set as default
// player" / --register-player), machine-wide: SuVtitle in every media file's "Open with" and in Settings > Default apps with
// its file types. Windows lets no application make itself the default; this REGISTERS it as a candidate.
// On top of entries() the installer adds what only an installer can own (flagged uninsdelete*): Applications\SuVtitle.exe
// (FriendlyAppName, DefaultIcon, shell\open\command, SupportedTypes: the "Open with > Choose another app" list and the
// Browse... case) and a Software\NEXTALE parent key that is dropped on uninstall only when empty (AudioPB may share it).
//
// Ported 2026-10-23 from AudioPB's scratchpad wg/gen_iss_registry.py (WG, 11/10/2026) -- same block shape as the 1.0.6
// installer's, under SuVtitle's names.
//
// It also GENERATES (03/11/2026, the LIGHT installer) the MODELS block at the top of the .iss -- ISPP defines: ModelCount,
// ModelsTotalBytes, ModelFile[] / ModelSize[] / ModelSha256[] / ModelUrl[] -- from installer/models.json, the one list of the
// AI models (file, size, SHA-256, the Hugging Face URL pinned to a commit, license). The light installer's [Code]
// (installer/models_download.iss) downloads from it what {app}\models lacks; the full one (/DFull) refuses a bundle whose
// model files or manifest rows differ from it. How the light installer BEHAVES is tested by tests/installer/ (this script
// checks the script's structure and that both variants compile).
//
// Usage (from anywhere):
//   node installer\gen_iss_registry.js            print the two blocks
//   node installer\gen_iss_registry.js --write    replace the two blocks in SuVtitle_Setup.iss (line endings kept)
//   node installer\gen_iss_registry.js --check [--no-compile]
//        exit 1 unless the .iss holds EXACTLY the generated blocks AND the other facts the .iss repeats from the C++ / CMake
//        sources still agree (version, mutex, exe name, payload layout incl. engine\sherpa, models.json = the models the
//        engine asks for, command-line switches incl. the --live shortcut, models\ kept on update and removed on uninstall)
//        AND both variants compile with ISCC (Inno Setup 6: SUVTITLE_ISCC, else PATH, else its standard folders) against
//        throwaway bundles: the LIGHT one packs no model, is one file and downloads exactly models.json's list; the FULL one
//        spans disks; each refuses the other's bundle, and the full one a model of another size or a manifest with other
//        model bytes. The compiles write nothing (ISCC /O-) and point ISCC's output folder at a throwaway one: ISCC deletes
//        the previous <name>.exe and <name>-N.bin of its output folder even with /O- (measured 03/11/2026: that is how the
//        1.1.0 setups of installer\Output went), so a check never touches installer\Output.
//        --no-compile skips the compile checks (they are reported SKIPPED, never passed).
// =============================================================================

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const REPO = path.resolve(__dirname, '..');
const ISS = path.join(REPO, 'installer', 'SuVtitle_Setup.iss');
const BEGIN = '; >>> player registration (generated) >>>';
const END = '; <<< player registration (generated) <<<';
const MBEGIN = '; >>> models (generated) >>>';
const MEND = '; <<< models (generated) <<<';
// The light installer's download code: its own file, #included in the .iss's [Code] (and by the test kit's setup).
const REGION_FILE = path.join(REPO, 'installer', 'models_download.iss');
const EXE = '{app}\\{#MyAppExe}';          // what the .iss calls the installed exe (a #define there)

const read = (rel) => fs.readFileSync(path.join(REPO, rel), 'utf8');
const unescapeC = (s) => s.replace(/\\(.)/g, (_, c) => (c === 'n' ? '\n' : c === 't' ? '\t' : c));   // only \\ and \" occur

function wlist(src, anchor, what) {
  const m = anchor.exec(src);
  if (!m) throw new Error(`cannot find ${what} in the C++ source (did its layout change? update this script's regex)`);
  const l = [...m[1].matchAll(/L"([a-z0-9]+)"/g)].map((x) => x[1]);
  if (l.length === 0 || new Set(l).size !== l.length) throw new Error(`${what}: empty or duplicated list`);
  return l;
}
function wconst(src, name) {
  const m = new RegExp(`const wchar_t\\* const ${name}\\s*=\\s*L"((?:[^"\\\\]|\\\\.)*)"\\s*;`).exec(src);
  if (!m) throw new Error(`cannot find the constant ${name} in app/TrAssoc.h`);
  return unescapeC(m[1]);
}

// Everything the block needs, read from the C++ sources.
function load() {
  const store = read('app/TrFileStore.h'), assoc = read('app/TrAssoc.h');
  const exts = wlist(store, /mediaExts\(\)\s*\{\s*static const wchar_t\* const k\[\]\s*=\s*\{([\s\S]*?)nullptr\s*\}/, 'TrFileStore.h mediaExts()');
  const audio = wlist(assoc, /isAudioExt\(const std::wstring& e\)\s*\{\s*static const wchar_t\* const a\[\]\s*=\s*\{([\s\S]*?)nullptr\s*\}/, 'TrAssoc.h isAudioExt()');
  for (const a of audio) if (!exts.includes(a)) throw new Error(`isAudioExt lists .${a}, which mediaExts() does not`);
  const suffix = /std::wstring\(kAppName\)\s*\+\s*\(prog == kProgAudio \? L"([^"]*)" : L"([^"]*)"\)/.exec(assoc);
  const cmd = /inline std::wstring openCommand\(const std::wstring& exe\)\s*\{\s*return L"((?:[^"\\]|\\.)*)" \+ exe \+ L"((?:[^"\\]|\\.)*)";\s*\}/.exec(assoc);
  const icon = /const std::wstring icon = exe \+ L"([^"]*)";/.exec(assoc);
  if (!suffix || !cmd || !icon) throw new Error('TrAssoc.h: the friendly-name suffixes, openCommand() or the icon line moved (update this script)');
  return {
    exts, audio,
    appName: wconst(assoc, 'kAppName'), appDesc: wconst(assoc, 'kAppDesc'),
    progVideo: wconst(assoc, 'kProgVideo'), progAudio: wconst(assoc, 'kProgAudio'), vendorKey: wconst(assoc, 'kVendorKey'),
    audioSuffix: suffix[1], videoSuffix: suffix[2],
    cmdPrefix: unescapeC(cmd[1]), cmdSuffix: unescapeC(cmd[2]), iconSuffix: icon[1],
  };
}

const q = (s) => '"' + s.replace(/"/g, '""') + '"';                       // Inno: a quote inside a quoted parameter is doubled
function reg(subkey, o) {
  let s = `Root: HKLM; Subkey: ${q(subkey)}`;
  if (o && o.type) {
    s += `; ValueType: ${o.type}; ValueName: ${q(o.name)}`;
    if (o.type !== 'none') s += `; ValueData: ${q(o.data)}`;
  }
  if (o && o.flags) s += `; Flags: ${o.flags}`;
  return s;
}

function buildBlock(m) {
  const progOf = (ext) => (m.audio.includes(ext) ? m.progAudio : m.progVideo);
  const command = m.cmdPrefix + EXE + m.cmdSuffix;
  const icon = EXE + m.iconSuffix;
  const L = [];
  L.push(BEGIN);
  L.push('; GENERATED by installer\\gen_iss_registry.js from app\\TrFileStore.h (mediaExts) and app\\TrAssoc.h (isAudioExt, the ProgIDs,');
  L.push(';   the names, openCommand): do not edit by hand -- `node installer\\gen_iss_registry.js --write` rewrites it and `--check`');
  L.push(';   fails when the two drift. Windows lets no application make itself the default player: these keys REGISTER SuVtitle');
  L.push(';   machine-wide as a candidate -- in the "Open with" list of every media file type it plays and in Settings > Default');
  L.push(';   apps with its file types. The app\'s own "Set as default player" writes the same per user (HKCU, TrAssoc.h entries())');
  L.push(';   and `--unregister-player` (run by the uninstaller, [Code] CurUninstallStepChanged) removes that. Everything below goes on uninstall');
  L.push(';   (uninsdeletekey / uninsdeletevalue / uninsdeletekeyifempty).');
  // --- what TrAssoc.h entries() writes, in its order ---
  for (const prog of [m.progVideo, m.progAudio]) {
    const c = 'Software\\Classes\\' + prog;
    const friendly = m.appName + (prog === m.progAudio ? m.audioSuffix : m.videoSuffix);
    L.push(reg(c, { type: 'string', name: '', data: friendly, flags: 'uninsdeletekey' }));
    L.push(reg(c, { type: 'string', name: 'FriendlyTypeName', data: friendly }));
    L.push(reg(c + '\\DefaultIcon', { type: 'string', name: '', data: icon }));
    L.push(reg(c + '\\shell\\open', { type: 'string', name: 'FriendlyAppName', data: m.appName }));
    L.push(reg(c + '\\shell\\open\\command', { type: 'string', name: '', data: command }));
  }
  for (const e of m.exts) L.push(reg(`Software\\Classes\\.${e}\\OpenWithProgids`, { type: 'none', name: progOf(e), flags: 'uninsdeletevalue' }));
  const nextale = m.vendorKey.split('\\')[0];
  L.push(reg('Software\\' + nextale, { flags: 'uninsdeletekeyifempty' }));      // AudioPB may share it: only when empty
  const vendor = 'Software\\' + m.vendorKey;
  L.push(reg(vendor, { flags: 'uninsdeletekey' }));
  L.push(reg(vendor + '\\Capabilities', { type: 'string', name: 'ApplicationName', data: m.appName }));
  L.push(reg(vendor + '\\Capabilities', { type: 'string', name: 'ApplicationDescription', data: m.appDesc }));
  L.push(reg(vendor + '\\Capabilities', { type: 'string', name: 'ApplicationIcon', data: icon }));
  for (const e of m.exts) L.push(reg(vendor + '\\Capabilities\\FileAssociations', { type: 'string', name: '.' + e, data: progOf(e) }));
  L.push(reg('Software\\RegisteredApplications', { type: 'string', name: m.appName, data: vendor + '\\Capabilities', flags: 'uninsdeletevalue' }));
  // --- the installer's own addition: Applications\<exe> (not in entries(), flagged here so a diff against it is explained) ---
  const apps = 'Software\\Classes\\Applications\\{#MyAppExe}';
  L.push('; installer-only (TrAssoc.h entries() does not write it): the Applications\\<exe> key, for "Open with > Choose another app".');
  L.push(reg(apps, { type: 'string', name: 'FriendlyAppName', data: m.appName, flags: 'uninsdeletekey' }));
  L.push(reg(apps + '\\DefaultIcon', { type: 'string', name: '', data: icon }));
  L.push(reg(apps + '\\shell\\open', { type: 'string', name: 'FriendlyAppName', data: m.appName }));
  L.push(reg(apps + '\\shell\\open\\command', { type: 'string', name: '', data: command }));
  for (const e of m.exts) L.push(reg(apps + '\\SupportedTypes', { type: 'string', name: '.' + e, data: '' }));
  L.push(END);
  return L;
}

// The models block: ISPP defines read by the .iss (the [Setup] room, the full variant's bundle check, the light variant's
// [Code] list). `models`: [{ name (or file), size, sha256, url }] -- installer/models.json through make_bundle.js
// loadModels(), or the test kit's local list. Every value lands in an ISPP "..." string and a Pascal '...' string: no quote.
function buildModelsBlock(models) {
  if (!Array.isArray(models) || models.length === 0) throw new Error('no models');
  const rows = models.map((x) => ({ name: x.name || x.file, size: x.size, sha256: x.sha256, url: x.url }));
  for (const r of rows) {
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(r.name || '') || !Number.isSafeInteger(r.size) || r.size <= 0
        || !/^[0-9a-f]{64}$/.test(r.sha256 || '') || !/^https?:\/\/[A-Za-z0-9._~:/?#\[\]@!$&()*+,;=%-]+$/.test(r.url || '')
        || /['"]/.test(r.url)) {
      throw new Error(`model ${JSON.stringify(r)}: a value that cannot go into the .iss as it is`);
    }
  }
  const n = rows.length;
  const L = [MBEGIN];
  L.push('; GENERATED by installer\\gen_iss_registry.js from installer\\models.json, the ONE list of the AI models (file, size, SHA-256,');
  L.push(';   the Hugging Face URL pinned to a commit, license): do not edit by hand -- `node installer\\gen_iss_registry.js --write`');
  L.push(';   rewrites it and `--check` fails when the two drift. LIGHT: [Code] downloads from these URLs what {app}\\models lacks and');
  L.push(';   Inno checks each SHA-256; FULL (/DFull): the bundle\'s manifest must list these files with these SHA-256 and sizes.');
  L.push(`#define ModelCount ${n}`);
  L.push(`#define ModelsTotalBytes ${rows.reduce((a, r) => a + r.size, 0)}`);
  for (const arr of ['ModelFile', 'ModelSize', 'ModelSha256', 'ModelUrl']) L.push(`#dim ${arr}[${n}]`);
  rows.forEach((r, i) => {
    L.push(`#define ModelFile[${i}] "${r.name}"`);
    L.push(`#define ModelSize[${i}] "${r.size}"`);
    L.push(`#define ModelSha256[${i}] "${r.sha256}"`);
    L.push(`#define ModelUrl[${i}] "${r.url}"`);
  });
  L.push(MEND);
  return L;
}

// ---------------------------------------------------------------------------------------------------
// --check: the blocks, then the facts the .iss repeats from elsewhere, then both variants compiled
// ---------------------------------------------------------------------------------------------------
function blockRe(begin = BEGIN, end = END) {
  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return new RegExp(esc(begin) + '[\\s\\S]*?' + esc(end));
}

// ISCC.exe: SUVTITLE_ISCC, then PATH, then Inno Setup 6's standard folders (all users, then the current user).
function findIscc() {
  const c = [];
  if (process.env.SUVTITLE_ISCC) c.push(process.env.SUVTITLE_ISCC);
  for (const d of (process.env.PATH || '').split(path.delimiter)) if (d) c.push(path.join(d, 'ISCC.exe'));
  for (const base of [process.env['ProgramFiles(x86)'], process.env.ProgramFiles]) if (base) c.push(path.join(base, 'Inno Setup 6', 'ISCC.exe'));
  if (process.env.LOCALAPPDATA) c.push(path.join(process.env.LOCALAPPDATA, 'Programs', 'Inno Setup 6', 'ISCC.exe'));
  return c.find((p) => { try { return fs.statSync(p).isFile(); } catch (_) { return false; } }) || null;
}

// A file of `size` bytes that takes no room: NTFS sparse (fsutil, no administrator rights needed), then extended. False
// when the volume cannot (FAT32, exFAT...): the file is then removed.
function sparseFile(file, size) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, '');
  const r = cp.spawnSync('fsutil', ['sparse', 'setflag', file], { encoding: 'utf8', windowsHide: true });
  if (r.status !== 0) { fs.rmSync(file, { force: true }); return false; }
  fs.truncateSync(file, size);
  return true;
}

// A throwaway bundle for a compile: every file the .iss checks for (empty), BUILD-Release.txt and a manifest. `models`:
// null = a LIGHT bundle (no app\models); else [{ name, sha256, size, fileSize? }]: app\models\<name>, a sparse file of
// fileSize (default: size) bytes, and its manifest row. Returns null when sparse files cannot be made there.
function fakeBundle(root, files, models) {
  const b = path.join(root, 'SuVtitle');
  const put = (rel, text = '') => { const p = path.join(b, rel); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, text); };
  for (const f of files) put(f);
  put('BUILD-Release.txt', 'a throwaway bundle of gen_iss_registry.js --check\n');
  const rows = ['# SuVtitle - bundle manifest (a throwaway bundle of gen_iss_registry.js --check)'];
  for (const m of models || []) {
    if (!sparseFile(path.join(b, 'app', 'models', m.name), m.fileSize === undefined ? m.size : m.fileSize)) return null;
    rows.push(`${m.sha256.toUpperCase()}\t${m.size}\tapp\\models\\${m.name}`);
  }
  put('manifest.txt', rows.join('\n') + '\n');
  return b;
}

// One compile that writes nothing: /O- (no setup) AND /O<outDir> (ISCC's output folder, a throwaway one: even with /O-,
// ISCC deletes the <name>.exe and <name>-N.bin of its output folder -- measured -- and the .iss's own is installer\Output).
// ISPP still writes the preprocessed script (/DPreprocessedOut).
function compileIss(iscc, bundleDir, full, ppOut, outDir) {
  fs.mkdirSync(outDir, { recursive: true });
  const args = ['/O-', `/O${outDir}`, `/DBundleDir=${bundleDir}`, `/DPreprocessedOut=${ppOut}`, ...(full ? ['/DFull'] : []), ISS];
  const r = cp.spawnSync(iscc, args, { encoding: 'utf8', windowsHide: true, timeout: 180000 });
  const text = ((r.stdout || '') + (r.stderr || '')).replace(/\r\n/g, '\n');
  let pp = null;
  try { pp = fs.readFileSync(ppOut, 'utf8').replace(/\r\n/g, '\n'); } catch (_) { /* refused before the end of the script */ }
  return { ok: r.status === 0, status: r.status, text, pp, error: r.error };
}

// The lines of one [Section] of a preprocessed script.
function section(pp, name) {
  const m = new RegExp('^\\[' + name + '\\]\\n([\\s\\S]*?)(?=^\\[[A-Za-z]+\\]\\s*$|(?![\\s\\S]))', 'm').exec(pp || '');
  return m ? m[1].split('\n') : [];
}

function check(m, issText, opts = {}) {
  const results = [];
  const ok = (name, cond, detail) => results.push({ name, pass: !!cond, detail: cond ? '' : (detail || '') });
  const skip = (name, why) => results.push({ name, pass: true, skipped: true, detail: why });
  const iss = issText.replace(/\r\n/g, '\n');
  // the .iss's own `#define NAME "value"` lines, to read a directive written as {#NAME} the way Inno will
  const defines = {};
  for (const d of iss.matchAll(/^\s*#define (\w+) "([^"]*)"/gm)) if (!(d[1] in defines)) defines[d[1]] = d[2];
  const expand = (v) => (v == null ? v : v.replace(/\{#(\w+)\}/g, (_, n) => (n in defines ? defines[n] : '{#' + n + '}')));

  // A1 the block itself
  const want = buildBlock(m).join('\n');
  const have = (blockRe().exec(iss) || [''])[0];
  let detail = '';
  if (have !== want) {
    const a = want.split('\n'), b = have.split('\n');
    let i = 0;
    while (i < a.length && i < b.length && a[i] === b[i]) i++;
    detail = `first difference at block line ${i + 1}:\n      generated: ${a[i]}\n      in the .iss: ${b[i]}`;
  }
  ok('the [Registry] player block is EXACTLY what the C++ lists generate (' + m.exts.length + ' extensions, ' + m.audio.length + ' audio)', have === want, detail);

  // A2 the version, one truth
  const def = /^#define MyAppVersion "([^"]+)"/m.exec(iss.replace(/^\s+#define/gm, '#define'));
  const rh = /#define SUV_VERSION_STR\s+"([^"]+)"/.exec(read('app/resource.h'));
  const ev = /#define SUVTITLE_ENGINE_VERSION_STR\s+"([^"]+)"/.exec(read('engine/EngineVersion.h'));
  const cm = /project\(SuVtitle\s+VERSION\s+([0-9.]+)/.exec(read('CMakeLists.txt'));
  const versions = { iss: def && def[1], 'app/resource.h': rh && rh[1], 'engine/EngineVersion.h': ev && ev[1], 'CMakeLists.txt': cm && cm[1] };
  ok('version: the .iss default, app/resource.h, engine/EngineVersion.h and CMakeLists.txt all say the same',
     Object.values(versions).every((v) => v && v === versions.iss), JSON.stringify(versions));

  // A3 the single-instance mutex (AppMutex makes Setup / Uninstall refuse to touch a running SuVtitle)
  const mutex = /constexpr wchar_t kMutexName\[\]\s*=\s*L"([^"]+)";/.exec(read('app/main.cpp'));
  const am = /^AppMutex=(.+)$/m.exec(iss);
  ok('AppMutex equals app/main.cpp kMutexName', mutex && am && expand(am[1].trim()) === mutex[1], `main.cpp: ${mutex && mutex[1]}, .iss: ${am && expand(am[1].trim())}`);

  // A4 the exe name
  const outName = /OUTPUT_NAME\s+"([^"]+)"/.exec(read('app/CMakeLists.txt'));
  const exeDef = /^#define MyAppExe "([^"]+)"/m.exec(iss);
  ok('MyAppExe equals app/CMakeLists.txt OUTPUT_NAME + .exe', outName && exeDef && exeDef[1] === outName[1] + '.exe', `${outName && outName[1]} / ${exeDef && exeDef[1]}`);

  // A5 a NEW AppId (never AudioPB's)
  const id = /^#define MyAppId "(\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\})"/m.exec(iss);
  const audiopb = ['{A3F71C28-9D4E-4B6A-8E12-7C5B0F2A9D64}', '{7B2F9A40-3C1E-4E8A-9D5B-6F0A1C2D3E4F}'];   // Translator edition's AppId, driver CLSID
  ok('AppId is a well-formed GUID and none of AudioPB\'s', id && !audiopb.includes(id[1].toUpperCase()), id ? id[1] : 'not found');

  // A6 the command-line switches the .iss uses exist in the app
  const launch = read('app/TrLaunch.h');
  // ISCC 6 refuses runasoriginaluser in [UninstallRun] (measured 23/10/2026), so the uninstaller runs it from [Code].
  ok('--unregister-player is parsed by TrLaunch.h and run by the uninstaller (usUninstall, before any file goes) AS THE ORIGINAL USER (HKCU is theirs, not the elevated account\'s)',
     /x == L"--unregister-player"/.test(launch)
     && /CurUninstallStepChanged[\s\S]{0,400}usUninstall[\s\S]{0,200}ExecAsOriginalUser\(ExpandConstant\('\{app\}\\\{#MyAppExe\}'\), '--unregister-player'/.test(iss)
     && !/^\[UninstallRun\]/m.test(iss),
     'TrLaunch.h / the CurUninstallStepChanged procedure');
  ok('the shell command is --play "%1" (TrLaunch.h parses --play)', /x == L"--play"/.test(launch) && m.cmdSuffix === '" --play "%1"', m.cmdSuffix);

  // A7 setup behaviour
  ok('ChangesAssociations=yes (Explorer re-reads the associations)', /^ChangesAssociations=yes$/m.test(iss));
  // (DiskSpanning: FULL variant only -- the compile checks below read it from each variant's preprocessed script)
  ok('installs to {autopf}\\NEXTALE\\SuVtitle', /^DefaultDirName=\{autopf\}\\NEXTALE\\SuVtitle$/m.test(iss));
  ok('OutputDir=Output is git-ignored (installer/Output/)', /^OutputDir=Output$/m.test(iss) && /^installer\/Output\/$/m.test(read('.gitignore')));

  // A8 the payload layout the engine resolves, and the models it asks for
  const tre = read('engine/TrEngine.cpp'), trh = read('engine/TrEngine.h'), mtr = read('engine/main_translator.cpp');
  const spkH = read('engine/TrSpeaker.h'), spkC = read('engine/TrSpeaker.cpp');
  const layout = /bw = root \+ L"([^"]+)", bl = root \+ L"([^"]+)", bm = root \+ L"([^"]+)"/.exec(tre);
  const leaf = [/pickExe\(cfg\.whisperBin,\s+bw,\s+dw,\s+L"([^"]+)"/.exec(tre), /pickExe\(cfg\.llamaBin,\s+bl,\s+dl,\s+L"([^"]+)"/.exec(tre)];
  const engineModels = [/kTurbo = "([^"]+)"/.exec(mtr), /kMtModelFile\s*=\s*"([^"]+)"/.exec(trh), /kMtModelFileSmall\s*=\s*"([^"]+)"/.exec(trh)];
  const bundle = require(path.join(REPO, 'deploy', 'make_bundle.js'));
  ok('the engine looks in <exe>\\engine\\whisper, \\engine\\llama, \\models for whisper-server.exe / llama-server.exe',
     layout && layout[1] === 'engine\\\\whisper' && layout[2] === 'engine\\\\llama' && layout[3] === 'models' && leaf[0] && leaf[1] && leaf[0][1] === 'whisper-server.exe' && leaf[1][1] === 'llama-server.exe',
     JSON.stringify([layout && layout.slice(1), leaf.map((x) => x && x[1])]));
  // the dev payload the engine reads from SUVTITLE_PAYLOAD is the layout make_bundle.js --payload reads: one payload serves both
  const devLay = /dw = dev \+ L"\\\\([^"]+)", dl = dev \+ L"\\\\([^"]+)", dm = dev \+ L"\\\\([^"]+)"/.exec(tre);
  const mbText = read('deploy/make_bundle.js');
  const payLay = [/const wBin = path\.join\(wRoot, '([^']+)'\)/.exec(mbText), /const lBin = path\.join\(wRoot, '([^']+)'\)/.exec(mbText),
                  /const wModels = path\.join\(wRoot, '([^']+)'\)/.exec(mbText)];
  ok('the dev payload (SUVTITLE_PAYLOAD: the engine) and make_bundle.js --payload have the SAME layout: whisper\\, llama\\, models\\',
     devLay && payLay.every(Boolean) && devLay[1] === 'whisper' && devLay[2] === 'llama' && devLay[3] === 'models'
     && payLay[0][1] === devLay[1] && payLay[1][1] === devLay[2] && payLay[2][1] === devLay[3],
     JSON.stringify([devLay && devLay.slice(1), payLay.map((x) => x && x[1])]));
  // SuVtitle 1.1.0, the LIVE mode: the live presets' ASR models (asrModelsFor: base / small / turbo), the speaker model
  // (kSpkModelFile) and the name its publisher gave it (kSpkModelFileHf) are models the ENGINE asks for too.
  const asrNames = [...tre.matchAll(/AsrModels\{\s*"([^"]+\.bin)",\s*"([^"]+\.bin)"\s*\}/g)].flatMap((x) => [x[1], x[2]]);
  const spkName = /kSpkModelFile\s*=\s*"([^"]+)"/.exec(spkH), spkHf = /kSpkModelFileHf\s*=\s*"([^"]+)"/.exec(tre);
  const wantModels = [...new Set([...engineModels.map((x) => x && x[1]), ...asrNames, spkName && spkName[1]])];
  const issFiles = [...iss.matchAll(/^#if !FileExists\(BundleAbs \+ "\\([^"]+)"\)/gm)].map((x) => x[1]);
  const need = ['app\\SuVtitle.exe', 'app\\SuVtitleEngine.exe', 'app\\web\\index.html', 'app\\engine\\whisper\\whisper-server.exe', 'app\\engine\\llama\\llama-server.exe',
                'app\\engine\\sherpa\\sherpa-onnx-c-api.dll', 'app\\engine\\sherpa\\onnxruntime.dll'];
  ok('the .iss refuses a bundle without the exes, the page, both servers and the sherpa runtime (the models: the checks of installer\\models.json below)',
     need.every((f) => issFiles.includes(f)), 'missing checks: ' + need.filter((f) => !issFiles.includes(f)).join(', '));
  // B -- the models (03/11/2026): installer/models.json is THE list. make_bundle.js reads it (MODELS); a broken file fails here.
  const models = bundle.MODELS || [];
  ok('installer\\models.json is valid: each model has a file name, a size, a SHA-256, a license and a Hugging Face URL pinned to a full commit (https://huggingface.co/<repo>/resolve/<commit>/<path>)',
     !bundle.MODELS_ERROR && models.length > 0 && models.every((x) => x.license && x.source && /^[0-9a-f]{40}$/.test(x.source.commit)),
     bundle.MODELS_ERROR ? bundle.MODELS_ERROR.message : 'no model');
  ok('make_bundle.js takes its models from installer\\models.json (a FULL bundle embeds exactly that list)',
     path.resolve(bundle.MODELS_JSON || '') === path.join(REPO, 'installer', 'models.json') && !/\bconst MODELS = \[/.test(mbText), String(bundle.MODELS_JSON));
  ok('installer\\models.json lists EXACTLY the ' + wantModels.length + ' models the ENGINE asks for (turbo, small, base, the 4B, the 1.5B, the speaker model)',
     engineModels.every(Boolean) && asrNames.length === 6 && spkName && wantModels.length === 6
     && JSON.stringify(models.map((x) => x.name).sort()) === JSON.stringify([...wantModels].sort()),
     JSON.stringify(models.map((x) => x.name)) + ' vs ' + JSON.stringify(wantModels) + ' / asr names ' + asrNames.length);
  const spkEntry = models.find((x) => spkName && x.name === spkName[1]);
  ok('the speaker model is saved under the engine\'s name; the file it is downloaded from is the publisher\'s Hugging Face name (TrEngine.cpp kSpkModelFileHf), which the bundle script also accepts in a payload',
     spkEntry && spkHf && Array.isArray(spkEntry.alt) && spkEntry.alt.length === 1 && spkEntry.alt[0] === spkHf[1]
     && spkEntry.source.path.split('/').pop() === spkHf[1], JSON.stringify([spkEntry && spkEntry.alt, spkHf && spkHf[1]]));
  {
    let want = '', err = '';
    try { want = buildModelsBlock(models).join('\n'); } catch (e) { err = e.message; }
    const have = (blockRe(MBEGIN, MEND).exec(iss) || [''])[0];
    let d = err;
    if (!err && have !== want) {
      const a = want.split('\n'), b2 = have.split('\n');
      let i = 0;
      while (i < a.length && i < b2.length && a[i] === b2[i]) i++;
      d = `first difference at block line ${i + 1}:\n      generated: ${a[i]}\n      in the .iss: ${b2[i]}\n      (node installer\\gen_iss_registry.js --write)`;
    }
    ok('the models block of the .iss is EXACTLY what installer\\models.json generates (' + models.length + ' models)', !err && want && have === want, d);
  }
  // models\ survives an update (the light installer keeps the models in place: no download) and goes on uninstall
  const sect = (name) => ((new RegExp('^\\[' + name + '\\]\\n([\\s\\S]*?)(?=^\\[|(?![\\s\\S]))', 'm').exec(iss) || [])[1] || '').split('\n').filter((l) => /^\s*Type:/.test(l));
  const instDel = sect('InstallDelete'), uninstDel = sect('UninstallDelete');
  ok('[InstallDelete] never touches {app}\\models (nor {app} itself): an update keeps the models in place and downloads nothing',
     instDel.length > 0 && instDel.every((l) => !/Name: "\{app\}(\\models[^"]*|\\\*|)"/i.test(l)), instDel.join(' | '));
  ok('[UninstallDelete] removes {app}\\models whole (the downloaded models are moved in by [Code]: not in the uninstall log)',
     uninstDel.some((l) => /^Type: filesandordirs; Name: "\{app\}\\models"$/.test(l)), uninstDel.join(' | '));
  // the download code: installer\models_download.iss, #included in [Code] for the light variant only (the kit #includes it
  // too). Its BEHAVIOUR is the test kit's (tests\installer); here, that the pieces the kit relies on are where they belong.
  const region = regionText(iss);
  ok('the LIGHT variant\'s [Code] #includes installer\\models_download.iss (under #ifndef Full): Inno\'s download page (CreateDownloadPage), every file with its SHA-256 (Add(url, name, sha256)), on wpReady (before anything is installed), placed at ssPostInstall',
     region && /CreateDownloadPage\(/.test(region) && /MdlPage\.Add\(MdlUrl\[i\], MdlFile\[i\], MdlSha256\[i\]\)/.test(region)
     && /<event\('NextButtonClick'\)>[\s\S]{0,200}CurPageID = wpReady[\s\S]{0,80}MdlFetchMissing/.test(region)
     && /<event\('CurStepChanged'\)>[\s\S]{0,200}ssPostInstall[\s\S]{0,40}MdlPlaceAll/.test(region)
     && /#for \{MdlI = 0; MdlI < ModelCount; MdlI\+\+\} MdlEmitModel/.test(region),
     region ? 'models_download.iss lacks one of: CreateDownloadPage, Add with the SHA-256, the wpReady / ssPostInstall hooks, the models loop'
            : 'no "#ifndef Full" + #include "models_download.iss" + "#endif" in [Code], or the file is missing');
  ok('a model gets its final name in ONE rename of a complete <file>.part (MoveFileExW, REPLACE_EXISTING), a silent failure ends at PrepareToInstall (exit code 7), a model not placed gives exit code 10 (GetCustomSetupExitCode), /VERIFYMODELS re-hashes the models in place',
     region && /'\.part'/.test(region) && /external 'MoveFileExW@kernel32\.dll stdcall'/.test(region)
     && /MdlMoveFileEx\(MdlPart\(i\), MdlFinal\(i\), MdlMoveReplace/.test(region)
     && /<event\('PrepareToInstall'\)>/.test(region) && /<event\('GetCustomSetupExitCode'\)>/.test(region)
     && /MdlExitMissing = 10;/.test(region) && /'\/VERIFYMODELS'/.test(region) && /GetSHA256OfFile\(/.test(region),
     'models_download.iss lacks one of these');
  ok('[Dirs] lets the uninstaller remove {app} (uninsalwaysuninstall): the light installer may create it before installing (a model moved to another drive than %TEMP%)',
     /^\[Dirs\]\n(?:;[^\n]*\n)*Name: "\{app\}"; Flags: uninsalwaysuninstall$/m.test(iss), 'the [Dirs] entry');
  // the sherpa runtime: <exe>\engine\sherpa (bundle) or <payload>\sherpa (dev) for the engine, <payload>\sherpa -> app\engine\sherpa for the bundle script
  const shLay = /root \+ L"(engine\\\\sherpa)" : \(devThere \? dev \+ L"(\\\\sherpa)"/.exec(tre);
  const shProbe = /fileExists\(sd \+ L"\\\\(sherpa-onnx-c-api\.dll)"\)/.exec(tre);
  const shLoad = [/cfg\.binDir \+ L"\\\\(onnxruntime\.dll)"/.exec(spkC), /cfg\.binDir \+ L"\\\\(sherpa-onnx-c-api\.dll)"/.exec(spkC)];
  const sBin = /const sBin = path\.join\(wRoot, '([^']+)'\)/.exec(mbText);
  ok('the sherpa runtime: the engine looks in <exe>\\engine\\sherpa / <payload>\\sherpa and loads sherpa-onnx-c-api.dll + onnxruntime.dll; make_bundle.js ships <payload>\\sherpa to app\\engine\\sherpa with those DLLs (+ the CRT, msvcp140_1 included)',
     shLay && shLay[1] === 'engine\\\\sherpa' && shLay[2] === '\\\\sherpa' && shProbe && shLoad.every(Boolean) && sBin && sBin[1] === 'sherpa'
     && /'app\\\\engine\\\\sherpa\\\\'/.test(mbText)
     && [shProbe[1], shLoad[0] && shLoad[0][1], shLoad[1] && shLoad[1][1]].every((d) => bundle.SHERPA_BIN.includes(d))
     && bundle.SHERPA_CRT_FILES.includes('msvcp140_1.dll'),
     JSON.stringify([shLay && shLay.slice(1), shProbe && shProbe[1], shLoad.map((x) => x && x[1]), sBin && sBin[1], bundle.SHERPA_BIN]));

  // A9 the "SuVtitle Live" Start-menu shortcut (1.1.0): the same exe and icon, --live, which TrLaunch.h parses
  const liveIcon = /^Name: "\{group\}\\\{#MyAppName\} Live"; Filename: "\{app\}\\\{#MyAppExe\}"; Parameters: "--live"; IconFilename: "\{app\}\\\{#MyAppExe\}"/m.test(iss);
  ok('the Start menu has "SuVtitle Live" = SuVtitle.exe --live (same icon) and TrLaunch.h parses --live', liveIcon && /x == L"--live"/.test(launch), 'the [Icons] line / TrLaunch.h');

  // A10 (1.1.0, N3) the native video engine and the Vulkan loader fallback: ONE layout for the bundle, the installer, the app
  // and the engine -- libmpv-2.dll in <app>\mpv\, the loader ONLY in <app>\mpv\vkfallback\
  const mpvA = read('engine/TrMpvAudio.cpp'), mpvH = read('engine/TrMpvAudio.h');
  const appMpv = read('app/MpvPlayer.cpp');
  const engDir = /fileExists\(exe \+ L"\\\\(mpv)\\\\" \+ kDllName\)/.exec(mpvA);
  const engSub = /kVulkanFallbackSubdir\[\] = L"([^"]+)";/.exec(mpvH);
  const appDir = /const std::wstring sub = dir \+ L"\\\\(mpv)";/.exec(appMpv);
  const vkfb = bundle.MPV_DIR + '\\' + (engSub && engSub[1]);
  const issNeed = ['app\\' + bundle.MPV_DIR + '\\libmpv-2.dll', ...bundle.VKFB_FILES.map((v) => 'app\\' + bundle.VKFB_DIR + '\\' + v.name)];
  ok('libmpv: the engine (TrMpvAudio.cpp) and the app (MpvPlayer.cpp) look in <exe>\\mpv\\, make_bundle.js ships it there and the Vulkan loader in mpv\\' + (engSub && engSub[1]) + '\\ (TrMpvAudio.h), the .iss refuses a bundle without them and clears {app}\\mpv on an upgrade',
     engDir && appDir && engSub && engDir[1] === bundle.MPV_DIR && appDir[1] === bundle.MPV_DIR && bundle.VKFB_DIR === vkfb
     && JSON.stringify(bundle.MPV_BIN) === JSON.stringify(['libmpv-2.dll']) && issNeed.every((f) => issFiles.includes(f))
     && /^Type: filesandordirs; Name: "\{app\}\\mpv"$/m.test(iss),
     JSON.stringify([engDir && engDir[1], appDir && appDir[1], engSub && engSub[1], bundle.MPV_DIR, bundle.VKFB_DIR, issNeed.filter((f) => !issFiles.includes(f))]));
  ok('the .iss no longer tells a PC without vulkan-1.dll to install a driver (the bundle ships the loader)',
     !/MsgBox\('Note: vulkan-1\.dll was not found/.test(iss), 'the old CurStepChanged note is still there');

  // C -- both variants COMPILED (ISCC /O- into a throwaway output folder: nothing written) against throwaway bundles, and
  // their preprocessed scripts read. A FULL bundle's models are sparse files of the pinned sizes (no disk space taken).
  const C = [
    'LIGHT compiles: ONE Setup.exe (no DiskSpanning) named SuVtitle_v<version>_Setup, no model in [Files], ExtraDiskSpaceRequired = the models\' bytes, and its [Code] downloads EXACTLY installer\\models.json (file, URL, SHA-256, size, in order)',
    'LIGHT refuses a FULL bundle (one with app\\models): the light installer can never embed the models by accident',
    'FULL (/DFull) compiles: SuVtitle_v<version>_Setup_Full + -1.bin (DiskSpanning), no download code, from a bundle whose models have the pinned sizes and whose manifest lists the models.json bytes',
    'FULL refuses a bundle whose manifest lists OTHER bytes for a model (another SHA-256)',
    'FULL refuses a model file whose size on disk is not the pinned one, even when its manifest row is (a file replaced after the assembly)',
    'FULL refuses a LIGHT bundle (no models)',
    'the compiles left the output folders alone (installer\\Output untouched)',
  ];
  if (opts.compile === false) { for (const c of C) skip(c, '--no-compile'); return results; }
  const iscc = findIscc();
  if (!iscc) {
    ok('ISCC.exe (Inno Setup 6) found, to compile both variants', false, 'not found: set SUVTITLE_ISCC, put it on PATH, or pass --no-compile (the compile checks are then SKIPPED)');
    return results;
  }
  const version = defines.MyAppVersion;
  const total = models.reduce((a, x) => a + x.size, 0);
  const realOut = path.join(REPO, 'installer', 'Output');
  const outBefore = listDir(realOut);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'suvtitle-isscheck-'));
  const out = path.join(tmp, 'out');
  try {
    const nonModel = issFiles.filter((f) => !/^app\\models\\/i.test(f));
    const lightB = fakeBundle(path.join(tmp, 'light'), nonModel, null);
    const fullB = fakeBundle(path.join(tmp, 'full'), nonModel, models);
    const otherB = fakeBundle(path.join(tmp, 'other'), nonModel,
      models.map((x, i) => (i === models.length - 1 ? { ...x, sha256: x.sha256.replace(/^./, (c) => (c === '0' ? '1' : '0')) } : x)));
    const shortB = fakeBundle(path.join(tmp, 'short'), nonModel, models.map((x, i) => (i === 0 ? { ...x, fileSize: x.size - 1 } : x)));
    if (!fullB || !otherB || !shortB) {
      for (const c of C.slice(1, 5)) skip(c, 'no sparse file in ' + tmp + ' (fsutil sparse setflag failed): the full bundle\'s 6 GB are not written for a check');
    }
    const L = compileIss(iscc, lightB, false, path.join(tmp, 'light.iss'), out);
    {
      const setup = section(L.pp, 'Setup'), files = section(L.pp, 'Files').filter((l) => /^Source:/.test(l)), code = section(L.pp, 'Code');
      const adds = code.map((l) => /^\s*MdlAdd\('([^']*)', '([^']*)', '([^']*)', '([^']*)'\);$/.exec(l)).filter(Boolean).map((x) => x.slice(1, 5));
      const wantAdds = models.map((x) => [x.name, x.url, x.sha256, String(x.size)]);
      const problems = [];
      if (!L.ok) problems.push('ISCC failed (' + L.status + '): ' + L.text.split('\n').filter((l) => /error|line \d+/i.test(l)).slice(-3).join(' | '));
      if (!setup.includes(`OutputBaseFilename=SuVtitle_v${version}_Setup`)) problems.push('OutputBaseFilename');
      if (setup.some((l) => /^DiskSpanning=yes/i.test(l))) problems.push('DiskSpanning=yes');
      if (!setup.includes(`ExtraDiskSpaceRequired=${total}`)) problems.push('ExtraDiskSpaceRequired');
      if (files.length !== 1 || !/\\app\\\*"; DestDir: "\{app\}"; Flags: ignoreversion recursesubdirs createallsubdirs$/.test(files[0]) || files.some((l) => /models/i.test(l))) problems.push('[Files]: ' + files.join(' | '));
      if (JSON.stringify(adds) !== JSON.stringify(wantAdds)) problems.push('the [Code] model list: ' + JSON.stringify(adds));
      ok(C[0], problems.length === 0, problems.join('; '));
    }
    if (fullB && otherB && shortB) {
      const LF = compileIss(iscc, fullB, false, path.join(tmp, 'light-on-full.iss'), out);
      ok(C[1], !LF.ok && /it is a FULL bundle/.test(LF.text), LF.ok ? 'it compiled' : LF.text.split('\n').slice(-3).join(' | '));
      const F = compileIss(iscc, fullB, true, path.join(tmp, 'full.iss'), out);
      {
        const setup = section(F.pp, 'Setup'), code = section(F.pp, 'Code').join('\n');
        const problems = [];
        if (!F.ok) problems.push('ISCC failed (' + F.status + '): ' + F.text.split('\n').filter((l) => /error|line \d+/i.test(l)).slice(-3).join(' | '));
        if (!setup.includes(`OutputBaseFilename=SuVtitle_v${version}_Setup_Full`)) problems.push('OutputBaseFilename');
        if (!setup.includes('DiskSpanning=yes')) problems.push('no DiskSpanning=yes');
        if (setup.some((l) => /^ExtraDiskSpaceRequired=/.test(l))) problems.push('ExtraDiskSpaceRequired');
        if (/MdlFetchMissing|CreateDownloadPage/.test(code)) problems.push('the download code is in it');
        ok(C[2], problems.length === 0, problems.join('; '));
      }
      const FO = compileIss(iscc, otherB, true, path.join(tmp, 'full-other.iss'), out);
      ok(C[3], !FO.ok && /is not the file installer\\models\.json pins/.test(FO.text), FO.ok ? 'it compiled' : FO.text.split('\n').slice(-3).join(' | '));
      const FS = compileIss(iscc, shortB, true, path.join(tmp, 'full-short.iss'), out);
      ok(C[4], !FS.ok && /bytes; installer\\models\.json pins/.test(FS.text), FS.ok ? 'it compiled' : FS.text.split('\n').slice(-3).join(' | '));
    }
    const FL = compileIss(iscc, lightB, true, path.join(tmp, 'full-on-light.iss'), out);
    ok(C[5], !FL.ok && /is not in the bundle's app\\models/.test(FL.text), FL.ok ? 'it compiled' : FL.text.split('\n').slice(-3).join(' | '));
    const outAfter = listDir(realOut);
    ok(C[6], JSON.stringify(outAfter) === JSON.stringify(outBefore), `before: ${outBefore.join(', ')} / after: ${outAfter.join(', ')}`);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true, maxRetries: 6, retryDelay: 100 });
  }
  return results;
}

// The names, sizes and times of a folder's files (none when it does not exist).
function listDir(dir) {
  try {
    return fs.readdirSync(dir).sort().map((n) => { const s = fs.statSync(path.join(dir, n)); return `${n}:${s.size}:${Math.round(s.mtimeMs)}`; });
  } catch (_) { return []; }
}

// The light installer's download code: the text of installer\models_download.iss when the .iss #includes it in [Code] for
// the light variant only (#ifndef Full ... #endif); else null.
function regionText(issText) {
  const iss = issText.replace(/\r\n/g, '\n');
  const code = iss.search(/^\[Code\]$/m);
  const inc = /^#ifndef Full\n(?:\/\/[^\n]*\n)*#include "models_download\.iss"\n#endif$/m.exec(iss);
  if (code < 0 || !inc || inc.index < code) return null;
  try { return fs.readFileSync(REGION_FILE, 'utf8').replace(/\r\n/g, '\n'); } catch (_) { return null; }
}

// ---------------------------------------------------------------------------------------------------
function main(argv) {
  const mode = argv[0] || '--print';
  const flags = argv.slice(1);
  const usage = 'usage: node installer\\gen_iss_registry.js [--print | --write | --check [--no-compile]]';
  if (!['--print', '--write', '--check'].includes(mode) || flags.some((f) => !(mode === '--check' && f === '--no-compile'))) { console.error(usage); return 2; }
  const m = load();
  const block = buildBlock(m);
  const bundle = require(path.join(REPO, 'deploy', 'make_bundle.js'));
  if (bundle.MODELS_ERROR && mode !== '--check') { console.error('XX installer\\models.json: ' + bundle.MODELS_ERROR.message); return 1; }
  const mblock = bundle.MODELS ? buildModelsBlock(bundle.MODELS) : [];
  if (mode === '--print') { console.log(block.join('\n') + '\n\n' + mblock.join('\n')); return 0; }
  const txt = fs.readFileSync(ISS, 'utf8');
  if (mode === '--write') {
    for (const [b, e] of [[BEGIN, END], [MBEGIN, MEND]]) {
      if (!blockRe(b, e).test(txt)) { console.error(`the markers\n  ${b}\n  ${e}\nare not in ${ISS}`); return 1; }
    }
    const eol = txt.includes('\r\n') ? '\r\n' : '\n';                  // keep the file's own line endings (.gitattributes: *.iss = CRLF)
    const out = txt.replace(blockRe(), () => block.join(eol)).replace(blockRe(MBEGIN, MEND), () => mblock.join(eol));
    if (out === txt) { console.log('blocks already up to date.'); return 0; }
    fs.writeFileSync(ISS, out);
    console.log(`blocks rewritten: registration ${block.length} lines (${m.exts.length} extensions, ${m.audio.length} audio), models ${mblock.length} lines (${bundle.MODELS.length} models).`);
    return 0;
  }
  const res = check(m, txt, { compile: !flags.includes('--no-compile') });
  for (const r of res) console.log(`  ${r.skipped ? 'SKIP' : r.pass ? 'PASS' : 'FAIL'}  ${r.name}${r.pass && !r.skipped ? '' : '\n      ' + r.detail}`);
  const bad = res.filter((r) => !r.pass).length, skipped = res.filter((r) => r.skipped).length;
  console.log(bad ? `\n${bad} of ${res.length} checks FAILED` : `\n${res.length - skipped} checks passed` + (skipped ? `, ${skipped} SKIPPED (--no-compile)` : ''));
  return bad ? 1 : 0;
}

module.exports = { load, buildBlock, buildModelsBlock, regionText, check, findIscc, BEGIN, END, MBEGIN, MEND, REGION_FILE };
if (require.main === module) {
  try { process.exitCode = main(process.argv.slice(2)); } catch (e) { console.error('XX ' + e.message); process.exitCode = 1; }
}

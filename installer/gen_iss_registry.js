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
// Usage (from anywhere):
//   node installer\gen_iss_registry.js            print the block
//   node installer\gen_iss_registry.js --write    replace the block in SuVtitle_Setup.iss (line endings kept)
//   node installer\gen_iss_registry.js --check    exit 1 unless the .iss holds EXACTLY the generated block AND the other facts
//                                                 the .iss repeats from the C++ / CMake sources still agree (version, mutex,
//                                                 exe name, payload layout, model names, command-line switches)
// =============================================================================

const fs = require('fs');
const path = require('path');

const REPO = path.resolve(__dirname, '..');
const ISS = path.join(REPO, 'installer', 'SuVtitle_Setup.iss');
const BEGIN = '; >>> player registration (generated) >>>';
const END = '; <<< player registration (generated) <<<';
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

// ---------------------------------------------------------------------------------------------------
// --check: the block, then the facts the .iss repeats from elsewhere
// ---------------------------------------------------------------------------------------------------
function blockRe() {
  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return new RegExp(esc(BEGIN) + '[\\s\\S]*?' + esc(END));
}

function check(m, issText) {
  const results = [];
  const ok = (name, cond, detail) => results.push({ name, pass: !!cond, detail: cond ? '' : (detail || '') });
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
  ok('DiskSpanning=yes (the bundle is far above the 4.2 GB limit of one Setup.exe)', /^DiskSpanning=yes$/m.test(iss));
  ok('installs to {autopf}\\NEXTALE\\SuVtitle', /^DefaultDirName=\{autopf\}\\NEXTALE\\SuVtitle$/m.test(iss));
  ok('OutputDir=Output is git-ignored (installer/Output/)', /^OutputDir=Output$/m.test(iss) && /^installer\/Output\/$/m.test(read('.gitignore')));

  // A8 the payload layout the engine resolves, and the models it asks for
  const tre = read('engine/TrEngine.cpp'), trh = read('engine/TrEngine.h'), mtr = read('engine/main_translator.cpp');
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
  const issFiles = [...iss.matchAll(/^#if !FileExists\(BundleAbs \+ "\\([^"]+)"\)/gm)].map((x) => x[1]);
  const need = ['app\\SuVtitle.exe', 'app\\SuVtitleEngine.exe', 'app\\web\\index.html', 'app\\engine\\whisper\\whisper-server.exe', 'app\\engine\\llama\\llama-server.exe',
                ...engineModels.map((x) => 'app\\models\\' + (x && x[1]))];
  ok('the .iss refuses a bundle without the exes, the page, both servers and the 3 models the ENGINE asks for',
     engineModels.every(Boolean) && need.every((f) => issFiles.includes(f)), 'missing checks: ' + need.filter((f) => !issFiles.includes(f)).join(', '));
  ok('make_bundle.js ships exactly those 3 models (turbo, the 4B, the 1.5B)',
     engineModels.every(Boolean) && JSON.stringify(bundle.MODELS.map((x) => x.name).sort()) === JSON.stringify(engineModels.map((x) => x[1]).sort()), JSON.stringify(bundle.MODELS.map((x) => x.name)));

  return results;
}

// ---------------------------------------------------------------------------------------------------
function main(argv) {
  const mode = argv[0] || '--print';
  if (!['--print', '--write', '--check'].includes(mode)) { console.error('usage: node installer\\gen_iss_registry.js [--print | --write | --check]'); return 2; }
  const m = load();
  const block = buildBlock(m);
  if (mode === '--print') { console.log(block.join('\n')); return 0; }
  const txt = fs.readFileSync(ISS, 'utf8');
  if (mode === '--write') {
    if (!blockRe().test(txt)) { console.error(`the markers\n  ${BEGIN}\n  ${END}\nare not in ${ISS}`); return 1; }
    const eol = txt.includes('\r\n') ? '\r\n' : '\n';                  // keep the file's own line endings (.gitattributes: *.iss = CRLF)
    const out = txt.replace(blockRe(), () => block.join(eol));
    if (out === txt) { console.log('block already up to date.'); return 0; }
    fs.writeFileSync(ISS, out);
    console.log(`block rewritten: ${block.length} lines (${m.exts.length} extensions, ${m.audio.length} audio).`);
    return 0;
  }
  const res = check(m, txt);
  for (const r of res) console.log(`  ${r.pass ? 'PASS' : 'FAIL'}  ${r.name}${r.pass ? '' : '\n      ' + r.detail}`);
  const bad = res.filter((r) => !r.pass).length;
  console.log(bad ? `\n${bad} of ${res.length} checks FAILED` : `\n${res.length} checks passed`);
  return bad ? 1 : 0;
}

module.exports = { load, buildBlock, check, BEGIN, END };
if (require.main === module) {
  try { process.exitCode = main(process.argv.slice(2)); } catch (e) { console.error('XX ' + e.message); process.exitCode = 1; }
}

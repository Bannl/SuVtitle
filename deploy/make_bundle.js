#!/usr/bin/env node
'use strict';
// =============================================================================
// SuVtitle - deploy/make_bundle.js
//
// Assembles the DEPLOYABLE FOLDER of SuVtitle: the one thing installer/SuVtitle_Setup.iss packs.
//
//   <out>\SuVtitle\                      default <repo>\deploy\out\SuVtitle (git-ignored)
//     manifest.txt                       size + SHA-256 of every file below (this script's own certificate)
//     BUILD-<type>.txt                   which build the binaries came from; the installer refuses anything but Release
//     app\                               -> becomes {app} = %ProgramFiles%\NEXTALE\SuVtitle
//       SuVtitle.exe, SuVtitleEngine.exe, WebView2Loader.dll        (from the build folder)
//       web\                                                         (from the build folder; SuVtitle.exe serves <exe>\web)
//       engine\whisper\   whisper-server + ggml (Vulkan + CPU) + the VC++ CRT       <- <payload>\whisper
//       engine\llama\     llama-server + ggml + every CPU backend + the CRT         <- <payload>\llama
//       models\           ggml-large-v3-turbo.bin, Qwen3-4B-...-Q5_K_M.gguf, qwen2.5-1.5b-...-q4_k_m.gguf   <- <payload>\models
//
// THE PAYLOAD is YOURS to supply: a folder (--payload <dir>, or the environment variable SUVTITLE_PAYLOAD) holding the Vulkan
// builds of whisper.cpp's whisper-server and llama.cpp's llama-server with their DLLs in whisper\ and llama\, and the models in
// models\. It is the same layout SuVtitleEngine.exe reads from SUVTITLE_PAYLOAD when nothing is staged next to it, so a payload
// that works for a dev run is the payload to bundle. Nothing of it is in this repository and no default points anywhere.
//
// That layout is what the engine looks for (engine/TrEngine.cpp resolvePathsAt: <exe dir>\engine\whisper, \engine\llama,
// \models). Ported 2026-10-23 from AudioPB's deploy/make_translator_bundle.ps1 (commit 641f1aa) -- same measured file lists,
// same CRT trick, same dumpbin closure, same refresh mode, same manifest -- minus everything that only the LIVE translator
// needs (sherpa-onnx, the speaker model, the small / base ASR models, the driver, the tray). It is node and not a new .ps1
// on purpose: antivirus products can quarantine a freshly written .ps1 file.
//
// WHAT IS SHIPPED AND WHY (director, 23/10/2026; the numbers were measured with dumpbin in AudioPB, not guessed):
//   whisper : ggml.dll STATICALLY imports ggml-cpu.dll AND ggml-vulkan.dll, and ggml-vulkan.dll imports vulkan-1.dll HARD:
//             without a Vulkan loader on the PC whisper-server.exe does not start at all, the CPU fallback included
//             (the installer says so). ggml-cpu.dll also imports VCOMP140.
//   llama   : ggml.dll imports only ggml-base.dll and LOADS its backends at run time by name (ggml_backend_load_all), so the
//             static closure is INCOMPLETE: ggml-vulkan.dll and every ggml-cpu-*.dll (one per instruction set; ggml picks the
//             best for the PC) must be there, or llama-server runs with NO backend. Vulkan absent = a clean CPU fallback.
//   models  : the turbo ASR model, and BOTH translation models -- the engine falls back to the 1.5B SILENTLY when the 4B
//             is absent, so a bundle without the 4B would translate with the small model and never say so.
//             NOT shipped: ggml-small / ggml-base (they were the LIVE pair; a FILE job reads them only when turbo is NOT
//             installed), sherpa + the speaker model (a file job tells no speakers apart), ggml-large-v3.bin (quality "max"
//             only; the engine says so when it is absent; --with-large-v3 puts it in).
//   CRT     : whisper and llama are /MD (MSVCP140, VCRUNTIME140, VCRUNTIME140_1, VCOMP140). SuVtitle's own exes are /MT
//             and need none. The CRT DLLs are copied NEXT TO each payload exe (app-local deployment, supported by
//             Microsoft) so a clean PC without the VC++ Redistributable still works.
//
// The checks that make the bundle trustworthy (each one fails loudly, none is cosmetic):
//   1. every source exists BEFORE anything is copied (the whole list of what is missing is printed);
//   2. the build folder is not older than the sources (warning), its web\ equals app\web (warning), its type is known;
//   3. refresh mode: a file with the same size AND the same mtime is not copied again; a file in app\ that the plan does
//      not know (a leftover of an older layout) is removed;
//   4. every .exe / .dll of the bundle is x64 (PE header), and no import library / debug file is in it;
//   5. dumpbin dependency CLOSURE per folder (dumpbin from the Visual Studio found by vswhere, or SUVTITLE_VS): every DLL an exe or DLL imports is either in its
//      folder or a Windows component; backends loaded by name (ggml-cpu-*.dll) are treated as roots;
//   6. manifest.txt: SHA-256 of everything (--verify re-checks a bundle against it).
//
// Usage: node deploy\make_bundle.js [options]      (node deploy\make_bundle.js --help)
// Exit code: 0 = bundle certified; 1 = a check failed or a source is missing; 2 = bad command line.
// =============================================================================

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const cp = require('child_process');

// -----------------------------------------------------------------------------
// The payload (AudioPB's lists @641f1aa, sections 4-8 of make_translator_bundle.ps1)
// -----------------------------------------------------------------------------
const WHISPER_BIN = ['whisper-server.exe', 'whisper.dll', 'ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll', 'ggml-vulkan.dll'];
const LLAMA_BIN = ['llama-server.exe', 'llama-server-impl.dll', 'llama.dll', 'llama-common.dll', 'mtmd.dll',
                   'ggml.dll', 'ggml-base.dll', 'libomp.dll', 'ggml-vulkan.dll'];
// libomp.dll's license text: shipped beside it when the source folder has it (a warning, never an error, when it does not).
const LLAMA_EXTRA = ['LICENSE-LLVM-OpenMP'];
// The CRT: what each /MD payload exe needs, MEASURED (dumpbin) -- the closure check below is the guard if llama.cpp
// ever imports one more (msvcp140_1.dll, concrt140.dll ...): it names the missing DLL, it is added here.
const CRT_FILES = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'];
const OMP_FILES = ['vcomp140.dll'];                       // whisper's ggml-cpu.dll only (llama uses its own libomp.dll)
const MODELS = [
  { name: 'ggml-large-v3-turbo.bin',            why: 'transcription (FILE quality auto / fast)' },
  { name: 'Qwen3-4B-Instruct-2507-Q5_K_M.gguf', why: 'translation (the engine default)' },
  { name: 'qwen2.5-1.5b-instruct-q4_k_m.gguf',  why: 'translation fallback (preset light; used when the 4B is absent)' },
];
const MODEL_LARGE_V3 = { name: 'ggml-large-v3.bin', why: 'transcription, quality "max" only (optional)' };

// What Windows itself provides, never us. Whole base names (a prefix match would let "mfc140u.dll" slip through as "mf").
// AudioPB's list + what SuVtitle's own exes and the Release build can import (Media Foundation is DELAY-loaded by the engine).
const SYSTEM_DLLS = new Set([
  'kernel32', 'kernelbase', 'ntdll', 'user32', 'advapi32', 'shell32', 'shlwapi', 'ole32', 'oleaut32', 'combase', 'ws2_32',
  'crypt32', 'psapi', 'setupapi', 'dbghelp', 'dxgi', 'winmm', 'avrt', 'winhttp', 'mmdevapi', 'gdi32', 'version', 'bcrypt',
  'rpcrt4', 'sechost', 'imm32', 'comdlg32', 'uxtheme', 'dwmapi', 'd3d11', 'd3d12', 'elscore', 'mf', 'mfplat', 'mfreadwrite',
  'comctl32', 'shcore', 'ucrtbase', 'msvcrt', 'wininet', 'iphlpapi', 'userenv', 'secur32', 'wtsapi32', 'netapi32', 'win32u',
  'gdi32full', 'msimg32', 'ncrypt', 'wintrust', 'cfgmgr32', 'powrprof', 'propsys', 'urlmon', 'windowscodecs', 'dcomp',
  'd2d1', 'dwrite',
]);
// Provided by the GPU driver, not by Windows and not by us: reported, accepted (the installer warns when it is absent).
const DRIVER_DLLS = new Set(['vulkan-1']);

// Never in a shipped folder.
const FORBIDDEN_EXT = ['.lib', '.exp', '.ilk', '.pdb', '.obj', '.pch', '.idb'];

const IMAGE_FILE_MACHINE_AMD64 = 0x8664;

// -----------------------------------------------------------------------------
// Console, retry, small file helpers
// -----------------------------------------------------------------------------
const warnings = [];
function out(s) {
  try { fs.writeSync(1, s + '\n'); } catch (_) { console.log(s); }      // synchronous: progress shows during long sync work
}
function say(m) { out(m); }
function step(m) { out(''); out('== ' + m); }
function warn(m) { warnings.push(m); out('!! ' + m); }
class BundleError extends Error {}
function fail(m) { throw new BundleError(m); }

function human(b) {
  if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GiB';
  if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MiB';
  if (b >= 1024) return Math.round(b / 1024) + ' KiB';
  return b + ' B';
}
const num = (n) => Number(n).toLocaleString('en-US');
const localStamp = (d) => { const p = (n) => String(n).padStart(2, '0'); return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`; };

function sleepMs(ms) { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms); }

// Bitdefender keeps a file that was JUST written in its scan window for a few tens of ms: opening, renaming or deleting it
// then fails with a sharing violation (machine note, 2026-10). Anything that touches a fresh file goes through here.
const TRANSIENT = new Set(['EBUSY', 'EPERM', 'EACCES', 'UNKNOWN']);
function retry(what, fn, tries = 6, delayMs = 250) {
  let last;
  for (let i = 1; i <= tries; i++) {
    try { return fn(); } catch (e) {
      last = e;
      if (!TRANSIENT.has(e.code)) throw e;
      if (i < tries) sleepMs(delayMs);
    }
  }
  fail(`${what} failed after ${tries} tries: ${last.message}`);
}

function isFile(p) { try { return fs.statSync(p).isFile(); } catch (_) { return false; } }
function isDir(p) { try { return fs.statSync(p).isDirectory(); } catch (_) { return false; } }

// All files below dir, as paths relative to it (backslash separated), in directory order.
function walk(dir, rel = '') {
  const res = [];
  for (const e of fs.readdirSync(rel ? path.join(dir, rel) : dir, { withFileTypes: true })) {
    const r = rel ? rel + '\\' + e.name : e.name;
    if (e.isDirectory()) res.push(...walk(dir, r));
    else if (e.isFile()) res.push(r);
  }
  return res;
}

function sha256File(file) {
  return retry('hash of ' + file, () => {
    const h = crypto.createHash('sha256');
    const fd = fs.openSync(file, 'r');
    try {
      const buf = Buffer.allocUnsafe(8 << 20);
      let n;
      while ((n = fs.readSync(fd, buf, 0, buf.length, null)) > 0) h.update(buf.subarray(0, n));
    } finally { fs.closeSync(fd); }
    return h.digest('hex').toUpperCase();
  });
}

// IMAGE_FILE_HEADER.Machine of a PE file, or null when it is not a PE.
function peMachine(file) {
  return retry('PE header of ' + file, () => {
    const fd = fs.openSync(file, 'r');
    try {
      const head = Buffer.alloc(64);
      if (fs.readSync(fd, head, 0, 64, 0) < 64 || head.readUInt16LE(0) !== 0x5a4d) return null;     // "MZ"
      const peOff = head.readUInt32LE(0x3c);
      const sig = Buffer.alloc(6);
      if (fs.readSync(fd, sig, 0, 6, peOff) < 6 || sig.readUInt32LE(0) !== 0x00004550) return null;   // "PE\0\0"
      return sig.readUInt16LE(4);
    } finally { fs.closeSync(fd); }
  });
}

// "14.52.37025" > "14.9.0": compare the numeric parts, not the text.
function cmpVersionDesc(a, b) {
  const pa = a.split('.').map(Number), pb = b.split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pb[i] || 0) - (pa[i] || 0);
    if (d) return d;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// Tool discovery
// -----------------------------------------------------------------------------
// The Visual Studio installs to search (dumpbin, the redistributable CRT), best first: SUVTITLE_VS (an install root, the
// same variable tools\vsenv.cmd honours), then vswhere's answer -- the released channels first, then the preview ones (vswhere
// hides those without -prerelease) -- then the usual %ProgramFiles% locations. No path of any machine is built in.
let vsRootsCache = null;
function vsInstallRoots() {
  if (vsRootsCache) return vsRootsCache;
  const roots = [];
  const add = (p) => { if (p && !roots.some((r) => r.toLowerCase() === p.toLowerCase())) roots.push(p); };
  add(process.env['SUVTITLE_VS']);
  const pf = process.env['ProgramFiles'], pf86 = process.env['ProgramFiles(x86)'];
  for (const base of [pf86, pf]) {
    if (!base) continue;
    const vsw = path.join(base, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
    if (!isFile(vsw)) continue;
    for (const extra of [[], ['-prerelease']]) {
      const r = cp.spawnSync(vsw, ['-latest', ...extra, '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                   '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true });
      if (r.status === 0) for (const l of (r.stdout || '').split(/\r?\n/)) add(l.trim());
    }
    break;
  }
  for (const base of [pf, pf86]) {
    if (!base) continue;
    for (const year of ['2026', '2022', '2019']) {
      for (const ed of ['Community', 'Professional', 'Enterprise', 'BuildTools']) add(path.join(base, 'Microsoft Visual Studio', year, ed));
    }
  }
  return (vsRootsCache = roots);
}
function vsRoots(sub) { return vsInstallRoots().map((r) => path.join(r, sub)); }

function findDumpbin() {
  for (const root of vsRoots('VC\\Tools\\MSVC')) {
    if (!isDir(root)) continue;
    const vers = fs.readdirSync(root, { withFileTypes: true }).filter((d) => d.isDirectory() && /^\d+(\.\d+)+$/.test(d.name))
      .map((d) => d.name).sort(cmpVersionDesc);
    for (const v of vers) {
      const p = path.join(root, v, 'bin', 'Hostx64', 'x64', 'dumpbin.exe');
      if (isFile(p)) return p;
    }
  }
  return null;
}

// Microsoft.VC*.CRT / .OpenMP below a Redist\MSVC root: the newest x64 one (not onecore, not debug_nonredist) -- AudioPB's rule.
function findDirsBelow(root, re, maxDepth) {
  const res = [];
  (function rec(dir, depth) {
    let ents;
    try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const e of ents) {
      if (!e.isDirectory()) continue;
      const p = path.join(dir, e.name);
      if (re.test(e.name)) res.push(p);
      else if (depth < maxDepth) rec(p, depth + 1);
    }
  })(root, 0);
  return res;
}
function pickNewest(dirs) {
  const ok = dirs.filter((d) => /\\x64\\/i.test(d + '\\') && !/\\(onecore|debug_nonredist)\\/i.test(d + '\\'));
  ok.sort((a, b) => {
    const va = (/\\(\d+(?:\.\d+)+)\\/.exec(a) || [0, '0'])[1], vb = (/\\(\d+(?:\.\d+)+)\\/.exec(b) || [0, '0'])[1];
    return cmpVersionDesc(va, vb) || (a < b ? 1 : -1);
  });
  return ok[0] || null;
}
function findVcRedist(override) {
  const roots = override ? [override] : vsRoots('VC\\Redist\\MSVC');
  for (const r of roots) {
    if (!isDir(r)) continue;
    const crt = pickNewest(findDirsBelow(r, /^Microsoft\.VC\d+\.CRT$/i, 4));
    if (!crt) continue;
    return { crt, omp: pickNewest(findDirsBelow(r, /^Microsoft\.VC\d+\.OpenMP$/i, 4)) };
  }
  return null;
}

// "Release" / "Debug" from the build folder's CMake cache (tools\buildlock.cmd passes -DCMAKE_BUILD_TYPE), else from its name.
function buildTypeOf(buildDir) {
  try {
    const m = /^CMAKE_BUILD_TYPE:STRING=(.*?)\r?$/m.exec(fs.readFileSync(path.join(buildDir, 'CMakeCache.txt'), 'utf8'));
    if (m && m[1].trim()) return { type: m[1].trim(), from: 'CMakeCache.txt' };
  } catch (_) { /* no cache: a copied folder */ }
  const n = path.basename(buildDir).toLowerCase();
  if (n.includes('release')) return { type: 'Release', from: 'folder name' };
  if (n.includes('debug')) return { type: 'Debug', from: 'folder name' };
  return { type: 'Unknown', from: 'nothing to go by' };
}

function git(repo, args) {
  try {
    const r = cp.spawnSync('git', ['-C', repo, ...args], { encoding: 'utf8', windowsHide: true });
    return r.status === 0 ? r.stdout.trim() : null;
  } catch (_) { return null; }
}

// -----------------------------------------------------------------------------
// dumpbin dependency closure
// -----------------------------------------------------------------------------
// "Image has the following dependencies:" and "Image has the following delay load dependencies:" (the engine delay-loads
// Media Foundation) both list DLL names, one per line, up to "Summary".
function parseDumpbinDependents(text) {
  const deps = [];
  let inList = false;
  for (const raw of text.split(/\r?\n/)) {
    const t = raw.trim();
    if (/^Image has the following (delay load )?dependencies/i.test(t)) { inList = true; continue; }
    if (!inList) continue;
    if (/^Summary/i.test(t)) { inList = false; continue; }
    if (/^[A-Za-z0-9_\-.+]+\.dll$/i.test(t)) deps.push(t);
  }
  return deps;
}

function isSystemDll(name) {
  const n = name.toLowerCase();
  if (/^(api|ext)-ms-win-.*\.dll$/.test(n)) return true;                // the umbrella forwarders of the Universal CRT / Win32 API sets
  return n.endsWith('.dll') && SYSTEM_DLLS.has(n.slice(0, -4));
}
function isDriverDll(name) {
  const n = name.toLowerCase();
  return n.endsWith('.dll') && DRIVER_DLLS.has(n.slice(0, -4));
}

function dependentsOf(dumpbin, file) {
  const r = cp.spawnSync(dumpbin, ['-nologo', '-dependents', file], { encoding: 'utf8', windowsHide: true, maxBuffer: 16 << 20 });
  if (r.error) fail(`dumpbin could not be started: ${r.error.message}`);
  if (r.status !== 0) fail(`dumpbin failed on ${file}: ${(r.stdout || '').trim().split(/\r?\n/).slice(-3).join(' | ')}`);
  return parseDumpbinDependents(r.stdout);
}

function isCrtDll(name) { return /^(msvcp140.*|vcruntime140.*|vcomp140|concrt140|vccorlib140)\.dll$/i.test(name); }

// groups: [{ name, dir, seeds: [file names] }]. Returns { checked, bad: [string], driver: Set, assumed: Set, analysed: Set of lower-case paths }.
// opts.assumeCrt (--skip-vcruntime): the VC++ Redistributable is assumed on the target PC, so its DLLs are not required in the folders.
function checkDependencies(dumpbin, groups, opts = {}) {
  const bad = [];
  const driver = new Set();
  const assumed = new Set();
  const analysed = new Set();
  let checked = 0;
  for (const g of groups) {
    const seen = new Set();
    const queue = [...g.seeds];
    while (queue.length) {
      const name = queue.shift();
      if (seen.has(name.toLowerCase())) continue;
      seen.add(name.toLowerCase());
      const p = path.join(g.dir, name);
      if (!isFile(p)) {
        if (isDriverDll(name)) driver.add(name.toLowerCase());
        else if (opts.assumeCrt && isCrtDll(name)) assumed.add(name.toLowerCase());
        else if (!isSystemDll(name)) bad.push(`${g.name}: ${name} MISSING (imported, not in the folder, not a Windows component)`);
        continue;
      }
      checked++;
      analysed.add(p.toLowerCase());
      for (const d of dependentsOf(dumpbin, p)) queue.push(d);
    }
  }
  return { checked, bad, driver, assumed, analysed };
}

// -----------------------------------------------------------------------------
// The manifest: "SHA256<TAB>bytes<TAB>path relative to the bundle folder"
// -----------------------------------------------------------------------------
function readManifest(bundle) {
  const p = path.join(bundle, 'manifest.txt');
  if (!isFile(p)) fail(`no manifest.txt in ${bundle} (assemble the bundle first)`);
  const rows = [];
  for (const line of fs.readFileSync(p, 'utf8').split(/\r?\n/)) {
    if (!line || line.startsWith('#')) continue;
    const f = line.split('\t');
    if (f.length !== 3 || !/^[0-9A-F]{64}$/.test(f[0]) || !/^\d+$/.test(f[1])) fail(`manifest.txt: malformed line "${line}"`);
    rows.push({ hash: f[0], size: Number(f[1]), rel: f[2] });
  }
  return rows;
}

function verifyBundle(bundle) {
  step('Verify ' + bundle);
  const rows = readManifest(bundle);
  const problems = [];
  const known = new Set();
  for (const r of rows) {
    known.add(r.rel.toLowerCase());
    const p = path.join(bundle, r.rel);
    if (!isFile(p)) { problems.push(`missing: ${r.rel}`); continue; }
    const st = fs.statSync(p);
    if (st.size !== r.size) { problems.push(`size ${num(st.size)} != ${num(r.size)}: ${r.rel}`); continue; }
    if (st.size >= 100 * 1048576) say(`   ... hash ${r.rel} (${human(st.size)})`);
    if (sha256File(p) !== r.hash) problems.push(`SHA-256 differs: ${r.rel}`);
  }
  for (const rel of walk(bundle)) {
    if (rel.toLowerCase() === 'manifest.txt') continue;
    if (!known.has(rel.toLowerCase())) problems.push(`not in the manifest: ${rel}`);
  }
  if (problems.length) {
    for (const p of problems) say('   ' + p);
    fail(`${problems.length} difference(s) between the bundle and its manifest.txt`);
  }
  say(`   OK: ${rows.length} files match their size and SHA-256.`);
  return 0;
}

// -----------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------
const USAGE = [
  'Usage: node deploy\\make_bundle.js [options]',
  '  --build <dir>         take SuVtitle.exe, SuVtitleEngine.exe, WebView2Loader.dll and web\\ from this build folder',
  '                        (default <repo>\\build\\x64-Release; e.g. build\\x64-Debug for a structure test)',
  '  --out <dir>           output root; the bundle is <dir>\\SuVtitle (default <repo>\\deploy\\out)',
  '  --payload <dir>       the AI payload: whisper\\ (whisper-server + DLLs), llama\\ (llama-server + DLLs), models\\ (the model files);',
  '                        default: the environment variable SUVTITLE_PAYLOAD. No built-in default: not needed for --verify / --help.',
  '  --vcredist <dir>      a VC++ Redist\\MSVC folder to search for the CRT DLLs (default: the Visual Studio found by vswhere, or SUVTITLE_VS)',
  '  --skip-models         do not copy the models: a STRUCTURE TEST only, the installer refuses such a bundle',
  '  --with-large-v3       also ship ggml-large-v3.bin (3 GB; quality "max" only; not shipped by default)',
  '  --skip-vcruntime      do not copy the CRT DLLs: the target PC is then ASSUMED to have the VC++ Redistributable',
  '                        (the dependency check accepts those imports and says so)',
  '  --no-verify-deps      skip the dumpbin dependency check',
  '  --clean               delete the existing bundle folder first (otherwise: refresh)',
  '  --dry-run             check the sources and print the plan, copy nothing',
  '  --verify              re-hash an assembled bundle against its manifest.txt, then exit',
  '  --help',
].join('\n');

function parseArgs(argv) {
  const o = { out: null, build: null, payload: process.env['SUVTITLE_PAYLOAD'] || null, vcredist: null, clean: false, skipModels: false,
              withLargeV3: false, skipVcRuntime: false, noVerifyDeps: false, dryRun: false, verify: false, help: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => { if (i + 1 >= argv.length) { const e = new BundleError(`${a} needs a value`); e.usage = true; throw e; } return argv[++i]; };
    switch (a) {
      case '--build': o.build = val(); break;
      case '--out': o.out = val(); break;
      case '--payload': o.payload = val(); break;
      case '--vcredist': o.vcredist = val(); break;
      case '--skip-models': o.skipModels = true; break;
      case '--with-large-v3': o.withLargeV3 = true; break;
      case '--skip-vcruntime': o.skipVcRuntime = true; break;
      case '--no-verify-deps': o.noVerifyDeps = true; break;
      case '--clean': o.clean = true; break;
      case '--dry-run': o.dryRun = true; break;
      case '--verify': o.verify = true; break;
      case '--help': case '-h': case '/?': o.help = true; break;
      default: { const e = new BundleError(`unknown option ${a}`); e.usage = true; throw e; }
    }
  }
  return o;
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
function newestSource(root, exts, skipDirs) {
  let best = null;
  (function rec(dir) {
    let ents;
    try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const e of ents) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) { if (!skipDirs.includes(e.name.toLowerCase())) rec(p); continue; }
      if (!e.isFile() || !exts.includes(path.extname(e.name).toLowerCase())) continue;
      const m = fs.statSync(p).mtimeMs;
      if (!best || m > best.mtimeMs) best = { file: p, mtimeMs: m };
    }
  })(root);
  return best;
}

function main(argv) {
  const o = parseArgs(argv);
  if (o.help) { say(USAGE); return 0; }

  const repo = path.resolve(__dirname, '..');
  const outRoot = path.resolve(o.out || path.join(repo, 'deploy', 'out'));
  const bundle = path.join(outRoot, 'SuVtitle');
  const appDir = path.join(bundle, 'app');
  if (o.verify) return verifyBundle(bundle);

  const buildDir = path.resolve(o.build || path.join(repo, 'build', 'x64-Release'));
  if (!o.payload) {
    const e = new BundleError('no payload: pass --payload <dir> or set SUVTITLE_PAYLOAD to a folder holding whisper\\ (whisper-server + its DLLs), llama\\ (llama-server + its DLLs) and models\\.');
    e.usage = true;
    throw e;
  }
  const wRoot = path.resolve(o.payload);
  const wBin = path.join(wRoot, 'whisper');
  const lBin = path.join(wRoot, 'llama');
  const wModels = path.join(wRoot, 'models');
  const bt = buildTypeOf(buildDir);

  say('SuVtitle - bundle assembly');
  say(`  repo      : ${repo}`);
  say(`  build     : ${buildDir}  [${bt.type}, from ${bt.from}]`);
  say(`  payload   : ${wRoot}`);
  say(`  bundle    : ${bundle}`);
  if (bt.type !== 'Release') warn(`the build folder is "${bt.type}": this bundle is NOT shippable (the installer refuses it unless told /DAllowDebugBundle).`);

  // --- the VC++ CRT ---------------------------------------------------------
  let crt = null;
  if (!o.skipVcRuntime) {
    crt = findVcRedist(o.vcredist);
    if (crt) say(`  CRT       : ${crt.crt}${crt.omp ? '' : '   (no OpenMP folder)'}`);
    else fail('the VC++ x64 redistributable (Microsoft.VC*.CRT) was NOT found. Pass --vcredist <...\\Redist\\MSVC> or --skip-vcruntime.');
  } else warn('--skip-vcruntime: the CRT DLLs are NOT bundled; the target PC needs the VC++ 2015-2022 x64 Redistributable.');

  // --- THE PLAN: one table, source -> path relative to the bundle folder ------
  const plan = [];
  const add = (src, rel) => plan.push({ src, rel });
  for (const n of ['SuVtitle.exe', 'SuVtitleEngine.exe', 'WebView2Loader.dll']) add(path.join(buildDir, n), 'app\\' + n);
  const webDir = path.join(buildDir, 'web');
  const webFiles = isDir(webDir) ? walk(webDir) : [];
  for (const r of webFiles) add(path.join(webDir, r), 'app\\web\\' + r);
  for (const n of WHISPER_BIN) add(path.join(wBin, n), 'app\\engine\\whisper\\' + n);
  for (const n of LLAMA_BIN) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
  // The CPU backends llama loads by name: ALL of them (about 18 MB; the PC this runs on is not known in advance).
  const cpuVariants = isDir(lBin) ? fs.readdirSync(lBin).filter((f) => /^ggml-cpu-.+\.dll$/i.test(f)).sort() : [];
  for (const n of cpuVariants) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
  for (const n of LLAMA_EXTRA) {
    if (isFile(path.join(lBin, n))) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
    else warn(`${n} (the license of libomp.dll) is not next to the llama binaries; not shipped.`);
  }
  if (crt) {
    for (const n of CRT_FILES) { add(path.join(crt.crt, n), 'app\\engine\\whisper\\' + n); add(path.join(crt.crt, n), 'app\\engine\\llama\\' + n); }
    if (crt.omp) for (const n of OMP_FILES) add(path.join(crt.omp, n), 'app\\engine\\whisper\\' + n);
    else warn('vcomp140.dll not found: whisper\'s ggml-cpu.dll imports it, whisper fails on a PC without the VC++ Redistributable.');
  }
  const modelList = o.skipModels ? [] : [...MODELS, ...(o.withLargeV3 ? [MODEL_LARGE_V3] : [])];
  for (const m of modelList) add(path.join(wModels, m.name), 'app\\models\\' + m.name);
  if (o.skipModels) warn('--skip-models: the models (about 5.6 GB) are NOT copied. INCOMPLETE bundle, a structure test only.');
  if (o.skipModels && o.withLargeV3) warn('--with-large-v3 ignored under --skip-models.');
  if (cpuVariants.length === 0) warn('no ggml-cpu-*.dll next to the llama binaries: the CPU fallback of llama-server would be DEAD.');

  // --- 1. sources, ALL of them, before anything is touched ------------------------
  step(`Checking ${plan.length} sources`);
  const missing = plan.filter((e) => !isFile(e.src)).map((e) => e.src);
  if (!isDir(webDir)) missing.push(webDir + '   (the build folder has no web\\ - build the suvtitle_web target)');
  else if (!webFiles.some((r) => r.toLowerCase() === 'index.html')) missing.push(path.join(webDir, 'index.html'));
  if (missing.length) {
    say(`   ${missing.length} source(s) MISSING:`);
    for (const m of missing) say('     ' + m);
    fail('sources missing - nothing was copied.');
  }
  let srcBytes = 0;
  for (const e of plan) srcBytes += fs.statSync(e.src).size;
  say(`   OK. ${plan.length} files, ${human(srcBytes)} to place.`);

  // --- 2. freshness: a binary older than its sources is a bundle that lies -------------
  step('Freshness of the build');
  const checkFresh = (exe, dirs, exts) => {
    const bin = path.join(buildDir, exe);
    const bm = fs.statSync(bin).mtimeMs;
    let newest = null;
    for (const d of dirs) {
      const n = newestSource(d, exts, ['web', 'scratchpad', 'build', 'work', 'tests']);
      if (n && (!newest || n.mtimeMs > newest.mtimeMs)) newest = n;
    }
    if (newest && newest.mtimeMs > bm + 2000) {
      warn(`${exe} (${localStamp(new Date(bm))}) is OLDER than ${path.relative(repo, newest.file)} (${localStamp(new Date(newest.mtimeMs))}): rebuild before shipping.`);
    } else say(`   ${exe}: not older than its sources.`);
  };
  checkFresh('SuVtitle.exe', [path.join(repo, 'app'), path.join(repo, 'common')], ['.cpp', '.h', '.rc', '.ico']);
  checkFresh('SuVtitleEngine.exe', [path.join(repo, 'engine'), path.join(repo, 'common')], ['.cpp', '.h', '.rc']);
  {
    // web\: the build folder's copy must BE app\web (CMake copies it per file; a stale one ships the previous page).
    const srcWeb = path.join(repo, 'app', 'web');
    const diffs = [];
    if (isDir(srcWeb)) {
      const sw = walk(srcWeb), bw = new Set(webFiles.map((f) => f.toLowerCase()));
      for (const r of sw) {
        if (!bw.has(r.toLowerCase())) { diffs.push(`${r} (not in the build's web\\)`); continue; }
        const a = path.join(srcWeb, r), b = path.join(webDir, r);
        if (fs.statSync(a).size !== fs.statSync(b).size || sha256File(a) !== sha256File(b)) diffs.push(`${r} (differs)`);
      }
      const sset = new Set(sw.map((f) => f.toLowerCase()));
      for (const r of webFiles) if (!sset.has(r.toLowerCase())) diffs.push(`${r} (only in the build's web\\)`);
    }
    if (diffs.length) warn(`the build's web\\ is not app\\web (${diffs.join('; ')}): rebuild so the page that ships is the page in the tree.`);
    else say('   web\\: identical to app\\web.');
  }

  // --- --dry-run: the plan, nothing touched ---------------------------------------------
  if (o.dryRun) {
    step('Plan (dry run, nothing copied)');
    for (const e of plan) say(`   ${e.rel}   <-  ${e.src}`);
    say(`   ${plan.length} files, ${human(srcBytes)}.`);
    return finish(0);
  }

  // --- 3. copy (refresh mode) ----------------------------------------------------------
  if (o.clean && isDir(bundle)) {
    step('--clean: removing the existing bundle');
    if (path.basename(bundle).toLowerCase() !== 'suvtitle' || path.dirname(bundle) === bundle) fail(`refusing to clean ${bundle}`);
    fs.rmSync(bundle, { recursive: true, force: true, maxRetries: 6, retryDelay: 250 });
  }
  step('Copy');
  fs.mkdirSync(appDir, { recursive: true });
  const todo = [];
  for (const e of plan) {
    const s = fs.statSync(e.src);
    let upToDate = false;
    try {
      const d = fs.statSync(path.join(bundle, e.rel));
      upToDate = d.isFile() && d.size === s.size && Math.abs(d.mtimeMs - s.mtimeMs) < 2000;
    } catch (_) { /* not there yet */ }
    if (!upToDate) todo.push({ e, s });
  }
  const need = todo.reduce((a, t) => a + t.s.size, 0);
  try {
    const sf = fs.statfsSync(bundle);
    const free = Number(sf.bavail) * Number(sf.bsize);
    if (free < need + 256 * 1048576) fail(`not enough free space on the output drive: ${human(need)} to copy (+256 MiB margin), ${human(free)} free.`);
  } catch (e) { if (e instanceof BundleError) throw e; warn('free space could not be read: ' + e.message); }
  let copiedBytes = 0;
  for (const t of todo) {
    const dst = path.join(bundle, t.e.rel);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    if (t.s.size >= 50 * 1048576) say(`   ... ${t.e.rel} (${human(t.s.size)})`);
    retry('copy of ' + t.e.rel, () => {
      try { fs.unlinkSync(dst); } catch (err) { if (err.code !== 'ENOENT') throw err; }
      fs.copyFileSync(t.e.src, dst);
    });
    retry('time stamp of ' + t.e.rel, () => fs.utimesSync(dst, t.s.atime, t.s.mtime));   // refresh mode compares size + mtime
    if (fs.statSync(dst).size !== t.s.size) fail(`${t.e.rel}: ${num(fs.statSync(dst).size)} bytes after the copy, ${num(t.s.size)} expected (disk full?).`);
    copiedBytes += t.s.size;
  }
  say(`   ${todo.length} copied (${human(copiedBytes)}), ${plan.length - todo.length} already up to date.`);

  // --- leftovers of an older layout (a dropped DLL, the old sherpa folder ...) must not ship ----
  {
    const planned = new Set(plan.map((e) => e.rel.toLowerCase()));
    const orphans = walk(appDir).map((r) => 'app\\' + r).filter((r) => !planned.has(r.toLowerCase()));
    for (const r of orphans) {
      say(`   removing a file the plan does not know: ${r}`);
      retry('removal of ' + r, () => fs.unlinkSync(path.join(bundle, r)));
    }
    const pruneEmpty = (dir) => {
      for (const e of fs.readdirSync(dir, { withFileTypes: true })) if (e.isDirectory()) pruneEmpty(path.join(dir, e.name));
      if (dir !== appDir && fs.readdirSync(dir).length === 0) fs.rmdirSync(dir);
    };
    pruneEmpty(appDir);
    if (orphans.length) say(`   ${orphans.length} leftover file(s) removed.`);
  }
  for (const r of walk(bundle)) {                                  // outside app\: reported, never touched
    const l = r.toLowerCase();
    if (l === 'manifest.txt' || l.startsWith('app\\') || /^build-[a-z0-9_.-]+\.txt$/.test(l)) continue;
    warn(`unexpected file in the bundle root (kept, listed in the manifest, not installed): ${r}`);
  }

  // --- 4. the payload is all there, x64, clean -----------------------------------------
  step('Payload checks');
  for (const m of modelList) {
    const p = path.join(appDir, 'models', m.name);
    if (!isFile(p) || fs.statSync(p).size !== fs.statSync(path.join(wModels, m.name)).size) fail(`model ${m.name} is not (completely) in the bundle - not certified.`);
  }
  if (!o.skipModels) say(`   ${modelList.length} models in place (${modelList.map((m) => m.name).join(', ')}).`);
  const binaries = walk(appDir).filter((r) => /\.(exe|dll)$/i.test(r));
  const notX64 = [];
  for (const r of binaries) {
    const mach = peMachine(path.join(appDir, r));
    if (mach !== IMAGE_FILE_MACHINE_AMD64) notX64.push(`${r} (${mach === null ? 'not a PE file' : 'machine 0x' + mach.toString(16)})`);
  }
  const forbidden = walk(appDir).filter((r) => FORBIDDEN_EXT.includes(path.extname(r).toLowerCase()));
  if (notX64.length || forbidden.length) {
    for (const n of notX64) say('   NOT x64: ' + n);
    for (const f of forbidden) say('   forbidden in a shipped folder: ' + f);
    fail('payload checks failed.');
  }
  say(`   ${binaries.length} executables / DLLs, all x64 (PE machine 0x8664); no import library, no debug file.`);

  // --- 5. the dependency closure ----------------------------------------------------------
  let depVerdict = 'not run (--no-verify-deps)';
  if (!o.noVerifyDeps) {
    step('Dependency closure (dumpbin)');
    const dumpbin = findDumpbin();
    if (!dumpbin) {
      warn('dumpbin.exe not found (Visual Studio): the dependency check was SKIPPED.');
      depVerdict = 'skipped (no dumpbin)';
    } else {
      say('   dumpbin : ' + dumpbin);
      const llamaDir = path.join(appDir, 'engine', 'llama');
      const groups = [
        { name: 'app', dir: appDir, seeds: ['SuVtitle.exe', 'SuVtitleEngine.exe', 'WebView2Loader.dll'] },
        { name: 'engine\\whisper', dir: path.join(appDir, 'engine', 'whisper'), seeds: ['whisper-server.exe'] },
        // llama's backends are loaded BY NAME at run time: roots, or the static closure misses them completely.
        { name: 'engine\\llama', dir: llamaDir, seeds: ['llama-server.exe', 'ggml-vulkan.dll',
          ...fs.readdirSync(llamaDir).filter((f) => /^ggml-cpu-.+\.dll$/i.test(f))] },
      ];
      const r = checkDependencies(dumpbin, groups, { assumeCrt: o.skipVcRuntime });
      if (r.bad.length) {
        say(`   ${r.bad.length} dependency problem(s):`);
        for (const b of r.bad) say('     ' + b);
        fail('dependency closure INCOMPLETE - bundle not certified.');
      }
      // A binary no seed reaches would ship UNVERIFIED (a stray DLL the plan added by mistake): say so.
      const unreached = binaries.filter((b) => !r.analysed.has(path.join(appDir, b).toLowerCase()));
      for (const b of unreached) warn(`${b} is in the bundle but no dependency chain reaches it: it was NOT analysed.`);
      depVerdict = `OK (${r.checked} binaries analysed, closure complete` +
                   (r.driver.size ? `; from the GPU driver: ${[...r.driver].join(', ')}` : '') +
                   (r.assumed.size ? `; ASSUMED on the target PC (--skip-vcruntime): ${[...r.assumed].join(', ')}` : '') + ')';
      say('   ' + depVerdict);
    }
  }

  // --- 6. BUILD marker + manifest -----------------------------------------------------------
  step('manifest.txt (SHA-256 of each file)');
  for (const f of fs.readdirSync(bundle)) if (/^BUILD-.*\.txt$/i.test(f)) retry('removal of ' + f, () => fs.unlinkSync(path.join(bundle, f)));
  const typeToken = bt.type.replace(/[^A-Za-z0-9_.-]/g, '_');
  const stamp = new Date();
  const commit = git(repo, ['rev-parse', '--short', 'HEAD']);
  const branch = git(repo, ['symbolic-ref', '--short', 'HEAD']);
  const gitLine = commit ? `${branch || '?'} @ ${commit}` : `${branch || '?'} (no commit yet)`;
  fs.writeFileSync(path.join(bundle, `BUILD-${typeToken}.txt`),
    `SuVtitle bundle built from a ${bt.type} build (${bt.from}): ${buildDir}\nassembled ${localStamp(stamp)}\n` +
    'The installer (installer\\SuVtitle_Setup.iss) packs this bundle only when this file is BUILD-Release.txt.\n');

  const rels = walk(bundle).filter((r) => r.toLowerCase() !== 'manifest.txt').sort((a, b) => (a.toLowerCase() < b.toLowerCase() ? -1 : 1));
  const rows = [];
  let total = 0;
  for (const r of rels) {
    const p = path.join(bundle, r);
    const sz = fs.statSync(p).size;
    if (sz >= 100 * 1048576) say(`   ... hash ${r} (${human(sz)})`);
    rows.push(`${sha256File(p)}\t${sz}\t${r}`);
    total += sz;
  }
  const head = [
    '# SuVtitle - bundle manifest',
    `# assembled ${localStamp(stamp)}`,
    `# build: ${bt.type} (${bt.from}) -- ${buildDir}`,
    `# git: ${gitLine}`,
    `# models: ${o.skipModels ? 'NOT INCLUDED (--skip-models: structure test, incomplete)' : modelList.map((m) => m.name).join(', ')}`,
    `# ${rows.length} files, ${total} bytes (${human(total)})`,
    `# dependencies: ${depVerdict}`,
    '# format: SHA256<TAB>bytes<TAB>path relative to this folder',
  ];
  const mp = path.join(bundle, 'manifest.txt');
  fs.writeFileSync(mp, head.concat(rows).join('\n') + '\n');
  say(`   ${rows.length} files hashed.`);

  // --- summary ---------------------------------------------------------------------------------
  step('Summary');
  say(`   bundle    : ${bundle}`);
  say(`   build     : ${bt.type}${bt.type === 'Release' ? '' : '   <-- NOT shippable'}`);
  say(`   files     : ${rows.length} (+ manifest.txt)`);
  say(`   size      : ${human(total)} (${num(total)} bytes)`);
  const groupsOut = [
    ['app (exes + web)', (r) => /^app\\[^\\]+$/i.test(r) || /^app\\web\\/i.test(r)],
    ['app\\engine\\whisper', (r) => /^app\\engine\\whisper\\/i.test(r)],
    ['app\\engine\\llama', (r) => /^app\\engine\\llama\\/i.test(r)],
    ['app\\models', (r) => /^app\\models\\/i.test(r)],
  ];
  for (const [n, f] of groupsOut) {
    const grp = rels.filter(f);
    const sz = grp.reduce((a, r) => a + fs.statSync(path.join(bundle, r)).size, 0);
    say(`   ${n.padEnd(22)} ${String(grp.length).padStart(3)} files  ${human(sz).padStart(10)}`);
  }
  say(`   deps      : ${depVerdict}`);
  if (o.skipModels) say('   REMINDER  : assembled WITHOUT the models (--skip-models); the installer will refuse it.');
  return finish(0);
}

function finish(code) {
  if (warnings.length) {
    out('');
    out(`${warnings.length} warning(s):`);
    for (const w of warnings) out('   !! ' + w);
  }
  return code;
}

module.exports = { MODELS, parseDumpbinDependents, isSystemDll, isDriverDll, isCrtDll, peMachine, buildTypeOf, checkDependencies, cmpVersionDesc, walk, SYSTEM_DLLS };

if (require.main === module) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (e) {
    if (e instanceof BundleError) {
      out('');
      out('XX ' + e.message);
      if (e.usage) { out(''); out(USAGE); }
      finish(1);
      process.exitCode = e.usage ? 2 : 1;
    } else {
      out('');
      out('XX unexpected error: ' + (e && e.stack || e));
      process.exitCode = 1;
    }
  }
}

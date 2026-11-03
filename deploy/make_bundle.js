#!/usr/bin/env node
'use strict';
// =============================================================================
// SuVtitle - deploy/make_bundle.js
//
// Assembles the DEPLOYABLE FOLDER of SuVtitle: the one thing installer/SuVtitle_Setup.iss packs. Two kinds (03/11/2026):
//   LIGHT (the default, for releases): no models\ -- the light installer downloads the AI models of installer/models.json
//         at install time, the missing ones only. ISCC installer\SuVtitle_Setup.iss.
//   FULL  (--full): the models embedded, for the offline installer (ISCC /DFull ...). They must be the bytes models.json
//         pins: their size is checked before the copy, their SHA-256 when the manifest hashes them.
//
//   <out>\SuVtitle\                      default <repo>\deploy\out\SuVtitle (git-ignored; a full one: --out deploy\out\full)
//     manifest.txt                       size + SHA-256 of every file below (this script's own certificate; removed while
//                                        the bundle is being re-assembled, so a failed run leaves an uninstallable bundle)
//     BUILD-<type>.txt                   which build the binaries came from; the installer refuses anything but Release
//     app\                               -> becomes {app} = %ProgramFiles%\NEXTALE\SuVtitle
//       SuVtitle.exe, SuVtitleEngine.exe, WebView2Loader.dll        (from the build folder)
//       web\                                                         (from the build folder; SuVtitle.exe serves <exe>\web)
//       engine\whisper\   whisper-server + ggml (Vulkan + CPU) + the VC++ CRT       <- <payload>\whisper
//       engine\llama\     llama-server + ggml + every CPU backend + the CRT         <- <payload>\llama
//       engine\sherpa\    sherpa-onnx + ONNX Runtime (the LIVE speaker model's runtime) + the CRT      <- <payload>\sherpa
//       mpv\libmpv-2.dll  the native video engine (1.1.0: it plays every file) and the analysis' decoder  <- --mpv / SUVTITLE_MPV_DIR
//       mpv\vkfallback\   vulkan-1.dll + VulkanRT-License.txt: Khronos' Vulkan loader (LunarG VulkanRT), used ONLY on a PC
//                         that has none                                       <- --vulkan-rt / SUVTITLE_VULKAN_RT (x64\vulkan-1.dll)
//       models\           FULL bundles only: the files of installer/models.json -- ggml-large-v3-turbo.bin, ggml-small.bin,
//                         ggml-base.bin, Qwen3-4B-...-Q5_K_M.gguf, qwen2.5-1.5b-...-q4_k_m.gguf,
//                         3dspeaker_campplus_zh_en_advanced.onnx                                        <- <payload>\models
//
// THE PAYLOAD is YOURS to supply: a folder (--payload <dir>, or the environment variable SUVTITLE_PAYLOAD) holding the Vulkan
// builds of whisper.cpp's whisper-server and llama.cpp's llama-server with their DLLs in whisper\ and llama\, the sherpa-onnx C
// API runtime (sherpa-onnx-c-api.dll, onnxruntime.dll, onnxruntime_providers_shared.dll) in sherpa\, and the models in models\
// (read by --full only).
// It is the same layout SuVtitleEngine.exe reads from SUVTITLE_PAYLOAD when nothing is staged next to it, so a payload that
// works for a dev run is the payload to bundle. Nothing of it is in this repository and no default points anywhere.
// The speaker model is accepted under the name its publisher gave it on Hugging Face
// (3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx, the same bytes) and shipped under the engine's own name.
//
// That layout is what the engine looks for (engine/TrEngine.cpp resolvePathsAt: <exe dir>\engine\whisper, \engine\llama,
// \engine\sherpa, \models). Ported 2026-10-23 from AudioPB's deploy/make_translator_bundle.ps1 (commit 641f1aa) -- same measured
// file lists, same CRT trick, same dumpbin closure, same refresh mode, same manifest; SuVtitle 1.1.0 (the LIVE mode) puts back
// what only the live translator needs: sherpa-onnx + the speaker model, and the small / base ASR models (not the driver, not the
// tray). It is node and not a new .ps1 on purpose: antivirus products can quarantine a freshly written .ps1 file.
//
// WHAT IS SHIPPED AND WHY (director, 23/10/2026 and 26/10/2026; the numbers were measured with dumpbin, not guessed):
//   whisper : ggml.dll STATICALLY imports ggml-cpu.dll AND ggml-vulkan.dll, and ggml-vulkan.dll imports vulkan-1.dll HARD:
//             without a Vulkan loader on the PC whisper-server.exe does not start at all, the CPU fallback included
//             (the installer says so). ggml-cpu.dll also imports VCOMP140.
//   llama   : ggml.dll imports only ggml-base.dll and LOADS its backends at run time by name (ggml_backend_load_all), so the
//             static closure is INCOMPLETE: ggml-vulkan.dll and every ggml-cpu-*.dll (one per instruction set; ggml picks the
//             best for the PC) must be there, or llama-server runs with NO backend. Vulkan absent = a clean CPU fallback.
//   sherpa  : the speaker model's runtime (the Live mode, and a file transcript's voice pass). sherpa-onnx-c-api.dll imports
//             onnxruntime.dll; onnxruntime_providers_shared.dll is loaded BY NAME by ONNX Runtime when a provider is asked
//             for, so it is a root of the closure check (like llama's backends). The engine loads them itself, from this
//             folder, ONNX Runtime FIRST (engine/TrSpeaker.cpp: recent Windows builds ship another onnxruntime.dll in System32).
//             A bundle without them would name every live line by its strip and make every transcript without speakers,
//             and never say why: it is refused.
//   models  : the turbo ASR model, and BOTH translation models -- the engine falls back to the 1.5B SILENTLY when the 4B
//             is absent, so a bundle without the 4B would translate with the small model and never say so. The LIVE mode adds
//             ggml-small.bin and ggml-base.bin (the live presets: balanced = small/small, light = base/base, quality = small
//             draft + turbo final; a FILE job also uses them when turbo is NOT installed) and the speaker model
//             (CAM++, ~27 MB). The live preset falls back to what is installed at or below the one asked, so a missing
//             small would degrade the live mode quietly: they are all required -- in a FULL bundle; the LIGHT installer
//             downloads them (installer/models.json is the one list: name, size, SHA-256, pinned Hugging Face URL, license).
//             NOT shipped: ggml-large-v3.bin (quality "max" only; the engine says so when it is absent; --full --with-large-v3
//             puts it in a full bundle).
//   mpv     : libmpv-2.dll (1.1.0, docs NATIVE_ENGINE_PLAN): ONE self-contained DLL (FFmpeg, libplacebo, libass... linked in;
//             shinchiro's GPL build, THIRD_PARTY_NOTICES.md says where its sources are), loaded at run time by SuVtitle.exe
//             (app\MpvPlayer.cpp) AND SuVtitleEngine.exe (engine\TrMpvAudio.cpp: the analysis' sound) from <app>\mpv\. Its
//             imports are Windows components -- and vulkan-1.dll, HARD (measured, dumpbin), which only a graphics driver installs.
//   vkfallback: THE VULKAN LOADER for a PC without one (the user's requirement 26/10: any computer, no graphics card needed).
//             libmpv and whisper-server cannot even START without vulkan-1.dll; the bundle ships Khronos' loader (LunarG's
//             VulkanRT redistributable, signed "LunarG, Inc.") in its OWN folder, mpv\vkfallback\ -- NEVER in app\ or mpv\ (a
//             copy there would be found BEFORE System32's and beat the PC's own loader): the app and the engine load it by
//             full path only when System32 has none, and append the folder to the AI servers' PATH (searched after System32).
//             With no driver behind it, it lists no device: libmpv plays with Direct3D 11, the servers run on the CPU.
//             The bundle refuses a vulkan-1.dll anywhere else.
//   CRT     : whisper, llama and sherpa-onnx are /MD (MSVCP140, VCRUNTIME140, VCRUNTIME140_1, VCOMP140; ONNX Runtime also
//             MSVCP140_1). SuVtitle's own exes are /MT and need none. The CRT DLLs are copied NEXT TO each payload exe /
//             DLL set (app-local deployment, supported by Microsoft) so a clean PC without the VC++ Redistributable still
//             works; they come from the Visual Studio redistributable folder, found by vswhere (or SUVTITLE_VS), exactly as
//             tools\vsenv.cmd finds Visual Studio. The engine loads sherpa with the DLL's own folder first in the search
//             order, so these copies are the ones that load.
//
// The checks that make the bundle trustworthy (each one fails loudly, none is cosmetic):
//   1. every source exists BEFORE anything is copied (the whole list of what is missing is printed);
//   2. the build folder is not older than the sources (warning), its web\ equals app\web (warning), its type is known;
//   3. refresh mode: a file with the same size AND the same mtime is not copied again; a file in app\ that the plan does
//      not know (a leftover of an older layout) is removed;
//   4. every .exe / .dll of the bundle is x64 (PE header), and no import library / debug file is in it;
//   5. dumpbin dependency CLOSURE per folder (dumpbin from the Visual Studio found by vswhere, or SUVTITLE_VS): every DLL an exe or DLL imports is either in its
//      folder or a Windows component; backends loaded by name (ggml-cpu-*.dll) are treated as roots;
//   6. manifest.txt: SHA-256 of everything (--verify re-checks a bundle against it); a FULL bundle's models must have the
//      SHA-256 (and, before the copy, the size) installer/models.json pins, or no manifest is written.
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
// sherpa-onnx (1.1.0, the LIVE speaker model): the C API DLL, ONNX Runtime, and ONNX Runtime's shared providers bridge. Only the
// first two are IMPORTED (sherpa-onnx-c-api.dll -> onnxruntime.dll); the third is loaded by name, so it is a root of the check.
// NOT shipped: the .lib import libraries, the C++ wrapper DLL and the probe exe that sit in the same folder of a payload.
const SHERPA_BIN = ['sherpa-onnx-c-api.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll'];
// The CRT: what each /MD payload exe needs, MEASURED (dumpbin) -- the closure check below is the guard if llama.cpp
// ever imports one more (msvcp140_1.dll, concrt140.dll ...): it names the missing DLL, it is added here.
const CRT_FILES = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'];
const OMP_FILES = ['vcomp140.dll'];                       // whisper's ggml-cpu.dll only (llama uses its own libomp.dll)
// ONNX Runtime (measured, dumpbin -dependents onnxruntime.dll) imports MSVCP140_1.dll as well: next to sherpa only.
const SHERPA_CRT_FILES = [...CRT_FILES, 'msvcp140_1.dll'];
// THE MODELS: installer/models.json is the one list (the light installer downloads from it, the full one embeds it, the
// .iss carries a block generated from it). Each entry becomes { name, why, size, sha256, url, source, license } here, and
// `alt` -- the name the publisher gave the same bytes on Hugging Face (the last part of source.path), when it is not
// `name` -- is tried in the payload when `name` is absent; the file always ships as `name`.
const MODELS_JSON = path.join(__dirname, '..', 'installer', 'models.json');
const SEG = '[A-Za-z0-9][A-Za-z0-9._-]*';
function loadModels(file = MODELS_JSON) {
  let j;
  try { j = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (e) { throw new Error(`${file}: ${e.message}`); }
  const bad = (m) => { throw new Error(`${path.basename(file)}: ${m}`); };
  if (!j || j.schema !== 1 || !Array.isArray(j.models) || j.models.length === 0) bad('expected { "schema": 1, "models": [ ... ] }');
  const seen = new Set();
  return j.models.map((m, i) => {
    const at = `models[${i}]${m && typeof m.file === 'string' ? ' (' + m.file + ')' : ''}`;
    if (!m || typeof m.file !== 'string' || !new RegExp(`^${SEG}$`).test(m.file)) bad(`${at}: "file" must be a plain file name`);
    if (seen.has(m.file.toLowerCase())) bad(`${at}: listed twice`);
    seen.add(m.file.toLowerCase());
    if (!Number.isSafeInteger(m.size) || m.size <= 0) bad(`${at}: "size" must be a positive integer (bytes)`);
    if (typeof m.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(m.sha256)) bad(`${at}: "sha256" must be 64 lower-case hex digits`);
    const s = m.source || {};
    if (typeof s.repo !== 'string' || !new RegExp(`^${SEG}/${SEG}$`).test(s.repo)) bad(`${at}: "source.repo" must be <owner>/<name>`);
    if (typeof s.commit !== 'string' || !/^[0-9a-f]{40}$/.test(s.commit)) bad(`${at}: "source.commit" must be a full commit id (a branch would let the file change under the installer)`);
    if (typeof s.path !== 'string' || !new RegExp(`^${SEG}(/${SEG})*$`).test(s.path)) bad(`${at}: "source.path" must be a path in the repository`);
    const url = `https://huggingface.co/${s.repo}/resolve/${s.commit}/${s.path}`;
    if (m.url !== url) bad(`${at}: "url" must be ${url} (the source, pinned to its commit)`);
    if (typeof m.license !== 'string' || !m.license.trim()) bad(`${at}: "license" is missing`);
    if (typeof m.why !== 'string' || !m.why.trim()) bad(`${at}: "why" is missing`);
    const hfName = s.path.split('/').pop();
    return { name: m.file, why: m.why, size: m.size, sha256: m.sha256, url: m.url, source: { repo: s.repo, commit: s.commit, path: s.path },
             license: m.license, ...(hfName !== m.file ? { alt: [hfName] } : {}) };
  });
}
// Read once; a broken models.json fails every assembly (main) and the installer check, never the require.
let MODELS = null, MODELS_ERROR = null;
try { MODELS = loadModels(); } catch (e) { MODELS_ERROR = e; }
const MODEL_LARGE_V3 = { name: 'ggml-large-v3.bin', why: 'transcription, quality "max" only (optional; not in models.json)' };
// 1.1.0: the native video engine (from --mpv / SUVTITLE_MPV_DIR) and the Vulkan loader fallback (from --vulkan-rt /
// SUVTITLE_VULKAN_RT = the extracted "VulkanRT-X64-<version>-Components" folder: x64\vulkan-1.dll + VulkanRT-License.txt).
const MPV_BIN = ['libmpv-2.dll'];
const MPV_DIR = 'mpv';                                    // <app>\mpv\ -- app\MpvPlayer.cpp defaultDllDir, engine\TrMpvAudio.cpp
const VKFB_DIR = 'mpv\\vkfallback';                        // engine\TrMpvAudio.h kVulkanFallbackSubdir, under MPV_DIR
const VKFB_FILES = [{ name: 'vulkan-1.dll', from: 'x64\\vulkan-1.dll' }, { name: 'VulkanRT-License.txt', from: 'VulkanRT-License.txt' }];

// What Windows itself provides, never us. Whole base names (a prefix match would let "mfc140u.dll" slip through as "mf").
// AudioPB's list + what SuVtitle's own exes and the Release build can import (Media Foundation is DELAY-loaded by the engine).
const SYSTEM_DLLS = new Set([
  'kernel32', 'kernelbase', 'ntdll', 'user32', 'advapi32', 'shell32', 'shlwapi', 'ole32', 'oleaut32', 'combase', 'ws2_32',
  'crypt32', 'psapi', 'setupapi', 'dbghelp', 'dxgi', 'winmm', 'avrt', 'winhttp', 'mmdevapi', 'gdi32', 'version', 'bcrypt',
  'rpcrt4', 'sechost', 'imm32', 'comdlg32', 'uxtheme', 'dwmapi', 'd3d11', 'd3d12', 'elscore', 'mf', 'mfplat', 'mfreadwrite',
  'comctl32', 'shcore', 'ucrtbase', 'msvcrt', 'wininet', 'iphlpapi', 'userenv', 'secur32', 'wtsapi32', 'netapi32', 'win32u',
  'gdi32full', 'msimg32', 'ncrypt', 'wintrust', 'cfgmgr32', 'powrprof', 'propsys', 'urlmon', 'windowscodecs', 'dcomp',
  'd2d1', 'dwrite',
  // 1.1.0, libmpv-2.dll's own imports (measured: its import table) that the list above lacked -- all Windows components
  'opengl32', 'wldap32', 'bcryptprimitives', 'avicap32', 'normaliz',
  // 1.1.0, SuVtitle.exe's own import (the native engine's host): UI Automation, a Windows component since Windows 7
  'uiautomationcore',
]);
// Provided by the GPU driver, not by Windows: reported, accepted -- and, since 1.1.0, ALSO shipped as the fallback in
// mpv\vkfallback\ (never next to its importers: the PC's own loader must win where there is one).
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
  '  --out <dir>           output root; the bundle is <dir>\\SuVtitle (default <repo>\\deploy\\out; for a full bundle beside the',
  '                        light one: --out deploy\\out\\full, git-ignored too)',
  '  --light               a LIGHT bundle: no models (the default; the light installer downloads installer\\models.json\'s models',
  '                        at install time). --skip-models is its former name',
  '  --full                a FULL bundle: the models of installer\\models.json embedded, from <payload>\\models, for the offline',
  '                        installer (ISCC /DFull); each one must have the size and the SHA-256 models.json pins',
  '  --payload <dir>       the AI payload: whisper\\ (whisper-server + DLLs), llama\\ (llama-server + DLLs), sherpa\\ (sherpa-onnx-c-api.dll,',
  '                        onnxruntime.dll, onnxruntime_providers_shared.dll), models\\ (--full only: the model files, the speaker model',
  '                        under either name); default: the environment variable SUVTITLE_PAYLOAD. No built-in default: not needed',
  '                        for --verify / --help.',
  '  --mpv <dir>           the folder holding libmpv-2.dll, the native video engine (default: the environment variable',
  '                        SUVTITLE_MPV_DIR, the one the app\'s build reads); shipped as app\\mpv\\libmpv-2.dll',
  '  --vulkan-rt <dir>     the extracted LunarG "VulkanRT-X64-<version>-Components" folder (x64\\vulkan-1.dll, VulkanRT-License.txt;',
  '                        default: SUVTITLE_VULKAN_RT); shipped as app\\mpv\\vkfallback\\ -- the loader a PC without one gets',
  '  --vcredist <dir>      a VC++ Redist\\MSVC folder to search for the CRT DLLs (default: the Visual Studio found by vswhere, or SUVTITLE_VS)',
  '  --with-large-v3       (--full only) also ship ggml-large-v3.bin (3 GB; quality "max" only; not in models.json)',
  '  --skip-vcruntime      do not copy the CRT DLLs: the target PC is then ASSUMED to have the VC++ Redistributable',
  '                        (the dependency check accepts those imports and says so)',
  '  --no-verify-deps      skip the dumpbin dependency check',
  '  --clean               delete the existing bundle folder first (otherwise: refresh)',
  '  --dry-run             check the sources and print the plan, copy nothing',
  '  --verify              re-hash an assembled bundle against its manifest.txt, then exit',
  '  --help',
].join('\n');

function parseArgs(argv) {
  const o = { out: null, build: null, payload: process.env['SUVTITLE_PAYLOAD'] || null, vcredist: null, clean: false, full: false, light: false,
              mpv: process.env['SUVTITLE_MPV_DIR'] || null, vulkanRt: process.env['SUVTITLE_VULKAN_RT'] || null,
              withLargeV3: false, skipVcRuntime: false, noVerifyDeps: false, dryRun: false, verify: false, help: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => { if (i + 1 >= argv.length) { const e = new BundleError(`${a} needs a value`); e.usage = true; throw e; } return argv[++i]; };
    switch (a) {
      case '--build': o.build = val(); break;
      case '--out': o.out = val(); break;
      case '--payload': o.payload = val(); break;
      case '--mpv': o.mpv = val(); break;
      case '--vulkan-rt': o.vulkanRt = val(); break;
      case '--vcredist': o.vcredist = val(); break;
      case '--light': case '--skip-models': o.light = true; break;
      case '--full': o.full = true; break;
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
  const conflict = (m) => { const e = new BundleError(m); e.usage = true; throw e; };
  if (o.full && o.light) conflict('--full and --light (--skip-models) contradict each other');
  if (o.withLargeV3 && !o.full) conflict('--with-large-v3 needs --full (a light bundle has no models)');
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
  if (MODELS_ERROR) fail(`installer\\models.json is not valid: ${MODELS_ERROR.message}`);
  const kind = o.full ? 'FULL' : 'LIGHT';

  const buildDir = path.resolve(o.build || path.join(repo, 'build', 'x64-Release'));
  if (!o.payload) {
    const e = new BundleError('no payload: pass --payload <dir> or set SUVTITLE_PAYLOAD to a folder holding whisper\\ (whisper-server + its DLLs), llama\\ (llama-server + its DLLs), sherpa\\ (sherpa-onnx + ONNX Runtime DLLs) and, for --full, models\\.');
    e.usage = true;
    throw e;
  }
  const wRoot = path.resolve(o.payload);
  const wBin = path.join(wRoot, 'whisper');
  const lBin = path.join(wRoot, 'llama');
  const sBin = path.join(wRoot, 'sherpa');
  const wModels = path.join(wRoot, 'models');
  // 1.1.0: libmpv and the Vulkan loader fallback, both REQUIRED (the player plays every file through libmpv; a PC without a
  // loader could start neither libmpv nor whisper-server)
  if (!o.mpv || !o.vulkanRt) {
    const e = new BundleError((!o.mpv ? 'no libmpv: pass --mpv <dir> or set SUVTITLE_MPV_DIR to the folder holding libmpv-2.dll. ' : '')
      + (!o.vulkanRt ? 'no Vulkan loader: pass --vulkan-rt <dir> or set SUVTITLE_VULKAN_RT to the extracted LunarG "VulkanRT-X64-<version>-Components" folder (x64\\vulkan-1.dll, VulkanRT-License.txt).' : ''));
    e.usage = true;
    throw e;
  }
  const mpvSrc = path.resolve(o.mpv);
  const vkSrc = path.resolve(o.vulkanRt);
  const bt = buildTypeOf(buildDir);

  say('SuVtitle - bundle assembly');
  say(`  kind      : ${kind}${o.full ? ' (the models embedded: the offline installer, ISCC /DFull)' : ' (no models: the installer downloads those of installer\\models.json)'}`);
  say(`  repo      : ${repo}`);
  say(`  build     : ${buildDir}  [${bt.type}, from ${bt.from}]`);
  say(`  payload   : ${wRoot}`);
  say(`  libmpv    : ${mpvSrc}`);
  say(`  Vulkan RT : ${vkSrc}`);
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
  const add = (src, rel, note) => plan.push({ src, rel, note });
  for (const n of ['SuVtitle.exe', 'SuVtitleEngine.exe', 'WebView2Loader.dll']) add(path.join(buildDir, n), 'app\\' + n);
  // The licences travel with the program (1.1.0): SuVtitle's own (GPL-3.0-or-later) and the notices of what the bundle
  // redistributes -- sherpa-onnx and ONNX Runtime among them, whose licences ask for their text to come along.
  for (const n of ['LICENSE', 'THIRD_PARTY_NOTICES.md']) add(path.join(repo, n), 'app\\' + n);
  const webDir = path.join(buildDir, 'web');
  const webFiles = isDir(webDir) ? walk(webDir) : [];
  for (const r of webFiles) add(path.join(webDir, r), 'app\\web\\' + r);
  for (const n of WHISPER_BIN) add(path.join(wBin, n), 'app\\engine\\whisper\\' + n);
  for (const n of LLAMA_BIN) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
  for (const n of SHERPA_BIN) add(path.join(sBin, n), 'app\\engine\\sherpa\\' + n);
  // 1.1.0: the native video engine, and the Vulkan loader in its OWN folder (see the header: never next to its importers)
  for (const n of MPV_BIN) add(path.join(mpvSrc, n), 'app\\' + MPV_DIR + '\\' + n);
  for (const v of VKFB_FILES) add(path.join(vkSrc, v.from), 'app\\' + VKFB_DIR + '\\' + v.name);
  // The CPU backends llama loads by name: ALL of them (about 18 MB; the PC this runs on is not known in advance).
  const cpuVariants = isDir(lBin) ? fs.readdirSync(lBin).filter((f) => /^ggml-cpu-.+\.dll$/i.test(f)).sort() : [];
  for (const n of cpuVariants) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
  for (const n of LLAMA_EXTRA) {
    if (isFile(path.join(lBin, n))) add(path.join(lBin, n), 'app\\engine\\llama\\' + n);
    else warn(`${n} (the license of libomp.dll) is not next to the llama binaries; not shipped.`);
  }
  if (crt) {
    for (const n of CRT_FILES) { add(path.join(crt.crt, n), 'app\\engine\\whisper\\' + n); add(path.join(crt.crt, n), 'app\\engine\\llama\\' + n); }
    for (const n of SHERPA_CRT_FILES) add(path.join(crt.crt, n), 'app\\engine\\sherpa\\' + n);
    if (crt.omp) for (const n of OMP_FILES) add(path.join(crt.omp, n), 'app\\engine\\whisper\\' + n);
    else warn('vcomp140.dll not found: whisper\'s ggml-cpu.dll imports it, whisper fails on a PC without the VC++ Redistributable.');
  }
  // The models: a FULL bundle only (a light one has no models\ at all: the light installer downloads them).
  const modelList = o.full ? [...MODELS, ...(o.withLargeV3 ? [MODEL_LARGE_V3] : [])] : [];
  // A model's source: its own name in the payload, else the first alternative name the payload has (the publisher's); it
  // ships under its own name either way. None there = the own name's path, which the sources check below reports.
  const modelSrc = (m) => [m.name, ...(m.alt || [])].map((n) => path.join(wModels, n)).find(isFile) || path.join(wModels, m.name);
  for (const m of modelList) add(modelSrc(m), 'app\\models\\' + m.name, m.alt ? 'or ' + m.alt.join(' / ') : undefined);
  if (cpuVariants.length === 0) warn('no ggml-cpu-*.dll next to the llama binaries: the CPU fallback of llama-server would be DEAD.');

  // --- 1. sources, ALL of them, before anything is touched ------------------------
  step(`Checking ${plan.length} sources`);
  const missing = plan.filter((e) => !isFile(e.src)).map((e) => e.src + (e.note ? `   (${e.note})` : ''));
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
  // A FULL bundle's models must be the files installer\models.json pins: their size now (a different quantization or an
  // older file is caught before gigabytes are copied), their SHA-256 when the manifest hashes them (step 6).
  if (o.full) {
    const wrong = [];
    for (const m of modelList) {
      if (m.size === undefined) continue;                       // ggml-large-v3.bin (--with-large-v3): not in models.json
      const sz = fs.statSync(modelSrc(m)).size;
      if (sz !== m.size) wrong.push(`${modelSrc(m)}: ${num(sz)} bytes; models.json pins ${num(m.size)} (${m.url})`);
    }
    if (wrong.length) {
      for (const w of wrong) say('   ' + w);
      fail('the payload\'s models are not the files installer\\models.json pins - nothing was copied.');
    }
    say(`   the ${MODELS.length} models have the sizes installer\\models.json pins (their SHA-256 is checked once copied).`);
  } else say(`   LIGHT bundle: no models (the light installer downloads the ${MODELS.length} of installer\\models.json).`);

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
  // The bundle loses its certificate while it changes: when anything below fails, no manifest.txt is left for the installer
  // to trust (it refuses a bundle without one) -- an older manifest would vouch for files it never saw.
  for (const f of fs.readdirSync(bundle)) {
    if (/^(manifest\.txt|BUILD-.*\.txt)$/i.test(f)) retry('removal of ' + f, () => fs.unlinkSync(path.join(bundle, f)));
  }
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
    if (!isFile(p) || fs.statSync(p).size !== fs.statSync(modelSrc(m)).size) fail(`model ${m.name} is not (completely) in the bundle - not certified.`);
  }
  if (o.full) {
    say(`   ${modelList.length} models in place:`);
    for (const m of modelList) {
      const from = path.basename(modelSrc(m));
      say(`     ${m.name.padEnd(40)} ${human(fs.statSync(path.join(appDir, 'models', m.name)).size).padStart(9)}  ${m.why}${from !== m.name ? `  [payload name: ${from}]` : ''}`);
    }
  }
  const binaries = walk(appDir).filter((r) => /\.(exe|dll)$/i.test(r));
  const notX64 = [];
  for (const r of binaries) {
    const mach = peMachine(path.join(appDir, r));
    if (mach !== IMAGE_FILE_MACHINE_AMD64) notX64.push(`${r} (${mach === null ? 'not a PE file' : 'machine 0x' + mach.toString(16)})`);
  }
  const forbidden = walk(appDir).filter((r) => FORBIDDEN_EXT.includes(path.extname(r).toLowerCase()));
  // 1.1.0: the Vulkan loader ONLY in mpv\vkfallback\ -- a copy next to the exes, next to libmpv or next to the servers would be
  // found BEFORE System32's (the application folder, AddDllDirectory folders and an exe's own folder all come first) and beat
  // the PC's own loader, the one that matches its driver.
  const strayVk = binaries.filter((r) => /(^|\\)vulkan-1\.dll$/i.test(r) && r.toLowerCase() !== (VKFB_DIR + '\\vulkan-1.dll').toLowerCase());
  if (notX64.length || forbidden.length || strayVk.length) {
    for (const n of notX64) say('   NOT x64: ' + n);
    for (const f of forbidden) say('   forbidden in a shipped folder: ' + f);
    for (const v of strayVk) say('   a Vulkan loader outside ' + VKFB_DIR + '\\ (it would beat the PC\'s own): ' + v);
    fail('payload checks failed.');
  }
  say(`   ${binaries.length} executables / DLLs, all x64 (PE machine 0x8664); no import library, no debug file; the Vulkan loader only in ${VKFB_DIR}\\.`);

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
        // sherpa-onnx: the engine LoadLibrary's sherpa-onnx-c-api.dll (-> onnxruntime.dll); the providers bridge is loaded by
        // ONNX Runtime by name. All three are roots, so none ships unanalysed.
        { name: 'engine\\sherpa', dir: path.join(appDir, 'engine', 'sherpa'), seeds: SHERPA_BIN },
        // 1.1.0: libmpv, loaded at run time by the app and the engine (a root); its vulkan-1.dll is the PC's, else the fallback
        // below (pre-loaded by full path) -- and the fallback loader itself, a root too (loaded by full path, never imported).
        { name: MPV_DIR, dir: path.join(appDir, MPV_DIR), seeds: MPV_BIN },
        { name: VKFB_DIR, dir: path.join(appDir, VKFB_DIR), seeds: ['vulkan-1.dll'] },
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
                   (r.driver.size ? `; from the GPU driver, else the bundled ${VKFB_DIR}: ${[...r.driver].join(', ')}` : '') +
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
    `kind: ${kind}${o.full ? ' (the models embedded: compile with ISCC /DFull)' : ' (no models: the light installer downloads them)'}\n` +
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
  // A FULL bundle certifies the models.json bytes or nothing: the full installer refuses a manifest that lists other ones.
  if (o.full) {
    const wrong = [];
    for (const m of MODELS) {
      const row = rows.find((x) => x.split('\t')[2].toLowerCase() === ('app\\models\\' + m.name).toLowerCase());
      const [h, sz] = row ? row.split('\t') : ['(missing)', '0'];
      if (h !== m.sha256.toUpperCase() || Number(sz) !== m.size) wrong.push(`${m.name}: SHA-256 ${h}, ${num(sz)} bytes; models.json pins ${m.sha256.toUpperCase()}, ${num(m.size)} bytes`);
    }
    if (wrong.length) {
      for (const w of wrong) say('   ' + w);
      fail('the bundle\'s models are not the files installer\\models.json pins - no manifest written, the installer refuses this bundle.');
    }
    say(`   the ${MODELS.length} models have the SHA-256 installer\\models.json pins.`);
  }
  const head = [
    '# SuVtitle - bundle manifest',
    `# assembled ${localStamp(stamp)}`,
    `# build: ${bt.type} (${bt.from}) -- ${buildDir}`,
    `# git: ${gitLine}`,
    `# kind: ${kind}${o.full ? ' (the models embedded, SHA-256 = installer\\models.json: the offline installer, ISCC /DFull)' : ' (no models: the installer downloads those of installer\\models.json)'}`,
    `# models: ${o.full ? modelList.map((m) => m.name).join(', ') : 'none in the bundle (light)'}`,
    `# native video engine: ${MPV_BIN.join(', ')} from ${mpvSrc}; Vulkan loader fallback (${VKFB_DIR}) from ${vkSrc}`,
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
  say(`   kind      : ${kind}${o.full ? '  (ISCC /DFull /DBundleDir=...)' : '  (ISCC installer\\SuVtitle_Setup.iss)'}`);
  say(`   build     : ${bt.type}${bt.type === 'Release' ? '' : '   <-- NOT shippable'}`);
  say(`   files     : ${rows.length} (+ manifest.txt)`);
  say(`   size      : ${human(total)} (${num(total)} bytes)`);
  const groupsOut = [
    ['app (exes + web)', (r) => /^app\\[^\\]+$/i.test(r) || /^app\\web\\/i.test(r)],
    ['app\\engine\\whisper', (r) => /^app\\engine\\whisper\\/i.test(r)],
    ['app\\engine\\llama', (r) => /^app\\engine\\llama\\/i.test(r)],
    ['app\\engine\\sherpa', (r) => /^app\\engine\\sherpa\\/i.test(r)],
    ['app\\mpv (libmpv)', (r) => /^app\\mpv\\[^\\]+$/i.test(r)],
    ['app\\mpv\\vkfallback', (r) => /^app\\mpv\\vkfallback\\/i.test(r)],
    ['app\\models', (r) => /^app\\models\\/i.test(r)],
  ];
  for (const [n, f] of groupsOut) {
    const grp = rels.filter(f);
    const sz = grp.reduce((a, r) => a + fs.statSync(path.join(bundle, r)).size, 0);
    say(`   ${n.padEnd(22)} ${String(grp.length).padStart(3)} files  ${human(sz).padStart(10)}`);
  }
  say(`   deps      : ${depVerdict}`);
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

module.exports = { MODELS, MODELS_ERROR, MODELS_JSON, loadModels, SHERPA_BIN, SHERPA_CRT_FILES, MPV_BIN, MPV_DIR, VKFB_DIR, VKFB_FILES, parseDumpbinDependents, isSystemDll, isDriverDll, isCrtDll, peMachine, buildTypeOf, checkDependencies, cmpVersionDesc, walk, SYSTEM_DLLS };

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

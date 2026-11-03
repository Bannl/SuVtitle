#!/usr/bin/env node
'use strict';
// =============================================================================
// SuVtitle - tests/installer/run_installer_kit.js: how the LIGHT installer BEHAVES, tested offline.
//
// Each case compiles tests\installer\dltest.iss -- a THROWAWAY setup (its own AppId, the current user's rights, no
// uninstaller, no registry) that #includes the light installer's own model download code, installer\models_download.iss
// -- and runs it silently (/VERYSILENT /SUPPRESSMSGBOXES) into a throwaway folder, against serve_models.js on 127.0.0.1
// (a stand-in for Hugging Face, its 302 hop included). No internet, no administrator rights, nothing of SuVtitle is
// installed. Then it checks the exit code, the files, what the server was asked and the setup's log. The test models
// are generated here, two sizes (1.2 MB and 33 MB; --big adds one above 2 GiB).
//
//   node tests\installer\run_installer_kit.js [--mutants] [--big] [--keep] [case ...]
//     --mutants  then the mutation sentinels: each mutant of models_download.iss must make one of its cases fail
//     --big      also a model above 2 GiB, on two drives (writes about 2.1 GiB three times: served file, {tmp}, install)
//     --keep     keep the work folders (default: removed at the end)
//
// Needs Node 18.15+ (fs.statfsSync) and Inno Setup 6 (ISCC.exe: SUVTITLE_ISCC, PATH, or its standard folders). The cases that need {tmp}
// and the installation folder on two drives use tests\installer\work\ when the repository is on another drive than
// %TEMP%, else SUVTITLE_KIT_XDIR (a folder on another drive than %TEMP%); with neither they print NOT CHECKED.
// Exit code: 0 = every case run (and every mutant) as expected; 1 = a failure; 2 = bad command line.
// =============================================================================

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const net = require('net');

const REPO = path.resolve(__dirname, '..', '..');
const gen = require(path.join(REPO, 'installer', 'gen_iss_registry.js'));
const REGION = path.join(REPO, 'installer', 'models_download.iss');
const DLTEST = path.join(__dirname, 'dltest.iss');
const A_ROOT = path.join(os.tmpdir(), 'suvtitle-installer-kit');

const SMALL = { file: 'kit-small.bin', size: 1234567 };
const MEDIUM = { file: 'kit-medium.gguf', size: 34567890 };
const BIG = { file: 'kit-big.bin', size: 2250000013 };

const say = (s) => process.stdout.write(s + '\n');
const rm = (p) => fs.rmSync(p, { recursive: true, force: true, maxRetries: 6, retryDelay: 200 });
const exists = (p) => { try { fs.statSync(p); return true; } catch (_) { return false; } };
const driveOf = (p) => path.parse(path.resolve(p)).root.toLowerCase();
const sha256File = (f) => {
  const h = crypto.createHash('sha256');
  const fd = fs.openSync(f, 'r');
  const buf = Buffer.alloc(8 * 1048576);
  try { let n; while ((n = fs.readSync(fd, buf, 0, buf.length, null)) > 0) h.update(n === buf.length ? buf : buf.subarray(0, n)); } finally { fs.closeSync(fd); }
  return h.digest('hex');
};

// Deterministic content: a xorshift stream seeded by the name.
function writeModel(file, size) {
  let x = 0x9e3779b9;
  for (const c of path.basename(file)) x = (x ^ c.charCodeAt(0)) * 16777619 >>> 0;
  const chunk = Buffer.alloc(4 * 1048576);
  const fd = fs.openSync(file, 'w');
  try {
    for (let done = 0; done < size;) {
      for (let i = 0; i < chunk.length; i += 4) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; chunk.writeUInt32LE(x, i); }
      const n = Math.min(chunk.length, size - done);
      fs.writeSync(fd, chunk, 0, n);
      done += n;
    }
  } finally { fs.closeSync(fd); }
}

// The other drive for the two-drive cases, or null.
function xRoot() {
  const own = path.join(__dirname, 'work');
  if (driveOf(own) !== driveOf(A_ROOT)) return own;
  const env = process.env.SUVTITLE_KIT_XDIR;
  if (env && driveOf(env) !== driveOf(A_ROOT)) return path.join(env, 'suvtitle-installer-kit');
  return null;
}
const freeBytes = (p) => { const s = fs.statfsSync(p); return s.bavail * s.bsize; };
const icaclsAces = (file) => {
  const r = cp.spawnSync('icacls', [file], { encoding: 'utf8', windowsHide: true });
  const lines = (r.stdout || '').split(/\r?\n/).map((l) => l.trim()).filter((l) => l && !/^Successfully processed/.test(l));
  if (lines.length) lines[0] = lines[0].slice(file.length).trim();
  return lines.filter(Boolean).sort();
};

// -----------------------------------------------------------------------------------------------------------------
// The cases. models: [{ m, mode, size?, sha256?, down? }] -- size / sha256 override what the setup is told (not served).
// cross: the installation folder on the other drive. preplace: { file: 'real' | 'junk' (another size) | 'corrupt' (same
// size, other bytes) }. leftovers: *.part files left in models\ beforehand. expect: see check().
// -----------------------------------------------------------------------------------------------------------------
const CASES = [
  { id: 'fresh', what: 'two models missing, same drive: downloaded (302 hop), SHA-256 checked, kept in {tmp} until the install, put in place with the folder\'s permissions',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], acl: true,
    expect: { exit: 0, placed: [SMALL, MEDIUM], requests: { [SMALL.file]: 2, [MEDIUM.file]: 2 }, readme: true,
      atInstall: { tmpHas: [SMALL.file, MEDIUM.file], modelsLacks: [SMALL.file + '.part', MEDIUM.file + '.part'] },
      memo: /AI models to download from Hugging Face \(34 MB; .*kit-small\.bin \(1 MB\).*kit-medium\.gguf \(33 MB\)/ } },
  { id: 'bad_hash', what: 'the server sends other bytes of the same size: SHA-256 mismatch, nothing installed, exit code 7',
    models: [{ m: SMALL, mode: 'bad' }],
    expect: { exit: 7, absent: [SMALL], requests: { [SMALL.file]: 2 }, readme: false, log: [/The hash of the file is incorrect/, /Setup stops before installing anything/] } },
  { id: 'in_place', what: 'both models in place with their sizes: kept, not a single request (an update needs no network)',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], preplace: { [SMALL.file]: 'real', [MEDIUM.file]: 'real' },
    expect: { exit: 0, unchanged: [SMALL, MEDIUM], requests: { [SMALL.file]: 0, [MEDIUM.file]: 0 }, readme: true, memo: /already installed: nothing to download/ } },
  { id: 'wrong_size', what: 'a model in place with another size (an older one): downloaded again and replaced',
    models: [{ m: SMALL, mode: 'ok' }], preplace: { [SMALL.file]: 'junk' },
    expect: { exit: 0, placed: [SMALL], requests: { [SMALL.file]: 2 }, readme: true } },
  { id: 'partial', what: 'one model in place, one missing: only the missing one is asked for',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], preplace: { [SMALL.file]: 'real' },
    expect: { exit: 0, unchanged: [SMALL], placed: [MEDIUM], requests: { [SMALL.file]: 0, [MEDIUM.file]: 2 }, readme: true } },
  { id: 'offline', what: 'nothing listens (offline): exit code 7, nothing installed',
    models: [{ m: SMALL, mode: 'ok', down: true }],
    expect: { exit: 7, absent: [SMALL], readme: false, log: [/could not be established|cannot connect|12029/i] } },
  { id: 'http_500', what: 'the server answers 500: exit code 7, nothing installed',
    models: [{ m: SMALL, mode: '500' }],
    expect: { exit: 7, absent: [SMALL], requests: { [SMALL.file]: 1 }, readme: false, log: [/500/] } },
  { id: 'retry_503', what: 'Retry (the silent answer forced to Retry): a 503, then the file; the model already downloaded is not asked again',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'flaky' }], silentAnswer: 'IDRETRY',
    expect: { exit: 0, placed: [SMALL, MEDIUM], requests: { [SMALL.file]: 2, [MEDIUM.file]: 3 }, readme: true } },
  { id: 'retry_bad_hash', what: 'Retry after a SHA-256 mismatch: the second download is the right file',
    models: [{ m: MEDIUM, mode: 'badonce' }], silentAnswer: 'IDRETRY',
    expect: { exit: 0, placed: [MEDIUM], requests: { [MEDIUM.file]: 4 }, readme: true } },
  { id: 'cross_drive', what: '{tmp} and the installation on two drives: each model leaves {tmp} for <file>.part on the installation drive right after its download; the final names appear only after the install',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], cross: true, acl: true,
    expect: { exit: 0, placed: [SMALL, MEDIUM], requests: { [SMALL.file]: 2, [MEDIUM.file]: 2 }, readme: true,
      atInstall: { tmpLacks: [SMALL.file, MEDIUM.file], modelsHas: [SMALL.file + '.part', MEDIUM.file + '.part'], modelsLacks: [SMALL.file, MEDIUM.file] } } },
  { id: 'cross_cancel', what: 'two drives, the second download fails: the first one\'s .part and the folders made for it go again (the folder did not exist)',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: '500' }], cross: true, noPreDir: true,
    expect: { exit: 7, requests: { [SMALL.file]: 2, [MEDIUM.file]: 1 }, instGone: true, log: [/staged as .*kit-small\.bin\.part/, /removed .*kit-small\.bin\.part \(not put in place\)/] } },
  { id: 'no_room', what: 'the models need more room than the drive has: said before any request, nothing installed',
    models: [{ m: SMALL, mode: 'ok', size: 50 * 1024 ** 4 }],
    expect: { exit: 7, requests: { [SMALL.file]: 0 }, readme: false, log: [/not enough free space/] } },
  { id: 'tmp_largest_only', what: 'two drives: {tmp}\'s drive needs room for the LARGEST model, not for all of them (3 models of 55% of its free space each; the downloads then fail with a 500)',
    cross: true, models: 'tmpLargest',
    expect: { exit: 7, readme: false, totalRequests: 1, noLog: [/not enough free space/], log: [/Download failed: 500/] } },
  { id: 'leftover_part', what: '*.part files left by a Setup that did not finish are deleted, never trusted',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], preplace: { [MEDIUM.file]: 'real' },
    leftovers: [SMALL.file + '.part', 'old-model.gguf.part'],
    expect: { exit: 0, placed: [SMALL], unchanged: [MEDIUM], requests: { [SMALL.file]: 2, [MEDIUM.file]: 0 }, readme: true,
      log: [/kit-small\.bin\.part, left by a Setup that did not finish/, /old-model\.gguf\.part, left by a Setup that did not finish/] } },
  { id: 'verify_damaged', what: '/VERIFYMODELS: a model in place with its size but other bytes is downloaded again; an intact one is hashed and kept',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], preplace: { [SMALL.file]: 'corrupt', [MEDIUM.file]: 'real' }, args: ['/VERIFYMODELS'],
    expect: { exit: 0, placed: [SMALL], unchanged: [MEDIUM], requests: { [SMALL.file]: 2, [MEDIUM.file]: 0 }, readme: true,
      log: [/kit-small\.bin in place has another SHA-256/, /kit-medium\.gguf in place: SHA-256 checked \(\/VERIFYMODELS\), kept/], memo: /checked first \(SHA-256, \/VERIFYMODELS\)/ } },
  { id: 'noverify_damaged', what: 'the same damaged model WITHOUT /VERIFYMODELS: kept (only its size is read: the default)',
    models: [{ m: SMALL, mode: 'ok' }], preplace: { [SMALL.file]: 'corrupt' },
    expect: { exit: 0, unchanged: [SMALL], requests: { [SMALL.file]: 0 }, readme: true } },
  { id: 'truncated_tmp', what: 'a download damaged before its placement (same drive): refused, the previous file kept, exit code 10',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], preplace: { [SMALL.file]: 'junk' }, truncateTmp: SMALL.file,
    expect: { exit: 10, unchanged: [SMALL], placed: [MEDIUM], readme: true, log: [/kit-small\.bin NOT put in place/, /exit code 10/, /previous file, of another size, is kept/] } },
  { id: 'truncated_part', what: 'a .part damaged before its placement (two drives): refused, the previous file kept, exit code 10',
    models: [{ m: SMALL, mode: 'ok' }, { m: MEDIUM, mode: 'ok' }], cross: true, preplace: { [SMALL.file]: 'junk' }, truncatePart: SMALL.file,
    expect: { exit: 10, unchanged: [SMALL], placed: [MEDIUM], readme: true, log: [/kit-small\.bin NOT put in place/, /exit code 10/] } },
  { id: 'locked', what: 'the previous model is held open by another process: the replacement fails, the previous file is intact, exit code 10',
    models: [{ m: SMALL, mode: 'ok' }], preplace: { [SMALL.file]: 'junk' }, lock: SMALL.file,
    expect: { exit: 10, unchanged: [SMALL], requests: { [SMALL.file]: 2 }, readme: true, log: [/cannot be replaced/, /exit code 10/] } },
  { id: 'rm_register', what: 'a model about to be replaced is registered for the in-use check (CloseApplications, the Restart Manager)',
    models: [{ m: SMALL, mode: 'ok' }], preplace: { [SMALL.file]: 'junk' }, closeApps: 'yes',
    expect: { exit: 0, placed: [SMALL], readme: true, log: [/registered for the in-use check/, /Found 1 files to register with RestartManager/] } },
  { id: 'memo_units', what: 'the Ready page\'s sizes: 1,073,741,823 bytes read "1.0 GB" (never "1024 MB"), 1,048,575 bytes "1 MB"',
    models: [{ m: SMALL, mode: '500', size: 1073741823 }, { m: MEDIUM, mode: '500', size: 1048575 }],
    expect: { exit: 7, readme: false, memo: /kit-small\.bin \(1\.0 GB\).*kit-medium\.gguf \(1 MB\)/ } },
  { id: 'big', what: 'a model above 2 GiB, two drives (64-bit sizes end to end)', big: true,
    models: [{ m: BIG, mode: 'ok' }], cross: true,
    expect: { exit: 0, placed: [BIG], requests: { [BIG.file]: 2 }, readme: true } },
];

// -----------------------------------------------------------------------------------------------------------------
// Mutation sentinels: each mutant of models_download.iss (find -> replace, exactly once) must make one of its cases fail.
// -----------------------------------------------------------------------------------------------------------------
const MUTANTS = [
  { id: 'size_ignored', why: 'a model in place is kept whatever its size',
    find: '  Result := MdlHasSize(MdlFinal(i), MdlSize[i]);', repl: '  Result := FileExists(MdlFinal(i));', cases: ['wrong_size'] },
  { id: 'no_part', why: 'the copy on the installation drive gets the final name at once (no .part)',
    find: "  Result := MdlStageDir + '\\' + MdlFile[i] + '.part';", repl: "  Result := MdlStageDir + '\\' + MdlFile[i];", cases: ['cross_drive'] },
  { id: 'part_size_unchecked', why: 'a damaged .part is put in place',
    find: "  if not MdlHasSize(MdlPart(i), MdlSize[i]) then begin\n    DeleteFile(MdlPart(i));\n    MdlStaged[i] := False;\n    Result := 'its downloaded copy",
    repl: "  if False then begin\n    DeleteFile(MdlPart(i));\n    MdlStaged[i] := False;\n    Result := 'its downloaded copy", cases: ['truncated_part'] },
  { id: 'old_deleted_first', why: 'the previous model is deleted before the new one is ready',
    find: "function MdlPlace(const i: Integer): String;\nvar Tries: Integer; Done: Boolean;\nbegin\n  Result := '';",
    repl: "function MdlPlace(const i: Integer): String;\nvar Tries: Integer; Done: Boolean;\nbegin\n  Result := '';\n  DeleteFile(MdlFinal(i));", cases: ['truncated_tmp'] },
  { id: 'leftovers_kept', why: 'a *.part left by an interrupted Setup stays',
    find: "procedure MdlDeleteLeftovers(const Dir: String);\nvar R: TFindRec; Names: array of String; i, k, n: Integer; Mine: Boolean;\nbegin\n",
    repl: "procedure MdlDeleteLeftovers(const Dir: String);\nvar R: TFindRec; Names: array of String; i, k, n: Integer; Mine: Boolean;\nbegin\n  Exit;\n", cases: ['leftover_part'] },
  { id: 'no_room_check', why: 'the free space is not checked',
    find: "  Result := '';\n  Tmp := ExpandConstant('{tmp}');", repl: "  Result := '';\n  Total := 0;\n  Exit;\n  Tmp := ExpandConstant('{tmp}');", cases: ['no_room'] },
  { id: 'tmp_needs_all', why: '{tmp}\'s drive must hold every model (the review\'s S2)',
    find: '        else if MdlSize[i] > TmpNeed then\n          TmpNeed := MdlSize[i];', repl: '        else\n          TmpNeed := TmpNeed + MdlSize[i];', cases: ['tmp_largest_only'] },
  { id: 'sha_dropped', why: 'the download is not checked against its SHA-256',
    find: 'MdlPage.Add(MdlUrl[i], MdlFile[i], MdlSha256[i]);', repl: "MdlPage.Add(MdlUrl[i], MdlFile[i], '');", cases: ['bad_hash'] },
  { id: 'acl_kept', why: 'a model renamed in from {tmp} keeps {tmp}\'s permissions',
    find: 'function MdlResetAcl(const F: String): Boolean;\nvar Tries, Rc: Integer;\nbegin\n  Result := False;',
    repl: 'function MdlResetAcl(const F: String): Boolean;\nvar Tries, Rc: Integer;\nbegin\n  Result := True;\n  Exit;', cases: ['fresh'] },
  { id: 'exit10_dropped', why: 'a model not put in place still exits 0',
    find: "  if MdlMissing <> '' then\n    Result := MdlExitMissing", repl: "  if False then\n    Result := MdlExitMissing", cases: ['locked'] },
  { id: 'exit1_silent', why: 'a silent failure ends with exit code 1 ("failed to initialize"), not 7',
    find: "        MdlStepError := 'The AI models could not be downloaded, so nothing was installed.';\n      Result := True;",
    repl: "        MdlStepError := 'The AI models could not be downloaded, so nothing was installed.';", cases: ['http_500'] },
  { id: 'verify_ignored', why: '/VERIFYMODELS is not recognised',
    find: "'/VERIFYMODELS') = 0 then", repl: "'/VERIFYMODELS-NOT') = 0 then", cases: ['verify_damaged'] },
  { id: 'staged_kept', why: 'a .part (and the folders made for it) stay when Setup stops',
    find: 'procedure MdlDeinitializeSetup;\nbegin\n  MdlDropStaged;\n  MdlRemoveMadeDirs;\nend;', repl: 'procedure MdlDeinitializeSetup;\nbegin\nend;', cases: ['cross_cancel'] },
  { id: 'staged_late', why: 'on another drive, the models wait in {tmp} until the install (the review\'s S2)',
    find: "      if (Err = '') and Cross and not MdlStaged[i] then\n        Err := MdlStage(i);", repl: "      if False then\n        Err := MdlStage(i);", cases: ['cross_drive'] },
  { id: 'rm_unregistered', why: 'a model about to be replaced is not checked for being in use',
    find: '      RegisterExtraCloseApplicationsResource(False, MdlFinal(i));\n', repl: '', cases: ['rm_register'] },
  { id: 'mb_1024', why: 'just under 1 GiB reads "1024 MB"',
    find: '  if MB < 1024 then', repl: '  if Bytes < 1073741824 then', cases: ['memo_units'] },
];

// -----------------------------------------------------------------------------------------------------------------
function parseArgs(argv) {
  const o = { mutants: false, big: false, keep: false, only: [] };
  for (const a of argv) {
    if (a === '--mutants') o.mutants = true;
    else if (a === '--big') o.big = true;
    else if (a === '--keep') o.keep = true;
    else if (/^[a-z0-9_]+$/.test(a) && CASES.some((c) => c.id === a)) o.only.push(a);
    else throw new Error(`unknown argument ${a}`);
  }
  return o;
}

function startServer(ctx) {
  return new Promise((resolve, reject) => {
    const s = cp.spawn(process.execPath, [path.join(__dirname, 'serve_models.js'), ctx.served, ctx.serverLog, ctx.hfHead], { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
    s.once('exit', (code) => reject(new Error('serve_models.js exited (' + code + ')')));
    s.stdout.once('data', (d) => { const m = /listening (\d+)/.exec(String(d)); if (!m) reject(new Error('serve_models.js: ' + d)); else resolve({ proc: s, port: Number(m[1]) }); });
  });
}
const freePort = () => new Promise((resolve) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
const serverRequests = (ctx) => { try { return fs.readFileSync(ctx.serverLog, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l)); } catch (_) { return []; } };

// A child process holding `file` with an exclusive lock (share mode 0) until killed.
function holdExclusive(file) {
  return new Promise((resolve, reject) => {
    const h = cp.spawn(process.execPath, ['-e', `const fs=require('fs');fs.openSync(${JSON.stringify(file)}, fs.constants.O_RDONLY | fs.constants.UV_FS_O_EXLOCK);process.stdout.write('held\\n');setInterval(()=>{},1000);`], { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
    h.once('exit', (code) => reject(new Error('the lock holder exited (' + code + ')')));
    h.stdout.once('data', () => resolve(h));
  });
}

function caseModels(c, ctx) {
  if (c.models === 'tmpLargest') {
    const each = Math.floor(freeBytes(ctx.a) * 0.55);
    return ['tl-a.bin', 'tl-b.bin', 'tl-c.bin'].map((n) => ({ name: n, size: each, sha256: ctx.sha[SMALL.file], served: SMALL.file,
      url: `http://127.0.0.1:${ctx.port}/hf/500/${c.id}__${SMALL.file}` }));
  }
  return c.models.map((x) => ({ name: x.m.file, size: x.size === undefined ? x.m.size : x.size, sha256: x.sha256 || ctx.sha[x.m.file], served: x.m.file,
    url: `http://127.0.0.1:${x.down ? ctx.downPort : ctx.port}/hf/${x.mode}/${c.id}__${x.m.file}` }));
}

function skipReason(c, ctx) {
  if (c.big && !ctx.opts.big) return 'the >2 GiB case runs with --big';
  if (c.cross && !ctx.x) return 'no second drive: the repository is on %TEMP%\'s drive and SUVTITLE_KIT_XDIR is not set';
  if (c.models === 'tmpLargest') {
    const each = Math.floor(freeBytes(ctx.a) * 0.55);
    if (freeBytes(ctx.x) < 3 * each + 2 * 1073741824) return `the second drive has less free space than 3 x ${each} bytes`;
  }
  return null;
}

async function runCase(c, ctx, regionFile, tag) {
  const r = { id: c.id, what: c.what, problems: [], tag };
  const why = skipReason(c, ctx);
  if (why) return { ...r, skipped: why };
  const root = c.cross ? ctx.x : ctx.a;
  const inst = path.join(root, 'inst-' + c.id);
  const models = path.join(inst, 'models');
  const tmp = path.join(ctx.a, 'tmp-' + c.id);
  rm(inst); rm(tmp);
  fs.mkdirSync(tmp, { recursive: true });
  if (!c.noPreDir) fs.mkdirSync(inst, { recursive: true });
  if (c.acl) {
    // an inheritable entry (Users: read) the install folder has and {tmp} has not: a file RENAMED in from {tmp} keeps
    // {tmp}'s permissions unless they are reset
    const g = cp.spawnSync('icacls', [inst, '/grant', '*S-1-5-32-545:(OI)(CI)(RX)'], { encoding: 'utf8', windowsHide: true });
    if (g.status !== 0) r.problems.push('icacls /grant failed: ' + g.stdout);
  }
  const before = {};
  if (c.preplace || c.leftovers) fs.mkdirSync(models, { recursive: true });
  for (const [f, kind] of Object.entries(c.preplace || {})) {
    const dst = path.join(models, f);
    if (kind === 'real') fs.copyFileSync(path.join(ctx.served, f), dst);
    else if (kind === 'junk') fs.writeFileSync(dst, Buffer.alloc(1000, 0x5a));
    else if (kind === 'corrupt') { fs.copyFileSync(path.join(ctx.served, f), dst); const fd = fs.openSync(dst, 'r+'); fs.writeSync(fd, Buffer.from('CORRUPT!'), 0, 8, 5000); fs.closeSync(fd); }
    before[f] = sha256File(dst);
  }
  for (const f of c.leftovers || []) fs.writeFileSync(path.join(models, f), Buffer.alloc(777, 0x33));
  // the case file: the models block (the generator of the real one), the output names, the knobs
  const list = caseModels(c, ctx);
  const caseFile = path.join(ctx.a, 'cases', `${c.id}${tag ? '_' + tag : ''}.iss`);
  const lines = [`; generated by tests\\installer\\run_installer_kit.js: case ${c.id}${tag ? ' (mutant ' + tag + ')' : ''}`,
    ...gen.buildModelsBlock(list),
    `#define OutDir "${path.join(ctx.a, 'setups')}"`, `#define OutBase "kit_${c.id}${tag ? '_' + tag : ''}"`,
    `#define ReadmeFile "${path.join(ctx.a, 'readme.txt')}"`, `#define CloseApps "${c.closeApps || 'no'}"`];
  if (c.silentAnswer) lines.push(`#define MdlSilentAnswer "${c.silentAnswer}"`);
  if (c.truncateTmp) lines.push(`#define KitTruncateTmp "${c.truncateTmp}"`);
  if (c.truncatePart) lines.push(`#define KitTruncatePart "${c.truncatePart}"`);
  fs.mkdirSync(path.dirname(caseFile), { recursive: true });
  fs.writeFileSync(caseFile, lines.join('\r\n') + '\r\n');
  const comp = cp.spawnSync(ctx.iscc, ['/Q', `/DCaseFile=${caseFile}`, ...(regionFile ? [`/DRegionFile=${regionFile}`] : []), DLTEST], { encoding: 'utf8', windowsHide: true, timeout: 180000 });
  if (comp.status !== 0) { r.problems.push('ISCC: ' + ((comp.stdout || '') + (comp.stderr || '')).trim().split(/\r?\n/).slice(-3).join(' | ')); r.compileError = true; return r; }
  const exe = path.join(ctx.a, 'setups', `kit_${c.id}${tag ? '_' + tag : ''}.exe`);
  const log = path.join(ctx.a, 'logs', `${c.id}${tag ? '_' + tag : ''}.log`);
  fs.mkdirSync(path.dirname(log), { recursive: true });
  rm(log);
  let holder = null;
  if (c.lock) holder = await holdExclusive(path.join(models, c.lock));
  const reqBefore = serverRequests(ctx).length;
  const t0 = Date.now();
  const run = cp.spawnSync(exe, ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', `/LOG=${log}`, `/DIR=${inst}`, ...(c.args || [])],
    { windowsHide: true, timeout: 900000, env: { ...process.env, TMP: tmp, TEMP: tmp } });
  r.ms = Date.now() - t0;
  if (holder) holder.kill();
  r.exit = run.status;
  const reqs = serverRequests(ctx).slice(reqBefore).filter((q) => q.path.includes(`/${c.id}__`));
  r.requests = reqs.map((q) => `${q.path.replace(/^\/(hf|cdn)\/([a-z0-9]+)\/[a-z0-9_]+__/, '/$1/$2/')} ${q.status}`);
  const text = exists(log) ? fs.readFileSync(log, 'utf8') : '';
  r.logLines = text.split(/\r?\n/).filter((l) => /Models: |KIT |Exception|hash|Found \d+ files|exit code|abort/i.test(l)).map((l) => l.replace(/^\S+ \S+\s+/, '')).slice(0, 80);
  const listing = (tagName) => text.split(/\r?\n/).map((l) => new RegExp(`KIT ${tagName} file (.+) (\\d+)$`).exec(l)).filter(Boolean).map((m) => m[1]);
  // --- expectations -------------------------------------------------------------------------------------------
  const e = c.expect, p = r.problems;
  if (r.exit !== e.exit) p.push(`exit code ${r.exit}, expected ${e.exit}`);
  for (const m of e.placed || []) {
    const f = path.join(models, m.file);
    if (!exists(f)) p.push(`${m.file} not in place`);
    else if (fs.statSync(f).size !== m.size || sha256File(f) !== ctx.sha[m.file]) p.push(`${m.file} in place but not the served bytes`);
  }
  for (const m of e.unchanged || []) {
    const f = path.join(models, m.file);
    if (!exists(f) || sha256File(f) !== before[m.file]) p.push(`${m.file} was changed (it had to stay as it was)`);
  }
  for (const m of e.absent || []) if (exists(path.join(models, m.file))) p.push(`${m.file} is there (it must not be)`);
  for (const [f, n] of Object.entries(e.requests || {})) {
    const got = reqs.filter((q) => q.path.endsWith(`__${f}`)).length;
    if (got !== n) p.push(`${f}: ${got} request(s), expected ${n}`);
  }
  if (e.totalRequests !== undefined && reqs.length !== e.totalRequests) p.push(`${reqs.length} request(s), expected ${e.totalRequests}`);
  if (e.readme !== undefined && exists(path.join(inst, 'readme.txt')) !== e.readme) p.push(e.readme ? 'the setup\'s own file was not installed' : 'the setup installed its file (it had to stop before)');
  if (e.instGone && exists(inst)) p.push(`${inst} is still there (made by the setup; it had to go)`);
  if (exists(models)) {
    const parts = fs.readdirSync(models).filter((n) => /\.part$/i.test(n));
    if (parts.length) p.push('*.part left: ' + parts.join(', '));
  }
  const leftTmp = exists(tmp) ? fs.readdirSync(tmp).filter((n) => /^is-.*\.tmp$/i.test(n)) : [];
  if (leftTmp.length) p.push('Setup\'s temporary folder was not removed: ' + leftTmp.join(', '));
  for (const re of e.log || []) if (!re.test(text)) p.push('the log lacks ' + re);
  for (const re of e.noLog || []) if (re.test(text)) p.push('the log has ' + re);
  if (e.memo) {
    const m = /KIT memo: (.*)$/m.exec(text);
    if (!m) p.push('no Ready-page memo in the log');
    else if (!e.memo.test(m[1])) p.push('memo: ' + m[1]);
  }
  if (e.atInstall) {
    const t = listing('install-tmp'), mo = listing('install-models');
    if (!text.includes('KIT tmp=')) p.push('the setup never reached the installation');
    for (const f of e.atInstall.tmpHas || []) if (!t.includes(f)) p.push(`at the installation {tmp} lacks ${f}`);
    for (const f of e.atInstall.tmpLacks || []) if (t.includes(f)) p.push(`at the installation {tmp} still holds ${f}`);
    for (const f of e.atInstall.modelsHas || []) if (!mo.includes(f)) p.push(`at the installation models\\ lacks ${f}`);
    for (const f of e.atInstall.modelsLacks || []) if (mo.includes(f)) p.push(`at the installation models\\ already holds ${f}`);
  }
  if (c.acl && exists(path.join(inst, 'readme.txt'))) {
    const ref = icaclsAces(path.join(inst, 'readme.txt'));
    for (const m of e.placed || []) {
      const f = path.join(models, m.file);
      if (exists(f) && JSON.stringify(icaclsAces(f)) !== JSON.stringify(ref)) p.push(`${m.file}: its permissions are not those of a file Inno installed there (${icaclsAces(f).join(' ')})`);
    }
  }
  r.pass = p.length === 0;
  return r;
}

async function main(argv) {
  let opts;
  try { opts = parseArgs(argv); } catch (e) { say('XX ' + e.message + '\nusage: node tests\\installer\\run_installer_kit.js [--mutants] [--big] [--keep] [case ...]'); return 2; }
  const iscc = gen.findIscc();
  if (!iscc) { say('NOT CHECKED: ISCC.exe (Inno Setup 6) not found -- set SUVTITLE_ISCC or put it on PATH.'); return 1; }
  const x = xRoot();
  const ctx = { opts, iscc, a: A_ROOT, x, served: path.join(A_ROOT, 'served'), serverLog: path.join(A_ROOT, 'server.log'), hfHead: path.join(A_ROOT, 'hfhead.json'), sha: {} };
  rm(A_ROOT);
  if (x) rm(x);
  fs.mkdirSync(ctx.served, { recursive: true });
  if (x) fs.mkdirSync(x, { recursive: true });
  fs.writeFileSync(path.join(A_ROOT, 'readme.txt'), 'the installer test kit\'s throwaway setup\r\n');
  fs.writeFileSync(ctx.serverLog, '');
  fs.writeFileSync(ctx.hfHead, '{}');
  say(`SuVtitle installer kit -- ${REGION}`);
  say(`  ISCC   : ${iscc}`);
  say(`  work   : ${A_ROOT}${x ? '   second drive: ' + x : '   (no second drive: the two-drive cases are NOT CHECKED)'}`);
  for (const m of [SMALL, MEDIUM, ...(opts.big ? [BIG] : [])]) {
    writeModel(path.join(ctx.served, m.file), m.size);
    ctx.sha[m.file] = sha256File(path.join(ctx.served, m.file));
  }
  const srv = await startServer(ctx);
  ctx.port = srv.port;
  ctx.downPort = await freePort();
  let bad = 0, notChecked = 0;
  const results = [];
  try {
    say('\n== cases');
    const baseline = {};
    for (const c of CASES) {
      if (opts.only.length && !opts.only.includes(c.id)) continue;
      const r = await runCase(c, ctx, null, '');
      baseline[c.id] = r;
      results.push(r);
      if (r.skipped) { notChecked++; say(`  NOT CHECKED  ${c.id}: ${r.skipped}`); continue; }
      if (!r.pass) bad++;
      say(`  ${r.pass ? 'PASS' : 'FAIL'}  ${c.id}  (exit ${r.exit}, ${r.ms} ms)  ${c.what}`);
      if (!r.pass) { for (const q of r.problems) say('        ' + q); say('        requests: ' + r.requests.join(', ')); for (const l of r.logLines.slice(0, 30)) say('        | ' + l); }
    }
    say('\n== check_models.js (the release check of the pinned URLs), against the loopback server');
    bad += await checkModelsTool(ctx);
    if (opts.mutants) {
      say('\n== mutation sentinels (installer\\models_download.iss)');
      const src = fs.readFileSync(REGION, 'utf8').replace(/\r\n/g, '\n');
      let killed = 0, ran = 0;
      for (const mu of MUTANTS) {
        const cases = mu.cases.map((id) => CASES.find((c) => c.id === id));
        const base = mu.cases.map((id) => baseline[id]);
        if (base.some((b) => !b || b.skipped || !b.pass)) { say(`  NOT RUN   ${mu.id}: its case(s) ${mu.cases.join(', ')} did not pass (or were not run) unmutated`); notChecked++; continue; }
        const n = src.split(mu.find).length - 1;
        if (n !== 1) { say(`  INVALID   ${mu.id}: its text occurs ${n} times in models_download.iss`); bad++; continue; }
        const text = src.replace(mu.find, () => mu.repl);
        const file = path.join(A_ROOT, 'mutants', mu.id + '.iss');
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, text.replace(/\n/g, '\r\n'));
        ran++;
        const rs = [];
        for (const c of cases) rs.push(await runCase(c, ctx, file, mu.id));
        if (rs.some((r) => r.compileError)) { bad++; say(`  INVALID   ${mu.id}: it does not compile (${rs.find((r) => r.compileError).problems[0]})`); continue; }
        const caught = rs.filter((r) => !r.pass);
        if (caught.length) { killed++; say(`  KILLED    ${mu.id} (${mu.why}) by ${caught.map((r) => r.id + ': ' + r.problems[0]).join(' / ')}`); }
        else { bad++; say(`  SURVIVED  ${mu.id} (${mu.why}): ${mu.cases.join(', ')} still pass`); }
      }
      say(`  ${killed} of ${ran} mutants killed`);
    }
  } finally {
    srv.proc.kill();
    fs.writeFileSync(path.join(A_ROOT, 'results.json'), JSON.stringify(results, null, 2));
    if (!opts.keep) { rm(A_ROOT); if (x) rm(x); }
  }
  say(bad ? `\nFAILED: ${bad} problem(s)` : `\nOK${notChecked ? ` (${notChecked} NOT CHECKED)` : ''}`);
  return bad ? 1 : 0;
}

// check_models.js against the loopback server: a clean answer for every model passes; four kinds of lies are each caught.
async function checkModelsTool(ctx) {
  const { loadModels } = require(path.join(REPO, 'deploy', 'make_bundle.js'));
  const models = loadModels();
  const table = (lie) => {
    const t = {};
    models.forEach((m, i) => {
      const p = m.url.slice('https://huggingface.co'.length);
      const h = { Location: `https://cdn.example.invalid/${m.sha256}`, 'X-Linked-Size': String(m.size), 'X-Linked-ETag': `"${m.sha256}"` };
      let status = 302;
      if (lie && i === 0) h['X-Linked-Size'] = String(m.size - 1);
      if (lie && i === 1) h['X-Linked-ETag'] = `"${'0'.repeat(64)}"`;
      if (lie && i === 2) status = 200;
      if (lie && i === 3) status = 404;
      t[p] = { status, headers: h };
    });
    return t;
  };
  const run = () => cp.spawnSync(process.execPath, [path.join(REPO, 'installer', 'check_models.js'), '--json', '--timeout', '10', '--origin', `http://127.0.0.1:${ctx.port}`], { encoding: 'utf8', windowsHide: true, timeout: 120000 });
  let bad = 0;
  fs.writeFileSync(ctx.hfHead, JSON.stringify(table(false)));
  const clean = run();
  let rows = [];
  try { rows = JSON.parse(clean.stdout); } catch (_) { rows = []; }
  const cleanOk = clean.status === 0 && rows.length === models.length && rows.every((r) => r.problems.length === 0);
  say(`  ${cleanOk ? 'PASS' : 'FAIL'}  every model served as pinned (a 302 with X-Linked-Size and X-Linked-ETag) -> exit 0`);
  if (!cleanOk) { bad++; say('        ' + (clean.stdout + clean.stderr).slice(0, 600)); }
  fs.writeFileSync(ctx.hfHead, JSON.stringify(table(true)));
  const lying = run();
  try { rows = JSON.parse(lying.stdout); } catch (_) { rows = []; }
  const want = [/X-Linked-Size \d+, models\.json pins/, /X-Linked-ETag 0{64}, models\.json pins/, /HTTP 200, expected a redirect/, /HTTP 404, expected a redirect/];
  const lieOk = lying.status === 1 && rows.length === models.length
    && want.every((re, i) => rows[i].problems.some((q) => re.test(q))) && rows.slice(4).every((r) => r.problems.length === 0);
  say(`  ${lieOk ? 'PASS' : 'FAIL'}  a wrong size, a wrong SHA-256, a 200 and a 404 are each named, the two right ones are not -> exit 1`);
  if (!lieOk) { bad++; say('        ' + JSON.stringify(rows.map((r) => r.problems)).slice(0, 800)); }
  const offline = cp.spawnSync(process.execPath, [path.join(REPO, 'installer', 'check_models.js'), '--timeout', '5', '--origin', `http://127.0.0.1:${ctx.downPort}`], { encoding: 'utf8', windowsHide: true, timeout: 120000 });
  const offOk = offline.status === 1 && /unreachable/.test(offline.stdout);
  say(`  ${offOk ? 'PASS' : 'FAIL'}  an origin that does not answer -> "unreachable", exit 1`);
  if (!offOk) { bad++; say('        ' + (offline.stdout + offline.stderr).slice(0, 400)); }
  return bad;
}

if (require.main === module) main(process.argv.slice(2)).then((c) => { process.exitCode = c; }, (e) => { console.error(e); process.exitCode = 1; });
module.exports = { CASES, MUTANTS };

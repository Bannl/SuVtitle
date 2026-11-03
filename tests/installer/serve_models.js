// =============================================================================
// SuVtitle - tests/installer/serve_models.js: the installer test kit's stand-in for Hugging Face, on 127.0.0.1 only.
// Started by run_installer_kit.js; never contacts anything.
//
//   node serve_models.js <files dir> <request log> [<hf-head json>]
//   prints "listening <port>" once ready (an ephemeral port).
//
// Downloads (the throwaway setups), like huggingface.co/<repo>/resolve/<commit>/<file> and its CDN:
//   /hf/<mode>/<alias>   302 to /cdn/<ok|bad>/<alias>?X-Amz-Signature=... (with X-Linked-Size), or an error, by <mode>:
//                          ok       the file
//                          bad      the file with ONE byte flipped (same size: only its SHA-256 tells)
//                          500      HTTP 500
//                          flaky    the first request of that alias: 503; then like ok
//                          badonce  the first request of that alias: the bad bytes; then like ok
//   /cdn/<ok|bad>/<alias>  200, the file (bad: one byte flipped)
//   <alias> is "<case>__<file>" (the case keeps its own counters) or "<file>"; <file> is read from <files dir>.
// check_models.js (--origin): any other path is looked up in <hf-head json>, read again at each request:
//   { "/<repo>/resolve/<commit>/<path>": { "status": 302, "headers": { "Location": "...", "X-Linked-Size": "...", ... } } }
// Every request is appended to <request log>, one JSON line.
// =============================================================================
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const [filesDir, logFile, headFile] = process.argv.slice(2);
if (!filesDir || !logFile) { console.error('usage: node serve_models.js <files dir> <request log> [<hf-head json>]'); process.exit(2); }
const seen = new Map();
const log = (o) => fs.appendFileSync(logFile, JSON.stringify({ t: Date.now(), ...o }) + '\n');
const fileOf = (alias) => {
  const name = alias.includes('__') ? alias.slice(alias.indexOf('__') + 2) : alias;
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)) return null;
  const p = path.join(filesDir, name);
  try { return fs.statSync(p).isFile() ? p : null; } catch (_) { return null; }
};

const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const done = (status, extra) => log({ method: req.method, path: u.pathname, status, ...extra });
  const m = /^\/(hf|cdn)\/([a-z0-9]+)\/([^/]+)$/.exec(u.pathname);
  if (!m) {
    let table = {};
    try { if (headFile) table = JSON.parse(fs.readFileSync(headFile, 'utf8')); } catch (_) { table = {}; }
    const e = table[u.pathname];
    if (!e) { res.writeHead(404); res.end(); return done(404); }
    res.writeHead(e.status, e.headers || {});
    res.end();
    return done(e.status, { head: true });
  }
  const [, tier, mode, alias] = m;
  const file = fileOf(alias);
  if (!file) { res.writeHead(404); res.end(); return done(404, { alias }); }
  const size = fs.statSync(file).size;
  if (tier === 'hf') {
    const n = (seen.get(mode + '/' + alias) || 0) + 1;
    seen.set(mode + '/' + alias, n);
    if (mode === '500') { res.writeHead(500, { 'Content-Type': 'text/plain' }); res.end('Internal Error'); return done(500); }
    if (mode === 'flaky' && n === 1) { res.writeHead(503, { 'Content-Type': 'text/plain' }); res.end('Service Unavailable'); return done(503); }
    if (!['ok', 'bad', 'flaky', 'badonce'].includes(mode)) { res.writeHead(404); res.end(); return done(404); }
    const target = mode === 'bad' || (mode === 'badonce' && n === 1) ? 'bad' : 'ok';
    res.writeHead(302, { Location: `/cdn/${target}/${alias}?X-Amz-Signature=kit&n=${n}`, 'X-Linked-Size': String(size) });
    res.end();
    return done(302, { to: target });
  }
  if (mode !== 'ok' && mode !== 'bad') { res.writeHead(404); res.end(); return done(404); }
  res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': String(size) });
  if (req.method === 'HEAD') { res.end(); return done(200, { head: true }); }
  let sent = 0;
  const rs = fs.createReadStream(file, { highWaterMark: 1 << 20 });
  rs.on('data', (chunk) => {
    if (mode === 'bad' && sent <= 1000 && sent + chunk.length > 1000) { chunk = Buffer.from(chunk); chunk[1000 - sent] ^= 0xff; }
    sent += chunk.length;
    if (!res.write(chunk)) { rs.pause(); res.once('drain', () => rs.resume()); }
  });
  rs.on('end', () => { res.end(); done(200, { bytes: sent, bad: mode === 'bad' }); });
  rs.on('error', (e) => { res.destroy(e); done(599, { error: e.message }); });
});
srv.listen(0, '127.0.0.1', () => { console.log('listening ' + srv.address().port); });

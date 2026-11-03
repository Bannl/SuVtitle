#!/usr/bin/env node
'use strict';
// =============================================================================
// SuVtitle - installer/check_models.js: a RELEASE-CHECKLIST tool, run before publishing a light installer.
//
// The light installer downloads the AI models from the URLs installer/models.json pins to a Hugging Face commit. A pinned
// commit guarantees the bytes, not that they can still be fetched: a repository deleted, gated, renamed or rewritten
// breaks every new install. This checks each URL WITHOUT downloading anything: one HEAD request per model, no redirect
// followed. Hugging Face answers a Git LFS / Xet file with a redirect to its CDN plus the file's size (X-Linked-Size)
// and SHA-256 (X-Linked-ETag, the LFS oid); both must be the ones models.json pins.
//
// Usage: node installer\check_models.js [--json] [--timeout <seconds>]
//        --origin <http(s)://host[:port]>   tests only: ask this origin instead of https://huggingface.co
//                                           (tests\installer\run_installer_kit.js points it at its loopback server)
// Exit code: 0 = every model is served as pinned; 1 = one is not, or cannot be reached; 2 = bad command line.
// =============================================================================

const http = require('http');
const https = require('https');
const path = require('path');
const { loadModels, MODELS_JSON } = require(path.join(__dirname, '..', 'deploy', 'make_bundle.js'));

const HF = 'https://huggingface.co';
const REDIRECTS = [301, 302, 303, 307, 308];

// "W/\"abc\"" -> "abc", lower case
function normalizeEtag(v) {
  return String(v || '').trim().replace(/^W\//i, '').replace(/^"(.*)"$/, '$1').toLowerCase();
}

// What is wrong with one HEAD answer for `model` ({ size, sha256 }); [] when it is served as pinned.
// `res`: { error } or { status, headers } (header names in lower case, as Node gives them).
function judge(model, res) {
  if (res.error) return [`unreachable: ${res.error}`];
  const p = [];
  const h = res.headers || {};
  if (!REDIRECTS.includes(res.status)) p.push(`HTTP ${res.status}, expected a redirect to the file (deleted, gated, renamed?)`);
  else if (!h.location) p.push(`HTTP ${res.status} without a Location`);
  if (h['x-linked-size'] === undefined) p.push('no X-Linked-Size');
  else if (String(h['x-linked-size']).trim() !== String(model.size)) p.push(`X-Linked-Size ${h['x-linked-size']}, models.json pins ${model.size}`);
  if (h['x-linked-etag'] === undefined) p.push('no X-Linked-ETag');
  else if (normalizeEtag(h['x-linked-etag']) !== model.sha256.toLowerCase()) p.push(`X-Linked-ETag ${normalizeEtag(h['x-linked-etag'])}, models.json pins ${model.sha256}`);
  return p;
}

// One HEAD request, no redirect followed. Resolves { status, headers } or { error }.
function head(url, timeoutMs) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (v) => { if (!done) { done = true; resolve(v); } };
    let req;
    try {
      const mod = url.startsWith('https:') ? https : http;
      req = mod.request(url, { method: 'HEAD', headers: { 'User-Agent': 'SuVtitle-check_models/1.0' } }, (res) => {
        res.resume();
        finish({ status: res.statusCode, headers: res.headers });
      });
    } catch (e) { finish({ error: e.message }); return; }
    req.setTimeout(timeoutMs, () => { req.destroy(new Error(`no answer in ${timeoutMs / 1000} s`)); });
    req.on('error', (e) => finish({ error: e.message }));
    req.end();
  });
}

function parseArgs(argv) {
  const o = { json: false, timeout: 20, origin: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => { if (i + 1 >= argv.length) throw new Error(`${a} needs a value`); return argv[++i]; };
    if (a === '--json') o.json = true;
    else if (a === '--timeout') { o.timeout = Number(val()); if (!(o.timeout > 0)) throw new Error('--timeout: a number of seconds'); }
    else if (a === '--origin') { o.origin = val().replace(/\/+$/, ''); if (!/^https?:\/\/[^/]+$/.test(o.origin)) throw new Error('--origin: http(s)://host[:port]'); }
    else throw new Error(`unknown option ${a}`);
  }
  return o;
}

async function main(argv) {
  let o, models;
  try { o = parseArgs(argv); } catch (e) {
    console.error(`XX ${e.message}\nusage: node installer\\check_models.js [--json] [--timeout <seconds>] [--origin <url>]`);
    return 2;
  }
  try { models = loadModels(); } catch (e) { console.error(`XX ${MODELS_JSON}: ${e.message}`); return 1; }
  const rows = [];
  for (const m of models) {
    const url = o.origin ? o.origin + m.url.slice(HF.length) : m.url;
    const res = await head(url, o.timeout * 1000);
    const problems = judge(m, res);
    let to = '';
    try { if (res.headers && res.headers.location) to = new URL(res.headers.location, url).host; } catch (_) { to = '?'; }
    rows.push({ file: m.name, url, status: res.status || null, redirect: to || null, problems });
    if (!o.json) {
      console.log(`  ${problems.length ? 'FAIL' : 'OK  '}  ${m.name}${problems.length ? '' : `  (${res.status} -> ${to}, ${m.size} bytes, SHA-256 as pinned)`}`);
      for (const p of problems) console.log(`        ${p}`);
    }
  }
  const bad = rows.filter((r) => r.problems.length).length;
  if (o.json) console.log(JSON.stringify(rows, null, 2));
  else console.log(bad ? `\n${bad} of ${rows.length} models are NOT served as installer\\models.json pins them` : `\nthe ${rows.length} models are served as installer\\models.json pins them`);
  return bad ? 1 : 0;
}

module.exports = { judge, head, normalizeEtag };
if (require.main === module) main(process.argv.slice(2)).then((c) => { process.exitCode = c; });

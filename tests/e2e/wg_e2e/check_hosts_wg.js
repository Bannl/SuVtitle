// check_hosts_wg.js -- the media host this kit feeds the page IS the product's (RV4-12, for the wg kit). T-WG, 2026-10-23.
//   node check_hosts_wg.js            exit 0 = runs\hosts.json (written by make_wg.js) holds exactly what the product's own C++ computes
//                                     for the same file: media key, host, URL, byte for byte
//   node check_hosts_wg.js --selftest exit 0 only if the SAME check FAILS when hosts.js is on the TEXTBOOK FNV basis (RV4-12's mistake)
// How: ..\we_e2e\fnv_twin.exe (built by ..\we_e2e\build_fnv_twin.cmd from fnv_twin.cpp, which #includes app\TrPlayer.h and calls the
// product's pbtrf::fileIdentity / mediaKey / folderKey / pctEncode and pbplayer::kMediaHostSuffix) answers "item<TAB><path>" with
// "<mediaKey><TAB><host><TAB><url>"; ..\we_e2e\hosts.js (what make_wg.js feeds the page with) answers the same question in JavaScript.
// The vectors, the media-identity corner cases and the textbook-basis control live in ..\we_e2e\check_hosts.js (T-E2E): this file only
// adds the proof for THIS kit's item (folder A's E01, the one file every G run plays), so it needs no vector of its own.
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const H = require("../we_e2e/hosts.js");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..", "..");
const WE = path.join(HERE, "..", "we_e2e");
const WORK = process.env.SUVTITLE_T_E2E_WORK || path.join(REPO, "scratchpad", "t-e2e", "fnv_twin");
const EXE = path.join(WORK, "fnv_twin.exe");

if (process.argv.includes("--selftest")) {
  const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { WE_HOSTS_MUTANT: "textbook" }), encoding: "utf8" });
  const caught = r.status === 1 && /CHECK_HOSTS_WG FAILED/.test(r.stdout || "");
  console.log(caught ? "SELFTEST OK -- with hosts.js on the TEXTBOOK basis (RV4-12's mistake) this check FAILS"
                     : "SELFTEST FAILED -- the check did not notice the textbook basis (exit " + r.status + ")");
  process.exit(caught ? 0 : 1);
}

// the helper: built when missing or older than any of its sources (the same rule as ..\we_e2e\check_hosts.js)
const sources = [path.join(WE, "fnv_twin.cpp"), path.join(REPO, "app", "TrFileStore.h"), path.join(REPO, "app", "TrPlayer.h"), path.join(REPO, "common", "TrPipe.h")];
const stale = !fs.existsSync(EXE) || sources.some((s) => fs.statSync(s).mtimeMs > fs.statSync(EXE).mtimeMs);
if (stale && !process.env.WE_HOSTS_MUTANT) {
  console.log("building fnv_twin.exe (" + (fs.existsSync(EXE) ? "a source is newer" : "missing") + ") ...");
  const b = spawnSync("cmd.exe", ["/d", "/c", path.join(WE, "build_fnv_twin.cmd")], { encoding: "utf8", env: process.env });
  if (b.status !== 0) { console.log((b.stdout || "") + (b.stderr || "")); console.log("CHECK_HOSTS_WG FAILED (the helper did not build)"); process.exit(1); }
}
if (!fs.existsSync(EXE)) { console.log("CHECK_HOSTS_WG FAILED: " + EXE + " missing (run ..\\we_e2e\\build_fnv_twin.cmd)"); process.exit(1); }

let pass = 0, fail = 0;
const ok = (name, cond, extra) => { if (cond) { pass++; console.log("  PASS  " + name); } else { fail++; console.log("  FAIL  " + name + (extra ? "  -> " + extra : "")); } };

let fed = null;
try { fed = JSON.parse(fs.readFileSync(path.join(HERE, "runs", "hosts.json"), "utf8")); } catch { /* reported below */ }
const items = fed && fed.items ? Object.entries(fed.items) : [];
ok("runs\\hosts.json lists the item the page is fed (run make_wg.js first)", items.length >= 1, fed ? "no `items`" : "missing");
if (items.length) {
  const r = spawnSync(EXE, [], { input: Buffer.from(items.map(([, it]) => "item\t" + it.path).join("\n") + "\n", "utf8"), encoding: "buffer", maxBuffer: 1 << 24 });
  const real = r.status === 0 ? r.stdout.toString("utf8").split("\n").map((s) => s.replace(/\r$/, "")) : [];
  items.forEach(([name, it], i) => {
    const [ckey, chost, curl] = (real[i] || "ERR").split("\t");
    const fresh = H.item(it.path);                 // what hosts.js computes NOW (the textbook basis under WE_HOSTS_MUTANT)
    ok(name + ": the C++ finds the file and builds its key + host + URL (" + (chost || real[i]) + ")", real[i] !== "ERR" && !!chost, real[i]);
    ok(name + ": hosts.js == the product (mediaKey, host, URL, byte for byte)", fresh.key === ckey && fresh.host === chost && fresh.url === curl,
       "kit " + fresh.host + " " + fresh.key + " | product " + chost + " " + ckey);
    ok(name + ": what make_wg.js fed the page == the product (the feed is current, not stale)", it.host === chost && it.url === curl && it.key === ckey,
       "fed " + it.host + " | product " + chost);
  });
}
console.log((fail ? "CHECK_HOSTS_WG FAILED" : "CHECK_HOSTS_WG OK") + " -- " + pass + " passed, " + fail + " failed");
process.exit(fail ? 1 : 0);

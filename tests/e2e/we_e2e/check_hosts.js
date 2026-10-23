// check_hosts.js -- the we_e2e kit's host names (hosts.js) against the PRODUCT's own C++ (RV4-12). T-E2E, 2026-10-23.
//   node check_hosts.js [--no-build]        exit 0 = the kit computes EXACTLY the product's keys, hosts and URLs
//
// How: fnv_twin.exe (built by build_fnv_twin.cmd from fnv_twin.cpp, which #includes app\TrPlayer.h and calls
// pbtrf::fnv1a64 / folderKey / mediaKey / fileIdentity / pctEncode and pbplayer::kMediaHostSuffix) answers a list of requests;
// hosts.js answers the same list; every answer must be identical. The list is (1) fixed vectors -- strings, folders, media
// identities (the empty string is the offset basis itself: it alone tells 1469598103934665603 from the textbook value) -- and
// (2) the kit's REAL items (runs\hosts.json, written by make_we.js: the exact paths whose hosts the page is fed), for which the C++
// reads the size and FILETIME itself, and builds host and URL the way Player::loadItem does.
// The check proves it can fail: it also computes the vectors with the TEXTBOOK basis (RV4-12's mistake) and requires THAT to
// disagree with the product -- a check that cannot see the old bug would pass for any basis.
//   node check_hosts.js --selftest          runs this check against RV4-12's mistake (hosts.js on the textbook basis) and exits 0 only
//                                           if it FAILS there -- the proof the check can fail
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const H = require("./hosts.js");

const HERE = __dirname;
if (process.argv.includes("--selftest")) {
  const r = spawnSync(process.execPath, [__filename, "--no-build"], { env: Object.assign({}, process.env, { WE_HOSTS_MUTANT: "textbook" }), encoding: "utf8" });
  const fails = ((r.stdout || "").match(/^ {2}FAIL /gm) || []).length;
  const caught = r.status === 1 && /CHECK_HOSTS FAILED/.test(r.stdout || "") && fails > 0;
  console.log(caught ? "SELFTEST OK -- with hosts.js on the TEXTBOOK basis (RV4-12's mistake) this check FAILS (" + fails + " failing lines)"
                     : "SELFTEST FAILED -- the check did not notice the textbook basis (exit " + r.status + ")");
  process.exit(caught ? 0 : 1);
}
const REPO = path.resolve(HERE, "..", "..", "..");
const WORK = process.env.SUVTITLE_T_E2E_WORK || path.join(REPO, "scratchpad", "t-e2e", "fnv_twin");
const EXE = path.join(WORK, "fnv_twin.exe");
let pass = 0, fail = 0;
const ok = (name, cond, extra) => { if (cond) { pass++; console.log("  PASS  " + name); } else { fail++; console.log("  FAIL  " + name + (extra ? "  -> " + extra : "")); } };

// ---- 1. the helper: built when missing or older than any of its sources --------------------------------------------------
const sources = [path.join(HERE, "fnv_twin.cpp"), path.join(REPO, "app", "TrFileStore.h"), path.join(REPO, "app", "TrPlayer.h"),
  path.join(REPO, "common", "TrPipe.h")];
const stale = !fs.existsSync(EXE) || sources.some((s) => fs.statSync(s).mtimeMs > fs.statSync(EXE).mtimeMs);
if (stale && !process.argv.includes("--no-build")) {
  console.log("building fnv_twin.exe (" + (fs.existsSync(EXE) ? "a source is newer" : "missing") + ") ...");
  const b = spawnSync("cmd.exe", ["/d", "/c", path.join(HERE, "build_fnv_twin.cmd")], { encoding: "utf8", env: process.env });
  if (b.status !== 0) { console.log((b.stdout || "") + (b.stderr || "")); console.log("CHECK_HOSTS FAILED (the helper did not build)"); process.exit(1); }
  console.log("  " + (b.stdout || "").trim().split("\n").pop());
}
if (!fs.existsSync(EXE)) { console.log("CHECK_HOSTS FAILED: " + EXE + " missing (run build_fnv_twin.cmd)"); process.exit(1); }

function ask(lines) {
  const r = spawnSync(EXE, [], { input: Buffer.from(lines.join("\n") + "\n", "utf8"), encoding: "buffer", maxBuffer: 1 << 24 });
  if (r.status !== 0) { console.log("fnv_twin.exe exited " + r.status); process.exit(1); }
  const out = r.stdout.toString("utf8").split("\n").map((s) => s.replace(/\r$/, ""));
  out.pop();                                                  // the empty tail after the last newline
  return out;
}

// ---- 2. the fixed vectors ------------------------------------------------------------------------------------------------
const STRINGS = ["", "a", "foobar", "dir\t", "dir\td:\\x y\\z", "\u00c9t\u00e9 \u4e2d\u6587", "tab\tinside"];
const FOLDERS = [
  "D:\\Work\\SuVtitle\\tests\\e2e\\media_gen\\we\\A",
  "D:\\Work\\SuVtitle\\tests\\e2e\\media_gen\\we\\Season 1",
  "D:\\Films\\Season 1", "d:\\films\\SEASON 1", "C:\\", "D:\\F\u00edlms\\\u00dcn\u00efcode \u4e2d\u6587"];
const MEDIA = [["D:\\Films\\Film.mp4", "10", "20"], ["d:\\films\\FILM.MP4", "10", "20"],
  ["D:\\x\\E01 Pilot.mp4", "3473319", "134000000000000000"], ["D:\\x\\big.mkv", "4294967301", "9223372036854775809"]];
const reqs = [...STRINGS.map((s) => "fnv\t" + s), ...FOLDERS.map((f) => "folder\t" + f), ...MEDIA.map((m) => "media\t" + m.join("\t"))];
const want = (basis) => [...STRINGS.map((s) => H.fnv1a64(s, basis)), ...FOLDERS.map((f) => H.folderKey(f, basis)),
  ...MEDIA.map(([p, s, t]) => H.mediaKey(p, s, t, basis))];
const cpp = ask(reqs);
console.log("== vectors: " + reqs.length + " requests, the product's C++ against hosts.js");
const kit = want(undefined);                                  // the kit's own default basis -- what make_we.js feeds the page
reqs.forEach((r, i) => ok(JSON.stringify(r).slice(1, -1).slice(0, 96) + "  " + cpp[i], cpp[i] === kit[i], "kit " + kit[i]));
ok("the empty string hashes to the offset basis itself: 1469598103934665603 = 0x" + BigInt(1469598103934665603n).toString(16),
   cpp[0] === BigInt(1469598103934665603n).toString(16).padStart(16, "0") && kit[0] === cpp[0], cpp[0]);

// ---- 3. the control: the textbook basis (RV4-12) MUST disagree -----------------------------------------------------------
const old = want(H.TEXTBOOK_BASIS);
ok("control: with the TEXTBOOK offset basis (0xcbf29ce484222325) every vector disagrees with the product -- this check would have caught RV4-12",
   cpp.every((c, i) => c !== old[i]), "textbook " + old.join(","));

// ---- 4. the kit's real items ----------------------------------------------------------------------------------------------
console.log("== the kit's real items (runs\\hosts.json)");
let hosts = null;
try { hosts = JSON.parse(fs.readFileSync(path.join(HERE, "runs", "hosts.json"), "utf8")); } catch { /* reported below */ }
const items = hosts && hosts.items ? Object.entries(hosts.items) : [];
ok("runs\\hosts.json lists the items the page is fed (run make_we.js first)", items.length >= 1, hosts ? "no `items`" : "missing");
if (items.length) {
  const real = ask(items.map(([, it]) => "item\t" + it.path));
  items.forEach(([name, it], i) => {
    const [ckey, chost, curl] = real[i].split("\t");
    const fresh = H.item(it.path);
    ok(name + ": the C++ finds the file and builds its key + host + URL (" + (chost || real[i]) + ")", real[i] !== "ERR" && !!chost, real[i]);
    ok(name + ": hosts.js == the product (mediaKey, host, URL, byte for byte)", fresh.key === ckey && fresh.host === chost && fresh.url === curl,
       "kit " + fresh.host + " " + fresh.key + " | product " + chost + " " + ckey);
    ok(name + ": what make_we.js fed the page == the product (the feed is current, not stale)",
       it.host === chost && it.url === curl && it.key === ckey, "fed " + it.host + " | product " + chost);
  });
  const wrong = H.item(items[0][1].path, H.TEXTBOOK_BASIS).host;
  ok("control: " + items[0][0] + " computed with the TEXTBOOK basis lands on another host (" + wrong + ") than the product's", wrong !== real[0].split("\t")[1]);
}
console.log((fail ? "CHECK_HOSTS FAILED" : "CHECK_HOSTS OK") + " -- " + pass + " passed, " + fail + " failed");
process.exit(fail ? 1 : 0);

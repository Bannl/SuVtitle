// hosts.js -- the product's media/folder keys and virtual-host URLs, in JavaScript, for the we_e2e kit (make_we.js feeds them to
// the page; check_hosts.js compares them with what the product's own C++ prints, through fnv_twin.exe). T-E2E, 2026-10-23.
//
// WHY THIS FILE EXISTS (RV4-12): the kit first computed the hosts with the TEXTBOOK FNV-1a offset basis
// (0xcbf29ce484222325 = 14695981039346656037); the product's fnv1a64 (app\TrFileStore.h) starts from 1469598103934665603 -- the
// same number with its last digit dropped, an old typo that cannot be corrected any more: every cache key, every .srt registry
// line and every virtual host of a released version is derived from it. The kit was only self-consistent (it fed the page its
// own hosts and read the same hosts back), so it never reproduced the product's URLs. check_hosts.js now compares the two.
//
// What the product does (app\TrFileStore.h, app\TrPlayer.h loadItem):
//   fnv1a64(bytes)        h = 1469598103934665603; for each byte: h ^= byte; h *= 1099511628211  (mod 2^64)
//   hex16(h)              %016llx
//   folderKey(folder)     hex16(fnv1a64("dir\t" + toUtf8(lowerW(folder))))
//   mediaKey(path,size,t) hex16(fnv1a64(toUtf8(lowerW(path)) + "\t" + size + "\t" + t))      t = the FILETIME (100 ns ticks)
//   host                  "f" + folderKey(folder) + ".suvtitle-media.example"
//   url                   "https://" + host + "/" + pctEncode(utf8(leaf)) + "?v=" + mediaKey
// lowerW = CharLowerBuffW; here String.toLowerCase(), which is the same for every ASCII path (the kit's media live under
// ...\tests\e2e\media_gen\we\...); check_hosts.js feeds the C++ the kit's real paths, so a path where the two differ is caught.
const fs = require("fs");
const path = require("path");

const PRODUCT_BASIS = 1469598103934665603n;     // app\TrFileStore.h:151 -- NOT the textbook 0xcbf29ce484222325
const TEXTBOOK_BASIS = 0xcbf29ce484222325n;     // kept ONLY so check_hosts.js can prove its check would catch the old mistake
const PRIME = 1099511628211n;
const MASK = 0xffffffffffffffffn;
const MEDIA_HOST_SUFFIX = ".suvtitle-media.example";   // app\TrPlayer.h kMediaHostSuffix (check_hosts.js checks it against the C++)

// WE_HOSTS_MUTANT=textbook: used ONLY by `check_hosts.js --selftest`, which runs the check against RV4-12's mistake and requires it to FAIL
const KIT_BASIS = process.env.WE_HOSTS_MUTANT === "textbook" ? TEXTBOOK_BASIS : PRODUCT_BASIS;

function fnv1a64(str, basis = KIT_BASIS) {
  let h = basis;
  for (const c of Buffer.from(str, "utf8")) { h ^= BigInt(c); h = (h * PRIME) & MASK; }
  return h.toString(16).padStart(16, "0");
}
const folderKey = (folder, basis) => fnv1a64("dir\t" + folder.toLowerCase(), basis);
const mediaKey = (p, size, filetime, basis) => fnv1a64(p.toLowerCase() + "\t" + size + "\t" + filetime, basis);
const folderHost = (folder, basis) => "f" + folderKey(folder, basis) + MEDIA_HOST_SUFFIX;
// RFC 3986 unreserved kept, everything else %XX (app\TrFileStore.h pctEncode; python's quote(safe="-._~"))
const quote = (s) => encodeURIComponent(s).replace(/[!'()*]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase());

// TrPlayer.h loadItem: the per-FOLDER host, the leaf, ?v=<mediaKey> (the FILETIME read from the file, like fileIdentity)
function item(p, basis) {
  const st = fs.statSync(p, { bigint: true });
  const ft = st.mtimeNs / 100n + 116444736000000000n;
  const key = mediaKey(p, st.size, ft, basis);
  const folder = path.dirname(p), leaf = path.basename(p), host = folderHost(folder, basis);
  return { path: p, leaf, host, key, url: "https://" + host + "/" + quote(leaf) + "?v=" + key,
           map: { __map: host + "=" + folder + ":denycors" } };
}

module.exports = { PRODUCT_BASIS, TEXTBOOK_BASIS, MEDIA_HOST_SUFFIX, fnv1a64, folderKey, mediaKey, folderHost, quote, item };

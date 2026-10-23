// paths.js -- where tests/web reads what it checks (S-WEB, 2026-10-23).
//  * THE PAGE: app/web/index.html -- SuVtitle's one page (AudioPB's ui/web/translator.html @641f1aa, its File mode only).
//  * THE C++ TWINS of the page's tables (TrFileStore.h mediaExts, SubtitleReader.h encodingTable / isSubtitleExt,
//    TrPlayer.h's range rules, MediaTracks.h's failure sentences...) and the app <-> page CONTRACT (which messages the
//    app handles and emits): SuVtitle's own sources -- app/ and, for the language door, engine/.
//    (Until the publication wave a source that had not landed yet was read from its AudioPB original with `git show` in a
//    sibling checkout; every source is in this tree now, so that fallback is gone and `fallbacks` stays an empty list.)
"use strict";
const fs = require("fs");
const path = require("path");

const SUV = path.resolve(__dirname, "..", "..");
const WEB = path.join(SUV, "app", "web");
// RV4-9 (review 23/10): the page AS THE HARNESS READS IT is LF, whatever the checkout made of it (core.autocrlf=true on
// Windows turns it CRLF): every hash, regex and sentinel is written against LF -- readPage() / lf() everywhere the
// page is read. SUV_PAGE=<file> points the whole harness at a COPY of the page (the CRLF / LF proof, kept out of the
// repository); its neighbours are still read from app/web/.
const PAGE = process.env.SUV_PAGE ? path.resolve(process.env.SUV_PAGE) : path.join(WEB, "index.html");
const lf = (s) => String(s).replace(/\r\n/g, "\n");
const readPage = () => lf(fs.readFileSync(PAGE, "utf8"));
const fallbacks = [];

// rel641 = the file's path at 641f1aa ("ui/TrFileStore.h", "translator/TrEngine.cpp"): SuVtitle keeps the file NAMES
// (SPLIT_PLAN S3), the folder is app/ for ui/*, engine/ for translator/*.
function cppSource(rel641) {
  const base = path.basename(rel641);
  const here = path.join(SUV, rel641.startsWith("translator/") ? "engine" : "app", base);
  if (!fs.existsSync(here)) throw new Error("tests/web: " + path.relative(SUV, here) + " is missing (asked for as " + rel641 + ")");
  return { text: fs.readFileSync(here, "utf8").replace(/\r\n/g, "\n"), from: path.relative(SUV, here), fallback: false };
}
const cpp = (rel641) => cppSource(rel641).text;

// The app's C++ as ONE text, for the contract checks ("is this message handled / emitted anywhere in the app?"): every
// .cpp/.h of app/.
function appContractSources() {
  const dir = path.join(SUV, "app");
  const files = fs.readdirSync(dir).filter((f) => /\.(cpp|h)$/i.test(f));
  if (!files.some((f) => /^bridge\.cpp$/i.test(f))) throw new Error("tests/web: app/bridge.cpp is missing");
  return { text: files.map((f) => fs.readFileSync(path.join(dir, f), "utf8")).join("\n").replace(/\r\n/g, "\n"),
           from: "app/{" + files.join(",") + "}", fallback: false };
}

module.exports = { SUV, WEB, PAGE, lf, readPage, cppSource, cpp, appContractSources, fallbacks };

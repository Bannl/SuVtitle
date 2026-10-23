// show_wg.js <run>... -- prints the evals of WG runs (runs\<run>.jsonl), compactly (a debugging aid; the judge is check_wg.js).
// PORTED (T-WG, 2026-10-23) from AudioPB 641f1aa's we_e2e\show_wg.py. WG_RUNS=<folder> reads another folder's runs.
const fs = require("fs");
const path = require("path");

const RUNS = process.env.WG_RUNS ? path.resolve(process.env.WG_RUNS) : path.join(__dirname, "runs");
const KEEP = ["title", "brand", "status", "body", "barRect", "barOpacity", "barIdle", "viewBox", "stageBox", "panelShown", "panelBox", "listBtn",
  "listBtnOn", "edShown", "edWhat", "edState", "edMsg", "edSrc", "edDst", "lines", "rows", "rowTexts", "selRow", "toast", "vt", "vPaused", "vw",
  "more", "stripBox", "statusCut", "w", "h", "overflowX"];
for (const name of process.argv.slice(2)) {
  console.log("===== " + name);
  for (const line of fs.readFileSync(path.join(RUNS, name + ".jsonl"), "utf8").split("\n")) {
    if (!line.trim()) continue;
    const o = JSON.parse(line);
    const h = o.__host;
    if (h === "eval" && typeof o.result === "string" && o.result.indexOf(" {") >= 0) {
      const at = o.result.indexOf(" ");
      const d = JSON.parse(o.result.slice(at + 1));
      const keep = {};
      for (const k of KEEP) if (k in d) keep[k] = d[k];
      console.log(o.result.slice(0, at), JSON.stringify(keep).slice(0, 1800));
    } else if (["exception", "waitfor-timeout", "msg-objects", "focus"].indexOf(h) >= 0) {
      console.log("  host:", JSON.stringify(o).slice(0, 400));
    } else if (h === undefined && ["subEdit", "translatorFileDrop", "playerSetDefault", "subSaveAs"].indexOf(o.type) >= 0) {
      console.log("  page->", JSON.stringify(o).slice(0, 400));
    } else if (h === "console" && (o.level === "error" || o.level === "warning")) {
      console.log("  console:", JSON.stringify(o).slice(0, 300));
    }
  }
}

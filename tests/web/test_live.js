// test_live.js -- the harness of SuVtitle's LIVE window (app/web/live.html), run headless (run_live.js): behaviour checks,
// the page <-> app CONTRACT (docs/LIVE_PLAN.md 3.3: every message's exact shape), the security rule (no markup from data),
// the twins (TrPipe.h's limits, the C++ language doors, index.html's tables and tokens) -- and a MUTATION SENTINEL per
// guard: the page with that guard removed must FAIL its probe (a check that cannot fail is not a check).
// L3, 2026-10-25. PORTED: AudioPB 1.1.0's "Translator window -- the second surface" checks (its harness, 50f79d6) -- the
// dialogue, the live line and its typewriter, the commit-as-you-go trim, the END, the bar, Show, Export, the language
// rows -- against the same page grown its SOURCE plate. NEW: the plate (strips, picker, Listen, meters, INPUT states),
// the state line, the contract.
// The player's Live button + notice (index.html, LIVE_PLAN 3.3's last paragraph) are checked in the last section, when
// index.html carries them.
// Run: node tests/web/test_live.js   (exit 0 = ALL PASSED)
"use strict";
const fs = require("fs");
const path = require("path");
const { runLive, readLivePage } = require("./run_live.js");
const P = require("./paths.js");
const { stripComments } = require("./jslex.js");

let pass = 0, fail = 0;
const ok = (name, cond, extra) => {
  if (cond) { pass++; console.log("  PASS  " + name); }
  else { fail++; console.log("  FAIL  " + name + (extra !== undefined ? "  -> " + JSON.stringify(extra) : "")); }
};
const section = (s) => console.log("\n=== " + s + " ===");

const html = readLivePage();                                    // LF, whatever the checkout (RV4-9)
const script = (html.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
const codeOnly = (src) => stripComments(src);
const MARKUP_SINK = /\binnerHTML\b|\bouterHTML\b|\binsertAdjacentHTML\b|\bdocument\.write(ln)?\b|\beval\s*\(|\bnew\s+Function\s*\(/;
const pageOnly = script.slice(0, script.indexOf("Design-time mock"));   // the page without the mock that plays the app
const readSrc = (rel) => fs.readFileSync(path.join(P.SUV, rel), "utf8").replace(/\r\n/g, "\n");

// ---- fixtures ------------------------------------------------------------------------------------------------------
const UR = "in:{0.0.1.00000000}.{a1b2c3d4-0000-4000-8000-1face0000001}";     // a 6-channel interface
const MIC = "in:{0.0.1.00000000}.{a1b2c3d4-0000-4000-8000-headset00002}";    // a mono headset microphone
const CAB = "in:{0.0.1.00000000}.{a1b2c3d4-0000-4000-8000-vcable000003}";
const ARR = "in:{0.0.1.00000000}.{a1b2c3d4-0000-4000-8000-array0000004}";
const SPK = "out:{0.0.0.00000000}.{a1b2c3d4-0000-4000-8000-speakers0005}";
const ZOOM = "app:Zoom.exe";
const LIST = [
  { kind: "in", key: UR, label: "Line (Studio Interface)", state: "ok", rate: 48000, ch: 6, flags: "default" },
  { kind: "in", key: MIC, label: "Headset Microphone", state: "ok", rate: 16000, ch: 1, flags: "comm" },
  { kind: "in", key: CAB, label: "Virtual Line Output", state: "ok", rate: 48000, ch: 2, flags: "" },
  { kind: "in", key: ARR, label: "Microphone Array", state: "denied", rate: 48000, ch: 2, flags: "" },
  { kind: "out", key: SPK, label: "Speakers", state: "ok", rate: 48000, ch: 2, flags: "default,comm" },
  { kind: "app", key: ZOOM, label: "Meeting app", state: "ok", rate: 48000, ch: 2, flags: "" },
];
const MODELS = { asr: "small\u2192large-v3-turbo", mt: "Qwen3-4B-Instruct-2507-Q5_K_M.gguf" };
const inp = (key, label, chan, gainDb, state, why) =>
  Object.assign({ key, label, chan, gainDb }, state !== undefined ? { state } : {}, why !== undefined ? { why } : {});
const st = (o) => Object.assign({ type: "translatorState", active: true, running: true, status: "listening",
  preset: "balanced", mode: "auto", langA: "en", langB: "zh", gpu: true, models: MODELS,
  spk: { on: true, model: "campplus", speakers: 0 }, sessionLines: 0, live: { on: false, inputs: [] } }, o);
const liveSt = (on, inputs, o) => st(Object.assign({ live: { on, inputs } }, o || {}));
const TWO = [inp(UR, "Me", 1, 0), inp(UR, "Guest", 2, 3)];
// TrPipe.h LANES (R1): a strip's name on the wire -- "<key>|<chan>" for a one-channel strip (written out here, not
// computed: the test holds the rule, the page must meet it). The app's speaker key without a voiceprint: {lane, language}.
const L1 = UR + "|1", L2 = UR + "|2";
const SPK_A = { key: L1 + "|en", slot: L1, label: "Me", color: 0 };
const SPK_B = { key: L2 + "|zh", slot: L2, label: "Guest", color: 1 };
const PAL = ["#c97c64", "#81972b", "#309da9", "#5e5ad3", "#944baf", "#bf367f"];   // 1.1.0's six

// ---- page readers ----------------------------------------------------------------------------------------------------
const q = (root, sel) => root.querySelector(sel);
const stripsOf = (w) => w.ids.strips.children;
const strip = (w, i) => {
  const r = stripsOf(w)[i];
  return r ? { root: r, lamp: q(r, ".strip-lamp"), name: q(r, ".strip-name"), x: q(r, ".strip-x"), kind: q(r, ".strip-kind"),
               src: q(r, ".strip-src-name"), why: q(r, ".strip-why"), ch: q(r, ".strip-ch"), trim: q(r, ".strip-trim"),
               db: q(r, ".strip-db"), meter: q(r, ".meter") } : null;
};
const lvl = (w, i) => { const s = strip(w, i); return s ? s.meter.style["--lvl"] : null; };
const ids = (w) => stripsOf(w).map((r) => r.dataset.key + "#" + r.dataset.chan);
const cfgs = (w) => w.sent("liveConfig");
const lastCfg = (w) => w.last("liveConfig");
const J = (x) => JSON.stringify(x);
const openPick = (w, list, gen) => { w.ids.addSource.click(); if (list) w.send({ type: "liveSources", gen: gen || 1, list }); };
const pickRows = (w) => w.ids.pickerList.querySelectorAll(".pk-row");
const pickRow = (w, key) => pickRows(w).find((r) => r.dataset.key === key) || null;
const pickAdd = (w, key) => { const r = pickRow(w, key); return r ? q(r, ".pk-add") : null; };
const pickChip = (w, key, c) => { const r = pickRow(w, key); return r ? r.querySelectorAll(".pk-ch").find((b) => b.dataset.pick === key + "#" + c) || null : null; };
const stateLine = (w) => w.ids.state.textContent;
const typeName = (w, i, text) => { const s = strip(w, i); s.name.focus(); s.name.value = text; s.name.dispatch("input"); return s; };
const enter = (s) => s.name.dispatch("keydown", { key: "Enter" });
// every string the page puts in front of a person: text, tooltips, accessible names, placeholders
function rendered(w) {
  const out = [];
  const walk = (n) => {
    if (n._text) out.push(n._text);
    if (n.title) out.push(String(n.title));
    ["title", "aria-label", "placeholder"].forEach((k) => { const v = n.attrs && n.attrs[k]; if (v) out.push(String(v)); });
    n.children.forEach(walk);
  };
  walk(w.doc.documentElement);
  return out.join("\n");
}

// =====================================================================================================================
section("L3 -- the port: ONE standalone page, branded SuVtitle Live, 1.1.0's window whole");
{
  ok("its title is \"SuVtitle Live\"", /<title>SuVtitle Live<\/title>/.test(html));
  ok("the lockup: SuVtitle in Bahnschrift 600, its V in heliconia, \"Live\" as its sub-line (index.html's rule, no font file)",
     /<span class="brand-name"><span class="brand-txt">Su<\/span><span class="brand-accent">V<\/span><span class="brand-txt">title<\/span><\/span>\s*<span class="brand-sub">Live<\/span>/.test(html)
     && /\.brand-name \{ font-family: var\(--display\); font-size: 24px; font-weight: 600; font-stretch: normal;/.test(html)
     && /--display: "Bahnschrift", "Segoe UI Variable Display", "Segoe UI", system-ui, sans-serif;/.test(html)
     && !/@font-face\s*\{/.test(html) && !/url\(\s*["']?[^)"']*\.(woff2?|ttf|otf|eot)\b/i.test(html));
  ok("no Overcame anywhere (AudioPB's purchased face stays out of the repository)", !/overcame/i.test(html));
  const visible = (h) => h.replace(/<!--[\s\S]*?-->/g, " ")
    .replace(/<style>([\s\S]*?)<\/style>/g, (m0, css) => "<style>" + css.replace(/\/\*[\s\S]*?\*\//g, " ") + "</style>")
    .replace(/<script>([\s\S]*?)<\/script>/g, (m0, js) => "<script>" + codeOnly(js) + "</script>");
  ok("no \"AudioPB\" a user can see (comments aside: the history stays in them)", !/AudioPB/.test(visible(html)),
     (visible(html).match(/.{0,40}AudioPB.{0,40}/) || [])[0]);
  ok("...and not one word of AudioPB's routing core, comments included (the boundary rule: no \"fabric\")", !/fabric/i.test(html));
  ok("standalone: no external stylesheet, script, font, image, frame, form or import -- one inline <script>, one <style>",
     !/<link\b/i.test(html) && !/<script\b[^>]*\bsrc=/i.test(html) && !/@import/.test(html) && !/<(img|iframe|object|embed|form|audio|video)\b/i.test(html)
     && (html.match(/<script>/g) || []).length === 1 && (html.match(/<style>/g) || []).length === 1);
  ok("...every CSS url() is a data: URL (the policy allows img-src data: only)",
     (html.match(/url\(([^)]*)\)/g) || []).every((u) => /^url\("data:/.test(u)));
  ok("...no style attribute in the markup, no inline event handler, no javascript: URL",
     !/\sstyle="/.test(html.replace(/<script>[\s\S]*?<\/script>/, "")) && !/<[a-z][^>]*\son[a-z]+\s*=/i.test(html.replace(/<script>[\s\S]*?<\/script>/, ""))
     && !/javascript:/i.test(html));
  ok("...and no network at all in the code (fetch, XHR, WebSocket, EventSource, workers, beacons, window.open)",
     !/\bfetch\s*\(|XMLHttpRequest|WebSocket|EventSource|\bWorker\s*\(|SharedWorker|sendBeacon|window\.open\s*\(|\bimport\s*\(/.test(codeOnly(script)));
  const looked = [...script.matchAll(/getElementById\("([^"]+)"\)/g)].map((m) => m[1]);
  ok("every id the script looks up exists in the markup (" + looked.length + ")",
     looked.length >= 30 && looked.every((id) => html.indexOf('id="' + id + '"') >= 0), looked.filter((id) => html.indexOf('id="' + id + '"') < 0));
  const w = runLive();
  ok("on load the page says `ready` -- and nothing else (no list asked, no config, no Listen)",
     J(w.outbox) === J([{ type: "ready" }]), w.outbox);
  ok("1.1.0's window is all there: the log, the live line's classes, Jump to latest, the state line, the bar's controls",
     ["log", "resume", "state", "langA", "dirBtn", "langB", "showBoth", "showA", "showB", "fontDown", "fontUp", "pinToggle", "clearBtn", "exportBtn"]
       .every((id) => !!w.ids[id])
     && ["tr-para", "tr-line", "tr-who", "tr-dash", "tr-who-name", "tr-dst", "tr-src", "tr-lang", "tr-ms", "tr-live", "tr-live-dst",
         "tr-live-src", "tr-live-dots", "tr-live-lang", "tr-live-stable", "tr-live-tentative"].every((c) => html.indexOf(c) >= 0));
  ok("...and the new: the SOURCE plate (strips, Add source, Listen + On air, the picker), the mode, the preset, a theme toggle",
     ["plate", "strips", "addSource", "listenBtn", "onAir", "picker", "pickerList", "modeAuto", "modeA2B", "modeB2A", "presetSel", "themeBtn", "tally"]
       .every((id) => !!w.ids[id]));
  const labels = [...html.matchAll(/<span class="lbl">([^<]+)<\/span>/g)].map((m) => m[1]);
  ok("the controls sit in labelled plates, in the mockup's order: Source, Translate, Show, Text, Window", labels.join("|") === "Source|Translate|Show|Text|Window", labels);
  ok("the deck is three REGIONS laid out by CSS alone (#plate, #trRow, #bar: order + flex-basis), overlaying the log",
     /#plate \{ order: 1; flex: 1 1 100%; \}/.test(html) && /#trRow \{ order: 2;/.test(html) && /#bar   \{ order: 3;/.test(html)
     && /#deck \{\s*position: fixed;/.test(html) && !/style\.(order|gridArea|flex)\b/.test(codeOnly(script)));
  ok("...which FOLD into plain rows on a narrow or short window (1.1.0's rule: labels and plate chrome go, the lockup shrinks; below 560 px it goes)",
     /@media \(max-width: 900px\), \(max-height: 560px\) \{[\s\S]{0,600}#deck \.grp \{ flex-direction: row;[^}]*border: none;[\s\S]{0,80}#deck \.lbl \{ display: none; \}/.test(html)
     && /@media \(max-width: 560px\) \{ \.brand \{ display: none; \} \}/.test(html));
  ok("the controls are not selectable text, the dialogue and the names being typed still are",
     /#deck, #state, #tally \{ user-select: none; \}/.test(html) && /#deck input\[type="text"\] \{ user-select: text; \}/.test(html)
     && !/#log[^{]*\{[^}]*user-select: none/.test(html));
  ok("the window is the app's own (native, movable): no drag region drawn by the page", !/app-region/.test(html));
  ["#resume", ".tr-live-dst", ".tr-live-lang", ".tr-live-src", ".picker", ".pk-note", ".strip-why"].forEach((sel) => {
    ok("live.html: " + sel + " really disappears when hidden ([hidden] guard present)", new RegExp(sel.replace(/[.#]/g, "\\$&") + "\\[hidden\\]").test(html));
  });
}

// =====================================================================================================================
section("L3 -- SuVtitle's design language: tokens, the speaker palette, the theme");
{
  const css = fs.readFileSync(path.join(__dirname, "ref", "tokens_641f1aa.css"), "utf8");
  const tokensOf = (src, selector) => {
    const at = src.indexOf(selector + " {");
    if (at < 0) return null;
    const body = src.slice(src.indexOf("{", at) + 1, src.indexOf("}", at));
    const out = {};
    for (const m of body.matchAll(/(--[\w-]+)\s*:\s*([^;]+);/g)) out[m[1]] = m[2].replace(/\s+/g, " ").trim();
    return out;
  };
  const index = P.readPage();
  [":root", ':root[data-theme="dark"]', ':root[data-theme="light"]'].forEach((b) => {
    const master = tokensOf(css, b), copy = tokensOf(html, b), twin = tokensOf(index, b);
    const names = Object.keys(master || {}).filter((k) => !/^--spk/.test(k));
    const missing = names.filter((k) => !(k in (copy || {})));
    const drift = names.filter((k) => k in (copy || {}) && copy[k] !== master[k]);
    const notTwin = names.filter((k) => !twin || twin[k] !== (copy || {})[k]);
    ok("tokens " + b + ": every one of the reference's (tests/web/ref) is in live.html with the SAME value -- and the same as index.html's",
       !!master && !!copy && names.length >= 9 && missing.length === 0 && drift.length === 0 && notTwin.length === 0, { missing, drift, notTwin });
  });
  const spkVars = [1, 2, 3, 4, 5, 6].map((i) => ((new RegExp("--spk-" + i + ":\\s*(#[0-9a-fA-F]{6})").exec(html) || [])[1] || "").toLowerCase());
  const jsPal = ((/const SPEAKER_COLORS = \[([\s\S]*?)\];/.exec(script) || ["", ""])[1].match(/#[0-9a-fA-F]{6}/g) || []).map((h) => h.toLowerCase());
  ok("the speaker palette: 1.1.0's six, the same in :root (--spk-1..6) and in the script that resolves the app's index",
     spkVars.join() === PAL.join() && jsPal.join() === PAL.join(), { css: spkVars, js: jsPal });
  const wl = runLive({ ls: { "patchbus.theme": "light" } });
  ok("the theme kept in the profile (patchbus.theme = light, the player's key) is the page's from its first paint",
     wl.doc.documentElement.getAttribute("data-theme") === "light" && /const THEME_KEY = "patchbus\.theme";/.test(script));
  wl.win.dispatch("storage", { key: "patchbus.theme", newValue: "dark" });
  ok("...and the player's toggle reaches it LIVE (the storage event)", wl.doc.documentElement.getAttribute("data-theme") === "dark");
  wl.win.dispatch("storage", { key: "patchbus.theme", newValue: "purple" });
  wl.win.dispatch("storage", { key: "audiopb.tr.font", newValue: "light" });
  ok("...while an unknown value, or another key, changes nothing", wl.doc.documentElement.getAttribute("data-theme") === "dark");
  ok("nothing chosen yet: dark (the player's default)", runLive().doc.documentElement.getAttribute("data-theme") === "dark");
  const wt = runLive();
  ok("the window has its own toggle (--live opens it alone): it offers the OTHER theme", wt.ids.themeBtn.title === "Light theme"
     && wt.ids.themeBtn.getAttribute("aria-label") === "Light theme");
  wt.outbox.length = 0;
  wt.ids.themeBtn.click();
  ok("...switches, keeps it in the profile under the shared key (the player follows), and sends nothing to the app",
     wt.doc.documentElement.getAttribute("data-theme") === "light" && wt.lsStore["patchbus.theme"] === "light"
     && wt.ids.themeBtn.title === "Dark theme" && wt.outbox.length === 0);
  wt.ids.themeBtn.click();
  ok("...and back", wt.doc.documentElement.getAttribute("data-theme") === "dark" && wt.lsStore["patchbus.theme"] === "dark");
}

// =====================================================================================================================
// 1.1.0's "Translator window -- the second surface", ported: the page itself, executed.
section("1.1.0 ported -- the dialogue, the live line, the typewriter (the second surface, executed)");
const w = runLive();
const wLive    = () => w.ids.log.querySelector(".tr-live");
const wParas   = () => w.ids.log.querySelectorAll(".tr-para");
const wLastMs  = () => { const c = w.ids.log.querySelectorAll(".tr-ms"); return c[c.length - 1]; };
const wPartial = (segId, lang, src, dst) => w.send({ type: "translatorPartial", segId, t: 8000, lang, src, ...(dst ? { dst } : {}) });
const wTyped   = (segId, lang, src, dst) => { wPartial(segId, lang, src, dst); w.pumpTimers(1000); };
const wFinal   = (segId, t, lang, src, dst, ms, waitMs, spk) => w.send({ type: "translatorLine", t, segId, lang, src, dst, ms, waitMs, spk });
{
  ok("the window announces itself on load, and asks for nothing else", w.outbox.length === 1 && J(w.outbox[0]) === J({ type: "ready" }), w.outbox);
  ok("it consumes every inbound emission, the §7 partial and the W3 end included",
     ["translatorState", "translatorLine", "translatorPartial", "translatorEnd", "translatorExported"].every((t) => script.indexOf('msg.type === "' + t + '"') >= 0));
  ok("it de-duplicates the replay on 1.1.0's identity (t + src)", /const identity = \(l\) => String\(l\.t\) \+ " " \+ String\(l\.src \|\| ""\)/.test(script));
  wPartial("w1", "en", "Right,");
  ok("a partial opens the live line", !!wLive() && !wParas().length);
  ok("...as the last child of the log", w.ids.log.children[w.ids.log.children.length - 1] === wLive());
  ok("...with no draft and no speaker yet", w.ids.log.querySelector(".tr-live-dst").hidden === true && !wLive().querySelector(".tr-who"));
  const wNode = wLive();
  wTyped("w1", "en", "Right, let's set the levels.", "好的，我们来调");
  ok("A SAME-segId PARTIAL UPDATES THE SAME ELEMENT (no flicker, no second line)",
     wLive() === wNode && w.ids.log.querySelectorAll(".tr-live").length === 1
     && w.ids.log.querySelector(".tr-live-src").textContent === "Right, let's set the levels.");
  ok("...and the draft shows once it exists", w.ids.log.querySelector(".tr-live-dst").hidden === false);
  wPartial("w2", "zh", "现在");
  ok("a new segId replaces the content in place", w.ids.log.querySelectorAll(".tr-live").length === 1 && wLive() === wNode && wLive().dataset.seg === "w2");
  wFinal("w2", 8100, "zh", "现在怎么样？", "How is it now?", 340, 900, SPK_B);
  ok("the matching final clears it and renders the paragraph", !wLive() && wParas().length === 1 && wParas()[0].querySelector(".tr-dst").textContent === "How is it now?");
  ok("...tinted with the palette INDEX the app sends, resolved to the same hex", wParas()[0].style["--spk"] === PAL[1], wParas()[0].style["--spk"]);
  ok("...named with the strip's name the app put on the line", wParas()[0].querySelector(".tr-who-name").textContent === "Guest");
  ok("the chip shows waitMs", wLastMs().textContent === "900 ms", wLastMs().textContent);
  ok("...with the inference time in the tooltip", wLastMs().title === "Time from end of speech to this line · ASR+MT 340 ms", wLastMs().title);
  wFinal("w3", 8200, "en", "Recording in five.", "五秒后开始录音。", 700, 1840, SPK_A);
  ok("a final with no partial before it still lands", wParas().length === 2 && !wLive());
  ok("...and a wait over a second reads in seconds", wLastMs().textContent === "1.8 s");
  w.send({ type: "translatorLine", t: 8300, lang: "en", src: "Older record.", dst: "旧版。", ms: 2400, spk: SPK_A });
  ok("a line with no waitMs falls back to `ms`", wLastMs().textContent === "2.4 s");
  wPartial("w4", "zh", "还有");
  wFinal("w4b", 8400, "en", "And one more thing.", "还有一件事。", 500, 1200, SPK_A);
  ok("the live line at the foot does not break the speaker merge", wParas().length === 2 && wParas()[1].querySelectorAll(".tr-line").length === 3, { paras: wParas().length });
  ok("...and it is dropped back at the foot, still in flight", w.ids.log.children[w.ids.log.children.length - 1] === wLive() && wLive().dataset.seg === "w4");
  w.ids.log.scrollHeight = 900; w.ids.log.clientHeight = 300;
  w.ids.log.scrollTop = 120; w.ids.log.dispatch("scroll");
  ok("scrolled up: the pill appears", w.ids.resume.hidden === false);
  wPartial("w5", "en", "Still speaking");
  ok("A PARTIAL IS SILENT FOR THE PILL", w.ids.log.scrollTop === 120 && w.ids.resume.hidden === false, w.ids.log.scrollTop);
  w.ids.resume.click();
  ok("the pill re-pins and jumps to the latest", w.ids.resume.hidden === true && w.ids.log.scrollTop === 900);
  w.ids.clearBtn.click();
  wPartial("orphan", "en", "Actually, hold on");
  w.pumpTimers(19000);
  ok("an abandoned partial survives 19 s of silence", !!wLive());
  w.pumpTimers(1500);
  ok("...then fades at 20 s", !!wLive() && wLive().classList.contains("is-fading"));
  w.pumpTimers(1000);
  ok("...and clears itself", !wLive() && wParas().length === 0);
  wPartial("w6", "en", "Half a sentence");
  w.ids.clearBtn.click();
  ok("Clear takes the sentence in flight with it", !wLive() && wParas().length === 0);
  w.send({ type: "translatorPartial", t: 8888, lang: "en", src: "No identity." });
  ok("a partial with no segId is refused", !wLive() && wParas().length === 0);
  w.send({ type: "translatorPartial", segId: 0, t: 8887, lang: "en", src: "Segment zero." });
  ok("...and so is segId 0, the wire's way of saying 'no segment'", !wLive());
  wPartial("wlive", "en", "Genuinely in flight");
  w.send({ type: "translatorLine", t: 8886, segId: 0, lang: "en", src: "No segment.", dst: "没有段落。", ms: 400, waitMs: 1100, spk: SPK_A });
  ok("...a final with segId 0 cannot claim a live line it never began", !!wLive() && wLive().dataset.seg === "wlive" && wParas().length === 1);
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "w7", t: 8900, lang: "en", src: "In flight." });
  w.send({ type: "translatorLine", t: 8900, segId: "w7", lang: "en", src: "In flight.", dst: "在途中。", ms: 400, waitMs: 1100, spk: SPK_A });
  ok("a partial never enters the replay memory: a final sharing its t + src still lands", wParas().length === 1 && !wLive());

  const wSrcSpans = () => w.ids.log.querySelector(".tr-live-src").children;
  const wDstSpans = () => w.ids.log.querySelector(".tr-live-dst").children;
  const wSrc = () => w.ids.log.querySelector(".tr-live-src").textContent;
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "t1", t: 8910, lang: "en", src: "One two three four five six seven eight" });
  ok("the typewriter: the line starts empty", wSrc() === "", wSrc());
  w.pumpTimers(55);
  ok("...one word at a time", wSrc().trim() === "One", wSrc());
  w.pumpTimers(400);
  ok("...and the tail lands inside the same budget", wSrc() === "One two three four five six seven eight", wSrc());
  ok("...with one timer left on the page: the §7 watchdog, the typewriter having stopped", w.pendingTimers() === 1, w.pendingTimers());
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "t2", t: 8920, lang: "en", src: "The quick brown fox jumps" });
  w.pumpTimers(170);
  const wRt = wSrc();
  ok("mid-typing", wRt === "The quick ", wRt);
  w.send({ type: "translatorPartial", segId: "t2", t: 8921, lang: "en", src: "The quick brown fox jumps over the lazy dog" });
  ok("A NEWER PARTIAL RETARGETS: nothing rewinds", wSrc() === wRt, wSrc());
  w.pumpTimers(1000);
  ok("...and it converges on the newer text", wSrc() === "The quick brown fox jumps over the lazy dog", wSrc());
  w.ids.clearBtn.click();
  wTyped("t3", "en", "Right, let's set the lever");
  w.send({ type: "translatorPartial", segId: "t3", t: 8930, lang: "en", src: "Right, let's set the levels before we start." });
  ok("a revision drops the stale tail in ONE repaint", wSrc() === "Right, let's set the ", wSrc());
  w.pumpTimers(1000);
  ok("...and retypes the corrected one", wSrc() === "Right, let's set the levels before we start.", wSrc());
  w.ids.clearBtn.click();
  wTyped("t4", "en", "Set the levels");
  ok("the live source is two spans: settled + moving", wSrcSpans().length === 2 && wSrcSpans()[0].classList.contains("tr-live-stable") && wSrcSpans()[1].classList.contains("tr-live-tentative"));
  ok("...and a first partial is tentative from end to end", wSrcSpans()[0].textContent === "" && wSrcSpans()[1].textContent === "Set the levels");
  const wHead = wSrcSpans()[0];
  wTyped("t4", "en", "Set the levels before we start", "先调电平");
  ok("what two partials agreed on settles into the stable span, rewritten in place",
     wSrcSpans()[0] === wHead && wSrcSpans()[0].textContent === "Set the levels " && wSrcSpans()[1].textContent === "before we start",
     { stable: wSrcSpans()[0].textContent, tentative: wSrcSpans()[1].textContent });
  ok("...and the draft is tentative until IT has been said twice", wDstSpans()[0].textContent === "" && wDstSpans()[1].textContent === "先调电平");
  wTyped("t4", "en", "Set the levels before we start now", "先调电平再开始");
  ok("...then settles from the left, like the source", wDstSpans()[0].textContent === "先调电平" && wDstSpans()[1].textContent === "再开始");
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "t5", t: 8940, lang: "en", src: "Ten little words are queued up on this line here", dst: "十个字排着队" });
  w.pumpTimers(40);
  ok("ONE ANIMATION TIMER FOR BOTH FIELDS (plus the watchdog)", w.pendingTimers() === 2, w.pendingTimers());
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "t6", t: 8950, lang: "en", src: "A sentence still being typed out" });
  w.pumpTimers(100);
  ok("mid-typing when the final arrives", wSrc() !== "A sentence still being typed out");
  wFinal("t6", 8951, "en", "A sentence still being typed out", "一句还在输入的话", 400, 1200, SPK_A);
  ok("a final cancels the animation and lands whole", !wLive() && wParas().length === 1 && wParas()[0].querySelector(".tr-dst").textContent === "一句还在输入的话");
  ok("...leaving nothing ticking", w.pendingTimers() === 0 && w.pumpTimers(5000) === 0);
  w.ids.clearBtn.click();
  w.setReduceMotion(true);
  w.send({ type: "translatorPartial", segId: "t7", t: 8960, lang: "en", src: "No typewriter for this reader at all" });
  ok("REDUCED MOTION: the text lands whole and instantly", wSrc() === "No typewriter for this reader at all", wSrc());
  ok("...and nothing is left ticking but the watchdog", w.pendingTimers() === 1);
  w.setReduceMotion(false);
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "t8", t: 8970, lang: "zh", src: "现在怎么样？我们再试一次。" });
  w.pumpTimers(45);
  ok("a CJK partial is paced by CHARACTERS", wSrc() === "现", wSrc());
  w.pumpTimers(2000);
  ok("...and lands complete", wSrc() === "现在怎么样？我们再试一次。", wSrc());
  const wGrow = (n) => Array.from({ length: n }, (_, i) => "w" + (i + 1)).join(" ");
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "c1", t: 8980, lang: "en", src: wGrow(6) });
  w.pumpTimers(600);
  w.send({ type: "translatorPartial", segId: "c1", t: 8981, lang: "en", src: wGrow(12) });
  ok("W1: at a 600 ms cadence the previous partial is fully on screen when the next one lands", wSrc().trim() === wGrow(6), wSrc());
  w.pumpTimers(500);
  ok("...and the new tail has caught up within half a second", wSrc() === wGrow(12), wSrc());
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "c2", t: 8990, lang: "en", src: wGrow(5) });
  w.pumpTimers(1500);
  w.send({ type: "translatorPartial", segId: "c2", t: 8991, lang: "en", src: wGrow(10) });
  w.pumpTimers(1500);
  w.send({ type: "translatorPartial", segId: "c2", t: 8992, lang: "en", src: wGrow(15) });
  w.pumpTimers(900);
  ok("W2 AT A SLOW CADENCE: 900 ms in, still typing rather than finished and frozen", wSrc() !== wGrow(15) && wSrc().length > wGrow(10).length, wSrc());
  w.pumpTimers(150);
  ok("...and lands on the full text, before the next partial is due", wSrc() === wGrow(15), wSrc());
  w.pumpTimers(1500);
  w.send({ type: "translatorPartial", segId: "c3", t: 8993, lang: "en", src: wGrow(5) });
  w.pumpTimers(500);
  ok("THE MEASURED CADENCE OUTLIVES THE SENTENCE", wSrc() !== wGrow(5) && wSrc().length > 0, wSrc());
  w.pumpTimers(520);
  ok("...and the new segment lands over that measured window", wSrc() === wGrow(5), wSrc());
  w.ids.clearBtn.click();
  ok("the typewriter's pacing constants are 1.1.0's", [/TW_MIN_MS = 30\b/, /TW_MAX_MS = 250\b/, /TW_SEED_MS = 500\b/, /TW_AHEAD = 0\.8\b/,
     /TW_WIN_MIN = 150\b/, /TW_WIN_MAX = 1200\b/, /TW_EMA_A = 0\.5\b/, /TW_DT_MIN = 50\b/, /TW_DT_MAX = 3000\b/, /LIVE_ORPHAN_MS = 20000\b/].every((re) => re.test(script)));
  const twStopBody = (/function twStop\(\)\s*\{([\s\S]*?)\n\s*\}/.exec(script) || [])[1];
  ok("twStop() does not reset the cadence (it belongs to the ENGINE); twForget() does, for the Clear",
     !!twStopBody && !/TW_SEED_MS/.test(twStopBody) && /function twForget\(\)/.test(script));
  const twBlock = (() => { const a = script.indexOf("TW_MIN_MS"), b = script.search(/function livePaint/); return a > 0 && b > a ? codeOnly(script.slice(a, b)) : ""; })();
  ok("...and the cadence is measured on performance.now, never on Date.now", /performance\.now\(\)/.test(twBlock) && !/Date\./.test(twBlock));
}

section("1.1.0 ported -- the TRANSLATE row: the pair, the direction (the engine's echo wins)");
{
  w.ids.clearBtn.click();
  w.outbox.length = 0;
  const optOf = (sel) => sel.children.map((o) => o.value + "=" + o.textContent).join("|");
  // (U1, the user 02/11: a non-Latin script is followed by its English name -- the six of them; the Latin ones as they were)
  const THIRTEEN = "en=English|zh=简体中文 (Simplified Chinese)|zh-tw=繁體中文 (Traditional Chinese)|fr=Français|de=Deutsch|es=Español|it=Italiano"
    + "|pt=Português|ja=日本語 (Japanese)|ko=한국어 (Korean)|ru=Русский (Russian)|nl=Nederlands|ar=العربية (Arabic)";
  ok("BOTH SELECTS LIST THE SAME THIRTEEN LANGUAGES, in their own names -- + the English one after a non-Latin script (U1) (zh-tw after zh)",
     optOf(w.ids.langA) === THIRTEEN && optOf(w.ids.langB) === THIRTEEN, optOf(w.ids.langA));
  ok("...opening on the pair, with the direction glyph for 'both ways'", w.ids.langA.value === "en" && w.ids.langB.value === "zh" && w.ids.dirBtn.textContent === "⇄");
  ok("...and the Show group labelled from that pair", w.ids.showBoth.textContent === "Both" && w.ids.showA.textContent === "English" && w.ids.showB.textContent === "简体中文" && w.ids.showBoth.classList.contains("is-on"));
  w.ids.langA.value = "fr"; w.ids.langA.dispatch("change");
  ok("CHANGING ONE SELECT SENDS THE WHOLE PAIR (a pair is one fact) -- and nothing else", w.outbox.length === 1 && J(w.outbox[0]) === J({ type: "translatorCtl", langA: "fr", langB: "zh" }), w.outbox);
  ok("...painted at once rather than after a round trip", w.ids.langA.value === "fr" && w.ids.showA.textContent === "Français");
  w.outbox.length = 0;
  w.ids.langB.value = "fr"; w.ids.langB.dispatch("change");
  ok("THE SAME LANGUAGE TWICE IS REFUSED AS A SWAP", w.outbox.length === 1 && J(w.outbox[0]) === J({ type: "translatorCtl", langA: "zh", langB: "fr" }), w.outbox);
  ok("...and the selects show the swap, never fr/fr", w.ids.langA.value === "zh" && w.ids.langB.value === "fr");
  w.outbox.length = 0;
  w.ids.dirBtn.click();
  ok("THE DIRECTION CYCLES ⇄ → → and sends the mode, on its own", w.ids.dirBtn.textContent === "→" && w.outbox.length === 1 && J(w.outbox[0]) === J({ type: "translatorCtl", mode: "a2b" }), [w.ids.dirBtn.textContent, w.outbox]);
  ok("...with a title in the pair's own names", w.ids.dirBtn.title.indexOf("简体中文 (Simplified Chinese) → Français") === 0, w.ids.dirBtn.title);
  ok("...and the mode's three words follow it (A → B pressed, labelled with the codes)", w.ids.modeA2B.classList.contains("is-on") && w.ids.modeA2B.getAttribute("aria-pressed") === "true"
     && w.ids.modeA2B.textContent === "ZH → FR" && w.ids.modeB2A.textContent === "FR → ZH" && w.ids.modeAuto.textContent === "Both ways" && !w.ids.modeAuto.classList.contains("is-on"));
  w.outbox.length = 0;
  w.ids.dirBtn.click();
  ok("...then → ←", w.ids.dirBtn.textContent === "←" && J(w.outbox[0]) === J({ type: "translatorCtl", mode: "b2a" }));
  w.outbox.length = 0;
  w.ids.dirBtn.click();
  ok("...and back to ⇄, the third of three", w.ids.dirBtn.textContent === "⇄" && J(w.outbox[0]) === J({ type: "translatorCtl", mode: "auto" }));
  w.outbox.length = 0;
  w.ids.modeB2A.click();
  ok("the mode's own buttons send the same message (B → A)", J(w.outbox) === J([{ type: "translatorCtl", mode: "b2a" }]) && w.ids.dirBtn.textContent === "←", w.outbox);
  w.outbox.length = 0;
  w.ids.modeB2A.click();
  ok("...and pressing the mode already in force sends NOTHING (only a change travels)", w.outbox.length === 0, w.outbox);
  w.outbox.length = 0;
  w.send(st({ mode: "b2a", langA: "en", langB: "ja" }));
  ok("THE ENGINE'S ECHO WINS over the optimistic copy", w.ids.langA.value === "en" && w.ids.langB.value === "ja" && w.ids.dirBtn.textContent === "←" && w.ids.showB.textContent === "日本語");
  ok("...and receiving a state sends NOTHING back (no echo storm)", w.outbox.length === 0);
  w.send({ type: "translatorState", running: true, status: "listening", preset: "balanced" });
  ok("A STATE WITH NO PAIR leaves the two languages this feature was built for standing", w.ids.langA.value === "en" && w.ids.langB.value === "zh"
     && w.ids.showA.textContent === "English" && w.ids.showB.textContent === "简体中文");
}

section("1.1.0 ported -- the display language (Show both / A / B), on this window");
{
  const wRows = () => w.ids.log.querySelectorAll(".tr-line");
  const wMainOf = (i) => wRows()[i].querySelector(".tr-dst").textContent;
  const wSubOf = (i) => { const s = wRows()[i].querySelector(".tr-src"); return s ? s.textContent : null; };
  const wTitleOf = (i) => wRows()[i].querySelector(".tr-lang").title;
  w.ids.clearBtn.click();
  w.outbox.length = 0;
  w.send({ type: "translatorLine", t: 7100, lang: "en", src: "Let's set the levels.", dst: "我们来调电平。", dstLang: "zh", ms: 500, waitMs: 1200, spk: SPK_A });
  w.send({ type: "translatorLine", t: 7200, lang: "zh", src: "好的，已经在送了。", dst: "Sure, it's sending now.", dstLang: "en", ms: 500, waitMs: 1200, spk: SPK_B });
  w.send({ type: "translatorLine", t: 7300, lang: "fr", src: "Bonjour tout le monde.", dst: "大家好。", dstLang: "zh", ms: 500, waitMs: 1200, spk: SPK_A });
  ok("BOTH renders the translation with the original under it", wMainOf(0) === "我们来调电平。" && wSubOf(0) === "Let's set the levels.");
  w.ids.showA.click();
  ok("A DISPLAY LANGUAGE IS NEVER SENT", w.outbox.length === 0, w.outbox);
  ok("SHOW ENGLISH: source for the English line, translation for the mandarin one, ONE text each",
     wMainOf(0) === "Let's set the levels." && wMainOf(1) === "Sure, it's sending now." && wSubOf(0) === null && wSubOf(1) === null);
  ok("...a third language falls back to its translation", wMainOf(2) === "大家好。");
  ok("...the badge still names what was SPOKEN, its tooltip what is on the row", wRows()[1].querySelector(".tr-lang").textContent === "中" && wTitleOf(1) === "Detected language of what was said · showing English", wTitleOf(1));
  ok("...and it is remembered under the key SHARED with the player (audiopb.tr.show)", w.lsStore["audiopb.tr.show"] === "a", w.lsStore["audiopb.tr.show"]);
  w.ids.showB.click();
  ok("SHOW 简体中文 re-paints the SAME rows the other way round, appending nothing", wMainOf(0) === "我们来调电平。" && wMainOf(1) === "好的，已经在送了。" && wRows().length === 3);
  w.send({ type: "translatorLine", t: 7400, lang: "en", src: "Older record.", dst: "旧版。", ms: 500, waitMs: 1200, spk: SPK_A });
  ok("a line with no dstLang reconstructs its target as the other half of the pair", wMainOf(3) === "旧版。" && wTitleOf(3) === "Detected language of what was said · showing 简体中文 (Simplified Chinese)", wTitleOf(3));
  w.win.dispatch("storage", { key: "audiopb.tr.show", newValue: "a" });
  ok("THE PLAYER'S Show reaches this window LIVE (the shared key, D-L18): the rows re-paint, nothing is written back", wMainOf(0) === "Let's set the levels." && w.ids.showA.classList.contains("is-on")
     && w.lsStore["audiopb.tr.show"] === "b");
  w.win.dispatch("storage", { key: "audiopb.tr.show", newValue: "nonsense" });
  ok("...an unknown value changes nothing", w.ids.showA.classList.contains("is-on"));
  w.ids.clearBtn.click();
  wTyped("w2a", "en", "Speaking in English now", "现在用英文说");
  ok("LIVE, SHOW ENGLISH: the draft's SOURCE is the line", w.ids.log.querySelector(".tr-live-dst").textContent === "Speaking in English now" && w.ids.log.querySelector(".tr-live-src").hidden === true);
  w.ids.showB.click();
  ok("LIVE, SHOW 简体中文: the same draft flips to its translation, in place", w.ids.log.querySelector(".tr-live-dst").textContent === "现在用英文说");
  ok("...and a display switch does not re-type it", w.pendingTimers() === 1, w.pendingTimers());
  w.ids.clearBtn.click();
  w.send({ type: "translatorPartial", segId: "w2b", t: 7500, lang: "en", src: "No draft for this one yet" });
  w.pumpTimers(1000);
  ok("LIVE WITH NO DRAFT YET: the source stands in rather than an empty row", w.ids.log.querySelector(".tr-live-dst").hidden === false && w.ids.log.querySelector(".tr-live-dst").textContent === "No draft for this one yet");
  w.ids.showBoth.click();
  ok("...and BOTH puts it back: draft on top (still empty), original underneath", w.ids.log.querySelector(".tr-live-src").textContent === "No draft for this one yet" && w.ids.log.querySelector(".tr-live-dst").hidden === true);
  w.ids.clearBtn.click();
  // WA: Traditional Chinese
  const WTS = (extra) => w.send(st(extra));
  WTS({ langA: "en", langB: "zh", mode: "auto" });
  w.outbox.length = 0;
  w.ids.langB.value = "zh-tw"; w.ids.langB.dispatch("change");
  ok("WA: choosing 繁體中文 sends the CANONICAL code, the pair whole", w.outbox.length === 1 && J(w.outbox[0]) === J({ type: "translatorCtl", langA: "en", langB: "zh-tw" }), w.outbox);
  WTS({ langA: "en", langB: "zh-TW", mode: "a2b" });
  ok("WA: the ECHO, spelled zh-TW, lands as zh-tw -- select, Show, the direction, the mode's words",
     w.ids.langB.value === "zh-tw" && w.ids.showB.textContent === "繁體中文" && w.ids.dirBtn.title.indexOf("English → 繁體中文 (Traditional Chinese) only") === 0 && w.ids.modeA2B.textContent === "EN → ZH-TW");
  WTS({ langA: "cht", langB: "en", mode: "auto" });
  ok("WA: ...and cht, on the A half", w.ids.langA.value === "zh-tw" && w.ids.showA.textContent === "繁體中文");
  WTS({ langA: "en", langB: "zh-tw", mode: "auto" });
  w.outbox.length = 0;
  w.ids.langA.value = "zh"; w.ids.langA.dispatch("change");
  ok("WA: 简体中文 against 繁體中文 is a pair (a script conversion), never a swap", J(w.outbox) === J([{ type: "translatorCtl", langA: "zh", langB: "zh-tw" }]), w.outbox);
  WTS({ langA: "en", langB: "zh-tw" });
  w.ids.clearBtn.click();
  w.send({ type: "translatorLine", t: 7600, lang: "zh-tw", src: "對白音量剛好。", dst: "The dialogue level is just right.", dstLang: "en", ms: 500, waitMs: 1200, spk: SPK_B });
  w.send({ type: "translatorLine", t: 7700, lang: "en", src: "Mute track four.", dst: "把第四軌靜音。", dstLang: "zh-tw", ms: 500, waitMs: 1200, spk: SPK_A });
  ok("WA: a zh-tw line is badged 繁, an English one EN", wRows()[0].querySelector(".tr-lang").textContent === "繁" && wRows()[1].querySelector(".tr-lang").textContent === "EN");
  ok("WA: a Traditional translation is tagged zh-TW, its English original en (the right CJK face)",
     wRows()[1].querySelector(".tr-dst").getAttribute("lang") === "zh-TW" && wRows()[1].querySelector(".tr-src").getAttribute("lang") === "en");
  w.ids.clearBtn.click();
  wTyped("w-wa", "zh-tw", "我們再聽", "Let's listen");
  ok("WA: the live line of a zh-tw draft is badged 繁, its texts tagged (zh-TW source, en draft)",
     w.ids.log.querySelector(".tr-live-lang").textContent === "繁" && w.ids.log.querySelector(".tr-live-src").getAttribute("lang") === "zh-TW");
  w.ids.clearBtn.click();
  WTS({ langA: "en", langB: "zh", mode: "b2a" });
  w.outbox.length = 0;
}

section("1.1.0 ported -- W3, commit as you go: closes:false keeps the live line, trimmed; END");
{
  const wSrcSpans = () => w.ids.log.querySelector(".tr-live-src").children;
  const wCommit = (segId, t, lang, src, dst, spk, extra) => w.send({ type: "translatorLine", t, segId, lang, src, dst, ms: 500, waitMs: 1100, spk, ...(extra || {}) });
  const wSrcOr = () => (wLive() ? w.ids.log.querySelector(".tr-live-src").textContent : "<no live line>");
  const wHeadOr = () => (wLive() ? wSrcSpans()[0].textContent : "<no live line>");
  const wTailOr = () => (wLive() ? wSrcSpans()[1].textContent : "<no live line>");
  const wDstTxt = () => (wLive() ? w.ids.log.querySelector(".tr-live-dst").textContent : "<no live line>");
  const wDstHid = () => (wLive() ? w.ids.log.querySelector(".tr-live-dst").hidden : null);
  w.ids.clearBtn.click();
  wTyped("c1", "en", "Right, the room is ready. Let's start", "好的，房间准备好了");
  const wTwBefore = w.pendingTimers();
  wCommit("c1", 9800, "en", "Right, the room is ready.", "好的，房间准备好了。", SPK_A, { seq: 1, closes: false });
  ok("a closes:false LINE appends, and LEAVES THE LIVE LINE STANDING", wParas().length === 1 && wParas()[0].querySelectorAll(".tr-line").length === 1 && !!wLive() && wLive().dataset.seg === "c1");
  ok("...with the committed sentence cut off its front, WHOLE and before a single tick", wSrcOr() === "Let's start" && w.pendingTimers() === wTwBefore, { shown: wSrcOr(), timers: w.pendingTimers() });
  ok("...and the draft translation gone with it", wDstHid() === true);
  w.send({ type: "translatorPartial", segId: "c1", t: 9801, lang: "en", src: "Let's start" });
  ok("...so the engine's tail PART re-types nothing, and settles what it agrees with", wSrcOr() === "Let's start" && wHeadOr() === "Let's start" && wTailOr() === "");
  wTyped("c1", "en", "Let's start with the drum stems. Then we", "我们先从鼓");
  wCommit("c1", 9802, "en", "Let's start with the drum stems.", "我们先从鼓的分轨开始。", SPK_A, { seq: 2, closes: false });
  ok("the second sentence joins the SAME paragraph, em-dash on the first row only", wParas().length === 1 && wParas()[0].querySelectorAll(".tr-line").length === 2
     && !!wParas()[0].querySelectorAll(".tr-line")[0].querySelector(".tr-who") && !wParas()[0].querySelectorAll(".tr-line")[1].querySelector(".tr-who"));
  ok("...and the live line is trimmed again", wSrcOr() === "Then we", wSrcOr());
  wTyped("c1", "en", "Then we print the mix.", "然后我们导出混音");
  wCommit("c1", 9803, "en", "Then we print the mix.", "然后我们导出混音。", SPK_A, { seq: 3, closes: true });
  ok("...and closes:true ends the utterance: three sentences, one paragraph, no live line", !wLive() && wParas().length === 1 && wParas()[0].querySelectorAll(".tr-line").length === 3);
  w.ids.clearBtn.click();
  wTyped("c2", "en", "right,  let's set the LEVELS, and then");
  wCommit("c2", 9810, "en", "Right, let's set the levels.", "我们来调电平。", SPK_A, { seq: 1, closes: false });
  ok("case, spacing and the sentence's own punctuation may differ", !!wLive() && wSrcOr() === "and then", wSrcOr());
  w.ids.clearBtn.click();
  wTyped("c3", "en", "Something else entirely");
  wCommit("c3", 9820, "en", "A sentence that was never on that line.", "另一句。", SPK_A, { seq: 1, closes: false });
  ok("...and a head that is NOT the head is refused, leaving the live line untouched", !!wLive() && wSrcOr() === "Something else entirely", wSrcOr());
  w.ids.clearBtn.click();
  wTyped("c4", "en", "The level is set now. Mm");
  wCommit("c4", 9830, "en", "The level is set now.", "音量已经调好了。", SPK_A, { seq: 1, closes: false });
  w.send({ type: "translatorEnd", segId: "c-other" });
  ok("an END for another segment is ignored", !!wLive() && wLive().dataset.seg === "c4");
  w.send({ type: "translatorEnd", segId: "c4" });
  ok("...and the matching one fells the live line, appending nothing", !wLive() && wParas().length === 1 && wParas()[0].querySelectorAll(".tr-line").length === 1);
  ok("...taking the orphan watchdog with it", w.pendingTimers() === 0, w.pendingTimers());
  w.ids.clearBtn.click();
  wTyped("c5", "en", "An older record's line.");
  w.send({ type: "translatorLine", t: 9850, segId: "c5", lang: "en", src: "An older record's line.", dst: "旧的一行。", ms: 400, waitMs: 1100, spk: SPK_A });
  ok("a line with no `closes` at all still closes: missing means TRUE", !wLive() && wParas().length === 1);
  w.ids.showB.click();
  w.ids.clearBtn.click();
  wTyped("c6", "en", "Set the levels. Then we", "先调电平");
  wCommit("c6", 9860, "en", "Set the levels.", "先调电平。", SPK_A, { seq: 1, closes: false, dstLang: "zh" });
  ok("SHOW 简体中文 + closes:false: the committed row is the translation, like any line", w.ids.log.querySelectorAll(".tr-dst")[0].textContent === "先调电平。");
  ok("...and the trimmed live line stands in with its source, its draft having gone", wDstTxt() === "Then we" && wDstHid() === false);
  w.ids.showBoth.click();
  w.ids.clearBtn.click();
  w.setReduceMotion(true);
  w.send({ type: "translatorPartial", segId: "c7", t: 9870, lang: "en", src: "All at once here. And the rest" });
  wCommit("c7", 9871, "en", "All at once here.", "一次全部。", SPK_A, { seq: 1, closes: false });
  ok("reduced motion is unaffected", !!wLive() && wSrcOr() === "And the rest", wSrcOr());
  w.setReduceMotion(false);
  w.ids.clearBtn.click();
  w.outbox.length = 0;
  const punct = (/TRIM_PUNCT = \/\[([\s\S]*?)\]\//.exec(script) || [])[1];
  ok("the head trim's punctuation class is 1.1.0's, character for character", punct === ".,;:!?…。、，；：！？·\"'“”‘’()（）\\[\\]{}「」『』—–", punct);
}

section("1.1.0 ported -- W5, Export: the button, its tooltip, the acknowledgement");
{
  w.send(st({ sessionLines: 42 }));
  ok("the bar carries Export, in the Window plate after Clear", !!w.ids.exportBtn && html.indexOf('id="clearBtn"') < html.indexOf('id="exportBtn"'));
  ok("...INSIDE the deck, so the idle fade takes it with the rest", (() => { let n = w.ids.exportBtn; while (n && n !== w.ids.deck) n = n.parentNode; return n === w.ids.deck; })());
  ok("...its tooltip promising what the SESSION FILE holds (42), not what is on screen", w.ids.exportBtn.title.indexOf("42 lines") > 0 && w.ids.exportBtn.title.indexOf(".docx") > 0, w.ids.exportBtn.title);
  w.send(st({ sessionLines: 1 }));
  ok("...counting in English (1 line, not 1 lines)", w.ids.exportBtn.title.indexOf("1 line") > 0 && w.ids.exportBtn.title.indexOf("1 lines") < 0, w.ids.exportBtn.title);
  w.send({ type: "translatorState", running: true, status: "listening", preset: "balanced" });
  ok("A STATE WITH NO sessionLines reads as 'nothing recorded yet'", w.ids.exportBtn.title.indexOf("nothing recorded yet") > 0, w.ids.exportBtn.title);
  w.outbox.length = 0;
  w.ids.exportBtn.click();
  ok("clicking it sends translatorExport{format:\"docx\"} AND NOTHING ELSE", J(w.outbox) === J([{ type: "translatorExport", format: "docx" }]), w.outbox);
  const wStatus = () => w.ids.state.textContent;
  const wIdle = wStatus();
  w.send({ type: "translatorExported", ok: true, lines: 42, path: "C:\\Users\\me\\Documents\\SuVtitle Live 2026-10-25 15-40.docx" });
  ok("AN OK ACK BORROWS THE STATE LINE and names the FILE, not the path just typed", wStatus() === "Saved · SuVtitle Live 2026-10-25 15-40.docx", wStatus());
  ok("...in the ordinary style: a save that worked is not an error", !w.ids.state.classList.contains("is-error"));
  w.pumpTimers(3000);
  ok("...and it stays long enough to be read", wStatus().indexOf("Saved") === 0, wStatus());
  w.pumpTimers(1500);
  ok("...then hands the line back", wStatus() === wIdle, wStatus());
  w.send({ type: "translatorExported", ok: false, err: "nothing to export yet" });
  ok("A FAILURE says what went wrong, in the app's own sentence", wStatus() === "Export failed — nothing to export yet", wStatus());
  ok("...and wears the error style (it pins itself through the idle fade)", w.ids.state.classList.contains("is-error"));
  w.pumpTimers(5000);
  ok("...STILL there where a 4 s success would be gone", wStatus().indexOf("Export failed") === 0, wStatus());
  w.pumpTimers(1500);
  ok("...then gives the line back", wStatus() === wIdle && !w.ids.state.classList.contains("is-error"), wStatus());
  w.send({ type: "translatorExported", ok: false, err: "cancelled" });
  ok("A CANCEL SAYS NOTHING AT ALL", wStatus() === wIdle && !w.ids.state.classList.contains("is-error"), wStatus());
  // the idle fade, as this window has it: only while listening
  w.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  w.pumpTimers(9000);
  ok("listening, the deck has faded once the reader has been still", w.ids.deck.classList.contains("is-idle") && w.ids.state.classList.contains("is-idle"));
  w.send({ type: "translatorExported", ok: true, lines: 3, path: "D:\\x\\Meeting.docx" });
  ok("...and an ack WAKES it rather than being written under the fade", !w.ids.deck.classList.contains("is-idle") && wStatus() === "Saved · Meeting.docx", wStatus());
  w.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  w.pumpTimers(9000);
  w.ids.clearBtn.click();
  w.outbox.length = 0;
}

section("1.1.0 ported -- the language normaliser mirrors the C++ doors (app + engine)");
// The normaliser PROBES (each is a check here AND the condition its mutation sentinel must see FAIL).
const winYue = (x) => {
  x.send(st({ langA: "yue", langB: "fr" }));
  x.outbox.length = 0;
  x.ids.langB.value = "de";
  x.ids.langB.dispatch("change");
  const c = x.last("translatorCtl");
  return !!c && c.langA === "yue" && c.langB === "de";
};
const winNonCode = (x) => {
  x.send(st({ langA: "en", langB: "zh" }));
  x.send({ type: "translatorLine", t: 77, lang: "spa", src: "hola", dst: "hello", dstLang: "en", spk: { key: "nc", label: "NC", color: 0 } });
  const b = x.ids.log.querySelectorAll(".tr-lang"), s = x.ids.log.querySelectorAll(".tr-src");
  return b.length > 0 && b[b.length - 1].textContent === "SPA" && s.length > 0 && !s[s.length - 1].getAttribute("lang");
};
{
  const contract = P.appContractSources();
  const cppRows = (src, fn) => {
    const blk = new RegExp(fn + "[\\s\\S]*?kAlias\\[\\] = \\{([\\s\\S]*?)\\};").exec(src);
    return blk ? [...blk[1].matchAll(/\{\s*L?"([^"]+)",\s*L?"([^"]+)"\s*\}/g)].map((m) => m[1] + ">" + m[2]).sort().join(",") : null;
  };
  const jsRows = (src, name) => {
    const blk = new RegExp("const " + name + " = \\{([\\s\\S]*?)\\};").exec(src);
    return blk ? [...blk[1].matchAll(/"([^"]+)":\s*"([^"]+)"/g)].map((m) => m[1] + ">" + m[2]).sort().join(",") : null;
  };
  const b = cppRows(contract.text, "trLangCode"), e = cppRows(P.cpp("translator/TrEngine.cpp"), "normalizeLangCode");
  const t = jsRows(html, "LANG_ALIAS"), ix = jsRows(P.readPage(), "LANG_ALIAS");
  ok("the alias tables (the app's door, the engine's, the player's, this page's) hold the SAME rows", !!b && b === e && b === t && t === ix, { app: b, engine: e, live: t, player: ix });
  ok("...and the page keeps no two-letter prefix fallback", !/return s\.slice\(0, 2\)/.test(html));
  const tbl = (s) => (/const TR_LANGS = \[([\s\S]*?)\];/.exec(s) || [])[1];
  ok("the thirteen languages are index.html's, row for row (a fourteenth goes in both pages or in neither)", !!tbl(html) && tbl(html) === tbl(P.readPage()));
  const x = runLive();
  ok("a running yue pair: picking the OTHER half sends yue back -- never \"yu\"", winYue(x));
  ok("...and the window NAMES it by its code (no table row for yue)", x.ids.showA.textContent === "YUE", x.ids.showA.textContent);
  x.outbox.length = 0;
  x.ids.langA.value = "";
  x.ids.langA.dispatch("change");
  ok("a select on NO code sends nothing at all", x.count("translatorCtl") === 0, x.outbox);
  const y = runLive();
  y.send(st({ langA: "en", langB: "zh" }));
  y.send({ type: "translatorLine", t: 1, lang: "cmn", src: "x", dst: "y", dstLang: "en", spk: { key: "k", label: "L", color: 0 } });
  y.send({ type: "translatorLine", t: 2, lang: "eng", src: "x2", dst: "y2", dstLang: "zh", spk: { key: "k", label: "L", color: 0 } });
  y.send({ type: "translatorLine", t: 3, lang: "sv", src: "x3", dst: "y3", dstLang: "en", spk: { key: "k", label: "L", color: 0 } });
  const badges = y.ids.log.querySelectorAll(".tr-lang").map((b2) => b2.textContent);
  ok("badges read through the same rows: cmn -> 中, eng -> EN, a plain 2-letter code (sv) -> SV", badges.join(",") === "中,EN,SV", badges);
  ok("a NON-code (spa): shown as it came (SPA), no lang attribute, never \"sp\"", winNonCode(runLive()));
  ok("the CSS and comments name today's labels (no bare \"中文\" outside the why-not note)",
     (html.match(/(?<![体體])中文/g) || []).length === 1 && /no longer labelled "中文"/.test(html), html.match(/.{0,30}(?<![体體])中文.{0,10}/g));
}

// =====================================================================================================================
// THE CONTRACT (docs/LIVE_PLAN.md 3.3) and THE PLATE
// =====================================================================================================================
// ---- the probes: each is a check below AND the condition its mutation sentinel must see FAIL ---------------------------
const echoWins = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0), inp(MIC, "Host", 0, -3)]));
  const a = ids(x).join() === [UR + "#1", MIC + "#0"].join() && strip(x, 1).name.value === "Host" && strip(x, 1).db.textContent === "−3.0 dB";
  x.send(liveSt(false, [inp(MIC, "Host", 0, -3)]));
  return a && ids(x).join() === MIC + "#0";
};
const inputsAbsentKeeps = (x) => {
  x.send(liveSt(false, TWO));
  x.send(st({ live: { on: true } }));                                  // on, and no `inputs` reported at all
  const kept = stripsOf(x).length === 2 && x.ids.listenBtn.getAttribute("aria-pressed") === "true";
  x.send(liveSt(true, []));                                            // an EMPTY list: no strip
  return kept && stripsOf(x).length === 0;
};
const addOne = (x) => {          // strips [Me on UR ch1]; the picker; UR's channel 2 added -> 2 strips, pending
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  openPick(x, LIST);
  x.outbox.length = 0;
  pickAdd(x, UR).click();
};
const noFlicker = (x) => {
  addOne(x);
  const cfg = lastCfg(x), el2 = stripsOf(x)[1];
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));                       // an echo the app posted BEFORE it read ours
  const held = stripsOf(x).length === 2 && stripsOf(x)[1] === el2;
  x.send(liveSt(false, cfg.inputs));                                   // ours
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));                       // a LATER change by the app: it wins at once
  return !!cfg && held && stripsOf(x).length === 1;
};
const refusalShows = (x) => {
  addOne(x);
  // R4: the app's refusal is a warn + the previous strips, sent ONCE -- nothing else follows it
  x.send({ type: "warn", text: "That change cannot be applied." });
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const held = stripsOf(x).length === 2;
  x.pumpTimers(1600);                                                    // the pending window ends on its own...
  return held && stripsOf(x).length === 1 && strip(x, 0).name.value === "Me";   // ...and the app's word stands
};
// R4: live.wanted -- the app WANTS to listen, the engine has not confirmed: the dot blinks, the line says so, and a
// second press asks to STOP (a start in progress can be cancelled); absent, `wanted` reads as `on` (an older sender)
const wantedStarting = (x) => {
  x.send(liveSt(false, TWO));
  x.send(st({ live: { on: false, wanted: true, inputs: TWO } }));
  const starting = x.ids.listenBtn.classList.contains("is-pending") && stateLine(x) === "starting to listen…"
    && x.ids.listenBtn.getAttribute("aria-pressed") === "false" && !x.ids.onAir.classList.contains("is-lit");
  x.outbox.length = 0;
  x.ids.listenBtn.click();
  const cancels = J(x.outbox) === J([{ type: "liveListen", on: false }]);
  x.send(st({ live: { on: false, wanted: false, inputs: TWO } }));
  return starting && cancels && !x.ids.listenBtn.classList.contains("is-pending") && /^not listening/.test(stateLine(x));
};
const lampOnEchoOnly = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  x.outbox.length = 0;
  x.ids.listenBtn.click();
  const asked = J(x.outbox) === J([{ type: "liveListen", on: true }]);
  const dark = x.ids.listenBtn.getAttribute("aria-pressed") === "false" && !x.ids.onAir.classList.contains("is-lit")
    && !x.doc.body.classList.contains("is-on-air") && x.ids.listenBtn.classList.contains("is-pending");
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  return asked && dark && x.ids.listenBtn.getAttribute("aria-pressed") === "true" && x.ids.onAir.classList.contains("is-lit")
    && x.doc.body.classList.contains("is-on-air") && !x.ids.listenBtn.classList.contains("is-pending");
};
const levelsOnlyOnAir = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  x.send({ type: "liveLevels", levels: { [L1]: -12 } });
  const quiet = lvl(x, 0) === "0.000" && !strip(x, 0).meter.classList.contains("is-lit");
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  x.send({ type: "liveLevels", levels: { [L1]: -12 } });
  return quiet && lvl(x, 0) === "0.800";
};
const metersGoStale = (x) => {
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  x.send({ type: "liveLevels", levels: { [L1]: -30 } });
  const lit = lvl(x, 0) === "0.500";
  x.pumpTimers(1100);
  const still = lvl(x, 0) === "0.500";
  x.pumpTimers(200);
  return lit && still && lvl(x, 0) === "0.000";
};
const metersOffOnStop = (x) => {
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  x.send({ type: "liveLevels", levels: { [L1]: -6 } });
  const lit = lvl(x, 0) === "0.900";
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  return lit && lvl(x, 0) === "0.000" && strip(x, 0).meter.title === "";
};
const sourcesOnlyOnOpen = (x) => {
  x.send(liveSt(true, TWO));
  x.send({ type: "liveLevels", levels: { [L1]: -20 } });
  x.send({ type: "liveSources", gen: 3, list: LIST });                 // a list pushed on its own: no request back
  x.send({ type: "translatorLine", t: 5, lang: "en", src: "a", dst: "b", spk: SPK_A });
  const asks = () => x.sent("liveSources").filter((m) => m.open !== false).length;
  const none = x.count("liveSources") === 0;
  x.ids.addSource.click();
  const one = J(x.sent("liveSources")) === J([{ type: "liveSources" }]);
  x.ids.addSource.click();                                              // closes: SAYS so (fix round, RVB-7), asks nothing
  const closed = J(x.sent("liveSources")) === J([{ type: "liveSources" }, { type: "liveSources", open: false }]);
  x.ids.addSource.click();
  return none && one && closed && asks() === 2;
};
const clashRule = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  openPick(x, LIST);
  const row = pickAdd(x, UR);
  const a = !!row && !row.disabled && row.dataset.pick === UR + "#2"
    && pickChip(x, UR, 0).disabled && pickChip(x, UR, 1).disabled && !pickChip(x, UR, 2).disabled && !pickChip(x, UR, 6).disabled;
  x.send(liveSt(false, [inp(UR, "Line", 0, 0)]));
  const r2 = pickAdd(x, UR);
  return a && r2.disabled && [0, 1, 2, 3, 4, 5, 6].every((c) => pickChip(x, UR, c).disabled)
    && /set that strip to one channel/.test(q(pickRow(x, UR), ".pk-why").textContent);
};
const FOUR = [inp(UR, "A", 1, 0), inp(UR, "B", 2, 0), inp(MIC, "C", 0, 0), inp(SPK, "D", 0, 0)];
const fourMax = (x) => {
  x.send(liveSt(false, FOUR));
  openPick(x, LIST);
  x.outbox.length = 0;
  pickAdd(x, CAB).click();
  return x.ids.addSource.disabled === true && /Up to 4 sources/.test(x.ids.addSource.title) && x.count("liveConfig") === 0
    && x.ids.pickerNote.hidden === false && /Up to 4 sources at once/.test(x.ids.pickerNote.textContent)
    && pickRows(x).every((r) => q(r, ".pk-add").disabled);
};
const labelBytes = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const s = typeName(x, 0, "é".repeat(40));
  const cut = s.name.value === "é".repeat(32);
  s.name.value = "\u{1F600}".repeat(20); s.name.dispatch("input");
  const emoji = s.name.value === "\u{1F600}".repeat(16);
  x.outbox.length = 0;
  enter(s);
  const c = lastCfg(x);
  return cut && emoji && !!c && c.inputs[0].label === "\u{1F600}".repeat(16);
};
const labelClean = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const s = typeName(x, 0, "Pi\u202Eerre\u200F\u0007");
  x.outbox.length = 0;
  enter(s);
  const c = lastCfg(x);
  return !!c && !/[\u202E\u200F\u0007]/.test(c.inputs[0].label) && c.inputs[0].label === "Pi erre";
};
const deviceNameClean = (x) => {
  const evil = "Mic\u202E (ahem)\u2066";
  x.send(liveSt(false, []));
  openPick(x, [{ kind: "in", key: MIC, label: evil, state: "ok", ch: 1, flags: "" }]);
  const nm = q(pickRow(x, MIC), ".pk-name").textContent;
  x.outbox.length = 0;
  pickAdd(x, MIC).click();
  const c = lastCfg(x);
  return !/[\u202E\u2066]/.test(nm) && !!c && !/[\u202E\u2066]/.test(c.inputs[0].label) && !/[\u202E\u2066]/.test(strip(x, 0).src.textContent);
};
const onlyChanged = (x) => {
  x.send(st({ preset: "balanced", presetReq: "balanced" }));
  x.outbox.length = 0;
  x.ids.dirBtn.click();
  x.ids.presetSel.value = "quality"; x.ids.presetSel.dispatch("change");
  x.ids.langB.value = "ja"; x.ids.langB.dispatch("change");
  x.ids.pinToggle.click();
  x.ids.clearBtn.click();
  return J(x.outbox) === J([{ type: "translatorCtl", mode: "a2b" }, { type: "translatorCtl", preset: "quality" },
    { type: "translatorCtl", langA: "en", langB: "ja" }, { type: "translatorCtl", alwaysOnTop: true }, { type: "translatorCtl", clear: true }]);
};
const presetUnchangedSilent = (x) => {
  x.send(st({ preset: "balanced", presetReq: "balanced" }));
  x.outbox.length = 0;
  x.ids.presetSel.value = "balanced"; x.ids.presetSel.dispatch("change");
  x.send(st({ preset: "balanced" }));            // never chosen: what runs is what is shown
  x.ids.presetSel.value = "balanced"; x.ids.presetSel.dispatch("change");
  return x.outbox.length === 0 && x.ids.presetSel.value === "balanced";
};
const focusedNameKept = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const s = typeName(x, 0, "Pie");
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  const kept = s.name.value === "Pie";
  x.outbox.length = 0;
  s.name.blur();
  const c = lastCfg(x);
  return kept && !!c && c.inputs[0].label === "Pie";
};
const trimThrottled = (x) => {
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  const s = strip(x, 0);
  x.outbox.length = 0;
  s.trim.dispatch("pointerdown");
  for (let k = 1; k <= 8; k++) { s.trim.value = String(k * 0.5); s.trim.dispatch("input"); x.pumpTimers(20); }
  const before = x.count("liveConfig"), readout = s.db.textContent;
  x.pumpTimers(60);
  const mid = cfgs(x);
  for (let k = 9; k <= 12; k++) { s.trim.value = String(k * 0.5); s.trim.dispatch("input"); x.pumpTimers(20); }
  s.trim.value = "6.5"; s.trim.dispatch("change");
  const all = cfgs(x);
  return before === 0 && readout === "+4.0 dB" && mid.length === 1 && mid[0].inputs[0].gainDb === 4
    && all.length === 2 && all[1].inputs[0].gainDb === 6.5 && x.pendingTimers() >= 0;
};
const listenNoStripHint = (x) => {
  x.send(liveSt(false, []));
  x.outbox.length = 0;
  x.ids.listenBtn.click();
  return J(x.outbox) === J([{ type: "liveListen", on: true }]) && stateLine(x) === "Add a source first."
    && x.ids.addSource.classList.contains("is-beckon") && !x.ids.listenBtn.classList.contains("is-pending")
    && x.ids.listenBtn.getAttribute("aria-pressed") === "false";
};
const neverSaysListening = (x) => {
  x.send(liveSt(false, TWO, { status: "listening" }));
  return stateLine(x).indexOf("listening") !== 0 && /^not listening/.test(stateLine(x)) && !x.ids.state.classList.contains("is-listening");
};
const noFadeOffAir = (x) => {
  x.send(liveSt(false, TWO));
  x.win.dispatch("pointermove");
  x.pumpTimers(10000);
  const stays = !x.ids.deck.classList.contains("is-idle");
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  x.pumpTimers(3100);
  const fades = x.ids.deck.classList.contains("is-idle");
  x.win.dispatch("pointermove");
  x.ids.addSource.click();
  x.pumpTimers(5000);
  return stays && fades && !x.ids.deck.classList.contains("is-idle");
};
const replayOnce = (x) => {
  const l = { type: "translatorLine", t: 1700000000123, lang: "en", src: "Once.", dst: "一次。", ms: 300, waitMs: 900, spk: SPK_A };
  x.send(l); x.send(l);
  x.send(Object.assign({}, l, { t: 1700000000124 }));
  return x.ids.log.querySelectorAll(".tr-line").length === 2;
};
const clearedKeepsState = (x) => {
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  x.send({ type: "translatorLine", t: 9, lang: "en", src: "Hello.", dst: "你好。", spk: SPK_A });
  x.send({ type: "translatorState", cleared: true });
  return x.ids.log.children.length === 0 && /^listening/.test(stateLine(x)) && stripsOf(x).length === 2
    && x.ids.listenBtn.getAttribute("aria-pressed") === "true";
};
const warnShown = (x) => {
  x.send(st());
  x.send({ type: "warn", text: "The engine stopped responding; restarting it." });
  const shown = stateLine(x) === "The engine stopped responding; restarting it." && x.ids.state.classList.contains("is-error");
  x.pumpTimers(6100);
  return shown && /^not listening/.test(stateLine(x));
};
const topmostFollows = (x) => {
  x.send({ type: "translatorWindow", topmost: true });
  const on = x.ids.pinToggle.classList.contains("is-on") && x.ids.pinToggle.getAttribute("aria-pressed") === "true";
  x.send({ type: "translatorWindow" });
  const kept = x.ids.pinToggle.classList.contains("is-on");
  x.outbox.length = 0;
  x.ids.pinToggle.click();
  return on && kept && J(x.outbox) === J([{ type: "translatorCtl", alwaysOnTop: false }]);
};
const channelLabelFollows = (x) => {
  x.send(liveSt(false, [inp(UR, "Line (Studio Interface)", 0, 0), inp(MIC, "Pierre", 0, 0)]));
  openPick(x, LIST);
  x.ids.pickerClose.click();
  x.outbox.length = 0;
  const s = strip(x, 0);
  s.ch.value = "2"; s.ch.dispatch("change");
  const a = lastCfg(x);
  return !!a && a.inputs[0].label === "Line (Studio Interface) · ch 2" && a.inputs[0].chan === 2 && a.inputs[1].label === "Pierre";
};
const CACHE = { "suvtitle.live.sources": JSON.stringify({ [UR]: ["Line (Studio Interface)", 6] }) };
const cacheAfterReload = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const s = strip(x, 0);
  return s.src.textContent === "Line (Studio Interface)" && s.ch.children.length === 7 && s.ch.children[6].textContent === "Ch 6";
};
// R1 -- TWO STRIPS ON CHANNELS 1 AND 2 OF ONE SOURCE (the user's "canal 1 = moi, canal 2 = l'invité"): each its own lane.
const levelsByLane = (x) => {          // the echo names no lane (a strip not named yet): the page builds the SAME rule
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  x.send({ type: "liveLevels", levels: { [L1]: -6, [L2]: -36, [UR]: -60 } });
  return lvl(x, 0) === "0.900" && lvl(x, 1) === "0.400";
};
const laneFromEcho = (x) => {          // the APP names the lanes (live.inputs[].lane): the page goes by the echo's
  x.send(liveSt(true, [Object.assign(inp(UR, "Me", 1, 0, "open"), { lane: "lane-A" }),
                       Object.assign(inp(UR, "Guest", 2, 3, "open"), { lane: "lane-B" })]));
  x.send({ type: "liveLevels", levels: { "lane-A": -36, "lane-B": -6, [L1]: -60, [L2]: -60 } });
  return lvl(x, 0) === "0.400" && lvl(x, 1) === "0.900";
};
const deviceKeyLightsNoChannel = (x) => {   // the DEVICE's key is ITS MIX's lane: a channel strip never reads it
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  x.send({ type: "liveLevels", levels: { [UR]: -18 } });
  return lvl(x, 0) === "0.000" && lvl(x, 1) === "0.000" && !strip(x, 0).meter.classList.contains("is-lit")
    && !strip(x, 1).meter.classList.contains("is-lit");
};
const laneAfterChannelMove = (x) => {  // its channel moved: the echo's name was the OLD channel's -- the new one is read
  x.send(liveSt(true, [Object.assign(inp(UR, "Guest", 2, 3, "open"), { lane: L2 })]));
  openPick(x, LIST);
  x.ids.pickerClose.click();
  const s = strip(x, 0);
  s.ch.value = "3"; s.ch.dispatch("change");
  x.send({ type: "liveLevels", levels: { [UR + "|3"]: -6, [L2]: -36 } });
  return lastCfg(x).inputs[0].chan === 3 && lvl(x, 0) === "0.900";
};
const twoSpeakersOneDevice = (x) => {  // two lanes of one device: two paragraphs, two names, two colours
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  x.send({ type: "translatorLine", t: 61, segId: 61, lang: "en", src: "Levels first.", dst: "先调电平。", dstLang: "zh", spk: SPK_A });
  x.send({ type: "translatorLine", t: 62, segId: 62, lang: "zh", src: "好的。", dst: "Sure.", dstLang: "en", spk: SPK_B });
  const paras = x.ids.log.querySelectorAll(".tr-para");
  const names = x.ids.log.querySelectorAll(".tr-who-name").map((n) => n.textContent);
  return paras.length === 2 && paras[0].dataset.spk === L1 + "|en" && paras[1].dataset.spk === L2 + "|zh"
    && J(names) === J(["Me", "Guest"]) && paras[0].style["--spk"] !== paras[1].style["--spk"];
};
const genNotCompared = (x) => {
  x.send(liveSt(false, []));
  openPick(x, LIST, 7);
  x.send({ type: "liveSources", gen: 1, list: [LIST[1]] });            // an engine restarted: its gens start again
  return pickRows(x).length === 1 && pickRow(x, MIC) !== null;
};
const disabledRowInert = (x) => {
  x.send(liveSt(false, []));
  openPick(x, LIST);
  const greyed = pickAdd(x, ARR).disabled && pickRow(x, ARR).classList.contains("is-off");
  const old = pickAdd(x, MIC);                                          // a row the NEXT list will grey out
  x.send({ type: "liveSources", gen: 2, list: LIST.map((s) => (s.key === MIC ? Object.assign({}, s, { state: "busy" }) : s)) });
  x.outbox.length = 0;
  pickAdd(x, ARR).click();                                              // (the stub fires even a disabled button)
  old.click();                                                          // ...and even one the list has replaced since
  return greyed && x.count("liveConfig") === 0 && stripsOf(x).length === 0;
};
const typingKeepsKeys = (x) => {
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const s = typeName(x, 0, "Me");
  x.win.dispatch("keydown", { key: "-", ctrlKey: true, target: s.name });
  x.win.dispatch("keydown", { key: "End", target: s.name });
  return !("audiopb.tr.font" in x.lsStore);
};
const ctrl0Azerty = (x) => {
  x.ids.fontUp.click();
  const up = x.lsStore["audiopb.tr.font"] === "30";
  x.win.dispatch("keydown", { key: "à", code: "Digit0", ctrlKey: true });
  return up && x.lsStore["audiopb.tr.font"] === "28";
};

section("CONTRACT -- page -> app: ready, translatorCtl (only the changed field), translatorExport");
{
  const x = runLive();
  ok("ready{} alone, as 1.1.0 (the app knows which view it is)", J(x.outbox) === J([{ type: "ready" }]));
  ok("ONLY THE FIELD THAT CHANGED travels: mode, preset, the pair (both halves), alwaysOnTop, clear -- one message each", onlyChanged(runLive()));
  ok("...and a preset already in force sends nothing (asked, or never chosen and running)", presetUnchangedSilent(runLive()));
  const y = runLive();
  y.send(st());
  y.outbox.length = 0;
  ["dirBtn", "modeA2B", "showA", "fontUp", "pinToggle", "themeBtn", "clearBtn", "exportBtn"].forEach((id) => y.ids[id].click());
  ok("the page never names a scope: the app adds scope=live (this view's translatorCtl IS the live pair)",
     y.outbox.length > 0 && y.outbox.every((m) => !("scope" in m)) && !/scope/.test(codeOnly(pageOnly)), y.outbox);
  ok("...and it sends nothing of the player's (no translatorFile*, player*, openLive, translatorFullscreen)",
     !/translatorFile|"player[A-Z]|openLive|translatorFullscreen|translatorTopmost/.test(codeOnly(pageOnly)));
  const sentTypes = [...new Set([...codeOnly(script).matchAll(/send\(\{\s*type:\s*"(\w+)"/g)].map((m) => m[1]))].sort();
  ok("the page sends exactly the contract's six message types (ready, translatorCtl, translatorExport, liveSources, liveConfig, liveListen)",
     J(sentTypes) === J(["liveConfig", "liveListen", "liveSources", "ready", "translatorCtl", "translatorExport"]), sentTypes);
  const handled = [...new Set([...pageOnly.matchAll(/msg\.type === "(\w+)"/g)].map((m) => m[1]))].sort();
  ok("...and handles exactly the contract's inbound types (+ nothing else)", J(handled) === J(["liveLevels", "liveSources", "translatorEnd", "translatorExported",
     "translatorLine", "translatorPartial", "translatorState", "translatorWindow", "warn"]), handled);
}

section("CONTRACT -- the strips: the echo (translatorState.live), liveConfig on EVERY edit");
{
  ok("THE ECHO WINS: translatorState.live.inputs IS the strips (order, names, channels, trims)", echoWins(runLive()));
  ok("...a `live` with no `inputs` is not a report about them (the strips stand); an EMPTY list is (no strip)", inputsAbsentKeeps(runLive()));
  const x = runLive();
  x.send(liveSt(false, [inp(UR, "Me", 1, 0), inp(UR, "Guest", 2, 3)]));
  const s0 = strip(x, 0), s1 = strip(x, 1);
  ok("each strip: its lamp, its NAME (the speaker's), its source, its channel, its trim, its meter, a remove button",
     !!s0.lamp && s0.name.value === "Me" && s1.name.value === "Guest" && s0.ch.value === "1" && s1.ch.value === "2"
     && s0.db.textContent === "0.0 dB" && s1.db.textContent === "+3.0 dB" && s1.trim.value === "3" && !!s0.meter && s0.x.getAttribute("aria-label") === "Remove Me");
  ok("...the strip's group names it for a screen reader", s0.root.getAttribute("role") === "group" && s0.root.getAttribute("aria-label") === "Me");
  ok("...its kind tag and a source name before any list is known (a generic word, never the raw endpoint id)",
     s0.kind.textContent === "In" && s0.src.textContent === "Input" && s0.src.textContent.indexOf("{") < 0);
  ok("no strip: body.no-strips (the empty log says \"Add a source, then press Listen.\")", runLive().doc.body.classList.contains("no-strips")
     && !x.doc.body.classList.contains("no-strips") && /body\.no-strips #log:empty::after \{ content: "Add a source, then press Listen\."; \}/.test(html));
  // every edit -> one liveConfig with the WHOLE set, exactly {key, label, chan, gainDb}
  x.outbox.length = 0;
  const n1 = typeName(x, 1, "Invité");
  enter(n1);
  ok("a NAME committed (Enter) -> ONE liveConfig with the whole set, each input exactly {key, label, chan, gainDb}",
     J(x.outbox) === J([{ type: "liveConfig", inputs: [{ key: UR, label: "Me", chan: 1, gainDb: 0 }, { key: UR, label: "Invité", chan: 2, gainDb: 3 }] }]), x.outbox);
  x.outbox.length = 0;
  n1.name.focus(); n1.name.blur();
  ok("...an unchanged name sends nothing", x.outbox.length === 0, x.outbox);
  const n0 = typeName(x, 0, "Moi");
  n0.name.blur();
  ok("leaving the field commits too (blur)", x.count("liveConfig") === 1 && lastCfg(x).inputs[0].label === "Moi");
  x.outbox.length = 0;
  const n2 = typeName(x, 0, "Somebody else");
  n2.name.dispatch("keydown", { key: "Escape" });
  ok("Esc gives the old name back and sends nothing", n2.name.value === "Moi" && x.count("liveConfig") === 0, [n2.name.value, x.outbox]);
  const n3 = typeName(x, 0, "   ");
  enter(n3);
  ok("an EMPTY name falls back to the default (the source's name), never to nothing", lastCfg(x).inputs[0].label === "Input · ch 1", lastCfg(x).inputs[0].label);
  x.outbox.length = 0;
  strip(x, 1).ch.value = "3"; strip(x, 1).ch.dispatch("change");
  ok("a CHANNEL changed -> one liveConfig (chan 3; a custom name stays)", J(lastCfg(x).inputs[1]) === J({ key: UR, label: "Invité", chan: 3, gainDb: 3 }) && x.count("liveConfig") === 1, x.outbox);
  x.outbox.length = 0;
  strip(x, 1).ch.value = "3"; strip(x, 1).ch.dispatch("change");
  ok("...the same channel again sends nothing", x.count("liveConfig") === 0);
  strip(x, 1).ch.value = "0"; strip(x, 1).ch.dispatch("change");
  ok("...and a channel the RULE forbids (its Mix next to channel 1) is never sent", x.count("liveConfig") === 0 && strip(x, 1).ch.value === "3");
  x.outbox.length = 0;
  strip(x, 0).trim.value = "-7.5"; strip(x, 0).trim.dispatch("change");
  ok("a TRIM let go -> one liveConfig with the exact value", x.count("liveConfig") === 1 && lastCfg(x).inputs[0].gainDb === -7.5 && strip(x, 0).db.textContent === "−7.5 dB");
  x.outbox.length = 0;
  strip(x, 0).trim.dispatch("dblclick");
  ok("...a double-click on it -> back to 0 dB (sent)", x.count("liveConfig") === 1 && lastCfg(x).inputs[0].gainDb === 0);
  x.outbox.length = 0;
  strip(x, 0).trim.value = "40"; strip(x, 0).trim.dispatch("change");
  ok("...a value outside -24..+24 is clamped before it travels", lastCfg(x).inputs[0].gainDb === 24, lastCfg(x));
  ok("WHILE IT MOVES the readout follows at once, the app hears it at most every 200 ms, and exactly when let go", trimThrottled(runLive()));
  x.outbox.length = 0;
  strip(x, 1).x.click();
  ok("a strip REMOVED -> one liveConfig without it", x.count("liveConfig") === 1 && J(lastCfg(x).inputs.map((i) => i.label)) === J(["Input · ch 1"]) && stripsOf(x).length === 1);
  ok("...its element gone, and the keyboard on the empty slot", El_active(x) === x.ids.addSource);
  x.outbox.length = 0;
  strip(x, 0).x.click();
  ok("...the last one too: liveConfig{inputs:[]} (0 strips is a set)", J(x.outbox) === J([{ type: "liveConfig", inputs: [] }]) && stripsOf(x).length === 0
     && x.doc.body.classList.contains("no-strips"));
  ok("NO FLICKER: an echo the app posted before reading our edit does not take the new strip away; ours then lands, and a later change of the app's wins at once", noFlicker(runLive()));
  ok("...and an edit the app REFUSED (R4: a warn + its previous set, sent ONCE) is shown as refused once the short pending window is over -- no further message needed", refusalShows(runLive()));
  ok("THE CONTROL UNDER THE HAND keeps what the user is doing: an echo does not overwrite a name being typed; leaving the field sends it", focusedNameKept(runLive()));
  const y = runLive();
  y.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  const t0 = strip(y, 0).trim;
  t0.dispatch("pointerdown");
  t0.value = "5"; t0.dispatch("input");
  y.send(liveSt(false, [inp(UR, "Me", 1, 2)]));
  ok("...nor a trim being dragged (the thumb stays under the pointer)", t0.value === "5" && strip(y, 0).db.textContent === "+5.0 dB");
  y.win.dispatch("pointerup");
}
function El_active(x) { return x.doc.activeElement; }

section("CONTRACT -- the picker: liveSources{} when it opens, the list, the rules");
{
  ok("liveSources{} is sent EXACTLY when the picker opens -- never at load, on a state, on a line, on levels or on a list pushed by the app", sourcesOnlyOnOpen(runLive()));
  const x = runLive();
  x.send(liveSt(false, []));
  x.ids.addSource.click();
  ok("opened before any list: it says so (\"Looking for sources…\"), the Add slot says it is expanded",
     x.ids.picker.hidden === false && x.ids.pickerNote.textContent === "Looking for sources…" && x.ids.addSource.getAttribute("aria-expanded") === "true");
  ok("...and the keyboard waits on its Close button", El_active(x) === x.ids.pickerClose);
  x.send({ type: "liveSources", gen: 1, list: LIST });
  const secs = x.ids.pickerList.children.map((s) => q(s, ".pk-sec-title").textContent);
  ok("the list lands: THREE sections, Inputs / Sound of an output / Applications", J(secs) === J(["Inputs", "Sound of an output", "Applications"]), secs);
  ok("...the keyboard moves to the first row", El_active(x) === pickAdd(x, UR));
  const names = pickRows(x).map((r) => q(r, ".pk-name").textContent);
  ok("...rows sorted: the usable ones first, Windows' defaults first among them, then by name",
     J(names) === J(["Line (Studio Interface)", "Headset Microphone", "Virtual Line Output", "Microphone Array", "Speakers", "Meeting app"]), names);
  const badges = (key) => pickRow(x, key).querySelectorAll(".pk-badge").map((b) => b.textContent).join(",");
  ok("...each row: name + badges (Default, Communications -- from a comma list or an array)", badges(UR) === "Default" && badges(MIC) === "Communications"
     && badges(SPK) === "Default,Communications" && badges(CAB) === "");
  const arr = pickRow(x, ARR);
  ok("a row that cannot be added is GREYED, says WHY, and does nothing", arr.classList.contains("is-off") && q(arr, ".pk-add").disabled
     && q(arr, ".pk-why").textContent === "Windows blocks microphone access for desktop apps (Settings › Privacy › Microphone)");
  ok("...a click on it (the stub fires even a disabled button) adds nothing", disabledRowInert(runLive()));
  const y = runLive();
  y.send(liveSt(false, []));
  openPick(y, [{ kind: "in", key: MIC, label: "Unplugged", state: "absent", ch: 1 }, { kind: "in", key: CAB, label: "Taken", state: "busy", ch: 2 },
    { kind: "app", key: ZOOM, label: "Old Windows", state: "unsupported", ch: 2 }, { kind: "out", key: SPK, label: "Odd", state: "weird", ch: 2 },
    { kind: "in", key: SPK, label: "kind and key disagree", state: "ok", ch: 2 }, { kind: "sys", key: "sys:", label: "not a kind", state: "ok" }]);
  const why = (key) => { const r = pickRow(y, key); return r ? q(r, ".pk-why").textContent : null; };
  ok("the reasons: absent, busy, an application on a Windows without process loopback, an unknown state",
     why(MIC) === "Not connected" && why(CAB) === "In use by another program (exclusive mode)"
     && why(ZOOM) === "Needs Windows 11 (or Windows 10 build 20348 or later)" && why(SPK) === "Unavailable", [why(MIC), why(CAB), why(ZOOM), why(SPK)]);
  ok("...and a row whose kind does not match its key, or of a kind the contract does not have (sys:), is not listed", pickRows(y).length === 4);
  const z = runLive();
  z.send(liveSt(false, []));
  openPick(z, LIST);
  z.outbox.length = 0;
  pickAdd(z, MIC).click();
  ok("ADDING a source: one liveConfig, the new strip on its MIX, named after the source -- then the picker closes and says so (liveSources{open:false}, RVB-7)",
     J(z.outbox) === J([{ type: "liveConfig", inputs: [{ key: MIC, label: "Headset Microphone", chan: 0, gainDb: 0 }] }, { type: "liveSources", open: false }]), z.outbox);
  ok("...the picker closes and the new strip's NAME has the keyboard (next: type the speaker's name)", z.ids.picker.hidden === true && El_active(z) === strip(z, 0).name);
  ok("...the strip shows its source's name and kind", strip(z, 0).src.textContent === "Headset Microphone" && strip(z, 0).kind.textContent === "In");
  openPick(z, LIST);
  ok("the same mono source again: refused, with the reason", q(pickRow(z, MIC), ".pk-add").disabled && q(pickRow(z, MIC), ".pk-why").textContent === "Already on a strip");
  z.outbox.length = 0;
  pickChip(z, UR, 1).click();
  ok("a CHANNEL of a multichannel device, from its chip: named \"<source> · ch N\"", J(lastCfg(z).inputs[1]) === J({ key: UR, label: "Line (Studio Interface) · ch 1", chan: 1, gainDb: 0 }), lastCfg(z));
  openPick(z, LIST);
  ok("THE RULE (one device twice only on two different channels >= 1, never next to its mix): the row now adds channel 2, Mix and 1 are disabled", clashRule(runLive()));
  ok("applications are one stream: no channel chips; inputs and outputs with 2+ channels have them (Mix + 1..ch)",
     pickRow(z, ZOOM).querySelectorAll(".pk-ch").length === 0 && pickRow(z, SPK).querySelectorAll(".pk-ch").length === 3
     && pickRow(z, MIC).querySelectorAll(".pk-ch").length === 0 && pickRow(z, UR).querySelectorAll(".pk-ch").length === 7);
  z.outbox.length = 0;
  pickAdd(z, ZOOM).click();
  ok("an application is added on its mix", lastCfg(z).inputs[2].key === ZOOM && lastCfg(z).inputs[2].chan === 0 && lastCfg(z).inputs[2].label === "Meeting app");
  ok("FOUR AT MOST: with four strips the Add slot is disabled (and says why), the picker says so and every row is inert", fourMax(runLive()));
  const v = runLive();
  v.send(liveSt(false, []));
  openPick(v, LIST);
  v.picker = v.ids.picker;
  v.ids.picker.dispatch("keydown", { key: "ArrowDown" });
  const second = El_active(v);
  v.ids.picker.dispatch("keydown", { key: "ArrowUp" });
  ok("THE KEYBOARD: arrows move along the rows and their channels", second === pickChip(v, UR, 0) && El_active(v) === pickAdd(v, UR), [second && second.dataset.pick]);
  v.ids.picker.dispatch("keydown", { key: "Escape" });
  ok("...Esc closes it and gives the keyboard back to the Add slot", v.ids.picker.hidden === true && El_active(v) === v.ids.addSource && v.ids.addSource.getAttribute("aria-expanded") === "false");
  openPick(v, LIST);
  v.win.dispatch("pointerdown", { target: v.ids.log });
  ok("...a press anywhere else closes it", v.ids.picker.hidden === true);
  openPick(v, LIST);
  v.win.dispatch("pointerdown", { target: q(pickRow(v, UR), ".pk-name") });
  ok("...but not a press inside it", v.ids.picker.hidden === false);
  pickChip(v, UR, 3).focus();
  v.send({ type: "liveSources", gen: 2, list: LIST.slice().reverse() });
  ok("a list pushed while it is open repaints it -- the keyboard stays on the same point", El_active(v) && El_active(v).dataset.pick === UR + "#3");
  ok("the newest list to ARRIVE is the list (an engine restart starts gen again at 1: never compared)", genNotCompared(runLive()));
  ok("a device's name is CLEANED before it is shown or becomes a strip's name (no bidi override, no isolate)", deviceNameClean(runLive()));
  const u = runLive();
  u.send(liveSt(false, []));
  const longName = "A very long device name that goes on and on — Ünïcödé — 设备名称很长很长很长";
  openPick(u, [{ kind: "in", key: UR, label: longName, state: "ok", ch: 4, flags: "" }]);
  pickChip(u, UR, 4).click();
  const lab = lastCfg(u).inputs[0].label;
  ok("a default name is cut to 64 BYTES of UTF-8 on a whole character, \"…\", keeping its \" · ch N\"",
     Buffer.byteLength(lab, "utf8") <= 64 && /… · ch 4$/.test(lab) && lab.indexOf("\uFFFD") < 0, [lab, Buffer.byteLength(lab, "utf8")]);
}

section("CONTRACT -- the strip's name: the speaker's, <= 64 bytes, clean; its channel; its source remembered");
{
  ok("a name typed is held to 64 BYTES of UTF-8 as it is typed (32 x é), and an emoji is never cut in half", labelBytes(runLive()));
  ok("control and bidi characters never reach the wire", labelClean(runLive()));
  ok("a channel changed on a strip still named by default: its name follows (\"... · ch 2\"); a name the user chose stays", channelLabelFollows(runLive()));
  const x = runLive();
  x.send(liveSt(false, [inp(UR, "Line (Studio Interface) · ch 1", 1, 0)]));
  x.outbox.length = 0;
  strip(x, 0).ch.value = "2"; strip(x, 0).ch.dispatch("change");
  ok("...even before any list (by its \" · ch N\" ending)", lastCfg(x).inputs[0].label === "Line (Studio Interface) · ch 2", lastCfg(x));
  const y = runLive();
  y.send(liveSt(false, [inp(UR, "A", 1, 0), inp(UR, "B", 2, 0)]));
  openPick(y, LIST);
  y.ids.pickerClose.click();
  const opts = strip(y, 0).ch.children.map((o) => o.textContent + (o.disabled ? "x" : "")).join(",");
  ok("the channel selector: Mix + every channel of the device; Mix and the other strip's channel offered DISABLED", opts === "Mixx,Ch 1,Ch 2x,Ch 3,Ch 4,Ch 5,Ch 6", opts);
  const m = runLive();
  m.send(liveSt(false, [inp(MIC, "Host", 0, 0)]));
  openPick(m, LIST);
  ok("...a mono device: Mix only", strip(m, 0).ch.children.map((o) => o.textContent).join() === "Mix");
  const u = runLive();
  u.send(liveSt(false, [inp(CAB, "Cable", 0, 0)]));
  ok("...a device never listed yet: Mix + 1..2 (and its title says how to list them all)", strip(u, 0).ch.children.map((o) => o.textContent).join() === "Mix,Ch 1,Ch 2"
     && /open Add source once/.test(strip(u, 0).ch.title));
  ok("A SOURCE IS REMEMBERED (suvtitle.live.sources): after a restart, before any list, a strip shows its source's name and channels", cacheAfterReload(runLive({ ls: CACHE })));
  const z = runLive();
  z.send(liveSt(false, []));
  openPick(z, LIST);
  pickChip(z, UR, 1).click();
  const kept = JSON.parse(z.lsStore["suvtitle.live.sources"] || "{}");
  ok("...written when a strip is added (name, channel count -- nothing else)", J(kept[UR]) === J(["Line (Studio Interface)", 6]), kept);
  ok("...a damaged value is ignored, never fatal", (() => { try { const d = runLive({ ls: { "suvtitle.live.sources": "{not json" } }); d.send(liveSt(false, [inp(UR, "Me", 1, 0)])); return strip(d, 0).src.textContent === "Input"; } catch (e) { return false; } })());
  const keys = new Set();
  const all = runLive({ ls: CACHE });
  all.send(liveSt(false, []));
  openPick(all, LIST);
  pickAdd(all, MIC).click();
  all.ids.fontUp.click(); all.ids.showA.click(); all.ids.themeBtn.click();
  Object.keys(all.lsStore).forEach((k) => keys.add(k));
  ok("the page writes ONLY its four keys to the profile (theme, font, show -- the player's too -- and its sources)",
     [...keys].every((k) => ["patchbus.theme", "audiopb.tr.font", "audiopb.tr.show", "suvtitle.live.sources"].indexOf(k) >= 0) && keys.size === 4, [...keys]);
}

section("CONTRACT -- Listen, the On air lamp, the INPUT states, the meters (liveLevels)");
{
  ok("LISTEN WITH NO STRIP still answers: liveListen{on:true} is sent, a hint on the state line, the empty slot calls attention", listenNoStripHint(runLive()));
  ok("LISTEN asks; ONLY THE ECHO (translatorState.live.on) lights the button and the On air lamp -- the dot blinks in between", lampOnEchoOnly(runLive()));
  ok("R4: live.wanted without live.on = STARTING -- the dot blinks, \"starting to listen…\", and a second press asks to stop (a start can be cancelled)", wantedStarting(runLive()));
  const x = runLive();
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open"), inp(UR, "Guest", 2, 3, "absent"), inp(MIC, "Host", 0, 0, "denied", "Microphone access is off for desktop apps."), inp(SPK, "Room", 0, 0, "stalled")]));
  x.outbox.length = 0;
  x.ids.listenBtn.click();
  ok("pressed while listening: liveListen{on:false}", J(x.outbox) === J([{ type: "liveListen", on: false }]));
  const lamp = (i) => strip(x, i).lamp.className, whyT = (i) => (strip(x, i).why.hidden ? "" : strip(x, i).why.textContent);
  ok("the INPUT states on the lamps: open = green, absent = dashed, denied = red, stalled = amber", lamp(0) === "strip-lamp is-open" && lamp(1) === "strip-lamp is-absent"
     && lamp(2) === "strip-lamp is-bad" && lamp(3) === "strip-lamp is-warn", [0, 1, 2, 3].map(lamp));
  ok("...and their reasons on the strip: the engine's sentence when it gave one, ours otherwise", whyT(0) === "" && whyT(1) === "Not connected"
     && whyT(2) === "Microphone access is off for desktop apps." && whyT(3) === "No sound arriving" && strip(x, 2).why.classList.contains("is-bad"));
  ok("...also in the strip's accessible name", strip(x, 2).root.getAttribute("aria-label") === "Host — Microphone access is off for desktop apps."
     && strip(x, 0).root.getAttribute("aria-label") === "Me — open");
  x.send(liveSt(false, [inp(UR, "Me", 1, 0, "open"), inp(UR, "Guest", 2, 3, "absent"), inp(MIC, "Host", 0, 0, "denied", "Microphone access is off for desktop apps."), inp(SPK, "Room", 0, 0, "stalled")]));
  ok("stopped: the lamps go dark -- except a FAILURE, which keeps its red and its reason until the next start",
     lamp(0) === "strip-lamp" && lamp(1) === "strip-lamp" && lamp(3) === "strip-lamp" && lamp(2) === "strip-lamp is-bad" && whyT(2) !== "" && whyT(1) === "");
  ok("METERS ONLY WHILE LISTENING: levels received when not listening paint nothing", levelsOnlyOnAir(runLive()));
  const y = runLive();
  y.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  const at = (db) => { y.send({ type: "liveLevels", levels: { [L1]: db } }); return lvl(y, 0); };
  ok("the scale: -60 dBFS = empty, -30 = half, 0 = full; above 0 and below -60 clamped, garbage ignored",
     at(-60) === "0.000" && at(-30) === "0.500" && at(0) === "1.000" && at(12) === "1.000" && at(-120) === "0.000"
     && at("x") === "0.000" && at(null) === "0.000", [at(-60), at(-30)]);
  y.pumpTimers(2000);
  y.send({ type: "liveLevels", levels: { [L1]: -6 } });
  y.pumpTimers(500);
  y.send({ type: "liveLevels", levels: { [L1]: -42 } });
  const pk = strip(y, 0).meter.style["--pk"];
  ok("PEAK HOLD: the peak stays at the loudest of the last 1.5 s while the bar falls", lvl(y, 0) === "0.300" && pk === "0.900", [lvl(y, 0), pk]);
  y.pumpTimers(1100);
  y.send({ type: "liveLevels", levels: { [L1]: -42 } });
  ok("...then falls to the bar", strip(y, 0).meter.style["--pk"] === "0.300");
  y.send({ type: "liveLevels", levels: { [L1]: -0.5 } });
  ok("CLIP: at -1 dBFS and above the meter's edge goes red, for 2 s", strip(y, 0).meter.classList.contains("is-clip"));
  y.pumpTimers(1000);
  y.send({ type: "liveLevels", levels: { [L1]: -20 } });
  const still = strip(y, 0).meter.classList.contains("is-clip");
  y.pumpTimers(1100);
  y.send({ type: "liveLevels", levels: { [L1]: -20 } });
  ok("...then clears", still && !strip(y, 0).meter.classList.contains("is-clip"));
  ok("...its reading in the meter's tooltip (dBFS, one decimal)", strip(y, 0).meter.title === "-20.0 dBFS", strip(y, 0).meter.title);
  ok("STALE LEVELS EMPTY THE METERS (1.2 s after the last): a frozen meter would say a microphone still hears something", metersGoStale(runLive()));
  ok("...and stopping empties them at once", metersOffOnStop(runLive()));
  ok("R1 -- TWO STRIPS ON CHANNELS 1 AND 2 OF ONE SOURCE: two meters, each reading its LANE (\"<key>|<chan>\", TrPipe.h LANES) -- built by the page's own rule while the echo names none",
     levelsByLane(runLive()));
  ok("...the lane the APP names (translatorState.live.inputs[].lane) is the one read", laneFromEcho(runLive()));
  ok("...the device's bare key (its MIX's lane) lights no channel strip -- never \"the first strip of that device\"", deviceKeyLightsNoChannel(runLive()));
  ok("...a strip whose channel just moved reads its NEW lane at once (the echo's name was the old channel's)", laneAfterChannelMove(runLive()));
  ok("...and their lines: two paragraphs, two names, two colours (the app's speaker key = {lane, language})", twoSpeakersOneDevice(runLive()));
  const z = runLive();
  z.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  z.send({ type: "liveLevels", levels: { [L1]: -18 } });
  const z0 = lvl(z, 0);
  z.send({ type: "liveLevels", levels: [1, 2] });
  z.send({ type: "liveLevels" });
  ok("...a malformed message changes nothing", z0 === "0.700" && lvl(z, 0) === "0.700" && lvl(z, 1) === "0.000");
}

section("CONTRACT -- the state line, the window, warn, cleared, the replay");
{
  const x = runLive();
  x.send(st());
  ok("running, not listening: \"not listening · <models> · GPU\" (the models short: small → large-v3-turbo · Qwen3-4B)",
     stateLine(x) === "not listening · small → large-v3-turbo · Qwen3-4B · GPU", stateLine(x));
  ok("THE WINDOW NEVER SAYS \"listening\" WHILE NO INPUT IS OPEN (the engine's \"listening\" also means idle)", neverSaysListening(runLive()));
  x.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i)), { spk: { on: true, model: "campplus", speakers: 3 } }));
  ok("listening: \"listening · small → large-v3-turbo · Qwen3-4B · GPU · 2 inputs · 3 voices\", green",
     stateLine(x) === "listening · small → large-v3-turbo · Qwen3-4B · GPU · 2 inputs · 3 voices" && x.ids.state.classList.contains("is-listening"), stateLine(x));
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")], { status: "transcribing", gpu: false, spk: { on: true, model: "campplus", speakers: 1 } }));
  ok("...transcribing, on the CPU, one input, one voice (singular)", stateLine(x) === "transcribing… · small → large-v3-turbo · Qwen3-4B · CPU · 1 input · 1 voice"
     && x.ids.state.classList.contains("is-transcribing"), stateLine(x));
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")], { spk: { on: false, model: "", speakers: 0 } }));
  ok("...no voices without the diarization stage", !/voice/.test(stateLine(x)), stateLine(x));
  x.send(st({ status: "analyzing" }));
  ok("the player's film being analysed, not listening: \"analysing a film\"", /^analysing a film · /.test(stateLine(x)) && !x.ids.state.classList.contains("is-listening"), stateLine(x));
  x.send(st({ status: "loading", models: { asr: "", mt: "" } }));
  ok("loading: \"loading models…\", amber", stateLine(x) === "loading models…" && x.ids.state.classList.contains("is-loading"));
  x.send(st({ status: "error", err: "No model for the live\u202E pair." }));
  ok("an error: \"error — <the engine's sentence>\", red, its sentence CLEANED", stateLine(x) === "error — No model for the live pair." && x.ids.state.classList.contains("is-error"), stateLine(x));
  const y = runLive();
  y.send(st({ running: false, status: "loading" }));
  ok("an engine not running (RV4-4): \"starting…\"", stateLine(y) === "starting…");
  y.pumpTimers(15100);
  ok("...then, 15 s down, \"engine not running\" in red", stateLine(y) === "engine not running" && y.ids.state.classList.contains("is-error"));
  y.send(st({ models: "small→turbo · Qwen3-4B Q5_K_M" }));
  ok("a `models` STRING (an older sender) is read too", stateLine(y) === "not listening · small → turbo · Qwen3-4B · GPU", stateLine(y));
  ok("WARN{text}: the app's sentence on the state line, in the error style, for 6 s", warnShown(runLive()));
  const z = runLive();
  z.send({ type: "warn", text: "Bad\u202Edevice <b>x</b>" });
  ok("...cleaned, and as TEXT", stateLine(z) === "Bad device <b>x</b>", stateLine(z));
  ok("translatorWindow{topmost}: the pin follows the window's REAL state; absent, it stays; the first click then UNPINS (RV4-7)", topmostFollows(runLive()));
  ok("CLEARED: {cleared:true} -- from either side -- empties the log and keeps the state (listening, the strips)", clearedKeepsState(runLive()));
  ok("THE REPLAY: a line already shown (same t + src) is applied once", replayOnce(runLive()));
  const r = runLive();
  r.send(liveSt(true, TWO.map((i) => Object.assign({ state: "open" }, i))));
  r.ids.clearBtn.click();
  ok("the user's Clear: the log goes, translatorCtl{clear:true} -- and nothing of the strips or Listen", J(r.outbox.slice(1)) === J([{ type: "translatorCtl", clear: true }])
     && stripsOf(r).length === 2, r.outbox);
}

section("L3 -- the idle fade, the tally, the keyboard");
{
  ok("THE DECK FADES ONLY WHILE LISTENING (nothing to read otherwise, Listen in sight), never with the picker open", noFadeOffAir(runLive()));
  const x = runLive();
  x.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")]));
  typeName(x, 0, "Mo");
  x.pumpTimers(5000);
  ok("...nor while a name is being typed", !x.ids.deck.classList.contains("is-idle"));
  strip(x, 0).name.blur();
  x.win.dispatch("pointermove");
  x.pumpTimers(3100);
  ok("...and the ON AIR tally stays in sight over a faded deck (CSS: the deck idle, on air)", x.ids.deck.classList.contains("is-idle")
     && /body\.is-on-air #deck\.is-idle ~ #tally \{ opacity: 0\.85; \}/.test(html) && x.doc.body.classList.contains("is-on-air"));
  x.send(liveSt(false, [inp(UR, "Me", 1, 0)]));
  ok("stopping brings the deck back at once", !x.ids.deck.classList.contains("is-idle"));
  ok("a name being typed keeps its keys (End, Ctrl+-) -- they are the text field's, not the dialogue's", typingKeepsKeys(runLive()));
  ok("Ctrl+0 by the PHYSICAL key (RV4-8): AZERTY's top-row 0 types \"à\"", ctrl0Azerty(runLive()));
  const y = runLive();
  y.win.dispatch("wheel", { ctrlKey: true, deltaY: -100 });
  ok("Ctrl+wheel = the dialogue's size, kept under the key shared with the player (audiopb.tr.font)", y.lsStore["audiopb.tr.font"] === "30"
     && y.doc.documentElement.style["--tr-font"] === "30px");
  ok("A-/A+ bounded 14..72", (() => { const f = runLive({ ls: { "audiopb.tr.font": "71" } }); f.ids.fontUp.click(); f.ids.fontUp.click(); const hi = f.lsStore["audiopb.tr.font"] === "72"; const g = runLive({ ls: { "audiopb.tr.font": "9" } }); return hi && g.doc.documentElement.style["--tr-font"] === "14px"; })());
}

section("SECURITY -- nothing from outside becomes markup; every NAME is cleaned");
{
  ok("THE SECURITY RULE: nothing in the page's code can build markup -- no innerHTML / outerHTML / insertAdjacentHTML / document.write, no eval / new Function",
     !MARKUP_SINK.test(codeOnly(script)), (codeOnly(script).match(MARKUP_SINK) || [])[0]);
  [["innerHTML", "n.why.innerHTML = why;"], ["outerHTML", "n.why.outerHTML = why;"], ["insertAdjacentHTML", "n.why.insertAdjacentHTML(\"beforeend\", why);"],
   ["document.write", "document.write(why);"]].forEach(([sink, code]) => {
    const m = script.replace("    n.why.textContent = why;\n", "    " + code + "\n");
    ok("MUTANT \"a strip's reason written with " + sink + "\" is caught by THE SECURITY RULE", m !== script && MARKUP_SINK.test(codeOnly(m)));
  });
  ok("...and the lexer is not fooled by a sink after a \"//\" inside a string", MARKUP_SINK.test(codeOnly('const g = "a // b"; el.x.innerHTML = t;')));
  const x = runLive();
  const nasty = "<img src=x onerror=alert(1)>";
  x.send(liveSt(true, [inp(UR, nasty, 1, 0, "denied", nasty)]));
  openPick(x, [{ kind: "in", key: MIC, label: nasty, state: "ok", ch: 1 }]);
  x.send({ type: "translatorLine", t: 1, lang: "en", src: nasty, dst: nasty, spk: { key: "k", label: nasty, color: 2 } });
  x.send({ type: "translatorPartial", segId: 3, t: 2, lang: "en", src: nasty, dst: nasty });
  x.pumpTimers(2000);
  const nodes = []; (function walk(n) { nodes.push(n); n.children.forEach(walk); })(x.doc.documentElement);
  ok("a device's name, a strip's name, an engine's reason, a speaker's name, a line, a draft: all shown as TEXT, no element made of them",
     strip(x, 0).name.value === nasty && strip(x, 0).why.textContent === nasty && q(pickRow(x, MIC), ".pk-name").textContent === nasty
     && x.ids.log.querySelector(".tr-who-name").textContent === nasty && x.ids.log.querySelector(".tr-dst").textContent === nasty
     && nodes.every((n) => n.tagName !== "IMG"));
  const y = runLive();
  y.send({ type: "translatorLine", t: 1, lang: "ar", src: "\u200Fمرحبا\u200E", dst: "hello", spk: { key: "k", label: "Ali\u202E\u2067", color: 0 } });
  ok("a TRANSCRIPT's text is not touched (RLM / LRM are real there); the speaker's NAME is cleaned",
     y.ids.log.querySelector(".tr-src").textContent === "\u200Fمرحبا\u200E" && y.ids.log.querySelector(".tr-who-name").textContent === "Ali");
  const z = runLive();
  z.send({ type: "translatorExported", ok: true, path: "D:\\x\\evil\u202Excod.docx" });
  ok("the export's file name is cleaned too", stateLine(z) === "Saved · evil xcod.docx", stateLine(z));
  const r = runLive();
  r.send(liveSt(true, [inp(UR, "Me", 1, 0, "open")], { spk: { on: true, model: "campplus", speakers: 2 } }));
  openPick(r, LIST.filter((s) => s.key !== CAB));
  r.send({ type: "translatorLine", t: 1, lang: "en", src: "Hello.", dst: "你好。", spk: SPK_A });
  r.send({ type: "warn", text: "The engine stopped responding; restarting it." });
  const words = rendered(r);
  ok("not one word of AudioPB's world on screen (AudioPB, card, cable, fabric, Translator, Overcame) in a full scene",
     !/AudioPB|\bcards?\b|\bcables?\b|fabric|\bTranslator\b|Overcame/i.test(words), (words.match(/.{0,30}(AudioPB|\bcards?\b|\bcables?\b|fabric|\bTranslator\b|Overcame).{0,30}/i) || [])[0]);
}

section("PARITY -- the page and SuVtitle's sources agree (TrPipe.h's live limits, index.html's cleaning)");
{
  const pipe = readSrc("common/TrPipe.h");
  const cnum = (name) => { const m = new RegExp("constexpr \\w+\\s+" + name + "\\s*=\\s*(-?[\\d.]+)").exec(pipe); return m ? Number(m[1]) : NaN; };
  const jnum = (name) => { const m = new RegExp("const " + name + " = (-?[\\d.]+)").exec(script); return m ? Number(m[1]) : NaN; };
  const jpair = (re) => { const m = re.exec(script); return m ? m.slice(1).map(Number) : []; };
  const [gmin, gmax] = jpair(/const GAIN_MIN = (-?\d+), GAIN_MAX = (-?\d+)/);
  ok("<= 4 strips (kMaxLiveInputs), names <= 64 bytes (kLiveLabelMax), channels 0..8 (kLiveMaxChan), trims -24..+24 dB (kLiveGain*)",
     cnum("kMaxLiveInputs") === 4 && jnum("MAX_STRIPS") === 4 && cnum("kLiveLabelMax") === 64 && jnum("LABEL_MAX") === 64
     && cnum("kLiveMaxChan") === 8 && jnum("MAX_CHAN") === 8 && cnum("kLiveGainMinDb") === gmin && cnum("kLiveGainMaxDb") === gmax && gmin === -24 && gmax === 24,
     { pipe: [cnum("kMaxLiveInputs"), cnum("kLiveLabelMax"), cnum("kLiveMaxChan"), cnum("kLiveGainMinDb"), cnum("kLiveGainMaxDb")], page: [jnum("MAX_STRIPS"), jnum("LABEL_MAX"), jnum("MAX_CHAN"), gmin, gmax] });
  // R1 (TrPipe.h LANES): ONE rule on three sides -- TrPipe.h's liveLaneId (the engine binds its lanes with it), the app
  // (live.inputs[].lane; a line's speaker label found BY LANE) and this page's laneRule. Read from the sources.
  const cLane = /inline std::string liveLaneId\(const std::string& key, uint32_t chan\) \{\s*return chan \? key \+ "(.)" \+ std::to_string\(chan\) : key;\s*\}/.exec(pipe);
  const jLane = /const laneRule = \(key, chan\) => \(chan \? key \+ "(.)" \+ chan : key\);/.exec(script);
  const bridge = readSrc("app/bridge.cpp"), hub = readSrc("engine/TrCaptureHub.cpp");
  const body = (src, head) => { const at = src.indexOf(head); return at < 0 ? "" : src.slice(at, src.indexOf("\n}\n", at) + 2); };
  const labelFn = body(bridge, "std::wstring Bridge::stripLabelFor("), liveFn = body(bridge, "std::wstring Bridge::liveJson()");
  ok("LANES: one rule, three sides -- TrPipe.h's liveLaneId, the engine's lane names, this page's laneRule (the same \"|\", the mix = the bare key)",
     !!cLane && !!jLane && cLane[1] === "|" && jLane[1] === "|"
     && /std::string laneName\(const Lane& l\) \{ return trpipe::liveLaneId\(l\.key, l\.chan\); \}/.test(hub), [cLane && cLane[1], jLane && jLane[1]]);
  ok("...and the app: live.inputs[].lane by that rule, a speaker's label looked up BY LANE (a channel's source name: \"<name> · ch N\") -- never the first strip of a device",
     liveFn.indexOf("\\\"lane\\\"") > 0 && liveFn.indexOf("pbt::trpipe::liveLaneId(s.key, s.chan)") > 0
     && labelFn.indexOf("pbt::trpipe::liveLaneId(s.key, s.chan) == id") > 0 && labelFn.indexOf("pbt::trpipe::liveLaneId(src.first, c) == id") > 0
     && !/\bs\.key == /.test(labelFn), { liveFn: liveFn.length, labelFn: labelFn.length });
  const kinds = ((/kinds\[\] = \{([^}]*)\}/.exec(pipe) || [])[1] || "").match(/"(\w+)"/g).map((s) => s.replace(/"/g, "")).join("|");
  ok("the key kinds are TrPipe.h's (liveKeyKind: in|out|app|wav)", kinds === "in|out|app|wav" && script.indexOf("/^(in|out|app|wav):./") > 0, kinds);
  const srcStates = ((/state = ok \| absent \| busy \| denied \| unsupported/.exec(pipe) || [])[0] || "");
  ok("the source states are SRC's (ok | absent | busy | denied | unsupported)", !!srcStates && /new Set\(\["ok", "absent", "busy", "denied", "unsupported"\]\)/.test(script));
  const inStates = (/state = open \| absent \| busy \| denied \| unsupported \| stalled \| error/.exec(pipe) || [])[0];
  ok("every INPUT state has its words (open | absent | busy | denied | unsupported | stalled | error)", !!inStates
     && ["absent", "busy", "denied", "unsupported", "stalled", "error"].every((s) => new RegExp(s + ": \"").test(script)) && /s\.state === "open"/.test(script));
  ok("the \"same key twice\" rule is unpackLive's condition, character for character in spirit: same channel, or either on its mix",
     /o\.key == in\.key && \(o\.chan == in\.chan \|\| o\.chan == 0 \|\| in\.chan == 0\)/.test(pipe)
     && /s\.key === key && \(s\.chan === chan \|\| s\.chan === 0 \|\| chan === 0\)/.test(script));
  // ...and BEHAVIOURALLY: the page's verdict on every pair of channels = unpackLive's
  const x = runLive();
  let agree = true;
  for (let a = 0; a <= 8 && agree; a++) for (let b = 0; b <= 8 && agree; b++) {
    x.send(liveSt(false, [inp(UR, "A", a, 0)]));
    x.send({ type: "liveSources", gen: 1, list: [{ kind: "in", key: UR, label: "Dev", state: "ok", ch: 8 }] });
    const opt = strip(x, 0) && x.ids.strips.children.length === 1;
    const refused = (a === b || a === 0 || b === 0);
    openPick(x, null);
    const chip = pickChip(x, UR, b);
    agree = opt && !!chip && chip.disabled === refused;
    x.ids.pickerClose.click();
  }
  ok("...behaviourally: for every pair of channels (0..8) on one device, the picker refuses exactly what unpackLive refuses", agree);
  const ix = P.readPage();
  const junk = (s) => (/const NAME_JUNK = (\/.*\/g);/.exec(s) || [])[1];
  ok("the NAME cleaning is index.html's (RV4-5: the same character class)", !!junk(html) && junk(html) === junk(ix), [junk(html), junk(ix)]);
}

section("L3 -- the design mock (a plain browser: the window plays itself)");
{
  const m = runLive({ noWebView: true, hash: "#listen" });
  m.pumpTimers(500);
  ok("no WebView2: nothing is posted, the mock plays the app -- two strips on one interface (\"Me\" ch 1, \"Guest\" ch 2)",
     m.outbox.length === 0 && ids(m).join() === ["#1", "#2"].map((c) => "in:{0.0.1.00000000}.{3c1e5a20-mock-iface}" + c).join() && strip(m, 1).name.value === "Guest");
  ok("...#listen: it listens, the lamp lit by its echo", m.ids.onAir.classList.contains("is-lit"));
  m.pumpTimers(6000);
  ok("...the meters move and the scripted meeting is spoken by the strips", parseFloat(lvl(m, 0)) > 0 && m.ids.log.querySelectorAll(".tr-who-name").some((n) => n.textContent === "Me"));
  m.ids.addSource.click();
  m.pumpTimers(400);
  ok("...the picker lists its sources", pickRows(m).length >= 6);
  const e = runLive({ noWebView: true, hash: "#empty" });
  e.pumpTimers(500);
  e.ids.listenBtn.click();
  ok("...#empty: no strip, and Listen answers with the hint", stripsOf(e).length === 0 && stateLine(e) === "Add a source first.");
}

// =====================================================================================================================
section("MUTATION SENTINELS -- live.html with one guard removed each");
{
  // A mutation that changes nothing would test the original page; a mutant that does not LOAD tests nothing; a probe
  // that THROWS proves only that the probe broke. Each of the three is a FAILED sentinel, named as such.
  const judge = (name, applies, load, probe) => {
    if (!applies) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    let page;
    try { page = load(); } catch (e) { return ok("MUTANT \"" + name + "\" is caught by its check", false, "the mutant does not load: " + e.message); }
    let healthy = true, threw = "";
    try { healthy = !!probe(page); } catch (e) { threw = String((e && e.message) || e); }
    if (threw) return ok("MUTANT \"" + name + "\" is caught by its check", false, "the probe THREW instead of failing: " + threw);
    ok("MUTANT \"" + name + "\" is caught by its check", !healthy);
  };
  const caught = (name, mutate, probe, opts) => judge(name, mutate(script) !== script, () => runLive(Object.assign({ mutate, name }, opts || {})), probe);
  const probes = { echoWins, inputsAbsentKeeps, noFlicker, refusalShows, lampOnEchoOnly, levelsOnlyOnAir, metersGoStale, metersOffOnStop, sourcesOnlyOnOpen, clashRule,
                   fourMax, labelBytes, labelClean, deviceNameClean, onlyChanged, presetUnchangedSilent, focusedNameKept, trimThrottled,
                   listenNoStripHint, neverSaysListening, noFadeOffAir, replayOnce, clearedKeepsState, warnShown, topmostFollows,
                   channelLabelFollows, levelsByLane, laneFromEcho, deviceKeyLightsNoChannel, laneAfterChannelMove, twoSpeakersOneDevice,
                   wantedStarting, genNotCompared, disabledRowInert, typingKeepsKeys, ctrl0Azerty, winYue, winNonCode };
  const sick = Object.keys(probes).filter((k) => { try { return !probes[k](runLive()); } catch (e) { return true; } });
  ok("(every probe passes on the real page)", sick.length === 0 && cacheAfterReload(runLive({ ls: CACHE })), sick);
  const R = (a, b) => (s) => s.split(a).join(b);
  caught("the echo ignored: the strips are only what the page edited", R("      strips = echo.map((e) => {", "      if (false) strips = echo.map((e) => {"), echoWins);
  caught("a `live` without `inputs` read as \"no strip\"", R("    const echo = Array.isArray(lv.inputs) ? lv.inputs.map(inputOf).filter(Boolean).slice(0, MAX_STRIPS) : null;",
    "    const echo = (Array.isArray(lv.inputs) ? lv.inputs : []).map(inputOf).filter(Boolean).slice(0, MAX_STRIPS);"), inputsAbsentKeeps);
  caught("no pending window: an echo older than our edit takes the new strip away", R("    pending = { sent: inputs, until: nowMs() + PENDING_MS };", "    pending = null;"), noFlicker);
  caught("the pending window never ends: a refused edit stays on screen", R("      else if (!(nowMs() < pending.until)) pending = null;", ""), refusalShows);
  caught("Listen lights the lamp on the CLICK (the page claims a microphone is open on its own say-so)",
    R("    paintListen();\n    send({ type: \"liveListen\", on: want });", "    onAir = want; wanted = want; paintListen();\n    send({ type: \"liveListen\", on: want });"), lampOnEchoOnly);
  caught("levels painted while not listening", R("    if (!onAir || !wanted) return;            // levels only WHILE LISTENING", "    // levels"), levelsOnlyOnAir);
  caught("no stale timeout: a meter freezes on its last value", R("    staleTimer = setTimeout(() => { staleTimer = null; metersOff(); }, LEVELS_STALE_MS);", ""), metersGoStale);
  caught("the meters kept lit after a stop", R("    if (!onAir || !wanted) metersOff();", "    if (false) metersOff();"), metersOffOnStop);
  caught("the list asked for at load", R("  if (hasWebView) send({ type: \"ready\" });", "  if (hasWebView) { send({ type: \"ready\" }); send({ type: \"liveSources\" }); }"), sourcesOnlyOnOpen);
  caught("the list NOT asked for when the picker opens", R("    send({ type: \"liveSources\" });\n    paintPicker();", "    paintPicker();"), sourcesOnlyOnOpen);
  caught("the rule weakened: a channel allowed next to its own mix",
    R("list.some((s) => s.uid !== exceptUid && s.key === key && (s.chan === chan || s.chan === 0 || chan === 0));", "list.some((s) => s.uid !== exceptUid && s.key === key && s.chan === chan);"), clashRule);
  caught("five strips allowed", R("const MAX_STRIPS = 4;", "const MAX_STRIPS = 5;"), fourMax);
  caught("the name held to 64 CHARACTERS rather than bytes", R("if (utf8Len(name.value) > LABEL_MAX) name.value = fitBytes(name.value, LABEL_MAX);", "if (name.value.length > LABEL_MAX) name.value = name.value.slice(0, LABEL_MAX);"), labelBytes);
  caught("the name sent uncleaned", R("const cleanLabel = (raw) => fitBytes(cleanName(raw, 400), LABEL_MAX).trim();", "const cleanLabel = (raw) => fitBytes(String(raw == null ? \"\" : raw), LABEL_MAX).trim();"), labelClean);
  caught("a device's name shown uncleaned", R("return { kind, key, label: cleanName(x.label, 120) || sourceName(key),", "return { kind, key, label: String(x.label || \"\") || sourceName(key),"), deviceNameClean);
  caught("the preset sent with the mode (more than the changed field)", R("    send({ type: \"translatorCtl\", preset: v });", "    send({ type: \"translatorCtl\", preset: v, mode: trState.mode });"), onlyChanged);
  caught("a preset already in force sent again", R("    if (v === (trState.presetReq || trState.preset)) return;   // nothing changed: nothing travels", ""), presetUnchangedSilent);
  caught("the echo overwrites a name being typed", R("    if (document.activeElement !== n.name && n.name.value !== s.label) n.name.value = s.label;", "    if (n.name.value !== s.label) n.name.value = s.label;"), focusedNameKept);
  caught("every trim movement sent (no throttle)", R("    if (!trimTimer) trimTimer = setTimeout(() => {", "    sendTrim(uid, trimLive.v, false);\n    if (!trimTimer) trimTimer = setTimeout(() => {"), trimThrottled);
  caught("the trim's final value not sent when let go", R("    sendTrim(uid, clampGain(raw), /*final=*/true);", "    trimLive = null;"), trimThrottled);
  caught("Listen with no strip sends nothing", R("    paintListen();\n    send({ type: \"liveListen\", on: want });", "    paintListen();\n    if (strips.length) send({ type: \"liveListen\", on: want });"), listenNoStripHint);
  caught("the window says \"listening\" whenever the ENGINE does", R("    const head = onAir ? (s === \"transcribing\" ? \"transcribing…\" : \"listening\")", "    const head = (onAir || s === \"listening\") ? (s === \"transcribing\" ? \"transcribing…\" : \"listening\")"), neverSaysListening);
  caught("the deck fades while not listening (1.1.0's unconditional fade)", R("  const idleAllowed = () => onAir && wanted && el.picker.hidden && !typing(document.activeElement);", "  const idleAllowed = () => true;"), noFadeOffAir);
  caught("the replay identity dropped (a reload duplicates the log)", R("      if (seen.has(id)) return;           // the reload replay: apply each line at most once", ""), replayOnce);
  caught("the bare {cleared:true} read as a whole state", R("      if (typeof msg.running !== \"boolean\" && !msg.status && !msg.live) return;", ""), clearedKeepsState);
  caught("warn{text} ignored", R("      if (t) showAck(t, true, 6000);", ""), warnShown);
  caught("translatorWindow.topmost ignored (the pin boots off)", R("    if (typeof msg.topmost === \"boolean\") { onTop = msg.topmost; paintPin(); }", ""), topmostFollows);
  caught("a default name does not follow its channel", R("    if (isDefaultName(s)) {", "    if (false) {"), channelLabelFollows);
  judge("the remembered sources not read at start", R("        if (kindOf(k) && Array.isArray(v)) srcInfo.set", "        if (false) srcInfo.set")(script) !== script,
    () => runLive({ mutate: R("        if (kindOf(k) && Array.isArray(v)) srcInfo.set", "        if (false) srcInfo.set"), ls: CACHE }), cacheAfterReload);
  // R1 (TrPipe.h LANES): each way back to "a strip is named by its device's key" must be caught
  caught("levels read by the DEVICE's key (two strips on one device show the same meter)", R("    const id = laneOf(s);", "    const id = s.key;"), levelsByLane);
  caught("a channel strip falls back to its device's key (the old \"first strip of that key\")", R("    const v = own.call(lv, id) ? lv[id] : undefined;", "    const v = own.call(lv, id) ? lv[id] : own.call(lv, s.key) ? lv[s.key] : undefined;"), deviceKeyLightsNoChannel);
  caught("the lane rule loses the channel (the page's lane = the key)", R("  const laneRule = (key, chan) => (chan ? key + \"|\" + chan : key);", "  const laneRule = (key, chan) => key;"), levelsByLane);
  caught("the lane rule's separator drifts from TrPipe.h's ('#' -- the first proposal)", R("  const laneRule = (key, chan) => (chan ? key + \"|\" + chan : key);", "  const laneRule = (key, chan) => (chan ? key + \"#\" + chan : key);"), levelsByLane);
  caught("the echo's lane ignored (the page's own guess wins over the app)", R("  const laneOf = (s) => s.lane || laneRule(s.key, s.chan);", "  const laneOf = (s) => laneRule(s.key, s.chan);"), laneFromEcho);
  caught("a moved channel keeps the echo's OLD lane", R("Object.assign({}, x, { chan, label, lane: \"\" })", "Object.assign({}, x, { chan, label })"), laneAfterChannelMove);
  // R4: live.wanted, and a refusal the app says ONCE
  caught("live.wanted ignored (a start in progress neither shown nor cancellable)", R("    wanted = typeof lv.wanted === \"boolean\" ? lv.wanted : onAir;", "    wanted = onAir;"), wantedStarting);
  caught("the pending window only ends on the NEXT echo (a refused edit stays on screen)", R("    pendingTimer = setTimeout(pendingOver, PENDING_MS);", ""), refusalShows);
  caught("lists ordered by gen (an engine restart's lists ignored forever)", R("    const first = !sources;", "    if (sources && Number(msg.gen) < sources.gen) return;\n    const first = !sources;"), genNotCompared);
  caught("addStrip trusts the button it was called from (a row the list has since greyed still adds)", R("    if (!src || src.state !== \"ok\" || strips.length >= MAX_STRIPS) return;", "    if (!src) return;"), disabledRowInert);
  caught("a source that cannot be captured offered as addable (its state ignored)", R("    if (src.state !== \"ok\") {\n      const why", "    if (false) {\n      const why"), disabledRowInert);
  caught("a name being typed loses its keys to the dialogue's shortcuts", R("    if (typing(e.target)) return;", ""), typingKeepsKeys);
  caught("Ctrl+0 by the character again (AZERTY's \"à\" missed)", R("(e.code === \"Digit0\" || e.code === \"Numpad0\" || e.key === \"0\")", "(e.key === \"0\")"), ctrl0Azerty);
  caught("the two-letter prefix fallback is back", R('return /^[a-z]{2}$/.test(s) ? s : "";', "return s.slice(0, 2);"), winNonCode);
  caught("the yue row dropped from the aliases", R('"cmn": "zh", "yue": "yue",', '"cmn": "zh",'), winYue);
  // 1.1.0's W3 / typewriter guards
  const cadence = (x) => {
    const g = (n) => Array.from({ length: n }, (_, i) => "w" + (i + 1)).join(" ");
    x.send({ type: "translatorPartial", segId: "c2", t: 1, lang: "en", src: g(5) }); x.pumpTimers(1500);
    x.send({ type: "translatorPartial", segId: "c2", t: 2, lang: "en", src: g(10) }); x.pumpTimers(1500);
    x.send({ type: "translatorPartial", segId: "c2", t: 3, lang: "en", src: g(15) }); x.pumpTimers(1500 + 1050);
    x.send({ type: "translatorPartial", segId: "c3", t: 4, lang: "en", src: g(5) }); x.pumpTimers(500);
    const s = x.ids.log.querySelector(".tr-live-src").textContent;
    return s !== g(5) && s.length > 0;
  };
  caught("twStop resets the cadence at every sentence (W2's bug)", R("    tw.at = NaN;\n    tw.fields.forEach", "    tw.ema = TW_SEED_MS; tw.at = NaN;\n    tw.fields.forEach"), cadence);
  const closesDefault = (x) => {
    x.send({ type: "translatorPartial", segId: 5, t: 1, lang: "en", src: "Old line." });
    x.send({ type: "translatorLine", t: 2, segId: 5, lang: "en", src: "Old line.", dst: "x", spk: SPK_A });
    return !x.ids.log.querySelector(".tr-live");
  };
  caught("a line with no `closes` read as closes:false", R("const closesOf = (l) => l.closes !== false;", "const closesOf = (l) => l.closes === true;"), closesDefault);
  const headStrict = (x) => {
    x.send({ type: "translatorPartial", segId: 6, t: 1, lang: "en", src: "Something else entirely" });
    x.pumpTimers(1000);
    x.send({ type: "translatorLine", t: 2, segId: 6, lang: "en", src: "Never on that line.", dst: "x", seq: 1, closes: false, spk: SPK_A });
    return x.ids.log.querySelector(".tr-live-src").textContent === "Something else entirely";
  };
  caught("a committed head that is NOT the head is cut anyway", R("    if (cut < 0) return;      // not our text", "    if (cut < 0) { live = { ...live, src: \"\" }; livePaint(live, true); return; }      // not our text"), headStrict);
  const seg0 = (x) => {
    x.send({ type: "translatorPartial", segId: 0, t: 1, lang: "en", src: "Segment zero." });
    return !x.ids.log.querySelector(".tr-live");
  };
  caught("segId 0 read as an identity", R("const segIdOf = (v) => (v == null || Number(v) === 0 ? \"\" : String(v));", "const segIdOf = (v) => (v == null ? \"\" : String(v));"), seg0);
  const endOther = (x) => {
    x.send({ type: "translatorPartial", segId: 7, t: 1, lang: "en", src: "Still talking" });
    x.send({ type: "translatorEnd", segId: 8 });
    return !!x.ids.log.querySelector(".tr-live");
  };
  caught("an END for another segment fells the live line", R("    if (!segId || !live || live.segId !== segId) return;", "    if (!segId || !live) return;"), endOther);
  // the security rule's own sentinels are in the SECURITY section (the four sinks put back)
}

// =====================================================================================================================
// FIX ROUND (2026-10-26, docs/REVIEW_RV-LB.md + LIVE_PLAN.md "ADDENDUM FIX ROUND") -- the page's half: the scrollback
// bounded by ROWS (RVB-2), the DRAIN (LIVE 0 = stop listening, then finish), a refused LIVE shown (RVB-5), the faded
// deck's first key (RVB-14), the picker's close said (RVB-7: in the CONTRACT section's probe), a '|' key refused (RVA-10).
// Each probe is a check here AND the condition its mutants must see fail.
section("FIX ROUND -- rows not paragraphs (RVB-2), the drain, a refusal shown (RVB-5), the faded deck's key (RVB-14), '|' (RVA-10)");
{
  const OPEN2 = TWO.map((i) => Object.assign({ state: "open" }, i));
  const lineOf = (k, spk) => ({ type: "translatorLine", t: 200000 + k, segId: 5000 + k, lang: "en", src: "Line " + k + ".",
                                dst: "L" + k, dstLang: "zh", spk: spk || SPK_A });
  const rowsOf = (x) => x.ids.log.querySelectorAll(".tr-line");
  const mainOf = (r) => { const d = r && r.querySelector(".tr-dst"); return d ? d.textContent : null; };
  // RVB-2: ONE speaker (one paragraph), 620 lines -> 500 rows (the 500 `lines` keeps), the paragraph's head still named;
  // a Show A / Both repaint shows the very same 500 (no sudden cut)
  const rowsCapped = (x) => {
    x.send(liveSt(true, OPEN2));
    for (let k = 0; k < 620; k++) x.send(lineOf(k));
    const paras = x.ids.log.querySelectorAll(".tr-para"), r = rowsOf(x);
    const head = paras[0] && paras[0].firstElementChild;
    const named = !!head && !!head.querySelector(".tr-who") && x.ids.log.querySelectorAll(".tr-who").length === 1
      && head.querySelector(".tr-who-name").textContent === "Me";
    const span = [mainOf(r[0]), mainOf(r[r.length - 1])];
    x.ids.showA.click();
    const afterA = rowsOf(x).length;
    x.ids.showBoth.click();
    const r2 = rowsOf(x);
    return paras.length === 1 && r.length === 500 && named && J(span) === J(["L120", "L619"]) && afterA === 500
      && r2.length === 500 && mainOf(r2[0]) === "L120";
  };
  // ...and two speakers taking turns: 500 rows too (each paragraph one row), the oldest paragraphs gone whole
  const rowsCappedTurns = (x) => {
    x.send(liveSt(true, OPEN2));
    for (let k = 0; k < 600; k++) x.send(lineOf(k, k % 2 ? SPK_B : SPK_A));
    const paras = x.ids.log.querySelectorAll(".tr-para");
    return rowsOf(x).length === 500 && paras.length === 500 && paras.every((p) => p.querySelectorAll(".tr-who").length === 1);
  };
  ok("RVB-2: ONE speaker's 620 lines (one paragraph) keep 500 ROWS in the page -- the oldest go, the paragraph keeps its name; Show A / Both repaints the same 500",
     rowsCapped(runLive()));
  ok("...two speakers taking turns: 500 rows, 500 named paragraphs (the old 240-paragraph cap is gone)", rowsCappedTurns(runLive()));

  // THE DRAIN: Listen off -> live {on:true, wanted:false} until the engine is done -> live {on:false}
  const drainShown = (x) => {
    x.send(st({ live: { on: true, wanted: true, inputs: OPEN2 } }));
    x.send({ type: "liveLevels", levels: { [L1]: -6 } });
    const listening = /^listening/.test(stateLine(x)) && x.ids.onAir.classList.contains("is-lit") && lvl(x, 0) === "0.900";
    x.send(st({ live: { on: true, wanted: false, inputs: TWO } }));
    x.send({ type: "liveLevels", levels: { [L1]: -6 } });
    const fin = stateLine(x) === "Finishing the last sentences…" && x.ids.state.classList.contains("is-loading")
      && x.ids.onAir.classList.contains("is-finishing") && !x.ids.onAir.classList.contains("is-lit")
      && x.ids.listenBtn.getAttribute("aria-pressed") === "false" && x.ids.listenBtn.classList.contains("is-pending")
      && !x.doc.body.classList.contains("is-on-air") && lvl(x, 0) === "0.000"
      && /finishing the last sentences/.test(x.ids.listenBtn.title);
    x.send(lineOf(1));                                                    // the drained sentences still arrive
    const lineIn = rowsOf(x).length === 1;
    x.pumpTimers(5000);
    const noFade = !x.ids.deck.classList.contains("is-idle");             // ...and the deck stays to say so
    x.send(st({ live: { on: false, wanted: false, inputs: TWO } }));
    const done = /^not listening/.test(stateLine(x)) && !x.ids.onAir.classList.contains("is-finishing")
      && !x.ids.onAir.classList.contains("is-lit") && !x.ids.listenBtn.classList.contains("is-pending");
    return listening && fin && lineIn && noFade && done;
  };
  // ...a press while it finishes = listen AGAIN (the engine resumes: LIVE 1 during the drain)
  const drainResume = (x) => {
    x.send(st({ live: { on: true, wanted: false, inputs: OPEN2 } }));
    x.outbox.length = 0;
    x.ids.listenBtn.click();
    return J(x.outbox) === J([{ type: "liveListen", on: true }]);
  };
  ok("THE DRAIN: Listen off while the engine still finishes (live.on && !live.wanted) -- \"Finishing the last sentences…\", the ON AIR lamp blinks on its way off, Listen is up, no meter, the lines still come, the deck does not fade; live.on false -> not listening",
     drainShown(runLive()));
  ok("...a press of Listen while it finishes asks to listen AGAIN (liveListen{on:true}: the engine resumes)", drainResume(runLive()));
  ok("...the blinking lamp is CSS (pulse), steady and dimmed under reduced motion",
     /\.on-air\.is-finishing \{[^}]*animation: pulse/.test(html) && /\.on-air\.is-finishing \{ animation: none; opacity: 0\.55; \}/.test(html));

  // RVB-5: the app wants to listen, the engine said why it will not -> said, never "starting to listen…" for ever
  const refusedSaid = (x) => {
    x.send(st({ live: { on: false, wanted: true, inputs: TWO }, err: "Test inputs (wav:) are for a test engine only." }));
    return stateLine(x) === "could not listen — Test inputs (wav:) are for a test engine only." && x.ids.state.classList.contains("is-error");
  };
  ok("RVB-5: wanted and not listening WITH the engine's err -> the state line says why (red), not \"starting to listen…\"", refusedSaid(runLive()));

  // RVB-14: the deck faded with the keyboard on Listen -> the first key only wakes it (Space / Enter do not stop listening)
  const fadedKey = (x) => {
    x.send(liveSt(true, OPEN2));
    x.ids.listenBtn.focus();
    x.pumpTimers(3100);
    const faded = x.ids.deck.classList.contains("is-idle");
    x.outbox.length = 0;
    const e1 = x.win.dispatch("keydown", { key: " ", code: "Space", target: x.ids.listenBtn });
    const woke = !x.ids.deck.classList.contains("is-idle");
    const e2 = x.win.dispatch("keydown", { key: " ", code: "Space", target: x.ids.listenBtn });
    return faded && e1.defaultPrevented === true && woke && e2.defaultPrevented === false && x.outbox.length === 0;
  };
  ok("RVB-14: the deck faded with the keyboard on Listen -- the first key only WAKES it (prevented: the invisible control does not act); the next one is the control's",
     fadedKey(runLive()));

  // RVA-10: '|' is the lane separator -- a key holding one is no source and no strip
  const pipeKeys = (x) => {
    x.send(liveSt(false, [inp(UR, "Me", 1, 0), inp(MIC + "|1", "Pipe", 0, 0)]));
    const shown = stripsOf(x).length === 1 && strip(x, 0).name.value === "Me";
    openPick(x, [LIST[0], Object.assign({}, LIST[1], { key: MIC + "|2", label: "Piped" })]);
    return shown && pickRows(x).length === 1 && !pickRow(x, MIC + "|2");
  };
  ok("RVA-10: a key holding '|' (the lane separator) is no strip in the echo and no row of the picker", pipeKeys(runLive()));

  // their sentinels
  const judgeF = (name, mutate, probe) => {
    const m = mutate(script);
    if (m === script) return ok("MUTANT (fix round) \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    let page;
    try { page = runLive({ mutate, name }); } catch (e) { return ok("MUTANT (fix round) \"" + name + "\" is caught by its check", false, "does not load: " + e.message); }
    let healthy = true;
    try { healthy = !!probe(page); } catch (e) { return ok("MUTANT (fix round) \"" + name + "\" is caught by its check", false, "the probe THREW: " + e.message); }
    ok("MUTANT (fix round) \"" + name + "\" is caught by its check", !healthy);
  };
  const R = (a, b) => (s) => s.split(a).join(b);
  judgeF("the scrollback capped by PARAGRAPHS again (one speaker grows the page without limit)", R("    while (rows > MAX) {", "    while (false) {"), rowsCapped);
  judgeF("a trimmed paragraph loses its speaker's name", R("else if (who && !next.querySelector(\".tr-who\")) next.insertBefore(who, next.firstElementChild);", "else if (false) next.insertBefore(who, next.firstElementChild);"), rowsCapped);
  judgeF("the row count not reset by a repaint (Show A cuts the scrollback)", R("    el.log.replaceChildren();\n    rows = 0;\n    lines.forEach(appendLine);", "    el.log.replaceChildren();\n    lines.forEach(appendLine);"), rowsCapped);
  judgeF("the drain not said (the window reads \"listening\" with the microphones closed)", R("    if (onAir && !wanted) return { text: \"Finishing the last sentences…\", cls: \"loading\" };", ""), drainShown);
  judgeF("the ON AIR lamp stays lit through the drain", R("el.onAir.classList.toggle(\"is-lit\", onAir && !finishing);", "el.onAir.classList.toggle(\"is-lit\", onAir);"), drainShown);
  judgeF("the meters keep moving through the drain", R("    if (!onAir || !wanted) return;            // levels only", "    if (!onAir) return;            // levels only"), drainShown);
  judgeF("the deck fades while the window says it is finishing", R("const idleAllowed = () => onAir && wanted && el.picker.hidden", "const idleAllowed = () => onAir && el.picker.hidden"), drainShown);
  judgeF("a refusal's err ignored (\"starting to listen…\" for ever)", R("    if (!onAir && wanted) return trState.err ?", "    if (!onAir && wanted) return false ?"), refusedSaid);
  judgeF("the faded deck's first key acts on the invisible control", R("    if (asleep && inside(e.target, el.deck)) { e.preventDefault(); return; }", ""), fadedKey);
  judgeF("a key with '|' taken as a source / a strip", R("return m && s.indexOf(\"|\") < 0 ? m[1] : \"\";", "return m ? m[1] : \"\";"), pipeKeys);
  judgeF("the picker's close not said (a respawned engine asked for lists for ever)", R("    send({ type: \"liveSources\", open: false });\n", ""), sourcesOnlyOnOpen);
}

// =====================================================================================================================
// THE PLAYER'S HALF (index.html: the Live button + the notice, LIVE_PLAN 3.3's last paragraph). index.html is the
// player's page: these checks run it through ITS runner (run_ui.js) -- the player's own harness is test_ui.js.
section("THE PLAYER'S HALF (index.html) -- the Live button (openLive{}) and the notice (translatorState.live.on)");
{
  const { runTranslatorWindow } = require("./run_ui.js");
  const idx = P.readPage();
  const idxScript = (idx.match(/<script>([\s\S]*?)<\/script>/) || ["", ""])[1];
  const NOTICE = "Live translation is on — this film's subtitles will be made when it stops.";
  const pst = (o) => Object.assign({ type: "translatorState", running: true, status: "listening", preset: "balanced", mode: "auto",
    langA: "es", langB: "fr", gpu: true, models: MODELS }, o);
  // the probes (checks here AND the sentinels' conditions)
  const liveBtnOpens = (x) => {
    x.outbox.length = 0;
    x.ids.liveBtn.click();
    return J(x.outbox) === J([{ type: "openLive" }]);
  };
  // The Live button's dot follows live.on (the engine's echo); a state that does not report the live path leaves it; the
  // notice (fix round, RVB-10) is about a FILM held -- translatorFile.phase "live" -- and nothing else: its "Open Live"
  // sends openLive{}, the next phase takes it away.
  const noticeFollows = (x) => {
    const off0 = x.ids.liveNotice.hidden === true;
    x.send(pst({ live: { on: true, inputs: [inp(UR, "Me", 1, 0, "open")] } }));
    const dot = x.ids.liveBtn.classList.contains("is-on-air") && x.ids.liveNotice.hidden === true;   // no film waits: no notice
    x.send({ type: "translatorFile", jobId: 1, seq: 1, phase: "live", itemId: 1, name: "film.mkv", totalMs: 60000, doneMs: 24000 });
    const on = x.ids.liveNotice.hidden === false && x.ids.liveBtn.classList.contains("is-on-air");   // (a translatorFile moves no dot)
    x.send(pst({}));                                                    // a state that does not report the live path
    const kept = x.ids.liveNotice.hidden === false && x.ids.liveBtn.classList.contains("is-on-air");
    x.outbox.length = 0;
    x.ids.liveNoticeOpen.click();
    const opened = J(x.outbox) === J([{ type: "openLive" }]);
    x.send(pst({ live: { on: false, inputs: [] } }));
    const dotOff = !x.ids.liveBtn.classList.contains("is-on-air");
    x.send({ type: "translatorFile", jobId: 1, seq: 2, phase: "analyzing", itemId: 1, name: "film.mkv", totalMs: 60000, doneMs: 24000 });
    return off0 && dot && on && kept && opened && dotOff && x.ids.liveNotice.hidden === true;
  };
  // L2's R3: the film's analysis WAITS for Live -- translatorFile.phase "live": the notice + its "Open Live"; the dot only on live.on
  const noticeOnLivePhase = (x) => {
    x.send(pst({ live: { on: false, wanted: false, inputs: [] } }));
    x.send({ type: "translatorFile", jobId: 1, seq: 1, phase: "live", itemId: 1, name: "film.mkv", totalMs: 60000 });
    const shown = x.ids.liveNotice.hidden === false && !x.ids.liveBtn.classList.contains("is-on-air");
    x.send({ type: "translatorFile", jobId: 1, seq: 2, phase: "analyzing", itemId: 1, name: "film.mkv", totalMs: 60000 });
    return shown && x.ids.liveNotice.hidden === true;
  };
  // RVB-10: listening (or about to) with NO film, or with a COMPLETE film -- the notice's "this film's subtitles will be
  // made when it stops" would be untrue: hidden (the dot says Live listens)
  const noticeNotWithoutFilm = (x) => {
    x.send(pst({ live: { on: false, wanted: true, inputs: [inp(UR, "Me", 1, 0)] } }));
    const wantedNoFilm = x.ids.liveNotice.hidden === true;
    x.send(pst({ live: { on: true, wanted: true, inputs: [inp(UR, "Me", 1, 0, "open")] } }));
    const onNoFilm = x.ids.liveNotice.hidden === true && x.ids.liveBtn.classList.contains("is-on-air");
    x.send({ type: "translatorFile", jobId: 2, seq: 1, phase: "done", itemId: 1, name: "film.mkv", totalMs: 60000, doneMs: 60000, complete: true });
    return wantedNoFilm && onNoFilm && x.ids.liveNotice.hidden === true;
  };
  // RVB-9: the player's strip while Live holds the analysis -- said (how far it got, that it resumes), never blank, and
  // nothing to start (clicking it would start nothing: the gate holds every FILE). U1 (02/11): the strip's start is ONE slot
  // -- Translate / Cancel / Re-analyse -- and a file held for Live is a file ASKED for: its slot is Cancel (the wish dropped:
  // it will not start when Live stops), never Translate nor Re-analyse.
  const stripOnLive = (x) => {
    x.send(pst({ live: { on: true, wanted: true, inputs: [inp(UR, "Me", 1, 0, "open")] } }));
    x.send({ type: "translatorFile", jobId: 3, seq: 1, phase: "live", itemId: 1, name: "film.mkv", totalMs: 100000, doneMs: 40000, translate: true });
    return x.ids.fileStatus.textContent === "Waiting for Live — 40% analysed · resumes when Live stops listening"
      && x.ids.fileReanalyse.hidden === true && x.ids.fileTranslate.hidden === true && x.ids.fileCancel.hidden === false;
  };
  ok("the bar's Window plate carries the Live button -- named \"Translate Live\" (U1, the user: so it says what it does); no new plate (U1: the pair left the bar)",
     /<span class="lbl">Window<\/span>\s*<span class="grp-row">\s*<!--[^>]*-->\s*<button id="liveBtn" type="button"[^>]*>Translate Live<\/button>/.test(idx)
     && [...idx.matchAll(/<span class="lbl">([^<]+)<\/span>/g)].map((m) => m[1]).join("|") === "Window");
  ok("...a click sends openLive{} and nothing else -- the app opens or shows the Live window", liveBtnOpens(runTranslatorWindow()));
  const x = runTranslatorWindow();
  ok("the notice is there, hidden, at the top of the player's pane, its words the contract's",
     x.ids.liveNotice.hidden === true && x.ids.liveNotice.children[0].textContent === NOTICE && x.ids.liveNoticeOpen.textContent === "Open Translate Live"
     && idx.indexOf('id="liveNotice"') > idx.indexOf('<section id="filePane">') && idx.indexOf('id="liveNotice"') < idx.indexOf('id="fileEmpty"'));
  ok("translatorState.live.on lights the Live button's dot; a state that does not report the live path leaves it; the notice comes with a FILM held (phase \"live\"), its \"Open Live\" sends openLive{}; the next phase hides it",
     noticeFollows(runTranslatorWindow()));
  ok("R3: translatorFile.phase \"live\" shows the notice (the film's analysis waits for Live; the film plays with what it has) -- and the next phase takes it away",
     noticeOnLivePhase(runTranslatorWindow()));
  ok("RVB-10: listening -- or about to (live.wanted) -- with NO film, or with a COMPLETE one, shows NO notice (its words would be untrue); the dot says Live listens",
     noticeNotWithoutFilm(runTranslatorWindow()));
  ok("RVB-9: while Live holds the analysis the status strip SAYS so (\"Waiting for Live — 40% analysed · resumes when Live stops listening\"), nothing to start -- U1: its slot is Cancel (the wish dropped), no Translate, no Re-analyse",
     stripOnLive(runTranslatorWindow()));
  const y = runTranslatorWindow();
  y.send(pst({ mode: "a2b", live: { on: true, inputs: [inp(UR, "Me", 1, 0, "open")] } }));
  ok("this page's pair stays the PLAYER's (es/fr here), whatever the Live window runs", y.ids.langA.value === "es" && y.ids.langB.value === "fr" && y.ids.dirBtn.textContent === "→");
  ok("...and the notice is hidden in full screen (CSS), its [hidden] guard present, not selectable",
     /body\.is-full \.live-notice \{ display: none; \}/.test(idx) && /\.live-notice\[hidden\] \{ display: none; \}/.test(idx)
     && /\.live-notice \{[^}]*user-select: none;/.test(idx));
  ok("the player's addition is ONE delimited block in each place (CSS, the button, the notice, the script) + the fix round's strip line (RVB-9), and nothing of the Live window comes with it",
     (idx.match(/SUVTITLE LIVE \(1\.1\.0, L3\)/g) || []).length === 4 && (idx.match(/\(end SUVTITLE LIVE\)/g) || []).length === 3
     && (idx.match(/SUVTITLE LIVE \(1\.1\.0 fix round, RVB-9\)/g) || []).length === 1 && (idx.match(/SUVTITLE LIVE \(fix round, RVB-9\)/g) || []).length === 1
     && !/liveSources|liveConfig|liveListen|liveLevels|translatorLine|tr-live/.test(idx));
  ok("...its new code builds no markup and names no sink (THE SECURITY RULE, as test_ui.js holds the whole page to)",
     !MARKUP_SINK.test(codeOnly((/\/\* ---- SUVTITLE LIVE \(1\.1\.0, L3\): the door[\s\S]*?\(end SUVTITLE LIVE\)/.exec(idxScript) || [""])[0])));
  // sentinels
  const judgeP = (name, mutate, probe) => {
    const m = mutate(idxScript);
    if (m === idxScript) return ok("MUTANT (player) \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    let page; try { page = runTranslatorWindow({ mutate, name }); } catch (e) { return ok("MUTANT (player) \"" + name + "\" is caught by its check", false, "does not load: " + e.message); }
    let healthy = true; try { healthy = !!probe(page); } catch (e) { return ok("MUTANT (player) \"" + name + "\" is caught by its check", false, "the probe THREW: " + e.message); }
    ok("MUTANT (player) \"" + name + "\" is caught by its check", !healthy);
  };
  const R = (a, b) => (s) => s.split(a).join(b);
  judgeP("the Live button's dot ignores the live path", R("      paintLiveDoor(msg.live);", "      // paintLiveDoor"), noticeFollows);
  judgeP("a state without `live` puts the dot out", R("    if (lv && typeof lv === \"object\" && typeof lv.on === \"boolean\") {          // not reported: as it was", "    { lv = lv && typeof lv === \"object\" ? lv : {};"), noticeFollows);
  judgeP("translatorFile.phase \"live\" ignored (R3)", R("      paintLiveDoor();                    // SUVTITLE LIVE (1.1.0)", "      //"), noticeOnLivePhase);
  judgeP("the notice back on live.on / live.wanted whatever the film (RVB-10)", R("    liveDoor.notice.hidden = F.phase !== \"live\";", "    liveDoor.notice.hidden = !(liveDoor.on || liveDoor.wanted || F.phase === \"live\");"), noticeNotWithoutFilm);
  judgeP("the strip blank while Live holds the analysis (RVB-9)", R("    } else if (F.phase === \"live\") {", "    } else if (false) {"), stripOnLive);
  // (U3 re-point: the slot's line now also asks Translate's own wish -- the mutant still takes the live hold out)
  judgeP("Translate offered while Live holds the analysis (RVB-9; U1: the slot)", R("    const run = (fileRunning() || p === \"live\") && F.translate;", "    const run = fileRunning() && F.translate;"), stripOnLive);
  judgeP("the Live button sends nothing", R("  liveDoor.btn.addEventListener(\"click\", () => send({ type: \"openLive\" }));", ""), liveBtnOpens);
  judgeP("the notice's link sends nothing", R("  liveDoor.open.addEventListener(\"click\", () => send({ type: \"openLive\" }));", ""), noticeFollows);
}

// U1 (02/11/2026, the user's requests -- docs\PROGRESS_U1.md): the Live window's half -- its lockup (what SuVtitle does to
// the right of the name, its maker under it) and its language names ("日本語 (Japanese)" in the selects and the tooltips; the
// Show segments keep the bare name: a one-row deck). The player's half is test_u1.js (test_ui.js).
section("U1 -- the Live window: the lockup (legend + maker) and the language names");
{
  const lockupLive = (h) => /<div class="brand"[^>]*>\s*<span class="brand-line">\s*<span class="brand-name"><span class="brand-txt">Su<\/span><span class="brand-accent">V<\/span><span class="brand-txt">title<\/span><\/span>\s*<span class="brand-sub">Live<\/span>\s*<span class="brand-tag">Transcription, Translation &amp; Subtitles<\/span>\s*<\/span>\s*<span class="brand-by">by NEXTALE Audio<\/span>\s*<\/div>/.test(h)
    && /\.brand-tag \{\s*font-family: var\(--display\);/.test(h) && /\.brand-by \{[^}]*color: var\(--text-faint\);/.test(h)
    && /@media \(max-width: 900px\), \(max-height: 560px\) \{[\s\S]{0,500}\.brand-tag \{ display: none; \}/.test(h);
  const q = (x, s) => (x.doc.querySelector(s) || {}).textContent;
  ok("the lockup: SuVtitle (Bahnschrift) + Live, its legend to the right after a hairline, its maker small and faint under it; the narrow deck keeps the name, Live and the maker (the legend is the tooltip)",
     lockupLive(html) && (() => { const x = runLive(); return q(x, ".brand-tag") === "Transcription, Translation & Subtitles" && q(x, ".brand-by") === "by NEXTALE Audio"
       && /^SuVtitle Live — Transcription, Translation & Subtitles, by NEXTALE Audio$/.test(x.doc.querySelector(".brand").getAttribute("title")); })());
  const namesLive = (x) => {
    x.send(st({ langA: "ja", langB: "en" }));
    return x.ids.langA.title === "日本語 (Japanese)" && x.ids.showA.textContent === "日本語" && x.ids.showB.textContent === "English"
      && x.ids.showA.title === "Show only 日本語 (Japanese) — one line per sentence" && /日本語 \(Japanese\)/.test(x.ids.dirBtn.title);
  };
  ok("the names: the selects' tooltip and the direction say \"日本語 (Japanese)\"; the Show segments the bare name, their tooltip the whole one",
     namesLive(runLive()));
  // sentinels
  const judgeL = (name, mutate, probe) => {
    if (mutate(script) === script) return ok("MUTANT (U1) \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    let x; try { x = runLive({ mutate, name }); } catch (e) { return ok("MUTANT (U1) \"" + name + "\" is caught by its check", false, "does not load: " + e.message); }
    let healthy = true; try { healthy = !!probe(x); } catch (e) { return ok("MUTANT (U1) \"" + name + "\" is caught by its check", false, "the probe THREW: " + e.message); }
    ok("MUTANT (U1) \"" + name + "\" is caught by its check", !healthy);
  };
  const judgeH = (name, mutateHtml, check) => {
    const m = mutateHtml(html);
    if (m === html) return ok("MUTANT (U1) \"" + name + "\" is caught by its check", false, "the mutation did not apply");
    ok("MUTANT (U1) \"" + name + "\" is caught by its check", !check(m));
  };
  const R = (a, b) => (s) => s.split(a).join(b);
  judgeL("the Show segments glossed (the one-row deck wraps)", R("    const lab = { both: \"Both\", a: langNative(p.a), b: langNative(p.b) };", "    const lab = { both: \"Both\", a: langName(p.a), b: langName(p.b) };"), namesLive);
  judgeL("the Show tooltip bare", R("        : \"Show only \" + full[v] + \" — one line per sentence\";", "        : \"Show only \" + lab[v] + \" — one line per sentence\";"), namesLive);
  judgeL("no English name after a non-Latin one", R("    return hit && hit[2] ? hit[1] + \" (\" + hit[2] + \")\" : langNative(c);", "    return langNative(c);"), namesLive);
  judgeH("the legend gone", R("<span class=\"brand-tag\">Transcription, Translation &amp; Subtitles</span>", ""), lockupLive);
  judgeH("the maker gone", R("<span class=\"brand-by\">by NEXTALE Audio</span>", ""), lockupLive);
  judgeH("the legend kept on the narrow deck (it wraps the deck onto a second row)", R("      .brand-tag { display: none; }\n", ""), lockupLive);
}

console.log("\n" + (fail === 0 ? "ALL " + pass + " CHECKS PASSED" : pass + " passed, " + fail + " FAILED"));
process.exit(fail === 0 ? 0 : 1);

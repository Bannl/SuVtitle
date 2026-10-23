// Minimal DOM good enough to run ui/web/app.js headlessly (no jsdom available).
// Only what app.js touches: ids, classes, template cloning, listeners, SVG creation.

class ClassList {
  constructor(el) { this.el = el; this.set = new Set(); }
  add(...c)    { c.forEach(x => x && this.set.add(x)); }
  remove(...c) { c.forEach(x => this.set.delete(x)); }
  contains(c)  { return this.set.has(c); }
  toggle(c, force) {
    const on = (force === undefined) ? !this.set.has(c) : !!force;
    if (on) this.set.add(c); else this.set.delete(c);
    return on;
  }
  get value() { return [...this.set].join(" "); }
}

let AUTO_ID = 0;

class El {
  constructor(tag, ns) {
    this.tagName = String(tag).toUpperCase();
    this.ns = ns || null;
    this.children = [];
    this.parentNode = null;
    this.attrs = {};
    // Plain property writes (style.background = ...) AND the CSS-variable API coexist here:
    // setProperty stores on the same object, so tests read either form uniformly.
    this.style = {
      setProperty(k, v) { this[k] = String(v); },
      removeProperty(k) { delete this[k]; },
      getPropertyValue(k) { return this[k] || ""; },
    };
    this.dataset = {};
    this.classList = new ClassList(this);
    this._text = "";
    this.hidden = false;
    this.disabled = false;
    this.value = "";
    this.type = "";
    this.listeners = {};
    this._uid = ++AUTO_ID;
  }
  // ---- attributes ----
  setAttribute(k, v) {
    this.attrs[k] = String(v);
    if (k === "class") { this.classList.set = new Set(String(v).split(/\s+/).filter(Boolean)); }
    if (k === "id") this.id = String(v);
  }
  getAttribute(k) { return k === "class" ? this.classList.value : (this.attrs[k] ?? null); }
  removeAttribute(k) { delete this.attrs[k]; }
  get className() { return this.classList.value; }
  set className(v) { this.classList.set = new Set(String(v).split(/\s+/).filter(Boolean)); }

  // ---- text ----
  get textContent() {
    if (this.children.length === 0) return this._text;
    return this.children.map(c => c.textContent).join("");
  }
  set textContent(v) { this._text = String(v); this.children = []; }

  set innerHTML(v) { if (v === "") { this.children = []; this._text = ""; } }
  get outerHTML() { return `<${this.tagName.toLowerCase()}>`; }

  // ---- tree ----
  appendChild(c) {
    if (c.parentNode) c.remove();               // moving a node detaches it first (DOM semantics)
    c.parentNode = this; this.children.push(c);
    return c;
  }
  insertBefore(c, ref) {
    if (c.parentNode) c.remove();
    c.parentNode = this;
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i < 0) this.children.push(c); else this.children.splice(i, 0, c);
    return c;
  }
  get nextSibling() {
    if (!this.parentNode) return null;
    const i = this.parentNode.children.indexOf(this);
    return (i >= 0 && i + 1 < this.parentNode.children.length) ? this.parentNode.children[i + 1] : null;
  }
  replaceChildren(...c) {
    this.children.forEach(ch => { ch.parentNode = null; });
    this.children = [];
    c.forEach(x => this.appendChild(x));
  }
  remove() {
    if (!this.parentNode) return;
    const i = this.parentNode.children.indexOf(this);
    if (i >= 0) this.parentNode.children.splice(i, 1);
    this.parentNode = null;
  }
  get firstElementChild() { return this.children[0] || null; }

  cloneNode() {
    const c = new El(this.tagName, this.ns);
    c.attrs = { ...this.attrs };
    c.classList.set = new Set(this.classList.set);
    c._text = this._text;
    c.id = this.id;
    this.children.forEach(ch => c.appendChild(ch.cloneNode(true)));
    return c;
  }

  // ---- selectors (supports the tiny subset app.js uses) ----
  _matches(sel) {
    if (sel.startsWith(".")) {
      // ".a.b" or ".a" — every class must be present
      return sel.slice(1).split(".").every(c => this.classList.contains(c));
    }
    if (sel.startsWith("#")) return this.id === sel.slice(1);
    // '.mtx-cell[data-i="0"][data-j="1"]'
    const m = sel.match(/^([.#][\w-]+)((\[[^\]]+\])*)$/);
    if (m) {
      if (!this._matches(m[1])) return false;
      const attrs = m[2].match(/\[[^\]]+\]/g) || [];
      return attrs.every(a => {
        const kv = a.slice(1, -1).match(/^([\w-]+)="([^"]*)"$/);
        if (!kv) return false;
        const key = kv[1].startsWith("data-") ? kv[1].slice(5) : kv[1];
        const src = kv[1].startsWith("data-") ? this.dataset : this.attrs;
        return String(src[key]) === kv[2];
      });
    }
    return this.tagName === sel.toUpperCase();
  }
  _walk(out) { this.children.forEach(c => { out.push(c); c._walk(out); }); return out; }
  querySelectorAll(sel) {
    const parts = sel.split(",").map(s => s.trim());
    return this._walk([]).filter(e => parts.some(p => {
      const desc = p.split(/\s+/);           // ".node.drop-ok" vs ".a .b"
      return desc.length === 1 ? e._matches(desc[0])
                               : e._matches(desc[desc.length - 1]);
    }));
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  closest(sel) { let n = this; while (n) { if (n._matches && n._matches(sel)) return n; n = n.parentNode; } return null; }

  // ---- events ----
  addEventListener(t, fn) { (this.listeners[t] ||= []).push(fn); }
  removeEventListener(t, fn) {
    if (this.listeners[t]) this.listeners[t] = this.listeners[t].filter(f => f !== fn);
  }
  dispatch(t, ev = {}) {
    const e = { type: t, target: this, preventDefault() {}, stopPropagation() {}, ...ev };
    (this.listeners[t] || []).forEach(fn => fn(e));
  }
  click() { this.dispatch("click"); }
  // FOCUS (WG review #1): document.activeElement follows focus()/blur(), and moving the focus fires the old
  // element's "blur" -- the editor's fields commit on blur and keep what the user typed while focused, which a
  // test must be able to reproduce. One focus for the whole stub (tests drive one page at a time); a document
  // whose active element belongs to another document simply never matches its own fields.
  focus() {
    const prev = El._active;
    if (prev === this) return;
    El._active = this;
    if (prev && prev.dispatch) prev.dispatch("blur");
  }
  blur() {
    if (El._active !== this) return;
    El._active = null;
    this.dispatch("blur");
  }

  // No layout engine here, so everything reports the same box by default. A test that needs
  // real geometry (e.g. "which END of the cable did the pointer grab?") sets _rect on the
  // elements it cares about -- see setRect() in test_ui.js.
  getBoundingClientRect() {
    return this._rect || { left: 0, top: 0, width: 800, height: 600, right: 800, bottom: 600 };
  }

  // <canvas>: no rasteriser here, but app.js must be able to CALL the 2d API (the FX scope and
  // the EQ curve paint on every spectrum frame). Every method is a no-op recorder, so a drawing
  // bug shows up as a thrown TypeError in the test run rather than passing silently.
  getContext(kind) {
    if (kind !== "2d") return null;
    if (this._ctx) return this._ctx;
    const noop = () => {};
    this._ctx = {
      canvas: this,
      clearRect: noop, beginPath: noop, moveTo: noop, lineTo: noop, closePath: noop,
      stroke: noop, fill: noop, arc: noop, fillText: noop, rect: noop, save: noop,
      restore: noop, translate: noop, scale: noop, setLineDash: noop,
      createLinearGradient: () => ({ addColorStop: noop }),
      fillStyle: "", strokeStyle: "", lineWidth: 1, globalAlpha: 1,
      font: "", textAlign: "", textBaseline: "",
    };
    return this._ctx;
  }
}

function makeDocument(byId) {
  const root = new El("html");
  root.setAttribute("data-theme", "dark");
  const doc = {
    documentElement: root,
    _byId: byId,
    getElementById: (id) => byId[id] || null,
    createElement: (t) => new El(t),
    // A text node is just a leaf whose textContent is the string (the readout chips use one
    // so a label's <b> and its value are separate elements).
    createTextNode: (s) => { const e = new El("#text"); e.textContent = String(s); return e; },
    createElementNS: (ns, t) => new El(t, ns),
    querySelector: (s) => root.querySelector(s),
    querySelectorAll: (s) => root.querySelectorAll(s),
    addEventListener() {},
    hidden: false,
    body: root,
  };
  Object.defineProperty(doc, "activeElement", { get: () => El._active || root, configurable: true });
  return doc;
}

/* =============================================================================
   WC (file mode, 2026-10-10): the pieces translator.html's File mode needs.
   ============================================================================= */

// ---- the page's OWN markup as the stub DOM ------------------------------------------------
// Before WC the harness hand-built the dozen elements translator.html looks up by id, which
// works until the page grows thirty more and the two lists drift apart without a sound. So the
// tree is now built FROM the page's markup: tags, ids, classes, the boolean attributes the
// script toggles (hidden, disabled), the attributes it reads (type, value, title, data-*,
// aria-*), and the text of leaf elements (button labels). Comments, <!doctype>, and the raw
// text of <script>/<style> are skipped; void elements never take children. Entities: the five.
// What it is NOT: an HTML parser. The page is hand-written and closes what it opens; a
// construct this does not understand fails the existing checks loudly (they ran first, green).
const VOID_TAGS = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                           "source", "track", "wbr"]);
const RAW_TAGS = new Set(["script", "style", "textarea", "title"]);
const decodeEntities = (s) => s.replace(/&(amp|lt|gt|quot|#39|nbsp);/g, (m, e) =>
  ({ amp: "&", lt: "<", gt: ">", quot: '"', "#39": "'", nbsp: " " })[e]);

function buildFromMarkup(html, makeEl) {
  const root = makeEl("#root");
  const ids = {};
  const stack = [root];
  const re = /<!--[\s\S]*?-->|<!doctype[^>]*>|<\/([a-zA-Z][\w-]*)\s*>|<([a-zA-Z][\w-]*)((?:\s+[^\s=>\/]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+))?)*)\s*(\/?)>|([^<]+)/gi;
  let m;
  while ((m = re.exec(html))) {
    if (m[0].startsWith("<!")) continue;
    if (m[1]) {                                   // </tag>
      const t = m[1].toLowerCase();
      for (let i = stack.length - 1; i > 0; i--)
        if (stack[i].tagName.toLowerCase() === t) { stack.length = i; break; }
      continue;
    }
    if (m[2]) {                                   // <tag ...>
      const tag = m[2].toLowerCase();
      const el = makeEl(tag);
      const attrRe = /([^\s=>\/]+)(?:\s*=\s*("([^"]*)"|'([^']*)'|([^\s>]+)))?/g;
      let a;
      while ((a = attrRe.exec(m[3] || ""))) {
        const k = a[1].toLowerCase();
        const v = a[2] === undefined ? "" : decodeEntities(a[3] ?? a[4] ?? a[5] ?? "");
        if (k === "hidden")   { el.hidden = true; el.attrs.hidden = ""; continue; }
        if (k === "disabled") { el.disabled = true; continue; }
        if (k.startsWith("data-")) {
          el.dataset[k.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = v;
          continue;
        }
        el.setAttribute(k, v);
        if (k === "type")  el.type = v;
        if (k === "value") el.value = v;
        if (k === "title") el.title = v;
      }
      if (el.id) ids[el.id] = el;
      stack[stack.length - 1].appendChild(el);
      if (RAW_TAGS.has(tag)) {                    // raw text: skip to the matching end tag
        const end = html.toLowerCase().indexOf("</" + tag, re.lastIndex);
        const body = html.slice(re.lastIndex, end < 0 ? html.length : end);
        if (tag !== "script" && tag !== "style") el.textContent = decodeEntities(body.trim());
        re.lastIndex = end < 0 ? html.length : end;
        continue;                                 // the end tag is consumed by the next match
      }
      if (!VOID_TAGS.has(tag) && !m[4]) stack.push(el);
      continue;
    }
    if (m[5]) {                                   // text: only a LEAF keeps it (labels)
      const txt = decodeEntities(m[5]).replace(/\s+/g, " ").trim();
      const host = stack[stack.length - 1];
      if (txt && host !== root && host.children.length === 0)
        host.textContent = (host._text ? host._text + " " : "") + txt;
    }
  }
  return { root, ids };
}

// ---- media: just enough HTMLMediaElement / TextTrack / VTTCue for the File mode --------------
// No decoder, no clock of its own: a test DRIVES it (_fire for an event, _seek to move the
// playhead, which also recomputes every track's activeCues and raises cuechange the way a real
// element does). Every call the page makes is recorded where a check can read it.
class VTTCueStub {
  constructor(startTime, endTime, text) {
    this.startTime = startTime; this.endTime = endTime; this.text = String(text);
    this.id = ""; this.track = null;
  }
}
class TextTrackStub {
  constructor(kind, label, language) {
    this.kind = kind; this.label = label; this.language = language || "";
    this._mode = "disabled";
    this.modeLog = [];                        // every mode the page set, in order
    this._cues = []; this.activeCues = []; this.listeners = {};
  }
  get mode() { return this._mode; }
  set mode(v) { this._mode = String(v); this.modeLog.push(this._mode); }
  get cues() { return this._cues; }           // array-like: length + index, like TextTrackCueList
  addCue(c) { c.track = this; this._cues.push(c); this._cues.sort((a, b) => a.startTime - b.startTime); }
  removeCue(c) {
    const i = this._cues.indexOf(c);
    if (i < 0) throw new Error("NotFoundError: the cue is not in this track");
    this._cues.splice(i, 1); c.track = null;
  }
  addEventListener(t, fn) { (this.listeners[t] ||= []).push(fn); }
  removeEventListener(t, fn) { if (this.listeners[t]) this.listeners[t] = this.listeners[t].filter((f) => f !== fn); }
  dispatch(t) { (this.listeners[t] || []).slice().forEach((fn) => fn({ type: t, target: this })); }
  _at(sec) {
    const next = this._mode === "disabled" ? [] : this._cues.filter((c) => c.startTime <= sec && sec < c.endTime);
    const changed = next.length !== this.activeCues.length || next.some((c, i) => c !== this.activeCues[i]);
    this.activeCues = next;
    if (changed) this.dispatch("cuechange");
  }
}
// ---- WH (11/10): HTMLMediaElement.audioTracks, as Chromium has it with --enable-blink-features=AudioVideoTracks ----
// Measured by the director (wv2probe, WH_PLAN F3): the list holds the tracks Chromium can DECODE, in container order
// (an undecodable one -- DTS -- is ABSENT, and the ids are Chromium's own numbering, never the container's), each with
// {id, kind, label (Matroska Name / MP4 hdlr name), language (as stored: "eng", "fre"...)}; the FIRST one is enabled at
// load, whatever Matroska's FlagDefault says; setting `enabled` switches the sound mid-play.
// Measured by WH-P (review WHP-11, wh_e2e\make_measure.js, WebView2 154, 11/10): ONE `change` per write that FLIPS a
// track, always queued (never inside the setter); ENABLING a track silently disables the one that was enabled (no event
// of its own: Chromium plays one audio track); disabling the only enabled one leaves NONE (silence). The stub does the
// same: `exclusive` (default) = Chromium; exclusive = false (opts.audioNonExclusive) = a runtime that follows the spec
// to the letter (several may be enabled) -- where the page's own "the others off" is what keeps one. Events are queued
// on `schedule` (run_ui.js hands it the sandbox's clock); `enabledLog` = every write the PAGE made.
class AudioTrackStub {
  constructor(list, o) {
    this._list = list;
    this.id = String(o.id == null ? "" : o.id);
    this.kind = String(o.kind == null ? "main" : o.kind);
    this.label = String(o.label == null ? "" : o.label);
    this.language = String(o.language == null ? "" : o.language);
    this._enabled = false;
  }
  get enabled() { return this._enabled; }
  set enabled(v) {
    const b = !!v;
    this._list.enabledLog.push(this.id + "=" + (b ? 1 : 0));
    if (b === this._enabled) return;
    this._enabled = b;
    if (b && this._list.exclusive) this._list._t.forEach((t) => { if (t !== this) t._enabled = false; });
    this._list._queue("change");
  }
}
class AudioTrackListStub {
  constructor() {
    this._t = [];
    this.listeners = {};
    this.enabledLog = [];
    this.changeEvents = 0;
    this.exclusive = true;                     // Chromium (measured); false = several may be enabled (the spec's letter)
    this.schedule = (fn) => fn();              // replaced by the sandbox's clock (runTranslatorWindow)
  }
  get length() { return this._t.length; }
  getTrackById(id) { return this._t.find((t) => t.id === String(id)) || null; }
  addEventListener(t, fn) { (this.listeners[t] ||= []).push(fn); }
  removeEventListener(t, fn) { if (this.listeners[t]) this.listeners[t] = this.listeners[t].filter((f) => f !== fn); }
  dispatch(t, extra) {
    const e = Object.assign({ type: t, target: this }, extra || {});
    (this.listeners[t] || []).slice().forEach((fn) => fn(e));
  }
  // one queued event per occurrence (Blink: a media element task each)
  _queue(type, extra) {
    this.schedule(() => { if (type === "change") this.changeEvents++; this.dispatch(type, extra); });
  }
  // what Chromium lists at the metadata: the list replaced, the FIRST entry enabled (F3: FlagDefault ignored)
  _set(list) {
    this._clear();
    (list || []).forEach((o, i) => {
      const t = new AudioTrackStub(this, o);
      t._enabled = i === 0;
      this._t.push(t);
      this[i] = t;
      this._queue("addtrack", { track: t });
    });
  }
  // a new resource (load()): the old tracks go
  _clear() {
    const old = this._t;
    this._t = [];
    old.forEach((t, i) => { delete this[i]; this._queue("removetrack", { track: t }); });
  }
  enabledIds() { return this._t.filter((t) => t._enabled).map((t) => t.id); }
}

function makeMediaEl(tag) {
  const e = new El(tag);
  e.src = ""; e.currentTime = 0; e.duration = NaN; e.paused = true; e.readyState = 0;
  e.error = null; e.videoWidth = 0; e.videoHeight = 0; e.muted = false; e.volume = 1;
  e.textTracks = [];
  e.audioTracks = new AudioTrackListStub();   // WH: an empty list until a test says what Chromium lists
  e.loads = 0;                                // how many times the page (re)loaded it
  e.playCalls = 0;
  e.addTextTrack = (kind, label, lang) => { const t = new TextTrackStub(kind, label, lang); e.textTracks.push(t); return t; };
  e.play = () => { e.playCalls++; e.paused = false; e.dispatch("play"); return Promise.resolve(); };
  e.pause = () => { if (!e.paused) { e.paused = true; e.dispatch("pause"); } };
  // (WH, review WHP-11: the load algorithm -- readyState back to HAVE_NOTHING, no duration, no picture size, the previous
  // resource's sound tracks gone: nothing is known of the new one until its metadata)
  e.load = () => {
    e.loads++;
    e.readyState = 0; e.duration = NaN; e.videoWidth = 0; e.videoHeight = 0;
    if (e.audioTracks && e.audioTracks._clear) e.audioTracks._clear();
  };
  // WH test driver: the sound tracks Chromium lists for the file (call it before firing loadedmetadata)
  e._setAudioTracks = (list) => { if (e.audioTracks && e.audioTracks._set) e.audioTracks._set(list); };
  e.requestFullscreen = () => { e._fullscreen = true; return Promise.resolve(); };
  e.canPlayType = () => "";
  const setAttr = e.setAttribute.bind(e);
  e.setAttribute = (k, v) => { setAttr(k, v); if (k === "src") e.src = String(v); };
  const rmAttr = e.removeAttribute.bind(e);
  e.removeAttribute = (k) => { rmAttr(k); if (k === "src") e.src = ""; };
  e.getAttribute = (k) => (k === "src" ? (e.src || null) : (e.attrs[k] ?? null));
  // test drivers
  e._fire = (type, props) => { Object.assign(e, props || {}); e.dispatch(type); };
  e._seek = (sec) => {
    e.currentTime = sec;
    e.textTracks.forEach((t) => t._at(sec));
    e.dispatch("timeupdate");
  };
  return e;
}
const makeEl = (tag) => (tag === "video" || tag === "audio") ? makeMediaEl(tag) : new El(tag);

module.exports = { El, makeDocument, buildFromMarkup, makeMediaEl, makeEl, TextTrackStub, VTTCueStub,
                   AudioTrackStub, AudioTrackListStub };

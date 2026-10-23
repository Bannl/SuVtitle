// make_media.js -- the page E2E kits' MEDIA, generated (S-WEB, 2026-10-23). Never committed: tests/**/media_gen/ is
// git-ignored, so a fresh clone runs this first (the run_*.cmd of each kit call it; it skips what is already there).
//   node tests\e2e\make_media.js [--force]          (ffmpeg from PATH, or the FFMPEG / FFPROBE variables)
//
// WHY GENERATED, AND FROM NOTHING: AudioPB's kits played the maintainer's WH test files (built from two speech
// recordings that are not in this repository) and WC's clips (a 227 MB folder of AudioPB's scratch work).
// SuVtitle's kits must not depend on either: the same STREAM LAYOUTS are rebuilt here from
// ffmpeg's own sources (testsrc2 picture, sine tones -- 440 Hz "English", 660 Hz "Chinese", so an ear can tell the two
// sound tracks apart with --sound) and the three tiny subtitle fixtures of wh_e2e\fixtures\ (copied from the frozen
// wh\media\). What the page E2E reads is the LAYOUT -- which tracks, in which order, their codecs, languages, names
// and flags -- never the speech: no engine runs in these kits (the page talks to a fake bridge).
//
// Every file is then CHECKED with ffprobe against the layout the frozen original had (probed 23/10 from the
// originals: AudioPB's wh\media\ test set and WC's mp4_h264_aac.mp4): a generator that drifted (another
// ffmpeg naming the MP4 handlers differently, a muxer dropping a flag) fails HERE, not as a puzzling E2E result.
// (The originals spoke at 16 kHz mono -- speech material; these tones are 48 kHz stereo, like a film: the page never
// sees a channel count, only the tray's description of it, which the feeds write.)
//   wh\mkv_2a_def2.mkv     h264 640x360 25 fps 12 s (no flag) | aac eng "English speech" (not default) | aac chi
//                          "Chinese speech" (DEFAULT -- the tray's choice; Chromium still enables its first) | subrip
//                          eng | ass fre "French ASS" (FORCED) | webvtt ger
//   wh\mkv_dts_first.mkv   h264 | dts eng (default; Chromium cannot decode it -> absent from audioTracks) | aac chi (not
//                          default)
//   wh\mp4_2a_movtext.mp4  h264 | aac eng (default) | aac chi -- both named by the MP4 muxer's hdlr "SoundHandler",
//                          the name Chromium shows as the label | mov_text eng "SubtitleHandler" (default, as the
//                          MP4 muxer marks it)
//   we\A\E01 Pilot.mp4     h264 High 1280x720 | aac 48 kHz stereo | 10 s (WC's mp4_h264_aac.mp4 under we_e2e's name)
//   we\A\Interview.mp3    mp3 192 kb/s stereo | 10 s (WC's mp3_192k.mp3) -- the sound-file run (A) and the folder drops (D)
//   we\B\E02 The Bridge.mp4, we\Season 1\E03.mp4, we\Season 1\E04.mp3 -- COPIES of the two above, as in the frozen kit
//     (make_we.py copied one clip under every name): the runs that need other folders (D drops, R reloads) only need a
//     different NAME and FOLDER, never different pictures. (T-E2E, 2026-10-23, RV4-11: the 12 dropped we_e2e runs)
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const HERE = __dirname;
const OUT = path.join(HERE, "media_gen");
const FIX = path.join(HERE, "wh_e2e", "fixtures");
const FFMPEG = process.env.FFMPEG || "ffmpeg";
const FFPROBE = process.env.FFPROBE || "ffprobe";
const force = process.argv.includes("--force");

const tone = (hz, sec) => ["-f", "lavfi", "-i", "sine=frequency=" + hz + ":sample_rate=48000:duration=" + sec];
const picture = (w, h, sec) => ["-f", "lavfi", "-i", "testsrc2=size=" + w + "x" + h + ":rate=25:duration=" + sec];
const h264 = (profile) => ["-c:v", "libx264", "-profile:v", profile, "-pix_fmt", "yuv420p", "-preset", "veryfast", "-g", "25"];

// expected stream layout: [codec_type, codec_name, language, title|handler, default, forced]
const MEDIA = [
  { rel: "wh/mkv_2a_def2.mkv", sec: 12,
    args: [...picture(640, 360, 12), ...tone(440, 12), ...tone(660, 12),
      "-i", path.join(FIX, "sub_en.srt"), "-i", path.join(FIX, "sub_fr.ass"), "-i", path.join(FIX, "sub_de.vtt"),
      "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s", "-map", "4:s", "-map", "5:s",
      ...h264("baseline"), "-c:a", "aac", "-b:a", "96k", "-ac", "2", "-c:s:0", "srt", "-c:s:1", "ass", "-c:s:2", "webvtt",
      "-metadata:s:a:0", "language=eng", "-metadata:s:a:0", "title=English speech",
      "-metadata:s:a:1", "language=chi", "-metadata:s:a:1", "title=Chinese speech",
      "-metadata:s:s:0", "language=eng", "-metadata:s:s:1", "language=fre", "-metadata:s:s:1", "title=French ASS",
      "-metadata:s:s:2", "language=ger",
      "-disposition:v:0", "0", "-disposition:a:0", "0", "-disposition:a:1", "default",
      "-disposition:s:0", "0", "-disposition:s:1", "forced", "-disposition:s:2", "0", "-t", "12"],
    want: [["video", "h264", "", "", 0, 0], ["audio", "aac", "eng", "English speech", 0, 0], ["audio", "aac", "chi", "Chinese speech", 1, 0],
           ["subtitle", "subrip", "eng", "", 0, 0], ["subtitle", "ass", "fre", "French ASS", 0, 1], ["subtitle", "webvtt", "ger", "", 0, 0]],
    video: [640, 360] },
  { rel: "wh/mkv_dts_first.mkv", sec: 12,
    args: [...picture(640, 360, 12), ...tone(440, 12), ...tone(660, 12),
      "-map", "0:v", "-map", "1:a", "-map", "2:a",
      ...h264("baseline"), "-c:a:0", "dca", "-strict", "-2", "-ac:a:0", "2", "-c:a:1", "aac", "-b:a:1", "96k", "-ac:a:1", "2",
      "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=chi",
      "-disposition:v:0", "0", "-disposition:a:0", "default", "-disposition:a:1", "0", "-t", "12"],
    want: [["video", "h264", "", "", 0, 0], ["audio", "dts", "eng", "", 1, 0], ["audio", "aac", "chi", "", 0, 0]],
    video: [640, 360] },
  { rel: "wh/mp4_2a_movtext.mp4", sec: 12,
    args: [...picture(640, 360, 12), ...tone(440, 12), ...tone(660, 12), "-i", path.join(FIX, "sub_en.srt"),
      "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
      ...h264("baseline"), "-c:a", "aac", "-b:a", "96k", "-ac", "2", "-c:s", "mov_text",
      "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=chi", "-metadata:s:s:0", "language=eng",
      "-disposition:a:0", "default", "-disposition:a:1", "0", "-disposition:s:0", "default",
      "-movflags", "+faststart", "-t", "12"],
    want: [["video", "h264", "", "VideoHandler", 1, 0], ["audio", "aac", "eng", "SoundHandler", 1, 0],
           ["audio", "aac", "chi", "SoundHandler", 0, 0], ["subtitle", "mov_text", "eng", "SubtitleHandler", 1, 0]],
    video: [640, 360] },
  { rel: "we/A/E01 Pilot.mp4", sec: 10,
    args: [...picture(1280, 720, 10), ...tone(440, 10), "-map", "0:v", "-map", "1:a",
      ...h264("high"), "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "48000", "-movflags", "+faststart", "-t", "10"],
    want: [["video", "h264", "", "VideoHandler", 1, 0], ["audio", "aac", "", "SoundHandler", 1, 0]],
    video: [1280, 720] },
  { rel: "we/A/Interview.mp3", sec: 10,
    args: [...tone(440, 10), "-map", "0:a", "-c:a", "libmp3lame", "-b:a", "192k", "-ar", "44100", "-ac", "2", "-t", "10"],
    want: [["audio", "mp3", "", "", 0, 0]], video: null },
  { rel: "we/B/E02 The Bridge.mp4", copyOf: "we/A/E01 Pilot.mp4", sec: 10,
    want: [["video", "h264", "", "VideoHandler", 1, 0], ["audio", "aac", "", "SoundHandler", 1, 0]], video: [1280, 720] },
  { rel: "we/Season 1/E03.mp4", copyOf: "we/A/E01 Pilot.mp4", sec: 10,
    want: [["video", "h264", "", "VideoHandler", 1, 0], ["audio", "aac", "", "SoundHandler", 1, 0]], video: [1280, 720] },
  { rel: "we/Season 1/E04.mp3", copyOf: "we/A/Interview.mp3", sec: 10,
    want: [["audio", "mp3", "", "", 0, 0]], video: null },
];

function probe(file) {
  const r = spawnSync(FFPROBE, ["-v", "error", "-show_streams", "-show_format", "-of", "json", file], { encoding: "utf8" });
  if (r.status !== 0) return { err: (r.stderr || String(r.error || "ffprobe failed")).trim() };
  try { return JSON.parse(r.stdout); } catch (e) { return { err: "ffprobe output: " + e.message }; }
}
// ffprobe names Matroska's "und"/missing language "" here; an MP4 track's name is its handler_name tag
function layout(p) {
  return (p.streams || []).map((s) => {
    const t = s.tags || {};
    const lang = t.language && t.language !== "und" ? t.language : "";
    return [s.codec_type, s.codec_name, lang, t.title || t.handler_name || "",
            s.disposition ? s.disposition.default : 0, s.disposition ? s.disposition.forced : 0];
  });
}
function verify(m, file) {
  const p = probe(file);
  if (p.err) return ["ffprobe: " + p.err];
  const got = layout(p), bad = [];
  if (got.length !== m.want.length) bad.push(got.length + " streams, " + m.want.length + " expected: " + JSON.stringify(got));
  m.want.forEach((w, i) => { if (JSON.stringify(got[i]) !== JSON.stringify(w)) bad.push("stream " + i + ": " + JSON.stringify(got[i]) + " != " + JSON.stringify(w)); });
  const v = (p.streams || []).find((s) => s.codec_type === "video");
  if (m.video && (!v || v.width !== m.video[0] || v.height !== m.video[1])) bad.push("picture " + (v ? v.width + "x" + v.height : "none"));
  if (!m.video && v) bad.push("a sound file with a picture (" + v.width + "x" + v.height + ")");
  const dur = Number(p.format && p.format.duration);
  if (!(Math.abs(dur - m.sec) < 0.35)) bad.push("duration " + dur + " (" + m.sec + " s expected)");
  return bad;
}

let failed = 0;
for (const m of MEDIA) {
  const file = path.join(OUT, ...m.rel.split("/"));
  if (!force && fs.existsSync(file) && verify(m, file).length === 0) { console.log("ok (kept)  " + m.rel); continue; }
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = file + ".part" + path.extname(file);          // never a half-written file under the final name
  let r;
  if (m.copyOf) {                                           // a copy of an entry made above (verified like the others)
    try { fs.copyFileSync(path.join(OUT, ...m.copyOf.split("/")), tmp); r = { status: 0 }; } catch (e) { r = { status: 1, stderr: e.message }; }
  } else {
    r = spawnSync(FFMPEG, ["-y", "-hide_banner", "-loglevel", "error", ...m.args, tmp], { encoding: "utf8" });
  }
  if (r.status !== 0) { failed++; console.log("FAILED     " + m.rel + "\n" + (r.stderr || String(r.error)).trim()); try { fs.unlinkSync(tmp); } catch {} continue; }
  // (the antivirus' scan window after a write: a rename right away can fail -- retry, see the user's machine notes)
  let moved = false;
  for (let i = 0; i < 20 && !moved; i++) {
    try { fs.renameSync(tmp, file); moved = true; } catch { const t = Date.now() + 50; while (Date.now() < t) { /* wait */ } }
  }
  if (!moved) { failed++; console.log("FAILED     " + m.rel + ": the rename of " + tmp + " kept failing"); continue; }
  const bad = verify(m, file);
  if (bad.length) { failed++; console.log("WRONG      " + m.rel + "\n  " + bad.join("\n  ")); continue; }
  console.log("made       " + m.rel + "  (" + fs.statSync(file).size + " bytes)");
}
console.log(failed ? "make_media: " + failed + " FAILED" : "make_media: all " + MEDIA.length + " files ok in " + OUT);
process.exit(failed ? 1 : 0);

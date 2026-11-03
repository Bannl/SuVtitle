// make_tc_fixtures.js -- U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 4): small media files that CARRY A SOURCE
// TIMECODE, made with ffmpeg, for timecode_smoke's folder mode (app/SourceTimecode.h on real files written by a real muxer).
//   node tests\cpp\make_tc_fixtures.js <outDir>        (ffmpeg: the FFMPEG variable, else `ffmpeg` on PATH; nothing downloaded)
// Writes, 1-2 s each: tc25.mov (tmcd 10:00:00:00, 25 fps), tc2997df.mov (01:00:00;00, 29.97 drop-frame), tc25.mp4 (tmcd in an
// MP4), tc25.mxf (10:00:00:00, 25), tc2997df.mxf (01:00:00;00 DF), bwf48k.wav (bext time_reference = 10:00:00 at 48 kHz),
// rf64bwf.wav (the same as RF64), notc.mp4 (no timecode at all). Exit 1 when ffmpeg is missing (the smoke then says NOT CHECKED).
"use strict";
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");
const out = process.argv[2];
if (!out) { console.log("usage: node make_tc_fixtures.js <outDir>"); process.exit(2); }
fs.mkdirSync(out, { recursive: true });
const ff = process.env.FFMPEG || "ffmpeg";
const lav = (v) => ["-f", "lavfi", "-i", v];
const jobs = [
  ["tc25.mov", [...lav("testsrc=size=160x120:rate=25"), ...lav("sine=frequency=440:sample_rate=48000"), "-t", "2", "-c:v", "mpeg4",
                "-c:a", "pcm_s16le", "-timecode", "10:00:00:00"]],
  ["tc2997df.mov", [...lav("testsrc=size=160x120:rate=30000/1001"), ...lav("sine=sample_rate=48000"), "-t", "2", "-c:v", "mpeg4",
                    "-c:a", "pcm_s16le", "-timecode", "01:00:00;00"]],
  ["tc25.mp4", [...lav("testsrc=size=160x120:rate=25"), ...lav("sine=sample_rate=48000"), "-t", "2", "-c:v", "mpeg4", "-c:a", "aac",
                "-timecode", "10:00:00:00", "-write_tmcd", "1"]],
  ["tc25.mxf", [...lav("testsrc=size=720x576:rate=25"), ...lav("sine=sample_rate=48000"), "-t", "1", "-c:v", "mpeg2video",
                "-pix_fmt", "yuv420p", "-b:v", "2M", "-c:a", "pcm_s16le", "-timecode", "10:00:00:00"]],
  ["tc2997df.mxf", [...lav("testsrc=size=720x480:rate=30000/1001"), ...lav("sine=sample_rate=48000"), "-t", "1", "-c:v", "mpeg2video",
                    "-pix_fmt", "yuv420p", "-b:v", "2M", "-c:a", "pcm_s16le", "-timecode", "01:00:00;00"]],
  ["bwf48k.wav", [...lav("sine=frequency=440:sample_rate=48000"), "-t", "1", "-c:a", "pcm_s16le", "-write_bext", "1",
                  "-metadata", "time_reference=1728000000"]],
  ["rf64bwf.wav", [...lav("sine=frequency=440:sample_rate=48000"), "-t", "1", "-c:a", "pcm_s16le", "-write_bext", "1",
                   "-metadata", "time_reference=1728000000", "-rf64", "always"]],
  ["notc.mp4", [...lav("testsrc=size=160x120:rate=25"), "-t", "1", "-c:v", "mpeg4"]],
];
let bad = 0;
for (const [name, args] of jobs) {
  const dst = path.join(out, name);
  try {
    execFileSync(ff, ["-hide_banner", "-loglevel", "error", "-y", ...args, dst], { stdio: ["ignore", "pipe", "pipe"] });
    console.log("  made " + name + " (" + fs.statSync(dst).size + " bytes)");
  } catch (e) {
    ++bad;
    console.log("  FAILED " + name + ": " + String(e.stderr || e.message).split("\n")[0]);
  }
}
process.exit(bad ? 1 : 0);

# make_media.py -- N3 (2026-10-26): the clips of tests/cpp/mpvaudio/mpvaudio_smoke.cpp, made with ffmpeg (FFMPEG, else
# ffmpeg on PATH) into <out> (work\media, git-ignored). Made once: a clip that exists is kept. Standard library only.
#   python -I make_media.py <out folder>
# THE TIMELINE CLIPS carry 50 ms bursts of 1 kHz at 0.5 at AUDIO times 2.0 and 5.0 s, the audio placed against the picture
# the way real files do it -- an MP4 edit list, a late MPEG-TS / FLV / Matroska track, a whole file starting at 10 s, a
# picture starting late -- so where the bursts land on the PLAYER's clock is known by construction (the smoke's table).
import os, subprocess, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
FF = os.environ.get("FFMPEG") or "ffmpeg"
BURSTS = "aevalsrc=if(between(t\\,2\\,2.05)+between(t\\,5\\,5.05)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=10"
VID = "testsrc2=s=160x90:r=10:d=10"
H264 = ["-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"]
# 120 s: a 30 ms burst of 1 kHz every 10 s from 3 s (seek alignment) over a quiet 300 Hz bed (never digital silence)
LONG = ("aevalsrc=0.05*sin(2*PI*300*t)+if(lt(mod(t-3\\,10)\\,0.03)*gte(t\\,3)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=120")

def tone(hz, d):
    return "sine=frequency=%d:sample_rate=48000:duration=%d" % (hz, d)

CLIPS = [
    ("late_mp4.mp4", ["-f", "lavfi", "-i", VID, "-itsoffset", "1.5", "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a",
                      *H264, "-c:a", "aac", "-b:a", "128k"]),
    ("late_ts.ts", ["-f", "lavfi", "-i", VID, "-itsoffset", "1.5", "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a",
                    *H264, "-c:a", "aac", "-b:a", "128k"]),
    ("late_mkv.mkv", ["-f", "lavfi", "-i", VID, "-itsoffset", "1.5", "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a",
                      *H264, "-c:a", "flac"]),
    ("ofs10_mkv.mkv", ["-f", "lavfi", "-i", VID, "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a", *H264, "-c:a", "flac",
                       "-output_ts_offset", "10"]),
    ("early_mp4.mp4", ["-itsoffset", "0.5", "-f", "lavfi", "-i", VID, "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a",
                       *H264, "-c:a", "aac", "-b:a", "128k"]),
    ("aud.m4a", ["-f", "lavfi", "-i", BURSTS, "-c:a", "aac", "-b:a", "128k"]),
    ("late_flv.flv", ["-f", "lavfi", "-i", VID, "-itsoffset", "1.5", "-f", "lavfi", "-i", BURSTS, "-map", "0:v", "-map", "1:a",
                      *H264, "-c:a", "aac", "-b:a", "128k", "-ar", "44100"]),
    # the sound ends at 5 s, the picture at 10 s: a start in the silent tail is "the end", never an error
    ("shortaud.mkv", ["-f", "lavfi", "-i", VID, "-f", "lavfi", "-i", BURSTS.replace("d=10", "d=5"), "-map", "0:v", "-map", "1:a",
                      *H264, "-c:a", "flac"]),
    # two sound tracks: 440 Hz (ord 0, not default), 1320 Hz (ord 1, flagged DEFAULT -- mpv's own pick, the player's too)
    ("two.mkv", ["-f", "lavfi", "-i", VID.replace("d=10", "d=6"), "-f", "lavfi", "-i", tone(440, 6), "-f", "lavfi", "-i", tone(1320, 6),
                 "-map", "0:v", "-map", "1:a", "-map", "2:a", *H264, "-c:a", "flac",
                 "-metadata:s:a:0", "language=eng", "-metadata:s:a:0", "title=A440",
                 "-metadata:s:a:1", "language=fra", "-metadata:s:a:1", "title=B1320",
                 "-disposition:a:0", "0", "-disposition:a:1", "default"]),
    ("two.mp4", ["-f", "lavfi", "-i", VID.replace("d=10", "d=6"), "-f", "lavfi", "-i", tone(440, 6), "-f", "lavfi", "-i", tone(1320, 6),
                 "-map", "0:v", "-map", "1:a", "-map", "2:a", *H264, "-c:a", "aac", "-b:a", "96k"]),
    ("vonly.mkv", ["-f", "lavfi", "-i", VID.replace("d=10", "d=3"), *H264]),
    ("long.mkv", ["-f", "lavfi", "-i", LONG, "-c:a", "flac"]),
    ("long.mp4", ["-f", "lavfi", "-i", LONG, "-c:a", "aac", "-b:a", "128k"]),
    ("long.mp3", ["-f", "lavfi", "-i", LONG, "-c:a", "libmp3lame", "-q:a", "4"]),
]

made = 0
for name, args in CLIPS:
    p = os.path.join(out, name)
    if os.path.isfile(p) and os.path.getsize(p) > 0:
        continue
    r = subprocess.run([FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", *args, p], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(p):
        print("make_media: ffmpeg failed on %s: %s" % (name, (r.stderr or "").strip()[-400:]))
        sys.exit(1)
    made += 1
nm = os.path.join(out, "notmedia.mkv")
if not os.path.isfile(nm):
    with open(nm, "w") as f:
        f.write("This is not a media file.\n")

# THE SOUND FORMAT CHANGES MID-STREAM (02/11): an MPEG-TS joined from three parts the way a broadcast recording changes --
# 4 s stereo 48 kHz (440 Hz), 4 s 5.1 48 kHz (880 Hz), 4 s stereo 44.1 kHz (1320 Hz), stream-copied by the concat demuxer.
fc = os.path.join(out, "fmtchange.ts")
if not (os.path.isfile(fc) and os.path.getsize(fc) > 0):
    parts = os.path.join(out, "fmt_parts")
    os.makedirs(parts, exist_ok=True)
    seg = [("segA.ts", 440, "48000", "2"), ("segB.ts", 880, "48000", "6"), ("segC.ts", 1320, "44100", "2")]
    for name, hz, rate, ch in seg:
        r = subprocess.run([FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=160x90:r=25:d=4",
                            "-f", "lavfi", "-i", "sine=frequency=%d:sample_rate=%s:duration=4" % (hz, rate), "-map", "0:v", "-map", "1:a",
                            *H264, "-c:a", "aac", "-ac", ch, "-ar", rate, "-b:a", "192k", os.path.join(parts, name)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("make_media: ffmpeg failed on %s: %s" % (name, (r.stderr or "").strip()[-400:]))
            sys.exit(1)
    lst = os.path.join(parts, "list.txt")
    with open(lst, "w") as f:
        for name, _, _, _ in seg:
            f.write("file '%s'\n" % os.path.join(parts, name).replace("\\", "/"))
    r = subprocess.run([FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", fc],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(fc):
        print("make_media: ffmpeg concat failed: %s" % (r.stderr or "").strip()[-400:])
        sys.exit(1)
    made += 1

# TIMESTAMP GAPS (02/11): two 5 s MPEG-TS parts joined BYTE FOR BYTE, the second stamped later (a broadcast capture that lost
# its signal) or from 0 again (a reset). Part A: a burst at 2.0 s; part B: a burst 1.0 s into it (3.0 s for the reset).
#   gap_ts.ts   B stamped +7 s  -> a ~2 s gap: the burst at the player's 8.0 s       gap_mkv.mkv  the same, remuxed to Matroska
#   gap60.ts    B stamped +60 s -> a 55 s jump: a CLOCK reset, counted on (~6.06 s)  reset_ts.ts  B from 0 again: counted on (~8.06 s)
def ts_part(name, at, offset):
    p = os.path.join(parts2, name)
    if os.path.isfile(p) and os.path.getsize(p) > 0:
        return p
    args = [FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=160x90:r=25:d=5", "-f", "lavfi",
            "-i", "aevalsrc=if(between(t\\,%g\\,%g)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=5" % (at, at + 0.05), "-map", "0:v", "-map", "1:a",
            *H264, "-c:a", "aac", "-b:a", "128k"]
    if offset:
        args += ["-output_ts_offset", str(offset)]
    r = subprocess.run(args + [p], capture_output=True, text=True)
    if r.returncode != 0:
        print("make_media: ffmpeg failed on %s: %s" % (name, (r.stderr or "").strip()[-400:]))
        sys.exit(1)
    return p

parts2 = os.path.join(out, "gap_parts")
os.makedirs(parts2, exist_ok=True)
for name, b, boff in (("gap_ts.ts", "gapB7.ts", 7), ("gap60.ts", "gapB60.ts", 60), ("reset_ts.ts", "resetB.ts", 0)):
    dst = os.path.join(out, name)
    if os.path.isfile(dst) and os.path.getsize(dst) > 0:
        continue
    pa = ts_part("gapA.ts", 2, 0)
    pb = ts_part(b, 3 if b == "resetB.ts" else 1, boff)
    with open(dst, "wb") as fo:
        for p in (pa, pb):
            with open(p, "rb") as fi:
                fo.write(fi.read())
    made += 1
# reset_long.ts: part A, then 60 s stamped from 0 again with a burst every 10 s from 3 s (44.1 kHz): after a reset the frames
# are RE-STAMPED by the session's chain, and a coarse time base would drift until aresample trims -- the bursts must stay
# exactly 10 s apart, none lost.
rl = os.path.join(out, "reset_long.ts")
if not (os.path.isfile(rl) and os.path.getsize(rl) > 0):
    pa = ts_part("gapA.ts", 2, 0)
    lb = os.path.join(parts2, "longB.ts")
    r = subprocess.run([FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=160x90:r=25:d=60",
                        "-f", "lavfi", "-i", LONG.replace("s=48000:d=120", "s=44100:d=60"), "-map", "0:v", "-map", "1:a", *H264,
                        "-c:a", "aac", "-ar", "44100", "-b:a", "128k", lb], capture_output=True, text=True)
    if r.returncode != 0:
        print("make_media: ffmpeg failed on longB.ts: %s" % (r.stderr or "").strip()[-400:])
        sys.exit(1)
    with open(rl, "wb") as fo:
        for p in (pa, lb):
            with open(p, "rb") as fi:
                fo.write(fi.read())
    made += 1
gm = os.path.join(out, "gap_mkv.mkv")
if not (os.path.isfile(gm) and os.path.getsize(gm) > 0):
    r = subprocess.run([FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", os.path.join(out, "gap_ts.ts"), "-c", "copy", gm],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("make_media: ffmpeg remux failed: %s" % (r.stderr or "").strip()[-400:])
        sys.exit(1)
    made += 1
print("make_media: %d clip(s) made, %d in %s" % (made, len(CLIPS) + 6, out))

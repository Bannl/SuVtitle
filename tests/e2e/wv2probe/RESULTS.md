# wv2probe — WebView2 facts for the Translator FILE MODE (measured 2026-10-10)

**Runtime: WebView2 154.0.4258.62** (`GetAvailableCoreWebView2BrowserVersionString`) · SDK 1.0.2592.51 ·
Windows 11 Pro build 26300 · AMD Radeon RX 7700 XT (driver 32.0.32015.2008) · window 1280x800 on the
second screen, RasterizationScale 1.

Setup, identical to the planned product: page `https://patchbus.local/<page>.html` served by
**WebResourceRequested** (`Cache-Control: no-store`, like AudioPBConfig); media on
**`https://audiopb-media.example/`** via `SetVirtualHostNameToFolderMapping(..., DENY_CORS)` → every
media request is cross-origin. Media tests play muted (`ICoreWebView2_8::IsMuted` + `muted` element).
Everything here reruns with `run_facts.cmd` (154 s). A rerun gave the same verdicts and pixel-identical cue widths.

---

## A. Codec / container matrix (probe: `probe/matrix.html`, raw: `runs/matrix.jsonl`, full table: `runs/matrix_table.md`)

Method, per file: a fresh `<video muted preload=auto>` gets `src` → `loadedmetadata`/`error` (10 s cap);
`play()` muted for 1.5 s; `webkitAudioDecodedByteCount`, `webkitVideoDecodedByteCount`,
`getVideoPlaybackQuality().totalVideoFrames`; then `currentTime = 7` → `seeked`. "Picture" means videoWidth > 0 **and**
decoded frames > 0. The picture was also checked on screenshots (`runs/frame_*.png`: correct testsrc2 frame 75 @ 3.000 s for
HEVC Main10, H.264 4:2:2 10-bit and AV1; black for ProRes).
Clips: testsrc2 1280x720 25 fps + speech (one English recording, not part of this repository) 48 kHz, 10 s.

| file | streams | canPlayType: 1st / 2nd MIME tried ¹ | load | picture | audio decoded | plays (1.5 s) | seek 7 s | VERDICT |
|---|---|---|---|---|---|---|---|---|
| mp4_h264_aac.mp4 | h264 + aac 48k/2ch | maybe / probably | ok 18 ms | 1280x720 | yes | 1.46 s | seeked 75 ms | **VIDEO OK** |
| mp4_hevc_aac.mp4 | hevc + aac | maybe / probably | ok 7 ms | 1280x720 | yes | 1.46 s | seeked 107 ms | **VIDEO OK** ² |
| mp4_hevc10_aac.mp4 | hevc Main 10 + aac | maybe / probably | ok 6 ms | 1280x720 | yes | 1.46 s | seeked 97 ms | **VIDEO OK** ² |
| mp4_h264_422_10bit_aac.mp4 | h264 High 4:2:2 10-bit + aac | maybe / maybe | ok 6 ms | 1280x720 | yes | 1.46 s | seeked 159 ms | **VIDEO OK** (software) |
| mp4_av1_opus.mp4 | av1 + opus | maybe / probably | ok 7 ms | 1280x720 | yes | 1.46 s | seeked 15 ms | **VIDEO OK** |
| mov_h264_aac.mov | h264 + aac | "" / "" | ok 6 ms | 1280x720 | yes | 1.46 s | seeked 70 ms | **VIDEO OK** |
| mov_hevc_aac.mov | hevc + aac (iPhone-style) | "" / "" | ok 6 ms | 1280x720 | yes | 1.46 s | seeked 97 ms | **VIDEO OK** ² |
| mov_prores_pcm16.mov | prores Proxy + pcm_s16le | "" | ok 7 ms | **none** | yes | 1.46 s | seeked 1 ms | **AUDIO-ONLY** |
| mov_h264_pcm24.mov | h264 + pcm_s24le | "" | ok 9 ms | 1280x720 | yes | 1.46 s | seeked 70 ms | **VIDEO OK** |
| m4v_h264_aac.m4v | h264 + aac | maybe / probably | ok 5 ms | 1280x720 | yes | 1.46 s | seeked 75 ms | **VIDEO OK** |
| mkv_h264_aac.mkv | h264 + aac | maybe / probably | ok 6 ms | 1280x720 | yes | 1.46 s | seeked 81 ms | **VIDEO OK** |
| mkv_h264_ac3.mkv | h264 + ac3 | maybe / probably | ok 4 ms | 1280x720 | **yes (AC-3!)** | 1.46 s | seeked 111 ms | **VIDEO OK** |
| mkv_hevc_aac.mkv | hevc + aac | maybe | ok 5 ms | 1280x720 | yes | 1.46 s | seeked 86 ms | **VIDEO OK** ² |
| webm_vp9_opus.webm | vp9 + opus | maybe / probably | ok 4 ms | 1280x720 | yes | 1.46 s | seeked 27 ms | **VIDEO OK** |
| webm_vp8_vorbis.webm | vp8 + vorbis | maybe / probably | ok 5 ms | 1280x720 | yes | 1.46 s | seeked 56 ms | **VIDEO OK** |
| avi_mpeg4_mp3.avi | mpeg4 + mp3 | "" / "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| avi_h264_pcm.avi | h264 + pcm_s16le | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| wmv_wmv2_wmav2.wmv | wmv2 + wmav2 | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| mpg_mpeg2_mp2.mpg | mpeg2video + mp2 (PS) | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| ts_h264_aac.ts | h264 + aac | "" / "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| 3gp_h264_aac.3gp | h264 + aac | maybe / probably | ok 5 ms | 1280x720 | yes | 1.46 s | seeked 79 ms | **VIDEO OK** |
| mxf_dnxhr_pcm24.mxf | DNxHR LB + pcm_s24le (OP1a) | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| wav_pcm16.wav | pcm_s16le 48k/2ch | maybe / probably | ok 5 ms | none | yes | 1.46 s | seeked 0 ms | **AUDIO OK** |
| wav_pcm24.wav | pcm_s24le | maybe | ok 5 ms | none | yes | 1.46 s | seeked 0 ms | **AUDIO OK** |
| wav_f32.wav | pcm_f32le | maybe / "" (`codecs="3"`) | ok 5 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| wav_96k_24.wav | pcm_s24le 96k | maybe | ok 7 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| bwf_pcm24.wav | pcm_s24le + bext | maybe | ok 7 ms | none | yes | 1.46 s | seeked 0 ms | **AUDIO OK** |
| wav_poly8_24.wav | pcm_s24le **8 ch** (poly) | maybe | ok 9 ms | none | yes | 1.46 s | seeked 2 ms | **AUDIO OK** |
| wav_rf64_24.wav | pcm_s24le, **RF64** header | maybe | ok 6 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| aiff_pcm16.aiff | pcm_s16be | "" / "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| aiff_pcm24.aiff | pcm_s24be | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| aifc_sowt.aifc | AIFF-C sowt | "" / "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| mp3_192k.mp3 | mp3 | probably / probably | ok 3 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| m4a_aac.m4a | aac | maybe / probably | ok 3 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| m4a_alac.m4a | alac | maybe / "" | error 4 DEMUXER_ERROR_NO_SUPPORTED_STREAMS | – | – | – | – | **FAIL** |
| adts_aac.aac | aac (ADTS) | probably | ok 3 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| flac_16.flac | flac | probably / "" (`codecs="flac"`) | ok 3 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| ogg_vorbis.ogg | vorbis | maybe / probably | ok 4 ms | none | yes | 1.46 s | seeked 1 ms | **AUDIO OK** |
| opus_96k.opus | opus (Ogg) | probably (`audio/ogg;codecs=opus`) / "" (`audio/opus`) | ok 3 ms | none | yes | 1.46 s | seeked 3 ms | **AUDIO OK** |
| wma_wmav2.wma | wmav2 | "" | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** |
| ac3_192k.ac3 | raw AC-3 | "" / probably (`audio/mp4;codecs=ac-3`) | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** (container) |
| eac3_192k.eac3 | raw E-AC-3 | "" / probably (`audio/mp4;codecs=ec-3`) | error 4 DEMUXER_ERROR_COULD_NOT_OPEN | – | – | – | – | **FAIL** (container) |
| mka_aac.mka | aac in Matroska | maybe / "" | ok 3 ms | none | yes | 1.46 s | seeked 3 ms | **AUDIO OK** |
| mka_opus.mka | opus in Matroska | maybe / probably | ok 3 ms | none | yes | 1.46 s | seeked 4 ms | **AUDIO OK** |

Totals: 15 VIDEO OK · 1 AUDIO-ONLY (ProRes) · 15 AUDIO OK · 13 FAIL. No file played video with silent audio.
Every failure is immediate (`error` code 4 within a few ms), never a hang.

¹ The exact MIME strings are in `media/manifest.json`. **`canPlayType` is not a usable pre-flight**: `""` for every .mov
(they play), `""` for `audio/wav; codecs="3"` and `audio/flac; codecs="flac"` (both play), `probably` for
`audio/mp4; codecs="ac-3"` while the raw .ac3 file fails. Probe by loading (`loadedmetadata`/`error`, a few ms here) instead.
² **Machine-dependent.** `navigator.mediaCapabilities.decodingInfo` (`runs/mediacaps.jsonl`), [supported, smooth, powerEfficient]:
H.264 8-bit [t,t,**t**] · H.264 4:2:2 10-bit [t,t,f] (software) · HEVC Main and Main10 [t,t,**t**] · AV1 [t,t,**t**] · VP9 [t,t,**t**] · VP8 [t,t,f].
HEVC decodes on this GPU's hardware. Chromium has no software HEVC decoder, so expect HEVC to FAIL on machines without
hardware HEVC decode. AV1 has a software fallback. Audio decodingInfo: AAC, Opus, FLAC, MP3, **AC-3, E-AC-3** supported; ALAC not.
MIME types the mapping serves (from CDP Network): .mov `video/quicktime`, .mkv `video/matroska`, .m4a `audio/x-m4a`,
**.ts `text/plain`**, .mxf/.eac3 `application/octet-stream`. That does not matter here because Chromium sniffs the container.

## B. Long file over the mapping (`runs/seek.jsonl`)

10-minute 720p H.264 + AAC (52 MB), played muted, then seeks while playing:

| file | loadedmetadata | → 9:00 seeked | → 9:00 playing again ³ | → 1:00 | → 5:00 | → 9:59 |
|---|---|---|---|---|---|---|
| long10min_faststart.mp4 (moov first) | 16–21 ms | **6–8 ms** | 313–316 ms | 6–7 / 311–315 ms | 6–8 / 314–315 ms | 90–106 / 396–414 ms |
| long10min_moovend.mp4 (moov last, camera/DAW default) | 10–14 ms | **6–7 ms** | 311–313 ms | 6–8 / 314–315 ms | 6–8 / 312–315 ms | 96–127 / 402–435 ms |

Two runs; each cell gives `seeked` / playing again. ³ "Playing again" = `currentTime` > target + 0.25 s, so about 250 ms of it is playback itself.
**Range requests work through the mapping**: each seek is a new `GET` with `Range: bytes=N-`, answered **206** with
`Content-Range` and `Accept-Ranges: bytes` in about 2 ms. A moov-last file costs one extra range request (`bytes=51740672-`)
5 ms after the first. `buffered` shows disjoint ranges ([0,36],[540,588] …), so nothing re-downloads the whole file.
The documented "media over a virtual host can be very slow" warning does **not** apply to this runtime.

## C. Security of DENY_CORS (`probe/cors.html`, `runs/cors_*.jsonl`)

Page origin `https://patchbus.local`. Media host mapped with the kind in the column header. The last column is a control: the same folder
**served** by WebResourceRequested on another origin (no CORS headers).

| check | DENY_CORS | ALLOW | DENY | served control |
|---|---|---|---|---|
| `<video src>` (no `crossorigin`) | **loads** (97–104 ms incl. decoder start) | loads | **error 4** "Format error" (misleading) | loads |
| `<video crossorigin=anonymous>` | error 4 | loads | error 4 | error 4 |
| `fetch(url)` (cors mode) | **TypeError: Failed to fetch** | 200 | fails | fails |
| `fetch(url, {headers:{Range}})` | fails | **206** | fails | fails |
| XMLHttpRequest | fails (status 0) | 200 | fails | fails |
| **`fetch(url, {mode:"no-cors"})`** | **200, type "basic", full body readable (62/62 bytes)** | 200 basic | fails | opaque, status 0 (correct) |
| canvas `drawImage(video)` + `getImageData` | **readable (not tainted)** | readable | n/a | SecurityError (correct) |
| `<track src>` cross-origin, no `crossorigin` | blocked (same-origin fallback) | blocked | blocked | blocked |
| `<track src>` + `crossorigin` | blocked | **loads** | blocked | blocked |

How DENY_CORS refuses: it answers with an **empty** `Access-Control-Allow-Origin`. Console: *"… has been blocked by CORS policy:
The 'Access-Control-Allow-Origin' header has a value '' …"*, CDP `corsError: AllowOriginMismatch`.
**⚠ DENY_CORS blocks CORS-mode reads only.** A page in that WebView can still read *every file in the mapped folder* with
`fetch(url, {mode:"no-cors"})` (it gets a readable `basic` response instead of the spec's opaque one) or by canvas readback. The
served-host control behaves to spec, so this comes from the folder mapping. Do not treat the mapping as a confidentiality
boundary: map the narrowest folder possible, and never let untrusted content run in that WebView (no remote navigation, no HTML injection).

**`.example` vs `.local`.** First media load on a fresh process, page→`loadeddata`, three runs each:
`audiopb-media.example` 26 / 28 / 34 ms · `audiopb-media.local` 31 / 34 / 36 ms. **No first-load delay for media on either
TLD.** (In cors.html, which includes decoder start, the first load was 87–104 ms on both, and the second 28–31 ms.)

**But NAVIGATIONS to any `.local` host cost about 2.0 s, every time** (served or mapped). The document arrives in 11 ms; the commit
waits about 2 s. The cause is **SmartScreen** reputation checking (`ICoreWebView2Settings8::IsReputationCheckingRequired`):

| navigation | SmartScreen on (default, the product today) | SmartScreen off (`--no-smartscreen`) |
|---|---|---|
| `https://patchbus.local/echo.html`, 4 consecutive navigations | 2022, 2011, 2011, 2012 ms (also 2023–2029 on other fresh profiles) | 263 (cold), 13, 12, 11 ms |
| real `ui/web/translator.html`, fresh profile | **2044, 2046 ms** | 59, 139 ms |
| `https://probe.example/echo.html` | 1079 (cold process), 34, 50, 32 ms | — |

These flags made no difference: `LocalNetworkAccessChecks`, the Private Network Access features, `--host-resolver-rules`.
→ The shipped AudioPBConfig most likely pays **+2 s on every main-window and Translator-window page load**. Measured here with an
identical setup; the product itself was not touched.

## D. Native subtitles (`probe/subs.html`, `runs/subs*.jsonl`, screenshots in `runs/subs/`; all inspected)

TextTrack `kind=subtitles`, `mode=showing`, VTTCue added from JS (no `<track src>`: DENY_CORS forbids it, see C).
Style: `video::cue { font: 600 30px/1.3 "Segoe UI","Microsoft JhengHei UI",…; background: rgba(0,0,0,.6) }`,
`video::cue(.src) { font-size: 20px; font-weight: 400; color: #c9cdd4 }`.

| test | result | evidence |
|---|---|---|
| two lines: translation + `<c.src>original</c>` | ✅ renders. Big line wraps, small grey line under it, per-line translucent background | `sub_1_two_lines.png` |
| CJK `繁體中文：對白音量剛好，但殘響太長了。` | ✅ correct glyphs (JhengHei fallback), full-width punctuation | `sub_2_cjk.png` |
| long cue | ✅ wraps to 4 lines + src line, stays inside the picture | `sub_3_long_wrap.png` |
| cue added **while playing**, already covering the playhead ("late") | ✅ 6/6 active **synchronously** (`activeCues` contains it right after `addCue`, `enter` at 0–1 ms) and painted | `sub_4_progressive_mid.png`, `runs/subs.jsonl` progressive-done |
| cue added while playing, ahead of the playhead | ✅ 5/5 `enter`/`exit` within 1–4 ms of the cue's start/end (e.g. start 1.06 → entered @1.063) | progressive-done (22 `cuechange` for 11 cues) |
| remove all cues while paused | ✅ display clears at once | `sub_5_cleared_while_paused.png` |
| re-add while paused | ✅ shows at once, no seek needed | `sub_6_readded_while_paused.png` |
| `mode = "hidden"` / back to `"showing"` | ✅ box disappears and `activeCues` still populated / box returns | `sub_7_mode_hidden.png`, `sub_8_mode_showing_again.png` |
| `<audio>` + track `mode="hidden"`, `cuechange` | ✅ **12/12** events exactly at the boundaries (0.5 enter, 1.3 exit, … 6.3) with `activeCues` text. This drives the audio-only big panel | `runs/subs2.jsonl` audio-hidden-done |
| native controls visible | ✅ cues move up above the control bar | `sub_10_native_controls.png` |
| fullscreen via `video.requestFullscreen()` with a user gesture (CDP `userGesture`) | ✅ `ContainsFullScreenElementChanged` fires. The element fills the **WebView bounds** (the host must resize the window itself) | `sub_9_fullscreen_js.png` |
| fullscreen via the **native controls' button** | ✅ **testable headlessly**: a CDP `Input.dispatchMouseEvent` click at the button → fullscreen on | `native_fullscreen_click.png` |

**Cue styling facts** (widths measured by `tools/png_lines.py`, 19-glyph CJK line, `runs/subs_line_widths.txt`):
- **`video::cue { font-size }` is ignored**, in every form: 30 px, 60 px, `!important`, set before the cues exist. All give 493 px =
  Chromium's default **5 % of the video height** (27 px at 540 px tall, ~40 px in fullscreen). Weight, colour and background from `video::cue` do apply.
- **Sizes work through the class form**: `video::cue(.tr)` / `video::cue(.src)` on `<c.tr>…</c>` / `<c.src>…</c>` spans. 44 px → 805 px.
  **Recipe: wrap both lines**, `"<c.tr>translation</c>\n<c.src>original</c>"`.
- **A CSS change does not reach cue boxes already on screen** (`font_A_src40_dynamic.png` is unchanged). It shows after the cues are
  re-created (`font_A2_…`) or after **`track.mode = "hidden"; track.mode = "showing"`** (`font_D_…`, 56 px). The A−/A+ buttons must do that toggle.
- **`em` sizes follow the video into fullscreen**: `::cue(.tr){font-size:1.4em}`, `::cue(.src){font-size:.8em}` → 691 px windowed and 1024 px fullscreen
  (×1.48 = 800/540). px sizes stay fixed. `em_1_windowed.png`, `em_2_fullscreen.png`.

## E. `chrome.webview.postMessageWithAdditionalObjects` (`probe/misc.html`, `runs/misc.jsonl`)

- `typeof` → **"function"** (also `hostObjects`, `releaseBuffer`).
- **Real path round trip verified without an OS drag-drop.** CDP `DOM.setFileInputFiles` put a real path on `<input type=file>`
  (a file-backed File, like a dropped one). The page called `postMessageWithAdditionalObjects({type:"files-posted"}, input.files)` and the host got
  `ICoreWebView2File::get_Path` = the full Windows path (`{"__host":"msg-objects","files":["C:\\…\\media\\mp4_h264_aac.mp4"]}`).
- A JS-constructed `new File([...])` throws: *"additional File object is not a file on the disk"*.

## Other facts picked up on the way

- **Autoplay**: unmuted `play()` without user activation → `NotAllowedError`. With a gesture → plays. Muted `<video>` autoplays,
  but **muted `<audio>` does not** ("play() failed because the user didn't interact with the document first"). Chromium's muted
  exemption covers `<video>` only, so the audio-only view must start from a click (or use a `<video>` element for audio files).
- The 2nd screen renders at ~165 Hz (rAF 160–171/s during the matrix). The page was never throttled (`visibilityState` "visible" throughout).
- Focus: the window (WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW, SW_SHOWNOACTIVATE) never took the foreground (`{"__host":"focus","stolen":false}` at navigation and exit in every run).
- Running two instances back-to-back on the **same** user-data folder: the second waits ~5 s for the first browser process to exit.

## Not verified

- Real OS **drag-drop** (simulated through `DOM.setFileInputFiles`; same File kind, but the drop event path itself was not exercised).
- **Hidden / minimised / occluded** window behaviour (not tested after the instruction to keep windows on the second screen; nothing covered the probe window).
- Audible output: everything ran muted, so A/V sync and audio device behaviour were not judged.
- Real-size files: only 10 s clips plus one 52 MB file. Multi-GB RF64/MOV files and VFR or odd-timebase camera files were not tested.
- Other machines: HEVC (and AC-3/E-AC-3 decoding, which likely comes from the OS) depend on hardware and OS components. Not checked on a second machine.
- The watchdog exit path (code 3) was not triggered. The other exit codes (0/1/2/4/5) were.
- Why SmartScreen stalls exactly 2 s on `.local` (timeout vs. lookup) is not established, only that turning it off removes the stall.

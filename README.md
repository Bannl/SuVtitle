# SuVtitle

**SuVtitle** is a video and audio player for Windows that **transcribes and translates what is said in a
file, offline, and shows it as subtitles that stay ahead of the picture**. Speech recognition runs on
[whisper.cpp](https://github.com/ggerganov/whisper.cpp) and translation on
[llama.cpp](https://github.com/ggerganov/llama.cpp) with a Qwen model, on the GPU (Vulkan) or the CPU.
Nothing you watch, say or transcribe leaves your computer: the one request SuVtitle makes is its update check (see
*Updates*), which you can turn off.

- Start watching after a short load: the analysis runs ahead of the playhead, follows your seeks and is
  cached, and one `.srt` per language is written next to the file as it goes.
- A real player: queue, folders, resume, full screen, keyboard shortcuts, default-player registration.
- **Plays almost every file** (1.1.0): a native video engine (libmpv) plays MKV, MP4, AVI, TS / M2TS, MPG, WMV, FLV,
  OGG and the codecs inside them, with or without a graphics card (hardware decoding when there is one).
- Subtitles from everywhere: SuVtitle's own, `.srt` / `.vtt` / `.ass` files, and the film's embedded
  text tracks; choose the sound track of a multi-audio film -- MKV and MP4, and (1.1.0) the AVI, TS / M2TS, MPG, WMV,
  FLV and OGG files the native video engine lists -- and the analysis follows the track you hear.
- A subtitle editor (text and timing) and a subtitle style panel.
- **Transcripts** (1.1.0): what is said in a file, speaker by speaker (recognised by voice), with a timecode at
  each turn, saved as a clean Word file for professional transcribers.
- **Live mode** (1.1.0): a compact window that transcribes and translates, as it happens, a microphone or line
  input, the sound of an output, or one application, as live subtitles plus a transcript you can export.

## Get SuVtitle

- Ready-made Windows installer (€10, updates included): https://www.nextale-audio.com/suvtitle
- SuVtitle is free software (GPL v3), so you can also build it yourself: see [Building](#building).

SuVtitle is published by NEXTALE Audio. It began as the file-mode player of AudioPB Translator. See
`CHANGELOG.md` for what is in each version.

## Live mode

SuVtitle 1.1.0 adds a **Live window** beside the player. It listens to the sound you point it at, transcribes and
translates it as it is spoken, and shows the result as live subtitles and a running transcript. It is compact and
has an always-on-top pin, so it can sit over a video call.

- **Sources.** Each source is a *strip*: an **input** (a microphone, a line, the output of a virtual cable, or one
  channel of an audio interface), **the sound of an output** (what a playback device is playing, captured by
  loopback), or **one application** (that application's audio only; this needs Windows 10 build 20348 or later,
  or Windows 11, and is greyed out below that). Up to **4 strips** run at once, each with a gain trim and a level
  meter. The meters run only while listening, and the source list never opens a device: nothing is opened until you
  press **Listen**, and listening never starts by itself at launch.
- **Speakers.** Each strip names its speaker (you type the name). When the speaker model is installed (see *The AI
  payload*), the voices heard on one strip are also told apart, and every line carries its speaker's colour; without
  it, lines are labelled by their strip.
- **Languages.** The Live window has **its own language pair** and direction, separate from the player's pair: a
  film can be translated Spanish to French while a call is translated French to Chinese. The quality preset (Auto,
  Quality, Balanced, Light) is chosen from a menu in the Live window.
- **Session journal and export.** Every final line is journaled to
  `%APPDATA%\SuVtitle\Live sessions\<date>_<time>.jsonl`. **Export** writes the session as a `.docx`; **Clear**
  starts a new session. The current session survives a restart of the app.
- **Starting it.** Use the player's **Translate Live** button, or start `SuVtitle.exe --live` (the installer adds a
  **SuVtitle Live** shortcut to the Start menu); `--live` opens the Live window alone. Closing the Live window stops
  listening; the app quits when its last window closes.
- **Live and file analysis never run together.** Pressing Listen while a film is being analysed cancels that
  analysis (what is done is kept) and a notice on the player says so; the analysis resumes by itself when the live
  stops. A film opened during a live session plays with the subtitles already made.
- **Listen off finishes the last sentences.** Stopping releases the devices at once; the sentences already heard are
  then still transcribed and translated ("Finishing the last sentences..."). On the CPU that can take tens of
  seconds.

## Transcription

Next to **Translate**, the player has a **Transcribe** button. Pressed, SuVtitle writes down what is said in the file,
**speaker by speaker**, with a **timecode at each turn**, and saves it as a Word file next to the media:
`<name> - transcript.docx`.

- **One analysis feeds both.** Translate and Transcribe can both be on: the file is analysed once (whisper runs once),
  then a short **voice pass** recognises the speakers by their voice (the CAM++ speaker model) on the finished analysis.
  A file already translated is transcribed by the voice pass alone. Transcribe alone also runs the analysis' translation
  step (the same analysis serves both buttons): on a CPU that costs about 40 % more than the transcription alone would
  (measured on this project's test PC with the shipping models).
- **Each button has its own Cancel**, named for its work: **Cancel translation** and **Cancel transcript** (the same
  words in the **⋯** menu of a narrow window). Cancelling one never stops the other's work: the analysis goes on while
  the other button still wants it.
- **The Transcript tab** (next to Subtitles and Queue) shows it as it grows; a click on a turn plays the film from there.
  **Click a speaker's name to rename that speaker** (every turn of it); giving two speakers the same name makes them one
  person. A rename -- or an edit of the subtitles that changes the transcript -- updates the Word file by itself, a
  second or two after the last change, when SuVtitle wrote that file and it has not been changed since. A Word file you
  edited is never touched: the tab says so, and **Save .docx…** offers a new name. When the Word file is open in another
  program, the tab says to close it to update it and nothing is written anywhere else; the next rename, edit,
  **Re-transcribe** or **Save .docx…** tries again. A read-only Word file is left as it is: **Save .docx…** writes a
  new one.
- **Timecodes.** `[hh:mm:ss]` from the start of the file -- or, when the recording carries its own start timecode (a
  BWF / RF64 / BW64 file from a field recorder, with iXML; a MOV / MP4 with a timecode track; an MXF), the **source
  timecode** `[hh:mm:ss:ff]` (`;` before the frames for drop-frame). The Word file's header says which. A Word file
  written with the source timecode keeps it: when the timecode cannot be read on a later run (a sleeping disk, a file
  in use), SuVtitle neither rewrites it with times from the start of the file nor calls it out of date.
- **Dialogue lines.** A subtitle that holds two speakers' lines ("- Yes. - No.") is split into its lines in the
  transcript, each given its own speaker; the subtitles themselves are not changed.
- **One Word file per sound track.** The first sound track's transcript is `<name> - transcript.docx`; each other
  sound track of the film gets its own, `<name> - transcript (track N).docx` (N = its number in the track menu). An
  analysis of the same sound track in another language pair or with another model never replaces that Word file and
  writes no new one by itself: its Transcript tab says so, and **Save .docx…** writes it to a new file.
- **Transcribe all** (the Queue tab) writes one Word file per file of the list, one after another. **Cancel all** shows
  while the list has work running or waiting, and stops only what the list launched (a file you pressed Translate or
  Transcribe on yourself goes on). A Word file SuVtitle did not write is never overwritten (the transcript takes another
  name: `<name> - transcript (SuVtitle).docx`); one you edited since SuVtitle wrote it is kept as it is (Transcribe all
  leaves it, Re-transcribe writes the other name). When a file's Word file is not next to it -- saved in another folder,
  not written, or not up to date -- its row in the Queue tab says where it is or why, and one message at the end counts
  them.
- The Export menu has both Word files: **Bilingual subtitles (.docx)** (each subtitle with its translation) and
  **Transcript (.docx)**.

Without the speaker model the transcript is still made, with no names, and says why.

## Requirements

- Windows 10 or 11, x64. Live mode on **one application's** sound needs Windows 10 build 20348 or later, or
  Windows 11; the other sources work on any supported Windows.
- The Microsoft Edge **WebView2 Runtime** (built into Windows 11; on Windows 10 install the Evergreen runtime).
- No graphics card is required. A graphics driver that provides Vulkan makes the analysis faster; without one,
  everything runs on the CPU, and the picture is drawn through Direct3D 11 (Windows' software renderer when there is no
  GPU at all). At its start the engine asks once which GPU it can use: when there is none, every model runs on the CPU
  from the start, the Live window says "CPU", and its Auto preset takes Light at once. libmpv and the Vulkan builds of
  whisper.cpp import `vulkan-1.dll` even to run on the CPU, so SuVtitle ships Khronos' Vulkan loader for a PC that has
  none (`mpv\vkfallback\`); a PC whose driver installed `System32\vulkan-1.dll` keeps using its own.

## Repository layout

| Folder | Content |
|---|---|
| `app/` | `SuVtitle.exe`: the windows (Win32 + WebView2: the player and the Live window), the player core, subtitles, the engine supervisor |
| `app/web/` | the pages the windows show (`index.html` the player, `live.html` the Live window; HTML/CSS/JS, served locally by the app) |
| `engine/` | `SuVtitleEngine.exe`: drives whisper-server and llama-server, runs the file analysis and the Live mode (WASAPI capture, voice-activity detection, speaker labels) |
| `common/` | `TrPipe.h`, the protocol between the app and the engine |
| `tests/` | C++ smoke tests and mutants (`cpp/`), the page harnesses (`web/`), end-to-end kits (`e2e/`), the boundary check (`boundary/`) |
| `tools/` | build scripts |
| `deploy/`, `installer/` | the bundle script and the Inno Setup installer |

## Building

You need **Visual Studio 2022 or later** with the *Desktop development with C++* workload (MSVC, CMake and
Ninja come with it). SuVtitle is developed with Visual Studio 2026.

```
tools\buildlock.cmd x64-Release
```

The script configures `build\x64-Release` on first use, then builds. The output is
`build\x64-Release\SuVtitle.exe`, `SuVtitleEngine.exe`, `WebView2Loader.dll` and the page in `web\`.

- Visual Studio is found by `tools\vsenv.cmd`: the folder named by `SUVTITLE_VS` (an installation root), else
  an already loaded *Developer Command Prompt*, else the latest installation `vswhere` lists (preview channels
  included).
- The WebView2 SDK is the NuGet package `Microsoft.Web.WebView2` 1.0.2592.51, never stored in this repository.
  The first configure downloads it from nuget.org into the build folder. To build offline, unzip the `.nupkg`
  (it is a zip) and set `WEBVIEW2_NUGET_DIR` to the folder that holds its `build\native\`; the build folder
  remembers it.
- The native video engine is libmpv, loaded at run time from `mpv\libmpv-2.dll` next to `SuVtitle.exe` (or from
  the exe's own folder). Building needs no mpv file, and without the DLL the player plays everything through the
  page's own `<video>`. For a development build that plays through mpv, set `SUVTITLE_MPV_DIR` to a folder that
  holds a `libmpv-2.dll` (a libmpv development build); the build copies it to `mpv\` next to the exe, and the
  build folder remembers the variable. `tests\cpp\mpv\` checks the engine against that DLL (it also needs
  `include\mpv\client.h` from the same folder).
- `SuVtitleEngine.exe` loads the same `libmpv-2.dll` (`<exe>\mpv\`, else the exe's folder, else `SUVTITLE_MPV_DIR`)
  to hear the files it analyses, headless: the subtitles land on the player's own timeline and every format the player
  reads can be analysed. Without the DLL it falls back to Windows Media Foundation, which reads fewer formats (no FLV,
  no DTS or TrueHD) and places some of them differently in time (MPEG-TS).

## The AI payload

The engine needs the whisper.cpp and llama.cpp server binaries, the sherpa-onnx runtime and six models. They are
not in this repository. An installed SuVtitle has them next to the executable (the installer downloads the six models
from Hugging Face while it installs, see *Packaging*):

```
<app>\engine\whisper\   whisper-server.exe + its ggml DLLs (Vulkan build)
<app>\engine\llama\     llama-server.exe + its ggml DLLs (Vulkan build)
<app>\engine\sherpa\    sherpa-onnx-c-api.dll, onnxruntime.dll, onnxruntime_providers_shared.dll   (Live mode and transcripts: speakers)
<app>\mpv\              libmpv-2.dll: the native video engine (it plays every file; the analysis hears the files through it)
<app>\mpv\vkfallback\   vulkan-1.dll + VulkanRT-License.txt: Khronos' Vulkan loader, used only on a PC that has none
<app>\models\           ggml-large-v3-turbo.bin                   films; the final model of the Live "quality" preset
                        ggml-small.bin, ggml-base.bin             Live mode ("balanced" and "light" presets)
                        Qwen3-4B-Instruct-2507-Q5_K_M.gguf        translation
                        qwen2.5-1.5b-instruct-q4_k_m.gguf         translation, smaller (fallback)
                        3dspeaker_campplus_zh_en_advanced.onnx    the speaker model (CAM++): Live mode and transcripts
```

- whisper.cpp and llama.cpp: the Windows Vulkan release builds of each project.
- `libmpv-2.dll`: a libmpv development build for Windows x64 (SuVtitle 1.1.0 ships shinchiro's
  `mpv-dev-x86_64-20261021-git-e471496c56`; `THIRD_PARTY_NOTICES.md` says where its sources are).
- `vulkan-1.dll` and `VulkanRT-License.txt`: LunarG's Vulkan runtime redistributable, the `x64` folder of
  `VulkanRT-X64-<version>-Components` (1.4.363.0 in 1.1.0).
- `ggml-large-v3-turbo.bin`, `ggml-small.bin`, `ggml-base.bin`: the whisper.cpp model repository on Hugging Face
  (`ggerganov/whisper.cpp`).
- The Qwen models: GGUF quantizations of `Qwen3-4B-Instruct-2507` (Q5_K_M, Unsloth's:
  `unsloth/Qwen3-4B-Instruct-2507-GGUF`) and `Qwen2.5-1.5B-Instruct` (q4_k_m, the Qwen team's:
  `Qwen/Qwen2.5-1.5B-Instruct-GGUF`) on Hugging Face.
- `sherpa-onnx-c-api.dll`, `onnxruntime.dll`, `onnxruntime_providers_shared.dll`: the DLLs of the Windows x64
  *shared* build of [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (its `lib\` folder). The engine loads
  them itself, at run time, from their own folder, never from a copy of ONNX Runtime that Windows ships.
- The speaker model: the CAM++ model of the [3D-Speaker](https://github.com/modelscope/3D-Speaker) project, as an
  ONNX file published on Hugging Face (`csukuangfj/speaker-embedding-models`) under the name
  `3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx`. SuVtitle accepts that name as it is; the installer
  saves the same bytes as `3dspeaker_campplus_zh_en_advanced.onnx`.
- `installer\models.json` lists the six model files with their size, their SHA-256, their license and the Hugging Face
  URL each one comes from, pinned to a commit: those exact bytes are what the installer downloads (and what a payload's
  `models\` must hold for an offline installer).

A development build has nothing next to it, so give it a payload folder in the environment variable
`SUVTITLE_PAYLOAD`, laid out without the `engine\` level:

```
<payload>\whisper\   <payload>\llama\   <payload>\sherpa\   <payload>\models\
```

The engine looks, in order, at the `WHISPERBIN`, `LLAMABIN` and `MODELS` lines of `%APPDATA%\SuVtitle\engine.txt`,
then at the folders next to its executable, then at `SUVTITLE_PAYLOAD`. A payload with a missing part is reported
as an error naming that part, never completed from another one. The speaker parts follow the same source
(`engine\sherpa\` next to the executable, else `<payload>\sherpa\`; an `SPKMODEL` line in `engine.txt` can name the
model file) but are optional at run time: without either of them the Live mode still runs and labels each line by its
strip, and a transcript is made without speaker names. The bundle script, on the other hand, requires them.

libmpv and the Vulkan loader are not part of the AI payload folder: the bundle script takes them from their own folders
(see *Packaging*).

Licenses of the bundled components: `THIRD_PARTY_NOTICES.md`.

## Using your own models

The player transcribes with whisper large-v3-turbo and translates with Qwen3-4B. The small **Models** menu (on the empty
screen, and next to **Translate** once a file is open) shows the two models in use and lets you pick another one, or
**Import model…** your own:

- **Transcription:** any [whisper.cpp](https://github.com/ggerganov/whisper.cpp) model in its own ggml format (`.bin`):
  tiny, base, small, medium, large, large-v3-turbo, their `.en` versions, distil-whisper conversions, fine-tunes, and
  quantized copies (`q5_0`, `q8_0`...).
- **Translation:** any GGUF model that [llama.cpp](https://github.com/ggerganov/llama.cpp) runs, as long as it is an
  instruct (chat) model: it is asked to translate one subtitle at a time.

SuVtitle reads the file's header to tell which kind it is (never its name) and refuses anything else. An imported model
is used where it is, never copied, and remembered across restarts; one whose file has gone is shown greyed as "not found".
If a model will not load, the analysis stops with a message naming it and SuVtitle goes back to its own model for that
kind. In **Translate all** or **Transcribe all** (the Queue tab), the list goes on with SuVtitle's own model and that
file is done again with it, once (its row says "Redone with the default model"). An analysis made with an imported model
is cached under that model, so it never replaces the default model's subtitles of the same film. How good the subtitles
are depends on the model you choose: a small model is faster and less accurate, and a translation model that does not
know a language translates it badly. Live mode keeps its own presets.

To remove an imported model, click the **×** beside it in the **Models** menu. SuVtitle asks whether to remove it from
the list only, or to delete its file too: the file then goes to the **Recycle Bin**, where you can restore it (on a
drive without one, Windows asks before deleting it). If the file cannot be moved (another program has it open, or its
drive is not connected), nothing changes and SuVtitle says why. A file in one of SuVtitle's own folders (a model it
ships) is never deleted. A model that an analysis is using cannot be removed until that analysis ends or is cancelled;
removing the model you had chosen brings back SuVtitle's own model for that kind.

## Updates

SuVtitle is free software (GPL v3): anyone can build it from this repository and use it at no cost. NEXTALE Audio sells a
ready-made installer (the program, its engines, the AI models fetched for you), updates included. Buying it unlocks
nothing: a build from this repository runs exactly the same program.

- **The check.** Once at each start, in the background once the window is up, SuVtitle reads
  `https://www.nextale-audio.com/updates/suvtitle.json` (the latest version, the address of SuVtitle's page, a line of
  notes). The request names only SuVtitle's version (`User-Agent: SuVtitle/<version>`); each step gives up after 10
  seconds, and any failure is silent.
- **The banner.** A newer version shows a strip above the player, never in full screen or over the picture. **Download**
  opens `https://www.nextale-audio.com/suvtitle?update=<new version>&from=<your version>` in your browser (an address
  built into SuVtitle); the page sends the latest installer's download link to the email address you bought it with, and
  that installer replaces this version. If you build SuVtitle yourself, pull this repository and build it again.
  **Later** hides the strip until the next start; **Skip this version** keeps that version out of the banner (a newer one
  is offered). A prerelease is offered only to a prerelease.
- **The Updates dialog** (the **⋯** menu, or the **Updates…** link of the empty window): the version you have; a newer one
  with its **Download** button (a skipped one too, with **Offer it again**); the **Check for updates when SuVtitle starts**
  box.
- **Turning the check off:** untick that box, or add the line `UPDATECHECK 0` to `%APPDATA%\SuVtitle\app.txt`, where
  SuVtitle also keeps `UPDATESKIP <version>`, the version you skipped.
- A test instance (`--test-instance`) never contacts the site (only a local test server named by `SUVTITLE_TEST_UPDATE_URL`)
  and never opens your browser.

## Tests

```
node tests\web\test_ui.js          the page harness (behaviour checks + mutation sentinels)
node tests\web\test_host_pins.js   source checks of the app's C++
tests\cpp\build_player_smoke.cmd   (and the other build_*.cmd) the C++ smoke tests
py -3 -I tests\cpp\fixround_pins.py   source checks of a fix round
build\x64-Debug\SuVtitleEngine.exe --selftest offline   the engine's offline selftest
```

The Live mode (1.1.0) adds:

```
node tests\web\test_live.js             the Live window's page harness: behaviour, the page <-> app contract, a mutation sentinel per guard
node tests\web\test_host_pins_live.js   source checks of the Live wiring in the app's C++ (bridge.cpp, main.cpp), each proved against mutants
node tests\boundary\check_boundary.js   source scan: fails if a file names an internal of NEXTALE Audio's closed audio-routing core (prints 0 hits)
tests\cpp\build_capture_smoke.cmd       the capture hub's smoke test (--quick: virtual clock, no device; no argument: this PC's devices too)
tests\cpp\capture\capmut.cmd            the capture hub's mutation sentinels (each one-line change must make the smoke fail)
tests\cpp\cmut_live.cmd                 mutation sentinels of the Live parts of the C++ smokes (supervisor, player gate, app config)
tests\e2e\real\run_live_e2e.cmd         the real Live window against a fake engine, driven through DevTools (needs SUV_L2, a work folder)
tests\e2e\real\run_live_real_e2e.cmd    the real Live mode end to end: real engine on the CPU, `wav:` test inputs (needs SUV_L4, SUVTITLE_PAYLOAD, SUVTITLE_TEST_MEDIA)
build\x64-Debug\SuVtitleEngine.exe --selftest-live --cpu   the live path on the real models
build\x64-Debug\SuVtitleEngine.exe --selftest-spk  --cpu   speaker separation on recorded meetings
build\x64-Debug\SuVtitleEngine.exe --selftest-gpuprobe   the start's GPU probe: its parser, its launcher, a host's first STATE (needs SUVTITLE_PAYLOAD; never with --cpu, which hides the GPU from it: it refuses)
py -3 -I tests\cpp\cmut_gpu.py          its mutation sentinels: engine mutants built on build\x64-Debug's objects (SUVTITLE_PAYLOAD)
```

The native video engine (1.1.0) adds (with `SUVTITLE_MPV_DIR` set, see *Building*, and ffmpeg for their clips):

```
tests\cpp\mpv\build_mpv_smoke.cmd     the engine's wrapper (app\MpvPlayer.cpp) against the real libmpv: its ABI, every call, what is heard
tests\cpp\mpv\cmut_mpv.cmd            mutation sentinels of that smoke (each one-line change must make it fail)
tests\cpp\mpv\run_native_real.cmd     the real app as a test instance: the page over mpv's picture, forwarded input, cursor, full screen, drops
node tests\web\test_mpv_pins.js       source checks of the engine's host guards (MpvPlayer.cpp, main.cpp: mpv's locked options, the DLL search, the test hooks), each proved against mutants
tests\cpp\mpvaudio\build_mpvaudio_smoke.cmd   the analysis' sound through libmpv (engine\TrMpvAudio.cpp): the player's timeline on clips built with offsets, sound tracks, seeks, a format change, no network access, the DLL search path, the Vulkan loader fallback (that part needs SUVTITLE_VULKAN_RT)
tests\cpp\mpvaudio\cmut_mpvaudio.cmd          mutation sentinels of that smoke
```

Transcripts (1.1.0) add:

```
tests\cpp\build_timecode_smoke.cmd    the source-timecode reader (BWF / RF64 / BW64 + iXML, MOV / MP4, MXF): stamps, a byte budget, fuzzing (+ an AddressSanitizer build; --no-heavy skips it)
tests\cpp\build_transcript_smoke.cmd  the speaker transcript's Word file, then tests\cpp\transcript_check.py reads it back as a ZIP
tests\cpp\cmut_u3.cmd                 mutation sentinels of the transcript's C++ (the wire, the records, the turns, the core, the Word file, the timecode, the supervisor)
node tests\cpp\make_tc_fixtures.js <dir>   media files that carry a timecode, made with ffmpeg, for build_timecode_smoke.cmd
```

The update banner adds:

```
node tests\web\test_upd.js            the banner, the Updates dialog, a mutation sentinel per rule (test_ui.js runs it too)
tests\cpp\build_update_smoke.cmd      app\NextaleUpdate.cpp (the version rule, the address guard, the manifest) and app.txt's update lines, against a loopback server: no test contacts the site
tests\cpp\cmut_upd.cmd                mutation sentinels of that smoke
```

The light installer adds (Inno Setup 6's `ISCC.exe` needed; no administrator rights, no internet):

```
node tests\installer\run_installer_kit.js   the light installer's model download (installer\models_download.iss) in throwaway setups against a loopback server: downloads, errors, exit codes, a model in use, two drives (--mutants: its mutation sentinels; --big: a model above 2 GiB)
node installer\gen_iss_registry.js --check  the installer script against the sources it repeats; compiles both variants
```

- The engine's selftests need a build (`tools\buildlock.cmd x64-Debug`; the Release executable runs them too).
  `SuVtitleEngine.exe --help` lists the other offline ones.
- `--selftest-live` and `--selftest-spk` run the real models, so they need the payload (`SUVTITLE_PAYLOAD`) and the
  maintainer's recordings (`SUVTITLE_TEST_MEDIA`, below); run one model test at a time and leave the machine calm.
- The two end-to-end kits start a **test instance** (`--test-instance <tag>`) with its own profile folder, so they
  never touch your settings; only a test instance accepts the `wav:` inputs that replay a recording as a strip.
- The JavaScript tests need [Node.js](https://nodejs.org) 18 or later. A few C++ test tools are Python 3 scripts
  (standard library only); the `.cmd` scripts find Python with `tools\pyenv.cmd`: `SUVTITLE_PYTHON`, else the
  `py` launcher, else `python`.
- `build_tracks_smoke.cmd` also builds an AddressSanitizer fuzz variant, which needs Visual Studio's *C++
  AddressSanitizer* component. `build_tracks_smoke.cmd --no-heavy` skips it.
- `tests\e2e\make_media.js` generates its clips with ffmpeg (`ffmpeg` and `ffprobe` on `PATH`, or the `FFMPEG`
  and `FFPROBE` variables).
- Some checks use the maintainer's own speech recordings, which are not published: `SUVTITLE_TEST_MEDIA` (and
  `SUVTITLE_TEST_FILM` for a long film). Without them, those checks print *NOT CHECKED* and the run still passes.
- The end-to-end kits in `tests\e2e\` open WebView2 windows, preferably on a second screen.

Code comments cite the maintainers' design notes and review rounds (`docs\…`, `SuVtitle.md`, IDs such as
`RV2-1`). Those notes are not part of this repository.

## Packaging

```
node deploy\make_bundle.js --payload <payload folder> --mpv <libmpv folder> --vulkan-rt <VulkanRT Components folder>
ISCC installer\SuVtitle_Setup.iss
```

The bundle script assembles `deploy\out\SuVtitle\` from a Release build and a payload folder (the
`SUVTITLE_PAYLOAD` layout above, `sherpa\` included; `--payload` can be left out when that variable is set), the native
video engine (`--mpv`, else `SUVTITLE_MPV_DIR`: a folder holding `libmpv-2.dll`) and Khronos' Vulkan loader (`--vulkan-rt`,
else `SUVTITLE_VULKAN_RT`: the extracted `VulkanRT-X64-<version>-Components` folder; shipped ONLY in `mpv\vkfallback\`, never
next to an exe, where it would beat the PC's own loader), checks every
binary's dependencies (the C++ runtime DLLs of whisper, llama and sherpa-onnx are copied next to them from the Visual
Studio redistributable) and writes a SHA-256 manifest; `--verify` re-checks an assembled bundle against it. The installer
([Inno Setup](https://jrsoftware.org/isinfo.php) 6) packs that folder and adds a **SuVtitle Live** shortcut
(`SuVtitle.exe --live`) to the Start menu.

The installer is **one file of about 60 MB: it does not carry the models**. When you click Install, it downloads from
Hugging Face the models that the installation folder does not already hold with their exact size (about 5.9 GB on a
first install), from the URLs pinned in `installer\models.json`, and checks the SHA-256 of each one; an update whose
models are in place downloads nothing. It checks the free disk space first (when `%TEMP%` is on another drive than the
installation, that drive needs room for the largest model only: each model moves to the installation drive right after its
download). When a download fails (no connection, a server error, a file that does not match its SHA-256) it says why and
offers Retry; nothing has been installed at that point. A model gets its final name only once it is complete (it is
written as `<file>.part`, then renamed in one step, the previous model staying until then), so an interrupted
installation never leaves a damaged model behind. To check the installed models byte for byte, run the installer with
`/VERIFYMODELS` (about 11 seconds per GB): a damaged one is downloaded again.

A silent install (`/VERYSILENT`) downloads the same way. With `/SUPPRESSMSGBOXES` an error answers Cancel instead of
asking, and the exit code tells what happened:

| Exit code | Meaning |
|---|---|
| 0 | installed, every model in place |
| 7 | the models could not be downloaded (no connection, a server error, a SHA-256 mismatch, not enough disk space): nothing was installed |
| 10 | installed, but a model could not be put in place (in use, or damaged after its download): run the installer again, it fetches only what is missing |
| 11 | installed, but the permissions of a model could not be reset: other Windows accounts may not read it (`icacls "<installation folder>\models" /reset /T` fixes it) |

The other codes are Inno Setup's (2: cancelled before the installation started).

An offline installer with the models inside (about 6 GB: `SuVtitle_v<version>_Setup_Full.exe` and its `-1.bin`, to keep
together) is built from a full bundle, whose models must be the files `installer\models.json` pins (the bundle script and
the installer both check them):

```
node deploy\make_bundle.js --full --out deploy\out\full --payload <payload folder> --mpv <libmpv folder> --vulkan-rt <VulkanRT Components folder>
ISCC /DFull /DBundleDir=..\deploy\out\full\SuVtitle installer\SuVtitle_Setup.iss
```

`node installer\gen_iss_registry.js --check` checks the installer script against the sources it repeats (and compiles
both variants when Inno Setup's `ISCC.exe` is found). Before publishing a light installer, `node installer\check_models.js`
asks Hugging Face whether each pinned URL still serves the file `installer\models.json` pins: one HEAD request per model,
nothing downloaded (exit code 1 when one does not).

## License

SuVtitle is free software: you can redistribute it and/or modify it under the terms of the **GNU General
Public License, version 3 or (at your option) any later version**. See `LICENSE`. The components it uses
keep their own licenses: see `THIRD_PARTY_NOTICES.md`.

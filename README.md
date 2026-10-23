# SuVtitle

**SuVtitle** is a video and audio player for Windows that **transcribes and translates what is said in a
file, offline, and shows it as subtitles that stay ahead of the picture**. Speech recognition runs on
[whisper.cpp](https://github.com/ggerganov/whisper.cpp) and translation on
[llama.cpp](https://github.com/ggerganov/llama.cpp) with a Qwen model, on the GPU (Vulkan) or the CPU.
Nothing leaves your computer.

- Start watching after a short load: the analysis runs ahead of the playhead, follows your seeks and is
  cached, and one `.srt` per language is written next to the file as it goes.
- A real player: queue, folders, resume, full screen, keyboard shortcuts, default-player registration.
- Subtitles from everywhere: SuVtitle's own, `.srt` / `.vtt` / `.ass` files, and the film's embedded
  text tracks; choose the sound track of a multi-audio film (the analysis follows the track you hear).
- A subtitle editor (text and timing) and a subtitle style panel.

SuVtitle is published by NEXTALE. It began as the file-mode player of AudioPB Translator. See
`CHANGELOG.md` for what is in each version.

## Requirements

- Windows 10 or 11, x64.
- The Microsoft Edge **WebView2 Runtime** (built into Windows 11; on Windows 10 install the Evergreen runtime).
- For GPU inference, a graphics driver that provides Vulkan (`vulkan-1.dll`). The Vulkan builds of
  whisper.cpp and llama.cpp load it even when they fall back to the CPU.

## Repository layout

| Folder | Content |
|---|---|
| `app/` | `SuVtitle.exe`: the window (Win32 + WebView2), the player core, subtitles, the engine supervisor |
| `app/web/` | the page the window shows (HTML/CSS/JS, served locally by the app) |
| `engine/` | `SuVtitleEngine.exe`: drives whisper-server and llama-server, runs the file analysis |
| `common/` | `TrPipe.h`, the protocol between the app and the engine |
| `tests/` | C++ smoke tests and mutants (`cpp/`), the page harness (`web/`), page and real-app end-to-end kits (`e2e/`) |
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

## The AI payload

The engine needs the whisper.cpp and llama.cpp server binaries and three models. They are not in this
repository. An installed SuVtitle has them next to the executable:

```
<app>\engine\whisper\   whisper-server.exe + its ggml DLLs (Vulkan build)
<app>\engine\llama\     llama-server.exe + its ggml DLLs (Vulkan build)
<app>\models\           ggml-large-v3-turbo.bin
                        Qwen3-4B-Instruct-2507-Q5_K_M.gguf
                        qwen2.5-1.5b-instruct-q4_k_m.gguf
```

- whisper.cpp and llama.cpp: the Windows Vulkan release builds of each project.
- `ggml-large-v3-turbo.bin`: the whisper.cpp model repository on Hugging Face (`ggerganov/whisper.cpp`).
- The Qwen models: GGUF quantizations of `Qwen3-4B-Instruct-2507` (Q5_K_M) and `Qwen2.5-1.5B-Instruct`
  (q4_k_m) on Hugging Face.

A development build has nothing next to it, so give it a payload folder in the environment variable
`SUVTITLE_PAYLOAD`, laid out without the `engine\` level:

```
<payload>\whisper\   <payload>\llama\   <payload>\models\
```

The engine looks, in order, at the `WHISPERBIN`, `LLAMABIN` and `MODELS` lines of `%APPDATA%\SuVtitle\engine.txt`,
then at the folders next to its executable, then at `SUVTITLE_PAYLOAD`. A payload with a missing part is reported
as an error naming that part, never completed from another one.

## Tests

```
node tests\web\test_ui.js          the page harness (behaviour checks + mutation sentinels)
node tests\web\test_host_pins.js   source checks of the app's C++
tests\cpp\build_player_smoke.cmd   (and the other build_*.cmd) the C++ smoke tests
py -3 -I tests\cpp\fixround_pins.py   source checks of a fix round
build\x64-Debug\SuVtitleEngine.exe --selftest offline   the engine's offline selftest
```

- The engine's selftests need a build (`tools\buildlock.cmd x64-Debug`; the Release executable runs them too).
  `SuVtitleEngine.exe --help` lists the other offline ones.
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
node deploy\make_bundle.js --payload <payload folder>
ISCC installer\SuVtitle_Setup.iss
```

The bundle script assembles `deploy\out\SuVtitle\` from a Release build and a payload folder (the
`SUVTITLE_PAYLOAD` layout above; `--payload` can be left out when that variable is set), checks every
binary's dependencies and writes a SHA-256 manifest. The installer ([Inno Setup](https://jrsoftware.org/isinfo.php)
6) packs that folder. The installer is large, because of the models.

## License

SuVtitle is free software: you can redistribute it and/or modify it under the terms of the **GNU General
Public License, version 3 or (at your option) any later version**. See `LICENSE`. The components it uses
keep their own licenses: see `THIRD_PARTY_NOTICES.md`.

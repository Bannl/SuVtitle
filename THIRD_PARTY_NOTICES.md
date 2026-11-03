# Third-party components

SuVtitle's own source code is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
This repository contains **only SuVtitle's own code**. It does not contain the components below: the build
fetches the WebView2 SDK; the installer bundles the AI runtime, the native video engine (libmpv) and a Vulkan loader, and
downloads the AI models from their publishers' Hugging Face pages while it installs (the offline "full" installer bundles
them instead; `installer\models.json` names each file, its source and its SHA-256). Each keeps its own license.

| Component | Used for | How SuVtitle gets it | License |
|---|---|---|---|
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) (`whisper-server` + ggml) | speech recognition server | prebuilt Vulkan build, bundled by the installer under `engine\whisper\` | MIT |
| [llama.cpp](https://github.com/ggerganov/llama.cpp) (`llama-server` + ggml) | translation server | prebuilt Vulkan build, bundled under `engine\llama\` | MIT |
| Whisper `large-v3-turbo`, `small` and `base` weights (OpenAI), ggml conversions by the whisper.cpp project | transcription models (films: `large-v3-turbo`; Live mode: `small`, `base`, and `large-v3-turbo` for the quality preset) | downloaded by the installer from `ggerganov/whisper.cpp` on Hugging Face into `models\ggml-large-v3-turbo.bin`, `models\ggml-small.bin`, `models\ggml-base.bin` | MIT |
| Qwen3-4B-Instruct-2507 (Qwen team, Alibaba Cloud), GGUF quantization `Q5_K_M` by Unsloth | translation model | downloaded by the installer from `unsloth/Qwen3-4B-Instruct-2507-GGUF` into `models\Qwen3-4B-Instruct-2507-Q5_K_M.gguf` | Apache-2.0 |
| Qwen2.5-1.5B-Instruct (Qwen team, Alibaba Cloud), GGUF `q4_k_m` | smaller translation model (fallback) | downloaded by the installer from `Qwen/Qwen2.5-1.5B-Instruct-GGUF` into `models\qwen2.5-1.5b-instruct-q4_k_m.gguf` | Apache-2.0 |
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (`sherpa-onnx-c-api.dll`) | runtime of the speaker model (Live mode and file transcripts: which voice said a line) | prebuilt, bundled under `engine\sherpa\`; `SuVtitleEngine.exe` loads it at run time, it is never linked | Apache-2.0 |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) (`onnxruntime.dll`, `onnxruntime_providers_shared.dll`) | runs the speaker model for sherpa-onnx | the copy that comes with the sherpa-onnx release, bundled under `engine\sherpa\` | MIT |
| CAM++ speaker-embedding model of the [3D-Speaker](https://github.com/modelscope/3D-Speaker) project (ModelScope `iic/speech_campplus_sv_zh_en_16k-common_advanced`), ONNX build | tells the speakers of a Live strip apart | downloaded by the installer from `csukuangfj/speaker-embedding-models` (published as `3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx`, which SuVtitle also accepts) into `models\3dspeaker_campplus_zh_en_advanced.onnx` | Apache-2.0 as declared for the source model; **not stated by the host of the ONNX build** — see the note below |
| Microsoft WebView2 SDK | the window's web view | NuGet package `Microsoft.Web.WebView2` downloaded at build time; `WebView2Loader.dll` is shipped next to `SuVtitle.exe` | Microsoft WebView2 SDK license |
| Microsoft Visual C++ runtime DLLs | runtime of the whisper, llama and sherpa-onnx / ONNX Runtime binaries | copied next to those binaries by the bundle script | Microsoft Visual C++ Redistributable license |
| [mpv](https://mpv.io) (`libmpv-2.dll`) | the native video engine: plays every file under the page; the file analysis hears the files through it | a libmpv development build for Windows x64 (shinchiro's [mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake), build `20261021`, mpv git `e471496c56`), bundled under `mpv\`; `SuVtitle.exe` and `SuVtitleEngine.exe` load it at run time, it is never linked | mpv's own code: GPL-2.0-or-later; the shipped `libmpv-2.dll` as a whole is distributed under GPL-3.0-or-later, because the FFmpeg inside it is (see *Where the sources of the shipped binaries are*) |
| [FFmpeg](https://ffmpeg.org) and the other libraries built into `libmpv-2.dll` | demuxing, decoding, resampling (FFmpeg); rendering (libplacebo); the rest as listed below | statically linked into `libmpv-2.dll` by the same build | FFmpeg: GPL-3.0-or-later as built (`--enable-gpl --enable-version3`); the others: their own licenses (see *Inside libmpv-2.dll*) |
| Vulkan loader (`vulkan-1.dll`) | lets libmpv and whisper-server start on a PC whose graphics driver installed no Vulkan loader (they import it even to run on the CPU) | the PC's own loader (`System32`) when there is one; otherwise the copy bundled under `mpv\vkfallback\`: Khronos' [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) 1.4.363 from LunarG's VulkanRT redistributable `VulkanRT-X64-1.4.363.0-Components` (signed by LunarG, Inc.), shipped with its `VulkanRT-License.txt` | Apache-2.0, with MIT-licensed parts (`VulkanRT-License.txt`) |
| Windows Media Foundation, Extended Linguistic Services, the Windows Audio Session API (WASAPI), WebView2 Runtime | the analysis' fallback decoder (when libmpv cannot be used) and the sound copy of the page's own player, Chinese script conversion, the Live mode's audio capture, rendering | Windows components; not redistributed | — |

Fonts: SuVtitle ships no font file; it uses Windows system fonts (Bahnschrift, Segoe UI and the system CJK faces).

## Inside libmpv-2.dll (as the build declares them)

`libmpv-2.dll` is ONE file that contains mpv and every library below, statically linked by shinchiro's build scripts.

- mpv `v0.41.0-1227-ge471496c5` (built 21/10/2026), features enabled by the build, as mpv itself reports them: amf
  build-date cplugins cuda-hwaccel cuda-interop d3d-hwaccel d3d11 d3d9-hwaccel debug direct3d dos-paths dvda dvdnav
  dxgi-debug-d3d11 egl-angle egl-angle-win32 ffmpeg ffnvcodec gl gl-dxinterop gl-dxinterop-d3d9 gl-win32 glob glob-win32
  **gpl** iconv javascript jpeg lcms2 libarchive libass libavdevice libbluray libcurl libplacebo luajit openal rubberband
  rubberband-3 sdl2-gamepad shaderc sixel spirv-cross subrandr uchardet vaapi vaapi-win32 vapoursynth vector
  vk-khr-display vulkan wasapi win32 win32-desktop win32-executable win32-smtc win32-threads zimg zimg-st428 zlib.
- FFmpeg `N-127893-g9a861e968` (libavcodec 63.2.100, libavformat 63.11.101, libavfilter 12.5.100, libavutil 61.15.100,
  libswresample 7.3.101, libswscale 10.2.100), with these external libraries found in the binary: dav1d, libaom,
  libvpx, libdavs2, libuavs3d, libjxl, libwebp, Opus, Vorbis, Speex, libmodplug, x264, ZVBI, SRT, libssh.
- libplacebo `v7.377.0`; libass with HarfBuzz and FriBidi 1.0.17; LuaJIT 2.1 and MuJS (mpv's scripting, which
  SuVtitle switches off); libarchive, libbluray, libdvdnav, uchardet, Rubber Band, Little CMS, OpenAL Soft, shaderc,
  SPIRV-Cross, zimg, zlib, libjpeg, libcurl, SDL2, libsixel, subrandr.
- The exact version of each, and the upstream repository it is built from, are fixed by the build scripts at that build:
  `packages/*.cmake` of [mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake).

## Where the sources of the shipped binaries are

- mpv: <https://github.com/mpv-player/mpv>, commit `e471496c56` (version `v0.41.0-1227-ge471496c5`).
- FFmpeg: <https://git.ffmpeg.org/ffmpeg.git> (mirror <https://github.com/FFmpeg/FFmpeg>), commit `9a861e968`
  (version `N-127893-g9a861e968`).
- The build recipe that made `libmpv-2.dll` (and the source of every library inside it): shinchiro's
  [mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake), GitHub Actions build `20261021`, archive
  `mpv-dev-x86_64-20261021-git-e471496c56.7z` (SHA-256 `efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633`);
  the shipped `libmpv-2.dll` is that archive's (SHA-256 `04e4d27dc385dc18ddd93fbe69c75e635b864f3c6ec53a9122df54c2f7842f4a`).
- The Vulkan loader: <https://github.com/KhronosGroup/Vulkan-Loader>, tag `vulkan-sdk-1.4.363.0`; the redistributable is
  LunarG's `VulkanRT-X64-1.4.363.0-Components.zip` (<https://vulkan.lunarg.com>); the shipped `vulkan-1.dll` has SHA-256
  `e1fcfc9489beefa6a6d9d11d6c7517f6a2b7f16e0d3fb8f4103bee0210f314a3`.
- The exact corresponding source archives of the shipped `libmpv-2.dll` (mpv, FFmpeg, every library inside it and the
  build recipe) and of the Vulkan loader are published at <https://www.nextale-audio.com/suvtitle/source>.
- SuVtitle is GPL-3.0-or-later; mpv is GPL-2.0-or-later and FFmpeg as built GPL-3.0-or-later (configured with
  `--enable-gpl --enable-version3`: `packages/ffmpeg.cmake` of mpv-winbuild-cmake at commit `1f8d2e76255a`), so
  `libmpv-2.dll` as shipped is GPL-3.0-or-later, the same license as SuVtitle. Whoever distributes the installer must keep
  the corresponding source of these binaries available (GPL-3.0 section 6): keep a copy of those exact sources, rather
  than relying on the links above staying up.

## Where the licenses above were read (26/10/2026)

- sherpa-onnx, Apache License 2.0: <https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE>
- ONNX Runtime, MIT License (Copyright (c) Microsoft Corporation): <https://github.com/microsoft/onnxruntime/blob/main/LICENSE>.
  Its binary releases include components that have notices of their own: <https://github.com/microsoft/onnxruntime/blob/main/ThirdPartyNotices.txt>.
- Whisper weights: the README of <https://github.com/openai/whisper> says "Whisper's code and model weights are released under the
  MIT License"; the ggml conversions are published at <https://huggingface.co/ggerganov/whisper.cpp>, whose page declares `license: mit`.
- 3D-Speaker (code), Apache License 2.0: <https://github.com/modelscope/3D-Speaker/blob/main/LICENSE>. ModelScope's metadata for the
  source model `iic/speech_campplus_sv_zh_en_16k-common_advanced` reads `"License": "Apache License 2.0"`
  (<https://www.modelscope.cn/api/v1/models/iic/speech_campplus_sv_zh_en_16k-common_advanced>).
- Qwen3-4B-Instruct-2507, Apache License 2.0 (read 03/11/2026): the GGUF the installer downloads is Unsloth's
  (<https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF>), whose page declares `license: apache-2.0`, as does the
  original model's (<https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507>).
- Qwen2.5-1.5B-Instruct GGUF, Apache License 2.0 (read 03/11/2026): <https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF>
  declares `license: apache-2.0`.

**Note on the speaker model.** The file SuVtitle loads is an ONNX build of that model, published on Hugging Face as
`csukuangfj/speaker-embedding-models` (<https://huggingface.co/csukuangfj/speaker-embedding-models>). That page has no model card and
states no license, so **the license of the ONNX copy itself is unconfirmed**; the Apache-2.0 above is the source model's. Confirm it
with the model's publishers before you redistribute a package that contains the file. Without the file (and without
`engine\sherpa\`), SuVtitle still runs: the Live mode then labels each line by its strip instead of by its speaker.

Before redistributing a binary package, check each model's license and terms on the page you downloaded it from:
quantized GGUF files are published by different people, and their model cards are what binds you. The installer
NEXTALE Audio distributes contains no model: on each PC it downloads them from the pages above, pinned to a commit and checked
against their SHA-256 (`installer\models.json`). The offline "full" installer (`ISCC /DFull`) does contain them.

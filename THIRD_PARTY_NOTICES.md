# Third-party components

SuVtitle's own source code is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
This repository contains **only SuVtitle's own code**. It does not contain the components below: the build
fetches the WebView2 SDK, and the installer bundles the AI runtime and models. Each keeps its own license.

| Component | Used for | How SuVtitle gets it | License |
|---|---|---|---|
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) (`whisper-server` + ggml) | speech recognition server | prebuilt Vulkan build, bundled by the installer under `engine\whisper\` | MIT |
| [llama.cpp](https://github.com/ggerganov/llama.cpp) (`llama-server` + ggml) | translation server | prebuilt Vulkan build, bundled under `engine\llama\` | MIT |
| Whisper `large-v3-turbo` weights (OpenAI), ggml conversion by the whisper.cpp project | transcription model | `models\ggml-large-v3-turbo.bin` | MIT |
| Qwen3-4B-Instruct-2507 (Qwen team, Alibaba Cloud), GGUF quantization `Q5_K_M` | translation model | `models\Qwen3-4B-Instruct-2507-Q5_K_M.gguf` | Apache-2.0 |
| Qwen2.5-1.5B-Instruct (Qwen team, Alibaba Cloud), GGUF `q4_k_m` | smaller translation model (fallback) | `models\qwen2.5-1.5b-instruct-q4_k_m.gguf` | Apache-2.0 |
| Microsoft WebView2 SDK | the window's web view | NuGet package `Microsoft.Web.WebView2` downloaded at build time; `WebView2Loader.dll` is shipped next to `SuVtitle.exe` | Microsoft WebView2 SDK license |
| Microsoft Visual C++ runtime DLLs | runtime of the whisper/llama binaries | copied next to those binaries by the bundle script | Microsoft Visual C++ Redistributable license |
| Vulkan loader (`vulkan-1.dll`) | GPU inference | provided by the graphics driver; not redistributed | — |
| Windows Media Foundation, Extended Linguistic Services, WebView2 Runtime | media decoding, Chinese script conversion, rendering | Windows components; not redistributed | — |

Fonts: SuVtitle ships no font file; it uses Windows system fonts (Bahnschrift, Segoe UI and the system CJK faces).

Before redistributing a binary package, check each model's license and terms on the page you downloaded it from:
quantized GGUF files are published by different people, and their model cards are what binds you.

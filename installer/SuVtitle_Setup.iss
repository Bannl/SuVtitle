; ==============================================================================
; SuVtitle - Inno Setup Installer Script
; NEXTALE
; ------------------------------------------------------------------------------
; SuVtitle is the subtitle PLAYER that was split out of AudioPB on 23/10/2026: a normal
; Windows application (a window and a taskbar button, no tray icon) that plays a video
; or audio file and transcribes / translates it OFFLINE into subtitles that stay ahead
; of the picture, with the full subtitle tooling (.srt / .vtt / .ass, the film's own
; tracks, an editor, transcript export). Since 1.1.0 it also has a LIVE mode (its own
; window, opened by the player's Live button or by the "SuVtitle Live" shortcut below):
; live transcription + translation of an input, of an output or of one application. It
; ships, beside the app, the local AI engines driven as child processes by SuVtitleEngine.exe
; -- whisper.cpp (transcription), llama.cpp / Qwen (translation) -- and sherpa-onnx (which
; tells the speakers of a live strip apart, loaded by the engine itself), and it needs their
; models: about 5.9 GB. The AI runs on 127.0.0.1: nothing SuVtitle hears, shows or transcribes leaves the machine. What does
; use the network: this (LIGHT) installer downloads the models, and the app asks www.nextale-audio.com once per start
; whether a newer version is published (README, "Updates"; it can be turned off).
;
; TWO VARIANTS of this one script (the director's decision, 03/11/2026):
;   * LIGHT -- the default, the installer NEXTALE distributes: ONE file, SuVtitle_v<version>_Setup.exe, with everything
;     but the AI models. When the user clicks Install, [Code] (installer\models_download.iss, #included at the end; the
;     test kit tests\installer\ runs the same file) downloads from Hugging Face, one at a time, every model that
;     {app}\models lacks, from the URLs installer\models.json pins to a commit, each checked by its SHA-256, and puts each
;     in place through a .part file and one rename, so a model is never left half-written under its final name. An
;     UPDATE whose models are all in place (same file, same size) downloads nothing and needs no network; /VERIFYMODELS
;     also checks their SHA-256 and downloads a damaged one again. Exit codes of its own: 7 (Inno's "cannot proceed": a
;     silent install that could not get the models; nothing installed), 10 (installed, but a model could not be put in
;     place), 11 (installed, but the permissions of a model could not be reset). Compiled from a LIGHT bundle
;     (deploy\make_bundle.js, its default: no models\ in it).
;   * FULL -- ISCC /DFull: the offline installer, the models embedded (the 1.1.0 test installer's design):
;     SuVtitle_v<version>_Setup_Full.exe + SuVtitle_v<version>_Setup_Full-1.bin (about 6 GB, DiskSpanning: keep the two
;     together). Compiled from a FULL bundle (make_bundle.js --full): each models.json file must be there with THAT size
;     (read on disk) and its manifest row must give THAT SHA-256 and size, so the full installer never embeds other bytes
;     than the light one downloads (`make_bundle.js --verify --out deploy\out\full` re-hashes the files themselves).
;
; The models -- installer\models.json is the ONE list (file, size, SHA-256, the pinned Hugging Face URL, license); the
; director's decision, 23/10/2026 + 26/10/2026 for the live mode: ggml-large-v3-turbo.bin (transcription of films; the
; final model of the live quality preset), ggml-small.bin and ggml-base.bin (the live presets balanced / light),
; Qwen3-4B-Instruct-2507-Q5_K_M.gguf (translation), qwen2.5-1.5b-instruct-q4_k_m.gguf (translation fallback) and
; 3dspeaker_campplus_zh_en_advanced.onnx (the speaker model). A missing 4B is INVISIBLE at runtime -- the engine falls
; back to the 1.5B without a word -- and a missing live model or speaker runtime degrades the live mode just as quietly,
; so the light installer fetches every one of them and the full one refuses a bundle that lacks one. NOT installed:
; ggml-large-v3.bin (quality "max" only; the engine says so when it is absent -- make_bundle.js --full --with-large-v3
; puts it in a full bundle).
;
; What gets installed ({app} = %ProgramFiles%\NEXTALE\SuVtitle):
;   * SuVtitle.exe        - the window (Win32 + WebView2): player, Live window, queue, subtitles, editor
;   * SuVtitleEngine.exe  - the AI helper SuVtitle.exe spawns and supervises (it also does the live capture)
;   * WebView2Loader.dll, web\ (the pages SuVtitle.exe serves from <exe>\web), LICENSE, THIRD_PARTY_NOTICES.md
;   * engine\whisper\, engine\llama\ (the servers + ggml backends + the VC++ CRT, app-local)
;   * engine\sherpa\ (sherpa-onnx + ONNX Runtime + the VC++ CRT, app-local)
;   * mpv\libmpv-2.dll (1.1.0: the native video engine -- it plays every file -- and the analysis' decoder; GPL build)
;   * mpv\vkfallback\vulkan-1.dll + VulkanRT-License.txt (1.1.0: Khronos' Vulkan loader, LunarG VulkanRT, used ONLY on a PC
;     whose graphics driver installed none: libmpv and whisper-server import vulkan-1.dll and would not even start)
;   * models\ (light: downloaded at install time, the missing ones only; full: embedded)
;   * Start menu: "SuVtitle", "SuVtitle Live" (SuVtitle.exe --live) and the uninstaller
;
; SOURCE = the assembled, dumpbin-verified bundle, produced by deploy\make_bundle.js. Build the Release, assemble, THEN
; compile this script:
;   LIGHT  node deploy\make_bundle.js --payload <your AI payload folder> --mpv <dir> --vulkan-rt <dir>     (-> deploy\out\SuVtitle)
;          ISCC.exe installer\SuVtitle_Setup.iss
;   FULL   node deploy\make_bundle.js --full --out deploy\out\full --payload <...> --mpv <dir> --vulkan-rt <dir>
;          ISCC.exe /DFull /DBundleDir=..\deploy\out\full\SuVtitle installer\SuVtitle_Setup.iss
; (--payload can be left out when SUVTITLE_PAYLOAD is set; see make_bundle.js.) A light compile refuses a bundle that has
; models, a full compile refuses one without them: a bundle is one or the other.
; ISCC.exe is Inno Setup 6's compiler: on PATH, or in the standard install folders -- all users:
; %ProgramFiles(x86)%\Inno Setup 6\ISCC.exe, current user: %LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe (call it by that full
; path when it is not on PATH). Call it from cmd or PowerShell, NOT Git Bash: it mangles the /Q-style switches.
; Overridable: /DMyAppVersion=1.1.1   /DBundleDir=<path of another bundle, absolute or relative to installer\>
;              /DFull                 (the offline variant, see above)
;              /DAllowDebugBundle     (compile a bundle made from a Debug build: for tests only)
;              /DSliceSize=2000000000 (FULL only: cut the data into ~2 GB files; see DiskSpanning below)
; MEASURED 03/11/2026 (Inno 6.7.1, the 1.1.0 bundles, ISCC at idle priority): LIGHT 20-35 s -> one 64,082,329-byte
; Setup.exe (0.29 GiB packed solid); FULL 340 s -> a 2,093,345-byte Setup_Full.exe + a 6,034,493,524-byte Setup_Full-1.bin.
;
; The player registration ([Registry], HKLM) and the models block are GENERATED by installer\gen_iss_registry.js, from the
; C++ lists and from installer\models.json; `node installer\gen_iss_registry.js --check` fails when this file drifts from
; its sources (those blocks, the version, the mutex, the payload layout, the two variants -- it compiles both ...).
; How the light installer BEHAVES (downloads, errors, exit codes, a model in use, another drive...) is tested by
; `node tests\installer\run_installer_kit.js` (throwaway setups around models_download.iss and a loopback server).
;
; Install is machine-wide (HKLM, Program Files) -> ADMIN. Uninstall removes the machine keys
; itself and runs `SuVtitle.exe --unregister-player` AS THE ORIGINAL USER ([Code], ExecAsOriginalUser) for
; the per-user (HKCU) ones. SuVtitle's data (%APPDATA%\SuVtitle, %LOCALAPPDATA%\SuVtitle: settings, queue,
; the analyses cache) is the user's and is left in place.
; ==============================================================================

#define MyAppName "SuVtitle"
#ifndef MyAppVersion
  ; ONE number for the installer; app/resource.h, engine/EngineVersion.h and CMakeLists.txt carry the same
  ; (`gen_iss_registry.js --check` compares them). Overridable with /DMyAppVersion=...
  #define MyAppVersion "1.1.0"
#endif
#define MyAppPublisher "NEXTALE"
#define MyAppURL "https://www.nextale-audio.com/"
#define MyAppExe "SuVtitle.exe"
; Installer AppId (the uninstall identity): unique to SuVtitle, constant across versions. Not AudioPB's.
#define MyAppId "{6FF1C973-F626-4B99-8A05-6A37BB152543}"
; The application's single-instance mutex (app/main.cpp kMutexName): while it exists, Setup and Uninstall
; refuse to touch a running SuVtitle instead of failing on locked files.
#define MyAppMutex "SuVtitle.SingleInstance"

; >>> models (generated) >>>
; GENERATED by installer\gen_iss_registry.js from installer\models.json, the ONE list of the AI models (file, size, SHA-256,
;   the Hugging Face URL pinned to a commit, license): do not edit by hand -- `node installer\gen_iss_registry.js --write`
;   rewrites it and `--check` fails when the two drift. LIGHT: [Code] downloads from these URLs what {app}\models lacks and
;   Inno checks each SHA-256; FULL (/DFull): the bundle's manifest must list these files with these SHA-256 and sizes.
#define ModelCount 6
#define ModelsTotalBytes 6295224687
#dim ModelFile[6]
#dim ModelSize[6]
#dim ModelSha256[6]
#dim ModelUrl[6]
#define ModelFile[0] "ggml-large-v3-turbo.bin"
#define ModelSize[0] "1624555275"
#define ModelSha256[0] "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"
#define ModelUrl[0] "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo.bin"
#define ModelFile[1] "ggml-small.bin"
#define ModelSize[1] "487601967"
#define ModelSha256[1] "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"
#define ModelUrl[1] "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.bin"
#define ModelFile[2] "ggml-base.bin"
#define ModelSize[2] "147951465"
#define ModelSha256[2] "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"
#define ModelUrl[2] "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-base.bin"
#define ModelFile[3] "Qwen3-4B-Instruct-2507-Q5_K_M.gguf"
#define ModelSize[3] "2889514080"
#define ModelSha256[3] "5bde5e9d883622acb02bf77fe7dcbc56a8b9a9ad4be78a72ca23a532658b4ecb"
#define ModelUrl[3] "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/a06e946bb6b655725eafa393f4a9745d460374c9/Qwen3-4B-Instruct-2507-Q5_K_M.gguf"
#define ModelFile[4] "qwen2.5-1.5b-instruct-q4_k_m.gguf"
#define ModelSize[4] "1117320736"
#define ModelSha256[4] "6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e"
#define ModelUrl[4] "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/91cad51170dc346986eccefdc2dd33a9da36ead9/qwen2.5-1.5b-instruct-q4_k_m.gguf"
#define ModelFile[5] "3dspeaker_campplus_zh_en_advanced.onnx"
#define ModelSize[5] "28281164"
#define ModelSha256[5] "aa3cfc16963a10586a9393f5035d6d6b57e98d358b347f80c2a30bf4f00ceba2"
#define ModelUrl[5] "https://huggingface.co/csukuangfj/speaker-embedding-models/resolve/0743f301363dec56491a490f6d6cbc9d67f9a3bf/3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx"
; <<< models (generated) <<<

; The assembled bundle root: relative to THIS .iss (installer\), or absolute (/DBundleDir=...).
#ifndef BundleDir
  #define BundleDir "..\deploy\out\SuVtitle"
#endif
#if Copy(BundleDir, 2, 1) == ":"
  #define BundleAbs BundleDir
#else
  #define BundleAbs AddBackslash(SourcePath) + BundleDir
#endif

; Fail fast if the bundle was not assembled, or is not what it should be.
#if !FileExists(BundleAbs + "\manifest.txt")
  #error Bundle not found. Run `node deploy\make_bundle.js` first (see the header).
#endif
#if !FileExists(BundleAbs + "\app\SuVtitle.exe")
  #error Bundle incomplete (SuVtitle.exe missing). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\SuVtitleEngine.exe")
  #error Bundle incomplete (SuVtitleEngine.exe missing). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\web\index.html")
  #error Bundle incomplete (the page, web\index.html, is missing). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\engine\whisper\whisper-server.exe")
  #error Bundle incomplete (engine\whisper\whisper-server.exe missing). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\engine\llama\llama-server.exe")
  #error Bundle incomplete (engine\llama\llama-server.exe missing). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\engine\sherpa\sherpa-onnx-c-api.dll")
  #error Bundle incomplete (engine\sherpa\sherpa-onnx-c-api.dll missing -- the live mode would name voices by strip, silently). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\engine\sherpa\onnxruntime.dll")
  #error Bundle incomplete (engine\sherpa\onnxruntime.dll missing -- sherpa-onnx cannot load without it). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\mpv\libmpv-2.dll")
  #error Bundle incomplete (mpv\libmpv-2.dll missing -- the player would fall back to the page's own video for EVERY file, and the analysis to Media Foundation). Re-run deploy\make_bundle.js with --mpv.
#endif
#if !FileExists(BundleAbs + "\app\mpv\vkfallback\vulkan-1.dll")
  #error Bundle incomplete (mpv\vkfallback\vulkan-1.dll missing -- a PC with no Vulkan loader could start neither libmpv nor whisper-server). Re-run deploy\make_bundle.js with --vulkan-rt.
#endif
#if !FileExists(BundleAbs + "\app\mpv\vkfallback\VulkanRT-License.txt")
  #error Bundle incomplete (the Vulkan loader's licence, mpv\vkfallback\VulkanRT-License.txt, is missing). Re-run deploy\make_bundle.js with --vulkan-rt.
#endif
#ifdef Full
  ; FULL: every model of installer\models.json (the generated block above) must be in the bundle, and be THAT file: the
  ; bundle's manifest (make_bundle.js: the SHA-256 and the size of each file, re-checked by make_bundle.js --verify) must
  ; list it with the SHA-256 and the size models.json pins. A missing 4B would be invisible at runtime (the engine falls
  ; back to the 1.5B without a word), a missing live or speaker model would degrade the live mode just as quietly, and an
  ; older or other model would make the full installer differ from the light one.
  #pragma parseroption -p-
  #define FullTab "\t"
  #pragma parseroption -p+
  #define FullI 0
  #define MfHandle 0
  #define MfWant ""
  #define MfFound 0
  #sub MfScanLine
    #if FileRead(MfHandle) == MfWant
      #expr MfFound = 1
    #endif
  #endsub
  #sub FullCheckModel
    #if !FileExists(BundleAbs + "\app\models\" + ModelFile[FullI])
      #expr Error("Bundle payload incomplete: the model " + ModelFile[FullI] + " (installer\models.json) is not in the bundle's app\models. Assemble a FULL bundle: node deploy\make_bundle.js --full (see the header).")
    #endif
    ; the file itself, not only its manifest row (a file replaced after the assembly; --verify re-hashes them all)
    #if Str(FileSize(BundleAbs + "\app\models\" + ModelFile[FullI])) != ModelSize[FullI]
      #expr Error("The bundle's app\models\" + ModelFile[FullI] + " has " + Str(FileSize(BundleAbs + "\app\models\" + ModelFile[FullI])) + " bytes; installer\models.json pins " + ModelSize[FullI] + ". The bundle changed after its assembly: re-assemble it (make_bundle.js --full), or check it (make_bundle.js --verify).")
    #endif
    #expr MfWant = UpperCase(ModelSha256[FullI]) + FullTab + ModelSize[FullI] + FullTab + "app\models\" + ModelFile[FullI]
    #expr MfFound = 0
    #for {MfHandle = FileOpen(BundleAbs + "\manifest.txt"); MfHandle && !FileEof(MfHandle); ""} MfScanLine
    #if MfHandle
      #expr FileClose(MfHandle)
    #endif
    #if !MfFound
      #expr Error("The bundle's " + ModelFile[FullI] + " is not the file installer\models.json pins (SHA-256 " + ModelSha256[FullI] + ", " + ModelSize[FullI] + " bytes): its manifest.txt lists other bytes. Re-assemble the full bundle (make_bundle.js --full) from a payload holding the models.json files.")
    #endif
  #endsub
  #for {FullI = 0; FullI < ModelCount; FullI++} FullCheckModel
#else
  ; LIGHT: no model in the setup -- [Code] downloads them. A bundle WITH models is a full bundle: refused, so the light
  ; installer can never grow back to 6 GB by accident.
  #if DirExists(BundleAbs + "\app\models")
    #error This bundle has an app\models folder: it is a FULL bundle. Compile it with /DFull, or assemble a light bundle (node deploy\make_bundle.js, without --full).
  #endif
#endif
; make_bundle.js stamps the build type it took the binaries from; only a Release is installable.
#if !FileExists(BundleAbs + "\BUILD-Release.txt")
  #ifdef AllowDebugBundle
    #pragma message "WARNING: this bundle was NOT made from a Release build (/DAllowDebugBundle given). Never ship this setup."
  #else
    #error This bundle was not made from a Release build (no BUILD-Release.txt next to manifest.txt). Build x64-Release and re-run deploy\make_bundle.js (or pass /DAllowDebugBundle for a test compile).
  #endif
#endif

[Setup]
AppId={{#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 NEXTALE
; The publisher's folder under Program Files, and a DEDICATED SuVtitle folder inside it (the uninstaller only ever
; sweeps SuVtitle's own sub-folders, see [UninstallDelete]).
DefaultDirName={autopf}\NEXTALE\SuVtitle
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
AppMutex={#MyAppMutex}
SetupMutex=SuVtitleSetupMutex
OutputDir=Output
#ifdef Full
OutputBaseFilename=SuVtitle_v{#MyAppVersion}_Setup_Full
#else
OutputBaseFilename=SuVtitle_v{#MyAppVersion}_Setup
#endif
SetupIconFile=..\app\suvtitle.ico
#ifdef Full
; ~6 GB of mostly-incompressible model weights: max LZMA2, solid off (solid over incompressible blobs
; costs compile + extract time for almost no size gain).
Compression=lzma2/max
SolidCompression=no
#else
; ~0.3 GB of executables and DLLs, many of them near-copies (15 ggml-cpu-*.dll, the VC++ CRT next to each server): SOLID
; (measured: 309 MB of bundle -> a 64 MB Setup.exe, far under the 315 MB a file may weigh on the release host).
Compression=lzma2/max
SolidCompression=yes
#endif
LZMANumBlockThreads=4
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExe}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
#ifdef Full
; DISK SPANNING IS MANDATORY ABOVE 4.2 GB: Inno refuses a single Setup.exe larger than 4,200,000,000 bytes
; (Windows' own ceiling on one executable image). With DiskSpanning=yes the files go to a SEPARATE data file and
; Setup.exe stays a ~2 MB stub: SuVtitle_v<version>_Setup_Full.exe + SuVtitle_v<version>_Setup_Full-1.bin. KEEP THEM IN THE
; SAME FOLDER: the .exe is the stub, the .bin holds the files, and Setup cannot run without it (it asks for the missing 'disk').
; MEASURED (Inno 6.7.1, 23/10/2026; AudioPB's 6.0 GB setups come out the same way): DiskSliceSize=max does NOT cut
; the data into 2.1 GB pieces -- it is ONE 5.4 GB -1.bin (5,405,135,311 bytes for the 5.40 GiB bundle). A host or a
; FAT32 stick that needs smaller files: compile with /DSliceSize=2000000000 (a number, 262144..2100000000) and
; the data comes out as -1.bin, -2.bin, -3.bin.
#ifndef SliceSize
  #define SliceSize "max"
#endif
DiskSpanning=yes
SlicesPerDisk=1
DiskSliceSize={#SliceSize}
#else
; LIGHT: one Setup.exe, no -1.bin. The models it downloads are not [Files] entries, so Inno would not count them: the
; directory page asks for their room too (a FRESH install downloads them all; the exact check, of the models really
; missing, is the [Code] one before the download).
ExtraDiskSpaceRequired={#ModelsTotalBytes}
#endif
; The player's file associations (the [Registry] block): Explorer re-reads them at the end.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[InstallDelete]
; An upgrade in place (same AppId): the engine, the page and the native video engine (mpv\, 1.1.0) are rebuilt from the
; bundle, so a file the new version no longer ships (a dropped DLL, an old page asset) must not survive. models\ is NEVER
; touched here: the light installer keeps every model that is in place with its size (an update downloads nothing) and
; replaces only the others; the full one overwrites them file by file (ignoreversion); a model the user dropped in by
; hand stays.
Type: filesandordirs; Name: "{app}\engine"
Type: filesandordirs; Name: "{app}\web"
Type: filesandordirs; Name: "{app}\mpv"

[Dirs]
; When {tmp} is on another drive than {app}, the light installer moves each downloaded model to {app}\models\<file>.part
; BEFORE the installation (models_download.iss), so it may create {app} itself. The uninstaller removes a folder it did
; not create only with this flag (when the folder is empty, so a folder the user shares keeps its content).
Name: "{app}"; Flags: uninsalwaysuninstall

[Files]
; Everything under the bundle's app\ - the exes, WebView2Loader.dll, the licences, web\, engine\{whisper,llama,sherpa}\, mpv\
; and, in a FULL bundle only, models\ - recursively. One rule, no file listed by name: a file added to the bundle by
; make_bundle.js ships here without touching this section. (A light bundle has no models\: the checks above refuse one that has.)
Source: "{#BundleAbs}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
; The LIVE mode's own entry (1.1.0): the same exe and the same icon, started with --live, which opens the Live window alone
; (app/main.cpp, TrLaunch.h; an already running SuVtitle opens its Live window instead of starting a second app).
Name: "{group}\{#MyAppName} Live"; Filename: "{app}\{#MyAppExe}"; Parameters: "--live"; IconFilename: "{app}\{#MyAppExe}"; Comment: "Live transcription and translation of an input, an output or one application"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Registry]
; >>> player registration (generated) >>>
; GENERATED by installer\gen_iss_registry.js from app\TrFileStore.h (mediaExts) and app\TrAssoc.h (isAudioExt, the ProgIDs,
;   the names, openCommand): do not edit by hand -- `node installer\gen_iss_registry.js --write` rewrites it and `--check`
;   fails when the two drift. Windows lets no application make itself the default player: these keys REGISTER SuVtitle
;   machine-wide as a candidate -- in the "Open with" list of every media file type it plays and in Settings > Default
;   apps with its file types. The app's own "Set as default player" writes the same per user (HKCU, TrAssoc.h entries())
;   and `--unregister-player` (run by the uninstaller, [Code] CurUninstallStepChanged) removes that. Everything below goes on uninstall
;   (uninsdeletekey / uninsdeletevalue / uninsdeletekeyifempty).
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Video"; ValueType: string; ValueName: ""; ValueData: "SuVtitle video"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Video"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "SuVtitle video"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Video\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Video\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "SuVtitle"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Video\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" --play ""%1"""
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Audio"; ValueType: string; ValueName: ""; ValueData: "SuVtitle audio"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Audio"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "SuVtitle audio"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Audio\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Audio\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "SuVtitle"
Root: HKLM; Subkey: "Software\Classes\NEXTALE.SuVtitle.Audio\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" --play ""%1"""
Root: HKLM; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.m4v\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.wmv\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mpg\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mpeg\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.m2ts\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mts\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.flv\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.3gp\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.ogv\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mxf\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Video"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mp3\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.wav\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.m4a\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.aac\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.flac\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.ogg\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.oga\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.opus\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mka\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.wma\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.aif\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.aiff\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.aifc\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.ac3\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.eac3\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.amr\OpenWithProgids"; ValueType: none; ValueName: "NEXTALE.SuVtitle.Audio"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\NEXTALE"; Flags: uninsdeletekeyifempty
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "SuVtitle"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Plays videos and audio files, with their subtitles transcribed and translated by SuVtitle."
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities"; ValueType: string; ValueName: "ApplicationIcon"; ValueData: "{app}\{#MyAppExe},0"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4v"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avi"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wmv"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpg"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpeg"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m2ts"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mts"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flv"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".3gp"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogv"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mxf"; ValueData: "NEXTALE.SuVtitle.Video"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp3"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wav"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4a"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aac"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flac"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogg"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".oga"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".opus"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mka"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wma"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aif"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aiff"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aifc"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ac3"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".eac3"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\NEXTALE\SuVtitle\Capabilities\FileAssociations"; ValueType: string; ValueName: ".amr"; ValueData: "NEXTALE.SuVtitle.Audio"
Root: HKLM; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "SuVtitle"; ValueData: "Software\NEXTALE\SuVtitle\Capabilities"; Flags: uninsdeletevalue
; installer-only (TrAssoc.h entries() does not write it): the Applications\<exe> key, for "Open with > Choose another app".
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "SuVtitle"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "SuVtitle"
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" --play ""%1"""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mp4"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".m4v"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mkv"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".webm"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mov"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".avi"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".wmv"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mpg"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mpeg"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".ts"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".m2ts"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mts"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".flv"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".3gp"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".ogv"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mxf"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mp3"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".wav"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".m4a"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".aac"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".flac"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".ogg"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".oga"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".opus"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".mka"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".wma"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".aif"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".aiff"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".aifc"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".ac3"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".eac3"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExe}\SupportedTypes"; ValueType: string; ValueName: ".amr"; ValueData: ""
; <<< player registration (generated) <<<

[Run]
; The original user, never the elevated one: the app must run in the user's own session and profile (and
; Explorer's drag and drop does not reach an elevated window).
Filename: "{app}\{#MyAppExe}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

; (No [UninstallRun] section: its entries cannot carry runasoriginaluser -- ISCC 6 refuses the flag there. The one
; command the uninstaller has to run, `--unregister-player`, is run from [Code], CurUninstallStepChanged, as the
; original user.)

[UninstallDelete]
; Files that are not in the uninstall log: the models the LIGHT installer downloaded (moved into models\ by [Code], so
; Inno never logged them), a model the user dropped into models\ by hand (ggml-large-v3.bin), anything the engine or the
; page left in its own folders. The install folder itself is NOT swept whole - the user may have chosen a shared folder.
Type: filesandordirs; Name: "{app}\engine"
Type: filesandordirs; Name: "{app}\models"
Type: filesandordirs; Name: "{app}\web"

[Code]
// ---------------------------------------------------------------------------
// Microsoft Edge WebView2 Runtime present? SuVtitle.exe needs it to render its window.
// ---------------------------------------------------------------------------
function WebView2Installed(): Boolean;
var pv: String;
begin
  Result := False;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) then
    Result := (pv <> '') and (pv <> '0.0.0.0');
  if (not Result) and RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) then
    Result := (pv <> '') and (pv <> '0.0.0.0');
end;

function InitializeSetup(): Boolean;
var rc: Integer;
begin
  Result := True;

  // Warn (do not block) if the WebView2 Runtime is missing - the window needs it. Never in a silent install.
  if (not WizardSilent) and (not WebView2Installed()) then begin
    if MsgBox('{#MyAppName}''s window needs the Microsoft Edge WebView2 Runtime, which was not '
              + 'found on this PC.' + #13#10 + #13#10
              + 'It is free and pre-installed on Windows 11. Open the download page now?' + #13#10
              + '(You can finish installing either way; {#MyAppName} will not start until it is installed.)',
              mbConfirmation, MB_YESNO) = IDYES then
      ShellExec('open', 'https://go.microsoft.com/fwlink/p/?LinkId=2124703', '', '', SW_SHOW, ewNoWait, rc);
  end;
end;

// ---------------------------------------------------------------------------
// Uninstall: what the app registered for the USER (HKCU: "Set as default player..." / --register-player) goes too.
// `SuVtitle.exe --unregister-player` opens no window, starts no engine and touches nothing but those keys
// (app/main.cpp, TrAssoc.h unregister()); it runs BEFORE any file is removed (usUninstall). As the ORIGINAL user:
// the uninstaller is elevated, and with a different admin account HKCU would be the admin's hive, not the user's.
// ---------------------------------------------------------------------------
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var rc: Integer;
begin
  if CurUninstallStep = usUninstall then
    ExecAsOriginalUser(ExpandConstant('{app}\{#MyAppExe}'), '--unregister-player', '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// (1.1.0) No more "vulkan-1.dll was not found" note after the install: whisper-server and libmpv import vulkan-1.dll,
// and the bundle now ships Khronos' loader in {app}\mpv\vkfallback\ -- used ONLY where System32 has none (libmpv: loaded by
// full path before it, app\MpvPlayer.cpp and engine\TrMpvAudio.cpp; the servers: that folder appended to their PATH, which
// Windows searches after System32). With no GPU driver behind it the loader lists no device: the picture plays through
// Direct3D 11 (WARP when there is no GPU at all) and the models run on the CPU. Nothing for the user to install.

#ifndef Full
// The LIGHT installer's model download: installer\models_download.iss (what it does: its header). The installer test kit
// (tests\installer\dltest.iss) #includes the same file, so the kit tests the code that ships. It reads the generated
// models block at the top of this script.
#include "models_download.iss"
#endif

// For tests: /DPreprocessedOut=<file> writes the script exactly as the compiler sees it after ISPP (every macro
// expanded), so the effective [Registry] lines can be compared with the C++ (TrAssoc.h entries()) without installing.
#ifdef PreprocessedOut
  #expr SaveToFile(PreprocessedOut)
#endif

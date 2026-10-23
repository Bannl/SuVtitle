; ==============================================================================
; SuVtitle - Inno Setup Installer Script
; NEXTALE
; ------------------------------------------------------------------------------
; SuVtitle is the subtitle PLAYER that was split out of AudioPB on 23/10/2026: a normal
; Windows application (a window and a taskbar button, no tray icon) that plays a video
; or audio file and transcribes / translates it OFFLINE into subtitles that stay ahead
; of the picture, with the full subtitle tooling (.srt / .vtt / .ass, the film's own
; tracks, an editor, transcript export). It ships, beside the app, two local AI engines
; driven as child processes by SuVtitleEngine.exe -- whisper.cpp (transcription) and
; llama.cpp / Qwen (translation) -- and their models: about 5.6 GB. Everything runs on
; 127.0.0.1; nothing is sent off the machine.
;
; Models that ship (director's decision, 23/10/2026): ggml-large-v3-turbo.bin (transcription),
; Qwen3-4B-Instruct-2507-Q5_K_M.gguf (translation) and qwen2.5-1.5b-instruct-q4_k_m.gguf
; (translation fallback). A missing 4B is INVISIBLE at runtime -- the engine falls back to
; the 1.5B without a word -- so the preconditions below refuse a bundle that lacks any of
; them. NOT shipped: ggml-small / ggml-base (they belonged to the LIVE translator, which
; stayed in AudioPB), sherpa-onnx and its speaker model (a file job tells no speakers
; apart), ggml-large-v3.bin (quality "max" only; the engine says so when it is absent --
; make_bundle.js --with-large-v3 puts it in).
;
; What gets installed ({app} = %ProgramFiles%\NEXTALE\SuVtitle):
;   * SuVtitle.exe        - the window (Win32 + WebView2): player, queue, subtitles, editor
;   * SuVtitleEngine.exe  - the AI helper SuVtitle.exe spawns and supervises
;   * WebView2Loader.dll, web\ (the page SuVtitle.exe serves from <exe>\web)
;   * engine\whisper\, engine\llama\ (the servers + ggml backends + the VC++ CRT, app-local)
;   * models\
;
; SOURCE = the assembled, dumpbin-verified bundle (deploy\out\SuVtitle\), produced by
; deploy\make_bundle.js. Build the Release, assemble, THEN compile this script:
;   node deploy\make_bundle.js --payload <your AI payload folder>      (or set SUVTITLE_PAYLOAD; see make_bundle.js)
;   ISCC.exe installer\SuVtitle_Setup.iss
; ISCC.exe is Inno Setup 6's compiler: on PATH, or in the standard install folders -- all users:
; %ProgramFiles(x86)%\Inno Setup 6\ISCC.exe, current user: %LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe (call it by that full
; path when it is not on PATH). Call it from cmd or PowerShell, NOT Git Bash: it mangles the /Q-style switches.
; Overridable: /DMyAppVersion=1.0.1   /DBundleDir=<absolute path of another bundle>
;              /DAllowDebugBundle     (compile a bundle made from a Debug build: for tests only)
;              /DSliceSize=2000000000 (cut the data into ~2 GB files; see DiskSpanning below)
; The setup is far above 4.2 GB, so it ships as a ~2 MB Setup.exe stub + Setup-1.bin, the DATA (DiskSpanning);
; keep them in the same folder. A full compile takes minutes (measured 23/10/2026: 227 s, LZMA2 over 5.4 GiB).
;
; The player registration ([Registry], HKLM) is GENERATED from the C++ lists by
; installer\gen_iss_registry.js; `node installer\gen_iss_registry.js --check` fails when this
; file drifts from the sources (that block, the version, the mutex, the payload layout ...).
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
  #define MyAppVersion "1.0.0"
#endif
#define MyAppPublisher "NEXTALE"
#define MyAppURL "https://www.nextale-audio.com/"
#define MyAppExe "SuVtitle.exe"
; Installer AppId (the uninstall identity): unique to SuVtitle, constant across versions. Not AudioPB's.
#define MyAppId "{6FF1C973-F626-4B99-8A05-6A37BB152543}"
; The application's single-instance mutex (app/main.cpp kMutexName): while it exists, Setup and Uninstall
; refuse to touch a running SuVtitle instead of failing on locked files.
#define MyAppMutex "SuVtitle.SingleInstance"

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
#if !FileExists(BundleAbs + "\app\models\ggml-large-v3-turbo.bin")
  #error Bundle payload incomplete (the transcription model ggml-large-v3-turbo.bin is missing - was make_bundle.js run with --skip-models?).
#endif
#if !FileExists(BundleAbs + "\app\models\Qwen3-4B-Instruct-2507-Q5_K_M.gguf")
  #error Bundle payload incomplete (Qwen3-4B translation model missing -- the engine would silently run the 1.5B). Re-run deploy\make_bundle.js.
#endif
#if !FileExists(BundleAbs + "\app\models\qwen2.5-1.5b-instruct-q4_k_m.gguf")
  #error Bundle payload incomplete (the 1.5B translation fallback is missing). Re-run deploy\make_bundle.js.
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
OutputBaseFilename=SuVtitle_v{#MyAppVersion}_Setup
SetupIconFile=..\app\suvtitle.ico
; ~5.6 GB of mostly-incompressible model weights: max LZMA2, solid off (solid over incompressible blobs
; costs compile + extract time for almost no size gain).
Compression=lzma2/max
SolidCompression=no
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
; DISK SPANNING IS MANDATORY ABOVE 4.2 GB: Inno refuses a single Setup.exe larger than 4,200,000,000 bytes
; (Windows' own ceiling on one executable image). With DiskSpanning=yes the files go to a SEPARATE data file and
; Setup.exe stays a ~2 MB stub: SuVtitle_v1.0.0_Setup.exe + SuVtitle_v1.0.0_Setup-1.bin. KEEP THEM IN THE SAME
; FOLDER: the .exe is the stub, the .bin holds the files, and Setup cannot run without it (it asks for the missing 'disk').
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
; The player's file associations (the [Registry] block): Explorer re-reads them at the end.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[InstallDelete]
; An upgrade in place (same AppId): the engine and the page are rebuilt from the bundle, so a file the new
; version no longer ships (a dropped DLL, an old page asset) must not survive. models\ is NOT touched here:
; it is overwritten file by file (ignoreversion) and a model the user dropped in by hand stays.
Type: filesandordirs; Name: "{app}\engine"
Type: filesandordirs; Name: "{app}\web"

[Files]
; Everything under the bundle's app\ - the exes, WebView2Loader.dll, web\, engine\{whisper,llama}\ and models\ -
; recursively. One rule, no model listed by name: a file added to the bundle by make_bundle.js ships here
; without touching this section.
Source: "{#BundleAbs}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
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
; Files that are not in the uninstall log: a model the user dropped into models\ by hand (ggml-large-v3.bin),
; anything the engine or the page left in its own folders. The install folder itself is NOT swept whole - the
; user may have chosen a shared folder.
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

// A GPU with a Vulkan loader is optional but strongly wanted: whisper-server hard-imports vulkan-1.dll (it will
// not start without it, the CPU fallback included), so a truly GPU-less PC needs the loader. Inform on the
// finished install; the engine still falls back to the CPU for the models when no GPU can run them.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if (not WizardSilent) and (not FileExists(ExpandConstant('{sys}\vulkan-1.dll'))) then
      MsgBox('Note: vulkan-1.dll was not found on this PC.' + #13#10 + #13#10
             + 'The transcription engine needs a Vulkan loader to start; it usually ships '
             + 'with your graphics driver. If transcription does not start, install/update your GPU '
             + 'driver (AMD, NVIDIA or Intel). Machines with a real GPU already have it.',
             mbInformation, MB_OK);
  end;
end;

// For tests: /DPreprocessedOut=<file> writes the script exactly as the compiler sees it after ISPP (every macro
// expanded), so the effective [Registry] lines can be compared with the C++ (TrAssoc.h entries()) without installing.
#ifdef PreprocessedOut
  #expr SaveToFile(PreprocessedOut)
#endif

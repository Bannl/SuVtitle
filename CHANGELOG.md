# Changelog

## 1.1.0 — 2026-11-03

The **Live mode**, **transcripts**, **your own models**, a **native video engine** that plays almost every file,
and the fixes the 1.0.0 player needed once it was in use.

### Live mode
- A Live window beside the player. Open it with the player's **Translate Live** button, or start
  `SuVtitle.exe --live` (the installer adds a **SuVtitle Live** shortcut to the Start menu; it opens the Live window
  alone). It transcribes and translates, as it is spoken, an **input** (a microphone, a line, the output of a virtual
  cable, one channel of an interface), **the sound of an output** (loopback) or **one application** (Windows 10 build
  20348 or later, or Windows 11; the entry is greyed out below that). The engine captures the audio itself, through
  WASAPI.
- Up to **4 strips** at once, each with a gain trim, a level meter and the name of its speaker. When the speaker
  model is installed, the voices heard on one strip are also told apart (CAM++ through sherpa-onnx) and every line
  carries its speaker's colour.
- **One language pair per mode**: the Live window has its own pair and direction, separate from the player's pair,
  and a menu for the quality preset (Auto, Quality, Balanced, Light).
- **Session journal and export**: every final line is journaled in `%APPDATA%\SuVtitle\Live sessions\`; Export writes a
  `.docx`; Clear starts a new session; the current session survives a restart of the app.
- **Live and file analysis never run together.** Listen cancels the analysis in progress (what is done is kept) and it
  resumes by itself when the live stops; a film opened during a live session plays with the subtitles already made.
- **Listen off finishes the last sentences.** The devices are released at once; the sentences already heard are
  then transcribed and translated. On the CPU that can take tens of seconds.
- Nothing listens at launch, the source list never opens a device and the level meters run only while listening.
  Capturing a microphone opts out of Windows' communications ducking, so other sounds keep their volume. An
  application source follows that application's own processes (a browser's helpers included) and finds it again by
  name if it restarts.
- Closing the Live window stops listening; the app quits when its last window closes.
- The AI payload gains `ggml-small.bin`, `ggml-base.bin`, the CAM++ speaker model and the sherpa-onnx runtime (about
  0.7 GB).

### Translate and transcribe
- **Translate is a button.** A file is analysed only when you ask: **Translate** for the file you watch, **Translate
  all** (the Queue tab) for the whole list. A film already analysed shows its subtitles at once, without a press, and
  the next files of the queue are no longer analysed ahead. Once the film is complete the button offers **Re-analyse**.
- **Translate all** analyses the list one file at a time, in its order, the playing file first; each row says queued,
  analysing (with its percentage), done, failed or cancelled. **Cancel all** shows while the list has work running or
  waiting, and stops only what the list launched: a file you pressed yourself goes on. Live translation holds the
  list; it resumes when Live stops.
- **Transcribe**, next to Translate, writes down what is said in the file, **speaker by speaker**, with a **timecode at
  each turn**, in a Word file next to the media: `<name> - transcript.docx`. Translate and Transcribe share one
  analysis (whisper runs once); a short voice pass then tells the speakers apart (the CAM++ speaker model). Without
  the speaker model the transcript is still made, with no names.
- **Each button has its own Cancel**, named for its work: **Cancel translation** and **Cancel transcript**. Cancelling
  one never stops the other's work.
- **The Transcript tab** shows the transcript as it grows; a click on a turn plays the film from there. Click a
  speaker's name to rename that speaker everywhere; two speakers given the same name become one person. A rename, or a
  subtitle edit that changes the transcript, updates the Word file by itself a second or two later, when SuVtitle
  wrote that file and it has not been changed since.
- **Timecodes** from the start of the file, or the recording's own start timecode when it carries one (BWF / RF64 /
  BW64 with iXML, a MOV / MP4 timecode track, MXF), drop-frame included. A Word file written with the source timecode
  keeps it when the timecode cannot be read on a later run.
- **Dialogue lines**: a subtitle that holds two speakers' lines ("- Yes. - No.") is split in the transcript, each line
  given its own speaker; the subtitles themselves are not changed.
- **One Word file per sound track**: another sound track of the film writes `<name> - transcript (track 2).docx`
  (the number of the track menu). A transcript of the same track in another language pair or with another model never
  replaces its Word file (your speaker names stay): its tab says so, and **Save .docx…** writes it to a new file.
- **Your Word files are safe.** A Word file SuVtitle did not write is never overwritten (the transcript takes another
  name, `<name> - transcript (SuVtitle).docx`), and one you edited is kept as it is. A Word file open in Word is never
  replaced by a copy elsewhere: the tab says to close it, and the next change tries again. A read-only one is left as
  it is.
- **Transcribe all** (the Queue tab) writes one Word file per file of the list. A file whose Word file is not next to
  it (saved elsewhere, not written, not up to date) says where or why on its row, and one message at the end counts
  them.
- The Export menu has both Word files: **Bilingual subtitles (.docx)** and **Transcript (.docx)**.

### Your own models
- **The Models menu** (on the empty screen, and next to Translate) shows the transcription and translation models in
  use. Pick another one, or **Import model…** your own: any whisper.cpp model (`.bin`) for transcription, any instruct
  GGUF model that llama.cpp runs for translation. SuVtitle reads the file's header to tell which kind it is (never
  its name), uses the file where it is and remembers it; a file that has gone shows greyed as "not found".
- A model that will not load stops the analysis with a message naming it, and SuVtitle goes back to its own model. In
  Translate all or Transcribe all the list goes on and that file is done again with the default model, once ("Redone
  with the default model"). An analysis made with an imported model is cached under that model, so it never replaces
  the default model's subtitles.
- **Removing an imported model** (the × beside it) asks whether to remove it from the list only or to delete its file
  too; the file then goes to the Recycle Bin. A model SuVtitle ships is never deleted, and a model an analysis is using
  cannot be removed until that analysis ends.

### Native video engine
- **The player plays almost every file.** A native video engine (libmpv) now plays every file, picture and sound:
  MKV, MP4, AVI, TS / M2TS, MPG / VOB, WMV, FLV, OGG, and the codecs the page could not always play (HEVC Main 10,
  AV1, MPEG-2, Xvid / DivX, WMV, Theora...). The page draws its interface over the picture; the mouse, the keyboard,
  the cursor, full screen and drag-and-drop work as before. If the engine cannot start, or a file fails in it, the
  player goes back to the page's own `<video>` by itself.
- **No graphics card required.** Hardware decoding where the PC has it, software decoding otherwise; a PC with no
  usable GPU draws through Windows' software renderer, with a lighter profile. SuVtitle ships Khronos' Vulkan loader
  for a PC that has none (`mpv\vkfallback\`); a PC that has its own keeps using it.
- **The sound tracks of AVI, TS / M2TS, MPG, WMV, FLV and OGG files**: the engine lists them, so the right-click
  menu and the CC panel offer them as for an MKV, and the analysis follows the track you hear. The list read from an
  MKV or an MP4 is never replaced.
- **The analysis hears the file through the same engine**: the player's own timeline (start offsets included), any
  sound track of any container. A recording whose sound changes format midway (channels, sample rate) no longer
  stops the analysis, and gaps in its timestamps are filled with silence, so the subtitles stay in time.
- **Safer by design**: a media file never makes the player open an address it names (playlists, HLS manifests and
  edit lists are not followed), and the program loads its libraries only from its own folders and System32, never
  from the folder of the film it opens.

### Player
- **Right-click menu** on the picture (and on the playing row of the queue): the file's sound tracks, its subtitle
  choices (SuVtitle's own, external files, the film's embedded tracks), "Load a subtitle file..." and the subtitle
  settings, at the pointer. They are the rows of the CC panel, so both doors agree. Full screen's idle hide stays off
  while a menu or panel is open.
- **Full screen no longer freezes the screen** on the system that showed it (an AMD graphics system at 165 Hz: the
  picture and sound went on while the cursor, the play bar and the subtitles froze until full screen was left). In
  full screen the subtitles are now drawn directly instead of blended with the picture; Ctrl+Alt+V still switches
  the renderer by choice.
- **Freeze watch**: when the page notices it stopped drawing, one line (full screen, foreground, minimised, whether an
  analysis is running) is added to `%LOCALAPPDATA%\SuVtitle\player.log` (60 lines a minute at most; the file restarts
  at 512 KB). Nothing leaves the computer.
- **A resumed analysis no longer leaves holes.** A file analysis that was cancelled and resumed (by Listen, for
  example) could leave stretches of speech with no subtitle; the resumed run now covers them.
- The language pair moved from the top bar to beside **Translate** (on the empty screen: beside the Models menu).
  Languages written in another script show their own name with the English one: "日本語 (Japanese)".
- The name line reads "Transcription, Translation & Subtitles", with "by NEXTALE Audio" under it.

### Updates
- **An update banner.** Once at each start SuVtitle asks `www.nextale-audio.com` whether a newer version is published (in
  the background; the request names only SuVtitle's version; offline or no answer: nothing is said). A newer one shows a
  strip above the player, never in full screen: **Download** opens SuVtitle's page on www.nextale-audio.com in your browser
  (`https://www.nextale-audio.com/suvtitle?update=<new>&from=<yours>`), where the new version's installer is (it replaces
  this version); **Later** (until the next start) and **Skip this version** (the Updates dialog still names it and can
  offer it again). A prerelease is offered only to a prerelease, never to a release.
- **The Updates dialog** (the **⋯** menu, or the empty window's link): the version you have, a newer one with its
  **Download** (a skipped one too, with **Offer it again**), and the check on or off. `UPDATECHECK 0` in `app.txt` turns
  the check off too.
- A drop zone taller than the room left (a short window) now scrolls from its top instead of being cut off above.

### Installer
- **One file of about 60 MB.** The installer no longer carries the AI models: when you click Install it downloads from
  Hugging Face the ones the installation folder lacks (about 5.9 GB the first time), each from a URL pinned to a commit
  and checked against its SHA-256 (`installer\models.json` lists them, with their licenses). An update whose models are
  in place downloads nothing and needs no connection.
- The Ready page says what will be downloaded and how much. The free disk space is checked first; a failed download
  (no connection, a server error, a corrupted file) says why and offers Retry, and nothing is installed until every
  model is there.
- A model gets its final name only once it is complete (written as `.part`, then renamed in one step; the previous model
  stays until then): a crash, a power cut or a killed installer never leaves a damaged model that later installations
  would keep. `/VERIFYMODELS` checks the installed models byte for byte (SHA-256) and downloads a damaged one again.
- Installing on another drive than `%TEMP%`: each model moves to the installation drive right after its download, so
  `%TEMP%`'s drive needs room for the largest model only.
- Silent installs tell the outcome by their exit code: 7, nothing installed (the models could not be downloaded); 10,
  installed but a model could not be put in place (it is named at the end; the installer fetches it on its next run);
  11, installed but the permissions of a model could not be reset.
- An offline installer with the models inside (about 6 GB, two files) can still be built (`ISCC /DFull`).

### Robustness
- Every Live message is validated field by field on both sides of the pipe (counts, source kinds, label length,
  channel, gain, duplicate sources); a malformed message is refused and reported, never half-applied.
- If the engine dies it is restarted with a growing delay, the listening session starts again with the same strips,
  and speaker colours start afresh, so a colour never stays on the wrong person.
- Closing the last window while listening hides the windows at once, stops listening, waits (up to 15 s) for the last
  sentences and exits; the transcript is already on disk. A browser-process crash is recovered without a dialog and
  without taking the foreground.
- **On a PC with no usable GPU** (no Vulkan device) the engine says **CPU** from its start, where it said GPU: every
  model starts on the CPU at once, and the Live window's **Auto** preset takes **Light** at once (it used to time the
  large model on the CPU first). A graphics driver installed later is used at the next start.

## 1.0.0 — unreleased

First release of SuVtitle as its own application. It started as the file-mode player of NEXTALE Audio's AudioPB
Translator and was split out of it on 2026-10-23.

### Player
- Plays video and audio files in a WebView2 window: queue, folders, repeat and shuffle, resume, A-B loop,
  speed, full screen with auto-hiding controls, drag and drop, single instance with `--play <files>`.
- Can be registered as a Windows default player (Open with, Default apps).
- HEVC/x265 playback keeps hardware decoding. In full screen the picture is composited with the page, so
  subtitles stay visible at every size. Ctrl+Alt+V switches to a canvas fallback renderer.

### Subtitles
- Offline transcription and translation of the file's speech, done ahead of the playhead while you watch
  (whisper.cpp and llama.cpp servers, GPU through Vulkan or CPU). The analysis follows seeks and is cached.
- One `.srt` per language is written next to the media as the analysis progresses.
- External subtitle files (`.srt`, `.vtt`, `.ass`, `.ssa`) with encoding detection and override.
- The film's own tracks: choose the sound track (the analysis follows the track you hear); embedded text
  subtitles from MKV/WebM/MP4/MOV (SRT, ASS/SSA, WebVTT, mov_text) are selectable; image subtitles (VobSub,
  PGS) are listed as not supported.
- Subtitle editor (text, timing, split, merge, shift), style settings (size slider, position, colour,
  outline, font), delay, export.

### Robustness
- The engine runs as a separate process over a named pipe that only the current user can open; the page
  is served locally, with a content security policy and no network access.
- First run copies (never moves) the player data and analyses of an existing AudioPB installation.

# Changelog

## 1.0.0 — unreleased

First release of SuVtitle as its own application. It started as the file-mode player of NEXTALE's AudioPB
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

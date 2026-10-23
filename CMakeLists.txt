# =============================================================================
#  SuVtitle (NEXTALE) -- the subtitle player: plays a video or audio file and
#  transcribes / translates it OFFLINE into subtitles that stay ahead of the picture.
#  Split out of AudioPB_Translator on 2026-10-23 (AudioPB keeps the LIVE translator;
#  everything about media FILES lives here). Building, the AI payload, tests: README.md.
#
#  Two executables, x64 only:
#    SuVtitle.exe        app/     the window (Win32 + WebView2): player, queue, subtitles
#                                 (AudioPB's / files / the film's own tracks), editor, export
#    SuVtitleEngine.exe  engine/  the AI helper: whisper-server (transcription) + llama-server
#                                 (translation) driven over a named pipe (common/TrPipe.h)
#
#  Build (Ninja + MSVC from VS 2026, outside PATH):
#    tools\buildlock.cmd x64-Debug                 (configure if needed, then build all)
#    tools\buildlock.cmd x64-Release --target suvtitle_app
#  Output: build\<config>\SuVtitle.exe, SuVtitleEngine.exe, web\ (the page), WebView2Loader.dll
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(SuVtitle
    VERSION 1.0.0
    DESCRIPTION "SuVtitle - subtitle player with offline transcription and translation"
    LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SuVtitle builds for x64 only (the AI payload is x64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)   # MSVC_RUNTIME_LIBRARY: our own binaries are /MT (no VC++ redist needed)

set(SUVTITLE_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}")
set(SUVTITLE_COMMON "${SUVTITLE_ROOT}/common")   # TrPipe.h: the app <-> engine contract (director-owned)

# Every executable lands in the build directory's root, side by side: SuVtitle.exe spawns
# SuVtitleEngine.exe by sibling path, and serves web\ from its own folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(engine)   # target suvtitle_engine -> SuVtitleEngine.exe
add_subdirectory(app)      # target suvtitle_app    -> SuVtitle.exe (+ web\ copied next to it)

# =============================================================================
#  SuVtitleEngine.exe  (target suvtitle_engine)  -  the AI half of SuVtitle, x64 only
# =============================================================================
#  AudioPB's AudioPBTranslator.exe (PatchBus build/CMakeLists.txt, block "(D)", commit 641f1aa) minus its LIVE
#  path (SPLIT_PLAN S5): the FILE MODE that transcribes and translates media files, driving two third-party
#  HTTP servers (whisper.cpp's whisper-server, llama.cpp's llama-server) as children, for SuVtitle.exe, which
#  spawns it by sibling path and talks to it over a pipe (common/TrPipe.h). Its own process for the reasons
#  AudioPB gave: the children must die with something that is not the app, a 1.6-3 GB model load has no
#  business in the process that owns the window, and a crash here must cost the subtitles, never the player.
#
#  NOT linked (S5): the fabric core (PatchBus engine/shm.cpp, ring_buffer.cpp, async_src.cpp), the live
#  consumer (TrAudio.cpp, and avrt with it), diarization (TrSpeaker.cpp; sherpa was reached at run time anyway).
#  Sources keep AudioPB's file names (S3), so each one diffs against its 641f1aa origin.
# =============================================================================
enable_language(RC)   # engine.rc (MSVC enables it with CXX anyway; said, not assumed)

add_executable(suvtitle_engine
    main_translator.cpp
    TrEngine.cpp
    TrMedia.cpp
    TrFileJob.cpp
    TrEngine.h
    TrMedia.h
    TrFileJob.h
    TrText.h
    EngineVersion.h
    "${SUVTITLE_COMMON}/TrPipe.h"
    engine.rc             # version resource 1.0.0 + SuVtitle's icon (app/suvtitle.ico, the placeholder)
)

target_include_directories(suvtitle_engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SUVTITLE_COMMON}"            # TrPipe.h
)

# winhttp: the loopback HTTP client that drives the two child servers.
# ws2_32:  findFreePort BINDS a port to learn one is free (the only non-racy way).
# shlwapi, user32: as the helper had them.
# elscore: Windows' Extended Linguistic Services -- the word-level Simplified <-> Traditional converter of the
#          zh / zh-tw script nets (TrEngine.cpp also names it in a #pragma comment, as it did in AudioPB).
target_link_libraries(suvtitle_engine PRIVATE winhttp ws2_32 shlwapi user32 elscore)
# FILE MODE (AudioPB wave WB): Media Foundation reads the recording (TrMedia.cpp) -- system DLLs, nothing
# redistributed -- and the Resampler DSP's CLSID lives in wmcodecdspuuid.
# /!\ DELAY-LOADED: Windows N / KN have no mfplat.dll / mfreadwrite.dll without the Media Feature Pack, and a
# load-time import would stop the engine from starting AT ALL; TrMedia.cpp checks the DLLs exist before its
# first Media Foundation call and turns their absence into one sentence (FDONE error).
target_link_libraries(suvtitle_engine PRIVATE mfplat mfreadwrite mfuuid ole32 wmcodecdspuuid delayimp)
target_link_options(suvtitle_engine PRIVATE "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll")

target_compile_definitions(suvtitle_engine PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    # The CRT's "unsafe function" deprecation (C4996: getenv, _wfopen, sscanf -- all in the selftests). Not a
    # defect report but a nudge towards the _s variants; the ported code keeps 641f1aa's calls (parity), so
    # the nudge is switched off rather than the level lowered.
    _CRT_SECURE_NO_WARNINGS
)
# /W3: AudioPB's helper had no explicit level (CMake >= 3.15 adds none, so cl's default /W1); the engine is held
# to /W3, warning-free.
target_compile_options(suvtitle_engine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3>)

# /MT: no VC++ redistributable to install (cmake_policy CMP0091 NEW at the top level).
set_property(TARGET suvtitle_engine PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Next to SuVtitle.exe (CMAKE_RUNTIME_OUTPUT_DIRECTORY, top level), which spawns it by sibling path.
set_target_properties(suvtitle_engine PROPERTIES OUTPUT_NAME "SuVtitleEngine")

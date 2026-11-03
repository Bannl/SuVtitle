# =============================================================================
#  SuVtitle - app/CMakeLists.txt   (S-HOST, 2026-10-23)
#  target suvtitle_app -> SuVtitle.exe: the window (Win32 + WebView2) -- the player, its queue, its subtitles, the
#  engine's supervisor. Ported from AudioPB's patchbus_ui target (PatchBus build/CMakeLists.txt at 641f1aa) minus
#  the desk: no fabric, no router, no FX, no VST3, no licence. (2026-11-03: AudioPB's update check came back --
#  app/NextaleUpdate.cpp, WinHTTP -- in EVERY build: the code is public; its opt-out is app.txt's UPDATECHECK.)
#
#  Next to the exe (CMAKE_RUNTIME_OUTPUT_DIRECTORY = the build dir's root, top-level CMakeLists):
#    SuVtitle.exe, WebView2Loader.dll (portability of loose builds), web\ (a copy of app\web\, S-WEB's page).
#  The engine SuVtitleEngine.exe (engine/, S-ENG) lands in the same folder: the app spawns it by sibling path.
#
#  Build ONLY through tools\buildlock.cmd x64-Debug --target suvtitle_app (the shared lock).
# =============================================================================
set(APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
#  WebView2 SDK -- the Microsoft.Web.WebView2 NuGet package, never vendored (AudioPB's patchbus_acquire_webview2,
#  641f1aa, renamed; same package version as AudioPB 1.0.6 ships with). Where it comes from, in order:
#    (1) -DWEBVIEW2_NUGET_DIR=<an extracted package>          (the cache variable)
#    (2) the environment variable WEBVIEW2_NUGET_DIR           (handy for a first configure through buildlock.cmd)
#    (2b) the folder (2) named at an earlier configure of this build dir: CMake re-configures by itself (a CMakeLists.txt
#         edit, a new file under app/web), often from a shell without the variable, and that must not become a download
#    (3) <build dir>/_deps/webview2                            (a previous download)
#    (4) a download of the .nupkg from nuget.org into (3)
#  An extracted package is NuGet's own layout; to get one without this script, unzip Microsoft.Web.WebView2.<version>.nupkg
#  (it is a zip) and point WEBVIEW2_NUGET_DIR at the folder. The layout relied on: <pkg>/build/native/include/WebView2.h,
#  <pkg>/build/native/x64/WebView2LoaderStatic.lib, <pkg>/build/native/x64/WebView2Loader.dll.
#  tests/e2e/wv2probe/build.cmd looks for the same package (it reuses the copy this configure leaves in (3)).
# -----------------------------------------------------------------------------
set(WEBVIEW2_VERSION "1.0.2592.51" CACHE STRING "Microsoft.Web.WebView2 nuget version to fetch")
set(WEBVIEW2_NUGET_DIR "" CACHE PATH "Path to an already-extracted Microsoft.Web.WebView2 nuget package (skips download)")

function(suvtitle_webview2_ok dir out)
    set(_ok FALSE)
    if(dir AND EXISTS "${dir}/build/native/include/WebView2.h" AND EXISTS "${dir}/build/native/x64/WebView2LoaderStatic.lib")
        set(_ok TRUE)
    endif()
    set(${out} ${_ok} PARENT_SCOPE)
endfunction()

function(suvtitle_acquire_webview2 out_dir)
    suvtitle_webview2_ok("${WEBVIEW2_NUGET_DIR}" _ok)
    if(_ok)
        message(STATUS "SuVtitle: using the supplied WebView2 SDK at ${WEBVIEW2_NUGET_DIR}")
        set(${out_dir} "${WEBVIEW2_NUGET_DIR}" PARENT_SCOPE)
        return()
    endif()
    if(DEFINED ENV{WEBVIEW2_NUGET_DIR})
        suvtitle_webview2_ok("$ENV{WEBVIEW2_NUGET_DIR}" _ok)
        if(_ok)
            message(STATUS "SuVtitle: using the WebView2 SDK named by the environment: $ENV{WEBVIEW2_NUGET_DIR}")
            set(SUVTITLE_WEBVIEW2_ENV_DIR "$ENV{WEBVIEW2_NUGET_DIR}" CACHE INTERNAL "the WEBVIEW2_NUGET_DIR an earlier configure got from the environment")
            set(${out_dir} "$ENV{WEBVIEW2_NUGET_DIR}" PARENT_SCOPE)
            return()
        endif()
    endif()
    suvtitle_webview2_ok("${SUVTITLE_WEBVIEW2_ENV_DIR}" _ok)
    if(_ok)
        message(STATUS "SuVtitle: using the WebView2 SDK the environment named at an earlier configure: ${SUVTITLE_WEBVIEW2_ENV_DIR}")
        set(${out_dir} "${SUVTITLE_WEBVIEW2_ENV_DIR}" PARENT_SCOPE)
        return()
    endif()
    set(_pkg_root "${CMAKE_BINARY_DIR}/_deps/webview2")
    suvtitle_webview2_ok("${_pkg_root}" _ok)
    if(_ok)
        set(${out_dir} "${_pkg_root}" PARENT_SCOPE)
        return()
    endif()
    message(STATUS "SuVtitle: downloading Microsoft.Web.WebView2 ${WEBVIEW2_VERSION} ...")
    set(_nupkg "${CMAKE_BINARY_DIR}/_deps/webview2.zip")
    file(DOWNLOAD
        "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
        "${_nupkg}"
        SHOW_PROGRESS
        STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR
            "Failed to download the WebView2 nuget (code ${_dl_status}).\n"
            "Download it by hand and pass -DWEBVIEW2_NUGET_DIR=<extracted dir> (or set the environment variable).")
    endif()
    file(MAKE_DIRECTORY "${_pkg_root}")
    file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_pkg_root}")
    suvtitle_webview2_ok("${_pkg_root}" _ok)
    if(NOT _ok)
        message(FATAL_ERROR "WebView2 package extracted but WebView2.h / WebView2LoaderStatic.lib not found under ${_pkg_root}.")
    endif()
    set(${out_dir} "${_pkg_root}" PARENT_SCOPE)
endfunction()

suvtitle_acquire_webview2(WV2_DIR)
set(WV2_INCLUDE "${WV2_DIR}/build/native/include")
set(WV2_LIB     "${WV2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
set(WV2_DLL     "${WV2_DIR}/build/native/x64/WebView2Loader.dll")

# -----------------------------------------------------------------------------
#  SuVtitle.exe
# -----------------------------------------------------------------------------
add_executable(suvtitle_app WIN32
    "${APP_DIR}/main.cpp"
    "${APP_DIR}/bridge.cpp"
    "${APP_DIR}/bridge.h"
    # the engine's supervisor (spawn, named pipe, watchdog, respawn) -- the protocol is common/TrPipe.h
    "${APP_DIR}/TranslatorManager.cpp"
    "${APP_DIR}/TranslatorManager.h"
    # the Word transcript (Export > .docx): dependency-free, a ZIP of five XML parts
    "${APP_DIR}/DocxWriter.cpp"
    "${APP_DIR}/DocxWriter.h"
    # the native video engine (1.1.0, docs/NATIVE_CONTRACT.md): libmpv loaded at run time, never a hard import
    "${APP_DIR}/MpvPlayer.cpp"
    "${APP_DIR}/MpvPlayer.h"
    # the update banner (2026-11-03): the site's manifest read once per start (WinHTTP, off the UI thread), the page's URL guard
    "${APP_DIR}/NextaleUpdate.cpp"
    "${APP_DIR}/NextaleUpdate.h"
    # header-only, pure where they can be (each one has its smoke in tests/cpp)
    "${APP_DIR}/TrPlayer.h"
    "${APP_DIR}/TrFileStore.h"
    "${APP_DIR}/TrWindowState.h"
    "${APP_DIR}/SubtitleReader.h"
    "${APP_DIR}/SubtitleWriter.h"
    "${APP_DIR}/MediaTracks.h"
    "${APP_DIR}/TrLaunch.h"
    "${APP_DIR}/TrAssoc.h"
    "${APP_DIR}/JsonR.h"
    "${APP_DIR}/JsonW.h"
    "${APP_DIR}/AppConfig.h"
    "${APP_DIR}/AudioPBImport.h"
    "${SUVTITLE_COMMON}/TrPipe.h"
    # the icon (window class, taskbar, Alt-Tab, Explorer) + the version resource; MSVC compiles .rc natively
    "${APP_DIR}/suvtitle.rc"
    "${APP_DIR}/resource.h"
)

target_include_directories(suvtitle_app PRIVATE
    "${APP_DIR}"
    "${SUVTITLE_COMMON}"   # TrPipe.h: the app <-> engine contract (director-owned)
    "${WV2_INCLUDE}"
)

target_link_libraries(suvtitle_app PRIVATE
    "${WV2_LIB}"
    ole32 oleaut32 uuid user32 gdi32 shell32 shlwapi version advapi32
    # 1.1.0 (N1): the page's visual over mpv's picture (DirectComposition) and its UI Automation tree (visual hosting)
    dcomp uiautomationcore
    # the update banner (2026-11-03): NextaleUpdate.cpp's GET of the site's manifest
    winhttp
)

target_compile_definitions(suvtitle_app PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
)

# /W4: the smokes already compile every header at /W4 (tests/cpp); the product does too. NO /utf-8: every source is
# ASCII (the house rule since AudioPB -- a typed non-ASCII character in a wide literal would be read as CP1252).
target_compile_options(suvtitle_app PRIVATE /W4)

# Our own binaries are /MT: no VC++ redistributable needed (top-level: CMP0091 NEW).
set_property(TARGET suvtitle_app PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(suvtitle_app PROPERTIES OUTPUT_NAME "SuVtitle")   # -> SuVtitle.exe

# -----------------------------------------------------------------------------
#  web\ next to the exe (S-WEB owns app\web\; S-HOST owns this copy rule).
#  NOT a POST_BUILD step: that only fires when the exe RELINKS, so a page-only edit would silently ship the PREVIOUS
#  page (AudioPB's lesson). One OUTPUT-based copy per file re-runs whenever its source changes, the files are found by
#  a CONFIGURE_DEPENDS glob (a new file of S-WEB's re-runs the configure by itself), sub-folders keep their layout
#  (main.cpp serves web\<sub>\<file> too), and the copy is a hard prerequisite of the exe: ANY build refreshes web\.
#  SUVTITLE LIVE (1.1.0, 2026-10-25): the Live window's page, web\live.html (L3's), rides the same glob -- nothing to add
#  per page; main.cpp says "web\live.html is not installed" in the Live window if it is missing, and serves it under its
#  own Content-Security-Policy (no media).
# -----------------------------------------------------------------------------
file(GLOB_RECURSE SUVTITLE_WEB_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false "${APP_DIR}/web/*")
set(_web_copied)
foreach(_web_src IN LISTS SUVTITLE_WEB_SOURCES)
    file(RELATIVE_PATH _web_rel "${APP_DIR}/web" "${_web_src}")
    set(_web_dst "${CMAKE_BINARY_DIR}/web/${_web_rel}")
    add_custom_command(OUTPUT "${_web_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_web_src}" "${_web_dst}"
        DEPENDS "${_web_src}"
        COMMENT "Copying web/${_web_rel} next to SuVtitle.exe")
    list(APPEND _web_copied "${_web_dst}")
endforeach()
add_custom_target(suvtitle_web ALL DEPENDS ${_web_copied})
add_dependencies(suvtitle_app suvtitle_web)

# The WebView2 *runtime* (Evergreen) is part of Windows 11. The loader is linked statically, so its DLL is not needed;
# it is copied anyway for loose builds (AudioPB did the same).
if(EXISTS "${WV2_DLL}")
    add_custom_command(TARGET suvtitle_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WV2_DLL}" "$<TARGET_FILE_DIR:suvtitle_app>/WebView2Loader.dll"
        COMMENT "Copying WebView2Loader.dll next to SuVtitle.exe")
endif()

# -----------------------------------------------------------------------------
#  libmpv, the native video engine (1.1.0, N1 -- docs/NATIVE_CONTRACT.md). SuVtitle.exe loads libmpv-2.dll AT RUN TIME
#  (app/MpvPlayer.cpp: <exe dir>\mpv\, else next to the exe) and declares the part of mpv's C API it uses itself, so the
#  build needs no mpv file at all, and an exe without the DLL plays everything with the page's <video>. For a dev build
#  that plays through mpv, a folder holding libmpv-2.dll is named, in order, by:
#    (1) -DSUVTITLE_MPV_DIR=<folder>                          (the cache variable)
#    (2) the environment variable SUVTITLE_MPV_DIR              (handy for a first configure through buildlock.cmd)
#    (2b) the folder (2) named at an earlier configure of this build dir (CMake re-configures by itself, often from a shell
#         without the variable: that must not drop the DLL -- the WEBVIEW2_NUGET_DIR rule above)
#  and the build copies it to <build dir>\mpv\ -- the bundle's layout. One OUTPUT-based copy: refreshed when the DLL changes.
# -----------------------------------------------------------------------------
set(SUVTITLE_MPV_DIR "" CACHE PATH "Folder holding libmpv-2.dll for a dev build (copied to <build dir>/mpv); empty = none")
set(_mpv_dir "${SUVTITLE_MPV_DIR}")
if(NOT _mpv_dir AND DEFINED ENV{SUVTITLE_MPV_DIR} AND NOT "$ENV{SUVTITLE_MPV_DIR}" STREQUAL "")
    set(_mpv_dir "$ENV{SUVTITLE_MPV_DIR}")
    set(SUVTITLE_MPV_ENV_DIR "$ENV{SUVTITLE_MPV_DIR}" CACHE INTERNAL "the SUVTITLE_MPV_DIR an earlier configure got from the environment")
endif()
if(NOT _mpv_dir AND SUVTITLE_MPV_ENV_DIR)
    set(_mpv_dir "${SUVTITLE_MPV_ENV_DIR}")
endif()
if(_mpv_dir AND EXISTS "${_mpv_dir}/libmpv-2.dll")
    message(STATUS "SuVtitle: libmpv-2.dll from ${_mpv_dir} (copied to mpv\\ next to SuVtitle.exe)")
    set(_mpv_dst "${CMAKE_BINARY_DIR}/mpv/libmpv-2.dll")
    add_custom_command(OUTPUT "${_mpv_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/mpv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mpv_dir}/libmpv-2.dll" "${_mpv_dst}"
        DEPENDS "${_mpv_dir}/libmpv-2.dll"
        COMMENT "Copying libmpv-2.dll to mpv\\ next to SuVtitle.exe")
    add_custom_target(suvtitle_mpv ALL DEPENDS "${_mpv_dst}")
    add_dependencies(suvtitle_app suvtitle_mpv)
elseif(_mpv_dir)
    message(WARNING "SuVtitle: SUVTITLE_MPV_DIR (${_mpv_dir}) holds no libmpv-2.dll: this build plays everything with the page's <video>")
else()
    message(STATUS "SuVtitle: no SUVTITLE_MPV_DIR: no libmpv-2.dll copied (the page's <video> plays everything; README, Building)")
endif()

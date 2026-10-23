@echo off
rem ===========================================================================
rem  wh_e2e (SuVtitle): the REAL app\web\index.html in a REAL WebView2 (wv2probe: second screen, never focused,
rem  muted), against the fake bridge runs\feed_tracks.jsonl (make_feed.js) and the generated media
rem  ..\media_gen\wh (..\make_media.js, skipped when already there). The switches are the product's (app\main.cpp:
rem  autoplay, video overlays off, AudioVideoTracks) + --disable-gpu (software rendering: the user may be on the GPU,
rem  and the track checks need none). Then check_e2e.js judges runs\out_tracks.jsonl: exit 0 = green.
rem  PORTED (S-WEB, 2026-10-23) from AudioPB 641f1aa's scratchpad\ui_test\wh_e2e\run_e2e.cmd: the page and its origin
rem  (https://suvtitle.local/index.html), the media host, the outputs under runs\ and the profile udf_wh\ (both
rem  git-ignored), the product's third switch (--disable-direct-composition-video-overlays, AudioPB 1.0.6).
rem ===========================================================================
setlocal
cd /d "%~dp0"
set "WV=%~dp0..\wv2probe\wv2probe.exe"
for %%I in ("%~dp0..\..\..\app\web") do set "WEB=%%~fI"
for %%I in ("%~dp0..\media_gen\wh") do set "MEDIA=%%~fI"
if not exist "%WV%" echo run_e2e: wv2probe.exe missing -- run ..\wv2probe\build.cmd first& exit /b 1
node ..\make_media.js || exit /b 1
node make_feed.js || exit /b 1
if exist runs\out_tracks.jsonl del runs\out_tracks.jsonl
"%WV%" --url https://suvtitle.local/index.html --serve "suvtitle.local=%WEB%" ^
  --map "suvtitle-media.example=%MEDIA%:denycors" ^
  --feed runs\feed_tracks.jsonl --out runs\out_tracks.jsonl --udf udf_wh ^
  --browser-args "--autoplay-policy=no-user-gesture-required --disable-direct-composition-video-overlays --enable-blink-features=AudioVideoTracks --disable-gpu" ^
  --no-smartscreen --size 1280x800 --timeout 120000 --quiet
set "RC=%ERRORLEVEL%"
echo wv2probe exit %RC%
if not "%RC%"=="0" exit /b %RC%
node check_e2e.js
exit /b %ERRORLEVEL%

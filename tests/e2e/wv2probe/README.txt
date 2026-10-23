SuVtitle copy (S-WEB, 2026-10-23): ported from AudioPB 641f1aa (scratchpad, filemode_2026-10-10, wv2probe_tool; frozen
copy in SuVtitle, scratchpad, from_audiopb_641f1aa). Only the INSTRUMENT came over: src, build.cmd (new: plain cl;
the WebView2 SDK is NOT vendored, build.cmd takes the NuGet package, see BUILD), this README and RESULTS.md (the
measurements of 10-11/10, runtime 154). The probe pages, feeds, tools, run_facts.cmd and media named under FILES below
stayed in the frozen copy. SuVtitle kits: tests/e2e/wh_e2e and tests/e2e/we_e2e (media: tests/e2e/make_media.js).
Runtime seen 23/10/2026: 155.0.4283.62. The second screen is now named DISPLAY2 (same place: -1920,0 1920x1080).

wv2probe 1.0 -- WebView2 test instrument for AudioPB (NOT product code)
=======================================================================

One WebView2 in a window that never takes focus. It loads a URL, maps/serves virtual hosts the way
the product does, replays a JSONL "feed" into the page (PostWebMessageAsJson + host commands), and
appends everything the page posts plus host events (navigation, console, JS exceptions, network,
screenshots, exit) to a JSONL "out" file. Built to drive the real ui/web/translator.html with a
fake bridge, and to measure what this machine's WebView2 can play (RESULTS.md).

Runtime measured: WebView2 154.0.4258.62 (printed at every start). SDK 1.0.2592.51 (the NuGet package, see BUILD).


BUILD
-----
  build.cmd        plain cl (the Visual Studio found by tools\vsenv.cmd: vswhere, or SUVTITLE_VS), static WebView2
                   loader, /MT -> wv2probe.exe (no DLLs). Sources: src\wv2probe.cpp (one file); the build does not
                   depend on the product's CMake tree.
                   THE SDK is the Microsoft.Web.WebView2 NuGet package, version 1.0.2592.51 (the one app\CMakeLists.txt
                   uses); nothing of it is vendored here. build.cmd looks for it, in this order:
                     1. WEBVIEW2_NUGET_DIR = an extracted package (the folder with build\native\include and
                        build\native\x64 in it);
                     2. the copy the app's configure extracted: <repo>\build\x64-Release\_deps\webview2, then x64-Debug;
                     3. sdk-cache\webview2 here (git-ignored): the download of an earlier run of this script;
                     4. else it DOWNLOADS https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2592.51 into
                        sdk-cache\ (curl.exe + tar.exe, both part of Windows 10 1803 and later).
                   (A .nupkg is a zip: unzip it by hand and set WEBVIEW2_NUGET_DIR to get the same without the download.)


WINDOW PLACEMENT (default)
--------------------------
  The window opens at --pos -1880,40 (default), i.e. INSIDE THE SECOND SCREEN (\\.\DISPLAY12 =
  (-1920,0) 1920x1080, left of the primary). If no monitor holds that point/rectangle at run time
  (MonitorFromRect/MonitorFromPoint), it falls back to the bottom-right corner of the primary work area.
  Always WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW + SW_SHOWNOACTIVATE + MA_NOACTIVATE: it never takes
  the foreground and has no taskbar button. Each run logs {"__host":"focus","stolen":false} at
  navigation and at exit, and "start" lists the monitors and the placement decision.
  The WebView is MUTED (ICoreWebView2_8::put_IsMuted) unless --sound is given.


COMMAND LINE
------------
  wv2probe --url <url> [options]
    --url <url>                           page to load (required)
    --map <host>=<folder>[:allow|deny|denycors]
                                          SetVirtualHostNameToFolderMapping (repeatable; default denycors).
                                          Served INSIDE WebView2: WebResourceRequested never sees it.
    --serve <host>=<folder>               serve https://<host>/ (and http://) from <folder> through
                                          WebResourceRequested, Content-Type by extension +
                                          "Cache-Control: no-store, must-revalidate", like the
                                          product's patchbus.local. Sub-folders ok, ".." refused (403),
                                          missing file -> 404 (logged). No Range support. (repeatable)
    --feed <file.jsonl>                   run the feed after the FIRST NavigationCompleted (below)
    --out <file.jsonl>                    APPEND page messages + host events, one JSON per line
    --exit-on <substring>                 exit 0 when a page message contains it (repeatable)
    --timeout <ms>                        hard end of the run, exit code 2 (default 60000, 1000..86400000)
    --shot <file.png>                     CapturePreview PNG at exit (also at timeout)
    --size <W>x<H>                        client size in physical px (default 1280x800)
    --pos <X>,<Y>                         window top-left (default -1880,40 = second screen, see above)
    --scale <f>|auto                      RasterizationScale (default 1: CSS px == screenshot px)
    --udf <folder>                        WebView2 user-data folder (default <exe dir>\udf)
    --browser-args "<args>"               AdditionalBrowserArguments
    --net                                 log CDP Network events: request (method, Range header), response
                                          (status, MIME, Content-Range, ACAO...), failure (CORS reason), done
    --log-serve                           log every request answered by --serve
    --sound                               do not mute the WebView
    --product-settings                    context menus / status bar / DevTools off, like AudioPBConfig
                                          (CDP console capture keeps working)
    --no-smartscreen                      ICoreWebView2Settings8::put_IsReputationCheckingRequired(FALSE)
                                          (removes the ~2 s stall on every *.local navigation; RESULTS.md C)
    --quiet                               stdout gets only important lines; --out still gets everything

  Relative paths (feed, out, shot, folders, __shot) resolve against the CURRENT DIRECTORY.
  On this machine cmd.exe does not search the current directory for programs: call wv2probe by
  its full path in .cmd files (see run_facts.cmd).

  Exit codes: 0 ok | 1 bad args / feed error / init failure | 2 --timeout | 3 watchdog (UI thread stuck
              20 s past --timeout: TerminateProcess) | 4 __waitfor timed out | 5 first navigation failed
              | 6 browser or render process died | N = {"__exit":N}
  It cannot hang: the UI-thread timer ends the run at --timeout, and the exit screenshot has its own
  12 s grace. The watchdog thread kills the process if the UI thread itself is stuck.


FEED FORMAT (UTF-8, one JSON value per line; '#' or '//' lines and blank lines are comments)
-----------
  A line whose FIRST key starts with "__" is a host command. Any other JSON is posted to the page
  with PostWebMessageAsJson (exactly what the product's bridge does). Lines run in order, each
  async command finishes before the next line runs. All lines are validated before the window opens.
    {"__sleep":ms}                        wait (message pump keeps running)
    {"__waitfor":"substr","ms":10000}     wait for a PAGE message containing substr. It also matches
                                          messages received since the previous line started, so a reply
                                          that raced ahead is not missed (the first line also sees
                                          messages posted during page load). Timeout -> exit 4.
    {"__eval":"js"}                       ExecuteScript. The result is logged (promises are NOT awaited).
    {"__gesture":"js"}                    CDP Runtime.evaluate with userGesture:true, awaitPromise:true,
                                          returnByValue:true. Counts as a user click: autoplay with
                                          sound, requestFullscreen, element.click() on buttons.
    {"__cdp":"Method","params":{...}}     any DevTools-protocol call, e.g. Input.dispatchMouseEvent
                                          (a real mouse click at CSS-pixel x,y)
    {"__shot":"path.png"}                 CapturePreview PNG (10 s cap, logged with byte size)
    {"__setfiles":"css selector","files":["C:\\abs\\path"]}
                                          CDP DOM.setFileInputFiles: real file paths on an
                                          <input type=file> (file-backed File objects, like a drop)
    {"__nav":"url"}                       navigate, wait for NavigationCompleted
    {"__size":"WxH"}                      resize the window/WebView
    {"__mark":any}                        write a marker into --out
    {"__exit":code}                       end the run with that exit code
  When the feed ends: exit 0, unless --exit-on was given (then wait for it, or --timeout).
  Without --feed: run until --exit-on matches or --timeout.


OUT FORMAT (--out, JSONL, appended)
----------
  Page messages: the raw JSON the page posted (chrome.webview.postMessage), unchanged.
  Host events: {"__host":"<kind>","t":<ms since start>, ...} with kind =
    start (runtime, monitors, placement) | view | map | serve-host | nav-start | nav (ok, webError, http, ms)
    | focus | post (each feed message, so ordering is visible) | waitfor | waitfor-timeout | eval | gesture | cdp
    | shot | setfiles | msg-objects (ICoreWebView2File paths from postMessageWithAdditionalObjects)
    | console (Runtime.consoleAPICalled) | exception (Runtime.exceptionThrown) | log (Log.entryAdded:
    CORS errors, failed loads, security) | net (--net) | serve (404s; all with --log-serve)
    | fullscreen (ContainsFullScreenElementChanged) | process-failed | env-error | timeout | exit-begin | exit.


EXAMPLE 1 -- load a page, post two messages, take a shot (feeds\example.jsonl, probe\echo.html)
---------
  feeds\example.jsonl:
    {"__waitfor":"\"ready\"","ms":5000}
    {"type":"hello","n":1}
    {"type":"hello","n":2,"text":"繁體中文：對白音量剛好"}
    {"__waitfor":"\"n\":2","ms":5000}
    {"__eval":"document.getElementById('log').textContent.split('\\n').length - 1"}
    {"__shot":"runs/example.png"}
    {"__exit":0}

  wv2probe.exe --url https://probe.example/echo.html --serve "probe.example=probe"
               --feed feeds\example.jsonl --out runs\example.jsonl
  -> runs\example.jsonl holds {"type":"ready",...}, two "post" events, the two {"type":"echo",...}
     replies, the eval result (2), the shot event; runs\example.png shows both messages.


EXAMPLE 2 -- the REAL translator.html with a fake bridge (feeds\translator_demo.jsonl)
---------
  wv2probe.exe --url https://suvtitle.local/index.html --serve "suvtitle.local=<repo>\app\web"
               --product-settings --feed feeds\translator_demo.jsonl --out runs\translator_demo.jsonl
  The page posts {"type":"ready"}. The feed answers with translatorState, two translatorLine, one
  translatorPartial, takes runs\translator_fake_bridge.png, sends translatorEnd, then clicks the page's
  Clear button with a __gesture. The page's reply {"type":"translatorCtl","clear":true} lands in --out.
  (Read-only use of the product folder. The page's localStorage lives in wv2probe's own udf.)


EXAMPLE 3 -- the planned FILE MODE topology (page on one origin, media on a DENY_CORS mapping)
---------
  wv2probe.exe --url https://patchbus.local/subs.html --serve "patchbus.local=probe"
               --map "audiopb-media.example=media:denycors" --feed feeds\subs.jsonl --out runs\subs.jsonl


GOTCHAS
-------
  * Same --udf back-to-back: the second run waits ~5 s for the previous browser process (logged as a
    slow "view"). Different --udf folders do not wait.
  * Every navigation to a *.local host costs ~2.0 s (SmartScreen) unless --no-smartscreen. Media and
    sub-resource requests on .local hosts are NOT delayed.
  * Muted <audio>.play() needs a user activation (muted <video> does not): use {"__gesture":"1"} first.
  * ::cue font-size: see RESULTS.md D (bare video::cue font-size is ignored, use ::cue(.class)).
  * --out is appended: delete it between runs if you parse it.


FILES
-----
  wv2probe.exe, build.cmd, src\wv2probe.cpp (the WebView2 SDK 1.0.2592.51 comes from the NuGet package, see BUILD)
  probe\   echo.html (smoke test) | matrix.html (A + B + "show" frame check) | cors.html (C) | timing.html
           (navigation/resource timing) | subs.html (D) | misc.html (E + autoplay)
  feeds\   example | matrix, seek, cors, misc (generated by tools\make_feeds.py) | subs, subs2-4 | frames
           | navtime_* | timing | mediacaps | translator_demo | ready_then_exit
  tools\   make_media.py (ffmpeg clips -> media\ + manifest.json; refuses a non-empty media\) |
           make_feeds.py | report_matrix.py (matrix jsonl -> Markdown table) | png_lines.py (measure
           cue text lines in screenshots)   -- run with python -I (a Python 3, standard library only)
  run_facts.cmd   reruns everything behind RESULTS.md (about 2.5 min, windows on the 2nd screen)
  media\   generated clips (227 MB) | runs\  out files + screenshots | udf*\  disposable WebView2 profiles

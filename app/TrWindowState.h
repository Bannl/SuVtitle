// =============================================================================
// SuVtitle - app/TrWindowState.h   (the window's posted state)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TrWindowState.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: nothing but this header. In SuVtitle the window IS
// the app (always shown while it runs) and its page has only the File mode: the one-shot `file` request is kept
// (parity: --play still asks it), the page may ignore it.
// FIX ROUND 2026-10-23 (review RV4-7, SPLIT_PLAN ADDENDUM): `topmost` -- the window's REAL always-on-top state -- on
// EVERY post; the page's pin follows it.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// THE TRANSLATOR WINDOW'S OWN STATE, as the bridge tells it to the window's page
// (translatorWindow{shown, fullscreen[, file]}): two FACTS of the window -- on screen or closed to
// the tray, full screen or not (main.cpp owns both) -- and one REQUEST: the tray's
// "AudioPB Player..." item asking the page to show its File tab.
//
// The request is ONE-SHOT. It is carried by the first post that reaches a page which has said
// `ready` since its last (re)load, and then it is forgotten. Review of WE, C1: the flag that held it
// was never consumed (the "page is ready" bit was never set), so from the first use of the tray item
// on, EVERY post -- each show and hide, each full-screen change, each page reload -- told the page
// "File", dragging a user who had gone back to Live to the File tab, whose playerView{file} can start
// a pre-analysis that PAUSES the live translation in the middle of a meeting.
//
// Pure (no Win32, no WebView): bridge.cpp holds one and posts what post() returns;
// scratchpad/ui_test/player_smoke.cpp proves "Live stays Live" on it. ASCII only (no /utf-8).
// =============================================================================
#pragma once

#include <string>

#include "JsonW.h"

namespace pbplayer {

struct WindowState {
    bool shown = false;        // the window is on screen (not closed to the tray)
    bool fullscreen = false;   // the WINDOW is full screen (main.cpp makes it borderless)
    bool topmost = false;      // the window IS always on top: Windows' own WS_EX_TOPMOST, read back by main.cpp (RV4-7)
    bool showFile = false;     // pending: the next post to a READY page says file:true
    bool pageReady = false;    // the window's page has said `ready` since its last (re)load

    void attached()  { shown = true; pageReady = false; }   // a new view: its page is still loading
    void detached()  { shown = false; pageReady = false; }
    void reloading() { pageReady = false; }                 // its page navigates again (a new folder)
    void ready()     { pageReady = true; }                  // ...and has said `ready`
    void askFile()   { showFile = true; }                   // the tray's "AudioPB Player..." (SuVtitle: --play)

    // Nothing is posted to a page that has not said `ready`: it would be lost, or -- during a reload --
    // reach the OLD document. Its `ready` gets the whole state (the bridge posts on it).
    bool canPost() const { return pageReady; }

    // The message; `file` only to a ready page, and then CONSUMED.
    std::wstring post() {
        const bool file = showFile && pageReady;
        patchbus::JsonW j;
        j.obj().kv(L"type", L"translatorWindow").kv(L"shown", shown).kv(L"fullscreen", fullscreen).kv(L"topmost", topmost);
        if (file) j.kv(L"file", true);
        j.end();
        if (file) showFile = false;
        return j.take();
    }
};

}   // namespace pbplayer

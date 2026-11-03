// =============================================================================
// SuVtitle - app/NextaleUpdate.h   (NEW 03/11/2026: the update banner's native half)
//
// PORTED from AudioPB (PatchBus repo, ui/NextaleUpdate.h + .cpp, branch APB_TRANSLATOR): the same protocol, the same
// version rule, the same URL guard -- for SuVtitle, and stricter where SuVtitle's rules ask it:
//   * the manifest is SuVtitle's: the NEXTALE release tool publishes
//       https://www.nextale-audio.com/updates/suvtitle.json = {"version":"1.2.0","url":"https://www.nextale-audio.com/suvtitle","notes":"..."}
//     and the app reads it ONCE per start, off its UI thread (bridge.cpp, "THE UPDATE BANNER"); any failure -- offline, an
//     HTTP error (the 404 of a manifest not published yet), a malformed or oversized answer, a manifest whose page is not
//     on nextale-audio.com -- is "no update", silently;
//   * SuVtitle is free software (GPL v3); NEXTALE Audio sells a ready-made installer. The banner's Download opens
//     SuVtitle's PAGE on the site, pageUrl() = kPageUrl + "?update=<new>&from=<this version>": ONE constant, two versions
//     checked by versionOk (the installer is on that page). The manifest's `url` is CHECKED (siteUrl: a manifest whose page
//     is off the site is refused whole) but never OPENED: Info.url is never what the browser is handed;
//   * the manifest is read by a real JSON parser (JsonR.h), never by a text search: a "version" inside the notes cannot be
//     taken for the version, and \uXXXX escapes decode;
//   * nothing is opened but what urlAllowed() lets through -- EXACTLY https://www.nextale-audio.com/... or https://
//     nextale-audio.com/... -- and the page never hands a URL back: the bridge opens what IT built (pageUrl;
//     tests\web\test_host_pins.js D1: no member the bridge reads from the page names a url);
//   * a TEST INSTANCE never asks the site (bridge.cpp: only a loopback server named by SUVTITLE_TEST_UPDATE_URL) nor opens
//     the browser, and the fetcher refuses plain http anywhere but on the loopback.
// The pure parts are proved by tests\cpp\update_smoke.cpp (against a loopback server: no test contacts the site); its
// mutation sentinels are tests\cpp\cmut_upd.py. ASCII only (the build has no /utf-8).
// =============================================================================
#pragma once

#include <string>

namespace nextale {
namespace Update {

// SuVtitle's manifest (the release tool's product id is "suvtitle").
constexpr const wchar_t* kManifestUrl = L"https://www.nextale-audio.com/updates/suvtitle.json";
// SuVtitle's PAGE on the site -- the one address the banner's Download opens: ONE constant, the only place it is written;
// pageUrl() adds "?update=<new>&from=<this version>".
constexpr const char* kPageUrl = "https://www.nextale-audio.com/suvtitle";
constexpr unsigned long kTimeoutMs = 10000;   // each WinHTTP step: resolve, connect, send, receive
constexpr size_t kBodyMax = 64 * 1024;        // a manifest is a few hundred bytes: more is not one
constexpr size_t kNotesMax = 500;             // bytes of notes kept (one line, cut on a UTF-8 boundary)

struct Info {
    bool        available = false;   // a STRICTLY newer, well-formed version is published
    std::string version;             // the manifest's version (versionOk)
    std::string url;                 // SuVtitle's page, as published (siteUrl) -- checked, never opened (pageUrl is)
    std::string notes;               // one line (control characters made spaces), <= kNotesMax bytes, UTF-8
};

// ---- pure --------------------------------------------------------------------------------------------------------
// A version the release tool writes: 1 to 3 dot-separated groups of 1-9 digits ("1", "1.2", "1.2.3"), optionally "-" and a
// prerelease tag of [0-9A-Za-z.-] ("1.2.0-beta", "1.2.0-rc.1"), 32 characters at most. Its alphabet is the URL's unreserved
// one: it goes into pageUrl's query as it is.
bool versionOk(const std::string& v);
// Is `remote` strictly newer than `local`? AudioPB's rule: major.minor.patch compared as numbers (a missing part is 0), and at
// equal numbers a release supersedes a prerelease ("1.2.0" after "1.2.0-beta"); two prereleases of one number are equal.
bool isNewer(const std::string& remote, const std::string& local);
// A prerelease: a version with a "-" tag ("1.2.0-beta", "1.2.0-rc.1").
bool isPrerelease(const std::string& v);
// Is `remote` OFFERED to a build of `local`? Strictly newer (isNewer) -- and a PRERELEASE only to a prerelease build: a release
// build is never offered a beta (review of 03/11, N12: "1.2.0-beta" in the manifest would have reached every 1.1.0 user); a
// prerelease build is offered a newer prerelease, and the release of its own number.
bool offered(const std::string& remote, const std::string& local);
// SuVtitle's SITE: "https://www.nextale-audio.com/..." or "https://nextale-audio.com/..." (the slash after the host is part of
// the rule: no lookalike host, no user:password@), printable ASCII without a space, a fragment or " < > \ ^ ` { | }, 512
// characters at most. The manifest's `url` must be one.
bool siteUrl(const std::string& url);
// THE ONLY URLs SuVtitle ever opens: a siteUrl. Nothing else: no other host, scheme, port, credentials.
bool urlAllowed(const std::string& url);
// SuVtitle's page for an update: EXACTLY kPageUrl + "?update=<newVersion>&from=<currentVersion>" -- the address is the
// constant's, never the manifest's (Info.url) nor the page's. "" = refused: either version not versionOk (its alphabet is the
// URL's unreserved one, so a version that passes needs no escape and can add no parameter, path or fragment).
std::string pageUrl(const std::string& newVersion, const std::string& currentVersion);
// The manifest's three strings (UTF-8 in, UTF-8 out; a leading BOM ignored): false = not a JSON object, `version` or `url`
// absent or not a string, `notes` present but neither a string nor null. Nothing is validated here but the JSON (evaluate
// does).
bool parseManifest(const std::string& body, std::string& version, std::string& url, std::string& notes);
// The whole rule on a manifest's bytes: parsed, version versionOk, url siteUrl (checked -- a page off the site refuses the
// whole manifest -- but never opened: pageUrl is), notes made one line -- and available only when the version is OFFERED
// to `currentVersion` (newer; a prerelease only to a prerelease). Anything wrong = {available:false}.
Info evaluate(const std::string& body, const std::string& currentVersion);
// SUVTITLE_TEST_UPDATE_URL (a TEST INSTANCE's manifest, bridge.cpp): plain http on the loopback ONLY --
// "http://127.0.0.1:<port>/..." or "http://localhost:<port>/..." -- printable ASCII, <= 512.
bool loopbackUrlOk(const std::wstring& url);

// ---- the network (blocking: OFF the UI thread) ----------------------------------------------------------------------
// GET `url` -> `body` (2xx only, <= kBodyMax bytes -- more is refused, never truncated --, every WinHTTP step <= timeoutMs,
// and no read started past 3 x timeoutMs: a server that drips bytes is dropped). https anywhere, plain http only on the
// loopback (refused before anything is opened otherwise); the loopback never goes through a proxy and never follows a
// redirect (it could send the fetch off the machine). `userAgent` = "SuVtitle/<version>". False = any failure.
bool fetch(const std::wstring& url, const std::wstring& userAgent, std::string& body, unsigned long timeoutMs = kTimeoutMs);
// fetch + evaluate. Any failure = {available:false}.
Info check(const std::string& currentVersion, const std::wstring& manifestUrl = kManifestUrl,
           unsigned long timeoutMs = kTimeoutMs);
// Open `url` in the default browser -- only if urlAllowed (checked again here, whatever the caller did). True = launched.
bool openInBrowser(const std::string& url);

} // namespace Update
} // namespace nextale

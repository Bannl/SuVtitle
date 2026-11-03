// =============================================================================
// SuVtitle tests/cpp/trmgr/fake_helper.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/trmgr/fake_helper.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include (common/TrPipe.h); built as SuVtitleEngine.exe.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// fake_helper -- built as SuVtitleEngine.exe NEXT TO trmgr_smoke.exe, so the REAL
// ui/TranslatorManager spawns it exactly as it spawns the real helper (same pipe, same
// HELLO, same supervision). It speaks only the FILE MODE half of TrPipe.h, scripted by the
// FILE's PATH:
//   ok3       FPROG loading (total + hasVideo, before any "model"), FPROG analyzing, 3 FCUE,
//             FDONE ok 3
//   hold      FPROG loading, then waits: FCANCEL -> FDONE cancelled; a newer FILE -> cancelled
//   crash     FPROG loading, then the process dies (no FDONE: the tray must make one up)
//   doneexit  2 FCUE + FDONE ok 2, then the process exits AT ONCE (the tray must read the
//             real FDONE out of the pipe before it declares the death)
//   flood     600 LINE with 50 FCUE among them, written as fast as the pipe takes them,
//             then FDONE ok 50
// FPROXY with src "proxyhold" is held (FCANCEL -> ok=0 "cancelled"; a newer FPROXY ->
// ok=0 "superseded"); any other src gets a small file at out + ok=1.
// Every message received is appended to fake_rx.log (one line, tabs shown as |), after a
// "START <pid>" line per incarnation -- that log is how the smoke proves what was SENT.
//
// SUVTITLE LIVE (1.1.0, L2 2026-10-25): the LIVE half too, scripted by the STRIP's LABEL and the live CTL:
//   LIVE 1 <strips>  STATE live=1 (+ the live pair it was given), then INPUT <key> open per strip; a strip labelled
//                    "crash" makes the process die ~100 ms later (a capture driver taking the engine down: RV2-5),
//                    one labelled "levels" makes it write 500 LEVELS back to back (the newest = -12.5 dBFS)
//   LIVE 0           STATE live=0
//   SRCLIST          SRC x3 (in, out, app) + SRCEND, a new gen each time; the SECOND list of an incarnation is SHORT
//                    (SRCEND says 4: the manager must ask again), and so is every list from the FOURTH on (a list that
//                    stays short: the manager asks again twice, then delivers it as it is)
//   CTL scope=live   STATE echo of the live pair; with clear=1 and langA "dr": 300 LINE (segId 1..300) then the
//                    process exits AT ONCE -- AudioPB 1.1.0's rv5_drain helper (RV5-1), on the live CTL
// FIX ROUND (2026-10-26): a LIVE 1 with a strip labelled "refuse" is REFUSED the engine's way (one STATE whose err says
// why, the live state unchanged: no INPUT); with the file "srcdie.flag" next to the exe, a SRCLIST makes the process die
// (an engine that crashes while it enumerates: every incarnation, while the flag is there).
// U3 (2026-11-02, the TRANSCRIPTION): FVOICE, in the FILE job's slot (a FILE or an FVOICE cancels the held job, either
// kind), scripted by its PATH:
//   voice3      FPROG voices, ONE FSPK (one label per piece: 1, 2, 1, ...; 2 speakers, model "campplus"), FDONE ok <n>
//   voicehold   FPROG voices, then waits: FCANCEL -> FDONE cancelled; a newer FILE / FVOICE -> cancelled (no FSPK)
//   voicecrash  FPROG voices, then the process dies (no FDONE: the tray must make one up)
// =============================================================================
#include "../../../common/TrPipe.h"

#include <cstdio>
#include <share.h>
#include <string>

namespace tp = pbt::trpipe;

static std::wstring g_log;

// SHARED opens on both sides (_wfopen_s is exclusive: the smoke polls this log every 20 ms, and a
// line whose append met that read was silently LOST -- the 2026-10-10 "(held again)" flake), plus
// a short retry for the AV scan window after a write.
static void logLine(const std::string& s) {
    FILE* f = nullptr;
    for (int i = 0; i < 25 && !f; ++i) {
        f = _wfsopen(g_log.c_str(), L"ab", _SH_DENYNO);
        if (!f) ::Sleep(20);
    }
    if (!f) return;
    std::string v = s;
    for (auto& c : v) if (c == '\t') c = '|';
    std::fprintf(f, "%s\n", v.c_str());
    std::fclose(f);
}

static void send(HANDLE pipe, const std::string& m) { tp::writeMessage(pipe, m, 3000); }

static void prog(HANDLE p, uint64_t job, const char* phase, uint64_t done, uint64_t total) {
    tp::WireFileProg g;
    g.jobId = job; g.phase = phase; g.doneMs = done; g.totalMs = total;
    g.asrModel = "ggml-large-v3-turbo.bin"; g.mtModel = "fake-mt.gguf"; g.hasVideo = true;
    send(p, tp::packFileProg(g));
}
static void cue(HANDLE p, uint64_t job, uint32_t idx) {
    tp::WireCue c;
    c.jobId = job; c.idx = idx; c.startMs = 1000ULL * idx; c.endMs = 1000ULL * idx + 900;
    c.lang = "en"; c.dstLang = "fr"; c.src = "line " + std::to_string(idx); c.dst = "ligne " + std::to_string(idx);
    send(p, tp::packCue(c));
}
static void done(HANDLE p, uint64_t job, const char* result, uint32_t count, const std::string& err = "") {
    tp::WireFileDone d;
    d.jobId = job; d.result = result; d.count = count; d.err = err;
    send(p, tp::packFileDone(d));
}
static void proxyDone(HANDLE p, uint64_t id, bool ok, const std::string& out, const std::string& err) {
    tp::WireProxyDone d;
    d.jobId = id; d.ok = ok; d.out = out; d.err = err;
    send(p, tp::packProxyDone(d));
}

int wmain(int argc, wchar_t** argv) {
    std::wstring pipeName;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--pipe") pipeName = argv[i + 1];
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_log = self;
    g_log = g_log.substr(0, g_log.find_last_of(L"\\/") + 1) + L"fake_rx.log";
    logLine("START " + std::to_string(::GetCurrentProcessId()));
    if (pipeName.empty()) return 2;

    HANDLE pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    DWORD mode = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    send(pipe, "HELLO\t" + tp::makeTranslatorSlotName());
    tp::WireState st;
    st.status = "listening";
    send(pipe, tp::packState(st));

    uint64_t heldJob = 0, heldProxy = 0;
    for (;;) {
        std::string msg;
        bool broken = false;
        if (!tp::readMessage(pipe, msg, 50, &broken)) {
            if (broken) return 0;
            continue;
        }
        logLine("RX " + msg);
        const auto t = tp::split(msg);
        if (t.empty()) continue;
        if (t[0] == "QUIT") return 0;
        if (t[0] == "FILE") {
            tp::WireFile f;
            if (!tp::unpackFile(t, f)) continue;
            if (heldJob) { done(pipe, heldJob, "cancelled", 0); heldJob = 0; }   // a FILE cancels a running job
            if (f.path == "ok3") {
                prog(pipe, f.jobId, "loading", 0, 9000);
                prog(pipe, f.jobId, "analyzing", 3000, 9000);
                for (uint32_t i = 0; i < 3; ++i) cue(pipe, f.jobId, i);
                done(pipe, f.jobId, "ok", 3);
            } else if (f.path == "hold") {
                prog(pipe, f.jobId, "loading", 0, 9000);
                heldJob = f.jobId;
            } else if (f.path == "crash") {
                prog(pipe, f.jobId, "loading", 0, 9000);
                ::Sleep(50);
                ::ExitProcess(3);
            } else if (f.path == "deaf") {
                // Stops READING (the tray's next big command fills the pipe and its WRITE blocks),
                // then writes its last words and closes the pipe while that write is pending: the
                // tray finds the real FDONE ok only if it drains before declaring the death.
                prog(pipe, f.jobId, "loading", 0, 9000);
                ::Sleep(1200);
                cue(pipe, f.jobId, 0);
                cue(pipe, f.jobId, 1);
                done(pipe, f.jobId, "ok", 2);
                ::CloseHandle(pipe);
                ::Sleep(15000);
                return 0;
            } else if (f.path == "doneexit") {
                cue(pipe, f.jobId, 0);
                cue(pipe, f.jobId, 1);
                done(pipe, f.jobId, "ok", 2);
                ::ExitProcess(0);
            } else if (f.path == "flood") {
                uint32_t idx = 0;
                for (int i = 0; i < 600; ++i) {
                    tp::WireLine l;
                    l.tEpochMs = 1000 + (uint64_t)i; l.lang = "en"; l.dstLang = "fr";
                    l.src = "live " + std::to_string(i); l.dst = "direct " + std::to_string(i);
                    l.segId = 1 + (uint64_t)i; l.seq = 1; l.closes = true;
                    send(pipe, tp::packLine(l));
                    if (i % 12 == 0 && idx < 50) cue(pipe, f.jobId, idx++);
                }
                while (idx < 50) cue(pipe, f.jobId, idx++);
                done(pipe, f.jobId, "ok", 50);
            } else {
                done(pipe, f.jobId, "error", 0, "unknown test file");
            }
        } else if (t[0] == "FVOICE") {                      // U3: the voice pass (the FILE job's slot)
            tp::WireVoice v;
            if (!tp::unpackVoice(t, v)) continue;
            if (heldJob) { done(pipe, heldJob, "cancelled", 0); heldJob = 0; }   // the slot is shared: either kind cancels
            std::vector<tp::MsRange> spans;
            tp::parseRanges(v.spans, spans);
            prog(pipe, v.jobId, "voices", 0, 9000);
            if (v.path == "voice3") {
                tp::WireSpk s;
                s.jobId = v.jobId; s.model = "campplus";
                for (size_t i = 0; i < spans.size(); ++i) s.labels.push_back(1u + (uint32_t)(i % 2));
                s.speakers = spans.size() >= 2 ? 2u : 1u;
                send(pipe, tp::packSpk(s));
                done(pipe, v.jobId, "ok", (uint32_t)spans.size());
            } else if (v.path == "voicehold") {
                heldJob = v.jobId;
            } else if (v.path == "voicecrash") {
                ::Sleep(50);
                ::ExitProcess(4);
            } else {
                done(pipe, v.jobId, "error", 0, "unknown test file");
            }
        } else if (t[0] == "FCANCEL") {
            uint64_t id = 0;
            if (!tp::unpackFileCancel(t, id)) continue;
            if (id == heldJob)   { done(pipe, heldJob, "cancelled", 0); heldJob = 0; }
            if (id == heldProxy) { proxyDone(pipe, heldProxy, false, "", "cancelled"); heldProxy = 0; }
        } else if (t[0] == "FPROXY") {
            tp::WireProxy p;
            if (!tp::unpackProxy(t, p)) continue;
            if (heldProxy) { proxyDone(pipe, heldProxy, false, "", "superseded"); heldProxy = 0; }
            if (p.src == "proxyhold") { heldProxy = p.jobId; continue; }
            FILE* o = nullptr;
            const std::wstring out(p.out.begin(), p.out.end());   // the smoke's paths are ASCII
            if (_wfopen_s(&o, out.c_str(), L"wb") == 0 && o) { std::fputs("RIFF", o); std::fclose(o); }
            proxyDone(pipe, p.jobId, true, p.out, "");
        // ---- SUVTITLE LIVE (1.1.0) ----------------------------------------------------------------------------------
        } else if (t[0] == "LIVE") {
            tp::WireLive l;
            if (!tp::unpackLive(t, l)) continue;            // the real engine answers STATE err; the smoke never sends one
            bool refuse = false;
            for (const auto& in : l.inputs) refuse = refuse || in.label == "refuse";
            if (refuse) {                                   // (fix round) the engine's refusal: one STATE with err, live unchanged
                tp::WireState r = st;
                r.err = "The live translation could not open these inputs.";
                send(pipe, tp::packState(r));
                continue;
            }
            st.live = l.on;
            send(pipe, tp::packState(st));
            if (!l.on) continue;
            for (const auto& in : l.inputs) {
                tp::WireInput wi;
                wi.key = in.key; wi.state = "open";
                send(pipe, tp::packInput(wi));
            }
            for (const auto& in : l.inputs) {
                if (in.label == "crash") { ::Sleep(100); ::ExitProcess(9); }
                if (in.label == "levels") {
                    for (int i = 0; i < 500; ++i) {
                        tp::WireLevels lv;
                        lv.peaks.push_back({ in.key, i == 499 ? -12.5f : -60.0f + static_cast<float>(i % 40) });
                        send(pipe, tp::packLevels(lv));
                    }
                }
            }
        } else if (t[0] == "SRCLIST") {
            {   // (fix round) an engine that crashes while it enumerates -- while the smoke's flag file is there
                const std::wstring flag = g_log.substr(0, g_log.find_last_of(L"\\/") + 1) + L"srcdie.flag";
                if (::GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) { ::Sleep(50); ::ExitProcess(8); }
            }
            static uint32_t gen = 0, lists = 0;
            ++gen; ++lists;
            const char* const rows[3][4] = { { "in", "in:{0.0.1.00000000}.{mic}", "Microphone (USB)", "default,comm" },
                                             { "out", "out:{0.0.0.00000000}.{spk}", "Speakers", "default" },
                                             { "app", "app:zoom.exe", "Zoom Meetings", "" } };
            for (const auto& r : rows) {
                tp::WireSrc s;
                s.gen = gen; s.kind = r[0]; s.key = r[1]; s.label = r[2]; s.state = "ok"; s.rate = 48000; s.ch = 2; s.flags = r[3];
                send(pipe, tp::packSrc(s));
            }
            tp::WireSrcEnd e;
            e.gen = gen; e.n = (lists == 2 || lists >= 4) ? 4u : 3u;   // the 2nd list of this incarnation is SHORT (asked again); so is every list from the 4th on (a list that STAYS short: the bound)
            send(pipe, tp::packSrcEnd(e));
        } else if (t[0] == "CTL") {
            tp::WireCtl c;
            if (!tp::unpackCtl(t, c) || !tp::ctlIsLive(c)) continue;
            if (c.clear && c.langA == "dr") {               // AudioPB 1.1.0's rv5_drain: its last words, then gone
                for (int i = 1; i <= 300; ++i) {
                    tp::WireLine l;
                    l.tEpochMs = 1000 + (uint64_t)i; l.lang = "en"; l.dstLang = "fr";
                    l.src = "live " + std::to_string(i); l.dst = "direct " + std::to_string(i);
                    l.segId = (uint64_t)i; l.seq = 1; l.closes = true;
                    send(pipe, tp::packLine(l));
                }
                ::ExitProcess(0);
            }
            if (!c.mode.empty()) st.liveMode = c.mode;
            if (!c.langA.empty() && !c.langB.empty()) { st.liveLangA = c.langA; st.liveLangB = c.langB; }
            send(pipe, tp::packState(st));                  // the echo of the live pair (the engine's STATE wins)
        }
        // anything else: logged above, nothing to answer.
    }
}

// =============================================================================
// SuVtitle - engine/TrSpeaker.h   (same-source speaker diarization)
//
// SuVtitle 1.1.0 (2026-10-25): ported VERBATIM from AudioPB's translator/TrSpeaker.h at commit 50f79d6 (AudioPB
// Translator 1.1.0), for SuVtitleEngine.exe. Only the wording of this header changed ("cable" read as the live
// path's INPUT -- a strip of the Live window --, translator.txt as engine.txt) and the comments' non-ASCII
// characters (house rule: ASCII-only C++). "The helper" below is SuVtitleEngine.exe.
//
// The nominal case is a PROFESSIONAL MEETING: five or six people on ONE input,
// possibly all speaking the same language. TrAudio's per-input attribution is exact
// but blind there -- everyone is "the loudest input", because there is only one. So
// this stage names the speaker by VOICE: one embedding per VAD segment, clustered
// online against running-mean centroids.
//
// WHAT IS BENCHED AND WHAT IS NOT. Every constant below was measured before this
// file existed (D1, 06/09, 24 synthetic-meeting utterances, 4 voices: two same-
// language en-US, one accented, one zh):
//   * model     = 3D-Speaker CAM++ zh_en_advanced (27 MB, 192 dims, 11-29 ms/segment
//                 on this CPU). ERes2NetV2 was TRIED AND REJECTED: it is trained on
//                 Chinese alone and its intra/inter margin on the English voices came
//                 out NEGATIVE (-0.087), i.e. it confuses two English speakers more
//                 than it confuses one speaker with himself. CAM++ margin: +0.132.
//   * threshold = 0.50 cosine. The bench hit 4 clusters at 100 % purity across the
//                 WHOLE 0.40-0.70 plateau, so 0.50 sits in the middle of a plateau
//                 rather than on a cliff edge.
//   * cap       = 8 speakers, short segments (< 1.2 s) attach to the previous speaker.
// The honest reserves the bench could NOT settle: TTS voices are cleaner than a real
// room, overlapped speech gets ONE label (no source separation in v1), and the first
// minute of any session is unstable while the clusters form.
//
// /!\ THE SYSTEM32 TRAP -- WHY NOTHING HERE IS LINKED IMPLICITLY.
// Windows 11 ships its own onnxruntime.dll in System32 (Windows ML, ORT 1.17.1).
// It is OLDER than the one sherpa-onnx needs, it sits on the implicit-link search
// path, and it WINS: the C API then dies with "API version [27] not available ...
// ORT 1.17.1". That was hit live during the bench, not theorised. So the whole
// sherpa surface is reached through LoadLibraryEx + GetProcAddress (ONE typedef
// table, in TrSpeaker.cpp), our own onnxruntime.dll is loaded FIRST from the payload
// folder so the loader can never reach for System32's, and this target links neither
// sherpa-onnx-c-api.lib nor onnxruntime.lib.
//
// OPTIONAL AT RUNTIME. No DLL, no model, a bad model, a refusing extractor -> the
// stage reports why and disables ITSELF. The helper then falls back to T1's per-input
// attribution, which is still exact whenever the speakers are on different inputs.
// A missing diarizer must cost the user speaker colours, never the subtitles.
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pbt {

// ---------------------------------------------------------------------------
// The benched constants of record. Named, so they can be argued with -- and
// overridable from engine.txt (SPKTHRESH / SPKMAX / SPKMINSEC / SPKRECLUSTER),
// because the bench was synthetic and a real room may want a different seat on the
// 0.40-0.70 plateau.
// ---------------------------------------------------------------------------
constexpr double kSpkThresholdDefault = 0.50;   // cosine vs the winning centroid
constexpr int    kSpkMaxDefault       = 8;      // cap; at the cap we assign to the best anyway
constexpr double kSpkMinSecDefault    = 1.2;    // shorter -> attach to the PREVIOUS speaker
constexpr int    kSpkReclusterDefault = 32;     // segments between re-cluster passes (0 = off)
constexpr int    kSpkRingMax          = 256;    // embeddings kept for the re-cluster pass
constexpr int    kSpkThreadsDefault   = 4;      // ORT intra-op threads (the bench's setting)

// The model of record, and the short tag STATE reports to the UI.
inline const char* kSpkModelFile = "3dspeaker_campplus_zh_en_advanced.onnx";
inline const char* kSpkModelTag  = "campplus";

struct SpkConfig {
    bool         on      = true;
    std::wstring model;          // full path to the .onnx ("" = nothing resolved)
    std::wstring binDir;         // folder holding sherpa-onnx-c-api.dll + onnxruntime.dll
    double       threshold = kSpkThresholdDefault;
    int          maxSpeakers = kSpkMaxDefault;
    double       minSec    = kSpkMinSecDefault;
    int          recluster = kSpkReclusterDefault;
    int          threads   = kSpkThreadsDefault;
};

// ---------------------------------------------------------------------------
// The clusterer. PURE MATH -- no sherpa, no model, no files -- so the selftest can
// exercise the remap without an inference engine, and so a defect in here is never
// confused with a defect in the loader.
//
// Online pass (the benched algorithm, verbatim): cosine against every running-mean
// centroid; best >= threshold -> that speaker, centroid updated as a running mean
// and RENORMALISED; otherwise a new speaker while we are under the cap; at the cap,
// the best centroid takes it anyway (a ninth voice joins the nearest of the eight
// rather than vanishing).
//
// Re-cluster pass: the online pass is order-dependent by construction -- whoever
// spoke first defines the centroid everyone else is measured against. Every
// `recluster` assignments we therefore re-partition the STORED embeddings with
// agglomerative AVERAGE LINKAGE at the same threshold, which has no such bias, and
// remap the result onto the labels already on screen. THE REMAP IS THE POINT: a
// speaker's colour is keyed on the label, so a label that changed meaning would
// repaint the transcript behind the user's eyes.
// ---------------------------------------------------------------------------
class OnlineClusterer {
public:
    void configure(double threshold, int maxSpeakers, int recluster);
    void reset();

    // Assign one embedding (which MUST already be L2-normalised). Returns the label
    // ("S1".."S8"). `sim` = cosine to the winning centroid, or -1 when the embedding
    // opened a new cluster. `didRecluster` = this call also ran a re-cluster pass.
    std::string add(const std::vector<float>& v, double& sim, bool& didRecluster);

    // The short-segment policy lives in TrSpeaker (it decides not to embed at all);
    // this is the answer it hands back. "" before anything has been assigned.
    const std::string& previousLabel() const { return prevLabel_; }
    bool  hasPrevious() const { return prev_ >= 0; }

    int   speakerCount() const { return (int)clusters_.size(); }
    int   assigned()     const { return assigned_; }

    // Run a re-cluster pass NOW (the selftest's proof of the remap; the live path
    // reaches it through the counter inside add()).
    bool  forceRecluster();

    // Label of cluster `i` and the labels currently live, for assertions.
    std::string labelAt(int i) const;
    std::vector<std::string> labels() const;
    // The label carried by every remembered embedding, oldest first. The selftest reads
    // it to prove what the remap promises: after a pass, each label still means the same
    // person. Nothing in the live path uses it.
    std::vector<std::string> ringLabels() const;

    static std::string labelOf(int id);          // 1 -> "S1"

private:
    struct Cluster {
        std::vector<float> centroid;             // running mean, renormalised
        int  n     = 0;                          // members folded into it
        int  label = 0;                          // 1-based; "S<label>" on screen
    };
    // One remembered embedding. `label` (not the cluster index) is what the remap
    // counts overlaps on: cluster indices are rebuilt by every pass, labels are not.
    struct Stored {
        std::vector<float> v;
        int      label = 0;
        uint64_t seq   = 0;
    };

    int  allocLabel();                           // smallest never-used, else smallest free
    int  clusterOfLabel(int label) const;
    void reclusterNow();

    double thresholdV_  = kSpkThresholdDefault;
    int    maxSpeakers_ = kSpkMaxDefault;
    int    recluster_   = kSpkReclusterDefault;

    std::vector<Cluster> clusters_;
    std::deque<Stored>   ring_;                  // last kSpkRingMax embeddings
    uint64_t             seq_       = 0;
    int                  assigned_  = 0;         // embeddings clustered since reset
    int                  sinceRecl_ = 0;
    int                  prev_      = -1;        // previous cluster INDEX
    std::string          prevLabel_;
    uint32_t             usedEver_  = 0;         // bitmask of labels ever handed out
};

// ---------------------------------------------------------------------------
// The stage: the dynamically-loaded extractor + the clusterer, with the weak-
// embedding policy between them.
//
// LIFETIME: begin() once per run, on the pipeline thread, before the first segment.
// The extractor is not thread-safe and is only ever touched from that thread.
// ---------------------------------------------------------------------------
class TrSpeaker {
public:
    TrSpeaker() = default;
    ~TrSpeaker();

    TrSpeaker(const TrSpeaker&) = delete;
    TrSpeaker& operator=(const TrSpeaker&) = delete;

    // Load the DLLs and the model. Returns true when the stage is armed; false means
    // DISABLED, NOT broken -- `why` carries one human sentence for the log and the
    // caller carries on with per-input attribution. Never throws, never crashes on a
    // missing or mismatched runtime (see the System32 note above).
    bool begin(const SpkConfig& cfg, std::string& why);
    void end();

    bool               enabled()  const { return enabled_; }
    const std::string& why()      const { return why_; }        // why it is off, or the version banner
    uint32_t           loadMs()   const { return loadMs_; }     // model load, measured
    int   speakerCount() const { return clusterer_.speakerCount(); }
    int   dim()          const { return dim_; }

    // One segment -> a label. `mono16k` is the segment's audio at 16 kHz (exactly what
    // the ASR is about to be fed). Returns "" when the stage is disabled.
    //   sim  = cosine to the winning centroid; -1 = new cluster; -2 = short-segment
    //          attach (no embedding was computed at all)
    //   ms   = embedding wall time, 0 for the short-segment path
    std::string classify(const std::vector<float>& mono16k, double& sim, uint32_t& ms);

    // U3 (2026-11-02, the TRANSCRIPT's voice pass -- main_translator.cpp FVOICE): ONE voiceprint of `mono16k` (16 kHz),
    // L2-normalised, and nothing else: no clusterer, no short-segment policy -- the voice pass embeds a whole file's pieces
    // and clusters them at once (clusterOffline below). The same guarded call and the same fault rule as classify(): three
    // faulted calls in a row disable the stage. false = disabled, "not ready" (too little audio), or a faulted call.
    bool embed(const std::vector<float>& mono16k, std::vector<float>& out, uint32_t& ms);

    OnlineClusterer&       clusterer()       { return clusterer_; }
    const OnlineClusterer& clusterer() const { return clusterer_; }

private:
    bool            enabled_ = false;
    std::string     why_;
    uint32_t        loadMs_  = 0;
    int             dim_     = 0;
    double          minSec_  = kSpkMinSecDefault;
    // Consecutive FAULTED embedding calls. A model that faults once will fault again,
    // and retrying a corrupted extractor for the rest of a two-hour meeting is not
    // resilience -- so after a few strikes the stage takes itself off the field.
    int             faults_  = 0;
    static constexpr int kMaxFaults = 3;
    OnlineClusterer clusterer_;

    // Opaque on purpose: the sherpa typedefs live in ONE place (TrSpeaker.cpp) and
    // nothing else in the build ever sees that header or that .lib.
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------------------
// U3 (2026-11-02): THE OFFLINE CLUSTERING of the transcript's voice pass. PURE MATH.
//
// OnlineClusterer::reclusterNow's rule -- average linkage, the Lance-Williams update, merge the closest pair while its
// similarity is >= `threshold`, and further while there are more than `cap` clusters -- run ONCE over a whole file's
// voiceprints. For a FILE everything is known in advance and nothing is on screen yet: no online pass (its order
// dependence is what the live re-cluster pass exists to heal) and no remap. Inputs L2-normalised; an EMPTY vector takes no
// part and is answered -1. Returns one cluster index per input, 0..k-1, numbered by each cluster's FIRST member in input
// order -- inputs in time order make it the order of first appearance.
//
// Accelerated with a NEAREST-NEIGHBOUR ARRAY (each row's best partner, rescanned only when the merge touched it): the SAME
// merges, in the same order, with the same tie-breaks and the same double arithmetic as the naive O(n^3) loop -- the
// engine's [0] U3 block proves the partitions identical. Memory: n(n-1)/2 doubles (3000 inputs = 36 MB, transient).
std::vector<int> clusterOffline(const std::vector<std::vector<float>>& v, double threshold, int cap);

} // namespace pbt

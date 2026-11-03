// =============================================================================
// SuVtitle - engine/TrSpeaker.cpp
//
// SuVtitle 1.1.0 (2026-10-25): ported VERBATIM from AudioPB's translator/TrSpeaker.cpp at commit 50f79d6 (AudioPB
// Translator 1.1.0); only this header and the comments' non-ASCII characters changed (TrSpeaker.h).
// =============================================================================
#include "TrSpeaker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace pbt {

// ===========================================================================
//  THE SHERPA SURFACE -- ONE typedef table, reached by name, never by link
//
//  These declarations are a hand copy of sherpa-onnx/c-api/c-api.h for the ten
//  entry points this stage uses. They are duplicated ON PURPOSE: including that
//  header would drag __declspec(dllimport) into the build, and the .lib that
//  answers it is the exact thing the System32 trap makes unusable. Everything
//  here is extern "C", default calling convention, x64 -- so a plain function
//  pointer is the whole ABI.
// ===========================================================================
// A NAMED namespace, not an anonymous one: TrSpeaker::Impl (below) has members of
// these types, and a class with external linkage must not be built out of
// internal-linkage pieces. Nothing outside this file ever names them.
namespace spkdll {

struct SherpaExtractor;      // opaque
struct SherpaStream;         // opaque

struct SherpaExtractorCfg {  // SherpaOnnxSpeakerEmbeddingExtractorConfig, field for field
    const char* model;
    int32_t     num_threads;
    int32_t     debug;
    const char* provider;
};

struct SherpaApi {
    const SherpaExtractor* (*CreateExtractor)(const SherpaExtractorCfg*)                 = nullptr;
    void                   (*DestroyExtractor)(const SherpaExtractor*)                   = nullptr;
    int32_t                (*Dim)(const SherpaExtractor*)                                = nullptr;
    const SherpaStream*    (*CreateStream)(const SherpaExtractor*)                       = nullptr;
    void                   (*AcceptWaveform)(const SherpaStream*, int32_t, const float*, int32_t) = nullptr;
    void                   (*InputFinished)(const SherpaStream*)                         = nullptr;
    int32_t                (*IsReady)(const SherpaExtractor*, const SherpaStream*)       = nullptr;
    const float*           (*ComputeEmbedding)(const SherpaExtractor*, const SherpaStream*) = nullptr;
    void                   (*DestroyEmbedding)(const float*)                             = nullptr;
    void                   (*DestroyStream)(const SherpaStream*)                         = nullptr;
    const char*            (*VersionStr)()                                               = nullptr;
};

// ---------------------------------------------------------------------------
// SEH guards. The failure this stage defends against does not politely return
// null -- an onnxruntime the C API refuses to talk to takes the process with it,
// which is exactly what happened on the bench machine. These two functions hold
// nothing that needs unwinding (raw pointers and ints only), which is what lets
// __try live inside them at all.
// ---------------------------------------------------------------------------
const SherpaExtractor* guardedCreate(const SherpaApi* api, const SherpaExtractorCfg* cfg) {
    __try { return api->CreateExtractor(cfg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// 1 = an embedding of `dim` floats landed in `out`; 0 = the model said "not ready"
// (too little audio); -1 = the call faulted.
int guardedEmbed(const SherpaApi* api, const SherpaExtractor* ex,
                 const float* samples, int32_t n, float* out, int32_t dim) {
    __try {
        const SherpaStream* st = api->CreateStream(ex);
        if (!st) return 0;
        api->AcceptWaveform(st, 16000, samples, n);
        api->InputFinished(st);
        int ok = 0;
        if (api->IsReady(ex, st)) {
            const float* v = api->ComputeEmbedding(ex, st);
            if (v) {
                for (int32_t i = 0; i < dim; ++i) out[i] = v[i];
                api->DestroyEmbedding(v);
                ok = 1;
            }
        }
        api->DestroyStream(st);
        return ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

} // namespace spkdll

using namespace spkdll;

namespace {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
bool fileHere(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string wideToNarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// The C API takes the model path as a NARROW string and does not say in which code
// page. Every path we generate is ASCII in the normal case, where UTF-8 and the ANSI
// code page agree byte for byte. When it is not -- a user profile with an accent in
// it -- ask Windows for the 8.3 short name, which is ASCII by construction.
std::string narrowModelPath(const std::wstring& w) {
    bool ascii = true;
    for (wchar_t c : w) if (c > 127) { ascii = false; break; }
    std::wstring use = w;
    if (!ascii) {
        wchar_t sh[MAX_PATH] = {};
        const DWORD n = ::GetShortPathNameW(w.c_str(), sh, MAX_PATH);
        if (n > 0 && n < MAX_PATH) use.assign(sh, n);
    }
    int n = ::WideCharToMultiByte(CP_ACP, 0, use.data(), (int)use.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_ACP, 0, use.data(), (int)use.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// GetTickCount64 has a ~15.6 ms granularity and the thing being timed here takes
// 11-29 ms, so a tick clock would report this stage's cost as "0 or 16 or 31" -- three
// values, none of them true. QPC, like everywhere else in the house that measures
// something small.
double nowMs() {
    LARGE_INTEGER f{}, t{};
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&t);
    return f.QuadPart ? (double)t.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

double cosineOf(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += (double)a[i] * (double)b[i];
    return acc;
}

void l2Normalise(std::vector<float>& v) {
    double acc = 0.0;
    for (float x : v) acc += (double)x * (double)x;
    const double n = std::sqrt(acc);
    if (n <= 1e-12) return;
    for (float& x : v) x = (float)((double)x / n);
}

} // namespace

// ===========================================================================
//  OnlineClusterer
// ===========================================================================
std::string OnlineClusterer::labelOf(int id) { return "S" + std::to_string(id); }

void OnlineClusterer::configure(double threshold, int maxSpeakers, int recluster) {
    thresholdV_ = threshold;
    // usedEver_ is a 32-bit mask of labels ever handed out, so 32 is a hard ceiling
    // on the cap -- far above the 8 the bench fixed, and cheaper than a set.
    maxSpeakers_ = std::max(1, std::min(32, maxSpeakers));
    recluster_   = std::max(0, recluster);
}

void OnlineClusterer::reset() {
    clusters_.clear();
    ring_.clear();
    seq_ = 0;
    assigned_ = 0;
    sinceRecl_ = 0;
    prev_ = -1;
    prevLabel_.clear();
    usedEver_ = 0;
}

int OnlineClusterer::clusterOfLabel(int label) const {
    for (size_t i = 0; i < clusters_.size(); ++i)
        if (clusters_[i].label == label) return (int)i;
    return -1;
}

std::string OnlineClusterer::labelAt(int i) const {
    if (i < 0 || i >= (int)clusters_.size()) return {};
    return labelOf(clusters_[(size_t)i].label);
}

std::vector<std::string> OnlineClusterer::labels() const {
    std::vector<std::string> out;
    out.reserve(clusters_.size());
    for (const auto& c : clusters_) out.push_back(labelOf(c.label));
    return out;
}

std::vector<std::string> OnlineClusterer::ringLabels() const {
    std::vector<std::string> out;
    out.reserve(ring_.size());
    for (const auto& s : ring_) out.push_back(labelOf(s.label));
    return out;
}

// Prefer a label NEVER handed out. Recycling a retired one would give an existing
// colour to a DIFFERENT person, which is precisely what this whole file exists to
// prevent; only when all of them have been used does a free label get a second life.
int OnlineClusterer::allocLabel() {
    uint32_t live = 0;
    for (const auto& c : clusters_)
        if (c.label >= 1 && c.label <= 32) live |= (1u << (c.label - 1));
    for (int id = 1; id <= maxSpeakers_; ++id) {
        const uint32_t bit = 1u << (id - 1);
        if (!(usedEver_ & bit) && !(live & bit)) { usedEver_ |= bit; return id; }
    }
    for (int id = 1; id <= maxSpeakers_; ++id) {
        const uint32_t bit = 1u << (id - 1);
        if (!(live & bit)) { usedEver_ |= bit; return id; }
    }
    return maxSpeakers_;      // unreachable: the caller checked the cap first
}

std::string OnlineClusterer::add(const std::vector<float>& v, double& sim, bool& didRecluster) {
    sim = -1.0;
    didRecluster = false;
    if (v.empty()) return prevLabel_;

    int    best    = -1;
    double bestSim = -2.0;
    for (size_t i = 0; i < clusters_.size(); ++i) {
        const double s = cosineOf(clusters_[i].centroid, v);
        if (s > bestSim) { bestSim = s; best = (int)i; }
    }

    auto fold = [&](int at) {                    // running mean, then RENORMALISE
        Cluster& c = clusters_[(size_t)at];
        const double k = (double)c.n;
        const size_t d = std::min(c.centroid.size(), v.size());
        for (size_t j = 0; j < d; ++j)
            c.centroid[j] = (float)(((double)c.centroid[j] * k + (double)v[j]) / (k + 1.0));
        l2Normalise(c.centroid);
        ++c.n;
    };

    int at;
    if (best >= 0 && bestSim >= thresholdV_) {
        at  = best;
        sim = bestSim;
        fold(at);
    } else if ((int)clusters_.size() < maxSpeakers_) {
        Cluster c;
        c.centroid = v;
        c.n        = 1;
        c.label    = allocLabel();
        clusters_.push_back(std::move(c));
        at  = (int)clusters_.size() - 1;
        sim = -1.0;
    } else {
        // At the cap. The ninth voice joins the NEAREST of the eight rather than
        // vanishing -- a wrong colour is recoverable, a blank one is not. Note that the
        // centroid is deliberately NOT folded here (the benched behaviour): an
        // embedding we already judged out-of-family must not drag the family's centre.
        at  = best;
        sim = bestSim;
    }

    prev_      = at;
    prevLabel_ = labelOf(clusters_[(size_t)at].label);

    Stored s;
    s.v     = v;
    s.label = clusters_[(size_t)at].label;
    s.seq   = seq_++;
    ring_.push_back(std::move(s));
    while ((int)ring_.size() > kSpkRingMax) ring_.pop_front();

    ++assigned_;
    ++sinceRecl_;
    if (recluster_ > 0 && sinceRecl_ >= recluster_ && (int)ring_.size() >= 2) {
        sinceRecl_ = 0;
        reclusterNow();
        didRecluster = true;
    }
    return prevLabel_;
}

bool OnlineClusterer::forceRecluster() {
    if ((int)ring_.size() < 2) return false;
    sinceRecl_ = 0;
    reclusterNow();
    return true;
}

// ---------------------------------------------------------------------------
// The re-cluster pass.
//
// WHY IT EXISTS: the online pass is order-dependent by construction. Whoever spoke
// first defines the centroid everybody else is measured against, so an unlucky first
// utterance (a cough, a "yeah", a phrase said over someone) can seat a speaker badly
// and every later segment inherits that seat. Average-linkage agglomerative
// clustering over the same embeddings has no such bias, so re-running it and adopting
// the result heals the session as it goes.
//
// WHY THE REMAP IS THE HARD PART: colours are keyed on the label. A pass that returns
// a BETTER partition but renames it has repainted the transcript behind the user's
// eyes, which is worse than the imperfect partition it replaced. So the new clusters
// are matched onto the labels already on screen by MAXIMUM MEMBER OVERLAP (ties to
// the older label), then two named rules finish the job: a MERGE keeps the OLDER of
// the two labels it swallowed, and a SPLIT gives the old label to the biggest piece
// and the next free label to the rest.
// ---------------------------------------------------------------------------
void OnlineClusterer::reclusterNow() {
    const int n = (int)ring_.size();
    if (n < 2) return;
    const int dim = (int)ring_[0].v.size();
    if (dim <= 0) return;

    // Clusters with NO member left in the window are outside this pass's evidence
    // entirely. Carry them over verbatim: a speaker must not lose his identity (and
    // his colour) merely for having been quiet for 256 utterances.
    uint32_t inWindow = 0;
    for (const auto& s : ring_)
        if (s.label >= 1 && s.label <= 32) inWindow |= (1u << (s.label - 1));
    std::vector<Cluster> carried;
    uint32_t claimed = 0;
    for (const auto& c : clusters_) {
        const uint32_t bit = (c.label >= 1 && c.label <= 32) ? (1u << (c.label - 1)) : 0u;
        if (bit && !(inWindow & bit)) { carried.push_back(c); claimed |= bit; }
    }
    const int budget = maxSpeakers_ - (int)carried.size();
    if (budget < 1) return;          // nothing left to spend: leave the partition alone

    // ---- average-linkage agglomeration, Lance-Williams update ------------------
    std::vector<double> sim((size_t)n * (size_t)n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double s = cosineOf(ring_[(size_t)i].v, ring_[(size_t)j].v);
            sim[(size_t)i * n + j] = s;
            sim[(size_t)j * n + i] = s;
        }
    std::vector<int>  size((size_t)n, 1);
    std::vector<char> active((size_t)n, 1);
    std::vector<std::vector<int>> members((size_t)n);
    for (int i = 0; i < n; ++i) members[(size_t)i].push_back(i);
    int nact = n;

    while (nact > 1) {
        double bs = -2.0;
        int bi = -1, bj = -1;
        for (int i = 0; i < n; ++i) {
            if (!active[(size_t)i]) continue;
            for (int j = i + 1; j < n; ++j) {
                if (!active[(size_t)j]) continue;
                const double s = sim[(size_t)i * n + j];
                if (s > bs) { bs = s; bi = i; bj = j; }
            }
        }
        if (bi < 0) break;
        // Stop when the closest pair is genuinely separated AND we are inside the cap.
        // The second half is not decoration: agglomeration at a threshold can return
        // more clusters than the cap allows, and the cap is a promise to the palette.
        if (bs < thresholdV_ && nact <= budget) break;

        for (int k = 0; k < n; ++k) {
            if (!active[(size_t)k] || k == bi || k == bj) continue;
            const double v = ((double)size[(size_t)bi] * sim[(size_t)bi * n + k]
                            + (double)size[(size_t)bj] * sim[(size_t)bj * n + k])
                           / (double)(size[(size_t)bi] + size[(size_t)bj]);
            sim[(size_t)bi * n + k] = v;
            sim[(size_t)k * n + bi] = v;
        }
        size[(size_t)bi] += size[(size_t)bj];
        members[(size_t)bi].insert(members[(size_t)bi].end(),
                                   members[(size_t)bj].begin(), members[(size_t)bj].end());
        members[(size_t)bj].clear();
        active[(size_t)bj] = 0;
        --nact;
    }

    // ---- the new partition, with its oldest member (the deterministic tie-break) --
    struct NewC { std::vector<int> mem; uint64_t oldest = 0; int label = 0; };
    std::vector<NewC> fresh;
    for (int i = 0; i < n; ++i) {
        if (!active[(size_t)i] || members[(size_t)i].empty()) continue;
        NewC c;
        c.mem = members[(size_t)i];
        c.oldest = ring_[(size_t)c.mem[0]].seq;
        for (int m : c.mem) c.oldest = std::min(c.oldest, ring_[(size_t)m].seq);
        fresh.push_back(std::move(c));
    }
    if (fresh.empty()) return;

    // ---- remap: maximum member overlap, ties to the OLDER label -----------------
    struct Tri { int overlap; int label; int c; uint64_t oldest; };
    std::vector<Tri> tri;
    for (size_t c = 0; c < fresh.size(); ++c) {
        int cnt[33] = {};
        for (int m : fresh[c].mem) {
            const int L = ring_[(size_t)m].label;
            if (L >= 1 && L <= 32) ++cnt[L];
        }
        for (int L = 1; L <= 32; ++L)
            if (cnt[L] > 0) tri.push_back({ cnt[L], L, (int)c, fresh[c].oldest });
    }
    std::sort(tri.begin(), tri.end(), [](const Tri& a, const Tri& b) {
        if (a.overlap != b.overlap) return a.overlap > b.overlap;   // most members wins
        if (a.label   != b.label)   return a.label   < b.label;     // tie -> the OLDER label
        return a.oldest < b.oldest;
    });
    std::vector<int> got(fresh.size(), 0);
    for (const auto& t : tri) {
        const uint32_t bit = 1u << (t.label - 1);
        if (got[(size_t)t.c] || (claimed & bit)) continue;
        got[(size_t)t.c] = t.label;
        claimed |= bit;
    }

    // A MERGE keeps the OLDER label: when one new cluster swallowed two labels, the
    // one the user saw first is the one that survives. The label it gives up is
    // RETIRED for this pass (not handed straight to another cluster), so the override
    // can never start a chain of renames.
    uint32_t retired = 0;
    std::vector<size_t> order(fresh.size());
    std::iota(order.begin(), order.end(), (size_t)0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return fresh[a].oldest < fresh[b].oldest; });
    for (size_t c : order) {
        int oldestLabel = 0;
        for (int m : fresh[c].mem) {
            const int L = ring_[(size_t)m].label;
            if (L >= 1 && (oldestLabel == 0 || L < oldestLabel)) oldestLabel = L;
        }
        if (!oldestLabel) continue;
        const uint32_t obit = 1u << (oldestLabel - 1);
        if (got[c] == oldestLabel || (claimed & obit) || (retired & obit)) continue;
        if (got[c]) { const uint32_t cur = 1u << (got[c] - 1); claimed &= ~cur; retired |= cur; }
        got[c] = oldestLabel;
        claimed |= obit;
    }

    // A SPLIT spawns the next free label (preferring one never handed out, as always).
    for (size_t c = 0; c < fresh.size(); ++c) {
        if (got[c]) continue;
        int pick = 0;
        for (int id = 1; id <= maxSpeakers_ && !pick; ++id) {
            const uint32_t bit = 1u << (id - 1);
            if (!(usedEver_ & bit) && !(claimed & bit) && !(retired & bit)) pick = id;
        }
        for (int id = 1; id <= maxSpeakers_ && !pick; ++id) {
            const uint32_t bit = 1u << (id - 1);
            if (!(claimed & bit) && !(retired & bit)) pick = id;
        }
        for (int id = 1; id <= maxSpeakers_ && !pick; ++id) {
            const uint32_t bit = 1u << (id - 1);
            if (!(claimed & bit)) pick = id;
        }
        if (!pick) pick = maxSpeakers_;
        got[c] = pick;
        claimed |= 1u << (pick - 1);
    }

    // ---- commit -----------------------------------------------------------------
    std::vector<Cluster> next = carried;
    for (size_t c = 0; c < fresh.size(); ++c) {
        Cluster nc;
        nc.centroid.assign((size_t)dim, 0.0f);
        for (int m : fresh[c].mem) {
            const std::vector<float>& e = ring_[(size_t)m].v;
            const size_t d = std::min((size_t)dim, e.size());
            for (size_t j = 0; j < d; ++j) nc.centroid[j] += e[j];
        }
        l2Normalise(nc.centroid);
        nc.n     = (int)fresh[c].mem.size();
        nc.label = got[c];
        for (int m : fresh[c].mem) ring_[(size_t)m].label = got[c];
        next.push_back(std::move(nc));
    }
    clusters_.swap(next);
    for (const auto& c : clusters_)
        if (c.label >= 1 && c.label <= 32) usedEver_ |= (1u << (c.label - 1));

    // The segment that triggered the pass is the LAST ring entry, and its line has not
    // been emitted yet: re-read its label so the caller ships the post-remap answer.
    if (!ring_.empty()) {
        prevLabel_ = labelOf(ring_.back().label);
        prev_      = clusterOfLabel(ring_.back().label);
    }
}

// ===========================================================================
//  TrSpeaker
// ===========================================================================
struct TrSpeaker::Impl {
    HMODULE               ort    = nullptr;    // OUR onnxruntime, loaded first on purpose
    HMODULE               lib    = nullptr;    // sherpa-onnx-c-api.dll
    DLL_DIRECTORY_COOKIE  cookie = nullptr;
    SherpaApi             api;
    const SherpaExtractor* ex    = nullptr;
    std::vector<float>     scratch;            // one embedding, reused
};

TrSpeaker::~TrSpeaker() { end(); }

bool TrSpeaker::begin(const SpkConfig& cfg, std::string& why) {
    end();
    why.clear();
    why_.clear();
    enabled_ = false;
    minSec_  = cfg.minSec;
    faults_  = 0;
    clusterer_.configure(cfg.threshold, cfg.maxSpeakers, cfg.recluster);
    clusterer_.reset();

    if (!cfg.on)          { why_ = "speaker diarization is off (SPKON 0)";      why = why_; return false; }
    if (cfg.model.empty()) { why_ = "no speaker model configured (SPKMODEL)";   why = why_; return false; }
    if (!fileHere(cfg.model)) {
        why_ = "speaker model not found: " + wideToNarrowUtf8(cfg.model);
        why = why_;
        return false;
    }
    if (cfg.binDir.empty()) { why_ = "sherpa-onnx runtime folder not found";    why = why_; return false; }

    const std::wstring ortPath = cfg.binDir + L"\\onnxruntime.dll";
    const std::wstring libPath = cfg.binDir + L"\\sherpa-onnx-c-api.dll";
    if (!fileHere(libPath)) {
        why_ = "sherpa-onnx-c-api.dll not found in " + wideToNarrowUtf8(cfg.binDir);
        why = why_;
        return false;
    }
    if (!fileHere(ortPath)) {
        why_ = "onnxruntime.dll not found in " + wideToNarrowUtf8(cfg.binDir);
        why = why_;
        return false;
    }

    Impl* im = new Impl();

    // /!\ THE SYSTEM32 TRAP, DEFENDED THREE WAYS.
    //   1. AddDllDirectory + the LOAD_LIBRARY_SEARCH_* flags. Those flags put USER_DIRS
    //      ahead of System32 in the search order, and passing any of them per call means
    //      we do NOT have to change the process-wide default search mode (which would
    //      affect every other DLL this exe loads).
    //   2. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which temporarily puts the loaded DLL's own
    //      folder at the FRONT of the list used for its dependencies -- and the
    //      dependency in question, onnxruntime.dll, is sitting right there.
    //   3. Belt and braces, and the only one that is airtight: load OUR onnxruntime.dll
    //      FIRST. Once a module of that base name is in the process, the loader hands the
    //      same one to sherpa; System32's copy can no longer be reached at all.
    im->cookie = ::AddDllDirectory(cfg.binDir.c_str());
    const DWORD flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS
                      | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;

    im->ort = ::LoadLibraryExW(ortPath.c_str(), nullptr, flags);
    if (!im->ort) {
        why_ = "onnxruntime.dll would not load (error " + std::to_string((unsigned)::GetLastError()) + ")";
        why = why_;
        if (im->cookie) ::RemoveDllDirectory(im->cookie);
        delete im;
        return false;
    }
    im->lib = ::LoadLibraryExW(libPath.c_str(), nullptr, flags);
    if (!im->lib) {
        why_ = "sherpa-onnx-c-api.dll would not load (error " + std::to_string((unsigned)::GetLastError()) + ")";
        why = why_;
        ::FreeLibrary(im->ort);
        if (im->cookie) ::RemoveDllDirectory(im->cookie);
        delete im;
        return false;
    }

    // The whole imported surface, by name, in one place. A single missing symbol
    // disables the stage rather than crashing on a null call later.
    struct Bind { const char* name; FARPROC* slot; };
    const Bind binds[] = {
        { "SherpaOnnxCreateSpeakerEmbeddingExtractor",            (FARPROC*)&im->api.CreateExtractor  },
        { "SherpaOnnxDestroySpeakerEmbeddingExtractor",           (FARPROC*)&im->api.DestroyExtractor },
        { "SherpaOnnxSpeakerEmbeddingExtractorDim",               (FARPROC*)&im->api.Dim              },
        { "SherpaOnnxSpeakerEmbeddingExtractorCreateStream",      (FARPROC*)&im->api.CreateStream     },
        { "SherpaOnnxOnlineStreamAcceptWaveform",                 (FARPROC*)&im->api.AcceptWaveform   },
        { "SherpaOnnxOnlineStreamInputFinished",                  (FARPROC*)&im->api.InputFinished    },
        { "SherpaOnnxSpeakerEmbeddingExtractorIsReady",           (FARPROC*)&im->api.IsReady          },
        { "SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding",  (FARPROC*)&im->api.ComputeEmbedding },
        { "SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding",  (FARPROC*)&im->api.DestroyEmbedding },
        { "SherpaOnnxDestroyOnlineStream",                        (FARPROC*)&im->api.DestroyStream    },
        { "SherpaOnnxGetVersionStr",                              (FARPROC*)&im->api.VersionStr       },
    };
    for (const auto& b : binds) {
        *b.slot = ::GetProcAddress(im->lib, b.name);
        if (!*b.slot) {
            why_ = std::string("sherpa-onnx is missing ") + b.name + " (wrong build?)";
            why = why_;
            ::FreeLibrary(im->lib);
            ::FreeLibrary(im->ort);
            if (im->cookie) ::RemoveDllDirectory(im->cookie);
            delete im;
            return false;
        }
    }

    const std::string modelNarrow = narrowModelPath(cfg.model);
    SherpaExtractorCfg sc{};
    sc.model       = modelNarrow.c_str();
    sc.num_threads = cfg.threads > 0 ? cfg.threads : kSpkThreadsDefault;
    sc.debug       = 0;
    sc.provider    = "cpu";      // 11-29 ms/segment measured; no GPU dependency anywhere

    const double t0 = nowMs();
    im->ex = guardedCreate(&im->api, &sc);
    loadMs_ = (uint32_t)(nowMs() - t0 + 0.5);
    if (!im->ex) {
        why_ = "the speaker model would not load: " + wideToNarrowUtf8(cfg.model);
        why = why_;
        ::FreeLibrary(im->lib);
        ::FreeLibrary(im->ort);
        if (im->cookie) ::RemoveDllDirectory(im->cookie);
        delete im;
        return false;
    }
    dim_ = (int)im->api.Dim(im->ex);
    if (dim_ <= 0 || dim_ > 4096) {
        why_ = "the speaker model reported an impossible embedding size";
        why = why_;
        im->api.DestroyExtractor(im->ex);
        ::FreeLibrary(im->lib);
        ::FreeLibrary(im->ort);
        if (im->cookie) ::RemoveDllDirectory(im->cookie);
        delete im;
        return false;
    }
    im->scratch.assign((size_t)dim_, 0.0f);

    // Say WHICH onnxruntime answered. It is the one fact this whole loading dance
    // exists to control, and "C:\Windows\System32\onnxruntime.dll" appearing here
    // would mean the trap had been walked into again -- which is not something anyone
    // should have to attach a debugger to find out.
    wchar_t ortHere[MAX_PATH] = {};
    ::GetModuleFileNameW(im->ort, ortHere, MAX_PATH);

    const char* ver = im->api.VersionStr ? im->api.VersionStr() : nullptr;
    why_ = std::string("sherpa-onnx ") + (ver ? ver : "?") + ", " + kSpkModelTag
         + ", dim " + std::to_string(dim_) + ", loaded in " + std::to_string(loadMs_) + " ms"
         + "\n             ort: " + wideToNarrowUtf8(ortHere);
    why  = why_;
    impl_ = im;
    enabled_ = true;
    return true;
}

void TrSpeaker::end() {
    if (impl_) {
        if (impl_->ex && impl_->api.DestroyExtractor) impl_->api.DestroyExtractor(impl_->ex);
        if (impl_->lib) ::FreeLibrary(impl_->lib);
        if (impl_->ort) ::FreeLibrary(impl_->ort);
        if (impl_->cookie) ::RemoveDllDirectory(impl_->cookie);
        delete impl_;
        impl_ = nullptr;
    }
    enabled_ = false;
    dim_     = 0;
}

std::string TrSpeaker::classify(const std::vector<float>& mono16k, double& sim, uint32_t& ms) {
    sim = -1.0;
    ms  = 0;
    if (!enabled_ || !impl_) return {};

    // The weak-embedding policy, benched: under ~1.2 s there is not enough voice in the
    // segment for the embedding to mean anything, so the segment ATTACHES to whoever
    // just spoke -- which is what a short interjection almost always is, a continuation
    // or an answer inside the same turn. The one exception is the first utterance of a
    // session: with nobody to attach to, a weak embedding still beats no attribution.
    const double secs = (double)mono16k.size() / 16000.0;
    if (secs < minSec_ && clusterer_.hasPrevious()) {
        sim = -2.0;                       // -2 = attached, no embedding computed
        return clusterer_.previousLabel();
    }

    const double t0 = nowMs();
    const int rc = guardedEmbed(&impl_->api, impl_->ex, mono16k.data(),
                                (int32_t)mono16k.size(), impl_->scratch.data(), (int32_t)dim_);
    ms = (uint32_t)(nowMs() - t0 + 0.5);
    if (rc != 1) {
        // "Not ready" (too little audio for the model's own front end) or a faulted call.
        // Either way this segment gets the previous speaker rather than a fabricated one.
        sim = -3.0;
        if (rc < 0 && ++faults_ >= kMaxFaults) {
            why_ = "the speaker model faulted " + std::to_string(faults_)
                 + " times in a row; diarization disabled for this session";
            ::OutputDebugStringA(("[AudioPBTranslator] " + why_ + "\n").c_str());
            enabled_ = false;             // the extractor stays loaded; we simply stop asking
        }
        return clusterer_.hasPrevious() ? clusterer_.previousLabel() : std::string();
    }
    faults_ = 0;

    std::vector<float> v(impl_->scratch.begin(), impl_->scratch.end());
    l2Normalise(v);
    bool didRecluster = false;
    return clusterer_.add(v, sim, didRecluster);
}

// U3 (2026-11-02): the voice pass' voiceprint -- classify()'s embedding half, without its policy and its clusterer.
bool TrSpeaker::embed(const std::vector<float>& mono16k, std::vector<float>& out, uint32_t& ms) {
    out.clear();
    ms = 0;
    if (!enabled_ || !impl_ || mono16k.empty()) return false;
    const double t0 = nowMs();
    const int rc = guardedEmbed(&impl_->api, impl_->ex, mono16k.data(),
                                (int32_t)mono16k.size(), impl_->scratch.data(), (int32_t)dim_);
    ms = (uint32_t)(nowMs() - t0 + 0.5);
    if (rc != 1) {
        // classify()'s rule: a model that faults keeps faulting -- three in a row and the stage is off
        if (rc < 0 && ++faults_ >= kMaxFaults) {
            why_ = "the speaker model faulted " + std::to_string(faults_)
                 + " times in a row; speaker recognition disabled for this file";
            enabled_ = false;
        }
        return false;
    }
    faults_ = 0;
    out.assign(impl_->scratch.begin(), impl_->scratch.end());
    l2Normalise(out);
    return true;
}

// ===========================================================================
//  U3 (2026-11-02): clusterOffline -- see TrSpeaker.h. The naive loop it reproduces is reclusterNow's: for every
//  merge, scan the active pairs (i ascending, j ascending) for the strictly greatest similarity, stop when it is under
//  the threshold and the count is within the cap, fold the pair with the Lance-Williams average. Here each row i keeps
//  nn[i] = the FIRST j > i holding its row's maximum (nnSim[i]); the global pick is the first row holding the greatest
//  nnSim -- the very pair the naive scan stops on. After a merge (bj into bi) only the rows the merge touched are looked at
//  again: bi's own row; a row whose partner was bi or bj (rescanned); a row i < bi, whose entry (i, bi) changed (compared).
// ===========================================================================
std::vector<int> clusterOffline(const std::vector<std::vector<float>>& v, double threshold, int cap) {
    std::vector<int> out(v.size(), -1);
    std::vector<size_t> idx;                                   // the inputs that take part
    for (size_t i = 0; i < v.size(); ++i) if (!v[i].empty()) idx.push_back(i);
    const size_t n = idx.size();
    if (n == 0) return out;
    if (cap < 1) cap = 1;
    // the upper triangle, row by row: (i, j), i < j
    std::vector<double> sim(n * (n - 1) / 2, 0.0);
    auto tri = [n](size_t i, size_t j) { return i * n - i * (i + 1) / 2 + (j - i - 1); };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) sim[tri(i, j)] = cosineOf(v[idx[i]], v[idx[j]]);
    auto S = [&](size_t a, size_t b) -> double& { return a < b ? sim[tri(a, b)] : sim[tri(b, a)]; };

    const size_t kNone = (size_t)-1;
    std::vector<int>    size(n, 1);
    std::vector<char>   active(n, 1);
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i) parent[i] = i;
    std::vector<size_t> nn(n, kNone);
    std::vector<double> nnSim(n, -2.0);
    auto rescan = [&](size_t i) {
        nn[i] = kNone;
        nnSim[i] = -2.0;
        for (size_t j = i + 1; j < n; ++j)
            if (active[j] && S(i, j) > nnSim[i]) { nnSim[i] = S(i, j); nn[i] = j; }
    };
    for (size_t i = 0; i < n; ++i) rescan(i);

    size_t nact = n;
    while (nact > 1) {
        size_t bi = kNone;
        double bs = -2.0;
        for (size_t i = 0; i < n; ++i)
            if (active[i] && nn[i] != kNone && nnSim[i] > bs) { bs = nnSim[i]; bi = i; }
        if (bi == kNone) break;
        const size_t bj = nn[bi];
        // the naive loop's stop, word for word: genuinely apart AND within the cap
        if (bs < threshold && nact <= (size_t)cap) break;
        for (size_t k = 0; k < n; ++k) {
            if (!active[k] || k == bi || k == bj) continue;
            S(bi, k) = ((double)size[bi] * S(bi, k) + (double)size[bj] * S(bj, k)) / (double)(size[bi] + size[bj]);
        }
        size[bi] += size[bj];
        active[bj] = 0;
        parent[bj] = bi;
        --nact;
        for (size_t i = 0; i < n; ++i) {
            if (!active[i]) continue;
            if (i == bi || nn[i] == bi || nn[i] == bj) { rescan(i); continue; }
            if (i < bi) {
                const double s = S(i, bi);
                if (s > nnSim[i] || (s == nnSim[i] && bi < nn[i])) { nnSim[i] = s; nn[i] = bi; }
            }
        }
    }
    // each input's root, numbered by its cluster's first member in input order
    std::vector<int> label(n, -1);
    int next = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t r = i;
        while (parent[r] != r) r = parent[r];
        if (label[r] < 0) label[r] = next++;
        out[idx[i]] = label[r];
    }
    return out;
}

} // namespace pbt

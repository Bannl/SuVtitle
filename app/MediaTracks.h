// =============================================================================
// SuVtitle - app/MediaTracks.h   (the film's own tracks: probe + embedded subtitle extraction)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/MediaTracks.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: "AudioPB" -> "SuVtitle" in the failure sentences (the
// page matches them by "subtitle track", not by name).
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// THE FILM'S OWN TRACKS, read by the TRAY. The user (11/10, his first MKV -- an anime film with five sound tracks
// and eight subtitle tracks): "il faut aussi avoir les options pour choisir des sous-titres et audio deja inclus
// dans le fichier video". Nothing else can tell us what a container holds (WH_PLAN_pistes F3, measured on WebView2
// 154): Chromium's audioTracks HIDES the tracks it cannot decode and RENUMBERS the rest, ignores FlagDefault, and
// its textTracks is EMPTY for every embedded subtitle; Media Foundation (the helper) only ever opened the first
// sound track (F5). So this header reads the container itself:
//   * probe(): Matroska / WebM (EBML) and ISO-BMFF (mp4 m4v mov 3gp m4a). HEADERS ONLY -- the Matroska head (SeekHead,
//     Info, Tracks: a few KB, measured 12 KB on his 2.09 GB film) and the MP4 moov's small boxes (the big sample
//     tables are located, never read at probe time). Result: the container, the picture, every sound track with
//     its ord (0-based among the sound tracks in container order, the undecodable ones INCLUDED -- TrPipe.h WH
//     block), Matroska/MP4 track id, name, language, codec, channels, the default flag and whether the WebView can
//     play it (a static table of what wv2probe MEASURED playable: PROBE_RESULTS A), and every subtitle track (text
//     or image, default, forced, readable or not and why).
//   * extract(): one TEXT subtitle track as a document SubtitleReader.h reads (UTF-8 forced): SRT for S_TEXT/UTF8|
//     ASCII and MP4 tx3g / QuickTime text (its style boxes become <i>/<b>/<u>), ASS/SSA for S_TEXT/ASS|SSA (the
//     CodecPrivate header + Dialogue lines rebuilt in the header's own Format order), WebVTT for S_TEXT/WEBVTT,
//     D_WEBVTT/* and MP4 wvtt. Matroska: the Cues FAST PATH when EVERY cue point of the track carries a
//     CueRelativePosition and every indexed position really holds a block of the track (mkvmerge and ffmpeg index
//     every subtitle block -- F4: 1031 cue points = 1031 blocks on the film), else a header-only walk of every
//     cluster (a 1 MB sequential window; payloads of other tracks are never read). SuVtitle review RV1-1
//     (2026-10-23): the fast path only for a file whose Segment Info NAMES one of those two muxers
//     (cuesIndexEverySub) -- Cues are a seek index, not one entry per block, and a partial index read the rest of
//     the track as absent, silently; every other file is walked. SimpleBlock and BlockGroup
//     (+BlockDuration); a laced subtitle block is skipped and counted; ContentEncoding none / header stripping /
//     zlib (the small RAW INFLATE below -- no new library), anything else or encryption = the track is listed and
//     says why it cannot be read. MP4: stts/ctts/stsc/stsz|stz2/stco|co64 and the edit list (an initial empty edit
//     shifts the track, the first media time starts it).
// HOSTILE INPUT (D9): every size is checked against the bytes left and against a cap (an element <= 64 MB except a
// Segment or a Cluster, a CodecPrivate <= 1 MB, a subtitle block <= 1 MB, the document <= 20 MB = SubtitleReader's own
// ceiling, EBML depth <= 8, box depth <= 12, <= 128 tracks), every loop advances or stops, and an atomic cancel flag
// is looked at in every loop. scratchpad/ui_test/tracks_smoke.cpp truncates and flips every test file under ASan.
//
// THREADS: pure functions over a byte Source; probe() and extract() run on WORKER threads (bridge.cpp), never on the
// UI thread. A FileSource opens the file READ ONLY, shared, and never writes anything anywhere.
// NOTE: ASCII only (the build has no /utf-8).
// =============================================================================
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SubtitleReader.h"

namespace pbmedia {

// ---------------------------------------------------------------------------
// Caps (D9)
// ---------------------------------------------------------------------------
// (an element READ into memory is capped; one only stepped over -- a hundred MB of attached fonts, a video frame -- is
//  checked against the bytes left and never read. The Matroska parsers have a FIXED nesting -- Segment > Tracks >
//  TrackEntry > ContentEncodings > ContentEncoding > ContentCompression is the deepest, 6 -- no recursion on the file's
//  say-so, so kMaxEbmlDepth holds by construction; the ISO-BMFF walk recurses and checks kMaxBoxDepth.)
constexpr uint64_t kMaxElem       = 64ull << 20;   // any element read whole (a Segment or a Cluster never is)
constexpr uint64_t kMaxHeadElem   = 8ull << 20;    // Tracks / Info / SeekHead, read whole at probe time
constexpr uint64_t kMaxCodecPriv  = 1ull << 20;
constexpr uint64_t kMaxSubBlock   = 1ull << 20;    // one subtitle block / sample
constexpr uint64_t kMaxDoc        = pbsubr::kMaxBytes;   // the synthesized document (D9: 32 MB; the reader takes 20)
constexpr uint64_t kMaxCuesBytes  = kMaxElem;      // the Cues element (a film's full index: 241 KB on his)
constexpr uint64_t kMaxTable      = kMaxElem;      // one MP4 sample table of a SUBTITLE track
constexpr uint64_t kMaxSmallBox   = 1ull << 20;    // tkhd, mdhd, hdlr, elst, stsd...: read whole
constexpr size_t   kMaxTracks     = 128;
constexpr int      kMaxEbmlDepth  = 8;             // (by construction, see above)
constexpr int      kMaxBoxDepth   = 12;
constexpr size_t   kMaxTopLevel   = 4096;          // top-level elements / boxes looked at before the media data
constexpr size_t   kMaxSeekHeads  = 4;
constexpr size_t   kMaxSamples    = 2000000;       // MP4 samples of a subtitle track considered
constexpr size_t   kMaxBlocks     = 200000;        // subtitle blocks of one track collected
constexpr uint64_t kNoDurCapMs    = 10000;         // a block with no duration lasts until the next one, at most this
constexpr uint64_t kMaxMediaMs    = 100000000000ull;   // ~3 years: anything later is a broken timestamp
constexpr uint64_t kResyncScan    = 8ull << 20;    // after a damaged stretch, how far to look for the next Cluster
constexpr int      kMaxResyncs    = 64;
constexpr uint64_t kMaxPayload    = 64ull << 20;   // subtitle bytes read for ONE track in all (a film's: ~100 KB; MP4
                                                   // samples may all point at the same megabyte: 2 M x 1 MB otherwise)

// ---------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    virtual bool readAt(uint64_t off, void* dst, size_t n) = 0;   // exactly n bytes, or false
};

// A buffer in memory (the fuzz smoke truncates and flips copies of the test files through this).
class MemSource : public Source {
public:
    MemSource(const void* p, size_t n) : p_(static_cast<const uint8_t*>(p)), n_(n) {}
    uint64_t size() const override { return n_; }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off > n_ || n > n_ - off) return false;
        if (n) std::memcpy(dst, p_ + off, n);
        return true;
    }
private:
    const uint8_t* p_;
    size_t n_;
};

// The file, READ ONLY and shared (a film still being written or played elsewhere stays readable; nothing is ever
// written). `sequential` = the cluster walk (the OS reads ahead).
class FileSource : public Source {
public:
    explicit FileSource(const std::wstring& path, bool sequential = false) {
        h_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | (sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_FLAG_RANDOM_ACCESS),
                           nullptr);
        LARGE_INTEGER s{};
        if (h_ != INVALID_HANDLE_VALUE && ::GetFileSizeEx(h_, &s) && s.QuadPart >= 0) size_ = static_cast<uint64_t>(s.QuadPart);
    }
    ~FileSource() override { if (h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_); }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (h_ == INVALID_HANDLE_VALUE || off > size_ || n > size_ - off) return false;
        uint8_t* d = static_cast<uint8_t*>(dst);
        while (n) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            const DWORD want = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
            DWORD got = 0;
            if (!::ReadFile(h_, d, want, &got, &ov) || got == 0) return false;   // (the file shrank under us)
            d += got;
            off += got;
            n -= got;
        }
        return true;
    }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// A window over a Source: element headers are parsed from it without a read each. Over MEMORY (a whole Tracks or
// Cues element already read) it hands out pointers into that buffer directly.
class Reader {
public:
    explicit Reader(Source& s, size_t window = 64 * 1024) : s_(&s), win_(window < 64 ? 64 : window) {}
    Reader(const void* mem, size_t n) : mem_(static_cast<const uint8_t*>(mem)), memLen_(n) {}
    uint64_t size() const { return s_ ? s_->size() : memLen_; }
    uint64_t reads() const { return reads_; }
    // [off, off+n) as a pointer (n <= the window), or nullptr past the end / on a read error.
    const uint8_t* at(uint64_t off, size_t n) {
        if (!s_) return (off <= memLen_ && n <= memLen_ - off) ? mem_ + off : nullptr;
        if (n > win_) return nullptr;
        if (len_ && off >= bufOff_ && off - bufOff_ <= len_ && n <= len_ - (off - bufOff_)) return buf_.data() + (off - bufOff_);
        const uint64_t sz = s_->size();
        if (off > sz || n > sz - off) return nullptr;
        const size_t want = static_cast<size_t>(std::min<uint64_t>(win_, sz - off));
        if (buf_.size() < win_) buf_.resize(win_);
        ++reads_;
        if (!s_->readAt(off, buf_.data(), want)) { len_ = 0; return nullptr; }
        bufOff_ = off;
        len_ = want;
        return buf_.data();
    }
    // Any number of bytes (a payload), copied out.
    bool bytes(uint64_t off, uint64_t n, std::string& out) {
        out.clear();
        const uint64_t sz = size();
        if (off > sz || n > sz - off) return false;
        out.resize(static_cast<size_t>(n));
        if (n == 0) return true;
        if (!s_) { std::memcpy(&out[0], mem_ + off, static_cast<size_t>(n)); return true; }
        if (n <= win_) {
            const uint8_t* p = at(off, static_cast<size_t>(n));
            if (!p) { out.clear(); return false; }
            std::memcpy(&out[0], p, static_cast<size_t>(n));
            return true;
        }
        ++reads_;
        if (!s_->readAt(off, &out[0], static_cast<size_t>(n))) { out.clear(); return false; }
        return true;
    }
private:
    Source* s_ = nullptr;
    const uint8_t* mem_ = nullptr;
    size_t memLen_ = 0;
    size_t win_ = 0;
    std::vector<uint8_t> buf_;
    uint64_t bufOff_ = 0;
    size_t len_ = 0;
    uint64_t reads_ = 0;
};

inline uint64_t be(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
inline bool cancelled(const std::atomic<bool>* c) { return c && c->load(std::memory_order_relaxed); }
inline std::string cutNul(std::string s) {
    const size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    return s;
}
inline std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
inline bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
inline std::string fourccText(uint32_t f) {
    std::string s;
    for (int k = 3; k >= 0; --k) {
        const char c = static_cast<char>((f >> (8 * k)) & 0xFF);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}
constexpr uint32_t fcc(const char (&s)[5]) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

// ---------------------------------------------------------------------------
// zlib (RFC 1950) over a RAW INFLATE (RFC 1951), Mark Adler's "puff" structure: what mkvmerge's
// "--compression zlib" writes on subtitle tracks (and the film's VobSub tracks carry: F4). Bounded both ways:
// input underflow, an over-subscribed or incomplete code, a distance past the start, more output than `cap` --
// each one is a failure, never a read or a write out of bounds. Proved against Python's zlib (tracks_smoke).
// ---------------------------------------------------------------------------
namespace zinf {
struct State {
    const uint8_t* in = nullptr;
    size_t inLen = 0, inPos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::string* out = nullptr;
    size_t cap = 0;
};
inline int bits(State& s, int need) {
    uint32_t val = s.bitBuf;
    while (s.bitCnt < need) {
        if (s.inPos >= s.inLen) return -1;
        val |= static_cast<uint32_t>(s.in[s.inPos++]) << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = need >= 32 ? 0 : (val >> need);
    s.bitCnt -= need;
    return static_cast<int>(val & ((1u << need) - 1u));
}
struct Huffman { short count[16]; short symbol[288]; };
// 0 = a complete code; > 0 = incomplete (that many codes missing); < 0 = over-subscribed.
inline int construct(Huffman& h, const short* length, int n) {
    for (int len = 0; len <= 15; ++len) h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym) {
        const short l = length[sym];
        if (l < 0 || l > 15) return -1;
        h.count[l]++;
    }
    if (h.count[0] == n) return 0;                   // no codes at all: decoding with it fails
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = static_cast<short>(sym);
    return left;
}
inline int decode(State& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        const int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        const int count = h.count[len];
        if (code - count < first) {
            const int at = index + (code - first);
            return (at >= 0 && at < 288) ? h.symbol[at] : -1;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;                                       // ran out of codes
}
inline int stored(State& s) {
    s.bitBuf = 0;                                    // the rest of the current byte is padding
    s.bitCnt = 0;
    if (s.inLen - s.inPos < 4) return -1;
    const unsigned len = s.in[s.inPos] | (static_cast<unsigned>(s.in[s.inPos + 1]) << 8);
    const unsigned nlen = s.in[s.inPos + 2] | (static_cast<unsigned>(s.in[s.inPos + 3]) << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFFu)) return -1;
    if (s.inLen - s.inPos < len) return -1;
    if (s.cap - s.out->size() < len) return -1;
    s.out->append(reinterpret_cast<const char*>(s.in + s.inPos), len);
    s.inPos += len;
    return 0;
}
inline int codes(State& s, const Huffman& lencode, const Huffman& distcode) {
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                     131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                     2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s.out->size() >= s.cap) return -1;
            s.out->push_back(static_cast<char>(sym));
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            const int e = bits(s, lext[sym]);
            if (e < 0) return -1;
            const size_t len = static_cast<size_t>(lbase[sym] + e);
            const int ds = decode(s, distcode);
            if (ds < 0 || ds >= 30) return -1;
            const int de = bits(s, dext[ds]);
            if (de < 0) return -1;
            const size_t dist = static_cast<size_t>(dbase[ds] + de);
            if (dist > s.out->size()) return -1;     // too far back
            if (s.cap - s.out->size() < len) return -1;
            for (size_t k = 0; k < len; ++k) {
                const char c = (*s.out)[s.out->size() - dist];
                s.out->push_back(c);
            }
        }
    }
}
inline int fixedBlock(State& s) {
    struct Tables {
        Huffman len{}, dist{};
        Tables() {
            short l[288];
            int sym = 0;
            for (; sym < 144; ++sym) l[sym] = 8;
            for (; sym < 256; ++sym) l[sym] = 9;
            for (; sym < 280; ++sym) l[sym] = 7;
            for (; sym < 288; ++sym) l[sym] = 8;
            construct(len, l, 288);
            for (sym = 0; sym < 30; ++sym) l[sym] = 5;
            construct(dist, l, 30);
        }
    };
    static const Tables t;                           // (a function-local static: built once, thread-safe)
    return codes(s, t.len, t.dist);
}
inline int dynamicBlock(State& s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    short lengths[320] = {};
    int index = 0;
    for (; index < ncode; ++index) {
        const int b = bits(s, 3);
        if (b < 0) return -1;
        lengths[order[index]] = static_cast<short>(b);
    }
    for (; index < 19; ++index) lengths[order[index]] = 0;
    Huffman lencode{}, distcode{};
    if (construct(lencode, lengths, 19) != 0) return -1;   // the code-length code must be complete
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = static_cast<short>(sym);
        } else {
            short len = 0;
            int rep = 0;
            if (sym == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                const int b = bits(s, 2);
                if (b < 0) return -1;
                rep = 3 + b;
            } else if (sym == 17) {
                const int b = bits(s, 3);
                if (b < 0) return -1;
                rep = 3 + b;
            } else {
                const int b = bits(s, 7);
                if (b < 0) return -1;
                rep = 11 + b;
            }
            if (index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -1;                 // no end-of-block code
    int err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen != lencode.count[0] + lencode.count[1])) return -1;
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist != distcode.count[0] + distcode.count[1])) return -1;
    return codes(s, lencode, distcode);
}
}   // namespace zinf

// A raw deflate stream -> `out` (at most `cap` bytes). `used` = the input bytes it took.
inline bool inflateRaw(const uint8_t* in, size_t n, std::string& out, size_t cap, size_t* used = nullptr) {
    zinf::State s;
    s.in = in;
    s.inLen = n;
    s.out = &out;
    s.cap = cap;
    out.clear();
    int last = 0;
    do {
        last = zinf::bits(s, 1);
        const int type = zinf::bits(s, 2);
        if (last < 0 || type < 0) return false;
        const int err = type == 0 ? zinf::stored(s) : type == 1 ? zinf::fixedBlock(s) : type == 2 ? zinf::dynamicBlock(s) : -1;
        if (err != 0) return false;
    } while (!last);
    if (used) *used = s.inPos;
    return true;
}
inline uint32_t adler32(const std::string& d) {
    uint32_t a = 1, b = 0;
    for (unsigned char c : d) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}
// A zlib stream (2-byte header, deflate, Adler-32): what Matroska's ContentCompAlgo 0 means. No preset dictionary.
inline bool zlibInflate(const std::string& in, std::string& out, size_t cap) {
    out.clear();
    if (in.size() < 6) return false;
    const uint8_t cmf = static_cast<uint8_t>(in[0]), flg = static_cast<uint8_t>(in[1]);
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((static_cast<unsigned>(cmf) << 8) | flg) % 31u != 0 || (flg & 0x20)) return false;
    size_t used = 0;
    if (!inflateRaw(reinterpret_cast<const uint8_t*>(in.data()) + 2, in.size() - 2, out, cap, &used)) { out.clear(); return false; }
    if (in.size() - 2 - used < 4) { out.clear(); return false; }
    const uint8_t* t = reinterpret_cast<const uint8_t*>(in.data()) + 2 + used;
    if (static_cast<uint32_t>(be(t, 4)) != adler32(out)) { out.clear(); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// What a probe answers (the contract of WH_PLAN_pistes section 2, plus what extract() needs)
// ---------------------------------------------------------------------------
struct VideoInfo { std::string codec; uint32_t w = 0, h = 0; };
struct AudioTrack {
    uint32_t    ord = 0;        // 0-based among the sound tracks, container order, undecodable ones INCLUDED (D2)
    uint32_t    track = 0;      // Matroska TrackNumber / MP4 track_ID
    std::string name, lang, codec;
    uint32_t    ch = 0;
    bool        def = false;    // Matroska FlagDefault (absent = 1) and FlagEnabled / MP4 tkhd "enabled"
    bool        play = false;   // the WebView can play it (webViewPlays)
};
// A Matroska ContentEncoding (in DECODING order once planned).
struct Enc { uint64_t order = 0, scope = 1, type = 0, algo = 0; std::string settings; bool comp = false, encr = false; };
struct TableRef { uint64_t pos = 0, size = 0; bool has = false; };
struct SubTrack {
    uint32_t    track = 0;
    std::string name, lang, codec;
    bool        image = false;  // pictures (VobSub, PGS, DVB...): listed, never read here
    bool        def = false, forced = false;
    bool        ok = false;     // SuVtitle can read it (a text track whose encodings it can undo)
    std::string why;            // !ok: one sentence for the user
    // how to read it
    enum class Synth : uint8_t { None, Srt, Ass, Vtt, VttBlock, Tx3g, Wvtt };
    Synth       synth = Synth::None;
    bool        ssa = false;    // S_TEXT/SSA (the default Format line starts with Marked)
    std::string header;         // ASS/SSA: the script header (the CodecPrivate, decoded)
    std::vector<Enc> encs;      // Matroska: the content encodings to undo, in decoding order
    double      trackScale = 1.0;
    // ISO-BMFF
    uint32_t    timescale = 0;
    int64_t     mediaTime = 0;  // the edit list's first media time (track timescale)
    uint64_t    emptyMs = 0;    // ...after its leading empty edits
    TableRef    stts, ctts, stsc, stsz, stz2, stco, co64;
};
struct Info {
    enum class State : uint8_t { None, Ready, Error };
    State       state = State::None;    // None = not a container this reads (wav, mp3...)
    std::string container;              // "mkv" | "webm" | "mp4" | "mov" | ""
    bool        hasVideo = false;
    VideoInfo   video;
    std::vector<AudioTrack> audio;
    std::vector<SubTrack>   subs;
    std::string err;                    // Error: what went wrong (a log line, not shown)
    uint64_t    durationMs = 0;
    std::string muxer;                  // WritingApp, else MuxingApp (Matroska)
    std::string muxingApp, writingApp;  // the Segment Info's two names (cuesIndexEverySub)
    // Matroska
    uint64_t    segData = 0, segEnd = 0, cuesPos = 0, firstCluster = 0;
    uint64_t    tcScale = 1000000;
    // ISO-BMFF
    bool        fragmented = false;
    const SubTrack* sub(uint32_t track) const {
        for (const auto& s : subs) if (s.track == track) return &s;
        return nullptr;
    }
};

// Sentences (the player says them through translatorFileNotice when a track cannot be shown)
inline const char* errRead()       { return "The video file could not be read."; }
inline const char* errDamaged()    { return "This subtitle track could not be read (the file looks damaged)."; }
inline const char* errEncrypted()  { return "This subtitle track is encrypted: SuVtitle can't read it."; }
inline const char* errCompressed() { return "This subtitle track is compressed in a way SuVtitle can't read."; }
inline const char* errImage()      { return "This subtitle track is made of pictures (DVD or Blu-ray subtitles): SuVtitle can only show text subtitles."; }
inline const char* errFragmented() { return "Subtitles inside a fragmented MP4 file can't be read yet."; }
inline const char* errEmpty()      { return "This subtitle track holds no subtitles."; }
inline std::string errFormat(const std::string& codec) {
    return "SuVtitle can't read this kind of subtitle track (" + (codec.empty() ? std::string("unknown") : codec) + ").";
}

// WHAT THE WEBVIEW PLAYS (D3) -- the codecs wv2probe MEASURED playable on this machine's WebView2 154 (PROBE_RESULTS A:
// AAC, MP3, Opus, Vorbis, FLAC, AC-3 in MKV (!), PCM s16le/s24le/f32le) plus E-AC-3 (decodingInfo "supported"). NOT
// DTS (F3: absent from audioTracks), TrueHD/MLP, ALAC (measured: NO_SUPPORTED_STREAMS), MP2, big-endian PCM, WMA, AMR:
// unmeasured or failing = not playable (the page then maps what it sees by language and label: D4).
inline bool webViewPlays(const std::string& codec) {
    return codec == "AAC" || codec == "MP3" || codec == "Opus" || codec == "Vorbis" || codec == "FLAC" || codec == "AC-3"
        || codec == "E-AC-3" || codec == "PCM";
}

// WHOSE CUES ARE TRUSTED (review RV1-1, 2026-10-23). Matroska's Cues are a SEEK index: nothing in the spec promises an
// entry per subtitle block, and a partial index read through the fast path loses the rest of the track without a
// word (RV-1's crafted h1_cues_partial.mkv: one cue of two). Only the two muxers MEASURED to index every subtitle block
// are believed: mkvmerge (WritingApp "mkvmerge v..", MuxingApp "libebml v.. + libmatroska v..": its default cue mode
// for a subtitle track is every block, F4's film: 1031 cue points = 1031 blocks) and ffmpeg's libavformat (MuxingApp
// "Lavf.." -- also HandBrake's, OBS's: a cue point per subtitle packet). Anything else, or no name: the walk, which
// reads every block (F4: 300-400 ms for a 2 GB film). KNOWN LIMIT, written down: a file that NAMES one of the two but
// carries a partial index (a forged or hand-edited header) is still read through its index.
inline bool cuesIndexEverySub(const std::string& muxingApp, const std::string& writingApp) {
    auto starts = [](const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; };
    if (starts(muxingApp, "Lavf") || starts(writingApp, "Lavf")) return true;
    return starts(writingApp, "mkvmerge")
        && (muxingApp.find("libebml") != std::string::npos || muxingApp.find("libmatroska") != std::string::npos);
}

// ---------------------------------------------------------------------------
// EBML
// ---------------------------------------------------------------------------
namespace mkv {
constexpr uint32_t kEBML = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
    kSeekID = 0x53AB, kSeekPos = 0x53AC, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1, kDuration = 0x4489,
    kMuxingApp = 0x4D80, kWritingApp = 0x5741, kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7,
    kTrackType = 0x83, kFlagEnabled = 0xB9, kFlagDefault = 0x88, kFlagForced = 0x55AA, kCodecID = 0x86,
    kCodecPrivate = 0x63A2, kName = 0x536E, kLanguage = 0x22B59C, kLanguageBCP47 = 0x22B59D,
    kTrackTimestampScale = 0x23314F, kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA, kAudio = 0xE1,
    kChannels = 0x9F, kBitDepth = 0x6264, kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kCEOrder = 0x5031,
    kCEScope = 0x5032, kCEType = 0x5033, kCECompression = 0x5034, kCECompAlgo = 0x4254, kCECompSettings = 0x4255,
    kCEEncryption = 0x5035, kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTrackPositions = 0xB7, kCueTrack = 0xF7,
    kCueClusterPosition = 0xF1, kCueRelativePosition = 0xF0, kCluster = 0x1F43B675, kTimestamp = 0xE7,
    kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1, kBlockDuration = 0x9B, kTags = 0x1254C367,
    kChapters = 0x1043A770, kAttachments = 0x1941A469;

struct Elem {
    uint32_t id = 0;
    uint64_t pos = 0, data = 0, size = 0;
    bool     unknown = false;   // an unknown size (or one running past its parent / the file: clamped to `limit`)
    uint64_t end() const { return data + size; }
};
// A Segment-level ID: where an UNKNOWN-size Cluster ends.
inline bool level1(uint32_t id) {
    return id == kCluster || id == kCues || id == kTags || id == kChapters || id == kAttachments || id == kSeekHead
        || id == kInfo || id == kTracks || id == kSegment || id == kEBML;
}
// One element header at `at`, inside [at, limit). IDs are 1-4 bytes (the length marker kept), sizes 1-8 bytes (all
// value bits set = unknown). `open` = a Segment or a Cluster: an unknown size, or one running past `limit` (a file
// still downloading, a live recording), is CLAMPED to `limit`; any other element must fit or it is refused.
inline bool readElem(Reader& r, uint64_t at, uint64_t limit, Elem& e, bool open = false) {
    if (limit > r.size()) limit = r.size();
    if (at >= limit) return false;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(12, limit - at));
    const uint8_t* p = r.at(at, n);
    if (!p) return false;
    const uint8_t b0 = p[0];
    const size_t il = b0 >= 0x80 ? 1 : b0 >= 0x40 ? 2 : b0 >= 0x20 ? 3 : b0 >= 0x10 ? 4 : 0;
    if (!il || il >= n) return false;
    const uint8_t s0 = p[il];
    if (!s0) return false;
    size_t sl = 1;
    uint8_t m = 0x80;
    while (!(s0 & m)) { ++sl; m = static_cast<uint8_t>(m >> 1); }
    if (il + sl > n) return false;
    uint64_t v = s0 & static_cast<uint8_t>(m - 1);
    bool ones = v == static_cast<uint64_t>(static_cast<uint8_t>(m - 1));
    for (size_t k = 1; k < sl; ++k) {
        v = (v << 8) | p[il + k];
        if (p[il + k] != 0xFF) ones = false;
    }
    e.id = static_cast<uint32_t>(be(p, il));
    e.pos = at;
    e.data = at + il + sl;
    e.unknown = false;
    if (e.data > limit) return false;
    if (ones || v > limit - e.data) {
        if (!open) return false;
        e.unknown = true;
        e.size = limit - e.data;
        return true;
    }
    e.size = v;
    return true;
}
inline uint64_t uintOf(Reader& r, const Elem& e) {
    if (e.size == 0 || e.size > 8) return 0;
    const uint8_t* p = r.at(e.data, static_cast<size_t>(e.size));
    return p ? be(p, static_cast<size_t>(e.size)) : 0;
}
inline double floatOf(Reader& r, const Elem& e) {
    const uint8_t* p = (e.size == 4 || e.size == 8) ? r.at(e.data, static_cast<size_t>(e.size)) : nullptr;
    if (!p) return 0.0;
    if (e.size == 4) {
        const uint32_t u = static_cast<uint32_t>(be(p, 4));
        float f;
        std::memcpy(&f, &u, 4);
        return static_cast<double>(f);
    }
    const uint64_t u = be(p, 8);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
inline std::string strOf(Reader& r, const Elem& e, uint64_t cap = 4096) {
    std::string s;
    if (e.size > cap || !r.bytes(e.data, e.size, s)) return std::string();
    return cutNul(s);
}
// ticks (Cluster/Block timestamps) -> ms through TimestampScale (ns per tick) and TrackTimestampScale.
inline int64_t ticksToMs(int64_t ticks, uint64_t scaleNs, double trackScale) {
    const double v = static_cast<double>(ticks) * static_cast<double>(scaleNs) * trackScale / 1e6;
    if (!(v == v)) return 0;                                      // NaN
    if (v > static_cast<double>(kMaxMediaMs)) return static_cast<int64_t>(kMaxMediaMs);
    if (v < -static_cast<double>(kMaxMediaMs)) return -static_cast<int64_t>(kMaxMediaMs);
    return static_cast<int64_t>(v < 0 ? v - 0.5 : v + 0.5);
}
}   // namespace mkv

// ---------------------------------------------------------------------------
// Codec names (the contract's `codec`: what a person reads in the Audio list)
// ---------------------------------------------------------------------------
inline std::string mkvAudioCodec(const std::string& id, const std::string& priv) {
    if (startsWith(id, "A_AAC")) return "AAC";
    if (id == "A_MPEG/L3") return "MP3";
    if (id == "A_MPEG/L2") return "MP2";
    if (id == "A_MPEG/L1") return "MP1";
    if (id == "A_OPUS") return "Opus";
    if (id == "A_VORBIS") return "Vorbis";
    if (id == "A_FLAC") return "FLAC";
    if (id == "A_EAC3" || id == "A_AC3/BSID10") return "E-AC-3";
    if (startsWith(id, "A_AC3")) return "AC-3";
    if (startsWith(id, "A_DTS")) return "DTS";
    if (id == "A_TRUEHD") return "TrueHD";
    if (id == "A_MLP") return "MLP";
    if (id == "A_ALAC") return "ALAC";
    if (id == "A_PCM/INT/LIT" || id == "A_PCM/FLOAT/IEEE") return "PCM";
    if (id == "A_PCM/INT/BIG") return "PCM (big-endian)";
    if (id == "A_MS/ACM" && priv.size() >= 2) {                      // a WAVEFORMATEX: its format tag says
        const unsigned tag = static_cast<uint8_t>(priv[0]) | (static_cast<unsigned>(static_cast<uint8_t>(priv[1])) << 8);
        if (tag == 0x0001 || tag == 0x0003 || tag == 0xFFFE) return "PCM";
        if (tag == 0x0055) return "MP3";
        if (tag == 0x0050) return "MP2";
        if (tag == 0x00FF || tag == 0x1610 || tag == 0x706D) return "AAC";
        if (tag == 0x2000) return "AC-3";
        if (tag == 0x2001) return "DTS";
        if (tag >= 0x0160 && tag <= 0x0163) return "WMA";
        return "MS audio";
    }
    if (startsWith(id, "A_REAL")) return "RealAudio";
    if (id == "A_TTA1") return "TTA";
    if (id == "A_WAVPACK4") return "WavPack";
    return id.size() > 2 ? id.substr(2) : id;
}
inline std::string mkvVideoCodec(const std::string& id, const std::string& priv) {
    if (id == "V_MPEGH/ISO/HEVC") return "HEVC";
    if (id == "V_MPEG4/ISO/AVC") return "H.264";
    if (id == "V_AV1") return "AV1";
    if (id == "V_VP9") return "VP9";
    if (id == "V_VP8") return "VP8";
    if (startsWith(id, "V_MPEG4/ISO")) return "MPEG-4";
    if (id == "V_MPEG2") return "MPEG-2";
    if (id == "V_MPEG1") return "MPEG-1";
    if (id == "V_THEORA") return "Theora";
    if (id == "V_PRORES") return "ProRes";
    if (id == "V_MJPEG") return "MJPEG";
    if (id == "V_MS/VFW/FOURCC" && priv.size() >= 20) {               // a BITMAPINFOHEADER: biCompression
        std::string f = priv.substr(16, 4);
        for (char& c : f) if (c < 32 || c > 126) c = '?';
        return f;
    }
    return id.size() > 2 ? id.substr(2) : id;
}

// ---------------------------------------------------------------------------
// The Matroska probe
// ---------------------------------------------------------------------------
namespace mkvprobe {
using namespace mkv;
struct Track {
    uint64_t number = 0, type = 0, channels = 1, w = 0, h = 0;
    bool def = true, enabled = true, forced = false;
    std::string codecId, name, lang = "eng", bcp47, priv;
    bool privTooBig = false;
    double trackScale = 1.0;
    std::vector<Enc> encs;
};
inline void parseEncodings(Reader& r, const Elem& ces, Track& t) {
    uint64_t at = ces.data;
    for (int guard = 0; at < ces.end() && guard < 16; ++guard) {
        Elem ce;
        if (!readElem(r, at, ces.end(), ce)) break;
        at = ce.end();
        if (ce.id != kContentEncoding) continue;
        Enc enc;
        for (uint64_t a2 = ce.data; a2 < ce.end();) {
            Elem c;
            if (!readElem(r, a2, ce.end(), c)) break;
            a2 = c.end();
            if (c.id == kCEOrder) enc.order = uintOf(r, c);
            else if (c.id == kCEScope) enc.scope = uintOf(r, c);
            else if (c.id == kCEType) enc.type = uintOf(r, c);
            else if (c.id == kCEEncryption) enc.encr = true;
            else if (c.id == kCECompression) {
                enc.comp = true;
                for (uint64_t a3 = c.data; a3 < c.end();) {
                    Elem k;
                    if (!readElem(r, a3, c.end(), k)) break;
                    a3 = k.end();
                    if (k.id == kCECompAlgo) enc.algo = uintOf(r, k);
                    else if (k.id == kCECompSettings && k.size <= 4096) r.bytes(k.data, k.size, enc.settings);
                }
            }
        }
        t.encs.push_back(enc);
    }
}
inline void parseTrackEntry(Reader& r, const Elem& te, Track& t) {
    for (uint64_t at = te.data; at < te.end();) {
        Elem c;
        if (!readElem(r, at, te.end(), c)) break;
        at = c.end();
        switch (c.id) {
            case kTrackNumber: t.number = uintOf(r, c); break;
            case kTrackType: t.type = uintOf(r, c); break;
            case kFlagDefault: t.def = uintOf(r, c) != 0; break;
            case kFlagEnabled: t.enabled = uintOf(r, c) != 0; break;
            case kFlagForced: t.forced = uintOf(r, c) != 0; break;
            case kCodecID: t.codecId = strOf(r, c, 256); break;
            case kName: t.name = strOf(r, c, 4096); break;
            case kLanguage: t.lang = strOf(r, c, 64); break;
            case kLanguageBCP47: t.bcp47 = strOf(r, c, 64); break;
            case kTrackTimestampScale: { const double d = floatOf(r, c); if (d > 0 && d < 1e6) t.trackScale = d; break; }
            case kCodecPrivate:
                if (c.size > kMaxCodecPriv) t.privTooBig = true;
                else r.bytes(c.data, c.size, t.priv);
                break;
            case kVideo:
                for (uint64_t a2 = c.data; a2 < c.end();) {
                    Elem v;
                    if (!readElem(r, a2, c.end(), v)) break;
                    a2 = v.end();
                    if (v.id == kPixelWidth) t.w = uintOf(r, v);
                    else if (v.id == kPixelHeight) t.h = uintOf(r, v);
                }
                break;
            case kAudio:
                for (uint64_t a2 = c.data; a2 < c.end();) {
                    Elem v;
                    if (!readElem(r, a2, c.end(), v)) break;
                    a2 = v.end();
                    if (v.id == kChannels) t.channels = uintOf(r, v);
                }
                break;
            case kContentEncodings: parseEncodings(r, c, t); break;
            default: break;
        }
    }
}
inline void parseSeekHead(Reader& r, const Elem& sh, uint64_t segData, uint64_t segEnd,
                          std::vector<std::pair<uint32_t, uint64_t>>& seeks) {
    if (sh.size > kMaxHeadElem) return;
    std::string buf;
    if (!r.bytes(sh.data, sh.size, buf)) return;
    Reader m(buf.data(), buf.size());
    for (uint64_t at = 0; at < buf.size() && seeks.size() < 256;) {
        Elem s;
        if (!readElem(m, at, buf.size(), s)) break;
        at = s.end();
        if (s.id != kSeek) continue;
        uint32_t id = 0;
        uint64_t pos = 0;
        bool hasPos = false;
        for (uint64_t a2 = s.data; a2 < s.end();) {
            Elem c;
            if (!readElem(m, a2, s.end(), c)) break;
            a2 = c.end();
            if (c.id == kSeekID && c.size >= 1 && c.size <= 4) id = static_cast<uint32_t>(uintOf(m, c));
            else if (c.id == kSeekPos) { pos = uintOf(m, c); hasPos = true; }
        }
        if (id && hasPos && pos < segEnd - segData) seeks.push_back({ id, segData + pos });
    }
}
}   // namespace mkvprobe

// A subtitle track's synthesis and readability from its CodecID (+ encodings).
inline void classifyMkvSub(const std::string& id, SubTrack& s) {
    using S = SubTrack::Synth;
    if (id == "S_TEXT/UTF8" || id == "S_TEXT/ASCII") { s.codec = "SRT"; s.synth = S::Srt; }
    else if (id == "S_TEXT/ASS" || id == "S_ASS") { s.codec = "ASS"; s.synth = S::Ass; }
    else if (id == "S_TEXT/SSA" || id == "S_SSA") { s.codec = "SSA"; s.synth = S::Ass; s.ssa = true; }
    else if (id == "S_TEXT/WEBVTT") { s.codec = "WebVTT"; s.synth = S::Vtt; }
    else if (id == "D_WEBVTT/SUBTITLES" || id == "D_WEBVTT/CAPTIONS" || id == "D_WEBVTT/DESCRIPTIONS") { s.codec = "WebVTT"; s.synth = S::VttBlock; }
    else if (id == "S_VOBSUB") { s.codec = "VobSub"; s.image = true; }
    else if (id == "S_HDMV/PGS") { s.codec = "PGS"; s.image = true; }
    else if (id == "S_DVBSUB") { s.codec = "DVB"; s.image = true; }
    else if (id == "S_IMAGE/BMP") { s.codec = "Bitmap"; s.image = true; }
    else if (id == "S_ARIBSUB") { s.codec = "ARIB"; s.image = true; }
    else if (id == "D_WEBVTT/METADATA") { s.codec = "WebVTT metadata"; }
    else if (id == "S_HDMV/TEXTST") { s.codec = "HDMV text"; }
    else if (id == "S_KATE") { s.codec = "Kate"; }
    else if (id == "S_TEXT/USF") { s.codec = "USF"; }
    else s.codec = id.size() > 2 ? id.substr(2) : id;
}

// Encodings in DECODING order (the highest ContentEncodingOrder first -- the spec's rule); false + why when one of
// them cannot be undone here (encryption, bzlib, lzo).
inline bool planEncodings(std::vector<Enc>& encs, std::string& why) {
    std::stable_sort(encs.begin(), encs.end(), [](const Enc& a, const Enc& b) { return a.order > b.order; });
    for (const Enc& e : encs) {
        if (e.type != 0 || e.encr) { why = errEncrypted(); return false; }
        if (e.algo != 0 && e.algo != 3) { why = errCompressed(); return false; }
    }
    return true;
}
// Undo the encodings whose scope has `scopeBit` (1 = the frames, 2 = the CodecPrivate).
inline bool undoEncodings(const std::vector<Enc>& encs, uint64_t scopeBit, std::string& data, size_t cap) {
    for (const Enc& e : encs) {
        if (!(e.scope & scopeBit)) continue;
        if (e.algo == 3) {                                         // header stripping: the stripped bytes go back on
            if (e.settings.size() > cap || data.size() > cap - e.settings.size()) return false;
            data.insert(0, e.settings);
        } else if (e.algo == 0) {
            std::string o;
            if (!zlibInflate(data, o, cap)) return false;
            data.swap(o);
        } else {
            return false;
        }
    }
    return true;
}

inline bool probeMatroska(Reader& r, Info& out, const std::atomic<bool>* cancel) {
    using namespace mkv;
    auto fail = [&](const char* why) { out.state = Info::State::Error; out.err = why; return false; };
    Elem h;
    if (!readElem(r, 0, r.size(), h) || h.id != kEBML || h.size > 4096) return fail("no EBML header");
    std::string docType = "matroska";
    for (uint64_t at = h.data; at < h.end();) {
        Elem c;
        if (!readElem(r, at, h.end(), c)) break;
        at = c.end();
        if (c.id == kDocType) docType = lowerAscii(strOf(r, c, 64));
    }
    if (docType != "matroska" && docType != "webm") return fail("not a Matroska document");
    out.container = docType == "webm" ? "webm" : "mkv";
    // the Segment: the next top-level element (a Void or a CRC tolerated before it)
    Elem seg;
    bool found = false;
    uint64_t at = h.end();
    for (int k = 0; k < 16 && at < r.size(); ++k) {
        if (!readElem(r, at, r.size(), seg, true)) break;
        if (seg.id == kSegment) { found = true; break; }
        if (seg.unknown) break;
        at = seg.end();
    }
    if (!found) return fail("no Segment");
    out.segData = seg.data;
    out.segEnd = seg.end();
    // its head: everything before the first Cluster
    std::vector<std::pair<uint32_t, uint64_t>> seeks;
    uint64_t infoPos = 0, tracksPos = 0;
    at = seg.data;
    for (size_t n = 0; at < out.segEnd && n < kMaxTopLevel; ++n) {
        if (cancelled(cancel)) return fail("cancelled");
        Elem c;
        if (!readElem(r, at, out.segEnd, c, true)) break;
        if (c.id == kCluster) { out.firstCluster = c.pos; break; }
        if (c.unknown) break;                                       // only a Cluster may run open
        if (c.id == kSeekHead) mkvprobe::parseSeekHead(r, c, out.segData, out.segEnd, seeks);
        else if (c.id == kInfo && !infoPos) infoPos = c.pos;
        else if (c.id == kTracks && !tracksPos) tracksPos = c.pos;
        else if (c.id == kCues && !out.cuesPos) out.cuesPos = c.pos;
        at = c.end();
    }
    // the SeekHead's pointers -- Cues are usually at the END (mkvmerge, ffmpeg); a SeekHead may point to another one
    size_t nestedHeads = 0;
    for (size_t i = 0; i < seeks.size() && i < 256; ++i) {
        const uint32_t id = seeks[i].first;
        const uint64_t pos = seeks[i].second;
        if (id == kSeekHead && nestedHeads < kMaxSeekHeads) {
            Elem sh;
            if (readElem(r, pos, out.segEnd, sh) && sh.id == kSeekHead) { ++nestedHeads; mkvprobe::parseSeekHead(r, sh, out.segData, out.segEnd, seeks); }
        } else if (id == kInfo && !infoPos) infoPos = pos;
        else if (id == kTracks && !tracksPos) tracksPos = pos;
        else if (id == kCues && !out.cuesPos) out.cuesPos = pos;
    }
    if (out.cuesPos) {                                             // verified now (extract() trusts it)
        Elem cu;
        if (!readElem(r, out.cuesPos, out.segEnd, cu) || cu.id != kCues) out.cuesPos = 0;
    }
    if (infoPos) {
        Elem e;
        if (readElem(r, infoPos, out.segEnd, e) && e.id == kInfo && e.size <= kMaxHeadElem) {
            double dur = 0;
            for (uint64_t a = e.data; a < e.end();) {
                Elem c;
                if (!readElem(r, a, e.end(), c)) break;
                a = c.end();
                if (c.id == kTimestampScale) { const uint64_t v = uintOf(r, c); if (v) out.tcScale = v; }
                else if (c.id == kDuration) dur = floatOf(r, c);
                else if (c.id == kMuxingApp) { out.muxingApp = strOf(r, c, 256); if (out.muxer.empty()) out.muxer = out.muxingApp; }
                else if (c.id == kWritingApp) { out.writingApp = strOf(r, c, 256); out.muxer = out.writingApp; }
            }
            if (dur > 0 && dur * static_cast<double>(out.tcScale) / 1e6 < static_cast<double>(kMaxMediaMs))
                out.durationMs = static_cast<uint64_t>(dur * static_cast<double>(out.tcScale) / 1e6 + 0.5);
        }
    }
    if (!tracksPos) return fail("no track list");
    Elem t;
    if (!readElem(r, tracksPos, out.segEnd, t) || t.id != kTracks || t.size > kMaxHeadElem) return fail("unreadable track list");
    std::string buf;
    if (!r.bytes(t.data, t.size, buf)) return fail("unreadable track list");
    Reader m(buf.data(), buf.size());
    std::vector<uint64_t> numbers;
    size_t entries = 0;
    for (uint64_t a = 0; a < buf.size() && entries < kMaxTracks;) {
        if (cancelled(cancel)) return fail("cancelled");
        Elem te;
        if (!readElem(m, a, buf.size(), te)) break;
        a = te.end();
        if (te.id != kTrackEntry) continue;
        ++entries;
        mkvprobe::Track k;
        mkvprobe::parseTrackEntry(m, te, k);
        if (!k.number || std::find(numbers.begin(), numbers.end(), k.number) != numbers.end()) continue;   // (a duplicate: the first wins)
        numbers.push_back(k.number);
        const std::string lang = !k.bcp47.empty() ? k.bcp47 : k.lang;
        if (k.type == 1) {
            if (!out.hasVideo) {
                out.hasVideo = true;
                out.video.codec = mkvVideoCodec(k.codecId, k.priv);
                out.video.w = static_cast<uint32_t>(std::min<uint64_t>(k.w, 1u << 20));
                out.video.h = static_cast<uint32_t>(std::min<uint64_t>(k.h, 1u << 20));
            }
        } else if (k.type == 2) {
            AudioTrack au;
            au.ord = static_cast<uint32_t>(out.audio.size());
            au.track = static_cast<uint32_t>(std::min<uint64_t>(k.number, 0xFFFFFFFFull));
            au.name = k.name;
            au.lang = lang;
            au.codec = mkvAudioCodec(k.codecId, k.priv);
            au.ch = static_cast<uint32_t>(std::min<uint64_t>(k.channels, 255));
            au.def = k.def && k.enabled;
            au.play = webViewPlays(au.codec);
            out.audio.push_back(au);
        } else if (k.type == 17) {
            SubTrack s;
            s.track = static_cast<uint32_t>(std::min<uint64_t>(k.number, 0xFFFFFFFFull));
            s.name = k.name;
            s.lang = lang;
            s.def = k.def && k.enabled;
            s.forced = k.forced;
            s.trackScale = k.trackScale;
            classifyMkvSub(k.codecId, s);
            s.encs = k.encs;
            if (s.image) { s.ok = false; s.why = errImage(); }
            else if (s.synth == SubTrack::Synth::None) { s.ok = false; s.why = errFormat(s.codec); }
            else if (!planEncodings(s.encs, s.why)) s.ok = false;
            else if (s.synth == SubTrack::Synth::Ass) {
                std::string head = k.priv;
                if (k.privTooBig || !undoEncodings(s.encs, 2, head, kMaxCodecPriv)) { s.ok = false; s.why = errCompressed(); }
                else { s.header = head; s.ok = true; }
            } else s.ok = true;
            out.subs.push_back(s);
        }
    }
    out.state = Info::State::Ready;
    return true;
}

// ---------------------------------------------------------------------------
// ISO-BMFF
// ---------------------------------------------------------------------------
namespace bmff {
struct Box {
    uint32_t type = 0;
    uint64_t pos = 0, data = 0, end = 0;
    uint64_t size() const { return end - data; }
};
inline bool readBox(Reader& r, uint64_t at, uint64_t limit, Box& b) {
    if (limit > r.size()) limit = r.size();
    if (at >= limit || limit - at < 8) return false;
    const uint8_t* p = r.at(at, static_cast<size_t>(std::min<uint64_t>(16, limit - at)));
    if (!p) return false;
    uint64_t size = be(p, 4);
    b.type = static_cast<uint32_t>(be(p + 4, 4));
    uint64_t hdr = 8;
    if (size == 1) {
        if (limit - at < 16) return false;
        size = be(p + 8, 8);
        hdr = 16;
    } else if (size == 0) {
        size = limit - at;                                          // "to the end"
    }
    if (size < hdr || size > limit - at) return false;
    b.pos = at;
    b.data = at + hdr;
    b.end = at + size;
    if (b.type == fcc("uuid")) {
        if (b.end - b.data < 16) return false;
        b.data += 16;
    }
    return true;
}
struct Trak {
    uint32_t id = 0, flags = 0, handler = 0, timescale = 0, fourcc = 0, ch = 0, w = 0, h = 0;
    std::string hname, uname, lang = "und", elng, codec;
    int64_t mediaTime = 0;
    uint64_t emptyMovieTicks = 0;
    TableRef stts, ctts, stsc, stsz, stz2, stco, co64;
    bool littleEndianPcm = false;
};
// The QuickTime "Macintosh" language codes (an mdhd language below 0x400) that films actually carry.
inline std::string macLang(unsigned v) {
    static const char* const k[] = { "eng", "fre", "ger", "ita", "dut", "swe", "spa", "dan", "por", "nor", "heb", "jpn", "ara",
                                     "fin", "gre", "ice", "mlt", "tur", "hrv", "chi", "urd", "hin", "tha", "kor", "lit", "pol",
                                     "hun", "est", "lav", "smi", "fao", "per", "rus", "chi" };
    return v < sizeof(k) / sizeof(k[0]) ? std::string(k[v]) : std::string("und");
}
inline bool genericHandlerName(const std::string& n) {
    return n.empty() || n.find("Handler") != std::string::npos || startsWith(n, "Core Media") || startsWith(n, "ISO Media")
        || startsWith(n, "Mainconcept") || n == "Mono" || n == "Stereo";
}
inline std::string mp4AudioCodec(uint32_t f, int objectType, bool le) {
    if (f == fcc("mp4a")) {
        switch (objectType) {
            case 0x40: case 0x66: case 0x67: case 0x68: case -1: return "AAC";
            case 0x69: case 0x6B: return "MP3";
            case 0xA5: return "AC-3";
            case 0xA6: return "E-AC-3";
            case 0xA9: return "DTS";
            case 0xDD: return "Vorbis";
            case 0xAD: return "Opus";
            default: return "MPEG-4 audio";
        }
    }
    if (f == fcc("ac-3")) return "AC-3";
    if (f == fcc("ec-3")) return "E-AC-3";
    if (f == fcc("Opus")) return "Opus";
    if (f == fcc("fLaC")) return "FLAC";
    if (f == fcc("alac")) return "ALAC";
    if (f == fcc("dtsc") || f == fcc("dtsh") || f == fcc("dtsl") || f == fcc("dtse") || f == fcc("dtsx")) return "DTS";
    if (f == fcc("mlpa")) return "TrueHD";
    if (f == fcc("samr")) return "AMR";
    if (f == fcc("sawb")) return "AMR-WB";
    if (f == fcc(".mp3") || f == 0x6D730055u) return "MP3";
    if (f == fcc("sowt") || f == fcc("lpcm") || f == fcc("raw ") || f == fcc("NONE")) return "PCM";
    if (f == fcc("twos")) return "PCM (big-endian)";
    if (f == fcc("in24") || f == fcc("in32") || f == fcc("fl32") || f == fcc("fl64")) return le ? "PCM" : "PCM (big-endian)";
    if (f == fcc("ulaw") || f == fcc("alaw")) return "G.711";
    return fourccText(f);
}
inline std::string mp4VideoCodec(uint32_t f) {
    if (f == fcc("avc1") || f == fcc("avc3")) return "H.264";
    if (f == fcc("hvc1") || f == fcc("hev1") || f == fcc("dvh1") || f == fcc("dvhe")) return "HEVC";
    if (f == fcc("av01")) return "AV1";
    if (f == fcc("vp09")) return "VP9";
    if (f == fcc("vp08")) return "VP8";
    if (f == fcc("mp4v")) return "MPEG-4";
    if (f == fcc("mp2v")) return "MPEG-2";
    if (f == fcc("apch") || f == fcc("apcn") || f == fcc("apcs") || f == fcc("apco") || f == fcc("ap4h") || f == fcc("ap4x")) return "ProRes";
    if (f == fcc("jpeg") || f == fcc("mjpa") || f == fcc("mjpb")) return "MJPEG";
    return fourccText(f);
}
// An MPEG-4 descriptor's length (1-4 bytes of 7 bits).
inline bool descLen(const uint8_t* p, size_t n, size_t& at, uint32_t& len) {
    len = 0;
    for (int k = 0; k < 4; ++k) {
        if (at >= n) return false;
        const uint8_t b = p[at++];
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}
// esds -> DecoderConfigDescriptor.objectTypeIndication, -1 when absent.
inline int esdsObjectType(const std::string& esds) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(esds.data());
    const size_t n = esds.size();
    size_t at = 4;                                                  // version + flags
    uint32_t len = 0;
    if (at >= n || p[at++] != 0x03 || !descLen(p, n, at, len)) return -1;
    if (n - at < 3) return -1;
    at += 2;                                                        // ES_ID
    const uint8_t fl = p[at++];
    if (fl & 0x80) at += 2;
    if (fl & 0x40) { if (at >= n) return -1; at += 1u + p[at]; }
    if (fl & 0x20) at += 2;
    if (at >= n || p[at++] != 0x04 || !descLen(p, n, at, len)) return -1;
    return at < n ? p[at] : -1;
}
inline bool smallBox(Reader& r, const Box& b, std::string& out) {
    return b.size() <= kMaxSmallBox && r.bytes(b.data, b.size(), out);
}
// The first sample entry of stsd: the codec, and for sound its channels and esds, for pictures its size.
inline void parseStsd(Reader& r, const Box& stsd, Trak& t) {
    std::string d;
    if (!smallBox(r, stsd, d) || d.size() < 16) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
    const uint64_t esize = be(p + 8, 4);
    t.fourcc = static_cast<uint32_t>(be(p + 12, 4));
    if (esize < 16 || esize > d.size() - 8) return;
    const uint8_t* e = p + 8;                                       // the entry: [size][type][6 reserved][2 dref]...
    const size_t en = static_cast<size_t>(esize);
    if (t.handler == fcc("vide") && en >= 36) {
        t.w = static_cast<uint32_t>(be(e + 32, 2));
        t.h = static_cast<uint32_t>(be(e + 34, 2));
        return;
    }
    if (t.handler != fcc("soun") || en < 36) return;
    const unsigned ver = static_cast<unsigned>(be(e + 16, 2));     // QuickTime sound description version
    t.ch = static_cast<uint32_t>(be(e + 24, 2));
    size_t kids = 36;
    if (ver == 1) kids = 52;
    else if (ver == 2) {
        kids = 72;
        if (en >= 64) t.ch = static_cast<uint32_t>(be(e + 56, 4));
    }
    int objectType = -1;
    // the entry's children: esds (directly, or inside QuickTime's 'wave'), enda (QuickTime PCM endianness)
    Reader m(e, en);
    auto look = [&](const Box& c) {
        if (c.type == fcc("esds")) {
            std::string es;
            if (m.bytes(c.data, c.size(), es)) objectType = esdsObjectType(es);
        } else if (c.type == fcc("enda") && c.size() >= 2) {
            const uint8_t* q = m.at(c.data, 2);
            if (q) t.littleEndianPcm = be(q, 2) != 0;
        }
    };
    for (uint64_t at = kids; at < en;) {
        Box c;
        if (!readBox(m, at, en, c)) break;
        at = c.end;
        look(c);
        if (c.type != fcc("wave")) continue;
        for (uint64_t a2 = c.data; a2 < c.end;) {
            Box w;
            if (!readBox(m, a2, c.end, w)) break;
            a2 = w.end;
            look(w);
        }
    }
    t.codec = mp4AudioCodec(t.fourcc, objectType, t.littleEndianPcm);
}
// A trak's boxes, each honoured only under its OWN parent: a QuickTime minf carries a second hdlr -- the DATA handler
// ('dhlr' / 'url ', measured on ffmpeg's .mov) -- which must not be taken for the media's ('vide' / 'soun' / 'sbtl').
// `qt` = a QuickTime file: an mdhd language of 0 is then the Macintosh code for English, not "nothing".
inline void walkTrak(Reader& r, const Box& parent, Trak& t, bool qt, int depth, const std::atomic<bool>* cancel) {
    if (depth > kMaxBoxDepth) return;
    const uint32_t pt = parent.type;
    size_t n = 0;
    for (uint64_t at = parent.data; at < parent.end && n < 1024; ++n) {
        if (cancelled(cancel)) return;
        Box b;
        if (!readBox(r, at, parent.end, b)) break;
        at = b.end;
        const uint32_t y = b.type;
        if ((y == fcc("mdia") && pt == fcc("trak")) || (y == fcc("minf") && pt == fcc("mdia")) || (y == fcc("stbl") && pt == fcc("minf"))
            || (y == fcc("edts") && pt == fcc("trak")) || (y == fcc("udta") && pt == fcc("trak"))) {
            walkTrak(r, b, t, qt, depth + 1, cancel);
        } else if (pt == fcc("trak") && y == fcc("tkhd")) {
            std::string d;
            if (!smallBox(r, b, d) || d.size() < 24) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
            t.flags = static_cast<uint32_t>(be(p + 1, 3));
            t.id = static_cast<uint32_t>(be(p + (p[0] == 1 ? 20 : 12), 4));
        } else if (pt == fcc("mdia") && y == fcc("mdhd")) {
            std::string d;
            if (!smallBox(r, b, d)) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
            const size_t tsAt = (d.size() > 0 && p[0] == 1) ? 20 : 12, lAt = (d.size() > 0 && p[0] == 1) ? 32 : 20;
            if (d.size() >= tsAt + 4) t.timescale = static_cast<uint32_t>(be(p + tsAt, 4));
            if (d.size() >= lAt + 2) {
                const unsigned v = static_cast<unsigned>(be(p + lAt, 2));
                if (v == 0x7FFF || (v == 0 && !qt)) t.lang = "und";
                else if (v < 0x400) t.lang = macLang(v);
                else {
                    std::string l;
                    for (int k = 2; k >= 0; --k) l += static_cast<char>(((v >> (5 * k)) & 0x1F) + 0x60);
                    bool okL = true;
                    for (char c : l) if (c < 'a' || c > 'z') okL = false;
                    t.lang = okL ? l : "und";
                }
            }
        } else if (pt == fcc("mdia") && y == fcc("elng")) {
            std::string d;
            if (smallBox(r, b, d) && d.size() > 4 && d.size() < 64) t.elng = cutNul(d.substr(4));
        } else if (pt == fcc("mdia") && y == fcc("hdlr")) {
            std::string d;
            if (!smallBox(r, b, d) || d.size() < 12) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
            t.handler = static_cast<uint32_t>(be(p + 8, 4));
            if (d.size() > 24) {
                std::string nm = d.substr(24);
                const uint8_t l0 = static_cast<uint8_t>(nm[0]);
                if (l0 > 0 && static_cast<size_t>(l0) + 1 == nm.size()) nm = nm.substr(1);   // QuickTime's Pascal string
                t.hname = cutNul(nm);
                if (t.hname.size() > 256) t.hname.resize(256);
            }
        } else if (pt == fcc("udta") && y == fcc("name")) {
            std::string d;
            if (smallBox(r, b, d) && d.size() < 1024) t.uname = cutNul(d);
        } else if (pt == fcc("edts") && y == fcc("elst")) {
            std::string d;
            if (!smallBox(r, b, d) || d.size() < 8) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
            const bool v1 = p[0] == 1;
            const uint64_t count = be(p + 4, 4);
            const size_t es = v1 ? 20 : 12;
            uint64_t empty = 0;
            for (uint64_t k = 0; k < count && k < 1000 && 8 + (k + 1) * es <= d.size(); ++k) {
                const uint8_t* q = p + 8 + k * es;
                const uint64_t segDur = v1 ? be(q, 8) : be(q, 4);
                const int64_t mt = v1 ? static_cast<int64_t>(be(q + 8, 8)) : static_cast<int64_t>(static_cast<int32_t>(be(q + 4, 4)));
                if (mt == -1) { if (segDur < (1ull << 48)) empty += segDur; continue; }
                t.mediaTime = mt < 0 ? 0 : mt;
                break;
            }
            t.emptyMovieTicks = empty;
        } else if (pt == fcc("stbl") && y == fcc("stsd")) {
            parseStsd(r, b, t);
        } else if (pt == fcc("stbl")) {
            TableRef* tr = y == fcc("stts") ? &t.stts : y == fcc("ctts") ? &t.ctts : y == fcc("stsc") ? &t.stsc
                         : y == fcc("stsz") ? &t.stsz : y == fcc("stz2") ? &t.stz2 : y == fcc("stco") ? &t.stco
                         : y == fcc("co64") ? &t.co64 : nullptr;
            if (tr) { tr->pos = b.data; tr->size = b.size(); tr->has = true; }
        }
    }
}
}   // namespace bmff

inline bool probeBmff(Reader& r, Info& out, const std::atomic<bool>* cancel) {
    using namespace bmff;
    auto fail = [&](const char* why) { out.state = Info::State::Error; out.err = why; return false; };
    Box moov;
    bool haveMoov = false, haveFtyp = false;
    uint32_t brand = 0;
    uint64_t at = 0;
    for (size_t n = 0; at < r.size() && n < kMaxTopLevel; ++n) {
        if (cancelled(cancel)) return fail("cancelled");
        Box b;
        if (!readBox(r, at, r.size(), b)) break;
        at = b.end;
        if (b.type == fcc("ftyp") && !haveFtyp && b.size() >= 4) {
            const uint8_t* p = r.at(b.data, 4);
            if (p) brand = static_cast<uint32_t>(be(p, 4));
            haveFtyp = true;
        } else if (b.type == fcc("moov")) { moov = b; haveMoov = true; break; }
    }
    out.container = (!haveFtyp || brand == fcc("qt  ")) ? "mov" : "mp4";
    if (!haveMoov) return fail("no moov box");
    uint32_t movieTs = 0;
    uint64_t movieDur = 0;
    std::vector<Trak> traks;
    size_t n = 0;
    for (uint64_t a = moov.data; a < moov.end && n < kMaxTopLevel; ++n) {
        if (cancelled(cancel)) return fail("cancelled");
        Box b;
        if (!readBox(r, a, moov.end, b)) break;
        a = b.end;
        if (b.type == fcc("mvhd")) {
            std::string d;
            if (smallBox(r, b, d) && d.size() >= 20) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data());
                const bool v1 = p[0] == 1;
                if (d.size() >= (v1 ? 32u : 20u)) {
                    movieTs = static_cast<uint32_t>(be(p + (v1 ? 20 : 12), 4));
                    movieDur = v1 ? be(p + 24, 8) : be(p + 16, 4);
                }
            }
        } else if (b.type == fcc("mvex")) {
            out.fragmented = true;
        } else if (b.type == fcc("trak") && traks.size() < kMaxTracks) {
            Trak t;
            walkTrak(r, b, t, out.container == "mov", 1, cancel);
            traks.push_back(t);
        }
    }
    if (movieTs && movieDur && movieDur / movieTs < kMaxMediaMs / 1000) out.durationMs = movieDur * 1000 / movieTs;
    for (const Trak& t : traks) {
        const std::string lang = !t.elng.empty() ? t.elng : t.lang;
        const std::string name = !t.uname.empty() ? t.uname : (genericHandlerName(t.hname) ? std::string() : t.hname);
        if (t.handler == fcc("vide")) {
            if (!out.hasVideo) {
                out.hasVideo = true;
                out.video.codec = mp4VideoCodec(t.fourcc);
                out.video.w = t.w;
                out.video.h = t.h;
            }
        } else if (t.handler == fcc("soun")) {
            AudioTrack au;
            au.ord = static_cast<uint32_t>(out.audio.size());
            au.track = t.id;
            au.name = name;
            au.lang = lang;
            au.codec = t.codec.empty() ? mp4AudioCodec(t.fourcc, -1, false) : t.codec;
            au.ch = std::min<uint32_t>(t.ch, 255);
            au.def = (t.flags & 1) != 0;                                 // tkhd "enabled"
            au.play = webViewPlays(au.codec);
            out.audio.push_back(au);
        } else if (t.handler == fcc("sbtl") || t.handler == fcc("text") || t.handler == fcc("subt") || t.handler == fcc("clcp")) {
            SubTrack s;
            s.track = t.id;
            s.name = name;
            s.lang = lang;
            s.def = (t.flags & 1) != 0;
            s.timescale = t.timescale;
            s.mediaTime = t.mediaTime;
            s.emptyMs = movieTs ? (t.emptyMovieTicks / movieTs * 1000 + (t.emptyMovieTicks % movieTs) * 1000 / movieTs) : 0;
            s.stts = t.stts; s.ctts = t.ctts; s.stsc = t.stsc; s.stsz = t.stsz; s.stz2 = t.stz2; s.stco = t.stco; s.co64 = t.co64;
            using S = SubTrack::Synth;
            if (t.fourcc == fcc("tx3g") || t.fourcc == fcc("text")) { s.codec = t.fourcc == fcc("tx3g") ? "Timed Text" : "QuickTime text"; s.synth = S::Tx3g; }
            else if (t.fourcc == fcc("wvtt")) { s.codec = "WebVTT"; s.synth = S::Wvtt; }
            else if (t.fourcc == fcc("stpp")) s.codec = "TTML";
            else if (t.fourcc == fcc("c608") || t.fourcc == fcc("c708")) s.codec = "CEA-608";
            else s.codec = fourccText(t.fourcc);
            if (s.synth == S::None) { s.ok = false; s.why = errFormat(s.codec); }
            else if (out.fragmented) { s.ok = false; s.why = errFragmented(); }
            else if (!s.timescale || !s.stts.has || !s.stsc.has || !(s.stsz.has || s.stz2.has) || !(s.stco.has || s.co64.has)) {
                s.ok = false;
                s.why = errDamaged();
            } else s.ok = true;
            out.subs.push_back(s);
        }
    }
    out.state = Info::State::Ready;
    return true;
}

// ---------------------------------------------------------------------------
// PROBE: what the file is (sniffed from its first bytes, never trusted from its extension) and what it holds.
// ---------------------------------------------------------------------------
inline Info probe(Source& src, const std::atomic<bool>* cancel = nullptr) {
    Info out;
    Reader r(src, 64 * 1024);
    const uint8_t* p = r.at(0, static_cast<size_t>(std::min<uint64_t>(12, src.size())));
    if (!p || src.size() < 12) return out;                          // (too small to be a container: None)
    if (be(p, 4) == mkv::kEBML) { probeMatroska(r, out, cancel); return out; }
    const uint32_t t = static_cast<uint32_t>(be(p + 4, 4));
    if (t == fcc("ftyp") || t == fcc("moov") || t == fcc("mdat") || t == fcc("free") || t == fcc("skip") || t == fcc("wide")
        || t == fcc("pnot")) {
        probeBmff(r, out, cancel);
        return out;
    }
    return out;
}
inline Info probeFile(const std::wstring& path, const std::atomic<bool>* cancel = nullptr) {
    FileSource f(path);
    if (!f.ok()) { Info i; i.state = Info::State::Error; i.err = "the file could not be opened"; return i; }
    return probe(f, cancel);
}
// The containers this header reads, by extension: the player asks for a probe only for these (a wav or an mp3 has
// one sound track and nothing to choose -- its job starts at once, exactly as before WH).
inline bool probeWorthyExt(const std::wstring& ext) {
    std::wstring e = ext;
    for (wchar_t& c : e) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return e == L"mkv" || e == L"mka" || e == L"webm" || e == L"mp4" || e == L"m4v" || e == L"m4a" || e == L"mov"
        || e == L"3gp" || e == L"3g2";
}

// ---------------------------------------------------------------------------
// EXTRACTION
// ---------------------------------------------------------------------------
struct Extracted {
    bool        ok = false;
    std::string err;                    // !ok: a sentence for the user ("" = cancelled)
    pbsubr::Format format = pbsubr::Format::Unknown;
    std::string doc;                    // the synthesized document, UTF-8
    size_t      blocks = 0;             // blocks / samples of the track read
    size_t      laced = 0;              // laced subtitle blocks skipped
    size_t      skipped = 0;            // too big, undecodable, out of time
    size_t      cues = 0;               // cues written into the document
    uint64_t    payload = 0;            // subtitle bytes read (kMaxPayload)
    bool        viaCues = false;        // Matroska: read through the Cues index
    bool        truncated = false;      // a cap stopped it
    int         resyncs = 0;            // damaged stretches skipped (the walk found the next Cluster)
    std::vector<uint64_t> blockPos;     // (tests) where each block element started
};
enum class Path { Auto, Walk, Cues };

namespace detail {
struct Blk {
    int64_t  ms = 0;                    // start
    bool     hasDur = false;
    uint64_t durMs = 0;
    std::string data;                   // decoded payload
    uint64_t pos = 0;
    uint32_t seq = 0;
};
// "<track VINT><int16 timestamp><flags>" at the head of a SimpleBlock / Block.
inline bool blockHead(const uint8_t* p, size_t n, uint64_t& track, int16_t& tc, uint8_t& flags, size_t& hdr) {
    if (!n || !p[0]) return false;
    size_t l = 1;
    uint8_t m = 0x80;
    while (!(p[0] & m)) { ++l; m = static_cast<uint8_t>(m >> 1); }
    if (l > 8 || l + 3 > n) return false;
    uint64_t v = p[0] & static_cast<uint8_t>(m - 1);
    for (size_t k = 1; k < l; ++k) v = (v << 8) | p[k];
    track = v;
    tc = static_cast<int16_t>(static_cast<uint16_t>((p[l] << 8) | p[l + 1]));
    flags = p[l + 2];
    hdr = l + 3;
    return true;
}
struct Walk {
    Reader& r;
    const Info& info;
    const SubTrack& st;
    std::vector<Blk>& out;
    Extracted& ex;
    const std::atomic<bool>* cancel;
    bool stop = false;
    // One block element (SimpleBlock, or a BlockGroup's Block) at [data, data+size) of a cluster at `ctc`. false = a
    // block of ANOTHER track (or unreadable head): the Cues path's check needs it.
    bool takeBlock(uint64_t data, uint64_t size, int64_t ctc, bool hasDur, uint64_t durTicks, uint64_t elemPos, bool& ours) {
        ours = false;
        const uint8_t* p = r.at(data, static_cast<size_t>(std::min<uint64_t>(size, 11)));
        uint64_t trk = 0;
        int16_t tc = 0;
        uint8_t fl = 0;
        size_t hdr = 0;
        if (!p || !blockHead(p, static_cast<size_t>(std::min<uint64_t>(size, 11)), trk, tc, fl, hdr)) return false;
        if (trk != st.track) return true;
        ours = true;
        ++ex.blocks;
        if (out.size() >= kMaxBlocks) { ex.truncated = true; return true; }
        if ((fl >> 1) & 3) { ++ex.laced; return true; }             // laced subtitle frames: never seen, skipped
        if (size - hdr > kMaxSubBlock) { ++ex.skipped; return true; }
        if (ex.payload + (size - hdr) > kMaxPayload) { ex.truncated = true; stop = true; return true; }
        ex.payload += size - hdr;
        Blk b;
        if (!r.bytes(data + hdr, size - hdr, b.data)) { ++ex.skipped; return true; }
        if (!undoEncodings(st.encs, 1, b.data, kMaxSubBlock)) { ++ex.skipped; return true; }
        b.ms = mkv::ticksToMs(ctc + tc, info.tcScale, st.trackScale);
        b.hasDur = hasDur;
        b.durMs = hasDur ? static_cast<uint64_t>(std::max<int64_t>(0, mkv::ticksToMs(static_cast<int64_t>(std::min<uint64_t>(durTicks, 1ull << 40)), info.tcScale, st.trackScale))) : 0;
        b.pos = elemPos;
        b.seq = static_cast<uint32_t>(out.size());
        ex.blockPos.push_back(elemPos);
        out.push_back(std::move(b));
        return true;
    }
    // A BlockGroup's children: its Block and BlockDuration (the order inside is free).
    void group(const mkv::Elem& g, int64_t ctc) {
        uint64_t bData = 0, bSize = 0, dur = 0;
        bool haveBlock = false, haveDur = false;
        for (uint64_t at = g.data; at < g.end();) {
            mkv::Elem c;
            if (!mkv::readElem(r, at, g.end(), c)) break;
            at = c.end();
            if (c.id == mkv::kBlock && !haveBlock) { bData = c.data; bSize = c.size; haveBlock = true; }
            else if (c.id == mkv::kBlockDuration) { dur = mkv::uintOf(r, c); haveDur = true; }
        }
        bool ours = false;
        if (haveBlock) takeBlock(bData, bSize, ctc, haveDur, dur, g.pos, ours);
    }
    // One Cluster from its data; returns where the walk goes on (the cluster's end; for an open one, the first
    // Segment-level element after it).
    uint64_t cluster(const mkv::Elem& c, uint64_t segEnd) {
        const uint64_t lim = c.unknown ? segEnd : c.end();
        int64_t ctc = 0;
        uint64_t at = c.data;
        while (at < lim) {
            if (cancelled(cancel)) { stop = true; return lim; }
            mkv::Elem k;
            // (read OPEN: in an unknown-size cluster the element that ends it -- the next Cluster -- may itself be of
            // unknown size; it must be recognised by its ID before its size is judged)
            const bool read = mkv::readElem(r, at, lim, k, true);
            if (read && c.unknown && mkv::level1(k.id)) return at;
            if (!read || k.unknown) {
                // damaged inside the cluster: a known-size one is left at its end, an open one is searched past
                return c.unknown ? resync(at + 1, segEnd) : lim;
            }
            if (k.id == mkv::kTimestamp) ctc = static_cast<int64_t>(std::min<uint64_t>(mkv::uintOf(r, k), 1ull << 52));
            else if (k.id == mkv::kSimpleBlock) { bool ours = false; takeBlock(k.data, k.size, ctc, false, 0, k.pos, ours); }
            else if (k.id == mkv::kBlockGroup) group(k, ctc);
            if (out.size() >= kMaxBlocks) { stop = true; return lim; }
            if (stop) return lim;                                   // (the payload budget is spent)
            at = k.end();
        }
        return lim;
    }
    // After a damaged stretch: the next Cluster ID within kResyncScan bytes whose element reads.
    uint64_t resync(uint64_t from, uint64_t segEnd) {
        if (ex.resyncs >= kMaxResyncs) { stop = true; return segEnd; }
        ++ex.resyncs;
        const uint64_t until = std::min<uint64_t>(segEnd, from + kResyncScan);
        uint64_t at = from;
        while (at + 4 <= until) {
            if (cancelled(cancel)) { stop = true; return segEnd; }
            const size_t n = static_cast<size_t>(std::min<uint64_t>(4096, until - at));
            const uint8_t* p = r.at(at, n);
            if (!p) break;
            for (size_t i = 0; i + 4 <= n; ++i) {
                if (p[i] == 0x1F && p[i + 1] == 0x43 && p[i + 2] == 0xB6 && p[i + 3] == 0x75) {
                    mkv::Elem c;
                    if (mkv::readElem(r, at + i, segEnd, c, true) && c.id == mkv::kCluster) return at + i;
                }
            }
            if (n < 4) break;
            at += n - 3;
        }
        stop = true;
        return segEnd;
    }
    void all() {
        const uint64_t segEnd = std::min<uint64_t>(info.segEnd, r.size());
        uint64_t at = info.firstCluster ? info.firstCluster : info.segData;
        while (at < segEnd && !stop) {
            if (cancelled(cancel)) { stop = true; break; }
            mkv::Elem c;
            if (!mkv::readElem(r, at, segEnd, c, true)) { at = resync(at + 1, segEnd); continue; }
            if (c.id == mkv::kCluster) { at = cluster(c, segEnd); continue; }
            if (c.unknown) { at = resync(at + 1, segEnd); continue; }   // only a Cluster may run open
            at = c.end();
        }
    }
};
// THE CUES FAST PATH: false = not usable (no index of this track, a cue point without CueRelativePosition, a
// position that holds no block of this track) -- the caller walks instead.
inline bool viaCues(Reader& r, const Info& info, const SubTrack& st, std::vector<Blk>& out, Extracted& ex,
                    const std::atomic<bool>* cancel) {
    if (!info.cuesPos) return false;
    mkv::Elem cu;
    if (!mkv::readElem(r, info.cuesPos, info.segEnd, cu) || cu.id != mkv::kCues || cu.size > kMaxCuesBytes) return false;
    std::string buf;
    if (!r.bytes(cu.data, cu.size, buf)) return false;
    Reader m(buf.data(), buf.size());
    std::vector<std::pair<uint64_t, uint64_t>> pos;
    bool all = true;
    for (uint64_t at = 0; at < buf.size();) {
        if (cancelled(cancel)) return false;
        mkv::Elem cp;
        if (!mkv::readElem(m, at, buf.size(), cp)) return false;
        at = cp.end();
        if (cp.id != mkv::kCuePoint) continue;
        for (uint64_t a2 = cp.data; a2 < cp.end();) {
            mkv::Elem tp;
            if (!mkv::readElem(m, a2, cp.end(), tp)) return false;
            a2 = tp.end();
            if (tp.id != mkv::kCueTrackPositions) continue;
            uint64_t trk = 0, cl = 0, rel = 0;
            bool hasCl = false, hasRel = false;
            for (uint64_t a3 = tp.data; a3 < tp.end();) {
                mkv::Elem c;
                if (!mkv::readElem(m, a3, tp.end(), c)) return false;
                a3 = c.end();
                if (c.id == mkv::kCueTrack) trk = mkv::uintOf(m, c);
                else if (c.id == mkv::kCueClusterPosition) { cl = mkv::uintOf(m, c); hasCl = true; }
                else if (c.id == mkv::kCueRelativePosition) { rel = mkv::uintOf(m, c); hasRel = true; }
            }
            if (trk != st.track) continue;
            if (!hasCl || !hasRel) { all = false; continue; }
            if (pos.size() >= kMaxBlocks) return false;
            pos.push_back({ cl, rel });
        }
    }
    if (!all || pos.empty()) return false;
    std::sort(pos.begin(), pos.end());
    pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
    const uint64_t segEnd = std::min<uint64_t>(info.segEnd, r.size());
    if (segEnd <= info.segData) return false;                       // (a file cut before its Segment's data)
    struct Cl { int64_t tc = 0; uint64_t data = 0, lim = 0; bool ok = false; };
    std::map<uint64_t, Cl> clusters;
    Walk w{ r, info, st, out, ex, cancel };
    for (const auto& pr : pos) {
        if (cancelled(cancel)) return false;
        if (pr.first >= segEnd - info.segData) return false;
        const uint64_t cabs = info.segData + pr.first;
        auto it = clusters.find(cabs);
        if (it == clusters.end()) {
            Cl c;
            mkv::Elem ce;
            if (mkv::readElem(r, cabs, segEnd, ce, true) && ce.id == mkv::kCluster) {
                c.data = ce.data;
                c.lim = ce.unknown ? segEnd : ce.end();
                // its Timestamp: the first child in practice (after a CRC-32 at most); looked for among the first 8
                uint64_t at = ce.data;
                for (int k = 0; k < 8 && at < c.lim; ++k) {
                    mkv::Elem t;
                    if (!mkv::readElem(r, at, c.lim, t)) break;
                    at = t.end();
                    if (t.id == mkv::kTimestamp) { c.tc = static_cast<int64_t>(std::min<uint64_t>(mkv::uintOf(r, t), 1ull << 52)); c.ok = true; break; }
                    if (t.id == mkv::kSimpleBlock || t.id == mkv::kBlockGroup) break;
                }
            }
            it = clusters.emplace(cabs, c).first;
        }
        const Cl& c = it->second;
        if (!c.ok || pr.second >= c.lim - c.data) return false;
        mkv::Elem b;
        if (!mkv::readElem(r, c.data + pr.second, c.lim, b)) return false;
        const size_t blocksBefore = ex.blocks;
        if (b.id == mkv::kSimpleBlock) {
            bool ours = false;
            if (!w.takeBlock(b.data, b.size, c.tc, false, 0, b.pos, ours) || !ours) return false;
        } else if (b.id == mkv::kBlockGroup) {
            w.group(b, c.tc);
            if (ex.blocks == blocksBefore) return false;              // the group held no block of this track
        } else {
            return false;
        }
    }
    return true;
}

// ---- the documents ----
inline void defuseArrow(std::string& s) {
    size_t at = 0;
    while ((at = s.find("-->", at)) != std::string::npos) { s.replace(at, 3, "->"); at = at > 0 ? at - 1 : 0; }
}
// A cue's text as the lines of an SRT/VTT cue: CR/LF normalized, no control character, no EMPTY line (a blank line
// would END the cue and turn the rest into garbage), no "-->" (a timing line to a parser).
inline std::string bodyLines(const std::string& raw) {
    std::vector<std::string> lines;
    std::string cur;
    auto push = [&] {
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
        size_t k = 0;
        while (k < cur.size() && (cur[k] == ' ' || cur[k] == '\t')) ++k;
        if (k < cur.size()) { std::string l = cur.substr(k); defuseArrow(l); lines.push_back(l); }
        cur.clear();
    };
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\r') { push(); if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i; continue; }
        if (c == '\n') { push(); continue; }
        if (c == '\t') { cur += ' '; continue; }
        if (u < 0x20 || u == 0x7F) continue;
        cur += c;
    }
    push();
    std::string o;
    for (size_t i = 0; i < lines.size(); ++i) { if (i) o += "\r\n"; o += lines[i]; }
    return o;
}
inline std::string stamp(uint64_t ms, char sep, bool assStyle = false) {
    const unsigned long long h = ms / 3600000ULL;
    const unsigned m = static_cast<unsigned>((ms / 60000ULL) % 60ULL), s = static_cast<unsigned>((ms / 1000ULL) % 60ULL);
    const unsigned f = static_cast<unsigned>(ms % 1000ULL);
    char b[48];
    if (assStyle) std::snprintf(b, sizeof b, "%llu:%02u:%02u.%03u", h, m, s, f);
    else std::snprintf(b, sizeof b, "%02llu:%02u:%02u%c%03u", h, m, s, sep, f);
    return b;
}
// The ASS/SSA header without its [Events] section (written again at the end), and that section's Format columns.
inline void assHeader(const std::string& priv, bool ssa, std::string& head, std::string& formatLine) {
    head.clear();
    formatLine.clear();
    std::string section;
    size_t at = 0;
    while (at < priv.size()) {
        size_t nl = priv.find('\n', at);
        if (nl == std::string::npos) nl = priv.size();
        std::string line = priv.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        while (!t.empty() && (t[0] == ' ' || t[0] == '\t')) t.erase(0, 1);
        if (!t.empty() && t[0] == '[') section = lowerAscii(t.substr(0, t.find(']') == std::string::npos ? t.size() : t.find(']') + 1));
        if (section == "[events]") {
            if (formatLine.empty() && lowerAscii(t.substr(0, 7)) == "format:") formatLine = t;
            continue;
        }
        head += line + "\r\n";
    }
    if (head.find("[Script Info]") == std::string::npos && head.find("[script info]") == std::string::npos)
        head = "[Script Info]\r\n" + std::string(ssa ? "ScriptType: v4.00\r\n" : "ScriptType: v4.00+\r\n") + "\r\n" + head;
    if (formatLine.empty())
        formatLine = ssa ? "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"
                         : "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text";
}
inline std::vector<std::string> assColumns(const std::string& formatLine) {
    std::vector<std::string> cols;
    std::string cur;
    for (size_t k = 7; k <= formatLine.size(); ++k) {
        if (k == formatLine.size() || formatLine[k] == ',') {
            std::string n = cur;
            while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
            while (!n.empty() && (n[0] == ' ' || n[0] == '\t')) n.erase(0, 1);
            cols.push_back(lowerAscii(n));
            cur.clear();
            if (cols.size() > 64) break;
        } else {
            cur += formatLine[k];
        }
    }
    return cols;
}
// A Matroska ASS block ("ReadOrder, <every Format column but Start and End>"; the last, Text, keeps its commas) as a
// Dialogue line in the header's own column order.
inline std::string assDialogue(const std::string& payload, const std::vector<std::string>& cols, uint64_t s, uint64_t e) {
    std::vector<std::string> nonTime;
    for (const auto& c : cols) if (c != "start" && c != "end") nonTime.push_back(c);
    const size_t want = 1 + nonTime.size();
    std::vector<std::string> f;
    std::string cur;
    for (char ch : payload) {
        if (ch == ',' && f.size() + 1 < want) { f.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    f.push_back(cur);
    std::map<std::string, std::string> byCol;
    if (f.size() == want) {
        for (size_t k = 0; k < nonTime.size(); ++k) byCol[nonTime[k]] = f[k + 1];
    } else {
        byCol["text"] = payload;                                    // not the block layout: the whole as text
    }
    std::string text = byCol.count("text") ? byCol["text"] : std::string();
    std::string clean;                                              // one line: a raw line break is \N
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') { clean += "\\N"; if (i + 1 < text.size() && text[i + 1] == '\n') ++i; continue; }
        if (c == '\n') { clean += "\\N"; continue; }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;
        clean += c;
    }
    byCol["text"] = clean;
    std::string o = "Dialogue: ";
    for (size_t k = 0; k < cols.size(); ++k) {
        if (k) o += ",";
        if (cols[k] == "start") o += stamp(s, '.', true);
        else if (cols[k] == "end") o += stamp(e, '.', true);
        else {
            std::string v = byCol.count(cols[k]) ? byCol[cols[k]] : std::string();
            if (cols[k] != "text") {                                // a middle field can hold no comma, no break
                for (char& ch : v) if (ch == ',' || ch == '\r' || ch == '\n') ch = ' ';
            }
            o += v;
        }
    }
    return o;
}
// tx3g / QuickTime text: <uint16 length><text>[boxes]; a 'styl' box's records (startChar, endChar, flags) become
// <b>/<i>/<u> around those CHARACTERS (code points, not bytes).
inline std::string tx3gText(const std::string& sample) {
    if (sample.size() < 2) return std::string();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(sample.data());
    const size_t len = static_cast<size_t>(be(p, 2));
    if (len == 0 || len > sample.size() - 2) return std::string();
    std::string text = sample.substr(2, len);
    if (text.size() >= 2 && static_cast<uint8_t>(text[0]) == 0xFE && static_cast<uint8_t>(text[1]) == 0xFF) {   // UTF-16 BE
        std::wstring w;
        for (size_t i = 2; i + 1 < text.size(); i += 2)
            w += static_cast<wchar_t>((static_cast<uint8_t>(text[i]) << 8) | static_cast<uint8_t>(text[i + 1]));
        text = pbsubr::toUtf8(w);
    }
    // the code points' byte offsets
    std::vector<size_t> cpAt;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) cpAt.push_back(i);
    const size_t ncp = cpAt.size();
    std::vector<uint8_t> fl(ncp, 0);
    bool styled = false;
    Reader m(p, sample.size());
    for (uint64_t at = 2 + len; at + 8 <= sample.size();) {
        bmff::Box b;
        if (!bmff::readBox(m, at, sample.size(), b)) break;
        at = b.end;
        if (b.type != fcc("styl") || b.size() < 2) continue;
        const uint8_t* q = p + b.data;
        const uint64_t cnt = be(q, 2);
        for (uint64_t k = 0; k < cnt && k < 4096 && 2 + (k + 1) * 12 <= b.size(); ++k) {
            const uint8_t* e = q + 2 + k * 12;
            const size_t s0 = static_cast<size_t>(be(e, 2)), e0 = static_cast<size_t>(be(e + 2, 2));
            const uint8_t face = e[6] & 7;                          // bold 1, italic 2, underline 4
            for (size_t c = s0; c < e0 && c < ncp; ++c) { fl[c] |= face; styled = styled || face; }
        }
    }
    if (!styled) return text;
    std::string o;
    uint8_t cur = 0;
    auto close = [&](uint8_t f) { if (f & 4) o += "</u>"; if (f & 2) o += "</i>"; if (f & 1) o += "</b>"; };
    auto open = [&](uint8_t f) { if (f & 1) o += "<b>"; if (f & 2) o += "<i>"; if (f & 4) o += "<u>"; };
    for (size_t c = 0; c < ncp; ++c) {
        if (fl[c] != cur) { close(cur); open(fl[c]); cur = fl[c]; }
        const size_t from = cpAt[c], to = c + 1 < ncp ? cpAt[c + 1] : text.size();
        o += text.substr(from, to - from);
    }
    close(cur);
    return o;
}
// wvtt: a sample is boxes -- 'vttc' (a cue: 'payl' = its text) or 'vtte' (nothing). Several vttc = several cues.
inline void wvttTexts(const std::string& sample, std::vector<std::string>& texts) {
    Reader m(sample.data(), sample.size());
    for (uint64_t at = 0; at + 8 <= sample.size() && texts.size() < 64;) {
        bmff::Box b;
        if (!bmff::readBox(m, at, sample.size(), b)) break;
        at = b.end;
        if (b.type != fcc("vttc")) continue;
        for (uint64_t a2 = b.data; a2 + 8 <= b.end;) {
            bmff::Box c;
            if (!bmff::readBox(m, a2, b.end, c)) break;
            a2 = c.end;
            if (c.type == fcc("payl")) { std::string t; if (m.bytes(c.data, c.size(), t)) texts.push_back(t); }
        }
    }
}
}   // namespace detail

// Matroska blocks -> the cues' times: a block with no duration lasts until the next block of the track (an empty one
// included: that is how a muxer ENDS a subtitle without BlockDuration), at most kNoDurCapMs.
inline void synthesize(const Info& info, const SubTrack& st, std::vector<detail::Blk>& blocks, Extracted& ex) {
    using S = SubTrack::Synth;
    std::stable_sort(blocks.begin(), blocks.end(), [](const detail::Blk& a, const detail::Blk& b) {
        return a.ms < b.ms || (a.ms == b.ms && a.seq < b.seq);
    });
    std::string doc, assHead, formatLine;
    std::vector<std::string> cols;
    if (st.synth == S::Ass) {
        detail::assHeader(st.header, st.ssa, assHead, formatLine);
        cols = detail::assColumns(formatLine);
        doc = assHead + "\r\n[Events]\r\n" + formatLine + "\r\n";
        ex.format = pbsubr::Format::Ass;
    } else if (st.synth == S::Vtt || st.synth == S::VttBlock || st.synth == S::Wvtt) {
        doc = "WEBVTT\r\n\r\n";
        ex.format = pbsubr::Format::Vtt;
    } else {
        ex.format = pbsubr::Format::Srt;
    }
    // the next block's start STRICTLY after each one -- one backward pass (a scan per block was quadratic: 200 000 blocks
    // at one timestamp, a hostile file, would have held the worker for minutes)
    std::vector<int64_t> nextStart(blocks.size(), -1);
    for (size_t i = blocks.size(); i-- > 1;)
        nextStart[i - 1] = blocks[i].ms > blocks[i - 1].ms ? blocks[i].ms : nextStart[i];
    unsigned n = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const detail::Blk& b = blocks[i];
        int64_t s = b.ms, e = 0;
        if (b.hasDur && b.durMs > 0) e = s + static_cast<int64_t>(std::min<uint64_t>(b.durMs, kMaxMediaMs));
        else {
            const int64_t nx = nextStart[i];
            e = (nx > s) ? std::min<int64_t>(nx, s + static_cast<int64_t>(kNoDurCapMs)) : s + static_cast<int64_t>(kNoDurCapMs);
            if (nx <= s && info.durationMs > static_cast<uint64_t>(std::max<int64_t>(s, 0)) && static_cast<int64_t>(info.durationMs) < e)
                e = static_cast<int64_t>(info.durationMs);
        }
        if (e <= 0) { ++ex.skipped; continue; }
        if (s < 0) s = 0;
        if (e <= s) e = s + 1;
        std::string piece;
        if (st.synth == S::Ass) {
            const std::string line = detail::assDialogue(cutNul(b.data), cols, static_cast<uint64_t>(s), static_cast<uint64_t>(e));
            piece = line + "\r\n";
        } else {
            std::string text = cutNul(b.data);
            if (st.synth == S::VttBlock) {                          // "<id>\n<settings>\n<text>" (WebM, ffmpeg)
                const size_t p1 = text.find('\n');
                const size_t p2 = p1 == std::string::npos ? std::string::npos : text.find('\n', p1 + 1);
                if (p2 != std::string::npos) text = text.substr(p2 + 1);
            }
            const std::string body = detail::bodyLines(text);
            if (body.empty()) continue;                             // an empty block: the end of the one before
            if (ex.format == pbsubr::Format::Srt) piece = std::to_string(++n) + "\r\n";
            piece += detail::stamp(static_cast<uint64_t>(s), ex.format == pbsubr::Format::Srt ? ',' : '.') + " --> "
                   + detail::stamp(static_cast<uint64_t>(e), ex.format == pbsubr::Format::Srt ? ',' : '.') + "\r\n" + body + "\r\n\r\n";
        }
        if (doc.size() + piece.size() > kMaxDoc) { ex.truncated = true; break; }
        doc += piece;
        ++ex.cues;
    }
    ex.doc.swap(doc);
}

inline bool extractMatroska(Reader& r, Reader& rw, const Info& info, const SubTrack& st, Extracted& ex,
                            const std::atomic<bool>* cancel, Path path) {
    std::vector<detail::Blk> blocks;
    bool done = false;
    // Auto: the Cues only from a muxer known to index every subtitle block (RV1-1); Cues: forced (the tests)
    const bool tryCues = path == Path::Cues || (path == Path::Auto && cuesIndexEverySub(info.muxingApp, info.writingApp));
    if (tryCues) {
        Extracted tmp;
        std::vector<detail::Blk> b2;
        if (detail::viaCues(r, info, st, b2, tmp, cancel)) {
            blocks.swap(b2);
            ex.blocks = tmp.blocks; ex.laced = tmp.laced; ex.skipped = tmp.skipped; ex.truncated = tmp.truncated;
            ex.payload = tmp.payload;                               // (RV1-2: it read 0 after the Cues path)
            ex.blockPos.swap(tmp.blockPos);
            ex.viaCues = true;
            done = true;
        }
        if (cancelled(cancel)) { ex.err.clear(); return false; }
        if (!done && path == Path::Cues) { ex.err = errDamaged(); return false; }
    }
    if (!done) {
        detail::Walk w{ rw, info, st, blocks, ex, cancel };
        w.all();
        if (cancelled(cancel)) { ex.err.clear(); return false; }
    }
    synthesize(info, st, blocks, ex);
    return true;
}

inline bool extractBmff(Reader& r, const Info& info, const SubTrack& st, Extracted& ex, const std::atomic<bool>* cancel) {
    (void)info;
    auto table = [&](const TableRef& t, std::string& out) {
        return t.has && t.size <= kMaxTable && r.bytes(t.pos, t.size, out);
    };
    std::string stts, ctts, stsc, stsz, stco;
    bool c64 = false, z2 = false;
    if (!table(st.stts, stts) || !table(st.stsc, stsc)) { ex.err = errDamaged(); return false; }
    if (st.stsz.has) { if (!table(st.stsz, stsz)) { ex.err = errDamaged(); return false; } }
    else if (!table(st.stz2, stsz)) { ex.err = errDamaged(); return false; } else z2 = true;
    if (st.stco.has) { if (!table(st.stco, stco)) { ex.err = errDamaged(); return false; } }
    else if (!table(st.co64, stco)) { ex.err = errDamaged(); return false; } else c64 = true;
    if (st.ctts.has) table(st.ctts, ctts);
    auto u32 = [](const std::string& s, size_t at) -> uint64_t {
        return at + 4 <= s.size() ? be(reinterpret_cast<const uint8_t*>(s.data()) + at, 4) : 0;
    };
    // sizes
    std::vector<uint32_t> sizes;
    if (!z2) {
        if (stsz.size() < 12) { ex.err = errDamaged(); return false; }
        const uint64_t fixed = u32(stsz, 4), count = u32(stsz, 8);
        if (count > kMaxSamples) { ex.err = errDamaged(); return false; }
        if (fixed) sizes.assign(static_cast<size_t>(count), static_cast<uint32_t>(fixed));
        else {
            if (count > (stsz.size() - 12) / 4) { ex.err = errDamaged(); return false; }
            sizes.resize(static_cast<size_t>(count));
            for (size_t i = 0; i < sizes.size(); ++i) sizes[i] = static_cast<uint32_t>(u32(stsz, 12 + 4 * i));
        }
    } else {
        if (stsz.size() < 12) { ex.err = errDamaged(); return false; }
        const unsigned field = static_cast<uint8_t>(stsz[7]);
        const uint64_t count = u32(stsz, 8);
        if (count > kMaxSamples || (field != 4 && field != 8 && field != 16)) { ex.err = errDamaged(); return false; }
        const uint64_t need = (count * field + 7) / 8;
        if (need > stsz.size() - 12) { ex.err = errDamaged(); return false; }
        sizes.resize(static_cast<size_t>(count));
        const uint8_t* q = reinterpret_cast<const uint8_t*>(stsz.data()) + 12;
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (field == 16) sizes[i] = static_cast<uint32_t>(be(q + 2 * i, 2));
            else if (field == 8) sizes[i] = q[i];
            else sizes[i] = (i & 1) ? (q[i / 2] & 0x0F) : (q[i / 2] >> 4);
        }
    }
    const size_t nSamples = sizes.size();
    // chunk offsets
    std::vector<uint64_t> chunks;
    {
        if (stco.size() < 8) { ex.err = errDamaged(); return false; }
        const uint64_t count = u32(stco, 4);
        const size_t es = c64 ? 8 : 4;
        if (count > kMaxSamples || count > (stco.size() - 8) / es) { ex.err = errDamaged(); return false; }
        chunks.resize(static_cast<size_t>(count));
        for (size_t i = 0; i < chunks.size(); ++i)
            chunks[i] = c64 ? be(reinterpret_cast<const uint8_t*>(stco.data()) + 8 + 8 * i, 8) : u32(stco, 8 + 4 * i);
    }
    // sample -> offset through stsc
    std::vector<uint64_t> offs(nSamples, UINT64_MAX);
    {
        if (stsc.size() < 8) { ex.err = errDamaged(); return false; }
        const uint64_t count = u32(stsc, 4);
        if (count > kMaxSamples || count > (stsc.size() - 8) / 12) { ex.err = errDamaged(); return false; }
        size_t si = 0;
        for (uint64_t e = 0; e < count && si < nSamples; ++e) {
            if (cancelled(cancel)) { ex.err.clear(); return false; }
            const uint64_t first = u32(stsc, 8 + 12 * e), spc = u32(stsc, 12 + 12 * e);
            const uint64_t nextFirst = e + 1 < count ? u32(stsc, 8 + 12 * (e + 1)) : static_cast<uint64_t>(chunks.size()) + 1;
            if (first == 0 || nextFirst < first) { ex.err = errDamaged(); return false; }
            for (uint64_t c = first; c < nextFirst && c <= chunks.size() && si < nSamples; ++c) {
                uint64_t off = chunks[static_cast<size_t>(c - 1)];
                for (uint64_t k = 0; k < spc && si < nSamples; ++k) {
                    offs[si] = off;
                    off += sizes[si];
                    ++si;
                }
            }
        }
    }
    // decode times (stts) + composition offsets (ctts)
    std::vector<int64_t> dts(nSamples, 0);
    std::vector<uint64_t> durs(nSamples, 0);
    {
        if (stts.size() < 8) { ex.err = errDamaged(); return false; }
        const uint64_t count = u32(stts, 4);
        if (count > kMaxSamples || count > (stts.size() - 8) / 8) { ex.err = errDamaged(); return false; }
        size_t si = 0;
        int64_t t = 0;
        for (uint64_t e = 0; e < count && si < nSamples; ++e) {
            const uint64_t sc = u32(stts, 8 + 8 * e), d = u32(stts, 12 + 8 * e);
            for (uint64_t k = 0; k < sc && si < nSamples; ++k) {
                dts[si] = t;
                durs[si] = d;
                t += static_cast<int64_t>(d);
                if (t > (1ll << 60)) t = 1ll << 60;
                ++si;
            }
        }
        if (ctts.size() >= 8) {
            const uint64_t cc = u32(ctts, 4);
            size_t ci = 0;
            for (uint64_t e = 0; e < cc && e < (ctts.size() - 8) / 8 && ci < nSamples; ++e) {
                const uint64_t sc = u32(ctts, 8 + 8 * e);
                const int64_t off = static_cast<int32_t>(static_cast<uint32_t>(u32(ctts, 12 + 8 * e)));
                for (uint64_t k = 0; k < sc && ci < nSamples; ++k) dts[ci++] += off;
            }
        }
    }
    const double ts = static_cast<double>(st.timescale);
    auto toMs = [&](int64_t ticks) -> int64_t {
        const double v = static_cast<double>(ticks - st.mediaTime) * 1000.0 / ts + static_cast<double>(st.emptyMs);
        if (!(v == v)) return 0;
        if (v > static_cast<double>(kMaxMediaMs)) return static_cast<int64_t>(kMaxMediaMs);
        if (v < -static_cast<double>(kMaxMediaMs)) return -static_cast<int64_t>(kMaxMediaMs);
        return static_cast<int64_t>(v < 0 ? v - 0.5 : v + 0.5);
    };
    std::vector<detail::Blk> blocks;
    for (size_t i = 0; i < nSamples; ++i) {
        if (cancelled(cancel)) { ex.err.clear(); return false; }
        if (offs[i] == UINT64_MAX) break;
        ++ex.blocks;
        const uint32_t sz = sizes[i];
        if (sz > kMaxSubBlock) { ++ex.skipped; continue; }
        if (ex.payload + sz > kMaxPayload) { ex.truncated = true; break; }
        ex.payload += sz;
        std::string data;
        if (sz && !r.bytes(offs[i], sz, data)) { ++ex.skipped; continue; }
        std::vector<std::string> texts;
        if (st.synth == SubTrack::Synth::Wvtt) detail::wvttTexts(data, texts);
        else texts.push_back(detail::tx3gText(data));
        const int64_t s = toMs(dts[i]);
        const uint64_t dMs = static_cast<uint64_t>(std::max<int64_t>(0, toMs(dts[i] + static_cast<int64_t>(std::min<uint64_t>(durs[i], 1ull << 40))) - s));
        for (const auto& t : texts) {
            detail::Blk b;
            b.ms = s;
            b.hasDur = durs[i] > 0;
            b.durMs = dMs;
            b.data = t;                                             // an empty text still ENDS the one before (a gap)
            b.pos = offs[i];
            b.seq = static_cast<uint32_t>(blocks.size());
            if (blocks.size() >= kMaxBlocks) { ex.truncated = true; break; }
            blocks.push_back(std::move(b));
        }
        ex.blockPos.push_back(offs[i]);
    }
    SubTrack as = st;
    as.synth = st.synth == SubTrack::Synth::Wvtt ? SubTrack::Synth::Vtt : SubTrack::Synth::Srt;   // (texts already plain)
    synthesize(info, as, blocks, ex);
    return true;
}

// ONE TEXT SUBTITLE TRACK -> a document (Extracted.doc in Extracted.format). The probe's Info must be this file's.
inline bool extract(Source& src, const Info& info, uint32_t track, Extracted& ex, const std::atomic<bool>* cancel = nullptr,
                    Path path = Path::Auto) {
    ex = Extracted{};
    const SubTrack* st = info.sub(track);
    if (!st) { ex.err = errDamaged(); return false; }
    if (!st->ok) { ex.err = st->why.empty() ? errFormat(st->codec) : st->why; return false; }
    Reader r(src, 64 * 1024);
    bool ok = false;
    if (info.container == "mkv" || info.container == "webm") {
        Reader rw(src, 1u << 20);                                   // the walk: a 1 MB sequential window
        ok = extractMatroska(r, rw, info, *st, ex, cancel, path);
    } else {
        ok = extractBmff(r, info, *st, ex, cancel);
    }
    if (!ok) return false;
    if (ex.cues == 0) { ex.err = ex.blocks ? errDamaged() : errEmpty(); return false; }
    ex.ok = true;
    return true;
}

// What the player wants: the extraction, read by SubtitleReader (UTF-8 forced, the format never sniffed).
struct SubResult {
    bool ok = false;
    std::string err;                    // "" when cancelled
    pbsubr::Result parsed;
    Extracted ex;
};
inline void extractParsed(Source& src, const Info& info, uint32_t track, SubResult& res, const std::atomic<bool>* cancel,
                          Path path = Path::Auto) {
    res = SubResult{};
    if (!extract(src, info, track, res.ex, cancel, path)) { res.err = res.ex.err; return; }
    res.parsed = pbsubr::parseAs(res.ex.doc, pbsubr::kCpUtf8, res.ex.format);
    if (!res.parsed.ok) { res.err = res.parsed.err.empty() ? std::string(errDamaged()) : res.parsed.err; return; }
    res.ex.doc.clear();
    res.ex.doc.shrink_to_fit();                                     // (the cues are what is kept)
    res.ok = true;
}
inline void extractFile(const std::wstring& path, const Info& info, uint32_t track, SubResult& res,
                        const std::atomic<bool>* cancel, Path p = Path::Auto) {
    FileSource f(path, true);
    if (!f.ok()) { res = SubResult{}; res.err = errRead(); return; }
    extractParsed(f, info, track, res, cancel, p);
}

}   // namespace pbmedia

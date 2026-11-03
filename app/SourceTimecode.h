// =============================================================================
// SuVtitle - app/SourceTimecode.h   (the SOURCE TIMECODE a recording carries -- U3, the transcript's timecodes)
//
// U3 (2026-11-02, docs/PROGRESS_U3.md section 4 -- the user: "start of the file, and if the file has a timecode, take it into
// account"). A transcript's stamps are file-relative ("[00:01:23]") unless the recording carries its own START timecode: then
// they are SOURCE timecodes = that start + the position, "[10:00:03:12]" (";" before the frames for drop-frame), what a sound
// engineer reads on his recorder, his camera, his edit. Read here, in the APP, by our own parser -- never by mpv nor the
// engine's decoders (not ours, and a timecode is three numbers in a header):
//   (1) BWF from field recorders -- RIFF/WAVE, RF64 and BW64 (sizes in 'ds64'): 'bext' TimeReference (samples since
//       midnight at the 'fmt ' rate) and, when present, 'iXML' TIMECODE_RATE ("25/1", "30000/1001") and TIMECODE_FLAG
//       (DF / NDF). A bext whose TimeReference is 0 and no iXML timecode = none (a DAW export writes 0).
//   (2) MOV / MP4 from cameras -- the track whose 'hdlr' is 'tmcd': its 'stsd' entry (flags bit 0 = drop frame, timescale,
//       frame duration, frames per second) and its FIRST SAMPLE (a big-endian frame number at its first chunk's offset).
//   (3) MXF -- the header metadata's Timecode Component (StartTimecode, RoundedTimecodeBase, DropFrame) of the MATERIAL
//       package (strong references followed by InstanceUID: package -> track -> sequence -> component), else the first
//       SOURCE package's; the track's EditRate gives the exact rate.
// Anything else -- mkv, ts, mp3, an mp4 without tmcd (Sony XAVC's timecode is in an XML sidecar: not read) -- is "none":
// file-relative stamps. ANY FAILURE IS "NONE" too, silently: truncated, corrupt or lying sizes, a file gone or locked.
//
// READ ONLY WHAT IS NEEDED (the director + the integrator, 02/11). Never a whole box, chunk or partition to find three
// numbers: box / chunk / KLV HEADERS are walked and everything off the path is SKIPPED by its size. MOV / MP4 reads the
// headers on the path moov > trak > mdia > (hdlr) > minf > stbl, the 'tmcd' stsd entry, the first chunk offset, the first
// sample size and the 4 sample bytes -- a moov at the END of a big camera file (tens of MB of tables) costs a few KB. BWF reads
// chunk headers, then 'fmt ', 'bext' and 'iXML' (<= kMaxIxml). MXF reads KLV headers and only the header metadata's sets it
// needs, and stops as soon as the material package's Timecode Component is resolved. A HARD BUDGET of bytes read per file:
// kBudget (2 MB) -- kBudgetMxf (8 MB) for MXF, whose header partition may hold many sets; kMaxPartition (16 MB) is the
// partition's CEILING, never a read size. tests/cpp/timecode_smoke.cpp counts every byte read and asserts the budget,
// a moov at the end of a 6 GB virtual file included.
//
// PURE but for readFileTimecode (CreateFileW + bounded ReadFile, on a WORKER -- a share may be slow): the parser reads
// through a Reader, the smoke feeds it buffers. Header-only, ASCII only, /W4 clean, no /utf-8 (like the product).
// =============================================================================
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pbtc {

inline constexpr uint64_t kBudget       = 2ull << 20;    // bytes read per file, MOV / MP4 / BWF
inline constexpr uint64_t kBudgetMxf    = 8ull << 20;    // ... MXF (its header metadata, set by set)
inline constexpr uint64_t kMaxPartition = 16ull << 20;   // an MXF header partition's ceiling (never a read size)
inline constexpr size_t   kMaxWalk      = 4096;          // boxes / chunks / KLV packets walked at most, in all
inline constexpr size_t   kMaxIxml      = 1u << 20;      // an iXML chunk read at most
inline constexpr size_t   kMaxSet       = 64u << 10;     // one MXF set (or the Primer) read at most

// The bounded reader: `read(offset, out, n)` puts up to n bytes at `offset` into `out` and says how many. `get` asks for
// exactly n bytes inside the file, charges them to the budget, and says false when it cannot -- past the end, over the
// budget (then `over` stays raised: every answer after it is "none"), or a short read.
struct Reader {
    uint64_t size = 0;
    std::function<size_t(uint64_t, void*, size_t)> read;
    uint64_t budget = kBudget;
    uint64_t used = 0;
    size_t   walked = 0;
    bool     over = false;
    bool get(uint64_t off, void* out, size_t n) {
        if (over || !read || off > size || n > size - off) return false;
        if (n > budget - used) { over = true; return false; }
        used += n;
        if (n == 0) return true;
        if (read(off, out, n) != n) { over = true; return false; }
        return true;
    }
    bool step() { if (++walked > kMaxWalk) { over = true; return false; } return true; }
};

struct SourceTc {
    bool        found = false;
    std::string origin;              // "bwf" | "bwf+ixml" | "mov" | "mxf"
    bool        hasRate = false;     // a frame rate is known: startFrames, fps, base, drop
    uint64_t    startFrames = 0;     // frames since 00:00:00:00 at the rate (drop-frame: the frames really counted)
    uint32_t    fpsNum = 0, fpsDen = 1;   // the exact rate: 25/1, 30000/1001, 24000/1001...
    uint32_t    base = 0;            // the rounded frame base: frames per LABELLED second (24, 25, 30, 50, 60)
    bool        drop = false;        // drop-frame labelling (29.97: 2 frame numbers a minute, 59.94: 4)
    uint64_t    startSamples = 0;    // no rate (a BWF without iXML): samples since midnight ...
    uint32_t    sampleRate = 0;      // ... at this rate
};

// ---- small pieces -----------------------------------------------------------------------------------------------------
inline uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint64_t be64(const unsigned char* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
inline uint16_t be16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t le32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t le64(const unsigned char* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
inline uint32_t roundedBase(uint32_t num, uint32_t den) { return den ? (uint32_t)((num + den / 2) / den) : 0; }
// A plausible frame rate: 1-120 fps, a real denominator.
inline bool rateOk(uint32_t num, uint32_t den) { return den > 0 && num > 0 && num / den <= 120 && num >= den; }
// Drop-frame labelling exists for a base of 30 or 60 only (29.97 / 59.94); a file that flags it is believed then (its labels are
// what the recorder or the camera displayed), and ignored on any other base.
inline bool dropOk(uint32_t base) { return base == 30 || base == 60; }

// ---- (1) BWF: RIFF / RF64 / BW64 ----------------------------------------------------------------------------------------
// iXML's value of <tag>...</tag> (the first one), trimmed; "" when absent. Plain text search: iXML is flat enough for it.
inline std::string ixmlValue(const std::string& x, const char* tag) {
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    const size_t a = x.find(open);
    if (a == std::string::npos) return std::string();
    const size_t b = x.find(close, a + open.size());
    if (b == std::string::npos || b - a > 256) return std::string();
    std::string v = x.substr(a + open.size(), b - a - open.size());
    while (!v.empty() && (unsigned char)v.back() <= ' ') v.pop_back();
    size_t k = 0;
    while (k < v.size() && (unsigned char)v[k] <= ' ') ++k;
    return v.substr(k);
}
// "25", "25/1", "30000/1001", "29.97" (and "29.97DF" style suffixes ignored) -> num / den; false when it is none of them.
inline bool parseRate(const std::string& s, uint32_t& num, uint32_t& den) {
    num = 0; den = 1;
    if (s.empty() || s.size() > 24) return false;
    const size_t slash = s.find('/');
    auto digits = [](const std::string& t, uint64_t& v) {
        v = 0;
        if (t.empty() || t.size() > 9) return false;
        for (char c : t) { if (c < '0' || c > '9') return false; v = v * 10 + (uint64_t)(c - '0'); }
        return true;
    };
    uint64_t a = 0, b = 0;
    if (slash != std::string::npos) {
        if (!digits(s.substr(0, slash), a) || !digits(s.substr(slash + 1), b) || b == 0) return false;
        num = (uint32_t)a; den = (uint32_t)b;
        return rateOk(num, den);
    }
    const size_t dot = s.find('.');
    std::string whole = s.substr(0, dot), frac = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    while (!frac.empty() && (frac.back() < '0' || frac.back() > '9')) frac.pop_back();   // "29.97DF" -> "97"
    if (!digits(whole, a)) return false;
    if (frac.empty()) { num = (uint32_t)a; den = 1; return rateOk(num, den); }
    if (!digits(frac, b)) return false;
    // the NTSC family written as decimals: 23.976 / 29.97 / 59.94 / 47.952 / 119.88 -> x1000/1001
    const double v = (double)a + (double)b / std::pow(10.0, (double)frac.size());
    const uint32_t r = (uint32_t)(v + 0.5);
    if (r > 0 && std::fabs(v - r * 1000.0 / 1001.0) < 0.005) { num = r * 1000; den = 1001; return rateOk(num, den); }
    num = (uint32_t)(v * 1000.0 + 0.5); den = 1000;
    return rateOk(num, den);
}
inline SourceTc readBwf(Reader& r, bool rf64) {
    SourceTc tc;
    unsigned char h[16];
    uint64_t ds64Data = 0;
    std::map<std::string, uint64_t> ds64Table;
    uint32_t sampleRate = 0;
    bool haveBext = false, haveIxml = false;
    uint64_t timeRef = 0;
    std::string ixml;
    uint64_t at = 12;
    while (at + 8 <= r.size && r.step()) {
        if (!r.get(at, h, 8)) break;
        const std::string id((const char*)h, 4);
        uint64_t sz = le32(h + 4);
        if (rf64 && sz == 0xFFFFFFFFull) {
            if (id == "data") sz = ds64Data;
            else { auto f = ds64Table.find(id); if (f == ds64Table.end()) break; sz = f->second; }
        }
        const uint64_t body = at + 8;
        if (sz > r.size - body) break;                           // a size that runs past the file: stop (truncated, lying)
        if (id == "ds64" && rf64 && sz >= 28) {
            unsigned char d[28];
            if (!r.get(body, d, 28)) break;
            ds64Data = le64(d + 8);
            const uint32_t n = le32(d + 24);
            for (uint32_t k = 0; k < n && k < 16 && 28 + 12ull * (k + 1) <= sz; ++k) {
                unsigned char e[12];
                if (!r.get(body + 28 + 12ull * k, e, 12)) break;
                ds64Table[std::string((const char*)e, 4)] = le64(e + 4);
            }
        } else if (id == "fmt " && sz >= 16) {
            unsigned char f[16];
            if (!r.get(body, f, 16)) break;
            sampleRate = le32(f + 4);
        } else if (id == "bext" && sz >= 346) {
            unsigned char t[8];
            if (!r.get(body + 338, t, 8)) break;                  // TimeReferenceLow / High
            timeRef = le64(t);
            haveBext = true;
        } else if (id == "iXML" && sz > 0) {
            const size_t n = (size_t)std::min<uint64_t>(sz, kMaxIxml);
            ixml.assign(n, '\0');
            if (!r.get(body, &ixml[0], n)) break;
            haveIxml = true;
        }
        if (sampleRate && haveBext && haveIxml) break;            // all it needs
        at = body + sz + (sz & 1);
    }
    if (r.over || !sampleRate || sampleRate > 768000) return SourceTc{};
    // iXML may carry the stamp itself (TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI / _LO) when there is no bext
    if (!haveBext && haveIxml) {
        const std::string hi = ixmlValue(ixml, "TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI"), lo = ixmlValue(ixml, "TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO");
        if (!hi.empty() && !lo.empty()) {
            timeRef = ((uint64_t)std::strtoul(hi.c_str(), nullptr, 10) << 32) | (uint64_t)std::strtoul(lo.c_str(), nullptr, 10);
            haveBext = true;
        }
    }
    uint32_t num = 0, den = 1;
    const bool rate = haveIxml && parseRate(ixmlValue(ixml, "TIMECODE_RATE"), num, den);
    if (!haveBext || (timeRef == 0 && !rate)) return SourceTc{};   // a DAW's 0 with nothing else: no source timecode
    if (timeRef > 86400ull * sampleRate) timeRef %= 86400ull * sampleRate;   // a recorder past midnight: the day's position
    tc.found = true;
    tc.origin = rate ? "bwf+ixml" : "bwf";
    tc.sampleRate = sampleRate;
    tc.startSamples = timeRef;
    if (rate) {
        tc.hasRate = true;
        tc.fpsNum = num; tc.fpsDen = den;
        tc.base = roundedBase(num, den);
        const std::string flag = ixmlValue(ixml, "TIMECODE_FLAG");
        tc.drop = (flag == "DF" || flag == "df") && dropOk(tc.base);
        tc.startFrames = (uint64_t)((double)timeRef * num / ((double)den * sampleRate) + 1e-6);
    }
    return tc;
}

// ---- (2) MOV / MP4: the 'tmcd' track ----------------------------------------------------------------------------------
// One box header at `at` inside [.., end): its type, where its payload starts and where it ends. false = no box fits.
inline bool boxAt(Reader& r, uint64_t at, uint64_t end, char type[5], uint64_t& body, uint64_t& next) {
    unsigned char h[16];
    if (at + 8 > end || !r.step() || !r.get(at, h, 8)) return false;
    uint64_t sz = be32(h);
    std::memcpy(type, h + 4, 4);
    type[4] = '\0';
    body = at + 8;
    if (sz == 1) {
        if (at + 16 > end || !r.get(at + 8, h + 8, 8)) return false;
        sz = be64(h + 8);
        body = at + 16;
    } else if (sz == 0) {
        sz = end - at;                                           // "to the end of its parent"
    }
    if (sz < body - at || sz > end - at) return false;          // shorter than its header, or past its parent: a lie
    next = at + sz;
    return true;
}
// The first child box of `type` in [from, end); false when none.
inline bool childBox(Reader& r, uint64_t from, uint64_t end, const char* type, uint64_t& body, uint64_t& next) {
    char t[5];
    for (uint64_t at = from; at < end;) {
        uint64_t b = 0, n = 0;
        if (!boxAt(r, at, end, t, b, n)) return false;
        if (std::strcmp(t, type) == 0) { body = b; next = n; return true; }
        if (n <= at) return false;
        at = n;
    }
    return false;
}
inline SourceTc readMov(Reader& r) {
    uint64_t mb = 0, me = 0;
    if (!childBox(r, 0, r.size, "moov", mb, me)) return SourceTc{};
    char t[5];
    for (uint64_t at = mb; at < me;) {                          // every trak of the moov, headers only
        uint64_t tb = 0, te = 0;
        if (!boxAt(r, at, me, t, tb, te) || te <= at) return SourceTc{};
        at = te;
        if (std::strcmp(t, "trak") != 0) continue;
        uint64_t db = 0, de = 0, hb = 0, he = 0, nb = 0, ne = 0, sb = 0, se = 0;
        if (!childBox(r, tb, te, "mdia", db, de)) continue;
        if (!childBox(r, db, de, "hdlr", hb, he) || he - hb < 12) continue;
        unsigned char hd[12];
        if (!r.get(hb, hd, 12)) return SourceTc{};
        if (std::memcmp(hd + 8, "tmcd", 4) != 0) continue;      // not the timecode track: its tables are never read
        if (!childBox(r, db, de, "minf", nb, ne) || !childBox(r, nb, ne, "stbl", sb, se)) return SourceTc{};
        // stsd: version/flags(4) count(4), then the first entry: size(4) 'tmcd' reserved(6) dataRef(2) reserved(4)
        // flags(4) timescale(4) frameDuration(4) numberOfFrames(1)
        uint64_t xb = 0, xe = 0;
        if (!childBox(r, sb, se, "stsd", xb, xe) || xe - xb < 8 + 33) return SourceTc{};
        unsigned char sd[41];
        if (!r.get(xb, sd, 41) || std::memcmp(sd + 12, "tmcd", 4) != 0) return SourceTc{};
        // offsets in sd: 0 version/flags, 4 count, 8 size, 12 'tmcd', 16 reserved(6), 22 dataRef, 24 reserved(4), 28 flags,
        // 32 timescale, 36 frameDuration, 40 numberOfFrames
        const uint32_t flags = be32(sd + 28), scale = be32(sd + 32), dur = be32(sd + 36);
        const uint32_t nfr = sd[40];
        // the first chunk's offset (stco or co64) and the first sample's size (stsz)
        uint64_t off = 0;
        uint64_t cb = 0, ce = 0;
        if (childBox(r, sb, se, "stco", cb, ce) && ce - cb >= 12) {
            unsigned char c[12];
            if (!r.get(cb, c, 12) || be32(c + 4) == 0) return SourceTc{};
            off = be32(c + 8);
        } else if (childBox(r, sb, se, "co64", cb, ce) && ce - cb >= 16) {
            unsigned char c[16];
            if (!r.get(cb, c, 16) || be32(c + 4) == 0) return SourceTc{};
            off = be64(c + 8);
        } else {
            return SourceTc{};
        }
        uint64_t zb = 0, ze = 0;
        if (!childBox(r, sb, se, "stsz", zb, ze) || ze - zb < 12) return SourceTc{};
        unsigned char z[16];
        if (!r.get(zb, z, 12)) return SourceTc{};
        uint32_t ssz = be32(z + 4);
        if (ssz == 0) {
            if (be32(z + 8) == 0 || ze - zb < 16 || !r.get(zb + 12, z + 12, 4)) return SourceTc{};
            ssz = be32(z + 12);
        }
        if (ssz < 4) return SourceTc{};
        unsigned char fr[4];
        if (!r.get(off, fr, 4)) return SourceTc{};
        if (r.over || !scale || !dur || !nfr || nfr > 120) return SourceTc{};
        SourceTc tc;
        tc.found = true;
        tc.origin = "mov";
        tc.hasRate = true;
        tc.fpsNum = scale; tc.fpsDen = dur;
        if (!rateOk(scale, dur)) return SourceTc{};
        tc.base = nfr;
        tc.drop = (flags & 1u) && dropOk(tc.base);
        tc.startFrames = be32(fr);
        return tc;
    }
    return SourceTc{};
}

// ---- (3) MXF: the Timecode Component ----------------------------------------------------------------------------------
// The ULs this reader knows. Compared without byte 7 (the registry's version).
inline bool ulIs(const unsigned char* k, const unsigned char* ul) {
    for (int i = 0; i < 16; ++i) if (i != 7 && k[i] != ul[i]) return false;
    return true;
}
// BER length at `at`: its value and how many bytes it took; false = not one (or longer than 8 bytes).
inline bool berAt(Reader& r, uint64_t at, uint64_t& len, uint32_t& took) {
    unsigned char b[9];
    if (!r.get(at, b, 1)) return false;
    if (b[0] < 0x80) { len = b[0]; took = 1; return true; }
    const uint32_t n = b[0] & 0x7f;
    if (n == 0 || n > 8 || !r.get(at + 1, b + 1, n)) return false;
    len = 0;
    for (uint32_t i = 0; i < n; ++i) len = (len << 8) | b[1 + i];
    took = 1 + n;
    return true;
}
inline SourceTc readMxf(Reader& r) {
    // a HEADER partition pack: 06 0E 2B 34 02 05 01 vv 0D 01 02 01 01 02 ss 00 (ss 1-4: open / closed, incomplete / complete)
    static const unsigned char kPartition[14] = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x02 };
    static const unsigned char kPrimer[16]    = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x05,0x01,0x00 };
    // a header metadata LOCAL SET (2-byte tags, 2-byte lengths): 06 0E 2B 34 02 53 01 vv 0D 01 01 01 01 01 kk 00, its kind kk:
    // 0x36 material package, 0x37 source package, 0x3B timeline track, 0x0F sequence, 0x14 timecode component
    static const unsigned char kSet[14]       = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01 };
    auto prefixIs = [](const unsigned char* key, const unsigned char* p, int n) {
        for (int i = 0; i < n; ++i) if (i != 7 && key[i] != p[i]) return false;
        return true;
    };
    enum Kind { kNone, kMaterial, kSource, kTrack, kSequence, kTimecode };
    // the properties, by their STATIC local tags (what every writer uses; a Primer remapping one is followed below)
    const uint16_t kInstance = 0x3C0A, kTracks = 0x4403, kSeqRef = 0x4803, kEditRate = 0x4B01, kComponents = 0x1001,
                   kStart = 0x1501, kBase = 0x1502, kDrop = 0x1503;
    static const unsigned char ulInstance[16]   = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x01,0x01,0x01,0x15,0x02,0x00,0x00,0x00,0x00 };
    static const unsigned char ulTracks[16]     = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x06,0x01,0x01,0x04,0x06,0x05,0x00,0x00 };
    static const unsigned char ulSeqRef[16]     = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x06,0x01,0x01,0x04,0x02,0x04,0x00,0x00 };
    static const unsigned char ulEditRate[16]   = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x05,0x30,0x04,0x05,0x00,0x00,0x00,0x00 };
    static const unsigned char ulComponents[16] = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x06,0x01,0x01,0x04,0x06,0x09,0x00,0x00 };
    static const unsigned char ulStart[16]      = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x07,0x02,0x01,0x03,0x01,0x05,0x00,0x00 };
    static const unsigned char ulBase[16]       = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x01,0x04,0x04,0x01,0x01,0x02,0x06,0x00,0x00 };
    static const unsigned char ulDrop[16]       = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x01,0x04,0x04,0x01,0x01,0x05,0x00,0x00,0x00 };
    std::map<uint16_t, uint16_t> remap;                          // a Primer's dynamic tag -> the static tag it stands for
    struct SetV { Kind kind = kNone; std::vector<std::string> refs; std::string seq; uint32_t rateNum = 0, rateDen = 0;
                  bool haveStart = false; uint64_t start = 0; uint32_t base = 0; bool drop = false; };
    std::map<std::string, SetV> sets;                           // InstanceUID (16 bytes) -> what it says
    std::vector<std::string> materials, sources;
    // the header partition pack: where the header metadata ends
    unsigned char k[16];
    uint64_t at = 0, len = 0;
    uint32_t took = 0;
    if (!r.get(0, k, 16) || !prefixIs(k, kPartition, 14) || k[14] < 1 || k[14] > 4 || k[15] != 0) return SourceTc{};
    if (!berAt(r, 16, len, took) || len < 88) return SourceTc{};
    unsigned char pp[88];
    if (!r.get(16 + took, pp, 88)) return SourceTc{};
    // Major(2) Minor(2) KAGSize(4) ThisPartition(8) PreviousPartition(8) FooterPartition(8), then HeaderByteCount(8) at 32
    const uint64_t headerBytes = be64(pp + 32);
    if (headerBytes == 0) return SourceTc{};
    at = 16 + took + len;
    const uint64_t end = std::min<uint64_t>(r.size, at + std::min<uint64_t>(headerBytes, kMaxPartition) + 1024);
    auto resolve = [&](const std::vector<std::string>& pkgs, SourceTc& out) {
        for (const auto& p : pkgs) {
            auto pi = sets.find(p);
            if (pi == sets.end()) continue;
            for (const auto& tr : pi->second.refs) {
                auto ti = sets.find(tr);
                if (ti == sets.end() || ti->second.kind != kTrack) continue;
                auto si = sets.find(ti->second.seq);
                if (si == sets.end()) continue;
                std::vector<std::string> comps = si->second.kind == kSequence ? si->second.refs : std::vector<std::string>{ ti->second.seq };
                for (const auto& c : comps) {
                    auto ci = sets.find(c);
                    if (ci == sets.end() || ci->second.kind != kTimecode || !ci->second.haveStart || !ci->second.base
                        || ci->second.base > 120) continue;
                    out = SourceTc{};
                    out.found = true;
                    out.origin = "mxf";
                    out.hasRate = true;
                    out.base = ci->second.base;
                    out.startFrames = ci->second.start;
                    const uint32_t rn = ti->second.rateNum, rd = ti->second.rateDen;
                    if (rateOk(rn, rd) && roundedBase(rn, rd) == out.base) { out.fpsNum = rn; out.fpsDen = rd; }
                    else if (ci->second.drop && (out.base == 30 || out.base == 60)) { out.fpsNum = out.base * 1000; out.fpsDen = 1001; }
                    else { out.fpsNum = out.base; out.fpsDen = 1; }
                    out.drop = ci->second.drop && dropOk(out.base);
                    return true;
                }
            }
        }
        return false;
    };
    SourceTc tc;
    while (at + 17 <= end && r.step()) {
        if (!r.get(at, k, 16) || !berAt(r, at + 16, len, took)) break;
        const uint64_t body = at + 16 + took;
        if (len > r.size - body) break;
        if (ulIs(k, kPrimer) && len >= 8 && len <= kMaxSet) {
            std::vector<unsigned char> v((size_t)len);
            if (!r.get(body, v.data(), v.size())) break;
            const uint32_t n = be32(v.data()), isz = be32(v.data() + 4);
            if (isz == 18)
                for (uint32_t i = 0; i < n && 8 + 18ull * (i + 1) <= len; ++i) {
                    const unsigned char* e = v.data() + 8 + 18 * i;
                    const uint16_t tag = be16(e);
                    const unsigned char* ul = e + 2;
                    const uint16_t stat = ulIs(ul, ulInstance) ? kInstance : ulIs(ul, ulTracks) ? kTracks : ulIs(ul, ulSeqRef) ? kSeqRef
                                        : ulIs(ul, ulEditRate) ? kEditRate : ulIs(ul, ulComponents) ? kComponents : ulIs(ul, ulStart) ? kStart
                                        : ulIs(ul, ulBase) ? kBase : ulIs(ul, ulDrop) ? kDrop : 0;
                    if (stat && tag != stat) remap[tag] = stat;
                }
        } else if (prefixIs(k, kSet, 14) && k[15] == 0x00 && len <= kMaxSet) {
            const Kind kind = k[14] == 0x36 ? kMaterial : k[14] == 0x37 ? kSource : k[14] == 0x3B ? kTrack
                            : k[14] == 0x0F ? kSequence : k[14] == 0x14 ? kTimecode : kNone;
            if (kind != kNone) {                                 // only the sets on the path are READ; the others skipped
                std::vector<unsigned char> v((size_t)len);
                if (!r.get(body, v.data(), v.size())) break;
                SetV s;
                s.kind = kind;
                std::string uid;
                for (size_t p = 0; p + 4 <= v.size();) {
                    uint16_t tag = be16(&v[p]);
                    const uint16_t l = be16(&v[p + 2]);
                    if (p + 4 + l > v.size()) break;
                    const unsigned char* d = &v[p + 4];
                    auto rm = remap.find(tag);
                    if (rm != remap.end()) tag = rm->second;
                    auto batch = [&](std::vector<std::string>& out) {
                        if (l < 8) return;
                        const uint32_t n = be32(d), isz = be32(d + 4);
                        if (isz != 16) return;
                        for (uint32_t i = 0; i < n && 8 + 16ull * (i + 1) <= l; ++i) out.emplace_back((const char*)d + 8 + 16 * i, 16);
                    };
                    if (tag == kInstance && l == 16) uid.assign((const char*)d, 16);
                    else if (tag == kTracks && (kind == kMaterial || kind == kSource)) batch(s.refs);
                    else if (tag == kComponents && kind == kSequence) batch(s.refs);
                    else if (tag == kSeqRef && kind == kTrack && l == 16) s.seq.assign((const char*)d, 16);
                    else if (tag == kEditRate && kind == kTrack && l == 8) { s.rateNum = be32(d); s.rateDen = be32(d + 4); }
                    else if (tag == kStart && kind == kTimecode && l == 8) { s.start = be64(d); s.haveStart = true; }
                    else if (tag == kBase && kind == kTimecode && l == 2) s.base = be16(d);
                    else if (tag == kDrop && kind == kTimecode && l == 1) s.drop = d[0] != 0;
                    p += 4 + l;
                }
                if (!uid.empty()) {
                    sets[uid] = s;
                    if (kind == kMaterial) materials.push_back(uid);
                    if (kind == kSource) sources.push_back(uid);
                }
                // stop as soon as the MATERIAL package's timecode is resolved
                if (!materials.empty() && resolve(materials, tc)) return tc;
            }
        }
        at = body + len;
    }
    if (r.over) return SourceTc{};
    if (resolve(materials, tc) || resolve(sources, tc)) return tc;
    return SourceTc{};
}

// ---- the entry points ---------------------------------------------------------------------------------------------------
// The format by its first bytes, then its reader under its budget. "None" on anything else.
inline SourceTc readSourceTimecode(Reader& r) {
    unsigned char h[16];
    r.used = 0;
    r.walked = 0;
    r.over = false;
    if (r.size < 16 || !r.get(0, h, 16)) return SourceTc{};
    if ((std::memcmp(h, "RIFF", 4) == 0 || std::memcmp(h, "RF64", 4) == 0 || std::memcmp(h, "BW64", 4) == 0)
        && std::memcmp(h + 8, "WAVE", 4) == 0) {
        r.budget = kBudget;
        return readBwf(r, std::memcmp(h, "RIFF", 4) != 0);
    }
    if (h[0] == 0x06 && h[1] == 0x0E && h[2] == 0x2B && h[3] == 0x34) { r.budget = kBudgetMxf; return readMxf(r); }
    static const char* const kTop[] = { "ftyp", "moov", "wide", "mdat", "free", "skip", "pnot", "uuid" };
    for (const char* t : kTop) if (std::memcmp(h + 4, t, 4) == 0) { r.budget = kBudget; return readMov(r); }
    return SourceTc{};
}

// The file at `path`, read shared (the user may have it open), through bounded positioned reads. A file gone, locked or
// unreadable is "none". Runs on a WORKER: a share may be slow.
inline SourceTc readFileTimecode(const std::wstring& path, uint64_t* bytesRead = nullptr) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return SourceTc{};
    LARGE_INTEGER sz{};
    SourceTc tc;
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        Reader r;
        r.size = (uint64_t)sz.QuadPart;
        r.read = [h](uint64_t off, void* out, size_t n) -> size_t {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD got = 0;
            if (n > 0x7FFFFFFF || !::ReadFile(h, out, (DWORD)n, &got, &ov)) return 0;
            return (size_t)got;
        };
        tc = readSourceTimecode(r);
        if (bytesRead) *bytesRead = r.used;
    }
    ::CloseHandle(h);
    return tc;
}

// ---- the stamps ---------------------------------------------------------------------------------------------------------
// A frame count -> its drop-frame label's frame count (29.97: frame numbers 00 and 01 skipped every minute but the tenth;
// 59.94: 00-03): the standard conversion.
inline uint64_t dropFrameLabel(uint64_t frames, uint32_t base) {
    const uint64_t d = base == 60 ? 4 : 2;
    const uint64_t per10 = (uint64_t)base * 600 - d * 9, perMin = (uint64_t)base * 60 - d;
    const uint64_t tens = frames / per10, rem = frames % per10;
    return frames + d * 9 * tens + (rem > d ? d * ((rem - d) / perMin) : 0);
}
// The stamp of a position `posMs` from the start of the file: "[hh:mm:ss:ff]" / "[hh:mm:ss;ff]" with a source rate,
// "[hh:mm:ss]" with a source start and no rate, and the file-relative "[hh:mm:ss]" without a source timecode. 24 h wrap.
inline std::string formatTc(const SourceTc& tc, uint64_t posMs, bool brackets = true) {
    char b[48];
    unsigned long long hh = 0, mm = 0, ss = 0, ff = 0;
    bool frames = false;
    char sep = ':';
    if (tc.found && tc.hasRate && tc.base && tc.fpsDen) {
        const uint64_t perDay = tc.drop ? (uint64_t)24 * 6 * ((uint64_t)tc.base * 600 - (tc.base == 60 ? 4 : 2) * 9)
                                        : (uint64_t)tc.base * 86400;
        uint64_t n = (tc.startFrames + (uint64_t)((double)posMs * tc.fpsNum / ((double)tc.fpsDen * 1000.0) + 1e-6)) % perDay;
        if (tc.drop) { n = dropFrameLabel(n, tc.base); sep = ';'; }
        ff = n % tc.base;
        const uint64_t secs = n / tc.base;
        ss = secs % 60; mm = (secs / 60) % 60; hh = (secs / 3600) % 24;
        frames = true;
    } else {
        uint64_t secs = posMs / 1000;
        if (tc.found && tc.sampleRate) secs = (tc.startSamples + posMs * (uint64_t)tc.sampleRate / 1000) / tc.sampleRate;
        if (tc.found) secs %= 86400;
        ss = secs % 60; mm = (secs / 60) % 60; hh = tc.found ? (secs / 3600) % 24 : secs / 3600;
    }
    if (frames) std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu%c%02llu", hh, mm, ss, sep, ff);
    else        std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu", hh, mm, ss);
    return brackets ? std::string("[") + b + "]" : std::string(b);
}
// "25", "29.97", "23.976", "59.94": the rate as people say it.
inline std::string rateText(uint32_t num, uint32_t den) {
    if (!den) return std::string();
    if (num % den == 0) return std::to_string(num / den);
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", (double)num / den);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
// The header's sentence: which timecode the stamps are.
inline std::string describeTc(const SourceTc& tc) {
    if (!tc.found) return "from the start of the file";
    if (tc.hasRate)
        return "source, " + formatTc(tc, 0, false) + " at " + rateText(tc.fpsNum, tc.fpsDen) + " fps"
             + (tc.drop ? " drop-frame" : "") + ", read from the file";
    return "source, " + formatTc(tc, 0, false) + " (no frame rate in the file), read from the file";
}

}   // namespace pbtc

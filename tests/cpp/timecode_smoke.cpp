// =============================================================================
// timecode_smoke -- U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 4): app/SourceTimecode.h, the SOURCE
// timecode a recording carries (BWF / RF64 / BW64 + iXML, MOV / MP4 tmcd, MXF), read by OUR parser through a bounded,
// budgeted reader.
//   [1] the stamps: formatTc (NDF, 29.97 and 59.94 drop-frame, 23.976, no rate, the 24 h wrap, file-relative), describeTc,
//       parseRate.
//   [2] BWF hand-built: bext alone, bext + iXML (25, 29.97 DF), TimeReference 0 (none / with a rate), iXML after a big data
//       chunk, RF64 and BW64 (sizes in ds64).
//   [3] MOV / MP4 hand-built: a video track with BIG tables before the tmcd track (skipped, never read), stco / co64, the
//       drop-frame flag; THE MOOV AT THE END OF A 6 GB FILE (a virtual reader) read in a few KB.
//   [4] MXF hand-built: the material package's Timecode Component, a source package's only, a Primer-remapped tag, DF.
//   [5] FUZZ-ISH, every fixture: every truncation, every 4-byte window of its first 4 KB set to 0 / 1 / 0x7FFFFFFF /
//       0xFFFFFFFF (lying sizes), 20000 seeded byte flips in all -- never a read outside the file (the test reader counts
//       them), never over the budget, never a crash, never an insane answer (a frame base of 0 or > 120, a start without a
//       rate nor a sample rate): "none", the fixture's own, or another SANE timecode (a flipped digit is another timecode).
//   [6] the files: readFileTimecode on the ffmpeg-made fixtures of make_tc_fixtures.js when a folder is given
//       (`timecode_smoke <dir>`: NOT CHECKED without it), a file gone = none.
// Sentinels: tests/cpp/cmut_u3.py (tc_u3). build_timecode_smoke.cmd also builds an AddressSanitizer variant (--no-heavy skips
// it). /W4, no /utf-8. ASCII only.
// =============================================================================
#include "../../app/SourceTimecode.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
}

// ---- the test reader: a buffer (or a virtual file), every read counted, a read outside the file counted as a FAULT -------
struct Mem {
    std::string bytes;            // the file's start
    uint64_t    virtSize = 0;     // > bytes.size(): a VIRTUAL file -- zeros up to `tailAt`, then `tail`
    uint64_t    tailAt = 0;
    std::string tail;
    uint64_t    cut = UINT64_MAX;  // a TRUNCATED file: its first "cut" bytes (no copy)
    uint64_t    faults = 0, reads = 0;
    uint64_t size() const { return virtSize ? virtSize : std::min<uint64_t>(bytes.size(), cut); }
};
static pbtc::SourceTc readMem(Mem& m, uint64_t* used = nullptr) {
    pbtc::Reader r;
    r.size = m.size();
    r.read = [&m](uint64_t off, void* out, size_t n) -> size_t {
        ++m.reads;
        if (off > m.size() || n > m.size() - off) { ++m.faults; return 0; }
        unsigned char* o = (unsigned char*)out;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t at = off + i;
            if (at < m.bytes.size()) o[i] = (unsigned char)m.bytes[(size_t)at];
            else if (m.virtSize && at >= m.tailAt && at - m.tailAt < m.tail.size()) o[i] = (unsigned char)m.tail[(size_t)(at - m.tailAt)];
            else o[i] = 0;
        }
        return n;
    };
    const pbtc::SourceTc tc = pbtc::readSourceTimecode(r);
    if (used) *used = r.used;
    return tc;
}
static std::string same(const pbtc::SourceTc& a) { return a.found ? pbtc::formatTc(a, 0) + " " + pbtc::rateText(a.fpsNum, a.fpsDen) + (a.drop ? "DF" : "") : "none"; }

// ---- byte builders ------------------------------------------------------------------------------------------------------
static void le32(std::string& b, uint32_t v) { for (int i = 0; i < 4; ++i) b += (char)((v >> (8 * i)) & 0xff); }
static void le64(std::string& b, uint64_t v) { le32(b, (uint32_t)v); le32(b, (uint32_t)(v >> 32)); }
static void le16(std::string& b, uint32_t v) { b += (char)(v & 0xff); b += (char)((v >> 8) & 0xff); }
static void be32(std::string& b, uint32_t v) { for (int i = 3; i >= 0; --i) b += (char)((v >> (8 * i)) & 0xff); }
static void be16(std::string& b, uint32_t v) { b += (char)((v >> 8) & 0xff); b += (char)(v & 0xff); }
static void be64(std::string& b, uint64_t v) { be32(b, (uint32_t)(v >> 32)); be32(b, (uint32_t)v); }
static std::string chunk(const char* id, const std::string& data) {
    std::string c(id, 4);
    le32(c, (uint32_t)data.size());
    c += data;
    if (data.size() & 1) c += '\0';
    return c;
}
static std::string fmtChunk(uint32_t rate) {
    std::string f;
    le16(f, 1); le16(f, 1); le32(f, rate); le32(f, rate * 2); le16(f, 2); le16(f, 16);
    return chunk("fmt ", f);
}
static std::string bextChunk(uint64_t timeRef) {
    std::string b(602, '\0');
    std::string t;
    le64(t, timeRef);
    b.replace(338, 8, t);
    return chunk("bext", b);
}
static std::string ixmlChunk(const std::string& rate, const std::string& flag) {
    std::string x = "<?xml version=\"1.0\"?><BWFXML><IXML_VERSION>1.6</IXML_VERSION><SPEED><TIMECODE_RATE>" + rate
                  + "</TIMECODE_RATE><TIMECODE_FLAG>" + flag + "</TIMECODE_FLAG></SPEED></BWFXML>";
    return chunk("iXML", x);
}
static std::string riff(const std::string& body) {
    std::string w = "RIFF";
    le32(w, (uint32_t)(4 + body.size()));
    return w + "WAVE" + body;
}
static std::string box(const char* type, const std::string& payload) {
    std::string b;
    be32(b, (uint32_t)(8 + payload.size()));
    b.append(type, 4);
    return b + payload;
}
static std::string fullBox(const char* type, uint32_t vf, const std::string& payload) {
    std::string p;
    be32(p, vf);
    return box(type, p + payload);
}
static std::string hdlr(const char* handler) {
    std::string p;
    be32(p, 0);                       // pre_defined
    p.append(handler, 4);
    p += std::string(12, '\0');
    p += "Handler";
    p += '\0';
    return fullBox("hdlr", 0, p);
}
// a tmcd trak whose sample (the frame number) sits at `sampleAt` (absolute), stco or co64
static std::string tmcdTrak(uint32_t flags, uint32_t scale, uint32_t dur, uint32_t nfr, uint64_t sampleAt, bool co64) {
    std::string entry;
    entry += std::string(6, '\0'); be16(entry, 1);    // reserved, data reference index
    be32(entry, 0);                                   // reserved
    be32(entry, flags); be32(entry, scale); be32(entry, dur);
    entry += (char)nfr; entry += '\0';
    std::string stsdP;
    be32(stsdP, 1);
    std::string e;
    be32(e, (uint32_t)(8 + entry.size()));
    e += "tmcd";
    stsdP += e + entry;
    std::string co;
    be32(co, 1);
    if (co64) be64(co, sampleAt); else be32(co, (uint32_t)sampleAt);
    std::string sz;
    be32(sz, 4); be32(sz, 1);                         // every sample 4 bytes, 1 sample
    const std::string stbl = box("stbl", fullBox("stsd", 0, stsdP) + fullBox(co64 ? "co64" : "stco", 0, co) + fullBox("stsz", 0, sz));
    return box("trak", box("mdia", hdlr("tmcd") + box("minf", stbl)));
}
// a video trak with an stsz table of `samples` entries (its stbl must never be READ)
static std::string bigVideoTrak(uint32_t samples) {
    std::string sz;
    be32(sz, 0); be32(sz, samples);
    sz += std::string((size_t)samples * 4, '\x01');
    return box("trak", box("mdia", hdlr("vide") + box("minf", box("stbl", fullBox("stsz", 0, sz)))));
}
// ftyp + moov(video trak + tmcd trak) + mdat holding the frame number
static std::string movFile(uint32_t flags, uint32_t scale, uint32_t dur, uint32_t nfr, uint32_t startFrame, bool co64, uint32_t bigSamples) {
    const std::string ftyp = box("ftyp", std::string("qt  ") + std::string(4, '\0') + "qt  ");
    // the moov's size does not depend on the sample offset's VALUE: build it once to measure, then for real
    const std::string probe = box("moov", bigVideoTrak(bigSamples) + tmcdTrak(flags, scale, dur, nfr, 0, co64));
    const uint64_t sampleAt = ftyp.size() + probe.size() + 8;
    std::string fr;
    be32(fr, startFrame);
    return ftyp + box("moov", bigVideoTrak(bigSamples) + tmcdTrak(flags, scale, dur, nfr, sampleAt, co64)) + box("mdat", fr);
}
// MXF: a KLV with a BER length of 4 bytes
static std::string klv(const unsigned char* key, const std::string& v) {
    std::string k((const char*)key, 16);
    k += '\x83';                                       // BER: 3 length bytes follow
    k += (char)((v.size() >> 16) & 0xff); k += (char)((v.size() >> 8) & 0xff); k += (char)(v.size() & 0xff);
    return k + v;
}
static std::string uid(int n) { std::string u(16, '\0'); u[15] = (char)n; u[0] = 0x55; return u; }
static std::string tagv(uint16_t t, const std::string& v) { std::string o; be16(o, t); be16(o, (uint32_t)v.size()); return o + v; }
static std::string batch(const std::vector<std::string>& ids) { std::string o; be32(o, (uint32_t)ids.size()); be32(o, 16); for (auto& i : ids) o += i; return o; }
static std::string mxfFile(bool material, uint64_t start, uint16_t base, bool drop, uint32_t rn, uint32_t rd, bool remap) {
    static const unsigned char kPart[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x02,0x04,0x00 };
    static const unsigned char kPrimer[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x05,0x01,0x00 };
    unsigned char kPkg[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x36,0x00 };
    unsigned char kTrack[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x3B,0x00 };
    unsigned char kSeq[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x0F,0x00 };
    unsigned char kTc[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x14,0x00 };
    unsigned char kOther[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x47,0x00 };
    if (!material) kPkg[14] = 0x37;
    static const unsigned char ulStart[16] = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02,0x07,0x02,0x01,0x03,0x01,0x05,0x00,0x00 };
    std::string sets;
    std::string s8; be64(s8, start);
    std::string b2; be16(b2, base);
    const uint16_t startTag = (uint16_t)(remap ? 0x8001 : 0x1501);
    sets += klv(kOther, tagv(0x3C0A, uid(9)) + std::string(300, '\x7f'));       // a descriptor: skipped, never read
    sets += klv(kPkg, tagv(0x3C0A, uid(1)) + tagv(0x4403, batch({ uid(2) })));
    std::string er; be32(er, rn); be32(er, rd);
    sets += klv(kTrack, tagv(0x3C0A, uid(2)) + tagv(0x4803, uid(3)) + tagv(0x4B01, er));
    sets += klv(kSeq, tagv(0x3C0A, uid(3)) + tagv(0x1001, batch({ uid(4) })));
    sets += klv(kTc, tagv(0x3C0A, uid(4)) + tagv(startTag, s8) + tagv(0x1502, b2) + tagv(0x1503, std::string(1, drop ? '\x01' : '\x00')));
    std::string primer;
    be32(primer, remap ? 1 : 0); be32(primer, 18);
    if (remap) { be16(primer, 0x8001); primer.append((const char*)ulStart, 16); }
    const std::string header = klv(kPrimer, primer) + sets;
    std::string pp(88, '\0');                         // the partition pack: HeaderByteCount at 32
    std::string hb; be64(hb, header.size());
    pp.replace(32, 8, hb);
    return klv(kPart, pp) + header;
}

int main(int argc, char** argv) {
    std::printf("timecode_smoke: app/SourceTimecode.h -- the source timecode a recording carries\n");
    using pbtc::SourceTc;
    // ---- [1] the stamps
    std::printf("[1] the stamps\n");
    {
        SourceTc nd; nd.found = true; nd.hasRate = true; nd.fpsNum = 25; nd.fpsDen = 1; nd.base = 25; nd.startFrames = 900000;
        ok("[1] 25 fps NDF: 10:00:00:00 + 3.48 s = [10:00:03:12]", pbtc::formatTc(nd, 3480) == "[10:00:03:12]", pbtc::formatTc(nd, 3480));
        SourceTc df; df.found = true; df.hasRate = true; df.fpsNum = 30000; df.fpsDen = 1001; df.base = 30; df.drop = true;
        df.startFrames = 1799;
        ok("[1] 29.97 DF: 00:00:59;29 then 00:01:00;02 (00 and 01 skipped)", pbtc::formatTc(df, 0) == "[00:00:59;29]"
           && pbtc::formatTc(df, 34) == "[00:01:00;02]", pbtc::formatTc(df, 34));
        df.startFrames = 17982;
        ok("[1] 29.97 DF: the 10th minute keeps its frames (17982 -> 00:10:00;00)", pbtc::formatTc(df, 0) == "[00:10:00;00]", pbtc::formatTc(df, 0));
        df.startFrames = 107892;
        ok("[1] 29.97 DF: one hour = 107892 frames -> 01:00:00;00", pbtc::formatTc(df, 0) == "[01:00:00;00]", pbtc::formatTc(df, 0));
        SourceTc d60 = df; d60.fpsNum = 60000; d60.base = 60; d60.startFrames = 3599;
        ok("[1] 59.94 DF: 00:00:59;59 then 00:01:00;04", pbtc::formatTc(d60, 0) == "[00:00:59;59]" && pbtc::formatTc(d60, 17) == "[00:01:00;04]",
           pbtc::formatTc(d60, 17));
        SourceTc s23; s23.found = true; s23.hasRate = true; s23.fpsNum = 24000; s23.fpsDen = 1001; s23.base = 24; s23.startFrames = 24;
        ok("[1] 23.976 NDF: base 24", pbtc::formatTc(s23, 0) == "[00:00:01:00]");
        SourceTc nr; nr.found = true; nr.sampleRate = 48000; nr.startSamples = 1728000000ull;
        ok("[1] a source start with no rate: [hh:mm:ss]", pbtc::formatTc(nr, 1500) == "[10:00:01]", pbtc::formatTc(nr, 1500));
        SourceTc wr = nd; wr.startFrames = 86400ull * 25 - 1;
        ok("[1] the 24 h wrap", pbtc::formatTc(wr, 80) == "[00:00:00:01]", pbtc::formatTc(wr, 80));
        ok("[1] no source timecode: file-relative, hours always shown", pbtc::formatTc(SourceTc{}, 3723456) == "[01:02:03]"
           && pbtc::formatTc(SourceTc{}, 0) == "[00:00:00]");
        ok("[1] describeTc: the header's sentence, each case",
           pbtc::describeTc(nd).find("source, 10:00:00:00 at 25 fps") == 0 && pbtc::describeTc(df).find("drop-frame") != std::string::npos
           && pbtc::describeTc(nr).find("no frame rate") != std::string::npos && pbtc::describeTc(SourceTc{}) == "from the start of the file",
           pbtc::describeTc(df));
        uint32_t n = 0, d = 0;
        ok("[1] parseRate: 25/1, 30000/1001, 25, 29.97, 23.976, 59.94DF; junk refused",
           pbtc::parseRate("25/1", n, d) && n == 25 && d == 1 && pbtc::parseRate("30000/1001", n, d) && n == 30000 && d == 1001
           && pbtc::parseRate("25", n, d) && n == 25 && pbtc::parseRate("29.97", n, d) && n == 30000 && d == 1001
           && pbtc::parseRate("23.976", n, d) && n == 24000 && pbtc::parseRate("59.94DF", n, d) && n == 60000
           && !pbtc::parseRate("", n, d) && !pbtc::parseRate("x/1", n, d) && !pbtc::parseRate("25/0", n, d) && !pbtc::parseRate("1000/1", n, d));
    }
    // ---- [2] BWF
    std::printf("[2] BWF (RIFF / RF64 / BW64, bext, iXML)\n");
    std::vector<std::pair<std::string, Mem>> fixtures;            // every hand-built fixture, for [5]
    std::vector<std::string> expect;
    auto keep = [&](const char* name, const Mem& m, const SourceTc& tc) { fixtures.push_back({ name, m }); expect.push_back(same(tc)); };
    {
        Mem a; a.bytes = riff(fmtChunk(48000) + bextChunk(1728000000ull) + chunk("data", std::string(9600, '\0')));
        uint64_t used = 0;
        const SourceTc t = readMem(a, &used);
        ok("[2] bext alone: found, no rate, 10:00:00", t.found && !t.hasRate && t.origin == "bwf" && pbtc::formatTc(t, 0) == "[10:00:00]", same(t));
        keep("bwf", a, t);
        Mem b; b.bytes = riff(fmtChunk(48000) + bextChunk(1728000000ull) + chunk("data", std::string(96000, '\0')) + ixmlChunk("25/1", "NDF"));
        const SourceTc tb = readMem(b, &used);
        ok("[2] + iXML AFTER a big data chunk (skipped by its size): 25 fps, [10:00:00:00]", tb.found && tb.hasRate && tb.origin == "bwf+ixml"
           && tb.base == 25 && pbtc::formatTc(tb, 0) == "[10:00:00:00]" && used < 4096, same(tb) + " used " + std::to_string(used));
        keep("bwf+ixml", b, tb);
        Mem c; c.bytes = riff(fmtChunk(48000) + bextChunk(1728000000ull) + ixmlChunk("30000/1001", "DF") + chunk("data", std::string(100, '\0')));
        const SourceTc tcc = readMem(c);
        ok("[2] iXML 30000/1001 DF: drop-frame", tcc.found && tcc.drop && tcc.base == 30, same(tcc));
        Mem z; z.bytes = riff(fmtChunk(48000) + bextChunk(0) + chunk("data", std::string(100, '\0')));
        ok("[2] a bext TimeReference of 0 and no iXML: NONE (a DAW export)", !readMem(z).found);
        Mem zr; zr.bytes = riff(fmtChunk(48000) + bextChunk(0) + ixmlChunk("25", "NDF") + chunk("data", std::string(100, '\0')));
        const SourceTc tz = readMem(zr);
        ok("[2] TimeReference 0 WITH an iXML rate: 00:00:00:00", tz.found && pbtc::formatTc(tz, 0) == "[00:00:00:00]", same(tz));
        // RF64: the data chunk's size in ds64, the iXML after it
        for (const char* magic : { "RF64", "BW64" }) {
            std::string ds;
            le64(ds, 0); le64(ds, 96000); le64(ds, 48000); le32(ds, 0);
            std::string body = chunk("ds64", ds) + fmtChunk(48000) + bextChunk(1728000000ull);
            body += "data"; le32(body, 0xFFFFFFFFu); body += std::string(96000, '\0');
            body += ixmlChunk("25/1", "NDF");
            Mem r; r.bytes = std::string(magic) + std::string("\xff\xff\xff\xff", 4) + "WAVE" + body;
            const SourceTc tr = readMem(r);
            ok(std::string("[2] " + std::string(magic) + ": the data size from ds64, the iXML after it").c_str(),
               tr.found && tr.hasRate && pbtc::formatTc(tr, 0) == "[10:00:00:00]", same(tr));
            keep(magic, r, tr);
        }
    }
    // ---- [3] MOV / MP4
    std::printf("[3] MOV / MP4 (the tmcd track)\n");
    {
        Mem a; a.bytes = movFile(0, 2500, 100, 25, 900000, false, 300000);   // a video track with a 1.2 MB stsz first
        uint64_t used = 0;
        const SourceTc t = readMem(a, &used);
        ok("[3] the tmcd track behind a video track with 1.2 MB of tables: found 10:00:00:00 at 25, the tables NEVER read",
           t.found && t.origin == "mov" && t.base == 25 && pbtc::formatTc(t, 0) == "[10:00:00:00]" && used < 2048,
           same(t) + " used " + std::to_string(used));
        Mem small; small.bytes = movFile(0, 2500, 100, 25, 900000, false, 4);
        keep("mov", small, readMem(small));
        Mem b; b.bytes = movFile(1, 30000, 1001, 30, 107892, true, 4);
        const SourceTc tb = readMem(b);
        ok("[3] co64, the drop-frame flag: 01:00:00;00 at 29.97 DF", tb.found && tb.drop && pbtc::formatTc(tb, 0) == "[01:00:00;00]", same(tb));
        keep("mov-df-co64", b, tb);
        Mem none; none.bytes = box("ftyp", "isom") + box("moov", bigVideoTrak(4)) + box("mdat", "x");
        ok("[3] no tmcd track: none", !readMem(none).found);
        // THE MOOV AT THE END OF A 6 GB FILE: ftyp, an mdat of 6 GB (largesize), then the moov -- a virtual reader
        const std::string ftyp = box("ftyp", std::string("qt  ") + std::string(4, '\0') + "qt  ");
        const uint64_t mdatSize = 6ull << 30;
        std::string mdatHead;
        be32(mdatHead, 1); mdatHead += "mdat"; be64(mdatHead, mdatSize);
        const uint64_t moovAt = ftyp.size() + mdatSize;
        // the frame number sits inside the mdat, 64 bytes in
        const uint64_t sampleAt = ftyp.size() + 16 + 64;
        Mem v;
        v.bytes = ftyp + mdatHead + std::string(64, '\0');
        std::string fr; be32(fr, 900000);
        v.bytes += fr;
        v.tail = box("moov", bigVideoTrak(2000000) + tmcdTrak(0, 25, 1, 25, sampleAt, true));   // 8 MB of tables ahead of it
        v.tailAt = moovAt;
        v.virtSize = moovAt + v.tail.size();
        const SourceTc tv = readMem(v, &used);
        std::printf("    a %llu-byte file, its moov last (8 MB of tables in it): %llu bytes read\n",
                    (unsigned long long)v.virtSize, (unsigned long long)used);
        ok("[3] a moov at the END of a 6 GB file, 8 MB of tables in it: the timecode in a few KB (< 4 KB read)",
           tv.found && pbtc::formatTc(tv, 0) == "[10:00:00:00]" && used < 4096 && v.faults == 0, same(tv));
    }
    // ---- [4] MXF
    std::printf("[4] MXF (the Timecode Component)\n");
    {
        Mem a; a.bytes = mxfFile(true, 900000, 25, false, 25, 1, false);
        uint64_t used = 0;
        const SourceTc t = readMem(a, &used);
        ok("[4] the material package -> track -> sequence -> Timecode Component: 10:00:00:00 at 25 (a 300-byte descriptor skipped)",
           t.found && t.origin == "mxf" && pbtc::formatTc(t, 0) == "[10:00:00:00]" && used < a.bytes.size(), same(t) + " used " + std::to_string(used));
        keep("mxf", a, t);
        Mem s; s.bytes = mxfFile(false, 900000, 25, false, 25, 1, false);
        ok("[4] a source package only: its timecode", readMem(s).found);
        Mem rm; rm.bytes = mxfFile(true, 900000, 25, false, 25, 1, true);
        const SourceTc tr = readMem(rm);
        ok("[4] StartTimecode under a Primer-remapped dynamic tag: followed", tr.found && pbtc::formatTc(tr, 0) == "[10:00:00:00]", same(tr));
        keep("mxf-remap", rm, tr);
        Mem df; df.bytes = mxfFile(true, 107892, 30, true, 30000, 1001, false);
        const SourceTc td = readMem(df);
        ok("[4] drop-frame with the track's exact 30000/1001: 01:00:00;00", td.found && td.drop && td.fpsDen == 1001
           && pbtc::formatTc(td, 0) == "[01:00:00;00]", same(td));
        keep("mxf-df", df, td);
        // THE BUDGET ITSELF: a header metadata of 200 TRACK sets of 60 KB -- sets the reader must read (12 MB) and no package
        // to resolve: it stops at kBudgetMxf (8 MB), the answer none -- never a 12 MB read
        static const unsigned char kPartB[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x02,0x04,0x00 };
        static const unsigned char kPrimerB[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x05,0x01,0x01,0x0D,0x01,0x02,0x01,0x01,0x05,0x01,0x00 };
        static const unsigned char kTrackB[16] = { 0x06,0x0E,0x2B,0x34,0x02,0x53,0x01,0x01,0x0D,0x01,0x01,0x01,0x01,0x01,0x3B,0x00 };
        std::string primer;
        be32(primer, 0); be32(primer, 18);
        std::string header = klv(kPrimerB, primer);
        for (int k = 0; k < 200; ++k) header += klv(kTrackB, tagv(0x3C0A, uid(10 + k)) + tagv(0x7F01, std::string(60000, 'x')));
        std::string pp(88, '\0');
        std::string hb; be64(hb, header.size());
        pp.replace(32, 8, hb);
        Mem big; big.bytes = klv(kPartB, pp) + header;
        const SourceTc tb = readMem(big, &used);
        ok("[4] the BUDGET: a header of 200 sets of 60 KB the reader must read (12 MB): stopped at 8 MB, the answer none",
           !tb.found && used <= pbtc::kBudgetMxf && used > pbtc::kBudgetMxf / 2 && big.faults == 0,
           std::to_string(used) + " of " + std::to_string(big.bytes.size()));
    }
    // ---- [5] fuzz-ish
    std::printf("[5] fuzz-ish: truncations, lying sizes, byte flips (%zu fixtures)\n", fixtures.size());
    {
        uint64_t faults = 0, overBudget = 0, runs = 0, odd = 0;
        uint32_t rng = 20261102u;
        auto frand = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (size_t f = 0; f < fixtures.size(); ++f) {
            Mem m = fixtures[f].second;                               // ONE copy, changed in place and restored
            const std::string base = m.bytes;
            auto judge = [&]() {
                uint64_t used = 0;
                m.faults = 0;
                const SourceTc t = readMem(m, &used);
                ++runs;
                faults += m.faults;
                if (used > pbtc::kBudgetMxf) ++overBudget;
                if (t.found && same(t) != expect[f] && (t.hasRate ? (t.base == 0 || t.base > 120) : t.sampleRate == 0)) ++odd;
                (void)pbtc::formatTc(t, 123456);                      // whatever it found must format
                (void)pbtc::describeTc(t);
            };
            const size_t step = base.size() > 4096 ? 7 : 1;
            for (size_t n = 0; n <= base.size(); n += step) { m.cut = n; judge(); }
            m.cut = UINT64_MAX;
            static const uint32_t lies[] = { 0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu };
            const size_t span = std::min<size_t>(base.size(), 4096);
            for (size_t at = 0; at + 4 <= span; ++at)
                for (uint32_t lie : lies) {
                    std::string v;
                    le32(v, lie);
                    m.bytes.replace(at, 4, v);
                    judge();
                    m.bytes.replace(at, 4, base, at, 4);
                }
            for (int k = 0; k < 20000 / (int)fixtures.size(); ++k) {
                const int flips = 1 + (int)(frand() % 4);
                for (int q = 0; q < flips; ++q) m.bytes[frand() % m.bytes.size()] = (char)(frand() & 0xff);
                judge();
                m.bytes = base;
            }
        }
        std::printf("    %llu parses: %llu read(s) outside the file, %llu over the budget, %llu insane answer(s)\n",
                    (unsigned long long)runs, (unsigned long long)faults, (unsigned long long)overBudget, (unsigned long long)odd);
        ok("[5] never a read outside the file, never over the budget, never an insane answer, never a crash",
           runs > 20000 && faults == 0 && overBudget == 0 && odd == 0);
    }
    // ---- [6] the files
    std::printf("[6] the files (readFileTimecode)\n");
    {
        ok("[6] a file that is not there: none", !pbtc::readFileTimecode(L"Z:\\no\\such\\file.mov").found);
        if (argc < 2) {
            std::printf("  NOT CHECKED: the ffmpeg-made fixtures (node tests\\cpp\\make_tc_fixtures.js <dir>, then timecode_smoke <dir>)\n");
        } else {
            const std::string dir = argv[1];
            struct Want { const char* file; const char* stamp; uint32_t base; bool drop; bool rate; };
            static const Want kWant[] = {
                { "tc25.mov", "[10:00:00:00]", 25, false, true }, { "tc2997df.mov", "[01:00:00;00]", 30, true, true },
                { "tc25.mp4", "[10:00:00:00]", 25, false, true }, { "tc25.mxf", "[10:00:00:00]", 25, false, true },
                { "tc2997df.mxf", "[01:00:00;00]", 30, true, true }, { "bwf48k.wav", "[10:00:00]", 0, false, false },
                { "rf64bwf.wav", "[10:00:00]", 0, false, false } };
            for (const Want& w : kWant) {
                const std::string p = dir + "\\" + w.file;
                std::wstring wp(p.begin(), p.end());
                uint64_t used = 0;
                const SourceTc t = pbtc::readFileTimecode(wp, &used);
                const std::string got = t.found ? pbtc::formatTc(t, 0) : std::string("none");
                const bool good = t.found && got == w.stamp && t.hasRate == w.rate && (!w.rate || (t.base == w.base && t.drop == w.drop));
                ok((std::string("[6] ") + w.file + ": " + w.stamp + " (" + std::to_string(used) + " bytes read)").c_str(), good,
                   got + " " + t.origin);
            }
            const std::string p = dir + "\\notc.mp4";
            ok("[6] notc.mp4 (no timecode): none", !pbtc::readFileTimecode(std::wstring(p.begin(), p.end())).found);
        }
    }
    std::printf("\ntimecode_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

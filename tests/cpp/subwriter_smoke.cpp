// =============================================================================
// SuVtitle tests/cpp/subwriter_smoke.cpp -- PORTED 2026-10-23 from AudioPB 641f1aa (PatchBus scratchpad/ui_test/subwriter_smoke.cpp; frozen
// copy: SuVtitle\scratchpad\from_audiopb_641f1aa\ui_test\). Changed: the include. Same checks, same count.
// The original header follows (read ui/ as app/, translator/TrPipe.h as common/TrPipe.h, AudioPB as SuVtitle).
// subwriter_smoke -- the WC (file mode) unit check for ui/SubtitleWriter.h.
//
// Header-only and pure, so it is compiled ALONE (build_subwriter_smoke.cmd, no /utf-8 like
// the product). Every non-ASCII string below is written as UTF-8 BYTES (\x escapes) for the
// same reason the header is ASCII: a typed glyph would compile right only on a machine whose
// ANSI code page happens to be UTF-8.
// =============================================================================
#include "../../app/SubtitleWriter.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else {
        ++g_fail;
        std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str());
    }
}

static std::string vis(const std::string& s) {   // CR/LF made visible for a failure message
    std::string o;
    for (char c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}
static int widthOf(const std::string& s) {
    int w = 0;
    for (char32_t c : pbsub::decodeUtf8(s)) w += pbsub::isWide(c) ? 2 : 1;
    return w;
}
static std::string joinNoSpace(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& s : v) for (char c : s) if (c != ' ') o += c;
    return o;
}
static std::string noSpace(const std::string& s) {
    std::string o;
    for (char c : s) if (c != ' ') o += c;
    return o;
}
static size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}
static pbsub::Cue cue(uint64_t s, uint64_t e, const char* lang, const char* dstLang,
                      const std::string& src, const std::string& dst, bool pass = false) {
    pbsub::Cue c;
    c.startMs = s; c.endMs = e; c.lang = lang; c.dstLang = dstLang;
    c.src = src; c.dst = dst; c.pass = pass;
    return c;
}

// U+4E00.. "one two three ... " ideographs, as UTF-8.
static const char* const kCjk[] = {
    "\xE4\xB8\x80", "\xE4\xBA\x8C", "\xE4\xB8\x89", "\xE5\x9B\x9B", "\xE4\xBA\x94", "\xE5\x85\xAD",
    "\xE4\xB8\x83", "\xE5\x85\xAB", "\xE4\xB9\x9D", "\xE5\x8D\x81", "\xE7\x99\xBE", "\xE5\x8D\x83",
};
static std::string cjkRun(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += kCjk[i % 12];
    return s;
}
static const std::string kComma  = "\xEF\xBC\x8C";   // U+FF0C full-width comma
static const std::string kStop   = "\xE3\x80\x82";   // U+3002 ideographic full stop
static const std::string kOpenQ  = "\xE3\x80\x8C";   // U+300C left corner bracket
static const std::string kCloseQ = "\xE3\x80\x8D";   // U+300D right corner bracket

int main() {
    using pbsub::Which;
    std::printf("== timestamps ==\n");
    ok("0 ms -> 00:00:00,000", pbsub::stamp(0, ',') == "00:00:00,000", pbsub::stamp(0, ','));
    ok("1 h 02 m 03.456 s", pbsub::stamp(3723456, ',') == "01:02:03,456", pbsub::stamp(3723456, ','));
    ok("59.999 s", pbsub::stamp(59999, ',') == "00:00:59,999");
    ok("100 h: hours are never capped", pbsub::stamp(360000000ULL, '.') == "100:00:00.000",
       pbsub::stamp(360000000ULL, '.'));
    ok("VTT separator is a full stop", pbsub::stamp(1500, '.') == "00:00:01.500");

    std::printf("== text cleaning ==\n");
    ok("line breaks/tabs become ONE space, controls vanish, ends trimmed",
       pbsub::cleanText("  a\r\n\r\nb\tc  d \x01" "e  ") == "a b c d e",
       pbsub::cleanText("  a\r\n\r\nb\tc  d \x01" "e  "));
    ok("invalid UTF-8 becomes U+FFFD, never a crash or a silent drop",
       pbsub::cleanText("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    ok("a CESU surrogate is not a character either",
       pbsub::cleanText("x\xED\xA0\x80y") == "x\xEF\xBF\xBDy",
       vis(pbsub::cleanText("x\xED\xA0\x80y")));
    ok("a truncated sequence at the end is replaced, not read past",
       pbsub::cleanText("ok\xE4\xB8") == "ok\xEF\xBF\xBD\xEF\xBF\xBD");
    ok("4-byte characters survive the round trip",
       pbsub::encodeUtf8(pbsub::decodeUtf8("\xF0\x9F\x8E\xA7")) == "\xF0\x9F\x8E\xA7");

    std::printf("== wrapping: Latin ==\n");
    {
        const std::string t = "Hello everyone, thanks for coming to tonight's mixing session.";
        const auto w = pbsub::wrap(t);
        ok("62 columns -> exactly two lines", w.size() == 2, std::to_string(w.size()));
        ok("...both inside 42", w.size() == 2 && widthOf(w[0]) <= 42 && widthOf(w[1]) <= 42);
        const int d = w.size() == 2 ? widthOf(w[0]) - widthOf(w[1]) : 99;
        ok("...balanced (the two lines within 8 columns of each other)", d >= -8 && d <= 8,
           w.size() == 2 ? w[0] + " | " + w[1] : std::string());
        ok("...nothing lost or duplicated", w.size() == 2 && w[0] + " " + w[1] == t);
    }
    {
        const auto w = pbsub::wrap("Short and sweet.");
        ok("a line that fits stays ONE line", w.size() == 1 && w[0] == "Short and sweet.");
    }
    {
        const std::string word(60, 'x');
        const auto w = pbsub::wrap(word);
        ok("one unbreakable 60-letter word is never cut inside", w.size() == 1 && w[0] == word);
    }
    {
        // 107 columns: more than two full lines, less than three.
        const std::string t = "This is a deliberately long subtitle that keeps going past two full "
                              "lines, so it has to become three lines.";
        const auto w = pbsub::wrap(t);
        bool fit = true;
        for (const auto& l : w) fit = fit && widthOf(l) <= 42;
        ok("past two full lines -> three, every one inside 42", w.size() == 3 && fit,
           std::to_string(w.size()));
        ok("...and still nothing lost", joinNoSpace(w) == noSpace(t));
    }
    {
        // The clause bonus: "Yes, " ends a clause early, but taking it would leave a long second
        // line -- the balance must still win here.
        const auto w = pbsub::wrap("Yes, and then we will record the drums tomorrow morning at nine");
        ok("a clause end does not buy a lopsided split", w.size() == 2 && widthOf(w[0]) > 20,
           w.empty() ? std::string() : w[0]);
    }

    std::printf("== wrapping: CJK ==\n");
    {
        const std::string t = cjkRun(24);   // 48 columns, all wide
        const auto w = pbsub::wrap(t);
        ok("24 ideographs -> two lines of 12 (16 per line is the CJK limit)",
           w.size() == 2 && widthOf(w[0]) == 24 && widthOf(w[1]) == 24,
           w.size() == 2 ? std::to_string(widthOf(w[0])) + "/" + std::to_string(widthOf(w[1])) : "");
        ok("...and the characters are all there, in order", w.size() == 2 && w[0] + w[1] == t);
    }
    {
        const auto w = pbsub::wrap(cjkRun(16));
        ok("16 ideographs is exactly one full line", w.size() == 1);
    }
    {
        // 12 ideographs, the full-width comma, 11 more: a 12|12 split would START line 2 with
        // the comma. Kinsoku forbids it; the break goes after the comma instead.
        const std::string t = cjkRun(12) + kComma + cjkRun(11);
        const auto w = pbsub::wrap(t);
        const bool startsWithComma = w.size() == 2 && w[1].compare(0, kComma.size(), kComma) == 0;
        ok("no line starts with a clause comma", w.size() == 2 && !startsWithComma);
        ok("...the break lands right after it (the clause end is preferred)",
           w.size() == 2 && w[0] == cjkRun(12) + kComma, w.empty() ? std::string() : w[0]);
    }
    {
        // KINSOKU proper, with nothing else pulling the same way: a CLOSING BRACKET (not a
        // clause end, so no bonus) sits exactly at the perfect 24|24 cut. Only the no-line-start
        // rule keeps it off the head of line 2.
        const std::string t = cjkRun(12) + kCloseQ + cjkRun(11);
        const auto w = pbsub::wrap(t);
        ok("KINSOKU: a closing bracket never starts a line, even at the perfect cut",
           w.size() == 2 && w[1].compare(0, kCloseQ.size(), kCloseQ) != 0
           && w[0] + w[1] == t, w.size() == 2 ? w[1] : std::string());
    }
    {
        // An ideographic full stop exactly at the balanced cut.
        const std::string t = cjkRun(12) + kStop + cjkRun(12);
        const auto w = pbsub::wrap(t);
        bool bad = false;
        for (size_t i = 1; i < w.size(); ++i)
            if (w[i].compare(0, kStop.size(), kStop) == 0) bad = true;
        ok("KINSOKU: no line starts with a full stop", !w.empty() && !bad);
    }
    {
        // An opening corner bracket right where the balanced cut would fall: it must not be
        // left dangling at the END of line 1.
        const std::string t = cjkRun(11) + kOpenQ + cjkRun(5) + kCloseQ + cjkRun(6);
        const auto w = pbsub::wrap(t);
        bool bad = false;
        for (size_t i = 0; i + 1 < w.size(); ++i) {
            const std::string& l = w[i];
            if (l.size() >= kOpenQ.size() && l.compare(l.size() - kOpenQ.size(), kOpenQ.size(), kOpenQ) == 0)
                bad = true;
        }
        ok("KINSOKU: no line ends with an opening bracket", w.size() >= 2 && !bad);
        ok("...and none starts with the closing one", w.size() >= 2
           && w[1].compare(0, kCloseQ.size(), kCloseQ) != 0);
    }
    {
        // Mixed: ideographs around a Latin word. The Latin word is never split; the cut may sit
        // at the boundary between scripts.
        const std::string t = cjkRun(10) + "iPhone" + cjkRun(10);
        const auto w = pbsub::wrap(t);
        bool split = false;
        for (const auto& l : w) {
            const size_t at = l.find("iPh");
            if (at != std::string::npos && l.find("iPhone") == std::string::npos) split = true;
        }
        ok("mixed CJK + Latin: wrapped, and the Latin word stays whole", w.size() == 2 && !split);
        ok("...nothing lost", joinNoSpace(w) == noSpace(t));
    }

    std::printf("== SRT ==\n");
    {
        std::vector<pbsub::Cue> cs;
        cs.push_back(cue(1000, 3500, "en", "fr", "Hello there.", "Bonjour."));
        cs.push_back(cue(3723456, 3725000, "en", "fr", "Second.", "Deuxi\xC3\xA8me."));
        const std::string s = pbsub::srt(cs, Which::Dst);
        const std::string want =
            "\xEF\xBB\xBF"
            "1\r\n00:00:01,000 --> 00:00:03,500\r\nBonjour.\r\n\r\n"
            "2\r\n01:02:03,456 --> 01:02:05,000\r\nDeuxi\xC3\xA8me.\r\n\r\n";
        ok("exact bytes: BOM, numbering from 1, comma stamps, CRLF, blank line after each cue",
           s == want, vis(s));
    }
    {
        std::vector<pbsub::Cue> cs;
        cs.push_back(cue(0, 2000, "en", "fr", "a", "Line one\n\nline two\r\nand three"));
        cs.push_back(cue(2000, 2000, "en", "fr", "b", "Zero length"));
        cs.push_back(cue(3000, 4000, "en", "fr", "", ""));                      // no text at all
        cs.push_back(cue(5000, 6000, "en", "fr", "c", "He said --> stop"));
        const std::string s = pbsub::srt(cs, Which::Dst);
        size_t lone = 0;
        for (size_t i = 0; i < s.size(); ++i) if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) ++lone;
        ok("every newline is CRLF", lone == 0);
        ok("a blank line INSIDE a cue's text is collapsed (one cue = one blank line)",
           count(s, "\r\n\r\n") == 3, std::to_string(count(s, "\r\n\r\n")));
        ok("a zero-length cue still ends AFTER it starts",
           s.find("00:00:02,000 --> 00:00:02,001") != std::string::npos);
        ok("a cue with no text is skipped and the numbering stays contiguous",
           s.find("\r\n3\r\n00:00:05,000") != std::string::npos && s.find("\r\n4\r\n") == std::string::npos,
           vis(s));
        ok("\"-->\" inside a sentence is defused", s.find("He said -> stop") != std::string::npos
           && count(s, "-->") == 3);
    }
    {
        std::vector<pbsub::Cue> cs;
        cs.push_back(cue(0, 1000, "en", "fr", "Hello.", "Bonjour."));
        cs.push_back(cue(1000, 2000, "fr", "fr", "D\xC3\xA9j\xC3\xA0 en fran\xC3\xA7" "ais.",
                         "D\xC3\xA9j\xC3\xA0 en fran\xC3\xA7" "ais.", /*pass=*/true));
        const std::string both = pbsub::srt(cs, Which::Both);
        ok("Both: translation, then the original in <i>",
           both.find("Bonjour.\r\n<i>Hello.</i>\r\n") != std::string::npos, vis(both));
        ok("...a PASSTHROUGH is written once, never as its own original",
           count(both, "D\xC3\xA9j\xC3\xA0 en fran\xC3\xA7" "ais.") == 1);
        const std::string src = pbsub::srt(cs, Which::Src);
        ok("Src: the original language only",
           src.find("Hello.") != std::string::npos && src.find("Bonjour.") == std::string::npos);
    }
    {
        std::vector<pbsub::Cue> cs;
        cs.push_back(cue(0, 4000, "en", "zh-tw", "x", cjkRun(24)));
        const std::string s = pbsub::srt(cs, Which::Dst);
        ok("a CJK cue is written as two balanced lines of 12",
           s.find("\r\n" + cjkRun(12) + "\r\n" + cjkRun(12) + "\r\n\r\n") != std::string::npos,
           vis(s));
    }

    std::printf("== VTT ==\n");
    {
        std::vector<pbsub::Cue> cs;
        cs.push_back(cue(1000, 2500, "en", "fr", "A & B < C > D --> E", "A & B < C > D --> E"));
        cs.push_back(cue(3000, 4000, "en", "fr", "Hello.", "Bonjour."));
        const std::string v = pbsub::vtt(cs, Which::Both);
        ok("header: BOM + WEBVTT + a blank line",
           v.compare(0, 3 + 10, "\xEF\xBB\xBF" "WEBVTT\r\n\r\n") == 0, vis(v.substr(0, 20)));
        ok("full-stop stamps", v.find("00:00:01.000 --> 00:00:02.500") != std::string::npos);
        ok("& < > escaped, so no tag and no arrow can come out of a sentence",
           v.find("A &amp; B &lt; C &gt; D --&gt; E") != std::string::npos
           && count(v, "-->") == 2, vis(v));
        ok("the original rides a class span players may style, in italics",
           v.find("Bonjour.\r\n<c.orig><i>Hello.</i></c>\r\n") != std::string::npos);
    }

    std::printf("== language tags ==\n");
    ok("zh-tw -> zh-TW", pbsub::langTag("zh-tw") == "zh-TW");
    ok("a plain code lower case", pbsub::langTag("FR") == "fr");

    // ---- review of WC (2026-10-10) --------------------------------------------------------------
    std::printf("== kinsoku at SPACES (French typography) ==\n");
    {
        // Measured on the previous rule: the best-balanced break that FITS was the space before
        // " ? Il", so line 2 opened with the question mark.
        const std::string t = "Tu as vu ce qu'a fait ce jeune r\xC3\xA9" "alisateur ? Il a tout tourn\xC3\xA9 en une nuit.";
        const auto w = pbsub::wrap(t);
        ok("a French \" ?\" stays at the END of its line, never opens the next",
           w.size() == 2 && w[0] == "Tu as vu ce qu'a fait ce jeune r\xC3\xA9" "alisateur ?"
           && w[1] == "Il a tout tourn\xC3\xA9 en une nuit.",
           w.size() == 2 ? w[0] + " | " + w[1] : std::to_string(w.size()));
    }
    {
        const std::string t = "On appelle \xC3\xA7" "a \xC2\xAB le silence \xC2\xBB partout ici, tu sais.";
        const auto w = pbsub::wrap(t);
        ok("a closing guillemet stays with its words (a closing guillemet never opens a line)",
           w.size() == 2 && w[0] == "On appelle \xC3\xA7" "a \xC2\xAB le silence \xC2\xBB" && w[1] == "partout ici, tu sais.",
           w.size() == 2 ? w[0] + " | " + w[1] : std::to_string(w.size()));
    }
    {
        // ...and as a PROPERTY over sentences that put the spaced marks at every position.
        const char* kFr[] = {
            "Est-ce que tu connais le nom du r\xC3\xA9" "alisateur ? Il a tout tourn\xC3\xA9 seul.",
            "Le chef op a enfin trouv\xC3\xA9 la bonne lumi\xC3\xA8re ! On tourne demain matin, promis.",
            "Il a dit une chose importante au r\xC3\xA9" "alisateur : on recommence demain.",
            "C'est ce qu'il appelle \xC2\xAB le grand silence \xC2\xBB dans tout le film ; et c'est vrai.",
            "Il appelle \xC3\xA7" "a \xC2\xAB la sc\xC3\xA8ne du grand escalier \xC2\xBB depuis le premier jour.",
        };
        bool bad = false;
        std::string where;
        for (const char* s : kFr) {
            for (const auto& l : pbsub::wrap(s)) {
                const std::u32string u = pbsub::decodeUtf8(l);
                if (u.empty()) continue;
                if (pbsub::noLineStart(u.front()) || pbsub::noLineEnd(u.back())) { bad = true; where = l; }
            }
        }
        ok("no line of any of them starts with ? ! : ; or a closing guillemet, or ends with an opening one", !bad, where);
    }
    std::printf("== srtSafe leaves no \"-->\" at all ==\n");
    {
        const char* kIn[] = { "--->", "---->", "a-->b", "-->-->", "x --> y ---> z", "<!-- note -->", "------>" };
        bool bad = false;
        std::string where;
        for (const char* s : kIn) {
            const std::string o = pbsub::srtSafe(s);
            if (o.find("-->") != std::string::npos) { bad = true; where = std::string(s) + " => " + o; }
        }
        ok("no input re-forms \"-->\" (\"--->\" used to become \"-->\")", !bad, where);
        ok("...and a lone arrow is still just shortened", pbsub::srtSafe("a-->b") == "a->b", pbsub::srtSafe("a-->b"));
        const std::string s = pbsub::srt({ cue(0, 1000, "en", "fr", "x", "go ---> there") }, pbsub::Which::Dst);
        ok("...through srt() too: the only \"-->\" in the file is the timing line's", count(s, "-->") == 1, vis(s));
    }
    {
        // WG review #14: a line break the USER typed (the editor's Shift+Enter) stays a line break in the file
        const std::string s = pbsub::srt({ cue(0, 1000, "fr", "en", "Premier\nSecond", "First line\r\nSecond line") }, pbsub::Which::Both);
        ok("a typed line break is KEPT (each part wrapped on its own): \"First line\" / \"Second line\" over the original's two",
           s.find("\r\nFirst line\r\nSecond line\r\n<i>Premier\r\nSecond</i>\r\n") != std::string::npos, vis(s));
        const std::string v = pbsub::vtt({ cue(0, 1000, "fr", "en", "x", "A & B\nC") }, pbsub::Which::Dst);
        ok("...in WebVTT too (each line escaped)", v.find("\r\nA &amp; B\r\nC\r\n") != std::string::npos, vis(v));
        const std::vector<std::string> w = pbsub::wrapKeepingBreaks("Hello everyone, thanks for coming to tonight's mixing session.\nOK.");
        ok("...a long part is still wrapped (2 lines) and the short one after the break stays its own line",
           w.size() == 3 && w[2] == "OK.", std::to_string(w.size()));
        const std::vector<std::string> plain = pbsub::wrapKeepingBreaks("Short and sweet.");
        ok("...a text with no break: exactly wrap()'s lines (an analysis' text is untouched)",
           plain == pbsub::wrap("Short and sweet."));
    }

    std::printf("\n%s: %d passed, %d failed\n", g_fail ? "SUBWRITER SMOKE FAILED" : "SUBWRITER SMOKE OK",
                g_pass, g_fail);
    return g_fail ? 1 : 0;
}

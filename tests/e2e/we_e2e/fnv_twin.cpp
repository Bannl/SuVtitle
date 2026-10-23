// =============================================================================
// SuVtitle tests/e2e/we_e2e/fnv_twin.cpp -- the PRODUCT's media/folder keys and virtual-host URLs, printed by the product's
// own code, so the we_e2e kit's JavaScript formula (hosts.js) can be compared with it (check_hosts.js). T-E2E, 2026-10-23
// (RV4-12: the kit used the textbook FNV-1a offset basis, the product uses 1469598103934665603 -- see app\TrFileStore.h).
//
// NOT product code. It #includes app\TrPlayer.h (-> TrFileStore.h, common\TrPipe.h) and calls the very functions the
// player calls in loadItem (app\TrPlayer.h, "n.mediaKey = ... n.host = ... n.url = ..."): pbtrf::fileIdentity,
// pbtrf::mediaKey, pbtrf::folderKey, pbtrf::pctEncode, pbplayer::kMediaHostSuffix. The two lines that COMPOSE host and url
// are repeated here (they live inside a Player method, which needs a whole fake host to run); tests\web\test_host_pins.js
// pins those two product lines textually, so a change there fails that pin instead of silently drifting from this helper.
//
// Protocol: stdin, one request per line, TAB-separated, UTF-8 (the answer is one line per request, TAB-separated):
//   fnv<TAB><string>                 -> hex16(fnv1a64(string))
//   folder<TAB><folder>              -> folderKey(folder)
//   media<TAB><path><TAB><size><TAB><mtime>   -> mediaKey(path, size, mtime)       (mtime = FILETIME, 100 ns ticks)
//   item<TAB><path>                  -> <mediaKey> <TAB> <host> <TAB> <url>, the way Player::loadItem builds them for a real
//                                       file (size and FILETIME read by pbtrf::fileIdentity, folder = pbtrf::dirOf(path))
// A line it cannot read answers "ERR". ASCII-only source, no /utf-8 (like the product).
// =============================================================================
#include "../../../app/TrPlayer.h"

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    size_t at = 0;
    for (;;) {
        const size_t t = s.find('\t', at);
        if (t == std::string::npos) { v.push_back(s.substr(at)); break; }
        v.push_back(s.substr(at, t - at));
        at = t + 1;
    }
    return v;
}
static void say(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); std::fputc('\n', stdout); }

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string line;
    int c;
    for (;;) {
        line.clear();
        bool any = false;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n') { line.push_back((char)c); any = true; }
        if (!any && c == EOF) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line);
        // (fnv and folder take the REST of the line: the string may hold tabs -- "dir<TAB>..." is the folder key's own prefix)
        const size_t tab = line.find('\t');
        const std::string rest = tab == std::string::npos ? std::string() : line.substr(tab + 1);
        if (f[0] == "fnv" && tab != std::string::npos) {
            say(pbtrf::hex16(pbtrf::fnv1a64(rest)));
        } else if (f[0] == "folder" && tab != std::string::npos) {
            say(pbtrf::folderKey(pbtrf::fromUtf8(rest)));
        } else if (f[0] == "media" && f.size() == 4) {
            say(pbtrf::mediaKey(pbtrf::fromUtf8(f[1]), std::strtoull(f[2].c_str(), nullptr, 10), std::strtoull(f[3].c_str(), nullptr, 10)));
        } else if (f[0] == "item" && f.size() == 2) {
            const std::wstring path = pbtrf::fromUtf8(f[1]);
            uint64_t size = 0, mtime = 0;
            if (!pbtrf::fileIdentity(path, size, mtime)) { say("ERR"); }
            else {
                const std::wstring folder = pbtrf::dirOf(path), name = pbtrf::leafOf(path);
                const std::string mediaKey = pbtrf::mediaKey(path, size, mtime);
                // app\TrPlayer.h loadItem, the same two lines:
                const std::wstring host = L"f" + pbtrf::fromUtf8(pbtrf::folderKey(folder)) + pbplayer::kMediaHostSuffix;
                const std::wstring url = L"https://" + host + L"/" + pbtrf::fromUtf8(pbtrf::pctEncode(pbtrf::toUtf8(name))) + L"?v=" + pbtrf::fromUtf8(mediaKey);
                say(mediaKey + "\t" + pbtrf::toUtf8(host) + "\t" + pbtrf::toUtf8(url));
            }
        } else {
            say("ERR");
        }
    }
    return 0;
}

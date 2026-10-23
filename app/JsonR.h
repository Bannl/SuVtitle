// =============================================================================
// SuVtitle - app/JsonR.h   (a strict bounded JSON reader)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/JsonR.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: nothing but this header.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// A small, STRICT JSON READER for the few bridge messages that carry structure. The bridge reads its flat
// messages with jsonFindString/Number/Bool (bridge.cpp), which is fine for {type, id, on}, but the subtitle
// editor's edit (wave WG: "remove these cues, add these", each with two texts that may hold any character, a
// quote and a backslash included) needs a real parser: an escaped quote must not end a string, \uXXXX must decode.
//
// The input is WebView2's get_WebMessageAsJson -- UTF-16, produced by the browser's own JSON.stringify -- but it is
// still treated as UNTRUSTED: bounded depth (32), bounded size (the caller's), a bounded NUMBER OF VALUES (256 K: a
// "[0,0,0,...]" under the size cap would otherwise build ~1 GB of Values and throw bad_alloc out of a WebView2 callback
// -- review of WG #17), no recursion past the depth, every failure a clean `false`. Strings are UTF-16 (surrogate
// pairs pass through as code units, which is what a wstring holds). Numbers are doubles, read back as integers by the
// accessors when asked.
//
// Header-only, no dependency, pure: proved in scratchpad/ui_test/wg_smoke.cpp. ASCII only (no /utf-8).
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace pbjson {

struct Value {
    enum class Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<Value> a;
    std::vector<std::pair<std::wstring, Value>> o;

    bool isArr() const { return kind == Kind::Arr; }
    bool isObj() const { return kind == Kind::Obj; }
    // object member, or nullptr
    const Value* get(const wchar_t* key) const {
        if (kind != Kind::Obj) return nullptr;
        for (const auto& m : o) if (m.first == key) return &m.second;
        return nullptr;
    }
    std::wstring str(const wchar_t* key, const std::wstring& dflt = std::wstring()) const {
        const Value* v = get(key);
        return v && v->kind == Kind::Str ? v->s : dflt;
    }
    // a non-negative integer member (a cue time, an id): false if absent, not a number, negative or fractional
    bool u64(const wchar_t* key, uint64_t& out) const {
        const Value* v = get(key);
        return v && v->asU64(out);
    }
    bool asU64(uint64_t& out) const {
        if (kind != Kind::Num || !(n >= 0) || n > 9007199254740992.0 || n != static_cast<double>(static_cast<uint64_t>(n))) return false;
        out = static_cast<uint64_t>(n);
        return true;
    }
    bool flag(const wchar_t* key, bool dflt = false) const {
        const Value* v = get(key);
        return v && v->kind == Kind::Bool ? v->b : dflt;
    }
};

class Reader {
public:
    explicit Reader(const std::wstring& t) : s_(t) {}
    bool parse(Value& out) {
        i_ = 0;
        values_ = 0;
        ws();
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }
private:
    const std::wstring& s_;
    size_t i_ = 0;
    size_t values_ = 0;
public:
    static constexpr int kMaxDepth = 32;
    // (the largest real message: a Shift of 20 000 AudioPB lines = 20 000 objects of 8 members + 20 000 removed idx
    // + the envelope, ~200 000 values)
    static constexpr size_t kMaxValues = 1u << 18;
private:

    void ws() { while (i_ < s_.size() && (s_[i_] == L' ' || s_[i_] == L'\t' || s_[i_] == L'\n' || s_[i_] == L'\r')) ++i_; }
    bool lit(const wchar_t* w) {
        size_t k = 0;
        while (w[k]) { if (i_ + k >= s_.size() || s_[i_ + k] != w[k]) return false; ++k; }
        i_ += k;
        return true;
    }
    static int hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }
    bool string(std::wstring& out) {
        if (i_ >= s_.size() || s_[i_] != L'"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const wchar_t c = s_[i_++];
            if (c == L'"') return true;
            if (c < 0x20) return false;                         // a raw control character: not JSON
            if (c != L'\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            const wchar_t e = s_[i_++];
            switch (e) {
                case L'"': out += L'"'; break;
                case L'\\': out += L'\\'; break;
                case L'/': out += L'/'; break;
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u': {
                    if (i_ + 4 > s_.size()) return false;
                    int v = 0;
                    for (int k = 0; k < 4; ++k) { const int h = hex(s_[i_ + k]); if (h < 0) return false; v = v * 16 + h; }
                    i_ += 4;
                    out += static_cast<wchar_t>(v);
                    break;
                }
                default: return false;
            }
        }
        return false;                                         // unterminated
    }
    bool number(double& out) {
        const size_t b = i_;
        if (i_ < s_.size() && s_[i_] == L'-') ++i_;
        if (i_ >= s_.size()) return false;
        if (s_[i_] == L'0') ++i_;
        else if (s_[i_] >= L'1' && s_[i_] <= L'9') { while (i_ < s_.size() && s_[i_] >= L'0' && s_[i_] <= L'9') ++i_; }
        else return false;
        if (i_ < s_.size() && s_[i_] == L'.') {
            ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= L'0' && s_[i_] <= L'9')) return false;
            while (i_ < s_.size() && s_[i_] >= L'0' && s_[i_] <= L'9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == L'e' || s_[i_] == L'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == L'+' || s_[i_] == L'-')) ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= L'0' && s_[i_] <= L'9')) return false;
            while (i_ < s_.size() && s_[i_] >= L'0' && s_[i_] <= L'9') ++i_;
        }
        const std::wstring t = s_.substr(b, i_ - b);
        wchar_t* end = nullptr;
        out = std::wcstod(t.c_str(), &end);
        return end && *end == 0;
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth || i_ >= s_.size() || ++values_ > kMaxValues) return false;
        const wchar_t c = s_[i_];
        if (c == L'{') {
            v.kind = Value::Kind::Obj;
            ++i_; ws();
            if (i_ < s_.size() && s_[i_] == L'}') { ++i_; return true; }
            for (;;) {
                ws();
                std::wstring k;
                if (!string(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != L':') return false;
                ++i_; ws();
                Value m;
                if (!value(m, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(m));
                ws();
                if (i_ < s_.size() && s_[i_] == L',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == L'}') { ++i_; return true; }
                return false;
            }
        }
        if (c == L'[') {
            v.kind = Value::Kind::Arr;
            ++i_; ws();
            if (i_ < s_.size() && s_[i_] == L']') { ++i_; return true; }
            for (;;) {
                ws();
                Value m;
                if (!value(m, depth + 1)) return false;
                v.a.push_back(std::move(m));
                ws();
                if (i_ < s_.size() && s_[i_] == L',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == L']') { ++i_; return true; }
                return false;
            }
        }
        if (c == L'"') { v.kind = Value::Kind::Str; return string(v.s); }
        if (lit(L"true"))  { v.kind = Value::Kind::Bool; v.b = true; return true; }
        if (lit(L"false")) { v.kind = Value::Kind::Bool; v.b = false; return true; }
        if (lit(L"null"))  { v.kind = Value::Kind::Null; return true; }
        v.kind = Value::Kind::Num;
        return number(v.n);
    }
};

// The whole message, or false (malformed, too deep, or larger than `maxChars`).
inline bool parse(const std::wstring& text, Value& out, size_t maxChars = 8u << 20) {
    out = Value{};
    if (text.size() > maxChars) return false;
    Reader r(text);
    return r.parse(out);
}

}   // namespace pbjson

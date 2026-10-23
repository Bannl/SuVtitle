// =============================================================================
// SuVtitle - app/JsonW.h -- a tiny JSON writer for the bridge's outbound messages.
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/JsonW.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: nothing but this header (namespace patchbus kept).
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// WHY: the bridge grew ~40 hand-concatenated wstring emitters. They work, but every
// new one re-risks the same two bugs -- a forgotten jsonEscape on a user-controlled
// string, and a structural slip (missing comma/brace) that silently kills a message.
// This writer makes both IMPOSSIBLE by construction: every string value goes through
// the escaper, and the structure comes from the call shape.
//
// POLICY (deliberate): existing, proven emitters are converted OPPORTUNISTICALLY when
// a change already touches them -- churning 40 verified call sites in one sweep would
// trade a theoretical bug for real regression risk. Every NEW emitter uses JsonW.
//
// Shape: values are appended in call order; obj()/arr() open a scope, end() closes it.
//   JsonW j; j.obj();
//   j.kv(L"type", L"settings"); j.key(L"dirs"); j.arr();
//   for (...) j.str(dir);
//   j.end(); j.end(); postToWeb(j.take());
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace patchbus {

class JsonW {
public:
    // ---- scopes -------------------------------------------------------------
    JsonW& obj() { pre(); out_ += L'{'; stack_.push_back(true);  first_ = true; return *this; }
    JsonW& arr() { pre(); out_ += L'['; stack_.push_back(false); first_ = true; return *this; }
    JsonW& end() {
        if (!stack_.empty()) {
            out_ += stack_.back() ? L'}' : L']';
            stack_.pop_back();
        }
        first_ = false;   // the closed scope is a value in its parent
        return *this;
    }

    // ---- object members -----------------------------------------------------
    JsonW& key(const std::wstring& k) { pre(); esc(k); out_ += L':'; first_ = true; return *this; }
    JsonW& kv(const std::wstring& k, const std::wstring& v) { key(k); return str(v); }
    JsonW& kv(const std::wstring& k, const wchar_t* v)      { key(k); return str(v ? v : L""); }
    JsonW& kv(const std::wstring& k, bool v)                { key(k); return boolean(v); }
    JsonW& kv(const std::wstring& k, long long v)           { key(k); return num(v); }
    JsonW& kv(const std::wstring& k, unsigned v)            { key(k); return num((long long)v); }
    JsonW& kv(const std::wstring& k, int v)                 { key(k); return num((long long)v); }

    // ---- values (array elements, or after key()) ----------------------------
    JsonW& str(const std::wstring& v) { pre(); esc(v); return *this; }
    JsonW& num(long long v)           { pre(); out_ += std::to_wstring(v); return *this; }
    JsonW& boolean(bool v)            { pre(); out_ += v ? L"true" : L"false"; return *this; }

    // The finished message. take() moves; the writer is spent afterwards.
    std::wstring take() { return std::move(out_); }

private:
    void pre() {
        if (!first_ && !out_.empty() && out_.back() != L'{' && out_.back() != L'['
            && out_.back() != L':')
            out_ += L',';
        first_ = false;
    }
    void esc(const std::wstring& s) {
        out_ += L'"';
        for (wchar_t c : s) {
            switch (c) {
                case L'"':  out_ += L"\\\""; break;
                case L'\\': out_ += L"\\\\"; break;
                case L'\n': out_ += L"\\n";  break;
                case L'\r': out_ += L"\\r";  break;
                case L'\t': out_ += L"\\t";  break;
                default:
                    if (c < 0x20) {
                        wchar_t b[8];
                        swprintf(b, 8, L"\\u%04x", (unsigned)c);
                        out_ += b;
                    } else out_ += c;
            }
        }
        out_ += L'"';
    }

    std::wstring       out_;
    std::vector<bool>  stack_;   // true = object scope, false = array scope
    bool               first_ = true;
};

} // namespace patchbus

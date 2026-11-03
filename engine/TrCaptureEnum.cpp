// =============================================================================
// SuVtitle - engine/TrCaptureEnum.cpp   (the picker's list -- see TrCaptureEnum.h)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b. Clean room: Microsoft's documentation of IMMDeviceEnumerator,
// PKEY_Device_FriendlyName, PKEY_AudioEngine_DeviceFormat, IAudioSessionManager2 / IAudioSessionEnumerator /
// IAudioSessionControl2, the ToolHelp snapshot, VerQueryValue and RtlGetVersion.
// =============================================================================
#include <initguid.h>              // first: the PROPERTYKEYs of mmdeviceapi.h / functiondiscoverykeys_devpkey.h are
                                   // DEFINED here (selectany), not just declared
#include "TrCaptureEnum.h"

#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace pbt {
namespace cap {

namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring defaultId(IMMDeviceEnumerator* en, EDataFlow flow, ERole role) {
    IMMDevice* d = nullptr;
    std::wstring id;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, role, &d)) && d) {   // E_NOTFOUND: no device of that kind
        LPWSTR s = nullptr;
        if (SUCCEEDED(d->GetId(&s)) && s) { id = s; ::CoTaskMemFree(s); }
    }
    rel(d);
    return id;
}

// Name and shared-mode format of an endpoint, from its property store: no audio client is created, so the picker
// opens nothing (a microphone's privacy indicator never lights because a list was shown).
void endpointProps(IMMDevice* d, std::string& label, uint32_t& rate, uint32_t& ch) {
    IPropertyStore* ps = nullptr;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return;
    PROPVARIANT v;
    ::PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        label = utf8(v.pwszVal);
    ::PropVariantClear(&v);
    ::PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.pBlobData &&
        v.blob.cbSize >= 16) {
        WAVEFORMATEXTENSIBLE fx = {};
        std::memcpy(&fx, v.blob.pBlobData, std::min<size_t>(v.blob.cbSize, sizeof(fx)));
        rate = fx.Format.nSamplesPerSec;
        ch = fx.Format.nChannels;
    }
    ::PropVariantClear(&v);
    rel(ps);
}

// The processes that have a (not expired, not "system sounds") audio session on this render endpoint.
void sessionPids(IMMDevice* d, std::vector<uint32_t>& pids) {
    IAudioSessionManager2* sm = nullptr;
    IAudioSessionEnumerator* se = nullptr;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sm))) && sm &&
        SUCCEEDED(sm->GetSessionEnumerator(&se)) && se) {
        int n = 0;
        if (SUCCEEDED(se->GetCount(&n))) {
            for (int i = 0; i < n; ++i) {
                IAudioSessionControl* c = nullptr;
                IAudioSessionControl2* c2 = nullptr;
                if (SUCCEEDED(se->GetSession(i, &c)) && c &&
                    SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&c2))) && c2) {
                    AudioSessionState st = AudioSessionStateInactive;
                    DWORD pid = 0;
                    const bool system = c2->IsSystemSoundsSession() == S_OK;
                    const bool expired = SUCCEEDED(c->GetState(&st)) && st == AudioSessionStateExpired;
                    // AUDCLNT_S_NO_SINGLE_PROCESS is a SUCCESS code: the pid is then the session's creator, good enough.
                    if (!system && !expired && SUCCEEDED(c2->GetProcessId(&pid)) && pid != 0) pids.push_back(pid);
                }
                rel(c2);
                rel(c);
            }
        }
    }
    rel(se);
    rel(sm);
}

std::string asciiLower(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}

int kindRank(const std::string& k) { return k == "in" ? 0 : k == "out" ? 1 : 2; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------
uint32_t windowsBuild() {
    typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    static uint32_t build = []() -> uint32_t {
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        if (!nt) return 0;
        const RtlGetVersionFn fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
        if (!fn) return 0;
        RTL_OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        return fn(&vi) == 0 ? (uint32_t)vi.dwBuildNumber : 0u;
    }();
    return build;
}

bool processLoopbackSupported() { return windowsBuild() >= 20348; }

std::wstring lowerW(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return o;
}

bool isShellProcess(const std::wstring& e) {
    static const wchar_t* const shells[] = {
        L"system", L"[system process]", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"svchost.exe", L"explorer.exe", L"sihost.exe", L"runtimebroker.exe", L"userinit.exe",
        L"dwm.exe", L"taskhostw.exe", L"applicationframehost.exe", L"shellexperiencehost.exe",
        L"startmenuexperiencehost.exe", L"searchhost.exe", L"textinputhost.exe", L"lockapp.exe",
        // a console or a terminal: "python play.py" started from one is python, not the terminal
        L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"windowsterminal.exe", L"openconsole.exe", L"conhost.exe",
    };
    for (const wchar_t* s : shells) if (e == s) return true;
    return false;
}

std::wstring appLabel(uint32_t pid, const std::wstring& exe) {
    std::wstring label;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[1024];
        DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
        if (::QueryFullProcessImageNameW(h, 0, path, &n)) {
            DWORD dummy = 0;
            const DWORD sz = ::GetFileVersionInfoSizeW(path, &dummy);
            if (sz) {
                std::vector<unsigned char> blk(sz);
                if (::GetFileVersionInfoW(path, 0, sz, blk.data())) {
                    struct LangCp { WORD lang; WORD cp; };
                    LangCp* tr = nullptr;
                    UINT cb = 0;
                    std::vector<LangCp> tries;
                    if (::VerQueryValueW(blk.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &cb) && tr)
                        for (UINT i = 0; i < cb / sizeof(LangCp); ++i) tries.push_back(tr[i]);
                    tries.push_back({ 0x0409, 0x04b0 });
                    tries.push_back({ 0x0409, 0x04e4 });
                    for (const LangCp& t : tries) {
                        wchar_t sub[64];
                        std::swprintf(sub, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", t.lang, t.cp);
                        wchar_t* p = nullptr;
                        UINT len = 0;
                        if (::VerQueryValueW(blk.data(), sub, reinterpret_cast<void**>(&p), &len) && p && len > 1) {
                            label.assign(p, wcsnlen(p, len));
                            while (!label.empty() && (label.back() == L' ' || label.back() == L'\t')) label.pop_back();
                            while (!label.empty() && (label.front() == L' ' || label.front() == L'\t')) label.erase(0, 1);
                            if (!label.empty()) break;
                        }
                    }
                }
            }
        }
        ::CloseHandle(h);
    }
    return label.empty() ? exe : label;
}

// ---- ProcessTable -----------------------------------------------------------------------------------------------
ProcessTable::ProcessTable() {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
        Proc p;
        p.pid = pe.th32ProcessID;
        p.parent = pe.th32ParentProcessID;
        p.exe = pe.szExeFile;
        procs_[p.pid] = p;
    }
    ::CloseHandle(snap);
}

ProcessTable::ProcessTable(const std::vector<Proc>& procs, const std::map<uint32_t, uint64_t>& created)
    : created_(created), synthetic_(true) {
    for (const Proc& p : procs) procs_[p.pid] = p;
}

const ProcessTable::Proc* ProcessTable::find(uint32_t pid) const {
    const auto it = procs_.find(pid);
    return it == procs_.end() ? nullptr : &it->second;
}

uint64_t ProcessTable::created(uint32_t pid) const {
    const auto it = created_.find(pid);
    if (it != created_.end()) return it->second;
    if (synthetic_) return 0;
    uint64_t t = 0;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        FILETIME c, e, k, u;
        if (::GetProcessTimes(h, &c, &e, &k, &u)) t = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
        ::CloseHandle(h);
    }
    created_[pid] = t;
    return t;
}

bool isWebView2Process(const std::wstring& exeLower) { return exeLower == L"msedgewebview2.exe"; }

// RVA-4: a parent link is believed only when it is PROVED -- both creation times readable, the parent the older. An
// unreadable time (another user's process, a protected one: about half of a PC's processes for an unelevated engine)
// cannot tell a real parent from a dead one's recycled pid, so it is no link at all.
bool ProcessTable::linked(uint32_t child, uint32_t parent) const {
    const uint64_t cc = created(child), pc = created(parent);
    return cc && pc && pc <= cc;
}

uint32_t ProcessTable::rootOf(uint32_t pid) const {
    uint32_t cur = pid;
    for (int step = 0; step < 64; ++step) {
        const Proc* p = find(cur);
        if (!p) return cur;
        const uint32_t par = p->parent;
        if (par == 0 || par == cur) return cur;
        const Proc* pp = find(par);
        if (!pp) return cur;                                       // the parent is gone: an orphan is its own root
        if (!linked(cur, par)) return cur;                         // RVA-4: not provably its parent
        const std::wstring ce = lowerW(p->exe), pe = lowerW(pp->exe);
        // RVA-2: the SAME exe is the same application; a WebView2 process belongs to its host; anything else launched it
        if (ce != pe && !isWebView2Process(ce)) return cur;
        cur = par;
    }
    return cur;
}

bool ProcessTable::inTreeOf(uint32_t pid, uint32_t ancestor) const {
    uint32_t cur = pid;
    for (int step = 0; step < 64; ++step) {
        if (cur == ancestor) return true;
        const Proc* p = find(cur);
        if (!p) return false;
        const uint32_t par = p->parent;
        if (par == 0 || par == cur || !find(par)) return false;
        if (!linked(cur, par)) return false;                       // RVA-4
        cur = par;
    }
    return false;
}

std::vector<uint32_t> ProcessTable::rootsNamed(const std::wstring& exeLower, const std::vector<uint32_t>& hidden) const {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto& kv : procs_) {
        if (lowerW(kv.second.exe) != exeLower || rootOf(kv.first) != kv.first) continue;
        bool hide = false;
        for (uint32_t h : hidden) if (h && inTreeOf(kv.first, h)) hide = true;
        if (hide) continue;
        const uint64_t c = created(kv.first);
        v.push_back({ c ? c : ~0ull, kv.first });
    }
    std::sort(v.begin(), v.end());
    std::vector<uint32_t> out;
    for (const auto& x : v) out.push_back(x.second);
    return out;
}

// ---- the list ---------------------------------------------------------------------------------------------------
std::vector<SourceInfo> enumerateSources(IMMDeviceEnumerator* en, const std::vector<uint32_t>& hiddenTrees,
                                         std::string* note) {
    std::vector<SourceInfo> list;
    if (!en) return list;
    const std::wstring defIn = defaultId(en, eCapture, eConsole), comIn = defaultId(en, eCapture, eCommunications);
    const std::wstring defOut = defaultId(en, eRender, eConsole), comOut = defaultId(en, eRender, eCommunications);
    std::vector<uint32_t> pids;
    const EDataFlow flows[2] = { eCapture, eRender };
    for (EDataFlow flow : flows) {
        IMMDeviceCollection* col = nullptr;
        UINT n = 0;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col || FAILED(col->GetCount(&n))) {
            rel(col);
            if (note) *note += std::string("the ") + (flow == eCapture ? "input" : "output") + " list was refused; ";
            continue;
        }
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            LPWSTR id = nullptr;
            if (FAILED(col->Item(i, &d)) || !d || FAILED(d->GetId(&id)) || !id) { rel(d); continue; }
            const std::wstring wid = id;
            ::CoTaskMemFree(id);
            SourceInfo s;
            s.kind = flow == eCapture ? "in" : "out";
            s.key = s.kind + ":" + utf8(wid);
            s.state = "ok";
            endpointProps(d, s.label, s.rate, s.ch);
            if (s.label.empty()) s.label = s.kind == "in" ? "Input" : "Output";
            const bool isDef = wid == (flow == eCapture ? defIn : defOut);
            const bool isCom = wid == (flow == eCapture ? comIn : comOut);
            s.flags = isDef && isCom ? "default,comm" : isDef ? "default" : isCom ? "comm" : "";
            list.push_back(s);
            if (flow == eRender) sessionPids(d, pids);
            rel(d);
        }
        rel(col);
    }

    if (!pids.empty()) {
        const ProcessTable pt;
        const bool supported = processLoopbackSupported();
        std::vector<std::wstring> seen;
        for (uint32_t pid : pids) {
            bool hide = false;
            for (uint32_t h : hiddenTrees) if (h && pt.inTreeOf(pid, h)) hide = true;
            if (hide) continue;
            const uint32_t root = pt.rootOf(pid);
            const ProcessTable::Proc* rp = pt.find(root);
            if (!rp || rp->exe.empty()) continue;
            const std::wstring exeL = lowerW(rp->exe);
            if (isShellProcess(exeL) || std::find(seen.begin(), seen.end(), exeL) != seen.end()) continue;
            seen.push_back(exeL);
            SourceInfo s;
            s.kind = "app";
            s.key = "app:" + utf8(exeL);
            s.label = utf8(appLabel(root, rp->exe));
            s.state = supported ? "ok" : "unsupported";
            s.rate = 48000;                 // what the capture asks Windows for (TrCaptureWasapi.cpp)
            s.ch = 2;
            list.push_back(s);
        }
    }

    std::stable_sort(list.begin(), list.end(), [](const SourceInfo& a, const SourceInfo& b) {
        const int ka = kindRank(a.kind), kb = kindRank(b.kind);
        if (ka != kb) return ka < kb;
        const std::string la = asciiLower(a.label), lb = asciiLower(b.label);
        if (la != lb) return la < lb;
        return a.key < b.key;
    });
    return list;
}

} // namespace cap
} // namespace pbt

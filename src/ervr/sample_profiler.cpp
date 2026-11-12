#include "sample_profiler.h"
#include "frame_probe.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ervr {
namespace {

constexpr int kMaxDepth = 48;
constexpr int kMaxHot = 24;
constexpr double kReportSeconds = 45.0;

std::atomic<bool> g_started{false};

double NowSec() {
    static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (double)x.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f;
}

// The suspended thread's committed stack: [lo, hi). Every unwind step stays inside it, so the
// unwinder never faults while a game thread is held (a fault would run the crash reporter's
// logging with that thread - maybe holding the log lock - suspended).
bool StackRange(uint64_t rsp, uint64_t& lo, uint64_t& hi) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)rsp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    lo = rsp;
    hi = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    return hi > lo;
}

// SEH-isolated, POD only. Holds the thread for the capture and the unwind, nothing else: no
// allocation, no locks of ours, no logging between SuspendThread and ResumeThread.
int CaptureStack(HANDLE h, uint64_t* out, int cap, int* inKernel) {
    if (SuspendThread(h) == (DWORD)-1) return -1;
    int n = 0;
    alignas(16) CONTEXT c;
    c.ContextFlags = CONTEXT_FULL;
    *inKernel = 0;
    if (GetThreadContext(h, &c)) {
        uint64_t lo = 0, hi = 0;
        if (StackRange(c.Rsp, lo, hi)) {
            __try {
                // Blocked in a system call: the instruction before rip is `syscall` (0F 05).
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery((void*)(c.Rip - 2), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                    const uint8_t* ip = (const uint8_t*)c.Rip;
                    if (ip[-2] == 0x0F && ip[-1] == 0x05) *inKernel = 1;
                }
                while (n < cap && c.Rip >= 0x10000) {
                    out[n++] = c.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                    if (!fn) {
                        if (n > 1) break;   // only the top frame may be a leaf; below that, unknown code
                        if (c.Rsp < lo || c.Rsp + 8 > hi) break;
                        c.Rip = *(DWORD64*)c.Rsp;
                        c.Rsp += 8;
                    } else {
                        if (c.Rsp < lo || c.Rsp + 0x400 > hi) break;   // frame reads stay on the stack
                        void* hd = nullptr; DWORD64 est = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &hd, &est, nullptr);
                    }
                    if (c.Rsp < lo || c.Rsp >= hi) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    ResumeThread(h);
    return n;
}

struct Hot { DWORD tid; HANDLE h; std::string name; bool frame; };

std::string ThreadName(HANDLE h) {
    using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!fn) return {};
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(fn(h, &w)) && w) {
        char buf[96] = {};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        out = buf;
        LocalFree(w);
    }
    return out;
}

// Function identity: module base + the function's first byte (leaf frames: the pc itself).
uint64_t FunctionOf(uint64_t pc) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, nullptr);
    return fn ? base + fn->BeginAddress : pc;
}

std::string Name(uint64_t addr) {
    static std::unordered_map<uint64_t, std::string> s_mod;   // module base -> short name
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m) {
        char b[32]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)addr); return b;
    }
    const uint64_t base = (uint64_t)m;
    auto it = s_mod.find(base);
    if (it == s_mod.end()) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* slash = wcsrchr(path, L'\\');
        char nm[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, nm, sizeof(nm) - 1, nullptr, nullptr);
        std::string s = nm;
        if (m == GetModuleHandleW(nullptr)) s = "exe";
        else if (auto dot = s.rfind('.'); dot != std::string::npos) s.resize(dot);
        it = s_mod.emplace(base, s).first;
    }
    char b[96]; snprintf(b, sizeof(b), "%s+0x%llx", it->second.c_str(), (unsigned long long)(addr - base));
    return b;
}

std::vector<Hot> FindHotThreads() {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<Hot> all;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (h) all.push_back({te.th32ThreadID, h, {}, false});
    }
    CloseHandle(snap);
    // 3 s of 200 Hz top-of-stack samples: which threads are actually running (not parked in a system call).
    std::vector<int> busy(all.size(), 0);
    uint64_t pc[2]; int kern = 0;
    const double t0 = NowSec();
    int rounds = 0;
    while (NowSec() - t0 < 3.0) {
        for (size_t i = 0; i < all.size(); ++i)
            if (CaptureStack(all[i].h, pc, 1, &kern) > 0 && !kern) ++busy[i];
        ++rounds;
        Sleep(5);
    }
    const DWORD frameTid = FramePumpFrameThread();
    std::vector<size_t> order(all.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return busy[a] > busy[b]; });
    std::vector<Hot> hot;
    std::vector<bool> keep(all.size(), false);
    for (size_t k = 0; k < order.size(); ++k) {
        const size_t i = order[k];
        const bool isFrame = all[i].tid == frameTid;
        if (isFrame || ((int)hot.size() < kMaxHot - 1 && busy[i] * 50 >= rounds)) {   // >= 2% running
            Hot x = all[i];
            x.frame = isFrame;
            x.name = ThreadName(x.h);
            hot.push_back(x);
            keep[i] = true;
        }
    }
    for (size_t i = 0; i < all.size(); ++i) if (!keep[i]) CloseHandle(all[i].h);
    std::string line;
    for (size_t k = 0; k < hot.size(); ++k) {
        char b[160];
        size_t idx = 0;
        for (size_t i = 0; i < all.size(); ++i) if (all[i].tid == hot[k].tid) idx = i;
        snprintf(b, sizeof(b), "%s T%zu tid %lu%s%s%s %d%%", k ? "," : "", k, hot[k].tid, hot[k].frame ? " (frame)" : "",
                 hot[k].name.empty() ? "" : " ", hot[k].name.c_str(), rounds ? busy[idx] * 100 / rounds : 0);
        line += b;
    }
    LOGI("[sample] %zu threads, %zu sampled (running >= 2%% of a 3 s look, plus the frame thread):%s", all.size(),
         hot.size(), line.c_str());
    return hot;
}

// Thread kinds the report keeps apart: the frame thread (queue_tasks, both passes), the renderer's
// GX threads (GXWorker:n, GXRenderThread), everything else.
enum Kind { kFrame = 0, kGx = 1, kOther = 2, kKinds = 3 };
const char* kKindName[kKinds] = {"frame thread", "GX threads", "other threads"};

// Call tree, root (thread start) to leaf: key = the function addresses from the root down.
using Tree = std::unordered_map<std::string, double>;

struct Agg {
    std::unordered_map<uint64_t, double> self[3][kKinds];   // ms of running thread time, by pass bucket
    Tree tree[3][kKinds];                                    // running samples
    Tree waitTree[3];                                        // the frame thread's system-call samples
    std::vector<double> busy[3], wait[3];                    // per hot thread
    double t0 = 0;
    long seq0[2] = {};
};

void AddTree(Tree& t, const uint64_t* s, int depth, double dtMs) {
    std::string key;
    key.reserve((size_t)depth * 8);
    for (int d = depth - 1; d >= 0; --d) {   // root first
        const uint64_t f = FunctionOf(s[d]);
        key.append((const char*)&f, 8);
        t[key] += dtMs;
    }
}

// Depth-first, heaviest child first; nodes under `minMs` per pass are left out. Chains of
// single children (thread start, dispatch trampolines) print on, so indentation stays true.
void PrintTree(const Tree& t, double passes, double minMs, const char* title, int maxLines) {
    std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> kids;
    for (auto& [k, v] : t) {
        if (v / passes < minMs) continue;
        kids[k.substr(0, k.size() - 8)].push_back({k, v});
    }
    for (auto& [k, v] : kids)
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
    LOGI("[tree] %s (ms per pass, >= %.2f):", title, minMs);
    int lines = 0;
    std::vector<std::pair<std::string, int>> stack;
    for (auto it = kids[""].rbegin(); it != kids[""].rend(); ++it) stack.push_back({it->first, 0});
    while (!stack.empty() && lines < maxLines) {
        auto [k, depth] = stack.back();
        stack.pop_back();
        uint64_t f; memcpy(&f, k.data() + k.size() - 8, 8);
        const double ms = t.at(k) / passes;
        char b[192];
        snprintf(b, sizeof(b), "[tree] %*s%s %.2f", depth * 2 > 60 ? 60 : depth * 2, "", Name(f).c_str(), ms);
        LOGI("%s", b);
        ++lines;
        auto kit = kids.find(k);
        if (kit != kids.end())
            for (auto c = kit->second.rbegin(); c != kit->second.rend(); ++c) stack.push_back({c->first, depth + 1});
    }
}

void Report(Agg& a, const std::vector<Hot>& hot) {
    const long p1 = FramePumpPassSeq(1) - a.seq0[0], p2 = FramePumpPassSeq(2) - a.seq0[1];
    const long passes[3] = {p1, p1, p2};   // "between passes" per pass-1 frame
    const char* label[3] = {"between passes", "pass 1", "pass 2"};
    std::string th;
    for (size_t k = 0; k < hot.size(); ++k) {
        char b[96];
        snprintf(b, sizeof(b), "%s T%zu %.2f/%.2f (wait %.2f/%.2f)", k ? "," : "", k,
                 p1 ? a.busy[1][k] / p1 : 0.0, p2 ? a.busy[2][k] / p2 : 0.0, p1 ? a.wait[1][k] / p1 : 0.0,
                 p2 ? a.wait[2][k] / p2 : 0.0);
        th += b;
    }
    LOGI("[sample] %.0f s: %ld pass-1 and %ld pass-2 frames; ms per pass running (in a system call) p1/p2:%s",
         NowSec() - a.t0, p1, p2, th.c_str());
    for (int p : {2, 1}) {
        if (passes[p] <= 0) continue;
        for (int c = 0; c < kKinds; ++c) {
            std::vector<std::pair<uint64_t, double>> v(a.self[p][c].begin(), a.self[p][c].end());
            std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
            std::string line;
            for (size_t i = 0; i < v.size() && i < 25; ++i) {
                const double ms = v[i].second / passes[p];
                if (ms < 0.03) break;
                char b[128];
                snprintf(b, sizeof(b), ", %s %.2f", Name(v[i].first).c_str(), ms);
                line += b;
            }
            LOGI("[sample] %s, %s SELF (running), ms per pass%s", label[p], kKindName[c], line.c_str());
        }
        char title[96];
        snprintf(title, sizeof(title), "%s, frame thread running", label[p]);
        PrintTree(a.tree[p][kFrame], (double)passes[p], 0.08, title, 160);
        snprintf(title, sizeof(title), "%s, frame thread in a system call", label[p]);
        PrintTree(a.waitTree[p], (double)passes[p], 0.15, title, 50);
        snprintf(title, sizeof(title), "%s, GX threads running (summed)", label[p]);
        PrintTree(a.tree[p][kGx], (double)passes[p], 0.2, title, 80);
    }
}

void Run() {
    // Wait for stereo to be running in the world: the threads worth sampling are busy then.
    while (FramePumpPassSeq(2) < 300) Sleep(500);
    std::vector<Hot> hot = FindHotThreads();
    if (hot.empty()) { LOGW("[sample] no threads to sample"); return; }
    Agg a;
    for (int p = 0; p < 3; ++p) { a.busy[p].assign(hot.size(), 0.0); a.wait[p].assign(hot.size(), 0.0); }
    a.t0 = NowSec(); a.seq0[0] = FramePumpPassSeq(1); a.seq0[1] = FramePumpPassSeq(2);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    std::vector<uint64_t> stacks(hot.size() * kMaxDepth);
    std::vector<int> depth(hot.size()), kern(hot.size());
    std::vector<int> kind(hot.size());
    for (size_t k = 0; k < hot.size(); ++k)
        kind[k] = hot[k].frame ? kFrame : hot[k].name.rfind("GX", 0) == 0 ? kGx : kOther;
    double last = NowSec();
    while (Cfg().profileSample) {
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -10000;   // 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 5);
        } else {
            Sleep(1);
        }
        const double now = NowSec();
        const double dtMs = std::min((now - last) * 1000.0, 5.0);
        last = now;
        const int pass = FramePumpProfilePass();
        const int bucket = pass == 1 ? 1 : pass == 2 ? 2 : 0;
        for (size_t k = 0; k < hot.size(); ++k)
            depth[k] = CaptureStack(hot[k].h, &stacks[k * kMaxDepth], kMaxDepth, &kern[k]);
        // Everything below runs with every thread running again.
        for (size_t k = 0; k < hot.size(); ++k) {
            if (depth[k] <= 0) continue;
            const uint64_t* s = &stacks[k * kMaxDepth];
            if (kern[k]) {
                a.wait[bucket][k] += dtMs;
                if (kind[k] == kFrame && bucket) AddTree(a.waitTree[bucket], s, depth[k], dtMs);
                continue;
            }
            a.busy[bucket][k] += dtMs;
            a.self[bucket][kind[k]][FunctionOf(s[0])] += dtMs;
            if (bucket && kind[k] != kOther) AddTree(a.tree[bucket][kind[k]], s, depth[k], dtMs);
        }
        if (now - a.t0 >= kReportSeconds) {
            Report(a, hot);
            for (int p = 0; p < 3; ++p) {
                for (int c = 0; c < kKinds; ++c) { a.self[p][c].clear(); a.tree[p][c].clear(); }
                a.waitTree[p].clear();
                std::fill(a.busy[p].begin(), a.busy[p].end(), 0.0);
                std::fill(a.wait[p].begin(), a.wait[p].end(), 0.0);
            }
            a.t0 = NowSec(); a.seq0[0] = FramePumpPassSeq(1); a.seq0[1] = FramePumpPassSeq(2);
            last = NowSec();
        }
    }
    if (timer) CloseHandle(timer);
    for (auto& x : hot) CloseHandle(x.h);
}

}  // namespace

void SampleProfilerStart() {
    if (!Cfg().profileSample || g_started.exchange(true)) return;
    LOGI("[sample] sampling profiler armed: starts once stereo pairs run in the world");
    std::thread(Run).detach();
}

}  // namespace ervr

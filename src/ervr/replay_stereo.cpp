#include "replay_stereo.h"
#include "config.h"
#include "game_er.h"
#include "stereo.h"
#include "log.h"
#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <immintrin.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ervr {
namespace {

long long Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }

// StereoMode=replay is the product (level 3: shadows, held signals, the GPU camera patch); the
// [Experimental] ReplayStereo levels are the bench tests that led to it (1 replay as-is, 2 CPU patch, 3 GPU patch).
int ReplayLevel() {
    static const int s_level = Cfg().vrStereoMode == "replay" ? 3 : Cfg().replayStereo;
    return s_level;
}
bool ReplayIsStereoMode() {
    static const bool s_on = Cfg().vrStereoMode == "replay";
    return s_on;
}

std::string ThreadName(DWORD tid) {
    std::string name;
    if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
        using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
        PWSTR w = nullptr;
        if (fn && SUCCEEDED(fn(h, &w)) && w) {
            char b[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b) - 1, nullptr, nullptr);
            name = b;
            LocalFree(w);
        }
        CloseHandle(h);
    }
    return name;
}

std::string ThreadLabel(DWORD tid) {
    static std::map<DWORD, std::string> s_names;
    auto it = s_names.find(tid);
    if (it != s_names.end()) return it->second;
    const std::string name = ThreadName(tid);
    char b[96];
    snprintf(b, sizeof(b), "%lu%s%s", tid, name.empty() ? "" : " ", name.c_str());
    return s_names.emplace(tid, b).first->second;
}

// ---- CPU-visible buffers, from creation: GPU virtual address -> resource ---------------------------------
// (ReShade's map events need its full-add-on build; creation events do not. A buffer's CPU pointer is
// taken by mapping it ourselves when it is needed - persistent maps return the same pointer.)
struct CpuBuffer { uint64_t gpu = 0, size = 0; ID3D12Resource* res = nullptr; uint8_t* cpu = nullptr; };
SRWLOCK g_bufLock = SRWLOCK_INIT;
std::vector<CpuBuffer> g_bufs;   // sorted by gpu

const CpuBuffer* FindBufferLocked(uint64_t va) {
    auto it = std::upper_bound(g_bufs.begin(), g_bufs.end(), va,
                               [](uint64_t v, const CpuBuffer& b) { return v < b.gpu; });
    if (it == g_bufs.begin()) return nullptr;
    --it;
    return va < it->gpu + it->size ? &*it : nullptr;
}

// CPU pointer for a GPU address inside a tracked buffer, `need` bytes readable; null if untracked.
uint8_t* CpuOf(uint64_t va, uint64_t need, uint64_t* avail = nullptr) {
    AcquireSRWLockExclusive(&g_bufLock);
    CpuBuffer* b = const_cast<CpuBuffer*>(FindBufferLocked(va));
    uint8_t* out = nullptr;
    if (b) {
        if (!b->cpu) {
            void* p = nullptr;
            const D3D12_RANGE none{0, 0};
            if (SUCCEEDED(b->res->Map(0, &none, &p)) && p) b->cpu = (uint8_t*)p;   // stays mapped (ref-counted)
        }
        const uint64_t off = va - b->gpu;
        if (b->cpu && off + need <= b->size) {
            out = b->cpu + off;
            if (avail) *avail = b->size - off;
        }
    }
    ReleaseSRWLockExclusive(&g_bufLock);
    return out;
}

// ---- descriptor tracking: which descriptor slot holds which constant buffer ------------------------------
// Descriptors are opaque driver data - a CBV's buffer address cannot be read back from a heap - so every
// CreateConstantBufferView is remembered by destination CPU handle and followed through CopyDescriptors(Simple).
// SRV/UAV creation into a slot forgets it. Device hooks go in at device creation, before the game's first view.
struct CbvInfo { uint64_t va; uint32_t size; };
std::mutex g_descMtx;
std::unordered_map<uint64_t, CbvInfo> g_cbvAt;   // CPU descriptor handle -> CBV
UINT g_descInc = 0;                                // CBV/SRV/UAV handle increment
// Pass log (bench): shader and unordered views by slot too - which textures a table brings in.
int PassLogSeconds();
struct ViewInfo { const void* res; uint32_t fmt; uint16_t dim; char kind; };   // res: identity only, never dereferenced
std::unordered_map<uint64_t, ViewInfo> g_viewAt;   // under g_descMtx; filled only while ERVR_PASS_LOG is set
std::atomic<long> g_cbvCreates{0}, g_srvUavCreates{0}, g_copiesSimple{0}, g_copiesMulti{0}, g_descCopied{0};

using PfnCreateCbv = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnCreateSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnCreateUav = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnCopyDescs = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PfnCopySimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
PfnCreateCbv  g_origCreateCbv = nullptr;
PfnCreateSrv  g_origCreateSrv = nullptr;
PfnCreateUav  g_origCreateUav = nullptr;
PfnCopyDescs  g_origCopyDescs = nullptr;
PfnCopySimple g_origCopySimple = nullptr;

// Per-frame list of the constant-buffer views the game creates (census: where the camera lives, frame to frame).
struct FrameCbv { uint64_t va; uint32_t size; uint64_t handle; DWORD tid; };
constexpr int kFrameCbv = 65536;
FrameCbv g_frameCbv[kFrameCbv];
std::atomic<int> g_frameCbvN{0};
// One-shot creation trace ([Experimental] ReplayMatrixCensus arms it with the census): every constant view, game
// submission and present, with QPC times, for a few frames - written to cbv-trace.csv next to the ini.
struct TraceEv { long long t; uint64_t va; uint32_t size; DWORD tid; char kind; };
constexpr int kTrace = 200000;
TraceEv g_trace[kTrace];
std::atomic<int> g_traceN{0};
std::atomic<bool> g_traceOn{false};
inline void TraceNote(char kind, uint64_t va, uint32_t size) {
    if (!g_traceOn.load(std::memory_order_relaxed)) return;
    const int i = g_traceN.fetch_add(1, std::memory_order_relaxed);
    if (i < kTrace) { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_trace[i] = {q.QuadPart, va, size, GetCurrentThreadId(), kind}; }
}
// Where the frame's last submission fell in the creation order: views made after it are the NEXT frame's - the engine
// writes the next frame's first blocks (its lighting and scene blocks) before this frame's present (2026-11-13 ring
// scan: a whole 2048 B lighting block of the frame being replayed had its view in the previous present's window).
std::atomic<int> g_cbvAtLastExec{0};
std::atomic<bool> g_frameCbvArmed{false};

void STDMETHODCALLTYPE HkCreateCbv(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_origCreateCbv(d, desc, h);
    g_cbvCreates.fetch_add(1, std::memory_order_relaxed);
    if (desc) TraceNote('C', desc->BufferLocation, desc->SizeInBytes);
    if (desc && g_frameCbvArmed.load(std::memory_order_relaxed)) {
        const int i = g_frameCbvN.fetch_add(1, std::memory_order_relaxed);
        if (i < kFrameCbv) g_frameCbv[i] = {desc->BufferLocation, desc->SizeInBytes, h.ptr, GetCurrentThreadId()};
    }
    if (!Cfg().replayCensus) return;   // the slot map is census-only: ~16k views a frame through one lock is ms
    std::lock_guard<std::mutex> g(g_descMtx);
    if (desc && desc->BufferLocation) g_cbvAt[h.ptr] = {desc->BufferLocation, desc->SizeInBytes};
    else g_cbvAt.erase(h.ptr);
    g_viewAt.erase(h.ptr);
}
void STDMETHODCALLTYPE HkCreateSrv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_origCreateSrv(d, r, desc, h);
    g_srvUavCreates.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(g_descMtx);
    g_cbvAt.erase(h.ptr);
    if (PassLogSeconds() > 0) g_viewAt[h.ptr] = {r, desc ? (uint32_t)desc->Format : 0u, desc ? (uint16_t)desc->ViewDimension : (uint16_t)0, 's'};
}
void STDMETHODCALLTYPE HkCreateUav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_origCreateUav(d, r, c, desc, h);
    g_srvUavCreates.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(g_descMtx);
    g_cbvAt.erase(h.ptr);
    if (PassLogSeconds() > 0) g_viewAt[h.ptr] = {r, desc ? (uint32_t)desc->Format : 0u, desc ? (uint16_t)desc->ViewDimension : (uint16_t)0, 'u'};
}
// Caller holds g_descMtx.
void CopyRangeLocked(uint64_t dst, uint64_t src, UINT n) {
    const bool views = PassLogSeconds() > 0;
    for (UINT i = 0; i < n; ++i) {
        auto it = g_cbvAt.find(src + (uint64_t)i * g_descInc);
        if (it != g_cbvAt.end()) g_cbvAt[dst + (uint64_t)i * g_descInc] = it->second;
        else g_cbvAt.erase(dst + (uint64_t)i * g_descInc);
        if (views) {
            auto v = g_viewAt.find(src + (uint64_t)i * g_descInc);
            if (v != g_viewAt.end()) g_viewAt[dst + (uint64_t)i * g_descInc] = v->second;
            else g_viewAt.erase(dst + (uint64_t)i * g_descInc);
        }
    }
}
void STDMETHODCALLTYPE HkCopySimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    g_origCopySimple(d, n, dst, src, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_descInc) return;
    g_copiesSimple.fetch_add(1, std::memory_order_relaxed);
    g_descCopied.fetch_add(n, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(g_descMtx);
    CopyRangeLocked(dst.ptr, src.ptr, n);
}
void STDMETHODCALLTYPE HkCopyDescs(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dsts, const UINT* dsz, UINT ns,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* srcs, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    g_origCopyDescs(d, nd, dsts, dsz, ns, srcs, ssz, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_descInc || !dsts || !srcs) return;
    g_copiesMulti.fetch_add(1, std::memory_order_relaxed);
    // Walk both range lists in step: descriptor k of the flattened source goes to descriptor k of the destination.
    std::lock_guard<std::mutex> g(g_descMtx);
    UINT di = 0, dOff = 0, si = 0, sOff = 0;
    while (di < nd && si < ns) {
        const UINT dLen = dsz ? dsz[di] : 1, sLen = ssz ? ssz[si] : 1;
        const UINT run = std::min(dLen - dOff, sLen - sOff);
        CopyRangeLocked(dsts[di].ptr + (uint64_t)dOff * g_descInc, srcs[si].ptr + (uint64_t)sOff * g_descInc, run);
        g_descCopied.fetch_add(run, std::memory_order_relaxed);
        dOff += run; sOff += run;
        if (dOff == dLen) { ++di; dOff = 0; }
        if (sOff == sLen) { ++si; sOff = 0; }
    }
}

// ---- native D3D12 queue hooks (underneath ReShade's proxies: every real submission) ---------------------
using PfnExec   = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PfnSignal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using PfnWait   = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using PfnFenceEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
using PfnAllocReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);

PfnExec       g_origExec = nullptr;
PfnSignal     g_origSignal = nullptr;
PfnWait       g_origWait = nullptr;
PfnFenceEvent g_origFenceEvent = nullptr;
PfnAllocReset g_origAllocReset = nullptr;
std::atomic<bool> g_fenceHooked{false}, g_listHooked{false}, g_allocHooked{false};

enum EvKind : uint8_t { EvExec, EvSignal, EvWait, EvCpuWait, EvListReset, EvAllocReset, EvPresent };
struct Ev {
    long long t;
    void* a;
    void* b;
    uint64_t v;
    void* lists[3];
    DWORD tid;
    uint8_t kind, qtype, ervr;
};
constexpr int kEv = 6000;
Ev g_ev[kEv];
std::atomic<int> g_evN{0};
std::atomic<bool> g_armed{false};       // queue census window
thread_local bool t_inErvrPresent = false;

uint8_t QueueType(ID3D12CommandQueue* q) {
    static std::map<ID3D12CommandQueue*, uint8_t> s_types;
    static SRWLOCK s_lock = SRWLOCK_INIT;
    AcquireSRWLockShared(&s_lock);
    auto it = s_types.find(q);
    const bool have = it != s_types.end();
    const uint8_t t = have ? it->second : 0;
    ReleaseSRWLockShared(&s_lock);
    if (have) return t;
    const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    AcquireSRWLockExclusive(&s_lock);
    s_types[q] = (uint8_t)d.Type;
    ReleaseSRWLockExclusive(&s_lock);
    return (uint8_t)d.Type;
}

Ev* Rec(uint8_t kind) {
    if (!g_armed.load(std::memory_order_relaxed)) return nullptr;
    const int i = g_evN.fetch_add(1, std::memory_order_relaxed);
    if (i >= kEv) return nullptr;
    Ev* e = &g_ev[i];
    *e = Ev{};
    e->t = Qpc();
    e->tid = GetCurrentThreadId();
    e->kind = kind;
    e->ervr = t_inErvrPresent ? 1 : 0;
    return e;
}

bool HookAddr(void* target, void* detour, void** orig, const char* what, bool quiet = false) {
    const MH_STATUS c = MH_CreateHook(target, detour, orig);
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    if (e != MH_OK) { LOGW("[replay] hooking %s at %p failed (%d)", what, target, (int)e); return false; }
    if (!quiet) LOGI("[replay] %s hooked at %p", what, target);
    return true;
}
bool HookSlot(void* obj, int slot, void* detour, void** orig, const char* what) {
    return HookAddr((*(void***)obj)[slot], detour, orig, what);
}

void HookFenceVt(ID3D12Fence* f);
void HookListVt(ID3D12CommandList* l);

void ShadowSubmitted(UINT n, ID3D12CommandList* const* lists);
std::string ThreadLabel(DWORD tid);
const char* QName(uint8_t t);

// The queue the game presents on: the only one whose submissions make up the frame the replay redraws, and the only
// one whose fence signals are held. The queue hooks are code hooks, so they see EVERY queue on the device - and the
// game's streaming threads (LoadProcess_M*) submit on queues of their own while the player moves: counted as
// unshadowed lists of the frame, every such upload kept that frame from being replayed (2026-11-13: in-world
// "lists without a shadow", all from LoadProcess threads; the first played test flipped stereo <-> big screen).
// What another queue uploads is in place before the frame reads it, so the replay has nothing to redo there.
std::atomic<ID3D12CommandQueue*> g_gameQueue{nullptr};
std::atomic<long> g_otherQueueExecs{0};
void NoteOtherQueue(ID3D12CommandQueue* q) {
    g_otherQueueExecs.fetch_add(1, std::memory_order_relaxed);
    static std::mutex s_m;
    static std::vector<ID3D12CommandQueue*> s_seen;
    std::lock_guard<std::mutex> g(s_m);
    if (s_seen.size() >= 16 || std::find(s_seen.begin(), s_seen.end(), q) != s_seen.end()) return;
    s_seen.push_back(q);
    LOGI("[replay] queue %p (%s) takes submissions from [%s]: not the game's present queue - not shadowed, its "
         "signals not held", (void*)q, QName(QueueType(q)), ThreadLabel(GetCurrentThreadId()).c_str());
}

void PassSubmitted(UINT n, ID3D12CommandList* const* lists);   // pass log (bench), below

void STDMETHODCALLTYPE HkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (n && lists && lists[0] && !g_listHooked.load(std::memory_order_relaxed)) HookListVt(lists[0]);
    if (n && lists) {
        if (q == g_gameQueue.load(std::memory_order_relaxed)) {
            TraceNote(t_inErvrPresent ? 'e' : 'E', 0, n);
            if (!t_inErvrPresent) PassSubmitted(n, lists);
            ShadowSubmitted(n, lists);
            if (!t_inErvrPresent) g_cbvAtLastExec.store(g_frameCbvN.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            NoteOtherQueue(q);
        }
    }
    if (Ev* e = Rec(EvExec)) {
        e->a = q; e->v = n; e->qtype = QueueType(q);
        for (UINT i = 0; i < n && i < 3; ++i) e->lists[i] = lists[i];
    }
    g_origExec(q, n, lists);
}
// The game's fence signals, held back while its frame's shadows wait to be replayed. The game frees memory
// and resources once its fences say the GPU is done with a frame; the replay runs a frame's commands again
// AFTER those signals would have landed, so without this it reads what the game already released (2026-11-12:
// device lost at the first loading screen). Released at the present, right behind the replay - or by the
// watchdog if no present comes, so the game can never wait on a signal we hold.
struct HeldSignal { ID3D12CommandQueue* q; ID3D12Fence* f; UINT64 v; };
std::mutex g_heldMtx;
std::vector<HeldSignal> g_held;
std::atomic<long long> g_heldSince{0};       // QPC of the oldest held signal (0 = none)
std::atomic<bool> g_holdSignals{false};
std::atomic<bool> g_heldFlushedEarly{false};  // the watchdog released them: that frame must not be replayed
bool IsGameThread();

// Caller holds g_heldMtx.
void ReleaseHeldLocked() {
    for (const HeldSignal& h : g_held) g_origSignal(h.q, h.f, h.v);
    g_held.clear();
    g_heldSince.store(0);
}

HRESULT STDMETHODCALLTYPE HkSignal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    if (f && !g_fenceHooked.load(std::memory_order_relaxed)) HookFenceVt(f);
    if (Ev* e = Rec(EvSignal)) { e->a = q; e->b = f; e->v = v; e->qtype = QueueType(q); }
    if (g_holdSignals.load(std::memory_order_relaxed) && q == g_gameQueue.load(std::memory_order_relaxed) &&
        !t_inErvrPresent && IsGameThread()) {
        std::lock_guard<std::mutex> g(g_heldMtx);
        if (g_held.empty()) g_heldSince.store(Qpc());
        g_held.push_back({q, f, v});
        return S_OK;
    }
    return g_origSignal(q, f, v);
}
HRESULT STDMETHODCALLTYPE HkWait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    if (Ev* e = Rec(EvWait)) { e->a = q; e->b = f; e->v = v; e->qtype = QueueType(q); }
    return g_origWait(q, f, v);
}
HRESULT STDMETHODCALLTYPE HkFenceEvent(ID3D12Fence* f, UINT64 v, HANDLE h) {
    if (Ev* e = Rec(EvCpuWait)) { e->a = f; e->v = v; }
    return g_origFenceEvent(f, v, h);
}
HRESULT STDMETHODCALLTYPE HkAllocReset(ID3D12CommandAllocator* a) {
    if (Ev* e = Rec(EvAllocReset)) e->a = a;
    return g_origAllocReset(a);
}

void HookFenceVt(ID3D12Fence* f) {
    if (g_fenceHooked.exchange(true)) return;
    HookSlot(f, 9, (void*)&HkFenceEvent, (void**)&g_origFenceEvent, "ID3D12Fence::SetEventOnCompletion");
}
void HookAllocVt(ID3D12CommandAllocator* a) {
    if (g_allocHooked.exchange(true)) return;
    HookSlot(a, 8, (void*)&HkAllocReset, (void**)&g_origAllocReset, "ID3D12CommandAllocator::Reset");
}

// ---- command-list census ------------------------------------------------------------------------------
// One typed detour per vtable slot of ID3D12GraphicsCommandList10 (slot order taken from d3d12.h's C
// vtable, verified 2026-11-12: 9 Close, 10 Reset, 12 DrawInstanced ... 85 DispatchGraph). Today they
// count calls and note root constant-buffer bindings; the mirroring that makes the second eye's shadow
// lists hangs off the same detours.
constexpr int kSlots = 86;
std::atomic<long> g_calls[kSlots];
std::atomic<bool> g_listArmed{false};   // the list census frame
thread_local ID3D12RootSignature* t_gfxSig = nullptr;
thread_local ID3D12RootSignature* t_cmpSig = nullptr;

struct CbvRec { ID3D12RootSignature* sig; uint64_t va; DWORD tid; uint16_t root; uint8_t compute; };
constexpr int kCbv = 200000;
CbvRec g_cbv[kCbv];
std::atomic<int> g_cbvN{0};
std::map<DWORD, long> g_threadCalls;   // filled at dump from per-thread counters
struct ThreadCount { DWORD tid; std::atomic<long> n; };
ThreadCount g_tc[64];
std::atomic<int> g_tcN{0};
thread_local int t_tcIndex = -1;

void CountThread() {
    if (t_tcIndex < 0) {
        const int i = g_tcN.fetch_add(1);
        if (i >= 64) { t_tcIndex = 63; return; }
        g_tc[i].tid = GetCurrentThreadId();
        t_tcIndex = i;
    }
    g_tc[t_tcIndex].n.fetch_add(1, std::memory_order_relaxed);
}

inline void OnListCall(int slot) {
    if (!g_listArmed.load(std::memory_order_relaxed) || t_inErvrPresent) return;
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
    CountThread();
}

// Root constant buffers bound this frame (never through a view, so the patch's view list misses them): address, compute.
struct RootCbv { uint64_t va; uint8_t compute; uint8_t root; };
constexpr int kRootCbv = 4096;
RootCbv g_rootCbv[kRootCbv];
std::atomic<int> g_rootCbvN{0};
void NoteRootCbv(bool compute, UINT root, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_inErvrPresent || !va) return;
    const int i = g_rootCbvN.fetch_add(1, std::memory_order_relaxed);
    if (i < kRootCbv) g_rootCbv[i] = {va, (uint8_t)compute, (uint8_t)root};
}

// Root 32-bit constants (values baked into the command list - a replay cannot change them): calls, values, and how many
// hold the camera position (the last present's camera, kept in g_camForRc).
std::atomic<long> g_rcCalls{0}, g_rcValues{0}, g_rcCamHits{0};
float g_camForRc[3] = {};
void NoteRootConstants(UINT n, const void* src) {
    if (t_inErvrPresent || !src) return;
    g_rcCalls.fetch_add(1, std::memory_order_relaxed);
    g_rcValues.fetch_add((long)n, std::memory_order_relaxed);
    const float* f = (const float*)src;
    for (UINT i = 0; i + 3 <= n; ++i)
        if (std::fabs(f[i] - g_camForRc[0]) < 5e-3f && std::fabs(f[i + 1] - g_camForRc[1]) < 5e-3f &&
            std::fabs(f[i + 2] - g_camForRc[2]) < 5e-3f)
            g_rcCamHits.fetch_add(1, std::memory_order_relaxed);
}

void NoteCbv(bool compute, UINT root, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (!g_listArmed.load(std::memory_order_relaxed) || t_inErvrPresent) return;
    const int i = g_cbvN.fetch_add(1, std::memory_order_relaxed);
    if (i < kCbv) g_cbv[i] = {compute ? t_cmpSig : t_gfxSig, va, GetCurrentThreadId(), (uint16_t)root, (uint8_t)compute};
}

// Shader-visible heaps, as bound: a table's GPU handle maps back to the CPU handle its descriptors were
// written through (cpu = cpuStart + (gpu - gpuStart)).
struct HeapInfo { uint64_t cpu, gpu; UINT num; D3D12_DESCRIPTOR_HEAP_TYPE type; };
std::mutex g_heapMtx;
std::map<ID3D12DescriptorHeap*, HeapInfo> g_heaps;
thread_local ID3D12DescriptorHeap* t_viewHeap = nullptr;   // the list's bound CBV/SRV/UAV heap

void NoteHeaps(UINT n, ID3D12DescriptorHeap* const* heaps) {
    for (UINT i = 0; i < n && heaps; ++i) {
        ID3D12DescriptorHeap* h = heaps[i];
        if (!h) continue;
        {
            std::lock_guard<std::mutex> g(g_heapMtx);
            auto it = g_heaps.find(h);
            if (it != g_heaps.end()) {
                if (it->second.type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) t_viewHeap = h;
                continue;
            }
        }
        const D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
        HeapInfo hi{h->GetCPUDescriptorHandleForHeapStart().ptr,
                    (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? h->GetGPUDescriptorHandleForHeapStart().ptr : 0,
                    d.NumDescriptors, d.Type};
        std::lock_guard<std::mutex> g(g_heapMtx);
        g_heaps[h] = hi;
        if (d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) t_viewHeap = h;
    }
}

struct TblRec { ID3D12RootSignature* sig; uint64_t gpu; ID3D12DescriptorHeap* heap; uint16_t root; uint8_t compute; };
constexpr int kTbl = 200000;
TblRec g_tbl[kTbl];
std::atomic<int> g_tblN{0};

void NoteTable(bool compute, UINT root, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (!g_listArmed.load(std::memory_order_relaxed) || t_inErvrPresent) return;
    const int i = g_tblN.fetch_add(1, std::memory_order_relaxed);
    if (i < kTbl) g_tbl[i] = {compute ? t_cmpSig : t_gfxSig, h.ptr, t_viewHeap, (uint16_t)root, (uint8_t)compute};
}

// ---- depth-target census ([Experimental] ReplayDepthCensus) ---------------------------------------------------------
// Which depth buffers the game draws into, frame by frame. A shadow cascade drawn only every few frames is kept from an
// earlier frame - and the replay, which draws the frame a second time for the right eye, would leave ITS version behind
// for the next left eye (2026-11-13: "the shadow in my left eye is offset to the left of the geometry it falls on").
// The resource's description is taken when the view is made: the census never touches a resource that may be gone.
struct DsvInfo {
    const void* res = nullptr;   // identity only, never dereferenced
    uint64_t w = 0; uint32_t h = 0; uint16_t depth = 0, mips = 0; DXGI_FORMAT resFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; D3D12_DSV_DIMENSION dim = D3D12_DSV_DIMENSION_UNKNOWN; UINT mip = 0, slice = 0, slices = 1;
};
using PfnCreateDsv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
PfnCreateDsv g_origCreateDsv = nullptr;
std::mutex g_dsvMtx;
std::unordered_map<uint64_t, DsvInfo> g_dsvAt;   // DSV CPU handle -> what it views

// Shadow-map depth views ([Experimental] ReplaySkipShadowMap): a depth-only format on a power-of-two target - the sun's
// 1024x512 D16 atlas at Low shadows; the scene's depth buffers are frame-sized. Read lock-free on the recording threads.
bool g_skipShadowMap = false;   // set with the device hooks, from the config
std::atomic<uint64_t> g_shadowDsv[64];
std::atomic<int> g_shadowDsvN{0};
std::atomic<long> g_shadowMapSkips{0};
thread_local bool t_dsvShadow = false;
bool IsShadowDsv(uint64_t h) {
    const int n = std::min(g_shadowDsvN.load(std::memory_order_acquire), 64);
    for (int i = 0; i < n; ++i)
        if (g_shadowDsv[i].load(std::memory_order_relaxed) == h) return true;
    return false;
}
void NoteShadowDsv(uint64_t h, bool shadow) {   // caller holds g_dsvMtx
    const int n = std::min(g_shadowDsvN.load(std::memory_order_relaxed), 64);
    for (int i = 0; i < n; ++i)
        if (g_shadowDsv[i].load(std::memory_order_relaxed) == h) { if (!shadow) g_shadowDsv[i].store(1); return; }
    if (!shadow || n >= 64) return;
    g_shadowDsv[n].store(h);
    g_shadowDsvN.store(n + 1, std::memory_order_release);
}

void STDMETHODCALLTYPE HkCreateDsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_origCreateDsv(d, r, desc, h);
    if (!r) { std::lock_guard<std::mutex> g(g_dsvMtx); NoteShadowDsv(h.ptr, false); return; }
    DsvInfo i;
    const D3D12_RESOURCE_DESC rd = r->GetDesc();
    i.res = r; i.w = rd.Width; i.h = rd.Height; i.depth = rd.DepthOrArraySize; i.mips = rd.MipLevels; i.resFmt = rd.Format;
    i.fmt = desc ? desc->Format : rd.Format;
    i.dim = desc ? desc->ViewDimension : D3D12_DSV_DIMENSION_TEXTURE2D;
    if (desc) {
        switch (desc->ViewDimension) {
            case D3D12_DSV_DIMENSION_TEXTURE2D: i.mip = desc->Texture2D.MipSlice; break;
            case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
                i.mip = desc->Texture2DArray.MipSlice; i.slice = desc->Texture2DArray.FirstArraySlice;
                i.slices = desc->Texture2DArray.ArraySize; break;
            case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
                i.slice = desc->Texture2DMSArray.FirstArraySlice; i.slices = desc->Texture2DMSArray.ArraySize; break;
            default: break;
        }
    }
    const bool pow2 = i.w && i.h && (i.w & (i.w - 1)) == 0 && (i.h & (i.h - 1)) == 0 && i.w <= 16384 && i.h <= 16384;
    // Every 16-bit depth target too (the scene's depth buffers are D32S8/D24S8): the 768x512 D16 one (two draws a frame) is
    // not a power of two.
    const bool shadow = (pow2 && i.fmt == DXGI_FORMAT_D32_FLOAT) || i.fmt == DXGI_FORMAT_D16_UNORM;
    std::lock_guard<std::mutex> g(g_dsvMtx);
    g_dsvAt[h.ptr] = i;
    NoteShadowDsv(h.ptr, shadow);
    if (shadow) {
        static int s_logged = 0;
        if (s_logged++ < 8)
            LOGI("[replay] shadow-map depth view %llx: %llux%u x%u fmt %u", (unsigned long long)h.ptr, (unsigned long long)i.w,
                 i.h, i.depth, (unsigned)i.fmt);
    }
}

// Per DSV handle, lock-free (recording threads): draws and clears, and in how many frames (presents) each happened.
struct DepthUse {
    std::atomic<uint64_t> key{0};
    std::atomic<long> draws{0}, clears{0}, framesDrawn{0}, framesCleared{0}, lastDraw{-1}, lastClear{-1}, firstFrame{-1};
};
constexpr int kDepthUse = 4096;
DepthUse g_depthUse[kDepthUse];
std::atomic<bool> g_depthArmed{false};
std::atomic<long> g_depthFrame{0};
// Per frame, the order depth targets were first drawn in (which pass family each is), for the census's frame list.
thread_local ID3D12GraphicsCommandList* t_dsvList = nullptr;   // the list t_dsv was bound on
thread_local uint64_t t_dsv = 0;

DepthUse* DepthSlot(uint64_t h) {
    uint32_t i = (uint32_t)(((h >> 3) * 2654435761ull) & (kDepthUse - 1));
    for (int n = 0; n < kDepthUse; ++n, i = (i + 1) & (kDepthUse - 1)) {
        uint64_t k = g_depthUse[i].key.load(std::memory_order_relaxed);
        if (k == h) return &g_depthUse[i];
        if (k == 0) {
            if (g_depthUse[i].key.compare_exchange_strong(k, h)) return &g_depthUse[i];
            if (k == h) return &g_depthUse[i];
        }
    }
    return nullptr;
}

void DepthNote(uint64_t h, bool clear) {
    if (!h) return;
    DepthUse* u = DepthSlot(h);
    if (!u) return;
    const long f = g_depthFrame.load(std::memory_order_relaxed);
    long none = -1;
    u->firstFrame.compare_exchange_strong(none, f);
    if (clear) {
        u->clears.fetch_add(1, std::memory_order_relaxed);
        if (u->lastClear.exchange(f) != f) u->framesCleared.fetch_add(1, std::memory_order_relaxed);
    } else {
        u->draws.fetch_add(1, std::memory_order_relaxed);
        if (u->lastDraw.exchange(f) != f) u->framesDrawn.fetch_add(1, std::memory_order_relaxed);
    }
}

void DumpDepthCensus(long frames) {
    struct Row { DsvInfo i; uint64_t h; long draws, clears, fd, fc; bool known; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> g(g_dsvMtx);
        for (DepthUse& u : g_depthUse) {
            const uint64_t h = u.key.load();
            if (!h) continue;
            auto it = g_dsvAt.find(h);
            rows.push_back({it != g_dsvAt.end() ? it->second : DsvInfo{}, h, u.draws.load(), u.clears.load(), u.framesDrawn.load(),
                            u.framesCleared.load(), it != g_dsvAt.end()});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.i.res != b.i.res) return a.i.w * a.i.h != b.i.w * b.i.h ? a.i.w * a.i.h > b.i.w * b.i.h : a.i.res < b.i.res;
        return a.i.slice < b.i.slice;
    });
    LOGI("[replay] ---- depth-target census: %ld frames, %zu depth views drawn or cleared ----", frames, rows.size());
    for (const Row& r : rows) {
        if (!r.known) {
            LOGI("[replay] depth view %llx (made before the hook): drawn in %ld of %ld frames (%ld draws), cleared in %ld",
                 (unsigned long long)r.h, r.fd, frames, r.draws, r.fc);
            continue;
        }
        LOGI("[replay] depth %p %llux%u x%u mips %u fmt %u (view fmt %u dim %d mip %u slice %u+%u): drawn in %ld of %ld frames "
             "(%.0f draws a drawn frame), cleared in %ld",
             r.i.res, (unsigned long long)r.i.w, r.i.h, r.i.depth, r.i.mips, (unsigned)r.i.resFmt, (unsigned)r.i.fmt, (int)r.i.dim,
             r.i.mip, r.i.slice, r.i.slices, r.fd, frames, r.fd ? (double)r.draws / r.fd : 0.0, r.fc);
    }
    LOGI("[replay] ---- depth-target census end ----");
}

// ---- pass log (bench: ERVR_PASS_LOG=<seconds> in the environment) ------------------------------------------------------
// One frame's commands in submission order, <seconds> after the player is in the world: pipelines, draws and dispatches
// with their arguments, render and depth targets, root signatures, descriptor tables with the constant views in their
// first slots ([Experimental] ReplayCensus keeps the slot map), root views and constants, barriers, copies, markers.
// Written to passlog.csv (+ passlog-cbv.bin: every constant view it names, raw) next to the ini; the shader dump
// (reshade_addon.cpp, ERVR_SHADER_DUMP) says which shaders each pipeline runs and what each root parameter binds.
struct PassEv { char kind; uint64_t a, b, c, d; std::string s; };
std::atomic<int> g_passLog{0};   // 0 off; 1 lists reset from here keep their calls; 2 the frame's submissions collected; 3 done
std::mutex g_passMtx;
std::unordered_map<ID3D12GraphicsCommandList*, std::vector<PassEv>> g_passOf;   // game list -> its calls since its reset
struct PassList { ID3D12GraphicsCommandList* list; std::vector<PassEv> ev; bool recorded; };
std::vector<PassList> g_passFrame;   // the collected frame, in submission order

int PassLogSeconds() {
    static const int s = [] {
        wchar_t b[32] = {};
        return GetEnvironmentVariableW(L"ERVR_PASS_LOG", b, 32) ? _wtoi(b) : 0;
    }();
    return s;
}
inline bool PassLogRecording() {
    const int s = g_passLog.load(std::memory_order_relaxed);
    return (s == 1 || s == 2) && !t_inErvrPresent;
}
void PassListReset(ID3D12GraphicsCommandList* l) {
    std::lock_guard<std::mutex> g(g_passMtx);
    g_passOf[l].clear();
}
void PassNote(ID3D12GraphicsCommandList* l, PassEv&& e) {
    std::lock_guard<std::mutex> g(g_passMtx);
    auto it = g_passOf.find(l);
    if (it != g_passOf.end()) it->second.push_back(std::move(e));
}
void PassSubmitted(UINT n, ID3D12CommandList* const* lists) {
    const int s = g_passLog.load(std::memory_order_relaxed);
    if (s != 1 && s != 2) return;
    std::lock_guard<std::mutex> g(g_passMtx);
    for (UINT i = 0; i < n; ++i) {
        auto* gl = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
        auto it = g_passOf.find(gl);
        if (s == 2) {
            if (it != g_passOf.end()) g_passFrame.push_back({gl, it->second, true});
            else g_passFrame.push_back({gl, {}, false});
        }
        if (it != g_passOf.end()) it->second.clear();
    }
}
// A descriptor table's first 16 slots: the constant views in them ("slot:va:size;"), from the census slot map.
std::string PassTableViews(D3D12_GPU_DESCRIPTOR_HANDLE h) {
    HeapInfo hi{};
    {
        std::lock_guard<std::mutex> g(g_heapMtx);
        auto it = g_heaps.find(t_viewHeap);
        if (it == g_heaps.end()) return "?heap";
        hi = it->second;
    }
    if (!hi.gpu || !g_descInc || h.ptr < hi.gpu || h.ptr >= hi.gpu + (uint64_t)hi.num * g_descInc) return "?range";
    const uint64_t cpu = hi.cpu + (h.ptr - hi.gpu);
    std::string s;
    std::lock_guard<std::mutex> g(g_descMtx);
    for (int k = 0; k < 24; ++k) {
        const uint64_t at = cpu + (uint64_t)k * g_descInc;
        char b[80];
        auto it = g_cbvAt.find(at);
        if (it != g_cbvAt.end()) {
            snprintf(b, sizeof(b), "%d:c:%llx:%u;", k, (unsigned long long)it->second.va, it->second.size);
            s += b;
            continue;
        }
        auto v = g_viewAt.find(at);
        if (v != g_viewAt.end()) {
            snprintf(b, sizeof(b), "%d:%c:%llx:f%u:d%u;", k, v->second.kind, (unsigned long long)v->second.res, v->second.fmt,
                     (unsigned)v->second.dim);
            s += b;
        }
    }
    return s;
}
std::string PassMarker(UINT meta, const void* p, UINT size) {
    std::string s;
    if (!p || !size) return s;
    if (meta == 0) {   // PIX: wide string
        const wchar_t* w = (const wchar_t*)p;
        for (UINT i = 0; i < size / 2 && w[i] && s.size() < 96; ++i) s += (char)(w[i] < 128 ? w[i] : '?');
    } else {           // 1 = narrow string; 2 = PIX3 blob (printable bytes kept)
        const char* c = (const char*)p;
        for (UINT i = 0; i < size && s.size() < 96; ++i)
            if (c[i] >= 32 && c[i] < 127 && c[i] != ',') s += c[i];
    }
    return s;
}
template <int Slot, typename... A>
void PassRecord(ID3D12GraphicsCommandList* self, A... a) {
    const auto args = std::tuple<A...>(a...);
    (void)args;
    PassEv e{};
    if constexpr (Slot == 25) {
        e.kind = 'P'; e.a = (uint64_t)std::get<0>(args);
    } else if constexpr (Slot == 46) {   // OMSetRenderTargets
        const D3D12_CPU_DESCRIPTOR_HANDLE* rt = std::get<1>(args);
        const D3D12_CPU_DESCRIPTOR_HANDLE* ds = std::get<3>(args);
        e.kind = 'R'; e.a = std::get<0>(args); e.b = rt && std::get<0>(args) ? rt->ptr : 0; e.c = std::get<2>(args); e.d = ds ? ds->ptr : 0;
    } else if constexpr (Slot == 68) {   // BeginRenderPass
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt = std::get<1>(args);
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds = std::get<2>(args);
        e.kind = 'R'; e.a = std::get<0>(args); e.b = rt && std::get<0>(args) ? rt->cpuDescriptor.ptr : 0; e.d = ds ? ds->cpuDescriptor.ptr : 0;
    } else if constexpr (Slot == 12 || Slot == 13) {
        e.kind = Slot == 12 ? 'D' : 'I'; e.a = std::get<0>(args); e.b = std::get<1>(args); e.c = std::get<2>(args);
    } else if constexpr (Slot == 14) {
        e.kind = 'X'; e.a = std::get<0>(args); e.b = std::get<1>(args); e.c = std::get<2>(args);
    } else if constexpr (Slot == 59) {
        e.kind = 'E'; e.a = (uint64_t)std::get<0>(args); e.b = std::get<1>(args); e.c = (uint64_t)std::get<2>(args); e.d = std::get<3>(args);
    } else if constexpr (Slot == 29 || Slot == 30) {
        e.kind = 'S'; e.a = Slot == 29; e.b = (uint64_t)std::get<0>(args);
    } else if constexpr (Slot == 31 || Slot == 32) {
        e.kind = 'T'; e.a = Slot == 31; e.b = std::get<0>(args); e.c = std::get<1>(args).ptr; e.s = PassTableViews(std::get<1>(args));
    } else if constexpr (Slot >= 37 && Slot <= 42) {   // root CBV / SRV / UAV
        e.kind = Slot <= 38 ? 'V' : Slot <= 40 ? 'W' : 'Q'; e.a = (Slot & 1); e.b = std::get<0>(args); e.c = std::get<1>(args);
    } else if constexpr (Slot == 35 || Slot == 36) {
        e.kind = 'K'; e.a = Slot == 35; e.b = std::get<0>(args); e.c = std::get<1>(args);
        const float* f = (const float*)std::get<2>(args);
        for (UINT i = 0; f && i < std::get<1>(args) && i < 24; ++i) { char b[24]; snprintf(b, sizeof(b), "%g;", f[i]); e.s += b; }
    } else if constexpr (Slot == 33 || Slot == 34) {
        e.kind = 'k'; e.a = Slot == 33; e.b = std::get<0>(args); e.c = std::get<1>(args); e.d = std::get<2>(args);
    } else if constexpr (Slot == 47) {
        e.kind = 'Z'; e.a = std::get<0>(args).ptr;
    } else if constexpr (Slot == 48) {
        e.kind = 'C'; e.a = std::get<0>(args).ptr;
    } else if constexpr (Slot == 49 || Slot == 50) {
        e.kind = 'U'; e.a = std::get<0>(args).ptr; e.b = std::get<1>(args).ptr; e.c = (uint64_t)std::get<2>(args);
    } else if constexpr (Slot == 56 || Slot == 57) {
        e.kind = Slot == 56 ? 'M' : 'B'; e.s = PassMarker(std::get<0>(args), std::get<1>(args), std::get<2>(args));
    } else if constexpr (Slot == 58) {
        e.kind = 'e';
    } else if constexpr (Slot == 15) {   // dst resource, dst GPU address, src resource, bytes; s = src offset
        ID3D12Resource* dst = std::get<0>(args);
        e.kind = 'y'; e.a = (uint64_t)dst; e.b = dst ? dst->GetGPUVirtualAddress() + std::get<1>(args) : 0;
        e.c = (uint64_t)std::get<2>(args); e.d = std::get<4>(args);
        char b[24]; snprintf(b, sizeof(b), "%llx", (unsigned long long)std::get<3>(args)); e.s = b;
    } else if constexpr (Slot == 16) {
        const D3D12_TEXTURE_COPY_LOCATION* d = std::get<0>(args);
        const D3D12_TEXTURE_COPY_LOCATION* s = std::get<4>(args);
        e.kind = 'z'; e.a = d ? (uint64_t)d->pResource : 0; e.b = s ? (uint64_t)s->pResource : 0;
    } else if constexpr (Slot == 17) {
        e.kind = 'Y'; e.a = (uint64_t)std::get<0>(args); e.b = (uint64_t)std::get<1>(args);
    } else if constexpr (Slot == 26) {   // ResourceBarrier: transitions as "resource:before>after/subresource;"
        const UINT n = std::get<0>(args);
        const D3D12_RESOURCE_BARRIER* br = std::get<1>(args);
        e.kind = 'b'; e.a = n;
        for (UINT i = 0; br && i < n && i < 16; ++i) {
            char b[96];
            if (br[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                snprintf(b, sizeof(b), "%llx:%x>%x/%x;", (unsigned long long)br[i].Transition.pResource,
                         (unsigned)br[i].Transition.StateBefore, (unsigned)br[i].Transition.StateAfter, br[i].Transition.Subresource);
            else if (br[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
                snprintf(b, sizeof(b), "%llx:uav;", (unsigned long long)br[i].UAV.pResource);
            else
                snprintf(b, sizeof(b), "%llx:alias;", (unsigned long long)br[i].Aliasing.pResourceAfter);
            e.s += b;
        }
    } else if constexpr (Slot == 28) {
        e.kind = 'H'; e.a = std::get<0>(args);
    } else {
        return;
    }
    PassNote(self, std::move(e));
}

// ---- shadow recording: the second eye's command lists ------------------------------------------------
// Every command a game render thread records is recorded again into a shadow list of ours. The game resets
// each list right after submitting it (measured 2026-11-12: often within 0.1 ms) and records the next batch
// into the same object, so the game's own lists cannot be executed a second time - the shadows can.
using PfnListClose = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PfnListReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
PfnListClose g_listClose = nullptr;   // trampolines: our own calls on shadows never re-enter the detours
PfnListReset g_listReset = nullptr;

std::atomic<bool> g_shadowOn{false};
ID3D12Device* g_dev = nullptr;
struct Shadow {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    uint64_t fence = 0;                              // our replay fence value it was submitted under (0 = never)
    ID3D12GraphicsCommandList* owner = nullptr;      // the game list it is recording with, until submitted
    uint32_t gen = 0;                                // bumps every recording: validates thread-local caches
    // The game may execute a closed list again without re-recording it. Its shadow therefore stays the list's
    // entry in g_shadowOf after submission, until the game resets the list (or it goes stale), and every
    // submission adds a use: a shadow returns to the pool only once it is out of the map AND out of every batch
    // and every in-flight replay. All four under g_poolMtx (taken inside g_mapLock where both are needed).
    bool inMap = false;
    bool submitted = false;
    int uses = 0;
    ULONGLONG lastUse = 0;
    // Transitions of the per-eye volumes (see EyeVolume) this list records, in order: where the game leaves each one.
    std::vector<std::pair<ID3D12Resource*, uint32_t>> volStates;
};
std::atomic<uint32_t> g_shadowGen{0};
thread_local uint32_t t_lastGen = 0;
std::mutex g_poolMtx;
std::vector<Shadow*> g_free;
std::vector<std::pair<Shadow*, uint64_t>> g_inflight;   // one entry per replayed use, with that replay's fence value
int g_shadowCount = 0;
constexpr int kMaxShadows = 1024;
SRWLOCK g_mapLock = SRWLOCK_INIT;
std::unordered_map<ID3D12GraphicsCommandList*, Shadow*> g_shadowOf;   // game list -> the shadow recording with it
// Why a submitted list had no shadow (counted under g_mapLock): lists that never once recorded with one, and lists
// that did before but not this time (reset inside the present scope or on the XR thread, or dropped as stale).
std::unordered_set<ID3D12GraphicsCommandList*> g_everShadowed;
std::atomic<long> g_unshNever{0}, g_unshElsewhere{0}, g_reexecServed{0}, g_createdOpen{0}, g_staleDropped{0};
struct UnshadowedNote { DWORD tid; uint8_t never; };
UnshadowedNote g_unshNotes[8];
std::atomic<int> g_unshNoteN{0};
thread_local ID3D12GraphicsCommandList* t_lastGame = nullptr;
thread_local Shadow* t_lastShadow = nullptr;
std::mutex g_batchMtx;
std::vector<Shadow*> g_batch;            // shadows of what the game submitted since the last present, in order
std::atomic<bool> g_frameIncomplete{false};
std::atomic<long> g_shadowMisses{0}, g_unshadowedExecs{0}, g_replays{0}, g_replayLists{0}, g_skipped{0},
    g_watchdogFlushes{0};
thread_local int t_gameThread = -1;

// Whose command lists are the game's: everything recorded outside the present call (the present scope runs
// from ERVR's present event to ReShade's finish_present, so it covers ERVR's, ReShade's and the runtime's own
// work) on any thread but ERVR's XR thread. The game records on its GX workers and GX render thread, Havok's
// workers, and its unnamed main thread (~8 lists a frame, 2026-11-12 census).
bool IsGameThread() {
    if (t_gameThread < 0) t_gameThread = ThreadName(GetCurrentThreadId()) == "ERVR XR" ? 0 : 1;
    return t_gameThread == 1;
}

// The device's CreateCommandList, past HkCreateList when that is in (set by InstallDeviceHooks).
using PfnCreateList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                  ID3D12PipelineState*, REFIID, void**);
PfnCreateList g_origCreateList = nullptr;
thread_local bool t_creatingShadow = false;   // HkCreateList must not shadow the shadow pool's own lists

Shadow* ShadowAcquire() {
    std::lock_guard<std::mutex> g(g_poolMtx);
    if (!g_free.empty()) { Shadow* s = g_free.back(); g_free.pop_back(); return s; }
    if (g_shadowCount >= kMaxShadows || !g_dev || !g_listClose) return nullptr;
    auto* s = new Shadow();
    // A pool list made through the hooked CreateCommandList would start a shadow of itself, inside this lock
    // (2026-11-13: the first flat run with the hook died the moment shadowing began).
    t_creatingShadow = true;
    const bool made =
        SUCCEEDED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->alloc))) &&
        SUCCEEDED(g_origCreateList
                      ? g_origCreateList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc, nullptr, IID_PPV_ARGS(&s->list))
                      : g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc, nullptr, IID_PPV_ARGS(&s->list)));
    t_creatingShadow = false;
    if (!made) {
        if (s->alloc) s->alloc->Release();
        delete s;
        return nullptr;
    }
    g_listClose(s->list);   // created open; the pool holds closed lists
    ++g_shadowCount;
    return s;
}
void ShadowFreeLocked(Shadow* s) {
    s->fence = 0; s->owner = nullptr; s->inMap = false; s->submitted = false; s->uses = 0;
    g_free.push_back(s);
}
// Caller holds g_poolMtx: one batch or in-flight use is over.
void ShadowDropUseLocked(Shadow* s) {
    if (s->uses > 0) --s->uses;
    if (!s->uses && !s->inMap) ShadowFreeLocked(s);
}

// The shadow recording alongside `game`, or null (no shadow / shadowing off). Thread-local cache first:
// a thread records one list at a time, so nearly every call is a hit; owner + generation make a stale
// cache entry (the list reset and recorded elsewhere since) fall through to the map.
inline ID3D12GraphicsCommandList* ShadowOf(ID3D12GraphicsCommandList* game) {
    if (!g_shadowOn.load(std::memory_order_relaxed)) return nullptr;
    Shadow* c = t_lastShadow;
    if (game == t_lastGame && c && c->owner == game && c->gen == t_lastGen) return c->list;
    AcquireSRWLockShared(&g_mapLock);
    auto it = g_shadowOf.find(game);
    Shadow* s = it != g_shadowOf.end() ? it->second : nullptr;
    if (s && s->owner != game) s = nullptr;   // submitted (closed): kept for re-execution, never recorded into again
    ReleaseSRWLockShared(&g_mapLock);
    t_lastGame = game; t_lastShadow = s; t_lastGen = s ? s->gen : 0;
    return s ? s->list : nullptr;
}

// The game reset `game` on a recording thread: start a fresh shadow with the same initial pipeline state.
void ShadowBegin(ID3D12GraphicsCommandList* game, ID3D12PipelineState* pso) {
    Shadow* old = nullptr;
    AcquireSRWLockExclusive(&g_mapLock);
    if (auto it = g_shadowOf.find(game); it != g_shadowOf.end()) { old = it->second; g_shadowOf.erase(it); }
    ReleaseSRWLockExclusive(&g_mapLock);
    if (old) {
        std::lock_guard<std::mutex> g(g_poolMtx);
        old->inMap = false;
        if (!old->submitted) {   // recorded but never submitted: still open, its allocator never on the GPU
            g_listClose(old->list);
            ShadowFreeLocked(old);
        } else if (!old->uses) {
            ShadowFreeLocked(old);
        }   // else: still in a batch or a replay in flight - freed when the last use ends
    }
    // Only direct lists: copy and compute lists belong to the game's other queues, which the replay leaves alone.
    const bool want = g_shadowOn.load() && IsGameThread() && !t_inErvrPresent &&
                      game->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
    Shadow* s = want ? ShadowAcquire() : nullptr;
    if (s) {
        s->alloc->Reset();
        if (FAILED(g_listReset(s->list, s->alloc, pso))) {
            std::lock_guard<std::mutex> g(g_poolMtx);
            ShadowFreeLocked(s);
            s = nullptr;
        }
    }
    if (!s && want) g_shadowMisses.fetch_add(1, std::memory_order_relaxed);
    if (s) {
        s->volStates.clear();
        s->owner = game;
        s->gen = g_shadowGen.fetch_add(1) + 1;
        AcquireSRWLockExclusive(&g_mapLock);
        {
            std::lock_guard<std::mutex> g(g_poolMtx);
            s->inMap = true;
            s->submitted = false;
            s->uses = 0;
        }
        g_shadowOf[game] = s;
        g_everShadowed.insert(game);
        ReleaseSRWLockExclusive(&g_mapLock);
    }
    t_lastGame = game; t_lastShadow = s; t_lastGen = s ? s->gen : 0;
}

// The game submitted these lists: their shadows join this frame's replay batch, in the same order.
void ShadowSubmitted(UINT n, ID3D12CommandList* const* lists) {
    if (!g_shadowOn.load(std::memory_order_relaxed) || t_inErvrPresent || !IsGameThread()) return;
    std::vector<Shadow*> found;
    found.reserve(n);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_mapLock);
    for (UINT i = 0; i < n; ++i) {
        auto* gl = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
        auto it = g_shadowOf.find(gl);
        if (it == g_shadowOf.end()) {
            g_frameIncomplete.store(true);
            g_unshadowedExecs.fetch_add(1);
            const bool never = !g_everShadowed.count(gl);
            (never ? g_unshNever : g_unshElsewhere).fetch_add(1, std::memory_order_relaxed);
            const int k = g_unshNoteN.fetch_add(1, std::memory_order_relaxed);
            if (k < 8) g_unshNotes[k] = {GetCurrentThreadId(), (uint8_t)never};
            continue;
        }
        Shadow* s = it->second;
        // The entry stays: a closed list may be executed again without a reset, and its shadow with it.
        std::lock_guard<std::mutex> g(g_poolMtx);
        if (s->submitted) g_reexecServed.fetch_add(1, std::memory_order_relaxed);
        s->submitted = true;
        s->owner = nullptr;   // no more recording into it: thread-local caches must miss
        ++s->uses;
        s->lastUse = now;
        found.push_back(s);
    }
    ReleaseSRWLockExclusive(&g_mapLock);
    t_lastGame = nullptr; t_lastShadow = nullptr;
    std::lock_guard<std::mutex> g(g_batchMtx);
    g_batch.insert(g_batch.end(), found.begin(), found.end());
}

void NoteCopySource(ID3D12Resource* src, uint64_t off, uint64_t n, bool replayed);   // the GPU patch's copy sources, below

// Per-eye volumes (see EyeVolume below): the game's 3D UAV textures, lock-free for the barrier hook.
constexpr int kVolMax = 32;
std::atomic<ID3D12Resource*> g_volRes[kVolMax];
std::atomic<int> g_volResN{0};
// ResourceBarrier on a game list: transitions of a per-eye volume go on that list's shadow record, in order.
void NoteVolBarriers(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* br) {
    const int nv = std::min(g_volResN.load(std::memory_order_acquire), kVolMax);
    if (!nv || !br || t_inErvrPresent) return;
    for (UINT i = 0; i < n; ++i) {
        if (br[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        ID3D12Resource* r = br[i].Transition.pResource;
        bool mine = false;
        for (int k = 0; k < nv && !mine; ++k) mine = g_volRes[k].load(std::memory_order_relaxed) == r;
        if (!mine) continue;
        if (!ShadowOf(self) || !t_lastShadow) return;   // not a list the replay runs
        t_lastShadow->volStates.push_back({r, (uint32_t)br[i].Transition.StateAfter});
    }
}

template <int Slot, typename MF> struct LH;
template <int Slot, typename R, typename C, typename... A>
struct LH<Slot, R (STDMETHODCALLTYPE C::*)(A...)> {
    using Fn = R(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, A...);
    static inline Fn orig = nullptr;
    static R STDMETHODCALLTYPE Detour(ID3D12GraphicsCommandList* self, A... a) {
        if constexpr (Slot == 10) {   // Reset: the game's first, then a fresh shadow
            const R r = orig(self, a...);
            if (g_shadowOn.load(std::memory_order_relaxed)) ShadowBegin(self, std::get<1>(std::tuple<A...>(a...)));
            if (PassLogRecording()) {
                PassListReset(self);
                if (ID3D12PipelineState* p0 = std::get<1>(std::tuple<A...>(a...))) PassNote(self, PassEv{'P', (uint64_t)p0, 0, 0, 0, {}});
            }
            OnListCall(Slot);
            t_gfxSig = nullptr; t_cmpSig = nullptr;
            if (t_dsvList == self) t_dsvList = nullptr;
            return r;
        } else {
            bool mirror = true;   // [Experimental] ReplaySkipShadowMap: the shadow map's own draws are not recorded again
            if constexpr (Slot == 46 || Slot == 68) {
                if (g_skipShadowMap && !t_inErvrPresent) {
                    const auto args = std::tuple<A...>(a...);
                    uint64_t h = 0;
                    if constexpr (Slot == 46) { const D3D12_CPU_DESCRIPTOR_HANDLE* d = std::get<3>(args); h = d ? d->ptr : 0; }
                    else { const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d = std::get<2>(args); h = d ? d->cpuDescriptor.ptr : 0; }
                    t_dsvList = self; t_dsv = h; t_dsvShadow = h && IsShadowDsv(h);
                }
            }
            if constexpr (Slot == 12 || Slot == 13 || Slot == 59) {
                if (g_skipShadowMap && t_dsvList == self && t_dsvShadow) { mirror = false; g_shadowMapSkips.fetch_add(1, std::memory_order_relaxed); }
            }
            if constexpr (Slot == 47) {
                if (g_skipShadowMap && IsShadowDsv(std::get<0>(std::tuple<A...>(a...)).ptr)) { mirror = false; g_shadowMapSkips.fetch_add(1, std::memory_order_relaxed); }
            }
            if (mirror)
                if (ID3D12GraphicsCommandList* sh = ShadowOf(self)) orig(sh, a...);   // the same command, recorded again
            if (PassLogRecording()) PassRecord<Slot>(self, a...);
            if constexpr (Slot == 26) {   // ResourceBarrier: where the per-eye volumes are left
                if (g_volResN.load(std::memory_order_relaxed)) {
                    const auto args = std::tuple<A...>(a...);
                    NoteVolBarriers(self, std::get<0>(args), std::get<1>(args));
                }
            }
            if constexpr (Slot == 15) {   // CopyBufferRegion: constants staged in the ring for a GPU-only buffer
                if (!t_inErvrPresent) {
                    const auto args = std::tuple<A...>(a...);
                    NoteCopySource(std::get<2>(args), std::get<3>(args), std::get<4>(args), ShadowOf(self) != nullptr);
                }
            }
        }
        OnListCall(Slot);
        if constexpr (Slot == 46 || Slot == 47 || Slot == 68 || Slot == 12 || Slot == 13 || Slot == 59) {
            if (g_depthArmed.load(std::memory_order_relaxed) && !t_inErvrPresent) {
                const auto args = std::tuple<A...>(a...);
                (void)args;
                if constexpr (Slot == 46) {   // OMSetRenderTargets: the depth view the next draws go to
                    const D3D12_CPU_DESCRIPTOR_HANDLE* d = std::get<3>(args);
                    t_dsvList = self; t_dsv = d ? d->ptr : 0;
                } else if constexpr (Slot == 68) {   // BeginRenderPass
                    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d = std::get<2>(args);
                    t_dsvList = self; t_dsv = d ? d->cpuDescriptor.ptr : 0;
                } else if constexpr (Slot == 47) {   // ClearDepthStencilView
                    DepthNote(std::get<0>(args).ptr, true);
                } else if (t_dsvList == self && t_dsv) {   // a draw
                    DepthNote(t_dsv, false);
                }
            }
        }
        if constexpr (Slot == 35 || Slot == 36) {
            const auto args = std::tuple<A...>(a...);
            NoteRootConstants(std::get<1>(args), std::get<2>(args));
        }
        if constexpr (Slot == 33 || Slot == 34) g_rcCalls.fetch_add(1, std::memory_order_relaxed);
        if constexpr (Slot == 29) t_cmpSig = std::get<0>(std::tuple<A...>(a...));
        if constexpr (Slot == 30) t_gfxSig = std::get<0>(std::tuple<A...>(a...));
        if constexpr (Slot == 37 || Slot == 38) {
            const auto args = std::tuple<A...>(a...);
            NoteCbv(Slot == 37, std::get<0>(args), std::get<1>(args));
            NoteRootCbv(Slot == 37, std::get<0>(args), std::get<1>(args));
        }
        if constexpr (Slot == 28) {
            const auto args = std::tuple<A...>(a...);
            NoteHeaps(std::get<0>(args), std::get<1>(args));
        }
        if constexpr (Slot == 31 || Slot == 32) {
            const auto args = std::tuple<A...>(a...);
            NoteTable(Slot == 31, std::get<0>(args), std::get<1>(args));
        }
        return orig(self, a...);
    }
};

#define ERVR_LIST_HOOK(slot, iface, method) \
    { slot, (void*)&LH<slot, decltype(&iface::method)>::Detour, (void**)&LH<slot, decltype(&iface::method)>::orig, #method }

struct SlotHook { int slot; void* detour; void** orig; const char* name; };
const SlotHook kListHooks[] = {
    ERVR_LIST_HOOK(9, ID3D12GraphicsCommandList, Close),
    ERVR_LIST_HOOK(10, ID3D12GraphicsCommandList, Reset),
    ERVR_LIST_HOOK(11, ID3D12GraphicsCommandList, ClearState),
    ERVR_LIST_HOOK(12, ID3D12GraphicsCommandList, DrawInstanced),
    ERVR_LIST_HOOK(13, ID3D12GraphicsCommandList, DrawIndexedInstanced),
    ERVR_LIST_HOOK(14, ID3D12GraphicsCommandList, Dispatch),
    ERVR_LIST_HOOK(15, ID3D12GraphicsCommandList, CopyBufferRegion),
    ERVR_LIST_HOOK(16, ID3D12GraphicsCommandList, CopyTextureRegion),
    ERVR_LIST_HOOK(17, ID3D12GraphicsCommandList, CopyResource),
    ERVR_LIST_HOOK(18, ID3D12GraphicsCommandList, CopyTiles),
    ERVR_LIST_HOOK(19, ID3D12GraphicsCommandList, ResolveSubresource),
    ERVR_LIST_HOOK(20, ID3D12GraphicsCommandList, IASetPrimitiveTopology),
    ERVR_LIST_HOOK(21, ID3D12GraphicsCommandList, RSSetViewports),
    ERVR_LIST_HOOK(22, ID3D12GraphicsCommandList, RSSetScissorRects),
    ERVR_LIST_HOOK(23, ID3D12GraphicsCommandList, OMSetBlendFactor),
    ERVR_LIST_HOOK(24, ID3D12GraphicsCommandList, OMSetStencilRef),
    ERVR_LIST_HOOK(25, ID3D12GraphicsCommandList, SetPipelineState),
    ERVR_LIST_HOOK(26, ID3D12GraphicsCommandList, ResourceBarrier),
    ERVR_LIST_HOOK(27, ID3D12GraphicsCommandList, ExecuteBundle),
    ERVR_LIST_HOOK(28, ID3D12GraphicsCommandList, SetDescriptorHeaps),
    ERVR_LIST_HOOK(29, ID3D12GraphicsCommandList, SetComputeRootSignature),
    ERVR_LIST_HOOK(30, ID3D12GraphicsCommandList, SetGraphicsRootSignature),
    ERVR_LIST_HOOK(31, ID3D12GraphicsCommandList, SetComputeRootDescriptorTable),
    ERVR_LIST_HOOK(32, ID3D12GraphicsCommandList, SetGraphicsRootDescriptorTable),
    ERVR_LIST_HOOK(33, ID3D12GraphicsCommandList, SetComputeRoot32BitConstant),
    ERVR_LIST_HOOK(34, ID3D12GraphicsCommandList, SetGraphicsRoot32BitConstant),
    ERVR_LIST_HOOK(35, ID3D12GraphicsCommandList, SetComputeRoot32BitConstants),
    ERVR_LIST_HOOK(36, ID3D12GraphicsCommandList, SetGraphicsRoot32BitConstants),
    ERVR_LIST_HOOK(37, ID3D12GraphicsCommandList, SetComputeRootConstantBufferView),
    ERVR_LIST_HOOK(38, ID3D12GraphicsCommandList, SetGraphicsRootConstantBufferView),
    ERVR_LIST_HOOK(39, ID3D12GraphicsCommandList, SetComputeRootShaderResourceView),
    ERVR_LIST_HOOK(40, ID3D12GraphicsCommandList, SetGraphicsRootShaderResourceView),
    ERVR_LIST_HOOK(41, ID3D12GraphicsCommandList, SetComputeRootUnorderedAccessView),
    ERVR_LIST_HOOK(42, ID3D12GraphicsCommandList, SetGraphicsRootUnorderedAccessView),
    ERVR_LIST_HOOK(43, ID3D12GraphicsCommandList, IASetIndexBuffer),
    ERVR_LIST_HOOK(44, ID3D12GraphicsCommandList, IASetVertexBuffers),
    ERVR_LIST_HOOK(45, ID3D12GraphicsCommandList, SOSetTargets),
    ERVR_LIST_HOOK(46, ID3D12GraphicsCommandList, OMSetRenderTargets),
    ERVR_LIST_HOOK(47, ID3D12GraphicsCommandList, ClearDepthStencilView),
    ERVR_LIST_HOOK(48, ID3D12GraphicsCommandList, ClearRenderTargetView),
    ERVR_LIST_HOOK(49, ID3D12GraphicsCommandList, ClearUnorderedAccessViewUint),
    ERVR_LIST_HOOK(50, ID3D12GraphicsCommandList, ClearUnorderedAccessViewFloat),
    ERVR_LIST_HOOK(51, ID3D12GraphicsCommandList, DiscardResource),
    ERVR_LIST_HOOK(52, ID3D12GraphicsCommandList, BeginQuery),
    ERVR_LIST_HOOK(53, ID3D12GraphicsCommandList, EndQuery),
    ERVR_LIST_HOOK(54, ID3D12GraphicsCommandList, ResolveQueryData),
    ERVR_LIST_HOOK(55, ID3D12GraphicsCommandList, SetPredication),
    ERVR_LIST_HOOK(56, ID3D12GraphicsCommandList, SetMarker),
    ERVR_LIST_HOOK(57, ID3D12GraphicsCommandList, BeginEvent),
    ERVR_LIST_HOOK(58, ID3D12GraphicsCommandList, EndEvent),
    ERVR_LIST_HOOK(59, ID3D12GraphicsCommandList, ExecuteIndirect),
    ERVR_LIST_HOOK(60, ID3D12GraphicsCommandList1, AtomicCopyBufferUINT),
    ERVR_LIST_HOOK(61, ID3D12GraphicsCommandList1, AtomicCopyBufferUINT64),
    ERVR_LIST_HOOK(62, ID3D12GraphicsCommandList1, OMSetDepthBounds),
    ERVR_LIST_HOOK(63, ID3D12GraphicsCommandList1, SetSamplePositions),
    ERVR_LIST_HOOK(64, ID3D12GraphicsCommandList1, ResolveSubresourceRegion),
    ERVR_LIST_HOOK(65, ID3D12GraphicsCommandList1, SetViewInstanceMask),
    ERVR_LIST_HOOK(66, ID3D12GraphicsCommandList2, WriteBufferImmediate),
    ERVR_LIST_HOOK(67, ID3D12GraphicsCommandList3, SetProtectedResourceSession),
    ERVR_LIST_HOOK(68, ID3D12GraphicsCommandList4, BeginRenderPass),
    ERVR_LIST_HOOK(69, ID3D12GraphicsCommandList4, EndRenderPass),
    ERVR_LIST_HOOK(70, ID3D12GraphicsCommandList4, InitializeMetaCommand),
    ERVR_LIST_HOOK(71, ID3D12GraphicsCommandList4, ExecuteMetaCommand),
    ERVR_LIST_HOOK(72, ID3D12GraphicsCommandList4, BuildRaytracingAccelerationStructure),
    ERVR_LIST_HOOK(73, ID3D12GraphicsCommandList4, EmitRaytracingAccelerationStructurePostbuildInfo),
    ERVR_LIST_HOOK(74, ID3D12GraphicsCommandList4, CopyRaytracingAccelerationStructure),
    ERVR_LIST_HOOK(75, ID3D12GraphicsCommandList4, SetPipelineState1),
    ERVR_LIST_HOOK(76, ID3D12GraphicsCommandList4, DispatchRays),
    ERVR_LIST_HOOK(77, ID3D12GraphicsCommandList5, RSSetShadingRate),
    ERVR_LIST_HOOK(78, ID3D12GraphicsCommandList5, RSSetShadingRateImage),
    ERVR_LIST_HOOK(79, ID3D12GraphicsCommandList6, DispatchMesh),
    ERVR_LIST_HOOK(80, ID3D12GraphicsCommandList7, Barrier),
    ERVR_LIST_HOOK(81, ID3D12GraphicsCommandList8, OMSetFrontAndBackStencilRef),
    ERVR_LIST_HOOK(82, ID3D12GraphicsCommandList9, RSSetDepthBias),
    ERVR_LIST_HOOK(83, ID3D12GraphicsCommandList9, IASetIndexBufferStripCutValue),
    ERVR_LIST_HOOK(84, ID3D12GraphicsCommandList10, SetProgram),
    ERVR_LIST_HOOK(85, ID3D12GraphicsCommandList10, DispatchGraph),
};

// The highest command-list interface the runtime's lists implement: slots past its vtable do not exist.
int ListVtableSlots(ID3D12CommandList* l) {
    struct V { REFIID iid; int slots; };
    const V vs[] = {{__uuidof(ID3D12GraphicsCommandList10), 86}, {__uuidof(ID3D12GraphicsCommandList9), 84},
                    {__uuidof(ID3D12GraphicsCommandList8), 82}, {__uuidof(ID3D12GraphicsCommandList7), 81},
                    {__uuidof(ID3D12GraphicsCommandList6), 80}, {__uuidof(ID3D12GraphicsCommandList5), 79},
                    {__uuidof(ID3D12GraphicsCommandList4), 77}, {__uuidof(ID3D12GraphicsCommandList3), 68},
                    {__uuidof(ID3D12GraphicsCommandList2), 67}, {__uuidof(ID3D12GraphicsCommandList1), 66},
                    {__uuidof(ID3D12GraphicsCommandList), 60}};
    for (const V& v : vs) {
        void* p = nullptr;
        if (SUCCEEDED(l->QueryInterface(v.iid, &p)) && p) {
            ((IUnknown*)p)->Release();
            return v.slots;
        }
    }
    return 0;
}

void HookListVt(ID3D12CommandList* l) {
    if (g_listHooked.exchange(true)) return;
    if (l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) { g_listHooked.store(false); return; }
    const int slots = ListVtableSlots(l);
    void** vt = *(void***)l;
    int ok = 0;
    // Created first, enabled together: one thread freeze for all of them instead of one per method.
    for (const SlotHook& h : kListHooks) {
        if (h.slot >= slots) continue;
        if (MH_CreateHook(vt[h.slot], h.detour, h.orig) == MH_OK && MH_QueueEnableHook(vt[h.slot]) == MH_OK) ++ok;
        else LOGW("[replay] command-list %s (slot %d) not hooked", h.name, h.slot);
    }
    const MH_STATUS applied = MH_ApplyQueued();
    LOGI("[replay] command-list vtable (%d slots): %d of %zu methods hooked (apply %d)", slots, ok,
         sizeof(kListHooks) / sizeof(kListHooks[0]), (int)applied);
    g_listClose = LH<9, decltype(&ID3D12GraphicsCommandList::Close)>::orig;
    g_listReset = LH<10, decltype(&ID3D12GraphicsCommandList::Reset)>::orig;
    // The allocator vtable, for the reset census, from a fresh one: the lists' allocators are the game's.
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        ID3D12CommandAllocator* a = nullptr;
        if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) && a) {
            HookAllocVt(a);
            a->Release();
        }
        dev->Release();
    }
}

using PfnCreateCommitted = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, REFIID, void**);
using PfnCreateCommitted1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                        const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
PfnCreateCommitted g_origCreateCommitted = nullptr;
PfnCreateCommitted1 g_origCreateCommitted1 = nullptr;
HRESULT STDMETHODCALLTYPE HkCreateCommitted(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                            const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
HRESULT STDMETHODCALLTYPE HkCreateCommitted1(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                             const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                             ID3D12ProtectedResourceSession*, REFIID, void**);

// A list made by CreateCommandList is born OPEN and records its first batch without a Reset - the call every
// shadow starts at - so that batch would reach the queue unshadowed and its frame could not be replayed (the game
// makes new lists while it plays). Its shadow starts here instead.
HRESULT STDMETHODCALLTYPE HkCreateList(ID3D12Device* d, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a,
                                       ID3D12PipelineState* pso, REFIID riid, void** pp) {
    const HRESULT hr = g_origCreateList(d, mask, type, a, pso, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && type == D3D12_COMMAND_LIST_TYPE_DIRECT && !t_creatingShadow &&
        g_shadowOn.load(std::memory_order_relaxed) && g_listHooked.load(std::memory_order_relaxed) && IsGameThread() &&
        !t_inErvrPresent) {
        ID3D12GraphicsCommandList* gl = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*pp)->QueryInterface(IID_PPV_ARGS(&gl))) && gl) {
            ShadowBegin(gl, pso);
            gl->Release();
            g_createdOpen.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return hr;
}

std::atomic<bool> g_deviceHooked{false};
void InstallDeviceHooks(ID3D12Device* d) {
    if (!d || g_deviceHooked.exchange(true)) return;
    g_descInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (ReplayLevel() > 0)
        HookSlot(d, 12, (void*)&HkCreateList, (void**)&g_origCreateList, "ID3D12Device::CreateCommandList");
    HookSlot(d, 17, (void*)&HkCreateCbv, (void**)&g_origCreateCbv, "ID3D12Device::CreateConstantBufferView");
    g_skipShadowMap = Cfg().replaySkipShadowMap;
    if (Cfg().replayDepthCensus > 0.f || Cfg().replaySkipShadowMap || PassLogSeconds() > 0)
        HookSlot(d, 21, (void*)&HkCreateDsv, (void**)&g_origCreateDsv, "ID3D12Device::CreateDepthStencilView");
    if (Cfg().replayCensus) {   // the descriptor-slot map (census): SRV/UAV forget a slot, copies carry it
        HookSlot(d, 18, (void*)&HkCreateSrv, (void**)&g_origCreateSrv, "ID3D12Device::CreateShaderResourceView");
        HookSlot(d, 19, (void*)&HkCreateUav, (void**)&g_origCreateUav, "ID3D12Device::CreateUnorderedAccessView");
        HookSlot(d, 23, (void*)&HkCopyDescs, (void**)&g_origCopyDescs, "ID3D12Device::CopyDescriptors");
        HookSlot(d, 24, (void*)&HkCopySimple, (void**)&g_origCopySimple, "ID3D12Device::CopyDescriptorsSimple");
    }
    if (ReplayLevel() >= 3) {   // the constants ring is created at boot: these must be in before it
        HookSlot(d, 27, (void*)&HkCreateCommitted, (void**)&g_origCreateCommitted, "ID3D12Device::CreateCommittedResource");
        ID3D12Device4* d4 = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d4))) && d4) {
            HookSlot(d4, 53, (void*)&HkCreateCommitted1, (void**)&g_origCreateCommitted1, "ID3D12Device4::CreateCommittedResource1");
            d4->Release();
        }
    }
}

void InstallQueueHooks(ID3D12CommandQueue* q) {
    g_gameQueue.store(q);   // before the hooks: they sort every submission by it
    HookSlot(q, 10, (void*)&HkExec, (void**)&g_origExec, "ID3D12CommandQueue::ExecuteCommandLists");
    HookSlot(q, 14, (void*)&HkSignal, (void**)&g_origSignal, "ID3D12CommandQueue::Signal");
    HookSlot(q, 15, (void*)&HkWait, (void**)&g_origWait, "ID3D12CommandQueue::Wait");
}

const char* QName(uint8_t t) {
    return t == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : t == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute"
         : t == D3D12_COMMAND_LIST_TYPE_COPY ? "copy" : "other";
}

void DumpQueueCensus(int n, DWORD frameThread) {
    static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (double)x.QuadPart; }();
    if (n > kEv) n = kEv;
    LOGI("[replay] ---- submission census: %d events over 6 presents (frame thread %lu) ----", n, frameThread);
    const long long t0 = n ? g_ev[0].t : 0;
    for (int i = 0; i < n; ++i) {
        const Ev& e = g_ev[i];
        char line[320];
        const double ms = (double)(e.t - t0) * 1000.0 / f;
        const std::string th = ThreadLabel(e.tid);
        switch (e.kind) {
        case EvExec:
            snprintf(line, sizeof(line), "+%8.3f %-22s %sEXEC   %s q%p n=%llu [%p %p %p]", ms, th.c_str(), e.ervr ? "(ervr) " : "",
                     QName(e.qtype), e.a, (unsigned long long)e.v, e.lists[0], e.lists[1], e.lists[2]);
            break;
        case EvSignal:
            snprintf(line, sizeof(line), "+%8.3f %-22s %sSIGNAL %s q%p fence %p = %llu", ms, th.c_str(), e.ervr ? "(ervr) " : "",
                     QName(e.qtype), e.a, e.b, (unsigned long long)e.v);
            break;
        case EvWait:
            snprintf(line, sizeof(line), "+%8.3f %-22s %sWAIT   %s q%p fence %p >= %llu", ms, th.c_str(), e.ervr ? "(ervr) " : "",
                     QName(e.qtype), e.a, e.b, (unsigned long long)e.v);
            break;
        case EvCpuWait:
            snprintf(line, sizeof(line), "+%8.3f %-22s CPU waits on fence %p >= %llu", ms, th.c_str(), e.a, (unsigned long long)e.v);
            break;
        case EvListReset:
            snprintf(line, sizeof(line), "+%8.3f %-22s list %p reset (allocator %p)", ms, th.c_str(), e.a, e.b);
            break;
        case EvAllocReset:
            snprintf(line, sizeof(line), "+%8.3f %-22s allocator %p reset", ms, th.c_str(), e.a);
            break;
        case EvPresent:
            snprintf(line, sizeof(line), "+%8.3f %-22s ======== PRESENT (queue %p) ========", ms, th.c_str(), e.a);
            break;
        }
        LOGI("[census] %s", line);
    }
    LOGI("[replay] ---- census end ----");
}

// Where, in `n` bytes at `p`, three consecutive floats sit within `tol` of v (byte offsets, up to 8).
int FindVec3(const uint8_t* p, size_t n, const float v[3], float tol, int* offs, int maxOut) {
    int k = 0;
    for (size_t o = 0; o + 12 <= n && k < maxOut; o += 4) {
        float f[3];
        memcpy(f, p + o, 12);
        if (std::fabs(f[0] - v[0]) < tol && std::fabs(f[1] - v[1]) < tol && std::fabs(f[2] - v[2]) < tol) offs[k++] = (int)o;
    }
    return k;
}

long g_descBase[5];   // descriptor counters at the census frame's start

void DumpTableCensus(const float cam[16], bool haveCam) {
    LOGI("[desc] this frame: %ld CBV creates, %ld SRV/UAV creates, %ld simple copies, %ld multi copies, %ld descriptors copied",
         g_cbvCreates.load() - g_descBase[0], g_srvUavCreates.load() - g_descBase[1], g_copiesSimple.load() - g_descBase[2],
         g_copiesMulti.load() - g_descBase[3], g_descCopied.load() - g_descBase[4]);
    {
        std::lock_guard<std::mutex> g(g_heapMtx);
        for (auto& [h, hi] : g_heaps)
            LOGI("[desc] heap %p type %d %s, %u descriptors, cpu %llx gpu %llx", (void*)h, (int)hi.type,
                 hi.gpu ? "shader-visible" : "staging", hi.num, (unsigned long long)hi.cpu, (unsigned long long)hi.gpu);
    }
    const int n = std::min(g_tblN.load(), kTbl);
    struct Ref { uint64_t va; uint32_t size; long refs = 0; void* sig; int root, slot, compute; };
    std::map<uint64_t, Ref> vas;                           // unique CBV addresses tables reach
    struct Group { long n = 0; std::map<uint64_t, long> handles; std::string pattern; };
    std::map<std::tuple<void*, int, int>, Group> groups;
    {
        std::lock_guard<std::mutex> gh(g_heapMtx);
        std::lock_guard<std::mutex> gd(g_descMtx);
        for (int i = 0; i < n; ++i) {
            const TblRec& r = g_tbl[i];
            Group& grp = groups[{(void*)r.sig, (int)r.root, (int)r.compute}];
            ++grp.n;
            if (grp.handles.size() < 4096) ++grp.handles[r.gpu];
            auto hit = g_heaps.find(r.heap);
            if (hit == g_heaps.end() || !hit->second.gpu || r.gpu < hit->second.gpu) continue;
            const uint64_t cpu = hit->second.cpu + (r.gpu - hit->second.gpu);
            std::string pat;
            for (int k = 0; k < 32; ++k) {
                auto c = g_cbvAt.find(cpu + (uint64_t)k * g_descInc);
                if (c == g_cbvAt.end()) continue;
                if (grp.pattern.empty()) { char b[16]; snprintf(b, sizeof(b), " %d", k); pat += b; }
                Ref& ref = vas[c->second.va];
                if (!ref.refs) ref = {c->second.va, c->second.size, 0, (void*)r.sig, (int)r.root, k, (int)r.compute};
                ++ref.refs;
            }
            if (grp.pattern.empty()) grp.pattern = pat.empty() ? " none" : pat;
        }
    }
    LOGI("[desc] %d table bindings in %zu (root signature, index) groups reach %zu distinct constant buffers (CBV slots "
         "looked for in the 32 descriptors after each table start)", n, groups.size(), vas.size());
    for (auto& [k, grp] : groups)
        LOGI("[desc] %s sig %p root %d: %ld binds, %zu%s tables, CBV slots in the first:%s", std::get<2>(k) ? "compute " : "graphics",
             std::get<0>(k), std::get<1>(k), grp.n, grp.handles.size(), grp.handles.size() >= 4096 ? "+" : "", grp.pattern.c_str());
    const float pos[3] = {cam[12], cam[13], cam[14]};
    const float fwd[3] = {cam[8], cam[9], cam[10]};
    const float rgt[3] = {cam[0], cam[1], cam[2]};
    int dumped = 0, matched = 0;
    for (auto& [va, ref] : vas) {
        uint64_t avail = 0;
        const uint8_t* p = CpuOf(va, 16, &avail);
        if (!p || !haveCam) continue;
        const size_t scan = (size_t)std::min<uint64_t>(std::min<uint64_t>(avail, ref.size ? ref.size : 4096), 4096);
        int po[8], fo[8], ro[8];
        const int np = FindVec3(p, scan, pos, 0.05f, po, 8), nf = FindVec3(p, scan, fwd, 0.002f, fo, 8),
                  nr = FindVec3(p, scan, rgt, 0.002f, ro, 8);
        if (!np && !nf && !nr) continue;
        ++matched;
        char hits[220] = {};
        int w = 0;
        for (int i = 0; i < np; ++i) w += snprintf(hits + w, sizeof(hits) - w, " pos@%d", po[i]);
        for (int i = 0; i < nf; ++i) w += snprintf(hits + w, sizeof(hits) - w, " fwd@%d", fo[i]);
        for (int i = 0; i < nr; ++i) w += snprintf(hits + w, sizeof(hits) - w, " right@%d", ro[i]);
        LOGI("[desc] CAMERA? cb %llx size %u, %ld table refs (first: %s sig %p root %d slot %d):%s", (unsigned long long)va,
             ref.size, ref.refs, ref.compute ? "compute" : "graphics", ref.sig, ref.root, ref.slot, hits);
        if (dumped < 6) {
            ++dumped;
            const size_t dump = (size_t)std::min<uint64_t>(std::min<uint64_t>(avail, ref.size ? ref.size : 512), 512);
            for (size_t o = 0; o < dump; o += 64) {
                float f[16] = {};
                memcpy(f, p + o, std::min<size_t>(64, dump - o));
                LOGI("[desc]   +%03zu: %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f",
                     o, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            }
        }
    }
    LOGI("[desc] %d of %zu constant buffers hold the camera's position/axes", matched, vas.size());
}

// One frame's created constant-buffer views: how many of each size, and which carry the camera.
void DumpFrameCbvCensus(int frame) {
    float cam[16] = {}, fov = 0.f;
    if (!GameERRenderCamera(cam, &fov)) return;
    const float pos[3] = {cam[12], cam[13], cam[14]}, fwd[3] = {cam[8], cam[9], cam[10]}, rgt[3] = {cam[0], cam[1], cam[2]};
    const int n = std::min(g_frameCbvN.load(), kFrameCbv);
    std::map<uint32_t, long> bySize;
    std::map<uint64_t, uint32_t> unique;
    std::map<uint64_t, std::pair<uint64_t, DWORD>> where;   // va -> (descriptor handle, creating thread) of its first view
    std::map<uint64_t, long> byHandleBlock;                  // descriptor handle >> 20 -> views created there
    for (int i = 0; i < n; ++i) {
        const FrameCbv& c = g_frameCbv[i];
        ++bySize[c.size];
        unique[c.va] = c.size;
        where.emplace(c.va, std::make_pair(c.handle, c.tid));
        ++byHandleBlock[c.handle >> 20];
    }
    std::string hist;
    for (auto& [sz, c] : bySize) { char b[32]; snprintf(b, sizeof(b), " %u:%ld", sz, c); hist += b; }
    LOGI("[cbvframe %d] %d CBVs created (%zu distinct addresses); by size:%s", frame, n, unique.size(), hist.c_str());
    std::string blocks;
    for (auto& [b, c] : byHandleBlock) { char s[48]; snprintf(s, sizeof(s), " %llx..:%ld", (unsigned long long)(b << 20), c); blocks += s; }
    LOGI("[cbvframe %d] descriptor handle blocks (1 MB):%s", frame, blocks.c_str());
    // How expensive is reading upload memory? Time one 16-byte read from every distinct view.
    {
        LARGE_INTEGER t0, t1, qf; QueryPerformanceCounter(&t0);
        volatile float sink = 0.f; int reads = 0;
        for (auto& [va, sz] : unique) {
            if (const uint8_t* p = CpuOf(va, 16)) { float v[4]; memcpy(v, p, 16); sink = sink + v[0]; ++reads; }
        }
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&qf);
        LOGI("[cbvframe %d] one 16-byte read from each of %d views took %.3f ms", frame, reads,
             (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart);
    }
    int matched = 0;
    for (auto& [va, sz] : unique) {
        if (sz > 4096) continue;
        uint64_t avail = 0;
        const uint8_t* p = CpuOf(va, 16, &avail);
        if (!p) continue;
        const size_t scan = (size_t)std::min<uint64_t>(avail, sz);
        int po[8], fo[8], ro[8];
        const int np = FindVec3(p, scan, pos, 0.05f, po, 8), nf = FindVec3(p, scan, fwd, 0.002f, fo, 8),
                  nr = FindVec3(p, scan, rgt, 0.002f, ro, 8);
        if (!np && !nf && !nr) continue;
        char hits[200] = {};
        int w = 0;
        for (int i = 0; i < np; ++i) w += snprintf(hits + w, sizeof(hits) - w, " pos@%d", po[i]);
        for (int i = 0; i < nf; ++i) w += snprintf(hits + w, sizeof(hits) - w, " fwd@%d", fo[i]);
        for (int i = 0; i < nr; ++i) w += snprintf(hits + w, sizeof(hits) - w, " right@%d", ro[i]);
        const auto& wh = where[va];
        if (++matched <= 40)
            LOGI("[cbvframe %d] camera in cb %llx size %u (view at %llx by %s):%s", frame, (unsigned long long)va, sz,
                 (unsigned long long)wh.first, ThreadLabel(wh.second).c_str(), hits);
    }
    LOGI("[cbvframe %d] %d CBVs hold the camera", frame, matched);
}

void DumpListCensus() {
    LOGI("[replay] ---- command-list census, one frame (game recording threads; ERVR's own excluded) ----");
    std::string line;
    for (const SlotHook& h : kListHooks) {
        const long n = g_calls[h.slot].load();
        if (!n) continue;
        char b[80];
        snprintf(b, sizeof(b), " %s(%d)=%ld", h.name, h.slot, n);
        line += b;
        if (line.size() > 180) { LOGI("[lists]%s", line.c_str()); line.clear(); }
    }
    if (!line.empty()) LOGI("[lists]%s", line.c_str());
    line.clear();
    for (int i = 0; i < g_tcN.load() && i < 64; ++i) {
        char b[96];
        snprintf(b, sizeof(b), " [%s] %ld", ThreadLabel(g_tc[i].tid).c_str(), g_tc[i].n.load());
        line += b;
    }
    LOGI("[lists] calls by thread:%s", line.c_str());

    // Root constant-buffer bindings, grouped by (root signature, root index, graphics/compute).
    float cam[16] = {}, fov = 0.f;
    const bool haveCam = GameERRenderCamera(cam, &fov);
    LOGI("[cbv] render camera: right (%.4f %.4f %.4f) up (%.4f %.4f %.4f) fwd (%.4f %.4f %.4f) pos (%.4f %.4f %.4f) fov %.4f%s",
         cam[0], cam[1], cam[2], cam[4], cam[5], cam[6], cam[8], cam[9], cam[10], cam[12], cam[13], cam[14], fov,
         haveCam ? "" : " (NOT READ)");
    struct Key { ID3D12RootSignature* sig; uint16_t root; uint8_t compute; };
    struct Group { Key k; long n = 0; std::map<uint64_t, long> vas; };
    std::map<std::tuple<void*, int, int>, Group> groups;
    const int n = std::min(g_cbvN.load(), kCbv);
    for (int i = 0; i < n; ++i) {
        const CbvRec& r = g_cbv[i];
        Group& g = groups[{(void*)r.sig, (int)r.root, (int)r.compute}];
        g.k = {r.sig, r.root, r.compute};
        ++g.n;
        if (g.vas.size() < 4096) ++g.vas[r.va];
    }
    LOGI("[cbv] %d root CBV bindings in %zu (root signature, index) groups", n, groups.size());
    const float pos[3] = {cam[12], cam[13], cam[14]};
    const float fwd[3] = {cam[8], cam[9], cam[10]};
    const float rgt[3] = {cam[0], cam[1], cam[2]};
    for (auto& [key, g] : groups) {
        // The address bound most often in the group, and how many distinct ones.
        uint64_t topVa = 0; long topN = 0;
        for (auto& [va, c] : g.vas) if (c > topN) { topN = c; topVa = va; }
        uint64_t avail = 0;
        const uint8_t* p = CpuOf(topVa, 16, &avail);
        int posOff[8], fwdOff[8], rgtOff[8];
        const size_t scan = p ? (size_t)std::min<uint64_t>(avail, 4096) : 0;
        const int np = p && haveCam ? FindVec3(p, scan, pos, 0.05f, posOff, 8) : 0;
        const int nf = p && haveCam ? FindVec3(p, scan, fwd, 0.002f, fwdOff, 8) : 0;
        const int nr = p && haveCam ? FindVec3(p, scan, rgt, 0.002f, rgtOff, 8) : 0;
        char hits[200] = {};
        int w = 0;
        for (int i = 0; i < np; ++i) w += snprintf(hits + w, sizeof(hits) - w, " pos@%d", posOff[i]);
        for (int i = 0; i < nf; ++i) w += snprintf(hits + w, sizeof(hits) - w, " fwd@%d", fwdOff[i]);
        for (int i = 0; i < nr; ++i) w += snprintf(hits + w, sizeof(hits) - w, " right@%d", rgtOff[i]);
        LOGI("[cbv] %s sig %p root %d: %ld binds, %zu%s addresses, top %llx x%ld%s%s", g.k.compute ? "compute " : "graphics",
             (void*)g.k.sig, (int)g.k.root, g.n, g.vas.size(), g.vas.size() >= 4096 ? "+" : "",
             (unsigned long long)topVa, topN, p ? "" : " (not a tracked CPU buffer)", hits);
        if (p && (np || nf || nr)) {
            // The camera lives here: dump the block (first 512 bytes) for the offline layout read.
            const size_t dump = (size_t)std::min<uint64_t>(avail, 512);
            for (size_t o = 0; o < dump; o += 64) {
                float f[16] = {};
                memcpy(f, p + o, std::min<size_t>(64, dump - o));
                LOGI("[cbv]   +%03zu: %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f",
                     o, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            }
        }
    }
    DumpTableCensus(cam, haveCam);
    LOGI("[replay] ---- command-list census end ----");
}

// ---- the replay: this frame's shadows, executed again right before the present ------------------------
ID3D12Fence* g_replayFence = nullptr;
uint64_t g_replayFenceVal = 0;
bool g_presentPending = false;
bool g_batchHeld = false;        // the presented frame's fence signals were held (it may be replayed)   // stereo mode: this present's frame awaits ReplayStereoSubmitSecondEye (present thread)

// ---- camera patch (ReplayStereo=2 experiment): the frame's constants moved to the second eye -----------
// Generic over the buffer's layout: every 16-byte row built from a camera axis a with a translation that
// is a's projection of the camera position (view and view-projection rows: s*a, s*(-a.p)) gets the second
// eye's translation; every inverse-view row (r_i, u_i, f_i, p_i) its position component; every copy of the
// position itself the second eye's. Rotation is shared by the two eyes, so nothing else changes.
struct CamPatch { float r[3], u[3], f[3], p[3], pr[3]; };
std::atomic<long> g_patchedRows{0}, g_patchedPos{0}, g_patchedBuffers{0};

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// One buffer: read with streaming loads (write-combined memory), patch, write back only the rows that changed.
int PatchBuffer(uint8_t* p, uint32_t size, const CamPatch& c) {
    alignas(16) float buf[1024];
    const uint32_t n = std::min<uint32_t>(size, sizeof(buf)) & ~15u;
    if (!n || ((uintptr_t)p & 15)) return 0;
    for (uint32_t o = 0; o < n; o += 16)
        _mm_store_si128((__m128i*)((uint8_t*)buf + o), _mm_stream_load_si128((__m128i*)(p + o)));
    const float* axes[3] = {c.r, c.u, c.f};
    int changed = 0;
    // Rows (16-byte aligned).
    for (uint32_t o = 0; o < n; o += 16) {
        float* row = buf + o / 4;
        const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (len < 1e-4f) continue;
        bool done = false;
        for (int k = 0; k < 3 && !done; ++k) {
            const float s = Dot3(row, axes[k]);
            if (std::fabs(std::fabs(s) - len) > 1e-4f * len) continue;           // not parallel to this axis
            const float wL = -s * Dot3(axes[k], c.p);
            if (std::fabs(row[3] - wL) > 1e-3f * std::max(1.f, std::fabs(wL))) continue;   // translation is not the camera's
            row[3] = -s * Dot3(axes[k], c.pr);
            _mm_storeu_ps((float*)(p + o), _mm_load_ps(row));
            ++changed; g_patchedRows.fetch_add(1, std::memory_order_relaxed);
            done = true;
        }
        if (done) continue;
        for (int i = 0; i < 3; ++i) {                                              // inverse-view row i
            if (std::fabs(row[0] - c.r[i]) < 1e-4f && std::fabs(row[1] - c.u[i]) < 1e-4f &&
                std::fabs(row[2] - c.f[i]) < 1e-4f && std::fabs(row[3] - c.p[i]) < 1e-3f * std::max(1.f, std::fabs(c.p[i]))) {
                row[3] = c.pr[i];
                _mm_storeu_ps((float*)(p + o), _mm_load_ps(row));
                ++changed; g_patchedRows.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    // Positions (any 4-byte alignment).
    for (uint32_t o = 0; o + 12 <= n; o += 4) {
        float* v = buf + o / 4;
        if (std::fabs(v[0] - c.p[0]) < 1e-4f * std::max(1.f, std::fabs(c.p[0])) &&
            std::fabs(v[1] - c.p[1]) < 1e-4f * std::max(1.f, std::fabs(c.p[1])) &&
            std::fabs(v[2] - c.p[2]) < 1e-4f * std::max(1.f, std::fabs(c.p[2]))) {
            memcpy(p + o, c.pr, 12);
            ++changed; g_patchedPos.fetch_add(1, std::memory_order_relaxed);
            o += 8;
        }
    }
    return changed;
}

// ---- the GPU patch (ReplayStereo=3): the same rules as PatchBuffer, run by a compute shader --------------
// The game keeps every per-frame constant in one 160 MB upload ring (measured 2026-11-12: committed, GPU
// va fa00000). Upload memory is not GPU-writable, so the ring is created instead in a custom heap with the
// same CPU behaviour (write-combined system memory) that also allows UAV writes; between the first run and
// the replay a compute pass patches the camera rows of exactly the views the frame created.
struct Ring { ID3D12Resource* res = nullptr; uint64_t gpu = 0, size = 0; };
Ring g_ring;


// Constants the game stages in the ring and then copies into GPU-only buffers of its own inside the frame's lists. The
// 2026-11-13 pass log: the sun's screen-space cascaded shadow (GXScreenSpaceCascadedShadow_PCF4) reads cbPerScene - 560 B,
// its four view-space cascade matrices at +64 - from a default-heap buffer that a CopyBufferRegion fills from the ring
// right before each draw; the light accumulation's cbLightAccParam (384 B, view and camera matrices) the same way. The
// staging region is no view, so the patch never saw it and the replayed copy handed the right eye the left eye's
// shadow matrices: the right eye's sun shadows sampled an eye-width off ("the shadows are offset between eyes"). Each
// small copy out of the ring joins the frame's views; the replayed copy then carries the patched values.
// Only copies in lists the replay runs again: the game's copy-queue lists (streaming) never replay, and the patch must not
// touch what they read.
std::atomic<long> g_copySources{0}, g_copySourcesElsewhere{0};
void NoteCopySource(ID3D12Resource* src, uint64_t off, uint64_t n, bool replayed) {
    if (!src || src != g_ring.res || n < 64 || n > 4096 || (off & 15) || !g_frameCbvArmed.load(std::memory_order_relaxed))
        return;
    if (!replayed) { g_copySourcesElsewhere.fetch_add(1, std::memory_order_relaxed); return; }
    const int i = g_frameCbvN.fetch_add(1, std::memory_order_relaxed);
    if (i < kFrameCbv) g_frameCbv[i] = {g_ring.gpu + off, (uint32_t)n, 0, GetCurrentThreadId()};
    g_copySources.fetch_add(1, std::memory_order_relaxed);
}

bool IsTheRing(const D3D12_HEAP_PROPERTIES* hp, const D3D12_RESOURCE_DESC* d) {
    return !g_ring.res && hp && d && hp->Type == D3D12_HEAP_TYPE_UPLOAD && d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
           d->Width >= (64ull << 20);
}
void NoteRing(REFIID riid, void** out, uint64_t width) {
    if (!out || !*out) return;
    ID3D12Resource* r = nullptr;
    if (FAILED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&r))) || !r) return;
    g_ring.res = r;   // the game owns it for the whole session; our reference keeps the pointer honest
    g_ring.gpu = r->GetGPUVirtualAddress();
    g_ring.size = width;
    LOGI("[replay] the constants ring (%llu MB at gpu %llx) is GPU-writable: custom heap, write-combined system memory, UAV",
         (unsigned long long)(width >> 20), (unsigned long long)g_ring.gpu);
    (void)riid;
}
HRESULT STDMETHODCALLTYPE HkCreateCommitted(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                            const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
                                            const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    if (!IsTheRing(hp, d)) return g_origCreateCommitted(dev, hp, hf, d, st, cv, riid, out);
    D3D12_HEAP_PROPERTIES custom{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0,
                                 hp->CreationNodeMask, hp->VisibleNodeMask};
    D3D12_RESOURCE_DESC desc = *d;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = g_origCreateCommitted(dev, &custom, hf, &desc, st, cv, riid, out);
    if (SUCCEEDED(hr)) NoteRing(riid, out, d->Width);
    else { LOGW("[replay] GPU-writable ring refused (0x%08lx): kept as upload", (unsigned long)hr);
           hr = g_origCreateCommitted(dev, hp, hf, d, st, cv, riid, out); }
    return hr;
}
HRESULT STDMETHODCALLTYPE HkCreateCommitted1(ID3D12Device4* dev, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                             const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
                                             const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid, void** out) {
    if (!IsTheRing(hp, d)) return g_origCreateCommitted1(dev, hp, hf, d, st, cv, ps, riid, out);
    D3D12_HEAP_PROPERTIES custom{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0,
                                 hp->CreationNodeMask, hp->VisibleNodeMask};
    D3D12_RESOURCE_DESC desc = *d;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = g_origCreateCommitted1(dev, &custom, hf, &desc, st, cv, ps, riid, out);
    if (SUCCEEDED(hr)) NoteRing(riid, out, d->Width);
    else { LOGW("[replay] GPU-writable ring refused (0x%08lx): kept as upload", (unsigned long)hr);
           hr = g_origCreateCommitted1(dev, hp, hf, d, st, cv, ps, riid, out); }
    return hr;
}

// R/U/F/P: the camera the engine reported at the frame's end; D: the second eye's offset in the world. Matching
// the frame's camera rows needs the camera the frame was DRAWN with, to ~0.8 deg and ~1 mm: the frame-end camera
// is that only while the head holds still (2026-11-13, first played test: near things "huge, then normal, and
// back" - turning the head left the rows unmatched, the right eye drawn from the left eye's spot). So CSFind takes
// the camera from the frame's own scene constants first, and CSPatch matches against that.
const char kPatchHLSL[] = R"(
cbuffer Cam : register(b0) { float4 R; float4 U; float4 F; float4 P; float4 D; uint ViewCount; uint SlotOff; uint2 Pad; };
ByteAddressBuffer Views : register(t0);     // (byte offset in the ring, size) per view
RWByteAddressBuffer Ring : register(u0);
// Per slot, 256 bytes: +0 camera found, +4 view rows patched, +8 inverse-view rows patched, +12 positions patched,
// +16 column-stored view translations patched, +20 inverse view-projections patched; the frame's own camera:
// +32 right, +48 up, +64 forward, +80 position, +96 its projection (4 rows).
RWByteAddressBuffer Stat : register(u1);
// Every value the patch replaces, as (ring offset, the game's value): CSRestore puts them back once the replay has run.
// Some of the game's constant blocks outlive the frame they were written for - while the camera holds still the game
// does not rewrite them - so a patched value left in place is read by the NEXT frame, the left eye's, as its own
// (2026-11-13: "the shadow in my left eye is offset to the left"; frame dumps: blocks holding the view matrix of the
// camera plus the replay's offset). +192 entries this frame, +196 view-space matrices moved (CSFold).
RWByteAddressBuffer Undo : register(u2);
// One bit per ring dword: patched this frame. The game binds most blocks through several views at once (2026-11-13:
// ~3-7 per block), and a rule that moves a value by an offset must move it once. CSRestore clears the bits it set.
RWByteAddressBuffer Mark : register(u3);
#define UNDO_CAP 32768
bool Near(float a, float b, float rel) { return abs(a - b) <= rel * max(1.0, abs(b)); }
// Bench bisection (CSPatch only; Pad.x = size classes whose camera positions are left alone): 1 256 B, 2 512, 4 768,
// 8 1024, 16 1280-1536, 32 2048, 64 3072-3328, 128 4352, 256 6144, 512 12288-13312, 1024 26624, 2048 65536, 4096 other.
// ... and within 2048 B blocks, by the position's offset (Pad.x bits 16-21): +48, +448, +1136, +1568, +1968, other.
uint Offset2048Bit(uint off) {
    return off == 48 ? 0x10000u : off == 448 ? 0x20000u : off == 1136 ? 0x40000u : off == 1568 ? 0x80000u : off == 1968 ? 0x100000u : 0x200000u;
}
uint SizeClass(uint size) {
    return size == 256 ? 1u : size == 512 ? 2u : size == 768 ? 4u : size == 1024 ? 8u : (size == 1280 || size == 1536) ? 16u :
           size == 2048 ? 32u : (size == 3072 || size == 3328) ? 64u : size == 4352 ? 128u : size == 6144 ? 256u :
           (size == 12288 || size == 13312) ? 512u : size == 26624 ? 1024u : size == 65536 ? 2048u : 4096u;
}

// One patched dword, once a frame, logged for CSRestore - the log only ever holds the game's own values.
// Every patched value is a float: an infinity or NaN is never written, nor written over (2026-11-13 census: rows of the
// game's "no limit" sentinel, +-3.4e38, overflowed the rules' length tests and came out +-inf in ~26 places a frame).
bool Finite(uint bits) { return (bits & 0x7f800000u) != 0x7f800000u; }
void Put(uint addr, uint val) {
    if (!Finite(val) || !Finite(Ring.Load(addr))) return;
    uint w = addr >> 2, bit = 1u << (w & 31), was;
    Mark.InterlockedOr((w >> 5) * 4, bit, was);
    if (was & bit) return;       // another view over the same bytes patched it already
    uint i;
    Stat.InterlockedAdd(SlotOff + 192, 1, i);
    if (i >= UNDO_CAP) { Mark.InterlockedAnd((w >> 5) * 4, ~bit, was); return; }   // no room to undo it: not patched
    Undo.Store2(i * 8, uint2(addr, Ring.Load(addr)));
    Ring.Store(addr, val);
}
void Put3(uint addr, uint3 v) { Put(addr, v.x); Put(addr + 4, v.y); Put(addr + 8, v.z); }
void Put4(uint addr, uint4 v) { Put(addr, v.x); Put(addr + 4, v.y); Put(addr + 8, v.z); Put(addr + 12, v.w); }

[numthreads(1, 1, 1)]
void CSClear() {
    Stat.Store4(SlotOff, uint4(0, 0, 0, 0)); Stat.Store4(SlotOff + 16, uint4(0, 0, 0, 0));
    Stat.Store4(SlotOff + 160, uint4(0, 0, 0, 0)); Stat.Store4(SlotOff + 176, uint4(0, 0, 0, 0));
    Stat.Store4(SlotOff + 192, uint4(0, 0, 0, 0));
    Stat.Store(SlotOff + 1008, 0);
}

// After the replay: the game's values back where the patch changed them.
[numthreads(64, 1, 1)]
void CSRestore(uint3 dt : SV_DispatchThreadID) {
    uint n = min(Stat.Load(SlotOff + 192), UNDO_CAP);
    if (dt.x >= n) return;
    uint2 e = Undo.Load2(dt.x * 8);
    if (Pad.x == 0) Ring.Store(e.x, e.y);   // Pad.x: the matrix census's frame - its second look reads the patched values
    uint w = e.x >> 2, was;
    Mark.InterlockedAnd((w >> 5) * 4, ~(1u << (w & 31)), was);
}

)"
R"(bool Close4(float4 a, float4 b, float floorv) { return all(abs(a - b) <= 2e-4 * max(abs(b), floorv)); }

// View-space matrices the game builds from a world-space one it keeps in the same block: M = W . invV - the sun's
// shadow lookup (2048 B blocks: W at +576, M at +688; also 256/512/6144/13312 B, always 112 bytes apart) and the
// decal and light-volume boxes (768 B: W at +64, M at +432). The lighting rebuilds a pixel's position in VIEW space
// and maps it with M, so the right eye needs M . T(Dv), Dv the eye offset in view space; left alone, its shadows were
// sampled an eye-width off (2026-11-13 bench capture: the replay's shadows 1 m off at a 1 m offset). Found by the pair:
// M . V equals another affine matrix within 512 bytes of it in the same view.
)"
R"(// The light's other cascades sit in the same block in view form only (2048 B lighting block: cascade 0 twinned at
// +576/+688, the others at +752, +816, +880 and the near one at +960 - same basis, other scales): every affine matrix
// of the view whose rows are parallel to a twinned one's is moved the same way.
groupshared float3 gsBasis[4][3];
groupshared uint gsBasisN;
groupshared uint gsTwin[16];
groupshared uint gsTwinN;

// Finite and of a sane size only: rows of infinities pass any parallel test, and a fold built on them wrote inf/NaN into
// the game's constants (2026-11-13 frame dumps).
bool Sane3(float3 a) { return all(abs(a) < 1e8) && length(a) > 1e-9; }
bool Parallel(float3 a, float3 b) { return Sane3(a) && Sane3(b) && abs(dot(a, b)) >= 0.99995 * length(a) * length(b); }
// Rows each along a unit axis (any order, any scale): identity-like transforms - view-to-view ones (culling, occlusion),
// world-aligned boxes and volumes. Never a light's view-space basis and never folded: 2026-11-13 census, an identity
// adopted as a basis moved ~990 values a frame (the occlusion-culling matrix among them) - flicker and speckles.
bool AxisRow(float3 a) { float3 n = abs(a) / max(length(a), 1e-30); return max(n.x, max(n.y, n.z)) > 0.99995; }
bool AxisAligned(float3 a, float3 b, float3 c) { return AxisRow(a) && AxisRow(b) && AxisRow(c); }
#define CHUNK_BIT 0x80000000u   // a view entry that is a span chunk (see GpuPatchList), not a view the game made
#define BASES 64                // frame-wide view-space bases (CSFold -> CSFoldAll)

[numthreads(64, 1, 1)]
void CSFold(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint base = 0, size = 0;   // no early return: every thread reaches the group barriers below
    if (gid.x < ViewCount && Stat.Load(SlotOff) != 0 && !(Pad.y & 32)) {
        uint2 v = Views.Load2(gid.x * 8);
        base = v.x;
        size = (v.y & CHUNK_BIT) == 0 && v.y >= 128 ? v.y : 0;   // views only: the fold is not for raw ring chunks
    }
    if (tid.x == 0) { gsBasisN = 0; gsTwinN = 0; }
    GroupMemoryBarrierWithGroupSync();
    float3 a0 = asfloat(Stat.Load3(SlotOff + 32)), a1 = asfloat(Stat.Load3(SlotOff + 48)), a2 = asfloat(Stat.Load3(SlotOff + 64));
    float3 cp = asfloat(Stat.Load3(SlotOff + 80));
    float4 v0 = float4(a0, -dot(a0, cp)), v1 = float4(a1, -dot(a1, cp)), v2 = float4(a2, -dot(a2, cp));
    float3 dv = float3(dot(a0, D.xyz), dot(a1, D.xyz), dot(a2, D.xyz)) * (D.w > 0 ? D.w : 1.0);   // D.w: probe gain
    uint folds = 0;
    for (uint o = tid.x * 16; o + 64 <= size; o += 64 * 16) {
        float4 m3 = asfloat(Ring.Load4(base + o + 48));
        if (any(abs(m3 - float4(0, 0, 0, 1)) > 1e-6)) continue;
        float4 m0 = asfloat(Ring.Load4(base + o)), m1 = asfloat(Ring.Load4(base + o + 16)), m2 = asfloat(Ring.Load4(base + o + 32));
        if (!Sane3(m0.xyz) || !Sane3(m1.xyz) || !Sane3(m2.xyz) || any(abs(float3(m0.w, m1.w, m2.w)) >= 1e8)) continue;
        if (AxisAligned(m0.xyz, m1.xyz, m2.xyz)) continue;
        float4 k0 = m0.x * v0 + m0.y * v1 + m0.z * v2 + float4(0, 0, 0, m0.w);
        float4 k1 = m1.x * v0 + m1.y * v1 + m1.z * v2 + float4(0, 0, 0, m1.w);
        float4 k2 = m2.x * v0 + m2.y * v1 + m2.z * v2 + float4(0, 0, 0, m2.w);
        // M = invV itself (M . V = identity): the inverse-view rule's.
        if (Close4(k0, float4(1, 0, 0, 0), 1e-3) && Close4(k1, float4(0, 1, 0, 0), 1e-3) && Close4(k2, float4(0, 0, 1, 0), 1e-3)) continue;
        float kmax = max(max(max(abs(k0.x), abs(k0.y)), max(abs(k0.z), abs(k1.x))), max(max(abs(k1.y), abs(k1.z)), max(abs(k2.x), max(abs(k2.y), abs(k2.z)))));
        float fl = max(kmax * 1e-3, 1e-9);
        uint lo = o >= 512 ? o - 512 : 0, hi = min(size - 64, o + 512);
        bool hit = false;
        uint twin = 0;
        for (uint s = lo; s <= hi && !hit; s += 16) {
            if (s == o) continue;
            float4 w3 = asfloat(Ring.Load4(base + s + 48));
            if (any(abs(w3 - float4(0, 0, 0, 1)) > 1e-6)) continue;
            hit = Close4(asfloat(Ring.Load4(base + s)), k0, fl) && Close4(asfloat(Ring.Load4(base + s + 16)), k1, fl) &&
                  Close4(asfloat(Ring.Load4(base + s + 32)), k2, fl);
            twin = s;
        }
        if (hit) {
            Put(base + o + 12, asuint(m0.w + dot(m0.xyz, dv)));
            Put(base + o + 28, asuint(m1.w + dot(m1.xyz, dv)));
            Put(base + o + 44, asuint(m2.w + dot(m2.xyz, dv)));
            ++folds;
            uint bi, ti, gi;
            InterlockedAdd(gsBasisN, 1, bi);
            if (bi < 4) { gsBasis[bi][0] = m0.xyz; gsBasis[bi][1] = m1.xyz; gsBasis[bi][2] = m2.xyz; }
            // Frame-wide too, for CSFoldAll: the same light's cascades also sit in blocks without a world twin.
            const uint ng = min(Stat.Load(SlotOff + 1008), BASES);
            bool known = false;
            for (uint q = 0; q < ng && !known; ++q) {
                const uint a = SlotOff + 1024 + q * 48;
                known = Parallel(m0.xyz, asfloat(Stat.Load3(a))) && Parallel(m1.xyz, asfloat(Stat.Load3(a + 16))) &&
                        Parallel(m2.xyz, asfloat(Stat.Load3(a + 32)));
            }
            if (!known) {
                Stat.InterlockedAdd(SlotOff + 1008, 1, gi);
                if (gi < BASES) {
                    const uint a = SlotOff + 1024 + gi * 48;
                    Stat.Store3(a, asuint(m0.xyz)); Stat.Store3(a + 16, asuint(m1.xyz)); Stat.Store3(a + 32, asuint(m2.xyz));
                }
            }
            InterlockedAdd(gsTwinN, 1, ti);
            if (ti < 16) gsTwin[ti] = twin;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint nb = min(gsBasisN, 4), nt = min(gsTwinN, 16);
    if (nb) {
        for (uint o = tid.x * 16; o + 64 <= size; o += 64 * 16) {
            float4 m3 = asfloat(Ring.Load4(base + o + 48));
            if (any(abs(m3 - float4(0, 0, 0, 1)) > 1e-6)) continue;
            bool isTwin = false;
            for (uint t = 0; t < nt; ++t) isTwin = isTwin || gsTwin[t] == o;
            if (isTwin) continue;
            float4 m0 = asfloat(Ring.Load4(base + o)), m1 = asfloat(Ring.Load4(base + o + 16)), m2 = asfloat(Ring.Load4(base + o + 32));
            if (any(abs(float3(m0.w, m1.w, m2.w)) >= 1e8) || AxisAligned(m0.xyz, m1.xyz, m2.xyz)) continue;
            for (uint b = 0; b < nb; ++b) {
                if (Parallel(m0.xyz, gsBasis[b][0]) && Parallel(m1.xyz, gsBasis[b][1]) && Parallel(m2.xyz, gsBasis[b][2])) {
                    Put(base + o + 12, asuint(m0.w + dot(m0.xyz, dv)));
                    Put(base + o + 28, asuint(m1.w + dot(m1.xyz, dv)));
                    Put(base + o + 44, asuint(m2.w + dot(m2.xyz, dv)));
                    ++folds;
                    break;
                }
            }
        }
    }
    if (folds) Stat.InterlockedAdd(SlotOff + 196, folds);
}

)"
R"(// The scene block: a view matrix at +0 - orthonormal rows (axis, -axis.p), then (0,0,0,1) - with its position p
// also at +240 (2026-11-12 census: dozens of copies a frame, one per pass). Within reach of the frame-end camera,
// so a light's or a shadow cascade's view never wins.
[numthreads(64, 1, 1)]
void CSFind(uint3 dt : SV_DispatchThreadID) {
    if (dt.x >= ViewCount) return;
    uint2 v = Views.Load2(dt.x * 8);
    if ((v.y & CHUNK_BIT) || v.y < 256) return;
    uint b = v.x;
    float4 r0 = asfloat(Ring.Load4(b)), r1 = asfloat(Ring.Load4(b + 16)), r2 = asfloat(Ring.Load4(b + 32)),
           r3 = asfloat(Ring.Load4(b + 48));
    if (any(abs(r3 - float4(0, 0, 0, 1)) > 1e-5)) return;
    if (abs(dot(r0.xyz, r0.xyz) - 1) > 2e-3 || abs(dot(r1.xyz, r1.xyz) - 1) > 2e-3 || abs(dot(r2.xyz, r2.xyz) - 1) > 2e-3) return;
    if (abs(dot(r0.xyz, r1.xyz)) > 2e-3 || abs(dot(r0.xyz, r2.xyz)) > 2e-3 || abs(dot(r1.xyz, r2.xyz)) > 2e-3) return;
    float3 p = asfloat(Ring.Load3(b + 240));
    if (!Near(r0.w, -dot(r0.xyz, p), 1e-4) || !Near(r1.w, -dot(r1.xyz, p), 1e-4) || !Near(r2.w, -dot(r2.xyz, p), 1e-4)) return;
    if (distance(p, P.xyz) > 3.0 || dot(r2.xyz, F.xyz) < 0.5 || dot(r0.xyz, R.xyz) < 0.5) return;
    uint was;
    Stat.InterlockedCompareExchange(SlotOff, 0, 1, was);
    if (was == 0) {
        Stat.Store3(SlotOff + 32, asuint(r0.xyz));
        Stat.Store3(SlotOff + 48, asuint(r1.xyz));
        Stat.Store3(SlotOff + 64, asuint(r2.xyz));
        Stat.Store3(SlotOff + 80, asuint(p));
        [unroll] for (uint i = 0; i < 4; ++i) Stat.Store4(SlotOff + 96 + 16 * i, Ring.Load4(b + 64 + 16 * i));   // projection
    }
}

)"
R"(// Every scene block of the frame against the camera CSFind kept (before anything is patched): a frame drawn with more
// than one camera - one from before the game's own smoothing, a previous frame's - has blocks that differ, and the
// patch moves only rows that match the kept one. +24 scene blocks, +28 of them another camera, +160 the largest
// position difference (m), +164 the largest axis difference (1 - cos), +168 the largest axis difference among blocks
// at the same position (rotation only).
[numthreads(64, 1, 1)]
void CSCount(uint3 dt : SV_DispatchThreadID) {
    if (dt.x >= ViewCount || Stat.Load(SlotOff) == 0) return;
    uint2 v = Views.Load2(dt.x * 8);
    if ((v.y & CHUNK_BIT) || v.y < 256) return;
    uint b = v.x;
    float4 r0 = asfloat(Ring.Load4(b)), r1 = asfloat(Ring.Load4(b + 16)), r2 = asfloat(Ring.Load4(b + 32)),
           r3 = asfloat(Ring.Load4(b + 48));
    if (any(abs(r3 - float4(0, 0, 0, 1)) > 1e-5)) return;
    if (abs(dot(r0.xyz, r0.xyz) - 1) > 2e-3 || abs(dot(r1.xyz, r1.xyz) - 1) > 2e-3 || abs(dot(r2.xyz, r2.xyz) - 1) > 2e-3) return;
    if (abs(dot(r0.xyz, r1.xyz)) > 2e-3 || abs(dot(r0.xyz, r2.xyz)) > 2e-3 || abs(dot(r1.xyz, r2.xyz)) > 2e-3) return;
    float3 p = asfloat(Ring.Load3(b + 240));
    if (!Near(r0.w, -dot(r0.xyz, p), 1e-4) || !Near(r1.w, -dot(r1.xyz, p), 1e-4) || !Near(r2.w, -dot(r2.xyz, p), 1e-4)) return;
    if (distance(p, P.xyz) > 3.0) return;
    float3 kr = normalize(asfloat(Stat.Load3(SlotOff + 32))), ku = normalize(asfloat(Stat.Load3(SlotOff + 48))),
           kf = normalize(asfloat(Stat.Load3(SlotOff + 64)));
    float3 kp = asfloat(Stat.Load3(SlotOff + 80));
    float dpos = distance(p, kp);
    float dang = max(max(1 - dot(normalize(r0.xyz), kr), 1 - dot(normalize(r1.xyz), ku)), 1 - dot(normalize(r2.xyz), kf));
    dang = max(dang, 0.0);
    Stat.InterlockedAdd(SlotOff + 24, 1);
    // 1 - cos: 0.1 deg 1.52e-6, 1 deg 1.52e-4, 10 deg 0.0152, 90 deg 1.
    const bool other = dpos > 1e-3 || dang > 1.52e-6;
    if (other) Stat.InterlockedAdd(SlotOff + 28, 1);
    if (dpos > 1e-3) Stat.InterlockedAdd(SlotOff + 188, 1);
    if (dang > 1.0) Stat.InterlockedAdd(SlotOff + 184, 1);
    else if (dang > 0.0152) Stat.InterlockedAdd(SlotOff + 180, 1);
    else if (dang > 1.52e-4) Stat.InterlockedAdd(SlotOff + 176, 1);
    else if (dang > 1.52e-6) Stat.InterlockedAdd(SlotOff + 172, 1);
    uint was;
    Stat.InterlockedMax(SlotOff + 160, asuint(dpos), was);
    Stat.InterlockedMax(SlotOff + 164, asuint(dang), was);
    if (dpos <= 1e-3) Stat.InterlockedMax(SlotOff + 168, asuint(dang), was);
}

)"
R"(// Every affine matrix of every view whose rows are parallel to a view-space basis CSFold proved with a twin somewhere in
// the frame: the light's cascades in blocks that carry no world twin (1024/1280 B cascade blocks, most 256/512 B ones -
// 2026-11-13 frame dumps: dozens a frame, none moved; the right eye's ground shadow came from one of them).
[numthreads(64, 1, 1)]
void CSFoldAll(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    if (gid.x >= ViewCount || Stat.Load(SlotOff) == 0 || (Pad.y & 64)) return;
    const uint ng = min(Stat.Load(SlotOff + 1008), BASES);
    if (ng == 0) return;
    uint2 v = Views.Load2(gid.x * 8);
    if (v.y & CHUNK_BIT) return;
    uint base = v.x, size = v.y;
    float3 a0 = asfloat(Stat.Load3(SlotOff + 32)), a1 = asfloat(Stat.Load3(SlotOff + 48)), a2 = asfloat(Stat.Load3(SlotOff + 64));
    float3 dv = float3(dot(a0, D.xyz), dot(a1, D.xyz), dot(a2, D.xyz)) * (D.w > 0 ? D.w : 1.0);   // D.w: probe gain
    uint folds = 0;
    for (uint o = tid.x * 16; o + 64 <= size; o += 64 * 16) {
        float4 m3 = asfloat(Ring.Load4(base + o + 48));
        if (any(abs(m3 - float4(0, 0, 0, 1)) > 1e-6)) continue;
        float4 m0 = asfloat(Ring.Load4(base + o)), m1 = asfloat(Ring.Load4(base + o + 16)), m2 = asfloat(Ring.Load4(base + o + 32));
        if (!Sane3(m0.xyz) || !Sane3(m1.xyz) || !Sane3(m2.xyz) || any(abs(float3(m0.w, m1.w, m2.w)) >= 1e8)) continue;
        if (AxisAligned(m0.xyz, m1.xyz, m2.xyz)) continue;
        for (uint q = 0; q < ng; ++q) {
            const uint a = SlotOff + 1024 + q * 48;
            if (Parallel(m0.xyz, asfloat(Stat.Load3(a))) && Parallel(m1.xyz, asfloat(Stat.Load3(a + 16))) &&
                Parallel(m2.xyz, asfloat(Stat.Load3(a + 32)))) {
                Put(base + o + 12, asuint(m0.w + dot(m0.xyz, dv)));
                Put(base + o + 28, asuint(m1.w + dot(m1.xyz, dv)));
                Put(base + o + 44, asuint(m2.w + dot(m2.xyz, dv)));
                ++folds;
                break;
            }
        }
    }
    if (folds) Stat.InterlockedAdd(SlotOff + 196, folds);
}

[numthreads(64, 1, 1)]
void CSPatch(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    if (gid.x >= ViewCount) return;
    uint2 v = Views.Load2(gid.x * 8);
    uint base = v.x, size = v.y & ~CHUNK_BIT;
    float3 axes[3] = { R.xyz, U.xyz, F.xyz };
    float3 cp = P.xyz;
    const bool found = Stat.Load(SlotOff) != 0;
    float4 clip = 0;       // a test point 5 m ahead, 1 m right, 0.5 m up - in clip space, by the frame's projection
    float3 test = 0;       // and in the world: an inverse view-projection must take the one back to the other
    if (found) {
        axes[0] = asfloat(Stat.Load3(SlotOff + 32));
        axes[1] = asfloat(Stat.Load3(SlotOff + 48));
        axes[2] = asfloat(Stat.Load3(SlotOff + 64));
        cp = asfloat(Stat.Load3(SlotOff + 80));
        const float4 tv = float4(1.0, 0.5, 5.0, 1.0);
        clip = float4(dot(asfloat(Stat.Load4(SlotOff + 96)), tv), dot(asfloat(Stat.Load4(SlotOff + 112)), tv),
                      dot(asfloat(Stat.Load4(SlotOff + 128)), tv), dot(asfloat(Stat.Load4(SlotOff + 144)), tv));
        test = cp + axes[0] * 1.0 + axes[1] * 0.5 + axes[2] * 5.0;
    }
    float3 pr = cp + D.xyz;
    // The view matrix stored by columns ends in its translation row (-r.p, -u.p, -f.p, 1).
    const float3 tl = float3(-dot(axes[0], cp), -dot(axes[1], cp), -dot(axes[2], cp));
    const float3 tr = float3(-dot(axes[0], pr), -dot(axes[1], pr), -dot(axes[2], pr));
    uint rows = 0, inv = 0, pos = 0, vcol = 0, ivp = 0;
    for (uint o = tid.x * 16; o + 16 <= size; o += 64 * 16) {
        float4 row = asfloat(Ring.Load4(base + o));
        float len = length(row.xyz);
        // Sentinel (+-3.4e38) and non-finite rows are no camera rows: their lengths overflow and "inf <= inf" passed the
        // parallel tests. The position copies below are read on their own.
        bool done = any(!(abs(row) < 1e30));
        if (!done && !(Pad.y & 8) && abs(row.w - 1.0) < 1e-5 && Near(row.x, tl.x, 1e-4) && Near(row.y, tl.y, 1e-4) && Near(row.z, tl.z, 1e-4)) {
            Put3(base + o, asuint(tr));
            done = true;
            ++vcol;
        }
        // An inverse view-projection stored by rows, found at its last row (0, 0, 1/near, ~0) and proven on the
        // test point: the second eye's is rows 0-2 plus the eye offset's component times that last row - exact, as
        // its only camera position is in invV's translation column. Lighting and shadow passes rebuild world
        // positions from depth with it; left alone, the right eye's shadows were sampled an eye-width off
        // (2026-11-13: "something off with shadows"; census: ~51 a frame, none moved).
        if (!(Pad.y & 16) && !done && found && o >= 48 && abs(row.x) < 1e-5 && abs(row.y) < 1e-5 && abs(row.z) > 1e-3 && abs(row.w) < 1e-2) {
            float4 m0 = asfloat(Ring.Load4(base + o - 48)), m1 = asfloat(Ring.Load4(base + o - 32)),
                   m2 = asfloat(Ring.Load4(base + o - 16));
            float4 h = float4(dot(m0, clip), dot(m1, clip), dot(m2, clip), dot(row, clip));
            if (abs(h.w) > 1e-6 && all(abs(h.xyz / h.w - test) <= 0.02)) {
                Put4(base + o - 48, asuint(m0 + D.x * row));
                Put4(base + o - 32, asuint(m1 + D.y * row));
                Put4(base + o - 16, asuint(m2 + D.z * row));
                done = true;
                ++ivp;
            }
        }
        if (len > 1e-4) {
            [unroll] for (int k = 0; k < 3; ++k) {
                if (!done && !(Pad.y & 1)) {
                    float s = dot(row.xyz, axes[k]);
                    float wL = -s * dot(axes[k], cp);
                    if (abs(abs(s) - len) <= 1e-4 * len && abs(row.w - wL) <= 1e-3 * max(1.0, abs(wL))) {
                        Put(base + o + 12, asuint(-s * dot(axes[k], pr)));
                        done = true;
                        ++rows;
                    }
                }
            }
            [unroll] for (int i = 0; i < 3; ++i) {
                if (!done && !(Pad.y & 2) && abs(row.x - axes[0][i]) < 1e-4 && abs(row.y - axes[1][i]) < 1e-4 &&
                    abs(row.z - axes[2][i]) < 1e-4 && Near(row.w, cp[i], 1e-3)) {
                    Put(base + o + 12, asuint(pr[i]));
                    done = true;
                    ++inv;
                }
            }
        }
        [unroll] for (uint j = 0; j < 16; j += 4) {
            uint s = o + j;
            if (!(Pad.y & 4) && !((Pad.y & 128) && size == 2048) && !(Pad.x & SizeClass(size)) &&
                !(size == 2048 && (Pad.x & Offset2048Bit(s))) && s + 12 <= size) {
                float3 q = asfloat(Ring.Load3(base + s));
                if (Near(q.x, cp.x, 1e-4) && Near(q.y, cp.y, 1e-4) && Near(q.z, cp.z, 1e-4)) {
                    Put3(base + s, asuint(pr));
                    ++pos;
                }
            }
        }
    }
    if (rows) Stat.InterlockedAdd(SlotOff + 4, rows);
    if (inv) Stat.InterlockedAdd(SlotOff + 8, inv);
    if (pos) Stat.InterlockedAdd(SlotOff + 12, pos);
    if (vcol) Stat.InterlockedAdd(SlotOff + 16, vcol);
    if (ivp) Stat.InterlockedAdd(SlotOff + 20, ivp);
}
)";

struct GpuPatcher {
    bool tried = false, ok = false;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;        // CSPatch
    ID3D12PipelineState* psoClear = nullptr;
    ID3D12PipelineState* psoFind = nullptr;
    ID3D12PipelineState* psoCount = nullptr;
    ID3D12PipelineState* psoFold = nullptr;     // view-space matrices built from a world-space twin (CSFold)
    ID3D12PipelineState* psoFoldAll = nullptr;  // ... and every matrix of the frame sharing such a matrix's basis
    ID3D12PipelineState* psoRestore = nullptr;  // the game's values back after the replay (CSRestore)
    ID3D12Resource* undo = nullptr;            // default heap, UAV: kUndoCap x (ring offset, value)
    ID3D12Resource* mark = nullptr;            // default heap, UAV: one bit per ring dword (patched this frame)
    static constexpr uint32_t kUndoCap = 32768; // = UNDO_CAP in kPatchHLSL
    ID3D12CommandAllocator* restoreAlloc[4] = {};
    ID3D12GraphicsCommandList* restore = nullptr;
    float k[24] = {};                          // the newest patch list's root constants (the restore reuses them)
    ID3D12Resource* stat = nullptr;            // default heap, UAV: kSlots x 64 bytes (see kPatchHLSL's Stat)
    static constexpr int kSlots = 4;
    static constexpr uint32_t kMaxViews = 32768;
    ID3D12Resource* views = nullptr;          // upload: kSlots x kMaxViews x (offset, size)
    uint8_t* viewsCpu = nullptr;
    ID3D12CommandAllocator* alloc[kSlots] = {};
    uint64_t slotFence[kSlots] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    int slot = 0;
    int lastSlot = -1;                         // the slot the newest patch list used

    // GPU timing, per slot: [0] patch start (= the left frame's GPU work done), [1] patch end, [2] replay end.
    ID3D12QueryHeap* qh = nullptr;
    ID3D12Resource* rb = nullptr;              // readback: kSlots x 3 timestamps
    uint64_t* rbCpu = nullptr;
    ID3D12CommandAllocator* tailAlloc[kSlots] = {};
    ID3D12GraphicsCommandList* tail = nullptr; // after the shadows: the replay-end stamp + resolve
    bool timed[kSlots] = {};
    double tsFreq = 0.0;
    uint64_t prevEnd = 0, prevFence = 0;       // the newest frame read back: its replay end, its replay fence value
} g_patcher;
// 5 s sums (microseconds) of the timed frames: the left frame (previous replay's end -> this patch's start, i.e.
// the game's frame plus our blits when the GPU never idles), the patch, the replay.
std::atomic<long long> g_gpuLeftUs{0}, g_gpuPatchUs{0}, g_gpuReplayUs{0};
std::atomic<long> g_gpuTimed{0}, g_gpuLeftN{0};
// The patch's own account, per frame read back: was the frame's own camera found, and how many rows and positions
// moved to the second eye. A frame with none moved shows the right eye from the left eye's spot.
std::atomic<long> g_patchFrames{0}, g_patchCamFound{0}, g_patchNone{0};
std::atomic<long long> g_patchRows{0}, g_patchInv{0}, g_patchPos{0}, g_patchVcol{0}, g_patchIvp{0};
// CSFold's view-space matrices moved, and the undo log's entries (CSRestore) - with the frames it overflowed in.
std::atomic<long long> g_patchFold{0}, g_undoEntries{0};
std::atomic<long long> g_spanBytes{0}, g_spanChunks{0};   // the frame's ring span patched beyond its views
std::atomic<long> g_carried{0};   // views carried into the next frame's patch (made after a frame's last submission)
std::atomic<long> g_undoOverflow{0};
// CSCount: scene blocks a frame, how many carry a camera other than the one the patch matched against, and how far
// off the farthest is (5 s maxima: metres; degrees between axes; degrees for blocks at the same position).
std::atomic<long long> g_sceneBlocks{0}, g_sceneOther{0};
std::atomic<long long> g_otherBucket[5];   // other camera by axis difference: 0.1-1, 1-10, 10-90, >90 deg; moved >1 mm
std::atomic<long> g_framesOther{0};
std::atomic<float> g_otherMaxPos{0.f}, g_otherMaxDeg{0.f}, g_otherMaxRotDeg{0.f};
constexpr size_t kStatRbOff = (size_t)GpuPatcher::kSlots * 3 * 8;   // stats after the timestamps in the readback
constexpr uint32_t kStatSlot = 4096;  // bytes per slot in the stat buffer (see kPatchHLSL's Stat; bases from +1024)
constexpr uint32_t kStatRb = 256;     // the whole slot, copied out per frame

void AtomicMaxF(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Caller: the slot's previous use has finished on the GPU. Its stamps go into the 5 s sums.
void GpuTimingCollect(int s) {
    GpuPatcher& g = g_patcher;
    if (!g.timed[s] || !g.rbCpu || g.tsFreq <= 0.0) return;
    g.timed[s] = false;
    {
        uint32_t st[kStatRb / 4];
        memcpy(st, (const uint8_t*)g.rbCpu + kStatRbOff + (size_t)s * kStatRb, kStatRb);
        g_sceneBlocks.fetch_add(st[6], std::memory_order_relaxed);
        g_sceneOther.fetch_add(st[7], std::memory_order_relaxed);
        for (int k = 0; k < 5; ++k) g_otherBucket[k].fetch_add(st[43 + k], std::memory_order_relaxed);   // +172..+188
        if (st[7]) {
            g_framesOther.fetch_add(1, std::memory_order_relaxed);
            float dpos, dang, drot;
            memcpy(&dpos, &st[40], 4); memcpy(&dang, &st[41], 4); memcpy(&drot, &st[42], 4);
            auto deg = [](float oneMinusCos) {
                return (float)(std::acos(std::max(-1.0, std::min(1.0, 1.0 - (double)oneMinusCos))) * 57.29577951308232);
            };
            AtomicMaxF(g_otherMaxPos, dpos);
            AtomicMaxF(g_otherMaxDeg, deg(dang));
            AtomicMaxF(g_otherMaxRotDeg, deg(drot));
        }
        g_patchFrames.fetch_add(1, std::memory_order_relaxed);
        if (st[0]) g_patchCamFound.fetch_add(1, std::memory_order_relaxed);
        if (!st[1] && !st[2] && !st[3]) g_patchNone.fetch_add(1, std::memory_order_relaxed);
        g_patchRows.fetch_add(st[1], std::memory_order_relaxed);
        g_patchInv.fetch_add(st[2], std::memory_order_relaxed);
        g_patchPos.fetch_add(st[3], std::memory_order_relaxed);
        g_patchVcol.fetch_add(st[4], std::memory_order_relaxed);
        g_patchIvp.fetch_add(st[5], std::memory_order_relaxed);
        g_undoEntries.fetch_add(st[48], std::memory_order_relaxed);   // +192
        g_patchFold.fetch_add(st[49], std::memory_order_relaxed);     // +196
        if (st[48] > GpuPatcher::kUndoCap) g_undoOverflow.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t* t = g.rbCpu + (size_t)s * 3;
    if (!t[0] || t[1] < t[0] || t[2] < t[1]) return;
    const double us = 1e6 / g.tsFreq;
    g_gpuPatchUs.fetch_add((long long)((t[1] - t[0]) * us), std::memory_order_relaxed);
    g_gpuReplayUs.fetch_add((long long)((t[2] - t[1]) * us), std::memory_order_relaxed);
    g_gpuTimed.fetch_add(1, std::memory_order_relaxed);
    if (g.prevEnd && g.slotFence[s] == g.prevFence + 1 && t[0] > g.prevEnd) {   // consecutive replays only
        g_gpuLeftUs.fetch_add((long long)((t[0] - g.prevEnd) * us), std::memory_order_relaxed);
        g_gpuLeftN.fetch_add(1, std::memory_order_relaxed);
    }
    g.prevEnd = t[2];
    g.prevFence = g.slotFence[s];
}

bool GpuPatcherInit(ID3D12Device* dev) {
    GpuPatcher& g = g_patcher;
    if (g.tried) return g.ok;
    g.tried = true;
    ID3DBlob* err = nullptr;
    D3D12_ROOT_PARAMETER rp[6]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = {0, 0, 24};
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor = {0, 0};
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor = {0, 0};
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].Descriptor = {1, 0};
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[4].Descriptor = {2, 0};
    rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[5].Descriptor = {3, 0};
    rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{6, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.rs)))) {
        LOGE("[replay] patch root signature failed");
        return false;
    }
    sig->Release();
    struct Entry { const char* name; ID3D12PipelineState** pso; };
    const Entry entries[] = {{"CSClear", &g.psoClear}, {"CSFind", &g.psoFind}, {"CSCount", &g.psoCount}, {"CSFold", &g.psoFold}, {"CSFoldAll", &g.psoFoldAll},
                             {"CSPatch", &g.pso}, {"CSRestore", &g.psoRestore}};
    for (const Entry& e : entries) {
        ID3DBlob* cs = nullptr;
        if (FAILED(D3DCompile(kPatchHLSL, sizeof(kPatchHLSL) - 1, "ervr_replay_patch", nullptr, nullptr, e.name, "cs_5_0", 0, 0,
                              &cs, &err))) {
            LOGE("[replay] patch shader %s: %s", e.name, err ? (const char*)err->GetBufferPointer() : "compile failed");
            if (err) err->Release();
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.rs;
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        const HRESULT hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(e.pso));
        cs->Release();
        if (FAILED(hr)) { LOGE("[replay] patch PSO %s failed", e.name); return false; }
    }
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (uint64_t)GpuPatcher::kSlots * GpuPatcher::kMaxViews * 8;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&g.views))) ||
        FAILED(g.views->Map(0, nullptr, (void**)&g.viewsCpu))) { LOGE("[replay] patch view list buffer failed"); return false; }
    {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC sd = bd;
        sd.Width = (uint64_t)GpuPatcher::kSlots * kStatSlot;
        sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&g.stat)))) { LOGE("[replay] patch stat buffer failed"); return false; }
        sd.Width = (uint64_t)GpuPatcher::kUndoCap * 8;
        if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&g.undo)))) { LOGE("[replay] patch undo buffer failed"); return false; }
        sd.Width = ((g_ring.size / 4 + 31) / 32) * 4;   // committed: starts zeroed
        if (!g_ring.res || FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON,
                                                               nullptr, IID_PPV_ARGS(&g.mark)))) {
            LOGE("[replay] patch mark buffer failed");
            return false;
        }
    }
    for (int i = 0; i < GpuPatcher::kSlots; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.restoreAlloc[i])))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.restoreAlloc[0], nullptr, IID_PPV_ARGS(&g.restore))))
        return false;
    g_listClose(g.restore);
    for (int i = 0; i < GpuPatcher::kSlots; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i])))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, IID_PPV_ARGS(&g.list)))) return false;
    g_listClose(g.list);
    // Timing (optional: the patch works without it).
    {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, (UINT)GpuPatcher::kSlots * 3, 0};
        D3D12_HEAP_PROPERTIES rbp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = bd;
        rd.Width = kStatRbOff + (uint64_t)GpuPatcher::kSlots * kStatRb;   // timestamps, then the patch's counters per slot
        bool ok = SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.qh))) &&
                  SUCCEEDED(dev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&g.rb))) &&
                  SUCCEEDED(g.rb->Map(0, nullptr, (void**)&g.rbCpu));
        for (int i = 0; ok && i < GpuPatcher::kSlots; ++i)
            ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.tailAlloc[i])));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.tailAlloc[0], nullptr,
                                                    IID_PPV_ARGS(&g.tail)));
        if (ok) g_listClose(g.tail);
        else { g.rbCpu = nullptr; LOGW("[replay] GPU timing unavailable"); }
    }
    g.ok = true;
    LOGI("[replay] GPU camera patch ready");
    return true;
}

// After the shadows, in the same submission: the replay-end stamp and the resolve of the slot's three stamps.
ID3D12CommandList* GpuTimingTail(ID3D12CommandQueue* q) {
    GpuPatcher& g = g_patcher;
    const int s = g.lastSlot;
    if (!g.rbCpu || !g.tail || s < 0) return nullptr;
    if (g.tsFreq <= 0.0) { UINT64 hz = 0; if (SUCCEEDED(q->GetTimestampFrequency(&hz))) g.tsFreq = (double)hz; }
    g.tailAlloc[s]->Reset();
    g_listReset(g.tail, g.tailAlloc[s], nullptr);
    g.tail->EndQuery(g.qh, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)s * 3 + 2);
    g.tail->ResolveQueryData(g.qh, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)s * 3, 3, g.rb, (uint64_t)s * 3 * 8);
    // The patch's counters (camera found, rows, inverse rows, positions) ride along.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {g.stat, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    g.tail->ResourceBarrier(1, &b);
    g.tail->CopyBufferRegion(g.rb, kStatRbOff + (uint64_t)s * kStatRb, g.stat, (uint64_t)s * kStatSlot, kStatRb);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.tail->ResourceBarrier(1, &b);
    g_listClose(g.tail);
    g.timed[s] = true;
    return g.tail;
}

// After the shadows, in the same submission: every value the patch replaced goes back (CSRestore), so the next frame -
// the left eye's - never reads the right eye's constants out of a block the game did not rewrite.
ID3D12CommandList* GpuRestoreList(bool bitsOnly) {
    GpuPatcher& g = g_patcher;
    const int s = g.lastSlot;
    if (!g.ok || !g.restore || s < 0) return nullptr;
    g.restoreAlloc[s]->Reset();
    g_listReset(g.restore, g.restoreAlloc[s], g.psoRestore);
    D3D12_RESOURCE_BARRIER b[4]{};
    b[0].Type = b[1].Type = b[2].Type = b[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {g_ring.res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_GENERIC_READ,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[1].Transition = {g.stat, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[2].Transition = {g.undo, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[3].Transition = {g.mark, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    g.restore->ResourceBarrier(4, b);
    g.restore->SetComputeRootSignature(g.rs);
    float k[24];
    memcpy(k, g.k, sizeof k);
    const uint32_t only = bitsOnly ? 1u : 0u;
    memcpy(k + 22, &only, 4);
    g.restore->SetComputeRoot32BitConstants(0, 24, k, 0);
    g.restore->SetComputeRootShaderResourceView(1, g.views->GetGPUVirtualAddress());
    g.restore->SetComputeRootUnorderedAccessView(2, g_ring.gpu);
    g.restore->SetComputeRootUnorderedAccessView(3, g.stat->GetGPUVirtualAddress());
    g.restore->SetComputeRootUnorderedAccessView(4, g.undo->GetGPUVirtualAddress());
    g.restore->SetComputeRootUnorderedAccessView(5, g.mark->GetGPUVirtualAddress());
    g.restore->Dispatch(GpuPatcher::kUndoCap / 64, 1, 1);
    D3D12_RESOURCE_BARRIER u{};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = g_ring.res;
    g.restore->ResourceBarrier(1, &u);
    for (D3D12_RESOURCE_BARRIER& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    g.restore->ResourceBarrier(4, b);
    g_listClose(g.restore);
    return g.restore;
}

// ---- per-eye volumes: the right eye's own fog history ------------------------------------------------------------------
// The volumetric fog reprojects last frame's volumes (GXVolumetricFog_DirLightShadowReproj reads g_PrevDirLightShadowTex,
// WriteInScatteringPreShadowed g_PrevInjectingDensity): 3D textures in ping-pong, written one frame and read the next.
// The replay runs the frame's commands again, so after it every volume held the RIGHT eye's version and the next left eye
// reprojected the right eye's fog ("fog sorting off in the left eye", 2026-11-13; its dithered sun shadow never settled
// - speckles on far terrain). Every 3D UAV texture the game makes gets a right-eye copy: before the replay the left
// eye's contents are set aside and the right eye's put in, after it the reverse (4 copies a volume a frame).
struct EyeVolume {
    ID3D12Resource* res = nullptr;       // the game's (identity: ours never outlive it, see ReplayStereoOnDestroyResource)
    D3D12_RESOURCE_DESC desc{};
    ID3D12Resource* right = nullptr;     // the right eye's contents between replays (COPY_DEST at rest)
    ID3D12Resource* save = nullptr;      // the left eye's while a replay runs (COPY_SOURCE at rest)
    bool rightValid = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;   // where the game leaves it (the fog's: UAV)
    bool known = false;                  // a transition of it seen: never transitioned from a guessed state
};
std::mutex g_volMtx;
std::vector<EyeVolume> g_vols;
std::vector<std::pair<ID3D12Resource*, uint64_t>> g_volGraveyard;   // ours, released once the replay fence passes
struct VolSwap {
    ID3D12CommandAllocator* alloc[GpuPatcher::kSlots][2] = {};
    ID3D12GraphicsCommandList* list[2] = {};
    bool tried = false, ok = false;
} g_volSwap;
std::atomic<long> g_volSwaps{0};
std::atomic<long long> g_volBytes{0};

void VolAdd(ID3D12Resource* r, const D3D12_RESOURCE_DESC& d) {
    std::lock_guard<std::mutex> g(g_volMtx);
    for (const EyeVolume& v : g_vols) if (v.res == r) return;
    const int n = g_volResN.load();
    if (n >= kVolMax) return;
    EyeVolume v;
    v.res = r;
    v.desc = d;
    g_vols.push_back(v);
    g_volRes[n].store(r);
    g_volResN.store(n + 1, std::memory_order_release);
    LOGI("[replay] per-eye volume %p: %llux%ux%u fmt %u, %u mips (the right eye keeps its own copy)", (void*)r,
         (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, (unsigned)d.Format, d.MipLevels);
}
void VolRemove(ID3D12Resource* r) {
    std::lock_guard<std::mutex> g(g_volMtx);
    for (auto it = g_vols.begin(); it != g_vols.end(); ++it) {
        if (it->res != r) continue;
        if (it->right) g_volGraveyard.push_back({it->right, g_replayFenceVal});
        if (it->save) g_volGraveyard.push_back({it->save, g_replayFenceVal});
        g_vols.erase(it);
        const int n = g_volResN.load();
        for (int k = 0; k < n; ++k)
            if (g_volRes[k].load() == r) { g_volRes[k].store(g_volRes[n - 1].load()); g_volRes[n - 1].store(nullptr); g_volResN.store(n - 1); break; }
        return;
    }
}

// Present thread: this frame's transitions (its lists in submission order) say where each volume was left; then the two
// copy lists - before the replay (left eye's aside, right eye's in) and after it (right eye's kept, left eye's back).
// Both or neither; tied to the patch's slot (its allocators are free once that slot's fence passed).
bool VolSwapLists(ID3D12Device* dev, const std::vector<Shadow*>& batch, ID3D12CommandList** in, ID3D12CommandList** out) {
    *in = *out = nullptr;
    GpuPatcher& g = g_patcher;
    const int s = g.lastSlot;
    if (!dev || s < 0 || !g_volResN.load()) return false;
    VolSwap& w = g_volSwap;
    if (!w.tried) {
        w.tried = true;
        bool ok = true;
        for (int i = 0; i < GpuPatcher::kSlots && ok; ++i)
            for (int k = 0; k < 2 && ok; ++k)
                ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&w.alloc[i][k])));
        for (int k = 0; k < 2 && ok; ++k) {
            ok = SUCCEEDED(g_origCreateList ? g_origCreateList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, w.alloc[0][k], nullptr,
                                                               IID_PPV_ARGS(&w.list[k]))
                                            : dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, w.alloc[0][k], nullptr,
                                                                     IID_PPV_ARGS(&w.list[k])));
            if (ok) g_listClose(w.list[k]);
        }
        w.ok = ok;
        if (!ok) LOGW("[replay] per-eye volumes: command lists could not be made - the fog history stays shared");
    }
    if (!w.ok) return false;
    std::lock_guard<std::mutex> lk(g_volMtx);
    {   // where the game left each volume this frame (the last transition wins; none = unchanged since last frame)
        for (Shadow* sh : batch)
            for (auto& [r, st] : sh->volStates)
                for (EyeVolume& v : g_vols)
                    if (v.res == r) { v.state = (D3D12_RESOURCE_STATES)st; v.known = true; }
    }
    for (auto it = g_volGraveyard.begin(); it != g_volGraveyard.end();) {   // ours no replay uses any more
        if (g_replayFence && g_replayFence->GetCompletedValue() >= it->second) { it->first->Release(); it = g_volGraveyard.erase(it); }
        else ++it;
    }
    std::vector<EyeVolume*> vs;
    long long bytes = 0;
    for (EyeVolume& v : g_vols) {
        if (!v.known || v.state == D3D12_RESOURCE_STATE_COPY_SOURCE || v.state == D3D12_RESOURCE_STATE_COPY_DEST) continue;
        if (!v.right || !v.save) {
            const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            if (!v.right && FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &v.desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                nullptr, IID_PPV_ARGS(&v.right))))
                v.right = nullptr;
            if (!v.save && FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &v.desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                               nullptr, IID_PPV_ARGS(&v.save))))
                v.save = nullptr;
            if (!v.right || !v.save) continue;
        }
        vs.push_back(&v);
        bytes += (long long)v.desc.Width * v.desc.Height * v.desc.DepthOrArraySize;
    }
    if (vs.empty()) return false;
    auto T = [](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        return x;
    };
    constexpr D3D12_RESOURCE_STATES SRC = D3D12_RESOURCE_STATE_COPY_SOURCE, DST = D3D12_RESOURCE_STATE_COPY_DEST;
    for (int k = 0; k < 2; ++k) {
        ID3D12GraphicsCommandList* l = w.list[k];
        w.alloc[s][k]->Reset();
        g_listReset(l, w.alloc[s][k], nullptr);
        std::vector<D3D12_RESOURCE_BARRIER> pre, mid, post;
        for (EyeVolume* v : vs) {
            if (v->state == SRC || v->state == DST) continue;   // mid-copy at the frame's end: leave it alone
            if (k == 0) {   // before the replay: left eye's aside, right eye's in
                pre.push_back(T(v->res, v->state, SRC)); pre.push_back(T(v->save, SRC, DST));
                mid.push_back(T(v->res, SRC, DST));
                if (v->rightValid) mid.push_back(T(v->right, DST, SRC));
                post.push_back(T(v->res, DST, v->state));
                if (v->rightValid) post.push_back(T(v->right, SRC, DST));
            } else {        // after it: right eye's kept, left eye's back
                pre.push_back(T(v->res, v->state, SRC));
                mid.push_back(T(v->res, SRC, DST)); mid.push_back(T(v->save, DST, SRC));
                post.push_back(T(v->res, DST, v->state));
            }
        }
        if (!pre.empty()) l->ResourceBarrier((UINT)pre.size(), pre.data());
        for (EyeVolume* v : vs) {
            if (v->state == SRC || v->state == DST) continue;
            if (k == 0) l->CopyResource(v->save, v->res); else l->CopyResource(v->right, v->res);
        }
        if (!mid.empty()) l->ResourceBarrier((UINT)mid.size(), mid.data());
        for (EyeVolume* v : vs) {
            if (v->state == SRC || v->state == DST) continue;
            if (k == 0) { if (v->rightValid) l->CopyResource(v->res, v->right); }
            else l->CopyResource(v->res, v->save);
        }
        if (!post.empty()) l->ResourceBarrier((UINT)post.size(), post.data());
        g_listClose(l);
    }
    for (EyeVolume* v : vs) if (v->state != SRC && v->state != DST) v->rightValid = true;   // the out list fills it
    *in = w.list[0];
    *out = w.list[1];
    g_volSwaps.fetch_add((long)vs.size(), std::memory_order_relaxed);
    g_volBytes.store(bytes, std::memory_order_relaxed);
    return true;
}

// The patch list for this frame: ring -> UAV, patch the views, ring -> read. Null if it cannot be built.
ID3D12CommandList* GpuPatchList(const CamPatch& c, const std::vector<std::pair<uint64_t, uint32_t>>& vas) {
    GpuPatcher& g = g_patcher;
    if (!g.ok || !g_ring.res || !g_replayFence) return nullptr;
    const int s = g.slot;
    if (g.slotFence[s] > g_replayFence->GetCompletedValue()) return nullptr;   // that slot still on the GPU
    g.slot = (g.slot + 1) % GpuPatcher::kSlots;
    uint32_t n = 0;
    uint32_t* out = (uint32_t*)(g.viewsCpu + (size_t)s * GpuPatcher::kMaxViews * 8);
    uint64_t lo = ~0ull, hi = 0;
    for (auto& [va, sz] : vas) {
        if (va < g_ring.gpu || va + sz > g_ring.gpu + g_ring.size || sz > 65536) continue;
        if (n >= GpuPatcher::kMaxViews) break;
        out[n * 2] = (uint32_t)(va - g_ring.gpu);
        out[n * 2 + 1] = sz;
        lo = std::min<uint64_t>(lo, va - g_ring.gpu);
        hi = std::max<uint64_t>(hi, va - g_ring.gpu + sz);
        ++n;
    }
    if (!n) return nullptr;
    // The rest of the frame's part of the ring too, in overlapping 4 KB chunks: about a third of its camera values sit
    // outside every view the frame created (2026-11-13 ring scan: 443 positions, 233 view rows) - blocks the shaders
    // reach through views made once, or as buffers - and the sun's shadow in the right eye was sampled through them.
    if (false && hi > lo) {   // OFF: the ring holds vertex/index/argument data too - patching it hung the GPU (2026-11-13)
        const uint64_t margin = 64ull << 10;
        lo = (lo > margin ? lo - margin : 0) & ~255ull;
        hi = std::min<uint64_t>(g_ring.size, hi + margin);
        uint32_t chunks = 0;
        for (uint64_t c = lo; c < hi && n < GpuPatcher::kMaxViews; c += 3584, ++chunks) {
            out[n * 2] = (uint32_t)c;
            out[n * 2 + 1] = (uint32_t)std::min<uint64_t>(4096, g_ring.size - c) | 0x80000000u;   // CHUNK_BIT
            ++n;
        }
        g_spanBytes.fetch_add((long long)(hi - lo), std::memory_order_relaxed);
        g_spanChunks.fetch_add(chunks, std::memory_order_relaxed);
    }
    GpuTimingCollect(s);   // the slot's previous frame has finished: its stamps are in
    g.alloc[s]->Reset();
    g_listReset(g.list, g.alloc[s], g.pso);
    if (g.rbCpu) g.list->EndQuery(g.qh, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)s * 3 + 0);
    D3D12_RESOURCE_BARRIER b[4]{};
    b[0].Type = b[1].Type = b[2].Type = b[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {g_ring.res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_GENERIC_READ,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[1].Transition = {g.stat, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[2].Transition = {g.undo, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    b[3].Transition = {g.mark, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    g.list->ResourceBarrier(4, b);
    g.list->SetComputeRootSignature(g.rs);
    float k[24] = {};
    memcpy(k + 0, c.r, 12); memcpy(k + 4, c.u, 12); memcpy(k + 8, c.f, 12); memcpy(k + 12, c.p, 12);
    for (int i = 0; i < 3; ++i) k[16 + i] = c.pr[i] - c.p[i];   // D: the second eye's offset in the world
    k[19] = Cfg().replayFoldGain;   // D.w: bench probe - the view-space folds moved this many times over (1 = right)
    const uint32_t cnt = n, slotOff = (uint32_t)s * kStatSlot;
    memcpy(k + 20, &cnt, 4);
    memcpy(k + 21, &slotOff, 4);
    const uint32_t rulesOff = (uint32_t)Cfg().replayRulesOff;
    memcpy(k + 23, &rulesOff, 4);   // Pad.y
    const uint32_t posSizesOff = (uint32_t)Cfg().replayPosSizesOff;
    memcpy(k + 22, &posSizesOff, 4);   // Pad.x (CSPatch); the restore list sets its own
    g.list->SetComputeRoot32BitConstants(0, 24, k, 0);
    memcpy(g.k, k, sizeof k);
    g.list->SetComputeRootShaderResourceView(1, g.views->GetGPUVirtualAddress() + (uint64_t)s * GpuPatcher::kMaxViews * 8);
    g.list->SetComputeRootUnorderedAccessView(2, g_ring.gpu);
    g.list->SetComputeRootUnorderedAccessView(3, g.stat->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(4, g.undo->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(5, g.mark->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER us{};
    us.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    us.UAV.pResource = g.stat;
    // Clear the slot's counters, find the frame's own camera in its scene constants, then patch against it.
    g.list->SetPipelineState(g.psoClear);
    g.list->Dispatch(1, 1, 1);
    g.list->ResourceBarrier(1, &us);
    g.list->SetPipelineState(g.psoFind);
    g.list->Dispatch((n + 63) / 64, 1, 1);
    g.list->ResourceBarrier(1, &us);
    g.list->SetPipelineState(g.psoCount);
    g.list->Dispatch((n + 63) / 64, 1, 1);
    {
        D3D12_RESOURCE_BARRIER all{};   // the count must read the ring before the patch writes it
        all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        all.UAV.pResource = nullptr;
        g.list->ResourceBarrier(1, &all);
        g.list->SetPipelineState(g.psoFold);
        g.list->Dispatch(n, 1, 1);
        g.list->ResourceBarrier(1, &all);
        g.list->SetPipelineState(g.psoFoldAll);
        g.list->Dispatch(n, 1, 1);
        g.list->ResourceBarrier(1, &all);
    }
    g.list->SetPipelineState(g.pso);
    g.list->Dispatch(n, 1, 1);
    D3D12_RESOURCE_BARRIER u[4]{};
    u[0].Type = u[1].Type = u[2].Type = u[3].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u[0].UAV.pResource = g_ring.res;
    u[1].UAV.pResource = g.stat;
    u[2].UAV.pResource = g.undo;
    u[3].UAV.pResource = g.mark;
    g.list->ResourceBarrier(4, u);
    for (D3D12_RESOURCE_BARRIER& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    g.list->ResourceBarrier(4, b);   // stat back to COMMON: the tail copies the counters out of it
    if (g.rbCpu) g.list->EndQuery(g.qh, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)s * 3 + 1);
    g_listClose(g.list);
    g.lastSlot = s;
    g.slotFence[s] = g_replayFenceVal + 1;   // the replay's own signal value, set right after this submission
    g_patchedBuffers.fetch_add(n, std::memory_order_relaxed);
    return g.list;
}

// ---- bench plan ([Experimental] ReplayBenchPlan): what the replay leaves behind for the next frame -------------------
// Phase 0: no eye offset, the game's own frame on screen; 1: ReplayEyeOffset, the game's own frame on screen - the
// replay still runs, so anything it leaves behind that the next frame reads (a shadow cascade kept across frames, a
// history buffer) moves in the game's own frame; 2: the offset, the replay's frame on screen. -1: no plan.
int BenchPhase() {
    const float per = Cfg().replayBenchPlan;
    if (per <= 0.f) return -1;
    static ULONGLONG s_active = 0;
    static int s_last = -1;
    if (!GameERPlayerActive()) { s_active = 0; s_last = -1; return 0; }
    if (!s_active) s_active = GetTickCount64();
    const ULONGLONG in = GetTickCount64() - s_active;
    const int ph = (int)((in / (ULONGLONG)(per * 1000.f)) % 3);
    if (ph != s_last) {
        s_last = ph;
        static const char* const kText[3] = {"no eye offset, the game's own frame on screen",
                                             "eye offset on, the game's own frame on screen (the replay still runs)",
                                             "eye offset on, the replay's frame on screen"};
        LOGI("[replay] bench plan phase %d at %.1f s: %s", ph, in / 1000.0, kText[ph]);
    }
    return ph;
}

struct BenchShow {
    ID3D12Resource* saved = nullptr;
    D3D12_RESOURCE_DESC desc{};
    ID3D12CommandAllocator* alloc[GpuPatcher::kSlots][2] = {};
    ID3D12GraphicsCommandList* list[2] = {};
    bool failed = false;
} g_benchShow;

void WaitReplayFence(DWORD ms) {
    if (!g_replayFence || g_replayFence->GetCompletedValue() >= g_replayFenceVal) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev && SUCCEEDED(g_replayFence->SetEventOnCompletion(g_replayFenceVal, ev))) WaitForSingleObject(ev, ms);
    if (ev) CloseHandle(ev);
}

// [0], ahead of the replay: the game's frame copied aside. [1], behind it: copied back over the replay's. Recorded with
// per-slot allocators of the patch list just built (`slot`), which that slot's fence already guards.
ID3D12CommandList* BenchShowList(ID3D12Device* dev, ID3D12Resource* back, int which, int slot) {
    BenchShow& b = g_benchShow;
    if (b.failed || !dev || !back || slot < 0 || slot >= GpuPatcher::kSlots) return nullptr;
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    if (which == 0 && b.saved && (bd.Width != b.desc.Width || bd.Height != b.desc.Height || bd.Format != b.desc.Format)) {
        WaitReplayFence(500);   // a resized swapchain: the old copy may still be in use
        b.saved->Release();
        b.saved = nullptr;
    }
    if (!b.saved) {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = bd;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&b.saved)))) {
            LOGW("[replay] bench plan: no copy of the back buffer (%llux%u fmt %u)", (unsigned long long)bd.Width, bd.Height,
                 (unsigned)bd.Format);
            b.failed = true;
            return nullptr;
        }
        b.desc = bd;
    }
    if (!b.alloc[slot][which] &&
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.alloc[slot][which])))) {
        b.failed = true;
        return nullptr;
    }
    if (!b.list[which]) {
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.alloc[slot][which], nullptr,
                                          IID_PPV_ARGS(&b.list[which])))) {
            b.failed = true;
            return nullptr;
        }
        g_listClose(b.list[which]);
    }
    b.alloc[slot][which]->Reset();
    g_listReset(b.list[which], b.alloc[slot][which], nullptr);
    ID3D12GraphicsCommandList* l = b.list[which];
    D3D12_RESOURCE_BARRIER t[2]{};
    t[0].Type = t[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (which == 0) {
        t[0].Transition = {back, all, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
        l->ResourceBarrier(1, t);
        l->CopyResource(b.saved, back);
        t[0].Transition = {back, all, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT};
        t[1].Transition = {b.saved, all, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    } else {
        t[0].Transition = {back, all, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
        l->ResourceBarrier(1, t);
        l->CopyResource(back, b.saved);
        t[0].Transition = {back, all, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT};
        t[1].Transition = {b.saved, all, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    }
    l->ResourceBarrier(2, t);
    g_listClose(l);
    return l;
}

// ---- bench capture ([Experimental] ReplayBenchCapture) ----------------------------------------------------------------
// 1: this frame - the game's frame saved, the replay moved by ReplayEyeOffset, the replay saved. 2: the game's camera is
// moved by the same offset (UpdateCamera), the replay runs unmoved. 3: a few frames on, that frame saved; camera back.
// 4: the replay stays unmoved. 5: a few frames on, the game's frame saved again (cap-4): against cap-1 it says whether the
// replays moved by the offset before cap-1 left anything in the first eye's frame.
std::atomic<float> g_benchCamShift{0.f};
std::atomic<bool> g_benchShiftDiag{false};   // census shifts: along a skew camera-space direction, not the right axis
int BenchCaptureStep() {
    const float at = Cfg().replayBenchCapture;
    if (at <= 0.f) return 0;
    static ULONGLONG s_active = 0;
    static int s_state = 0, s_n = 0;
    if (s_state >= 5) return 0;
    if (!GameERPlayerActive()) { s_active = 0; return 0; }
    if (!s_active) s_active = GetTickCount64();
    switch (s_state) {
        case 0:
            if (GetTickCount64() - s_active < (ULONGLONG)(at * 1000.f)) return 0;
            s_state = 1;
            return 1;
        case 1:
            g_benchCamShift.store(Cfg().replayEyeOffset);
            s_state = 2;
            s_n = 0;
            return 2;
        case 2:
            if (++s_n < 8) return 2;
            s_state = 3;
            return 3;
        case 3:
            s_state = 4;
            s_n = 0;
            return 4;
        default:
            if (++s_n < 8) return 4;
            s_state = 5;
            return 5;
    }
}

// A copy of the back buffer into a readback buffer, recorded on the frame's device and submitted inside the replay's own
// ExecuteCommandLists (Dx12DumpBackbuffer's separate submission crashed in D3D12Core here, 2026-11-13). The file is
// written by a worker once the replay fence passes `fenceVal`.
struct BenchGrab {
    ID3D12Resource* rb = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint32_t w = 0, h = 0, pitch = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    std::wstring path;
};

ID3D12CommandList* BenchGrabList(const Dx12Frame& f, const wchar_t* name, std::vector<BenchGrab>& out) {
    if (!f.device || !f.backBuffer) return nullptr;
    BenchGrab g;
    const D3D12_RESOURCE_DESC sd = f.backBuffer->GetDesc();
    g.w = (uint32_t)sd.Width; g.h = sd.Height; g.fmt = sd.Format;
    g.pitch = (g.w * 4 + 255) & ~255u;
    std::wstring p = Cfg().iniPath;
    const size_t sl = p.find_last_of(L"\\/");
    g.path = (sl == std::wstring::npos ? std::wstring() : p.substr(0, sl + 1)) + name;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (uint64_t)g.pitch * g.h; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(f.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g.rb))) ||
        FAILED(f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc))) ||
        FAILED(f.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list)))) {
        if (g.list) g.list->Release();
        if (g.alloc) g.alloc->Release();
        if (g.rb) g.rb->Release();
        LOGW("[replay] bench capture: %ls could not be set up", name);
        return nullptr;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {f.backBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    g.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {g.fmt, g.w, g.h, 1, g.pitch};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = f.backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.list->ResourceBarrier(1, &b);
    g_listClose(g.list);
    float cam[16] = {};
    GameERRenderCamera(cam, nullptr);
    LOGI("[replay] bench capture: %ls queued (camera at %.3f %.3f %.3f, right %.3f %.3f %.3f)", name, cam[12], cam[13],
         cam[14], cam[0], cam[1], cam[2]);
    ID3D12CommandList* l = g.list;
    out.push_back(std::move(g));
    return l;
}

// Worker: waits for the replay fence, writes each grab as a PPM, releases everything.
void BenchGrabWrite(std::vector<BenchGrab> grabs, uint64_t fenceVal) {
    if (grabs.empty() || !g_replayFence) return;
    ID3D12Fence* fence = g_replayFence;
    fence->AddRef();
    std::thread([grabs = std::move(grabs), fence, fenceVal]() mutable {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev && fence->GetCompletedValue() < fenceVal && SUCCEEDED(fence->SetEventOnCompletion(fenceVal, ev)))
            WaitForSingleObject(ev, 5000);
        if (ev) CloseHandle(ev);
        const bool done = fence->GetCompletedValue() >= fenceVal;
        for (BenchGrab& g : grabs) {
            void* mem = nullptr;
            const D3D12_RANGE rr{0, (SIZE_T)g.pitch * g.h};
            bool ok = false;
            if (done && SUCCEEDED(g.rb->Map(0, &rr, &mem)) && mem) {
                HANDLE fh = CreateFileW(g.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (fh != INVALID_HANDLE_VALUE) {
                    char hdr[64];
                    const int hl = snprintf(hdr, sizeof hdr, "P6\n%u %u\n255\n", g.w, g.h);
                    DWORD wr = 0;
                    WriteFile(fh, hdr, hl, &wr, nullptr);
                    std::vector<uint8_t> row((size_t)g.w * 3);
                    const bool tenBit = g.fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
                    const bool bgra = g.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || g.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    for (uint32_t y = 0; y < g.h; ++y) {
                        const uint8_t* s = (const uint8_t*)mem + (size_t)y * g.pitch;
                        for (uint32_t x = 0; x < g.w; ++x) {
                            if (tenBit) {
                                const uint32_t v = ((const uint32_t*)s)[x];
                                row[x * 3 + 0] = (uint8_t)((v & 0x3FF) >> 2);
                                row[x * 3 + 1] = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
                                row[x * 3 + 2] = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
                            } else {
                                row[x * 3 + 0] = s[x * 4 + (bgra ? 2 : 0)];
                                row[x * 3 + 1] = s[x * 4 + 1];
                                row[x * 3 + 2] = s[x * 4 + (bgra ? 0 : 2)];
                            }
                        }
                        WriteFile(fh, row.data(), (DWORD)row.size(), &wr, nullptr);
                    }
                    CloseHandle(fh);
                    ok = true;
                }
                const D3D12_RANGE none{0, 0};
                g.rb->Unmap(0, &none);
            }
            LOGI("[replay] bench capture: %ls %s", g.path.c_str(), ok ? "written" : "NOT written");
            g.list->Release(); g.alloc->Release(); g.rb->Release();
        }
        fence->Release();
    }).detach();
}

// ---- matrix census ([Experimental] ReplayMatrixCensus) ------------------------------------------------------------
// One frame's constants searched for the 4x4 matrices built from its camera. The scene block gives the camera the
// frame was drawn with (view at +0, position at +240; +64 read as the projection); every 16-byte-aligned 4x4 of every
// view is then tested against it: view, view-projection, inverse view, inverse view-projection, each stored by rows
// and by columns. The patch moves view / view-projection rows, inverse-view rows and position copies; anything else
// found here is drawn by the right eye from the left eye's spot (2026-11-13: "something off with shadows").
std::atomic<bool> g_mcensusArm{false};
struct M4 { float m[4][4]; };
void Mul4(const M4& a, const float v[4], float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2] + a.m[i][3] * v[3];
}
M4 Transposed(const M4& a) {
    M4 t;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) t.m[i][j] = a.m[j][i];
    return t;
}
M4 MulMM(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
// Written as !(x <= tol): NaN fails every comparison, and data full of NaN must never read as a match.
bool SamePoint(const float h[4], const float want[3], float tol) {   // homogeneous h, after the divide
    if (!(std::fabs(h[3]) >= 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) if (!(std::fabs(h[i] / h[3] - want[i]) <= tol)) return false;
    return true;
}
bool Same4(const float a[4], const float b[4], float rel) {
    for (int i = 0; i < 4; ++i) if (!(std::fabs(a[i] - b[i]) <= rel * std::max(1.f, std::fabs(b[i])))) return false;
    return true;
}

using CensusViews = std::map<uint64_t, std::pair<uint32_t, DWORD>>;   // va -> (size, creating thread)
std::map<std::string, int> CensusScan(const CensusViews& views, const M4& V, const M4& P, const float cp[3], bool verbose);
void PairCensus(const CensusViews& views, const float cp[3]);
std::string CensusSum(const std::map<std::string, int>& counts) {
    std::string s;
    for (auto& [k, c] : counts) s += " " + k + ": " + std::to_string(c) + ";";
    return s.empty() ? " none" : s;
}
struct CensusSave { CensusViews views; M4 V{}, P{}; float cp[3] = {}; bool pending = false; int dump = -1; };
CensusSave g_mcSave;
int g_mcDumps = 0;   // frames dumped so far ([Experimental] ReplayMatrixCensus: three, half a second apart)

// One frame's constants, raw, for offline analysis (tools: scratchpad capcompare/mdump): header 'CMVR', view count, the
// scene camera (V, P by rows, position), the eye offset applied (0 before the patch), the ring's GPU base; then per view
// its GPU address, byte count, creating thread and bytes.
void DumpFrameConstants(const CensusViews& views, const M4& V, const M4& P, const float cp[3], const float delta[3],
                        const wchar_t* name) {
    std::wstring path = Cfg().iniPath;
    const size_t sl = path.find_last_of(L"\\/");
    path = (sl == std::wstring::npos ? std::wstring() : path.substr(0, sl + 1)) + name;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { LOGW("[mcensus] frame dump %ls: cannot write", name); return; }
    const uint32_t magic = 0x52564D43, n = (uint32_t)views.size();
    fwrite(&magic, 4, 1, f); fwrite(&n, 4, 1, f);
    fwrite(V.m, 64, 1, f); fwrite(P.m, 64, 1, f); fwrite(cp, 12, 1, f); fwrite(delta, 12, 1, f);
    fwrite(&g_ring.gpu, 8, 1, f);
    std::vector<uint8_t> buf;
    size_t bytes = 0;
    for (auto& [va, st] : views) {
        uint64_t avail = 0;
        const uint8_t* b = CpuOf(va, 16, &avail);
        const uint32_t len = b ? (uint32_t)std::min<uint64_t>(avail, st.first) : 0;
        const uint32_t tid = st.second;
        fwrite(&va, 8, 1, f); fwrite(&len, 4, 1, f); fwrite(&tid, 4, 1, f);
        if (len) {
            buf.resize(len);
            memcpy(buf.data(), b, len);
            fwrite(buf.data(), 1, len, f);
            bytes += len;
        }
    }
    fclose(f);
    LOGI("[mcensus] frame dump %ls: %u views, %zu bytes", name, n, bytes);
}

// The pass log's frame (see PassRecord): passlog.csv - the depth views known, then per submitted list its calls; and
// passlog-cbv.bin - 'PLCB', count, then per constant view the tables named: GPU address, bytes, the bytes.
void PassLogWrite() {
    std::vector<PassList> fr;
    {
        std::lock_guard<std::mutex> g(g_passMtx);
        fr.swap(g_passFrame);
        g_passOf.clear();
    }
    std::wstring dir = Cfg().iniPath;
    const size_t sl = dir.find_last_of(L"\\/");
    dir = sl == std::wstring::npos ? std::wstring() : dir.substr(0, sl + 1);
    FILE* f = _wfopen((dir + L"passlog.csv").c_str(), L"w");
    if (!f) { LOGW("[passlog] cannot write passlog.csv"); return; }
    fprintf(f, "# ring %llx size %llx\n", (unsigned long long)g_ring.gpu, (unsigned long long)g_ring.size);
    {
        std::lock_guard<std::mutex> g(g_dsvMtx);
        for (auto& [h, i] : g_dsvAt)
            fprintf(f, "dsv,%llx,%p,%llux%u,%u,fmt %u,view fmt %u,dim %d,mip %u,slice %u+%u,shadow %d\n", (unsigned long long)h, i.res,
                    (unsigned long long)i.w, i.h, i.depth, (unsigned)i.resFmt, (unsigned)i.fmt, (int)i.dim, i.mip, i.slice, i.slices,
                    IsShadowDsv(h) ? 1 : 0);
    }
    std::map<uint64_t, uint32_t> cbvs;
    size_t evs = 0;
    for (size_t li = 0; li < fr.size(); ++li) {
        const PassList& pl = fr[li];
        fprintf(f, "list,%zu,%p,%d,%zu\n", li, (void*)pl.list, pl.recorded ? 1 : 0, pl.ev.size());
        for (size_t ei = 0; ei < pl.ev.size(); ++ei) {
            const PassEv& e = pl.ev[ei];
            fprintf(f, "%zu,%zu,%c,%llx,%llx,%llx,%llx,%s\n", li, ei, e.kind, (unsigned long long)e.a, (unsigned long long)e.b,
                    (unsigned long long)e.c, (unsigned long long)e.d, e.s.c_str());
            if (e.kind == 'T') {   // "slot:c:va:size;" entries
                size_t p = 0;
                while ((p = e.s.find(":c:", p)) != std::string::npos) {
                    unsigned long long va = 0; unsigned size = 0;
                    if (sscanf(e.s.c_str() + p + 3, "%llx:%u", &va, &size) == 2) cbvs[va] = std::max(cbvs[va], size);
                    p += 3;
                }
            } else if (e.kind == 'V' && e.c) {
                cbvs[e.c] = std::max<uint32_t>(cbvs[e.c], 256);
            }
        }
        evs += pl.ev.size();
    }
    fclose(f);
    size_t bytes = 0;
    if (FILE* b = _wfopen((dir + L"passlog-cbv.bin").c_str(), L"wb")) {
        const uint32_t magic = 0x4243'4C50, n = (uint32_t)cbvs.size();
        fwrite(&magic, 4, 1, b); fwrite(&n, 4, 1, b);
        for (auto& [va, size] : cbvs) {
            uint64_t avail = 0;
            const uint8_t* p = CpuOf(va, 16, &avail);
            const uint32_t len = p ? (uint32_t)std::min<uint64_t>(avail, size) : 0;
            fwrite(&va, 8, 1, b); fwrite(&len, 4, 1, b);
            if (len) { fwrite(p, 1, len, b); bytes += len; }
        }
        fclose(b);
    }
    LOGI("[passlog] frame written: %zu lists, %zu calls, %zu constant views (%zu bytes) -> passlog.csv, passlog-cbv.bin",
         fr.size(), evs, cbvs.size(), bytes);
}

void MatrixCensus(const std::vector<std::pair<uint64_t, uint32_t>>& vas) {
    std::map<uint64_t, std::pair<uint32_t, DWORD>> views;   // va -> (size, creating thread: not kept, 0)
    for (auto& [va, sz] : vas) views.emplace(va, std::make_pair(sz, (DWORD)0));
    {   // the frame's root constant buffers too (thread 0xFFFFFFFF marks them in the dump), 1 KB each
        const int nr = std::min(g_rootCbvN.load(), kRootCbv);
        std::map<uint64_t, int> uniq;
        int inRing = 0, cpuOut = 0, gpuOnly = 0;
        for (int i = 0; i < nr; ++i) uniq[g_rootCbv[i].va] += 1;
        for (auto& [va, cnt] : uniq) {
            const bool ring = g_ring.res && va >= g_ring.gpu && va < g_ring.gpu + g_ring.size;
            uint64_t avail = 0;
            const bool cpu = CpuOf(va, 16, &avail) != nullptr;
            ring ? ++inRing : cpu ? ++cpuOut : ++gpuOnly;
            if (cpu) views.emplace(va, std::make_pair((uint32_t)std::min<uint64_t>(avail, 1024), 0xFFFFFFFFu));
        }
        LOGI("[mcensus] root constant buffers this frame: %d bindings, %zu addresses - %d in the constants ring, %d in other "
             "CPU-visible buffers, %d not CPU-visible", nr, uniq.size(), inRing, cpuOut, gpuOnly);
        LOGI("[mcensus] root 32-bit constants since the last census: %ld calls, %ld values, %ld camera positions among them",
             g_rcCalls.exchange(0), g_rcValues.exchange(0), g_rcCamHits.exchange(0));
        int shown = 0;
        for (auto& [va, cnt] : uniq) {
            if (shown++ >= 4) break;
            LOGI("[mcensus]   root CBV %llx x%d%s", (unsigned long long)va, cnt,
                 g_ring.res && va >= g_ring.gpu && va < g_ring.gpu + g_ring.size ? " (ring)" : "");
        }
    }
    {   // Camera values in the ring OUTSIDE this frame's views (a constant block whose view was made once, at load, and is
        // rewritten in place each frame never reaches the patch's view list).
        float cam[16] = {};
        uint64_t avail = 0;
        // Off by default: reading the whole write-combined ring stalls the game 5-20 s (it found what it was for: ~a third
        // of the frame's camera values outside its views, 2026-11-13).
        static const bool s_ringScan = GetEnvironmentVariableW(L"ERVR_RING_SCAN", nullptr, 0) > 0;
        const uint8_t* ring = (s_ringScan && g_ring.res) ? CpuOf(g_ring.gpu, 16, &avail) : nullptr;
        if (ring && GameERRenderCamera(cam, nullptr)) {
            std::vector<std::pair<uint64_t, uint64_t>> iv;   // covered ring offsets [a, b)
            for (auto& [va, st] : views)
                if (va >= g_ring.gpu && va < g_ring.gpu + g_ring.size) iv.push_back({va - g_ring.gpu, va - g_ring.gpu + st.first});
            std::sort(iv.begin(), iv.end());
            uint64_t spanLo = iv.empty() ? 0 : iv.front().first, spanHi = 0;   // this frame's part of the ring
            for (auto& x : iv) spanHi = std::max(spanHi, x.second);
            auto covered = [&](uint64_t off) {
                if (off < spanLo || off >= spanHi) return true;   // the other frame's half: not this frame's business
                auto it = std::upper_bound(iv.begin(), iv.end(), std::make_pair(off, ~0ull));
                while (it != iv.begin()) { --it; if (off >= it->first && off < it->second) return true; if (it->second + 65536 < off) break; }
                return false;
            };
            const float* ax[3] = {cam, cam + 4, cam + 8};
            const float* p = cam + 12;
            const size_t nf = (size_t)(avail / 4);
            std::vector<float> chunk(1 << 20);
            long posIn = 0, posOut = 0, rowIn = 0, rowOut = 0;
            std::map<uint64_t, int> outPages;   // 4 KB pages with uncovered hits
            for (size_t c0 = 0; c0 < nf; c0 += chunk.size() - 4) {
                const size_t cn = std::min(chunk.size(), nf - c0);
                memcpy(chunk.data(), ring + c0 * 4, cn * 4);
                for (size_t i = 0; i + 3 <= cn; ++i) {
                    const float* v = &chunk[i];
                    if (std::fabs(v[0] - p[0]) < 5e-3f && std::fabs(v[1] - p[1]) < 5e-3f && std::fabs(v[2] - p[2]) < 5e-3f) {
                        const uint64_t off = (c0 + i) * 4;
                        if (covered(off)) ++posIn; else { ++posOut; outPages[off >> 12]++; }
                    }
                    if (((c0 + i) & 3) == 0 && i + 4 <= cn) {
                        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        if (!(len > 1e-4f) || !(len < 1e4f)) continue;
                        for (int k = 0; k < 3; ++k) {
                            const float s = v[0] * ax[k][0] + v[1] * ax[k][1] + v[2] * ax[k][2];
                            const float wl = -s * (ax[k][0] * p[0] + ax[k][1] * p[1] + ax[k][2] * p[2]);
                            if (std::fabs(std::fabs(s) - len) <= 1e-4f * len && std::fabs(v[3] - wl) <= 5e-3f + 1e-5f * std::fabs(wl)) {
                                const uint64_t off = (c0 + i) * 4;
                                if (covered(off)) ++rowIn; else { ++rowOut; outPages[off >> 12]++; }
                                break;
                            }
                        }
                    }
                }
            }
            LOGI("[mcensus] the ring between this frame's first and last view (+%llx..+%llx): camera positions %ld inside its "
                 "views, %ld OUTSIDE them; view rows %ld inside, %ld OUTSIDE (%zu 4 KB pages)", (unsigned long long)spanLo,
                 (unsigned long long)spanHi, posIn, posOut, rowIn, rowOut, outPages.size());
            int added = 0;
            for (auto& [pg, c] : outPages) {   // into the frame dump as pseudo-views (thread 0xFFFFFFFE), 4 KB each
                if (added++ >= 48) break;
                views.emplace(g_ring.gpu + (pg << 12), std::make_pair(4096u, 0xFFFFFFFEu));
            }
            int shown = 0;
            for (auto& [pg, c] : outPages) {
                if (shown++ >= 16) break;
                LOGI("[mcensus]   uncovered camera values: ring +%llx (gpu %llx): %d", (unsigned long long)(pg << 12),
                     (unsigned long long)(g_ring.gpu + (pg << 12)), c);
            }
        }
    }
    {   // Camera values in CPU-visible buffers OUTSIDE the ring (structured buffers, staging copies): the patch never sees them.
        std::vector<CpuBuffer> bufs;
        AcquireSRWLockExclusive(&g_bufLock);
        bufs = g_bufs;
        ReleaseSRWLockExclusive(&g_bufLock);
        float cam[16] = {};
        if (GameERRenderCamera(cam, nullptr)) {
            const float* ax[3] = {cam, cam + 4, cam + 8};
            const float* p = cam + 12;
            for (const CpuBuffer& b : bufs) {
                if (b.gpu == g_ring.gpu || b.size < 64 || b.size > (64ull << 20)) continue;
                uint64_t avail = 0;
                const uint8_t* m = CpuOf(b.gpu, 16, &avail);
                if (!m) continue;
                std::vector<float> f((size_t)(avail / 4));
                memcpy(f.data(), m, f.size() * 4);
                int pos = 0, rows = 0, firstPos = -1, firstRow = -1;
                for (size_t i = 0; i + 3 <= f.size(); ++i) {
                    if (std::fabs(f[i] - p[0]) < 5e-3f && std::fabs(f[i + 1] - p[1]) < 5e-3f && std::fabs(f[i + 2] - p[2]) < 5e-3f) {
                        if (firstPos < 0) firstPos = (int)(i * 4);
                        ++pos;
                    }
                }
                for (size_t i = 0; i + 4 <= f.size(); i += 4) {
                    const float* r = &f[i];
                    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                    if (!(len > 1e-4f) || !(len < 1e4f)) continue;
                    for (int k = 0; k < 3; ++k) {
                        const float s = r[0] * ax[k][0] + r[1] * ax[k][1] + r[2] * ax[k][2];
                        const float wl = -s * (ax[k][0] * p[0] + ax[k][1] * p[1] + ax[k][2] * p[2]);
                        if (std::fabs(std::fabs(s) - len) <= 1e-4f * len && std::fabs(r[3] - wl) <= 5e-3f + 1e-5f * std::fabs(wl)) {
                            if (firstRow < 0) firstRow = (int)(i * 4);
                            ++rows;
                            break;
                        }
                    }
                }
                if (pos || rows)
                    LOGI("[mcensus] camera values OUTSIDE the ring: buffer gpu %llx (%llu KB): %d camera positions (first at +%d), "
                         "%d view rows (first at +%d)", (unsigned long long)b.gpu, (unsigned long long)(b.size >> 10), pos,
                         firstPos, rows, firstRow);
            }
        }
    }
    LARGE_INTEGER t0, t1, qf;
    QueryPerformanceCounter(&t0);
    // 1. The scene block (CSFind's signature).
    M4 V{}, P{};
    float cp[3] = {};
    uint64_t sceneVa = 0;
    for (auto& [va, st] : views) {
        if (st.first < 256 || st.first > 4096) continue;
        uint64_t avail = 0;
        const uint8_t* b = CpuOf(va, 256, &avail);
        if (!b || avail < 256) continue;
        float r[16], p[3];
        memcpy(r, b, 64);
        memcpy(p, b + 240, 12);
        auto dot3 = [](const float* a, const float* c) { return a[0] * c[0] + a[1] * c[1] + a[2] * c[2]; };
        bool ok = std::fabs(r[12]) + std::fabs(r[13]) + std::fabs(r[14]) < 1e-5f && std::fabs(r[15] - 1.f) < 1e-5f;
        for (int i = 0; ok && i < 3; ++i) {
            ok = std::fabs(dot3(r + 4 * i, r + 4 * i) - 1.f) < 2e-3f &&
                 std::fabs(r[4 * i + 3] + dot3(r + 4 * i, p)) <= 1e-4f * std::max(1.f, std::fabs(r[4 * i + 3]));
        }
        ok = ok && std::fabs(dot3(r, r + 4)) < 2e-3f && std::fabs(dot3(r, r + 8)) < 2e-3f && std::fabs(dot3(r + 4, r + 8)) < 2e-3f;
        if (!ok) continue;
        memcpy(V.m, b, 64);
        memcpy(P.m, b + 64, 64);
        memcpy(cp, p, 12);
        sceneVa = va;
        break;
    }
    if (!sceneVa) { LOGW("[mcensus] no scene block in this frame's %zu views", views.size()); return; }
    {
        uint64_t avail = 0;
        const uint8_t* b = CpuOf(sceneVa, 768, &avail);
        const size_t lim = (size_t)std::min<uint64_t>(avail, views[sceneVa].first);
        LOGI("[mcensus] scene block %llx (%u bytes, by %s), as 4x4 rows:", (unsigned long long)sceneVa, views[sceneVa].first,
             ThreadLabel(views[sceneVa].second).c_str());
        for (size_t o = 0; b && o + 64 <= lim; o += 64) {
            float f[16];
            memcpy(f, b + o, 64);
            LOGI("[mcensus]   +%03zu: %10.4f %10.4f %10.4f %10.4f | %10.4f %10.4f %10.4f %10.4f | %10.4f %10.4f %10.4f %10.4f | "
                 "%10.4f %10.4f %10.4f %10.4f", o, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                 f[12], f[13], f[14], f[15]);
        }
    }
    const std::map<std::string, int> counts = CensusScan(views, V, P, cp, true);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&qf);
    LOGI("[mcensus] %zu views read in %.1f ms - matrices of the camera found:%s", views.size(),
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart, CensusSum(counts).c_str());
    PairCensus(views, cp);
    {
        const float none[3] = {};
        wchar_t name[64];
        swprintf(name, 64, L"frame-%d-before.bin", g_mcDumps);
        DumpFrameConstants(views, V, P, cp, none, name);
    }
    // Kept for the second look, once this frame's replay - patch included - has run on the GPU.
    g_mcSave.views = views;
    g_mcSave.V = V;
    g_mcSave.P = P;
    memcpy(g_mcSave.cp, cp, 12);
    g_mcSave.dump = g_mcDumps++;
    g_mcSave.pending = true;
}

// Matrices that are another matrix of the frame with the camera position folded in: B = A . T(+-p) - same 3x3, B's
// translation = A's +- A3x3.p. A shader that works in camera-relative space (positions minus the camera) takes the
// camera into such a matrix; the patch moves neither A nor B, so the right eye would sample whatever B maps - shadow
// cascades are the usual case. Also where the plain copies of the camera position live.
void PairCensus(const CensusViews& views, const float cp[3]) {
    struct Aff { uint64_t va; uint32_t off, size; float m[3][4]; };
    std::map<std::array<int, 9>, std::vector<Aff>> by3x3;
    int posCopies = 0, posLogged = 0, affine = 0;
    std::vector<uint8_t> buf;
    for (auto& [va, st] : views) {
        const uint32_t size = st.first;
        if (size < 16 || size > 65536) continue;
        uint64_t avail = 0;
        const uint8_t* b = CpuOf(va, 16, &avail);
        if (!b) continue;
        const size_t lim = (size_t)std::min<uint64_t>(avail, size);
        buf.resize(lim);
        memcpy(buf.data(), b, lim);
        for (size_t o = 0; o + 12 <= lim; o += 4) {   // vec3 copies of p, any 4-byte alignment
            float v[3];
            memcpy(v, buf.data() + o, 12);
            bool same = true;
            for (int i = 0; i < 3; ++i) same = same && std::fabs(v[i] - cp[i]) <= 1e-4f * std::max(1.f, std::fabs(cp[i]));
            if (!same) continue;
            ++posCopies;
            if (posLogged++ < 24) {
                std::string ctx;
                const size_t from = o >= 32 ? (o - 32) & ~size_t(15) : 0;
                for (size_t k = from; k + 16 <= std::min(lim, from + 96); k += 16) {
                    float f[4];
                    memcpy(f, buf.data() + k, 16);
                    char t[96];
                    snprintf(t, sizeof t, " [+%zu %.4g %.4g %.4g %.4g]", k, f[0], f[1], f[2], f[3]);
                    ctx += t;
                }
                LOGI("[mcensus] camera position copy at %llx+%zu (size %u, by %s):%s", (unsigned long long)va, o, size,
                     ThreadLabel(st.second).c_str(), ctx.c_str());
            }
        }
        for (size_t o = 0; o + 64 <= lim; o += 16) {   // affine 4x4s by rows: last row (0,0,0,1)
            float m[4][4];
            memcpy(m, buf.data() + o, 64);
            if (!(std::fabs(m[3][0]) < 1e-6f && std::fabs(m[3][1]) < 1e-6f && std::fabs(m[3][2]) < 1e-6f &&
                  std::fabs(m[3][3] - 1.f) < 1e-6f)) continue;
            float sum = 0.f;
            bool finite = true;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) {
                finite = finite && std::isfinite(m[i][j]);
                if (j < 3) sum += std::fabs(m[i][j]);
            }
            if (!finite || sum < 1e-3f) continue;
            ++affine;
            std::array<int, 9> key;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) key[i * 3 + j] = (int)std::lround(m[i][j] * 1e4f);
            Aff a{va, (uint32_t)o, size, {}};
            memcpy(a.m, m, sizeof a.m);
            auto& v = by3x3[key];
            if (v.size() < 64) v.push_back(a);
        }
    }
    int pairs = 0, logged = 0;
    for (auto& [key, list] : by3x3) {
        for (size_t i = 0; i < list.size(); ++i) for (size_t j = 0; j < list.size(); ++j) {
            if (i == j) continue;
            const Aff& A = list[i];
            const Aff& B = list[j];
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                bool ok = true;
                for (int r = 0; r < 3 && ok; ++r) {
                    const float want = A.m[r][3] + sgn * (A.m[r][0] * cp[0] + A.m[r][1] * cp[1] + A.m[r][2] * cp[2]);
                    ok = std::fabs(B.m[r][3] - want) <= 1e-3f * std::max(1.f, std::fabs(want)) &&
                         std::fabs(B.m[r][3] - A.m[r][3]) > 1e-3f;
                }
                if (!ok) continue;
                ++pairs;
                if (logged++ < 20)
                    LOGI("[mcensus] camera folded in: %llx+%u (size %u) = %llx+%u (size %u) . T(%sp); rows %.4f %.4f %.4f %.4f | "
                         "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", (unsigned long long)B.va, B.off, B.size,
                         (unsigned long long)A.va, A.off, A.size, sgn > 0 ? "+" : "-", B.m[0][0], B.m[0][1], B.m[0][2],
                         B.m[0][3], B.m[1][0], B.m[1][1], B.m[1][2], B.m[1][3], B.m[2][0], B.m[2][1], B.m[2][2], B.m[2][3]);
            }
        }
    }
    LOGI("[mcensus] %d copies of the camera position, %d affine matrices, %d pairs with the camera folded in", posCopies,
         affine, pairs);
}

// The census's second look: the same views after the GPU patch. Every camera matrix should now be the RIGHT eye's
// (the camera moved by the patch's offset); any still matching the left eye's camera is one the patch missed.
void MatrixCensusAfter(const float delta[3]) {
    g_mcSave.pending = false;
    M4 VR = g_mcSave.V;
    float pr[3];
    for (int i = 0; i < 3; ++i) pr[i] = g_mcSave.cp[i] + delta[i];
    for (int i = 0; i < 3; ++i)
        VR.m[i][3] = -(VR.m[i][0] * pr[0] + VR.m[i][1] * pr[1] + VR.m[i][2] * pr[2]);
    const auto right = CensusScan(g_mcSave.views, VR, g_mcSave.P, pr, false);
    const auto left = CensusScan(g_mcSave.views, g_mcSave.V, g_mcSave.P, g_mcSave.cp, false);
    LOGI("[mcensus] after the patch (offset %.4f %.4f %.4f) - the right eye's camera:%s | still the left eye's:%s", delta[0],
         delta[1], delta[2], CensusSum(right).c_str(), CensusSum(left).c_str());
    if (g_mcSave.dump >= 0) {
        wchar_t name[64];
        swprintf(name, 64, L"frame-%d-after.bin", g_mcSave.dump);
        DumpFrameConstants(g_mcSave.views, g_mcSave.V, g_mcSave.P, g_mcSave.cp, delta, name);
    }
}

// Every 16-byte-aligned 4x4 of every view, classified against one camera (view V with position cp, projection P).
std::map<std::string, int> CensusScan(const std::map<uint64_t, std::pair<uint32_t, DWORD>>& views, const M4& V, const M4& P,
                                      const float cp[3], bool verbose) {
    // Test points: X in the world (5 m ahead, 1 m right, 0.5 m up), its view-space and clip-space images.
    float X[4], Xv[4], C[4];
    for (int i = 0; i < 3; ++i) X[i] = cp[i] + 5.f * V.m[2][i] + 1.f * V.m[0][i] + 0.5f * V.m[1][i];
    X[3] = 1.f;
    Mul4(V, X, Xv);
    const M4 VP = MulMM(P, V);
    Mul4(VP, X, C);
    if (verbose)
        LOGI("[mcensus] camera (%.4f %.4f %.4f); test point view (%.3f %.3f %.3f %.3f) clip (%.3f %.3f %.3f %.3f)", cp[0],
             cp[1], cp[2], Xv[0], Xv[1], Xv[2], Xv[3], C[0], C[1], C[2], C[3]);
    std::map<std::string, int> counts;
    int logged = 0, dumped = 0;
    std::vector<uint8_t> buf;
    for (auto& [va, st] : views) {
        size_t firstHit = SIZE_MAX, lastHit = 0;
        int hits = 0;
        const uint32_t size = st.first;
        if (size < 64 || size > 65536) continue;
        uint64_t avail = 0;
        const uint8_t* b = CpuOf(va, 64, &avail);
        if (!b) continue;
        const size_t lim = (size_t)std::min<uint64_t>(avail, size);
        buf.resize(lim);
        memcpy(buf.data(), b, lim);   // one sequential pass over write-combined memory
        for (size_t o = 0; o + 64 <= lim; o += 16) {
            M4 M;
            memcpy(M.m, buf.data() + o, 64);
            const M4 T = Transposed(M);
            float out[4];
            const char* kind = nullptr;
            if (Mul4(M, C, out), SamePoint(out, X, 0.02f)) kind = "inverse view-projection (rows)";
            else if (Mul4(T, C, out), SamePoint(out, X, 0.02f)) kind = "inverse view-projection (columns)";
            else if (Mul4(M, Xv, out), SamePoint(out, X, 0.005f) && std::fabs(out[3] - 1.f) < 1e-3f) kind = "inverse view (rows)";
            else if (Mul4(T, Xv, out), SamePoint(out, X, 0.005f) && std::fabs(out[3] - 1.f) < 1e-3f) kind = "inverse view (columns)";
            else if (Mul4(M, X, out), Same4(out, Xv, 1e-3f)) kind = "view (rows)";
            else if (Mul4(T, X, out), Same4(out, Xv, 1e-3f)) kind = "view (columns)";
            else if (Mul4(M, X, out), Same4(out, C, 1e-3f)) kind = "view-projection (rows)";
            else if (Mul4(T, X, out), Same4(out, C, 1e-3f)) kind = "view-projection (columns)";
            if (!kind) continue;
            const int seen = ++counts[kind];
            ++hits;
            firstHit = std::min(firstHit, o);
            lastHit = std::max(lastHit, o);
            if (!verbose) continue;
            if (logged++ < 60)
                LOGI("[mcensus] %s at %llx+%zu (view size %u, by %s)", kind, (unsigned long long)va, o, size,
                     ThreadLabel(st.second).c_str());
            if (seen <= 3)   // the first few of each kind, as stored
                for (int r = 0; r < 4; ++r)
                    LOGI("[mcensus]     %s #%d row %d: %12.5f %12.5f %12.5f %12.5f", kind, seen, r, M.m[r][0], M.m[r][1],
                         M.m[r][2], M.m[r][3]);
        }
        // Views with matches close together: the raw rows around them, to tell real matrices from overlaps.
        if (verbose && hits >= 2 && lastHit - firstHit < 256 && dumped < 10) {
            ++dumped;
            const size_t from = firstHit >= 16 ? firstHit - 16 : 0, to = std::min(lim, lastHit + 96);
            LOGI("[mcensus] rows of %llx (size %u) from +%zu:", (unsigned long long)va, size, from);
            for (size_t o = from; o + 16 <= to; o += 16) {
                float f[4];
                memcpy(f, buf.data() + o, 16);
                LOGI("[mcensus]   +%04zu: %12.5f %12.5f %12.5f %12.5f", o, f[0], f[1], f[2], f[3]);
            }
        }
    }
    return counts;
}

std::vector<std::pair<uint64_t, uint32_t>> TakeFrameViews() {
    {
        float cam[16];
        if (GameERRenderCamera(cam, nullptr)) memcpy(g_camForRc, cam + 12, 12);
    }
    const bool census = g_mcensusArm.exchange(false);
    {
        static int s_tracePresents = -1;
        if (census && s_tracePresents < 0) { g_traceN.store(0); g_traceOn.store(true); s_tracePresents = 0; }
        if (s_tracePresents >= 0 && g_traceOn.load()) {
            TraceNote('P', 0, 0);
            if (++s_tracePresents >= 7) {
                g_traceOn.store(false);
                std::wstring path = Cfg().iniPath;
                const size_t sl = path.find_last_of(L"\\/");
                path = (sl == std::wstring::npos ? std::wstring() : path.substr(0, sl + 1)) + L"cbv-trace.csv";
                if (FILE* f = _wfopen(path.c_str(), L"w")) {
                    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
                    const int n = std::min(g_traceN.load(), kTrace);
                    fprintf(f, "kind,us,ringoff,size,tid\n");
                    const long long t0 = n ? g_trace[0].t : 0;
                    for (int i = 0; i < n; ++i) {
                        const TraceEv& e = g_trace[i];
                        fprintf(f, "%c,%.1f,%lld,%u,%lu\n", e.kind, (double)(e.t - t0) * 1e6 / (double)fq.QuadPart,
                                e.va ? (long long)(e.va - g_ring.gpu) : -1ll, e.size, e.tid);
                    }
                    fclose(f);
                    LOGI("[mcensus] creation trace: %d events over 7 presents -> cbv-trace.csv", n);
                }
                s_tracePresents = 99;
            }
        }
    }
    const int n = std::min(g_frameCbvN.exchange(0), kFrameCbv);
    const int cut = std::min(std::max(g_cbvAtLastExec.exchange(0), 0), n);
    static std::vector<std::pair<uint64_t, uint32_t>> s_carry;   // made after the last frame's final submission: this frame's
    std::vector<std::pair<uint64_t, uint32_t>> vas;
    vas.reserve(n + s_carry.size());
    vas.insert(vas.end(), s_carry.begin(), s_carry.end());
    g_carried.fetch_add((long)s_carry.size(), std::memory_order_relaxed);
    s_carry.clear();
    for (int i = 0; i < cut; ++i) vas.push_back({g_frameCbv[i].va, g_frameCbv[i].size});
    for (int i = cut; i < n; ++i) s_carry.push_back({g_frameCbv[i].va, g_frameCbv[i].size});
    std::sort(vas.begin(), vas.end());
    vas.erase(std::unique(vas.begin(), vas.end()), vas.end());
    if (census) MatrixCensus(vas);   // exactly the views this frame's patch gets
    g_rootCbvN.store(0);
    return vas;
}

// Every view the game created this frame, patched. Slow on purpose (it reads them all): the experiment
// proves the patch makes a correct second eye before the cheap way of finding the camera buffers.
void PatchFrameConstants(const CamPatch& c) {
    const int n = std::min(g_frameCbvN.exchange(0), kFrameCbv);
    std::vector<std::pair<uint64_t, uint32_t>> vas;
    vas.reserve(n);
    for (int i = 0; i < n; ++i) vas.push_back({g_frameCbv[i].va, g_frameCbv[i].size});
    std::sort(vas.begin(), vas.end());
    vas.erase(std::unique(vas.begin(), vas.end()), vas.end());
    static ULONGLONG s_rangeLog = 0;
    if (!vas.empty() && GetTickCount64() - s_rangeLog > 5000) {
        s_rangeLog = GetTickCount64();
        std::map<uint64_t, std::pair<uint64_t, long>> bufs;   // containing buffer base -> (size, views)
        AcquireSRWLockShared(&g_bufLock);
        for (auto& [va, sz] : vas)
            if (const CpuBuffer* b = FindBufferLocked(va)) { auto& e = bufs[b->gpu]; e.first = b->size; ++e.second; }
        ReleaseSRWLockShared(&g_bufLock);
        std::string s;
        for (auto& [base, e] : bufs) { char t[96]; snprintf(t, sizeof(t), " %llx(%llu KB):%ld", (unsigned long long)base, (unsigned long long)(e.first >> 10), e.second); s += t; }
        LOGI("[replay] this frame's views span %llx..%llx (%llu KB) in buffers:%s", (unsigned long long)vas.front().first,
             (unsigned long long)(vas.back().first + vas.back().second),
             (unsigned long long)((vas.back().first + vas.back().second - vas.front().first) >> 10), s.c_str());
    }
    for (auto& [va, sz] : vas) {
        if (sz > 4096) continue;
        uint64_t avail = 0;
        uint8_t* p = CpuOf(va, 16, &avail);
        if (!p) continue;
        if (PatchBuffer(p, (uint32_t)std::min<uint64_t>(sz, avail), c)) g_patchedBuffers.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecycleShadows() {
    const uint64_t done = g_replayFence ? g_replayFence->GetCompletedValue() : 0;
    {
        std::lock_guard<std::mutex> g(g_poolMtx);
        for (size_t i = 0; i < g_inflight.size();) {
            if (g_inflight[i].second <= done) {
                ShadowDropUseLocked(g_inflight[i].first);
                g_inflight[i] = g_inflight.back();
                g_inflight.pop_back();
            } else {
                ++i;
            }
        }
    }
    // Submitted shadows the game neither reset nor executed again for a while: their list was destroyed or
    // re-recorded on a thread we do not shadow. Out of the map, so the pool does not leak them.
    static ULONGLONG s_lastSweep = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastSweep < 1000) return;
    s_lastSweep = now;
    AcquireSRWLockExclusive(&g_mapLock);
    {
        std::lock_guard<std::mutex> g(g_poolMtx);
        for (auto it = g_shadowOf.begin(); it != g_shadowOf.end();) {
            Shadow* s = it->second;
            if (s->submitted && !s->uses && now - s->lastUse > 3000) {
                ShadowFreeLocked(s);
                it = g_shadowOf.erase(it);
                g_staleDropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_mapLock);
}

// The frame's camera, taken when its engine pass ends (every camera writer and the game's smoothing done):
// the renderer drew the frame from exactly this, so the constants carry exactly these values. Read at the
// next present - which is where that frame is shown (ELDEN RING presents a frame inside the next one's
// DrawBegin).
struct FrameCamera { float cam[16]; float delta[3]; bool valid = false; };
std::mutex g_frameCamMtx;
FrameCamera g_frameCam;

bool FrameCamPatch(CamPatch& c) {
    FrameCamera fc;
    { std::lock_guard<std::mutex> g(g_frameCamMtx); fc = g_frameCam; }
    if (!fc.valid) return false;
    for (int i = 0; i < 3; ++i) {
        c.r[i] = fc.cam[i]; c.u[i] = fc.cam[4 + i]; c.f[i] = fc.cam[8 + i]; c.p[i] = fc.cam[12 + i];
        c.pr[i] = c.p[i] + fc.delta[i];
    }
    return true;
}

// Present thread, inside the present event: everything the game submitted for the frame being presented has
// been submitted (its GPU work lands ~3 ms before the present that shows it). `submit`: replay the frame as
// the second eye (else its shadows are dropped). `useFrameCamera`: the stereo mode's real second eye; the
// bench levels move the current camera by [Experimental] ReplayEyeOffset along its right axis.
// True when the replay went on the queue with its camera patch.
bool ReplayAtPresent(const Dx12Frame& f, bool submit = true, bool useFrameCamera = false) {
    if (!g_replayFence && f.device && FAILED(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_replayFence))))
        g_replayFence = nullptr;
    RecycleShadows();
    std::vector<Shadow*> batch;
    { std::lock_guard<std::mutex> g(g_batchMtx); batch.swap(g_batch); }
    const bool incomplete = g_frameIncomplete.exchange(false);
    const bool flushedEarly = g_heldFlushedEarly.exchange(false);
    // Not across a world transition: loading screens and teardown free resources wholesale.
    const bool worldSteady = GameERPlayerActive() && !GameERLoadingScreen() && !GameERWorldDying();
    // A frame with a list we hold no shadow of cannot be replayed whole, and one whose fence signals the
    // watchdog already released may reference freed memory: skip it (its shadows go straight back).
    bool replay = submit && g_batchHeld && !batch.empty() && !incomplete && !flushedEarly && worldSteady && g_replayFence &&
                  ReplayLevel() > 0;
    ID3D12CommandList* patchList = nullptr;
    float eyeDelta[3] = {};   // the patch's offset this frame (for the census's second look)
    const int benchPhase = (!useFrameCamera && ReplayLevel() >= 3) ? BenchPhase() : -1;
    const int capStep = (!useFrameCamera && ReplayLevel() >= 3) ? BenchCaptureStep() : 0;
    if (ReplayLevel() >= 3) {
        // The GPU patch: no wait, no CPU reads - a compute pass ahead of the replay in the same submission.
        std::vector<std::pair<uint64_t, uint32_t>> vas = TakeFrameViews();   // consumed every frame
        CamPatch c;
        bool haveCam = false;
        if (useFrameCamera) {
            haveCam = FrameCamPatch(c);
        } else {
            float cam[16] = {}, fov = 0.f;
            if ((haveCam = GameERRenderCamera(cam, &fov))) {
                const float off = (benchPhase == 0 || capStep >= 2) ? 0.f : Cfg().replayEyeOffset;
                for (int i = 0; i < 3; ++i) {
                    c.r[i] = cam[i]; c.u[i] = cam[4 + i]; c.f[i] = cam[8 + i]; c.p[i] = cam[12 + i];
                    c.pr[i] = c.p[i] + c.r[i] * off;
                }
            }
        }
        if (replay && haveCam && GpuPatcherInit(f.device)) patchList = GpuPatchList(c, vas);
        for (int i = 0; i < 3; ++i) eyeDelta[i] = c.pr[i] - c.p[i];
        // The stereo mode replays only with its camera patch: without it the "second eye" is the first again.
        if (useFrameCamera && !patchList) replay = false;
    } else if (replay && ReplayLevel() == 2) {
        // The second eye's constants overwrite the first's in place: wait until the GPU has finished the
        // first run, then patch (the ring region is not rewritten before this frame's signals are released).
        float cam[16] = {}, fov = 0.f;
        if (GameERRenderCamera(cam, &fov)) {
            g_origSignal(f.queue, g_replayFence, ++g_replayFenceVal);
            if (g_replayFence->GetCompletedValue() < g_replayFenceVal) {
                static HANDLE s_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                g_replayFence->SetEventOnCompletion(g_replayFenceVal, s_ev);
                WaitForSingleObject(s_ev, 200);
            }
            CamPatch c;
            for (int i = 0; i < 3; ++i) {
                c.r[i] = cam[i]; c.u[i] = cam[4 + i]; c.f[i] = cam[8 + i]; c.p[i] = cam[12 + i];
                c.pr[i] = c.p[i] + c.r[i] * Cfg().replayEyeOffset;
            }
            PatchFrameConstants(c);
        }
    }
    if (replay) {
        std::vector<ID3D12CommandList*> lists;
        lists.reserve(batch.size() + 4);
        // Bench plan phases 0 and 1: the game's own frame goes back over the replay's before the present.
        ID3D12CommandList* showPre = nullptr;
        ID3D12CommandList* showPost = nullptr;
        if (patchList && (benchPhase == 0 || benchPhase == 1)) {
            showPre = BenchShowList(f.device, f.backBuffer, 0, g_patcher.lastSlot);
            showPost = showPre ? BenchShowList(f.device, f.backBuffer, 1, g_patcher.lastSlot) : nullptr;
            if (!showPost) showPre = nullptr;   // never one without the other: the copy's states pair up
        }
        std::vector<BenchGrab> grabs;   // bench capture: the game's frame ahead of the replay, the replay's behind it
        if (capStep == 1 || capStep == 3 || capStep == 5)
            if (ID3D12CommandList* g = BenchGrabList(f, capStep == 1   ? L"cap-1-game.ppm"
                                                        : capStep == 3 ? L"cap-3-game-moved.ppm"
                                                                       : L"cap-4-game-back.ppm", grabs))
                lists.push_back(g);
        if (showPre) lists.push_back(showPre);
        if (patchList) lists.push_back(patchList);
        // The fog's history volumes: the right eye's in for the replay, the left eye's back after it (see EyeVolume).
        ID3D12CommandList* volIn = nullptr;
        ID3D12CommandList* volOut = nullptr;
        if (patchList && !(capStep == 1 && Cfg().replayTwice)) VolSwapLists(f.device, batch, &volIn, &volOut);
        if (volIn) lists.push_back(volIn);
        for (Shadow* s : batch) lists.push_back(s->list);
        if (volOut) lists.push_back(volOut);
        // Bench capture, [Experimental] ReplayTwice: the frame replayed a second time on the capture frame - the second run
        // reads whatever the first left behind (a texture the frame reads before it writes it holds the right eye's
        // version then, not the game's). The first run's image is cap-2-replay.ppm, the second's cap-2b-replay-again.ppm.
        if (capStep == 1 && Cfg().replayTwice && patchList) {
            if (ID3D12CommandList* g = BenchGrabList(f, L"cap-2-replay.ppm", grabs)) lists.push_back(g);
            g_origExec(f.queue, (UINT)lists.size(), lists.data());
            g_origSignal(f.queue, g_replayFence, ++g_replayFenceVal);
            WaitReplayFence(2000);   // a list may not be submitted again while it is still executing
            lists.clear();
            for (Shadow* s : batch) lists.push_back(s->list);
        }
        // The game's constants back as they were - on the matrix census's frame only the patch marks, as its second
        // look reads the patched values after this replay.
        if (patchList)
            if (ID3D12CommandList* r = GpuRestoreList(g_mcSave.pending)) lists.push_back(r);
        if (patchList)
            if (ID3D12CommandList* t = GpuTimingTail(f.queue)) lists.push_back(t);
        if (showPost) lists.push_back(showPost);
        if (capStep == 1)
            if (ID3D12CommandList* g = BenchGrabList(f, Cfg().replayTwice ? L"cap-2b-replay-again.ppm" : L"cap-2-replay.ppm", grabs))
                lists.push_back(g);
        g_origExec(f.queue, (UINT)lists.size(), lists.data());
        if (capStep == 3) g_benchCamShift.store(0.f);
        g_origSignal(f.queue, g_replayFence, ++g_replayFenceVal);
        BenchGrabWrite(std::move(grabs), g_replayFenceVal);
        g_replays.fetch_add(1, std::memory_order_relaxed);
        g_replayLists.fetch_add((long)lists.size(), std::memory_order_relaxed);
        if (g_mcSave.pending && patchList) {   // the census's second look: wait for this replay, read what it drew from
            if (g_replayFence->GetCompletedValue() < g_replayFenceVal) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev && SUCCEEDED(g_replayFence->SetEventOnCompletion(g_replayFenceVal, ev))) WaitForSingleObject(ev, 500);
                if (ev) CloseHandle(ev);
            }
            MatrixCensusAfter(eyeDelta);
        }
    } else if (!batch.empty()) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_mcSave.pending) {   // this frame was not replayed with its patch: look again at the next one
        g_mcSave.pending = false;
        g_mcensusArm.store(true);
    }
    // The game's own signals for the frame go in behind the replay: its idea of "the GPU is done with that
    // frame" now includes the second run.
    { std::lock_guard<std::mutex> g(g_heldMtx); ReleaseHeldLocked(); }
    std::lock_guard<std::mutex> g(g_poolMtx);
    for (Shadow* s : batch) {
        if (replay) { s->fence = g_replayFenceVal; g_inflight.push_back({s, g_replayFenceVal}); }
        else ShadowDropUseLocked(s);
    }
    return replay && patchList;
}

// No present for a while with signals held (a stall, a menu that stops presenting): let them go so the
// game never waits on us. The frame they belonged to is then not replayed.
void SignalWatchdog() {
    static const long long f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x.QuadPart; }();
    for (;;) {
        Sleep(2);
        const long long since = g_heldSince.load(std::memory_order_relaxed);
        if (!since || (Qpc() - since) * 1000 / f < 60) continue;
        std::lock_guard<std::mutex> g(g_heldMtx);
        if (g_held.empty()) continue;
        ReleaseHeldLocked();
        g_heldFlushedEarly.store(true);
        g_watchdogFlushes.fetch_add(1, std::memory_order_relaxed);
    }
}

// Why the newest frame could not be replayed (present thread), and how often each reason held over 5 s.
enum Blocker { BlkNone, BlkNotHeld, BlkEmpty, BlkIncomplete, BlkWatchdog, BlkWorld, BlkCamera, BlkPatch, BlkSlot, BlkCount };
const char* const kBlockerText[BlkCount] = {"", "fence signals not held", "nothing submitted", "a list without a shadow",
                                            "signals released by the watchdog", "world not steady", "no frame camera",
                                            "no camera patch", "patch slot busy"};
Blocker g_blocker = BlkNone;
std::atomic<long> g_blockerN[BlkCount];

void ReplayStats() {
    static ULONGLONG s_last = GetTickCount64();
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 5000) return;
    s_last = now;
    size_t inflight = 0, free = 0; int total = 0;
    { std::lock_guard<std::mutex> g(g_poolMtx); inflight = g_inflight.size(); free = g_free.size(); total = g_shadowCount; }
    const long r = g_replays.exchange(0), rl = g_replayLists.exchange(0);
    LOGI("[replay] 5 s: %ld frames replayed (%ld lists), %ld skipped, %ld lists recorded without a shadow, %ld submitted "
         "without one, %ld watchdog signal releases | shadows %d (%zu in flight, %zu free)%s", r, rl, g_skipped.exchange(0),
         g_shadowMisses.exchange(0), g_unshadowedExecs.exchange(0), g_watchdogFlushes.exchange(0), total, inflight, free,
         g_dev ? "" : " NO DEVICE");
    if (ReplayLevel() >= 2)
        LOGI("[replay] 5 s camera patch: %ld buffers, %ld matrix rows, %ld positions", g_patchedBuffers.exchange(0),
             g_patchedRows.exchange(0), g_patchedPos.exchange(0));
    {
        // What kept frames from being replayed, and where the unshadowed lists came from.
        std::string why;
        for (int b = 1; b < BlkCount; ++b)
            if (const long c = g_blockerN[b].exchange(0)) { char t[80]; snprintf(t, sizeof t, ", %s %ld", kBlockerText[b], c); why += t; }
        const long never = g_unshNever.exchange(0), elsewhere = g_unshElsewhere.exchange(0);
        const long reexec = g_reexecServed.exchange(0), created = g_createdOpen.exchange(0), stale = g_staleDropped.exchange(0);
        if (!why.empty() || never || elsewhere || reexec || created || stale) {
            std::string who;
            const int k = std::min(g_unshNoteN.exchange(0), 8);
            for (int i = 0; i < k; ++i) {
                who += " [" + ThreadLabel(g_unshNotes[i].tid) + (g_unshNotes[i].never ? ", never shadowed]" : "]");
            }
            LOGI("[replay] 5 s not replayable:%s | lists without a shadow: %ld never shadowed, %ld shadowed before but "
                 "not this time%s%s | re-executions served by a kept shadow %ld, lists created open and shadowed %ld, "
                 "stale shadows dropped %ld, submissions on the game's other queues %ld", why.empty() ? " -" : why.c_str() + 1,
                 never, elsewhere, who.empty() ? "" : "; by", who.c_str(), reexec, created, stale,
                 g_otherQueueExecs.exchange(0));
        } else {
            g_unshNoteN.store(0);
        }
    }
    if (const long pf = g_patchFrames.exchange(0)) {
        LOGI("[replay] 5 s camera patch on the GPU: %ld frames, the frame's own camera found in %ld, %ld with nothing "
             "moved (right eye from the left eye's spot) | per frame %.1f view rows, %.1f inverse-view rows, %.1f positions, "
             "%.1f column-stored view translations, %.1f inverse view-projections", pf, g_patchCamFound.exchange(0),
             g_patchNone.exchange(0), (double)g_patchRows.exchange(0) / pf, (double)g_patchInv.exchange(0) / pf,
             (double)g_patchPos.exchange(0) / pf, (double)g_patchVcol.exchange(0) / pf, (double)g_patchIvp.exchange(0) / pf);
        LOGI("[replay] 5 s camera patch: views made after a frame's last submission, patched with the next frame: %.1f a "
             "frame | constants staged in the ring and copied to GPU-only buffers (shadow cascades, light pass): %.1f a frame "
             "(%.1f more in lists the replay never runs, left alone)",
             (double)g_carried.exchange(0) / pf, (double)g_copySources.exchange(0) / pf,
             (double)g_copySourcesElsewhere.exchange(0) / pf);
        LOGI("[replay] 5 s per-eye volumes (the fog's history): %.1f swapped a frame of %d known, %lld K texels each way",
             (double)g_volSwaps.exchange(0) / pf, g_volResN.load(), g_volBytes.load() >> 10);
        LOGI("[replay] 5 s camera patch, view-space matrices (sun shadow lookup, decal and light-volume boxes) moved %.1f a "
             "frame | %.0f values a frame put back after the replay | the frame's ring span %.0f KB (%.0f chunks) a frame%s",
             (double)g_patchFold.exchange(0) / pf, (double)g_undoEntries.exchange(0) / pf,
             (double)g_spanBytes.exchange(0) / 1024.0 / pf, (double)g_spanChunks.exchange(0) / pf,
             g_undoOverflow.load() ? " - THE UNDO LOG OVERFLOWED (values past it were not patched)" : "");
        g_undoOverflow.store(0);
        const long fo = g_framesOther.exchange(0);
        LOGI("[replay] 5 s cameras per frame: %.1f scene blocks a frame, %.2f of them drawn with a camera other than the "
             "one the patch matched (in %ld of %ld frames; farthest %.1f mm, %.2f deg; same spot, other rotation %.2f deg) | "
             "a frame: %.2f off by 0.1-1 deg, %.2f by 1-10, %.2f by 10-90, %.2f by more, %.2f moved >1 mm",
             (double)g_sceneBlocks.exchange(0) / pf, (double)g_sceneOther.exchange(0) / pf, fo, pf,
             g_otherMaxPos.exchange(0.f) * 1000.0, (double)g_otherMaxDeg.exchange(0.f), (double)g_otherMaxRotDeg.exchange(0.f),
             (double)g_otherBucket[0].exchange(0) / pf, (double)g_otherBucket[1].exchange(0) / pf,
             (double)g_otherBucket[2].exchange(0) / pf, (double)g_otherBucket[3].exchange(0) / pf,
             (double)g_otherBucket[4].exchange(0) / pf);
    }
    if (const long n = g_gpuTimed.exchange(0)) {
        const long nl = g_gpuLeftN.exchange(0);
        const long long l = g_gpuLeftUs.exchange(0);
        LOGI("[replay] 5 s GPU ms per frame: left eye %.2f (the game's frame + our blits: previous replay's end to "
             "this patch, %ld consecutive frames), camera patch %.2f, right-eye replay %.2f (%ld frames timed)",
             nl ? (double)l / 1000.0 / nl : 0.0, nl, (double)g_gpuPatchUs.exchange(0) / 1000.0 / n,
             (double)g_gpuReplayUs.exchange(0) / 1000.0 / n, n);
    }
    if (g_skipShadowMap) LOGI("[replay] 5 s shadow-map draws and clears left out of the replay: %ld", g_shadowMapSkips.exchange(0));
    if (g_dev) {
        const HRESULT hr = g_dev->GetDeviceRemovedReason();
        if (hr != S_OK) LOGE("[replay] DEVICE REMOVED: 0x%08lX", (unsigned long)hr);
    }
}

}  // namespace

void ReplayStereoOnPresent(const Dx12Frame& f) {
    static bool s_installed = false;
    if (!s_installed) {
        s_installed = true;
        if (Cfg().replayCensus || ReplayLevel() > 0) {
            g_dev = f.device;
            if (Cfg().replayCensus || ReplayLevel() >= 2) InstallDeviceHooks(f.device);   // no-op if creation did
            if (ReplayLevel() >= 2) g_frameCbvArmed.store(true);   // the patch needs every view the frame makes
            InstallQueueHooks(f.queue);
            if (ReplayLevel() > 0) {
                g_shadowOn.store(true);
                g_holdSignals.store(true);
                std::thread(SignalWatchdog).detach();
                LOGI("[replay] single-frame stereo: shadow recording on, the game's fence signals held until each "
                     "frame's replay (ReplayStereo=%d)", ReplayLevel());
            }
        }
    }
    if (ReplayLevel() >= 3 && Cfg().replayMatrixCensus > 0.f) {
        // Three frames' camera matrices, 0.7 s apart. At the bench levels the game's own camera is moved 0, 0.5 and 1 m
        // along its right axis for them (0.3 s ahead, so the frame dumped is drawn from there): what moves with it is
        // camera-dependent, and how it moves says how the camera is folded in.
        static ULONGLONG s_active = 0;
        static int s_armed = 0, s_shifted = 0;
        if (!GameERPlayerActive()) s_active = 0;
        else if (!s_active) s_active = GetTickCount64();
        else if (s_armed < 3) {
            const ULONGLONG at = (ULONGLONG)(Cfg().replayMatrixCensus * 1000.f) + 700ull * s_armed, in = GetTickCount64() - s_active;
            if (!ReplayIsStereoMode() && s_shifted == s_armed && in + 300 > at) {
                g_benchShiftDiag.store(true);
                g_benchCamShift.store(0.5f * s_armed);
                ++s_shifted;
            }
            if (in > at) {
                ++s_armed;
                g_mcensusArm.store(true);
            }
        } else if (!ReplayIsStereoMode() && s_shifted == 3 && GetTickCount64() - s_active >
                   (ULONGLONG)(Cfg().replayMatrixCensus * 1000.f) + 2500ull) {
            g_benchCamShift.store(0.f);
            g_benchShiftDiag.store(false);
            s_shifted = 4;
        }
    }
    if (PassLogSeconds() > 0) {   // bench: one frame's commands (see PassRecord)
        static ULONGLONG s_active = 0;
        const int st = g_passLog.load();
        if (!GameERPlayerActive()) s_active = 0;
        else if (!s_active) s_active = GetTickCount64();
        if (st == 0 && s_active && GetTickCount64() - s_active > (ULONGLONG)PassLogSeconds() * 1000ull) {
            g_passLog.store(1);   // lists reset from here on keep their calls
            LOGI("[passlog] armed");
        } else if (st == 1) {
            { std::lock_guard<std::mutex> g(g_passMtx); g_passFrame.clear(); }
            g_passLog.store(2);   // what the game submits until the next present is the frame logged
        } else if (st == 2) {
            g_passLog.store(3);
            PassLogWrite();
        }
    }
    if (Cfg().replayDepthCensus > 0.f) {   // 120 frames of depth-target writes, once
        static ULONGLONG s_active = 0;
        static int s_state = 0;
        static long s_f0 = 0;
        const long frame = g_depthFrame.fetch_add(1) + 1;
        if (!GameERPlayerActive()) s_active = 0;
        else if (!s_active) s_active = GetTickCount64();
        if (s_state == 0 && s_active && GetTickCount64() - s_active > (ULONGLONG)(Cfg().replayDepthCensus * 1000.f)) {
            s_state = 1;
            s_f0 = frame;
            g_depthArmed.store(true);
            LOGI("[replay] depth-target census armed for 120 frames");
        } else if (s_state == 1 && frame - s_f0 >= 120) {
            s_state = 2;
            g_depthArmed.store(false);
            DumpDepthCensus(frame - s_f0);
        }
    }
    if (ReplayLevel() > 0 && g_origExec) {
        // Hold the game's fence signals only for frames that will be replayed: in the world, and (stereo mode)
        // with the headset showing the app. Boot, loading and title flows wait on those fences.
        const bool steady = GameERPlayerActive() && !GameERLoadingScreen() && !GameERWorldDying();
        const bool hold = steady && (!ReplayIsStereoMode() || StereoReplayVisible());
        g_batchHeld = g_holdSignals.load();   // the frame being presented was recorded under this decision
        if (g_holdSignals.exchange(hold) && !hold) { std::lock_guard<std::mutex> g(g_heldMtx); ReleaseHeldLocked(); }
        if (ReplayIsStereoMode()) g_presentPending = true;   // the stereo layer replays between its eye blits
        else ReplayAtPresent(f);                             // bench levels: right here
        ReplayStats();
    }
    if (!Cfg().replayCensus || !g_origExec) return;
    // Census windows, from 25 s after the player is in the world: six presents of queue events, then one
    // frame of command-list calls and root constant-buffer bindings.
    static ULONGLONG s_activeSince = 0;
    static int s_queueLeft = -1, s_listState = 0;
    if (!GameERPlayerActive()) { s_activeSince = 0; return; }
    if (!s_activeSince) s_activeSince = GetTickCount64();
    if (s_queueLeft < 0 && GetTickCount64() - s_activeSince > 25000) {
        g_evN.store(0);
        s_queueLeft = 7;
        g_armed.store(true);
        LOGI("[replay] queue census armed");
    }
    if (s_queueLeft > 0) {
        if (Ev* e = Rec(EvPresent)) e->a = f.queue;
        if (--s_queueLeft == 0) {
            g_armed.store(false);
            DumpQueueCensus(g_evN.load(), GetCurrentThreadId());
            s_listState = 1;
        }
        return;
    }
    if (s_listState == 1 && g_listHooked.load()) {   // arm: the next frame's recording is counted
        for (auto& c : g_calls) c.store(0);
        g_cbvN.store(0);
        g_tblN.store(0);
        g_descBase[0] = g_cbvCreates.load(); g_descBase[1] = g_srvUavCreates.load(); g_descBase[2] = g_copiesSimple.load();
        g_descBase[3] = g_copiesMulti.load(); g_descBase[4] = g_descCopied.load();
        g_listArmed.store(true);
        s_listState = 2;
    } else if (s_listState == 2) {                    // that frame's lists were all submitted before this present
        g_listArmed.store(false);
        s_listState = 3;
        DumpListCensus();
    } else if (s_listState >= 3 && s_listState < 7) { // three consecutive frames of created CBVs
        if (s_listState > 3) DumpFrameCbvCensus(s_listState - 3);
        g_frameCbvN.store(0);
        g_frameCbvArmed.store(s_listState < 6);
        ++s_listState;
    }
}

void ReplayStereoPresentScope(bool inside) { t_inErvrPresent = inside; }

bool ReplayStereoActive() { return ReplayIsStereoMode() && g_origExec && g_shadowOn.load(); }

bool ReplayStereoCanSubmit() {
    auto no = [](Blocker b) { g_blocker = b; g_blockerN[b].fetch_add(1, std::memory_order_relaxed); return false; };
    if (!g_presentPending || !g_batchHeld || !g_replayFence) return no(BlkNotHeld);
    { std::lock_guard<std::mutex> g(g_batchMtx); if (g_batch.empty()) return no(BlkEmpty); }
    if (g_frameIncomplete.load()) return no(BlkIncomplete);
    if (g_heldFlushedEarly.load()) return no(BlkWatchdog);
    if (!GameERPlayerActive() || GameERLoadingScreen() || GameERWorldDying()) return no(BlkWorld);
    { std::lock_guard<std::mutex> g(g_frameCamMtx); if (!g_frameCam.valid) return no(BlkCamera); }
    if (!g_ring.res || (g_patcher.tried && !g_patcher.ok)) return no(BlkPatch);
    if (g_patcher.ok && g_patcher.slotFence[g_patcher.slot] > g_replayFence->GetCompletedValue()) return no(BlkSlot);
    g_blocker = BlkNone;
    return true;
}

const char* ReplayStereoBlocker() { return kBlockerText[g_blocker]; }

bool ReplayStereoSubmitSecondEye(const Dx12Frame& f) {
    if (!g_presentPending) return false;
    g_presentPending = false;
    return ReplayAtPresent(f, true, true);
}

void ReplayStereoPresentEnd(const Dx12Frame& f) {
    if (!g_presentPending) return;   // the stereo layer replayed (or there is nothing to do)
    g_presentPending = false;
    ReplayAtPresent(f, false, true);  // no stereo this present: the frame's shadows go back, its signals go out
}

void ReplayStereoFrameEnd() {
    if (!ReplayIsStereoMode()) return;
    FrameCamera fc;
    fc.valid = GameERRenderCamera(fc.cam, nullptr) && GameERSecondEyeDelta(fc.delta);
    std::lock_guard<std::mutex> g(g_frameCamMtx);
    g_frameCam = fc;
}

int ReplayStereoLevel() { return ReplayLevel(); }

float ReplayStereoBenchCameraShift() { return g_benchCamShift.load(std::memory_order_relaxed); }
bool ReplayStereoBenchShiftSkew() { return g_benchShiftDiag.load(std::memory_order_relaxed); }

void ReplayStereoOnInitDevice(ID3D12Device* d) {
    if (Cfg().replayCensus || ReplayLevel() >= 2) InstallDeviceHooks(d);
}

void ReplayStereoOnInitResource(ID3D12Resource* r, int heap, bool buffer) {
    if (!buffer && r && ReplayLevel() >= 3) {   // the game's 3D UAV textures: per-eye volumes (the fog's history)
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            d.SampleDesc.Count == 1)
            VolAdd(r, d);
        return;
    }
    // CPU-visible buffers only (upload, ReBAR "gpu upload", custom): where per-frame constants live.
    if (!buffer || !r || !(heap == 2 || heap == 5 || heap == 6) || !(Cfg().replayCensus || ReplayLevel() >= 2)) return;
    const uint64_t gpu = r->GetGPUVirtualAddress();
    if (!gpu) return;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Width >= (1u << 20)) {   // the big ones: per-frame rings live in these
        D3D12_HEAP_PROPERTIES hp{}; D3D12_HEAP_FLAGS hf{};
        const HRESULT hr = r->GetHeapProperties(&hp, &hf);
        LOGI("[replay] CPU-visible buffer %p: %llu KB at gpu %llx, reshade heap %d, d3d12 heap type %d cpu page %d pool %d "
             "flags 0x%x (heap props %s), resource flags 0x%x", (void*)r, (unsigned long long)(d.Width >> 10),
             (unsigned long long)gpu, heap, (int)hp.Type, (int)hp.CPUPageProperty, (int)hp.MemoryPoolPreference, (unsigned)hf,
             SUCCEEDED(hr) ? "committed" : "placed?", (unsigned)d.Flags);
    }
    AcquireSRWLockExclusive(&g_bufLock);
    auto it = std::lower_bound(g_bufs.begin(), g_bufs.end(), gpu, [](const CpuBuffer& b, uint64_t v) { return b.gpu < v; });
    if (it == g_bufs.end() || it->gpu != gpu) g_bufs.insert(it, CpuBuffer{gpu, d.Width, r, nullptr});
    ReleaseSRWLockExclusive(&g_bufLock);
}

void ReplayStereoOnDestroyResource(ID3D12Resource* r) {
    if (!r || !(Cfg().replayCensus || ReplayLevel() >= 2)) return;
    if (g_volResN.load(std::memory_order_relaxed)) VolRemove(r);
    AcquireSRWLockExclusive(&g_bufLock);
    for (auto it = g_bufs.begin(); it != g_bufs.end(); ++it)
        if (it->res == r) { g_bufs.erase(it); break; }
    ReleaseSRWLockExclusive(&g_bufLock);
}

}

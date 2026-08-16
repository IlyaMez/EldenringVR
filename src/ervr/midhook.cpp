#include "midhook.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <cstring>
extern "C" {
#include <hde64.h>
}

namespace ervr {
namespace {

struct Hook {
    uint8_t* target = nullptr;
    size_t stolen = 0;
    uint8_t original[32]{};
    uint8_t* stub = nullptr;
    size_t stubSize = 0;
};

// Allocate RWX memory within +/-1 GB of `near` so rel32 jumps work both ways.
uint8_t* AllocNear(uint8_t* nearAddr, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t base = ((uintptr_t)nearAddr) & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x40000000; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t addr = sgn < 0 ? base - d : base + d;
            if (addr < (uintptr_t)si.lpMinimumApplicationAddress || addr > (uintptr_t)si.lpMaximumApplicationAddress) continue;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_FREE) continue;
            void* p = VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

struct Emitter {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void bytes(std::initializer_list<uint8_t> l) { for (auto v : l) b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    size_t size() const { return b.size(); }
};

// movups [rsp+disp], xmmN  (store=true)  /  movups xmmN, [rsp+disp]  (store=false)
void EmitMovupsRsp(Emitter& e, int n, uint32_t disp, bool store) {
    if (n >= 8) e.u8(0x44);
    e.u8(0x0F); e.u8(store ? 0x11 : 0x10);
    uint8_t modrm = (uint8_t)(((disp < 128) ? 0x44 : 0x84) | ((n & 7) << 3));
    e.u8(modrm); e.u8(0x24);
    if (disp < 128) e.u8((uint8_t)disp); else e.u32(disp);
}

struct SuspendGuard {
    std::vector<HANDLE> threads;
    SuspendGuard() {
        DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{ sizeof(te) };
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (h) { SuspendThread(h); threads.push_back(h); }
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    ~SuspendGuard() { for (HANDLE h : threads) { ResumeThread(h); CloseHandle(h); } }
};

} // namespace

// Emits the register save/handler-call/restore prologue+epilogue into e.
static void EmitFrame(Emitter& e, MidHookFn handler, void* userdata) {
    e.u8(0x9C);                                        // pushfq
    e.bytes({0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57});   // push rax..rdi
    for (int r = 0; r < 8; ++r) { e.u8(0x41); e.u8((uint8_t)(0x50 + r)); }  // push r8..r15
    const uint32_t frame = 256 + 8;                    // xmm[16] + rsp field
    e.bytes({0x48, 0x81, 0xEC}); e.u32(frame);         // sub rsp, frame
    for (int n = 0; n < 16; ++n) EmitMovupsRsp(e, n, n * 16, true);
    e.bytes({0x48, 0x8D, 0x84, 0x24}); e.u32(frame + 17 * 8);   // lea rax,[rsp+frame+136]  (= original rsp)
    e.bytes({0x48, 0x89, 0x84, 0x24}); e.u32(256);              // mov [rsp+256], rax
    e.bytes({0x48, 0x89, 0xE1});                       // mov rcx, rsp
    e.bytes({0x48, 0xBA}); e.u64((uint64_t)userdata);  // mov rdx, userdata
    e.bytes({0x48, 0xB8}); e.u64((uint64_t)handler);   // mov rax, handler
    e.bytes({0x48, 0x89, 0xE3});                       // mov rbx, rsp
    e.bytes({0x48, 0x83, 0xE4, 0xF0});                 // and rsp, -16
    e.bytes({0x48, 0x83, 0xEC, 0x20});                 // sub rsp, 0x20
    e.bytes({0xFF, 0xD0});                             // call rax
    e.bytes({0x48, 0x89, 0xDC});                       // mov rsp, rbx
    for (int n = 0; n < 16; ++n) EmitMovupsRsp(e, n, n * 16, false);
    e.bytes({0x48, 0x81, 0xC4}); e.u32(frame);         // add rsp, frame
    for (int r = 7; r >= 0; --r) { e.u8(0x41); e.u8((uint8_t)(0x58 + r)); }  // pop r15..r8
    e.bytes({0x5F, 0x5E, 0x5D});                       // pop rdi, rsi, rbp
    e.bytes({0x48, 0x8D, 0x64, 0x24, 0x08});           // lea rsp,[rsp+8]  (skip rsp slot)
    e.bytes({0x5B, 0x5A, 0x59, 0x58});                 // pop rbx, rdx, rcx, rax
    e.u8(0x9D);                                        // popfq
}

void* MidHookMakeStub(void* originalFn, MidHookFn handler, void* userdata) {
    Emitter e;
    EmitFrame(e, handler, userdata);
    e.bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});    // jmp [rip+0]
    e.u64((uint64_t)originalFn);
    void* mem = VirtualAlloc(nullptr, e.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;
    memcpy(mem, e.b.data(), e.size());
    FlushInstructionCache(GetCurrentProcess(), mem, e.size());
    return mem;
}

void* MidHookInstall(void* targetv, MidHookFn handler, void* userdata) {
    auto* target = (uint8_t*)targetv;
    // 1) determine how many whole instructions cover >= 5 bytes; collect relocation info.
    struct Ins { size_t off, len; bool ripRel; bool rel32; bool rel8; int dispOff; };
    std::vector<Ins> ins;
    size_t stolen = 0;
    while (stolen < 5) {
        hde64s hs;
        unsigned len = hde64_disasm(target + stolen, &hs);
        if (hs.flags & F_ERROR) { LOGE("[midhook] disasm error at %p+%zu", targetv, stolen); return nullptr; }
        Ins i{ stolen, len, false, false, false, 0 };
        if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
            i.ripRel = true;
            int immSize = (hs.flags & F_IMM8) ? 1 : (hs.flags & F_IMM16) ? 2 : (hs.flags & F_IMM32) ? 4 : (hs.flags & F_IMM64) ? 8 : 0;
            i.dispOff = (int)len - 4 - immSize;
        }
        if (hs.flags & F_RELATIVE) {
            if (hs.flags & F_IMM32) { i.rel32 = true; i.dispOff = (int)len - 4; }
            else { i.rel8 = true; }
        }
        if (i.rel8) { LOGE("[midhook] rel8 branch inside stolen bytes at %p+%zu; pick another hook point", targetv, stolen); return nullptr; }
        if (stolen + len > sizeof(Hook::original)) { LOGE("[midhook] stolen bytes too long"); return nullptr; }
        ins.push_back(i);
        stolen += len;
    }

    // 2) build stub
    Emitter e;
    EmitFrame(e, handler, userdata);
    size_t relocStart = e.size();
    e.raw(target, stolen);                             // relocated original instructions (fixed below)
    e.u8(0xE9); e.u32(0);                              // jmp back (patched below)

    size_t stubSize = e.size();
    uint8_t* stub = AllocNear(target, stubSize + 16);
    if (!stub) { LOGE("[midhook] could not allocate trampoline near %p", targetv); return nullptr; }

    // 3) fix relocations for the new address
    for (auto& i : ins) {
        if (!i.ripRel && !i.rel32) continue;
        int32_t old; memcpy(&old, &e.b[relocStart + i.off + i.dispOff], 4);
        uint8_t* oldEnd = target + i.off + i.len;
        uint8_t* dest = oldEnd + old;
        uint8_t* newEnd = stub + relocStart + i.off + i.len;
        int64_t nd = (int64_t)(dest - newEnd);
        if (nd < INT32_MIN || nd > INT32_MAX) { LOGE("[midhook] relocation out of range"); VirtualFree(stub, 0, MEM_RELEASE); return nullptr; }
        int32_t nd32 = (int32_t)nd; memcpy(&e.b[relocStart + i.off + i.dispOff], &nd32, 4);
    }
    {
        uint8_t* jmpEnd = stub + stubSize;
        int64_t back = (int64_t)((target + stolen) - jmpEnd);
        int32_t b32 = (int32_t)back; memcpy(&e.b[stubSize - 4], &b32, 4);
    }
    memcpy(stub, e.b.data(), stubSize);

    // 4) patch target: jmp stub + nops
    auto* h = new Hook();
    h->target = target; h->stolen = stolen; h->stub = stub; h->stubSize = stubSize;
    memcpy(h->original, target, stolen);
    {
        SuspendGuard guard;
        DWORD oldProt;
        if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) { LOGE("[midhook] VirtualProtect failed"); delete h; VirtualFree(stub, 0, MEM_RELEASE); return nullptr; }
        uint8_t patch[32]; memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xE9;
        int32_t rel = (int32_t)((int64_t)stub - (int64_t)(target + 5));
        memcpy(patch + 1, &rel, 4);
        memcpy(target, patch, stolen);
        VirtualProtect(target, stolen, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, stolen);
    }
    LOGI("[midhook] installed at %p (%zu bytes stolen, stub %p)", targetv, stolen, (void*)stub);
    return h;
}

void MidHookRemove(void* handle) {
    auto* h = (Hook*)handle;
    if (!h) return;
    {
        SuspendGuard guard;
        DWORD oldProt;
        if (VirtualProtect(h->target, h->stolen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(h->target, h->original, h->stolen);
            VirtualProtect(h->target, h->stolen, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), h->target, h->stolen);
        }
    }
    // Leak the stub intentionally: a thread might still be executing inside it.
    delete h;
}
}

// ---- self test ------------------------------------------------------------------
namespace ervr {
namespace {
#pragma optimize("", off)
__declspec(noinline) int64_t SelfTestTarget(int64_t a, int64_t b) {
    int64_t x = a * 3;      // rcx
    int64_t y = b + 7;      // rdx
    x ^= 0x55; y ^= 0x22;
    return x + y;
}
#pragma optimize("", on)
void SelfTestHandler(MidHookRegs* r, void* ud) {
    *(int*)ud += 1;
    r->rcx = 10;            // override a
    r->rdx = 3;             // override b
    r->xmm[5].f[0] = 42.f;  // clobber an xmm to make sure restore doesn't break anything
}
}
bool MidHookSelfTest() {
    int hits = 0;
    // Hook the function entry: entry bytes are ordinary and relocatable.
    void* h = MidHookInstall((void*)&SelfTestTarget, &SelfTestHandler, &hits);
    if (!h) return false;
    int64_t r = SelfTestTarget(1, 2);
    MidHookRemove(h);
    int64_t expect = ((10 * 3) ^ 0x55) + ((3 + 7) ^ 0x22);
    int64_t after = SelfTestTarget(1, 2);
    int64_t expectAfter = ((1 * 3) ^ 0x55) + ((2 + 7) ^ 0x22);
    bool ok = hits == 1 && r == expect && after == expectAfter;
    LOGI("[midhook] self-test %s (hits=%d, r=%lld expect %lld, after=%lld expect %lld)", ok ? "PASSED" : "FAILED", hits, (long long)r, (long long)expect, (long long)after, (long long)expectAfter);
    return ok;
}
}

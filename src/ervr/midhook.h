#pragma once
#include <cstdint>
#include <cstddef>

namespace ervr {

// Register file captured at a mid-function hook. Memory order matches the stub's
// stack frame (low -> high addresses). Handlers may modify any GPR/XMM (except
// rsp) and the change is applied when the original code resumes.
struct MidHookRegs {
    struct Xmm { float f[4]; } xmm[16];
    uint64_t rsp;          // rsp at the hooked instruction
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rsp_dummy, rbx, rdx, rcx, rax;
    uint64_t rflags;
};

using MidHookFn = void (*)(MidHookRegs* regs, void* userdata);

// Installs a mid-function hook at `target`. At least 5 bytes of whole instructions
// are relocated to a trampoline allocated near the target (RIP-relative operands
// and rel32 branches are re-encoded). The handler runs BEFORE the relocated
// instructions. Returns an opaque handle or nullptr.
void* MidHookInstall(void* target, MidHookFn handler, void* userdata);
void  MidHookRemove(void* handle);
// Builds (but does not install) a stub that saves all registers, calls handler,
// restores, and jumps to originalFn. Use it to replace vtable entries safely.
void* MidHookMakeStub(void* originalFn, MidHookFn handler, void* userdata);
// Hooks a local function, verifies register override + restore. Logs the result.
bool  MidHookSelfTest();
}

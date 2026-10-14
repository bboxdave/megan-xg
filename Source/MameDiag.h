// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once
//==============================================================================
// DIAG START 2026-07-27 — "why does the MAME engine go away on ~11% of boots?"
//
// The intermittent boot crash itself is fixed: the plugin no longer dies when the engine disappears. What is
// still unexplained is the disappearance. This module is the instrument for
// that question — it records, to a plain text file, everything the engine run
// can tell us about how it ended:
//
//   * every engine milestone (thread start, osd init, RUNNING, osd_exit,
//     execute() return, retract), with timestamps and thread ids
//   * MAME's own osd_printf_{error,warning,info} stream, so a clean
//     emu_fatalerror exit shows its message instead of just vanishing
//   * on an SEH fault: the code, the faulting instruction, the accessed
//     address + its VirtualQuery state, and a REAL x64 stack unwind
//     (RtlVirtualUnwind off the exception CONTEXT — no dbghelp, no PDBs
//     needed in-process; each frame prints as module+RVA, which the existing
//     dbghelp P/Invoke symbolizer resolves offline against the PDB).
//
// Everything here is allocation-free and CRT-light on the fault path: the log
// line is built in a stack buffer and written with raw Win32, because an
// access violation may well mean the heap is the thing that is broken.
//==============================================================================

#ifdef _WIN32

// This header may be included before anything else pulls in windows.h, so it
// must set the guard itself — without NOMINMAX the min/max macros break every
// later std::numeric_limits<...>::max() in the MAME headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mamediag
{

inline char  gLogPath[MAX_PATH] = {};
inline bool  gEnabled           = false;
inline DWORD gT0                = 0;   // GetTickCount at first init(), process-wide

// Bounds the MAME-sourced message stream (see mameMessageBudget below). A VST3
// host can hold several plugin instances in one process, so these are per
// process, which is also the scope of the log file.
inline long  gMameMsgBudget     = 0;

//==============================================================================
inline void writeRaw(const char* text, int len)
{
    if (!gEnabled || len <= 0)
        return;

    // Opened and closed per line on purpose: if the process dies mid-boot the
    // log must already be on disk, not in a handle's write cache.
    HANDLE h = CreateFileA(gLogPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(h, text, (DWORD) len, &written, nullptr);
    CloseHandle(h);
}

inline void vlogf(const char* fmt, va_list ap)
{
    char line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);

    int n = _snprintf(line, sizeof(line) - 3, "%02d:%02d:%02d.%03d +%6lums t%-5lu ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                      (unsigned long) (GetTickCount() - gT0), GetCurrentThreadId());
    if (n < 0) n = 0;

    int m = _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    if (m < 0) m = (int) (sizeof(line) - n - 3);   // truncated
    n += m;

    line[n++] = '\r';
    line[n++] = '\n';
    writeRaw(line, n);
}

inline void logf(const char* fmt, ...)
{
    if (!gEnabled) return;
    va_list ap;
    va_start(ap, fmt);
    vlogf(fmt, ap);
    va_end(ap);
}

//==============================================================================
// Enable logging. `path` is the full path of the log file; the header line
// records the module base of the plugin binary so an offline symbolizer can
// turn any raw address in this log back into an RVA.
//
// IDEMPOTENT, because a VST3 host can hold several plugin instances in one
// process and each one's constructor calls this. Only the first call stamps the
// header and takes the `gT0` epoch — otherwise every new instance would reset
// the "+Nms" column mid-log and it would stop meaning anything. Individual
// instances stay identifiable without a header of their own: the "processor
// ctor" line and every engine line carry a thread id, and the engine lines also
// carry `this`.
inline void init(const char* path)
{
    static LONG alreadyInitialised = 0;
    if (InterlockedExchange(&alreadyInitialised, 1) != 0)
        return;

    lstrcpynA(gLogPath, path, MAX_PATH);
    gEnabled = true;
    gT0      = GetTickCount();

    // Rotate at 1 MB (~hundreds of boots) so the log can never grow without
    // bound. One generation back is kept: a fault that made the user come
    // looking is usually still in .old.
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExA(gLogPath, GetFileExInfoStandard, &fad)
        && fad.nFileSizeHigh == 0 && fad.nFileSizeLow > (1u << 20))
    {
        char old[MAX_PATH];
        _snprintf(old, sizeof(old) - 1, "%s.old", gLogPath);
        old[MAX_PATH - 1] = 0;
        DeleteFileA(old);
        MoveFileA(gLogPath, old);
    }

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR) &gEnabled, &self);
    char selfPath[MAX_PATH] = {};
    if (self) GetModuleFileNameA(self, selfPath, MAX_PATH);

    logf("======== RUN START  pid=%lu  exe=%s", GetCurrentProcessId(), exe);
    logf("         module=%s base=0x%p", selfPath, (void*) self);
}

//==============================================================================
// Budget for MAME-sourced messages. Everything else logged here is bounded by
// construction (a fixed handful of lines per engine run), but MAME's message
// stream is not: one osd_printf_error on a per-frame code path would turn this
// logger into a file open per frame. Nothing like that has been observed, but
// the failure mode is bad enough — a hot path in a DAW session — to be worth
// three lines of insurance.
//
// Call takeMameMessageBudget() per message; false = stop logging them. The
// budget is granted per engine run by resetMameMessageBudget(), so a plugin
// that reboots its engine gets a fresh allowance rather than going permanently
// quiet after one noisy run.
inline void resetMameMessageBudget(long lines = 200)
{
    InterlockedExchange(&gMameMsgBudget, lines);
}

inline bool takeMameMessageBudget()
{
    const long left = InterlockedDecrement(&gMameMsgBudget);
    if (left == 0)
        logf("  mame[.....] message budget exhausted; further MAME output suppressed"
             " for this engine run");
    return left >= 0;
}

//==============================================================================
// "mu100.exe+0x1234abc" for an address, so the offline PDB symbolizer has an
// RVA to work with. Falls back to the bare address when it is not in a module
// (a stale stack address, a JIT block, freed memory).
inline void describeAddr(const void* addr, char* out, size_t outSize)
{
    HMODULE mod = nullptr;
    if (addr != nullptr
        && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR) addr, &mod)
        && mod != nullptr)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        _snprintf(out, outSize, "%-28s +0x%08llx   (0x%016llx)", base,
                  (unsigned long long) ((uintptr_t) addr - (uintptr_t) mod),
                  (unsigned long long) (uintptr_t) addr);
    }
    else
    {
        _snprintf(out, outSize, "%-28s              (0x%016llx)", "<no module>",
                  (unsigned long long) (uintptr_t) addr);
    }
    out[outSize - 1] = 0;
}

//==============================================================================
// What kind of memory is at `addr`? RESERVE/FREE on a pointer that used to be
// valid is the signature of recycled thread stack — that is exactly what
// identified the original dangling running_machine, so it is worth having on
// every fault.
inline void logMemoryAt(const char* what, const void* addr)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (addr == nullptr || VirtualQuery(addr, &mbi, sizeof(mbi)) == 0)
    {
        logf("    %s 0x%016llx : VirtualQuery failed (unmapped)", what,
             (unsigned long long) (uintptr_t) addr);
        return;
    }

    const char* state = mbi.State == MEM_COMMIT  ? "COMMIT"
                      : mbi.State == MEM_RESERVE ? "RESERVE"
                      : mbi.State == MEM_FREE    ? "FREE"
                                                 : "?";
    const char* type  = mbi.Type == MEM_IMAGE   ? "IMAGE"
                      : mbi.Type == MEM_MAPPED  ? "MAPPED"
                      : mbi.Type == MEM_PRIVATE ? "PRIVATE"
                                                : "-";
    logf("    %s 0x%016llx : %s %s protect=0x%lx region=0x%llx+0x%llx", what,
         (unsigned long long) (uintptr_t) addr, state, type,
         (unsigned long) mbi.Protect,
         (unsigned long long) (uintptr_t) mbi.AllocationBase,
         (unsigned long long) mbi.RegionSize);
}

//==============================================================================
// Log the exception + a real stack walk, called FROM the SEH filter, i.e. on
// the faulting thread with its frames still intact.
inline void logException(EXCEPTION_POINTERS* ep)
{
    if (!gEnabled || ep == nullptr || ep->ExceptionRecord == nullptr)
        return;

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;

    char site[128];
    describeAddr(er->ExceptionAddress, site, sizeof(site));
    logf("!!!! MAME ENGINE FAULT  code=0x%08lx  flags=0x%lx  at %s",
         (unsigned long) er->ExceptionCode, (unsigned long) er->ExceptionFlags, site);

    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
         || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
        && er->NumberParameters >= 2)
    {
        const ULONG_PTR op = er->ExceptionInformation[0];
        logf("     access=%s address=0x%016llx",
             op == 0 ? "READ" : op == 1 ? "WRITE" : op == 8 ? "EXECUTE" : "?",
             (unsigned long long) er->ExceptionInformation[1]);
        logMemoryAt("target ", (const void*) er->ExceptionInformation[1]);
    }
    logMemoryAt("faultpc", er->ExceptionAddress);

#ifdef _M_X64
    if (ep->ContextRecord != nullptr)
    {
        const CONTEXT& c = *ep->ContextRecord;
        logf("     rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c.Rax, c.Rbx, c.Rcx, c.Rdx);
        logf("     rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
        logf("     r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c.R8, c.R9, c.R10, c.R11);
        logf("     r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c.R12, c.R13, c.R14, c.R15);

        // Real unwind, not a stack scan: RtlLookupFunctionEntry reads the PE's
        // .pdata, so this needs no symbols at all and gives exact frames.
        CONTEXT ctx = *ep->ContextRecord;
        logf("     --- stack (module+RVA; symbolize offline against the PDBs) ---");

        for (int frame = 0; frame < 48 && ctx.Rip != 0; ++frame)
        {
            char desc[128];
            describeAddr((const void*) ctx.Rip, desc, sizeof(desc));
            logf("     #%-2d %s", frame, desc);

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (rf == nullptr)
            {
                // Leaf function (or unknown code): the return address is at RSP.
                // Bail if that would read something unmapped.
                MEMORY_BASIC_INFORMATION mbi = {};
                if (VirtualQuery((void*) ctx.Rsp, &mbi, sizeof(mbi)) == 0
                    || mbi.State != MEM_COMMIT)
                    break;
                ctx.Rip  = *(DWORD64*) ctx.Rsp;
                ctx.Rsp += 8;
            }
            else
            {
                PVOID   handlerData      = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx,
                                 &handlerData, &establisherFrame, nullptr);
            }
        }
    }
#endif // _M_X64
}

} // namespace mamediag

#else  // !_WIN32

namespace mamediag
{
inline void init(const char*) {}
inline void logf(const char*, ...) {}
}

#endif // _WIN32

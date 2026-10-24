/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#ifdef _WIN32
static unsigned long long start_tick; /* GetTickCount64() at start, for the fault report */
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
#include "runtime_scene_debug.inc"
#include "runtime_menu_repeat.inc"
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
/* SSE4a (EXTRQ, INSERTQ) exists on the PS4's Jaguar and on AMD CPUs; Intel CPUs raise #UD on it.
 * Register forms only (memory operands do not exist): 66|F2 [REX] 0F 78|79 ModRM(mod 11) [ib ib]. */
typedef struct { int length, insert, immediate, dst, src; unsigned len, idx; } Sse4a;
static int decode_sse4a(const unsigned char *p, Sse4a *o) {
    if (p[0] != 0x66 && p[0] != 0xf2) return 0;
    const unsigned rex = (p[1] & 0xf0) == 0x40 ? p[1] : 0;
    const unsigned char *op = p + (rex ? 2 : 1);
    if (op[0] != 0x0f || (op[1] != 0x78 && op[1] != 0x79) || (op[2] & 0xc0) != 0xc0) return 0;
    o->insert = p[0] == 0xf2;
    o->immediate = op[1] == 0x78;
    o->dst = ((op[2] >> 3) & 7) | (rex & 4 ? 8 : 0);
    o->src = (op[2] & 7) | (rex & 1 ? 8 : 0);
    if (o->immediate && !o->insert) { /* extrq xmm, len, idx: 66 0F 78 /0 */
        if ((op[2] >> 3) & 7) return 0;
        o->dst = o->src;
    }
    o->length = (int)(op - p) + 3 + (o->immediate ? 2 : 0);
    o->len = o->immediate ? op[3] & 63u : 0;
    o->idx = o->immediate ? op[4] & 63u : 0;
    return 1;
}
static int host_has_sse4a(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000001u) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
    return (c >> 6) & 1;
}
static int host_has_sse41(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0));
    return (c >> 19) & 1;
}
/* On a host without SSE4a, `extrq xmm, len, idx` in guest code is rewritten before entry:
 * extrq x, 8, 0  -> pmovzxbq x, x (same low qword; SSE4.1), padded with nop, in place;
 * any other      -> jmp to a stub: psllq x, 64-idx-len; psrlq x, 64-len; jmp back.
 * EXTRQ leaves the upper qword undefined, so neither replacement has to preserve it. The stubs
 * go in the unmapped pages between an executable segment and the next one (the padding of the
 * segment's last page stays free for patch code caves). Sites that do not fit, and the other
 * SSE4a forms, are emulated in the fault handler (Windows). */
static uint64_t rewrite_sse4a(Segment *segments, uint64_t count, uint64_t image_size, uint64_t *left) {
    const int sse41 = host_has_sse41();
    uint64_t rewritten = 0;
    *left = 0;
    for (uint64_t s = 0; s < count; ++s) {
        if (!(segments[s].flags & 1) || segments[s].size < 8) continue;
        const uint64_t start = segments[s].address, stub_begin = round_page(start + segments[s].size);
        uint64_t stub_end = round_page(image_size), used = 0;
        for (uint64_t i = 0; i < count; ++i)
            if (segments[i].address >= stub_begin && segments[i].address < stub_end) stub_end = segments[i].address;
        unsigned char *at = image + start, *end = image + start + segments[s].size - 7;
        for (; at <= end; ++at) {
            Sse4a o;
            if (!decode_sse4a(at, &o) || o.insert || !o.immediate) continue;
            const unsigned len = o.len ? o.len : 64;
            if (len + o.idx > 64) { ++*left; continue; }
            const unsigned char rex = o.dst >= 8 ? 0x41 : 0, reg = o.dst & 7;
            unsigned char code[16];
            int n = 0;
            if (len == 8 && !o.idx && sse41) {
                code[n++] = 0x66;
                if (o.dst >= 8) code[n++] = 0x45;
                code[n++] = 0x0f; code[n++] = 0x38; code[n++] = 0x32; code[n++] = (unsigned char)(0xc0 | reg << 3 | reg);
            } else if (len < 64) {
                unsigned char stub[24];
                int k = 0;
                const unsigned shifts[2][2] = {{6, 64 - o.idx - len}, {2, 64 - len}}; /* /6 psllq, /2 psrlq */
                for (int j = 0; j < 2; ++j) {
                    if (!shifts[j][1]) continue;
                    stub[k++] = 0x66;
                    if (rex) stub[k++] = rex;
                    stub[k++] = 0x0f; stub[k++] = 0x73; stub[k++] = (unsigned char)(0xc0 | shifts[j][0] << 3 | reg);
                    stub[k++] = (unsigned char)shifts[j][1];
                }
                const uint64_t here = stub_begin + used, site = (uint64_t)(at - image);
                if (here + k + 5 > stub_end) { ++*left; at += o.length - 1; continue; }
                int32_t back = (int32_t)((site + o.length) - (here + k + 5)), to = (int32_t)(here - (site + 5));
                stub[k++] = 0xe9; memcpy(stub + k, &back, 4); k += 4;
                memcpy(image + here, stub, (size_t)k);
                used += (uint64_t)k;
                code[n++] = 0xe9; memcpy(code + n, &to, 4); n += 4;
            } /* else extrq x, 64, 0 leaves the low qword unchanged: all nop */
            memset(code + n, 0x90, (size_t)(o.length - n));
            memcpy(at, code, (size_t)o.length);
            ++rewritten;
            at += o.length - 1;
        }
        if (used) segments[s].size = stub_begin + used - start; /* mapped executable with the segment */
    }
    return rewritten;
}
static int stub_unresolved; /* --stub-unresolved: development aid, log each missing import once and return 0 */
static ABI uint64_t unresolved_soft(uint32_t id, uintptr_t argument) {
    static unsigned char seen[4096];
    if (id < sizeof seen && !__sync_lock_test_and_set(&seen[id], 1))
        printf("STUBBED: %s (%s) arg0=0x%" PRIxPTR " caller=+0x%" PRIxPTR "\n", names[id], runtime_import_name(names[id]), argument,
               (uintptr_t)__builtin_return_address(0) - (uintptr_t)image);
    /* A stubbed import that a guest thread polls in a loop (the USB event loop, for one) returns at once and would
     * keep a whole CPU core busy. Past 2000 calls in 100 ms a call sleeps 1 ms, as the blocking original would. */
    if (id < 4096) {
        static unsigned calls[4096]; static uint64_t window[4096];
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        const uint64_t ms=(uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u;
        if (ms-window[id]>=100) { window[id]=ms; calls[id]=0; }
#ifdef _WIN32
        if (++calls[id]>2000) compat_sleep_ns(1000000);
#else
        if (++calls[id]>2000) usleep(1000);
#endif
    }
    return 0;
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
#ifdef _WIN32
/* Diagnostics for the log: third-party modules in the process, a small crash dump, and exit markers. */
#include <psapi.h>
#include <dbghelp.h>
#include <signal.h>

/* DLLs loaded from outside Windows and the game's own folders (overlays, capture tools, injected code). */
static void list_third_party_modules(FILE *out, const char *prefix) {
    HMODULE modules[512];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    char system[MAX_PATH] = "", windows[MAX_PATH] = "", own[MAX_PATH] = "";
    GetSystemDirectoryA(system, sizeof system);
    GetWindowsDirectoryA(windows, sizeof windows);
    if (GetModuleFileNameA(NULL, own, sizeof own)) {
        char *slash = strrchr(own, 92);
        if (slash) *slash = 0;
    }
    int shown = 0;
    for (DWORD i = 0; i < needed / sizeof modules[0] && shown < 24; ++i) {
        char path[MAX_PATH];
        if (!K32GetModuleFileNameExA(GetCurrentProcess(), modules[i], path, sizeof path)) continue;
        if (!_strnicmp(path, system, strlen(system)) || !_strnicmp(path, windows, strlen(windows))) continue;
        if (own[0] && !_strnicmp(path, own, strlen(own))) continue;
        const char *name = strrchr(path, 92);
        name = name ? name + 1 : path;
        if (!shown) fprintf(out, "%s", prefix);
        fprintf(out, " %s", name);
        ++shown;
    }
    if (shown) fputc(10, out);
}

/* A small dump (stacks, threads, modules; no game memory) next to the log, to attach to a report. It is written by
 * a helper thread that is waited for at most ten seconds, so a dump that hangs cannot keep the game open. */
static EXCEPTION_POINTERS *dump_exception;
static DWORD dump_thread_id;
static DWORD WINAPI crash_dump_worker(LPVOID unused);
static void write_crash_dump(EXCEPTION_POINTERS *info) {
    dump_exception = info;
    dump_thread_id = GetCurrentThreadId();
    HANDLE worker = CreateThread(NULL, 0, crash_dump_worker, NULL, 0, NULL);
    if (worker) {
        WaitForSingleObject(worker, 10000);
        CloseHandle(worker);
    }
}
static void write_crash_dump_now(EXCEPTION_POINTERS *info, DWORD faulting_thread) {
    const char *dir = getenv("BB_CRASH_DIR");
    if (!dir || !*dir) return;
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) return;
    typedef BOOL(WINAPI * WriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                       PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    WriteDumpFn write = (WriteDumpFn)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s%ccrash_%llu.dmp", dir, 92, (unsigned long long)GetTickCount64());
    HANDLE file = write ? CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception = {faulting_thread, info, FALSE};
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo),
                              &exception, NULL, NULL);
        CloseHandle(file);
        fprintf(stderr, "  crash dump: %s%s", path, ok ? "" : " (incomplete)");
        fputc(10, stderr);
    }
}

static DWORD WINAPI crash_dump_worker(LPVOID unused) {
    (void)unused;
    write_crash_dump_now(dump_exception, dump_thread_id);
    return 0;
}

static void note_exit(void) {
    runtime_ffb_release("the game quit");
    puts("Runtime: the process ended normally");
    fflush(NULL);
}
/* The console window closed (or Ctrl+C, log off, shut down): Windows ends the process right after this. */
static BOOL WINAPI console_closing(DWORD event) {
    (void)event;
    runtime_ffb_release("the console closed");
    return FALSE;
}
static void on_abort(int signal_number) {
    (void)signal_number;
    runtime_ffb_release("abort");
    fputs("Fatal: abort() was called (a failed check in the runtime or a library)", stderr);
    fputc(10, stderr);
    fflush(NULL);
    _exit(22);
}
#endif
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  uptime %llu s\n",(unsigned long long)(GetTickCount64()-start_tick)/1000ULL);
    {
        MEMORYSTATUSEX memory;
        memset(&memory,0,sizeof memory);
        memory.dwLength=sizeof memory;
        PROCESS_MEMORY_COUNTERS counters;
        memset(&counters,0,sizeof counters);
        if (GlobalMemoryStatusEx(&memory) && K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof counters))
            fprintf(stderr,"  memory: process %llu MB (peak %llu MB), system free %llu of %llu MB, commit free %llu MB\n",
                    (unsigned long long)(counters.WorkingSetSize>>20),(unsigned long long)(counters.PeakWorkingSetSize>>20),
                    (unsigned long long)(memory.ullAvailPhys>>20),(unsigned long long)(memory.ullTotalPhys>>20),
                    (unsigned long long)(memory.ullAvailPageFile>>20));
    }
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    fprintf(stderr,"  r8=%016llx r9=%016llx r10=%016llx r11=%016llx\n  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15);
    {
        unsigned char code[16];
        SIZE_T got=0;
        if (ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)c->Rip,code,sizeof(code),&got) && got) {
            fprintf(stderr,"  code at rip:");
            for (SIZE_T i=0;i<got;++i) fprintf(stderr," %02x",code[i]);
            fputc(10,stderr);
        }
    }
    list_third_party_modules(stderr,"  third-party modules:");
    if (gpu_enabled) { bbgpu_dump_guest_writes(info); bbgpu_print_recent_passes(); }
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    /* Code that does not keep a frame pointer: return addresses found on the stack. */
    {
        uintptr_t stack[96];
        SIZE_T got=0;
        if (c->Rsp && ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)c->Rsp,stack,sizeof(stack),&got)) {
            int shown=0;
            for (SIZE_T i=0; i<got/sizeof(stack[0]) && shown<12; ++i) {
                const uintptr_t off=stack[i]-(uintptr_t)image;
                if (image && stack[i]>(uintptr_t)image && (off<0x1560000 || (off>=0x2820000 && off<0x3400000))) {
                    if (!shown) fprintf(stderr,"  stack:");
                    fprintf(stderr," +0x%" PRIxPTR,off);
                    ++shown;
                }
            }
            if (shown) fputc(10,stderr);
        }
    }
    /* What the registers point to: eight words at each readable pointer, and the words around +0x740 of RSI (the
     * object a crashing function was working on). */
    {
        const struct { const char *name; DWORD64 value; } regs[] = {
            {"rax",c->Rax},{"rbx",c->Rbx},{"rcx",c->Rcx},{"rdx",c->Rdx},{"rsi",c->Rsi},{"rdi",c->Rdi},{"r8",c->R8},
            {"r9",c->R9},{"r10",c->R10},{"r11",c->R11},{"r12",c->R12},{"r13",c->R13},{"r14",c->R14},{"r15",c->R15}};
        for (size_t i=0;i<sizeof regs/sizeof regs[0];++i) {
            uintptr_t address=(uintptr_t)regs[i].value;
            unsigned long long words[8];
            SIZE_T got=0;
            if (address<0x10000 || address>0x00007fffffffffffULL) continue;
            if (!ReadProcessMemory(GetCurrentProcess(),(void *)address,words,sizeof words,&got) || got!=sizeof words) continue;
            fprintf(stderr,"  %s ->",regs[i].name);
            for (int k=0;k<8;++k) fprintf(stderr," %llx",words[k]);
            fputc(10,stderr);
        }
        unsigned long long object[14];
        SIZE_T got=0;
        if (c->Rsi>0x10000 && c->Rsi<0x00007fffffffffffULL &&
            ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rsi+0x738),object,sizeof object,&got) && got==sizeof object) {
            fprintf(stderr,"  rsi+0x738 ->");
            for (int k=0;k<14;++k) fprintf(stderr," %llx",object[k]);
            fputc(10,stderr);
        }
    }
    /* The 128 bytes below the stack pointer (a leaf function's locals live there). */
    {
        uintptr_t below[16];
        SIZE_T got=0;
        if (c->Rsp>0x1000 && ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rsp-sizeof(below)),below,sizeof(below),&got) && got==sizeof(below)) {
            fprintf(stderr,"  below rsp:");
            for (int i=0;i<16;++i) fprintf(stderr," %llx",(unsigned long long)below[i]);
            fputc(10,stderr);
        }
    }
    write_crash_dump(info);
    runtime_ffb_release("a crash");
    fputs("Runtime: the process was ended after the fault above",stderr);
    fputc(10,stderr);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && bbgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)bb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image) {
        /* SSE4a guest instructions left after rewrite_sse4a run here, one fault each. */
        CONTEXT *c=info->ContextRecord;
        uintptr_t rip=(uintptr_t)c->Rip;
        Sse4a o;
        if (rip-(uintptr_t)image<runtime_image_size && decode_sse4a((const unsigned char *)rip,&o)) {
            M128A *x=c->FltSave.XmmRegisters;
            unsigned len=o.len, idx=o.idx;
            if (!o.immediate) {
                uint64_t control=o.insert ? (uint64_t)x[o.src].High : x[o.src].Low;
                len=control&63; idx=(control>>8)&63;
            }
            if (!len) len=64;
            if (len+idx<=64) {
                uint64_t mask=len==64 ? ~UINT64_C(0) : (UINT64_C(1)<<len)-1;
                if (o.insert) x[o.dst].Low=(x[o.dst].Low&~(mask<<idx))|((x[o.src].Low&mask)<<idx);
                else x[o.dst].Low=(x[o.dst].Low>>idx)&mask;
                c->Rip+=o.length;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* BB_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov <reg>, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    /* gs:[0] load into any 64-bit register: 65, REX.W (48; 4c for r8-r15), 8b, ModRM with a SIB
       byte (mod 00, rm 100, any reg field), SIB 25 (absolute), disp32 0 (rewritten below). */
    enum { LENGTH = 9 };
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<LENGTH) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-LENGTH;
        for (; at<=end; ++at)
            if (at[0]==0x65 && (at[1]==0x48 || at[1]==0x4c) && at[2]==0x8b && (at[3]&0xc7)==0x04 &&
                at[4]==0x25 && !at[5] && !at[6] && !at[7] && !at[8]) {
                memcpy(at+5,&displacement,4); ++patched; at+=LENGTH-1;
            }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* A patch may also land in the padding up to the end of a segment's last page (code caves such as
 * the anti-freeze guard of the Second Son patch set): the page is mapped with the segment's
 * protection, only the segment's own size stops short of it. */
static int patchable(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    if (mapped(segments, count, address, bytes)) return 1;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t end = segments[i].address + round_page(segments[i].size);
        if (address >= segments[i].address && bytes <= end - segments[i].address && address - segments[i].address <= end - segments[i].address - bytes) return 1;
    }
    return 0;
}
/* BB_SHARED_LOG and BB_SHARED_PAD (default 3; gpu/shim/bbport_shared_log.cpp): the six call/jmp sites of the game's SystemShared
 * allocator (eboot 0x753a80) and the GDS-count call of the VideoShared one (0x753b40, site 0x7584e1) go through near
 * thunks (mov ecx, site; movabs rax, wrapper; jmp rax) to the wrapper in gpu/shim/bbport_shared_log.cpp. rcx is free at
 * all seven: both allocators take three arguments. A site is patched only when it is exactly e8/e9 to its allocator. */
static void patch_shared_alloc(unsigned char *image, uint64_t size) {
    static const uint64_t alloc = 0x753a80, video_alloc = 0x753b40;
    static const uint64_t sites[7] = {0x744379, 0x744c8c, 0x75848e, 0x7584c4, 0x100acff, 0x100ad1c, 0x7584e1};
    const char *pad = getenv("BB_SHARED_PAD");
    const int log = getenv("BB_SHARED_LOG") != NULL, padding = pad ? (int)strtoul(pad, NULL, 0) : 3;
    if ((!log && !padding) || size < 0x100ad1c + 5) return;
    uintptr_t wrapper = bbgpu_shared_alloc_hook((uintptr_t)image + alloc, (uintptr_t)image);
    if (!wrapper) return;
    unsigned char *thunks = allocate(page_size);
    int patched = 0;
    for (int c = 0; c < 7; ++c) {
        const int payload = sites[c] == 0x75848e || sites[c] == 0x7584c4, count = sites[c] == 0x7584e1;
        /* BB_SHARED_PAD alone patches only the sites it pads (bit 1 payloads, bit 2 counts) */
        if (!(log && !count) && !((padding & 1) && payload) && !((padding & 2) && count)) continue;
        const uint64_t target = count ? video_alloc : alloc;
        unsigned char *site = image + sites[c];
        int32_t rel;
        memcpy(&rel, site + 1, 4);
        if ((site[0] != 0xe8 && site[0] != 0xe9) || (int64_t)sites[c] + 5 + (int64_t)rel != (int64_t)target) {
            printf("BB_SHARED_LOG: site %#" PRIx64 " does not call the allocator; not patched\n", sites[c]);
            continue;
        }
        unsigned char *t = thunks + 32 * c;
        uint32_t id = (uint32_t)sites[c];
        t[0] = 0xb9; memcpy(t + 1, &id, 4);                    /* mov ecx, site */
        t[5] = 0x48; t[6] = 0xb8; memcpy(t + 7, &wrapper, 8);  /* movabs rax, wrapper */
        t[15] = 0xff; t[16] = 0xe0;                            /* jmp rax */
        int64_t to = (int64_t)((uintptr_t)t - ((uintptr_t)site + 5));
        if (to != (int32_t)to) {
            printf("BB_SHARED_LOG: thunk out of range of %#" PRIx64 "; not patched\n", sites[c]);
            continue;
        }
        rel = (int32_t)to;
        memcpy(site + 1, &rel, 4); /* keeps e8 (call) or e9 (tail jump) */
        ++patched;
    }
    protect(thunks, page_size, 5);
    printf("BB_SHARED_LOG/PAD: %d SystemShared allocator sites patched (log %d, pad %d)\n", patched, log, padding);
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !patchable(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Upscaling (BB_AA = dlss or fsr3 with BB_AA_SCALE = quality, balanced, performance or ultra_performance): the game
 * renders its scene at the output size over the scale and FSR or DLSS brings it back to the output size
 * (gpu/shadps4/video_core/renderer_vulkan/vk_drive_aa.cpp). DriveClub 1.28 keeps its output size in the code (the
 * immediates of mov r8d / mov r9d at +0x754c19 / +0x754c1f, which the resolution patches change) and its render size
 * in data (+0x12d0544, +0x12d0554, as the "Internal resolution" patches): the render size is set here, after the
 * patches, so it follows any output size, widescreen ones too. Both multiples of 8. */
static void apply_upscale_render_size(unsigned char *image, uint64_t size) {
    static const struct { const char *name; float scale; } scales[] = {
        {"quality", 1.5f}, {"balanced", 1.7f}, {"performance", 2.0f}, {"ultra_performance", 3.0f}};
    const char *aa=getenv("BB_AA"), *want=getenv("BB_AA_SCALE");
    if (!aa || (strcmp(aa,"dlss") && strcmp(aa,"fsr3")) || !want) return;
    float scale=0.0f;
    for (size_t i=0;i<sizeof scales/sizeof *scales;++i) if (!strcmp(want,scales[i].name)) scale=scales[i].scale;
    if (scale<=0.0f || size<0x12d0558) return;
    if (image[0x754c19]!=0x41 || image[0x754c1a]!=0xb8 || image[0x754c1f]!=0x41 || image[0x754c20]!=0xb9) {
        printf("Upscaling: the game's output size is not where eboot 1.28 has it; rendering at the output size\n");
        return;
    }
    uint32_t out_w, out_h, render_w, render_h;
    memcpy(&out_w,image+0x754c1b,4); memcpy(&out_h,image+0x754c21,4);
    if (out_w<640 || out_w>16384 || out_h<360 || out_h>8640) return;
    render_w=(uint32_t)((float)out_w/scale+4.0f)/8*8;
    render_h=(uint32_t)((float)out_h/scale+4.0f)/8*8;
    memcpy(image+0x12d0544,&render_w,4); memcpy(image+0x12d0554,&render_h,4);
    printf("Upscaling: the game renders at %ux%u for its %ux%u output (%s)\n",render_w,render_h,out_w,out_h,want);
}
/* Pop-in: the game leaves out every object smaller than a share of the screen given per kind of object in
 * viewportquality.xml (static_objects 0.02 in a race), so barriers and trees appeared from nothing as the car came
 * closer. eboot 1.28 +0x60ff90 copies a view's quality tag into its cull parameters: +0x60ffcb the shadow view's
 * shadowCullThreshold, +0x60ffd8 cullThreshold and cullThresholdZPass (vmovss xmm0,[r14+x]; vmovss [rbx+y],xmm0).
 * Both go through a thunk that multiplies each value, then back to +0x60ffee. BB_CULL_SCALE (default 0.25: objects
 * stay until about 7 pixels tall at 1440p; as fast as the game, where 0 cost ~6 FPS in the 1% lows) and
 * BB_CULL_SCALE_SHADOW (default 0.25) set the factors, 1 keeps the game's. The depth pre-pass (cullThresholdZPass,
 * 0.35: only large occluders) keeps the game's unless BB_CULL_SCALE_ZPASS says otherwise: scaling it draws every
 * small object again in that pass. */
static float cull_factor(const char *name, float fallback) {
    const char *env=getenv(name);
    const float value=env && *env ? (float)atof(env) : fallback;
    return value>=0.0f && value<=4.0f ? value : fallback;
}
static void apply_cull_scale(unsigned char *image, uint64_t size) {
    if (size<0x60fff0) return;
    const float scales[3]={cull_factor("BB_CULL_SCALE",0.25f),cull_factor("BB_CULL_SCALE_ZPASS",1.0f),
                           cull_factor("BB_CULL_SCALE_SHADOW",0.25f)};
    if (scales[0]==1.0f && scales[1]==1.0f && scales[2]==1.0f) return;
    static const unsigned char expect[]={0xc4,0xc1,0x7a,0x10,0x46,0x38,0xc5,0xfa,0x11,0x43,0x0c,0xeb,0x16,
                                         0xc4,0xc1,0x7a,0x10,0x46,0x30,0xc5,0xfa,0x11,0x43,0x0c,
                                         0xc4,0xc1,0x7a,0x10,0x46,0x34,0xc5,0xfa,0x11,0x43,0x10};
    unsigned char *site=image+0x60ffcb;
    if (memcmp(site,expect,sizeof expect)) { printf("Pop-in: the cull thresholds are not where eboot 1.28 has them; not changed\n"); return; }
    unsigned char *t=allocate(page_size);
    memcpy(t+0x80,scales,sizeof scales);
    /* (from, to, scale) triples: tag offset -> cull parameter offset, times scales[i]; the shadow thunk at 0, the
     * other at 0x30 */
    static const unsigned char copies[2][6]={{0x38,0x0c,2,0,0,0},{0x30,0x0c,0,0x34,0x10,1}};
    for (int k=0;k<2;++k) {
        unsigned char *c=t+0x30*k;
        int n=0;
        for (int j=0;j<6 && copies[k][j];j+=3) {
            const unsigned char load[]={0xc4,0xc1,0x7a,0x10,0x46,copies[k][j]}, store[]={0xc5,0xfa,0x11,0x43,copies[k][j+1]};
            memcpy(c+n,load,6); n+=6;
            c[n++]=0xc5; c[n++]=0xfa; c[n++]=0x59; c[n++]=0x05;                  /* vmulss xmm0,xmm0,[rip+scale] */
            const int32_t at=(int32_t)((t+0x80+4*copies[k][j+2])-(c+n+4)); memcpy(c+n,&at,4); n+=4;
            memcpy(c+n,store,5); n+=5;
        }
        const int64_t back=(int64_t)((uintptr_t)(image+0x60ffee)-(uintptr_t)(c+n+5)), to=(int64_t)((uintptr_t)c-(uintptr_t)(site+(k?13:0)+5));
        if (back!=(int32_t)back || to!=(int32_t)to) { printf("Pop-in: thunk out of range; cull thresholds not changed\n"); return; }
        c[n++]=0xe9; const int32_t b32=(int32_t)back; memcpy(c+n,&b32,4);
    }
    for (int k=0;k<2;++k) {
        unsigned char *from=site+(k?13:0);
        const int32_t to=(int32_t)((t+0x30*k)-(from+5));
        memset(from,0x90,k?22:13);
        from[0]=0xe9; memcpy(from+1,&to,4);
    }
    protect(t,page_size,5);
    printf("Pop-in: cull thresholds times %.3f (depth pre-pass %.3f, shadows %.3f)\n",scales[0],scales[1],scales[2]);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets BB_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    runtime_ffb_release("restart");
    TerminateProcess(GetCurrentProcess(),0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

#ifdef _WIN32
/* Freeze report: when no frame has been presented for 15 seconds after the game is up, the instruction pointer and
 * stack of every thread go into the log (once per freeze, at most three times). Nothing is stopped or changed. */
#include <tlhelp32.h>
static DWORD WINAPI hang_watchdog(LPVOID unused) {
    (void)unused;
    unsigned long long last_count = 0;
    ULONGLONG last_change = GetTickCount64();
    int reports = 0, reported_this_freeze = 0;
    for (;;) {
        Sleep(1000);
        const unsigned long long count = bbgpu_flip_count();
        const ULONGLONG now = GetTickCount64();
        if (count != last_count) {
            last_count = count;
            last_change = now;
            reported_this_freeze = 0;
            continue;
        }
        if (!count || reported_this_freeze || reports >= 3 || now - last_change < 15000) continue;
        reported_this_freeze = 1;
        ++reports;
        bbgpu_print_recent_passes();
        runtime_diag_print(stdout); /* before any thread is suspended: it takes a lock */
        runtime_rwlock_dump(stdout);
        printf("Hang: no frame presented for %llu s (uptime %llu s); threads:", (unsigned long long)((now - last_change) / 1000),
               (unsigned long long)((now - start_tick) / 1000));
        fputc(10, stdout);
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te;
        memset(&te, 0, sizeof te);
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!thread) continue;
            if (SuspendThread(thread) != (DWORD)-1) {
                CONTEXT context;
                memset(&context, 0, sizeof context);
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(thread, &context)) {
                    char where[MAX_PATH + 64];
                    describe_address(where, sizeof where, (uintptr_t)context.Rip);
                    printf("  thread %lu at %s; stack:", te.th32ThreadID, where);
                    uintptr_t words[1024];
                    SIZE_T got = 0;
                    int shown = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)context.Rsp, words, sizeof words, &got)) {
                        for (SIZE_T i = 0; i < got / sizeof words[0] && shown < 24; ++i) {
                            const uintptr_t off = words[i] - (uintptr_t)image;
                            if (image && words[i] > (uintptr_t)image && (off < 0x1560000 || (off >= 0x2820000 && off < 0x3400000))) {
                                printf(" +0x%llx", (unsigned long long)off);
                                ++shown;
                            }
                        }
                        /* Return addresses inside the runtime itself (renderer and runtime threads): look them up with the
                         * release's executable. */
                        static uintptr_t host_base = 0, host_size = 0;
                        if (!host_base) {
                            host_base = (uintptr_t)GetModuleHandleA(NULL);
                            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)host_base;
                            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(host_base + dos->e_lfanew);
                            host_size = nt->OptionalHeader.SizeOfImage;
                        }
                        int host_shown = 0;
                        for (SIZE_T i = 0; i < got / sizeof words[0] && host_shown < 14; ++i) {
                            if (words[i] >= host_base && words[i] < host_base + host_size) {
                                printf("%s host+0x%llx", host_shown ? "" : "  |", (unsigned long long)(words[i] - host_base));
                                ++host_shown;
                            }
                        }
                    }
                    fputc(10, stdout);
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snap);
    }
    return 0;
}
#endif
#ifdef _WIN32
/* One block at the top of every log: processor, memory, Windows and the launcher's choices, so that a report
 * can be understood without asking the player. */
static void print_system_info(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char brand[64] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i), "c"(0));
        memcpy(brand + i * 16 + 0, &a, 4);
        memcpy(brand + i * 16 + 4, &b, 4);
        memcpy(brand + i * 16 + 8, &c, 4);
        memcpy(brand + i * 16 + 12, &d, 4);
    }
    const char *name = brand;
    while (*name == ' ') ++name;
    for (size_t n = strlen(brand); n && brand[n - 1] == ' '; --n) brand[n - 1] = 0;
    unsigned a1 = 0, c1 = 0, b7 = 0, c81 = 0, d81 = 0;
    __asm__ volatile("cpuid" : "=a"(a1), "=b"(b), "=c"(c1), "=d"(d) : "a"(1u), "c"(0));
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b7), "=c"(c), "=d"(d) : "a"(7u), "c"(0));
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c81), "=d"(d81) : "a"(0x80000001u), "c"(0));
    printf("CPU: %s; avx %d avx2 %d bmi1 %d movbe %d lzcnt %d sse4a %d\n", name, (c1 >> 28) & 1, (b7 >> 5) & 1,
           (b7 >> 3) & 1, (c1 >> 22) & 1, (c81 >> 5) & 1, (c81 >> 6) & 1);
    MEMORYSTATUSEX memory;
    memset(&memory, 0, sizeof memory);
    memory.dwLength = sizeof memory;
    unsigned long long total = 0, avail = 0;
    if (GlobalMemoryStatusEx(&memory)) {
        total = memory.ullTotalPhys >> 20;
        avail = memory.ullAvailPhys >> 20;
    }
    unsigned long major = 0, minor = 0, build = 0;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn get_version = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (get_version) {
        OSVERSIONINFOW info;
        memset(&info, 0, sizeof info);
        info.dwOSVersionInfoSize = sizeof info;
        if (get_version(&info) == 0) {
            major = info.dwMajorVersion;
            minor = info.dwMinorVersion;
            build = info.dwBuildNumber;
        }
    }
    printf("System: Windows %lu.%lu build %lu, %llu MB memory (%llu MB free)\n", major, minor, build, total, avail);
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
        typedef const char *(CDECL * WineVersionFn)(void);
        WineVersionFn wine = (WineVersionFn)(void *)GetProcAddress(ntdll, "wine_get_version");
        printf("System: running under Wine/Proton %s", wine ? wine() : "?");
        fputc(10, stdout);
    }
    const char *summary = getenv("BB_SETTINGS_SUMMARY");
    if (summary && *summary) printf("Launcher: %s\n", summary);
    {
        const char *note = getenv("BB_PREFLIGHT_NOTE");
        if (note && *note) printf("Launcher warning: %s\n", note);
    }
    /* BB_* variables that test scripts or the launcher set (the ones that hold paths or names are left out). */
    {
        char *block = GetEnvironmentStringsA();
        if (block) {
            int shown = 0;
            for (const char *entry = block; *entry; entry += strlen(entry) + 1) {
                if (strncmp(entry, "BB_", 3) != 0) continue;
                if (!strncmp(entry, "BB_SETTINGS_SUMMARY=", 20) || !strncmp(entry, "BB_USER_NAME=", 13) ||
                    !strncmp(entry, "BB_CRASH_DIR=", 13) || !strncmp(entry, "BB_GPU_USER_DIR=", 16) ||
                    !strncmp(entry, "BB_CONFIG=", 10) || !strncmp(entry, "BB_TOGGLE_FILE=", 15) ||
                    !strncmp(entry, "BB_DUMP_DIR=", 12) || !strncmp(entry, "BB_CAPTURE_DIR=", 15))
                    continue;
                if (!shown) printf("Environment:");
                printf(" %s", entry);
                ++shown;
            }
            if (shown) fputc(10, stdout);
            FreeEnvironmentStringsA(block);
        }
    }
}
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
    start_tick=GetTickCount64();
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The launcher sends both streams to the same log file. Buffered stderr flushed in blocks, so a
       flush landed in the middle of a line another thread was writing and the reports came out
       spliced. One unbuffered write per line keeps every line whole. */
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    print_system_info();
#endif
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0_base_dir=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *mods_dir[4]={0}; int mods_n=0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--app0-base") && i+1<argc) app0_base_dir=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--mods") && i+1<argc) { const char *mv=argv[++i]; if (mods_n<4) mods_dir[mods_n++]=mv; }
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--stub-unresolved")) stub_unresolved=1;
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        runtime_file_set_base(app0_base_dir);
        for (int mi=0;mi<mods_n;++mi) { runtime_file_add_mods(mods_dir[mi]); runtime_benchmark_note_mods(mods_dir[mi]); }
        runtime_benchmark_configure(user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        bbnet_configure(app0,serial,title,version);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    atexit(note_exit);
    signal(SIGABRT,on_abort);
    SetConsoleCtrlHandler(console_closing,TRUE);
    if (getenv("BB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"BB_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>15)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    runtime_image_base = (uintptr_t)image; runtime_image_size = size;
    runtime_diag_start();
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
#ifdef _WIN32
        CreateThread(NULL,0,hang_watchdog,NULL,0,NULL);
        list_third_party_modules(stdout,"Third-party modules:");
#endif
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = stub_unresolved ? (uintptr_t)unresolved_soft : (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) scene_debug_install();
    if (!cpu_only) apply_upscale_render_size(image, size);
    if (!cpu_only) apply_cull_scale(image, size);
    if (!cpu_only) patch_shared_alloc(image, size);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    const char *sse4a_rewrite=getenv("BB_SSE4A_REWRITE"); /* 1 forces it on, 0 leaves every site to the fault handler */
    if (sse4a_rewrite ? strcmp(sse4a_rewrite,"0")!=0 : !host_has_sse4a()) {
        uint64_t left, rewritten=rewrite_sse4a(segments, ns, size, &left);
        printf("SSE4a: %" PRIu64 " guest EXTRQ rewritten, %" PRIu64 " left to the fault handler\n", rewritten, left);
    }
    menu_repeat_install();
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[3]; } params = {1, {"/app0/eboot.bin", NULL, NULL}};
    if ((params.argv[1]=runtime_benchmark_guest_arg())) params.argc=2;
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}

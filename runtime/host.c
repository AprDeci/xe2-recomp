/* Loader and dispatcher for the recompiled game: the console's dynamic linker, natively.
 *
 * Maps each recompiled module's image at its guest address, resolves symbols across the
 * modules in load order (plus native replacements), and then starts the process the way the
 * console does - by calling the SDK's nn::init::Start, which initialises the C library and
 * OS layer, runs the game's nninitStartup, asks the linker to run every module's static
 * constructors, and finally calls the game's nnMain.
 *
 * Usage: ssbu <directory with *.reloc.bin images> [--trace] */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "display.h"
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern const RtModule rt_module_main, rt_module_subsdk0, rt_module_sdk;
#define BOOT_MODULES 3
#define MAX_MODULES 1024
/* Loaded modules in load (and symbol search) order: the three the process starts with, then
 * the NROs the game loads through nn::ro. */
static const RtModule *modules[MAX_MODULES] = {&rt_module_main, &rt_module_subsdk0, &rt_module_sdk};
static u32 module_count = BOOT_MODULES;
static GuestFn **code[MAX_MODULES];               /* per module, indexed by (address - base) / 4 */

/* Dispatch: which module covers each 16 MB of the module area (module index + 1). */
#define SLOT_BASE 0x7000000000ull
#define SLOT_SHIFT 24
#define SLOT_COUNT 1024
static u16 slot_module[SLOT_COUNT];
static struct Native { GuestFn *fn; const char *name; } natives[8192];
static u32 native_count;
static __declspec(thread) u32 last_native;        /* which native slot the current thread just looked up */
static const char *phase = "start-up";
int rt_trace;
static int status_every;   /* seconds between thread dumps, 0 for none */
extern u64 gpu_stat_batches, gpu_stat_lists, gpu_stat_draws, gpu_stat_dispatches, gfx_stat_dispatched, gfx_stat_residents, gfx_stat_clears, gfx_stat_drawn, gfx_stat_skipped,
    gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_watch_us, gfx_skip_shader, gfx_skip_topology, gfx_skip_target;
u64 present_frames(void);
void gfx_report_compiles(void (*line)(const char *text));
const char *rt_data_dir;
/* This thread is reading another thread's stack (debug_profile.c): a fault there is expected and
 * handled where it happens, not a crash. */
__declspec(thread) int rt_probing;

/* ---- diagnostics ----------------------------------------------------------------- */
static struct HostFn { u64 host, guest; } *by_host;   /* guest == 0 marks runtime code */
static u32 by_host_count;

static int by_host_cmp(const void *a, const void *b) {
    u64 x = ((const struct HostFn *)a)->host, y = ((const struct HostFn *)b)->host;
    return x < y ? -1 : x > y;
}

const char *rt_describe(u64 addr, char *buf, size_t n) {
    for (u32 m = 0; m < module_count; m++)
        if (addr - modules[m]->base < modules[m]->image_size) {
            snprintf(buf, n, "%s+%#llx", modules[m]->name, addr - modules[m]->base);
            return buf;
        }
    if (addr - NATIVE_BASE < (u64)native_count * 16) snprintf(buf, n, "native:%s", natives[(addr - NATIVE_BASE) >> 4].name);
    else snprintf(buf, n, "%#llx", addr);
    return buf;
}

/* Guest function containing a host code address, or 0 for runtime code. */
static u64 guest_of(u64 pc) {
    u32 lo = 0, hi = by_host_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (by_host[mid].host <= pc) lo = mid + 1; else hi = mid;
    }
    return lo && pc - by_host[lo - 1].host <= (4u << 20) ? by_host[lo - 1].guest : 0;
}

/* A host code address in recompiled code, as the guest function's place ("main+0x1234"); NULL
 * for any other code (debug_profile.c's thread dumps). */
const char *rt_describe_host(u64 pc, char *buf, size_t n) {
    u64 guest = by_host ? guest_of(pc) : 0;
    return guest ? rt_describe(guest, buf, n) : NULL;
}

/* ---- reports: to the log, and for a crash or a stop also to reports\<kind>_<time>.txt ------ */
#ifdef _WIN32
static HANDLE report_file = INVALID_HANDLE_VALUE;
#else
static int report_fd = -1;   /* (Linux: a plain descriptor, so that writing a crash report allocates nothing) */
#endif

/* (no heap: a crash may have happened inside the allocator) */
static void out(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    fputs(buf, stderr);
#ifdef _WIN32
    DWORD written;
    if (report_file != INVALID_HANDLE_VALUE) WriteFile(report_file, buf, (DWORD)n, &written, NULL);
#else
    if (report_fd >= 0 && write(report_fd, buf, (size_t)n) < 0) report_fd = -1;
#endif
}
static void out_line(const char *text) { out("%s", text); }

/* reports\<kind>_<date>_<time>.<ext>, in the current directory (the port root, where every
 * launcher starts the game) */
void rt_report_path(char *path, size_t n, const char *kind, const char *ext) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("reports", NULL);
#ifdef _WIN32
    snprintf(path, n, "reports\\%s_%04u%02u%02u_%02u%02u%02u.%s", kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
#else
    snprintf(path, n, "reports/%s_%04u%02u%02u_%02u%02u%02u.%s", kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
#endif
}

static void report_open(const char *kind, char *path, size_t n) {
    rt_report_path(path, n, kind, "txt");
#ifdef _WIN32
    report_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
#else
    report_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

/* The report's last line (where it is), and closed. */
static void report_close(const char *path) {
#ifdef _WIN32
    if (report_file == INVALID_HANDLE_VALUE) return;
    out("report: %s\n", path);
    CloseHandle(report_file);
    report_file = INVALID_HANDLE_VALUE;
#else
    if (report_fd < 0) return;
    out("report: %s\n", path);
    close(report_fd);
    report_fd = -1;
#endif
}

#ifdef _WIN32
/* ---- minidumps, written by a second process (this exe with --write-dump): a process dumping
 * itself can deadlock on a lock one of its suspended threads holds. The request goes through a
 * helper thread started with the process, so a faulting thread with no stack left needs none.
 * SSBU_MINIDUMP=0: none; =full: all memory (the guest's too: several GB). */
static struct { HANDLE request, done; char path[MAX_PATH]; DWORD tid; EXCEPTION_POINTERS *e; volatile LONG ok, busy; } dumper;

static DWORD WINAPI dump_helper(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"minidump helper");
    for (;;) {
        WaitForSingleObject(dumper.request, INFINITE);
        wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH + 160];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --write-dump %lu %lu %llx \"%hs\"", exe, GetCurrentProcessId(), dumper.tid,
                   (unsigned long long)(uintptr_t)dumper.e, dumper.path);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        InterlockedExchange(&dumper.ok, code == 0);
        SetEvent(dumper.done);
    }
}

/* A minidump of this process at path (with thread tid's exception, if e); 1 when written. */
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms) {
    if (!dumper.request || InterlockedExchange(&dumper.busy, 1)) return 0;
    snprintf(dumper.path, sizeof dumper.path, "%s", path);
    dumper.tid = tid;
    dumper.e = e;
    ResetEvent(dumper.done);
    SetEvent(dumper.request);
    int ok = WaitForSingleObject(dumper.done, wait_ms) == WAIT_OBJECT_0 && dumper.ok;
    InterlockedExchange(&dumper.busy, 0);
    return ok;
}

static void dump_helper_start(void) {
    const char *m = getenv("SSBU_MINIDUMP");
    if (m && !strcmp(m, "0")) return;
    dumper.request = CreateEventW(NULL, FALSE, FALSE, NULL);
    dumper.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, dump_helper, NULL, 0, NULL));
}

/* The second process: ssbu --write-dump <pid> <thread id> <exception pointers, hex> <file> */
static int write_dump_main(char **argv) {
    DWORD pid = strtoul(argv[2], NULL, 10), tid = strtoul(argv[3], NULL, 10);
    unsigned long long ptrs = strtoull(argv[4], NULL, 16);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    BOOL (WINAPI *write)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                         PMINIDUMP_CALLBACK_INFORMATION) =
        dbghelp ? (BOOL (WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                   PMINIDUMP_CALLBACK_INFORMATION))(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!proc || !write) return 2;
    HANDLE f = CreateFileA(argv[5], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 3;
    const char *m = getenv("SSBU_MINIDUMP");
    MINIDUMP_TYPE type = m && !_stricmp(m, "full")
                             ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
                             : (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    MINIDUMP_EXCEPTION_INFORMATION mei = {tid, (PEXCEPTION_POINTERS)(uintptr_t)ptrs, TRUE};
    BOOL ok = write(proc, pid, f, type, ptrs ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    if (!ok) DeleteFileA(argv[5]);
    return ok ? 0 : 4;
}
#else
/* Linux: no minidumps. A crash or freeze report is text only; the system's core dump, where it
 * keeps them (ulimit -c, systemd-coredump), has the rest. */
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms) {
    (void)path; (void)tid; (void)e; (void)wait_ms;
    return 0;
}
#endif

static void print_frames(void **frames, int n) {
    char name[96];
    out("guest backtrace (innermost first):\n");
    for (int i = 0, shown = 0; i < n && shown < 28; i++) {
        u64 guest = guest_of((u64)frames[i]);
        if (!guest) {   /* runtime code: its offset in the executable (see the linker map) */
            static u64 image;
            if (!image) image = (u64)GetModuleHandleW(NULL);
            if (rt_trace || getenv("SSBU_HOST_FRAMES")) out("  [host +%#llx]\n", (u64)frames[i] - image);
            continue;
        }
        out("  %s\n", rt_describe(guest, name, sizeof name));
        shown++;
    }
}

void rt_backtrace(void) {
    void *frames[62];
    print_frames(frames, CaptureStackBackTrace(1, 62, frames, NULL));
}

#ifdef _WIN32
/* Unwind a host context, collecting return addresses. */
static int unwind(CONTEXT ctx, void **frames, int max) {
    int n = 0;
    while (n < max && ctx.Rip) {
        frames[n++] = (void *)ctx.Rip;
        DWORD64 image;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!fn) {   /* a leaf function: no unwind data, the return address is on top of the stack */
            if (!ctx.Rsp || IsBadReadPtr((void *)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void *handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &establisher, NULL);
    }
    return n;
}

void rt_backtrace_thread(HANDLE thread, int max_frames) {
    void *frames[96];
    char name[96];
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == (DWORD)-1) return;
    int n = GetThreadContext(thread, &ctx) ? unwind(ctx, frames, 96) : 0;
    ResumeThread(thread);
    static int host_frames = -1;
    if (host_frames < 0) host_frames = getenv("SSBU_HOST_FRAMES") != NULL;
    u64 image = (u64)GetModuleHandleW(NULL);
    for (int i = 0, shown = 0; i < n && shown < max_frames; i++) {
        u64 guest = guest_of((u64)frames[i]);
        if (!guest) {
            if (host_frames && (u64)frames[i] - image < 0x40000000ull) fprintf(stderr, "      [host +%#llx]\n", (u64)frames[i] - image);
            else if (host_frames) {
                HMODULE mod = NULL;
                char path[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)frames[i], &mod);
                if (mod) GetModuleFileNameA(mod, path, sizeof path);
                const char *base = strrchr(path, '\\');
                fprintf(stderr, "      [%s +%#llx]\n", base ? base + 1 : path, (u64)frames[i] - (u64)mod);
            }
            continue;
        }
        fprintf(stderr, "      %s\n", rt_describe(guest, name, sizeof name));
        shown++;
    }
}

/* A host fault inside recompiled code is a guest fault: report where, and what it touched. */
static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (rt_probing) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG faulting;
    if (InterlockedExchange(&faulting, 1)) {   /* another thread is reporting its fault: let it finish */
        Sleep(90000);
        TerminateProcess(GetCurrentProcess(), 6);
    }
    char name[96], report[MAX_PATH];
    fflush(stdout);
    report_open("crash", report, sizeof report);
    out("\n*** host exception %#lx during %s", code, phase);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        out(": %s address %s", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            rt_describe(e->ExceptionRecord->ExceptionInformation[1], name, sizeof name));
    out("\n");
    /* where on the host: the faulting instruction's module and offset (symbolize with tools/hostsym.py) */
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    u64 pc = (u64)e->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
        GetModuleFileNameA(mod, modname, sizeof modname);
    out("host pc %#llx: %s +%#llx (thread %lu)\n", pc, strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname,
        pc - (u64)mod, GetCurrentThreadId());
    {   /* and the host frames above it */
        CONTEXT ctx = *e->ContextRecord;
        for (int i = 0; i < 24; i++) {
            DWORD64 image;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
            if (!f) break;
            void *handler;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler, &establisher, NULL);
            if (!ctx.Rip) break;
            mod = NULL;
            strcpy(modname, "?");
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &mod))
                GetModuleFileNameA(mod, modname, sizeof modname);
            out("  host frame %s +%#llx\n", strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname, ctx.Rip - (u64)mod);
        }
    }
    gfx_report_compiles(out_line);   /* (a fault in the driver's shader compiler names the program) */
    void *frames[62];
    print_frames(frames, unwind(*e->ContextRecord, frames, 62));   /* from the faulting frame */
    char dump[MAX_PATH];
    snprintf(dump, sizeof dump, "%.*s.dmp", (int)strlen(report) - 4, report);
    int dumped = report_file != INVALID_HANDLE_VALUE && rt_write_dump(dump, GetCurrentThreadId(), e, 100000);
    if (report_file != INVALID_HANDLE_VALUE) out("report: %s%s%s\n", report, dumped ? "; minidump: " : "", dumped ? dump : "");
    if (report_file != INVALID_HANDLE_VALUE) CloseHandle(report_file);
    fflush(stderr);
    ExitProcess(5);
}
#else
/* ---- Linux: faults and other threads' stacks come through signals ---------------------- */
#include <dlfcn.h>
#include <execinfo.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>

int rt_ww_fault(void *address);        /* (linux/win32.c) a write the fallback write watch takes */
void rt_thread_signal_stack(void);

static void host_frame(const char *indent, void *pc) {   /* "[module +offset]" */
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        out("%s[%s +%#llx]%s%s\n", indent, base ? base + 1 : di.dli_fname, (u64)pc - (u64)di.dli_fbase,
            di.dli_sname ? " " : "", di.dli_sname ? di.dli_sname : "");
    } else out("%s[%#llx]\n", indent, (u64)pc);
}

/* Another thread's frames: it is interrupted and records its own stack. */
#define SIG_BACKTRACE (SIGRTMIN + 2)
static void *bt_frames[96];
static volatile int bt_count;
static sem_t bt_done;

static void on_backtrace_signal(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    int saved = errno;
    bt_count = backtrace(bt_frames, 96);
    sem_post(&bt_done);
    errno = saved;
}

void rt_backtrace_thread(HANDLE thread, int max_frames) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pid_t tid = rt_thread_tid(thread);
    if (!tid) return;
    pthread_mutex_lock(&lock);
    while (sem_trywait(&bt_done) == 0) {}   /* (an answer that came after its wait gave up) */
    bt_count = 0;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 250000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    if (syscall(SYS_tgkill, getpid(), tid, SIG_BACKTRACE) == 0 && sem_timedwait(&bt_done, &until) == 0) {
        static int host_frames = -1;
        if (host_frames < 0) host_frames = getenv("SSBU_HOST_FRAMES") != NULL;
        char name[96];
        for (int i = 2, shown = 0; i < bt_count && shown < max_frames; i++) {   /* (past the handler and the signal frame) */
            u64 guest = guest_of((u64)bt_frames[i]);
            if (!guest) { if (host_frames) host_frame("      ", bt_frames[i]); continue; }
            fprintf(stderr, "      %s\n", rt_describe(guest, name, sizeof name));
            shown++;
        }
    }
    pthread_mutex_unlock(&lock);
}

/* A host fault inside recompiled code is a guest fault: report where, and what it touched.
 * (Writes to pages the fallback write watch protects are not faults: they are noted and go on.) */
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    if (sig == SIGSEGV && (uc->uc_mcontext.gregs[REG_ERR] & 2) && rt_ww_fault(si->si_addr)) return;
    static volatile pid_t reporter;
    pid_t me = gettid(), none = 0;
    if (reporter == me) _exit(6);   /* (a fault while reporting one) */
    if (!__atomic_compare_exchange_n(&reporter, &none, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) for (;;) pause();   /* (one report at a time) */
    char name[96], report[MAX_PATH];
    fflush(stdout);
    report_open("crash", report, sizeof report);
    out("\n*** host exception (signal %d: %s) during %s", sig, sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error" :
        sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic" : "?", phase);
    if (sig == SIGSEGV || sig == SIGBUS)
        out(": %s address %s", uc->uc_mcontext.gregs[REG_ERR] & 2 ? "writing" : "reading", rt_describe((u64)si->si_addr, name, sizeof name));
    out("\n");
    u64 pc = (u64)uc->uc_mcontext.gregs[REG_RIP];
    out("host pc %#llx (thread %d %s):\n", pc, (int)gettid(), rt_thread_description(gettid()));
    host_frame("  ", (void *)pc);
    void *frames[64];
    int n = backtrace(frames, 64), from = 0;
    for (int i = 0; i < n; i++) if ((u64)frames[i] == pc) { from = i; break; }   /* (the faulting frame onwards) */
    for (int i = from + 1; i < n && i < from + 24; i++) host_frame("  host frame ", frames[i]);
    gfx_report_compiles(out_line);   /* (a fault in the shader compiler names the program) */
    print_frames(frames + from, n - from);
    report_close(report);
    fflush(stderr);
    _exit(5);
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_backtrace_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIG_BACKTRACE, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    sem_init(&bt_done, 0, 0);
    rt_thread_signal_stack();
    void *warm[4];
    backtrace(warm, 4);   /* (its first call loads the unwinder: not inside a signal handler) */
}
#endif

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    char text[768], report[MAX_PATH];
    fflush(stdout);
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    report_open("stopped", report, sizeof report);
    out("\n*** stopped during %s: %s\n", phase, text);
    rt_backtrace();
    report_close(report);
    fflush(stderr);
    ExitProcess(3);
}

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    char name[96];
    (void)c;
    rt_fatal("guest trap at %s: %s", rt_describe(pc, name, sizeof name), why);
}

/* The original of a wrapped guest function (HLE_WRAP) this game version does not have:
 * tools/recomp.py links each f_<address>_orig no module defines to this one. Nothing calls
 * it, since nothing reaches that wrapper either. */
void rt_wrap_missing(Cpu *c) { rt_trap(c, 0, "wrapped guest function missing from this game version"); }

/* ---- dispatch -------------------------------------------------------------------- */
/* The tables are 2 bytes per byte of code (hundreds of MB), so a lookup in them mostly misses
 * the processor's caches; a small cache of recent ones goes first. Modules are never unloaded,
 * so an entry never goes stale. Its key is the address mixed with the function, so an entry
 * another thread is halfway through writing never matches. */
#define LOOKUP_CACHE 8192
static struct { volatile u64 key; GuestFn *volatile fn; } lookup_cache[LOOKUP_CACHE];
#define LOOKUP_MIX(fn) ((u64)(fn) * 0x9E3779B97F4A7C15ull)

GuestFn *rt_lookup(u64 addr) {
    char name[96];
    u32 h = (u32)((addr >> 2) * 0xD6E8FEB86659FD93ull >> 51) & (LOOKUP_CACHE - 1);
    GuestFn *hit = lookup_cache[h].fn;
    if ((lookup_cache[h].key ^ LOOKUP_MIX(hit)) == addr && hit) return hit;
    u64 slot = (addr - SLOT_BASE) >> SLOT_SHIFT;
    if (slot < SLOT_COUNT && slot_module[slot]) {
        u32 m = slot_module[slot] - 1u;
        u64 off = addr - modules[m]->base;
        if (off < modules[m]->text_size) {
            GuestFn *fn = off & 3 ? NULL : code[m][off >> 2];
            if (fn) {
                lookup_cache[h].fn = fn;
                lookup_cache[h].key = addr ^ LOOKUP_MIX(fn);
                return fn;
            }
            rt_fatal("indirect call to %s, which is not a function entry", rt_describe(addr, name, sizeof name));
        }
    }
    u64 off = addr - NATIVE_BASE;
    if (off < (u64)native_count * 16 && !(off & 15)) {
        last_native = (u32)(off >> 4);
        return natives[off >> 4].fn;
    }
    rt_fatal("indirect call to %#llx, which is outside every module", addr);
}

static void unresolved(Cpu *c);

GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) {
    GuestFn *fn = rt_lookup(addr);
    if (fn != unresolved && addr) {   /* (that one finds its symbol from the lookup itself) */
        cache->fn = fn;
        cache->key = addr ^ RT_CACHE_MIX(fn);
    }
    return fn;
}

void rt_call(Cpu *c, u64 guest_addr) {
    c->x[30] = 0;
    rt_lookup(guest_addr)(c);
}

u64 rt_symbol(const char *name);

u64 rt_callsym(Cpu *c, const char *symbol, int n, ...) {
    u64 fn = rt_symbol(symbol);
    if (!fn) rt_fatal("native code called %s, which no module exports", symbol);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c->x[i] = va_arg(ap, u64);
    va_end(ap);
    rt_call(c, fn);
    return c->x[0];
}

u64 rt_now_ns(void) {
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&t);
    u64 d = (u64)(t.QuadPart - start.QuadPart);
    return d / (u64)freq.QuadPart * 1000000000ull + d % (u64)freq.QuadPart * 1000000000ull / (u64)freq.QuadPart;
}

/* The second half of the native region holds small constant data handed to the guest. */
u64 rt_guest_copy(const void *data, size_t size) {
    static volatile LONG64 used;
    u64 at = NATIVE_BASE + 0x80000 + (u64)InterlockedExchangeAdd64(&used, (LONG64)((size + 15) & ~15ull));
    if (at + size > NATIVE_BASE + 0x100000) rt_fatal("native data region exhausted");
    memcpy(P(void, at), data, size);
    return at;
}

u64 rt_native(GuestFn *fn, const char *name) {
    if (native_count == sizeof natives / sizeof *natives) rt_fatal("too many native functions");
    natives[native_count] = (struct Native){fn, name};
    return NATIVE_BASE + 16ull * native_count++;
}

/* ---- symbols --------------------------------------------------------------------- */
/* missing: nothing defines the name yet (a module loaded later may). */
static struct Sym { const char *name; u64 addr; u8 missing; } *symtab;
static u32 symtab_size, unresolved_count;
static struct Sym *sym_slot(const char *name);

/* Relocations that refer to a missing symbol, patched when a loaded module defines it. */
static struct Site { struct Sym *sym; u64 at; i64 addend; } *sites;
static u32 site_count, site_cap;

/* Bound to symbols nothing defines: binds lazily, as the console's linker does for the
 * game's references into modules it loads later, or reports the missing symbol. */
static void unresolved(Cpu *c) {
    const char *name = natives[last_native].name;
    struct Sym *s = sym_slot(name);
    if (s->name && !s->missing) { rt_lookup(s->addr)(c); return; }
    rt_fatal("call to %s, which no module defines and has no native implementation", name);
}

static u32 hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static struct Sym *sym_slot(const char *name) {
    u32 i = hash(name) & (symtab_size - 1);
    while (symtab[i].name && strcmp(symtab[i].name, name)) i = (i + 1) & (symtab_size - 1);
    return &symtab[i];
}

u64 rt_symbol(const char *name) { return sym_slot(name)->addr; }

/* Functions the loader wraps so that progress through start-up is visible. */
static u64 real_startup, real_main;
static void wrap_startup(Cpu *c) { phase = "nninitStartup"; printf("[boot] nninitStartup\n"); fflush(stdout); rt_call(c, real_startup); phase = "start-up after nninitStartup"; }
static void wrap_main(Cpu *c) { phase = "nnMain"; printf("[boot] nnMain entered\n"); fflush(stdout); rt_call(c, real_main); printf("[boot] nnMain returned\n"); }

static u64 resolve(const char *name, int weak) {
    struct Sym *s = sym_slot(name);
    if (s->name) return s->addr;
    u64 addr = 0;
    for (u32 k = 0; k < hle_count && !addr; k++)
        if (!strcmp(hle_table[k].name, name)) addr = rt_native(hle_table[k].fn, hle_table[k].name);
    int missing = !addr;
    if (!addr && !weak) {
        addr = rt_native(unresolved, name);
        unresolved_count++;
    }
    *s = (struct Sym){name, addr, (u8)missing};
    return addr;
}

/* ---- loading --------------------------------------------------------------------- */
int main(int argc, char **argv);
static DWORD WINAPI guest_main(LPVOID unused);
static u32 by_host_cap;

/* Map module m's image at its guest address and make its code reachable. */
static void map_module(u32 m, const char *dir) {
    const RtModule *mod = modules[m];
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.reloc.bin", dir, mod->name);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    u64 size = (mod->image_size + 0xFFFF) & ~0xFFFFull, text = (mod->text_size + 0xFFF) & ~0xFFFull;
    u8 *image = VirtualAlloc((void *)mod->base, size, MEM_COMMIT, PAGE_READWRITE);   /* inside its reservation (NROs) */
    if (image != (u8 *)mod->base) image = VirtualAlloc((void *)mod->base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (image != (u8 *)mod->base) rt_fatal("cannot map %s at %#llx", mod->name, mod->base);
    if (fread(image, 1, mod->image_size, f) < mod->text_size) rt_fatal("%s is truncated", path);
    fclose(f);
    kernel_add_region(mod->base, text, 3, 5);                        /* code */
    kernel_add_region(mod->base + text, size - text, 4, 3);          /* data */

    for (u64 a = mod->base; a < mod->base + size; a += 1ull << SLOT_SHIFT) {
        u64 slot = (a - SLOT_BASE) >> SLOT_SHIFT;
        if (slot >= SLOT_COUNT || (slot_module[slot] && slot_module[slot] != m + 1))
            rt_fatal("%s at %#llx overlaps another module's dispatch slot", mod->name, mod->base);
        slot_module[slot] = (u16)(m + 1);
    }
    code[m] = VirtualAlloc(NULL, mod->text_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (by_host_count + mod->func_count > by_host_cap) {
        by_host_cap = (by_host_count + mod->func_count) * 2;
        by_host = realloc(by_host, sizeof *by_host * by_host_cap);
    }
    for (u32 i = 0; i < mod->func_count; i++) {
        code[m][(mod->funcs[i].addr - mod->base) >> 2] = mod->funcs[i].fn;
        by_host[by_host_count++] = (struct HostFn){(u64)mod->funcs[i].fn, mod->funcs[i].addr};
    }
}

/* Publish module m's exports. The first module to define a name wins, except over a name
 * nothing defined yet: references to it already made are patched to the definition. */
static void publish_exports(u32 m) {
    const RtModule *mod = modules[m];
    int defined_missing = 0;
    for (u32 i = 0; i < mod->export_count; i++) {
        struct Sym *s = sym_slot(mod->exports[i].name);
        if (!s->name) *s = (struct Sym){mod->exports[i].name, mod->exports[i].addr, 0};
        else if (s->missing) { s->addr = mod->exports[i].addr; s->missing = 0; defined_missing = 1; }
    }
    if (!defined_missing) return;
    for (u32 i = 0; i < site_count;)
        if (!sites[i].sym->missing) {
            st64(sites[i].at, sites[i].sym->addr + (u64)sites[i].addend);
            sites[i] = sites[--site_count];
        } else i++;
}

/* Resolve module m's symbol references and apply its symbol relocations. */
static u32 bind_module(u32 m) {
    const RtModule *mod = modules[m];
    u64 *target = malloc(sizeof *target * (mod->import_count + 1));
    struct Sym **sym = malloc(sizeof *sym * (mod->import_count + 1));
    for (u32 i = 0; i < mod->import_count; i++) {
        target[i] = resolve(mod->imports[i], mod->import_weak[i]);
        sym[i] = sym_slot(mod->imports[i]);
    }
    for (u32 i = 0; i < mod->reloc_count; i++) {
        const RtReloc *r = &mod->relocs[i];
        st64(mod->base + r->offset, target[r->import] + (u64)r->addend);
        if (sym[r->import]->missing) {
            if (site_count == site_cap) sites = realloc(sites, sizeof *sites * (site_cap = site_cap * 2 + 1024));
            sites[site_count++] = (struct Site){sym[r->import], mod->base + r->offset, r->addend};
        }
    }
    free(target);
    free(sym);
    return mod->reloc_count;
}

static void load(const char *dir) {
    u32 funcs = 0, exports = 0, relocs = 0;
    for (u32 m = 0; m < module_count; m++) { funcs += modules[m]->func_count; exports += modules[m]->export_count; }
    for (u32 m = 0; m < rt_nro_count; m++) exports += rt_nro_modules[m]->export_count;
    for (symtab_size = 1024; symtab_size < exports * 3 + 65536; symtab_size *= 2) {}
    symtab = calloc(symtab_size, sizeof *symtab);
    by_host_cap = funcs + 16;
    by_host = malloc(sizeof *by_host * by_host_cap);

    for (u32 m = 0; m < module_count; m++) {
        map_module(m, dir);
        publish_exports(m);
    }
    /* The NROs the game loads later each have a fixed address: keep their ranges free now,
     * before the graphics driver and the heap start taking address space (they did, and a
     * fighter's module then could not be mapped). */
    for (u32 k = 0; k < rt_nro_count; k++) {
        const RtModule *nro = rt_nro_modules[k];
        u64 size = (nro->image_size + 0xFFFF) & ~0xFFFFull;
        if (VirtualAlloc((void *)nro->base, size, MEM_RESERVE, PAGE_NOACCESS) != (void *)nro->base)
            fprintf(stderr, "[ro] warning: cannot reserve %s's range at %#llx (error %lu)\n", nro->name, nro->base, (unsigned long)GetLastError());
    }
    GuestFn *own[] = {(GuestFn *)rt_call, (GuestFn *)rt_lookup, (GuestFn *)rt_backtrace, (GuestFn *)rt_fatal,
                      (GuestFn *)rt_trap, (GuestFn *)rt_svc, (GuestFn *)load, (GuestFn *)main, (GuestFn *)guest_main,
                      wrap_startup, wrap_main, unresolved};
    for (u32 i = 0; i < sizeof own / sizeof *own; i++) by_host[by_host_count++] = (struct HostFn){(u64)own[i], 0};
    qsort(by_host, by_host_count, sizeof *by_host, by_host_cmp);

    /* Wrap the two entry points the SDK calls back into, to report progress. */
    real_startup = rt_symbol("nninitStartup");
    real_main = rt_symbol("nnMain");
    sym_slot("nninitStartup")->addr = rt_native(wrap_startup, "nninitStartup");
    sym_slot("nnMain")->addr = rt_native(wrap_main, "nnMain");

    if (VirtualAlloc((void *)NATIVE_BASE, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)NATIVE_BASE) {
        fprintf(stderr, "cannot map the native-function region\n");
        exit(2);
    }
    for (u32 m = 0; m < module_count; m++) relocs += bind_module(m);
    printf("loaded %u modules: %u functions, %u exported symbols, %u symbol relocations; "
           "%u symbols bound to native code, %u left undefined (%u NROs recompiled for loading later)\n",
           module_count, funcs, exports, relocs, hle_count, unresolved_count, rt_nro_count);
}

/* nn::ro: load the recompiled module built from the NRO with this image - map it at its
 * fixed address, link it against everything loaded so far and run its initialiser. Returns
 * its load address, or 0 if no recompiled module matches the image. */
u64 rt_load_nro(Cpu *c, u64 image) {
    static SRWLOCK lock = SRWLOCK_INIT;
    char id[65];
    for (int i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02X", *P(u8, image + 0x40 + i));
    for (int n = 64; n > 1 && id[n - 1] == '0'; n--) id[n - 1] = 0;
    AcquireSRWLockExclusive(&lock);
    for (u32 m = BOOT_MODULES; m < module_count; m++)
        if (!strcmp(modules[m]->build_id, id)) { ReleaseSRWLockExclusive(&lock); return modules[m]->base; }
    const RtModule *mod = NULL;
    for (u32 k = 0; k < rt_nro_count && !mod; k++)
        if (!strcmp(rt_nro_modules[k]->build_id, id)) mod = rt_nro_modules[k];
    if (!mod || module_count == MAX_MODULES) { ReleaseSRWLockExclusive(&lock); return 0; }
    u32 m = module_count;
    modules[m] = mod;
    map_module(m, rt_data_dir);
    qsort(by_host, by_host_count, sizeof *by_host, by_host_cmp);
    publish_exports(m);
    u32 relocs = bind_module(m);
    module_count++;
    ReleaseSRWLockExclusive(&lock);
    printf("[ro] loaded %s at %#llx: %u functions, %u exports, %u symbol relocations\n",
           mod->name, mod->base, mod->func_count, mod->export_count, relocs);
    fflush(stdout);
    if (mod->init != mod->base) rt_call(c, mod->init);
    return mod->base;
}

/* ---- start-up -------------------------------------------------------------------- */
static void exception_handler_ready(Cpu *c) { (void)c; }

/* The linker's part of start-up: run each module's init routine, dependencies first. Each
 * routine registers its module with the C library and runs its static constructors. */
static void call_initializers(Cpu *c) {
    for (u32 m = BOOT_MODULES; m-- > 0;) {
        static char label[64];
        snprintf(label, sizeof label, "static constructors of %s", modules[m]->name);
        phase = label;
        printf("[boot] %s\n", label);
        fflush(stdout);
        rt_call(c, modules[m]->init);
    }
    phase = "start-up after static constructors";
}

static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"guest main thread");
    u32 handle;
    Cpu *c = kernel_main_thread(&handle);
    u64 start = rt_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) rt_fatal("the SDK does not export nn::init::Start");
    c->x[0] = handle;
    c->x[1] = 0;
    c->x[2] = rt_native(exception_handler_ready, "exception_handler_ready");
    c->x[3] = rt_native(call_initializers, "call_initializers");
    phase = "SDK initialisation (nn::init::Start)";
    printf("[boot] nn::init::Start\n");
    fflush(stdout);
    rt_call(c, start);
    return 0;
}

static void print_status(void) {
    fprintf(stderr, "\n[status] %.0f s into %s; GPU: %llu submissions, %llu command lists, %llu draws, %llu clears, %llu compute launches (%llu run, %llu GPU-resident buffers made); %llu frames presented\n",
            (double)rt_now_ns() / 1e9, phase, gpu_stat_batches, gpu_stat_lists, gpu_stat_draws, gfx_stat_clears, gpu_stat_dispatches,
            gfx_stat_dispatched, gfx_stat_residents,
            present_frames());
    fprintf(stderr, "[status] renderer: %llu draws drawn, %llu skipped (%llu shader, %llu topology, %llu target); %llu shaders (%llu failed), %llu textures; %llu ms watching writes\n",
            gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target, gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_watch_us / 1000);
    kernel_dump_threads();
    fflush(stderr);
}

#ifndef _WIN32
int rt_window_init(void);                                  /* (linux/window.c) */
_Noreturn void rt_window_loop(int every, void (*status)(void));

/* (the main thread runs the window's events: the status dumps, which stop every thread a
 * moment, go on a thread of their own so that input never waits for them) */
static DWORD WINAPI status_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"status");
    for (;;) {
        Sleep((DWORD)status_every * 1000);
        print_status();
    }
}
#endif

/* SSBU_CRASH_TEST=<seconds>: a deliberate fault then (to try the crash report and minidump). */
static DWORD WINAPI crash_test(void *seconds) {
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    fprintf(stderr, "[test] SSBU_CRASH_TEST: writing to address 0\n");
    *(volatile int *)0 = 0;
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc >= 6 && !strcmp(argv[1], "--write-dump")) return write_dump_main(argv);
#endif
    if (argc < 2) { fprintf(stderr, "usage: ssbu <directory with *.reloc.bin images> [--trace]\n"); return 2; }
    display_dpi_aware();
#ifdef _WIN32
    {   /* which build this is, for reports */
        wchar_t exe[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) {
            FILETIME local;
            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &st);
        }
        fprintf(stderr, "[boot] %ls, built %04u-%02u-%02u %02u:%02u\n", exe, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
#else
    {   /* which build this is, for reports (and the time zone read now, not first in a crash report) */
        char exe[4096];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[len > 0 ? len : 0] = 0;
        struct stat st;
        struct tm tm = {0};
        tzset();
        if (len > 0 && !stat(exe, &st)) localtime_r(&st.st_mtime, &tm);
        fprintf(stderr, "[boot] %s, built %04d-%02d-%02d %02d:%02d\n", exe, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
#endif
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) rt_trace = 1;
        if (!strcmp(argv[i], "--status") && i + 1 < argc) status_every = atoi(argv[++i]);
    }
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);
    dump_helper_start();
#else
    install_signals();
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "[mem] write watch: %s\n", rt_ww_mode());
#endif
    if (getenv("SSBU_CRASH_TEST")) CloseHandle(CreateThread(NULL, 0, crash_test, (void *)(uintptr_t)atoi(getenv("SSBU_CRASH_TEST")), 0, NULL));
    rt_data_dir = argv[1];
    kernel_init();
    load(argv[1]);
#ifndef _WIN32
    rt_window_init();   /* (SDL wants its window and events on the main thread, which then runs them) */
#endif
    /* Guest call depth becomes host call depth, so the guest runs on a thread with a large stack. */
    HANDLE t = CreateThread(NULL, 512ull << 20, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#ifdef _WIN32
    while (WaitForSingleObject(t, status_every ? status_every * 1000 : INFINITE) == WAIT_TIMEOUT) print_status();
#else
    (void)t;
    if (status_every > 0) CloseHandle(CreateThread(NULL, 0, status_thread, NULL, 0, NULL));
    rt_window_loop(0, NULL);
#endif
    return 0;
}

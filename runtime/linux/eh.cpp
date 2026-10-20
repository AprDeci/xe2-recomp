/* The host exception behind guest C++ exceptions, on Linux (see hle_unwind.c and recomp.h).
 *
 * rt_unwind_jumpto names the landing pad and the stack pointer of the frame to resume; this
 * throws them as a C++ exception, which every landing site's catch looks at (rt_eh_filter) and
 * passes on unless it is the one. If no frame takes it, the C++ runtime terminates: the
 * terminate handler says which pad nobody resumed, as the Windows build does. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

namespace std {   /* (no C++ library headers in this build: just what is needed) */
typedef void (*terminate_handler)();
terminate_handler set_terminate(terminate_handler) noexcept;
}

extern "C" {
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...);
const char *rt_describe(u64 guest_addr, char *buf, size_t n);
}

static thread_local u64 pending_lp, pending_sp;

static void nobody_resumed() {
    char name[96];
    if (pending_lp)
        rt_fatal("C++ exception: no recompiled frame resumes landing pad %s (sp %#llx)", rt_describe(pending_lp, name, sizeof name),
                 (unsigned long long)pending_sp);
    rt_fatal("a host C++ exception was not caught");
}

static struct Install { Install() { std::set_terminate(nobody_resumed); } } install;

extern "C" __attribute__((noreturn)) void rt_eh_raise(u64 lp, u64 sp) {
    pending_lp = lp;
    pending_sp = sp;
    throw RtEhSignal{lp, sp};
}

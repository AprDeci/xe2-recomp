/* Generic stub services, natively.
 *
 * WindowSmash implements, by name, the services SUPER SMASH BROS actually calls (vi, audio,
 * mii, mm, hwopus) and HLEs nn::hid's state queries directly. Another engine (here
 * Xenoblade 2 / Monolith) reaches the console through the ordinary nnSdk IPC path: it opens
 * many more named services through sm: and drives them with CMIF commands. Most of those
 * commands just report success during initialisation; only a few return a fixed-layout
 * struct the game unpacks immediately.
 *
 * This module lets sm: hand out a session for any known service and answers commands with a
 * plain success (no output), which keeps initialisation moving. Queries whose output the game
 * actually depends on are added explicitly in ipc_boot.c as the boot trace shows the need.
 * Nothing here emulates hardware; the real work (hid state, graphics, filesystem) is supplied
 * elsewhere or kept for later.
 */
#include <string.h>

#include "ipc.h"

/* Services we provide a stub object for. Lookup is by exact name. */
static const char *const PROVIDED[] = {
    /* input */
    "hid", "hid:0", "hid:1", "hid:2", "hid:3", "hid:4", "hid:sys",
    /* filesystem */
    "fsp-srv", "fsp-ldr", "fsp:ldr", "fsp:save", "fsp:pr",
    /* gpu / display */
    "nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t", "nvdisp", "nvdisp:a",
    /* applets, application manager, loaders */
    "appletOE", "appletAE", "appletManager", "am", "am:sys", "am:net", "am:app",
    "ldr", "ldr:ro", "ldr:shel", "pm:dmnt", "pm:shell", "pm:info", "pm:bm",
    /* settings, time, accounts */
    "set", "set:sys", "set:cal", "set:fd", "settings",
    "time", "time:a", "time:s", "time:u", "time:su",
    "account", "account:a", "acnt:a", "sprofile:u", "sprofile:a",
    /* add-on content, amiibo */
    "aoc:u", "nfp:user", "nfp:sys",
    /* clocks / random / power / performance */
    "pcv", "pcv:disp", "csrng", "spl:", "spl:mii", "spl:net", "spl:ssl", "spl:es",
    "pctl", "pctl:a", "apm", "apm:p", "apm:sys",
    /* networking / reporting / misc system */
    "bsd:u", "bsd:s", "nifm:a", "nifm:s", "nifm:u", "socket:su",
    "prepo:u", "prepo:a", "srepo:u", "ntc:u", "eupld:r",
    "insc:srv", "xcd:sys", "usb:hs", "usb:hs:net", "usb:ds",
    "spsm", "pdm:qry", "grc:s", "grc:c", "sysup:u", "olsc:u",
    "mm:u", "mii:e", "mii:u", "sdmmc:", "htcs:", "erpt:r", "ro:1", "lr", "lr:u",
    NULL,
};

static void generic_handle(IpcObject *self, Ipc *ipc) {
    /* Succeed with no output; ipc_unknown logs each (service, command) once for the trace. */
    ipc_unknown(ipc, self);
}

IpcObject *service_generic(const char *name) {
    for (const char *const *p = PROVIDED; *p; p++)
        if (!strcmp(name, *p)) return ipc_new_object(*p, generic_handle, NULL);
    return NULL;
}

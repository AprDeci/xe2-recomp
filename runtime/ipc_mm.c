/* mm:u - the multimedia clock manager, natively. Video decoding asks it for engine clock
 * rates; here every request is granted and the rate reads back as the maximum asked for. */
#include <string.h>

#include "ipc.h"

static struct { u32 used, rate; } requests[64];

static void request(IpcObject *self, Ipc *ipc) {
    u32 id = ipc_u32(ipc, 0) % 64;
    switch (ipc->command) {
    case 0: case 1: return;                                         /* InitializeOld / FinalizeOld */
    case 2: case 6: requests[id].rate = ipc_u32(ipc, 8); return;   /* SetAndWait(Old)(id, min, max) */
    case 3: case 7: IPC_PUSH(ipc, requests[id].rate); return;      /* Get(Old) */
    case 4:                                                          /* Initialize(module, priority, clear mode) */
        for (u32 i = 1; i < 64; i++)
            if (!requests[i].used) { requests[i].used = 1; IPC_PUSH(ipc, i); return; }
        IPC_PUSH(ipc, (u32)63);
        return;
    case 5: requests[id].used = 0; return;                          /* Finalize */
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_mm(const char *name) {
    return strcmp(name, "mm:u") ? NULL : ipc_new_object("mm:u", request, NULL);
}

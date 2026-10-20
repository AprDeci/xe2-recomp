/* hid - the input service, natively.
 *
 * The state queries a game makes (nn::hid::GetNpadState and friends) are implemented directly
 * in hle_hid.c and reached through the recompiler's native forwarding, so they do not come
 * here. What comes here is the IPC handshake every engine performs first: open the "hid"
 * service, CreateAppletResource, and map the shared memory it hands back. That memory is where
 * the console publishes controller state; supplying a real, mapped block lets the mapping and
 * later reads succeed. The game's own GetNpadState calls still return live pad state from
 * hle_hid.c, so the block stays a valid but quiet snapshot.
 */
#include <string.h>

#include "ipc.h"

enum { HID_SHARED_SIZE = 0x4000 };   /* the AppletResource shared-memory block */

static u32 shmem_handle;

static void hid_root(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: {   /* CreateAppletResource(appletResourceUserId) -> shared-memory handle */
        if (!shmem_handle) {
            void *mem = NULL;
            shmem_handle = kernel_new_shared_memory(&mem, HID_SHARED_SIZE);
            memset(mem, 0, HID_SHARED_SIZE);
        }
        ipc_copy_handle(ipc, shmem_handle);
        return;
    }
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_hid(const char *name) {
    if (strcmp(name, "hid")) return NULL;
    return ipc_new_object("hid", hid_root, NULL);
}

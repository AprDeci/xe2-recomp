/* Native system services: the server side of the console's IPC (HIPC transport, CMIF
 * command protocol), for the few services whose clients run as recompiled SDK code. */
#pragma once
#include "host.h"

typedef struct IpcBuffer { u64 addr, size; } IpcBuffer;
typedef struct IpcObject IpcObject;
typedef struct Ipc Ipc;

/* A service object: handles commands sent to it. */
struct IpcObject {
    const char *name;
    void (*handle)(IpcObject *self, Ipc *ipc);
    void *state;
};

/* One request being served, and the response being built. */
struct Ipc {
    Cpu *cpu;
    u32 command;
    const u8 *in;   u32 in_size;              /* command arguments */
    IpcBuffer a[8], b[8], x[8], c[8];         /* send / receive / pointer / receive-list buffers */
    u32 na, nb, nx, nc;
    u32 copy_in[8], move_in[8], ncopy_in, nmove_in;
    u32 in_objects[8], nin_objects;           /* domain object ids passed in */

    u32 result;
    u8 out[0x200];  u32 out_size;
    u32 copy_out[8], move_out[8], ncopy_out, nmove_out;
    IpcObject *objects_out[8];  u32 nobjects_out;
    struct { u64 addr; u32 size, index; } x_out[8]; u32 nx_out;
};

u32 ipc_u32(Ipc *ipc, u32 offset);
u64 ipc_u64(Ipc *ipc, u32 offset);
void ipc_push(Ipc *ipc, const void *data, u32 size);
#define IPC_PUSH(ipc, value) do { __typeof__(value) v_ = (value); ipc_push(ipc, &v_, sizeof v_); } while (0)
void ipc_copy_handle(Ipc *ipc, u32 handle);
void ipc_object(Ipc *ipc, IpcObject *object);         /* a new service object handed to the client */
/* Output buffer i of the request, whichever kind the client used (B, or C with an X reply). */
u64 ipc_out_buffer(Ipc *ipc, u32 index, u64 *size);
u64 ipc_in_buffer(Ipc *ipc, u32 index, u64 *size);    /* input buffer i (A, or X) */
void ipc_wrote(Ipc *ipc, u32 index, u64 size);         /* how much of output buffer i was filled */
void ipc_unknown(Ipc *ipc, IpcObject *self);           /* log an unimplemented command, succeed */

IpcObject *ipc_new_object(const char *name, void (*handle)(IpcObject *, Ipc *), void *state);

/* Services: named objects reachable through the service manager. */
IpcObject *service_vi(const char *name);
IpcObject *service_audio(const char *name);
IpcObject *service_mii(const char *name);
IpcObject *service_mm(const char *name);
IpcObject *service_opus(const char *name);
IpcObject *service_generic(const char *name);   /* stub services for any engine's nnSdk IPC */
IpcObject *service_hid(const char *name);        /* nn::hid, including its shared memory */

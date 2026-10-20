/* State shared between the GPU device layer (hle_nv.c) and the GPU back end (gpu.c). */
#pragma once
#include "host.h"

#define NVMAP_MAX 65536
#define SYNCPT_MAX 192

typedef struct NvMap { u64 size, addr; u32 align, kind; } NvMap;          /* a memory handle */
typedef struct GpuMapping { u64 va, size, guest; } GpuMapping;           /* GPU VA -> guest memory */

extern NvMap gpu_maps[NVMAP_MAX];
extern u32 gpu_syncpt_min[SYNCPT_MAX], gpu_syncpt_max[SYNCPT_MAX];

u64 gpu_va_to_guest(u64 va);

/* Work submitted on a channel: `count` GPFIFO entries (each an address/length pair of a
 * command list in GPU virtual memory). Called with the device lock held. */
void gpu_submit(u32 channel, const u64 *entries, u32 count);

/* The multimedia engines (gpu_host1x.c): an nvmap handle's address in their address space,
 * and a command buffer for the engine of class cls ({handle, byte offset, word count}). */
u32 host1x_map(u32 handle);
void host1x_submit(u32 cls, u32 handle, u32 offset, u32 words);

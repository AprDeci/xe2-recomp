/* Presentation: frames leaving the display's buffer queue. */
#pragma once
#include "host.h"

/* Called at vsync with the graphic buffer description the driver registered for the slot
 * being shown (the flattened buffer, as received). */
void present_frame(const u8 *graphic_buffer, u32 size);
u64 present_frames(void);

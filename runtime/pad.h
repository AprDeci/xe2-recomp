/* Controllers for nn::hid (hle_pad.c): up to eight players' pads, read on an input thread.
 *
 * Slot n is npad id n (player n + 1). A slot's state is in the console's terms: NpadButton bits
 * A..DOWN (0-15) and sticks with +y up, inside a circle of radius 32767. */
#pragma once
#include "host.h"

enum { PAD_SLOTS = 8 };

typedef struct PadState {
    u32 connected;            /* a device drives this slot */
    u64 buttons;
    i32 lx, ly, rx, ry;
} PadState;

void pad_start(void);                                     /* once; the first hid call does it */
int pad_get(u32 slot, PadState *out);                     /* returns out->connected */
int pad_keyboard_slot(void);                              /* the keyboard's slot, or -1 */
int pad_background(void);                                 /* pads work while the window is not focused */
/* Rumble on one side (1 left, 2 right) of a slot's pad, amplitude 0..1. */
void pad_rumble(u32 slot, int side, float amplitude);
float pad_rumble_value(u32 slot, int side);               /* what was last sent */
int pad_describe(char *buf, int size);                    /* "1 DualSense, 2 Xbox" for the F2 panel */

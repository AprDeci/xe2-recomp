/* The debug overlay: frame rate, a frame time graph and statistics, drawn over each
 * presented frame (gpu_overlay.c). */
#pragma once
#include <d3d11.h>

/* Called once per presented frame, after the picture is in rtv (at x, y, w x h): notes the
 * frame's time and, if show, draws the overlay over the picture; detail (lines separated by
 * '\n', or NULL) goes under the graph. Leaves the pipeline state changed. */
void overlay_present(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x, unsigned y, unsigned w,
                     unsigned h, int show, const char *detail);

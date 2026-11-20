#ifndef RECOMP_DEKO_SWITCH_H
#define RECOMP_DEKO_SWITCH_H
#include <deko3d.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

DkDevice deko_switch_device(void);
DkQueue deko_switch_queue(void);
void deko_switch_window_size(uint32_t *w, uint32_t *h);

/* Hand off a GPU-rendered RGBA8 image (owned by gfx_deko3d.c) for swapchain presentation. */
void window_present_deko(const DkImage *img, uint32_t w, uint32_t h, DkFence *ready_fence);

/* ImGui overlay hooks (implemented in overlay_switch.cpp) */
int overlay_switch_init(DkDevice dev, DkQueue queue);
void overlay_switch_shutdown(void);
void overlay_switch_input(uint64_t buttons, int touch_down, float tx, float ty);
void overlay_switch_render(DkCmdBuf cmd, const DkImageView *target, uint32_t w, uint32_t h, double dt_s);

#ifdef __cplusplus
}
#endif
#endif

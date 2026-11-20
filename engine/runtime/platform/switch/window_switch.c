/*
 * Nintendo Switch host window, input, touch, six-axis motion, audio (audout), and deko3d swapchain
 * presentation loop. Replaces SDL3 window.c on TARGET=switch.
 */
#include <switch.h>
#include <deko3d.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "deko_switch.h"
#include "rt.h"
#include "hle.h"
#include "metrics.h"
#include "settings.h"
#include "overlay.h"
#include "platform.h"

enum {
	SELECT = 0x1, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
	LTRIG = 0x100, RTRIG = 0x200, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000,
	L3 = 0x2, R3 = 0x4, L1 = 0x400, R1 = 0x800, PAD_L2 = 0x10000, PAD_R2 = 0x20000
};

#define FB_NUM 2
#define PRESENT_CMDMEM_SIZE (128 * 1024)

static DkDevice g_dev;
static DkQueue g_queue;
static DkMemBlock g_fb_mem;
static DkImage g_fbs[FB_NUM];
static DkSwapchain g_swapchain;
static DkMemBlock g_cmd_mem;
static DkCmdBuf g_cmdbufs[FB_NUM];
static DkFence g_present_fence[FB_NUM];
static int g_present_busy[FB_NUM];
static uint32_t g_fb_w = 1280, g_fb_h = 720;
static int g_active;

/* Software fallback frame upload (when RECOMP_GFX=soft or before first GPU draw) */
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;
static uint8_t *soft_frame;
static uint32_t soft_w, soft_h, soft_new;
static DkMemBlock soft_stage_mem, soft_img_mem;
static DkImage soft_img;
static uint32_t soft_tex_w, soft_tex_h;

/* GPU-rendered frame handoff from gfx_deko3d.c */
static const DkImage *gpu_frame_img;
static uint32_t gpu_frame_w, gpu_frame_h, gpu_frame_new;
static DkFence gpu_frame_fence;
static int gpu_frame_has_fence;

/* Input state (written by main loop, read by game threads) */
static volatile uint32_t g_buttons;
static volatile uint8_t g_sticks[4] = {128, 128, 128, 128};
enum { ST_FINGERS = 6 };
static volatile int st_down[ST_FINGERS], st_x[ST_FINGERS], st_y[ST_FINGERS], st_id[ST_FINGERS];
static int last_touch_key[2];

static volatile float fps_cur, fps_mean;
static volatile uint64_t fps_n;

void motion_feed_accel(const float s[3], uint64_t t_ns);
void motion_feed_gyro(const float s[3], uint64_t t_ns);

DkDevice deko_switch_device(void) { return g_dev; }
DkQueue deko_switch_queue(void) { return g_queue; }
void deko_switch_window_size(uint32_t *w, uint32_t *h) { *w = g_fb_w; *h = g_fb_h; }

int window_active(void) { return g_active; }
int window_gamepad_connected(void) { return 1; }
int window_gyro_present(void) { return 1; }
int window_raw_look(void) { return 0; }
void window_set_raw_look(int on) { (void)on; }
int window_mouse_look(void) { return 0; }
float window_mouse_sens(void) { return 1.0f; }
int window_mouse_invert(void) { return 0; }
int window_mouse_delta(float *dx, float *dy) { *dx = *dy = 0; return 0; }
void window_tilt_tick(int kind) { (void)kind; }
void window_touch_gesture_hint(void) {}
void window_touch_twist_hint(void) {}
int window_get_aspect(void) { return settings_int("aspect", 0); }
void window_set_aspect(int mode) { settings_set_int("aspect", mode); settings_save(); }
int window_get_fullscreen(void) { return 1; }
void window_set_fullscreen(int on) { (void)on; }
int window_aspect_supported(void) { return 0; }
float window_aspect_widen(void) { return 1.0f; }
void window_pixel_size(uint32_t *w, uint32_t *h) { *w = g_fb_w; *h = g_fb_h; }
void window_fps_stats(float *cur, float *mean, uint64_t *n) {
	if (cur) *cur = fps_cur;
	if (mean) *mean = fps_mean;
	if (n) *n = fps_n;
}
void vkdriver_apply(void) {}
int window_last_touch_key(int port) { return last_touch_key[port & 1]; }
int window_audio_auto_delay_ms(void) { return 20; }
int window_controls_count(void) { return 0; }
void window_controls_row(int i, const char **action, const char **key, const char **pad) {
	(void)i; *action = *key = *pad = "";
}

void window_input(uint32_t *b, uint8_t s[4]) {
	*b = g_buttons & ~(L3 | R3 | PAD_L2 | PAD_R2);
	for (int i = 0; i < 4; i++) s[i] = g_sticks[i];
}

void window_input_pad2(uint32_t *b, uint8_t s[4]) {
	uint32_t v = g_buttons;
	window_input(b, s);
	*b = (*b & ~(LTRIG | RTRIG))
	   | (v & LTRIG ? L1 : 0) | (v & RTRIG ? R1 : 0)
	   | (v & PAD_L2 ? LTRIG : 0) | (v & PAD_R2 ? RTRIG : 0)
	   | (v & (L3 | R3));
}

int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max) {
	if (port != 0) return 0;
	int n = 0;
	for (int i = 0; i < ST_FINGERS && n < max; i++) {
		if (st_down[i]) {
			keys[n] = 20 + i;
			xs[n] = st_x[i];
			ys[n] = st_y[i];
			n++;
		}
	}
	if (n) last_touch_key[0] = keys[0];
	return n;
}

int window_touch(int *x, int *y) {
	for (int i = 0; i < ST_FINGERS; i++) {
		if (st_down[i]) { *x = st_x[i]; *y = st_y[i]; return 1; }
	}
	return 0;
}

/* ---- Audio output via libnx audout (48 kHz stereo S16) ---- */
#define AUD_BUFS 3
#define AUD_BUF_BYTES 0x4000
static int aud_ready;
static AudioOutBuffer aud_bufs[AUD_BUFS];
static uint8_t *aud_mem[AUD_BUFS];
static int aud_idx;
static pthread_mutex_t aud_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t aud_in_freq = 48000, aud_in_ch = 2;

int audio_open(uint32_t freq, uint32_t channels, uint32_t samples) {
	(void)samples;
	pthread_mutex_lock(&aud_lock);
	aud_in_freq = freq ? freq : 48000;
	aud_in_ch = channels ? channels : 2;
	if (!aud_ready && g_active) {
		if (R_SUCCEEDED(audoutInitialize()) && R_SUCCEEDED(audoutStartAudioOut())) {
			for (int i = 0; i < AUD_BUFS; i++) {
				aud_mem[i] = (uint8_t *)memalign(0x1000, AUD_BUF_BYTES);
				memset(aud_mem[i], 0, AUD_BUF_BYTES);
				aud_bufs[i].next = NULL;
				aud_bufs[i].buffer = aud_mem[i];
				aud_bufs[i].buffer_size = AUD_BUF_BYTES;
				aud_bufs[i].data_size = 0;
				aud_bufs[i].data_offset = 0;
			}
			aud_ready = 1;
		}
	}
	pthread_mutex_unlock(&aud_lock);
	return aud_ready ? 1 : 0;
}

void audio_output(int dev, const void *buf, uint32_t bytes) {
	if (!dev || !aud_ready || !bytes) return;
	if (overlay_game_paused()) {
		while (overlay_game_paused() && appletMainLoop()) svcSleepThread(5000000ULL);
		return;
	}
	pthread_mutex_lock(&aud_lock);
	/* Convert mono to stereo if needed; cap to buffer size */
	uint32_t out_bytes = bytes;
	AudioOutBuffer *b = &aud_bufs[aud_idx];
	/* If buffer is currently queued, wait for a free slot */
	bool busy = false;
	if (R_SUCCEEDED(audoutContainsAudioOutBuffer(b, &busy)) && busy) {
		AudioOutBuffer *rel = NULL;
		u32 cnt = 0;
		audoutWaitPlayFinish(&rel, &cnt, UINT64_MAX);
	}
	float vol = (float)settings_float("volume", 1.0);
	if (vol < 0.0f) vol = 0.0f;
	if (vol > 1.0f) vol = 1.0f;
	if (aud_in_ch == 1) {
		uint32_t ns = bytes / 2;
		out_bytes = ns * 4;
		if (out_bytes > AUD_BUF_BYTES) out_bytes = AUD_BUF_BYTES;
		const int16_t *src = (const int16_t *)buf;
		int16_t *dst = (int16_t *)b->buffer;
		uint32_t out_ns = out_bytes / 4;
		for (uint32_t i = 0; i < out_ns; i++) {
			int16_t s = (int16_t)((float)src[i] * vol);
			dst[2 * i] = s;
			dst[2 * i + 1] = s;
		}
	} else {
		if (out_bytes > AUD_BUF_BYTES) out_bytes = AUD_BUF_BYTES;
		if (vol >= 0.999f) {
			memcpy(b->buffer, buf, out_bytes);
		} else {
			const int16_t *src = (const int16_t *)buf;
			int16_t *dst = (int16_t *)b->buffer;
			uint32_t n = out_bytes / 2;
			for (uint32_t i = 0; i < n; i++) dst[i] = (int16_t)((float)src[i] * vol);
		}
	}
	b->data_size = out_bytes;
	audoutAppendAudioOutBuffer(b);
	aud_idx = (aud_idx + 1) % AUD_BUFS;
	pthread_mutex_unlock(&aud_lock);
}

/* ---- Frame presentation handoff ---- */
void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	if (!w || !h || w > 4096 || h > 4096 || !g_active) return;
	pthread_mutex_lock(&frame_lock);
	gpu_frame_new = 0;
	if (!soft_frame || soft_w != w || soft_h != h) {
		free(soft_frame);
		soft_frame = (uint8_t *)malloc(4 * (size_t)w * h);
		soft_w = w;
		soft_h = h;
	}
	for (uint32_t y = 0; y < h; y++)
		memcpy(soft_frame + 4 * (size_t)w * y, G(base + 4 * pitch * y), 4 * (size_t)w);
	soft_new = 1;
	pthread_cond_signal(&frame_cond);
	pthread_mutex_unlock(&frame_lock);
}

void window_present_deko(const DkImage *img, uint32_t w, uint32_t h, DkFence *ready_fence) {
	if (!img || !w || !h || !g_active) return;
	pthread_mutex_lock(&frame_lock);
	soft_new = 0;
	gpu_frame_img = img;
	gpu_frame_w = w;
	gpu_frame_h = h;
	if (ready_fence) { gpu_frame_fence = *ready_fence; gpu_frame_has_fence = 1; }
	else gpu_frame_has_fence = 0;
	gpu_frame_new = 1;
	pthread_cond_signal(&frame_cond);
	pthread_mutex_unlock(&frame_lock);
}

static void create_swapchain(uint32_t w, uint32_t h) {
	if (g_swapchain) {
		dkQueueWaitIdle(g_queue);
		dkSwapchainDestroy(g_swapchain);
		dkMemBlockDestroy(g_fb_mem);
	}
	g_fb_w = w;
	g_fb_h = h;
	nwindowSetDimensions(nwindowGetDefault(), w, h);

	DkImageLayoutMaker lm;
	dkImageLayoutMakerDefaults(&lm, g_dev);
	lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
	lm.format = DkImageFormat_RGBA8_Unorm;
	lm.dimensions[0] = w;
	lm.dimensions[1] = h;
	DkImageLayout lay;
	dkImageLayoutInitialize(&lay, &lm);
	uint32_t fbsz = dkImageLayoutGetSize(&lay);
	uint32_t align = dkImageLayoutGetAlignment(&lay);
	fbsz = (fbsz + align - 1) & ~(align - 1);

	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, FB_NUM * fbsz);
	mb.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	g_fb_mem = dkMemBlockCreate(&mb);

	DkImage const *ptrs[FB_NUM];
	for (int i = 0; i < FB_NUM; i++) {
		dkImageInitialize(&g_fbs[i], &lay, g_fb_mem, (uint32_t)i * fbsz);
		ptrs[i] = &g_fbs[i];
	}
	DkSwapchainMaker sm;
	dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), ptrs, FB_NUM);
	g_swapchain = dkSwapchainCreate(&sm);
}

static void ensure_soft_tex(uint32_t w, uint32_t h) {
	if (soft_img_mem && soft_tex_w == w && soft_tex_h == h) return;
	if (soft_img_mem) {
		dkQueueWaitIdle(g_queue);
		dkMemBlockDestroy(soft_img_mem);
		dkMemBlockDestroy(soft_stage_mem);
	}
	DkImageLayoutMaker lm;
	dkImageLayoutMakerDefaults(&lm, g_dev);
	lm.flags = DkImageFlags_Usage2DEngine;
	lm.format = DkImageFormat_RGBA8_Unorm;
	lm.dimensions[0] = w;
	lm.dimensions[1] = h;
	DkImageLayout lay;
	dkImageLayoutInitialize(&lay, &lm);
	uint32_t sz = dkImageLayoutGetSize(&lay);
	uint32_t align = dkImageLayoutGetAlignment(&lay);
	sz = (sz + align - 1) & ~(align - 1);

	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, sz);
	mb.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	soft_img_mem = dkMemBlockCreate(&mb);
	dkImageInitialize(&soft_img, &lay, soft_img_mem, 0);

	uint32_t stg_sz = (4 * w * h + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
	dkMemBlockMakerDefaults(&mb, g_dev, stg_sz);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	soft_stage_mem = dkMemBlockCreate(&mb);
	soft_tex_w = w;
	soft_tex_h = h;
}

static AppletHookCookie g_applet_hook;
static volatile int g_need_resize;
static void on_applet_hook(AppletHookType type, void *param) {
	(void)param;
	if (type == AppletHookType_OnOperationMode) g_need_resize = 1;
}

static uint8_t norm_axis(int32_t v) {
	int32_t s = 128 + (v * 127) / 32767;
	if (s < 0) s = 0;
	if (s > 255) s = 255;
	return (uint8_t)s;
}

static void poll_input(PadState *pad, HidSixAxisSensorHandle *sixaxis, int nsix) {
	padUpdate(pad);
	u64 k = padGetButtons(pad);
	u64 kdown = padGetButtonsDown(pad);
	int swap_ab = settings_int("switch_swap_ab", 0);

	/* Toggle overlay menu with Minus+Plus or L3+R3 */
	if (((k & HidNpadButton_Plus) && (k & HidNpadButton_Minus) && (kdown & (HidNpadButton_Plus | HidNpadButton_Minus))) ||
	    ((k & HidNpadButton_StickL) && (k & HidNpadButton_StickR) && (kdown & (HidNpadButton_StickL | HidNpadButton_StickR)))) {
		overlay_toggle();
	}

	/* Touch screen -> Vita 1920x1088 front panel (scaled from 1280x720) */
	HidTouchScreenState ts;
	memset(&ts, 0, sizeof ts);
	size_t nts = hidGetTouchScreenStates(&ts, 1);
	int td = 0;
	float tx = 0, ty = 0;
	if (nts > 0 && ts.count > 0 && !overlay_is_open()) {
		int c = ts.count > ST_FINGERS ? ST_FINGERS : ts.count;
		for (int i = 0; i < ST_FINGERS; i++) {
			if (i < c) {
				st_down[i] = 1;
				st_id[i] = (int)ts.touches[i].finger_id;
				st_x[i] = (int)((uint64_t)ts.touches[i].x * 1920u / 1280u);
				st_y[i] = (int)((uint64_t)ts.touches[i].y * 1088u / 720u);
			} else {
				st_down[i] = 0;
			}
		}
	} else {
		for (int i = 0; i < ST_FINGERS; i++) st_down[i] = 0;
		if (nts > 0 && ts.count > 0) {
			td = 1;
			tx = (float)ts.touches[0].x * ((float)g_fb_w / 1280.0f);
			ty = (float)ts.touches[0].y * ((float)g_fb_h / 720.0f);
		}
	}

	overlay_switch_input(k, td, tx, ty);

	if (overlay_is_open()) {
		g_buttons = 0;
		g_sticks[0] = g_sticks[1] = g_sticks[2] = g_sticks[3] = 128;
		return;
	}

	uint32_t vb = 0;
	if (swap_ab) {
		if (k & HidNpadButton_B) vb |= CROSS;
		if (k & HidNpadButton_A) vb |= CIRCLE;
		if (k & HidNpadButton_Y) vb |= SQUARE;
		if (k & HidNpadButton_X) vb |= TRIANGLE;
	} else {
		/* Match Vita physical face positions or standard Nintendo layout */
		if (k & HidNpadButton_B) vb |= CROSS;
		if (k & HidNpadButton_A) vb |= CIRCLE;
		if (k & HidNpadButton_Y) vb |= SQUARE;
		if (k & HidNpadButton_X) vb |= TRIANGLE;
	}
	if (k & HidNpadButton_Plus) vb |= START;
	if (k & HidNpadButton_Minus) vb |= SELECT;
	if (k & HidNpadButton_Up) vb |= UP;
	if (k & HidNpadButton_Down) vb |= DOWN;
	if (k & HidNpadButton_Left) vb |= LEFT;
	if (k & HidNpadButton_Right) vb |= RIGHT;
	if (k & HidNpadButton_L) vb |= LTRIG;
	if (k & HidNpadButton_R) vb |= RTRIG;
	if (k & HidNpadButton_ZL) vb |= PAD_L2;
	if (k & HidNpadButton_ZR) vb |= PAD_R2;
	if (k & HidNpadButton_StickL) vb |= L3;
	if (k & HidNpadButton_StickR) vb |= R3;
	g_buttons = vb;

	HidAnalogStickState ls = padGetStickPos(pad, 0);
	HidAnalogStickState rs = padGetStickPos(pad, 1);
	g_sticks[0] = norm_axis(ls.x);
	g_sticks[1] = norm_axis(-ls.y);
	g_sticks[2] = norm_axis(rs.x);
	g_sticks[3] = norm_axis(-rs.y);

	/* Feed first active six-axis sensor */
	for (int i = 0; i < nsix; i++) {
		HidSixAxisSensorState sa;
		if (hidGetSixAxisSensorStates(sixaxis[i], &sa, 1) > 0) {
			float acc[3] = { sa.acceleration.x, sa.acceleration.y, sa.acceleration.z };
			float gyr[3] = { sa.angular_velocity.x, sa.angular_velocity.y, sa.angular_velocity.z };
			uint64_t tns = armTicksToNs(armGetSystemTick());
			motion_feed_accel(acc, tns);
			motion_feed_gyro(gyr, tns);
			break;
		}
	}
}

typedef struct { void *(*fn)(void *); void *arg; } GameStart;
static void *game_entry_trampoline(void *p) {
	extern void switch_tune_thread(const char *, int, int);
	extern int waitstats_on(void);
	extern void waitstats_name(const char *);
	switch_tune_thread("vita-main", 64, 1);
	if (waitstats_on()) waitstats_name("vita-main");
	GameStart *g = (GameStart *)p;
	return g->fn(g->arg);
}

void window_run(void *(*fn)(void *), void *arg) {
	if (getenv("RECOMP_HEADLESS")) {
		fn(arg);
		return;
	}

	DkDeviceMaker dm;
	dkDeviceMakerDefaults(&dm);
	g_dev = dkDeviceCreate(&dm);
	DkQueueMaker qm;
	dkQueueMakerDefaults(&qm, g_dev);
	qm.flags = DkQueueFlags_Graphics;
	g_queue = dkQueueCreate(&qm);

	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, FB_NUM * PRESENT_CMDMEM_SIZE);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_cmd_mem = dkMemBlockCreate(&mb);
	DkCmdBufMaker cm;
	dkCmdBufMakerDefaults(&cm, g_dev);
	for (int i = 0; i < FB_NUM; i++) {
		g_cmdbufs[i] = dkCmdBufCreate(&cm);
		dkCmdBufAddMemory(g_cmdbufs[i], g_cmd_mem, (uint32_t)i * PRESENT_CMDMEM_SIZE, PRESENT_CMDMEM_SIZE);
	}

	uint32_t init_w = appletGetOperationMode() == AppletOperationMode_Console ? 1920 : 1280;
	uint32_t init_h = appletGetOperationMode() == AppletOperationMode_Console ? 1080 : 720;
	create_swapchain(init_w, init_h);
	appletHook(&g_applet_hook, on_applet_hook, NULL);

	overlay_switch_init(g_dev, g_queue);

	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);
	hidInitializeTouchScreen();

	HidSixAxisSensorHandle sixaxis[4];
	int nsix = 0;
	if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&sixaxis[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
		hidStartSixAxisSensor(sixaxis[0]);
		nsix = 1;
	}

	g_active = 1;
	GameStart gs = { fn, arg };
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
	pthread_create(&th, &attr, game_entry_trampoline, &gs);
	pthread_attr_destroy(&attr);
	pthread_detach(th);

	uint64_t prev_us = plat_now_us();
	uint64_t start_us = prev_us;
	int presented_once = 0;

	while (appletMainLoop() && !overlay_wants_quit()) {
		if (g_need_resize) {
			g_need_resize = 0;
			uint32_t nw = appletGetOperationMode() == AppletOperationMode_Console ? 1920 : 1280;
			uint32_t nh = appletGetOperationMode() == AppletOperationMode_Console ? 1080 : 720;
			if (nw != g_fb_w || nh != g_fb_h) create_swapchain(nw, nh);
		}

		poll_input(&pad, sixaxis, nsix);

		/* Fit 16:9 Vita aspect inside swapchain */
		uint32_t src_w = 0, src_h = 0;
		const DkImage *src_img = NULL;
		DkFence src_fence;
		int have_gpu = 0, have_soft = 0, src_has_fence = 0;
		int ov_open = overlay_is_open();

		pthread_mutex_lock(&frame_lock);
		if (!gpu_frame_new && !soft_new && !ov_open && presented_once) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 16000000L;
			if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
			pthread_cond_timedwait(&frame_cond, &frame_lock, &ts);
		}
		if (gpu_frame_img) {
			src_img = gpu_frame_img;
			src_w = gpu_frame_w;
			src_h = gpu_frame_h;
			src_fence = gpu_frame_fence;
			src_has_fence = gpu_frame_has_fence;
			have_gpu = gpu_frame_new;
			gpu_frame_new = 0;
		} else if (soft_frame) {
			ensure_soft_tex(soft_w, soft_h);
			if (soft_new) {
				memcpy(dkMemBlockGetCpuAddr(soft_stage_mem), soft_frame, 4 * (size_t)soft_w * soft_h);
				soft_new = 0;
				have_soft = 1;
			}
			src_img = &soft_img;
			src_w = soft_w;
			src_h = soft_h;
		}
		pthread_mutex_unlock(&frame_lock);

		if (!have_gpu && !have_soft && !ov_open && presented_once) continue;

		int slot = -1;
		DkFence acq_fence;
		dkSwapchainAcquireImage(g_swapchain, &slot, &acq_fence);
		if (g_present_busy[slot]) {
			dkFenceWait(&g_present_fence[slot], -1);
			g_present_busy[slot] = 0;
		}
		DkCmdBuf cmd = g_cmdbufs[slot];

		dkCmdBufClear(cmd);
		DkImageView dst_view;
		dkImageViewDefaults(&dst_view, &g_fbs[slot]);
		DkImageView const *dst_ptrs[] = { &dst_view };
		dkCmdBufBindRenderTargets(cmd, dst_ptrs, 1, NULL);
		static const float black[4] = {0, 0, 0, 1};
		dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, black);

		if (have_soft) {
			DkImageView sv;
			dkImageViewDefaults(&sv, &soft_img);
			DkCopyBuf cb = { dkMemBlockGetGpuAddr(soft_stage_mem), 0, 0 };
			DkImageRect rc = { 0, 0, 0, src_w, src_h, 1 };
			dkCmdBufCopyBufferToImage(cmd, &cb, &sv, &rc, 0);
			dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
		}
		if (src_img && src_w && src_h) {
			DkImageView sv;
			dkImageViewDefaults(&sv, src_img);
			DkImageRect sr = { 0, 0, 0, src_w, src_h, 1 };
			/* Preserve aspect ratio inside g_fb_w x g_fb_h */
			float sx = (float)g_fb_w / (float)src_w;
			float sy = (float)g_fb_h / (float)src_h;
			float sc = sx < sy ? sx : sy;
			uint32_t dw = (uint32_t)((float)src_w * sc);
			uint32_t dh = (uint32_t)((float)src_h * sc);
			DkImageRect dr = { (g_fb_w - dw) / 2, (g_fb_h - dh) / 2, 0, dw, dh, 1 };
			dkCmdBufBlitImage(cmd, &sv, &sr, &dst_view, &dr, DkBlitFlag_FilterLinear | DkBlitFlag_ModeBlit, 0);
			dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
		}

		uint64_t now = plat_now_us();
		double dt_s = (double)(now - prev_us) / 1e6;
		prev_us = now;
		if (dt_s > 0.0001) {
			fps_cur = (float)(1.0 / dt_s);
			fps_n++;
			double elapsed = (double)(now - start_us) / 1e6;
			if (elapsed > 0.01) fps_mean = (float)((double)fps_n / elapsed);
		}

		overlay_switch_render(cmd, &dst_view, g_fb_w, g_fb_h, dt_s);

		DkCmdList list = dkCmdBufFinishList(cmd);
		dkQueueWaitFence(g_queue, &acq_fence);
		if (have_gpu && src_has_fence) dkQueueWaitFence(g_queue, &src_fence);
		dkQueueSubmitCommands(g_queue, list);
		dkQueueSignalFence(g_queue, &g_present_fence[slot], false);
		g_present_busy[slot] = 1;
		dkQueuePresentImage(g_queue, g_swapchain, slot);
		presented_once = 1;
		metrics_window_frame(plat_now_us() - now);
	}

	g_active = 0;
	dkQueueWaitIdle(g_queue);
	overlay_switch_shutdown();
	appletUnhook(&g_applet_hook);
	if (aud_ready) {
		audoutStopAudioOut();
		audoutExit();
	}
	if (soft_img_mem) {
		dkMemBlockDestroy(soft_img_mem);
		dkMemBlockDestroy(soft_stage_mem);
	}
	for (int i = 0; i < FB_NUM; i++) dkCmdBufDestroy(g_cmdbufs[i]);
	dkMemBlockDestroy(g_cmd_mem);
	dkSwapchainDestroy(g_swapchain);
	dkMemBlockDestroy(g_fb_mem);
	dkQueueDestroy(g_queue);
	dkDeviceDestroy(g_dev);
}

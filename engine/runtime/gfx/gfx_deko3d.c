/*
 * Hardware GPU backend for Nintendo Switch using deko3d (libdeko3d).
 * Implements the gfx_gpu_* backend interface declared in gfx.h / gfx_dispatch.c,
 * loading precompiled .dksh shaders (USSE -> SPIR-V -> GLSL 460 -> uam) and falling back
 * to gfx_soft for any uncompiled shader or unsupported instruction.
 */
#include <switch.h>
#include <deko3d.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ARM_FEATURE_CRC32
#include <arm_acle.h>
#endif
#include "rt.h"
#include "gfx.h"
#include "usse.h"
#include "usse_spirv.h"
#include "deko_switch.h"
#include "platform.h"
#include "settings.h"
#include "gxp_precompile.h"

/* ---- Helpers ---- */
static uint64_t us_now(void) { return plat_now_us(); }
static uint64_t hash_bytes(const void *p, size_t n) {
	const uint8_t *b = (const uint8_t *)p;
	size_t i = 0;
#ifdef __ARM_FEATURE_CRC32
	uint32_t c0 = 0xFFFFFFFFu, c1 = 0x12345678u, c2 = 0x9ABCDEF0u, c3 = 0x87654321u;
	for (; i + 32 <= n; i += 32) {
		uint64_t w0, w1, w2, w3;
		memcpy(&w0, b + i, 8);
		memcpy(&w1, b + i + 8, 8);
		memcpy(&w2, b + i + 16, 8);
		memcpy(&w3, b + i + 24, 8);
		c0 = __crc32cd(c0, w0);
		c1 = __crc32cd(c1, w1);
		c2 = __crc32cd(c2, w2);
		c3 = __crc32cd(c3, w3);
	}
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, b + i, 8);
		c0 = __crc32cd(c0, w);
	}
	for (; i < n; i++) c0 = __crc32cb(c0, b[i]);
	return ((uint64_t)(c0 ^ c2) << 32) | (uint64_t)(c1 ^ c3);
#else
	uint64_t h0 = 1469598103934665603ULL, h1 = 1099511628211ULL;
	for (; i + 16 <= n; i += 16) {
		uint64_t w0, w1;
		memcpy(&w0, b + i, 8);
		memcpy(&w1, b + i + 8, 8);
		h0 = (h0 ^ w0) * 1099511628211ULL;
		h1 = (h1 ^ w1) * 1099511628211ULL;
	}
	uint64_t h = h0 ^ (h1 * 0x9e3779b97f4a7c15ULL);
	for (; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
#endif
}
static uint32_t hash32_bytes(const void *p, size_t n) {
	const uint8_t *b = (const uint8_t *)p;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* ---- Device, queue, command buffer & memory pools ---- */
#define CODE_POOL_SZ     (16u << 20)
#define VRING_WORDS      (8u << 20)   /* 32 MB vertex/PA/raw stream buffer */
#define IRING_WORDS      (1u << 20)   /* 4 MB index buffer */
#define WRING_WORDS      (1u << 20)   /* 4 MB LDST window buffer */
#define URING_BYTES      (8u << 20)   /* 8 MB uniform buffer ring (256B aligned) */
#define QBUF_BYTES       4096u
#define UPLOAD_POOL_SZ   (16u << 20)
#define READBACK_POOL_SZ (8u << 20)
#define FRAME_SLOTS      3
#define CMD_SLICE_SZ     (2u << 20)
#define MAX_IMG_DESC     2048
#define MAX_SMP_DESC     64

static int g_inited, g_no_gpu;
static DkDevice g_dev;
static DkQueue g_queue;
static DkMemBlock g_cmd_mem;
static DkCmdBuf g_cmds[FRAME_SLOTS];
static DkFence g_slot_fence[FRAME_SLOTS];
static uint8_t g_slot_busy[FRAME_SLOTS];
static int g_cur_slot;

static inline DkCmdBuf cur_cmd(void) { return g_cmds[g_cur_slot]; }

static DkMemBlock g_code_mem;
static uint32_t g_code_off;

static DkMemBlock g_ring_mem;
static uint8_t *g_ring_cpu;
static DkGpuAddr g_ring_gpu;
static uint32_t g_voff, g_ioff, g_woff, g_uoff, g_qoff;
static uint32_t g_vpos, g_ipos, g_wpos, g_upos;

static DkMemBlock g_desc_mem;
static DkImageDescriptor *g_img_descs;
static DkSamplerDescriptor *g_smp_descs;
static DkGpuAddr g_img_desc_gpu, g_smp_desc_gpu;
static int g_next_img_desc = 1;   /* 0 reserved for blank/null if needed */
static int g_next_smp_desc = 0;
static int g_desc_dirty = 0;

static DkMemBlock g_upload_mem, g_readback_mem;
static uint8_t *g_upload_cpu, *g_readback_cpu;
static DkGpuAddr g_upload_gpu, g_readback_gpu;
static uint32_t g_upload_pos;

/* Built-in utility shaders */
static DkShader g_sh_full_vs, g_sh_view_fs, g_sh_pre_z_fs, g_sh_pre_s_fs;
static int g_builtin_ok;

/* Present triple-buffer images */
#define PRES_NUM 3
static DkImage g_pres_img[PRES_NUM];
static DkMemBlock g_pres_mem[PRES_NUM];
static uint32_t g_pres_w[PRES_NUM], g_pres_h[PRES_NUM];
static DkFence g_pres_fence[PRES_NUM];
static uint8_t g_pres_busy[PRES_NUM];
static int g_pres_idx;

typedef struct {
	int valid, u_valid;
	const DkShader *vs, *fs;
	DkRasterizerState rs;
	float db_units, db_factor;
	DkColorState cs;
	DkBlendState bs;
	DkColorWriteState cws;
	DkDepthStencilState dss;
	uint8_t st_wmask, st_ref, st_cmask;
	DkViewport vp;
	DkScissor sc;
	UsseDrawUniforms last_vu, last_fu;
	uint32_t last_vu_off, last_fu_off;
} DekoStateCache;
static DekoStateCache g_st;

static int rscale_cur = 1, rscale_want = 1;
static uint32_t n_gpu_draws, n_fb_draws;

/* ---- Render scale & capability queries ---- */
int gfx_gpu_render_scale_max(void) { return 2; }
int gfx_gpu_render_scale(void) { return rscale_cur; }
int gfx_gpu_render_scale_wanted(void) { return rscale_want; }
void gfx_gpu_render_scale_request(int n) {
	if (n < 1) n = 1;
	if (n > gfx_gpu_render_scale_max()) n = gfx_gpu_render_scale_max();
	rscale_want = n;
}
void gfx_gpu_render_scale_cycle(void) {
	gfx_gpu_render_scale_request(rscale_want >= gfx_gpu_render_scale_max() ? 1 : rscale_want + 1);
}
int gfx_gpu_hud_supported(void) { return 0; }
int gfx_gpu_ssao_supported(void) { return 0; }
int gfx_gpu_fsr_supported(void) { return 0; }
int gfx_gpu_target_scale_supported(void) { return 0; }
int gfx_gpu_fsr_active(void) { return 0; }
void gfx_gpu_fsr_toggle(void) {}
int gfx_gpu_mv_on(void) { return 0; }
void gfx_gpu_frame_queued(void) {}
void gfx_gpu_shader_prep_tick(void) {}
int gfx_gpu_shader_prep(int *done, int *total, double *eta_s) {
	if (done) *done = 0; if (total) *total = 0; if (eta_s) *eta_s = 0;
	return 0;
}
void gfx_gpu_shader_prep_skip(void) {}
void gfx_gpu_builds_stop(void) {}
void gfx_gpu_builds_wait(void) {}
void gfx_gpu_program_registered(uint32_t gxp) { (void)gxp; }
void gfx_gpu_quit(void) {}
void gfx_gpu_build_status(double *wait_ms, double *total_ms, int *inflight) {
	if (wait_ms) *wait_ms = 0; if (total_ms) *total_ms = 0; if (inflight) *inflight = 0;
}
void gfx_gpu_fence_stats_finish(void) {}
void gfx_gpu_history_save(int mark) { (void)mark; }
void gfx_gpu_hud_note_present(uint32_t base) { (void)base; }
void gfx_gpu_get_counts(uint32_t *gpu_draws, uint32_t *fallback_draws) {
	if (gpu_draws) *gpu_draws = __atomic_load_n(&n_gpu_draws, __ATOMIC_RELAXED);
	if (fallback_draws) *fallback_draws = __atomic_load_n(&n_fb_draws, __ATOMIC_RELAXED);
}
static GfxGpuFrameCosts g_costs, g_costs_last;
void gfx_gpu_last_frame_costs(GfxGpuFrameCosts *out) { *out = g_costs_last; }
void *gfx_gpu_create_device(void) { return deko_switch_device(); }
int gfx_gpu_readbacks_due(void **fences, int max) { (void)fences; (void)max; return 0; }
void gfx_gpu_readbacks_wait(void **fences, int n) { (void)fences; (void)n; }
int gfx_gpu_vis_defer(const uint32_t *addr, const uint32_t *val, int n) { (void)addr; (void)val; (void)n; return 0; }
int gfx_gpu_vis_deferred(void) { return 0; }
void gfx_gpu_vis_flush(int wait) { (void)wait; }
int gfx_gpu_scene_open_on(uint32_t base) { (void)base; return 0; }
int gfx_gpu_transfer_downscale(uint32_t src, uint32_t sw, uint32_t sh, uint32_t sstride, uint32_t dst, uint32_t dstride) {
	(void)src; (void)sw; (void)sh; (void)sstride; (void)dst; (void)dstride;
	return 0;
}

static int load_dksh_file(DkShader *sh, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0 || g_code_off + (uint32_t)sz > CODE_POOL_SZ - DK_SHADER_CODE_UNUSABLE_SIZE) { fclose(f); return 0; }
	void *tmp = malloc((size_t)sz);
	if (!tmp) { fclose(f); return 0; }
	size_t nr = fread(tmp, 1, (size_t)sz, f);
	fclose(f);
	if (nr != (size_t)sz) { free(tmp); return 0; }
	uint32_t off = g_code_off;
	memcpy((uint8_t *)dkMemBlockGetCpuAddr(g_code_mem) + off, tmp, (size_t)sz);
	free(tmp);
	g_code_off = align_up(off + (uint32_t)sz, DK_SHADER_CODE_ALIGNMENT);
	DkShaderMaker sm;
	dkShaderMakerDefaults(&sm, g_code_mem, off);
	dkShaderInitialize(sh, &sm);
	return dkShaderIsValid(sh) ? 1 : 0;
}

#define PRE_DKSH_HT 2048
typedef struct {
	char key[96];
	DkShader shader;
	uint8_t used;
} PreDkshEntry;
static PreDkshEntry g_pre_dksh[PRE_DKSH_HT];
static int g_n_pre_dksh;

static int cmp_str_ptr(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void preload_dksh_dir(const char *dir_path) {
	DIR *d = opendir(dir_path);
	if (!d) return;
	char *names[1024];
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < 1024) {
		size_t len = strlen(de->d_name);
		if (len <= 5 || strcmp(de->d_name + len - 5, ".dksh") != 0) continue;
		if (!strcmp(de->d_name, "fullscreen_vsh.dksh") || !strcmp(de->d_name, "view_fsh.dksh") ||
		    !strcmp(de->d_name, "preload_depth_fsh.dksh") || !strcmp(de->d_name, "preload_stencil_fsh.dksh"))
			continue;
		names[n++] = strdup(de->d_name);
	}
	closedir(d);
	if (!n) return;
	qsort(names, (size_t)n, sizeof(char *), cmp_str_ptr);
	int loaded = 0;
	for (int i = 0; i < n; i++) {
		char stem[96], path[1024];
		size_t slen = strlen(names[i]) - 5;
		if (slen >= sizeof stem) { free(names[i]); continue; }
		memcpy(stem, names[i], slen);
		stem[slen] = 0;
		snprintf(path, sizeof path, "%s/%s", dir_path, names[i]);
		free(names[i]);
		uint32_t h = hash32_bytes(stem, slen) & (PRE_DKSH_HT - 1u);
		while (g_pre_dksh[h].used && strcmp(g_pre_dksh[h].key, stem) != 0)
			h = (h + 1u) & (PRE_DKSH_HT - 1u);
		if (g_pre_dksh[h].used) continue;
		if (load_dksh_file(&g_pre_dksh[h].shader, path)) {
			memcpy(g_pre_dksh[h].key, stem, slen + 1);
			g_pre_dksh[h].used = 1;
			loaded++;
			g_n_pre_dksh++;
		}
	}
	if (loaded) fprintf(stderr, "deko3d: preloaded %d shaders from %s (%u KB code)\n", loaded, dir_path, g_code_off >> 10);
}

static int find_preloaded_dksh(const char *key, DkShader *out) {
	if (!g_n_pre_dksh) return 0;
	uint32_t h = hash32_bytes(key, strlen(key)) & (PRE_DKSH_HT - 1u);
	while (g_pre_dksh[h].used) {
		if (strcmp(g_pre_dksh[h].key, key) == 0) {
			*out = g_pre_dksh[h].shader;
			return 1;
		}
		h = (h + 1u) & (PRE_DKSH_HT - 1u);
	}
	return 0;
}

static int ensure_gpu(void) {
	if (g_inited) return !g_no_gpu;
	g_inited = 1;
	g_dev = deko_switch_device();
	if (!g_dev) { g_no_gpu = 1; return 0; }
	DkQueueMaker qm;
	dkQueueMakerDefaults(&qm, g_dev);
	qm.flags = DkQueueFlags_Graphics;
	g_queue = dkQueueCreate(&qm);
	if (!g_queue) { g_no_gpu = 1; return 0; }

	rscale_want = getenv("RECOMP_RENDER_SCALE") ? atoi(getenv("RECOMP_RENDER_SCALE")) : settings_int("render_scale", 1);
	if (rscale_want < 1) rscale_want = 1;
	if (rscale_want > gfx_gpu_render_scale_max()) rscale_want = gfx_gpu_render_scale_max();
	rscale_cur = rscale_want;

	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, FRAME_SLOTS * CMD_SLICE_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_cmd_mem = dkMemBlockCreate(&mb);
	DkCmdBufMaker cm;
	dkCmdBufMakerDefaults(&cm, g_dev);
	for (int i = 0; i < FRAME_SLOTS; i++) {
		g_cmds[i] = dkCmdBufCreate(&cm);
		dkCmdBufAddMemory(g_cmds[i], g_cmd_mem, (uint32_t)i * CMD_SLICE_SZ, CMD_SLICE_SZ);
	}

	dkMemBlockMakerDefaults(&mb, g_dev, CODE_POOL_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
	g_code_mem = dkMemBlockCreate(&mb);

	uint32_t vbytes = VRING_WORDS * 4u, ibytes = IRING_WORDS * 4u, wbytes = WRING_WORDS * 4u;
	uint32_t ring_sz = align_up(vbytes + ibytes + wbytes + URING_BYTES + QBUF_BYTES, DK_MEMBLOCK_ALIGNMENT);
	dkMemBlockMakerDefaults(&mb, g_dev, ring_sz);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_ring_mem = dkMemBlockCreate(&mb);
	g_ring_cpu = (uint8_t *)dkMemBlockGetCpuAddr(g_ring_mem);
	g_ring_gpu = dkMemBlockGetGpuAddr(g_ring_mem);
	g_voff = 0;
	g_ioff = g_voff + vbytes;
	g_woff = g_ioff + ibytes;
	g_uoff = g_woff + wbytes;
	g_qoff = g_uoff + URING_BYTES;

	uint32_t idesc_sz = align_up(MAX_IMG_DESC * DK_IMAGE_DESCRIPTOR_ALIGNMENT, 256);
	uint32_t sdesc_sz = align_up(MAX_SMP_DESC * DK_SAMPLER_DESCRIPTOR_ALIGNMENT, 256);
	dkMemBlockMakerDefaults(&mb, g_dev, align_up(idesc_sz + sdesc_sz, DK_MEMBLOCK_ALIGNMENT));
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_desc_mem = dkMemBlockCreate(&mb);
	uint8_t *dc = (uint8_t *)dkMemBlockGetCpuAddr(g_desc_mem);
	DkGpuAddr dg = dkMemBlockGetGpuAddr(g_desc_mem);
	g_img_descs = (DkImageDescriptor *)dc;
	g_smp_descs = (DkSamplerDescriptor *)(dc + idesc_sz);
	g_img_desc_gpu = dg;
	g_smp_desc_gpu = dg + idesc_sz;

	dkMemBlockMakerDefaults(&mb, g_dev, UPLOAD_POOL_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_upload_mem = dkMemBlockCreate(&mb);
	g_upload_cpu = (uint8_t *)dkMemBlockGetCpuAddr(g_upload_mem);
	g_upload_gpu = dkMemBlockGetGpuAddr(g_upload_mem);

	dkMemBlockMakerDefaults(&mb, g_dev, READBACK_POOL_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached;
	g_readback_mem = dkMemBlockCreate(&mb);
	g_readback_cpu = (uint8_t *)dkMemBlockGetCpuAddr(g_readback_mem);
	g_readback_gpu = dkMemBlockGetGpuAddr(g_readback_mem);

	g_builtin_ok = load_dksh_file(&g_sh_full_vs, "romfs:/shaders/fullscreen_vsh.dksh")
	            && load_dksh_file(&g_sh_view_fs, "romfs:/shaders/view_fsh.dksh")
	            && load_dksh_file(&g_sh_pre_z_fs, "romfs:/shaders/preload_depth_fsh.dksh")
	            && load_dksh_file(&g_sh_pre_s_fs, "romfs:/shaders/preload_stencil_fsh.dksh");
	if (!g_builtin_ok) fprintf(stderr, "deko3d: warning: built-in romfs shaders failed to load\n");

	/* Ensure translator defaults match our PC shader dumps */
	usse_spirv_set_fetch_mode(1);
	usse_spirv_set_canonical_varyings(1);
	gxp_precompile_maybe();   /* RECOMP_PRECOMPILE_SPV=<dir>: every program in the game's files dumped once, then boot as usual */

	const char *dksh_dir = getenv("RECOMP_DKSH_DIR"), *home = getenv("RECOMP_HOME");
	char sdir[1024];
	if (dksh_dir && *dksh_dir) preload_dksh_dir(dksh_dir);
	else {
		snprintf(sdir, sizeof sdir, "%s/shaders", home ? home : ".");
		preload_dksh_dir(sdir);
	}
	preload_dksh_dir("romfs:/shaders");
	return 1;
}

/* ---- Render targets & depth/stencil surfaces ---- */
enum { TK_U8 = 0, TK_F16 = 1, TK_F32 = 2, TK_U8S = 3 };
static int target_kind(uint32_t fmt) {
	switch (fmt & 0xF1800000u) {
	case 0x41000000u:
	case 0x01000000u: case 0x00800000u: case 0xF0000000u: case 0x21000000u: case 0x31000000u: return TK_F16;
	case 0x10800000u: case 0x11000000u: return TK_F32;
	default: return GFX_TGT_GAMMA(fmt) && (fmt & 0xF1800000u) == 0 ? TK_U8S : TK_U8;
	}
}
static DkImageFormat kind_format(int k) {
	switch (k) {
	case TK_F32: return DkImageFormat_RGBA32_Float;
	case TK_F16: return DkImageFormat_RGBA16_Float;
	case TK_U8S: return DkImageFormat_RGBA8_Unorm_sRGB;
	default:     return DkImageFormat_RGBA8_Unorm;
	}
}

typedef struct {
	uint32_t addr, format, width, height, stride, guest_bytes;
	uint8_t type, ms_x, ms_y, scale_mode;
	int kind, sx, sy, desc_id, dirty, gpu_newer;
	uint64_t gen, bar_gen;
	DkImage img, fetch_img;
	DkMemBlock mem, fetch_mem;
	int fetch_desc_id;
} TargetEntry;

typedef struct {
	TargetEntry *te;
	uint32_t x, y, w, h;
	int kind, sx, sy, desc_id;
	uint64_t copy_gen;
	DkImage img;
	DkMemBlock mem;
} ViewEntry;

static uint32_t target_guest_bytes(const TargetEntry *te) {
	GfxTarget gt = { .format = te->format };
	uint32_t bpp = (uint32_t)gfx_target_bpp(&gt);
	return (bpp ? bpp : 4u) * te->stride * te->height;
}

typedef struct {
	uint32_t addr, ds_addr, ds_stencil, ds_format, width, height, stride;
	uint8_t type;
	int sx, sy;
	DkImage img;
	DkMemBlock mem;
} DepthEntry;

#define MAX_TARGETS 128
#define MAX_DEPTHS  64
#define MAX_VIEWS   64
static TargetEntry g_targets[MAX_TARGETS];
static int g_ntargets;
static DepthEntry g_depths[MAX_DEPTHS];
static int g_ndepths;
static ViewEntry g_views[MAX_VIEWS];
static int g_nviews;

static GfxTarget g_scene_tgt;
static TargetEntry *g_scene_te;
static DepthEntry *g_scene_de;
static int g_scene_first_draw, g_scene_need_bind, g_scene_sx = 1, g_scene_sy = 1;
static int g_batch_open, g_need_tex_barrier;
static ViewEntry *g_copy_views[8];
static int g_ncopy;
static void open_batch(void) {
	if (g_batch_open) return;
	if (g_slot_busy[g_cur_slot]) {
		uint64_t tw0 = us_now();
		dkFenceWait(&g_slot_fence[g_cur_slot], -1);
		g_slot_busy[g_cur_slot] = 0;
		g_costs.ring_waits++;
		g_costs.t_ring_wait += us_now() - tw0;
	}
	DkCmdBuf cmd = cur_cmd();
	dkCmdBufClear(cmd);
	dkCmdBufBindImageDescriptorSet(cmd, g_img_desc_gpu, MAX_IMG_DESC);
	dkCmdBufBindSamplerDescriptorSet(cmd, g_smp_desc_gpu, MAX_SMP_DESC);
	g_batch_open = 1;
	g_scene_need_bind = 1;
	g_st.valid = 0;
	g_st.u_valid = 0;
}

static void wait_all_slots(void);
static void submit_batch(int wait_idle, DkFence *out_fence) {
	if (!g_batch_open) {
		if (wait_idle) wait_all_slots();
		return;
	}
	g_batch_open = 0;
	uint64_t tw0 = us_now();
	int s = g_cur_slot;
	DkCmdList list = dkCmdBufFinishList(g_cmds[s]);
	dkQueueSubmitCommands(g_queue, list);
	dkQueueSignalFence(g_queue, &g_slot_fence[s], (wait_idle || out_fence) ? true : false);
	dkQueueFlush(g_queue);
	if (out_fence) *out_fence = g_slot_fence[s];
	g_slot_busy[s] = 1;
	g_cur_slot = (s + 1) % FRAME_SLOTS;
	if (wait_idle) {
		dkFenceWait(&g_slot_fence[s], -1);
		g_slot_busy[s] = 0;
	}
	g_costs.flushes++;
	g_costs.t_tsync += us_now() - tw0;
}

static void wait_all_slots(void) {
	if (g_batch_open) submit_batch(0, NULL);
	uint64_t tw0 = us_now();
	int waited = 0;
	for (int i = 0; i < FRAME_SLOTS; i++) {
		if (g_slot_busy[i]) {
			dkFenceWait(&g_slot_fence[i], -1);
			g_slot_busy[i] = 0;
			waited = 1;
		}
	}
	if (waited) g_costs.t_tsync += us_now() - tw0;
}

static void flush_batch(void) { submit_batch(0, NULL); }

static void *upload_alloc(size_t bytes, DkGpuAddr *out_gpu) {
	if (bytes > UPLOAD_POOL_SZ) return NULL;
	uint32_t off = align_up(g_upload_pos, 256u);
	if (off + bytes > UPLOAD_POOL_SZ) {
		wait_all_slots();
		g_upload_pos = 0;
		off = 0;
	}
	g_upload_pos = off + (uint32_t)bytes;
	*out_gpu = g_upload_gpu + off;
	return g_upload_cpu + off;
}

static void alloc_image_block(uint32_t w, uint32_t h, DkImageFormat fmt, uint32_t flags, DkImageType type, uint32_t depth_or_layers, uint32_t mips, DkImage *out_img, DkMemBlock *out_mem) {
	DkImageLayoutMaker lm;
	dkImageLayoutMakerDefaults(&lm, g_dev);
	lm.type = type;
	lm.flags = flags;
	lm.format = fmt;
	lm.dimensions[0] = w;
	lm.dimensions[1] = h;
	lm.dimensions[2] = depth_or_layers;
	lm.mipLevels = mips ? mips : 1;
	DkImageLayout lay;
	dkImageLayoutInitialize(&lay, &lm);
	uint32_t sz = align_up((uint32_t)dkImageLayoutGetSize(&lay), (uint32_t)dkImageLayoutGetAlignment(&lay));
	sz = align_up(sz, DK_MEMBLOCK_ALIGNMENT);
	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, sz);
	mb.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	*out_mem = dkMemBlockCreate(&mb);
	dkImageInitialize(out_img, &lay, *out_mem, 0);
}

static int write_img_desc(int id, const DkImage *img, DkImageType type_override, uint32_t layers, uint32_t mips) {
	if (id <= 0 || id >= MAX_IMG_DESC) return 0;
	DkImageView iv;
	dkImageViewDefaults(&iv, img);
	if (type_override != DkImageType_None) iv.type = type_override;
	if (layers) iv.layerCount = (uint16_t)layers;
	if (mips) iv.mipLevelCount = (uint8_t)mips;
	dkImageDescriptorInitialize(&g_img_descs[id], &iv, false, false);
	g_desc_dirty = 1;
	return id;
}

static int register_img_desc(const DkImage *img, DkImageType type_override, uint32_t layers, uint32_t mips) {
	if (g_next_img_desc >= MAX_IMG_DESC) return 0;
	return write_img_desc(g_next_img_desc++, img, type_override, layers, mips);
}

static float *get_tgt_scratch(size_t bytes) {
	static float *buf;
	static size_t cap;
	if (bytes > cap) {
		float *nb = (float *)realloc(buf, bytes);
		if (!nb) return NULL;
		buf = nb;
		cap = bytes;
	}
	return buf;
}

static void target_upload_from_guest(TargetEntry *te) {
	if (!te->addr || !te->stride || !te->height) return;
	uint32_t npix = te->stride * te->height;
	size_t bytes = (size_t)npix * (te->kind == TK_F32 ? 16 : te->kind == TK_F16 ? 8 : 4);
	DkGpuAddr dst_gpu = 0;
	uint8_t *dst = (uint8_t *)upload_alloc(bytes, &dst_gpu);
	if (!dst) return;
	GfxTarget gt = { .data = te->addr, .format = te->format, .width = te->width, .height = te->height, .stride = te->stride, .type = te->type };
	if (te->kind == TK_F32 || te->kind == TK_F16) {
		float *tmp = get_tgt_scratch((size_t)npix * 16);
		if (!tmp) return;
		gfx_decode_target_f32(&gt, tmp);
		if (te->kind == TK_F32) {
			memcpy(dst, tmp, (size_t)npix * 16);
		} else {
			/* Convert float32 -> IEEE float16 */
			uint16_t *h = (uint16_t *)dst;
			for (size_t i = 0; i < (size_t)npix * 4; i++) {
				uint32_t f;
				memcpy(&f, &tmp[i], 4);
				uint32_t s = (f >> 16) & 0x8000u;
				int32_t e = (int32_t)((f >> 23) & 0xff) - 127 + 15;
				uint32_t m = f & 0x7fffffu;
				h[i] = (uint16_t)(e <= 0 ? s : e >= 31 ? (s | 0x7c00u) : (s | ((uint32_t)e << 10) | (m >> 13)));
			}
		}
	} else {
		gfx_decode_target_rgba8(&gt, dst);
	}
	DkImageView iv;
	dkImageViewDefaults(&iv, &te->img);
	DkCopyBuf cb = { dst_gpu, 0, 0 };
	DkImageRect rc = { 0, 0, 0, te->stride, te->height, 1 };
	open_batch();
	dkCmdBufBarrier(cur_cmd(), DkBarrier_Primitives, 0);
	dkCmdBufCopyBufferToImage(cur_cmd(), &cb, &iv, &rc, 0);
	dkCmdBufBarrier(cur_cmd(), DkBarrier_Primitives, DkInvalidateFlags_Image);
	g_scene_need_bind = 1;
	g_st.valid = 0;
}

static void target_download_to_guest(TargetEntry *te) {
	if (!te->gpu_newer || !te->addr || !te->stride || !te->height) return;
	uint64_t tdl0 = us_now();
	uint32_t npix = te->stride * te->height;
	size_t bytes = (size_t)npix * (te->kind == TK_F32 ? 16 : te->kind == TK_F16 ? 8 : 4);
	if (bytes > READBACK_POOL_SZ) return;

	open_batch();
	dkCmdBufBarrier(cur_cmd(), DkBarrier_Fragments, DkInvalidateFlags_L2Cache);
	DkImageView iv;
	dkImageViewDefaults(&iv, &te->img);
	DkCopyBuf cb = { g_readback_gpu, 0, 0 };
	DkImageRect rc = { 0, 0, 0, te->stride, te->height, 1 };
	dkCmdBufCopyImageToBuffer(cur_cmd(), &iv, &rc, &cb, 0);
	submit_batch(1, NULL);

	GfxTarget gt = { .data = te->addr, .format = te->format, .width = te->width, .height = te->height, .stride = te->stride, .type = te->type };
	if (te->kind == TK_U8 || te->kind == TK_U8S) {
		gfx_encode_target_rgba8(&gt, g_readback_cpu);
	} else if (te->kind == TK_F32) {
		gfx_encode_target_f32(&gt, (const float *)g_readback_cpu);
	} else {
		float *tmp = get_tgt_scratch((size_t)npix * 16);
		if (tmp) {
			const uint16_t *h = (const uint16_t *)g_readback_cpu;
			for (size_t i = 0; i < (size_t)npix * 4; i++) {
				uint32_t s = (uint32_t)(h[i] & 0x8000u) << 16;
				uint32_t e = (h[i] >> 10) & 31u;
				uint32_t m = h[i] & 0x3ffu;
				uint32_t f = e == 0 ? s : e == 31 ? (s | 0x7f800000u | (m << 13)) : (s | ((e + 112u) << 23) | (m << 13));
				memcpy(&tmp[i], &f, 4);
			}
			gfx_encode_target_f32(&gt, tmp);
		}
	}
	te->gpu_newer = 0;
	g_costs.tgt_downloads++;
	g_costs.t_tgt_download += us_now() - tdl0;
}

static uint32_t g_tgt_min_addr = ~0u, g_tgt_max_end = 0;

static TargetEntry *get_target(const GfxTarget *t) {
	int k = target_kind(t->format);
	uint32_t tw = t->stride ? t->stride : t->width;
	uint32_t th = t->height;
	for (int i = 0; i < g_ntargets; i++) {
		TargetEntry *e = &g_targets[i];
		if (e->addr == t->data && e->stride == tw && e->height == th && e->kind == k) {
			if (e->dirty) { target_upload_from_guest(e); e->dirty = 0; }
			return e;
		}
	}
	if (g_ntargets >= MAX_TARGETS) return NULL;
	TargetEntry *e = &g_targets[g_ntargets++];
	memset(e, 0, sizeof *e);
	e->addr = t->data;
	e->format = t->format;
	e->width = t->width;
	e->height = th;
	e->stride = tw;
	e->type = t->type;
	e->ms_x = t->ms_x;
	e->ms_y = t->ms_y;
	e->scale_mode = t->scale_mode;
	e->kind = k;
	e->sx = rscale_cur;
	e->sy = rscale_cur;
	e->guest_bytes = target_guest_bytes(e);
	if (e->addr) {
		if (e->addr < g_tgt_min_addr) g_tgt_min_addr = e->addr;
		if (e->addr + e->guest_bytes > g_tgt_max_end) g_tgt_max_end = e->addr + e->guest_bytes;
	}
	alloc_image_block(tw * (uint32_t)e->sx, th * (uint32_t)e->sy, kind_format(k),
	                  DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
	                  DkImageType_2D, 1, 1, &e->img, &e->mem);
	e->desc_id = register_img_desc(&e->img, DkImageType_2D, 1, 1);
	if (e->sx == 1 && e->sy == 1) target_upload_from_guest(e);
	return e;
}

static DepthEntry *get_depth(const GfxTarget *t, uint32_t w, uint32_t h) {
	uint32_t key = t->ds_data ? t->ds_data : t->data;
	for (int i = 0; i < g_ndepths; i++) {
		DepthEntry *e = &g_depths[i];
		if (e->addr == key && e->width == w && e->height == h) return e;
	}
	if (g_ndepths >= MAX_DEPTHS) return NULL;
	DepthEntry *e = &g_depths[g_ndepths++];
	memset(e, 0, sizeof *e);
	e->addr = key;
	e->ds_addr = t->ds_data;
	e->ds_stencil = t->ds_stencil_data;
	e->ds_format = t->ds_format;
	e->width = w;
	e->height = h;
	e->stride = t->ds_stride ? t->ds_stride : w;
	e->type = t->ds_type;
	e->sx = rscale_cur;
	e->sy = rscale_cur;
	alloc_image_block(w * (uint32_t)e->sx, h * (uint32_t)e->sy, DkImageFormat_Z24S8,
	                  DkImageFlags_UsageRender | DkImageFlags_HwCompression,
	                  DkImageType_2D, 1, 1, &e->img, &e->mem);
	return e;
}

/* ---- Texture & sampler cache ---- */
typedef struct {
	uint32_t addr, format, width, height;
	int layout, bc, levels;
	uint64_t chash, last_scene;
	DkImage img;
	DkMemBlock mem;
	int desc_id;
} TexEntry;

static uint64_t g_scene_seq = 1;
static uint64_t g_write_clock = 1;
static uint64_t g_frame_seq = 1;

#define MAX_TEX 1024
#define TEX_HT_SIZE 2048
static TexEntry g_texs[MAX_TEX];
static uint16_t g_tex_ht[TEX_HT_SIZE];
static int g_ntexs;

static uint32_t tex_key_hash(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt, int layout) {
	uint32_t k = addr ^ (fmt * 0x9E3779B1u) ^ ((w | (h << 16)) * 0x85EBCA77u) ^ ((uint32_t)layout * 0xC2B2AE3Du);
	k ^= k >> 16; k *= 0x85EBCA6Bu; k ^= k >> 13;
	return k;
}

typedef struct {
	uint8_t min_f, mag_f, mip_l, u_m, v_m;
	int desc_id;
} SmpEntry;
static SmpEntry g_smps[MAX_SMP_DESC];
static int g_nsmps;

static DkWrapMode conv_wrap(uint8_t m) {
	switch (m & 7u) {
	case 0: return DkWrapMode_Repeat;
	case 1: return DkWrapMode_MirroredRepeat;
	case 3: return DkWrapMode_MirrorClampToEdge;
	case 4: return DkWrapMode_Repeat;
	default: return DkWrapMode_ClampToEdge;
	}
}

static int get_sampler(const GfxTexture *t) {
	uint8_t min_f = (t->min_filter == 1 || t->min_filter == 2) ? 1 : 0, mag_f = t->mag_filter & 1u;
	uint8_t mip_l = (uint8_t)(((t->min_filter & 3) == 2) || GFX_TEX_MIP_LINEAR(t->format));
	uint8_t u_m = t->u_mode & 7u, v_m = t->v_mode & 7u;
	for (int i = 0; i < g_nsmps; i++) {
		SmpEntry *s = &g_smps[i];
		if (s->min_f == min_f && s->mag_f == mag_f && s->mip_l == mip_l && s->u_m == u_m && s->v_m == v_m)
			return s->desc_id;
	}
	if (g_next_smp_desc >= MAX_SMP_DESC) return 0;
	int id = g_next_smp_desc++;
	DkSampler ds;
	dkSamplerDefaults(&ds);
	ds.minFilter = min_f ? DkFilter_Linear : DkFilter_Nearest;
	ds.magFilter = mag_f ? DkFilter_Linear : DkFilter_Nearest;
	ds.mipFilter = mip_l ? DkMipFilter_Linear : DkMipFilter_None;
	ds.wrapMode[0] = conv_wrap(u_m);
	ds.wrapMode[1] = conv_wrap(v_m);
	ds.wrapMode[2] = DkWrapMode_ClampToEdge;
	dkSamplerDescriptorInitialize(&g_smp_descs[id], &ds);
	g_smps[g_nsmps++] = (SmpEntry){ min_f, mag_f, mip_l, u_m, v_m, id };
	g_desc_dirty = 1;
	return id;
}

static uint64_t tex_guest_hash(const GfxTexture *t) {
	uint32_t bpp = (uint32_t)gfx_texture_bpp(t);
	size_t n = bpp ? (size_t)t->width * t->height * bpp : (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * 16;
	if (n > (4u << 20)) n = 4u << 20;
	const uint8_t *p = (const uint8_t *)G(t->data);
	uint64_t h;
	static int exact = -1;
	if (exact < 0) exact = getenv("RECOMP_EXACT_TEX_HASH") != 0;
	if (exact || n <= 65536) {
		h = hash_bytes(p, n);
	} else {
		h = hash_bytes(p, 4096);
		size_t step = (n - 4096) >> 6;
		for (size_t off = 4096; off + 64 <= n; off += step)
			h ^= hash_bytes(p + off, 64) * 0x9e3779b97f4a7c15ull;
	}
	return h ^ ((uint64_t)t->format << 32) ^ ((uint64_t)t->width << 16) ^ t->height;
}

static void upload_tex_pixels(TexEntry *e, const GfxTexture *t) {
	int is_cube = e->layout == 3;
	int faces = is_cube ? 6 : 1;
	int lvls = e->levels;
	size_t total = 0;
	for (int f = 0; f < faces; f++) {
		for (int l = 0; l < lvls; l++) {
			GfxTexture view;
			if (is_cube) gfx_texture_face_level(t, f, l, &view);
			else gfx_texture_level(t, l, &view);
			uint32_t w = view.width ? view.width : 1, h = view.height ? view.height : 1;
			size_t sz = e->bc ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (size_t)e->bc : (size_t)w * h * 4u;
			total += align_up((uint32_t)sz, 256u);
		}
	}
	DkGpuAddr base_gpu = 0;
	uint8_t *base_cpu = (uint8_t *)upload_alloc(total, &base_gpu);
	if (!base_cpu) return;

	open_batch();
	dkCmdBufBarrier(cur_cmd(), DkBarrier_Primitives, 0);
	size_t off = 0;
	for (int f = 0; f < faces; f++) {
		for (int l = 0; l < lvls; l++) {
			GfxTexture view;
			if (is_cube) gfx_texture_face_level(t, f, l, &view);
			else gfx_texture_level(t, l, &view);
			uint32_t w = view.width ? view.width : 1, h = view.height ? view.height : 1;
			size_t sz = e->bc ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (size_t)e->bc : (size_t)w * h * 4u;
			if (e->bc) gfx_texture_blocks(&view, base_cpu + off);
			else gfx_decode_texture_rgba8(&view, base_cpu + off);
			DkImageView iv;
			dkImageViewDefaults(&iv, &e->img);
			iv.layerOffset = (uint16_t)f;
			iv.layerCount = 1;
			iv.mipLevelOffset = (uint8_t)l;
			iv.mipLevelCount = 1;
			DkCopyBuf cb = { base_gpu + off, 0, 0 };
			DkImageRect rc = { 0, 0, 0, w, h, 1 };
			dkCmdBufCopyBufferToImage(cur_cmd(), &cb, &iv, &rc, 0);
			off += align_up((uint32_t)sz, 256u);
		}
	}
	dkCmdBufBarrier(cur_cmd(), DkBarrier_Primitives, DkInvalidateFlags_Image);
	g_scene_need_bind = 1;
	g_st.valid = 0;
}

static DkResHandle bind_unit_tex(const GfxTexture *t, int unit, uint32_t *srgb_dec, uint32_t *srgb_enc) {
	if (!t || !t->data || !t->width || !t->height) return 0;
	TargetEntry *te = NULL;
	if (t->data >= g_tgt_min_addr && t->data < g_tgt_max_end) {
		for (int i = 0; i < g_ntargets; i++) {
			TargetEntry *c = &g_targets[i];
			if (!c->gpu_newer || !c->addr) continue;
			uint32_t sz = c->guest_bytes;
			if (t->data >= c->addr && t->data < c->addr + sz) {
				int c_exact = (c->addr == t->data);
				int te_exact = te ? (te->addr == t->data) : 0;
				if (!te || (c_exact > te_exact) || (c_exact == te_exact && c->gen > te->gen))
					te = c;
			}
		}
	}
	if (te) {
		GfxTarget gt = { .format = te->format };
		uint32_t bpp = (uint32_t)gfx_target_bpp(&gt);
		if (!bpp) bpp = 4u;
		uint32_t off_px = (t->data - te->addr) / bpp;
		uint32_t x0 = off_px % te->stride, y0 = off_px / te->stride;
		int layout0 = (t->u_mode >> 4) & 3;
		int exact_dims = (t->data == te->addr && layout0 != 3 && !te->dirty && t->width == te->stride && t->height == te->height);
		int sub_dims = (!exact_dims && t->data == te->addr && layout0 != 3 && !te->dirty && t->width <= te->stride && t->height <= te->height);
		if ((exact_dims || sub_dims) && unit >= 0 && unit < 16) {
			int tg = GFX_TEX_GAMMA(t->format) != 0, st = (te->kind == TK_U8S);
			if (tg && !st && srgb_dec) *srgb_dec |= 1u << unit;
			if (!tg && st && srgb_enc) *srgb_enc |= 1u << unit;
		}
		if (exact_dims && te != g_scene_te) {
			if (te->bar_gen != te->gen) { g_need_tex_barrier = 1; te->bar_gen = te->gen; }
			return dkMakeTextureHandle((uint32_t)te->desc_id, (uint32_t)get_sampler(t));
		}
		if ((exact_dims && te == g_scene_te) || sub_dims) {
			ViewEntry *v = NULL;
			for (int i = 0; i < g_nviews; i++) {
				ViewEntry *c = &g_views[i];
				if (c->te == te && c->x == x0 && c->y == y0 && c->w == t->width && c->h == t->height &&
				    c->kind == te->kind && c->sx == te->sx && c->sy == te->sy) {
					v = c; break;
				}
			}
			if (!v) {
				int reuse_vdesc = -1;
				if (g_nviews < MAX_VIEWS) {
					v = &g_views[g_nviews++];
				} else {
					wait_all_slots();
					v = &g_views[0];
					for (int i = 1; i < g_nviews; i++)
						if (g_views[i].copy_gen < v->copy_gen) v = &g_views[i];
					reuse_vdesc = v->desc_id;
					if (v->mem) dkMemBlockDestroy(v->mem);
				}
				memset(v, 0, sizeof *v);
				v->te = te; v->x = x0; v->y = y0; v->w = t->width; v->h = t->height;
				v->kind = te->kind; v->sx = te->sx; v->sy = te->sy;
				alloc_image_block(t->width * (uint32_t)te->sx, t->height * (uint32_t)te->sy, kind_format(te->kind),
				                  DkImageFlags_Usage2DEngine, DkImageType_2D, 1, 1, &v->img, &v->mem);
				v->desc_id = reuse_vdesc > 0 ? write_img_desc(reuse_vdesc, &v->img, DkImageType_2D, 1, 1)
				                             : register_img_desc(&v->img, DkImageType_2D, 1, 1);
			}
			if (v->copy_gen != te->gen || te == g_scene_te) {
				int seen = 0;
				for (int c = 0; c < g_ncopy; c++) if (g_copy_views[c] == v) seen = 1;
				if (!seen && g_ncopy < 8) g_copy_views[g_ncopy++] = v;
				v->copy_gen = te->gen;
			}
			return dkMakeTextureHandle((uint32_t)v->desc_id, (uint32_t)get_sampler(t));
		}
		static int dl_said;
		if (dl_said++ < 16) {
			fprintf(stderr, "deko3d: target download fallback tex %#x fmt %#x %ux%u u%#x over tgt %#x fmt %#x %ux%u (stride %u) kind %d\n",
			        t->data, t->format, t->width, t->height, t->u_mode, te->addr, te->format, te->width, te->height, te->stride, te->kind);
		}
		target_download_to_guest(te);
	}
	int layout = (t->u_mode >> 4) & 3;
	uint64_t th0 = us_now();
	uint32_t tslot = tex_key_hash(t->data, t->width, t->height, t->format, layout) & (TEX_HT_SIZE - 1);
	while (g_tex_ht[tslot]) {
		TexEntry *e = &g_texs[g_tex_ht[tslot] - 1u];
		if (e->addr == t->data && e->width == t->width && e->height == t->height && e->format == t->format && e->layout == layout) {
			if (e->last_scene != g_frame_seq) {
				e->last_scene = g_frame_seq;
				uint64_t ch = tex_guest_hash(t);
				if (e->chash != ch) {
					e->chash = ch;
					g_costs.tex_uploads++;
					upload_tex_pixels(e, t);
				}
			}
			g_costs.t_texup += us_now() - th0;
			return dkMakeTextureHandle((uint32_t)e->desc_id, (uint32_t)get_sampler(t));
		}
		tslot = (tslot + 1u) & (TEX_HT_SIZE - 1);
	}
	int tidx, reuse_desc = -1;
	if (g_ntexs < MAX_TEX) {
		tidx = g_ntexs++;
		g_tex_ht[tslot] = (uint16_t)(tidx + 1);
	} else {
		int vic = 0;
		for (int i = 1; i < g_ntexs; i++)
			if (g_texs[i].last_scene < g_texs[vic].last_scene) vic = i;
		if (g_texs[vic].last_scene + 3 >= g_frame_seq) wait_all_slots();
		reuse_desc = g_texs[vic].desc_id;
		if (g_texs[vic].mem) dkMemBlockDestroy(g_texs[vic].mem);
		tidx = vic;
		memset(g_tex_ht, 0, sizeof g_tex_ht);
		for (int i = 0; i < g_ntexs; i++) {
			if (i == tidx) continue;
			TexEntry *te2 = &g_texs[i];
			uint32_t s2 = tex_key_hash(te2->addr, te2->width, te2->height, te2->format, te2->layout) & (TEX_HT_SIZE - 1);
			while (g_tex_ht[s2]) s2 = (s2 + 1u) & (TEX_HT_SIZE - 1);
			g_tex_ht[s2] = (uint16_t)(i + 1);
		}
		uint32_t ns = tex_key_hash(t->data, t->width, t->height, t->format, layout) & (TEX_HT_SIZE - 1);
		while (g_tex_ht[ns]) ns = (ns + 1u) & (TEX_HT_SIZE - 1);
		g_tex_ht[ns] = (uint16_t)(tidx + 1);
	}

	int lvls = layout == 3 ? gfx_texture_cube_levels(t) : gfx_texture_levels(t);
	if (lvls < 1) lvls = 1;
	uint32_t base_fmt = t->format & 0x9F000000u;
	int bc = (base_fmt == 0x85000000u) ? 8 : (base_fmt == 0x86000000u || base_fmt == 0x87000000u) ? 16 : 0;
	DkImageFormat dfmt = DkImageFormat_RGBA8_Unorm;
	if (bc) {
		int srgb = GFX_TEX_GAMMA(t->format) != 0;
		if (base_fmt == 0x85000000u) dfmt = srgb ? DkImageFormat_RGBA_BC1_sRGB : DkImageFormat_RGBA_BC1;
		else if (base_fmt == 0x86000000u) dfmt = srgb ? DkImageFormat_RGBA_BC2_sRGB : DkImageFormat_RGBA_BC2;
		else dfmt = srgb ? DkImageFormat_RGBA_BC3_sRGB : DkImageFormat_RGBA_BC3;
	} else if (GFX_TEX_GAMMA(t->format)) {
		dfmt = DkImageFormat_RGBA8_Unorm_sRGB;
	}

	uint64_t ch = tex_guest_hash(t);
	TexEntry *e = &g_texs[tidx];
	g_costs.tex_new++; g_costs.tex_uploads++;
	memset(e, 0, sizeof *e);
	e->addr = t->data;
	e->format = t->format;
	e->width = t->width;
	e->height = t->height;
	e->layout = layout;
	e->bc = bc;
	e->levels = lvls;
	e->chash = ch;
	e->last_scene = g_frame_seq;
	DkImageType itype = layout == 3 ? DkImageType_Cubemap : DkImageType_2D;
	alloc_image_block(t->width, t->height, dfmt, 0, itype, layout == 3 ? 6 : 1, (uint32_t)lvls, &e->img, &e->mem);
	e->desc_id = reuse_desc > 0 ? write_img_desc(reuse_desc, &e->img, itype, layout == 3 ? 6 : 1, (uint32_t)lvls)
	                            : register_img_desc(&e->img, itype, layout == 3 ? 6 : 1, (uint32_t)lvls);
	upload_tex_pixels(e, t);
	g_costs.t_texup += us_now() - th0;
	return dkMakeTextureHandle((uint32_t)e->desc_id, (uint32_t)get_sampler(t));
}

/* ---- Shader cache (.dksh) ---- */
typedef struct {
	uint32_t hash;
	int is_frag, kind, points, fzero, pcf;
	uint32_t cube_mask, snap_mask, layout_hash;
	int ok, tried;
	DkShader shader;
	int smp_units[16], nsmp, fetch, nldst, pa_words, ldst_sa[8];
} DekoShader;

#define MAX_SHADERS 2048
#define SH_HT_SIZE 4096
static DekoShader g_shaders[MAX_SHADERS];
static uint16_t g_sh_ht[SH_HT_SIZE];
static int g_nshaders;

static uint32_t sh_key_hash(uint32_t hash, int is_frag, int kind, uint32_t cube, uint32_t snap, uint32_t layout_hash, int points, int fzero) {
	uint32_t h = hash ^ (cube * 0x9E3779B1u) ^ (snap * 0x85EBCA77u) ^ (layout_hash * 0xC2B2AE3Du);
	h ^= (uint32_t)((is_frag & 1) | ((kind & 7) << 1) | ((points & 7) << 4) | ((fzero & 15) << 7)) * 0x27D4EB2Du;
	h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13;
	return h;
}

static void make_shader_key(char *out, size_t n, uint32_t hash, int is_frag, int kind, uint32_t cube, uint32_t snap, uint32_t layout_hash, int points, int fzero, int pcf) {
	snprintf(out, n, "%08x_%c_k%d_c%08x_s%08x_l%08x_p%d_z%d_f%d",
	         hash, is_frag ? 'f' : 'v', kind, cube, snap, layout_hash, points, fzero, pcf);
}

static void build_sa_smp(const UsseDraw *draw, uint32_t out[256]) {
	for (int i = 0; i < 256; i++)
		out[i] = draw->sa_unit[i] ? draw->sa_unit[i] - 1u : draw->sa[i].u;
}

static DekoShader *get_deko_shader(uint32_t addr, const UsseProgram *prog, const UsseVertexLayout *vl, const UsseFragLayout *fl,
                                   const UsseDraw *draw, int kind, uint32_t cube_mask, uint32_t snap_mask,
                                   const UsseVfLayout *vf, uint32_t layout_hash, int points, int fzero) {
	(void)addr;
	uint32_t hash = usse_program_hash(prog);
	int is_frag = usse_is_fragment(prog);
	uint32_t sa_buf[256];
	int sa_built = 0;
	if ((cube_mask || snap_mask) && draw) {
		build_sa_smp(draw, sa_buf);
		sa_built = 1;
	}
	usse_spirv_key(prog, fl, sa_built ? sa_buf : NULL, &kind, &cube_mask, &snap_mask);
	if (!is_frag) kind = -1;

	uint32_t slot = sh_key_hash(hash, is_frag, kind, cube_mask, snap_mask, layout_hash, points, fzero) & (SH_HT_SIZE - 1);
	while (g_sh_ht[slot]) {
		DekoShader *s = &g_shaders[g_sh_ht[slot] - 1u];
		if (s->hash == hash && s->is_frag == is_frag && s->kind == kind && s->cube_mask == cube_mask &&
		    s->snap_mask == snap_mask && s->layout_hash == layout_hash && s->points == points && s->fzero == fzero) {
			return s->ok ? s : NULL;
		}
		slot = (slot + 1u) & (SH_HT_SIZE - 1);
	}
	if (g_nshaders >= MAX_SHADERS) return NULL;
	int idx = g_nshaders++;
	DekoShader *s = &g_shaders[idx];
	memset(s, 0, sizeof *s);
	s->hash = hash; s->is_frag = is_frag; s->kind = kind; s->cube_mask = cube_mask;
	s->snap_mask = snap_mask; s->layout_hash = layout_hash; s->points = points; s->fzero = fzero;
	s->tried = 1;
	g_sh_ht[slot] = (uint16_t)(idx + 1);

	/* Translate once on CPU solely to extract sampler units, pa_words, ldst_sa, and frag_fetch metadata */
	g_costs.shaders++;
	uint64_t ts0 = us_now();
	if (!sa_built) {
		if (draw) build_sa_smp(draw, sa_buf);
		else memset(sa_buf, 0, sizeof sa_buf);
		sa_built = 1;
	}
	int smp_units[16], nsmp = 0;
	usse_spirv_set_points(points);
	usse_spirv_set_fetch_zero(fzero >= USSE_FB_LERP_KEEP ? 2 : fzero ? 1 : 0);
	SpirvModule m = usse_spirv_translate(prog, vl, fl, sa_buf, 16, smp_units, &nsmp, kind < 0 ? 0 : kind, cube_mask, snap_mask, vf);
	usse_spirv_set_points(0);
	usse_spirv_set_fetch_zero(0);
	if (!m.ok) { usse_spirv_free(&m); return NULL; }
	s->nsmp = nsmp;
	memcpy(s->smp_units, smp_units, sizeof smp_units);
	s->fetch = m.frag_fetch;
	s->nldst = m.nldst_sa;
	s->pa_words = m.pa_words;
	memcpy(s->ldst_sa, m.ldst_sa, sizeof s->ldst_sa);

	char key[96], path[1024];
	make_shader_key(key, sizeof key, hash, is_frag, kind, cube_mask, snap_mask, layout_hash, points, fzero, 0);
	const char *dir = getenv("RECOMP_DKSH_DIR"), *home = getenv("RECOMP_HOME");
	s->ok = find_preloaded_dksh(key, &s->shader);
	if (!s->ok) {
		if (dir && *dir) snprintf(path, sizeof path, "%s/%s.dksh", dir, key);
		else snprintf(path, sizeof path, "%s/shaders/%s.dksh", home ? home : ".", key);
		s->ok = load_dksh_file(&s->shader, path);
	}
	if (!s->ok) {
		snprintf(path, sizeof path, "romfs:/shaders/%s.dksh", key);
		s->ok = load_dksh_file(&s->shader, path);
	}
	int dumped = m.words && m.nwords != 0;
	if (!s->ok && dumped) {
		/* the translated SPIR-V, in the RECOMP_DUMP_SPV layout tools/build_dksh.py reads: the next build has it */
		char sp[1100];
		snprintf(sp, sizeof sp, "%s/shaders_missing", home ? home : ".");
		mkdir(sp, 0777);
		snprintf(sp, sizeof sp, "%s/shaders_missing/%s.spv", home ? home : ".", key);
		FILE *sf = fopen(sp, "wb");
		if (sf) { fwrite(m.words, 4, m.nwords, sf); fclose(sf); }
		else fprintf(stderr, "deko3d: spv dump fopen(%s) failed errno %d\n", sp, errno);
		snprintf(sp, sizeof sp, "%s/shaders_missing/%s.meta", home ? home : ".", key);
		FILE *mf = fopen(sp, "w");
		if (mf) { fprintf(mf, "%d %d 0\n", is_frag, nsmp); fclose(mf); }
		else fprintf(stderr, "deko3d: meta dump fopen(%s) failed errno %d\n", sp, errno);
	}
	usse_spirv_free(&m);
	g_costs.t_shader += us_now() - ts0;
	if (!s->ok) fprintf(stderr, "deko3d: missing shader %s.dksh (%s)\n", key, dumped ? "SPIR-V kept in shaders_missing/" : "no SPIR-V to dump");
	return s->ok ? s : NULL;
}

/* ---- State mapping helpers ---- */
static DkCompareOp conv_cmp(uint32_t f) {
	static const DkCompareOp t[8] = {
		DkCompareOp_Never, DkCompareOp_Less, DkCompareOp_Equal, DkCompareOp_Lequal,
		DkCompareOp_Greater, DkCompareOp_NotEqual, DkCompareOp_Gequal, DkCompareOp_Always
	};
	return t[f & 7];
}
static DkStencilOp conv_sop(uint8_t o) {
	static const DkStencilOp t[8] = {
		DkStencilOp_Keep, DkStencilOp_Zero, DkStencilOp_Replace, DkStencilOp_Incr,
		DkStencilOp_Decr, DkStencilOp_Invert, DkStencilOp_IncrWrap, DkStencilOp_DecrWrap
	};
	return t[o & 7];
}
static DkBlendFactor conv_factor(uint8_t f) {
	static const DkBlendFactor t[10] = {
		DkBlendFactor_Zero, DkBlendFactor_One, DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor,
		DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_DstColor, DkBlendFactor_InvDstColor,
		DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha
	};
	return f < 10 ? t[f] : DkBlendFactor_One;
}
static DkBlendOp conv_blend_op(uint8_t fn) {
	switch (fn) {
	case 2: return DkBlendOp_Sub;
	case 3: return DkBlendOp_RevSub;
	case 4: return DkBlendOp_Min;
	case 5: return DkBlendOp_Max;
	default: return DkBlendOp_Add;
	}
}

/* ---- Batching ---- */
static void soft_fallback(const GfxDraw *d, const char *why) {
	(void)why;
	__atomic_add_fetch(&n_fb_draws, 1, __ATOMIC_RELAXED);
	wait_all_slots();
	if (g_scene_te) target_download_to_guest(g_scene_te);
	gfx_soft_draw(d);
	if (g_scene_te) g_scene_te->dirty = 1;
}

void gfx_gpu_guest_writing(uint32_t addr, uint32_t len) {
	if (!ensure_gpu()) return;
	for (int i = 0; i < g_ntargets; i++) {
		TargetEntry *e = &g_targets[i];
		uint32_t sz = target_guest_bytes(e);
		if (addr < e->addr + sz && addr + len > e->addr && e->gpu_newer) {
			wait_all_slots();
			target_download_to_guest(e);
		}
	}
}
void gfx_gpu_guest_written(uint32_t addr, uint32_t len) {
	if (!ensure_gpu()) return;
	for (int i = 0; i < g_ntargets; i++) {
		TargetEntry *e = &g_targets[i];
		uint32_t sz = target_guest_bytes(e);
		if (addr < e->addr + sz && addr + len > e->addr) e->dirty = 1;
	}
}
void gfx_gpu_guest_reading(uint32_t addr, uint32_t len) { gfx_gpu_guest_writing(addr, len); }
void gfx_gpu_sync_all(void) {
	if (!ensure_gpu()) return;
	uint64_t ts0 = us_now();
	static int sync_dl = -1;
	if (sync_dl < 0) sync_dl = getenv("RECOMP_GPU_SYNC_DOWNLOAD") != 0;
	if (sync_dl) {
		wait_all_slots();
		for (int i = 0; i < g_ntargets; i++) target_download_to_guest(&g_targets[i]);
	} else {
		submit_batch(0, NULL);
	}
	g_costs.t_scene_sync += us_now() - ts0;
}

void gfx_gpu_begin_scene(const GfxTarget *t) {
	if (!ensure_gpu()) { gfx_soft_begin_scene(t); return; }
	g_scene_seq++;
	rscale_cur = rscale_want;
	g_scene_tgt = *t;
	g_scene_te = t->data ? get_target(t) : NULL;
	uint32_t dw = g_scene_te ? g_scene_te->stride : (t->width ? t->width : 960);
	uint32_t dh = g_scene_te ? g_scene_te->height : (t->height ? t->height : 544);
	g_scene_de = t->depth ? get_depth(t, dw, dh) : NULL;
	g_scene_sx = g_scene_te ? g_scene_te->sx : rscale_cur;
	g_scene_sy = g_scene_te ? g_scene_te->sy : rscale_cur;
	g_scene_first_draw = 1;
	g_scene_need_bind = 1;
}

static uint32_t vf_attr_bytes(uint8_t fmt) {
	switch (fmt) { case 9: case 10: return 4; case 6: case 3: case 7: case 2: case 8: return 2; default: return 1; }
}

#define VF_CACHE_SIZE 256
typedef struct {
	const UsseProgram *vp;
	uint32_t vp_hash;
	uint16_t nattr, inst_st_mask;
	uint8_t hw_inst, valid, ok;
	GfxAttr attr[16];
	uint16_t stride[16];
	UsseVfLayout vf;
	uint32_t layout_hash;
	uint32_t st_ext[16];
} VfCacheEntry;
static VfCacheEntry g_vf_cache[VF_CACHE_SIZE];

static uint32_t vf_key_hash(uint32_t vp_hash, int hw_inst, uint16_t inst_st_mask, const GfxDraw *d) {
	uint32_t h = vp_hash ^ ((uint32_t)hw_inst << 24) ^ ((uint32_t)inst_st_mask << 8) ^ (uint32_t)d->nattr;
	h ^= hash32_bytes(d->attr, (size_t)d->nattr * sizeof(GfxAttr));
	h ^= hash32_bytes(d->stride, sizeof d->stride);
	h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13;
	return h;
}

void gfx_gpu_draw(const GfxDraw *d) {
	if (!ensure_gpu()) { gfx_soft_draw(d); return; }
	if (!g_scene_te && !g_scene_de) { soft_fallback(d, "no target"); return; }
	if (d->clip_mode == 0x40000000u) return;
	if (g_scene_te && g_scene_te->dirty) { target_upload_from_guest(g_scene_te); g_scene_te->dirty = 0; }
	uint64_t td0 = us_now();
	g_costs.draws++;

	int kind = gfx_prim_kind(d->prim);
	int is_points = kind == GFX_PRIM_POINTS;
	const UsseProgram *vp = usse_get(d->vprog), *fp = usse_get(d->fprog);
	const UsseVertexLayout *vl = usse_vertex_layout(vp);
	const UsseFragLayout *fl = usse_fragment_layout(fp);

	UsseDraw vdraw, fdraw;
	usse_setup_registers(vp, &vdraw, d->vuniforms, d->vubuf);
	usse_setup_registers(fp, &fdraw, d->funiforms, d->fubuf);
	UsseInst scratch;
	usse_run_secondary(vp, &scratch, &vdraw);
	usse_run_secondary(fp, &scratch, &fdraw);

	uint32_t cube_mask = 0, vcube_mask = 0;
	for (int i = 0; i < 16; i++) {
		if (d->tex[i].data && ((d->tex[i].u_mode >> 4) & 3) == 3) cube_mask |= 1u << i;
		if (d->vtex[i].data && ((d->vtex[i].u_mode >> 4) & 3) == 3) vcube_mask |= 1u << i;
	}

	uint32_t ntri = gfx_prim_tris(d->prim, d->count);
	uint32_t nverts = is_points ? 6u * d->count : 3u * ntri;
	if (!nverts) return;

	/* Try GPU-side raw vertex fetch plan first (matching gfx_gpu.c) */
	const UsseVfLayout *vf_ptr = NULL;
	const uint32_t *st_ext_ptr = NULL;
	uint32_t layout_hash = 0;
	int use_vf = 0;
	int hw_inst = gfx_draw_instanced(d) && !is_points && d->indices &&
	              (kind == GFX_PRIM_TRIANGLES || kind == GFX_PRIM_EDGES) &&
	              d->index_wrap >= 3 && (d->index_wrap % 3 == 0) && (d->count % d->index_wrap == 0);
	uint32_t inst_cnt = hw_inst ? (d->count / d->index_wrap) : 1u;
	uint32_t idx_scan = d->index_wrap ? d->index_wrap : d->count;
	uint32_t lo = ~0u, hi = 0;
	int idx_seq = 1;
	if (d->indices && idx_scan) {
		const uint8_t *ib = G(d->indices);
		if (!d->index_format) {
			const uint16_t *ib16 = (const uint16_t *)ib;
			if (is_points) {
				uint32_t first_ix = ib16[0];
				for (uint32_t i = 0; i < idx_scan; i++) {
					uint32_t ix = ib16[i];
					if (ix != first_ix + i) idx_seq = 0;
					if (ix < lo) lo = ix;
					if (ix > hi) hi = ix;
				}
			} else {
				for (uint32_t i = 0; i < idx_scan; i++) {
					uint32_t ix = ib16[i];
					if (ix < lo) lo = ix;
					if (ix > hi) hi = ix;
				}
			}
		} else {
			const uint32_t *ib32 = (const uint32_t *)ib;
			if (is_points) {
				uint32_t first_ix = ib32[0];
				for (uint32_t i = 0; i < idx_scan; i++) {
					uint32_t ix = ib32[i];
					if (ix != first_ix + i) idx_seq = 0;
					if (ix < lo) lo = ix;
					if (ix > hi) hi = ix;
				}
			} else {
				for (uint32_t i = 0; i < idx_scan; i++) {
					uint32_t ix = ib32[i];
					if (ix < lo) lo = ix;
					if (ix > hi) hi = ix;
				}
			}
		}
	} else {
		lo = 0;
		hi = d->count ? d->count - 1 : 0;
	}
	uint32_t nv_span = hi >= lo ? (hi - lo + 1u) : 1u;
	uint32_t draw_nverts = hw_inst ? d->index_wrap : nverts;
	int can_vf = (!gfx_draw_instanced(d) || hw_inst) && hi >= lo && (is_points ? (!d->indices || idx_seq) : (nv_span <= 4u * draw_nverts + 4096u));
	if (can_vf && d->nattr <= 16) {
		uint32_t vp_hash = usse_program_hash(vp);
		uint16_t inst_st_mask = 0;
		if (hw_inst) {
			for (int s = 0; s < d->nstream && s < 16; s++)
				if (d->stream_src[s] >= 2) inst_st_mask |= (uint16_t)(1u << s);
		}
		VfCacheEntry *ce = &g_vf_cache[vf_key_hash(vp_hash, hw_inst, inst_st_mask, d) & (VF_CACHE_SIZE - 1)];
		if (!(ce->valid && ce->vp == vp && ce->vp_hash == vp_hash && ce->hw_inst == (uint8_t)hw_inst &&
		      ce->inst_st_mask == inst_st_mask && ce->nattr == (uint16_t)d->nattr &&
		      memcmp(ce->attr, d->attr, (size_t)d->nattr * sizeof(GfxAttr)) == 0 &&
		      memcmp(ce->stride, d->stride, sizeof ce->stride) == 0)) {
			memset(ce, 0, sizeof *ce);
			ce->vp = vp; ce->vp_hash = vp_hash; ce->hw_inst = (uint8_t)hw_inst;
			ce->inst_st_mask = inst_st_mask; ce->nattr = (uint16_t)d->nattr;
			memcpy(ce->attr, d->attr, (size_t)d->nattr * sizeof(GfxAttr));
			memcpy(ce->stride, d->stride, sizeof ce->stride);
			ce->valid = 1;
			GfxVertexPacker pk;
			gfx_vertex_packer_init(&pk, d, vl, usse_program_pa_words(vp));
			if (!pk.slow) {
				int j = 0, ok = 1, any_inst_st = 0;
				for (int i = 0; i < d->nattr && j < pk.n; i++) {
					const GfxAttr *a = &d->attr[i];
					if (!usse_find_attr(vl, a->reg)) continue;
					const GfxPackAttr *q = &pk.a[j];
					if (a->stream >= 16 || q->base != d->stream[a->stream] + a->offset || q->stride != d->stride[a->stream]) {
						ok = 0; break;
					}
					uint16_t vf_st = (uint16_t)(a->stream | ((hw_inst && d->stream_src[a->stream] >= 2) ? 0x100u : 0u));
					if (vf_st & 0x100u) any_inst_st = 1;
					ce->vf.a[j] = (UsseVfAttr){ vf_st, a->offset, d->stride[a->stream], q->reg, q->fmt, q->comps, q->kind, q->dcomps, q->rawlen };
					uint32_t ext = ce->vf.a[j].offset + (ce->vf.a[j].kind == 1 ? ce->vf.a[j].rawlen : ce->vf.a[j].comps * vf_attr_bytes(ce->vf.a[j].fmt));
					if (ext > ce->st_ext[a->stream]) ce->st_ext[a->stream] = ext;
					j++;
				}
				if (ok && j == pk.n) {
					ce->vf.n = pk.n; ce->vf.npa = pk.npa; ce->vf.index_reg = pk.index_reg; ce->vf.instance_reg = pk.instance_reg;
					ce->layout_hash = hash32_bytes(&ce->vf, sizeof ce->vf) | 1u;
					if (hw_inst && !any_inst_st && ce->vf.instance_reg >= 0) ce->layout_hash = (ce->layout_hash ^ 0x494E5354u) | 1u;
					ce->ok = 1;
				}
			}
		}
		if (ce->ok && !(is_points && d->indices && ce->vf.index_reg >= 0 && lo != 0)) {
			vf_ptr = &ce->vf;
			st_ext_ptr = ce->st_ext;
			layout_hash = ce->layout_hash;
			use_vf = 1;
		}
	}

	int vs_pts = is_points ? 2 : 0;
	DekoShader *vs = get_deko_shader(d->vprog, vp, vl, NULL, &vdraw, -1, vcube_mask, 0, use_vf ? vf_ptr : NULL, layout_hash, vs_pts, 0);
	if (use_vf && !vs) {
		use_vf = 0; layout_hash = 0;
		vs = get_deko_shader(d->vprog, vp, vl, NULL, &vdraw, -1, vcube_mask, 0, NULL, 0, vs_pts, 0);
	}
	if (is_points && !vs) {
		vs_pts = 1;
		vs = get_deko_shader(d->vprog, vp, vl, NULL, &vdraw, -1, vcube_mask, 0, NULL, 0, 1, 0);
	}
	int ckind = g_scene_te ? g_scene_te->kind : TK_U8;
	int fzero = fl && fl->frag_color_used && (ckind == TK_F16) && !(d->blend.present && (d->blend.color_func || d->blend.alpha_func)) ? usse_fragment_fb_blend(fp) : 0;
	DekoShader *fs = get_deko_shader(d->fprog, fp, vl, fl, &fdraw, ckind, cube_mask, 0, NULL, 0, is_points, fzero);
	if (!vs || !fs) { soft_fallback(d, "missing dksh"); return; }

	/* Bind textures BEFORE ring/draw setup so inline uploads & views stay cleanly ordered */
	uint32_t srgb_dec = 0, srgb_enc = 0, vsrgb_dec = 0, vsrgb_enc = 0;
	DkResHandle ftex[24] = {0}, vtex[16] = {0};
	for (int i = 0; i < fs->nsmp && i < 16; i++) {
		int u = fs->smp_units[i];
		ftex[i] = bind_unit_tex((u >= 0 && u < 16) ? &d->tex[u] : NULL, u, &srgb_dec, &srgb_enc);
	}
	for (int i = 0; i < vs->nsmp && i < 16; i++) {
		int u = vs->smp_units[i];
		vtex[i] = bind_unit_tex((u >= 0 && u < 16) ? &d->vtex[u] : NULL, u, &vsrgb_dec, &vsrgb_enc);
	}
	int n_ftex = fs->nsmp;

	/* Size vertex, index, window, and uniform allocations */
	int raw_idx = d->indices && (kind == GFX_PRIM_TRIANGLES || kind == GFX_PRIM_EDGES);
	int use_idx16 = 0;
	uint32_t vwords_need = 0;
	uint32_t idraw_cnt = hw_inst ? d->index_wrap : nverts;
	if (use_vf) {
		for (int s = 0; s < d->nstream && s < 16; s++) {
			if (!d->stream[s] || !st_ext_ptr[s]) continue;
			uint32_t bytes = (hw_inst && d->stream_src[s] >= 2) ? ((inst_cnt - 1u) * d->stride[s] + st_ext_ptr[s]) : ((hi - lo) * d->stride[s] + st_ext_ptr[s]);
			vwords_need += align_up(bytes, 4) / 4u + 4u;
		}
	}
	uint32_t iwords_need = (use_vf && !is_points) ? ((raw_idx && !d->index_format) ? (idraw_cnt + 1u) / 2u : idraw_cnt) : 0;
	if (use_vf && (vwords_need > (2u << 20) || vwords_need + 64u > VRING_WORDS || iwords_need > IRING_WORDS)) {
		use_vf = 0; layout_hash = 0;
		vs = get_deko_shader(d->vprog, vp, vl, NULL, &vdraw, -1, vcube_mask, 0, NULL, 0, vs_pts, 0);
		if (is_points && !vs) { vs_pts = 1; vs = get_deko_shader(d->vprog, vp, vl, NULL, &vdraw, -1, vcube_mask, 0, NULL, 0, 1, 0); }
		if (!vs) { soft_fallback(d, "missing dksh"); return; }
	}
	int pk_slow = 0;
	int indexed_pack = !use_vf && !is_points && !gfx_draw_instanced(d) && d->indices && hi >= lo && nv_span <= nverts && nverts <= IRING_WORDS;
	int npa = vs->pa_words ? vs->pa_words : 1;
	if (npa > 160) npa = 160;
	if (!use_vf) {
		uint32_t npack = is_points ? (vs_pts == 2 ? d->count : nverts) : (indexed_pack ? nv_span : nverts);
		vwords_need = npack * (uint32_t)npa;
		iwords_need = indexed_pack ? nverts : 0;
		if (vwords_need + 64u > VRING_WORDS || iwords_need > IRING_WORDS) { soft_fallback(d, "draw larger than ring"); return; }
	}
	uint32_t wwords_need = (uint32_t)vs->nldst * (USSE_LDST_WINDOW / 4u);
	uint32_t ustride = align_up(sizeof(UsseDrawUniforms), 256);
	uint32_t vStart = align_up(g_vpos, 64), iStart = align_up(g_ipos, 16), wStart = align_up(g_wpos, 64);
	int vwrap = vStart + vwords_need + 64u > VRING_WORDS;
	int iwrap = iStart + iwords_need > IRING_WORDS;
	int wwrap = wStart + wwords_need + 64u > WRING_WORDS;
	int uwrap = g_upos + 2u * ustride > URING_BYTES;
	if (vwrap || iwrap || wwrap || uwrap) {
		wait_all_slots();
		if (vwrap) { g_vpos = 0; vStart = 0; }
		if (iwrap) { g_ipos = 0; iStart = 0; }
		if (wwrap) { g_wpos = 0; wStart = 0; }
		if (uwrap) g_upos = 0;
	}
	g_vpos = vStart; g_ipos = iStart; g_wpos = wStart;

	open_batch();
	DkCmdBuf cmd = cur_cmd();
	if (g_need_tex_barrier || g_ncopy) {
		dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
		for (int i = 0; i < g_ncopy; i++) {
			ViewEntry *v = g_copy_views[i];
			TargetEntry *te = v->te;
			DkImageView sv, dv;
			dkImageViewDefaults(&sv, &te->img);
			dkImageViewDefaults(&dv, &v->img);
			DkImageRect sr = { v->x * (uint32_t)te->sx, v->y * (uint32_t)te->sy, 0,
			                   v->w * (uint32_t)te->sx, v->h * (uint32_t)te->sy, 1 };
			DkImageRect dr = { 0, 0, 0, v->w * (uint32_t)te->sx, v->h * (uint32_t)te->sy, 1 };
			dkCmdBufCopyImage(cmd, &sv, &sr, &dv, &dr, 0);
		}
		if (g_ncopy) {
			dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
		}
		g_ncopy = 0;
		g_need_tex_barrier = 0;
		g_desc_dirty = 0;
	} else if (g_desc_dirty) {
		dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
		g_desc_dirty = 0;
	}
	if (g_scene_need_bind) {
		DkImageView cv, dv;
		DkImageView const *cptr[1] = { NULL };
		if (g_scene_te) { dkImageViewDefaults(&cv, &g_scene_te->img); cptr[0] = &cv; }
		if (g_scene_de) dkImageViewDefaults(&dv, &g_scene_de->img);
		dkCmdBufBindRenderTargets(cmd, g_scene_te ? cptr : NULL, g_scene_te ? 1u : 0u, g_scene_de ? &dv : NULL);
		if (g_scene_de && g_scene_first_draw && !(g_scene_tgt.ds_data && g_scene_tgt.ds_force_load)) {
			float bz = g_scene_tgt.ds_data ? g_scene_tgt.ds_background_depth : 1.0f;
			uint8_t bs = g_scene_tgt.ds_data ? g_scene_tgt.ds_background_stencil : 0;
			dkCmdBufClearDepthStencil(cmd, true, bz, 0xff, bs);
		}
		g_scene_first_draw = 0;
		g_scene_need_bind = 0;
	}

	/* Upload vertex streams / packed PA records */
	uint64_t tvf0 = us_now();
	UsseDrawUniforms vu, fu;
	memcpy(vu.sa, vdraw.sa, sizeof vu.sa);
	memset(&vu.viewport_z, 0, sizeof vu - offsetof(UsseDrawUniforms, viewport_z));
	memcpy(fu.sa, fdraw.sa, sizeof fu.sa);
	memset(&fu.viewport_z, 0, sizeof fu - offsetof(UsseDrawUniforms, viewport_z));
	if (use_vf) {
		vu.vfetch[0] = lo;
		vu.vfetch[1] = (uint32_t)d->nstream;
		vu.vfetch[3] = 0x3f800000u;
		for (int s = 0; s < d->nstream && s < 16; s++) {
			if (!d->stream[s] || !st_ext_ptr[s]) continue;
			int is_ist = hw_inst && d->stream_src[s] >= 2;
			uint32_t bytes = is_ist ? ((inst_cnt - 1u) * d->stride[s] + st_ext_ptr[s]) : ((hi - lo) * d->stride[s] + st_ext_ptr[s]);
			uint32_t src_addr = is_ist ? d->stream[s] : (d->stream[s] + lo * d->stride[s]);
			uint32_t wcnt = align_up(bytes, 4) / 4u;
			uint32_t off = g_vpos;
			g_vpos += wcnt;
			memcpy((uint32_t *)(g_ring_cpu + g_voff) + off, G(src_addr), bytes);
			vu.stream[s][0] = off - vStart;
			vu.stream[s][1] = d->stride[s];
		}
		if (!is_points) {
			void *idst = (uint32_t *)(g_ring_cpu + g_ioff) + iStart;
			if (raw_idx) {
				if (!d->index_format) {
					use_idx16 = 1;
					memcpy(idst, G(d->indices), (size_t)idraw_cnt * 2u);
					g_ipos = iStart + (idraw_cnt + 1u) / 2u;
				} else {
					memcpy(idst, G(d->indices), (size_t)idraw_cnt * 4u);
					g_ipos = iStart + idraw_cnt;
				}
			} else {
				uint32_t *ip = (uint32_t *)idst;
				for (uint32_t t = 0; t < ntri; t++) {
					uint32_t pos[3];
					gfx_prim_tri(d->prim, t, pos);
					for (int k = 0; k < 3; k++) {
						uint32_t p = pos[k];
						ip[3 * t + k] = d->indices ? (d->index_format ? RD32(d->indices + 4 * p) : RD16(d->indices + 2 * p)) : p;
					}
				}
				g_ipos = iStart + nverts;
			}
		}
	} else {
		uint32_t *vdst = (uint32_t *)(g_ring_cpu + g_voff) + vStart;
		uint32_t rec[160];
		GfxVertexPacker pk;
		gfx_vertex_packer_init(&pk, d, vl, npa);
		pk_slow = pk.slow;
		if (is_points) {
			if (vs_pts == 2) {
				for (uint32_t i = 0; i < d->count; i++) {
					uint32_t ix = d->indices ? (d->index_format ? RD32(d->indices + 4 * i) : RD16(d->indices + 2 * i)) : i;
					gfx_vertex_pack(&pk, ix, 0, rec);
					memcpy(vdst, rec, (size_t)npa * 4u);
					vdst += npa;
				}
				g_vpos = vStart + d->count * (uint32_t)npa;
			} else {
				for (uint32_t i = 0; i < d->count; i++) {
					uint32_t ix = d->indices ? (d->index_format ? RD32(d->indices + 4 * i) : RD16(d->indices + 2 * i)) : i;
					gfx_vertex_pack(&pk, ix, 0, rec);
					for (int c = 0; c < 6; c++) {
						memcpy(vdst, rec, (size_t)npa * 4u);
						vdst += npa;
					}
				}
				g_vpos = vStart + nverts * (uint32_t)npa;
			}
		} else if (indexed_pack) {
			for (uint32_t v = lo; v <= hi; v++) {
				gfx_vertex_pack(&pk, v, 0, rec);
				memcpy(vdst, rec, (size_t)npa * 4u);
				vdst += npa;
			}
			g_vpos = vStart + nv_span * (uint32_t)npa;
			uint32_t *idst = (uint32_t *)(g_ring_cpu + g_ioff) + iStart;
			for (uint32_t t = 0; t < ntri; t++) {
				uint32_t pos[3];
				gfx_prim_tri(d->prim, t, pos);
				for (int k = 0; k < 3; k++) {
					uint32_t p = pos[k];
					uint32_t ix = d->index_format ? RD32(d->indices + 4 * p) : RD16(d->indices + 2 * p);
					idst[3 * t + k] = ix - lo;
				}
			}
			g_ipos = iStart + nverts;
		} else {
			for (uint32_t t = 0; t < ntri; t++) {
				uint32_t pos[3];
				gfx_prim_tri(d->prim, t, pos);
				for (int k = 0; k < 3; k++) {
					uint32_t p = pos[k];
					uint32_t ipos = gfx_draw_index_pos(d, p);
					uint32_t inst = gfx_draw_instance(d, p);
					uint32_t ix = d->indices ? (d->index_format ? RD32(d->indices + 4 * ipos) : RD16(d->indices + 2 * ipos)) : ipos;
					gfx_vertex_pack(&pk, ix, inst, rec);
					memcpy(vdst, rec, (size_t)npa * 4u);
					vdst += npa;
				}
			}
			g_vpos = vStart + nverts * (uint32_t)npa;
		}
	}
	{
		uint64_t dvf = us_now() - tvf0;
		g_costs.t_vfetch += dvf;
		if (dvf > g_costs.t_vfetch_max) g_costs.t_vfetch_max = dvf;
		static uint32_t vf_seen[16];
		static int vf_nseen, vf_big;
		int log_it = 0;
		if (dvf > 20000 && vf_big < 16) { vf_big++; log_it = 1; }
		else if (dvf > 5000 && vf_nseen < 16) {
			int found = 0;
			for (int k = 0; k < vf_nseen; k++) if (vf_seen[k] == vs->hash) { found = 1; break; }
			if (!found) { vf_seen[vf_nseen++] = vs->hash; log_it = 1; }
		}
		if (log_it) {
			fprintf(stderr, "    vfetch slow: %.1f ms vprog %#x (hash %08x) count %u ntri %u lo %u hi %u use_vf %d idx_pack %d pts %d/%d slow %d vwords %u inst %u wrap %u\n",
			        dvf / 1e3, d->vprog, vs->hash, d->count, ntri, lo, hi, use_vf, indexed_pack, is_points, vs_pts, pk_slow, g_vpos - vStart, inst_cnt, d->index_wrap);
		}
	}

	/* Canonical varyings & sRGB fixup metadata (vertex keeps vfetch[2..3] = {0, 1.0f}) */
	if (!use_vf) vu.vfetch[3] = 0x3f800000u;
	fu.vfetch[2] = usse_spirv_vary_ncomp(vl, 0);
	fu.vfetch[3] = usse_spirv_vary_ncomp(vl, 1);
	vu.tex_srgb[0] = vsrgb_dec; vu.tex_srgb[1] = vsrgb_enc;
	fu.tex_srgb[0] = srgb_dec;  fu.tex_srgb[1] = srgb_enc;

	/* LDST windows */
	for (int k = 0; k < vs->nldst && k < 8; k++) {
		uint32_t b = vdraw.sa[vs->ldst_sa[k] & 255].u;
		if (!b || b > 0xFFFFFFFFu - USSE_LDST_WINDOW) continue;
		uint32_t off = g_wpos;
		g_wpos += USSE_LDST_WINDOW / 4u;
		memcpy((uint32_t *)(g_ring_cpu + g_woff) + off, G(b), USSE_LDST_WINDOW);
		vu.win[k][0] = b; vu.win[k][1] = USSE_LDST_WINDOW; vu.win[k][2] = off - wStart;
	}

	int vz_on = d->viewport_set && !d->viewport_disabled;
	vu.viewport_z[0] = fu.viewport_z[0] = vz_on ? d->vp_zoff : 0.0f;
	vu.viewport_z[1] = fu.viewport_z[1] = vz_on ? d->vp_zscale : 1.0f;
	if (is_points) {
		float kx = vz_on ? fabsf(d->vp_xscale) : 1.0f;
		float ky = vz_on ? fabsf(d->vp_yscale) : 1.0f;
		int ydown = d->viewport_set && !d->viewport_disabled ? d->vp_yscale > 0 : 1;
		int xleft = d->viewport_set && !d->viewport_disabled ? d->vp_xscale < 0 : 0;
		vu.viewport_z[2] = 0.5f * (xleft ? -1.0f : 1.0f) / (kx > 0 ? kx : 1.0f);
		vu.viewport_z[3] = 0.5f * (ydown ? 1.0f : -1.0f) / (ky > 0 ? ky : 1.0f);
		float sz = d->point_line_width ? (float)d->point_line_width : 1.0f;
		float flp = d->polygon_mode == 2 ? 1.0f : 0.0f;
		memcpy(&vu.vfetch[0], &sz, 4);
		memcpy(&vu.vfetch[1], &flp, 4);
	}

	for (int i = 0; i < fs->nsmp && i < 16; i++) {
		int u = fs->smp_units[i];
		if (u >= 0 && u < 16 && d->tex[u].data) fu.lod_bias[u] = GFX_TEX_LOD_BIAS(d->tex[u].format);
	}
	for (int i = 0; i < vs->nsmp && i < 16; i++) {
		int u = vs->smp_units[i];
		if (u >= 0 && u < 16 && d->vtex[u].data) vu.lod_bias[u] = GFX_TEX_LOD_BIAS(d->vtex[u].format);
	}

	/* Framebuffer fetch copy if needed */
	if (fs->fetch == 1 && g_scene_te) {
		if (!g_scene_te->fetch_mem) {
			alloc_image_block(g_scene_te->stride * (uint32_t)g_scene_sx, g_scene_te->height * (uint32_t)g_scene_sy,
			                  kind_format(g_scene_te->kind), DkImageFlags_Usage2DEngine, DkImageType_2D, 1, 1,
			                  &g_scene_te->fetch_img, &g_scene_te->fetch_mem);
			g_scene_te->fetch_desc_id = register_img_desc(&g_scene_te->fetch_img, DkImageType_2D, 1, 1);
		}
		DkImageView sv, dv;
		dkImageViewDefaults(&sv, &g_scene_te->img);
		dkImageViewDefaults(&dv, &g_scene_te->fetch_img);
		DkImageRect rc = { 0, 0, 0, g_scene_te->stride * (uint32_t)g_scene_sx, g_scene_te->height * (uint32_t)g_scene_sy, 1 };
		dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
		dkCmdBufCopyImage(cmd, &sv, &rc, &dv, &rc, 0);
		dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
		g_desc_dirty = 0;
		GfxTexture pt = { .min_filter = 0, .mag_filter = 0, .u_mode = 2, .v_mode = 2 };
		ftex[n_ftex++] = dkMakeTextureHandle((uint32_t)g_scene_te->fetch_desc_id, (uint32_t)get_sampler(&pt));
		if (g_desc_dirty) { dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors); g_desc_dirty = 0; }
	}

	/* Push uniforms (skip uncached ring write + rebind when unchanged in batch) */
	if (!g_st.valid || !g_st.u_valid || memcmp(&vu, &g_st.last_vu, sizeof vu) != 0) {
		uint32_t vu_off = g_upos;
		g_upos += ustride;
		memcpy(g_ring_cpu + g_uoff + vu_off, &vu, sizeof vu);
		dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, g_ring_gpu + g_uoff + vu_off, sizeof vu);
		g_st.last_vu = vu;
		g_st.last_vu_off = vu_off;
	}
	if (!g_st.valid || !g_st.u_valid || memcmp(&fu, &g_st.last_fu, sizeof fu) != 0) {
		uint32_t fu_off = g_upos;
		g_upos += ustride;
		memcpy(g_ring_cpu + g_uoff + fu_off, &fu, sizeof fu);
		dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, g_ring_gpu + g_uoff + fu_off, sizeof fu);
		g_st.last_fu = fu;
		g_st.last_fu_off = fu_off;
	}
	g_st.u_valid = 1;

	/* Bind shaders & fixed state with per-batch shadow deduplication */
	if (!g_st.valid || g_st.vs != &vs->shader || g_st.fs != &fs->shader) {
		DkShader const *shs[2] = { &vs->shader, &fs->shader };
		dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shs, 2);
		g_st.vs = &vs->shader; g_st.fs = &fs->shader;
	}

	DkRasterizerState rs; memset(&rs, 0, sizeof rs); dkRasterizerStateDefaults(&rs);
	rs.frontFace = DkFrontFace_CCW;
	rs.cullMode = is_points ? DkFace_None : d->cull == 1 ? DkFace_Back : d->cull == 2 ? DkFace_Front : DkFace_None;
	float db_units = (float)d->depth_bias_units;
	float db_factor = (float)d->depth_bias_factor * (float)g_scene_sx;
	if (d->depth_bias_factor || d->depth_bias_units) rs.depthBiasEnableMask = 7;
	if (!g_st.valid || memcmp(&rs, &g_st.rs, sizeof rs)) {
		dkCmdBufBindRasterizerState(cmd, &rs);
		g_st.rs = rs;
	}
	if (rs.depthBiasEnableMask && (!g_st.valid || g_st.db_units != db_units || g_st.db_factor != db_factor)) {
		dkCmdBufSetDepthBias(cmd, db_units, 0.0f, db_factor);
		g_st.db_units = db_units; g_st.db_factor = db_factor;
	}

	DkColorState cs; memset(&cs, 0, sizeof cs); dkColorStateDefaults(&cs);
	DkBlendState bs; memset(&bs, 0, sizeof bs); dkBlendStateDefaults(&bs);
	if (fzero) {
		dkColorStateSetBlendEnable(&cs, 0, true);
		dkBlendStateSetOps(&bs, DkBlendOp_Add, DkBlendOp_Add);
		if (fzero <= 2) dkBlendStateSetFactors(&bs, DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One, fzero == 2 ? DkBlendFactor_Zero : DkBlendFactor_One);
		else dkBlendStateSetFactors(&bs, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha, fzero == 4 ? DkBlendFactor_One : DkBlendFactor_Zero, fzero == 4 ? DkBlendFactor_InvSrcAlpha : DkBlendFactor_One);
	} else if (d->blend.present && (d->blend.color_func || d->blend.alpha_func)) {
		dkColorStateSetBlendEnable(&cs, 0, true);
		dkBlendStateSetOps(&bs, conv_blend_op(d->blend.color_func), conv_blend_op(d->blend.alpha_func));
		dkBlendStateSetFactors(&bs, conv_factor(d->blend.color_src), conv_factor(d->blend.color_dst), conv_factor(d->blend.alpha_src), conv_factor(d->blend.alpha_dst));
	}
	DkColorWriteState cws; memset(&cws, 0, sizeof cws); dkColorWriteStateDefaults(&cws);
	uint8_t cmask = d->fp_disabled ? 0 : (d->blend.present ? d->blend.mask : 0xf);
	dkColorWriteStateSetMask(&cws, 0, ((cmask & 1) ? DkColorMask_R : 0) | ((cmask & 2) ? DkColorMask_G : 0) | ((cmask & 4) ? DkColorMask_B : 0) | ((cmask & 8) ? DkColorMask_A : 0));
	if (!g_st.valid || memcmp(&cs, &g_st.cs, sizeof cs)) { dkCmdBufBindColorState(cmd, &cs); g_st.cs = cs; }
	if (!g_st.valid || memcmp(&bs, &g_st.bs, sizeof bs)) { dkCmdBufBindBlendStates(cmd, 0, &bs, 1); g_st.bs = bs; }
	if (!g_st.valid || memcmp(&cws, &g_st.cws, sizeof cws)) { dkCmdBufBindColorWriteState(cmd, &cws); g_st.cws = cws; }

	int ds_depth_ok = !g_scene_tgt.ds_data || (g_scene_tgt.ds_format & 0x00044000u) != 0;
	int ds_stencil_ok = !g_scene_tgt.ds_data || (g_scene_tgt.ds_format & 0x00022000u) != 0;
	DkDepthStencilState dss; memset(&dss, 0, sizeof dss); dkDepthStencilStateDefaults(&dss);
	if (g_scene_de) {
		dss.depthTestEnable = ds_depth_ok ? true : false;
		dss.depthCompareOp = conv_cmp(d->depth_func);
		dss.depthWriteEnable = (ds_depth_ok && d->depth_write != 0) ? true : false;
		dss.stencilTestEnable = ds_stencil_ok && (d->stencil_func != 7 || d->stencil_fail || d->depth_fail || d->depth_pass);
		dss.stencilFrontCompareOp = dss.stencilBackCompareOp = conv_cmp(d->stencil_func);
		dss.stencilFrontFailOp = dss.stencilBackFailOp = conv_sop(d->stencil_fail);
		dss.stencilFrontDepthFailOp = dss.stencilBackDepthFailOp = conv_sop(d->depth_fail);
		dss.stencilFrontPassOp = dss.stencilBackPassOp = conv_sop(d->depth_pass);
	} else {
		dss.depthTestEnable = false;
		dss.depthWriteEnable = false;
	}
	if (!g_st.valid || memcmp(&dss, &g_st.dss, sizeof dss)) { dkCmdBufBindDepthStencilState(cmd, &dss); g_st.dss = dss; }
	if (dss.stencilTestEnable && (!g_st.valid || g_st.st_wmask != d->stencil_wmask || g_st.st_ref != d->stencil_ref || g_st.st_cmask != d->stencil_cmask)) {
		dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, d->stencil_wmask, d->stencil_ref, d->stencil_cmask);
		g_st.st_wmask = d->stencil_wmask; g_st.st_ref = d->stencil_ref; g_st.st_cmask = d->stencil_cmask;
	}

	uint32_t vw = g_scene_tgt.width ? g_scene_tgt.width : (g_scene_te ? g_scene_te->width : 960);
	uint32_t vh = g_scene_te ? g_scene_te->height : (g_scene_de ? g_scene_de->height : 544);
	DkViewport vp2 = {0};
	if (d->viewport_set && !d->viewport_disabled) {
		vp2.x = (d->vp_xoff - d->vp_xscale) * (float)g_scene_sx;
		vp2.y = (d->vp_yoff + d->vp_yscale) * (float)g_scene_sy;
		vp2.width = (2.0f * d->vp_xscale) * (float)g_scene_sx;
		vp2.height = (-2.0f * d->vp_yscale) * (float)g_scene_sy;
		vp2.near = 0.0f; vp2.far = 1.0f;
	} else if (d->viewport_set) {
		vp2 = (DkViewport){ -(float)g_scene_sx, -(float)g_scene_sy, 2.0f * (float)g_scene_sx, 2.0f * (float)g_scene_sy, 0.0f, 1.0f };
	} else {
		vp2 = (DkViewport){ 0.0f, 0.0f, (float)(vw * (uint32_t)g_scene_sx), (float)(vh * (uint32_t)g_scene_sy), 0.0f, 1.0f };
	}
	if (!g_st.valid || memcmp(&vp2, &g_st.vp, sizeof vp2)) { dkCmdBufSetViewports(cmd, 0, &vp2, 1); g_st.vp = vp2; }

	int sx0 = 0, sy0 = 0, sx1 = (int)vw, sy1 = (int)vh;
	if (d->clip_mode == 0x80000000u) {
		if ((int)d->clip_xmin > sx0) sx0 = (int)d->clip_xmin;
		if ((int)d->clip_ymin > sy0) sy0 = (int)d->clip_ymin;
		if ((int)d->clip_xmax + 1 < sx1) sx1 = (int)d->clip_xmax + 1;
		if ((int)d->clip_ymax + 1 < sy1) sy1 = (int)d->clip_ymax + 1;
		if (sx1 < sx0) sx1 = sx0; if (sy1 < sy0) sy1 = sy0;
	}
	DkScissor sc = { (uint32_t)(sx0 * g_scene_sx), (uint32_t)(sy0 * g_scene_sy),
	                 (uint32_t)((sx1 - sx0) * g_scene_sx), (uint32_t)((sy1 - sy0) * g_scene_sy) };
	if (!g_st.valid || memcmp(&sc, &g_st.sc, sizeof sc)) { dkCmdBufSetScissors(cmd, 0, &sc, 1); g_st.sc = sc; }
	g_st.valid = 1;

	uint32_t vlen = align_up((g_vpos - vStart) * 4u, 256u);
	if (!vlen) vlen = 256u;
	uint32_t wlen = align_up((g_wpos - wStart) * 4u, 256u);
	if (!wlen) wlen = 256u;
	DkBufExtents sb[2] = {
		{ g_ring_gpu + g_voff + vStart * 4u, vlen },
		{ g_ring_gpu + g_woff + wStart * 4u, wlen }
	};
	dkCmdBufBindStorageBuffers(cmd, DkStage_Vertex, 0, sb, vs->nldst ? 2u : 1u);
	if (n_ftex > 0) dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, ftex, (uint32_t)n_ftex);
	if (vs->nsmp > 0) dkCmdBufBindTextures(cmd, DkStage_Vertex, 0, vtex, (uint32_t)vs->nsmp);

	if ((use_vf && !is_points) || indexed_pack) {
		uint32_t vcnt = (use_vf && hw_inst) ? d->index_wrap : nverts;
		uint32_t icnt = (use_vf && hw_inst) ? inst_cnt : 1u;
		dkCmdBufBindIdxBuffer(cmd, use_idx16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, g_ring_gpu + g_ioff + iStart * 4u);
		dkCmdBufDrawIndexed(cmd, DkPrimitive_Triangles, vcnt, icnt, 0, 0, 0);
	} else {
		dkCmdBufDraw(cmd, DkPrimitive_Triangles, nverts, 1, 0, 0);
	}
	if (g_scene_te) {
		if (!g_scene_te->gpu_newer) {
			uint32_t lo_a = g_scene_te->addr, hi_a = lo_a + g_scene_te->guest_bytes;
			for (int i = 0; i < g_ntargets; i++) {
				TargetEntry *o = &g_targets[i];
				if (o != g_scene_te && o->gpu_newer && o->addr < hi_a && o->addr + o->guest_bytes > lo_a)
					o->gpu_newer = 0;
			}
		}
		g_scene_te->gpu_newer = 1;
		g_scene_te->gen = ++g_write_clock;
	}
	__atomic_add_fetch(&n_gpu_draws, 1, __ATOMIC_RELAXED);
	g_costs.t_total += us_now() - td0;
}

void gfx_gpu_end_scene(void) {
	if (!ensure_gpu()) gfx_soft_end_scene();
}

void gfx_gpu_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
	if (!ensure_gpu()) { gfx_soft_present(base, width, height, pitch, format); return; }
	static uint64_t pres_count;
	pres_count++;

	TargetEntry *src = NULL;
	for (int i = 0; i < g_ntargets; i++) {
		if (g_targets[i].addr == base && g_targets[i].gpu_newer) { src = &g_targets[i]; break; }
	}
	if (src && format == 0) {
		uint32_t pw = width * (uint32_t)src->sx;
		uint32_t ph = height * (uint32_t)src->sy;
		int idx = g_pres_idx;
		g_pres_idx = (g_pres_idx + 1) % PRES_NUM;
		if (g_pres_busy[idx]) {
			dkFenceWait(&g_pres_fence[idx], -1);
			g_pres_busy[idx] = 0;
		}
		if (!g_pres_mem[idx] || g_pres_w[idx] != pw || g_pres_h[idx] != ph) {
			wait_all_slots();
			dkQueueWaitIdle(g_queue);
			if (g_pres_mem[idx]) dkMemBlockDestroy(g_pres_mem[idx]);
			alloc_image_block(pw, ph, DkImageFormat_RGBA8_Unorm,
			                  DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
			                  DkImageType_2D, 1, 1, &g_pres_img[idx], &g_pres_mem[idx]);
			g_pres_w[idx] = pw; g_pres_h[idx] = ph;
		}
		DkImageView sv, dv;
		dkImageViewDefaults(&sv, &src->img);
		dkImageViewDefaults(&dv, &g_pres_img[idx]);
		DkImageRect sr = { 0, 0, 0, pw, ph, 1 };
		DkImageRect dr = { 0, 0, 0, pw, ph, 1 };
		open_batch();
		DkCmdBuf cmd = cur_cmd();
		dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
		dkCmdBufBlitImage(cmd, &sv, &sr, &dv, &dr, DkBlitFlag_ModeBlit, 0);
		dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
		DkFence pres_fence;
		submit_batch(0, &pres_fence);
		g_pres_fence[idx] = pres_fence;
		g_pres_busy[idx] = 1;
		static long df_want = -1;
		if (df_want < 0) { const char *e = getenv("RECOMP_DUMP_FRAME"); df_want = e && *e ? atol(e) : 0; }
		int do_dump = df_want > 0 && pres_count == (uint64_t)df_want;
		if (do_dump && (size_t)pw * ph * 4u <= READBACK_POOL_SZ) {
			dkFenceWait(&pres_fence, -1);
			g_pres_busy[idx] = 0;
			open_batch();
			DkCopyBuf cb = { g_readback_gpu, 0, 0 };
			DkImageRect cr = { 0, 0, 0, pw, ph, 1 };
			dkCmdBufCopyImageToBuffer(cur_cmd(), &dv, &cr, &cb, 0);
			submit_batch(1, NULL);
			char dp[128];
			const char *hm = getenv("RECOMP_HOME");
			snprintf(dp, sizeof dp, "%s/frame_%llu.ppm", hm ? hm : ".", (unsigned long long)pres_count);
			FILE *df = fopen(dp, "wb");
			if (df) {
				fprintf(df, "P6\n%u %u\n255\n", pw, ph);
				for (uint32_t i = 0; i < pw * ph; i++) fwrite(g_readback_cpu + 4u * i, 1, 3, df);
				fclose(df);
				fprintf(stderr, "deko3d: dumped %s (%ux%u)\n", dp, pw, ph);
			}
		}
		window_present_deko(&g_pres_img[idx], pw, ph, &pres_fence);
		gfx_soft_skip_window = 1;
	} else if (g_batch_open) {
		submit_batch(0, NULL);
	}
	gfx_soft_present(base, width, height, pitch, format);
	g_frame_seq++;
	g_costs.nshaders = (uint32_t)g_nshaders;
	g_costs_last = g_costs;
	memset(&g_costs, 0, sizeof g_costs);
}

/*
 * gxp_precompile.c: the game's GXP shader programs, translated once and dumped as the SPIR-V
 * inputs tools/build_dksh.py turns into .dksh -- RECOMP_PRECOMPILE_SPV=<dir>, two halves:
 *
 * At gfx init, gxp_precompile_maybe: the on-disk half. gxp_index.c (linked into both builds, only
 * the desktop recipes ever start it) scans every file under app0 for GXP headers and reads the lot.
 * It finds only the programs stored raw -- SSD keeps all but its boot pair compressed inside
 * stream.dat, so this half is mostly empty for it (Killzone: all 3070).
 *
 * At registration, gxp_precompile_register (gxm_sony.c's sceGxmShaderPatcherRegisterProgram hook):
 * a program's bytes are real only now, whatever container they came out of -- hashed for the dedup
 * (the patcher ring re-registers), then the same variants. Content that merely LOADS is captured,
 * draws never need to happen, and each capture survives on disk for every later boot.
 *
 * The variants either half translates are exactly gfx_deko3d.c's get_deko_shader for a draw of the
 * program's layout-independent keys:
 *   vertex: the PA-record fallback (l00000000) any draw can use, point modes p0/p1/p2 -- a miss on
 *           the fast vf-plan variant (its l<hash> key, a per-draw vertex format) falls back to it
 *           on the GPU at full speed, so uncompiled content stops crawling whatever it draws;
 *   fragment: kind 0 (RGBA8, TK_U8), no snap, p0/p1 -- canonical varyings are on for deko3d, so a
 *           fragment module depends on its own program alone -- plus its predicted cube mask
 *           (usse_spirv_predict_cube: the units the program's own SMPs read as cube maps).
 * A module whose sampler units the draw's SA mapping decides (usse_spirv_sa_read nonzero after the
 * translate) is skipped: the game binds those, so they still come from a play's own
 * shaders_missing/ dump. The whole set is the one input build_dksh.py already reads: one capture
 * run, sync the .dksh into shaders/, and no revisit of that content costs a software frame again.
 */
#include "gxp_index.h"
#include "gxp_precompile.h"
#include "platform.h"
#include "rt.h"
#include "usse.h"
#include "usse_spirv.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern uint32_t guest_alloc(uint32_t size, uint32_t align);

static const char *spv_dir(void) { const char *d = getenv("RECOMP_PRECOMPILE_SPV"); return d && *d ? d : 0; }

static int dump_variants(const UsseProgram *prog, uint32_t hash, const char *dir, int *sadept, int *nfail) {
	const int is_frag = usse_is_fragment(prog);
	const UsseVertexLayout *vl = is_frag ? 0 : usse_vertex_layout(prog);
	const UsseFragLayout *fl = is_frag ? usse_fragment_layout(prog) : 0;
	uint32_t sa0[256];
	memset(sa0, 0, sizeof sa0);
	uint32_t cubes[2]; int ncube = 1;   /* its own SMPs' cube units, when it has any: the env-map material's c0000000c, predicted */
	cubes[0] = 0;
	cubes[1] = usse_spirv_predict_cube(prog, fl, sa0);
	if (cubes[1]) ncube = 2;
	int ndump = 0;
	for (int ci = 0; ci < ncube; ci++) {
		int kind = is_frag ? 0 : -1;
		uint32_t cube = cubes[ci], snap = 0;
		usse_spirv_key(prog, fl, 0, &kind, &cube, &snap);   /* as a base draw: no SA, so the key stays what was passed */
		if (!is_frag) kind = -1;
		const int npts = is_frag ? 2 : 3;   /* vertex: p0, p2 (points probing its vf plan first), then p1; fragment: p0/p1 */
		for (int points = 0; points < npts; points++) {
			int smp_units[16], nsmp = 0;
			usse_spirv_set_points(points);
			usse_spirv_set_fetch_zero(0);
			SpirvModule m = usse_spirv_translate(prog, vl, fl, sa0, 16, smp_units, &nsmp, kind < 0 ? 0 : kind, cube, snap, 0);
			usse_spirv_set_points(0);
			usse_spirv_set_fetch_zero(0);
			uint32_t sard[8];
			usse_spirv_sa_read(sard);
			int sa_dep = 0;
			for (int k = 0; k < 8; k++) if (sard[k]) { sa_dep = 1; break; }
			if (!m.ok) (*nfail)++;
			else if (sa_dep) (*sadept)++;   /* the game's draw decides its sampler units: a play's dump, not ours */
			else {
				char key[96], p[1100];   /* get_deko_shader's key, the layout-independent variants */
				snprintf(key, sizeof key, "%08x_%c_k%d_c%08x_s%08x_l%08x_p%d_z%d_f%d",
				         hash, is_frag ? 'f' : 'v', kind, cube, snap, 0, points, 0, 0);
				snprintf(p, sizeof p, "%s/%s.spv", dir, key);
				FILE *f = fopen(p, "wb");
				if (f) { fwrite(m.words, 4, m.nwords, f); fclose(f); }
				else fprintf(stderr, "gxp precompile: fopen(%s) failed errno %d\n", p, errno);
				snprintf(p, sizeof p, "%s/%s.meta", dir, key);
				f = fopen(p, "w");
				if (f) { fprintf(f, "%d %d 0\n", is_frag, nsmp); fclose(f); }
				ndump++;
			}
			usse_spirv_free(&m);
		}
	}
	return ndump;
}

static int dump_one(uint32_t scratch, const uint8_t *b, uint32_t sz, const char *dir, int *sadept, int *nfail) {
	memcpy(G(scratch), b, sz < 128 ? 128 : sz);
	UsseProgram *prog = usse_decode_uncached(scratch);
	if (!prog) { (*nfail)++; return 0; }
	int n = dump_variants(prog, usse_program_hash(prog), dir, sadept, nfail);
	usse_program_free(prog);
	return n;
}

/* the registration hook's state: the dedup (the patcher ring re-registers programs) and the
 * early ones, held until the backend's init has set the translator defaults its own draws build
 * with (gxp_precompile_maybe runs right after them): a module built before that would not be the
 * one get_deko_shader builds later, so those programs wait for the flush at init's end */
static pthread_mutex_t reg_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t seen[8192];
static int nseen, defaults_set;
static uint32_t early[64];
static int nearly;

static void register_one(uint32_t prog_addr) {   /* reg_m held */
	uint32_t size;
	uint32_t hash = usse_content_hash(prog_addr, &size);
	if (!hash) return;
	for (int i = 0; i < nseen; i++) if (seen[i] == hash) return;
	if (nseen < (int)(sizeof seen / sizeof seen[0])) seen[nseen++] = hash;
	const char *dir = spv_dir();
	if (!dir) return;
	int sadept = 0, failed = 0;
	const UsseProgram *prog = usse_get(prog_addr);
	int nd = prog ? dump_variants(prog, hash, dir, &sadept, &failed) : 0;
	fprintf(stderr, "gxp precompile: registered %08x: %d modules, %d draw-dependent (left to a play's dump), %d untranslated\n",
	        hash, nd, sadept, failed);
}

void gxp_precompile_register(uint32_t prog_addr) {
	if (!spv_dir() || !prog_addr || memcmp(G(prog_addr), "GXP", 3)) return;
	pthread_mutex_lock(&reg_m);
	if (!defaults_set) {   /* before the backend's translator defaults: held for the init's flush */
		if (nearly < (int)(sizeof early / sizeof early[0])) early[nearly++] = prog_addr;
		else fprintf(stderr, "gxp precompile: early-registration backlog full, %#x left to a later re-registration\n", prog_addr);
		pthread_mutex_unlock(&reg_m);
		return;
	}
	register_one(prog_addr);
	pthread_mutex_unlock(&reg_m);
}

void gxp_precompile_maybe(void) {
	pthread_mutex_lock(&reg_m);
	defaults_set = 1;   /* the backend's translator defaults are set (this call is right after them): flush what registered before */
	for (int i = 0; i < nearly; i++) register_one(early[i]);
	nearly = 0;
	pthread_mutex_unlock(&reg_m);
	const char *dir = spv_dir();
	if (!dir) return;
	const char *fs = getenv("RECOMP_FS");
	char app0[1024];
	snprintf(app0, sizeof app0, "%s/app0", fs && *fs ? fs : "fs");
	mkdir(dir, 0777);
	fprintf(stderr, "gxp precompile: indexing every GXP under %s -> %s\n", app0, dir);
	gxp_index_start(app0, dir, 0, 0);
	while (!gxp_index_ready()) plat_sleep_ns(20 * 1000 * 1000ull);   /* once: a few seconds; programs.idx caches it after */
	const int n = gxp_index_count();
	uint32_t scratch = guest_alloc(2u << 20, 64);
	int dumped = 0, sadept = 0, failed = 0;
	if (!scratch) fprintf(stderr, "gxp precompile: no guest scratch\n");
	for (int i = 0; scratch && i < n; i++) {
		uint32_t sz;
		const uint8_t *b = gxp_index_program(gxp_index_hash_at(i), &sz);
		if (!b || sz > (2u << 20)) continue;
		dumped += dump_one(scratch, b, sz, dir, &sadept, &failed);
	}
	fprintf(stderr, "gxp precompile: %d programs on disk, %d modules dumped, %d draw-dependent (left to a play's dump), %d untranslated -> %s\n",
	        n, dumped, sadept, failed, dir);
}

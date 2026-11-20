/*
 * Nintendo Switch entry point (libnx): initializes Switch services (romfs, sockets, nxlink, csrng),
 * resolves the game's base folder from argv[0], reads its launch.ini (image, entry, RECOMP_* --
 * written by dev/switch-install.py), and hands control to recomp_host_main().
 */
#include <switch.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int recomp_host_main(int argc, char **argv);
void *recomp_lookup(uint32_t addr);

static int nxlink_sock = -1;

void userAppInit(void) {
	romfsInit();
	socketInitializeDefault();
	nxlink_sock = nxlinkStdio();
	csrngInitialize();
}

void switch_tune_thread(const char *nm, int vita_prio, int is_main) {
	(void)vita_prio;
	int is_host_main = is_main && nm && !strcmp(nm, "host-main");
	int is_disp = nm && strstr(nm, "DisplayQueue");
	int is_bagger = nm && strstr(nm, "Task_Bagger");
	int is_gamedata = nm && strstr(nm, "Gamedata");
	int is_audio = nm && (strstr(nm, "Audio") || strstr(nm, "Ngs") || strstr(nm, "AK::"));
	int is_io = nm && (strstr(nm, "Streamer") || strstr(nm, "Fios"));
	int is_bg = nm && (strstr(nm, "Rankings") || strstr(nm, "NEAR") || strstr(nm, "DRM") || strstr(nm, "Trophy"));
	u32 nx_prio = is_host_main ? 38 : is_disp ? 39 : is_main ? 40 : is_bagger ? 41 : is_gamedata ? 43 : is_audio ? 44 : is_io ? 45 : is_bg ? 52 : 46;
	s32 core;
	if (is_host_main) core = 2;
	else if (is_main) core = 0;
	else if (is_bagger) {
		static int tb_idx;
		core = (__atomic_fetch_add(&tb_idx, 1, __ATOMIC_RELAXED) & 1) ? 2 : 1;
	} else if (is_gamedata) {
		static int gd_idx;
		core = (__atomic_fetch_add(&gd_idx, 1, __ATOMIC_RELAXED) & 1) ? 2 : 1;
	} else if (is_io) core = 1;
	else core = 2;
	svcSetThreadPriority(CUR_THREAD_HANDLE, nx_prio);
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
}

void userAppExit(void) {
	csrngExit();
	if (nxlink_sock >= 0) close(nxlink_sock);
	socketExit();
	romfsExit();
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

static int ini_matches_nro(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[1024];
	int ok = 0;
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line), *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		if (!strcmp(trim(s), "entry")) {
			uint32_t ent = (uint32_t)strtoul(trim(eq + 1), NULL, 16);
			ok = ent && recomp_lookup(ent & ~1u) != NULL;
			break;
		}
	}
	fclose(f);
	return ok;
}

int main(int argc, char **argv) {
	/* the game's folder is the one the .nro sits in (dev/switch-install.py lays it out); its launch.ini says what to run */
	static char home[1024] = "sdmc:/switch/nativ";
	if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
		snprintf(home, sizeof home, "%s", argv[0]);
		char *sl = strrchr(home, '/');
		if (sl) *sl = 0;
	} else {
		/* Ryujinx direct-.nro launch passes no directory in argv[0]: match subdir launch.ini via NRO entry symbol */
		char probe[1100];
		snprintf(probe, sizeof probe, "%s/launch.ini", home);
		if (access(probe, F_OK) != 0) {
			DIR *d = opendir("sdmc:/switch");
			if (d) {
				struct dirent *de;
				char fallback[1024] = "";
				while ((de = readdir(d))) {
					if (de->d_name[0] == '.') continue;
					snprintf(probe, sizeof probe, "sdmc:/switch/%s/launch.ini", de->d_name);
					if (access(probe, F_OK) != 0) continue;
					if (!fallback[0]) snprintf(fallback, sizeof fallback, "sdmc:/switch/%s", de->d_name);
					if (ini_matches_nro(probe)) { snprintf(home, sizeof home, "sdmc:/switch/%s", de->d_name); fallback[0] = 0; break; }
				}
				closedir(d);
				if (fallback[0]) snprintf(home, sizeof home, "%s", fallback);
			}
		}
	}
	setenv("RECOMP_HOME", home, 0);
	setenv("RECOMP_GPU_NO_TEX_WATCH", "1", 0);
	setenv("RECOMP_ONLINE", "0", 0);

	if (nxlink_sock < 0) {
		char logp[1100];
		snprintf(logp, sizeof logp, "%s/nativ.log", home);
		FILE *lf = freopen(logp, "w", stderr);
		if (lf) setvbuf(stderr, NULL, _IOLBF, 0);
	}
	switch_tune_thread("host-main", 160, 1);

	static char image[1100], entry[64] = "", budget[64] = "18446744073709551615";
	snprintf(image, sizeof image, "%s/recomp/game.img", home);
	setenv("RECOMP_FS", home, 0);
	char shdir[1100];
	snprintf(shdir, sizeof shdir, "%s/shaders", home);
	setenv("RECOMP_DKSH_DIR", shdir, 0);

	char ini[1100];
	snprintf(ini, sizeof ini, "%s/launch.ini", home);
	FILE *f = fopen(ini, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof line, f)) {
			char *s = trim(line), *eq = strchr(s, '=');
			if (!*s || *s == '#' || *s == ';' || !eq) continue;
			*eq = 0;
			char *k = trim(s), *v = trim(eq + 1);
			if (!strcmp(k, "image")) snprintf(image, sizeof image, "%s", v);
			else if (!strcmp(k, "entry")) snprintf(entry, sizeof entry, "%s", v);
			else if (!strcmp(k, "budget")) snprintf(budget, sizeof budget, "%s", v);
			else if (!strncmp(k, "RECOMP_", 7)) setenv(k, v, 1);
		}
		fclose(f);
	}
	if (!entry[0] || !getenv("RECOMP_MODULES")) {
		fprintf(stderr, "switch: %s has no entry/RECOMP_MODULES -- stage the game with dev/switch-install.py\n", ini);
		return 1;
	}

	fprintf(stderr, "switch: home=%s image=%s entry=%s fs=%s\n", home, image, entry, getenv("RECOMP_FS"));
	char *av[5];
	int ac = 0;
	av[ac++] = (char *)"nativ";
	av[ac++] = image;
	av[ac++] = entry;
	if (budget[0]) av[ac++] = budget;
	av[ac] = NULL;
	return recomp_host_main(ac, av);
}

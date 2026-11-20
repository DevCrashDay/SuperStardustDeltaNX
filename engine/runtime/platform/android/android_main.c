/*
 * Android's entry point. SDL's activity loads this library and calls SDL_main with no arguments
 * worth having, so the three the host takes on every other platform (the image, the entry address
 * and the import budget) are read from a file instead: launch.ini in the app's external files
 * directory, written by dev/android-install.sh beside the game it pushed. Any other key in it whose
 * name starts with RECOMP_ becomes an environment variable before the host starts, which is where
 * the pack's RUN_ENV and RECOMP_FS (the game's fs/ root: io.c) arrive.
 *
 * Nothing else here is Android-specific: main.c is compiled as recomp_host_main (-Dmain=, see
 * runtime/build.py) and runs unchanged under it.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int recomp_host_main(int argc, char **argv);

/* stdout and stderr go nowhere on Android, and every diagnostic this runtime writes goes there
 * (the STOP:/Fault:/gpu: lines, the hitch reports). Both ends of a pipe are pumped into logcat
 * under the "nativ" tag, line by line, so `adb logcat -s nativ` reads exactly what a terminal would. */
static void *log_pump(void *fd) {
	char buf[1024]; int n, used = 0;
	while ((n = (int)read((int)(intptr_t)fd, buf + used, sizeof buf - 1 - used)) > 0) {
		used += n; buf[used] = 0;
		char *s = buf, *nl;
		while ((nl = strchr(s, '\n'))) { *nl = 0; __android_log_write(ANDROID_LOG_INFO, "nativ", s); s = nl + 1; }
		used = (int)strlen(s); memmove(buf, s, (size_t)used + 1);
		if (used == sizeof buf - 1) { __android_log_write(ANDROID_LOG_INFO, "nativ", buf); used = 0; }   /* a line longer than the buffer: flushed as it is */
	}
	return 0;
}
static void logcat_stdio(void) {
	setvbuf(stdout, 0, _IOLBF, 0); setvbuf(stderr, 0, _IOLBF, 0);
	int p[2];
	if (pipe(p)) return;
	dup2(p[1], STDOUT_FILENO); dup2(p[1], STDERR_FILENO); close(p[1]);
	pthread_t t; pthread_create(&t, 0, log_pump, (void *)(intptr_t)p[0]); pthread_detach(t);
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	logcat_stdio();
	const char *ext = SDL_GetAndroidExternalStoragePath();
	if (!ext) { fprintf(stderr, "android: no external files directory\n"); return 1; }
	char ini[1024]; snprintf(ini, sizeof ini, "%s/launch.ini", ext);
	FILE *f = fopen(ini, "r");
	if (!f) { fprintf(stderr, "android: no %s -- push a game with dev/android-install.sh\n", ini); return 1; }
	static char image[1024], entry[64], budget[64];
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line), *eq = strchr(s, '=');
		if (!*s || *s == '#' || *s == ';' || !eq) continue;
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (!strcmp(k, "image")) snprintf(image, sizeof image, "%s", v);
		else if (!strcmp(k, "entry")) snprintf(entry, sizeof entry, "%s", v);
		else if (!strcmp(k, "budget")) snprintf(budget, sizeof budget, "%s", v);
		/* SDL_ too: SDL reads its hints from the environment, and SDL_VULKAN_LIBRARY is how a
		 * different Vulkan driver (Mesa's Turnip, say) is chosen over the system one */
		else if (!strncmp(k, "RECOMP_", 7) || !strncmp(k, "SDL_", 4)) setenv(k, v, 1);
	}
	fclose(f);
	if (!image[0] || !entry[0]) { fprintf(stderr, "android: %s needs image= and entry=\n", ini); return 1; }
	fprintf(stderr, "android: %s entry %s (fs %s)\n", image, entry, getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs");
	char *av[5]; int ac = 0;
	av[ac++] = (char *)"nativ"; av[ac++] = image; av[ac++] = entry;
	if (budget[0]) av[ac++] = budget;
	av[ac] = 0;
	return recomp_host_main(ac, av);
}

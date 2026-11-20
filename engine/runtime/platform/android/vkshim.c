/*
 * libvkshim.so -- the Vulkan library SDL loads on Android when a replacement GPU driver is shipped
 * with the game (Mesa's Turnip, say, which reports Vulkan 1.3 where a Retroid Pocket Flip2's stock
 * Adreno driver reports 1.1).
 *
 * Why a shim at all. SDL takes a path (SDL_VULKAN_LIBRARY), dlopens it itself and looks up
 * vkGetInstanceProcAddr. A driver like Turnip cannot be opened that way: dlopen runs in the app's own
 * linker namespace, which may load only the platform's public libraries, and the driver needs
 * libhardware.so, which is not one of them -- so the load fails and SDL_GPU finds no backend at all.
 * libadrenotools opens it in a namespace that can see /system/lib64 and /vendor/lib64 (its
 * linkernsbypass), but hands back a handle, not a path. This library is the join: SDL opens THIS,
 * which only needs public libraries, and it opens the real driver and forwards.
 *
 * It finds everything itself, so nothing has to tell it where it lives: dladdr gives this library's
 * own path, whose directory is the app's nativeLibraryDir -- which is both where adrenotools' hooks
 * must be (its documented requirement) and where dev/android-install.sh puts the driver.
 * RECOMP_VULKAN_DRIVER names the driver if it is not libvulkan_freedreno.so (launch.ini
 * passes RECOMP_ keys through as environment variables).
 *
 * If anything here fails, adrenotools falls back to the system driver and the game still runs; the
 * reason goes to the log either way.
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <adrenotools/driver.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "nativ", __VA_ARGS__)

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_GetInstanceProcAddr)(void *instance, const char *name);

static PFN_GetInstanceProcAddr real_gipa;

static void load_driver(void) {
	static int tried;
	if (tried) return;
	tried = 1;

	Dl_info info;
	char dir[PATH_MAX];
	if (!dladdr((void *)&load_driver, &info) || !info.dli_fname) { LOG("vkshim: dladdr found no path for this library"); return; }
	snprintf(dir, sizeof dir, "%s", info.dli_fname);
	char *slash = strrchr(dir, '/');
	if (!slash) { LOG("vkshim: %s has no directory", info.dli_fname); return; }
	slash[1] = 0;   /* adrenotools wants the directories with their trailing slash */

	const char *name = getenv("RECOMP_VULKAN_DRIVER");
	if (!name || !*name) name = "libvulkan_freedreno.so";

	/* RECOMP_VULKAN_DRIVER_DIR: where the driver itself is, when it is not shipped in the package --
	 * gfx/vkdriver.c points this at the drivers/ folder beside the game's files, so a player can add
	 * a newer Turnip without a new package. The HOOK directory stays this library's own, which is
	 * what adrenotools documents and what the package always has. Loading from outside the package
	 * is why adrenotools is needed at all: it maps the driver through a memfd rather than dlopening
	 * a path Android would refuse. */
	const char *drvdir = getenv("RECOMP_VULKAN_DRIVER_DIR");
	char drvbuf[PATH_MAX];
	if (drvdir && *drvdir) {
		snprintf(drvbuf, sizeof drvbuf, "%s%s", drvdir, drvdir[strlen(drvdir) - 1] == '/' ? "" : "/");
		drvdir = drvbuf;
	} else drvdir = dir;

	/* tmpLibDir is only used below API 29 (memfd is used instead), and no file redirection or memory
	 * import is wanted here: the driver is shipped in the package, not supplied by the player. */
	void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, dir, drvdir, name, NULL, NULL);
	if (!h) { LOG("vkshim: adrenotools could not open %s from %s (%s)", name, drvdir, dlerror() ? dlerror() : "no reason given"); return; }
	real_gipa = (PFN_GetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
	if (!real_gipa) { LOG("vkshim: %s has no vkGetInstanceProcAddr", name); return; }
	/* This handle is Android's own loader, always: adrenotools opens /system/lib64/libvulkan.so in a
	 * namespace where its hook intercepts the loader's dlopen of the vendor driver and substitutes
	 * the custom one. So getting a handle says nothing about which driver won -- the hook does that
	 * swap later, logs it under the "hook_impl" tag, and FALLS BACK TO THE SYSTEM DRIVER quietly if
	 * the custom one will not map. What the game actually got is in the gpu banner at startup, which
	 * names the driver as well as the GPU ("Adreno (TM) 650 / turnip Mesa ..." against
	 * "... / Qualcomm Technologies Inc. Adreno Vulkan Driver"). */
	LOG("vkshim: handing %s to Android's loader, driver from %s", name, drvdir);
}

/* the one symbol SDL asks this library for; every other entry point is reached through it */
PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *name) {
	load_driver();
	return real_gipa ? real_gipa(instance, name) : NULL;
}

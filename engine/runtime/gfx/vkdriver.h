/*
 * Which Vulkan driver the game runs on (settings key "vulkan_driver"; the overlay's Display page).
 * See vkdriver.c.
 */
#ifndef VKDRIVER_H
#define VKDRIVER_H

#define VKDRIVER_NAME_MAX 64
#define VKDRIVER_MAX 6

/* where a replaceable driver is looked for, or "" where none can be: the player drops .so files
 * there and the menu lists them */
const char *vkdriver_dir(void);
/* the drivers found there, by file name, newest-looking last (plain alphabetical). Returns how many,
 * at most `max`. Zero means the menu offers the system driver alone. */
int vkdriver_list(char (*names)[VKDRIVER_NAME_MAX], int max);
/* the setting -> the environment SDL and the loader shim read. Called once at startup, after the
 * settings are loaded and BEFORE the window opens: a driver is chosen when the Vulkan library is
 * first opened, so changing it afterwards would mean tearing down every pipeline and texture. */
void vkdriver_apply(void);
/* what this run actually got: a driver's file name, or "system" */
const char *vkdriver_current(void);

#endif

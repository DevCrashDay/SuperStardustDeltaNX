/* platform.h on Nintendo Switch (libnx / Horizon OS) */
#include "platform.h"
#include "switch_compat.h"
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Guest 32-bit address space inside Horizon OS StackRegion:
 * Horizon's StackRegion is ~1.875 GiB (0x8000000 + 0x78000000) and svcMapMemory
 * requires destinations in StackRegion (svcMapPhysicalMemory fails with 0xfa01 in NROs
 * without SystemResourceSize). Vita only populates:
 *   [0x10000000 .. 0x50000000)  1 GiB   guest heap -> arena + 0x00000000
 *   [0x80000000 .. 0x90000000)  256 MiB ELF/img    -> arena + 0x40000000
 *   [0xfff00000 .. 0x100000000) 1 MiB   high slots -> arena + 0x50000000
 *   arena + [0x50100000 .. 0x54000000)  extra 1-MiB segments on demand
 */
#define STACK_ARENA_SIZE  0x54000000ULL
#define STACK_ARENA_PAGES (STACK_ARENA_SIZE >> 12)

uintptr_t recomp_segs[4096];
static uint8_t *arena_base;
static uint64_t extra_off = 0x50100000ULL;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t mapped_pages[(STACK_ARENA_PAGES + 7) / 8];

static inline int page_is_mapped(uint32_t ap) { return (mapped_pages[ap >> 3] >> (ap & 7)) & 1; }
static inline void page_mark_mapped(uint32_t ap) { mapped_pages[ap >> 3] |= (uint8_t)(1u << (ap & 7)); }

void *plat_reserve(size_t bytes) {
	if (bytes >= (1ull << 32)) {
		virtmemLock();
		void *p = virtmemFindStack(STACK_ARENA_SIZE, 0x1000);
		if (p && !virtmemAddReservation(p, STACK_ARENA_SIZE)) p = NULL;
		virtmemUnlock();
		fprintf(stderr, "switch: plat_reserve(%#zx -> %#llx) -> %p\n",
			bytes, (unsigned long long)STACK_ARENA_SIZE, p);
		if (p) {
			arena_base = (uint8_t *)p;
			extra_off = 0x50100000ULL;
			memset(mapped_pages, 0, sizeof mapped_pages);
			memset(recomp_segs, 0, sizeof recomp_segs);
			uintptr_t b = (uintptr_t)arena_base;
			for (uint32_t s = 0x100; s < 0x500; s++) recomp_segs[s] = b - 0x10000000ULL;
			for (uint32_t s = 0x800; s < 0x900; s++) recomp_segs[s] = (b + 0x40000000ULL) - 0x80000000ULL;
			recomp_segs[0xfff] = (b + 0x50000000ULL) - 0xfff00000ULL;
		}
		return p;
	}
	return calloc(1, bytes);
}

static uintptr_t host_for_page(uint32_t gp) {
	uint32_t seg = gp >> 8;
	if (!recomp_segs[seg]) {
		if (extra_off + (1ULL << 20) > STACK_ARENA_SIZE) {
			fprintf(stderr, "switch: extra 1MiB arena exhausted for guest %#x\n", gp << 12);
			exit(1);
		}
		recomp_segs[seg] = (uintptr_t)arena_base + extra_off - ((uintptr_t)seg << 20);
		extra_off += (1ULL << 20);
	}
	return recomp_segs[seg] + ((uintptr_t)gp << 12);
}

uint32_t plat_host_to_guest(const void *p) {
	if (!p || !arena_base) return 0;
	uintptr_t a = (uintptr_t)p, b = (uintptr_t)arena_base;
	if (a < b || a >= b + STACK_ARENA_SIZE) return 0;
	uintptr_t off = a - b;
	if (off < 0x40000000ULL) return (uint32_t)(0x10000000ULL + off);
	if (off < 0x50000000ULL) return (uint32_t)(0x80000000ULL + (off - 0x40000000ULL));
	if (off < 0x50100000ULL) return (uint32_t)(0xfff00000ULL + (off - 0x50000000ULL));
	for (uint32_t s = 0; s < 4096; s++)
		if (recomp_segs[s] && a >= recomp_segs[s] + ((uintptr_t)s << 20) && a < recomp_segs[s] + (((uintptr_t)s + 1) << 20))
			return (uint32_t)(a - recomp_segs[s]);
	return 0;
}

void plat_map_guest(uint32_t gaddr, uint32_t size) {
	if (!size || !arena_base) return;
	uint32_t p0 = gaddr >> 12;
	uint32_t p1 = (uint32_t)(((uint64_t)gaddr + size - 1) >> 12);

	pthread_mutex_lock(&map_lock);
	uint32_t p = p0;
	while (p <= p1) {
		uintptr_t dst0 = host_for_page(p);
		uint32_t ap0 = (uint32_t)((dst0 - (uintptr_t)arena_base) >> 12);
		if (page_is_mapped(ap0)) { p++; continue; }
		uint32_t r0 = p;
		p++;
		while (p <= p1) {
			uintptr_t dstn = host_for_page(p);
			if (dstn != dst0 + ((uintptr_t)(p - r0) << 12)) break;
			uint32_t apn = (uint32_t)((dstn - (uintptr_t)arena_base) >> 12);
			if (page_is_mapped(apn)) break;
			p++;
		}
		size_t run_pages = p - r0;
		size_t run_bytes = run_pages << 12;
		void *src = memalign(0x1000, run_bytes);
		if (!src) {
			fprintf(stderr, "switch: memalign(%zu) failed for guest %#x+%#x\n", run_bytes, r0 << 12, size);
			pthread_mutex_unlock(&map_lock);
			exit(1);
		}
		memset(src, 0, run_bytes);
		Result rc = svcMapMemory((void *)dst0, src, run_bytes);
		if (R_FAILED(rc)) {
			fprintf(stderr, "switch: svcMapMemory(%p, %p, %#zx) failed: %#x\n", (void *)dst0, src, run_bytes, rc);
			pthread_mutex_unlock(&map_lock);
			exit(1);
		}
		for (size_t q = 0; q < run_pages; q++) page_mark_mapped(ap0 + (uint32_t)q);
	}
	pthread_mutex_unlock(&map_lock);
}

void plat_thread_name(char *buf, size_t n) {
	if (n) buf[0] = 0;
}

int plat_protect(void *p, size_t bytes, int writable) {
	(void)p; (void)bytes; (void)writable;
	return -1;
}

size_t plat_page_size(void) { return 0x1000; }
void plat_add_fault_hook(PlatFaultHook hook) { (void)hook; }
void plat_set_crash_handler(PlatCrashFn fn) { (void)fn; }
void plat_backtrace(FILE *f) { (void)f; }

int plat_cpu_count(void) { return 3; }

double plat_cpu_seconds(void) {
	return (double)armTicksToNs(armGetSystemTick()) / 1e9;
}

uint64_t plat_now_us(void) {
	return armTicksToNs(armGetSystemTick()) / 1000u;
}

void plat_sleep_ns(uint64_t ns) {
	svcSleepThread((s64)ns);
}

void plat_random(void *buf, size_t n) {
	if (R_FAILED(csrngGetRandomBytes(buf, n))) {
		uint64_t t = armGetSystemTick();
		uint8_t *b = (uint8_t *)buf;
		for (size_t i = 0; i < n; i++) {
			t = t * 6364136223846793005ULL + 1442695040888963407ULL;
			b[i] = (uint8_t)(t >> 56);
		}
	}
}

/* ---- POSIX helpers missing in newlib for libnx ---- */
static pthread_mutex_t io_seek_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t pread(int fd, void *buf, size_t n, off_t off) {
	pthread_mutex_lock(&io_seek_lock);
	off_t cur = lseek(fd, 0, SEEK_CUR);
	if (cur == (off_t)-1 || lseek(fd, off, SEEK_SET) == (off_t)-1) {
		pthread_mutex_unlock(&io_seek_lock);
		return -1;
	}
	ssize_t r = read(fd, buf, n);
	int saved = errno;
	lseek(fd, cur, SEEK_SET);
	errno = saved;
	pthread_mutex_unlock(&io_seek_lock);
	return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
	pthread_mutex_lock(&io_seek_lock);
	off_t cur = lseek(fd, 0, SEEK_CUR);
	if (cur == (off_t)-1 || lseek(fd, off, SEEK_SET) == (off_t)-1) {
		pthread_mutex_unlock(&io_seek_lock);
		return -1;
	}
	ssize_t r = write(fd, buf, n);
	int saved = errno;
	lseek(fd, cur, SEEK_SET);
	errno = saved;
	pthread_mutex_unlock(&io_seek_lock);
	return r;
}

char *realpath(const char *path, char *resolved) {
	if (!path) { errno = EINVAL; return NULL; }
	struct stat st;
	if (stat(path, &st) != 0) return NULL;
	char *out = resolved ? resolved : (char *)malloc(PATH_MAX);
	if (!out) { errno = ENOMEM; return NULL; }
	/* Normalize '.' and '..' segments while preserving devkitPro device prefix (e.g. sdmc:/, romfs:/) */
	const char *colon = strchr(path, ':');
	size_t devlen = colon ? (size_t)(colon - path + 1) : 0;
	char tmp[PATH_MAX];
	if (devlen) {
		snprintf(tmp, sizeof tmp, "%s", path);
	} else if (path[0] == '/') {
		snprintf(tmp, sizeof tmp, "%s", path);
	} else {
		char cwd[PATH_MAX];
		if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
		snprintf(tmp, sizeof tmp, "%s/%s", cwd, path);
	}
	char *segs[128];
	int nseg = 0;
	char prefix[64] = "";
	char *body = tmp;
	if (devlen && devlen < sizeof prefix) {
		memcpy(prefix, tmp, devlen);
		prefix[devlen] = 0;
		body = tmp + devlen;
	}
	char *save = NULL;
	for (char *tok = strtok_r(body, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, ".")) continue;
		if (!strcmp(tok, "..")) { if (nseg > 0) nseg--; continue; }
		if (nseg < 128) segs[nseg++] = tok;
	}
	size_t pos = 0;
	if (prefix[0]) pos += (size_t)snprintf(out + pos, PATH_MAX - pos, "%s", prefix);
	if (nseg == 0) {
		snprintf(out + pos, PATH_MAX - pos, "/");
	} else {
		for (int i = 0; i < nseg && pos < PATH_MAX; i++)
			pos += (size_t)snprintf(out + pos, PATH_MAX - pos, "/%s", segs[i]);
	}
	return out;
}

time_t timegm(struct tm *tm) {
	int y = tm->tm_year + 1900;
	int m = tm->tm_mon + 1;
	int d = tm->tm_mday;
	y -= (m <= 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	int64_t days = era * 146097 + (int64_t)doe - 719468;
	return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

int waitpid(int pid, int *status, int opts) { (void)pid; (void)status; (void)opts; errno = ENOSYS; return -1; }
int execvp(const char *file, char *const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }

int flock(int fd, int op) { (void)fd; (void)op; return 0; }

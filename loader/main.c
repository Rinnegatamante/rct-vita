/* main.c -- RollerCoaster Tycoon Classic .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2023 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>
#include <fmod/fmod.h>
#include <semaphore.h>

#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fnmatch.h>

#include <png.h>
#include <zip.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"
#include "unzip.h"

//#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define dlog printf
#else
#define dlog
#endif

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static char data_path[256];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int enable_dlcs = 0;

long sysconf(int name) {
	return 0;
}

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

#if 1
void *__wrap_calloc(uint32_t nmember, uint32_t size) { return vglCalloc(nmember, size); }
void __wrap_free(void *addr) { vglFree(addr); };
void *__wrap_malloc(uint32_t size) { return vglMalloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return vglMemalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return vglRealloc(ptr, size); };
#else
void *__wrap_calloc(uint32_t nmember, uint32_t size) { return __real_calloc(nmember, size); }
void __wrap_free(void *addr) { __real_free(addr); };
void *__wrap_malloc(uint32_t size) { return __real_malloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return __real_memalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return __real_realloc(ptr, size); };	
#endif

void __wrap_abort() {
	sceClibPrintf("abort called from %p\n", __builtin_return_address(0));
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;
int sceLibcHeapSize = 32 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module main_mod, stl_mod;

int ret4() { return 4; }

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = memalign(alignment, size);
	return 0;
}

int debugPrintf(char *text, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/rct.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#include "pthr.h"

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;
extern void *__aeabi_ul2d;
extern void *__aeabi_f2ulz;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	sceClibPrintf("throwing %s\n", *str);
}

int rename_hook(const char *fname, const char *fname2) {
#ifdef ENABLE_DEBUG
	sceClibPrintf("rename %s -> %s\n", fname, fname2);
#endif
	char real_fname[256], real_fname2[256];
	char *src = fname, *dst = fname2;
	if (strncmp(fname, "ux0:", 4)) {
		if (fname[0] == '.')
			sprintf(real_fname, "%s/%s", data_path, &fname[2]);
		else
			sprintf(real_fname, "%s/%s", data_path, fname);
		src = real_fname;
	}
	if (strncmp(fname2, "ux0:", 4)) {
		if (fname2[0] == '.')
			sprintf(real_fname2, "%s/%s", data_path, &fname2[2]);
		else
			sprintf(real_fname2, "%s/%s", data_path, fname2);
		dst = real_fname2;
	}
	return rename(src, dst);
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
#ifdef ENABLE_DEBUG
	sceClibPrintf("fopen(%s,%s)\n", fname, mode);
#endif
	if (strncmp(fname, "ux0:", 4)) {
		if (fname[0] == '.')
			sprintf(real_fname, "%s/%s", data_path, &fname[2]);
		else
			sprintf(real_fname, "%s/%s", data_path, fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode) {
	int f;
	char real_fname[256];
#ifdef ENABLE_DEBUG
	sceClibPrintf("open(%s)\n", fname);
#endif
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname);
		f = open(real_fname, flags, mode);
	} else {
		f = open(fname, flags, mode);
	}
	return f;
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int chk_guard = 0x42424242;
static int *__stack_chk_guard_fake = &chk_guard;

static FILE __sF_fake[0x1000][3];

typedef struct __attribute__((__packed__)) stat64_bionic {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long __pad4;
} stat64_bionic;

inline void stat_newlib_to_stat_bionic(const struct stat * src, stat64_bionic * dst)
{
    if (!src) return;
    if (!dst) dst = malloc(sizeof(stat64_bionic));

    dst->st_dev = src->st_dev;
    dst->st_ino = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_uid = src->st_uid;
    dst->st_gid = src->st_gid;
    dst->st_rdev = src->st_rdev;
    dst->st_size = src->st_size;
    dst->st_blksize = src->st_blksize;
    dst->st_blocks = src->st_blocks;
    dst->st_atime = src->st_atime;
    dst->st_atime_nsec = 0;
    dst->st_mtime = src->st_mtime;
    dst->st_mtime_nsec = 0;
    dst->st_ctime = src->st_ctime;
    dst->st_ctime_nsec = 0;
}

int stat_hook(const char *_pathname, void *statbuf) {
	//sceClibPrintf("stat(%s)\n", _pathname);
	struct stat st;
    int res = stat(_pathname, &st);

    if (res == 0)
        stat_newlib_to_stat_bionic(&st, statbuf);

    return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
    int res = fstat(fd, &st);
    if (res == 0)
        stat_newlib_to_stat_bionic(&st, statbuf);

    return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void *sceClibMemclr(void *dst, SceSize len) {
	if (!dst) {
		sceClibPrintf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

void abort_hook() {
	//dlog("ABORT CALLED!!!\n");
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

static so_default_dynlib gl_hook[] = {
	{"glPixelStorei", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	char real_fname[256];
	//sceClibPrintf("opendir(%s)\n", dirname);
	SceUID uid;
	if (strncmp(dirname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, dirname);
		uid = sceIoDopen(real_fname);
	} else {
		uid = sceIoDopen(dirname);
	}
	

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

uint64_t lseek64(int fd, uint64_t offset, int whence) {
	return lseek(fd, offset, whence);
}

EGLBoolean eglGetConfigs(EGLDisplay display, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
	*num_config = 0;
	return GL_FALSE;
}

uint64_t current_timestamp_ms() {
    struct timeval te;
    gettimeofday(&te, NULL);
    return (te.tv_sec*1000LL + te.tv_usec/1000);
}

void str_remove(char *str, const char *sub) {
    char *p, *q, *r;

    if (*sub && (q = r = strstr(str, sub)) != NULL) {
        size_t len = strlen(sub);
        while ((r = strstr(p = r + len, sub)) != NULL) {
            while (p < r)
                *q++ = *p++;
        }
        while ((*q++ = *p++) != '\0')
            continue;
    }
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}

extern void *_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_;
extern void *_ZN4FMOD6Studio4Bank6unloadEv;
extern void *_ZN4FMOD5Sound11getUserDataEPPv;
extern void *_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_;
extern void *_ZN4FMOD5Sound15getNumSubSoundsEPi;
extern void *_ZN4FMOD5Sound11getSubSoundEiPPS0_;
extern void *_ZN4FMOD12ChannelGroup12getNumGroupsEPi;
extern void *_ZN4FMOD12ChannelGroup8getGroupEiPPS0_;
extern void *_ZN4FMOD12ChannelGroup14getNumChannelsEPi;
extern void *_ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE;
extern void *_ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE;
extern void *_ZN4FMOD14ChannelControl4stopEv;
extern void *_ZN4FMOD5Sound11setUserDataEPv;
extern void *_ZN4FMOD5Sound7releaseEv;
extern void *_ZN4FMOD6System10getChannelEiPPNS_7ChannelE;
extern void *_ZN4FMOD6Studio6System6updateEv;
extern void *_ZN4FMOD6Studio4Bank7isValidEv;
extern void *_ZN4FMOD6Studio4Bank15getLoadingStateEP25FMOD_STUDIO_LOADING_STATE;
extern void *_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE;
extern void *_ZN4FMOD5Sound15getSystemObjectEPPNS_6SystemE;
extern void *_ZN4FMOD6System21getMasterChannelGroupEPPNS_12ChannelGroupE;
extern void *_ZN4FMOD6Studio16EventDescription16unloadSampleDataEv;
extern void *_ZN4FMOD6Studio16EventDescription21getSampleLoadingStateEP25FMOD_STUDIO_LOADING_STATE;
extern void *_ZN4FMOD6Studio16EventDescription14loadSampleDataEv;
extern void *_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE;
extern void *_ZN4FMOD5Sound9getLengthEPjj;
extern void *_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i;
extern void *_ZN4FMOD6Studio6System10lookupPathEPK9FMOD_GUIDPciPi;
extern void *_ZN4FMOD6Studio6System6createEPPS1_j;
extern void *_ZN4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE;
extern void *_ZN4FMOD6System11setCallbackEPF11FMOD_RESULTP11FMOD_SYSTEMjPvS4_S4_Ej;
extern void *_ZN4FMOD6System10getVersionEPj;
extern void *_ZN4FMOD6System19setStreamBufferSizeEjj;
extern void *_ZN4FMOD6System16setDSPBufferSizeEji;
extern void *_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi;
extern void *_ZN4FMOD6Studio6System10initializeEijjPv;
extern void *_ZN4FMOD6Studio6System7releaseEv;
extern void *_ZN4FMOD6Studio6System13flushCommandsEv;
extern void *_ZN4FMOD6Studio6System12getBankCountEPi;
extern void *_ZN4FMOD6Studio4Bank15getLoadingStateEP25FMOD_STUDIO_LOADING_STATE;
extern void *_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE;
extern void *_ZN4FMOD5Sound15getSystemObjectEPPNS_6SystemE;
extern void *_ZN4FMOD6System21getMasterChannelGroupEPPNS_12ChannelGroupE;
extern void *_ZN4FMOD6Studio6System12getEventByIDEPK9FMOD_GUIDPPNS0_16EventDescriptionE;
extern void *_ZN4FMOD6Studio16EventDescription16unloadSampleDataEv;
extern void *_ZN4FMOD6Studio16EventDescription21getSampleLoadingStateEP25FMOD_STUDIO_LOADING_STATE;
extern void *_ZN4FMOD6Studio16EventDescription14loadSampleDataEv;
extern void *_ZN4FMOD5Sound9getLengthEPjj;
extern void *_ZN4FMOD6Studio6System11getBankListEPPNS0_4BankEiPi;
extern void *_ZN4FMOD6Studio4Bank13getEventCountEPi;
extern void *_ZN4FMOD6Studio4Bank12getEventListEPPNS0_16EventDescriptionEiPi;
extern void *_ZN4FMOD6Studio16EventDescription5getIDEP9FMOD_GUID;
extern void *_ZN4FMOD6Studio4Bank11getBusCountEPi;
extern void *_ZN4FMOD6Studio4Bank10getBusListEPPNS0_3BusEiPi;
extern void *_ZN4FMOD6Studio3Bus5getIDEP9FMOD_GUID;
//extern void *_ZN4FMOD6Studio3Bus13setFaderLevelEf;
extern void *_ZN4FMOD6Studio3Bus7setMuteEb;
extern void *_ZN4FMOD6Studio3Bus9setPausedEb;
extern void *_ZN4FMOD6Studio3Bus7isValidEv;
extern void *_ZN4FMOD6Studio3Bus15getChannelGroupEPPNS_12ChannelGroupE;
extern void *_ZN4FMOD6Studio6System10getBusByIDEPK9FMOD_GUIDPPNS0_3BusE;
extern void *_ZN4FMOD6Studio3Bus18unlockChannelGroupEv;
extern void *_ZN4FMOD6Studio3Bus16lockChannelGroupEv;
extern void *_ZN4FMOD6Studio16EventDescription17getParameterCountEPi;
extern void *_ZN4FMOD6Studio16EventDescription19getParameterByIndexEiP33FMOD_STUDIO_PARAMETER_DESCRIPTION;
extern void *_ZN4FMOD6Studio13EventInstance7isValidEv;
extern void *_ZN4FMOD6Studio13EventInstance9setPausedEb;
extern void *_ZN4FMOD6Studio13EventInstance19setTimelinePositionEi;
extern void *_ZN4FMOD6Studio16EventDescription4is3DEPb;
extern void *_ZN4FMOD6Studio13EventInstance11setUserDataEPv;
extern void *_ZN4FMOD6Studio13EventInstance11setCallbackEPF11FMOD_RESULTjP25FMOD_STUDIO_EVENTINSTANCEPvEj;
extern void *_ZN4FMOD6Studio13EventInstance5startEv;
extern void *_ZN4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE;
extern void *_ZN4FMOD6Studio13EventInstance15set3DAttributesEPK18FMOD_3D_ATTRIBUTES;
extern void *_ZN4FMOD6Studio13EventInstance7releaseEv;
extern void *_ZN4FMOD6Studio13EventInstance9setVolumeEf;
extern void *_ZN4FMOD6Studio16EventDescription9getLengthEPi;
extern void *_ZN4FMOD6Studio13EventInstance16getPlaybackStateEP26FMOD_STUDIO_PLAYBACK_STATE;
extern void *_ZN4FMOD6Studio16EventDescription9isOneshotEPb;
extern void *_ZN4FMOD6Studio16EventDescription15getUserPropertyEPKcP25FMOD_STUDIO_USER_PROPERTY;
extern void *_ZN4FMOD7Channel11setPositionEjj;
extern void *_ZN4FMOD7Channel11getPositionEPjj;
extern void *_ZN4FMOD6Studio13EventInstance4stopE21FMOD_STUDIO_STOP_MODE;
//extern void *_ZN4FMOD6Studio13EventInstance13getCueByIndexEiPPNS0_11CueInstanceE;
extern void *_ZN4FMOD6Studio13EventInstance12getParameterEPKcPPNS0_17ParameterInstanceE;
extern void *_ZN4FMOD6Studio17ParameterInstance8setValueEf;
//extern void *_ZN4FMOD6Studio11CueInstance7triggerEv;
extern void *_ZN4FMOD6Studio13EventInstance19getTimelinePositionEPi;
extern void *_ZN4FMOD6Studio13EventInstance15getChannelGroupEPPNS_12ChannelGroupE;
extern void *_ZN4FMOD14ChannelControl13getAudibilityEPf;
extern void *_ZN4FMOD6Studio13EventInstance17getParameterCountEPi;
extern void *_ZN4FMOD6Studio13EventInstance19getParameterByIndexEiPPNS0_17ParameterInstanceE;
extern void *_ZN4FMOD6Studio17ParameterInstance14getDescriptionEP33FMOD_STUDIO_PARAMETER_DESCRIPTION;
extern void *_ZN4FMOD6Studio16EventDescription7isValidEv;
extern void *_ZN4FMOD6Studio17ParameterInstance7isValidEv;
extern void *_ZN4FMOD6Studio13EventInstance11getUserDataEPPv;
extern void *_ZN4FMOD6Studio6System12getSoundInfoEPKcP22FMOD_STUDIO_SOUND_INFO;
extern void *_ZN4FMOD6Studio6System21setListenerAttributesEiPK18FMOD_3D_ATTRIBUTES;
extern void *_ZN4FMOD6System11mixerResumeEv;
extern void *_ZN4FMOD6System12mixerSuspendEv;
extern void *_ZN4FMOD14ChannelControl9getVolumeEPf;
extern void *_ZN4FMOD14ChannelControl9setVolumeEf;
extern void *_ZN4FMOD14ChannelControl7setMuteEb;
extern void *_ZN4FMOD14ChannelControl9setPausedEb;
extern void *_ZN4FMOD3DSP7releaseEv;
extern void *_ZN4FMOD12ChannelGroup7releaseEv;
extern void *_ZN4FMOD3DSP17setParameterFloatEif;
extern void *_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE;
extern void *_ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE;
extern void *_ZN4FMOD3DSP16setChannelFormatEji16FMOD_SPEAKERMODE;
extern void *_ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE;
extern void *_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE;
extern void *_ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE;
extern void *_ZN4FMOD14ChannelControl7setModeEj;
extern void *_ZN4FMOD14ChannelControl11setUserDataEPv;
extern void *_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E;
extern void *_ZN4FMOD14ChannelControl9isPlayingEPb;
extern void *_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_;
extern void *_ZN4FMOD14ChannelControl11getUserDataEPPv;
extern void *_ZN4FMOD14ChannelControl19setReverbPropertiesEif;
extern void *_ZN4FMOD14ChannelControl6setPanEf;
extern void *_ZN4FMOD14ChannelControl8setPitchEf;
extern void *_ZN4FMOD14ChannelControl19get3DMinMaxDistanceEPfS1_;
extern void *_ZN4FMOD14ChannelControl19set3DMinMaxDistanceEff;
extern void *_ZN4FMOD7Channel12setLoopCountEi;
extern void *_ZN4FMOD14ChannelControl15getSystemObjectEPPNS_6SystemE;
extern void *_ZN4FMOD7Channel15getChannelGroupEPPNS_12ChannelGroupE;
extern void *_ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE;
extern void *_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE;
extern void *_ZN4FMOD5Sound13getLoopPointsEPjjS1_j;
extern void *_ZN4FMOD5Sound16getNumSyncPointsEPi;
extern void *_ZN4FMOD3DSP15setParameterIntEii;
extern void *_ZN4FMOD6System13getNumDriversEPi;
extern void *_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE;
extern void *_ZN4FMOD6System4initEijPv;
extern void *_ZN4FMOD6System6updateEv;
extern void *_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_;
extern void *_ZN4FMOD7Channel12setFrequencyEf;
extern void *_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_;
extern void *_ZN4FMOD14ChannelControl14set3DOcclusionEff;
extern void *_ZN4FMOD7Channel11setPriorityEi;
extern void *_ZN4FMOD6System13set3DSettingsEfff;
extern void *_ZN4FMOD6System5closeEv;
extern void *_ZN4FMOD6System7releaseEv;
extern void *_ZN4FMOD5Sound19set3DMinMaxDistanceEff;
extern void *_ZN4FMOD5Sound7setModeEj;
extern void *_ZN4FMOD6System12createStreamEPKcjP22;
extern void *_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE;
extern void *_ZN4FMOD14ChannelControl12getMixMatrixEPfPiS2_i;
extern void *_ZN4FMOD7Channel12getFrequencyEPf;
extern void *_ZN4FMOD5Sound11getDefaultsEPfPi;

extern FMOD_RESULT F_API __real_FMOD_System_CreateSound(FMOD_SYSTEM *system, const char *name_or_data, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO *exinfo, FMOD_SOUND **sound);
extern FMOD_RESULT F_API __real_FMOD_System_CreateStream(FMOD_SYSTEM *system, const char *name_or_data, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO *exinfo, FMOD_SOUND **sound);

FMOD_RESULT F_API __wrap_FMOD_System_CreateSound(FMOD_SYSTEM *system, const char *name_or_data, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO *exinfo, FMOD_SOUND **sound) {
    char fname[256];
	sprintf(fname, "ux0:data/rct/assets/%s", &name_or_data[22]);
	return __real_FMOD_System_CreateSound(system, fname, mode, exinfo, sound);
}

FMOD_RESULT F_API __wrap_FMOD_System_CreateStream(FMOD_SYSTEM *system, const char *name_or_data, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO *exinfo, FMOD_SOUND **sound) {
    char fname[256];
	sprintf(fname, "ux0:data/rct/assets/%s", &name_or_data[22]);
	return __real_FMOD_System_CreateStream(system, fname, mode, exinfo, sound);
}

extern void *__aeabi_ul2f;

int AAssetDir_close() {
	return 0;
}
int AAssetDir_getNextFileName() {
	return 0;
}

int AAssetManager_openDir() {
	return 0;
}

int nanosleep_soloader (const struct timespec *rqtp,
        __attribute__((unused)) struct timespec *rmtp) {
    if (!rqtp) {
        errno = EFAULT;
        return -1;
    }

    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec > 999999999) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t us = rqtp->tv_sec * 1000000 + (rqtp->tv_nsec+999) / 1000;

    sceKernelDelayThread(us);
    return 0;
}

char *glGetString_hook(GLenum cap) {
	if (cap == GL_VENDOR)
		return "Imagination";
	return glGetString(cap);
}

char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dest_len, size_t src_len) {
	return strncpy(dst, src, n);
}

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

int __vsprintf_chk(char* dest, int flags, size_t dest_len_from_compiler, const char *format, va_list va) {
	return vsprintf(dest, format, va);
}

void *__memmove_chk(void *dest, const void *src, size_t len, size_t dstlen) {
	return memmove(dest, src, len);
}

void *__memset_chk(void *dest, int val, size_t len, size_t dstlen) {
	return memset(dest, val, len);
}

size_t __strlcat_chk (char *dest, char *src, size_t len, size_t dstlen) {
	return strlcat(dest, src, len);
}

size_t __strlcpy_chk (char *dest, char *src, size_t len, size_t dstlen) {
	return strlcpy(dest, src, len);
}

char* __strchr_chk(const char* p, int ch, size_t s_len) {
	return strchr(p, ch);
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
	return strcat(dest, src);
}

char *__strrchr_chk(const char *p, int ch, size_t s_len) {
	return strrchr(p, ch);
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen) {
	return strcpy(dest, src);
}

char *__strncat_chk(char *s1, const char *s2, size_t n, size_t s1len) {
	return strncat(s1, s2, n);
}

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
	return memcpy(dest, src, len);
}

int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *format, va_list args) {
	return vsnprintf(s, maxlen, format, args);
}

void *dlsym_hook( void *handle, const char *symbol);

void __stack_chk_fail_fake() {
	sceClibPrintf("Stack smash on %p\n", __builtin_return_address(0));
}

void glShaderSource_hook(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length) {
	char *s = strstr(*string, "vec2(vIndex,0.0)");
	if (s) {
		char *shd = (char *)malloc(strlen(*string) + 1024);
		shd[0] = 0;
		strncpy(shd, *string, s - *string);
		shd[s - *string] = 0;
		strcat(shd, "vec2(vIndex + (1.0f / 128.0f), 0.0f)");
		strcat(shd, &s[16]);
		glShaderSource(handle, 1, &shd, NULL);
		free(shd);
	} else {
		glShaderSource(handle, count, string, length);
	}
}

static so_default_dynlib default_dynlib[] = {
	{ "glShaderSource", (uintptr_t)&glShaderSource_hook },
	{ "slCreateEngine", (uintptr_t)&slCreateEngine },
	{ "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
	{ "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
	{ "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
	{ "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
	{ "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
	{ "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
	{ "__memcpy_chk", (uintptr_t)&__memcpy_chk },
	{ "__memmove_chk", (uintptr_t)&__memmove_chk },
	{ "__memset_chk", (uintptr_t)&__memset_chk },
	{ "__strcat_chk", (uintptr_t)&__strcat_chk },
	{ "__strchr_chk", (uintptr_t)&__strchr_chk },
	{ "__strcpy_chk", (uintptr_t)&__strcpy_chk },
	{ "__strncpy_chk2", (uintptr_t)&__strncpy_chk2 },
	{ "__strlcat_chk", (uintptr_t)&__strlcat_chk },
	{ "__strlcpy_chk", (uintptr_t)&__strlcpy_chk },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "__strncat_chk", (uintptr_t)&__strncat_chk },
	{ "__strrchr_chk", (uintptr_t)&__strrchr_chk },
	{ "__vsprintf_chk", (uintptr_t)&__vsprintf_chk },
	{ "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk },
	{ "AAssetDir_close", (uintptr_t)&AAssetDir_close },
	{ "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
	{ "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },
	{ "setpriority", (uintptr_t)&ret0 },
	{ "Android_JNI_SetupThread", (uintptr_t)&ret0 },
	{ "pthread_setname_np", (uintptr_t)&ret0 },
	{ "sem_init", (uintptr_t)&sem_init_soloader },
	{ "sem_post", (uintptr_t)&sem_post_soloader },
	{ "sem_timedwait", (uintptr_t)&sem_timedwait_soloader },
	{ "sem_trywait", (uintptr_t)&sem_trywait_soloader },
	{ "sem_wait", (uintptr_t)&sem_wait_soloader },
	{ "sem_destroy", (uintptr_t)&sem_destroy_soloader },
	{ "sem_getvalue", (uintptr_t)&sem_getvalue_soloader },
	{ "eglGetCurrentDisplay", (uintptr_t)&ret0 },
	{ "eglGetConfigs", (uintptr_t)&eglGetConfigs },
	{ "AAssetManager_fromJava", (uintptr_t)&ret0 },
	{ "AAssetManager_open", (uintptr_t)&ret0 },
	{ "AAsset_close", (uintptr_t)&ret0 },
	{ "AAsset_getLength", (uintptr_t)&ret0 },
	{ "AAsset_getRemainingLength", (uintptr_t)&ret0 },
	{ "AAsset_read", (uintptr_t)&ret0 },
	{ "AAsset_seek", (uintptr_t)&ret0 },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri},
	{ "glGetString", (uintptr_t)&glGetString_hook},
	{ "glGetError", (uintptr_t)&ret0},
	{ "glReadPixels", (uintptr_t)&glReadPixels},
	{ "glShaderSource", (uintptr_t)&glShaderSource},
	{ "sincosf", (uintptr_t)&sincosf },
	{ "gmtime", (uintptr_t)&gmtime },
	{ "mktime", (uintptr_t)&mktime },
	{ "ctime", (uintptr_t)&ctime },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&memmove },
	{ "__aeabi_memmove8", (uintptr_t)&memmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&memmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
	{ "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&ret0 },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "exp2f", (uintptr_t)&exp2f },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "feof", (uintptr_t)&feof },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgetc", (uintptr_t)&fgetc },
	{ "fgets", (uintptr_t)&fgets },
	{ "floor", (uintptr_t)&floor },
	{ "fileno", (uintptr_t)&fileno },
	{ "floorf", (uintptr_t)&floorf },
	{ "fnmatch", (uintptr_t)&fnmatch },
	{ "fminf", (uintptr_t)&fminf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "funopen", (uintptr_t)&funopen },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "open", (uintptr_t)&open_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	{ "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	// { "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat_hook },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isascii", (uintptr_t)&isascii },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "lseek64", (uintptr_t)&lseek64 },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&memmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	//{ "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_soloader },
	{ "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
	//{ "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader },
	//{ "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader },
	{ "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
	{ "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_create", (uintptr_t) &pthread_create_soloader },
	{ "pthread_detach", (uintptr_t) &pthread_detach_soloader },
	{ "pthread_equal", (uintptr_t) &pthread_equal_soloader },
	{ "pthread_exit", (uintptr_t) &pthread_exit },
	//{ "pthread_getattr_np", (uintptr_t) &pthread_getattr_np_soloader },
	{ "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader},
	{ "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
	{ "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader},
	{ "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader},
	//{ "pthread_mutexattr_setpshared", (uintptr_t) &pthread_mutexattr_setpshared_soloader},
	{ "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader},
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t) &pthread_self_soloader },
	{ "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "pread", (uintptr_t)&pread },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "rename", (uintptr_t)&rename_hook },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strtok", (uintptr_t)&strtok },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "round", (uintptr_t)&round },
	{ "lround", (uintptr_t)&lround },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "truncf", (uintptr_t)&truncf },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsscanf", (uintptr_t)&vsscanf },
	{ "vasprintf", (uintptr_t)&vasprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	{ "sigaction", (uintptr_t)&ret0 },
	{ "zlibVersion", (uintptr_t)&zlibVersion },
	// { "writev", (uintptr_t)&writev },
	{ "unlink", (uintptr_t)&unlink },
	{ "raise", (uintptr_t)&raise },
	{ "posix_memalign", (uintptr_t)&posix_memalign },
	{ "swprintf", (uintptr_t)&swprintf },
	{ "wcscpy", (uintptr_t)&wcscpy },
	{ "wcscat", (uintptr_t)&wcscat },
	{ "wcstombs", (uintptr_t)&wcstombs },
	{ "wcsstr", (uintptr_t)&wcsstr },
	{ "compress", (uintptr_t)&compress },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "atof", (uintptr_t)&atof },
	{ "remove", (uintptr_t)&remove },
	{ "__system_property_get", (uintptr_t)&ret0 },
	{ "strnlen", (uintptr_t)&strnlen },
	{ "_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_", (uintptr_t)&_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_ },
	{ "_ZN4FMOD6Studio4Bank6unloadEv", (uintptr_t)&_ZN4FMOD6Studio4Bank6unloadEv },
	{ "_ZN4FMOD5Sound11getUserDataEPPv", (uintptr_t)&_ZN4FMOD5Sound11getUserDataEPPv },
	{ "_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_", (uintptr_t)&_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_ },
	{ "_ZN4FMOD5Sound15getNumSubSoundsEPi", (uintptr_t)&_ZN4FMOD5Sound15getNumSubSoundsEPi },
	{ "_ZN4FMOD5Sound11getSubSoundEiPPS0_", (uintptr_t)&_ZN4FMOD5Sound11getSubSoundEiPPS0_ },
	{ "_ZN4FMOD12ChannelGroup12getNumGroupsEPi", (uintptr_t)&_ZN4FMOD12ChannelGroup12getNumGroupsEPi },
	{ "_ZN4FMOD12ChannelGroup8getGroupEiPPS0_", (uintptr_t)&_ZN4FMOD12ChannelGroup8getGroupEiPPS0_ },
	{ "_ZN4FMOD12ChannelGroup14getNumChannelsEPi", (uintptr_t)&_ZN4FMOD12ChannelGroup14getNumChannelsEPi },
	{ "_ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE", (uintptr_t)&_ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE },
	{ "_ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE", (uintptr_t)&_ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE },
	{ "_ZN4FMOD14ChannelControl4stopEv", (uintptr_t)&_ZN4FMOD14ChannelControl4stopEv },
	{ "_ZN4FMOD5Sound11setUserDataEPv", (uintptr_t)&_ZN4FMOD5Sound11setUserDataEPv },
	{ "_ZN4FMOD5Sound7releaseEv", (uintptr_t)&_ZN4FMOD5Sound7releaseEv },
	{ "_ZN4FMOD6System10getChannelEiPPNS_7ChannelE", (uintptr_t)&_ZN4FMOD6System10getChannelEiPPNS_7ChannelE },
	{ "_ZN4FMOD6Studio6System6updateEv", (uintptr_t)&_ZN4FMOD6Studio6System6updateEv },
	{ "_ZNK4FMOD6Studio4Bank7isValidEv", (uintptr_t)&_ZN4FMOD6Studio4Bank7isValidEv },
	{ "_ZNK4FMOD6Studio4Bank15getLoadingStateEP25FMOD_STUDIO_LOADING_STATE", (uintptr_t)&_ZN4FMOD6Studio4Bank15getLoadingStateEP25FMOD_STUDIO_LOADING_STATE },
	{ "_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE", (uintptr_t)&_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE },
	{ "_ZN4FMOD5Sound15getSystemObjectEPPNS_6SystemE", (uintptr_t)&_ZN4FMOD5Sound15getSystemObjectEPPNS_6SystemE },
	{ "_ZN4FMOD6System21getMasterChannelGroupEPPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD6System21getMasterChannelGroupEPPNS_12ChannelGroupE },
	{ "_ZN4FMOD6Studio16EventDescription16unloadSampleDataEv", (uintptr_t)&_ZN4FMOD6Studio16EventDescription16unloadSampleDataEv },
	{ "_ZNK4FMOD6Studio16EventDescription21getSampleLoadingStateEP25FMOD_STUDIO_LOADING_STATE", (uintptr_t)&_ZN4FMOD6Studio16EventDescription21getSampleLoadingStateEP25FMOD_STUDIO_LOADING_STATE },
	{ "_ZN4FMOD6Studio16EventDescription14loadSampleDataEv", (uintptr_t)&_ZN4FMOD6Studio16EventDescription14loadSampleDataEv },
	{ "FMOD_Memory_GetStats", (uintptr_t)&FMOD_Memory_GetStats },
	{ "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", (uintptr_t)&_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE },
	{ "_ZN4FMOD5Sound9getLengthEPjj", (uintptr_t)&_ZN4FMOD5Sound9getLengthEPjj },
	{ "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i", (uintptr_t)&_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i },
	{ "FMOD_Memory_Initialize", (uintptr_t)&FMOD_Memory_Initialize },
	{ "_ZNK4FMOD6Studio6System10lookupPathEPK9FMOD_GUIDPciPi", (uintptr_t)&_ZN4FMOD6Studio6System10lookupPathEPK9FMOD_GUIDPciPi },
	{ "_ZN4FMOD6Studio6System6createEPPS1_j", (uintptr_t)&_ZN4FMOD6Studio6System6createEPPS1_j },
	{ "_ZNK4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE", (uintptr_t)&_ZN4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE },
	{ "_ZN4FMOD6System11setCallbackEPF11FMOD_RESULTP11FMOD_SYSTEMjPvS4_S4_Ej", (uintptr_t)&_ZN4FMOD6System11setCallbackEPF11FMOD_RESULTP11FMOD_SYSTEMjPvS4_S4_Ej },
	{ "_ZN4FMOD6System10getVersionEPj", (uintptr_t)&_ZN4FMOD6System10getVersionEPj },
	{ "_ZN4FMOD6System19setStreamBufferSizeEjj", (uintptr_t)&_ZN4FMOD6System19setStreamBufferSizeEjj },
	{ "_ZN4FMOD6System16setDSPBufferSizeEji", (uintptr_t)&_ZN4FMOD6System16setDSPBufferSizeEji },
	{ "_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi", (uintptr_t)&_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi },
	{ "_ZN4FMOD6Studio6System10initializeEijjPv", (uintptr_t)&_ZN4FMOD6Studio6System10initializeEijjPv },
	{ "_ZN4FMOD6Studio6System7releaseEv", (uintptr_t)&_ZN4FMOD6Studio6System7releaseEv },
	{ "_ZN4FMOD6Studio6System13flushCommandsEv", (uintptr_t)&_ZN4FMOD6Studio6System13flushCommandsEv },
	{ "_ZNK4FMOD6Studio6System12getBankCountEPi", (uintptr_t)&_ZN4FMOD6Studio6System12getBankCountEPi },
	{ "_ZN4FMOD5Sound9getLengthEPjj", (uintptr_t)&_ZN4FMOD5Sound9getLengthEPjj },
	{ "_ZNK4FMOD6Studio6System11getBankListEPPNS0_4BankEiPi", (uintptr_t)&_ZN4FMOD6Studio6System11getBankListEPPNS0_4BankEiPi },
	{ "_ZNK4FMOD6Studio4Bank13getEventCountEPi", (uintptr_t)&_ZN4FMOD6Studio4Bank13getEventCountEPi },
	{ "_ZNK4FMOD6Studio4Bank12getEventListEPPNS0_16EventDescriptionEiPi", (uintptr_t)&_ZN4FMOD6Studio4Bank12getEventListEPPNS0_16EventDescriptionEiPi },
	{ "_ZNK4FMOD6Studio16EventDescription5getIDEP9FMOD_GUID", (uintptr_t)&_ZN4FMOD6Studio16EventDescription5getIDEP9FMOD_GUID },
	{ "_ZNK4FMOD6Studio4Bank11getBusCountEPi", (uintptr_t)&_ZN4FMOD6Studio4Bank11getBusCountEPi },
	{ "_ZNK4FMOD6Studio4Bank10getBusListEPPNS0_3BusEiPi", (uintptr_t)&_ZN4FMOD6Studio4Bank10getBusListEPPNS0_3BusEiPi },
	{ "_ZNK4FMOD6Studio3Bus5getIDEP9FMOD_GUID", (uintptr_t)&_ZN4FMOD6Studio3Bus5getIDEP9FMOD_GUID },
	{ "_ZN4FMOD6Studio3Bus13setFaderLevelEf", (uintptr_t)&ret0}, //&_ZN4FMOD6Studio3Bus13setFaderLevelEf },
	{ "_ZN4FMOD6Studio3Bus7setMuteEb", (uintptr_t)&_ZN4FMOD6Studio3Bus7setMuteEb },
	{ "_ZN4FMOD6Studio3Bus9setPausedEb", (uintptr_t)&_ZN4FMOD6Studio3Bus9setPausedEb },
	{ "_ZNK4FMOD6Studio3Bus7isValidEv", (uintptr_t)&_ZN4FMOD6Studio3Bus7isValidEv },
	{ "_ZNK4FMOD6Studio3Bus15getChannelGroupEPPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD6Studio3Bus15getChannelGroupEPPNS_12ChannelGroupE },
	{ "_ZNK4FMOD6Studio6System10getBusByIDEPK9FMOD_GUIDPPNS0_3BusE", (uintptr_t)&_ZN4FMOD6Studio6System10getBusByIDEPK9FMOD_GUIDPPNS0_3BusE },
	{ "_ZN4FMOD6Studio3Bus18unlockChannelGroupEv", (uintptr_t)&_ZN4FMOD6Studio3Bus18unlockChannelGroupEv },
	{ "_ZN4FMOD6Studio3Bus16lockChannelGroupEv", (uintptr_t)&_ZN4FMOD6Studio3Bus16lockChannelGroupEv },
	{ "_ZNK4FMOD6Studio16EventDescription17getParameterCountEPi", (uintptr_t)&_ZN4FMOD6Studio16EventDescription17getParameterCountEPi },
	{ "_ZNK4FMOD6Studio16EventDescription19getParameterByIndexEiP33FMOD_STUDIO_PARAMETER_DESCRIPTION", (uintptr_t)&_ZN4FMOD6Studio16EventDescription19getParameterByIndexEiP33FMOD_STUDIO_PARAMETER_DESCRIPTION },
	{ "_ZNK4FMOD6Studio13EventInstance7isValidEv", (uintptr_t)&_ZN4FMOD6Studio13EventInstance7isValidEv },
	{ "_ZN4FMOD6Studio13EventInstance9setPausedEb", (uintptr_t)&_ZN4FMOD6Studio13EventInstance9setPausedEb },
	{ "_ZN4FMOD6Studio13EventInstance19setTimelinePositionEi", (uintptr_t)&_ZN4FMOD6Studio13EventInstance19setTimelinePositionEi },
	{ "_ZNK4FMOD6Studio16EventDescription4is3DEPb", (uintptr_t)&_ZN4FMOD6Studio16EventDescription4is3DEPb },
	{ "_ZN4FMOD6Studio13EventInstance11setUserDataEPv", (uintptr_t)&_ZN4FMOD6Studio13EventInstance11setUserDataEPv },
	{ "_ZN4FMOD6Studio13EventInstance11setCallbackEPF11FMOD_RESULTjP25FMOD_STUDIO_EVENTINSTANCEPvEj", (uintptr_t)&_ZN4FMOD6Studio13EventInstance11setCallbackEPF11FMOD_RESULTjP25FMOD_STUDIO_EVENTINSTANCEPvEj },
	{ "_ZN4FMOD6Studio13EventInstance5startEv", (uintptr_t)&_ZN4FMOD6Studio13EventInstance5startEv },
	{ "_ZNK4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE", (uintptr_t)&_ZN4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE },
	{ "_ZN4FMOD6Studio13EventInstance15set3DAttributesEPK18FMOD_3D_ATTRIBUTES", (uintptr_t)&_ZN4FMOD6Studio13EventInstance15set3DAttributesEPK18FMOD_3D_ATTRIBUTES },
	{ "_ZN4FMOD6Studio13EventInstance7releaseEv", (uintptr_t)&_ZN4FMOD6Studio13EventInstance7releaseEv },
	{ "_ZN4FMOD6Studio13EventInstance9setVolumeEf", (uintptr_t)&_ZN4FMOD6Studio13EventInstance9setVolumeEf },
	{ "_ZNK4FMOD6Studio16EventDescription9getLengthEPi", (uintptr_t)&_ZN4FMOD6Studio16EventDescription9getLengthEPi },
	{ "_ZNK4FMOD6Studio13EventInstance16getPlaybackStateEP26FMOD_STUDIO_PLAYBACK_STATE", (uintptr_t)&_ZN4FMOD6Studio13EventInstance16getPlaybackStateEP26FMOD_STUDIO_PLAYBACK_STATE },
	{ "_ZNK4FMOD6Studio16EventDescription9isOneshotEPb", (uintptr_t)&_ZN4FMOD6Studio16EventDescription9isOneshotEPb },
	{ "_ZNK4FMOD6Studio16EventDescription15getUserPropertyEPKcP25FMOD_STUDIO_USER_PROPERTY", (uintptr_t)&_ZN4FMOD6Studio16EventDescription15getUserPropertyEPKcP25FMOD_STUDIO_USER_PROPERTY },
	{ "_ZN4FMOD7Channel11setPositionEjj", (uintptr_t)&_ZN4FMOD7Channel11setPositionEjj },
	{ "_ZN4FMOD7Channel11getPositionEPjj", (uintptr_t)&_ZN4FMOD7Channel11getPositionEPjj },
	{ "_ZN4FMOD6Studio13EventInstance4stopE21FMOD_STUDIO_STOP_MODE", (uintptr_t)&_ZN4FMOD6Studio13EventInstance4stopE21FMOD_STUDIO_STOP_MODE },
	{ "_ZNK4FMOD6Studio13EventInstance13getCueByIndexEiPPNS0_11CueInstanceE", (uintptr_t)&ret0 }, //_ZN4FMOD6Studio13EventInstance13getCueByIndexEiPPNS0_11CueInstanceE },
	{ "_ZNK4FMOD6Studio13EventInstance12getParameterEPKcPPNS0_17ParameterInstanceE", (uintptr_t)&_ZN4FMOD6Studio13EventInstance12getParameterEPKcPPNS0_17ParameterInstanceE },
	{ "_ZN4FMOD6Studio17ParameterInstance8setValueEf", (uintptr_t)&_ZN4FMOD6Studio17ParameterInstance8setValueEf },
	{ "_ZN4FMOD6Studio11CueInstance7triggerEv", (uintptr_t)&ret0 }, //_ZN4FMOD6Studio11CueInstance7triggerEv },
	{ "_ZNK4FMOD6Studio13EventInstance19getTimelinePositionEPi", (uintptr_t)&_ZN4FMOD6Studio13EventInstance19getTimelinePositionEPi },
	{ "_ZNK4FMOD6Studio13EventInstance15getChannelGroupEPPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD6Studio13EventInstance15getChannelGroupEPPNS_12ChannelGroupE },
	{ "_ZN4FMOD14ChannelControl13getAudibilityEPf", (uintptr_t)&_ZN4FMOD14ChannelControl13getAudibilityEPf },
	{ "_ZNK4FMOD6Studio13EventInstance17getParameterCountEPi", (uintptr_t)&_ZN4FMOD6Studio13EventInstance17getParameterCountEPi },
	{ "_ZNK4FMOD6Studio13EventInstance19getParameterByIndexEiPPNS0_17ParameterInstanceE", (uintptr_t)&_ZN4FMOD6Studio13EventInstance19getParameterByIndexEiPPNS0_17ParameterInstanceE },
	{ "_ZNK4FMOD6Studio17ParameterInstance14getDescriptionEP33FMOD_STUDIO_PARAMETER_DESCRIPTION", (uintptr_t)&_ZN4FMOD6Studio17ParameterInstance14getDescriptionEP33FMOD_STUDIO_PARAMETER_DESCRIPTION },
	{ "_ZNK4FMOD6Studio16EventDescription7isValidEv", (uintptr_t)&_ZN4FMOD6Studio16EventDescription7isValidEv },
	{ "_ZNK4FMOD6Studio17ParameterInstance7isValidEv", (uintptr_t)&_ZN4FMOD6Studio17ParameterInstance7isValidEv },
	{ "_ZNK4FMOD6Studio13EventInstance11getUserDataEPPv", (uintptr_t)&_ZN4FMOD6Studio13EventInstance11getUserDataEPPv },
	{ "_ZNK4FMOD6Studio6System12getSoundInfoEPKcP22FMOD_STUDIO_SOUND_INFO", (uintptr_t)&_ZN4FMOD6Studio6System12getSoundInfoEPKcP22FMOD_STUDIO_SOUND_INFO },
	{ "_ZN4FMOD6Studio6System21setListenerAttributesEiPK18FMOD_3D_ATTRIBUTES", (uintptr_t)&_ZN4FMOD6Studio6System21setListenerAttributesEiPK18FMOD_3D_ATTRIBUTES },
	{ "_ZN4FMOD6System11mixerResumeEv", (uintptr_t)&_ZN4FMOD6System11mixerResumeEv },
	{ "_ZN4FMOD6System12mixerSuspendEv", (uintptr_t)&_ZN4FMOD6System12mixerSuspendEv },
	{ "_ZN4FMOD14ChannelControl9getVolumeEPf", (uintptr_t)&_ZN4FMOD14ChannelControl9getVolumeEPf },
	{ "_ZN4FMOD14ChannelControl9setVolumeEf", (uintptr_t)&_ZN4FMOD14ChannelControl9setVolumeEf },
	{ "_ZN4FMOD14ChannelControl7setMuteEb", (uintptr_t)&_ZN4FMOD14ChannelControl7setMuteEb },
	{ "_ZN4FMOD14ChannelControl9setPausedEb", (uintptr_t)&_ZN4FMOD14ChannelControl9setPausedEb },
	{ "_ZN4FMOD3DSP7releaseEv", (uintptr_t)&_ZN4FMOD3DSP7releaseEv },
	{ "_ZN4FMOD12ChannelGroup7releaseEv", (uintptr_t)&_ZN4FMOD12ChannelGroup7releaseEv },
	{ "_ZN4FMOD3DSP17setParameterFloatEif", (uintptr_t)&_ZN4FMOD3DSP17setParameterFloatEif },
	{ "_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE },
	{ "_ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE", (uintptr_t)&_ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE },
	{ "_ZN4FMOD3DSP16setChannelFormatEji16FMOD_SPEAKERMODE", (uintptr_t)&_ZN4FMOD3DSP16setChannelFormatEji16FMOD_SPEAKERMODE },
	{ "_ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE", (uintptr_t)&_ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE },
	{ "_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE", (uintptr_t)&_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE },
	{ "_ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE", (uintptr_t)&_ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE },
	{ "_ZN4FMOD14ChannelControl7setModeEj", (uintptr_t)&_ZN4FMOD14ChannelControl7setModeEj },
	{ "_ZN4FMOD14ChannelControl11setUserDataEPv", (uintptr_t)&_ZN4FMOD14ChannelControl11setUserDataEPv },
	{ "_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E", (uintptr_t)&_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E },
	{ "_ZN4FMOD14ChannelControl9isPlayingEPb", (uintptr_t)&_ZN4FMOD14ChannelControl9isPlayingEPb },
	{ "_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_", (uintptr_t)&_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_ },
	{ "_ZN4FMOD14ChannelControl11getUserDataEPPv", (uintptr_t)&_ZN4FMOD14ChannelControl11getUserDataEPPv },
	{ "_ZN4FMOD14ChannelControl19setReverbPropertiesEif", (uintptr_t)&_ZN4FMOD14ChannelControl19setReverbPropertiesEif },
	{ "_ZN4FMOD14ChannelControl6setPanEf", (uintptr_t)&_ZN4FMOD14ChannelControl6setPanEf },
	{ "_ZN4FMOD14ChannelControl8setPitchEf", (uintptr_t)&_ZN4FMOD14ChannelControl8setPitchEf },
	{ "_ZN4FMOD14ChannelControl19get3DMinMaxDistanceEPfS1_", (uintptr_t)&_ZN4FMOD14ChannelControl19get3DMinMaxDistanceEPfS1_ },
	{ "_ZN4FMOD14ChannelControl19set3DMinMaxDistanceEff", (uintptr_t)&_ZN4FMOD14ChannelControl19set3DMinMaxDistanceEff },
	{ "_ZN4FMOD7Channel12setLoopCountEi", (uintptr_t)&_ZN4FMOD7Channel12setLoopCountEi },
	{ "_ZN4FMOD14ChannelControl15getSystemObjectEPPNS_6SystemE", (uintptr_t)&_ZN4FMOD14ChannelControl15getSystemObjectEPPNS_6SystemE },
	{ "_ZN4FMOD7Channel15getChannelGroupEPPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD7Channel15getChannelGroupEPPNS_12ChannelGroupE },
	{ "_ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE", (uintptr_t)&_ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE },
	{ "_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE", (uintptr_t)&_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE },
	{ "_ZN4FMOD5Sound13getLoopPointsEPjjS1_j", (uintptr_t)&_ZN4FMOD5Sound13getLoopPointsEPjjS1_j },
	{ "_ZN4FMOD5Sound16getNumSyncPointsEPi", (uintptr_t)&_ZN4FMOD5Sound16getNumSyncPointsEPi },
	{ "_ZN4FMOD3DSP15setParameterIntEii", (uintptr_t)&_ZN4FMOD3DSP15setParameterIntEii },
	{ "FMOD_System_Create", (uintptr_t)&FMOD_System_Create },
	{ "_ZN4FMOD6System13getNumDriversEPi", (uintptr_t)&_ZN4FMOD6System13getNumDriversEPi },
	{ "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE", (uintptr_t)&_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE },
	{ "_ZN4FMOD6System4initEijPv", (uintptr_t)&_ZN4FMOD6System4initEijPv },
	{ "_ZN4FMOD6System6updateEv", (uintptr_t)&_ZN4FMOD6System6updateEv },
	{ "_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_", (uintptr_t)&_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_ },
	{ "_ZN4FMOD7Channel12setFrequencyEf", (uintptr_t)&_ZN4FMOD7Channel12setFrequencyEf },
	{ "_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_", (uintptr_t)&ret0 }, //_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_ },
	{ "_ZN4FMOD14ChannelControl14set3DOcclusionEff", (uintptr_t)&_ZN4FMOD14ChannelControl14set3DOcclusionEff },
	{ "_ZN4FMOD7Channel11setPriorityEi", (uintptr_t)&_ZN4FMOD7Channel11setPriorityEi },
	{ "_ZN4FMOD6System13set3DSettingsEfff", (uintptr_t)&_ZN4FMOD6System13set3DSettingsEfff },
	{ "_ZN4FMOD6System5closeEv", (uintptr_t)&_ZN4FMOD6System5closeEv },
	{ "_ZN4FMOD6System7releaseEv", (uintptr_t)&_ZN4FMOD6System7releaseEv },
	{ "_ZN4FMOD5Sound19set3DMinMaxDistanceEff", (uintptr_t)&_ZN4FMOD5Sound19set3DMinMaxDistanceEff },
	{ "_ZN4FMOD5Sound7setModeEj", (uintptr_t)&_ZN4FMOD5Sound7setModeEj },
	{ "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", (uintptr_t)&_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE },
	{ "_ZN4FMOD5Sound11getDefaultsEPfPi", (uintptr_t)&_ZN4FMOD5Sound11getDefaultsEPfPi },
	{ "_ZN4FMOD7Channel12getFrequencyEPf", (uintptr_t)&_ZN4FMOD7Channel12getFrequencyEPf},
	{ "_ZN4FMOD14ChannelControl12getMixMatrixEPfPiS2_i", (uintptr_t)&_ZN4FMOD14ChannelControl12getMixMatrixEPfPiS2_i},
	{ "nanosleep", (uintptr_t)&nanosleep_soloader },
	{ "div", (uintptr_t)&div },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

void *dlsym_hook( void *handle, const char *symbol) {
	sceClibPrintf("dlsym %s\n", symbol);
	for (size_t i = 0; i < numhooks; ++i) {
		if (!strcmp(symbol, default_dynlib[i].symbol)) {
			return default_dynlib[i].func;
		}
	}
	return vglGetProcAddress(symbol);
}

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
	NATIVE_GET_PERMISSION,
	NATIVE_EXPANSION_IS_FILE_NEEDED,
	NATIVE_EXPANSION_GET_STATE,
	NATIVE_IAP_IS_PURCHASED
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
	{ "NativeGetPermission", NATIVE_GET_PERMISSION },
	{ "NativeExpansionIsFileNeeded", NATIVE_EXPANSION_IS_FILE_NEEDED },
	{ "NativeExpansionIsFileNeeded", NATIVE_EXPANSION_GET_STATE },
	{ "NativeIAPIsPurchased", NATIVE_IAP_IS_PURCHASED },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	sceClibPrintf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	sceClibPrintf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case NATIVE_IAP_IS_PURCHASED:
		return enable_dlcs;
	case NATIVE_GET_PERMISSION:
		return 1;
	case NATIVE_EXPANSION_IS_FILE_NEEDED:
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case NATIVE_EXPANSION_GET_STATE:
		return 6;
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	if (!string)
		return 0;
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	sceClibPrintf("GetFieldID %s\n", name);
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx) {
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

char duration[32];
void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	int lang = -1;
	switch (methodID) {
	default:
		return 0x34343434;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0.0f;
	}
}

float CallStaticDoubleMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0.0f;
	}
}

int GetArrayLength(void *env, void *array) {
	sceClibPrintf("GetArrayLength returned %d\n", *(int *)array);
	return *(int *)array;
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

void JAVAInit(void *fake_env, void *class) {
	uint32_t addr = (uint32_t)fake_env;
	kuKernelCpuUnrestrictedMemcpy((void *)(main_mod.text_base + 0xCA3F5C), &addr, 4);
	kuKernelCpuUnrestrictedMemcpy((void *)(main_mod.text_base + 0xCA3F60), &addr, 4);
}

void OEUtilLog(const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[UTIL_LOG] %s\n", string);
#endif
	return 0;
}

void patch_game(void) {
	hook_addr((uintptr_t)so_symbol(&main_mod, "_Z8JAVAInitP7_JavaVMP7_jclass"), (uintptr_t)&JAVAInit);
	hook_addr((uintptr_t)so_symbol(&main_mod, "_Z9OEUtilLogPKcz"), (uintptr_t)&OEUtilLog);

	// libzip
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_add"), (uintptr_t)&zip_add);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_add_dir"), (uintptr_t)&zip_add_dir);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_archive_set_tempdir"), (uintptr_t)&zip_archive_set_tempdir);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_close"), (uintptr_t)&zip_close);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_delete"), (uintptr_t)&zip_delete);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_dir_add"), (uintptr_t)&zip_dir_add);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_discard"), (uintptr_t)&zip_discard);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_clear"), (uintptr_t)&zip_error_clear);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_code_system"), (uintptr_t)&zip_error_code_system);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_code_zip"), (uintptr_t)&zip_error_code_zip);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_fini"), (uintptr_t)&zip_error_fini);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_get"), (uintptr_t)&zip_error_get);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_get_sys_type"), (uintptr_t)&zip_error_get_sys_type);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_init"), (uintptr_t)&zip_error_init);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_init_with_code"), (uintptr_t)&zip_error_init_with_code);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_set"), (uintptr_t)&zip_error_set);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_strerror"), (uintptr_t)&zip_error_strerror);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_system_type"), (uintptr_t)&zip_error_system_type);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_to_data"), (uintptr_t)&zip_error_to_data);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_error_to_str"), (uintptr_t)&zip_error_to_str);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fclose"), (uintptr_t)&zip_fclose);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fdopen"), (uintptr_t)&zip_fdopen);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_add"), (uintptr_t)&zip_file_add);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_error_clear"), (uintptr_t)&zip_file_error_clear);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_error_get"), (uintptr_t)&zip_file_error_get);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_field_delete"), (uintptr_t)&zip_file_extra_field_delete);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_field_delete_by_id"), (uintptr_t)&zip_file_extra_field_delete_by_id);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_field_get"), (uintptr_t)&zip_file_extra_field_get);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_field_get_by_id"), (uintptr_t)&zip_file_extra_field_get_by_id);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_field_set"), (uintptr_t)&zip_file_extra_field_set);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_fields_count"), (uintptr_t)&zip_file_extra_fields_count);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_extra_fields_count_by_id"), (uintptr_t)&zip_file_extra_fields_count_by_id);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_get_comment"), (uintptr_t)&zip_file_get_comment);
	//ook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_get_error"), (uintptr_t)&zip_file_get_error);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_get_external_attributes"), (uintptr_t)&zip_file_get_external_attributes);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_rename"), (uintptr_t)&zip_file_rename);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_replace"), (uintptr_t)&zip_file_replace);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_set_comment"), (uintptr_t)&zip_file_set_comment);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_set_external_attributes"), (uintptr_t)&zip_file_set_external_attributes);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_set_mtime"), (uintptr_t)&zip_file_set_mtime);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_file_strerror"), (uintptr_t)&zip_file_strerror);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fopen"), (uintptr_t)&zip_fopen);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fopen_encrypted"), (uintptr_t)&zip_fopen_encrypted);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fopen_index"), (uintptr_t)&zip_fopen_index);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fopen_index_encrypted"), (uintptr_t)&zip_fopen_index_encrypted);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_fread"), (uintptr_t)&zip_fread);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_archive_comment"), (uintptr_t)&zip_get_archive_comment);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_archive_flag"), (uintptr_t)&zip_get_archive_flag);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_error"), (uintptr_t)&zip_get_error);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_file_comment"), (uintptr_t)&zip_get_file_comment);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_name"), (uintptr_t)&zip_get_name);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_num_entries"), (uintptr_t)&zip_get_num_entries);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_get_num_files"), (uintptr_t)&zip_get_num_files);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_name_locate"), (uintptr_t)&zip_name_locate);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_open"), (uintptr_t)&zip_open);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_open_from_source"), (uintptr_t)&zip_open_from_source);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_rename"), (uintptr_t)&zip_rename);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_replace"), (uintptr_t)&zip_replace);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_set_archive_comment"), (uintptr_t)&zip_set_archive_comment);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_set_archive_flag"), (uintptr_t)&zip_set_archive_flag);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_set_default_password"), (uintptr_t)&zip_set_default_password);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_set_file_comment"), (uintptr_t)&zip_set_file_comment);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_set_file_compression"), (uintptr_t)&zip_set_file_compression);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_begin_write"), (uintptr_t)&zip_source_begin_write);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_buffer"), (uintptr_t)&zip_source_buffer);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_buffer_create"), (uintptr_t)&zip_source_buffer_create);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_close"), (uintptr_t)&zip_source_close);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_commit_write"), (uintptr_t)&zip_source_commit_write);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_crc"), (uintptr_t)&zip_source_crc);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_deflate"), (uintptr_t)&zip_source_deflate);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_error"), (uintptr_t)&zip_source_error);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_file"), (uintptr_t)&zip_source_file);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_file_create"), (uintptr_t)&zip_source_file_create);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_filep"), (uintptr_t)&zip_source_filep);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_filep_create"), (uintptr_t)&zip_source_filep_create);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_free"), (uintptr_t)&zip_source_free);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_function"), (uintptr_t)&zip_source_function);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_function_create"), (uintptr_t)&zip_source_function_create);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_is_deleted"), (uintptr_t)&zip_source_is_deleted);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_keep"), (uintptr_t)&zip_source_keep);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_layered"), (uintptr_t)&zip_source_layered);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_layered_create"), (uintptr_t)&zip_source_layered_create);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_make_command_bitmap"), (uintptr_t)&zip_source_make_command_bitmap);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_open"), (uintptr_t)&zip_source_open);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_pkware"), (uintptr_t)&zip_source_pkware);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_read"), (uintptr_t)&zip_source_read);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_remove"), (uintptr_t)&zip_source_remove);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_rollback_write"), (uintptr_t)&zip_source_rollback_write);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_seek"), (uintptr_t)&zip_source_seek);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_seek_compute_offset"), (uintptr_t)&zip_source_seek_compute_offset);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_seek_write"), (uintptr_t)&zip_source_seek_write);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_stat"), (uintptr_t)&zip_source_stat);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_supports"), (uintptr_t)&zip_source_supports);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_tell"), (uintptr_t)&zip_source_tell);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_tell_write"), (uintptr_t)&zip_source_tell_write);
	//hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_window"), (uintptr_t)&zip_source_window);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_write"), (uintptr_t)&zip_source_write);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_source_zip"), (uintptr_t)&zip_source_zip);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_stat"), (uintptr_t)&zip_stat);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_stat_index"), (uintptr_t)&zip_stat_index);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_stat_init"), (uintptr_t)&zip_stat_init);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_strerror"), (uintptr_t)&zip_strerror);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_unchange"), (uintptr_t)&zip_unchange);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_unchange_all"), (uintptr_t)&zip_unchange_all);
	hook_addr((uintptr_t)so_symbol(&main_mod, "zip_unchange_archive"), (uintptr_t)&zip_unchange_archive);
}

void loadConfig(void) {
	char buffer[30];
	int value;

	FILE *config = fopen("app0:settings.cfg", "r");

	if (config) {
		while (EOF != fscanf(config, "%[^=]=%d\n", buffer, &value)) {
			if (strcmp("enable_dlcs", buffer) == 0) enable_dlcs = value;
		}
		fclose(config);
	}
}

void *real_main(void *argv) {
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);
	
	SceAppUtilAppEventParam eventParam;
	memset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
	sceAppUtilReceiveAppEvent(&eventParam);
	if (eventParam.type == 0x05) {
		sceAppMgrLoadExec("app0:safe.bin", NULL, NULL);
	}
	
	sceClibPrintf("Loading FMOD Studio...\n");
	if (!file_exists("ur0:/data/libfmodstudio.suprx"))
		fatal_error("Error libfmodstudio.suprx is not installed.");
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
		int ret = sceNetShowNetstat();
		SceNetInitParam initparam;
		if (ret == SCE_NET_ERROR_ENOTINIT) {
			initparam.memory = malloc(141 * 1024);
			initparam.size = 141 * 1024;
			initparam.flags = 0;
			sceNetInit(&initparam);
		}
	sceClibPrintf("sceKernelLoadStartModule %x\n", sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, NULL, 0, NULL, NULL));
	sceClibPrintf("sceKernelLoadStartModule %x\n", sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, NULL, 0, NULL, NULL));
	sceClibPrintf("sceKernelLoadStartModule %x\n", sceKernelLoadStartModule("ur0:data/libfmodstudio.suprx", 0, NULL, 0, NULL, NULL));
	
	sceClibPrintf("Booting...\n");
	sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);	
	
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");
	
	loadConfig();
	
	char fname[256];
	sprintf(data_path, "ux0:data/rct");
	
	unz_file_info file_info;
	unzFile apk_file = unzOpen("ux0:data/rct/game.apk");
	if (!apk_file)
		fatal_error("Error could not find ux0:data/rct/game.apk.");
	int res = unzLocateFile(apk_file, "lib/armeabi-v7a/libstlport_shared.so", NULL);
	unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
	unzOpenCurrentFile(apk_file);
	uint64_t so_size = file_info.uncompressed_size;
	uint8_t *so_buffer = (uint8_t *)malloc(so_size);
	unzReadCurrentFile(apk_file, so_buffer, so_size);
	unzCloseCurrentFile(apk_file);
	res = so_mem_load(&stl_mod, so_buffer, so_size, LOAD_ADDRESS);
	if (res < 0)
		fatal_error("Error could not load lib/armeabi-v7a/libstlport_shared.so from inside game.apk. (Errorcode: 0x%08X)", res);
	free(so_buffer);
	so_relocate(&stl_mod);
	so_resolve(&stl_mod, default_dynlib, sizeof(default_dynlib), 0);
	so_flush_caches(&stl_mod);
	so_initialize(&stl_mod);
	
	unzLocateFile(apk_file, "lib/armeabi-v7a/libCarbonAndroid.so", NULL);
	unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
	unzOpenCurrentFile(apk_file);
	so_size = file_info.uncompressed_size;
	so_buffer = (uint8_t *)malloc(so_size);
	unzReadCurrentFile(apk_file, so_buffer, so_size);
	unzCloseCurrentFile(apk_file);
	res = so_mem_load(&main_mod, so_buffer, so_size, LOAD_ADDRESS + 0x1000000);
	if (res < 0)
		fatal_error("Error could not load lib/armeabi-v7a/libCarbonAndroid.so from inside game.apk. (Errorcode: 0x%08X)", res);
	free(so_buffer);
	so_relocate(&main_mod);
	so_resolve(&main_mod, default_dynlib, sizeof(default_dynlib), 0);
	patch_game();
	so_flush_caches(&main_mod);
	so_initialize(&main_mod);
	unzClose(apk_file);
	
	//vglUseCachedMem(GL_TRUE);
	vglUseVram(GL_FALSE);
	vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
	vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)GetEnv;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x10) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x22C) = (uintptr_t)CallStaticDoubleMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;

	int (*EngineInit)(void *env, int a2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineInit");
	int (*EngineSetAPKPath)(void *env, int a2, char *path) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineSetAPKPath");
	int (*EngineSetExpansionFilePaths)(void *env, int a2, char *path, char *path2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineSetExpansionFilePaths");
	int (*EngineSetSaveDataPath)(void *env, int a2, char *path, char *path2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineSetSaveDataPath");
	int (*EngineRendererDrawFrame)(void *env, int a2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineRendererDrawFrame");
	int (*EngineTouchAdd)(void *env, int a2, int id, float x, float y) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineTouchAdd");
	int (*EngineTouchMove)(void *env, int a2, int id, float x, float y) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineTouchMove");
	int (*EngineTouchRemove)(void *env, int a2, int id, float x, float y) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineTouchRemove");
	int (*AppCreateEngine)(void *env, int a2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_AppCreateEngine");
	int (*AppCreateApplication)(void *env, int a2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_AppCreateApplication");
	int (*EngineSetAppInfo)(void *env, int a2, char *name, char *dev_id, char *model, char *sdk_ver, char *lang, char *country, int store) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineSetAppInfo");
	int (*EngineSetVersionString)(void *env, int a2, char *path) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineSetVersionString");
	int (*EngineRendererSurfaceCreated)(void *env, int a2) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineRendererSurfaceCreated");
	int (*EngineRendererSurfaceChanged)(void *env, int a2, int w, int h) = (void *) so_symbol(&main_mod, "Java_com_Origin8_OEAndroid_JNI_EngineRendererSurfaceChanged");
	
	sceClibPrintf("AppCreateEngine\n");
	AppCreateEngine(&fake_env, 0);
	
	sceClibPrintf("EngineInit\n");
	EngineInit(&fake_env, 0);
	
	sceClibPrintf("EngineSetAPKPath\n");
	EngineSetAPKPath(&fake_env, 0, "ux0:data/rct/game.apk");
	
	sceClibPrintf("EngineSetVersionString\n");
	EngineSetVersionString(&fake_env, 0, "1.2.1");
	
	sceClibPrintf("EngineSetSaveDataPath\n");
	EngineSetSaveDataPath(&fake_env, 0, "ux0:data/rct", "ux0:data/rct");
	
	sceClibPrintf("EngineSetExpansionFilePaths\n");
	EngineSetExpansionFilePaths(&fake_env, 0, "ux0:data/rct/main.obb", "ux0:data/rct/patch.obb");
	
	sceClibPrintf("EngineSetAppInfo\n");
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &res);
	switch (res) {
	case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "en", "GB", 0);
		break;
	case SCE_SYSTEM_PARAM_LANG_SPANISH:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "es", "", 0);
		break;
	case SCE_SYSTEM_PARAM_LANG_FRENCH:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "fr", "", 0);
		break;
	case SCE_SYSTEM_PARAM_LANG_ITALIAN:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "it", "", 0);
		break;
	case SCE_SYSTEM_PARAM_LANG_GERMAN:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "de", "", 0);
		break;
	case SCE_SYSTEM_PARAM_LANG_DUTCH:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "nl", "", 0);
		break;
	default:
		EngineSetAppInfo(&fake_env, 0, "rct", "PSVita", "PSVita", "19", "en", "US", 0);
		break;
	}
	
	sceClibPrintf("EngineRendererSurfaceCreated\n");
	EngineRendererSurfaceCreated(&fake_env, 0);
	
	sceClibPrintf("EngineRendererSurfaceChanged\n");
	EngineRendererSurfaceChanged(&fake_env, 0, SCREEN_W, SCREEN_H);
	
	sceClibPrintf("AppCreateApplication\n");
	AppCreateApplication(&fake_env, 0);
	
	int lastX[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
	int lastY[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};

	sceClibPrintf("Entering main loop\n");
	uint32_t oldpad = 0;
	for (;;) {
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		
		for (int i = 0; i < SCE_TOUCH_MAX_REPORT; i++) {
			if (i < touch.reportNum) {
				float x = (float)touch.report[i].x * (float)960.0f / 1920.0f;
				float y = (float)touch.report[i].y * (float)544.0f / 1088.0f;
				int id = i;

				if (lastX[i] == -1 || lastY[i] == -1) {
					EngineTouchAdd(&fake_env, NULL, i, x, y);
				} else {
					EngineTouchMove(&fake_env, NULL, i, x, y);
				}

				lastX[i] = x;
				lastY[i] = y;

			} else {
				if (lastX[i] != -1 || lastY[i] != -1) {
					EngineTouchRemove(&fake_env, NULL, i, lastX[i], lastY[i]);
					lastX[i] = -1;
					lastY[i] = -1;
				}
			}
		}
		
		#define fakeInput(btn, x, y, id) \
			if ((pad.buttons & btn) == btn) { \
				if ((oldpad & btn) == btn) { \
					EngineTouchMove(&fake_env, 0, id, x, y); \
				} else { \
					EngineTouchAdd(&fake_env, 0, id, x, y); \
				} \
			} else if ((oldpad & btn) == btn) { \
				EngineTouchRemove(&fake_env, 0, id, x, y); \
			}
		
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		fakeInput(SCE_CTRL_LTRIGGER, 885.0f, 31.0f, 5) // Rotate Left
		fakeInput(SCE_CTRL_RTRIGGER, 928.0f, 76.0f, 6) // Rotate Right
		fakeInput(SCE_CTRL_START, 932.0f, 518.0f, 7) // Pause
		oldpad = pad.buttons;
		
		EngineRendererDrawFrame(&fake_env, 0);
		vglSwapBuffers(GL_FALSE);
	}
}

int main(int argc, char *argv[]) {
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 0x400000);
	pthread_create(&t, &attr, real_main, NULL);
	
	return sceKernelExitDeleteThread(0);
}

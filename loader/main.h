#ifndef __MAIN_H__
#define __MAIN_H__

#include <stdint.h>
#include <psp2/touch.h>
#include "config.h"
#include "so_util.h"

int debugPrintf(char *text, ...);

int ret0();
uint64_t current_timestamp_ms(void);

int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int cpuAffinityMask);

SceUID _vshKernelSearchModuleByName(const char *, const void *);

extern SceTouchPanelInfo panelInfoFront, panelInfoBack;

#endif

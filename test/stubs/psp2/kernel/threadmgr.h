#ifndef STUB_THREADMGR_H
#define STUB_THREADMGR_H
#include <psp2/types.h>
SceUID sceKernelCreateThread(const char*, int(*)(SceSize,void*),int,int,int,int,void*);
int sceKernelStartThread(SceUID,SceSize,void*);
int sceKernelWaitThreadEnd(SceUID,int*,SceUInt32*);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(SceUInt32);
SceUInt64 sceKernelGetProcessTimeWide(void);
#endif

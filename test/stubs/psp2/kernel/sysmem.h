#ifndef STUB_SYSMEM_H
#define STUB_SYSMEM_H
#include <psp2/types.h>
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0D808060
SceUID sceKernelAllocMemBlock(const char*,int,SceSize,void*);
int sceKernelGetMemBlockBase(SceUID,void**);
int sceKernelFreeMemBlock(SceUID);
#endif

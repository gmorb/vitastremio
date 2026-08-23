#ifndef STUB_FCNTL_H
#define STUB_FCNTL_H
#include <psp2/types.h>
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDONLY 0x0001
#define SCE_O_CREAT  0x0200
#define SCE_O_APPEND 0x0100
#define SCE_O_TRUNC  0x0400
SceUID sceIoOpen(const char*,int,int);
int sceIoWrite(SceUID,const void*,SceSize);
int sceIoRead(SceUID,void*,SceSize);
int sceIoClose(SceUID);
int sceIoRemove(const char*);
#endif

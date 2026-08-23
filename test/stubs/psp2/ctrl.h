#ifndef STUB_CTRL_H
#define STUB_CTRL_H
#define SCE_CTRL_MODE_ANALOG 1
#define SCE_CTRL_UP       0x0010
#define SCE_CTRL_RIGHT    0x0020
#define SCE_CTRL_DOWN     0x0040
#define SCE_CTRL_LEFT     0x0080
#define SCE_CTRL_LTRIGGER 0x0100
#define SCE_CTRL_RTRIGGER 0x0200
#define SCE_CTRL_TRIANGLE 0x1000
#define SCE_CTRL_CIRCLE   0x2000
#define SCE_CTRL_CROSS    0x4000
#define SCE_CTRL_SQUARE   0x8000
#define SCE_CTRL_SELECT   0x0001
#define SCE_CTRL_START    0x0008
typedef struct { unsigned int timeStamp; unsigned int buttons;
                 unsigned char lx,ly,rx,ry; unsigned char rsrv[16]; } SceCtrlData;
int sceCtrlSetSamplingMode(int);
int sceCtrlPeekBufferPositive(int,SceCtrlData*,int);
#endif

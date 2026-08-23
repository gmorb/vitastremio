#ifndef STUB_VIDEODEC_H
#define STUB_VIDEODEC_H
#include <psp2/types.h>
#define SCE_VIDEODEC_TYPE_HW_AVCDEC 0x1001
typedef struct { SceUInt32 size, horizontal, vertical, numOfRefFrames, numOfStreams; }
        SceVideodecQueryInitInfoHwAvcdec;
typedef union { SceUInt8 reserved[32]; SceVideodecQueryInitInfoHwAvcdec hwAvc; }
        SceVideodecQueryInitInfo;
typedef struct { SceUInt32 horizontal, vertical, numOfRefFrames; } SceAvcdecQueryDecoderInfo;
typedef struct { SceUInt32 frameMemSize; } SceAvcdecDecoderInfo;
typedef struct { void *pBuf; SceUInt32 size; } SceAvcdecBuf;
typedef struct { SceUInt32 handle; SceAvcdecBuf frameBuf; } SceAvcdecCtrl;
typedef struct { SceUInt32 lower, upper; } SceAvcdecTime;
typedef struct { void *pBuf; SceUInt32 size; } SceAvcdecEs;
typedef struct { SceAvcdecTime pts, dts; SceAvcdecEs es; } SceAvcdecAu;
typedef struct { SceUInt32 pixelType, framePitch, frameWidth, frameHeight;
                 SceUInt32 horizontalSize, verticalSize; void *pPicture[2]; }
        SceAvcdecFrame;
typedef struct { SceUInt32 size; SceAvcdecFrame frame; } SceAvcdecPicture;
typedef struct { SceUInt32 numOfElm; SceAvcdecPicture **pPicture; SceUInt32 numOfOutput; }
        SceAvcdecArrayPicture;
int sceVideodecInitLibrary(SceUInt32, const SceVideodecQueryInitInfoHwAvcdec*);
int sceVideodecTermLibrary(SceUInt32);
int sceAvcdecQueryDecoderMemSize(SceUInt32, const SceAvcdecQueryDecoderInfo*, SceAvcdecDecoderInfo*);
int sceAvcdecCreateDecoder(SceUInt32, SceAvcdecCtrl*, const SceAvcdecQueryDecoderInfo*);
int sceAvcdecDeleteDecoder(SceAvcdecCtrl*);
int sceAvcdecDecode(SceAvcdecCtrl*, const SceAvcdecAu*, SceAvcdecArrayPicture*);
#endif

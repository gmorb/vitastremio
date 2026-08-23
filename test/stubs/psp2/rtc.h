#ifndef STUB_RTC_H
#define STUB_RTC_H
#include <psp2/types.h>
typedef struct { SceUInt64 tick; } SceRtcTick;
int sceRtcGetCurrentTick(SceRtcTick*);
#endif

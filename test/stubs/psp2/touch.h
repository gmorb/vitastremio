#ifndef STUB_TOUCH_H
#define STUB_TOUCH_H
#include <psp2/types.h>
#define SCE_TOUCH_PORT_FRONT 0
#define SCE_TOUCH_SAMPLING_STATE_START 1
typedef struct { SceUInt8 id,force; SceUInt16 x,y; SceUInt8 rsrv[8];
                 SceUInt16 info; } SceTouchReport;
typedef struct { SceUInt64 timeStamp; SceUInt32 status, reportNum;
                 SceTouchReport report[8]; } SceTouchData;
int sceTouchSetSamplingState(SceUInt32,SceUInt32);
int sceTouchPeek(SceUInt32,SceTouchData*,SceUInt32);
int sceTouchEnableTouchForce(SceUInt32);
#endif

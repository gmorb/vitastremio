#ifndef STUB_COMMON_DIALOG_H
#define STUB_COMMON_DIALOG_H
#define SCE_COMMON_DIALOG_STATUS_FINISHED 2
typedef struct { int language, enterButtonAssign; int reserved[32]; } SceCommonDialogConfigParam;
void sceCommonDialogConfigParamInit(SceCommonDialogConfigParam*);
int sceCommonDialogSetConfigParam(const SceCommonDialogConfigParam*);
#endif

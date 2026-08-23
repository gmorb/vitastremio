#ifndef STUB_IME_DIALOG_H
#define STUB_IME_DIALOG_H
#include <psp2/types.h>
#include <psp2/common_dialog.h>
#define SCE_IME_DIALOG_MAX_TEXT_LENGTH 512
#define SCE_IME_LANGUAGE_ENGLISH 0x10000
#define SCE_IME_TYPE_NUMBER 1
#define SCE_IME_TYPE_BASIC_LATIN 3
#define SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT 0
#define SCE_IME_DIALOG_BUTTON_ENTER 1
typedef struct { SceUInt32 sdkVersion, inputMethod; SceUInt64 supportedLanguages;
  SceBool languagesForced; SceUInt32 type, option; void *filter;
  SceUInt32 dialogMode, textBoxMode; const SceWChar16 *title;
  SceUInt32 maxTextLength; SceWChar16 *initialText; SceWChar16 *inputTextBuffer;
  SceUInt8 reserved[32]; } SceImeDialogParam;
typedef struct { SceUInt8 reserved[32]; int result; int button; } SceImeDialogResult;
void sceImeDialogParamInit(SceImeDialogParam*);
int sceImeDialogInit(const SceImeDialogParam*);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(SceImeDialogResult*);
int sceImeDialogTerm(void);
#endif

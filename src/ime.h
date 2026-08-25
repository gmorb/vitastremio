/* ime.h -- on-screen keyboard (SceImeDialog) wrapper.
 *
 * The system IME is a "common dialog": it renders over the app, and the app
 * must keep drawing and must call vita2d_common_dialog_update() every frame
 * while it is up, or the keyboard appears frozen. So this is a state machine
 * polled from the render loop, not a blocking call.
 *
 * Usage:
 *     vs_ime_open("Server address", "192.168.1.10:8480", 0);
 *     ...each frame:
 *     int r = vs_ime_poll(buf, sizeof(buf));
 *     if (r == 1)  { use buf }
 *     if (r == -1) { user cancelled }
 *
 * Text is converted to and from UTF-16. Conversion is ASCII-only in both
 * directions: anything above U+007F becomes '?'. That is fine for IP
 * addresses, and lossy for search terms in non-Latin scripts -- which is
 * consistent with the rest of the app, since the PGF font we render with
 * can't display them anyway.
 */
#ifndef VS_IME_H
#define VS_IME_H

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <string.h>

#define VS_IME_MAX 128

static uint16_t g_ime_title[64];
static uint16_t g_ime_initial[VS_IME_MAX + 1];
/* The IME writes here directly; must outlive the dialog and stay aligned. */
static uint16_t g_ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1]
    __attribute__((aligned(4)));
static int g_ime_active;

static void vs_utf16(const char *in, uint16_t *out, int out_max)
{
    int i = 0;
    for (; in[i] && i < out_max - 1; i++)
        out[i] = (unsigned char)in[i] < 0x80 ? (uint16_t)in[i] : (uint16_t)'?';
    out[i] = 0;
}

static void vs_utf8(const uint16_t *in, char *out, int out_max)
{
    int i = 0;
    for (; in[i] && i < out_max - 1; i++)
        out[i] = in[i] < 0x80 ? (char)in[i] : '?';
    out[i] = 0;
}

/* numeric: restrict the keyboard to digits and punctuation, for addresses. */
static int vs_ime_open(const char *title, const char *initial, int numeric)
{
    SceImeDialogParam p;

    if (g_ime_active) return -1;

    vs_utf16(title, g_ime_title, 64);
    vs_utf16(initial ? initial : "", g_ime_initial, VS_IME_MAX + 1);
    memset(g_ime_buf, 0, sizeof(g_ime_buf));

    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced    = SCE_TRUE;
    p.type               = numeric ? SCE_IME_TYPE_NUMBER
                                   : SCE_IME_TYPE_BASIC_LATIN;
    p.option             = 0;
    p.textBoxMode        = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title              = g_ime_title;
    p.maxTextLength      = VS_IME_MAX;
    p.initialText        = g_ime_initial;
    p.inputTextBuffer    = g_ime_buf;

    if (sceImeDialogInit(&p) < 0) return -1;
    g_ime_active = 1;
    return 0;
}

static int vs_ime_is_active(void) { return g_ime_active; }

/* Returns 0 while running, 1 on accept (out filled), -1 on cancel. */
static int vs_ime_poll(char *out, int out_max)
{
    SceImeDialogResult res;

    if (!g_ime_active) return 0;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;

    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    g_ime_active = 0;

    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return -1;
    vs_utf8(g_ime_buf, out, out_max);
    return 1;
}

#endif /* VS_IME_H */

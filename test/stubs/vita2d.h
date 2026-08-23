#ifndef STUB_VITA2D_H
#define STUB_VITA2D_H
#include <psp2/types.h>
#define SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 0x00000000
#define RGBA8(r,g,b,a) ((unsigned)((r)|((g)<<8)|((b)<<16)|((a)<<24)))
typedef struct vita2d_texture vita2d_texture;
typedef struct vita2d_pgf vita2d_pgf;
int  vita2d_init(void);
int  vita2d_init_advanced(unsigned int);
int  vita2d_fini(void);
void vita2d_set_clear_color(unsigned int);
void vita2d_set_vblank_wait(int);
void vita2d_start_drawing(void);
void vita2d_end_drawing(void);
void vita2d_clear_screen(void);
void vita2d_swap_buffers(void);
void vita2d_wait_rendering_done(void);
void vita2d_common_dialog_update(void);
void vita2d_draw_rectangle(float,float,float,float,unsigned int);
void vita2d_draw_fill_circle(float,float,float,unsigned int);
void vita2d_draw_line(float,float,float,float,unsigned int);
void vita2d_draw_texture_scale(const vita2d_texture*,float,float,float,float);
int  vita2d_pgf_text_width(vita2d_pgf*,float,const char*);
void vita2d_draw_texture(const vita2d_texture*,float,float);
vita2d_texture *vita2d_create_empty_texture_format(unsigned,unsigned,int);
vita2d_texture *vita2d_load_JPEG_buffer(const void*,unsigned long);
void vita2d_free_texture(vita2d_texture*);
void *vita2d_texture_get_datap(const vita2d_texture*);
vita2d_pgf *vita2d_load_default_pgf(void);
void vita2d_free_pgf(vita2d_pgf*);
int vita2d_pgf_draw_text(vita2d_pgf*,int,int,unsigned int,float,const char*);
#endif

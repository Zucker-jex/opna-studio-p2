#ifndef STUDIO_UI_H
#define STUDIO_UI_H

#include <stdint.h>

enum { PG_SEQ = 0, PG_SYN, PG_MIX, PG_VIS, PG_SYS, PG_N };

void ui_init(void);
void ui_splash(void);
void ui_input(int buttons, uint32_t now_ms);
void ui_touch(int x, int y);
void ui_draw(void);
void ui_anim(void);
void ui_draw_dirty(void);
int ui_dirty_pending(void);
void ui_redraw_pattern(void);
uint32_t ui_generation(void);
void ui_set_page(int page);
int  ui_page(void);
void ui_sim_demo(uint32_t ms);

#endif

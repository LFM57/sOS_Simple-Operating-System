#ifndef LIBGUI_H
#define LIBGUI_H

#include "sos.h"

typedef struct {
    unsigned int* pixels;
    int width;
    int height;
} lg_surface_t;

typedef struct {
    int x, y, w, h;
    unsigned int bg;
    unsigned int border;
    const char* title;
    unsigned int title_fg;
} lg_panel_t;

typedef struct {
    int x, y;
    const char* text;
    unsigned int fg;
} lg_label_t;

typedef struct {
    int x, y, w, h;
    const char* text;
    unsigned int bg;
    unsigned int hover_bg;
    unsigned int press_bg;
    unsigned int fg;
    unsigned int border;
    int hovered;
    int pressed;
} lg_button_t;

typedef struct {
    int x, y, size;
    const char* label;
    int checked;
    unsigned int fg;
    unsigned int box_bg;
    unsigned int border;
    unsigned int check_col;
} lg_checkbox_t;

typedef struct {
    int x, y, w, h;
    char* buf;
    int buf_cap;
    int len;
    int cursor;
    int focused;
    const char* placeholder;
    unsigned int bg;
    unsigned int fg;
    unsigned int border;
    unsigned int focus_border;
} lg_text_input_t;

typedef struct {
    int x, y, w, h;
    int total;
    int visible;
    int offset;
    int vertical;
    int dragging;
    int drag_offset;
    unsigned int track_col;
    unsigned int thumb_col;
    unsigned int border;
} lg_scrollbar_t;

/* Geometry & drawing primitives */
int lg_point_in_rect(int px, int py, int rx, int ry, int rw, int rh);
void lg_put_pixel(lg_surface_t* s, int x, int y, unsigned int col);
void lg_fill_rect(lg_surface_t* s, int x, int y, int w, int h, unsigned int col);
void lg_draw_rect(lg_surface_t* s, int x, int y, int w, int h, unsigned int col);
void lg_draw_line(lg_surface_t* s, int x0, int y0, int x1, int y1, unsigned int col);

/* 8x16 bitmap text rendering */
void lg_draw_char(lg_surface_t* s, char c, int x, int y, unsigned int fg, unsigned int bg, int transparent);
void lg_draw_text(lg_surface_t* s, const char* str, int x, int y, unsigned int fg);
void lg_draw_text_bg(lg_surface_t* s, const char* str, int x, int y, unsigned int fg, unsigned int bg);
int lg_measure_text(const char* str);

/* Widgets */
void lg_panel_draw(lg_surface_t* s, const lg_panel_t* p);
void lg_label_draw(lg_surface_t* s, const lg_label_t* l);

void lg_button_init(lg_button_t* btn, int x, int y, int w, int h, const char* text);
void lg_button_draw(lg_surface_t* s, const lg_button_t* btn);
int lg_button_handle_event(lg_button_t* btn, const gui_event_t* ev, int* out_dirty);

void lg_checkbox_init(lg_checkbox_t* cb, int x, int y, const char* label, int checked);
void lg_checkbox_draw(lg_surface_t* s, const lg_checkbox_t* cb);
int lg_checkbox_handle_event(lg_checkbox_t* cb, const gui_event_t* ev, int* out_dirty);

void lg_text_input_init(lg_text_input_t* ti, int x, int y, int w, int h, char* buf, int buf_cap, const char* placeholder);
void lg_text_input_draw(lg_surface_t* s, const lg_text_input_t* ti);
int lg_text_input_handle_event(lg_text_input_t* ti, const gui_event_t* ev, int* out_dirty);

void lg_scrollbar_init(lg_scrollbar_t* sb, int x, int y, int w, int h, int vertical);
void lg_scrollbar_draw(lg_surface_t* s, const lg_scrollbar_t* sb);
int lg_scrollbar_handle_event(lg_scrollbar_t* sb, const gui_event_t* ev, int* out_dirty);

int lg_message_box(const char* title, const char* message);

#endif

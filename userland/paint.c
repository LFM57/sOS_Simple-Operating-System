#include "libgui.h"

#define INIT_WIN_W 400
#define INIT_WIN_H 260
#define TOOLBAR_H  34

static const unsigned int palette[6] = {
    0x00EF4444, /* Red */
    0x0022C55E, /* Green */
    0x003B82F6, /* Blue */
    0x00F59E0B, /* Amber */
    0x000F172A, /* Ink */
    0x00FFFFFF  /* Eraser */
};

static const char* palette_names[6] = {
    "Red", "Green", "Blue", "Amber", "Ink", "Erase"
};

static void flush_rect(int win, const lg_surface_t* surf, int mapped, int x, int y, int w, int h) {
    if (mapped) {
        win_invalidate(win, x, y, w, h);
    } else {
        win_present_rect(win, surf->pixels, x, y, w, h);
    }
}

static void layout_toolbar_buttons(int win_w, lg_button_t* help_btn, lg_button_t* clear_btn) {
    clear_btn->x = win_w - 68;
    clear_btn->y = 6;
    clear_btn->w = 60;
    clear_btn->h = 22;

    help_btn->x = win_w - 100;
    help_btn->y = 6;
    help_btn->w = 26;
    help_btn->h = 22;
}

static void draw_toolbar(lg_surface_t* surf, int active_idx, const lg_button_t* help_btn, const lg_button_t* clear_btn) {
    lg_panel_t bar = { 0, 0, surf->width, TOOLBAR_H, 0x001E293B, 0x00334155, (void*)0, 0x00F8FAFC };
    lg_panel_draw(surf, &bar);

    for (int i = 0; i < 6; i++) {
        int sx = 8 + i * 28;
        int sy = 7;
        lg_fill_rect(surf, sx, sy, 22, 20, palette[i]);
        lg_draw_rect(surf, sx, sy, 22, 20, (i == active_idx) ? 0x00FFFFFF : 0x0064748B);
        if (i == active_idx) {
            lg_draw_rect(surf, sx - 1, sy - 1, 24, 22, 0x0038BDF8);
        }
    }

    if (surf->width >= 340) {
        lg_label_t tool_lbl = { 184, 9, palette_names[active_idx], 0x00E2E8F0 };
        lg_label_draw(surf, &tool_lbl);
    }

    if (surf->width >= 285) {
        lg_button_draw(surf, help_btn);
    }
    lg_button_draw(surf, clear_btn);
}

static void clear_canvas(lg_surface_t* surf, int active_idx, const lg_button_t* help_btn, const lg_button_t* clear_btn) {
    lg_fill_rect(surf, 0, TOOLBAR_H, surf->width, surf->height - TOOLBAR_H, 0x00FFFFFF);
    draw_toolbar(surf, active_idx, help_btn, clear_btn);
}

static void draw_brush_stroke(lg_surface_t* surf, int x0, int y0, int x1, int y1, unsigned int col,
                              int* out_dx, int* out_dy, int* out_dw, int* out_dh) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;
    int steps = (adx > ady) ? adx : ady;
    if (steps == 0) steps = 1;

    int min_x = (x0 < x1 ? x0 : x1) - 2;
    int max_x = (x0 > x1 ? x0 : x1) + 2;
    int min_y = (y0 < y1 ? y0 : y1) - 2;
    int max_y = (y0 > y1 ? y0 : y1) + 2;
    if (min_y < TOOLBAR_H) min_y = TOOLBAR_H;

    for (int i = 0; i <= steps; i++) {
        int px = x0 + (dx * i) / steps;
        int py = y0 + (dy * i) / steps;
        if (py >= TOOLBAR_H + 2) {
            lg_fill_rect(surf, px - 2, py - 2, 5, 5, col);
        }
    }

    if (min_x < 0) min_x = 0;
    if (max_x >= surf->width) max_x = surf->width - 1;
    if (max_y >= surf->height) max_y = surf->height - 1;
    *out_dx = min_x;
    *out_dy = min_y;
    *out_dw = (max_x >= min_x) ? (max_x - min_x + 1) : 0;
    *out_dh = (max_y >= min_y) ? (max_y - min_y + 1) : 0;
}

static unsigned int* resize_fallback_buf(unsigned int* buf, int old_w, int old_h, int new_w, int new_h) {
    unsigned int needed = (unsigned int)new_w * (unsigned int)new_h;
    unsigned int* next = (unsigned int*)malloc(needed * sizeof(unsigned int));
    if (!next) return buf;
    for (unsigned int i = 0; i < needed; i++) next[i] = 0x00FFFFFF;

    int copy_w = (old_w < new_w) ? old_w : new_w;
    int copy_h = (old_h < new_h) ? old_h : new_h;
    for (int row = TOOLBAR_H; row < copy_h; row++) {
        for (int col = 0; col < copy_w; col++) {
            next[row * new_w + col] = buf[row * old_w + col];
        }
    }
    free(buf);
    return next;
}

void _start() {
    int win = win_create("sOS Paint", INIT_WIN_W, INIT_WIN_H);
    if (win < 0) {
        print("paint: Failed to create GUI window (is GUI running?).\n");
        exit(1);
    }

    int mapped = 0;
    unsigned int* pixels = win_map(win);
    if (pixels) {
        mapped = 1;
    } else {
        pixels = (unsigned int*)malloc((unsigned int)INIT_WIN_W * (unsigned int)INIT_WIN_H * sizeof(unsigned int));
        if (!pixels) {
            win_destroy(win);
            exit(1);
        }
    }

    lg_surface_t surf = { pixels, INIT_WIN_W, INIT_WIN_H };

    lg_button_t help_btn;
    lg_button_t clear_btn;
    lg_button_init(&help_btn, 0, 0, 26, 22, "?");
    lg_button_init(&clear_btn, 0, 0, 60, 22, "Clear");
    layout_toolbar_buttons(surf.width, &help_btn, &clear_btn);

    int active_idx = 2; /* Start with Blue */
    int is_drawing = 0;
    int last_x = 0;
    int last_y = 0;

    clear_canvas(&surf, active_idx, &help_btn, &clear_btn);
    flush_rect(win, &surf, mapped, 0, 0, surf.width, surf.height);

    gui_event_t ev;
    while (win_get_event(win, &ev) > 0) {
        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        } else if (ev.type == GUI_EVENT_WINDOW_RESIZE) {
            if (ev.width > 0 && ev.height > 0 && (ev.width != surf.width || ev.height != surf.height)) {
                int old_w = surf.width;
                int old_h = surf.height;
                if (mapped) {
                    unsigned int* remapped = win_map(win);
                    if (!remapped) continue;
                    surf.pixels = remapped;
                } else {
                    unsigned int* next_buf = resize_fallback_buf(surf.pixels, old_w, old_h, ev.width, ev.height);
                    if (next_buf == surf.pixels && (unsigned int)ev.width * (unsigned int)ev.height > (unsigned int)old_w * (unsigned int)old_h) {
                        continue;
                    }
                    surf.pixels = next_buf;
                }
                surf.width = ev.width;
                surf.height = ev.height;
                if (mapped) {
                    if (ev.width > old_w) {
                        lg_fill_rect(&surf, old_w, TOOLBAR_H, ev.width - old_w, ev.height - TOOLBAR_H, 0x00FFFFFF);
                    }
                    if (ev.height > old_h) {
                        lg_fill_rect(&surf, 0, old_h, ev.width, ev.height - old_h, 0x00FFFFFF);
                    }
                }
                layout_toolbar_buttons(surf.width, &help_btn, &clear_btn);
                draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
                flush_rect(win, &surf, mapped, 0, 0, surf.width, surf.height);
            }
            continue;
        } else if (ev.type == GUI_EVENT_KEY_PRESS) {
            if (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
                if (ev.key == 'q' || ev.key == 'Q' || ev.key == GUI_KEY_ESC) {
                    break;
                } else if (ev.key == 'c' || ev.key == 'C') {
                    clear_canvas(&surf, active_idx, &help_btn, &clear_btn);
                    flush_rect(win, &surf, mapped, 0, 0, surf.width, surf.height);
                } else if (ev.key == 'h' || ev.key == 'H') {
                    lg_message_box("sOS Paint Help", "LMB: Draw | 1-6/Wheel: Color | C: Clear | Q: Quit");
                } else if (ev.key >= '1' && ev.key <= '6') {
                    active_idx = ev.key - '1';
                    draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
                    flush_rect(win, &surf, mapped, 0, 0, surf.width, TOOLBAR_H);
                }
            }
            continue;
        }

        int btn_dirty = 0;
        if (lg_button_handle_event(&clear_btn, &ev, &btn_dirty)) {
            clear_canvas(&surf, active_idx, &help_btn, &clear_btn);
            flush_rect(win, &surf, mapped, 0, 0, surf.width, surf.height);
            continue;
        }
        if (surf.width >= 285 && lg_button_handle_event(&help_btn, &ev, &btn_dirty)) {
            draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
            flush_rect(win, &surf, mapped, 0, 0, surf.width, TOOLBAR_H);
            lg_message_box("sOS Paint Help", "LMB: Draw | 1-6/Wheel: Color | C: Clear | Q: Quit");
            continue;
        }
        if (btn_dirty) {
            draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
            flush_rect(win, &surf, mapped, 0, 0, surf.width, TOOLBAR_H);
        }

        if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == 1) {
            if (ev.y < TOOLBAR_H) {
                for (int i = 0; i < 6; i++) {
                    int sx = 8 + i * 28;
                    if (lg_point_in_rect(ev.x, ev.y, sx, 7, 22, 20)) {
                        active_idx = i;
                        draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
                        flush_rect(win, &surf, mapped, 0, 0, surf.width, TOOLBAR_H);
                        break;
                    }
                }
            } else {
                int dx, dy, dw, dh;
                is_drawing = 1;
                last_x = ev.x;
                last_y = ev.y;
                draw_brush_stroke(&surf, ev.x, ev.y, ev.x, ev.y, palette[active_idx], &dx, &dy, &dw, &dh);
                if (dw > 0 && dh > 0) flush_rect(win, &surf, mapped, dx, dy, dw, dh);
            }
        } else if (ev.type == GUI_EVENT_MOUSE_MOVE && is_drawing && (ev.button & 0x01)) {
            if (ev.y >= TOOLBAR_H) {
                int dx, dy, dw, dh;
                draw_brush_stroke(&surf, last_x, last_y, ev.x, ev.y, palette[active_idx], &dx, &dy, &dw, &dh);
                last_x = ev.x;
                last_y = ev.y;
                if (dw > 0 && dh > 0) flush_rect(win, &surf, mapped, dx, dy, dw, dh);
            }
        } else if (ev.type == GUI_EVENT_MOUSE_UP && ev.button == 1) {
            is_drawing = 0;
        } else if (ev.type == GUI_EVENT_MOUSE_SCROLL && ev.dz != 0) {
            int delta = (ev.dz > 0) ? 1 : -1;
            active_idx = (active_idx + delta + 6) % 6;
            draw_toolbar(&surf, active_idx, &help_btn, &clear_btn);
            flush_rect(win, &surf, mapped, 0, 0, surf.width, TOOLBAR_H);
        }
    }

    if (!mapped && surf.pixels) {
        free(surf.pixels);
    }
    win_destroy(win);
    exit(0);
}
